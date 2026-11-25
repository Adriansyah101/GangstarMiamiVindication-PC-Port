#ifndef GMV_IMPORTS_H
#define GMV_IMPORTS_H

#include <stddef.h>
#include <stdint.h>

#include "macho.h"

typedef struct {
    const char *name;
    int ordinal;
    int nslots, lazy_slots, nl_slots, data_slots;
    int weak_import;
    int is_data;
    uint32_t target;
    enum { IMP_HOST, IMP_MISSING_CODE, IMP_MISSING_DATA, IMP_WEAK_NULL } how;
} import;

typedef struct {
    import *imp;
    int n;
    uint32_t nslots;
    uint32_t count[4];
} import_table;

typedef struct {
    const char *name;
    void *addr;
} host_symbol;

int imports_collect(const macho *m, import_table *t, char *err, size_t errsz);
int imports_bind(const macho *m, import_table *t, char *err, size_t errsz);
const char *imports_guard_name(uint32_t addr);
const char *imports_lib_name(const macho *m, int ordinal);

#endif
