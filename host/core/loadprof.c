#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "log.h"
#if GMV_DEV
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "compat.h"
#else
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include "compat.h"
#endif

#include "layout.h"
#include "shims.h"

#define GAME_TEXT_LO GMV_TEXT_LO
#define GAME_TEXT_HI GMV_TEXT_HI

static volatile int g_on = -1;
static int g_perf;
static double g_thresh_ms = 40.0;
static double g_qpc_per_ms;
static DWORD g_main_tid;
static uintptr_t g_stack_lo, g_stack_hi;
static uint32_t g_host_lo, g_host_hi;
static volatile int g_in_tick;

static struct { long n; long long bytes, qpc; } g_main[GMV_PROF_N];
static volatile LONG g_bg_n;
static volatile LONG64 g_bg_bytes, g_bg_qpc;

static long long g_tick_t0;

int gmv_prof_on(void)
{
    if (g_on < 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_per_ms = (double)f.QuadPart / 1000.0;
        const char *e = gmv_dbgenv("GMV_LOAD_PROFILE");
        int v = e ? atoi(e) : 0;
        if (v > 1)
            g_thresh_ms = v;
        const char *p = gmv_dbgenv("GMV_PERF_PROFILE");
        g_perf = gmv_env_on(p);
        if (v <= 0 && g_perf)
            g_thresh_ms = 1e12;
        g_on = v > 0 || g_perf;
    }
    return g_on;
}

int gmv_perf_on(void)
{
    return gmv_prof_on() && g_perf;
}

long long gmv_prof_now(void)
{
    return gmv_qpc();
}

static DWORD self_tid(void)
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    static __thread DWORD tid;
    if (!tid)
        tid = GetCurrentThreadId();
    return tid;
#endif
}

void gmv_prof_add(int kind, long long t0, long long bytes)
{
    long long d = gmv_prof_now() - t0;
    if (!g_main_tid)
        return;
    if (self_tid() == g_main_tid) {
        g_main[kind].n++;
        g_main[kind].bytes += bytes;
        g_main[kind].qpc += d;
    } else if (kind == GMV_PROF_IO) {
        InterlockedIncrement(&g_bg_n);
        InterlockedExchangeAdd64(&g_bg_bytes, bytes);
        InterlockedExchangeAdd64(&g_bg_qpc, d);
    }
}

#define RING 16384
#define CHAIN 12
static struct { long long t; uint32_t eip, crt, ret, host; int tick; uint32_t chain[CHAIN]; } g_ring[RING];
static volatile LONG g_ring_pos;
static uint32_t g_crt_lo, g_crt_hi;

static int in_game(uint32_t a)
{
    if (a >= GMV_LOWCOPY_BASE && a < GMV_LOWCOPY_BASE + GMV_LOWCOPY_CAP)
        a -= GMV_LOW_DELTA;
    return a >= GAME_TEXT_LO && a < GAME_TEXT_HI;
}

static uint32_t game_addr(uint32_t a)
{
    return (a >= GMV_LOWCOPY_BASE && a < GMV_LOWCOPY_BASE + GMV_LOWCOPY_CAP) ? a - GMV_LOW_DELTA : a;
}

static int after_call(uint32_t a)
{
    if (a >= GAME_TEXT_LO && a < GMV_RESERVE_START + 8)
        return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
    if (((a - 7) ^ a) >> 12) {
#ifdef _WIN32
        uint8_t first;
        if (!ReadProcessMemory(GetCurrentProcess(), p - 7, &first, 1, NULL))
            return 0;
#else
        if (!gmv_readable(p - 7, 1))
            return 0;
#endif
    }
    if (p[-5] == 0xE8)
        return *(const int32_t *)(p - 4) != 0;
    for (int len = 2; len <= 7; len++)
        if (p[-len] == 0xFF && ((p[-len + 1] >> 3) & 7) == 2)
            return 1;
    return 0;
}

