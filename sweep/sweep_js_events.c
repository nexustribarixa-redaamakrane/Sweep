/* sweep_js_events.c - event objects, listener storage, capture/bubble dispatch.
 *
 * Scope is deliberately a bounded DOM subset, not full browser event
 * compatibility: no event loop/task queue, no UI event defaults, no passive
 * listeners, no trusted-event semantics and no cross-document events.
 */
#include "sweep_js_events.h"

#include <string.h>

#include "sweep_js_dom.h"

#define SWEEP_JS_FUNCS_KEY "__sweep_listener_funcs"

static SWEEP_JS_CONTEXT* context_from_engine(SWEEP_JS_ENGINE* engine) {
    return (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
}

static bool arg_dom_text(SWEEP_JS_CONTEXT* context, int index, char* out,
                         size_t cap, size_t* out_len) {
    size_t len = 0u;
    const char* p = sweep_js_engine_to_lstring(context->engine, index, &len);
    if (!p) p = "";
    return sweep_js_make_dom_text(context, p, len, out, cap, out_len);
}

static bool read_flag(SWEEP_JS_ENGINE* engine, int obj, const char* key) {
    bool value = false;
    (void) sweep_js_engine_get_prop_string(engine, obj, key);
    value = sweep_js_engine_get_boolean(engine, -1);
    sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);
    return value;
}

static void write_flag(SWEEP_JS_ENGINE* engine, int obj, const char* key,
                       bool value) {
    sweep_js_engine_push_boolean(engine, value);
    (void) sweep_js_engine_put_prop_string(engine, obj, key);
}

static void write_null(SWEEP_JS_ENGINE* engine, int obj, const char* key) {
    sweep_js_engine_push_null(engine);
    (void) sweep_js_engine_put_prop_string(engine, obj, key);
}

static void write_number(SWEEP_JS_ENGINE* engine, int obj, const char* key,
                         double value) {
    sweep_js_engine_push_number(engine, value);
    (void) sweep_js_engine_put_prop_string(engine, obj, key);
}

static bool read_bool_prop(SWEEP_JS_ENGINE* engine, int obj, const char* key) {
    bool value = false;
    if (sweep_js_engine_get_prop_string(engine, obj, key)) {
        value = sweep_js_engine_get_boolean(engine, -1);
    }
    sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);
    return value;
}

static void def_method(SWEEP_JS_ENGINE* engine, int obj, const char* name,
                       SWEEP_JS_ENGINE_CFUNC fn, int nargs) {
    sweep_js_engine_push_c_function(engine, fn, nargs);
    (void) sweep_js_engine_put_prop_string(engine, obj, name);
}

/* ---- Event object methods ---- */

static int get_default_prevented(SWEEP_JS_ENGINE* engine) {
    int top = sweep_js_engine_get_top(engine);
    bool prevented;
    sweep_js_engine_push_this(engine);
    prevented = read_flag(engine, top, "__sweep_default_prevented");
    sweep_js_engine_set_top(engine, top);
    sweep_js_engine_push_boolean(engine, prevented);
    return 1;
}

static int prevent_default(SWEEP_JS_ENGINE* engine) {
    int top = sweep_js_engine_get_top(engine);
    sweep_js_engine_push_this(engine);
    if (read_flag(engine, top, "__sweep_cancelable")) {
        write_flag(engine, top, "__sweep_default_prevented", true);
    }
    sweep_js_engine_set_top(engine, top);
    return 0;
}

static int stop_propagation(SWEEP_JS_ENGINE* engine) {
    int top = sweep_js_engine_get_top(engine);
    sweep_js_engine_push_this(engine);
    write_flag(engine, top, "__sweep_stop", true);
    sweep_js_engine_set_top(engine, top);
    return 0;
}

static int stop_immediate_propagation(SWEEP_JS_ENGINE* engine) {
    int top = sweep_js_engine_get_top(engine);
    sweep_js_engine_push_this(engine);
    write_flag(engine, top, "__sweep_stop", true);
    write_flag(engine, top, "__sweep_stop_immediate", true);
    sweep_js_engine_set_top(engine, top);
    return 0;
}

/* ---- Event constructor ---- */

