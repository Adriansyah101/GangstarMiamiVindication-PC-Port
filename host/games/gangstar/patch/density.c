#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

#define SV_FN        0x00080272u

/* 0x082504 0x0828f8 0x4c +0x2a8 0x07fcea 0x64ef88 */
#define PD_MAX       4.0
#define PD_TOTAL_IMM 0x000803f3u
#define PD_CENTRE_SITE 0x001a2396u   /* call 0x082c12 */
#define PD_CENTRE_FN   0x00082c12u
static const uint32_t pd_centres[3][2] = {
    {0x00688cc0u, 0x00688cf0u}, {0x00689120u, 0x00689130u}, {0x00688ce0u, 0x00688d10u},
};
static float pd_centre_a[3][2];
#define PD_KEEP_SITE 0x001a93adu   /* 0x1a8f6a 0x5f6ea0 PIC 0x1a8f7b */
#define PD_KEEP_BASE 0x001a8f7bu
#define PD_KEEP_OLD  0x005f6ea0u
#define PD_KEEP      1.2
static float pd_keep2;

const pd_kinds_t pd_kinds[3] = {
    {"GMV_TRAFFIC_DENSITY",    3, 18, 0x1c, 0x000802ecu, 0x006426e0u, 400, 800},
    {"GMV_PEDESTRIAN_DENSITY", 8, 14, 0x20, 0x00080326u, 0x00642820u, 500, 1000},
    {"GMV_PARKED_CAR_DENSITY", 17, 10, 0x24, 0x00080309u, 0x00642760u, 400, 800},
};
double pd_scale[3] = {1, 1, 1};
double pd_user[3] = {1, 1, 1};
int pd_on, pd_racing;
#define PD_RACE_MGR  0x00689e98u   /* 0xf2ed0 +8 */

static uint32_t pd_rd(uint32_t a)
{
    return (a > 0x1000 && !IsBadReadPtr((void *)(uintptr_t)a, 4)) ? *(const uint32_t *)(uintptr_t)a : 0;
}

static int pd_race_on(void)
{
    uint32_t mgr = *(const uint32_t *)(uintptr_t)PD_RACE_MGR, race = mgr ? *(const uint32_t *)(uintptr_t)(mgr + 8) : 0;
    int st = race ? *(const int *)(uintptr_t)(race + 0x14) : 0;
    return st == 1 || st == 2;
}

int pd_write(void)
{
    int total = 48;
    for (int i = 0; i < 3; i++) {
        int cap = (int)(pd_kinds[i].cap * pd_scale[i] + 0.5);
        total += cap - pd_kinds[i].cap;
        if (!patch_bytes(pd_kinds[i].cap_imm, (const unsigned char *)&cap, 4))
            return -1;
    }
    unsigned char t = (unsigned char)(total < 0 ? 0 : total > 127 ? 127 : total);
    patch_bytes(PD_TOTAL_IMM, &t, 1);
    int more = 0;
    for (int i = 0; i < 3; i++)
        more |= pd_scale[i] > 1;
    double keep = pd_racing || !more ? 1 : PD_KEEP;
    pd_keep2 = (float)(14000 * keep * 14000 * keep);
    return t;
}

static void pd_follow_race(void)
{
    if (!pd_on || pd_race_on() == pd_racing)
        return;
    pd_racing = !pd_racing;
    for (int i = 0; i < 3; i++)
        pd_scale[i] = pd_racing ? 1 : pd_user[i];
    if (pd_write() < 0)
        gmv_log("[gamepatch] population density write failed (%lu)\n", GetLastError());
    else
        gmv_log("[gamepatch] population density: %s\n", pd_racing ? "race on, the Mac's city until it ends"
                                                                  : "race over, the densities are back");
}

static int pd_cap(int v, int k)
{
    int n = (int)(v * pd_scale[k] + 0.5), most = (int)(pd_kinds[k].cap * pd_scale[k] + 0.5);
    return v < 0 ? v : n < most ? n : most;
}

