#if defined _WIN32 && !defined _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "layout.h"
#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

extern char *gmv_std_string_new(const char *s, uint32_t len);

#define MAX_STREAM 64
/* ios_base base+0x68 */
#define WIN_LO 208
#define WIN_HI 320

static struct stream {
    uint32_t lo, hi, center;
    char *buf;
    size_t len, cap;
} g_stream[MAX_STREAM];

static SRWLOCK g_stream_lock = SRWLOCK_INIT;
#define LOCK()   AcquireSRWLockExclusive(&g_stream_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&g_stream_lock)

static struct stream *stream_find(const void *p)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    struct stream *best = NULL;
    uint32_t bestd = 0;
    for (int i = 0; i < MAX_STREAM; i++)
        if (g_stream[i].buf && a >= g_stream[i].lo && a < g_stream[i].hi) {
            uint32_t d = a > g_stream[i].center ? a - g_stream[i].center : g_stream[i].center - a;
            if (!best || d < bestd) {
                best = &g_stream[i];
                bestd = d;
            }
        }
    return best;
}

static struct stream *stream_create(const void *anchor)
{
    uint32_t t = (uint32_t)(uintptr_t)anchor;
    for (int i = 0; i < MAX_STREAM; i++) {
        if (!g_stream[i].buf) {
            g_stream[i].lo = t - WIN_LO;
            g_stream[i].hi = t + WIN_HI;
            g_stream[i].center = t;
            g_stream[i].cap = 128;
            g_stream[i].len = 0;
            g_stream[i].buf = malloc(g_stream[i].cap);
            if (g_stream[i].buf)
                g_stream[i].buf[0] = 0;
            return &g_stream[i];
        }
    }
    static int warned;
    if (!warned) {
        warned = 1;
        gmv_log("[stream] all %d stream slots in use; text written to a new stream is lost\n", MAX_STREAM);
    }
    return NULL;
}

static void stream_register(const void *ios_base)
{
    struct stream *s = stream_find(ios_base);
    if (s) {
        s->len = 0;
        s->center = (uint32_t)(uintptr_t)ios_base;
        if (s->buf)
            s->buf[0] = 0;
        return;
    }
    stream_create(ios_base);
}

static void stream_free(const void *p)
{
    struct stream *s = stream_find(p);
    if (s) {
        free(s->buf);
        s->buf = NULL;
    }
}

/* 0x63d3ec str() */
#define GSB_VTABLE_SLOT GMV_STRINGBUF_VTABLE_SLOT

static void gsb_sync(const void *p, const struct stream *s)
{
    uint32_t vt = GSB_VTABLE_SLOT ? *(const uint32_t *)(uintptr_t)GSB_VTABLE_SLOT : 0;
    if (!vt)
        return;
    vt += 8;
    uint32_t *q = (uint32_t *)(uintptr_t)p, *sb = NULL;
    if (q[0] == vt)
        sb = q;
    else if (q[1] == vt)
        sb = q + 1;
    if (!sb)
        return;
    uint32_t b = (uint32_t)(uintptr_t)s->buf, e = b + (uint32_t)s->len;
    sb[1] = b; sb[2] = b; sb[3] = e;
    sb[4] = b; sb[5] = e; sb[6] = e;
}

static void stream_append(const void *p, const char *d, size_t n)
{
    struct stream *s = stream_find(p);
    if (!s)
        s = stream_create(p);
    if (!s || !d || !n)
        return;
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap * 2;
        while (nc < s->len + n + 1)
            nc *= 2;
        char *nb = realloc(s->buf, nc);
        if (!nb)
            return;
        s->buf = nb;
        s->cap = nc;
    }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    s->buf[s->len] = 0;
    gsb_sync(p, s);
}

static void append_locked(const void *p, const char *d, size_t n) { LOCK(); stream_append(p, d, n); UNLOCK(); }

static void ios_base_ctor(void *self) { LOCK(); stream_register(self); UNLOCK(); }
static void stream_dtor(void *self)   { LOCK(); stream_free(self); UNLOCK(); }
static void stream_noop(void *self)   { (void)self; }

static void *ls_int(void *os, int v)        { char t[32]; int n = snprintf(t, sizeof t, "%d", v); append_locked(os, t, n); return os; }
static void *ls_ulong(void *os, uint32_t v) { char t[32]; int n = snprintf(t, sizeof t, "%u", v); append_locked(os, t, n); return os; }
static void *ls_float(void *os, float v)    { char t[48]; int n = snprintf(t, sizeof t, "%g", (double)v); append_locked(os, t, n); return os; }
static void *ls_cstr(void *os, const char *s) { if (s) append_locked(os, s, strlen(s)); return os; }
static void *ls_char(void *os, int c)       { char ch = (char)c; append_locked(os, &ch, 1); return os; }
static void *ls_stdstr(void *os, void *str) { const char *d = str ? *(const char *const *)str : NULL; if (d) append_locked(os, d, *(const uint32_t *)(d - 12)); return os; }
static void *ostream_insert(void *os, const char *s, int n) { if (s && n > 0) append_locked(os, s, (size_t)n); return os; }

extern void gmv_oss_str(void);

