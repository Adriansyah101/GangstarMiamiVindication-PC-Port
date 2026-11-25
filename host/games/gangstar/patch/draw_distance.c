#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* DRAW_DISTANCE 0x1a8c5e 0x949a00 +0x0c +0x10 0x76c59 */
#define DD_READ_SITE   0x001a8c5eu
#define DD_READ_FN     0x005edb64u
#define DD_ACTOR_SITE  0x00076c59u   /* 0x5c7f1b(%eax),%ecx */
#define DD_PROXIES_IMM 0x001a8c9fu   /* movl $0x4000 */
#define DD_PAIRS_IMM   0x001a8ca4u   /* movl $0x20000,%eax: */
#define DD_MAX_PAIRS   0x00649684u
#define DD_MAX         4.0
#define DD_KEEP_K      0x005f096cu
#define DD_LIVE_AREA   4.0

static const unsigned char dd_actor_code[6] = { 0x8b, 0x88, 0x1b, 0x7f, 0x5c, 0x00 };
const uint32_t dd_proxy_sites[2] = { 0x001bf2a0u, 0x001bf400u };
int dd_mac_profile[5];        /* +0x0c +0x10 */
double dd_scale;
static int dd_proxy_peak = 1024;

/* +0x0c +0x10 +0x24 +0x50 */
__attribute__((cdecl, force_align_arg_pointer))
int gmv_create_proxy(char *bp, void *aabb, void *shape)
{
    int n = *(int *)(bp + 0x50), cap = *(int *)(uintptr_t)DD_MAX_PROXIES;
    if (n >= cap || *(int *)(bp + 0x24) == -1) {
        static int warned;
        if (!warned++)
            gmv_log("[gamepatch] collision proxies full (%d of %d): new shapes get no collision\n", n, cap);
        return -1;
    }
    if (n + 1 >= dd_proxy_peak) {
        dd_proxy_peak *= 2;
        gmv_trace("[gamepatch] collision in use: %d of %d proxies, %d of %d pairs\n", n + 1, cap,
                *(int *)(bp + 0x10), *(int *)(uintptr_t)DD_MAX_PAIRS);
    }
    return ((int (*)(char *, void *, void *))(uintptr_t)DD_PROXY_FN)(bp, aabb, shape);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_read_perf_profile(int *prof, void *stream)
{
    ((void (*)(int *, void *))(uintptr_t)DD_READ_FN)(prof, stream);
    dd_mac_profile[3] = prof[3];
    dd_mac_profile[4] = prof[4];
    prof[3] = (int)(prof[3] * dd_scale);
    prof[4] = (int)(prof[4] * dd_scale);
    gmv_log("[gamepatch] draw distance %.2gx: full detail %d -> %d, low detail %d -> %d (world units of about 1 cm)\n",
            dd_scale, dd_mac_profile[3], prof[3], dd_mac_profile[4], prof[4]);
}

int dd_on;
int dd_live;
int dd_tables_in;
patch_undo dd_undo, dd_tables_undo;

static void dd_sizes(double s, uint32_t *proxies, uint32_t *pairs)
{
    double area = s * s;
    if (tables_live() && area < DD_LIVE_AREA)
        area = DD_LIVE_AREA;
    int k = 1;
    while (k < area)
        k *= 2;
    *proxies = (uint32_t)(0x4000 * ceil(area));
    *pairs = 0x20000u * k;
}

/* 0x1a8c79 */
void dd_tables_fit(double s)
{
    uint32_t proxies, pairs, *sizes = (uint32_t *)(uintptr_t)DD_MAX_PROXIES;
    if (!dd_tables_in)
        return;
    dd_sizes(s, &proxies, &pairs);
    if (*(const uint32_t *)(uintptr_t)DD_PROXIES_IMM < proxies)
        patch_bytes(DD_PROXIES_IMM, (const unsigned char *)&proxies, 4);
    if (*(const uint32_t *)(uintptr_t)DD_PAIRS_IMM < pairs)
        patch_bytes(DD_PAIRS_IMM, (const unsigned char *)&pairs, 4);
    if (!((const int *)(uintptr_t)DD_PROFILE)[3])
        return;
    proxies = proxies > sizes[0] ? proxies : sizes[0];
    pairs = pairs > sizes[1] ? pairs : sizes[1];
    if (proxies == sizes[0] && pairs == sizes[1])
        return;
    uint32_t world = *(const uint32_t *)(uintptr_t)DD_WORLD;
    void *bp = world ? *(void **)(uintptr_t)(world + DD_WORLD_BP) : NULL;
    if (bp && !gmv_bp_grow(bp, proxies, pairs)) {
        gmv_log("[gamepatch] no memory for the larger collision tables; they stay at %u proxies\n", sizes[0]);
        return;
    }
    gmv_log("[gamepatch] collision tables now %u proxies, %u pairs (were %u, %u)\n", proxies, pairs, sizes[0], sizes[1]);
    sizes[0] = proxies, sizes[1] = pairs, sizes[2] = pairs, sizes[3] = pairs - 1;
}

int dd_set(double s)
{
    if (!dd_live)
        return 0;
    dd_scale = s < 1 ? 1 : s > DD_MAX ? DD_MAX : s;
    dd_tables_fit(dd_scale);
    if (!dd_mac_profile[3]) {
        gmv_log("[gamepatch] draw distance %.2gx from the city's load\n", dd_scale);
        return 1;
    }
    int *prof = (int *)(uintptr_t)DD_PROFILE;
    prof[3] = (int)(dd_mac_profile[3] * dd_scale);
    prof[4] = (int)(dd_mac_profile[4] * dd_scale);
    if (*(const uint8_t *)(uintptr_t)DD_RADIUS_SET)
        *(float *)(uintptr_t)DD_RADIUS = (float)prof[3];
    if (*(const uint8_t *)(uintptr_t)DD_KEEP_SET)
        *(float *)(uintptr_t)DD_KEEP = (float)prof[3] * *(const float *)(uintptr_t)DD_KEEP_K;
    gmv_log("[gamepatch] draw distance now %.2gx: full detail %d, low detail %d\n", dd_scale, prof[3], prof[4]);
    return 1;
}

void apply_draw_distance(void)
{
    const char *e = gmv_getenv("GMV_DRAW_DISTANCE");
    double s = e && *e ? atof(e) : 1;
    int live = tables_live();
    if (!(s > 1) && !live)
        return;
    dd_scale = s < 1 ? 1 : s > DD_MAX ? DD_MAX : s;
    uint32_t proxies = 0x4000, pairs = 0x20000;
    if (!sm_is_call(DD_READ_SITE, DD_READ_FN) ||
        memcmp((const void *)(uintptr_t)DD_ACTOR_SITE, dd_actor_code, sizeof dd_actor_code) ||
        (!dd_tables_in && (memcmp((const void *)(uintptr_t)DD_PROXIES_IMM, &proxies, 4) ||
                           memcmp((const void *)(uintptr_t)DD_PAIRS_IMM, &pairs, 4) ||
                           !sm_is_call(dd_proxy_sites[0], DD_PROXY_FN) || !sm_is_call(dd_proxy_sites[1], DD_PROXY_FN)))) {
        gmv_log("[gamepatch] profile read, actor removal or collision sizes not the expected bytes; "
                "draw distance skipped\n");
        return;
    }
    dd_sizes(dd_scale, &proxies, &pairs);
    if (!dd_tables_in) {
        patch_undo *keep = patch_undo_now;
        patch_undo_now = &dd_tables_undo;
        if (!patch_bytes(DD_PROXIES_IMM, (const unsigned char *)&proxies, 4) ||
            !patch_bytes(DD_PAIRS_IMM, (const unsigned char *)&pairs, 4)) {
            patch_undo_now = keep;
            gmv_log("[gamepatch] collision size write failed (%lu); draw distance skipped\n", GetLastError());
            return;
        }
        for (int i = 0; i < 2; i++)
            patch_call(dd_proxy_sites[i], (void *)gmv_create_proxy);
        patch_undo_now = keep;
        dd_tables_in = 1;
    }
    gmv_log("[gamepatch] collision sized for the draw distance: %u proxies, %u pairs (Mac: 16384, 131072)\n",
            proxies, pairs);
    unsigned char mov[6] = { 0xb9, 0, 0, 0, 0, 0x90 };          /* movl $dd_mac_profile,%ecx; nop */
    uint32_t p = (uint32_t)(uintptr_t)dd_mac_profile;
    memcpy(mov + 1, &p, 4);
    if (!patch_bytes(DD_ACTOR_SITE, mov, sizeof mov)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); draw distance skipped\n", DD_ACTOR_SITE, GetLastError());
        return;
    }
    if (!patch_call(DD_READ_SITE, (void *)gmv_read_perf_profile)) {
        patch_bytes(DD_ACTOR_SITE, dd_actor_code, sizeof dd_actor_code);
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); draw distance skipped\n", DD_READ_SITE + 1, GetLastError());
        return;
    }
    dd_on = 1;
}

/* updateCells +0x1a4 0x13d3f0 */
static const uint32_t dd_cells_old_r1[4] = { 0x0013d672u, 0x0013d708u, 0x0013d72fu, 0x0013d751u };   /* [ebp+0x10] */
static const uint32_t dd_cells_old_r12[4] = { 0x0013d822u, 0x0013d8b1u, 0x0013d8d8u, 0x0013d8f6u };  /* [ebp-0x7c] */

int dd_cells_aim(void)
{
    for (int i = 0; i < 4; i++)
        if (*(const uint8_t *)(uintptr_t)dd_cells_old_r1[i] != 0x10 ||
            *(const uint8_t *)(uintptr_t)dd_cells_old_r12[i] != 0x84)
            return 0;
    const unsigned char r1 = 0x18, r12 = 0x1c;   /* [ebp+0x18] [ebp+0x1c] */
    for (int i = 0; i < 4; i++)
        if (!patch_bytes(dd_cells_old_r1[i], &r1, 1) || !patch_bytes(dd_cells_old_r12[i], &r12, 1))
            return 0;
    return 1;
}
