#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* mgr+0x90 0x8d018 0x98148 scripts+0x3a8 0x33400 0x977ac */
#define MSF_SITE  0x000986d4u

static int msf_set(int on)
{
    static const unsigned char orig[8] = { 0x39, 0xb8, 0xa8, 0x03, 0x00, 0x00, 0x7f, 0xd4 };   /* cmp [eax+0x3a8],edi; jg */
    static const unsigned char fixd[8] = { 0x83, 0xff, 0x08, 0x90, 0x90, 0x90, 0x7c, 0xd4 };   /* cmp edi,8; nop; jl */
    const void *p = (const void *)(uintptr_t)MSF_SITE;
    if (memcmp(p, orig, sizeof orig) && memcmp(p, fixd, sizeof fixd))
        return -1;
    return patch_bytes(MSF_SITE, on ? fixd : orig, sizeof orig) ? on : -2;
}

void apply_mission_save_fix(void)
{
    if (gmv_env_off(gmv_getenv("GMV_MISSION_SAVE_FIX")))
        return;
    int r = msf_set(1);
    if (r == -1)
        gmv_log("[gamepatch] mission flags' save loop 0x%08x not the expected bytes; MISSION_SAVE_FIX skipped\n", MSF_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); MISSION_SAVE_FIX skipped\n", MSF_SITE, GetLastError());
    else
        gmv_log("[gamepatch] MISSION_SAVE_FIX on: the save keeps all 8 groups of mission flags (Reginald's included)\n");
}
