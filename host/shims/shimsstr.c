#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

typedef struct {
    uint32_t length;
    uint32_t capacity;
    int refcount;
} Rep;

#define REP_OF(data)   ((Rep *)((char *)(data) - sizeof(Rep)))
#define REP_DATA(rep)  ((char *)(rep) + sizeof(Rep))

static struct { Rep rep; char nul; } g_empty = {{0, 0, 0}, 0};

static char *empty_data(void) { return REP_DATA(&g_empty.rep); }

static char *mp(void *self)        { return *(char **)self; }
static void  set_mp(void *self, char *d) { *(char **)self = d; }

static char *rep_alloc(uint32_t len)
{
    Rep *r = malloc(sizeof(Rep) + len + 1);
    r->length = len;
    r->capacity = len;
    r->refcount = 0;
    char *d = REP_DATA(r);
    d[len] = 0;
    return d;
}

char *gmv_std_string_new(const char *s, uint32_t len)
{
    char *d = rep_alloc(len);
    if (s && len)
        memcpy(d, s, len);
    return d;
}

static void rep_release(char *data)
{
    Rep *r = REP_OF(data);
    if (r == &g_empty.rep)
        return;
    if (__sync_fetch_and_add(&r->refcount, -1) <= 0)
        free(r);
}

static void str_rep_destroy(void *rep, const void *a)
{
    (void)a;
    if (rep != (void *)&g_empty.rep)
        free(rep);
}

static void str_set(void *self, const char *src, uint32_t len)
{
    char *d = rep_alloc(len);
    if (src && len)
        memcpy(d, src, len);
    rep_release(mp(self));
    set_mp(self, d);
}

static void *str_ctor_cstr(void *self, const char *s, void *a)
{
    (void)a;
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    char *d = rep_alloc(len);
    if (len)
        memcpy(d, s, len);
    set_mp(self, d);
    return self;
}

static void *str_ctor_default(void *self)
{
    __sync_add_and_fetch(&g_empty.rep.refcount, 1);
    set_mp(self, empty_data());
    return self;
}

static void *str_ctor_copy(void *self, void *other)
{
    char *d = mp(other);
    __sync_add_and_fetch(&REP_OF(d)->refcount, 1);
    set_mp(self, d);
    return self;
}

static void str_dtor(void *self)
{
    rep_release(mp(self));
}

static const char *str_c_str(void *self) { return mp(self); }

static void str_clear(void *self)
{
    rep_release(mp(self));
    __sync_add_and_fetch(&g_empty.rep.refcount, 1);
    set_mp(self, empty_data());
}

static void *str_assign_cstr_n(void *self, const char *s, uint32_t n)
{
    str_set(self, s, n);
    return self;
}
static void *str_assign_str(void *self, void *other)
{
    char *d = mp(other);
    str_set(self, d, REP_OF(d)->length);
    return self;
}
static void *str_op_assign_cstr(void *self, const char *s)
{
    str_set(self, s, s ? (uint32_t)strlen(s) : 0);
    return self;
}
static void *str_op_assign_str(void *self, void *other)
{
    return str_assign_str(self, other);
}

static void str_append_bytes(void *self, const char *add, uint32_t addlen)
{
    char *cur = mp(self);
    uint32_t curlen = REP_OF(cur)->length;
    char *nd = rep_alloc(curlen + addlen);
    memcpy(nd, cur, curlen);
    if (addlen)
        memcpy(nd + curlen, add, addlen);
    rep_release(cur);
    set_mp(self, nd);
}
static void *str_append_cstr(void *self, const char *s)
{
    str_append_bytes(self, s, s ? (uint32_t)strlen(s) : 0);
    return self;
}
static void *str_append_str(void *self, void *other)
{
    char *d = mp(other);
    str_append_bytes(self, d, REP_OF(d)->length);
    return self;
}

