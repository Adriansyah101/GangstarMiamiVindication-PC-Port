#include <math.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

float g_vr_strength = 2.0f;
#if GMV_DEV
int g_vr_debug;
#endif
#define VR_QUEUE 256

typedef struct {
    GLint vp[4], sbox[4];
    GLfloat dr[2], po[2];
    GLenum cull_face, front_face;
    int cull, poly, scissor;
} vr_state_t;

static struct {
    unsigned frame;
    unsigned bodies;
    int ready;
    GLuint p_mark, p_pass, fbo, t_n, t_d;
    GLint u_mv, u_mvp, u_nm;
    int w, h;
    struct {
        mark_t d;
        vr_state_t st;
        GLfloat mv[16], mvp[16], nm[9];
    } q[VR_QUEUE];
} vr;

int vr_frame(void)
{
    return vr.frame == g_frame_no + 1;
}

static const char vr_vs_mark[] =
    "#version 120\n"
    "uniform mat4 mv, mvp;\n"
    "uniform mat3 nm;\n"
    "varying vec3 n, e;\n"
    "void main() {\n"
    "    n = nm * gl_Normal;\n"
    "    e = (mv * gl_Vertex).xyz;\n"
    "    gl_Position = mvp * gl_Vertex;\n"
    "}\n";

static const char vr_fs_mark[] =
    "#version 120\n"
    "varying vec3 n, e;\n"
    "void main() {\n"
    "    vec3 m = normalize(n), g = cross(dFdx(e), dFdy(e));\n"
    "    if (dot(g, e) > 0.0) g = -g;\n"
    "    float a = 1.0;\n"
    "    if (dot(m, g) < 0.0) { m = -m; a = 0.667; }\n"
    "    gl_FragColor = vec4(m * 0.5 + 0.5, a);\n"
    "}\n";

