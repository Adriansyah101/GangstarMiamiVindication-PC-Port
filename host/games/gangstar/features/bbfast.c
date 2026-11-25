#include "compat.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"

#ifndef __clang__
#pragma GCC target("sse2,fpmath=sse")
#endif

#define BB_FN       0x00131f1eu
#define BB_FN_END   0x001325d0u
#define BB_FN_FNV   0xa43f962au
#define BB_CALLER   0x001315dau   /* 0x1315bc */
#define BB_SITE_MAP 0x0013223au   /* call 0x517b10 */
#define BB_SITE_SIN 0x001323ebu   /* call 0x5eeeae */
#define BB_MAP      0x00517b10u
#define BB_ACOSF    0x005ee986u
#define BB_SINF     0x005eeeaeu
#define BB_COSF     0x005ee9dau
#define BB_APP      0x006853a0u
#define BB_GETAPP   0x000031b0u
static const uint32_t bb_getapp_sites[4] = { 0x131f30u, 0x131f76u, 0x131fa8u, 0x13203du };

#define AT(p, o, T) (*(T *)((uint8_t *)(p) + (o)))
#define U16(p, o) AT(p, o, uint16_t)
#define I32(p, o) AT(p, o, int32_t)
#define U32(p, o) AT(p, o, uint32_t)
#define PTR(p, o) AT(p, o, uint8_t *)
#define VT(o, off) (*(void **)(*(uint8_t **)(o) + (off)))

typedef const float *(*bb_vecfn)(void *);
typedef float (*bb_ffn)(void *);
typedef uint8_t *(*bb_mapfn)(void *, int);
typedef void (*bb_vfn)(void *);
typedef float (*bb_mathfn)(float);

static void (*g_orig)(void *);
static bb_mapfn g_fwd_map;
static bb_mathfn g_fwd_sin;
static int g_on, g_verify;

static struct {
    unsigned calls, fallback, checked, bad, rewritten;
    unsigned long long elems, passed, turned, verts;
} g_st;
static unsigned g_checked_all, g_bad_all, g_logged;

typedef struct { float *p; uint32_t ox, oy, nx, ny; } bb_write;
typedef struct { void **a; int n, cap; } bb_plist;
typedef struct { uint32_t *a; int n, cap; } bb_ulist;
static bb_write *g_w;
static int g_nw, g_wcap, g_wfull;
static uint32_t *g_seen;
static int g_seencap;
static bb_plist g_maps_mine, g_maps_orig, g_data_mine, g_data_orig;
static bb_ulist g_ang_mine, g_ang_orig;
static int g_rec_orig;

static int bb_grow(void **a, int *cap, int need, size_t sz)
{
    if (need <= *cap)
        return 1;
    int c = *cap ? *cap * 2 : 1024;
    while (c < need)
        c *= 2;
    void *n = realloc(*a, (size_t)c * sz);
    if (!n)
        return 0;
    *a = n;
    *cap = c;
    return 1;
}

static void bb_padd(bb_plist *l, void *p)
{
    if (bb_grow((void **)&l->a, &l->cap, l->n + 1, sizeof *l->a))
        l->a[l->n] = p;
    l->n++;
}

static void bb_uadd(bb_ulist *l, uint32_t v)
{
    if (bb_grow((void **)&l->a, &l->cap, l->n + 1, sizeof *l->a))
        l->a[l->n] = v;
    l->n++;
}

