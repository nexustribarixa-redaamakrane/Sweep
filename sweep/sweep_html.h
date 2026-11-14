/* sweep_html.h - Sweep HTML tokenizer and basic DOM (foundational subset).
 * Real state-machine tokenizer (not a tag scanner) + bounded DOM tree builder.
 * Deterministic error recovery for malformed-but-recoverable markup.
 * Scope: common tags/attributes/text/comments/doctype; not a full HTML5 spec impl.
 */
#ifndef SWEEP_HTML_H
#define SWEEP_HTML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sweep_text.h"

#define SWEEP_HTML_NAME_MAX 48u
#define SWEEP_HTML_ATTR_MAX 16u
#define SWEEP_HTML_ATTR_VAL_MAX 256u
#define SWEEP_HTML_TEXT_MAX 1024u
#define SWEEP_DOM_NODES_MAX 128u
#define SWEEP_DOM_CHILDREN_MAX 32u
#define SWEEP_DOM_SERIALIZE_MAX 8192u

typedef enum _SWEEP_HTML_TOKEN_TYPE {
    SWEEP_TOK_EOF = 0,
    SWEEP_TOK_TEXT,
    SWEEP_TOK_START_TAG,
    SWEEP_TOK_END_TAG,
    SWEEP_TOK_COMMENT,
    SWEEP_TOK_DOCTYPE,
    SWEEP_TOK_ERROR /* recovered */
} SWEEP_HTML_TOKEN_TYPE;

typedef struct _SWEEP_HTML_ATTR {
    char name[SWEEP_HTML_NAME_MAX];
    char value[SWEEP_HTML_ATTR_VAL_MAX];
} SWEEP_HTML_ATTR;

typedef struct _SWEEP_HTML_TOKEN {
    SWEEP_HTML_TOKEN_TYPE type;
    char name[SWEEP_HTML_NAME_MAX];   /* tag/doctype name, lowercased for tags */
    char data[SWEEP_HTML_TEXT_MAX];   /* text/comment */
    SWEEP_HTML_ATTR attrs[SWEEP_HTML_ATTR_MAX];
    uint32_t attr_count;
    bool self_closing;
} SWEEP_HTML_TOKEN;

typedef enum _SWEEP_HTML_STATE {
    SWEEP_HTML_DATA = 0,
    SWEEP_HTML_TAG_OPEN,
    SWEEP_HTML_END_TAG_OPEN,
    SWEEP_HTML_TAG_NAME,
    SWEEP_HTML_BEFORE_ATTR_NAME,
    SWEEP_HTML_ATTR_NAME,
    SWEEP_HTML_AFTER_ATTR_NAME,
    SWEEP_HTML_BEFORE_ATTR_VALUE,
    SWEEP_HTML_ATTR_VALUE_DQ,
    SWEEP_HTML_ATTR_VALUE_SQ,
    SWEEP_HTML_ATTR_VALUE_UQ,
    SWEEP_HTML_AFTER_ATTR_VALUE,
    SWEEP_HTML_SELF_CLOSING,
    SWEEP_HTML_MARKUP_DECL,
    SWEEP_HTML_COMMENT,
    SWEEP_HTML_COMMENT_DASH,
    SWEEP_HTML_COMMENT_END,
    SWEEP_HTML_DOCTYPE,
    SWEEP_HTML_BOGUS_COMMENT,
    SWEEP_HTML_RAW_TEXT,
    SWEEP_HTML_RAW_CLOSE
} SWEEP_HTML_STATE;

typedef struct _SWEEP_HTML_TOKENIZER {
    const char* input;
    size_t len;
    size_t pos;
    SWEEP_HTML_STATE state;
    SWEEP_HTML_TOKEN cur;
    char text_buf[SWEEP_HTML_TEXT_MAX];
    size_t text_len;
    bool has_text;
    /* pending reconsume */
    bool reconsume;
    char re_char;
    /* raw-text element state (script/style): content is not tokenized as tags
     * until the matching end tag is seen. */
    char raw_tag[SWEEP_HTML_NAME_MAX];
    char raw_candidate[SWEEP_HTML_NAME_MAX + 4u];
    size_t raw_candidate_len;
    bool raw_close_done;
} SWEEP_HTML_TOKENIZER;

void sweep_html_tokenizer_init(SWEEP_HTML_TOKENIZER* tz, const char* html, size_t len);
/* Returns true if a token was produced (including EOF once). */
bool sweep_html_next_token(SWEEP_HTML_TOKENIZER* tz, SWEEP_HTML_TOKEN* out);

/* --- Basic DOM --- */
typedef enum _SWEEP_DOM_NODE_TYPE {
    SWEEP_DOM_DOCUMENT = 1,
    SWEEP_DOM_ELEMENT,
    SWEEP_DOM_TEXT,
    SWEEP_DOM_COMMENT
} SWEEP_DOM_NODE_TYPE;

typedef struct _SWEEP_DOM_NODE {
    bool in_use;
    SWEEP_DOM_NODE_TYPE type;
    uint32_t parent; /* 0 = none/root container */
    uint32_t children[SWEEP_DOM_CHILDREN_MAX];
    uint32_t child_count;
    char name[SWEEP_HTML_NAME_MAX];
    char text[SWEEP_HTML_TEXT_MAX];
    SWEEP_HTML_ATTR attrs[SWEEP_HTML_ATTR_MAX];
    uint32_t attr_count;
    /* Identity generation for this slot: starts at 1 and increments every
     * time the slot is reclaimed and reused.  JavaScript wrappers capture
     * (id, generation); a wrapper whose generation no longer matches the
     * slot is stale and must be rejected, never followed. */
    uint32_t generation;
} SWEEP_DOM_NODE;