static bool push_event_object(SWEEP_JS_CONTEXT* context, const char* type,
                              size_t type_len, bool bubbles, bool cancelable) {
    SWEEP_JS_ENGINE* engine = context->engine;
    int obj;
    sweep_js_engine_push_object(engine);
    obj = sweep_js_engine_get_top(engine) - 1;

    sweep_js_engine_push_lstring(engine, type, type_len);
    (void) sweep_js_engine_put_prop_string(engine, obj, "type");
    sweep_js_engine_push_boolean(engine, bubbles);
    (void) sweep_js_engine_put_prop_string(engine, obj, "bubbles");
    sweep_js_engine_push_boolean(engine, cancelable);
    (void) sweep_js_engine_put_prop_string(engine, obj, "cancelable");
    write_null(engine, obj, "target");
    write_null(engine, obj, "currentTarget");
    write_number(engine, obj, "eventPhase", 0.0);

    (void) sweep_js_engine_def_hidden_boolean(engine, obj,
        "__sweep_event", true);
    write_flag(engine, obj, "__sweep_cancelable", cancelable);
    write_flag(engine, obj, "__sweep_default_prevented", false);
    write_flag(engine, obj, "__sweep_stop", false);
    write_flag(engine, obj, "__sweep_stop_immediate", false);
    def_method(engine, obj, "preventDefault", prevent_default, 0);
    def_method(engine, obj, "stopPropagation", stop_propagation, 0);
    def_method(engine, obj, "stopImmediatePropagation",
               stop_immediate_propagation, 0);
    (void) sweep_js_engine_def_accessor(engine, obj, "defaultPrevented",
                                        get_default_prevented, NULL);
    return true;
}

static int event_constructor(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = context_from_engine(engine);
    const char* type;
    size_t type_len = 0u;
    bool bubbles = false;
    bool cancelable = false;

    if (sweep_js_engine_is_nullish(engine, 0)) {
        (void) sweep_js_engine_throw_type_error(engine,
            "Event type is required");
        return 0;
    }
    type = sweep_js_engine_to_lstring(engine, 0, &type_len);
    if (!type) { type = ""; type_len = 0u; }
    if (type_len >= SWEEP_JS_EVENT_TYPE_MAX) {
        (void) sweep_js_engine_throw_range_error(engine, "event type too long");
        return 0;
    }
    if (sweep_js_engine_is_object(engine, 1)) {
        bubbles = read_bool_prop(engine, 1, "bubbles");
        cancelable = read_bool_prop(engine, 1, "cancelable");
    }
    (void) push_event_object(context, type, type_len, bubbles, cancelable);
    return 1;
}

/* ---- listeners ---- */

static bool listener_type_from_arg(SWEEP_JS_CONTEXT* context, int index,
                                   char* out, size_t* out_len) {
    return arg_dom_text(context, index, out, SWEEP_JS_EVENT_TYPE_MAX, out_len);
}

static bool listener_capture_from_arg(SWEEP_JS_ENGINE* engine, int index) {
    bool capture = false;
    if (sweep_js_engine_is_nullish(engine, index)) return false;
    if (sweep_js_engine_is_object(engine, index)) {
        capture = read_bool_prop(engine, index, "capture");
    } else {
        capture = sweep_js_engine_get_boolean(engine, index);
    }
    return capture;
}

static bool stored_function_equals(SWEEP_JS_CONTEXT* context, int slot,
                                   int function_index) {
    SWEEP_JS_ENGINE* engine = context->engine;
    bool equal;
    (void) sweep_js_engine_get_stash(engine, SWEEP_JS_FUNCS_KEY);
    (void) sweep_js_engine_get_prop_index(engine, -1, (uint32_t) slot);
    equal = sweep_js_engine_is_function(engine, -1) &&
            sweep_js_engine_strict_equals(engine, -1, function_index);
    sweep_js_engine_remove(engine, -2);
    sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);
    return equal;
}

static int add_event_listener(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = context_from_engine(engine);
    uint32_t target;
    char type[SWEEP_JS_EVENT_TYPE_MAX];
    size_t type_len = 0u;
    bool capture;
    uint32_t i, slot = SWEEP_JS_LISTENERS_MAX;

    if (!sweep_js_dom_this_node(context, &target)) return 0;
    if (!sweep_js_engine_is_function(engine, 1)) {
        (void) sweep_js_engine_throw_type_error(engine,
            "addEventListener requires a function");
        return 0;
    }
    if (!listener_type_from_arg(context, 0, type, &type_len)) {
        (void) sweep_js_engine_throw_range_error(engine, "event type too long");
        return 0;
    }
    capture = listener_capture_from_arg(engine, 2);

    for (i = 0u; i < SWEEP_JS_LISTENERS_MAX; ++i) {
        SWEEP_JS_LISTENER* rec = &context->listeners[i];
        if (rec->active && rec->target == target &&
            rec->generation ==
                sweep_dom_node_generation(context->dom, target) &&
            rec->capture == capture &&
            strcmp(rec->type, type) == 0 &&
            stored_function_equals(context, rec->slot, 1)) {
            return 0;
        }
    }
    for (i = 0u; i < SWEEP_JS_LISTENERS_MAX; ++i) {
        if (!context->listeners[i].active) { slot = i; break; }
    }
    if (slot == SWEEP_JS_LISTENERS_MAX) {
        (void) sweep_js_engine_throw_range_error(engine, "listener limit");
        return 0;
    }

    (void) sweep_js_engine_get_stash(engine, SWEEP_JS_FUNCS_KEY);
    sweep_js_engine_dup(engine, 1);
    (void) sweep_js_engine_put_prop_index(engine, -2, slot);
    sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);

    context->listeners[slot].active = true;
    context->listeners[slot].target = target;
    context->listeners[slot].generation =
        sweep_dom_node_generation(context->dom, target);
    context->listeners[slot].capture = capture;
    context->listeners[slot].serial = ++context->listener_serial;
    context->listeners[slot].slot = (int) slot;
    memcpy(context->listeners[slot].type, type, type_len + 1u);
    return 0;
}

