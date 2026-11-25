#if defined _WIN32 && !defined _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat.h"
#ifdef _WIN32
#include <mmsystem.h>
#else
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "log.h"
#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

struct mac_timespec { int32_t tv_sec; int32_t tv_nsec; };
struct mac_timeval  { int32_t tv_sec; int32_t tv_usec; };
struct mac_tm {
    int sec, min, hour, mday, mon, year, wday, yday, isdst;
    long gmtoff;
    const char *zone;
};

/* __DefaultRuneLocale +0x34 */
#define RL_A 0x00000100u
#define RL_C 0x00000200u
#define RL_D 0x00000400u
#define RL_G 0x00000800u
#define RL_L 0x00001000u
#define RL_P 0x00002000u
#define RL_S 0x00004000u
#define RL_U 0x00008000u
#define RL_X 0x00010000u
#define RL_B 0x00020000u
#define RL_R 0x00040000u
static struct {
    char magic[8];
    char encoding[32];
    void *sgetrune, *sputrune;
    uint32_t invalid_rune;      /* 0x30 __runetype 0x34 */
    uint32_t runetype[256];
    uint32_t maplower[256];
    uint32_t mapupper[256];
    char tail[64];
} g_rune;

static void rune_init(void)
{
    memcpy(g_rune.magic, "RuneMagi", 8);
    memcpy(g_rune.encoding, "UTF-8", 5);
    for (int c = 0; c < 256; c++) {
        uint32_t t = 0;
        if (isalpha(c)) t |= RL_A;
        if (iscntrl(c)) t |= RL_C;
        if (isdigit(c)) t |= RL_D;
        if (isgraph(c)) t |= RL_G;
        if (islower(c)) t |= RL_L;
        if (ispunct(c)) t |= RL_P;
        if (isspace(c)) t |= RL_S;
        if (isupper(c)) t |= RL_U;
        if (isxdigit(c)) t |= RL_X;
        if (c == ' ' || c == '\t') t |= RL_B;
        if (isprint(c)) t |= RL_R;
        g_rune.runetype[c] = t;
        g_rune.maplower[c] = (uint32_t)tolower(c);
        g_rune.mapupper[c] = (uint32_t)toupper(c);
    }
}

static CRITICAL_SECTION g_lock;
static FILE *shim_stdoutp;
static FILE *shim_stderrp;
static int g_ready;

static void timer_resolution_init(void)
{
#ifndef _WIN32
}
#else
    timeBeginPeriod(1);
    typedef struct { ULONG Version, ControlMask, StateMask; } power_throttling;
    BOOL (WINAPI *setpi)(HANDLE, int, void *, DWORD) = (BOOL (WINAPI *)(HANDLE, int, void *, DWORD))
        (uintptr_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (setpi) {
        power_throttling pt = {1, 0x4, 0};
        setpi(GetCurrentProcess(), 4 , &pt, sizeof pt);
    }
}
#endif

void gmv_shims_init(void)
{
    if (g_ready)
        return;
    InitializeCriticalSection(&g_lock);
    shim_stdoutp = stdout;
    shim_stderrp = stderr;
    rune_init();
    timer_resolution_init();
    g_ready = 1;
}

#define MUTEX_MAGIC 0x584d564du
#define COND_MAGIC  0x43564d47u

typedef struct { uint32_t magic; CRITICAL_SECTION cs; } shim_mutex;
#ifdef _WIN32
typedef struct { uint32_t magic; CONDITION_VARIABLE cv; } shim_cond;
#define COND_CV(c) (&(c)->cv)
#else
typedef struct { uint32_t magic; CONDITION_VARIABLE *cv; } shim_cond;
#define COND_CV(c) ((c)->cv)
_Static_assert(sizeof(shim_mutex) <= 44, "a mutex must fit the Mac's pthread_mutex_t");
static void cond_make(shim_cond *c)
{
    c->cv = malloc(sizeof *c->cv);
    InitializeConditionVariable(c->cv);
}
#endif

static shim_mutex *mutex_of(void *p)
{
    shim_mutex *m = p;
    if (m->magic != MUTEX_MAGIC) {
        EnterCriticalSection(&g_lock);
        if (m->magic != MUTEX_MAGIC) {
            InitializeCriticalSection(&m->cs);
            m->magic = MUTEX_MAGIC;
        }
        LeaveCriticalSection(&g_lock);
    }
    return m;
}

static int shim_mutex_init(void *mutex, const void *attr)
{
    (void)attr;
    shim_mutex *m = mutex;
    InitializeCriticalSection(&m->cs);
    m->magic = MUTEX_MAGIC;
    return 0;
}

static int shim_mutex_lock(void *mutex)
{
    shim_mutex *m = mutex_of(mutex);
    if (!gmv_prof_on()) {
        EnterCriticalSection(&m->cs);
    } else if (!TryEnterCriticalSection(&m->cs)) {
        long long t0 = gmv_prof_now();
        EnterCriticalSection(&m->cs);
        gmv_prof_add(GMV_PROF_LOCK, t0, 0);
    }
    return 0;
}
static int shim_mutex_unlock(void *mutex)  { LeaveCriticalSection(&mutex_of(mutex)->cs); return 0; }
static int shim_mutex_trylock(void *mutex) { return TryEnterCriticalSection(&mutex_of(mutex)->cs) ? 0 : 16; }

static int shim_mutex_destroy(void *mutex)
{
    shim_mutex *m = mutex;
    if (m->magic == MUTEX_MAGIC) {
        DeleteCriticalSection(&m->cs);
        m->magic = 0;
    }
    return 0;
}

static int shim_mutexattr_noop(void *a)        { (void)a; return 0; }
static int shim_mutexattr_settype(void *a, int type) { (void)a; (void)type; return 0; }

static shim_cond *cond_of(void *p)
{
    shim_cond *c = p;
    if (c->magic != COND_MAGIC) {
        EnterCriticalSection(&g_lock);
        if (c->magic != COND_MAGIC) {
#ifdef _WIN32
            InitializeConditionVariable(&c->cv);
#else
            cond_make(c);
#endif
            c->magic = COND_MAGIC;
        }
        LeaveCriticalSection(&g_lock);
    }
    return c;
}

static int shim_cond_init(void *cond, const void *attr)
{
    (void)attr;
    shim_cond *c = cond;
#ifdef _WIN32
    InitializeConditionVariable(&c->cv);
#else
    cond_make(c);
#endif
    c->magic = COND_MAGIC;
    return 0;
}

static int g_thread_trace = -1;
static int thread_trace_on(void)
{
    if (!GMV_DEV)
        return 0;
    if (g_thread_trace < 0) {
        const char *e = gmv_dbgenv("GMV_THREAD_TRACE");
        g_thread_trace = gmv_env_on(e);
    }
    return g_thread_trace;
}

#ifdef _WIN32
static int shim_cond_destroy(void *cond)   { ((shim_cond *)cond)->magic = 0; return 0; }
#else
static int shim_cond_destroy(void *cond)
{
    shim_cond *c = cond;
    if (c->magic == COND_MAGIC) {
        pthread_cond_destroy(c->cv);
        free(c->cv);
        c->magic = 0;
    }
    return 0;
}
#endif
static int shim_cond_signal(void *cond)
{
    if (thread_trace_on())
        gmv_log("[thread] t%lu cond_signal    %p\n", GetCurrentThreadId(), cond);
    WakeConditionVariable(COND_CV(cond_of(cond)));
    return 0;
}
static int shim_cond_broadcast(void *cond)
{
    if (thread_trace_on())
        gmv_log("[thread] t%lu cond_broadcast %p\n", GetCurrentThreadId(), cond);
    WakeAllConditionVariable(COND_CV(cond_of(cond)));
    return 0;
}

static int shim_cond_wait(void *cond, void *mutex)
{
    if (thread_trace_on())
        gmv_log("[thread] t%lu cond_wait ->   %p\n", GetCurrentThreadId(), cond);
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    SleepConditionVariableCS(COND_CV(cond_of(cond)), &mutex_of(mutex)->cs, INFINITE);
    if (t0)
        gmv_prof_add(GMV_PROF_WAIT, t0, 0);
    return 0;
}

