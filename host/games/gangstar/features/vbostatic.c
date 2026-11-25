#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>
#include <emmintrin.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "glshim.h"

static const uint32_t vs_skip[] = {0x468f82u, 0x315f7au, 0x3162b6u, 0x316854u, 0x469f4fu};

#define VS_CTOR_SITE 0x00501af1u
#define VS_CTOR 0x004c6ae0u
#define VS_MAP 0x00517b10u
#define VS_VTBL 0x00680a48u                             /* 0x5036c0 0x5035b0 */
static const unsigned char vs_map_pro[7] = {0x55, 0x89, 0xe5, 0x53, 0x8b, 0x4d, 0x08};
#define VS_SETDATA 0x005177f0u
static const unsigned char vs_set_pro[6] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x28};
typedef void (*setdata_fn)(void *buf, unsigned size, void *data, int own);
static setdata_fn g_setdata;

#define VS_QUIET 4
#define VS_BUDGET (3u << 20)
#define VS_BUDGET_STATIC (512u << 10)

typedef void (*ctor_fn)(void *obj, void *drv, int type, int usage, unsigned size, void *data, int keep);
typedef void *(*map_fn)(void *buf, int access);
typedef void (*dtor_fn)(void *obj);
typedef void (*bind_fn)(void *buf, int drop);
static map_fn g_map;
static dtor_fn g_dtor0, g_dtor1;
static bind_fn g_bind;
#define VS_BIND 0x00504580u

static int g_static;
static volatile LONG g_lockv;
static DWORD g_gl_tid;

#define VS_SITES 64
#define VS_SITE_MAX 2000
static struct site {
    uint32_t ret;
    unsigned n, by_usage[6], by_type[4];
    unsigned conv, rev_map, rev_hash;
    unsigned long long bytes, conv_bytes;
} g_site[VS_SITES];
static int g_nsite;
static unsigned g_more;

#if GMV_DEV
static volatile LONG g_map_n[8][2];
#endif

enum { W_WATCH, W_STATIC, W_NEVER };
typedef struct {
    uint8_t *obj;
    uint32_t s1, s2;
    uint16_t quiet;
    uint8_t state, site;
} went;
static went *g_w;
static int g_nw, g_wcap;
static int32_t *g_ix;
static uint32_t g_ixcap;
static int g_cursor, g_scursor;
static uint8_t **g_byname;
static uint32_t g_bynamecap;
static struct { unsigned conv, rev_map, rev_hash, rev_set, leaked, gone, kept; unsigned long long conv_bytes; } g_tot;

static void vs_lock(void)
{
    while (InterlockedCompareExchange(&g_lockv, 1, 0))
        YieldProcessor();
}

static void vs_unlock(void)
{
    InterlockedExchange(&g_lockv, 0);
}

static uint32_t vs_slot(const void *obj)
{
    uint32_t h = (uint32_t)(uintptr_t)obj;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h & (g_ixcap - 1);
}

static int vs_find(const void *obj)
{
    if (!g_ixcap)
        return -1;
    for (uint32_t i = vs_slot(obj);; i = (i + 1) & (g_ixcap - 1)) {
        int32_t k = g_ix[i];
        if (k < 0)
            return -1;
        if (g_w[k].obj == obj)
            return k;
    }
}

static int vs_rehash(uint32_t cap)
{
    int32_t *ix = malloc(cap * sizeof *ix);
    if (!ix)
        return 0;
    for (uint32_t i = 0; i < cap; i++)
        ix[i] = -1;
    free(g_ix);
    g_ix = ix;
    g_ixcap = cap;
    for (int k = 0; k < g_nw; k++) {
        uint32_t i = vs_slot(g_w[k].obj);
        while (g_ix[i] >= 0)
            i = (i + 1) & (cap - 1);
        g_ix[i] = k;
    }
    return 1;
}

