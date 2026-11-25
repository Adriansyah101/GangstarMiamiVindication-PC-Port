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

/* *0x949a60 **0x63e4cc *0x949a68 *0x949a6c 0x142690 */
#define SM_SUN_GLOW   0x00949a68u
#define SM_MOON_GLOW  0x00949a6cu
#define SM_DRAW_SUN   0x0064110fu
#define SM_DRAW_MOON  0x00641110u
#define SM_DISC_BYTES 0x1f0u
#define SM_DISC_OFS   0x1dc
#define SM_DISC_SIZE  1024.0f

const uint32_t sm_track_sites[] = { 0x00143520u, 0x00143552u };
static unsigned char *g_sm_failed;
int g_sm_on;
int g_sm_hooked;
int g_tod_ms = -1;
float g_sun_dir[3];
float g_moon_dir[3];
float g_cam_world[3];

typedef void (*sm_vfn_p)(void *, const void *);

void **sm_vt(void *obj)
{
    return *(void ***)obj;
}

/* 0x1aba04 0x1abc3d */
static int *sm_material(const char *tex)
{
    int *mat = NULL;
    __asm__ volatile(
        "subl $16, %%esp\n\t"
        "movl %0, (%%esp)\n\t"
        "movl %1, 4(%%esp)\n\t"
        "movl $8, 8(%%esp)\n\t"
        "movl $0x1aba04, %%eax\n\t"
        "call *%%eax\n\t"
        "addl $12, %%esp"
        :
        : "r"(&mat), "r"(tex)
        : "eax", "ecx", "edx", "memory", "cc");
    return mat;
}

/* iPhone 0x2bf3ac 0x1aba04 0x469c10 setMaterial 0x469bb0 */
static unsigned char *sm_make_disc(void *driver, void *parent, const char *tex)
{
    int *mat = sm_material(tex);
    if (!mat) {
        gmv_log("[sunmoon] no material for %s; that disc is left out\n", tex);
        return NULL;
    }
    unsigned char *n = ((unsigned char *(*)(uint32_t, const char *, int, int))(uintptr_t)0x904e0u)(
        SM_DISC_BYTES, "the patches", 107, 1);
    float pos[3] = {0, 0, 0}, size[2] = {SM_DISC_SIZE, SM_DISC_SIZE};
    ((void (*)(void *, void *, void *, const float *, const float *, int, int))(uintptr_t)0x469c10u)(
        n, driver, NULL, pos, size, -1, -1);
    float *ofs = (float *)(n + SM_DISC_OFS);
    ofs[0] = ofs[1] = 0;
    ofs[2] = -1000;
    ((void (*)(void *, int **))(uintptr_t)0x469bb0u)(n, &mat);
    if (--*mat == 0) {
        ((void (*)(int *))(uintptr_t)0x4da6d0u)(mat);
        ((void (*)(void *))(uintptr_t)0x90450u)(mat);
    }
    void *sn = n + 4;
    ((sm_vfn_i)sm_vt(sn)[0x48 / 4])(sn, 0);
    ((sm_vfn_i)sm_vt(sn)[0xd4 / 4])(sn, 100);
    ((sm_vfn_p)sm_vt(parent)[0x5c / 4])(parent, sn);
    return n;
}

