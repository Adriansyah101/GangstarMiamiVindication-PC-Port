#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "log.h"
#include "layout.h"
#include "shims.h"
#include "veh.h"

typedef void *(*msg_t)(void *self, const char *sel, ...);

extern void *gmv_objc_last_eaglview;
extern void *gmv_objc_last_gamewindow;
extern void *gmv_objc_last_gamedelegate;

int gmv_input_wants_tap;

static unsigned char g_cs_keys[SDL_SCANCODE_COUNT];
static int g_alt_enter;

static int g_client_w = 1024, g_client_h = 768;
static int g_key_pending;
static int g_key_inited;

struct minput { int kind; float x, y, d; };
#define MAXIN 128
static struct minput g_q[MAXIN];
static int g_head, g_tail;
static int g_button_down;

/* 0x3f4a CGGetLastMouseDelta 0x59b700 */
static int   g_look_dx, g_look_dy;
static int   g_game_cursor_hidden;
static int   g_focus;

static void look_clip(void)
{
    SDL_Window *w = gmv_gl_window();
    if (!w)
        return;
    SDL_Rect r;
    gmv_gl_view_rect(&r.x, &r.y, &r.w, &r.h, NULL, NULL);
    SDL_SetWindowMouseRect(w, &r);
}

void gmv_input_set_cursor_hidden(int hidden)
{
    hidden = hidden ? 1 : 0;
    if (hidden != g_game_cursor_hidden) {
        g_game_cursor_hidden = hidden;
        static int tr = -1;
        if (tr < 0) { const char *e = gmv_dbgenv("GMV_INPUT_TRACE"); tr = (gmv_env_on(e)) ? 1 : 0; }
        if (tr) gmv_trace("[input] game cursor %s\n", hidden ? "HIDDEN" : "SHOWN");
    }
}

int gmv_input_is_cursor_hidden(void)
{
    return g_game_cursor_hidden;
}

static void look_add_raw(int dx, int dy)
{
    g_look_dx += dx;
    g_look_dy += dy;
    static int tr = -1;
    if (tr < 0) { const char *e = gmv_dbgenv("GMV_INPUT_TRACE"); tr = (gmv_env_on(e)) ? 1 : 0; }
    if (GMV_DEV && tr) { static int lc; if ((lc++ % 30) == 0)
        gmv_log("[input] raw look delta=(%d,%d) accum=(%d,%d)\n", dx, dy, g_look_dx, g_look_dy); }
}

void gmv_input_take_mouse_delta(int *dx, int *dy)
{
    if (dx) *dx = g_look_dx;
    if (dy) *dy = g_look_dy;
    g_look_dx = g_look_dy = 0;
}

static float g_ptr_x = 512.0f, g_ptr_y = 384.0f;
static int g_pad_moved;

static void q_push(int kind, float x, float y)
{
    int n = (g_head + 1) % MAXIN;
    if (kind <= 3) {
        g_ptr_x = x;
        g_ptr_y = y;
    }
    if (n == g_tail)
        return;
    g_q[g_head].kind = kind;
    g_q[g_head].x = x;
    g_q[g_head].y = y;
    g_q[g_head].d = 0.0f;
    g_head = n;
}

static void q_push_scroll(float x, float y, float dy)
{
    int n = (g_head + 1) % MAXIN;
    if (n == g_tail)
        return;
    g_q[g_head].kind = 8;
    g_q[g_head].x = x;
    g_q[g_head].y = y;
    g_q[g_head].d = dy;
    g_head = n;
}

void gmv_input_set_client_size(int w, int h)
{
    if (w > 0) g_client_w = w;
    if (h > 0) g_client_h = h;
}

void gmv_input_view_changed(void)
{
    look_clip();
}

/* transformMousePoint 0x19a4d0 */
static void to_game(int x, int y, float *gx, float *gy)
{
    int vx = 0, vy = 0, vw = g_client_w, vh = g_client_h, sw = g_client_w, sh = g_client_h;
    gmv_gl_view_rect(&vx, &vy, &vw, &vh, &sw, &sh);
    float fx = (float)(x - vx) * (float)sw / (float)vw;
    float fy = (float)(y - vy) * (float)sh / (float)vh;
    if (fx < 0.0f) fx = 0.0f;
    if (fx > (float)(sw - 1)) fx = (float)(sw - 1);
    if (fy < 0.0f) fy = 0.0f;
    if (fy > (float)(sh - 1)) fy = (float)(sh - 1);
    *gx = fx * GMV_VIEW_FRAME_W / (float)sw;
    *gy = GMV_VIEW_FRAME_H - fy * GMV_VIEW_FRAME_H / (float)sh;
}

