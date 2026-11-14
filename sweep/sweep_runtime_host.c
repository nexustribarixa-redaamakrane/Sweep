/* sweep_runtime_host.c - hosted implementation of the Sweep runtime seam.
 *
 * This file is the ONLY place in the runtime abstraction that assumes a hosted
 * C library.  It exists so the existing browser engine keeps its current
 * behaviour while the dependency sits behind function pointers.  It is not a
 * freestanding backend.
 */
#include "sweep_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static void* host_alloc(void* user, size_t size) {
    (void)user;
    if (size == 0u) return NULL;
    return malloc(size);
}

static void* host_realloc(void* user, void* ptr, size_t size) {
    (void)user;
    if (size == 0u) return NULL; /* caller routes free() before reaching us */
    return realloc(ptr, size);
}

static void host_free(void* user, void* ptr) {
    (void)user;
    free(ptr);
}

static uint64_t host_monotonic_ms(void* user) {
    (void)user;
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
    }
#endif
}

static void host_diag(void* user, const char* text) {
    (void)user;
    if (text == NULL) return;
    fputs(text, stderr);
}

/* Bounded file read.  Writes at most out_cap-1 bytes, always NUL-terminates,
 * and reports an over-cap resource as failure instead of truncating it. */
static bool host_read_resource(void* user, const char* path, char* out,
                               size_t out_cap, size_t* out_len) {
    FILE* fp;
    size_t n = 0u;

    (void)user;
    if (out_len != NULL) *out_len = 0u;
    if (path == NULL || out == NULL || out_cap < 2u) return false;

    fp = fopen(path, "rb");
    if (fp == NULL) return false;

    while (n < out_cap - 1u) {
        size_t got = fread(out + n, 1u, out_cap - 1u - n, fp);
        n += got;
        if (got == 0u) break;
    }
    if (n == out_cap - 1u) {
        int extra = fgetc(fp);
        if (extra != EOF) { /* resource is larger than the caller's buffer */
            (void)fclose(fp);
            out[0] = '\0';
            return false;
        }
    }
    if (ferror(fp)) {
        (void)fclose(fp);
        out[0] = '\0';
        return false;
    }
    (void)fclose(fp);
    out[n] = '\0';
    if (out_len != NULL) *out_len = n;
    return true;
}

static bool host_init(void* user) {
    (void)user;
    return true;
}

static void host_shutdown(void* user) {
    (void)user;
}

static const SWEEP_RUNTIME_OPS g_host_ops = {
    host_alloc,
    host_realloc,
    host_free,
    host_monotonic_ms,
    host_diag,
    host_read_resource,
    host_init,
    host_shutdown
};

const SWEEP_RUNTIME_OPS* sweep_runtime_host_ops(void) {
    return &g_host_ops;
}

/* Legacy compatibility for APIs whose signature predates the runtime seam
 * (sweep_resource_fetch_file).  Kept in the host backend so the engine side
 * never owns hidden hosted state; new code should pass an explicit runtime. */
SWEEP_RUNTIME* sweep_runtime_host_default(void) {
    static SWEEP_RUNTIME s_host_runtime;
    static bool s_attempted = false;

    if (!s_attempted) {
        s_attempted = true;
        (void)sweep_runtime_init(&s_host_runtime, &g_host_ops, NULL);
    }
    return s_host_runtime.initialized ? &s_host_runtime : NULL;
}
