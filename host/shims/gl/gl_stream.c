/* gl_stream.c -- STREAM_VBO: the game's client vertex arrays copied into a buffer object of the host's before a draw */
#include <stdint.h>
#include <string.h>
#include "compat.h"
#include <GL/gl.h>

#include "log.h"
#include "shims.h"
#include "glshim.h"

/* ---- STREAM_VBO (a port speed-up, [Advanced]) ---------------------------------------------------------------------
   Why.  The game draws part of the city, the cars and the people from vertex arrays in its own memory (on the Mac, as
   glitch's usage-4 buffers; STATIC_VBO moves the unchanging ones into buffer objects, the rest stay).  For such a draw
   the driver has to find the vertex range from the indices and copy the vertices itself, inside the draw call: on
   NVIDIA's driver the draws cost most of the main thread's time in the city (nvoglv32 ~40-45 % of the CPU samples,
   PERF_PROFILE), much less on AMD's.  The FastFive and GT Racing ports gained from the same copy made by the host.  Not
   a fault of the game (the Mac's driver had its own costs) nor of the port.
   What.  Before a draw with enabled client arrays the host finds the vertex range (from the indices: the game's memory,
   or a buffer made static by the game's code, gmv_game_gl_buffer_bytes), copies those bytes into a ring mapped once
   (glBufferStorage, persistent and coherent: the copy is a memcpy, no glBufferSubData, which made AMD's driver slower in
   the GT Racing port) and points the arrays at it; the game's GL_ARRAY_BUFFER binding is put back.  Vertex numbers are
   kept (no base vertex, as GT Racing's MIXED_KEEP): an array the game reads from its own buffer object in the same
   draw, and the effects that draw the same vertices again right after (the shadows, the shiny cars, the glow layer),
   use the game's own indices.  The ring is four parts with a fence each: a part is written again only after the GPU
   has finished the draws that read it.  The arrays stay pointed at the ring until the game sets a pointer (QUERY_FREE's
   array wrappers clear the mark) or a draw that is not streamed needs the game's memory again.  The host's readers of
   the arrays see the game's values (QUERY_FREE's shadow, which STREAM_VBO needs).  The bytes drawn are the same.
   STREAM_VBO=0: every draw as the game makes it. */

#define SV_SEG (4u << 20)
#define SV_SEGS 4
#define SV_RING (SV_SEG * SV_SEGS)
#define ARRAY_BUF 0x8892

static int sv_on = -1;
static GLuint sv_buf;
static uint8_t *sv_map;
static size_t sv_off;                                   /* where the next copy may go */
static unsigned sv_seg;                                 /* the part now written */
static void *sv_fence[SV_SEGS];
static unsigned sv_moved;                               /* GA_ bits: arrays pointed at the ring */

static struct {
    void(APIENTRY *BindBuffer)(GLenum, GLuint);
    void(APIENTRY *GenBuffers)(GLsizei, GLuint *);
    void(APIENTRY *BufferStorage)(GLenum, intptr_t, const void *, GLbitfield);
    void *(APIENTRY *MapBufferRange)(GLenum, intptr_t, intptr_t, GLbitfield);
    void *(APIENTRY *FenceSync)(GLenum, GLbitfield);
    GLenum(APIENTRY *ClientWaitSync)(void *, GLbitfield, unsigned long long);
    void(APIENTRY *DeleteSync)(void *);
    void(APIENTRY *VertexPointer)(GLint, GLenum, GLsizei, const void *);
    void(APIENTRY *NormalPointer)(GLenum, GLsizei, const void *);
    void(APIENTRY *ColorPointer)(GLint, GLenum, GLsizei, const void *);
    void(APIENTRY *TexCoordPointer)(GLint, GLenum, GLsizei, const void *);
    void(APIENTRY *ClientActiveTexture)(GLenum);
} r;

