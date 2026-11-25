#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "layout.h"
#include "log.h"
#include "macho.h"
#include "shims.h"

#define FN(mach_name, host_fn) {mach_name, (void *)(host_fn)}

#define OC_ISA   0
#define OC_SUPER 4
#define OC_NAME  8
#define OC_SIZE  20
#define OC_METHODS 28

static uint32_t u32(uint32_t addr) { return *(const uint32_t *)(uintptr_t)addr; }

static int in_image(uint32_t a) { return a >= GMV_RESERVE_START && a < GMV_MACHO_END; }

#define MAX_CLASS 64
static uint32_t g_class[MAX_CLASS];
static int g_nclass;
static int g_ready;
static uint32_t g_cfstr_lo, g_cfstr_hi;

static int is_registered(uint32_t cls)
{
    for (int i = 0; i < g_nclass; i++)
        if (g_class[i] == cls)
            return 1;
    return 0;
}

static uint32_t class_by_name(const char *name)
{
    for (int i = 0; i < g_nclass; i++)
        if (!strcmp((const char *)(uintptr_t)u32(g_class[i] + OC_NAME), name))
            return g_class[i];
    return 0;
}

void gmv_objc_init(const macho *m)
{
    if (g_ready)
        return;
    g_ready = 1;
    const macho_sect *mod = macho_sect_named(m, "__OBJC", "__module_info");
    if (!mod) {
        gmv_log("[objc] no __OBJC,__module_info -- runtime has no classes\n");
        return;
    }
    for (uint32_t a = mod->addr; a + 16 <= mod->addr + mod->size; a += 16) {
        uint32_t symtab = u32(a + 12);
        if (!symtab || !in_image(symtab))
            continue;
        uint16_t cls_cnt = *(const uint16_t *)(uintptr_t)(symtab + 8);
        for (uint16_t i = 0; i < cls_cnt && g_nclass < MAX_CLASS; i++) {
            uint32_t cls = u32(symtab + 12 + 4 * i);
            if (in_image(cls))
                g_class[g_nclass++] = cls;
        }
    }
    const macho_sect *cf = macho_sect_named(m, "__DATA", "__cfstring");
    if (cf) {
        g_cfstr_lo = cf->addr;
        g_cfstr_hi = cf->addr + cf->size;
    }
    gmv_trace("[objc] registered %d class(es):", g_nclass);
    for (int i = 0; i < g_nclass; i++)
        gmv_trace(" %s", (const char *)(uintptr_t)u32(g_class[i] + OC_NAME));
    gmv_trace("\n");
}

int gmv_objc_is_cfstring(const void *p)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    return g_cfstr_hi && a >= g_cfstr_lo && a < g_cfstr_hi;
}

static uint32_t method_in_list(uint32_t mlist, const char *sel)
{
    if (!mlist || !in_image(mlist))
        return 0;
    uint32_t count = u32(mlist + 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = mlist + 8 + 12 * i;
        const char *name = (const char *)(uintptr_t)u32(e);
        if (name && !strcmp(name, sel))
            return u32(e + 8);
    }
    return 0;
}

static uint32_t find_instance_imp(uint32_t cls, const char *sel)
{
    while (cls && is_registered(cls)) {
        uint32_t imp = method_in_list(u32(cls + OC_METHODS), sel);
        if (imp)
            return imp;
        uint32_t super = u32(cls + OC_SUPER);
        cls = super ? class_by_name((const char *)(uintptr_t)super) : 0;
    }
    return 0;
}

static uint32_t find_class_imp(uint32_t cls, const char *sel)
{
    uint32_t meta = u32(cls + OC_ISA);
    return in_image(meta) ? method_in_list(u32(meta + OC_METHODS), sel) : 0;
}

static struct fakecls { const char *name; } g_fake[128];
static volatile int g_nfake;
static SRWLOCK g_fake_lock = SRWLOCK_INIT;

static struct fakecls *fake_class(const char *name)
{
    struct fakecls *c = &g_fake[0];
    AcquireSRWLockExclusive(&g_fake_lock);
    int i;
    for (i = 0; i < g_nfake; i++)
        if (!strcmp(g_fake[i].name, name))
            break;
    if (i < g_nfake) {
        c = &g_fake[i];
    } else if (g_nfake < (int)(sizeof g_fake / sizeof *g_fake)) {
        g_fake[g_nfake].name = name;
        c = &g_fake[g_nfake];
        g_nfake++;
    }
    ReleaseSRWLockExclusive(&g_fake_lock);
    return c;
}

static int is_fake(uint32_t p)
{
    return p >= (uint32_t)(uintptr_t)g_fake && p < (uint32_t)(uintptr_t)(g_fake + g_nfake);
}

const char *gmv_objc_instance_class_name(const void *p)
{
    uint32_t s = (uint32_t)(uintptr_t)p;
    if (!s || in_image(s))
        return NULL;
    uint32_t isa = u32(s);
    if (is_registered(isa))
        return (const char *)(uintptr_t)u32(isa + OC_NAME);
    if (is_fake(isa))
        return ((struct fakecls *)(uintptr_t)isa)->name;
    return NULL;
}

