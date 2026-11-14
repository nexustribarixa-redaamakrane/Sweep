/* sweep_document.h - document lifecycle coordinator (Phase 4).
 *
 * Connects the existing HTML/DOM, CSS, JavaScript, layout and paint-command
 * subsystems into one coherent pipeline:
 *
 *   HTML parse -> DOM -> JS init/scripts -> style -> layout -> paint -> READY
 *
 * DOM mutations (direct C or via JavaScript/Duktape callbacks) never trigger
 * rendering from inside the mutation itself.  They only bump the DOM revision
 * and set dirty flags; sweep_document_update() is the single controlled
 * boundary that recomputes style, layout and paint and resets the paint
 * buffer so stale commands cannot remain visible.
 *
 * Conservative invalidation (documented, correctness first):
 *   - text-content mutations skip the style pass (computed styles read
 *     tags/attrs/tree, not text) but invalidate layout + paint;
 *   - every other mutation (structure, any attribute) invalidates
 *     style + layout + paint, because descendant/attribute selectors make
 *     precise dependency tracking unsafe without a full dependency graph.
 *
 * Paint output is bounded command generation only: there is no physical
 * display compositor and no real glyph rasterizer.
 *
 * Host-only, like the JS layer (Duktape needs a hosted libc).
 */
#ifndef SWEEP_DOCUMENT_H
#define SWEEP_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sweep_css.h"
#include "sweep_engine.h"
#include "sweep_html.h"
#include "sweep_js.h"
#include "sweep_render.h"
#include "sweep_text.h"

#define SWEEP_DOCUMENT_ERROR_MAX 256u
#define SWEEP_DOCUMENT_SHEET_MAX 4096u
#define SWEEP_DOCUMENT_MAX_SHEET_SOURCES 8u
#define SWEEP_DOCUMENT_SHEET_PATH_MAX 128u

typedef enum _SWEEP_DOCUMENT_STATE {
    SWEEP_DOC_EMPTY = 0,   /* created, nothing loaded */
    SWEEP_DOC_PARSING,     /* load in progress (transient) */
    SWEEP_DOC_DOM_READY,   /* DOM built, pipeline not yet run */
    SWEEP_DOC_STYLE_DIRTY, /* style pass pending */
    SWEEP_DOC_LAYOUT_DIRTY,/* layout pass pending */
    SWEEP_DOC_PAINT_DIRTY, /* paint pass pending */
    SWEEP_DOC_READY,       /* style/layout/paint are current */
    SWEEP_DOC_FAILED,      /* pipeline failed; terminal until re-load */
    SWEEP_DOC_DESTROYED    /* destroyed (struct freed; state for logic) */
} SWEEP_DOCUMENT_STATE;

typedef struct _SWEEP_DOCUMENT_STATS {
    uint32_t style_passes;
    uint32_t layout_passes;
    uint32_t paint_passes;
    uint32_t style_nodes;      /* last computed-style node count */
    bool layout_truncated;     /* layout box limit hit on last pass */
    bool paint_truncated;      /* paint command capacity hit on last pass */
    SWEEP_JS_SCRIPT_STATS scripts; /* script pass stats (zeros if no JS) */
    /* Phase 5: stylesheet/dependency accounting (last style pass). */
    uint32_t stylesheet_sources;   /* discovered author sources in document */
    uint32_t stylesheet_links;     /* external <link rel="stylesheet"> found */
    uint32_t stylesheet_loaded;    /* sources successfully loaded/parsed */
    uint32_t stylesheet_errors;    /* sources that failed to load/parse/apply */
    uint32_t selector_tests;       /* selector-match evaluations performed */
    uint32_t rules_skipped;        /* rules skipped by candidate filtering */
    uint32_t style_recompute_skipped; /* style passes avoided (provably unaffected) */
    /* Phase 6: affected-element restyle / stylesheet contribution / layout diagnostics.
     * Counts are deterministic and bounded to actual work performed. */
    uint32_t restyle_candidates;      /* elements considered for restyling */
    uint32_t restyle_applied;         /* elements whose computed style was recomputed */
    uint32_t restyle_skipped;         /* elements preserved (not recomputed) in incremental path */
    uint32_t restyle_subtree_escalations; /* escalations from element to subtree scope */
    uint32_t restyle_full_fallbacks;  /* fallbacks to full-document restyle */
    uint32_t stylesheet_sources_reparsed; /* author sources reparsed this style pass */
    uint32_t stylesheet_sources_reused;   /* author sources reused without reparsing */
    uint32_t stylesheet_combined_rebuilds; /* combined author sheet rebuilds */
    uint32_t dependency_index_rebuilds;    /* dependency index rebuilds */
    uint32_t layout_regions_considered;    /* layout regions/roots considered (bounded) */
    uint32_t layout_full_fallbacks;        /* full-layout fallbacks */
} SWEEP_DOCUMENT_STATS;