static void stack_frames(uint32_t esp, uint32_t *crt, uint32_t *ret, uint32_t *host, uint32_t *chain)
{
    int nc = 0;
    *crt = *ret = *host = 0;
    if (chain)
        memset(chain, 0, CHAIN * sizeof *chain);
    if (esp < g_stack_lo || esp >= g_stack_hi)
        return;
    const uint32_t *p = (const uint32_t *)(uintptr_t)esp;
    for (int i = 0; i < 8192 && (uintptr_t)(p + i) < g_stack_hi; i++) {
        uint32_t v = p[i];
        if (*ret) {
            if (in_game(v) && after_call(v)) {
                chain[nc++] = game_addr(v);
                if (nc == CHAIN)
                    return;
            }
        } else if (!*crt && v >= g_crt_lo && v < g_crt_hi && after_call(v)) {
            *crt = v;
        } else if (!*host && v >= g_host_lo && v < g_host_hi && after_call(v)) {
            *host = v;
        } else if (in_game(v) && after_call(v)) {
            *ret = game_addr(v);
            if (!chain)
                return;
            chain[nc++] = *ret;
        }
    }
}

static void ring_put(uint32_t eip, uint32_t crt, uint32_t ret, uint32_t host, int tick, const uint32_t *chain)
{
    LONG i = g_ring_pos % RING;
    if (g_perf)
        memcpy(g_ring[i].chain, chain, sizeof g_ring[i].chain);
    g_ring[i].t = gmv_prof_now();
    g_ring[i].eip = eip;
    g_ring[i].crt = crt;
    g_ring[i].ret = ret;
    g_ring[i].host = host;
    g_ring[i].tick = tick;
    InterlockedIncrement(&g_ring_pos);
}

#ifdef _WIN32
static DWORD WINAPI sampler(void *arg)
{
    HANDLE th = arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        Sleep(1);
        if (SuspendThread(th) == (DWORD)-1)
            continue;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        uint32_t eip = 0, crt = 0, ret = 0, host = 0, chain[CHAIN];
        int tick = g_in_tick;
        if (GetThreadContext(th, &c)) {
            eip = c.Eip;
            stack_frames(c.Esp, &crt, &ret, &host, g_perf ? chain : NULL);
        }
        ResumeThread(th);
        if (eip)
            ring_put(eip, crt, ret, host, tick, chain);
    }
    return 0;
}
#else
#define SAMPLE_SIG (SIGRTMAX - 3)
static pid_t g_pid;

static void on_sample(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    int saved = errno;
    const ucontext_t *uc = ucv;
    uint32_t eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP], crt, ret, host, chain[CHAIN];
    stack_frames((uint32_t)uc->uc_mcontext.gregs[REG_ESP], &crt, &ret, &host, g_perf ? chain : NULL);
    if (eip)
        ring_put(eip, crt, ret, host, g_in_tick, chain);
    errno = saved;
}

static void *sampler(void *arg)
{
    pid_t tid = (pid_t)(uintptr_t)arg;
    struct timespec ms = { 0, 1000000 };
    for (;;) {
        nanosleep(&ms, NULL);
        syscall(SYS_tgkill, g_pid, tid, SAMPLE_SIG);
    }
    return NULL;
}

struct obj { uint32_t a; const char *want; uint32_t bias, lo, hi; int exe; char name[24]; };

static int obj_cb(struct dl_phdr_info *in, size_t size, void *arg)
{
    (void)size;
    struct obj *o = arg;
    uint32_t lo = ~0u, hi = 0;
    for (int i = 0; i < in->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &in->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD)
            continue;
        uint32_t s = (uint32_t)(in->dlpi_addr + ph->p_vaddr), e = s + (uint32_t)ph->p_memsz;
        if (s < lo)
            lo = s;
        if (e > hi)
            hi = e;
    }
    const char *name = in->dlpi_name ? in->dlpi_name : "", *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    if (lo >= hi || (o->want ? strncmp(b, o->want, strlen(o->want)) : o->a < lo || o->a >= hi))
        return 0;
    o->bias = (uint32_t)in->dlpi_addr;
    o->lo = lo;
    o->hi = hi;
    o->exe = !*name;
    snprintf(o->name, sizeof o->name, "%.23s", b);
    return 1;
}

static int obj_find(struct obj *o)
{
    return dl_iterate_phdr(obj_cb, o);
}
#endif

