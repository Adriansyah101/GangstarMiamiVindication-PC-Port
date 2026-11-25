#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"

#define BP_CALLSITE   0x001c1a1bu   /* call 0x1b92f0 */
#define BP_QUERY_AABB 0x001b92f0u
#define BP_BOUNDS_FN  0x001b9140u
#define BP_MAXPROXIES 0x00649680u
#define BP_AXIS_QUERY 0x001b8d50u
#define BP_DESTROY_CALL 0x001b9d82u /* call 0x1b8d50 */
#define BP_CREATE_CALL  0x001b9f02u /* call 0x1b8d50 */

typedef struct { uint32_t value, proxy, stab; } bp_bound;
typedef struct { uint32_t lower[2], upper[2], overlap, stamp; void *user; } bp_proxy;

#define BP_POOL(bp)   (*(bp_proxy **)((char *)(bp) + 0x20))
#define BP_BOUNDS(bp, a) (*(bp_bound **)((char *)(bp) + 0x28 + 4 * (a)))
#define BP_NPROXY(bp) (*(uint32_t *)((char *)(bp) + 0x50))
#define BP_STAMP(bp)  (*(uint32_t *)((char *)(bp) + 0x54))
#define BP_RESULTS(bp) (*(uint32_t **)((char *)(bp) + 0x30))
#define BP_NRESULT(bp) (*(uint32_t *)((char *)(bp) + 0x34))

#define BP_ALLOC 0x000908a0u
#define BP_FREE  0x00090430u

typedef void (*bp_bounds_fn)(void *bp, uint32_t *lower, uint32_t *upper, const void *aabb);
typedef int  (*bp_query_fn)(void *bp, const void *aabb, void **user, int max);
typedef void (*bp_axis_fn)(void *bp, int *lq, int *uq, uint32_t lv, uint32_t uv, bp_bound *b, int n, int axis);

static volatile LONG g_busy;
static int g_on, g_axis_on;
static uint32_t *g_cand;
static uint32_t *g_key;
static uint32_t *g_seq, *g_ref;
static uint32_t g_cap;
static int g_verify;
static unsigned g_mismatch, g_axmismatch;

/* BinarySearch 0x1b8d50 */
static uint32_t bp_search(const bp_bound *b, int n, uint32_t v)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (v < b[mid].value)
            hi = mid - 1;
        else if (v > b[mid].value)
            lo = mid + 1;
        else
            return (uint32_t)mid;
    }
    return (uint32_t)lo;
}

static struct {
    unsigned queries, results, verified, busy, big, wide_hit, wide_miss, rebuilds;
    unsigned long long walked, range, stab;
} g_st;

static uint32_t *g_wide[2], g_nwide[2];
static const void *g_wide_bp[2];
static uint32_t g_wthr[2];
static uint8_t *g_mark[2];
static uint32_t g_markcap;
static unsigned g_since[2], g_long[2];
static int g_markfail;
static uint32_t *g_span, *g_wk, *g_aid;
static uint32_t *g_farl;
static uint32_t g_hist[2][32];
static uint32_t **const g_bufs[] = { &g_cand, &g_key, &g_wide[0], &g_wide[1], &g_seq, &g_ref,
                                     &g_farl, &g_span, &g_wk, &g_aid };

static void bp_wide_rebuild(void *bp, int a, uint32_t n)
{
    const bp_bound *b = BP_BOUNDS(bp, a);
    const bp_proxy *pool = BP_POOL(bp);
    g_since[a] = g_long[a] = 0;
    if (g_markfail)
        return;
    uint32_t hist[32] = { 0 }, top = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (b[i].value & 1)
            continue;
        uint32_t id = b[i].proxy;
        const bp_proxy *p = pool + id;
        hist[31 - __builtin_clz((p->upper[a] - p->lower[a]) | 1)]++;
        if (id > top)
            top = id;
    }
    uint32_t t = 4, best = 0xffffffffu, above = 0;
    for (int k = 31; k >= 4; k--) {
        above += hist[k];
        uint32_t cost = above + (k < 30 ? (1u << k) / 2 : 0x40000000u);
        if (cost <= best) {
            best = cost;
            t = (uint32_t)k;
        }
    }
    uint32_t T = 1u << t;
    uint32_t need = *(const uint32_t *)(uintptr_t)BP_MAXPROXIES;
    if (need < top + 1)
        need = top + 1;
    if (need > g_markcap) {
        for (int x = 0; x < 2; x++) {
            uint8_t *m = realloc(g_mark[x], need);
            if (!m) {
                g_markfail = 1;
                g_wide_bp[0] = g_wide_bp[1] = NULL;
                gmv_log("[broadphase] out of memory for the wide lists; the original spanning walk kept\n");
                return;
            }
            memset(m + g_markcap, 0, need - g_markcap);
            g_mark[x] = m;
        }
        g_markcap = need;
    }
    uint8_t *mk = g_mark[a];
    for (uint32_t w = 0; w < g_nwide[a]; w++)
        mk[g_wide[a][w]] = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (b[i].value & 1)
            continue;
        uint32_t id = b[i].proxy;
        const bp_proxy *p = pool + id;
        if (p->upper[a] - p->lower[a] >= T && k < g_cap) {
            g_wide[a][k++] = id;
            mk[id] = 1;
        }
    }
    g_nwide[a] = k;
    g_wthr[a] = T;
    g_wide_bp[a] = bp;
    if (GMV_DEV) {
        g_st.rebuilds++;
        memcpy(g_hist[a], hist, sizeof hist);
    }
}

