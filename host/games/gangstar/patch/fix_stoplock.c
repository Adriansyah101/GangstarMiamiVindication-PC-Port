#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* vtbl+0xa0 0x112bc4 app->[0x17ad8] 0x1132b2 [+0x184] */
#define STOPLOCK_LOAD  0x001132b2u   /* mov eax,[eax+0x17ad8] */
#define TICK_NOW_MS    0x00949adcu   /* now_ms 0x1b8aa0 +4 */

int stoplock_set(int on)
{
    static const unsigned char orig[6] = { 0x8b, 0x80, 0xd8, 0x7a, 0x01, 0x00 };
    unsigned char site[6] = { 0xa1 };                    /* mov eax,[moffs32]; nop */
    uint32_t now = TICK_NOW_MS;
    memcpy(site + 1, &now, 4);
    site[5] = 0x90;
    const void *p = (const void *)(uintptr_t)STOPLOCK_LOAD;
    if (memcmp(p, orig, sizeof orig) && memcmp(p, site, sizeof site))
        return -1;
    return patch_bytes(STOPLOCK_LOAD, on ? site : orig, sizeof site) ? on : -2;
}

void apply_stoplock_fix(void)
{
    if (gmv_env_off(gmv_getenv("GMV_STOPLOCK_FIX")))
        return;
    int r = stoplock_set(1);
    if (r == -1) {
        gmv_log("[gamepatch] stop-lock load 0x%08x not the expected bytes; fix skipped\n", STOPLOCK_LOAD);
        return;
    }
    if (r == -2) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); stop-lock fix skipped\n", STOPLOCK_LOAD, GetLastError());
        return;
    }
    gmv_log("[gamepatch] vehicle stop-lock timer fixed at 0x%08x: stamp = now_ms [0x%08x] instead of frame dt "
            "(STOPLOCK_FIX=0 to disable)\n", STOPLOCK_LOAD, TICK_NOW_MS);
}
