#ifndef GANGSTAR_H
#define GANGSTAR_H

#include <stdint.h>
#include <stddef.h>

int gmv_set_aniso(float level);
void gmv_set_menuflash_fix(int on);
int gmv_set_post(const char *key, float v);
int gmv_ds_active(void);
const float *gmv_ds_caster(void);
/* 0x659f28 */
uint32_t gmv_top_state_vtable(void);
void gmv_death_poll(void);
void gmv_mission_replay_tick(void);
void gmv_playerswitch_inventory_tick(void);
int gmv_spawn_fade_level(int add);
int gmv_uc_on(void);
int gmv_uc_init(void);
const char *gmv_uc_redirect(const char *path, char *buf, size_t n);
int gmv_uc_type(void);
int gmv_uc_res_base(void);
int gmv_uc_pick_type(void);
int gmv_uc_has_smoke(void);
void gmv_uc_tick(void);
int gmv_pak_unpack(const char *pak, const char *dir, const char *tag);
int gmv_rs_on(void);
int gmv_rs_have(void);
int gmv_rs_init(void);
const char *gmv_rs_redirect(const char *path, char *buf, size_t n);
int gmv_rs_counts(int st, int *mac, int *all);
int gmv_rs_is_song(int id);
int gmv_bp_grow(void *bp, uint32_t proxies, uint32_t pairs);
int gmv_bp_shrink(void *bp, uint32_t proxies, uint32_t pairs);
void gmv_b2_world_new(void);
void gmv_pool_tick(void);
void gmv_menu_loop_tick(void);
void gmv_test_spawn_car_tick(void);
void gmv_test_chase_tick(void);

void gmv_apply_broadphase(void);
void gmv_broadphase_report(double seconds);
void gmv_broadphase_tick(unsigned *made, unsigned *gone, int *bounds);

void gmv_apply_b2solve(void);
void gmv_b2solve_report(double seconds);

/* 0x131f1e */
void gmv_apply_bbfast(void);
void gmv_bbfast_report(double seconds);

void gmv_apply_hudbars(void);
void gmv_hudbars_report(double seconds);

void gmv_vbostatic_install(void);
void gmv_vbostatic_frame(void);
const void *gmv_vbostatic_client(unsigned name, uintptr_t off);
#if GMV_DEV
void gmv_vbostatic_report(double seconds);
#else
#define gmv_vbostatic_report(s) ((void)0)
#endif

#if GMV_DEV
void gmv_fog_skip_report(double seconds);
void gmv_stream_probe_report(double seconds);
void gmv_a2c_report(double seconds);
#endif

#define GMV_CR_STATION 5
int gmv_cr_enable(int on);
int gmv_cr_songs(void);
void gmv_cr_set_volume(float v);
void gmv_cr_mix(int16_t *buf, uint32_t frames, uint32_t chans, uint32_t rate);
int gmv_cutscene_key(unsigned sym);
void gmv_cutscene_pump(void);
int   gmv_pad_menu_active(void);
int   gmv_pointer_up(void);
int   gmv_padicons_on(void);
void  gmv_padicons_upload(int level, unsigned fmt, int w, int h, int size, const void *data);
void  gmv_padicons_deleted(int n, const unsigned *names);
void  gmv_padicons_sync(void);

#if GMV_DEV
void gmv_uifind_probe(void);
void gmv_b2step_probe(void);
void gmv_billboard_probe(void);              /* PERF_PROFILE 0x131f1e */
#else
#define gmv_uifind_probe() ((void)0)
#define gmv_b2step_probe() ((void)0)
#define gmv_billboard_probe() ((void)0)
#endif

int code_write(uint32_t va, const void *buf, size_t n);
int code_jmp(uint32_t site, const void *to);
int code_call(uint32_t site, const void *to);
uint32_t code_call_target(uint32_t site);

#endif
