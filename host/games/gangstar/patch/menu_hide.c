#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x1691b0 0x659048 0x16e91c 0x171c00 0x1729da 0x2fdf80 */
#define MG_VT_SHOW     0x0065904cu
#define MG_SHOW_FN     0x0016e91cu
#define MG_VT_TEXTS    0x00659060u
#define MG_TEXTS_FN    0x00171c00u
#define MG_SET_VISIBLE 0x002fdf80u   /* +0xab */
#define MG_PRESS_JNE   0x00172bf1u
#define MG_RELEASE_JE  0x00172aceu
#define GL_PRESS_JNE   0x00172bb5u
#define GL_RELEASE_JNE 0x00172aaau

int mg_more, mg_live;
static int mg_hooked;
static int mg_more_hidden, mg_live_hidden;

static void mg_hide_one(void *fx, const char *path, int hide, int *hidden)
{
    if (hide || *hidden)
        ((void (*)(void *, const char *, int))(uintptr_t)MG_SET_VISIBLE)(fx, path, !hide);
    *hidden = hide;
}

static void mg_hide(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    if (!fx)
        return;
    mg_hide_one(fx, "widgetMainMenu.btnMore", mg_more, &mg_more_hidden);
    mg_hide_one(fx, "widgetMainMenu.btnGLLive", mg_live, &mg_live_hidden);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mg_show(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)MG_SHOW_FN)(w);
    mg_hide(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_mg_texts(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)MG_TEXTS_FN)(w);
    mg_hide(w);
}

int hide_main_menu_set(int more, int live)
{
    static const unsigned char more_press[2][2] = { { 0x75, 0x2b }, { 0xeb, 0x2b } };
    static const unsigned char more_release[2][2] = { { 0x74, 0xdc }, { 0x90, 0x90 } };
    static const unsigned char live_press[2][2] = { { 0x75, 0x26 }, { 0xeb, 0x26 } };
    static const unsigned char live_release[2][2] = { { 0x75, 0x0e }, { 0xeb, 0x0e } };
    const void *mp = (const void *)(uintptr_t)MG_PRESS_JNE, *mr = (const void *)(uintptr_t)MG_RELEASE_JE;
    const void *lp = (const void *)(uintptr_t)GL_PRESS_JNE, *lr = (const void *)(uintptr_t)GL_RELEASE_JNE;
    uint32_t s = (uint32_t)(uintptr_t)gmv_mg_show, t = (uint32_t)(uintptr_t)gmv_mg_texts;
    uint32_t vs = *(const uint32_t *)(uintptr_t)MG_VT_SHOW, vt = *(const uint32_t *)(uintptr_t)MG_VT_TEXTS;
    if ((mg_hooked ? vs != s || vt != t : vs != MG_SHOW_FN || vt != MG_TEXTS_FN) ||
        (memcmp(mp, more_press[0], 2) && memcmp(mp, more_press[1], 2)) ||
        (memcmp(mr, more_release[0], 2) && memcmp(mr, more_release[1], 2)) ||
        (memcmp(lp, live_press[0], 2) && memcmp(lp, live_press[1], 2)) ||
        (memcmp(lr, live_release[0], 2) && memcmp(lr, live_release[1], 2)))
        return -1;
    if (!mg_hooked) {
        if (!patch_bytes(MG_VT_SHOW, (const unsigned char *)&s, 4) || !patch_bytes(MG_VT_TEXTS, (const unsigned char *)&t, 4))
            return -2;
        mg_hooked = 1;
    }
    mg_more = more;
    mg_live = live;
    if (!patch_bytes(MG_PRESS_JNE, more_press[more != 0], 2) || !patch_bytes(MG_RELEASE_JE, more_release[more != 0], 2) ||
        !patch_bytes(GL_PRESS_JNE, live_press[live != 0], 2) || !patch_bytes(GL_RELEASE_JNE, live_release[live != 0], 2))
        return -2;
    return 1;
}

void hide_main_menu_unhook(void)
{
    uint32_t a = MG_SHOW_FN, b = MG_TEXTS_FN;
    if (!mg_hooked)
        return;
    patch_bytes(MG_VT_SHOW, (const unsigned char *)&a, 4);
    patch_bytes(MG_VT_TEXTS, (const unsigned char *)&b, 4);
    mg_hooked = mg_more = mg_live = 0;
}

void apply_hide_more_games(void)
{
    const char *e = gmv_getenv("GMV_HIDE_MORE_GAMES");
    const char *g = gmv_getenv("GMV_HIDE_GAMELOFT_LIVE");
    int more = gmv_env_on(e), live = gmv_env_on(g);
    if (!more && !live)
        return;
    int r = hide_main_menu_set(more, live);
    if (r == -1)
        gmv_log("[gamepatch] main menu not the expected bytes; the More Games corner and the Gameloft LIVE! badge stay\n");
    else if (r == -2)
        gmv_log("[gamepatch] write failed (%lu); the More Games corner / Gameloft LIVE! badge partly hidden\n", GetLastError());
    else {
        if (mg_more)
            gmv_log("[gamepatch] no More Games corner on the main menu (HIDE_MORE_GAMES=0 to show it)\n");
        if (mg_live)
            gmv_log("[gamepatch] no Gameloft LIVE! badge on the main menu (HIDE_GAMELOFT_LIVE=0 to show it)\n");
    }
}
