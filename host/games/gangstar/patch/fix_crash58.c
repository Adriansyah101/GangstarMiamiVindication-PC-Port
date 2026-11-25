#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x58c1ab 0x58c190 0x58c196 NULL */
#define CR58_FUNC 0x0058c190u   /* push ebp; mov ebp,esp; sub esp,0x28 */
#define CR58_CONT 0x0058c196u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_crash58_log(void *thisp, void *caller, const uint32_t *args, const uint32_t *frame)
{
    const uint32_t *t = (const uint32_t *)thisp;
    uint32_t vtbl = (t && !IsBadReadPtr((void *)t, 4)) ? t[0] : 0;
    uint32_t f4   = (t && !IsBadReadPtr((void *)t, 8)) ? t[1] : 0;
    static unsigned n;
    if (n++ < 64 || (n % 512) == 0)
        gmv_log("[crash58] guarded null forward: this=%p vtbl=0x%08x field4=0x%08x caller=%p (n=%u)\n",
                thisp, vtbl, f4, caller, n);
#if !GMV_DEV
    (void)args;
    (void)frame;
#else
    if (n > 8)
        return;
    /* [ebp+0xc] *db+0x10 image+0x20 */
    uint32_t db = 0, image = 0, root = 0, namep = 0, chars = 0;
    if (frame && !IsBadReadPtr((void *)frame, 0x10))
        db = frame[3];
    if (db && !IsBadReadPtr((void *)(uintptr_t)db, 4)) {
        uint32_t d = *(const uint32_t *)(uintptr_t)db;
        if (d && !IsBadReadPtr((void *)(uintptr_t)d, 0x14))
            image = *(const uint32_t *)(uintptr_t)(d + 0x10);
    }
    if (image && !IsBadReadPtr((void *)(uintptr_t)image, 0x24))
        root = *(const uint32_t *)(uintptr_t)(image + 0x20);
    if (root && !IsBadReadPtr((void *)(uintptr_t)root, 8))
        namep = *(const uint32_t *)(uintptr_t)(root + 4);
    if (namep && !IsBadReadPtr((void *)(uintptr_t)namep, 4))
        chars = *(const uint32_t *)(uintptr_t)namep;
    const char *fail;
    int err;
    long open_files;
    gmv_fopen_info(&fail, &err, &open_files);
    gmv_log("[crash58] chunk size 0x%x offset 0x%x, bank root 0x%08x name pointer 0x%08x \"%.200s\"\n",
            args ? args[2] : 0, args ? args[3] : 0, root, namep,
            chars && !IsBadReadPtr((void *)(uintptr_t)chars, 1) ? (const char *)(uintptr_t)chars : "");
    gmv_log("[crash58] open streams %ld of %d; last failed open errno %d \"%s\"\n",
            open_files, _getmaxstdio(), err, fail);
#endif
}

void apply_crash58_guard(void)
{
    if (gmv_env_off(gmv_getenv("GMV_CRASH58_GUARD")))
        return;
    /* push ebp; mov ebp,esp; sub esp,0x28 */
    static const unsigned char expect[8] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x28, 0x89, 0x5d };
    if (memcmp((const void *)(uintptr_t)CR58_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; crash58 guard skipped\n", CR58_FUNC);
        return;
    }

    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for crash58 trampoline failed (%lu); guard skipped\n", GetLastError());
        return;
    }

    /* [esp] [esp+4] [esp+8] [esp+0xc] [esp+0x10] __cdecl */
    size_t o = 0;
    tr[o++] = 0x8b; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04; /* mov eax,[esp+4] */
    tr[o++] = 0x85; tr[o++] = 0xc0;                                 /* test eax,eax */
    tr[o++] = 0x74; size_t j1 = o++;
    tr[o++] = 0x8b; tr[o++] = 0x40; tr[o++] = 0x04;                 /* mov eax,[eax+4] */
    tr[o++] = 0x85; tr[o++] = 0xc0;                                 /* test eax,eax */
    tr[o++] = 0x74; size_t j2 = o++;
    tr[o++] = 0x55;                                                 /* push ebp */
    tr[o++] = 0x89; tr[o++] = 0xe5;                                 /* mov ebp,esp */
    tr[o++] = 0x83; tr[o++] = 0xec; tr[o++] = 0x28;                 /* sub esp,0x28 */
    tr[o++] = 0xff; tr[o++] = 0x25; size_t contref = o; o += 4;
    size_t lnull = o;
    tr[o++] = 0x8b; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04; /* mov eax,[esp+4] */
    tr[o++] = 0x8b; tr[o++] = 0x14; tr[o++] = 0x24;                 /* mov edx,[esp] */
    tr[o++] = 0x89; tr[o++] = 0xe1;                                 /* mov ecx,esp */
    tr[o++] = 0x55;                                                 /* push ebp */
    tr[o++] = 0x51;                                                 /* push ecx */
    tr[o++] = 0x52;                                                 /* push edx */
    tr[o++] = 0x50;                                                 /* push eax */
    tr[o++] = 0xb8;                                                 /* mov eax,&gmv_crash58_log */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_crash58_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x10;                 /* add esp,16 */
    tr[o++] = 0x31; tr[o++] = 0xc0;                                 /* xor eax,eax */
    tr[o++] = 0xc3;
    size_t contslot = o;
    { uint32_t c = CR58_CONT; memcpy(tr + o, &c, 4); o += 4; }

    tr[j1] = (unsigned char)(lnull - (j1 + 1));
    tr[j2] = (unsigned char)(lnull - (j2 + 1));
    { uint32_t a = (uint32_t)(uintptr_t)(tr + contslot); memcpy(tr + contref, &a, 4); }
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    if (!patch_jmp(CR58_FUNC, tr, 6)) {                             /* 0x58c196 */
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); crash58 guard skipped\n", CR58_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] crash58 null-forward guard applied at 0x%08x (trampoline @ %p; permanent, "
            "CRASH58_GUARD=0 to disable); null this->field4 now logged as [crash58] + returns 0\n",
            CR58_FUNC, (void *)tr);
}