static void pd_set_band(double s)
{
    if (s < 1)
        s = 1;
    for (int i = 0; i < 3; i++) {
        float *a = (float *)(uintptr_t)pd_centres[i][0];
        const float *b = (const float *)(uintptr_t)pd_centres[i][1];
        for (int j = 0; j < 2; j++)
            a[j] = (float)(b[j] + (pd_centre_a[i][j] - b[j]) * s);
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_spawn_centres(uint32_t arg)
{
    ((void (*)(uint32_t))(uintptr_t)PD_CENTRE_FN)(arg);
    pd_follow_race();
    for (int i = 0; i < 3; i++) {
        const float *a = (const float *)(uintptr_t)pd_centres[i][0];
        pd_centre_a[i][0] = a[0], pd_centre_a[i][1] = a[1];
    }
    double s = 1;
    for (int k = 0; k < 3; k++)
        if (pd_scale[k] > s)
            s = pd_scale[k];
    pd_set_band(s);
}

int sf_hooked;
__attribute__((cdecl, force_align_arg_pointer, noinline))
int gmv_spawn_make(uint32_t sp, uint32_t loc)
{
    if (sf_hooked)
        sf_before_make();
    int r = ((int (*)(uint32_t, uint32_t))(uintptr_t)PD_MAKE_FN)(sp, loc);
    if (sf_hooked)
        sf_after_make();
    return r;
}

#define PD_BUILD_SITE 0x001ad591u   /* call 0x0819e8 */
#define PD_BUILD_FN   0x000819e8u
#define PD_NEW_FN     0x000220d4u
#define PD_TYPE_FN    0x00026e1eu
#define PD_FREE_FN    0x00090430u
#define PD_CLEAR_SIZE 0x0007fb31u   /* 0x400 */
static const uint32_t pd_pool_refs[10][2] = {
    {0x0007f9d5u, 0x0007f9d2u}, {0x0007fb26u, 0x0007f9d2u}, {0x0008199au, 0x00081969u},
    {0x00081b0bu, 0x000819f9u}, {0x00081bc3u, 0x00081bb0u}, {0x00081e6du, 0x00081c49u},
    {0x00082856u, 0x00082515u}, {0x00083244u, 0x00083017u}, {0x0008393bu, 0x00083017u},
    {0x00084160u, 0x0008407eu},
};
static const uint32_t pd_pool_bounds[8] = {     /* 0x100 */
    0x0007f9dau, 0x000819dau, 0x00081bd6u, 0x00081e86u, 0x00082866u, 0x00083256u, 0x00083956u, 0x00084176u,
};
uint32_t pd_pool[PD_POOL_N];
int pd_pool_on;

double pd_built[3];
static uint32_t pd_extra[PD_POOL_N];
int pd_extra_n;
static int pd_trim;

#if GMV_DEV
#define PD_LEFT_N 8192
static uint32_t pd_left[PD_LEFT_N];
static int pd_left_n;
#endif

static void pd_let_go(uint32_t o)
{
#if GMV_DEV
    if (pd_left_n < PD_LEFT_N)
        pd_left[pd_left_n++] = o;
#else
    (void)o;
#endif
}

static double pd_pool_scale(uint32_t vt)
{
    double s = vt == 0x0064ef88u ? pd_built[1]
             : vt == 0x00656068u || vt == 0x00656228u ? (pd_built[0] > pd_built[2] ? pd_built[0] : pd_built[2])
             : 1;
    return s < 1 ? 1 : s;
}

static void pd_built_read(void)
{
    for (int i = 0; i < 3; i++) {
        const char *f = tab_live() ? gmv_config_file_value(pd_kinds[i].env + 4) : NULL;
        double v = f && *f ? atof(f) : 1;
        v = v > PD_MAX ? PD_MAX : v;
        pd_built[i] = v > pd_user[i] ? v : pd_user[i];
    }
}

static int pd_pool_add_more(const int *made, const int *have, const uint32_t *vt, int *full)
{
    uint32_t world = pd_rd(SV_WORLD);
    int slot = 0, added = 0;
    for (int t = 0; t < 256 && slot < PD_POOL_N; t++)
        for (int extra = (int)(made[t] * pd_pool_scale(vt[t]) + 0.5) - made[t] - have[t]; extra > 0; extra--) {
            while (slot < PD_POOL_N && pd_pool[slot])
                slot++;
            if (slot == PD_POOL_N || pd_extra_n == PD_POOL_N)
                break;
            /* +0x1c 0xb4 */
            uint32_t o = ((uint32_t (*)(uint32_t, int))(uintptr_t)PD_NEW_FN)(world, t);
            if (!o)
                break;
            *(uint8_t *)(uintptr_t)(o + 5) |= 1;
            ((int (*)(uint32_t))(uintptr_t)PD_TYPE_FN)(o);
            uint32_t *res = (uint32_t *)(uintptr_t)(o + 0x1c);
            if (*res)
                ((void (*)(uint32_t))(uintptr_t)PD_FREE_FN)(*res);
            *res = 0;
            ((void (*)(uint32_t))(uintptr_t)pd_rd(pd_rd(o) + 0xb4))(o);
            pd_pool[slot] = o, pd_extra[pd_extra_n++] = o, added++;
        }
    *full = slot == PD_POOL_N;
    return added;
}

static int pd_pool_add(const int *made, const uint32_t *vt, int *full)
{
    static const int none[256];
    return pd_pool_add_more(made, none, vt, full);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pool_build(void)
{
    static uint32_t before[PD_POOL_N];
    pd_built_read();
    pd_extra_n = 0;
    memcpy(before, pd_pool, sizeof pd_pool);
    ((void (*)(void))(uintptr_t)PD_BUILD_FN)();
    int made[256] = {0}, n = 0, added = 0;
    uint32_t vt[256] = {0};
    for (int i = 0; i < PD_POOL_N; i++)
        if (pd_pool[i] && pd_pool[i] != before[i]) {
            int t = ((int (*)(uint32_t))(uintptr_t)PD_TYPE_FN)(pd_pool[i]);
            n++;
            if (t >= 0 && t < 256)
                made[t]++, vt[t] = pd_rd(pd_pool[i]);
        }
    int full = 0;
    added = pd_pool_add(made, vt, &full);
    gmv_log("[gamepatch] population density: the spawn pool holds %d objects, %d more for the densities "
            "(room for %d)%s\n", n, added, PD_POOL_N, full ? "; FULL" : "");
}

int pd_is_extra(uint32_t o)
{
    for (int i = 0; i < pd_extra_n; i++)
        if (pd_extra[i] == o)
            return 1;
    return 0;
}

int pd_object_known(uint32_t o)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    uint32_t world = pd_rd(SV_WORLD);
    for (int l = 0; world && l < 3; l++) {
        uint32_t x = *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; x && i < 8192; i++, x = *(const uint32_t *)(uintptr_t)(x + 0xc))
            if (x == o)
                return 1;
    }
    for (int i = 0; i < PD_POOL_N; i++)
        if (pd_pool[i] == o)
            return 1;
    for (int i = 0; i < 0x100; i++)
        if (((const uint32_t *)(uintptr_t)PD_POOL_OLD)[i] == o)
            return 1;
    return 0;
}

/* +0xc8 0x10f0c0 0x10a613 */
void cr_cars_off_sixth(void)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    uint32_t world = pd_rd(SV_WORLD);
    const uint32_t *pool = pd_pool_on ? pd_pool : (const uint32_t *)(uintptr_t)PD_POOL_OLD;
    int n = pd_pool_on ? PD_POOL_N : 0x100;
    char (*is_vehicle)(uint32_t) = (char (*)(uint32_t))(uintptr_t)0x00020c6au;
    for (int i = 0; i < n; i++)
        if (pool[i] && is_vehicle(pool[i]) && (int)pd_rd(pool[i] + 0xc8) >= CR_GAME_STATIONS)
            *(int *)(uintptr_t)(pool[i] + 0xc8) = 0;
    for (int l = 0; world && l < 3; l++) {
        uint32_t o = *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; o && i < 8192; i++, o = *(const uint32_t *)(uintptr_t)(o + 0xc))
            if (is_vehicle(o) && (int)pd_rd(o + 0xc8) >= CR_GAME_STATIONS)
                *(int *)(uintptr_t)(o + 0xc8) = 0;
    }
}

