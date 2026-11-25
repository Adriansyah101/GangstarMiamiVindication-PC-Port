#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <SDL3/SDL_video.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

static int cgl_set_parameter(void *ctx, int param, const void *val)
{
    (void)ctx; (void)param; (void)val;
    return 0;
}
static int cgl_get_parameter(void *ctx, int param, int *val)
{
    (void)ctx; (void)param;
    if (val)
        *val = 0;
    return 0;
}
static int cgl_enable(void *ctx, int feature)  { (void)ctx; (void)feature; return 0; }
static int cgl_disable(void *ctx, int feature) { (void)ctx; (void)feature; return 0; }
static int cgl_lock(void *ctx)   { (void)ctx; return 0; }
static int cgl_unlock(void *ctx) { (void)ctx; return 0; }

extern void gmv_gl_lazy(void);

/* GL __stdcall */
struct glcell {
    void *fn;
    const char *name;
    int tried;
};

static uint8_t *g_gl_thunks;
static size_t g_gl_off;

void *gl_real(const char *name)
{
    void *p = (void *)(uintptr_t)SDL_GL_GetProcAddress(name);
    uintptr_t v = (uintptr_t)p;
    if (v <= 3 || v == (uintptr_t)-1)
        p = NULL;
#ifdef _WIN32
    if (!p) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl)
            p = (void *)(uintptr_t)GetProcAddress(gl, name);
    }
#endif
    return p;
}

void *gmv_gl_lookup(struct glcell *c)
{
    if (!c->fn && !c->tried) {
        c->tried = 1;
        c->fn = gl_real(c->name);
        if (!c->fn)
            gmv_trace("[gl] unresolved %s -> no-op\n", c->name);
    }
    return c->fn;
}

/* mov eax; jmp gmv_gl_lazy */
void *gmv_gl_resolve(const char *name)
{
    if (!name || strncmp(name, "_gl", 3) != 0)
        return NULL;
    static int trace_checked, msaa_wrap;
    if (!trace_checked) {
        trace_checked = 1;
#if GMV_DEV
        const char *e = gmv_dbgenv("GMV_GL_TRACE");
        g_gl_trace = gmv_env_on(e);
#endif
        const char *nf = gmv_getenv("GMV_NO_FLICKER");
        g_no_flicker = gmv_env_flag(nf, 1);
        const char *tf = gmv_getenv("GMV_TEXFIX");
        g_texfix = gmv_env_flag(tf, 1);
        g_tab_live = gmv_config_tab();
        msaa_wrap = 1;
        gmv_game_gl_setup();
    }
    {
        void *t = gmv_game_gl_intercept(name);
        if (t)
            return t;
    }
    if (gmv_gl_query_free()) {
        void *t = gmv_gl_arrays_intercept(name);
        if (t)
            return t;
        if (!strcmp(name, "_glClientActiveTexture") || !strcmp(name, "_glClientActiveTextureARB") ||
            !strcmp(name, "_glEnableClientState") || !strcmp(name, "_glDisableClientState") ||
            !strcmp(name, "_glEnable") || !strcmp(name, "_glDisable") || !strcmp(name, "_glColorMask") ||
            !strcmp(name, "_glClearColor"))
            return gl_trace_intercept(name);
    }
    if (msaa_wrap) {
        if (!strcmp(name, "_glGetIntegerv"))
            return (void *)ms_GetIntegerv;
        if (!strcmp(name, "_glReadPixels"))
            return (void *)ms_ReadPixels;
        if (!strcmp(name, "_glCopyTexSubImage2D"))
            return (void *)ms_CopyTexSubImage2D;
        if (!strcmp(name, "_glDrawBuffers"))
            return (void *)ms_DrawBuffers;
    }
    if (!strcmp(name, "_glViewport") || !strcmp(name, "_glBindFramebuffer") ||
        !strcmp(name, "_glBindFramebufferEXT") || !strcmp(name, "_glDeleteFramebuffers") ||
        !strcmp(name, "_glDeleteFramebuffersEXT"))
        return gl_trace_intercept(name);
#if GMV_DEV
    if (gmv_prof_on()) {
        void *t = gl_prof_intercept(name);
        if (t)
            return t;
    }
#endif
    if (g_gl_trace || g_no_flicker || g_texfix || gmv_game_gl_wants_draws()) {
        void *t = gl_trace_intercept(name);
        if (t)
            return t;
    }
    if (!g_gl_thunks) {
        g_gl_thunks = VirtualAlloc(NULL, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_gl_thunks)
            return NULL;
    }
    if (g_gl_off + 16 > 0x20000)
        return NULL;
    struct glcell *cell = calloc(1, sizeof *cell);
    if (!cell)
        return NULL;
    cell->name = _strdup(name + 1);

    uint8_t *p = g_gl_thunks + g_gl_off;
    g_gl_off += 16;
    p[0] = 0xB8;                                 /* mov eax */
    memcpy(p + 1, &cell, 4);
    p[5] = 0xE9;
    uint32_t rel = (uint32_t)((uintptr_t)gmv_gl_lazy - (uintptr_t)(p + 10));
    memcpy(p + 6, &rel, 4);
    return p;
}

const host_symbol gmv_host_symbols_gl[] = {
    FN("_CGLSetParameter", cgl_set_parameter),
    FN("_CGLGetParameter", cgl_get_parameter),
    FN("_CGLEnable", cgl_enable),
    FN("_CGLDisable", cgl_disable),
    FN("_CGLLockContext", cgl_lock),
    FN("_CGLUnlockContext", cgl_unlock),

    {NULL, NULL},
};