typedef enum _SWEEP_RESTYLE_SCOPE {
    SWEEP_RESTYLE_SCOPE_NONE = 0,
    SWEEP_RESTYLE_SCOPE_ELEMENT,   /* affected element(s) only, if safe */
    SWEEP_RESTYLE_SCOPE_SUBTREE,   /* element and descendants */
    SWEEP_RESTYLE_SCOPE_DOCUMENT   /* whole document (conservative) */
} SWEEP_RESTYLE_SCOPE;

typedef enum _SWEEP_RESTYLE_REASON {
    SWEEP_RESTYLE_REASON_NONE = 0,
    SWEEP_RESTYLE_REASON_ATTR,
    SWEEP_RESTYLE_REASON_CLASS,
    SWEEP_RESTYLE_REASON_ID,
    SWEEP_RESTYLE_REASON_STRUCTURE,
    SWEEP_RESTYLE_REASON_STYLESHEET,
    SWEEP_RESTYLE_REASON_INLINE_STYLE,
    SWEEP_RESTYLE_REASON_INHERITANCE,
    SWEEP_RESTYLE_REASON_DESCENDANT_SELECTOR,
    SWEEP_RESTYLE_REASON_ANCESTOR_DEPENDENT,
    SWEEP_RESTYLE_REASON_UNKNOWN
} SWEEP_RESTYLE_REASON;

#define SWEEP_RESTYLE_SET_MAX 32u

typedef struct _SWEEP_RESTYLE_ENTRY {
    uint32_t node_id;
    uint32_t generation; /* generation at invalidation time; stale if mismatched */
    uint8_t scope;       /* SWEEP_RESTYLE_SCOPE */
    uint8_t reason;      /* SWEEP_RESTYLE_REASON */
    bool direct;         /* true for directly affected root, false for propagated */
} SWEEP_RESTYLE_ENTRY;

typedef struct _SWEEP_RESTYLE_SET {
    SWEEP_RESTYLE_ENTRY entries[SWEEP_RESTYLE_SET_MAX];
    uint8_t count;
    bool overflow; /* more affected roots than tracked: treat as document scope */
} SWEEP_RESTYLE_SET;

typedef enum _SWEEP_DOCUMENT_SHEET_KIND {
    SWEEP_SHEET_INLINE = 0,   /* <style> element text */
    SWEEP_SHEET_LINK,         /* <link rel="stylesheet" href=...> */
    SWEEP_SHEET_OVERRIDE      /* host-set via sweep_document_set_stylesheet */
} SWEEP_DOCUMENT_SHEET_KIND;

typedef enum _SWEEP_DOCUMENT_SHEET_STATUS {
    SWEEP_SHEET_PENDING = 0,
    SWEEP_SHEET_LOADED,
    SWEEP_SHEET_ERROR
} SWEEP_DOCUMENT_SHEET_STATUS;

typedef struct _SWEEP_DOCUMENT_SHEET_SOURCE {
    uint32_t id;          /* stable identity (1-based, per-document) */
    uint8_t kind;         /* SWEEP_DOCUMENT_SHEET_KIND */
    uint8_t status;       /* SWEEP_DOCUMENT_SHEET_STATUS */
    bool enabled;
    bool duplicate;       /* duplicate href/reference suppressed */
    uint32_t source_order;/* 0-based document/cascade order */
    uint32_t element_id;  /* DOM node id for inline/link element (0 for override) */
    uint32_t revision;    /* increments when this source's parsed content changes */
    uint16_t rule_count;  /* parsed rules contributed (before merge truncation) */
    char path[SWEEP_DOCUMENT_SHEET_PATH_MAX]; /* href or "<style>" / "<override>" */
    char error[64];       /* bounded diagnostic on error */
} SWEEP_DOCUMENT_SHEET_SOURCE;

