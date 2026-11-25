#include <math.h>
#include <stdio.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "render.h"

const char fh_fs_head[] =
    "#version 120\n"
    "uniform sampler2D t0, t1;\n"
    "uniform mat4 invp;\n"
    "uniform vec4 vp, fogp, hgt;\n"
    "uniform vec2 dr;\n"
    "float fog_clear() {\n"
    "    vec4 p = invp * vec4((gl_FragCoord.xy - vp.xy) / vp.zw * 2.0 - 1.0,\n"
    "                         (gl_FragCoord.z - dr.x) / (dr.y - dr.x) * 2.0 - 1.0, 1.0);\n"
    "    vec3 e = p.xyz / p.w;\n"
    "    float d = length(e);\n"
    "    if (d > fogp.z) discard;\n"
    "    float t = clamp((d - fogp.x) * fogp.y, 0.0, 1.0);\n"
    "    float a0 = max(hgt.w, 0.0), a1 = max(hgt.w + dot(e, hgt.xyz), 0.0);\n"
    "    float rho = abs(a1 - a0) < 1e-3 ? exp(-0.5 * (a0 + a1)) : (exp(-a0) - exp(-a1)) / (a1 - a0);\n"
    "    float a = fogp.w * rho;\n"
    "    float clear = a > 1e-3 ? (exp(-a * t * t) - exp(-a)) / (1.0 - exp(-a)) : 1.0 - t * t;\n"
    "    float t8 = t * t; t8 *= t8; t8 *= t8;\n"
    "    return clear * (1.0 - t8);\n"
    "}\n"
    "vec4 fogged(vec4 c) {\n"
    "    c.rgb = mix(gl_Fog.color.rgb, c.rgb, fog_clear());\n"
    "    return c;\n"
    "}\n";
const unsigned fh_fs_head_size = sizeof fh_fs_head;

static const char *const fh_fs_main[3] = {
    "void main() { gl_FragColor = fogged(gl_Color); }\n",
    "void main() { gl_FragColor = fogged(texture2DProj(t0, gl_TexCoord[0]) * gl_Color); }\n",
    "void main() {\n"
    "    vec4 c = texture2DProj(t0, gl_TexCoord[0]) * gl_Color, s = texture2DProj(t1, gl_TexCoord[1]);\n"
    "    gl_FragColor = fogged(vec4(min(c.rgb + s.rgb, 1.0), c.a * s.a));\n"
    "}\n",
};

fh_t fh;

float fh_amount(float d, float h0, float h1)
{
    float fs = g_ao_fogp[1], fe = g_ao_fogp[2];
    if (!(fe > fs))
        return d >= fe;
    float t = (d - fs) / (fe - fs);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float a0 = g_fog_height > 0 ? fmaxf(h0 / g_fog_height, 0) : 0, a1 = g_fog_height > 0 ? fmaxf(h1 / g_fog_height, 0) : 0;
    float rho = fabsf(a1 - a0) < 1e-3f ? expf(-0.5f * (a0 + a1)) : (expf(-a0) - expf(-a1)) / (a1 - a0);
    float a = FOG_HYBRID_K * rho;
    float clear = a > 1e-3f ? (expf(-a * t * t) - expf(-a)) / (1 - expf(-a)) : 1 - t * t;
    float t8 = t * t;
    t8 *= t8;
    t8 *= t8;
    return 1 - clear * (1 - t8);
}

static int fh_init(void)
{
    static const char *const what[3] = {"hybrid fog (colour)", "hybrid fog (texture)", "hybrid fog (texture + added layer)"};
    if (!ao_procs("the hybrid fog"))
        return 0;
    for (int v = 0; v < 3; v++) {
        char src[sizeof fh_fs_head + 512];
        snprintf(src, sizeof src, "%s%s", fh_fs_head, fh_fs_main[v]);
        if (!(fh.p[v] = ao_program(what[v], NULL, src)))
            return 0;
        fh.u_invp[v] = aof.GetUniformLocation(fh.p[v], "invp");
        fh.u_vp[v] = aof.GetUniformLocation(fh.p[v], "vp");
        fh.u_dr[v] = aof.GetUniformLocation(fh.p[v], "dr");
        fh.u_fogp[v] = aof.GetUniformLocation(fh.p[v], "fogp");
        fh.u_hgt[v] = aof.GetUniformLocation(fh.p[v], "hgt");
        aof.UseProgram(fh.p[v]);
        aof.Uniform1i(aof.GetUniformLocation(fh.p[v], "t0"), 0);
        aof.Uniform1i(aof.GetUniformLocation(fh.p[v], "t1"), 1);
    }
    aof.UseProgram(0);
    memset(fh.set, 0xff, sizeof fh.set);
    return 1;
}

static int fh_variant(unsigned tf)
{
    unsigned on = (g_tf_tex2d & ~tf) & 0xF;
    if (!on)
        return 0;
    if (!(on & 1) || g_te[0].mode != GL_MODULATE)
        return -1;
    if (on == 1)
        return 1;
    if (on != 3 || g_te[1].mode != 0x8570 || g_te[1].crgb != GL_ADD || g_te[1].calpha != GL_MODULATE ||
        g_te[1].scale[0] != 1 || g_te[1].scale[1] != 1)
        return -1;
    const GLint *s = g_te[1].src, *o = g_te[1].op;      /* 0x8578 0x1702 */
    int rgb = ((s[0] == 0x8578 && s[1] == 0x1702) || (s[0] == 0x1702 && s[1] == 0x8578)) && o[0] == 0x300 && o[1] == 0x300;
    int alpha = ((s[3] == 0x8578 && s[4] == 0x1702) || (s[3] == 0x1702 && s[4] == 0x8578)) && o[3] == 0x302 && o[4] == 0x302;
    return rgb && alpha ? 2 : -1;
}

