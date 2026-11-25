#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#if GMV_DEV
#define GMV_GLQ_NO_MACROS
#endif
#include "glshim.h"

/* TEXFIX 0x1a5cea 0x1b5094 */
int g_texfix = 1;
unsigned g_tf_active, g_tf_client;
uint8_t g_tf_tex2d, g_tf_tcarr;
GLuint g_tf_bound[8];
static unsigned g_tf_fixed;

#if GMV_DEV
static struct {
    int on;
    unsigned calls[SC_N], same[SC_N];
    GLuint tex[4];
    unsigned draws, same_sig, same_mv, merge;
    GLint sig[22];
    GLfloat mv[16];
    const char *end;
    int have;
} dp = {.on = -1};

void gmv_gl_state_note(int kind, int same)
{
    dp.calls[kind]++;
    dp.same[kind] += same != 0;
}

static GLint dp_ptr(GLenum pname)
{
    void *p = NULL;
    glGetPointerv(pname, &p);
    return (GLint)(uintptr_t)p;
}

static void draw_probe(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    if (dp.on < 0) {
        const char *e = gmv_dbgenv("GMV_DRAW_PROBE");
        dp.on = gmv_perf_on() && gmv_env_on(e);
    }
    if (!dp.on)
        return;
    GLint s[22] = {0};
    GLfloat mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetIntegerv(GL_BLEND_SRC, &s[0]);
    glGetIntegerv(GL_BLEND_DST, &s[1]);
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &s[2]);
    glGetFloatv(GL_ALPHA_TEST_REF, (GLfloat *)&s[3]);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &s[4]);
    s[5] = glIsEnabled(GL_ALPHA_TEST) | glIsEnabled(GL_BLEND) << 1 | glIsEnabled(GL_DEPTH_TEST) << 2 |
           glIsEnabled(GL_FOG) << 3 | glIsEnabled(GL_CULL_FACE) << 4;
    s[6] = g_tf_tex2d | g_tf_tcarr << 8;
    s[7] = (GLint)dp.tex[0];
    s[8] = (GLint)dp.tex[1];
    s[9] = g_te[0].mode;
    s[10] = g_te[1].mode;
    s[11] = g_te[0].crgb;
    s[12] = g_te[1].crgb;
    glGetFloatv(GL_CURRENT_COLOR, (GLfloat *)&s[13]);
    glGetIntegerv(0x8896, &s[17]);
    s[18] = dp_ptr(GL_VERTEX_ARRAY_POINTER);
    s[19] = dp_ptr(GL_TEXTURE_COORD_ARRAY_POINTER) ^ dp_ptr(GL_COLOR_ARRAY_POINTER);
    glGetIntegerv(0x8895, &s[20]);
    s[21] = (GLint)(mode | type << 16);
    int same = dp.have && !memcmp(s, dp.sig, sizeof s);
    int same_mv = same && !memcmp(mv, dp.mv, sizeof mv);
    const char *at = idx;
    dp.draws++;
    dp.same_sig += same;
    dp.same_mv += same_mv;
    dp.merge += same_mv && mode == GL_TRIANGLES && at == dp.end;
    dp.end = at + (size_t)count * (type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_BYTE ? 1 : 4);
    memcpy(dp.sig, s, sizeof s);
    memcpy(dp.mv, mv, sizeof mv);
    dp.have = 1;
}

void gmv_gl_draw_probe_report(double seconds)
{
    static const char *const nm[SC_N] = {"glBindTexture", "glTexEnv", "glBlendFunc", "glAlphaFunc", "glDepthMask"};
    char b[400];
    int len = 0;
    for (int k = 0; k < SC_N && len < (int)sizeof b - 80; k++)
        len += snprintf(b + len, sizeof b - len, "%s %s %.0f (%.0f%% the same)", k ? "," : "", nm[k],
                        dp.calls[k] / seconds, dp.calls[k] ? dp.same[k] * 100.0 / dp.calls[k] : 0.0);
    gmv_logfile("[perf]   state calls a second:%s\n", b);
    if (dp.draws)
        gmv_logfile("[perf]   draw probe: %.0f glDrawElements a second; state as the draw before %.1f%%, with its "
                    "modelview too %.1f%%, and triangles going on from its indices %.1f%% (%.0f a second)\n",
                    dp.draws / seconds, dp.same_sig * 100.0 / dp.draws, dp.same_mv * 100.0 / dp.draws,
                    dp.merge * 100.0 / dp.draws, dp.merge / seconds);
    memset(dp.calls, 0, sizeof dp.calls);
    memset(dp.same, 0, sizeof dp.same);
    dp.draws = dp.same_sig = dp.same_mv = dp.merge = 0;
}
#endif