typedef struct _SWEEP_DOCUMENT_CONFIG {
    SWEEP_VIEWPORT viewport;       /* zero width/height = default 800x600 */
    SWEEP_TEXT_MODE text_mode;     /* threads JS boundary + layout + paint */
    bool js_enabled;               /* false = DOM/CSS/layout/paint only */
    /* JS runtime knobs (used only when js_enabled).  text_mode here is
     * ignored: the document's text_mode always wins so layout, paint and
     * the JS string boundary can never disagree. */
    size_t js_memory_limit;
    uint32_t js_timeout_ms;
    SWEEP_JS_RESOURCE_FETCH js_fetch;
    void* js_fetch_user_data;
} SWEEP_DOCUMENT_CONFIG;

typedef struct SWEEP_DOCUMENT SWEEP_DOCUMENT;

/* Allocate and zero a document in EMPTY state.  Returns NULL on failure. */
SWEEP_DOCUMENT* sweep_document_create(const SWEEP_DOCUMENT_CONFIG* config);

/* Destroy JS runtime/context, DOM and pipeline buffers (frees `doc`). */
void sweep_document_destroy(SWEEP_DOCUMENT* doc);

/* Load HTML into the document: parse, build the author stylesheet from
 * <style> elements, init JS (if enabled) and run classic <script> elements,
 * then run the full style/layout/paint pipeline.
 * Allowed from EMPTY only.  Script errors never fail the document (they are
 * reported via stats/last_error) so a broken script cannot corrupt the DOM;
 * pipeline failures (invalid viewport, layout failure) set FAILED. */
bool sweep_document_load(SWEEP_DOCUMENT* doc, const char* html, size_t len);

/* Replace the author stylesheet (plain CSS text).  Marks the style pass
 * dirty; call sweep_document_update() to restyle.  Later <style>-element
 * changes via DOM/JS also re-extract the sheet on the next style pass. */
bool sweep_document_set_stylesheet(SWEEP_DOCUMENT* doc, const char* css,
                                   size_t len);

/* Negotiate and install a new viewport.  Marks layout + paint dirty. */
bool sweep_document_set_viewport(SWEEP_DOCUMENT* doc, SWEEP_VIEWPORT viewport);

/* The single controlled rendering boundary.  From DOM_READY/dirty states it
 * runs (style?) -> layout -> paint and reaches READY; from a clean READY it
 * is a no-op.  Rejected (false) from EMPTY/PARSING/FAILED/DESTROYED.
 * A paint-capacity overflow still reaches READY with paint_truncated set. */
bool sweep_document_update(SWEEP_DOCUMENT* doc);

/* Evaluate JavaScript in the document context, then leave the document
 * dirty for sweep_document_update() to process (rendering never happens
 * inside the Duktape callback).  Returns false on JS errors; the document
 * itself stays usable (READY/dirty), never FAILED, on JS errors. */
bool sweep_document_evaluate(SWEEP_DOCUMENT* doc, const char* source,
                             SWEEP_JS_RESULT* out);
bool sweep_document_evaluate_cstr(SWEEP_DOCUMENT* doc, const char* source,
                                  SWEEP_JS_RESULT* out);

/* Run (additional) classic <script> elements found in the DOM.  Scripts run
 * exactly once per element per load; scripts added via innerHTML need this
 * explicit pass (innerHTML never auto-executes). */
bool sweep_document_run_scripts(SWEEP_DOCUMENT* doc, SWEEP_JS_SCRIPT_STATS* stats);

/* Effective state: if stored READY but the DOM revision moved since the last
 * pipeline pass, reports the pending dirty stage instead of READY. */
SWEEP_DOCUMENT_STATE sweep_document_state(const SWEEP_DOCUMENT* doc);
const char* sweep_document_state_name(SWEEP_DOCUMENT_STATE state);

SWEEP_DOM* sweep_document_dom(SWEEP_DOCUMENT* doc);
const SWEEP_DOM* sweep_document_dom_const(const SWEEP_DOCUMENT* doc);
SWEEP_JS_CONTEXT* sweep_document_js_context(SWEEP_DOCUMENT* doc);
SWEEP_TEXT_MODE sweep_document_text_mode(const SWEEP_DOCUMENT* doc);

