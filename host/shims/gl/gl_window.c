#include <stdlib.h>
#include "compat.h"
#include <GL/gl.h>
#include <SDL3/SDL.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

#ifndef GL_BGR_EXT
#endif

#if GMV_DEV
int g_gl_trace;
#else
#endif
int g_no_flicker;
volatile int g_drew_since_swap;

SDL_Window *g_window;
void *g_glctx;

#if GMV_DEV
#endif

static double g_pace_ms = -1.0;
volatile LONG g_pace_gen;
int g_swap_default = 1;
static int g_swap_want = -2;

double pace_frame_ms(void)
{
    if (g_pace_ms < 0.0) {
        const char *fm = gmv_getenv("GMV_FRAME_MS");
        g_pace_ms = (fm && *fm) ? atof(fm) : 1000.0 / 60.0;
        if (g_pace_ms < 0.0)
            g_pace_ms = 0.0;
    }
    return g_pace_ms;
}

double gmv_pace_frame_ms(void)
{
    return pace_frame_ms();
}

void gmv_set_frame_ms(double ms)
{
    g_pace_ms = ms > 0.0 ? ms : 0.0;
    g_pace_gen++;
}

int swap_interval(int v)
{
    return SDL_GL_SetSwapInterval(v);
}

int swap_want(void)
{
    if (g_swap_want == -2) {
        const char *si = gmv_getenv("GMV_SWAP_INTERVAL");
        g_swap_want = (si && *si) ? atoi(si) : -1;
    }
    return g_swap_want;
}

int gmv_set_swap_interval(int v)
{
    g_swap_want = v < 0 ? -1 : v;
    g_pace_gen++;
    return 1;
}

SDL_Window *gmv_gl_window(void)
{
    return g_window;
}

void *gmv_gl_window_hwnd(void)
{
    return g_window ? SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL)
                    : NULL;
}