#ifndef _WIN32
static void code_name(uint32_t a, char *out, size_t n)
{
    struct obj o = { .a = a };
    Dl_info di;
    if (a >= GMV_THUNK_BASE && a < GMV_THUNK_BASE + GMV_THUNK_SIZE)
        snprintf(out, n, "thunk");
    else if (!obj_find(&o))
        snprintf(out, n, "?%08x", a);
    else if (o.exe)
        snprintf(out, n, "host 0x%x", a);
    else if (dladdr((const void *)(uintptr_t)a, &di) && di.dli_sname && di.dli_saddr &&
             (uint32_t)(uintptr_t)di.dli_saddr <= a)
        snprintf(out, n, "%.24s!%.40s+0x%x", o.name, di.dli_sname, a - (uint32_t)(uintptr_t)di.dli_saddr);
    else
        snprintf(out, n, "%.24s+0x%x", o.name, a - o.bias);
}
#else
static void code_name(uint32_t a, char *out, size_t n)
{
    HMODULE h = NULL;
    char path[MAX_PATH];
    if (a >= GMV_THUNK_BASE && a < GMV_THUNK_BASE + GMV_THUNK_SIZE) {
        snprintf(out, n, "thunk");
        return;
    }
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (const char *)(uintptr_t)a, &h) ||
        !GetModuleFileNameA(h, path, sizeof path)) {
        snprintf(out, n, "?%08x", a);
        return;
    }
    if (h == GetModuleHandleA(NULL)) {
        snprintf(out, n, "host 0x%x", a);
        return;
    }
    const char *b = path;
    for (const char *q = path; *q; q++)
        if (*q == '\\' || *q == '/')
            b = q + 1;
    const uint8_t *base = (const uint8_t *)h;
    const IMAGE_NT_HEADERS32 *nt = (const void *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    uint32_t rva = a - (uint32_t)(uintptr_t)base, best = 0;
    const char *bn = NULL;
    if (dd->VirtualAddress) {
        const IMAGE_EXPORT_DIRECTORY *ex = (const void *)(base + dd->VirtualAddress);
        const DWORD *names = (const void *)(base + ex->AddressOfNames);
        const WORD *ords = (const void *)(base + ex->AddressOfNameOrdinals);
        const DWORD *funcs = (const void *)(base + ex->AddressOfFunctions);
        for (DWORD i = 0; i < ex->NumberOfNames; i++) {
            DWORD f = funcs[ords[i]];
            if (f <= rva && f > best) {
                best = f;
                bn = (const char *)(base + names[i]);
            }
        }
    }
    if (bn)
        snprintf(out, n, "%.24s!%.40s+0x%x", b, bn, rva - best);
    else
        snprintf(out, n, "%.24s+0x%x", b, rva);
}
#endif

static void report_samples(long long t0, long long t1)
{
    enum { MAXK = 64 };
    struct { uint32_t eip, crt, ret; int n; } k[MAXK];
    int nk = 0, total = 0;
    LONG end = g_ring_pos;
    for (LONG j = end - 1; j >= 0 && j > end - RING; j--) {
        LONG i = j % RING;
        if (g_ring[i].t > t1)
            continue;
        if (g_ring[i].t < t0)
            break;
        uint32_t eip = g_ring[i].eip, crt = g_ring[i].crt, ret = g_ring[i].ret;
        if (in_game(eip)) {
            eip = game_addr(eip) & ~0xffu;
        } else {
            char nm[96];
            code_name(eip, nm, sizeof nm);
            char *plus = strrchr(nm, '+');
            if (plus && strchr(nm, '!'))
                eip -= (uint32_t)strtoul(plus + 1, NULL, 16);
        }
        total++;
        int x;
        for (x = 0; x < nk; x++)
            if (k[x].eip == eip && k[x].crt == crt && k[x].ret == ret)
                break;
        if (x == nk) {
            if (nk == MAXK)
                continue;
            k[nk].eip = eip;
            k[nk].crt = crt;
            k[nk].ret = ret;
            k[nk].n = 0;
            nk++;
        }
        k[x].n++;
    }
    if (!total)
        return;
    gmv_logfile("[loadprof]   %d samples:\n", total);
    for (int shown = 0; shown < 8; shown++) {
        int best = -1;
        for (int x = 0; x < nk; x++)
            if (k[x].n > 0 && (best < 0 || k[x].n > k[best].n))
                best = x;
        if (best < 0 || k[best].n * 100 < total * 2)
            break;
        char a[96], c[96];
        if (in_game(k[best].eip))
            snprintf(a, sizeof a, "game 0x%06x", k[best].eip);
        else
            code_name(k[best].eip, a, sizeof a);
        if (k[best].crt)
            code_name(k[best].crt, c, sizeof c);
        else
            c[0] = 0;
        gmv_logfile("[loadprof]     %3d%%  %s%s%s <- game 0x%06x\n", k[best].n * 100 / total, a,
                    c[0] ? " <- " : "", c, k[best].ret);
        k[best].n = 0;
    }
}

