#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

#define DS_SIZE_HIGH 2048
#define DS_SIZE_LOW 1024
int g_shadow_quality = 2;
#define DS_RADIUS 6000.0f
#define DS_DEPTH 20000.0f
#define DS_AHEAD 0.6f
#define DS_BIAS 25.0f
#define DS_BIAS_WORLD 1.0f
#define DS_BIT 0x40
#define DS_EYE_NEAR 500.0f
#define DS_STRENGTH 0.45f
#define DS_MOON_STRENGTH 0.30f
#define DS_SKY_STRENGTH 0.40f
#define DS_SKY_SOFT 2.0f
#define DS_LOW 0.03f
#define DS_SUN_HIGH 0.20f
#define DS_MOON_HIGH 0.14f
#define DS_SEA_Z (-20.0f)
#define DS_QUEUE 512
extern float g_sun_dir[3];
extern float g_moon_dir[3];
extern float g_cam_world[3];

static struct {
    unsigned frame;
    unsigned tried;
    unsigned n[4];
    unsigned mask;
#if GMV_DEV
    int eye_said;
    unsigned eye_frame;
    unsigned eye_bad, eye_frames, eye_nomap, eye_nocam;
    unsigned eye_kind[4];
    unsigned eye_reach, eye_reach_frames, eye_reach_nomap, eye_reach_frame;
    float eye_min;
    DWORD eye_t;
#endif
    int ready;
    int size;
    GLuint p_map, p_pass[2], fbo, t;
    GLint u_lvp;
    GLfloat lvp[16];
    GLfloat iv[16];
    GLfloat leye[3];
    GLfloat strength, soft;
    GLfloat dir[3];
    const char *source;
    unsigned nq;
    struct {
        mark_t d;
        GLfloat m[16];
    } q[DS_QUEUE];
} ds;

int ds_frame(void)
{
    return ds.frame == g_frame_no + 1;
}

int ds_kind(GLenum mode, GLsizei count, int body)
{
    if (!g_ds_want || !g_ds)
        return 0;
    if (body)
        return body;
    if (g_glt.last_fb || g_sky_draw || mode < GL_TRIANGLES || count < 3 || g_blend_on || !g_ao_dmask || !g_ao_depth_on ||
        !gmv_ds_caster())
        return 0;
    ao_proj_refresh();
    return g_ao_persp ? 3 : 0;
}

static const char ds_vs_map[] =
    "#version 120\n"
    "uniform mat4 lvp;\n"
    "void main() { gl_Position = lvp * gl_Vertex; }\n";

static const char ds_fs_map[] =
    "#version 120\n"
    "void main() { gl_FragColor = vec4(1.0); }\n";

