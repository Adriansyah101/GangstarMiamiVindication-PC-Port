#include <stdint.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>
#include <SDL3/SDL.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

void gmv_video_start(void)
{
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", gmv_env_off(gmv_getenv("GMV_DPI_AWARE")) ? "unaware" : "system");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        gmv_log("[gl] SDL video failed (%s)\n", SDL_GetError());
        gmv_fatal(10, "The game's window could not be set up (%s).", SDL_GetError());
    }
    gmv_trace("[gl] SDL video up in %.1f ms\n",
              (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
}

void gmv_video_desktop(int *w, int *h)
{
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m) {
        *w = m->w;
        *h = m->h;
    }
}

#ifdef _WIN32
static LRESULT CALLBACK pf_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static HWND pf_window(void)
{
    if (gmv_env_off(gmv_getenv("GMV_SWAP_COPY")))
        return NULL;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = pf_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = GMV_GAME_NAME "Format";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return NULL;
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {0}, want = {0};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf && DescribePixelFormat(dc, pf, sizeof want, &want) && !(want.dwFlags & PFD_SWAP_COPY)) {
        const DWORD need = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SWAP_COPY;
        int n = DescribePixelFormat(dc, 1, sizeof want, NULL);
        for (int i = 1; i <= n; i++) {
            PIXELFORMATDESCRIPTOR c = {0};
            if (!DescribePixelFormat(dc, i, sizeof c, &c))
                continue;
            if ((c.dwFlags & need) == need && !(c.dwFlags & PFD_GENERIC_FORMAT) &&
                c.iPixelType == want.iPixelType && c.cColorBits == want.cColorBits &&
                c.cAlphaBits == want.cAlphaBits && c.cDepthBits == want.cDepthBits &&
                c.cStencilBits == want.cStencilBits && c.cAccumBits == want.cAccumBits) {
                pf = i;
                break;
            }
        }
    }
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
        gmv_log("[gl] no format of our own (%lu); SDL chooses\n", GetLastError());
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return NULL;
    }
    PIXELFORMATDESCRIPTOR got = {0};
    DescribePixelFormat(dc, pf, sizeof got, &got);
    gmv_trace("[gl] pixelformat #%d flags=0x%lx doublebuffer=%d swap=%s color=%d alpha=%d depth=%d stencil=%d\n",
              pf, got.dwFlags, (got.dwFlags & PFD_DOUBLEBUFFER) ? 1 : 0,
              (got.dwFlags & PFD_SWAP_COPY) ? "copy" : (got.dwFlags & PFD_SWAP_EXCHANGE) ? "exchange" : "undefined",
              got.cColorBits, got.cAlphaBits, got.cDepthBits, got.cStencilBits);
    ReleaseDC(hwnd, dc);
    return hwnd;
}
#endif

static int g_asked_w, g_asked_h;

#if !defined(_WIN32) && defined(GMV_ICON_FILE)
__asm__(".pushsection .rodata\n"
        "gmv_icon_ico:\n"
        ".incbin \"" GMV_ICON_FILE "\"\n"
        "gmv_icon_ico_end:\n"
        ".popsection\n");
extern const unsigned char gmv_icon_ico[], gmv_icon_ico_end[];

