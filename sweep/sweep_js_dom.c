/* sweep_js_dom.c - mediated DOM bindings over sweep_html. */
#include "sweep_js_dom.h"

#include <ctype.h>
#include <string.h>

#include "sweep_css.h"

#define SWEEP_JS_NODE_KEY       "__sweep_node"
#define SWEEP_JS_GEN_KEY        "__sweep_gen"
#define SWEEP_JS_WRAPPERS_KEY   "__sweep_wrappers"
#define SWEEP_JS_PROTO_NODE_KEY "__sweep_proto_node"
#define SWEEP_JS_PROTO_ELEM_KEY "__sweep_proto_element"
#define SWEEP_JS_PROTO_DOC_KEY  "__sweep_proto_document"
#define SWEEP_JS_HTML_MAX       (SWEEP_DOM_SERIALIZE_MAX * 2u)

static void copy_lower(char* dst, size_t cap, const char* src) {
    size_t i = 0u;
    if (!dst || cap == 0u) return;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] && i + 1u < cap) {
        dst[i] = (char) tolower((unsigned char) src[i]);
        ++i;
    }
    dst[i] = '\0';
}

static bool valid_attr_name(const char* name) {
    size_t i;
    if (!name || name[0] == '\0') return false;
    for (i = 0u; name[i]; ++i) {
        unsigned char c = (unsigned char) name[i];
        if (isspace(c) || c == '=' || c == '<' || c == '>' || c == '/' ||
            c == '"' || c == '\'') {
            return false;
        }
    }
    return true;
}

static bool arg_text(SWEEP_JS_CONTEXT* context, int index, char* out,
                     size_t cap, size_t* out_len) {
    size_t len = 0u;
    const char* p = sweep_js_engine_to_lstring(context->engine, index, &len);
    if (!p) p = "";
    return sweep_js_make_dom_text(context, p, len, out, cap, out_len);
}

/* Resolve an object stack index to a genuine, cached DOM wrapper. */
static bool object_node_id(SWEEP_JS_CONTEXT* context, int index,
                           uint32_t* out_id) {
    SWEEP_JS_ENGINE* engine = context->engine;
    int orig = sweep_js_engine_get_top(engine);
    int obj = index < 0 ? orig + index : index;
    double raw;
    uint32_t id;
    bool ok = false;

    if (obj < 0 || obj >= orig ||
        !sweep_js_engine_is_object(engine, obj)) {
        (void) sweep_js_engine_throw_type_error(engine, "not a DOM node");
        return false;
    }
    (void) sweep_js_engine_get_prop_string(engine, obj, SWEEP_JS_NODE_KEY);
    raw = sweep_js_engine_get_number(engine, orig);
    id = (raw >= 1.0 && raw <= (double) SWEEP_DOM_NODES_MAX)
        ? (uint32_t) raw : 0u;
    if (id != 0u) {
        (void) sweep_js_engine_get_stash(engine, SWEEP_JS_WRAPPERS_KEY);
        (void) sweep_js_engine_get_prop_index(engine, orig + 1, id - 1u);
        ok = sweep_js_engine_is_object(engine, orig + 2) &&
             sweep_js_engine_strict_equals(engine, obj, orig + 2);
        /* Generation check: a wrapper captured before its slot was reclaimed
         * and reused must be rejected, not followed into the new node. */
        if (ok) {
            (void) sweep_js_engine_get_prop_string(engine, obj,
                                                   SWEEP_JS_GEN_KEY);
            raw = sweep_js_engine_get_number(engine,
                                             sweep_js_engine_get_top(engine) - 1);
            if ((double) sweep_dom_node_generation(context->dom, id) != raw) ok = false;
        }
    }
    sweep_js_engine_set_top(engine, orig);
    if (!ok || !sweep_dom_get(context->dom, id)) {
        (void) sweep_js_engine_throw_type_error(engine, "invalid DOM node");
        return false;
    }
    *out_id = id;
    return true;
}

