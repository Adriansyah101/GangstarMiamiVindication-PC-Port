#include "compat.h"
#include <stdint.h>

#include "layout.h"
#include "log.h"
#include "shims.h"
#include "gangstar.h"
#include "patch.h"

/* 0x658e88 0x16fa34 0x16fbee [w+0xc]->vt[0x3c] 0x16fb70 */
#define CS_WIDGET      43

void cs_btn_visible(void *fx, int on)
{
    ((void (*)(void *, const char *, int))(uintptr_t)CR_VISIBLE)(fx, "widgetSkipScene.btnSkip", on);
}

const uint32_t cs_hide_sites[] = { 0x000ce020u, 0x000ce34cu };
volatile int g_cs_on, g_cs_hooked;
volatile int g_cs_line;
volatile int g_cs_skip, g_cs_next;

unsigned char *cs_widget(void)
{
    unsigned char *app = (unsigned char *)(uintptr_t)CS_APP;
    if (!*(const volatile unsigned char *)(uintptr_t)CS_APP_GUARD || *(int *)(app + 0x218) <= CS_WIDGET)
        return NULL;
    return (*(unsigned char ***)(app + 0x214))[CS_WIDGET];
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_texts(unsigned char *w)
{
    ((void (*)(void *))(uintptr_t)CS_TEXTS)(w);
    void *fx = *(void **)(w + 8);
    if (fx)
        cs_btn_visible(fx, !g_cs_on);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_show(void *app)
{
    g_cs_line = 1;
    ((void (*)(void *))(uintptr_t)(g_cs_on ? CS_ARROW_HIDE : CS_ARROW_SHOW))(app);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_arrow_hide(void *app)
{
    g_cs_line = 0;
    ((void (*)(void *))(uintptr_t)CS_ARROW_HIDE)(app);
}

__attribute__((cdecl, force_align_arg_pointer))
void gmv_cs_cursor(void *cursor, int on)
{
    ((void (*)(void *, int))(uintptr_t)CS_CURSOR_SET)(cursor, on && !g_cs_on);
}