/* 0x1b8d50 */
static uint32_t bp_spanning(void *bp, const bp_bound *b, int n, int a, uint32_t lq, uint32_t *out,
                            unsigned long long *look, int *used)
{
    const bp_proxy *pool = BP_POOL(bp);
    uint32_t s = b[lq - 1].stab;
    *used = 0;
    if (g_wide_bp[a] == bp) {
        uint32_t ga = 0;
        for (uint32_t w = g_nwide[a]; w--;) {
            uint32_t id = g_wide[a][w];
            const bp_proxy *p = pool + id;
            uint32_t L = p->lower[a], U = p->upper[a];
            if (L >= lq || U < lq || U >= (uint32_t)n)
                continue;
            if (b[L].proxy != id || (b[L].value & 1) || b[U].proxy != id || !(b[U].value & 1))
                continue;
            g_aid[ga] = id;
            g_farl[ga] = L;
            ga++;
        }
        *look += g_nwide[a];
        if (ga <= s) {
            const uint8_t *mk = g_mark[a];
            uint32_t need = s - ga, nw = 0;
            int i = (int)lq - 1;
            for (; need && i >= 0; i--) {
                if (b[i].value & 1)
                    continue;
                uint32_t id = b[i].proxy;
                if (id < g_markcap && mk[id])
                    continue;
                if (pool[id].upper[a] < lq)
                    continue;
                g_wk[nw++] = id;
                need--;
            }
            uint32_t walked = lq - 1 - (uint32_t)i;
            *look += walked;
            if (!need) {
                for (uint32_t x = 1; x < ga; x++) {
                    uint32_t l = g_farl[x], id = g_aid[x], j = x;
                    for (; j && g_farl[j - 1] < l; j--) {
                        g_farl[j] = g_farl[j - 1];
                        g_aid[j] = g_aid[j - 1];
                    }
                    g_farl[j] = l;
                    g_aid[j] = id;
                }
                uint32_t x = 0, y = 0, k = 0;
                while (x < nw || y < ga) {
                    if (y == ga || (x < nw && pool[g_wk[x]].lower[a] > g_farl[y]))
                        out[k++] = g_wk[x++];
                    else
                        out[k++] = g_aid[y++];
                }
                *used = 1;
                if (walked > 4 * g_wthr[a])
                    g_long[a]++;
                if (++g_since[a] >= 8192 || g_long[a] >= 32)
                    bp_wide_rebuild(bp, a, (uint32_t)n);
                return k;
            }
        }
    }
    uint32_t k = 0;
    int i = (int)lq - 1;
    for (uint32_t left = s; left && i >= 0; i--) {
        if (b[i].value & 1)
            continue;
        if (pool[b[i].proxy].upper[a] < lq)
            continue;
        out[k++] = b[i].proxy;
        left--;
    }
    *look += lq - 1 - (uint32_t)i;
    bp_wide_rebuild(bp, a, (uint32_t)n);
    return k;
}

