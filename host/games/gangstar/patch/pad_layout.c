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

/* 0x1e 0x191a0 0x1d handler+0x1c +0x24 +0x10 +0x14 */
#define PL_BTN_EV_VT  0x0063e178u
#define PL_PLAYER_PP  0x0063e010u
#define PL_LOCK_FLAG  0x6f8         /* 0x0faa3a */
#define PL_TARGET     0x650

static const uint32_t pl_vt[PL_KINDS] = { 0x00656648u, 0x00656aa8u, 0x00656ae8u, 0x00650a88u };
static const uint32_t pl_orig[PL_KINDS] = { 0x0011ca7eu, 0x0012bb8eu, 0x0012e99eu, 0x000a5e96u };

const int pl_map[PL_KINDS][PL_BTNS] = {
    {    4, 0x12, 0x15, 5,   10, 0x11, PL_AIM,    1, 0, 0xb, 0,    0,    0,   9,   0,  10 },
    {   10, PL_AIM, 9, 5,    1, 0x17,   0x28, 0x27, 0, 0xb, 0, 0x14, 0x16, 0xd, 0xc, 0xe },
    {   10,    1,    9, 3, 0x29, 0x2a,   0x28, 0x27, 0, 0xb, 0,    0,    0, 0xd, 0xc, 0xe },
    {   10,    0,  0xf, 9,    9,   10,      0,    1, 0, 0xb, 0,    0,    0,   0,   0,   0 },
};

typedef void (*pl_fn)(void *, int *, void *);
volatile int g_pl_on, g_pl_ok, g_pl_aim_btn = -1;

