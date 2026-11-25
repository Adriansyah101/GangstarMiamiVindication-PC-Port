#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* DRAW_DISTANCE 0x142892 +0x0c 0xb5a00 0x54cec0 0x142d5a */
#define FOG_SITE 0x00142d5au
#define FOG_FN   0x0054cec0u
#define FOG_START_M      100.0f
#define FOG_END_M        800.0f
#define FOG_GAME_START_M 450.0f
#define FOG_GAME_END_M   900.0f
#define FOG_END_PER_X    100.0f

#define SKY_RADIUS 60000.0f         /* 0xbe7d0 */

float g_fog_world_far;
float g_fog_clip;
float sky_k = 1;
float g_cam_z;

int g_fog_all, g_sky_draw, g_cp_draw;
static int fog_mode = -1;
int fog_sky_wrapped;

int gmv_fog_mode(void)
{
    if (fog_mode < 0) {
        const char *m = gmv_getenv("GMV_FOG_MODE");
        fog_mode = m && *m ? atoi(m) : 0;
        fog_mode = fog_mode < 0 ? 0 : fog_mode > 2 ? 2 : fog_mode;
    }
    return fog_mode;
}

int gmv_fog_mode_set(int mode)
{
    gmv_fog_mode();
    fog_mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    if (fog_mode && !fog_sky_wrapped)
        fog_mode = 0;
    g_fog_all = fog_mode > 0;
    if (!g_fog_all)
        sky_k = 0;
    gmv_log("[gamepatch] fog mode %d%s\n", fog_mode, g_fog_all ? ": fog on the whole world" : ": the game's own fog");
    return fog_mode;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_set_fog(void *params, int id, int index, const float *range)
{
    g_fog_world_far = (3 * range[1] + 2 * range[0]) / 5;
    int mac = gmv_config_mac();
    static float logged = -1;
    if (mac && !g_fog_all) {
        if (logged != 0) {
            logged = 0;
            gmv_log("[gamepatch] fog %.0f..%.0f m, the game's own (ORIGINAL_MAC_EXPERIENCE)\n", range[0] / 100, range[1] / 100);
        }
        return ((int (*)(void *, int, int, const float *))(uintptr_t)FOG_FN)(params, id, index, range);
    }
    double dd = dd_on && dd_scale > 1 ? dd_scale : 1;
    float extra = (float)(FOG_END_PER_X * (dd - 1));
    float r[2] = { (g_fog_all ? FOG_START_M : FOG_GAME_START_M) * 100, ((g_fog_all ? FOG_END_M : FOG_GAME_END_M) + extra) * 100 };
    if (g_fog_all) {
        g_fog_clip = r[1] < g_fog_world_far ? r[1] : g_fog_world_far;
        sky_k = 1 - SKY_RADIUS / g_fog_clip;
    }
    if (logged != r[1] + g_fog_all) {
        logged = r[1] + g_fog_all;
        if (g_fog_all)
            gmv_log("[gamepatch] fog %.0f..%.0f m at draw distance %.2gx (the game's %.0f..%.0f m); the world seen to %.0f m, "
                    "the skyline's foot there\n", r[0] / 100, r[1] / 100, dd, range[0] / 100, range[1] / 100, g_fog_clip / 100);
        else
            gmv_log("[gamepatch] the game's own fog at %.0f..%.0f m at draw distance %.2gx (the game's %.0f..%.0f m)\n",
                    r[0] / 100, r[1] / 100, dd, range[0] / 100, range[1] / 100);
    }
    return ((int (*)(void *, int, int, const float *))(uintptr_t)FOG_FN)(params, id, index, r);
}

/* 0xbe468 0x6522d8 g_sky_draw */
#define FOG_SKY_VT     0x006522d8u
#define FOG_SKY_RENDER 0x000be468u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_skybox_render(void *n, int arg)
{
    g_sky_draw = 1;
    ((void (*)(void *, int))(uintptr_t)FOG_SKY_RENDER)(n, arg);
    g_sky_draw = 0;
}

#define CP_RENDER_SITE 0x000b9a26u

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_transparent_render(uint32_t n, uint32_t pass)
{
    for (uint32_t p = n, k = 0; p && k < 4 && !g_cp_draw; p = *(uint32_t *)(uintptr_t)(p + 0xd8), k++) {
        const char *s = *(const char **)(uintptr_t)(p + 0xc);
        g_cp_draw = s && !strncmp(s, "checkpoint_", 11) &&
                    (!strncmp(s + 11, "race", 4) || !strncmp(s + 11, "mission", 7) || !strncmp(s + 11, "drive", 5) ||
                     !strncmp(s + 11, "store", 5) || !strncmp(s + 11, "elevator", 8) || !strncmp(s + 11, "arrow", 5));
    }
    ((void (*)(uint32_t, uint32_t))(uintptr_t)(*(uint32_t **)(uintptr_t)n)[0x1c / 4])(n, pass);
    g_cp_draw = 0;
}

void apply_checkpoint_glow(void)
{
    static const unsigned char orig[12] = { 0x8b, 0x02, 0x89, 0x5c, 0x24, 0x04, 0x89, 0x14, 0x24, 0xff, 0x50, 0x1c };
    unsigned char b[12] = { 0x89, 0x5c, 0x24, 0x04, 0x89, 0x14, 0x24, 0xe8 };
    const char *e = gmv_getenv("GMV_BLOOM");
    if (gmv_config_mac() || (!(e && atoi(e) > 0) && !tab_live()))
        return;
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)gmv_transparent_render - (CP_RENDER_SITE + sizeof b));
    memcpy(b + 8, &rel, 4);
    const void *at = (const void *)(uintptr_t)CP_RENDER_SITE;
    if (memcmp(at, orig, sizeof b) && memcmp(at, b, sizeof b))
        gmv_log("[gamepatch] transparent render call not the expected bytes; checkpoints glow in the bloom\n");
    else
        patch_bytes(CP_RENDER_SITE, b, sizeof b);
}

