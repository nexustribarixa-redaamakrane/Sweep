/* sweep_css_test.c - standalone host tests for the Sweep CSS engine.
 * Build (from Sweep root):
 *   gcc -std=c99 -Wall -Wextra -Werror -Isweep
 *       sweep/sweep_html.c sweep/sweep_text.c sweep/sweep_engine.c
 *       sweep/sweep_css.c sweep/tests/sweep_css_test.c
 *       -o build-generated/sweep_css_test.exe && build-generated/sweep_css_test.exe
 *
 * Large objects (DOM, styles, stylesheet, layout) are file-scope to keep the
 * host stack small; this mirrors the constrained-target intent of the engine.
 */
#include <stdio.h>
#include <string.h>

#include "../sweep_css.h"

static int g_failures = 0;
static int g_passed = 0;

static SWEEP_DOM g_dom;
static SWEEP_COMPUTED_STYLE g_styles[SWEEP_DOM_NODES_MAX];
static SWEEP_CSS_STYLESHEET g_sheet;
static SWEEP_LAYOUT g_layout;
static SWEEP_CSS_STATS g_stats;
static SWEEP_VIEWPORT g_viewport;

static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

static void check_eq_i(const char* label, int got, int want) {
    if (got == want) {
        ++g_passed;
        printf("[PASS] %s (= %d)\n", label, want);
    } else {
        ++g_failures;
        printf("[FAIL] %s: got %d, want %d\n", label, got, want);
    }
}

static bool setup(const char* html, const char* css, uint32_t w, uint32_t h,
                  SWEEP_TEXT_MODE mode) {
    size_t n;
    memset(&g_sheet, 0, sizeof(g_sheet));
    memset(&g_stats, 0, sizeof(g_stats));
    if (!html) return false;
    if (sweep_dom_parse(&g_dom, html, strlen(html)) == 0u) return false;
    if (css && !sweep_css_parse_stylesheet(css, strlen(css), &g_sheet)) {
        return false;
    }
    n = sweep_css_compute_all(&g_dom, &g_sheet, g_styles, SWEEP_DOM_NODES_MAX,
                              &g_stats);
    if (n == 0u) return false;
    g_viewport.width_px = w;
    g_viewport.height_px = h;
    g_viewport.device_scale = 1u;
    return sweep_layout_build(&g_dom, g_styles, SWEEP_DOM_NODES_MAX,
                              &g_viewport, mode, &g_layout);
}

static uint32_t node_by_id(const char* id) {
    uint32_t i;
    for (i = 0u; i < g_dom.count; ++i) {
        const SWEEP_DOM_NODE* n = &g_dom.nodes[i];
        if (n->in_use && n->type == SWEEP_DOM_ELEMENT) {
            const char* v = sweep_dom_get_attr(n, "id");
            if (v && strcmp(v, id) == 0) return i + 1u;
        }
    }
    return 0u;
}

static SWEEP_LAYOUT_BOX* box_for(uint32_t node) {
    uint32_t i;
    for (i = 0u; i < g_layout.count; ++i) {
        if (g_layout.boxes[i].in_use && g_layout.boxes[i].node == node) {
            return &g_layout.boxes[i];
        }
    }
    return 0;
}

static uint32_t count_text_boxes(void) {
    uint32_t i, n = 0u;
    for (i = 0u; i < g_layout.count; ++i) {
        if (g_layout.boxes[i].in_use && g_layout.boxes[i].text_len > 0u) ++n;
    }
    return n;
}

