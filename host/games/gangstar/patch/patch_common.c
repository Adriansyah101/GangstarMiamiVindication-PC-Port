#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

patch_undo *patch_undo_now;
patch_undo mac_undo;

#if GMV_DEV
ma_site_t ma_site[MA_SITES];
int ma_sites;

static void ma_note(uint32_t va, size_t n)
{
    if (ma_sites >= MA_SITES || n > sizeof ma_site[0].was || (va >= 0x11000u && va < 0x63d000u) ||
        (va >= 0x64ea40u && va < 0x6847b8u))
        return;
    for (int i = 0; i < ma_sites; i++)
        if (ma_site[i].va == va)
            return;
    ma_site[ma_sites].va = va, ma_site[ma_sites].n = (unsigned)n;
    memcpy(ma_site[ma_sites++].was, (const void *)(uintptr_t)va, n);
}
#endif

int patch_bytes(uint32_t va, const unsigned char *buf, size_t n)
{
    DWORD old;
    void *p = (void *)(uintptr_t)va;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
#if GMV_DEV
    ma_note(va, n);
#endif
    if (patch_undo_now) {
        patch_undo *u = patch_undo_now;
        if (n <= sizeof u->e[0].was && u->n < PATCH_UNDO_MAX) {
            u->e[u->n].va = va, u->e[u->n].n = (unsigned)n;
            memcpy(u->e[u->n++].was, p, n);
        } else
            gmv_log("[gamepatch] patch at 0x%08x cannot be taken out again\n", va);
    }
    memcpy(p, buf, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

void patch_undo_all(patch_undo *u)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = NULL;
    while (u->n > 0) {
        u->n--;
        patch_bytes(u->e[u->n].va, u->e[u->n].was, u->e[u->n].n);
    }
    patch_undo_now = keep;
}

void emit_jmp(unsigned char *tr, size_t *o, uint32_t dst)
{
    int32_t rel = (int32_t)(dst - ((uint32_t)(uintptr_t)tr + (uint32_t)*o + 5));
    tr[(*o)++] = 0xe9;
    memcpy(tr + *o, &rel, 4);
    *o += 4;
}

void emit_jmp_abs(unsigned char *tr, size_t *o, uint32_t dst)
{
    uint32_t slot = (uint32_t)(uintptr_t)(tr + *o + 6);
    tr[(*o)++] = 0xff;
    tr[(*o)++] = 0x25;
    memcpy(tr + *o, &slot, 4);
    *o += 4;
    memcpy(tr + *o, &dst, 4);
    *o += 4;
}

int patch_jmp(uint32_t site, const void *to, size_t len)
{
    unsigned char code[16];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)to - (site + 5));
    if (len < 5 || len > sizeof code)
        return 0;
    memset(code, 0x90, len);
    code[0] = 0xe9;
    memcpy(code + 1, &rel, 4);
    return patch_bytes(site, code, len);
}

int patch_call(uint32_t site, const void *to)
{
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)to - (site + 5));
    return patch_bytes(site + 1, (const unsigned char *)&rel, 4);
}

int code_write(uint32_t va, const void *buf, size_t n)
{
    DWORD old;
    void *p = (void *)(uintptr_t)va;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, buf, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

int code_jmp(uint32_t site, const void *to)
{
    unsigned char code[5] = { 0xe9 };
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)to - (site + 5));
    memcpy(code + 1, &rel, 4);
    return code_write(site, code, sizeof code);
}

int code_call(uint32_t site, const void *to)
{
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)to - (site + 5));
    return code_write(site + 1, &rel, 4);
}

uint32_t code_call_target(uint32_t site)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)site;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return p[0] == 0xe8 ? site + 5 + (uint32_t)rel : 0;
}
