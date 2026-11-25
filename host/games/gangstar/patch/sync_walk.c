#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xc897a 0x657c8 0x6341e 0x2472c 0x659c8 +0x2e8 +0x514 */
#define WS_RAY_CALL   0x000659c8u   /* call 0x2472c */
#define WS_RAY_FN     0x0002472cu
#define WS_SLOTS      64

typedef uint32_t (*ws_ray_fn)(uint32_t, float *, float *, int, int, int, float *, int, int, float *);
static struct { uint32_t ch; uint32_t res; float dest[3], hit[3], steer[3]; double due, seen; } g_ws[WS_SLOTS];
double g_ws_tick;
double g_ws_qpf;
int g_ws_on, g_as_on;

double ws_now_ms(void)
{
    return (double)gmv_qpc() / g_ws_qpf;
}

#if GMV_DEV
#endif

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_walk_ray(uint32_t ch, float *from, float *dir, int a4, int a5, int a6, float *hit,
                             int a8, int a9, float *steer)
{
#if GMV_DEV
    pd_ws_left(ch);
#endif
    if (!g_ws_on)
        return ((ws_ray_fn)(uintptr_t)WS_RAY_FN)(ch, from, dir, a4, a5, a6, hit, a8, a9, steer);
    const float *dest = (const float *)(uintptr_t)(ch + 0x514);
    double now = ws_now_ms();
    unsigned i, slot = WS_SLOTS, old = 0;
    for (i = 0; i < WS_SLOTS; i++) {
        if (g_ws[i].ch == ch) { slot = i; break; }
        if (g_ws[i].seen < g_ws[old].seen) old = i;
    }
    if (slot < WS_SLOTS && now - g_ws[slot].seen < 100.0 && now < g_ws[slot].due - g_ws_tick &&
        !memcmp(g_ws[slot].dest, dest, 12)) {
        g_ws[slot].seen = now;
        memcpy(hit, g_ws[slot].hit, 12);
        memcpy(steer, g_ws[slot].steer, 12);
        return g_ws[slot].res;
    }
#if GMV_DEV
    uint32_t vt_was = *(const uint32_t *)(uintptr_t)ch;
#endif
    uint32_t res = ((ws_ray_fn)(uintptr_t)WS_RAY_FN)(ch, from, dir, a4, a5, a6, hit, a8, a9, steer);
#if GMV_DEV
    pd_ws_report(ch, vt_was);
#endif
    if (slot == WS_SLOTS) {
        slot = old;
        g_ws[slot].ch = ch;
        g_ws[slot].due = 0;
    }
    g_ws[slot].due = now - g_ws[slot].due < WS_MAC_MS && !memcmp(g_ws[slot].dest, dest, 12)
                     ? g_ws[slot].due + WS_MAC_MS : now + WS_MAC_MS;
    g_ws[slot].seen = now;
    g_ws[slot].res = res;
    memcpy(g_ws[slot].dest, dest, 12);
    memcpy(g_ws[slot].hit, hit, 12);
    memcpy(g_ws[slot].steer, steer, 12);
    return res;
}

int ws_installed;

static void walk_sync_in(double ms)
{
    static int failed;
    int want = !gmv_env_off(gmv_getenv("GMV_WALK_SYNC")) && ms < 16.6;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ws_qpf = (double)f.QuadPart / 1000.0;
    g_ws_tick = ms;
    if (want && !ws_installed && !failed) {
        if (!sm_is_call(WS_RAY_CALL, WS_RAY_FN)) {
            gmv_log("[gamepatch] walk command ray call 0x%08x not the expected bytes; walk sync skipped\n",
                    WS_RAY_CALL);
            failed = 1;
        } else if (patch_call(WS_RAY_CALL, (void *)gmv_walk_ray)) {
            ws_installed = 1;
            gmv_log("[gamepatch] walk sync installed: scripted walkers steer as at the Mac's 30 updates/s "
                    "(%.3f ms ticks)\n", ms);
            g_ws_on = 1;
            return;
        } else
            failed = 1;
    }
    if (ws_installed && g_ws_on != want)
        gmv_log("[gamepatch] walk sync %s (%.3f ms ticks)\n", want ? "on" : "off", ms);
    g_ws_on = want && ws_installed;
}

void walk_sync_set(double ms)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    walk_sync_in(ms);
    patch_undo_now = keep;
}

void apply_walk_sync(void)
{
    walk_sync_set(gmv_pace_frame_ms());
}
