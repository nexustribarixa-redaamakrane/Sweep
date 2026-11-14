/* sweep_runtime_test.c - host tests for the injected Sweep runtime seam.
 *
 * Covers validation/binding, routed operations, deterministic failure when an
 * optional operation is missing, bounded resource reads, deadline arithmetic,
 * the hosted backend, and the runtime-explicit resource provider.  This suite
 * is deliberately hosted: the runtime seam itself is what is under test, not
 * a kernel backend.
 *
 * Build (from Sweep root): included in `make sweep-tests`.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../sweep_runtime.h"
#include "../sweep_resource.h"

static int g_failures = 0;
static int g_passed = 0;

static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

/* ------------------------------------------------------------------------- */
/* Fake runtime: no libc allocation, records every routed call.              */
/* ------------------------------------------------------------------------- */

typedef struct {
    int alloc_calls;
    int realloc_calls;
    int free_calls;
    int diag_calls;
    int read_calls;
    int init_calls;
    int shutdown_calls;
    size_t last_alloc_size;
    char last_diag[64];
    const char* payload;
    size_t payload_len;
    int force_read_failure;
    int force_read_overrun;
} FAKE_RT;

static unsigned char s_arena[256];
static int s_init_result = 1;

static void* fake_alloc(void* user, size_t size) {
    FAKE_RT* f = (FAKE_RT*) user;
    f->alloc_calls++;
    f->last_alloc_size = size;
    return (size == 0u) ? NULL : (void*) s_arena;
}

static void* fake_realloc(void* user, void* ptr, size_t size) {
    FAKE_RT* f = (FAKE_RT*) user;
    f->realloc_calls++;
    f->last_alloc_size = size;
    (void) ptr;
    return (size == 0u) ? NULL : (void*) s_arena;
}

static void fake_free(void* user, void* ptr) {
    FAKE_RT* f = (FAKE_RT*) user;
    f->free_calls++;
    (void) ptr;
}

static uint64_t fake_monotonic(void* user) {
    (void) user;
    return 1234u;
}

static void fake_diag(void* user, const char* text) {
    FAKE_RT* f = (FAKE_RT*) user;
    size_t n;
    f->diag_calls++;
    n = strlen(text);
    if (n >= sizeof(f->last_diag)) n = sizeof(f->last_diag) - 1u;
    memcpy(f->last_diag, text, n);
    f->last_diag[n] = '\0';
}

static bool fake_read(void* user, const char* path, char* out,
                      size_t out_cap, size_t* out_len) {
    FAKE_RT* f = (FAKE_RT*) user;
    size_t n = f->payload_len;

    f->read_calls++;
    (void) path;
    if (f->force_read_failure) return false;
    if (f->force_read_overrun) {
        if (out_len) *out_len = out_cap; /* contract violation on purpose */
        return true;
    }
    if (n + 1u > out_cap) return false;
    if (n > 0u) memcpy(out, f->payload, n);
    out[n] = '\0';
    if (out_len) *out_len = n;
    return true;
}

static bool fake_init(void* user) {
    FAKE_RT* f = (FAKE_RT*) user;
    f->init_calls++;
    return s_init_result != 0;
}

static void fake_shutdown(void* user) {
    FAKE_RT* f = (FAKE_RT*) user;
    f->shutdown_calls++;
}

static void fake_ops(SWEEP_RUNTIME_OPS* ops) {
    ops->alloc = fake_alloc;
    ops->realloc = fake_realloc;
    ops->free = fake_free;
    ops->monotonic_ms = fake_monotonic;
    ops->diag = fake_diag;
    ops->read_resource = fake_read;
    ops->init = fake_init;
    ops->shutdown = fake_shutdown;
}

/* ------------------------------------------------------------------------- */

