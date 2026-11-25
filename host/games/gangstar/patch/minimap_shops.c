#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* +0x9c +0x5c 0x46eda 0x239ce 0x4623a 0x15-0x16 0x4679d */
#define MS_SITE 0x0004679du

static const unsigned char ms_orig[17] = {
    0x8a, 0x04, 0x01, 0x88, 0xc1, 0x80, 0xc1, 0xeb, 0x80, 0xf9, 0x02, 0x0f, 0x82, 0x16, 0x01, 0x00, 0x00 };
static const unsigned char ms_new[17] = {
    0x8a, 0x04, 0x01, 0x3c, 0x16, 0x0f, 0x84, 0x1c, 0x01, 0x00, 0x00, 0x3c, 0x15, 0x74, 0x73, 0x90, 0x90 };

int minimap_shops_set(int on)
{
    const void *p = (const void *)(uintptr_t)MS_SITE;
    if (memcmp(p, ms_orig, sizeof ms_orig) && memcmp(p, ms_new, sizeof ms_new))
        return 0;
    return patch_bytes(MS_SITE, on ? ms_new : ms_orig, sizeof ms_orig);
}

void apply_minimap_shops(void)
{
    const char *e = gmv_getenv("GMV_MINIMAP_SHOPS");
    if (!(gmv_env_on(e)))
        return;
    if (minimap_shops_set(1))
        gmv_log("[gamepatch] the minimap shows the shops near you (MINIMAP_SHOPS=0 for the Mac's minimap)\n");
    else
        gmv_log("[gamepatch] minimap draw loop not the expected bytes (or write failed, %lu); no shops on the minimap\n",
                GetLastError());
}
