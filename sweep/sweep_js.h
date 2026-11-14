/* sweep_js.h - Sweep JavaScript runtime (per-document contexts).
 *
 * Phase 3 embeds a real ECMAScript engine (Duktape 2.7.0) behind
 * sweep_js_engine.  This module owns bounded, independent per-document
 * execution contexts, installs the mediated browser API (document/Event), and
 * runs classic <script> elements.  It is host-test-only: Duktape needs a host
 * libc, so it is not part of the freestanding kernel image.
 *
 * Security boundary: scripts see only the globals installed here (document,
 * Event) plus built-in ECMAScript objects.  There is no KDB, kernel, physical
 * memory, filesystem, device or OpenWindows-management binding.
 *
 * External scripts are fetched only through SWEEP_JS_RESOURCE_FETCH.  There is
 * no HTTP/TCP/DNS/TLS/socket path in this tree, so "src" works only when the
 * embedding host registers an explicit local-resource provider.
 */
#ifndef SWEEP_JS_H
#define SWEEP_JS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sweep_html.h"
#include "sweep_text.h"

#define SWEEP_JS_CONTEXTS_MAX        8u
#define SWEEP_JS_LISTENERS_MAX      32u
#define SWEEP_JS_EVENT_TYPE_MAX     32u
#define SWEEP_JS_SOURCE_MAX      65536u
#define SWEEP_JS_RESULT_MAX       4096u
#define SWEEP_JS_ERROR_MAX         256u
#define SWEEP_JS_STRING_MAX       4096u
#define SWEEP_JS_DISPATCH_DEPTH_MAX  4u

typedef enum _SWEEP_JS_STATUS {
    SWEEP_JS_OK = 0,
    SWEEP_JS_ERR_ARGUMENT,
    SWEEP_JS_ERR_LIMIT,
    SWEEP_JS_ERR_ENGINE,
    SWEEP_JS_ERR_SYNTAX,
    SWEEP_JS_ERR_RUNTIME,
    SWEEP_JS_ERR_TIMEOUT,
    SWEEP_JS_ERR_MEMORY,
    SWEEP_JS_ERR_UNSUPPORTED,
    SWEEP_JS_ERR_RESOURCE
} SWEEP_JS_STATUS;

typedef struct _SWEEP_JS_RESULT {
    SWEEP_JS_STATUS status;
    bool timed_out;
    bool is_undefined;
    char value[SWEEP_JS_RESULT_MAX];
    size_t value_len;
    char error[SWEEP_JS_ERROR_MAX];
} SWEEP_JS_RESULT;

/* Explicit local-resource provider.  `url` is DOM-side UTF-8.  On success the
 * provider writes at most out_cap bytes, sets *out_len, and optionally writes
 * a MIME type.  Returning false means the resource is unavailable; it is never
 * interpreted as a network fetch. */
typedef bool (*SWEEP_JS_RESOURCE_FETCH)(void* user_data, const char* url,
                                        char* out, size_t out_cap,
                                        size_t* out_len, char* mime_out,
                                        size_t mime_cap);

typedef struct _SWEEP_JS_RUNTIME_CONFIG {
    size_t memory_limit;      /* per-context heap ceiling; 0 = default 1 MiB */
    uint32_t timeout_ms;      /* per-evaluation deadline; 0 = default 2000 ms */
    SWEEP_TEXT_MODE text_mode;/* WEB_COMPAT unless set to NATIVE_SU */
    SWEEP_JS_RESOURCE_FETCH fetch;
    void* fetch_user_data;
} SWEEP_JS_RUNTIME_CONFIG;

typedef struct _SWEEP_JS_SCRIPT_STATS {
    uint32_t seen;        /* script elements found in document order */
    uint32_t executed;    /* completed without an error */
    uint32_t failed;      /* attempted and raised an engine/resource error */
    uint32_t skipped;     /* empty or non-JavaScript type */
    uint32_t unsupported; /* module / unsupported MIME / unsupported feature */
    uint32_t external;    /* external src scripts attempted */
    bool saw_defer;       /* attributes observed; scheduling is NOT implemented */
    bool saw_async;
} SWEEP_JS_SCRIPT_STATS;

typedef struct SWEEP_JS_RUNTIME SWEEP_JS_RUNTIME;
typedef struct SWEEP_JS_CONTEXT SWEEP_JS_CONTEXT;

bool sweep_js_runtime_init(SWEEP_JS_RUNTIME** out_runtime,
                           const SWEEP_JS_RUNTIME_CONFIG* config);
void sweep_js_runtime_shutdown(SWEEP_JS_RUNTIME* runtime);

/* Create an independent global object (Duktape heap) bound to `dom`.  The DOM
 * must outlive the context; the context never frees it.  Each context gets its
 * own wrappers, listeners and globals, so two documents cannot share state. */
SWEEP_JS_CONTEXT* sweep_js_context_create(SWEEP_JS_RUNTIME* runtime,
                                          SWEEP_DOM* dom);
void sweep_js_context_destroy(SWEEP_JS_CONTEXT* context);

/* Evaluate DOM-side UTF-8 source.  `out` is always populated (status/error). */
bool sweep_js_evaluate(SWEEP_JS_CONTEXT* context, const char* source,
                       size_t source_len, SWEEP_JS_RESULT* out);
bool sweep_js_evaluate_cstr(SWEEP_JS_CONTEXT* context, const char* source,
                            SWEEP_JS_RESULT* out);

/* Execute classic <script> elements in document order, synchronously.  Inline
 * source comes from the DOM; external source comes only from the configured
 * provider.  defer/async are counted in stats but not scheduled (no event
 * loop/task queue exists yet).  Scripts added during this pass are not run. */
bool sweep_js_run_document_scripts(SWEEP_JS_CONTEXT* context,
                                   SWEEP_JS_SCRIPT_STATS* stats);

const char* sweep_js_context_last_error(const SWEEP_JS_CONTEXT* context);
size_t sweep_js_context_memory_used(const SWEEP_JS_CONTEXT* context);
size_t sweep_js_context_memory_limit(const SWEEP_JS_CONTEXT* context);
SWEEP_TEXT_MODE sweep_js_context_text_mode(const SWEEP_JS_CONTEXT* context);
bool sweep_js_context_dom_dirty(const SWEEP_JS_CONTEXT* context);

#endif /* SWEEP_JS_H */
