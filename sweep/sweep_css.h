/* sweep_css.h - Sweep CSS engine, computed styles, basic layout and paint.
 *
 * Host-testable foundation layered on the existing Sweep HTML/DOM
 * (sweep_html.h), engine/paint/viewport (sweep_engine.h) and text modes
 * (sweep_text.h).  Everything is bounded and uses no dynamic allocation.
 *
 * Scope (implemented, tested):
 *   - CSS tokenizer (identifiers, at-keywords, hashes, strings, numbers,
 *     dimensions, percentages, punctuation, comments, delimiters).
 *   - Declaration parser (normalized property names, !important, recovery).
 *   - Selectors: universal, type, class, id, descendant, child, multiple
 *     classes, attribute existence/equality, selector lists.
 *   - Cascade: specificity, source order, inline style, !important,
 *     inheritance for inherited properties, UA defaults, initial values.
 *   - Layout: block/inline/none display, basic block flow, line wrapping of
 *     inline text, width/height, margin/padding/border, viewport bounds.
 *   - Paint: bounded CLEAR/RECT/TEXT command generation from layout boxes.
 *
 * Not implemented (documented, not faked): full HTML5 tree construction,
 * CSS percentage-dependent layout edge cases, floats/flex/grid/positioning,
 * media queries, pseudo-classes/elements, font shaping/rasterization,
 * real HTTP/TLS/JS or a physical graphics backend.
 *
 * Units: SWEEP_CSS_LENGTH stores the numeric value in thousandths
 * (value_milli) plus an explicit unit.  300px is {300000, PX}.  Percentages
 * are stored as percent * 1000 (50% = {50000, PERCENT}).  Layout resolves to
 * integer pixels through sweep_css_length_resolve_px(); unsupported units
 * are never silently treated as pixels.
 *
 * Colors: 0xAARRGGBB (alpha high byte).  SWEEP_CSS_COLOR_TRANSPARENT = 0.
 */
#ifndef SWEEP_CSS_H
#define SWEEP_CSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sweep_engine.h"
#include "sweep_html.h"
#include "sweep_text.h"

/* ---- resource limits (chosen for constrained/associated-kernel use) ---- */
#define SWEEP_CSS_MAX_TOKENS              512u
#define SWEEP_CSS_TOKEN_TEXT_MAX           96u
#define SWEEP_CSS_MAX_RULES                16u
#define SWEEP_CSS_MAX_SELECTORS             4u /* selectors per rule (selector list) */
#define SWEEP_CSS_SELECTOR_PARTS_MAX        3u /* compounds per complex selector */
#define SWEEP_CSS_COMPOUND_SIMPLES_MAX      4u /* simple selectors per compound */
#define SWEEP_CSS_MAX_DECLS                12u
#define SWEEP_CSS_PROP_MAX                 32u
#define SWEEP_CSS_DECL_VALUE_MAX           64u
#define SWEEP_CSS_NAME_MAX                 48u
#define SWEEP_CSS_ATTR_VALUE_MAX           64u
#define SWEEP_CSS_FONT_FAMILY_MAX          48u
#define SWEEP_CSS_MAX_STYLE_NODES  SWEEP_DOM_NODES_MAX
#define SWEEP_LAYOUT_MAX_BOXES     SWEEP_DOM_NODES_MAX
#define SWEEP_LAYOUT_MAX_DEPTH            32u
#define SWEEP_LAYOUT_MAX_PX          1000000
#define SWEEP_PAINT_TEXT_CP_MAX          256u

#define SWEEP_CSS_COLOR_BLACK        0xFF000000u
#define SWEEP_CSS_COLOR_TRANSPARENT  0x00000000u

/* ---- tokenizer ---- */
typedef enum _SWEEP_CSS_TOKEN_TYPE {
    SWEEP_CSS_TOK_EOF = 0,
    SWEEP_CSS_TOK_IDENT,
    SWEEP_CSS_TOK_AT_KEYWORD,
    SWEEP_CSS_TOK_HASH,
    SWEEP_CSS_TOK_STRING,
    SWEEP_CSS_TOK_NUMBER,
    SWEEP_CSS_TOK_DIMENSION,
    SWEEP_CSS_TOK_PERCENTAGE,
    SWEEP_CSS_TOK_COLON,
    SWEEP_CSS_TOK_SEMICOLON,
    SWEEP_CSS_TOK_COMMA,
    SWEEP_CSS_TOK_LBRACE,
    SWEEP_CSS_TOK_RBRACE,
    SWEEP_CSS_TOK_LPAREN,
    SWEEP_CSS_TOK_RPAREN,
    SWEEP_CSS_TOK_LBRACKET,
    SWEEP_CSS_TOK_RBRACKET,
    SWEEP_CSS_TOK_WHITESPACE,
    SWEEP_CSS_TOK_COMMENT,
    SWEEP_CSS_TOK_DELIM,
    SWEEP_CSS_TOK_ERROR
} SWEEP_CSS_TOKEN_TYPE;

