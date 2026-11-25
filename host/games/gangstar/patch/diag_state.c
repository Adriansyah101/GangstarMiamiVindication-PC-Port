#include "compat.h"
#include <stdint.h>
#include <string.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"

#if GMV_DEV
typedef void *(*msg_t)(void *self, const char *sel, ...);
extern void *gmv_objc_last_eaglview;
extern void *gmv_objc_last_gamewindow;
extern void *gmv_objc_last_gamedelegate;
extern int gmv_input_wants_tap;

/* 0x659f28 0x35d08() 0x32de0(fe) 0x1adbc4() */
__attribute__((noinline, force_align_arg_pointer))
static void gmv_menu_gates(int *fe_trans, int *fe_state, int *all_loaded)
{
    void *(*fe_get)(void)  = (void *(*)(void))(uintptr_t)0x35d08u;
    int   (*fe_tr)(void *) = (int (*)(void *))(uintptr_t)0x32de0u;
    int   (*loaded)(void)  = (int (*)(void))(uintptr_t)0x1adbc4u;
    void *fe = fe_get();
    *fe_trans   = fe ? (fe_tr(fe) & 0xff) : -1;
    *fe_state   = fe ? *(const int *)((const unsigned char *)fe + 0xc) : -0x1000;
    *all_loaded = loaded() & 0xff;
    /* 0x1b5b52 fe->screens[0][0x51] 0x32d98 [0x949ad8] */
    int scount = fe ? *(const int *)((const unsigned char *)fe + 0x2ec) : -1;
    void *sarr = fe ? *(void *const *)((const unsigned char *)fe + 0x2dc) : NULL;
    void *mainmenu = *(void *const *)(uintptr_t)0x949ad8u;
    unsigned char e5e4 = *(const unsigned char *)(uintptr_t)0x63e5e4u;
    /* *[0x63e038] 0x1a1752 G1[+0x94] [0x18] [0x20] edi[0x94] */
    void *g1slot = *(void *const *)(uintptr_t)0x63e038u;          /* 0x685668 */
    void *g1     = g1slot ? *(void *const *)g1slot : NULL;
    void *g1pg   = g1 ? *(void *const *)((const unsigned char *)g1 + 0x94) : (void *)-1;
    void *eamgr = *(void *const *)(uintptr_t)0x689ea8u;
    int   ctx   = eamgr ? *(const int *)((const unsigned char *)eamgr + 0x75c) : -0x1000;
    int   mmSt  = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x18) : -0x1000;
    int   mmCtx = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x20) : -0x1000;
    /* MainMenu [scr+0x1c] */
    int   mm4   = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x4) : -0x1000;
    int   mm8   = mainmenu ? *(const int *)((const unsigned char *)mainmenu + 0x8) : -0x1000;
    void *mm1c  = mainmenu ? *(void *const *)((const unsigned char *)mainmenu + 0x1c) : NULL;
    void *mm1cVt = (mm1c && !IsBadReadPtr(mm1c, 4)) ? *(void *const *)mm1c : NULL;
    int   mm1cSt = (mm1c && !IsBadReadPtr(mm1c, 0x20)) ? *(const int *)((const unsigned char *)mm1c + 0x18) : -0x1000;
    gmv_log("[state] MMSCR count[+4]=%d top[+8]=%d successor[+0x1c]=%p succVt=%p succState[+0x18]=%d\n",
            mm4, mm8, mm1c, mm1cVt, mm1cSt);
    /* vt[0x10] 0x32706 */
    void *warr = mainmenu ? *(void *const *)((const unsigned char *)mainmenu + 0x10) : NULL;
    if (warr && !IsBadReadPtr(warr, 12)) {
        for (int i = 0; i < 3; i++) {
            void *w = ((void *const *)warr)[i];
            void *wvt = (w && !IsBadReadPtr(w, 4)) ? *(void *const *)w : NULL;
            void *w8  = (wvt && !IsBadReadPtr(wvt, 0xc)) ? ((void *const *)wvt)[2] : NULL;
            void *w0  = (wvt && !IsBadReadPtr(wvt, 4)) ? ((void *const *)wvt)[0] : NULL;
            gmv_log("[state] WIDGET[%d]=%p vt=%p vt0=%p vt8(isDone)=%p\n", i, w, wvt, w0, w8);
        }
    } else {
        gmv_log("[state] WIDGET array=%p (unreadable)\n", warr);
    }
    /* [0x63e6c8] widget[0x10] [App+0x17ad8] [0x63e6c4] */
    {
        void *app3 = *(void *const *)(uintptr_t)0x6853a0u;
        int dt   = app3 ? *(const int *)((const unsigned char *)app3 + 0x17ad8) : -1;
        int w0dur = *(const int *)(uintptr_t)0xa9dc78u;   /* WIDGET[0][+0x10] */
        int alpha = *(const int *)(uintptr_t)0x63e6c4u;
        int ftmr  = *(const int *)(uintptr_t)0x63e6c8u;
        gmv_log("[state] FADE w0dur[+0x10]=%d frameDt[App+0x17ad8]=%d  alpha[0x63e6c4]=%d timer[0x63e6c8]=%d\n",
                w0dur, dt, alpha, ftmr);
    }
    const unsigned char *g6 = (const unsigned char *)(uintptr_t)0x949a58u;
    gmv_log("[state] FE fe=%p screens[0].count=%d arr=%p  mainMenu=%p st18=%d ctx20=%d onEnterRan=%u\n"
            "        G1slot[0x685668]=%p G1obj=%p curPage[G1+0x94]=%p  eamgr[0x689ea8]=%p ctx[+0x75c]=%d  gate6=%02x%02x%02x%02x%02x%02x\n",
            fe, scount, sarr, mainmenu, mmSt, mmCtx, e5e4,
            g1slot, g1, g1pg, eamgr, ctx, g6[0], g6[1], g6[2], g6[3], g6[4], g6[5]);
    /* 0x15e4fa() core[0xb4]->vtable[0x28]("MainMenu") */
    static int once;
    if (!once) {
        once = 1;
        void *(*core_get)(void) = (void *(*)(void))(uintptr_t)0x15e4fau;
        void *core = core_get();
        void *mgr  = core ? *(void *const *)((const unsigned char *)core + 0xb4) : NULL;
        void *vt   = mgr  ? *(void *const *)mgr : NULL;
        void *s28  = vt   ? *(void *const *)((const unsigned char *)vt + 0x28) : NULL;
        /* 0x3005a0 [0x104]/[0x108] [0x114]/[0x118] [0xf8] */
        int   defN  = mgr ? *(const int *)((const unsigned char *)mgr + 0x108) : -0x1000;
        void *defA  = mgr ? *(void *const *)((const unsigned char *)mgr + 0x104) : NULL;
        int   stkN  = mgr ? *(const int *)((const unsigned char *)mgr + 0x118) : -0x1000;
        void *stkA  = mgr ? *(void *const *)((const unsigned char *)mgr + 0x114) : NULL;
        unsigned f8 = mgr ? *(const unsigned char *)((const unsigned char *)mgr + 0xf8) : 0;
        void *top   = (stkA && stkN > 0) ? ((void *const *)stkA)[stkN - 1] : NULL;
        const char *topNm = top ? (const char *)((const unsigned char *)top + 8) : "<none>";
        int   topSt = top ? *(const int *)((const unsigned char *)top + 0x58) : -0x1000;
        void *topVt = top ? *(void *const *)top : NULL;
        int   top54 = top ? *(const int *)((const unsigned char *)top + 0x54) : -0x1000;
        int   mmDef = 0; char mmName[32] = "";
        if (defA && defN > 0 && defN < 4096)
            for (int i = 0; i < defN; i++) {
                void *d = ((void *const *)defA)[i];
                if (!d) continue;
                const char *nm = (const char *)((const unsigned char *)d + 8);
                if (!IsBadReadPtr(nm, 9) && !memcmp(nm, "MainMenu", 9)) { mmDef = 1; break; }
                if (i == 0 && !IsBadReadPtr(nm, 1)) { lstrcpynA(mmName, nm, 31); }
            }
        gmv_log("[state] SCENE core=%p mgr=%p vt=%p vt[0x28]=%p  defN=%d defArr=%p hasMainMenuDef=%d def[0]Name=\"%s\"\n"
                "        stackN=%d top=%p topVt=%p topName=\"%s\" topState[0x58]=%d top[0x54]=%d flags[0xf8]=0x%02x\n",
                core, mgr, vt, s28, defN, defA, mmDef, mmName, stkN, top, topVt, top ? topNm : "<none>", topSt, top54, f8);
        /* 0x1a1752 *(0x68a100) 0x124ab0(G1[0x94]) */
        void *sub   = *(void *const *)(uintptr_t)0x68a100u;
        void *subVt = sub ? *(void *const *)sub : NULL;
        void *sub8  = subVt ? *(void *const *)((const unsigned char *)subVt + 8) : NULL;
        void *sub18 = subVt ? *(void *const *)((const unsigned char *)subVt + 0x18) : NULL;
        gmv_log("[state] SUBSYS *(0x68a100)=%p vt=%p vt[8]=%p vt[0x18]=%p  G1curPage[+0x94]=%p\n",
                sub, subVt, sub8, sub18, g1pg);
        /* 0x1a9488 [0x68a768..0x68a76c] */
        void *vb = *(void *const *)(uintptr_t)0x68a768u;
        void *ve = *(void *const *)(uintptr_t)0x68a76cu;
        long  vn = (vb && ve) ? (long)(((const char *)ve - (const char *)vb) / 8) : -1;
        void *vb2 = *(void *const *)(uintptr_t)0x68a778u;
        void *ve2 = *(void *const *)(uintptr_t)0x68a77cu;
        long  vn2 = (vb2 && ve2) ? (long)(((const char *)ve2 - (const char *)vb2) / 8) : -1;
        gmv_log("[state] FEDRAW vec[0x68a768]=%p..%p count=%ld   vec2[0x68a778]=%p..%p count=%ld\n",
                vb, ve, vn, vb2, ve2, vn2);
        /* 0x4600 [App+0x17a88] */
        void *app = *(void *const *)(uintptr_t)0x6853a0u;
        if (app) {
            void *sent80 = (void *)((const unsigned char *)app + 0x17a80);
            void *sent88 = (void *)((const unsigned char *)app + 0x17a88);
            int k = 0; void *node = *(void *const *)sent80;
            while (node && node != sent80 && k < 10) {
                void *obj = *(void *const *)((const unsigned char *)node + 8);
                void *ovt = (obj && !IsBadReadPtr(obj, 4)) ? *(void *const *)obj : NULL;
                gmv_log("[state] A80[%d] node=%p obj=%p objvt=%p\n", k, node, obj, ovt);
                node = *(void *const *)node; k++;
            }
            if (k == 0) gmv_log("[state] A80 list EMPTY\n");
            k = 0; node = *(void *const *)sent88;
            while (node && node != sent88 && k < 10) {
                void *obj = *(void *const *)((const unsigned char *)node + 8);
                void *ovt = (obj && !IsBadReadPtr(obj, 4)) ? *(void *const *)obj : NULL;
                gmv_log("[state] A88[%d] node=%p obj=%p objvt=%p\n", k, node, obj, ovt);
                node = *(void *const *)node; k++;
            }
            if (k == 0) gmv_log("[state] A88 list EMPTY\n");
            /* vid->[0x10] vt[0x14]/vt[0x18] [layer+0xb4] */
            void *vid = *(void *const *)((const unsigned char *)app + 0x17aa4);
            void *layer = (vid && !IsBadReadPtr(vid, 0x14)) ? *(void *const *)((const unsigned char *)vid + 0x10) : NULL;
            if (layer && !IsBadReadPtr(layer, 0xc0)) {
                const uint32_t *L = (const uint32_t *)layer;
                gmv_log("[state] LAYER vid=%p layer=%p vt=%08x  [+0xb4]=%08x [+0xb8]=%08x [+0xbc]=%08x\n",
                        vid, layer, L[0], L[0xb4/4], L[0xb8/4], L[0xbc/4]);
            }
        }
        if (g1pg && (uintptr_t)g1pg > 0x10000 && !IsBadReadPtr(g1pg, 32 * 4)) {
            const uint32_t *pg = (const uint32_t *)g1pg;
            gmv_log("[state] PAGE obj=%p vt=%08x\n", g1pg, pg[0]);
            for (int r = 0; r < 4; r++)
                gmv_log("        +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", r * 0x20,
                        pg[r*8+0], pg[r*8+1], pg[r*8+2], pg[r*8+3],
                        pg[r*8+4], pg[r*8+5], pg[r*8+6], pg[r*8+7]);
        }
    }
}