bool sweep_js_dom_this_node(SWEEP_JS_CONTEXT* context, uint32_t* out_id) {
    SWEEP_JS_ENGINE* engine = context->engine;
    int orig = sweep_js_engine_get_top(engine);
    bool ok;
    sweep_js_engine_push_this(engine);
    ok = object_node_id(context, orig, out_id);
    sweep_js_engine_set_top(engine, orig);
    return ok;
}

bool sweep_js_dom_arg_node(SWEEP_JS_CONTEXT* context, int index,
                           uint32_t* out_id) {
    return object_node_id(context, index, out_id);
}

bool sweep_js_dom_push_node(SWEEP_JS_CONTEXT* context, uint32_t id) {
    SWEEP_JS_ENGINE* engine = context->engine;
    const SWEEP_DOM_NODE* node;
    const char* proto_key;
    if (id == 0u) {
        sweep_js_engine_push_null(engine);
        return true;
    }
    node = sweep_dom_get(context->dom, id);
    if (!node) {
        sweep_js_engine_push_null(engine);
        return true;
    }
    (void) sweep_js_engine_get_stash(engine, SWEEP_JS_WRAPPERS_KEY);
    (void) sweep_js_engine_get_prop_index(engine, -1, id - 1u);
    if (sweep_js_engine_is_object(engine, -1)) {
        /* Cached wrapper is only valid if its generation matches the live
         * slot; a stale entry (slot reclaimed + reused) is discarded and a
         * fresh wrapper created below. */
        int top = sweep_js_engine_get_top(engine);
        (void) sweep_js_engine_get_prop_string(engine, top - 1,
                                               SWEEP_JS_GEN_KEY);
        {
            double gen = sweep_js_engine_get_number(engine,
                                                    sweep_js_engine_get_top(engine) - 1);
            /* Stack is [wrappers, cached, gen]; drop gen first, then the
             * original cleanup (remove wrappers, return cached) applies. */
            sweep_js_engine_set_top(engine, top);
            if (gen == (double) node->generation) {
                sweep_js_engine_remove(engine, -2);
                return true;
            }
        }
        sweep_js_engine_set_top(engine, top - 2);
    } else {
        sweep_js_engine_set_top(engine, sweep_js_engine_get_top(engine) - 2);
    }

    sweep_js_engine_push_object(engine);
    (void) sweep_js_engine_def_hidden_number(engine, -1, SWEEP_JS_NODE_KEY,
                                             (double) id);
    (void) sweep_js_engine_def_hidden_number(engine, -1, SWEEP_JS_GEN_KEY,
                                             (double) node->generation);
    if (node->type == SWEEP_DOM_DOCUMENT) proto_key = SWEEP_JS_PROTO_DOC_KEY;
    else if (node->type == SWEEP_DOM_ELEMENT) proto_key = SWEEP_JS_PROTO_ELEM_KEY;
    else proto_key = SWEEP_JS_PROTO_NODE_KEY;
    (void) sweep_js_engine_get_stash(engine, proto_key);
    (void) sweep_js_engine_set_prototype(engine, -2, -1);
    sweep_js_engine_remove(engine, -1);       /* prototype */
    (void) sweep_js_engine_get_stash(engine, SWEEP_JS_WRAPPERS_KEY);
    sweep_js_engine_dup(engine, -2);          /* the wrapper */
    (void) sweep_js_engine_put_prop_index(engine, -2, id - 1u);
    sweep_js_engine_remove(engine, -1);       /* wrapper array */
    return true;
}

bool sweep_js_dom_push_node_prototype(SWEEP_JS_CONTEXT* context) {
    return sweep_js_engine_get_stash(context->engine, SWEEP_JS_PROTO_NODE_KEY);
}

/* ---- shared accessors/methods ---- */

static int get_parent(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    const SWEEP_DOM_NODE* n;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    if (!n || n->parent == 0u) sweep_js_engine_push_null(engine);
    else (void) sweep_js_dom_push_node(context, n->parent);
    return 1;
}

static int get_first_child(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    const SWEEP_DOM_NODE* n;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    if (!n || n->child_count == 0u) sweep_js_engine_push_null(engine);
    else (void) sweep_js_dom_push_node(context, n->children[0]);
    return 1;
}

