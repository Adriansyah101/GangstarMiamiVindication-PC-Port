#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xcd8be 0xcda6c 0x283b0 [[app+0x17ac8]+0x18] 0x19d72 */
#define CINE_LOCK_FN   0x000283b0u   /* [obj+0x18] 0x19d72 */

static const uint32_t cine_lock_sites[] = {
    0x000cd908u, 0x000cd97cu,        /* CineFadeIn 0xcd8be */
    0x000cdae1u, 0x000cdb7fu,        /* CineFadeOut 0xcda6c */
};

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cine_fade_lock(unsigned char *lock)
{
    void *(*fe_get)(void)  = (void *(*)(void))(uintptr_t)0x35d08u;
    int   (*fe_tr)(void *) = (int (*)(void *))(uintptr_t)0x32de0u;
    if (gmv_top_state_vtable() == MAIN_MENU_VT) {
        void *fe = fe_get();
        if (fe && !(fe_tr(fe) & 0xff)) {
            lock[0x18] = 1;
            return;
        }
    }
    ((void (*)(unsigned char *))(uintptr_t)CINE_LOCK_FN)(lock);
}

/* gmv_cine_fade_lock 0x283b0 */
int menu_fade_clicks_set(int on)
{
    const uint32_t ours = (uint32_t)(uintptr_t)gmv_cine_fade_lock, to = on ? ours : CINE_LOCK_FN;
    for (size_t i = 0; i < sizeof cine_lock_sites / sizeof *cine_lock_sites; i++) {
        uint32_t now = code_call_target(cine_lock_sites[i]);
        if (now != CINE_LOCK_FN && now != ours)
            return -1;
    }
    for (size_t i = 0; i < sizeof cine_lock_sites / sizeof *cine_lock_sites; i++) {
        if (!patch_call(cine_lock_sites[i], (const void *)(uintptr_t)to))
            return -2;
    }
    return on;
}

void apply_menu_fade_clicks(void)
{
    if (gmv_env_off(gmv_getenv("GMV_MENU_FADE_CLICKS")))
        return;
    int r = menu_fade_clicks_set(1);
    if (r == -1)
        gmv_log("[gamepatch] fader calls (0x%08x..) not the expected calls to 0x%08x; menu fade clicks skipped\n",
                cine_lock_sites[0], CINE_LOCK_FN);
    else if (r == -2)
        gmv_log("[gamepatch] a write failed (%lu); menu fade clicks partly applied\n", GetLastError());
    else
        gmv_log("[gamepatch] main menu takes clicks during its background fades (fader input reset skipped on the "
                "main menu; MENU_FADE_CLICKS=0 to disable)\n");
}
