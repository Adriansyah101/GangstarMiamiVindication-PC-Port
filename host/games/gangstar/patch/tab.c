#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x6585e0 +0x14 0x172f0e 0x171a08 0x8f426 0x166fe2 */
#define OM_LINE_TEXT   0x0015c0f8u
#define OM_EVENT_PATH  0x00180878u
#define OM_LABEL_SITE  0x00171b07u
#define OM_HIDE6_SITE  0x0016715eu
#define OM_SLOT        6
#define OM_CLICK_SITE  0x00167208u   /* call 0x16eff4 */
#define OM_TABLE_CMP   0x0016720du   /* cmp eax,3 */
#define OM_REG_SITE    0x001609c4u
#define OM_MGR_FN      0x0015e4fau   /* +0x214 +0x218 */
#define OM_PAGES       0x0016f698u   /* +0x18 */
#define OM_OPT_SHOW    0x00166fe2u
#define OM_OPT_EVENT   0x00167192u
#define OM_GS_CTOR     0x0016c044u
#define OM_GS_BYTES    0x90u
#define OM_GS_VT       0x00658ca8u
#define OM_GS_HIDE     0x0016990au
#define OM_SOUND_FN    0x0005fd5au   /* 0x5a 0x56 0x57 OK */
#define OM_LOCK        0x0068a544u
#define OM_LIST_VT     0x00658fa8u
#define OM_LIST_SLOTS  10
#define OM_LIST_SHOW   0x00170c04u
#define OM_LIST_HIDE   0x00170d68u
#define OM_LIST_EVENT  0x00170df6u
#define OM_LIST_TEXTS  0x00171430u
#define OM_LIST_RESET  0x0017f4a6u
#define OM_APP_FN      0x000031b0u
#define OM_RES_NOW     0x00003850u
#define OM_RES_SET     0x00003a24u
#define OM_RES_COUNT   0x17afc
#define OM_FULL_NOW    0x0019b920u
#define OM_W_LANGUAGE  9
#define OM_W_ROW       0x1d
#define OM_W_LIST      0x24
#define OM_TAB_ID      0x7f00
#define OM_STATE       "OptionGamepatch"

enum { OM_RESTART, OM_LIVE, OM_RESOLUTION, OM_FULL_SCREEN, OM_JOIN };

#define OM_VALS 12

typedef struct {
    const char *key, *title;
    const char *blank, *unit;
    int live;
    const char *val[OM_VALS], *name[OM_VALS];
    int (*set)(const char *key, double v);
} om_item;

static int om_set_radio_songs(const char *key, double v);
static int om_set_uc(const char *key, double v);

static int om_set_msaa(const char *key, double v)
{
    (void)key;
    return gmv_set_msaa((int)v);
}

static int om_set_aniso(const char *key, double v)
{
    (void)key;
    return gmv_set_aniso((float)v);
}

static int om_set_frame_ms(const char *key, double v)
{
    (void)key;
    if (fabs(v - 1000.0 / 60.0) < 0.002)
        v = 1000.0 / 60.0;
    gmv_set_frame_ms(v);
    physics_sync_set(v);
    cam_sync_set(v);
    walk_sync_set(v);
    ai_sync_set(v);
    return 1;
}

static int om_set_vsync(const char *key, double v)
{
    (void)key;
    return gmv_set_swap_interval((int)v);
}

static int om_set_fix(const char *key, double v)
{
    int on = v != 0;
    if (!strcmp(key, "MENUFLASH_FIX")) {
        gmv_set_menuflash_fix(on);
        return 1;
    }
    return !strcmp(key, "LOAD_SLEEP_SKIP") ? load_sleep_skip_set(on) == on
         : !strcmp(key, "MENU_FADE_CLICKS") ? menu_fade_clicks_set(on) == on
         : !strcmp(key, "LOAD_BUDGET") ? load_budget_set((int)v) == (int)v
         : !strcmp(key, "RADIO_LOGO_FIX") ? radio_logo_set(on) == on
         : !strcmp(key, "STOPLOCK_FIX") ? stoplock_set(on) == on
         : !strcmp(key, "AI_STEERING_FIX") ? ai_steer_set(on) == on
         : !strcmp(key, "BOAT_SEAT_FIX") ? boat_seat_set(on) == on
         : !strcmp(key, "FADE_EDGE_FIX") ? fade_edge_set(on) == on
         : !strcmp(key, "HIDE_MORE_GAMES") ? hide_main_menu_set(on, mg_live) == 1
         : !strcmp(key, "HIDE_GAMELOFT_LIVE") ? hide_main_menu_set(mg_more, on) == 1
         : !strcmp(key, "CUTSCENE_KEYS") ? cs_set(on)
         : !strcmp(key, "TUTORIAL_KEYS") ? tk_set(on)
         : !strcmp(key, "PAD_LAYOUT") ? pl_set(on)
         : !strcmp(key, "PAD_MENU") ? pm_set(on)
         : !strcmp(key, "PAD_HINTS") ? ph_set(on)
         : !strcmp(key, "PAD_ICONS") ? gmv_padicons_on()
         : !strcmp(key, "KEY_MOVE") ? km_set(on)
         : !strcmp(key, "SWITCH_SAVE_FIX") ? ssf_set(on) : !strcmp(key, "PLAYERSWITCH_INVENTORY") ? si_set(on)
         : !strcmp(key, "LC_SWITCH") ? lc_set(on) : 0;
}

static int om_set_graphics(const char *key, double v)
{
    if (!strcmp(key, "SUN_MOON"))
        return sun_moon_set((int)v) == (int)v;
    return gmv_set_post(key, (float)v);
}

static int om_set_dd(const char *key, double v)
{
    (void)key;
    return dd_set(v);
}

static int om_set_mac(const char *key, double v);

#define OM_OFF_ON {"0", "1"}, {"OFF", "ON"}
#define OM_DENSITIES {"0", "0.5", "1", "1.5", "2", "2.5", "3", "3.5", "4"}, \
                     {"NONE", "0.5X", "1X (MAC)", "1.5X", "2X", "2.5X", "3X", "3.5X", "4X"}

