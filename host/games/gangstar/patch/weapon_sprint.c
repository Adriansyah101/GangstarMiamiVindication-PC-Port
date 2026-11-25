#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x11e002 +0x666 vt[0x78](1) +0x48 0x11e269 0x11e2cc */

void apply_weapon_sprint(void)
{
    const char *e = gmv_getenv("GMV_ALLOW_WEAPON_SPRINT");
    if (!(gmv_env_on(e)))
        return;
    static const unsigned char press_expect[3] = { 0x8b, 0x75, 0xf0 }, release_expect[3] = { 0x8b, 0x46, 0x48 };
    if (memcmp((const void *)(uintptr_t)WS_PRESS_SITE, press_expect, 3) != 0 ||
        memcmp((const void *)(uintptr_t)WS_RELEASE_SITE, release_expect, 3) != 0) {
        gmv_log("[gamepatch] sprint handler not the expected bytes; sprinting still puts the weapon away\n");
        return;
    }
    static const unsigned char press_jmp[2] = { 0xeb, 0x61 }, release_jmp[2] = { 0xeb, 0x1c };
    if (!patch_bytes(WS_PRESS_SITE, press_jmp, 2) || !patch_bytes(WS_RELEASE_SITE, release_jmp, 2))
        gmv_log("[gamepatch] write failed (%lu); sprinting may still put the weapon away\n", GetLastError());
    else
        gmv_log("[gamepatch] the weapon stays in hand while sprinting (ALLOW_WEAPON_SPRINT=0 for the Mac's fists)\n");
}
