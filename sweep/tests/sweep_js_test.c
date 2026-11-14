/* sweep_js_test.c - host tests for the Sweep JavaScript subsystem.
 *
 * These tests are split by label prefix:
 *   [adapter]  engine adapter / runtime / context lifecycle (not ECMAScript)
 *   [engine]   real Duktape-backed ECMAScript execution (not simulated)
 *   [dom]      mediated DOM bindings over sweep_html
 *   [events]   bounded DOM-style event dispatch
 *   [text]     explicit SuperUnicode/UTF-16 boundary behaviour
 *   [script]   classic <script> execution and resource-provider loading
 *   [security] isolation and bounds
 *
 * Build (from Sweep root):
 *   make build-generated/sweep_js_test.exe
 *   build-generated/sweep_js_test.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sweep_js.h"
#include "../sweep_js_engine.h"
#include "../sweep_js_text.h"

static int g_failures = 0;
static int g_passed = 0;

static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

static SWEEP_DOM g_doms[4];

static SWEEP_DOM* dom_use(int slot, const char* html) {
    SWEEP_DOM* dom = &g_doms[slot];
    sweep_dom_init(dom);
    if (html) sweep_dom_parse(dom, html, strlen(html));
    return dom;
}

static int eval_ok(SWEEP_JS_CONTEXT* ctx, const char* src, SWEEP_JS_RESULT* r) {
    if (!sweep_js_evaluate_cstr(ctx, src, r)) {
        printf("      eval failed: %s | src=%s\n", r->error, src);
        return 0;
    }
    return 1;
}

static int eval_is(SWEEP_JS_CONTEXT* ctx, const char* src, const char* want) {
    SWEEP_JS_RESULT r;
    if (!eval_ok(ctx, src, &r)) return 0;
    if (strcmp(r.value, want) != 0) {
        printf("      got '%s' want '%s' | src=%s\n", r.value, want, src);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* adapter / runtime                                                   */
/* ------------------------------------------------------------------ */

static void test_runtime(void) {
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* contexts[SWEEP_JS_CONTEXTS_MAX];
    SWEEP_JS_CONTEXT* extra;
    SWEEP_DOM* dom;
    uint32_t i;
    SWEEP_JS_RESULT r;

    check("[adapter] runtime init rejects NULL out", !sweep_js_runtime_init(NULL, NULL));
    check("[adapter] runtime init", sweep_js_runtime_init(&rt, NULL) && rt != NULL);
    check("[adapter] context rejects NULL runtime",
          sweep_js_context_create(NULL, dom_use(0, NULL)) == NULL);

    dom = dom_use(1, "<html><body><p id='p'>x</p></body></html>");
    check("[adapter] context rejects NULL DOM", sweep_js_context_create(rt, NULL) == NULL);

    for (i = 0; i < SWEEP_JS_CONTEXTS_MAX; ++i) {
        contexts[i] = sweep_js_context_create(rt, dom);
        check("[adapter] context create within limit", contexts[i] != NULL);
    }
    extra = sweep_js_context_create(rt, dom);
    check("[adapter] context limit enforced", extra == NULL);
    sweep_js_context_destroy(contexts[0]);
    extra = sweep_js_context_create(rt, dom);
    check("[adapter] destroyed context slot reused", extra != NULL);
    if (extra) sweep_js_context_destroy(extra);
    for (i = 1; i < SWEEP_JS_CONTEXTS_MAX; ++i) sweep_js_context_destroy(contexts[i]);

    {
        SWEEP_JS_CONTEXT* ctx = sweep_js_context_create(rt, dom);
        check("[adapter] context last error empty", ctx && sweep_js_context_last_error(ctx)[0] == '\0');
        check("[adapter] default memory limit",
              ctx && sweep_js_context_memory_limit(ctx) == SWEEP_JS_ENGINE_DEFAULT_MEMORY);
        check("[adapter] memory used positive", ctx && sweep_js_context_memory_used(ctx) > 0u);
        check("[adapter] default text mode is WEB_COMPAT",
              ctx && sweep_js_context_text_mode(ctx) == SWEEP_TEXT_WEB_COMPAT);
        check("[adapter] evaluate(NULL context) fails",
              !sweep_js_evaluate(NULL, "1", 1u, &r) && r.status == SWEEP_JS_ERR_ARGUMENT);
        sweep_js_context_destroy(ctx);
    }

    sweep_js_runtime_shutdown(rt);
    check("[adapter] shutdown NULL is safe", (sweep_js_runtime_shutdown(NULL), 1));
}

/* ------------------------------------------------------------------ */
/* real ECMAScript                                                     */
/* ------------------------------------------------------------------ */