typedef enum _SWEEP_DOM_MUTATION_KIND {
    SWEEP_DOM_MUT_NONE = 0,
    SWEEP_DOM_MUT_TEXT,      /* text-content/layout-relevant only */
    SWEEP_DOM_MUT_ATTR,      /* attribute set/remove */
    SWEEP_DOM_MUT_STRUCTURE, /* insert/remove/reorder/innerHTML replacement */
    SWEEP_DOM_MUT_STYLE      /* stylesheet-affecting content (e.g. <style> text) */
} SWEEP_DOM_MUTATION_KIND;

typedef struct _SWEEP_DOM {
    SWEEP_DOM_NODE nodes[SWEEP_DOM_NODES_MAX];
    uint32_t count;
    uint32_t document;    /* node id (index+1) */
    uint32_t revision;    /* bumped by every applied mutation */
    uint32_t dirty_node;  /* nearest affected node, 0 = whole document */
    bool style_dirty;
    bool layout_dirty;
    bool paint_dirty;
    /* Last mutation details (best-effort, for incremental invalidation). */
    uint8_t mutation_kind;   /* SWEEP_DOM_MUTATION_KIND */
    char mutation_attr[SWEEP_HTML_NAME_MAX]; /* attribute name, if any */
    bool mutation_class;     /* attribute mutation touched class */
    bool mutation_id_attr;   /* attribute mutation touched id */
} SWEEP_DOM;

void sweep_dom_init(SWEEP_DOM* dom);
uint32_t sweep_dom_parse(SWEEP_DOM* dom, const char* html, size_t len);
const SWEEP_DOM_NODE* sweep_dom_get(const SWEEP_DOM* dom, uint32_t id);
uint32_t sweep_dom_find_element(const SWEEP_DOM* dom, uint32_t root, const char* name);
uint32_t sweep_dom_find_by_id(const SWEEP_DOM* dom, uint32_t root, const char* id);
const char* sweep_dom_get_attr(const SWEEP_DOM_NODE* n, const char* name);
void sweep_dom_text_content(const SWEEP_DOM* dom, uint32_t id, char* out, size_t cap);

/* --- controlled mutation (bounded) ---
 * Creating a node consumes a fixed DOM slot.  Nodes removed via
 * sweep_dom_remove_child remain in_use and detached so that existing
 * JavaScript wrappers stay valid and the node can be re-appended.
 * Children discarded by sweep_dom_set_text_content/sweep_dom_set_inner_html
 * are instead reclaimed (slot freed, generation bumped) because those APIs
 * semantically replace the old subtree; stale JavaScript wrappers to
 * reclaimed nodes are rejected by generation check (see sweep_js_dom). */
uint32_t sweep_dom_create_element(SWEEP_DOM* dom, const char* name);
uint32_t sweep_dom_create_text(SWEEP_DOM* dom, const char* text);
uint32_t sweep_dom_create_comment(SWEEP_DOM* dom, const char* text);

/* Attach/detach nodes.  append/insert reject cycles, non-container parents
 * and full child lists.  insert_before(..., 0) appends. */
bool sweep_dom_append_child(SWEEP_DOM* dom, uint32_t parent, uint32_t child);
bool sweep_dom_insert_before(SWEEP_DOM* dom, uint32_t parent, uint32_t child,
                             uint32_t reference);
bool sweep_dom_remove_child(SWEEP_DOM* dom, uint32_t child);

bool sweep_dom_set_attr(SWEEP_DOM* dom, uint32_t id, const char* name,
                        const char* value);
bool sweep_dom_remove_attr(SWEEP_DOM* dom, uint32_t id, const char* name);

/* textContent setter: replace children with one text node (or none if empty).
 * For text/comment nodes the node's own payload is replaced. */
bool sweep_dom_set_text_content(SWEEP_DOM* dom, uint32_t id, const char* text);

/* innerHTML setter/getter.  Setter parses a bounded fragment using the same
 * tokenizer/DOM builder, then replaces the element's children.  Getter emits
 * a bounded serialization of children (not the element itself). */
bool sweep_dom_set_inner_html(SWEEP_DOM* dom, uint32_t id, const char* html,
                              size_t len);
bool sweep_dom_get_inner_html(const SWEEP_DOM* dom, uint32_t id, char* out,
                              size_t cap, size_t* out_len);

/* Invalidation bookkeeping.  Mutations mark dirty flags, bump revision, and
 * record best-effort mutation details (kind/attr/class/id) so the document
 * lifecycle can apply dependency-aware invalidation.  Text-content mutations
 * skip the style flag (text does not affect selector matching); structural
 * and attribute mutations remain conservative unless proven unaffected. */
void sweep_dom_clear_dirty(SWEEP_DOM* dom);
bool sweep_dom_is_dirty(const SWEEP_DOM* dom);

/* Mark a stylesheet-affecting change (e.g. external <link> collection change)
 * that requires a full style pass on next update. */
void sweep_dom_mark_style_dirty(SWEEP_DOM* dom);

/* Generation of the node currently occupying slot `id` (0 if empty). */
uint32_t sweep_dom_node_generation(const SWEEP_DOM* dom, uint32_t id);

#endif /* SWEEP_HTML_H */
