#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x165172 0x16531c 0x16d334 0x74 0x6583a8 0xf +0x14 */
#define MR_HIDE6_SITE  0x001652b7u
#define MR_HIDE5_SITE  0x00165297u
#define MR_CLICK_SITE  0x0016540bu   /* call 0x16eff4 */
#define MR_TABLE_CMP   0x00165410u   /* cmp eax,3 */
#define MR_REG_SITE    0x001600b4u
#define MR_SM_CTOR     0x0016d334u
#define MR_SM_BYTES    0x74u
#define MR_SM_VT       0x006583a8u
#define MR_SM_SHOW     0x001654dcu
#define MR_SM_HIDE     0x0016335cu
#define MR_SM_EVENT    0x00165e62u
#define MR_SV_SHOW     0x00165172u
#define MR_SV_EVENT    0x0016531cu
#define MR_W_MISSION   6
#define MR_W_NAME      0xf
#define MR_WM_VT       0x00659488u
#define MR_WM_SLOTS    7
#define MR_WM_EVENT    0x001798e8u
#define MR_WM_TEXTS    0x00179baau
#define MR_FRAME       0x002fd8a0u
#define MR_MONEY       0x0008e6e2u
#define MR_SET_ID      0x0016eb56u
#define MR_IDS_RESIZE  0x005edb8eu   /* 0x5c */
#define MR_IDS_SITE    0x0016d37fu
#define MR_CAT_X       398
#define MR_CAT_Y       117
#define MR_TOD_CAT_X   635           /* 0x43a8 */
#define MR_TOD_CAT_Y   322           /* 0x1cd4 */
#define MR_KEY_PATH    "widgetActionKeyTitle.titleAction"
#define MR_KEY_X       17
#define MR_KEY_Y       62
#define MR_TOD_KEY_X   153           /* 0x120c txtValue */
#define MR_SLOT        6
#define MR_STATE       "ServiceReplay"
#define MR_TOD_STATE   "ServiceTime"
#define MR_SM_IDS      7             /* 0x1e 1 0x11 0x13 6 0x12 0xf */
#define MR_MAX         128
#define MR_SC_SET      0x00032284u
#define MR_SC_STOP     0x00032202u
#define MR_SC_STEP     0x000321bau
#define MR_SC_UNDO     0x0003236cu
#define MR_SC_RESUME   0x0003244cu
#define MR_SC_ID       0x00032560u
#define MR_SC_FIND     0x00032d98u
#define MR_CP_NAME     0x000c7e8eu   /* 0x8f */
#define MR_OBJ_FIND    0x00034af8u
#define MR_START       0x00180360u   /* widgetMission +0x14 */
#define MR_POP_TO      0x0015cafcu
#define MR_MENU_OFF    0x0015cbe2u
#define MR_GAME_ON     0x001a46dau
#define MR_UNLOCK_SLOT 0x006553e8u
#define MR_UNLOCK      0x000d080eu
#define MR_SAVE        0x00098148u
#define MR_SAVE_NOW    0x000f3606u   /* 0x98148 */
#define MR_HUD         0x00685668u
#define MR_HUD_SHOWN   0x00029400u
#define MR_HUD_SHOW    0x000293c8u   /* HUD +4 0x296b0 */
#define MR_CINE        0x0003440eu   /* +0xc */
#define MR_RESET       0x00033e78u   /* 0x8d1dc */
#define MR_FAIL_SITE   0x0003705du   /* 0x83 */
#define MR_OBJ_NAMED   0x00032838u
#define MR_IS_PLAYER   0x000617b2u
#define MR_IS_DEAD     0x0007e294u
#define MR_WORLD_OFF   0x424         /* 0x24270 0x2100a */
#define MR_POS_STORE   0x000f4774u   /* +0x7d0..+0x7e3 */
#define MR_POS_BACK    0x000f4860u
#define MR_POS_OFF     0x7d0
#define MR_FADE_OUT_MS 350
#define MR_FADE_HOLD_MS 250
#define MR_FADE_IN_MS  500
#define MR_WATCH_MS    30000
#define MR_SETTLE_MS   1500
#define MR_WAIT_MS     60000

static int mr_page, mr_n, mr_cur, mr_cat, mr_pos[3];
static const char *const mr_cat_name[3] = { "STORY", "SIDE", "RACES" };
static uint32_t mr_list[MR_MAX];
static uint32_t mr_vt[OM_VT_N], mr_tod_vt[OM_VT_N];
static unsigned char *mr_tod_page;
static uint32_t mr_wm_vt[2 + MR_WM_SLOTS];
static uint32_t mr_cat_vt[2 + OM_CAT_SLOTS], *mr_cat_prev;
static float mr_cat_home[2];
uint32_t mr_unlock_prev;
#if GMV_DEV
#define MR_ML_UNLOCK ((uint32_t)(uintptr_t)gmv_ml_unlock)
#else
#define MR_ML_UNLOCK 0u
#endif
static uint32_t mr_msg_rec;
static const char *mr_msg;

rp_t rp;

uint32_t mr_mgr(void)
{
    uint32_t owner = ms_rd(MS_OWNER);
    return owner ? ms_rd(owner + 4) : 0;
}

static int mr_rec_id(uint32_t rec, uint32_t mgr)
{
    uint32_t recs = mgr ? ms_rd(mgr + 0x50) : 0;
    int n = recs ? (int)ms_rd(mgr + 0x54) : 0;
    for (int r = 0; r < n && r < 1000; r++)
        if (ms_rd(recs + 4 * r) == rec)
            return r;
    return -1;
}

static uint32_t mr_cmd(uint32_t sc, int i)
{
    return i >= 0 && i < (int)ms_rd(sc + 4) ? ms_rd(ms_rd(sc + 0x10) + 4 * i) : 0;
}

/* 0xa8/0x81 0x8f 0x63 */
static uint32_t mr_script(int id, int *at)
{
    static uint32_t c_list;
    static int c_id = -1, c_at;
    static uint32_t c_sc;
    uint32_t list = ms_rd(MS_SCRIPTS + 0x2dc);
    int n = (int)ms_rd(MS_SCRIPTS + 0x2ec);
    if (id < 0 || !list || n <= 0 || n > MS_SC_MAX)
        return 0;
    if (list != c_list || id != c_id) {
        c_list = list, c_id = id, c_sc = 0;
        for (int s = 0; s < n && !c_sc; s++) {
            uint32_t sc = ms_rd(list + 4 * s);
            int i = 0, t;
            if (!sc || ((int (*)(uint32_t))(uintptr_t)MR_SC_ID)(sc) != id)
                continue;
            while ((t = ms_cmd_type(mr_cmd(sc, i))) == 0xa8 || t == 0x81)
                i++;
            if (t == 0x8f || t == 0x63)
                c_sc = sc, c_at = i;
        }
    }
    if (at)
        *at = c_at;
    return c_sc;
}

static int mr_snap_ix(uint32_t sc)
{
    for (int s = 0; s < rp.n; s++)
        if (rp.s_sc[s] == sc)
            return s;
    return -1;
}

static int mr_hud_shown(void)
{
    uint32_t hud = ms_rd(MR_HUD);
    return hud ? ((int (*)(uint32_t))(uintptr_t)MR_HUD_SHOWN)(hud) & 0xff : -1;
}

static uint32_t mr_obj(uint32_t name)
{
    return name ? ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)MR_OBJ_NAMED)(rp.sc, name) : 0;
}

enum { MF_NONE, MF_DARKENING, MF_DARK, MF_CLEARING };
mf_t mf;