static int get_last_child(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    const SWEEP_DOM_NODE* n;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    if (!n || n->child_count == 0u) sweep_js_engine_push_null(engine);
    else (void) sweep_js_dom_push_node(context, n->children[n->child_count - 1u]);
    return 1;
}

static int get_children(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id, i, out = 0u;
    const SWEEP_DOM_NODE* n;
    int arr;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    sweep_js_engine_push_array(engine);
    arr = sweep_js_engine_get_top(engine) - 1;
    if (n) {
        for (i = 0u; i < n->child_count; ++i) {
            const SWEEP_DOM_NODE* c = sweep_dom_get(context->dom, n->children[i]);
            if (c && c->type == SWEEP_DOM_ELEMENT) {
                if (!sweep_js_dom_push_node(context, n->children[i])) return 0;
                (void) sweep_js_engine_put_prop_index(engine, arr, out++);
            }
        }
    }
    return 1;
}

static int get_text_content(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char text[SWEEP_DOM_SERIALIZE_MAX];
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    sweep_dom_text_content(context->dom, id, text, sizeof(text));
    if (!sweep_js_push_dom_text(context, text)) {
        (void) sweep_js_engine_throw_range_error(engine, "textContent too large");
        return 0;
    }
    return 1;
}

static int set_text_content(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char text[SWEEP_HTML_TEXT_MAX];
    size_t len = 0u;
    const SWEEP_DOM_NODE* n;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    if (!n || n->type == SWEEP_DOM_DOCUMENT) {
        (void) sweep_js_engine_throw_error(engine,
            "textContent cannot be set on the document");
        return 0;
    }
    if (!arg_text(context, 0, text, sizeof(text), &len)) {
        (void) sweep_js_engine_throw_range_error(engine, "textContent too large");
        return 0;
    }
    if (!sweep_dom_set_text_content(context->dom, id, text)) {
        (void) sweep_js_engine_throw_error(engine, "textContent update failed");
        return 0;
    }
    return 0;
}

static int append_child(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t parent, child;
    const SWEEP_DOM_NODE* c;
    if (!sweep_js_dom_this_node(context, &parent)) return 0;
    if (!sweep_js_dom_arg_node(context, 0, &child)) return 0;
    c = sweep_dom_get(context->dom, child);
    if (c && c->type == SWEEP_DOM_DOCUMENT) {
        (void) sweep_js_engine_throw_error(engine, "cannot append a document");
        return 0;
    }
    if (!sweep_dom_append_child(context->dom, parent, child)) {
        (void) sweep_js_engine_throw_error(engine,
            "appendChild rejected (cycle or DOM limit)");
        return 0;
    }
    (void) sweep_js_dom_push_node(context, child);
    return 1;
}

static int insert_before(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t parent, child, ref = 0u;
    const SWEEP_DOM_NODE* c;
    if (!sweep_js_dom_this_node(context, &parent)) return 0;
    if (!sweep_js_dom_arg_node(context, 0, &child)) return 0;
    if (!sweep_js_engine_is_nullish(engine, 1)) {
        if (!sweep_js_dom_arg_node(context, 1, &ref)) return 0;
    }
    c = sweep_dom_get(context->dom, child);
    if (c && c->type == SWEEP_DOM_DOCUMENT) {
        (void) sweep_js_engine_throw_error(engine, "cannot insert a document");
        return 0;
    }
    if (!sweep_dom_insert_before(context->dom, parent, child, ref)) {
        (void) sweep_js_engine_throw_error(engine,
            "insertBefore rejected (reference, cycle or DOM limit)");
        return 0;
    }
    (void) sweep_js_dom_push_node(context, child);
    return 1;
}

static int remove_child(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t parent, child;
    const SWEEP_DOM_NODE* c;
    if (!sweep_js_dom_this_node(context, &parent)) return 0;
    if (!sweep_js_dom_arg_node(context, 0, &child)) return 0;
    c = sweep_dom_get(context->dom, child);
    if (!c || c->parent != parent) {
        (void) sweep_js_engine_throw_error(engine,
            "removeChild: node is not a child of this parent");
        return 0;
    }
    if (!sweep_dom_remove_child(context->dom, child)) {
        (void) sweep_js_engine_throw_error(engine, "removeChild failed");
        return 0;
    }
    (void) sweep_js_dom_push_node(context, child);
    return 1;
}

