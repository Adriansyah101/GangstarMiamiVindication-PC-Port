#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xa2fc2..0xa3f61 vtbl+0xd8 0x689dd0 0x10aba8 0x63e478 */
#define SOLO_MIX_SITE    0x005bfdceu  /* mov edx,[0x94b8dc]; mov [esp+4],edx; mov [esp],eax */
#define SOLO_MIX_CONT    0x005bfddeu
#define SOLO_ACCUM_PTR   0x0094b8dcu
#define CARSND_API_SLOTS 0x0063e478u  /* isPlaying setVolume setPitch 0x10aba8 */
#define CARSND_API_TABLE 0x00689dd0u
#define SM_ISPLAYING     0x0006149au
#define SM_PLAY          0x0006137cu
#define SM_SETPITCH      0x00061036u
#define SM_SINGLETON     0x00688780u
#define AUDIO_DEVICE     0x0064d5e0u
#define DEV_FIND_SOURCE  0x005b0440u
#define DEV_LOCK         0x005be000u
#define DEV_UNLOCK       0x005bdfd0u

#define SOLO_MAX      32
#define SOLO_KEEP_MS  1000
#define SOLO_FRAMES   4096

static void *volatile g_solo_voice[SOLO_MAX];
static volatile DWORD g_solo_stamp[SOLO_MAX];
static int32_t        g_solo_scratch[SOLO_FRAMES * 2];

typedef uint32_t (*solo_api2_t)(uint32_t, uint32_t);
typedef uint32_t (*solo_api4_t)(uint32_t, uint32_t, uint32_t, uint32_t);
static solo_api2_t g_solo_orig_isplaying;
static solo_api4_t g_solo_orig_play;

static void *solo_resolve(int sample, int handle, char **src_out)
{
    *src_out = NULL;
    if (handle < 0 || handle > 3)
        return NULL;
    const char *sm = *(const char *const *)(uintptr_t)SM_SINGLETON;
    if (!sm || !(sm[0x220] & 2))
        return NULL;
    int idx = sample - *(const int *)(sm + 0x250);
    if (idx < 0 || idx >= *(const int *)(sm + 0x24c))
        return NULL;
    char *inst = *(char **)(*(char *const *)(sm + 0xc) + idx * 0x10 + handle * 4);
    char *dev  = *(char *const *)(uintptr_t)AUDIO_DEVICE;
    if (!inst || !dev)
        return NULL;

    typedef void (*lock_t)(void *);
    typedef char *(*find_t)(void *, void *);
    ((lock_t)(uintptr_t)DEV_LOCK)(dev + 0xf8);
    char *src   = ((find_t)(uintptr_t)DEV_FIND_SOURCE)(dev, inst + 0x24);
    void *voice = src ? *(void **)(src + 0x134) : NULL;
    ((lock_t)(uintptr_t)DEV_UNLOCK)(dev + 0xf8);
    *src_out = src;
    return voice;
}