static void vs_add(uint8_t *obj, int site)
{
    if (vs_find(obj) >= 0)
        return;
    if (g_nw == g_wcap) {
        int c = g_wcap ? g_wcap * 2 : 1024;
        went *w = realloc(g_w, (size_t)c * sizeof *w);
        if (!w)
            return;
        g_w = w;
        g_wcap = c;
    }
    if ((uint32_t)(g_nw + 1) * 2 > g_ixcap && !vs_rehash(g_ixcap ? g_ixcap * 2 : 4096))
        return;
    went *e = &g_w[g_nw];
    memset(e, 0, sizeof *e);
    e->obj = obj;
    e->site = (uint8_t)site;
    uint32_t i = vs_slot(obj);
    while (g_ix[i] >= 0)
        i = (i + 1) & (g_ixcap - 1);
    g_ix[i] = g_nw++;
}

static void vs_ix_del(uint32_t i)
{
    uint32_t m = g_ixcap - 1;
    g_ix[i] = -1;
    for (uint32_t j = (i + 1) & m; g_ix[j] >= 0; j = (j + 1) & m) {
        int32_t k = g_ix[j];
        uint32_t h = vs_slot(g_w[k].obj);
        if (((j - h) & m) >= ((j - i) & m)) {
            g_ix[i] = k;
            g_ix[j] = -1;
            i = j;
        }
    }
}

static void vs_event(const char *what, const uint8_t *b, int site);

static void vs_remove(const void *obj)
{
    int k = vs_find(obj);
    if (k < 0)
        return;
    if (g_w[k].state == W_STATIC)
        vs_event("destroyed static", obj, g_w[k].site);
    uint32_t i = vs_slot(obj);
    while (g_ix[i] != k)
        i = (i + 1) & (g_ixcap - 1);
    vs_ix_del(i);
    int last = g_nw - 1;
    if (k != last) {
        uint32_t j = vs_slot(g_w[last].obj);
        while (g_ix[j] != last)
            j = (j + 1) & (g_ixcap - 1);
        g_w[k] = g_w[last];
        g_ix[j] = k;
    }
    g_nw--;
    g_tot.gone++;
}

__attribute__((target("sse2")))
static void vs_sum(const uint8_t *p, uint32_t n, uint32_t *a, uint32_t *b)
{
    __m128i v1 = _mm_setzero_si128(), v2 = _mm_setzero_si128();
    uint32_t k = n / 16;
    for (uint32_t i = 0; i < k; i++) {
        v1 = _mm_add_epi32(v1, _mm_loadu_si128((const __m128i *)(p + i * 16)));
        v2 = _mm_add_epi32(v2, v1);
    }
    uint32_t l1[4], l2[4];
    _mm_storeu_si128((__m128i *)l1, v1);
    _mm_storeu_si128((__m128i *)l2, v2);
    uint32_t s1 = 0x12345678u ^ n, s2 = 0;
    for (int j = 0; j < 4; j++) {
        s1 = s1 * 31 + l1[j];
        s2 = s2 * 31 + l2[j];
    }
    for (uint32_t i = k * 16; i < n; i++) {
        s1 += p[i];
        s2 += s1;
    }
    *a = s1;
    *b = s2;
}

#if GMV_DEV
#define VS_EVENTS 300
#endif
static void vs_event(const char *what, const uint8_t *b, int site)
{
#if GMV_DEV
    static volatile LONG n;
    if (!gmv_perf_on() || InterlockedIncrement(&n) > VS_EVENTS)
        return;
    uint32_t data, size, name;
    memcpy(&data, b + 8, 4);
    memcpy(&size, b + 0xc, 4);
    memcpy(&name, b + 0x18, 4);
    gmv_log("[vbostatic] %s %p (site 0x%06x, %s, usage %u, flags 0x%02x, data 0x%08x, %u bytes, GL %u)\n", what,
            (const void *)b, site >= 0 ? g_site[site].ret : 0, b[0x10] ? "index" : "vertex", b[0x11], b[0x12], data,
            size, name);
#else
    (void)what, (void)b, (void)site;
#endif
}

