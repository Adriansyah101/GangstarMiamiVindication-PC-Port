/* gl_present.c -- the present: what happens between frames, and the tick pacer */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat.h"
#include <GL/gl.h>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <mmsystem.h>                  /* timeBeginPeriod (winmm) */
#endif

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "glshim.h"

#if GMV_DEV
/* PERF_PROFILE, the GPU's side (loadprof.c has the CPU's): a timestamp query at points through each
   drawn frame -- its first clear or draw, the post pass (ao_run: the sea's and the cars' reflections,
   the AO, the bloom), the present (the MSAA resolve and the blit to the window) and its end.  The
   results are read PM_RING frames later, when the GPU is long done, so nothing waits for them.  A
   span is the GPU's time between two points, idle time included: when the CPU is the bottleneck the
   scene's span is the CPU's pace rather than the GPU's work.  The post pass and the present are
   issued back to back, so theirs are the GPU's work. */
pm_t pm = {.on = -1};

static int pm_init(void)
{
    const char *e = gmv_dbgenv("GMV_PERF_PROFILE");
    if (!e || !*e || *e == '0')
        return 0;
    pm.GenQueries = (void *)gl_real("glGenQueries");
    pm.QueryCounter = (void *)gl_real("glQueryCounter");
    pm.GetQueryObjectiv = (void *)gl_real("glGetQueryObjectiv");
    pm.GetQueryObjectui64v = (void *)gl_real("glGetQueryObjectui64v");
    if (!pm.GenQueries || !pm.QueryCounter || !pm.GetQueryObjectiv || !pm.GetQueryObjectui64v) {
        gmv_log("[perf] the driver has no timer queries; the GPU's side left out\n");
        return 0;
    }
    pm.GenQueries(PM_RING * PM_N, &pm.q[0][0]);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    pm.freq = (double)f.QuadPart / 1000.0;
    pm.t_log = gmv_prof_now();
    gmv_log("[perf] GPU timer queries ready (%s)\n", (const char *)glGetString(GL_RENDERER));
    return 1;
}

void pm_mark(int m)
{
    if (pm.on <= 0 || !pm.started)
        return;
    pm.QueryCounter(pm.q[pm.slot][m], 0x8E28);          /* GL_TIMESTAMP */
    pm.set[pm.slot] |= 1u << m;
    pm.cpu[m] = gmv_prof_now();
}

/* The results of the frame that used this slot PM_RING frames ago. */
static void pm_collect(int s)
{
    unsigned set = pm.set[s];
    pm.set[s] = 0;
    if (!(set & 1u << PM_START) || !(set & 1u << PM_END))
        return;
    GLint ok = 0;
    pm.GetQueryObjectiv(pm.q[s][PM_END], 0x8867, &ok);  /* GL_QUERY_RESULT_AVAILABLE */
    if (!ok) {
        pm.late++;
        return;
    }
    unsigned long long t[PM_N] = {0};
    for (int m = 0; m < PM_N; m++)
        if (set & 1u << m)
            pm.GetQueryObjectui64v(pm.q[s][m], 0x8866, &t[m]);  /* GL_QUERY_RESULT, ns */
    unsigned long long prev = t[PM_START];
    for (int m = 1; m < PM_N; m++)
        if (set & 1u << m) {
            pm.seg[m] += (double)(t[m] - prev) / 1e6;
            prev = t[m];
        }
    double tot = (double)(t[PM_END] - t[PM_START]) / 1e6;
    pm.total += tot;
    if (tot > pm.max_total) pm.max_total = tot;
    if (set & 1u << PM_POST) pm.posts++;
    pm.draws_sum += pm.sdraws[s];
    if (pm.sdraws[s] > pm.draws_max) pm.draws_max = pm.sdraws[s];
    pm.frames++;
}

/* The frame's first clear or draw. */
void pm_begin(void)
{
    if (pm.on < 0)
        pm.on = pm_init();
    if (pm.on <= 0)
        return;
    pm.slot = (pm.slot + 1) % PM_RING;
    pm_collect(pm.slot);
    pm.started = 1;
    pm.draws = 0;
    pm_mark(PM_START);
}