static void solo_note(int sample, int handle)
{
    char *src;
    void *voice = solo_resolve(sample, handle, &src);
    if (!voice)
        return;

    DWORD now = GetTickCount();
    int slot = -1, oldest = 0;
    for (int i = 0; i < SOLO_MAX; i++) {
        if (g_solo_voice[i] == voice) { slot = i; break; }
        if (now - g_solo_stamp[i] > now - g_solo_stamp[oldest]) oldest = i;
    }
    if (slot < 0) {
        static unsigned logged;
        slot = oldest;
        g_solo_voice[slot] = voice;
        if (logged++ < 32)
            gmv_log("[solo] player-car voice %p (vtbl %p) <- sample %d handle %d src %p\n",
                    voice, *(void **)voice, sample, handle, (void *)src);
    }
    g_solo_stamp[slot] = now;
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_solo_isplaying(uint32_t sample, uint32_t handle)
{
    uint32_t r = g_solo_orig_isplaying(sample, handle);
    if (r & 0xff)
        solo_note((int)sample, (int)handle);
    return r;
}

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_solo_play(uint32_t sample, uint32_t loop, uint32_t vol, uint32_t pitch)
{
    uint32_t h = g_solo_orig_play(sample, loop, vol, pitch);
    solo_note((int)sample, (int)h);
    return h;
}

/* 0xa3016/0xa3308/0xa35b2/0xa387e +0xa0 0x5b2200 +0x98 */
typedef uint32_t (*solo_api3_t)(uint32_t, uint32_t, uint32_t);
static solo_api3_t g_solo_orig_setpitch;
static int g_carpitch_trace;

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_solo_setpitch(uint32_t sample, uint32_t handle, uint32_t pitch)
{
    uint32_t r = g_solo_orig_setpitch(sample, handle, pitch);
    if (!g_carpitch_trace)
        return r;
    static int   ls[32];
    static float lp[32];
    static unsigned n, lines;
    float p; memcpy(&p, &pitch, 4);
    int i = 0;
    while (i < (int)n && ls[i] != (int)sample) i++;
    if (i == (int)n) {
        if (n >= 32) return r;
        ls[n] = (int)sample; lp[n] = -1.0f; n++;
    }
    float d = p - lp[i]; if (d < 0) d = -d;
    if (d <= lp[i] * 0.005f || lines >= 6000)
        return r;
    lp[i] = p;
    if (lines++ == 0)
        gmv_log("[carpitch] voice ramp limit DAT_0064d5f8=%d\n", *(const int *)(uintptr_t)0x0064d5f8u);
    char *src;
    const int32_t *v = (const int32_t *)solo_resolve((int)sample, (int)handle, &src);
    float s98 = 0, sa0 = 0;
    if (src) { memcpy(&s98, src + 0x98, 4); memcpy(&sa0, src + 0xa0, 4); }
    gmv_log("[carpitch] t=%lu s%d h%d req=%.3f src(a0=%.3f 98=%.3f) voice(tgt=%.3f cur=%.3f step=%d st=%d len=%d)\n",
            GetTickCount(), (int)sample, (int)handle, (double)p, (double)sa0, (double)s98,
            v ? v[0x60 / 4] / 16384.0 : -1.0, v ? v[0x64 / 4] / 16384.0 : -1.0,
            v ? v[0x68 / 4] : 0, v ? v[0x7c / 4] : 0, v ? v[8 / 4] : 0);
    return r;
}

__attribute__((cdecl, force_align_arg_pointer))
static void *gmv_solo_mixbuf(void *voice, void *accum, int frames)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < SOLO_MAX; i++)
        if (g_solo_voice[i] == voice && now - g_solo_stamp[i] < SOLO_KEEP_MS)
            return accum;
    if (frames <= 0 || frames > SOLO_FRAMES)
        return accum;
    memset(g_solo_scratch, 0, (size_t)frames * 8);
    return g_solo_scratch;
}

static int solo_swap_ptr(uint32_t va, uint32_t expect, uint32_t repl)
{
    uint32_t cur;
    memcpy(&cur, (const void *)(uintptr_t)va, 4);
    if (cur != expect)
        return 0;
    return patch_bytes(va, (const unsigned char *)&repl, 4);
}