static int shim_cond_timedwait(void *cond, void *mutex, const struct mac_timespec *abstime)
{
    DWORD ms = INFINITE;
    if (abstime) {
        struct mac_timeval now;
        extern int gmv_gettimeofday(struct mac_timeval *, void *);
        gmv_gettimeofday(&now, NULL);
        long long rel = ((long long)abstime->tv_sec - now.tv_sec) * 1000
                      + ((long long)abstime->tv_nsec / 1000000 - now.tv_usec / 1000);
        ms = rel <= 0 ? 0 : (DWORD)rel;
    }
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    BOOL woke = SleepConditionVariableCS(COND_CV(cond_of(cond)), &mutex_of(mutex)->cs, ms);
    DWORD err = woke ? 0 : GetLastError();
    if (t0)
        gmv_prof_add(GMV_PROF_WAIT, t0, 0);
    if (woke)
        return 0;
    return err == ERROR_TIMEOUT ? 60 : 0;
}

static struct { unsigned long tid; HANDLE h; } g_threads[256];
static int g_nthreads;

typedef void *(*pthread_fn)(void *);
typedef struct { pthread_fn fn; void *arg; } thread_start;

__attribute__((force_align_arg_pointer))
static DWORD WINAPI thread_tramp(LPVOID p)
{
    thread_start s = *(thread_start *)p;
    free(p);
    return (DWORD)(uintptr_t)s.fn(s.arg);
}

static int shim_pthread_create(unsigned long *thread, const void *attr, pthread_fn fn, void *arg)
{
    (void)attr;
    thread_start *s = malloc(sizeof *s);
    if (!s)
        return 11;
    s->fn = fn;
    s->arg = arg;
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, thread_tramp, s, 0, &tid);
    if (!h) {
        free(s);
        return 11;
    }
    if (GMV_DEV) {
        static int tr = -1;
        if (tr < 0) { const char *e = gmv_dbgenv("GMV_THREAD_TRACE"); tr = gmv_env_on(e); }
        if (tr)
            gmv_log("[thread] pthread_create fn=0x%08x arg=0x%08x -> tid=%lu\n",
                    (unsigned)(uintptr_t)fn, (unsigned)(uintptr_t)arg, (unsigned long)tid);
    }
    EnterCriticalSection(&g_lock);
    if (g_nthreads < (int)(sizeof g_threads / sizeof *g_threads)) {
        g_threads[g_nthreads].tid = tid;
        g_threads[g_nthreads].h = h;
        g_nthreads++;
    } else {
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
    *thread = tid;
    return 0;
}

static int shim_pthread_join(unsigned long thread, void **retval)
{
    HANDLE h = NULL;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_nthreads; i++)
        if (g_threads[i].tid == thread) {
            h = g_threads[i].h;
            g_threads[i] = g_threads[--g_nthreads];
            break;
        }
    LeaveCriticalSection(&g_lock);
    if (!h)
        return 3;
    WaitForSingleObject(h, INFINITE);
    if (retval) {
        DWORD code = 0;
        GetExitCodeThread(h, &code);
        *retval = (void *)(uintptr_t)code;
    }
    CloseHandle(h);
    return 0;
}

static unsigned long shim_pthread_self(void)              { return GetCurrentThreadId(); }
static int shim_pthread_equal(unsigned long a, unsigned long b) { return a == b; }
static int shim_pthread_getschedparam(unsigned long t, int *policy, void *param)
{
    (void)t;
    if (policy) *policy = 1;
    (void)param;
    return 0;
}

static int shim_key_create(unsigned long *key, void (*destr)(void *))
{
    (void)destr;
    DWORD k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES)
        return 11;
    *key = k;
    return 0;
}
static int shim_key_delete(unsigned long key)             { TlsFree(key); return 0; }
static void *shim_getspecific(unsigned long key)          { return TlsGetValue(key); }
static int shim_setspecific(unsigned long key, const void *v) { return TlsSetValue(key, (void *)v) ? 0 : 22; }

static int shim_attr_noop(void *a)                        { (void)a; return 0; }
static int shim_attr_set1(void *a, long v)                { (void)a; (void)v; return 0; }

#define MAC_PROT_READ  0x1
#define MAC_PROT_WRITE 0x2
#define MAC_PROT_EXEC  0x4
#define MAC_MAP_ANON   0x1000
#define MAC_MAP_FAILED ((void *)-1)

#ifndef _WIN32
static void *shim_mmap(void *addr, size_t len, int prot, int flags, int fd, long long offset)
{
    (void)offset;
    if (!(flags & MAC_MAP_ANON) || fd != -1) {
        gmv_log("[shim] mmap of a file (fd %d, flags 0x%x) is not supported yet\n", fd, flags);
        return MAC_MAP_FAILED;
    }
    void *p = mmap(addr, len, prot & (MAC_PROT_READ | MAC_PROT_WRITE | MAC_PROT_EXEC), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? MAC_MAP_FAILED : p;
}

static int shim_munmap(void *addr, size_t len)
{
    return munmap(addr, len) ? -1 : 0;
}
#else
static DWORD prot_to_win(int prot)
{
    switch (prot & (MAC_PROT_READ | MAC_PROT_WRITE | MAC_PROT_EXEC)) {
    case 0:                                                  return PAGE_NOACCESS;
    case MAC_PROT_READ:                                      return PAGE_READONLY;
    case MAC_PROT_READ | MAC_PROT_WRITE:                     return PAGE_READWRITE;
    case MAC_PROT_WRITE:                                     return PAGE_READWRITE;
    case MAC_PROT_EXEC:                                      return PAGE_EXECUTE;
    case MAC_PROT_READ | MAC_PROT_EXEC:                      return PAGE_EXECUTE_READ;
    default:                                                 return PAGE_EXECUTE_READWRITE;
    }
}

static void *shim_mmap(void *addr, size_t len, int prot, int flags, int fd, long long offset)
{
    (void)offset;
    if (!(flags & MAC_MAP_ANON) || fd != -1) {
        gmv_log("[shim] mmap of a file (fd %d, flags 0x%x) is not supported yet\n", fd, flags);
        return MAC_MAP_FAILED;
    }
    void *p = VirtualAlloc(addr, len, MEM_RESERVE | MEM_COMMIT, prot_to_win(prot));
    if (!p && addr)
        p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, prot_to_win(prot));
    return p ? p : MAC_MAP_FAILED;
}

static int shim_munmap(void *addr, size_t len)
{
    (void)len;
    if (VirtualFree(addr, 0, MEM_RELEASE))
        return 0;
    gmv_log("[shim] munmap(0x%p) did not match a whole mmap region\n", addr);
    return -1;
}
#endif

static int shim_sched_yield(void)
{
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    SwitchToThread();
    if (t0)
        gmv_prof_add(GMV_PROF_SLEEP, t0, 0);
    return 0;
}

static void sleep_us(uint64_t usec)
{
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
#ifdef _WIN32
    if (usec < 1000)
        Sleep(0);
    else
        Sleep((DWORD)((usec + 500) / 1000));
#else
    struct timespec t = {(time_t)(usec / 1000000u), (long)(usec % 1000000u) * 1000L};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
#endif
    if (t0) {
        gmv_prof_add(GMV_PROF_SLEEP, t0, 0);
        gmv_prof_sleep_note((unsigned)usec);
    }
}

static int shim_nanosleep(const struct mac_timespec *req, struct mac_timespec *rem)
{
    if (req)
        sleep_us((uint64_t)req->tv_sec * 1000000u + (uint32_t)req->tv_nsec / 1000u);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static int shim_usleep(unsigned int usec) { sleep_us(usec); return 0; }

static int shim_sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp,
                       void *newp, size_t newlen)
{
    (void)name; (void)namelen; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    return -1;
}
static int shim_sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                             void *newp, size_t newlen)
{
    (void)name; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    return -1;
}