static void mf_put(int a)
{
    *(int32_t *)(uintptr_t)MR_FADE = mf.a = a;
}

static int mf_dark(void)
{
    if (mf.phase == MF_NONE) {
        if (*(const int32_t *)(uintptr_t)MR_FADE != MR_FADE_NONE)
            return 1;
        mf.phase = MF_DARKENING, mf.t = GetTickCount();
        mf_put(0);
    }
    return mf.phase == MF_DARK;
}

static void mf_clear(DWORD after)
{
    if (mf.phase != MF_NONE)
        mf.phase = MF_CLEARING, mf.t = GetTickCount() + after;
}

static void mf_tick(void)
{
    if (mf.phase == MF_NONE)
        return;
    if (*(const int32_t *)(uintptr_t)MR_FADE != mf.a) {
        gmv_trace("[replay] the game's fader took over the fade (%d)\n", *(const int32_t *)(uintptr_t)MR_FADE);
        mf.phase = MF_NONE;
        return;
    }
    DWORD now = GetTickCount();
    int ms = (int)(now - mf.t);
    if (mf.phase == MF_DARKENING) {
        mf_put(ms >= MR_FADE_OUT_MS ? 255 : 255 * ms / MR_FADE_OUT_MS);
        if (mf.a == 255)
            mf.phase = MF_DARK, mf.t = now;
    } else if (mf.phase == MF_DARK) {
        if (rp.phase != MR_ENDING || ms > 3000)
            mf_clear(0);
    } else if (ms >= MR_FADE_IN_MS) {
        mf_put(MR_FADE_NONE);
        mf.phase = MF_NONE;
    } else if (ms > 0)
        mf_put(255 - 255 * ms / MR_FADE_IN_MS);
}

static void mr_watch(void)
{
    if (!rp.watch)
        return;
    if ((int)(GetTickCount() - rp.watch) > 0 || mr_mgr() != rp.mgr || ms_rd(MS_SCRIPTS + 0x2dc) != rp.list) {
        rp.watch = 0;
        return;
    }
    for (int k = 0; k < rp.no; k++) {
        int on = *(const uint16_t *)(uintptr_t)(rp.o_obj[k] + 4) & 1;
        if (on != rp.o_seen[k])
            gmv_trace("[replay] object \"%s\" switched %s after the put-back (+4 %04x)\n", (const char *)(uintptr_t)rp.o_name[k],
                    on ? "on again" : "off", *(const uint16_t *)(uintptr_t)(rp.o_obj[k] + 4));
        rp.o_seen[k] = (unsigned char)on;
    }
}

#if GMV_DEV
/* HUD 0x293c8 0x296b0 +0xec 0x2941c 0x2942c */
#define MR_HT_LINES 400
static int mr_ht_on, mr_ht_hooked, mr_ht_lines, mr_ht_shown = -2, mr_ht_ec = -2;
static uint32_t mr_ht_cur;

static uint32_t mr_ht_va(uint32_t a)
{
    return a - GMV_LOWCOPY_BASE < GMV_LOWCOPY_CAP ? a - GMV_LOW_DELTA : a;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_hud_hook(int which, uint32_t ret, uint32_t ebp)
{
    static const char *const what[4] = { "show", "hide", "+0xec set", "+0xec cleared" };
    uint32_t hud = ms_rd(MR_HUD), mgr = mr_mgr();
    if (!mr_ht_on || mr_ht_lines >= MR_HT_LINES || !hud || which < 0 || which > 3)
        return;
    mr_ht_lines++;
    gmv_log("[hudtrace] %s from 0x%x (its caller 0x%x): shown %d, +0xec %d, mission %d state %d, cutscene mode %d\n",
            what[which], mr_ht_va(ret), ebp ? mr_ht_va(ms_rd(ebp + 4)) : 0, mr_hud_shown(),
            *(const unsigned char *)(uintptr_t)(hud + 0xec), mgr ? (int)ms_rd(mgr + 4) : -2,
            mgr ? (int)ms_rd(mgr + 0x1c) : -2, (int)ms_rd(MS_SCRIPTS + 0xc));
}

/* push ebp */
static int mr_ht_hook(uint32_t va, const unsigned char *pro, size_t len, int which)
{
    if (len < 5 || len > 8 || memcmp((const void *)(uintptr_t)va, pro, len))
        return 0;
    unsigned char *st = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st)
        return 0;
    size_t o = 0;
    uint32_t a = (uint32_t)(uintptr_t)&gmv_mr_hud_hook;
    st[o++] = 0x55;                                                     /* push ebp */
    st[o++] = 0xff; st[o++] = 0x74; st[o++] = 0x24; st[o++] = 0x04;     /* push [esp+4] */
    st[o++] = 0x6a; st[o++] = (unsigned char)which;
    st[o++] = 0xb8; memcpy(st + o, &a, 4); o += 4;                      /* mov eax,fn */
    st[o++] = 0xff; st[o++] = 0xd0;                                     /* call eax */
    st[o++] = 0x83; st[o++] = 0xc4; st[o++] = 0x0c;                     /* add esp,12 */
    memcpy(st + o, pro, len);
    o += len;
    emit_jmp(st, &o, va + (uint32_t)len);
    FlushInstructionCache(GetCurrentProcess(), st, o);
    return patch_jmp(va, st, len);
}

static void mr_ht_start(void)
{
    static const unsigned char show[7] = { 0x55, 0x89, 0xe5, 0x56, 0x83, 0xec, 0x04 };
    static const unsigned char hide[5] = { 0x55, 0x89, 0xe5, 0x57, 0x56 };
    static const unsigned char flag[6] = { 0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08 };
    if (!mr_ht_hooked) {
        mr_ht_hooked = 1;
        int ok = mr_ht_hook(MR_HUD_SHOW, show, sizeof show, 0) + mr_ht_hook(0x000296b0u, hide, sizeof hide, 1)
               + mr_ht_hook(0x0002941cu, flag, sizeof flag, 2) + mr_ht_hook(0x0002942cu, flag, sizeof flag, 3);
        gmv_log("[hudtrace] %d of 4 HUD functions hooked; logging what shows or hides the HUD from now on\n", ok);
    }
    mr_ht_on = 1;
    mr_ht_lines = 0;
    mr_ht_shown = mr_ht_ec = -2;
}

static void mr_ht_poll(void)
{
    uint32_t hud = mr_ht_on ? ms_rd(MR_HUD) : 0;
    if (!hud || mr_ht_lines >= MR_HT_LINES)
        return;
    int shown = mr_hud_shown(), ec = *(const unsigned char *)(uintptr_t)(hud + 0xec);
    uint32_t cur = ms_rd(hud + 4);
    if (shown == mr_ht_shown && ec == mr_ht_ec && cur == mr_ht_cur)
        return;
    mr_ht_shown = shown, mr_ht_ec = ec, mr_ht_cur = cur;
    mr_ht_lines++;
    gmv_log("[hudtrace] now: shown %d, +0xec %d, HUD in use %s (vtable %06x)\n", shown, ec,
            cur == ms_rd(hud + 8) ? "walking" : cur == ms_rd(hud + 0xc) ? "driving" : "other", cur ? ms_rd(cur) : 0);
}
#else
static void mr_ht_start(void) {}
static void mr_ht_poll(void) {}
#endif

