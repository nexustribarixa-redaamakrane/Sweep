/* sweep_js_engine.h - engine adapter boundary for the Sweep JavaScript runtime.
 *
 * This header intentionally contains no engine-specific types.  The selected
 * engine (Duktape 2.7.0) lives only behind sweep_js_engine.c so it can be
 * replaced without rewriting DOM/event/browser code.
 *
 * Index conventions follow the adapter, not the engine: non-negative indices
 * are absolute stack slots, negative indices are relative to the value stack
 * top (-1 is the top value).
 */
#ifndef SWEEP_JS_ENGINE_H
#define SWEEP_JS_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SWEEP_JS_ENGINE SWEEP_JS_ENGINE;

/* Return number of values pushed by a native callback (normally 0 or 1).
 * A callback reports an error by calling one of the throw helpers; those do
 * not return to the callback. */
typedef int (*SWEEP_JS_ENGINE_CFUNC)(SWEEP_JS_ENGINE* engine);

#define SWEEP_JS_ENGINE_DEFAULT_MEMORY    (1024u * 1024u)
#define SWEEP_JS_ENGINE_DEFAULT_TIMEOUT_MS 2000u
#define SWEEP_JS_ENGINE_ERROR_MAX         256u

typedef struct _SWEEP_JS_ENGINE_CONFIG {
    size_t   memory_limit; /* 0 = engine default; enforced in the allocator */
    uint32_t timeout_ms;   /* 0 = no execution deadline for an evaluation */
    void*    user_data;    /* opaque browser/context pointer */
} SWEEP_JS_ENGINE_CONFIG;

bool sweep_js_engine_create(const SWEEP_JS_ENGINE_CONFIG* config,
                            SWEEP_JS_ENGINE** out_engine);
void sweep_js_engine_destroy(SWEEP_JS_ENGINE* engine);

void* sweep_js_engine_user_data(const SWEEP_JS_ENGINE* engine);
size_t sweep_js_engine_memory_used(const SWEEP_JS_ENGINE* engine);
size_t sweep_js_engine_memory_limit(const SWEEP_JS_ENGINE* engine);
bool sweep_js_engine_timed_out(const SWEEP_JS_ENGINE* engine);
const char* sweep_js_engine_last_error(const SWEEP_JS_ENGINE* engine);

/* Evaluate UTF-8/CESU-8 source.  On success value_out receives a bounded,
 * stringified result (or an empty string when value_out is NULL/0) and
 * out_is_undefined (optional) tells whether the raw result was undefined.
 * On failure error_out receives a bounded diagnostic and false is returned. */
bool sweep_js_engine_eval(SWEEP_JS_ENGINE* engine, const char* source,
                          size_t source_len, char* value_out,
                          size_t value_cap, char* error_out, size_t error_cap,
                          bool* out_is_undefined);

/* ---- value-stack helpers used by DOM/event bindings ---- */
int  sweep_js_engine_get_top(SWEEP_JS_ENGINE* engine);
void sweep_js_engine_set_top(SWEEP_JS_ENGINE* engine, int top);

void sweep_js_engine_push_undefined(SWEEP_JS_ENGINE* engine);
void sweep_js_engine_push_null(SWEEP_JS_ENGINE* engine);
void sweep_js_engine_push_boolean(SWEEP_JS_ENGINE* engine, bool value);
void sweep_js_engine_push_number(SWEEP_JS_ENGINE* engine, double value);
void sweep_js_engine_push_string(SWEEP_JS_ENGINE* engine, const char* value);
void sweep_js_engine_push_lstring(SWEEP_JS_ENGINE* engine, const char* value,
                                  size_t len);
void sweep_js_engine_push_global_object(SWEEP_JS_ENGINE* engine);
void sweep_js_engine_push_this(SWEEP_JS_ENGINE* engine);
void sweep_js_engine_push_object(SWEEP_JS_ENGINE* engine);
void sweep_js_engine_push_array(SWEEP_JS_ENGINE* engine);
void sweep_js_engine_dup(SWEEP_JS_ENGINE* engine, int index);
void sweep_js_engine_remove(SWEEP_JS_ENGINE* engine, int index);

bool sweep_js_engine_get_boolean(SWEEP_JS_ENGINE* engine, int index);
double sweep_js_engine_get_number(SWEEP_JS_ENGINE* engine, int index);
const char* sweep_js_engine_get_string(SWEEP_JS_ENGINE* engine, int index);
const char* sweep_js_engine_get_lstring(SWEEP_JS_ENGINE* engine, int index,
                                        size_t* out_len);
