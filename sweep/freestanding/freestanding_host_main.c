/* freestanding_host_main.c - host harness for the freestanding Duktape objects.
 *
 * The Duktape object, the libc shim, the arena and the smoke test are compiled
 * with -ffreestanding -nostdlib -fno-builtin and are checked separately for
 * undefined hosted symbols (see Makefile target sweep-freestanding-check).
 * This harness supplies the process entry/exit and printf, nothing else.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sweep_fs_arena.h"
#include "sweep_runtime.h"

#define FS_ARENA_BYTES (4u * 1024u * 1024u)

#if defined(_MSC_VER)
__declspec(align(16)) static unsigned char s_arena_storage[FS_ARENA_BYTES];
#else
static unsigned char s_arena_storage[FS_ARENA_BYTES] __attribute__((aligned(16)));
#endif

int sweep_fs_duktape_smoke(SWEEP_RUNTIME* rt, char* out, size_t out_cap);

static uint64_t fs_monotonic(void* user) {
    (void) user;
    return 0u;
}

static void fs_diag(void* user, const char* text) {
    (void) user;
    if (text != NULL) (void) fputs(text, stderr);
}

int main(void) {
    SWEEP_FS_ARENA arena;
    SWEEP_RUNTIME rt;
    SWEEP_RUNTIME_OPS ops;
    char status[128];
    int failures = 0;
    int rc;

    (void) printf("=== Sweep freestanding Duktape smoke ===\n");

    /* Direct allocator contract checks (no Duktape involved yet). */
    if (!sweep_fs_arena_init(&arena, s_arena_storage, FS_ARENA_BYTES)) {
        (void) printf("[FAIL] arena init\n");
        return 1;
    }
    {
        unsigned char* p = (unsigned char*) sweep_fs_arena_alloc(&arena, 64u);
        unsigned char* q;
        size_t i;
        int ok = p != NULL;
        if (ok) for (i = 0u; i < 64u; ++i) p[i] = (unsigned char) (i + 1u);
        q = (unsigned char*) sweep_fs_arena_realloc(&arena, p, 128u);
        ok = ok && q != NULL;
        if (ok) for (i = 0u; i < 64u; ++i) {
            if (q[i] != (unsigned char) (i + 1u)) ok = 0;
        }
        (void) printf("%s arena alloc/realloc preserves bytes\n",
                      ok ? "[PASS]" : "[FAIL]");
        failures += !ok;
        sweep_fs_arena_free(&arena, q);
        (void) printf("%s arena free returns all bytes\n",
                      arena.used == 0u ? "[PASS]" : "[FAIL]");
        failures += (arena.used != 0u);
        (void) printf("%s arena alloc(0) is NULL\n",
                      sweep_fs_arena_alloc(&arena, 0u) == NULL ? "[PASS]" : "[FAIL]");
        failures += (sweep_fs_arena_alloc(&arena, 0u) != NULL);
        (void) printf("%s arena realloc(p,0) frees\n",
                      (sweep_fs_arena_realloc(&arena,
                          sweep_fs_arena_alloc(&arena, 16u), 0u) == NULL &&
                       arena.used == 0u) ? "[PASS]" : "[FAIL]");
        failures += (arena.used != 0u);
    }

    memset(&ops, 0, sizeof(ops));
    ops.alloc = sweep_fs_arena_alloc;
    ops.realloc = sweep_fs_arena_realloc;
    ops.free = sweep_fs_arena_free;
    ops.monotonic_ms = fs_monotonic;
    ops.diag = fs_diag;
    memset(&rt, 0, sizeof(rt));
    if (!sweep_runtime_init(&rt, &ops, &arena)) {
        (void) printf("[FAIL] runtime init over arena\n");
        return 1;
    }

    status[0] = '\0';
    rc = sweep_fs_duktape_smoke(&rt, status, sizeof(status));
    (void) printf("%s Duktape script / allocator hooks (rc=%d, status=%s)\n",
                  rc == 0 ? "[PASS]" : "[FAIL]", rc, status);
    failures += (rc != 0);

    (void) printf("%s heap released all arena bytes (%lu bytes outstanding)\n",
                  arena.used == 0u ? "[PASS]" : "[FAIL]",
                  (unsigned long) arena.used);
    failures += (arena.used != 0u);
    (void) printf("%s arena high-water mark recorded (%lu bytes)\n",
                  arena.peak > 0u ? "[PASS]" : "[FAIL]",
                  (unsigned long) arena.peak);
    failures += (arena.peak == 0u);

    sweep_runtime_shutdown(&rt);
    (void) printf("--------------------------------\n");
    (void) printf("failures: %d\n", failures);
    (void) fflush(stdout);
    return failures != 0;
}