char *oss_str_impl(void *self)
{
    LOCK();
    struct stream *s = stream_find(self);
    char *r = gmv_std_string_new(s ? s->buf : "", s ? (uint32_t)s->len : 0);
    UNLOCK();
    return r;
}

static char *sb_at(void *self, int end)
{
    LOCK();
    struct stream *s = stream_find(self);
    char *r = s ? s->buf + (end ? s->len : 0) : NULL;
    UNLOCK();
    return r;
}
static char *sb_pbase(void *self) { return sb_at(self, 0); }
static char *sb_pptr(void *self)  { return sb_at(self, 1); }
static char *sb_egptr(void *self) { return sb_at(self, 1); }
static char *sb_eback(void *self) { return sb_at(self, 0); }
static char *sb_gptr(void *self)  { return sb_at(self, 0); }

static void *g_vt_streambuf[16];
static void *g_vt_stringbuf[16];
static void *g_vt_ostringstream[16];
static void *g_vt_basic_ios[16];
static void *g_tt_ostringstream[16];
static void *g_vt_class_type_info[8];
static void *g_vt_si_class_type_info[8];

const host_symbol gmv_host_symbols_stream[] = {
    FN("__ZNSt8ios_baseC2Ev", ios_base_ctor),
    FN("__ZNSt8ios_baseD2Ev", stream_dtor),
    FN("__ZNSt9basic_iosIcSt11char_traitsIcEEC2Ev", stream_noop),
    FN("__ZNSt9basic_iosIcSt11char_traitsIcEED2Ev", stream_noop),
    FN("__ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E", stream_noop),
    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEEC2Ev", stream_noop),
    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEED2Ev", stream_noop),
    FN("__ZNSt15basic_stringbufIcSt11char_traitsIcESaIcEE7_M_syncEPcmm", stream_noop),
    FN("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", stream_noop),
    FN("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", stream_dtor),
    FN("__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", stream_noop),
    FN("__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev", stream_dtor),
    FN("__ZNSt6localeC1Ev", stream_noop),
    FN("__ZNSt6localeD1Ev", stream_noop),
    FN("__ZNSdD0Ev", stream_dtor),
    FN("__ZNSdD1Ev", stream_dtor),
    FN("__ZNSdD2Ev", stream_dtor),
    FN("__ZNSiD0Ev", stream_dtor),
    FN("__ZNSiD1Ev", stream_dtor),
    FN("__ZNSoD0Ev", stream_dtor),
    FN("__ZNSoD1Ev", stream_dtor),
    FN("__ZThn8_NSdD0Ev", stream_dtor),
    FN("__ZThn8_NSdD1Ev", stream_dtor),
    FN("__ZTv0_n12_NSdD0Ev", stream_dtor),
    FN("__ZTv0_n12_NSdD1Ev", stream_dtor),
    FN("__ZTv0_n12_NSiD0Ev", stream_dtor),
    FN("__ZTv0_n12_NSiD1Ev", stream_dtor),
    FN("__ZTv0_n12_NSoD0Ev", stream_dtor),
    FN("__ZTv0_n12_NSoD1Ev", stream_dtor),
    FN("__ZNSdC2Ev", stream_noop),

    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEE4syncEv", stream_noop),
    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEE5imbueERKSt6locale", stream_noop),
    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv", stream_noop),
    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", stream_noop),
    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", ostream_insert),
    FN("__ZNSt15basic_streambufIcSt11char_traitsIcEE4setgEPcS3_S3_", stream_noop),

    FN("__ZNKSt15basic_streambufIcSt11char_traitsIcEE5pbaseEv", sb_pbase),
    FN("__ZNKSt15basic_streambufIcSt11char_traitsIcEE4pptrEv", sb_pptr),
    FN("__ZNKSt15basic_streambufIcSt11char_traitsIcEE5egptrEv", sb_egptr),
    FN("__ZNKSt15basic_streambufIcSt11char_traitsIcEE5ebackEv", sb_eback),
    FN("__ZNKSt15basic_streambufIcSt11char_traitsIcEE4gptrEv", sb_gptr),

    FN("__ZNSolsEi", ls_int),
    FN("__ZNSolsEf", ls_float),
    FN("__ZNSo9_M_insertImEERSoT_", ls_ulong),
    FN("__ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i", ostream_insert),
    FN("__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc", ls_cstr),
    FN("__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c", ls_char),
    FN("__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E", ls_stdstr),

    FN("__ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv", gmv_oss_str),
    FN("__ZNKSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv", gmv_oss_str),

    FN("__ZTVSt15basic_streambufIcSt11char_traitsIcEE", g_vt_streambuf),
    FN("__ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE", g_vt_stringbuf),
    FN("__ZTVSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE", g_vt_ostringstream),
    FN("__ZTVSt9basic_iosIcSt11char_traitsIcEE", g_vt_basic_ios),
    FN("__ZTTSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE", g_tt_ostringstream),
    FN("__ZTVN10__cxxabiv117__class_type_infoE", g_vt_class_type_info),
    FN("__ZTVN10__cxxabiv120__si_class_type_infoE", g_vt_si_class_type_info),

    {NULL, NULL},
};
