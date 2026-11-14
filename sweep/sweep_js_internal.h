/* sweep_js_internal.h - shared definitions for the Sweep JS subsystem.
 * Not part of the public browser API.
 */
#ifndef SWEEP_JS_INTERNAL_H
#define SWEEP_JS_INTERNAL_H

#include "sweep_js.h"
#include "sweep_js_engine.h"

typedef struct _SWEEP_JS_LISTENER {
    bool active;
    uint32_t target;   /* DOM node id */
    uint32_t generation; /* DOM slot generation at registration; stale
                            listeners (node reclaimed + slot reused) are
                            dropped at dispatch, never fired on a new node */
    char type[SWEEP_JS_EVENT_TYPE_MAX];
    bool capture;
    uint32_t serial;   /* registration order; used to freeze dispatch sets */
    int slot;          /* stash array slot anchoring the function */
} SWEEP_JS_LISTENER;

struct SWEEP_JS_RUNTIME {
    bool initialized;
    SWEEP_JS_RUNTIME_CONFIG config;
    SWEEP_JS_CONTEXT* contexts[SWEEP_JS_CONTEXTS_MAX];
};

struct SWEEP_JS_CONTEXT {
    bool in_use;
    SWEEP_JS_RUNTIME* runtime;
    SWEEP_JS_ENGINE* engine;
    SWEEP_DOM* dom;
    char last_error[SWEEP_JS_ERROR_MAX];
    SWEEP_JS_LISTENER listeners[SWEEP_JS_LISTENERS_MAX];
    uint32_t listener_serial;
    uint32_t dispatch_depth;
    uint32_t event_errors;
    bool globals_installed;
};

void sweep_js_set_error(SWEEP_JS_CONTEXT* context, const char* message);

/* Explicit engine-string boundary.  `make_dom_text` converts an engine
 * (CESU-8/extended UTF-8) string to DOM-side bytes; `make_engine_text` does the
 * reverse.  Both honour the context text mode. */
bool sweep_js_make_dom_text(SWEEP_JS_CONTEXT* context, const char* in,
                            size_t in_len, char* out, size_t out_cap,
                            size_t* out_len);
bool sweep_js_make_engine_text(SWEEP_JS_CONTEXT* context, const char* in,
                               size_t in_len, char* out, size_t out_cap,
                               size_t* out_len);

/* Convert a DOM-side C string and push it as a JS string. */
bool sweep_js_push_dom_text(SWEEP_JS_CONTEXT* context, const char* dom_text);

#endif /* SWEEP_JS_INTERNAL_H */
