#include <stddef.h>
#include <stdint.h>

#include "game.h"
#include "macho.h"
#include "shims.h"

void gmv_shims_init(void) {}
void *gmv_shim_lookup(const char *name) { (void)name; return NULL; }
void gmv_fnd_set_bundle_path(const char *path) { (void)path; }
void gmv_objc_init(const macho *m) { (void)m; }
void gmv_appkit_init_constants(void) {}
void gmv_apply_gamepatch(void) {}
void gmv_game_crash_note(const gmv_regs *r) { (void)r; }
void gmv_audio_stop(void) {}
void gmv_video_start(void) {}
void gmv_snd_init(void) {}
void gmv_joy_start(void) {}
void gmv_game_perf_report(double seconds) { (void)seconds; }
void gmv_game_perf_counts(unsigned *made, unsigned *gone, int *bounds) { *made = *gone = 0; *bounds = 0; }