static size_t shim_wcslen(const uint32_t *s) { size_t n = 0; if (s) while (s[n]) n++; return n; }
static int shim_wcscmp(const uint32_t *a, const uint32_t *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}
static uint32_t *shim_wcscpy(uint32_t *d, const uint32_t *s)
{
    uint32_t *r = d;
    while ((*d++ = *s++)) {}
    return r;
}
static void *shim_wmemcpy(uint32_t *d, const uint32_t *s, size_t n) { for (size_t i = 0; i < n; i++) d[i] = s[i]; return d; }
static void *shim_wmemmove(uint32_t *d, const uint32_t *s, size_t n) { memmove(d, s, n * 4); return d; }
static void *shim_wmemchr(const uint32_t *s, uint32_t c, size_t n) { for (size_t i = 0; i < n; i++) if (s[i] == c) return (void *)(s + i); return NULL; }
static int shim_wmemcmp(const uint32_t *a, const uint32_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static int wide_put_str(uint32_t *out, size_t n, size_t *o, const void *s, int wide,
                        int width, int prec, int left)
{
    static const uint32_t wnull[] = {'(', 'n', 'u', 'l', 'l', ')', 0};
    size_t len = 0;
    if (!s)
        s = wide ? (const void *)wnull : (const void *)"(null)";
    if (wide) {
        const uint32_t *w = s;
        while ((prec < 0 || (int)len < prec) && w[len])
            len++;
    } else {
        const unsigned char *c = s;
        while ((prec < 0 || (int)len < prec) && c[len])
            len++;
    }
    int pad = width > (int)len ? width - (int)len : 0;
    int over = 0;
    for (int pass = 0; pass < 3; pass++) {
        size_t cnt = pass == 1 ? len : (size_t)((pass == 0) != left ? pad : 0);
        for (size_t i = 0; i < cnt; i++, (*o)++) {
            uint32_t ch = pass != 1 ? ' ' : wide ? ((const uint32_t *)s)[i] : ((const unsigned char *)s)[i];
            if (*o + 1 < n)
                out[*o] = ch;
            else
                over = 1;
        }
    }
    return over;
}

static int shim_swprintf(uint32_t *out, size_t n, const uint32_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t o = 0;
    int over = 0;
    for (const uint32_t *f = fmt; f && *f; f++) {
        if (*f != '%') {
            if (o + 1 < n) out[o] = *f; else over = 1;
            o++;
            continue;
        }
        char spec[48];
        int k = 0, width = -1, prec = -1, left = 0, lng = 0, half = 0, ldbl = 0;
        spec[k++] = '%';
        f++;
        while (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0') {
            left |= *f == '-';
            spec[k++] = (char)*f++;
        }
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; spec[k++] = '-'; width = -width; }
            f++;
        } else if (*f >= '0' && *f <= '9') {
            for (width = 0; *f >= '0' && *f <= '9'; f++)
                width = width * 10 + (int)(*f - '0');
        }
        if (width >= 0)
            k += snprintf(spec + k, 12, "%d", width);
        if (*f == '.') {
            f++;
            if (*f == '*') {
                prec = va_arg(ap, int);
                f++;
            } else {
                for (prec = 0; *f >= '0' && *f <= '9'; f++)
                    prec = prec * 10 + (int)(*f - '0');
            }
            if (prec >= 0)
                k += snprintf(spec + k, 12, ".%d", prec);
        }
        for (;; f++) {
            if (*f == 'l') lng++;
            else if (*f == 'q' || *f == 'j') lng = 2;
            else if (*f == 'h') half++;
            else if (*f == 'L') ldbl = 1;
            else if (*f != 'z' && *f != 't') break;
        }
        uint32_t c = *f;
        if (!c)
            break;
        char tmp[128];
        int m = 0;
        switch (c) {
        case '%':
            tmp[0] = '%';
            m = 1;
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (lng >= 2) {
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = (char)c; spec[k] = 0;
                m = snprintf(tmp, sizeof tmp, spec, va_arg(ap, long long));
            } else {
                for (int i = 0; i < half && i < 2; i++)
                    spec[k++] = 'h';
                spec[k++] = (char)c; spec[k] = 0;
                m = snprintf(tmp, sizeof tmp, spec, va_arg(ap, int));
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = ldbl ? (double)va_arg(ap, long double) : va_arg(ap, double);
            spec[k++] = (char)c; spec[k] = 0;
            m = snprintf(tmp, sizeof tmp, spec, v);
            break;
        }
        case 'p':
            spec[k++] = 'p'; spec[k] = 0;
            m = snprintf(tmp, sizeof tmp, spec, va_arg(ap, void *));
            break;
        case 'c': case 'C': {
            uint32_t ch = (c == 'C' || lng) ? va_arg(ap, uint32_t) : (unsigned char)va_arg(ap, int);
            uint32_t one[2] = {ch, 0};
            over |= wide_put_str(out, n, &o, one, 1, width, -1, left);
            continue;
        }
        case 's': case 'S':
            over |= wide_put_str(out, n, &o, va_arg(ap, const void *), c == 'S' || lng, width, prec, left);
            continue;
        case 'n':
            *va_arg(ap, int *) = (int)o;
            continue;
        default:
            tmp[0] = '%';
            tmp[1] = c < 0x80 ? (char)c : '?';
            m = 2;
            break;
        }
        if (m < 0)
            m = 0;
        if (m >= (int)sizeof tmp)
            m = (int)sizeof tmp - 1;
        for (int i = 0; i < m; i++, o++) {
            if (o + 1 < n) out[o] = (unsigned char)tmp[i]; else over = 1;
        }
    }
    va_end(ap);
    if (n)
        out[o < n ? o : n - 1] = 0;
    return over ? -1 : (int)o;
}

#define MAC_DIRENT_SIZE 1048
static void st_put16(char *p, uint16_t v);
static void st_put64(char *p, uint64_t v);
#ifndef _WIN32
typedef struct {
    DIR *d;
    uint32_t count;
    unsigned char ent[MAC_DIRENT_SIZE];
} shim_dir;

static void *shim_opendir(const char *path)
{
    shim_dir *d = path ? calloc(1, sizeof *d) : NULL;
    if (!d) {
        errno = path ? ENOMEM : ENOENT;
        return NULL;
    }
    if (!(d->d = opendir(path))) {
        free(d);
        return NULL;
    }
    return d;
}

static void *shim_readdir(void *dir)
{
    shim_dir *d = dir;
    struct dirent *de = d ? readdir(d->d) : NULL;
    if (!de)
        return NULL;
    unsigned char *e = d->ent;
    memset(e, 0, MAC_DIRENT_SIZE);
    size_t nl = strlen(de->d_name);
    if (nl > 1023)
        nl = 1023;
    st_put64((char *)e + 0, ++d->count);
    st_put64((char *)e + 8, d->count);
    st_put16((char *)e + 16, MAC_DIRENT_SIZE);
    st_put16((char *)e + 18, (uint16_t)nl);
    e[20] = de->d_type == DT_DIR ? 4 : 8;
    memcpy(e + 21, de->d_name, nl);
    return e;
}

static int shim_closedir(void *dir)
{
    shim_dir *d = dir;
    if (!d)
        return -1;
    closedir(d->d);
    free(d);
    return 0;
}
#else
typedef struct {
    HANDLE h;
    int pending;
    uint32_t count;
    WIN32_FIND_DATAA fd;
    unsigned char ent[MAC_DIRENT_SIZE];
} shim_dir;

static void *shim_opendir(const char *path)
{
    if (!path) {
        errno = ENOENT;
        return NULL;
    }
    if (path[0] == '.' && (path[1] == '/' || path[1] == '\\') && path[2] && path[3] == ':')
        path += 2;
    char pat[MAX_PATH + 4];
    size_t l = strlen(path);
    if (l + 3 > MAX_PATH) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(pat, path, l);
    if (l && pat[l - 1] != '/' && pat[l - 1] != '\\')
        pat[l++] = '\\';
    pat[l++] = '*';
    pat[l] = 0;
    shim_dir *d = calloc(1, sizeof *d);
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        errno = ENOENT;
        return NULL;
    }
    d->pending = 1;
    return d;
}

static void *shim_readdir(void *dir)
{
    shim_dir *d = dir;
    if (!d)
        return NULL;
    if (!d->pending && !FindNextFileA(d->h, &d->fd))
        return NULL;
    d->pending = 0;
    unsigned char *e = d->ent;
    memset(e, 0, MAC_DIRENT_SIZE);
    size_t nl = strlen(d->fd.cFileName);
    if (nl > 1023)
        nl = 1023;
    st_put64((char *)e + 0, ++d->count);
    st_put64((char *)e + 8, d->count);
    st_put16((char *)e + 16, MAC_DIRENT_SIZE);
    st_put16((char *)e + 18, (uint16_t)nl);
    e[20] = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    memcpy(e + 21, d->fd.cFileName, nl);
    return e;
}

static int shim_closedir(void *dir)
{
    shim_dir *d = dir;
    if (!d)
        return -1;
    FindClose(d->h);
    free(d);
    return 0;
}
#endif

