#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

#define UC_ENG_SITE   0x00114f17u    /* cmp eax,0xc; ja 0x114fcc */
#define UC_ENG_DONE   0x00114fccu
#define UC_NEAR       15000.0f
#define UC_FAR        20000.0f
#define LC_ENTITY 0x00034af8u

static void uc_hooks_out(void);

/* LC_SWITCH LC_00 0x655de8 0x655e9c 0x8f */
#define LC_PLACE_EXEC   0x000ced20u   /* 0x6a ObjectEnable +0x14 +0x1c */
#define LC_HIDE_EXEC    0x000cec5au   /* 0x69 ObjectDisable +0x14 */
#define LC_MOVE_EXEC    0x000cefc0u   /* 0x6b ObjectMoveTo +0x14 +0x1c */
#define LC_MOVE_END     0x000cf2e8u
#define LC_RELEASE_EXEC 0x000cf3d0u   /* 0x6c ObjectRelease +0x14 +4 0x800 */
#define LC_STOP_EXEC    0x000cf610u   /* 0x6f ObjectStop +0x14 */
#define LC_FOLLOW_EXEC  0x000c8364u   /* 0x0f ActorFollow +0x14 +0x1c */
#define LC_WAIT_SHOW    0x000d0fe2u   /* 0x8f StoryWaitCheckpoint +0x14 +0x18 -1 */
#define LC_WAIT_TEST    0x000d109au   /* 0xd6bb0 */
#define LC_WAIT_DONE    0x000d102cu
#define LC_WAIT_UNDO    0x000d10c2u
#define LC_SWITCH_FN    0x000f6b48u
#define LC_CAMERA       0x0004547eu
#define LC_CAMERA_SET   0x00044b88u
#define LC_CONTROLS_AT  0x00017ac8u   /* 0x31b0() +0x1c */
#define LC_CONTROLS_FREE 0x00028078u  /* +0x18 */
#define LC_CONTROLS_FOOT 0x000284f6u
#define LC_SCRIPTS      0x00035d08u   /* +0xc */
#define LCS_NAME       "LC_00"
#define LCS_JOHNNY     "Johnny_00"
#define LCS_POINT      "Checkpoint_LC_Hotel_00"
#define LCS_ARRIVE     "Marker_C3M06_LC_00"
#define LCS_LEFT       "Marker_C3M06_GG_00"
#define LC_RADIUS      150
#define LC_HOME_RADIUS 300
#define LC_SETTLE_MS   3000u
#define LC_FADE_MS     200

