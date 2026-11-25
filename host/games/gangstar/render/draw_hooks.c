#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "game.h"
#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

int g_ssr_want, g_ssr = 1;
int g_vr_want, g_vr = 1;
int g_ds_want, g_ds = 1;
int g_a2c_want, g_alpha_on, g_game_a2c;

static int fog_track, sf_want, sf_level;
static GLubyte sf_pat[64][128];

#ifndef GL_POLYGON_STIPPLE
#define GL_POLYGON_STIPPLE 0x0B42
#endif
static int sf_stipple_begin(void)
{
    static const unsigned char bayer[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
    };
    static int built, cur = -1;
    int lv = gmv_spawn_fade_level(g_blend_on && g_blend_dst == GL_ONE);
    if (lv >= 64)
        return 0;
    if (lv < 0)
        lv = 0;
    if (!built) {
        for (int l = 0; l < 64; l++)
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 32; x++)
                    if (bayer[y & 7][x & 7] < l)
                        sf_pat[l][y * 4 + x / 8] |= (GLubyte)(0x80 >> (x & 7));
        built = 1;
    }
    if (lv != cur) {
        glPolygonStipple(sf_pat[lv]);
        cur = lv;
    }
    glEnable(GL_POLYGON_STIPPLE);
    sf_level = lv + 1;
    return 1;
}

static void sf_stipple_end(int on)
{
    sf_level = 0;
    if (on)
        glDisable(GL_POLYGON_STIPPLE);
}

#define MK_BYTES (8u << 20)
static uint8_t mk_data[MK_BYTES];
static size_t mk_used, mk_sent;
static unsigned mk_frame;
static GLuint mk_buf;
static int mk_stip;
static struct {
    void(APIENTRY *BindBuffer)(GLenum, GLuint);
    void(APIENTRY *GenBuffers)(GLsizei, GLuint *);
    void(APIENTRY *BufferData)(GLenum, intptr_t, const void *, GLenum);
    void(APIENTRY *ClientActiveTexture)(GLenum);
    void(APIENTRY *DrawElements)(GLenum, GLsizei, GLenum, const void *);
} mk;

static size_t mk_put(const void *p, size_t n)
{
    size_t o = (mk_used + 15) & ~(size_t)15;
    if (o + n > MK_BYTES)
        return (size_t)-1;
    if (p)
        memcpy(mk_data + o, p, n);
    mk_used = o + n;
    return o;
}

static uint32_t mk_ix(const uint8_t *p, unsigned ts, GLsizei i)
{
    return ts == 2 ? ((const uint16_t *)p)[i] : ts == 1 ? p[i] : ((const uint32_t *)p)[i];
}

int mark_take(mark_t *m, GLenum mode, GLsizei count, GLenum type, const void *idx, int normals)
{
    if (mk_frame != g_frame_no + 1) {
        mk_frame = g_frame_no + 1;
        mk_used = mk_sent = 0;
    }
    gmv_gl_arr(GA_VERTEX, &m->a[0]);
    if (normals)
        gmv_gl_arr(GA_NORMAL, &m->a[1]);
    m->mode = mode, m->type = type, m->count = count, m->stipple = sf_level;
    unsigned ts = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_BYTE ? 1 : 4;
    GLint eb = gmv_gl_buf(0x8895);
    int copy = !m->a[0].buf || (normals && !m->a[1].buf);
    uint32_t lo = 0, hi = 0;
    if (eb && !copy) {
        m->ibuf = (GLuint)eb;
        m->ix = (uintptr_t)idx;
    } else {
        const uint8_t *ix = eb ? gmv_game_gl_buffer_bytes((unsigned)eb, (uintptr_t)idx) : idx;
        size_t o = ix ? mk_put(NULL, (size_t)count * ts) : (size_t)-1;
        if (o == (size_t)-1)
            return 0;
        if (copy) {
            lo = 0xffffffffu;
            for (GLsizei i = 0; i < count; i++) {
                uint32_t v = mk_ix(ix, ts, i);
                lo = v < lo ? v : lo, hi = v > hi ? v : hi;
            }
        }
        uint8_t *d = mk_data + o;
        for (GLsizei i = 0; i < count; i++) {
            uint32_t v = mk_ix(ix, ts, i) - lo;
            if (ts == 2) ((uint16_t *)d)[i] = (uint16_t)v;
            else if (ts == 1) d[i] = (uint8_t)v;
            else ((uint32_t *)d)[i] = v;
        }
        m->ibuf = 0;
        m->ix = o;
    }
    for (int k = 0; k <= normals; k++) {
        gmv_arr_t *a = &m->a[k];
        GLint t = a->type;
        uintptr_t es = (uintptr_t)a->size * (t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : t == GL_BYTE || t == GL_UNSIGNED_BYTE ? 1 : 4);
        uintptr_t st = a->stride ? (uintptr_t)a->stride : es;
        const char *p = a->ptr + lo * st;
        if (a->buf) {
            a->ptr = p;
            continue;
        }
        size_t o = mk_put(p, (hi - lo) * st + es);
        if (o == (size_t)-1)
            return 0;
        a->ptr = (const char *)o;
    }
    return 1;
}

