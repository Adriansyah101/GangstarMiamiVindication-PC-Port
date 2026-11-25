#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "addon_io.h"

#define UC_META_PACK  "file001674.dat"
#define UC_RES_PACK   "file000482.dat"
#define UC_META_MAC   255
#define UC_SLOT       100
#define UC_RES_MAC    1728
#define UC_BANK_PACK  "file003085.dat"
#define UC_DESC_PACK  "file002894.dat"
#define UC_SETS_PACK  "file000899.dat"
#define UC_INT_PACK   "file002229.dat"
#define UC_INT_NAME   "car_update_interior_d.tga"
#define UC_DESC_MAC   388
#define UC_DESC_SZ    21
#define UC_MT_AT      219
#define UC_MT_N       14
#define UC_SETS_MAC   44
#define UC_SLOTS      172
#define UC_SLOTS_UC   174
#define UC_EXTRA      33
#define UC_MT_SET     13
#define UC_PICKUP_SET 12

static const char *const uc_anim_files[] = { "anim_ingame.bdae", "anim_ingame.cst.desc.gmap", "AnimationSets.gmap" };
static const char *const uc_anim_packs[] = { UC_BANK_PACK, UC_DESC_PACK, UC_SETS_PACK };

static const char *const uc_files[] = {
    "car_monster.bdae", "car_monster_lo.bdae", "anim_cardoors_monster_left.bdae",
    "anim_cardoors_monster_right.bdae", "car_update_texture_d.tga", "car_update_texture_damage_d.tga",
    "sfx_monster_idle.wav", "sfx_monster_offhigh.wav", "sfx_monster_offlow.wav", "sfx_monster_offmid.wav",
    "sfx_monster_offverylow.wav", "sfx_monster_onhigh.wav", "sfx_monster_onlow.wav", "sfx_monster_onmid.wav",
    "sfx_monster_onverylow.wav", "sfx_monster_startup.wav",
};
#define UC_NFILES (sizeof uc_files / sizeof *uc_files)
#define UC_NSOUNDS 10                       /* 0x118 */

static const char *const uc_opt_files[] = { "p_smoke_02.bdae" };
#define UC_NOPT (sizeof uc_opt_files / sizeof *uc_opt_files)

static char uc_dir[MAX_PATH * 2], uc_cache[MAX_PATH * 2];
static int uc_ready;
static int uc_opt_have[UC_NOPT];
static volatile LONG uc_type = -1, uc_res_base = -1, uc_pick = -1;
static CRITICAL_SECTION uc_cs;

int gmv_uc_on(void)
{
    const char *e = gmv_config_live("GMV_UC_CRUSHER");
    return gmv_env_on(e);
}

#define UC_PAK_VER 1

static unsigned char uc_mask(uint32_t pos)
{
    uint32_t x = (pos * 2654435761u) ^ 0x5a17c3e9u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (unsigned char)x;
}

static uint32_t fnv1a(const unsigned char *d, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *d++) * 16777619u;
    return h;
}