extern void gmv_setjmp(void);
extern void gmv_longjmp(void);
static void *shim_ret_null(void) { return NULL; }
static int shim_ret0(void) { return 0; }
static void *g_tis_keylayout_key;

static int shim_chdir(const char *path) { return SetCurrentDirectoryA(path) ? 0 : -1; }
static int shim_mkdir(const char *path, unsigned mode) { (void)mode; return CreateDirectoryA(path, NULL) ? 0 : -1; }

static void st_put16(char *p, uint16_t v) { memcpy(p, &v, 2); }
static void st_put32(char *p, uint32_t v) { memcpy(p, &v, 4); }
static void st_put64(char *p, uint64_t v) { memcpy(p, &v, 8); }

#ifndef _WIN32
static int shim_stat(const char *path, void *buf)
{
    struct stat st;
    if (!path || !buf || stat(path, &st))
        return -1;
    char *b = buf;
    memset(b, 0, 108);
    st_put32(b + 0, (uint32_t)st.st_dev);
    st_put16(b + 4, (uint16_t)st.st_mode);
    st_put16(b + 6, (uint16_t)st.st_nlink);
    st_put64(b + 8, (uint64_t)st.st_ino);
    st_put32(b + 16, st.st_uid);
    st_put32(b + 20, st.st_gid);
    st_put32(b + 24, (uint32_t)st.st_rdev);
    st_put32(b + 28, (uint32_t)st.st_atim.tv_sec); st_put32(b + 32, (uint32_t)st.st_atim.tv_nsec);
    st_put32(b + 36, (uint32_t)st.st_mtim.tv_sec); st_put32(b + 40, (uint32_t)st.st_mtim.tv_nsec);
    st_put32(b + 44, (uint32_t)st.st_ctim.tv_sec); st_put32(b + 48, (uint32_t)st.st_ctim.tv_nsec);
    st_put32(b + 52, (uint32_t)st.st_ctim.tv_sec); st_put32(b + 56, (uint32_t)st.st_ctim.tv_nsec);
    st_put64(b + 60, (uint64_t)st.st_size);
    st_put64(b + 68, (uint64_t)st.st_blocks);
    st_put32(b + 76, (uint32_t)st.st_blksize);
    return 0;
}
#else
static uint32_t filetime_to_unix(const FILETIME *ft)
{
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (t < 116444736000000000ULL)
        return 0;
    return (uint32_t)((t - 116444736000000000ULL) / 10000000ULL);
}

static int shim_stat(const char *path, void *buf)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!path || !buf || !GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        errno = ENOENT;
        return -1;
    }
    char *b = buf;
    memset(b, 0, 108);
    int isdir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    uint64_t size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    uint32_t at = filetime_to_unix(&fad.ftLastAccessTime);
    uint32_t mt = filetime_to_unix(&fad.ftLastWriteTime);
    uint32_t ct = filetime_to_unix(&fad.ftCreationTime);

    st_put32(b + 0, 0);
    st_put16(b + 4, isdir ? (0040000 | 0777) : (0100000 | 0666));
    st_put16(b + 6, 1);
    st_put64(b + 8, 1);
    st_put32(b + 16, 0);
    st_put32(b + 20, 0);
    st_put32(b + 24, 0);
    st_put32(b + 28, at); st_put32(b + 32, 0);
    st_put32(b + 36, mt); st_put32(b + 40, 0);
    st_put32(b + 44, ct); st_put32(b + 48, 0);
    st_put32(b + 52, ct); st_put32(b + 56, 0);
    st_put64(b + 60, size);
    st_put64(b + 68, (size + 511) / 512);
    st_put32(b + 76, 4096);
    return 0;
}
#endif

static int shim_UpdateSystemActivity(unsigned char activity)
{
    (void)activity;
#ifdef _WIN32
    SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
#endif
    return 0;
}

#ifndef _WIN32
static long shim_sysconf(int name)
{
    switch (name) {
    case 29:
        return sysconf(_SC_PAGESIZE);
    case 57: case 58:
        return sysconf(name == 57 ? _SC_NPROCESSORS_CONF : _SC_NPROCESSORS_ONLN);
    case 5:  return 256;
    case 3:  return 100;
    default: return -1;
    }
}
#else
static long shim_sysconf(int name)
{
    SYSTEM_INFO si;
    switch (name) {
    case 29:
        GetSystemInfo(&si);
        return (long)si.dwPageSize;
    case 57: case 58:
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    case 5:  return 256;
    case 3:  return 100;
    default: return -1;
    }
}
#endif

static uint64_t shim_mach_absolute_time(void)
{
    long long c = gmv_qpc();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c * 1e9 / (double)f.QuadPart);
}

static int shim_mach_timebase_info(uint32_t *info)
{
    if (info) {
        info[0] = 1;
        info[1] = 1;
    }
    return 0;
}

static int32_t shim_time(int32_t *t)
{
    int32_t v = (int32_t)time(NULL);
    if (t)
        *t = v;
    return v;
}

int gmv_gettimeofday(struct mac_timeval *tv, void *tz)
{
    (void)tz;
#ifndef _WIN32
    if (tv) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        tv->tv_sec = (int32_t)ts.tv_sec;
        tv->tv_usec = (int32_t)(ts.tv_nsec / 1000);
    }
#else
    if (tv) {
        static void (WINAPI *precise)(LPFILETIME) = NULL;
        static int resolved = 0;
        if (!resolved) {
            resolved = 1;
            HMODULE k = GetModuleHandleA("kernel32.dll");
            if (k)
                precise = (void (WINAPI *)(LPFILETIME))(uintptr_t)
                          GetProcAddress(k, "GetSystemTimePreciseAsFileTime");
        }
        FILETIME ft;
        if (precise)
            precise(&ft);
        else
            GetSystemTimeAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        t -= 116444736000000000ULL;
        tv->tv_sec = (int32_t)(t / 10000000ULL);
        tv->tv_usec = (int32_t)((t % 10000000ULL) / 10);
    }
#endif
    if (GMV_DEV) {
        static int lt = -1;
        if (lt < 0) { const char *e = gmv_dbgenv("GMV_LOOP_TRACE"); lt = (gmv_env_on(e)) ? 1 : 0; }
        if (lt) {
            void *caller = __builtin_return_address(0);
            static void *who[32]; static unsigned cnt[32], nw, total;
            int idx = -1;
            for (unsigned i = 0; i < nw; i++) if (who[i] == caller) { idx = (int)i; break; }
            if (idx < 0 && nw < 32) { idx = (int)nw++; who[idx] = caller; }
            if (idx >= 0) cnt[idx]++;
            if ((++total % 600) == 0) {
                gmv_log("[loop] gettimeofday callers after %u calls:\n", total);
                for (unsigned i = 0; i < nw; i++)
                    gmv_log("[loop]   caller %p : %u calls\n", who[i], cnt[i]);
            }
        }
    }
    return 0;
}

static struct mac_tm *fill_tm(struct mac_tm *d, const struct tm *s, long gmtoff, const char *zone)
{
    if (!s)
        return NULL;
    d->sec = s->tm_sec;   d->min = s->tm_min;   d->hour = s->tm_hour;
    d->mday = s->tm_mday; d->mon = s->tm_mon;   d->year = s->tm_year;
    d->wday = s->tm_wday; d->yday = s->tm_yday; d->isdst = s->tm_isdst;
    d->gmtoff = gmtoff;
    d->zone = zone;
    return d;
}

static struct mac_tm *shim_gmtime(const int32_t *tp)
{
    static struct mac_tm out;
    time_t t = tp ? *tp : 0;
    return fill_tm(&out, gmtime(&t), 0, "UTC");
}

static struct mac_tm *shim_localtime(const int32_t *tp)
{
    static struct mac_tm out;
    time_t t = tp ? *tp : 0;
#ifdef _WIN32
    _tzset();
    return fill_tm(&out, localtime(&t), -_timezone, _tzname[0]);
#else
    static struct tm tm;
    return localtime_r(&t, &tm) ? fill_tm(&out, &tm, tm.tm_gmtoff, tm.tm_zone) : NULL;
#endif
}

