#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* PAD_MENU [[0x63e038]+0xd8] 0x181d70 0x181d14 */
static int g_pm_on;

int pm_set(int on)
{
    if (on != g_pm_on)
        gmv_log("[gamepatch] PAD_MENU %s\n", on ? "on" : "off");
    g_pm_on = on;
    return 1;
}

int pointer_shown(void)
{
    const unsigned char *g1, *cur;
    g1 = *(const unsigned char *const *)(uintptr_t)*(const uint32_t *)(uintptr_t)0x63e038u;
    cur = g1 ? *(const unsigned char *const *)(g1 + 0xd8) : NULL;
    return cur && (cur[4] || cur[5]);
}

int gmv_pointer_up(void)
{
    const uint32_t *cell = (const uint32_t *)(uintptr_t)*(const uint32_t *)(uintptr_t)0x63e038u;
    const unsigned char *g1 = cell ? (const unsigned char *)(uintptr_t)*cell : NULL;
    const unsigned char *cur = g1 ? *(const unsigned char *const *)(g1 + 0xd8) : NULL;
    return !cur || cur[4] || cur[5];
}

int gmv_pad_menu_active(void)
{
    return g_pm_on && pointer_shown();
}

void apply_pad_menu(void)
{
    const char *e = gmv_getenv("GMV_PAD_MENU");
    g_pm_on = gmv_env_on(e);
    if (g_pm_on)
        gmv_log("[gamepatch] PAD_MENU on: the pad moves the menu pointer (stick / D-pad), A clicks, B = Esc\n");
}
