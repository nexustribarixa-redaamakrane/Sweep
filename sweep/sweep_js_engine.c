/* sweep_js_engine.c - Duktape 2.7.0 adapter for the Sweep JavaScript runtime.
 *
 * This is the only Sweep translation unit that includes the engine headers.
 * It provides an opaque value-stack API, a custom allocator with a hard memory
 * ceiling, and a wall-clock execution deadline wired through Duktape's
 * interrupt counter (see third_party/duktape/duk_config.h).
 */
#include "sweep_js_engine.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "third_party/duktape/duktape.h"

#define SWEEP_JS_ENGINE_CFUNCS_MAX 96

typedef union SWEEP_JS_ALLOC_HEADER {
    size_t size;
    long double align_ld;
    void* align_ptr;
} SWEEP_JS_ALLOC_HEADER;

struct SWEEP_JS_ENGINE {
    duk_context* ctx;
    SWEEP_JS_ENGINE_CONFIG config;
    size_t memory_used;
    size_t memory_limit;
    char last_error[SWEEP_JS_ENGINE_ERROR_MAX];
    bool timed_out;
    bool deadline_active;
    clock_t deadline_ticks;
    SWEEP_JS_ENGINE_CFUNC cfuns[SWEEP_JS_ENGINE_CFUNCS_MAX];
    uint32_t cfunc_count;
};

static void copy_bounded(char* out, size_t cap, const char* src) {
    size_t n;
    if (!out || cap == 0u) return;
    if (!src) { out[0] = '\0'; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(out, src, n);
    out[n] = '\0';
}

static void set_error(SWEEP_JS_ENGINE* engine, const char* msg) {
    if (!engine) return;
    copy_bounded(engine->last_error, sizeof(engine->last_error), msg);
}

static void* sweep_js_alloc(void* udata, duk_size_t size) {
    SWEEP_JS_ENGINE* engine = (SWEEP_JS_ENGINE*) udata;
    SWEEP_JS_ALLOC_HEADER* hdr;
    size_t total;
    if (size == 0u) size = 1u;
    total = sizeof(SWEEP_JS_ALLOC_HEADER) + (size_t) size;
    if (engine->memory_limit != 0u &&
        (total > engine->memory_limit ||
         engine->memory_used > engine->memory_limit - total)) {
        return NULL;
    }
    hdr = (SWEEP_JS_ALLOC_HEADER*) malloc(total);
    if (!hdr) return NULL;
    hdr->size = (size_t) size;
    engine->memory_used += total;
    return (void*) (hdr + 1);
}

static void sweep_js_free(void* udata, void* ptr) {
    SWEEP_JS_ENGINE* engine = (SWEEP_JS_ENGINE*) udata;
    SWEEP_JS_ALLOC_HEADER* hdr;
    if (!ptr) return;
    hdr = ((SWEEP_JS_ALLOC_HEADER*) ptr) - 1;
    if (engine->memory_used >= sizeof(SWEEP_JS_ALLOC_HEADER) + hdr->size) {
        engine->memory_used -= sizeof(SWEEP_JS_ALLOC_HEADER) + hdr->size;
    } else {
        engine->memory_used = 0u;
    }
    free(hdr);
}

static void* sweep_js_realloc(void* udata, void* ptr, duk_size_t size) {
    SWEEP_JS_ENGINE* engine = (SWEEP_JS_ENGINE*) udata;
    SWEEP_JS_ALLOC_HEADER* hdr;
    size_t old_total, new_total;
    if (!ptr) return sweep_js_alloc(udata, size);
    if (size == 0u) { sweep_js_free(udata, ptr); return NULL; }
    hdr = ((SWEEP_JS_ALLOC_HEADER*) ptr) - 1;
    old_total = sizeof(SWEEP_JS_ALLOC_HEADER) + hdr->size;
    new_total = sizeof(SWEEP_JS_ALLOC_HEADER) + (size_t) size;
    if (engine->memory_limit != 0u && new_total > engine->memory_limit) {
        return NULL;
    }
    if (new_total > old_total &&
        engine->memory_limit != 0u &&
        engine->memory_used > engine->memory_limit - (new_total - old_total)) {
        return NULL;
    }
    hdr = (SWEEP_JS_ALLOC_HEADER*) realloc(hdr, new_total);
    if (!hdr) return NULL;
    if (new_total >= old_total) engine->memory_used += new_total - old_total;
    else engine->memory_used -= old_total - new_total;
    hdr->size = (size_t) size;
    return (void*) (hdr + 1);
}

/* Called by Duktape's interrupt handler through DUK_USE_EXEC_TIMEOUT_CHECK. */
int sweep_js_engine_timeout_check(void* udata) {
    SWEEP_JS_ENGINE* engine = (SWEEP_JS_ENGINE*) udata;
    if (!engine || !engine->deadline_active) return 0;
    return clock() >= engine->deadline_ticks ? 1 : 0;
}

static SWEEP_JS_ENGINE* engine_from_context(duk_context* ctx) {
    SWEEP_JS_ENGINE* engine = NULL;
    duk_push_global_stash(ctx);
    if (duk_get_prop_string(ctx, -1, "__sweep_engine_ptr")) {
        engine = (SWEEP_JS_ENGINE*) duk_get_pointer(ctx, -1);
    }
    duk_pop_2(ctx);
    return engine;
}

static duk_ret_t sweep_js_trampoline(duk_context* ctx) {
    SWEEP_JS_ENGINE* engine = engine_from_context(ctx);
    duk_int_t id;
    if (!engine) return DUK_RET_TYPE_ERROR;
    duk_push_current_function(ctx);
    duk_get_prop_string(ctx, -1, "__sweep_cfunc_id");
    id = duk_get_int(ctx, -1);
    duk_pop_2(ctx);
    if (id < 0 || (uint32_t) id >= engine->cfunc_count) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR, "invalid native callback");
    }
    return (duk_ret_t) engine->cfuns[id](engine);
}

