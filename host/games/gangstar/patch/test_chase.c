#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

#if GMV_DEV
#define TC_LION_TITLE 0x904d
#define TC_SC_ID      0x00032560u
#define TC_ENTITY     0x00034af8u
#define TC_SPEED      341
#define TC_FAST       359
#define TC_PLAYER_IN  2
#define TC_STREAM_MS  5000u
#define TC_TRAIL      256
#define TC_LAG_MS     600u

static const struct { int at, type; } tc_steps[] = {
    { 333, 0x6a }, { 334, 0x6b }, { 336, 0x6a }, { 337, 0x6a }, { 338, 0x6b }, { 339, 0x78 }, { 340, 0x4b },
    { 341, 0x4e }, { 342, 0x9b }, { 343, 0x05 }, { 359, 0x4e },
};
#define TC_STEPS (int)(sizeof tc_steps / sizeof *tc_steps)

static int tc_mode = -1, tc_phase;
static DWORD tc_since;
static uint32_t tc_veh, tc_sc;
static int tc_id, tc_nr, tc_n;
static struct { DWORD t; uc_spot_t s; float v[3]; } tc_trail[TC_TRAIL];
static int tc_head;

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static uint32_t tc_call1(uint32_t fn, uint32_t a)
{
    return ((uint32_t (*)(uint32_t))(uintptr_t)fn)(a);
}

