#include <stdint.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "log.h"
#include "shims.h"
#include "glshim.h"

typedef struct {
    signed char known;
    signed char on;
    GLint size, type, stride, buf;
    const char *ptr;
} ga_t;
static ga_t ga[GA_N];
static GLint ga_abo = -1, ga_ebo = -1;
static int ga_live;
static void(APIENTRY *ga_bind_real)(GLenum, GLuint);
static void(APIENTRY *ga_ptr_real[4])(GLint, GLenum, GLsizei, const void *);
static void(APIENTRY *ga_nptr_real)(GLenum, GLsizei, const void *);
static void(APIENTRY *ga_del_real)(GLsizei, const GLuint *);
#if GMV_DEV
static unsigned ga_bad;
#endif

static const struct { GLenum en, buf, sz, ty, st, ptr; } ga_names[4] = {
    {GL_VERTEX_ARRAY, 0x8896, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE, GL_VERTEX_ARRAY_POINTER},
    {GL_NORMAL_ARRAY, 0x8897, 0, GL_NORMAL_ARRAY_TYPE, GL_NORMAL_ARRAY_STRIDE, GL_NORMAL_ARRAY_POINTER},
    {GL_COLOR_ARRAY, 0x8898, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE, GL_COLOR_ARRAY_POINTER},
    {GL_TEXTURE_COORD_ARRAY, 0x889A, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE,
     GL_TEXTURE_COORD_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY_POINTER},
};

static int ga_kind(int which)
{
    return which >= GA_TEX0 ? 3 : which;
}

static int ga_on_shadow(void)
{
    return ga_live && gmv_gl_query_free();
}

static void(APIENTRY *ga_client_real(void))(GLenum)
{
    static void(APIENTRY * real)(GLenum);
    if (!real) { real = gl_real("glClientActiveTexture"); if (!real) real = gl_real("glClientActiveTextureARB"); }
    return real;
}

static void ga_ask(int which, ga_t *a, int want_on)
{
    int k = ga_kind(which);
    unsigned unit = which >= GA_TEX0 ? (unsigned)(which - GA_TEX0) : 0;
    int sw = k == 3 && ga_on_shadow() && unit != g_tf_client;
    void(APIENTRY * client)(GLenum) = sw ? ga_client_real() : NULL;
    if (sw && !client) {
        memset(a, 0, sizeof *a);
        return;
    }
    if (client) client(0x84C0 + unit);
    GLint v = 0;
    void *p = NULL;
    a->size = 3;
    if (ga_names[k].sz) glGetIntegerv(ga_names[k].sz, &a->size);
    glGetIntegerv(ga_names[k].ty, &v), a->type = v;
    glGetIntegerv(ga_names[k].st, &v), a->stride = v;
    glGetIntegerv(ga_names[k].buf, &v), a->buf = v;
    glGetPointerv(ga_names[k].ptr, &p), a->ptr = p;
    a->known = 1;
    if (want_on)
        a->on = glIsEnabled(ga_names[k].en) ? 1 : 0;
    if (client) client(0x84C0 + g_tf_client);
}

#if GMV_DEV
static void ga_check(int which, const ga_t *s)
{
    if (!gmv_gl_query_check() || (gmv_sv_moved() & (1u << which)))
        return;
    ga_t g;
    ga_ask(which, &g, 1);
    if ((s->known && (g.size != s->size || g.type != s->type || g.stride != s->stride || g.buf != s->buf ||
                      g.ptr != s->ptr)) || (s->on >= 0 && g.on != s->on)) {
        if (ga_bad++ < 20)
            gmv_log("[gl] QUERY_CHECK: array %d kept size %d type 0x%x stride %d buffer %d %p on %d, GL says %d 0x%x %d %d "
                    "%p %d\n", which, s->size, s->type, s->stride, s->buf, (const void *)s->ptr, s->on, g.size, g.type,
                    g.stride, g.buf, (const void *)g.ptr, g.on);
    }
}
#endif