#if GMV_DEV
static struct {
    unsigned draws, client, streamed, noidx, nofit, putback, waits, cn, bn;
    double bytes;
    long long wait_t, ct, bt;                           /* time in the game's draws with / without client arrays */
} st;
#endif

static unsigned type_size(GLint t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case 0x140C: return 4;    /* GL_FIXED */
    case GL_DOUBLE: return 8;
    default: return 0;
    }
}

static int sv_init(void)
{
    sv_on = 0;
    const char *e = gmv_getenv("GMV_STREAM_VBO");
    int want = !e || !*e || *e != '0';
    if (!want) {
        gmv_log("[vbo] STREAM_VBO=0: the client arrays are drawn as the game gives them\n");
        return 0;
    }
    if (!gmv_gl_query_free()) {
        gmv_log("[vbo] STREAM_VBO=1, but QUERY_FREE=0: it needs QUERY_FREE's array state; off\n");
        return 0;
    }
#define R(f, n) (*(void **)&r.f = gl_real(n))
    R(BindBuffer, "glBindBuffer"); R(GenBuffers, "glGenBuffers"); R(BufferStorage, "glBufferStorage");
    R(MapBufferRange, "glMapBufferRange"); R(FenceSync, "glFenceSync"); R(ClientWaitSync, "glClientWaitSync");
    R(DeleteSync, "glDeleteSync"); R(VertexPointer, "glVertexPointer"); R(NormalPointer, "glNormalPointer");
    R(ColorPointer, "glColorPointer"); R(TexCoordPointer, "glTexCoordPointer");
    R(ClientActiveTexture, "glClientActiveTexture");
#undef R
    if (!r.ClientActiveTexture) *(void **)&r.ClientActiveTexture = gl_real("glClientActiveTextureARB");
    void **all = (void **)&r;
    for (size_t i = 0; i < sizeof r / sizeof(void *); i++)
        if (!all[i]) {
            gmv_log("[vbo] STREAM_VBO=1, but the driver has no persistent buffer mapping (GL 4.4 glBufferStorage); off\n");
            return 0;
        }
    GLint abo = gmv_gl_buf(0x8894);                     /* GL_ARRAY_BUFFER_BINDING */
    r.GenBuffers(1, &sv_buf);
    r.BindBuffer(ARRAY_BUF, sv_buf);
    r.BufferStorage(ARRAY_BUF, SV_RING, NULL, 0x2 | 0x40 | 0x80);          /* MAP_WRITE | PERSISTENT | COHERENT */
    sv_map = r.MapBufferRange(ARRAY_BUF, 0, SV_RING, 0x2 | 0x40 | 0x80);
    r.BindBuffer(ARRAY_BUF, (GLuint)abo);
    if (!sv_buf || !sv_map) {
        gmv_log("[vbo] STREAM_VBO=1, but the ring could not be mapped; off\n");
        return 0;
    }
    sv_on = 1;
    gmv_log("[vbo] STREAM_VBO=1: client vertex arrays copied into a %u MB persistent ring (%u parts) before each draw\n",
            SV_RING >> 20, SV_SEGS);
    return 1;
}

static int sv_ready(void)
{
    return sv_on > 0 || (sv_on < 0 && sv_init());
}

/* Into the next part: the one left gets a fence, the one entered waits for its own (the GPU has finished drawing
   from it; at a few MB a frame that was several frames ago, so the wait is normally over already). */