void(APIENTRY *tf_active_real(void))(GLenum)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) { real = gl_real("glActiveTexture"); if (!real) real = gl_real("glActiveTextureARB"); }
    return real;
}
static void tr_ActiveTexture(GLenum unit)
{
    void(APIENTRY * real)(GLenum) = tf_active_real();
    g_tf_active = (unit - 0x84C0) & 7;
    if (real) real(unit);
}
static void tr_ClientActiveTexture(GLenum unit)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) { real = gl_real("glClientActiveTexture"); if (!real) real = gl_real("glClientActiveTextureARB"); }
    g_tf_client = (unit - 0x84C0) & 7;
    gmv_game_gl_client_unit(g_tf_client);
    if (real) real(unit);
}
static void tr_cap(GLenum cap, int on)
{
    if (cap == GL_SCISSOR_TEST) g_glt.scissor = on;
    else if (cap == GL_STENCIL_TEST) g_glt.stencil = on;
    else if (cap == GL_CULL_FACE) g_glt.cull = on;
    else if (cap == GL_POLYGON_OFFSET_FILL) g_glt.poly_fill = on;
}
static void tr_Enable(GLenum cap)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glEnable");
    if (cap == GL_TEXTURE_2D) g_tf_tex2d |= (uint8_t)(1u << g_tf_active);
    tr_cap(cap, 1);
    gmv_game_gl_cap(cap, 1);
    if (real) real(cap);
}
static void tr_Disable(GLenum cap)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glDisable");
    if (cap == GL_TEXTURE_2D) g_tf_tex2d &= (uint8_t)~(1u << g_tf_active);
    tr_cap(cap, 0);
    gmv_game_gl_cap(cap, 0);
    if (real) real(cap);
}
static void tr_EnableClientState(GLenum a)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glEnableClientState");
    if (a == GL_TEXTURE_COORD_ARRAY) g_tf_tcarr |= (uint8_t)(1u << g_tf_client);
    gmv_gl_arr_state(a, 1);
    gmv_game_gl_client_state(a, 1);
    if (real) real(a);
}
static void tr_DisableClientState(GLenum a)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glDisableClientState");
    if (a == GL_TEXTURE_COORD_ARRAY) g_tf_tcarr &= (uint8_t)~(1u << g_tf_client);
    gmv_gl_arr_state(a, 0);
    gmv_game_gl_client_state(a, 0);
    if (real) real(a);
}
#define TE_DEFAULT {GL_MODULATE, GL_MODULATE, GL_MODULATE, {0x1702, 0x8578, 0x8576, 0x1702, 0x8578, 0x8576}, \
                    {0x300, 0x300, 0x302, 0x302, 0x302, 0x302}, {1, 1}}
g_te_t g_te[4] = {TE_DEFAULT, TE_DEFAULT, TE_DEFAULT, TE_DEFAULT};

static void te_note(GLenum target, GLenum pname, GLfloat v)
{
    if (target != 0x2300 || g_tf_active >= 4)
        return;
#if GMV_DEV
    g_te_t was = g_te[g_tf_active];
#endif
    GLint i = (GLint)v;
    if (pname == 0x2200) g_te[g_tf_active].mode = i;
    else if (pname == 0x8571) g_te[g_tf_active].crgb = i;
    else if (pname == 0x8572) g_te[g_tf_active].calpha = i;
    else if (pname >= 0x8580 && pname <= 0x8582) g_te[g_tf_active].src[pname - 0x8580] = i;
    else if (pname >= 0x8588 && pname <= 0x858A) g_te[g_tf_active].src[pname - 0x8588 + 3] = i;
    else if (pname >= 0x8590 && pname <= 0x8592) g_te[g_tf_active].op[pname - 0x8590] = i;
    else if (pname >= 0x8598 && pname <= 0x859A) g_te[g_tf_active].op[pname - 0x8598 + 3] = i;
    else if (pname == 0x8573) g_te[g_tf_active].scale[0] = v;
    else if (pname == 0x0D1C) g_te[g_tf_active].scale[1] = v;
#if GMV_DEV
    else
        return;
    gmv_gl_state_note(SC_ENV, !memcmp(&was, &g_te[g_tf_active], sizeof was));
#endif
}
static void tr_TexEnvi(GLenum target, GLenum pname, GLint v)
{
    static void(APIENTRY * real)(GLenum, GLenum, GLint);
    if (!real) real = gl_real("glTexEnvi");
    te_note(target, pname, (GLfloat)v);
    if (real) real(target, pname, v);
}
static void tr_TexEnvf(GLenum target, GLenum pname, GLfloat v)
{
    static void(APIENTRY * real)(GLenum, GLenum, GLfloat);
    if (!real) real = gl_real("glTexEnvf");
    te_note(target, pname, v);
    if (real) real(target, pname, v);
}
static void tr_TexEnviv(GLenum target, GLenum pname, const GLint *v)
{
    static void(APIENTRY * real)(GLenum, GLenum, const GLint *);
    if (!real) real = gl_real("glTexEnviv");
    if (v) te_note(target, pname, (GLfloat)v[0]);
    if (real) real(target, pname, v);
}
static void tr_TexEnvfv(GLenum target, GLenum pname, const GLfloat *v)
{
    static void(APIENTRY * real)(GLenum, GLenum, const GLfloat *);
    if (!real) real = gl_real("glTexEnvfv");
    if (v) te_note(target, pname, v[0]);
    if (real) real(target, pname, v);
}