static int pak_same(const char *pak, const char *stamp)
{
    unsigned char h[16];
    char want[64], have[64] = "";
    FILE *f = fopen(pak, "rb");
    if (!f)
        return 0;
    size_t r = fread(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    if (r != sizeof h || memcmp(h, "GMVP", 4) || rd32(h + 4) != UC_PAK_VER)
        return 0;
    for (size_t i = 12; i < 16; i++)
        h[i] ^= uc_mask((uint32_t)i);
    snprintf(want, sizeof want, "%u %08x", (unsigned)n, rd32(h + 12));
    if (!(f = fopen(stamp, "rb")))
        return 0;
    r = fread(have, 1, sizeof have - 1, f);
    have[r] = 0;
    fclose(f);
    return !strcmp(have, want);
}

int gmv_pak_unpack(const char *pak, const char *dir, const char *tag)
{
    char stamp[MAX_PATH * 3], want[64], have[64] = "";
    snprintf(stamp, sizeof stamp, "%s" GMV_SEP "pak.stamp", dir);
    if (pak_same(pak, stamp))
        return 1;
    size_t n = 0;
    unsigned char *d = read_all(pak, &n, 64);
    int ok = 0;
    if (!d || n < 16 || memcmp(d, "GMVP", 4) || rd32(d + 4) != UC_PAK_VER) {
        gmv_log("[%s] %s is not an add-on pack (version %d)\n", tag, pak, UC_PAK_VER);
        goto done;
    }
    for (size_t i = 8; i < n; i++)
        d[i] ^= uc_mask((uint32_t)i);
    uint32_t cnt = rd32(d + 8), sum = rd32(d + 12);
    size_t o = 16, data = 0;
    for (uint32_t k = 0; k < cnt; k++) {
        if (o >= n || o + 1 + d[o] + 4 > n)
            break;
        data += rd32(d + o + 1 + d[o]);
        o += 1 + d[o] + 4;
        if (k + 1 == cnt)
            ok = 1;
    }
    if (!ok || o + data != n || fnv1a(d + o, data) != sum) {
        ok = 0;
        gmv_log("[%s] %s is damaged\n", tag, pak);
        goto done;
    }
    snprintf(want, sizeof want, "%u %08x", (unsigned)n, sum);
    FILE *f = fopen(stamp, "rb");
    if (f) {
        size_t r = fread(have, 1, sizeof have - 1, f);
        have[r] = 0;
        fclose(f);
    }
    if (!strcmp(have, want))
        goto done;
    CreateDirectoryA(dir, NULL);
    DeleteFileA(stamp);
    size_t ix = 16, at = o;
    for (uint32_t k = 0; k < cnt && ok; k++) {
        char name[256], p[MAX_PATH * 3];
        unsigned l = d[ix];
        memcpy(name, d + ix + 1, l);
        name[l] = 0;
        uint32_t sz = rd32(d + ix + 1 + l);
        ix += 1 + l + 4;
        if (strpbrk(name, "\\/:") || !strcmp(name, ".") || !strcmp(name, "..") || !*name) {
            gmv_log("[%s] %s holds a bad name\n", tag, pak);
            ok = 0;
            break;
        }
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", dir, name);
        if (!write_all(p, d + at, sz)) {
            gmv_log("[%s] could not write %s\n", tag, p);
            ok = 0;
        }
        at += sz;
    }
    if (ok && (!(f = fopen(stamp, "wb")) || fputs(want, f) < 0 || fclose(f)))
        gmv_log("[%s] could not write %s (the pack is unpacked again next time)\n", tag, stamp);
    if (ok)
        gmv_log("[%s] unpacked %s (%u files) to %s\n", tag, pak, cnt, dir);
done:
    free(d);
    return ok;
}

int gmv_uc_init(void)
{
    char exe[MAX_PATH], pak[MAX_PATH * 2];
    int want = gmv_uc_on();
    InitializeCriticalSection(&uc_cs);
    if (!GetModuleFileNameA(NULL, exe, sizeof exe))
        return 0;
    *strrchr(exe, GMV_DIRSEP) = 0;
    snprintf(uc_cache, sizeof uc_cache, "%s" GMV_SEP "cache" GMV_SEP "uc", exe);
    snprintf(pak, sizeof pak, "%s" GMV_SEP "add-on" GMV_SEP "crusher.pak", exe);
    if (GetFileAttributesA(pak) != INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryA(uc_cache, NULL);
        snprintf(uc_dir, sizeof uc_dir, "%s" GMV_SEP "cache" GMV_SEP "uc" GMV_SEP "crusher", exe);
        if (!gmv_pak_unpack(pak, uc_dir, "crusher"))
            return 0;
    } else
        snprintf(uc_dir, sizeof uc_dir, "%s" GMV_SEP "add-on" GMV_SEP "crusher", exe);
    int missing = 0;
    for (size_t i = 0; i <= UC_NFILES; i++) {
        char p[MAX_PATH * 3];
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", uc_dir, i < UC_NFILES ? uc_files[i] : "metatypes.gmap");
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
            if (want)
                gmv_log("[crusher] missing %s\n", p);
            missing++;
        }
    }
    uc_ready = !missing;
    if (uc_ready)
        CreateDirectoryA(uc_cache, NULL);
    for (size_t i = 0; i < UC_NOPT; i++) {
        char p[MAX_PATH * 3];
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", uc_dir, uc_opt_files[i]);
        uc_opt_have[i] = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    }
    if (uc_ready && !uc_opt_have[0])
        gmv_log("[crusher] no %s in the add-on: the truck has no exhaust smoke\n", uc_opt_files[0]);
    if (uc_ready)
        gmv_log("[crusher] add-on found: %s\n", uc_dir);
    else if (want)
        gmv_log("[crusher] UC_CRUSHER=1 but %s is incomplete (%d files missing); the Crusher is left out\n",
                uc_dir, missing);
    return uc_ready;
}

int gmv_uc_type(void) { return (int)uc_type; }
int gmv_uc_res_base(void) { return (int)uc_res_base; }
int gmv_uc_pick_type(void) { return (int)uc_pick; }
int gmv_uc_has_smoke(void) { return uc_ready && uc_opt_have[0]; }

/* Structs::Vehicle::Read 0x2357a */
typedef struct { size_t b14, v44, v54, arr, vbc; uint32_t narr; } vfields;