static const char ds_fs[] =
    "uniform sampler2D dep;\n"
    "uniform sampler2DShadow smap;\n"
    "uniform vec2 full, dr;\n"
    "uniform vec4 vp, fogp, wz, leye;\n"
    "uniform mat4 proj, invp, lvp;\n"
    "uniform float str, texel, soft, tw, zs, bias, sl, seaz;\n"
    "vec3 pos(vec2 px) {\n"
    "    float d = texture2D(dep, px / full).r;\n"
    "    vec4 p = invp * vec4((px - vp.xy) / vp.zw * 2.0 - 1.0, (d - dr.x) / (dr.y - dr.x) * 2.0 - 1.0, 1.0);\n"
    "    return p.xyz / p.w;\n"
    "}\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    float d = texture2D(dep, px / full).r;\n"
    "    if (d < dr.x || d >= dr.y - 1e-5) discard;\n"
    "    vec3 pc = pos(px);\n"
    "    if (dot(wz, vec4(pc, 1.0)) < seaz) discard;\n"
    "    vec3 q = (lvp * vec4(pc, 1.0)).xyz;\n"
    "    float e = max(abs(q.x), abs(q.y));\n"
    "    if (e >= 1.0 || abs(q.z) >= 1.0) discard;\n"
    "    vec3 xa = pos(px + vec2(1.0, 0.0)) - pc, xb = pc - pos(px - vec2(1.0, 0.0));\n"
    "    vec3 ya = pos(px + vec2(0.0, 1.0)) - pc, yb = pc - pos(px - vec2(0.0, 1.0));\n"
    "    vec3 n = normalize(cross(dot(xa, xa) < dot(xb, xb) ? xa : xb, dot(ya, ya) < dot(yb, yb) ? ya : yb));\n"
    "    if (dot(n, pc) > 0.0) n = -n;\n"
    "    float nl = dot(n, leye.xyz);\n"
    "    float face = smoothstep(0.02, 0.2, nl);\n"
    "    if (face <= 0.0) discard;\n"
    "    float clear = 1.0;\n"
    "    if (fogp.z == 2.0) {\n"
    "        float s = clamp((length(pc) - fogp.x) * fogp.y, 0.0, 1.0), s8 = s * s;\n"
    "        s8 *= s8; s8 *= s8;\n"
    "        clear = (fogp.w > 1e-3 ? (exp(-fogp.w * s * s) - exp(-fogp.w)) / (1.0 - exp(-fogp.w)) : 1.0 - s * s) * (1.0 - s8);\n"
    "    } else if (fogp.z == 1.0) {\n"
    "        float wc = proj[0][3] * pc.x + proj[1][3] * pc.y + proj[2][3] * pc.z + proj[3][3];\n"
    "        clear = 1.0 - clamp((wc - fogp.x) * fogp.y, 0.0, 1.0);\n"
    "    }\n"
    "    if (clear <= 0.0) discard;\n"
    "    float slope = min(sqrt(max(1.0 - nl * nl, 0.0)) / max(nl, 0.05), 4.0);\n"
    "    vec3 uvz = q * 0.5 + 0.5;\n"
    "    uvz.z -= (bias + sl * tw * slope) * zs;\n"
    "    float lit = 0.0;\n"
    "    float st = texel * soft;\n"
    "#if TAPS == 16\n"
    "    for (float j = -1.5; j < 2.0; j += 1.0)\n"
    "        for (float i = -1.5; i < 2.0; i += 1.0)\n"
    "            lit += shadow2D(smap, vec3(uvz.xy + vec2(i, j) * st, uvz.z)).r;\n"
    "#else\n"
    "    for (float j = -1.5; j < 2.0; j += 2.0) {\n"
    "        lit += shadow2D(smap, vec3(uvz.xy + vec2(-1.5, j) * st, uvz.z)).r;\n"
    "        lit += shadow2D(smap, vec3(uvz.xy + vec2(0.5, j) * st, uvz.z)).r;\n"
    "        lit += shadow2D(smap, vec3(uvz.xy + vec2(-0.5, j + 1.0) * st, uvz.z)).r;\n"
    "        lit += shadow2D(smap, vec3(uvz.xy + vec2(1.5, j + 1.0) * st, uvz.z)).r;\n"
    "    }\n"
    "#endif\n"
    "    float k = (1.0 - lit / float(TAPS)) * face * clear * (1.0 - smoothstep(0.85, 1.0, e)) * str;\n"
    "    if (k <= 0.002) discard;\n"
    "    gl_FragColor = vec4(vec3(1.0 - k), 1.0);\n"
    "}\n";

static char ds_fs8[sizeof ds_fs + 32], ds_fs16[sizeof ds_fs + 32];

static int ds_alloc(int size)
{
    GLint dfb = 0, rfb = 0;
    dfb = gmv_gl_cur_fb(MS_DRAW_FB_BINDING);
    rfb = gmv_gl_cur_fb(MS_READ_FB_BINDING);
    glPushAttrib(GL_TEXTURE_BIT | GL_COLOR_BUFFER_BIT | GL_PIXEL_MODE_BIT);
    ao_tex(ds.t, 0x81A6, size, size, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, 0x884C, 0x884E);
    glTexParameteri(GL_TEXTURE_2D, 0x884D, GL_LEQUAL);
    msf.BindFramebuffer(MS_FRAMEBUFFER, ds.fbo);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8D00, GL_TEXTURE_2D, ds.t, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
    glPopAttrib();
    if (st != 0x8CD5) {
        gmv_log("[shadow] framebuffer incomplete (0x%x) at %dx%d; dynamic shadows off\n", st, size, size);
        return 0;
    }
    ds.size = size;
    return 1;
}