static __attribute__((noinline)) int savegame_redirect(const char *path, char *out, size_t n)
{
    if (!path)
        return 0;
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    size_t bl = strlen(base);
    if (bl < 5 || bl > 63)
        return 0;
    int is_save = (!strcmp(base + bl - 4,  ".sav"));
    int is_pref = (bl > 12 && !strcmp(base + bl - 12, ".preferences"));
    if (!is_save && !is_pref)
        return 0;
    char exe[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (!k || k >= sizeof exe)
        return 0;
    char *slash = strrchr(exe, GMV_DIRSEP);
    if (!slash)
        return 0;
    slash[1] = 0;
    char dir[MAX_PATH + 16];
    snprintf(dir, sizeof dir, "%ssavegame", exe);
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s" GMV_SEP "%s", dir, base);
    out[n - 1] = 0;
    return 1;
}

static CRITICAL_SECTION g_wavmap_cs;
static int g_wavmap_mode = -1;
#define WAVPEND_SLOTS 64
static struct { DWORD tid; char wav[104]; } g_wavpend[WAVPEND_SLOTS];

static const char *wav_basename(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static int wav_ends_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++) {
        char a = s[ls - lf + i], c = suf[i];
        if ((a | 0x20) != (c | 0x20)) return 0;
    }
    return 1;
}

static void wavmap_note(const char *path, int ok)
{
    if (!GMV_DEV)
        return;
    if (g_wavmap_mode < 0) {
        const char *e = gmv_dbgenv("GMV_WAVMAP");
        g_wavmap_mode = (gmv_env_on(e)) ? 1 : 0;
        if (g_wavmap_mode)
            InitializeCriticalSection(&g_wavmap_cs);
    }
    if (g_wavmap_mode != 1 || !path)
        return;

    const char *b = wav_basename(path);
    DWORD tid = GetCurrentThreadId();

    if (!ok && wav_ends_ci(b, ".wav")) {
        EnterCriticalSection(&g_wavmap_cs);
        int slot = -1, freeslot = -1;
        for (int i = 0; i < WAVPEND_SLOTS; i++) {
            if (g_wavpend[i].tid == tid) { slot = i; break; }
            if (freeslot < 0 && g_wavpend[i].tid == 0) freeslot = i;
        }
        if (slot < 0) slot = (freeslot >= 0) ? freeslot : 0;
        g_wavpend[slot].tid = tid;
        snprintf(g_wavpend[slot].wav, sizeof g_wavpend[slot].wav, "%.103s", b);
        LeaveCriticalSection(&g_wavmap_cs);
        return;
    }

    if (ok && strncmp(b, "file", 4) == 0 && wav_ends_ci(b, ".dat")) {
        char matched[104];
        matched[0] = 0;
        EnterCriticalSection(&g_wavmap_cs);
        for (int i = 0; i < WAVPEND_SLOTS; i++) {
            if (g_wavpend[i].tid == tid && g_wavpend[i].wav[0]) {
                snprintf(matched, sizeof matched, "%s", g_wavpend[i].wav);
                g_wavpend[i].tid = 0;
                g_wavpend[i].wav[0] = 0;
                break;
            }
        }
        LeaveCriticalSection(&g_wavmap_cs);
        if (matched[0])
            gmv_log("[wavmap] %s -> %s\n", matched, b);
    }
}

static CRITICAL_SECTION g_fmiss_cs;
static struct { DWORD tid; int err; char name[104]; char path[MAX_PATH]; } g_fmiss[WAVPEND_SLOTS];

static void fmiss_note(const char *path, int ok)
{
    int err = errno;
    static int init;
    if (!init) {
        InitializeCriticalSection(&g_fmiss_cs);
        init = 1;
        gmv_log("[fmiss] FOPEN_TRACE=2: logging only assets with no pack file\n");
    }
    if (!path)
        return;
    const char *b = wav_basename(path);
    int pack = ok && strncmp(b, "file", 4) == 0 && wav_ends_ci(b, ".dat");
    DWORD tid = GetCurrentThreadId();
    char missed[MAX_PATH];
    int missed_err = 0;
    missed[0] = 0;

    EnterCriticalSection(&g_fmiss_cs);
    int slot = -1, freeslot = -1;
    for (int i = 0; i < WAVPEND_SLOTS; i++) {
        if (g_fmiss[i].tid == tid) { slot = i; break; }
        if (freeslot < 0 && g_fmiss[i].tid == 0) freeslot = i;
    }
    if (slot >= 0 && strcmp(g_fmiss[slot].name, b) != 0 && !pack) {
        snprintf(missed, sizeof missed, "%s", g_fmiss[slot].path);
        missed_err = g_fmiss[slot].err;
    }
    if (!ok) {
        if (slot < 0) slot = (freeslot >= 0) ? freeslot : 0;
        g_fmiss[slot].tid = tid;
        g_fmiss[slot].err = err;
        snprintf(g_fmiss[slot].name, sizeof g_fmiss[slot].name, "%.103s", b);
        snprintf(g_fmiss[slot].path, sizeof g_fmiss[slot].path, "%.259s", path);
    } else if (slot >= 0) {
        g_fmiss[slot].tid = 0;
        g_fmiss[slot].name[0] = 0;
    }
    LeaveCriticalSection(&g_fmiss_cs);
    if (missed[0])
        gmv_log("[fmiss] \"%s\" errno=%d (no pack file followed)\n", missed, missed_err);
}

/* FAST_FTELL 0x6839a8 0x5a0320 0x5a0170 */
#define FPOS_N 4096
static struct { FILE *f; long pos; int known, bin; } g_fpos[FPOS_N];
static int g_fpos_used;
static SRWLOCK g_fpos_lock = SRWLOCK_INIT;
static int g_fast_ftell = -1;

static int fast_ftell_on(void)
{
#ifndef _WIN32
    return 0;
#endif
    if (g_fast_ftell < 0) {
        const char *e = gmv_getenv("GMV_FAST_FTELL");
        g_fast_ftell = gmv_env_flag(e, 1);
    }
    return g_fast_ftell;
}

static unsigned fpos_hash(FILE *f)
{
    return ((uint32_t)(uintptr_t)f * 2654435761u) >> 20;
}

static __typeof__(&g_fpos[0]) fpos_find(FILE *f)
{
    for (unsigned i = fpos_hash(f); g_fpos[i].f; i = (i + 1) & (FPOS_N - 1))
        if (g_fpos[i].f == f)
            return &g_fpos[i];
    return NULL;
}

static void fpos_add(FILE *f, int bin)
{
    AcquireSRWLockExclusive(&g_fpos_lock);
    if (!fpos_find(f) && g_fpos_used < FPOS_N / 2) {
        unsigned i = fpos_hash(f);
        while (g_fpos[i].f)
            i = (i + 1) & (FPOS_N - 1);
        g_fpos[i].f = f;
        g_fpos[i].pos = 0;
        g_fpos[i].known = 1;
        g_fpos[i].bin = bin;
        g_fpos_used++;
    }
    ReleaseSRWLockExclusive(&g_fpos_lock);
}

static void fpos_remove(FILE *f)
{
    AcquireSRWLockExclusive(&g_fpos_lock);
    __typeof__(&g_fpos[0]) s = fpos_find(f);
    if (s) {
        unsigned i = (unsigned)(s - g_fpos), j = i;
        for (;;) {
            j = (j + 1) & (FPOS_N - 1);
            if (!g_fpos[j].f)
                break;
            unsigned k = fpos_hash(g_fpos[j].f);
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
                continue;
            g_fpos[i] = g_fpos[j];
            i = j;
        }
        g_fpos[i].f = NULL;
        g_fpos_used--;
    }
    ReleaseSRWLockExclusive(&g_fpos_lock);
}

#define FREAD_BUF (64 * 1024)
static int g_fast_fseek = -1;

static int fast_fseek_on(void)
{
#ifndef _WIN32
    return 0;
#endif
    if (g_fast_fseek < 0) {
        const char *e = gmv_getenv("GMV_FAST_FSEEK");
        g_fast_fseek = gmv_env_flag(e, 1) && fast_ftell_on();
    }
    return g_fast_fseek;
}

