#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* updateCells +0x1a4 */
#define OC_CELLS_SITE  0x00135a5au
#define OC_CELLS_FN    0x005ed73eu

#define OC_GRIDS 8
static struct { char *grid; float r1, r2, at[3]; } oc_grids[OC_GRIDS];
float oc_seed_r1, oc_seed_r2;
int dd_unhook;

void dd_take_out(void)
{
    patch_undo_all(&dd_undo);
    dd_on = dd_live = dd_unhook = 0;
    gmv_log("[gamepatch] draw distance hooks out: the game's own again\n");
}

#if GMV_DEV
/* 0x134fc0 +0x118 +0x08 +0x14 +0x20 +0x70 +0x2c */
static unsigned oc_probe_at, oc_origin_n;
static int oc_probe_on = -1;

static void oc_probe(char *grid, const float *pos)
{
    if (oc_probe_on < 0) {
        const char *e = gmv_dbgenv("GMV_COLLISION_PROBE");
        oc_probe_on = gmv_env_on(e);
    }
    unsigned now = GetTickCount();
    if (!oc_probe_on || now - oc_probe_at < 2000)
        return;
    oc_probe_at = now;
    uint32_t g = (uint32_t)(uintptr_t)grid, t = ms_rd(g + 0x118), cells = ms_rd(g + 0x16c);
    uint32_t bits = ms_rd(ms_rd(g + 0x178)), stride = ms_rd(t + 0x70), ents = ms_rd(t + 8);
    uint32_t ix = ms_rd(t + 0x14), segs = ms_rd(t + 0x20);
    int cols = (int)ms_rd(g + 0x148), rows = (int)ms_rd(g + 0x14c);
    const float *f = (const float *)(grid + 0x15c), h = 15000;
    static int told;
    if (!told++)
        gmv_log("[collision] grid %p: %d x %d cells of %.0f x %.0f m from %.0f %.0f\n", (void *)grid, cols, rows,
                cols > 0 ? f[2] / cols / 100 : 0, rows > 0 ? f[3] / rows / 100 : 0, f[0], f[1]);
    if (!t || !cells || !bits || cols <= 0 || rows <= 0 || f[2] <= 0 || f[3] <= 0)
        return;
    int x0 = (int)floorf((pos[0] - h - f[0]) * cols / f[2]), x1 = (int)ceilf((pos[0] + h - f[0]) * cols / f[2]);
    int y0 = (int)floorf((pos[1] - h - f[1]) * rows / f[3]), y1 = (int)ceilf((pos[1] + h - f[1]) * rows / f[3]);
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > cols ? cols : x1, y1 = y1 > rows ? rows : y1;
    int objs = 0, miss = 0, lost = 0, nload = 0, ncell = 0, shown = 0, kinds[6] = { 0 };
    for (int x = x0; x < x1; x++)
        for (int y = y0; y < y1; y++) {
            uint32_t bit = (uint32_t)(y * cols + x), c = cells + (uint32_t)(x * rows + y) * 12;
            int on = (int)(ms_rd(bits + (bit >> 5) * 4) >> (bit & 31) & 1);
            ncell++;
            nload += on;
            for (uint32_t q = ms_rd(c), e = ms_rd(c + 4); q && q < e; q += 4) {
                uint32_t id = ms_rd(ms_rd(q) + 4), d = ms_rd(ents + id * stride + 0x2c);
                uint32_t type = d ? ms_rd(d) & 0xffff : 0, v = d ? ms_rd(d + 4) : 0;
                if (type < 1 || type > 5 || !v)
                    continue;
                uint32_t k = (ms_rd(segs + ms_rd(ix + id * 8) * 0x14 + 0xc) & 0xffff) + ms_rd(ix + id * 8 + 4);
                const float *b = (const float *)(uintptr_t)ms_rd(ents + k * stride + 0xc);
                if (!b || b[0] > pos[0] + h || b[3] < pos[0] - h || b[1] > pos[1] + h || b[4] < pos[1] - h)
                    continue;
                objs++;
                if (ms_rd(v + 4) - ms_rd(v) > 3)
                    continue;
                if (!on) {
                    lost++;
                    continue;
                }
                miss++;
                kinds[type]++;
                if (shown++ < 3) {
                    float cx = f[0] + f[2] * x / cols, cy = f[1] + f[3] * y / rows;
                    float dx = (b[0] + b[3]) / 2 - pos[0], dy = (b[1] + b[4]) / 2 - pos[1];
                    gmv_log("[collision]   no body: object %u type %u, box %.0f %.0f .. %.0f %.0f, %.0f m away, "
                            "cell %d %d (%.0f %.0f .. %.0f %.0f)\n", id, type, b[0], b[1], b[3], b[4],
                            sqrtf(dx * dx + dy * dy) / 100, x, y, cx, cy, cx + f[2] / cols, cy + f[3] / rows);
                }
            }
        }
    gmv_log("[collision] probe at %.0f %.0f %.0f: %d static objects within %.0f m, %d of %d cells loaded, "
            "%d without a body (types 1-5: %d %d %d %d %d), %d in cells not loaded\n", pos[0], pos[1], pos[2], objs,
            h / 100, nload, ncell, miss, kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], lost);
}
#endif

