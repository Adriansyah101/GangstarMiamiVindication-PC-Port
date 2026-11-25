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

/* 0x563c90 0xFF descArr 0x14 0x5640f0 cl 0x563e53 */

#define VTX_SITE   0x00563e29u   /* cmp al,0xff; je 0x5640f0 */
#define VTX_CONT   0x00563e31u
#define VTX_DESTA  0x005640f0u   /* al==0xFF cl */
#define VTX_SKIP   0x00564070u

/* al 0xFF cl */
static unsigned g_vtx_miss_total;
#if GMV_DEV
#define VTX_NSEM 32
static unsigned g_vtx_miss_count[VTX_NSEM];
static int      g_vtx_log_on = -1;

static void hexbytes(char *out, const void *p, int n)
{
    if (!p || IsBadReadPtr(p, (UINT_PTR)n)) { strcpy(out, "<unreadable>"); return; }
    static const char hx[] = "0123456789abcdef";
    const unsigned char *b = (const unsigned char *)p;
    char *o = out;
    for (int i = 0; i < n; i++) {
        if (i) *o++ = ' ';
        *o++ = hx[b[i] >> 4];
        *o++ = hx[b[i] & 15];
    }
    *o = 0;
}

/* 0x14-byte +0x8 +0xa +0xe */
static void shader_attrs(char *out, uint32_t s)
{
    /* [s+0x1c] +4 s+0x34 */
    strcpy(out, "<unreadable>");
    if (!s || IsBadReadPtr((void *)(uintptr_t)s, 0x38))
        return;
    uint32_t arr = *(const uint32_t *)(uintptr_t)(s + 0x1c);
    unsigned n = *(const unsigned char *)(uintptr_t)(s + 0x34);
    if (n > 24)
        n = 24;
    if (n && (!arr || IsBadReadPtr((void *)(uintptr_t)arr, n * 8)))
        return;
    char *o = out;
    *o = 0;
    for (unsigned i = 0; i < n; i++)
        o += sprintf(o, "%s%u", i ? "," : "",
                     (unsigned)*(const unsigned short *)(uintptr_t)(arr + i * 8 + 4));
}

static void dump_shader(const char *tag, uint32_t s)
{
    char vs[32], fs[220], at[128];
    uint32_t key = 0, mask = 0, vp = 0, fp = 0;
    if (s && !IsBadReadPtr((void *)(uintptr_t)s, 0x54)) {
        key  = *(const uint32_t *)(uintptr_t)(s + 0x3c);
        mask = *(const uint32_t *)(uintptr_t)(s + 0x30);
        vp   = *(const uint32_t *)(uintptr_t)(s + 0x4c);
        fp   = *(const uint32_t *)(uintptr_t)(s + 0x50);
    }
    hexbytes(vs, (const void *)(uintptr_t)vp, 4);
    hexbytes(fs, (const void *)(uintptr_t)fp, 0x44);
    shader_attrs(at, s);
    gmv_logfile("          %s shader 0x%08x baker key 0x%08x attribute mask 0x%08x semantics %s\n", tag, s, key, mask, at);
    gmv_logfile("              vertex state   %s\n", vs);
    gmv_logfile("              fragment state %s\n", fs);
}

static void dump_descarr(const char *tag, uint32_t objp)
{
    if (!objp || IsBadReadPtr((void *)(uintptr_t)objp, 4)) {
        gmv_logfile("          %s obj 0x%08x <bad obj ptr>\n", tag, objp);
        return;
    }
    uint32_t base = *(const uint32_t *)(uintptr_t)objp;
    char hx[160];
    hexbytes(hx, (const void *)(uintptr_t)base, 0x14);
    gmv_logfile("          %s obj 0x%08x base 0x%08x  hdr %s\n", tag, objp, base, hx);
    for (int s = 0; s < 6; s++) {
        hexbytes(hx, (const void *)(uintptr_t)(base + 0x14 + (uint32_t)s * 16), 16);
        gmv_logfile("              [slot %d] %s\n", s, hx);
    }
}