static int seek_in_buffer(FILE *f, long pos, long to)
{
    int ok = 0;
#ifndef _WIN32
    (void)f; (void)pos; (void)to;
    return ok;
#else
    _lock_file(f);
    if ((f->_flag & (_IOREAD | _IOWRT | _IORW)) == _IOREAD && f->_base && f->_cnt > 0 &&
        f->_ptr >= f->_base && (f->_ptr - f->_base) + f->_cnt <= f->_bufsiz) {
        long start = pos - (long)(f->_ptr - f->_base), end = pos + f->_cnt;
        if (to >= start && to <= end) {
            f->_ptr = f->_base + (to - start);
            f->_cnt = (int)(end - to);
            f->_flag &= ~_IOEOF;
            ok = 1;
        }
    }
    _unlock_file(f);
    return ok;
#endif
}

static FILE *shim_fopen_inner(const char *path, const char *mode);

static volatile LONG g_files_open;
static char g_fopen_fail[MAX_PATH];
static volatile int g_fopen_errno;

void gmv_fopen_info(const char **fail_path, int *fail_errno, long *open_files)
{
    *fail_path = g_fopen_fail;
    *fail_errno = g_fopen_errno;
    *open_files = g_files_open;
}

#if GMV_DEV
static volatile LONG fs_open, fs_fail, fs_fail_bundle, fs_rb_bundle, fs_reopen, fs_close, fs_stat, fs_stat_fail,
    fs_stat_fail_bundle, fs_miss_hit, fs_pool_hit;
static long long fs_fail_ticks, fs_open_ticks, fs_stat_ticks;
static SRWLOCK fs_lock = SRWLOCK_INIT;
static uint32_t fs_recent[64];
static unsigned fs_recent_at;

static void fs_note_open(const char *path, const char *mode, int ok, long long t0)
{
    if (!gmv_perf_on())
        return;
    long long dt = gmv_qpc() - t0;
    int bundle = path && gmv_in_bundle(path[0] == '.' && (path[1] == '/' || path[1] == '\\') ? path + 2 : path);
    int rb = mode && mode[0] == 'r' && !strchr(mode, '+');
    AcquireSRWLockExclusive(&fs_lock);
    fs_open++;
    fs_open_ticks += dt;
    if (!ok) {
        fs_fail++;
        fs_fail_ticks += dt;
        fs_fail_bundle += bundle;
    } else if (bundle && rb) {
        fs_rb_bundle++;
        uint32_t h = gmv_fnv1a(path);
        for (int i = 0; i < 64; i++)
            if (fs_recent[i] == h) {
                fs_reopen++;
                break;
            }
        fs_recent[fs_recent_at++ & 63] = h;
    }
    ReleaseSRWLockExclusive(&fs_lock);
}

static void fs_note_stat(const char *path, int ok, long long t0)
{
    if (!gmv_perf_on())
        return;
    long long dt = gmv_qpc() - t0;
    AcquireSRWLockExclusive(&fs_lock);
    fs_stat++;
    fs_stat_ticks += dt;
    if (!ok) {
        fs_stat_fail++;
        fs_stat_fail_bundle += path && gmv_in_bundle(path);
    }
    ReleaseSRWLockExclusive(&fs_lock);
}

void gmv_file_stats_report(double seconds)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double ms = 1000.0 / (double)f.QuadPart;
    AcquireSRWLockExclusive(&fs_lock);
    gmv_logfile("[perf]   files (all threads): %.0f opens a second (%.2f ms), %.0f failed (%.0f in the bundle, %.0f of them "
                "answered by FILE_MISS_CACHE; %.2f ms), %.0f read-only in the bundle (%.0f of the last 64 again, %.0f "
                "taken from FILE_POOL), %.0f closes; %.0f stats (%.2f ms), %.0f failed (%.0f in the bundle)\n",
                fs_open / seconds, fs_open_ticks * ms / seconds, fs_fail / seconds, fs_fail_bundle / seconds,
                fs_miss_hit / seconds, fs_fail_ticks * ms / seconds, fs_rb_bundle / seconds, fs_reopen / seconds,
                fs_pool_hit / seconds, fs_close / seconds, fs_stat / seconds, fs_stat_ticks * ms / seconds,
                fs_stat_fail / seconds, fs_stat_fail_bundle / seconds);
    fs_open = fs_fail = fs_fail_bundle = fs_rb_bundle = fs_reopen = fs_close = fs_stat = fs_stat_fail =
        fs_stat_fail_bundle = fs_miss_hit = fs_pool_hit = 0;
    fs_fail_ticks = fs_open_ticks = fs_stat_ticks = 0;
    ReleaseSRWLockExclusive(&fs_lock);
}
#else
#define gmv_qpc() 0LL
#define fs_note_open(path, mode, ok, t0) ((void)0)
#define fs_note_stat(path, ok, t0) ((void)0)
#endif

#define POOL_N    32
#define POOL_OPEN 128
typedef struct { FILE *f; char mode[4]; char path[MAX_PATH]; unsigned long long used; } pool_ent;
static pool_ent g_pool[POOL_N], g_pool_open[POOL_OPEN];
static unsigned long long g_pool_clock;
static SRWLOCK g_pool_lock = SRWLOCK_INIT;
static int g_pool_on = -1;

static int pool_eligible(const char *path, const char *mode)
{
    if (g_pool_on < 0) {
        const char *e = gmv_getenv("GMV_FILE_POOL");
        g_pool_on = !e || !*e || *e != '0';
    }
    if (!g_pool_on || !path || !mode || mode[0] != 'r' || strchr(mode, '+') || strlen(mode) >= 4 ||
        strlen(path) >= MAX_PATH)
        return 0;
    const char *p = path[0] == '.' && (path[1] == '/' || path[1] == '\\') ? path + 2 : path;
#ifndef _WIN32
    if (path[0] == '.' && path[1] == '/')
        p = path + 1;
#endif
    return gmv_in_bundle(p);
}

static void fpos_rewound(FILE *f)
{
    if (!fast_ftell_on())
        return;
    AcquireSRWLockExclusive(&g_fpos_lock);
    __typeof__(&g_fpos[0]) s = fpos_find(f);
    if (s) {
        s->pos = 0;
        s->known = 1;
    }
    ReleaseSRWLockExclusive(&g_fpos_lock);
}

static FILE *pool_take(const char *path, const char *mode)
{
    FILE *f = NULL;
    AcquireSRWLockExclusive(&g_pool_lock);
    for (int i = 0; i < POOL_N; i++)
        if (g_pool[i].f && !strcmp(g_pool[i].path, path) && !strcmp(g_pool[i].mode, mode)) {
            f = g_pool[i].f;
            g_pool[i].f = NULL;
            break;
        }
    if (f)
        for (int i = 0; i < POOL_OPEN; i++)
            if (!g_pool_open[i].f) {
                g_pool_open[i].f = f;
                strcpy(g_pool_open[i].mode, mode);
                strcpy(g_pool_open[i].path, path);
                break;
            }
    ReleaseSRWLockExclusive(&g_pool_lock);
    if (f) {
        rewind(f);
        fpos_rewound(f);
#if GMV_DEV
        InterlockedIncrement(&fs_pool_hit);
#endif
    }
    return f;
}

static void pool_track(FILE *f, const char *path, const char *mode)
{
    AcquireSRWLockExclusive(&g_pool_lock);
    for (int i = 0; i < POOL_OPEN; i++)
        if (!g_pool_open[i].f) {
            g_pool_open[i].f = f;
            strcpy(g_pool_open[i].mode, mode);
            strcpy(g_pool_open[i].path, path);
            break;
        }
    ReleaseSRWLockExclusive(&g_pool_lock);
}

static int pool_park(FILE *f)
{
    FILE *out = NULL;
    int kept = 0;
    AcquireSRWLockExclusive(&g_pool_lock);
    for (int i = 0; i < POOL_OPEN; i++)
        if (g_pool_open[i].f == f) {
            int slot = -1;
            for (int k = 0; k < POOL_N; k++)
                if (!g_pool[k].f) {
                    slot = k;
                    break;
                }
            if (slot < 0) {
                slot = 0;
                for (int k = 1; k < POOL_N; k++)
                    if (g_pool[k].used < g_pool[slot].used)
                        slot = k;
                out = g_pool[slot].f;
            }
            g_pool[slot] = g_pool_open[i];
            g_pool[slot].used = ++g_pool_clock;
            g_pool_open[i].f = NULL;
            kept = 1;
            break;
        }
    ReleaseSRWLockExclusive(&g_pool_lock);
    if (out) {
        if (fast_ftell_on())
            fpos_remove(out);
        fclose(out);
    }
    return kept;
}

