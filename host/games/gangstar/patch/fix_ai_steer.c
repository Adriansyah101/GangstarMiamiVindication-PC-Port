#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xc8f00 0xc9a1a 0x6ea88 0x108780 */
#define AST_SITE     0x0006ecc1u   /* mov ecx,[ecx+0x11c] */
#define AST_STEER    0x11c
#define AST_POS      0x5c          /* 0x23d16 */
#define AST_FWD      0x68          /* 0x23da8 */
#define AST_VEH      0x558
#define AST_TARGET   0x514
#define AST_NEAR     0.1f          /* 0x5f1ba0 */
#define AST_RIGHT    1.57079633f   /* 0x5f1328 */

typedef void (*ast_fn)(uint32_t, int, float);
static const unsigned char ast_orig[6] = { 0x8b, 0x89, 0x1c, 0x01, 0x00, 0x00 };
static int ast_on;

static uint32_t ast_rd(uint32_t a)
{
    return *(const uint32_t *)(uintptr_t)a;
}

#if GMV_DEV
#define AST_SLOTS    32
#define AST_BODY     0x250         /* +0x40 +0x48 */
#define AST_STATE    0x49c
#define AST_FLAGS    0x584
typedef struct {
    int n, flips[2], fast[2], wflips;
    double travel[2], su[2], sw2, sv, sd, sz;
} ast_win;
typedef struct {
    uint32_t veh, ch;
    int dir[2], wsign, seen;
    float s[2];
    double flip_t[2];
    ast_win w;
} ast_stat;
static ast_stat ast_st[AST_SLOTS];
static int ast_probe;
static double ast_qpf, ast_next;

static double ast_now(void)
{
    return (double)gmv_qpc() / ast_qpf;
}

static void ast_report(void)
{
    for (int i = 0; i < AST_SLOTS; i++) {
        ast_stat *a = &ast_st[i];
        const ast_win *w = &a->w;
        if (a->veh && !w->n)
            a->veh = 0;
        if (!a->veh)
            continue;
        double n = w->n;
        gmv_log("[steer] car %08x type %d (driver %08x, state %d, flags %02x): %d calls, flips %d/%d (under 100 ms %d/%d), "
                "travel %.2f/%.2f, u %.2f/%.2f, yaw rms %.3f, yaw flips %d, speed %.0f, target %.0f away, %.0f up\n",
                a->veh, (int)(ast_rd(a->veh + 0x24) & 0xff), a->ch, (int)ast_rd(a->ch + AST_STATE),
                (unsigned)(ast_rd(a->ch + AST_FLAGS) & 0xff), w->n,
                w->flips[0], w->flips[1], w->fast[0], w->fast[1], w->travel[0], w->travel[1], w->su[0] / n,
                w->su[1] / n, sqrt(w->sw2 / n), w->wflips, w->sv / n, w->sd / n, w->sz / n);
        memset(&a->w, 0, sizeof a->w);
    }
}

static void ast_note(uint32_t veh, uint32_t ch, const int *dir, const float *u, float dist, float up)
{
    double now = ast_now();
    if (now >= ast_next) {
        if (ast_next)
            ast_report();
        ast_next = now + 1000.0;
    }
    int k = -1, fresh = -1;
    for (int i = 0; i < AST_SLOTS && k < 0; i++)
        if (ast_st[i].veh == veh)
            k = i;
        else if (!ast_st[i].veh && fresh < 0)
            fresh = i;
    if (k < 0) {
        if (fresh < 0)
            return;
        k = fresh;
        memset(&ast_st[k], 0, sizeof ast_st[k]);
        ast_st[k].veh = veh;
        ast_st[k].dir[0] = ast_st[k].dir[1] = -1;
    }
    ast_stat *a = &ast_st[k];
    ast_win *w = &a->w;
    int first = !a->seen;
    a->seen = 1;
    w->n++;
    a->ch = ch;
    for (int j = 0; j < 2; j++) {
        float s = dir[j] ? u[j] : -u[j];
        if (u[j] > 0.0f) {
            if (a->dir[j] >= 0 && dir[j] != a->dir[j]) {
                w->flips[j]++;
                w->fast[j] += now - a->flip_t[j] < 100.0;
                a->flip_t[j] = now;
            }
            a->dir[j] = dir[j];
        }
        if (!first)
            w->travel[j] += fabsf(s - a->s[j]);
        a->s[j] = s;
        w->su[j] += u[j];
    }
    uint32_t body = ast_rd(veh + AST_BODY);
    if (body) {
        const float *b = (const float *)(uintptr_t)body;
        float yaw = b[0x48 / 4];
        int sg = yaw > 0.05f ? 1 : yaw < -0.05f ? -1 : 0;
        if (sg && a->wsign && sg != a->wsign)
            w->wflips++;
        if (sg)
            a->wsign = sg;
        w->sw2 += (double)yaw * yaw;
        w->sv += sqrt((double)b[0x40 / 4] * b[0x40 / 4] + (double)b[0x44 / 4] * b[0x44 / 4]);
    }
    w->sd += dist;
    w->sz += fabsf(up);
}
#define ast_probe_on() ast_probe
#else
#define ast_probe_on() 0
#endif