static void q_push_at(int kind, int x, int y)
{
    float gx, gy;
    to_game(x, y, &gx, &gy);
    q_push(kind, gx, gy);
}

/* 0x18a9e SDL_PeepEvents 0x300 0x301 ev+0x10 */
struct kinput { unsigned type; int sym, scancode; };   /* 0x300 0x301 */
#define MAXKIN 64
static struct kinput g_kq[MAXKIN];
static int g_khead, g_ktail;

static void kq_push(unsigned type, int sym, int sc)
{
    int n = (g_khead + 1) % MAXKIN;
    if (n == g_ktail)
        return;
    g_kq[g_khead].type = type;
    g_kq[g_khead].sym = sym;
    g_kq[g_khead].scancode = sc;
    g_khead = n;
}

void gmv_input_escape(int down)
{
    kq_push(down ? 0x300u : 0x301u, 0x1b, 41);
}

#define GMV_SDLK_SCAN(s)  ((int)((unsigned)(s) | (1u << 30)))

static int gmv_key_to_sdl(SDL_Scancode code, int *out_scancode)
{
    int sym = 0, sc = 0;
    SDL_Keycode k = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, true);
    if (!(SDL_GetModState() & SDL_KMOD_NUM)) {
        code = code == SDL_SCANCODE_KP_4 ? SDL_SCANCODE_LEFT : code == SDL_SCANCODE_KP_6 ? SDL_SCANCODE_RIGHT
             : code == SDL_SCANCODE_KP_8 ? SDL_SCANCODE_UP : code == SDL_SCANCODE_KP_2 ? SDL_SCANCODE_DOWN
             : code == SDL_SCANCODE_KP_PERIOD ? SDL_SCANCODE_DELETE : code;
    }
    if (k >= 'a' && k <= 'z') {
        sym = (int)k;
        sc  = SDL_SCANCODE_A + (int)(k - 'a');
    } else if (k >= '1' && k <= '9') {
        sym = (int)k;
        sc  = SDL_SCANCODE_1 + (int)(k - '1');
    } else if (k == '0') {
        sym = '0'; sc = SDL_SCANCODE_0;
    } else if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) {
        sc  = (int)code;
        sym = GMV_SDLK_SCAN(sc);
    } else switch (code) {
        case SDL_SCANCODE_SPACE:     sym = ' ';  sc = 44; break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:  sym = '\r'; sc = 40; break;
        case SDL_SCANCODE_ESCAPE:    sym = 0x1b; sc = 41; break;
        case SDL_SCANCODE_BACKSPACE: sym = '\b'; sc = 42; break;
        case SDL_SCANCODE_TAB:       sym = '\t'; sc = 43; break;
        case SDL_SCANCODE_DELETE:    sym = 0x7f; sc = 76; break;
        case SDL_SCANCODE_LEFT:      sym = GMV_SDLK_SCAN(80);  sc = 80;  break;
        case SDL_SCANCODE_RIGHT:     sym = GMV_SDLK_SCAN(79);  sc = 79;  break;
        case SDL_SCANCODE_UP:        sym = GMV_SDLK_SCAN(82);  sc = 82;  break;
        case SDL_SCANCODE_DOWN:      sym = GMV_SDLK_SCAN(81);  sc = 81;  break;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:    sym = GMV_SDLK_SCAN(225); sc = 225; break;
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:     sym = GMV_SDLK_SCAN(224); sc = 224; break;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:      sym = GMV_SDLK_SCAN(226); sc = 226; break;
        default:
            if (k == ',')      { sym = ',';  sc = 54; }
            else if (k == '.') { sym = '.';  sc = 55; }
            else if (k == '-') { sym = '-';  sc = 45; }
            else if (k == '=' || k == '+') { sym = '=';  sc = 46; }
            break;
    }
    if (out_scancode) *out_scancode = sc;
    return sym;
}