/* 0x6a +0x1c 0x69 +0x14 PlayerSwitch 0x7b 0x70219 */
__attribute__((cdecl, force_align_arg_pointer, noinline))
static void mr_snapshot_world(void)
{
    char types[0x100] = { 0 }, line[600];
    int len = 0;
    rp.no = 0;
    rp.watch = 0;
    for (int i = 0, t; i < (int)ms_rd(rp.sc + 4) && i < 4 * MS_CMD_MAX; i++) {
        uint32_t c = mr_cmd(rp.sc, i), name, o;
        if ((t = ms_cmd_type(c)) >= 0 && t < 0x100 && types[t] < 99)
            types[t]++;
        if (t != 0x6a && t != 0x69 && t != 0x7b)
            continue;
        name = ms_rd(c + (t == 0x6a ? 0x1c : 0x14));
        o = mr_obj(name);
        int k = 0;
        while (k < rp.no && rp.o_obj[k] != o)
            k++;
        if (o && k == rp.no && rp.no < MR_OBJS)
            rp.o_name[rp.no] = name, rp.o_obj[rp.no] = o, rp.o_flags[rp.no++] = *(const uint16_t *)(uintptr_t)(o + 4);
    }
    for (int t = 0; t < 0x100 && len < (int)sizeof line - 16; t++)
        if (types[t])
            len += snprintf(line + len, sizeof line - len, " %02x*%d", t, types[t]);
    line[len] = 0;
    gmv_trace("[replay] the script's commands (type*count):%s\n", line);
    for (int k = 0; k < rp.no; k++)
        gmv_trace("[replay] it switches \"%s\": %08x, vtable %08x, +4 %04x\n", (const char *)(uintptr_t)rp.o_name[k],
                rp.o_obj[k], ms_rd(rp.o_obj[k]), rp.o_flags[k]);
    uint32_t p = rp.player = ms_rd(MR_PLAYER);
    if (p) {
        unsigned char keep[MR_POS_BYTES];
        memcpy(keep, (const void *)(uintptr_t)(p + MR_POS_OFF), MR_POS_BYTES);
        ((void (*)(uint32_t))(uintptr_t)MR_POS_STORE)(p);
        memcpy(rp.pos, (const void *)(uintptr_t)(p + MR_POS_OFF), MR_POS_BYTES);
        memcpy((void *)(uintptr_t)(p + MR_POS_OFF), keep, MR_POS_BYTES);
    }
}

