#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

void mac_features_in(void)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    apply_custom_radio();
    apply_cutscene_keys();
    apply_tutorial_keys();
    apply_pad_layout();
    apply_pad_menu();
    apply_pad_hints();
    apply_key_move();
    apply_switch_save_fix();
    apply_playerswitch_inventory();
    apply_lc_switch();
    apply_sun_moon();
    apply_fog();
    apply_sky_horizon();
    apply_checkpoint_glow();
    mr_save_hooks();
    tod_start_hook();
    patch_undo_now = keep;
    mac_features_on = 1;
}

/* 0xbefb5 */
static void sm_disc_remove(unsigned char **slot)
{
    unsigned char *n = *slot;
    if (!n)
        return;
    ((void (*)(void *))sm_vt(n + 4)[0x68 / 4])(n + 4);
    int top = *(int *)(*(unsigned char **)n - 0x10);
    if (--*(int *)(n + top + 4) == 0) {
        unsigned char *o = n + top;
        ((void (*)(void *))sm_vt(o)[8 / 4])(o);
        ((void (*)(void *))sm_vt(o)[4 / 4])(o);
    }
    *slot = NULL;
}

void mac_features_out(void)
{
    if (!mac_features_on)
        return;
    if (rp.phase != MR_IDLE) {
        if (!mac_out_pending)
            gmv_log("[gamepatch] a replay is under way: the hooks come out after it\n");
        mac_out_pending = 1;
        return;
    }
    mac_out_pending = 0;
    if (g_sm_hooked) {
        unsigned char *sky = *(unsigned char **)(uintptr_t)SM_SKYBOX_PP;
        if (sky) {
            sm_disc_remove((unsigned char **)(sky + 0x118));
            sm_disc_remove((unsigned char **)(sky + 0x11c));
        }
    }
    cr_page_mac();
    hide_main_menu_unhook();
    ds_show_all();
    int n = mac_undo.n;
    patch_undo_all(&mac_undo);
    mac_features_on = 0;
    g_cr_hooked = g_cs_hooked = g_tk_ok = g_pl_ok = g_pl_cam_hooked = g_ph_ok = g_km_ok = g_ssf_ok = 0;
    g_sm_hooked = fog_sky_wrapped = g_fog_all = 0;
    sky_k = 0;
    mr_unlock_prev = 0;
    phys_installed = g_phys_on = kick_tried = grip_tried = 0;
    cs_mgr_hooked = cs_installed = g_cam_sync = ws_installed = g_ws_on = as_installed = g_as_on = 0;
    gmv_log("[gamepatch] the other enhancements' hooks out too (%d patches): the bytes the game shipped with\n", n);
}

void gmv_apply_gamepatch(void)
{
#if GMV_DEV
    ma_snapshot();
#endif
    apply_vtxfix();
#if GMV_DEV
    apply_bakershare_log();
#endif
    apply_carsnd_trace();
    apply_engvol_trace();
    apply_engsrc_trace();
    apply_engdtor_trace();
    apply_frametick_probe();
    apply_fade_trace();
    apply_cine_trace();
    gmv_apply_broadphase();
    gmv_apply_b2solve();
    gmv_apply_hudbars();
    gmv_uifind_probe();
    gmv_b2step_probe();
    gmv_billboard_probe();
    gmv_apply_bbfast();
    gmv_vbostatic_install();
    apply_physics_sync();
    apply_cam_follow();
    apply_walk_sync();
    apply_ai_sync();
    apply_veh_trace();
    apply_crash58_guard();
    apply_stoplock_fix();
    apply_ai_steer();
    apply_boat_seat();
    apply_mission_save_fix();
    apply_menu_fade_clicks();
    apply_fade_edge_fix();
    apply_load_sleep_skip();
    apply_load_budget();
    apply_solo_car();
    apply_menu_loop();
    apply_radio_trace();
    apply_radio_logo_fix();
    apply_hide_more_games();
    apply_fast_load();
    patch_undo_now = &mac_undo;
    apply_short_warning();
    patch_undo_now = NULL;
    apply_weapon_sprint();
    apply_minimap_shops();
    dd_hooks_in();
    cr_file_wants();
    if (!gmv_config_mac())
        mac_features_in();
    crowd_hooks_in();
    apply_options_tab();
    apply_mission_replay();
    apply_uc_crusher();
    apply_radio_songs();
}