unsigned texfix_begin(GLenum mode)
{
    unsigned m = g_texfix && mode != GL_POINTS ? (unsigned)(g_tf_tex2d & ~g_tf_tcarr) & 0xF : 0;
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (!m || !act)
        return 0;
    for (unsigned u = 0; u < 4; u++)
        if (m & (1u << u)) { act(0x84C0 + u); glDisable(GL_TEXTURE_2D); }
    act(0x84C0 + g_tf_active);
    g_tf_fixed++;
    return m;
}
void texfix_end(unsigned m)
{
    void(APIENTRY * act)(GLenum) = tf_active_real();
    if (!m || !act)
        return;
    for (unsigned u = 0; u < 4; u++)
        if (m & (1u << u)) { act(0x84C0 + u); glEnable(GL_TEXTURE_2D); }
    act(0x84C0 + g_tf_active);
}

static void tr_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void(APIENTRY * real)(GLenum, GLint, GLsizei);
    if (!real) real = gl_real("glDrawArrays");
    g_glt.da_calls++; g_glt.da_verts += count;
    PM_DRAW();
    g_cap_draws++;
    if (g_cap_ticks >= 0) g_cap_run++;
    if (g_fade_pending) fade_state_log("arrays", mode, count);
    g_drew_since_swap = 1;
#if GMV_DEV
    long long t0 = gmv_perf_on() ? gmv_prof_now() : 0;
#endif
    unsigned cm = gmv_sv_arrays(first, count);
    gmv_game_draw_arrays((void *)real, mode, first, count, __builtin_return_address(0));
#if GMV_DEV
    if (t0) gmv_sv_draw_time(cm, t0);
#else
    (void)cm;
