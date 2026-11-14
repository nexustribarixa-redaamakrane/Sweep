/* kapa_dom_js.c - narrow JavaScript DOM bindings (R7 KAPA integration).
 *
 * Application-side adapter between the real Duktape engine and the real Sweep
 * DOM.  It deliberately does NOT re-implement any document model: every read
 * and write goes to the DOM tree built by sweep_dom_parse from the canned
 * fixture, and every JavaScript value is validated before it reaches it.
 *
 * Binding model
 * -------------
 *   document.getElementById(id)  -> Element wrapper | null
 *   element.getAttribute(name)   -> string | null
 *   element.textContent          -> string           (accessor, stringify of
 *                                                   descendant text)
 *   element.setTextContent(v)    -> undefined         (v coerced to string;
 *                                                   >1023 bytes refused)
 *   element.nodeName             -> string            (the parser's lowercased
 *                                                   tag name, as stored)
 *   element.children             -> Array<Element>    (element children only)
 *
 * Wrapper lifetime
 * ----------------
 * Every wrapper is a fresh object carrying two non-writable, non-enumerable,
 * non-configurable hidden properties:
 *   __sweep_node  - the DOM slot id (1..SWEEP_DOM_NODES_MAX)
 *   __sweep_gen   - the slot's generation at wrap time
 * Every access re-validates both against the live DOM (sweep_dom_get +
 * sweep_dom_node_generation); a stale wrapper (slot reclaimed and reused)
 * fails closed with a TypeError instead of following a recycled node.  The
 * DOM is not pointer-stable, and nothing in these bindings caches pointers.
 *
 * Errors are reported by throwing JavaScript errors with bounded, fixed
 * messages; failure paths never partially mutate the DOM tree - the DOM APIs
 * are internally consistent and this layer performs validation first.
 */
#include "kapa_dom_js.h"
#include <string.h>

static SWEEP_DOM* s_dom;

/* ---- validation helpers ------------------------------------------------- */

static bool s_get_hidden_int(duk_context* ctx, duk_idx_t obj, const char* key,
                             uint32_t* out) {
    double value = 0.0;

    if (!duk_is_object(ctx, obj)) {
        return false;
    }
    if (duk_get_prop_string(ctx, obj, key) == 0) {
        duk_pop(ctx);
        return false;
    }
    if (!duk_is_number(ctx, -1)) {
        duk_pop(ctx);
        return false;
    }
    value = duk_get_number(ctx, -1);
    duk_pop(ctx);
    if (value < 1.0 || value > 4294967295.0) {
        return false;
    }
    *out = (uint32_t) value;
    return true;
}

/* Resolve `this` (an element wrapper) to a live node id, or throw TypeError. */
static uint32_t s_this_node(duk_context* ctx) {
    uint32_t node = 0u, gen = 0u;

    if (!s_dom) {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "dom binding is not installed");
    }
    /* duk_push_this() pushes the call's `this` value; it stays valid for the
     * duration of this call. */
    duk_push_this(ctx);
    if (!s_get_hidden_int(ctx, -1, "__sweep_node", &node) ||
        !s_get_hidden_int(ctx, -1, "__sweep_gen", &gen)) {
        duk_pop(ctx);
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "not a genuine DOM element wrapper");
    }
    duk_pop(ctx);
    if (sweep_dom_node_generation(s_dom, node) != gen) {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "stale DOM element wrapper");
    }
    return node;
}

/* ---- wrapper construction ----------------------------------------------- */

static void s_push_wrapper(duk_context* ctx, uint32_t id) {
    const SWEEP_DOM_NODE* n;
    uint32_t gen;

    if (!s_dom) {
        duk_push_null(ctx);
        return;
    }
    n = sweep_dom_get(s_dom, id);
    if (!n) {
        duk_push_null(ctx);
        return;
    }
    gen = sweep_dom_node_generation(s_dom, id);

    duk_push_object(ctx);            /* wrapper object on stack top        */
    duk_get_global_string(ctx, "__kapa_proto");
    duk_set_prototype(ctx, -2);      /* wrapper.[[Prototype]] = proto      */

    /* Non-writable, non-enumerable, non-configurable hidden properties. */
    duk_push_number(ctx, (double) id);
    duk_put_prop_string(ctx, -2, "__sweep_node");
    duk_push_number(ctx, (double) gen);
    duk_put_prop_string(ctx, -2, "__sweep_gen");
}

/* Shared prototype with the element-facing methods/accessors. */
static void s_def_accessor(duk_context* ctx, const char* key,
                           duk_c_function getter, duk_c_function setter) {
    /* Not WRITABLE: an accessor property carrying HAVE_WRITABLE would be both
     * a data descriptor and an accessor descriptor to Duktape, which fails
     * with 'invalid descriptor'.  Omit it and accessors stay read-only. */
    duk_uint_t flags = DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER
                     | DUK_DEFPROP_CLEAR_ENUMERABLE
                     | DUK_DEFPROP_CLEAR_CONFIGURABLE;
    duk_push_lstring(ctx, key, strlen(key));
    if (getter) {
        duk_push_c_function(ctx, getter, 0);
    } else {
        duk_push_undefined(ctx);
    }
    if (setter) {
        duk_push_c_function(ctx, setter, 1);
    } else {
        duk_push_undefined(ctx);
    }
    duk_def_prop(ctx, -4, flags);
}

static void s_install_proto(duk_context* ctx);

/* ---- JS-facing methods -------------------------------------------------- */