static int vwalk(const unsigned char *e, size_t n, vfields *f)
{
    size_t o = 0;
#define NEED(k) do { if (o + (k) > n) return 0; } while (0)
#define STR() do { NEED(4); uint32_t l_ = rd32(e + o); o += 4; NEED(l_); o += l_; } while (0)
    NEED(8);
    if (rd32(e) != 3)
        return 0;
    o = 4;
    STR();
    NEED(16); f->b14 = o + 12; o += 16;
    STR(); STR();                            /* +0x1c */
    NEED(32); o += 32;
    NEED(4); o += 4;
    f->v44 = o; NEED(16); o += 16;
    f->v54 = o; NEED(16); o += 16;
    NEED(4); f->narr = rd32(e + o); o += 4;
    if (f->narr > 64) return 0;
    f->arr = o; NEED(4 * f->narr); o += 4 * f->narr;
    NEED(32); o += 32;
    STR();
    NEED(9); o += 9;
    NEED(28); o += 28;
    STR();
    f->vbc = o; NEED(4); o += 4;
    NEED(44); o += 44;
#undef STR
#undef NEED
    return o == n;
}

static const unsigned char *find_vehicle(const unsigned char *d, size_t n, const char *name, size_t *len,
                                         int *type)
{
    size_t nl = strlen(name), o = 4;
    for (uint32_t k = 0, cnt = rd32(d); k < cnt && o + 4 <= n; k++) {
        uint32_t sz = rd32(d + o);
        const unsigned char *b = d + o + 4;
        if (o + 4 + sz > n)
            return NULL;
        if (sz >= 8 + nl && rd32(b) == 3 && rd32(b + 4) == nl && !memcmp(b + 8, name, nl)) {
            *len = sz;
            if (type)
                *type = (int)k;
            return b;
        }
        o += 4 + sz;
    }
    return NULL;
}

static int32_t uc_anim_of(int32_t v) { return v >= UC_MT_AT ? v + UC_MT_N : v; }

static int32_t mac_anim_of(int32_t v)
{
    return v < UC_MT_AT ? v : v < UC_MT_AT + UC_MT_N ? UC_DESC_MAC + (v - UC_MT_AT) : v - UC_MT_N;
}

/* BRES +0x0c +0x10 0x3c +0x1c +0x20 +0x28 +0x38 */

static int memmem_s(const char *s, uint32_t n, const char *w)
{
    size_t wl = strlen(w);
    for (uint32_t i = 0; i + wl <= n; i++)
        if (!memcmp(s + i, w, wl))
            return 1;
    return 0;
}

typedef struct {
    unsigned char *d;
    size_t n;
    uint32_t *slot, nslot;
    uint32_t *seg, nseg;
} bres_t;

static void bres_insert(bres_t *b, uint32_t pos, const void *src, uint32_t len)
{
    for (uint32_t i = 0; i < b->nslot; i++) {
        uint32_t v = rd32(b->d + b->slot[i]);
        if (v >= pos)
            wr32(b->d + b->slot[i], v + len);
    }
    for (uint32_t i = 0; i < b->nseg; i++) {
        uint32_t v = rd32(b->d + b->seg[i] + 12);
        if (v >= pos)
            wr32(b->d + b->seg[i] + 12, v + len);
    }
    memmove(b->d + pos + len, b->d + pos, b->n - pos);
    if (src)
        memcpy(b->d + pos, src, len);
    else
        memset(b->d + pos, 0, len);
    b->n += len;
    for (uint32_t i = 0; i < b->nslot; i++)
        if (b->slot[i] >= pos)
            b->slot[i] += len;
    for (uint32_t i = 0; i < b->nseg; i++)
        if (b->seg[i] >= pos)
            b->seg[i] += len;
}

typedef struct { uint32_t root, nch, ch, nclip, clips, segs, nseg, seg0; } bres_info;

static int bres_parse(const unsigned char *d, size_t n, bres_info *i)
{
#define INB(o, k) ((uint64_t)(o) + (k) <= n)
    if (n < 0x40 || memcmp(d, "BRES", 4) || rd32(d + 8) != 0x3c || rd32(d + 0x0c) != n ||
        !INB(0x3c, 4ull * rd32(d + 0x10)) || !INB(i->root = rd32(d + 0x20), 0x3c))
        return 0;
    i->nch = rd32(d + i->root + 0x24), i->ch = rd32(d + i->root + 0x28);
    i->segs = rd32(d + i->root + 0x30);
    i->nclip = rd32(d + i->root + 0x34), i->clips = rd32(d + i->root + 0x38);
    if (!INB(i->ch, 0x20ull * i->nch) || !INB(i->clips, 12ull * i->nclip) || !INB(i->segs, 8))
        return 0;
    i->nseg = rd32(d + i->segs), i->seg0 = rd32(d + i->segs + 4);
    if (!i->nseg || !INB(i->seg0, 0x18ull * i->nseg))
        return 0;
    for (uint32_t k = 0; k < i->nseg; k++) {
        const unsigned char *s = d + i->seg0 + 0x18 * k;
        if (!INB(rd32(s + 12), rd32(s + 16)))
            return 0;
    }
    return 1;
#undef INB
}

