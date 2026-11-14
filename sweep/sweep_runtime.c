/* sweep_runtime.c - validation and routing for the Sweep runtime seam. */
#include "sweep_runtime.h"

static bool runtime_ready(const SWEEP_RUNTIME* rt) {
    return rt != NULL && rt->initialized && rt->ops != NULL;
}

bool sweep_runtime_init(SWEEP_RUNTIME* rt, const SWEEP_RUNTIME_OPS* ops,
                        void* user) {
    if (rt == NULL || ops == NULL) return false;
    /* The smallest set that can serve a bounded engine: storage plus time.
     * Diagnostics/resources/init/shutdown stay optional so a timing-only host
     * is not forced to stub them. */
    if (ops->alloc == NULL || ops->free == NULL || ops->monotonic_ms == NULL) {
        return false;
    }
    rt->ops = ops;
    rt->user = user;
    rt->initialized = false;
    if (ops->init != NULL && !ops->init(user)) {
        rt->ops = NULL;
        rt->user = NULL;
        return false;
    }
    rt->initialized = true;
    return true;
}

void sweep_runtime_shutdown(SWEEP_RUNTIME* rt) {
    if (!runtime_ready(rt)) {
        if (rt != NULL) {
            rt->ops = NULL;
            rt->user = NULL;
            rt->initialized = false;
        }
        return;
    }
    if (rt->ops->shutdown != NULL) rt->ops->shutdown(rt->user);
    rt->ops = NULL;
    rt->user = NULL;
    rt->initialized = false;
}

void* sweep_runtime_alloc(SWEEP_RUNTIME* rt, size_t size) {
    if (!runtime_ready(rt) || rt->ops->alloc == NULL || size == 0u) return NULL;
    return rt->ops->alloc(rt->user, size);
}

void* sweep_runtime_realloc(SWEEP_RUNTIME* rt, void* ptr, size_t size) {
    if (!runtime_ready(rt)) return NULL;
    if (ptr == NULL) return sweep_runtime_alloc(rt, size);
    if (size == 0u) {
        sweep_runtime_free(rt, ptr);
        return NULL;
    }
    /* No realloc operation: fail without touching ptr.  The caller still owns
     * the original block, which is the only safe outcome we can offer. */
    if (rt->ops->realloc == NULL) return NULL;
    return rt->ops->realloc(rt->user, ptr, size);
}

void sweep_runtime_free(SWEEP_RUNTIME* rt, void* ptr) {
    if (ptr == NULL) return;
    if (!runtime_ready(rt) || rt->ops->free == NULL) return;
    rt->ops->free(rt->user, ptr);
}

uint64_t sweep_runtime_monotonic_ms(SWEEP_RUNTIME* rt) {
    if (!runtime_ready(rt) || rt->ops->monotonic_ms == NULL) return 0u;
    return rt->ops->monotonic_ms(rt->user);
}

void sweep_runtime_diag(SWEEP_RUNTIME* rt, const char* text) {
    if (text == NULL) return;
    if (!runtime_ready(rt) || rt->ops->diag == NULL) return;
    rt->ops->diag(rt->user, text);
}

bool sweep_runtime_read_resource(SWEEP_RUNTIME* rt, const char* path,
                                 char* out, size_t out_cap,
                                 size_t* out_len) {
    size_t produced = 0u;

    if (out_len != NULL) *out_len = 0u;
    if (!runtime_ready(rt) || rt->ops->read_resource == NULL) return false;
    if (path == NULL || out == NULL || out_cap < 2u) return false;

    if (!rt->ops->read_resource(rt->user, path, out, out_cap, &produced)) {
        return false;
    }
    /* Defend the caller even if a backend ignores its contract: never accept a
     * length that overruns the buffer or a missing terminator. */
    if (produced >= out_cap) return false;
    out[produced] = '\0';
    if (out_len != NULL) *out_len = produced;
    return true;
}

bool sweep_runtime_deadline_reached(uint64_t now_ms, uint64_t deadline_ms) {
    return now_ms >= deadline_ms;
}
