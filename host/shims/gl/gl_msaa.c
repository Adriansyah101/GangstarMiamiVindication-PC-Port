#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

/* 0x199bd0 0x5f6de0 0x5f6e40 +0x13 0x19e06e */
int g_msaa;
static int g_msaa_max;
static int g_msaa_want = -1;
int g_tab_live;
GLuint g_ms_fbo, g_ms_rb[2];
static GLuint g_rs_fbo, g_rs_rb;
int g_ms_w, g_ms_h;
GLuint g_ms_glow;
int g_glow_bufs;
int g_glow_used;
static GLint g_diag_rfb, g_diag_dfb;

msf_t msf;

void *ms_proc(const char *core, const char *ext)
{
    void *p = gl_real(core);
    return p ? p : gl_real(ext);
}

int g_query_free = -1;
static GLint qf_draw = -1, qf_read = -1, qf_prog = -1;
static void(APIENTRY *qf_bind_real)(GLenum, GLuint);
static void(APIENTRY *qf_prog_real)(GLuint);
#if GMV_DEV
static int qf_check = -1;
static unsigned qf_bad;
#endif

static int qf_on(void)
{
    if (g_query_free < 0) {
        const char *e = gmv_getenv("GMV_QUERY_FREE");
        g_query_free = !e || !*e || *e != '0';
#if GMV_DEV
        const char *c = gmv_dbgenv("GMV_QUERY_CHECK");
        qf_check = gmv_env_on(c);
#endif
        gmv_log("[gl] QUERY_FREE=%d%s%s\n", g_query_free,
                g_query_free ? ": the framebuffer and program bindings and the client arrays kept by the host, not asked "
                               "of GL" : "",
#if GMV_DEV
                g_query_free && qf_check ? " (QUERY_CHECK: asked as well, differences logged)" : "");
#else
                "");
#endif
    }
    return g_query_free;
}

int gmv_gl_query_free(void)
{
    return qf_on();
}

#if GMV_DEV
int gmv_gl_query_check(void)
{
    return qf_on() && qf_check;
}
#endif

void gmv_gl_fb_note(GLenum target, GLuint fb)
{
    if (target == MS_FRAMEBUFFER || target == MS_DRAW_FB)
        qf_draw = (GLint)fb;
    if (target == MS_FRAMEBUFFER || target == MS_READ_FB)
        qf_read = (GLint)fb;
}

void gmv_gl_fb_forget(void)
{
    qf_draw = qf_read = -1;
}

static void APIENTRY qf_bind(GLenum target, GLuint fb)
{
    gmv_gl_fb_note(target, fb);
    qf_bind_real(target, fb);
}

static void APIENTRY qf_use(GLuint p)
{
    qf_prog = (GLint)p;
    qf_prog_real(p);
}

gl_bindfb_fn gmv_gl_track_fb(gl_bindfb_fn real)
{
    if (!real)
        return NULL;
    qf_bind_real = real;
    return qf_bind;
}

gl_useprog_fn gmv_gl_track_prog(gl_useprog_fn real)
{
    if (!real)
        return NULL;
    qf_prog_real = real;
    return qf_use;
}

static GLint qf_answer(GLint *kept, GLenum pname)
{
    if (!qf_on() || *kept < 0) {
        GLint v = 0;
        glGetIntegerv(pname, &v);
        if (g_query_free)
            *kept = v;
        return v;
    }
#if GMV_DEV
    if (qf_check) {
        GLint v = 0;
        glGetIntegerv(pname, &v);
        if (v != *kept && qf_bad++ < 20)
            gmv_log("[gl] QUERY_CHECK: 0x%x kept %d, GL says %d\n", pname, *kept, v);
    }
#endif
    return *kept;
}

GLint gmv_gl_cur_fb(GLenum pname)
{
    return qf_answer(pname == MS_READ_FB_BINDING ? &qf_read : &qf_draw, pname);
}

GLint gmv_gl_cur_prog(void)
{
    return qf_answer(&qf_prog, 0x8B8D);
}

static void msaa_clear(void)
{
    GLint dfb = 0;
    GLfloat cc[4];
    GLboolean cm[4], sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(MS_DRAW_FB_BINDING, &dfb);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    msf.BindFramebuffer(MS_DRAW_FB, g_ms_fbo);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
}