/* ---- element ---- */

static int get_id(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    const SWEEP_DOM_NODE* n;
    const char* v;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    v = n ? sweep_dom_get_attr(n, "id") : NULL;
    if (!sweep_js_push_dom_text(context, v ? v : "")) {
        (void) sweep_js_engine_throw_range_error(engine, "id too large");
        return 0;
    }
    return 1;
}

static int set_attr_value(SWEEP_JS_ENGINE* engine, const char* name, int index) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char value[SWEEP_HTML_ATTR_VAL_MAX];
    size_t len = 0u;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!arg_text(context, index, value, sizeof(value), &len)) {
        (void) sweep_js_engine_throw_range_error(engine,
            "attribute value exceeds DOM limit");
        return 0;
    }
    if (!sweep_dom_set_attr(context->dom, id, name, value)) {
        (void) sweep_js_engine_throw_error(engine, "attribute update failed");
        return 0;
    }
    return 0;
}

static int set_id(SWEEP_JS_ENGINE* engine) {
    return set_attr_value(engine, "id", 0);
}

static int get_class_name(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    const SWEEP_DOM_NODE* n;
    const char* v;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    v = n ? sweep_dom_get_attr(n, "class") : NULL;
    if (!sweep_js_push_dom_text(context, v ? v : "")) {
        (void) sweep_js_engine_throw_range_error(engine, "className too large");
        return 0;
    }
    return 1;
}

static int set_class_name(SWEEP_JS_ENGINE* engine) {
    return set_attr_value(engine, "class", 0);
}

static int get_tag_name(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id, i;
    const SWEEP_DOM_NODE* n;
    char name[SWEEP_HTML_NAME_MAX];
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    n = sweep_dom_get(context->dom, id);
    if (!n) { sweep_js_engine_push_string(engine, ""); return 1; }
    for (i = 0u; n->name[i] && i + 1u < sizeof(name); ++i) {
        name[i] = (char) toupper((unsigned char) n->name[i]);
    }
    name[i] = '\0';
    if (!sweep_js_push_dom_text(context, name)) {
        (void) sweep_js_engine_throw_range_error(engine, "tagName too large");
        return 0;
    }
    return 1;
}

static int get_inner_html(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char html[SWEEP_DOM_SERIALIZE_MAX];
    size_t len = 0u;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!sweep_dom_get_inner_html(context->dom, id, html, sizeof(html), &len) ||
        len >= sizeof(html) - 1u) {
        (void) sweep_js_engine_throw_range_error(engine, "innerHTML too large");
        return 0;
    }
    if (!sweep_js_push_dom_text(context, html)) {
        (void) sweep_js_engine_throw_range_error(engine, "innerHTML too large");
        return 0;
    }
    return 1;
}

static int set_inner_html(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char html[SWEEP_JS_HTML_MAX];
    size_t len = 0u;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!arg_text(context, 0, html, sizeof(html), &len)) {
        (void) sweep_js_engine_throw_range_error(engine, "innerHTML too large");
        return 0;
    }
    if (!sweep_dom_set_inner_html(context->dom, id, html, len)) {
        (void) sweep_js_engine_throw_error(engine, "innerHTML update failed");
        return 0;
    }
    return 0;
}

static int get_attribute(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char name[SWEEP_HTML_NAME_MAX], lower[SWEEP_HTML_NAME_MAX];
    size_t len = 0u;
    const SWEEP_DOM_NODE* n;
    const char* v;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!arg_text(context, 0, name, sizeof(name), &len) ||
        !valid_attr_name(name)) {
        (void) sweep_js_engine_throw_error(engine, "invalid attribute name");
        return 0;
    }
    copy_lower(lower, sizeof(lower), name);
    n = sweep_dom_get(context->dom, id);
    v = n ? sweep_dom_get_attr(n, lower) : NULL;
    if (!v) { sweep_js_engine_push_null(engine); return 1; }
    if (!sweep_js_push_dom_text(context, v)) {
        (void) sweep_js_engine_throw_range_error(engine, "attribute too large");
        return 0;
    }
    return 1;
}

