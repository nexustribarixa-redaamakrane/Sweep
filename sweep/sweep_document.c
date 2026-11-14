/* sweep_document.c - document lifecycle coordinator (Phase 4/5). */
#include "sweep_document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sweep_js_text.h"
#include "sweep_resource.h"

typedef struct _SWEEP_SHEET_SLOT {
    SWEEP_DOCUMENT_SHEET_SOURCE meta;
    SWEEP_CSS_STYLESHEET parsed;
    bool in_use;
    /* Phase 6: change tracking for incremental source rebuilding. */
    char inline_text[SWEEP_DOCUMENT_SHEET_MAX + 1u];
    uint32_t content_hash;
    bool parsed_valid;
} SWEEP_SHEET_SLOT;

struct SWEEP_DOCUMENT {
    SWEEP_DOM dom;
    SWEEP_DOCUMENT_CONFIG config;
    SWEEP_VIEWPORT viewport;
    SWEEP_DOCUMENT_STATE stored_state;
    uint32_t processed_revision;
    uint32_t style_revision;
    uint32_t paint_revision; /* increments when paint commands are regenerated */
    uint32_t render_revision; /* last paint revision rendered by attached renderer */
    bool render_current;     /* attached renderer output matches paint_revision */
    bool sheet_override;   /* host set_stylesheet active for next style pass */
    bool viewport_changed;
    bool has_pipeline;

    /* Host override buffer (one-shot until next extraction, preserves API). */
    char override_text[SWEEP_DOCUMENT_SHEET_MAX + 1u];

    /* Collected author stylesheet sources in document/cascade order. */
    SWEEP_SHEET_SLOT sources[SWEEP_DOCUMENT_MAX_SHEET_SOURCES];
    size_t source_count;
    uint32_t next_sheet_id;
    bool sources_dirty;      /* rediscovery/rebuild required */
    bool author_sheet_dirty; /* combined/index rebuild required */

    /* Phase 6: last affected-element restyle analysis (for diagnostics). */
    SWEEP_RESTYLE_SET last_restyle;

    /* Combined cascade input and dependency index built from it. */
    SWEEP_CSS_STYLESHEET author_sheet;
    SWEEP_CSS_DEP_INDEX dep_index;

    SWEEP_COMPUTED_STYLE styles[SWEEP_CSS_MAX_STYLE_NODES];
    SWEEP_LAYOUT layout;
    SWEEP_PAINT_CMD paint[SWEEP_PAINT_MAX_CMDS];
    size_t paint_count;

    /* Phase 8: external, caller-owned renderer/presentation integration. */
    SWEEP_RENDERER* renderer;
    SWEEP_PRESENT_TARGET* present_target;

    SWEEP_JS_RUNTIME* js_runtime;
    SWEEP_JS_CONTEXT* js_context;

    SWEEP_DOCUMENT_STATS stats;
    uint32_t style_recompute_skipped;
    char last_error[SWEEP_DOCUMENT_ERROR_MAX];
};

static void doc_error(SWEEP_DOCUMENT* doc, const char* message) {
    if (!doc || !message) return;
    snprintf(doc->last_error, sizeof(doc->last_error), "%s", message);
}

static uint32_t fnv1a32(const char* s) {
    uint32_t h = 2166136261u;
    if (!s) return h;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 16777619u;
    }
    return h;
}

static void sheet_meta_init(SWEEP_DOCUMENT_SHEET_SOURCE* m, uint32_t id,
                            SWEEP_DOCUMENT_SHEET_KIND kind, uint32_t order,
                            uint32_t element_id, const char* path) {
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->kind = (uint8_t)kind;
    m->status = (uint8_t)SWEEP_SHEET_PENDING;
    m->enabled = true;
    m->source_order = order;
    m->element_id = element_id;
    m->revision = 1u;
    if (!path) path = "";
    snprintf(m->path, sizeof(m->path), "%s", path);
}

static void sheet_source_bump(SWEEP_DOCUMENT_SHEET_SOURCE* m) {
    if (!m) return;
    m->revision++;
}

static bool rel_is_stylesheet(const char* rel) {
    /* rel is a space-separated token list; we require a "stylesheet" token
     * (case-insensitive).  This is conservative and correct for the subset. */
    const char* p = rel;
    if (!p) return false;
    while (*p) {
        const char* start;
        size_t n;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') ++p;
        if (*p == '\0') break;
        start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f') ++p;
        n = (size_t)(p - start);
        if (n == 10u &&
            (start[0] == 's' || start[0] == 'S') &&
            (start[1] == 't' || start[1] == 'T') &&
            (start[2] == 'y' || start[2] == 'Y') &&
            (start[3] == 'l' || start[3] == 'L') &&
            (start[4] == 'e' || start[4] == 'E') &&
            (start[5] == 's' || start[5] == 'S') &&
            (start[6] == 'h' || start[6] == 'H') &&
            (start[7] == 'e' || start[7] == 'E') &&
            (start[8] == 'e' || start[8] == 'E') &&
            (start[9] == 't' || start[9] == 'T')) {
            return true;
        }
    }
    return false;
}

static bool href_seen(const SWEEP_SHEET_SLOT* slots, size_t count, const char* href) {
    size_t i;
    if (!href) return false;
    for (i = 0u; i < count; ++i) {
        if (!slots[i].in_use) continue;
        if (slots[i].meta.kind == (uint8_t)SWEEP_SHEET_LINK &&
            strcmp(slots[i].meta.path, href) == 0) {
            return true;
        }
    }
    return false;
}

static SWEEP_SHEET_SLOT* find_prev_inline(SWEEP_SHEET_SLOT* prev, size_t prev_count,
                                          uint32_t element_id) {
    size_t i;
    for (i = 0u; i < prev_count; ++i) {
        if (prev[i].in_use && prev[i].meta.kind == (uint8_t)SWEEP_SHEET_INLINE &&
            prev[i].meta.element_id == element_id) {
            return &prev[i];
        }
    }
    return NULL;
}

static SWEEP_SHEET_SLOT* find_prev_link(SWEEP_SHEET_SLOT* prev, size_t prev_count,
                                        const char* path) {
    size_t i;
    if (!path) return NULL;
    for (i = 0u; i < prev_count; ++i) {
        if (prev[i].in_use && prev[i].meta.kind == (uint8_t)SWEEP_SHEET_LINK &&
            strcmp(prev[i].meta.path, path) == 0) {
            return &prev[i];
        }
    }
    return NULL;
}