static int remove_event_listener(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = context_from_engine(engine);
    uint32_t target, i;
    char type[SWEEP_JS_EVENT_TYPE_MAX];
    size_t type_len = 0u;
    bool capture;

    if (!sweep_js_dom_this_node(context, &target)) return 0;
    if (!sweep_js_engine_is_function(engine, 1)) return 0;
    if (!listener_type_from_arg(context, 0, type, &type_len)) return 0;
    capture = listener_capture_from_arg(engine, 2);

    for (i = 0u; i < SWEEP_JS_LISTENERS_MAX; ++i) {
        SWEEP_JS_LISTENER* rec = &context->listeners[i];
        if (rec->active && rec->target == target &&
            rec->generation ==
                sweep_dom_node_generation(context->dom, target) &&
            rec->capture == capture &&
            strcmp(rec->type, type) == 0 &&
            stored_function_equals(context, rec->slot, 1)) {
            rec->active = false;
            (void) sweep_js_engine_get_stash(engine, SWEEP_JS_FUNCS_KEY);
            sweep_js_engine_push_undefined(engine);
            (void) sweep_js_engine_put_prop_index(engine, -2, i);
            sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);
            return 0;
        }
    }
    return 0;
}

/* ---- dispatch ---- */

static void dispatch_listeners(SWEEP_JS_CONTEXT* context, uint32_t target,
                               bool capture, int event_index, const char* type,
                               uint32_t start_serial) {
    SWEEP_JS_ENGINE* engine = context->engine;
    uint32_t i;

    for (i = 0u; i < SWEEP_JS_LISTENERS_MAX; ++i) {
        SWEEP_JS_LISTENER* rec = &context->listeners[i];
        if (!rec->active || rec->target != target ||
            rec->generation !=
                sweep_dom_node_generation(context->dom, target) ||
            rec->capture != capture || strcmp(rec->type, type) != 0 ||
            rec->serial > start_serial) {
            /* Stale records (target slot reclaimed and reused) are dropped,
             * never fired on the new occupant of the slot. */
            if (rec->active && rec->target == target &&
                rec->generation !=
                    sweep_dom_node_generation(context->dom, target)) {
                rec->active = false;
            }
            continue;
        }

        (void) sweep_js_dom_push_node(context, target);
        (void) sweep_js_engine_put_prop_string(engine, event_index,
                                               "currentTarget");

        (void) sweep_js_engine_get_stash(engine, SWEEP_JS_FUNCS_KEY);
        (void) sweep_js_engine_get_prop_index(engine, -1, (uint32_t) rec->slot);
        sweep_js_engine_remove(engine, -2);
        if (!sweep_js_engine_is_function(engine, -1)) {
            sweep_js_engine_set_top(engine,
                sweep_js_engine_get_top(engine) - 1);
            continue;
        }
        (void) sweep_js_dom_push_node(context, target); /* this */
        sweep_js_engine_dup(engine, event_index);       /* argument */
        if (sweep_js_engine_pcall_method(engine, 1) == 0) {
            sweep_js_engine_set_top(engine,
                sweep_js_engine_get_top(engine) - 1); /* return value */
        } else {
            size_t elen = 0u;
            const char* text = sweep_js_engine_to_lstring(engine, -1, &elen);
            (void) elen;
            context->event_errors++;
            sweep_js_set_error(context, text ? text : "event listener error");
            sweep_js_engine_set_top(engine,
                sweep_js_engine_get_top(engine) - 1); /* error value */
        }
        if (read_flag(engine, event_index, "__sweep_stop_immediate")) {
            return;
        }
    }
}