static int set_attribute(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char name[SWEEP_HTML_NAME_MAX], value[SWEEP_HTML_ATTR_VAL_MAX];
    size_t nlen = 0u, vlen = 0u;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!arg_text(context, 0, name, sizeof(name), &nlen) ||
        !valid_attr_name(name)) {
        (void) sweep_js_engine_throw_error(engine, "invalid attribute name");
        return 0;
    }
    if (!arg_text(context, 1, value, sizeof(value), &vlen)) {
        (void) sweep_js_engine_throw_range_error(engine,
            "attribute value exceeds DOM limit");
        return 0;
    }
    if (!sweep_dom_set_attr(context->dom, id, name, value)) {
        (void) sweep_js_engine_throw_error(engine, "attribute update failed");
        return 0;
    }
    return 0;
}

static int remove_attribute(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id;
    char name[SWEEP_HTML_NAME_MAX];
    size_t len = 0u;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!arg_text(context, 0, name, sizeof(name), &len) ||
        !valid_attr_name(name)) {
        (void) sweep_js_engine_throw_error(engine, "invalid attribute name");
        return 0;
    }
    (void) sweep_dom_remove_attr(context->dom, id, name);
    return 0;
}

/* ---- queries ---- */

static bool tag_is(const SWEEP_DOM_NODE* n, const char* wanted) {
    return n && n->type == SWEEP_DOM_ELEMENT &&
           (wanted[0] == '*' || strcmp(n->name, wanted) == 0);
}

static bool collect_tags(SWEEP_JS_CONTEXT* context, uint32_t root,
                         const char* wanted, int arr, uint32_t* count) {
    const SWEEP_DOM_NODE* n = sweep_dom_get(context->dom, root);
    uint32_t i;
    if (!n) return true;
    for (i = 0u; i < n->child_count; ++i) {
        const SWEEP_DOM_NODE* c = sweep_dom_get(context->dom, n->children[i]);
        if (tag_is(c, wanted)) {
            if (!sweep_js_dom_push_node(context, n->children[i])) return false;
            (void) sweep_js_engine_put_prop_index(context->engine, arr, (*count)++);
        }
        if (!collect_tags(context, n->children[i], wanted, arr, count)) {
            return false;
        }
    }
    return true;
}

static bool find_selector(const SWEEP_DOM* dom, uint32_t root,
                          const SWEEP_CSS_SELECTOR* selector, uint32_t* found) {
    const SWEEP_DOM_NODE* n = sweep_dom_get(dom, root);
    uint32_t i;
    if (!n) return false;
    for (i = 0u; i < n->child_count; ++i) {
        const SWEEP_DOM_NODE* c = sweep_dom_get(dom, n->children[i]);
        if (c && c->type == SWEEP_DOM_ELEMENT &&
            sweep_css_selector_matches(dom, n->children[i], selector)) {
            *found = n->children[i];
            return true;
        }
        if (find_selector(dom, n->children[i], selector, found)) return true;
    }
    return false;
}

static bool collect_selector(SWEEP_JS_CONTEXT* context, uint32_t root,
                             const SWEEP_CSS_SELECTOR* selector, int arr,
                             uint32_t* count) {
    const SWEEP_DOM_NODE* n = sweep_dom_get(context->dom, root);
    uint32_t i;
    if (!n) return true;
    for (i = 0u; i < n->child_count; ++i) {
        const SWEEP_DOM_NODE* c = sweep_dom_get(context->dom, n->children[i]);
        if (c && c->type == SWEEP_DOM_ELEMENT &&
            sweep_css_selector_matches(context->dom, n->children[i], selector)) {
            if (!sweep_js_dom_push_node(context, n->children[i])) return false;
            (void) sweep_js_engine_put_prop_index(context->engine, arr, (*count)++);
        }
        if (!collect_selector(context, n->children[i], selector, arr, count)) {
            return false;
        }
    }
    return true;
}