static const char *bres_str(const unsigned char *d, size_t n, uint32_t p, uint32_t *len)
{
    if (p < 4 || p > n || (*len = rd32(d + p - 4)) > n - p)
        return NULL;
    return (const char *)d + p;
}

static int bres_same_str(const unsigned char *a, size_t an, uint32_t pa, const unsigned char *b, size_t bn, uint32_t pb)
{
    uint32_t la, lb;
    const char *sa = bres_str(a, an, pa, &la), *sb = bres_str(b, bn, pb, &lb);
    return sa && sb && la == lb && !memcmp(sa, sb, la);
}

static int seg_shift(unsigned char *s, uint32_t size, uint32_t nch, uint32_t frames, int write)
{
    if (size < 4 || rd32(s) != 2 * nch || 4 + 16ull * nch > size)
        return 0;
    for (uint32_t k = 0; k < 2 * nch; k += 2) {
        unsigned char *p = s + 4 + 8 * k;
        uint32_t cnt = rd32(p);
        uint64_t at = (uint64_t)(p - s) + 4 + rd32(p + 4);
        if (at + 2ull * cnt > size)
            return 0;
        unsigned prev = 0;
        for (uint32_t j = 0; j < cnt; j++) {
            unsigned t = s[at + 2 * j] | s[at + 2 * j + 1] << 8;
            if (t < prev || t + frames > 0xffff)
                return 0;
            prev = t;
            if (write)
                s[at + 2 * j] = (unsigned char)(t + frames), s[at + 2 * j + 1] = (unsigned char)((t + frames) >> 8);
        }
    }
    return 1;
}

