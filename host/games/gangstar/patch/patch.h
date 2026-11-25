#ifndef GMV_PATCH_H
#define GMV_PATCH_H

#include "compat.h"
#include <stdint.h>

#define PATCH_UNDO_MAX 160
typedef struct {
    int n;
    struct { uint32_t va; unsigned n; unsigned char was[16]; } e[PATCH_UNDO_MAX];
} patch_undo;
extern patch_undo *patch_undo_now;
extern patch_undo mac_undo;
#if GMV_DEV
#define MA_SITES 256
typedef struct { uint32_t va; unsigned n; unsigned char was[16]; } ma_site_t;
extern ma_site_t ma_site[MA_SITES];
extern int ma_sites;
#endif
int patch_bytes(uint32_t va, const unsigned char *buf, size_t n);
void patch_undo_all(patch_undo *u);
void emit_jmp(unsigned char *tr, size_t *o, uint32_t dst);
void emit_jmp_abs(unsigned char *tr, size_t *o, uint32_t dst);
int patch_jmp(uint32_t site, const void *to, size_t len);
int patch_call(uint32_t site, const void *to);

#if GMV_DEV
void apply_bakershare_log(void);
#endif
void apply_vtxfix(void);

extern volatile float g_carsnd_last_speed;
void apply_carsnd_trace(void);
void apply_veh_trace(void);

void apply_crash58_guard(void);

int stoplock_set(int on);
void apply_stoplock_fix(void);

int ai_steer_set(int on);
void apply_ai_steer(void);

int boat_seat_set(int on);
void apply_boat_seat(void);
#if GMV_DEV
int boat_probe_on(void);
void boat_probe_note(int cmd);
#endif

#define MAIN_MENU_VT   0x00659f28u
int menu_fade_clicks_set(int on);
void apply_menu_fade_clicks(void);

int fade_edge_set(int on);
void apply_fade_edge_fix(void);

int load_sleep_skip_set(int on);
void apply_load_sleep_skip(void);
int load_budget_set(int n);
void apply_load_budget(void);
void apply_fast_load(void);

void apply_engvol_trace(void);
void apply_engsrc_trace(void);
void apply_engdtor_trace(void);

void apply_frametick_probe(void);

void apply_fade_trace(void);
void apply_cine_trace(void);

extern int g_phys_on;
extern int kick_tried, grip_tried;
extern int phys_installed;
void physics_sync_set(double ms);
void apply_physics_sync(void);

extern int g_cam_sync;
extern int cs_mgr_hooked, cs_installed;
void cam_sync_set(double ms);
void apply_cam_follow(void);

#define WS_MAC_MS     33.333
extern double g_ws_tick;
extern double g_ws_qpf;
extern int g_ws_on, g_as_on;
double ws_now_ms(void);
extern int ws_installed;
void walk_sync_set(double ms);
void apply_walk_sync(void);

extern int as_installed;
void ai_sync_set(double ms);
void apply_ai_sync(void);

void apply_solo_car(void);

#define RADIO_PICK_FN   0x0005f3e8u
#define RADIO_TUNE_FN   0x0006000cu
#define RADIO_LOGO_FN   0x0017357eu
__attribute__((cdecl, force_align_arg_pointer))
int gmv_radio_pick(unsigned char *mm, int station);
void apply_radio_trace(void);
#define RADIO_APP_FN     0x0015e4fau   /* [+0x214] HUD [+0x218] */
#define RADIO_MM_PP      0x0063e030u
static inline unsigned char *radio_mm(void)
{
    unsigned char **mmp = *(unsigned char ***)(uintptr_t)RADIO_MM_PP;
    return mmp ? *mmp : NULL;
}
int radio_logo_set(int on);
void apply_radio_logo_fix(void);

#define CR_GAME_STATIONS 5
extern int g_cr_hooked, g_cr_on;
extern int g_cr_row_want, g_cr_row;
#define CR_VISIBLE     0x002fdf80u
#define CR_TITLE0      (-1397)
#define CR_BAR0        (-868)
#define CR_STEP        1760
#define CR_OFF         40000
void cr_page_mac(void);
void cr_mac_switch(int mac);
void cr_logo_hide_tick(void);
void cr_file_wants(void);
void apply_custom_radio(void);