static void test_engine(void) {
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* ctx;
    SWEEP_JS_RESULT r;
    static char big[SWEEP_JS_SOURCE_MAX + 2u];

    check("[adapter] engine runtime init", sweep_js_runtime_init(&rt, NULL));
    ctx = sweep_js_context_create(rt, dom_use(0, "<html><body></body></html>"));
    check("[adapter] engine context", ctx != NULL);
    if (!ctx) { sweep_js_runtime_shutdown(rt); return; }

    check("[engine] arithmetic", eval_is(ctx, "1 + 2 * 3", "7"));
    check("[engine] string concatenation", eval_is(ctx, "'a' + 'b'", "ab"));
    check("[engine] numbers", eval_is(ctx, "Math.floor(7 / 2)", "3"));
    check("[engine] array length", eval_is(ctx, "[1,2,3].length", "3"));
    check("[engine] object property", eval_is(ctx, "({a: 5}).a", "5"));
    check("[engine] function call", eval_is(ctx, "(function(x){return x*2;})(21)", "42"));
    check("[engine] closure", eval_is(ctx,
          "var mk = function(n){ return function(){ return n; }; }; mk(9)()", "9"));
    check("[engine] JSON", eval_is(ctx, "JSON.stringify({a:[1,2]})", "{\"a\":[1,2]}"));
    check("[engine] try/catch", eval_is(ctx,
          "var v = 0; try { throw new Error('x'); } catch (e) { v = 1; } v", "1"));
    check("[engine] RegExp", eval_is(ctx, "'abc'.replace(/b/, 'X')", "aXc"));
    check("[engine] undefined result flag",
          eval_ok(ctx, "void 0", &r) && r.is_undefined && r.status == SWEEP_JS_OK);
    check("[engine] defined result flag",
          eval_ok(ctx, "0", &r) && !r.is_undefined && strcmp(r.value, "0") == 0);

    check("[engine] syntax error reported",
          !sweep_js_evaluate_cstr(ctx, "var = ;", &r) && r.status == SWEEP_JS_ERR_SYNTAX);
    check("[engine] runtime error reported",
          !sweep_js_evaluate_cstr(ctx, "missingName + 1", &r) &&
          r.status == SWEEP_JS_ERR_RUNTIME);
    check("[engine] error buffer bounded",
          !sweep_js_evaluate_cstr(ctx, "throw new Error('boom')", &r) &&
          r.error[0] != '\0' && strlen(r.error) < SWEEP_JS_ERROR_MAX);

    check("[engine] infinite loop times out",
          !sweep_js_evaluate_cstr(ctx, "while (true) {}", &r) &&
          r.status == SWEEP_JS_ERR_TIMEOUT && r.timed_out);
    check("[engine] context usable after timeout", eval_is(ctx, "1 + 1", "2"));

    memset(big, ' ', sizeof(big));
    big[0] = '1'; big[1] = ';';
    check("[engine] source size limit",
          !sweep_js_evaluate(ctx, big, SWEEP_JS_SOURCE_MAX + 1u, &r) &&
          r.status == SWEEP_JS_ERR_LIMIT);

    sweep_js_context_destroy(ctx);
    sweep_js_runtime_shutdown(rt);
}

static void test_memory_cap(void) {
    SWEEP_JS_RUNTIME_CONFIG cfg;
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* ctx;
    SWEEP_JS_RESULT r;
    int ok;

    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_limit = 300000u;
    cfg.timeout_ms = 500u;
    check("[adapter] capped runtime init", sweep_js_runtime_init(&rt, &cfg));
    ctx = sweep_js_context_create(rt, dom_use(0, NULL));
    check("[adapter] capped context", ctx != NULL);
    if (ctx) {
        ok = sweep_js_evaluate_cstr(ctx,
            "var a=[]; for(;;){ a.push('0123456789012345678901234567890123456789'); }",
            &r);
        check("[engine] allocation failure reported, no crash",
              !ok && (r.status == SWEEP_JS_ERR_MEMORY || r.status == SWEEP_JS_ERR_TIMEOUT ||
                      r.status == SWEEP_JS_ERR_LIMIT));
        check("[engine] memory error has diagnostic", !ok && r.error[0] != '\0');
        sweep_js_context_destroy(ctx);
    }
    sweep_js_runtime_shutdown(rt);
}

/* ------------------------------------------------------------------ */
/* DOM bindings                                                        */
/* ------------------------------------------------------------------ */