void apply_fog(void)
{
    if (gmv_config_mac())
        return;
    if (gmv_fog_mode() || tab_live()) {
        uint32_t w = (uint32_t)(uintptr_t)gmv_skybox_render;
        if (*(uint32_t *)(uintptr_t)FOG_SKY_VT == FOG_SKY_RENDER && patch_bytes(FOG_SKY_VT, (const unsigned char *)&w, 4)) {
            fog_sky_wrapped = 1;
            g_fog_all = gmv_fog_mode() > 0;
            sky_k = g_fog_all ? 1.0f : 0.0f;
            gmv_log("[gamepatch] %s\n", g_fog_all ? "fog on the whole world (the sky left clear, the skyline hazed)"
                                                  : "the game's own fog, the whole world's ready for the options menu");
        } else {
            fog_mode = 0;
            gmv_log("[gamepatch] skybox render not the expected entry; fog stays on the game's own materials\n");
        }
    }
    if (!sm_is_call(FOG_SITE, FOG_FN)) {
        gmv_log("[gamepatch] fog setter not the expected bytes; the fog keeps the game's distances\n");
        return;
    }
    if (!patch_call(FOG_SITE, (void *)gmv_set_fog))
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); the fog keeps the game's distances\n", FOG_SITE + 1, GetLastError());
}

/* 0xbe5c6 eax */
#define SKY_HEIGHT_SITE 0x000be5c6u   /* movl $0x0 */
#define SKY_HEIGHT_CONT 0x000be5cdu

void apply_sky_horizon(void)
{
    static const unsigned char orig[7] = { 0xc7, 0x45, 0x80, 0x00, 0x00, 0x00, 0x00 };
    if (!fog_sky_wrapped)
        return;
    if (memcmp((const void *)(uintptr_t)SKY_HEIGHT_SITE, orig, sizeof orig)) {
        gmv_log("[gamepatch] skybox height not the expected bytes; the skyline stays at sea level\n");
        return;
    }
    static unsigned char *tr;
    if (!tr)
        tr = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) {
        gmv_log("[gamepatch] VirtualAlloc for the skybox height failed (%lu); the skyline stays at sea level\n",
                GetLastError());
        return;
    }
    size_t o = 0;
    static const unsigned char load[5] = { 0xf3, 0x0f, 0x10, 0x40, 0x08 };     /* 8(%eax),%xmm0 */
    memcpy(tr + o, load, 5), o += 5;
    tr[o++] = 0xf3, tr[o++] = 0x0f, tr[o++] = 0x11, tr[o++] = 0x05;
    uint32_t cz = (uint32_t)(uintptr_t)&g_cam_z;
    memcpy(tr + o, &cz, 4), o += 4;
    tr[o++] = 0xf3, tr[o++] = 0x0f, tr[o++] = 0x59, tr[o++] = 0x05;
    uint32_t k = (uint32_t)(uintptr_t)&sky_k;
    memcpy(tr + o, &k, 4), o += 4;
    static const unsigned char store[5] = { 0xf3, 0x0f, 0x11, 0x45, 0x80 };    /* %xmm0,-0x80(%ebp) */
    memcpy(tr + o, store, 5), o += 5;
    emit_jmp(tr, &o, SKY_HEIGHT_CONT);
    if (patch_jmp(SKY_HEIGHT_SITE, tr, 7))
        gmv_log("[gamepatch] skybox raised with the camera (trampoline @ %p): the skyline's foot stays where the "
                "fogged sea ends\n", (void *)tr);
    else
        gmv_log("[gamepatch] write at 0x%08x failed (%lu); the skyline stays at sea level\n", SKY_HEIGHT_SITE, GetLastError());
}