uint32_t sweep_document_revision(const SWEEP_DOCUMENT* doc);
uint32_t sweep_document_processed_revision(const SWEEP_DOCUMENT* doc);
uint32_t sweep_document_style_revision(const SWEEP_DOCUMENT* doc);
uint32_t sweep_document_paint_revision(const SWEEP_DOCUMENT* doc);
SWEEP_VIEWPORT sweep_document_viewport(const SWEEP_DOCUMENT* doc);

const SWEEP_LAYOUT* sweep_document_layout(const SWEEP_DOCUMENT* doc);
const SWEEP_COMPUTED_STYLE* sweep_document_style_of(const SWEEP_DOCUMENT* doc,
                                                     uint32_t node_id);
const SWEEP_PAINT_CMD* sweep_document_paint(const SWEEP_DOCUMENT* doc,
                                            size_t* out_count);
bool sweep_document_paint_truncated(const SWEEP_DOCUMENT* doc);

/* Phase 8: controlled document-render/presentation integration.
 * Renderer and presentation target are caller-owned/external; the document
 * does not take ownership or free them. Paint revision increments whenever
 * paint commands are regenerated. Rendered output becomes stale when paint
 * changes or viewport/surface-affecting state invalidates the current frame. */
bool sweep_document_attach_renderer(SWEEP_DOCUMENT* doc, SWEEP_RENDERER* renderer);
void sweep_document_detach_renderer(SWEEP_DOCUMENT* doc);
SWEEP_RENDERER* sweep_document_renderer(SWEEP_DOCUMENT* doc);
bool sweep_document_attach_present_target(SWEEP_DOCUMENT* doc, SWEEP_PRESENT_TARGET* target);
void sweep_document_detach_present_target(SWEEP_DOCUMENT* doc);
SWEEP_PRESENT_TARGET* sweep_document_present_target(SWEEP_DOCUMENT* doc);
uint32_t sweep_document_render_revision(const SWEEP_DOCUMENT* doc);
bool sweep_document_render_current(const SWEEP_DOCUMENT* doc);
SWEEP_RENDER_STATUS sweep_document_render(SWEEP_DOCUMENT* doc);
SWEEP_PRESENT_STATUS sweep_document_present(SWEEP_DOCUMENT* doc);

void sweep_document_stats(const SWEEP_DOCUMENT* doc, SWEEP_DOCUMENT_STATS* out);
const char* sweep_document_last_error(const SWEEP_DOCUMENT* doc);

/* Phase 5: stylesheet collection and dependency index (read-only views). */
size_t sweep_document_sheet_source_count(const SWEEP_DOCUMENT* doc);
bool sweep_document_sheet_source(const SWEEP_DOCUMENT* doc, size_t index,
                                 SWEEP_DOCUMENT_SHEET_SOURCE* out);
const SWEEP_CSS_STYLESHEET* sweep_document_author_sheet(const SWEEP_DOCUMENT* doc);
const SWEEP_CSS_DEP_INDEX* sweep_document_dep_index(const SWEEP_DOCUMENT* doc);

/* SuperUnicode transport: decode the bytes of one text node's payload into
 * SUCS codepoints under the document's text_mode via the mode-aware
 * CESU-8-aware decoder (sweep_js_cesu8_decode).  Per SUTR-0 §4.1 /
 * SUTR-1 §1.2, the range 0xD800-0xDFFF is 2,048 ordinary valid PUA
 * codepoints: NATIVE_SU decodes every value in that range as itself
 * (never paired, never rejected, exact round-trip).  WEB_COMPAT keeps
 * ECMAScript code-unit semantics at the web boundary (pairs combine; a
 * lone surrogate-range value from the web byte stream sinks to U+FFFD).
 * Trap slots/sentinel are rejected by sweep_sucs_is_valid upstream.
 * Returns false for a missing node or capacity overflow (with the
 * required count in *out_count). */
bool sweep_document_node_codepoints(const SWEEP_DOCUMENT* doc, uint32_t node_id,
                                    uint32_t* out, size_t cap,
                                    size_t* out_count);

/* True when the node's codepoints round-trip losslessly through the text
 * boundary (decode + re-encode equals the stored bytes) under text_mode. */
bool sweep_document_node_roundtrip(const SWEEP_DOCUMENT* doc, uint32_t node_id);

#endif /* SWEEP_DOCUMENT_H */
