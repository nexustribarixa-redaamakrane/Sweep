/* sweep_freestanding_libc.h - declarations for the minimal freestanding C
 * environment used by the freestanding Duktape build.
 *
 * The freestanding objects are compiled with -ffreestanding -nostdlib
 * -fno-builtin and must not reference the hosted C library.  This header
 * documents the exact substitution surface: memory/string primitives, the
 * handful of math routines Duktape needs, and the two Duktape feature hooks.
 */
#ifndef SWEEP_FREESTANDING_LIBC_H
#define SWEEP_FREESTANDING_LIBC_H

#include <stddef.h>
#include <stdint.h>

/* Memory / string primitives. */
void* sweep_fs_memcpy(void* dst, const void* src, size_t n);
void* sweep_fs_memmove(void* dst, const void* src, size_t n);
void* sweep_fs_memset(void* dst, int c, size_t n);
int   sweep_fs_memcmp(const void* a, const void* b, size_t n);
size_t sweep_fs_strlen(const char* s);
int   sweep_fs_strcmp(const char* a, const char* b);
int   sweep_fs_strncmp(const char* a, const char* b, size_t n);
char* sweep_fs_strcpy(char* dst, const char* src);
char* sweep_fs_strncpy(char* dst, const char* src, size_t n);
char* sweep_fs_strcat(char* dst, const char* src);
char* sweep_fs_strchr(const char* s, int c);
char* sweep_fs_strrchr(const char* s, int c);
char* sweep_fs_strstr(const char* hay, const char* needle);

/* ASCII character classification and case conversion (ASCII-only). */
int tolower(int c);
int toupper(int c);
int isalpha(int c);
int isalnum(int c);
int isdigit(int c);
int isspace(int c);
int isctrl(int c);

/* Math routines used by Duktape's number conversion and Math built-in. */
double sweep_fs_floor(double x);
double sweep_fs_ceil(double x);
double sweep_fs_trunc(double x);
double sweep_fs_fmod(double x, double y);
double sweep_fs_sqrt(double x);
double sweep_fs_pow(double x, double y);
double sweep_fs_exp(double x);
double sweep_fs_log(double x);
double sweep_fs_sin(double x);
double sweep_fs_cos(double x);
double sweep_fs_tan(double x);
double sweep_fs_asin(double x);
double sweep_fs_acos(double x);
double sweep_fs_atan(double x);
double sweep_fs_atan2(double y, double x);

/* Duktape feature hooks (see duk_config_freestanding.h). */
double sweep_fs_date_now_ms(void);
void   sweep_fs_fatal(void* udata, const char* msg);
int    sweep_js_engine_timeout_check(void* udata);

#endif /* SWEEP_FREESTANDING_LIBC_H */