void apply_short_warning(void);

#define CS_TEXTS       0x0016fbeeu
#define CS_ARROW_SHOW  0x0015ebfau
#define CS_ARROW_HIDE  0x0015eb48u
#define CS_CURSOR_SET  0x00181d14u
#define CS_APP         0x00a064a0u              /* 0x15e4fa +0x214 +0x218 */
#define CS_APP_GUARD   0x00a066d8u
void cs_btn_visible(void *fx, int on);
extern const uint32_t cs_hide_sites[2];
extern volatile int g_cs_on, g_cs_hooked;
extern volatile int g_cs_line;
extern volatile int g_cs_skip, g_cs_next;
unsigned char *cs_widget(void);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_texts(unsigned char *w);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_show(void *app);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_hide(void *app);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_cursor(void *cursor, int on);

extern volatile int g_tk_on, g_tk_ok, g_tk_press, g_tk_answer;
int tk_set(int on);
void apply_tutorial_keys(void);
int cs_set(int on);
void apply_cutscene_keys(void);

#define PL_AIM        0x100
#define PL_BTNS       16
enum { PL_FOOT, PL_CAR, PL_FLY, PL_SNIPER, PL_KINDS };
extern const int pl_map[PL_KINDS][PL_BTNS];
extern volatile int g_pl_on, g_pl_ok, g_pl_aim_btn;
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call1(uint32_t fn, uint32_t a);
extern int g_pl_cam_hooked;
#define PL_MOTO_VT  0x00656228u
int pl_set(int on);
#define PL_LABEL_FN    0x0008f426u
extern const uint32_t pl_scheme_vt[PL_KINDS];
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call2(uint32_t fn, uint32_t a, uint32_t b);
__attribute__((noinline, force_align_arg_pointer, optimize("no-optimize-sibling-calls")))
uint32_t pl_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);
void apply_pad_layout(void);

int pm_set(int on);
int pointer_shown(void);
void apply_pad_menu(void);

extern int g_ph_on, g_ph_ok;
int ph_set(int on);
void apply_pad_hints(void);

extern volatile int g_km_on, g_km_ok;
int km_set(int on);
void apply_key_move(void);

#define SSF_MAIN_PP   0x00689ea4u
#define SSF_CTRL_PP   0x00689ea8u    /* *0x63e010 */
#define SSF_PLAYER_VT 0x00655de8u
extern volatile int g_ssf_on, g_ssf_ok;
int ssf_set(int on);
__attribute__((cdecl, force_align_arg_pointer))
uint32_t gmv_ssf_write(uint32_t save, uint32_t path);
void apply_switch_save_fix(void);
void apply_mission_save_fix(void);

int si_set(int on);
void apply_playerswitch_inventory(void);

extern int mg_more, mg_live;
int hide_main_menu_set(int more, int live);
void hide_main_menu_unhook(void);
void apply_hide_more_games(void);

#define WS_PRESS_SITE   0x0011e269u     /* mov esi,[ebp-0x10]: */
#define WS_RELEASE_SITE 0x0011e2aeu     /* mov eax,[esi+0x48]: */
void apply_weapon_sprint(void);

int minimap_shops_set(int on);
void apply_minimap_shops(void);

#define SM_TOD_FN     0x00142892u
#define SM_TRACK_FN   0x00142690u
#define SM_SKYBOX_PP  0x0068a6e8u
extern const uint32_t sm_track_sites[2];
extern int g_sm_on;
extern int g_sky_glow_iphone;
extern int g_sm_hooked;
typedef void (*sm_vfn_i)(void *, int);
void **sm_vt(void *obj);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_tod(unsigned char *tod);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_track(unsigned char *smgr, unsigned char *disc, unsigned char *anchor, void *glow);
/* 0x676a70 */
#define SM_BB_RENDER       0x0046a580u
#define SM_BB_RENDER_THUNK 0x0046a570u
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render(unsigned char *n, int draw);
__attribute__((cdecl, force_align_arg_pointer))
void gmv_sun_moon_render_sn(unsigned char *sn, int draw);
void sm_discs_behind_farcity(void);
int sm_is_call(uint32_t site, uint32_t fn);
int sm_redirect(uint32_t site, void *hook);
int tab_live(void);
int tables_live(void);
int bloom_clock_on(void);
void sm_mode_set(int mode);
int sun_moon_set(int mode);