static bool parse_selector_arg(SWEEP_JS_CONTEXT* context, int index,
                               SWEEP_CSS_SELECTOR* selector, char* text,
                               size_t cap) {
    size_t len = 0u;
    if (!arg_text(context, index, text, cap, &len)) {
        (void) sweep_js_engine_throw_error(context->engine, "selector too long");
        return false;
    }
    if (!sweep_css_parse_selector(text, len, selector)) {
        (void) sweep_js_engine_throw_error(context->engine,
            "unsupported or invalid selector (single selector only)");
        return false;
    }
    return true;
}

static int query_selector(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    SWEEP_CSS_SELECTOR selector;
    char text[SWEEP_JS_STRING_MAX];
    uint32_t id, found = 0u;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!parse_selector_arg(context, 0, &selector, text, sizeof(text))) return 0;
    (void) find_selector(context->dom, id, &selector, &found);
    (void) sweep_js_dom_push_node(context, found);
    return 1;
}

static int query_selector_all(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    SWEEP_CSS_SELECTOR selector;
    char text[SWEEP_JS_STRING_MAX];
    uint32_t id, count = 0u;
    int arr;
    if (!sweep_js_dom_this_node(context, &id)) return 0;
    if (!parse_selector_arg(context, 0, &selector, text, sizeof(text))) return 0;
    sweep_js_engine_push_array(engine);
    arr = sweep_js_engine_get_top(engine) - 1;
    if (!collect_selector(context, id, &selector, arr, &count)) {
        (void) sweep_js_engine_throw_error(engine, "querySelectorAll failed");
        return 0;
    }
    return 1;
}

/* ---- document ---- */

static uint32_t document_element_id(const SWEEP_DOM* dom) {
    const SWEEP_DOM_NODE* doc = sweep_dom_get(dom, dom->document);
    uint32_t i;
    if (!doc) return 0u;
    for (i = 0u; i < doc->child_count; ++i) {
        const SWEEP_DOM_NODE* c = sweep_dom_get(dom, doc->children[i]);
        if (c && c->type == SWEEP_DOM_ELEMENT) return doc->children[i];
    }
    return 0u;
}

static int get_document_element(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    (void) sweep_js_dom_push_node(context, document_element_id(context->dom));
    return 1;
}

static int get_body(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id = sweep_dom_find_element(context->dom, context->dom->document,
                                         "body");
    (void) sweep_js_dom_push_node(context, id);
    return 1;
}

static int get_title(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    uint32_t id = sweep_dom_find_element(context->dom, context->dom->document,
                                         "title");
    char text[SWEEP_HTML_TEXT_MAX];
    if (id == 0u) { sweep_js_engine_push_string(engine, ""); return 1; }
    sweep_dom_text_content(context->dom, id, text, sizeof(text));
    if (!sweep_js_push_dom_text(context, text)) {
        (void) sweep_js_engine_throw_range_error(engine, "title too large");
        return 0;
    }
    return 1;
}

static int set_title(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    char text[SWEEP_HTML_TEXT_MAX];
    size_t len = 0u;
    uint32_t title, parent, text_node;
    if (!arg_text(context, 0, text, sizeof(text), &len)) {
        (void) sweep_js_engine_throw_range_error(engine, "title too large");
        return 0;
    }
    title = sweep_dom_find_element(context->dom, context->dom->document,
                                   "title");
    if (title != 0u) {
        if (!sweep_dom_set_text_content(context->dom, title, text)) {
            (void) sweep_js_engine_throw_error(engine, "title update failed");
            return 0;
        }
        return 0;
    }
    title = sweep_dom_create_element(context->dom, "title");
    if (title == 0u) {
        (void) sweep_js_engine_throw_range_error(engine, "DOM node limit");
        return 0;
    }
    if (text[0] != '\0') {
        text_node = sweep_dom_create_text(context->dom, text);
        if (text_node == 0u ||
            !sweep_dom_append_child(context->dom, title, text_node)) {
            (void) sweep_js_engine_throw_range_error(engine, "DOM node limit");
            return 0;
        }
    }
    parent = sweep_dom_find_element(context->dom, context->dom->document,
                                    "head");
    if (parent == 0u) parent = document_element_id(context->dom);
    if (parent == 0u) parent = context->dom->document;
    if (!sweep_dom_append_child(context->dom, parent, title)) {
        (void) sweep_js_engine_throw_error(engine, "title insertion failed");
        return 0;
    }
    return 0;
}

