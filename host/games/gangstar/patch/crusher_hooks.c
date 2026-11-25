#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* +0xbc 0x114f17 0x2f0-byte 0x10b05e 0x904d 0x82504 */
#define UC_ALLOC      0x000904e0u
#define UC_SND_CTOR   0x000a2a1cu
#define UC_SND_ADD    0x000a4702u    /* +0x1d0 */
#define UC_SND_DTOR   0x000a43ecu
#define UC_FREE       0x00090450u
#define UC_CAR_VT     0x00656068u
#define UC_CREATE     0x000220d4u
#define UC_FREE_STR   0x00090430u
#define UC_HOME_LIST  0x000239f4u
#define UC_TO_LIST    0x0002100au
#define UC_NAV_SET    0x0005770eu    /* +0x18 */
#define UC_B2_SET     0x001c0570u
#define UC_CAT_TAB    0x00641320u    /* 0xff */
#define UC_LION_TITLE 0x904d

int uc_hooks;
patch_undo uc_undo;

const uc_spot_t uc_beach = { { 71921.4f, -19957.6f, 200.0f }, -6.38779f, 1, 468, 1 };
uc_t uc;

static uint32_t uc_sid(uint32_t id)
{
    return id >= 0x118 && id <= 0x121 ? (uint32_t)gmv_uc_res_base() + (id - 0x118) : id == 0x27f ? 0x67au : id;
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
void gmv_uc_engine(uint32_t v)
{
    if (gmv_uc_res_base() < 0) {
        gmv_log("[crusher] its sounds were never added to the list; it runs silent\n");
        return;
    }
#define W(o, x) (*(uint32_t *)(uintptr_t)(d + (o)) = (uint32_t)(x))
#define B(o, x) (*(unsigned char *)(uintptr_t)(d + (o)) = (unsigned char)(x))
#define V(o) (*(uint32_t *)(uintptr_t)(v + (o)))
    uint32_t d = ((uint32_t (*)(uint32_t, const char *, int, int))UC_ALLOC)(0x2f0, "Vehicle.cpp", 0x19f2, 1);
    ((void (*)(uint32_t))UC_SND_CTOR)(d);
    W(8, 0);
    V(0x200) = 0, V(0x1ec) = 1000, V(0x1d8) = 10000, V(0x1f0) = 9000;
    W(0, 10000), W(4, V(0x1ec)), V(0x1e0) = V(0x1ec);
    W(0x14, 1), W(0x10, -1), W(0xc, V(0x194));
    *(float *)(uintptr_t)(v + 0x20c) = (float)(int32_t)V(0x194);
    B(0x2e8, 1), W(0x2ec, 0x3dcccccd);
    W(0x15c, 5), W(0x180, 0x42480000);
    W(0x160, uc_sid(0x120)), W(0x164, uc_sid(0x11e)), W(0x168, uc_sid(0x11f)), W(0x16c, uc_sid(0x11d)), W(0x170, uc_sid(0x11d));
    W(0x184, 0x68b), W(0x188, 0xbb8), W(0x18c, 0x1388), W(0x190, 0x1e9b), W(0x194, 0x2300);
    W(0x1a4, 0), W(0x1a8, 0x7d0), W(0x1ac, 0xdac), W(0x1b0, 0x170c), W(0x1b4, 0x21fc);
    W(0x1c4, 0xaf0), W(0x1c8, 0x12c0), W(0x1cc, 0x1b58), W(0x1d0, 0x222e), W(0x1d4, V(0x1d8));
    W(0x1e4, 6), W(0x208, 0x42480000);
    W(0x1e8, uc_sid(0x118)), W(0x1ec, uc_sid(0x11c)), W(0x1f0, uc_sid(0x11a)), W(0x1f4, uc_sid(0x11b));
    W(0x1f8, uc_sid(0x119)), W(0x1fc, uc_sid(0x119));
    W(0x20c, 1000), W(0x210, 0x5c8), W(0x214, 0xba9), W(0x218, 0x1388), W(0x21c, 0x1c20), W(0x220, 0x2300);
    W(0x22c, 0), W(0x230, 0x44c), W(0x234, 0x834), W(0x238, 0xe10), W(0x23c, 0x1450), W(0x240, 0x21fc);
    W(0x24c, 0x640), W(0x250, 0xa28), W(0x254, 0x10cc), W(0x258, 0x17d4), W(0x25c, 0x222e), W(0x260, V(0x1d8));
    B(0x26c, 1), B(0x26d, 1), B(0x26e, 0);
    W(0x270, uc_sid(0x27f)), W(0x274, 0x41c80000);
    W(0x27c, 0x1b58), W(0x280, 0x7d0), W(0x284, V(0x1d8));
    B(0x294, 0), B(0x288, 1), W(0x28c, uc_sid(0x121)), W(0x290, 0x42480000), B(0x2a0, 0);
    ((void (*)(uint32_t, uint32_t))UC_SND_ADD)(V(0x1d0), d);
    ((void (*)(uint32_t))UC_SND_DTOR)(d);
    ((void (*)(uint32_t))UC_FREE)(d);
#undef W
#undef B
#undef V
    if (!uc.said++)
        gmv_log("[crusher] monster engine sounds set up\n");
}

/* 0x5c */
__attribute__((cdecl, force_align_arg_pointer, noinline))
void obj_vec(uint32_t a, uint32_t c, float *out)
{
    uint32_t d = (uint32_t)(uintptr_t)out;
    __asm__ volatile("movl %%esp, %%esi\n\t"
                     "andl $-16, %%esp\n\t"
                     "subl $8, %%esp\n\t"
                     "pushl %%ecx\n\t"
                     "pushl %%edx\n\t"
                     "call *%%eax\n\t"
                     "movl %%esi, %%esp"
                     : "+a"(a), "+c"(c), "+d"(d) : : "esi", "memory", "cc");
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_pos(uint32_t obj, float *out)
{
    uint32_t a = ms_rd(ms_rd(obj) + 0x5c);
    out[0] = out[1] = out[2] = 0;
    if (a)
        obj_vec(a, obj, out);
}

float uc_dist(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

int uc_is_car(uint32_t o)
{
    return o && ms_rd(o) == UC_CAR_VT;
}

int uc_lion_done(uint32_t mgr)
{
    uint32_t recs = ms_rd(mgr + 0x50);
    int n = recs ? (int)ms_rd(mgr + 0x54) : 0;
    for (int r = 0; r < n && r < 1000; r++) {
        uint32_t rec = ms_rd(recs + 4 * r);
        if (!rec || ms_rd(rec + 0x24) != UC_LION_TITLE)
            continue;
        int i = (int)ms_rd(rec + 4), j = (int)ms_rd(rec + 0x1c), k = (int)ms_rd(rec + 0x18);
        int bit = 5 * (j - 1) + k - 1;
        return i >= 1 && i <= 7 && bit >= 0 && bit < 32 && (ms_rd(mgr + 0x8c + 4 * i) >> bit & 1);
    }
    return 0;
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_place(uint32_t o, const uc_spot_t *s, int fresh)
{
    /* 0x100d50 0x5f14e8 */
    float pos[3] = { s->pos[0], s->pos[1], s->pos[2] };
    float rot[4] = { 0, 0, -sinf(s->ang / 2), cosf(s->ang / 2) };
    void (*set_pos)(uint32_t, float *) = (void (*)(uint32_t, float *))ms_rd(ms_rd(o) + 0x6c);
    void (*set_rot)(uint32_t, float *) = (void (*)(uint32_t, float *))ms_rd(ms_rd(o) + 0x74);
    set_rot(o, rot);
    set_pos(o, pos);
    if (fresh) {
        ((void (*)(uint32_t))ms_rd(ms_rd(o) + 0x44))(o);
        set_rot(o, rot);
    }
    set_pos(o, pos);
    uint32_t nav = ms_rd(o + 0x18), body;
    if (nav)
        ((void (*)(uint32_t, uint32_t, uint32_t, float *))UC_NAV_SET)(nav, s->cell, s->tri, pos);
    if ((body = ms_rd(o + 0x250)) != 0) {
        float xy[2] = { pos[0] / 100.0f, pos[1] / 100.0f };
        ((void (*)(uint32_t, float *, float))UC_B2_SET)(body, xy, s->ang);
    }
    set_pos(o, pos);
}

int uc_category(int type)
{
    volatile unsigned char *cat = (volatile unsigned char *)(uintptr_t)UC_CAT_TAB;
    if (cat[type] == 0xff) {
        int pick = gmv_uc_pick_type();
        cat[type] = pick >= 0 && pick < 256 && cat[pick] != 0xff ? cat[pick] : 3;
        gmv_trace("[crusher] category %d (the Pickup's)\n", (int)(signed char)cat[type]);
    }
    return (signed char)cat[type];
}

__attribute__((cdecl, force_align_arg_pointer, noinline))
uint32_t uc_make(uint32_t world, int type, const uc_spot_t *s)
{
    int cat = uc_category(type);
    uint32_t o = ((uint32_t (*)(uint32_t, int))UC_CREATE)(world, type), nm;
    if (!o)
        return 0;
    if ((nm = ms_rd(o + 0x1c)) != 0)            /* 0x819e8 */
        ((void (*)(uint32_t))UC_FREE_STR)(nm);
    *(uint32_t *)(uintptr_t)(o + 0x1c) = 0;
    ((void (*)(uint32_t))ms_rd(ms_rd(o) + 0xb4))(o);
    ((void (*)(uint32_t, int))ms_rd(ms_rd(o) + 0x4c))(o, cat);     /* 0x2720a */
    int list = ((int (*)(uint32_t))UC_HOME_LIST)(o);
    ((void (*)(uint32_t, uint32_t, int))UC_TO_LIST)(world, o, list);
    uc_place(o, s, 1);
    return o;
}

/* 0x706a0 vt+0x68 0xf8ca6 0x10000 0x729c2 vt+0x5c */
#define UC_RADIO_SET 0x00026e58u
#define UC_SEAT_FREE 0x00109a38u

__attribute__((cdecl, force_align_arg_pointer, noinline))
static int uc_swap(uint32_t world, uint32_t p, uint32_t truck)
{
    uint32_t c = p + 4;
    uc_spot_t s = { { 0 }, 0, 0, 0, 1 };
    uint32_t tb = ms_rd(truck + 0x250), car, cb, rd, was = ms_rd(c + 0x584), drv = ms_rd(truck + 0x280);
    float vel[3] = { 0, 0, 0 }, speed = *(float *)(uintptr_t)(truck + 0x3d4), moved[6];
    int pick = gmv_uc_pick_type();
    memcpy(moved, (const void *)(uintptr_t)(truck + 0x128), sizeof moved);
    uc_pos(truck, s.pos);
    s.cell = ((uint32_t (*)(uint32_t))ms_rd(ms_rd(truck) + 0x54))(truck) & 0xffff;
    s.tri = ((uint32_t (*)(uint32_t))ms_rd(ms_rd(truck) + 0x58))(truck) & 0xffff;
    if (tb) {
        memcpy(&s.ang, (const void *)(uintptr_t)(tb + 0x38), 4);
        memcpy(vel, (const void *)(uintptr_t)(tb + 0x40), sizeof vel);
    }
    car = pick >= 0 ? uc_make(world, pick, &s) : 0;
    if (!car) {
        static int told;
        if (!told++)
            gmv_log("[crusher] no D340 to put the player in (type %d): the truck stays while they are in it\n", pick);
        return 0;
    }
    if ((rd = ms_rd(c + 0x14)) != 0 && (ms_rd(rd + 0x108) & 1))
        ((void (*)(uint32_t, int))UC_RADIO_SET)(truck, -1);
    if (was & 0x2000000)
        ((void (*)(uint32_t, uint32_t))UC_SEAT_FREE)(truck, c);
    if (drv == c) {
        ((void (*)(uint32_t))ms_rd(ms_rd(truck) + 0x100))(truck);
        *(uint32_t *)(uintptr_t)(truck + 0x280) = 0;
    }
    *(uint32_t *)(uintptr_t)(c + 0x584) &= ~0x2010400u;
    *(uint32_t *)(uintptr_t)(c + 0x670) = 1;
    ((void (*)(uint32_t, uint32_t))ms_rd(ms_rd(p) + 0x68))(p, car);
    if (tb && (cb = ms_rd(car + 0x250)) != 0) {
        memcpy((void *)(uintptr_t)(cb + 0x40), vel, sizeof vel);
        *(uint16_t *)(uintptr_t)cb &= (uint16_t)~8;
        /* +0x3d4 +0x134 0xfd205 0x164 0x1088b6 0x1136e8 +0x128 */
        memcpy((void *)(uintptr_t)(car + 0x128), moved, sizeof moved);
        *(float *)(uintptr_t)(car + 0x3d4) = speed;
    }
    *(unsigned char *)(uintptr_t)(car + 5) |= 8;
    gmv_trace("[crusher] the player's truck is a D340 now: %08x at %.0f %.0f %.0f, in it %d, seated %d, driver %d; "
            "before: flags %08x, the truck's driver %s, speed %.1f (velocity %.1f %.1f), angle %.2f; the D340's angle %.2f\n", car, s.pos[0], s.pos[1], s.pos[2], ms_rd(c + 0x558) == car,
            (ms_rd(c + 0x584) & 0x400) != 0, ms_rd(car + 0x280) == c, was, drv == c ? "the player" : drv ? "another" : "none",
            speed, vel[0], vel[1], s.ang, (cb = ms_rd(car + 0x250)) != 0 ? *(float *)(uintptr_t)(cb + 0x38) : 0.f);
    return 1;
}

/* ObjectDisable 0xac +0x584 0x400 0x706a0 */
__attribute__((cdecl, force_align_arg_pointer, noinline))
void uc_leave(uint32_t world, uint32_t p, int type)
{
    uint32_t c = p + 4, truck = uc.obj;
    if (pointer_shown())
        return;
    if (!(uc_is_car(truck) && (int)(ms_rd(truck + 0x24) & 0xff) == type)) {
        uc.obj = 0;
        return;
    }
    if (ms_rd(c + 0x558) == truck) {
        /* vt+0x114 vt+0x68 */
        int seated = (ms_rd(c + 0x584) & 0x400) != 0;
        int wreck = ((char (*)(uint32_t))ms_rd(ms_rd(truck) + 0x88))(truck) != 0;
        int same = ((uint32_t (*)(uint32_t))ms_rd(ms_rd(c) + 0x114))(c) == truck;
        if (!seated || wreck || !same) {
#if GMV_DEV
            static int last = -1;
            int now = seated | wreck << 1 | same << 2;
            if (now != last)
                gmv_log("[crusher] no D340 yet: seated %d, wreck %d, the getter gives the truck %d, flags %08x\n",
                        seated, wreck, same, ms_rd(c + 0x584));
            last = now;
#endif
            return;
        }
        if (!uc_swap(world, p, truck))
            return;
    }
    ((void (*)(uint32_t))ms_rd(ms_rd(truck) + 0xac))(truck);
    uc.obj = uc.last_veh = uc.survey_car = 0;
    gmv_trace("[crusher] out of the city (off)\n");
}

/* 0x23d16 0x654d6 +0x10 +0x2e8 */
void lc_pos(uint32_t o, float *p)
{
    p[0] = p[1] = p[2] = 0;
    if (!o || !ms_rd(o))
        return;
    uint32_t get = ms_rd(ms_rd(o) + 0x5c), type = ms_rd(o + 0x24) & 0xff;
    if (get < 0x11000u || get >= 0x63d000u)
        return;
    if (type != 35 && type != 46 && type != 154 && type != 164 && type != 255 && type != 254)
        return;
    if ((get == 0x23d16u || get == 0x654d6u) && !ms_rd(o + 0x10))
        return;
    if (get == 0x654d6u && ((ms_rd(o + 0x584) & 0x400) || (ms_rd(o + 0x328) == 8 && !ms_rd(o + 0x2e8))))
        return;
    uc_pos(o, p);
}