static void collect_style_sources(SWEEP_DOCUMENT* doc) {
    /* Discover inline <style> and <link rel="stylesheet"> in DOM order.
     * Phase 6: persistent per-source tracking. Inline/link sources are
     * matched to previous slots to preserve stable identity, detect changes,
     * and avoid reparsing unchanged sources when safe (reusing parsed rules).
     * External CSS is fetched through the configured resource provider;
     * fetched bytes are content-hashed so unchanged content is not reparsed. */
    SWEEP_SHEET_SLOT prev[SWEEP_DOCUMENT_MAX_SHEET_SOURCES];
    size_t prev_count = doc->source_count;
    SWEEP_SHEET_SLOT* slots = doc->sources;
    uint32_t stack[SWEEP_DOM_NODES_MAX];
    uint32_t sp = 0u;
    size_t order = 0u;
    size_t i;

    memcpy(prev, slots, sizeof(prev));
    memset(slots, 0, sizeof(doc->sources));
    doc->source_count = 0u;
    doc->stats.stylesheet_sources_reparsed = 0u;
    doc->stats.stylesheet_sources_reused = 0u;

    if (doc->dom.document == 0u || doc->dom.document > SWEEP_DOM_NODES_MAX) return;
    stack[sp++] = doc->dom.document;
    while (sp > 0u) {
        uint32_t id = stack[--sp];
        const SWEEP_DOM_NODE* n = sweep_dom_get(&doc->dom, id);
        if (!n) continue;

        if (n->type == SWEEP_DOM_ELEMENT && strcmp(n->name, "link") == 0) {
            const char* rel = sweep_dom_get_attr(n, "rel");
            const char* href = sweep_dom_get_attr(n, "href");
            if (rel_is_stylesheet(rel)) {
                if (doc->source_count < SWEEP_DOCUMENT_MAX_SHEET_SOURCES) {
                    SWEEP_SHEET_SLOT* slot = &slots[doc->source_count];
                    char path[SWEEP_DOCUMENT_SHEET_PATH_MAX];
                    memset(slot, 0, sizeof(*slot));
                    slot->in_use = true;
                    if (href && href[0] != '\0') snprintf(path, sizeof(path), "%s", href);
                    else snprintf(path, sizeof(path), "%s", "");

                    {
                        SWEEP_SHEET_SLOT* old = find_prev_link(prev, prev_count, path);
                        uint32_t sid = old ? old->meta.id : ++doc->next_sheet_id;
                        uint32_t rev = old ? old->meta.revision : 1u;
                        sheet_meta_init(&slot->meta, sid, SWEEP_SHEET_LINK, (uint32_t)order, id, path);
                        slot->meta.revision = rev;
                    }
                    slot->meta.status = (uint8_t)SWEEP_SHEET_PENDING;

                    if (!href || href[0] == '\0') {
                        slot->meta.status = (uint8_t)SWEEP_SHEET_ERROR;
                        snprintf(slot->meta.error, sizeof(slot->meta.error), "missing href");
                        slot->parsed_valid = false;
                        doc->stats.stylesheet_sources_reparsed++;
                    } else if (href_seen(slots, doc->source_count, path)) {
                        slot->meta.status = (uint8_t)SWEEP_SHEET_ERROR;
                        slot->meta.duplicate = true;
                        snprintf(slot->meta.error, sizeof(slot->meta.error), "duplicate");
                        slot->parsed_valid = false;
                        doc->stats.stylesheet_sources_reparsed++;
                    } else if (!doc->config.js_fetch) {
                        slot->meta.status = (uint8_t)SWEEP_SHEET_ERROR;
                        snprintf(slot->meta.error, sizeof(slot->meta.error), "no provider");
                        slot->parsed_valid = false;
                        doc->stats.stylesheet_sources_reparsed++;
                    } else {
                        char buf[SWEEP_RESOURCE_MAX_BYTES];
                        char mime[64];
                        size_t blen = 0u;
                        bool ok = doc->config.js_fetch(doc->config.js_fetch_user_data,
                                                       path, buf, sizeof(buf),
                                                       &blen, mime, sizeof(mime));
                        if (!ok) {
                            slot->meta.status = (uint8_t)SWEEP_SHEET_ERROR;
                            snprintf(slot->meta.error, sizeof(slot->meta.error), "fetch failed");
                            slot->parsed_valid = false;
                            doc->stats.stylesheet_sources_reparsed++;
                        } else if (strcmp(mime, "text/css") != 0) {
                            slot->meta.status = (uint8_t)SWEEP_SHEET_ERROR;
                            snprintf(slot->meta.error, sizeof(slot->meta.error), "bad mime");
                            slot->parsed_valid = false;
                            doc->stats.stylesheet_sources_reparsed++;
                        } else {
                            /* Hash content (bounded buffer) to detect changes. */
                            uint32_t h = 2166136261u;
                            size_t bi;
                            for (bi = 0u; bi < blen; ++bi) { h ^= (uint8_t)buf[bi]; h *= 16777619u; }
                            slot->content_hash = h;
                            {
                                SWEEP_SHEET_SLOT* old = find_prev_link(prev, prev_count, path);
                                if (old && old->parsed_valid &&
                                    old->meta.status == (uint8_t)SWEEP_SHEET_LOADED &&
                                    old->content_hash == h) {
                                    /* Reuse parsed contribution (no reparsing). */
                                    slot->parsed = old->parsed;
                                    slot->parsed_valid = true;
                                    slot->meta.rule_count = old->meta.rule_count;
                                    slot->meta.status = (uint8_t)SWEEP_SHEET_LOADED;
                                    slot->meta.revision = old->meta.revision;
                                    doc->stats.stylesheet_sources_reused++;
                                } else {
                                    bool parsed_ok = sweep_css_parse_stylesheet(buf, blen, &slot->parsed);
                                    slot->meta.rule_count = slot->parsed.rule_count;
                                    slot->parsed_valid = parsed_ok;
                                    if (!parsed_ok) {
                                        slot->meta.status = (uint8_t)SWEEP_SHEET_ERROR;
                                        snprintf(slot->meta.error, sizeof(slot->meta.error), "parse failed");
                                    } else {
                                        slot->meta.status = (uint8_t)SWEEP_SHEET_LOADED;
                                        slot->meta.error[0] = '\0';
                                    }
                                    if (old) sheet_source_bump(&slot->meta);
                                    doc->stats.stylesheet_sources_reparsed++;
                                }
                            }
                        }
                    }
                    doc->source_count++;
                    order++;
                } else {
                    order++;
                }
            }
        } else if (n->type == SWEEP_DOM_ELEMENT && strcmp(n->name, "style") == 0) {
            if (doc->source_count < SWEEP_DOCUMENT_MAX_SHEET_SOURCES) {
                SWEEP_SHEET_SLOT* slot = &slots[doc->source_count];
                memset(slot, 0, sizeof(*slot));
                slot->in_use = true;
                {
                    SWEEP_SHEET_SLOT* old = find_prev_inline(prev, prev_count, id);
                    uint32_t sid = old ? old->meta.id : ++doc->next_sheet_id;
                    uint32_t rev = old ? old->meta.revision : 1u;
                    sheet_meta_init(&slot->meta, sid, SWEEP_SHEET_INLINE, (uint32_t)order, id, "<style>");
                    slot->meta.revision = rev;
                }
                sweep_dom_text_content(&doc->dom, id, slot->inline_text, sizeof(slot->inline_text));
                slot->content_hash = fnv1a32(slot->inline_text);
                {
                    SWEEP_SHEET_SLOT* old = find_prev_inline(prev, prev_count, id);
                    if (old && old->parsed_valid &&
                        strcmp(old->inline_text, slot->inline_text) == 0) {
                        slot->parsed = old->parsed;
                        slot->parsed_valid = true;
                        slot->meta.rule_count = old->meta.rule_count;
                        slot->meta.status = (uint8_t)SWEEP_SHEET_LOADED;
                        slot->meta.revision = old->meta.revision;
                        doc->stats.stylesheet_sources_reused++;
                    } else {
                        (void) sweep_css_parse_stylesheet(slot->inline_text,
                                                          strlen(slot->inline_text),
                                                          &slot->parsed);
                        slot->meta.rule_count = slot->parsed.rule_count;
                        slot->meta.status = (uint8_t)SWEEP_SHEET_LOADED;
                        slot->parsed_valid = true;
                        if (old) sheet_source_bump(&slot->meta);
                        doc->stats.stylesheet_sources_reparsed++;
                    }
                }
                doc->source_count++;
                order++;
            } else {
                order++;
            }
            continue;
        }

        /* Push children in reverse so first child is processed first. */
        for (i = n->child_count; i > 0u && sp < SWEEP_DOM_NODES_MAX; --i) {
            stack[sp++] = n->children[i - 1u];
        }
    }
}

