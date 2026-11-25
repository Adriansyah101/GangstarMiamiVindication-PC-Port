#ifndef GMV_COMPAT_H
#define GMV_COMPAT_H

#include "platform.h"

#define GMV_DIRSEP (GMV_SEP[0])

#ifdef _WIN32
#include <windows.h>
#else

#include <dlfcn.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef unsigned long DWORD, ULONG;
typedef unsigned int UINT;
typedef long LONG;
typedef int64_t LONG64, LONGLONG;
typedef void *HANDLE, *HMODULE, *LPVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef union { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WINAPI
#define CALLBACK
#ifndef APIENTRY
#define APIENTRY
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_TIMEOUT 1460u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup

static __thread DWORD gmv_compat_last_error;
static inline DWORD GetLastError(void) { return gmv_compat_last_error; }

typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(cs); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) { return !pthread_mutex_trylock(cs); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(cs); }

typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void InitializeSRWLock(SRWLOCK *l) { pthread_mutex_init(l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_mutex_lock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_mutex_unlock(l); }

typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER
static inline void InitializeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_init(c, NULL); }
static inline void WakeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_signal(c); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_broadcast(c); }
static inline BOOL gmv_compat_cond_wait(CONDITION_VARIABLE *c, pthread_mutex_t *m, DWORD ms)
{
    if (ms == INFINITE) {
        pthread_cond_wait(c, m);
        return TRUE;
    }
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    if (pthread_cond_timedwait(c, m, &t) == ETIMEDOUT) {
        gmv_compat_last_error = ERROR_TIMEOUT;
        return FALSE;
    }
    return TRUE;
}
#define SleepConditionVariableCS(c, cs, ms) gmv_compat_cond_wait((c), (cs), (ms))
#define SleepConditionVariableSRW(c, l, ms, flags) ((void)(flags), gmv_compat_cond_wait((c), (l), (ms)))

#define InterlockedIncrement(p) __sync_add_and_fetch((p), 1)
#define InterlockedDecrement(p) __sync_sub_and_fetch((p), 1)
#define InterlockedExchangeAdd(p, v) __sync_fetch_and_add((p), (v))
#define InterlockedExchangeAdd64(p, v) __sync_fetch_and_add((p), (v))
#define InterlockedOr(p, v) __sync_fetch_and_or((p), (v))
#define InterlockedAnd(p, v) __sync_fetch_and_and((p), (v))
#define InterlockedCompareExchange(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedCompareExchangePointer(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangePointer(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define MemoryBarrier() __sync_synchronize()

static inline void Sleep(DWORD ms)
{
    if (!ms) {
        sched_yield();
        return;
    }
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}
static inline BOOL SwitchToThread(void) { return !sched_yield(); }
static inline uint64_t gmv_compat_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static inline DWORD GetTickCount(void) { return (DWORD)(gmv_compat_ns() / 1000000u); }
static inline uint64_t GetTickCount64(void) { return gmv_compat_ns() / 1000000u; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = (LONGLONG)gmv_compat_ns(); return TRUE; }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000; return TRUE; }
static inline DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }

#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
static inline DWORD TlsAlloc(void)
{
    pthread_key_t k;
    return pthread_key_create(&k, NULL) ? TLS_OUT_OF_INDEXES : (DWORD)k;
}
static inline BOOL TlsFree(DWORD k) { return !pthread_key_delete((pthread_key_t)k); }
static inline LPVOID TlsGetValue(DWORD k) { return pthread_getspecific((pthread_key_t)k); }
static inline BOOL TlsSetValue(DWORD k, LPVOID v) { return !pthread_setspecific((pthread_key_t)k, v); }