#endif
    post_fade_probe("drawarrays mode/count", mode, (unsigned)count);
}
#if GMV_DEV
static void fade_quad_dump(GLsizei count, GLenum type, const void *idx)
{
    static struct { uint32_t caller; unsigned n; } seen[8];
    uint32_t caller = g_quad_probe;
    if (count != 6)
        return;
    g_quad_probe = 0;
    unsigned *n = &seen[7].n;
    for (int i = 0; i < 8; i++)
        if (seen[i].caller == caller || !seen[i].caller) {
            seen[i].caller = caller;
            n = &seen[i].n;
            break;
        }
    if ((*n)++ >= 4)
        return;
    g_corner_watch = 1;
    GLenum err_before = glGetError();
    void(APIENTRY * active_tex)(GLenum) = gl_real("glActiveTexture");
    if (!active_tex) active_tex = gl_real("glActiveTextureARB");
    GLint act = 0, cact = 0, ebo = 0, abo = 0;
    glGetIntegerv(0x84E0, &act);
    glGetIntegerv(0x84E1, &cact);
    glGetIntegerv(0x8895, &ebo);
    glGetIntegerv(0x8894, &abo);
    char units[160] = "";
    if (active_tex) {
        size_t ul = 0;
        for (int u = 0; u < 4; u++) {
            active_tex(0x84C0 + u);
            GLint t = 0, env = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
            ul += snprintf(units + ul, sizeof units - ul, " u%d:%s tex=%d env=0x%x", u,
                           glIsEnabled(GL_TEXTURE_2D) ? "on" : "off", t, env);
        }
        active_tex(act);
    }
    GLint tw = 0, th = 0, tif = 0, tmin = 0, tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &tif);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &tmin);
    gmv_log("[fadequad] caller=0x%06x err_before=0x%x active=0x%x clientactive=0x%x ebo=%d abo=%d%s | bound tex=%d %dx%d ifmt=0x%x min=0x%x\n",
            caller, err_before, act, cact, ebo, abo, units, tex, tw, th, tif, tmin);
    GLfloat tc[4] = {0, 0, 0, 0};
    glGetFloatv(0x0B03, tc);
    gmv_log("[fadequad]   current texcoord=%.3f,%.3f,%.3f,%.3f  texfix=%s tex2d_bits=0x%x tcarray_bits=0x%x patched_draws=%u\n",
            tc[0], tc[1], tc[2], tc[3], g_texfix ? "on" : "off", g_tf_tex2d, g_tf_tcarr, g_tf_fixed);
    struct { GLenum en, sz, ty, st, ptr; const char *nm; } arr[3] = {
        {GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE, GL_VERTEX_ARRAY_POINTER, "pos"},
        {GL_COLOR_ARRAY, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE, GL_COLOR_ARRAY_POINTER, "col"},
        {GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE,
         GL_TEXTURE_COORD_ARRAY_POINTER, "uv"},
    };
    GLint asz[3], aty[3], ast[3];
    const unsigned char *aptr[3];
    for (int a = 0; a < 3; a++) {
        void *p = NULL;
        glGetIntegerv(arr[a].sz, &asz[a]);
        glGetIntegerv(arr[a].ty, &aty[a]);
        glGetIntegerv(arr[a].st, &ast[a]);
        glGetPointerv(arr[a].ptr, &p);
        aptr[a] = glIsEnabled(arr[a].en) ? p : NULL;
        gmv_log("[fadequad]   %s enabled=%d size=%d type=0x%x stride=%d ptr=%p\n", arr[a].nm,
                glIsEnabled(arr[a].en), asz[a], aty[a], ast[a], p);
    }
    GLfloat proj[16], mv[16];
    glGetFloatv(GL_PROJECTION_MATRIX, proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    gmv_log("[fadequad]   projection x=%.6f,%.6f,%.6f,%.6f y=%.6f,%.6f,%.6f,%.6f (scale, translate in rows)\n",
            proj[0], proj[4], proj[8], proj[12], proj[1], proj[5], proj[9], proj[13]);
    gmv_log("[fadequad]   modelview  x=%.6f,%.6f,%.6f,%.6f y=%.6f,%.6f,%.6f,%.6f\n",
            mv[0], mv[4], mv[8], mv[12], mv[1], mv[5], mv[9], mv[13]);
    GLint qvp[4] = {0}, qsc[4] = {0}, pmode[2] = {0}, cullm = 0;
    glGetIntegerv(GL_VIEWPORT, qvp);
    glGetIntegerv(GL_SCISSOR_BOX, qsc);
    glGetIntegerv(GL_POLYGON_MODE, pmode);
    glGetIntegerv(GL_CULL_FACE_MODE, &cullm);
    gmv_log("[fadequad]   viewport %d,%d,%d,%d scissor=%d %d,%d,%d,%d polygon mode 0x%x cull=%d 0x%x smooth=%d stipple=%d\n",
            qvp[0], qvp[1], qvp[2], qvp[3], glIsEnabled(GL_SCISSOR_TEST), qsc[0], qsc[1], qsc[2], qsc[3], pmode[0],
            glIsEnabled(GL_CULL_FACE), cullm, glIsEnabled(GL_POLYGON_SMOOTH), glIsEnabled(GL_POLYGON_STIPPLE));
    if (ebo || abo || !idx || IsBadReadPtr(idx, 12))
        return;
    float u0 = -1, v0 = -1;
    for (int k = 0; k < 6; k++) {
        unsigned vi = type == GL_UNSIGNED_SHORT ? ((const uint16_t *)idx)[k]
                    : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)idx)[k] : ((const uint32_t *)idx)[k];
        char line[200];
        size_t ll = snprintf(line, sizeof line, "idx%d=%u", k, vi);
        for (int a = 0; a < 3; a++) {
            if (!aptr[a])
                continue;
            int esz = aty[a] == GL_FLOAT ? 4 : aty[a] == GL_SHORT || aty[a] == GL_UNSIGNED_SHORT ? 2 : 1;
            int ncomp = asz[a] == 0x80E1 ? 4 : asz[a];
            int stride = ast[a] ? ast[a] : esz * ncomp;
            const unsigned char *p = aptr[a] + (size_t)vi * stride;
            if (IsBadReadPtr(p, esz * ncomp))
                continue;
            ll += snprintf(line + ll, sizeof line - ll, " %s=", arr[a].nm);
            for (int c = 0; c < ncomp && ll < sizeof line - 16; c++) {
                if (aty[a] == GL_FLOAT)
                    ll += snprintf(line + ll, sizeof line - ll, "%s%.3f", c ? "," : "", ((const float *)p)[c]);
                else if (esz == 2)
                    ll += snprintf(line + ll, sizeof line - ll, "%s%d", c ? "," : "", ((const int16_t *)p)[c]);
                else
                    ll += snprintf(line + ll, sizeof line - ll, "%s%u", c ? "," : "", p[c]);
            }
            if (a == 2 && aty[a] == GL_FLOAT && k == 0) { u0 = ((const float *)p)[0]; v0 = ((const float *)p)[1]; }
        }
        gmv_log("[fadequad]   %s\n", line);
    }
    if (tex && tw > 0 && th > 0 && (long)tw * th <= 4096L * 4096L) {
        unsigned char *px = malloc((size_t)tw * th * 4);
        if (px) {
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            int x = u0 < 0 ? 0 : (int)(u0 * tw), y = v0 < 0 ? 0 : (int)(v0 * th);
            if (x >= tw) x = tw - 1;
            if (y >= th) y = th - 1;
            const unsigned char *t = px + ((size_t)y * tw + x) * 4;
            gmv_log("[fadequad]   texel at uv0 (%d,%d) = %u,%u,%u,%u; texel(0,0) = %u,%u,%u,%u\n", x, y, t[0], t[1],
                    t[2], t[3], px[0], px[1], px[2], px[3]);
            free(px);
        }
    }
}
#endif

static void tr_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    static void(APIENTRY * real)(GLenum, GLsizei, GLenum, const void *);
    if (!real) real = gl_real("glDrawElements");
    g_glt.de_calls++; g_glt.de_verts += count;
    PM_DRAW();
    g_cap_draws++;
    if (g_cap_ticks >= 0) g_cap_run++;
    if (g_fade_pending) fade_state_log("elements", mode, count);
#if GMV_DEV
    if (g_quad_probe) fade_quad_dump(count, type, idx);
    if (dp.on) draw_probe(mode, count, type, idx);