/* Coercing variants used for DOM argument validation.  They run ES ToString /
 * ToNumber, so they may execute user code; callers are already inside a
 * bounded evaluation. */
const char* sweep_js_engine_to_lstring(SWEEP_JS_ENGINE* engine, int index,
                                       size_t* out_len);
double sweep_js_engine_to_number(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_is_undefined(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_is_null(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_is_nullish(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_is_object(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_is_function(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_is_string(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_is_array(SWEEP_JS_ENGINE* engine, int index);
bool sweep_js_engine_strict_equals(SWEEP_JS_ENGINE* engine, int a, int b);
size_t sweep_js_engine_get_length(SWEEP_JS_ENGINE* engine, int index);

bool sweep_js_engine_get_prop_string(SWEEP_JS_ENGINE* engine, int object_index,
                                     const char* key);
bool sweep_js_engine_put_prop_string(SWEEP_JS_ENGINE* engine, int object_index,
                                     const char* key);
bool sweep_js_engine_has_prop_string(SWEEP_JS_ENGINE* engine, int object_index,
                                     const char* key);
bool sweep_js_engine_delete_prop_string(SWEEP_JS_ENGINE* engine,
                                        int object_index, const char* key);
bool sweep_js_engine_get_prop_index(SWEEP_JS_ENGINE* engine, int object_index,
                                    uint32_t index);
bool sweep_js_engine_put_prop_index(SWEEP_JS_ENGINE* engine, int object_index,
                                    uint32_t index);

/* Global stash is not reachable from script.  The value is popped by put. */
bool sweep_js_engine_get_stash(SWEEP_JS_ENGINE* engine, const char* key);
bool sweep_js_engine_put_stash(SWEEP_JS_ENGINE* engine, const char* key);

void sweep_js_engine_push_c_function(SWEEP_JS_ENGINE* engine,
                                     SWEEP_JS_ENGINE_CFUNC fn, int nargs);
void sweep_js_engine_set_global_c_function(SWEEP_JS_ENGINE* engine,
                                           const char* name,
                                           SWEEP_JS_ENGINE_CFUNC fn, int nargs);
/* Defines a getter/setter pair on object_index.  Getter/setter may be NULL.
 * Returns false if the property could not be defined. */
bool sweep_js_engine_def_accessor(SWEEP_JS_ENGINE* engine, int object_index,
                                  const char* key, SWEEP_JS_ENGINE_CFUNC getter,
                                  SWEEP_JS_ENGINE_CFUNC setter);

/* Object plumbing for wrapper prototypes. */
bool sweep_js_engine_set_prototype(SWEEP_JS_ENGINE* engine, int object_index,
                                   int prototype_index);
/* Define a non-writable, non-enumerable, non-configurable own property. */
bool sweep_js_engine_def_hidden_number(SWEEP_JS_ENGINE* engine, int object_index,
                                       const char* key, double value);
bool sweep_js_engine_def_hidden_boolean(SWEEP_JS_ENGINE* engine, int object_index,
                                        const char* key, bool value);
/* Consume the value on top of the stack and store it as a global. */
bool sweep_js_engine_put_global_string(SWEEP_JS_ENGINE* engine, const char* name);

/* Protected call.  Stack layout: [ ... func arg1 ... argN ] for pcall,
 * [ ... func this arg1 ... argN ] for pcall_method.  Returns 0 on success and
 * leaves the return value on the stack; non-zero on error and leaves the error
 * value on the stack. */
int sweep_js_engine_pcall(SWEEP_JS_ENGINE* engine, int nargs);
int sweep_js_engine_pcall_method(SWEEP_JS_ENGINE* engine, int nargs);

/* These do not return.  Declared returning int so callers can write
 * `return sweep_js_engine_throw_type_error(...)` if they prefer. */
int sweep_js_engine_throw_type_error(SWEEP_JS_ENGINE* engine, const char* msg);
int sweep_js_engine_throw_range_error(SWEEP_JS_ENGINE* engine, const char* msg);
int sweep_js_engine_throw_reference_error(SWEEP_JS_ENGINE* engine, const char* msg);
int sweep_js_engine_throw_error(SWEEP_JS_ENGINE* engine, const char* msg);

#endif /* SWEEP_JS_ENGINE_H */
