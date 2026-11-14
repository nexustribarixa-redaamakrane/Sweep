/* sweep_css.c - Sweep CSS engine (tokenizer, cascade, layout, paint).
 * See sweep_css.h for the documented scope, units and limits. */
#include "sweep_css.h"

#include <string.h>

/* ======================================================================= */
/* Small helpers                                                           */
/* ======================================================================= */

static bool ch_is_digit(char c) { return c >= '0' && c <= '9'; }
static bool ch_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool ch_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static bool ch_is_ident_start(char c) {
    return ch_is_alpha(c) || c == '_' || c == '-' || (unsigned char)c >= 0x80u;
}
static bool ch_is_ident_char(char c) {
    return ch_is_ident_start(c) || ch_is_digit(c);
}
static char ch_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}
static bool str_eq_icase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (ch_lower(*a) != ch_lower(*b)) return false;
        ++a; ++b;
    }
    return *a == *b;
}
static void str_copy(char* dst, size_t cap, const char* src) {
    size_t n;
    if (!dst || cap == 0u) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}
static void str_copy_lower(char* dst, size_t cap, const char* src) {
    size_t i;
    str_copy(dst, cap, src);
    for (i = 0u; dst[i] != '\0'; ++i) dst[i] = ch_lower(dst[i]);
}
static bool buf_putc(char* buf, size_t cap, char c) {
    size_t n = strlen(buf);
    if (n + 1u >= cap) return false;
    buf[n] = c;
    buf[n + 1u] = '\0';
    return true;
}
static bool buf_puts(char* buf, size_t cap, const char* s) {
    size_t n = strlen(buf);
    size_t m = strlen(s);
    if (n + m + 1u > cap) return false;
    memcpy(buf + n, s, m + 1u);
    return true;
}

/* ======================================================================= */
/* CSS tokenizer                                                           */
/* ======================================================================= */

static void tok_clear(SWEEP_CSS_TOKEN* t, SWEEP_CSS_TOKEN_TYPE type) {
    memset(t, 0, sizeof(*t));
    t->type = type;
}

static SWEEP_CSS_TOKEN* tok_push(SWEEP_CSS_TOKEN* out, size_t cap, size_t* count,
                                 SWEEP_CSS_TOKEN_TYPE type) {
    SWEEP_CSS_TOKEN* t;
    if (*count + 1u >= cap) return (SWEEP_CSS_TOKEN*)0; /* reserve EOF slot */
    t = &out[*count];
    tok_clear(t, type);
    ++(*count);
    return t;
}

static void tok_set_span(SWEEP_CSS_TOKEN* t, const char* src, size_t b, size_t e) {
    size_t n = e - b;
    if (n >= sizeof(t->text)) n = sizeof(t->text) - 1u;
    memcpy(t->text, src + b, n);
    t->text[n] = '\0';
}

static bool number_start(const char* s, size_t len, size_t pos) {
    char c;
    if (pos >= len) return false;
    c = s[pos];
    if (ch_is_digit(c)) return true;
    if (c == '.' && pos + 1u < len && ch_is_digit(s[pos + 1u])) return true;
    if ((c == '-' || c == '+') && pos + 1u < len) {
        if (ch_is_digit(s[pos + 1u])) return true;
        if (s[pos + 1u] == '.' && pos + 2u < len && ch_is_digit(s[pos + 2u])) {
            return true;
        }
    }
    return false;
}

/* Parse a CSS number; value returned in thousandths. */
static size_t parse_number_value(const char* s, size_t len, size_t pos,
                                 int32_t* out_milli) {
    int64_t whole = 0;
    int64_t frac = 0;
    int frac_digits = 0;
    bool dot = false;
    bool negative = false;

    if (pos < len && (s[pos] == '-' || s[pos] == '+')) {
        negative = (s[pos] == '-');
        ++pos;
    }
    while (pos < len) {
        char c = s[pos];
        if (ch_is_digit(c)) {
            if (dot) {
                if (frac_digits < 3) {
                    frac = frac * 10 + (int64_t)(c - '0');
                    ++frac_digits;
                }
            } else if (whole < 1000000) {
                whole = whole * 10 + (int64_t)(c - '0');
            }
            ++pos;
        } else if (c == '.' && !dot) {
            dot = true;
            ++pos;
        } else {
            break;
        }
    }
    while (frac_digits < 3) {
        frac *= 10;
        ++frac_digits;
    }
    if (out_milli) {
        int64_t v = whole * 1000 + frac;
        if (negative) v = -v;
        if (v > 2147483647LL) v = 2147483647LL;
        if (v < -2147483647LL) v = -2147483647LL;
        *out_milli = (int32_t)v;
    }
    return pos;
}

static size_t read_ident(const char* s, size_t len, size_t pos,
                         char* out, size_t cap) {
    size_t o = 0u;
    while (pos < len) {
        char c = s[pos];
        if (c == '\\' && pos + 1u < len) {
            c = s[pos + 1u];
            if (o + 1u < cap) out[o++] = c;
            pos += 2u;
            continue;
        }
        if (!ch_is_ident_char(c)) break;
        if (o + 1u < cap) out[o++] = c;
        ++pos;
    }
    if (cap > 0u) out[o] = '\0';
    return pos;
}

static uint8_t unit_from_name(const char* name) {
    if (str_eq_icase(name, "px")) return SWEEP_CSS_UNIT_PX;
    if (str_eq_icase(name, "em")) return SWEEP_CSS_UNIT_EM;
    if (str_eq_icase(name, "rem")) return SWEEP_CSS_UNIT_REM;
    if (str_eq_icase(name, "pt")) return SWEEP_CSS_UNIT_PT;
    return SWEEP_CSS_UNIT_NONE;
}

static bool token_punct(SWEEP_CSS_TOKEN* out, size_t cap, size_t* count,
                        char c) {
    SWEEP_CSS_TOKEN* t;
    switch (c) {
    case ':': t = tok_push(out, cap, count, SWEEP_CSS_TOK_COLON); break;
    case ';': t = tok_push(out, cap, count, SWEEP_CSS_TOK_SEMICOLON); break;
    case ',': t = tok_push(out, cap, count, SWEEP_CSS_TOK_COMMA); break;
    case '{': t = tok_push(out, cap, count, SWEEP_CSS_TOK_LBRACE); break;
    case '}': t = tok_push(out, cap, count, SWEEP_CSS_TOK_RBRACE); break;
    case '(': t = tok_push(out, cap, count, SWEEP_CSS_TOK_LPAREN); break;
    case ')': t = tok_push(out, cap, count, SWEEP_CSS_TOK_RPAREN); break;
    case '[': t = tok_push(out, cap, count, SWEEP_CSS_TOK_LBRACKET); break;
    case ']': t = tok_push(out, cap, count, SWEEP_CSS_TOK_RBRACKET); break;
    default: t = tok_push(out, cap, count, SWEEP_CSS_TOK_DELIM); break;
    }
    if (!t) return false;
    t->text[0] = c;
    t->text[1] = '\0';
    return true;
}

bool sweep_css_tokenize(const char* css, size_t len,
                        SWEEP_CSS_TOKEN* out, size_t cap, size_t* out_count) {
    size_t pos = 0u;
    size_t count = 0u;
    const char* s = css ? css : "";

    if (out == (SWEEP_CSS_TOKEN*)0 || out_count == (size_t*)0 || cap < 2u) {
        if (out_count) *out_count = 0u;
        return false;
    }
    if (!css) len = 0u;

    while (pos < len) {
        char c = s[pos];

        if (ch_is_space(c)) {
            SWEEP_CSS_TOKEN* t;
            while (pos < len && ch_is_space(s[pos])) ++pos;
            t = tok_push(out, cap, &count, SWEEP_CSS_TOK_WHITESPACE);
            if (!t) { *out_count = count; return false; }
            str_copy(t->text, sizeof(t->text), " ");
            continue;
        }

        if (c == '/' && pos + 1u < len && s[pos + 1u] == '*') {
            SWEEP_CSS_TOKEN* t;
            pos += 2u;
            t = tok_push(out, cap, &count, SWEEP_CSS_TOK_COMMENT);
            if (!t) { *out_count = count; return false; }
            while (pos + 1u < len && !(s[pos] == '*' && s[pos + 1u] == '/')) ++pos;
            if (pos + 1u < len) {
                pos += 2u;
            } else {
                pos = len;
                t->unterminated = true;
            }
            continue;
        }

        if (c == '"' || c == '\'') {
            char quote = c;
            SWEEP_CSS_TOKEN* t = tok_push(out, cap, &count, SWEEP_CSS_TOK_STRING);
            if (!t) { *out_count = count; return false; }
            buf_putc(t->text, sizeof(t->text), quote);
            ++pos;
            while (pos < len) {
                char d = s[pos];
                if (d == '\\' && pos + 1u < len) {
                    buf_putc(t->text, sizeof(t->text), '\\');
                    buf_putc(t->text, sizeof(t->text), s[pos + 1u]);
                    pos += 2u;
                    continue;
                }
                buf_putc(t->text, sizeof(t->text), d);
                ++pos;
                if (d == quote) break;
            }
            {
                size_t n = strlen(t->text);
                if (n == 0u || t->text[n - 1u] != quote) {
                    t->unterminated = true;
                }
            }
            continue;
        }

        if (number_start(s, len, pos)) {
            size_t start = pos;
            int32_t milli = 0;
            SWEEP_CSS_TOKEN* t;
            pos = parse_number_value(s, len, pos, &milli);
            if (pos < len && s[pos] == '%') {
                ++pos;
                t = tok_push(out, cap, &count, SWEEP_CSS_TOK_PERCENTAGE);
                if (!t) { *out_count = count; return false; }
                t->number_milli = milli;
                t->unit = SWEEP_CSS_UNIT_PERCENT;
                tok_set_span(t, s, start, pos);
            } else if (pos < len && ch_is_ident_start(s[pos])) {
                char unit[16];
                size_t ustart = pos;
                pos = read_ident(s, len, pos, unit, sizeof(unit));
                t = tok_push(out, cap, &count, SWEEP_CSS_TOK_DIMENSION);
                if (!t) { *out_count = count; return false; }
                t->number_milli = milli;
                t->unit = unit_from_name(unit);
                tok_set_span(t, s, start, pos);
                (void)ustart;
            } else {
                t = tok_push(out, cap, &count, SWEEP_CSS_TOK_NUMBER);
                if (!t) { *out_count = count; return false; }
                t->number_milli = milli;
                t->unit = SWEEP_CSS_UNIT_NONE;
                tok_set_span(t, s, start, pos);
            }
            continue;
        }

        if (c == '@') {
            char name[SWEEP_CSS_TOKEN_TEXT_MAX];
            size_t start = pos;
            SWEEP_CSS_TOKEN* t;
            ++pos;
            if (pos < len && ch_is_ident_start(s[pos])) {
                pos = read_ident(s, len, pos, name, sizeof(name));
            } else {
                name[0] = '\0';
            }
            t = tok_push(out, cap, &count, SWEEP_CSS_TOK_AT_KEYWORD);
            if (!t) { *out_count = count; return false; }
            tok_set_span(t, s, start, pos);
            continue;
        }

        if (c == '#') {
            char name[SWEEP_CSS_TOKEN_TEXT_MAX];
            size_t start = pos;
            SWEEP_CSS_TOKEN* t;
            ++pos;
            if (pos < len && ch_is_ident_char(s[pos])) {
                pos = read_ident(s, len, pos, name, sizeof(name));
            }
            t = tok_push(out, cap, &count, SWEEP_CSS_TOK_HASH);
            if (!t) { *out_count = count; return false; }
            tok_set_span(t, s, start, pos);
            continue;
        }

        if (ch_is_ident_start(c)) {
            char name[SWEEP_CSS_TOKEN_TEXT_MAX];
            size_t start = pos;
            SWEEP_CSS_TOKEN* t;
            pos = read_ident(s, len, pos, name, sizeof(name));
            t = tok_push(out, cap, &count, SWEEP_CSS_TOK_IDENT);
            if (!t) { *out_count = count; return false; }
            tok_set_span(t, s, start, pos);
            continue;
        }

        if (c == ':' || c == ';' || c == ',' || c == '{' || c == '}' ||
            c == '(' || c == ')' || c == '[' || c == ']' || c == '*' ||
            c == '.' || c == '>' || c == '+' || c == '~' || c == '=' ||
            c == '!' || c == '|') {
            if (!token_punct(out, cap, &count, c)) {
                *out_count = count;
                return false;
            }
            ++pos;
            continue;
        }

        {
            SWEEP_CSS_TOKEN* t = tok_push(out, cap, &count, SWEEP_CSS_TOK_DELIM);
            if (!t) { *out_count = count; return false; }
            t->text[0] = c;
            t->text[1] = '\0';
            ++pos;
        }
    }

    {
        SWEEP_CSS_TOKEN* t = tok_push(out, cap, &count, SWEEP_CSS_TOK_EOF);
        if (!t) { *out_count = count; return false; }
    }
    *out_count = count;
    return true;
}

/* ======================================================================= */
/* Declaration parsing                                                     */
/* ======================================================================= */

#define SWEEP_CSS_DECL_TOK_CAP 128u

static bool tok_is_ws(const SWEEP_CSS_TOKEN* t) {
    return t->type == SWEEP_CSS_TOK_WHITESPACE || t->type == SWEEP_CSS_TOK_COMMENT;
}

static void decl_append_value(char* value, size_t cap, const SWEEP_CSS_TOKEN* t) {
    /* Join token spellings with single spaces; this normalizes whitespace and
     * keeps numbers/dimensions/hashes self-contained. */
    if (value[0] != '\0' && t->type != SWEEP_CSS_TOK_COMMA) {
        (void)buf_putc(value, cap, ' ');
    }
    (void)buf_puts(value, cap, t->text);
}

/* Parse declarations from a token slice [start,end).  Consumes tokens up to
 * and including a terminating RBRACE (if present) or the end. */
