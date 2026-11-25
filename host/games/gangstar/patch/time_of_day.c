#include "compat.h"
#include <stdint.h>
#include <stdio.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

#define TOD_OBJ        0x0068a100u   /* 0x14360c 0x63de70 */
#define TOD_SET        0x0014172cu
#define TOD_UPDATE     0x00141ff4u   /* 0x6573a8 +8 */
#define TOD_WINDOWS    0x0068a230u   /* +0x130 */
#define TOD_HUD        0x00685668u   /* 0x63e038 */
#define TOD_DAY_MS     86400000
#define TOD_HOUR_MS    3600000
#define TOD_MIN_MS     60000
#define TOD_LOAD       0x000f393au   /* 0x96f4e 0xf3638 */
#define TOD_LOAD_SITE  0x001692bdu   /* 0x1691b0 0x169223 */

static const char *const tod_names[4] = { "MORNING", "DAY", "SUNSET", "NIGHT" };
static uint32_t tod_seed;

int tod_on(void)
{
    const char *e = gmv_config_live("GMV_TIME_OF_DAY");
    return gmv_env_on(e);
}

static int tod_random_on(void)
{
    const char *e = gmv_config_live("GMV_RANDOM_TIME_OF_DAY");
    return gmv_env_on(e);
}

static uint32_t tod_rand(uint32_t n)
{
    if (!tod_seed)
        tod_seed = (uint32_t)gmv_qpc() | 1u;
    tod_seed ^= tod_seed << 13;
    tod_seed ^= tod_seed >> 17;
    tod_seed ^= tod_seed << 5;
    return tod_seed % n;
}

static int tod_state(void)
{
    return *(const int *)(uintptr_t)(TOD_OBJ + 4) & 3;
}

/* 0x141788 */
static int tod_clock(void)
{
    int64_t ms = *(const int64_t *)(uintptr_t)(TOD_OBJ + 8) % TOD_DAY_MS;
    return (int)(ms < 0 ? ms + TOD_DAY_MS : ms);
}

static void tod_hhmm(char *s, size_t n)
{
    int ms = tod_clock();
    snprintf(s, n, "%02d:%02d", ms / TOD_HOUR_MS, ms / TOD_MIN_MS % 60);
}

void tod_log(const char *how)
{
    char t[8];
    tod_hhmm(t, sizeof t);
    gmv_log("[time] %s: %s at %s\n", how, tod_names[tod_state()], t);
}

/* 0x142892 */
__attribute__((cdecl, force_align_arg_pointer, noinline))
static void tod_set(int hour, int min)
{
    ((void (*)(uint32_t, int, int))(uintptr_t)TOD_SET)(TOD_OBJ, hour, min);
    if (ms_rd(SM_SKYBOX_PP) && ms_rd(TOD_HUD))
        ((void (*)(uint32_t, double))(uintptr_t)TOD_UPDATE)(TOD_OBJ, 0.0);
}

void tod_header(void *fx)
{
    char t[8];
    tod_hhmm(t, sizeof t);
    om_text(fx, "widgetActionKeyTitle.titleAction", tod_names[tod_state()]);
    om_text(fx, "widgetActionKeyTitle.titleKey", "");
    om_text(fx, "widgetActionKeyTitle.txtValue", t);
}

void tod_step(int d)
{
    if (!tod_on() || mr_busy())
        return;
    tod_set((tod_clock() / TOD_HOUR_MS + d + 24) % 24, 0);
    tod_log("SERVICES");
}

static int tod_random(void)
{
    const int32_t *w = (const int32_t *)(uintptr_t)TOD_WINDOWS;
    int s = (int)tod_rand(4), start = w[2 * s], len = w[2 * s + 1];
    if (start < 0 || start >= TOD_DAY_MS || len <= 0 || len > TOD_DAY_MS) {
        gmv_log("[time] the day's periods are not the expected ones (%d %d); the time left\n", start, len);
        return 0;
    }
    int min = (start + (int)tod_rand((uint32_t)len)) / TOD_MIN_MS % 1440;
    tod_set(min / 60, min % 60);
    return 1;
}

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_tod_load(void *game, int fresh)
{
    ((void (*)(void *, int))(uintptr_t)TOD_LOAD)(game, fresh);
    if ((fresh & 0xff) || !tod_random_on() || mr_busy())
        return;
    if (tod_random())
        tod_log("CONTINUE, random start");
}

void tod_start_hook(void)
{
    if (sm_is_call(TOD_LOAD_SITE, TOD_LOAD) && sm_redirect(TOD_LOAD_SITE, (void *)gmv_tod_load))
        gmv_log("[time] CONTINUE starts at a random time of day%s\n", tod_random_on() ? "" : "; off for now");
    else if (!sm_is_call(TOD_LOAD_SITE, (uint32_t)(uintptr_t)gmv_tod_load))
        gmv_log("[time] the main menu's CONTINUE is not the expected call; no random start time\n");
}

int om_set_tod_start(const char *key, double v)
{
    (void)key, (void)v;
    return sm_is_call(TOD_LOAD_SITE, (uint32_t)(uintptr_t)gmv_tod_load);
}