static void build_author_sheet(SWEEP_DOCUMENT* doc) {
    size_t i;
    uint32_t links = 0u, loaded = 0u, errors = 0u;
    memset(&doc->author_sheet, 0, sizeof(doc->author_sheet));

    if (doc->sheet_override) {
        /* Host override is the sole author source for this style pass
         * (one-shot; cleared after build so DOM <style>/<link> resume). */
        (void) sweep_css_parse_stylesheet(doc->override_text,
                                          strlen(doc->override_text),
                                          &doc->author_sheet);
        doc->sheet_override = false;
        doc->stats.stylesheet_sources = 0u;
        doc->stats.stylesheet_links = 0u;
        doc->stats.stylesheet_loaded = 1u;
        doc->stats.stylesheet_errors = 0u;
        doc->stats.stylesheet_sources_reparsed = 1u;
        doc->stats.stylesheet_sources_reused = 0u;
    } else {
        collect_style_sources(doc);
        for (i = 0u; i < doc->source_count; ++i) {
            SWEEP_SHEET_SLOT* slot = &doc->sources[i];
            if (!slot->in_use) continue;
            if (slot->meta.kind == (uint8_t)SWEEP_SHEET_LINK) ++links;
            if (slot->meta.status == (uint8_t)SWEEP_SHEET_LOADED) {
                ++loaded;
                if (slot->meta.enabled && !slot->meta.duplicate) {
                    (void) sweep_css_stylesheet_append(&doc->author_sheet,
                                                       &slot->parsed);
                }
            } else if (slot->meta.status == (uint8_t)SWEEP_SHEET_ERROR) {
                ++errors;
            }
        }
        doc->stats.stylesheet_sources = (uint32_t)doc->source_count;
        doc->stats.stylesheet_links = links;
        doc->stats.stylesheet_loaded = loaded;
        doc->stats.stylesheet_errors = errors;
    }

    sweep_css_dep_index_build(&doc->author_sheet, &doc->dep_index);
    doc->stats.stylesheet_combined_rebuilds++;
    doc->stats.dependency_index_rebuilds++;
    doc->sources_dirty = false;
    doc->author_sheet_dirty = false;
}

static void restyle_set_clear(SWEEP_RESTYLE_SET* set) {
    if (!set) return;
    memset(set, 0, sizeof(*set));
}

static void restyle_set_add(SWEEP_RESTYLE_SET* set, uint32_t node_id,
                            uint32_t generation, uint8_t scope, uint8_t reason,
                            bool direct) {
    if (!set || node_id == 0u) return;
    if (set->count >= SWEEP_RESTYLE_SET_MAX) { set->overflow = true; return; }
    set->entries[set->count].node_id = node_id;
    set->entries[set->count].generation = generation;
    set->entries[set->count].scope = scope;
    set->entries[set->count].reason = reason;
    set->entries[set->count].direct = direct;
    set->count++;
}

static uint32_t dom_node_count(const SWEEP_DOM* dom) {
    return dom ? dom->count : 0u;
}