void gmv_state_trace(void)
{
    static int on = -1;
    if (!GMV_DEV)
        return;
    if (on < 0) {
        const char *e = gmv_dbgenv("GMV_STATE_TRACE");
        on = gmv_env_on(e);
    }
    if (!on)
        return;
    /* 0x6853a0 0x31b0 0x31b8 [App+0x17af0] [App+0x17b00] */
    void *app = *(void **)(uintptr_t)0x6853a0u;
    static unsigned n;
    static unsigned pf0 = 0xffffffffu, pb0 = 0xffffffffu;
    if (!app) {
        if (n == 0) gmv_log("[state] App singleton [0x6853a0] still NULL\n");
        n++;
        return;
    }
    if (n == 0) {
        /* 0x19c600 -> theApp->vtable[0x24](theApp) */
        void **vt = *(void ***)app;
        gmv_log("[state] theApp=%p vtable=%p tick vt[0x24]=%p (vt[0x20]=%p vt[0x28]=%p)\n",
                app, (void *)vt, vt[9], vt[8], vt[10]);
    }
    const unsigned char *a = app;
    unsigned af0 = a[0x17af0], b00 = a[0x17b00];
    /* 0x19c630 [[App+0x17ac4]+4] */
    void *sobj = *(void *const *)(a + 0x17ac4);
    int state = sobj ? *(const int *)((const unsigned char *)sobj + 4) : -1;
    static int pstate = -2;
    /* 0x4508 [App+0x17ac0]->vtable[2](dt) [App+0x17a80] */
    void *mgr = *(void *const *)(a + 0x17ac0);
    const void *lh = *(const void *const *)(a + 0x17a80);
    int list_empty = (lh == (const void *)(a + 0x17a80));
    const unsigned char *vc = *(const unsigned char *const *)(a + 0x17aa4);
    int deque_empty = -1;
    if (vc) {
        const void *dh = *(const void *const *)(vc + 0xb8);
        const void *dt = *(const void *const *)(vc + 0xc8);
        deque_empty = (dh == dt);
    }
    static int pdeque = -2;
    /* [mgr+0xc] [mgr+0x10] */
    int stackN = -1;
    if (mgr) {
        unsigned base = *(const unsigned *)((const unsigned char *)mgr + 0xc);
        unsigned top  = *(const unsigned *)((const unsigned char *)mgr + 0x10);
        stackN = (int)((top - base) / 4);
    }
    static int pstk = -2;
    /* 0x1b81a8 0xa064a0 [S+0x218] [[S+0x214]+0xf8][0x19] */
    int s218 = *(const int *)(uintptr_t)0xa066b8u;              /* S+0x218 */
    void *s214 = *(void *const *)(uintptr_t)0xa066b4u;          /* S+0x214 */
    void *rdyobj = s214 ? *(void *const *)((const unsigned char *)s214 + 0xf8) : NULL;
    int rdy = rdyobj ? *(const unsigned char *)((const unsigned char *)rdyobj + 0x19) : -1;
    static int prdy = -2;
    /* changeState 0x12d96 */
    void *topState = NULL;
    if (mgr && stackN >= 1) {
        void *base = *(void *const *)((const unsigned char *)mgr + 0xc);
        if (base)
            topState = *(void *const *)base;
    }
    static void *ptop = (void *)-1;
    if (topState && topState != ptop) {
        void **svt = *(void ***)topState;
        gmv_log("[state] f%u topState=%p vtable=%p update(vt6)=%p vt3=%p vt4=%p vt5=%p vt7=%p\n",
                n, topState, (void *)svt, svt[6], svt[3], svt[4], svt[5], svt[7]);
    }
    if (n == 0 && mgr) {
        void **mvt = *(void ***)mgr;
        gmv_log("[state] mgr=%p vtable=%p update vt[2]=%p\n", mgr, (void *)mvt, mvt[2]);
    }
    /* 0x65a388 [state+0xc] */
    {
        static int dismiss = -1;
        if (dismiss < 0) {
            const char *e = gmv_dbgenv("GMV_DISMISS_SPLASH");
            dismiss = gmv_env_on(e);
        }
        if (dismiss && topState && *(void **)topState == (void *)(uintptr_t)0x65a388u) {
            unsigned char *g = (unsigned char *)topState + 0xc;
            if (*g == 0) {
                *g = 1;
                gmv_log("[state] >>> DISMISS_SPLASH: forced [state+0xc]=1 on %p (f%u)\n", topState, n);
            }
        }
    }
    /* 0x4600 R10->vt[0x1c] 0x521a70 [0x68541a] [0x685438] */
    {
        static int force = -1;
        if (force < 0) {
            const char *e = gmv_dbgenv("GMV_FORCE_DRAW");
            force = gmv_env_on(e);
        }
        if (force) {
            *(volatile unsigned char *)(uintptr_t)0x68541au = 1;
            *(volatile unsigned char *)(uintptr_t)0x685438u = 1;
            if (n == 0)
                gmv_log("[state] >>> FORCE_DRAW: forcing [0x68541a]=[0x685438]=1 each frame\n");
        }
    }
    /* 0x1a2590 [0x949a58..0x949a5d] */
    {
        static int fg = -1;
        if (fg < 0) {
            const char *e = gmv_dbgenv("GMV_FORCE_GATE6");
            fg = gmv_env_on(e);
        }
        if (fg) {
            volatile unsigned char *g6f = (volatile unsigned char *)(uintptr_t)0x949a58u;
            for (int i = 0; i < 6; i++) g6f[i] = 1;
            if (n == 0)
                gmv_log("[state] >>> FORCE_GATE6: forcing [0x949a58..5d]=1 each frame\n");
        }
    }
    if (n < 8 || n % 120 == 0 || af0 != pf0 || b00 != pb0 || state != pstate || deque_empty != pdeque
        || stackN != pstk || rdy != prdy) {
        int gate = topState ? *(const unsigned char *)((const unsigned char *)topState + 0xc) : -1;
        gmv_log("[state] f%u App=0x%08x [+0x17af0]=%u [+0x17b00]=%u [+0x17ad8]=%d vid=0x%08x state=%d sobj=%p"
                " mgr=%p listEmpty=%d dequeEmpty=%d stackN=%d S218=%d rdyObj=%p rdy=%d top=%p gate=%d\n",
                n, (unsigned)(uintptr_t)app, af0, b00,
                *(const int *)(a + 0x17ad8), *(const unsigned *)(a + 0x17aa4), state, sobj,
                mgr, list_empty, deque_empty, stackN, s218, rdyobj, rdy, topState, gate);
        /* 0x4600 0x127f8 top->vt[0x2c] vid->[0x1c]->vt[0x18] */
        unsigned char cv1a = *(const unsigned char *)(uintptr_t)0x68541au;
        unsigned char cv38 = *(const unsigned char *)(uintptr_t)0x685438u;
        unsigned char cv1c = *(const unsigned char *)(uintptr_t)0x68541cu;
        unsigned char ae4  = *(const unsigned char *)(a + 0x17ae4);
        void *r1c = vc ? *(void *const *)(vc + 0x1c) : NULL;
        void *r10 = vc ? *(void *const *)(vc + 0x10) : NULL;
        void *r1c_vt18 = r1c ? (*(void ***)r1c)[6] : NULL;   /* 0x18 */
        void *r10_vt1c = r10 ? (*(void ***)r10)[7] : NULL;   /* 0x1c */
        gmv_log("[state] f%u draw: cv1a=%u cv38=%u cv1c=%u ae4=%u  R1c=%p clr.vt18=%p  "
                "R10=%p draw.vt1c=%p\n",
                n, cv1a, cv38, cv1c, ae4, r1c, r1c_vt18, r10, r10_vt1c);
        /* 0x659f28 [self+0x1d] [self+0x24]/[self+0x25] */
        if (topState && *(void **)topState == (void *)(uintptr_t)0x659f28u) {
            const unsigned char *mg = (const unsigned char *)(uintptr_t)0x949a58u;
            void *g1 = *(void *const *)(uintptr_t)0x63e038u;
            void *g1v = g1 ? *(void *const *)((const unsigned char *)g1 + 0x94) : NULL;
            gmv_log("[state] f%u MENU self=%p [+1d]=%u [+24]=%u [+25]=%u  gate6=%02x%02x%02x%02x%02x%02x"
                    "  G1=%p [G1+0x94]=%p\n",
                    n, topState,
                    *((const unsigned char *)topState + 0x1d),
                    *((const unsigned char *)topState + 0x24),
                    *((const unsigned char *)topState + 0x25),
                    mg[0], mg[1], mg[2], mg[3], mg[4], mg[5], g1, g1v);
            int ftr = -2, fst = -2, ld = -2;
            gmv_menu_gates(&ftr, &fst, &ld);
            gmv_log("[state] f%u MENU gates: feTrans=%d (state=%d; 1..6 blocks) allLoaded=%d "
                    "(both must be feTrans=0 & allLoaded=1 for menu logic to run)\n",
                    n, ftr, fst, ld);
        }
    }
    gmv_input_wants_tap = (topState && *(void **)topState == (void *)(uintptr_t)0x65a388u);
    pf0 = af0;
    pb0 = b00;
    pstate = state;
    pdeque = deque_empty;
    pstk = stackN;
    prdy = rdy;
    ptop = topState;
    n++;
}
void gmv_state_trace_present(void)
{
    unsigned frames = gmv_gl_frame_no();
    /* 0xa058e0 +0x18..+0x1c [job+0x14] 0x1adbc4 */
    {
        static int st = -2;
        if (st == -2) { const char *e = gmv_dbgenv("GMV_STATE_TRACE"); st = (gmv_env_on(e)) ? 1 : 0; }
        if (st && frames % 60 == 0) {
            unsigned char *mgr = (unsigned char *)(uintptr_t)0xa058e0u;
            void **beg = *(void ***)(mgr + 0x18);
            void **end = *(void ***)(mgr + 0x1c);
            long njob = beg ? (long)(end - beg) : -1;
            gmv_log("[gl] f%u JOBS mgr=%p vec=%p..%p n=%ld\n",
                    frames, (void *)mgr, (void *)beg, (void *)end, njob);
            if (beg && njob >= 0 && njob <= 128) {
                for (long i = 0; i < njob; i++) {
                    unsigned char *job = (unsigned char *)beg[i];
                    if (!job) { gmv_log("     job[%ld]=NULL\n", i); continue; }
                    gmv_log("     job[%ld]=%p done14=%d  vt=%08x f4=%08x f8=%08x fc=%08x f10=%08x\n",
                            i, (void *)job, *(int *)(job + 0x14),
                            *(unsigned *)(job + 0), *(unsigned *)(job + 4), *(unsigned *)(job + 8),
                            *(unsigned *)(job + 0xc), *(unsigned *)(job + 0x10));
                }
            }
        }
    }
}
#endif