static unsigned char *merge_bank(const unsigned char *m, size_t mn, const unsigned char *u, size_t un, uint32_t at,
                                 size_t *on)
{
    bres_info M, U;
    if (!bres_parse(m, mn, &M) || !bres_parse(u, un, &U) || M.nclip + UC_MT_N != U.nclip || M.nch != U.nch ||
        !at || at > M.nclip)
        return NULL;
    for (uint32_t k = 0; k < M.nch; k++)
        if (!bres_same_str(m, mn, rd32(m + M.ch + 0x20 * k), u, un, rd32(u + U.ch + 0x20 * k)))
            return NULL;
    for (uint32_t k = 0; k < M.nclip; k++)
        if (!bres_same_str(m, mn, rd32(m + M.clips + 12 * k), u, un,
                           rd32(u + U.clips + 12 * (k < at ? k : k + UC_MT_N))))
            return NULL;
    uint32_t lo = 0xffffffffu, hi = 0, slen = 0;
    for (uint32_t k = at; k < at + UC_MT_N; k++) {
        const unsigned char *c = u + U.clips + 12 * k;
        uint32_t l;
        const char *s = bres_str(u, un, rd32(c), &l);
        if (!s || l < 12 || !memmem_s(s, l, "monstertruck"))
            return NULL;
        slen += (4 + l + 1 + 3) & ~3u;
        if (rd32(c + 4) < lo) lo = rd32(c + 4);
        if (rd32(c + 8) > hi) hi = rd32(c + 8);
    }
    uint32_t first = 0, ncp = 0, blobs = 0;
    for (uint32_t k = 0; k < U.nseg; k++) {
        const unsigned char *s = u + U.seg0 + 0x18 * k;
        if (rd32(s) < hi && rd32(s + 4) > lo) {
            if (!ncp)
                first = k;
            else if (k != first + ncp)
                return NULL;
            ncp++, blobs += rd32(s + 16);
        }
    }
    const unsigned char *mlast = m + M.seg0 + 0x18 * (M.nseg - 1), *ufirst = u + U.seg0 + 0x18 * first;
    uint32_t mend = rd32(mlast + 4);
    if (!ncp || ncp > 64 || rd32(ufirst) > mend)
        return NULL;
    uint32_t frames = (uint32_t)(((uint64_t)(mend + 1000 - rd32(ufirst)) * 3 + 99) / 100);
    frames += (3 - frames % 3) % 3;
    uint32_t off_ms = frames / 3 * 100;
    for (uint32_t k = 0; k < ncp; k++) {
        const unsigned char *s = ufirst + 0x18 * k;
        if (!seg_shift((unsigned char *)u + rd32(s + 12), rd32(s + 16), U.nch, frames, 0))
            return NULL;
    }

    bres_t b = { 0 };
    uint32_t nslot = rd32(m + 0x10);
    size_t cap = mn + blobs + 12 * UC_MT_N + 0x18 * ncp + slen + 4 * UC_MT_N + 64;
    b.d = malloc(cap);
    b.slot = malloc(4 * (nslot + UC_MT_N));
    b.seg = malloc(4 * (M.nseg + ncp));
    if (!b.d || !b.slot || !b.seg)
        goto fail;
    memcpy(b.d, m, mn);
    b.n = mn;
    for (uint32_t k = 0; k < nslot; k++)
        if ((b.slot[k] = rd32(m + 0x3c + 4 * k)) > mn - 4)
            goto fail;
    b.nslot = nslot;
    for (uint32_t k = 0; k < M.nseg; k++)
        b.seg[k] = M.seg0 + 0x18 * k;
    b.nseg = M.nseg;

    uint32_t cpos = M.clips + 12 * at, before = 0xffffffffu;
    for (uint32_t k = 0; k < b.nslot; k++)
        if (b.slot[k] == cpos - 12)
            before = k;
    if (before == 0xffffffffu)
        goto fail;
    bres_insert(&b, cpos, NULL, 12 * UC_MT_N);
    memmove(b.slot + before + 1 + UC_MT_N, b.slot + before + 1, 4 * (b.nslot - before - 1));
    for (uint32_t k = 0; k < UC_MT_N; k++) {
        const unsigned char *c = u + U.clips + 12 * (at + k);
        b.slot[before + 1 + k] = cpos + 12 * k;
        wr32(b.d + cpos + 12 * k + 4, rd32(c + 4) + off_ms);
        wr32(b.d + cpos + 12 * k + 8, rd32(c + 8) + off_ms);
    }
    b.nslot += UC_MT_N;
    uint32_t root = rd32(b.d + 0x20);
    wr32(b.d + root + 0x34, M.nclip + UC_MT_N);

    uint32_t segs = rd32(b.d + root + 0x30), spos = rd32(b.d + segs + 4) + 0x18 * M.nseg;
    bres_insert(&b, spos, NULL, 0x18 * ncp);
    for (uint32_t k = 0; k < ncp; k++) {
        const unsigned char *s = ufirst + 0x18 * k;
        unsigned char *e = b.d + spos + 0x18 * k;
        wr32(e, rd32(s) + off_ms), wr32(e + 4, rd32(s + 4) + off_ms), wr32(e + 16, rd32(s + 16));
        b.seg[b.nseg++] = spos + 0x18 * k;
    }
    wr32(b.d + segs, M.nseg + ncp);
    wr32(b.d + b.seg[M.nseg - 1] + 4, rd32(ufirst) + off_ms);

    unsigned char *sb = calloc(1, slen + 4);
    if (!sb)
        goto fail;
    uint32_t spos2 = rd32(b.d + 0x20), so = 0, name[UC_MT_N];
    for (uint32_t k = 0; k < UC_MT_N; k++) {
        uint32_t l = 0;
        const char *s = bres_str(u, un, rd32(u + U.clips + 12 * (at + k)), &l);
        wr32(sb + so, l);
        memcpy(sb + so + 4, s, l);
        name[k] = spos2 + so + 4;
        so += (4 + l + 1 + 3) & ~3u;
    }
    bres_insert(&b, spos2, sb, so);
    free(sb);
    for (uint32_t k = 0; k < UC_MT_N; k++)
        wr32(b.d + b.slot[before + 1 + k], name[k]);
    root = rd32(b.d + 0x20);
    wr32(b.d + root + 0x20, rd32(ufirst + 0x18 * (ncp - 1) + 4) + off_ms);

    bres_insert(&b, 0x3c + 4 * nslot, NULL, 4 * UC_MT_N);
    for (uint32_t k = 0; k < b.nslot; k++)
        wr32(b.d + 0x3c + 4 * k, b.slot[k]);
    wr32(b.d + 0x10, b.nslot);

    for (uint32_t k = 0; k < ncp; k++) {
        const unsigned char *s = ufirst + 0x18 * k;
        memcpy(b.d + b.n, u + rd32(s + 12), rd32(s + 16));
        seg_shift(b.d + b.n, rd32(s + 16), U.nch, frames, 1);
        wr32(b.d + b.seg[M.nseg + k] + 12, (uint32_t)b.n);
        b.n += rd32(s + 16);
    }
    wr32(b.d + 0x0c, (uint32_t)b.n);
    wr32(b.d + 0x38, (uint32_t)b.n - rd32(b.d + 0x28));
    free(b.slot), free(b.seg);
    gmv_log("[crusher] the Mac's animation bank with Urban Crime's monster-truck clips (%u segments, moved %u ms)\n",
            ncp, off_ms);
    *on = b.n;
    return b.d;
fail:
    free(b.d), free(b.slot), free(b.seg);
    return NULL;
}