static void vs_revert(uint8_t *b)
{
    uint8_t f = b[0x12];
    b[0x11] = 4;
    if ((f & 8) && !(f & 0x20)) {
        uint32_t name;
        memcpy(&name, b + 0x18, 4);
        if (name && GetCurrentThreadId() == g_gl_tid) {
            static void(APIENTRY * del)(GLsizei, const GLuint *);
            if (!del) del = (void(APIENTRY *)(GLsizei, const GLuint *))(void *)gl_real("glDeleteBuffers");
            uint8_t *drv;
            memcpy(&drv, b + 0x14, 4);
            uint32_t *cache = (uint32_t *)(drv + 0x224 + 4 * (b[0x10] & 3));
            if (*cache == name)
                *cache = 0;
            if (del) {
                gmv_gl_buffers_deleted(1, &name);
                del(1, &name);
                memset(b + 0x18, 0, 4);
            } else
                g_tot.leaked++;
        } else if (name)
            g_tot.leaked++;
        b[0x12] = (uint8_t)(f & ~(8 | 2));
    } else
        b[0x12] = (uint8_t)(f & ~2);
}

/* 0x4c6ae0 0x501af1 */
static void vs_ctor(void *obj, void *drv, int type, int usage, unsigned size, void *data, int keep)
{
    /* sub esp,0x38 0x38 0x3c */
    uintptr_t at = (uintptr_t)&obj + 0x3c;
    uint32_t ret = *(const volatile uint32_t *)at;
    ((ctor_fn)(uintptr_t)VS_CTOR)(obj, drv, type, usage, size, data, keep);
    vs_lock();
    int si = -1;
    for (int i = 0; i < g_nsite; i++)
        if (g_site[i].ret == ret) { si = i; break; }
    if (si < 0 && g_nsite < VS_SITES) {
        si = g_nsite++;
        g_site[si].ret = ret;
    }
    if (si >= 0) {
        struct site *s = &g_site[si];
        s->n++;
        s->by_usage[(unsigned)usage < 6 ? usage : 5]++;
        s->by_type[(unsigned)type < 4 ? type : 3]++;
        s->bytes += size;
        int skip = 0;
        for (size_t i = 0; i < sizeof vs_skip / sizeof *vs_skip; i++)
            skip |= ret == vs_skip[i];
        if (g_static && usage == 4 && (type == 0 || type == 1) && !skip && s->n <= VS_SITE_MAX)
            vs_add(obj, si);
    } else
        g_more++;
    vs_unlock();
}

/* 0x517b10 +0x12 */
static void *vs_map(void *buf, int access)
{
    uint8_t *b = buf;
#if GMV_DEV
    InterlockedIncrement(&g_map_n[access & 7][(b[0x12] & 8) ? 1 : 0]);
#endif
    if (g_static) {
        vs_lock();
        int k = vs_find(b);
        if (k >= 0) {
            went *e = &g_w[k];
            if (e->state == W_STATIC) {
                vs_event("put back (map)", b, e->site);
                vs_revert(b);
                e->state = W_NEVER;
                g_tot.rev_map++;
                g_site[e->site].rev_map++;
            } else
                e->quiet = 0;
        }
        vs_unlock();
    }
    return g_map(buf, access);
}

static void vs_bind(void *buf, int drop)
{
    if ((drop & 0xff) && g_static) {
        vs_lock();
        if (vs_find(buf) >= 0) {
            drop = 0;
            g_tot.kept++;
        }
        vs_unlock();
    }
    uint32_t name0;
    memcpy(&name0, (uint8_t *)buf + 0x18, 4);
    g_bind(buf, drop);
    if (g_static) {
        uint32_t name;
        memcpy(&name, (uint8_t *)buf + 0x18, 4);
        if (name && !name0) {
            vs_lock();
            int k = vs_find(buf);
            if (k >= 0)
                vs_event("uploaded", buf, g_w[k].site);
            vs_unlock();
        }
        if (name && name < (1u << 20)) {
            if (name >= g_bynamecap) {
                uint32_t c = g_bynamecap ? g_bynamecap : 4096;
                while (c <= name)
                    c *= 2;
                uint8_t **a = realloc(g_byname, c * sizeof *a);
                if (!a)
                    return;
                memset(a + g_bynamecap, 0, (c - g_bynamecap) * sizeof *a);
                g_byname = a;
                g_bynamecap = c;
            }
            g_byname[name] = buf;
        }
    }
}