static const char vr_fs[] =
    "#version 120\n"
    "uniform sampler2D scn, dep, nrm, cdep;\n"
    "uniform vec2 full, dr;\n"
    "uniform vec4 vp, fogp;\n"
    "uniform mat4 proj, invp;\n"
    "uniform float str, wmin, reach, dbg;\n"
    "float dat(vec2 px) { return texture2D(dep, px / full).r; }\n"
    "vec3 sphere_scr(vec3 r, float b) {\n"
    "    vec3 d = normalize(r);\n"
    "    vec2 u = 0.5 + d.xy / (2.0 * sqrt(max(2.0 + 2.0 * d.z * sign(proj[2][3]), 1e-4)));\n"
    "    vec2 o = vec2(b * vp.w / vp.z, b);\n"
    "    vec3 c = texture2D(scn, (vp.xy + u * vp.zw) / full).rgb * 0.4;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u + vec2(o.x, 0.0), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u - vec2(o.x, 0.0), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u + vec2(0.0, o.y), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    c += texture2D(scn, (vp.xy + clamp(u - vec2(0.0, o.y), 0.0, 1.0) * vp.zw) / full).rgb * 0.15;\n"
    "    return c;\n"
    "}\n"
    "vec3 unproj(vec2 px, float z) {\n"
    "    vec4 p = invp * vec4((px - vp.xy) / vp.zw * 2.0 - 1.0, z, 1.0);\n"
    "    return p.xyz / p.w;\n"
    "}\n"
    "float wof(vec3 p) { return proj[0][3] * p.x + proj[1][3] * p.y + proj[2][3] * p.z + proj[3][3]; }\n"
    "vec2 screen(vec4 h) { return (h.xy / h.w * 0.5 + 0.5) * vp.zw + vp.xy; }\n"
    "float scene_w(vec2 px) {\n"
    "    float d = dat(px);\n"
    "    if (d < dr.x || d >= dr.y - 1e-5) return 1e30;\n"
    "    return wof(unproj(px, (d - dr.x) / (dr.y - dr.x) * 2.0 - 1.0));\n"
    "}\n"
    "void drop(vec3 c) {\n"
    "    if (dbg < 0.5) discard;\n"
    "    gl_FragColor = vec4(c, 0.6);\n"
    "}\n"
    "bool self_at(vec2 px, float sw, vec3 pc) {\n"
    "    if (texture2D(nrm, px / full).a < 0.5) return false;\n"
    "    vec3 q = unproj(px, (texture2D(cdep, px / full).r - dr.x) / (dr.y - dr.x) * 2.0 - 1.0);\n"
    "    return abs(wof(q) - sw) < 0.005 * sw + 5.0 && distance(q, pc) < 600.0;\n"
    "}\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n"
    "    vec4 nd = texture2D(nrm, px / full);\n"
    "    if (nd.a < 0.5) discard;\n"
    "    float cd = texture2D(cdep, px / full).r;\n"
    "    if (cd < dr.x || cd >= dr.y - 1e-5) discard;\n"
    "    vec3 pc = unproj(px, (cd - dr.x) / (dr.y - dr.x) * 2.0 - 1.0);\n"
    "    float wc = wof(pc);\n"
    "    if (scene_w(px) < wc * 0.995 - 5.0) {\n"
    "        drop(vec3(1.0, 0.0, 0.0));\n"
    "        return;\n"
    "    }\n"
    "    float clear = 1.0;\n"
    "    if (fogp.z == 2.0) {\n"
    "        float s = clamp((length(pc) - fogp.x) * fogp.y, 0.0, 1.0), s8 = s * s;\n"
    "        s8 *= s8; s8 *= s8;\n"
    "        clear = (fogp.w > 1e-3 ? (exp(-fogp.w * s * s) - exp(-fogp.w)) / (1.0 - exp(-fogp.w)) : 1.0 - s * s) * (1.0 - s8);\n"
    "    } else if (fogp.z == 1.0)\n"
    "        clear = 1.0 - clamp((wc - fogp.x) * fogp.y, 0.0, 1.0);\n"
    "    if (clear <= 0.0) {\n"
    "        drop(vec3(0.5));\n"
    "        return;\n"
    "    }\n"
    "    vec3 v = normalize(pc);\n"
    "    vec3 n = normalize(nd.xyz * 2.0 - 1.0);\n"
    "    float vn = dot(v, n);\n"
    "    float rim = smoothstep(0.03, 0.25, -vn);\n"
    "    if (rim <= 0.0) {\n"
    "        drop(vec3(0.0, 0.0, 1.0));\n"
    "        return;\n"
    "    }\n"
    "    if (vn > -0.05) n = normalize(n - v * (vn + 0.05));\n"
    "    vec3 r = reflect(v, n);\n"
    "    float F = (0.08 + 0.92 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0)) * rim;\n"
    "    vec3 p0 = pc + n * (0.004 * wc + 2.0);\n"
    "    float w0 = wof(p0);\n"
    "    vec3 p1 = p0 + r * reach;\n"
    "    float w1 = wof(p1);\n"
    "    if (w1 < wmin) p1 = p0 + (p1 - p0) * max((w0 - wmin) / (w0 - w1), 0.0);\n"
    "    vec4 h0 = proj * vec4(p0, 1.0), h1 = proj * vec4(p1, 1.0);\n"
    "    float k0 = 1.0 / h0.w, k1 = 1.0 / h1.w;\n"
    "    vec2 s0 = screen(h0), ds = screen(h1) - s0;\n"
    "    float amax = 1.0;\n"
    "    if (ds.x > 1e-3) amax = min(amax, (full.x - 0.5 - s0.x) / ds.x);\n"
    "    else if (ds.x < -1e-3) amax = min(amax, (0.5 - s0.x) / ds.x);\n"
    "    if (ds.y > 1e-3) amax = min(amax, (full.y - 0.5 - s0.y) / ds.y);\n"
    "    else if (ds.y < -1e-3) amax = min(amax, (0.5 - s0.y) / ds.y);\n"
    "    amax = max(amax, 0.0);\n"
    "    float noise = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));\n"
    "    float ap = 0.0, rwp = w0, hit = -1.0;\n"
    "    bool past = false, through = false;\n"
    "    for (int i = 1; i <= 48; i++) {\n"
    "        float a = amax * pow((float(i) - noise) / 48.0, 1.6);\n"
    "        float rw = 1.0 / mix(k0, k1, a), sw = scene_w(s0 + ds * a);\n"
    "        if (rw > sw && rw - sw < max(0.03 * sw + 50.0, 2.0 * abs(rw - rwp))) {\n"
    "            if (self_at(s0 + ds * a, sw, pc)) {\n"
    "                ap = a;\n"
    "                rwp = rw;\n"
    "                past = through = true;\n"
    "                continue;\n"
    "            }\n"
    "            float lo = ap;\n"
    "            hit = a;\n"
    "            if (!past) for (int j = 0; j < 5; j++) {\n"
    "                float m = 0.5 * (lo + hit);\n"
    "                if (1.0 / mix(k0, k1, m) > scene_w(s0 + ds * m)) hit = m; else lo = m;\n"
    "            }\n"
    "            break;\n"
    "        }\n"
    "        ap = a;\n"
    "        rwp = rw;\n"
    "        past = false;\n"
    "    }\n"
    "    if (dbg > 0.5) {\n"
    "        drop(nd.a < 0.84 ? vec3(1.0, 0.0, 1.0) : hit < 0.0 ? vec3(1.0, 1.0, 0.0) : through ? vec3(0.0, 1.0, 1.0) : vec3(0.0, 1.0, 0.0));\n"
    "        return;\n"
    "    }\n"
    "    if (hit < 0.0) {\n"
    "        float back = smoothstep(-0.95, -0.4, normalize(r).z * sign(proj[2][3]));\n"
    "        gl_FragColor = vec4(sphere_scr(r, 0.015), min(F * str, 0.85) * clear * back);\n"
    "        return;\n"
    "    }\n"
    "    vec2 s = s0 + ds * hit;\n"
    "    float e = clamp(min(min(s.x, full.x - s.x), min(s.y, full.y - s.y)) / (0.06 * full.y), 0.0, 1.0);\n"
    "    gl_FragColor = vec4(texture2D(scn, s / full).rgb, min(F * str, 0.85) * clear * e);\n"
    "}\n";