static void parse_decl_tokens(const SWEEP_CSS_TOKEN* tok, size_t n,
                              size_t* io, SWEEP_CSS_DECL* out, size_t cap,
                              size_t* out_count) {
    size_t i = *io;
    size_t count = 0u;

    while (i < n) {
        SWEEP_CSS_DECL decl;
        while (i < n && (tok_is_ws(&tok[i]) || tok[i].type == SWEEP_CSS_TOK_SEMICOLON)) ++i;
        if (i >= n) break;
        if (tok[i].type == SWEEP_CSS_TOK_RBRACE) { ++i; break; }
        if (tok[i].type != SWEEP_CSS_TOK_IDENT) {
            while (i < n && tok[i].type != SWEEP_CSS_TOK_SEMICOLON &&
                   tok[i].type != SWEEP_CSS_TOK_RBRACE) ++i;
            if (i < n && tok[i].type == SWEEP_CSS_TOK_SEMICOLON) ++i;
            continue;
        }

        memset(&decl, 0, sizeof(decl));
        str_copy_lower(decl.name, sizeof(decl.name), tok[i].text);
        ++i;
        while (i < n && tok_is_ws(&tok[i])) ++i;
        if (i >= n || tok[i].type != SWEEP_CSS_TOK_COLON) {
            while (i < n && tok[i].type != SWEEP_CSS_TOK_SEMICOLON &&
                   tok[i].type != SWEEP_CSS_TOK_RBRACE) ++i;
            if (i < n && tok[i].type == SWEEP_CSS_TOK_SEMICOLON) ++i;
            continue;
        }
        ++i;

        {
            int depth = 0;
            bool any = false;
            bool too_long = false;
            decl.value[0] = '\0';
            while (i < n) {
                SWEEP_CSS_TOKEN_TYPE tt = tok[i].type;
                if (depth == 0 && (tt == SWEEP_CSS_TOK_SEMICOLON || tt == SWEEP_CSS_TOK_RBRACE)) {
                    break;
                }
                if (tt == SWEEP_CSS_TOK_LPAREN || tt == SWEEP_CSS_TOK_LBRACKET) ++depth;
                else if ((tt == SWEEP_CSS_TOK_RPAREN || tt == SWEEP_CSS_TOK_RBRACKET) && depth > 0) --depth;

                if (tt == SWEEP_CSS_TOK_DELIM && tok[i].text[0] == '!') {
                    size_t j = i + 1u;
                    while (j < n && tok_is_ws(&tok[j])) ++j;
                    if (j < n && tok[j].type == SWEEP_CSS_TOK_IDENT &&
                        str_eq_icase(tok[j].text, "important")) {
                        decl.important = true;
                        i = j + 1u;
                        while (i < n && tok_is_ws(&tok[i])) ++i;
                        continue;
                    }
                }
                if (!tok_is_ws(&tok[i])) {
                    size_t before = strlen(decl.value);
                    decl_append_value(decl.value, sizeof(decl.value), &tok[i]);
                    if (strlen(decl.value) == before && before + 1u >= sizeof(decl.value)) {
                        too_long = true;
                    }
                    any = true;
                }
                ++i;
            }
            if (!any || too_long) {
                /* invalid/oversized declaration: drop safely */
                if (i < n && tok[i].type == SWEEP_CSS_TOK_SEMICOLON) ++i;
                continue;
            }
        }
        if (count < cap) {
            out[count] = decl;
            ++count;
        }
        if (i < n && tok[i].type == SWEEP_CSS_TOK_SEMICOLON) ++i;
        else if (i < n && tok[i].type == SWEEP_CSS_TOK_RBRACE) { ++i; break; }
    }
    *io = i;
    *out_count = count;
}

bool sweep_css_parse_declarations(const char* text, size_t len,
                                  SWEEP_CSS_DECL* out, size_t cap,
                                  size_t* out_count) {
    SWEEP_CSS_TOKEN tok[SWEEP_CSS_DECL_TOK_CAP];
    size_t n = 0u;
    size_t i = 0u;

    if (out == (SWEEP_CSS_DECL*)0 || out_count == (size_t*)0 || cap == 0u) {
        if (out_count) *out_count = 0u;
        return false;
    }
    *out_count = 0u;
    if (!sweep_css_tokenize(text, text ? len : 0u, tok, SWEEP_CSS_DECL_TOK_CAP, &n)) {
        return false;
    }
    parse_decl_tokens(tok, n, &i, out, cap, out_count);
    return true;
}

/* ======================================================================= */
/* Selector parsing and matching                                           */
/* ======================================================================= */

static bool parse_attr_selector(const SWEEP_CSS_TOKEN* tok, size_t n,
                                size_t* io, SWEEP_CSS_SIMPLE* simple) {
    size_t i = *io; /* at '[' */
    ++i;
    while (i < n && tok_is_ws(&tok[i])) ++i;
    if (i >= n || tok[i].type != SWEEP_CSS_TOK_IDENT) return false;
    str_copy(simple->name, sizeof(simple->name), tok[i].text);
    ++i;
    while (i < n && tok_is_ws(&tok[i])) ++i;
    if (i < n && tok[i].type == SWEEP_CSS_TOK_RBRACKET) {
        simple->kind = SWEEP_CSS_SIMPLE_ATTR_EXISTS;
        *io = i + 1u;
        return true;
    }
    if (i >= n || tok[i].type != SWEEP_CSS_TOK_DELIM || tok[i].text[0] != '=') {
        return false;
    }
    ++i;
    while (i < n && tok_is_ws(&tok[i])) ++i;
    if (i >= n) return false;
    if (tok[i].type == SWEEP_CSS_TOK_STRING) {
        const char* s = tok[i].text;
        size_t m = strlen(s);
        if (m >= 2u && (s[0] == '"' || s[0] == '\'') && s[m - 1u] == s[0]) {
            char tmp[SWEEP_CSS_ATTR_VALUE_MAX];
            size_t k;
            m -= 2u;
            if (m >= sizeof(tmp)) m = sizeof(tmp) - 1u;
            for (k = 0u; k < m; ++k) tmp[k] = s[1u + k];
            tmp[m] = '\0';
            str_copy(simple->value, sizeof(simple->value), tmp);
        } else {
            str_copy(simple->value, sizeof(simple->value), s);
        }
    } else if (tok[i].type == SWEEP_CSS_TOK_IDENT ||
               tok[i].type == SWEEP_CSS_TOK_NUMBER ||
               tok[i].type == SWEEP_CSS_TOK_HASH) {
        str_copy(simple->value, sizeof(simple->value), tok[i].text);
    } else {
        return false;
    }
    ++i;
    while (i < n && tok_is_ws(&tok[i])) ++i;
    if (i >= n || tok[i].type != SWEEP_CSS_TOK_RBRACKET) return false;
    simple->kind = SWEEP_CSS_SIMPLE_ATTR_EQUALS;
    *io = i + 1u;
    return true;
}

static void selector_specificity(SWEEP_CSS_SELECTOR* sel) {
    uint32_t p, k;
    sel->spec_a = 0u;
    sel->spec_b = 0u;
    sel->spec_c = 0u;
    for (p = 0u; p < sel->part_count; ++p) {
        const SWEEP_CSS_COMPOUND* c = &sel->parts[p];
        for (k = 0u; k < c->simple_count; ++k) {
            switch (c->simples[k].kind) {
            case SWEEP_CSS_SIMPLE_ID: ++sel->spec_a; break;
            case SWEEP_CSS_SIMPLE_CLASS:
            case SWEEP_CSS_SIMPLE_ATTR_EXISTS:
            case SWEEP_CSS_SIMPLE_ATTR_EQUALS: ++sel->spec_b; break;
            case SWEEP_CSS_SIMPLE_TYPE: ++sel->spec_c; break;
            default: break;
            }
        }
    }
}

static bool parse_one_selector(const SWEEP_CSS_TOKEN* tok, size_t b, size_t e,
                               SWEEP_CSS_SELECTOR* sel) {
    size_t i = b;
    memset(sel, 0, sizeof(*sel));

    while (i < e) {
        bool saw_ws = false;
        uint8_t comb = SWEEP_CSS_COMB_NONE;
        SWEEP_CSS_COMPOUND* comp;

        while (i < e && tok_is_ws(&tok[i])) { saw_ws = true; ++i; }
        if (i >= e || tok[i].type == SWEEP_CSS_TOK_COMMA) break;

        if (sel->part_count > 0u) {
            if (tok[i].type == SWEEP_CSS_TOK_DELIM && tok[i].text[0] == '>') {
                comb = SWEEP_CSS_COMB_CHILD;
                ++i;
                while (i < e && tok_is_ws(&tok[i])) ++i;
            } else if (saw_ws) {
                comb = SWEEP_CSS_COMB_DESCENDANT;
            } else {
                sel->unsupported = true;
                return false;
            }
            if (i >= e) { sel->unsupported = true; return false; }
        }
        if (sel->part_count >= SWEEP_CSS_SELECTOR_PARTS_MAX) {
            sel->unsupported = true;
            return false;
        }

        comp = &sel->parts[sel->part_count];
        memset(comp, 0, sizeof(*comp));
        comp->combinator = comb;

        while (i < e) {
            if (tok_is_ws(&tok[i]) || tok[i].type == SWEEP_CSS_TOK_COMMA) break;
            if (tok[i].type == SWEEP_CSS_TOK_DELIM && tok[i].text[0] == '>') break;

            if (comp->simple_count >= SWEEP_CSS_COMPOUND_SIMPLES_MAX) {
                sel->unsupported = true;
                return false;
            }

            if (tok[i].type == SWEEP_CSS_TOK_DELIM && tok[i].text[0] == '*') {
                SWEEP_CSS_SIMPLE* s = &comp->simples[comp->simple_count++];
                memset(s, 0, sizeof(*s));
                s->kind = SWEEP_CSS_SIMPLE_UNIVERSAL;
                ++i;
                continue;
            }
            if (tok[i].type == SWEEP_CSS_TOK_DELIM && tok[i].text[0] == '.') {
                SWEEP_CSS_SIMPLE* s;
                ++i;
                if (i >= e || tok[i].type != SWEEP_CSS_TOK_IDENT) {
                    sel->unsupported = true;
                    return false;
                }
                s = &comp->simples[comp->simple_count++];
                memset(s, 0, sizeof(*s));
                s->kind = SWEEP_CSS_SIMPLE_CLASS;
                str_copy(s->name, sizeof(s->name), tok[i].text);
                ++i;
                continue;
            }
            if (tok[i].type == SWEEP_CSS_TOK_IDENT) {
                SWEEP_CSS_SIMPLE* s = &comp->simples[comp->simple_count++];
                memset(s, 0, sizeof(*s));
                s->kind = SWEEP_CSS_SIMPLE_TYPE;
                str_copy(s->name, sizeof(s->name), tok[i].text);
                ++i;
                continue;
            }
            if (tok[i].type == SWEEP_CSS_TOK_HASH) {
                SWEEP_CSS_SIMPLE* s = &comp->simples[comp->simple_count++];
                memset(s, 0, sizeof(*s));
                s->kind = SWEEP_CSS_SIMPLE_ID;
                str_copy(s->name, sizeof(s->name), tok[i].text + 1u);
                ++i;
                continue;
            }
            if (tok[i].type == SWEEP_CSS_TOK_LBRACKET) {
                SWEEP_CSS_SIMPLE* s = &comp->simples[comp->simple_count++];
                memset(s, 0, sizeof(*s));
                if (!parse_attr_selector(tok, e, &i, s)) {
                    sel->unsupported = true;
                    return false;
                }
                continue;
            }
            if (tok[i].type == SWEEP_CSS_TOK_COLON) {
                /* Pseudo-classes/elements are unsupported in this phase: fail
                 * gracefully by marking the selector non-matching. */
                sel->unsupported = true;
                return false;
            }
            /* Any other token (e.g. '+', '~', '|') is unsupported. */
            sel->unsupported = true;
            return false;
        }

        if (comp->simple_count == 0u) {
            sel->unsupported = true;
            return false;
        }
        ++sel->part_count;
    }

    if (sel->part_count == 0u) {
        sel->unsupported = true;
        return false;
    }
    selector_specificity(sel);
    return !sel->unsupported;
}

static void parse_selector_list(const SWEEP_CSS_TOKEN* tok, size_t b, size_t e,
                                SWEEP_CSS_RULE* rule) {
    size_t i = b;
    rule->selector_count = 0u;
    rule->invalid_selectors = 0u;

    while (i < e) {
        size_t start = i;
        size_t end;
        int depth = 0;
        while (i < e) {
            if (tok[i].type == SWEEP_CSS_TOK_LBRACKET || tok[i].type == SWEEP_CSS_TOK_LPAREN) ++depth;
            else if ((tok[i].type == SWEEP_CSS_TOK_RBRACKET || tok[i].type == SWEEP_CSS_TOK_RPAREN) && depth > 0) --depth;
            if (depth == 0 && tok[i].type == SWEEP_CSS_TOK_COMMA) break;
            ++i;
        }
        end = i;
        while (start < end && tok_is_ws(&tok[start])) ++start;
        while (end > start && tok_is_ws(&tok[end - 1u])) --end;

        if (end > start) {
            SWEEP_CSS_SELECTOR sel;
            if (parse_one_selector(tok, start, end, &sel)) {
                if (rule->selector_count < SWEEP_CSS_MAX_SELECTORS) {
                    rule->selectors[rule->selector_count++] = sel;
                }
            } else {
                ++rule->invalid_selectors;
            }
        }
        if (i < e && tok[i].type == SWEEP_CSS_TOK_COMMA) ++i;
    }
}