static const char *tc_text(uint32_t p, char *out, size_t n)
{
    size_t o = 0;
    for (uint32_t i = 0; p && i < 40 && o + 1 < n; i++) {
        uint32_t w = ms_rd((p + i) & ~3u);
        unsigned char c = (unsigned char)(w >> 8 * ((p + i) & 3));
        if (c < 0x20 || c > 0x7e)
            break;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return out;
}

static uint32_t tc_cmd(uint32_t sc, int i)
{
    return i >= 0 && i < (int)ms_rd(sc + 4) ? ms_rd(ms_rd(sc + 0x10) + 4 * i) : 0;
}

static uint32_t tc_script(uint32_t mgr)
{
    uint32_t recs = ms_rd(mgr + 0x50), list = ms_rd(MS_SCRIPTS + 0x2dc);
    int nr = recs ? (int)ms_rd(mgr + 0x54) : 0, n = (int)ms_rd(MS_SCRIPTS + 0x2ec), id = -1;
    for (int r = 0; r < nr && r < 1000 && id < 0; r++)
        if (ms_rd(ms_rd(recs + 4 * r) + 0x24) == TC_LION_TITLE)
            id = r;
    for (int s = 0; list && id >= 0 && s < n && s < MS_SC_MAX; s++) {
        uint32_t sc = ms_rd(list + 4 * s);
        if (sc && ((int (*)(uint32_t))(uintptr_t)TC_SC_ID)(sc) == id)
            return sc;
    }
    tc_id = id, tc_nr = nr, tc_n = n;
    return 0;
}

static int tc_ready(void)
{
    for (int i = 0; i < TC_STEPS; i++)
        if ((ms_cmd_type(tc_cmd(tc_sc, tc_steps[i].at)) & 0xff) != tc_steps[i].type) {
            gmv_log("[chase] command %d is not the expected type %02x; no chase\n", tc_steps[i].at, tc_steps[i].type);
            return 0;
        }
    return 1;
}

static void tc_run(int from, int to)
{
    for (int i = from; i < to; i++) {
        uint32_t c = tc_cmd(tc_sc, tc_steps[i].at);
        char a[48], b[48];
        if (tc_steps[i].at == TC_FAST && tc_mode != 2)
            continue;
        gmv_log("[chase] run %d type %02x: \"%s\" \"%s\"\n", tc_steps[i].at, tc_steps[i].type,
                tc_text(ms_rd(c + 0x14), a, sizeof a), tc_text(ms_rd(c + 0x1c), b, sizeof b));
        tc_call1(ms_rd(ms_rd(c)), c);
        if (tc_steps[i].type == 0x6b)
            tc_call1(ms_rd(ms_rd(c) + 4), c);
    }
}

/* +0x250 +0x38 +0x40 +0x48 vt+0x54 vt+0x58 */
static void tc_follow(DWORD now)
{
    uint32_t vb = ms_rd(tc_veh + 0x250), car = ms_rd(ms_rd(MR_PLAYER) + 4 + 0x558), cb;
    if (!vb || !car || !(cb = ms_rd(car + 0x250)))
        return;
    int h = tc_head = (tc_head + 1) % TC_TRAIL;
    uc_spot_t *s = &tc_trail[h].s;
    const float *b = (const float *)(uintptr_t)vb;
    tc_trail[h].t = now;
    uc_pos(tc_veh, s->pos);
    s->ang = b[0x38 / 4];
    s->cell = tc_call1(ms_rd(ms_rd(tc_veh) + 0x54), tc_veh) & 0xffff;
    s->tri = tc_call1(ms_rd(ms_rd(tc_veh) + 0x58), tc_veh) & 0xffff;
    s->have = 1;
    memcpy(tc_trail[h].v, b + 0x40 / 4, sizeof tc_trail[h].v);
    for (int k = 1; k < TC_TRAIL; k++) {
        int i = (h - k + TC_TRAIL) % TC_TRAIL;
        if (!tc_trail[i].s.have)
            return;
        if (now - tc_trail[i].t >= TC_LAG_MS) {
            uc_place(car, &tc_trail[i].s, 0);
            memcpy((float *)(uintptr_t)cb + 0x40 / 4, tc_trail[i].v, sizeof tc_trail[i].v);
            return;
        }
    }
}

#define TB_FROM       371
#define TB_DOCK       384
#define TB_SKIP_AT    388
#define TB_SEEN       383
#define TB_END        411
#define TB_LOOK_MS    2000
#define TB_STUCK_MS   30000
#define TB_SAMPLE_MS  250
#define TB_TAIL_MS    4000

static const struct { int at, type; } tb_steps[] = {
    { 371, 0x5b }, { 372, 0x58 }, { 384, 0x6b }, { 394, 0x1a }, { 395, 0x47 }, { 396, 0x6b }, { 409, 0x59 },
    { 411, 0x93 },
};
#define TB_STEPS (int)(sizeof tb_steps / sizeof *tb_steps)

static int tb_mode = -1, tb_phase, tb_last = -1, tb_skipped;
static DWORD tb_since, tb_at, tb_look, tb_next;

static int tb_age(DWORD now, DWORD t)
{
    return (int)(now - t);
}
static uint32_t tb_sc;

static int tb_ready(void)
{
    for (int i = 0; i < TB_STEPS; i++)
        if ((ms_cmd_type(tc_cmd(tb_sc, tb_steps[i].at)) & 0xff) != tb_steps[i].type) {
            gmv_log("[boat] command %d is not the expected type %02x; no boat test\n", tb_steps[i].at, tb_steps[i].type);
            return 0;
        }
    return 1;
}

static int tb_find(DWORD now)
{
    uint32_t mgr = mr_mgr();
    if (!mgr || !ms_rd(MR_PLAYER) || pointer_shown() || (tb_mode && (int)ms_rd(mgr + 4) != -1)) {
        tb_since = 0;
        return 0;
    }
    if (!tb_since)
        tb_since = now | 1;
    if (tb_age(now, tb_since) < 3000 || (tb_look && tb_age(now, tb_look) < TB_LOOK_MS))
        return 0;
    tb_look = now;
    tb_sc = tc_script(mgr);
    if (!tb_sc) {
        if (tb_mode && tb_age(now, tb_since) >= 30000) {
            gmv_log("[boat] no script for ON A LION HUNT (mission %d of %d, %d scripts)\n", tc_id, tc_nr, tc_n);
            tb_phase = -1;
        }
        return 0;
    }
    if (!tb_mode)
        return 1;
    if (!tb_ready()) {
        tb_phase = -1;
        return 0;
    }
    uint32_t c = tc_cmd(tb_sc, TB_DOCK);
    char a[48], b[48];
    gmv_log("[boat] run %d: \"%s\" \"%s\"\n", TB_DOCK, tc_text(ms_rd(c + 0x14), a, sizeof a),
            tc_text(ms_rd(c + 0x1c), b, sizeof b));
    tc_call1(ms_rd(ms_rd(c)), c);
    tc_call1(ms_rd(ms_rd(c) + 4), c);
    tb_since = now | 1;
    tb_phase = 1;
    return 1;
}

static void tb_skip(int pc)
{
    unsigned char *w = cs_widget();
    void **l;
    if (!w || !w[0x10] || !(l = *(void ***)(w + 0xc)))
        return;
    ((void (*)(void *, void *, int))(*(uintptr_t **)l)[0x3c / 4])(l, w, 1);
    tb_skipped = 1;
    gmv_log("[boat] SKIP pressed at command %d\n", pc);
}

static void tb_tick(DWORD now)
{
    if (tb_mode < 0) {
        const char *e = gmv_dbgenv("GMV_TEST_BOAT");
        tb_mode = e && *e ? atoi(e) : 0;
        if (tb_mode)
            gmv_log("[boat] test aid: Clive's boat escape of ON A LION HUNT once in the city%s\n",
                    tb_mode == 2 ? ", skipped" : "");
    }
    int probe = boat_probe_on();
    if ((!tb_mode && !probe) || tb_phase < 0 || (!tb_sc && !tb_find(now)))
        return;
    if (tb_phase == 1) {
        if (tb_age(now, tb_since) < (int)TC_STREAM_MS)
            return;
        uint32_t c = tc_cmd(tb_sc, TB_FROM);
        int st = (int)ms_rd(tb_sc + 0x18), pc = (int)ms_rd(tb_sc + 8);
        *(volatile uint32_t *)(uintptr_t)(tb_sc + 8) = TB_FROM;
        tc_call1(ms_rd(ms_rd(c)), c);
        *(volatile uint32_t *)(uintptr_t)(tb_sc + 0x18) = 2;
        gmv_log("[boat] cutscene from command %d (the script was in state %d at %d)\n", TB_FROM, st, pc);
        tb_since = tb_at = now | 1;
        tb_phase = 2;
    }
    int pc = (int)ms_rd(tb_sc + 8);
    if (pc != tb_last) {
        if (pc >= TB_SEEN && pc <= TB_END)
            gmv_log("[boat] command %d type %02x\n", pc, ms_cmd_type(tc_cmd(tb_sc, pc)) & 0xff);
        tb_last = pc;
        tb_at = now;
    }
    if (tb_phase == 2) {
        if (tb_mode == 2 && !tb_skipped && pc >= TB_SKIP_AT && pc < TB_END)
            tb_skip(pc);
        if (pc >= TB_END) {
            gmv_log("[boat] cutscene over at command %d%s\n", pc, tb_mode == 2 && !tb_skipped ? ", not skipped" : "");
            tb_since = now | 1;
            tb_phase = 3;
        } else if (tb_age(now, tb_at) > TB_STUCK_MS) {
            gmv_log("[boat] still at command %d type %02x\n", pc, ms_cmd_type(tc_cmd(tb_sc, pc)) & 0xff);
            tb_at = now;
        }
    }
    if (!probe || pc < TB_SEEN || pc > TB_END || tb_age(now, tb_next) < TB_SAMPLE_MS)
        return;
    if (tb_phase == 3 && tb_age(now, tb_since) > TB_TAIL_MS)
        return;
    tb_next = now;
    boat_probe_note(pc);
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
void gmv_test_chase_tick(void)
{
    if (tc_mode < 0) {
        const char *e = gmv_dbgenv("GMV_TEST_CHASE");
        tc_mode = e && *e ? atoi(e) : 0;
        if (tc_mode)
            gmv_log("[chase] test aid: Clive's escape of ON A LION HUNT once in the city%s\n",
                    tc_mode == 2 ? ", at its last speed" : "");
    }
    DWORD now = GetTickCount();
    tb_tick(now);
    if (!tc_mode || tc_phase < 0)
        return;
    if (tc_phase == 0) {
        uint32_t mgr = mr_mgr(), p = ms_rd(MR_PLAYER);
        if (!mgr || !p || pointer_shown() || (int)ms_rd(mgr + 4) != -1) {
            tc_since = 0;
            return;
        }
        if (!tc_since)
            tc_since = now | 1;
        if (now - tc_since < 3000)
            return;
        tc_sc = tc_script(mgr);
        if (!tc_sc && now - tc_since < 30000)
            return;
        if (!tc_sc || !tc_ready()) {
            if (!tc_sc)
                gmv_log("[chase] no script for ON A LION HUNT (mission %d of %d, %d scripts)\n", tc_id, tc_nr, tc_n);
            tc_phase = -1;
            return;
        }
        tc_run(0, TC_PLAYER_IN);
        tc_since = now | 1;
        tc_phase = 1;
        return;
    }
    if (tc_phase == 1) {
        char name[48];
        if (now - tc_since < TC_STREAM_MS)
            return;
        tc_run(TC_PLAYER_IN, TC_STEPS);
        tc_text(ms_rd(tc_cmd(tc_sc, TC_SPEED) + 0x18), name, sizeof name);
        tc_veh = tc_call1(TC_ENTITY, (uint32_t)(uintptr_t)name);
        gmv_log("[chase] %s is %08x, its driver %08x\n", name, tc_veh, tc_veh ? ms_rd(tc_veh + 0x280) : 0);
        tc_phase = tc_veh ? 2 : -1;
        return;
    }
    tc_follow(now);
}
#else
void gmv_test_chase_tick(void) {}
#endif
