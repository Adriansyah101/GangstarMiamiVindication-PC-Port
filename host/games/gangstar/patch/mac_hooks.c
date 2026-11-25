#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

patch_undo pd_undo;
int dd_tables_back;

void dd_hooks_in(void)
{
    patch_undo_now = &dd_undo;
    apply_draw_distance();
    apply_origin_collision_fix();
    patch_undo_now = NULL;
}

void crowd_hooks_in(void)
{
    patch_undo_now = &pd_undo;
    apply_spawn_hook();
    apply_spawn_fade();
    patch_undo_now = NULL;
}

void mac_hooks_in(void)
{
    dd_unhook = dd_tables_back = 0;
    if (!dd_on) {
        const int *prof = (const int *)(uintptr_t)DD_PROFILE;
        if (prof[3])
            dd_mac_profile[3] = prof[3], dd_mac_profile[4] = prof[4];
        oc_seed_r1 = *(const uint8_t *)(uintptr_t)DD_RADIUS_SET ? *(const float *)(uintptr_t)DD_RADIUS : 0;
        oc_seed_r2 = *(const uint8_t *)(uintptr_t)DD_KEEP_SET ? *(const float *)(uintptr_t)DD_KEEP : 0;
        dd_hooks_in();
    }
    dd_tables_fit(dd_scale);
    mac_out_pending = 0;
    if (!mac_features_on)
        mac_features_in();
    if (gmv_uc_on())
        uc_hooks_in();
    if (!pd_on) {
        sf_menu = -1;
        crowd_hooks_in();
        pd_pool_top_up();
    }
}

void mac_hooks_out(void)
{
    if (pd_on || sf_hooked) {
        if (pd_pool_on)
            pd_unmove_pool();
        patch_undo_all(&pd_undo);
        pd_on = pd_centres_on = pd_pool_on = pd_racing = sf_hooked = sf_live = sf_n = 0;
        pd_trim_gen = pd_world_gen;
        gmv_log("[gamepatch] density and SPAWN_FADE hooks out, the spawn pool back in its 256 slots (%d of our "
                "copies still in the city)\n", pd_extra_n);
    }
    if (dd_on)
        dd_unhook = 1;
    dd_tables_back = dd_tables_in;
    mac_features_out();
}

static void dd_tables_mac(void)
{
    uint32_t *sizes = (uint32_t *)(uintptr_t)DD_MAX_PROXIES;
    patch_undo_all(&dd_tables_undo);
    sizes[0] = 0x4000, sizes[1] = 0x20000, sizes[2] = 0x20000, sizes[3] = 0x1ffff;
    dd_tables_in = dd_tables_back = 0;
    gmv_log("[gamepatch] collision tables the Mac's again: 16384 proxies, 131072 pairs\n");
}

void gmv_b2_world_new(void)
{
    pd_world_gen++;
    gmv_trace("[gamepatch] a new physics world (%d so far), the collision tables %s\n", pd_world_gen,
            dd_tables_back ? "go back to the Mac's" : "stay as they are");
    if (dd_unhook)
        dd_take_out();
    if (dd_tables_back)
        dd_tables_mac();
}

void dd_tables_tick(void)
{
    static int told_wait, told_no;
    if (!dd_tables_back || dd_unhook)
        return;
    uint32_t world = *(const uint32_t *)(uintptr_t)DD_WORLD;
    void *bp = world ? *(void **)(uintptr_t)(world + DD_WORLD_BP) : NULL;
    if (!bp || !((const int *)(uintptr_t)DD_PROFILE)[3])
        return;
    int r = *(int *)((char *)bp + 0x50) > 0x4000 - 0x400 ? 0 : gmv_bp_shrink(bp, 0x4000, 0x20000);
    if (r > 0) {
        told_wait = told_no = 0;
        dd_tables_mac();
    } else if (r == 0 && !told_wait++) {
        gmv_log("[gamepatch] collision tables: waiting for what is in use to fit the Mac's (%d proxies, %d pairs, "
                "%d buffered)\n", *(int *)((char *)bp + 0x50), *(int *)((char *)bp + 0x10), *(int *)((char *)bp + 0x18));
    } else if (r < 0 && !told_no++) {
        gmv_log("[gamepatch] collision tables: no memory or not what was expected; the Mac's sizes wait for the "
                "next start\n");
    }
}