const void *gmv_vbostatic_client(unsigned name, uintptr_t off)
{
    if (!g_static || name >= g_bynamecap || !g_byname[name])
        return NULL;
    uint8_t *b = g_byname[name];
    const void *r = NULL;
    vs_lock();
    int k = vs_find(b);
    if (k >= 0 && g_w[k].state == W_STATIC && (b[0x12] & 8)) {
        uint32_t nm, size;
        uint8_t *data;
        memcpy(&nm, b + 0x18, 4);
        memcpy(&data, b + 8, 4);
        memcpy(&size, b + 0xc, 4);
        if (nm == name && data && off < size)
            r = data + off;
    }
    vs_unlock();
    return r;
}

static void vs_setdata(void *buf, unsigned size, void *data, int own)
{
    if (g_static) {
        uint8_t *b = buf;
        vs_lock();
        int k = vs_find(b);
        if (k >= 0) {
            went *e = &g_w[k];
            if (e->state == W_STATIC) {
                vs_event("put back (new data)", b, e->site);
                vs_revert(b);
                e->state = W_NEVER;
                g_tot.rev_set++;
            } else
                e->quiet = 0;
        }
        vs_unlock();
    }
    g_setdata(buf, size, data, own);
}

static void vs_dtor0(void *obj)
{
    vs_lock();
    vs_remove(obj);
    vs_unlock();
    g_dtor0(obj);
}

static void vs_dtor1(void *obj)
{
    vs_lock();
    vs_remove(obj);
    vs_unlock();
    g_dtor1(obj);
}

static uint32_t vs_check(went *e)
{
    uint8_t *b = e->obj, *data;
    uint32_t size;
    memcpy(&data, b + 8, 4);
    memcpy(&size, b + 0xc, 4);
    if (!data || !size || b[0x13])
        return 0;
    uint32_t s1, s2;
    vs_sum(data, size, &s1, &s2);
    int same = s1 == e->s1 && s2 == e->s2;
    e->s1 = s1;
    e->s2 = s2;
    if (e->state == W_STATIC) {
        if (!same) {
            vs_event("put back (changed)", b, e->site);
            vs_revert(b);
            e->state = W_NEVER;
            g_tot.rev_hash++;
            g_site[e->site].rev_hash++;
        }
    } else if (!same)
        e->quiet = 0;
    else if (++e->quiet >= VS_QUIET && b[0x11] == 4 && !(b[0x12] & 8)) {
        b[0x11] = 0;
        b[0x12] |= 2;
        e->state = W_STATIC;
        vs_event("made static", b, e->site);
        g_tot.conv++;
        g_tot.conv_bytes += size;
        g_site[e->site].conv++;
        g_site[e->site].conv_bytes += size;
    }
    return size;
}

static void vs_round(int *cur, int state, uint32_t budget)
{
    uint32_t done = 0;
    for (int visits = 0; g_nw && done < budget && visits < g_nw; visits++) {
        if (*cur >= g_nw)
            *cur = 0;
        went *e = &g_w[(*cur)++];
        if (e->state == state)
            done += vs_check(e);
    }
}

void gmv_vbostatic_frame(void)
{
    if (!g_static)
        return;
    g_gl_tid = GetCurrentThreadId();
    vs_lock();
    vs_round(&g_cursor, W_WATCH, VS_BUDGET);
    vs_round(&g_scursor, W_STATIC, VS_BUDGET_STATIC);
    vs_unlock();
}

