#ifndef GMV_GAME_H
#define GMV_GAME_H

#include <stddef.h>
#include <stdint.h>

typedef struct { const char *name, *section, *def, *help; } gmv_setting;
typedef struct { const char *name, *val; } gmv_preset_value;
typedef struct {
    const gmv_setting *list;
    size_t n;
    const char *header;
    const char *const (*section_help)[2];
    size_t nsections;
    const gmv_preset_value *preset;
    size_t npreset;
    const char *preset_log;
    const char *const *retired;
    size_t nretired;
    const gmv_preset_value *absent;
    size_t nabsent;
} gmv_settings_table;
extern const gmv_settings_table gmv_game_settings;

void gmv_apply_gamepatch(void);
typedef struct { uint32_t eip, eax, ecx, edx, ebx, esi, edi, ebp, esp; } gmv_regs;
void gmv_game_crash_note(const gmv_regs *r);

const char *gmv_game_redirect(const char *path, char *buf, size_t n);
void gmv_game_audio_mix(int16_t *buf, uint32_t frames, uint32_t chans, uint32_t rate);

int gmv_game_key(unsigned sym);
void gmv_game_input_pump(void);
void gmv_game_input_tick(void);
void gmv_game_alt_enter(void);
void gmv_game_full_screen_tick(void);
int gmv_game_pad_pointer(void);
int gmv_game_pointer_shown(void);

void gmv_game_tick(void);
void gmv_game_between_frames(void);
void gmv_game_frame_end(void);
int gmv_game_present_hold(unsigned frame, int drew);
void gmv_game_present_decided(unsigned frame, int presented);
void gmv_game_after_present(void);
void gmv_game_tick_period(double ms);
void gmv_game_minimized(int on);

void gmv_game_gl_setup(void);
void *gmv_game_gl_intercept(const char *name);
int gmv_game_gl_wants_draws(void);
/* __stdcall */
void gmv_game_draw_arrays(void *real, unsigned mode, int first, int count, void *caller);
void gmv_game_draw_elements(void *real, unsigned mode, int count, unsigned type, const void *idx, void *caller);
void gmv_game_gl_cap(unsigned cap, int on);
void gmv_game_gl_clear_begin(unsigned mask);
void gmv_game_gl_clear_end(unsigned mask);
void gmv_game_gl_client_unit(unsigned unit);
void gmv_game_gl_client_state(unsigned array, int on);
const void *gmv_game_gl_buffer_bytes(unsigned name, uintptr_t off);
void *gmv_game_gl_draw_intercept(const char *name);

void gmv_game_perf_report(double seconds);
void gmv_game_perf_counts(unsigned *made, unsigned *gone, int *bounds);

#endif
