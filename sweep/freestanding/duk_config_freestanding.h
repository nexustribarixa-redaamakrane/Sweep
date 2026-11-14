/* duk_config_freestanding.h - Duktape 2.7.0 configuration overrides for the
 * Sweep freestanding build.
 *
 * This header is force-included (-include) before duktape.h/duktape.c.  It
 * pulls in the vendored generated config once, then removes every hosted
 * platform dependency (Windows time APIs, default malloc allocators, local
 * timezone/parse/format helpers) and installs the small Sweep shims.
 *
 * It is intentionally additive: sweep/third_party/duktape is not modified.
 */
#ifndef SWEEP_DUK_CONFIG_FREESTANDING_H
#define SWEEP_DUK_CONFIG_FREESTANDING_H

#include "duk_config.h"

/* The generated config was produced for hosted Windows.  Drop those providers
 * before duktape.c is compiled so no Win32 call sites are emitted. */
#undef DUK_USE_DATE_NOW_WINDOWS
#undef DUK_USE_DATE_NOW_WINDOWS_SUBMS
#undef DUK_USE_DATE_TZO_WINDOWS
#undef DUK_USE_DATE_TZO_WINDOWS_NO_DST
#undef DUK_USE_GET_MONOTONIC_TIME_WINDOWS_QPC
#undef DUK_USE_GET_MONOTONIC_TIME
#undef DUK_USE_DATE_PRS_STRPTIME
#undef DUK_USE_DATE_FMT_STRFTIME

/* Use Duktape's internal pow/log fallbacks instead of libm cbrt/log2/log10. */
#undef DUK_CBRT
#undef DUK_LOG2
#undef DUK_LOG10

/* No default malloc/realloc/free wrappers: every heap must be created with
 * explicit sweep_fs_* allocator hooks. */
#undef DUK_USE_PROVIDE_DEFAULT_ALLOC_FUNCTIONS

#undef DUK_USE_OS_STRING
#define DUK_USE_OS_STRING "sweep-freestanding"
#undef DUK_USE_TARGET_INFO
#define DUK_USE_TARGET_INFO "sweep-freestanding"

/* No hosted setjmp/longjmp: GCC's builtins are part of the freestanding
 * compiler support and need no library symbol.  The buffer shape is the
 * documented GCC x86-64 layout (void *[5]). */
typedef void* sweep_fs_jmpbuf[5];
#undef DUK_JMPBUF_TYPE
#undef DUK_SETJMP
#undef DUK_LONGJMP
#define DUK_JMPBUF_TYPE  sweep_fs_jmpbuf
#define DUK_SETJMP(jb)   __builtin_setjmp((jb))
#define DUK_LONGJMP(jb)  __builtin_longjmp((jb), 1)

/* Time and fatal-error hooks supplied by sweep_freestanding_libc.c. */
extern double sweep_fs_date_now_ms(void);
extern void sweep_fs_fatal(void* udata, const char* msg);

#define DUK_USE_DATE_GET_NOW(ctx)               sweep_fs_date_now_ms()
#define DUK_USE_DATE_GET_LOCAL_TZOFFSET(d)      ((duk_double_t) 0.0)
#define DUK_USE_FATAL_HANDLER(udata, msg)       sweep_fs_fatal((udata), (msg))

#endif /* SWEEP_DUK_CONFIG_FREESTANDING_H */
