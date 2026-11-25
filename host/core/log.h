#ifndef GMV_LOG_H
#define GMV_LOG_H

#include <stddef.h>

#ifdef GMV_RELEASE
#define GMV_DEV 0
#else
#define GMV_DEV 1
#endif

#define GMV_VERSION "V1.0.1"

void log_open(const char *path);
void log_discard(void);
void log_hide_path(const char *path, const char *tag);
void gmv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gmv_logfile(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#if GMV_DEV
#define gmv_trace gmv_log
#else
#define gmv_trace(...) do { if (0) gmv_log(__VA_ARGS__); } while (0)
#endif
void log_close(void);
void gmv_fatal(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
struct _EXCEPTION_POINTERS;
void gmv_dump_arm(const char *path);
void gmv_dump_note(struct _EXCEPTION_POINTERS *ep);
int gmv_dump_write(void);
int gmv_dump_is_writer(void);
void log_scrub_bytes(unsigned char *d, size_t n);

#endif