static int get_element_by_id(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    char id[SWEEP_HTML_ATTR_VAL_MAX];
    size_t len = 0u;
    uint32_t found;
    if (!arg_text(context, 0, id, sizeof(id), &len)) {
        (void) sweep_js_engine_throw_range_error(engine, "id too large");
        return 0;
    }
    found = id[0] ? sweep_dom_find_by_id(context->dom,
                                         context->dom->document, id) : 0u;
    (void) sweep_js_dom_push_node(context, found);
    return 1;
}

static int get_elements_by_tag_name(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    char name[SWEEP_HTML_NAME_MAX], lower[SWEEP_HTML_NAME_MAX];
    size_t len = 0u;
    uint32_t count = 0u;
    int arr;
    if (!arg_text(context, 0, name, sizeof(name), &len) || len == 0u) {
        (void) sweep_js_engine_throw_error(engine, "invalid tag name");
        return 0;
    }
    copy_lower(lower, sizeof(lower), name);
    sweep_js_engine_push_array(engine);
    arr = sweep_js_engine_get_top(engine) - 1;
    if (!collect_tags(context, context->dom->document, lower, arr, &count)) {
        (void) sweep_js_engine_throw_error(engine, "getElementsByTagName failed");
        return 0;
    }
    return 1;
}

static int create_element(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    char name[SWEEP_HTML_NAME_MAX], lower[SWEEP_HTML_NAME_MAX];
    size_t len = 0u;
    uint32_t id;
    if (!arg_text(context, 0, name, sizeof(name), &len) || len == 0u ||
        len >= SWEEP_HTML_NAME_MAX) {
        (void) sweep_js_engine_throw_error(engine, "invalid element name");
        return 0;
    }
    copy_lower(lower, sizeof(lower), name);
    id = sweep_dom_create_element(context->dom, lower);
    if (id == 0u) {
        (void) sweep_js_engine_throw_range_error(engine, "DOM node limit reached");
        return 0;
    }
    (void) sweep_js_dom_push_node(context, id);
    return 1;
}

static int create_text_node(SWEEP_JS_ENGINE* engine) {
    SWEEP_JS_CONTEXT* context = (SWEEP_JS_CONTEXT*) sweep_js_engine_user_data(engine);
    char text[SWEEP_HTML_TEXT_MAX];
    size_t len = 0u;
    uint32_t id;
    if (!arg_text(context, 0, text, sizeof(text), &len)) {
        (void) sweep_js_engine_throw_range_error(engine, "text node too large");
        return 0;
    }
    id = sweep_dom_create_text(context->dom, text);
    if (id == 0u) {
        (void) sweep_js_engine_throw_range_error(engine, "DOM node limit reached");
        return 0;
    }
    (void) sweep_js_dom_push_node(context, id);
    return 1;
}

/* ---- prototype installation ---- */

static void def_method(SWEEP_JS_CONTEXT* context, int proto, const char* name,
                       SWEEP_JS_ENGINE_CFUNC fn, int nargs) {
    SWEEP_JS_ENGINE* engine = context->engine;
    sweep_js_engine_push_c_function(engine, fn, nargs);
    (void) sweep_js_engine_put_prop_string(engine, proto, name);
}