static void test_binding(void) {
    SWEEP_RUNTIME rt;
    SWEEP_RUNTIME_OPS ops;
    FAKE_RT f;

    memset(&rt, 0, sizeof(rt));
    memset(&ops, 0, sizeof(ops));
    memset(&f, 0, sizeof(f));
    fake_ops(&ops);

    check("[runtime] init rejects NULL runtime",
          !sweep_runtime_init(NULL, &ops, &f));
    check("[runtime] init rejects NULL ops",
          !sweep_runtime_init(&rt, NULL, &f));

    {
        SWEEP_RUNTIME_OPS bad = ops;
        bad.alloc = NULL;
        check("[runtime] missing alloc rejected",
              !sweep_runtime_init(&rt, &bad, &f));
    }
    {
        SWEEP_RUNTIME_OPS bad = ops;
        bad.free = NULL;
        check("[runtime] missing free rejected",
              !sweep_runtime_init(&rt, &bad, &f));
    }
    {
        SWEEP_RUNTIME_OPS bad = ops;
        bad.monotonic_ms = NULL;
        check("[runtime] missing clock rejected",
              !sweep_runtime_init(&rt, &bad, &f));
    }

    s_init_result = 0;
    check("[runtime] failing init hook rejected",
          !sweep_runtime_init(&rt, &ops, &f) && !rt.initialized &&
          rt.ops == NULL && rt.user == NULL);
    s_init_result = 1;

    memset(&rt, 0, sizeof(rt));
    {
        int before = f.init_calls;
        check("[runtime] valid ops bind",
              sweep_runtime_init(&rt, &ops, &f) && rt.initialized &&
              rt.ops == &ops && rt.user == &f &&
              f.init_calls == before + 1);
    }

    /* Missing optional operations are deterministic, not libc fallbacks. */
    {
        SWEEP_RUNTIME_OPS minimal;
        SWEEP_RUNTIME r2;
        memset(&minimal, 0, sizeof(minimal));
        minimal.alloc = fake_alloc;
        minimal.free = fake_free;
        minimal.monotonic_ms = fake_monotonic;
        memset(&r2, 0, sizeof(r2));
        check("[runtime] minimal ops bind without optional hooks",
              sweep_runtime_init(&r2, &minimal, &f));
        check("[runtime] missing realloc fails without freeing",
              sweep_runtime_realloc(&r2, s_arena, 16u) == NULL &&
              f.free_calls == 0);
        check("[runtime] missing diag is a no-op",
              (sweep_runtime_diag(&r2, "ignored"), 1));
        check("[runtime] missing resource reader fails",
              !sweep_runtime_read_resource(&r2, "x", (char*) s_arena,
                                           sizeof(s_arena), NULL));
        sweep_runtime_shutdown(&r2);
        check("[runtime] shutdown without hook clears binding",
              !r2.initialized && r2.ops == NULL && r2.user == NULL);
    }

    sweep_runtime_shutdown(&rt);
    check("[runtime] shutdown calls hook and clears binding",
          f.shutdown_calls == 1 && !rt.initialized && rt.ops == NULL &&
          rt.user == NULL);
    check("[runtime] shutdown is idempotent",
          (sweep_runtime_shutdown(&rt), f.shutdown_calls == 1));
}