static inline DWORD GetFileAttributesA(const char *path)
{
    struct stat st;
    if (stat(path, &st))
        return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
static inline BOOL CreateDirectoryA(const char *path, void *security)
{
    (void)security;
    return !mkdir(path, 0755);
}
static inline BOOL DeleteFileA(const char *path) { return !unlink(path); }
static inline BOOL SetCurrentDirectoryA(const char *path) { return !chdir(path); }
static inline DWORD GetModuleFileNameA(HMODULE module, char *buf, DWORD n)
{
    (void)module;
    ssize_t got = readlink("/proc/self/exe", buf, n ? n - 1 : 0);
    if (got <= 0)
        return 0;
    buf[got] = 0;
    return (DWORD)got;
}

static inline char *lstrcpynA(char *d, const char *s, int n)
{
    if (n > 0) {
        size_t l = strnlen(s, (size_t)n - 1);
        memcpy(d, s, l);
        d[l] = 0;
    }
    return d;
}

#define _errno __errno_location
#define _msize malloc_usable_size
#define _fileno fileno

typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef struct { pthread_t t; int joined; DWORD code; } gmv_compat_thread;
typedef struct { LPTHREAD_START_ROUTINE fn; LPVOID arg; volatile DWORD tid; } gmv_compat_start;

static inline void *gmv_compat_thread_main(void *p)
{
    gmv_compat_start *s = p;
    LPTHREAD_START_ROUTINE fn = s->fn;
    LPVOID arg = s->arg;
    __atomic_store_n(&s->tid, GetCurrentThreadId(), __ATOMIC_RELEASE);
    return (void *)(uintptr_t)fn(arg);
}
static inline HANDLE CreateThread(void *security, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
                                  DWORD *tid)
{
    (void)security;
    (void)stack;
    (void)flags;
    gmv_compat_start s = {fn, arg, 0};
    gmv_compat_thread *h = calloc(1, sizeof *h);
    if (!h || pthread_create(&h->t, NULL, gmv_compat_thread_main, &s)) {
        free(h);
        return NULL;
    }
    while (!__atomic_load_n(&s.tid, __ATOMIC_ACQUIRE))
        sched_yield();
    if (tid)
        *tid = s.tid;
    return h;
}
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    gmv_compat_thread *t = h;
    (void)ms;
    if (!t->joined) {
        void *r = NULL;
        pthread_join(t->t, &r);
        t->code = (DWORD)(uintptr_t)r;
        t->joined = 1;
    }
    return 0;
}
static inline BOOL GetExitCodeThread(HANDLE h, DWORD *code)
{
    *code = ((gmv_compat_thread *)h)->code;
    return TRUE;
}
static inline BOOL CloseHandle(HANDLE h)
{
    gmv_compat_thread *t = h;
    if (!t->joined)
        pthread_detach(t->t);
    free(t);
    return TRUE;
}
static inline void ExitProcess(UINT code) { _exit((int)code); }

typedef struct { pthread_mutex_t m; int done; } INIT_ONCE, *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
static inline BOOL InitOnceExecuteOnce(INIT_ONCE *o, BOOL (*fn)(PINIT_ONCE, void *, void **), void *param, void **ctx)
{
    BOOL ok = TRUE;
    pthread_mutex_lock(&o->m);
    if (!o->done) {
        ok = fn(o, param, ctx);
        o->done = ok;
    }
    pthread_mutex_unlock(&o->m);
    return ok;
}

#define IsBadReadPtr(p, n) (!gmv_readable((p), (n)))
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_EXECUTE_WRITECOPY 0x80u
#define PAGE_GUARD 0x100u
typedef uintptr_t UINT_PTR;

typedef struct {
    void *BaseAddress, *AllocationBase;
    DWORD AllocationProtect;
    size_t RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION;
size_t VirtualQuery(const void *p, MEMORY_BASIC_INFORMATION *m, size_t n);
BOOL VirtualProtect(void *p, size_t n, DWORD prot, DWORD *old);
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static inline BOOL FlushInstructionCache(HANDLE process, const void *p, size_t n)
{
    (void)process;
    (void)p;
    (void)n;
    return TRUE;
}
#define YieldProcessor() __builtin_ia32_pause()
static inline int _getmaxstdio(void) { return (int)sysconf(_SC_OPEN_MAX); }
static inline LPVOID VirtualAlloc(LPVOID addr, size_t size, DWORD type, DWORD prot)
{
    (void)type;
    (void)prot;
    void *p = mmap(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
#include <SDL3/SDL_video.h>
#define wglGetCurrentContext() ((void *)SDL_GL_GetCurrentContext())

#endif

static inline long long gmv_qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
#endif