void ds_show_all(void);
int ds_tree_has(int t, uint32_t node, int add);
void apply_sun_moon(void);

#define DD_PROXY_FN    0x001b9e50u
#define DD_MAX_PROXIES 0x00649680u
#define DD_WORLD       0x006856c4u
#define DD_WORLD_BP    0x000191d8u
#define DD_PROFILE     0x00949a00u   /* +0x0c +0x10 */
#define DD_RADIUS      0x00a06d78u   /* 0x1a5d2a */
#define DD_RADIUS_SET  0x00a06d70u
#define DD_KEEP        0x00a06d88u   /* 0x5f096c */
#define DD_KEEP_SET    0x00a06d80u
extern const uint32_t dd_proxy_sites[2];
extern int dd_mac_profile[5];
extern double dd_scale;
__attribute__((cdecl, force_align_arg_pointer))
int gmv_create_proxy(char *bp, void *aabb, void *shape);
extern int dd_on;
extern int dd_live;
extern int dd_tables_in;
extern patch_undo dd_undo, dd_tables_undo;
void dd_tables_fit(double s);
int dd_set(double s);
void apply_draw_distance(void);
int dd_cells_aim(void);

extern float sky_k;
extern int g_fog_all, g_sky_draw, g_cp_draw;
extern int fog_sky_wrapped;
void apply_fog(void);
void apply_checkpoint_glow(void);
void apply_sky_horizon(void);

extern float oc_seed_r1, oc_seed_r2;
extern int dd_unhook;
void dd_take_out(void);
void apply_origin_collision_fix(void);

#define SV_SITE      0x00082af8u   /* call 0x080272 */
#define SV_WORLD     0x0068551cu   /* +0x414 +0x418 */
#define PD_MAKE_SITE   0x00082bd1u   /* call 0x082504 */
#define PD_MAKE_FN     0x00082504u
typedef struct {
    const char *env;
    int kind, cap, prof;
    uint32_t cap_imm, rec, d0, d1;
} pd_kinds_t;
extern const pd_kinds_t pd_kinds[3];
extern double pd_scale[3];
extern double pd_user[3];
extern int pd_on, pd_racing;
int pd_write(void);
extern int sf_hooked;
__attribute__((cdecl, force_align_arg_pointer, noinline))
int gmv_spawn_make(uint32_t sp, uint32_t loc);
#define PD_POOL_OLD   0x00689180u
#define PD_POOL_N     2048
extern uint32_t pd_pool[PD_POOL_N];
extern int pd_pool_on;
extern double pd_built[3];
extern int pd_extra_n;
int pd_is_extra(uint32_t o);
int pd_object_known(uint32_t o);
void cr_cars_off_sixth(void);
void pd_pool_top_up(void);
void pd_unmove_pool(void);
#if GMV_DEV
void pd_ws_left(uint32_t ch);
void pd_ws_report(uint32_t ch, uint32_t vt_was);
#endif
extern int pd_world_gen, pd_trim_gen;
extern int pd_centres_on;
__attribute__((cdecl, force_align_arg_pointer))
int gmv_can_spawn(uint32_t sp);
void apply_spawn_hook(void);

extern int sf_n, sf_live, sf_depth, sf_lost;
extern int sf_menu;
void sf_before_make(void);
void sf_after_make(void);
int spawn_fade_set(int on);
void apply_spawn_fade(void);
extern int mac_features_on, mac_out_pending, uc_loaded;

extern patch_undo pd_undo;
extern int dd_tables_back;
void dd_hooks_in(void);
void crowd_hooks_in(void);
void mac_hooks_in(void);
void mac_hooks_out(void);
void dd_tables_tick(void);

