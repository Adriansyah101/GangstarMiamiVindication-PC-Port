#ifdef _WIN32
#include <string.h>
#include <windows.h>

#include "game.h"
#include "imports.h"
#include "layout.h"
#include "log.h"
#include "veh.h"

#if GMV_DEV
static volatile uint32_t g_watch_addr;
#endif

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    /* [0x949a58] [0x68541a]/[0x685438] */
#if GMV_DEV
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        static volatile LONG whits, dumps;
        LONG h = InterlockedIncrement(&whits);
        int val = *(const int *)(uintptr_t)g_watch_addr;
        if (h <= 100)
            gmv_log("[watch] #%ld [0x%08x]<-%d eip=0x%08x (%s)\n",
                    h, (unsigned)g_watch_addr, val, (unsigned)cx->Eip, fault_where((uint32_t)cx->Eip));
        if (InterlockedIncrement(&dumps) <= 10) {
            gmv_log("        eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
                    cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi, cx->Ebp, cx->Esp);
            const uint32_t *sp = (const uint32_t *)(uintptr_t)cx->Esp;
            if (!IsBadReadPtr((void *)sp, 64 * sizeof(uint32_t)))
                for (int k = 0; k < 64; k++)
                    if (sp[k] >= 0x1000 && sp[k] < GMV_TEXT_HI)
                        gmv_log("        [esp+%02x] -> game 0x%08x\n", k * 4, sp[k]);
        }
        cx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uint32_t eip = cx->Eip, fault = (uint32_t)er->ExceptionInformation[1], to;
    int fetch = (uint32_t)(uintptr_t)er->ExceptionAddress == eip && fault == eip;

    if (fetch && fault_redirect(eip, &to)) {
        cx->Eip = to;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (gmv_dump_is_writer())
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG in_handler;
    LONG me = (LONG)GetCurrentThreadId();
    if (in_handler == me)
        return EXCEPTION_CONTINUE_SEARCH;
    const char *missing = imports_guard_name(fault);
    if (missing) {
        gmv_log("\n[MISSING DATA] game touched unimplemented data import %s at eip 0x%08x\n", missing, eip);
        gmv_dump_note(ep);
        gmv_fatal(4, "The game used data this port does not have yet: %s.", missing);
    }
    if (InterlockedCompareExchange(&in_handler, me, 0) != 0)
        Sleep(INFINITE);
    int access = er->ExceptionInformation[0] == 8 ? GMV_ACCESS_EXEC : er->ExceptionInformation[0] ? GMV_ACCESS_WRITE
                                                                                                 : GMV_ACCESS_READ;
    gmv_regs r = {eip, cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi, cx->Ebp, cx->Esp};
    fault_report(&r, access, fault);
    if (fault_stop_note()) {
        gmv_dump_note(ep);
        gmv_fatal(6, "The game crashed: a bad memory %s at 0x%08x, in %s.",
                  access == GMV_ACCESS_EXEC ? "jump" : fault_access_name(access), eip, fault_where(eip));
    }
    in_handler = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK last_chance(EXCEPTION_POINTERS *ep)
{
    uint32_t eip = (uint32_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    gmv_log("\n[fault] unhandled exception 0x%08lx at eip 0x%08x in %s\n",
            ep->ExceptionRecord->ExceptionCode, eip, fault_where(eip));
    gmv_dump_note(ep);
    gmv_fatal(9, "The game crashed: exception 0x%08lx at 0x%08x, in %s.",
              ep->ExceptionRecord->ExceptionCode, eip, fault_where(eip));
}

#if GMV_DEV
/* DR0 0x949a58 DR1 0x68541a DR2 0x685438 */
void gmv_watch_arm(uint32_t addr)
{
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &c)) {
        gmv_log("[watch] GetThreadContext failed err=%lu\n", GetLastError());
        return;
    }
    g_watch_addr = addr;
    c.Dr0 = addr;
    c.Dr1 = 0;
    c.Dr2 = 0;
    c.Dr3 = 0;
    c.Dr6 = 0;
    c.Dr7 = 0x000D0001u;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!SetThreadContext(th, &c)) {
        gmv_log("[watch] SetThreadContext failed err=%lu\n", GetLastError());
        return;
    }
    gmv_log("[watch] armed DR0=0x%08x(4B,write) DR7=0x%08x on tid=%lu\n",
            (unsigned)addr, (unsigned)0x000D0001u, GetCurrentThreadId());
}
#endif

int veh_install(uint32_t code_lo, uint32_t code_hi)
{
    fault_set_low_range(code_lo, code_hi);
    SetUnhandledExceptionFilter(last_chance);
    return AddVectoredExceptionHandler(1, handler) ? 0 : -1;
}
#endif