static int vr_init(void)
{
    if (!ao_procs("vehicle reflections"))
        return 0;
    vr.p_mark = ao_program("vehicle reflections' marking", vr_vs_mark, vr_fs_mark);
    vr.p_pass = vr.p_mark ? ao_program("vehicle reflections", ao_vs, vr_fs) : 0;
    if (!vr.p_pass) {
        gmv_log("[vr] vehicle reflections off\n");
        return 0;
    }
    vr.u_mv = aof.GetUniformLocation(vr.p_mark, "mv");
    vr.u_mvp = aof.GetUniformLocation(vr.p_mark, "mvp");
    vr.u_nm = aof.GetUniformLocation(vr.p_mark, "nm");
    glGenTextures(1, &vr.t_n);
    glGenTextures(1, &vr.t_d);
    msf.GenFramebuffers(1, &vr.fbo);
    return 1;
}

static int vr_alloc(int w, int h)
{
    vr.w = w;
    vr.h = h;
    ao_tex(vr.t_n, 0x8059, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    ao_tex(vr.t_d, 0x81A6, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);
    glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE);
    msf.BindFramebuffer(MS_FRAMEBUFFER, vr.fbo);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, vr.t_n, 0);
    aof.FramebufferTexture2D(MS_FRAMEBUFFER, 0x8D00, GL_TEXTURE_2D, vr.t_d, 0);
    GLenum st = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    if (st != 0x8CD5) {
        gmv_log("[vr] framebuffer incomplete (0x%x) at %dx%d; vehicle reflections off\n", st, w, h);
        return 0;
    }
    return 1;
}

