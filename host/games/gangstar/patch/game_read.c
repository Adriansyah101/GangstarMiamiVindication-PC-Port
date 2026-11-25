#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* *0x689e98 +0x50 +0x54 0x8ce6c +0x1c +0x8c 0x8da02 */
#define MS_APP       0x006853a0u

uint32_t ms_rd(uint32_t a)
{
    if (a - GMV_RESERVE_START <= GMV_MACHO_END - GMV_RESERVE_START - 4)
        return *(const uint32_t *)(uintptr_t)a;
#ifndef _WIN32
    return a >= 0x1000 && gmv_readable((const void *)(uintptr_t)a, 4) ? *(const uint32_t *)(uintptr_t)a : 0;
#else
    MEMORY_BASIC_INFORMATION mbi;
    if (a < 0x1000 || !VirtualQuery((const void *)(uintptr_t)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)) ||
        (const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)(uintptr_t)a < 4)
        return 0;
    return *(const uint32_t *)(uintptr_t)a;
#endif
}

/* 0x8f426 */
static const char *ms_str_at(uint32_t p, char *out, size_t n)
{
    size_t o = 0;
    out[o++] = '"';
    for (int i = 0; p && i < 48 && o + 3 < n; i++) {
        uint32_t w = ms_rd((p + i) & ~3u);
        unsigned char c = (unsigned char)(w >> 8 * ((p + i) & 3));
        if (!c)
            break;
        out[o++] = c >= 0x20 && c < 0x7f ? (char)c : '.';
    }
    out[o++] = '"';
    out[o] = 0;
    return out;
}

const char *ms_text(uint32_t id, char *out, size_t n)
{
    if (id == MS_NONE_ID)
        return "-";
    uint32_t table = ms_rd(ms_rd(MS_APP) + MS_STRINGS);
    uint32_t lang = ms_rd(table + 4);
    uint32_t grp = id >= 0x1000 && id < 0x10000 && lang < 16 ? ms_rd(table + 8 + lang * 0x34 + (id >> 12 & 15) * 4) : 0;
    uint32_t p = grp ? ms_rd(grp + (id & 0xfff) * 4) : 0;
    return p ? ms_str_at(p, out, n) : "?";
}

int ms_cmd_type(uint32_t c)
{
    uint32_t vt = ms_rd(c), fn = vt ? ms_rd(vt + 0x1c) : 0;
    return fn >= 0x2000 && fn < 0x600000 ? ((int (*)(uint32_t))(uintptr_t)fn)(c) : -1;
}