#endif
    g_drew_since_swap = 1;
    if (g_gl_trace) {
        static int logged;
        if (logged < 2) {
            logged++;
            gmv_log("[gltrace] glDrawElements mode=0x%x count=%d type=0x%x caller=%p\n",
                    mode, count, type, __builtin_return_address(0));
            const uint32_t *sp = (const uint32_t *)&mode;
            for (int k = 0; k < 64; k++)
                if (sp[k] >= GMV_TEXT_LO && sp[k] < GMV_TEXT_HI)
                    gmv_log("        [sp+%02x] -> game 0x%08x\n", k * 4, sp[k]);
        }
    }
#if GMV_DEV
    long long t0 = gmv_perf_on() ? gmv_prof_now() : 0;
#endif
    unsigned cm = gmv_sv_elements(count, type, idx);
    gmv_game_draw_elements((void *)real, mode, count, type, idx, __builtin_return_address(0));
#if GMV_DEV
    if (t0) gmv_sv_draw_time(cm, t0);
#else
    (void)cm;
#endif
#if GMV_DEV
    if (g_corner_watch == 1) {
        fade_corners("after the fade");
        g_corner_watch = 2;
    }
#endif
    post_fade_probe("drawelements mode/count", mode, (unsigned)count);
}
static void tr_BindFramebuffer(GLenum target, GLuint fb)
{
    static void(APIENTRY * real)(GLenum, GLuint);
    if (!real) { real = gl_real("glBindFramebuffer"); if (!real) real = gl_real("glBindFramebufferEXT"); }
    g_glt.last_fb = fb;
#if GMV_DEV
    {
        char b[16];
        snprintf(b, sizeof b, "B%u ", fb);
        cap_seq(b);
    }
#endif
    if (real) real(target, fb ? fb : g_ms_fbo);
    gmv_gl_fb_note(target, fb ? fb : g_ms_fbo);
}
static void tr_DeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    static void(APIENTRY * real)(GLsizei, const GLuint *);
    if (!real) { real = gl_real("glDeleteFramebuffers"); if (!real) real = gl_real("glDeleteFramebuffersEXT"); }
    gmv_gl_fb_forget();
    if (real) real(n, fbs);
}
static void tr_BufferData(GLenum target, intptr_t size, const void *data, GLenum usage)
{
    static void(APIENTRY * real)(GLenum, intptr_t, const void *, GLenum);
    if (!real) real = gl_real("glBufferData");
    if ((long)size > g_glt.max_buf) g_glt.max_buf = (long)size;
    if (real) real(target, size, data, usage);
}
static void tr_BindTexture(GLenum target, GLuint tex)
{
    static void(APIENTRY * real)(GLenum, GLuint);
    if (!real) real = gl_real("glBindTexture");
    g_glt.tex_binds++;
    if (target == GL_TEXTURE_2D)
        g_tf_bound[g_tf_active] = tex;
#if GMV_DEV
    if (target == GL_TEXTURE_2D && g_tf_active < 4) {
        gmv_gl_state_note(SC_BIND, dp.tex[g_tf_active] == tex);
        dp.tex[g_tf_active] = tex;
    }
#endif
    if (real) real(target, tex);
}

static struct { GLushort w, h; } *g_tex_dim;
static GLuint g_tex_dim_n;

void gmv_gl_tex_size(GLenum target, GLint level, GLsizei w, GLsizei h)
{
    GLuint t = g_tf_bound[g_tf_active];
    if (target != GL_TEXTURE_2D || level || !t || t >= (1u << 20))
        return;
    if (t >= g_tex_dim_n) {
        GLuint n = g_tex_dim_n ? g_tex_dim_n : 1024;
        while (n <= t)
            n *= 2;
        void *a = realloc(g_tex_dim, n * sizeof *g_tex_dim);
        if (!a)
            return;
        g_tex_dim = a;
        memset(g_tex_dim + g_tex_dim_n, 0, (n - g_tex_dim_n) * sizeof *g_tex_dim);
        g_tex_dim_n = n;
    }
    g_tex_dim[t].w = (GLushort)w;
    g_tex_dim[t].h = (GLushort)h;
}

void gmv_gl_tex_deleted(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; names && i < n; i++) {
        if (names[i] < g_tex_dim_n)
            g_tex_dim[names[i]].w = g_tex_dim[names[i]].h = 0;
        for (int u = 0; u < 8; u++)
            if (g_tf_bound[u] == names[i])
                g_tf_bound[u] = 0;
    }
}

void gmv_gl_tex_dims(GLuint t, GLint *w, GLint *h)
{
    *w = t < g_tex_dim_n ? g_tex_dim[t].w : 0;
    *h = t < g_tex_dim_n ? g_tex_dim[t].h : 0;
}