static void test_routing(void) {
    SWEEP_RUNTIME rt;
    SWEEP_RUNTIME_OPS ops;
    FAKE_RT f;
    char buf[16];
    size_t len = 99u;

    memset(&ops, 0, sizeof(ops));
    memset(&f, 0, sizeof(f));
    fake_ops(&ops);
    memset(&rt, 0, sizeof(rt));
    (void) sweep_runtime_init(&rt, &ops, &f);

    check("[runtime] alloc routes to backend",
          sweep_runtime_alloc(&rt, 32u) == (void*) s_arena &&
          f.alloc_calls == 1 && f.last_alloc_size == 32u);
    check("[runtime] zero-size alloc does not call backend",
          sweep_runtime_alloc(&rt, 0u) == NULL && f.alloc_calls == 1);
    check("[runtime] realloc(NULL, n) routes to alloc",
          sweep_runtime_realloc(&rt, NULL, 8u) == (void*) s_arena &&
          f.alloc_calls == 2 && f.realloc_calls == 0);
    check("[runtime] realloc(p, n) routes to backend",
          sweep_runtime_realloc(&rt, s_arena, 16u) == (void*) s_arena &&
          f.realloc_calls == 1);
    check("[runtime] realloc(p, 0) frees and returns NULL",
          sweep_runtime_realloc(&rt, s_arena, 0u) == NULL &&
          f.free_calls == 1 && f.realloc_calls == 1);
    check("[runtime] free(NULL) is a no-op",
          (sweep_runtime_free(&rt, NULL), f.free_calls == 1));
    check("[runtime] free routes to backend",
          (sweep_runtime_free(&rt, s_arena), f.free_calls == 2));
    check("[runtime] monotonic routes to backend",
          sweep_runtime_monotonic_ms(&rt) == 1234u);
    check("[runtime] diag routes to backend",
          (sweep_runtime_diag(&rt, "hello"), f.diag_calls == 1 &&
           strcmp(f.last_diag, "hello") == 0));
    check("[runtime] NULL diag text is ignored",
          (sweep_runtime_diag(&rt, NULL), f.diag_calls == 1));

    /* Bounded read: backend bytes are copied and NUL terminated. */
    f.payload = "abc";
    f.payload_len = 3u;
    memset(buf, 'x', sizeof(buf));
    len = 99u;
    check("[runtime] read copies bytes and reports length",
          sweep_runtime_read_resource(&rt, "file", buf, sizeof(buf), &len) &&
          len == 3u && strcmp(buf, "abc") == 0 && f.read_calls == 1);

    check("[runtime] read rejects capacity below 2",
          !sweep_runtime_read_resource(&rt, "file", buf, 1u, &len) &&
          len == 0u && f.read_calls == 1);
    check("[runtime] read rejects NULL path",
          !sweep_runtime_read_resource(&rt, NULL, buf, sizeof(buf), &len));
    check("[runtime] read rejects NULL output",
          !sweep_runtime_read_resource(&rt, "file", NULL, sizeof(buf), &len));

    f.payload = "0123456789";
    f.payload_len = 10u;
    check("[runtime] read rejects buffer overflow",
          !sweep_runtime_read_resource(&rt, "file", buf, 5u, &len) &&
          len == 0u);

    f.force_read_overrun = 1;
    check("[runtime] backend contract violation rejected",
          !sweep_runtime_read_resource(&rt, "file", buf, 5u, &len) &&
          len == 0u);
    f.force_read_overrun = 0;

    f.force_read_failure = 1;
    check("[runtime] backend failure propagates",
          !sweep_runtime_read_resource(&rt, "file", buf, sizeof(buf), &len) &&
          len == 0u);
    f.force_read_failure = 0;

    /* Unbound runtime: every routed op fails without touching a backend. */
    {
        SWEEP_RUNTIME dead;
        int calls = f.alloc_calls + f.free_calls + f.read_calls + f.diag_calls;
        memset(&dead, 0, sizeof(dead));
        check("[runtime] unbound alloc fails",
              sweep_runtime_alloc(&dead, 4u) == NULL);
        check("[runtime] unbound realloc fails",
              sweep_runtime_realloc(&dead, s_arena, 4u) == NULL);
        check("[runtime] unbound mono clock is zero",
              sweep_runtime_monotonic_ms(&dead) == 0u);
        check("[runtime] unbound read fails",
              !sweep_runtime_read_resource(&dead, "file", buf, sizeof(buf),
                                           NULL));
        sweep_runtime_diag(&dead, "noop");
        sweep_runtime_free(&dead, s_arena);
        check("[runtime] unbound runtime calls no backend",
              calls == f.alloc_calls + f.free_calls + f.read_calls +
                       f.diag_calls);
    }

    sweep_runtime_shutdown(&rt);
}

static void test_deadline(void) {
    check("[runtime] deadline reached at equality",
          sweep_runtime_deadline_reached(10u, 10u));
    check("[runtime] deadline reached past",
          sweep_runtime_deadline_reached(11u, 10u));
    check("[runtime] deadline not reached before",
          !sweep_runtime_deadline_reached(9u, 10u));
    check("[runtime] UINT64_MAX deadline never wraps",
          !sweep_runtime_deadline_reached(0u, UINT64_MAX) &&
          sweep_runtime_deadline_reached(UINT64_MAX, UINT64_MAX));
}

static void write_fixture(const char* name, const char* text) {
    char path[256];
    FILE* fp;
    snprintf(path, sizeof(path), "build-generated/runtime_fixtures/%s", name);
    fp = fopen(path, "wb");
    if (fp == NULL) return;
    (void) fwrite(text, 1u, strlen(text), fp);
    (void) fclose(fp);
}

