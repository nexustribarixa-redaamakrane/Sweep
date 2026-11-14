/* sweep_js.c - runtime/context lifecycle, evaluation and script loading. */
#include "sweep_js.h"

#include <stdlib.h>
#include <string.h>

#include "sweep_js_dom.h"
#include "sweep_js_engine.h"
#include "sweep_js_events.h"
#include "sweep_js_internal.h"
#include "sweep_js_text.h"

#define SWEEP_JS_ENGINE_SOURCE_MAX (SWEEP_JS_SOURCE_MAX * 2u)
#define SWEEP_JS_ENGINE_VALUE_MAX  (SWEEP_JS_RESULT_MAX * 3u)

void sweep_js_set_error(SWEEP_JS_CONTEXT* context, const char* message) {
    size_t i;
    if (!context) return;
    if (!message) message = "";
    for (i = 0u; message[i] && i + 1u < sizeof(context->last_error); ++i) {
        context->last_error[i] = message[i];
    }
    context->last_error[i] = '\0';
}

static void copy_bounded(char* out, size_t cap, const char* src) {
    size_t i;
    if (!out || cap == 0u) return;
    if (!src) { out[0] = '\0'; return; }
    for (i = 0u; src[i] && i + 1u < cap; ++i) out[i] = src[i];
    out[i] = '\0';
}

bool sweep_js_make_dom_text(SWEEP_JS_CONTEXT* context, const char* in,
                            size_t in_len, char* out, size_t out_cap,
                            size_t* out_len) {
    bool ok;
    size_t used = 0u;
    if (!out || out_cap == 0u) return false;
    out[0] = '\0';
    if (out_len) *out_len = 0u;
    ok = sweep_js_cesu8_to_utf8(
        context ? context->runtime->config.text_mode : SWEEP_TEXT_WEB_COMPAT,
        (const uint8_t*) (in ? in : ""), in ? in_len : 0u,
        (uint8_t*) out, out_cap - 1u, &used);
    if (used < out_cap) out[used] = '\0';
    else out[out_cap - 1u] = '\0';
    if (out_len) *out_len = used;
    return ok;
}

bool sweep_js_make_engine_text(SWEEP_JS_CONTEXT* context, const char* in,
                               size_t in_len, char* out, size_t out_cap,
                               size_t* out_len) {
    bool ok;
    size_t used = 0u;
    if (!out || out_cap == 0u) return false;
    out[0] = '\0';
    if (out_len) *out_len = 0u;
    ok = sweep_js_utf8_to_cesu8(
        context ? context->runtime->config.text_mode : SWEEP_TEXT_WEB_COMPAT,
        (const uint8_t*) (in ? in : ""), in ? in_len : 0u,
        (uint8_t*) out, out_cap - 1u, &used);
    if (used < out_cap) out[used] = '\0';
    else out[out_cap - 1u] = '\0';
    if (out_len) *out_len = used;
    return ok;
}

bool sweep_js_push_dom_text(SWEEP_JS_CONTEXT* context, const char* dom_text) {
    char engine_text[SWEEP_DOM_SERIALIZE_MAX * 2u];
    size_t len = 0u;
    if (!sweep_js_make_engine_text(context, dom_text ? dom_text : "",
                                   dom_text ? strlen(dom_text) : 0u,
                                   engine_text, sizeof(engine_text), &len)) {
        return false;
    }
    sweep_js_engine_push_lstring(context->engine, engine_text, len);
    return true;
}

/* ---- runtime ---- */

bool sweep_js_runtime_init(SWEEP_JS_RUNTIME** out_runtime,
                           const SWEEP_JS_RUNTIME_CONFIG* config) {
    SWEEP_JS_RUNTIME* runtime;
    if (!out_runtime) return false;
    *out_runtime = NULL;
    runtime = (SWEEP_JS_RUNTIME*) calloc(1u, sizeof(*runtime));
    if (!runtime) return false;
    if (config) runtime->config = *config;
    if (runtime->config.memory_limit == 0u) {
        runtime->config.memory_limit = SWEEP_JS_ENGINE_DEFAULT_MEMORY;
    }
    if (runtime->config.timeout_ms == 0u) {
        runtime->config.timeout_ms = SWEEP_JS_ENGINE_DEFAULT_TIMEOUT_MS;
    }
    if (runtime->config.text_mode != SWEEP_TEXT_NATIVE_SU) {
        runtime->config.text_mode = SWEEP_TEXT_WEB_COMPAT;
    }
    runtime->initialized = true;
    *out_runtime = runtime;
    return true;
}