typedef enum _SWEEP_CSS_UNIT {
    SWEEP_CSS_UNIT_NONE = 0, /* not a length / unsupported unit */
    SWEEP_CSS_UNIT_AUTO,
    SWEEP_CSS_UNIT_PX,
    SWEEP_CSS_UNIT_EM,
    SWEEP_CSS_UNIT_REM,
    SWEEP_CSS_UNIT_PERCENT,
    SWEEP_CSS_UNIT_PT
} SWEEP_CSS_UNIT;

typedef struct _SWEEP_CSS_LENGTH {
    int32_t value_milli; /* thousandths of the unit */
    uint8_t unit;        /* SWEEP_CSS_UNIT */
} SWEEP_CSS_LENGTH;

typedef struct _SWEEP_CSS_TOKEN {
    SWEEP_CSS_TOKEN_TYPE type;
    char text[SWEEP_CSS_TOKEN_TEXT_MAX]; /* normalized token spelling */
    int32_t number_milli;                /* numeric value for NUMBER/DIM/PERCENT */
    uint8_t unit;                        /* SWEEP_CSS_UNIT for numeric tokens */
    bool unterminated;                   /* unterminated string/comment */
} SWEEP_CSS_TOKEN;

/* Tokenize CSS.  Emits at most cap tokens including a trailing EOF token.
 * Returns false when the input exceeds cap or on unrecoverable input; the
 * tokenizer still terminates deterministically. */
bool sweep_css_tokenize(const char* css, size_t len,
                        SWEEP_CSS_TOKEN* out, size_t cap, size_t* out_count);

/* ---- declarations ---- */
typedef struct _SWEEP_CSS_DECL {
    char name[SWEEP_CSS_PROP_MAX];         /* lowercased property name */
    char value[SWEEP_CSS_DECL_VALUE_MAX];  /* normalized textual value */
    bool important;
} SWEEP_CSS_DECL;

/* Parse a declaration list (block contents or an inline style attribute).
 * Invalid declarations are skipped deterministically; at most cap are
 * stored.  Returns false only for invalid arguments/overflow. */
bool sweep_css_parse_declarations(const char* text, size_t len,
                                  SWEEP_CSS_DECL* out, size_t cap,
                                  size_t* out_count);

/* ---- selectors ---- */
typedef enum _SWEEP_CSS_SIMPLE_KIND {
    SWEEP_CSS_SIMPLE_UNIVERSAL = 0,
    SWEEP_CSS_SIMPLE_TYPE,
    SWEEP_CSS_SIMPLE_CLASS,
    SWEEP_CSS_SIMPLE_ID,
    SWEEP_CSS_SIMPLE_ATTR_EXISTS,
    SWEEP_CSS_SIMPLE_ATTR_EQUALS
} SWEEP_CSS_SIMPLE_KIND;

typedef enum _SWEEP_CSS_COMBINATOR {
    SWEEP_CSS_COMB_NONE = 0,
    SWEEP_CSS_COMB_DESCENDANT,
    SWEEP_CSS_COMB_CHILD
} SWEEP_CSS_COMBINATOR;

typedef struct _SWEEP_CSS_SIMPLE {
    uint8_t kind; /* SWEEP_CSS_SIMPLE_KIND */
    char name[SWEEP_CSS_NAME_MAX];  /* type/class/id/attribute name */
    char value[SWEEP_CSS_ATTR_VALUE_MAX]; /* attribute equality value */
} SWEEP_CSS_SIMPLE;

typedef struct _SWEEP_CSS_COMPOUND {
    uint8_t combinator; /* relation to the previous part (part 0 = NONE) */
    uint8_t simple_count;
    SWEEP_CSS_SIMPLE simples[SWEEP_CSS_COMPOUND_SIMPLES_MAX];
} SWEEP_CSS_COMPOUND;

typedef struct _SWEEP_CSS_SELECTOR {
    uint8_t part_count;
    bool unsupported;         /* pseudo-class/element or unparseable part */
    uint16_t spec_a;          /* id count */
    uint16_t spec_b;          /* class/attribute count */
    uint16_t spec_c;          /* type count */
    SWEEP_CSS_COMPOUND parts[SWEEP_CSS_SELECTOR_PARTS_MAX];
} SWEEP_CSS_SELECTOR;