static unsigned ico16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t ico32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static SDL_Surface *ico_picture(const unsigned char *ico, size_t n, const unsigned char *entry)
{
    uint32_t len = ico32(entry + 8), off = ico32(entry + 12);
    if (off > n || len > n - off || len < 40)
        return NULL;
    const unsigned char *h = ico + off;
    uint32_t hs = ico32(h);
    int w = (int)ico32(h + 4), ht = (int)ico32(h + 8) / 2;
    if (hs < 40 || ico16(h + 14) != 32 || ico32(h + 16) != 0 || w <= 0 || w > 256 || ht != w
        || len - hs < (uint32_t)w * (uint32_t)ht * 4)
        return NULL;
    SDL_Surface *s = SDL_CreateSurface(w, ht, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return NULL;
    for (int y = 0; y < ht; y++)
        memcpy((unsigned char *)s->pixels + (size_t)y * s->pitch, h + hs + (size_t)(ht - 1 - y) * w * 4, (size_t)w * 4);
    return s;
}

static void set_window_icon(SDL_Window *win)
{
    const unsigned char *ico = gmv_icon_ico;
    size_t n = (size_t)(gmv_icon_ico_end - gmv_icon_ico);
    if (n < 6 || ico16(ico + 2) != 1)
        return;
    unsigned count = ico16(ico + 4);
    SDL_Surface *pic[16];
    int np = 0;
    for (unsigned i = 0; i < count && np < 16 && 6 + 16 * (size_t)(i + 1) <= n; i++) {
        SDL_Surface *s = ico_picture(ico, n, ico + 6 + 16 * i);
        if (s)
            pic[np++] = s;
    }
    if (!np) {
        gmv_log("[gl] window icon: no picture in the built-in app.ico\n");
        return;
    }
    int big = 0;
    for (int i = 1; i < np; i++)
        if (pic[i]->w > pic[big]->w)
            big = i;
    for (int i = 0; i < np; i++)
        if (i != big) {
            SDL_AddSurfaceAlternateImage(pic[big], pic[i]);
            SDL_DestroySurface(pic[i]);
        }
    if (SDL_SetWindowIcon(win, pic[big]))
        gmv_log("[gl] window icon: %d sizes, largest %dx%d\n", np, pic[big]->w, pic[big]->h);
    else
        gmv_log("[gl] window icon not set (%s)\n", SDL_GetError());
    SDL_DestroySurface(pic[big]);
}
#endif

void ensure_gl(void)
{
    if (g_glctx)
        return;

    uint64_t t0 = SDL_GetPerformanceCounter();
    long rw = 1024, rh = 768; int fs = 0;
    gmv_effective_screen(&rw, &rh, &fs);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, GMV_GAME_TITLE);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, rw);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, rh);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fs != 0);
#ifdef _WIN32
    HWND format_of = pf_window();
    if (format_of)
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_PIXEL_FORMAT_HWND_POINTER, format_of);
#endif
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
#ifdef _WIN32
    if (format_of)
        DestroyWindow(format_of);
#endif
    gmv_log("[gl] window: %ldx%ld %s (from the game's saved resolution option)\n",
            rw, rh, fs ? "FULLSCREEN (borderless)" : "windowed");
    if (!g_window) {
        gmv_log("[gl] window failed (%s)\n", SDL_GetError());
        gmv_fatal(10, "The game's window could not be made with OpenGL (%s). Updating the graphics driver may help.",
                  SDL_GetError());
    }
    if (!fs) {
        g_asked_w = (int)rw;
        g_asked_h = (int)rh;
    }
    int cw = (int)rw, ch = (int)rh;
    SDL_GetWindowSizeInPixels(g_window, &cw, &ch);
    gmv_input_set_client_size(cw, ch);

#ifdef _WIN32
    {
        HWND hwnd = (HWND)gmv_gl_window_hwnd();
        HINSTANCE inst = GetModuleHandleA(NULL);
        HICON big = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                      LR_DEFAULTCOLOR);
        HICON sml = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                      LR_DEFAULTCOLOR);
        if (big) SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (sml) SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sml);
    }
#elif defined(GMV_ICON_FILE)
    set_window_icon(g_window);
#endif

    g_glctx = SDL_GL_CreateContext(g_window);
    if (!g_glctx) {
        gmv_log("[gl] context failed (%s)\n", SDL_GetError());
        gmv_fatal(10, "The graphics driver could not start OpenGL for this game (%s). "
                      "Updating the graphics driver may help.", SDL_GetError());
    }
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    gmv_log("[gl] window + GL context ready: GL %s, %s\n", ver ? ver : "?", ren ? ren : "?");
    gmv_trace("[gl] window and context took %.1f ms\n",
              (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    {
        int db = 0, r = 0, g = 0, b = 0, a = 0, d = 0, st = 0;
        SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &db);
        SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
        SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d);
        SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &st);
        gmv_trace("[gl] pixel format doublebuffer=%d color=%d/%d/%d alpha=%d depth=%d stencil=%d\n", db, r, g, b, a, d, st);
    }

    {
        int v = swap_want() >= 0 ? swap_want() : (pace_frame_ms() > 0.0 ? 0 : -1);
        SDL_GL_GetSwapInterval(&g_swap_default);
        if (v >= 0) {
            if (swap_interval(v))
                gmv_log("[framepace] swap interval %d; tick pacer %.2f ms%s\n", v,
                        pace_frame_ms(), pace_frame_ms() > 0.0 ? "" : " (off)");
            else
                gmv_log("[framepace] swap interval not available; left at the driver's default\n");
        }
    }
    gmv_gpu_profile_start();
    msaa_init();
}