static int bp_fast(void *bp, const void *aabb, void **user, int max)
{
    uint32_t lo[2], hi[2];
    ((bp_bounds_fn)(uintptr_t)BP_BOUNDS_FN)(bp, lo, hi, aabb);
    int n = 2 * (int)BP_NPROXY(bp);
    uint32_t count = 0;
    if (n > 0) {
        uint32_t lq[2], uq[2], s[2], cost[2];
        for (int a = 0; a < 2; a++) {
            const bp_bound *b = BP_BOUNDS(bp, a);
            lq[a] = bp_search(b, n, lo[a]);
            uq[a] = bp_search(b, n, hi[a]);
            s[a] = lq[a] ? b[lq[a] - 1].stab : 0;
            uint32_t back = g_wthr[a] / 2 < lq[a] ? g_wthr[a] / 2 : lq[a];
            cost[a] = (uq[a] - lq[a]) + (s[a] ? (g_wide_bp[a] == bp ? g_nwide[a] + back : lq[a]) : 0);
        }
        int e = cost[0] < cost[1] ? 0 : 1, o = e ^ 1;
        const bp_bound *b = BP_BOUNDS(bp, e);
        const bp_proxy *pool = BP_POOL(bp);
        for (uint32_t i = lq[e]; i < uq[e]; i++) {
            if (b[i].value & 1)
                continue;
            const bp_proxy *p = pool + b[i].proxy;
            if (p->lower[o] < uq[o] && p->upper[o] >= lq[o]) {
                g_cand[count] = b[i].proxy;
                g_key[count] = p->lower[1];
                count++;
            }
        }
        unsigned long long stab = 0;
        if (s[e]) {
            int used;
            uint32_t m = bp_spanning(bp, b, n, e, lq[e], g_span, &stab, &used);
            for (uint32_t j = 0; j < m; j++) {
                const bp_proxy *p = pool + g_span[j];
                if (p->lower[o] < uq[o] && p->upper[o] >= lq[o]) {
                    g_cand[count] = g_span[j];
                    g_key[count] = p->lower[1];
                    count++;
                }
            }
            if (GMV_DEV) {
                if (used)
                    g_st.wide_hit++;
                else
                    g_st.wide_miss++;
            }
        }
        unsigned walked = (uq[e] - lq[e]) + stab;
        if (GMV_DEV) {
            g_st.range += uq[e] - lq[e];
            g_st.stab += stab;
        }
        if (count > 1) {
            for (uint32_t i = 0; i < count; i++)
                g_key[i] = g_key[i] >= lq[1] ? g_key[i] - lq[1] : 0x80000000u + (lq[1] - g_key[i]);
            for (uint32_t i = 1; i < count; i++) {
                uint32_t k = g_key[i], c = g_cand[i], j = i;
                for (; j && g_key[j - 1] > k; j--) {
                    g_key[j] = g_key[j - 1];
                    g_cand[j] = g_cand[j - 1];
                }
                g_key[j] = k;
                g_cand[j] = c;
            }
        }
        if (GMV_DEV) {
            g_st.walked += walked;
            g_st.results += count;
        }
    }
    int out = (int)count < max ? (int)count : max;
    if (out < 0)
        out = 0;
    const bp_proxy *pool = BP_POOL(bp);
    for (int i = 0; i < out; i++)
        user[i] = pool[g_cand[i]].user;
    /* IncrementTimeStamp 0x1b93cc..0x1b9427 */
    if (BP_STAMP(bp) == 0xffff) {
        uint32_t np = *(const uint32_t *)(uintptr_t)BP_MAXPROXIES;
        bp_proxy *pp = BP_POOL(bp);
        for (uint32_t i = 0; i < np; i++)
            pp[i].stamp = 0;
        BP_STAMP(bp) = 1;
    } else {
        BP_STAMP(bp)++;
    }
    return out;
}

static int bp_room(uint32_t np)
{
    if (np <= g_cap)
        return 1;
    uint32_t cap = g_cap;
    while (cap < np && cap < (1u << 24))
        cap *= 2;
    if (cap < np)
        return 0;
    for (size_t i = 0; i < sizeof g_bufs / sizeof g_bufs[0]; i++) {
        uint32_t *p = realloc(*g_bufs[i], cap * sizeof **g_bufs[i]);
        if (!p)
            return 0;
        *g_bufs[i] = p;
    }
    g_cap = cap;
    gmv_trace("[broadphase] room for %u proxies now (%u in use, b2_maxProxies %u)\n", cap, np,
            *(const uint32_t *)(uintptr_t)BP_MAXPROXIES);
    return 1;
}

