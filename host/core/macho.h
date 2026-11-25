#ifndef GMV_MACHO_H
#define GMV_MACHO_H

#include <stddef.h>
#include <stdint.h>

#define MACHO_MAX_SEGS   8
#define MACHO_MAX_SECTS  64
#define MACHO_MAX_DYLIBS 32

typedef struct {
    char name[17];
    uint32_t vmaddr, vmsize, fileoff, filesize, initprot;
} macho_seg;

typedef struct {
    char segname[17], name[17];
    uint32_t addr, size, offset, flags, reserved1, reserved2;
} macho_sect;

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t cputype, filetype, flags, entry;
    macho_seg segs[MACHO_MAX_SEGS];
    int nsegs;
    macho_sect sects[MACHO_MAX_SECTS];
    int nsects;
    const char *dylibs[MACHO_MAX_DYLIBS];
    int ndylibs;
    uint32_t symoff, nsyms, stroff, strsize;
    uint32_t indirectsymoff, nindirectsyms;
    uint32_t bind_off, bind_size, weak_off, weak_size, lazy_off, lazy_size;
    uint32_t *hash;
    uint32_t hash_mask;
} macho;

enum { BIND_REGULAR, BIND_LAZY, BIND_WEAK };
#define BINDF_WEAK_IMPORT  1
#define BINDF_STRONG_DEF   8

typedef int (*macho_bind_cb)(void *ctx, uint32_t slot, const char *name, int ordinal,
                             int32_t addend, int flags);

int macho_parse(macho *m, const uint8_t *data, uint32_t size, char *err, size_t errsz);
const macho_sect *macho_sect_named(const macho *m, const char *seg, const char *sect);
const macho_sect *macho_sect_at(const macho *m, uint32_t addr);
const macho_seg *macho_seg_named(const macho *m, const char *seg);
int macho_binds(const macho *m, int which, macho_bind_cb cb, void *ctx, char *err, size_t errsz);
uint32_t macho_lookup(const macho *m, const char *name);
const uint8_t *macho_bytes(const macho *m, uint32_t addr, uint32_t n);

#endif
