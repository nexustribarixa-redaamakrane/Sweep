/* sweep_runtime.h - minimal injected runtime boundary for Sweep.
 *
 * Sweep is host-tested today and depends on the hosted C library.  This header
 * is the first, deliberately small step away from that: the engine side talks
 * to an explicit runtime object instead of calling malloc/realloc/free, a clock,
 * a logger or a file reader directly.
 *
 * It is NOT a freestanding runtime.  It only makes the hosted dependency a
 * single, replaceable seam.  A caller that keeps the existing host behaviour
 * opts into sweep_runtime_host_ops(); a KAPA/kernel host would supply its own
 * implementation later.
 *
 * Contracts:
 *   - alloc/free and monotonic_ms are mandatory; every other operation is
 *     optional and a missing operation fails deterministically (NULL/false/0)
 *     rather than falling back to libc.
 *   - realloc(NULL, n) is alloc(n); realloc(p, 0) frees p and returns NULL.
 *     A runtime with no realloc operation fails without freeing p, so the
 *     caller keeps ownership of the old block.
 *   - read_resource writes at most out_cap-1 bytes plus a NUL, never more than
 *     the caller's capacity.  Overflow is a failure, not a truncation.
 *   - No operation is called on a runtime that did not initialize.
 */
#ifndef SWEEP_RUNTIME_H
#define SWEEP_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _SWEEP_RUNTIME_OPS {
    void*    (*alloc)(void* user, size_t size);
    void*    (*realloc)(void* user, void* ptr, size_t size);
    void     (*free)(void* user, void* ptr);
    uint64_t (*monotonic_ms)(void* user);
    void     (*diag)(void* user, const char* text);
    bool     (*read_resource)(void* user, const char* path, char* out,
                              size_t out_cap, size_t* out_len);
    bool     (*init)(void* user);
    void     (*shutdown)(void* user);
} SWEEP_RUNTIME_OPS;

typedef struct _SWEEP_RUNTIME {
    const SWEEP_RUNTIME_OPS* ops;
    void*                    user;
    bool                     initialized;
} SWEEP_RUNTIME;

/* Validate and bind ops.  Fails on NULL arguments, on missing mandatory
 * operations, and when the optional init hook reports failure. */
bool sweep_runtime_init(SWEEP_RUNTIME* rt, const SWEEP_RUNTIME_OPS* ops,
                        void* user);
void sweep_runtime_shutdown(SWEEP_RUNTIME* rt);

/* Routed operations.  Each returns a deterministic failure when the runtime is
 * unbound, uninitialized, or does not implement the operation. */
void*    sweep_runtime_alloc(SWEEP_RUNTIME* rt, size_t size);
void*    sweep_runtime_realloc(SWEEP_RUNTIME* rt, void* ptr, size_t size);
void     sweep_runtime_free(SWEEP_RUNTIME* rt, void* ptr);
uint64_t sweep_runtime_monotonic_ms(SWEEP_RUNTIME* rt);
void     sweep_runtime_diag(SWEEP_RUNTIME* rt, const char* text);
bool     sweep_runtime_read_resource(SWEEP_RUNTIME* rt, const char* path,
                                     char* out, size_t out_cap,
                                     size_t* out_len);

/* Overflow-safe deadline test for a monotonic millisecond clock:
 * true once now_ms has reached deadline_ms.  UINT64_MAX never wraps. */
bool sweep_runtime_deadline_reached(uint64_t now_ms, uint64_t deadline_ms);

/* Hosted implementation: malloc/realloc/free, a monotonic millisecond clock,
 * stderr diagnostics and fopen/fread resource reads.  This preserves current
 * behaviour only; it is not a freestanding backend. */
const SWEEP_RUNTIME_OPS* sweep_runtime_host_ops(void);

/* Legacy-host compatibility object for APIs that predate the runtime seam
 * (sweep_resource_fetch_file).  Lazily initialized; NULL if binding failed.
 * New code should carry an explicit SWEEP_RUNTIME* instead. */
SWEEP_RUNTIME* sweep_runtime_host_default(void);

#endif /* SWEEP_RUNTIME_H */