#define SLEEP_KINDS 4
static struct { unsigned usec; uint32_t ret[3]; int n; } g_sleeps[SLEEP_KINDS];

void gmv_prof_sleep_note(unsigned usec)
{
    if (!g_main_tid || self_tid() != g_main_tid || usec < 2000)
        return;
    uint32_t ret[3] = { 0, 0, 0 };
    const uint32_t *p = (const uint32_t *)__builtin_frame_address(0);
    for (int i = 0, k = 0; i < 2048 && k < 3 && (uintptr_t)(p + i) < g_stack_hi; i++)
        if (in_game(p[i]) && after_call(p[i]))
            ret[k++] = game_addr(p[i]);
    for (int x = 0; x < SLEEP_KINDS; x++) {
        if (g_sleeps[x].n && (g_sleeps[x].usec != usec || memcmp(g_sleeps[x].ret, ret, sizeof ret)))
            continue;
        g_sleeps[x].usec = usec;
        memcpy(g_sleeps[x].ret, ret, sizeof ret);
        g_sleeps[x].n++;
        return;
    }
}

double ms_of(long long q) { return (double)q / g_qpc_per_ms; }

static struct { int ticks; double ms, tex, io, lock, rest; long long t0; } g_sum;

#ifndef _WIN32
static void start_main(void)
{
    pthread_attr_t at;
    void *lo;
    size_t sz;
    if (!pthread_getattr_np(pthread_self(), &at)) {
        if (!pthread_attr_getstack(&at, &lo, &sz)) {
            g_stack_lo = (uintptr_t)lo;
            g_stack_hi = (uintptr_t)lo + sz;
        }
        pthread_attr_destroy(&at);
    }
    struct obj crt = { .want = "libc.so" }, exe = { .a = (uint32_t)(uintptr_t)start_main };
    if (obj_find(&crt)) {
        g_crt_lo = crt.lo;
        g_crt_hi = crt.hi;
    }
    if (obj_find(&exe)) {
        g_host_lo = exe.lo;
        g_host_hi = exe.hi;
    }
    g_main_tid = self_tid();
    g_pid = getpid();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    pthread_attr_t ta;
    pthread_t t;
    if (!sigaction(SAMPLE_SIG, &sa, NULL) && !pthread_attr_init(&ta)) {
        pthread_attr_setstacksize(&ta, 64 * 1024);
        pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &ta, sampler, (void *)(uintptr_t)g_main_tid))
            gmv_log("[loadprof] no sampler thread: no samples\n");
        pthread_attr_destroy(&ta);
    }
    if (g_thresh_ms < 1e11)
        gmv_log("[loadprof] on: ticks over %.0f ms are broken down below (GMV_LOAD_PROFILE)\n",
                g_thresh_ms);
    if (g_perf)
        gmv_log("[perf] on: where each tick's time goes, summed every 5 s (GMV_PERF_PROFILE)\n");
}
#else
static void start_main(void)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    g_stack_lo = (uintptr_t)tib->StackLimit;
    g_stack_hi = (uintptr_t)tib->StackBase;
    const uint8_t *crt = (const uint8_t *)GetModuleHandleA("msvcrt.dll");
    if (crt) {
        const IMAGE_NT_HEADERS32 *nt = (const void *)(crt + ((const IMAGE_DOS_HEADER *)crt)->e_lfanew);
        g_crt_lo = (uint32_t)(uintptr_t)crt;
        g_crt_hi = g_crt_lo + nt->OptionalHeader.SizeOfImage;
    }
    const uint8_t *exe = (const uint8_t *)GetModuleHandleA(NULL);
    {
        const IMAGE_NT_HEADERS32 *nt = (const void *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
        g_host_lo = GMV_RESERVE_END;
        g_host_hi = (uint32_t)(uintptr_t)exe + nt->OptionalHeader.SizeOfImage;
    }
    HANDLE me = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &me,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (me)
        CloseHandle(CreateThread(NULL, 64 * 1024, sampler, me, 0, NULL));
    g_main_tid = GetCurrentThreadId();
    if (g_thresh_ms < 1e11)
        gmv_log("[loadprof] on: ticks over %.0f ms are broken down below (GMV_LOAD_PROFILE)\n",
                g_thresh_ms);
    if (g_perf)
        gmv_log("[perf] on: where each tick's time goes, summed every 5 s (GMV_PERF_PROFILE)\n");
}
#endif

