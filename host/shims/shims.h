#ifndef GMV_SHIMS_H
#define GMV_SHIMS_H

#include <stdarg.h>
#include <wchar.h>
#include <stddef.h>
#include <stdint.h>

#include "game.h"
#include "imports.h"
#include "log.h"

extern const host_symbol gmv_host_symbols[];
extern const host_symbol gmv_host_symbols_win32[];
extern const host_symbol gmv_host_symbols_cxx[];
extern const host_symbol gmv_host_symbols_sdl[];
extern const host_symbol gmv_host_symbols_objc[];
extern const host_symbol gmv_host_symbols_appkit[];
extern const host_symbol gmv_host_symbols_str[];
extern const host_symbol gmv_host_symbols_stream[];
extern const host_symbol gmv_host_symbols_cg[];
extern const host_symbol gmv_game_host_symbols[];
extern const host_symbol gmv_host_symbols_gl[];

void gmv_shims_init(void);

void gmv_config_load(const char *exe_dir);
const char *gmv_getenv(const char *name);
static inline int gmv_env_flag(const char *v, int dflt)
{
    return v ? *v && *v != '0' : dflt;
}
static inline int gmv_env_on(const char *v)
{
    return v && *v && *v != '0';
}
static inline int gmv_env_off(const char *v)
{
    return v && *v == '0';
}
static inline uint32_t gmv_fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}
#if GMV_DEV
#define gmv_dbgenv(name) gmv_getenv(name)
#define gmv_envkey(name) getenv(name)
#else
#define gmv_dbgenv(name) ((const char *)0)
#define gmv_envkey(name) ((const char *)0)
#endif
const char *gmv_config_file_value(const char *name);
const char *gmv_config_live(const char *name);
int gmv_config_save(const char *name, const char *val);
void gmv_config_set_mac(int on);
int gmv_config_mac(void);
int gmv_config_tab(void);
void gmv_fopen_info(const char **fail_path, int *fail_errno, long *open_files);

void *gmv_shim_lookup(const char *name);

void *gmv_gl_resolve(const char *name);
void *gl_real(const char *name);
void gmv_gl_swap(void);
double gmv_pace_frame_ms(void);
void gmv_set_frame_ms(double ms);
int gmv_set_swap_interval(int v);
int gmv_set_msaa(int samples);
void gmv_gpu_profile_start(void);
int gmv_gpu_integrated(void);
int gmv_setting_is_auto(const char *v);
void gmv_gl_redraw(void);
void gmv_gl_client_size(int *w, int *h);
void gmv_gl_render_size(int *w, int *h);
void gmv_gl_view_rect(int *x, int *y, int *w, int *h, int *gw, int *gh);
struct SDL_Window *gmv_gl_window(void);
void *gmv_gl_window_hwnd(void);
void gmv_video_start(void);
void gmv_video_desktop(int *w, int *h);
void gmv_gl_make_current_raw(void);
void gmv_gl_present_raw(void);
void gmv_gl_apply_mode(void);
void gmv_gl_fade_mark(uint32_t caller, int main_fade);
void gmv_gl_death_capture_arm(int health, int max_health);
enum { GMV_PROF_TEX, GMV_PROF_BUF, GMV_PROF_IO, GMV_PROF_LOCK, GMV_PROF_WAIT, GMV_PROF_SLEEP,
       GMV_PROF_SWAP, GMV_PROF_N };
#if GMV_DEV
int gmv_prof_on(void);
int gmv_perf_on(void);
long long gmv_prof_now(void);
void gmv_prof_add(int kind, long long t0, long long bytes);
void gmv_prof_sleep_note(unsigned usec);
void gmv_prof_tick_end(unsigned frame, int drew);
void gmv_prof_tick_start(void);
void gmv_file_stats_report(double seconds);
void gmv_gl_query_report(double seconds);
void gmv_gl_draw_probe_report(double seconds);
void gmv_gl_query_note_int(void);
void gmv_sv_report(double seconds);
#else
#define gmv_prof_on() 0
#define gmv_perf_on() 0
#define gmv_prof_now() 0LL
#define gmv_prof_add(kind, t0, bytes) ((void)(t0))
#define gmv_prof_sleep_note(usec) ((void)0)
#define gmv_prof_tick_end(frame, drew) ((void)0)
#define gmv_prof_tick_start() ((void)0)
#endif

unsigned gmv_gl_frame_no(void);

void gmv_objc_init(const macho *m);

void gmv_objc_msgSend(void);
void *gmv_objc_new_instance(const char *clsname);
int gmv_objc_responds(void *self, const char *sel);
int gmv_objc_is_cfstring(const void *p);
const char *gmv_objc_instance_class_name(const void *p);