__attribute__((cdecl, force_align_arg_pointer))
void gmv_ai_steer(uint32_t veh, int dir, float u, uint32_t ch)
{
    ast_fn steer = (ast_fn)(uintptr_t)ast_rd(ast_rd(veh) + AST_STEER);
    if (ast_rd(ch + AST_VEH) != veh) {
        steer(veh, dir, u);
        return;
    }
    const float *t = (const float *)(uintptr_t)(ch + AST_TARGET);
    float p[3], f[3];
    obj_vec(ast_rd(ast_rd(veh) + AST_POS), veh, p);
    obj_vec(ast_rd(ast_rd(veh) + AST_FWD), veh, f);
    float dx = t[0] - p[0], dy = t[1] - p[1], cr = dx * f[1] - dy * f[0], dt = dx * f[0] + dy * f[1];
    int d2 = cr >= 0.0f;
    float u2 = dx * dx + dy * dy < AST_NEAR ? 0.0f : fminf(fabsf(atan2f(cr, dt)) / AST_RIGHT, 1.0f);
#if GMV_DEV
    if (ast_probe) {
        int dirs[2] = { dir != 0, d2 };
        float us[2] = { u, u2 };
        ast_note(veh, ch, dirs, us, sqrtf(dx * dx + dy * dy), t[2] - p[2]);
    }
#endif
    if (ast_on)
        steer(veh, d2, u2);
    else
        steer(veh, dir, u);
}

/* [esp+4] [esp+8] [esp+0xc] edi */
__attribute__((naked)) static void ast_entry(void)
{
    __asm__("movl %esp, %eax\n\tpushl %edi\n\tpushl 12(%eax)\n\tpushl 8(%eax)\n\tpushl 4(%eax)\n\t"
            "call " GMV_ASM_SYM(gmv_ai_steer) "\n\taddl $16, %esp\n\tret");
}

int ai_steer_set(int on)
{
    unsigned char site[6] = { 0xb9 };      /* mov ecx,imm32; nop */
    uint32_t to = (uint32_t)(uintptr_t)ast_entry;
    memcpy(site + 1, &to, 4);
    site[5] = 0x90;
    const void *p = (const void *)(uintptr_t)AST_SITE;
    if (memcmp(p, ast_orig, sizeof ast_orig) && memcmp(p, site, sizeof site))
        return -1;
    ast_on = on;
    return patch_bytes(AST_SITE, on || ast_probe_on() ? site : ast_orig, sizeof site) ? on : -2;
}

void apply_ai_steer(void)
{
    int on = gmv_env_on(gmv_getenv("GMV_AI_STEERING_FIX"));
#if GMV_DEV
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    ast_qpf = (double)f.QuadPart / 1000.0;
    ast_probe = gmv_env_on(gmv_dbgenv("GMV_STEER_PROBE"));
    if (ast_probe)
        gmv_log("[steer] probe on, pairs are the game's 3D steer / the ground-plane steer; AI_STEERING_FIX=%d\n", on);
#endif
    if (!on && !ast_probe_on())
        return;
    int r = ai_steer_set(on);
    if (r == -1)
        gmv_log("[gamepatch] AI steering call 0x%08x not the expected bytes; AI_STEERING_FIX skipped\n", AST_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); AI_STEERING_FIX skipped\n", AST_SITE, GetLastError());
    else if (on)
        gmv_log("[gamepatch] AI drivers steer by the heading on the ground at 0x%08x (AI_STEERING_FIX=0 for the "
                "Mac's)\n", AST_SITE);
}