#endif

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_vtx_missing(uint32_t sem, uint32_t destslot, uint32_t frameEbp)
{
#if !GMV_DEV
    (void)sem;
    (void)destslot;
    (void)frameEbp;
    unsigned n = ++g_vtx_miss_total;
    if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        gmv_logfile("[vtxmiss] cumulative occurrences so far: %u\n", n);
#else
    if (g_vtx_log_on < 0) {
        g_vtx_log_on = !gmv_env_off(gmv_dbgenv("GMV_VTXLOG"));
    }
    g_vtx_miss_total++;
    if (g_vtx_log_on) {
        unsigned t = g_vtx_miss_total;
        if (t == 1 || t == 10 || t == 100 || t == 1000 || t == 10000 || t == 100000)
            gmv_logfile("[vtxmiss] cumulative occurrences so far: %u\n", t);
    }
    unsigned idx  = (sem < VTX_NSEM) ? sem : (VTX_NSEM - 1);
    unsigned seen = g_vtx_miss_count[idx]++;
    if (!g_vtx_log_on || seen != 0)
        return;

    uint32_t dmap = 0, dobj = 0, smap = 0, sobj = 0;
    const uint32_t *fp = (const uint32_t *)(uintptr_t)frameEbp;
    if (fp && !IsBadReadPtr((void *)fp, 0x20 * sizeof(uint32_t))) {
        dmap = fp[2];                           /* [ebp+0x08] */
        dobj = fp[3];                           /* [ebp+0x0c] */
        smap = fp[6];                           /* [ebp+0x18] */
        sobj = fp[7];                           /* [ebp+0x1c] */
    }
    /* 0xFF 0x14 */
    char dmapsb[96], smapsb[96], ddesc[64];
    hexbytes(dmapsb, (const void *)(uintptr_t)dmap, 22);
    hexbytes(smapsb, (const void *)(uintptr_t)smap, 22);
    strcpy(ddesc, "<n/a>");
    if (dobj && !IsBadReadPtr((void *)(uintptr_t)dobj, 4)) {
        uint32_t base = *(const uint32_t *)(uintptr_t)dobj;
        uint32_t ddp  = base + 0x14 + (destslot & 0xffu) * 16;
        hexbytes(ddesc, (const void *)(uintptr_t)ddp, 16);
    }
    gmv_logfile("[vtxmiss] semantic %-2u dest slot %u present, src slot 0xFF (absent)\n", sem, destslot & 0xffu);
    gmv_logfile("          destMap[0..21] = %s\n", dmapsb);
    gmv_logfile("          srcMap [0..21] = %s\n", smapsb);
    gmv_logfile("          destDesc(16B)  = %s\n", ddesc);
    dump_descarr("destObj READ/input ", dobj);
    dump_descarr("srcObj  WRITE/output", sobj);
    /* 0x4ca6d0 [ebp-0x1c] +0x14 */
    if (fp) {
        uint32_t s = fp[-7], b = 0, first = 0;
        if (s && !IsBadReadPtr((void *)(uintptr_t)s, 0x18))
            b = *(const uint32_t *)(uintptr_t)(s + 0x14);
        if (b && !IsBadReadPtr((void *)(uintptr_t)b, 0xc))
            first = *(const uint32_t *)(uintptr_t)(b + 8);
        gmv_logfile("          baker 0x%08x\n", b);
        dump_shader("this draw's", s);
        dump_shader("baker's    ", first);
    }
#endif
}

#if GMV_DEV
/* 0x4ca6d0 0x518750 vt+0x20 [0x63d57c] */
#define BAKER_MAP 0x0063d57cu

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_baker_ask(uint32_t shader, uint32_t key)
{
    static unsigned asked, shared, other_attrs;
    asked++;
    uint32_t map = *(const uint32_t *)(uintptr_t)BAKER_MAP;
    uint32_t head = map + 4, at = head;
    uint32_t node = *(const uint32_t *)(uintptr_t)(map + 8);
    while (node) {                              /* lower_bound +8 +0xc */
        if (*(const uint32_t *)(uintptr_t)(node + 0x10) >= key) {
            at = node;
            node = *(const uint32_t *)(uintptr_t)(node + 8);
        } else
            node = *(const uint32_t *)(uintptr_t)(node + 0xc);
    }
    uint32_t baker = 0, first = 0;
    if (at != head && *(const uint32_t *)(uintptr_t)(at + 0x10) == key)
        baker = *(const uint32_t *)(uintptr_t)(at + 0x14);
    if (baker && !IsBadReadPtr((void *)(uintptr_t)baker, 0xc))
        first = *(const uint32_t *)(uintptr_t)(baker + 8);
    if (first && first != shader) {
        char a[128], b[128];
        shader_attrs(a, shader);
        shader_attrs(b, first);
        int differ = strcmp(a, b) != 0;
        shared++;
        other_attrs += differ;
        if (shared <= 32) {
            gmv_logfile("[bakershare] key 0x%08x: shader 0x%08x gets baker 0x%08x made for shader 0x%08x; attributes %s\n",
                        key, shader, baker, first, differ ? "DIFFER" : "match");
            dump_shader("asking ", shader);
            dump_shader("creator", first);
        }
    }
    if (asked == 1 || asked % 20 == 0)
        gmv_logfile("[bakershare] %u shaders asked for a baker: %u got another shader's, %u of those with other attributes\n",
                    asked, shared, other_attrs);
}

