#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x60e2c [mm+0x21c] 0x60190 0x10a57c [car+0x120] */
#define RADIO_START_FN  0x00060190u
#define RADIO_SHOW_RET  0x000931beu
#define RADIO_BANK_PP   0x0063e56cu

static const uint32_t radio_pick_sites[]  = { 0x000601cdu, 0x000605bau };
static const uint32_t radio_tune_sites[]  = { 0x0006014bu, 0x00060186u };
static const uint32_t radio_start_sites[] = { 0x00036d1fu, 0x0010a5f5u, 0x0010a65au,
                                              0x0012cecfu, 0x0012f37fu, 0x001b493fu };
static const uint32_t radio_logo_sites[]  = { 0x000931b9u, 0x000932feu };
static const char *const radio_logo_names[5] = { "104FM", "303FM", "42FM", "101.1FM", "99FM" };

static int g_radio_logo = -1;

static uint32_t radio_rd(uint32_t a)
{
    return (a && !IsBadReadPtr((void *)(uintptr_t)a, 4)) ? *(const uint32_t *)(uintptr_t)a : 0;
}

static const char *radio_name(int id)
{
    uint32_t tbl = radio_rd(radio_rd(radio_rd(radio_rd(RADIO_BANK_PP))) + 4);
    if (!tbl || id < 0)
        return "?";
    const char *s = (const char *)(uintptr_t)radio_rd(tbl + (uint32_t)id * 8);
    return (s && !IsBadReadPtr(s, 1)) ? s : "?";
}

static const char *radio_logo_name(int station)
{
    return (station >= 0 && station < 5) ? radio_logo_names[station] : station == GMV_CR_STATION ? "CUSTOM" : "-";
}

/* RadioStationController mm+0x254 +0x1c +0x18 nB 0x95610 */
static void radio_dump_stations(const unsigned char *mm)
{
    uint32_t ctl = (uint32_t)(uintptr_t)(mm + 0x254);
    int n = (int)radio_rd(ctl + 0x1c), base = (int)radio_rd((uint32_t)(uintptr_t)(mm + 0x250));
    uint32_t arr = radio_rd(ctl + 0x18);
    gmv_trace("[radio] %d stations, slot base %d, %d slots, mm station count %d\n", n, base,
            (int)radio_rd((uint32_t)(uintptr_t)(mm + 0x24c)), (int)radio_rd((uint32_t)(uintptr_t)(mm + 0x270)));
    for (int i = 0; i < n && i < 16; i++) {
        uint32_t st = radio_rd(arr + 4 * (uint32_t)i);
        gmv_trace("[radio] station %d (HUD logo %s), x=%d\n", i, radio_logo_name(i), (int)radio_rd(st + 0x10));
        for (int l = 0; l < 2; l++) {
            int cnt = (int)radio_rd(st + 8 * (uint32_t)l);
            uint32_t list = radio_rd(st + 8 * (uint32_t)l + 4);
            for (int k = 0; k < cnt && k < 32; k++) {
                int v = (int)radio_rd(list + 4 * (uint32_t)k);
                gmv_trace("[radio]   %c[%d] = %d \"%s\" (base+%d: \"%s\")\n", l ? 'B' : 'A', k, v,
                        radio_name(v), v, radio_name(base + v));
            }
        }
    }
}