static bool install_node_prototype(SWEEP_JS_CONTEXT* context) {
    SWEEP_JS_ENGINE* engine = context->engine;
    int proto;
    sweep_js_engine_push_object(engine);
    proto = sweep_js_engine_get_top(engine) - 1;
    if (!sweep_js_engine_def_accessor(engine, proto, "parentNode", get_parent,
                                      NULL) ||
        !sweep_js_engine_def_accessor(engine, proto, "firstChild",
                                      get_first_child, NULL) ||
        !sweep_js_engine_def_accessor(engine, proto, "lastChild",
                                      get_last_child, NULL) ||
        !sweep_js_engine_def_accessor(engine, proto, "children", get_children,
                                      NULL) ||
        !sweep_js_engine_def_accessor(engine, proto, "textContent",
                                      get_text_content, set_text_content)) {
        return false;
    }
    def_method(context, proto, "appendChild", append_child, 1);
    def_method(context, proto, "insertBefore", insert_before, 2);
    def_method(context, proto, "removeChild", remove_child, 1);
    return sweep_js_engine_put_stash(engine, SWEEP_JS_PROTO_NODE_KEY);
}

static bool install_element_prototype(SWEEP_JS_CONTEXT* context) {
    SWEEP_JS_ENGINE* engine = context->engine;
    int proto;
    sweep_js_engine_push_object(engine);
    proto = sweep_js_engine_get_top(engine) - 1;
    (void) sweep_js_engine_get_stash(engine, SWEEP_JS_PROTO_NODE_KEY);
    (void) sweep_js_engine_set_prototype(engine, proto, -1);
    sweep_js_engine_remove(engine, -1);
    if (!sweep_js_engine_def_accessor(engine, proto, "id", get_id, set_id) ||
        !sweep_js_engine_def_accessor(engine, proto, "className",
                                      get_class_name, set_class_name) ||
        !sweep_js_engine_def_accessor(engine, proto, "tagName", get_tag_name,
                                      NULL) ||
        !sweep_js_engine_def_accessor(engine, proto, "innerHTML",
                                      get_inner_html, set_inner_html)) {
        return false;
    }
    def_method(context, proto, "getAttribute", get_attribute, 1);
    def_method(context, proto, "setAttribute", set_attribute, 2);
    def_method(context, proto, "removeAttribute", remove_attribute, 1);
    def_method(context, proto, "querySelector", query_selector, 1);
    def_method(context, proto, "querySelectorAll", query_selector_all, 1);
    return sweep_js_engine_put_stash(engine, SWEEP_JS_PROTO_ELEM_KEY);
}

static bool install_document_prototype(SWEEP_JS_CONTEXT* context) {
    SWEEP_JS_ENGINE* engine = context->engine;
    int proto;
    sweep_js_engine_push_object(engine);
    proto = sweep_js_engine_get_top(engine) - 1;
    (void) sweep_js_engine_get_stash(engine, SWEEP_JS_PROTO_NODE_KEY);
    (void) sweep_js_engine_set_prototype(engine, proto, -1);
    sweep_js_engine_remove(engine, -1);
    if (!sweep_js_engine_def_accessor(engine, proto, "documentElement",
                                      get_document_element, NULL) ||
        !sweep_js_engine_def_accessor(engine, proto, "body", get_body, NULL) ||
        !sweep_js_engine_def_accessor(engine, proto, "title", get_title,
                                      set_title)) {
        return false;
    }
    def_method(context, proto, "getElementById", get_element_by_id, 1);
    def_method(context, proto, "getElementsByTagName",
               get_elements_by_tag_name, 1);
    def_method(context, proto, "createElement", create_element, 1);
    def_method(context, proto, "createTextNode", create_text_node, 1);
    def_method(context, proto, "querySelector", query_selector, 1);
    def_method(context, proto, "querySelectorAll", query_selector_all, 1);
    return sweep_js_engine_put_stash(engine, SWEEP_JS_PROTO_DOC_KEY);
}

bool sweep_js_dom_install(SWEEP_JS_CONTEXT* context) {
    SWEEP_JS_ENGINE* engine;
    if (!context || !context->engine || !context->dom) return false;
    engine = context->engine;

    sweep_js_engine_push_array(engine);
    if (!sweep_js_engine_put_stash(engine, SWEEP_JS_WRAPPERS_KEY)) return false;
    if (!install_node_prototype(context)) return false;
    if (!install_element_prototype(context)) return false;
    if (!install_document_prototype(context)) return false;

    if (!sweep_js_dom_push_node(context, context->dom->document)) return false;
    return sweep_js_engine_put_global_string(engine, "document");
}