int gmv_gl_arr(int which, gmv_arr_t *out)
{
    if (which < 0 || which >= GA_N)
        return 0;
    ga_t *a = &ga[which];
    if (!ga_on_shadow()) {
        ga_t g;
        ga_ask(which, &g, 1);
        if (out) out->size = g.size, out->type = g.type, out->stride = g.stride, out->buf = g.buf, out->ptr = g.ptr;
        return g.on;
    }
    if (!a->known || a->on < 0) {
        ga_t g;
        ga_ask(which, &g, a->on < 0);
        if (!a->known) {
            signed char on = a->on;
            *a = g;
            a->on = on;
        }
        if (a->on < 0)
            a->on = g.on;
    }
#if GMV_DEV
    else
        ga_check(which, a);
#endif
    if (out) out->size = a->size, out->type = a->type, out->stride = a->stride, out->buf = a->buf, out->ptr = a->ptr;
    return a->on;
}

int gmv_gl_arr_on(int which)
{
    if (which < 0 || which >= GA_N)
        return 0;
    if (!ga_on_shadow() || ga[which].on < 0)
        return gmv_gl_arr(which, NULL);
    return ga[which].on;
}

static GLint ga_bound(GLint *kept, GLenum pname)
{
    if (!ga_on_shadow() || *kept < 0) {
        GLint v = 0;
        glGetIntegerv(pname, &v);
        if (ga_on_shadow())
            *kept = v;
        return v;
    }
#if GMV_DEV
    if (gmv_gl_query_check()) {
        GLint v = 0;
        glGetIntegerv(pname, &v);
        if (v != *kept && ga_bad++ < 20)
            gmv_log("[gl] QUERY_CHECK: 0x%x kept %d, GL says %d\n", pname, *kept, v);
    }
#endif
    return *kept;
}

GLint gmv_gl_buf(GLenum pname)
{
    gmv_arr_t a;
    switch (pname) {
    case 0x8894: return ga_bound(&ga_abo, pname);
    case 0x8895: return ga_bound(&ga_ebo, pname);
    case 0x8896: gmv_gl_arr(GA_VERTEX, &a); return a.buf;
    case 0x8897: gmv_gl_arr(GA_NORMAL, &a); return a.buf;
    case 0x8898: gmv_gl_arr(GA_COLOR, &a); return a.buf;
    case 0x889A:
        if (g_tf_client < 4) { gmv_arr_t t; gmv_gl_arr(GA_TEX0 + (int)g_tf_client, &t); return t.buf; }
        break;
    }
    GLint v = 0;
    glGetIntegerv(pname, &v);
    return v;
}

GLint gmv_gl_client_unit(void)
{
    if (ga_on_shadow())
        return (GLint)(0x84C0 + g_tf_client);
    GLint v = 0;
    glGetIntegerv(0x84E1, &v);
    return v;
}

void gmv_gl_arr_state(GLenum array, int on)
{
    int w = array == GL_VERTEX_ARRAY ? GA_VERTEX : array == GL_NORMAL_ARRAY ? GA_NORMAL
            : array == GL_COLOR_ARRAY ? GA_COLOR : array == GL_TEXTURE_COORD_ARRAY && g_tf_client < 4
            ? GA_TEX0 + (int)g_tf_client : -1;
    if (w >= 0)
        ga[w].on = on ? 1 : 0;
}

static void ga_note_bind(GLenum target, GLuint b)
{
    if (target == 0x8892) ga_abo = (GLint)b;
    else if (target == 0x8893) ga_ebo = (GLint)b;
}

static void ga_note_ptr(int which, GLint size, GLenum type, GLsizei stride, const void *p)
{
    if (which < 0 || which >= GA_N)
        return;
    ga_t *a = &ga[which];
    if (ga_abo < 0) {
        GLint v = 0;
        glGetIntegerv(0x8894, &v);
        ga_abo = v;
    }
    a->size = size, a->type = (GLint)type, a->stride = stride, a->buf = ga_abo, a->ptr = p;
    a->known = 1;
    gmv_sv_pointer_set(which);
}