void sweep_js_runtime_shutdown(SWEEP_JS_RUNTIME* runtime) {
    uint32_t i;
    if (!runtime) return;
    for (i = 0u; i < SWEEP_JS_CONTEXTS_MAX; ++i) {
        if (runtime->contexts[i]) sweep_js_context_destroy(runtime->contexts[i]);
    }
    runtime->initialized = false;
    free(runtime);
}

SWEEP_JS_CONTEXT* sweep_js_context_create(SWEEP_JS_RUNTIME* runtime,
                                          SWEEP_DOM* dom) {
    SWEEP_JS_CONTEXT* context;
    SWEEP_JS_ENGINE_CONFIG engine_config;
    uint32_t i, slot = SWEEP_JS_CONTEXTS_MAX;
    if (!runtime || !runtime->initialized || !dom) return NULL;
    for (i = 0u; i < SWEEP_JS_CONTEXTS_MAX; ++i) {
        if (!runtime->contexts[i]) { slot = i; break; }
    }
    if (slot == SWEEP_JS_CONTEXTS_MAX) return NULL;
    context = (SWEEP_JS_CONTEXT*) calloc(1u, sizeof(*context));
    if (!context) return NULL;
    context->runtime = runtime;
    context->dom = dom;
    engine_config.memory_limit = runtime->config.memory_limit;
    engine_config.timeout_ms = runtime->config.timeout_ms;
    engine_config.user_data = context;
    if (!sweep_js_engine_create(&engine_config, &context->engine)) {
        free(context);
        return NULL;
    }
    if (!sweep_js_dom_install(context) || !sweep_js_events_install(context)) {
        sweep_js_engine_destroy(context->engine);
        free(context);
        return NULL;
    }
    context->in_use = true;
    context->globals_installed = true;
    runtime->contexts[slot] = context;
    return context;
}

void sweep_js_context_destroy(SWEEP_JS_CONTEXT* context) {
    uint32_t i;
    if (!context) return;
    if (context->runtime) {
        for (i = 0u; i < SWEEP_JS_CONTEXTS_MAX; ++i) {
            if (context->runtime->contexts[i] == context) {
                context->runtime->contexts[i] = NULL;
                break;
            }
        }
    }
    if (context->engine) sweep_js_engine_destroy(context->engine);
    context->engine = NULL;
    context->in_use = false;
    free(context);
}

const char* sweep_js_context_last_error(const SWEEP_JS_CONTEXT* context) {
    return context ? context->last_error : "";
}

size_t sweep_js_context_memory_used(const SWEEP_JS_CONTEXT* context) {
    return context ? sweep_js_engine_memory_used(context->engine) : 0u;
}

size_t sweep_js_context_memory_limit(const SWEEP_JS_CONTEXT* context) {
    return context ? sweep_js_engine_memory_limit(context->engine) : 0u;
}

SWEEP_TEXT_MODE sweep_js_context_text_mode(const SWEEP_JS_CONTEXT* context) {
    return context ? context->runtime->config.text_mode : SWEEP_TEXT_WEB_COMPAT;
}

bool sweep_js_context_dom_dirty(const SWEEP_JS_CONTEXT* context) {
    return context ? sweep_dom_is_dirty(context->dom) : false;
}

/* ---- evaluation ---- */

static SWEEP_JS_STATUS classify_error(const char* error, bool timed_out) {
    if (timed_out) return SWEEP_JS_ERR_TIMEOUT;
    if (!error) return SWEEP_JS_ERR_ENGINE;
    if (strstr(error, "SyntaxError")) return SWEEP_JS_ERR_SYNTAX;
    if (strstr(error, "out of memory") || strstr(error, "alloc failed") ||
        strstr(error, "allocation failed")) {
        return SWEEP_JS_ERR_MEMORY;
    }
    if (strstr(error, "callstack") || strstr(error, "recursion") ||
        strstr(error, "stack limit")) {
        return SWEEP_JS_ERR_LIMIT;
    }
    if (strstr(error, "TypeError") || strstr(error, "ReferenceError") ||
        strstr(error, "RangeError") || strstr(error, "Error")) {
        return SWEEP_JS_ERR_RUNTIME;
    }
    return SWEEP_JS_ERR_ENGINE;
}