static int build_anims(const char *any)
{
    static int state;
    if (state)
        return state > 0;
    state = -1;
    char pack[MAX_PATH * 2], p[MAX_PATH * 3];
    snprintf(pack, sizeof pack, "%s", any);
    char *sl = strrchr(pack, '\\'), *sl2 = strrchr(pack, '/');
    if (sl2 > sl)
        sl = sl2;
    if (!sl)
        return 0;
    *sl = 0;
    unsigned char *d[3] = { 0 }, *m[3] = { 0 }, *out[3] = { 0 };
    size_t dn[3] = { 0 }, mn[3] = { 0 }, on[3] = { 0 };
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", uc_dir, uc_anim_files[i]);
        if (!(d[i] = read_all(p, &dn[i], 64))) {
            gmv_log("[crusher] no %s in the add-on: the Crusher enters as the Pickup does\n", uc_anim_files[i]);
            goto done;
        }
    }
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", pack, uc_anim_packs[i]);
        if (!(m[i] = read_all(p, &mn[i], 64)) || mn[i] < 4) {
            gmv_log("[crusher] could not read %s\n", p);
            goto done;
        }
        unhide(m[i]);
    }
    const uint32_t mt_desc = UC_DESC_MAC + UC_MT_N, mt_sets = UC_SETS_MAC + 1;
    const size_t set_mac = 4 * UC_SLOTS, set_uc = 4 * UC_SLOTS_UC;
    if (rd32(m[1]) != UC_DESC_MAC || mn[1] != 4 + UC_DESC_MAC * UC_DESC_SZ ||
        rd32(d[1]) != mt_desc || dn[1] != 4 + mt_desc * UC_DESC_SZ ||
        rd32(m[2]) != UC_SETS_MAC || mn[2] != 4 + UC_SETS_MAC * set_mac ||
        rd32(d[2]) != mt_sets || dn[2] != 4 + mt_sets * set_uc) {
        gmv_log("[crusher] the animation files are not the Mac's and Urban Crime's as known; the Crusher enters as the Pickup does\n");
        goto done;
    }
    const unsigned char *md = m[1] + 4, *ud = d[1] + 4;
    int32_t mt_clip = (int32_t)rd32(ud + UC_MT_AT * UC_DESC_SZ);
    for (int i = 0; i < UC_DESC_MAC; i++) {
        const unsigned char *a = md + i * UC_DESC_SZ, *b = ud + uc_anim_of(i) * UC_DESC_SZ;
        int32_t ca = (int32_t)rd32(a), cb = (int32_t)rd32(b);
        if (memcmp(a + 4, b + 4, UC_DESC_SZ - 4) || cb != (ca >= mt_clip ? ca + UC_MT_N : ca)) {
            gmv_log("[crusher] animation %d differs between the Mac and Urban Crime; the Crusher enters as the Pickup does\n", i);
            goto done;
        }
    }
    for (int k = 0; k < UC_SETS_MAC; k++) {
        const unsigned char *a = m[2] + 4 + k * set_mac, *b = d[2] + 4 + (k < UC_MT_SET ? k : k + 1) * set_uc;
        for (int s = 0, t = 0; s < UC_SLOTS; s++, t++) {
            if (t == UC_EXTRA)
                t += 2;
            int32_t va = (int32_t)rd32(a + 4 * s), vb = (int32_t)rd32(b + 4 * t);
            if (vb != (s && va >= 0 ? uc_anim_of(va) : va)) {
                gmv_log("[crusher] animation set %d, entry %d differs between the Mac and Urban Crime; the Crusher enters as the Pickup does\n",
                        k, s);
                goto done;
            }
        }
    }
    on[1] = 4 + mt_desc * UC_DESC_SZ;
    out[1] = malloc(on[1]);
    wr32(out[1], mt_desc);
    for (int i = 0; i < UC_DESC_MAC; i++)
        memcpy(out[1] + 4 + i * UC_DESC_SZ, ud + uc_anim_of(i) * UC_DESC_SZ, UC_DESC_SZ);
    memcpy(out[1] + 4 + UC_DESC_MAC * UC_DESC_SZ, ud + UC_MT_AT * UC_DESC_SZ, UC_MT_N * UC_DESC_SZ);
    on[2] = mn[2] + set_mac;
    out[2] = malloc(on[2]);
    memcpy(out[2], m[2], mn[2]);
    wr32(out[2], mt_sets);
    const unsigned char *b = d[2] + 4 + UC_MT_SET * set_uc;
    unsigned char *e = out[2] + mn[2];
    for (int s = 0, t = 0; s < UC_SLOTS; s++, t++) {
        if (t == UC_EXTRA)
            t += 2;
        int32_t v = (int32_t)rd32(b + 4 * t);
        wr32(e + 4 * s, (uint32_t)(s && v >= 0 ? mac_anim_of(v) : v));
    }
    const unsigned char *pk = m[2] + 4 + UC_PICKUP_SET * set_mac;
    int kinds = 0;
    for (int s = 1; s < UC_SLOTS; s++) {
        int32_t t = (int32_t)rd32(e + 4 * s), pt = (int32_t)rd32(pk + 4 * s);
        if (t < UC_DESC_MAC || t >= UC_DESC_MAC + UC_MT_N || pt < 0 || pt >= UC_DESC_MAC)
            continue;
        unsigned char *k = out[1] + 4 + t * UC_DESC_SZ + 9;
        uint32_t kind = rd32(md + pt * UC_DESC_SZ + 9);
        if (rd32(k) != kind)
            wr32(k, kind), kinds++;
    }
    if (!(out[0] = merge_bank(m[0], mn[0], d[0], dn[0], (uint32_t)mt_clip, &on[0]))) {
        gmv_log("[crusher] the animation banks are not the Mac's and Urban Crime's as known; the Crusher enters as the Pickup does\n");
        goto done;
    }
    ok = 1;
    for (int i = 0; i < 3 && ok; i++) {
        hide(out[i]);
        snprintf(p, sizeof p, "%s" GMV_SEP "%s", uc_cache, uc_anim_packs[i]);
        ok = write_all(p, out[i], on[i]);
    }
    if (ok) {
        state = 1;
        gmv_trace("[crusher] animations %d..%d and set %d are the monster truck's (%d motion kinds taken from the Pickup's)\n",
                UC_DESC_MAC, UC_DESC_MAC + UC_MT_N - 1, UC_SETS_MAC, kinds);
    } else
        gmv_log("[crusher] could not write the animation files to %s\n", uc_cache);