static int dispatch_event(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = context_from_engine(engine);
    uint32_t target;
    uint32_t path[SWEEP_DOM_NODES_MAX];
    uint32_t path_len = 0u, id;
    char type[SWEEP_JS_EVENT_TYPE_MAX];
    size_t type_len = 0u;
    uint32_t start_serial;
    bool bubbles, cancelable, prevented;
    int event_index = 0;

    if (!sweep_js_dom_this_node(context, &target)) return 0;
    if (!sweep_js_engine_is_object(engine, event_index)) {
        (void) sweep_js_engine_throw_type_error(engine,
            "dispatchEvent requires an event object");
        return 0;
    }
    if (!sweep_js_engine_get_prop_string(engine, event_index, "type") ||
        !sweep_js_engine_is_string(engine, -1)) {
        sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);
        (void) sweep_js_engine_throw_type_error(engine,
            "event.type must be a string");
        return 0;
    }
    {
        size_t len = 0u;
        const char* p = sweep_js_engine_get_lstring(engine, -1, &len);
        bool ok = p && sweep_js_make_dom_text(context, p, len, type,
                                              sizeof(type), &type_len);
        sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);
        if (!ok) {
            (void) sweep_js_engine_throw_range_error(engine,
                "event type too long");
            return 0;
        }
    }
    bubbles = read_bool_prop(engine, event_index, "bubbles");
    cancelable = read_bool_prop(engine, event_index, "cancelable");
    write_flag(engine, event_index, "__sweep_cancelable", cancelable);
    write_flag(engine, event_index, "__sweep_default_prevented", false);
    write_flag(engine, event_index, "__sweep_stop", false);
    write_flag(engine, event_index, "__sweep_stop_immediate", false);
    (void) sweep_js_dom_push_node(context, target);
    (void) sweep_js_engine_put_prop_string(engine, event_index, "target");
    write_null(engine, event_index, "currentTarget");
    write_number(engine, event_index, "eventPhase", 0.0);

    if (context->dispatch_depth >= SWEEP_JS_DISPATCH_DEPTH_MAX) {
        (void) sweep_js_engine_throw_range_error(engine,
            "event dispatch recursion limit");
        return 0;
    }
    context->dispatch_depth++;
    start_serial = context->listener_serial;

    id = target;
    while (id != 0u && path_len < SWEEP_DOM_NODES_MAX) {
        const SWEEP_DOM_NODE* n = sweep_dom_get(context->dom, id);
        path[path_len++] = id;
        if (!n) break;
        id = n->parent;
    }

    /* capture: document -> parent of target */
    {
        uint32_t i = path_len;
        while (i > 1u) {
            --i;
            write_number(engine, event_index, "eventPhase", 1.0);
            dispatch_listeners(context, path[i], true, event_index, type,
                               start_serial);
            if (read_flag(engine, event_index, "__sweep_stop")) break;
        }
    }
    if (!read_flag(engine, event_index, "__sweep_stop")) {
        write_number(engine, event_index, "eventPhase", 2.0);
        dispatch_listeners(context, target, true, event_index, type,
                           start_serial);
        if (!read_flag(engine, event_index, "__sweep_stop_immediate")) {
            dispatch_listeners(context, target, false, event_index, type,
                               start_serial);
        }
    }
    if (bubbles && !read_flag(engine, event_index, "__sweep_stop")) {
        uint32_t i;
        write_number(engine, event_index, "eventPhase", 3.0);
        for (i = 1u; i < path_len; ++i) {
            dispatch_listeners(context, path[i], false, event_index, type,
                               start_serial);
            if (read_flag(engine, event_index, "__sweep_stop")) break;
        }
    }

    write_null(engine, event_index, "currentTarget");
    write_number(engine, event_index, "eventPhase", 0.0);
    prevented = read_flag(engine, event_index, "__sweep_default_prevented");
    context->dispatch_depth--;
    sweep_js_engine_push_boolean(engine, !prevented);
    return 1;
}

bool sweep_js_events_install(SWEEP_JS_CONTEXT* context) {
    SWEEP_JS_ENGINE* engine;
    int proto;
    if (!context || !context->engine) return false;
    engine = context->engine;

    sweep_js_engine_push_array(engine);
    if (!sweep_js_engine_put_stash(engine, SWEEP_JS_FUNCS_KEY)) return false;

    if (!sweep_js_dom_push_node_prototype(context)) return false;
    proto = sweep_js_engine_get_top(engine) - 1;
    def_method(engine, proto, "addEventListener", add_event_listener, 3);
    def_method(engine, proto, "removeEventListener", remove_event_listener, 3);
    def_method(engine, proto, "dispatchEvent", dispatch_event, 1);
    sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 1);

    sweep_js_engine_push_c_function(engine, event_constructor, 2);
    return sweep_js_engine_put_global_string(engine, "Event");
}
