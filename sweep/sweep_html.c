/* sweep_html.c - HTML tokenizer + basic DOM builder. Foundational subset. */
#include "sweep_html.h"

#include <string.h>
#include <ctype.h>

static void s_copy(char* d, size_t c, const char* s, size_t n) {
    if (!d || c == 0u) return;
    if (!s) { d[0] = '\0'; return; }
    if (n >= c) n = c - 1u;
    memcpy(d, s, n); d[n] = '\0';
}
static void s_copyz(char* d, size_t c, const char* s) { s_copy(d, c, s, s ? strlen(s) : 0u); }
/* lowercasing done inline at parse/token points; keep helper local if needed */
static bool name_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false; ++a; ++b; }
    return *a == *b;
}

static void tok_reset(SWEEP_HTML_TOKEN* t) { memset(t, 0, sizeof(*t)); }
static void tok_emit_text(SWEEP_HTML_TOKENIZER* tz, SWEEP_HTML_TOKEN* out) {
    tok_reset(out); out->type = SWEEP_TOK_TEXT;
    s_copy(out->data, sizeof(out->data), tz->text_buf, tz->text_len);
    tz->text_len = 0u; tz->has_text = false;
}
static void text_push(SWEEP_HTML_TOKENIZER* tz, char c) {
    if (tz->text_len + 1u < sizeof(tz->text_buf)) { tz->text_buf[tz->text_len++] = c; tz->text_buf[tz->text_len] = '\0'; }
    tz->has_text = true;
}
static bool name_push(char* buf, size_t cap, char c) {
    size_t n = strlen(buf);
    if (n + 2u > cap) return false;
    buf[n] = c; buf[n + 1u] = '\0'; return true;
}

void sweep_html_tokenizer_init(SWEEP_HTML_TOKENIZER* tz, const char* html, size_t len) {
    if (!tz) return;
    memset(tz, 0, sizeof(*tz));
    tz->input = html ? html : ""; tz->len = html ? len : 0u;
    tz->state = SWEEP_HTML_DATA;
}

static bool next_char(SWEEP_HTML_TOKENIZER* tz, char* c) {
    if (tz->reconsume) { tz->reconsume = false; *c = tz->re_char; return true; }
    if (tz->pos >= tz->len) return false;
    *c = tz->input[tz->pos++]; return true;
}
static void reconsume(SWEEP_HTML_TOKENIZER* tz, char c) { tz->reconsume = true; tz->re_char = c; }

static bool is_void_tag(const char* n) {
    return name_eq(n,"area")||name_eq(n,"base")||name_eq(n,"br")||name_eq(n,"col")||name_eq(n,"embed")||
           name_eq(n,"hr")||name_eq(n,"img")||name_eq(n,"input")||name_eq(n,"link")||name_eq(n,"meta")||
           name_eq(n,"param")||name_eq(n,"source")||name_eq(n,"track")||name_eq(n,"wbr");
}
static bool is_raw_text_tag(const char* n) {
    return name_eq(n, "script") || name_eq(n, "style");
}
static bool is_ws(char c) {
    return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f';
}
static void raw_flush_candidate(SWEEP_HTML_TOKENIZER* tz) {
    size_t i;
    for (i = 0u; i < tz->raw_candidate_len; ++i) text_push(tz, tz->raw_candidate[i]);
    tz->raw_candidate_len = 0u;
}
static bool raw_candidate_prefix(const SWEEP_HTML_TOKENIZER* tz) {
    size_t i, tag_len = strlen(tz->raw_tag);
    for (i = 0u; i < tz->raw_candidate_len; ++i) {
        char expected;
        if (i >= 2u + tag_len) return false;
        expected = (i == 0u) ? '<' : (i == 1u) ? '/' : tz->raw_tag[i - 2u];
        if (tolower((unsigned char) tz->raw_candidate[i]) !=
            tolower((unsigned char) expected)) return false;
    }
    return true;
}
static void emit_start_tag(SWEEP_HTML_TOKENIZER* tz, SWEEP_HTML_TOKEN* out) {
    *out = tz->cur;
    if (!tz->cur.self_closing && is_raw_text_tag(tz->cur.name)) {
        s_copyz(tz->raw_tag, sizeof(tz->raw_tag), tz->cur.name);
        tz->raw_candidate_len = 0u;
        tz->raw_close_done = false;
        tz->state = SWEEP_HTML_RAW_TEXT;
    } else {
        tz->state = SWEEP_HTML_DATA;
    }
}