/* 0x1b92f0 0x2472c +0x20 +0x28/+0x2c +0x30 +0x50 */
__attribute__((cdecl, force_align_arg_pointer))
static int bp_query(void *bp, const void *aabb, void **user, int max)
{
    bp_query_fn orig = (bp_query_fn)(uintptr_t)BP_QUERY_AABB;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        if (GMV_DEV)
            g_st.busy++;
        return orig(bp, aabb, user, max);
    }
    if (!bp_room(BP_NPROXY(bp))) {
        if (GMV_DEV)
            g_st.big++;
        InterlockedExchange(&g_busy, 0);
        return orig(bp, aabb, user, max);
    }
    int r;
    if (GMV_DEV)
        g_st.queries++;
    if (!GMV_DEV || !g_verify) {
        r = bp_fast(bp, aabb, user, max);
    } else {
        void *a[256], *f[256];
        int m = max < 256 ? max : 256;
        int na = orig(bp, aabb, a, m);
        int nf = bp_fast(bp, aabb, f, m);
        g_st.verified++;
        if (na != nf || memcmp(a, f, (size_t)(na > 0 ? na : 0) * sizeof a[0])) {
            if (g_mismatch++ < 20)
                gmv_log("[broadphase] MISMATCH #%u: original %d results, fast %d (max %d, %u proxies)\n",
                        g_mismatch, na, nf, max, BP_NPROXY(bp));
        }
        if (na > 0)
            memcpy(user, a, (size_t)na * sizeof a[0]);
        r = na;
    }
    InterlockedExchange(&g_busy, 0);
    return r;
}

static struct {
    unsigned calls, busy, big, wide_hit, wide_miss, verified;
    unsigned long long range, stab;
} g_ax;

static unsigned g_tick_made, g_tick_gone;
static int g_tick_n;

void gmv_broadphase_tick(unsigned *made, unsigned *gone, int *bounds)
{
    *made = g_tick_made;
    *gone = g_tick_gone;
    *bounds = g_tick_n;
    g_tick_made = g_tick_gone = 0;
}

/* 0x1b8d50 IncrementOverlapCount */
static uint32_t bp_axis_pick(void *bp, uint32_t lq, uint32_t uq, const bp_bound *b, int n, int a, int fast,
                             uint32_t *seq)
{
    const bp_proxy *pool = BP_POOL(bp);
    uint32_t k = 0;
    for (uint32_t i = lq; i < uq; i++)
        if (!(b[i].value & 1))
            seq[k++] = b[i].proxy;
    if (fast && GMV_DEV)
        g_ax.range += uq - lq;
    if (lq == 0)
        return k;
    if (fast) {
        int used;
        unsigned long long look = 0;
        k += bp_spanning(bp, b, n, a, lq, seq + k, &look, &used);
        if (GMV_DEV) {
            g_ax.stab += look;
            if (used)
                g_ax.wide_hit++;
            else
                g_ax.wide_miss++;
        }
        return k;
    }
    for (uint32_t left = b[lq - 1].stab, i = lq - 1; left && i < lq; i--) {
        if (b[i].value & 1)
            continue;
        if (pool[b[i].proxy].upper[a] < lq)
            continue;
        seq[k++] = b[i].proxy;
        left--;
    }
    return k;
}

/* IncrementOverlapCount 0x1b8e00..0x1b8e4e */
static void bp_axis_apply(void *bp, const uint32_t *seq, uint32_t k)
{
    bp_proxy *pool = BP_POOL(bp);
    uint32_t st = BP_STAMP(bp);
    for (uint32_t j = 0; j < k; j++) {
        bp_proxy *p = pool + seq[j];
        if (p->stamp < st) {
            p->stamp = st;
            p->overlap = 1;
        } else {
            p->overlap = 2;
            BP_RESULTS(bp)[BP_NRESULT(bp)++] = seq[j];
        }
    }
}

