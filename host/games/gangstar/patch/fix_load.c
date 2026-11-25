#include "compat.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xd2164 0x327b4 0x641114 0x598100(16) 0xd21d8 */
#define WAIT_SLEEP_SITE 0x000d21d8u   /* call 0x598100 */
#define GAME_SLEEP_MS   0x00598100u

int load_sleep_skip_set(int on)
{
    static const unsigned char nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    unsigned char call[5] = { 0xe8 };
    int32_t rel = (int32_t)(GAME_SLEEP_MS - (WAIT_SLEEP_SITE + 5));
    memcpy(call + 1, &rel, 4);
    const unsigned char *p = (const unsigned char *)(uintptr_t)WAIT_SLEEP_SITE;
    if ((memcmp(p, call, 5) && memcmp(p, nop5, 5)) || memcmp(p - 7, "\xc7\x04\x24\x10\x00\x00\x00", 7) != 0)
        return -1;
    return patch_bytes(WAIT_SLEEP_SITE, on ? nop5 : call, 5) ? on : -2;
}

void apply_load_sleep_skip(void)
{
    if (gmv_env_off(gmv_getenv("GMV_LOAD_SLEEP_SKIP")))
        return;
    int r = load_sleep_skip_set(1);
    if (r == -1)
        gmv_log("[gamepatch] streaming-wait sleep call 0x%08x not the expected bytes; load sleep skip skipped\n",
                WAIT_SLEEP_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); load sleep skip not applied\n", WAIT_SLEEP_SITE,
                GetLastError());
    else
        gmv_log("[gamepatch] streaming wait no longer sleeps 16 ms per tick (LOAD_SLEEP_SKIP=0 to disable)\n");
}

/* 0x65a288 0x1b589e [state+0xc] 0x1abc92 0x1b5935 */
#define FL_CALL_SITE 0x001b5935u   /* call 0x1abc92 */
#define FL_STEP_FN   0x001abc92u
#define FL_LAST_STEP 15
#define FL_BUDGET_MS 10.0

__attribute__((cdecl, force_align_arg_pointer))
static int fl_step(int step, int *percent)
{
    int (*fn)(int, int *) = (int (*)(int, int *))(uintptr_t)FL_STEP_FN;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int s = fn(step, percent);
    while (s >= 0 && s <= FL_LAST_STEP) {
        QueryPerformanceCounter(&t);
        if ((double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart >= FL_BUDGET_MS)
            break;
        s = fn(s, percent);
    }
    return s;
}

void apply_fast_load(void)
{
    if (gmv_env_off(gmv_getenv("GMV_FAST_LOAD")))
        return;
    unsigned char call[5] = { 0xe8 };
    int32_t rel = (int32_t)(FL_STEP_FN - (FL_CALL_SITE + 5));
    memcpy(call + 1, &rel, 4);
    if (memcmp((const void *)(uintptr_t)FL_CALL_SITE, call, 5)) {
        gmv_log("[gamepatch] loading screen's step call 0x%08x not the expected bytes; FAST_LOAD skipped\n", FL_CALL_SITE);
        return;
    }
    rel = (int32_t)((uint32_t)(uintptr_t)fl_step - (FL_CALL_SITE + 5));
    memcpy(call + 1, &rel, 4);
    if (!patch_bytes(FL_CALL_SITE, call, 5)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); FAST_LOAD not applied\n", FL_CALL_SITE, GetLastError());
        return;
    }
    gmv_log("[gamepatch] start-up loading screen takes as many steps a frame as fit in %.0f ms (FAST_LOAD=0 to disable)\n",
            FL_BUDGET_MS);
}

/* 0x1b9e50 0x689fe0 0x689f28 0x641114 0xd21a4 0x1adbc4 */
#define WAIT_BUDGET_SITE 0x000d21a4u   /* %eax */
#define WAIT_BUDGET_GAME 1500

int load_budget_set(int n)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)WAIT_BUDGET_SITE;
    const int32_t to = n > 0 && n < WAIT_BUDGET_GAME ? n : WAIT_BUDGET_GAME;
    if (p[0] != 0xc7 || p[1] != 0x00 || p[6] != 0x8b || p[7] != 0x86)
        return -1;
    return patch_bytes(WAIT_BUDGET_SITE + 2, (const unsigned char *)&to, 4) ? n : -2;
}

void apply_load_budget(void)
{
    const char *e = gmv_getenv("GMV_LOAD_BUDGET");
    int n = e && *e ? atoi(e) : 200;
    if (n <= 0 || n >= WAIT_BUDGET_GAME)
        return;
    int r = load_budget_set(n);
    if (r == -1)
        gmv_log("[gamepatch] streaming wait's budget at 0x%08x not the expected bytes; LOAD_BUDGET skipped\n", WAIT_BUDGET_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); LOAD_BUDGET not applied\n", WAIT_BUDGET_SITE + 2, GetLastError());
    else
        gmv_log("[gamepatch] streaming wait takes %d items a tick, not %d (LOAD_BUDGET=0 to disable)\n", n, WAIT_BUDGET_GAME);
}