static void sm_glow_show(uint32_t pp, int on)
{
    unsigned char *g = *(unsigned char **)(uintptr_t)pp;
    if (g)
        ((sm_vfn_i)sm_vt(g + 4)[0x48 / 4])(g + 4, on);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_tod(unsigned char *tod)
{
    unsigned char *sky = *(unsigned char **)(uintptr_t)SM_SKYBOX_PP;
    unsigned char *parent = sky ? *(unsigned char **)(sky + 0xd8) : NULL;
    unsigned char **sun = sky ? (unsigned char **)(sky + 0x118) : NULL, **moon = sky ? sun + 1 : NULL;
    if (g_sm_on && parent && sky != g_sm_failed && !*sun && !*moon && *(void **)(parent + 0xfc)) {
        void *driver = ((void *(*)(void))(uintptr_t)0x31d0u)();
        if (*(const unsigned char *)(uintptr_t)SM_DRAW_SUN)
            *sun = sm_make_disc(driver, parent, "./add-sun.tga");
        if (*(const unsigned char *)(uintptr_t)SM_DRAW_MOON)
            *moon = sm_make_disc(driver, parent, "./add-moon.tga");
        if (!*sun && !*moon)
            g_sm_failed = sky;
        gmv_trace("[sunmoon] skybox %p: sun disc %p, moon disc %p, under %p (its parent %p, scene manager %p)\n",
                (void *)sky, (void *)*sun, (void *)*moon, (void *)parent, *(void **)(parent + 0xd8),
                *(void **)(parent + 0xfc));
    }
    ((void (*)(void *))(uintptr_t)SM_TOD_FN)(tod);
    long long ms = *(long long *)(tod + 8) % 86400000;
    g_tod_ms = (int)(ms < 0 ? ms + 86400000 : ms);
    if (!g_sm_on)
        return;
    int night = *(int *)(tod + 4) == 3;
    sm_glow_show(SM_SUN_GLOW, !night);
    sm_glow_show(SM_MOON_GLOW, night);
}

/* 0x142690 */
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_track(unsigned char *smgr, unsigned char *disc, unsigned char *anchor, void *glow)
{
    ((void (*)(void *, void *, void *, void *))(uintptr_t)SM_TRACK_FN)(smgr, NULL, anchor, glow);
    unsigned char *dummy = anchor ? *(unsigned char **)(anchor + 0xd8) : NULL;
    unsigned char *cam = *(unsigned char **)(smgr + 0xe4);
    if (dummy && glow && glow == *(void **)(uintptr_t)SM_SUN_GLOW)
        memcpy(g_sun_dir, dummy + 0x40, sizeof g_sun_dir);
    if (dummy && glow && glow == *(void **)(uintptr_t)SM_MOON_GLOW)
        memcpy(g_moon_dir, dummy + 0x40, sizeof g_moon_dir);
    if (cam)
        memcpy(g_cam_world, cam + 0x40, sizeof g_cam_world);
    if (disc && !g_sm_on) {
        ((float *)(disc + SM_DISC_OFS))[2] = -1000;
        return;
    }
    if (!disc || !dummy || !cam)
        return;
    float d[3], c[3];
    memcpy(d, dummy + 0x40, sizeof d);
    memcpy(c, cam + 0x40, sizeof c);
    float scale = ((const float *(*)(void *))sm_vt(dummy)[0x90 / 4])(dummy)[0];
    float l = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (l != 0.0f) {
        l = 1.0f / sqrtf(l);
        d[0] *= l, d[1] *= l, d[2] *= l;
    }
    d[0] *= 1000.0f, d[1] *= 1000.0f, d[2] *= 1000.0f;
    memcpy(disc + SM_DISC_OFS, d, sizeof d);
    float p[3] = {d[0] + c[0], d[1] + c[1], d[2] + c[2]};
    ((sm_vfn_p)sm_vt(disc + 4)[0xa4 / 4])(disc + 4, p);
    float size[2] = {SM_DISC_SIZE * scale, SM_DISC_SIZE * scale};
    ((sm_vfn_p)sm_vt(disc)[8 / 4])(disc, size);
}

extern int g_sky_glow_draw;

static int sm_is_glow(const unsigned char *n)
{
    return n == *(unsigned char **)(uintptr_t)SM_SUN_GLOW || n == *(unsigned char **)(uintptr_t)SM_MOON_GLOW;
}

/* 0x46afdb */
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render(unsigned char *n, int draw)
{
    g_sky_glow_draw = g_sm_on && sm_is_glow(n);
    ((void (*)(void *, int))(uintptr_t)SM_BB_RENDER)(n, draw);
    g_sky_glow_draw = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render_sn(unsigned char *sn, int draw)
{
    g_sky_glow_draw = g_sm_on && sm_is_glow(sn - 4);
    ((void (*)(void *, int))(uintptr_t)SM_BB_RENDER_THUNK)(sn, draw);
    g_sky_glow_draw = 0;
}

/* [+0x54c] [+0x550] iPhone 0x2bfef4 0xbe9f1 -> 0xbea57 */
#define SM_FARCITY_SITE 0x000be9f1u
#define SM_DISCS_END    0x000bea57u

static const unsigned char sm_farcity_code[] = {   /* 0x550(%esi),%eax */
    0x8b, 0x86, 0x50, 0x05, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x10, 0x8b, 0x08, 0x89, 0x04, 0x24,
    0xc7, 0x44, 0x24, 0x04, 0x01, 0x00, 0x00, 0x00, 0xff, 0x51, 0x1c,
};

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_sun_moon_layers(unsigned char *sky)
{
    for (int i = 0; i < 2; i++) {
        unsigned char *disc = *(unsigned char **)(sky + 0x118 + 4 * i);
        if (disc && (disc[0x10c] & 1))
            ((sm_vfn_i)sm_vt(disc)[0x20 / 4])(disc, 1);
    }
    void *city = *(void **)(sky + 0x550);
    if (city)
        ((sm_vfn_i)sm_vt(city)[0x1c / 4])(city, 1);
}

void sm_discs_behind_farcity(void)
{
    unsigned char code[11] = {0x56, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x04, 0xeb, 0};  /* push %esi; call; add $4,%esp; jmp */
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)gmv_sun_moon_layers - (SM_FARCITY_SITE + 6));
    memcpy(code + 2, &rel, 4);
    code[10] = (unsigned char)(SM_DISCS_END - (SM_FARCITY_SITE + sizeof code));
    if (memcmp((const void *)(uintptr_t)SM_FARCITY_SITE, sm_farcity_code, sizeof sm_farcity_code) != 0)
        gmv_log("[gamepatch] skybox layer draws not the expected bytes; the discs stay in front of the skyline\n");
    else if (patch_bytes(SM_FARCITY_SITE, code, sizeof code))
        gmv_log("[gamepatch] sun and moon discs drawn behind the distant skyline\n");
}

int sm_is_call(uint32_t site, uint32_t fn)
{
    return code_call_target(site) == fn;
}

int sm_redirect(uint32_t site, void *hook)
{
    if (patch_call(site, hook))
        return 1;
    gmv_log("[gamepatch] write at 0x%08x failed (%lu); sun and moon partly applied\n", site + 1, GetLastError());
    return 0;
}

int tab_live(void)
{
    return gmv_config_tab();
}

int tables_live(void)
{
    return tab_live() && !gmv_config_mac();
}

int bloom_clock_on(void)
{
    const char *b = gmv_getenv("GMV_BLOOM");
    return (b && atoi(b) > 0) || tab_live();
}

static int sm_mode(void)
{
    return !g_sm_on ? 0 : g_sky_glow_iphone ? 2 : 1;
}

void sm_mode_set(int mode)
{
    g_sm_on = mode != 0;
    g_sky_glow_iphone = mode == 2;
}

int sun_moon_set(int mode)
{
    if (!g_sm_hooked)
        return -1;
    if (sm_mode() != mode)
        gmv_log("[gamepatch] sun and moon %s\n", mode == 2 ? "as on the iPhone" : mode ? "enhanced" : "off");
    sm_mode_set(mode);
    return mode;
}
