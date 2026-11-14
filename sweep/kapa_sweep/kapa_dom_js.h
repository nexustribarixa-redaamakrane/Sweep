/* kapa_dom_js.h - narrow JavaScript DOM bindings for the R7 KAPA integration
 * test.  Application-side on purpose: no DOM symbols are added to the kernel.
 * The existing Sweep DOM (sweep_html.h) remains the single source of truth;
 * these bindings are thin, validated adapters over it. */
#ifndef KAPA_DOM_JS_H
#define KAPA_DOM_JS_H

#include <stdbool.h>

#include "duktape.h"
#include "sweep_html.h"

/* Binds the JS-facing document API onto the current Duktape context, bound
 * to `dom`.  Installs the `document` global with getElementById; every
 * element wrapper carries a non-writable hidden node id and generation that
 * are validated against the DOM slot's generation on every access, so a
 * detached/reused slot can never be followed through a stale wrapper.
 *
 * Returns true when the binding was installed (a single live installation at
 * a time per process image; a previous binding must be released with
 * kapa_dom_js_release before rebinding). */
bool kapa_dom_js_install(duk_context* ctx, SWEEP_DOM* dom);

/* Drops the binding's reference to the DOM and clears the cached bindings
 * state.  The DOM itself is owned by the caller; destroy/unbind order is
 * heap-first for the JS side, then the DOM. */
void kapa_dom_js_release(void);

#endif /* KAPA_DOM_JS_H */
