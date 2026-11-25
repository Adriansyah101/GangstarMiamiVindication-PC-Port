#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xcefc0 0x72914 0x72828 0x76090 0x7ad88 */
#define BS_SITE      0x000cf25du   /* call 0x72914 */
#define BS_SEAT      0x00072914u
#define BS_SEA       0x0063e658u   /* 0x7a2c2 */
#define BS_NODE      0x18
#define BS_CELL      0x30
#define BS_NODE_Z    8
#define BS_GROUND    0x54c         /* 0x72f1b 0x7b6e9 */
#define BS_DEEP      140.0f        /* 0x5f17f4 */
#define BS_GO        0x1c          /* 0x6a114 */
#define BS_AI        0x4e8
#define BS_AI_STEP   0x000f267cu   /* 0x76cea */

typedef void (*bs_fn)(uint32_t, uint32_t);
static const unsigned char bs_orig[5] = { 0xe8, 0xb2, 0x36, 0xfa, 0xff };
static int bs_on;

static int bs_in_water(uint32_t ch)
{
    uint32_t node = ms_rd(ch + BS_NODE), cp = node ? ms_rd(node + BS_CELL) : 0;
    if (!cp || (ms_rd(cp) & 0xffff) != (ms_rd(ms_rd(BS_SEA)) & 0xffff))
        return 0;
    return *(const float *)(uintptr_t)(node + BS_NODE_Z) - BS_DEEP > *(const float *)(uintptr_t)(ch + BS_GROUND);
}

#if GMV_DEV
#define BS_VEH       0x558
#define BS_ENTITY    0x00034af8u
#define BS_NODE_POS  0x004bfb70u
#define BS_ANIMS     0x006481c0u
#define BS_ANIM_N    388
#define BS_ANIM_ID   0x32c         /* 0x11af5e */
#define BS_WET       0x665         /* 0x7330e */
static int bs_probe;
static DWORD bs_t0;

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static uint32_t bs_call1(uint32_t fn, uint32_t a)
{
    return ((uint32_t (*)(uint32_t))(uintptr_t)fn)(a);
}

int boat_probe_on(void)
{
    return bs_probe;
}

static void bs_note(const char *what, int cmd, uint32_t ch, uint32_t boat)
{
    float p[3] = { 0 }, b[3] = { 0 };
    uint32_t node = ms_rd(ch + BS_NODE), cp = node ? ms_rd(node + BS_CELL) : 0, rn = ms_rd(ch + 0x10);
    int id = (int)ms_rd(ch + BS_ANIM_ID);
    const char *nm = id >= 0 && id < BS_ANIM_N ? (const char *)(uintptr_t)ms_rd(BS_ANIMS + 4 * id) : "none";
    if (rn)
        obj_vec(BS_NODE_POS, rn, p);
    if (boat)
        uc_pos(boat, b);
    float dx = p[0] - b[0], dy = p[1] - b[1];
    gmv_log("[boat] %7.2f s %s %d: Clive %08x in %08x, cell %u (sea %u), anim %d %s, going %d, arrived %d, "
            "wet %d, in water %d, ground %.1f, at %.1f %.1f %.1f, boat %.1f %.1f %.1f (%.1f away, "
            "%+.1f up)\n",
            (GetTickCount() - bs_t0) / 1000.0, what, cmd, ch, ms_rd(ch + BS_VEH), cp ? ms_rd(cp) & 0xffff : 0xffffu,
            ms_rd(ms_rd(BS_SEA)) & 0xffff, id, nm ? nm : "?", (int)(ms_rd(ch + 0x450) & 0xff),
            (int)(ms_rd(ch + 0x648) & 0xff), (int)(ms_rd(ch + BS_WET) & 0xff),
            bs_in_water(ch), *(const float *)(uintptr_t)(ch + BS_GROUND), p[0], p[1], p[2], b[0], b[1], b[2],
            sqrtf(dx * dx + dy * dy), p[2] - b[2]);
}

void boat_probe_note(int cmd)
{
    uint32_t ch = bs_call1(BS_ENTITY, (uint32_t)(uintptr_t)"BBM_00");
    if (ch)
        bs_note("command", cmd, ch, bs_call1(BS_ENTITY, (uint32_t)(uintptr_t)"SuperYacht_00"));
}
#define bs_probe_on() bs_probe
#else
#define bs_probe_on() 0
#endif

__attribute__((cdecl, force_align_arg_pointer))
void gmv_boat_seat(uint32_t ch, uint32_t veh)
{
#if GMV_DEV
    if (bs_probe)
        bs_note("seat, before", bs_on, ch, veh);
#endif
    uint32_t ai = ms_rd(ch + BS_AI);
    if (bs_on && ai && bs_in_water(ch)) {
        ((bs_fn)(uintptr_t)ms_rd(ms_rd(ch) + BS_GO))(ch, 0);
        ((bs_fn)(uintptr_t)BS_AI_STEP)(ai, 0);
    }
    ((bs_fn)(uintptr_t)BS_SEAT)(ch, veh);
#if GMV_DEV
    if (bs_probe)
        bs_note("seat, after", bs_on, ch, veh);
#endif
}

int boat_seat_set(int on)
{
    unsigned char site[5] = { 0xe8 };      /* 0xe8 rel32 */
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)gmv_boat_seat - (BS_SITE + 5));
    memcpy(site + 1, &rel, 4);
    const void *p = (const void *)(uintptr_t)BS_SITE;
    if (memcmp(p, bs_orig, sizeof bs_orig) && memcmp(p, site, sizeof site))
        return -1;
    bs_on = on;
    return patch_bytes(BS_SITE, on || bs_probe_on() ? site : bs_orig, sizeof site) ? on : -2;
}

void apply_boat_seat(void)
{
    int on = gmv_env_on(gmv_getenv("GMV_BOAT_SEAT_FIX"));
#if GMV_DEV
    bs_t0 = GetTickCount();
    bs_probe = gmv_env_on(gmv_dbgenv("GMV_BOAT_PROBE"));
    if (bs_probe)
        gmv_log("[boat] probe on, BOAT_SEAT_FIX=%d\n", on);
#endif
    if (!on && !bs_probe_on())
        return;
    int r = boat_seat_set(on);
    if (r == -1)
        gmv_log("[gamepatch] seat call 0x%08x not the expected bytes; BOAT_SEAT_FIX skipped\n", BS_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); BOAT_SEAT_FIX skipped\n", BS_SITE, GetLastError());
    else if (on)
        gmv_log("[gamepatch] a swimmer the script seats in a vehicle stops swimming first at 0x%08x "
                "(BOAT_SEAT_FIX=0 for the Mac's)\n", BS_SITE);
}