/* After the present's blit, before SwapBuffers. */
static void pm_end(void)
{
    if (pm.on <= 0 || !pm.started)
        return;
    pm_mark(PM_END);
    pm.sdraws[pm.slot] = pm.draws;
    pm.started = 0;
    unsigned set = pm.set[pm.slot];
    if (set & 1u << PM_POST)
        pm.cpu_post += (double)(pm.cpu[PM_BLOOM] - pm.cpu[PM_POST]) / pm.freq;
    pm.cpu_pres += (double)(pm.cpu[PM_END] - pm.cpu[PM_PRES]) / pm.freq;
    pm.cpu_frames++;
    long long now = gmv_prof_now();
    if ((double)(now - pm.t_log) / pm.freq < 5000.0 || !pm.frames)
        return;
    double n = pm.frames, np = pm.posts ? pm.posts : 1;
    gmv_log("[perf] GPU, %u frames (%u with the post pass): scene %.2f ms, then sea reflections %.2f, "
            "car reflections %.2f, AO %.2f, bloom %.2f (a frame with the pass), 2D/HUD %.2f, present %.2f; "
            "first draw to present %.2f ms (max %.1f); %u draws a frame (max %u); CPU in the post pass %.2f ms, "
            "in the present %.2f ms%s\n",
            pm.frames, pm.posts, pm.seg[PM_POST] / np, pm.seg[PM_SSR] / np, pm.seg[PM_VR] / np, pm.seg[PM_AO] / np,
            pm.seg[PM_BLOOM] / np, pm.seg[PM_PRES] / n, pm.seg[PM_END] / n, pm.total / n, pm.max_total,
            pm.draws_sum / pm.frames, pm.draws_max, pm.posts ? pm.cpu_post / pm.posts : 0.0,
            pm.cpu_pres / pm.cpu_frames, pm.late ? " (some results late)" : "");
    memset(pm.seg, 0, sizeof pm.seg);
    pm.total = pm.max_total = pm.cpu_post = pm.cpu_pres = 0;
    pm.frames = pm.late = pm.cpu_frames = pm.posts = pm.draws_sum = pm.draws_max = 0;
    pm.t_log = now;
}
#else
#define pm_end() ((void)0)
#endif

/* ---- PRECISE_WAIT (a port speed-up, [Timing], on by default and under ORIGINAL_MAC_EXPERIENCE) --------------------
   The pacer used to Sleep in whole milliseconds on the 1 ms timer and, since a Sleep may wake a tick late, spin the
   last 1.5-2.5 ms of every tick in Sleep(0): at 150 ticks a second that is a third of a core kept busy for nothing,
   power a laptop's built-in GPU shares.  Now it waits on a high-resolution waitable timer (Windows 10 1803 and
   later; it wakes within a fraction of a millisecond, whatever timeBeginPeriod says) until shortly before the
   deadline (by how late it has been waking, pw_learn) and spins only the rest; on Linux nanosleep is that timer.  The ticks come at the same
   deadlines.  Without such a timer (older Windows), the old waits.  PRECISE_WAIT=0: the old waits. */
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static HANDLE pw_timer;
#endif

static int precise_wait_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = gmv_getenv("GMV_PRECISE_WAIT");
        on = !(e && *e == '0');
#ifdef _WIN32
        if (on && !(pw_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                      TIMER_ALL_ACCESS))) {
            on = 0;
            gmv_log("[framepace] PRECISE_WAIT: Windows has no high-resolution timer here; the 1 ms sleeps\n");
        } else
#endif
            gmv_log("[framepace] PRECISE_WAIT=%d: %s\n", on,
                    on ? "the pacer waits on a high-resolution timer" : "the pacer sleeps in whole ms and spins");
    }
    return on;
}


