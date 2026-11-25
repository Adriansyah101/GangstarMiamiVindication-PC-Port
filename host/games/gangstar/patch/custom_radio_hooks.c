#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0..[mm+0x270]-1 0x60156 0x60122 0x6000c 0x121 0x5f3e8 */
#define CR_COUNT(mm)     (*(int *)((mm) + 0x270))

static const uint32_t cr_pick_sites[] = { 0x000601cdu, 0x000605bau };
static uint32_t g_cr_pick_next;      /* 0x5f3e8 */
int g_cr_hooked, g_cr_on;
int g_cr_row_want, g_cr_row;

/* [mm+0x270] */
static void cr_count_sync(unsigned char *mm)
{
    int want = g_cr_on && gmv_cr_songs() > 0 ? CR_GAME_STATIONS + 1 : CR_GAME_STATIONS;
    if (CR_COUNT(mm) == CR_GAME_STATIONS || CR_COUNT(mm) == CR_GAME_STATIONS + 1)
        CR_COUNT(mm) = want;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_cr_pick(unsigned char *mm, int station)
{
    cr_count_sync(mm);
    if (station >= CR_GAME_STATIONS) {
        if (station == GMV_CR_STATION && CR_COUNT(mm) > GMV_CR_STATION)
            return -1;
        station = 0;
        *(int *)(mm + 0x21c) = 0;
    }
    return ((int (*)(void *, int))(uintptr_t)g_cr_pick_next)(mm, station);
}

static int cr_hook(void)
{
    if (g_cr_hooked)
        return 1;
    const uint32_t mine = (uint32_t)(uintptr_t)gmv_cr_pick, trace = (uint32_t)(uintptr_t)gmv_radio_pick;
    uint32_t next = 0;
    for (size_t i = 0; i < sizeof cr_pick_sites / sizeof *cr_pick_sites; i++) {
        uint32_t s = cr_pick_sites[i];
        uint32_t to = sm_is_call(s, RADIO_PICK_FN) ? RADIO_PICK_FN : sm_is_call(s, trace) ? trace : 0;
        if (!to || (next && to != next))
            return 0;
        next = to;
    }
    g_cr_pick_next = next;
    for (size_t i = 0; i < sizeof cr_pick_sites / sizeof *cr_pick_sites; i++) {
        if (!patch_call(cr_pick_sites[i], (const void *)(uintptr_t)mine))
            return -1;
    }
    g_cr_hooked = 1;
    return 1;
}

static int custom_radio_set(int on)
{
    if (cr_hook() != 1)
        return 0;
    if (on != g_cr_on)
        gmv_log("[radio] CUSTOM_RADIO %s\n", on ? "on" : "off");
    g_cr_on = on;
    gmv_cr_enable(on);
    unsigned char *mm = radio_mm();
    if (mm) {
        if (!on && (*(uint32_t *)(mm + 0x220) & 1) && *(int *)(mm + 0x21c) >= CR_GAME_STATIONS)
            ((void (*)(void *, int))(uintptr_t)RADIO_TUNE_FN)(mm, 0);
        cr_count_sync(mm);
    }
    return 1;
}

/* SOUND 0x6593c8 0x178cf4 0x17860c */
#define CR_VOL_VT      0x006593c8u
#define CR_VOL_EVENT   0x0017860cu
#define CR_VOL_TEXTS   0x00178cf4u
#define CR_EV_PATH     0x005edb94u
#define CR_GOTO_FRAME  0x002fd810u
#define CR_GOTO_LABEL  0x002fd8a0u
#define CR_SET_TEXT    0x002fdea0u
#define CR_FIND        0x002fd160u
#define CR_SLIDER_FRAMES 0x005f1518u
#define CR_SLIDER_ADD1 0x005f6940u
#define CR_SLIDER_ADD2 0x005f4be8u

#define CR_MOVE        0x002fdf30u

static const struct { const char *name; int32_t was, now; } cr_rows[6] = {
    {"txtSFX", 203, CR_TITLE0}, {"barSFX", 732, CR_BAR0},
    {"txtVFX", 2180, CR_TITLE0 + CR_STEP}, {"barVFX", 2699, CR_BAR0 + CR_STEP},
    {"txtMusic", 4150, CR_TITLE0 + 2 * CR_STEP}, {"barMusic", 4671, CR_BAR0 + 2 * CR_STEP},
};

static float g_cr_vol = 1.0f;
static float g_cr_unmuted = 1.0f;

static void cr_row_visible(void *fx, const char *name, int on)
{
    char path[64];
    snprintf(path, sizeof path, "widgetVolumes.%s", name);
    ((void (*)(void *, const char *, int))(uintptr_t)CR_VISIBLE)(fx, path, on);
}

static void cr_rows_place(void *fx, int mac)
{
    char path[64];
    for (size_t i = 0; i < sizeof cr_rows / sizeof *cr_rows; i++) {
        snprintf(path, sizeof path, "widgetVolumes.%s", cr_rows[i].name);
        unsigned char *ch = ((unsigned char *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, path);
        float *m = ch ? *(float **)(ch + 0x5c) : NULL, y = (float)(mac ? cr_rows[i].was : cr_rows[i].now);
        if (!m || m[5] == y)
            continue;
        float x = m[2];
        ((void (*)(void *, const char *, int, int))(uintptr_t)CR_MOVE)(fx, path, (int)(x / 20), (int)(y / 20));
        if ((m = *(float **)(ch + 0x5c)) != NULL)
            m[2] = x, m[5] = y;
    }
}

static const char *const cr_parts[7] = {"txtRadio", "barRadio", "txtRadioOn", "crValueBar", "btnCrLeft",
                                        "btnCrRight", "crTextValue"};
static void *g_cr_fx[2];

static void cr_extras_place(void *fx, int show)
{
    char path[64];
    for (size_t i = 0; i < sizeof cr_parts / sizeof *cr_parts; i++) {
        snprintf(path, sizeof path, "widgetVolumes.%s", cr_parts[i]);
        unsigned char *ch = ((unsigned char *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, path);
        float *m = ch ? *(float **)(ch + 0x5c) : NULL;
        if (!m || (m[5] < CR_OFF / 2) == show)
            continue;
        float x = m[2], y = m[5] + (show ? -CR_OFF : CR_OFF);
        ((void (*)(void *, const char *, int, int))(uintptr_t)CR_MOVE)(fx, path, (int)(x / 20), (int)(y / 20));
        if ((m = *(float **)(ch + 0x5c)) != NULL)
            m[2] = x, m[5] = y;
    }
}

void cr_page_mac(void)
{
    for (int i = 0; i < 2; i++)
        if (g_cr_fx[i]) {
            cr_rows_place(g_cr_fx[i], 1);
            cr_extras_place(g_cr_fx[i], 0);
        }
}

static void cr_row_texts(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    int show = !gmv_config_mac();
    if (!fx || !g_cr_row)
        return;
    if (g_cr_fx[0] != fx && g_cr_fx[1] != fx)
        g_cr_fx[!!g_cr_fx[0]] = fx;
    cr_rows_place(fx, !show);
    cr_extras_place(fx, show);
    for (size_t i = 0; i < sizeof cr_parts / sizeof *cr_parts; i++)
        cr_row_visible(fx, cr_parts[i], show);
    if (!show)
        return;
    ((void (*)(void *, const char *, int, int))(uintptr_t)CR_GOTO_FRAME)(
        fx, "widgetVolumes.barRadio.btnVolume", (int)(g_cr_vol * *(const float *)(uintptr_t)CR_SLIDER_FRAMES), 0);
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_GOTO_LABEL)(
        fx, "widgetVolumes.barRadio.btnMute", g_cr_vol > 0 ? "on" : "off", 0);
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_SET_TEXT)(
        fx, "widgetVolumes.crTextValue", g_cr_on ? "ON" : "OFF", 1);
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_SET_TEXT)(
        fx, "widgetVolumes.txtRadio", "Custom Radio Volume", 1);
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)CR_SET_TEXT)(
        fx, "widgetVolumes.txtRadioOn", "Custom Radio", 1);
    static int told;
    if (!told++)
        gmv_trace("[gamepatch] SOUND page: txtRadio %p, txtRadioOn %p, btnCrLeft %p (txtMusic %p)\n",
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.txtRadio"),
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.txtRadioOn"),
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.btnCrLeft"),
                ((void *(*)(void *, const char *))(uintptr_t)CR_FIND)(fx, "widgetVolumes.txtMusic"));
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cr_vol_texts(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)CR_VOL_TEXTS)(w);
    cr_row_texts(w);
}