static void compute_restyle_set(SWEEP_DOCUMENT* doc, SWEEP_RESTYLE_SET* out) {
    /* Bounded, generation-safe affected-element restyle tracking.
     * Conservative: escalates to subtree/document when precise smaller scope
     * cannot be proven safe under supported selectors/combinators/inheritance. */
    restyle_set_clear(out);
    if (!doc) return;
    if (!doc->has_pipeline || doc->sheet_override ||
        doc->dom.mutation_kind == SWEEP_DOM_MUT_STYLE ||
        doc->dom.mutation_kind == SWEEP_DOM_MUT_STRUCTURE ||
        doc->dom.mutation_kind == SWEEP_DOM_MUT_NONE) {
        restyle_set_add(out, doc->dom.document,
                        sweep_dom_node_generation(&doc->dom, doc->dom.document),
                        (uint8_t)SWEEP_RESTYLE_SCOPE_DOCUMENT,
                        (uint8_t)((doc->dom.mutation_kind == SWEEP_DOM_MUT_STYLE ||
                                   doc->sheet_override)
                                  ? SWEEP_RESTYLE_REASON_STYLESHEET
                                  : (doc->dom.mutation_kind == SWEEP_DOM_MUT_STRUCTURE
                                     ? SWEEP_RESTYLE_REASON_STRUCTURE
                                     : SWEEP_RESTYLE_REASON_UNKNOWN)),
                        true);
        return;
    }
    if (doc->dom.mutation_kind == SWEEP_DOM_MUT_ATTR) {
        uint32_t node = doc->dom.dirty_node ? doc->dom.dirty_node : doc->dom.document;
        uint32_t gen = sweep_dom_node_generation(&doc->dom, node);
        const SWEEP_CSS_DEP_INDEX* idx = &doc->dep_index;
        bool structural = idx->has_structural || idx->has_universal ||
                          idx->has_descendant || idx->has_child;
        uint8_t reason = (uint8_t)SWEEP_RESTYLE_REASON_ATTR;
        if (doc->dom.mutation_class) reason = (uint8_t)SWEEP_RESTYLE_REASON_CLASS;
        else if (doc->dom.mutation_id_attr) reason = (uint8_t)SWEEP_RESTYLE_REASON_ID;
        else if (doc->dom.mutation_attr[0] != '\0' &&
                 strcmp(doc->dom.mutation_attr, "style") == 0) {
            reason = (uint8_t)SWEEP_RESTYLE_REASON_INLINE_STYLE;
        }
        if (!sweep_dom_get(&doc->dom, node)) {
            restyle_set_add(out, doc->dom.document,
                            sweep_dom_node_generation(&doc->dom, doc->dom.document),
                            (uint8_t)SWEEP_RESTYLE_SCOPE_DOCUMENT,
                            reason, true);
            out->overflow = true;
            return;
        }
        if (structural) {
            restyle_set_add(out, node, gen, (uint8_t)SWEEP_RESTYLE_SCOPE_DOCUMENT,
                            (uint8_t)SWEEP_RESTYLE_REASON_DESCENDANT_SELECTOR, true);
            doc->stats.restyle_subtree_escalations++;
            return;
        }
        /* No structural/universal dependencies: only this element's own
         * selector matching can change, but inherited properties may affect
         * descendants => restyle subtree rooted at element. */
        restyle_set_add(out, node, gen, (uint8_t)SWEEP_RESTYLE_SCOPE_SUBTREE,
                        reason, true);
        return;
    }
    restyle_set_add(out, doc->dom.document,
                    sweep_dom_node_generation(&doc->dom, doc->dom.document),
                    (uint8_t)SWEEP_RESTYLE_SCOPE_DOCUMENT,
                    (uint8_t)SWEEP_RESTYLE_REASON_UNKNOWN, true);
}

static bool do_style_pass(SWEEP_DOCUMENT* doc) {
    uint32_t styled = 0u;
    SWEEP_CSS_STATS cstats;
    SWEEP_RESTYLE_SET rs;
    bool do_full = true;
    memset(&cstats, 0, sizeof(cstats));
    doc->stats.restyle_candidates = 0u;
    doc->stats.restyle_applied = 0u;
    doc->stats.restyle_skipped = 0u;
    doc->stats.restyle_subtree_escalations = 0u;
    doc->stats.restyle_full_fallbacks = 0u;
    doc->stats.stylesheet_sources_reparsed = 0u;
    doc->stats.stylesheet_sources_reused = 0u;
    restyle_set_clear(&doc->last_restyle);

    build_author_sheet(doc);
    compute_restyle_set(doc, &rs);
    doc->last_restyle = rs;

    /* Incremental style application (Phase 6): recompute only the affected
     * subtree when scope is provably safe and the target is generation-valid.
     * All other cases escalate to full-document recompute for correctness. */
    if (!rs.overflow && rs.count == 1u) {
        const SWEEP_RESTYLE_ENTRY* e = &rs.entries[0];
        uint32_t nid = e->node_id;
        const SWEEP_DOM_NODE* n = sweep_dom_get(&doc->dom, nid);
        if (n && sweep_dom_node_generation(&doc->dom, nid) == e->generation) {
            if (e->scope == (uint8_t)SWEEP_RESTYLE_SCOPE_SUBTREE) {
                const SWEEP_COMPUTED_STYLE* parent_style = NULL;
                if (n->parent != 0u && n->parent <= SWEEP_CSS_MAX_STYLE_NODES) {
                    parent_style = &doc->styles[n->parent - 1u];
                }
                styled = sweep_css_recompute_subtree_indexed(&doc->dom, &doc->author_sheet,
                                                             &doc->dep_index, nid, parent_style,
                                                             doc->styles, SWEEP_CSS_MAX_STYLE_NODES,
                                                             &cstats);
                do_full = false;
                doc->stats.restyle_candidates = styled;
                doc->stats.restyle_applied = styled;
                if (dom_node_count(&doc->dom) > styled) {
                    doc->stats.restyle_skipped = dom_node_count(&doc->dom) - styled;
                }
            } else if (e->scope == (uint8_t)SWEEP_RESTYLE_SCOPE_ELEMENT) {
                /* Element-only (no descendant propagation proven unnecessary here)
                 * would require per-property inheritance diff; conservatively
                 * handled as subtree in analysis, so this path is unused now. */
            }
        } else {
            /* Stale/missing target: conservative full fallback. */
            rs.overflow = true;
        }
    }

    if (do_full) {
        styled = sweep_css_compute_all_indexed(&doc->dom, &doc->author_sheet,
                                               &doc->dep_index, doc->styles,
                                               SWEEP_CSS_MAX_STYLE_NODES, &cstats);
        doc->stats.restyle_candidates = styled;
        doc->stats.restyle_applied = styled;
        doc->stats.restyle_full_fallbacks = 1u;
        if (rs.count > 0u && rs.entries[0].scope == (uint8_t)SWEEP_RESTYLE_SCOPE_SUBTREE) {
            /* Should not happen, but be explicit. */
        }
        /* Normalize reported restyle root for full fallback. */
        doc->last_restyle.count = 1u;
        doc->last_restyle.overflow = rs.overflow;
        doc->last_restyle.entries[0].node_id = doc->dom.document;
        doc->last_restyle.entries[0].generation = sweep_dom_node_generation(&doc->dom, doc->dom.document);
        doc->last_restyle.entries[0].scope = (uint8_t)SWEEP_RESTYLE_SCOPE_DOCUMENT;
        doc->last_restyle.entries[0].reason = rs.count > 0u ? rs.entries[0].reason
                                                            : (uint8_t)SWEEP_RESTYLE_REASON_UNKNOWN;
        doc->last_restyle.entries[0].direct = true;
    }

    doc->stats.style_passes++;
    doc->stats.style_nodes = styled;
    doc->stats.selector_tests = cstats.selector_tests;
    doc->stats.rules_skipped = cstats.rules_skipped;
    doc->style_revision++;
    return true;
}

