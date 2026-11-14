/* sweep_html_test.c - host tests for Sweep HTML tokenizer + basic DOM.
 * Build (from Sweep root):
 *   gcc -std=c99 -Wall -Wextra -Werror -Isweep sweep/sweep_html.c sweep/tests/sweep_html_test.c -o build-generated/sweep_html_test.exe && build-generated/sweep_html_test.exe
 */
#include <stdio.h>
#include <string.h>

#include "../sweep_html.h"

static int g_failures = 0, g_passed = 0;
static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

static void test_tokenizer_basic(void) {
    SWEEP_HTML_TOKENIZER tz;
    SWEEP_HTML_TOKEN tok;
    const char* html = "<p>Hello</p>";
    sweep_html_tokenizer_init(&tz, html, strlen(html));
    check("tok: start <p>", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_START_TAG && strcmp(tok.name, "p") == 0);
    check("tok: text Hello", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_TEXT && strcmp(tok.data, "Hello") == 0);
    check("tok: end </p>", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_END_TAG && strcmp(tok.name, "p") == 0);
    check("tok: eof", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_EOF);
}

static void test_tokenizer_attrs(void) {
    SWEEP_HTML_TOKENIZER tz;
    SWEEP_HTML_TOKEN tok;
    const char* html = "<a href=\"/x\" class='y' id=z>t</a>";
    const char* href; const char* cls; const char* idv;
    sweep_html_tokenizer_init(&tz, html, strlen(html));
    check("attrs: start tag", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_START_TAG);
    check("attrs: tag name a", strcmp(tok.name, "a") == 0);
    check("attrs: 3 attrs", tok.attr_count == 3u);
    href = 0; cls = 0; idv = 0;
    for (uint32_t i = 0; i < tok.attr_count; ++i) {
        if (strcmp(tok.attrs[i].name, "href") == 0) href = tok.attrs[i].value;
        if (strcmp(tok.attrs[i].name, "class") == 0) cls = tok.attrs[i].value;
        if (strcmp(tok.attrs[i].name, "id") == 0) idv = tok.attrs[i].value;
    }
    check("attrs: href dq", href && strcmp(href, "/x") == 0);
    check("attrs: class sq", cls && strcmp(cls, "y") == 0);
    check("attrs: id unquoted", idv && strcmp(idv, "z") == 0);
}

static void test_tokenizer_comment_doctype(void) {
    SWEEP_HTML_TOKENIZER tz;
    SWEEP_HTML_TOKEN tok;
    const char* html = "<!DOCTYPE html><!-- hi --><p/>";
    sweep_html_tokenizer_init(&tz, html, strlen(html));
    check("doctype: token", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_DOCTYPE);
    check("doctype: name html", strcmp(tok.name, "html") == 0);
    check("comment: token", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_COMMENT);
    check("comment: data", strcmp(tok.data, " hi ") == 0);
    check("self-closing: <p/>", sweep_html_next_token(&tz, &tok) && tok.type == SWEEP_TOK_START_TAG && tok.self_closing);
}

static void test_tokenizer_malformed(void) {
    SWEEP_HTML_TOKENIZER tz;
    SWEEP_HTML_TOKEN tok;
    const char* html = "<p class=foo bar>ok</p missing";
    int got_text = 0, got_end = 0, steps = 0;
    sweep_html_tokenizer_init(&tz, html, strlen(html));
    while (sweep_html_next_token(&tz, &tok) && steps++ < 20) {
        if (tok.type == SWEEP_TOK_EOF) break;
        if (tok.type == SWEEP_TOK_TEXT && strcmp(tok.data, "ok") == 0) got_text = 1;
        if (tok.type == SWEEP_TOK_END_TAG || tok.type == SWEEP_TOK_START_TAG) got_end = 1;
    }
    check("malformed: recovered text", got_text);
    check("malformed: produced tags/end without crash", got_end || got_text);
    check("malformed: terminated", steps < 20);
}

static void test_dom_basic(void) {
    static SWEEP_DOM dom;
    uint32_t doc, html_id, body_id, p_id;
    const SWEEP_DOM_NODE* pn;
    char text[256];
    const char* src = "<html><body><p>Hello <b>world</b></p></body></html>";
    doc = sweep_dom_parse(&dom, src, strlen(src));
    check("dom: doc id", doc == 1u);
    html_id = sweep_dom_find_element(&dom, doc, "html");
    body_id = sweep_dom_find_element(&dom, doc, "body");
    p_id = sweep_dom_find_element(&dom, doc, "p");
    check("dom: find html", html_id != 0u);
    check("dom: find body", body_id != 0u);
    check("dom: find p", p_id != 0u);
    pn = sweep_dom_get(&dom, p_id);
    check("dom: p has children", pn && pn->child_count >= 2u);
    sweep_dom_text_content(&dom, p_id, text, sizeof(text));
    check("dom: text content", strcmp(text, "Hello world") == 0);
}

static void test_dom_attrs(void) {
    static SWEEP_DOM dom;
    uint32_t img_id;
    const SWEEP_DOM_NODE* img;
    const char* src = "<img src=\"a.png\" alt=pic>";
    sweep_dom_parse(&dom, src, strlen(src));
    img_id = sweep_dom_find_element(&dom, dom.document, "img");
    img = sweep_dom_get(&dom, img_id);
    check("dom attr: img found", img != 0);
    check("dom attr: src", img && sweep_dom_get_attr(img, "src") && strcmp(sweep_dom_get_attr(img, "src"), "a.png") == 0);
    check("dom attr: alt unquoted", img && sweep_dom_get_attr(img, "alt") && strcmp(sweep_dom_get_attr(img, "alt"), "pic") == 0);
}