__attribute__((cdecl, force_align_arg_pointer))
static void gmv_update_cells(char *grid, float *pos, float r1, float r2)
{
    float *last = (float *)(grid + 0x1a4);
    int zero = last[0] == 0 && last[1] == 0 && last[2] == 0, i, slot = 0;
#if GMV_DEV
    if (!pos[0] && !pos[1] && !pos[2] && oc_origin_n++ < 20)
        gmv_log("[collision] camera at the world origin (%u), the grid's last position %.0f %.0f %.0f\n", oc_origin_n,
                last[0], last[1], last[2]);
    oc_probe(grid, pos);
#endif
    for (i = 0; i < OC_GRIDS && oc_grids[i].grid != grid; i++)
        if (!oc_grids[i].grid)
            slot = i;
    if (i < OC_GRIDS)
        slot = i;
    int fresh = zero && !(i < OC_GRIDS && !oc_grids[i].at[0] && !oc_grids[i].at[1] && !oc_grids[i].at[2]);
    if (fresh && dd_scale > 1)
        last[0] = last[1] = last[2] = -1e7f;
    float o1 = r1, o2 = r2;
    if (i < OC_GRIDS && !fresh)
        o1 = oc_grids[slot].r1, o2 = oc_grids[slot].r2;
    else if (!fresh && oc_seed_r1 > 0)
        o1 = oc_seed_r1, o2 = oc_seed_r2;
    if (o1 != r1)
        gmv_log("[gamepatch] city cells: radius %.0f -> %.0f m\n", o1 / 100, r1 / 100);
    oc_grids[slot].grid = grid, oc_grids[slot].r1 = r1, oc_grids[slot].r2 = r2;
    memcpy(oc_grids[slot].at, pos, sizeof oc_grids[slot].at);
    ((void (*)(char *, float *, float, float, float, float))(uintptr_t)OC_CELLS_FN)(grid, pos, r1, r2, o1, o1 + o2);
    if (dd_unhook) {
        int all = 1;
        for (i = 0; i < OC_GRIDS; i++)
            all &= !oc_grids[i].grid || oc_grids[i].r1 == r1;
        if (all)
            dd_take_out();
    }
}

void apply_origin_collision_fix(void)
{
    if (!dd_on)
        return;
    if (!sm_is_call(OC_CELLS_SITE, OC_CELLS_FN)) {
        gmv_log("[gamepatch] cell update not the expected bytes; buildings near the world origin (most of "
                "the city with DRAW_DISTANCE) may have no collision\n");
        return;
    }
    if (!patch_call(OC_CELLS_SITE, (void *)gmv_update_cells))
        return;
    if (dd_scale > 1)
        gmv_log("[gamepatch] buildings near the world origin keep their collision (needed by DRAW_DISTANCE)\n");
    if (!tab_live())
        return;
    if (dd_cells_aim()) {
        dd_live = 1;
        gmv_log("[gamepatch] draw distance ready for the options menu (%.2gx now)\n", dd_scale);
    } else
        gmv_log("[gamepatch] cell update's radii not the expected bytes; DRAW_DISTANCE takes effect at the next start\n");
}