__attribute__((cdecl, force_align_arg_pointer))
int gmv_radio_pick(unsigned char *mm, int station)
{
    static int dumped;
    if (!dumped) {
        dumped = 1;
        radio_dump_stations(mm);
    }
    int slot = ((int (*)(void *, int))(uintptr_t)RADIO_PICK_FN)(mm, station);
    int base = *(int *)(mm + 0x250);
    gmv_trace("[radio] pick: station %d -> slot %d \"%s\" (base+slot \"%s\"); HUD logo %d %s; caller=%p\n",
            station, slot, radio_name(slot), radio_name(base + slot), g_radio_logo,
            radio_logo_name(g_radio_logo), __builtin_return_address(0));
    return slot;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_tune(unsigned char *mm, int station)
{
    int old = *(int *)(mm + 0x21c);
    uint32_t flags = *(uint32_t *)(mm + 0x220);
    ((void (*)(void *, int))(uintptr_t)RADIO_TUNE_FN)(mm, station);
    gmv_log("[radio] tune: station %d -> %d (flags 0x%x, now %d) caller=%p\n", old, station,
            flags, *(int *)(mm + 0x21c), __builtin_return_address(0));
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_start(unsigned char *mm, int station)
{
    int old = *(int *)(mm + 0x21c);
    uint32_t flags = *(uint32_t *)(mm + 0x220);
    ((void (*)(void *, int))(uintptr_t)RADIO_START_FN)(mm, station);
    if (GMV_DEV)
        gmv_log("[radio] start(%d): flags 0x%x -> 0x%x, station %d -> %d, slot %d%s caller=%p\n", station,
            flags, *(uint32_t *)(mm + 0x220), old, *(int *)(mm + 0x21c), *(int *)(mm + 0x218),
            (flags & 0x11) ? " (radio already on: ignored)" : "", __builtin_return_address(0));
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_logo(void *widget, int station)
{
    ((void (*)(void *, int))(uintptr_t)RADIO_LOGO_FN)(widget, station);
    void *ret = __builtin_return_address(0);
    int show = (uint32_t)(uintptr_t)ret == RADIO_SHOW_RET;
    if (show || station != g_radio_logo)
        gmv_log("[radio] HUD logo: station %d %s (%s)\n", station, radio_logo_name(station),
                show ? "Radio::show" : "refresh");
    g_radio_logo = station;
}

static int radio_redirect(const uint32_t *sites, size_t n, uint32_t fn, void *hook)
{
    for (size_t i = 0; i < n; i++) {
        if (!sm_is_call(sites[i], fn)) {
            gmv_log("[gamepatch] 0x%08x is not the expected call to 0x%08x; radio trace skipped\n", sites[i], fn);
            return 0;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (!patch_call(sites[i], hook)) {
            gmv_log("[gamepatch] write at 0x%08x failed (%lu); radio trace partly installed\n",
                    sites[i] + 1, GetLastError());
            return 0;
        }
    }
    return 1;
}

void apply_radio_trace(void)
{
    const char *e = gmv_dbgenv("GMV_RADIO_TRACE");
    if (!(gmv_env_on(e)))
        return;
#define RADIO_N(a) (sizeof (a) / sizeof *(a))
    if (radio_redirect(radio_pick_sites, RADIO_N(radio_pick_sites), RADIO_PICK_FN, (void *)gmv_radio_pick) &&
        radio_redirect(radio_tune_sites, RADIO_N(radio_tune_sites), RADIO_TUNE_FN, (void *)gmv_radio_tune) &&
        radio_redirect(radio_start_sites, RADIO_N(radio_start_sites), RADIO_START_FN, (void *)gmv_radio_start) &&
        radio_redirect(radio_logo_sites, RADIO_N(radio_logo_sites), RADIO_LOGO_FN, (void *)gmv_radio_logo))
        gmv_log("[gamepatch] radio trace installed: [radio] logs the station table, radio start, station "
                "switches, each pick and the HUD logo\n");
#undef RADIO_N
}

/* 0x9323a [mm+0x21c] 0x10a57c 0xf89fe 0x28564 0xf8ab7 */
#define RADIO_ON_FLAG    0x00647ed0u   /* 0x10a57c */
#define RADIO_IS_PLAYER  0x000617b2u
#define RADIO_ENTER_SITE 0x000f8aefu   /* 0xf89fe */
#define RADIO_ENTER_FN   0x000f6032u

static const uint32_t radio_entry_sites[] = { 0x0010a5f5u, 0x0010a65au };
static uint32_t g_radio_entry_next;  /* 0x60190 */
static int g_radio_predicted = -1;

/* 0x9323a */
static int radio_logo_show(const unsigned char *mm, int station)
{
    unsigned char *app = ((unsigned char *(*)(void))(uintptr_t)RADIO_APP_FN)();
    if (!app || *(int *)(app + 0x218) <= 0x34 || (*(const uint32_t *)(mm + 0x220) & 0x20))
        return -2;
    unsigned char *hud = *(unsigned char **)(app + 0x214);
    unsigned char *w = hud ? *(unsigned char **)(hud + 0xd0) : NULL;
    if (!w)
        return -2;
    int old = *(int *)(w + 0x14);
    ((void (*)(void *, int))(uintptr_t)RADIO_LOGO_FN)(w, station);
    g_radio_logo = station;
    return old;
}

__attribute__((cdecl, force_align_arg_pointer))
static int gmv_radio_enter(unsigned char *ch, int a, int b)
{
    unsigned char *car = *(unsigned char **)(ch + 0x55c);
    unsigned char *mm = radio_mm();
    if (car && mm && *(const unsigned char *)(uintptr_t)RADIO_ON_FLAG &&
        !(*(uint32_t *)(mm + 0x220) & 0x11) && ((int (*)(void *))(uintptr_t)RADIO_IS_PLAYER)(ch + 4)) {
        int station = (car[0x183] & 1) ? *(int *)(car + 0xc8) : *(int *)(car + 0x120);
        if (station == -1)
            station = *(int *)(mm + 0x21c);
        int old = radio_logo_show(mm, station);
        if (old != -2) {
            g_radio_predicted = station;
            if (old != station)
                gmv_trace("[radiologo] getting into a car with station %d: HUD logo %s -> %s\n", station,
                        radio_logo_name(old), radio_logo_name(station));
        }
    }
    return ((int (*)(void *, int, int))(uintptr_t)RADIO_ENTER_FN)(ch, a, b);
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_radio_entry(unsigned char *mm, int station)
{
    ((void (*)(void *, int))(uintptr_t)g_radio_entry_next)(mm, station);
    int now = *(int *)(mm + 0x21c), old = radio_logo_show(mm, now);
    if (g_radio_predicted >= 0 && g_radio_predicted != now)
        gmv_trace("[radiologo] the car radio started station %d, not the %d shown on getting in\n", now,
                g_radio_predicted);
    else if (old != -2 && old != now)
        gmv_trace("[radiologo] car radio on station %d: HUD logo %s -> %s\n", now, radio_logo_name(old),
                radio_logo_name(now));
    g_radio_predicted = -1;
}

int radio_logo_set(int on)
{
    const uint32_t entry = (uint32_t)(uintptr_t)gmv_radio_entry, enter = (uint32_t)(uintptr_t)gmv_radio_enter;
    const uint32_t trace = (uint32_t)(uintptr_t)gmv_radio_start;
    uint32_t next = g_radio_entry_next;
    if (!sm_is_call(RADIO_ENTER_SITE, RADIO_ENTER_FN) && !sm_is_call(RADIO_ENTER_SITE, enter))
        return -1;
    for (size_t i = 0; i < sizeof radio_entry_sites / sizeof *radio_entry_sites; i++) {
        uint32_t s = radio_entry_sites[i];
        if (next && sm_is_call(s, entry))
            continue;
        uint32_t to = sm_is_call(s, RADIO_START_FN) ? RADIO_START_FN : sm_is_call(s, trace) ? trace : 0;
        if (!to || (next && to != next))
            return -1;
        next = to;
    }
    g_radio_entry_next = next;
    for (size_t i = 0; i < sizeof radio_entry_sites / sizeof *radio_entry_sites; i++) {
        if (!patch_call(radio_entry_sites[i], (const void *)(uintptr_t)(on ? entry : next)))
            return -2;
    }
    if (!patch_call(RADIO_ENTER_SITE, (const void *)(uintptr_t)(on ? enter : RADIO_ENTER_FN)))
        return -2;
    g_radio_predicted = -1;
    return on;
}

void apply_radio_logo_fix(void)
{
    if (gmv_env_off(gmv_getenv("GMV_RADIO_LOGO_FIX")))
        return;
    int r = radio_logo_set(1);
    if (r == -1)
        gmv_log("[gamepatch] the car-entry calls (0x%08x, 0x%08x, 0x%08x) are not the expected ones; radio logo fix "
                "skipped\n", RADIO_ENTER_SITE, radio_entry_sites[0], radio_entry_sites[1]);
    else if (r == -2)
        gmv_log("[gamepatch] a write failed (%lu); radio logo fix partly applied\n", GetLastError());
    else
        gmv_log("[gamepatch] HUD radio logo shows the car's station as you get in (RADIO_LOGO_FIX=0 to disable)\n");
}