bool sweep_js_engine_create(const SWEEP_JS_ENGINE_CONFIG* config,
                            SWEEP_JS_ENGINE** out_engine) {
    SWEEP_JS_ENGINE* engine;
    duk_context* ctx;
    if (!out_engine) return false;
    *out_engine = NULL;
    engine = (SWEEP_JS_ENGINE*) calloc(1, sizeof(*engine));
    if (!engine) return false;
    if (config) engine->config = *config;
    if (engine->config.memory_limit == 0u) {
        engine->config.memory_limit = SWEEP_JS_ENGINE_DEFAULT_MEMORY;
    }
    if (engine->config.timeout_ms == 0u) {
        engine->config.timeout_ms = SWEEP_JS_ENGINE_DEFAULT_TIMEOUT_MS;
    }
    engine->memory_limit = engine->config.memory_limit;
    ctx = duk_create_heap(sweep_js_alloc, sweep_js_realloc, sweep_js_free,
                          engine, NULL);
    if (!ctx) {
        free(engine);
        return false;
    }
    engine->ctx = ctx;

    /* The engine pointer is stored in the global stash, unreachable from
     * script, so native callbacks can recover their adapter. */
    duk_push_global_stash(ctx);
    duk_push_pointer(ctx, engine);
    (void) duk_put_prop_string(ctx, -2, "__sweep_engine_ptr");
    duk_pop(ctx);

    /* Hide the engine-specific Duktape object from scripts: the browser API
     * boundary is document + standard ECMAScript, not engine internals. */
    duk_push_global_object(ctx);
    (void) duk_del_prop_string(ctx, -1, "Duktape");
    duk_pop(ctx);

    *out_engine = engine;
    return true;
}

void sweep_js_engine_destroy(SWEEP_JS_ENGINE* engine) {
    if (!engine) return;
    if (engine->ctx) {
        engine->deadline_active = false;
        duk_destroy_heap(engine->ctx);
        engine->ctx = NULL;
    }
    free(engine);
}

void* sweep_js_engine_user_data(const SWEEP_JS_ENGINE* engine) {
    return engine ? engine->config.user_data : NULL;
}

size_t sweep_js_engine_memory_used(const SWEEP_JS_ENGINE* engine) {
    return engine ? engine->memory_used : 0u;
}