static bool do_layout_pass(SWEEP_DOCUMENT* doc) {
    bool ok = sweep_layout_build(&doc->dom, doc->styles,
                                 SWEEP_CSS_MAX_STYLE_NODES, &doc->viewport,
                                 doc->config.text_mode, &doc->layout);
    doc->stats.layout_passes++;
    doc->stats.layout_truncated = doc->layout.truncated;
    doc->stats.layout_regions_considered = 1u;
    doc->stats.layout_full_fallbacks = 1u;
    doc->viewport_changed = false;
    return ok;
}

static bool do_paint_pass(SWEEP_DOCUMENT* doc) {
    size_t emitted = 0u;
    bool completed;
    memset(doc->paint, 0, sizeof(doc->paint));
    doc->paint_count = 0u;
    completed = sweep_paint_from_layout(&doc->layout, doc->config.text_mode,
                                        doc->paint, SWEEP_PAINT_MAX_CMDS,
                                        &emitted);
    doc->paint_count = emitted;
    doc->stats.paint_passes++;
    doc->stats.paint_truncated = !completed;
    /* New paint commands invalidate any previously rendered frame. */
    doc->paint_revision++;
    doc->render_current = false;
    if (doc->renderer) sweep_render_mark_stale(doc->renderer);
    return true;
}

static bool pipeline_fail(SWEEP_DOCUMENT* doc, const char* message) {
    doc_error(doc, message);
    doc->stored_state = SWEEP_DOC_FAILED;
    return false;
}

static bool mutation_can_skip_style(SWEEP_DOCUMENT* doc) {
    /* Conservative, dependency-index-aware decision: if the DOM reports a
     * style-relevant dirty flag but the mutation details prove no author rule
     * can be affected, we may skip the style pass (layout/paint still run).
     * If details are missing or inconclusive, we must recompute. */
    if (!doc) return false;
    if (doc->sheet_override) return false;
    if (!doc->has_pipeline) return false;
    switch (doc->dom.mutation_kind) {
    case SWEEP_DOM_MUT_NONE:
        return false;
    case SWEEP_DOM_MUT_TEXT:
        return true;
    case SWEEP_DOM_MUT_STYLE:
        return false;
    case SWEEP_DOM_MUT_ATTR: {
        SWEEP_CSS_MUTATION_KIND mk = SWEEP_CSS_MUT_ATTR;
        bool class_changed = doc->dom.mutation_class;
        bool id_changed = doc->dom.mutation_id_attr;
        return !sweep_css_dep_mutation_affects_style(&doc->dep_index, mk,
                                                     doc->dom.mutation_attr,
                                                     class_changed, id_changed);
    }
    case SWEEP_DOM_MUT_STRUCTURE:
        return false;
    default:
        return false;
    }
}

/* restyle helpers defined below after forward decls (moved) */

static bool run_pipeline(SWEEP_DOCUMENT* doc) {
    bool need_style = !doc->has_pipeline || doc->sheet_override ||
                      doc->dom.style_dirty;
    bool need_layout = need_style || doc->dom.layout_dirty ||
                       doc->viewport_changed;
    bool need_paint = need_layout || doc->dom.paint_dirty || !doc->has_pipeline;

    /* Incremental invalidation: skip a provably-unnecessary style pass. */
    if (need_style && doc->has_pipeline && !doc->sheet_override &&
        doc->dom.style_dirty && !doc->dom.layout_dirty && !doc->viewport_changed) {
        /* style_dirty alone would also imply layout_dirty in our marking, so
         * this case is rare; handled below. */
    }
    if (need_style && doc->has_pipeline && !doc->sheet_override &&
        doc->dom.style_dirty) {
        if (mutation_can_skip_style(doc)) {
            need_style = false;
            need_layout = true;  /* still must reflow/repaint conservatively */
            need_paint = true;
            doc->stats.style_recompute_skipped++;
            doc->style_recompute_skipped++;
        }
    }

    doc->stored_state = SWEEP_DOC_STYLE_DIRTY;
    if (need_style) {
        (void) do_style_pass(doc);
    } else if (need_layout || need_paint) {
        /* Keep stylesheet stats stable when style pass is skipped. */
    }
    doc->stored_state = SWEEP_DOC_LAYOUT_DIRTY;
    if (need_layout) {
        if (!do_layout_pass(doc)) return pipeline_fail(doc, "layout failed");
    }
    doc->stored_state = SWEEP_DOC_PAINT_DIRTY;
    if (need_paint) {
        (void) do_paint_pass(doc);
    }
    sweep_dom_clear_dirty(&doc->dom);
    doc->processed_revision = doc->dom.revision;
    doc->has_pipeline = true;
    doc->stored_state = SWEEP_DOC_READY;
    return true;
}

SWEEP_DOCUMENT* sweep_document_create(const SWEEP_DOCUMENT_CONFIG* config) {
    SWEEP_DOCUMENT* doc;
    SWEEP_VIEWPORT want;
    doc = (SWEEP_DOCUMENT*) calloc(1u, sizeof(*doc));
    if (!doc) return NULL;
    if (config) doc->config = *config;
    sweep_dom_init(&doc->dom);
    doc->stored_state = SWEEP_DOC_EMPTY;
    doc->next_sheet_id = 0u;
    doc->sources_dirty = true;
    doc->author_sheet_dirty = true;
    want = doc->config.viewport;
    if (want.width_px == 0u && want.height_px == 0u) {
        want.width_px = 800u; want.height_px = 600u; want.device_scale = 1u;
    }
    if (!SweepViewportNegotiate(&want, &doc->viewport)) {
        doc->viewport.width_px = 800u; doc->viewport.height_px = 600u;
        doc->viewport.device_scale = 1u;
    }
    return doc;
}