static void test_dom(void) {
    static const char* html =
        "<!doctype html><html><head><title>T</title></head><body>"
        "<div id='a'><span id='s1'>one</span><span id='s2'>two</span></div>"
        "<p id='p'>para</p></body></html>";
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* ctx;

    check("[adapter] DOM runtime init", sweep_js_runtime_init(&rt, NULL));
    ctx = sweep_js_context_create(rt, dom_use(0, html));
    check("[adapter] DOM context", ctx != NULL);
    if (!ctx) { sweep_js_runtime_shutdown(rt); return; }

    check("[dom] document is an object", eval_is(ctx, "typeof document", "object"));
    check("[dom] documentElement tagName", eval_is(ctx, "document.documentElement.tagName", "HTML"));
    check("[dom] body tagName", eval_is(ctx, "document.body.tagName", "BODY"));
    check("[dom] title getter", eval_is(ctx, "document.title", "T"));
    check("[dom] getElementById", eval_is(ctx, "document.getElementById('a').id", "a"));
    check("[dom] getElementById missing is null",
          eval_is(ctx, "document.getElementById('nope') === null", "true"));
    check("[dom] getElementsByTagName count",
          eval_is(ctx, "document.getElementsByTagName('span').length", "2"));
    check("[dom] getElementsByTagName wildcard",
          eval_is(ctx, "document.getElementsByTagName('*').length", "8"));
    check("[dom] querySelector", eval_is(ctx, "document.querySelector('#s1').id", "s1"));
    check("[dom] querySelector descendant",
          eval_is(ctx, "document.querySelector('div span').id", "s1"));
    check("[dom] querySelectorAll", eval_is(ctx, "document.querySelectorAll('span').length", "2"));
    check("[dom] element querySelector scopes to subtree",
          eval_is(ctx, "document.getElementById('a').querySelectorAll('span').length", "2"));
    check("[dom] querySelector no match is null",
          eval_is(ctx, "document.querySelector('article') === null", "true"));

    check("[dom] element id", eval_is(ctx, "document.getElementById('a').id", "a"));
    check("[dom] element className", eval_is(ctx, "document.getElementById('s1').className", ""));
    check("[dom] tagName upper-case", eval_is(ctx, "document.getElementById('s1').tagName", "SPAN"));
    check("[dom] textContent getter", eval_is(ctx, "document.getElementById('a').textContent", "onetwo"));
    check("[dom] parentNode", eval_is(ctx, "document.getElementById('s1').parentNode.id", "a"));
    check("[dom] firstChild", eval_is(ctx, "document.getElementById('a').firstChild.id", "s1"));
    check("[dom] lastChild", eval_is(ctx, "document.getElementById('a').lastChild.id", "s2"));
    check("[dom] children only elements", eval_is(ctx, "document.getElementById('a').children.length", "2"));

    check("[dom] getAttribute", eval_is(ctx,
          "(document.getElementById('a').setAttribute('data-k','v'),"
          " document.getElementById('a').getAttribute('data-k'))", "v"));
    check("[dom] getAttribute missing is null", eval_is(ctx,
          "document.getElementById('a').getAttribute('missing') === null", "true"));
    check("[dom] setAttribute replaces", eval_is(ctx,
          "var a=document.getElementById('a'); a.setAttribute('data-k','1');"
          "a.setAttribute('data-k','2'); a.getAttribute('data-k')", "2"));
    check("[dom] removeAttribute", eval_is(ctx,
          "var a=document.getElementById('a'); a.removeAttribute('data-k');"
          "a.getAttribute('data-k') === null", "true"));
    check("[dom] id setter updates lookup", eval_is(ctx,
          "var a=document.getElementById('a'); a.id='renamed';"
          "document.getElementById('renamed') === a", "true"));
    check("[dom] className setter", eval_is(ctx,
          "var a=document.getElementById('renamed'); a.className='k'; a.className", "k"));
    check("[dom] className reflected in attribute", eval_is(ctx,
          "document.getElementById('renamed').getAttribute('class')", "k"));

    check("[dom] createElement + appendChild", eval_is(ctx,
          "var e=document.createElement('em'); e.textContent='x';"
          "document.body.appendChild(e); document.querySelector('em').textContent", "x"));
    check("[dom] createElement tagName", eval_is(ctx,
          "document.createElement('DiV').tagName", "DIV"));
    check("[dom] createTextNode", eval_is(ctx,
          "var a=document.getElementById('renamed');"
          "var t=document.createTextNode('!'); a.appendChild(t); a.textContent", "onetwo!"));
    check("[dom] insertBefore", eval_is(ctx,
          "var a=document.getElementById('renamed');"
          "var s2=document.getElementById('s2');"
          "var t=document.createTextNode('-');"
          "a.insertBefore(t, s2); a.textContent", "one-two!"));

    check("[dom] removeChild detaches", eval_is(ctx,
          "var a=document.getElementById('renamed');"
          "var s2=document.getElementById('s2');"
          "a.removeChild(s2); s2.parentNode === null", "true"));
    check("[dom] detached wrapper remains usable", eval_is(ctx,
          "s2.textContent", "two"));
    check("[dom] reappend detached node", eval_is(ctx,
          "document.body.appendChild(s2); s2.parentNode === document.body", "true"));

    check("[dom] textContent setter", eval_is(ctx,
          "var p=document.getElementById('p'); p.textContent='hello'; p.textContent", "hello"));
    check("[dom] textContent replaces children", eval_is(ctx,
          "var p=document.getElementById('p'); p.textContent='a'; p.textContent='b';"
          "p.textContent + ':' + p.children.length", "b:0"));
    check("[dom] repeated textContent does not exhaust DOM", eval_is(ctx,
          "var p=document.getElementById('p');"
          "for (var i=0;i<60;i++) p.textContent='v'+i;"
          "p.textContent", "v59"));

    check("[dom] innerHTML setter builds DOM", eval_is(ctx,
          "var p=document.getElementById('p');"
          "p.innerHTML='<b id=\"b1\">bold</b>';"
          "document.getElementById('b1').textContent", "bold"));
    check("[dom] innerHTML getter serializes", eval_is(ctx,
          "var p=document.getElementById('p');"
          "p.innerHTML.indexOf('<b') >= 0 && p.innerHTML.indexOf('bold') >= 0", "true"));
    check("[dom] innerHTML escapes text", eval_is(ctx,
          "var p=document.getElementById('p'); p.textContent='<&>'; p.innerHTML", "&lt;&amp;&gt;"));

    check("[dom] document.title setter updates", eval_is(ctx,
          "document.title='New'; document.title", "New"));

    check("[dom] append document rejected", eval_is(ctx,
          "var t=false; try { document.appendChild(document); } catch(e) { t=true; } t", "true"));
    check("[dom] self-cycle rejected", eval_is(ctx,
          "var t=false; var e=document.createElement('i');"
          "try { e.appendChild(e); } catch(x) { t=true; } t", "true"));
    check("[dom] ancestor-cycle rejected", eval_is(ctx,
          "var t=false; var a=document.getElementById('renamed');"
          "try { a.firstChild.appendChild(a); } catch(x) { t=true; } t", "true"));
    check("[dom] forged wrapper rejected", eval_is(ctx,
          "var t=false; try { document.body.appendChild({__sweep_node:1}); }"
          "catch(e) { t = e instanceof TypeError; } t", "true"));
    check("[dom] prototype-poisoned fake rejected", eval_is(ctx,
          "Object.prototype.__sweep_node = 1;"
          "var t=false; try { document.body.appendChild({}); }"
          "catch(e) { t = e instanceof TypeError; }"
          "delete Object.prototype.__sweep_node; t", "true"));
    check("[dom] invalid attribute name rejected", eval_is(ctx,
          "var t=false; try { document.body.setAttribute('a b','x'); }"
          "catch(e) { t=true; } t", "true"));
    check("[dom] selector list unsupported", eval_is(ctx,
          "var t=false; try { document.querySelector('p, div'); }"
          "catch(e) { t=true; } t", "true"));
    check("[dom] document textContent rejected", eval_is(ctx,
          "var t=false; try { document.textContent='x'; }"
          "catch(e) { t=true; } t", "true"));
    check("[dom] childNodes is explicitly absent",
          eval_is(ctx, "typeof document.body.childNodes", "undefined"));
    check("[events] addEventListener available on nodes",
          eval_is(ctx, "typeof document.body.addEventListener === 'function'", "true"));

    check("[dom] getElementsByTagName returns snapshot", eval_is(ctx,
          "var arr=document.getElementsByTagName('span');"
          "var before=arr.length;"
          "document.body.appendChild(document.createElement('span'));"
          "arr.length === before", "true"));

    sweep_js_context_destroy(ctx);
    sweep_js_runtime_shutdown(rt);
}