size_t sweep_js_engine_memory_limit(const SWEEP_JS_ENGINE* engine) {
    return engine ? engine->memory_limit : 0u;
}

bool sweep_js_engine_timed_out(const SWEEP_JS_ENGINE* engine) {
    return engine ? engine->timed_out : false;
}

const char* sweep_js_engine_last_error(const SWEEP_JS_ENGINE* engine) {
    return engine ? engine->last_error : "";
}

bool sweep_js_engine_eval(SWEEP_JS_ENGINE* engine, const char* source,
                          size_t source_len, char* value_out,
                          size_t value_cap, char* error_out, size_t error_cap,
                          bool* out_is_undefined) {
    duk_int_t rc;
    const char* text;
    if (!engine || !engine->ctx) return false;
    if (value_out && value_cap) value_out[0] = '\0';
    if (error_out && error_cap) error_out[0] = '\0';
    if (out_is_undefined) *out_is_undefined = true;
    engine->last_error[0] = '\0';
    engine->timed_out = false;
    duk_set_top(engine->ctx, 0);
    engine->deadline_active = false;
    if (engine->config.timeout_ms != 0u) {
        engine->deadline_ticks = clock() +
            (clock_t) (((double) engine->config.timeout_ms *
                        (double) CLOCKS_PER_SEC) / 1000.0);
        engine->deadline_active = true;
    }
    rc = duk_peval_lstring(engine->ctx, source ? source : "",
                           (duk_size_t) (source ? source_len : 0u));
    engine->deadline_active = false;
    if (rc != 0) {
        text = duk_safe_to_string(engine->ctx, -1);
        set_error(engine, text ? text : "JavaScript error");
        if (text && (strstr(text, "execution timeout") != NULL ||
                     strstr(text, "exec timeout") != NULL)) {
            engine->timed_out = true;
        }
        if (error_out && error_cap) {
            copy_bounded(error_out, error_cap, engine->last_error);
        }
        duk_set_top(engine->ctx, 0);
        return false;
    }
    if (out_is_undefined) {
        *out_is_undefined = duk_is_undefined(engine->ctx, -1) != 0;
    }
    text = duk_safe_to_string(engine->ctx, -1);
    if (value_out && value_cap) {
        copy_bounded(value_out, value_cap, text ? text : "");
    }
    duk_set_top(engine->ctx, 0);
    return true;
}

int sweep_js_engine_get_top(SWEEP_JS_ENGINE* engine) {
    return engine && engine->ctx ? (int) duk_get_top(engine->ctx) : 0;
}

void sweep_js_engine_set_top(SWEEP_JS_ENGINE* engine, int top) {
    if (engine && engine->ctx && top >= 0) duk_set_top(engine->ctx, top);
}

void sweep_js_engine_push_undefined(SWEEP_JS_ENGINE* engine) {
    if (engine && engine->ctx) duk_push_undefined(engine->ctx);
}
void sweep_js_engine_push_null(SWEEP_JS_ENGINE* engine) {
    if (engine && engine->ctx) duk_push_null(engine->ctx);
}
void sweep_js_engine_push_boolean(SWEEP_JS_ENGINE* engine, bool value) {
    if (engine && engine->ctx) duk_push_boolean(engine->ctx, value ? 1 : 0);
}
void sweep_js_engine_push_number(SWEEP_JS_ENGINE* engine, double value) {
    if (engine && engine->ctx) duk_push_number(engine->ctx, value);
}
void sweep_js_engine_push_string(SWEEP_JS_ENGINE* engine, const char* value) {
    if (engine && engine->ctx) duk_push_string(engine->ctx, value ? value : "");
}
void sweep_js_engine_push_lstring(SWEEP_JS_ENGINE* engine, const char* value,
                                  size_t len) {
    if (engine && engine->ctx) {
        duk_push_lstring(engine->ctx, value ? value : "", (duk_size_t) len);
    }
}
void sweep_js_engine_push_global_object(SWEEP_JS_ENGINE* engine) {
    if (engine && engine->ctx) duk_push_global_object(engine->ctx);
}
void sweep_js_engine_push_this(SWEEP_JS_ENGINE* engine) {
    if (engine && engine->ctx) duk_push_this(engine->ctx);
}
void sweep_js_engine_push_object(SWEEP_JS_ENGINE* engine) {
    if (engine && engine->ctx) duk_push_object(engine->ctx);
}
void sweep_js_engine_push_array(SWEEP_JS_ENGINE* engine) {
    if (engine && engine->ctx) duk_push_array(engine->ctx);
}
void sweep_js_engine_dup(SWEEP_JS_ENGINE* engine, int index) {
    if (engine && engine->ctx) duk_dup(engine->ctx, index);
}
void sweep_js_engine_remove(SWEEP_JS_ENGINE* engine, int index) {
    if (engine && engine->ctx) duk_remove(engine->ctx, index);
}