/* 0x1b8d50 */
__attribute__((cdecl, force_align_arg_pointer))
static void bp_axisq(void *bp, int *lqo, int *uqo, uint32_t lv, uint32_t uv, bp_bound *b, int n, int a)
{
    bp_axis_fn orig = (bp_axis_fn)(uintptr_t)BP_AXIS_QUERY;
    if (a == 0) {
        if ((uint32_t)(uintptr_t)__builtin_return_address(0) == BP_CREATE_CALL + 5) {
#if GMV_DEV
            g_tick_made++;
#endif
        } else {
#if GMV_DEV
            g_tick_gone++;
#endif
        }
        if (GMV_DEV)
            g_tick_n = n;
    }
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        if (GMV_DEV)
            g_ax.busy++;
        orig(bp, lqo, uqo, lv, uv, b, n, a);
        return;
    }
    uint32_t need = BP_NPROXY(bp);
    if (n > 0 && (uint32_t)n / 2 + 1 > need)
        need = (uint32_t)n / 2 + 1;
    if ((a & ~1) || !bp_room(need)) {
        if (GMV_DEV)
            g_ax.big++;
        InterlockedExchange(&g_busy, 0);
        orig(bp, lqo, uqo, lv, uv, b, n, a);
        return;
    }
    if (GMV_DEV)
        g_ax.calls++;
    uint32_t lq = 0, uq = 0;
    if (n > 0) {
        lq = bp_search(b, n, lv);
        uq = bp_search(b, n, uv);
    }
    uint32_t k = bp_axis_pick(bp, lq, uq, b, n, a, 1, g_seq);
    if (!GMV_DEV || !g_verify) {
        bp_axis_apply(bp, g_seq, k);
        *lqo = (int)lq;
        *uqo = (int)uq;
    } else {
        uint32_t r = bp_axis_pick(bp, lq, uq, b, n, a, 0, g_ref);
        int bad = r != k || memcmp(g_seq, g_ref, k * sizeof *g_seq);
        const bp_proxy *pool = BP_POOL(bp);
        uint32_t st = BP_STAMP(bp), c0 = BP_NRESULT(bp), m = 0;
        for (uint32_t j = 0; j < k; j++)
            if (pool[g_seq[j]].stamp >= st)
                g_key[m++] = g_seq[j];
        orig(bp, lqo, uqo, lv, uv, b, n, a);
        if (*lqo != (int)lq || *uqo != (int)uq || BP_NRESULT(bp) - c0 != m ||
            memcmp(BP_RESULTS(bp) + c0, g_key, m * sizeof *g_key))
            bad = 1;
        for (uint32_t j = 0; j < k && !bad; j++)
            if (pool[g_seq[j]].stamp != st)
                bad = 1;
        g_ax.verified++;
        if (bad && g_axmismatch++ < 20)
            gmv_log("[broadphase] MISMATCH (create/destroy) #%u: axis %d, n %d, range %u..%u, ours %u picked, "
                    "the original's walk %u\n", g_axmismatch, a, n, lq, uq, k, r);
    }
    InterlockedExchange(&g_busy, 0);
}

#if GMV_DEV
void gmv_broadphase_report(double seconds)
{
    if (g_axis_on && (g_ax.calls | g_ax.busy | g_ax.big)) {
        unsigned q = g_ax.calls ? g_ax.calls : 1;
        gmv_logfile("[perf]   broadphase create/destroy: %.0f axis queries a second (%u to the original: %u "
                    "another thread's, %u too many proxies), %.0f bounds in range and %.0f in the spanning "
                    "walk a query (wide list looked at); the list answered %u, the original walk %u%s%s\n",
                    g_ax.calls / seconds, g_ax.busy + g_ax.big, g_ax.busy, g_ax.big,
                    (double)g_ax.range / q, (double)g_ax.stab / q, g_ax.wide_hit, g_ax.wide_miss,
                    g_verify ? ", verified against the original: " : "",
                    g_verify ? (g_axmismatch ? "MISMATCHES (see above)" : "all identical") : "");
        memset(&g_ax, 0, sizeof g_ax);
    }
    if (!g_on || !(g_st.queries | g_st.busy | g_st.big))
        return;
    unsigned q = g_st.queries ? g_st.queries : 1;
    gmv_logfile("[perf]   broadphase: %.0f fast world queries a second (%u to the original: %u another "
                "thread's, %u too many proxies), %.0f bounds walked (%.0f in range, %.0f spanning walk) "
                "and %.1f results a query; the wide list answered %u, the original walk %u (%u rebuilt; "
                "x/y lists %u/%u proxies, span >= %u/%u bounds)%s%s\n",
                g_st.queries / seconds, g_st.busy + g_st.big, g_st.busy, g_st.big,
                (double)g_st.walked / q, (double)g_st.range / q, (double)g_st.stab / q,
                (double)g_st.results / q, g_st.wide_hit, g_st.wide_miss, g_st.rebuilds,
                g_nwide[0], g_nwide[1], g_wthr[0], g_wthr[1],
                g_verify ? ", verified against the original: " : "",
                g_verify ? (g_mismatch ? "MISMATCHES (see above)" : "all identical") : "");
    static unsigned spans;
    if (spans++ % 12 == 0) {
        for (int a = 0; a < 2; a++) {
            char line[512];
            int len = 0;
            for (int k = 0; k < 32 && len < (int)sizeof line - 24; k++)
                if (g_hist[a][k])
                    len += snprintf(line + len, sizeof line - (size_t)len, " %u+:%u", 1u << k, g_hist[a][k]);
            line[len] = 0;
            gmv_logfile("[perf]   broadphase %c spans (bounds: proxies):%s\n", a ? 'y' : 'x', line);
        }
    }
    memset(&g_st, 0, sizeof g_st);
}
#endif