void mark_begin(int normals)
{
    if (!mk.DrawElements) {
        *(void **)&mk.BindBuffer = gl_real("glBindBuffer");
        *(void **)&mk.GenBuffers = gl_real("glGenBuffers");
        *(void **)&mk.BufferData = gl_real("glBufferData");
        *(void **)&mk.ClientActiveTexture = gl_real("glClientActiveTexture");
        *(void **)&mk.DrawElements = gl_real("glDrawElements");
    }
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    if (!mk_buf)
        mk.GenBuffers(1, &mk_buf);
    mk.BindBuffer(0x8892, mk_buf);
    if (mk_sent != mk_used) {
        mk.BufferData(0x8892, (intptr_t)mk_used, mk_data, 0x88E0);
        mk_sent = mk_used;
    }
    glDisableClientState(GL_COLOR_ARRAY);
    for (GLenum u = 0x84C3; u >= 0x84C0; u--) {
        mk.ClientActiveTexture(u);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    if (normals)
        glEnableClientState(GL_NORMAL_ARRAY);
    else
        glDisableClientState(GL_NORMAL_ARRAY);
    mk_stip = 0;
}

void mark_draw(const mark_t *m, int normals)
{
    mk.BindBuffer(0x8892, m->a[0].buf ? (GLuint)m->a[0].buf : mk_buf);
    glVertexPointer(m->a[0].size, (GLenum)m->a[0].type, m->a[0].stride, m->a[0].ptr);
    if (normals) {
        mk.BindBuffer(0x8892, m->a[1].buf ? (GLuint)m->a[1].buf : mk_buf);
        glNormalPointer((GLenum)m->a[1].type, m->a[1].stride, m->a[1].ptr);
    }
    mk.BindBuffer(0x8893, m->ibuf ? m->ibuf : mk_buf);
    if (m->stipple && m->stipple != mk_stip)
        glPolygonStipple(sf_pat[m->stipple - 1]);
    if (!m->stipple != !mk_stip) {
        if (m->stipple)
            glEnable(GL_POLYGON_STIPPLE);
        else
            glDisable(GL_POLYGON_STIPPLE);
    }
    mk_stip = m->stipple;
    mk.DrawElements(m->mode, m->count, m->type, (const void *)m->ix);
}

void mark_end(void)
{
    if (mk_stip)
        glDisable(GL_POLYGON_STIPPLE);
    glPopClientAttrib();
}

#if GMV_DEV
static struct {
    unsigned alpha, game, on;
    long long t;
} a2c_n;
#endif

static int a2c_begin(unsigned tf, int dk)
{
#if GMV_DEV
    a2c_n.alpha += g_alpha_on;
    a2c_n.game += g_alpha_on && g_game_a2c;
#endif
    if (!g_a2c_want || !g_alpha_on || g_game_a2c || g_msaa < 2 || tf || dk || g_blend_on || !g_ao_dmask ||
        g_sky_glow_draw || g_glt.last_fb || !(g_tf_tex2d & g_tf_tcarr & 1))
        return 0;
    ao_proj_refresh();
    if (!g_ao_persp)
        return 0;
#if GMV_DEV
    long long t0 = gmv_perf_on() ? gmv_prof_now() : 0;
#endif
    glEnable(0x809E);
#if GMV_DEV
    if (t0) a2c_n.t += gmv_prof_now() - t0;
    a2c_n.on++;
#endif
    return 1;
}

static void a2c_end(int on)
{
    if (!on)
        return;
#if GMV_DEV
    long long t0 = gmv_perf_on() ? gmv_prof_now() : 0;
#endif
    glDisable(0x809E);
#if GMV_DEV
    if (t0) a2c_n.t += gmv_prof_now() - t0;
#endif
}

#if GMV_DEV
void gmv_a2c_report(double seconds)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    gmv_logfile("[perf]   ALPHA_TO_COVERAGE=%d: %.0f alpha-tested draws a second, %.0f with the game's own 0x809e, "
                "%.0f given it here (%.3f ms a second in glEnable/glDisable 0x809e); last glAlphaFunc 0x%x %.3f\n",
                g_a2c_want, a2c_n.alpha / seconds, a2c_n.game / seconds, a2c_n.on / seconds,
                a2c_n.t * 1000.0 / (double)f.QuadPart / seconds, g_alpha_func, g_alpha_ref);
    memset(&a2c_n, 0, sizeof a2c_n);
    static int ab = -1;
    if (ab < 0) {
        const char *e = gmv_dbgenv("GMV_A2C_AB");
        ab = gmv_env_on(e);
    }
    if (ab)
        g_a2c_want = !g_a2c_want;
}
#endif

