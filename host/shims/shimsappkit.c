#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "compat.h"
#ifndef _WIN32
#include <SDL3/SDL_events.h>
#endif

#include "layout.h"
#include "log.h"
#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

typedef void *(*msg_t)(void *self, const char *sel, ...);

static int in_image(const void *p)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    return a >= GMV_RESERVE_START && a < GMV_MACHO_END;
}

static void appkit_exedir(char *out, size_t n)
{
    char exe[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (!k || k >= sizeof exe) { out[0] = 0; return; }
    char *slash = strrchr(exe, GMV_DIRSEP);
    if (slash) slash[1] = 0; else exe[0] = 0;
    snprintf(out, n, "%s", exe);
    out[n - 1] = 0;
}

static char g_cache_dir[MAX_PATH + 32];
static INIT_ONCE g_cache_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_dirs_logged;

static BOOL CALLBACK cache_dir_init(PINIT_ONCE once, void *param, void **ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    char exedir[MAX_PATH];
    appkit_exedir(exedir, sizeof exedir);
    if (exedir[0])
        snprintf(g_cache_dir, sizeof g_cache_dir, "%scache", exedir);
    else
        snprintf(g_cache_dir, sizeof g_cache_dir, "cache");
    g_cache_dir[sizeof g_cache_dir - 1] = 0;
    return TRUE;
}

static void *shim_NSSearchPathForDirectoriesInDomains(int dir, int mask, int expand)
{
    (void)mask;
    (void)expand;
    InitOnceExecuteOnce(&g_cache_once, cache_dir_init, NULL, NULL);
    if (GetFileAttributesA(g_cache_dir) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryA(g_cache_dir, NULL);
    LONG bit = (dir >= 0 && dir < 31) ? (1L << dir) : (1L << 31);
    if (!(InterlockedOr(&g_dirs_logged, bit) & bit))
        gmv_trace("[dirs] NSSearchPath dir=%d -> %s\n", dir, g_cache_dir);
    return gmv_make_nsarray1(gmv_make_nsstring(g_cache_dir));
}

static void au_fourcc(char out[5], uint32_t v)
{
    out[0] = (char)(v >> 24); out[1] = (char)(v >> 16);
    out[2] = (char)(v >> 8);  out[3] = (char)v; out[4] = 0;
    for (int i = 0; i < 4; i++)
        if ((unsigned char)out[i] < 32 || (unsigned char)out[i] > 126) out[i] = '.';
}

void    *gmv_au_render_cb;                      /* AURenderCallback __cdecl */
void    *gmv_au_render_refcon;
double   gmv_au_sample_rate = 44100.0;
uint32_t gmv_au_channels = 2, gmv_au_bits = 16, gmv_au_format_flags, gmv_au_fmtid;

static void *audio_find_next(void *inComponent, void *desc)
{
    (void)inComponent;
    if (desc) {
        const uint32_t *d = (const uint32_t *)desc;
        char t[5], s[5], m[5];
        au_fourcc(t, d[0]); au_fourcc(s, d[1]); au_fourcc(m, d[2]);
        gmv_trace("[audio] AudioComponentFindNext type='%s' sub='%s' manu='%s'\n", t, s, m);
    }
    return (void *)1;
}
static int audio_instance_new(void *comp, void **outInstance)
{
    (void)comp;
    if (outInstance)
        *outInstance = (void *)1;
    gmv_trace("[audio] AudioComponentInstanceNew -> handle 1\n");
    return 0;
}
static int audio_set_property(void *unit, uint32_t id, uint32_t scope, uint32_t elem,
                              const void *data, uint32_t size)
{
    (void)unit;
    gmv_trace("[audio] AudioUnitSetProperty id=%u scope=%u elem=%u size=%u\n", id, scope, elem, size);
    if (id == 8 && data && size >= 40) {
        double sr; uint32_t f[8];
        memcpy(&sr, data, 8);
        memcpy(f, (const unsigned char *)data + 8, 32);
        char fid[5]; au_fourcc(fid, f[0]);
        gmv_au_sample_rate = sr; gmv_au_fmtid = f[0]; gmv_au_format_flags = f[1];
        gmv_au_channels = f[5]; gmv_au_bits = f[6];
        gmv_trace("[audio]   StreamFormat sr=%.0f fmtid='%s' flags=0x%x bytesPerPkt=%u framesPerPkt=%u "
                "bytesPerFrame=%u chans=%u bitsPerChan=%u\n",
                sr, fid, f[1], f[2], f[3], f[4], f[5], f[6]);
    }
    if (id == 23 && data && size >= 8) {
        void *const *p = (void *const *)data;
        gmv_au_render_cb = p[0]; gmv_au_render_refcon = p[1];
        gmv_trace("[audio]   SetRenderCallback proc=%p refcon=%p\n", p[0], p[1]);
    }
    return 0;
}
static int audio_output_start(void *unit)
{
    (void)unit;
    gmv_trace("[audio] AudioOutputUnitStart (cb=%p refcon=%p sr=%.0f ch=%u bits=%u flags=0x%x)\n",
            gmv_au_render_cb, gmv_au_render_refcon, gmv_au_sample_rate,
            gmv_au_channels, gmv_au_bits, gmv_au_format_flags);
    gmv_audio_start();
    return 0;
}
static int audio_output_stop(void *unit)
{
    (void)unit;
    gmv_trace("[audio] AudioOutputUnitStop\n");
    gmv_audio_stop();
    return 0;
}
/* 0x5ea9f7 kAudioUnitProperty_MaximumFramesPerSlice */
static int audio_get_property(void *unit, uint32_t id, uint32_t scope, uint32_t elem,
                              void *data, uint32_t *size)
{
    (void)unit;
    gmv_trace("[audio] AudioUnitGetProperty id=%u scope=%u elem=%u\n", id, scope, elem);
    if (id == 14 && data && size && *size >= 4) {
        uint32_t frames = gmv_audio_frames();
        memcpy(data, &frames, 4);
        *size = 4;
        gmv_trace("[audio]   MaximumFramesPerSlice -> %u\n", frames);
        return 0;
    }
    return -10879;
}
static int audio_ok(void) { return 0; }

static void *cf_null(void) { return NULL; }
static int cf_zero(void) { return 0; }
static void *cf_main_bundle(void) { return (void *)1; }

static int io_zero_out3(uint32_t a, void *b, uint32_t *out) { (void)a; (void)b; if (out) *out = 0; return 0; }
static int io_masterport(int bootstrap, uint32_t *portOut) { (void)bootstrap; if (portOut) *portOut = 0; return 0; }
static uint32_t g_io_master_port_default;

static int shim_Gestalt(uint32_t selector, int *response) { (void)selector; if (response) *response = 0; return 0; }

/* SCNetworkReachability 0x19da80 */
static char g_sc_reach_dummy;
static void *sc_reach_create(void *allocator, const void *target) { (void)allocator; (void)target; return &g_sc_reach_dummy; }
static int sc_reach_get_flags(void *ref, uint32_t *flagsOut) { (void)ref; if (flagsOut) *flagsOut = 0; return 1; }

static void shim_NSLog(void *format, ...)
{
    const char *s = NULL;
    if (in_image(format)) {
        const char *cand = *(const char *const *)((const char *)format + 8);
        if (in_image(cand))
            s = cand;
    } else {
        s = gmv_nsstring_cstr(format);
    }
    if (!s) {
        gmv_trace("[NSLog] (game log message)\n");
        return;
    }
    char out[2048];
    va_list ap;
    va_start(ap, format);
    gmv_nsformat(out, sizeof out, s, ap);
    va_end(ap);
    gmv_trace("[NSLog] %s\n", out);
}

static void *g_nsapp;

static void *g_c_viewframe, *g_c_wswake, *g_c_wssleep, *g_c_pboard,
            *g_c_locdesc, *g_c_runloopmode, *g_c_country, *g_c_qtend, *g_c_bundlever;

static char g_cf_type_array_cb[64];
static void *g_cf_alloc_default;

void gmv_appkit_init_constants(void)
{
    g_c_viewframe   = gmv_make_nsstring("NSViewGlobalFrameDidChangeNotification");
    g_c_wswake      = gmv_make_nsstring("NSWorkspaceDidWakeNotification");
    g_c_wssleep     = gmv_make_nsstring("NSWorkspaceWillSleepNotification");
    g_c_pboard      = gmv_make_nsstring("NSStringPboardType");
    g_c_locdesc     = gmv_make_nsstring("NSLocalizedDescription");
    g_c_runloopmode = gmv_make_nsstring("kCFRunLoopDefaultMode");
    g_c_country     = gmv_make_nsstring("kCFLocaleCountryCode");
    g_c_qtend       = gmv_make_nsstring("QTMovieDidEndNotification");
    g_c_bundlever   = gmv_make_nsstring("CFBundleVersion");
}

/* -applicationDidFinishLaunching SSE esp */
__attribute__((force_align_arg_pointer))
static int shim_NSApplicationMain(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    msg_t msg = (msg_t)(void *)gmv_objc_msgSend;

    gmv_trace("[appkit] NSApplicationMain: bootstrapping the Cocoa app by hand\n");
    g_nsapp = gmv_objc_new_instance(GMV_CLASS_APP);
    void *delegate = gmv_objc_new_instance(GMV_CLASS_DELEGATE);
    gmv_trace("[appkit] NSApp=0x%08x, delegate=0x%08x; sending -applicationDidFinishLaunching:\n",
            (unsigned)(uintptr_t)g_nsapp, (unsigned)(uintptr_t)delegate);

    msg(delegate, "applicationDidFinishLaunching:", NULL);
    gmv_trace("[appkit] -applicationDidFinishLaunching: returned; entering the run loop\n");

    {
        extern void *gmv_objc_last_eaglview;
        if (gmv_objc_last_eaglview) {
            int w = 1024, h = 768;
            gmv_gl_render_size(&w, &h);
            gmv_trace("[appkit] priming EAGLView backing size %dx%d (+reshape)\n", w, h);
            msg(gmv_objc_last_eaglview, "setBackingSize::", w, h);
            msg(gmv_objc_last_eaglview, "reshape", NULL);
        }
    }

    for (int frame = 0; frame < 300; frame++) {
#ifdef _WIN32
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                gmv_trace("[appkit] WM_QUIT after %d frames\n", frame);
                return 0;
            }
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
#else
        SDL_PumpEvents();
#endif
        void *dt, *da;
        const char *ds;
        while (gmv_defer_take(&dt, &ds, &da))
            msg(dt, ds, da);
        if (frame == 3) {
            gmv_intro_play();
            gmv_fire_movie_end();
        }
        void *target = gmv_timer_target();
        if (target)
            msg(target, gmv_timer_selector(), gmv_timer_object());
        msg(delegate, "update", NULL);
        Sleep(16);
    }
    gmv_trace("[appkit] ran 300 frames without a missing symbol -- the game is updating\n");
    return 0;
}

const host_symbol gmv_host_symbols_appkit[] = {
    FN("_NSApplicationMain", shim_NSApplicationMain),
    FN("_NSApp", &g_nsapp),
    FN("_NSLog", shim_NSLog),
    FN("_NSSearchPathForDirectoriesInDomains", shim_NSSearchPathForDirectoriesInDomains),
    FN("_AudioComponentFindNext", audio_find_next),
    FN("_AudioComponentInstanceNew", audio_instance_new),
    FN("_AudioComponentInstanceDispose", audio_ok),
    FN("_AudioUnitInitialize", audio_ok),
    FN("_AudioUnitUninitialize", audio_ok),
    FN("_AudioUnitSetProperty", audio_set_property),
    FN("_AudioUnitGetProperty", audio_get_property),
    FN("_AudioOutputUnitStart", audio_output_start),
    FN("_AudioOutputUnitStop", audio_output_stop),
    FN("_CFRelease", cf_zero),
    FN("_CFRetain", cf_null),
    FN("_CFBundleGetMainBundle", cf_main_bundle),
    FN("_CFBundleGetValueForInfoDictionaryKey", cf_null),
    FN("_CFBundleGetInfoDictionary", cf_null),
    FN("_CFDictionaryGetValue", cf_null),
    FN("_CFDictionaryGetValueIfPresent", cf_zero),
    FN("_CFNumberGetValue", cf_zero),
    FN("_CFBooleanGetValue", cf_zero),
    FN("_CFArrayCreate", cf_null),
    FN("_CFAllocatorGetDefault", cf_null),
    FN("_IOServiceMatching", cf_null),
    FN("_IOBSDNameMatching", cf_null),
    FN("_IOServiceGetMatchingService", cf_null),
    FN("_IOServiceGetMatchingServices", io_zero_out3),
    FN("_IORegistryEntryGetParentEntry", io_zero_out3),
    FN("_IORegistryEntryCreateCFProperty", cf_null),
    FN("_IOMasterPort", io_masterport),
    FN("_IOIteratorNext", cf_zero),
    FN("_IOObjectRelease", cf_zero),
    FN("_kIOMasterPortDefault", &g_io_master_port_default),
    FN("_Gestalt", shim_Gestalt),
    FN("_SCNetworkReachabilityCreateWithAddress", sc_reach_create),
    FN("_SCNetworkReachabilityCreateWithName", sc_reach_create),
    FN("_SCNetworkReachabilityGetFlags", sc_reach_get_flags),
    FN("_NSViewGlobalFrameDidChangeNotification", &g_c_viewframe),
    FN("_NSWorkspaceDidWakeNotification", &g_c_wswake),
    FN("_NSWorkspaceWillSleepNotification", &g_c_wssleep),
    FN("_NSStringPboardType", &g_c_pboard),
    FN("_NSLocalizedDescriptionKey", &g_c_locdesc),
    FN("_NSDefaultRunLoopMode", &g_c_runloopmode),
    FN("_NSLocaleCountryCode", &g_c_country),
    FN("_QTMovieDidEndNotification", &g_c_qtend),
    FN("_kCFTypeArrayCallBacks", &g_cf_type_array_cb),
    FN("_kCFAllocatorDefault", &g_cf_alloc_default),
    FN("_kCFBundleVersionKey", &g_c_bundlever),

    {NULL, NULL},
};
