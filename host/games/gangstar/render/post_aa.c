#include "compat.h"
#include <GL/gl.h>

#include "log.h"
#include "render.h"

int g_paa_want;
static int paa_ready;
static GLuint paa_p;
static GLint paa_u_src, paa_u_rcp;

static const char paa_fs[] =
    "#version 120\n"
    "uniform sampler2D src;\n"
    "uniform vec2 rcp;\n"
    "float lum(vec2 p) { return dot(texture2D(src, p).rgb, vec3(0.299, 0.587, 0.114)); }\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy * rcp;\n"
    "    vec3 c = texture2D(src, uv).rgb;\n"
    "    float m = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "    float n = lum(uv - vec2(0.0, rcp.y)), s = lum(uv + vec2(0.0, rcp.y));\n"
    "    float w = lum(uv - vec2(rcp.x, 0.0)), e = lum(uv + vec2(rcp.x, 0.0));\n"
    "    float hi = max(max(max(n, s), max(w, e)), m);\n"
    "    float lo = min(min(min(n, s), min(w, e)), m);\n"
    "    float range = hi - lo;\n"
    "    if (range < max(0.0625, hi * 0.125)) {\n"
    "        gl_FragColor = vec4(c, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    float nw = lum(uv - rcp), se = lum(uv + rcp);\n"
    "    float ne = lum(uv + vec2(rcp.x, -rcp.y)), sw = lum(uv + vec2(-rcp.x, rcp.y));\n"
    "    float horz = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);\n"
    "    float vert = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);\n"
    "    bool hs = horz >= vert;\n"
    "    float sub = clamp(abs((2.0 * (n + s + w + e) + nw + ne + sw + se) / 12.0 - m) / range, 0.0, 1.0);\n"
    "    sub = (3.0 - 2.0 * sub) * sub * sub;\n"
    "    sub = sub * sub * 0.75;\n"
    "    float l1 = hs ? n : w, l2 = hs ? s : e;\n"
    "    float g1 = abs(l1 - m), g2 = abs(l2 - m);\n"
    "    bool p1 = g1 >= g2;\n"
    "    float grad = 0.25 * max(g1, g2);\n"
    "    float edge = 0.5 * ((p1 ? l1 : l2) + m);\n"
    "    float len = hs ? rcp.y : rcp.x;\n"
    "    if (p1)\n"
    "        len = -len;\n"
    "    vec2 pos = uv;\n"
    "    if (hs)\n"
    "        pos.y += 0.5 * len;\n"
    "    else\n"
    "        pos.x += 0.5 * len;\n"
    "    vec2 off = hs ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);\n"
    "    vec2 pn = pos - off, pp = pos + off;\n"
    "    float en = lum(pn) - edge, ep = lum(pp) - edge;\n"
    "    bool dn = abs(en) >= grad, dp = abs(ep) >= grad;\n"
    "    for (int i = 0; i < 8; i++) {\n"
    "        if (dn && dp)\n"
    "            break;\n"
    "        float k = i < 1 ? 1.5 : i < 5 ? 2.0 : i < 6 ? 4.0 : 8.0;\n"
    "        if (!dn) {\n"
    "            pn -= off * k;\n"
    "            en = lum(pn) - edge;\n"
    "            dn = abs(en) >= grad;\n"
    "        }\n"
    "        if (!dp) {\n"
    "            pp += off * k;\n"
    "            ep = lum(pp) - edge;\n"
    "            dp = abs(ep) >= grad;\n"
    "        }\n"
    "    }\n"
    "    float dsn = hs ? uv.x - pn.x : uv.y - pn.y;\n"
    "    float dsp = hs ? pp.x - uv.x : pp.y - uv.y;\n"
    "    bool below = m < edge;\n"
    "    bool good = dsn < dsp ? (en < 0.0) != below : (ep < 0.0) != below;\n"
    "    float px = max(good ? 0.5 - min(dsn, dsp) / (dsn + dsp) : 0.0, sub);\n"
    "    if (hs)\n"
    "        uv.y += px * len;\n"
    "    else\n"
    "        uv.x += px * len;\n"
    "    gl_FragColor = vec4(texture2D(src, uv).rgb, 1.0);\n"
    "}\n";

int paa_frame(void)
{
    return g_paa_want && paa_ready >= 0 && g_ms_fbo && (g_paa_want == 1 || g_msaa < 2);
}

int paa_init(void)
{
    if (!paa_ready) {
        paa_p = ao_procs("POST_AA") ? ao_program("FXAA", ao_vs, paa_fs) : 0;
        paa_ready = paa_p ? 1 : -1;
        if (!paa_p) {
            gmv_log("[aa] POST_AA off\n");
            return 0;
        }
        paa_u_src = aof.GetUniformLocation(paa_p, "src");
        paa_u_rcp = aof.GetUniformLocation(paa_p, "rcp");
    }
    return paa_ready > 0;
}

void paa_pass(int w, int h)
{
    GLuint t = post_color(w, h);
    if (!t) {
        gmv_log("[aa] no %dx%d colour copy; POST_AA off\n", w, h);
        paa_ready = -1;
        return;
    }
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    aof.UseProgram(paa_p);
    aof.ActiveTexture(0x84C0);
    glBindTexture(GL_TEXTURE_2D, t);
    aof.Uniform1i(paa_u_src, 0);
    aof.Uniform2f(paa_u_rcp, 1.0f / w, 1.0f / h);
    ao_tri();
}