void *gmv_fnd_class_method(const char *cls, const char *sel);
void *gmv_fnd_instance_method(const char *cls, const char *sel);
void *gmv_fnd_stret_method(const char *cls, const char *sel);
void gmv_fnd_set_bundle_path(const char *path);
int gmv_in_bundle(const char *path);

#ifndef _WIN32
int gmv_ff_start(const char *const *argv, int *fd);
size_t gmv_ff_read(int fd, void *buf, size_t n);
int gmv_ff_stop(int pid, int fd);
char *gmv_ff_output(const char *const *argv);
#endif
#ifdef _WIN32
typedef wchar_t gmv_pathch;
#else
typedef char gmv_pathch;
#endif
typedef struct gmv_song gmv_song;
int gmv_song_builtin(const gmv_pathch *path);
gmv_song *gmv_song_open(const gmv_pathch *path);
unsigned gmv_song_rate(const gmv_song *s);
unsigned gmv_song_channels(const gmv_song *s);
const char *gmv_song_decoder(const gmv_song *s);
size_t gmv_song_read(gmv_song *s, int16_t *out, size_t frames);
void gmv_song_close(gmv_song *s);
long gmv_prefs_get_long(const char *key, long def);
void gmv_effective_screen(long *w, long *h, int *fullscreen);
void gmv_chosen_resolution(long *w, long *h);
void *gmv_make_nsstring(const char *s);
const char *gmv_nsstring_cstr(const void *o);
void gmv_nsformat(char *out, size_t cap, const char *fmt, va_list ap);
extern void    *gmv_au_render_cb;
extern void    *gmv_au_render_refcon;
extern double   gmv_au_sample_rate;
extern uint32_t gmv_au_channels, gmv_au_bits, gmv_au_format_flags, gmv_au_fmtid;
void gmv_audio_start(void);
void gmv_audio_stop(void);
void *gmv_snd_open(uint32_t rate, uint32_t chans, uint32_t bits);
void gmv_snd_write(void *st, const void *pcm, size_t len);
int gmv_snd_queued(void *st);
void gmv_snd_close(void *st);
uint32_t gmv_audio_frames(void);

void *gmv_make_nsevent(float x, float y);
void *gmv_make_nsscroll(float x, float y, float deltaY);

void gmv_input_pump(void);
void gmv_input_drain(int *skip);
unsigned gmv_input_presses(void);
int gmv_input_scancode_held(int scancode);
void gmv_input_set_client_size(int w, int h);
void gmv_input_view_changed(void);
void gmv_input_take_mouse_delta(int *dx, int *dy);
void gmv_input_set_cursor_hidden(int hidden);
int  gmv_input_is_cursor_hidden(void);
/* SDL 0x300 KEYDOWN 0x301 KEYUP */
int gmv_input_get_sdl_event(void *buf, unsigned minType, unsigned maxType);
void gmv_input_escape(int down);
void  gmv_joy_init(uint32_t flags);
void  gmv_joy_quit(uint32_t flags);
int   gmv_joy_count(void);
void *gmv_joy_open(int index);
int   gmv_joy_opened(int index);
void  gmv_joy_close(void *joystick);
void  gmv_joy_update(void);
int   gmv_joy_num_axes(void *joystick);
int   gmv_joy_num_buttons(void *joystick);
int   gmv_joy_num_hats(void *joystick);
int   gmv_joy_axis(void *joystick, int axis);
int   gmv_joy_button(void *joystick, int button);
int   gmv_joy_hat(void *joystick, int hat);
int   gmv_joy_style(void);
int   gmv_joy_any_button(void);
int   gmv_joy_peek(float *x, float *y, unsigned *buttons, int *hat);
unsigned gmv_joy_last_use(void);
unsigned gmv_input_last_key(void);
unsigned gmv_input_tick(void);
void *gmv_tis_input_source_property(void *source, void *key);
void *gmv_make_nsarray1(void *elem);
void gmv_appkit_init_constants(void);

void *gmv_timer_target(void);
const char *gmv_timer_selector(void);
void *gmv_timer_object(void);
int gmv_defer_take(void **target, const char **sel, void **arg);

void gmv_fire_movie_end(void);

const char *gmv_intro_movie_path(void);
void gmv_intro_play(void);

void *gmv_gl_context_init(void *self, const char *sel, void *format, void *share);
void *gmv_gl_make_current(void *self, const char *sel, ...);
void *gmv_gl_flush_buffer(void *self, const char *sel, ...);

#endif
