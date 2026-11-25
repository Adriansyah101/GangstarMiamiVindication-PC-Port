#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* mov eax,[ebp+0x10] [esp+4],eax */
#define FE_SITE 0x004c5db8u

static const unsigned char fe_orig[7] = { 0x8b, 0x45, 0x10, 0x89, 0x44, 0x24, 0x04 };
static unsigned char *fe_stub;
static int32_t fe_rect[4];

__attribute__((cdecl, force_align_arg_pointer))
static const int32_t *gmv_fade_edge_rect(const int32_t *r)
{
    if (!r || r[0] != 0 || r[1] != 0)
        return r;
    fe_rect[0] = fe_rect[1] = -1;
    fe_rect[2] = r[2];
    fe_rect[3] = r[3];
    return fe_rect;
}

static void fe_site_bytes(unsigned char *b)
{
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)fe_stub - (FE_SITE + 5));
    b[0] = 0xe8;
    memcpy(b + 1, &rel, 4);
    b[5] = b[6] = 0x90;
}

int fade_edge_set(int on)
{
    if (!fe_stub) {
        unsigned char *s = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s)
            return -2;
        size_t o = 0;
        uint32_t fn = (uint32_t)(uintptr_t)&gmv_fade_edge_rect;
        s[o++] = 0x51;                                              /* push ecx */
        s[o++] = 0x52;                                              /* push edx */
        s[o++] = 0xff; s[o++] = 0x75; s[o++] = 0x10;                /* push dword [ebp+0x10] */
        s[o++] = 0xb8; memcpy(s + o, &fn, 4); o += 4;               /* mov eax,gmv_fade_edge_rect */
        s[o++] = 0xff; s[o++] = 0xd0;                               /* call eax */
        s[o++] = 0x83; s[o++] = 0xc4; s[o++] = 0x04;                /* add esp,4 */
        s[o++] = 0x5a;                                              /* pop edx */
        s[o++] = 0x59;                                              /* pop ecx */
        s[o++] = 0x89; s[o++] = 0x44; s[o++] = 0x24; s[o++] = 0x08; /* mov [esp+8],eax */
        s[o++] = 0xc3;
        FlushInstructionCache(GetCurrentProcess(), s, o);
        fe_stub = s;
    }
    unsigned char ours[7];
    fe_site_bytes(ours);
    const void *p = (const void *)(uintptr_t)FE_SITE;
    if (memcmp(p, fe_orig, sizeof fe_orig) && memcmp(p, ours, sizeof ours))
        return -1;
    return patch_bytes(FE_SITE, on ? ours : fe_orig, sizeof ours) ? on : -2;
}

void apply_fade_edge_fix(void)
{
    if (gmv_env_off(gmv_getenv("GMV_FADE_EDGE_FIX")))
        return;
    int r = fade_edge_set(1);
    if (r == -1)
        gmv_log("[gamepatch] 0x%08x not the expected bytes; fade edge fix skipped\n", FE_SITE);
    else if (r == -2)
        gmv_log("[gamepatch] fade edge fix not applied (%lu)\n", GetLastError());
    else
        gmv_log("[gamepatch] full-screen fades cover the top row and left column too (FADE_EDGE_FIX=0 to disable)\n");
}