static const om_item om_items[] = {
    {NULL, "RESOLUTION", "", "", OM_RESOLUTION, {NULL}, {NULL}, NULL},
    {NULL, "FULL SCREEN", "", "", OM_FULL_SCREEN, OM_OFF_ON, NULL},
    {"SWAP_INTERVAL", "VSYNC", "0", "", OM_LIVE, OM_OFF_ON, om_set_vsync},
    {"MSAA,ALPHA_TO_COVERAGE", "ANTI-ALIASING", "", "", OM_JOIN,
     {"0,0", "auto,0", "2,0", "4,0", "4,1", "8,0"}, {"OFF", "AUTO", "2X", "4X", "4X + A2C", "8X"}, NULL},
    {"ANISOTROPIC_FILTERING", "TEXTURE FILTERING", "0", "X", OM_LIVE,
     {"0", "2", "4", "8", "16"}, {"MAC", "2X", "4X", "8X", "16X"}, om_set_aniso},
    {"FRAME_MS", "FPS LIMITER", "16.667", " MS", OM_LIVE,
     {"20", "", "8.3333", "6.6667", "5.5556", "4.1667", "3.4722", "3.0303", "2.0833", "1.3889", "1.25"},
     {"25", "30 (MAC)", "60", "75", "90", "120", "144", "165", "240", "360", "400"}, om_set_frame_ms},
    {"FOG_MODE", "FOG", "0", "", OM_LIVE, {"0", "1", "2"}, {"MAC", "LINEAR", "HYBRID"}, om_set_graphics},
    {"DRAW_DISTANCE", "DRAW DISTANCE", "1", "X", OM_LIVE,
     {"1", "1.5", "2", "2.5", "3", "4"}, {"1X (MAC)", "1.5X", "2X", "2.5X", "3X", "4X"}, om_set_dd},
    {"TRAFFIC_DENSITY", "TRAFFIC", "1", "X", OM_LIVE, OM_DENSITIES, om_density},
    {"PEDESTRIAN_DENSITY", "PEDESTRIANS", "1", "X", OM_LIVE, OM_DENSITIES, om_density},
    {"PARKED_CAR_DENSITY", "PARKED CARS", "1", "X", OM_LIVE, OM_DENSITIES, om_density},
    {"AMBIENT_OCCLUSION", "AMBIENT OCCLUSION", "0", "", OM_LIVE, OM_OFF_ON, om_set_graphics},
    {"BLOOM", "BLOOM", "0", "", OM_LIVE, {"0", "1", "2"}, {"OFF", "LOW", "HIGH"}, om_set_graphics},
    {"BLOOM_THRESHOLD", "BLOOM INTENSITY", "0.75", "", OM_LIVE, {"0.75", "0.5"}, {"LOW", "HIGH"}, om_set_graphics},
    {"WATER_REFLECTIONS", "WATER REFLECTIONS", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "ENHANCED"}, om_set_graphics},
    {"VEHICLE_REFLECTIONS", "CAR REFLECTIONS", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "ENHANCED"}, om_set_graphics},
    {"SUN_MOON", "SUN AND MOON", "0", "", OM_LIVE, {"0", "2", "1"}, {"MAC", "IPHONE", "ENHANCED"}, om_set_graphics},
    {"DYNAMIC_SHADOWS,SHADOW_QUALITY", "DYNAMIC SHADOWS", "", "", OM_JOIN,
     {"0,", "1,auto", "1,1", "1,2"}, {"OFF", "AUTO", "LOW", "HIGH"}, NULL},
    {"SPAWN_FADE", "FADE IN TRAFFIC", "0", "", OM_LIVE, OM_OFF_ON, om_set_spawn_fade},
    {"POST_AA", "POST AA", "0", "", OM_LIVE, {"auto", "0", "1"}, {"AUTO", "OFF", "FXAA"}, om_set_graphics},
    {"ORIGINAL_MAC_EXPERIENCE", "ORIGINAL MAC EXPERIENCE", "0", "", OM_LIVE, OM_OFF_ON, om_set_mac},
    {"ALLOW_WEAPON_SPRINT", "WEAPON WHILE SPRINTING", "0", "", OM_LIVE, OM_OFF_ON, om_set_sprint},
    {"MINIMAP_SHOPS", "SHOPS ON MINIMAP", "0", "", OM_LIVE, OM_OFF_ON, om_set_minimap},
    {"MISSION_REPLAY", "MISSION REPLAY", "0", "", OM_LIVE, OM_OFF_ON, om_set_replay},
    {"TIME_OF_DAY", "TIME OF DAY TAB", "1", "", OM_LIVE, OM_OFF_ON, om_set_replay},
    {"RANDOM_TIME_OF_DAY", "RANDOM START TIME", "1", "", OM_LIVE, OM_OFF_ON, om_set_tod_start},
    {"PAD_LAYOUT", "PAD LAYOUT", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "GTA-LIKE"}, om_set_fix},
    {"PAD_MENU,PAD_HINTS,PAD_ICONS", "GAMEPAD UI & ICONS", "", "", OM_JOIN, OM_OFF_ON, NULL},
    {"KEY_MOVE", "KEYBOARD MOVEMENT", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "ENHANCED"}, om_set_fix},
    {"PLAYERSWITCH_INVENTORY", "PLAYERSWITCH INVENTORY", "0", "", OM_LIVE, {"0", "1"}, {"MAC", "SHARED"}, om_set_fix},
    {"LC_SWITCH", "PLAY AS L.C.", "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"UC_CRUSHER,RADIO_SONGS", "RESTORED ADD-ON CONTENT", "", "", OM_JOIN, OM_OFF_ON, NULL},
    {"RADIO_LOGO_FIX,MENUFLASH_FIX,FADE_EDGE_FIX,SWITCH_SAVE_FIX,AI_STEERING_FIX,BOAT_SEAT_FIX,MENU_FADE_CLICKS",
     "ORIGINAL BUG FIXES", "", "", OM_JOIN, OM_OFF_ON, NULL},
    {"HIDE_MORE_GAMES,HIDE_GAMELOFT_LIVE", "HIDE DEAD ONLINE MENUS", "", "", OM_JOIN, OM_OFF_ON, NULL},
    {"NO_INTRO", "SKIP INTRO MOVIE", "0", "", OM_RESTART, OM_OFF_ON, NULL},
    {"SHORT_WARNING", "SHORT START-UP WARNING", "0", "", OM_RESTART, OM_OFF_ON, NULL},
    {"FAST_LOAD", "FAST START-UP LOADING", "1", "", OM_RESTART, OM_OFF_ON, NULL},
    {"CUTSCENE_KEYS,TUTORIAL_KEYS", "CUTSCENE & POPUP KEYS", "", "", OM_JOIN, OM_OFF_ON, NULL},
    {"MSAA", NULL, "auto", "X", OM_LIVE, {"auto", "0", "2", "4", "8"}, {NULL}, om_set_msaa},
    {"ALPHA_TO_COVERAGE", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_graphics},
    {"DYNAMIC_SHADOWS", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_graphics},
    {"SHADOW_QUALITY", NULL, "auto", "", OM_LIVE, {"auto", "1", "2"}, {NULL}, om_set_graphics},
    {"PAD_MENU", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"PAD_HINTS", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"PAD_ICONS", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"UC_CRUSHER", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_uc},
    {"RADIO_SONGS", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_radio_songs},
    {"LOAD_SLEEP_SKIP", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"LOAD_BUDGET", NULL, "200", "", OM_LIVE, {"0", "200"}, {NULL}, om_set_fix},
    {"STOPLOCK_FIX", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"RADIO_LOGO_FIX", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"MENUFLASH_FIX", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"FADE_EDGE_FIX", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"SWITCH_SAVE_FIX", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"AI_STEERING_FIX", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"BOAT_SEAT_FIX", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"MENU_FADE_CLICKS", NULL, "1", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"HIDE_MORE_GAMES", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"HIDE_GAMELOFT_LIVE", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"CUTSCENE_KEYS", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
    {"TUTORIAL_KEYS", NULL, "0", "", OM_LIVE, OM_OFF_ON, om_set_fix},
};
#define OM_N (sizeof om_items / sizeof *om_items)
#define OM_K_RES  0
#define OM_K_FULL 1
#define OM_NO_ADDON "NO ADD ON"

typedef struct { const char *name, *from; int first, n, page1; } om_category;

static om_category om_cats[] = {
    {"DISPLAY", "RESOLUTION", 0, 0, 0}, {"GRAPHICS", "FOG", 0, 0, 0},
    {"FIXES", "ORIGINAL MAC EXPERIENCE", 0, 0, 0}, {"MENU", "HIDE DEAD ONLINE MENUS", 0, 0, 0},
};
#define OM_CATS (int)(sizeof om_cats / sizeof *om_cats)
static om_category om_mac_cat = {"PRESET", NULL, 0, 1, 0};
static size_t om_rows, om_k_mac;

typedef struct { int slot, first, n; } om_tab;

static const om_tab om_tabs[] = { {3, 0, 2}, {OM_SLOT, 2, 2} };
#define OM_TABS (int)(sizeof om_tabs / sizeof *om_tabs)

static uint32_t om_vt[OM_VT_N];
static uint32_t om_cat_vt[2 + OM_CAT_SLOTS], om_list_vt[2 + OM_LIST_SLOTS];
static double om_now[OM_N];
static int om_sel[OM_N];
static int om_later[OM_N];
static int om_res_now, om_full_now;
static int om_mac;
static int om_tab_now;
static int om_tab_cat[OM_TABS];
static int om_active;
static int om_list_keys;
static int om_list_ours;
int om_tab_files;
int om_credit;
int om_selectors;
static int om_foot;
void *(*om_opener)(const char *);
const char *const om_sel_names[4] = {"wValueBar", "btnValueLeft", "btnValueRight", "textValue"};

static int om_is_auto(const om_item *it, const char *s)
{
    return s && *s ? !_stricmp(s, "auto") : !_stricmp(it->blank, "auto");
}

static double om_auto(const om_item *it)
{
    if (!strcmp(it->key, "SHADOW_QUALITY"))
        return gmv_gpu_integrated() ? 1.0 : 2.0;
    if (!strcmp(it->key, "POST_AA"))
        return 2.0;
    return 4.0;
}

static double om_num(const om_item *it, const char *s)
{
    if (om_is_auto(it, s))
        return om_auto(it);
    if (s && *s)
        return atof(s);
    return atof(it->blank);
}

static int om_count(const om_item *it)
{
    int n = 0;
    while (n < OM_VALS && it->val[n])
        n++;
    return n;
}

static int om_index(const om_item *it, double v)
{
    for (int i = 0; i < om_count(it); i++)
        if (!om_is_auto(it, it->val[i]) && fabs(om_num(it, it->val[i]) - v) < 0.002)
            return i;
    return -1;
}

static int om_auto_index(const om_item *it)
{
    for (int i = 0; i < om_count(it); i++)
        if (it->val[i] && !_stricmp(it->val[i], "auto"))
            return i;
    return -1;
}

static int om_file_auto(const om_item *it)
{
    return om_auto_index(it) >= 0 && om_is_auto(it, gmv_config_file_value(it->key));
}

static double om_saved(const om_item *it)
{
    return om_num(it, gmv_config_file_value(it->key));
}

static double om_shown(int k)
{
    const om_item *it = &om_items[k];
    return om_sel[k] >= 0 ? om_num(it, it->val[om_sel[k]]) : om_saved(it);
}

static int om_fields(const char *s)
{
    int n = 1;
    while ((s = strchr(s, ',')))
        s++, n++;
    return n;
}

static const char *om_field(const char *s, int j, size_t *len)
{
    const char *e;
    while ((e = strchr(s, ',')) && j-- > 0)
        s = e + 1;
    *len = e ? (size_t)(e - s) : strlen(s);
    return s;
}

static int om_find(const char *key, size_t len)
{
    for (size_t k = 0; k < OM_N; k++)
        if (om_items[k].key && om_items[k].live != OM_JOIN && strlen(om_items[k].key) == len &&
            !strncmp(om_items[k].key, key, len))
            return (int)k;
    return -1;
}

static int om_member(size_t k, int j)
{
    size_t len;
    const char *key = om_field(om_items[k].key, j, &len);
    return om_find(key, len);
}

static int om_pick(int m, const char *v, size_t len)
{
    for (int i = 0; i < om_count(&om_items[m]); i++)
        if (strlen(om_items[m].val[i]) == len && !strncmp(om_items[m].val[i], v, len))
            return i;
    return -1;
}

static int om_join_is(size_t k, int i)
{
    for (int j = 0; j < om_fields(om_items[k].key); j++) {
        size_t len;
        const char *v = om_field(om_items[k].val[i], j, &len);
        int m = om_member(k, j);
        if (len && om_sel[m] != om_pick(m, v, len))
            return 0;
    }
    return 1;
}

static void om_join_sync(void)
{
    for (size_t k = 0; k < om_rows; k++) {
        if (om_items[k].live != OM_JOIN)
            continue;
        om_sel[k] = -1;
        for (int i = 0; i < om_count(&om_items[k]) && om_sel[k] < 0; i++)
            if (om_join_is(k, i))
                om_sel[k] = i;
    }
}

static void om_join_set(size_t k, int i)
{
    for (int j = 0; j < om_fields(om_items[k].key); j++) {
        size_t len;
        const char *v = om_field(om_items[k].val[i], j, &len);
        int m = om_member(k, j);
        if (len)
            om_sel[m] = om_pick(m, v, len);
    }
    om_join_sync();
}

static int om_layout(void)
{
    int c = 0;
    om_rows = 0;
    while (om_rows < OM_N && om_items[om_rows].title)
        om_rows++;
    for (size_t k = om_rows; k < OM_N; k++)
        if (om_items[k].title || om_items[k].live != OM_LIVE)
            return 0;
    for (size_t k = 0; k < om_rows; k++) {
        if (c < OM_CATS && !strcmp(om_items[k].title, om_cats[c].from))
            om_cats[c++].first = (int)k;
        if (om_items[k].live != OM_JOIN)
            continue;
        for (int j = 0; j < om_fields(om_items[k].key); j++) {
            int m = om_member(k, j);
            if (m < (int)om_rows)
                return 0;
            for (int i = 0; i < om_count(&om_items[k]); i++) {
                size_t len;
                const char *v = om_field(om_items[k].val[i], j, &len);
                if (len && om_pick(m, v, len) < 0)
                    return 0;
            }
        }
    }
    if (c != OM_CATS || om_cats[0].first)
        return 0;
    for (c = 0; c < OM_CATS; c++)
        if ((om_cats[c].n = (c + 1 < OM_CATS ? om_cats[c + 1].first : (int)om_rows) - om_cats[c].first) <= 0)
            return 0;
    int mac = om_find("ORIGINAL_MAC_EXPERIENCE", strlen("ORIGINAL_MAC_EXPERIENCE"));
    if (mac < 0 || mac >= (int)om_rows)
        return 0;
    om_mac_cat.first = mac;
    om_k_mac = (size_t)mac;
    return 1;
}

void *om_mgr(void)
{
    return ((void *(*)(void))(uintptr_t)OM_MGR_FN)();
}

unsigned char *om_widget(int i)
{
    unsigned char *m = om_mgr();
    return m && *(int *)(m + 0x218) > i ? (*(unsigned char ***)(m + 0x214))[i] : NULL;
}

void *om_app(void)
{
    return ((void *(*)(void))(uintptr_t)OM_APP_FN)();
}

void om_sound(int id)
{
    unsigned char *mm = radio_mm();
    if (mm)
        ((void (*)(void *, int, int, int, int))(uintptr_t)OM_SOUND_FN)(mm, id, 0, 0, 0);
}

void om_visible(void *fx, const char *path, int on)
{
    ((void (*)(void *, const char *, int))(uintptr_t)OM_SET_VISIBLE)(fx, path, on);
}

void om_text(void *fx, const char *path, const char *text)
{
    ((void (*)(void *, const char *, const char *, int))(uintptr_t)OM_SET_TEXT)(fx, path, text, 1);
}

static void om_line_text(int line, const char *field, const char *text)
{
    char path[96];
    snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d.%s", line, field);
    ((void (*)(void *, const char *, const char *, int, int))(uintptr_t)OM_LINE_TEXT)(om_mgr(), path, text, 1, 0);
}

void om_line_visible(void *fx, int line, const char *field, int on)
{
    char path[96];
    snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d.%s", line, field);
    om_visible(fx, path, on);
}

static int om_set_uc(const char *key, double v)
{
    (void)key;
    if (!uc_loaded)
        return 0;
    if (v != 0)
        uc_hooks_in();
    return 1;
}

/* RADIO_SONGS 0x953de */
static int om_set_radio_songs(const char *key, double v)
{
    (void)key;
    unsigned char *mm = radio_mm();
    int mac, all;
    if (!gmv_rs_have())
        return 0;
    if (!mm || !gmv_rs_counts(0, &mac, &all))
        return 1;
    int on = v != 0, n = *(int *)(mm + 0x254 + 0x1c);
    unsigned char **st = *(unsigned char ***)(mm + 0x254 + 0x18), **bag = *(unsigned char ***)(mm + 0x254 + 0x14);
    while (mm[0x275])                           /* 0x5fee8 */
        Sleep(0);
    for (int i = 0; i < n; i++)
        if (gmv_rs_counts(i, &mac, &all)) {
            *(int *)(st[i] + 8) = on ? all : mac;
            *(int *)(bag[i] + 4) = 0;
        }
    if (!on && (*(uint32_t *)(mm + 0x220) & 1) && gmv_rs_is_song(*(int *)(mm + 0x218)))
        ((void (*)(void *, int))(uintptr_t)RADIO_TUNE_FN)(mm, *(int *)(mm + 0x21c));
    gmv_log("[radio songs] %s\n", on ? "on" : "off: the Mac's songs");
    return 1;
}

void rs_tick(void)
{
    int mac, all;
    if (!gmv_rs_counts(0, &mac, &all))
        return;
    unsigned char *mm = radio_mm();
    if (!mm)
        return;
    int n = *(int *)(mm + 0x254 + 0x1c), on = gmv_rs_on(), differs = 0;
    unsigned char **st = *(unsigned char ***)(mm + 0x254 + 0x18), **bag = *(unsigned char ***)(mm + 0x254 + 0x14);
    if (n <= 0 || !st || !bag)
        return;
    for (int i = 0; i < n; i++) {
        if (!st[i] || !bag[i])
            return;
        if (gmv_rs_counts(i, &mac, &all))
            differs |= *(int *)(st[i] + 8) != (on ? all : mac);
    }
    if (differs)
        om_set_radio_songs(NULL, on);
}

static const om_category *om_category_now(void)
{
    return om_mac ? &om_mac_cat : &om_cats[om_tab_cat[om_tab_now]];
}

static int om_slot(void)
{
    return om_mac ? OM_SLOT : om_tabs[om_tab_now].slot;
}

static void om_load(void)
{
    om_res_now = ((int (*)(void *))(uintptr_t)OM_RES_NOW)(om_app());
    om_full_now = (((unsigned (*)(void))(uintptr_t)OM_FULL_NOW)() & 0xff) != 0;
    for (size_t k = 0; k < OM_N; k++)
        om_sel[k] = k == OM_K_RES ? om_res_now : k == OM_K_FULL ? om_full_now
                  : om_file_auto(&om_items[k]) ? om_auto_index(&om_items[k])
                                              : om_index(&om_items[k], om_saved(&om_items[k]));
    om_join_sync();
}

static void om_step(int k, int dir)
{
    const om_item *it = &om_items[k];
    int n, i = om_sel[k];
    if (it->live == OM_RESOLUTION) {
        unsigned h;
        om_resolution(0, &h);
        n = *(int *)((unsigned char *)om_app() + OM_RES_COUNT);
        if (n > 0)
            om_sel[k] = ((i + dir) % n + n) % n;
        return;
    }
    n = om_count(it);
    if (it->live == OM_JOIN) {
        om_join_set((size_t)k, i >= 0 ? (i + dir + n) % n : dir > 0 ? 0 : n - 1);
        return;
    }
    if (i >= 0)
        i = (i + dir + n) % n;
    else {
        int lo = om_auto_index(it) == 0;
        double v = om_saved(it), up = om_num(it, it->val[n - 1]) > om_num(it, it->val[lo]) ? 1.0 : -1.0;
        if (dir > 0) {
            for (i = 0; i < n && (om_num(it, it->val[i]) - v) * up <= 0.0; i++)
                ;
            if (i == n)
                i = 0;
        } else {
            for (i = n - 1; i >= 0 && (om_num(it, it->val[i]) - v) * up >= 0.0; i--)
                ;
            if (i < 0)
                i = n - 1;
        }
    }
    om_sel[k] = i;
}

static int om_no_addon(const om_item *it)
{
    return (it->set == om_set_uc && !uc_loaded) || (it->set == om_set_radio_songs && !gmv_rs_have());
}

static int om_set_mac(const char *key, double v)
{
    static const char *const startup_keys[] = {
        "TEXFIX", "SWAP_COPY", "NO_FLICKER", "VTXFIX", "CRASH58_GUARD", "FAST_FTELL", "FAST_FSEEK",
        "FAST_MEMMOVE",
#if GMV_DEV
        "SOLO_CAR",
#endif
        "NO_INTRO", "SHOW_CURSOR",
    };
    enum { NSTART = sizeof startup_keys / sizeof *startup_keys };
    const char *was[NSTART];
    char env[64];
    int on = v != 0, later = 0;
    (void)key;
    if (!tab_live())
        return 0;
    for (int i = 0; i < NSTART; i++) {
        snprintf(env, sizeof env, "GMV_%s", startup_keys[i]);
        was[i] = gmv_getenv(env);
    }
    gmv_config_set_mac(on);
    om_mac = on;
    gmv_log("[gamepatch] ORIGINAL_MAC_EXPERIENCE %s: the settings to %s\n", on ? "on" : "off",
            on ? "the Mac's" : "GangstarMV.ini's");
    if (!on)
        mac_hooks_in();
    for (size_t k = 0; k < OM_N; k++) {
        const om_item *it = &om_items[k];
        if (!it->key || k == om_k_mac || it->live != OM_LIVE)
            continue;
        snprintf(env, sizeof env, "GMV_%s", it->key);
        double t = on ? om_num(it, gmv_getenv(env)) : om_saved(it);
        int ok = it->set(it->key, t);
        if (ok)
            om_now[k] = t;
        om_later[k] = !ok && fabs(t - om_now[k]) >= 0.002;
        later |= om_later[k] && !om_no_addon(it);
    }
    const char *fh = gmv_getenv("GMV_FOG_HEIGHT");
    gmv_set_post("FOG_HEIGHT", fh && *fh && atof(fh) >= 0 ? (float)atof(fh) : 60.0f);
    cr_mac_switch(on);
    if (on)
        mac_hooks_out();
    for (int i = 0; i < NSTART; i++) {
        snprintf(env, sizeof env, "GMV_%s", startup_keys[i]);
        const char *now = gmv_getenv(env);
        if (strcmp(was[i] ? was[i] : "", now ? now : "")) {
            gmv_log("[gamepatch] options tab: %s=%s from the next start\n", startup_keys[i], now ? now : "");
            later = 1;
        }
    }
    if (later)
        gmv_log("[gamepatch] ORIGINAL_MAC_EXPERIENCE: some of it takes effect at the next start\n");
    return 1;
}

static int om_relayout;

static void om_apply_one(size_t k)
{
    const om_item *it = &om_items[k];
    if (!it->key || it->live == OM_JOIN || om_sel[k] < 0)
        return;
    double v = om_num(it, it->val[om_sel[k]]);
    int same = fabs(v - om_saved(it)) < 0.002 && om_is_auto(it, it->val[om_sel[k]]) == om_file_auto(it);
    if (same || !gmv_config_save(it->key, it->val[om_sel[k]]))
        return;
    if (om_mac && k != om_k_mac) {
        gmv_log("[gamepatch] options tab: %s=%s saved, in force without ORIGINAL_MAC_EXPERIENCE\n",
                it->key, it->val[om_sel[k]]);
        return;
    }
    int mac = om_mac;
    int now = it->live == OM_LIVE && it->set(it->key, v);
    if (now)
        om_now[k] = v;
    om_later[k] = !now && it->live != OM_RESTART && fabs(v - om_now[k]) >= 0.002;
    om_relayout |= om_mac != mac;
    gmv_log("[gamepatch] options tab: %s=%s %s\n", it->key, it->val[om_sel[k]],
            now ? "in force now" : fabs(v - om_now[k]) < 0.002 ? "(as running)"
            : om_no_addon(it) ? "saved; it needs its add-on, which was not there at the start" : "from the next start");
}

static void om_apply(void)
{
    om_apply_one(om_k_mac);
    for (size_t k = 0; k < OM_N; k++)
        if (k != om_k_mac)
            om_apply_one(k);
    if (om_sel[OM_K_RES] != om_res_now || om_sel[OM_K_FULL] != om_full_now) {
        int mode[2] = { om_sel[OM_K_RES], om_sel[OM_K_FULL] };
        unsigned h, w = om_resolution(mode[0], &h);
        ((void (*)(void *, int *, int))(uintptr_t)OM_RES_SET)(om_app(), mode, mode[1]);
        om_res_now = mode[0];
        om_full_now = mode[1];
        gmv_log("[gamepatch] options tab: resolution %ux%u%s\n", w, h, mode[1] ? ", full screen" : "");
    }
}

static const char *om_note(int k)
{
    const om_item *it = &om_items[k];
    if (it->live == OM_JOIN) {
        const char *note = NULL;
        for (int j = 0; j < om_fields(it->key); j++) {
            const char *n = om_note(om_member((size_t)k, j));
            if (n && (!note || strcmp(n, OM_NO_ADDON)))
                note = n;
        }
        return note;
    }
    if (!it->key || fabs(om_shown(k) - om_now[k]) < 0.002)
        return NULL;
    if (it->live != OM_RESTART && !om_later[k])
        return NULL;
    return om_no_addon(it) ? OM_NO_ADDON : "NEXT START";
}

static int om_line_item(const om_category *c, int page, int l)
{
    int p1 = c->page1 ? c->page1 : OM_LINES;
    int i = page ? p1 + (page - 1) * OM_LINES + l - 1 : l - 1;
    return l <= (page ? OM_LINES : p1) && i < c->n ? i : -1;
}

static int om_page_count(const om_category *c)
{
    int p1 = c->page1 ? c->page1 : OM_LINES;
    return c->n <= p1 ? 1 : 1 + (c->n - p1 + OM_LINES - 1) / OM_LINES;
}

static void om_fill(void)
{
    unsigned char *list = om_widget(OM_W_LIST), *pager = om_widget(OM_W_PAGER);
    if (!list)
        return;
    void *fx = *(void **)(list + 8);
    const om_category *c = om_category_now();
    int page = pager ? *(int *)(pager + 0x18) : 0, later = 0, later_start = 0;
    ((void (*)(void *))(uintptr_t)OM_LIST_RESET)(list);
    for (int l = 1; l <= OM_LINES; l++) {
        int i = om_line_item(c, page, l);
        char path[80], value[48];
        snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d", l);
        om_visible(fx, path, i >= 0);
        if (i < 0)
            continue;
        int k = c->first + i;
        const char *note = om_note(k);
        if (om_items[k].live == OM_RESOLUTION) {
            unsigned h, w = om_resolution(om_sel[k], &h);
            snprintf(value, sizeof value, "%uX%u", w, h);
        } else if (om_items[k].live == OM_JOIN)
            snprintf(value, sizeof value, "%s", om_sel[k] >= 0 ? om_items[k].name[om_sel[k]] : "CUSTOM");
        else if (om_sel[k] >= 0 && om_is_auto(&om_items[k], om_items[k].val[om_sel[k]])) {
            int a = om_index(&om_items[k], om_auto(&om_items[k]));
            const char *nm = a >= 0 ? om_items[k].name[a] : "";
            const char *cut = strstr(nm, " (");
            if (a < 0)
                snprintf(value, sizeof value, "AUTO");
            else
                snprintf(value, sizeof value, "AUTO (%.*s)", cut ? (int)(cut - nm) : (int)strlen(nm), nm);
        } else if (om_sel[k] >= 0)
            snprintf(value, sizeof value, "%s", om_items[k].name[om_sel[k]]);
        else
            snprintf(value, sizeof value, "%g%s", om_saved(&om_items[k]), om_items[k].unit);
        om_line_text(l, "wCostAction.textAction", om_items[k].title);
        if (om_selectors) {
            om_line_visible(fx, l, "wCostKey", 0);
            om_line_visible(fx, l, "wCostKey1", 0);
            om_line_visible(fx, l, "wCostKey2", 0);
            om_line_text(l, "textKey1", "");
            om_line_text(l, "textKey2", "");
            om_selectors_visible(fx, l, 1);
            if (note)
                strcat(value, " *");
            om_line_text(l, "textValue", value);
            later |= note != NULL;
            later_start |= note != NULL && strcmp(note, OM_NO_ADDON);
            continue;
        }
        om_line_text(l, "textKey1", value);
        om_line_text(l, "textKey2", note ? note : "");
        snprintf(path, sizeof path, "widgetControlHelps.wCusLine%d.wCostKey2", l);
        om_visible(fx, path, note != NULL);
    }
    if (om_selectors) {
        if (later)
            ((void (*)(void *, const char *, const char *, int, int))(uintptr_t)OM_LINE_TEXT)(
                om_mgr(), "widgetControlHelps.cust.wTextPressKey",
                later_start ? "* TAKES EFFECT AT THE NEXT START" : "* NEEDS ITS ADD ON FILE", 1, 0);
        om_foot = later;
        om_visible(fx, "widgetControlHelps.cust", later);
    }
    om_list_ours = 1;
}

static void om_cat_texts(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    om_text(fx, "widgetActionKeyTitle.titleAction", "SETTING");
    om_text(fx, "widgetActionKeyTitle.titleKey", "VALUE");
    om_text(fx, "widgetActionKeyTitle.txtValue", om_category_now()->name);
}

void om_pages(unsigned char *pager, int n, int i)
{
    *(int *)(pager + 0x18) = i;
    ((void (*)(void *, int, int, int))(uintptr_t)OM_PAGES)(pager, n, i, 1);
    ((void (*)(void *))(uintptr_t)(*(uint32_t **)pager)[OM_W_TEXTS])(pager);
}

static void om_credit_show(unsigned char *w);

static void om_show_category(void)
{
    unsigned char *pager = om_widget(OM_W_PAGER), *cat = om_widget(OM_W_CAT);
    if (pager)
        om_pages(pager, om_page_count(om_category_now()), 0);
    if (cat) {
        om_cat_texts(cat);
        om_credit_show(cat);
    }
    om_fill();
}

/* 0x19bcd0 0x45d000 0x68a6c0 */
#define FS_GAME_MS   0x0045d000u
#define FS_LAST_MS   0x0068a6c0u
#define FS_HOLD_MS   0x1f5u
static int fs_want = -1, fs_tries, fs_waits;
static DWORD fs_at;

static int fs_now(void)
{
    return (((unsigned (*)(void))(uintptr_t)OM_FULL_NOW)() & 0xff) != 0;
}

static void fs_request(void *app, int full)
{
    int mode[2] = { ((int (*)(void *))(uintptr_t)OM_RES_NOW)(app), full };
    ((void (*)(void *, int *, int))(uintptr_t)OM_RES_SET)(app, mode, mode[1]);
    om_res_now = mode[0];
    fs_at = GetTickCount();
}

__attribute__((force_align_arg_pointer, noinline))
void gmv_toggle_full_screen(void)
{
    void *app = om_app();
    if (!app)
        return;
    if (fs_want >= 0) {
        if (!fs_waits++)
            gmv_log("[gamepatch] Alt+Enter: still switching to %s, press ignored\n", fs_want ? "full screen" : "window");
        return;
    }
    int full = !fs_now();
    fs_request(app, full);
    fs_want = full;
    fs_tries = 1;
    fs_waits = 0;
    if (om_sel[OM_K_FULL] == om_full_now)
        om_sel[OM_K_FULL] = full;
    om_full_now = full;
    if (om_active && om_list_ours)
        om_fill();
    gmv_log("[gamepatch] Alt+Enter: %s\n", full ? "full screen" : "window");
}

__attribute__((force_align_arg_pointer, noinline))
void gmv_full_screen_tick(void)
{
    if (fs_want < 0)
        return;
    void *app = om_app();
    if (!app || fs_now() == fs_want) {
        fs_want = -1;
        return;
    }
    uint32_t since = ((uint32_t (*)(void))(uintptr_t)FS_GAME_MS)() - ms_rd(FS_LAST_MS);
    if (since < FS_HOLD_MS + 10 && GetTickCount() - fs_at < 2000)
        return;
    if (fs_tries >= 3) {
        gmv_log("[gamepatch] Alt+Enter: the switch to %s did not happen\n", fs_want ? "full screen" : "window");
        fs_want = -1;
        return;
    }
    if (GetTickCount() - fs_at < 50)
        return;
    fs_tries++;
    gmv_log("[gamepatch] Alt+Enter: the game's 500 ms wait dropped the switch to %s; asked again\n",
            fs_want ? "full screen" : "window");
    fs_request(app, fs_want);
}

static void om_show_mac_switch(void)
{
    om_relayout = 0;
    if (!om_mac)
        for (int t = 0; t < OM_TABS; t++)
            for (int c = om_tabs[t].first; c < om_tabs[t].first + om_tabs[t].n; c++)
                if ((int)om_k_mac >= om_cats[c].first && (int)om_k_mac < om_cats[c].first + om_cats[c].n)
                    om_tab_now = t, om_tab_cat[t] = c;
    unsigned char *tabs = om_widget(OM_W_TABS);
    if (tabs)
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, om_slot(), 1);
    om_show_category();
}