static int ds_init(void)
{
    if (!ao_procs("dynamic shadows"))
        return 0;
    snprintf(ds_fs8, sizeof ds_fs8, "#version 120\n#define TAPS 8\n%s", ds_fs);
    snprintf(ds_fs16, sizeof ds_fs16, "#version 120\n#define TAPS 16\n%s", ds_fs);
    ds.p_map = ao_program("dynamic shadows' map", ds_vs_map, ds_fs_map);
    ds.p_pass[0] = ds.p_map ? ao_program("dynamic shadows, 8 taps", ao_vs, ds_fs8) : 0;
    ds.p_pass[1] = ds.p_pass[0] ? ao_program("dynamic shadows", ao_vs, ds_fs16) : 0;
    if (!ds.p_pass[1]) {
        gmv_log("[shadow] dynamic shadows off\n");
        return 0;
    }
    ds.u_lvp = aof.GetUniformLocation(ds.p_map, "lvp");
    glGenTextures(1, &ds.t);
    msf.GenFramebuffers(1, &ds.fbo);
    return 1;
}

static int ds_size_sync(void)
{
    int q = g_shadow_quality ? g_shadow_quality : gmv_gpu_integrated() ? 1 : 2;
    int want = q == 1 ? DS_SIZE_LOW : DS_SIZE_HIGH;
    if (want == ds.size)
        return 1;
    int was = ds.size;
    if (!ds_alloc(want))
        return 0;
    gmv_log("[shadow] map %dx%d, %d taps (SHADOW_QUALITY=%d%s)%s\n", want, want, want == DS_SIZE_LOW ? 8 : 16,
            q == 1 ? 1 : 2, g_shadow_quality ? "" : gmv_gpu_integrated() ? ", auto: a built-in GPU" : ", auto: a graphics card",
            was ? ", from the options" : "");
    return 1;
}

void ds_mul(GLfloat *o, const GLfloat *a, const GLfloat *b)
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
}

static int ds_light(void)
{
    const float *m = g_sun_dir, *d = NULL;
    float len = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), high = DS_SUN_HIGH, full = DS_STRENGTH;
    if (len < 1e-3f)
        return 0;
    if (m[2] > DS_LOW * len) {
        d = m;
        ds.source = "sun";
    } else {
        m = g_moon_dir;
        len = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        high = DS_MOON_HIGH, full = DS_MOON_STRENGTH;
        if (len >= 1e-3f && m[2] > DS_LOW * len) {
            d = m;
            ds.source = "moon";
        }
    }
    float w = 0;
    if (d) {
        w = (d[2] / len - DS_LOW) / (high - DS_LOW);
        w = w > 1 ? 1 : w;
        w = w * w * (3 - 2 * w);
    } else {
        ds.source = "neither up";
    }
    float l[3] = {d ? w * d[0] / len : 0, d ? w * d[1] / len : 0, (d ? w * d[2] / len : 0) + 1 - w};
    float ll = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    for (int i = 0; i < 3; i++)
        ds.dir[i] = l[i] / ll;
    ds.strength = DS_SKY_STRENGTH + (full - DS_SKY_STRENGTH) * w;
    ds.soft = DS_SKY_SOFT + (1 - DS_SKY_SOFT) * w;
    return 1;
}

static int ds_eye_near(const GLfloat *iv)
{
    float ex = iv[12] - g_cam_world[0], ey = iv[13] - g_cam_world[1], ez = iv[14] - g_cam_world[2];
    return ex * ex + ey * ey + ez * ez <= DS_EYE_NEAR * DS_EYE_NEAR;
}