static void sv_next_part(void)
{
    sv_fence[sv_seg] = r.FenceSync(0x9117, 0);          /* GL_SYNC_GPU_COMMANDS_COMPLETE */
    sv_seg = (sv_seg + 1) % SV_SEGS;
    void **f = &sv_fence[sv_seg];
    if (*f) {
#if GMV_DEV
        long long t0 = gmv_prof_now();
        int waited = 0;
#endif
        for (int i = 0; i < 50; i++) {                  /* GL_SYNC_FLUSH_COMMANDS_BIT the first time; 0.1 s a try */
            GLenum w = r.ClientWaitSync(*f, i ? 0 : 1, 100000000ull);
#if GMV_DEV
            if (w == 0x911C) waited = 1;                /* GL_CONDITION_SATISFIED: it had to wait */
#endif
            if (w != 0x911B)                            /* not GL_TIMEOUT_EXPIRED: signalled, or failed */
                break;
#if GMV_DEV
            waited = 1;
#endif
        }
        r.DeleteSync(*f);
        *f = NULL;
#if GMV_DEV
        if (waited) {
            st.waits++;
            st.wait_t += gmv_prof_now() - t0;
        }
#endif
    }
}

/* Room for `bytes` at or past `need` (an array's vertex 0 lies `need` bytes before its first vertex copied), at an
   offset with the low four bits of `align` (the source's address: the arrays keep their alignment); the offset, or
   (size_t)-1 when it cannot fit. */
static size_t sv_at(size_t from, size_t need, uintptr_t align)
{
    size_t o = from > need ? from : need;
    return ((o + 15) & ~(size_t)15) + (align & 15);
}
static size_t sv_room(size_t need, size_t bytes, uintptr_t align)
{
    if (need > SV_RING - 32 || bytes > SV_RING - 32 - need)
        return (size_t)-1;
    size_t o = sv_at(sv_off, need, align);
    if (o + bytes > SV_RING) {                          /* round to the start: through every part to the first */
        do
            sv_next_part();
        while (sv_seg != 0);
        o = sv_at(0, need, align);
    }
    unsigned last = (unsigned)((o + bytes - 1) / SV_SEG);
    while (sv_seg != last)
        sv_next_part();
    return o;
}

static void sv_point(int w, const gmv_arr_t *a, const void *p)
{
    if (w == GA_VERTEX) r.VertexPointer(a->size, (GLenum)a->type, a->stride, p);
    else if (w == GA_NORMAL) r.NormalPointer((GLenum)a->type, a->stride, p);
    else if (w == GA_COLOR) r.ColorPointer(a->size, (GLenum)a->type, a->stride, p);
    else r.TexCoordPointer(a->size, (GLenum)a->type, a->stride, p);
}

/* Set the arrays in m (each a GA_ bit, with its pointer in p[]); texcoord units through the client unit. */
static void sv_set(unsigned m, const gmv_arr_t *a, const uintptr_t *p, GLuint buf)
{
    int unit = -1;
    GLuint abo = (GLuint)gmv_gl_buf(0x8894);            /* the game's, put back after */
    r.BindBuffer(ARRAY_BUF, buf);
    for (int w = 0; w < GA_N; w++) {
        if (!(m & (1u << w)))
            continue;
        if (w >= GA_TEX0 && w - GA_TEX0 != unit) {
            unit = w - GA_TEX0;
            r.ClientActiveTexture(0x84C0 + (GLenum)unit);
        }
        sv_point(w, &a[w], (const void *)p[w]);
    }
    if (unit >= 0 && (unsigned)unit != g_tf_client)
        r.ClientActiveTexture(0x84C0 + g_tf_client);
    r.BindBuffer(ARRAY_BUF, abo);
}

/* The moved arrays in m back to the game's memory (a draw that is not streamed). */
static void sv_put_back(unsigned m, const gmv_arr_t *a)
{
    m &= sv_moved;
    if (!m)
        return;
    uintptr_t p[GA_N];
    for (int w = 0; w < GA_N; w++)
        p[w] = (uintptr_t)a[w].ptr;
    sv_set(m, a, p, 0);
    sv_moved &= ~m;
#if GMV_DEV
    st.putback++;
#endif
}

/* The enabled arrays read from the game's memory, with their values. */
static unsigned sv_client(gmv_arr_t *a)
{
    unsigned m = 0;
    for (int w = 0; w < GA_N; w++) {
        if (!gmv_gl_arr_on(w))
            continue;
        gmv_gl_arr(w, &a[w]);
        if (!a[w].buf && a[w].ptr && a[w].size > 0 && type_size(a[w].type))
            m |= 1u << w;
    }
    return m;
}