static struct {
    long long t0;
    unsigned ticks, drawn;
    double work_drawn, work_upd, max_drawn, max_upd;
    double swap, upl, io, sleep, lockwait;
} g_pw;

static struct { uint32_t lo, hi; char name[24]; } g_mods[32];
static int g_nmods;

static const char *mod_of(uint32_t a)
{
    for (int i = 0; i < g_nmods; i++)
        if (a >= g_mods[i].lo && a < g_mods[i].hi)
            return g_mods[i].name;
#ifndef _WIN32
    struct obj o = { .a = a };
    if (!obj_find(&o))
        return "?";
    if (g_nmods == 32)
        return "other";
    g_mods[g_nmods].lo = o.lo;
    g_mods[g_nmods].hi = o.hi;
    snprintf(g_mods[g_nmods].name, sizeof g_mods[g_nmods].name, "%s", o.exe ? "host" : o.name);
    return g_mods[g_nmods++].name;
#else
    HMODULE h = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (const char *)(uintptr_t)a, &h) ||
        !GetModuleFileNameA(h, path, sizeof path))
        return "?";
    if (g_nmods == 32)
        return "other";
    const uint8_t *base = (const uint8_t *)h;
    const IMAGE_NT_HEADERS32 *nt = (const void *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const char *b = path;
    for (const char *q = path; *q; q++)
        if (*q == '\\' || *q == '/')
            b = q + 1;
    g_mods[g_nmods].lo = (uint32_t)(uintptr_t)base;
    g_mods[g_nmods].hi = g_mods[g_nmods].lo + nt->OptionalHeader.SizeOfImage;
    snprintf(g_mods[g_nmods].name, sizeof g_mods[g_nmods].name, "%.23s", b);
    return g_mods[g_nmods++].name;
#endif
}

static void perf_inclusive(long long t0, long long t1, int total)
{
    enum { HS = 4096 };
    static struct { uint32_t a; int n; } h[HS];
    memset(h, 0, sizeof h);
    LONG end = g_ring_pos;
    for (LONG j = end - 1; j >= 0 && j > end - RING; j--) {
        LONG i = j % RING;
        if (g_ring[i].t > t1)
            continue;
        if (g_ring[i].t < t0)
            break;
        if (!g_ring[i].tick)
            continue;
        for (int c = 0; c < CHAIN && g_ring[i].chain[c]; c++) {
            uint32_t a = g_ring[i].chain[c];
            int dup = 0;
            for (int d = 0; d < c; d++)
                dup |= g_ring[i].chain[d] == a;
            if (dup)
                continue;
            unsigned s = (a * 2654435761u) >> 20;
            for (int probe = 0; probe < HS; probe++, s = (s + 1) & (HS - 1)) {
                if (h[s].a == a || !h[s].a) {
                    h[s].a = a;
                    h[s].n++;
                    break;
                }
            }
        }
    }
    for (int shown = 0; shown < 40; shown++) {
        int best = -1;
        for (int x = 0; x < HS; x++)
            if (h[x].n > 0 && (best < 0 || h[x].n > h[best].n))
                best = x;
        if (best < 0 || h[best].n * 100 < total * 2)
            break;
        gmv_logfile("[perf]   incl %4.1f%%  game 0x%06x\n", h[best].n * 100.0 / total, h[best].a);
        h[best].n = 0;
    }
}