#define MS_OWNER     0x00689e98u
#define MS_STRINGS   0x17ac4       /* 0x8f426 */
#define MS_NONE_ID   0x1054
uint32_t ms_rd(uint32_t a);
const char *ms_text(uint32_t id, char *out, size_t n);
/* 0x35d08 0x94f300 +0x2dc +0x2ec +0x10 +0x18 +0x1c */
#define MS_SCRIPTS   0x0094f300u
#define MS_SC_MAX    1024
#define MS_CMD_MAX   400
int ms_cmd_type(uint32_t c);

#define OM_SET_ENTRY   0x00172f0eu
#define OM_SET_VISIBLE 0x002fdf80u
#define OM_SET_TEXT    0x002fdea0u
#define OM_MOVE        0x002fdf30u
#define OM_STRING_FN   0x0008f426u
#define OM_CLICK_FN    0x0016eff4u
#define OM_REG_FN      0x002fd1f0u
#define OM_SWITCH_FN   0x0015e71au
#define OM_LISTEN      0x0016eb12u
#define OM_TAB_SELECT  0x00180544u
#define OM_VT_N        17
#define OM_VT_SHOW     5             /* 0x169a6a */
#define OM_VT_HIDE     6
#define OM_VT_EVENT    15            /* 0x169b62 */
#define OM_ALLOC       0x000904e0u
#define OM_CAT_VT      0x00659388u
#define OM_CAT_SLOTS   7
#define OM_CAT_EVENT   0x0017802au
#define OM_CAT_TEXTS   0x001720b0u
#define OM_PLAIN_SHOW  0x00177f58u
#define OM_PLAIN_HIDE  0x00177fbcu
#define OM_KEEP        0x0016e9e6u
#define OM_W_SHOW      1
#define OM_W_HIDE      2
#define OM_W_KEEP      3
#define OM_W_EVENT     5
#define OM_W_TEXTS     6
#define OM_W_TABS      1
#define OM_W_PAGER     0x12
#define OM_W_CAT       0x22
#define OM_LINES       6
#define MR_TAB_ID      0x7f01
#define TOD_TAB_ID     0x7f02
#define TOD_SLOT       5
#define OM_SEL_X0      1429
#define OM_SEL_X1      7951
#define OM_SEL_Y0      21
#define OM_SEL_Y1      1202
extern int om_tab_files;
extern int om_credit;
extern int om_selectors;
extern void *(*om_opener)(const char *);
extern const char *const om_sel_names[4];
void *om_mgr(void);
unsigned char *om_widget(int i);
void *om_app(void);
void om_sound(int id);
void om_visible(void *fx, const char *path, int on);
void om_text(void *fx, const char *path, const char *text);
void om_line_visible(void *fx, int line, const char *field, int on);

uint32_t om_bits(const unsigned char *p, size_t *bit, int n);
int32_t om_sbits(const unsigned char *p, size_t *bit, int n);
void om_put_bits(unsigned char *p, size_t *bit, uint32_t v, int n);
int om_sbits_n(int32_t v);
typedef struct {
    int depth, cid, matrix, rotated;
    int32_t sx, sy, tx, ty;
    const char *name;
} om_place;
size_t om_tag(const unsigned char *s, size_t p, size_t end, int *code, size_t *data, size_t *len);
int om_sprite(const unsigned char *s, size_t start, size_t n, int cid, size_t *data, size_t *len);
int om_find_place(const unsigned char *s, size_t from, size_t end, const char *name, om_place *o);
size_t om_place_write(unsigned char *o, int depth, const om_place *m, const char *name);
unsigned char *om_swf_patch(const unsigned char *s, size_t n, size_t *out_n);

#define OM_SHOW_CREDIT 0
#if OM_SHOW_CREDIT
#define OM_CREDIT   "PATCH BY ADRIANSYAH101"
#endif
int om_opener_hook(void);
void om_selectors_visible(void *fx, int line, int on);
unsigned om_resolution(int i, unsigned *h);
int om_density(const char *key, double v);
int om_set_sprint(const char *key, double v);
int om_set_minimap(const char *key, double v);
int om_set_spawn_fade(const char *key, double v);