bool sweep_js_engine_get_boolean(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_get_boolean(engine->ctx, index) != 0 : false;
}
double sweep_js_engine_get_number(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_get_number(engine->ctx, index) : 0.0;
}
const char* sweep_js_engine_get_string(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_get_string(engine->ctx, index) : NULL;
}
const char* sweep_js_engine_get_lstring(SWEEP_JS_ENGINE* engine, int index,
                                        size_t* out_len) {
    duk_size_t len = 0u;
    const char* p;
    if (out_len) *out_len = 0u;
    if (!engine || !engine->ctx) return NULL;
    p = duk_get_lstring(engine->ctx, index, &len);
    if (out_len) *out_len = (size_t) len;
    return p;
}
const char* sweep_js_engine_to_lstring(SWEEP_JS_ENGINE* engine, int index,
                                       size_t* out_len) {
    duk_size_t len = 0u;
    const char* p;
    if (out_len) *out_len = 0u;
    if (!engine || !engine->ctx) return NULL;
    p = duk_safe_to_lstring(engine->ctx, index, &len);
    if (out_len) *out_len = (size_t) len;
    return p;
}
double sweep_js_engine_to_number(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_to_number(engine->ctx, index) : 0.0;
}
bool sweep_js_engine_is_undefined(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_is_undefined(engine->ctx, index) != 0 : true;
}
bool sweep_js_engine_is_null(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_is_null(engine->ctx, index) != 0 : false;
}
bool sweep_js_engine_is_nullish(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_is_null_or_undefined(engine->ctx, index) != 0 : true;
}
bool sweep_js_engine_is_object(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_is_object(engine->ctx, index) != 0 : false;
}
bool sweep_js_engine_is_function(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_is_function(engine->ctx, index) != 0 : false;
}
bool sweep_js_engine_is_string(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_is_string(engine->ctx, index) != 0 : false;
}
bool sweep_js_engine_is_array(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? duk_is_array(engine->ctx, index) != 0 : false;
}
bool sweep_js_engine_strict_equals(SWEEP_JS_ENGINE* engine, int a, int b) {
    return engine && engine->ctx ? duk_strict_equals(engine->ctx, a, b) != 0 : false;
}
size_t sweep_js_engine_get_length(SWEEP_JS_ENGINE* engine, int index) {
    return engine && engine->ctx ? (size_t) duk_get_length(engine->ctx, index) : 0u;
}

bool sweep_js_engine_get_prop_string(SWEEP_JS_ENGINE* engine, int object_index,
                                     const char* key) {
    return engine && engine->ctx
        ? duk_get_prop_string(engine->ctx, object_index, key ? key : "") != 0
        : false;
}
bool sweep_js_engine_put_prop_string(SWEEP_JS_ENGINE* engine, int object_index,
                                     const char* key) {
    return engine && engine->ctx
        ? duk_put_prop_string(engine->ctx, object_index, key ? key : "") != 0
        : false;
}
bool sweep_js_engine_has_prop_string(SWEEP_JS_ENGINE* engine, int object_index,
                                     const char* key) {
    return engine && engine->ctx
        ? duk_has_prop_string(engine->ctx, object_index, key ? key : "") != 0
        : false;
}
bool sweep_js_engine_delete_prop_string(SWEEP_JS_ENGINE* engine,
                                        int object_index, const char* key) {
    return engine && engine->ctx
        ? duk_del_prop_string(engine->ctx, object_index, key ? key : "") != 0
        : false;
}
bool sweep_js_engine_get_prop_index(SWEEP_JS_ENGINE* engine, int object_index,
                                    uint32_t index) {
    return engine && engine->ctx
        ? duk_get_prop_index(engine->ctx, object_index, (duk_uarridx_t) index) != 0
        : false;
}
bool sweep_js_engine_put_prop_index(SWEEP_JS_ENGINE* engine, int object_index,
                                    uint32_t index) {
    return engine && engine->ctx
        ? duk_put_prop_index(engine->ctx, object_index, (duk_uarridx_t) index) != 0
        : false;
}

