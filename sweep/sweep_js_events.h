/* sweep_js_events.h - bounded DOM-style events for Sweep JS contexts. */
#ifndef SWEEP_JS_EVENTS_H
#define SWEEP_JS_EVENTS_H

#include <stdbool.h>

#include "sweep_js_internal.h"

/* Installs the global Event constructor and addEventListener /
 * removeEventListener / dispatchEvent on the shared Node prototype. */
bool sweep_js_events_install(SWEEP_JS_CONTEXT* context);

#endif /* SWEEP_JS_EVENTS_H */