static void tr_TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                          GLenum format, GLenum type, const void *px)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                 const void *);
    if (!real) real = gl_real("glTexImage2D");
    gmv_gl_tex_size(target, level, w, h);
    if (real) real(target, level, ifmt, w, h, border, format, type, px);
}
static void tr_CompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLsizei size, const void *data)
{
    static void(APIENTRY * real)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
    if (!real) real = gl_real("glCompressedTexImage2D");
    gmv_gl_tex_size(target, level, w, h);
    if (real) real(target, level, ifmt, w, h, border, size, data);
}
static void tr_DeleteTextures(GLsizei n, const GLuint *names)
{
    static void(APIENTRY * real)(GLsizei, const GLuint *);
    if (!real) real = gl_real("glDeleteTextures");
    gmv_gl_tex_deleted(n, names);
    if (real) real(n, names);
}
static void tr_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    static void(APIENTRY * real)(GLboolean, GLboolean, GLboolean, GLboolean);
    if (!real) real = gl_real("glColorMask");
    g_glt.cmask[0] = r; g_glt.cmask[1] = g; g_glt.cmask[2] = b; g_glt.cmask[3] = a;
    if (real) real(r, g, b, a);
}
static void tr_DepthFunc(GLenum f)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glDepthFunc");
    g_glt.depth_func = f;
    if (real) real(f);
}
static void tr_ClearDepth(GLclampd d)
{
    static void(APIENTRY * real)(GLclampd);
    if (!real) real = gl_real("glClearDepth");
    g_glt.clear_depth = d;
    if (real) real(d);
}
static void tr_StencilFunc(GLenum f, GLint ref, GLuint mask)
{
    static void(APIENTRY * real)(GLenum, GLint, GLuint);
    if (!real) real = gl_real("glStencilFunc");
    g_glt.st_func = f; g_glt.st_ref = ref; g_glt.st_mask = mask;
    if (real) real(f, ref, mask);
}
static void tr_StencilOp(GLenum sf, GLenum zf, GLenum zp)
{
    static void(APIENTRY * real)(GLenum, GLenum, GLenum);
    if (!real) real = gl_real("glStencilOp");
    g_glt.st_op[0] = sf; g_glt.st_op[1] = zf; g_glt.st_op[2] = zp;
    if (real) real(sf, zf, zp);
}
static void tr_StencilMask(GLuint mask)
{
    static void(APIENTRY * real)(GLuint);
    if (!real) real = gl_real("glStencilMask");
    g_glt.st_wmask = mask;
    if (real) real(mask);
}
static void tr_ClearStencil(GLint s)
{
    static void(APIENTRY * real)(GLint);
    if (!real) real = gl_real("glClearStencil");
    g_glt.st_clear = s;
    if (real) real(s);
}
static void tr_CullFace(GLenum m)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glCullFace");
    g_glt.cull_face = m;
    if (real) real(m);
}
static void tr_FrontFace(GLenum m)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glFrontFace");
    g_glt.front_face = m;
    if (real) real(m);
}
static void tr_PolygonOffset(GLfloat f, GLfloat u)
{
    static void(APIENTRY * real)(GLfloat, GLfloat);
    if (!real) real = gl_real("glPolygonOffset");
    g_glt.po[0] = f; g_glt.po[1] = u;
    if (real) real(f, u);
}
static void tr_Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    static void(APIENTRY * real)(GLint, GLint, GLsizei, GLsizei);
    if (!real) real = gl_real("glScissor");
    g_glt.sbox[0] = x; g_glt.sbox[1] = y; g_glt.sbox[2] = w; g_glt.sbox[3] = h;
    if (real) real(x, y, w, h);
}

void *gl_trace_intercept(const char *name)
{
    void *gs = gmv_game_gl_draw_intercept(name);
    if (gs)
        return gs;
    static const struct { const char *n; void *f; } t[] = {
        {"_glClearColor", (void *)tr_ClearColor},
        {"_glClear", (void *)tr_Clear},
        {"_glViewport", (void *)tr_Viewport},
        {"_glDrawArrays", (void *)tr_DrawArrays},
        {"_glDrawElements", (void *)tr_DrawElements},
        {"_glBindFramebuffer", (void *)tr_BindFramebuffer},
        {"_glBindFramebufferEXT", (void *)tr_BindFramebuffer},
        {"_glDeleteFramebuffers", (void *)tr_DeleteFramebuffers},
        {"_glDeleteFramebuffersEXT", (void *)tr_DeleteFramebuffers},
        {"_glBufferData", (void *)tr_BufferData},
        {"_glBufferDataARB", (void *)tr_BufferData},
        {"_glBindTexture", (void *)tr_BindTexture},
        {"_glActiveTexture", (void *)tr_ActiveTexture},
        {"_glActiveTextureARB", (void *)tr_ActiveTexture},
        {"_glClientActiveTexture", (void *)tr_ClientActiveTexture},
        {"_glClientActiveTextureARB", (void *)tr_ClientActiveTexture},
        {"_glEnable", (void *)tr_Enable},
        {"_glDisable", (void *)tr_Disable},
        {"_glEnableClientState", (void *)tr_EnableClientState},
        {"_glDisableClientState", (void *)tr_DisableClientState},
        {"_glTexEnvi", (void *)tr_TexEnvi},
        {"_glTexEnvf", (void *)tr_TexEnvf},
        {"_glTexEnviv", (void *)tr_TexEnviv},
        {"_glTexEnvfv", (void *)tr_TexEnvfv},
        {"_glTexImage2D", (void *)tr_TexImage2D},
        {"_glCompressedTexImage2D", (void *)tr_CompressedTexImage2D},
        {"_glDeleteTextures", (void *)tr_DeleteTextures},
        {"_glColorMask", (void *)tr_ColorMask},
        {"_glDepthFunc", (void *)tr_DepthFunc},
        {"_glClearDepth", (void *)tr_ClearDepth},
        {"_glStencilFunc", (void *)tr_StencilFunc},
        {"_glStencilOp", (void *)tr_StencilOp},
        {"_glStencilMask", (void *)tr_StencilMask},
        {"_glClearStencil", (void *)tr_ClearStencil},
        {"_glCullFace", (void *)tr_CullFace},
        {"_glFrontFace", (void *)tr_FrontFace},
        {"_glPolygonOffset", (void *)tr_PolygonOffset},
        {"_glScissor", (void *)tr_Scissor},
        {NULL, NULL},
    };
    for (int i = 0; t[i].n; i++)
        if (!strcmp(t[i].n, name))
            return t[i].f;
    return NULL;
}