#if GMV_DEV
static void ds_eye_note(const float *abs, const GLfloat *iv, int kind)
{
    int nocam = !g_cam_world[0] && !g_cam_world[1] && !g_cam_world[2];
    ds.eye_bad++;
    ds.eye_nocam += (unsigned)nocam;
    if (ds.eye_frame != g_frame_no + 1) {
        ds.eye_frame = g_frame_no + 1;
        ds.eye_frames++;
    }
    ds.eye_kind[kind & 3]++;
    if (!nocam && !abs) {
        float d = sqrtf(iv[12] * iv[12] + iv[13] * iv[13] + iv[14] * iv[14]);
        if (!ds.eye_min || d < ds.eye_min)
            ds.eye_min = d;
        if (d < DS_RADIUS * (1.0f + DS_AHEAD)) {
            ds.eye_reach++;
            if (ds.eye_reach_frame != g_frame_no + 1) {
                ds.eye_reach_frame = g_frame_no + 1;
                ds.eye_reach_frames++;
            }
        }
    }
    if (!nocam && ds.eye_said < 3) {
        ds.eye_said++;
        gmv_log("[shadow] no view from this caster (kind %d, node %s), eye at %.0f %.0f %.0f, the camera is at %.0f %.0f %.0f\n",
                kind, abs ? "known" : "not in the table", iv[12], iv[13], iv[14], g_cam_world[0], g_cam_world[1], g_cam_world[2]);
    }
}

void ds_eye_frame_end(void)
{
    if (ds.eye_frame == g_frame_no + 1 && ds.tried != ds.eye_frame) {
        ds.eye_nomap++;
        ds.eye_reach_nomap += ds.eye_reach_frame == ds.eye_frame;
    }
    if (!ds.eye_bad || GetTickCount() - ds.eye_t < 60000)
        return;
    ds.eye_t = GetTickCount();
    gmv_log("[shadow] %u casters gave no view (%u with the camera not known yet; shiny %u, characters %u, parts %u), in %u frames, %u of those frames with no shadows; within the map's reach: %u casters in %u frames, %u of them frames with no shadows; the nearest %.0f m\n",
            ds.eye_bad, ds.eye_nocam, ds.eye_kind[1], ds.eye_kind[2], ds.eye_kind[3], ds.eye_frames, ds.eye_nomap,
            ds.eye_reach, ds.eye_reach_frames, ds.eye_reach_nomap, ds.eye_min / 100.0f);
    ds.eye_bad = ds.eye_nocam = ds.eye_frames = ds.eye_nomap = 0;
    ds.eye_reach = ds.eye_reach_frames = ds.eye_reach_nomap = 0;
    ds.eye_min = 0;
    memset(ds.eye_kind, 0, sizeof ds.eye_kind);
}
#endif

static int ds_begin(int kind)
{
    if (!ds_light())
        return 0;
    GLfloat v[16], iv[16], lw[16];
    memcpy(v, ao_matrix(GL_MODELVIEW), sizeof v);
    if (!ao_invert(v, iv))
        return -1;
    if (!ds_eye_near(iv)) {
        const float *abs = gmv_ds_caster();
        GLfloat ia[16], own[16];
        if (abs && ao_invert(abs, ia)) {
            memcpy(own, v, sizeof v);
            ds_mul(v, own, ia);
        }
        if (!abs || !ao_invert(v, iv) || !ds_eye_near(iv)) {
#if GMV_DEV
            ds_eye_note(abs, iv, kind);
#else
            (void)kind;
#endif
            return -1;
        }
    }
    const float *l = ds.dir;
    float u[3] = {-l[1], l[0], 0};
    float ul = sqrtf(u[0] * u[0] + u[1] * u[1]);
    if (ul < 1e-3f)
        u[0] = 1, u[1] = 0, ul = 1;
    u[0] /= ul, u[1] /= ul;
    float w[3] = {l[1] * u[2] - l[2] * u[1], l[2] * u[0] - l[0] * u[2], l[0] * u[1] - l[1] * u[0]};
    float fw = g_ao_proj[11] > 0 ? 1.0f : -1.0f;
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = iv[12 + i] + iv[8 + i] * fw * DS_RADIUS * DS_AHEAD;
    float tw = 2.0f * DS_RADIUS / (float)ds.size;
    float cu = floorf((u[0] * c[0] + u[1] * c[1] + u[2] * c[2]) / tw) * tw;
    float cw = floorf((w[0] * c[0] + w[1] * c[1] + w[2] * c[2]) / tw) * tw;
    float sx = 1.0f / DS_RADIUS, sz = 1.0f / DS_DEPTH;
    memset(lw, 0, sizeof lw);
    for (int i = 0; i < 3; i++) {
        lw[i * 4] = sx * u[i];
        lw[i * 4 + 1] = sx * w[i];
        lw[i * 4 + 2] = -sz * l[i];
    }
    lw[12] = -sx * cu;
    lw[13] = -sx * cw;
    lw[14] = sz * (l[0] * c[0] + l[1] * c[1] + l[2] * c[2]);
    lw[15] = 1;
    ds_mul(ds.lvp, lw, iv);
    memcpy(ds.iv, iv, sizeof iv);
    for (int i = 0; i < 3; i++)
        ds.leye[i] = v[i] * l[0] + v[4 + i] * l[1] + v[8 + i] * l[2];
    return 1;
}