void *gmv_objc_last_eaglview;
void *gmv_objc_last_gamewindow;
void *gmv_objc_last_gamedelegate;

void *gmv_objc_new_instance(const char *name)
{
    uint32_t cls = class_by_name(name);
    uint32_t isz = cls ? u32(cls + OC_SIZE) : 16;
    if (isz < 16)
        isz = 16;
    void *obj = calloc(1, isz);
    if (obj)
        *(uint32_t *)obj = cls ? cls : (uint32_t)(uintptr_t)fake_class(name);
    if (obj && name && !strcmp(name, GMV_CLASS_VIEW)) {
        gmv_objc_last_eaglview = obj;
        gmv_trace("[input] captured EAGLView instance 0x%08x\n", (unsigned)(uintptr_t)obj);
    }
    if (obj && name && !strcmp(name, GMV_CLASS_WINDOW))
        gmv_objc_last_gamewindow = obj;
    if (obj && name && !strcmp(name, GMV_CLASS_DELEGATE))
        gmv_objc_last_gamedelegate = obj;
    return obj;
}

static void *fnd_alloc(void *self, const char *sel, ...)
{
    (void)sel;
    return gmv_objc_new_instance((const char *)self);
}

static void *fnd_self(void *self, const char *sel, ...) { (void)sel; return self; }

static void objc_unhandled(const char *cls, const char *sel, const void *self)
{
    gmv_log("\n[objc] unhandled message [%s %s] (receiver 0x%08x)\n",
            cls ? cls : "?", sel ? sel : "?", (unsigned)(uintptr_t)self);
    gmv_log("stopping: this Objective-C class/selector needs a shim before the game gets further\n");
    gmv_fatal(7, "The game asked for something this port does not have yet: [%s %s].",
              cls ? cls : "?", sel ? sel : "?");
}

static int is_nsobject_init(const char *s)
{
    return !strcmp(s, "init") || !strcmp(s, "self") || !strcmp(s, "retain") ||
           !strcmp(s, "autorelease");
}
static int is_nsobject_noop(const char *s)
{
    return !strcmp(s, "release") || !strcmp(s, "drain") || !strcmp(s, "dealloc");
}

static const char *cocoa_super(const char *name)
{
    static const struct { const char *cls, *super; } m[] = {
        {"QTMovieView", "NSView"},     {"NSControl", "NSView"},
        {"NSButton", "NSControl"},     {"NSTextField", "NSControl"},
        {"NSView", "NSResponder"},     {"NSWindow", "NSResponder"},
        {"NSApplication", "NSResponder"}, {"NSResponder", "NSObject"},
        {"NSViewController", "NSResponder"}, {NULL, NULL},
    };
    for (int i = 0; m[i].cls; i++)
        if (!strcmp(m[i].cls, name))
            return m[i].super;
    return NULL;
}

void *gmv_objc_lookup(void *self, const char *sel)
{
    uint32_t s = (uint32_t)(uintptr_t)self;
    if (!s)
        return NULL;

    if (gmv_objc_is_cfstring(self)) {
        if (is_nsobject_init(sel))
            return (void *)fnd_self;
        if (is_nsobject_noop(sel))
            return NULL;
        void *f = gmv_fnd_instance_method("NSString", sel);
        if (f)
            return f;
        objc_unhandled("NSString", sel, self);
    }

    if (in_image(s)) {
        const char *name = (const char *)self;
        uint32_t cls = class_by_name(name);
        if (cls) {
            uint32_t imp = find_class_imp(cls, sel);
            if (imp)
                return (void *)(uintptr_t)imp;
        }
        if (!strcmp(sel, "alloc") || !strcmp(sel, "new") || !strcmp(sel, "allocWithZone:"))
            return (void *)fnd_alloc;
        if (!strcmp(sel, "class") || !strcmp(sel, "self"))
            return (void *)fnd_self;
        void *fimp = gmv_fnd_class_method(name, sel);
        if (fimp)
            return fimp;
        objc_unhandled(name, sel, self);
    }

    uint32_t isa = u32(s);
    const char *name;
    if (is_registered(isa)) {
        uint32_t imp = find_instance_imp(isa, sel);
        if (imp)
            return (void *)(uintptr_t)imp;
        name = (const char *)(uintptr_t)u32(isa + OC_NAME);
    } else if (is_fake(isa)) {
        name = ((struct fakecls *)(uintptr_t)isa)->name;
    } else {
        objc_unhandled("<unknown receiver>", sel, self);
        return NULL;
    }
    if (is_nsobject_init(sel))
        return (void *)fnd_self;
    if (is_nsobject_noop(sel))
        return NULL;
    for (const char *cn = name; cn;) {
        void *fimp = gmv_fnd_instance_method(cn, sel);
        if (fimp)
            return fimp;
        uint32_t c = class_by_name(cn);
        if (c) {
            uint32_t super = u32(c + OC_SUPER);
            cn = super ? (const char *)(uintptr_t)super : NULL;
        } else {
            cn = cocoa_super(cn);
        }
    }
    objc_unhandled(name, sel, self);
    return NULL;
}