#if GMV_DEV
static long long tex_bytes(GLsizei w, GLsizei h, GLenum format, GLenum type)
{
    int bpp;
    switch (type) {
    case 0x8363: case 0x8033: case 0x8034: bpp = 2; break;
    case GL_FLOAT: bpp = 16; break;
    default:
        switch (format) {
        case GL_RGBA: case 0x80E1: bpp = 4; break;
        case GL_RGB: case GL_BGR_EXT: bpp = 3; break;
        case GL_LUMINANCE_ALPHA: bpp = 2; break;
        default: bpp = 1; break;
        }
    }
    return (long long)w * h * bpp;
}

static void pf_done(int kind, long long t0, long long bytes, const char *what, GLsizei w, GLsizei h,
                    GLenum fmt, void *caller)
{
    (void)kind;
    gmv_prof_add(kind, t0, bytes);
    static int logged;
    static LARGE_INTEGER freq;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    double ms = (double)(gmv_prof_now() - t0) * 1000.0 / (double)freq.QuadPart;
    if (ms >= 10.0 && logged < 40) {
        logged++;
        gmv_log("[loadprof] slow %s %dx%d fmt 0x%x %lld KB: %.1f ms (caller %p)\n", what, w, h, fmt,
                bytes >> 10, ms, caller);
    }
}

static void pf_TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                          GLenum format, GLenum type, const void *px)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                 const void *);
    if (!real) real = gl_real("glTexImage2D");
    gmv_gl_tex_size(target, level, w, h);
    long long t0 = gmv_prof_now();
    if (real) real(target, level, ifmt, w, h, border, format, type, px);
    pf_done(GMV_PROF_TEX, t0, px ? tex_bytes(w, h, format, type) : 0, "glTexImage2D", w, h,
            (GLenum)ifmt, __builtin_return_address(0));
}
static void pf_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                             GLenum format, GLenum type, const void *px)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                 const void *);
    if (!real) real = gl_real("glTexSubImage2D");
    long long t0 = gmv_prof_now();
    if (real) real(target, level, x, y, w, h, format, type, px);
    pf_done(GMV_PROF_TEX, t0, tex_bytes(w, h, format, type), "glTexSubImage2D", w, h, format,
            __builtin_return_address(0));
}
void pf_CompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLsizei size, const void *data)
{
    static void(APIENTRY * real)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
    if (!real) real = gl_real("glCompressedTexImage2D");
    gmv_gl_tex_size(target, level, w, h);
    long long t0 = gmv_prof_now();
    if (real) real(target, level, ifmt, w, h, border, size, data);
    pf_done(GMV_PROF_TEX, t0, size, "glCompressedTexImage2D", w, h, ifmt, __builtin_return_address(0));
}
static void pf_CompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                       GLsizei h, GLenum format, GLsizei size, const void *data)
{
    static void(APIENTRY * real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei,
                                 const void *);
    if (!real) real = gl_real("glCompressedTexSubImage2D");
    long long t0 = gmv_prof_now();
    if (real) real(target, level, x, y, w, h, format, size, data);
    pf_done(GMV_PROF_TEX, t0, size, "glCompressedTexSubImage2D", w, h, format,
            __builtin_return_address(0));
}
static void pf_GenerateMipmap(GLenum target)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) real = gl_real("glGenerateMipmapEXT");
    long long t0 = gmv_prof_now();
    if (real) real(target);
    pf_done(GMV_PROF_TEX, t0, 0, "glGenerateMipmapEXT", 0, 0, 0, __builtin_return_address(0));
}
static void pf_BufferData(GLenum target, intptr_t size, const void *data, GLenum usage)
{
    long long t0 = gmv_prof_now();
    tr_BufferData(target, size, data, usage);
    pf_done(GMV_PROF_BUF, t0, data ? size : 0, "glBufferData", 0, 0, 0, __builtin_return_address(0));
}
static void pf_BufferSubData(GLenum target, intptr_t off, intptr_t size, const void *data)
{
    static void(APIENTRY * real)(GLenum, intptr_t, intptr_t, const void *);
    if (!real) real = gl_real("glBufferSubData");
    long long t0 = gmv_prof_now();
    if (real) real(target, off, size, data);
    pf_done(GMV_PROF_BUF, t0, size, "glBufferSubData", 0, 0, 0, __builtin_return_address(0));
}