int shine_body(GLenum mode, GLsizei count)
{
    if (g_glt.last_fb || g_sky_draw || mode < GL_TRIANGLES || count < 3 || g_blend_on || !g_ao_dmask || !g_ao_depth_on)
        return 0;
    unsigned on = g_tf_tex2d & (g_texfix ? g_tf_tcarr : 0xF) & 0xF;
    if ((on & 3) != 3 || g_te[1].mode != 0x8570 || g_te[1].crgb != GL_ADD || !gmv_gl_arr_on(GA_NORMAL))
        return 0;
    GLint tw1 = 0, th1 = 0;
    gmv_gl_tex_dims(g_tf_bound[1], &tw1, &th1);
    ao_proj_refresh();
    return !g_ao_persp ? 0 : tw1 <= 64 ? 2 : 1;
}

static void vr_normal(GLfloat *o, const GLfloat *m)
{
    for (int j = 0; j < 3; j++) {
        const GLfloat *a = m + (j + 1) % 3 * 4, *b = m + (j + 2) % 3 * 4;
        o[j * 3] = a[1] * b[2] - a[2] * b[1];
        o[j * 3 + 1] = a[2] * b[0] - a[0] * b[2];
        o[j * 3 + 2] = a[0] * b[1] - a[1] * b[0];
    }
    GLfloat d = m[0] * o[0] + m[1] * o[1] + m[2] * o[2];
    for (int k = 0; k < 9; k++)
        o[k] /= d;
}

void car_mark(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    int w = g_ms_w, h = g_ms_h;
    if (!g_ms_fbo)
        gmv_gl_client_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;
    if (!vr.ready)
        vr.ready = vr_init() ? 1 : -1;
    if (vr.ready < 0) {
        g_vr = 0;
        return;
    }
    if (w != vr.w || h != vr.h) {
        GLint dfb = gmv_gl_cur_fb(MS_DRAW_FB_BINDING), rfb = gmv_gl_cur_fb(MS_READ_FB_BINDING);
        glPushAttrib(GL_TEXTURE_BIT);
        int ok = vr_alloc(w, h);
        glPopAttrib();
        msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
        msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
        if (!ok) {
            g_vr = 0;
            return;
        }
    }
    if (!vr_frame()) {
        vr.frame = g_frame_no + 1;
        vr.bodies = 0;
    }
    if (vr.bodies == VR_QUEUE || !mark_take(&vr.q[vr.bodies].d, mode, count, type, idx, 1))
        return;
    const GLfloat *mv = ao_matrix(GL_MODELVIEW);
    memcpy(vr.q[vr.bodies].mv, mv, sizeof vr.q[vr.bodies].mv);
    ds_mul(vr.q[vr.bodies].mvp, ao_matrix(GL_PROJECTION), mv);
    vr_normal(vr.q[vr.bodies].nm, mv);
    vr.q[vr.bodies++].st = (vr_state_t){{g_glt.vp[0], g_glt.vp[1], g_glt.vp[2], g_glt.vp[3]},
                                        {g_glt.sbox[0], g_glt.sbox[1], g_glt.sbox[2], g_glt.sbox[3]},
                                        {g_ao_dr[0], g_ao_dr[1]}, {g_glt.po[0], g_glt.po[1]},
                                        g_glt.cull_face, g_glt.front_face, g_glt.cull, g_glt.poly_fill, g_glt.scissor};
}

static void vr_apply(const vr_state_t *s)
{
    glViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    glDepthRange(s->dr[0], s->dr[1]);
    if (s->cull)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    glCullFace(s->cull_face);
    glFrontFace(s->front_face);
    if (s->poly)
        glEnable(GL_POLYGON_OFFSET_FILL);
    else
        glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(s->po[0], s->po[1]);
    if (s->scissor)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    glScissor(s->sbox[0], s->sbox[1], s->sbox[2], s->sbox[3]);
}