bool sweep_css_parse_stylesheet(const char* css, size_t len,
                                SWEEP_CSS_STYLESHEET* out) {
    SWEEP_CSS_TOKEN tok[SWEEP_CSS_MAX_TOKENS];
    size_t n = 0u;
    size_t i = 0u;

    if (out == (SWEEP_CSS_STYLESHEET*)0) return false;
    memset(out, 0, sizeof(*out));
    if (!sweep_css_tokenize(css, css ? len : 0u, tok, SWEEP_CSS_MAX_TOKENS, &n)) {
        out->truncated = true;
        return false;
    }

    while (i < n) {
        SWEEP_CSS_RULE* rule;
        size_t start;
        size_t decl_i;
        size_t decl_count = 0u;

        while (i < n && tok_is_ws(&tok[i])) ++i;
        if (i >= n || tok[i].type == SWEEP_CSS_TOK_EOF) break;
        if (tok[i].type == SWEEP_CSS_TOK_RBRACE ||
            tok[i].type == SWEEP_CSS_TOK_SEMICOLON) { ++i; continue; }

        if (tok[i].type == SWEEP_CSS_TOK_AT_KEYWORD) {
            int depth = 0;
            ++i;
            while (i < n && tok[i].type != SWEEP_CSS_TOK_EOF) {
                if (tok[i].type == SWEEP_CSS_TOK_LBRACE) ++depth;
                else if (tok[i].type == SWEEP_CSS_TOK_RBRACE) {
                    if (depth > 0) --depth;
                    if (depth == 0) { ++i; break; }
                } else if (tok[i].type == SWEEP_CSS_TOK_SEMICOLON && depth == 0) {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        start = i;
        while (i < n && tok[i].type != SWEEP_CSS_TOK_LBRACE &&
               tok[i].type != SWEEP_CSS_TOK_SEMICOLON &&
               tok[i].type != SWEEP_CSS_TOK_RBRACE &&
               tok[i].type != SWEEP_CSS_TOK_EOF) {
            ++i;
        }
        if (i >= n || tok[i].type != SWEEP_CSS_TOK_LBRACE) {
            if (i < n && tok[i].type == SWEEP_CSS_TOK_SEMICOLON) ++i;
            else if (i < n && tok[i].type == SWEEP_CSS_TOK_RBRACE) ++i;
            ++out->error_count;
            continue;
        }

        if (out->rule_count >= SWEEP_CSS_MAX_RULES) {
            out->truncated = true;
            /* Skip the block. */
            {
                int depth = 0;
                while (i < n && tok[i].type != SWEEP_CSS_TOK_EOF) {
                    if (tok[i].type == SWEEP_CSS_TOK_LBRACE) ++depth;
                    else if (tok[i].type == SWEEP_CSS_TOK_RBRACE) {
                        if (depth > 0) --depth;
                        if (depth == 0) { ++i; break; }
                    }
                    ++i;
                }
            }
            continue;
        }

        rule = &out->rules[out->rule_count];
        memset(rule, 0, sizeof(*rule));
        parse_selector_list(tok, start, i, rule);
        ++i; /* consume '{' */
        decl_i = i;
        parse_decl_tokens(tok, n, &decl_i, rule->decls, SWEEP_CSS_MAX_DECLS,
                          &decl_count);
        rule->decl_count = (uint8_t)decl_count;
        i = decl_i;
        if (rule->selector_count > 0u || rule->invalid_selectors > 0u) {
            ++out->rule_count;
        }
    }

    return true;
}

/* ---- selector matching ---- */

static bool node_has_class(const SWEEP_DOM_NODE* node, const char* name) {
    const char* cls = sweep_dom_get_attr(node, "class");
    const char* p;
    if (!cls) return false;
    p = cls;
    while (*p) {
        const char* start;
        size_t len;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') ++p;
        start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f') ++p;
        len = (size_t)(p - start);
        if (len == strlen(name) && strncmp(start, name, len) == 0) return true;
    }
    return false;
}

static bool match_compound(const SWEEP_DOM_NODE* node,
                           const SWEEP_CSS_COMPOUND* comp) {
    uint32_t k;
    if (!node || node->type != SWEEP_DOM_ELEMENT) return false;
    for (k = 0u; k < comp->simple_count; ++k) {
        const SWEEP_CSS_SIMPLE* s = &comp->simples[k];
        switch (s->kind) {
        case SWEEP_CSS_SIMPLE_UNIVERSAL:
            break;
        case SWEEP_CSS_SIMPLE_TYPE:
            if (!str_eq_icase(node->name, s->name)) return false;
            break;
        case SWEEP_CSS_SIMPLE_CLASS:
            if (!node_has_class(node, s->name)) return false;
            break;
        case SWEEP_CSS_SIMPLE_ID: {
            const char* id = sweep_dom_get_attr(node, "id");
            if (!id || strcmp(id, s->name) != 0) return false;
            break;
        }
        case SWEEP_CSS_SIMPLE_ATTR_EXISTS:
            if (!sweep_dom_get_attr(node, s->name)) return false;
            break;
        case SWEEP_CSS_SIMPLE_ATTR_EQUALS: {
            const char* v = sweep_dom_get_attr(node, s->name);
            if (!v || strcmp(v, s->value) != 0) return false;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

static bool match_at(const SWEEP_DOM* dom, uint32_t node_id,
                     const SWEEP_CSS_SELECTOR* sel, uint32_t part) {
    const SWEEP_DOM_NODE* node = sweep_dom_get(dom, node_id);
    uint8_t comb;
    if (!node) return false;
    if (!match_compound(node, &sel->parts[part])) return false;
    if (part == 0u) return true;

    comb = sel->parts[part].combinator;
    if (comb == SWEEP_CSS_COMB_CHILD) {
        return node->parent ? match_at(dom, node->parent, sel, part - 1u) : false;
    }
    if (comb == SWEEP_CSS_COMB_DESCENDANT) {
        uint32_t p = node->parent;
        while (p != 0u) {
            if (match_at(dom, p, sel, part - 1u)) return true;
            {
                const SWEEP_DOM_NODE* pn = sweep_dom_get(dom, p);
                if (!pn) break;
                p = pn->parent;
            }
        }
        return false;
    }
    return false;
}

bool sweep_css_selector_matches(const SWEEP_DOM* dom, uint32_t node,
                                const SWEEP_CSS_SELECTOR* selector) {
    if (!dom || !selector) return false;
    if (selector->unsupported || selector->part_count == 0u) return false;
    return match_at(dom, node, selector, (uint32_t)(selector->part_count - 1u));
}

bool sweep_css_parse_selector(const char* text, size_t len,
                              SWEEP_CSS_SELECTOR* out) {
    SWEEP_CSS_TOKEN tok[SWEEP_CSS_MAX_TOKENS];
    size_t n = 0u, b = 0u, e;
    if (!text || !out) return false;
    memset(out, 0, sizeof(*out));
    if (!sweep_css_tokenize(text, len, tok, SWEEP_CSS_MAX_TOKENS, &n)) {
        out->unsupported = true;
        return false;
    }
    e = n;
    while (e > b && (tok[e - 1u].type == SWEEP_CSS_TOK_EOF || tok_is_ws(&tok[e - 1u]))) {
        --e;
    }
    while (b < e && tok_is_ws(&tok[b])) ++b;
    if (b >= e) { out->unsupported = true; return false; }
    for (size_t i = b; i < e; ++i) {
        if (tok[i].type == SWEEP_CSS_TOK_COMMA) {
            out->unsupported = true;
            return false;
        }
    }
    return parse_one_selector(tok, b, e, out);
}

/* ======================================================================= */
/* CSS value helpers                                                       */
/* ======================================================================= */

static bool value_tokens(const char* value, SWEEP_CSS_TOKEN* out, size_t cap,
                         size_t* out_count) {
    SWEEP_CSS_TOKEN raw[SWEEP_CSS_DECL_TOK_CAP];
    size_t n = 0u;
    size_t i;
    size_t count = 0u;

    if (!sweep_css_tokenize(value, value ? strlen(value) : 0u, raw,
                            SWEEP_CSS_DECL_TOK_CAP, &n)) {
        return false;
    }
    for (i = 0u; i < n && count < cap; ++i) {
        if (raw[i].type == SWEEP_CSS_TOK_EOF) break;
        if (tok_is_ws(&raw[i])) continue;
        out[count++] = raw[i];
    }
    *out_count = count;
    return true;
}

static bool length_from_token(const SWEEP_CSS_TOKEN* t, SWEEP_CSS_LENGTH* out) {
    memset(out, 0, sizeof(*out));
    if (t->type == SWEEP_CSS_TOK_IDENT && str_eq_icase(t->text, "auto")) {
        out->unit = SWEEP_CSS_UNIT_AUTO;
        return true;
    }
    if (t->type == SWEEP_CSS_TOK_NUMBER) {
        if (t->number_milli == 0) {
            out->unit = SWEEP_CSS_UNIT_PX;
            return true;
        }
        return false; /* unitless non-zero length is invalid */
    }
    if (t->type == SWEEP_CSS_TOK_DIMENSION || t->type == SWEEP_CSS_TOK_PERCENTAGE) {
        if (t->unit == SWEEP_CSS_UNIT_NONE || t->unit == SWEEP_CSS_UNIT_AUTO) {
            return false; /* unsupported unit is not pixels */
        }
        out->value_milli = t->number_milli;
        out->unit = t->unit;
        return true;
    }
    return false;
}

static bool parse_length_list(const SWEEP_CSS_TOKEN* toks, size_t n,
                              SWEEP_CSS_LENGTH out[4]) {
    SWEEP_CSS_LENGTH vals[4];
    size_t count = 0u;
    size_t i;
    for (i = 0u; i < n; ++i) {
        if (count >= 4u) return false;
        if (!length_from_token(&toks[i], &vals[count])) return false;
        ++count;
    }
    if (count == 0u) return false;
    switch (count) {
    case 1u:
        out[0] = vals[0]; out[1] = vals[0]; out[2] = vals[0]; out[3] = vals[0];
        break;
    case 2u:
        out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[0]; out[3] = vals[1];
        break;
    case 3u:
        out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[1];
        break;
    default:
        out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[3];
        break;
    }
    return true;
}

static bool parse_one_length(const SWEEP_CSS_TOKEN* toks, size_t n,
                             SWEEP_CSS_LENGTH* out) {
    if (n != 1u) return false;
    return length_from_token(&toks[0], out);
}

static bool color_from_name(const char* name, uint32_t* rgba) {
    static const struct { const char* name; uint32_t value; } table[] = {
        { "transparent", 0x00000000u },
        { "black",       0xFF000000u },
        { "white",       0xFFFFFFFFu },
        { "red",         0xFFFF0000u },
        { "green",       0xFF008000u },
        { "blue",        0xFF0000FFu },
        { "gray",        0xFF808080u },
        { "grey",        0xFF808080u },
        { "silver",      0xFFC0C0C0u },
        { "maroon",      0xFF800000u },
        { "navy",        0xFF000080u },
        { "teal",        0xFF008080u },
        { "olive",       0xFF808000u },
        { "lime",        0xFF00FF00u },
        { "aqua",        0xFF00FFFFu },
        { "fuchsia",     0xFFFF00FFu },
        { "yellow",      0xFFFFFF00u },
        { "orange",      0xFFFFA500u },
        { "purple",      0xFF800080u }
    };
    size_t i;
    for (i = 0u; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (str_eq_icase(name, table[i].name)) { *rgba = table[i].value; return true; }
    }
    return false;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool color_from_token(const SWEEP_CSS_TOKEN* t, uint32_t* rgba) {
    if (t->type == SWEEP_CSS_TOK_IDENT) {
        return color_from_name(t->text, rgba);
    }
    if (t->type == SWEEP_CSS_TOK_HASH) {
        const char* h = t->text;
        size_t n = strlen(h);
        int r, g, b;
        if (n < 2u || h[0] != '#') return false;
        if (n == 4u) {
            r = hex_val(h[1]); g = hex_val(h[2]); b = hex_val(h[3]);
            if (r < 0 || g < 0 || b < 0) return false;
            r = r * 17; g = g * 17; b = b * 17;
        } else if (n == 7u) {
            int r1 = hex_val(h[1]), r2 = hex_val(h[2]);
            int g1 = hex_val(h[3]), g2 = hex_val(h[4]);
            int b1 = hex_val(h[5]), b2 = hex_val(h[6]);
            if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) return false;
            r = r1 * 16 + r2; g = g1 * 16 + g2; b = b1 * 16 + b2;
        } else {
            return false;
        }
        *rgba = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
                (uint32_t)b;
        return true;
    }
    return false;
}

static bool parse_one_color(const SWEEP_CSS_TOKEN* toks, size_t n,
                            uint32_t* rgba) {
    if (n != 1u) return false;
    return color_from_token(&toks[0], rgba);
}

/* ======================================================================= */
/* Computed styles: initial values, UA defaults, cascade, inheritance      */
/* ======================================================================= */

static void len_zero(SWEEP_CSS_LENGTH* l) {
    l->value_milli = 0;
    l->unit = SWEEP_CSS_UNIT_PX;
}

static void len_px(SWEEP_CSS_LENGTH* l, int32_t px) {
    l->value_milli = px * 1000;
    l->unit = SWEEP_CSS_UNIT_PX;
}

void sweep_css_style_initial(SWEEP_COMPUTED_STYLE* style) {
    uint32_t i;
    if (!style) return;
    memset(style, 0, sizeof(*style));
    style->display = SWEEP_DISPLAY_INLINE;
    style->position = SWEEP_POSITION_STATIC;
    style->text_align = SWEEP_TEXT_ALIGN_LEFT;
    style->width.unit = SWEEP_CSS_UNIT_AUTO;
    style->height.unit = SWEEP_CSS_UNIT_AUTO;
    for (i = 0u; i < 4u; ++i) {
        len_zero(&style->margin[i]);
        len_zero(&style->padding[i]);
        len_zero(&style->border_width[i]);
    }
    style->color = SWEEP_CSS_COLOR_BLACK;
    style->background_color = SWEEP_CSS_COLOR_TRANSPARENT;
    style->border_color = SWEEP_CSS_COLOR_BLACK;
    style->font_size_px = 16;
    style->font_weight = 400;
    style->font_family[0] = '\0';
}

static bool tag_in(const char* tag, const char* const* list, size_t n) {
    size_t i;
    for (i = 0u; i < n; ++i) {
        if (str_eq_icase(tag, list[i])) return true;
    }
    return false;
}

void sweep_css_style_ua_defaults(const char* tag, SWEEP_COMPUTED_STYLE* style) {
    static const char* const block_tags[] = {
        "html", "body", "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
        "ul", "ol", "li", "section", "article", "header", "footer", "nav",
        "main", "blockquote", "pre", "table", "form", "hr", "figure"
    };
    static const char* const none_tags[] = {
        "head", "title", "meta", "link", "style", "script", "base", "template"
    };
    uint32_t i;

    if (!tag || !style) return;

    if (tag_in(tag, none_tags, sizeof(none_tags) / sizeof(none_tags[0]))) {
        style->display = SWEEP_DISPLAY_NONE;
        return;
    }
    if (tag_in(tag, block_tags, sizeof(block_tags) / sizeof(block_tags[0]))) {
        style->display = SWEEP_DISPLAY_BLOCK;
    }

    if (str_eq_icase(tag, "body")) {
        for (i = 0u; i < 4u; ++i) len_px(&style->margin[i], 8);
    } else if (str_eq_icase(tag, "p") || str_eq_icase(tag, "ul") ||
               str_eq_icase(tag, "ol") || str_eq_icase(tag, "blockquote") ||
               str_eq_icase(tag, "pre") || str_eq_icase(tag, "h1") ||
               str_eq_icase(tag, "h2") || str_eq_icase(tag, "h3") ||
               str_eq_icase(tag, "h4") || str_eq_icase(tag, "h5") ||
               str_eq_icase(tag, "h6")) {
        len_px(&style->margin[SWEEP_SIDE_TOP], 16);
        len_px(&style->margin[SWEEP_SIDE_BOTTOM], 16);
    }

    if (str_eq_icase(tag, "h1")) {
        style->font_size_px = 32; style->font_weight = 700;
    } else if (str_eq_icase(tag, "h2")) {
        style->font_size_px = 24; style->font_weight = 700;
    } else if (str_eq_icase(tag, "h3")) {
        style->font_size_px = 19; style->font_weight = 700;
    } else if (str_eq_icase(tag, "h4") || str_eq_icase(tag, "h5") ||
               str_eq_icase(tag, "h6")) {
        style->font_size_px = 16; style->font_weight = 700;
    } else if (str_eq_icase(tag, "b") || str_eq_icase(tag, "strong")) {
        style->font_weight = 700;
    } else if (str_eq_icase(tag, "a")) {
        style->color = 0xFF0000EEu;
    } else if (str_eq_icase(tag, "pre") || str_eq_icase(tag, "code")) {
        str_copy(style->font_family, sizeof(style->font_family), "monospace");
    }
}

typedef enum _SWEEP_CSS_SLOT {
    SLOT_DISPLAY = 0,
    SLOT_POSITION,
    SLOT_WIDTH,
    SLOT_HEIGHT,
    SLOT_MARGIN_T,
    SLOT_MARGIN_R,
    SLOT_MARGIN_B,
    SLOT_MARGIN_L,
    SLOT_PADDING_T,
    SLOT_PADDING_R,
    SLOT_PADDING_B,
    SLOT_PADDING_L,
    SLOT_BORDER_T,
    SLOT_BORDER_R,
    SLOT_BORDER_B,
    SLOT_BORDER_L,
    SLOT_BORDER_COLOR,
    SLOT_COLOR,
    SLOT_BACKGROUND_COLOR,
    SLOT_FONT_SIZE,
    SLOT_FONT_WEIGHT,
    SLOT_FONT_FAMILY,
    SLOT_TEXT_ALIGN,
    SLOT_COUNT
} SWEEP_CSS_SLOT;

typedef struct _SWEEP_CSS_WIN {
    bool set;
    uint8_t important;
    uint16_t a, b, c;
    uint32_t order;
} SWEEP_CSS_WIN;

typedef struct _SWEEP_CSS_META {
    uint8_t important;
    uint16_t a, b, c;
    uint32_t order;
} SWEEP_CSS_META;

static bool win_consider(SWEEP_CSS_WIN* win, const SWEEP_CSS_META* meta) {
    if (!win->set) {
        win->set = true;
        win->important = meta->important;
        win->a = meta->a; win->b = meta->b; win->c = meta->c;
        win->order = meta->order;
        return true;
    }
    if (meta->important != win->important) return meta->important > win->important;
    if (meta->a != win->a) return meta->a > win->a;
    if (meta->b != win->b) return meta->b > win->b;
    if (meta->c != win->c) return meta->c > win->c;
    if (meta->order < win->order) return false;
    win->order = meta->order;
    win->important = meta->important;
    win->a = meta->a; win->b = meta->b; win->c = meta->c;
    return true;
}

static void style_set_border_width(SWEEP_COMPUTED_STYLE* style, uint32_t side,
                                   const SWEEP_CSS_LENGTH* len) {
    SWEEP_CSS_LENGTH v = *len;
    if (v.unit == SWEEP_CSS_UNIT_AUTO || v.unit == SWEEP_CSS_UNIT_NONE) {
        v.value_milli = 0;
        v.unit = SWEEP_CSS_UNIT_PX;
    }
    style->border_width[side] = v;
}

static int32_t clamp_font_size(int32_t px) {
    if (px < 1) return 1;
    if (px > 512) return 512;
    return px;
}

static int32_t resolve_font_length(const SWEEP_CSS_LENGTH* len,
                                   int32_t parent_font) {
    int64_t v = len->value_milli;
    int64_t base = parent_font > 0 ? parent_font : 16;
    switch (len->unit) {
    case SWEEP_CSS_UNIT_PX: return clamp_font_size((int32_t)(v / 1000));
    case SWEEP_CSS_UNIT_EM: return clamp_font_size((int32_t)(v * base / 1000));
    case SWEEP_CSS_UNIT_REM: return clamp_font_size((int32_t)(v * 16 / 1000));
    case SWEEP_CSS_UNIT_PERCENT: return clamp_font_size((int32_t)(v * base / 100000));
    case SWEEP_CSS_UNIT_PT: return clamp_font_size((int32_t)(v * 96 / 72000));
    default: return 0;
    }
}

static void apply_declaration(SWEEP_COMPUTED_STYLE* style,
                              const SWEEP_CSS_DECL* decl,
                              const SWEEP_COMPUTED_STYLE* parent,
                              SWEEP_CSS_WIN* wins, const SWEEP_CSS_META* meta,
                              SWEEP_CSS_STATS* stats) {
    SWEEP_CSS_TOKEN vt[12];
    size_t vn = 0u;
    const char* name = decl->name;

    if (!value_tokens(decl->value, vt, 12u, &vn) || vn == 0u) {
        if (stats) ++stats->unsupported_values;
        return;
    }

    if (strcmp(name, "display") == 0) {
        if (vn == 1u && vt[0].type == SWEEP_CSS_TOK_IDENT) {
            uint8_t d;
            bool ok = true;
            if (str_eq_icase(vt[0].text, "block")) d = SWEEP_DISPLAY_BLOCK;
            else if (str_eq_icase(vt[0].text, "inline")) d = SWEEP_DISPLAY_INLINE;
            else if (str_eq_icase(vt[0].text, "none")) d = SWEEP_DISPLAY_NONE;
            else if (str_eq_icase(vt[0].text, "inline-block")) d = SWEEP_DISPLAY_INLINE_BLOCK;
            else { d = 0; ok = false; }
            if (ok && win_consider(&wins[SLOT_DISPLAY], meta)) {
                style->display = d;
                if (stats) ++stats->declarations_applied;
            } else if (!ok && stats) ++stats->unsupported_values;
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "position") == 0) {
        if (vn == 1u && vt[0].type == SWEEP_CSS_TOK_IDENT) {
            uint8_t p;
            bool ok = true;
            if (str_eq_icase(vt[0].text, "static")) p = SWEEP_POSITION_STATIC;
            else if (str_eq_icase(vt[0].text, "relative")) p = SWEEP_POSITION_RELATIVE;
            else if (str_eq_icase(vt[0].text, "absolute")) p = SWEEP_POSITION_ABSOLUTE;
            else if (str_eq_icase(vt[0].text, "fixed")) p = SWEEP_POSITION_FIXED;
            else { p = 0; ok = false; }
            if (ok && win_consider(&wins[SLOT_POSITION], meta)) {
                style->position = p;
                if (stats) ++stats->declarations_applied;
            } else if (!ok && stats) ++stats->unsupported_values;
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "width") == 0 || strcmp(name, "height") == 0) {
        SWEEP_CSS_LENGTH len;
        if (parse_one_length(vt, vn, &len)) {
            int slot = (name[0] == 'w') ? SLOT_WIDTH : SLOT_HEIGHT;
            if (win_consider(&wins[slot], meta)) {
                if (name[0] == 'w') style->width = len;
                else style->height = len;
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "margin") == 0 || strcmp(name, "padding") == 0) {
        SWEEP_CSS_LENGTH vals[4];
        bool is_margin = (name[0] == 'm');
        if (parse_length_list(vt, vn, vals)) {
            uint32_t k;
            for (k = 0u; k < 4u; ++k) {
                int slot = (int)(is_margin ? SLOT_MARGIN_T : SLOT_PADDING_T) + (int)k;
                if (!is_margin) {
                    if (vals[k].unit == SWEEP_CSS_UNIT_AUTO) {
                        if (stats) ++stats->unsupported_values;
                        continue;
                    }
                }
                if (win_consider(&wins[slot], meta)) {
                    if (is_margin) style->margin[k] = vals[k];
                    else style->padding[k] = vals[k];
                    if (stats) ++stats->declarations_applied;
                }
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "margin-top") == 0 || strcmp(name, "margin-right") == 0 ||
        strcmp(name, "margin-bottom") == 0 || strcmp(name, "margin-left") == 0 ||
        strcmp(name, "padding-top") == 0 || strcmp(name, "padding-right") == 0 ||
        strcmp(name, "padding-bottom") == 0 || strcmp(name, "padding-left") == 0) {
        SWEEP_CSS_LENGTH len;
        bool is_margin = (strncmp(name, "margin", 6u) == 0);
        uint32_t side = SWEEP_SIDE_TOP;
        int slot;
        if (strstr(name, "-right") != 0) side = SWEEP_SIDE_RIGHT;
        else if (strstr(name, "-bottom") != 0) side = SWEEP_SIDE_BOTTOM;
        else if (strstr(name, "-left") != 0) side = SWEEP_SIDE_LEFT;
        slot = (int)(is_margin ? SLOT_MARGIN_T : SLOT_PADDING_T) + (int)side;
        if (parse_one_length(vt, vn, &len) && !(len.unit == SWEEP_CSS_UNIT_AUTO && !is_margin)) {
            if (win_consider(&wins[slot], meta)) {
                if (is_margin) style->margin[side] = len;
                else style->padding[side] = len;
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "border-width") == 0) {
        SWEEP_CSS_LENGTH vals[4];
        if (parse_length_list(vt, vn, vals)) {
            uint32_t k;
            for (k = 0u; k < 4u; ++k) {
                int slot = (int)SLOT_BORDER_T + (int)k;
                if (win_consider(&wins[slot], meta)) {
                    style_set_border_width(style, k, &vals[k]);
                    if (stats) ++stats->declarations_applied;
                }
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "border-top-width") == 0 || strcmp(name, "border-right-width") == 0 ||
        strcmp(name, "border-bottom-width") == 0 || strcmp(name, "border-left-width") == 0) {
        SWEEP_CSS_LENGTH len;
        uint32_t side = SWEEP_SIDE_TOP;
        int slot;
        if (strstr(name, "-right") != 0) side = SWEEP_SIDE_RIGHT;
        else if (strstr(name, "-bottom") != 0) side = SWEEP_SIDE_BOTTOM;
        else if (strstr(name, "-left") != 0) side = SWEEP_SIDE_LEFT;
        slot = (int)SLOT_BORDER_T + (int)side;
        if (parse_one_length(vt, vn, &len)) {
            if (win_consider(&wins[slot], meta)) {
                style_set_border_width(style, side, &len);
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "border") == 0) {
        SWEEP_CSS_LENGTH widths[4];
        size_t wc = 0u;
        uint32_t color = 0u;
        bool has_color = false;
        size_t i;
        for (i = 0u; i < vn; ++i) {
            SWEEP_CSS_LENGTH len;
            uint32_t c;
            if (length_from_token(&vt[i], &len)) {
                SWEEP_CSS_LENGTH list[4];
                list[0] = len; list[1] = len; list[2] = len; list[3] = len;
                if (wc == 0u) {
                    widths[0] = list[0]; widths[1] = list[1];
                    widths[2] = list[2]; widths[3] = list[3];
                    wc = 1u;
                }
            } else if (!has_color && color_from_token(&vt[i], &c)) {
                color = c;
                has_color = true;
            }
        }
        if (wc > 0u) {
            uint32_t k;
            for (k = 0u; k < 4u; ++k) {
                int slot = (int)SLOT_BORDER_T + (int)k;
                if (win_consider(&wins[slot], meta)) {
                    style_set_border_width(style, k, &widths[k]);
                    if (stats) ++stats->declarations_applied;
                }
            }
        }
        if (has_color && win_consider(&wins[SLOT_BORDER_COLOR], meta)) {
            style->border_color = color;
            if (stats) ++stats->declarations_applied;
        }
        if (wc == 0u && !has_color && stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "border-color") == 0) {
        uint32_t c;
        if (parse_one_color(vt, vn, &c)) {
            if (win_consider(&wins[SLOT_BORDER_COLOR], meta)) {
                style->border_color = c;
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "color") == 0) {
        uint32_t c;
        if (parse_one_color(vt, vn, &c)) {
            if (win_consider(&wins[SLOT_COLOR], meta)) {
                style->color = c;
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "background-color") == 0 || strcmp(name, "background") == 0) {
        uint32_t c;
        bool ok = false;
        size_t i;
        for (i = 0u; i < vn && !ok; ++i) ok = color_from_token(&vt[i], &c);
        if (ok) {
            if (win_consider(&wins[SLOT_BACKGROUND_COLOR], meta)) {
                style->background_color = c;
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "font-size") == 0) {
        int32_t px = 0;
        int32_t parent_font = parent ? parent->font_size_px : style->font_size_px;
        if (vn == 1u && vt[0].type == SWEEP_CSS_TOK_IDENT) {
            if (str_eq_icase(vt[0].text, "inherit")) px = parent_font;
            else if (str_eq_icase(vt[0].text, "initial") || str_eq_icase(vt[0].text, "medium")) px = 16;
            else if (str_eq_icase(vt[0].text, "small")) px = 13;
            else if (str_eq_icase(vt[0].text, "large")) px = 19;
        } else {
            SWEEP_CSS_LENGTH len;
            if (parse_one_length(vt, vn, &len) && len.unit != SWEEP_CSS_UNIT_AUTO) {
                px = resolve_font_length(&len, parent_font);
            }
        }
        if (px > 0) {
            if (win_consider(&wins[SLOT_FONT_SIZE], meta)) {
                style->font_size_px = px;
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "font-weight") == 0) {
        int32_t w = 0;
        if (vn == 1u && vt[0].type == SWEEP_CSS_TOK_IDENT) {
            if (str_eq_icase(vt[0].text, "normal")) w = 400;
            else if (str_eq_icase(vt[0].text, "bold")) w = 700;
        } else if (vn == 1u && vt[0].type == SWEEP_CSS_TOK_NUMBER) {
            int32_t v = vt[0].number_milli / 1000;
            if (v >= 100 && v <= 900) w = v;
        }
        if (w > 0) {
            if (win_consider(&wins[SLOT_FONT_WEIGHT], meta)) {
                style->font_weight = w;
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "font-family") == 0) {
        char fam[SWEEP_CSS_FONT_FAMILY_MAX];
        size_t i = 0u;
        fam[0] = '\0';
        while (i < vn && fam[0] == '\0') {
            if (vt[i].type == SWEEP_CSS_TOK_IDENT) {
                str_copy(fam, sizeof(fam), vt[i].text);
            } else if (vt[i].type == SWEEP_CSS_TOK_STRING) {
                const char* s = vt[i].text;
                size_t m = strlen(s);
                if (m >= 2u && (s[0] == '"' || s[0] == '\'') && s[m - 1u] == s[0]) {
                    char tmp[SWEEP_CSS_FONT_FAMILY_MAX];
                    size_t k;
                    m -= 2u;
                    if (m >= sizeof(tmp)) m = sizeof(tmp) - 1u;
                    for (k = 0u; k < m; ++k) tmp[k] = s[1u + k];
                    tmp[m] = '\0';
                    str_copy(fam, sizeof(fam), tmp);
                } else {
                    str_copy(fam, sizeof(fam), s);
                }
            }
            ++i;
        }
        if (fam[0] != '\0') {
            if (win_consider(&wins[SLOT_FONT_FAMILY], meta)) {
                str_copy(style->font_family, sizeof(style->font_family), fam);
                if (stats) ++stats->declarations_applied;
            }
        } else if (stats) ++stats->unsupported_values;
        return;
    }
    if (strcmp(name, "text-align") == 0) {
        if (vn == 1u && vt[0].type == SWEEP_CSS_TOK_IDENT) {
            uint8_t a;
            bool ok = true;
            if (str_eq_icase(vt[0].text, "left")) a = SWEEP_TEXT_ALIGN_LEFT;
            else if (str_eq_icase(vt[0].text, "center")) a = SWEEP_TEXT_ALIGN_CENTER;
            else if (str_eq_icase(vt[0].text, "right")) a = SWEEP_TEXT_ALIGN_RIGHT;
            else if (str_eq_icase(vt[0].text, "justify")) a = SWEEP_TEXT_ALIGN_JUSTIFY;
            else { a = 0; ok = false; }
            if (ok && win_consider(&wins[SLOT_TEXT_ALIGN], meta)) {
                style->text_align = a;
                if (stats) ++stats->declarations_applied;
            } else if (!ok && stats) ++stats->unsupported_values;
        } else if (stats) ++stats->unsupported_values;
        return;
    }

    /* Unknown or unimplemented property: ignore safely. */
    if (stats) ++stats->declarations_ignored;
}

static void inherit_style(SWEEP_COMPUTED_STYLE* style,
                          const SWEEP_COMPUTED_STYLE* parent) {
    if (!parent) return;
    style->color = parent->color;
    style->font_size_px = parent->font_size_px;
    style->font_weight = parent->font_weight;
    style->text_align = parent->text_align;
    str_copy(style->font_family, sizeof(style->font_family), parent->font_family);
}

static bool compound_matches_subject_only(const SWEEP_DOM_NODE* node,
                                           const SWEEP_CSS_COMPOUND* comp) {
    uint32_t k;
    if (!node || node->type != SWEEP_DOM_ELEMENT) return false;
    for (k = 0u; k < comp->simple_count; ++k) {
        const SWEEP_CSS_SIMPLE* s = &comp->simples[k];
        switch (s->kind) {
        case SWEEP_CSS_SIMPLE_UNIVERSAL:
            break;
        case SWEEP_CSS_SIMPLE_TYPE:
            if (!str_eq_icase(node->name, s->name)) return false;
            break;
        case SWEEP_CSS_SIMPLE_CLASS:
            if (!node_has_class(node, s->name)) return false;
            break;
        case SWEEP_CSS_SIMPLE_ID: {
            const char* id = sweep_dom_get_attr(node, "id");
            if (!id || strcmp(id, s->name) != 0) return false;
            break;
        }
        case SWEEP_CSS_SIMPLE_ATTR_EXISTS:
            if (!sweep_dom_get_attr(node, s->name)) return false;
            break;
        case SWEEP_CSS_SIMPLE_ATTR_EQUALS: {
            const char* v = sweep_dom_get_attr(node, s->name);
            if (!v || strcmp(v, s->value) != 0) return false;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

static bool selector_subject_may_match(const SWEEP_DOM_NODE* node,
                                       const SWEEP_CSS_SELECTOR* sel) {
    if (!sel || sel->unsupported || sel->part_count == 0u) return false;
    /* Subject is the rightmost compound; for structural selectors ancestors
     * still matter, but the subject itself must satisfy its compound. */
    return compound_matches_subject_only(node,
                                         &sel->parts[sel->part_count - 1u]);
}

static bool rule_candidate_for_node(const SWEEP_DOM_NODE* node,
                                    const SWEEP_CSS_RULE* rule,
                                    const SWEEP_CSS_DEP_INDEX* index,
                                    uint16_t rule_index) {
    uint8_t s;
    if (!rule) return false;
    if (rule->selector_count == 0u) return false;
    if (index) {
        if (rule_index < SWEEP_CSS_MAX_RULES && index->rule_needs_tree[rule_index]) {
            return true; /* structural/universal: cannot filter by subject alone */
        }
        if (index->types.overflow || index->classes.overflow ||
            index->ids.overflow || index->attrs.overflow) {
            /* Index incomplete: stay conservative. */
        }
    } else {
        /* No index: conservative (full evaluation). */
        return true;
    }
    /* Non-structural rule: any selector whose subject compound cannot match
     * this node can be skipped; the rule is a candidate if any selector's
     * subject may match. */
    for (s = 0u; s < rule->selector_count; ++s) {
        if (selector_subject_may_match(node, &rule->selectors[s])) return true;
    }
    return false;
}

static void apply_cascade_indexed(const SWEEP_DOM* dom, uint32_t node_id,
                                  const SWEEP_CSS_STYLESHEET* sheet,
                                  const SWEEP_CSS_DEP_INDEX* index,
                                  SWEEP_COMPUTED_STYLE* style,
                                  const SWEEP_COMPUTED_STYLE* parent,
                                  SWEEP_CSS_STATS* stats) {
    SWEEP_CSS_WIN wins[SLOT_COUNT];
    uint32_t order = 0u;
    uint16_t r;
    const SWEEP_DOM_NODE* node = sweep_dom_get(dom, node_id);

    memset(wins, 0, sizeof(wins));
    if (!node) return;

    for (r = 0u; r < sheet->rule_count; ++r) {
        const SWEEP_CSS_RULE* rule = &sheet->rules[r];
        uint8_t s;
        if (index && node->type == SWEEP_DOM_ELEMENT &&
            !rule_candidate_for_node(node, rule, index, r)) {
            if (stats) ++stats->rules_skipped;
            continue;
        }
        for (s = 0u; s < rule->selector_count; ++s) {
            const SWEEP_CSS_SELECTOR* sel = &rule->selectors[s];
            uint8_t d;
            if (stats) ++stats->selector_tests;
            if (!sweep_css_selector_matches(dom, node_id, sel)) continue;
            if (stats) ++stats->rules_matched;
            for (d = 0u; d < rule->decl_count; ++d) {
                SWEEP_CSS_META meta;
                meta.important = rule->decls[d].important ? 1u : 0u;
                meta.a = sel->spec_a;
                meta.b = sel->spec_b;
                meta.c = sel->spec_c;
                meta.order = order++;
                apply_declaration(style, &rule->decls[d], parent, wins, &meta, stats);
            }
        }
    }

    /* Inline style: author origin with specificity above any selector. */
    {
        const char* inline_style = sweep_dom_get_attr(node, "style");
        if (inline_style && inline_style[0] != '\0') {
            SWEEP_CSS_DECL decls[SWEEP_CSS_MAX_DECLS];
            size_t dn = 0u;
            size_t i;
            if (sweep_css_parse_declarations(inline_style, strlen(inline_style),
                                             decls, SWEEP_CSS_MAX_DECLS, &dn)) {
                for (i = 0u; i < dn; ++i) {
                    SWEEP_CSS_META meta;
                    meta.important = decls[i].important ? 1u : 0u;
                    meta.a = 1000u;
                    meta.b = 0u;
                    meta.c = 0u;
                    meta.order = order++;
                    apply_declaration(style, &decls[i], parent, wins, &meta, stats);
                }
            }
        }
    }
}

static uint32_t compute_node_indexed(const SWEEP_DOM* dom,
                                     const SWEEP_CSS_STYLESHEET* sheet,
                                     const SWEEP_CSS_DEP_INDEX* index,
                                     uint32_t node_id,
                                     const SWEEP_COMPUTED_STYLE* parent,
                                     SWEEP_COMPUTED_STYLE* styles, size_t out_cap,
                                     SWEEP_CSS_STATS* stats) {
    const SWEEP_DOM_NODE* node;
    SWEEP_COMPUTED_STYLE* style;
    uint32_t i;
    uint32_t count = 0u;

    if (node_id == 0u || node_id > out_cap) return 0u;
    node = sweep_dom_get(dom, node_id);
    if (!node) return 0u;
    style = &styles[node_id - 1u];

    sweep_css_style_initial(style);
    inherit_style(style, parent);

    if (node->type == SWEEP_DOM_ELEMENT) {
        sweep_css_style_ua_defaults(node->name, style);
        apply_cascade_indexed(dom, node_id, sheet, index, style, parent, stats);
    } else if (node->type == SWEEP_DOM_DOCUMENT) {
        style->display = SWEEP_DISPLAY_BLOCK;
    } else {
        style->display = SWEEP_DISPLAY_INLINE;
    }
    ++count;
    if (stats) ++stats->nodes_recomputed;

    for (i = 0u; i < node->child_count; ++i) {
        count += compute_node_indexed(dom, sheet, index, node->children[i], style,
                                      styles, out_cap, stats);
    }
    return count;
}

uint32_t sweep_css_compute_all(const SWEEP_DOM* dom,
                               const SWEEP_CSS_STYLESHEET* sheet,
                               SWEEP_COMPUTED_STYLE* out, size_t out_cap,
                               SWEEP_CSS_STATS* stats) {
    return sweep_css_compute_all_indexed(dom, sheet, (const SWEEP_CSS_DEP_INDEX*)0,
                                         out, out_cap, stats);
}

uint32_t sweep_css_compute_all_indexed(const SWEEP_DOM* dom,
                                       const SWEEP_CSS_STYLESHEET* sheet,
                                       const SWEEP_CSS_DEP_INDEX* index,
                                       SWEEP_COMPUTED_STYLE* out,
                                       size_t out_cap,
                                       SWEEP_CSS_STATS* stats) {
    if (!dom || !sheet || !out || out_cap == 0u) return 0u;
    if (dom->document == 0u || dom->document > out_cap) return 0u;
    if (stats) memset(stats, 0, sizeof(*stats));
    return compute_node_indexed(dom, sheet, index, dom->document,
                                (const SWEEP_COMPUTED_STYLE*)0,
                                out, out_cap, stats);
}

uint32_t sweep_css_recompute_subtree_indexed(const SWEEP_DOM* dom,
                                             const SWEEP_CSS_STYLESHEET* sheet,
                                             const SWEEP_CSS_DEP_INDEX* index,
                                             uint32_t node_id,
                                             const SWEEP_COMPUTED_STYLE* parent_style,
                                             SWEEP_COMPUTED_STYLE* styles,
                                             size_t out_cap,
                                             SWEEP_CSS_STATS* stats) {
    if (!dom || !sheet || !styles || out_cap == 0u || node_id == 0u) return 0u;
    if (stats) memset(stats, 0, sizeof(*stats));
    return compute_node_indexed(dom, sheet, index, node_id, parent_style,
                                styles, out_cap, stats);
}

int32_t sweep_css_length_resolve_px(const SWEEP_CSS_LENGTH* length,
                                    int32_t base_px, int32_t font_px) {
    int64_t v;
    int64_t base = base_px;
    int64_t font = font_px > 0 ? font_px : 16;
    if (!length) return 0;
    v = length->value_milli;
    switch (length->unit) {
    case SWEEP_CSS_UNIT_PX: return (int32_t)(v / 1000);
    case SWEEP_CSS_UNIT_EM: return (int32_t)(v * font / 1000);
    case SWEEP_CSS_UNIT_REM: return (int32_t)(v * 16 / 1000);
    case SWEEP_CSS_UNIT_PERCENT: return (int32_t)(v * base / 100000);
    case SWEEP_CSS_UNIT_PT: return (int32_t)(v * 96 / 72000);
    default: return 0;
    }
}

/* Deterministic temporary measurement; see header. */
int32_t sweep_text_measure_px(const char* text, int32_t font_px,
                              SWEEP_TEXT_MODE mode) {
    sweep_cp_t cps[SWEEP_PAINT_TEXT_CP_MAX];
    size_t n = 0u;
    size_t i;
    int64_t width = 0;
    int32_t font = font_px > 0 ? font_px : 16;

    if (!text || text[0] == '\0') return 0;
    (void)sweep_utf8_decode(mode, (const uint8_t*)text, strlen(text), cps,
                            SWEEP_PAINT_TEXT_CP_MAX, &n);
    for (i = 0u; i < n; ++i) {
        sweep_cp_t cp = cps[i];
        if (cp == (sweep_cp_t)' ') width += font / 4;
        else if (cp < 0x80u) width += font / 2;
        else width += font;
    }
    if (width > 2147483647LL) width = 2147483647LL;
    return (int32_t)width;
}

/* ======================================================================= */
/* Stylesheet composition and selector dependency index (Phase 5)         */
/* ======================================================================= */

bool sweep_css_stylesheet_append(SWEEP_CSS_STYLESHEET* dst,
                                 const SWEEP_CSS_STYLESHEET* src) {
    uint16_t i;
    if (!dst || !src) return false;
    for (i = 0u; i < src->rule_count; ++i) {
        if (dst->rule_count >= SWEEP_CSS_MAX_RULES) {
            dst->truncated = true;
            break;
        }
        dst->rules[dst->rule_count++] = src->rules[i];
    }
    if (src->truncated) dst->truncated = true;
    dst->error_count = (uint16_t)(dst->error_count + src->error_count);
    return true;
}

static void dep_set_add(SWEEP_CSS_DEP_SET* set, const char* name) {
    uint8_t i;
    if (!set || !name || name[0] == '\0') return;
    for (i = 0u; i < set->count; ++i) {
        if (strcmp(set->names[i], name) == 0) return;
    }
    if (set->count >= SWEEP_CSS_DEP_KEY_MAX) {
        set->overflow = true;
        return;
    }
    str_copy(set->names[set->count], sizeof(set->names[set->count]), name);
    set->count++;
}

static bool dep_set_has(const SWEEP_CSS_DEP_SET* set, const char* name) {
    uint8_t i;
    if (!set || !name) return false;
    if (set->overflow) return true; /* conservative: treat as present */
    for (i = 0u; i < set->count; ++i) {
        if (strcmp(set->names[i], name) == 0) return true;
    }
    return false;
}

static void dep_index_rule(SWEEP_CSS_DEP_INDEX* idx, const SWEEP_CSS_RULE* rule,
                           uint16_t rule_index) {
    uint8_t s;
    bool needs_tree = false;
    if (!idx || !rule) return;
    for (s = 0u; s < rule->selector_count; ++s) {
        const SWEEP_CSS_SELECTOR* sel = &rule->selectors[s];
        uint8_t p;
        if (sel->unsupported) continue;
        idx->selector_count++;
        if (sel->part_count > 1u) needs_tree = true;
        for (p = 0u; p < sel->part_count; ++p) {
            const SWEEP_CSS_COMPOUND* comp = &sel->parts[p];
            uint8_t k;
            if (comp->combinator == SWEEP_CSS_COMB_DESCENDANT) {
                idx->has_descendant = true;
                idx->has_structural = true;
                needs_tree = true;
            } else if (comp->combinator == SWEEP_CSS_COMB_CHILD) {
                idx->has_child = true;
                idx->has_structural = true;
                needs_tree = true;
            }
            for (k = 0u; k < comp->simple_count; ++k) {
                const SWEEP_CSS_SIMPLE* sim = &comp->simples[k];
                switch (sim->kind) {
                case SWEEP_CSS_SIMPLE_UNIVERSAL:
                    idx->has_universal = true;
                    needs_tree = true;
                    break;
                case SWEEP_CSS_SIMPLE_TYPE:
                    idx->has_type = true;
                    dep_set_add(&idx->types, sim->name);
                    break;
                case SWEEP_CSS_SIMPLE_CLASS:
                    idx->has_class = true;
                    dep_set_add(&idx->classes, sim->name);
                    break;
                case SWEEP_CSS_SIMPLE_ID:
                    idx->has_id = true;
                    dep_set_add(&idx->ids, sim->name);
                    break;
                case SWEEP_CSS_SIMPLE_ATTR_EXISTS:
                    idx->has_attr = true;
                    dep_set_add(&idx->attrs, sim->name);
                    break;
                case SWEEP_CSS_SIMPLE_ATTR_EQUALS: {
                    char pair[SWEEP_CSS_NAME_MAX + SWEEP_CSS_ATTR_VALUE_MAX + 2u];
                    idx->has_attr = true;
                    dep_set_add(&idx->attrs, sim->name);
                    /* pair key: name=value (truncated deterministically) */
                    pair[0] = '\0';
                    str_copy(pair, sizeof(pair), sim->name);
                    (void) buf_putc(pair, sizeof(pair), '=');
                    (void) buf_puts(pair, sizeof(pair), sim->value);
                    dep_set_add(&idx->attr_pairs, pair);
                    break;
                }
                default:
                    needs_tree = true; /* unknown: conservative */
                    break;
                }
            }
        }
    }
    if (rule_index < SWEEP_CSS_MAX_RULES) {
        idx->rule_needs_tree[rule_index] = needs_tree ? 1u : 0u;
    }
}

void sweep_css_dep_index_build(const SWEEP_CSS_STYLESHEET* sheet,
                               SWEEP_CSS_DEP_INDEX* out) {
    uint16_t i;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sheet) return;
    out->rule_count = sheet->rule_count;
    for (i = 0u; i < sheet->rule_count; ++i) {
        dep_index_rule(out, &sheet->rules[i], i);
    }
}

bool sweep_css_dep_mutation_affects_style(const SWEEP_CSS_DEP_INDEX* index,
                                          SWEEP_CSS_MUTATION_KIND kind,
                                          const char* attr_name,
                                          bool class_changed,
                                          bool id_changed) {
    if (!index) return true; /* no index: conservative */
    switch (kind) {
    case SWEEP_CSS_MUT_NONE:
        return false;
    case SWEEP_CSS_MUT_TEXT:
        /* Text content does not affect selector matching; only layout/paint.
         * (Stylesheet text inside <style> is reported as STYLESHEET/ATTR on the
         * element by the document layer, not as TEXT.) */
        return false;
    case SWEEP_CSS_MUT_STYLESHEET:
        return true; /* author rules changed: recompute required */
    case SWEEP_CSS_MUT_INSERT:
    case SWEEP_CSS_MUT_REMOVE:
    case SWEEP_CSS_MUT_REORDER:
        /* Structural changes can satisfy or break type/class/id/attr and any
         * descendant/child selector.  Without per-element proof, be
         * conservative whenever any rule exists. */
        return index->rule_count > 0u || index->selector_count > 0u;
    case SWEEP_CSS_MUT_ATTR:
        /* Inline style changes computed values directly (cascade applies inline
         * after rules) so they always require recomputation.  Class/id are
         * attribute mutations in the DOM; when structural/universal selectors
         * exist, changing them can affect descendants/ancestors, so be
         * conservative.  For a plain attribute, only rules referencing that
         * attribute name (or universal/structural) can be affected. */
        if (attr_name && strcmp(attr_name, "style") == 0) return true;
        if (index->has_universal || index->has_structural) return true;
        if (class_changed) {
            /* If no rule ever references any class, class changes are safe to
             * ignore for author-style matching (UA/inline unaffected). */
            return index->has_class;
        }
        if (id_changed) {
            return index->has_id;
        }
        if (attr_name && attr_name[0] != '\0') {
            if (index->attrs.overflow) return true;
            return dep_set_has(&index->attrs, attr_name);
        }
        /* Unknown attribute mutation: conservative. */
        return true;
    default:
        return true;
    }
}

/* ======================================================================= */
/* Layout: block flow with inline text placement                           */
/* ======================================================================= */

static int32_t clamp_px(int32_t v) {
    if (v < 0) return 0;
    if (v > SWEEP_LAYOUT_MAX_PX) return SWEEP_LAYOUT_MAX_PX;
    return v;
}

static int32_t add_clamped(int32_t a, int32_t b) {
    int64_t v = (int64_t)a + (int64_t)b;
    if (v > 2147483000LL) v = 2147483000LL;
    if (v < -2147483000LL) v = -2147483000LL;
    return (int32_t)v;
}

/* Height percentages need a containing-block height, which this phase does not
 * thread through the layout.  Treat them (and AUTO/unsupported units) as AUTO
 * instead of silently resolving them against the width. */
static bool length_is_auto_height(const SWEEP_CSS_LENGTH* l) {
    return l->unit == SWEEP_CSS_UNIT_AUTO || l->unit == SWEEP_CSS_UNIT_NONE ||
           l->unit == SWEEP_CSS_UNIT_PERCENT;
}

static const SWEEP_COMPUTED_STYLE* style_of(const SWEEP_COMPUTED_STYLE* styles,
                                            size_t style_count,
                                            uint32_t node_id) {
    if (!styles || node_id == 0u || (size_t)node_id > style_count) return 0;
    return &styles[node_id - 1u];
}

static void resolve_edges(const SWEEP_COMPUTED_STYLE* st, int32_t base_w,
                          int32_t m[4], int32_t p[4], int32_t b[4]) {
    uint32_t i;
    for (i = 0u; i < 4u; ++i) {
        int32_t mv = st ? sweep_css_length_resolve_px(&st->margin[i], base_w,
                                                      st->font_size_px) : 0;
        int32_t pv = st ? sweep_css_length_resolve_px(&st->padding[i], base_w,
                                                      st->font_size_px) : 0;
        int32_t bv = st ? sweep_css_length_resolve_px(&st->border_width[i],
                                                      base_w,
                                                      st->font_size_px) : 0;
        m[i] = clamp_px(mv); /* negative margins not supported in this phase */
        p[i] = clamp_px(pv);
        b[i] = clamp_px(bv);
    }
}

static uint32_t layout_new_box(SWEEP_LAYOUT* L) {
    uint32_t idx;
    if (!L || L->count >= SWEEP_LAYOUT_MAX_BOXES) {
        if (L) L->truncated = true;
        return 0u;
    }
    idx = L->count + 1u;
    memset(&L->boxes[idx - 1u], 0, sizeof(L->boxes[idx - 1u]));
    L->boxes[idx - 1u].in_use = true;
    L->count = idx;
    return idx;
}

static void layout_link_child(SWEEP_LAYOUT* L, uint32_t parent, uint32_t child) {
    SWEEP_LAYOUT_BOX* p;
    SWEEP_LAYOUT_BOX* c;
    if (!L || parent == 0u || child == 0u) return;
    p = &L->boxes[parent - 1u];
    c = &L->boxes[child - 1u];
    c->parent = parent;
    if (p->first_child == 0u) {
        p->first_child = child;
    } else {
        uint32_t sib = p->first_child;
        while (L->boxes[sib - 1u].next_sibling != 0u) {
            sib = L->boxes[sib - 1u].next_sibling;
        }
        L->boxes[sib - 1u].next_sibling = child;
    }
}

static void layout_fill_box_style(SWEEP_LAYOUT_BOX* box,
                                  const SWEEP_COMPUTED_STYLE* st) {
    if (!box || !st) return;
    box->display = st->display;
    box->background_color = st->background_color;
    box->border_color = st->border_color;
    box->color = st->color;
    box->font_size_px = st->font_size_px;
    box->text_align = st->text_align;
}

/* Forward declarations. */
static int32_t layout_block_node(SWEEP_LAYOUT* L, const SWEEP_DOM* dom,
                                 const SWEEP_COMPUTED_STYLE* styles,
                                 size_t style_count, uint32_t node_id,
                                 uint32_t parent_box, int32_t margin_x,
                                 int32_t margin_y, int32_t avail_w,
                                 SWEEP_TEXT_MODE mode, uint32_t depth);

static int32_t layout_inline_children(SWEEP_LAYOUT* L, const SWEEP_DOM* dom,
                                      const SWEEP_COMPUTED_STYLE* styles,
                                      size_t style_count, uint32_t dom_parent,
                                      uint32_t parent_box, int32_t content_x,
                                      int32_t content_y, int32_t content_w,
                                      SWEEP_TEXT_MODE mode, uint32_t depth,
                                      bool allow_wrap, int32_t* out_bottom);

/* Lay out one text node as wrapped word boxes.  Updates *io_pen_x,
 * *io_line_top and *io_line_h.  Returns false only on storage exhaustion. */
static bool layout_text_node(SWEEP_LAYOUT* L, const SWEEP_DOM* dom,
                             const SWEEP_COMPUTED_STYLE* st, uint32_t node_id,
                             uint32_t parent_box, int32_t left, int32_t right,
                             int32_t* io_pen_x, int32_t* io_line_top,
                             int32_t* io_line_h, SWEEP_TEXT_MODE mode) {
    const SWEEP_DOM_NODE* node = sweep_dom_get(dom, node_id);
    const char* text;
    size_t len;
    size_t pos = 0u;
    int32_t font = st ? st->font_size_px : 16;
    int32_t line_h = font + font / 4;
    char word[SWEEP_HTML_TEXT_MAX];
    bool pending_space = false;

    if (!node || node->type != SWEEP_DOM_TEXT || !L) return true;
    text = node->text;
    len = strlen(text);

    while (pos < len) {
        size_t wstart;
        size_t wend;
        size_t wlen;
        int32_t word_w;
        int32_t need;
        uint32_t box;
        SWEEP_LAYOUT_BOX* b;

        while (pos < len) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                pending_space = true;
                ++pos;
            } else {
                break;
            }
        }
        if (pos >= len) break;

        wstart = pos;
        while (pos < len) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') break;
            ++pos;
        }
        wend = pos;
        wlen = wend - wstart;
        if (wlen >= sizeof(word)) wlen = sizeof(word) - 1u;
        memcpy(word, text + wstart, wlen);
        word[wlen] = '\0';
        word_w = sweep_text_measure_px(word, font, mode);

        need = word_w;
        if (pending_space && *io_pen_x > left) need += font / 4;

        if (*io_pen_x > left && *io_pen_x + need > right) {
            *io_line_top = add_clamped(*io_line_top, *io_line_h);
            *io_pen_x = left;
            *io_line_h = 0;
            pending_space = false;
            need = word_w;
        }
        if (pending_space && *io_pen_x > left) {
            *io_pen_x = add_clamped(*io_pen_x, font / 4);
        }
        pending_space = false;
        (void)need;

        box = layout_new_box(L);
        if (box == 0u) return false;
        layout_link_child(L, parent_box, box);
        b = &L->boxes[box - 1u];
        b->node = node_id;
        b->display = SWEEP_DISPLAY_INLINE;
        layout_fill_box_style(b, st);
        b->x = *io_pen_x;
        b->y = *io_line_top;
        b->width = word_w;
        b->height = font;
        b->content_x = b->x;
        b->content_y = b->y;
        b->content_w = word_w;
        b->content_h = font;
        b->text_offset = (uint32_t)wstart;
        b->text_len = (uint32_t)(wend - wstart);
        *io_pen_x = add_clamped(*io_pen_x, word_w);
        if (line_h > *io_line_h) *io_line_h = line_h;
    }
    return true;
}

/* Natural inline width: text content measurement plus padding/border/margins.
 * This is a deterministic approximation, not font shaping. */
static int32_t inline_natural_width(const SWEEP_DOM* dom,
                                    const SWEEP_COMPUTED_STYLE* styles,
                                    size_t style_count, uint32_t node_id,
                                    SWEEP_TEXT_MODE mode, uint32_t depth) {
    const SWEEP_DOM_NODE* node = sweep_dom_get(dom, node_id);
    const SWEEP_COMPUTED_STYLE* st;
    int32_t font;
    int32_t total;

    if (!node || depth > SWEEP_LAYOUT_MAX_DEPTH) return 0;
    st = style_of(styles, style_count, node_id);
    font = st ? st->font_size_px : 16;

    if (node->type == SWEEP_DOM_TEXT) {
        return sweep_text_measure_px(node->text, font, mode);
    }
    if (node->type == SWEEP_DOM_ELEMENT) {
        char buf[SWEEP_HTML_TEXT_MAX];
        uint32_t i;
        if (st && st->width.unit != SWEEP_CSS_UNIT_AUTO) {
            return clamp_px(sweep_css_length_resolve_px(&st->width, 0, font));
        }
        buf[0] = '\0';
        total = 0;
        for (i = 0u; i < node->child_count; ++i) {
            const SWEEP_COMPUTED_STYLE* cs =
                style_of(styles, style_count, node->children[i]);
            if (cs && cs->display == SWEEP_DISPLAY_NONE) continue;
            total = add_clamped(total, inline_natural_width(dom, styles,
                                                           style_count,
                                                           node->children[i],
                                                           mode, depth + 1u));
        }
        if (total == 0) {
            sweep_dom_text_content(dom, node_id, buf, sizeof(buf));
            total = sweep_text_measure_px(buf, font, mode);
        }
        return total;
    }
    return 0;
}

/* Lay out an inline element atomically (its descendants use inline flow). */
static int32_t layout_inline_element(SWEEP_LAYOUT* L, const SWEEP_DOM* dom,
                                     const SWEEP_COMPUTED_STYLE* styles,
                                     size_t style_count, uint32_t node_id,
                                     uint32_t parent_box, int32_t content_x,
                                     int32_t content_y, int32_t content_w,
                                     int32_t* io_pen_x, int32_t* io_line_top,
                                     int32_t* io_line_h, bool allow_wrap,
                                     SWEEP_TEXT_MODE mode, uint32_t depth) {
    const SWEEP_COMPUTED_STYLE* st = style_of(styles, style_count, node_id);
    const SWEEP_DOM_NODE* node = sweep_dom_get(dom, node_id);
    int32_t m[4], p[4], b[4];
    int32_t natural_cw;
    int32_t cw;
    int32_t border_w;
    int32_t total_w;
    int32_t font;
    int32_t child_bottom = 0;
    uint32_t box;
    SWEEP_LAYOUT_BOX* bx;

    if (!L || !node || !st) return 0;
    if (depth > SWEEP_LAYOUT_MAX_DEPTH) {
        L->truncated = true;
        return 0;
    }
    font = st->font_size_px;
    resolve_edges(st, content_w, m, p, b);

    natural_cw = inline_natural_width(dom, styles, style_count, node_id, mode,
                                      depth + 1u);
    if (st->width.unit != SWEEP_CSS_UNIT_AUTO) {
        cw = clamp_px(sweep_css_length_resolve_px(&st->width, content_w, font));
    } else {
        cw = clamp_px(natural_cw);
    }
    if (cw < 1) cw = 1;
    border_w = clamp_px(cw + p[SWEEP_SIDE_LEFT] + p[SWEEP_SIDE_RIGHT] +
                        b[SWEEP_SIDE_LEFT] + b[SWEEP_SIDE_RIGHT]);
    total_w = clamp_px(border_w + m[SWEEP_SIDE_LEFT] + m[SWEEP_SIDE_RIGHT]);

    if (allow_wrap && *io_pen_x > content_x && *io_pen_x + total_w > content_x + content_w) {
        *io_line_top = add_clamped(*io_line_top, *io_line_h);
        *io_pen_x = content_x;
        *io_line_h = 0;
    }

    box = layout_new_box(L);
    if (box == 0u) return 0;
    layout_link_child(L, parent_box, box);
    bx = &L->boxes[box - 1u];
    bx->node = node_id;
    bx->display = SWEEP_DISPLAY_INLINE;
    layout_fill_box_style(bx, st);
    bx->x = add_clamped(*io_pen_x, m[SWEEP_SIDE_LEFT]);
    bx->y = *io_line_top;
    bx->width = border_w;
    bx->content_x = bx->x + b[SWEEP_SIDE_LEFT] + p[SWEEP_SIDE_LEFT];
    bx->content_y = bx->y + b[SWEEP_SIDE_TOP] + p[SWEEP_SIDE_TOP];
    bx->content_w = cw;

    if (!layout_inline_children(L, dom, styles, style_count, node_id, box,
                                bx->content_x, bx->content_y, cw, mode,
                                depth + 1u, false, &child_bottom)) {
        L->truncated = true;
    }
    if (!length_is_auto_height(&st->height)) {
        bx->content_h = clamp_px(sweep_css_length_resolve_px(&st->height,
                                                             content_w, font));
    } else {
        bx->content_h = clamp_px(child_bottom - bx->content_y);
    }
    bx->height = clamp_px(bx->content_h + p[SWEEP_SIDE_TOP] +
                          p[SWEEP_SIDE_BOTTOM] + b[SWEEP_SIDE_TOP] +
                          b[SWEEP_SIDE_BOTTOM]);
    bx->margin[SWEEP_SIDE_TOP] = m[SWEEP_SIDE_TOP];
    bx->margin[SWEEP_SIDE_RIGHT] = m[SWEEP_SIDE_RIGHT];
    bx->margin[SWEEP_SIDE_BOTTOM] = m[SWEEP_SIDE_BOTTOM];
    bx->margin[SWEEP_SIDE_LEFT] = m[SWEEP_SIDE_LEFT];
    bx->padding[SWEEP_SIDE_TOP] = p[SWEEP_SIDE_TOP];
    bx->padding[SWEEP_SIDE_RIGHT] = p[SWEEP_SIDE_RIGHT];
    bx->padding[SWEEP_SIDE_BOTTOM] = p[SWEEP_SIDE_BOTTOM];
    bx->padding[SWEEP_SIDE_LEFT] = p[SWEEP_SIDE_LEFT];
    bx->border[SWEEP_SIDE_TOP] = b[SWEEP_SIDE_TOP];
    bx->border[SWEEP_SIDE_RIGHT] = b[SWEEP_SIDE_RIGHT];
    bx->border[SWEEP_SIDE_BOTTOM] = b[SWEEP_SIDE_BOTTOM];
    bx->border[SWEEP_SIDE_LEFT] = b[SWEEP_SIDE_LEFT];

    *io_pen_x = add_clamped(*io_pen_x, total_w);
    if (add_clamped(m[SWEEP_SIDE_TOP], add_clamped(bx->height,
                                                   m[SWEEP_SIDE_BOTTOM])) >
        *io_line_h) {
        *io_line_h = add_clamped(m[SWEEP_SIDE_TOP],
                                 add_clamped(bx->height, m[SWEEP_SIDE_BOTTOM]));
    }
    (void)content_y;
    return 1;
}

static int32_t layout_inline_children(SWEEP_LAYOUT* L, const SWEEP_DOM* dom,
                                      const SWEEP_COMPUTED_STYLE* styles,
                                      size_t style_count, uint32_t dom_parent,
                                      uint32_t parent_box, int32_t content_x,
                                      int32_t content_y, int32_t content_w,
                                      SWEEP_TEXT_MODE mode, uint32_t depth,
                                      bool allow_wrap, int32_t* out_bottom) {
    const SWEEP_DOM_NODE* parent = sweep_dom_get(dom, dom_parent);
    int32_t pen_x = content_x;
    int32_t line_top = content_y;
    int32_t line_h = 0;
    uint32_t i;

    if (out_bottom) *out_bottom = content_y;
    if (!parent) return 0;
    if (depth > SWEEP_LAYOUT_MAX_DEPTH) {
        if (L) L->truncated = true;
        return 0;
    }

    for (i = 0u; i < parent->child_count; ++i) {
        uint32_t child_id = parent->children[i];
        const SWEEP_DOM_NODE* child = sweep_dom_get(dom, child_id);
        const SWEEP_COMPUTED_STYLE* st = style_of(styles, style_count, child_id);
        if (!child) continue;
        if (st && st->display == SWEEP_DISPLAY_NONE) continue;

        if (child->type == SWEEP_DOM_TEXT) {
            if (!layout_text_node(L, dom, st, child_id, parent_box, content_x,
                                  content_x + content_w, &pen_x, &line_top,
                                  &line_h, mode)) {
                L->truncated = true;
            }
        } else if (child->type == SWEEP_DOM_ELEMENT) {
            if (!layout_inline_element(L, dom, styles, style_count, child_id,
                                       parent_box, content_x, content_y,
                                       content_w, &pen_x, &line_top, &line_h,
                                       allow_wrap, mode, depth + 1u)) {
                L->truncated = true;
            }
        } else {
            /* comments are not rendered */
        }
    }

    if (line_h > 0) line_top = add_clamped(line_top, line_h);
    if (out_bottom) *out_bottom = line_top;
    return 1;
}

/* Mixed block/inline flow inside a block container.  Inline runs are placed
 * into lines; block children stack vertically.  This is the single entry
 * point shared by the document root and every block element. */
static int32_t layout_flow_children(SWEEP_LAYOUT* L, const SWEEP_DOM* dom,
                                    const SWEEP_COMPUTED_STYLE* styles,
                                    size_t style_count, uint32_t dom_parent,
                                    uint32_t parent_box, int32_t content_x,
                                    int32_t content_y, int32_t content_w,
                                    SWEEP_TEXT_MODE mode, uint32_t depth,
                                    int32_t* out_bottom) {
    const SWEEP_DOM_NODE* parent = sweep_dom_get(dom, dom_parent);
    int32_t cursor_y = content_y;
    int32_t pen_x = content_x;
    int32_t line_top = content_y;
    int32_t line_h = 0;
    uint32_t i;

    if (out_bottom) *out_bottom = content_y;
    if (!parent) return 0;
    if (depth > SWEEP_LAYOUT_MAX_DEPTH) {
        if (L) L->truncated = true;
        return 0;
    }

    for (i = 0u; i < parent->child_count; ++i) {
        uint32_t child_id = parent->children[i];
        const SWEEP_DOM_NODE* child = sweep_dom_get(dom, child_id);
        const SWEEP_COMPUTED_STYLE* cs = style_of(styles, style_count, child_id);
        if (!child) continue;
        if (cs && cs->display == SWEEP_DISPLAY_NONE) continue;

        if (child->type == SWEEP_DOM_ELEMENT && cs &&
            cs->display == SWEEP_DISPLAY_BLOCK) {
            int32_t advance;
            if (line_h > 0) {
                cursor_y = add_clamped(line_top, line_h);
                pen_x = content_x;
                line_top = cursor_y;
                line_h = 0;
            }
            advance = layout_block_node(L, dom, styles, style_count, child_id,
                                        parent_box, content_x, cursor_y,
                                        content_w, mode, depth + 1u);
            cursor_y = add_clamped(cursor_y, advance);
            line_top = cursor_y;
        } else if (child->type == SWEEP_DOM_TEXT) {
            if (!layout_text_node(L, dom, cs, child_id, parent_box, content_x,
                                  content_x + content_w, &pen_x, &line_top,
                                  &line_h, mode)) {
                L->truncated = true;
            }
        } else if (child->type == SWEEP_DOM_ELEMENT) {
            if (!layout_inline_element(L, dom, styles, style_count, child_id,
                                       parent_box, content_x, line_top,
                                       content_w, &pen_x, &line_top, &line_h,
                                       true, mode, depth + 1u)) {
                L->truncated = true;
            }
        }
    }

    if (line_h > 0) cursor_y = add_clamped(line_top, line_h);
    if (out_bottom) *out_bottom = cursor_y;
    return 1;
}

static int32_t layout_block_node(SWEEP_LAYOUT* L, const SWEEP_DOM* dom,
                                 const SWEEP_COMPUTED_STYLE* styles,
                                 size_t style_count, uint32_t node_id,
                                 uint32_t parent_box, int32_t margin_x,
                                 int32_t margin_y, int32_t avail_w,
                                 SWEEP_TEXT_MODE mode, uint32_t depth) {
    const SWEEP_COMPUTED_STYLE* st = style_of(styles, style_count, node_id);
    const SWEEP_DOM_NODE* node = sweep_dom_get(dom, node_id);
    int32_t m[4], p[4], b[4];
    int32_t font;
    int32_t cw;
    int32_t ch;
    int32_t content_x;
    int32_t content_y;
    int32_t child_bottom = 0;
    uint32_t box;
    SWEEP_LAYOUT_BOX* bx;

    if (!node || !st) return 0;
    if (depth > SWEEP_LAYOUT_MAX_DEPTH) {
        if (L) L->truncated = true;
        return 0;
    }
    font = st->font_size_px;
    resolve_edges(st, avail_w, m, p, b);

    if (st->width.unit == SWEEP_CSS_UNIT_AUTO) {
        cw = avail_w - m[SWEEP_SIDE_LEFT] - m[SWEEP_SIDE_RIGHT] -
             p[SWEEP_SIDE_LEFT] - p[SWEEP_SIDE_RIGHT] -
             b[SWEEP_SIDE_LEFT] - b[SWEEP_SIDE_RIGHT];
        cw = clamp_px(cw);
    } else {
        cw = clamp_px(sweep_css_length_resolve_px(&st->width, avail_w, font));
    }

    box = layout_new_box(L);
    if (box == 0u) return 0;
    layout_link_child(L, parent_box, box);
    bx = &L->boxes[box - 1u];
    bx->node = node_id;
    bx->display = st->display;
    layout_fill_box_style(bx, st);
    bx->x = add_clamped(margin_x, m[SWEEP_SIDE_LEFT]);
    bx->y = add_clamped(margin_y, m[SWEEP_SIDE_TOP]);
    bx->width = clamp_px(cw + p[SWEEP_SIDE_LEFT] + p[SWEEP_SIDE_RIGHT] +
                         b[SWEEP_SIDE_LEFT] + b[SWEEP_SIDE_RIGHT]);
    bx->content_x = add_clamped(bx->x, add_clamped(b[SWEEP_SIDE_LEFT],
                                                   p[SWEEP_SIDE_LEFT]));
    bx->content_y = add_clamped(bx->y, add_clamped(b[SWEEP_SIDE_TOP],
                                                   p[SWEEP_SIDE_TOP]));
    bx->content_w = cw;
    bx->margin[SWEEP_SIDE_TOP] = m[SWEEP_SIDE_TOP];
    bx->margin[SWEEP_SIDE_RIGHT] = m[SWEEP_SIDE_RIGHT];
    bx->margin[SWEEP_SIDE_BOTTOM] = m[SWEEP_SIDE_BOTTOM];
    bx->margin[SWEEP_SIDE_LEFT] = m[SWEEP_SIDE_LEFT];
    bx->padding[SWEEP_SIDE_TOP] = p[SWEEP_SIDE_TOP];
    bx->padding[SWEEP_SIDE_RIGHT] = p[SWEEP_SIDE_RIGHT];
    bx->padding[SWEEP_SIDE_BOTTOM] = p[SWEEP_SIDE_BOTTOM];
    bx->padding[SWEEP_SIDE_LEFT] = p[SWEEP_SIDE_LEFT];
    bx->border[SWEEP_SIDE_TOP] = b[SWEEP_SIDE_TOP];
    bx->border[SWEEP_SIDE_RIGHT] = b[SWEEP_SIDE_RIGHT];
    bx->border[SWEEP_SIDE_BOTTOM] = b[SWEEP_SIDE_BOTTOM];
    bx->border[SWEEP_SIDE_LEFT] = b[SWEEP_SIDE_LEFT];

    content_x = bx->content_x;
    content_y = bx->content_y;

    if (!layout_flow_children(L, dom, styles, style_count, node_id, box,
                              content_x, content_y, cw, mode, depth + 1u,
                              &child_bottom)) {
        L->truncated = true;
    }

    if (!length_is_auto_height(&st->height)) {
        ch = clamp_px(sweep_css_length_resolve_px(&st->height, avail_w, font));
    } else {
        ch = clamp_px(child_bottom - content_y);
    }
    bx->content_h = ch;
    bx->height = clamp_px(ch + p[SWEEP_SIDE_TOP] + p[SWEEP_SIDE_BOTTOM] +
                          b[SWEEP_SIDE_TOP] + b[SWEEP_SIDE_BOTTOM]);

    return clamp_px(m[SWEEP_SIDE_TOP] + bx->height + m[SWEEP_SIDE_BOTTOM]);
}

bool sweep_layout_build(const SWEEP_DOM* dom,
                        const SWEEP_COMPUTED_STYLE* styles,
                        size_t style_count,
                        const SWEEP_VIEWPORT* viewport,
                        SWEEP_TEXT_MODE mode,
                        SWEEP_LAYOUT* out) {
    const SWEEP_DOM_NODE* root_node;
    uint32_t box;
    SWEEP_LAYOUT_BOX* bx;
    int32_t content_bottom = 0;
    int32_t vw;
    int32_t vh;

    if (!dom || !styles || !viewport || !out) return false;
    if (viewport->width_px == 0u || viewport->height_px == 0u) return false;
    if (style_count < (size_t)dom->count) return false;
    if (dom->document == 0u) return false;
    root_node = sweep_dom_get(dom, dom->document);
    if (!root_node) return false;

    /* Clamp before casting: a uint32 viewport above INT32_MAX must not become
     * a negative or implementation-defined int32 width. */
    vw = viewport->width_px > (uint32_t)SWEEP_LAYOUT_MAX_PX
             ? SWEEP_LAYOUT_MAX_PX
             : (int32_t)viewport->width_px;
    vh = viewport->height_px > (uint32_t)SWEEP_LAYOUT_MAX_PX
             ? SWEEP_LAYOUT_MAX_PX
             : (int32_t)viewport->height_px;

    memset(out, 0, sizeof(*out));
    out->dom = dom;
    out->viewport = *viewport;
    out->viewport.width_px = (uint32_t)vw;
    out->viewport.height_px = (uint32_t)vh;

    box = layout_new_box(out);
    if (box == 0u) return false;
    out->root = box;
    bx = &out->boxes[box - 1u];
    bx->node = dom->document;
    bx->display = SWEEP_DISPLAY_BLOCK;
    bx->x = 0;
    bx->y = 0;
    bx->width = vw;
    bx->content_x = 0;
    bx->content_y = 0;
    bx->content_w = vw;
    bx->background_color = SWEEP_CSS_COLOR_TRANSPARENT;
    bx->border_color = SWEEP_CSS_COLOR_BLACK;
    bx->color = SWEEP_CSS_COLOR_BLACK;
    bx->font_size_px = 16;
    bx->text_align = SWEEP_TEXT_ALIGN_LEFT;

    if (!layout_flow_children(out, dom, styles, style_count, dom->document,
                              box, 0, 0, vw, mode, 0u, &content_bottom)) {
        out->truncated = true;
    }
    bx->content_h = clamp_px(content_bottom);
    bx->height = vh;
    if (bx->content_h > bx->height) bx->height = bx->content_h;
    out->content_height = (uint32_t)bx->content_h;
    return true;
}

/* ======================================================================= */
/* Paint command generation                                                */
/* ======================================================================= */

static bool rect_intersect(const SWEEP_LAYOUT* L, int32_t x, int32_t y,
                           int32_t w, int32_t h, int32_t* ox, int32_t* oy,
                           uint32_t* ow, uint32_t* oh) {
    int32_t x1 = (int32_t)L->viewport.width_px;
    int32_t y1 = (int32_t)L->viewport.height_px;
    int32_t nx = x < 0 ? 0 : x;
    int32_t ny = y < 0 ? 0 : y;
    int32_t nx2 = add_clamped(x, w);
    int32_t ny2 = add_clamped(y, h);
    int32_t cx2 = nx2 < x1 ? nx2 : x1;
    int32_t cy2 = ny2 < y1 ? ny2 : y1;
    if (x >= x1 || y >= y1 || nx2 <= 0 || ny2 <= 0) return false;
    if (cx2 <= nx || cy2 <= ny) return false;
    *ox = nx;
    *oy = ny;
    *ow = (uint32_t)(cx2 - nx);
    *oh = (uint32_t)(cy2 - ny);
    return true;
}

static bool paint_push(SWEEP_PAINT_CMD* out, size_t cap, size_t* count,
                       SWEEP_PAINT_CMD cmd) {
    if (!SweepPaintAppend(out, cap, count, cmd)) return false;
    return true;
}

static bool paint_rect(SWEEP_LAYOUT* L, SWEEP_PAINT_CMD* out, size_t cap,
                       size_t* count, int32_t x, int32_t y, int32_t w, int32_t h,
                       uint32_t color) {
    SWEEP_PAINT_CMD cmd;
    int32_t ox, oy;
    uint32_t ow, oh;
    if (w <= 0 || h <= 0) return true;
    if ((color >> 24) == 0u) return true;
    if (!rect_intersect(L, x, y, w, h, &ox, &oy, &ow, &oh)) return true;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = (uint8_t)SWEEP_PAINT_RECT;
    cmd.x = ox;
    cmd.y = oy;
    cmd.w = ow;
    cmd.h = oh;
    cmd.color = color;
    return paint_push(out, cap, count, cmd);
}

static bool paint_box(SWEEP_LAYOUT* L, SWEEP_PAINT_CMD* out, size_t cap,
                      size_t* count, const SWEEP_LAYOUT_BOX* box,
                      SWEEP_TEXT_MODE mode) {
    const SWEEP_DOM_NODE* node;
    uint32_t i;

    /* Background covers the border box. */
    if ((box->background_color >> 24) != 0u) {
        if (!paint_rect(L, out, cap, count, box->x, box->y, box->width,
                        box->height, box->background_color)) {
            return false;
        }
    }

    /* Borders: draw the four edges inside the border box. */
    if (box->border[SWEEP_SIDE_TOP] > 0 &&
        !paint_rect(L, out, cap, count, box->x, box->y, box->width,
                    box->border[SWEEP_SIDE_TOP], box->border_color)) return false;
    if (box->border[SWEEP_SIDE_BOTTOM] > 0 &&
        !paint_rect(L, out, cap, count, box->x,
                    box->y + box->height - box->border[SWEEP_SIDE_BOTTOM],
                    box->width, box->border[SWEEP_SIDE_BOTTOM],
                    box->border_color)) return false;
    if (box->border[SWEEP_SIDE_LEFT] > 0 &&
        !paint_rect(L, out, cap, count, box->x, box->y,
                    box->border[SWEEP_SIDE_LEFT], box->height,
                    box->border_color)) return false;
    if (box->border[SWEEP_SIDE_RIGHT] > 0 &&
        !paint_rect(L, out, cap, count,
                    box->x + box->width - box->border[SWEEP_SIDE_RIGHT],
                    box->y, box->border[SWEEP_SIDE_RIGHT], box->height,
                    box->border_color)) return false;

    node = sweep_dom_get(L->dom, box->node);
    if (!node || node->type != SWEEP_DOM_TEXT || box->text_len == 0u) return true;
    /* Fully transparent text paints nothing (glyph commands have no alpha). */
    if ((box->color >> 24) == 0u) return true;

    /* Text: one bounded TEXT command per codepoint (glyph id = codepoint). */
    {
        sweep_cp_t cps[SWEEP_PAINT_TEXT_CP_MAX];
        size_t n = 0u;
        size_t len = strlen(node->text);
        size_t off = box->text_offset;
        size_t slice_len = (size_t)box->text_len;
        int32_t pen_x = box->x;
        int32_t font = box->font_size_px > 0 ? box->font_size_px : 16;
        if (off > len) return true;
        if (off + slice_len > len) slice_len = len - off;
        if (!sweep_utf8_decode(mode, (const uint8_t*)node->text + off,
                               slice_len, cps, SWEEP_PAINT_TEXT_CP_MAX,
                               &n)) {
            return true;
        }
        for (i = 0u; i < n; ++i) {
            SWEEP_PAINT_CMD cmd;
            int32_t adv;
            sweep_cp_t cp = cps[i];
            int32_t ox, oy;
            uint32_t ow, oh;
            if (cp == (sweep_cp_t)' ') adv = font / 4;
            else if (cp < 0x80u) adv = font / 2;
            else adv = font;
            if (adv <= 0) adv = 1;
            if (!rect_intersect(L, pen_x, box->y, adv, font, &ox, &oy, &ow,
                                &oh)) {
                pen_x = add_clamped(pen_x, adv);
                continue;
            }
            memset(&cmd, 0, sizeof(cmd));
            cmd.op = (uint8_t)SWEEP_PAINT_TEXT;
            cmd.x = ox;
            cmd.y = oy;
            cmd.w = ow;
            cmd.h = oh;
            cmd.color = box->color;
            cmd.glyph = (uint32_t)cp;
            cmd.font_size_px = (font > 0) ? (uint32_t)font : 0u;
            cmd.text_flags = (cmd.font_size_px != 0u) ? SWEEP_PAINT_TEXT_FLAG_FONT_SIZE_VALID : 0u;
            /* Baseline not explicitly computed by layout in this phase; renderer/provider
             * may use font metrics with bitmap-intrinsic placement when available. */
            if (!paint_push(out, cap, count, cmd)) return false;
            pen_x = add_clamped(pen_x, adv);
        }
    }
    return true;
}

bool sweep_paint_from_layout(const SWEEP_LAYOUT* layout, SWEEP_TEXT_MODE mode,
                             SWEEP_PAINT_CMD* out, size_t cap,
                             size_t* out_count) {
    SWEEP_PAINT_CMD clear;
    size_t count = 0u;
    uint32_t i;

    if (out_count) *out_count = 0u;
    if (!layout || !out || !out_count || cap == 0u) return false;
    if (layout->viewport.width_px == 0u || layout->viewport.height_px == 0u) {
        return false;
    }

    memset(&clear, 0, sizeof(clear));
    clear.op = (uint8_t)SWEEP_PAINT_CLEAR;
    clear.x = 0;
    clear.y = 0;
    clear.w = layout->viewport.width_px;
    clear.h = layout->viewport.height_px;
    clear.color = 0xFFFFFFFFu;
    if (!paint_push(out, cap, &count, clear)) {
        *out_count = count;
        return false;
    }

    /* Boxes are created in depth-first document order, so ascending index is
     * a valid paint order (ancestor background before descendant content). */
    for (i = 0u; i < layout->count; ++i) {
        const SWEEP_LAYOUT_BOX* box = &layout->boxes[i];
        if (!box->in_use) continue;
        if (!paint_box((SWEEP_LAYOUT*)layout, out, cap, &count, box, mode)) {
            *out_count = count;
            return false;
        }
    }

    *out_count = count;
    return true;
}
