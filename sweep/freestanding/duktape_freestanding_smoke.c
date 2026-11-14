/* duktape_freestanding_smoke.c - freestanding Duktape smoke test.
 *
 * Links against the freestanding Duktape object, the Sweep freestanding libc
 * shim and the arena allocator.  It uses Duktape's explicit allocator hooks
 * (duk_create_heap) bound to SWEEP_RUNTIME, never duk_create_heap_default, and
 * contains no stdio/heap/file dependency of its own.
 *
 * Returns 0 on success and writes a short status into out.  The host harness in
 * freestanding_host_main.c is the only part that prints, using the hosted
 * runtime backend.
 */
#include <stdbool.h>
#include <stddef.h>

#include "duktape.h"
#include "sweep_fs_arena.h"
#include "sweep_runtime.h"

/* Duktape allocator hooks routed through the injected Sweep runtime. */
static void* fs_duk_alloc(void* udata, duk_size_t size) {
    return sweep_runtime_alloc((SWEEP_RUNTIME*) udata, (size_t) size);
}

static void* fs_duk_realloc(void* udata, void* ptr, duk_size_t size) {
    return sweep_runtime_realloc((SWEEP_RUNTIME*) udata, ptr, (size_t) size);
}

static void fs_duk_free(void* udata, void* ptr) {
    sweep_runtime_free((SWEEP_RUNTIME*) udata, ptr);
}

static void copy_text(char* out, size_t cap, const char* text) {
    size_t i = 0u;
    if (out == NULL || cap == 0u) return;
    while (text != NULL && text[i] != '\0' && i + 1u < cap) {
        out[i] = text[i];
        ++i;
    }
    out[i] = '\0';
}

int sweep_fs_duktape_smoke(SWEEP_RUNTIME* rt, char* out, size_t out_cap) {
    static const char SCRIPT[] =
        "(function () {"
        "  var a = [1, 2, 3].map(function (x) { return x * 2; });"
        "  var o = { n: 0 };"
        "  for (var i = 0; i < 10; i++) { o.n += i; }"
        "  var ok = a.join(',') === '2,4,6' && o.n === 45;"
        "  ok = ok && JSON.parse('{\"x\":1}').x === 1;"
        "  ok = ok && /ab+c/.test('abbbc');"
        "  ok = ok && (function () {"
        "      try { null.x; } catch (e) { return e instanceof TypeError; }"
        "      return false;"
        "  })();"
        "  ok = ok && (0.1 + 0.2) > 0.3;"
        "  ok = ok && String(123) === '123';"
        "  ok = ok && typeof Date.now() === 'number';"
        "  return ok ? 'ok' : 'bad';"
        "})()";
    duk_context* ctx;

    if (rt == NULL || !rt->initialized) {
        copy_text(out, out_cap, "runtime-unbound");
        return 1;
    }

    ctx = duk_create_heap(fs_duk_alloc, fs_duk_realloc, fs_duk_free, rt, NULL);
    if (ctx == NULL) {
        copy_text(out, out_cap, "heap-create-failed");
        return 2;
    }

    if (duk_peval_string(ctx, SCRIPT) != 0) {
        copy_text(out, out_cap, duk_safe_to_string(ctx, -1));
        duk_destroy_heap(ctx);
        return 3;
    }

    {
        const char* result = duk_safe_to_string(ctx, -1);
        int ok = (result != NULL && result[0] == 'o' && result[1] == 'k' &&
                  result[2] == '\0');
        copy_text(out, out_cap, ok ? "ok" : (result ? result : "null-result"));
        duk_pop(ctx);
        duk_destroy_heap(ctx);
        return ok ? 0 : 4;
    }
}
