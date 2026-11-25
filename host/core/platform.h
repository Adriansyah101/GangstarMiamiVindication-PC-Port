#ifndef GMV_PLATFORM_H
#define GMV_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define GMV_PATH_MAX 1024

#ifdef _WIN32
#define GMV_SEP "\\"
#define GMV_PLATFORM_NAME "Win32"
#define GMV_PLATFORM_OS "Windows"
#define GMV_EXE_SUFFIX ".exe"
#else
#include <strings.h>
#define GMV_SEP "/"
#define GMV_PLATFORM_NAME "Linux"
#define GMV_PLATFORM_OS "Linux"
#define GMV_EXE_SUFFIX ""
#define __cdecl
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#endif

#define GMV_ASM_STR_(x) #x
#define GMV_ASM_STR(x) GMV_ASM_STR_(x)
#define GMV_ASM_SYM(name) GMV_ASM_STR(__USER_LABEL_PREFIX__) #name

int gmv_main(int argc, char **argv);

void gmv_exe_dir(char *out, size_t n);
FILE *gmv_fopen(const char *path, const char *mode);
int gmv_mkdir(const char *path);
int gmv_remove(const char *path);
int gmv_rename(const char *from, const char *to);
int gmv_replace(const char *from, const char *to);
int gmv_is_file(const char *path);
int gmv_full_path(const char *path, char *out, size_t n);
unsigned long gmv_last_error(void);

enum { GMV_PROT_NONE, GMV_PROT_R, GMV_PROT_RW, GMV_PROT_RX };
int gmv_protect(uint32_t addr, uint32_t size, int prot);
int gmv_readable(const void *p, size_t n);
void gmv_secure_zero(void *p, size_t n);
int gmv_module_of(uint32_t addr, char *name, size_t n, uint32_t *base);

uint32_t gmv_thread_id(void);
int gmv_thread_run(void (*fn)(void), int wait);
void gmv_sleep_forever(void) __attribute__((noreturn));
void gmv_exit(int code) __attribute__((noreturn));

void gmv_alert(const char *text, int on_top);
void gmv_release_cursor(void);

void gmv_platform_early(void);
void gmv_platform_log_start(void);
typedef int (*gmv_check_fn)(int ok, const char *fmt, ...);
void gmv_platform_address_space(gmv_check_fn check);

#endif
