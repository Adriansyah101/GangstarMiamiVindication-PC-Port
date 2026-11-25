#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0xf267c 0x6525c8 0xc653a 0xc645e 0xf3ee0 +0x34 0x782e8 */
#define AS_PROP_FN    0x000782e8u
#define AS_STUCK      0x54
#define AS_SLOTS      64

typedef uint32_t (*as_prop_fn)(uint32_t, uint32_t);
static struct { uint32_t ch; double due, seen; } g_as[AS_SLOTS];

__attribute__((cdecl, force_align_arg_pointer))
static uint32_t gmv_ai_prop(uint32_t ch, uint32_t id)
{
    if (id == AS_STUCK && g_as_on) {
        double now = ws_now_ms();
        unsigned i, slot = AS_SLOTS, old = 0;
        for (i = 0; i < AS_SLOTS; i++) {
            if (g_as[i].ch == ch) { slot = i; break; }
            if (g_as[i].seen < g_as[old].seen) old = i;
        }
        if (slot < AS_SLOTS && now - g_as[slot].seen < 100.0 && now < g_as[slot].due - g_ws_tick) {
            g_as[slot].seen = now;
            return 1;
        }
        if (slot == AS_SLOTS) {
            slot = old;
            g_as[slot].ch = ch;
            g_as[slot].due = 0;
        }
        g_as[slot].due = now - g_as[slot].due < WS_MAC_MS ? g_as[slot].due + WS_MAC_MS : now + WS_MAC_MS;
        g_as[slot].seen = now;
    }
    return ((as_prop_fn)(uintptr_t)AS_PROP_FN)(ch, id);
}

int as_installed;

static void ai_sync_in(double ms)
{
    /* NPC 0xf6660 */
    static const uint32_t slots[] = { 0x0064efbcu, 0x0064f7fcu }, calls[] = { 0x000f6673u, 0x000f6694u };
    static int failed;
    int want = !gmv_env_off(gmv_getenv("GMV_AI_SYNC")) && ms < 16.6;
    unsigned i;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ws_qpf = (double)f.QuadPart / 1000.0;
    g_ws_tick = ms;
    if (want && !as_installed && !failed) {
        for (i = 0; i < 2; i++)
            if (*(const uint32_t *)(uintptr_t)slots[i] != AS_PROP_FN || !sm_is_call(calls[i], AS_PROP_FN)) {
                gmv_log("[gamepatch] character property getter not at the expected places; AI sync skipped\n");
                failed = 1;
                return;
            }
        uint32_t fn = (uint32_t)(uintptr_t)gmv_ai_prop;
        for (i = 0; i < 2; i++) {
            patch_bytes(slots[i], (const unsigned char *)&fn, 4);
            patch_call(calls[i], (void *)gmv_ai_prop);
        }
        as_installed = 1;
        g_as_on = 1;
        gmv_log("[gamepatch] AI sync installed: characters' stuck check counts as at the Mac's 30 updates/s "
                "(%.3f ms ticks)\n", ms);
        return;
    }
    if (as_installed && g_as_on != want)
        gmv_log("[gamepatch] AI sync %s (%.3f ms ticks)\n", want ? "on" : "off", ms);
    g_as_on = want && as_installed;
}

void ai_sync_set(double ms)
{
    patch_undo *keep = patch_undo_now;
    patch_undo_now = &mac_undo;
    ai_sync_in(ms);
    patch_undo_now = keep;
}

void apply_ai_sync(void)
{
    ai_sync_set(gmv_pace_frame_ms());
}