bool sweep_js_engine_get_stash(SWEEP_JS_ENGINE* engine, const char* key) {
    if (!engine || !engine->ctx) return false;
    duk_push_global_stash(engine->ctx);
    (void) duk_get_prop_string(engine->ctx, -1, key ? key : "");
    duk_remove(engine->ctx, -2);
    return true;
}

bool sweep_js_engine_put_stash(SWEEP_JS_ENGINE* engine, const char* key) {
    if (!engine || !engine->ctx) return false;
    duk_push_global_stash(engine->ctx);
    duk_dup(engine->ctx, -2);
    (void) duk_put_prop_string(engine->ctx, -2, key ? key : "");
    duk_pop_2(engine->ctx);
    return true;
}

/* Register a callback once and reuse its slot.  Without this, every wrapper
 * accessor would consume a slot and exhaust the bounded callback table. */
static int engine_cfunc_id(SWEEP_JS_ENGINE* engine, SWEEP_JS_ENGINE_CFUNC fn) {
    uint32_t i;
    for (i = 0u; i < engine->cfunc_count; ++i) {
        if (engine->cfuns[i] == fn) return (int) i;
    }
    if (engine->cfunc_count >= SWEEP_JS_ENGINE_CFUNCS_MAX) return -1;
    engine->cfuns[engine->cfunc_count] = fn;
    return (int) engine->cfunc_count++;
}

void sweep_js_engine_push_c_function(SWEEP_JS_ENGINE* engine,
                                     SWEEP_JS_ENGINE_CFUNC fn, int nargs) {
    int id;
    if (!engine || !engine->ctx || !fn) return;
    id = engine_cfunc_id(engine, fn);
    if (id < 0) {
        duk_push_undefined(engine->ctx);
        return;
    }
    duk_push_c_function(engine->ctx, sweep_js_trampoline, (duk_idx_t) nargs);
    (void) sweep_js_engine_def_hidden_number(engine, -1, "__sweep_cfunc_id",
                                             (double) id);
}

void sweep_js_engine_set_global_c_function(SWEEP_JS_ENGINE* engine,
                                           const char* name,
                                           SWEEP_JS_ENGINE_CFUNC fn, int nargs) {
    if (!engine || !engine->ctx) return;
    sweep_js_engine_push_c_function(engine, fn, nargs);
    (void) duk_put_global_string(engine->ctx, name ? name : "");
}

bool sweep_js_engine_def_accessor(SWEEP_JS_ENGINE* engine, int object_index,
                                  const char* key, SWEEP_JS_ENGINE_CFUNC getter,
                                  SWEEP_JS_ENGINE_CFUNC setter) {
    duk_idx_t obj;
    duk_uint_t flags;
    if (!engine || !engine->ctx || !key) return false;
    obj = duk_normalize_index(engine->ctx, (duk_idx_t) object_index);
    if (obj < 0) return false;
    flags = DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
            DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_HAVE_CONFIGURABLE |
            DUK_DEFPROP_CONFIGURABLE;
    duk_push_string(engine->ctx, key);
    if (getter) sweep_js_engine_push_c_function(engine, getter, 0);
    else duk_push_undefined(engine->ctx);
    if (setter) sweep_js_engine_push_c_function(engine, setter, 1);
    else duk_push_undefined(engine->ctx);
    duk_def_prop(engine->ctx, obj, flags);
    return true;
}