void pd_pool_top_up(void)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    int made[256] = {0}, have[256] = {0}, full = 0;
    uint32_t vt[256] = {0}, world = pd_rd(SV_WORLD);
    pd_built_read();
    pd_trim = 0;
    if (!world || !pd_pool_on)
        return;
    for (int l = 0; l <= 3; l++) {
        uint32_t o = l == 3 ? 0 : *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; i < (l == 3 ? PD_POOL_N : 8192); i++) {
            if (l == 3)
                o = pd_pool[i];
            else if (!o)
                break;
            if (o && (l == 3 || (*(const uint8_t *)(uintptr_t)(o + 5) & 1))) {
                int t = ((int (*)(uint32_t))(uintptr_t)PD_TYPE_FN)(o);
                if (t >= 0 && t < 256) {
                    if (pd_is_extra(o))
                        have[t]++;
                    else
                        made[t]++, vt[t] = pd_rd(o);
                }
            }
            if (l != 3)
                o = *(const uint32_t *)(uintptr_t)(o + 0xc);
        }
    }
    int added = pd_pool_add_more(made, have, vt, &full);
    gmv_log("[gamepatch] population density: %d more objects in the spawn pool for the densities (%d there "
            "already)%s\n", added, pd_extra_n - added, full ? "; FULL" : "");
}

void pd_unmove_pool(void)
{
    uint32_t *old = (uint32_t *)(uintptr_t)PD_POOL_OLD;
    int at = 0;
    memset(old, 0, 0x100 * sizeof *old);
    for (int i = 0; i < PD_POOL_N; i++) {
        uint32_t o = pd_pool[i];
        if (!o)
            continue;
        pd_pool[i] = 0;
        if (pd_is_extra(o) || at == 0x100) {
            for (int e = 0; e < pd_extra_n; e++)
                if (pd_extra[e] == o)
                    pd_extra[e] = pd_extra[--pd_extra_n];
            pd_let_go(o);
        } else
            old[at++] = o;
    }
    pd_trim = pd_extra_n > 0;
}

#if GMV_DEV
void pd_ws_left(uint32_t ch)
{
    for (int i = 0; i < pd_left_n; i++)
        if (pd_left[i] == ch) {
            gmv_log("[pool] copy %08x was let go but is walking: vtable %08x, +4 %08x, the world's or a pool's %d\n",
                    ch, pd_rd(ch), pd_rd(ch + 4), pd_object_known(ch));
            pd_left[i] = pd_left[--pd_left_n];
            return;
        }
}