void apply_solo_car(void)
{
    const char *e = gmv_dbgenv("GMV_SOLO_CAR");
    if (!(gmv_env_on(e)))
        return;
    static const unsigned char expect[16] = {
        0x8b, 0x15, 0xdc, 0xb8, 0x94, 0x00,     /* mov edx,[0x94b8dc] */
        0x89, 0x54, 0x24, 0x04,                 /* mov [esp+4],edx */
        0x89, 0x04, 0x24,                       /* mov [esp],eax */
        0xff, 0x51, 0x60                        /* call [ecx+0x60] */
    };
    const uint32_t *slots = (const uint32_t *)(uintptr_t)CARSND_API_SLOTS;
    if (memcmp((const void *)(uintptr_t)SOLO_MIX_SITE, expect, sizeof expect) != 0 ||
        slots[0] != SM_ISPLAYING || slots[3] != SM_PLAY) {
        gmv_log("[gamepatch] mixer call / car-sound API slots not the expected bytes; solo-car skipped\n");
        return;
    }
    g_solo_orig_isplaying = (solo_api2_t)(uintptr_t)SM_ISPLAYING;
    g_solo_orig_play      = (solo_api4_t)(uintptr_t)SM_PLAY;
    uint32_t wis = (uint32_t)(uintptr_t)&gmv_solo_isplaying, wpl = (uint32_t)(uintptr_t)&gmv_solo_play;
    if (!solo_swap_ptr(CARSND_API_SLOTS + 0x0, SM_ISPLAYING, wis) ||
        !solo_swap_ptr(CARSND_API_SLOTS + 0xc, SM_PLAY, wpl)) {
        gmv_log("[gamepatch] car-sound API slot write failed (%lu); solo-car skipped\n", GetLastError());
        return;
    }
    /* 0x10aba8 */
    solo_swap_ptr(CARSND_API_TABLE + 0x0, SM_ISPLAYING, wis);
    solo_swap_ptr(CARSND_API_TABLE + 0x4, SM_PLAY, wpl);

    const char *t = gmv_dbgenv("GMV_CARSND_TRACE");
    if (gmv_env_on(t) && slots[4] == SM_SETPITCH) {
        uint32_t wsp = (uint32_t)(uintptr_t)&gmv_solo_setpitch;
        g_solo_orig_setpitch = (solo_api3_t)(uintptr_t)SM_SETPITCH;
        if (solo_swap_ptr(CARSND_API_SLOTS + 0x10, SM_SETPITCH, wsp)) {
            solo_swap_ptr(CARSND_API_TABLE + 0xc, SM_SETPITCH, wsp);
            g_carpitch_trace = 1;
            gmv_log("[gamepatch] car-pitch trace on: [carpitch] logs each engine setPitch vs AudioSource/voice "
                    "state (voice ramp limit DAT_0064d5f8=%d now)\n", *(const int *)(uintptr_t)0x0064d5f8u);
        }
    }

    unsigned char *tr = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for solo-car trampoline failed (%lu); skipped\n", GetLastError());
        return;
    }
    /* eax ecx [esp+0xc] 0x5bfdca */
    size_t o = 0;
    tr[o++] = 0x8b; tr[o++] = 0x15;                                  /* mov edx,[accum] */
    { uint32_t a = SOLO_ACCUM_PTR; memcpy(tr + o, &a, 4); o += 4; }
    tr[o++] = 0x51;                                                  /* push ecx */
    tr[o++] = 0x50;                                                  /* push eax */
    tr[o++] = 0xff; tr[o++] = 0x74; tr[o++] = 0x24; tr[o++] = 0x14;  /* push dword [esp+0x14] */
    tr[o++] = 0x52;                                                  /* push edx */
    tr[o++] = 0x50;                                                  /* push eax */
    tr[o++] = 0xb8;                                                  /* mov eax,&gmv_solo_mixbuf */
    { uint32_t fnp = (uint32_t)(uintptr_t)&gmv_solo_mixbuf; memcpy(tr + o, &fnp, 4); o += 4; }
    tr[o++] = 0xff; tr[o++] = 0xd0;                                  /* call eax */
    tr[o++] = 0x83; tr[o++] = 0xc4; tr[o++] = 0x0c;                  /* add esp,12 */
    tr[o++] = 0x89; tr[o++] = 0xc2;                                  /* mov edx,eax */
    tr[o++] = 0x58;                                                  /* pop eax */
    tr[o++] = 0x59;                                                  /* pop ecx */
    tr[o++] = 0x89; tr[o++] = 0x54; tr[o++] = 0x24; tr[o++] = 0x08;  /* mov [esp+8],edx */
    tr[o++] = 0x89; tr[o++] = 0x44; tr[o++] = 0x24; tr[o++] = 0x04;  /* mov [esp+4],eax */
    tr[o++] = 0xff; tr[o++] = 0x61; tr[o++] = 0x60;                  /* jmp [ecx+0x60] */
    FlushInstructionCache(GetCurrentProcess(), tr, o);

    unsigned char site[16];
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)tr - (SOLO_MIX_SITE + 5));
    site[0] = 0xe8;
    memcpy(site + 1, &rel, 4);
    site[5] = 0xeb;
    site[6] = (unsigned char)(SOLO_MIX_CONT - (SOLO_MIX_SITE + 7));
    memset(site + 7, 0x90, sizeof site - 7);
    if (!patch_bytes(SOLO_MIX_SITE, site, sizeof site)) {
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); solo-car skipped\n", SOLO_MIX_SITE, GetLastError());
        return;
    }
    gmv_log("[gamepatch] SOLO CAR (test aid): only the player's car sounds are mixed; traffic, peds, "
            "music, radio and other SFX are muted (mixer hook 0x%08x, trampoline %p)\n",
            SOLO_MIX_SITE, (void *)tr);
}
