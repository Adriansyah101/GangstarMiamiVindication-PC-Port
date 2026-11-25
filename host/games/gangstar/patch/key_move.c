#include "compat.h"
#include <math.h>
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x11edf4 0x27-0x2a 0x23-0x26 0x11dcd6 0x11db86 */
static const uint32_t km_site[4] = { 0x0011ef5bu, 0x0011ef6cu, 0x0011ef7du, 0x0011ef8eu };
static const uint32_t km_fn[4]   = { 0x0011dcd6u, 0x0011db86u, 0x0011d9c6u, 0x0011d818u };
#define KM_COVER_FN 0x00061686u      /* +0x58 */
#define KM_WALK     0.32f            /* 0x69554 */
#define KM_TURN     7.85f
#define KM_PI       3.14159265f
volatile int g_km_on, g_km_ok;

int km_set(int on)
{
    if (on != g_km_on)
        gmv_log("[gamepatch] KEY_MOVE %s\n", on ? "ENHANCED" : "MAC");
    g_km_on = on && g_km_ok;
    return g_km_ok || !on;
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static void km_move(unsigned char *ch, float angle, float speed)
{
    ((void (*)(unsigned char *, float, float))(uintptr_t)*(uint32_t *)(uintptr_t)(*(uint32_t *)ch + 0x180))
        (ch, angle, speed);
}

static int km_walk_key(void)
{
    return gmv_input_scancode_held(226) || gmv_input_scancode_held(230);
}

static void km_dir(int d, unsigned char *h, int *ev)
{
    static LARGE_INTEGER last, freq, seen[4];
    static float cur;
    static int moving;
    static unsigned char *last_h;
    unsigned char *ch = *(unsigned char **)(h + 0x34);
    LARGE_INTEGER now;
    float target, dt, speed, a;
    int fx, sx, lead = d;
    if (!g_km_on || !ch) {
        pl_call2(km_fn[d], (uint32_t)(uintptr_t)h, (uint32_t)(uintptr_t)ev);
        return;
    }
    if (ev[2] <= 1)
        h[0x54 + d] = 1;
    else {
        h[0x54 + d] = 0;
        if (d >= 2)
            ch[0x677] = 0;
    }
    fx = h[0x54] - h[0x55];
    sx = h[0x56] - h[0x57];
    if (!fx && !sx) {
        km_move(ch, 0.0f, 0.0f);
        moving = 0;
        return;
    }
    target = atan2f((float)sx, (float)fx);
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    seen[d] = now;
    for (int i = 3; i >= 0; i--)
        if (h[0x54 + i] && now.QuadPart - seen[i].QuadPart < freq.QuadPart / 10)
            lead = i;
    if (d != lead)
        return;
    dt = (float)((double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart);
    last = now;
    if (!moving || last_h != h || dt > 0.25f)
        cur = target;
    else {
        float diff = target - cur, step = KM_TURN * dt;
        while (diff > KM_PI)
            diff -= 2.0f * KM_PI;
        while (diff < -KM_PI)
            diff += 2.0f * KM_PI;
        if (fabsf(diff) <= step)
            cur = target;
        else
            cur += diff > 0.0f ? step : -step;
        if (cur > KM_PI)
            cur -= 2.0f * KM_PI;
        else if (cur < -KM_PI)
            cur += 2.0f * KM_PI;
    }
    moving = 1;
    last_h = h;
    speed = !ch[0x666] && km_walk_key() ? KM_WALK : 1.0f;
    if (h[0x5a] && speed > 0.5f)
        speed = 0.5f;
    a = fabsf(cur) < 0.0175f ? (cur < 0.0f ? -0.0175f : 0.0175f) : cur;
    km_move(ch, a, speed);
    if ((unsigned char)pl_call1(KM_COVER_FN, (uint32_t)(uintptr_t)ch) != h[0x58]) {
        km_move(ch, 0.0f, 0.0f);
        *(uint32_t *)(h + 0x54) = 0;
        h[0x58] = (unsigned char)pl_call1(KM_COVER_FN, (uint32_t)(uintptr_t)ch);
        moving = 0;
    }
}

__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_fwd(unsigned char *h, int *ev)   { km_dir(0, h, ev); }
__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_back(unsigned char *h, int *ev)  { km_dir(1, h, ev); }
__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_left(unsigned char *h, int *ev)  { km_dir(2, h, ev); }
__attribute__((cdecl, force_align_arg_pointer)) static void gmv_km_right(unsigned char *h, int *ev) { km_dir(3, h, ev); }

void apply_key_move(void)
{
    void *const hooks[4] = { (void *)gmv_km_fwd, (void *)gmv_km_back, (void *)gmv_km_left, (void *)gmv_km_right };
    const char *e = gmv_getenv("GMV_KEY_MOVE");
    g_km_on = gmv_env_on(e);
    if (gmv_config_mac())
        return;
    for (int i = 0; i < 4; i++)
        if (!sm_is_call(km_site[i], km_fn[i])) {
            gmv_log("[gamepatch] the on-foot move calls are not the expected ones; no KEY_MOVE\n");
            return;
        }
    for (int i = 0; i < 4; i++)
        if (!sm_redirect(km_site[i], hooks[i])) {
            g_km_on = 0;
            gmv_log("[gamepatch] a write failed (%lu); no KEY_MOVE\n", GetLastError());
            return;
        }
    g_km_ok = 1;
    gmv_log("[gamepatch] KEY_MOVE %s (ENHANCED: W/A/S/D walk diagonally, smooth turns, Alt walks)\n",
            g_km_on ? "ENHANCED" : "MAC");
}
