#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xf9e94 0x8ae3c 0x74 +0x7f4 0x8a998 +0x40 +0x6c */
#define SI_CASH     0x754u
#define SI_WEAPONS  0x7f4u
#define SI_SLOTS    16
#define SI_AMMO     0x40u
#define SI_AMMO_N   11
#define SI_IN_HAND  0x6cu
#define SI_ARMED    0x70u
#define SI_W_LOCKED 0xa1u
#define SI_W_KEEP1  0x3cu
#define SI_W_KEEP2  0x44u
static volatile int g_si_on;
static uint32_t si_who;
static uint32_t si_paused;
static int32_t si_paused_cash;

int si_set(int on)
{
    if (on != g_si_on)
        gmv_log("[gamepatch] PLAYERSWITCH_INVENTORY %s\n", on ? "on" : "off");
    g_si_on = on;
    if (!on)
        si_who = si_paused = 0;
    return 1;
}

static int si_mem(uint32_t a, uint32_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    return a >= 0x10000 && VirtualQuery((const void *)(uintptr_t)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
           (uint32_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)(uintptr_t)a) >= n;
}

static uint32_t si_infos(uint32_t p)
{
    uint32_t w = si_mem(p, SI_WEAPONS + 4) ? ms_rd(p + SI_WEAPONS) : 0;
    return w && si_mem(w, 0x74) && ms_rd(w + SI_IN_HAND) < SI_SLOTS ? w : 0;
}

static void si_copy(uint32_t from, uint32_t to, int may_lock)
{
    uint32_t fi = si_infos(from), ti = si_infos(to), hand = ms_rd(ti + SI_IN_HAND);
    *(int32_t *)(uintptr_t)(to + SI_CASH) = (int32_t)ms_rd(from + SI_CASH);
    for (int i = 0; i < SI_AMMO_N; i++)
        *(uint32_t *)(uintptr_t)(ti + SI_AMMO + 4 * i) = ms_rd(fi + SI_AMMO + 4 * i);
    /* 0xf4b44 0x11e5ba 0x11d222 0x8b00c */
    unsigned char armed = *(unsigned char *)(uintptr_t)(fi + SI_ARMED);
    if (armed || may_lock)
        *(unsigned char *)(uintptr_t)(ti + SI_ARMED) = armed;
    for (int i = 0; i < SI_SLOTS; i++) {
        uint32_t fw = ms_rd(fi + 4 * i), tw = ms_rd(ti + 4 * i);
        if (!fw || !tw || fw == tw || !si_mem(fw, SI_W_LOCKED + 1) || !si_mem(tw, SI_W_LOCKED + 1))
            continue;
        unsigned char lock = *(unsigned char *)(uintptr_t)(fw + SI_W_LOCKED);
        if (lock && !may_lock)
            continue;
        *(unsigned char *)(uintptr_t)(tw + SI_W_LOCKED) = (uint32_t)i == hand ? 0 : lock;
        *(uint32_t *)(uintptr_t)(tw + SI_W_KEEP1) = *(uint32_t *)(uintptr_t)(fw + SI_W_KEEP1);
        *(uint32_t *)(uintptr_t)(tw + SI_W_KEEP2) = *(uint32_t *)(uintptr_t)(fw + SI_W_KEEP2);
    }
}

static int si_weapons_owned(uint32_t p)
{
    uint32_t wi = si_infos(p);
    int n = 0;
    for (int i = 0; wi && i < SI_SLOTS; i++) {
        uint32_t w = ms_rd(wi + 4 * i);
        n += w && si_mem(w, SI_W_LOCKED + 1) && !*(unsigned char *)(uintptr_t)(w + SI_W_LOCKED);
    }
    return n;
}

__attribute__((force_align_arg_pointer))
void gmv_playerswitch_inventory_tick(void)
{
    lc_tick();
    if (!g_si_on)
        return;
    uint32_t mainp = ms_rd(SSF_MAIN_PP), ctrl = ms_rd(SSF_CTRL_PP), mgr = mr_mgr();
    int players = mainp && ctrl && mainp != ctrl && ms_rd(mainp) == SSF_PLAYER_VT && ms_rd(ctrl) == SSF_PLAYER_VT &&
                  si_infos(mainp) && si_infos(ctrl);
    int mission = !mgr || (int)ms_rd(mgr + 4) != -1 || ms_rd(mgr + 0x1c);
    if (si_who && (!players || ctrl != si_who || mission)) {
        if (players && ctrl == si_who) {
            si_paused = ctrl;
            si_paused_cash = (int32_t)ms_rd(ctrl + SI_CASH);
        }
        gmv_trace("[switchinv] sharing stopped (%s); Johnny's cash %d\n",
                !players ? "back to Johnny" : ctrl != si_who ? "another player" : "a mission began",
                mainp ? (int)ms_rd(mainp + SI_CASH) : 0);
        si_who = 0;
    }
    if (!players || mission) {
        if (!players)
            si_paused = 0;
        return;
    }
    if (!si_who) {
        int32_t made = 0;
        if (si_paused == ctrl && (made = (int32_t)ms_rd(ctrl + SI_CASH) - si_paused_cash) > 0)
            *(int32_t *)(uintptr_t)(mainp + SI_CASH) += made;
        else
            made = 0;
        si_paused = 0;
        si_copy(mainp, ctrl, 1);
        si_who = ctrl;
        gmv_trace("[switchinv] the player in control shares Johnny's things: cash %d (%d made in the mission), "
                "%d weapons\n", (int)ms_rd(ctrl + SI_CASH), (int)made, si_weapons_owned(ctrl));
        return;
    }
    si_copy(ctrl, mainp, 0);
}

void apply_playerswitch_inventory(void)
{
    const char *e = gmv_getenv("GMV_PLAYERSWITCH_INVENTORY");
    g_si_on = e && *e == '1';
    gmv_log("[gamepatch] PLAYERSWITCH_INVENTORY %s (in free roam the switched-to player shares Johnny's cash and "
            "weapons)\n", g_si_on ? "on" : "off");
}