bool sweep_js_evaluate(SWEEP_JS_CONTEXT* context, const char* source,
                       size_t source_len, SWEEP_JS_RESULT* out) {
    SWEEP_JS_RESULT local;
    char* engine_source;
    size_t engine_len = 0u;
    char engine_value[SWEEP_JS_ENGINE_VALUE_MAX];
    char engine_error[SWEEP_JS_ERROR_MAX * 2u];
    bool is_undefined = true;
    bool ok;

    if (!out) out = &local;
    memset(out, 0, sizeof(*out));
    out->status = SWEEP_JS_ERR_ARGUMENT;
    if (!context || !context->engine) {
        copy_bounded(out->error, sizeof(out->error), "no JavaScript context");
        return false;
    }
    if (source_len > SWEEP_JS_SOURCE_MAX) {
        out->status = SWEEP_JS_ERR_LIMIT;
        copy_bounded(out->error, sizeof(out->error), "script source too large");
        sweep_js_set_error(context, out->error);
        return false;
    }

    engine_source = (char*) malloc(SWEEP_JS_ENGINE_SOURCE_MAX);
    if (!engine_source) {
        out->status = SWEEP_JS_ERR_MEMORY;
        copy_bounded(out->error, sizeof(out->error),
                     "host allocation failed for script source");
        sweep_js_set_error(context, out->error);
        return false;
    }
    if (!sweep_js_make_engine_text(context, source ? source : "", source_len,
                                   engine_source, SWEEP_JS_ENGINE_SOURCE_MAX,
                                   &engine_len)) {
        free(engine_source);
        out->status = SWEEP_JS_ERR_LIMIT;
        copy_bounded(out->error, sizeof(out->error),
                     "script source exceeds text conversion limit");
        sweep_js_set_error(context, out->error);
        return false;
    }

    context->dispatch_depth = 0u;
    context->event_errors = 0u;
    engine_value[0] = '\0';
    engine_error[0] = '\0';
    ok = sweep_js_engine_eval(context->engine, engine_source, engine_len,
                              engine_value, sizeof(engine_value),
                              engine_error, sizeof(engine_error),
                              &is_undefined);
    free(engine_source);
    context->dispatch_depth = 0u;

    if (!ok) {
        size_t len = 0u;
        out->timed_out = sweep_js_engine_timed_out(context->engine);
        out->status = classify_error(engine_error, out->timed_out);
        out->is_undefined = true;
        if (!sweep_js_make_dom_text(context, engine_error,
                                    strlen(engine_error), out->error,
                                    sizeof(out->error), &len)) {
            copy_bounded(out->error, sizeof(out->error),
                         "JavaScript error (diagnostic truncated)");
        }
        if (out->error[0] == '\0') {
            copy_bounded(out->error, sizeof(out->error), "JavaScript error");
        }
        sweep_js_set_error(context, out->error);
        return false;
    }

    out->status = SWEEP_JS_OK;
    out->timed_out = false;
    out->is_undefined = is_undefined;
    if (!is_undefined) {
        size_t len = 0u;
        if (!sweep_js_make_dom_text(context, engine_value,
                                    strlen(engine_value), out->value,
                                    sizeof(out->value), &len)) {
            out->status = SWEEP_JS_ERR_LIMIT;
            copy_bounded(out->error, sizeof(out->error),
                         "evaluation result exceeds result limit");
            sweep_js_set_error(context, out->error);
            return false;
        }
        out->value_len = len;
    }
    context->last_error[0] = '\0';
    return true;
}

bool sweep_js_evaluate_cstr(SWEEP_JS_CONTEXT* context, const char* source,
                            SWEEP_JS_RESULT* out) {
    return sweep_js_evaluate(context, source, source ? strlen(source) : 0u, out);
}

/* ---- script elements ---- */

static void lower_trim_mime(const char* mime, char* out, size_t cap) {
    size_t i = 0u, j = 0u;
    if (!out || cap == 0u) return;
    if (!mime) { out[0] = '\0'; return; }
    while (mime[i] && mime[i] != ';' && j + 1u < cap) {
        unsigned char c = (unsigned char) mime[i++];
        if (c == ' ' || c == '\t') continue;
        if (c >= 'A' && c <= 'Z') c = (unsigned char) (c - 'A' + 'a');
        out[j++] = (char) c;
    }
    while (j > 0u && (out[j - 1u] == ' ' || out[j - 1u] == '\t')) --j;
    out[j] = '\0';
}

static bool is_javascript_mime(const char* mime) {
    char m[64];
    lower_trim_mime(mime, m, sizeof(m));
    if (m[0] == '\0') return true;
    return strcmp(m, "text/javascript") == 0 ||
           strcmp(m, "application/javascript") == 0 ||
           strcmp(m, "text/ecmascript") == 0 ||
           strcmp(m, "application/ecmascript") == 0 ||
           strcmp(m, "application/x-javascript") == 0;
}

static bool is_module_type(const char* type) {
    char m[64];
    lower_trim_mime(type, m, sizeof(m));
    return strstr(m, "module") != NULL;
}