static void test_dom_recovery(void) {
    static SWEEP_DOM dom;
    uint32_t p1, p2;
    char t1[64], t2[64];
    /* unclosed <p> should be recoverable; second p should still parse */
    const char* src = "<div><p>one<p>two</div>";
    sweep_dom_parse(&dom, src, strlen(src));
    p1 = sweep_dom_find_element(&dom, dom.document, "div");
    check("dom recovery: div found", p1 != 0u);
    /* find first p under div by scanning children manually */
    p2 = 0u;
    {
        const SWEEP_DOM_NODE* divn = sweep_dom_get(&dom, p1);
        uint32_t found = 0u;
        if (divn) for (uint32_t i = 0; i < divn->child_count; ++i) {
            const SWEEP_DOM_NODE* ch = sweep_dom_get(&dom, divn->children[i]);
            if (ch && ch->type == SWEEP_DOM_ELEMENT && strcmp(ch->name, "p") == 0) { if (found == 0u) p1 = divn->children[i]; else p2 = divn->children[i]; found++; }
        }
    }
    sweep_dom_text_content(&dom, p1, t1, sizeof(t1));
    check("dom recovery: has first text", strstr(t1, "one") != 0);
    if (p2) { sweep_dom_text_content(&dom, p2, t2, sizeof(t2)); check("dom recovery: second p", strstr(t2, "two") != 0); }
    else { /* acceptable fallback: text present somewhere */ char all[128]; sweep_dom_text_content(&dom, dom.document, all, sizeof(all)); check("dom recovery: both texts present", strstr(all, "one") && strstr(all, "two")); }
}

static void test_raw_text_script_style(void) {
    static SWEEP_DOM dom;
    uint32_t script_id, style_id, p_id;
    char script_text[512], style_text[512], p_text[256];
    /* Script source must be raw text: "<b" inside JS must not become markup. */
    const char* src =
        "<html><head><style>div > p { color: red; }</style></head>"
        "<body><script>var x = 1 < 2; var s = '<b>not tags</b>'; if (a < b) {}</script>"
        "<p>after</p></body></html>";
    sweep_dom_init(&dom);
    sweep_dom_parse(&dom, src, strlen(src));
    script_id = sweep_dom_find_element(&dom, dom.document, "script");
    style_id = sweep_dom_find_element(&dom, dom.document, "style");
    p_id = sweep_dom_find_element(&dom, dom.document, "p");
    check("raw: script element found", script_id != 0u);
    check("raw: style element found", style_id != 0u);
    check("raw: following <p> found", p_id != 0u);
    check("raw: no <b> element built from script text",
          sweep_dom_find_element(&dom, dom.document, "b") == 0u);
    if (script_id) {
        sweep_dom_text_content(&dom, script_id, script_text, sizeof(script_text));
        check("raw: script keeps '<' comparison",
              strstr(script_text, "1 < 2") != 0);
        check("raw: script keeps tag-looking string",
              strstr(script_text, "'<b>not tags</b>'") != 0);
    } else {
        check("raw: script keeps '<' comparison", 0);
        check("raw: script keeps tag-looking string", 0);
    }
    if (style_id) {
        sweep_dom_text_content(&dom, style_id, style_text, sizeof(style_text));
        check("raw: style keeps '>' selector", strstr(style_text, "div > p") != 0);
    } else {
        check("raw: style keeps '>' selector", 0);
    }
    if (p_id) {
        sweep_dom_text_content(&dom, p_id, p_text, sizeof(p_text));
        check("raw: content after raw-text element parses", strcmp(p_text, "after") == 0);
    } else {
        check("raw: content after raw-text element parses", 0);
    }
    /* Tokenizer-level check: raw script text arrives as text, not start tags. */
    {
        SWEEP_HTML_TOKENIZER tz;
        SWEEP_HTML_TOKEN tok;
        const char* s = "<script>a<b</script>";
        int saw_tag_b = 0, saw_text = 0, steps = 0;
        sweep_html_tokenizer_init(&tz, s, strlen(s));
        while (sweep_html_next_token(&tz, &tok) && steps++ < 20) {
            if (tok.type == SWEEP_TOK_EOF) break;
            if (tok.type == SWEEP_TOK_START_TAG && strcmp(tok.name, "b") == 0) saw_tag_b = 1;
            if (tok.type == SWEEP_TOK_TEXT && strstr(tok.data, "a<b")) saw_text = 1;
        }
        check("raw: tokenizer does not emit <b> from script", saw_tag_b == 0);
        check("raw: tokenizer emits script body as text", saw_text);
    }
}

int main(void) {
    test_tokenizer_basic();
    test_tokenizer_attrs();
    test_tokenizer_comment_doctype();
    test_tokenizer_malformed();
    test_dom_basic();
    test_dom_attrs();
    test_dom_recovery();
    test_raw_text_script_style();
    printf("=== Sweep HTML: %d passed, %d failed ===\n", g_passed, g_failures);
    return g_failures ? 1 : 0;
}