static void msaa_alloc(int w, int h)
{
    GLint rb = 0;
    glGetIntegerv(0x8CA7, &rb);
    msf.BindRenderbuffer(MS_RENDERBUFFER, g_ms_rb[0]);
    msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, g_msaa, 0x8051, w, h);
    msf.BindRenderbuffer(MS_RENDERBUFFER, g_ms_rb[1]);
    msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, g_msaa, 0x88F0, w, h);
    msf.BindRenderbuffer(MS_RENDERBUFFER, g_rs_rb);
    msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, 0, 0x8051, w, h);
    if (g_ms_glow) {
        msf.BindRenderbuffer(MS_RENDERBUFFER, g_ms_glow);
        msf.RenderbufferStorageMultisample(MS_RENDERBUFFER, g_msaa, 0x881A, w, h);
    }
    msf.BindRenderbuffer(MS_RENDERBUFFER, (GLuint)rb);
    g_ms_w = w;
    g_ms_h = h;
}

int g_fb_scale_off;

void msaa_init(void)
{
    const char *e = gmv_getenv("GMV_MSAA");
    int want = gmv_setting_is_auto(e) ? 4 : atoi(e);
    g_fb_scale_off = 1;
    msf.GenFramebuffers = ms_proc("glGenFramebuffers", "glGenFramebuffersEXT");
    msf.BindFramebuffer = gmv_gl_track_fb(ms_proc("glBindFramebuffer", "glBindFramebufferEXT"));
    msf.GenRenderbuffers = ms_proc("glGenRenderbuffers", "glGenRenderbuffersEXT");
    msf.BindRenderbuffer = ms_proc("glBindRenderbuffer", "glBindRenderbufferEXT");
    msf.RenderbufferStorageMultisample = ms_proc("glRenderbufferStorageMultisample",
                                                 "glRenderbufferStorageMultisampleEXT");
    msf.FramebufferRenderbuffer = ms_proc("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    msf.CheckFramebufferStatus = ms_proc("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    msf.BlitFramebuffer = ms_proc("glBlitFramebuffer", "glBlitFramebufferEXT");
    if (!msf.GenFramebuffers || !msf.BindFramebuffer || !msf.GenRenderbuffers || !msf.BindRenderbuffer ||
        !msf.RenderbufferStorageMultisample || !msf.FramebufferRenderbuffer || !msf.CheckFramebufferStatus ||
        !msf.BlitFramebuffer) {
        gmv_log("[msaa] MSAA=%d: the driver has no framebuffer objects; anti-aliasing off, and full screen "
                "draws at the monitor's resolution\n", want);
        return;
    }
    GLint mx = 0;
    glGetIntegerv(0x8D57, &mx);
    g_msaa_max = mx;
    g_msaa = want < mx ? want : mx;
    if (g_msaa <= 1) {
        if (want > 1)
            gmv_log("[msaa] MSAA=%d: GPU maximum %d samples; anti-aliasing off\n", want, mx);
        g_msaa = 0;
    }
    g_fb_scale_off = 0;
    int w = 1024, h = 768;
    gmv_gl_render_size(&w, &h);
    msf.GenFramebuffers(1, &g_ms_fbo);
    msf.GenFramebuffers(1, &g_rs_fbo);
    msf.GenRenderbuffers(2, g_ms_rb);
    msf.GenRenderbuffers(1, &g_rs_rb);
    msaa_alloc(w, h);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_rs_fbo);
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8CE0, MS_RENDERBUFFER, g_rs_rb);
    GLenum rs = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    msf.BindFramebuffer(MS_FRAMEBUFFER, g_ms_fbo);
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8CE0, MS_RENDERBUFFER, g_ms_rb[0]);
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8D00, MS_RENDERBUFFER, g_ms_rb[1]);
    msf.FramebufferRenderbuffer(MS_FRAMEBUFFER, 0x8D20, MS_RENDERBUFFER, g_ms_rb[1]);
    GLenum ms = msf.CheckFramebufferStatus(MS_FRAMEBUFFER);
    if (ms != 0x8CD5 || rs != 0x8CD5) {
        gmv_log("[msaa] framebuffer incomplete (0x%x / 0x%x); anti-aliasing off, and full screen draws at "
                "the monitor's resolution\n", ms, rs);
        msf.BindFramebuffer(MS_FRAMEBUFFER, 0);
        g_ms_fbo = 0;
        g_msaa = 0;
        g_fb_scale_off = 1;
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (g_msaa)
        gmv_log("[msaa] MSAA=%d%s, GPU maximum %d: the game draws into a %dx multisampled %dx%d screen\n",
                want, gmv_setting_is_auto(e) ? " (auto)" : "", mx, g_msaa, w, h);
    else
        gmv_log("[msaa] MSAA=%d%s, GPU maximum %d: the game draws into a %dx%d screen without samples\n",
                want, gmv_setting_is_auto(e) ? " (auto)" : "", mx, w, h);
}