static duk_ret_t js_get_element_by_id(duk_context* ctx) {
    const char* id = duk_get_string(ctx, 0);
    uint32_t found;

    if (!id || id[0] == '\0') {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "getElementById: expected id string");
    }
    if (!s_dom) {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "dom binding is not installed");
    }
    found = sweep_dom_find_by_id(s_dom, s_dom->document, id);
    s_push_wrapper(ctx, found);
    return 1;
}

static duk_ret_t js_get_attribute(duk_context* ctx) {
    uint32_t id = s_this_node(ctx);
    const char* name = duk_get_string(ctx, 0);
    const SWEEP_DOM_NODE* n;
    const char* value;

    if (!name || name[0] == '\0') {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "getAttribute: expected name string");
    }
    n = sweep_dom_get(s_dom, id);
    if (!n) {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "stale DOM element wrapper");
    }
    value = sweep_dom_get_attr(n, name);
    if (value) {
        duk_push_lstring(ctx, value, strlen(value));
    } else {
        duk_push_null(ctx);
    }
    return 1;
}

static duk_ret_t js_set_text_content(duk_context* ctx) {
    uint32_t id = s_this_node(ctx);
    const char* value;
    duk_size_t len = 0u;

    value = duk_get_lstring(ctx, 0, &len);
    if (!value) {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "setTextContent: bad value");
    }
    if (len >= SWEEP_HTML_TEXT_MAX) {
        (void) duk_error(ctx, DUK_ERR_RANGE_ERROR, "setTextContent: exceeds DOM text bound");
    }
    if (!sweep_dom_set_text_content(s_dom, id, value)) {
        (void) duk_error(ctx, DUK_ERR_ERROR, "setTextContent: mutation refused");
    }
    duk_push_undefined(ctx);
    return 1;
}

static duk_ret_t js_text_content_get(duk_context* ctx) {
    uint32_t id = s_this_node(ctx);
    char out[SWEEP_HTML_TEXT_MAX * 4u];

    sweep_dom_text_content(s_dom, id, out, sizeof(out));
    duk_push_lstring(ctx, out, strlen(out));
    return 1;
}

static duk_ret_t js_node_name_get(duk_context* ctx) {
    uint32_t id = s_this_node(ctx);
    const SWEEP_DOM_NODE* n = sweep_dom_get(s_dom, id);

    if (!n) {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "stale DOM element wrapper");
    }
    duk_push_lstring(ctx, n->name, strlen(n->name));
    return 1;
}

static duk_ret_t js_children_get(duk_context* ctx) {
    uint32_t id = s_this_node(ctx);
    const SWEEP_DOM_NODE* n = sweep_dom_get(s_dom, id);
    uint32_t i;
    uint32_t out = 0u;

    if (!n) {
        (void) duk_error(ctx, DUK_ERR_TYPE_ERROR, "stale DOM element wrapper");
    }
    duk_push_array(ctx);
    for (i = 0u; i < n->child_count; ++i) {
        const SWEEP_DOM_NODE* child = sweep_dom_get(s_dom, n->children[i]);
        if (!child || child->type != SWEEP_DOM_ELEMENT) {
            continue;
        }
        s_push_wrapper(ctx, n->children[i]);
        duk_put_prop_index(ctx, -2, (duk_uarridx_t) out++);
    }
    return 1;
}

/* ---- installation ------------------------------------------------------- */

static void s_install_proto(duk_context* ctx) {
    /* proto */
    duk_push_object(ctx);                          /* proto, top of stack */

    duk_push_c_function(ctx, js_get_attribute, 1);
    duk_put_prop_string(ctx, -2, "getAttribute");
    duk_push_c_function(ctx, js_set_text_content, 1);
    duk_put_prop_string(ctx, -2, "setTextContent");
    s_def_accessor(ctx, "textContent", js_text_content_get, 0);
    s_def_accessor(ctx, "nodeName", js_node_name_get, 0);
    s_def_accessor(ctx, "children", js_children_get, 0);

    /* Stash the prototype on the global object under a hidden key, so
     * s_push_wrapper can find it with plain property reads. */
    duk_push_global_object(ctx);                   /* global on top          */
    duk_push_string(ctx, "__kapa_proto");
    duk_dup(ctx, -3);                              /* dup proto (obj=-2 now) */
    duk_def_prop(ctx, -3,
                 DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_CLEAR_ENUMERABLE |
                 DUK_DEFPROP_CLEAR_WRITABLE | DUK_DEFPROP_CLEAR_CONFIGURABLE);
    duk_pop(ctx);                                  /* drop global            */
}

bool kapa_dom_js_install(duk_context* ctx, SWEEP_DOM* dom) {
    if (!ctx || !dom) {
        return false;
    }
    if (s_dom) {
        return false;             /* one live binding per image in R7 */
    }
    s_dom = dom;

    s_install_proto(ctx);                           /* __kapa_proto          */

    /* document global: non-enumerable, non-configurable, not writable. */
    duk_push_object(ctx);                           /* document object       */
    duk_push_c_function(ctx, js_get_element_by_id, 1);
    duk_put_prop_string(ctx, -2, "getElementById");
    duk_push_global_object(ctx);
    duk_push_string(ctx, "document");
    duk_dup(ctx, -3);                               /* dup document object    */
    duk_def_prop(ctx, -3,
                 DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_SET_WRITABLE |
                 DUK_DEFPROP_CLEAR_ENUMERABLE | DUK_DEFPROP_CLEAR_CONFIGURABLE);
    duk_pop(ctx);                                   /* drop global           */
    duk_pop(ctx);                                   /* drop document object  */
    duk_pop(ctx);                                   /* drop proto            */
    return true;
}

void kapa_dom_js_release(void) {
    s_dom = (SWEEP_DOM*) 0;
}