void gmv_game_gl_setup(void)
{
    const char *af = gmv_getenv("GMV_ANISOTROPIC_FILTERING");
    g_aniso = af ? (float)atof(af) : 0;
    if (g_aniso > 16)
        g_aniso = 16;
    const char *ao_env = gmv_getenv("GMV_AMBIENT_OCCLUSION");
    g_ao_want = gmv_env_on(ao_env);
    const char *as = gmv_getenv("GMV_AO_STRENGTH"), *ar = gmv_getenv("GMV_AO_RADIUS");
    if (as && *as) g_ao_strength = (float)atof(as);
    if (ar && *ar && atof(ar) > 0) g_ao_radius = (float)atof(ar);
    if (g_ao_want)
        gmv_log("[ao] AMBIENT_OCCLUSION=1: radius %g, strength %g\n", g_ao_radius, g_ao_strength);
    const char *bl_on = gmv_getenv("GMV_BLOOM");
    int bl_level = bl_on && *bl_on ? atoi(bl_on) : 0;
    g_bloom_want = bl_level > 0;
    const char *bi = gmv_getenv("GMV_BLOOM_INTENSITY"), *bt = gmv_getenv("GMV_BLOOM_THRESHOLD");
    if (bi && *bi && atof(bi) >= 0) g_bloom_intensity = (float)atof(bi);
    if (bt && *bt) g_bloom_threshold = (float)atof(bt);
    g_bloom_threshold = g_bloom_threshold < 0 ? 0 : g_bloom_threshold > 0.95f ? 0.95f : g_bloom_threshold;
    g_bloom_lights = bloom_part(bl_level, "GMV_BLOOM_LIGHTS");
    g_bloom_windows = bloom_part(bl_level, "GMV_BLOOM_WINDOWS");
    g_bg_want = g_bloom_want && (g_bloom_lights > 0 || g_bloom_windows > 0);
    if (g_bloom_want)
        gmv_log("[bloom] BLOOM=%d: threshold %g, intensity %g; lights %g, windows %g\n", bl_level, g_bloom_threshold,
                g_bloom_intensity, g_bloom_lights, g_bloom_windows);
    const char *wr = gmv_getenv("GMV_WATER_REFLECTIONS"), *rs = gmv_getenv("GMV_REFLECTION_STRENGTH");
    g_ssr_want = gmv_env_on(wr);
    if (rs && *rs) g_ssr_strength = (float)atof(rs);
    g_ssr_strength = g_ssr_strength < 0 ? 0 : g_ssr_strength > 2 ? 2 : g_ssr_strength;
    if (g_ssr_want)
        gmv_log("[ssr] WATER_REFLECTIONS=1: strength %g\n", g_ssr_strength);
    const char *vr_on = gmv_getenv("GMV_VEHICLE_REFLECTIONS"), *vs = gmv_getenv("GMV_VEHICLE_REFLECTION_STRENGTH");
    g_vr_want = gmv_env_on(vr_on);
    if (vs && *vs) g_vr_strength = (float)atof(vs);
    g_vr_strength = g_vr_strength < 0 ? 0 : g_vr_strength > 2 ? 2 : g_vr_strength;
    if (g_vr_want)
        gmv_log("[vr] VEHICLE_REFLECTIONS=1: strength %g\n", g_vr_strength);
    const char *ds_on = gmv_getenv("GMV_DYNAMIC_SHADOWS");
    g_ds_want = gmv_env_on(ds_on);
    const char *sq = gmv_getenv("GMV_SHADOW_QUALITY");
    g_shadow_quality = gmv_setting_is_auto(sq) ? 0 : *sq == '1' ? 1 : 2;
    if (g_ds_want && g_shadow_quality)
        gmv_log("[shadow] DYNAMIC_SHADOWS=1, SHADOW_QUALITY=%d\n", g_shadow_quality);
    else if (g_ds_want)
        gmv_log("[shadow] DYNAMIC_SHADOWS=1, SHADOW_QUALITY=auto\n");
    const char *sr = gmv_getenv("GMV_SHARED_RESOLVE");
    g_shared_resolve = !sr || !*sr || *sr != '0';
    if (g_ao_want || g_bloom_want || g_ssr_want || g_vr_want || g_ds_want)
        gmv_log("[post] SHARED_RESOLVE=%d: %s\n", g_shared_resolve,
                g_shared_resolve ? "the screen's depth copied once a frame for the effects" : "a depth copy for each effect");
#if GMV_DEV
    const char *vrd_on = gmv_dbgenv("GMV_VEHICLE_REFLECTION_DEBUG");
    g_vr_debug = gmv_env_on(vrd_on);
    if (g_vr_debug)
        gmv_log("[vr] VEHICLE_REFLECTION_DEBUG=1: the cars' pixels tinted by what became of them (red covered, grey "
                "fogged, blue edge-on, magenta normal turned round, yellow ray missed (screen sphere map), green hit, cyan hit past "
                "the car itself)%s\n", g_vr_want ? "" : "; VEHICLE_REFLECTIONS is off at startup");
#endif
    const char *a2c = gmv_getenv("GMV_ALPHA_TO_COVERAGE");
    g_a2c_want = gmv_env_on(a2c);
    if (g_a2c_want)
        gmv_log("[aa] ALPHA_TO_COVERAGE=1: the world's alpha-tested draws get alpha to coverage while MSAA is on\n");
    const char *pa = gmv_getenv("GMV_POST_AA");
    g_paa_want = pa && !_stricmp(pa, "auto") ? 2 : gmv_env_on(pa);
    if (g_paa_want)
        gmv_log("[aa] POST_AA=%s: FXAA on the 3D picture under the HUD%s\n", g_paa_want == 2 ? "auto" : "1",
                g_paa_want == 2 ? " while MSAA is off" : "");
    const char *sfw = gmv_getenv("GMV_SPAWN_FADE");
    sf_want = gmv_env_on(sfw);
    g_fog_mode = gmv_fog_mode();
    fog_track = g_fog_mode > 0;
    const char *fh_h = gmv_getenv("GMV_FOG_HEIGHT");
    if (fh_h && *fh_h && atof(fh_h) >= 0) g_fog_height = (float)atof(fh_h) * 100;
    if (g_fog_mode == 2)
        gmv_log("[fog] FOG_MODE=2: the hybrid fog, thinning with height over %g m\n", g_fog_height / 100);
    const char *fsf = gmv_getenv("GMV_FOG_SKIP_FAR");
    g_fog_skip_far = !fsf || !*fsf || *fsf != '0';
    if (g_fog_mode > 0)
        gmv_log("[fog] FOG_SKIP_FAR=%d: draws wholly past the fog's end %s\n", g_fog_skip_far,
                g_fog_skip_far ? "are left out" : "are sent and clipped");
}