/* ObjectEnable 0xcee9a 0x800 0x3706f 0xf356a 0xac +0x20 */
__attribute__((cdecl, force_align_arg_pointer, noinline))
static void mr_world_back(void)
{
    for (int k = 0; k < rp.no; k++) {
        uint32_t o = rp.o_obj[k], node = ms_rd(o + 0x10), list = ms_rd(o + 0x20);
        uint32_t off = ms_rd(ms_rd(MR_WORLD_REF)) + MR_WORLD_OFF;
        const char *how = "switched off";
        uint16_t *f = (uint16_t *)(uintptr_t)(o + 4), was = rp.o_flags[k], now = *f;
        const char *name = (const char *)(uintptr_t)rp.o_name[k];
        rp.o_seen[k] = (unsigned char)(now & 1);
        if (mr_obj(rp.o_name[k]) != o) {
            gmv_trace("[replay] object \"%s\": another object by that name now; left\n", name);
            continue;
        }
        if (was & 1) {
            if (!(now & 1))
                gmv_trace("[replay] object \"%s\": +4 %04x before START, %04x now; left\n", name, was, now);
            continue;
        }
        if (((int (*)(uint32_t))(uintptr_t)MR_IS_PLAYER)(o) & 0xff) {
            gmv_trace("[replay] object \"%s\": +4 %04x before START, %04x now, under control; left\n", name, was, now);
            continue;
        }
        if (!(now & 1) && list && list != off && node) {
            *f |= 1;
            how = "switched off again (not in the off list)";
        }
        if (*f & 1)
            ((void (*)(uint32_t))(uintptr_t)ms_rd(ms_rd(o) + 0xac))(o);
        else if (node) {
            ((void (*)(uint32_t, int))(uintptr_t)ms_rd(ms_rd(node) + 0x48))(node, 0);
            how = "its model hidden";
        } else
            how = "no model";
        rp.o_seen[k] = (unsigned char)(*f & 1);
        *f = (uint16_t)((*f & 3) | (was & ~3));
        gmv_trace("[replay] object \"%s\": +4 %04x before START, %04x now, list +%x; %s, +4 %04x, list +%x\n",
                name, was, now, list ? list - (off - MR_WORLD_OFF) : 0, how, *f,
                ms_rd(o + 0x20) ? ms_rd(o + 0x20) - (off - MR_WORLD_OFF) : 0);
    }
    uint32_t p = ms_rd(MR_PLAYER);
    const float *at = (const float *)(rp.pos + 4);
    if (!p || p != rp.player)
        gmv_trace("[replay] another character under control (%08x, %08x at START); not moved back\n", p, rp.player);
    else if (((int (*)(uint32_t))(uintptr_t)MR_IS_DEAD)(p + 4) & 0xff)
        gmv_trace("[replay] the player is dead; not moved back\n");
    else {
        unsigned char keep[MR_POS_BYTES];
        memcpy(keep, (const void *)(uintptr_t)(p + MR_POS_OFF), MR_POS_BYTES);
        memcpy((void *)(uintptr_t)(p + MR_POS_OFF), rp.pos, MR_POS_BYTES);
        ((void (*)(uint32_t))(uintptr_t)MR_POS_BACK)(p);
        memcpy((void *)(uintptr_t)(p + MR_POS_OFF), keep, MR_POS_BYTES);
        gmv_trace("[replay] the player moved back to where START found it (%.1f %.1f %.1f)\n", at[0], at[1], at[2]);
    }
    rp.watch = GetTickCount() + MR_WATCH_MS;
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
static void mr_snapshot(void)
{
    uint32_t list = ms_rd(MS_SCRIPTS + 0x2dc);
    int n = (int)ms_rd(MS_SCRIPTS + 0x2ec);
    rp.list = list;
    rp.n = list && n > 0 && n <= MS_SC_MAX ? n : 0;
    for (int s = 0; s < rp.n; s++) {
        uint32_t sc = rp.s_sc[s] = ms_rd(list + 4 * s);
        rp.s_st[s] = (int)ms_rd(sc + 0x18), rp.s_pc[s] = (int)ms_rd(sc + 8), rp.s_time[s] = ms_rd(sc + 0x20);
    }
    memcpy(rp.vars, (const void *)(uintptr_t)(MS_SCRIPTS + 0x3b8), sizeof rp.vars);
    for (int i = 0; i < 8; i++)
        rp.flags[i] = ms_rd(rp.mgr + 0x90 + 4 * i);
    rp.tries = ms_rd(rp.mgr + 0x80);
    rp.mission_sc = ms_rd(MS_SCRIPTS + 0x14);
    rp.tutorial = *(const unsigned char *)(uintptr_t)(rp.mgr + 0x20);
    rp.cine = (int)ms_rd(MS_SCRIPTS + 0xc);
    mr_snapshot_world();
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
static void mr_restore(const char *how)
{
    uint32_t mgr = rp.mgr;
    int idle = (int)ms_rd(mgr + 4) == -1 && !ms_rd(mgr + 0x1c), undone = 0, restarted = 0, resumed = 0, moved = 0;
    for (int s = 0; s < rp.n; s++) {
        uint32_t sc = rp.s_sc[s];
        int st = (int)ms_rd(sc + 0x18), pc = (int)ms_rd(sc + 8);
        if (!rp.s_st[s] && st) {
            if (sc == rp.sc && rp.passed)
                ((void (*)(uint32_t, int))(uintptr_t)MR_SC_STOP)(sc, 0);
            else
                ((void (*)(uint32_t))(uintptr_t)MR_SC_UNDO)(sc);
            gmv_trace("[replay] script %d (mission %d): idle before, state %d at %d now; %s\n", s,
                    ((int (*)(uint32_t))(uintptr_t)MR_SC_ID)(sc), st, pc,
                    sc == rp.sc && rp.passed ? "stopped" : "undone");
            undone++;
        } else if (rp.s_st[s] && !st) {
            ((void (*)(uint32_t, int, int))(uintptr_t)MR_SC_SET)(sc, 2, rp.s_st[s] == 2 ? -1 : (int)rp.s_time[s]);
            gmv_trace("[replay] script %d: state %d at %d before, stopped now; running again from its start\n", s,
                    rp.s_st[s], rp.s_pc[s]);
            restarted++;
        } else if (idle && rp.s_st[s] == 2 && st == 1 && pc == -1) {
            ((void (*)(uint32_t))(uintptr_t)MR_SC_RESUME)(sc);
            gmv_trace("[replay] script %d: still held from the mission; running again\n", s);
            resumed++;
        } else if (st != rp.s_st[s] || pc != rp.s_pc[s])
            moved++;
    }
    memcpy((void *)(uintptr_t)(MS_SCRIPTS + 0x3b8), rp.vars, sizeof rp.vars);
    for (int i = 0; i < 8; i++)
        *(uint32_t *)(uintptr_t)(mgr + 0x90 + 4 * i) = rp.flags[i];
    *(uint32_t *)(uintptr_t)(mgr + 0x80) = rp.tries;
    uint32_t msc = ms_rd(MS_SCRIPTS + 0x14);
    unsigned char *tut = (unsigned char *)(uintptr_t)(mgr + 0x20);
    if (idle && (msc != rp.mission_sc || *tut != rp.tutorial)) {
        gmv_trace("[replay] mission script %d (mission %d), tutorial flag %d; as before START: script %d, flag %d\n",
                msc ? mr_snap_ix(msc) : -1, msc ? ((int (*)(uint32_t))(uintptr_t)MR_SC_ID)(msc) : -1, *tut,
                rp.mission_sc ? mr_snap_ix(rp.mission_sc) : -1, rp.tutorial);
        *(uint32_t *)(uintptr_t)(MS_SCRIPTS + 0x14) = rp.mission_sc;
        *tut = rp.tutorial;
    }
    if (!rp.passed)
        mr_ht_start();
    int cine = (int)ms_rd(MS_SCRIPTS + 0xc), shown = mr_hud_shown();
    gmv_trace("[replay] cutscene mode %d (%d before START), HUD shown %d, +0xec %d\n", cine, rp.cine, shown,
            ms_rd(MR_HUD) ? *(const unsigned char *)(uintptr_t)(ms_rd(MR_HUD) + 0xec) : -1);
    if (idle && cine && !rp.cine) {
        ((void (*)(uint32_t, int))(uintptr_t)MR_CINE)(MS_SCRIPTS, 0);
        shown = mr_hud_shown();
        gmv_trace("[replay] cutscene mode set back to 0 (play), HUD shown %d\n", shown);
    }
    if (idle && !shown && !ms_rd(MS_SCRIPTS + 0xc)) {
        ((void (*)(uint32_t))(uintptr_t)MR_HUD_SHOW)(ms_rd(MR_HUD));
        gmv_trace("[replay] HUD shown again: %d\n", mr_hud_shown());
    }
    if (idle && rp.phase == MR_ENDING && !rp.passed)
        mr_world_back();
    int held = rp.held;
    rp.phase = MR_IDLE;
    gmv_trace("[replay] %s: %d scripts undone, %d run again, %d resumed, %d moved on their own (left); story variables, "
            "completion bits and tries as before, cash kept\n", how, undone, restarted, resumed, moved);
    if (held) {
        ((void (*)(void))(uintptr_t)MR_SAVE_NOW)();
        gmv_trace("[replay] the %d save%s held back during the replay made now\n", held, held == 1 ? "" : "s");
    }
}

/* START 0x8f */
__attribute__((cdecl, force_align_arg_pointer, noinline))
static void mr_begin(void)
{
    uint32_t sc = rp.sc, mgr = rp.mgr;
    unsigned char *wm = om_widget(MR_W_MISSION);
    int opener = rp.opener = ms_cmd_type(mr_cmd(sc, rp.at)) == 0x63;
    mr_snapshot();
    ((void (*)(uint32_t, int, int))(uintptr_t)MR_SC_SET)(sc, 2, -1);
    if (!opener) {
        while ((int)ms_rd(sc + 8) < rp.at && ((int (*)(uint32_t))(uintptr_t)MR_SC_STEP)(sc))
            ;
        if ((int)ms_rd(sc + 8) != rp.at || !wm) {
            mr_restore("not started: the script did not reach its checkpoint");
            return;
        }
        *(uint32_t *)(wm + 0x14) = sc;
        ((void (*)(void *))(uintptr_t)MR_START)(wm);
        if (!(ms_rd(mgr + 0x84) >> 8 & 0xff)) {
            mr_restore("not started: the game did not take the start");
            return;
        }
    }
    void *m = om_mgr();
    typedef void (*switch_fn)(void *, const char *, int);
    ((switch_fn)(uintptr_t)OM_SWITCH_FN)(m, "ServiceWeaponShop", 0);
    ((switch_fn)(uintptr_t)OM_SWITCH_FN)(m, "ServiceCarRental", 0);
    ((switch_fn)(uintptr_t)OM_SWITCH_FN)(m, "ServiceMissions", 0);
    ((void (*)(void *, const char *, int, int))(uintptr_t)MR_POP_TO)(m, "Map", 1, 1);
    ((void (*)(void *))(uintptr_t)MR_MENU_OFF)(m);
    ((unsigned char *)m)[0x224] = 1;
    ((void (*)(void))(uintptr_t)MR_GAME_ON)();
    rp.phase = MR_STARTING;
    rp.t = GetTickCount();
    if (mf.phase == MF_NONE && *(const int32_t *)(uintptr_t)MR_FADE == MR_FADE_NONE) {
        mf_put(255);
        mf.phase = MF_DARK;
        mf_clear(MR_FADE_HOLD_MS);
    }
    gmv_trace("[replay] mission %d started from script %d%s\n", rp.id, mr_snap_ix(sc),
            opener ? " (a chapter's first: set running)" : "");
}

/* 0x599d2f */
__attribute__((force_align_arg_pointer))
void gmv_mission_replay_tick(void)
{
    mf_tick();
    mr_watch();
    mr_ht_poll();
    if (rp.phase == MR_IDLE)
        return;
    DWORD now = GetTickCount();
    uint32_t mgr = mr_mgr();
    if (mgr != rp.mgr || ms_rd(MS_SCRIPTS + 0x2dc) != rp.list) {
        gmv_trace("[replay] the world was loaded again; the replay is dropped\n");
        rp.phase = MR_IDLE;
        return;
    }
    int cur = (int)ms_rd(mgr + 4), st = (int)ms_rd(mgr + 0x1c);
    switch (rp.phase) {
    case MR_ARMED:
        mr_begin();
        break;
    case MR_STARTING:
        if (cur == rp.id) {
            rp.phase = MR_UNDER_WAY;
            gmv_trace("[replay] mission %d under way\n", rp.id);
        } else if (now - rp.t > MR_WAIT_MS / 3)
            mr_restore("the mission never got under way");
        break;
    case MR_UNDER_WAY:
        if (cur == rp.id) {
            rp.passed |= st == 5;
            rp.failed |= st == 6;
            break;
        }
        rp.phase = MR_ENDING;
        rp.t = now;
        gmv_trace("[replay] mission %d over (%s)\n", rp.id, rp.passed ? "passed" : rp.failed ? "failed" : "aborted");
        break;
    case MR_ENDING: {
        int sst = (int)ms_rd(rp.sc + 0x18), t = ms_cmd_type(mr_cmd(rp.sc, (int)ms_rd(rp.sc + 8)));
        if (rp.opener && !rp.passed && cur == -1 && sst == 2 && (int)ms_rd(rp.sc + 8) < rp.at) {
            ((void (*)(uint32_t))(uintptr_t)MR_SC_UNDO)(rp.sc);
            sst = 0;
            gmv_trace("[replay] mission %d's script back before its start, running; undone so it does not start again\n",
                    rp.id);
        }
        if (cur == rp.id) {
            rp.phase = MR_UNDER_WAY;
            rp.passed = rp.failed = 0;
            gmv_trace("[replay] mission %d under way again\n", rp.id);
        } else if (cur != -1)
            mr_restore("another mission started");
        else if (((!st && (!sst || t == 0x8f || t == 0x81 || t == 0x63) && now - rp.t >= MR_SETTLE_MS) || now - rp.t > MR_WAIT_MS)
                 && (rp.passed || mf_dark())) {
            mr_restore(rp.passed ? "passed" : rp.failed ? "failed" : "aborted");
            mf_clear(MR_FADE_HOLD_MS);
        }
        break;
    }
    }
}

/* MissionStop 0x64 */
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_unlock(uint32_t cmd)
{
    if (rp.phase >= MR_UNDER_WAY && ms_rd(cmd + 4) == rp.sc && (int)ms_rd(rp.sc + 8) > rp.last_stop) {
        uint32_t target = ((uint32_t (*)(uint32_t, uint32_t, int))(uintptr_t)MR_SC_FIND)(MS_SCRIPTS, ms_rd(cmd + 0x10), 0);
        int s = target ? mr_snap_ix(target) : -1;
        if (s >= 0 && !rp.s_st[s]) {
            gmv_trace("[replay] the mission's end would start script %d; held back\n", s);
            return;
        }
    }
    ((void (*)(uint32_t))(uintptr_t)mr_unlock_prev)(cmd);
}

/* 0x63 */
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_fail_reset(uint32_t scripts, int type)
{
    uint32_t mgr = mr_mgr();
    int replayed = rp.phase == MR_UNDER_WAY && rp.opener && mgr == rp.mgr && (int)ms_rd(mgr + 4) == rp.id;
    ((void (*)(uint32_t, int))(uintptr_t)MR_RESET)(scripts, type);
    if (replayed && (int)ms_rd(rp.sc + 0x18) == 2) {
        int pc = (int)ms_rd(rp.sc + 8);
        ((void (*)(uint32_t))(uintptr_t)MR_SC_UNDO)(rp.sc);
        rp.failed = 1;
        gmv_trace("[replay] mission %d reset after a fail, its script back at %d; undone so the game does not "
                "restart it\n", rp.id, pc);
    }
}

__attribute__((cdecl, force_align_arg_pointer))
int gmv_mr_save(void *save, void *path)
{
    if (rp.phase != MR_IDLE) {
        if (!rp.held++)
            gmv_trace("[replay] a save during the replay waits for its end\n");
        return 0;
    }
    return (int)gmv_ssf_write((uint32_t)(uintptr_t)save, (uint32_t)(uintptr_t)path);
}

static const char *mr_str(int id)
{
    void *table = *(void **)((unsigned char *)om_app() + MS_STRINGS);
    return ((const char *(*)(void *, int))(uintptr_t)OM_STRING_FN)(table, id);
}

static int mr_key(uint32_t rec, uint32_t mgr)
{
    int i = (int)ms_rd(rec + 4), j = (int)ms_rd(rec + 0x1c), k = (int)ms_rd(rec + 0x18);
    if (i < 1 || i > 8 || j < 1 || j > 6 || k < 1 || k > 5 || (i == 6 ? 2 : i >= 7 ? 1 : 0) != mr_cat)
        return -1;
    int bit = 5 * (j - 1) + k - 1;
    if (!(ms_rd(mgr + 0x8c + 4 * i) >> bit & 1))
        return -1;
    return i * 32 + bit;
}

static void mr_collect(void)
{
    uint32_t owner = ms_rd(MS_OWNER), mgr = owner ? ms_rd(owner + 4) : 0;
    uint32_t recs = mgr ? ms_rd(mgr + 0x50) : 0;
    int n = recs ? (int)ms_rd(mgr + 0x54) : 0, key[MR_MAX];
    mr_n = 0;
    for (int r = 0; r < n && r < 1000; r++) {
        uint32_t rec = ms_rd(recs + 4 * r);
        int k = rec ? mr_key(rec, mgr) : -1, p;
        if (k < 0 || mr_n == MR_MAX)
            continue;
        for (p = mr_n++; p > 0 && key[p - 1] > k; p--)
            key[p] = key[p - 1], mr_list[p] = mr_list[p - 1];
        key[p] = k;
        mr_list[p] = rec;
    }
    if (mr_cur >= mr_n)
        mr_cur = 0;
}

/* widgetMission 0x179baa */
static void mr_fill(void)
{
    unsigned char *wm = om_widget(MR_W_MISSION), *name = om_widget(MR_W_NAME);
    if (!wm)
        return;
    void *fx = *(void **)(wm + 8);
    static const char *const fields[] = {
        "widgetMission.txtLocationTitle", "widgetMission.txtLocationValue", "widgetMission.txtTypeTitle",
        "widgetMission.txtTypeValue", "widgetMission.txtCashRewardTitle", "widgetMission.txtCashRewardValue",
    };
    uint32_t rec = mr_cur < mr_n ? mr_list[mr_cur] : 0;
    const char *msg = rec && !mr_script(mr_rec_id(rec, mr_mgr()), NULL)
        ? "THIS MISSION CANNOT BE REPLAYED"
        : rec && rec == mr_msg_rec ? mr_msg : NULL;
    om_visible(fx, "widgetMission.txtMessage", msg != NULL);
    if (msg)
        om_text(fx, "widgetMission.txtMessage", msg);
    om_visible(fx, "widgetMission.btnAbortMission", 0);
    om_visible(fx, "widgetMission.btnStartMission", rec && !msg);
    om_visible(fx, "widgetMission.iconNPC", rec != 0);
    om_visible(fx, "widgetMission.txtObjective", 1);
    for (size_t f = 0; f < sizeof fields / sizeof *fields; f++)
        om_visible(fx, fields[f], rec != 0);
    wm[0x1c] = rec != 0;
    if (name) {
        ((void (*)(void *, int))(uintptr_t)MR_SET_ID)(name, rec ? (int)ms_rd(rec + 0x10) : MS_NONE_ID);
        ((void (*)(void *))(uintptr_t)(*(uint32_t **)name)[OM_W_TEXTS])(name);
    }
    if (!rec) {
        static const char *const none[3] = {
            "NO COMPLETED STORY MISSIONS YET", "NO COMPLETED SIDE MISSIONS YET", "NO COMPLETED RACES YET" };
        om_text(fx, "widgetMission.txtObjective", none[mr_cat]);
        return;
    }
    void *table = *(void **)((unsigned char *)om_app() + MS_STRINGS);
    const char *giver = (const char *)(uintptr_t)ms_rd(rec + 0xc);
    om_text(fx, "widgetMission.btnStartMission.text", mr_str(0x1043));
    om_text(fx, "widgetMission.txtObjective", mr_str((int)ms_rd(rec + 0x24)));
    om_text(fx, fields[0], mr_str(0x1047));
    om_text(fx, fields[1], mr_str((int)ms_rd(rec + 0x14)));
    om_text(fx, fields[2], mr_str(0x1048));
    om_text(fx, fields[3], mr_str((int)ms_rd(rec + 0x28)));
    om_text(fx, fields[4], mr_str(0x104b));
    om_text(fx, fields[5], ((const char *(*)(void *, int, int, int))(uintptr_t)MR_MONEY)(table, (int)ms_rd(rec), 0, 1));
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)OM_SET_TEXT)(
        fx, "widgetMission.iconNPC.question", giver && !strcmp(giver, "Nobody") ? "?" : "", 0);
    if (giver)
        ((void (*)(void *, const char *, const char *, int))(uintptr_t)MR_FRAME)(fx, "widgetMission.iconNPC", giver, 0);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_wm_texts(unsigned char *w)
{
    if (mr_page == MR_SLOT)
        mr_fill();
    else
        ((void (*)(void *))(uintptr_t)MR_WM_TEXTS)(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_wm_event(unsigned char *w, unsigned char *ev)
{
    if (mr_page != MR_SLOT) {
        ((void (*)(void *, void *))(uintptr_t)MR_WM_EVENT)(w, ev);
        return;
    }
    const char *name = *(const char **)(ev + 4);
    if (*(int *)(ev + 8) != 6 || ev[0x24] || !name || strcmp(name, "btnStartMission") || mr_cur >= mr_n)
        return;
    ev[0x24] = 1;
    uint32_t rec = mr_list[mr_cur], mgr = mr_mgr();
    int id = mr_rec_id(rec, mgr), at = 0;
    uint32_t sc = mr_script(id, &at);
    const char *why = NULL;
    if (!mr_unlock_prev)
        why = "THE REPLAY COULD NOT BE SET UP IN THIS GAME";
    else if (rp.phase != MR_IDLE)
        why = "A REPLAY IS STILL UNDER WAY";
    else if ((int)ms_rd(mgr + 4) != -1 || ms_rd(mgr + 0x1c))
        why = "FINISH OR ABORT THE CURRENT MISSION FIRST";
    else if (!sc)
        why = "THIS MISSION CANNOT BE REPLAYED";
    else if (ms_rd(sc + 0x18))
        why = "THIS MISSION IS ON OFFER ALREADY: START IT FROM MISSIONS";
    else if (ms_cmd_type(mr_cmd(sc, at)) == 0x8f &&
             !((uint32_t (*)(void *))(uintptr_t)MR_OBJ_FIND)(((void *(*)(uint32_t))(uintptr_t)MR_CP_NAME)(mr_cmd(sc, at))))
        why = "ITS STARTING POINT IS NOT IN THE WORLD";
    char t[64];
    ms_text(ms_rd(rec + 0x24), t, sizeof t);
    if (why) {
        om_sound(0x54);
        mr_msg_rec = rec, mr_msg = why;
        mr_fill();
        gmv_trace("[replay] START on %s (mission %d): %s\n", t, id, why);
        return;
    }
    om_sound(0x57);
    memset(&rp, 0, sizeof rp);
    rp.id = id, rp.sc = sc, rp.at = at, rp.mgr = mgr, rp.list = ms_rd(MS_SCRIPTS + 0x2dc), rp.last_stop = -1;
    for (int i = 0; i < (int)ms_rd(sc + 4); i++)
        if (ms_cmd_type(mr_cmd(sc, i)) == 0x64)
            rp.last_stop = i;
    rp.phase = MR_ARMED;
    gmv_trace("[replay] START on %s (mission %d): its script's %s at command %d, its end after %d\n", t, id,
            ms_cmd_type(mr_cmd(sc, at)) == 0x63 ? "start" : "checkpoint", at, rp.last_stop);
}

static void mr_take_widget(void)
{
    unsigned char *wm = om_widget(MR_W_MISSION);
    if (wm && *(uint32_t *)wm == MR_WM_VT)
        *(uint32_t **)wm = mr_wm_vt + 2;
}

static void mr_cat_texts(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    if (mr_page == TOD_SLOT) {
        tod_header(fx);
        return;
    }
    om_text(fx, "widgetActionKeyTitle.titleAction", "");
    om_text(fx, "widgetActionKeyTitle.titleKey", "");
    om_text(fx, "widgetActionKeyTitle.txtValue", mr_cat_name[mr_cat]);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_cat_texts(unsigned char *w)
{
    if (mr_page)
        mr_cat_texts(w);
    else
        ((void (*)(void *))(uintptr_t)mr_cat_prev[OM_W_TEXTS])(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_cat_keep(unsigned char *w)
{
    if (!mr_page) {
        ((void (*)(void *))(uintptr_t)mr_cat_prev[OM_W_KEEP])(w);
        return;
    }
    mr_cat_texts(w);
    ((void (*)(void *))(uintptr_t)OM_KEEP)(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_cat_event(unsigned char *w, unsigned char *ev)
{
    if (!mr_page) {
        ((void (*)(void *, void *))(uintptr_t)mr_cat_prev[OM_W_EVENT])(w, ev);
        return;
    }
    const char *name = *(const char **)(ev + 4), *path;
    if (*(int *)(ev + 8) != 6 || ev[0x24] || !name)
        return;
    int d = !strcmp(name, "btnRight") ? 1 : !strcmp(name, "btnLeft") ? -1 : 0;
    if (!d || !(path = om_event_path(ev)) || strncmp(path, "widgetActionKeyTitle.", 21))
        return;
    ev[0x24] = 1;
    om_sound(0x56);
    if (mr_page == TOD_SLOT) {
        tod_step(d);
        mr_cat_texts(w);
        return;
    }
    unsigned char *pager = om_widget(OM_W_PAGER);
    mr_pos[mr_cat] = mr_cur;
    mr_cat = (mr_cat + d + 3) % 3;
    mr_cur = mr_pos[mr_cat];
    mr_collect();
    mr_cat_texts(w);
    if (pager)
        om_pages(pager, mr_n, mr_cur);
    mr_fill();
    gmv_trace("[replay] %s: %d completed\n", mr_cat_name[mr_cat], mr_n);
}

static int mr_take_cat(void)
{
    typedef void (*move_fn)(void *, const char *, int, int);
    unsigned char *cat = om_widget(OM_W_CAT), *ch;
    if (!cat || !(ch = *(unsigned char **)(cat + 4)))
        return 0;
    if (*(uint32_t **)cat != mr_cat_vt + 2) {
        mr_cat_prev = *(uint32_t **)cat;
        memcpy(mr_cat_vt, mr_cat_prev - 2, sizeof mr_cat_vt);
        mr_cat_vt[2 + OM_W_KEEP] = (uint32_t)(uintptr_t)gmv_mr_cat_keep;
        mr_cat_vt[2 + OM_W_EVENT] = (uint32_t)(uintptr_t)gmv_mr_cat_event;
        mr_cat_vt[2 + OM_W_TEXTS] = (uint32_t)(uintptr_t)gmv_mr_cat_texts;
        *(uint32_t **)cat = mr_cat_vt + 2;
        const float *m = *(const float **)(ch + 0x5c);
        mr_cat_home[0] = m[2];
        mr_cat_home[1] = m[5];
    }
    int tod = mr_page == TOD_SLOT, x = tod ? MR_TOD_CAT_X : MR_CAT_X, y = tod ? MR_TOD_CAT_Y : MR_CAT_Y;
    ((move_fn)(uintptr_t)OM_MOVE)(*(void **)(cat + 8), "widgetActionKeyTitle", x, y);
    ((move_fn)(uintptr_t)OM_MOVE)(*(void **)(cat + 8), MR_KEY_PATH, tod ? MR_TOD_KEY_X : MR_KEY_X, MR_KEY_Y);
    return 1;
}

static void mr_give_cat(void)
{
    unsigned char *cat = om_widget(OM_W_CAT), *ch;
    if (!cat || *(uint32_t **)cat != mr_cat_vt + 2)
        return;
    if (cat[0x10])
        ((void (*)(void *, int))(uintptr_t)mr_cat_vt[2 + OM_W_HIDE])(cat, 0);
    ((void (*)(void *, const char *, int, int))(uintptr_t)OM_MOVE)(*(void **)(cat + 8), MR_KEY_PATH, MR_KEY_X, MR_KEY_Y);
    ((void (*)(void *, const char *, int, int))(uintptr_t)OM_MOVE)(
        *(void **)(cat + 8), "widgetActionKeyTitle", (int)(mr_cat_home[0] / 20), (int)(mr_cat_home[1] / 20));
    if ((ch = *(unsigned char **)(cat + 4))) {
        float *m = *(float **)(ch + 0x5c);
        m[2] = mr_cat_home[0];
        m[5] = mr_cat_home[1];
        ch[0xa9] = 1;
    }
    *(uint32_t **)cat = mr_cat_prev;
}

static int mr_on(void)
{
    const char *e = gmv_config_live("GMV_MISSION_REPLAY");
    return gmv_env_on(e);
}

int mr_busy(void)
{
    uint32_t mgr = mr_mgr();
    return rp.phase != MR_IDLE || (mgr && ((int)ms_rd(mgr + 4) != -1 || ms_rd(mgr + 0x1c)));
}

static int mr_tab_on(int slot)
{
    return (slot == MR_SLOT ? mr_on() : slot == TOD_SLOT && mr_tod_page && tod_on()) && !mr_busy();
}

static void mr_tab(void *fx, const char *path, int visible, int slot, int id)
{
    unsigned char *tabs = om_widget(OM_W_TABS);
    if (tabs && mr_tab_on(slot)) {
        ((void (*)(void *, int, int, int))(uintptr_t)OM_SET_ENTRY)(tabs, slot, id, 1);
        visible = 1;
    }
    om_visible(fx, path, visible);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_tab6(void *fx, const char *path, int visible)
{
    mr_tab(fx, path, visible, MR_SLOT, MR_TAB_ID);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_tab5(void *fx, const char *path, int visible)
{
    mr_tab(fx, path, visible, TOD_SLOT, TOD_TAB_ID);
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_mr_clicked(unsigned char *tabs)
{
    int n = *(int *)(tabs + 0x58);
    if ((n != MR_SLOT && n != TOD_SLOT) || !mr_tab_on(n))
        return n;
    om_sound(0x5a);
    if (mr_page != n)
        ((void (*)(void *, const char *, int))(uintptr_t)OM_SWITCH_FN)(om_mgr(), n == MR_SLOT ? MR_STATE : MR_TOD_STATE, 0);
    return 4;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_show(unsigned char *self)
{
    unsigned char *tabs = om_widget(OM_W_TABS), *pager = om_widget(OM_W_PAGER), *wm = om_widget(MR_W_MISSION);
    mr_take_widget();
    mr_collect();
    mr_msg_rec = 0;
    mr_page = pager && wm && *(uint32_t **)wm == mr_wm_vt + 2 ? MR_SLOT : 0;
    if (mr_page)
        mr_take_cat();
    if (pager) {
        ((void (*)(void *, void *))(uintptr_t)OM_LISTEN)(pager, self);
        *(int *)(pager + 0x18) = mr_cur;
    }
    ((void (*)(void *))(uintptr_t)MR_SV_SHOW)(self);
    if (tabs)
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, MR_SLOT, 1);
    if (!mr_page) {
        gmv_log("[replay] the MISSIONS page's widgets are not the expected ones\n");
        return;
    }
    om_pages(pager, mr_n, mr_cur);
    mr_fill();
    gmv_trace("[replay] MISSION REPLAY page, %s: %d completed\n", mr_cat_name[mr_cat], mr_n);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_tod_show(unsigned char *self)
{
    unsigned char *tabs = om_widget(OM_W_TABS), *cat = om_widget(OM_W_CAT);
    mr_page = TOD_SLOT;
    if (!mr_take_cat())
        mr_page = 0;
    ((void (*)(void *))(uintptr_t)MR_SV_SHOW)(self);
    if (tabs)
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, TOD_SLOT, 1);
    if (!mr_page) {
        gmv_log("[time] the SERVICES header widget is not the expected one\n");
        return;
    }
    mr_cat_texts(cat);
    tod_log("TIME OF DAY page");
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_hide(unsigned char *self)
{
    ((void (*)(void *))(uintptr_t)MR_SM_HIDE)(self);
    if (mr_page != (self == mr_tod_page ? TOD_SLOT : MR_SLOT))
        return;
    mr_give_cat();
    mr_page = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_mr_event(unsigned char *self, unsigned char *w, int type)
{
    if (((unsigned char (*)(void *, void *, int))(uintptr_t)MR_SV_EVENT)(self, w, type))
        return 1;
    unsigned char *pager = om_widget(OM_W_PAGER);
    if (mr_page != MR_SLOT || !w || w != pager || (type != 1 && type != 2))
        return 0;
    int i = *(int *)(pager + 0x18);
    mr_cur = i >= 0 && i < mr_n ? i : 0;
    mr_msg_rec = 0;
    mr_fill();
    return 1;
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
static unsigned char *mr_new_page(const char *name, uint32_t *vt)
{
    unsigned char *s = ((unsigned char *(*)(uint32_t, const char *, int, int))(uintptr_t)OM_ALLOC)(
        MR_SM_BYTES, "the patches", 1096, 1);
    if (!s) {
        gmv_log("[replay] no memory for the %s page\n", name);
        return NULL;
    }
    ((void (*)(void *))(uintptr_t)MR_SM_CTOR)(s);
    strcpy((char *)s + 8, name);
    *(uint32_t **)s = vt;
    return s;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mr_register(void *menufx, void *state, const char *clip)
{
    typedef void (*reg_fn)(void *, void *, const char *);
    ((reg_fn)(uintptr_t)OM_REG_FN)(menufx, state, clip);
    unsigned char *s = mr_new_page(MR_STATE, mr_vt), *t;
    int nids = s ? *(int *)(s + 0x60) : 0;
    if (s)
        ((void (*)(void *, int))(uintptr_t)MR_IDS_RESIZE)(s + 0x5c, nids + 1);
    if (!s || *(int *)(s + 0x60) != nids + 1) {
        gmv_log("[replay] the MISSION REPLAY page's widget list did not grow; no MISSION REPLAY tab\n");
        return;
    }
    (*(uint32_t **)(s + 0x5c))[nids] = OM_W_CAT;
    ((reg_fn)(uintptr_t)OM_REG_FN)(menufx, s, clip);
    mr_take_widget();
    gmv_log("[replay] SERVICES menu: MISSION REPLAY tab registered in slot %d\n", MR_SLOT);
    uint32_t *ids = (t = mr_new_page(MR_TOD_STATE, mr_tod_vt)) ? *(uint32_t **)(t + 0x5c) : NULL;
    if (ids && *(int *)(t + 0x60) == MR_SM_IDS && ids[MR_SM_IDS - 3] == MR_W_MISSION &&
        ids[MR_SM_IDS - 2] == OM_W_PAGER && ids[MR_SM_IDS - 1] == MR_W_NAME) {
        ids[MR_SM_IDS - 3] = OM_W_CAT;
        ((void (*)(void *, int))(uintptr_t)MR_IDS_RESIZE)(t + 0x5c, MR_SM_IDS - 2);
    }
    if (!ids || *(int *)(t + 0x60) != MR_SM_IDS - 2) {
        gmv_log("[time] the SERVICES page's widget list is not the expected one; no TIME OF DAY tab\n");
        return;
    }
    ((reg_fn)(uintptr_t)OM_REG_FN)(menufx, t, clip);
    mr_tod_page = t;
    gmv_log("[time] SERVICES menu: TIME OF DAY tab registered in slot %d\n", TOD_SLOT);
}

static int mr_menu_in;

void mr_save_hooks(void)
{
    if (!mr_menu_in || gmv_config_mac() || mr_unlock_prev)
        return;
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    /* 0x6240 */
    static const uint32_t saves[] = { 0x00006240u, 0x000326f1u, 0x000a1547u, 0x000aa9a6u, 0x000f3624u,
                                      0x000f6b33u, 0x0016471fu, 0x001648d6u, 0x00164a0bu, 0x00164bd6u };
    uint32_t prev = *(const uint32_t *)(uintptr_t)MR_UNLOCK_SLOT;
    int ok = prev == MR_UNLOCK || (MR_ML_UNLOCK && prev == MR_ML_UNLOCK);
#define MR_LIVE(a) ((a) < GMV_RESERVE_START ? (a) + GMV_LOW_DELTA : (a))
    for (size_t i = 0; ok && i < sizeof saves / sizeof *saves; i++)
        ok = sm_is_call(MR_LIVE(saves[i]), MR_SAVE) ||
             sm_is_call(MR_LIVE(saves[i]), (uint32_t)(uintptr_t)gmv_ssf_write);
    if (ok) {
        uint32_t v = (uint32_t)(uintptr_t)gmv_mr_unlock;
        for (size_t i = 0; ok && i < sizeof saves / sizeof *saves; i++)
            ok = sm_redirect(MR_LIVE(saves[i]), (void *)gmv_mr_save);
#undef MR_LIVE
        if (ok && patch_bytes(MR_UNLOCK_SLOT, (const unsigned char *)&v, 4))
            mr_unlock_prev = prev;
    }
    if (mr_unlock_prev && (!sm_is_call(MR_FAIL_SITE, MR_RESET) || !sm_redirect(MR_FAIL_SITE, (void *)gmv_mr_fail_reset)))
        gmv_log("[replay] the fail reset is not the expected call; a chapter's first mission restarts on a fail\n");
    patch_undo_now = keep;
}

void apply_mission_replay(void)
{
    if (!mr_on() && !tod_on() && !tab_live())
        return;
    static const unsigned char table_cmp[4] = { 0x83, 0xf8, 0x03, 0x0f };
    const uint32_t *vt = (const uint32_t *)(uintptr_t)MR_SM_VT, *wv = (const uint32_t *)(uintptr_t)MR_WM_VT;
    const uint32_t *cv = (const uint32_t *)(uintptr_t)OM_CAT_VT;
    if (!sm_is_call(MR_HIDE6_SITE, OM_SET_VISIBLE) || !sm_is_call(MR_HIDE5_SITE, OM_SET_VISIBLE) ||
        !sm_is_call(MR_CLICK_SITE, OM_CLICK_FN) ||
        !sm_is_call(MR_REG_SITE, OM_REG_FN) || !sm_is_call(MR_IDS_SITE, MR_IDS_RESIZE) ||
        memcmp((const void *)(uintptr_t)MR_TABLE_CMP, table_cmp, 4) ||
        vt[OM_VT_SHOW] != MR_SM_SHOW || vt[OM_VT_HIDE] != MR_SM_HIDE || vt[OM_VT_EVENT] != MR_SM_EVENT ||
        wv[OM_W_EVENT] != MR_WM_EVENT || wv[OM_W_TEXTS] != MR_WM_TEXTS || wv[MR_WM_SLOTS] != 0 ||
        cv[OM_W_SHOW] != OM_PLAIN_SHOW || cv[OM_W_HIDE] != OM_PLAIN_HIDE || cv[OM_W_KEEP] != OM_KEEP ||
        cv[OM_W_EVENT] != OM_CAT_EVENT || cv[OM_W_TEXTS] != OM_CAT_TEXTS) {
        gmv_log("[replay] SERVICES menu not the expected bytes; no MISSION REPLAY tab\n");
        return;
    }
    memcpy(mr_wm_vt, wv - 2, sizeof mr_wm_vt);
    mr_wm_vt[2 + OM_W_EVENT] = (uint32_t)(uintptr_t)gmv_mr_wm_event;
    mr_wm_vt[2 + OM_W_TEXTS] = (uint32_t)(uintptr_t)gmv_mr_wm_texts;
    memcpy(mr_vt, vt, sizeof mr_vt);
    mr_vt[OM_VT_SHOW] = (uint32_t)(uintptr_t)gmv_mr_show;
    mr_vt[OM_VT_HIDE] = (uint32_t)(uintptr_t)gmv_mr_hide;
    mr_vt[OM_VT_EVENT] = (uint32_t)(uintptr_t)gmv_mr_event;
    memcpy(mr_tod_vt, vt, sizeof mr_tod_vt);
    mr_tod_vt[OM_VT_SHOW] = (uint32_t)(uintptr_t)gmv_tod_show;
    mr_tod_vt[OM_VT_HIDE] = (uint32_t)(uintptr_t)gmv_mr_hide;
    mr_tod_vt[OM_VT_EVENT] = MR_SV_EVENT;
    if (!tab_label_hook()) {
        gmv_log("[replay] the tab label lookup is not the expected one; no MISSION REPLAY tab\n");
        return;
    }
    if (!sm_redirect(MR_REG_SITE, (void *)gmv_mr_register) || !sm_redirect(MR_HIDE6_SITE, (void *)gmv_mr_tab6) ||
        !sm_redirect(MR_HIDE5_SITE, (void *)gmv_mr_tab5) || !sm_redirect(MR_CLICK_SITE, (void *)gmv_mr_clicked))
        return;
    mr_menu_in = 1;
    mr_save_hooks();
    gmv_log("[replay] a MISSION REPLAY tab in the SERVICES menu, showing the completed missions%s\n",
            !mr_on() ? "; hidden for now" : mr_unlock_prev ? ", START replays one"
                     : "; the game's saves or ScriptUnlock not the expected ones, so no START");
    gmv_log("[time] a TIME OF DAY tab in the SERVICES menu, below MISSION REPLAY%s\n", tod_on() ? "" : "; hidden for now");
}

int om_set_replay(const char *key, double v)
{
    (void)key, (void)v;
    return mr_menu_in;
}
