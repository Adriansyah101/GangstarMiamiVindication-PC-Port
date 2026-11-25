#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* Vehicle::getSpeed 0x101f74 [+0x3d4] */
#define CARSND_GETSPEED 0x00101f74u

volatile float g_carsnd_last_speed;

__attribute__((cdecl, force_align_arg_pointer))
static float gmv_carsnd_getspeed(void *thisp)
{
    float raw = thisp ? *(const float *)((const char *)thisp + 0x3d4) : 0.0f;
    float k1  = *(const float *)(uintptr_t)0x005f1150u;
    float k2  = *(const float *)(uintptr_t)0x005f1818u;
    float spd = raw / k1 * k2;                              /* getSpeed @0x101f74 */
    g_carsnd_last_speed = spd;

    void *caller = __builtin_return_address(0);
    static void *seen[48];
    static unsigned nseen;
    int known = 0;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == caller) { known = 1; break; }
    if (!known && nseen < 48) {
        seen[nseen++] = caller;
        gmv_log("[carsnd] getSpeed caller #%u = %p  (speed=%.2f km/h, this=%p)\n",
                nseen, caller, (double)spd, thisp);
    }

    static unsigned n;
    static float smax = 0.0f;
    int newmax = spd > smax + 1.0f;
    if (newmax) smax = spd;
    if (newmax || (n++ % 90) == 0)
        gmv_log("[carsnd] getSpeed=%.2f km/h (max %.2f) rawField[+0x3d4]=%.4f this=%p\n",
                (double)spd, (double)smax, (double)raw, thisp);
    return spd;
}

void apply_carsnd_trace(void)
{
    const char *e  = gmv_dbgenv("GMV_CARSND_TRACE");
    const char *e2 = gmv_dbgenv("GMV_ENGSRC_TRACE");
    if (!((gmv_env_on(e)) || (gmv_env_on(e2))))
        return;
    /* push ebp; mov ebp,esp; sub esp,4; call; pop eax */
    static const unsigned char expect[8] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x04, 0xe8, 0x00 };
    if (memcmp((const void *)(uintptr_t)CARSND_GETSPEED, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] getSpeed 0x%08x not the expected bytes; car-sound trace skipped\n",
                CARSND_GETSPEED);
        return;
    }
    if (!patch_jmp(CARSND_GETSPEED, (void *)&gmv_carsnd_getspeed, 5)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); car-sound trace skipped\n",
                CARSND_GETSPEED, GetLastError());
        return;
    }
    gmv_log("[gamepatch] car-sound trace installed: Vehicle::getSpeed -> host hook %p "
            "([carsnd] logs vehicle speed while driving)\n", (void *)&gmv_carsnd_getspeed);
}

/* Vehicle::aiSetDestination 0x101fee [+0x220] */
#define VEH_AISETDEST 0x00101feeu

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_veh_aisetdest(void *thisp, void *dest)
{
    if (thisp)
        *(void **)((char *)thisp + 0x220) = dest;   /* aiSetDestination [0x220] */

    static void *vt[64], *vd[64];
    static unsigned vn;
    int logit = 1;
    unsigned i;
    for (i = 0; i < vn; i++)
        if (vt[i] == thisp) break;
    if (i < vn) { logit = (vd[i] != dest); vd[i] = dest; }
    else if (vn < 64) { vt[vn] = thisp; vd[vn] = dest; vn++; }
    if (logit)
        gmv_log("[veh] aiSetDestination this=%p dest=%p %s caller=%p\n",
                thisp, dest, dest ? "SET" : "CLEAR", __builtin_return_address(0));
}

void apply_veh_trace(void)
{
    const char *e = gmv_dbgenv("GMV_VEH_TRACE");
    if (!(gmv_env_on(e)))
        return;
    /* push ebp; mov ebp,esp; mov eax,[ebp+0xc] */
    static const unsigned char expect[8] = { 0x55, 0x89, 0xe5, 0x8b, 0x45, 0x0c, 0x8b, 0x4d };
    if (memcmp((const void *)(uintptr_t)VEH_AISETDEST, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] aiSetDestination 0x%08x not the expected bytes; veh trace skipped\n",
                VEH_AISETDEST);
        return;
    }
    if (!patch_jmp(VEH_AISETDEST, (void *)&gmv_veh_aisetdest, 5)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); veh trace skipped\n",
                VEH_AISETDEST, GetLastError());
        return;
    }
    gmv_log("[gamepatch] veh trace installed: Vehicle::aiSetDestination -> host hook %p "
            "([veh] logs AI drive-to set/clear per vehicle)\n", (void *)&gmv_veh_aisetdest);
}