done:
    for (int i = 0; i < 3; i++)
        free(d[i]), free(m[i]), free(out[i]);
    return ok;
}

static int build_metatypes(const char *src, const char *dst)
{
    size_t mn = 0, un = 0, cn, mpn, upn;
    char up[MAX_PATH * 3];
    snprintf(up, sizeof up, "%s" GMV_SEP "metatypes.gmap", uc_dir);
    unsigned char *m = read_all(src, &mn, 64), *u = read_all(up, &un, 64), *out = NULL;
    int ok = 0;
    if (!m || !u || mn < 8 || un < 8) {
        gmv_log("[crusher] could not read %s or %s\n", src, up);
        goto done;
    }
    unhide(m);
    if (rd32(m) != UC_META_MAC) {
        gmv_log("[crusher] %s holds %u types, not the Mac's %u; left as it is\n", src, rd32(m), UC_META_MAC);
        goto done;
    }
    int mpick_type = -1;
    const unsigned char *crusher = find_vehicle(u, un, "Crusher", &cn, NULL);
    const unsigned char *upick = find_vehicle(u, un, "Pickup", &upn, NULL);
    const unsigned char *mpick = find_vehicle(m, mn, "Pickup", &mpn, &mpick_type);
    vfields fc, fu, fm;
    if (!crusher || !upick || !mpick || !vwalk(crusher, cn, &fc) || !vwalk(upick, upn, &fu) ||
        !vwalk(mpick, mpn, &fm) || fc.narr != fm.narr) {
        gmv_log("[crusher] the Crusher's or a Pickup's entry is not as expected; left out\n");
        goto done;
    }
    if (rd32(crusher + fc.v54) != rd32(upick + fu.v54) || rd32(crusher + fc.b14) != rd32(upick + fu.b14) ||
        memcmp(crusher + fc.arr, upick + fu.arr, 4 * fc.narr))
        gmv_log("[crusher] note: the Crusher's numbered fields differ from Urban Crime's Pickup; the Mac Pickup's are used\n");
    size_t so = 4;
    for (uint32_t k = 0; k < UC_SLOT && so + 4 <= mn; k++)
        so += 4 + rd32(m + so);
    if (so + 8 > mn || rd32(m + so) != 4 || rd32(m + so + 4) != 0xffffffffu) {
        gmv_log("[crusher] type %u is not the Mac's empty slot; left out\n", UC_SLOT);
        goto done;
    }
    out = malloc(mn + cn);
    memcpy(out, m, so);
    wr32(out + so, (uint32_t)cn);
    unsigned char *e = out + so + 4;
    memcpy(e, crusher, cn);
    memcpy(e + cn, m + so + 8, mn - so - 8);
    wr32(e + fc.b14, rd32(mpick + fm.b14));
    wr32(e + fc.v54, rd32(mpick + fm.v54));
    memcpy(e + fc.arr, mpick + fm.arr, 4 * fm.narr);
    uint32_t anims = build_anims(src) ? UC_SETS_MAC : rd32(mpick + fm.v44);
    gmv_trace("[crusher] type %u: enter animations %u -> %u (%s), engine sounds %u\n", UC_SLOT, rd32(e + fc.v44),
            anims, anims == UC_SETS_MAC ? "the monster truck's, as set 44" : "the Pickup's", rd32(e + fc.vbc));
    wr32(e + fc.v44, anims);
    hide(out);
    ok = write_all(dst, out, mn - 4 + cn);
    if (ok) {
        InterlockedExchange(&uc_pick, mpick_type);
        InterlockedExchange(&uc_type, UC_SLOT);
    }
done:
    free(m), free(u), free(out);
    return ok;
}