static void perf_samples(long long t0, long long t1)
{
    enum { MAXM = 16, MAXK = 256 };
    struct { const char *name; int n; } m[MAXM];
    struct { uint32_t eip, host, ret; int n; } k[MAXK];
    int nm = 0, nk = 0, total = 0, waiting = 0;
    LONG end = g_ring_pos;
    for (LONG j = end - 1; j >= 0 && j > end - RING; j--) {
        LONG i = j % RING;
        if (g_ring[i].t > t1)
            continue;
        if (g_ring[i].t < t0)
            break;
        if (!g_ring[i].tick) {
            waiting++;
            continue;
        }
        uint32_t eip = g_ring[i].eip;
        const char *mod;
        if (in_game(eip)) {
            mod = "game";
            eip = game_addr(eip);
        } else if (eip >= g_host_lo && eip < g_host_hi) {
            mod = "host";
        } else if (eip >= GMV_THUNK_BASE && eip < GMV_THUNK_BASE + GMV_THUNK_SIZE) {
            mod = "thunk";
        } else {
            mod = mod_of(eip);
        }
        total++;
        int x;
        for (x = 0; x < nm && strcmp(m[x].name, mod); x++)
            ;
        if (x == nm && nm < MAXM) {
            m[nm].name = mod;
            m[nm].n = 0;
            nm++;
        }
        if (x < nm)
            m[x].n++;
        eip &= ~0xffu;
        uint32_t host = g_ring[i].host, ret = g_ring[i].ret;
        if (!strcmp(mod, "game"))
            host = 0;
        for (x = 0; x < nk; x++)
            if (k[x].eip == eip && k[x].host == host && k[x].ret == ret)
                break;
        if (x == nk) {
            if (nk == MAXK)
                continue;
            k[nk].eip = eip;
            k[nk].host = host;
            k[nk].ret = ret;
            k[nk].n = 0;
            nk++;
        }
        k[x].n++;
    }
    if (!total)
        return;
    char line[512];
    int len = snprintf(line, sizeof line, "[perf]   %d samples in ticks (%d in the pacing wait):", total, waiting);
    for (int shown = 0; shown < MAXM; shown++) {
        int best = -1;
        for (int x = 0; x < nm; x++)
            if (m[x].n > 0 && (best < 0 || m[x].n > m[best].n))
                best = x;
        if (best < 0 || len > (int)sizeof line - 40)
            break;
        len += snprintf(line + len, sizeof line - len, " %s %d%%", m[best].name, m[best].n * 100 / total);
        m[best].n = 0;
    }
    gmv_logfile("%s\n", line);
    for (int shown = 0; shown < 16; shown++) {
        int best = -1;
        for (int x = 0; x < nk; x++)
            if (k[x].n > 0 && (best < 0 || k[x].n > k[best].n))
                best = x;
        if (best < 0 || k[best].n * 100 < total)
            break;
        char a[96], h[96];
        if (k[best].eip < GMV_THUNK_BASE)
            snprintf(a, sizeof a, "game 0x%06x", k[best].eip);
        else
            code_name(k[best].eip, a, sizeof a);
        if (k[best].host)
            snprintf(h, sizeof h, " <- host 0x%x", k[best].host);
        else
            h[0] = 0;
        gmv_logfile("[perf]     %4.1f%%  %s%s <- game 0x%06x\n", k[best].n * 100.0 / total, a, h, k[best].ret);
        k[best].n = 0;
    }
    perf_inclusive(t0, t1, total);
}

static void perf_tick(long long t, double ms, int drew)
{
    g_pw.ticks++;
    if (drew) {
        g_pw.drawn++;
        g_pw.work_drawn += ms;
        if (ms > g_pw.max_drawn) g_pw.max_drawn = ms;
    } else {
        g_pw.work_upd += ms;
        if (ms > g_pw.max_upd) g_pw.max_upd = ms;
    }
    g_pw.swap += ms_of(g_main[GMV_PROF_SWAP].qpc);
    g_pw.upl += ms_of(g_main[GMV_PROF_TEX].qpc) + ms_of(g_main[GMV_PROF_BUF].qpc);
    g_pw.io += ms_of(g_main[GMV_PROF_IO].qpc);
    g_pw.sleep += ms_of(g_main[GMV_PROF_SLEEP].qpc);
    g_pw.lockwait += ms_of(g_main[GMV_PROF_LOCK].qpc) + ms_of(g_main[GMV_PROF_WAIT].qpc);
    double span = ms_of(t - g_pw.t0);
    if (span < 5000.0)
        return;
    unsigned upd = g_pw.ticks - g_pw.drawn;
    gmv_log("[perf] %.1f s: %u ticks (%.0f a second), %u drew a frame (%.0f fps); work a tick: "
            "drawn %.2f ms (max %.1f), update only %.2f ms (max %.1f); a second: swap %.0f ms, "
            "uploads %.0f, file %.0f, the game's sleeps %.0f, locks/waits %.0f\n",
            span / 1000, g_pw.ticks, g_pw.ticks * 1000.0 / span, g_pw.drawn, g_pw.drawn * 1000.0 / span,
            g_pw.drawn ? g_pw.work_drawn / g_pw.drawn : 0.0, g_pw.max_drawn,
            upd ? g_pw.work_upd / upd : 0.0, g_pw.max_upd, g_pw.swap * 1000 / span, g_pw.upl * 1000 / span,
            g_pw.io * 1000 / span, g_pw.sleep * 1000 / span, g_pw.lockwait * 1000 / span);
    perf_samples(g_pw.t0, t);
    gmv_game_perf_report(span / 1000);
    memset(&g_pw, 0, sizeof g_pw);
    g_pw.t0 = t;
}