/* How late the timer wakes, learnt as it goes: the 90th percentile of the last few hundred waits (a running quantile:
   up 0.9 of a step when a wait woke later than it, down 0.1 when sooner), 0 to 1.5 ms.  The pacer wakes that much
   plus 50 us ahead of the deadline and spins the rest.  Where the timer is as exact as its name (it was on the GT
   Racing port's machine) that is the 50 us of the plan; on this laptop's Windows 11 it woke 0.25-0.5 ms late at 150
   ticks a second, and with a fixed 50 us 70 % of the ticks left 0.1-0.5 ms late; learnt, ~1 % (measured with the dev
   GMV_PW_MARGIN fixed margins of 0.05, 0.3 and 0.6 ms: no difference in CPU beyond the traffic's). */
static double pw_late = 0.3;

static void pw_learn(double over)
{
    pw_late += over > pw_late ? 0.009 : -0.001;
    pw_late = pw_late < 0.0 ? 0.0 : pw_late > 1.5 ? 1.5 : pw_late;
}

#if GMV_DEV
/* Dev: how late the timer woke (over the sleep asked for) and how late the tick left the pacer, for [framepace]. */
static unsigned pw_n, pw_o[4], pw_late_n[3], pw_ticks;
static double pw_omax, pw_lmax;
static void pw_over_note(double ms)
{
    pw_n++;
    pw_o[ms < 0.1 ? 0 : ms < 0.25 ? 1 : ms < 0.5 ? 2 : 3]++;
    if (ms > pw_omax) pw_omax = ms;
}
static void pw_late_note(double ms)
{
    pw_ticks++;
    if (ms >= 0.1) pw_late_n[ms < 0.5 ? 0 : ms < 1.0 ? 1 : 2]++;
    if (ms > pw_lmax) pw_lmax = ms;
}
static void pw_report(void)
{
    if (pw_ticks)
        gmv_log("[framepace]   waits %u: woke late <0.1 ms %u, 0.1-0.25 %u, 0.25-0.5 %u, >0.5 %u (max %.2f); ticks %u "
                "left late 0.1-0.5 ms %u, 0.5-1 %u, >1 %u (max %.2f); learnt lateness %.2f ms\n", pw_n, pw_o[0], pw_o[1], pw_o[2], pw_o[3],
                pw_omax, pw_ticks, pw_late_n[0], pw_late_n[1], pw_late_n[2], pw_lmax, pw_late);
    pw_n = pw_ticks = 0;
    memset(pw_o, 0, sizeof pw_o);
    memset(pw_late_n, 0, sizeof pw_late_n);
    pw_omax = pw_lmax = 0.0;
}
#endif

/* Sleep ms on the precise timer. */
static void precise_sleep(double ms)
{
#ifdef _WIN32
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(ms * 1e4);              /* relative, in 100 ns */
    if (!due.QuadPart)
        due.QuadPart = -1;
    if (SetWaitableTimerEx(pw_timer, &due, 0, NULL, NULL, NULL, 0))
        WaitForSingleObject(pw_timer, INFINITE);
#else
    struct timespec t = {(time_t)(ms / 1000.0), (long)(fmod(ms, 1000.0) * 1e6)};
    nanosleep(&t, NULL);
#endif
}

/* ---- BACKGROUND_THROTTLE (a port addition, [Timing], on by default, off under ORIGINAL_MAC_EXPERIENCE) ------------
   Why: the Mac game was a full-screen app that the system slowed as any other in the background; here the window went
   on drawing at the full FRAME_MS rate behind other windows and while minimized, a CPU core and the GPU busy on a
   picture nobody sees, and a game in progress went on (traffic, the police) with the player away.  A port gap.
   Each tick the window's state is read from SDL:
   - behind other windows: at most BACKGROUND_FPS drawn frames a second (30: the Mac's 60 ticks), the game still
     running, and still shown (a second monitor);
   - minimized: at most MINIMIZED_FPS (10), no swap (glFlush instead: nothing to show), the 1 ms timer period given
     back to Windows (timeEndPeriod; the pacer's waits are on the precise timer anyway), and the game told
     (gmv_game_minimized: Gangstar presses Esc in play, its own pause).  Restored: all of it undone; a paused game
     waits for the player.
   The rate change goes to the game too (gmv_game_tick_period), as a FRAME_MS change from the options does, so a
   fixed physics step follows it.  BACKGROUND_THROTTLE=0: full speed always.  Read again when ORIGINAL_MAC_EXPERIENCE
   is switched in game, under which it answers 0. */
