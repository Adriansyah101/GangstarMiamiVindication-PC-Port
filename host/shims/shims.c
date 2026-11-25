#include <ctype.h>
#include <emmintrin.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "log.h"
#include "shims.h"

static void shim_fatal(const char *why)
{
    gmv_log("\n[shim] %s -- stopping\n", why);
    gmv_fatal(4, "The game stopped on an internal error (%s).", why);
}

static void shim_abort(void)            { shim_fatal("abort() called by game"); }
static void shim_stack_chk_fail(void)   { shim_fatal("stack smashing detected (__stack_chk_fail)"); }

static void shim_assert_rtn(const char *func, const char *file, int line, const char *expr)
{
    gmv_log("\n[shim] Assertion failed: (%s), function %s, file %s, line %d.\n",
            expr ? expr : "?", func ? func : "?", file ? file : "?", line);
    shim_fatal("assert failed");
}

/* macOS rand() RAND_MAX 0x7fffffff 0x60f7f0 */
static unsigned long g_rand_next = 1;

static int shim_rand(void)
{
    unsigned long ctx = g_rand_next;
    if (ctx == 0)
        ctx = 123459876;
    long hi = (long)(ctx / 127773), lo = (long)(ctx % 127773);
    long x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7fffffff;
    g_rand_next = (unsigned long)x;
    return (int)((unsigned long)x % 0x80000000ul);
}

static const uintptr_t shim_stack_chk_guard = (uintptr_t)0x1234AB00u;

static void *shim_memcpy_chk(void *d, const void *s, size_t n, size_t dstlen)
{
    (void)dstlen;
    return memcpy(d, s, n);
}

static char *shim_strcpy_chk(char *d, const char *s, size_t dstlen)
{
    (void)dstlen;
    return strcpy(d, s);
}

static int shim_sprintf_chk(char *s, int flag, size_t dstlen, const char *fmt, ...)
{
    (void)flag;
    (void)dstlen;
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(s, fmt, ap);
    va_end(ap);
    return r;
}

static int g_fast_memmove = -1;

__attribute__((target("sse2"), optimize("no-tree-loop-distribute-patterns")))
static void *shim_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d - (uintptr_t)s >= n)
        return memmove(dst, src, n);
    if (g_fast_memmove < 0) {
        const char *e = gmv_getenv("GMV_FAST_MEMMOVE");
        g_fast_memmove = gmv_env_flag(e, 1);
    }
    if (!g_fast_memmove)
        return memmove(dst, src, n);
    if (n < 16) {
        while (n--)
            d[n] = s[n];
        return dst;
    }
    __m128i head = _mm_loadu_si128((const __m128i *)s);
    size_t p = n;
    while (p > 64) {
        p -= 64;
        __m128i a = _mm_loadu_si128((const __m128i *)(s + p + 48));
        __m128i b = _mm_loadu_si128((const __m128i *)(s + p + 32));
        __m128i c = _mm_loadu_si128((const __m128i *)(s + p + 16));
        __m128i e = _mm_loadu_si128((const __m128i *)(s + p));
        _mm_storeu_si128((__m128i *)(d + p + 48), a);
        _mm_storeu_si128((__m128i *)(d + p + 32), b);
        _mm_storeu_si128((__m128i *)(d + p + 16), c);
        _mm_storeu_si128((__m128i *)(d + p), e);
    }
    while (p > 16) {
        p -= 16;
        _mm_storeu_si128((__m128i *)(d + p), _mm_loadu_si128((const __m128i *)(s + p)));
    }
    _mm_storeu_si128((__m128i *)d, head);
    return dst;
}

static void shim_bzero(void *p, size_t n) { memset(p, 0, n); }
extern long long __divdi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

const host_symbol gmv_host_symbols[] = {
    FN("_malloc", malloc),
    FN("_free", free),
    FN("_calloc", calloc),
    FN("_realloc", realloc),
    FN("_malloc_size", _msize),
    FN("___bzero", shim_bzero),

    FN("_memchr", memchr),
    FN("_memcmp", memcmp),
    FN("_memcpy", memcpy),
    FN("_memmove", shim_memmove),
    FN("_memset", memset),
    FN("___memcpy_chk", shim_memcpy_chk),

    FN("_strcasecmp", _stricmp),
    FN("_strncasecmp", _strnicmp),
    FN("_strcat", strcat),
    FN("_strchr", strchr),
    FN("_strcmp", strcmp),
    FN("_strcpy", strcpy),
    FN("_strcspn", strcspn),
    FN("_strdup", _strdup),
    FN("_strlen", strlen),
    FN("_strncmp", strncmp),
    FN("_strncpy", strncpy),
    FN("_strrchr", strrchr),
    FN("_strstr", strstr),
    FN("_strtol", strtol),
    FN("_strtod$UNIX2003", strtod),
    FN("___strcpy_chk", shim_strcpy_chk),

    FN("_sprintf", sprintf),
    FN("_snprintf", snprintf),
    FN("_vsprintf", vsprintf),
    FN("_vsnprintf", vsnprintf),
    FN("_sscanf", sscanf),
    FN("___sprintf_chk", shim_sprintf_chk),

    FN("_atoi", atoi),
    FN("_atol", atol),
    FN("_atof", atof),
    FN("_qsort", qsort),
    FN("_rand", shim_rand),
    FN("_getenv", getenv),
    FN("___tolower", tolower),
    FN("___toupper", toupper),

    FN("___divdi3", __divdi3),
    FN("___udivdi3", __udivdi3),
    FN("___moddi3", __moddi3),
    FN("___umoddi3", __umoddi3),

    FN("_acos", acos),   FN("_acosf", acosf),
    FN("_asin", asin),
    FN("_atan", atan),   FN("_atan2", atan2),
    FN("_ceil", ceil),   FN("_ceilf", ceilf),
    FN("_cos", cos),     FN("_cosf", cosf),
    FN("_exp", exp),     FN("_expf", expf),
    FN("_floor", floor), FN("_floorf", floorf),
    FN("_fmax", fmax),   FN("_fmin", fmin),
    FN("_fmod", fmod),   FN("_fmodf", fmodf),
    FN("_ldexp", ldexp),
    FN("_log", log),     FN("_logf", logf),
    FN("_log10", log10), FN("_log2", log2),
    FN("_pow", pow),
    FN("_sin", sin),     FN("_sinf", sinf),
    FN("_sinh", sinh),
    FN("_tan", tan),
    FN("_lrint", lrint),

    FN("_abort", shim_abort),
    FN("_exit", exit),
    FN("___stack_chk_fail", shim_stack_chk_fail),
    FN("___assert_rtn", shim_assert_rtn),
    FN("___error", _errno),

    FN("___stack_chk_guard", &shim_stack_chk_guard),

    {NULL, NULL},
};

void *gmv_shim_lookup(const char *name)
{
    const host_symbol *tables[] = {gmv_host_symbols, gmv_host_symbols_win32, gmv_host_symbols_cxx,
                                   gmv_host_symbols_sdl, gmv_host_symbols_objc, gmv_host_symbols_appkit,
                                   gmv_host_symbols_str, gmv_host_symbols_cg, gmv_host_symbols_gl,
                                   gmv_host_symbols_stream, gmv_game_host_symbols};
    for (size_t t = 0; t < sizeof tables / sizeof *tables; t++)
        for (const host_symbol *h = tables[t]; h->name; h++)
            if (!strcmp(h->name, name))
                return h->addr;
    return gmv_gl_resolve(name);
}