void msaa_resolve(GLint x, GLint y, GLint w, GLint h)
{
    GLboolean sc = qf_on() ? (GLboolean)g_glt.scissor : glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
    msf.BindFramebuffer(MS_DRAW_FB, g_rs_fbo);
    msf.BlitFramebuffer(x, y, x + w, y + h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    msf.BindFramebuffer(MS_READ_FB, g_rs_fbo);
    if (sc) glEnable(GL_SCISSOR_TEST);
}

void msaa_present(void)
{
    if (!g_ms_fbo)
        return;
    GLint dfb = 0, rfb = 0;
    dfb = gmv_gl_cur_fb(MS_DRAW_FB_BINDING);
    rfb = gmv_gl_cur_fb(MS_READ_FB_BINDING);
    if (g_msaa)
        msaa_resolve(0, 0, g_ms_w, g_ms_h);
    else
        msf.BindFramebuffer(MS_READ_FB, g_ms_fbo);
    GLboolean sc = qf_on() ? (GLboolean)g_glt.scissor : glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_DRAW_FB, 0);
    int x, y, w, h, cw = g_ms_w, ch = g_ms_h;
    gmv_gl_view_rect(&x, &y, &w, &h, NULL, NULL);
    gmv_gl_client_size(&cw, &ch);
    if (x == 0 && y == 0 && w == g_ms_w && h == g_ms_h)
        msf.BlitFramebuffer(0, 0, g_ms_w, g_ms_h, 0, 0, g_ms_w, g_ms_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    else {
        GLfloat cc[4];
        GLboolean cm[4];
        if (qf_on()) {
            memcpy(cc, g_glt.cc, sizeof cc);
            memcpy(cm, g_glt.cmask, sizeof cm);
        } else {
            glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
            glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        glColorMask(cm[0], cm[1], cm[2], cm[3]);
        int gy = ch - y - h;
        msf.BlitFramebuffer(0, 0, g_ms_w, g_ms_h, x, gy, x + w, gy + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    if (sc) glEnable(GL_SCISSOR_TEST);
    msf.BindFramebuffer(MS_READ_FB, (GLuint)rfb);
    msf.BindFramebuffer(MS_DRAW_FB, (GLuint)dfb);
}

int gmv_set_msaa(int samples)
{
    if (!g_ms_fbo)
        return 0;
    g_msaa_want = samples <= 1 ? 0 : samples < g_msaa_max ? samples : g_msaa_max;
    return 1;
}

void msaa_sync_samples(void)
{
    int want = g_msaa_want;
    if (!g_ms_fbo || want < 0)
        return;
    g_msaa_want = -1;
    if (want == g_msaa)
        return;
    g_msaa = want;
    msaa_alloc(g_ms_w, g_ms_h);
    msaa_clear();
    gmv_log("[msaa] the screen now has %d samples%s\n", g_msaa, g_msaa ? "" : " (anti-aliasing off)");
}

void msaa_sync_size(void)
{
    if (!g_ms_fbo)
        return;
    int w = g_ms_w, h = g_ms_h;
    gmv_gl_render_size(&w, &h);
    if (w == g_ms_w && h == g_ms_h)
        return;
    msaa_alloc(w, h);
    msaa_clear();
    int cw = w, ch = h;
    gmv_gl_client_size(&cw, &ch);
    if (cw == w && ch == h)
        gmv_log("[msaa] screen resized to %dx%d\n", w, h);
    else
        gmv_log("[msaa] screen resized to %dx%d, scaled to fit the %dx%d window\n", w, h, cw, ch);
    gmv_input_view_changed();
}

void diag_read_begin(GLenum buf)
{
    if (g_ms_fbo) {
        glGetIntegerv(MS_DRAW_FB_BINDING, &g_diag_dfb);
        glGetIntegerv(MS_READ_FB_BINDING, &g_diag_rfb);
        if (buf == GL_BACK) {
            msaa_resolve(0, 0, g_ms_w, g_ms_h);
            return;
        }
        msf.BindFramebuffer(MS_READ_FB, 0);
    }
    glReadBuffer(buf);
}
void diag_read_end(void)
{
    if (g_ms_fbo) {
        msf.BindFramebuffer(MS_READ_FB, (GLuint)g_diag_rfb);
        msf.BindFramebuffer(MS_DRAW_FB, (GLuint)g_diag_dfb);
    }
}