static int bg_state;                                    /* 0 in front, 1 behind other windows, 2 minimized */
static int bg_on = -1, bg_mac = -1, bg_fps[3] = {0, 30, 10};

static int bg_fps_key(const char *key, int dflt)
{
    const char *e = gmv_getenv(key);
    int v = e && *e ? atoi(e) : dflt;
    return v < 1 ? 1 : v > 60 ? 60 : v;
}

/* The state now; changes are logged and passed on.  frames: the tick, for the log. */
static int bg_poll(unsigned frames)
{
    if (bg_mac != gmv_config_mac()) {
        bg_mac = gmv_config_mac();
        const char *e = gmv_getenv("GMV_BACKGROUND_THROTTLE");
        int was = bg_on;
        bg_on = !(e && *e == '0');
        bg_fps[1] = bg_fps_key("GMV_BACKGROUND_FPS", 30);
        bg_fps[2] = bg_fps_key("GMV_MINIMIZED_FPS", 10);
        if (bg_on != was && bg_on)
            gmv_log("[bg] BACKGROUND_THROTTLE=1: behind other windows at most %d drawn frames a second, "
                    "minimized %d and the game paused\n", bg_fps[1], bg_fps[2]);
        else if (bg_on != was)
            gmv_log("[bg] BACKGROUND_THROTTLE=0: full speed behind other windows and minimized\n");
    }
    SDL_WindowFlags f = g_window ? SDL_GetWindowFlags(g_window) : 0;
    int st = !bg_on ? 0 : (f & SDL_WINDOW_MINIMIZED) ? 2 : !(f & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0;
    if (st != bg_state) {
        static const char *const where[3] = {"in front: full speed", "behind other windows", "minimized"};
        if (st)
            gmv_log("[bg] %s: at most %d drawn frames a second (tick %u)\n", where[st], bg_fps[st], frames);
        else
            gmv_log("[bg] %s (tick %u)\n", where[0], frames);
#ifdef _WIN32
        if (st == 2)
            timeEndPeriod(1);
        else if (bg_state == 2)
            timeBeginPeriod(1);
#endif
        if (st == 2 || bg_state == 2)
            gmv_game_minimized(st == 2);
        bg_state = st;
    }
    return st;
}

/* ---- SPIKE_MS (a port diagnostic, [Log], off by default) ---------------------------------------------------------
   A tick longer than SPIKE_MS ms is logged with its number, how long the tick's own work took (the game's update and,
   on a drawn tick, its frame; the rest is the pacer's wait) and how long of it the swap took (the driver waiting for
   the GPU or the screen).  Cheap enough for a player's build: two clock reads a tick when on.  At most 10 lines a
   second; the ones left out are counted.  Not while BACKGROUND_THROTTLE slows the window. */
static double spike_ms = -1.0, spike_swap_ms;

static void spike_check(unsigned frames, double dt_ms, double work_ms, int drew)
{
    static double win_t;
    static unsigned win_n, win_skipped;
    if (spike_ms < 0.0) {
        const char *e = gmv_getenv("GMV_SPIKE_MS");
        spike_ms = e && *e ? atof(e) : 0.0;
        if (spike_ms > 0.0)
            gmv_log("[spike] SPIKE_MS=%g: ticks longer than that are logged\n", spike_ms);
    }
    if (spike_ms <= 0.0)
        return;
    win_t += dt_ms;
    if (win_t >= 1000.0) {
        if (win_skipped)
            gmv_log("[spike] and %u more over %g ms in that second\n", win_skipped, spike_ms);
        win_t = 0.0;
        win_n = win_skipped = 0;
    }
    if (dt_ms <= spike_ms)
        return;
    if (win_n++ >= 10) {
        win_skipped++;
        return;
    }
    gmv_log("[spike] tick %u: %.1f ms (its work %.1f ms, of it the swap %.1f ms; %s)\n", frames, dt_ms, work_ms,
            spike_swap_ms, drew ? "drawn" : "not drawn");
}

/* Present the window (used by SDL_GL_SwapBuffers).  Set GMV_DUMP_FRAME=<n> to capture frame n of
   the back buffer to gmv_frame.bmp (written in the game's cwd) for inspection. */
void gmv_gl_swap(void)
{
    ensure_gl();
#if GMV_DEV
    /* GMV_LOOP_TRACE=1: one-time dump of the call site(s) that reach the per-frame present, to
       locate the game's MAIN LOOP (and from there its update(dt) call).
       The direct caller may be a binding thunk, so also scan the stack for the first few return
       addresses that land in the game's __TEXT range (image base 0x1000 .. ~0x680000). */
    {
        static int lt = -1;
        if (lt < 0) { const char *e = gmv_dbgenv("GMV_LOOP_TRACE"); lt = (gmv_env_on(e)) ? 1 : 0; }
        static unsigned seen; static int done;
        if (lt && !done && ++seen >= 300) {             /* fire in the steady loop, not startup */
            done = 1;
            void *ret0 = __builtin_return_address(0);
            gmv_log("[loop] present direct caller = %p\n", ret0);
            void **sp = (void **)&ret0;                 /* scan stack upward for game-text returns */
            int found = 0;
            for (int i = 0; i < 256 && found < 8; i++) {
                uintptr_t v = (uintptr_t)sp[i];
                if (v >= GMV_TEXT_LO && v < GMV_TEXT_HI) {
                    gmv_log("[loop]   stack[%d] -> game addr %p\n", i, (void *)v);
                    found++;
                } else if (v >= GMV_LOWCOPY_BASE && v < GMV_LOWCOPY_BASE + GMV_LOWCOPY_CAP) {
                    /* the low range (incl. the per-frame tick 0x5f30) runs from its copy */
                    gmv_log("[loop]   stack[%d] -> game addr %p (low copy %p)\n", i,
                            (void *)(v - GMV_LOW_DELTA), (void *)v);
                    found++;
                }
            }
        }
    }
    static long dump_at = -2;
    if (dump_at == -2) {
        const char *e = gmv_dbgenv("GMV_DUMP_FRAME");
        dump_at = e ? atol(e) : -1;
    }
#endif
    static unsigned frames;
    if (g_window) {
        gmv_game_between_frames();
        gmv_game_frame_end();
#if GMV_DEV
        /* GMV_GL_FINISH=1: force a full GPU sync every frame.  Diagnostic for the "3D turns black
           but UI stays" symptom: it vanishes under GMV_GL_TRACE (whose per-frame glReadPixels also
           forces a sync), which points at a buffer-update race (the game overwrites a VBO/texture
           while the GPU is still reading it -- implicit on the old macOS GL, not on Windows).  If
           this toggle alone stops the black, the race is confirmed and the real fix is a sync. */
        static int glfin = -1;
        if (glfin < 0) { const char *e = gmv_dbgenv("GMV_GL_FINISH"); glfin = (gmv_env_on(e)) ? 1 : 0; }
        if (glfin) glFinish();
        if (g_gl_trace && (frames < 6 || frames % 60 == 0)) {
            unsigned char c[4] = {0};                   /* sample the just-rendered BACK centre */
            diag_read_begin(GL_BACK);
            glReadPixels(512, 384, 1, 1, 0x1908, 0x1401, c);    /* RGBA, UNSIGNED_BYTE */
            diag_read_end();
            gmv_log("[gltrace] f%u BACK pre-swap centre=%d,%d,%d,%d\n", frames, c[0], c[1], c[2], c[3]);
        }
        if ((long)frames == dump_at)
            dump_framebuffer("gmv_frame.bmp");
#endif
        /* Anti-flicker (GMV_NO_FLICKER): the engine renders at ~30fps (clears+draws every other
           frame) but SDL_GL_SwapBuffers is called every frame (~60fps); with double buffering the
           drawn content lands in one buffer while the other is shown on the alternate frames,
           giving a black/stale flicker.  Skip the present on frames where nothing was drawn, so the
           last fully-rendered frame stays on screen.  Default on; GMV_NO_FLICKER=0 disables. */
        int drew = g_drew_since_swap, did_swap = 1;
        int hold = gmv_game_present_hold(frames, drew);
#if GMV_DEV
        gmv_game_present_decided(frames, !(g_no_flicker && !drew) && !hold);
#endif
        gmv_game_tick();
#if GMV_DEV
        if (g_cap_ticks >= 0) {
            DWORD t = GetTickCount();
            int presented = !(g_no_flicker && !drew);
            cap_seq("");                                /* flush the last draw run */
            if (presented && (g_cap_presented++ % 4 == 0 || t - g_cap_last_fade < 300))
                fade_capture_dump(frames, t);
            gmv_log("[fadecap] f%u tick+%d t=%lu +%lums clears=%u draws=%u present=%s seq=%s\n", frames,
                    g_cap_ticks, (unsigned long)t, (unsigned long)(t - g_cap_t0), g_cap_clears, g_cap_draws,
                    presented ? "yes" : "skipped", g_cap_seq);
            g_cap_seqlen = 0; g_cap_seq[0] = 0;
            g_cap_ticks++;
            if (t - g_cap_t0 > CAP_MS) {
                g_cap_ticks = -1;
                gmv_log("[fadecap] session %u done (%u images)\n", g_cap_session - 1, g_cap_saved);
            }
        }
        g_cap_clears = g_cap_draws = 0;
        if (g_post_fade) {
            gmv_log("[postfade] ---- swap f%u\n", frames);
            g_post_fade = 0;
        }
#endif
        if ((g_no_flicker && !g_drew_since_swap) || hold) {
            did_swap = 0;   /* nothing new (or a held menu-entry frame) -- keep the current front */
        } else {
            pm_mark(PM_PRES);
#if GMV_DEV
            if (g_corner_watch == 2) {
                fade_corners("at the present");
                g_corner_watch = 0;
            }
#endif
            msaa_present();
            pm_end();
            long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
            LARGE_INTEGER s0, s1, sf;
            s0.QuadPart = 0;
            if (spike_ms > 0.0)
                QueryPerformanceCounter(&s0);
            if (bg_state == 2)
                glFlush();                              /* BACKGROUND_THROTTLE, minimized: nothing to show */
            else
                gl_swap_window();
            if (spike_ms > 0.0) {
                QueryPerformanceCounter(&s1);
                QueryPerformanceFrequency(&sf);
                spike_swap_ms = (double)(s1.QuadPart - s0.QuadPart) * 1000.0 / (double)sf.QuadPart;
            }
            if (t0)
                gmv_prof_add(GMV_PROF_SWAP, t0, 0);
            msaa_sync_size();
            gmv_game_after_present();
        }
        g_drew_since_swap = 0;
        gmv_prof_tick_end(frames, did_swap);

        /* Tick pacer (pace_frame_ms, default 60 Hz) + dt stats.  Every tick presents through here
           (drawn or not), so pacing this call paces the game's whole tick: update + every-other
           render, exactly what a 60 Hz vblank wait did on the Mac.  The pacer keeps a fixed deadline
           cadence (next += period) so it averages the exact rate -- the game's whole-ms dt then
           alternates 16/17 -- and after a hitch longer than a period (a load) it resyncs instead of
           sprinting to catch up.  The waits are PRECISE_WAIT's (above); BACKGROUND_THROTTLE lengthens
           the period behind other windows and while minimized; SPIKE_MS looks at each tick's length.
           [framepace] dt stats every ~2 s are logged when GMV_FRAME_MS or GMV_SWAP_INTERVAL is set
           explicitly or with GMV_PACE_LOG=1 (dev). */
        {
            static int cfg = -1, eff_state;
            static LONG gen;
            static double frame_ms, eff_ms, told_ms, period, next, t_exit;
            static LARGE_INTEGER freq, last;
#if GMV_DEV
            static int pace_log;
            static double wmin, wmax, wsum, wt; static unsigned wn, wdrew;
#endif
            LARGE_INTEGER now;
            if (cfg >= 0 && gen != g_pace_gen) {         /* a new FRAME_MS or VSYNC from the options menu */
                gen = g_pace_gen;
                frame_ms = told_ms = pace_frame_ms();   /* (the menu's setter gave the game this rate) */
                eff_state = -1;                         /* the period again, below */
                int v = swap_want() >= 0 ? swap_want() : frame_ms > 0.0 ? 0 : g_swap_default;
                swap_interval(v);
                gmv_log("[framepace] tick pacer now %.3f ms%s, swap interval %d\n", frame_ms,
                        frame_ms > 0.0 ? "" : " (off)", v);
            }
            if (cfg < 0) {
                cfg = 0;
                gen = g_pace_gen;
                frame_ms = told_ms = pace_frame_ms();
                eff_state = -1;
#if GMV_DEV
                const char *fm = gmv_getenv("GMV_FRAME_MS");
                const char *si = gmv_getenv("GMV_SWAP_INTERVAL");
                const char *pl = gmv_dbgenv("GMV_PACE_LOG");
                pace_log = gmv_env_flag(pl, (fm && *fm) || (si && *si));
                wmin = 1e9; wmax = 0; wsum = 0; wn = 0; wt = 0; wdrew = 0;
#endif
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&last);
                t_exit = (double)last.QuadPart;
                precise_wait_on();
            }
            /* BACKGROUND_THROTTLE: the period this tick, the game told when it changes */
            int st = bg_poll(frames), changed = st != eff_state;
            if (changed) {
                eff_ms = st && (frame_ms <= 0.0 || frame_ms < 500.0 / bg_fps[st]) ? 500.0 / bg_fps[st] : frame_ms;
                period = eff_ms * (double)freq.QuadPart / 1000.0;
                QueryPerformanceCounter(&now);
                next = (double)now.QuadPart;
                eff_state = st;
                if (eff_ms != told_ms) {
                    told_ms = eff_ms;
                    gmv_game_tick_period(eff_ms);
                }
            }
            QueryPerformanceCounter(&now);
            double work_ms = ((double)now.QuadPart - t_exit) * 1000.0 / (double)freq.QuadPart;
            if (eff_ms > 0.0) {
                next += period;
                if ((double)now.QuadPart > next + period) {
                    next = (double)now.QuadPart;            /* hitch: resync, don't burst ticks */
                } else {
                    int pw = precise_wait_on();
#if GMV_DEV
                    static double pw_fixed = -2.0;
                    if (pw_fixed < -1.0) {
                        const char *m = gmv_dbgenv("GMV_PW_MARGIN");
                        pw_fixed = m && *m ? atof(m) : -1.0;
                    }
#endif
                    for (;;) {
                        double rem = (next - (double)now.QuadPart) * 1000.0 / (double)freq.QuadPart;
                        if (rem <= 0.0) break;
                        if (!pw)
                            Sleep(rem > 2.5 ? (DWORD)(rem - 1.5) : 0);
                        else if (rem > 0.1) {
                            double m = pw_late + 0.05;  /* wake this far ahead (see pw_learn) */
#if GMV_DEV
                            if (pw_fixed >= 0.0) m = pw_fixed;
#endif
                            if (rem > m + 0.05) {
                                precise_sleep(rem - m);     /* PRECISE_WAIT: up to m before the deadline */
                                LARGE_INTEGER w; QueryPerformanceCounter(&w);
                                double over = ((double)w.QuadPart - (double)now.QuadPart) * 1000.0 /
                                              (double)freq.QuadPart - (rem - m);
                                pw_learn(over);
#if GMV_DEV
                                pw_over_note(over);
#endif
                            } else
                                SwitchToThread();
                        } else
                            SwitchToThread();               /* and spin the rest */
                        QueryPerformanceCounter(&now);
                    }
#if GMV_DEV
                    pw_late_note(((double)now.QuadPart - next) * 1000.0 / (double)freq.QuadPart);
#endif
                }
            }
            if (!st && !changed)                            /* SPIKE_MS */
                spike_check(frames, ((double)now.QuadPart - t_exit) * 1000.0 / (double)freq.QuadPart, work_ms, drew);
            t_exit = (double)now.QuadPart;
            spike_swap_ms = 0.0;
            gmv_prof_tick_start();
#if GMV_DEV
            if (pace_log) {
                QueryPerformanceCounter(&now);
                double dtms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
                last = now;
                if (dtms < wmin) wmin = dtms;
                if (dtms > wmax) wmax = dtms;
                wsum += dtms; wn++; wt += dtms; wdrew += drew ? 1u : 0u;
                if (wt >= 2000.0 && wn > 0) {
                    gmv_log("[framepace] %u frames/%.0fms: dt avg=%.1f min=%.1f max=%.1f ms "
                            "(~%.0f present fps) jitter=%.1f  drew=%u (~%.0f render fps)\n",
                            wn, wt, wsum / wn, wmin, wmax, 1000.0 * wn / wt, wmax - wmin,
                            wdrew, 1000.0 * wdrew / wt);
                    pw_report();
                    wmin = 1e9; wmax = 0; wsum = 0; wn = 0; wt = 0; wdrew = 0;
                }
            }
#endif
        }
#if GMV_DEV
        /* Flicker check (GMV_CHECK_FRONT=1): log drew/swap + count bright pixels in the FRONT-buffer
           text band each frame over a window.  Flicker -> nonblack alternates high/0. */
        {
            static int cf = -1;
            if (cf < 0) { const char *e = gmv_dbgenv("GMV_CHECK_FRONT"); cf = (gmv_env_on(e)) ? 1 : 0; }
            if (cf && frames >= 640 && frames <= 664) {
                static unsigned char buf[530 * 60 * 4];
                diag_read_begin(GL_FRONT);
                glReadPixels(258, 423, 530, 60, 0x1908, 0x1401, buf);  /* text band (GL y = 768-345) */
                diag_read_end();
                long nb = 0;
                for (int i = 0; i < 530 * 60; i++)
                    if (buf[i*4] > 16 || buf[i*4+1] > 16 || buf[i*4+2] > 16) nb++;
                gmv_log("[flickchk] f%u drew=%d swapped=%d FRONT-nonblack=%ld\n", frames, drew, did_swap, nb);
            }
        }
        if (frames % 60 == 0)
            gmv_log("[gl] SDL_GL_SwapBuffers: presented frame %u\n", frames);
        {   /* One-shot: identify the loop that presents without pumping (state3/menu modal wait).
               Scan this frame's stack for game-range return addresses to get the caller chain. */
            static int st2 = -2;
            if (st2 == -2) { const char *e = gmv_dbgenv("GMV_STATE_TRACE"); st2 = (gmv_env_on(e)) ? 0 : -1; }
            if (st2 == 0 && frames == 900) {
                volatile int anchor = 0;
                const uint32_t *sp = (const uint32_t *)&anchor;
                gmv_log("[gl] f900 present-loop stack scan (who calls 0x594fc0):\n");
                for (int k = 0; k < 512; k++) {
                    uint32_t v = sp[k];
                    if (v >= GMV_TEXT_LO && v < GMV_TEXT_HI)
                        gmv_log("        [+%03x] -> game 0x%08x\n", k * 4, v);
                }
                st2 = 1;
            }
        }
#else
        (void)did_swap;
#endif
        gl_trace_frame_end(frames);
        frames++;
    }
}