void sweep_document_destroy(SWEEP_DOCUMENT* doc) {
    if (!doc) return;
    /* Detach external renderer/presentation (caller-owned; never freed here). */
    if (doc->renderer) sweep_render_mark_stale(doc->renderer);
    doc->renderer = NULL;
    doc->present_target = NULL;
    doc->render_current = false;
    doc->stored_state = SWEEP_DOC_DESTROYED;
    if (doc->js_context) sweep_js_context_destroy(doc->js_context);
    if (doc->js_runtime) sweep_js_runtime_shutdown(doc->js_runtime);
    doc->js_context = NULL;
    doc->js_runtime = NULL;
    /* Parsed stylesheets and buffers are value members (no dangling provider
     * pointers, no heap ownership) so freeing the document is sufficient. */
    memset(doc->sources, 0, sizeof(doc->sources));
    free(doc);
}

bool sweep_document_load(SWEEP_DOCUMENT* doc, const char* html, size_t len) {
    if (!doc || doc->stored_state != SWEEP_DOC_EMPTY) return false;
    if (!html && len > 0u) return pipeline_fail(doc, "invalid HTML input");
    doc->stored_state = SWEEP_DOC_PARSING;
    (void) sweep_dom_parse(&doc->dom, html ? html : "", len);
    if (doc->dom.document == 0u) return pipeline_fail(doc, "DOM parse failed");
    doc->stored_state = SWEEP_DOC_DOM_READY;

    if (doc->config.js_enabled) {
        SWEEP_JS_RUNTIME_CONFIG js_cfg;
        memset(&js_cfg, 0, sizeof(js_cfg));
        js_cfg.memory_limit = doc->config.js_memory_limit;
        js_cfg.timeout_ms = doc->config.js_timeout_ms;
        js_cfg.text_mode = doc->config.text_mode;
        js_cfg.fetch = doc->config.js_fetch;
        js_cfg.fetch_user_data = doc->config.js_fetch_user_data;
        if (!sweep_js_runtime_init(&doc->js_runtime, &js_cfg)) {
            return pipeline_fail(doc, "JavaScript runtime init failed");
        }
        doc->js_context = sweep_js_context_create(doc->js_runtime, &doc->dom);
        if (!doc->js_context) {
            return pipeline_fail(doc, "JavaScript context create failed");
        }
        (void) sweep_js_run_document_scripts(doc->js_context, &doc->stats.scripts);
        {
            const char* e = sweep_js_context_last_error(doc->js_context);
            if (e && e[0] != '\0') {
                doc_error(doc, e);
            }
        }
    }
    return run_pipeline(doc);
}

bool sweep_document_set_stylesheet(SWEEP_DOCUMENT* doc, const char* css,
                                   size_t len) {
    size_t n;
    if (!doc) return false;
    if (doc->stored_state == SWEEP_DOC_EMPTY ||
        doc->stored_state == SWEEP_DOC_PARSING ||
        doc->stored_state == SWEEP_DOC_FAILED ||
        doc->stored_state == SWEEP_DOC_DESTROYED) return false;
    if (!css && len > 0u) return false;
    if (len > SWEEP_DOCUMENT_SHEET_MAX) len = SWEEP_DOCUMENT_SHEET_MAX;
    n = len;
    if (n > 0u) memcpy(doc->override_text, css, n);
    doc->override_text[n] = '\0';
    doc->sheet_override = true;
    if (doc->stored_state == SWEEP_DOC_READY) {
        doc->stored_state = SWEEP_DOC_STYLE_DIRTY;
    }
    /* Ensure the style pass runs even if the DOM revision hasn't changed. */
    sweep_dom_mark_style_dirty(&doc->dom);
    return true;
}

bool sweep_document_set_viewport(SWEEP_DOCUMENT* doc, SWEEP_VIEWPORT viewport) {
    if (!doc) return false;
    if (doc->stored_state == SWEEP_DOC_EMPTY ||
        doc->stored_state == SWEEP_DOC_PARSING ||
        doc->stored_state == SWEEP_DOC_FAILED ||
        doc->stored_state == SWEEP_DOC_DESTROYED) return false;
    (void) viewport;
    {
        SWEEP_VIEWPORT negotiated;
        if (!SweepViewportNegotiate(&viewport, &negotiated)) {
            doc_error(doc, "invalid viewport");
            return false;
        }
        doc->viewport = negotiated;
    }
    doc->viewport_changed = true;
    /* Viewport change requires layout/paint regeneration; rendered frame becomes stale. */
    doc->render_current = false;
    if (doc->renderer) sweep_render_mark_stale(doc->renderer);
    if (doc->stored_state == SWEEP_DOC_READY) {
        doc->stored_state = SWEEP_DOC_LAYOUT_DIRTY;
    }
    return true;
}

bool sweep_document_update(SWEEP_DOCUMENT* doc) {
    if (!doc) return false;
    switch (doc->stored_state) {
    case SWEEP_DOC_DOM_READY:
    case SWEEP_DOC_STYLE_DIRTY:
    case SWEEP_DOC_LAYOUT_DIRTY:
    case SWEEP_DOC_PAINT_DIRTY:
        break;
    case SWEEP_DOC_READY:
        if (doc->processed_revision == doc->dom.revision &&
            !doc->sheet_override && !doc->viewport_changed &&
            !sweep_dom_is_dirty(&doc->dom)) {
            return true;
        }
        break;
    default:
        return false;
    }
    return run_pipeline(doc);
}

bool sweep_document_evaluate(SWEEP_DOCUMENT* doc, const char* source,
                             SWEEP_JS_RESULT* out) {
    if (!doc || !doc->js_context || !source) return false;
    if (doc->stored_state == SWEEP_DOC_EMPTY ||
        doc->stored_state == SWEEP_DOC_PARSING ||
        doc->stored_state == SWEEP_DOC_FAILED ||
        doc->stored_state == SWEEP_DOC_DESTROYED) return false;
    {
        bool ok = sweep_js_evaluate_cstr(doc->js_context, source, out);
        if (!ok && out) doc_error(doc, out->error);
        return ok;
    }
}

bool sweep_document_evaluate_cstr(SWEEP_DOCUMENT* doc, const char* source,
                                  SWEEP_JS_RESULT* out) {
    if (!doc || !source) return false;
    return sweep_document_evaluate(doc, source, out);
}