static uint32_t bb_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float bb_sqrt(float x)
{
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline float bb_max(float a, float b)
{
    __asm__("maxss %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static inline float bb_min(float a, float b)
{
    __asm__("minss %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static uint8_t *bb_cam(void)
{
    uint8_t *s = *(uint8_t **)(uintptr_t)BB_APP;
    return PTR(PTR(PTR(s, 0x17aa4), 0x1c), 0xe4);
}

static void bb_release(uint8_t *o)
{
    if (--I32(o, 4) == 0) {
        ((bb_vfn)VT(o, 8))(o);
        ((bb_vfn)VT(o, 4))(o);
    }
}

static void bb_element(uint8_t *m, uint8_t *e, const float *b, float cx, float cy, float dx, float dy,
                       float dz, float dz0, int rec)
{
    float cz = (b[2] + b[5]) * 0.5f;
    uint32_t lim = U16(e, 0x26), first = U16(e, 0x24);

    uint32_t idx = U32(e, 4);
    uint8_t *tbl = PTR(m, 8), *ent = PTR(m, 0x14), *arr = PTR(m, 0x20);
    uint32_t k = U16(arr, U32(ent, idx * 8) * 0x14 + 0xc) + U32(ent, idx * 8 + 4);
    k *= U32(m, 0x70);
    uint8_t *q = PTR(tbl, k);
    uint32_t j = U32(tbl, k + 4);
    q = *(uint8_t **)q;
    uint8_t *mesh = PTR(arr, U32(q, j * 8) * 0x14);
    if (mesh)
        I32(mesh, 4)++;

    uint8_t *mb = PTR(mesh, 8);
    void *vb = PTR(mb, 0x14);
    if (rec)
        bb_padd(&g_maps_mine, vb);
    uint8_t *data = ((bb_mapfn)(uintptr_t)BB_MAP)(vb, 5);
    if (rec)
        bb_padd(&g_data_mine, data);
    uint32_t stride = U16(mb, 0x22), off0 = U32(mb, 0x18);
    const float *v0 = (const float *)(data + (stride * first + off0));
    const float *v1 = (const float *)(data + (stride * (first + 1) + off0));
    const float *v2 = (const float *)(data + (stride * (first + 2) + off0));

    float a = v0[2] - v1[2];
    float bz = v2[2] - v1[2];
    float n1 = (v0[0] - v1[0]) * bz - (v2[0] - v1[0]) * a;
    float n0 = (v2[1] - v1[1]) * a - (v0[1] - v1[1]) * bz;
    float l = n0 * n0 + n1 * n1;
    l = l + 0.0f;
    float nx = -n0, ny = -n1, nz;
    if (l != 0.0f) {
        float inv = 1.0f / bb_sqrt(l);
        nz = inv * 0.0f;
        ny = ny * inv;
        nx = nx * inv;
    } else
        nz = 0.0f;
    float dot = nz * dz + (dx * nx + dy * ny);
    float ang = ((bb_mathfn)(uintptr_t)BB_ACOSF)(bb_min(bb_max(-1.0f, dot), 1.0f));

    if (!(0.01f > __builtin_fabsf(ang))) {
        float cr = (nx * dy - ny * dx) + dz0;
        if (0.0f > cr)
            ang = -ang;
        if (rec)
            bb_uadd(&g_ang_mine, bb_bits(ang));
        float s = ((bb_mathfn)(uintptr_t)BB_SINF)(ang);
        float c = ((bb_mathfn)(uintptr_t)BB_COSF)(ang);
        g_st.turned++;
        if (lim > first) {
            float kz = cz * -0.0f;
            float ty = ((cy - cx * s) - cy * c) + kz;
            float tx = ((cx - cx * c) + cy * s) + kz;
            float ms = -s;
            uint32_t end = (first + 1 > lim ? first + 1 : lim) - 1;
            for (uint32_t v = first;; v++) {
                float *w = (float *)(data + ((uint32_t)U16(mb, 0x22) * v + off0));
                float x = w[0], y = w[1];
                float nxv = (x * c + y * ms) + tx;
                float nyv = (x * s + y * c) + ty;
                if (rec) {
                    if (bb_grow((void **)&g_w, &g_wcap, g_nw + 1, sizeof *g_w)) {
                        bb_write *r = &g_w[g_nw++];
                        r->p = w;
                        memcpy(&r->ox, &w[0], 4);
                        memcpy(&r->oy, &w[1], 4);
                        r->nx = bb_bits(nxv);
                        r->ny = bb_bits(nyv);
                    } else
                        g_wfull = 1;
                }
                w[0] = nxv;
                w[1] = nyv;
                g_st.verts++;
                if (v == end)
                    break;
            }
        }
    }

    if ((uint32_t)(uintptr_t)data + off0) {
        uint8_t *bf = PTR(mb, 0x14);
        uint8_t st = bf[0x13], n = st & 0x1f;
        if (n >= 2)
            bf[0x13] = (uint8_t)((st & 0xe0) | (n - 1));
        else {
            if (bf[0x12] & 0x20)
                ((bb_vfn)VT(bf, 0x18))(bf);
            bf[0x13] = 0;
        }
    }
    if (mesh)
        bb_release(mesh);
}

/* node+0x1c4 */
static void bb_turn(uint8_t *n, int rec)
{
    if (!bb_cam())
        return;
    if (U32(n, 0x1c8) + 0xb - U32(n, 0x1c4) < 0x17)
        return;
    uint8_t *m = PTR(n, 0x118);
    if (m)
        I32(m, 4)++;

    uint8_t *cam = bb_cam();
    const float *p = ((bb_vecfn)VT(cam, 0xa0))(cam);
    float px = p[0], py = p[1];
    cam = bb_cam();
    const float *t = ((bb_vecfn)VT(cam, 0x108))(cam);
    float dy = t[1] - py, dx = t[0] - px;
    float l = dx * dx + dy * dy;
    l = l + 0.0f;
    float dz = 0.0f;
    if (l != 0.0f) {
        float inv = 1.0f / bb_sqrt(l);
        dx = dx * inv;
        dy = dy * inv;
        dz = 0.0f * inv;
    }
    cam = bb_cam();
    float f = ((bb_ffn)VT(cam, 0x120))(cam);

    g_st.calls++;
    if (U32(n, 0x1c8) - U32(n, 0x1c4) + 0xb > 0x16) {
        float r2 = (1.5f * f) * f;
        float dz0 = dz * 0.0f;
        uint32_t count = 0, off = 4;
        for (;;) {
            uint32_t base = U32(n, 0x1c4);
            uint8_t **gb = *(uint8_t ***)(uintptr_t)(base + off - 4);
            uint8_t **ge = *(uint8_t ***)(uintptr_t)(base + off);
            count++;
            if ((uint32_t)((uint8_t *)ge - (uint8_t *)gb) > 3) {
                uint32_t i = 0;
                do {
                    uint8_t *e = gb[i];
                    i++;
                    g_st.elems++;
                    const float *b = *(const float **)(e + 0xc);
                    float cy = (b[1] + b[4]) * 0.5f;
                    float ey = cy - py;
                    float cx = (b[0] + b[3]) * 0.5f;
                    float ex = cx - px;
                    float d2 = ex * ex + ey * ey;
                    if (!(d2 >= r2) && !(0.0f > ex * dx + ey * dy)) {
                        g_st.passed++;
                        bb_element(m, e, b, cx, cy, dx, dy, dz, dz0, rec);
                    }
                    base = U32(n, 0x1c4);
                    gb = *(uint8_t ***)(uintptr_t)(base + off - 4);
                    ge = *(uint8_t ***)(uintptr_t)(base + off);
                } while ((uint32_t)((int32_t)((uint8_t *)ge - (uint8_t *)gb) >> 2) > i);
            }
            int32_t ng = (int32_t)(U32(n, 0x1c8) - U32(n, 0x1c4)) / 12;
            off += 12;
            if (!((uint32_t)ng > count))
                break;
        }
    }
    if (m)
        bb_release(m);
}

__attribute__((cdecl, force_align_arg_pointer))
static uint8_t *bbv_map(void *vb, int access)
{
    if (!g_rec_orig)
        return g_fwd_map(vb, access);
    bb_padd(&g_maps_orig, vb);
    uint8_t *d = g_fwd_map(vb, access);
    bb_padd(&g_data_orig, d);
    return d;
}

__attribute__((cdecl, force_align_arg_pointer))
static float bbv_sin(float a)
{
    if (g_rec_orig)
        bb_uadd(&g_ang_orig, bb_bits(a));
    return g_fwd_sin(a);
}

static int g_call_bad, g_call_lines;

static void bb_mismatch(const char *what, int i, uint32_t mine, uint32_t orig, uint32_t old)
{
    g_call_bad = 1;
    if (g_call_lines < 8 && g_logged < 80) {
        g_call_lines++;
        g_logged++;
        gmv_log("[bbfast] MISMATCH in call %u: %s [%d] mine 0x%08x original 0x%08x (before 0x%08x)\n",
                g_checked_all, what, i, mine, orig, old);
    }
}

static int bb_seen(const float *p)
{
    uint32_t v = (uint32_t)(uintptr_t)p, h = (v >> 2) * 0x9e3779b1u;
    for (uint32_t m = (uint32_t)g_seencap - 1, k = h & m;; k = (k + 1) & m) {
        if (g_seen[k] == v)
            return 1;
        if (!g_seen[k]) {
            g_seen[k] = v;
            return 0;
        }
    }
}

static void bb_check(uint8_t *n)
{
    g_nw = 0;
    g_wfull = 0;
    g_maps_mine.n = g_maps_orig.n = g_data_mine.n = g_data_orig.n = 0;
    g_ang_mine.n = g_ang_orig.n = 0;
    bb_turn(n, 1);
    for (int i = g_nw - 1; i >= 0; i--) {
        memcpy(&g_w[i].p[0], &g_w[i].ox, 4);
        memcpy(&g_w[i].p[1], &g_w[i].oy, 4);
    }
    g_rec_orig = 1;
    g_orig(n);
    g_rec_orig = 0;
    g_st.checked++;
    g_checked_all++;
    g_call_bad = g_call_lines = 0;

    int need = 64;
    while (need < g_nw * 2)
        need *= 2;
    if (g_wfull || g_maps_mine.n > g_maps_mine.cap || g_maps_orig.n > g_maps_orig.cap ||
        g_data_mine.n > g_data_mine.cap || g_data_orig.n > g_data_orig.cap ||
        g_ang_mine.n > g_ang_mine.cap || g_ang_orig.n > g_ang_orig.cap ||
        !bb_grow((void **)&g_seen, &g_seencap, need, sizeof *g_seen)) {
        bb_mismatch("a record could not grow; not compared", 0, 0, 0, 0);
        goto done;
    }
    if (g_maps_mine.n != g_maps_orig.n)
        bb_mismatch("map count", 0, (uint32_t)g_maps_mine.n, (uint32_t)g_maps_orig.n, 0);
    else
        for (int i = 0; i < g_maps_mine.n; i++) {
            if (g_maps_mine.a[i] != g_maps_orig.a[i])
                bb_mismatch("map buffer", i, (uint32_t)(uintptr_t)g_maps_mine.a[i],
                            (uint32_t)(uintptr_t)g_maps_orig.a[i], 0);
            if (g_data_mine.a[i] != g_data_orig.a[i])
                bb_mismatch("map data", i, (uint32_t)(uintptr_t)g_data_mine.a[i],
                            (uint32_t)(uintptr_t)g_data_orig.a[i], 0);
        }
    if (g_ang_mine.n != g_ang_orig.n)
        bb_mismatch("turn count", 0, (uint32_t)g_ang_mine.n, (uint32_t)g_ang_orig.n, 0);
    else
        for (int i = 0; i < g_ang_mine.n; i++)
            if (g_ang_mine.a[i] != g_ang_orig.a[i])
                bb_mismatch("angle", i, g_ang_mine.a[i], g_ang_orig.a[i], 0);
    memset(g_seen, 0, (size_t)g_seencap * sizeof *g_seen);
    for (int i = g_nw - 1; i >= 0; i--) {
        if (bb_seen(g_w[i].p)) {
            g_st.rewritten++;
            continue;
        }
        uint32_t x, y;
        memcpy(&x, &g_w[i].p[0], 4);
        memcpy(&y, &g_w[i].p[1], 4);
        if (x != g_w[i].nx)
            bb_mismatch("vertex x", i, g_w[i].nx, x, g_w[i].ox);
        if (y != g_w[i].ny)
            bb_mismatch("vertex y", i, g_w[i].ny, y, g_w[i].oy);
    }
done:
    if (g_call_bad) {
        g_st.bad++;
        g_bad_all++;
    }
}

/* 0x131f1e 0x1315bc 0x641111 */
__attribute__((cdecl, force_align_arg_pointer))
static void bb_entry(uint8_t *n)
{
    if (!PTR(n, 0x118)) {
        g_st.fallback++;
        g_orig(n);
        return;
    }
    if (g_verify)
        bb_check(n);
    else
        bb_turn(n, 0);
}

#if GMV_DEV
void gmv_bbfast_report(double seconds)
{
    if (!g_on || !g_st.calls)
        return;
    double c = g_st.calls;
    gmv_logfile("[perf]   billboard turn (bbfast): %.0f calls a second, %.0f elements a call, %.1f in range "
                "and in front, %.1f turned (%.1f vertices); original %u\n", c / seconds,
                (double)g_st.elems / c, (double)g_st.passed / c, (double)g_st.turned / c,
                (double)g_st.verts / c, g_st.fallback);
    if (g_verify)
        gmv_logfile("[perf]     checked against the original: %u calls, %s%u differed (since the start %u of %u); "
                    "%.1f vertex writes a call overwritten later in the same call\n", g_st.checked,
                    g_st.bad ? "" : "all identical, ", g_st.bad, g_bad_all, g_checked_all,
                    g_st.checked ? (double)g_st.rewritten / g_st.checked : 0.0);
    memset(&g_st, 0, sizeof g_st);
}
#endif

void gmv_apply_bbfast(void)
{
    if (gmv_env_off(gmv_dbgenv("GMV_FAST_BILLBOARD"))) {
        gmv_log("[bbfast] FAST_BILLBOARD=0: the original billboard turn kept\n");
        return;
    }
    uint32_t h = 0x811c9dc5u;
    for (uint32_t a = BB_FN; a < BB_FN_END; a++) {
        if ((a > BB_SITE_MAP && a <= BB_SITE_MAP + 4) || (a > BB_SITE_SIN && a <= BB_SITE_SIN + 4))
            continue;
        int site = 0;
        for (int i = 0; i < 4; i++)
            site |= a > bb_getapp_sites[i] && a <= bb_getapp_sites[i] + 4;
        if (site)
            continue;                            /* 0x31b0 */
        h = (h ^ *(const uint8_t *)(uintptr_t)a) * 0x01000193u;
    }
    static const struct { uint32_t va, bits; } k[] = {
        { 0x5f0680u, 0x3f800000u }, { 0x5f0964u, 0x3fc00000u }, { 0x5f096cu, 0x3f000000u },
        { 0x5f0688u, 0xbf800000u }, { 0x5f0968u, 0x3c23d70au }, { 0x5f0d28u, 0x80000000u },
        { 0x5f5540u, 0x80000000u }, { 0x5f5550u, 0x7fffffffu },
    };
    /* PERF_PROFILE 0x131f1e */
    int ok = h == BB_FN_FNV && code_call_target(BB_CALLER) && code_call_target(BB_SITE_MAP) &&
             code_call_target(BB_SITE_SIN) && !memcmp((const void *)(uintptr_t)BB_FN, "\x55\x89\xe5\x53\x57", 5);
    for (size_t i = 0; ok && i < sizeof k / sizeof k[0]; i++)
        ok = *(const uint32_t *)(uintptr_t)k[i].va == k[i].bits;
    for (int i = 0; ok && i < 4; i++) {
        uint32_t t = code_call_target(bb_getapp_sites[i]);
        ok = t == BB_GETAPP || t == BB_GETAPP + GMV_LOW_DELTA;
    }
    if (!ok) {
        gmv_log("[bbfast] the billboard turn 0x%06x is not as expected (bytes FNV %08x, want %08x); skipped\n",
                BB_FN, h, BB_FN_FNV);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[bbfast] VirtualAlloc for the trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    memcpy(tr, (const void *)(uintptr_t)BB_FN, 5);   /* push ebp; mov ebp,esp; push ebx; push edi */
    tr[5] = 0xe9;
    int32_t rel = (int32_t)((BB_FN + 5) - (uint32_t)(uintptr_t)(tr + 10));
    memcpy(tr + 6, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), tr, 4096);
    g_orig = (void (*)(void *))(void *)tr;

    if (GMV_DEV) {
        const char *v = gmv_dbgenv("GMV_BB_VERIFY");
        if (gmv_env_on(v)) {
            g_fwd_map = (bb_mapfn)(uintptr_t)code_call_target(BB_SITE_MAP);
            g_fwd_sin = (bb_mathfn)(uintptr_t)code_call_target(BB_SITE_SIN);
            g_verify = code_call(BB_SITE_MAP, (void *)bbv_map) && code_call(BB_SITE_SIN, (void *)bbv_sin);
            if (!g_verify)
                gmv_log("[bbfast] BB_VERIFY: write at the original's calls failed (%lu)\n", GetLastError());
        }
    }
    if (!code_jmp(BB_FN, (void *)bb_entry)) {
        gmv_log("[bbfast] write at 0x%06x failed (%lu)\n", BB_FN, GetLastError());
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    g_on = 1;
    gmv_log("[bbfast] billboard turn 0x%06x: range tests before the mesh lookup%s\n", BB_FN,
            g_verify ? "; GMV_BB_VERIFY: checked against the original every call" : "");
}