void gmv_gl_make_current_raw(void)
{
    ensure_gl();
    SDL_GL_MakeCurrent(g_window, g_glctx);
}
void gmv_gl_present_raw(void)
{
    if (g_window) {
        msaa_present();
        SDL_GL_SwapWindow(g_window);
        msaa_sync_size();
        gmv_game_after_present();
    }
}

void gmv_gl_redraw(void)
{
    static int said;
    if (!g_window || !g_ms_fbo || SDL_GL_GetCurrentContext() != g_glctx)
        return;
    msaa_present();
    SDL_GL_SwapWindow(g_window);
    if (!said) {
        said = 1;
        gmv_trace("[gl] drew the last frame again while the game waits for the window\n");
    }
}

void gmv_gl_client_size(int *w, int *h)
{
    int cw = 0, ch = 0;
    if (g_window && SDL_GetWindowSizeInPixels(g_window, &cw, &ch)) {
        if (w && cw > 0) *w = cw;
        if (h && ch > 0) *h = ch;
    }
}

void gl_swap_window(void)
{
    SDL_GL_SwapWindow(g_window);
}

void gl_window_fit(int fs, int w, int h)
{
    int full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (fs) {
        if (full)
            return;
        SDL_SetWindowFullscreen(g_window, true);
        gmv_trace("[gl] window: full screen\n");
    } else {
        int cw = 0, ch = 0;
        if (!full && w == g_asked_w && h == g_asked_h)
            return;
        g_asked_w = w;
        g_asked_h = h;
        if (full)
            SDL_SetWindowFullscreen(g_window, false);
        else if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED)
            SDL_RestoreWindow(g_window);
        SDL_GetWindowSizeInPixels(g_window, &cw, &ch);
        if (cw == w && ch == h)
            return;
        SDL_SetWindowSize(g_window, w, h);
        gmv_trace("[gl] window: client %dx%d\n", w, h);
    }
    SDL_SyncWindow(g_window);
    int cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(g_window, &cw, &ch);
    gmv_input_set_client_size(cw, ch);
}

void gmv_gl_render_size(int *w, int *h)
{
    gmv_gl_client_size(w, h);
    if (g_fb_scale_off)
        return;
    if (!gmv_prefs_get_long("FullScreen", 1)) {
        if (g_asked_w > 0 && g_asked_h > 0) {
            if (w) *w = g_asked_w;
            if (h) *h = g_asked_h;
        }
        return;
    }
    long rw = 0, rh = 0;
    gmv_chosen_resolution(&rw, &rh);
    if (rw > 0 && rh > 0) {
        if (w) *w = (int)rw;
        if (h) *h = (int)rh;
    }
}

void gmv_gl_view_rect(int *x, int *y, int *w, int *h, int *gw, int *gh)
{
    int cw = 1024, ch = 768;
    gmv_gl_client_size(&cw, &ch);
    int sw = cw, sh = ch;
    if (g_ms_fbo && g_ms_w > 0 && g_ms_h > 0) {
        sw = g_ms_w;
        sh = g_ms_h;
    }
    int vw = cw, vh = ch;
    if (sw != cw || sh != ch) {
        if ((long long)cw * sh > (long long)ch * sw)
            vw = (int)(((long long)sw * ch + sh / 2) / sh);
        else
            vh = (int)(((long long)sh * cw + sw / 2) / sw);
    }
    if (x) *x = (cw - vw) / 2;
    if (y) *y = (ch - vh) / 2;
    if (w) *w = vw;
    if (h) *h = vh;
    if (gw) *gw = sw;
    if (gh) *gh = sh;
}

void *gmv_gl_context_init(void *self, const char *sel, void *format, void *share)
{
    (void)sel;
    (void)format;
    (void)share;
    ensure_gl();
    return self;
}

void *gmv_gl_make_current(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    ensure_gl();
    SDL_GL_MakeCurrent(g_window, g_glctx);
    return NULL;
}

void *gmv_gl_flush_buffer(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    static unsigned frames;
    if (g_window) {
        gmv_game_frame_end();
        msaa_present();
        SDL_GL_SwapWindow(g_window);
        msaa_sync_size();
        gmv_game_after_present();
        if (frames % 60 == 0)
            gmv_trace("[gl] flushBuffer: presented frame %u\n", frames);
        gl_trace_frame_end(frames);
        frames++;
    }
    return NULL;
}