static void collect_scripts(const SWEEP_DOM* dom, uint32_t root,
                            uint32_t* ids, uint32_t* count) {
    const SWEEP_DOM_NODE* n = sweep_dom_get(dom, root);
    uint32_t i;
    if (!n) return;
    for (i = 0u; i < n->child_count; ++i) {
        const SWEEP_DOM_NODE* c = sweep_dom_get(dom, n->children[i]);
        if (c && c->type == SWEEP_DOM_ELEMENT && strcmp(c->name, "script") == 0) {
            if (*count < SWEEP_DOM_NODES_MAX) ids[(*count)++] = n->children[i];
        }
        collect_scripts(dom, n->children[i], ids, count);
    }
}

static size_t collect_inline_source(const SWEEP_DOM* dom, uint32_t id,
                                    char* out, size_t cap) {
    const SWEEP_DOM_NODE* n = sweep_dom_get(dom, id);
    uint32_t i;
    size_t used = 0u;
    if (!n || !out || cap == 0u) return 0u;
    out[0] = '\0';
    for (i = 0u; i < n->child_count; ++i) {
        const SWEEP_DOM_NODE* c = sweep_dom_get(dom, n->children[i]);
        size_t j;
        if (!c || c->type != SWEEP_DOM_TEXT) continue;
        for (j = 0u; c->text[j] && used + 1u < cap; ++j) {
            out[used++] = c->text[j];
        }
        out[used] = '\0';
    }
    return used;
}

bool sweep_js_run_document_scripts(SWEEP_JS_CONTEXT* context,
                                   SWEEP_JS_SCRIPT_STATS* stats) {
    SWEEP_JS_SCRIPT_STATS local;
    uint32_t ids[SWEEP_DOM_NODES_MAX];
    uint32_t count = 0u, i;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (!context || !context->dom) return false;

    collect_scripts(context->dom, context->dom->document, ids, &count);
    for (i = 0u; i < count; ++i) {
        uint32_t id = ids[i];
        const SWEEP_DOM_NODE* node = sweep_dom_get(context->dom, id);
        const char* type;
        const char* src;
        const char* defer;
        const char* async_attr;
        char* source;
        size_t source_len = 0u;
        bool external = false;
        bool external_allocated = false;
        SWEEP_JS_RESULT result;

        if (!node || node->type != SWEEP_DOM_ELEMENT ||
            strcmp(node->name, "script") != 0) {
            continue;
        }
        stats->seen++;
        type = sweep_dom_get_attr(node, "type");
        src = sweep_dom_get_attr(node, "src");
        defer = sweep_dom_get_attr(node, "defer");
        async_attr = sweep_dom_get_attr(node, "async");
        if (defer) stats->saw_defer = true;
        if (async_attr) stats->saw_async = true;

        if (type && type[0] != '\0') {
            if (is_module_type(type)) {
                stats->unsupported++;
                continue;
            }
            if (!is_javascript_mime(type)) {
                stats->skipped++;
                continue;
            }
        }

        if (src && src[0] != '\0') {
            char mime[64];
            size_t len = 0u;
            external = true;
            stats->external++;
            if (!context->runtime->config.fetch) {
                stats->failed++;
                sweep_js_set_error(context,
                    "external script requires a resource provider");
                continue;
            }
            source = (char*) malloc(SWEEP_JS_SOURCE_MAX);
            if (!source) {
                stats->failed++;
                sweep_js_set_error(context, "host allocation failed");
                continue;
            }
            external_allocated = true;
            mime[0] = '\0';
            if (!context->runtime->config.fetch(
                    context->runtime->config.fetch_user_data, src,
                    source, SWEEP_JS_SOURCE_MAX, &len, mime, sizeof(mime)) ||
                len > SWEEP_JS_SOURCE_MAX) {
                stats->failed++;
                free(source);
                sweep_js_set_error(context, "resource provider failed");
                continue;
            }
            if (!is_javascript_mime(mime)) {
                stats->unsupported++;
                free(source);
                continue;
            }
            source_len = len;
        } else {
            source = (char*) malloc(SWEEP_JS_SOURCE_MAX);
            if (!source) {
                stats->failed++;
                sweep_js_set_error(context, "host allocation failed");
                continue;
            }
            source_len = collect_inline_source(context->dom, id, source,
                                               SWEEP_JS_SOURCE_MAX);
            if (source_len == 0u) {
                stats->skipped++;
                free(source);
                continue;
            }
        }

        (void) external;
        if (sweep_js_evaluate(context, source, source_len, &result)) {
            stats->executed++;
        } else {
            stats->failed++;
        }
        if (external_allocated) free(source);
        else free(source);
    }
    return stats->failed == 0u;
}