typedef struct _SWEEP_CSS_RULE {
    SWEEP_CSS_SELECTOR selectors[SWEEP_CSS_MAX_SELECTORS];
    uint8_t selector_count;
    uint8_t invalid_selectors;
    SWEEP_CSS_DECL decls[SWEEP_CSS_MAX_DECLS];
    uint8_t decl_count;
} SWEEP_CSS_RULE;

typedef struct _SWEEP_CSS_STYLESHEET {
    SWEEP_CSS_RULE rules[SWEEP_CSS_MAX_RULES];
    uint16_t rule_count;
    uint16_t error_count;
    bool truncated;
} SWEEP_CSS_STYLESHEET;

/* Parse a stylesheet.  Returns false for invalid arguments or tokenizer
 * overflow.  At-rules (including @media blocks) are skipped deterministically
 * (media queries are unsupported in this phase). */
bool sweep_css_parse_stylesheet(const char* css, size_t len,
                                SWEEP_CSS_STYLESHEET* out);

/* Append every rule from src onto dst in order (preserving cascade/source
 * order across stylesheets).  Sets dst->truncated when dst reaches
 * SWEEP_CSS_MAX_RULES; already-stored rules are kept.  Returns true on
 * success (possibly truncated), false for invalid arguments. */
bool sweep_css_stylesheet_append(SWEEP_CSS_STYLESHEET* dst,
                                 const SWEEP_CSS_STYLESHEET* src);

/* Match one selector against one DOM node (id = index+1 as in sweep_html). */
bool sweep_css_selector_matches(const SWEEP_DOM* dom, uint32_t node,
                                const SWEEP_CSS_SELECTOR* selector);

/* Parse a single selector (no selector list / comma) for querySelector and
 * related bindings.  Returns false for unsupported syntax rather than
 * pretending to match it. */
bool sweep_css_parse_selector(const char* text, size_t len,
                              SWEEP_CSS_SELECTOR* out);

/* ---- computed styles ---- */
typedef enum _SWEEP_DISPLAY {
    SWEEP_DISPLAY_INLINE = 0,
    SWEEP_DISPLAY_BLOCK,
    SWEEP_DISPLAY_NONE,
    SWEEP_DISPLAY_INLINE_BLOCK
} SWEEP_DISPLAY;

typedef enum _SWEEP_POSITION {
    SWEEP_POSITION_STATIC = 0,
    SWEEP_POSITION_RELATIVE,
    SWEEP_POSITION_ABSOLUTE,
    SWEEP_POSITION_FIXED
} SWEEP_POSITION;

typedef enum _SWEEP_TEXT_ALIGN {
    SWEEP_TEXT_ALIGN_LEFT = 0,
    SWEEP_TEXT_ALIGN_CENTER,
    SWEEP_TEXT_ALIGN_RIGHT,
    SWEEP_TEXT_ALIGN_JUSTIFY
} SWEEP_TEXT_ALIGN;

/* Box side indexes: top, right, bottom, left. */
#define SWEEP_SIDE_TOP    0u
#define SWEEP_SIDE_RIGHT  1u
#define SWEEP_SIDE_BOTTOM 2u
#define SWEEP_SIDE_LEFT   3u

typedef struct _SWEEP_COMPUTED_STYLE {
    uint8_t display;    /* SWEEP_DISPLAY */
    uint8_t position;   /* SWEEP_POSITION (layout implements static flow only) */
    uint8_t text_align; /* SWEEP_TEXT_ALIGN */

    SWEEP_CSS_LENGTH width;  /* AUTO initial */
    SWEEP_CSS_LENGTH height; /* AUTO initial */

    SWEEP_CSS_LENGTH margin[4];
    SWEEP_CSS_LENGTH padding[4];
    SWEEP_CSS_LENGTH border_width[4];

    uint32_t color;            /* 0xAARRGGBB, initial opaque black */
    uint32_t background_color; /* 0xAARRGGBB, initial transparent */
    uint32_t border_color;     /* 0xAARRGGBB, initial opaque black */

    int32_t font_size_px; /* resolved px, initial 16 */
    int32_t font_weight;  /* 100..900, initial 400 */
    char font_family[SWEEP_CSS_FONT_FAMILY_MAX];
} SWEEP_COMPUTED_STYLE;