static volatile int g_lc_on;
static uint32_t lc_char;
static int lc_marker, lc_moved, lc_as, lc_fade, lc_alpha, lc_said;
static DWORD lc_idle_since, lc_checked, lc_fade_at, lc_cam_until;
static uint32_t lc_cam_char;
static int lc_cam_ours;
static uint32_t lc_ctx[64];           /* 0x32838 +0x14 */
static uint32_t lc_wait[12], lc_wait_ctx[64];   /* 0x8f */

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static uint32_t lc_call1(uint32_t fn, uint32_t a)
{
    return ((uint32_t (*)(uint32_t))(uintptr_t)fn)(a);
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static void lc_call_vt_6c(uint32_t obj, const float *pos)
{
    ((void (*)(uint32_t, const float *))(uintptr_t)ms_rd(ms_rd(obj) + 0x6c))(obj, pos);
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static uint32_t lc_call0(uint32_t fn)
{
    return ((uint32_t (*)(void))(uintptr_t)fn)();
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static void lc_controls_foot_call(uint32_t ctl)
{
    ((void (*)(uint32_t, int))(uintptr_t)LC_CONTROLS_FOOT)(ctl, 1);
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static void lc_camera_set(uint32_t mgr, uint32_t whom)
{
    ((void (*)(uint32_t, int, uint32_t, float, int))(uintptr_t)LC_CAMERA_SET)(mgr, 0, whom, -1.0f, 0);
}

static uint32_t lc_find(const char *name)
{
    return lc_call1(LC_ENTITY, (uint32_t)(uintptr_t)name);
}

/* +4 +0x14 +0x1c */
static void lc_command(uint32_t exec, const char *at14, const char *at1c)
{
    uint32_t cmd[12] = { 0 };
    memset(lc_ctx, 0, sizeof lc_ctx);
    cmd[1] = (uint32_t)(uintptr_t)lc_ctx;
    cmd[5] = (uint32_t)(uintptr_t)at14;
    cmd[7] = (uint32_t)(uintptr_t)at1c;
    lc_call1(exec, (uint32_t)(uintptr_t)cmd);
}

static int lc_all_done(uint32_t mgr, int *done, int *all)
{
    uint32_t recs = ms_rd(mgr + 0x50);
    int n = recs ? (int)ms_rd(mgr + 0x54) : 0;
    *done = *all = 0;
    for (int r = 0; r < n && r < 1000; r++) {
        uint32_t rec = ms_rd(recs + 4 * r);
        int i = (int)ms_rd(rec + 4), j = (int)ms_rd(rec + 0x1c), k = (int)ms_rd(rec + 0x18);
        if (!rec || i < 1 || i > 7 || j < 1 || j > 6 || k < 1 || k > 5)
            continue;
        ++*all;
        *done += (int)(ms_rd(mgr + 0x8c + 4 * i) >> (5 * (j - 1) + k - 1) & 1);
    }
    return *all > 0 && *done == *all;
}

static void lc_switch(uint32_t player, const char *why)
{
    gmv_trace("[lcswitch] %s\n", why);
    lc_call1(LC_SWITCH_FN, player);
}

int lc_set(int on)
{
    if (on != g_lc_on)
        gmv_log("[gamepatch] LC_SWITCH %s\n", on ? "on" : "off");
    g_lc_on = on;
    return 1;
}

static const float lc_point[3] = { 71920.f, -24900.f, 203.f };

static void lc_marker_up(void)
{
    memset(lc_wait_ctx, 0, sizeof lc_wait_ctx);
    memset(lc_wait, 0, sizeof lc_wait);
    lc_wait[1] = (uint32_t)(uintptr_t)lc_wait_ctx;
    lc_wait[5] = (uint32_t)(uintptr_t)LCS_POINT;
    lc_wait[6] = LC_RADIUS;
    lc_call1(LC_WAIT_SHOW, (uint32_t)(uintptr_t)lc_wait);
    lc_call_vt_6c(lc_wait_ctx[0x14 / 4], lc_point);
    lc_marker = lc_moved = 1;
}

static void lc_point_home(void)
{
    static const float home[3] = { 72183.f, -24453.f, 192.f };
    uint32_t cp = lc_wait_ctx[0x14 / 4];
    if (lc_moved && cp && lc_find(LCS_POINT) == cp) {
        lc_call_vt_6c(cp, home);
        *(int32_t *)(uintptr_t)(cp + 0x4c) = LC_HOME_RADIUS;
    }
    lc_moved = 0;
}

static void lc_marker_down(void)
{
    uint32_t cp = lc_wait_ctx[0x14 / 4];
    if (lc_marker && cp && (ms_rd(cp + 4) & 1))
        lc_call1(LC_WAIT_UNDO, (uint32_t)(uintptr_t)lc_wait);
    lc_marker = 0;
    lc_point_home();
}

/* +0x18 0x1911f 0x28084 0xf95b5 0x73949 0xf8802 +0x588 */
static void lc_controls_free(void)
{
    uint32_t ctl = ms_rd(lc_call0(0x000031b0u) + LC_CONTROLS_AT);
    if (ctl && (ms_rd(ctl + 0x18) & 0xff) && !ms_rd(lc_call0(LC_SCRIPTS) + 0xc)) {
        gmv_trace("[lcswitch] the controls were locked by the one left behind: freed\n");
        lc_call1(LC_CONTROLS_FREE, ctl);
    }
}

/* 0x284f6 */
static void lc_controls_foot(uint32_t whom)
{
    uint32_t ctl = ms_rd(lc_call0(0x000031b0u) + LC_CONTROLS_AT);
    if (ctl && (int)ms_rd(ctl + 0x1c) == 3 && ms_rd(ctl) != ms_rd(ctl + 4) && !ms_rd(whom + 0x558)) {
        gmv_trace("[lcswitch] on foot, but the controls' handler is %08x, not the on-foot %08x: restarted\n",
                ms_rd(ctl), ms_rd(ctl + 4));
        lc_controls_foot_call(ctl);
    }
}

/* 0x284f6 0x11ceb4 */
static void lc_camera(uint32_t whom)
{
    uint32_t mgr = lc_call0(LC_CAMERA), cam = mgr ? ms_rd(mgr + 8) : 0, on = cam ? ms_rd(cam + 4) : 0;
    uint32_t ctl = ms_rd(lc_call0(0x000031b0u) + LC_CONTROLS_AT);
    if (!cam)
        return;
    if (on != whom && !(ms_rd(whom + 0x584) & 0x2000400u)) {
        gmv_trace("[lcswitch] the camera follows %08x, not %08x; controls' state %d, active %d, their target %08x, vehicle %08x: given\n",
                on, whom, ctl ? (int)ms_rd(ctl + 0x1c) : -1, ctl ? (int)(ms_rd(ctl + 0x18) & 0xffff) : -1,
                ctl && ms_rd(ctl) ? ms_rd(ms_rd(ctl) + 0x34) : 0, ms_rd(whom + 0x558));
        lc_camera_set(mgr, whom);
    }
    /* 0x44b88 +0x58 +0x26 0x455a0 0x44640 0xf8afc 0x44262 */
    if (*(const unsigned char *)(uintptr_t)(mgr + 0x26) && *(const float *)(uintptr_t)(mgr + 0x58) < 0.f) {
        gmv_trace("[lcswitch] a cut into the camera's blend would leave it still: blend ended\n");
        *(unsigned char *)(uintptr_t)(mgr + 0x26) = 0;
    }
}

#if GMV_DEV
static DWORD lc_trace_until, lc_trace_next;
static uint32_t lc_trace_on;
static int lc_trace_kind;

/* 0x41ad8 */
static void lc_cam_trace(uint32_t whom, DWORD now)
{
    static const unsigned char at[] = {0xc, 0x10, 0x14, 0x20, 0x18, 0x1c};
    uint32_t mgr, cam, on, ctl;
    int kind = -1;
    unsigned i;
    float p[3], dx, dy, dz;
    const float *look, *eye;
    if (!lc_trace_until)
        return;
    if ((int)(now - lc_trace_until) >= 0) {
        lc_trace_until = 0;
        return;
    }
    mgr = lc_call0(LC_CAMERA);
    cam = mgr ? ms_rd(mgr + 8) : 0;
    if (!cam)
        return;
    for (i = 0; i < sizeof at; i++)
        if (ms_rd(mgr + at[i]) == cam)
            kind = (int)i;
    on = ms_rd(cam + 4);
    if (kind == lc_trace_kind && on == lc_trace_on && (int)(now - lc_trace_next) < 0)
        return;
    lc_trace_kind = kind;
    lc_trace_on = on;
    lc_trace_next = now + 1000;
    ctl = ms_rd(lc_call0(0x000031b0u) + LC_CONTROLS_AT);
    lc_pos(whom, p);
    eye = (const float *)(uintptr_t)(cam + 8);
    look = (const float *)(uintptr_t)(cam + 0x14);
    dx = look[0] - p[0];
    dy = look[1] - p[1];
    dz = look[2] - p[2];
    gmv_trace("[lcswitch] camera kind %d on %08x (player %08x, vehicle %08x) at %.0f %.0f %.0f looks %.0f %.0f %.0f, player %.0f %.0f %.0f, "
            "off %.0f; manager %06x, its flag %d; controls' state %d, on-foot handler %d\n",
            kind, on, whom, ms_rd(whom + 0x558), eye[0], eye[1], eye[2], look[0], look[1], look[2], p[0], p[1], p[2],
            sqrt(dx * dx + dy * dy + dz * dz), ms_rd(mgr + 0x24) & 0xffffff, ms_rd(cam + 0x20) & 0xff,
            ctl ? (int)ms_rd(ctl + 0x1c) : -1, ctl ? ms_rd(ctl) == ms_rd(ctl + 4) : 0);
}
#endif

static void lc_fade_put(int a)
{
    *(int32_t *)(uintptr_t)MR_FADE = lc_alpha = a;
}

static void lc_swap(uint32_t player, const char *who, const char *left)
{
    lc_command(LC_PLACE_EXEC, LCS_ARRIVE, who);
    lc_command(LC_FOLLOW_EXEC, who, "Null");
    lc_command(LC_MOVE_EXEC, LCS_LEFT, "Player");
    lc_command(LC_MOVE_END, LCS_LEFT, "Player");
    lc_call1(LC_SWITCH_FN, player);
    lc_controls_free();
    lc_command(LC_FOLLOW_EXEC, left, who);
    lc_command(LC_RELEASE_EXEC, left, NULL);
    lc_cam_char = player + 4;
    lc_cam_until = GetTickCount() + 2000;
    lc_cam_ours = 1;
    lc_camera(lc_cam_char);
#if GMV_DEV
    lc_trace_until = lc_cam_until + 13000;
    lc_trace_kind = -2;
#endif
}

static void lc_left_behind(uint32_t mainp, uint32_t ctrl)
{
    lc_as = 0;
    lc_command(LC_FOLLOW_EXEC, LCS_NAME, "Null");
    lc_command(LC_RELEASE_EXEC, LCS_NAME, NULL);
    if (ctrl == mainp) {
        lc_command(LC_FOLLOW_EXEC, LCS_JOHNNY, "Null");
        *(unsigned char *)(uintptr_t)(mainp + 4 + 5) &= (unsigned char)~8;
    }
    lc_cam_char = ctrl + 4;
    lc_cam_until = GetTickCount() + 2000;
    lc_cam_ours = 0;
    gmv_trace("[lcswitch] the game took control from L.C. (to %s); Johnny's flags %04x\n",
            ctrl == mainp ? "Johnny" : "another player", ms_rd(mainp + 8) & 0xffff);
}

void lc_tick(void)
{
    if (!g_lc_on && !lc_marker && !lc_as && !lc_fade)
        return;
    uint32_t mgr = mr_mgr(), world = ms_rd(ms_rd(MR_WORLD_REF)), mainp = ms_rd(SSF_MAIN_PP), ctrl = ms_rd(SSF_CTRL_PP);
    if (!mgr || !world || !mainp || !ctrl || ms_rd(mainp) != SSF_PLAYER_VT) {
        lc_char = 0;
        lc_marker = lc_moved = lc_as = lc_fade = 0;
        lc_idle_since = lc_cam_until = 0;
        return;
    }
    int idle = (int)ms_rd(mgr + 4) == -1 && !ms_rd(mgr + 0x1c);
    uint32_t lcp = lc_char ? lc_char - 4 : 0;
    DWORD now = GetTickCount();
    if (lc_as && ctrl != lcp)
        lc_left_behind(mainp, ctrl);
#if GMV_DEV
    lc_cam_trace(ctrl + 4, now);
#endif
    if (lc_cam_until) {
        if ((int)(now - lc_cam_until) >= 0) {
            uint32_t ctl = ms_rd(lc_call0(0x000031b0u) + LC_CONTROLS_AT);
            lc_cam_until = 0;
            if (ctl)
                gmv_trace("[lcswitch] 2 s after the switch: controls' state %d, active %d, on-foot handler %d\n",
                        (int)ms_rd(ctl + 0x1c), (int)(ms_rd(ctl + 0x18) & 0xffff), ms_rd(ctl) == ms_rd(ctl + 4));
        } else if (lc_cam_char == ctrl + 4) {
            lc_camera(lc_cam_char);
            if (lc_cam_ours && idle)
                lc_controls_foot(lc_cam_char);
        }
    }
    if (!g_lc_on || !idle) {
        lc_marker_down();
        lc_idle_since = 0;
        if (lc_fade) {
            if (*(const int32_t *)(uintptr_t)MR_FADE == lc_alpha)
                lc_fade_put(MR_FADE_NONE);
            lc_fade = 0;
        }
        if (lc_as && !g_lc_on) {
            lc_swap(mainp, LCS_JOHNNY, LCS_NAME);
            lc_command(LC_FOLLOW_EXEC, LCS_NAME, "Null");
            lc_command(LC_HIDE_EXEC, LCS_NAME, NULL);
            lc_as = 0;
            gmv_trace("[lcswitch] LC_SWITCH turned off while playing as L.C.: back to Johnny at the hotel\n");
        } else if (lc_as)
            lc_switch(mainp, "a mission began while playing as L.C.: back to Johnny");
        return;
    }
    if (lc_fade == 1) {
        int ms = (int)(now - lc_fade_at);
        if (ms < LC_FADE_MS) {
            lc_fade_put(255 * ms / LC_FADE_MS);
            return;
        }
        lc_fade_put(255);
        float p[3];
        if (lc_as)
            lc_swap(mainp, LCS_JOHNNY, LCS_NAME);
        else
            lc_swap(lcp, LCS_NAME, LCS_JOHNNY);
        lc_as = !lc_as;
        lc_pos(lc_as ? lc_char : mainp + 4, p);
        gmv_trace("[lcswitch] playing as %s, at %.0f %.0f %.0f\n", lc_as ? "L.C." : "Johnny", p[0], p[1], p[2]);
        lc_marker_up();
        lc_fade = 2;
        lc_fade_at = now;
        return;
    }
    if (lc_fade == 2) {
        int ms = (int)(now - lc_fade_at);
        if (ms < LC_FADE_MS) {
            lc_fade_put(255 - 255 * ms / LC_FADE_MS);
            return;
        }
        lc_fade_put(MR_FADE_NONE);
        lc_fade = 0;
    }
    if (ctrl != mainp && !lc_as) {
        lc_marker_down();
        return;
    }
#if GMV_DEV
    {
        static int was_shown;
        int shown = pointer_shown();
        if (shown && !was_shown) {
            float p[3];
            lc_pos(ctrl + 4, p);
            gmv_trace("[lcswitch] paused at %.0f %.0f %.0f\n", p[0], p[1], p[2]);
        }
        was_shown = shown;
    }
#endif
    if (pointer_shown() || *(const int32_t *)(uintptr_t)MR_FADE != MR_FADE_NONE)
        return;
    if (!lc_marker) {
        if (!lc_idle_since)
            lc_idle_since = now;
        if (now - lc_idle_since < LC_SETTLE_MS || now - lc_checked < 1000)
            return;
        int done, all;
        uint32_t c, cp;
        lc_checked = now;
        if (!lc_all_done(mgr, &done, &all)) {
            if (lc_said != 1)
                gmv_trace("[lcswitch] %d of %d missions done: L.C.'s marker comes once they all are\n", done, all);
            lc_said = 1;
        } else if (!(c = lc_find(LCS_NAME)) || !(cp = lc_find(LCS_POINT)) || !lc_find(LCS_ARRIVE) || !lc_find(LCS_LEFT) ||
                   lc_find(LCS_JOHNNY) != mainp + 4 || ms_rd(c - 4) != SSF_PLAYER_VT) {
            if (lc_said != 2)
                gmv_log("[lcswitch] %s, %s, %s, %s or %s not in this level; no L.C.\n", LCS_NAME, LCS_JOHNNY, LCS_POINT,
                        LCS_ARRIVE, LCS_LEFT);
            lc_said = 2;
        } else {
            float p[3];
            lc_char = c;
            lc_marker_up();
            lc_said = 0;
            lc_pos(cp, p);
            gmv_trace("[lcswitch] all %d missions done: L.C.'s marker is up at his hotel (%.0f %.0f %.0f), radius %d, shown %d\n",
                    all, p[0], p[1], p[2], (int)ms_rd(cp + 0x4c), (int)(ms_rd(cp + 4) & 1));
        }
        return;
    }
    if (now - lc_checked >= 1000) {
        uint32_t cp = lc_wait_ctx[0x14 / 4];
        lc_checked = now;
        if (lc_find(LCS_POINT) != cp || !(ms_rd(cp + 4) & 1)) {
            lc_marker = lc_moved = 0;
            return;
        }
    }
    if (!(lc_call1(LC_WAIT_TEST, (uint32_t)(uintptr_t)lc_wait) & 0xff))
        return;
    lc_call1(LC_WAIT_DONE, (uint32_t)(uintptr_t)lc_wait);
    lc_marker = 0;
    {
        uint32_t veh = ms_rd(ctrl + 0x55c);
        gmv_trace("[lcswitch] %s is at the marker%s\n", lc_as ? "L.C." : "Johnny",
                !veh ? "" : ms_rd(veh) == PL_MOTO_VT ? ", on a motorbike" : ", in a vehicle");
    }
    lc_command(LC_STOP_EXEC, "Player", NULL);
    lc_fade = 1;
    lc_fade_at = now;
    lc_fade_put(0);
}

void apply_lc_switch(void)
{
    const char *e = gmv_getenv("GMV_LC_SWITCH");
    g_lc_on = e && *e == '1';
    gmv_log("[gamepatch] LC_SWITCH %s (once every mission is done, the marker at L.C.'s hotel switches to him and back)\n",
            g_lc_on ? "on" : "off");
}

#if GMV_DEV
__attribute__((cdecl, force_align_arg_pointer, noinline))
static void uc_survey_exit(uint32_t car)
{
    uc_spot_t s = { { 0 }, 0, 0, 0, 1 };
    uint32_t body = ms_rd(car + 0x250);
    uc_pos(car, s.pos);
    s.cell = ((uint32_t (*)(uint32_t))ms_rd(ms_rd(car) + 0x54))(car) & 0xffff;
    s.tri = ((uint32_t (*)(uint32_t))ms_rd(ms_rd(car) + 0x58))(car) & 0xffff;
    if (body)
        memcpy(&s.ang, (const void *)(uintptr_t)(body + 0x38), 4);
    gmv_log("[crusher] spot: { { %.1ff, %.1ff, %.1ff }, %.5ff, %u, %u, 1 } (type %d); walk 10 m away to put the Crusher there\n",
            s.pos[0], s.pos[1], s.pos[2], s.ang, s.cell, s.tri, (int)(ms_rd(car + 0x24) & 0xff));
    uc.cand = s;
    uc.survey_car = car;
}

/* 0xac ObjectDisable */
__attribute__((cdecl, force_align_arg_pointer, noinline))
static void uc_survey_take(void)
{
    ((void (*)(uint32_t))ms_rd(ms_rd(uc.survey_car) + 0xac))(uc.survey_car);
    uc.survey_car = 0;
    uc.spot = uc.cand;
    if (uc.obj && uc_is_car(uc.obj)) {
        uc_place(uc.obj, &uc.spot, 0);
        gmv_log("[crusher] moved to the new spot\n");
    }
}
#endif

#if GMV_DEV
static const struct { uint32_t va, n; } ma_range[] = {
    { 0x00011000u, 0x0062c000u }, { GMV_LOWCOPY_BASE, 0x00010c44u }, { 0x0064ea40u, 0x00035d78u },
};
#define MA_RANGES (sizeof ma_range / sizeof *ma_range)
static unsigned char *ma_copy[MA_RANGES];

void ma_snapshot(void)
{
    for (size_t r = 0; r < MA_RANGES; r++)
        if ((ma_copy[r] = VirtualAlloc(NULL, ma_range[r].n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) != NULL)
            memcpy(ma_copy[r], (const void *)(uintptr_t)ma_range[r].va, ma_range[r].n);
}

static void ma_report(void)
{
    enum { MAX_LINES = 300 };
    int runs = 0, shown = 0;
    unsigned bytes = 0;
    gmv_log("[macaudit] groups left in: enhancements %d, draw distance %d, collision tables %d, crowd %d, Crusher %d\n",
            mac_undo.n, dd_undo.n, dd_tables_undo.n, pd_undo.n, uc_undo.n);
    for (size_t r = 0; r < MA_RANGES; r++) {
        const unsigned char *now = (const unsigned char *)(uintptr_t)ma_range[r].va, *was = ma_copy[r];
        if (!was)
            continue;
        for (uint32_t i = 0; i < ma_range[r].n; i++) {
            if (now[i] == was[i])
                continue;
            uint32_t at = i, n = 0;
            while (i < ma_range[r].n && (now[i] != was[i] || (i + 1 < ma_range[r].n && now[i + 1] != was[i + 1])))
                i++, n++;
            runs++, bytes += n;
            if (shown++ < MAX_LINES) {
                char a[52] = "", b[52] = "";
                for (uint32_t k = 0; k < n && k < 16; k++) {
                    snprintf(a + k * 3, 4, "%02x ", was[at + k]);
                    snprintf(b + k * 3, 4, "%02x ", now[at + k]);
                }
                gmv_log("[macaudit] %08x+%u: shipped %s now %s\n", ma_range[r].va + at, n, a, b);
            }
        }
    }
    for (int s = 0; s < ma_sites; s++)
        if (memcmp((const void *)(uintptr_t)ma_site[s].va, ma_site[s].was, ma_site[s].n)) {
            runs++, bytes += ma_site[s].n;
            gmv_log("[macaudit] data %08x+%u differs from before the patch\n", ma_site[s].va, ma_site[s].n);
        }
    gmv_log("[macaudit] %d places, %u bytes differ from the shipped game (%s)\n", runs, bytes,
            shown > MAX_LINES ? "the first 300 listed" : "all listed");
}

static void ma_tick(DWORD now)
{
    static int reported;
    static DWORD since;
    if (!gmv_config_mac()) {
        reported = 0, since = 0;
        return;
    }
    if (reported >= 2)
        return;
    if (!since)
        since = now | 1;
    if (reported) {
        if (now - since > 120000) {
            ma_report();
            reported = 2;
        }
        return;
    }
    int waiting = mac_features_on || mac_out_pending || uc_hooks || dd_on || dd_unhook || dd_tables_back || pd_on || sf_hooked;
    if (!waiting) {
        ma_report();
        reported = 1;
        since = now | 1;
    } else if (now - since > 60000) {
        gmv_log("[macaudit] still in after 60 s: enhancements %d, replay wait %d, Crusher %d, draw distance %d (going %d), "
                "tables going %d, crowd %d, fade %d\n", mac_features_on, mac_out_pending, uc_hooks, dd_on, dd_unhook,
                dd_tables_back, pd_on, sf_hooked);
        since = now | 1;
    }
}
#endif

__attribute__((force_align_arg_pointer))
void gmv_uc_tick(void)
{
    int type = gmv_uc_type();
    DWORD now = GetTickCount();
    if (mac_out_pending)
        mac_features_out();
    dd_tables_tick();
    cr_logo_hide_tick();
    rs_tick();
#if GMV_DEV
    ma_tick(now);
#endif
    if (type < 0 || now - uc.t < 100) {
        if (type < 0 && uc_hooks && !gmv_uc_on())
            uc_hooks_out();
        return;
    }
    uc.t = now;
    uint32_t mgr = mr_mgr(), list = ms_rd(MS_SCRIPTS + 0x2dc);
    if (mgr != uc.mgr || list != uc.list) {
        uc.obj = uc.last_veh = uc.survey_car = 0;
        uc.mgr = mgr, uc.list = list;
    }
    uint32_t world = ms_rd(ms_rd(MR_WORLD_REF)), p = ms_rd(MR_PLAYER);
    if (!gmv_uc_on()) {
        if (uc.obj && mgr && world && p)
            uc_leave(world, p, type);
        if (!uc.obj && uc_hooks)
            uc_hooks_out();
        return;
    }
    if (!mgr || !world || !p || (int)ms_rd(world + 0x408) <= type || !ms_rd(world + 0x40c))
        return;
    uint32_t entry = ms_rd(ms_rd(world + 0x40c) + 4 * type);
    if (!entry || ms_rd(entry) != 3)
        return;
    uint32_t veh = ms_rd(p + 4 + 0x558);
    float me[3], at[3];
    if (uc.obj)
        uc_category(type);
    uc_pos(p + 4, me);
#if GMV_DEV
    const char *sv = gmv_dbgenv("GMV_UC_SPOT_SURVEY");
    if (gmv_env_on(sv)) {
        if (uc.last_veh && !veh && uc_is_car(uc.last_veh) && uc.last_veh != uc.obj)
            uc_survey_exit(uc.last_veh);
        else if (veh)
            uc.survey_car = 0;
        if (uc_is_car(uc.survey_car) && (uc_pos(uc.survey_car, at), uc_dist(me, at) > 1000.0f))
            uc_survey_take();
    }
#endif
    uc.last_veh = veh;
    const uc_spot_t *s = uc.spot.have ? &uc.spot : uc_beach.have ? &uc_beach : NULL;
    if (!s)
        return;
    float d_ps = uc_dist(me, s->pos);
    if (uc.obj && !(uc_is_car(uc.obj) && (int)(ms_rd(uc.obj + 0x24) & 0xff) == type)) {
        gmv_trace("[crusher] lost track of it (%08x: vtable %08x, type %u, flags %08x)\n", uc.obj, ms_rd(uc.obj),
                ms_rd(uc.obj + 0x24) & 0xff, ms_rd(uc.obj + 4));
        uc.obj = 0;
    }
    if (uc.obj && veh != uc.obj) {
        uc_pos(uc.obj, at);
        if (uc_dist(me, at) > UC_FAR) {
            if (((char (*)(uint32_t))ms_rd(ms_rd(uc.obj) + 0x88))(uc.obj)) {
                ((void (*)(uint32_t))ms_rd(ms_rd(uc.obj) + 0xac))(uc.obj);
                gmv_trace("[crusher] the wreck switched off; a new one comes later\n");
                uc.obj = 0;
            } else if (uc_dist(at, s->pos) > 500.0f && d_ps > UC_FAR) {
                uc_place(uc.obj, s, 0);
                gmv_trace("[crusher] put back on its spot\n");
            }
        }
    }
    if (uc.obj || d_ps > UC_NEAR || d_ps < 600.0f || (int)ms_rd(mgr + 4) != -1 || !uc_lion_done(mgr))
        return;
    uc.obj = uc_make(world, type, s);
    gmv_trace("[crusher] parked at %.0f %.0f %.0f: %08x\n", s->pos[0], s->pos[1], s->pos[2], uc.obj);
}

#define UC_ATTACH     0x00115b6au
#define UC_DETACH     0x001158f4u
#define UC_UPDATE     0x00112bc4u
#define UC_PS_MGR     0x000a9546u
#define UC_PS_LOAD    0x000a9210u
#define UC_PS_MAKE    0x000a8ab6u
#define UC_PS_FREE    0x000a86a2u
#define UC_PS_ARG     0x00647e00u    /* p_smoke_00 0x115327 */
#define UC_FIND_NODE  0x004c0c30u
#define UC_SPEED      0x3d4          /* +0x3f8 */

/* 0x48 setVisible 0x5c addChild 0x60 removeChild */
typedef struct { uint32_t veh, tmpl, node[2], ps[2]; } uc_smoke_t;
static uc_smoke_t uc_smoke[4];
static uint32_t (*uc_attach_orig)(uint32_t, uint32_t, uint32_t);
static void (*uc_detach_orig)(uint32_t);
static void (*uc_update_orig)(uint32_t, uint32_t);

static uc_smoke_t *uc_smoke_of(uint32_t v)
{
    for (size_t i = 0; i < sizeof uc_smoke / sizeof *uc_smoke; i++)
        if (uc_smoke[i].veh == v)
            return &uc_smoke[i];
    return NULL;
}

static void uc_smoke_on(uint32_t v)
{
    static const char *const names[2] = { "dummy_p_smoke_02_left-node", "dummy_p_smoke_02_right-node" };
    static int said;
    uc_smoke_t *s = uc_smoke_of(v);
    if (s)
        memset(s, 0, sizeof *s);
    if (!gmv_uc_has_smoke() || (int)(ms_rd(v + 0x24) & 0xff) != gmv_uc_type())
        return;
    uint32_t root = ms_rd(v + 0x10);
    if (!root || (!s && !(s = uc_smoke_of(0))))
        return;
    uint32_t mgr = ((uint32_t (*)(void))UC_PS_MGR)();
    uint32_t tmpl = ((uint32_t (*)(uint32_t, const char *, uint32_t, uint32_t))UC_PS_LOAD)(
        mgr, "./p_smoke_02.bdae", ms_rd(UC_PS_ARG), 0);
    s->veh = v, s->tmpl = tmpl;
    for (int k = 0; k < 2; k++) {
        uint32_t node = ((uint32_t (*)(uint32_t, const char *))UC_FIND_NODE)(root, names[k]), ps;
        if (!node || !(ps = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))UC_PS_MAKE)(
                                 mgr, tmpl, 0, 0, 0)))
            continue;
        ((void (*)(uint32_t, int))ms_rd(ms_rd(ps) + 0x48))(ps, 0);
        ((void (*)(uint32_t, uint32_t))ms_rd(ms_rd(node) + 0x5c))(node, ps);
        s->node[k] = node, s->ps[k] = ps;
    }
    if (!said++)
        gmv_trace("[crusher] exhaust smoke on %08x: template %d, stacks %08x %08x\n", v, (int)tmpl, s->ps[0],
                s->ps[1]);
}

static void uc_smoke_off(uc_smoke_t *s)
{
    uint32_t mgr = ((uint32_t (*)(void))UC_PS_MGR)();
    for (int k = 0; k < 2; k++)
        if (s->ps[k]) {
            ((void (*)(uint32_t, uint32_t))ms_rd(ms_rd(s->node[k]) + 0x60))(s->node[k], s->ps[k]);
            ((void (*)(uint32_t, uint32_t, uint32_t))UC_PS_FREE)(mgr, s->tmpl, s->ps[k]);
        }
    memset(s, 0, sizeof *s);
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t uc_attach_hook(uint32_t v, uint32_t a, uint32_t model)
{
    uint32_t r = uc_attach_orig(v, a, model);
    uc_smoke_on(v);
    return r;
}

__attribute__((cdecl, force_align_arg_pointer))
static void uc_detach_hook(uint32_t v)
{
    uc_smoke_t *s = v ? uc_smoke_of(v) : NULL;
    if (s)
        uc_smoke_off(s);
    uc_detach_orig(v);
}

__attribute__((cdecl, force_align_arg_pointer))
static void uc_update_hook(uint32_t v, uint32_t dt)
{
    uc_update_orig(v, dt);
    uc_smoke_t *s = v ? uc_smoke_of(v) : NULL;
    if (!s)
        return;
    int on = (double)*(const float *)(uintptr_t)(v + UC_SPEED) > 0.1;
    for (int k = 0; k < 2; k++)
        if (s->ps[k])
            ((void (*)(uint32_t, int))*(uint32_t *)(uintptr_t)(*(uint32_t *)(uintptr_t)s->ps[k] + 0x48))(s->ps[k], on);
}

static void *hook_entry6(uint32_t fn, void *repl)
{
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x53, 0x57, 0x56 };
    if (memcmp((const void *)(uintptr_t)fn, expect, sizeof expect))
        return NULL;
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr)
        return NULL;
    size_t o = sizeof expect;
    memcpy(tr, expect, sizeof expect);
    emit_jmp(tr, &o, fn + sizeof expect);
    FlushInstructionCache(GetCurrentProcess(), tr, o);
    return patch_jmp(fn, repl, 6) ? tr : NULL;
}

static void apply_uc_smoke(void)
{
    if (!gmv_uc_has_smoke())
        return;
    static const uint32_t fns[3] = { UC_ATTACH, UC_DETACH, UC_UPDATE };
    for (int i = 0; i < 3; i++)
        if (memcmp((const void *)(uintptr_t)fns[i], "\x55\x89\xe5\x53\x57\x56", 6)) {
            gmv_log("[crusher] vehicle function 0x%06x is not as expected; no exhaust smoke\n", fns[i]);
            return;
        }
    uc_attach_orig = (uint32_t (*)(uint32_t, uint32_t, uint32_t))hook_entry6(UC_ATTACH, (void *)uc_attach_hook);
    uc_detach_orig = (void (*)(uint32_t))hook_entry6(UC_DETACH, (void *)uc_detach_hook);
    uc_update_orig = (void (*)(uint32_t, uint32_t))hook_entry6(UC_UPDATE, (void *)uc_update_hook);
    gmv_log("[crusher] exhaust smoke hooked (attach %s, detach %s, update %s)\n", uc_attach_orig ? "ok" : "FAILED",
            uc_detach_orig ? "ok" : "FAILED", uc_update_orig ? "ok" : "FAILED");
}

static void uc_hooks_in_group(void)
{
    apply_uc_smoke();
    static const unsigned char want[] = { 0x83, 0xf8, 0x0c, 0x0f, 0x87, 0xac, 0x00, 0x00, 0x00 };
    if (memcmp((const void *)(uintptr_t)UC_ENG_SITE, want, sizeof want)) {
        gmv_log("[crusher] the engine sound switch at 0x%06x is not as expected; the truck will run silent\n",
                UC_ENG_SITE);
        return;
    }
    static unsigned char *tr;
    if (!tr)
        tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr)
        return;
    size_t o = 0;
    int32_t r;
    tr[o++] = 0x83, tr[o++] = 0xf8, tr[o++] = 0x0d;                  /* cmp eax,13 */
    tr[o++] = 0x0f, tr[o++] = 0x85;
    r = (int32_t)(UC_ENG_DONE - ((uint32_t)(uintptr_t)tr + o + 4));
    memcpy(tr + o, &r, 4), o += 4;
    tr[o++] = 0xff, tr[o++] = 0x75, tr[o++] = 0x08;                  /* push [ebp+8]: */
    tr[o++] = 0xe8;
    r = (int32_t)((uint32_t)(uintptr_t)gmv_uc_engine - ((uint32_t)(uintptr_t)tr + o + 4));
    memcpy(tr + o, &r, 4), o += 4;
    tr[o++] = 0x83, tr[o++] = 0xc4, tr[o++] = 0x04;                  /* add esp,4 */
    emit_jmp(tr, &o, UC_ENG_DONE);
    r = (int32_t)((uint32_t)(uintptr_t)tr - (UC_ENG_SITE + 9));
    patch_bytes(UC_ENG_SITE + 5, (const unsigned char *)&r, 4);
    gmv_log("[crusher] engine sound set 13 hooked\n");
}

void uc_hooks_in(void)
{
    if (!uc_loaded || uc_hooks)
        return;
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &uc_undo;
    uc_hooks_in_group();
    patch_undo_now = keep;
    uc_hooks = 1;
}

static void uc_hooks_out(void)
{
    for (size_t i = 0; i < sizeof uc_smoke / sizeof *uc_smoke; i++)
        if (uc_smoke[i].veh)
            uc_smoke_off(&uc_smoke[i]);
    patch_undo_all(&uc_undo);
    uc_hooks = 0;
    gmv_log("[crusher] its hooks out: the game's own vehicle code again\n");
}

void apply_uc_crusher(void)
{
    if (!gmv_uc_init())
        return;
    uc_loaded = 1;
    if (gmv_uc_on())
        uc_hooks_in();
}

void apply_radio_songs(void)
{
    gmv_rs_init();
}