static int bp_aim(uint32_t at, void *fn)
{
    if (code_call(at, fn))
        return 1;
    gmv_log("[broadphase] write at 0x%08x failed (%lu)\n", at, GetLastError());
    return 0;
}

void gmv_apply_broadphase(void)
{
    if (gmv_env_off(gmv_dbgenv("GMV_FAST_BROADPHASE"))) {
        gmv_log("[broadphase] FAST_BROADPHASE=0: the original query kept\n");
        return;
    }
    uint32_t maxp = *(const uint32_t *)(uintptr_t)BP_MAXPROXIES;
    if (maxp == 0 || maxp > (1u << 20)) {
        gmv_log("[broadphase] unexpected b2_maxProxies %u; fast queries skipped\n", maxp);
        return;
    }
    g_cap = maxp;
    for (size_t i = 0; i < sizeof g_bufs / sizeof g_bufs[0]; i++) {
        *g_bufs[i] = malloc(maxp * sizeof **g_bufs[i]);
        if (!*g_bufs[i]) {
            gmv_log("[broadphase] out of memory; fast queries skipped\n");
            return;
        }
    }
    if (GMV_DEV) {
        const char *v = gmv_dbgenv("GMV_BP_VERIFY");
        g_verify = gmv_env_on(v);
    }

    static const unsigned char head[9] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x5c };
    if (code_call_target(BP_CALLSITE) != BP_QUERY_AABB || memcmp((const void *)(uintptr_t)BP_QUERY_AABB, head, sizeof head)) {
        gmv_log("[broadphase] unexpected code at 0x%08x/0x%08x; fast world query skipped\n", BP_CALLSITE,
                BP_QUERY_AABB);
    } else if (bp_aim(BP_CALLSITE, (void *)&bp_query)) {
        g_on = 1;
        gmv_log("[broadphase] fast world query installed at 0x%08x (b2_maxProxies %u)%s\n", BP_CALLSITE, maxp,
                g_verify ? "; GMV_BP_VERIFY: every query checked against the original" : "");
    }

    static const unsigned char ahead[9] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x18 };
    /* +0x30/+0x34 */
    static const unsigned char stab[] = { 0x8b, 0x44, 0x81, 0x08 };            /* 0x1b8e65 */
    static const unsigned char upper[] = { 0x3b, 0x5c, 0x90, 0x08, 0x77 };     /* 0x1b8e95 */
    static const unsigned char res[] = { 0x8b, 0x50, 0x34, 0x8b, 0x40, 0x30 }; /* 0x1b8ee6 */
    if (code_call_target(BP_DESTROY_CALL) != BP_AXIS_QUERY || code_call_target(BP_CREATE_CALL) != BP_AXIS_QUERY ||
        memcmp((const void *)(uintptr_t)BP_AXIS_QUERY, ahead, sizeof ahead) ||
        memcmp((const void *)(uintptr_t)0x001b8e65u, stab, sizeof stab) ||
        memcmp((const void *)(uintptr_t)0x001b8e95u, upper, sizeof upper) ||
        memcmp((const void *)(uintptr_t)0x001b8ee6u, res, sizeof res)) {
        gmv_log("[broadphase] unexpected code at 0x%08x/0x%08x/0x%08x; fast create/destroy query skipped\n",
                BP_AXIS_QUERY, BP_DESTROY_CALL, BP_CREATE_CALL);
    } else if (bp_aim(BP_DESTROY_CALL, (void *)&bp_axisq)) {
        if (bp_aim(BP_CREATE_CALL, (void *)&bp_axisq)) {
            g_axis_on = 1;
            gmv_log("[broadphase] fast create/destroy query installed at 0x%08x and 0x%08x%s\n",
                    BP_CREATE_CALL, BP_DESTROY_CALL,
                    g_verify ? "; GMV_BP_VERIFY: every one checked against the original" : "");
        } else {
            bp_aim(BP_DESTROY_CALL, (void *)(uintptr_t)BP_AXIS_QUERY);
        }
    }
}

/* 0x1b8fa0 0x1bd0a0 +0x20 0x1c +0x28/+0x2c 0x18 +0x30 */
static uint32_t bp_pair_hash(uint32_t a, uint32_t b)         /* 0x1bcee0 */
{
    uint32_t k = a > b ? a << 16 | b : b << 16 | a;
    k = ~k + (k << 15);
    k ^= k >> 12;
    k += k << 2;
    k ^= k >> 4;
    k *= 2057;
    return k ^ k >> 16;
}

