#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x4c5ce0 0x34918 0x6424b4 0x8d050 0x1a5b32 0x1b279e */
#define FADE_FUNC 0x004c5ce0u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_fade_log(uint32_t caller, uint32_t color, const int32_t *rect)
{
    if (!rect || IsBadReadPtr(rect, 16) || rect[0] != 0 || rect[1] != 0)
        return;
    gmv_gl_fade_mark(caller, caller == 0x1b5094);
    static uint32_t lcaller, lcolor;
    static unsigned lines;
    if (caller == lcaller && color == lcolor)
        return;
    lcaller = caller; lcolor = color;
    if (lines++ < 4000)
        gmv_log("[fade] t=%lu caller=0x%06x alpha=%u rgb=%06x rect=%dx%d\n", (unsigned long)GetTickCount(),
                caller, color >> 24, color & 0xffffff, (int)rect[2], (int)rect[3]);
}

void apply_fade_trace(void)
{
    const char *e = gmv_dbgenv("GMV_FADE_TRACE");
    if (!(gmv_env_on(e)))
        return;
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x57, 0x56, 0x53 };
    if (memcmp((const void *)(uintptr_t)FADE_FUNC, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; fade trace skipped\n", FADE_FUNC);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    /* [esp]=ret [esp+4]=driver [esp+8]=color [esp+0xc]=rect* */
    size_t o = 0;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x0c;  /* push dword [esp+0xc] */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x0c;  /* push dword [esp+0xc] */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8] */
    tr[o++] = 0xb8; { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_fade_log; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x0c;                 /* add esp,0xc */
    tr[o++] = 0x55; tr[o++] = 0x89; tr[o++] = 0xe5;                 /* push ebp; mov ebp,esp */
    tr[o++] = 0x57; tr[o++] = 0x56; tr[o++] = 0x53;                 /* push edi; push esi; push ebx */
    emit_jmp_abs(tr, &o, FADE_FUNC + 6);
    FlushInstructionCache(GetCurrentProcess(), tr, o);
    if (!patch_jmp(FADE_FUNC, tr, 6)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); fade trace skipped\n", FADE_FUNC, GetLastError());
        return;
    }
    gmv_log("[gamepatch] fade trace installed at 0x%08x (trampoline @ %p); [fade] logs black-rect alpha changes\n",
            FADE_FUNC, (void *)tr);
}

/* 0xcd8be 0xcda6c 0x6424b0 */
#define CINE_FADEIN_UPD  0x000cd8beu
#define CINE_FADEOUT_UPD 0x000cda6cu
#define CINE_TIMER       0x006424b0u
#define CINE_ALPHA       0x006424b4u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cine_log(uint32_t which, uint32_t ret, const int32_t *cmd, int32_t dt)
{
    static unsigned lines;
    if (lines >= 6000 || !cmd || IsBadReadPtr(cmd, 0x14))
        return;
    lines++;
    gmv_log("[cine] t=%lu %s ret=0x%06x cmd=%p len=%d c=%d dt=%d timer=%d alpha=%d\n",
            (unsigned long)GetTickCount(), which ? "out" : "in ", ret, (const void *)cmd, (int)cmd[4],
            (int)cmd[3], (int)dt, *(const int32_t *)(uintptr_t)CINE_TIMER,
            *(const int32_t *)(uintptr_t)CINE_ALPHA);
}

/* __cdecl */
static void hook_cine_update(uint32_t fn, uint32_t which, void *cb)
{
    static const unsigned char expect[6] = { 0x55, 0x89, 0xe5, 0x53, 0x57, 0x56 };
    if (memcmp((const void *)(uintptr_t)fn, expect, sizeof expect) != 0) {
        gmv_log("[gamepatch] fn 0x%08x not the expected bytes; trace hook skipped\n", fn);
        return;
    }
    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    /* [esp]=ret [esp+4] [esp+8]=dt */
    size_t o = 0;
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8] */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8] */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x08;  /* push dword [esp+8]; ret */
    tr[o++] = 0x68; memcpy(tr + o, &which, 4); o += 4;
    tr[o++] = 0xb8; { uint32_t fnp = (uint32_t)(uintptr_t)cb; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                 /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x10;                 /* add esp,0x10 */
    memcpy(tr + o, expect, sizeof expect); o += sizeof expect;
    emit_jmp_abs(tr, &o, fn + 6);
    FlushInstructionCache(GetCurrentProcess(), tr, o);
    if (!patch_jmp(fn, tr, 6)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); trace hook skipped\n", fn, GetLastError());
        return;
    }
    gmv_log("[gamepatch] trace hook installed at 0x%08x (trampoline @ %p)\n", fn, (void *)tr);
}

/* 0xf8282 0xf9050 +0x578 +0x574 0x2f644 0x7e278 */
#define PLAYER_APPLY_DAMAGE 0x000f8282u
#define PLAYER_HEALTH_OFF   (4 + 0x578)
#define PLAYER_MAXHP_OFF    (4 + 0x574)
static volatile uint32_t g_player;
static volatile int g_player_hp0;

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_player_damage(uint32_t which, uint32_t ret, uint32_t player, int32_t damage)
{
    (void)which;
    if (player != g_player && !IsBadReadPtr((const void *)(uintptr_t)player, PLAYER_HEALTH_OFF + 4)) {
        g_player_hp0 = *(const int32_t *)(uintptr_t)(player + PLAYER_HEALTH_OFF);
        gmv_trace("[death] player object %08x health %d (applyDamage from 0x%06x, damage %d)\n", player,
                g_player_hp0, ret, (int)damage);
        g_player = player;
    }
}

void gmv_death_poll(void)
{
    static uint32_t lplayer;
    static int lhp;
    uint32_t p = g_player;
    if (!p || IsBadReadPtr((const void *)(uintptr_t)p, PLAYER_HEALTH_OFF + 4))
        return;
    int hp = *(const int32_t *)(uintptr_t)(p + PLAYER_HEALTH_OFF);
    int maxhp = *(const int32_t *)(uintptr_t)(p + PLAYER_MAXHP_OFF);
    if (p != lplayer)
        lhp = g_player_hp0;
    if (lhp > 0 && hp <= 0) {
        gmv_trace("[death] t=%lu player health %d -> %d (max %d)\n", (unsigned long)GetTickCount(), lhp, hp, maxhp);
        gmv_gl_death_capture_arm(hp, maxhp);
    }
    lplayer = p;
    lhp = hp;
}

void apply_cine_trace(void)
{
    const char *e = gmv_dbgenv("GMV_FADE_TRACE");
    if (!(gmv_env_on(e)))
        return;
    hook_cine_update(CINE_FADEIN_UPD, 0, (void *)&gmv_cine_log);
    hook_cine_update(CINE_FADEOUT_UPD, 1, (void *)&gmv_cine_log);
    hook_cine_update(PLAYER_APPLY_DAMAGE, 2, (void *)&gmv_player_damage);
}
