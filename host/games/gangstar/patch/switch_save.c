#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xd00a8 0xf6b48 *0x689ea8 *0x689ea4 0xf3518 +0x754 */
static const uint32_t ssf_site[10] = {
    0x00006240u, 0x000326f1u, 0x000a1547u, 0x000aa9a6u, 0x000f3624u,
    0x000f6b33u, 0x0016471fu, 0x001648d6u, 0x00164a0bu, 0x00164bd6u,
};
#define SSF_WRITE_FN  0x00098148u
volatile int g_ssf_on, g_ssf_ok;
static const unsigned ssf_field[4] = { 0x754, 0x580, 0x75c, 0x7f4 };

int ssf_set(int on)
{
    if (on != g_ssf_on)
        gmv_log("[gamepatch] SWITCH_SAVE_FIX %s\n", on ? "on" : "off");
    g_ssf_on = on && g_ssf_ok;
    return g_ssf_ok || !on;
}

__attribute__((cdecl, force_align_arg_pointer))
uint32_t gmv_ssf_write(uint32_t save, uint32_t path)
{
    unsigned char *mainp = *(unsigned char **)(uintptr_t)SSF_MAIN_PP;
    unsigned char *ctrl = *(unsigned char **)(uintptr_t)SSF_CTRL_PP;
    uint32_t keep[4], r;
    if (!g_ssf_on || !g_ssf_ok || !mainp || !ctrl || mainp == ctrl)
        return pl_call2(SSF_WRITE_FN, save, path);
    if (*(uint32_t *)mainp != SSF_PLAYER_VT || *(uint32_t *)ctrl != SSF_PLAYER_VT) {
        gmv_log("[switchsave] the players are not the expected kind (%08x, %08x); saved as the game does\n",
                *(uint32_t *)mainp, *(uint32_t *)ctrl);
        return pl_call2(SSF_WRITE_FN, save, path);
    }
    for (int i = 0; i < 4; i++) {
        keep[i] = *(uint32_t *)(ctrl + ssf_field[i]);
        *(uint32_t *)(ctrl + ssf_field[i]) = *(uint32_t *)(mainp + ssf_field[i]);
    }
    r = pl_call2(SSF_WRITE_FN, save, path);
    for (int i = 0; i < 4; i++)
        *(uint32_t *)(ctrl + ssf_field[i]) = keep[i];
    gmv_trace("[switchsave] saved while playing as another character: Johnny's cash %d (not %d), outfit %d, "
            "armour and weapons kept\n", (int)*(uint32_t *)(mainp + 0x754), (int)keep[0],
            (int)*(uint32_t *)(mainp + 0x75c));
    return r;
}

/* GMV_RESERVE_START 0x6240 */
static uint32_t ssf_live(uint32_t a)
{
    return a < GMV_RESERVE_START ? a + GMV_LOW_DELTA : a;
}

void apply_switch_save_fix(void)
{
    g_ssf_on = !gmv_env_off(gmv_getenv("GMV_SWITCH_SAVE_FIX"));
    if (gmv_config_mac())
        return;
    const uint32_t mr = (uint32_t)(uintptr_t)gmv_mr_save;
    for (size_t i = 0; i < sizeof ssf_site / sizeof *ssf_site; i++)
        if (!sm_is_call(ssf_live(ssf_site[i]), SSF_WRITE_FN) && !sm_is_call(ssf_live(ssf_site[i]), mr)) {
            g_ssf_on = 0;
            gmv_log("[gamepatch] the save calls are not the expected ones (0x%08x); no SWITCH_SAVE_FIX\n", ssf_site[i]);
            return;
        }
    for (size_t i = 0; i < sizeof ssf_site / sizeof *ssf_site; i++)
        if (!sm_is_call(ssf_live(ssf_site[i]), mr) && !sm_redirect(ssf_live(ssf_site[i]), (void *)gmv_ssf_write)) {
            g_ssf_on = 0;
            gmv_log("[gamepatch] a write failed (%lu); no SWITCH_SAVE_FIX\n", GetLastError());
            return;
        }
    g_ssf_ok = 1;
    gmv_log("[gamepatch] SWITCH_SAVE_FIX %s (a save made as Betty keeps Johnny's cash, armour, outfit and weapons)\n",
            g_ssf_on ? "on" : "off");
}