bool sweep_document_run_scripts(SWEEP_DOCUMENT* doc, SWEEP_JS_SCRIPT_STATS* stats) {
    if (!doc || !doc->js_context) return false;
    if (doc->stored_state == SWEEP_DOC_EMPTY ||
        doc->stored_state == SWEEP_DOC_PARSING ||
        doc->stored_state == SWEEP_DOC_FAILED ||
        doc->stored_state == SWEEP_DOC_DESTROYED) return false;
    {
        bool ok = sweep_js_run_document_scripts(doc->js_context, stats);
        if (stats) doc->stats.scripts = *stats;
        return ok;
    }
}

SWEEP_DOCUMENT_STATE sweep_document_state(const SWEEP_DOCUMENT* doc) {
    if (!doc) return SWEEP_DOC_DESTROYED;
    if (doc->stored_state == SWEEP_DOC_READY &&
        (doc->processed_revision != doc->dom.revision ||
         doc->sheet_override || doc->viewport_changed)) {
        if (doc->sheet_override || doc->dom.style_dirty) return SWEEP_DOC_STYLE_DIRTY;
        if (doc->viewport_changed || doc->dom.layout_dirty) return SWEEP_DOC_LAYOUT_DIRTY;
        if (doc->dom.paint_dirty) return SWEEP_DOC_PAINT_DIRTY;
    }
    return doc->stored_state;
}

const char* sweep_document_state_name(SWEEP_DOCUMENT_STATE state) {
    switch (state) {
    case SWEEP_DOC_EMPTY: return "EMPTY";
    case SWEEP_DOC_PARSING: return "PARSING";
    case SWEEP_DOC_DOM_READY: return "DOM_READY";
    case SWEEP_DOC_STYLE_DIRTY: return "STYLE_DIRTY";
    case SWEEP_DOC_LAYOUT_DIRTY: return "LAYOUT_DIRTY";
    case SWEEP_DOC_PAINT_DIRTY: return "PAINT_DIRTY";
    case SWEEP_DOC_READY: return "READY";
    case SWEEP_DOC_FAILED: return "FAILED";
    case SWEEP_DOC_DESTROYED: return "DESTROYED";
    }
    return "UNKNOWN";
}

SWEEP_DOM* sweep_document_dom(SWEEP_DOCUMENT* doc) {
    return doc ? &doc->dom : (SWEEP_DOM*)NULL;
}
const SWEEP_DOM* sweep_document_dom_const(const SWEEP_DOCUMENT* doc) {
    return doc ? &doc->dom : (const SWEEP_DOM*)NULL;
}
SWEEP_JS_CONTEXT* sweep_document_js_context(SWEEP_DOCUMENT* doc) {
    return doc ? doc->js_context : (SWEEP_JS_CONTEXT*)NULL;
}
SWEEP_TEXT_MODE sweep_document_text_mode(const SWEEP_DOCUMENT* doc) {
    return doc ? doc->config.text_mode : SWEEP_TEXT_WEB_COMPAT;
}
uint32_t sweep_document_revision(const SWEEP_DOCUMENT* doc) {
    return doc ? doc->dom.revision : 0u;
}
uint32_t sweep_document_processed_revision(const SWEEP_DOCUMENT* doc) {
    return doc ? doc->processed_revision : 0u;
}
uint32_t sweep_document_style_revision(const SWEEP_DOCUMENT* doc) {
    return doc ? doc->style_revision : 0u;
}
uint32_t sweep_document_paint_revision(const SWEEP_DOCUMENT* doc) {
    return doc ? doc->paint_revision : 0u;
}
SWEEP_VIEWPORT sweep_document_viewport(const SWEEP_DOCUMENT* doc) {
    static const SWEEP_VIEWPORT none = {0u, 0u, 0u};
    return doc ? doc->viewport : none;
}
const SWEEP_LAYOUT* sweep_document_layout(const SWEEP_DOCUMENT* doc) {
    return doc ? &doc->layout : (const SWEEP_LAYOUT*)NULL;
}
const SWEEP_COMPUTED_STYLE* sweep_document_style_of(const SWEEP_DOCUMENT* doc,
                                                     uint32_t node_id) {
    if (!doc || !node_id || node_id > SWEEP_CSS_MAX_STYLE_NODES) return NULL;
    return &doc->styles[node_id - 1u];
}
const SWEEP_PAINT_CMD* sweep_document_paint(const SWEEP_DOCUMENT* doc,
                                            size_t* out_count) {
    if (out_count) *out_count = doc ? doc->paint_count : 0u;
    return doc ? doc->paint : (const SWEEP_PAINT_CMD*)NULL;
}
bool sweep_document_paint_truncated(const SWEEP_DOCUMENT* doc) {
    return doc && doc->stats.paint_truncated;
}
void sweep_document_stats(const SWEEP_DOCUMENT* doc, SWEEP_DOCUMENT_STATS* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (doc) {
        *out = doc->stats;
        out->style_recompute_skipped = doc->style_recompute_skipped;
    }
}
const char* sweep_document_last_error(const SWEEP_DOCUMENT* doc) {
    return (doc && doc->last_error[0] != '\0') ? doc->last_error : "";
}

size_t sweep_document_sheet_source_count(const SWEEP_DOCUMENT* doc) {
    return doc ? doc->source_count : 0u;
}
bool sweep_document_sheet_source(const SWEEP_DOCUMENT* doc, size_t index,
                                 SWEEP_DOCUMENT_SHEET_SOURCE* out) {
    if (!doc || !out || index >= doc->source_count) return false;
    *out = doc->sources[index].meta;
    return true;
}
const SWEEP_CSS_STYLESHEET* sweep_document_author_sheet(const SWEEP_DOCUMENT* doc) {
    return doc ? &doc->author_sheet : (const SWEEP_CSS_STYLESHEET*)NULL;
}
const SWEEP_CSS_DEP_INDEX* sweep_document_dep_index(const SWEEP_DOCUMENT* doc) {
    return doc ? &doc->dep_index : (const SWEEP_CSS_DEP_INDEX*)NULL;
}

bool sweep_document_attach_renderer(SWEEP_DOCUMENT* doc, SWEEP_RENDERER* renderer) {
    if (!doc || doc->stored_state == SWEEP_DOC_DESTROYED) return false;
    if (doc->renderer && doc->renderer != renderer) sweep_render_mark_stale(doc->renderer);
    doc->renderer = renderer;
    doc->render_current = false;
    if (doc->renderer) sweep_render_mark_stale(doc->renderer);
    return true;
}