static void vr_flush(void)
{
    msf.BindFramebuffer(MS_FRAMEBUFFER, vr.fbo);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    aof.UseProgram(vr.p_mark);
    mark_begin(1);
    for (unsigned i = 0; i < vr.bodies; i++) {
        if (!i || memcmp(&vr.q[i].st, &vr.q[i - 1].st, sizeof vr.q[i].st))
            vr_apply(&vr.q[i].st);
        aof.UniformMatrix4fv(vr.u_mv, 1, GL_FALSE, vr.q[i].mv);
        aof.UniformMatrix4fv(vr.u_mvp, 1, GL_FALSE, vr.q[i].mvp);
        aof.UniformMatrix3fv(vr.u_nm, 1, GL_FALSE, vr.q[i].nm);
        mark_draw(&vr.q[i].d, 1);
    }
    mark_end();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_SCISSOR_TEST);
}

void vr_pass(int w, int h, int li)
{
    const GLfloat *P = g_ao_layer[li].m, *dr = g_ao_layer[li].dr, *fogs = g_ao_layer[li].fog;
    const GLint *vp = g_ao_layer[li].vp;
    GLfloat invp[16], fogp[4];
    if (vr.w != w || vr.h != h || !ao_invert(P, invp) || vp[2] <= 0 || vp[3] <= 0 || !(dr[1] > dr[0]))
        return;
    if (!ssr_grab(w, h)) {
        g_vr = 0;
        return;
    }
    float a = P[11] > 0 ? -P[10] : P[10], nearp = fabsf(P[14] / (a - 1)), farp = fabsf(P[14] / (a + 1));
    ssr_fog(fogs, fogp);
    vr_flush();

    GLuint p = vr.p_pass;
    aof.UseProgram(p);
    aof.Uniform1i(aof.GetUniformLocation(p, "scn"), 0);
    aof.Uniform1i(aof.GetUniformLocation(p, "dep"), 1);
    aof.Uniform1i(aof.GetUniformLocation(p, "nrm"), 2);
    aof.Uniform1i(aof.GetUniformLocation(p, "cdep"), 3);
    aof.Uniform2f(aof.GetUniformLocation(p, "full"), (GLfloat)w, (GLfloat)h);
    aof.Uniform2f(aof.GetUniformLocation(p, "dr"), dr[0], dr[1]);
    aof.Uniform4f(aof.GetUniformLocation(p, "vp"), (GLfloat)vp[0], (GLfloat)vp[1], (GLfloat)vp[2], (GLfloat)vp[3]);
    aof.Uniform4f(aof.GetUniformLocation(p, "fogp"), fogp[0], fogp[1], fogp[2], fogp[3]);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "proj"), 1, GL_FALSE, P);
    aof.UniformMatrix4fv(aof.GetUniformLocation(p, "invp"), 1, GL_FALSE, invp);
    aof.Uniform1f(aof.GetUniformLocation(p, "str"), g_vr_strength);
    aof.Uniform1f(aof.GetUniformLocation(p, "dbg"), (GLfloat)g_vr_debug);
    aof.Uniform1f(aof.GetUniformLocation(p, "wmin"), nearp * 1.5f);
    aof.Uniform1f(aof.GetUniformLocation(p, "reach"), farp * 2);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);
    glViewport(0, 0, w, h);
    aof.ActiveTexture(0x84C3);
    glBindTexture(GL_TEXTURE_2D, vr.t_d);
    aof.ActiveTexture(0x84C2);
    glBindTexture(GL_TEXTURE_2D, vr.t_n);
    aof.ActiveTexture(0x84C1);
    glBindTexture(GL_TEXTURE_2D, ssr.t_dep);
    aof.ActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D, ssr.t_scene);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    aof.BlendEquation(0x8006);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    ao_tri();
    glDisable(GL_BLEND);

    static unsigned runs;
    if (runs++ % 1200 == 0)
        gmv_trace("[vr] pass %u at %dx%d: %u car bodies, strength %g, fog %s\n", runs, w, h, vr.bodies, g_vr_strength,
                fogp[2] == 2 ? "hybrid" : fogp[2] == 1 ? "linear" : "none");
}