void ds_mark(int kind, GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    if (!ds.ready)
        ds.ready = ds_init() ? 1 : -1;
    if (ds.ready < 0) {
        g_ds = 0;
        return;
    }
    if (ds.tried == g_frame_no + 1 && !ds_frame())
        return;
    if (ds.tried != g_frame_no + 1) {
        if (!ds_size_sync()) {
            ds.ready = -1;
            g_ds = 0;
            return;
        }
        int b = ds_begin(kind);
        if (b < 0)
            return;
        ds.tried = g_frame_no + 1;
        if (!b)
            return;
        ds.frame = g_frame_no + 1;
        ds.n[1] = ds.n[2] = ds.n[3] = 0;
        ds.nq = 0;
    }
    ds.n[kind]++;
    if (ds.nq == DS_QUEUE || !mark_take(&ds.q[ds.nq].d, mode, count, type, idx, 0))
        return;
    ds_mul(ds.q[ds.nq++].m, ds.lvp, ao_matrix(GL_MODELVIEW));
}

static void ds_flush(void)
{
    msf.BindFramebuffer(MS_FRAMEBUFFER, ds.fbo);
    glViewport(0, 0, ds.size, ds.size);
    glDepthRange(0, 1);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glClearDepth(1);
    glClear(GL_DEPTH_BUFFER_BIT);
    aof.UseProgram(ds.p_map);
    mark_begin(0);
    for (unsigned i = 0; i < ds.nq; i++) {
        aof.UniformMatrix4fv(ds.u_lvp, 1, GL_FALSE, ds.q[i].m);
        mark_draw(&ds.q[i].d, 0);
    }
    mark_end();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
}

int ds_mask_begin(int body)
{
    if (!body || !g_ds_want || !g_ds)
        return 0;
    int sc = 0;
    if (ds.mask != g_frame_no + 1) {
        ds.mask = g_frame_no + 1;
        sc = g_glt.scissor;
        if (sc)
            glDisable(GL_SCISSOR_TEST);
        glStencilMask(DS_BIT);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glClearStencil(g_glt.st_clear);
    }
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, DS_BIT, DS_BIT);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(DS_BIT);
    return 1 + sc;
}

void ds_mask_end(int on)
{
    if (!on)
        return;
    if (!g_glt.stencil)
        glDisable(GL_STENCIL_TEST);
    glStencilFunc(g_glt.st_func, g_glt.st_ref, g_glt.st_mask);
    glStencilOp(g_glt.st_op[0], g_glt.st_op[1], g_glt.st_op[2]);
    glStencilMask(g_glt.st_wmask);
    if (on == 2)
        glEnable(GL_SCISSOR_TEST);
}