const char *om_event_path(unsigned char *ev)
{
    return ((const char *(*)(void *))(uintptr_t)OM_EVENT_PATH)(ev);
}

/* +8 +0x24 */
__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_event(unsigned char *w, unsigned char *ev)
{
    if (!om_active) {
        ((void (*)(void *, void *))(uintptr_t)OM_CAT_EVENT)(w, ev);
        return;
    }
    const char *name = *(const char **)(ev + 4), *path;
    if (*(int *)(ev + 8) != 6 || ev[0x24] || !name)
        return;
    int d = !strcmp(name, "btnRight") ? 1 : !strcmp(name, "btnLeft") ? -1 : 0;
    if (!d || !(path = om_event_path(ev)) || strncmp(path, "widgetActionKeyTitle.", 21))
        return;
    ev[0x24] = 1;
    if (om_mac)
        return;
    const om_tab *t = &om_tabs[om_tab_now];
    int *c = &om_tab_cat[om_tab_now];
    om_sound(0x56);
    *c = t->first + (*c - t->first + d + t->n) % t->n;
    om_show_category();
}

static void om_credit_show(unsigned char *w)
{
    void *fx = *(void **)(w + 8);
    int on = om_active && om_slot() == OM_SLOT;
    if (!om_credit || !fx)
        return;
    om_visible(fx, "widgetActionKeyTitle.txtPatchBy", on);
#if OM_SHOW_CREDIT
    if (on) {
        static int told;
        om_text(fx, "widgetActionKeyTitle.txtPatchBy", OM_CREDIT);
        if (!told++)
            gmv_trace("[gamepatch] options tab: txtPatchBy %p\n",
                    ((void *(*)(void *, const char *))(uintptr_t)0x002fd160u)(fx, "widgetActionKeyTitle.txtPatchBy"));
    }
#endif
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_texts(unsigned char *w)
{
    if (om_active)
        om_cat_texts(w);
    else
        ((void (*)(void *))(uintptr_t)OM_CAT_TEXTS)(w);
    om_credit_show(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_keep(unsigned char *w)
{
    if (!om_active)
        ((void (*)(void *))(uintptr_t)OM_CAT_TEXTS)(w);
    ((void (*)(void *))(uintptr_t)OM_KEEP)(w);
    om_credit_show(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_cat_show(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)OM_PLAIN_SHOW)(w);
    om_credit_show(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_show(unsigned char *w)
{
    if (om_active) {
        ((void (*)(void *))(uintptr_t)OM_PLAIN_SHOW)(w);
        return;
    }
    for (int l = 1; om_selectors && l <= OM_LINES; l++)
        om_selectors_visible(*(void **)(w + 8), l, 0);
    ((void (*)(void *))(uintptr_t)OM_LIST_SHOW)(w);
    om_list_keys = 1;
    om_list_ours = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_hide(unsigned char *w, int fade)
{
    if (om_list_keys)
        ((void (*)(void *, int))(uintptr_t)OM_LIST_HIDE)(w, fade);
    else
        ((void (*)(void *, int))(uintptr_t)OM_PLAIN_HIDE)(w, fade);
    om_list_keys = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_keep(unsigned char *w)
{
    if (!om_active && om_list_ours) {
        gmv_om_list_hide(w, 0);
        gmv_om_list_show(w);
    }
    ((void (*)(void *))(uintptr_t)OM_KEEP)(w);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_texts(unsigned char *w)
{
    if (!om_active) {
        ((void (*)(void *))(uintptr_t)OM_LIST_TEXTS)(w);
        return;
    }
    void *fx = *(void **)(w + 8);
    om_visible(fx, "widgetControlHelps.cust", om_selectors && om_foot);
    om_text(fx, "widgetControlHelps.btnOk.text", "APPLY");
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_list_event(unsigned char *w, unsigned char *ev)
{
    if (!om_active) {
        ((void (*)(void *, void *))(uintptr_t)OM_LIST_EVENT)(w, ev);
        return;
    }
    const char *name = *(const char **)(ev + 4), *path;
    int kind = *(int *)(ev + 8);
    if (ev[0x24] || !name)
        return;
    if (kind == 6 && !strcmp(name, "btnOk")) {
        ev[0x24] = 1;
        om_sound(0x57);
        om_apply();
        if (om_relayout)
            om_show_mac_switch();
        else
            om_fill();
        return;
    }
    int dir = 0;
    if (kind == 6 && om_selectors)
        dir = !strcmp(name, "btnValueRight") ? 1 : !strcmp(name, "btnValueLeft") ? -1 : 0;
    else if (kind == 0 && !om_selectors)
        dir = !strcmp(name, "wCostKey1") ? 1 : !strcmp(name, "wCostKey2") ? -1 : 0;
    if (!dir || !(path = om_event_path(ev)))
        return;
    unsigned char *pager = om_widget(OM_W_PAGER);
    const om_category *c = om_category_now();
    for (int l = 1; l <= OM_LINES; l++) {
        char want[80];
        snprintf(want, sizeof want, "widgetControlHelps.wCusLine%d.%s", l, name);
        if (strcmp(path, want))
            continue;
        int i = om_line_item(c, pager ? *(int *)(pager + 0x18) : 0, l);
        ev[0x24] = 1;
        if (i < 0 || *(const unsigned char *)(uintptr_t)OM_LOCK)
            return;
        om_sound(0x56);
        om_step(c->first + i, dir);
        om_fill();
        return;
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static const char *gmv_tab_label(void *table, int id)
{
    if (id == OM_TAB_ID)
        return "GAMEPATCH";
    if (id == MR_TAB_ID)
        return "MISSION REPLAY";
    if (id == TOD_TAB_ID)
        return "TIME OF DAY";
    return ((const char *(*)(void *, int))(uintptr_t)OM_STRING_FN)(table, id);
}

int tab_label_hook(void)
{
    static int done;
    if (!done && sm_is_call(OM_LABEL_SITE, OM_STRING_FN) && sm_redirect(OM_LABEL_SITE, (void *)gmv_tab_label))
        done = 1;
    return done;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_tab6(void *fx, const char *path, int visible)
{
    unsigned char *tabs = om_widget(OM_W_TABS);
    if (tabs) {
        ((void (*)(void *, int, int, int))(uintptr_t)OM_SET_ENTRY)(tabs, OM_SLOT, OM_TAB_ID, 1);
        visible = 1;
    }
    om_visible(fx, path, visible);
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_om_clicked(unsigned char *tabs)
{
    int n = *(int *)(tabs + 0x58), t = 0;
    while (!om_mac && t < OM_TABS && om_tabs[t].slot != n)
        t++;
    if (om_mac ? n != OM_SLOT : t == OM_TABS)
        return n;
    om_sound(0x5a);
    if (!om_active) {
        om_tab_now = om_mac ? 0 : t;
        ((void (*)(void *, const char *, int))(uintptr_t)OM_SWITCH_FN)(om_mgr(), OM_STATE, 0);
    } else if (!om_mac && t != om_tab_now) {
        om_apply();
        if (om_relayout) {
            om_show_mac_switch();
            return 4;
        }
        om_tab_now = t;
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, n, 1);
        om_show_category();
    }
    return 4;
}

static void om_take_widgets(void)
{
    unsigned char *cat = om_widget(OM_W_CAT), *list = om_widget(OM_W_LIST);
    if (cat && *(uint32_t *)cat == OM_CAT_VT)
        *(uint32_t **)cat = om_cat_vt + 2;
    if (list && *(uint32_t *)list == OM_LIST_VT) {
        om_list_keys = list[0x10];
        *(uint32_t **)list = om_list_vt + 2;
    }
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_show(unsigned char *self)
{
    unsigned char *tabs = om_widget(OM_W_TABS), *pager = om_widget(OM_W_PAGER);
    unsigned char *cat = om_widget(OM_W_CAT), *list = om_widget(OM_W_LIST);
    void *m = om_mgr();
    om_take_widgets();
    om_active =cat && list && pager && *(uint32_t **)cat == om_cat_vt + 2 && *(uint32_t **)list == om_list_vt + 2;
    om_load();
    if (pager)
        ((void (*)(void *, void *))(uintptr_t)OM_LISTEN)(pager, self);
    ((void (*)(void *))(uintptr_t)OM_OPT_SHOW)(self);
    ((void (*)(void *, int))(uintptr_t)0x0015b4b8u)(m, 1);
    if (tabs)
        ((void (*)(void *, int, int))(uintptr_t)OM_TAB_SELECT)(tabs, om_slot(), 1);
    if (!om_active) {
        gmv_log("[gamepatch] options tab: the CONTROLS page's widgets are not the expected ones\n");
        return;
    }
    if (((unsigned char (*)(void))(uintptr_t)0x001b5520u)())
        ((void (*)(void *))(uintptr_t)0x0015b48eu)(m);
    ((void (*)(void *, const char *, int, int))(uintptr_t)OM_MOVE)(*(void **)(self + 4), "widgetArrows", 0x276, 0x183);
    om_show_category();
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_hide(unsigned char *self)
{
    unsigned char *pager = om_widget(OM_W_PAGER);
    if (om_active) {
        om_apply();
        om_relayout = 0;
        if (pager && *(unsigned char **)(pager + 0xc) == self)
            ((void (*)(void *, const char *, int, int))(uintptr_t)OM_MOVE)(*(void **)(self + 4), "widgetArrows", 0x1c0, 0x1ad);
    }
    ((void (*)(void *))(uintptr_t)OM_GS_HIDE)(self);
    om_active = 0;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_om_event(unsigned char *self, unsigned char *w, int type)
{
    if (((unsigned char (*)(void *, void *, int))(uintptr_t)OM_OPT_EVENT)(self, w, type))
        return 1;
    if (!om_active || !w || w != om_widget(OM_W_PAGER) || (type != 1 && type != 2))
        return 0;
    om_fill();
    return 1;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_om_register(void *menufx, void *state, const char *clip)
{
    typedef void (*reg_fn)(void *, void *, const char *);
    ((reg_fn)(uintptr_t)OM_REG_FN)(menufx, state, clip);
    unsigned char *s = ((unsigned char *(*)(uint32_t, const char *, int, int))(uintptr_t)OM_ALLOC)(
        OM_GS_BYTES, "the patches", 1147, 1);
    if (!s) {
        gmv_log("[gamepatch] no memory for the GAMEPATCH options page\n");
        return;
    }
    ((void (*)(void *))(uintptr_t)OM_GS_CTOR)(s);
    uint32_t *ids = *(uint32_t **)(s + 0x5c);
    int nids = *(int *)(s + 0x60), row = -1, lang = -1;
    for (int i = 0; i < nids; i++) {
        if (ids[i] == OM_W_ROW)
            row = i;
        else if (ids[i] == OM_W_LANGUAGE)
            lang = i;
    }
    if (row < 0 || lang < 0) {
        gmv_log("[gamepatch] the Game page's widgets are not the expected ones; no GAMEPATCH tab\n");
        return;
    }
    ids[row] = OM_W_CAT;
    ids[lang] = OM_W_LIST;
    strcpy((char *)s + 8, OM_STATE);
    memcpy(om_vt, (const void *)(uintptr_t)OM_GS_VT, sizeof om_vt);
    om_vt[OM_VT_SHOW] = (uint32_t)(uintptr_t)gmv_om_show;
    om_vt[OM_VT_HIDE] = (uint32_t)(uintptr_t)gmv_om_hide;
    om_vt[OM_VT_EVENT] = (uint32_t)(uintptr_t)gmv_om_event;
    *(uint32_t **)s = om_vt;
    ((reg_fn)(uintptr_t)OM_REG_FN)(menufx, s, clip);
    om_take_widgets();
    if (om_mac)
        gmv_log("[gamepatch] options menu: GAMEPATCH tab registered in slot %d (1 setting)\n", OM_SLOT);
    else
        gmv_log("[gamepatch] options menu: VIDEO (slot 3, %d categories) and GAMEPATCH (slot %d, %d categories) "
                "registered (%d settings)\n", om_tabs[0].n, OM_SLOT, om_tabs[1].n, (int)om_rows);
}

void apply_options_tab(void)
{
    if (!tab_live())
        return;
    static const unsigned char table_cmp[4] = { 0x83, 0xf8, 0x03, 0x77 };
    const uint32_t *vt = (const uint32_t *)(uintptr_t)OM_GS_VT;
    const uint32_t *cv = (const uint32_t *)(uintptr_t)OM_CAT_VT, *lv = (const uint32_t *)(uintptr_t)OM_LIST_VT;
    int covered = 0;
    for (int t = 0; t < OM_TABS; t++) {
        covered = om_tabs[t].first == covered && om_tabs[t].n > 0 ? covered + om_tabs[t].n : -1;
        om_tab_cat[t] = om_tabs[t].first;
    }
    if (!sm_is_call(OM_HIDE6_SITE, OM_SET_VISIBLE) || !sm_is_call(OM_CLICK_SITE, OM_CLICK_FN) ||
        !sm_is_call(OM_REG_SITE, OM_REG_FN) || !sm_is_call(OM_LABEL_SITE, OM_STRING_FN) ||
        memcmp((const void *)(uintptr_t)OM_TABLE_CMP, table_cmp, 4) || vt[OM_VT_SHOW] != 0x00169a6au ||
        vt[OM_VT_HIDE] != OM_GS_HIDE || vt[OM_VT_EVENT] != 0x00169b62u ||
        cv[OM_W_SHOW] != OM_PLAIN_SHOW || cv[OM_W_KEEP] != OM_KEEP || cv[OM_W_EVENT] != OM_CAT_EVENT ||
        cv[OM_W_TEXTS] != OM_CAT_TEXTS || lv[OM_W_SHOW] != OM_LIST_SHOW || lv[OM_W_HIDE] != OM_LIST_HIDE ||
        lv[OM_W_KEEP] != OM_KEEP || lv[OM_W_EVENT] != OM_LIST_EVENT || lv[OM_W_TEXTS] != OM_LIST_TEXTS) {
        gmv_log("[gamepatch] options menu not the expected bytes; no GAMEPATCH tab\n");
        return;
    }
    if (!om_layout() || covered != OM_CATS ||
        om_items[OM_K_RES].live != OM_RESOLUTION || om_items[OM_K_FULL].live != OM_FULL_SCREEN) {
        gmv_log("[gamepatch] options tab: the categories do not cover the settings; no GAMEPATCH tab\n");
        return;
    }
    const char *m = gmv_getenv("GMV_ORIGINAL_MAC_EXPERIENCE");
    om_mac = gmv_env_on(m);
    for (size_t k = 0; k < OM_N; k++) {
        char env[64];
        if (!om_items[k].key || om_items[k].live == OM_JOIN)
            continue;
        snprintf(env, sizeof env, "GMV_%s", om_items[k].key);
        om_now[k] = om_num(&om_items[k], gmv_getenv(env));
    }
    memcpy(om_cat_vt, cv - 2, sizeof om_cat_vt);
    om_cat_vt[2 + OM_W_SHOW] = (uint32_t)(uintptr_t)gmv_om_cat_show;
    om_cat_vt[2 + OM_W_KEEP] = (uint32_t)(uintptr_t)gmv_om_cat_keep;
    om_cat_vt[2 + OM_W_EVENT] = (uint32_t)(uintptr_t)gmv_om_cat_event;
    om_cat_vt[2 + OM_W_TEXTS] = (uint32_t)(uintptr_t)gmv_om_cat_texts;
    memcpy(om_list_vt, lv - 2, sizeof om_list_vt);
    om_list_vt[2 + OM_W_SHOW] = (uint32_t)(uintptr_t)gmv_om_list_show;
    om_list_vt[2 + OM_W_HIDE] = (uint32_t)(uintptr_t)gmv_om_list_hide;
    om_list_vt[2 + OM_W_KEEP] = (uint32_t)(uintptr_t)gmv_om_list_keep;
    om_list_vt[2 + OM_W_EVENT] = (uint32_t)(uintptr_t)gmv_om_list_event;
    om_list_vt[2 + OM_W_TEXTS] = (uint32_t)(uintptr_t)gmv_om_list_texts;
    if (sm_redirect(OM_REG_SITE, (void *)gmv_om_register) && tab_label_hook() &&
        sm_redirect(OM_HIDE6_SITE, (void *)gmv_om_tab6) && sm_redirect(OM_CLICK_SITE, (void *)gmv_om_clicked))
        gmv_log("[gamepatch] a GAMEPATCH tab in the options menu%s (GAMEPATCH_TAB=0 to remove it)\n",
                om_mac ? ", with only the ORIGINAL_MAC_EXPERIENCE switch" : ", and VIDEO with DISPLAY and GRAPHICS");
    else
        return;
    om_tab_files = 1;
    if (!om_opener_hook())
        gmv_log("[gamepatch] options tab: the file opener is not the expected one; the list keeps its key boxes\n");
}