void rs_tick(void);
void om_pages(unsigned char *pager, int n, int i);
const char *om_event_path(unsigned char *ev);
int tab_label_hook(void);
void apply_options_tab(void);

#define MR_WORLD_REF   0x0063e170u   /* +0x418 +0x430 +0x424 */
#define MR_PLAYER      0x00689ea8u   /* 0x689ea4 */
#define MR_POS_BYTES   0x14
#define MR_FADE        0x006424b4u   /* 0x34918 -999 */
#define MR_FADE_NONE   (-999)
#define MR_OBJS        64
#define MR_VARS        18
extern uint32_t mr_unlock_prev;
enum { MR_IDLE, MR_ARMED, MR_STARTING, MR_UNDER_WAY, MR_ENDING };
typedef struct {
    int phase, id, at, opener, last_stop, passed, failed, held, n;
    uint32_t sc, mgr, list;
    DWORD t;
    uint32_t vars[MR_VARS], flags[8], tries;
    uint32_t mission_sc;
    unsigned char tutorial;
    int cine;
    uint32_t player;
    unsigned char pos[MR_POS_BYTES];
    int no;
    uint32_t o_name[MR_OBJS], o_obj[MR_OBJS];
    uint16_t o_flags[MR_OBJS];
    unsigned char o_seen[MR_OBJS];
    DWORD watch;
    uint32_t s_sc[MS_SC_MAX], s_time[MS_SC_MAX];
    int s_st[MS_SC_MAX], s_pc[MS_SC_MAX];
} rp_t;
extern rp_t rp;
uint32_t mr_mgr(void);
typedef struct { int phase, a; DWORD t; } mf_t;
extern mf_t mf;
__attribute__((cdecl, force_align_arg_pointer))
int gmv_mr_save(void *save, void *path);
void mr_save_hooks(void);
void apply_mission_replay(void);
int om_set_replay(const char *key, double v);
int mr_busy(void);
int tod_on(void);
void tod_header(void *fx);
void tod_step(int d);
void tod_log(const char *how);
void tod_start_hook(void);
int om_set_tod_start(const char *key, double v);

#if GMV_DEV
__attribute__((cdecl, force_align_arg_pointer))
void gmv_ml_unlock(uint32_t c);
void apply_menu_loop(void);
#endif
#if !(GMV_DEV)
void apply_menu_loop(void);
#endif

extern int uc_hooks;
extern patch_undo uc_undo;
typedef struct { float pos[3], ang; uint32_t cell, tri; int have; } uc_spot_t;
extern const uc_spot_t uc_beach;
typedef struct {
    uc_spot_t spot, cand;
    uint32_t obj, mgr, list, last_veh, survey_car;
    DWORD t;
    int said;
} uc_t;
extern uc_t uc;
__attribute__((cdecl, force_align_arg_pointer, noinline))
void gmv_uc_engine(uint32_t v);
__attribute__((cdecl, force_align_arg_pointer, noinline))
void obj_vec(uint32_t a, uint32_t c, float *out);
__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_pos(uint32_t obj, float *out);
float uc_dist(const float *a, const float *b);
int uc_is_car(uint32_t o);
int uc_lion_done(uint32_t mgr);
__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_place(uint32_t o, const uc_spot_t *s, int fresh);
int uc_category(int type);
__attribute__((cdecl, force_align_arg_pointer, noinline))
uint32_t uc_make(uint32_t world, int type, const uc_spot_t *s);
__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_leave(uint32_t world, uint32_t p, int type);
void lc_pos(uint32_t o, float *p);

int lc_set(int on);
void lc_tick(void);
void apply_lc_switch(void);
#if GMV_DEV
void ma_snapshot(void);
#endif
void uc_hooks_in(void);
void apply_uc_crusher(void);
void apply_radio_songs(void);

void mac_features_in(void);
void mac_features_out(void);

#endif
