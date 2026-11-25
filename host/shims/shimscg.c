#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "log.h"
#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

static const uint32_t g_main_display = 1;

typedef struct { float x, y, w, h; } CGRect;

enum { CG_MODE_TAG = 0x43474d44u, CG_ARRAY_TAG = 0x43474152u };

typedef struct { uint32_t tag; uint32_t w, h; } cg_mode;
typedef struct { uint32_t tag; uint32_t count; cg_mode *modes; } cg_mode_array;

#define CG_MAX_MODES 64
static cg_mode g_modes[CG_MAX_MODES];
static uint32_t g_mode_count;
static cg_mode g_current_mode;
static cg_mode_array g_mode_array;
static int g_modes_ready;

static void cg_add_mode(uint32_t w, uint32_t h)
{
    if (!w || !h)
        return;
    for (uint32_t i = 0; i < g_mode_count; i++)
        if (g_modes[i].w == w && g_modes[i].h == h)
            return;
    if (g_mode_count >= CG_MAX_MODES)
        return;
    g_modes[g_mode_count].tag = CG_MODE_TAG;
    g_modes[g_mode_count].w = w;
    g_modes[g_mode_count].h = h;
    g_mode_count++;
}

static void cg_ensure_modes(void)
{
    if (g_modes_ready)
        return;
    g_modes_ready = 1;

    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(display);
    uint32_t cw = desk ? (uint32_t)desk->w : 1024, ch = desk ? (uint32_t)desk->h : 768;
    g_current_mode.tag = CG_MODE_TAG;
    g_current_mode.w = cw;
    g_current_mode.h = ch;
    cg_add_mode(cw, ch);

    int nm = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &nm);
    for (int i = nm - 1; i >= 0; i--)
        cg_add_mode((uint32_t)modes[i]->w, (uint32_t)modes[i]->h);
    SDL_free(modes);

    static const uint32_t common[][2] = {
        {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 800}, {1280, 1024},
        {1366, 768}, {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200},
    };
    for (size_t i = 0; i < sizeof common / sizeof *common; i++)
        cg_add_mode(common[i][0], common[i][1]);

    g_mode_array.tag = CG_ARRAY_TAG;
    g_mode_array.count = g_mode_count;
    g_mode_array.modes = g_modes;
    for (uint32_t i = 0; i < g_mode_count; i++)
        gmv_trace("[cg] mode %ux%u\n", (unsigned)g_modes[i].w, (unsigned)g_modes[i].h);
    gmv_log("[cg] %u display modes enumerated (desktop %ux%u)\n",
            (unsigned)g_mode_count, (unsigned)cw, (unsigned)ch);
}

static uint32_t cg_MainDisplayID(void) { return g_main_display; }

__attribute__((callee_pop_aggregate_return(1)))
static CGRect cg_DisplayBounds(uint32_t display)
{
    (void)display;
    cg_ensure_modes();
    CGRect r = {0.0f, 0.0f, (float)g_current_mode.w, (float)g_current_mode.h};
    return r;
}

static float cg_RectGetWidth(CGRect r)  { return r.w; }
static float cg_RectGetHeight(CGRect r) { return r.h; }

static void *cg_DisplayCopyDisplayMode(uint32_t display) { (void)display; cg_ensure_modes(); return &g_current_mode; }
static void *cg_DisplayCurrentMode(uint32_t display)     { (void)display; cg_ensure_modes(); return &g_current_mode; }

static size_t cg_DisplayModeGetWidth(cg_mode *mode)
{
    if (mode && mode->tag == CG_MODE_TAG)
        return mode->w;
    cg_ensure_modes();
    return g_current_mode.w;
}
static size_t cg_DisplayModeGetHeight(cg_mode *mode)
{
    if (mode && mode->tag == CG_MODE_TAG)
        return mode->h;
    cg_ensure_modes();
    return g_current_mode.h;
}
static double cg_DisplayModeGetRefreshRate(void *mode) { (void)mode; return 60.0; }
static void *cg_DisplayModeCopyPixelEncoding(void *mode)
{
    (void)mode;
    return gmv_make_nsstring("--------RRRRRRRRGGGGGGGGBBBBBBBB");
}

/* 0x19f100 */
static void *cg_CopyAllDisplayModes(uint32_t display, void *opts)
{
    (void)display;
    (void)opts;
    cg_ensure_modes();
    return &g_mode_array;
}

