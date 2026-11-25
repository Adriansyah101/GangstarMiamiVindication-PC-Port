#ifndef GMV_VEH_H
#define GMV_VEH_H

#include <stdint.h>

#include "game.h"

int veh_install(uint32_t code_lo, uint32_t code_hi);
int veh_selftest(void);

void veh_note_init(uint32_t index, uint32_t addr);
void veh_clear_init(void);
void veh_note_main(uint32_t addr);

void gmv_watch_arm(uint32_t addr);

#ifndef _WIN32
void gmv_dump_note_signal(int sig, const void *si, const void *uc);
#endif

enum { GMV_ACCESS_READ, GMV_ACCESS_WRITE, GMV_ACCESS_EXEC };
void fault_set_low_range(uint32_t lo, uint32_t hi);
int fault_redirect(uint32_t eip, uint32_t *to);
const char *fault_where(uint32_t a);
const char *fault_access_name(int access);
void fault_report(const gmv_regs *r, int access, uint32_t addr);
int fault_stop_note(void);

#endif
