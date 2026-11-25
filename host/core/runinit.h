#ifndef GMV_RUNINIT_H
#define GMV_RUNINIT_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"
#include "macho.h"

int run_initializers(const macho *m, const gmvp *t, uint32_t limit, char *err, size_t errsz);

int run_main(const macho *m, const gmvp *t, uint32_t main_addr, char *err, size_t errsz);

#endif