int gmv_game_gl_wants_draws(void)
{
    return g_ao_want || g_bloom_want || g_ssr_want || g_vr_want || g_ds_want || fog_track || g_tab_live || sf_want ||
           g_a2c_want || g_paa_want;
}

void *gmv_game_gl_intercept(const char *name)
{
    if (g_ao_want || g_bloom_want || g_ssr_want || g_vr_want || g_ds_want || fog_track || g_tab_live || g_a2c_want ||
        g_paa_want) {
        void *t = gl_ao_intercept(name);
        if (t)
            return t;
    }
    if (gmv_padicons_on()) {
        if (!strcmp(name, "_glCompressedTexImage2D"))
            return (void *)pi_CompressedTexImage2D;
        if (!strcmp(name, "_glDeleteTextures"))
            return (void *)pi_DeleteTextures;
    }
    if (g_aniso > 1 || g_tab_live) {
        if (!strcmp(name, "_glTexParameterf"))
            return (void *)af_TexParameterf;
        if (!strcmp(name, "_glTexParameteri"))
            return (void *)af_TexParameteri;
        if (g_tab_live && !strcmp(name, "_glDeleteTextures"))
            return (void *)af_DeleteTextures;
    }
    return NULL;
}

void *gmv_game_gl_draw_intercept(const char *name)
{
    (void)name;
    return NULL;
}