static int build_res(const char *src, const char *dst)
{
    size_t n = 0;
    unsigned char *d = read_all(src, &n, 64), *out = NULL;
    int ok = 0;
    if (!d || n < 4)
        goto done;
    unhide(d);
    uint32_t cnt = rdbe(d);
    size_t o = 4;
    for (uint32_t k = 0; k < 2 * cnt && o + 4 <= n; k++)
        o += 4 + rdbe(d + o);
    if (cnt != UC_RES_MAC || o != n) {
        gmv_log("[crusher] %s: %u sounds, %u of %u bytes parsed; not the Mac's list, left as it is\n", src, cnt,
                (unsigned)o, (unsigned)n);
        goto done;
    }
    out = malloc(n + UC_NSOUNDS * 128);
    memcpy(out, d, n);
    o = n;
    for (int i = 0; i < UC_NSOUNDS; i++) {
        const char *f = uc_files[UC_NFILES - UC_NSOUNDS + i];
        char id[64];
        size_t l = strlen(f);
        snprintf(id, sizeof id, "%.*s_wav", (int)(l - 4), f);
        wrbe(out + o, (uint32_t)l), memcpy(out + o + 4, f, l), o += 4 + l;
        l = strlen(id);
        wrbe(out + o, (uint32_t)l), memcpy(out + o + 4, id, l), o += 4 + l;
    }
    wrbe(out, cnt + UC_NSOUNDS);
    hide(out);
    ok = write_all(dst, out, o);
    if (ok) {
        InterlockedExchange(&uc_res_base, (LONG)cnt);
        gmv_trace("[crusher] sounds %u..%u: the monster engine\n", cnt, cnt + UC_NSOUNDS - 1);
    }
done:
    free(d), free(out);
    return ok;
}

static int build_interior(const char *path, const char *out)
{
    char src[MAX_PATH * 2];
    snprintf(src, sizeof src, "%.*s%s", (int)(base_name(path) - path), path, UC_INT_PACK);
    size_t n = 0;
    unsigned char *d = read_all(src, &n, 64);
    int ok = 0;
    if (d && n > 128) {
        unhide(d);
        ok = !memcmp(d, "DDS ", 4) && write_all(out, d, n);
    }
    free(d);
    if (ok)
        gmv_log("[crusher] the cab's texture: the Mac's interior (%s)\n", UC_INT_PACK);
    else
        gmv_log("[crusher] could not make the cab's texture from %s: the cab stays pink\n", src);
    return ok;
}

/* 0x53c530 0x50efd0 0x225f2 +0x24 */
const char *gmv_uc_redirect(const char *path, char *buf, size_t n)
{
    if (!uc_ready || !path || !strstr(path, "_packMac"))
        return NULL;
    const char *b = base_name(path);
    static volatile LONG seen;
    for (size_t i = 0; i < UC_NFILES; i++)
        if (!_stricmp(b, uc_files[i])) {
            snprintf(buf, n, "%s" GMV_SEP "%s", uc_dir, uc_files[i]);
            if (!(InterlockedOr(&seen, 1L << i) & (1L << i)))
                gmv_log("[crusher] %s from the add-on\n", uc_files[i]);
            return buf;
        }
    for (size_t i = 0; i < UC_NOPT; i++)
        if (uc_opt_have[i] && !_stricmp(b, uc_opt_files[i])) {
            snprintf(buf, n, "%s" GMV_SEP "%s", uc_dir, uc_opt_files[i]);
            if (!(InterlockedOr(&seen, 1L << (UC_NFILES + i)) & (1L << (UC_NFILES + i))))
                gmv_log("[crusher] %s from the add-on\n", uc_opt_files[i]);
            return buf;
        }
    for (size_t i = 0; i < 3; i++)
        if (!_stricmp(b, uc_anim_packs[i])) {
            EnterCriticalSection(&uc_cs);
            int ok = build_anims(path);
            LeaveCriticalSection(&uc_cs);
            if (!ok)
                return NULL;
            snprintf(buf, n, "%s" GMV_SEP "%s", uc_cache, uc_anim_packs[i]);
            return buf;
        }
    int meta = !_stricmp(b, UC_META_PACK), res = !_stricmp(b, UC_RES_PACK), cab = !_stricmp(b, UC_INT_NAME);
    if (!meta && !res && !cab)
        return NULL;
    snprintf(buf, n, "%s" GMV_SEP "%s", uc_cache, cab ? UC_INT_NAME : b);
    EnterCriticalSection(&uc_cs);
    static int built[3], good[3];
    int k = meta ? 0 : res ? 1 : 2;
    if (!built[k]) {
        built[k] = 1;
        good[k] = meta ? build_metatypes(path, buf) : res ? build_res(path, buf) : build_interior(path, buf);
    }
    LeaveCriticalSection(&uc_cs);
    return good[k] ? buf : NULL;
}