static void fh_unknown(unsigned tf)
{
    static GLint seen[8][5];
    static int n;
    GLint k[5] = {(GLint)((g_tf_tex2d & ~tf) & 0xF), g_te[0].mode, g_te[1].mode, g_te[1].crgb, g_te[1].calpha};
    for (int i = 0; i < n; i++)
        if (!memcmp(seen[i], k, sizeof k))
            return;
    if (n == 8)
        return;
    memcpy(seen[n++], k, sizeof k);
    gmv_trace("[fog] hybrid fog: a setup without a shader keeps the linear fog (units on 0x%x, env 0x%x / 0x%x, "
            "combine 0x%x / 0x%x)\n", k[0], k[1], k[2], k[3], k[4]);
}

static void fh_count(int shaded)
{
    if (fh.frame != g_frame_no) {
        DWORD now = GetTickCount();
        if (!fh.next_log)
            fh.next_log = now + 10000;
        else if (fh.logs < 6 && (int)(now - fh.next_log) >= 0) {
            fh.next_log = now + 10000;
            fh.logs++;
            gmv_trace("[fog] hybrid fog, frame %u: %u draws through its shaders, %u with the linear fog\n", fh.frame,
                    fh.shaded, fh.linear);
        }
        fh.frame = g_frame_no;
        fh.shaded = fh.linear = 0;
    }
    if (shaded)
        fh.shaded++;
    else
        fh.linear++;
}

int fh_params(GLfloat u[30])
{
    if (memcmp(fh.proj, g_ao_proj, sizeof fh.proj)) {
        memcpy(fh.proj, g_ao_proj, sizeof fh.proj);
        fh.inv_ok = ao_invert(fh.proj, fh.invp);
    }
    if (!fh.inv_ok || g_glt.vp[2] <= 0 || g_glt.vp[3] <= 0 || !(g_ao_dr[1] > g_ao_dr[0]))
        return 0;
    memset(u, 0, 30 * sizeof *u);
    memcpy(u, fh.invp, sizeof fh.invp);
    for (int k = 0; k < 4; k++)
        u[16 + k] = (GLfloat)g_glt.vp[k];
    u[20] = g_ao_dr[0], u[21] = g_ao_dr[1];
    u[22] = g_ao_fogp[1], u[23] = 1 / (g_ao_fogp[2] - g_ao_fogp[1]), u[24] = g_ao_fogp[2], u[25] = FOG_HYBRID_K;
    if (g_fog_height > 0 && sky.n) {
        GLfloat x = sky.inv[2], y = sky.inv[5], z = sky.inv[8], l = sqrtf(x * x + y * y + z * z);
        if (l > 0) {
            GLfloat s = 1 / (l * g_fog_height);
            u[26] = x * s, u[27] = y * s, u[28] = z * s, u[29] = g_cam_z / g_fog_height;
        }
    }
    return 1;
}

int fog_hybrid_begin(GLenum mode, int ff, unsigned tf)
{
    if (g_fog_mode != 2 || fh.ready < 0 || mode == GL_POINTS || !g_fog_all || g_sky_draw || g_sky_glow_draw ||
        g_glt.last_fb || !g_ao_depth_on || g_ao_fogp[0] != GL_LINEAR || !(g_ao_fogp[2] > g_ao_fogp[1]))
        return 0;
    if (ff < 1 || ff > 3) {
        if (ff || !g_ao_fog_on)
            return 0;
        ao_proj_refresh();
        if (!g_ao_persp)
            return 0;
    }
    int v = fh_variant(tf);
    if (v < 0) {
        fh_unknown(tf);
        fh_count(0);
        return 0;
    }
    if (!fh.ready) {
        fh.ready = fh_init() ? 1 : -1;
        gmv_log(fh.ready > 0 ? "[fog] the hybrid fog's shaders are ready\n"
                             : "[fog] the hybrid fog falls back to the linear fog\n");
        if (fh.ready < 0)
            return 0;
    }
    GLfloat u[30];
    if (!fh_params(u)) {
        fh_count(0);
        return 0;
    }
    aof.UseProgram(fh.p[v]);
    if (memcmp(fh.set[v], u, sizeof u)) {
        memcpy(fh.set[v], u, sizeof u);
        aof.UniformMatrix4fv(fh.u_invp[v], 1, GL_FALSE, u);
        aof.Uniform4f(fh.u_vp[v], u[16], u[17], u[18], u[19]);
        aof.Uniform2f(fh.u_dr[v], u[20], u[21]);
        aof.Uniform4f(fh.u_fogp[v], u[22], u[23], u[24], u[25]);
        aof.Uniform4f(fh.u_hgt[v], u[26], u[27], u[28], u[29]);
    }
    static int logged;
    if (!logged++)
        gmv_trace("[fog] hybrid fog: %.0f..%.0f m, the eye %.1f m up, up in eye space (%.2f %.2f %.2f)\n",
                g_ao_fogp[1] / 100, g_ao_fogp[2] / 100, g_cam_z / 100, u[26] * g_fog_height, u[27] * g_fog_height,
                u[28] * g_fog_height);
    fh_count(1);
    return 1;
}

void fog_hybrid_end(int on)
{
    if (on)
        aof.UseProgram(0);
}