static void hook_baker_maker(uint32_t fn)
{
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53 };
    if (memcmp((const void *)(uintptr_t)fn, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; baker log skipped\n", fn);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr)
        return;
    size_t o = 0;
    tr[o++] = 0x60;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x28; /* push [esp+0x28] */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x28; /* push [esp+0x28] */
    tr[o++] = 0xb8;                                                 /* mov eax,&gmv_baker_ask */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_baker_ask; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x08;                 /* add esp,8 */
    tr[o++] = 0x61;
    memcpy(tr + o, expect, sizeof expect); o += sizeof expect;
    emit_jmp(tr, &o, fn + sizeof expect);
    FlushInstructionCache(GetCurrentProcess(), tr, o);
    if (!patch_jmp(fn, tr, 6))
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); baker log skipped\n", fn, GetLastError());
}

void apply_bakershare_log(void)
{
    if (gmv_env_off(gmv_dbgenv("GMV_BAKERSHARE")))
        return;
    hook_baker_maker(0x004ca6d0u);
    hook_baker_maker(0x00518750u);
    gmv_log("[gamepatch] baker log at 0x004ca6d0 and 0x00518750: shaders sharing a baker logged as [bakershare]\n");
}
#endif

void apply_vtxfix(void)
{
    if (gmv_env_off(gmv_getenv("GMV_VTXFIX"))) {
        gmv_log("[gamepatch] vertex-converter cl-guard DISABLED (VTXFIX=0)\n");
        return;
    }

    static const unsigned char expect[8] = { 0x3c, 0xff, 0x0f, 0x84, 0xbf, 0x02, 0x00, 0x00 };
    if (memcmp((const void *)(uintptr_t)VTX_SITE, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] site 0x%08x not the expected bytes; vertex fix skipped\n", VTX_SITE);
        return;
    }

    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for trampoline failed (%lu); vertex fix skipped\n", GetLastError());
        return;
    }

    /* al,0xff L_A cl,0xff L_B VTX_CONT */
    size_t o = 0;
    tr[o++] = 0x3c; tr[o++] = 0xff;                 /* cmp al,0xff */
    tr[o++] = 0x0f; tr[o++] = 0x84;
    size_t je_a = o; o += 4;
    tr[o++] = 0x80; tr[o++] = 0xf9; tr[o++] = 0xff; /* cmp cl,0xff */
    tr[o++] = 0x0f; tr[o++] = 0x84;
    size_t je_b = o; o += 4;
    emit_jmp(tr, &o, VTX_CONT);

    size_t l_a = o;
    emit_jmp(tr, &o, VTX_DESTA);                    /* L_A al==0xFF */

    size_t l_b = o;
    tr[o++] = 0x60;
    tr[o++] = 0x55;                                 /* push ebp */
    tr[o++] = 0x50;                                 /* push eax */
    tr[o++] = 0x53;                                 /* push ebx */
    tr[o++] = 0xb8;                                 /* mov eax */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_vtx_missing; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x0c; /* add esp,12 */
    tr[o++] = 0x61;
    emit_jmp(tr, &o, VTX_SKIP);

    { int32_t r = (int32_t)(l_a - (je_a + 4)); memcpy(tr + je_a, &r, 4); }
    { int32_t r = (int32_t)(l_b - (je_b + 4)); memcpy(tr + je_b, &r, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    if (!patch_jmp(VTX_SITE, tr, 8)) {             /* 0x563e31 */
        gmv_log("[gamepatch] VirtualProtect/write at 0x%08x failed (%lu); vertex fix skipped\n",
                VTX_SITE, GetLastError());
        return;
    }

    gmv_log("[gamepatch] vertex-converter cl-guard applied at 0x%08x (trampoline @ %p); "
            "dest-present/src-absent cases counted as [vtxmiss]\n",
            VTX_SITE, (void *)tr);
}