bool sweep_html_next_token(SWEEP_HTML_TOKENIZER* tz, SWEEP_HTML_TOKEN* out) {
    char c;
    if (!tz || !out) return false;
    if (tz->raw_close_done) {
        tz->raw_close_done = false;
        tok_reset(out);
        out->type = SWEEP_TOK_END_TAG;
        s_copyz(out->name, sizeof(out->name), tz->raw_tag);
        return true;
    }
    for (;;) {
        bool have = next_char(tz, &c);
        switch (tz->state) {
        case SWEEP_HTML_DATA:
            if (!have) { if (tz->has_text) { tok_emit_text(tz, out); return true; } tok_reset(out); out->type = SWEEP_TOK_EOF; return true; }
            if (c == '<') { if (tz->has_text) { tok_emit_text(tz, out); reconsume(tz,c); return true; } tz->state = SWEEP_HTML_TAG_OPEN; }
            else text_push(tz, c);
            break;
        case SWEEP_HTML_TAG_OPEN:
            if (!have) { text_push(tz,'<'); tok_emit_text(tz,out); return true; }
            if (c == '/') { tz->state = SWEEP_HTML_END_TAG_OPEN; }
            else if (c == '!') { tz->state = SWEEP_HTML_MARKUP_DECL; }
            else if (isalpha((unsigned char)c)) { tok_reset(&tz->cur); tz->cur.type = SWEEP_TOK_START_TAG; name_push(tz->cur.name, sizeof(tz->cur.name), (char)tolower((unsigned char)c)); tz->state = SWEEP_HTML_TAG_NAME; }
            else if (c == '?') { tz->state = SWEEP_HTML_BOGUS_COMMENT; }
            else { text_push(tz,'<'); reconsume(tz,c); tz->state = SWEEP_HTML_DATA; }
            break;
        case SWEEP_HTML_END_TAG_OPEN:
            if (!have) { text_push(tz,'<'); text_push(tz,'/'); tok_emit_text(tz,out); return true; }
            if (isalpha((unsigned char)c)) { tok_reset(&tz->cur); tz->cur.type = SWEEP_TOK_END_TAG; name_push(tz->cur.name, sizeof(tz->cur.name), (char)tolower((unsigned char)c)); tz->state = SWEEP_HTML_TAG_NAME; }
            else { tz->state = SWEEP_HTML_BOGUS_COMMENT; }
            break;
        case SWEEP_HTML_TAG_NAME:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { tz->state = SWEEP_HTML_BEFORE_ATTR_NAME; }
            else if (c == '/') { tz->state = SWEEP_HTML_SELF_CLOSING; }
            else if (c == '>') { tz->cur.self_closing = tz->cur.self_closing || is_void_tag(tz->cur.name); emit_start_tag(tz, out); return true; }
            else { name_push(tz->cur.name, sizeof(tz->cur.name), (char)tolower((unsigned char)c)); }
            break;
        case SWEEP_HTML_BEFORE_ATTR_NAME:
            if (!have || c == '/' || c == '>') { reconsume(tz, c); tz->state = SWEEP_HTML_AFTER_ATTR_NAME; if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; } }
            else if (!(c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f')) {
                if (tz->cur.attr_count < SWEEP_HTML_ATTR_MAX) { memset(&tz->cur.attrs[tz->cur.attr_count], 0, sizeof(tz->cur.attrs[0])); name_push(tz->cur.attrs[tz->cur.attr_count].name, sizeof(tz->cur.attrs[0].name), (char)tolower((unsigned char)c)); tz->cur.attr_count++; }
                tz->state = SWEEP_HTML_ATTR_NAME;
            }
            break;
        case SWEEP_HTML_ATTR_NAME:
            if (!have || c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='/'||c=='>'||c=='=') {
                reconsume(tz,c); tz->state = SWEEP_HTML_AFTER_ATTR_NAME; if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            } else if (tz->cur.attr_count > 0u) name_push(tz->cur.attrs[tz->cur.attr_count-1u].name, sizeof(tz->cur.attrs[0].name), (char)tolower((unsigned char)c));
            break;
        case SWEEP_HTML_AFTER_ATTR_NAME:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f') { }
            else if (c=='/') { tz->state = SWEEP_HTML_SELF_CLOSING; }
            else if (c=='=') { tz->state = SWEEP_HTML_BEFORE_ATTR_VALUE; }
            else if (c=='>') { tz->cur.self_closing = tz->cur.self_closing || is_void_tag(tz->cur.name); emit_start_tag(tz, out); return true; }
            else { if (tz->cur.attr_count < SWEEP_HTML_ATTR_MAX) { memset(&tz->cur.attrs[tz->cur.attr_count],0,sizeof(tz->cur.attrs[0])); name_push(tz->cur.attrs[tz->cur.attr_count].name,sizeof(tz->cur.attrs[0].name),(char)tolower((unsigned char)c)); tz->cur.attr_count++; } tz->state = SWEEP_HTML_ATTR_NAME; }
            break;
        case SWEEP_HTML_BEFORE_ATTR_VALUE:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f') { }
            else if (c=='"') { tz->state = SWEEP_HTML_ATTR_VALUE_DQ; }
            else if (c=='\'') { tz->state = SWEEP_HTML_ATTR_VALUE_SQ; }
            else if (c=='>') { *out = tz->cur; emit_start_tag(tz, out); return true; }
            else { reconsume(tz,c); tz->state = SWEEP_HTML_ATTR_VALUE_UQ; }
            break;
        case SWEEP_HTML_ATTR_VALUE_DQ:
        case SWEEP_HTML_ATTR_VALUE_SQ:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if ((tz->state==SWEEP_HTML_ATTR_VALUE_DQ && c=='"') || (tz->state==SWEEP_HTML_ATTR_VALUE_SQ && c=='\'')) { tz->state = SWEEP_HTML_AFTER_ATTR_VALUE; }
            else if (tz->cur.attr_count>0u) name_push(tz->cur.attrs[tz->cur.attr_count-1u].value,sizeof(tz->cur.attrs[0].value),c);
            break;
        case SWEEP_HTML_ATTR_VALUE_UQ:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f') { tz->state = SWEEP_HTML_BEFORE_ATTR_NAME; }
            else if (c=='>') { *out = tz->cur; emit_start_tag(tz, out); return true; }
            else if (tz->cur.attr_count>0u) name_push(tz->cur.attrs[tz->cur.attr_count-1u].value,sizeof(tz->cur.attrs[0].value),c);
            break;
        case SWEEP_HTML_AFTER_ATTR_VALUE:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f') tz->state = SWEEP_HTML_BEFORE_ATTR_NAME;
            else if (c=='/') tz->state = SWEEP_HTML_SELF_CLOSING;
            else if (c=='>') { tz->cur.self_closing = tz->cur.self_closing || is_void_tag(tz->cur.name); emit_start_tag(tz, out); return true; }
            else { reconsume(tz,c); tz->state = SWEEP_HTML_BEFORE_ATTR_NAME; }
            break;
        case SWEEP_HTML_SELF_CLOSING:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c=='>') { tz->cur.self_closing = true; emit_start_tag(tz, out); return true; }
            else { reconsume(tz,c); tz->state = SWEEP_HTML_BEFORE_ATTR_NAME; }
            break;
        case SWEEP_HTML_MARKUP_DECL:
            /* very small subset: <!-- ... --> and <!DOCTYPE ...> */
            if (!have) { tok_reset(out); out->type = SWEEP_TOK_EOF; return true; }
            if (c == '-' && tz->pos < tz->len && tz->input[tz->pos] == '-') {
                tz->pos++; /* consume second '-' */
                tok_reset(&tz->cur); tz->cur.type = SWEEP_TOK_COMMENT; tz->cur.data[0] = '\0';
                tz->state = SWEEP_HTML_COMMENT;
            } else {
                char look[8]; size_t li = 0;
                look[li++] = (char)tolower((unsigned char)c);
                while (li + 1u < sizeof(look) && tz->pos < tz->len && isalpha((unsigned char)tz->input[tz->pos])) {
                    look[li++] = (char)tolower((unsigned char)tz->input[tz->pos++]);
                }
                look[li] = '\0';
                if (strncmp(look, "doctype", 7) == 0) {
                    tok_reset(&tz->cur); tz->cur.type = SWEEP_TOK_DOCTYPE; tz->state = SWEEP_HTML_DOCTYPE;
                } else {
                    tz->state = SWEEP_HTML_BOGUS_COMMENT;
                }
            }
            break;
        case SWEEP_HTML_COMMENT:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c=='-') tz->state = SWEEP_HTML_COMMENT_DASH; else name_push(tz->cur.data, sizeof(tz->cur.data), c);
            break;
        case SWEEP_HTML_COMMENT_DASH:
            if (!have) { name_push(tz->cur.data,sizeof(tz->cur.data),'-'); *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c=='-') tz->state = SWEEP_HTML_COMMENT_END; else { name_push(tz->cur.data,sizeof(tz->cur.data),'-'); name_push(tz->cur.data,sizeof(tz->cur.data),c); tz->state = SWEEP_HTML_COMMENT; }
            break;
        case SWEEP_HTML_COMMENT_END:
            if (!have) { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (c=='>') { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            else { name_push(tz->cur.data,sizeof(tz->cur.data),'-'); name_push(tz->cur.data,sizeof(tz->cur.data),'-'); reconsume(tz,c); tz->state = SWEEP_HTML_COMMENT; }
            break;
        case SWEEP_HTML_DOCTYPE:
            if (!have || c=='>') { *out = tz->cur; tz->state = SWEEP_HTML_DATA; return true; }
            if (!(c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f') && tz->cur.name[0]=='\0') name_push(tz->cur.name,sizeof(tz->cur.name),(char)tolower((unsigned char)c));
            else if (isalpha((unsigned char)c) && tz->cur.name[0]!='\0' && strlen(tz->cur.name)<7) name_push(tz->cur.name,sizeof(tz->cur.name),(char)tolower((unsigned char)c));
            break;
        case SWEEP_HTML_BOGUS_COMMENT:
            if (!have || c=='>') { tz->state = SWEEP_HTML_DATA; if (!have) { tok_reset(out); out->type = SWEEP_TOK_EOF; return true; } }
            break;
        case SWEEP_HTML_RAW_TEXT:
            if (!have) {
                raw_flush_candidate(tz);
                tz->state = SWEEP_HTML_DATA;
                if (tz->has_text) { tok_emit_text(tz, out); return true; }
                tok_reset(out); out->type = SWEEP_TOK_EOF; return true;
            }
            if (tz->raw_candidate_len == 0u) {
                if (c == '<') {
                    tz->raw_candidate[0] = '<';
                    tz->raw_candidate_len = 1u;
                } else {
                    text_push(tz, c);
                }
            } else {
                if (tz->raw_candidate_len + 1u < sizeof(tz->raw_candidate)) {
                    tz->raw_candidate[tz->raw_candidate_len++] = c;
                    if (!raw_candidate_prefix(tz)) {
                        /* The last character made the candidate invalid. Flush
                         * the preceding bytes, then re-examine this character. */
                        if (tz->raw_candidate_len > 0u) tz->raw_candidate_len--;
                        raw_flush_candidate(tz);
                        if (c == '<') {
                            tz->raw_candidate[0] = '<';
                            tz->raw_candidate_len = 1u;
                        } else {
                            text_push(tz, c);
                        }
                    } else if (tz->raw_candidate_len ==
                               2u + strlen(tz->raw_tag)) {
                        tz->state = SWEEP_HTML_RAW_CLOSE;
                    }
                } else {
                    raw_flush_candidate(tz);
                    text_push(tz, c);
                }
            }
            break;
        case SWEEP_HTML_RAW_CLOSE:
            if (!have) {
                raw_flush_candidate(tz);
                tz->state = SWEEP_HTML_DATA;
                if (tz->has_text) { tok_emit_text(tz, out); return true; }
                tok_reset(out); out->type = SWEEP_TOK_EOF; return true;
            }
            if (c == '>') {
                tz->raw_candidate_len = 0u;
                tz->state = SWEEP_HTML_DATA;
                if (tz->has_text) {
                    tz->raw_close_done = true;
                    tok_emit_text(tz, out);
                    return true;
                }
                tok_reset(out);
                out->type = SWEEP_TOK_END_TAG;
                s_copyz(out->name, sizeof(out->name), tz->raw_tag);
                return true;
            }
            if (is_ws(c)) break;
            /* Not a real end tag: emit the candidate as ordinary text and
             * resume raw-text scanning. */
            raw_flush_candidate(tz);
            tz->state = SWEEP_HTML_RAW_TEXT;
            if (c == '<') {
                tz->raw_candidate[0] = '<';
                tz->raw_candidate_len = 1u;
            } else {
                text_push(tz, c);
            }
            break;
        default: tz->state = SWEEP_HTML_DATA; break;
        }
    }
}

/* --- DOM --- */
void sweep_dom_init(SWEEP_DOM* dom) {
    if (!dom) return;
    memset(dom, 0, sizeof(*dom));
    dom->nodes[0].in_use = true; dom->nodes[0].type = SWEEP_DOM_DOCUMENT;
    dom->nodes[0].generation = 1u;
    dom->count = 1u; dom->document = 1u;
}
static uint32_t dom_new(SWEEP_DOM* dom, SWEEP_DOM_NODE_TYPE type) {
    uint32_t i;
    for (i = 0u; i < SWEEP_DOM_NODES_MAX; ++i) if (!dom->nodes[i].in_use) {
        uint32_t prev_gen = dom->nodes[i].generation;
        memset(&dom->nodes[i], 0, sizeof(dom->nodes[i]));
        dom->nodes[i].in_use = true; dom->nodes[i].type = type;
        /* A reused slot gets a fresh generation so stale wrappers (which
         * captured the old generation) are rejected, never followed. */
        dom->nodes[i].generation = prev_gen + 1u;
        dom->count++; return i+1u;
    }
    return 0u;
}
static void dom_append(SWEEP_DOM* dom, uint32_t parent, uint32_t child) {
    SWEEP_DOM_NODE* p; SWEEP_DOM_NODE* c;
    if (!dom || !parent || !child) return;
    p = &dom->nodes[parent-1u]; c = &dom->nodes[child-1u];
    if (p->child_count < SWEEP_DOM_CHILDREN_MAX) p->children[p->child_count++] = child;
    c->parent = parent;
}
const SWEEP_DOM_NODE* sweep_dom_get(const SWEEP_DOM* dom, uint32_t id) {
    if (!dom || !id || id > SWEEP_DOM_NODES_MAX || !dom->nodes[id-1u].in_use) return 0;
    return &dom->nodes[id-1u];
}
static void dom_build_into(SWEEP_DOM* dom, uint32_t root, const char* html,
                           size_t len) {
    SWEEP_HTML_TOKENIZER tz;
    SWEEP_HTML_TOKEN tok;
    uint32_t stack[64];
    uint32_t sp = 0u;
    uint32_t cur = root;
    sweep_html_tokenizer_init(&tz, html, len);
    while (sweep_html_next_token(&tz, &tok)) {
        if (tok.type == SWEEP_TOK_EOF) break;
        if (tok.type == SWEEP_TOK_TEXT) {
            if (tok.data[0] != '\0') {
                uint32_t id = dom_new(dom, SWEEP_DOM_TEXT);
                if (id) {
                    s_copyz(dom->nodes[id-1u].text,
                            sizeof(dom->nodes[id-1u].text), tok.data);
                    dom_append(dom, cur, id);
                }
            }
        } else if (tok.type == SWEEP_TOK_COMMENT) {
            uint32_t id = dom_new(dom, SWEEP_DOM_COMMENT);
            if (id) {
                s_copyz(dom->nodes[id-1u].text,
                        sizeof(dom->nodes[id-1u].text), tok.data);
                dom_append(dom, cur, id);
            }
        } else if (tok.type == SWEEP_TOK_START_TAG) {
            uint32_t id = dom_new(dom, SWEEP_DOM_ELEMENT);
            if (id) {
                uint32_t ai;
                s_copyz(dom->nodes[id-1u].name,
                        sizeof(dom->nodes[id-1u].name), tok.name);
                dom->nodes[id-1u].attr_count = tok.attr_count;
                for (ai = 0u; ai < tok.attr_count && ai < SWEEP_HTML_ATTR_MAX; ++ai) {
                    dom->nodes[id-1u].attrs[ai] = tok.attrs[ai];
                }
                dom_append(dom, cur, id);
                if (!tok.self_closing && !is_void_tag(tok.name)) {
                    if (sp < 64u) stack[sp++] = cur;
                    cur = id;
                }
            }
        } else if (tok.type == SWEEP_TOK_END_TAG) {
            /* deterministic recovery: close current if matching, else find the
             * nearest matching open element on the stack, else ignore. */
            const SWEEP_DOM_NODE* cn = sweep_dom_get(dom, cur);
            if (cn && cn->type == SWEEP_DOM_ELEMENT && name_eq(cn->name, tok.name)) {
                if (sp > 0u) cur = stack[--sp];
            } else {
                uint32_t i = sp;
                while (i > 0u) {
                    i--;
                    cn = sweep_dom_get(dom, stack[i]);
                    if (cn && cn->type == SWEEP_DOM_ELEMENT &&
                        name_eq(cn->name, tok.name)) {
                        sp = i;
                        cur = stack[i];
                        break;
                    }
                }
            }
        }
    }
}
uint32_t sweep_dom_find_element(const SWEEP_DOM* dom, uint32_t root, const char* name) {
    const SWEEP_DOM_NODE* n; uint32_t i, r;
    n = sweep_dom_get(dom, root); if (!n) return 0u;
    if (n->type == SWEEP_DOM_ELEMENT && name_eq(n->name, name)) return root;
    for (i = 0u; i < n->child_count; ++i) { r = sweep_dom_find_element(dom, n->children[i], name); if (r) return r; }
    return 0u;
}
const char* sweep_dom_get_attr(const SWEEP_DOM_NODE* n, const char* name) {
    uint32_t i; if (!n) return 0;
    for (i = 0u; i < n->attr_count; ++i) if (name_eq(n->attrs[i].name, name)) return n->attrs[i].value;
    return 0;
}
static void text_cat(char* out, size_t cap, const char* s) {
    size_t o = strlen(out), n = s ? strlen(s) : 0u;
    if (o + 1u >= cap || n == 0u) return;
    if (n >= cap - o) n = cap - o - 1u;
    memcpy(out + o, s, n); out[o + n] = '\0';
}
static void text_content_append(const SWEEP_DOM* dom, uint32_t id, char* out, size_t cap) {
    const SWEEP_DOM_NODE* n; uint32_t i;
    n = sweep_dom_get(dom, id); if (!n) return;
    if (n->type == SWEEP_DOM_TEXT || n->type == SWEEP_DOM_COMMENT) {
        if (n->type == SWEEP_DOM_TEXT) text_cat(out, cap, n->text);
        return;
    }
    for (i = 0u; i < n->child_count; ++i) text_content_append(dom, n->children[i], out, cap);
}
void sweep_dom_text_content(const SWEEP_DOM* dom, uint32_t id, char* out, size_t cap) {
    if (!out || cap == 0u) return;
    out[0] = '\0';
    text_content_append(dom, id, out, cap);
}

uint32_t sweep_dom_parse(SWEEP_DOM* dom, const char* html, size_t len) {
    if (!dom) return 0u;
    sweep_dom_init(dom);
    dom_build_into(dom, dom->document, html, len);
    return dom->document;
}
static bool dom_container(const SWEEP_DOM_NODE* n) {
    return n && (n->type == SWEEP_DOM_DOCUMENT || n->type == SWEEP_DOM_ELEMENT);
}
static bool node_is_ancestor(const SWEEP_DOM* dom, uint32_t ancestor,
                             uint32_t node) {
    const SWEEP_DOM_NODE* n;
    uint32_t guard = 0u;
    if (!ancestor || !node) return false;
    n = sweep_dom_get(dom, node);
    while (n && n->parent && guard++ < SWEEP_DOM_NODES_MAX) {
        if (n->parent == ancestor) return true;
        n = sweep_dom_get(dom, n->parent);
    }
    return false;
}
static bool dom_detach(SWEEP_DOM* dom, uint32_t child) {
    SWEEP_DOM_NODE* c;
    SWEEP_DOM_NODE* p;
    uint32_t i;
    if (!dom || !child) return false;
    c = &dom->nodes[child - 1u];
    if (!c->in_use || !c->parent) return false;
    p = &dom->nodes[c->parent - 1u];
    for (i = 0u; i < p->child_count; ++i) {
        if (p->children[i] == child) {
            uint32_t j;
            for (j = i + 1u; j < p->child_count; ++j) {
                p->children[j - 1u] = p->children[j];
            }
            p->child_count--;
            c->parent = 0u;
            return true;
        }
    }
    c->parent = 0u;
    return false;
}
static void dom_record_mutation(SWEEP_DOM* dom, uint8_t kind,
                                 const char* attr_name,
                                 bool class_changed, bool id_changed) {
    if (!dom) return;
    dom->mutation_kind = kind;
    dom->mutation_attr[0] = '\0';
    if (attr_name && attr_name[0] != '\0') {
        s_copyz(dom->mutation_attr, sizeof(dom->mutation_attr), attr_name);
    }
    dom->mutation_class = class_changed;
    dom->mutation_id_attr = id_changed;
}
static void dom_mark_dirty(SWEEP_DOM* dom, uint32_t node, uint8_t kind,
                           const char* attr_name, bool class_changed,
                           bool id_changed) {
    if (!dom) return;
    dom->revision++;
    dom->dirty_node = node;
    dom->style_dirty = true;
    dom->layout_dirty = true;
    dom->paint_dirty = true;
    dom_record_mutation(dom, kind, attr_name, class_changed, id_changed);
}
static void dom_mark_dirty_text(SWEEP_DOM* dom, uint32_t node) {
    /* Text bytes cannot change computed styles of remaining nodes (style
     * matching reads tags/attrs/tree, not text).  Skip the style pass;
     * layout (wrapping) and paint (glyphs) still invalidate. */
    if (!dom) return;
    dom->revision++;
    dom->dirty_node = node;
    dom->layout_dirty = true;
    dom->paint_dirty = true;
    dom_record_mutation(dom, SWEEP_DOM_MUT_TEXT, (const char*)0, false, false);
}
/* Free one detached subtree rooted at `root` (must have parent==0 and must
 * not be the document node).  Traversal is bounded by SWEEP_DOM_NODES_MAX so
 * a corrupt parent/child cycle cannot loop forever.  Children are visited
 * only when their parent pointer agrees with the current node. */
static void dom_reclaim_subtree(SWEEP_DOM* dom, uint32_t root, uint32_t depth,
                                uint32_t* budget) {
    SWEEP_DOM_NODE* n;
    uint32_t i, kids[SWEEP_DOM_CHILDREN_MAX], kc;
    if (!dom || !root || root > SWEEP_DOM_NODES_MAX || depth > SWEEP_DOM_NODES_MAX) return;
    if (*budget == 0u) return;
    n = &dom->nodes[root - 1u];
    if (!n->in_use || n->type == SWEEP_DOM_DOCUMENT || n->parent != 0u) return;
    kc = n->child_count;
    if (kc > SWEEP_DOM_CHILDREN_MAX) kc = SWEEP_DOM_CHILDREN_MAX;
    for (i = 0u; i < kc; ++i) kids[i] = n->children[i];
    /* Clear the root first so a corrupt child pointer back to the root is no
     * longer followed; the node becomes a free slot with bumped generation. */
    n->in_use = false; n->child_count = 0u; n->parent = 0u;
    n->generation++;
    if (dom->count > 0u) dom->count--;
    (*budget)--;
    for (i = 0u; i < kc; ++i) {
        uint32_t k = kids[i];
        if (k && k <= SWEEP_DOM_NODES_MAX && dom->nodes[k - 1u].in_use &&
            dom->nodes[k - 1u].parent == root) {
            /* Detach from parent view (parent is already free) and reclaim. */
            dom->nodes[k - 1u].parent = 0u;
            dom_reclaim_subtree(dom, k, depth + 1u, budget);
        }
    }
}
uint32_t sweep_dom_node_generation(const SWEEP_DOM* dom, uint32_t id) {
    if (!dom || !id || id > SWEEP_DOM_NODES_MAX) return 0u;
    if (!dom->nodes[id - 1u].in_use) return 0u;
    return dom->nodes[id - 1u].generation;
}
void sweep_dom_clear_dirty(SWEEP_DOM* dom) {
    if (!dom) return;
    dom->style_dirty = false;
    dom->layout_dirty = false;
    dom->paint_dirty = false;
    dom->dirty_node = 0u;
    dom->mutation_kind = SWEEP_DOM_MUT_NONE;
    dom->mutation_attr[0] = '\0';
    dom->mutation_class = false;
    dom->mutation_id_attr = false;
}

void sweep_dom_mark_style_dirty(SWEEP_DOM* dom) {
    if (!dom) return;
    dom->style_dirty = true;
    dom->layout_dirty = true;
    dom->paint_dirty = true;
    dom_record_mutation(dom, SWEEP_DOM_MUT_STYLE, (const char*)0, false, false);
}
bool sweep_dom_is_dirty(const SWEEP_DOM* dom) {
    return dom && (dom->style_dirty || dom->layout_dirty || dom->paint_dirty);
}
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
uint32_t sweep_dom_create_element(SWEEP_DOM* dom, const char* name) {
    uint32_t id;
    if (!dom || !name || name[0] == '\0') return 0u;
    id = dom_new(dom, SWEEP_DOM_ELEMENT);
    if (!id) return 0u;
    copy_lower(dom->nodes[id - 1u].name,
               sizeof(dom->nodes[id - 1u].name), name);
    return id;
}
uint32_t sweep_dom_create_text(SWEEP_DOM* dom, const char* text) {
    uint32_t id;
    if (!dom) return 0u;
    id = dom_new(dom, SWEEP_DOM_TEXT);
    if (!id) return 0u;
    s_copyz(dom->nodes[id - 1u].text, sizeof(dom->nodes[id - 1u].text),
            text ? text : "");
    return id;
}
uint32_t sweep_dom_create_comment(SWEEP_DOM* dom, const char* text) {
    uint32_t id;
    if (!dom) return 0u;
    id = dom_new(dom, SWEEP_DOM_COMMENT);
    if (!id) return 0u;
    s_copyz(dom->nodes[id - 1u].text, sizeof(dom->nodes[id - 1u].text),
            text ? text : "");
    return id;
}
bool sweep_dom_append_child(SWEEP_DOM* dom, uint32_t parent, uint32_t child) {
    SWEEP_DOM_NODE* p;
    SWEEP_DOM_NODE* c;
    if (!dom || !parent || !child || parent == child) return false;
    if (parent > SWEEP_DOM_NODES_MAX || child > SWEEP_DOM_NODES_MAX) return false;
    p = &dom->nodes[parent - 1u];
    c = &dom->nodes[child - 1u];
    if (!p->in_use || !c->in_use || !dom_container(p)) return false;
    if (node_is_ancestor(dom, child, parent)) return false;
    if (c->parent != parent && p->child_count >= SWEEP_DOM_CHILDREN_MAX) {
        return false;
    }
    if (c->parent) (void) dom_detach(dom, child);
    dom_append(dom, parent, child);
    dom_mark_dirty(dom, parent, SWEEP_DOM_MUT_STRUCTURE, (const char*)0, false, false);
    return true;
}
bool sweep_dom_insert_before(SWEEP_DOM* dom, uint32_t parent, uint32_t child,
                             uint32_t reference) {
    SWEEP_DOM_NODE* p;
    SWEEP_DOM_NODE* c;
    uint32_t i, at;
    bool found = false;
    if (!dom || !parent || !child || parent == child) return false;
    if (parent > SWEEP_DOM_NODES_MAX || child > SWEEP_DOM_NODES_MAX ||
        reference > SWEEP_DOM_NODES_MAX) return false;
    if (reference == 0u) return sweep_dom_append_child(dom, parent, child);
    if (reference == child) return true;
    p = &dom->nodes[parent - 1u];
    c = &dom->nodes[child - 1u];
    if (!p->in_use || !c->in_use || !dom_container(p)) return false;
    if (node_is_ancestor(dom, child, parent)) return false;
    for (i = 0u; i < p->child_count; ++i) {
        if (p->children[i] == reference) { found = true; break; }
    }
    if (!found) return false;
    if (c->parent != parent && p->child_count >= SWEEP_DOM_CHILDREN_MAX) {
        return false;
    }
    if (c->parent) (void) dom_detach(dom, child);
    at = p->child_count;
    for (i = 0u; i < p->child_count; ++i) {
        if (p->children[i] == reference) { at = i; break; }
    }
    if (p->child_count < SWEEP_DOM_CHILDREN_MAX) {
        for (i = p->child_count; i > at; --i) {
            p->children[i] = p->children[i - 1u];
        }
        p->children[at] = child;
        p->child_count++;
        c->parent = parent;
        dom_mark_dirty(dom, parent, SWEEP_DOM_MUT_STRUCTURE, (const char*)0, false, false);
        return true;
    }
    return false;
}
bool sweep_dom_remove_child(SWEEP_DOM* dom, uint32_t child) {
    const SWEEP_DOM_NODE* c;
    uint32_t parent;
    if (!dom || !child) return false;
    c = sweep_dom_get(dom, child);
    if (!c || !c->parent) return false;
    parent = c->parent;
    if (!dom_detach(dom, child)) return false;
    dom_mark_dirty(dom, parent, SWEEP_DOM_MUT_STRUCTURE, (const char*)0, false, false);
    return true;
}
bool sweep_dom_set_attr(SWEEP_DOM* dom, uint32_t id, const char* name,
                        const char* value) {
    SWEEP_DOM_NODE* n;
    char key[SWEEP_HTML_NAME_MAX];
    uint32_t i;
    if (!dom || !id || id > SWEEP_DOM_NODES_MAX || !name || name[0] == '\0') return false;
    n = &dom->nodes[id - 1u];
    if (!n->in_use || n->type != SWEEP_DOM_ELEMENT) return false;
    copy_lower(key, sizeof(key), name);
    for (i = 0u; i < n->attr_count; ++i) {
        if (strcmp(n->attrs[i].name, key) == 0) {
            s_copyz(n->attrs[i].value, sizeof(n->attrs[i].value),
                    value ? value : "");
            dom_mark_dirty(dom, id, SWEEP_DOM_MUT_ATTR, key,
                           strcmp(key, "class") == 0, strcmp(key, "id") == 0);
            return true;
        }
    }
    if (n->attr_count >= SWEEP_HTML_ATTR_MAX) return false;
    s_copyz(n->attrs[n->attr_count].name,
            sizeof(n->attrs[n->attr_count].name), key);
    s_copyz(n->attrs[n->attr_count].value,
            sizeof(n->attrs[n->attr_count].value), value ? value : "");
    n->attr_count++;
    dom_mark_dirty(dom, id, SWEEP_DOM_MUT_ATTR, key,
                   strcmp(key, "class") == 0, strcmp(key, "id") == 0);
    return true;
}
bool sweep_dom_remove_attr(SWEEP_DOM* dom, uint32_t id, const char* name) {
    SWEEP_DOM_NODE* n;
    char key[SWEEP_HTML_NAME_MAX];
    uint32_t i;
    if (!dom || !id || id > SWEEP_DOM_NODES_MAX || !name) return false;
    n = &dom->nodes[id - 1u];
    if (!n->in_use || n->type != SWEEP_DOM_ELEMENT) return false;
    copy_lower(key, sizeof(key), name);
    for (i = 0u; i < n->attr_count; ++i) {
        if (strcmp(n->attrs[i].name, key) == 0) {
            uint32_t j;
            for (j = i + 1u; j < n->attr_count; ++j) {
                n->attrs[j - 1u] = n->attrs[j];
            }
            n->attr_count--;
            dom_mark_dirty(dom, id, SWEEP_DOM_MUT_ATTR, key,
                           strcmp(key, "class") == 0, strcmp(key, "id") == 0);
            return true;
        }
    }
    return false;
}
static bool dom_is_style_context(SWEEP_DOM* dom, uint32_t id) {
    const SWEEP_DOM_NODE* n = sweep_dom_get(dom, id);
    if (!n) return false;
    if (n->type == SWEEP_DOM_ELEMENT && strcmp(n->name, "style") == 0) return true;
    if (n->parent) {
        const SWEEP_DOM_NODE* p = sweep_dom_get(dom, n->parent);
        if (p && p->type == SWEEP_DOM_ELEMENT && strcmp(p->name, "style") == 0) return true;
    }
    return false;
}
bool sweep_dom_set_text_content(SWEEP_DOM* dom, uint32_t id, const char* text) {
    SWEEP_DOM_NODE* n;
    uint32_t child_count, i, reuse = 0u;
    uint32_t old_kids[SWEEP_DOM_CHILDREN_MAX];
    if (!dom || !id || id > SWEEP_DOM_NODES_MAX) return false;
    n = &dom->nodes[id - 1u];
    if (!n->in_use) return false;
    if (n->type == SWEEP_DOM_TEXT || n->type == SWEEP_DOM_COMMENT) {
        s_copyz(n->text, sizeof(n->text), text ? text : "");
        /* Text inside <style> feeds the stylesheet, so it needs a style pass. */
        if (dom_is_style_context(dom, id)) {
            dom_mark_dirty(dom, id, SWEEP_DOM_MUT_STYLE, (const char*)0, false, false);
        } else {
            dom_mark_dirty_text(dom, id);
        }
        return true;
    }
    if (!dom_container(n)) return false;
    /* Reuse an existing leading text node so repeated textContent writes do not
     * consume a fresh DOM slot. */
    if (text && text[0] != '\0' && n->child_count > 0u) {
        const SWEEP_DOM_NODE* first = sweep_dom_get(dom, n->children[0]);
        if (first && first->type == SWEEP_DOM_TEXT) reuse = n->children[0];
    }
    child_count = n->child_count;
    if (child_count > SWEEP_DOM_CHILDREN_MAX) child_count = SWEEP_DOM_CHILDREN_MAX;
    for (i = 0u; i < child_count; ++i) old_kids[i] = n->children[i];
    for (i = 0u; i < child_count; ++i) {
        (void) dom_detach(dom, n->children[0]);
    }
    if (text && text[0] != '\0') {
        if (reuse != 0u && sweep_dom_get(dom, reuse)) {
            s_copyz(dom->nodes[reuse - 1u].text,
                    sizeof(dom->nodes[reuse - 1u].text), text);
            dom_append(dom, id, reuse);
        } else {
            uint32_t child = sweep_dom_create_text(dom, text);
            if (!child) {
                /* Out of slots: the replacement text could not be created.
                 * Old children are gone (detached); mark dirty and report. */
                dom_mark_dirty_text(dom, id);
                return false;
            }
            dom_append(dom, id, child);
        }
    }
    /* The discarded subtree is replaced, not removed via removeChild, so it
     * is safe to reclaim: no caller keeps these nodes for re-append, and any
     * stale JS wrapper is rejected by the bumped slot generation. */
    {
        uint32_t budget = SWEEP_DOM_NODES_MAX;
        for (i = 0u; i < child_count; ++i) {
            if (old_kids[i] != reuse) dom_reclaim_subtree(dom, old_kids[i], 0u, &budget);
        }
    }
    if (dom_is_style_context(dom, id)) {
        dom_mark_dirty(dom, id, SWEEP_DOM_MUT_STYLE, (const char*)0, false, false);
    } else {
        dom_mark_dirty_text(dom, id);
    }
    return true;
}
bool sweep_dom_set_inner_html(SWEEP_DOM* dom, uint32_t id, const char* html,
                              size_t len) {
    SWEEP_DOM_NODE* n;
    uint32_t child_count, i;
    uint32_t old_kids[SWEEP_DOM_CHILDREN_MAX];
    if (!dom || !id || id > SWEEP_DOM_NODES_MAX) return false;
    n = &dom->nodes[id - 1u];
    if (!n->in_use || n->type != SWEEP_DOM_ELEMENT) return false;
    if (strcmp(n->name, "style") == 0) {
        /* innerHTML on <style> is a stylesheet change: full invalidation. */
    }
    child_count = n->child_count;
    if (child_count > SWEEP_DOM_CHILDREN_MAX) child_count = SWEEP_DOM_CHILDREN_MAX;
    for (i = 0u; i < child_count; ++i) old_kids[i] = n->children[i];
    for (i = 0u; i < child_count; ++i) {
        (void) dom_detach(dom, n->children[0]);
    }
    /* Reclaim the replaced subtree before building so repeated innerHTML
     * replacement reuses slots instead of exhausting the 128-node cap. */
    {
        uint32_t budget = SWEEP_DOM_NODES_MAX;
        for (i = 0u; i < child_count; ++i) dom_reclaim_subtree(dom, old_kids[i], 0u, &budget);
    }
    dom_build_into(dom, id, html ? html : "", len);
    dom_mark_dirty(dom, id, SWEEP_DOM_MUT_STRUCTURE, (const char*)0, false, false);
    return true;
}

typedef struct _SWEEP_SERIALIZE_STATE {
    char* out;
    size_t cap;
    size_t used;
} SWEEP_SERIALIZE_STATE;
static void ser_char(SWEEP_SERIALIZE_STATE* s, char c) {
    if (s->used + 1u < s->cap) {
        s->out[s->used++] = c;
        s->out[s->used] = '\0';
    }
}
static void ser_text(SWEEP_SERIALIZE_STATE* s, const char* text) {
    size_t i;
    if (!text) return;
    for (i = 0u; text[i]; ++i) {
        unsigned char c = (unsigned char) text[i];
        if (c == '&') { ser_char(s, '&'); ser_char(s, 'a'); ser_char(s, 'm'); ser_char(s, 'p'); ser_char(s, ';'); }
        else if (c == '<') { ser_char(s, '&'); ser_char(s, 'l'); ser_char(s, 't'); ser_char(s, ';'); }
        else if (c == '>') { ser_char(s, '&'); ser_char(s, 'g'); ser_char(s, 't'); ser_char(s, ';'); }
        else ser_char(s, (char) c);
    }
}
static void ser_attr(SWEEP_SERIALIZE_STATE* s, const char* text) {
    size_t i;
    if (!text) return;
    for (i = 0u; text[i]; ++i) {
        unsigned char c = (unsigned char) text[i];
        if (c == '&') { ser_char(s, '&'); ser_char(s, 'a'); ser_char(s, 'm'); ser_char(s, 'p'); ser_char(s, ';'); }
        else if (c == '"') { ser_char(s, '&'); ser_char(s, 'q'); ser_char(s, 'u'); ser_char(s, 'o'); ser_char(s, 't'); ser_char(s, ';'); }
        else if (c == '<') { ser_char(s, '&'); ser_char(s, 'l'); ser_char(s, 't'); ser_char(s, ';'); }
        else ser_char(s, (char) c);
    }
}
static void ser_node(const SWEEP_DOM* dom, uint32_t id, SWEEP_SERIALIZE_STATE* s,
                     uint32_t depth) {
    const SWEEP_DOM_NODE* n;
    uint32_t i;
    if (depth > 32u) return;
    n = sweep_dom_get(dom, id);
    if (!n) return;
    if (n->type == SWEEP_DOM_TEXT) { ser_text(s, n->text); return; }
    if (n->type == SWEEP_DOM_COMMENT) {
        ser_char(s, '<'); ser_char(s, '!'); ser_char(s, '-'); ser_char(s, '-');
        ser_text(s, n->text);
        ser_char(s, '-'); ser_char(s, '-'); ser_char(s, '>');
        return;
    }
    if (n->type != SWEEP_DOM_ELEMENT) {
        for (i = 0u; i < n->child_count; ++i) ser_node(dom, n->children[i], s, depth + 1u);
        return;
    }
    ser_char(s, '<');
    for (i = 0u; n->name[i]; ++i) ser_char(s, n->name[i]);
    for (i = 0u; i < n->attr_count; ++i) {
        ser_char(s, ' ');
        {
            uint32_t k;
            for (k = 0u; n->attrs[i].name[k]; ++k) ser_char(s, n->attrs[i].name[k]);
        }
        ser_char(s, '=');
        ser_char(s, '"');
        ser_attr(s, n->attrs[i].value);
        ser_char(s, '"');
    }
    ser_char(s, '>');
    if (!is_void_tag(n->name)) {
        for (i = 0u; i < n->child_count; ++i) ser_node(dom, n->children[i], s, depth + 1u);
        ser_char(s, '<'); ser_char(s, '/');
        for (i = 0u; n->name[i]; ++i) ser_char(s, n->name[i]);
        ser_char(s, '>');
    }
}
bool sweep_dom_get_inner_html(const SWEEP_DOM* dom, uint32_t id, char* out,
                              size_t cap, size_t* out_len) {
    const SWEEP_DOM_NODE* n;
    SWEEP_SERIALIZE_STATE s;
    uint32_t i;
    if (!out || cap == 0u) return false;
    out[0] = '\0';
    if (out_len) *out_len = 0u;
    n = sweep_dom_get(dom, id);
    if (!n) return false;
    s.out = out; s.cap = cap; s.used = 0u;
    for (i = 0u; i < n->child_count; ++i) {
        ser_node(dom, n->children[i], &s, 1u);
    }
    if (out_len) *out_len = s.used;
    return true;
}
uint32_t sweep_dom_find_by_id(const SWEEP_DOM* dom, uint32_t root, const char* id) {
    const SWEEP_DOM_NODE* n;
    uint32_t i, found;
    if (!dom || !id) return 0u;
    n = sweep_dom_get(dom, root);
    if (!n) return 0u;
    if (n->type == SWEEP_DOM_ELEMENT) {
        const char* v = sweep_dom_get_attr(n, "id");
        if (v && strcmp(v, id) == 0) return root;
    }
    for (i = 0u; i < n->child_count; ++i) {
        found = sweep_dom_find_by_id(dom, n->children[i], id);
        if (found) return found;
    }
    return 0u;
}
