#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* +0x54 +0x38 0x4000 +0xdc 0x5c2dc0 */
#define VC_FUNC 0x005c2dc0u     /* 0x6846c8 vtbl[0x60] */
#define VC_CONT 0x005c2dc6u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_engvol_log(void *voice)
{
    if (!voice || IsBadReadPtr(voice, 0xe0))
        return;
    const int32_t *v = (const int32_t *)voice;
    int32_t f38 = v[0x38 / 4];            /* 0x4000 */
    int32_t f54 = v[0x54 / 4];            /* 0x4000 */
    int32_t f64 = v[0x64 / 4];            /* PITCH 0x4000 0x8000 */
    int32_t f6c = v[0x6c / 4];
    int32_t fdc = v[0xdc / 4];            /* 0x4000 */

    /* 0x5c2e61..0x5c2e75 */
    static void    *seen[48];
    static int32_t  lrate[48], lvol[48], lpit[48], lstep[48];
    static unsigned nseen, lines, seq;

    int idx = -1;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == voice) { idx = (int)i; break; }

    int first = 0;
    if (idx < 0) {
        if (f38 != 11888)
            return;
        if (nseen >= 48)
            return;
        idx = (int)nseen++;
        seen[idx] = voice;
        lrate[idx] = f38; lvol[idx] = f54; lpit[idx] = f64; lstep[idx] = f6c;
        first = 1;
    }

    int32_t dr = f38 - lrate[idx]; if (dr < 0) dr = -dr;
    int32_t dv = f54 - lvol[idx];  if (dv < 0) dv = -dv;
    int32_t dp = f64 - lpit[idx];  if (dp < 0) dp = -dp;
    int32_t ds = f6c - lstep[idx]; if (ds < 0) ds = -ds;

    if (first || dr > 64 || dv > 128 || dp > 64 || ds > 64) {
        lrate[idx] = f38; lvol[idx] = f54; lpit[idx] = f64; lstep[idx] = f6c;
        if (lines++ < 4000)
            gmv_log("[engrev] #%u v%d base=%d pitch=%.3fx step=%d(%.3fx) vol=%.3f dop=%.3fx spd=%.0f%s\n",
                    seq++, idx, (int)f38, (double)f64 / 16384.0,
                    (int)f6c, (double)f6c / 16384.0,
                    (double)f54 / 16384.0, (double)fdc / 16384.0,
                    (double)g_carsnd_last_speed,
                    first ? " [new]" : "");
    }
}

void apply_engvol_trace(void)
{
    const char *e = gmv_dbgenv("GMV_CARSND_TRACE");
    if (!(gmv_env_on(e)))
        return;
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x38 };
    if (memcmp((const void *)(uintptr_t)VC_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; engvol trace skipped\n", VC_FUNC);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for engvol trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    /* [esp]=retaddr [esp+4]=voice __cdecl */
    size_t o = 0;
    tr[o++] = 0x8b; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04;  /* mov eax,[esp+4] */
    tr[o++] = 0x50;                                                  /* push eax */
    tr[o++] = 0xb8;                                                  /* mov eax,&gmv_engvol_log */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_engvol_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x04;                 /* add esp,4 */
    tr[o++] = 0x55;                                                 /* push ebp */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp */
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x38;                 /* sub esp,0x38 */
    emit_jmp_abs(tr, &o, VC_CONT);
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    if (!patch_jmp(VC_FUNC, tr, 6)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); engvol trace skipped\n", VC_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] engine-rev trace installed at 0x%08x (trampoline @ %p); "
            "[engrev] logs engine-voice rate/volume/doppler over time (pitch sweep vs crossfade)\n",
            VC_FUNC, (void *)tr);
}