void gmv_game_gl_cap(unsigned cap, int on)
{
    ao_cap(cap, on);
}

void gmv_game_gl_clear_begin(unsigned mask)
{
    ao_clear(mask);
}

void gmv_game_gl_clear_end(unsigned mask)
{
    bg_clear(mask);
}

const void *gmv_game_gl_buffer_bytes(unsigned name, uintptr_t off)
{
    return gmv_vbostatic_client(name, off);
}

void gmv_game_gl_client_unit(unsigned unit)
{
    (void)unit;
}

void gmv_game_gl_client_state(unsigned array, int on)
{
    (void)array;
    (void)on;
}

void gmv_game_frame_end(void)
{
    ao_frame_end();
}

void gmv_game_after_present(void)
{
    live_sync();
}

void gmv_game_draw_arrays(void *real, unsigned mode, int first, int count, void *caller)
{
    (void)caller;
    ao_draw();
    unsigned tf = texfix_begin(mode);
    int ff = fog_force_begin();
    int cl = fog_clip_begin();
    int skip = cl ? fog_skip_far() : 0;
    int fhy = fog_hybrid_begin(mode, ff, tf);
    glow_state_t gs;
    if (g_sky_glow_draw) glow_begin(&gs);
    int bgd = bg_draw_begin(mode);
    int cpm = bg_cp_mark();
    int sfs = sf_stipple_begin();
    int a2c = a2c_begin(tf, 0);
    if (real && skip != 1) {
        if (skip) fog_skip_check(1);
        ((void(APIENTRY *)(GLenum, GLint, GLsizei))real)(mode, first, count);
        if (skip) fog_skip_check(0);
    }
    a2c_end(a2c);
    if (g_sky_glow_draw) glow_end(&gs);
    if (bgd) bg_draw_end(bgd, 0, mode, first, count, 0, NULL);
    ds_mask_end(cpm);
    sf_stipple_end(sfs);
    if (cl) glDisable(FOG_CLIP_PLANE);
    fog_hybrid_end(fhy);
    fog_force_end(ff);
    texfix_end(tf);
}

#if GMV_DEV
static struct {
    unsigned seen, client, mixed, vbo, cidx, none;
    double bytes;
} sp;