static int ga_unit_which(void)
{
    return g_tf_client < 4 ? GA_TEX0 + (int)g_tf_client : -1;
}

void gmv_gl_bind_buffer(GLenum target, GLuint b)
{
    if (!ga_bind_real) ga_bind_real = gl_real("glBindBuffer");
    ga_note_bind(target, b);
    if (ga_bind_real) ga_bind_real(target, b);
}

void gmv_gl_arr_pointer(int which, GLint size, GLenum type, GLsizei stride, const void *p)
{
    int k = ga_kind(which);
    if (which >= GA_TEX0 && which != ga_unit_which())
        return;
    if (k == 1) {
        if (!ga_nptr_real) ga_nptr_real = gl_real("glNormalPointer");
        if (ga_nptr_real) ga_nptr_real(type, stride, p);
    } else {
        static const char *nm[4] = {"glVertexPointer", NULL, "glColorPointer", "glTexCoordPointer"};
        if (!ga_ptr_real[k]) ga_ptr_real[k] = gl_real(nm[k]);
        if (ga_ptr_real[k]) ga_ptr_real[k](size, type, stride, p);
    }
    ga_note_ptr(which, k == 1 ? 3 : size, type, stride, p);
}

void gmv_gl_buffers_deleted(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; names && i < n; i++) {
        GLint b = (GLint)names[i];
        if (!b)
            continue;
        if (ga_abo == b) ga_abo = -1;
        if (ga_ebo == b) ga_ebo = -1;
        for (int w = 0; w < GA_N; w++)
            if (ga[w].known && ga[w].buf == b)
                ga[w].known = 0;
    }
}

static void ga_BindBuffer(GLenum target, GLuint b)
{
    gmv_gl_bind_buffer(target, b);
}
static void ga_DeleteBuffers(GLsizei n, const GLuint *names)
{
    if (!ga_del_real) ga_del_real = gl_real("glDeleteBuffers");
    gmv_gl_buffers_deleted(n, names);
    if (ga_del_real) ga_del_real(n, names);
}
static void ga_VertexPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
    gmv_gl_arr_pointer(GA_VERTEX, size, type, stride, p);
}
static void ga_NormalPointer(GLenum type, GLsizei stride, const void *p)
{
    gmv_gl_arr_pointer(GA_NORMAL, 3, type, stride, p);
}
static void ga_ColorPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
    gmv_gl_arr_pointer(GA_COLOR, size, type, stride, p);
}
static void ga_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
    int w = ga_unit_which();
    if (w < 0) {
        static void(APIENTRY * real)(GLint, GLenum, GLsizei, const void *);
        if (!real) real = gl_real("glTexCoordPointer");
        if (real) real(size, type, stride, p);
        return;
    }
    gmv_gl_arr_pointer(w, size, type, stride, p);
}

void *gmv_gl_arrays_intercept(const char *name)
{
    static const struct { const char *n; void *f; } t[] = {
        {"_glBindBuffer", (void *)ga_BindBuffer},
        {"_glBindBufferARB", (void *)ga_BindBuffer},
        {"_glDeleteBuffers", (void *)ga_DeleteBuffers},
        {"_glDeleteBuffersARB", (void *)ga_DeleteBuffers},
        {"_glVertexPointer", (void *)ga_VertexPointer},
        {"_glNormalPointer", (void *)ga_NormalPointer},
        {"_glColorPointer", (void *)ga_ColorPointer},
        {"_glTexCoordPointer", (void *)ga_TexCoordPointer},
        {NULL, NULL},
    };
    if (!gmv_gl_query_free())
        return NULL;
    ga_live = 1;
    for (int i = 0; t[i].n; i++)
        if (!strcmp(t[i].n, name))
            return t[i].f;
    return NULL;
}