int gmv_objc_responds(void *self, const char *sel)
{
    uint32_t s = (uint32_t)(uintptr_t)self;
    if (!s)
        return 0;
    uint32_t isa = u32(s);
    const char *name;
    if (is_registered(isa)) {
        if (find_instance_imp(isa, sel))
            return 1;
        name = (const char *)(uintptr_t)u32(isa + OC_NAME);
    } else if (is_fake(isa)) {
        name = ((struct fakecls *)(uintptr_t)isa)->name;
    } else {
        return 0;
    }
    if (is_nsobject_init(sel) || is_nsobject_noop(sel))
        return 1;
    for (const char *cn = name; cn;) {
        if (gmv_fnd_instance_method(cn, sel))
            return 1;
        uint32_t c = class_by_name(cn);
        if (c) {
            uint32_t super = u32(c + OC_SUPER);
            cn = super ? (const char *)(uintptr_t)super : NULL;
        } else {
            cn = cocoa_super(cn);
        }
    }
    return 0;
}

void *gmv_objc_super_lookup(void *superptr, const char *sel)
{
    const uint32_t *sup = superptr;
    if (!sup)
        return NULL;
    uint32_t sclass = sup[1];
    const char *clsname;
    if (is_registered(sclass))
        clsname = (const char *)(uintptr_t)u32(sclass + OC_NAME);
    else if (in_image(sclass))
        clsname = (const char *)(uintptr_t)sclass;
    else
        return NULL;

    uint32_t c = class_by_name(clsname);
    if (c) {
        uint32_t imp = find_instance_imp(c, sel);
        if (imp)
            return (void *)(uintptr_t)imp;
    }
    if (is_nsobject_init(sel))
        return (void *)fnd_self;
    if (is_nsobject_noop(sel))
        return NULL;
    for (const char *cn = clsname; cn;) {
        void *f = gmv_fnd_instance_method(cn, sel);
        if (f)
            return f;
        uint32_t cc = class_by_name(cn);
        if (!cc)
            break;
        uint32_t s2 = u32(cc + OC_SUPER);
        cn = s2 ? (const char *)(uintptr_t)s2 : NULL;
    }
    objc_unhandled(clsname, sel, superptr);
    return NULL;
}

void *gmv_objc_stret_lookup(void *self, const char *sel)
{
    uint32_t s = (uint32_t)(uintptr_t)self;
    if (!s)
        return NULL;
    if (!in_image(s)) {
        uint32_t isa = u32(s);
        if (is_registered(isa)) {
            uint32_t imp = find_instance_imp(isa, sel);
            if (imp)
                return (void *)(uintptr_t)imp;
        }
    }
    const char *cn = gmv_objc_instance_class_name(self);
    if (!cn && in_image(s))
        cn = (const char *)self;
    for (const char *c = cn; c;) {
        void *f = gmv_fnd_stret_method(c, sel);
        if (f)
            return f;
        uint32_t cc = class_by_name(c);
        if (cc) {
            uint32_t sup = u32(cc + OC_SUPER);
            c = sup ? (const char *)(uintptr_t)sup : NULL;
        } else {
            c = cocoa_super(c);
        }
    }
    objc_unhandled(cn ? cn : "?", sel, self);
    return NULL;
}

/* NSClassFromString 0x180eeb iTunes */
static void *ns_class_from_string(void *name)
{
    const char *s = gmv_nsstring_cstr(name);
    return s ? (void *)(uintptr_t)class_by_name(s) : NULL;
}

extern void gmv_objc_msgSend_fpret(void);
extern void gmv_objc_msgSendSuper(void);
extern void gmv_objc_msgSend_stret(void);

static void objc_exc_noop(void *data) { (void)data; }
static void *objc_exc_extract(void *data) { (void)data; return NULL; }
static int objc_exc_match(void *cls, void *exc) { (void)cls; (void)exc; return 0; }
static void objc_exc_throw(void *exc)
{
    (void)exc;
    gmv_log("\n[objc] objc_exception_throw -- unhandled ObjC exception, stopping\n");
    gmv_fatal(8, "The game stopped on an Objective-C error (an exception nothing caught).");
}

const host_symbol gmv_host_symbols_objc[] = {
    FN("_objc_msgSend", gmv_objc_msgSend),
    FN("_objc_msgSend_fpret", gmv_objc_msgSend_fpret),
    FN("_objc_msgSendSuper", gmv_objc_msgSendSuper),
    FN("_objc_msgSend_stret", gmv_objc_msgSend_stret),
    FN("_objc_exception_try_enter", objc_exc_noop),
    FN("_objc_exception_try_exit", objc_exc_noop),
    FN("_objc_exception_extract", objc_exc_extract),
    FN("_objc_exception_match", objc_exc_match),
    FN("_objc_exception_throw", objc_exc_throw),
    FN("_NSClassFromString", ns_class_from_string),

    {NULL, NULL},
};