static void test_host_backend(void) {
    SWEEP_RUNTIME rt;
    char buf[64];
    size_t len = 0u;
    void* p;

    check("[runtime] hosted ops are available",
          sweep_runtime_host_ops() != NULL);
    memset(&rt, 0, sizeof(rt));
    check("[runtime] hosted runtime initializes",
          sweep_runtime_init(&rt, sweep_runtime_host_ops(), NULL) &&
          rt.initialized);

    p = sweep_runtime_alloc(&rt, 64u);
    check("[runtime] hosted alloc returns memory", p != NULL);
    check("[runtime] hosted realloc grows memory",
          sweep_runtime_realloc(&rt, p, 128u) != NULL);
    sweep_runtime_free(&rt, p);
    check("[runtime] hosted free succeeds", 1);

    {
        uint64_t a = sweep_runtime_monotonic_ms(&rt);
        uint64_t b = sweep_runtime_monotonic_ms(&rt);
        check("[runtime] hosted clock is monotonic", b >= a);
    }

    (void) mkdir("build-generated");
    (void) mkdir("build-generated/runtime_fixtures");
    write_fixture("host.txt", "host-bytes");

    check("[runtime] hosted read loads a file",
          sweep_runtime_read_resource(&rt, "build-generated/runtime_fixtures/host.txt",
                                      buf, sizeof(buf), &len) &&
          len == strlen("host-bytes") && strcmp(buf, "host-bytes") == 0);
    check("[runtime] hosted read rejects missing file",
          !sweep_runtime_read_resource(&rt, "build-generated/runtime_fixtures/nope.txt",
                                       buf, sizeof(buf), &len) && len == 0u);
    check("[runtime] hosted read rejects over-cap file",
          !sweep_runtime_read_resource(&rt, "build-generated/runtime_fixtures/host.txt",
                                       buf, 5u, &len) && len == 0u);
    check("[runtime] hosted read accepts exact capacity",
          sweep_runtime_read_resource(&rt, "build-generated/runtime_fixtures/host.txt",
                                      buf, strlen("host-bytes") + 1u, &len) &&
          len == strlen("host-bytes"));

    check("[runtime] legacy host runtime is available",
          sweep_runtime_host_default() != NULL &&
          sweep_runtime_host_default()->initialized);

    sweep_runtime_shutdown(&rt);
}

static void test_resource_via_runtime(void) {
    SWEEP_RUNTIME rt;
    SWEEP_RUNTIME_OPS ops;
    FAKE_RT f;
    SWEEP_RESOURCE_ROOT root;
    char out[64];
    char mime[32];
    size_t out_len = 0u;

    memset(&ops, 0, sizeof(ops));
    memset(&f, 0, sizeof(f));
    fake_ops(&ops);
    memset(&rt, 0, sizeof(rt));
    (void) sweep_runtime_init(&rt, &ops, &f);
    check("[runtime] resource root init",
          sweep_resource_root_init(&root, "fixtures"));

    f.payload = "body{}";
    f.payload_len = 6u;
    check("[runtime] resource fetch uses injected runtime",
          sweep_resource_fetch_runtime(&rt, &root, "site.css", out, sizeof(out),
                                       &out_len, mime, sizeof(mime)) &&
          strcmp(out, "body{}") == 0 && out_len == 6u &&
          strcmp(mime, "text/css") == 0 && f.read_calls == 1);

    check("[runtime] resource fetch rejects traversal before reading",
          !sweep_resource_fetch_runtime(&rt, &root, "../site.css", out,
                                       sizeof(out), &out_len, mime,
                                       sizeof(mime)) &&
          f.read_calls == 1);

    check("[runtime] resource fetch rejects over-cap payload",
          !sweep_resource_fetch_runtime(&rt, &root, "site.css", out, 4u,
                                       &out_len, mime, sizeof(mime)));

    f.force_read_failure = 1;
    check("[runtime] resource fetch propagates read failure",
          !sweep_resource_fetch_runtime(&rt, &root, "site.css", out,
                                       sizeof(out), &out_len, mime,
                                       sizeof(mime)));
    f.force_read_failure = 0;

    /* Unknown extension: ASCII sniffs as text/plain, binary is rejected. */
    f.payload = "plain";
    f.payload_len = 5u;
    check("[runtime] unknown ASCII extension sniffs as text/plain",
          sweep_resource_fetch_runtime(&rt, &root, "blob.dat", out, sizeof(out),
                                       &out_len, mime, sizeof(mime)) &&
          strcmp(mime, "text/plain") == 0);
    f.payload = "\x01\x02";
    f.payload_len = 2u;
    check("[runtime] unknown binary extension rejected",
          !sweep_resource_fetch_runtime(&rt, &root, "blob.dat", out, sizeof(out),
                                       &out_len, mime, sizeof(mime)));

    {
        SWEEP_RUNTIME dead;
        memset(&dead, 0, sizeof(dead));
        check("[runtime] resource fetch rejects unbound runtime",
              !sweep_resource_fetch_runtime(&dead, &root, "site.css", out,
                                           sizeof(out), &out_len, mime,
                                           sizeof(mime)));
        check("[runtime] resource fetch rejects NULL runtime",
              !sweep_resource_fetch_runtime(NULL, &root, "site.css", out,
                                           sizeof(out), &out_len, mime,
                                           sizeof(mime)));
    }

    sweep_runtime_shutdown(&rt);
}

int main(void) {
    printf("=== Sweep runtime seam tests ===\n");
    test_binding();
    test_routing();
    test_deadline();
    test_host_backend();
    test_resource_via_runtime();
    printf("--------------------------------\n");
    printf("passed: %d, failed: %d\n", g_passed, g_failures);
    return g_failures ? 1 : 0;
}