static void stream_probe(GLsizei count, GLenum type, const void *idx)
{
    static int on = -1;
    if (on < 0) {
        const char *e = gmv_dbgenv("GMV_STREAM_PROBE");
        on = gmv_perf_on() && gmv_env_on(e);
    }
    if (!on || (sp.seen++ & 15))
        return;
    static const struct { GLenum en, buf, sz, ty, st; } arr[4] = {
        {GL_VERTEX_ARRAY, 0x8896, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE},
        {GL_COLOR_ARRAY, 0x8898, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE},
        {GL_NORMAL_ARRAY, 0x8897, 0, GL_NORMAL_ARRAY_TYPE, GL_NORMAL_ARRAY_STRIDE},
        {GL_TEXTURE_COORD_ARRAY, 0x889A, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE,
         GL_TEXTURE_COORD_ARRAY_STRIDE},
    };
    int nc = 0, nb = 0, elem = 0;
    GLint eb = 0;
    glGetIntegerv(0x8895, &eb);
    unsigned lo = ~0u, hi = 0;
    if (!eb && idx && count > 0) {
        for (GLsizei i = 0; i < count; i++) {
            unsigned v = type == GL_UNSIGNED_SHORT ? ((const uint16_t *)idx)[i]
                         : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)idx)[i] : ((const uint32_t *)idx)[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    double bytes = 0;
    for (int a = 0; a < 4; a++) {
        if (!glIsEnabled(arr[a].en))
            continue;
        GLint b = 0, sz = 3, ty = GL_FLOAT, st = 0;
        glGetIntegerv(arr[a].buf, &b);
        if (b) {
            nb++;
            continue;
        }
        nc++;
        if (arr[a].sz) glGetIntegerv(arr[a].sz, &sz);
        glGetIntegerv(arr[a].ty, &ty);
        glGetIntegerv(arr[a].st, &st);
        elem = ty == GL_FLOAT || ty == GL_INT || ty == GL_UNSIGNED_INT ? 4 : ty == GL_SHORT || ty == GL_UNSIGNED_SHORT ? 2 : 1;
        if (!st) st = sz * elem;
        if (hi >= lo && lo != ~0u)
            bytes += (double)(hi - lo + 1) * st;
    }
    if (!nc && !nb) sp.none++;
    else if (nc && nb) sp.mixed++;
    else if (nc) sp.client++;
    else sp.vbo++;
    if (!eb) sp.cidx++;
    sp.bytes += bytes;
}

void gmv_stream_probe_report(double seconds)
{
    unsigned n = sp.client + sp.mixed + sp.vbo + sp.none;
    if (n)
        gmv_logfile("[perf]   draw arrays (1 draw in 16): %.0f glDrawElements a second; vertex arrays all client %.0f%%, "
                    "mixed with buffers %.0f%%, all buffers %.0f%%; client indices %.0f%%; client bytes in the index range "
                    "%.1f KB a draw, %.1f MB a second\n",
                    sp.seen / seconds, sp.client * 100.0 / n, sp.mixed * 100.0 / n, sp.vbo * 100.0 / n,
                    sp.cidx * 100.0 / n, sp.bytes / n / 1024, sp.bytes / n * sp.seen / seconds / 1048576);
    memset(&sp, 0, sizeof sp);
}
#endif

void gmv_game_draw_elements(void *real, unsigned mode, int count, unsigned type, const void *idx, void *caller)
{
    (void)caller;
#if GMV_DEV
    stream_probe(count, type, idx);
#endif
    ao_draw();
    int body = (g_vr_want && g_vr) || (g_ds_want && g_ds) ? shine_body(mode, count) : 0;
    int dk = ds_kind(mode, count, body);
    int dm = ds_mask_begin(dk);
    unsigned tf = texfix_begin(mode);
    int sf = sky_read(count, type, idx);
    int ff = fog_force_begin();
    int cl = fog_clip_begin();
    int skip = cl ? fog_skip_far() : 0;
    int fhy = fog_hybrid_begin(mode, ff, tf);
    glow_state_t gs;
    if (g_sky_glow_draw) glow_begin(&gs);
    int bgd = bg_draw_begin(mode);
    int sm = sea_mark_begin(count, type, idx);
    int cpm = bg_cp_mark();
    int sfs = sf_stipple_begin();
    int a2c = a2c_begin(tf, dk);
    if (real && skip != 1) {
        if (skip) fog_skip_check(1);
        ((void(APIENTRY *)(GLenum, GLsizei, GLenum, const void *))real)(mode, count, type, idx);
        if (skip) fog_skip_check(0);
    }
    a2c_end(a2c);
    if (g_sky_glow_draw) glow_end(&gs);
    if (bgd) bg_draw_end(bgd, 1, mode, 0, count, type, idx);
    if (sf) sky_floor_end();
    if (ff == 1) sea_beyond(count, type, idx);
    sea_mark_end(sm);
    ds_mask_end(cpm);
    if (cl) glDisable(FOG_CLIP_PLANE);
    fog_hybrid_end(fhy);
    fog_force_end(ff);
    texfix_end(tf);
    ds_mask_end(dm);
    if (body == 1 && g_vr_want && g_vr) car_mark(mode, count, type, idx);
    if (dk) ds_mark(dk, mode, count, type, idx);
    sf_stipple_end(sfs);
}