void pd_ws_report(uint32_t ch, uint32_t vt_was)
{
    static const uint32_t lists[3] = {0x418, 0x424, 0x430};
    static int on = -1;
    if (on < 0)
        on = gmv_env_on(gmv_dbgenv("GMV_POOL_PROBE"));
    if (!on)
        return;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((const void *)(uintptr_t)(ch + 0x514), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
        return;
    uint32_t world = pd_rd(SV_WORLD);
    int in_list = -1, in_pool = 0, in_old = 0;
    for (int l = 0; world && l < 3 && in_list < 0; l++) {
        uint32_t o = *(const uint32_t *)(uintptr_t)(world + lists[l] + 4);
        for (int i = 0; o && i < 8192; i++, o = *(const uint32_t *)(uintptr_t)(o + 0xc))
            if (o == ch)
                in_list = l;
    }
    for (int i = 0; i < PD_POOL_N; i++)
        in_pool |= pd_pool[i] == ch;
    for (int i = 0; i < 0x100; i++)
        in_old |= ((const uint32_t *)(uintptr_t)PD_POOL_OLD)[i] == ch;
    gmv_log("[pool] walker %08x gone during its ray: vtable was %08x, now %08x; in the world's list %d, our pool %d, "
            "the Mac's pool %d, one of our copies now %d\n",
            ch, vt_was, pd_rd(ch), in_list, in_pool, in_old, pd_is_extra(ch));
}
#endif

int pd_world_gen, pd_trim_gen;
static int pd_tick_n = -1;

void gmv_pool_tick(void)
{
    pd_tick_n = -1;
    if (pd_trim) {
        uint32_t *old = (uint32_t *)(uintptr_t)PD_POOL_OLD;
        if (pd_trim_gen != pd_world_gen)
            pd_extra_n = 0;
        for (int i = 0; i < 0x100 && pd_extra_n; i++)
            for (int e = 0; old[i] && e < pd_extra_n; e++)
                if (pd_extra[e] == old[i]) {
                    pd_let_go(old[i]);
                    pd_extra[e] = pd_extra[--pd_extra_n];
                    old[i] = 0;
                }
        pd_trim = pd_extra_n > 0;
    }
#if GMV_DEV
    static int peak[3];
    static DWORD next;
    static const int kind[3] = {3, 8, 17};
    const int *cnt = (const int *)(uintptr_t)pd_rd(pd_rd(SV_WORLD) + 0x414);
    if (!cnt)
        return;
    for (int k = 0; k < 3; k++)
        if (cnt[kind[k]] > peak[k])
            peak[k] = cnt[kind[k]];
    DWORD now = GetTickCount();
    if ((int)(now - next) >= 0) {
        int free_n = 0, n = pd_pool_on ? PD_POOL_N : 0x100;
        const uint32_t *p = pd_pool_on ? pd_pool : (const uint32_t *)(uintptr_t)PD_POOL_OLD;
        for (int i = 0; i < n; i++)
            free_n += p[i] != 0;
        if (next && (peak[0] || peak[1] || peak[2]))
            gmv_log("[crowd] last 15 s, most at once: traffic %d, people and drivers %d, parked %d; now in the pool "
                    "%d (of %d slots), our copies %d, night cap %s\n", peak[0], peak[1], peak[2], free_n, n, pd_extra_n,
                    pd_rd(0x0068a104u) == 3 ? "on" : "off");
        peak[0] = peak[1] = peak[2] = 0;
        next = now + 15000;
    }
#endif
}

static int pd_move_pool(void)
{
    for (int i = 0; i < 10; i++)
        if (*(const uint32_t *)(uintptr_t)pd_pool_refs[i][0] + pd_pool_refs[i][1] != PD_POOL_OLD)
            return 0;
    for (int i = 0; i < 8; i++)
        if (*(const uint32_t *)(uintptr_t)pd_pool_bounds[i] != 0x100)
            return 0;
    if (*(const uint32_t *)(uintptr_t)PD_CLEAR_SIZE != 0x400 || !sm_is_call(PD_BUILD_SITE, PD_BUILD_FN))
        return 0;
    uint32_t n = PD_POOL_N, size = sizeof pd_pool;
    memcpy(pd_pool, (const void *)(uintptr_t)PD_POOL_OLD, 0x100 * sizeof *pd_pool);
    memset((void *)(uintptr_t)PD_POOL_OLD, 0, 0x100 * sizeof *pd_pool);
    for (int i = 0; i < 10; i++) {
        uint32_t d = (uint32_t)(uintptr_t)pd_pool - pd_pool_refs[i][1];
        if (!patch_bytes(pd_pool_refs[i][0], (const unsigned char *)&d, 4))
            return 0;
    }
    for (int i = 0; i < 8; i++)
        patch_bytes(pd_pool_bounds[i], (const unsigned char *)&n, 4);
    patch_bytes(PD_CLEAR_SIZE, (const unsigned char *)&size, 4);
    return patch_call(PD_BUILD_SITE, (void *)gmv_pool_build);
}

int pd_centres_on;

__attribute__((cdecl, force_align_arg_pointer))
int gmv_can_spawn(uint32_t sp)
{
    int kind = *(const int *)(uintptr_t)(sp + 0x28), k = -1, r;
    pd_follow_race();
    for (int i = 0; i < 3; i++)
        if (pd_kinds[i].kind == kind && pd_scale[i] != 1)
            k = i;
    if (pd_centres_on)
        pd_set_band(k >= 0 ? pd_scale[k] : 1);
    if (pd_on && k >= 0) {
        uint32_t w = *(const uint32_t *)(uintptr_t)SV_WORLD;
        const int *c = w ? *(const int *const *)(uintptr_t)(w + 0x414) : NULL;
        int n = c ? c[3] + c[8] + c[17] : -1;
        if (pd_tick_n >= 0 && n > pd_tick_n)
            return 0;
        pd_tick_n = n;
    }
    int *prof = (int *)(uintptr_t)pd_rd(0x0063dfbcu);
    if (pd_on && k >= 0 && prof) {
        int *own = (int *)(uintptr_t)(sp + 0x74), day = own[0], night = own[1];
        int *pc = (int *)((char *)prof + pd_kinds[k].prof), pv = *pc;
        own[0] = pd_cap(day, k), own[1] = pd_cap(night, k), *pc = pd_cap(pv, k);
        r = ((int (*)(uint32_t))(uintptr_t)SV_FN)(sp);
        own[0] = day, own[1] = night, *pc = pv;
    } else
        r = ((int (*)(uint32_t))(uintptr_t)SV_FN)(sp);
    return r;
}

#define TJ_STEP_SITE  0x001a1f80u
#define TJ_CARS       0x0068a788u
#define TJ_CAR        0x00656068u
#define TJ_MOTO       0x00656228u
#define TJ_FIRST      0x00020fd2u
#define TJ_KEEP_SITE  0x001a1baau
#define TJ_KEEP_FN    0x001a8f6au
#define TJ_GONE_SITE  0x001a93e8u
#define TJ_WORLD      0x00949a54u
#define TJ_CLEAR_SITE 0x0007fe99u
#define TJ_CLEAR_FN   0x0007fbeau
#define TJ_REVERSE    0x00108758u
#define TJ_STEER      0x00108780u
#define TJ_ACCEL      0x00108708u
#define TJ_SCRIPTED   0x20000000u
#define TJ_N          64
#define TJ_WAIT       3.5f
#define TJ_BACK       1.2f
#define TJ_GIVE_UP    9.0f
#define TJ_FAR        3000.0f
#define TJ_AWAY       400.0f
#define TJ_AHEAD      1500.0f
#define TJ_SIDE       350.0f
#define TJ_SLOW       0.25f
#define TJ_CLEAR      1200.0f
#define TJ_CRAWL      25.0f
#define TJ_SAME       0.7f
#define TJ_GIVE       1.75f
#define TJ_HOLD       1.0f
#define TJ_HELI       0x006568c8u
#define TJ_SPOT_FN    0x0007fceau
#define TJ_HIGH       15000.0f
static const uint32_t tj_spot_sites[2] = {0x00082b8cu, 0x00082bbeu};

typedef struct {
    uint32_t veh;
    unsigned seen;
    int dir, gone;
    float t, next, back, at[2];
} tj_car;
static tj_car tj[TJ_N];
static unsigned tj_tick;
static int tj_gone_n;
static struct {
    uint32_t veh;
    unsigned seen;
    float v, hold;
} tj_run[4];
static struct {
    uint32_t veh;
    unsigned until;
    int back;
    float m[2];
} tj_way[8];

static uint32_t tj_rd(uint32_t a)
{
    return *(const uint32_t *)(uintptr_t)a;
}

static int tj_heli(void)
{
    uint32_t me = tj_rd(MR_PLAYER), v = me ? tj_rd(me + 4 + 0x558) : 0;
    return v && tj_rd(v) == TJ_HELI && pd_scale[0] > 0;
}

static int tj_traffic(uint32_t veh)
{
    uint32_t vt = tj_rd(veh), ch, me = tj_rd(MR_PLAYER);
    if ((vt != TJ_CAR && vt != TJ_MOTO) || tj_rd(veh + 0x2a8) != 3 || !(ch = tj_rd(veh + 0x280)))
        return 0;
    return tj_rd(ch + 0x558) == veh && !(tj_rd(ch + 0x584) & 4) && (!me || ch != me + 4);
}

static int tj_front(uint32_t veh, const float *p, int *dir)
{
    uint32_t me = tj_rd(MR_PLAYER), mine = me ? tj_rd(me + 4 + 0x558) : 0;
    uint32_t v = tj_rd(TJ_CARS), e = tj_rd(TJ_CARS + 4), hit = 0;
    float f[3], q[3], best = TJ_AHEAD;
    int why = 0;
    obj_vec(tj_rd(tj_rd(veh) + 0x68), veh, f);
    for (int k = me && !mine ? -1 : 0; v + 4 * k < e; k++) {
        uint32_t o = k < 0 ? me + 4 : tj_rd(v + 4 * k);
        if (o == veh)
            continue;
        uc_pos(o, q);
        float dx = q[0] - p[0], dy = q[1] - p[1], along = dx * f[0] + dy * f[1], side = dx * f[1] - dy * f[0];
        if (along > 0 && along < best && side * side < TJ_SIDE * TJ_SIDE)
            best = along, why = k < 0 || o == mine ? 2 : 1, *dir = side >= 0, hit = o;
    }
    if (why == 1 && tj_rd(hit + 0x280)) {
        obj_vec(tj_rd(tj_rd(hit) + 0x68), hit, q);
        if (q[0] * f[0] + q[1] * f[1] > TJ_SAME)
            why = 0;
    }
    return why;
}

static void tj_step(uint32_t veh, float dt)
{
    if (!tj_traffic(veh))
        return;
    const float *b = (const float *)(uintptr_t)tj_rd(veh + 0x250);
    float v2 = b ? b[0x10] * b[0x10] + b[0x11] * b[0x11] : 0, p[3];
    tj_car *c = NULL, *spare = NULL;
    for (int i = 0; i < TJ_N && !c; i++)
        if (tj[i].veh == veh)
            c = &tj[i];
        else if (!spare && (!tj[i].veh || tj_tick - tj[i].seen > 60))
            spare = &tj[i];
    if (!c && (v2 >= TJ_SLOW || !spare || (tj_tick + (veh >> 4)) % 16))
        return;
    uc_pos(veh, p);
    int dir = (int)(veh >> 5 & 1), why;
    if (!c) {
        int push = (tj_rd(veh + 0x180) & 1) && *(const float *)(uintptr_t)(veh + 0x18c) > 0;
        if ((why = tj_front(veh, p, &dir)) == 2 || (!why && !push))
            return;
        c = spare;
        tj_gone_n -= c->gone;
        memset(c, 0, sizeof *c);
        c->veh = veh, c->dir = dir, c->next = TJ_WAIT, c->at[0] = p[0], c->at[1] = p[1];
    }
    float ax = p[0] - c->at[0], ay = p[1] - c->at[1];
    if (ax * ax + ay * ay > TJ_AWAY * TJ_AWAY) {
        tj_gone_n -= c->gone;
        memset(c, 0, sizeof *c);
        return;
    }
    c->seen = tj_tick;
    c->t += dt;
    if (c->back > 0) {
        c->back -= dt;
        ((void (*)(uint32_t, float))(uintptr_t)TJ_REVERSE)(veh, 1.0f);
        ((void (*)(uint32_t, int, float))(uintptr_t)TJ_STEER)(veh, c->dir, 1.0f);
        if (c->back <= 0)
            c->next = c->t + TJ_WAIT;
    } else if (c->t >= c->next) {
        if ((why = tj_front(veh, p, &dir)) == 2)
            c->t = 0, c->next = TJ_WAIT;
        else
            c->back = TJ_BACK, c->dir = why ? dir : c->dir;
    }
    uint32_t me = tj_rd(MR_PLAYER);
    if (!c->gone && me && c->t > TJ_GIVE_UP) {
        float q[3];
        uc_pos(me + 4, q);
        q[0] -= p[0], q[1] -= p[1];
        if (q[0] * q[0] + q[1] * q[1] > TJ_FAR * TJ_FAR)
            c->gone = 1, tj_gone_n++;
    }
}

static int tj_chased(uint32_t veh)
{
    uint32_t vt = tj_rd(veh), ch = tj_rd(veh + 0x280), me = tj_rd(MR_PLAYER);
    if ((vt != TJ_CAR && vt != TJ_MOTO) || !ch || !me || !(tj_rd(veh + 0x180) & TJ_SCRIPTED))
        return 0;
    return tj_rd(ch + 0x558) == veh && (tj_rd(ch + 0x584) & 4) && ch != me + 4 && tj_rd(ch + 0x64c) != me + 4;
}

static void tj_chase(uint32_t veh, float dt)
{
    float *b = (float *)(uintptr_t)tj_rd(veh + 0x250), f[3], p[3], g[3];
    int r = -1, spare = -1;
    for (int i = 0; i < 4 && r < 0; i++)
        if (tj_run[i].veh == veh)
            r = i;
        else if (spare < 0 && (!tj_run[i].veh || tj_tick - tj_run[i].seen > 60))
            spare = i;
    if (!b || (r < 0 && (r = spare) < 0))
        return;
    obj_vec(tj_rd(tj_rd(veh) + 0x68), veh, f);
    float fv = b[0x10] * f[0] + b[0x11] * f[1];
    if (tj_run[r].veh != veh)
        tj_run[r].veh = veh, tj_run[r].v = fv, tj_run[r].hold = 0;
    tj_run[r].seen = tj_tick;
    uint32_t hit = 0, v = tj_rd(TJ_CARS), e = tj_rd(TJ_CARS + 4);
    for (uint32_t c = tj_rd((uint32_t)(uintptr_t)b + 0x70); c && !hit; c = tj_rd(c + 0xc))
        if (tj_rd(tj_rd(c + 4) + 8) && !(tj_rd(tj_rd(c + 4) + 4) & 1))
            for (uint32_t o = v; o < e && !hit; o += 4)
                if (tj_rd(tj_rd(o) + 0x250) == tj_rd(c) && tj_traffic(tj_rd(o)))
                    hit = tj_rd(o);
    if (!hit || (tj_rd(veh + 0x180) & 0xa))
        tj_run[r].v = fv, tj_run[r].hold = hit ? tj_run[r].hold : 0;
    else if (fv < tj_run[r].v && tj_run[r].hold < TJ_HOLD) {
        b[0x10] += f[0] * (tj_run[r].v - fv), b[0x11] += f[1] * (tj_run[r].v - fv);
        tj_run[r].hold += dt;
    }
    if (!hit)
        return;
    int y = -1, live = 0;
    for (int i = 0; i < 8 && !live; i++) {
        int on = (int)(tj_way[i].until - tj_tick) > 0;
        if (on && tj_way[i].veh == hit)
            y = i, live = 1;
        else if (!on && y < 0)
            y = i;
    }
    if (y < 0)
        return;
    tj_way[y].until = tj_tick + (unsigned)(TJ_GIVE / dt);
    if (live)
        return;
    uc_pos(veh, p);
    uc_pos(hit, g);
    float a0 = f[1], a1 = -f[0];
    if (f[0] * (g[1] - p[1]) - f[1] * (g[0] - p[0]) >= 0)
        a0 = -a0, a1 = -a1;
    obj_vec(tj_rd(tj_rd(hit) + 0x68), hit, g);
    tj_way[y].veh = hit, tj_way[y].back = a0 * g[0] + a1 * g[1] < 0;
    tj_way[y].m[0] = a0 + (tj_way[y].back ? -g[0] : g[0]), tj_way[y].m[1] = a1 + (tj_way[y].back ? -g[1] : g[1]);
}

static int tj_give_way(uint32_t veh)
{
    for (int i = 0; i < 8; i++)
        if (tj_way[i].veh == veh && (int)(tj_way[i].until - tj_tick) > 0) {
            float g[3], *m = tj_way[i].m;
            if (!tj_traffic(veh))
                return 0;
            obj_vec(tj_rd(tj_rd(veh) + 0x68), veh, g);
            float cr = (m[0] * g[1] - m[1] * g[0]) / sqrtf(m[0] * m[0] + m[1] * m[1] + 1e-6f);
            ((void (*)(uint32_t, float))(uintptr_t)(tj_way[i].back ? TJ_REVERSE : TJ_ACCEL))(veh, 1.0f);
            ((void (*)(uint32_t, int, float))(uintptr_t)TJ_STEER)(veh, cr >= 0, fabsf(cr));
            return 1;
        }
    return 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_tj_step(uint32_t veh, float dt)
{
    if (veh == tj_rd(tj_rd(TJ_CARS)))
        tj_tick++;
    if (!pd_racing && tj_chased(veh))
        tj_chase(veh, dt);
    else if (!tj_give_way(veh) && pd_scale[0] > 1)
        tj_step(veh, dt);
    ((void (*)(uint32_t, float))(uintptr_t)tj_rd(tj_rd(veh) + 0x174))(veh, dt);
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_tj_keep(uint32_t self, uint32_t obj)
{
    for (int i = 0; i < TJ_N && tj_gone_n; i++)
        if (tj[i].veh == obj && tj[i].gone) {
            uint32_t r = ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)code_call_target(TJ_GONE_SITE))(tj_rd(TJ_WORLD), obj);
            if (r != obj)
                memset(&tj[i], 0, sizeof tj[i]), tj_gone_n--;
            return r;
        }
    uint32_t vt = tj_rd(obj);
    float *b = (float *)(uintptr_t)pd_centres[0][1] + 2, was = *b, p[3];
    int on = (vt == TJ_CAR || vt == TJ_MOTO) && tj_heli();
    if (on)
        uc_pos(obj, p), on = fabsf(was - p[2]) < TJ_HIGH;
    if (on)
        *b = p[2];
    uint32_t r = ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)TJ_KEEP_FN)(self, obj);
    *b = was;
    return r;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_tj_spot(uint32_t sp, uint32_t loc)
{
    int kind = *(const int *)(uintptr_t)(sp + 0x28), on = (kind == 3 || kind == 8) && tj_heli();
    float lz = *(const float *)(uintptr_t)(loc + 0x10), *c[4] = {0}, was[4] = {0};
    if (on && kind == 8)
        return 0;
    on = on && fabsf(((const float *)(uintptr_t)pd_centres[0][0])[2] - lz) < TJ_HIGH;
    for (int i = 0; on && i < 4; i++)
        c[i] = (float *)(uintptr_t)pd_centres[i / 2][i % 2] + 2, was[i] = *c[i], *c[i] = lz;
    int r = ((int (*)(uint32_t, uint32_t))(uintptr_t)TJ_SPOT_FN)(sp, loc) & 0xff;
    for (int i = 0; on && i < 4; i++)
        *c[i] = was[i];
    return r;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_tj_clear(uint32_t sp, uint32_t loc)
{
    int r = ((int (*)(uint32_t, uint32_t))(uintptr_t)TJ_CLEAR_FN)(sp, loc) & 0xff;
    uint32_t w = tj_rd(SV_WORLD);
    if (r || pd_scale[0] <= 1 || *(const int *)(uintptr_t)(sp + 0x28) != 3 || !w)
        return r;
    const float *l = (const float *)(uintptr_t)(loc + 8);
    float q[3];
    for (uint32_t o = ((uint32_t (*)(uint32_t, int))(uintptr_t)TJ_FIRST)(w, 0); o; o = tj_rd(o + 0xc)) {
        uint32_t vt = tj_rd(o);
        const float *b = (const float *)(uintptr_t)tj_rd(o + 0x250);
        if ((vt != TJ_CAR && vt != TJ_MOTO) || !tj_rd(o + 0x280) || !b ||
            b[0x10] * b[0x10] + b[0x11] * b[0x11] >= TJ_CRAWL)
            continue;
        uc_pos(o, q);
        q[0] -= l[0], q[1] -= l[1];
        if (q[0] * q[0] + q[1] * q[1] < TJ_CLEAR * TJ_CLEAR)
            return 1;
    }
    return 0;
}

static void apply_population_density(void)
{
    static const unsigned char total_code[3] = { 0x83, 0xf9, 0x30 };   /* cmpl $48,%ecx */
    int any = 0;
    for (int i = 0; i < 3; i++) {
        const char *e = gmv_getenv(pd_kinds[i].env);
        double s = e && *e ? atof(e) : 1;
        pd_user[i] = pd_scale[i] = s < 0 ? 0 : s > PD_MAX ? PD_MAX : s;
        any |= pd_scale[i] != 1;
    }
    if (!any && !tables_live())
        return;
    int ok = (sm_is_call(SV_SITE, SV_FN) || sm_is_call(SV_SITE, (uint32_t)(uintptr_t)gmv_can_spawn)) &&
             !memcmp((const void *)(uintptr_t)(PD_TOTAL_IMM - 2), total_code, 3);
    for (int i = 0; i < 3 && ok; i++) {
        const uint32_t *rec = (const uint32_t *)(uintptr_t)pd_kinds[i].rec;
        ok = *(const uint8_t *)(uintptr_t)(pd_kinds[i].cap_imm - 1) == 0xbf &&
             *(const int *)(uintptr_t)pd_kinds[i].cap_imm == pd_kinds[i].cap &&
             rec[0] == (uint32_t)pd_kinds[i].kind && rec[2] == pd_kinds[i].d0 && rec[3] == pd_kinds[i].d1;
    }
    if (!ok) {
        gmv_log("[gamepatch] spawn check or spawn records not the expected bytes; population density skipped\n");
        return;
    }
    int t = pd_write();
    if (t < 0) {
        gmv_log("[gamepatch] population density write failed (%lu)\n", GetLastError());
        return;
    }
    int deeper = tables_live();
    for (int i = 0; i < 3; i++)
        deeper |= pd_scale[i] > 1;
    if (deeper && sm_is_call(PD_CENTRE_SITE, PD_CENTRE_FN))
        pd_centres_on = patch_call(PD_CENTRE_SITE, (void *)gmv_spawn_centres);
    else if (deeper)
        gmv_log("[gamepatch] spawn centres' update not the expected bytes; the spawn strip stays as deep as the Mac's\n");
    int keep = 0;
    if (deeper && *(const uint32_t *)(uintptr_t)PD_KEEP_SITE + PD_KEEP_BASE == PD_KEEP_OLD &&
        *(const float *)(uintptr_t)PD_KEEP_OLD == 14000.0f * 14000.0f) {
        uint32_t d = (uint32_t)(uintptr_t)&pd_keep2 - PD_KEEP_BASE;
        keep = patch_bytes(PD_KEEP_SITE, (const unsigned char *)&d, 4);
    } else if (deeper)
        gmv_log("[gamepatch] despawn check not the expected bytes; cars and people go where the Mac's do\n");
    if (deeper && !(pd_pool_on = pd_move_pool()))
        gmv_log("[gamepatch] spawn pool code not the expected bytes; no more cars or people than the Mac's pool holds\n");
    if (!dd_on)
        for (int i = 0; i < 2; i++)
            if (sm_is_call(dd_proxy_sites[i], DD_PROXY_FN))
                patch_call(dd_proxy_sites[i], (void *)gmv_create_proxy);
    pd_on = 1;
    gmv_log("[gamepatch] population density: traffic %.2gx (cap 18 -> %d), pedestrians %.2gx (14 -> %d), "
            "parked cars %.2gx (10 -> %d), all together 48 -> %d%s%s%s; the Mac's during races\n",
            pd_scale[0], (int)(18 * pd_scale[0] + 0.5), pd_scale[1], (int)(14 * pd_scale[1] + 0.5),
            pd_scale[2], (int)(10 * pd_scale[2] + 0.5), t, pd_centres_on ? "; spawn strip deepened inwards to match" : "",
            pd_pool_on ? "; spawn pool moved to make room" : "", keep ? "; taken away farther out to match" : "");
}

void apply_spawn_hook(void)
{
    apply_population_density();
    if (!pd_on)
        return;
    if (!sm_is_call(SV_SITE, SV_FN) && !sm_is_call(SV_SITE, (uint32_t)(uintptr_t)gmv_can_spawn)) {
        gmv_log("[gamepatch] spawn check call not the expected bytes; population densities not applied\n");
        return;
    }
    patch_call(SV_SITE, (void *)gmv_can_spawn);
    static const unsigned char step_orig[6] = { 0xff, 0x91, 0x74, 0x01, 0x00, 0x00 };
    unsigned char step[6] = { 0xe8 };
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)gmv_tj_step - (TJ_STEP_SITE + 5));
    memcpy(step + 1, &rel, 4);
    step[5] = 0x90;
    const void *at = (const void *)(uintptr_t)TJ_STEP_SITE;
    if ((memcmp(at, step_orig, sizeof step) && memcmp(at, step, sizeof step)) ||
        (!sm_is_call(TJ_KEEP_SITE, TJ_KEEP_FN) && !sm_is_call(TJ_KEEP_SITE, (uint32_t)(uintptr_t)gmv_tj_keep)) ||
        (!sm_is_call(TJ_CLEAR_SITE, TJ_CLEAR_FN) && !sm_is_call(TJ_CLEAR_SITE, (uint32_t)(uintptr_t)gmv_tj_clear))) {
        gmv_log("[gamepatch] vehicle step, despawn or spawn clearance call not the expected bytes; stuck traffic is "
                "left to the game\n");
        return;
    }
    patch_bytes(TJ_STEP_SITE, step, sizeof step);
    patch_call(TJ_KEEP_SITE, (void *)gmv_tj_keep);
    patch_call(TJ_CLEAR_SITE, (void *)gmv_tj_clear);
    for (int i = 0; i < 2; i++)
        if (sm_is_call(tj_spot_sites[i], TJ_SPOT_FN))
            patch_call(tj_spot_sites[i], (void *)gmv_tj_spot);
        else
            gmv_log("[gamepatch] spawn spot check call not the expected bytes; no traffic below a helicopter\n");
}
