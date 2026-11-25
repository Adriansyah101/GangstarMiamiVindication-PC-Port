#include <stdio.h>

#include "image.h"
#include "layout.h"
#include "log.h"
#include "macho.h"
#include "runinit.h"
#include "shims.h"
#include "veh.h"

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

/* esp */
__attribute__((force_align_arg_pointer))
int run_initializers(const macho *m, const gmvp *t, uint32_t limit, char *err, size_t errsz)
{
    const macho_sect *init = macho_sect_named(m, "__DATA", "__mod_init_func");
    if (!init || init->size % 4)
        FAIL("no usable __DATA,__mod_init_func section");
    uint32_t total = init->size / 4;
    const uint32_t *fns = (const uint32_t *)(uintptr_t)init->addr;
    uint32_t n = (limit && limit < total) ? limit : total;

    gmv_trace("\n== stage 2d: running %u of %u static initializer(s); GAME CODE EXECUTES NOW ==\n", n, total);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t run_addr = image_code_addr(t, fns[i]);
        gmv_trace("  init[%3u] @ 0x%08x (stored 0x%08x) ...\n", i, run_addr, fns[i]);
        veh_note_init(i, run_addr);
        void (*fn)(void) = (void (*)(void))(uintptr_t)run_addr;
        fn();
        veh_clear_init();
    }
    gmv_trace("== %u static initializer(s) returned normally ==\n", n);
    return 0;
}

__attribute__((force_align_arg_pointer))
int run_main(const macho *m, const gmvp *t, uint32_t main_addr, char *err, size_t errsz)
{
    if (!main_addr)
        FAIL("main() address is unknown (crt1 scan failed)");
    gmv_objc_init(m);
    gmv_appkit_init_constants();
    gmv_apply_gamepatch();
    uint32_t run_addr = image_code_addr(t, main_addr);

    static char arg0[] = GMV_GAME_NAME;
    static char *argv[] = {arg0, NULL};
    static char *envp[] = {NULL};
    static char apple0[] = GMV_GAME_NAME;
    static char *apple[] = {apple0, NULL};

    gmv_trace("\n== stage 3: calling main @ 0x%08x (argc=1); GAME main() RUNS NOW ==\n", run_addr);
    veh_note_main(run_addr);
    int (*mainfn)(int, char **, char **, char **) =
        (int (*)(int, char **, char **, char **))(uintptr_t)run_addr;
    int rc = mainfn(1, argv, envp, apple);
    gmv_trace("== main() returned %d ==\n", rc);
    return 0;
}