bool sweep_js_engine_set_prototype(SWEEP_JS_ENGINE* engine, int object_index,
                                   int prototype_index) {
    duk_idx_t obj;
    if (!engine || !engine->ctx) return false;
    obj = duk_normalize_index(engine->ctx, (duk_idx_t) object_index);
    if (obj < 0) return false;
    duk_dup(engine->ctx, (duk_idx_t) prototype_index);
    duk_set_prototype(engine->ctx, obj);
    return true;
}

bool sweep_js_engine_def_hidden_number(SWEEP_JS_ENGINE* engine, int object_index,
                                       const char* key, double value) {
    duk_idx_t obj;
    if (!engine || !engine->ctx || !key) return false;
    obj = duk_normalize_index(engine->ctx, (duk_idx_t) object_index);
    if (obj < 0) return false;
    duk_push_string(engine->ctx, key);
    duk_push_number(engine->ctx, value);
    duk_def_prop(engine->ctx, obj,
                 DUK_DEFPROP_HAVE_VALUE |
                 DUK_DEFPROP_HAVE_WRITABLE |
                 DUK_DEFPROP_HAVE_ENUMERABLE |
                 DUK_DEFPROP_HAVE_CONFIGURABLE);
    return true;
}

bool sweep_js_engine_def_hidden_boolean(SWEEP_JS_ENGINE* engine, int object_index,
                                        const char* key, bool value) {
    duk_idx_t obj;
    if (!engine || !engine->ctx || !key) return false;
    obj = duk_normalize_index(engine->ctx, (duk_idx_t) object_index);
    if (obj < 0) return false;
    duk_push_string(engine->ctx, key);
    duk_push_boolean(engine->ctx, value ? 1 : 0);
    duk_def_prop(engine->ctx, obj,
                 DUK_DEFPROP_HAVE_VALUE |
                 DUK_DEFPROP_HAVE_WRITABLE |
                 DUK_DEFPROP_HAVE_ENUMERABLE |
                 DUK_DEFPROP_HAVE_CONFIGURABLE);
    return true;
}

bool sweep_js_engine_put_global_string(SWEEP_JS_ENGINE* engine, const char* name) {
    if (!engine || !engine->ctx || !name) return false;
    duk_push_global_object(engine->ctx);
    duk_dup(engine->ctx, -2);
    (void) duk_put_prop_string(engine->ctx, -2, name);
    duk_remove(engine->ctx, -2);
    duk_pop(engine->ctx);
    return true;
}

int sweep_js_engine_pcall(SWEEP_JS_ENGINE* engine, int nargs) {
    if (!engine || !engine->ctx) return -1;
    return (int) duk_pcall(engine->ctx, (duk_idx_t) nargs);
}

int sweep_js_engine_pcall_method(SWEEP_JS_ENGINE* engine, int nargs) {
    if (!engine || !engine->ctx) return -1;
    return (int) duk_pcall_method(engine->ctx, (duk_idx_t) nargs);
}

int sweep_js_engine_throw_type_error(SWEEP_JS_ENGINE* engine, const char* msg) {
    if (!engine || !engine->ctx) return 0;
    return (int) duk_error(engine->ctx, DUK_ERR_TYPE_ERROR, "%s",
                          msg ? msg : "invalid operation");
}
int sweep_js_engine_throw_range_error(SWEEP_JS_ENGINE* engine, const char* msg) {
    if (!engine || !engine->ctx) return 0;
    return (int) duk_error(engine->ctx, DUK_ERR_RANGE_ERROR, "%s",
                          msg ? msg : "out of range");
}
int sweep_js_engine_throw_reference_error(SWEEP_JS_ENGINE* engine, const char* msg) {
    if (!engine || !engine->ctx) return 0;
    return (int) duk_error(engine->ctx, DUK_ERR_REFERENCE_ERROR, "%s",
                          msg ? msg : "invalid reference");
}
int sweep_js_engine_throw_error(SWEEP_JS_ENGINE* engine, const char* msg) {
    if (!engine || !engine->ctx) return 0;
    return (int) duk_error(engine->ctx, DUK_ERR_ERROR, "%s",
                          msg ? msg : "operation failed");
}
