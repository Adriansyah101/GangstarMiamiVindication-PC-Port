#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x65a488 0x1b814e 0x1b8a60 [state+0xc] 0x1b81a8 0x3a97 */
#define SW_VT       0x0065a488u
#define SW_UPDATE   0x001b81a8u
#define SW_CLOCK    0x001b8a60u
#define SW_HOLD_MS  15000u
#define SW_SHORT_MS 3000u

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t sw_clock(void)
{
    return ((uint32_t (*)(void))(uintptr_t)SW_CLOCK)();
}

static int sw_pressed(int prime)
{
    static unsigned seen;
    static unsigned char pad;
    unsigned n = gmv_input_presses();
    int hit = n != seen;
    seen = n;
    unsigned char p = (unsigned char)gmv_joy_any_button();
    hit |= p && !pad;
    pad = p;
    return !prime && hit;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_sw_update(unsigned char *st, void *mgr)
{
    static unsigned char *seen;
    uint32_t now = sw_clock(), start = *(uint32_t *)(st + 0xc);
    int first = st != seen, press = sw_pressed(first);
    seen = st;
    if (now - start < SW_HOLD_MS && (now - start >= SW_SHORT_MS || press)) {
        *(uint32_t *)(st + 0xc) = now - SW_HOLD_MS;
        gmv_trace("[gamepatch] start-up warning: on after %u ms (%s)\n", now - start, press ? "a key" : "3 s");
    }
    ((void (*)(void *, void *))(uintptr_t)SW_UPDATE)(st, mgr);
}

void apply_short_warning(void)
{
    const char *e = gmv_getenv("GMV_SHORT_WARNING");
    uint32_t mine = (uint32_t)(uintptr_t)gmv_sw_update;
    if (!(gmv_env_on(e)))
        return;
    if (((const uint32_t *)(uintptr_t)SW_VT)[6] != SW_UPDATE || !patch_bytes(SW_VT + 6 * 4, (const unsigned char *)&mine, 4)) {
        gmv_log("[gamepatch] the start-up warning's state is not the expected one; SHORT_WARNING skipped\n");
        return;
    }
    gmv_log("[gamepatch] SHORT_WARNING=1: the start-up warning shows for 3 s (the game's 15), any key skips it\n");
}