static void cr_save(void)
{
    char b[16];
    snprintf(b, sizeof b, "%.2f", (double)g_cr_vol);
    gmv_config_save("CUSTOM_RADIO", g_cr_on ? "1" : "0");
    gmv_config_save("CUSTOM_RADIO_VOLUME", b);
}

static void cr_set_volume(float v)
{
    g_cr_vol = v < 0 ? 0.0f : v > 1 ? 1.0f : v;
    if (g_cr_vol > 0.001f)
        g_cr_unmuted = g_cr_vol;
    gmv_cr_set_volume(g_cr_vol);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cr_vol_event(unsigned char *w, uint32_t *ev)
{
    const char *path = ((const char *(*)(void *))(uintptr_t)CR_EV_PATH)(ev);
    const char *name = (const char *)(uintptr_t)ev[1];
    int kind = (int)ev[2];
    /* barRadio 0x16e9e8 */
    if (!g_cr_row || gmv_config_mac() || !path || !name ||
        (!strstr(path, "barRadio") && strcmp(name, "btnCrLeft") && strcmp(name, "btnCrRight"))) {
        ((void (*)(void *, void *))(uintptr_t)CR_VOL_EVENT)(w, ev);
        return;
    }
    if (!strcmp(name, "btnVolume") && kind >= 3 && kind <= 5) {
        float r[8] = {0}, x;
        void *ch = (void *)(uintptr_t)ev[0];
        ((void (*)(void *, float *))(uintptr_t)(*(uint32_t **)ch)[0x12c / 4])(ch, r);
        memcpy(&x, &ev[3], 4);
        cr_set_volume(r[1] > r[0] ? x / (r[1] - r[0]) + *(const float *)(uintptr_t)CR_SLIDER_ADD1 +
                                        *(const float *)(uintptr_t)CR_SLIDER_ADD2 : 0.0f);
        if (kind != 5)
            cr_save();
    } else if (!strcmp(name, "btnMute") && kind == 6) {
        cr_set_volume(g_cr_vol > 0.001f ? 0.0f : g_cr_unmuted);
        cr_save();
    } else if ((!strcmp(name, "btnCrLeft") || !strcmp(name, "btnCrRight")) && kind == 6) {
        om_sound(0x56);
        custom_radio_set(!g_cr_on);
        cr_save();
    } else if (!(!strcmp(name, "btnVolume") && kind == 1))
        return;
    *((unsigned char *)ev + 0x24) = 1;
    gmv_cr_vol_texts(w);
}

static int g_cr_logo_hide;

void cr_mac_switch(int mac)
{
    g_cr_logo_hide = mac;
    if (!g_cr_hooked)
        return;
    const char *f = gmv_config_file_value("CUSTOM_RADIO");
    custom_radio_set(!mac && gmv_env_on(f));
    if (mac) {
        unsigned char *mm = radio_mm();
        if (mm && *(int *)(mm + 0x21c) >= CR_GAME_STATIONS)
            *(int *)(mm + 0x21c) = 0;
        cr_cars_off_sixth();
    }
}

/* HUD 0x17357e [widget+0x14] widget+0x1c iPod 0x17b7c4 */
#define CR_STR_CSTR 0x005edc48u
#define CR_PLAY_LIST "IG_HUD_Radio.radioON.play_list"

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_cr_logo(unsigned char *w, int station)
{
    typedef void (*vis_fn)(void *, const char *, int);
    *(int *)(w + 0x14) = station;
    for (int i = 0; i < CR_GAME_STATIONS; i++) {
        const char *path = ((const char *(*)(void *))(uintptr_t)CR_STR_CSTR)(w + 0x1c + 4 * i);
        ((vis_fn)(uintptr_t)CR_VISIBLE)(*(void **)(w + 8), path, i == station);
    }
    ((vis_fn)(uintptr_t)CR_VISIBLE)(*(void **)(w + 8), CR_PLAY_LIST, station == GMV_CR_STATION);
}

void cr_logo_hide_tick(void)
{
    if (!g_cr_logo_hide)
        return;
    unsigned char *app = ((unsigned char *(*)(void))(uintptr_t)RADIO_APP_FN)();
    unsigned char *hud = app && *(int *)(app + 0x218) > 0x34 ? *(unsigned char **)(app + 0x214) : NULL;
    unsigned char *w = hud ? *(unsigned char **)(hud + 0xd0) : NULL;
    if (w) {
        ((void (*)(void *, const char *, int))(uintptr_t)CR_VISIBLE)(*(void **)(w + 8), CR_PLAY_LIST, 0);
        g_cr_logo_hide = 0;
    }
}

static const unsigned char cr_logo_fn_was[5] = {0x55, 0x89, 0xe5, 0x53, 0x57};     /* push ebp; mov ebp,esp; push ebx; push edi */

static int cr_logo_hook(void)
{
    if (memcmp((const void *)(uintptr_t)RADIO_LOGO_FN, cr_logo_fn_was, sizeof cr_logo_fn_was))
        return 0;
    return patch_jmp(RADIO_LOGO_FN, (void *)gmv_cr_logo, 5);
}

void cr_file_wants(void)
{
    const uint32_t *vt = (const uint32_t *)(uintptr_t)CR_VOL_VT;
    if (gmv_config_mac() && !tab_live())
        return;
    if (vt[5] == CR_VOL_EVENT && vt[6] == CR_VOL_TEXTS && om_opener_hook())
        g_cr_row_want = 1;
    else
        gmv_log("[gamepatch] the SOUND page's widget is not the expected one; no Custom Radio row "
                "(CUSTOM_RADIO in GangstarMV.ini still works)\n");
}

void apply_custom_radio(void)
{
    const char *e = gmv_getenv("GMV_CUSTOM_RADIO"), *v = gmv_getenv("GMV_CUSTOM_RADIO_VOLUME");
    int want = gmv_env_on(e);
    if (gmv_config_mac())
        return;
    cr_set_volume(v && *v ? (float)atof(v) : 1.0f);
    int r = cr_hook();
    if (r != 1) {
        gmv_log(r ? "[gamepatch] a write failed (%lu); no custom radio station\n"
                  : "[gamepatch] the radio's song-pick calls are not the expected ones (%lu); no custom radio station\n",
                GetLastError());
        return;
    }
    if (g_cr_row_want) {
        uint32_t ev = (uint32_t)(uintptr_t)gmv_cr_vol_event, tx = (uint32_t)(uintptr_t)gmv_cr_vol_texts;
        patch_bytes(CR_VOL_VT + 5 * 4, (const unsigned char *)&ev, 4);
        patch_bytes(CR_VOL_VT + 6 * 4, (const unsigned char *)&tx, 4);
    }
    if (!cr_logo_hook())
        gmv_log("[gamepatch] the HUD radio's logo code is not the expected one; no logo for the sixth station\n");
    if (want)
        custom_radio_set(1);
    gmv_log("[gamepatch] CUSTOM_RADIO %s: a sixth radio station for the songs in the \"custom radio\" folder, "
            "switched on the SOUND page (Custom Radio)\n", want ? "on" : "off");
}