int gmv_bp_grow(void *bp_, uint32_t np, uint32_t npairs)
{
    typedef void *(*alloc_fn)(int);
    typedef void (*free_fn)(void *);
    alloc_fn al = (alloc_fn)(uintptr_t)BP_ALLOC;
    free_fn fr = (free_fn)(uintptr_t)BP_FREE;
    char *bp = bp_;
    const uint32_t *g = (const uint32_t *)(uintptr_t)BP_MAXPROXIES;
    uint32_t op = g[0], opairs = g[1], otable = g[2];
    static const struct { int at, per, proxies; } t[7] = {
        {0x20, 0x1c, 1}, {0x28, 0x18, 1}, {0x2c, 0x18, 1}, {0x30, 4, 1},
        {0x08, 0x14, 0}, {0x14, 8, 0}, {0x1c, 4, 0},
    };
    char *now[7], *was[7];
    int ok = 1;
    for (int i = 0; i < 7; i++) {
        now[i] = al((int)((t[i].proxies ? np : npairs) * t[i].per));
        was[i] = *(char **)(bp + t[i].at);
        ok &= now[i] != NULL;
    }
    if (!ok) {
        for (int i = 0; i < 7; i++)
            if (now[i])
                fr(now[i]);
        return 0;
    }
    for (int i = 0; i < 6; i++)
        memcpy(now[i], was[i], (size_t)(t[i].proxies ? op : opairs) * t[i].per);

    bp_proxy *pool = (bp_proxy *)now[0];
    for (uint32_t i = op; i < np; i++) {
        pool[i].lower[0] = i + 1 < np ? i + 1 : *(uint32_t *)(bp + 0x24);
        pool[i].overlap = 0xffff;
        pool[i].stamp = 0;
        pool[i].user = NULL;
    }
    if (np > op)
        *(uint32_t *)(bp + 0x24) = op;

    typedef struct { void *user; int32_t id1, id2, next; uint32_t status; } bp_pair;
    bp_pair *pairs = (bp_pair *)now[4];
    for (uint32_t i = opairs; i < npairs; i++) {
        pairs[i].user = NULL;
        pairs[i].id1 = pairs[i].id2 = -1;
        pairs[i].next = i + 1 < npairs ? (int32_t)(i + 1) : *(int32_t *)(bp + 0x0c);
        pairs[i].status = 0;
    }
    if (npairs > opairs)
        *(int32_t *)(bp + 0x0c) = (int32_t)opairs;

    int32_t *table = (int32_t *)now[6];
    const int32_t *otab = (const int32_t *)was[6];
    memset(table, 0xff, (size_t)npairs * 4);
    for (uint32_t b = 0; b < otable; b++)
        for (int32_t i = otab[b], next; i != -1; i = next) {
            uint32_t h = bp_pair_hash((uint32_t)pairs[i].id1, (uint32_t)pairs[i].id2) & (npairs - 1);
            next = pairs[i].next;
            pairs[i].next = table[h];
            table[h] = i;
        }

    for (int i = 0; i < 7; i++) {
        *(char **)(bp + t[i].at) = now[i];
        fr(was[i]);
    }
    return 1;
}