static int cmp_bytes(const char *a, uint32_t alen, const char *b, uint32_t blen)
{
    uint32_t n = alen < blen ? alen : blen;
    int r = n ? memcmp(a, b, n) : 0;
    if (r)
        return r;
    return (alen > blen) - (alen < blen);
}
static int str_compare_cstr(void *self, const char *s)
{
    char *d = mp(self);
    return cmp_bytes(d, REP_OF(d)->length, s, s ? (uint32_t)strlen(s) : 0);
}
static int str_compare_str(void *self, void *other)
{
    char *a = mp(self), *b = mp(other);
    return cmp_bytes(a, REP_OF(a)->length, b, REP_OF(b)->length);
}

extern void gmv_throw_out_of_range(const char *what);
extern void gmv_throw_length_error(const char *what);
extern void gmv_throw_bad_alloc(void);

static int str_compare_sub(void *self, uint32_t pos1, uint32_t n1, void *other, uint32_t pos2, uint32_t n2)
{
    char *a = mp(self), *b = mp(other);
    uint32_t la = REP_OF(a)->length, lb = REP_OF(b)->length;
    if (pos1 > la || pos2 > lb)
        gmv_throw_out_of_range("basic_string::compare");
    if (n1 > la - pos1)
        n1 = la - pos1;
    if (n2 > lb - pos2)
        n2 = lb - pos2;
    uint32_t n = n1 < n2 ? n1 : n2;
    int r = n ? memcmp(a + pos1, b + pos2, n) : 0;
    return r ? r : (int)(n1 - n2);
}

static Rep *str_rep_create(uint32_t capacity, uint32_t old_capacity, const void *a)
{
    (void)a;
    const uint32_t max_size = (0xffffffffu - sizeof(Rep) - 1) / 4;
    if (capacity > max_size)
        gmv_throw_length_error("basic_string::_S_create");
    if (capacity > old_capacity && capacity < 2 * old_capacity)
        capacity = 2 * old_capacity;
    uint32_t size = capacity + 1 + sizeof(Rep);
    const uint32_t adj = size + 4 * sizeof(void *);
    if (adj > 4096 && capacity > old_capacity) {
        capacity += 4096 - adj % 4096;
        if (capacity > max_size)
            capacity = max_size;
        size = capacity + 1 + sizeof(Rep);
    }
    Rep *r = malloc(size);
    if (!r)
        gmv_throw_bad_alloc();
    r->capacity = capacity;
    r->refcount = 0;
    return r;
}

const host_symbol gmv_host_symbols_str[] = {
    FN("__ZNSs4_Rep20_S_empty_rep_storageE", &g_empty.rep),
    FN("__ZNSs4_Rep10_M_destroyERKSaIcE", str_rep_destroy),

    FN("__ZNSsC1EPKcRKSaIcE", str_ctor_cstr),
    FN("__ZNSsC1Ev", str_ctor_default),
    FN("__ZNSsC1ERKSs", str_ctor_copy),
    FN("__ZNSsD1Ev", str_dtor),

    FN("__ZNKSs5c_strEv", str_c_str),
    FN("__ZNKSs4dataEv", str_c_str),

    FN("__ZNSs5clearEv", str_clear),
    FN("__ZNSs6assignEPKcm", str_assign_cstr_n),
    FN("__ZNSs6assignERKSs", str_assign_str),
    FN("__ZNSsaSEPKc", str_op_assign_cstr),
    FN("__ZNSsaSERKSs", str_op_assign_str),
    FN("__ZNSs6appendEPKc", str_append_cstr),
    FN("__ZNSs6appendERKSs", str_append_str),
    FN("__ZNSspLEPKc", str_append_cstr),
    FN("__ZNKSs7compareEPKc", str_compare_cstr),
    FN("__ZNKSs7compareERKSs", str_compare_str),
    FN("__ZNKSs7compareEmmRKSsmm", str_compare_sub),
    FN("__ZNSs4_Rep9_S_createEmmRKSaIcE", str_rep_create),

    {NULL, NULL},
};