/* TISGetInputSourceProperty 0x19f0b0 0x18ac4e */
static int kbd_is_azerty(void)
{
    static int seen = -1;
    int azerty = SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false) == 'a' &&
                 SDL_GetKeyFromScancode(SDL_SCANCODE_W, SDL_KMOD_NONE, false) == 'z';
    if (azerty != seen) {
        seen = azerty;
        gmv_log("[input] keyboard layout: %s\n", azerty ? "AZERTY, the game's French keys" : "the game's usual keys");
    }
    return azerty;
}

void *gmv_tis_input_source_property(void *source, void *key)
{
    static void *french;
    (void)source;
    (void)key;
    if (!kbd_is_azerty())
        return NULL;
    if (!french)
        french = gmv_make_nsstring("com.apple.keylayout.French");
    return french;
}

/* SDL_Event keysym.scancode@0xc keysym.sym@0x10 */
int gmv_input_get_sdl_event(void *buf, unsigned minType, unsigned maxType)
{
    if (!buf)
        return 0;
    if (!(minType <= 0x301u && maxType >= 0x300u))
        return 0;
    if (g_ktail == g_khead)
        return 0;
    struct kinput *e = &g_kq[g_ktail];
    g_ktail = (g_ktail + 1) % MAXKIN;
    unsigned char *p = (unsigned char *)buf;
    memset(p, 0, 28);
    *(unsigned *)(p + 0x00) = e->type;                        /* 0x300 0x301 */
    p[0x08] = (unsigned char)(e->type == 0x300u ? 1 : 0);
    *(int *)(p + 0x0c) = e->scancode;
    *(int *)(p + 0x10) = e->sym;
    {
        static int ktr = -1;
        if (ktr < 0) { const char *en = gmv_dbgenv("GMV_INPUT_TRACE"); ktr = (gmv_env_on(en)) ? 1 : 0; }
        if (GMV_DEV && ktr) gmv_log("[input] deliver SDL key %s sym=0x%x sc=%d\n",
                         e->type == 0x300u ? "DOWN" : "UP", (unsigned)e->sym, e->scancode);
    }
    return 1;
}

static unsigned g_kbm_tick;
static unsigned g_presses;

unsigned gmv_input_last_key(void) { return g_kbm_tick; }
unsigned gmv_input_tick(void) { return (unsigned)SDL_GetTicks(); }
unsigned gmv_input_presses(void) { return g_presses; }
int gmv_input_scancode_held(int scancode)
{
    int n = 0;
    const bool *keys = SDL_GetKeyboardState(&n);
    return scancode >= 0 && scancode < n && keys[scancode];
}

static int show_os_cursor(void)
{
    static int v = -1;
    if (v < 0) { const char *e = gmv_getenv("GMV_SHOW_CURSOR"); v = (gmv_env_on(e)) ? 1 : 0; }
    return v;
}

static void cursor_sync(void)
{
    static int off;
    int want = gmv_input_is_cursor_hidden() && !show_os_cursor();
    if (want != off && gmv_gl_window()) {
        off = want;
        if (want)
            SDL_HideCursor();
        else
            SDL_ShowCursor();
    }
}

static void look_mode_sync(void)
{
    static int relative;
    SDL_Window *w = gmv_gl_window();
    int want = !gmv_game_pointer_shown();
    if (!w || want == relative)
        return;
    relative = want;
    if (!SDL_SetWindowRelativeMouseMode(w, want))
        gmv_log("[input] relative mouse failed (%s)\n", SDL_GetError());
    gmv_trace("[input] the mouse is the %s\n", want ? "camera" : "pointer");
}

static int g_move_pending, g_move_x, g_move_y;

static void move_flush(void)
{
    if (g_move_pending) {
        g_move_pending = 0;
        q_push_at(3, g_move_x, g_move_y);
    }
}

static void input_key_down(const SDL_KeyboardEvent *e)
{
    if (e->repeat || e->scancode >= SDL_SCANCODE_COUNT)
        return;
    int quiet = e->scancode == SDL_SCANCODE_TAB || e->scancode == SDL_SCANCODE_CAPSLOCK ||
                (e->scancode >= SDL_SCANCODE_LCTRL && e->scancode <= SDL_SCANCODE_RGUI);
    if (!quiet)
        g_presses++;
    if ((e->scancode == SDL_SCANCODE_RETURN || e->scancode == SDL_SCANCODE_KP_ENTER) && (e->mod & SDL_KMOD_ALT)) {
        g_alt_enter = 1;
        g_cs_keys[e->scancode] = 1;
        return;
    }
    g_kbm_tick = gmv_input_tick() | 1;
    int sc = 0, sym = gmv_key_to_sdl(e->scancode, &sc);
    if (sym && gmv_game_key((unsigned)sym)) {
        g_cs_keys[e->scancode] = 1;
        sym = 0;
    }
    if (sym)
        kq_push(0x300u, sym, sc);
}