/* 0x1bcee0 */
int gmv_bp_shrink(void *bp_, uint32_t np, uint32_t npairs)
{
    typedef void *(*alloc_fn)(int);
    typedef void (*free_fn)(void *);
    typedef struct { void *user; int32_t id1, id2, next; uint32_t status; } bp_pair;
    alloc_fn al = (alloc_fn)(uintptr_t)BP_ALLOC;
    free_fn fr = (free_fn)(uintptr_t)BP_FREE;
    char *bp = bp_;
    const uint32_t *g = (const uint32_t *)(uintptr_t)BP_MAXPROXIES;
    uint32_t op = g[0], otable = g[2], used = BP_NPROXY(bp), moved = 0;
    if (np > op || npairs > g[1])
        return -1;
    if (used > np || *(uint32_t *)(bp + 0x10) > npairs || *(uint32_t *)(bp + 0x18))
        return 0;

    const bp_proxy *old = BP_POOL(bp);
    const bp_pair *opairs = *(const bp_pair **)(bp + 0x08);
    const int32_t *otab = *(const int32_t **)(bp + 0x1c);
    uint32_t live = 0, chained = 0;
    for (uint32_t i = 0; i < op; i++) {
        if (old[i].overlap == 0xffff)
            continue;
        live++;
        if (i >= np && (!old[i].user || *(uint32_t *)((char *)old[i].user + 0x20) != i))
            return -1;
    }
    if (live != used)
        return -1;
    for (uint32_t b = 0; b < otable; b++)
        for (int32_t i = otab[b]; i != -1; i = opairs[i].next) {
            uint32_t a = (uint32_t)opairs[i].id1, c = (uint32_t)opairs[i].id2;
            if (++chained > npairs || a >= op || c >= op || old[a].overlap == 0xffff || old[c].overlap == 0xffff)
                return -1;
        }
    if (chained != *(uint32_t *)(bp + 0x10))
        return -1;

    static const struct { int at, per, proxies; } t[7] = {
        {0x20, 0x1c, 1}, {0x28, 0x18, 1}, {0x2c, 0x18, 1}, {0x30, 4, 1},
        {0x08, 0x14, 0}, {0x14, 8, 0}, {0x1c, 4, 0},
    };
    char *now[7], *was[7];
    uint32_t *to = al((int)((op - np + 1) * 4));
    int ok = to != NULL;
    for (int i = 0; i < 7; i++) {
        now[i] = al((int)((t[i].proxies ? np : npairs) * t[i].per));
        was[i] = *(char **)(bp + t[i].at);
        ok &= now[i] != NULL;
    }
    if (!ok) {
        for (int i = 0; i < 7; i++)
            if (now[i])
                fr(now[i]);
        if (to)
            fr(to);
        return -1;
    }

    bp_proxy *pool = (bp_proxy *)now[0];
    memcpy(pool, old, (size_t)np * 0x1c);
    uint32_t place = 0;
    for (uint32_t i = np; i < op; i++) {
        if (old[i].overlap == 0xffff)
            continue;
        while (pool[place].overlap != 0xffff)
            place++;
        pool[place] = old[i];
        *(uint32_t *)((char *)old[i].user + 0x20) = place;
        to[i - np] = place;
        moved++;
    }
    uint32_t head = 0xffffffffu;
    for (uint32_t i = np; i--;)
        if (pool[i].overlap == 0xffff) {
            pool[i].lower[0] = head;
            head = i;
        }
    *(uint32_t *)(bp + 0x24) = head;

    for (int a = 0; a < 2; a++) {
        bp_bound *b = (bp_bound *)now[1 + a];
        memcpy(b, was[1 + a], (size_t)used * 2 * sizeof *b);
        for (uint32_t i = 0; i < used * 2; i++)
            if (b[i].proxy >= np)
                b[i].proxy = to[b[i].proxy - np];
    }

    bp_pair *pairs = (bp_pair *)now[4];
    int32_t *table = (int32_t *)now[6];
    uint32_t n = 0;
    memset(table, 0xff, (size_t)npairs * 4);
    for (uint32_t b = 0; b < otable; b++)
        for (int32_t i = otab[b]; i != -1; i = opairs[i].next) {
            uint32_t a = (uint32_t)opairs[i].id1, c = (uint32_t)opairs[i].id2;
            a = a >= np ? to[a - np] : a;
            c = c >= np ? to[c - np] : c;
            uint32_t h = bp_pair_hash(a, c) & (npairs - 1);
            pairs[n] = opairs[i];
            pairs[n].id1 = (int32_t)(a < c ? a : c);
            pairs[n].id2 = (int32_t)(a < c ? c : a);
            pairs[n].next = table[h];
            table[h] = (int32_t)n++;
        }
    *(int32_t *)(bp + 0x0c) = n < npairs ? (int32_t)n : -1;
    for (uint32_t i = n; i < npairs; i++) {
        pairs[i].user = NULL;
        pairs[i].id1 = pairs[i].id2 = -1;
        pairs[i].next = i + 1 < npairs ? (int32_t)(i + 1) : -1;
        pairs[i].status = 0;
    }

    for (int i = 0; i < 7; i++) {
        *(char **)(bp + t[i].at) = now[i];
        fr(was[i]);
    }
    fr(to);
    for (int a = 0; a < 2; a++) {
        for (uint32_t w = 0; g_mark[a] && w < g_nwide[a]; w++)
            g_mark[a][g_wide[a][w]] = 0;
        g_nwide[a] = 0;
        g_wide_bp[a] = NULL;
    }
    gmv_trace("[broadphase] tables down to %u proxies, %u pairs: %u proxies in use (%u moved), %u pairs\n", np, npairs,
            used, moved, n);
    return 1;
}