/* 0x5b1980 +0x134/+0x138 +0x5c/+0x60 +0x94/+0x98 0x2c */
#define ENGSRC_FUNC 0x005b1980u
#define ENGSRC_CONT 0x005b1986u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_engsrc_log(void *src, uint32_t dtbits)
{
    if (!src || IsBadReadPtr(src, 0x140))
        return;
    static int armed;
    if (!armed) {
        if (g_carsnd_last_speed > 1.0f) armed = 1;
        else return;
    }
    static unsigned lines;
    if (lines >= 1200)
        return;

    const int32_t *s = (const int32_t *)src;
    int32_t volc = s[0x5c / 4], volt = s[0x60 / 4];
    int32_t pitc = s[0x94 / 4], pitt = s[0x98 / 4];
    void *v0 = (void *)(uintptr_t)s[0x134 / 4];
    void *v1 = (void *)(uintptr_t)s[0x138 / 4];
    float dt = 0.0f; memcpy(&dt, &dtbits, 4);

    float fpc, fpt, fvc, fvt;
    memcpy(&fpc, &pitc, 4); memcpy(&fpt, &pitt, 4);
    memcpy(&fvc, &volc, 4); memcpy(&fvt, &volt, 4);

    static void  *seen[64];
    static float  lpc[64], lvc[64];
    static unsigned nseen;
    int idx = -1;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == src) { idx = (int)i; break; }
    int first = 0;
    if (idx < 0) {
        if (nseen >= 64) return;
        idx = (int)nseen++; seen[idx] = src; lpc[idx] = fpc; lvc[idx] = fvc; first = 1;
    }
    float dp = fpc - lpc[idx]; if (dp < 0) dp = -dp;
    float dv = fvc - lvc[idx]; if (dv < 0) dv = -dv;
    if (first || dp > 0.01f || dv > 0.02f) {
        lpc[idx] = fpc; lvc[idx] = fvc;
        lines++;
        gmv_log("[engsrc] %p dt=%.4f pit=%.3f->%.3f(i %d/%d) vol=%.3f->%.3f v=%p,%p spd=%.0f%s\n",
                src, (double)dt, (double)fpc, (double)fpt, (int)pitc, (int)pitt,
                (double)fvc, (double)fvt, v0, v1, (double)g_carsnd_last_speed,
                first ? " [new]" : "");
    }
}

void apply_engsrc_trace(void)
{
    const char *e = gmv_dbgenv("GMV_ENGSRC_TRACE");
    if (!(gmv_env_on(e)))
        return;
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53 };
    if (memcmp((const void *)(uintptr_t)ENGSRC_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; engsrc trace skipped\n", ENGSRC_FUNC);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for engsrc trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    /* [esp]=ret [esp+4] [esp+8]=dt __cdecl */
    size_t o = 0;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8] */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8] */
    tr[o++] = 0xb8;                                                  /* mov eax,&gmv_engsrc_log */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_engsrc_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x08;                 /* add esp,8 */
    tr[o++] = 0x55;                                                 /* push ebp */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp */
    tr[o++] = 0x57;                                                 /* push edi */
    tr[o++] = 0x56;                                                 /* push esi */
    tr[o++] = 0x53;                                                 /* push ebx */
    emit_jmp_abs(tr, &o, ENGSRC_CONT);
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    if (!patch_jmp(ENGSRC_FUNC, tr, 6)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); engsrc trace skipped\n", ENGSRC_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] AudioSource-update trace installed at 0x%08x (trampoline @ %p); "
            "[engsrc] logs per-source pitch/vol/dt vs speed while driving\n", ENGSRC_FUNC, (void *)tr);
}

/* 0x5b1900 -> 0x90450 0x5b1940 */
#define ENGDTOR_DEL   0x005b1900u
#define ENGDTOR_NODEL 0x005b1940u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_engdtor_log(void *src, uint32_t which)
{
    static unsigned ndel, nnodel, lines;
    if (which) nnodel++; else ndel++;
    unsigned tot = ndel + nnodel;
    if (lines++ < 200 || (tot % 100) == 0)
        gmv_log("[engdtor] destroy %s src=%p  (deleting=%u non-deleting=%u total=%u)\n",
                which ? "nodel" : "del", src, ndel, nnodel, tot);
}

static void install_dtor_hook(uint32_t addr, uint32_t which)
{
    static const unsigned char expect[7] = { 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14 };
    if (memcmp((const void *)(uintptr_t)addr, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] dtor 0x%08x not the expected bytes; engdtor hook skipped\n", addr);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    size_t o = 0;
    tr[o++] = 0x68; { memcpy(tr + o, &which, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08; /* push dword [esp+8] */
    tr[o++] = 0xb8; { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_engdtor_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x08;                 /* add esp,8 */
    tr[o++] = 0x55;                                                 /* push ebp */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp */
    tr[o++] = 0x53;                                                 /* push ebx */
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x14;                 /* sub esp,0x14 */
    emit_jmp_abs(tr, &o, addr + 7);
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    if (!patch_jmp(addr, tr, 7)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); engdtor hook skipped\n", addr, GetLastError());
        return;
    }
    gmv_log("[gamepatch] AudioSource dtor hook at 0x%08x (which=%u, trampoline @ %p)\n", addr, which, (void *)tr);
}

void apply_engdtor_trace(void)
{
    const char *e = gmv_dbgenv("GMV_ENGSRC_TRACE");
    if (!(gmv_env_on(e)))
        return;
    install_dtor_hook(ENGDTOR_DEL, 0);
    install_dtor_hook(ENGDTOR_NODEL, 1);
}