typedef struct _SWEEP_CSS_STATS {
    uint32_t rules_matched;
    uint32_t declarations_applied;
    uint32_t declarations_ignored;
    uint32_t unsupported_values;
    /* Phase 5: selector-matching work accounting.  selector_tests counts
     * sweep_css_selector_matches evaluations actually performed; rules_skipped
     * counts rule entries skipped by dependency/index candidate filtering
     * (a rule may have multiple selectors).  A conservative full-reference
     * run performs selector_tests for every (styled-node, selector) pair. */
    uint32_t selector_tests;
    uint32_t rules_skipped;
    uint32_t nodes_recomputed; /* nodes whose computed style was (re)derived */
} SWEEP_CSS_STATS;

void sweep_css_style_initial(SWEEP_COMPUTED_STYLE* style);
void sweep_css_style_ua_defaults(const char* tag, SWEEP_COMPUTED_STYLE* style);

/* Compute styles for every node (comments/text inherit typography and are
 * display:inline).  out is indexed by (node id - 1).  Returns number of nodes
 * styled, or 0 on invalid input/insufficient storage. */
uint32_t sweep_css_compute_all(const SWEEP_DOM* dom,
                               const SWEEP_CSS_STYLESHEET* sheet,
                               SWEEP_COMPUTED_STYLE* out, size_t out_cap,
                               SWEEP_CSS_STATS* stats);

/* ---- selector dependency index (Phase 5) ----
 * Bounded index over author-rule selectors.  Used for (a) conservative
 * mutation-to-selector invalidation decisions and (b) incremental candidate
 * filtering: when computing styles, rules whose dependency keys cannot match
 * a node's features are skipped, reducing selector evaluations while keeping
 * final computed styles identical to a full reference run.  Never used to
 * skip a required recomputation. */
#define SWEEP_CSS_DEP_KEY_MAX  32u
#define SWEEP_CSS_DEP_NAME_MAX SWEEP_CSS_NAME_MAX

typedef struct _SWEEP_CSS_DEP_SET {
    char names[SWEEP_CSS_DEP_KEY_MAX][SWEEP_CSS_DEP_NAME_MAX];
    uint8_t count;
    bool overflow; /* more distinct keys than tracked; treat as "any" */
} SWEEP_CSS_DEP_SET;

typedef struct _SWEEP_CSS_DEP_INDEX {
    bool has_universal;
    bool has_structural; /* any descendant or child combinator */
    bool has_descendant;
    bool has_child;
    bool has_type;
    bool has_class;
    bool has_id;
    bool has_attr;
    SWEEP_CSS_DEP_SET types;
    SWEEP_CSS_DEP_SET classes;
    SWEEP_CSS_DEP_SET ids;
    SWEEP_CSS_DEP_SET attrs;      /* attribute names (existence or equality) */
    SWEEP_CSS_DEP_SET attr_pairs; /* "name=value" for equality selectors */
    uint16_t rule_count;
    uint16_t selector_count;
    /* Per-rule mask: 1 if the rule's selectors can possibly match a node given
     * only the node's own tag/id/classes/attributes (ignoring tree position).
     * Structural selectors (descendant/child) are always "possible" for any
     * node because a match depends on ancestors/siblings; universal is always
     * possible.  Used for candidate filtering inside compute. */
    uint8_t rule_needs_tree[SWEEP_CSS_MAX_RULES];
} SWEEP_CSS_DEP_INDEX;

typedef enum _SWEEP_CSS_MUTATION_KIND {
    SWEEP_CSS_MUT_NONE = 0,
    SWEEP_CSS_MUT_TEXT,          /* text-content change: not selector-relevant */
    SWEEP_CSS_MUT_ATTR,          /* attribute/class/id/style change */
    SWEEP_CSS_MUT_INSERT,        /* element insertion */
    SWEEP_CSS_MUT_REMOVE,        /* element removal */
    SWEEP_CSS_MUT_REORDER,       /* child reordering */
    SWEEP_CSS_MUT_STYLESHEET     /* stylesheet added/removed/changed */
} SWEEP_CSS_MUTATION_KIND;

/* Build an index from all selectors in a stylesheet.  Safe on empty sheets. */
void sweep_css_dep_index_build(const SWEEP_CSS_STYLESHEET* sheet,
                               SWEEP_CSS_DEP_INDEX* out);

/* Conservative answer: could this mutation kind (optionally targeting an
 * attribute name / old+new values for class or id) affect any author rule?
 * When it cannot prove "no", it returns true.  attr_name/class_changed/
 * id_changed may be NULL/false when unknown. */
bool sweep_css_dep_mutation_affects_style(const SWEEP_CSS_DEP_INDEX* index,
                                          SWEEP_CSS_MUTATION_KIND kind,
                                          const char* attr_name,
                                          bool class_changed,
                                          bool id_changed);

