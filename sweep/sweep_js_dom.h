/* sweep_js_dom.h - DOM bindings over the existing sweep_html tree.
 *
 * Wrappers are plain JS objects with a non-writable hidden node id and a
 * shared prototype.  Wrappers are cached per context and validated against the
 * cache on every access, so a script cannot forge a wrapper to reach a node it
 * was not given.  The DOM itself is the single source of truth; this module
 * never creates a second tree.
 */
#ifndef SWEEP_JS_DOM_H
#define SWEEP_JS_DOM_H

#include <stdbool.h>
#include <stdint.h>

#include "sweep_js_internal.h"

/* Creates the Node/Element/Document prototypes, wrapper cache and the global
 * `document` object.  Must be called once per context before events install. */
bool sweep_js_dom_install(SWEEP_JS_CONTEXT* context);

/* Push a wrapper for `id` (or null for id 0 / invalid).  Always pushes one
 * value.  Returns false only for an internal stack error. */
bool sweep_js_dom_push_node(SWEEP_JS_CONTEXT* context, uint32_t id);

/* Validate that the current `this` is a genuine wrapper; on failure throws a
 * TypeError and returns false. */
bool sweep_js_dom_this_node(SWEEP_JS_CONTEXT* context, uint32_t* out_id);

/* Validate a stack argument as a genuine wrapper. */
bool sweep_js_dom_arg_node(SWEEP_JS_CONTEXT* context, int index,
                           uint32_t* out_id);

/* Push the shared Node prototype (used by the event install). */
bool sweep_js_dom_push_node_prototype(SWEEP_JS_CONTEXT* context);

#endif /* SWEEP_JS_DOM_H */