static void input_key_up(const SDL_KeyboardEvent *e)
{
    if (e->scancode >= SDL_SCANCODE_COUNT)
        return;
    int sc = 0, sym = gmv_key_to_sdl(e->scancode, &sc);
    if (g_cs_keys[e->scancode]) {
        g_cs_keys[e->scancode] = 0;
        sym = 0;
    }
    if (sym)
        kq_push(0x301u, sym, sc);
}

static void input_button(const SDL_MouseButtonEvent *e)
{
    int x = (int)e->x, y = (int)e->y;
    move_flush();
    if (e->down) {
        g_presses++;
        if (e->button == SDL_BUTTON_LEFT || e->button == SDL_BUTTON_RIGHT)
            g_kbm_tick = gmv_input_tick() | 1;
    }
    if (e->button == SDL_BUTTON_LEFT) {
        g_button_down = e->down;
        q_push_at(e->down ? 1 : 2, x, y);
    } else if (e->button == SDL_BUTTON_RIGHT) {
        /* -rightMouseDown IMP 0x199800 */
        q_push_at(e->down ? 4 : 5, x, y);
    }
}

static void input_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        input_key_down(&e->key);
        break;
    case SDL_EVENT_KEY_UP:
        input_key_up(&e->key);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        input_button(&e->button);
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        static int last_mx = -1, last_my = -1;
        int mx = (int)e->motion.x, my = (int)e->motion.y;
        look_add_raw((int)e->motion.xrel, (int)e->motion.yrel);
        if (g_pad_moved && mx == last_mx && my == last_my)
            break;
        g_pad_moved = 0;
        last_mx = mx;
        last_my = my;
        g_move_pending = 1;
        g_move_x = mx;
        g_move_y = my;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        float gx, gy;
        move_flush();
        to_game((int)e->wheel.mouse_x, (int)e->wheel.mouse_y, &gx, &gy);
        q_push_scroll(gx, gy, e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_focus = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        g_key_pending = g_focus ? 1 : -1;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        gmv_input_set_client_size(e->window.data1, e->window.data2);
        look_clip();
        break;
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        /* 0x19af50 */
        gmv_log("[input] window closed (Alt+F4 or the close button): quitting\n");
        log_close();
        _Exit(0);
    default:
        break;
    }
}

static volatile int g_pumping;

static bool SDLCALL redraw_watch(void *userdata, SDL_Event *e)
{
    (void)userdata;
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && g_pumping)
        gmv_gl_redraw();
    return true;
}

void gmv_input_drain(int *skip)
{
    static int watching;
    SDL_Event e;
    if (!watching)
        watching = SDL_AddEventWatch(redraw_watch, NULL);
    g_pumping = 1;
    SDL_PumpEvents();
    g_pumping = 0;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (skip) {
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_ESCAPE ||
                e.key.scancode == SDL_SCANCODE_SPACE || e.key.scancode == SDL_SCANCODE_RETURN ||
                e.key.scancode == SDL_SCANCODE_KP_ENTER)) {
                *skip = 1;
                continue;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                *skip = 1;
                continue;
            }
        }
        input_event(&e);
    }
    move_flush();
}

__attribute__((noinline, force_align_arg_pointer))
/* mov byte [pic+0x4ef670],1 0x19b03c */
#define GMV_KEYWINDOW_FLAG ((volatile unsigned char *)(uintptr_t)GMV_KEYWINDOW_FLAG_ADDR)