static FILE *shim_fopen(const char *path, const char *mode)
{
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    long long ts = GMV_DEV ? gmv_qpc() : 0;
    int pooled = pool_eligible(path, mode);
    if (pooled) {
        FILE *pf = pool_take(path, mode);
        if (pf) {
            fs_note_open(path, mode, 1, ts);
            if (t0)
                gmv_prof_add(GMV_PROF_IO, t0, 0);
            InterlockedIncrement(&g_files_open);
            return pf;
        }
    }
    FILE *f = shim_fopen_inner(path, mode);
    if (f && pooled)
        pool_track(f, path, mode);
    fs_note_open(path, mode, f != NULL, ts);
    (void)ts;
    if (t0)
        gmv_prof_add(GMV_PROF_IO, t0, 0);
    if (f)
        InterlockedIncrement(&g_files_open);
#ifndef _WIN32
    static int big = -1;
    if (big < 0) {
        const char *e = gmv_getenv("GMV_FAST_FSEEK");
        big = gmv_env_flag(e, 1);
    }
    if (f && big && mode && mode[0] == 'r' && !strchr(mode, '+'))
        setvbuf(f, NULL, _IOFBF, FREAD_BUF);
#endif
    if (f && mode && mode[0] == 'r' && !strchr(mode, '+') && fast_ftell_on()) {
        int bin = strchr(mode, 'b') != NULL;
        if (bin && fast_fseek_on())
            setvbuf(f, NULL, _IOFBF, FREAD_BUF);
        fpos_add(f, bin);
    }
    return f;
}

static int shim_fclose(FILE *f)
{
#if GMV_DEV
    if (gmv_perf_on())
        InterlockedIncrement(&fs_close);
#endif
    if (pool_park(f)) {
        InterlockedDecrement(&g_files_open);
        return 0;
    }
    if (fast_ftell_on())
        fpos_remove(f);
    InterlockedDecrement(&g_files_open);
    return fclose(f);
}

static size_t shim_fread(void *buf, size_t size, size_t n, FILE *f)
{
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    size_t got = fread(buf, size, n, f);
    if (fast_ftell_on()) {
        AcquireSRWLockExclusive(&g_fpos_lock);
        __typeof__(&g_fpos[0]) s = fpos_find(f);
        if (s && s->known) {
            if (got == n)
                s->pos += (long)(got * size);
            else
                s->known = 0;
        }
        ReleaseSRWLockExclusive(&g_fpos_lock);
    }
    if (t0)
        gmv_prof_add(GMV_PROF_IO, t0, (long long)got * size);
    return got;
}

static int shim_fseek(FILE *f, long off, int whence)
{
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    if (fast_fseek_on() && (whence == SEEK_SET || whence == SEEK_CUR)) {
        long pos = 0, to = -1;
        AcquireSRWLockShared(&g_fpos_lock);
        __typeof__(&g_fpos[0]) s = fpos_find(f);
        if (s && s->known && s->bin) {
            pos = s->pos;
            to = whence == SEEK_SET ? off : pos + off;
        }
        ReleaseSRWLockShared(&g_fpos_lock);
        if (to >= 0) {
            int r = seek_in_buffer(f, pos, to) ? 0 : fseek(f, to, SEEK_SET);
            AcquireSRWLockExclusive(&g_fpos_lock);
            s = fpos_find(f);
            if (s) {
                s->pos = to;
                s->known = r == 0;
            }
            ReleaseSRWLockExclusive(&g_fpos_lock);
            if (t0)
                gmv_prof_add(GMV_PROF_IO, t0, 0);
            return r;
        }
    }
    int r = fseek(f, off, whence);
    if (fast_ftell_on()) {
        AcquireSRWLockExclusive(&g_fpos_lock);
        __typeof__(&g_fpos[0]) s = fpos_find(f);
        if (s) {
            if (r == 0 && whence == SEEK_SET) {
                s->pos = off;
                s->known = 1;
            } else if (r == 0 && whence == SEEK_CUR && s->known) {
                s->pos += off;
            } else {
                s->known = 0;
            }
        }
        ReleaseSRWLockExclusive(&g_fpos_lock);
    }
    if (t0)
        gmv_prof_add(GMV_PROF_IO, t0, 0);
    return r;
}

static long shim_ftell(FILE *f)
{
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    long r = -1;
    int cached = 0;
    if (fast_ftell_on()) {
        AcquireSRWLockShared(&g_fpos_lock);
        __typeof__(&g_fpos[0]) s = fpos_find(f);
        if (s && s->known) {
            r = s->pos;
            cached = 1;
        }
        ReleaseSRWLockShared(&g_fpos_lock);
    }
    if (!cached) {
        r = ftell(f);
        if (r >= 0 && fast_ftell_on()) {
            AcquireSRWLockExclusive(&g_fpos_lock);
            __typeof__(&g_fpos[0]) s = fpos_find(f);
            if (s) {
                s->pos = r;
                s->known = 1;
            }
            ReleaseSRWLockExclusive(&g_fpos_lock);
        }
    }
    if (t0)
        gmv_prof_add(GMV_PROF_IO, t0, 0);
    return r;
}

#define MISS_SLOTS 4096
static struct { uint32_t h; char *path; } g_miss[MISS_SLOTS];
static unsigned g_miss_used;
static SRWLOCK g_miss_lock = SRWLOCK_INIT;
static int g_miss_on = -1;

static uint32_t miss_hash(const char *s)
{
    uint32_t h = gmv_fnv1a(s);
    return h ? h : 1;
}

static int miss_on(void)
{
    if (g_miss_on < 0) {
        const char *e = gmv_getenv("GMV_FILE_MISS_CACHE");
        g_miss_on = !e || !*e || *e != '0';
    }
    return g_miss_on;
}

static int miss_known(const char *path)
{
    uint32_t h = miss_hash(path);
    int found = 0;
    AcquireSRWLockShared(&g_miss_lock);
    for (unsigned i = h & (MISS_SLOTS - 1); g_miss[i].h; i = (i + 1) & (MISS_SLOTS - 1))
        if (g_miss[i].h == h && !strcmp(g_miss[i].path, path)) {
            found = 1;
            break;
        }
    ReleaseSRWLockShared(&g_miss_lock);
    return found;
}

static void miss_add(const char *path)
{
    uint32_t h = miss_hash(path);
    AcquireSRWLockExclusive(&g_miss_lock);
    if (g_miss_used < MISS_SLOTS / 2) {
        unsigned i = h & (MISS_SLOTS - 1);
        for (; g_miss[i].h; i = (i + 1) & (MISS_SLOTS - 1))
            if (g_miss[i].h == h && !strcmp(g_miss[i].path, path))
                break;
        if (!g_miss[i].h && (g_miss[i].path = _strdup(path)) != NULL) {
            g_miss[i].h = h;
            g_miss_used++;
        }
    }
    ReleaseSRWLockExclusive(&g_miss_lock);
}

static void miss_clear(void)
{
    AcquireSRWLockExclusive(&g_miss_lock);
    for (unsigned i = 0; i < MISS_SLOTS; i++) {
        free(g_miss[i].path);
        g_miss[i].path = NULL;
        g_miss[i].h = 0;
    }
    g_miss_used = 0;
    ReleaseSRWLockExclusive(&g_miss_lock);
}