void gmv_vbostatic_install(void)
{
    const char *e = gmv_getenv("GMV_STATIC_VBO");
    int want = gmv_env_flag(e, 1);
    if (!want && !gmv_perf_on())
        return;
    const uint32_t *vt = (const uint32_t *)(uintptr_t)VS_VTBL;
    if (code_call_target(VS_CTOR_SITE) != VS_CTOR || vt[0] != 0x5036c0u || vt[1] != 0x5035b0u ||
        vt[3] != VS_BIND || memcmp((const void *)(uintptr_t)VS_MAP, vs_map_pro, sizeof vs_map_pro) ||
        memcmp((const void *)(uintptr_t)VS_SETDATA, vs_set_pro, sizeof vs_set_pro)) {
        gmv_log("[vbostatic] the buffer code is not as expected; skipped\n");
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr)
        return;
    memcpy(tr, vs_map_pro, sizeof vs_map_pro);
    tr[7] = 0xe9;
    int32_t r = (int32_t)(VS_MAP + 7 - (uint32_t)(uintptr_t)(tr + 12));
    memcpy(tr + 8, &r, 4);
    unsigned char *ts = tr + 16;
    memcpy(ts, vs_set_pro, sizeof vs_set_pro);
    ts[6] = 0xe9;
    r = (int32_t)(VS_SETDATA + 6 - (uint32_t)(uintptr_t)(ts + 11));
    memcpy(ts + 7, &r, 4);
    FlushInstructionCache(GetCurrentProcess(), tr, 4096);
    g_map = (map_fn)(void *)tr;
    g_setdata = (setdata_fn)(void *)ts;
    g_dtor0 = (dtor_fn)(uintptr_t)vt[0];
    g_dtor1 = (dtor_fn)(uintptr_t)vt[1];
    g_bind = (bind_fn)(uintptr_t)vt[3];
    uint32_t bnd = (uint32_t)(uintptr_t)vs_bind;
    uint32_t d[2] = {(uint32_t)(uintptr_t)vs_dtor0, (uint32_t)(uintptr_t)vs_dtor1};
    if (!code_write(VS_VTBL, d, sizeof d) || !code_write(VS_VTBL + 12, &bnd, 4) ||
        !code_jmp(VS_MAP, (void *)vs_map) || !code_jmp(VS_SETDATA, (void *)vs_setdata) ||
        !code_call(VS_CTOR_SITE, (void *)vs_ctor)) {
        gmv_log("[vbostatic] patch failed (%lu)\n", GetLastError());
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    g_static = want;
    gmv_log("[vbostatic] %s\n", g_static ? "STATIC_VBO=1: the game's models that stay unchanged are kept in GL buffer "
                                           "objects (GL_STATIC_DRAW)" : "STATIC_VBO=0: only counting the game's buffers");
}

#if GMV_DEV
void gmv_vbostatic_report(double seconds)
{
    if (seconds <= 0 || !g_map)
        return;
    struct site s[VS_SITES];
    int n, nw, nstatic = 0;
    vs_lock();
    n = g_nsite;
    memcpy(s, g_site, sizeof s);
    nw = g_nw;
    for (int k = 0; k < g_nw; k++)
        nstatic += g_w[k].state == W_STATIC;
    __typeof__(g_tot) tot = g_tot;
    vs_unlock();
    static int last_n = -1;
    static unsigned last_total, last_rev;
    unsigned total = 0;
    for (int i = 0; i < n; i++)
        total += s[i].n;
    if (n != last_n || total != last_total || tot.rev_map + tot.rev_hash != last_rev) {
        last_n = n;
        last_total = total;
        last_rev = tot.rev_map + tot.rev_hash;
        gmv_log("[perf]   glitch buffers so far: %u from %d sites%s\n", total, n, g_more ? " (more not listed)" : "");
        for (int i = 0; i < n; i++)
            gmv_log("[perf]     site 0x%06x: %u (vertex %u, index %u, other %u), usage 0/1/2/4 = %u/%u/%u/%u, "
                    "%.1f MB; made static %u (%.1f MB), put back: mapped %u, changed %u\n", s[i].ret, s[i].n,
                    s[i].by_type[0], s[i].by_type[1], s[i].by_type[2] + s[i].by_type[3], s[i].by_usage[0],
                    s[i].by_usage[1], s[i].by_usage[2], s[i].by_usage[4], (double)s[i].bytes / 1048576.0, s[i].conv,
                    (double)s[i].conv_bytes / 1048576.0, s[i].rev_map, s[i].rev_hash);
    }
    if (g_static)
        gmv_log("[perf]   static buffers: %d watched, %d static now; so far made static %u (%.1f MB), put back "
                "mapped %u / new data %u / changed without either %u, GL buffers left %u, destroyed %u; data kept at "
                "bind %u\n", nw, nstatic, tot.conv, (double)tot.conv_bytes / 1048576.0, tot.rev_map, tot.rev_set,
                tot.rev_hash, tot.leaked, tot.gone, tot.kept);
    LONG m[8][2];
    for (int a = 0; a < 8; a++)
        for (int g = 0; g < 2; g++)
            m[a][g] = InterlockedExchange(&g_map_n[a][g], 0);
    gmv_log("[perf]   buffer maps a second by access 0-5 (client / GL): %.0f/%.0f %.0f/%.0f %.0f/%.0f %.0f/%.0f "
            "%.0f/%.0f %.0f/%.0f\n", m[0][0] / seconds, m[0][1] / seconds, m[1][0] / seconds, m[1][1] / seconds,
            m[2][0] / seconds, m[2][1] / seconds, m[3][0] / seconds, m[3][1] / seconds, m[4][0] / seconds,
            m[4][1] / seconds, m[5][0] / seconds, m[5][1] / seconds);
}

void gmv_vbostatic_fault(void)
{
    if (!g_static)
        return;
    gmv_log("        [vbostatic] %d watched; static ones:\n", g_nw);
    int shown = 0;
    uint8_t *drv = NULL;
    for (int k = 0; k < g_nw && shown < 40; k++) {
        if (g_w[k].state != W_STATIC)
            continue;
        const uint8_t *b = g_w[k].obj;
        if (IsBadReadPtr(b, 0x1c)) {
            gmv_log("        [vbostatic]   %p unreadable\n", (const void *)b);
            continue;
        }
        uint32_t data, size, name;
        memcpy(&data, b + 8, 4);
        memcpy(&size, b + 0xc, 4);
        memcpy(&name, b + 0x18, 4);
        memcpy(&drv, b + 0x14, 4);
        gmv_log("        [vbostatic]   %p site 0x%06x %s usage %u flags 0x%02x map %u data 0x%08x..0x%08x GL %u%s\n",
                (const void *)b, g_site[g_w[k].site].ret, b[0x10] ? "index" : "vertex", b[0x11], b[0x12], b[0x13],
                data, data + size, name, IsBadReadPtr((const void *)(uintptr_t)data, size ? size : 1) ? " (data unreadable)" : "");
        shown++;
    }
    if (drv && !IsBadReadPtr(drv + 0x224, 8)) {
        uint32_t c[2];
        memcpy(c, drv + 0x224, 8);
        gmv_log("        [vbostatic] glitch's bound buffers: array %u, element %u\n", c[0], c[1]);
    }
    if (GetCurrentThreadId() != g_gl_tid || !wglGetCurrentContext())
        return;
    GLint ab = 0, eb = 0, vb = 0, cb = 0, tb = 0, nb = 0, ct = 0;
    glGetIntegerv(0x8894, &ab);
    glGetIntegerv(0x8895, &eb);
    glGetIntegerv(0x8896, &vb);
    glGetIntegerv(0x8897, &nb);
    glGetIntegerv(0x8898, &cb);
    glGetIntegerv(0x84E1, &ct);
    glGetIntegerv(0x889A, &tb);
    void *vp = NULL, *cp = NULL, *tp = NULL, *np = NULL;
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &vp);
    glGetPointerv(GL_COLOR_ARRAY_POINTER, &cp);
    glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &tp);
    glGetPointerv(GL_NORMAL_ARRAY_POINTER, &np);
    gmv_log("        [vbostatic] GL: array buffer %d, element buffer %d; vertex %s buf %d ptr %p, colour %s buf %d ptr %p, "
            "normal %s buf %d ptr %p, texcoord (unit 0x%x) %s buf %d ptr %p\n", ab, eb,
            glIsEnabled(GL_VERTEX_ARRAY) ? "on" : "off", vb, vp, glIsEnabled(GL_COLOR_ARRAY) ? "on" : "off", cb, cp,
            glIsEnabled(GL_NORMAL_ARRAY) ? "on" : "off", nb, np, ct, glIsEnabled(GL_TEXTURE_COORD_ARRAY) ? "on" : "off",
            tb, tp);
}
#endif