void sweep_document_detach_renderer(SWEEP_DOCUMENT* doc) {
    if (!doc) return;
    if (doc->renderer) sweep_render_mark_stale(doc->renderer);
    doc->renderer = NULL;
    doc->render_current = false;
}

SWEEP_RENDERER* sweep_document_renderer(SWEEP_DOCUMENT* doc) {
    return doc ? doc->renderer : NULL;
}

bool sweep_document_attach_present_target(SWEEP_DOCUMENT* doc, SWEEP_PRESENT_TARGET* target) {
    if (!doc || doc->stored_state == SWEEP_DOC_DESTROYED) return false;
    doc->present_target = target;
    return true;
}

void sweep_document_detach_present_target(SWEEP_DOCUMENT* doc) {
    if (!doc) return;
    doc->present_target = NULL;
}

SWEEP_PRESENT_TARGET* sweep_document_present_target(SWEEP_DOCUMENT* doc) {
    return doc ? doc->present_target : NULL;
}

uint32_t sweep_document_render_revision(const SWEEP_DOCUMENT* doc) {
    return doc ? doc->render_revision : 0u;
}

bool sweep_document_render_current(const SWEEP_DOCUMENT* doc) {
    if (!doc) return false;
    if (!doc->render_current) return false;
    if (doc->render_revision != doc->paint_revision) return false;
    if (doc->renderer && !sweep_render_is_current(doc->renderer, doc->paint_revision)) return false;
    return true;
}

SWEEP_RENDER_STATUS sweep_document_render(SWEEP_DOCUMENT* doc) {
    const SWEEP_PAINT_CMD* cmds;
    size_t count = 0u;
    SWEEP_RENDER_STATUS st;
    if (!doc) return SWEEP_RENDER_INVALID;
    if (doc->stored_state == SWEEP_DOC_EMPTY || doc->stored_state == SWEEP_DOC_PARSING ||
        doc->stored_state == SWEEP_DOC_FAILED || doc->stored_state == SWEEP_DOC_DESTROYED) {
        return SWEEP_RENDER_INVALID;
    }
    if (!doc->renderer) return SWEEP_RENDER_NO_SURFACE;
    cmds = sweep_document_paint(doc, &count);
    st = sweep_render_commands_revision(doc->renderer, cmds, count,
                                        sweep_document_paint_truncated(doc),
                                        doc->paint_revision);
    if (st == SWEEP_RENDER_OK || st == SWEEP_RENDER_TRUNCATED) {
        doc->render_revision = doc->paint_revision;
        doc->render_current = true;
    } else {
        doc->render_current = false;
        if (doc->renderer) sweep_render_mark_stale(doc->renderer);
    }
    return st;
}

SWEEP_PRESENT_STATUS sweep_document_present(SWEEP_DOCUMENT* doc) {
    if (!doc) return SWEEP_PRESENT_INVALID;
    if (!doc->present_target) return SWEEP_PRESENT_NO_TARGET;
    if (!doc->renderer) return SWEEP_PRESENT_INVALID;
    if (!sweep_document_render_current(doc)) return SWEEP_PRESENT_INVALID; /* stale/not rendered */
    return sweep_present_frame(doc->present_target, &doc->renderer->surface, doc->paint_revision);
}

bool sweep_document_node_codepoints(const SWEEP_DOCUMENT* doc, uint32_t node_id,
                                    uint32_t* out, size_t cap,
                                    size_t* out_count) {
    const SWEEP_DOM_NODE* n;
    uint32_t cps[SWEEP_PAINT_TEXT_CP_MAX];
    size_t ncp = 0u;
    if (out_count) *out_count = 0u;
    if (!doc) return false;
    n = sweep_dom_get(&doc->dom, node_id);
    if (!n || n->type != SWEEP_DOM_TEXT) return false;
    (void) sweep_js_cesu8_decode(doc->config.text_mode,
                                 (const uint8_t*) n->text, strlen(n->text),
                                 cps, SWEEP_PAINT_TEXT_CP_MAX, &ncp);
    if (out_count) *out_count = ncp;
    if (!out || cap < ncp) return false;
    {
        size_t i;
        for (i = 0u; i < ncp; ++i) out[i] = cps[i];
    }
    return true;
}

bool sweep_document_node_roundtrip(const SWEEP_DOCUMENT* doc, uint32_t node_id) {
    const SWEEP_DOM_NODE* n;
    uint32_t cps[SWEEP_PAINT_TEXT_CP_MAX];
    uint8_t back[SWEEP_HTML_TEXT_MAX * 4u];
    size_t ncp = 0u, i, used = 0u;
    if (!doc) return false;
    n = sweep_dom_get(&doc->dom, node_id);
    if (!n || n->type != SWEEP_DOM_TEXT) return false;
    (void) sweep_js_cesu8_decode(doc->config.text_mode,
                                 (const uint8_t*) n->text, strlen(n->text),
                                 cps, SWEEP_PAINT_TEXT_CP_MAX, &ncp);
    for (i = 0u; i < ncp; ++i) {
        uint32_t cp = cps[i];
        if (cp < 0x80u) {
            if (used + 1u > sizeof(back)) return false;
            back[used++] = (uint8_t) cp;
        } else if (cp < 0x800u) {
            if (used + 2u > sizeof(back)) return false;
            back[used++] = (uint8_t) (0xC0u | (cp >> 6));
            back[used++] = (uint8_t) (0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (used + 3u > sizeof(back)) return false;
            back[used++] = (uint8_t) (0xE0u | (cp >> 12));
            back[used++] = (uint8_t) (0x80u | ((cp >> 6) & 0x3Fu));
            back[used++] = (uint8_t) (0x80u | (cp & 0x3Fu));
        } else {
            if (used + 4u > sizeof(back)) return false;
            back[used++] = (uint8_t) (0xF0u | (cp >> 18));
            back[used++] = (uint8_t) (0x80u | ((cp >> 12) & 0x3Fu));
            back[used++] = (uint8_t) (0x80u | ((cp >> 6) & 0x3Fu));
            back[used++] = (uint8_t) (0x80u | (cp & 0x3Fu));
        }
    }
    if (used != strlen(n->text)) return false;
    return memcmp(back, n->text, used) == 0;
}