void ds_pass(int w, int h, int li)
{
    const GLfloat *P = g_ao_layer[li].m, *dr = g_ao_layer[li].dr, *fogs = g_ao_layer[li].fog;
    const GLint *vp = g_ao_layer[li].vp;
    GLfloat invp[16], fogp[4];
    if (!ao_invert(P, invp) || vp[2] <= 0 || vp[3] <= 0 || !(dr[1] > dr[0]))
        return;
    if (ssr.bad || ((w != ssr.w || h != ssr.h) && !ssr_alloc(w, h))) {
        ssr.bad = 1;
        g_ds = 0;
        return;
    }
    GLuint dep = post_depth(w, h);
    if (!dep) {
        msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
        msf.BindFramebuffer(MS_DRAW_FB, ssr.fbo_depth);
        msf.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        dep = ssr.t_depth;
    }
    ssr_fog(fogs, fogp);
    ds_flush();

    GLuint p = ds.p_pass[ds.size != DS_SIZE_LOW];
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 0);
    aof.Uniform1i(aof.GetUniformLocation(p, "smap"), 1);
    aof.Uniform2f(aof.GetUniformLocation(p, "full"), (GLfloat)w, (GLfloat)h);
    aof.Uniform2f(aof.GetUniformLocation(p, "dr"), dr[0], dr[1]);
    aof.Uniform4f(aof.GetUniformLocation(p, "vp"), (GLfloat)vp[0], (GLfloat)vp[1], (GLfloat)vp[2], (GLfloat)vp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "fogp"), fogp[0], fogp[1], fogp[2], fogp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "wz"), ds.iv[2], ds.iv[6], ds.iv[10], ds.iv[14]);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "proj"), 1, GL_FALSE, P);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "invp"), 1, GL_FALSE, invp);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "lvp"), 1, GL_FALSE, ds.lvp);
    aof.Uniform4f(aof.GetUniformLocation(p, "leye"), ds.leye[0], ds.leye[1], ds.leye[2], 0);
    aof.Uniform1f(aof.GetUniformLocation(p, "str"), ds.strength);
    aof.Uniform1f(aof.GetUniformLocation(p, "texel"), 1.0f / (float)ds.size);
    aof.Uniform1f(aof.GetUniformLocation(p, "soft"), ds.soft);
    aof.Uniform1f(aof.GetUniformLocation(p, "tw"), 2.0f * DS_RADIUS / (float)ds.size);
    aof.Uniform1f(aof.GetUniformLocation(p, "zs"), 0.5f / DS_DEPTH);
    aof.Uniform1f(aof.GetUniformLocation(p, "seaz"), DS_SEA_Z);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);
    glViewport(0, 0, w, h);
    aof.ActiveTexture(0x84C1);
    glBindTexture(GL_TEXTURE_2D, ds.t);
    aof.ActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D, dep);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    aof.BlendEquation(0x8006);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    GLint bias = aof.GetUniformLocation(p, "bias"), sl = aof.GetUniformLocation(p, "sl");
    static GLint sbits = -1;
    if (sbits < 0)
        glGetIntegerv(GL_STENCIL_BITS, &sbits);
    if (sbits) {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_NOTEQUAL, DS_BIT, DS_BIT);
        aof.Uniform1f(bias, DS_BIAS_WORLD);
        aof.Uniform1f(sl, 0);
        ao_tri();
        glStencilFunc(GL_EQUAL, DS_BIT, DS_BIT);
    }
    aof.Uniform1f(bias, DS_BIAS);
    aof.Uniform1f(sl, 2);
    ao_tri();
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);

    static unsigned runs;
    if (runs++ % 1200 == 0)
        gmv_trace("[shadow] pass %u at %dx%d: %u shiny bodies, %u character draws, %u other parts, %s, light towards %.2f %.2f %.2f, hour %.2f, strength %.2f\n",
                runs, w, h, ds.n[1], ds.n[2], ds.n[3], ds.source, ds.dir[0], ds.dir[1], ds.dir[2], g_tod_ms / 3600000.0f, ds.strength);
}

int gmv_ds_active(void)
{
    return g_ds_want && g_ds;
}
