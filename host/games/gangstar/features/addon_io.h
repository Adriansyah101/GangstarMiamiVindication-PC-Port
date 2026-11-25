#ifndef GMV_ADDON_IO_H
#define GMV_ADDON_IO_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wr32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint32_t rdbe(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void wrbe(unsigned char *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static inline void unhide(unsigned char *d) { for (int i = 0; i < 4; i++) d[i] = (unsigned char)(d[i] - i - 1); }
static inline void hide(unsigned char *d) { for (int i = 0; i < 4; i++) d[i] = (unsigned char)(d[i] + i + 1); }

static inline const char *base_name(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static inline unsigned char *read_all(const char *path, size_t *n, size_t spare)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = len > 0 ? malloc((size_t)len + spare) : NULL;
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *n = d ? (size_t)len : 0;
    return d;
}

static inline int write_all(const char *path, const unsigned char *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    int ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

#endif