/* Vertices first..last of the arrays in m into the ring, their vertex numbers kept; 0 = it could not. */
static int sv_move(unsigned m, const gmv_arr_t *a, uint32_t first, uint32_t last)
{
    struct { uintptr_t lo, hi; int w; } b[GA_N];
    int nb = 0;
    for (int w = 0; w < GA_N; w++) {
        if (!(m & (1u << w)))
            continue;
        uintptr_t es = (uintptr_t)a[w].size * type_size(a[w].type);
        uintptr_t stv = a[w].stride ? (uintptr_t)a[w].stride : es;
        uintptr_t p = (uintptr_t)a[w].ptr;
        b[nb].lo = p + first * stv;
        b[nb].hi = p + last * stv + es;
        b[nb].w = w;
        nb++;
    }
    for (int x = 1; x < nb; x++)                        /* by lo: arrays in one interleaved block go in once */
        for (int y = x; y > 0 && b[y].lo < b[y - 1].lo; y--) {
            __typeof__(b[0]) t = b[y];
            b[y] = b[y - 1];
            b[y - 1] = t;
        }
    uintptr_t p[GA_N] = {0};
    for (int x = 0; x < nb;) {
        uintptr_t glo = b[x].lo, ghi = b[x].hi;
        int y = x + 1;
        while (y < nb && b[y].lo < ghi) {
            if (b[y].hi > ghi)
                ghi = b[y].hi;
            y++;
        }
        size_t need = 0;                                /* the ring's offset of each array's vertex 0 is >= 0 */
        for (int k = x; k < y; k++) {
            uintptr_t ap = (uintptr_t)a[b[k].w].ptr;
            if (glo > ap && glo - ap > need)
                need = glo - ap;
        }
        size_t bytes = ghi - glo, o = sv_room(need, bytes, glo);
        if (o == (size_t)-1) {
#if GMV_DEV
            st.nofit++;
#endif
            return 0;                                   /* blocks copied before stay unused */
        }
        memcpy(sv_map + o, (const void *)glo, bytes);
        sv_off = o + bytes;
#if GMV_DEV
        st.bytes += (double)bytes;
#endif
        for (int k = x; k < y; k++)
            p[b[k].w] = o + ((uintptr_t)a[b[k].w].ptr - glo);
        x = y;
    }
    sv_set(m, a, p, sv_buf);
    sv_moved |= m;
    return 1;
}

/* The draw's enabled client arrays (GA_ bits) when STREAM_VBO is on, or when PERF_PROFILE times the draws (dev) with
   QUERY_FREE's shadow to tell them; *on: STREAM_VBO is. */
#if GMV_DEV
/* GMV_STREAM_AB=1 (dev, with PERF_PROFILE): STREAM_VBO on and off by turns, one PERF_PROFILE window each, so that both
   are measured with the same traffic in one run (gmv_sv_report says which a window had). */
static int sv_ab = -1, sv_ab_off;
#endif

static unsigned sv_begin(gmv_arr_t *a, int *on)
{
    *on = sv_ready();
#if GMV_DEV
    if (sv_ab < 0) {
        const char *e = gmv_dbgenv("GMV_STREAM_AB");
        sv_ab = *on && gmv_perf_on() && gmv_env_on(e);
    }
    int ab_off = sv_ab && sv_ab_off;
    if (ab_off)
        *on = 0;
#endif
    if (!*on && !(gmv_perf_on() && gmv_gl_query_free()))
        return 0;
    unsigned m = sv_client(a);
#if GMV_DEV
    st.draws++;
    if (m) st.client++;
    if (ab_off && (m & sv_moved))
        sv_put_back(m, a);                              /* the game's memory again while off */
#endif
    return m;
}