static int cf_ArrayGetCount(void *array)
{
    cg_mode_array *a = array;
    return (a && a->tag == CG_ARRAY_TAG) ? (int)a->count : 0;
}
static void *cf_ArrayGetValueAtIndex(void *array, int idx)
{
    cg_mode_array *a = array;
    if (a && a->tag == CG_ARRAY_TAG && idx >= 0 && (uint32_t)idx < a->count)
        return &a->modes[idx];
    return NULL;
}
static int cf_StringCompare(void *a, void *b, int opts)
{
    const char *sa = gmv_nsstring_cstr(a), *sb = gmv_nsstring_cstr(b);
    if (!sa || !sb)
        return (sa == sb) ? 0 : (sa ? 1 : -1);
    int r = (opts & 1) ? SDL_strcasecmp(sa, sb) : strcmp(sa, sb);
    return (r < 0) ? -1 : (r > 0) ? 1 : 0;
}

static int cg_DisplayHideCursor(uint32_t d) { (void)d; gmv_input_set_cursor_hidden(1); return 0; }
static int cg_DisplayShowCursor(uint32_t d) { (void)d; gmv_input_set_cursor_hidden(0); return 0; }
static int cg_CursorIsVisible(void) { return !gmv_input_is_cursor_hidden(); }
/* 0x3f4a */
static void cg_GetLastMouseDelta(int *dx, int *dy) { gmv_input_take_mouse_delta(dx, dy); }
static int cg_WarpMouseCursorPosition(float x, float y) { (void)x; (void)y; return 0; }
static int cg_AssociateMouseAndMouseCursorPosition(int connected) { (void)connected; return 0; }
static int cg_WindowLevelForKey(int key) { (void)key; return 0; }

static int cg_ok(void) { return 0; }
static int cg_BeginDisplayConfiguration(void **config) { if (config) *config = (void *)1; return 0; }
static int cg_AcquireDisplayFadeReservation(float seconds, uint32_t *token)
{
    (void)seconds;
    if (token)
        *token = 1;
    return 0;
}

static int cg_GetDisplayList(uint32_t max, uint32_t *displays, uint32_t *count)
{
    if (displays && max >= 1)
        displays[0] = g_main_display;
    if (count)
        *count = 1;
    return 0;
}

const host_symbol gmv_host_symbols_cg[] = {
    FN("_CGMainDisplayID", cg_MainDisplayID),
    FN("_CGDisplayBounds", cg_DisplayBounds),
    FN("_CGRectGetWidth", cg_RectGetWidth),
    FN("_CGRectGetHeight", cg_RectGetHeight),
    FN("_CGDisplayCopyDisplayMode", cg_DisplayCopyDisplayMode),
    FN("_CGDisplayCurrentMode", cg_DisplayCurrentMode),
    FN("_CGDisplayModeGetWidth", cg_DisplayModeGetWidth),
    FN("_CGDisplayModeGetHeight", cg_DisplayModeGetHeight),
    FN("_CGDisplayModeGetRefreshRate", cg_DisplayModeGetRefreshRate),
    FN("_CGDisplayModeCopyPixelEncoding", cg_DisplayModeCopyPixelEncoding),
    FN("_CGDisplayCopyAllDisplayModes", cg_CopyAllDisplayModes),

    FN("_CFArrayGetCount", cf_ArrayGetCount),
    FN("_CFArrayGetValueAtIndex", cf_ArrayGetValueAtIndex),
    FN("_CFStringCompare", cf_StringCompare),

    FN("_CGDisplayHideCursor", cg_DisplayHideCursor),
    FN("_CGDisplayShowCursor", cg_DisplayShowCursor),
    FN("_CGCursorIsVisible", cg_CursorIsVisible),
    FN("_CGGetLastMouseDelta", cg_GetLastMouseDelta),
    FN("_CGWarpMouseCursorPosition", cg_WarpMouseCursorPosition),
    FN("_CGAssociateMouseAndMouseCursorPosition", cg_AssociateMouseAndMouseCursorPosition),
    FN("_CGWindowLevelForKey", cg_WindowLevelForKey),
    FN("_CGGetActiveDisplayList", cg_GetDisplayList),
    FN("_CGGetOnlineDisplayList", cg_GetDisplayList),

    FN("_CGBeginDisplayConfiguration", cg_BeginDisplayConfiguration),
    FN("_CGConfigureDisplayWithDisplayMode", cg_ok),
    FN("_CGConfigureDisplayFadeEffect", cg_ok),
    FN("_CGCompleteDisplayConfiguration", cg_ok),
    FN("_CGCancelDisplayConfiguration", cg_ok),
    FN("_CGDisplayCapture", cg_ok),
    FN("_CGDisplayRelease", cg_ok),
    FN("_CGCaptureAllDisplays", cg_ok),
    FN("_CGReleaseAllDisplays", cg_ok),
    FN("_CGDisplayFade", cg_ok),
    FN("_CGAcquireDisplayFadeReservation", cg_AcquireDisplayFadeReservation),
    FN("_CGReleaseDisplayFadeReservation", cg_ok),
    FN("_CGSetDisplayTransferByTable", cg_ok),
    FN("_CGGetDisplayTransferByTable", cg_ok),

    {NULL, NULL},
};