/* Compute styles using candidate filtering from the dependency index: rules
 * that cannot match a node based on its own features and the index are not
 * selector-tested for that node (structural/universal rules are never
 * skipped).  Final computed styles are identical to sweep_css_compute_all for
 * the same DOM/sheet.  index may be NULL (falls back to full evaluation).
 * Stats report selector_tests/rules_skipped/nodes_recomputed. */
uint32_t sweep_css_compute_all_indexed(const SWEEP_DOM* dom,
                                       const SWEEP_CSS_STYLESHEET* sheet,
                                       const SWEEP_CSS_DEP_INDEX* index,
                                       SWEEP_COMPUTED_STYLE* out,
                                       size_t out_cap,
                                       SWEEP_CSS_STATS* stats);

/* Recompute styles for one subtree rooted at node_id (inclusive) in place.
 * Preserves computed styles of nodes outside that subtree.  parent_style must
 * be the already-current computed style of node_id's parent (NULL for the
 * document root).  Returns number of nodes recomputed.  Safe incremental
 * element/subtree restyling when the caller can prove no nodes outside the
 * subtree are affected. */
uint32_t sweep_css_recompute_subtree_indexed(const SWEEP_DOM* dom,
                                             const SWEEP_CSS_STYLESHEET* sheet,
                                             const SWEEP_CSS_DEP_INDEX* index,
                                             uint32_t node_id,
                                             const SWEEP_COMPUTED_STYLE* parent_style,
                                             SWEEP_COMPUTED_STYLE* styles,
                                             size_t out_cap,
                                             SWEEP_CSS_STATS* stats);

/* Resolve a length to integer pixels.  base_px is the containing-block width
 * for percentages; font_px is the current font size for em. */
int32_t sweep_css_length_resolve_px(const SWEEP_CSS_LENGTH* length,
                                    int32_t base_px, int32_t font_px);

/* Deterministic temporary text measurement: decodes UTF-8/SUCS codepoints and
 * assigns fixed advances (space = font/4, ASCII = font/2, other = font).
 * This is NOT font shaping or real font metrics; it exists so layout/paint
 * can be exercised before a real font engine is integrated. */
int32_t sweep_text_measure_px(const char* text, int32_t font_px,
                              SWEEP_TEXT_MODE mode);

/* ---- layout ---- */
typedef struct _SWEEP_LAYOUT_BOX {
    bool in_use;
    uint32_t node;   /* DOM node id (index+1), 0 for synthetic */
    uint32_t parent; /* layout box index+1, 0 = root */
    uint32_t first_child;
    uint32_t next_sibling;

    uint8_t display; /* SWEEP_DISPLAY */

    /* Border-box rectangle (excludes margin). */
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    /* Content box rectangle. */
    int32_t content_x;
    int32_t content_y;
    int32_t content_w;
    int32_t content_h;

    int32_t margin[4];
    int32_t padding[4];
    int32_t border[4];

    /* For SWEEP_DOM_TEXT nodes: byte slice of the node text painted by this
     * box.  Word-splitting produces several boxes for one text node; offset
     * and length keep paint output non-duplicated and bounded. */
    uint32_t text_offset;
    uint32_t text_len;

    uint32_t background_color;
    uint32_t border_color;
    uint32_t color;
    int32_t font_size_px;
    uint8_t text_align;
} SWEEP_LAYOUT_BOX;

typedef struct _SWEEP_LAYOUT {
    const SWEEP_DOM* dom;
    SWEEP_LAYOUT_BOX boxes[SWEEP_LAYOUT_MAX_BOXES];
    uint32_t count;
    uint32_t root; /* index+1 */
    SWEEP_VIEWPORT viewport;
    uint32_t content_height;
    bool truncated; /* box limit hit */
} SWEEP_LAYOUT;

/* Build a layout tree from the DOM and computed styles.  Returns false for
 * invalid input (dom/styles/viewport), including a zero-size viewport. */
bool sweep_layout_build(const SWEEP_DOM* dom,
                        const SWEEP_COMPUTED_STYLE* styles,
                        size_t style_count,
                        const SWEEP_VIEWPORT* viewport,
                        SWEEP_TEXT_MODE mode,
                        SWEEP_LAYOUT* out);

/* Generate bounded paint commands from a layout.  Emits CLEAR first, then
 * backgrounds/borders/text in tree order.  Returns false when the command
 * capacity is exceeded (out_count then holds the number actually emitted). */
bool sweep_paint_from_layout(const SWEEP_LAYOUT* layout, SWEEP_TEXT_MODE mode,
                             SWEEP_PAINT_CMD* out, size_t cap,
                             size_t* out_count);

#endif /* SWEEP_CSS_H */