static FILE *shim_fopen_inner(const char *path, const char *mode)
{
    char redir[MAX_PATH * 2];
    if (savegame_redirect(path, redir, sizeof redir))
        path = redir;
    if (path && path[0] == '.' && (path[1] == '/' || path[1] == '\\') &&
        ((path[2] | 32) >= 'a' && (path[2] | 32) <= 'z') && path[3] == ':' &&
        (path[4] == '\\' || path[4] == '/'))
        path += 2;
#ifndef _WIN32
    else if (path && path[0] == '.' && path[1] == '/' && gmv_in_bundle(path + 1))
        path += 1;
#endif
    char ucbuf[MAX_PATH];
    const char *uc = gmv_game_redirect(path, ucbuf, sizeof ucbuf);
    if (uc)
        path = uc;
    int ro = mode && mode[0] == 'r' && !strchr(mode, '+');
    int cacheable = path && miss_on() && gmv_in_bundle(path);
    if (cacheable && !ro)
        miss_clear();
    FILE *f = NULL;
    int known = cacheable && ro && miss_known(path);
    if (known) {
        errno = ENOENT;
#if GMV_DEV
        InterlockedIncrement(&fs_miss_hit);
#endif
    } else {
        f = fopen(path, mode);
    }
    if (!f && path && !known) {
        for (const char *p = path; p[0] && p[1] && p[2]; p++) {
            char c = p[0];
            if (((c | 32) >= 'a' && (c | 32) <= 'z') && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) {
                if (p != path)
                    f = fopen(p, mode);
                break;
            }
        }
    }
    if (!f && path) {
        g_fopen_errno = errno;
        lstrcpynA(g_fopen_fail, path, sizeof g_fopen_fail);
        if (cacheable && ro && !known && errno == ENOENT)
            miss_add(path);
    }
    wavmap_note(path, f != NULL);
    if (!GMV_DEV)
        return f;
    static int trace = -1;
    if (trace < 0) {
        const char *e = gmv_dbgenv("GMV_FOPEN_TRACE");
        trace = (gmv_env_on(e)) ? (*e == '2' ? 2 : 1) : 0;
    }
    if (trace == 1)
        gmv_log("[fopen] %s \"%s\" (%s)\n", f ? "ok  " : "FAIL", path ? path : "(null)",
                mode ? mode : "?");
    else if (trace == 2)
        fmiss_note(path, f != NULL);
    return f;
}

static int shim_pread_inner(int fd, void *buf, size_t count, long long offset);

static int shim_pread(int fd, void *buf, size_t count, long long offset)
{
    long long t0 = gmv_prof_on() ? gmv_prof_now() : 0;
    int n = shim_pread_inner(fd, buf, count, offset);
    if (t0)
        gmv_prof_add(GMV_PROF_IO, t0, n > 0 ? n : 0);
    return n;
}

static SRWLOCK g_pread_lock = SRWLOCK_INIT;

static int shim_pread_inner(int fd, void *buf, size_t count, long long offset)
{
#ifndef _WIN32
    int n = (int)pread64(fd, buf, count, offset);
    (void)g_pread_lock;
#else
    AcquireSRWLockExclusive(&g_pread_lock);
    long long cur = _lseeki64(fd, 0, SEEK_CUR);
    if (cur < 0 || _lseeki64(fd, offset, SEEK_SET) < 0) {
        ReleaseSRWLockExclusive(&g_pread_lock);
        return -1;
    }
    int n = _read(fd, buf, (unsigned)count);
    _lseeki64(fd, cur, SEEK_SET);
    ReleaseSRWLockExclusive(&g_pread_lock);
#endif
    if (!GMV_DEV)
        return n;
    static int trace = -1;
    if (trace < 0) {
        const char *e = gmv_dbgenv("GMV_FOPEN_TRACE");
        trace = gmv_env_on(e);
    }
    if (trace && n != (int)count)
        gmv_log("[pread] fd=%d off=%lld want=%u got=%d\n", fd, offset, (unsigned)count, n);
    return n;
}

static int shim_stat_counted(const char *path, void *buf)
{
    long long ts = GMV_DEV ? gmv_qpc() : 0;
    int r = shim_stat(path, buf);
    fs_note_stat(path, r == 0, ts);
    (void)ts;
    return r;
}

const host_symbol gmv_host_symbols_win32[] = {
    FN("_pthread_mutex_init", shim_mutex_init),
    FN("_pthread_mutex_lock", shim_mutex_lock),
    FN("_pthread_mutex_unlock", shim_mutex_unlock),
    FN("_pthread_mutex_trylock", shim_mutex_trylock),
    FN("_pthread_mutex_destroy", shim_mutex_destroy),
    FN("_pthread_mutexattr_init", shim_mutexattr_noop),
    FN("_pthread_mutexattr_destroy$UNIX2003", shim_mutexattr_noop),
    FN("_pthread_mutexattr_settype", shim_mutexattr_settype),

    FN("_pthread_cond_init$UNIX2003", shim_cond_init),
    FN("_pthread_cond_destroy", shim_cond_destroy),
    FN("_pthread_cond_signal", shim_cond_signal),
    FN("_pthread_cond_broadcast", shim_cond_broadcast),
    FN("_pthread_cond_wait$UNIX2003", shim_cond_wait),
    FN("_pthread_cond_timedwait$UNIX2003", shim_cond_timedwait),

    FN("_pthread_create", shim_pthread_create),
    FN("_pthread_join$UNIX2003", shim_pthread_join),
    FN("_pthread_self", shim_pthread_self),
    FN("_pthread_equal", shim_pthread_equal),
    FN("_pthread_getschedparam", shim_pthread_getschedparam),
    FN("_pthread_key_create", shim_key_create),
    FN("_pthread_key_delete", shim_key_delete),
    FN("_pthread_getspecific", shim_getspecific),
    FN("_pthread_setspecific", shim_setspecific),
    FN("_pthread_attr_init", shim_attr_noop),
    FN("_pthread_attr_destroy", shim_attr_noop),
    FN("_pthread_attr_setdetachstate", shim_attr_set1),
    FN("_pthread_attr_setstacksize", shim_attr_set1),

    FN("_mmap$UNIX2003", shim_mmap),
    FN("_munmap$UNIX2003", shim_munmap),

    FN("_sched_yield", shim_sched_yield),
    FN("_nanosleep$UNIX2003", shim_nanosleep),
    FN("_usleep$UNIX2003", shim_usleep),
    FN("_sysconf", shim_sysconf),
    FN("_sysctl", shim_sysctl),
    FN("_sysctlbyname", shim_sysctlbyname),
    FN("_UpdateSystemActivity", shim_UpdateSystemActivity),
    FN("_chdir", shim_chdir),
    FN("_mkdir", shim_mkdir),
    FN("_stat$INODE64", shim_stat_counted),
    FN("_lstat$INODE64", shim_stat_counted),
    FN("__DefaultRuneLocale", &g_rune),
    FN("_wcslen", shim_wcslen),
    FN("_wcscmp", shim_wcscmp),
    FN("_wcscpy", shim_wcscpy),
    FN("_wmemcpy", shim_wmemcpy),
    FN("_wmemmove", shim_wmemmove),
    FN("_wmemchr", shim_wmemchr),
    FN("_wmemcmp", shim_wmemcmp),
    FN("_swprintf", shim_swprintf),
    FN("_opendir$INODE64$UNIX2003", shim_opendir),
    FN("_readdir$INODE64", shim_readdir),
    FN("_closedir$UNIX2003", shim_closedir),
    FN("_TISCopyCurrentKeyboardLayoutInputSource", shim_ret_null),
    FN("_TISGetInputSourceProperty", gmv_tis_input_source_property),
    FN("_UCKeyTranslate", shim_ret0),
    FN("_KBGetLayoutType", shim_ret0),
    FN("_LMGetKbdType", shim_ret0),
    FN("_kTISPropertyUnicodeKeyLayoutData", &g_tis_keylayout_key),
    FN("_kTISPropertyInputSourceID", &g_tis_keylayout_key),
    FN("_setjmp", gmv_setjmp),
    FN("__setjmp", gmv_setjmp),
    FN("_longjmp", gmv_longjmp),
    FN("_mach_absolute_time", shim_mach_absolute_time),
    FN("_mach_timebase_info", shim_mach_timebase_info),

    FN("_time", shim_time),
    FN("_gettimeofday", gmv_gettimeofday),
    FN("_gmtime", shim_gmtime),
    FN("_localtime", shim_localtime),

    FN("_fopen$UNIX2003", shim_fopen),
    FN("_fopen", shim_fopen),
    FN("_fclose", shim_fclose),
    FN("_fread", shim_fread),
    FN("_fwrite$UNIX2003", fwrite),
    FN("_fseek", shim_fseek),
    FN("_ftell", shim_ftell),
    FN("_feof", feof),
    FN("_fflush", fflush),
    FN("_fileno", _fileno),
    FN("_pread$UNIX2003", shim_pread),
    FN("_pread", shim_pread),
    FN("_clearerr", clearerr),
    FN("_remove", remove),
    FN("_fprintf", fprintf),
    FN("_fputs$UNIX2003", fputs),
    FN("_printf", printf),
    FN("_vprintf", vprintf),
    FN("_puts", puts),
    FN("_putchar", putchar),

    FN("___stdoutp", &shim_stdoutp),
    FN("___stderrp", &shim_stderrp),

    {NULL, NULL},
};