unsigned gmv_sv_elements(GLsizei count, GLenum type, const void *idx)
{
    gmv_arr_t a[GA_N];
    int on;
    unsigned m = sv_begin(a, &on);
    if (!m || !on)
        return m;
    unsigned ts = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_INT ? 4 : 0;
    GLint eb = ts && count > 0 ? gmv_gl_buf(0x8895) : 0;        /* GL_ELEMENT_ARRAY_BUFFER_BINDING */
    const uint8_t *ix = !ts || count <= 0 ? NULL : eb ? gmv_game_gl_buffer_bytes((unsigned)eb, (uintptr_t)idx) : idx;
    if (ix) {
        uint32_t lo = 0xffffffffu, hi = 0;
        if (ts == 2)
            for (GLsizei k = 0; k < count; k++) {
                uint32_t v = ((const uint16_t *)ix)[k];
                lo = v < lo ? v : lo, hi = v > hi ? v : hi;
            }
        else
            for (GLsizei k = 0; k < count; k++) {
                uint32_t v = ts == 1 ? ix[k] : ((const uint32_t *)ix)[k];
                lo = v < lo ? v : lo, hi = v > hi ? v : hi;
            }
        if (sv_move(m, a, lo, hi)) {
#if GMV_DEV
            st.streamed++;
#endif
            return m;
        }
    }
#if GMV_DEV
    else
        st.noidx++;
#endif
    sv_put_back(m, a);
    return m;
}

unsigned gmv_sv_arrays(GLint first, GLsizei count)
{
    gmv_arr_t a[GA_N];
    int on;
    unsigned m = sv_begin(a, &on);
    if (!m || !on)
        return m;
    if (first >= 0 && count > 0 && sv_move(m, a, (uint32_t)first, (uint32_t)first + (uint32_t)count - 1)) {
#if GMV_DEV
        st.streamed++;
#endif
        return m;
    }
    sv_put_back(m, a);
    return m;
}

/* QUERY_FREE's pointer wrappers: the game (or the host through gmv_gl_arr_pointer) set this array again. */
void gmv_sv_pointer_set(int which)
{
    sv_moved &= ~(1u << which);
}

unsigned gmv_sv_moved(void)
{
    return sv_moved;
}

#if GMV_DEV
/* gl_draw.c, PERF_PROFILE: one of the game's draws took from t0 (the effects' hooks around it included). */
void gmv_sv_draw_time(unsigned m, long long t0)
{
    long long dt = gmv_prof_now() - t0;
    if (m) st.ct += dt, st.cn++;
    else st.bt += dt, st.bn++;
}

void gmv_sv_report(double seconds)
{
    if (!st.draws)
        return;
    double ms_of(long long q);                          /* loadprof.c */
    gmv_logfile("[perf]   draws: %.0f a second, %.0f with client arrays (%.1f us each, %.2f ms a second), %.0f without "
                "(%.1f us each, %.2f ms)\n", st.draws / seconds, st.cn / seconds,
                st.cn ? ms_of(st.ct) * 1000 / st.cn : 0, ms_of(st.ct) / seconds, st.bn / seconds,
                st.bn ? ms_of(st.bt) * 1000 / st.bn : 0, ms_of(st.bt) / seconds);
    if (sv_ab > 0) {
        gmv_logfile("[perf]   STREAM_AB: this window STREAM_VBO=%d\n", !sv_ab_off);
        sv_ab_off = !sv_ab_off;
    }
    if (sv_on > 0)
        gmv_logfile("[perf]   STREAM_VBO: %.0f draws a second streamed (%.1f MB a second); not streamed: %.0f no index "
                    "data, %.0f too big; %.0f put back; %u fence waits (%.2f ms)\n", st.streamed / seconds,
                    st.bytes / seconds / 1048576, st.noidx / seconds, st.nofit / seconds, st.putback / seconds,
                    st.waits, ms_of(st.wait_t));
    memset(&st, 0, sizeof st);
}
#endif