static void test_dom_limits(void) {
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* ctx;
    SWEEP_JS_RESULT r;

    check("[adapter] limit runtime init", sweep_js_runtime_init(&rt, NULL));

    /* Per-parent child-list cap. */
    ctx = sweep_js_context_create(rt, dom_use(0, "<html><body></body></html>"));
    check("[adapter] limit context", ctx != NULL);
    if (ctx) {
        check("[dom] child-list cap enforced", eval_is(ctx,
              "var made=0, threw=false;"
              "try { while (true) {"
              "  document.body.appendChild(document.createElement('i')); made++; }"
              "} catch(e) { threw = true; }"
              "threw + ':' + (made <= 40) + ':' + (document.body.children.length <= 32)",
              "true:true:true"));
        sweep_js_context_destroy(ctx);
    }

    /* Whole-document node cap (creation without attaching). */
    ctx = sweep_js_context_create(rt, dom_use(1, "<html><body></body></html>"));
    check("[adapter] node-cap context", ctx != NULL);
    if (ctx) {
        check("[dom] DOM node cap throws RangeError", eval_is(ctx,
              "var full=false, made=0;"
              "try { for (var i=0;i<300;i++) { document.createElement('i'); made++; } }"
              "catch(e) { full = e instanceof RangeError; }"
              "full + ':' + (made <= 127)", "true:true"));
        check("[dom] document still readable after cap", eval_is(ctx,
              "typeof document.body.tagName", "string"));
        check("[dom] innerHTML over-capacity does not crash",
              sweep_js_evaluate_cstr(ctx,
                  "document.body.innerHTML='<b>1</b><b>2</b><b>3</b>'", &r));
        check("[dom] context remains stable", eval_is(ctx, "1+1", "2"));
        sweep_js_context_destroy(ctx);
    }
    sweep_js_runtime_shutdown(rt);
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

static void test_events(void) {
    static const char* html =
        "<html><body><div id='a'><span id='s'>x</span></div><p id='p'>y</p></body></html>";
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* ctx;

    check("[adapter] event runtime init", sweep_js_runtime_init(&rt, NULL));
    ctx = sweep_js_context_create(rt, dom_use(0, html));
    check("[adapter] event context", ctx != NULL);
    if (!ctx) { sweep_js_runtime_shutdown(rt); return; }

    check("[events] capture/target/bubble order", eval_is(ctx,
          "var log=[];"
          "document.addEventListener('z', function(e){log.push('cap');}, true);"
          "document.addEventListener('z', function(e){log.push('bub');}, false);"
          "document.getElementById('a').addEventListener('z', function(e){log.push('target');}, false);"
          "document.getElementById('a').dispatchEvent(new Event('z', {bubbles:true}));"
          "log.join(',')", "cap,target,bub"));
    check("[events] capture=false listener not called in capture", eval_is(ctx,
          "var n=0; var a=document.getElementById('a');"
          "a.addEventListener('q', function(){n++;}, false);"
          "a.dispatchEvent(new Event('q')); n", "1"));
    check("[events] event properties", eval_is(ctx,
          "var seen=null; var a=document.getElementById('a');"
          "a.addEventListener('props', function(e){"
          " seen = e.type + ':' + (e.target===a) + ':' + (e.currentTarget===a) +"
          " ':' + e.bubbles + ':' + e.cancelable; });"
          "a.dispatchEvent(new Event('props', {bubbles:true, cancelable:true})); seen",
          "props:true:true:true:true"));
    check("[events] removeEventListener", eval_is(ctx,
          "var n=0; var a=document.getElementById('a'); var f=function(){n++;};"
          "a.addEventListener('r', f); a.removeEventListener('r', f);"
          "a.dispatchEvent(new Event('r')); n", "0"));
    check("[events] capture flag is part of listener identity", eval_is(ctx,
          "var n=0; var a=document.getElementById('a'); var f=function(){n++;};"
          "a.addEventListener('r2', f, true); a.removeEventListener('r2', f, false);"
          "a.dispatchEvent(new Event('r2')); n", "1"));
    check("[events] duplicate listener ignored", eval_is(ctx,
          "var n=0; var a=document.getElementById('a'); var f=function(){n++;};"
          "a.addEventListener('d', f); a.addEventListener('d', f);"
          "a.dispatchEvent(new Event('d')); n", "1"));
    check("[events] stopPropagation", eval_is(ctx,
          "var n=0; var a=document.getElementById('a');"
          "a.addEventListener('sp', function(e){n++; e.stopPropagation();});"
          "document.body.addEventListener('sp', function(e){n+=10;});"
          "a.dispatchEvent(new Event('sp', {bubbles:true})); n", "1"));
    check("[events] stopImmediatePropagation", eval_is(ctx,
          "var n=0; var a=document.getElementById('a');"
          "a.addEventListener('si', function(e){n++; e.stopImmediatePropagation();});"
          "a.addEventListener('si', function(e){n+=10;});"
          "a.dispatchEvent(new Event('si')); n", "1"));
    check("[events] preventDefault on cancelable", eval_is(ctx,
          "var e=new Event('c', {cancelable:true}); var a=document.getElementById('a');"
          "a.addEventListener('c', function(ev){ev.preventDefault();});"
          "var ret=a.dispatchEvent(e); e.defaultPrevented + ':' + ret", "true:false"));
    check("[events] preventDefault on non-cancelable ignored", eval_is(ctx,
          "var e=new Event('nc'); var a=document.getElementById('a');"
          "a.addEventListener('nc', function(ev){ev.preventDefault();});"
          "var ret=a.dispatchEvent(e); e.defaultPrevented + ':' + ret", "false:true"));
    check("[events] listener error does not abort dispatch", eval_is(ctx,
          "var n=0; var a=document.getElementById('a');"
          "a.addEventListener('err', function(){ throw new Error('x'); });"
          "a.addEventListener('err', function(){ n=1; });"
          "a.dispatchEvent(new Event('err')); n", "1"));
    check("[events] dispatch recursion guard", eval_is(ctx,
          "var a=document.getElementById('a'); var err=null;"
          "a.addEventListener('rec', function(){"
          " try { a.dispatchEvent(new Event('rec')); } catch(e) { err=e; } });"
          "a.dispatchEvent(new Event('rec')); err !== null", "true"));
    check("[events] document can dispatch", eval_is(ctx,
          "var e=new Event('doc'); var t=false;"
          "document.addEventListener('doc', function(){t=true;});"
          "document.dispatchEvent(e); t && e.target===document", "true"));
    check("[events] listener limit", eval_is(ctx,
          "var a=document.getElementById('a'); var full=false;"
          "try { for (var i=0;i<40;i++) a.addEventListener('lim', function(){}); }"
          "catch(e) { full=true; } full", "true"));
    check("[events] Event requires type", eval_is(ctx,
          "var t=false; try { new Event(); } catch(e) { t=true; } t", "true"));
    check("[events] unsupported constructor absent",
          eval_is(ctx, "typeof MouseEvent", "undefined"));

    sweep_js_context_destroy(ctx);
    sweep_js_runtime_shutdown(rt);
}

/* ------------------------------------------------------------------ */
/* text boundary                                                       */
/* ------------------------------------------------------------------ */

static int bytes_are(const char* s, size_t len, const unsigned char* want,
                     size_t want_len) {
    return len == want_len && memcmp(s, want, want_len) == 0;
}

static void test_text(void) {
    static const unsigned char pair_utf8[4] = { 0xF0u, 0x9Fu, 0x98u, 0x80u };
    static const unsigned char lone_utf8[3] = { 0xEFu, 0xBFu, 0xBDu };
    static const unsigned char lone_cesu[3] = { 0xEDu, 0xA0u, 0x80u };
    static const unsigned char pair_cesu[6] = {
        0xEDu, 0xA0u, 0xBDu, 0xEDu, 0xB8u, 0x80u
    };
    static const unsigned char pair_in[4] = { 0xF0u, 0x9Fu, 0x98u, 0x80u };
    static const unsigned char lone_in[3] = { 0xEDu, 0xA0u, 0x80u };
    unsigned char out[16];
    size_t used = 0u;
    SWEEP_JS_RUNTIME_CONFIG cfg;
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* ctx;
    SWEEP_JS_RESULT r;

    check("[text] WEB_COMPAT combines surrogate pairs",
          sweep_js_cesu8_to_utf8(SWEEP_TEXT_WEB_COMPAT, pair_cesu, 6u, out,
                                 sizeof(out), &used) &&
          used == 4u && memcmp(out, pair_utf8, 4u) == 0);
    used = 0u;
    check("[text] WEB_COMPAT replaces unpaired surrogate",
          sweep_js_cesu8_to_utf8(SWEEP_TEXT_WEB_COMPAT, lone_cesu, 3u, out,
                                 sizeof(out), &used) &&
          used == 3u && memcmp(out, lone_utf8, 3u) == 0);
    used = 0u;
    check("[text] WEB_COMPAT encodes non-BMP as CESU-8",
          sweep_js_utf8_to_cesu8(SWEEP_TEXT_WEB_COMPAT, pair_in, 4u, out,
                                 sizeof(out), &used) &&
          used == 6u && memcmp(out, pair_cesu, 6u) == 0);
    used = 0u;
    check("[text] NATIVE_SU preserves unpaired surrogate",
          sweep_js_cesu8_to_utf8(SWEEP_TEXT_NATIVE_SU, lone_cesu, 3u, out,
                                 sizeof(out), &used) &&
          used == 3u && memcmp(out, lone_cesu, 3u) == 0);
    used = 0u;
    check("[text] NATIVE_SU never pairs surrogates",
          sweep_js_cesu8_to_utf8(SWEEP_TEXT_NATIVE_SU, pair_cesu, 6u, out,
                                 sizeof(out), &used) &&
          used == 6u && memcmp(out, pair_cesu, 6u) == 0);
    used = 0u;
    check("[text] NATIVE_SU rejects non-16-bit scalar",
          sweep_js_utf8_to_cesu8(SWEEP_TEXT_NATIVE_SU, pair_in, 4u, out,
                                 sizeof(out), &used) &&
          used == 3u && memcmp(out, lone_utf8, 3u) == 0);
    used = 0u;
    check("[text] NATIVE_SU passes surrogate bytes through",
          sweep_js_utf8_to_cesu8(SWEEP_TEXT_NATIVE_SU, lone_in, 3u, out,
                                 sizeof(out), &used) &&
          used == 3u && memcmp(out, lone_cesu, 3u) == 0);

    /* End-to-end: engine string literal -> DOM boundary -> result. */
    memset(&cfg, 0, sizeof(cfg));
    check("[adapter] web text runtime", sweep_js_runtime_init(&rt, &cfg));
    ctx = sweep_js_context_create(rt, dom_use(0, "<html><body><p id='m'>x</p></body></html>"));
    check("[adapter] web text context", ctx != NULL);
    if (ctx) {
        check("[text] WEB_COMPAT pair through engine",
              eval_ok(ctx, "'\\uD83D\\uDE00'", &r) &&
              bytes_are(r.value, r.value_len, pair_utf8, 4u));
        check("[text] WEB_COMPAT lone through engine",
              eval_ok(ctx, "'\\uD800'", &r) &&
              bytes_are(r.value, r.value_len, lone_utf8, 3u));
        check("[text] WEB_COMPAT DOM round trip",
              eval_ok(ctx,
                  "var m=document.getElementById('m');"
                  "m.textContent='\\uD83D\\uDE00'; m.textContent", &r) &&
              bytes_are(r.value, r.value_len, pair_utf8, 4u));
        sweep_js_context_destroy(ctx);
    }
    sweep_js_runtime_shutdown(rt);

    memset(&cfg, 0, sizeof(cfg));
    cfg.text_mode = SWEEP_TEXT_NATIVE_SU;
    check("[adapter] native text runtime", sweep_js_runtime_init(&rt, &cfg));
    ctx = sweep_js_context_create(rt, dom_use(0, "<html><body><p id='m'>x</p></body></html>"));
    check("[adapter] native text context", ctx != NULL);
    if (ctx) {
        check("[text] NATIVE_SU lone through engine",
              eval_ok(ctx, "'\\uD800'", &r) &&
              bytes_are(r.value, r.value_len, lone_cesu, 3u));
        check("[text] NATIVE_SU pair through engine",
              eval_ok(ctx, "'\\uD83D\\uDE00'", &r) &&
              bytes_are(r.value, r.value_len, pair_cesu, 6u));
        check("[text] NATIVE_SU DOM round trip",
              eval_ok(ctx,
                  "var m=document.getElementById('m');"
                  "m.textContent='\\uD800'; m.textContent", &r) &&
              bytes_are(r.value, r.value_len, lone_cesu, 3u));
        sweep_js_context_destroy(ctx);
    }
    sweep_js_runtime_shutdown(rt);
}

/* ------------------------------------------------------------------ */
/* script loading                                                      */
/* ------------------------------------------------------------------ */

static bool provider_js(void* ud, const char* url, char* out, size_t cap,
                        size_t* out_len, char* mime, size_t mime_cap) {
    const char* src =
        "document.getElementById('ext').textContent='fetched'; var ext=7;";
    (void) ud;
    if (strcmp(url, "app.js") != 0) return false;
    if (strlen(src) >= cap) return false;
    memcpy(out, src, strlen(src) + 1u);
    *out_len = strlen(src);
    if (mime_cap) {
        const char* m = "text/javascript";
        size_t n = strlen(m);
        if (n >= mime_cap) n = mime_cap - 1u;
        memcpy(mime, m, n);
        mime[n] = '\0';
    }
    return true;
}

static bool provider_plain(void* ud, const char* url, char* out, size_t cap,
                           size_t* out_len, char* mime, size_t mime_cap) {
    const char* src = "var should_not_run=1;";
    (void) ud;
    if (strcmp(url, "plain.txt") != 0) return false;
    if (strlen(src) >= cap) return false;
    memcpy(out, src, strlen(src) + 1u);
    *out_len = strlen(src);
    if (mime_cap) {
        const char* m = "text/plain";
        size_t n = strlen(m);
        if (n >= mime_cap) n = mime_cap - 1u;
        memcpy(mime, m, n);
        mime[n] = '\0';
    }
    return true;
}

static void test_scripts(void) {
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* ctx;
    SWEEP_JS_SCRIPT_STATS st;
    static const char* html =
        "<html><body>"
        "<div id='ext'>no</div>"
        "<script>var order=[]; order.push('one'); if (1 < 2) order.push('lt');</script>"
        "<script type='text/plain'>order.push('skip');</script>"
        "<script type='module'>order.push('module');</script>"
        "<script type='application/javascript'>order.push('mime');</script>"
        "<script></script>"
        "<script>var = ;</script>"
        "<script>order.push('two');</script>"
        "</body></html>";

    check("[adapter] script runtime init", sweep_js_runtime_init(&rt, NULL));
    ctx = sweep_js_context_create(rt, dom_use(0, html));
    check("[adapter] script context", ctx != NULL);
    if (!ctx) { sweep_js_runtime_shutdown(rt); return; }

    check("[script] pass reports the syntax failure",
          !sweep_js_run_document_scripts(ctx, &st));
    check("[script] all script elements seen", st.seen == 7u);
    check("[script] non-JS types skipped, modules unsupported",
          st.skipped == 2u && st.unsupported == 1u);
    check("[script] syntax error failed once", st.failed == 1u);
    check("[script] executed the rest in order", st.executed == 3u);
    check("[script] raw-text tokenizer keeps '<' inside script",
          eval_is(ctx, "order.join(',')", "one,lt,mime,two"));

    check("[script] innerHTML script does not auto-execute", eval_is(ctx,
          "var d=document.createElement('div');"
          "d.innerHTML='<script>var innerRan=1;<\\/script>';"
          "typeof innerRan", "undefined"));
    check("[script] explicit pass runs newly inserted script", eval_is(ctx,
          "var d=document.getElementById('ext');"
          "d.innerHTML='<script>var innerRan=2;<\\/script>';"
          "typeof innerRan", "undefined"));
    {
        SWEEP_JS_SCRIPT_STATS st2;
        (void) sweep_js_run_document_scripts(ctx, &st2);
        check("[script] second pass executes inserted script",
              eval_is(ctx, "innerRan", "2"));
    }

    sweep_js_context_destroy(ctx);
    sweep_js_runtime_shutdown(rt);

    /* External provider. */
    {
        SWEEP_JS_RUNTIME_CONFIG cfg;
        SWEEP_JS_RUNTIME* prt = NULL;
        static const char* ehtml =
            "<html><body><div id='ext'>no</div>"
            "<script src='app.js'></script></body></html>";
        memset(&cfg, 0, sizeof(cfg));
        cfg.fetch = provider_js;
        check("[adapter] provider runtime", sweep_js_runtime_init(&prt, &cfg));
        ctx = sweep_js_context_create(prt, dom_use(0, ehtml));
        check("[adapter] provider context", ctx != NULL);
        if (ctx) {
            check("[script] external script executed",
                  sweep_js_run_document_scripts(ctx, &st) &&
                  st.external == 1u && st.executed == 1u);
            check("[script] external source ran",
                  eval_is(ctx, "ext", "7"));
            check("[script] external script mutated DOM",
                  eval_is(ctx, "document.getElementById('ext').textContent", "fetched"));
            sweep_js_context_destroy(ctx);
        }
        sweep_js_runtime_shutdown(prt);
    }
    {
        SWEEP_JS_RUNTIME_CONFIG cfg;
        SWEEP_JS_RUNTIME* prt = NULL;
        static const char* ehtml =
            "<html><body><script src='plain.txt'></script></body></html>";
        memset(&cfg, 0, sizeof(cfg));
        cfg.fetch = provider_plain;
        check("[adapter] MIME runtime", sweep_js_runtime_init(&prt, &cfg));
        ctx = sweep_js_context_create(prt, dom_use(0, ehtml));
        check("[adapter] MIME context", ctx != NULL);
        if (ctx) {
            check("[script] non-JS external MIME is not executed",
                  sweep_js_run_document_scripts(ctx, &st) &&
                  st.unsupported == 1u && st.executed == 0u);
            check("[script] non-JS source did not run",
                  eval_is(ctx, "typeof should_not_run", "undefined"));
            sweep_js_context_destroy(ctx);
        }
        sweep_js_runtime_shutdown(prt);
    }
    {
        static const char* ehtml =
            "<html><body><script src='app.js'></script></body></html>";
        check("[adapter] no-provider runtime", sweep_js_runtime_init(&rt, NULL));
        ctx = sweep_js_context_create(rt, dom_use(0, ehtml));
        check("[adapter] no-provider context", ctx != NULL);
        if (ctx) {
            check("[script] external script without provider fails",
                  !sweep_js_run_document_scripts(ctx, &st) &&
                  st.external == 1u && st.failed == 1u);
            check("[script] failure has bounded diagnostic",
                  sweep_js_context_last_error(ctx)[0] != '\0');
            sweep_js_context_destroy(ctx);
        }
        sweep_js_runtime_shutdown(rt);
    }

    /* defer/async are observed but not scheduled. */
    {
        static const char* dhtml =
            "<html><body><script defer async>var d=1;</script></body></html>";
        check("[adapter] defer runtime", sweep_js_runtime_init(&rt, NULL));
        ctx = sweep_js_context_create(rt, dom_use(0, dhtml));
        check("[adapter] defer context", ctx != NULL);
        if (ctx) {
            check("[script] defer/async observed, still executed synchronously",
                  sweep_js_run_document_scripts(ctx, &st) && st.saw_defer &&
                  st.saw_async && st.executed == 1u);
            sweep_js_context_destroy(ctx);
        }
        sweep_js_runtime_shutdown(rt);
    }
}

/* ------------------------------------------------------------------ */
/* security / isolation                                                */
/* ------------------------------------------------------------------ */

static void test_security(void) {
    SWEEP_JS_RUNTIME* rt = NULL;
    SWEEP_JS_CONTEXT* a;
    SWEEP_JS_CONTEXT* b;
    SWEEP_DOM* doma;
    SWEEP_DOM* domb;

    check("[adapter] isolation runtime init", sweep_js_runtime_init(&rt, NULL));
    doma = dom_use(0, "<html><body><p id='x'>a</p></body></html>");
    domb = dom_use(1, "<html><body><p id='x'>b</p></body></html>");
    a = sweep_js_context_create(rt, doma);
    b = sweep_js_context_create(rt, domb);
    check("[adapter] isolation contexts", a != NULL && b != NULL);

    {
        static const char* missing[] = {
            "KDB", "kernel", "process", "require", "alert", "Duktape",
            "OpenWindows", "fs", "socket", "XMLHttpRequest"
        };
        size_t i;
        char src[128];
        for (i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i) {
            snprintf(src, sizeof(src), "typeof %s", missing[i]);
            check("[security] host/network global absent", eval_is(a, src, "undefined"));
        }
    }

    check("[security] globals isolated between contexts", eval_is(a, "var marker=1; marker", "1"));
    check("[security] second context has no marker", eval_is(b, "typeof marker", "undefined"));
    check("[security] DOM state isolated per document",
          eval_is(a, "document.getElementById('x').textContent", "a") &&
          eval_is(b, "document.getElementById('x').textContent", "b"));

    check("[security] listener state does not cross contexts", eval_is(a,
          "var n=0; document.getElementById('x').addEventListener('iso', function(){n++;});"
          "document.getElementById('x').dispatchEvent(new Event('iso')); n", "1"));
    check("[security] other context has no listeners", eval_is(b,
          "var n=0; document.getElementById('x').addEventListener('iso', function(){n++;});"
          "document.getElementById('x').dispatchEvent(new Event('iso')); n", "1"));

    sweep_js_context_destroy(a);
    sweep_js_context_destroy(b);
    check("[security] destroying contexts is safe", 1);
    sweep_js_runtime_shutdown(rt);
}

int main(void) {
    printf("=== Sweep JS subsystem tests ===\n");
    test_runtime();
    test_engine();
    test_memory_cap();
    test_dom();
    test_dom_limits();
    test_events();
    test_text();
    test_scripts();
    test_security();
    printf("--------------------------------\n");
    printf("passed: %d, failed: %d\n", g_passed, g_failures);
    return g_failures ? 1 : 0;
}