static void input_dispatch(void)
{
    msg_t msg = (msg_t)(void *)gmv_objc_msgSend;

    if (!g_key_inited && gmv_objc_last_gamewindow) {
        g_key_inited = 1;
        g_key_pending = 1;
    }
    if (g_key_pending) {
        if (GMV_KEYWINDOW_FLAG_ADDR)
            *GMV_KEYWINDOW_FLAG = (g_key_pending > 0) ? 1 : 0;
        g_key_pending = 0;
    }

    void *view = gmv_objc_last_eaglview;
    if (!view)
        return;
#if GMV_DEV
    static int itrace = -1;
    if (itrace < 0) { const char *e = gmv_dbgenv("GMV_INPUT_TRACE"); itrace = (gmv_env_on(e)) ? 1 : 0; }
#else
    enum { itrace = 0 };
#endif
    while (g_tail != g_head) {
        struct minput *e = &g_q[g_tail];
        g_tail = (g_tail + 1) % MAXIN;
        const char *sel;
        void *ev;
        if (e->kind == 8) {
            sel = "scrollWheel:";
            ev  = gmv_make_nsscroll(e->x, e->y, e->d);
        } else {
            ev  = gmv_make_nsevent(e->x, e->y);
            sel = e->kind == 1 ? "mouseDown:" :
                  e->kind == 2 ? "mouseUp:" :
                  e->kind == 4 ? "rightMouseDown:" :
                  e->kind == 5 ? "rightMouseUp:" :
                  e->kind == 6 ? "rightMouseDragged:" :
                  (g_button_down ? "mouseDragged:" : "mouseMoved:");
        }
        if (!gmv_objc_responds(view, sel)) {
            static int warned_kind[9];
            if (itrace && e->kind < 9 && !warned_kind[e->kind]) {
                warned_kind[e->kind] = 1;
                gmv_log("[input] view does not implement -%s; dropping (no-op)\n", sel);
            }
            continue;
        }
        if (itrace && e->kind != 3 && e->kind != 6)
            gmv_log("[input] %-18s cocoa=(%.0f,%.0f) d=%.2f view=%p keyWin=%u\n",
                    sel, e->x, e->y, e->d, view, GMV_KEYWINDOW_FLAG_ADDR ? (unsigned)*GMV_KEYWINDOW_FLAG : 0u);
        msg(view, sel, ev);
    }
}

#define PM_SPEED 900.0f
#define PM_BTN_A 1
#define PM_BTN_B 2

static void pad_pointer(void)
{
    static uint64_t last;
    static unsigned prev;
    static int active, a_down;
    uint64_t now = SDL_GetPerformanceCounter();
    float x, y, dt, m;
    unsigned b;
    int hat, on = gmv_game_pad_pointer() && g_focus;

    dt = last ? (float)(now - last) / (float)SDL_GetPerformanceFrequency() : 0.0f;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;
    if (on != active) {
        gmv_trace("[padmenu] pointer %s\n", on ? "on (the game shows its pointer)" : "off");
        active = on;
        if (!on && a_down) {
            a_down = 0;
            g_button_down = 0;
            q_push(2, g_ptr_x, g_ptr_y);
        }
    }
    if (!on || !gmv_joy_peek(&x, &y, &b, &hat)) {
        prev = 0;
        return;
    }
    if (hat & 1) y = -1.0f;
    if (hat & 4) y = 1.0f;
    if (hat & 2) x = 1.0f;
    if (hat & 8) x = -1.0f;
    m = x * x + y * y;
    if (m > 0.0f) {
        float k = (m > 1.0f ? 1.0f / sqrtf(m) : sqrtf(m)) * PM_SPEED * dt;
        float nx = g_ptr_x + x * k, ny = g_ptr_y - y * k;
        nx = nx < 0.0f ? 0.0f : nx > GMV_VIEW_FRAME_W - 1.0f ? GMV_VIEW_FRAME_W - 1.0f : nx;
        ny = ny < 1.0f ? 1.0f : ny > GMV_VIEW_FRAME_H ? GMV_VIEW_FRAME_H : ny;
        if (nx != g_ptr_x || ny != g_ptr_y) {
            q_push(3, nx, ny);
            g_pad_moved = 1;
        }
    }
    if ((b & ~prev) & (1u << PM_BTN_A)) {
        a_down = 1;
        g_button_down = 1;
        q_push(1, g_ptr_x, g_ptr_y);
    } else if ((prev & ~b) & (1u << PM_BTN_A) && a_down) {
        a_down = 0;
        g_button_down = 0;
        q_push(2, g_ptr_x, g_ptr_y);
    }
    if ((b & ~prev) & (1u << PM_BTN_B))
        kq_push(0x300u, 0x1b, 41);
    if ((prev & ~b) & (1u << PM_BTN_B))
        kq_push(0x301u, 0x1b, 41);
    prev = b;
}