static uint32_t count_paint_op(const SWEEP_PAINT_CMD* cmds, size_t count,
                               uint8_t op) {
    size_t i;
    uint32_t n = 0u;
    for (i = 0u; i < count; ++i) if (cmds[i].op == op) ++n;
    return n;
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                          */
/* ------------------------------------------------------------------ */
static void test_tokenizer(void) {
    SWEEP_CSS_TOKEN tok[64];
    size_t n = 0u, i;

    check("tok: basic declaration",
          sweep_css_tokenize("color: red;", strlen("color: red;"), tok, 64u,
                             &n) && n == 6u);
    check("tok: ident first", tok[0].type == SWEEP_CSS_TOK_IDENT &&
          strcmp(tok[0].text, "color") == 0);
    check("tok: colon", tok[1].type == SWEEP_CSS_TOK_COLON);
    check("tok: value ident", tok[3].type == SWEEP_CSS_TOK_IDENT &&
          strcmp(tok[3].text, "red") == 0);
    check("tok: eof last", tok[n - 1u].type == SWEEP_CSS_TOK_EOF);

    check("tok: comment",
          sweep_css_tokenize("/* x */p{}", strlen("/* x */p{}"), tok, 64u, &n) &&
          n >= 5u && tok[0].type == SWEEP_CSS_TOK_COMMENT);
    check("tok: comment followed by ident",
          tok[1].type == SWEEP_CSS_TOK_IDENT && strcmp(tok[1].text, "p") == 0);

    check("tok: unterminated comment",
          sweep_css_tokenize("/* open", strlen("/* open"), tok, 64u, &n) &&
          tok[0].type == SWEEP_CSS_TOK_COMMENT && tok[0].unterminated);

    check("tok: string",
          sweep_css_tokenize("\"a b\"", strlen("\"a b\""), tok, 64u, &n) &&
          tok[0].type == SWEEP_CSS_TOK_STRING && strcmp(tok[0].text, "\"a b\"") == 0);
    check("tok: unterminated string",
          sweep_css_tokenize("\"abc", strlen("\"abc"), tok, 64u, &n) &&
          tok[0].type == SWEEP_CSS_TOK_STRING && tok[0].unterminated);

    check("tok: dimensions/percent/number",
          sweep_css_tokenize("300px 50% 1.5 -2", strlen("300px 50% 1.5 -2"), tok,
                             64u, &n) && n >= 7u);
    check("tok: dimension type", tok[0].type == SWEEP_CSS_TOK_DIMENSION);
    check_eq_i("tok: dimension value", tok[0].number_milli, 300000);
    check("tok: dimension unit", tok[0].unit == SWEEP_CSS_UNIT_PX);
    check("tok: percentage type", tok[2].type == SWEEP_CSS_TOK_PERCENTAGE);
    check_eq_i("tok: percentage value", tok[2].number_milli, 50000);
    check("tok: number type", tok[4].type == SWEEP_CSS_TOK_NUMBER);
    check_eq_i("tok: fractional value", tok[4].number_milli, 1500);
    check_eq_i("tok: negative value", tok[6].number_milli, -2000);

    check("tok: punctuation",
          sweep_css_tokenize("{ } ( ) [ ]", strlen("{ } ( ) [ ]"), tok, 64u,
                             &n) && n >= 7u);
    {
        static const SWEEP_CSS_TOKEN_TYPE want[6] = {
            SWEEP_CSS_TOK_LBRACE, SWEEP_CSS_TOK_RBRACE,
            SWEEP_CSS_TOK_LPAREN, SWEEP_CSS_TOK_RPAREN,
            SWEEP_CSS_TOK_LBRACKET, SWEEP_CSS_TOK_RBRACKET
        };
        int ok = 1;
        size_t k = 0u;
        for (i = 0u; i < n && k < 6u; ++i) {
            if (tok[i].type == SWEEP_CSS_TOK_WHITESPACE) continue;
            if (tok[i].type != want[k]) ok = 0;
            ++k;
        }
        check("tok: punctuation sequence", ok && k == 6u);
    }

    check("tok: token cap overflow rejected",
          !sweep_css_tokenize("a b c d e", strlen("a b c d e"), tok, 4u, &n));
    check("tok: bad args rejected",
          !sweep_css_tokenize("a", 1u, (SWEEP_CSS_TOKEN*)0, 4u, &n));
    check("tok: tiny cap rejected",
          !sweep_css_tokenize("a", 1u, tok, 1u, &n));
}

/* ------------------------------------------------------------------ */
/* Declarations                                                       */
/* ------------------------------------------------------------------ */
static void test_declarations(void) {
    SWEEP_CSS_DECL d[8];
    size_t n = 0u;

    check("decl: two declarations",
          sweep_css_parse_declarations("color: red; width: 300px;",
                                       strlen("color: red; width: 300px;"),
                                       d, 8u, &n) && n == 2u);
    check("decl: name normalized", strcmp(d[0].name, "color") == 0);
    check("decl: value", strcmp(d[0].value, "red") == 0);
    check("decl: dimension value", strcmp(d[1].value, "300px") == 0);

    check("decl: uppercase property name lowercased",
          sweep_css_parse_declarations("Color: Red;", strlen("Color: Red;"),
                                       d, 8u, &n) && n == 1u &&
          strcmp(d[0].name, "color") == 0);

    check("decl: whitespace tolerated",
          sweep_css_parse_declarations("  color :  red ;  ",
                                       strlen("  color :  red ;  "), d, 8u,
                                       &n) && n == 1u &&
          strcmp(d[0].name, "color") == 0 &&
          strcmp(d[0].value, "red") == 0);

    check("decl: !important recognized",
          sweep_css_parse_declarations("color: red !important;",
                                       strlen("color: red !important;"), d, 8u,
                                       &n) && n == 1u && d[0].important &&
          strcmp(d[0].value, "red") == 0);

    check("decl: invalid skipped",
          sweep_css_parse_declarations("color red; width: 10px;",
                                       strlen("color red; width: 10px;"), d, 8u,
                                       &n) && n == 1u &&
          strcmp(d[0].name, "width") == 0);

    check("decl: empty list",
          sweep_css_parse_declarations("", 0u, d, 8u, &n) && n == 0u);
    check("decl: bad args",
          !sweep_css_parse_declarations("a:b", 3u, (SWEEP_CSS_DECL*)0, 8u, &n));
}

/* ------------------------------------------------------------------ */
/* Selectors                                                          */
/* ------------------------------------------------------------------ */
static void test_selectors(void) {
    static const char* html =
        "<div id=\"main\" class=\"article featured\">"
        "<p id=\"p1\" hidden=\"1\">A</p>"
        "<p id=\"p2\" type=\"text\">B</p>"
        "<span id=\"sp\">c</span>"
        "</div>";
    static const char* css =
        "*{} p{} .article{} #main{} div p{} div > p{} "
        ".article.featured{} [hidden]{} [type=\"text\"]{} "
        "h1, h2{} p:hover{}";
    uint32_t divn, p1, p2, sp;

    check("sel: stylesheet parses",
          setup(html, css, 400u, 300u, SWEEP_TEXT_WEB_COMPAT));
    check_eq_i("sel: rule count", (int)g_sheet.rule_count, 11);
    check_eq_i("sel: unsupported selector counted",
               (int)g_sheet.rules[10].invalid_selectors, 1);

    divn = node_by_id("main");
    p1 = node_by_id("p1");
    p2 = node_by_id("p2");
    sp = node_by_id("sp");
    check("sel: ids resolved", divn != 0u && p1 != 0u && p2 != 0u && sp != 0u);

    check("sel: universal matches div",
          sweep_css_selector_matches(&g_dom, divn, &g_sheet.rules[0].selectors[0]));
    check("sel: universal matches span",
          sweep_css_selector_matches(&g_dom, sp, &g_sheet.rules[0].selectors[0]));
    check("sel: type p matches p1",
          sweep_css_selector_matches(&g_dom, p1, &g_sheet.rules[1].selectors[0]));
    check("sel: type p does not match span",
          !sweep_css_selector_matches(&g_dom, sp, &g_sheet.rules[1].selectors[0]));
    check("sel: class matches div",
          sweep_css_selector_matches(&g_dom, divn, &g_sheet.rules[2].selectors[0]));
    check("sel: class does not match p",
          !sweep_css_selector_matches(&g_dom, p1, &g_sheet.rules[2].selectors[0]));
    check("sel: id matches",
          sweep_css_selector_matches(&g_dom, divn, &g_sheet.rules[3].selectors[0]));
    check("sel: id does not match other",
          !sweep_css_selector_matches(&g_dom, p1, &g_sheet.rules[3].selectors[0]));
    check("sel: descendant matches p1",
          sweep_css_selector_matches(&g_dom, p1, &g_sheet.rules[4].selectors[0]));
    check("sel: descendant rejectsdiv",
          !sweep_css_selector_matches(&g_dom, divn, &g_sheet.rules[4].selectors[0]));
    check("sel: child matches p2",
          sweep_css_selector_matches(&g_dom, p2, &g_sheet.rules[5].selectors[0]));
    check("sel: multiple classes match",
          sweep_css_selector_matches(&g_dom, divn, &g_sheet.rules[6].selectors[0]));
    check("sel: multiple classes reject partial",
          !sweep_css_selector_matches(&g_dom, p1, &g_sheet.rules[6].selectors[0]));
    check("sel: [hidden] matches p1",
          sweep_css_selector_matches(&g_dom, p1, &g_sheet.rules[7].selectors[0]));
    check("sel: [hidden] rejects p2",
          !sweep_css_selector_matches(&g_dom, p2, &g_sheet.rules[7].selectors[0]));
    check("sel: [type=text] matches p2",
          sweep_css_selector_matches(&g_dom, p2, &g_sheet.rules[8].selectors[0]));
    check("sel: selector list has two entries",
          g_sheet.rules[9].selector_count == 2u);
    check("sel: h1 list no match",
          !sweep_css_selector_matches(&g_dom, divn, &g_sheet.rules[9].selectors[0]));
    check("sel: unsupported selector never matches",
          g_sheet.rules[10].selector_count == 0u);
    check_eq_i("sel: specificity a/b/c",
               (int)(g_sheet.rules[5].selectors[0].spec_a * 100 +
                     g_sheet.rules[5].selectors[0].spec_b * 10 +
                     g_sheet.rules[5].selectors[0].spec_c), 2);
}

/* ------------------------------------------------------------------ */
/* Cascade, specificity, inheritance                                  */
/* ------------------------------------------------------------------ */
static void test_cascade(void) {
    uint32_t p;

    check("cascade: setup",
          setup("<p id=\"i\" class=\"c\">Hi</p>",
                "p{color:red} .c{color:green} #i{color:blue}", 300u, 200u,
                SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("i");
    check("cascade: id specificity wins",
          p != 0u && g_styles[p - 1u].color == 0xFF0000FFu);

    check("cascade: source order setup",
          setup("<p id=\"i\">Hi</p>", "p{color:red} p{color:green}", 300u, 200u,
                SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("i");
    check("cascade: later rule wins",
          p != 0u && g_styles[p - 1u].color == 0xFF008000u);

    check("cascade: inline setup",
          setup("<p id=\"i\" style=\"color:purple\">Hi</p>",
                "#i{color:blue}", 300u, 200u, SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("i");
    check("cascade: inline beats selector",
          p != 0u && g_styles[p - 1u].color == 0xFF800080u);

    check("cascade: important setup",
          setup("<p id=\"i\" style=\"color:purple\">Hi</p>",
                "#i{color:blue !important}", 300u, 200u,
                SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("i");
    check("cascade: important beats inline",
          p != 0u && g_styles[p - 1u].color == 0xFF0000FFu);

    check("cascade: inheritance setup",
          setup("<div id=\"o\" style=\"color:red;font-size:20px;"
                "background-color:blue\"><span id=\"in\">x</span></div>",
                (const char*)0, 300u, 200u, SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("in");
    check("inherit: color inherited",
          p != 0u && g_styles[p - 1u].color == 0xFFFF0000u);
    check("inherit: font size inherited",
          p != 0u && g_styles[p - 1u].font_size_px == 20);
    check("inherit: background not inherited",
          p != 0u && g_styles[p - 1u].background_color ==
                        SWEEP_CSS_COLOR_TRANSPARENT);
    check("inherit: display initial inline",
          p != 0u && g_styles[p - 1u].display == SWEEP_DISPLAY_INLINE);

    check("cascade: em font setup",
          setup("<div style=\"font-size:20px\"><span id=\"in\" "
                "style=\"font-size:2em\">x</span></div>",
                (const char*)0, 300u, 200u, SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("in");
    check("inherit: em resolved against parent", p != 0u &&
          g_styles[p - 1u].font_size_px == 40);

    check("cascade: initial values setup",
          setup("<div><span id=\"in\">x</span></div>", (const char*)0,
                300u, 200u, SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("in");
    check("initial: width auto",
          p != 0u && g_styles[p - 1u].width.unit == SWEEP_CSS_UNIT_AUTO);
    check("initial: margin zero",
          p != 0u && g_styles[p - 1u].margin[SWEEP_SIDE_TOP].value_milli == 0);
    check("initial: color black",
          p != 0u && g_styles[p - 1u].color == SWEEP_CSS_COLOR_BLACK);

    check("cascade: UA heading defaults setup",
          setup("<h1 id=\"h\">T</h1><script>x</script>", (const char*)0,
                300u, 200u, SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("h");
    check("ua: h1 block", p != 0u &&
          g_styles[p - 1u].display == SWEEP_DISPLAY_BLOCK);
    check("ua: h1 font size", p != 0u && g_styles[p - 1u].font_size_px == 32);
    check("ua: h1 bold", p != 0u && g_styles[p - 1u].font_weight == 700);
    check("ua: script not rendered",
          box_for(sweep_dom_find_element(&g_dom, g_dom.document, "script")) == 0);
    check_eq_i("cascade: no rules matched", (int)g_stats.rules_matched, 0);
}

/* ------------------------------------------------------------------ */
/* Length resolution and temporary measurement                        */
/* ------------------------------------------------------------------ */
static void test_lengths(void) {
    SWEEP_CSS_LENGTH len;

    len.value_milli = 300000; len.unit = SWEEP_CSS_UNIT_PX;
    check_eq_i("len: px", sweep_css_length_resolve_px(&len, 200, 16), 300);
    len.value_milli = 50000; len.unit = SWEEP_CSS_UNIT_PERCENT;
    check_eq_i("len: percent", sweep_css_length_resolve_px(&len, 200, 16), 100);
    len.value_milli = 2000; len.unit = SWEEP_CSS_UNIT_EM;
    check_eq_i("len: em", sweep_css_length_resolve_px(&len, 200, 16), 32);
    len.value_milli = 12000; len.unit = SWEEP_CSS_UNIT_PT;
    check_eq_i("len: pt", sweep_css_length_resolve_px(&len, 200, 16), 16);
    len.value_milli = 0; len.unit = SWEEP_CSS_UNIT_AUTO;
    check_eq_i("len: auto is not px", sweep_css_length_resolve_px(&len, 200, 16), 0);

    check_eq_i("measure: ascii", sweep_text_measure_px("A", 16,
               SWEEP_TEXT_WEB_COMPAT), 8);
    check_eq_i("measure: space", sweep_text_measure_px("A A", 16,
               SWEEP_TEXT_WEB_COMPAT), 20);
    check_eq_i("measure: non-ascii",
               sweep_text_measure_px("\xC3\xA9", 16, SWEEP_TEXT_WEB_COMPAT), 16);
    check_eq_i("measure: astral is one codepoint (web)",
               sweep_text_measure_px("\xF0\x9F\x98\x80", 16,
                                     SWEEP_TEXT_WEB_COMPAT), 16);
    check_eq_i("measure: astral is one codepoint (native)",
               sweep_text_measure_px("\xF0\x9F\x98\x80", 16,
                                     SWEEP_TEXT_NATIVE_SU), 16);
}

/* ------------------------------------------------------------------ */
/* Layout                                                             */
/* ------------------------------------------------------------------ */
static void test_layout(void) {
    SWEEP_LAYOUT_BOX* b;
    uint32_t p, d, text;

    check("layout: block page setup",
          setup("<html><body style=\"margin:0\">"
                "<div id=\"d\" style=\"width:300px;height:200px;margin:10px;"
                "padding:8px;background-color:blue\"></div></body></html>",
                (const char*)0, 400u, 300u, SWEEP_TEXT_WEB_COMPAT));
    d = node_by_id("d");
    b = box_for(d);
    check("layout: block box exists", b != 0);
    if (b) {
        check_eq_i("layout: block x", b->x, 10);
        check_eq_i("layout: block y", b->y, 10);
        check_eq_i("layout: border box width", b->width, 316);
        check_eq_i("layout: border box height", b->height, 216);
        check_eq_i("layout: content x", b->content_x, 18);
        check_eq_i("layout: content y", b->content_y, 18);
        check_eq_i("layout: content width", b->content_w, 300);
        check_eq_i("layout: content height", b->content_h, 200);
        check_eq_i("layout: margin left", b->margin[SWEEP_SIDE_LEFT], 10);
        check_eq_i("layout: padding top", b->padding[SWEEP_SIDE_TOP], 8);
        check("layout: background kept", b->background_color == 0xFF0000FFu);
    }

    check("layout: viewport constraint setup",
          setup("<html><body style=\"margin:0\"><div id=\"d\"></div></body>"
                "</html>", (const char*)0, 200u, 100u,
                SWEEP_TEXT_WEB_COMPAT));
    d = node_by_id("d");
    b = box_for(d);
    check("layout: auto width box", b != 0);
    if (b) {
        check_eq_i("layout: auto width fills viewport", b->content_w, 200);
        check_eq_i("layout: auto box x", b->x, 0);
    }

    check("layout: nested stacking setup",
          setup("<body style=\"margin:0\"><div style=\"margin:0\">"
                "<div id=\"a\" style=\"height:50px;margin:0\"></div>"
                "<div id=\"b\" style=\"height:30px;margin:0\"></div>"
                "</div></body>", (const char*)0, 300u, 200u,
                SWEEP_TEXT_WEB_COMPAT));
    b = box_for(node_by_id("a"));
    check("layout: first child at top", b != 0 && b->y == 0 && b->height == 50);
    b = box_for(node_by_id("b"));
    check("layout: second child stacked", b != 0 && b->y == 50 &&
          b->height == 30);

    check("layout: inline text setup",
          setup("<body style=\"margin:0\"><p id=\"p\" style=\"margin:0;"
                "font-size:10px\">Hello world</p></body>",
                (const char*)0, 300u, 200u, SWEEP_TEXT_WEB_COMPAT));
    p = node_by_id("p");
    b = box_for(p);
    check("layout: paragraph box", b != 0 && b->height == 12);
    check_eq_i("layout: two text boxes", (int)count_text_boxes(), 2);
    if (b && g_dom.nodes[p - 1u].child_count > 0u) {
        text = g_dom.nodes[p - 1u].children[0];
        {
            uint32_t i;
            uint32_t found = 0u;
            for (i = 0u; i < g_layout.count; ++i) {
                SWEEP_LAYOUT_BOX* tb = &g_layout.boxes[i];
                if (tb->in_use && tb->node == text) {
                    if (found == 0u) {
                        check_eq_i("layout: first word x", tb->x, 0);
                        check_eq_i("layout: first word width", tb->width, 25);
                    } else if (found == 1u) {
                        check_eq_i("layout: second word x", tb->x, 27);
                        check_eq_i("layout: second word width", tb->width, 25);
                    }
                    ++found;
                }
            }
            check_eq_i("layout: words materialized", (int)found, 2);
        }
    }

    check("layout: inline element setup",
          setup("<body style=\"margin:0\"><p style=\"margin:0\">"
                "<span id=\"s\">Hi</span></p></body>", (const char*)0,
                300u, 200u, SWEEP_TEXT_WEB_COMPAT));
    b = box_for(node_by_id("s"));
    check("layout: inline span box", b != 0 && b->display == SWEEP_DISPLAY_INLINE);
    check("layout: inline span width", b != 0 && b->width == 16);
    check("layout: inline span owns text", b != 0 && b->first_child != 0u &&
          g_layout.boxes[b->first_child - 1u].text_len == 2u);
    check("layout: inline span text parent", b != 0 && b->first_child != 0u &&
          g_layout.boxes[b->first_child - 1u].parent ==
              (uint32_t)((size_t)(b - g_layout.boxes) + 1u));

    check("layout: display none setup",
          setup("<body style=\"margin:0\"><div id=\"d\" style=\"display:none\">"
                "x</div><p id=\"v\">y</p></body>", (const char*)0, 300u, 200u,
                SWEEP_TEXT_WEB_COMPAT));
    check("layout: display none omitted", box_for(node_by_id("d")) == 0);
    check("layout: sibling after none laid out", box_for(node_by_id("v")) != 0);

    check("layout: wrapping setup",
          setup("<body style=\"margin:0\"><p style=\"margin:0;font-size:10px;"
                "width:30px\">aaa bbb</p></body>", (const char*)0, 300u, 200u,
                SWEEP_TEXT_WEB_COMPAT));
    check_eq_i("layout: wrapped into two boxes", (int)count_text_boxes(), 2);
    {
        uint32_t i;
        int saw_first = 0, saw_second = 0;
        for (i = 0u; i < g_layout.count; ++i) {
            SWEEP_LAYOUT_BOX* tb = &g_layout.boxes[i];
            if (!tb->in_use || tb->text_len == 0u) continue;
            if (!saw_first) { saw_first = 1; check_eq_i("layout: wrap line 1", tb->y, 0); }
            else if (tb->y == 12) { saw_second = 1; }
        }
        check("layout: wrap line 2 at line height", saw_second);
    }

    check("layout: overflow setup",
          setup("<body style=\"margin:0\"><div id=\"d\" style=\"height:20px\">"
                "alpha beta gamma delta epsilon zeta eta theta</div></body>",
                (const char*)0, 80u, 20u, SWEEP_TEXT_WEB_COMPAT));
    b = box_for(node_by_id("d"));
    check("layout: fixed height kept", b != 0 && b->height == 20);
    check("layout: content overflows viewport",
          g_layout.content_height >= 20u && count_text_boxes() > 1u);

    check("layout: percentage height treated as auto",
          setup("<body style=\"margin:0\"><div id=\"d\" style=\"height:50%;"
                "font-size:10px\">Hi</div></body>", (const char*)0, 200u, 100u,
                SWEEP_TEXT_WEB_COMPAT));
    b = box_for(node_by_id("d"));
    check("layout: percentage height box", b != 0);
    if (b) {
        /* 50% of width would be 100px; auto content is one 12px line. */
        check_eq_i("layout: percentage height not width-resolved",
                   b->content_h, 12);
        check_eq_i("layout: percentage height border box", b->height, 12);
    }

    check("layout: oversized viewport clamped setup",
          setup("<body style=\"margin:0\"><div id=\"d\"></div></body>",
                (const char*)0, 0xFFFFFFFFu, 0xFFFFFFFFu,
                SWEEP_TEXT_WEB_COMPAT));
    check_eq_i("layout: viewport width clamped",
               (int)g_layout.viewport.width_px, SWEEP_LAYOUT_MAX_PX);
    check_eq_i("layout: viewport height clamped",
               (int)g_layout.viewport.height_px, SWEEP_LAYOUT_MAX_PX);
    b = box_for(node_by_id("d"));
    check("layout: oversized auto width not negative",
          b != 0 && b->content_w == SWEEP_LAYOUT_MAX_PX);

    check("layout: invalid zero viewport",
          !setup("<p>x</p>", (const char*)0, 0u, 200u,
                 SWEEP_TEXT_WEB_COMPAT));
    check("layout: invalid style storage",
          setup("<p id=\"p\">x</p>", (const char*)0, 200u, 100u,
                SWEEP_TEXT_WEB_COMPAT) &&
          !sweep_layout_build(&g_dom, g_styles, 1u, &g_viewport,
                              SWEEP_TEXT_WEB_COMPAT, &g_layout));
    check("layout: null arguments",
          !sweep_layout_build((const SWEEP_DOM*)0, g_styles,
                              SWEEP_DOM_NODES_MAX, &g_viewport,
                              SWEEP_TEXT_WEB_COMPAT, &g_layout));
}

/* ------------------------------------------------------------------ */
/* Paint integration                                                  */
/* ------------------------------------------------------------------ */
static void test_paint(void) {
    SWEEP_PAINT_CMD cmds[SWEEP_PAINT_MAX_CMDS];
    size_t count = 0u, i;
    int all_inside = 1;

    check("paint: text page setup",
          setup("<body style=\"margin:0\"><p style=\"margin:0;font-size:10px\">"
                "Hi</p></body>", (const char*)0, 100u, 50u,
                SWEEP_TEXT_WEB_COMPAT));
    check("paint: generation succeeds",
          sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds,
                                  SWEEP_PAINT_MAX_CMDS, &count) && count >= 3u);
    check("paint: clear first", count > 0u &&
          cmds[0].op == (uint8_t)SWEEP_PAINT_CLEAR);
    check("paint: text commands",
          count_paint_op(cmds, count, (uint8_t)SWEEP_PAINT_TEXT) == 2u);
    for (i = 0u; i < count; ++i) {
        int64_t x2 = (int64_t)cmds[i].x + (int64_t)cmds[i].w;
        int64_t y2 = (int64_t)cmds[i].y + (int64_t)cmds[i].h;
        if (cmds[i].x < 0 || cmds[i].y < 0 || x2 > 100 || y2 > 50) {
            all_inside = 0;
        }
    }
    check("paint: all commands inside viewport", all_inside);

    check("paint: background setup",
          setup("<body style=\"margin:0\"><div id=\"d\" style=\"width:100px;"
                "height:50px;background-color:blue;border:2px solid red\">"
                "</div></body>", (const char*)0, 200u, 100u,
                SWEEP_TEXT_WEB_COMPAT));
    check("paint: background generation",
          sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds,
                                  SWEEP_PAINT_MAX_CMDS, &count));
    check("paint: blue background rect emitted",
          count_paint_op(cmds, count, (uint8_t)SWEEP_PAINT_RECT) >= 5u);
    {
        int saw_blue = 0, saw_red = 0;
        for (i = 0u; i < count; ++i) {
            if (cmds[i].op != (uint8_t)SWEEP_PAINT_RECT) continue;
            if (cmds[i].color == 0xFF0000FFu) saw_blue = 1;
            if (cmds[i].color == 0xFFFF0000u) saw_red = 1;
        }
        check("paint: background color", saw_blue);
        check("paint: border color", saw_red);
    }

    check("paint: transparent text setup",
          setup("<body style=\"margin:0\"><p style=\"margin:0;color:transparent\">"
                "Hi</p></body>", (const char*)0, 100u, 50u,
                SWEEP_TEXT_WEB_COMPAT));
    check("paint: transparent text generation",
          sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds,
                                  SWEEP_PAINT_MAX_CMDS, &count));
    check_eq_i("paint: transparent text emits no glyphs",
               (int)count_paint_op(cmds, count, (uint8_t)SWEEP_PAINT_TEXT), 0);
    check_eq_i("paint: transparent text emits only clear", (int)count, 1);

    check("paint: capacity page setup",
          setup("<body style=\"margin:0\"><div style=\"background-color:red;"
                "border:2px solid blue;width:100px;height:40px\"></div></body>",
                (const char*)0, 200u, 100u, SWEEP_TEXT_WEB_COMPAT));
    check("paint: capacity overflow reports failure",
          !sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds, 2u,
                                   &count) && count == 2u);
    check("paint: one-slot clear succeeds then fails on content",
          !sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds, 1u,
                                   &count) && count == 1u);

    check("paint: invalid arguments",
          !sweep_paint_from_layout((const SWEEP_LAYOUT*)0, SWEEP_TEXT_WEB_COMPAT,
                                   cmds, 4u, &count) &&
          !sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds, 0u,
                                   &count) &&
          !sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT,
                                   (SWEEP_PAINT_CMD*)0, 4u, &count) &&
          !sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds, 4u,
                                   (size_t*)0));
    {
        SWEEP_LAYOUT zero;
        memset(&zero, 0, sizeof(zero));
        check("paint: zero viewport rejected",
              !sweep_paint_from_layout(&zero, SWEEP_TEXT_WEB_COMPAT, cmds, 4u,
                                       &count));
    }
}

/* ------------------------------------------------------------------ */
/* SuperUnicode text integration                                      */
/* ------------------------------------------------------------------ */
static void test_superunicode(void) {
    SWEEP_PAINT_CMD cmds[SWEEP_PAINT_MAX_CMDS];
    size_t count = 0u, i;
    int astral_count = 0, surrogate_count = 0;

    check("su: emoji layout (web)",
          setup("<body style=\"margin:0\"><p style=\"margin:0\">\xF0\x9F\x98\x80"
                "</p></body>", (const char*)0, 200u, 100u,
                SWEEP_TEXT_WEB_COMPAT));
    check("su: emoji web paint",
          sweep_paint_from_layout(&g_layout, SWEEP_TEXT_WEB_COMPAT, cmds,
                                  SWEEP_PAINT_MAX_CMDS, &count));
    for (i = 0u; i < count; ++i) {
        if (cmds[i].op != (uint8_t)SWEEP_PAINT_TEXT) continue;
        if (cmds[i].glyph == 0x1F600u) ++astral_count;
        if (cmds[i].glyph >= 0xD800u && cmds[i].glyph <= 0xDFFFu) {
            ++surrogate_count;
        }
    }
    check_eq_i("su: web astral codepoint is one glyph", astral_count, 1);
    check_eq_i("su: web never emits surrogate halves", surrogate_count, 0);

    astral_count = 0;
    surrogate_count = 0;
    check("su: emoji layout (native)",
          setup("<body style=\"margin:0\"><p style=\"margin:0\">\xF0\x9F\x98\x80"
                "</p></body>", (const char*)0, 200u, 100u,
                SWEEP_TEXT_NATIVE_SU));
    check("su: emoji native paint",
          sweep_paint_from_layout(&g_layout, SWEEP_TEXT_NATIVE_SU, cmds,
                                  SWEEP_PAINT_MAX_CMDS, &count));
    for (i = 0u; i < count; ++i) {
        if (cmds[i].op != (uint8_t)SWEEP_PAINT_TEXT) continue;
        if (cmds[i].glyph == 0x1F600u) ++astral_count;
        if (cmds[i].glyph >= 0xD800u && cmds[i].glyph <= 0xDFFFu) {
            ++surrogate_count;
        }
    }
    check_eq_i("su: native preserves astral codepoint", astral_count, 1);
    check_eq_i("su: native never surrogate-pairs", surrogate_count, 0);
}

/* ---- Phase 5: stylesheet composition, dependency index, filtering ---- */

static void test_stylesheet_append_and_order(void) {
    SWEEP_CSS_STYLESHEET a, b, c;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&c, 0, sizeof(c));
    check("p5: parse sheet a",
          sweep_css_parse_stylesheet("p{color:red;}", strlen("p{color:red;}"), &a));
    check("p5: parse sheet b",
          sweep_css_parse_stylesheet("p{color:green;}", strlen("p{color:green;}"), &b));
    check("p5: append b onto a", sweep_css_stylesheet_append(&a, &b));
    check_eq_i("p5: appended rule count", (int)a.rule_count, 2);
    /* Later rule in source order wins at equal specificity. */
    check("p5: cascade order setup",
          setup("<p id='x'>t</p>", (const char*)0, 200u, 100u, SWEEP_TEXT_WEB_COMPAT));
    {
        uint32_t n = sweep_css_compute_all(&g_dom, &a, g_styles, SWEEP_DOM_NODES_MAX, &g_stats);
        uint32_t id = node_by_id("x");
        check("p5: append order cascade", n > 0u && id != 0u && g_styles[id - 1u].color == 0xFF008000u);
    }
    /* Truncation: fill to max then append. */
    memset(&c, 0, sizeof(c));
    c.rule_count = SWEEP_CSS_MAX_RULES;
    check("p5: append into full sheet truncates",
          sweep_css_stylesheet_append(&c, &b) && c.truncated);
    check_eq_i("p5: full sheet keeps max", (int)c.rule_count, (int)SWEEP_CSS_MAX_RULES);
}

static bool dep_affects(SWEEP_CSS_MUTATION_KIND kind, const char* attr,
                        bool cls, bool id_attr, const char* css) {
    SWEEP_CSS_STYLESHEET sh; SWEEP_CSS_DEP_INDEX idx;
    memset(&sh, 0, sizeof(sh));
    if (css && css[0]) (void) sweep_css_parse_stylesheet(css, strlen(css), &sh);
    sweep_css_dep_index_build(&sh, &idx);
    return sweep_css_dep_mutation_affects_style(&idx, kind, attr, cls, id_attr);
}

static void test_dependency_index(void) {
    SWEEP_CSS_STYLESHEET sh; SWEEP_CSS_DEP_INDEX idx;

    /* Type dependency */
    memset(&sh, 0, sizeof(sh));
    (void) sweep_css_parse_stylesheet("div{color:red;}", 16u, &sh);
    sweep_css_dep_index_build(&sh, &idx);
    check("p5: dep has type", idx.has_type && !idx.has_class && !idx.has_id && !idx.has_structural);
    check("p5: dep type set contains div", idx.types.count >= 1u);

    /* Class dependency */
    memset(&sh, 0, sizeof(sh));
    (void) sweep_css_parse_stylesheet(".box{color:red;}", 16u, &sh);
    sweep_css_dep_index_build(&sh, &idx);
    check("p5: dep has class", idx.has_class && idx.classes.count >= 1u);
    check("p5: dep class affects class mutation",
          dep_affects(SWEEP_CSS_MUT_ATTR, "class", true, false, ".box{color:red;}"));
    check("p5: dep class irrelevant attr unaffected (no structural)",
          !dep_affects(SWEEP_CSS_MUT_ATTR, "title", false, false, ".box{color:red;}"));

    /* ID dependency */
    memset(&sh, 0, sizeof(sh));
    (void) sweep_css_parse_stylesheet("#main{color:red;}", 17u, &sh);
    sweep_css_dep_index_build(&sh, &idx);
    check("p5: dep has id", idx.has_id && idx.ids.count >= 1u);
    check("p5: dep id affects id mutation",
          dep_affects(SWEEP_CSS_MUT_ATTR, "id", false, true, "#main{color:red;}"));

    /* Attribute dependencies */
    check("p5: dep attr exists",
          dep_affects(SWEEP_CSS_MUT_ATTR, "data-x", false, false, "[data-x]{color:red;}"));
    check("p5: dep attr equals",
          dep_affects(SWEEP_CSS_MUT_ATTR, "type", false, false, "[type=\"text\"]{color:red;}"));
    check("p5: dep unrelated attr safe",
          !dep_affects(SWEEP_CSS_MUT_ATTR, "title", false, false, "[data-x]{color:red;}"));

    /* Structural / descendant / child */
    memset(&sh, 0, sizeof(sh));
    (void) sweep_css_parse_stylesheet("div p{color:red;}", 17u, &sh);
    sweep_css_dep_index_build(&sh, &idx);
    check("p5: dep descendant structural", idx.has_descendant && idx.has_structural);
    check("p5: dep descendant conservative on attr",
          dep_affects(SWEEP_CSS_MUT_ATTR, "class", true, false, "div p{color:red;}"));
    memset(&sh, 0, sizeof(sh));
    (void) sweep_css_parse_stylesheet("div > p{color:red;}", 19u, &sh);
    sweep_css_dep_index_build(&sh, &idx);
    check("p5: dep child structural", idx.has_child && idx.has_structural);

    /* Universal */
    check("p5: dep universal conservative",
          dep_affects(SWEEP_CSS_MUT_ATTR, "title", false, false, "*{color:red;}"));

    /* Selector list */
    memset(&sh, 0, sizeof(sh));
    (void) sweep_css_parse_stylesheet("h1, .x, #y{color:red;}", 23u, &sh);
    sweep_css_dep_index_build(&sh, &idx);
    check("p5: dep selector list collects all",
          idx.has_type && idx.has_class && idx.has_id);

    /* Text mutations never affect selectors. */
    check("p5: dep text mutation irrelevant",
          !dep_affects(SWEEP_CSS_MUT_TEXT, (const char*)0, false, false, "p{color:red;}"));

    /* Stylesheet/structural mutations conservative when rules exist. */
    check("p5: dep stylesheet mutation affects",
          dep_affects(SWEEP_CSS_MUT_STYLESHEET, (const char*)0, false, false, "p{color:red;}"));
    check("p5: dep insert mutation affects when rules exist",
          dep_affects(SWEEP_CSS_MUT_INSERT, (const char*)0, false, false, "p{color:red;}"));
    check("p5: dep insert irrelevant on empty sheet",
          !dep_affects(SWEEP_CSS_MUT_INSERT, (const char*)0, false, false, ""));
}

static void test_indexed_filtering_correctness(void) {
    SWEEP_CSS_STYLESHEET sh; SWEEP_CSS_DEP_INDEX idx;
    SWEEP_CSS_STATS ref_st, idx_st;
    SWEEP_COMPUTED_STYLE ref_styles[SWEEP_DOM_NODES_MAX];
    SWEEP_COMPUTED_STYLE idx_styles[SWEEP_DOM_NODES_MAX];
    static const char* html =
        "<div id='a' class='x'><p id='b'>t</p></div>"
        "<span id='c' class='y'>s</span>";
    uint32_t nref, nidx, i;
    int same = 1;

    /* Mix of type, class, id, attr, descendant selectors. */
    static const char* css =
        "div{color:red;} .x{background-color:blue;} #b{font-weight:700;}"
        "[data-z]{color:green;} div p{padding-top:5px;} span.y{margin-top:3px;}";

    check("p5: filter setup parse",
          sweep_dom_parse(&g_dom, html, strlen(html)) != 0u &&
          sweep_css_parse_stylesheet(css, strlen(css), &sh));
    sweep_css_dep_index_build(&sh, &idx);

    nref = sweep_css_compute_all(&g_dom, &sh, ref_styles, SWEEP_DOM_NODES_MAX, &ref_st);
    nidx = sweep_css_compute_all_indexed(&g_dom, &sh, &idx, idx_styles,
                                         SWEEP_DOM_NODES_MAX, &idx_st);
    check_eq_i("p5: filtered node count equals reference", (int)nidx, (int)nref);
    for (i = 0u; i < nref && same; ++i) {
        if (memcmp(&ref_styles[i], &idx_styles[i], sizeof(ref_styles[i])) != 0) same = 0;
    }
    check("p5: filtered computed styles identical to full reference", same);

    /* Candidate filtering must reduce selector evaluations: rules that depend
     * only on other tags/classes/ids are skipped for non-matching elements. */
    check("p5: selector tests counted",
          idx_st.selector_tests > 0u && ref_st.selector_tests >= idx_st.selector_tests);
    check("p5: rules skipped by candidate filtering",
          idx_st.rules_skipped > 0u);

    /* A reference-equivalent run with no index must test every (node,selector). */
    check("p5: full reference performs maximal tests",
          ref_st.selector_tests >= idx_st.selector_tests);

    /* Structural-heavy sheet: conservative (no unsafe skipping). */
    memset(&sh, 0, sizeof(sh));
    (void) sweep_css_parse_stylesheet("div span{color:red;} body > div{color:blue;}",
                                      54u, &sh);
    sweep_css_dep_index_build(&sh, &idx);
    nidx = sweep_css_compute_all_indexed(&g_dom, &sh, &idx, idx_styles,
                                         SWEEP_DOM_NODES_MAX, &idx_st);
    nref = sweep_css_compute_all(&g_dom, &sh, ref_styles, SWEEP_DOM_NODES_MAX, &ref_st);
    same = 1;
    for (i = 0u; i < nref && same; ++i) {
        if (memcmp(&ref_styles[i], &idx_styles[i], sizeof(ref_styles[i])) != 0) same = 0;
    }
    check("p5: structural sheet still correct vs reference", same && nidx == nref);
}

int main(void) {
    printf("Sweep CSS engine host tests\n");
    printf("===========================\n");
    test_tokenizer();
    test_declarations();
    test_selectors();
    test_cascade();
    test_lengths();
    test_layout();
    test_paint();
    test_superunicode();
    test_stylesheet_append_and_order();
    test_dependency_index();
    test_indexed_filtering_correctness();
    printf("---------------------------\n");
    printf("passed: %d, failed: %d\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