/* SSE esp 0x19d72 -> 0x599d2f */
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static void pl_orig_call(int kind, void *h, int *ev, void *bus)
{
    ((pl_fn)(uintptr_t)pl_orig[kind])(h, ev, bus);
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static int pl_type(int *ev)
{
    return ((int (*)(int *))(*(uintptr_t **)ev)[2])(ev);
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call1(uint32_t fn, uint32_t a)
{
    return ((uint32_t (*)(uint32_t))(uintptr_t)fn)(a);
}

/* +0x18 */
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static void pl_cam_turn(unsigned char *cam, int x, int y)
{
    ((void (*)(unsigned char *, int, int))(uintptr_t)*(uint32_t *)(uintptr_t)(*(uint32_t *)cam + 0x18))(cam, x, y);
}

static int *pl_node(unsigned char *h, int key)
{
    unsigned char *head = h + 0x20, *n = *(unsigned char **)(h + 0x24), *best = head;
    while (n) {
        if (*(int *)(n + 0x10) < key)
            n = *(unsigned char **)(n + 0xc);
        else {
            best = n;
            n = *(unsigned char **)(n + 8);
        }
    }
    return best != head && *(int *)(best + 0x10) == key ? (int *)(best + 0x14) : NULL;
}

static unsigned char *pl_player(void)
{
    return *(unsigned char **)(uintptr_t)*(const uint32_t *)(uintptr_t)PL_PLAYER_PP;
}

static void pl_run(int kind, unsigned char *h, int *ev, void *bus, int act)
{
    int b = ev[2], *v = pl_node(h, b), keep;
    if (!v) {
        int probe[4] = { ev[0], ev[1], b, 3 };
        pl_orig_call(kind, h, probe, bus);
        if (!(v = pl_node(h, b)))
            return;
    }
    keep = *v;
    *v = act;
    pl_orig_call(kind, h, ev, bus);
    if ((v = pl_node(h, b)) && *v == act)
        *v = keep;
}

static void pl_button(int kind, unsigned char *h, int *ev, void *bus)
{
    int b = ev[2], st = ev[3], act;
    unsigned char *p;
    if (b < 0 || b >= PL_BTNS)
        return;
    act = pl_map[kind][b];
    if (b == g_pl_aim_btn && st == 2 && kind != PL_SNIPER) {
        g_pl_aim_btn = -1;
        if ((p = pl_player()) && p[PL_LOCK_FLAG]) {
            int e2[4] = { ev[0], ev[1], b, 0 };
            void *tgt = *(void **)(p + PL_TARGET);
            pl_run(kind, h, e2, bus, 0x18);
            gmv_trace("[padlayout] lock-on released: flag -> %d (target while held %p)\n", p[PL_LOCK_FLAG], tgt);
        }
    }
    if (act == PL_AIM) {
        if (st == 0) {
            g_pl_aim_btn = b;
            if ((p = pl_player()) && !p[PL_LOCK_FLAG]) {
                pl_run(kind, h, ev, bus, 0x18);
                gmv_trace("[padlayout] lock-on pressed: flag -> %d\n", p[PL_LOCK_FLAG]);
            } else
                gmv_trace("[padlayout] lock-on pressed: %s\n", p ? "flag already on" : "no player");
        }
        return;
    }
    if (act)
        pl_run(kind, h, ev, bus, act);
}

/* 0x1b 0x3ed8 handler+0x38 */
#define PL_GTA_TURN 4.0f
static void pl_stick(int kind, unsigned char *h, int *ev)
{
    static LARGE_INTEGER last[2], freq;
    static float rest[2][2];
    LARGE_INTEGER now;
    int s = ev[2] >= 2;
    float k = kind == PL_SNIPER ? 50.0f : 16.0f, ms, scale;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    ms = last[s].QuadPart ? (float)((double)(now.QuadPart - last[s].QuadPart) * 1000.0 / (double)freq.QuadPart) : 1e9f;
    last[s] = now;
    if (ms > 100.0f) {
        ms = 1000.0f / 30.0f;
        rest[s][0] = rest[s][1] = 0.0f;
    } else if (ms > 66.7f)
        ms = 66.7f;
    scale = ms * 0.03f;
    if (kind == PL_SNIPER && g_pl_on) {
        float fov = *(float *)(h + 0x38);
        if (fov >= 6.0f && fov <= 50.0f)
            scale *= fov / 50.0f;
    } else if (g_pl_on)
        scale *= PL_GTA_TURN;
    for (int a = 0; a < 2; a++) {
        float v = *(float *)&ev[3 + a], want;
        int n;
        if (v == 0.0f) {
            rest[s][a] = 0.0f;
            continue;
        }
        if (kind == PL_SNIPER && g_pl_on)
            v *= fabsf(v);
        want = v * k * scale + rest[s][a];
        n = (int)want;
        rest[s][a] = want - (float)n;
        *(float *)&ev[3 + a] = n ? ((float)n + (n > 0 ? 0.5f : -0.5f)) / k : 0.0f;
    }
}

/* +0x18 0x64f528 +0x34/+0x38 0x64f568 +0x50/+0x54 */
#define PL_CAM_SITE    0x001a2316u
#define PL_CAM_UPDATE  0x00044640u
#define PL_CAM_MGR     0x0004547eu
#define PL_CAM_K       0x005f476cu
int g_pl_cam_hooked;
static volatile float g_rs_x, g_rs_y;
static volatile int g_rs_on;
static LARGE_INTEGER g_rs_t, g_rs_freq;

static void pl_cam_read(int kind, unsigned char *h, int *ev, void *bus)
{
    unsigned char *mgr = (unsigned char *)(uintptr_t)pl_call1(PL_CAM_MGR, 0), keep;
    int e2[5];
    memcpy(e2, ev, sizeof e2);
    e2[3] = e2[4] = 0;
    keep = mgr[0x24];
    mgr[0x24] = 0;
    pl_orig_call(kind, h, e2, bus);
    if (mgr[0x24]) {
        g_rs_x = *(float *)&ev[4];
        g_rs_y = *(float *)&ev[3];
        QueryPerformanceCounter(&g_rs_t);
        g_rs_on = g_rs_x != 0.0f || g_rs_y != 0.0f;
    } else {
        mgr[0x24] = keep;
        g_rs_on = 0;
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pl_cam_update(unsigned char *mgr, int dt)
{
    static const uint32_t vts[3][3] = { { 0x0064f528u, 0x34, 0x38 }, { 0x0064f568u, 0x50, 0x54 },
                                        { 0x00656b28u, 0x78, 0x7c } };
    unsigned char *cam = *(unsigned char **)(mgr + 8);
    if (g_rs_on && (gmv_qpc() - g_rs_t.QuadPart) * 4 > g_rs_freq.QuadPart)
        g_rs_on = 0;
    if (g_rs_on && cam && !gmv_config_mac()) {
        uint32_t vt = *(uint32_t *)cam;
        for (int i = 0; i < 3; i++)
            if (vt == vts[i][0]) {
                float k = *(const float *)(uintptr_t)PL_CAM_K * (g_pl_on ? PL_GTA_TURN : 1.0f) * 256.0f;
                pl_cam_turn(cam, (int)lrintf(g_rs_x * k), (int)lrintf(g_rs_y * k));
                *(float *)(cam + vts[i][1]) *= 1.0f / 256.0f;
                *(float *)(cam + vts[i][2]) *= 1.0f / 256.0f;
                mgr[0x24] = 1;
                break;
            }
    }
    ((void (*)(unsigned char *, int))(uintptr_t)PL_CAM_UPDATE)(mgr, dt);
}

/* 0x12c51c 0x12c628 0x109304 */
#define PL_LEAN_BTN 20
static int g_pl_lean;                                   /* +1 0x12 -1 0x11 */

static void pl_lean_ev(int kind, unsigned char *h, void *bus, int dir, int st)
{
    int e[4] = { (int)(*(const uint32_t *)(uintptr_t)PL_BTN_EV_VT + 8), 0x1e, PL_LEAN_BTN + (dir < 0), st };
    pl_run(kind, h, e, bus, dir > 0 ? 0x12 : 0x11);
}

static void pl_lean(int kind, unsigned char *h, void *bus, float v)
{
    int want = v > 0.5f ? 1 : v < -0.5f ? -1 : 0;
    if (want != g_pl_lean && g_pl_lean) {
        pl_lean_ev(kind, h, bus, g_pl_lean, 2);
        pl_lean_ev(kind, h, bus, g_pl_lean, 3);
    }
    if (want)
        pl_lean_ev(kind, h, bus, want, want != g_pl_lean ? 0 : 1);
    g_pl_lean = want;
}

/* 0x11f42e 0x11dcd6 */
static void pl_walk(int *e2)
{
    float x = *(float *)&e2[4], y = *(float *)&e2[3], s = fabsf(x) + fabsf(y), r, k;
    if (s <= 0.0f)
        return;
    r = sqrtf(x * x + y * y) / 0.97f;
    if (r > 1.0f)
        r = 1.0f;
    k = r / (s * 0.6f);
    *(float *)&e2[4] = x * k;
    *(float *)&e2[3] = y * k;
}

static void pl_event(int kind, unsigned char *h, int *ev, void *bus)
{
    int type;
    if (!ev) {
        pl_orig_call(kind, h, ev, bus);
        return;
    }
    type = pl_type(ev);
    if (type == 0x1b && kind != PL_FLY && kind != PL_SNIPER && ev[2] >= 2 && g_pl_cam_hooked && !gmv_config_mac()) {
        pl_cam_read(kind, h, ev, bus);
        return;
    }
    if (type == 0x1b && kind != PL_FLY && (kind == PL_SNIPER || ev[2] >= 2) && !gmv_config_mac()) {
        int e2[5];
        memcpy(e2, ev, sizeof e2);
        pl_stick(kind, h, e2);
        pl_orig_call(kind, h, e2, bus);
        return;
    }
    if (!g_pl_on) {
        pl_orig_call(kind, h, ev, bus);
        return;
    }
    if (type == 0x1e) {
        pl_button(kind, h, ev, bus);
        return;
    }
    if (type == 0x1d) {
        int d = ev[2];
        if (d >= 1 && d <= 4) {
            int e2[4] = { (int)(*(const uint32_t *)(uintptr_t)PL_BTN_EV_VT + 8), 0x1e, 11 + d, ev[3] };
            pl_button(kind, h, e2, bus);
        }
        return;
    }
    if (kind == PL_CAR && type == 0x1c && ev[2] == 1) {
        /* h+0x34 0x10500a */
        uint32_t veh = *(const uint32_t *)(h + 0x34);
        int moto = veh && *(const uint32_t *)(uintptr_t)veh == PL_MOTO_VT;
        pl_lean(kind, h, bus, moto ? *(float *)&ev[3] : 0.0f);
        return;
    }
    if (kind == PL_FOOT && type == 0x1b && ev[2] == 1) {
        int e2[5];
        memcpy(e2, ev, sizeof e2);
        pl_walk(e2);
        pl_orig_call(kind, h, e2, bus);
        return;
    }
    pl_orig_call(kind, h, ev, bus);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pl_foot(void *h, int *ev, void *bus) { pl_event(PL_FOOT, h, ev, bus); }
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pl_car(void *h, int *ev, void *bus) { pl_event(PL_CAR, h, ev, bus); }
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pl_fly(void *h, int *ev, void *bus) { pl_event(PL_FLY, h, ev, bus); }
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pl_sniper(void *h, int *ev, void *bus) { pl_event(PL_SNIPER, h, ev, bus); }

int pl_set(int on)
{
    if (!g_pl_ok)
        return 0;
    if (on != g_pl_on)
        gmv_log("[gamepatch] PAD_LAYOUT %s\n", on ? "GTA-LIKE" : "MAC");
    g_pl_on = on;
    g_pl_aim_btn = -1;
    g_pl_lean = 0;
    return 1;
}

/* 0x17f854 0x170224 0x1705bc [mgr+0x200] 0x1870a4 */
#define PL_ROWS_SITE   0x0017f98bu   /* call 0x1870a4 */
#define PL_ROWS_FN     0x001870a4u
#define PL_LABEL_SITE  0x0017fa08u   /* call 0x8f426 */
#define PL_REBIND_SITE 0x0017fc0eu   /* call 0x1865ee */
#define PL_REBIND_FN   0x001865eeu
#define PL_COUNT_FN    0x001838e8u
#define PL_ROW_ID      0x7e00
#define PL_ROWS_MAX    16

typedef struct { int name; signed char btn, dpad; } pl_row;
#define PB(id, b) {id, b, 0}
#define PD(id, d) {id, -1, d}
static const pl_row pl_rows[PL_KINDS][PL_ROWS_MAX] = {
    {   PB(0x1193, 7), PB(0x10ed, 6), PB(0x10d3, 1), PB(0x10e1, 2), PB(0x10d7, 3), PB(0x10d9, 0),
        PB(0x10df, 5), PB(0x1195, 4), PD(0x1194, 2), PB(0x1185, 9) },
    {   PB(0x10b1, 7), PB(0x10af, 6), PB(0x10b7, 5), PB(0x1193, 4), PB(0x10ed, 1), PB(0x10b9, 3), PB(0x1194, 2),
        PB(0x1195, 0), PB(0x10c5, 11), PB(0x1185, 9), PD(0x10c7, 2), PD(0x10c9, 4), PD(0x10cf, 3),
        PD(0x10d1, 1) },
    {   PB(0x10f7, 7), PB(0x10f9, 6), PB(0x10fb, 4), PB(0x10fd, 5), PB(0x1193, 1), PB(0x1101, 3),
        PB(0x1194, 2), PB(0x1195, 0), PB(0x1185, 9), PD(0x10c7, 2), PD(0x10c9, 4), PD(0x10cf, 3) },
    {   PB(0x1193, 7), PB(0x1197, 5), PB(0x1198, 4), PB(0x1196, 2), PB(0x1185, 9) },
};
#undef PB
#undef PD
const uint32_t pl_scheme_vt[PL_KINDS] = { 0x00659e50u, 0x00659e68u, 0x00659e38u, 0x00659e20u };
static uint32_t pl_scheme_name[PL_KINDS];

static int pl_row_count(int kind)
{
    int n = 0;
    while (n < PL_ROWS_MAX && pl_rows[kind][n].name)
        n++;
    return n;
}

static int pl_scheme_kind(const void *scheme)
{
    if (!g_pl_on || !scheme)
        return -1;
    for (int k = 0; k < PL_KINDS; k++)
        if (*(const uint32_t *)scheme == pl_scheme_vt[k])
            return k;
    return -1;
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call2(uint32_t fn, uint32_t a, uint32_t b)
{
    return ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)fn)(a, b);
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c)
{
    return ((uint32_t (*)(uint32_t, uint32_t, uint32_t))(uintptr_t)fn)(a, b, c);
}

__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
static uint32_t pl_call4(uint32_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))(uintptr_t)fn)(a, b, c, d);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_pl_padrows(void *scheme, int page, int *rows)
{
    int k = pl_scheme_kind(scheme), n;
    if (k < 0 || !rows) {
        pl_call3(PL_ROWS_FN, (uint32_t)(uintptr_t)scheme, (uint32_t)page, (uint32_t)(uintptr_t)rows);
        return;
    }
    n = pl_row_count(k);
    for (int i = 0; i < 6; i++) {
        int r = page * 6 + i;
        rows[i * 2] = r >= 0 && r < n ? PL_ROW_ID + k * 32 + r : -1;
        rows[i * 2 + 1] = r >= 0 && r < n ? pl_rows[k][r].btn : -1;
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_pl_padcount(const unsigned char *scheme)
{
    int k = pl_scheme_kind(scheme);
    if (k >= 0)
        return pl_row_count(k);
    return (int)((*(const uint32_t *)(scheme + 0x50) - *(const uint32_t *)(scheme + 0x4c)) >> 2);
}

static uint32_t pl_scheme_name_of(const void *scheme, int action)
{
    for (int k = 0; k < PL_KINDS; k++)
        if (*(const uint32_t *)scheme == pl_scheme_vt[k])
            return pl_call2(pl_scheme_name[k], (uint32_t)(uintptr_t)scheme, (uint32_t)action);
    return 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_pl_padname(void *scheme, int action)
{
    if (action >= PL_ROW_ID && action < PL_ROW_ID + PL_KINDS * 32)
        return (uint32_t)action;
    return pl_scheme_name_of(scheme, action);
}

__attribute__((cdecl, force_align_arg_pointer))
static const char *gmv_pl_padlabel(void *table, int id)
{
    static char buf[PL_KINDS][PL_ROWS_MAX][96];
    static const char *const dir[5] = { "", "UP", "RIGHT", "DOWN", "LEFT" };
    int k = (id - PL_ROW_ID) / 32, r = (id - PL_ROW_ID) % 32;
    const char *s;
    if (id < PL_ROW_ID || k >= PL_KINDS || r >= PL_ROWS_MAX || !pl_rows[k][r].name)
        return (const char *)(uintptr_t)pl_call2(PL_LABEL_FN, (uint32_t)(uintptr_t)table, (uint32_t)id);
    s = (const char *)(uintptr_t)pl_call2(PL_LABEL_FN, (uint32_t)(uintptr_t)table, (uint32_t)pl_rows[k][r].name);
    if (!pl_rows[k][r].dpad)
        return s;
    snprintf(buf[k][r], sizeof buf[k][r], "%s (D-PAD %s)", s ? s : "", dir[(int)pl_rows[k][r].dpad]);
    return buf[k][r];
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_pl_padrebind(void *scheme, void *ev, void *rows, int row)
{
    if (pl_scheme_kind(scheme) >= 0)
        return 1;
    return (unsigned char)pl_call4(PL_REBIND_FN, (uint32_t)(uintptr_t)scheme, (uint32_t)(uintptr_t)ev,
                                   (uint32_t)(uintptr_t)rows, (uint32_t)row);
}

static void pl_controls_page_install(void)
{
    static const unsigned char count_body[] = { 0x55, 0x89, 0xe5, 0x8b, 0x4d, 0x08, 0x8b, 0x41, 0x50,
                                                0x2b, 0x41, 0x4c, 0xc1, 0xf8, 0x02, 0x5d, 0xc3 };
    static const uint32_t names[PL_KINDS] = { 0x00183620u, 0x001834d0u, 0x0018377cu, 0x001838a8u };
    int ok = sm_is_call(PL_ROWS_SITE, PL_ROWS_FN) && sm_is_call(PL_LABEL_SITE, PL_LABEL_FN) &&
             sm_is_call(PL_REBIND_SITE, PL_REBIND_FN) &&
             !memcmp((const void *)(uintptr_t)PL_COUNT_FN, count_body, sizeof count_body);
    for (int k = 0; ok && k < PL_KINDS; k++)
        ok = *(const uint32_t *)(uintptr_t)(pl_scheme_vt[k] + 0xc) == names[k];
    if (!ok) {
        gmv_log("[gamepatch] the gamepad controls page is not the expected one; it keeps the Mac buttons\n");
        return;
    }
    for (int k = 0; k < PL_KINDS; k++) {
        uint32_t f = (uint32_t)(uintptr_t)gmv_pl_padname;
        pl_scheme_name[k] = names[k];
        ok &= patch_bytes(pl_scheme_vt[k] + 0xc, (const unsigned char *)&f, 4);
    }
    {   /* __cdecl */
        ok &= patch_jmp(PL_COUNT_FN, (void *)gmv_pl_padcount, 5);
    }
    ok &= sm_redirect(PL_ROWS_SITE, (void *)gmv_pl_padrows) && sm_redirect(PL_LABEL_SITE, (void *)gmv_pl_padlabel) &&
          sm_redirect(PL_REBIND_SITE, (void *)gmv_pl_padrebind);
    gmv_log("[gamepatch] gamepad controls page %s\n",
            ok ? "shows the GTA-LIKE buttons while that layout is on" : "partly hooked (a write failed)");
}

void apply_pad_layout(void)
{
    static const pl_fn mine[PL_KINDS] = { gmv_pl_foot, gmv_pl_car, gmv_pl_fly, gmv_pl_sniper };
    const char *e = gmv_getenv("GMV_PAD_LAYOUT");
    int ok = 1;
    if (gmv_config_mac())
        return;
    for (int k = 0; k < PL_KINDS; k++)
        ok &= ((const uint32_t *)(uintptr_t)pl_vt[k])[2] == pl_orig[k];
    if (!ok) {
        gmv_log("[gamepatch] the input handlers are not the expected ones; no PAD_LAYOUT\n");
        return;
    }
    for (int k = 0; ok && k < PL_KINDS; k++) {
        uint32_t f = (uint32_t)(uintptr_t)mine[k];
        ok = patch_bytes(pl_vt[k] + 2 * 4, (const unsigned char *)&f, 4);
    }
    if (!ok) {
        gmv_log("[gamepatch] a write failed (%lu); PAD_LAYOUT partly in\n", GetLastError());
        return;
    }
    g_pl_ok = 1;
    g_pl_on = gmv_env_on(e);
    gmv_log("[gamepatch] PAD_LAYOUT %s (MAC = the game's pad buttons, GTA-LIKE = the GTA V-like layout)\n",
            g_pl_on ? "GTA-LIKE" : "MAC");
    pl_controls_page_install();
    QueryPerformanceFrequency(&g_rs_freq);
    if (sm_is_call(PL_CAM_SITE, PL_CAM_UPDATE) && sm_redirect(PL_CAM_SITE, (void *)gmv_pl_cam_update))
        g_pl_cam_hooked = 1;
    gmv_log("[gamepatch] pad camera %s\n", g_pl_cam_hooked ? "turns by the clock (any frame rate)"
                                                           : "update call not the expected one; turns per read");
}