__attribute__((force_align_arg_pointer))
void gmv_input_pump(void)
{
    /* MainMenu [0xa9dc34+0x18] 0xa9dc4c */
#if GMV_DEV
    static int watch_armed, watch_mode = -1;
    if (watch_mode < 0) {
        const char *e = gmv_dbgenv("GMV_WATCH");
        watch_mode = (gmv_env_on(e)) ? 1 : 0;
    }
    if (watch_mode == 1 && !watch_armed && gmv_input_wants_tap) {
        const char *a = gmv_dbgenv("GMV_WATCH_ADDR");
        if (a && *a) gmv_watch_arm((uint32_t)strtoul(a, NULL, 16));
        watch_armed = 1;
    }
#endif
    gmv_input_drain(NULL);
    cursor_sync();
    look_mode_sync();
    if (g_alt_enter) {
        g_alt_enter = 0;
        gmv_game_alt_enter();
    }
    gmv_game_full_screen_tick();

#if GMV_DEV
    static long click_at = -2;
    static unsigned frame;
    if (click_at == -2) {
        const char *e = gmv_dbgenv("GMV_TEST_CLICK");
        click_at = e ? atol(e) : -1;
    }
    if (click_at >= 0) {
        float cx = GMV_VIEW_FRAME_W / 2.0f, cy = GMV_VIEW_FRAME_H / 2.0f;
        if ((long)frame == click_at) {
            gmv_log("[input] TEST click down at centre (%.0f,%.0f)\n", cx, cy);
            g_button_down = 1;
            q_push(1, cx, cy);
        } else if ((long)frame == click_at + 6) {
            gmv_log("[input] TEST click up\n");
            g_button_down = 0;
            q_push(2, cx, cy);
        }
    }
    {
        static int fatal_kind = -1;
        if (fatal_kind < 0) {
            const char *e = gmv_dbgenv("GMV_TEST_FATAL");
            fatal_kind = e ? atoi(e) : 0;
        }
        if (fatal_kind && frame == 600) {
            void gmv_missing_import(uint32_t index, uint32_t ret);
            gmv_log("[input] TEST_FATAL=%d\n", fatal_kind);
            static volatile uintptr_t bad = 4;
            if (fatal_kind == 1)
                *(volatile int *)bad = 1;
            else if (fatal_kind == 2)
                gmv_missing_import(0, 0);
            else if (fatal_kind == 3)
                __asm__("ud2");
            else if (fatal_kind == 4)
                SDL_CreateThread((SDL_ThreadFunction)(uintptr_t)4, "fatal", NULL);
        }
    }
    frame++;
    {
        static int lg = -2;
        if (lg == -2) { const char *e = gmv_dbgenv("GMV_STATE_TRACE"); lg = (gmv_env_on(e)) ? 1 : 0; }
        if (lg && frame % 120 == 0)
            gmv_log("[input] PUMP call #%u\n", frame);
    }

    /* [state3+0xc] */
    static int s3_mode = -2, s3_phase;
    if (s3_mode == -2) {
        const char *e = gmv_dbgenv("GMV_CLICK_S3");
        s3_mode = (gmv_env_on(e)) ? 0 : -1;
    }
    if (s3_mode == 0) {
        float cx = GMV_VIEW_FRAME_W / 2.0f, cy = GMV_VIEW_FRAME_H / 2.0f;
        if (s3_phase == 0 && gmv_input_wants_tap) {
            gmv_log("[input] S3 auto-click DOWN at centre (%.0f,%.0f)\n", cx, cy);
            g_button_down = 1;
            q_push(1, cx, cy);
            s3_phase = 1;
        } else if (s3_phase >= 1 && s3_phase < 8) {
            s3_phase++;
        } else if (s3_phase == 8) {
            gmv_log("[input] S3 auto-click UP\n");
            g_button_down = 0;
            q_push(2, cx, cy);
            s3_phase = 9;
        }
    }
#endif

    gmv_game_input_pump();
    pad_pointer();
    input_dispatch();
    gmv_game_input_tick();
}