void gmv_prof_tick_end(unsigned frame, int drew)
{
    if (!gmv_prof_on())
        return;
    g_in_tick = 0;
    long long t = gmv_prof_now();
    if (!g_main_tid) {
        start_main();
        g_sum.t0 = t;
        g_pw.t0 = t;
        return;
    }
    double ms = ms_of(t - g_tick_t0);
    unsigned bp_made, bp_gone;
    int bp_bounds;
    gmv_game_perf_counts(&bp_made, &bp_gone, &bp_bounds);
    if (g_perf && g_tick_t0)
        perf_tick(t, ms, drew);
    if (g_tick_t0 && ms >= g_thresh_ms) {
        double tex = ms_of(g_main[GMV_PROF_TEX].qpc), buf = ms_of(g_main[GMV_PROF_BUF].qpc);
        double io = ms_of(g_main[GMV_PROF_IO].qpc), lock = ms_of(g_main[GMV_PROF_LOCK].qpc);
        double wait = ms_of(g_main[GMV_PROF_WAIT].qpc), slp = ms_of(g_main[GMV_PROF_SLEEP].qpc);
        double swp = ms_of(g_main[GMV_PROF_SWAP].qpc);
        double rest = ms - tex - buf - io - lock - wait - slp - swp;
        gmv_logfile("[loadprof] f%u tick %.1f ms: tex %ld/%lld KB %.1f ms, buf %ld/%lld KB %.1f ms, "
                    "io %ld/%lld KB %.1f ms, lock %.1f ms, wait %.1f ms, sleep %ld/%.1f ms, swap %.1f ms, "
                    "rest %.1f ms | loaders io %ld/%lld KB %.1f ms\n",
                    frame, ms, g_main[GMV_PROF_TEX].n, g_main[GMV_PROF_TEX].bytes >> 10, tex,
                    g_main[GMV_PROF_BUF].n, g_main[GMV_PROF_BUF].bytes >> 10, buf,
                    g_main[GMV_PROF_IO].n, g_main[GMV_PROF_IO].bytes >> 10, io, lock, wait,
                    g_main[GMV_PROF_SLEEP].n, slp, swp, rest,
                    (long)g_bg_n, (long long)g_bg_bytes >> 10, ms_of(g_bg_qpc));
        report_samples(g_tick_t0, t);
        if (bp_made | bp_gone)
            gmv_logfile("[loadprof]   collision: %u proxies made, %u removed, %d bounds an axis\n", bp_made, bp_gone,
                        bp_bounds);
        for (int x = 0; x < SLEEP_KINDS && g_sleeps[x].n; x++)
            gmv_logfile("[loadprof]   sleep %u us x%d <- game 0x%06x <- 0x%06x <- 0x%06x\n",
                        g_sleeps[x].usec, g_sleeps[x].n, g_sleeps[x].ret[0], g_sleeps[x].ret[1],
                        g_sleeps[x].ret[2]);
        g_sum.ticks++;
        g_sum.ms += ms;
        g_sum.tex += tex + buf;
        g_sum.io += io;
        g_sum.lock += lock + wait;
        g_sum.rest += rest;
    }
    if (ms_of(t - g_sum.t0) >= 5000.0) {
        if (g_sum.ticks)
            gmv_log("[loadprof] last 5 s: %d slow ticks, %.0f ms in them: uploads %.0f, io %.0f, "
                    "lock/wait %.0f, rest %.0f ms\n", g_sum.ticks, g_sum.ms, g_sum.tex, g_sum.io,
                    g_sum.lock, g_sum.rest);
        memset(&g_sum, 0, sizeof g_sum);
        g_sum.t0 = t;
    }
}

void gmv_prof_tick_start(void)
{
    if (!gmv_prof_on() || !g_main_tid)
        return;
    memset(g_main, 0, sizeof g_main);
    memset(g_sleeps, 0, sizeof g_sleeps);
    InterlockedExchange(&g_bg_n, 0);
    InterlockedExchange64(&g_bg_bytes, 0);
    InterlockedExchange64(&g_bg_qpc, 0);
    g_tick_t0 = gmv_prof_now();
    g_in_tick = 1;
}
#endif