volatile unsigned gmv_glq_host;
static unsigned q_err, q_flt, q_int;
static long long q_err_t, q_flt_t;

static GLenum pf_GetError(void)
{
    static GLenum(APIENTRY * real)(void);
    if (!real) real = gl_real("glGetError");
    long long t0 = gmv_qpc();
    GLenum e = real ? real() : 0;
    q_err_t += gmv_qpc() - t0;
    q_err++;
    return e;
}

static void pf_GetFloatv(GLenum pname, GLfloat *v)
{
    static void(APIENTRY * real)(GLenum, GLfloat *);
    if (!real) real = gl_real("glGetFloatv");
    long long t0 = gmv_qpc();
    if (real) real(pname, v);
    q_flt_t += gmv_qpc() - t0;
    q_flt++;
}

void gmv_gl_query_note_int(void)
{
    q_int++;
}

static struct { const char *file; int line; unsigned n; long long t; } q_site[128];
static long long q_host_t;

static void q_site_add(const char *file, int line, long long dt)
{
    gmv_glq_host++;
    q_host_t += dt;
    unsigned h = ((unsigned)(uintptr_t)file * 31u + (unsigned)line) & 127;
    for (unsigned i = 0; i < 128; i++, h = (h + 1) & 127) {
        if (q_site[h].file == file && q_site[h].line == line) {
            q_site[h].n++;
            q_site[h].t += dt;
            return;
        }
        if (!q_site[h].file) {
            q_site[h].file = file;
            q_site[h].line = line;
            q_site[h].n = 1;
            q_site[h].t = dt;
            return;
        }
    }
}

#define Q_TIMED(call)                                                                                                  \
    long long t0 = gmv_qpc();                                                                                            \
    call;                                                                                                              \
    q_site_add(file, line, gmv_qpc() - t0)
void gmv_q_GetIntegerv(const char *file, int line, GLenum p, GLint *v) { Q_TIMED(glGetIntegerv(p, v)); }
void gmv_q_GetFloatv(const char *file, int line, GLenum p, GLfloat *v) { Q_TIMED(glGetFloatv(p, v)); }
void gmv_q_GetBooleanv(const char *file, int line, GLenum p, GLboolean *v) { Q_TIMED(glGetBooleanv(p, v)); }
void gmv_q_GetPointerv(const char *file, int line, GLenum p, void **v) { Q_TIMED(glGetPointerv(p, v)); }
GLboolean gmv_q_IsEnabled(const char *file, int line, GLenum c)
{
    GLboolean r;
    Q_TIMED(r = glIsEnabled(c));
    return r;
}

void gmv_gl_query_report(double seconds)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double ms = 1000.0 / (double)f.QuadPart;
    gmv_logfile("[perf]   GL state queries: the game's glGetError %.0f a second (%.2f ms), glGetFloatv %.0f (%.2f ms), "
                "glGetIntegerv %.0f; the host's own %.0f a second (%.2f ms)\n",
                q_err / seconds, q_err_t * ms / seconds, q_flt / seconds, q_flt_t * ms / seconds, q_int / seconds,
                gmv_glq_host / seconds, q_host_t * ms / seconds);
    q_host_t = 0;
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < 128; i++)
            if (q_site[i].n && (best < 0 || q_site[i].n > q_site[best].n))
                best = i;
        if (best < 0)
            break;
        const char *base = strrchr(q_site[best].file, '/');
        gmv_logfile("[perf]     %.0f a second (%.2f ms): %s:%d\n", q_site[best].n / seconds, q_site[best].t * ms / seconds,
                    base ? base + 1 : q_site[best].file, q_site[best].line);
        q_site[best].t = 0;
        q_site[best].n = 0;
    }
    for (int i = 0; i < 128; i++)
        q_site[i].n = 0, q_site[i].t = 0;
    q_err = q_flt = q_int = 0;
    q_err_t = q_flt_t = 0;
    gmv_glq_host = 0;
}

void *gl_prof_intercept(const char *name)
{
    static const struct { const char *n; void *f; } t[] = {
        {"_glGetError", (void *)pf_GetError},
        {"_glGetFloatv", (void *)pf_GetFloatv},
        {"_glTexImage2D", (void *)pf_TexImage2D},
        {"_glTexSubImage2D", (void *)pf_TexSubImage2D},
        {"_glCompressedTexImage2D", (void *)pf_CompressedTexImage2D},
        {"_glCompressedTexSubImage2D", (void *)pf_CompressedTexSubImage2D},
        {"_glGenerateMipmapEXT", (void *)pf_GenerateMipmap},
        {"_glBufferData", (void *)pf_BufferData},
        {"_glBufferSubData", (void *)pf_BufferSubData},
        {NULL, NULL},
    };
    for (int i = 0; t[i].n; i++)
        if (!strcmp(t[i].n, name))
            return t[i].f;
    return NULL;
}
#endif
