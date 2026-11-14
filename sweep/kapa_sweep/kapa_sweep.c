/* kapa_sweep.c - the Sweep freestanding runtime as a native KAPA application.
 *
 * R6/R7: this is the actual freestanding Sweep runtime (the R4 Duktape-over-
 * SWEEP_RUNTIME port, unmodified in behavior) executed at CPL3 through the
 * R5-proven controlled execution path.  R7 extends it with the actual Sweep
 * HTML parser and DOM implementation (sweep_html.c), driven by Duktape at
 * CPL3 through the narrow, validated binding in kapa_dom_js.c.  It is not a
 * browser and it does not load CSS/layout/rendering: it proves that the
 * parser, the DOM and the JavaScript bridge the browser will stand on can
 * run inside a KAPA process.
 *
 * Startup ABI (the only entry contract, inherited from the OWX/PS launcher):
 *   RCX = argc, RDX = argv, R8 = envp   (Microsoft x64, like every .owx)
 * The image keeps no hosted dependencies and no unrestricted kernel reach:
 * every service call goes through the restricted KAPA gateway (vector 0x80 in
 * this profile) and only the services below are used:
 *
 *   main.log           - attributed diagnostics (message bytes are data,
 *                        never format directives)
 *   main.runtime_info  - 8-byte LE monotonic millisecond uptime
 *
 * The runtime talks to SWEEP_RUNTIME only; the ops are wired to the
 * freestanding arena (allocation/reallocation) and to the two services above
 * (clock and diagnostics).  No resource-read service exists in R7 yet, so
 * read_resource is deliberately left unimplemented (deterministic NULL).
 *
 * Exit status is defined so the boot log can distinguish every failure stage:
 *   0xE0 bad startup arguments      0xE1 arena init failed
 *   0xE2 runtime bind failed        0xE3 Duktape heap creation failed
 *   0xE4 JS script 1 failed         0xE5 JS smoke script failed
 *   0xE6 arena not fully released   0xD0 DOM parse failed
 *   0xD1 DOM binding install failed 0xD2 DOM JavaScript execution failed
 *   0xD3 C-side DOM verification failed
 *   0x5A success
 */
#include "duktape.h"
#include "sweep_fs_arena.h"
#include "sweep_runtime.h"
#include "sweep_html.h"
#include "kapa_dom_js.h"
#include "kapa_dom_fixture.h"
#include "../../emergency/ow_gate.h"
#include <string.h>

#define FS_ARENA_BYTES (192u * 1024u)

static unsigned char s_arena_storage[FS_ARENA_BYTES] __attribute__((aligned(16)));
static SWEEP_DOM     g_dom;           /* .bss; inline bounded node table      */

/* ---- bounded log helpers (message bytes are data, never directives) ----- */

static void kapa_log(const char* msg) {
    OW_SYS_KDB_BRIDGE_CALL call;
    unsigned long len = 0UL;

    while (msg && msg[len] != '\0') ++len;

    call.Magic = OW_SYS_KDB_BRIDGE_MAGIC;
    call.Version = OW_SYS_KDB_BRIDGE_VERSION;
    call.ServiceId = OW_KDB_SERVICE_LOG;
    call.Flags = 0U;
    call.RequestLength = (uint32_t) len;
    call.ResponseCapacity = 0U;
    call.ResponseLength = 0U;
    call.TargetInstanceId = 0U;
    call.RequestAddress = (uint64_t) (uintptr_t) msg;
    call.ResponseAddress = 0U;
    call.Reserved = 0U;
    (void) ow_gate_bridge(&call);
}

static void kapa_fmt_u64(char* out, size_t cap, uint64_t v) {
    char tmp[24];
    size_t i = 0U, j = 0U;

    if (cap == 0U) return;
    if (v == 0U) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (v != 0U && i < sizeof(tmp)) {
        tmp[i++] = (char) ('0' + (int) (v % 10ULL));
        v /= 10ULL;
    }
    while (i > 0U && j + 1U < cap) {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';
}

static void kapa_log_prefix_u64(const char* prefix, uint64_t value,
                                const char* suffix) {
    char buf[128];
    char num[24];
    size_t i = 0U;
    size_t j = 0U;

    while (prefix && prefix[i] != '\0' && j + 1U < sizeof(buf)) {
        buf[j++] = prefix[i++];
    }
    kapa_fmt_u64(num, sizeof(num), value);
    i = 0U;
    while (num[i] != '\0' && j + 1U < sizeof(buf)) {
        buf[j++] = num[i++];
    }
    i = 0U;
    while (suffix && suffix[i] != '\0' && j + 1U < sizeof(buf)) {
        buf[j++] = suffix[i++];
    }
    buf[j] = '\0';
    kapa_log(buf);
}

/* ---- main.runtime_info: 8-byte LE monotonic millisecond uptime --------- */

static uint64_t kapa_uptime_ms(void) {
    uint8_t resp[8];
    OW_SYS_KDB_BRIDGE_CALL call;
    uint32_t k;
    uint64_t v = 0ULL;

    call.Magic = OW_SYS_KDB_BRIDGE_MAGIC;
    call.Version = OW_SYS_KDB_BRIDGE_VERSION;
    call.ServiceId = OW_KDB_SERVICE_RUNTIME_INFO;
    call.Flags = 0U;
    call.RequestLength = 0U;
    call.ResponseCapacity = 8U;
    call.ResponseLength = 0U;
    call.TargetInstanceId = 0U;
    call.RequestAddress = 0U;
    call.ResponseAddress = (uint64_t) (uintptr_t) resp;
    call.Reserved = 0U;
    if (ow_gate_bridge(&call) != 0) {
        return 0ULL; /* deterministic failure: zero clock, never a lie */
    }
    for (k = 0U; k < 8U; ++k) {
        v |= ((uint64_t) resp[k]) << (k * 8U);
    }
    return v;
}

/* ---- Sweep runtime ops backed by the freestanding arena + gateway ------ */

static void* kapa_alloc(void* user, size_t size) {
    return sweep_fs_arena_alloc((SWEEP_FS_ARENA*) user, size);
}

static void* kapa_realloc(void* user, void* ptr, size_t size) {
    return sweep_fs_arena_realloc((SWEEP_FS_ARENA*) user, ptr, size);
}

static void kapa_free(void* user, void* ptr) {
    sweep_fs_arena_free((SWEEP_FS_ARENA*) user, ptr);
}

static uint64_t kapa_monotonic(void* user) {
    (void) user;
    return kapa_uptime_ms();
}

static void kapa_diag(void* user, const char* text) {
    (void) user;
    kapa_log(text ? text : "(null diagnostic)");
}

static SWEEP_RUNTIME_OPS kapa_ops;       /* runtime-populated; see below */

/* Duktape allocator hooks routed through SWEEP_RUNTIME (never libc). */
static void* fs_duk_alloc(void* udata, duk_size_t size) {
    return sweep_runtime_alloc((SWEEP_RUNTIME*) udata, (size_t) size);
}

static void* fs_duk_realloc(void* udata, void* ptr, duk_size_t size) {
    return sweep_runtime_realloc((SWEEP_RUNTIME*) udata, ptr, (size_t) size);
}

static void fs_duk_free(void* udata, void* ptr) {
    sweep_runtime_free((SWEEP_RUNTIME*) udata, ptr);
}

#define KAPA_JS_SCRIPT_1 "(function(){var r=0;for(var i=0;i<10;i++){r+=i;}return r;})()"

static const char KAPA_JS_SCRIPT_2[] =
    "(function () {"
    "  var a = [1, 2, 3].map(function (x) { return x * 2; });"
    "  var o = { n: 0 };"
    "  for (var i = 0; i < 10; i++) { o.n += i; }"
    "  var ok = a.join(',') === '2,4,6' && o.n === 45;"
    "  ok = ok && JSON.parse('{\"x\":1}').x === 1;"
    "  ok = ok && /ab+c/.test('abbbc');"
    "  ok = ok && (function () {"
    "      try { null.x; } catch (e) { return e instanceof TypeError; }"
    "      return false;"
    "  })();"
    "  ok = ok && (0.1 + 0.2) > 0.3;"
    "  ok = ok && String(123) === '123';"
    "  ok = ok && typeof Date.now() === 'number';"
    "  return ok ? 'ok' : 'bad';"
    "})()";

static const char KAPA_JS_DOM_SCRIPT[] =
    "(function () {"
    "  var el = document.getElementById('marker');"
    "  if (el === null || el === undefined) { throw new Error('marker missing'); }"
    "  if (el.nodeName !== 'div') { throw new Error('nodeName mismatch: ' + el.nodeName); }"
    "  if (el.getAttribute('data-id') !== 'r7-marker') { throw new Error('data-id mismatch'); }"
    "  if (el.getAttribute('class') !== 'probe') { throw new Error('class mismatch'); }"
    "  var t = el.textContent;"
    "  if (t !== 'hello sweep dom') { throw new Error('textContent mismatch: ' + t); }"
    "  var kids = el.children;"
    "  if (!kids || kids.length !== 1) { throw new Error('children length mismatch'); }"
    "  if (kids[0].nodeName !== 'span') { throw new Error('child nodeName mismatch: ' + kids[0].nodeName); }"
    "  el.setTextContent('r7-js-mutated');"
    "  if (el.textContent !== 'r7-js-mutated') { throw new Error('mutation mismatch'); }"
    "  return 'r7-ok';"
    "})()";

static void kapa_exit(int code) {
    (void) ow_gate_call(OW_GATE_PS_EXIT, (int64_t) code, 0, 0);
}

int kapa_sweep_main(int argc, char** argv, char** envp) {
    SWEEP_FS_ARENA arena;
    SWEEP_RUNTIME rt;
    duk_context* ctx;
    const char* result;
    double number;
    int rc;

    (void) envp; /* no environment in this profile; never dereferenced */

    /* Startup ABI validation: RCX = argc, RDX = argv per the OWX contract. */
    if (argc < 1 || argv == (char**) 0) {
        kapa_log("kapa-sweep: bad startup args");
        kapa_exit(0xE0);
        return 0xE0; /* not reached: the exit syscall terminates the thread */
    }

    /* Runtime: bounded arena first, then the injected runtime binding. */
    if (!sweep_fs_arena_init(&arena, s_arena_storage, sizeof(s_arena_storage))) {
        kapa_log("kapa-sweep: arena init failed");
        kapa_exit(0xE1);
        return 0xE1;
    }
    kapa_ops.alloc = kapa_alloc;
    kapa_ops.realloc = kapa_realloc;
    kapa_ops.free = kapa_free;
    kapa_ops.monotonic_ms = kapa_monotonic;
    kapa_ops.diag = kapa_diag;
    kapa_ops.read_resource = NULL; /* no such KAPA service exists yet */
    kapa_ops.init = NULL;
    kapa_ops.shutdown = NULL;
    memset(&rt, 0, sizeof(rt));
    if (!sweep_runtime_init(&rt, &kapa_ops, &arena)) {
        kapa_log("kapa-sweep: runtime bind failed");
        kapa_exit(0xE2);
        return 0xE2;
    }
    kapa_log_prefix_u64("kapa-sweep: runtime initialized, arena ",
                        (uint64_t) sizeof(s_arena_storage), " bytes");

    /* Actual Duktape execution at CPL3. */
    ctx = duk_create_heap(fs_duk_alloc, fs_duk_realloc, fs_duk_free, &rt, NULL);
    if (ctx == NULL) {
        kapa_log("kapa-sweep: duktape heap creation failed");
        kapa_exit(0xE3);
        return 0xE3;
    }

    /* Script 1: deterministic numeric result, retrieved and verified. */
    rc = duk_peval_string(ctx, KAPA_JS_SCRIPT_1);
    if (rc != 0 || !duk_is_number(ctx, -1)) {
        kapa_log("kapa-sweep: script 1 failed (eval or type)");
        duk_destroy_heap(ctx);
        kapa_exit(0xE4);
        return 0xE4;
    }
    number = duk_get_number(ctx, -1);
    duk_pop(ctx);
    if (number != 45.0) {
        kapa_log_prefix_u64("kapa-sweep: script 1 wrong result=",
                            (uint64_t) number, "");
        duk_destroy_heap(ctx);
        kapa_exit(0xE4);
        return 0xE4;
    }
    kapa_log("kapa-sweep: script 1 ok result=45");

    /* Script 2: the R4 freestanding smoke, executed by the real engine. */
    rc = duk_peval_string(ctx, KAPA_JS_SCRIPT_2);
    result = (rc == 0) ? duk_safe_to_string(ctx, -1) : (const char*) 0;
    if (rc != 0 || result == NULL || result[0] != 'o' || result[1] != 'k'
        || result[2] != '\0') {
        kapa_log("kapa-sweep: script 2 (duktape smoke) failed");
        duk_destroy_heap(ctx);
        kapa_exit(0xE5);
        return 0xE5;
    }
    duk_pop(ctx);
    kapa_log("kapa-sweep: script 2 ok (duktape smoke)");

    /* Teardown phase 1: heap destruction must drain the arena completely. */
    duk_destroy_heap(ctx);
    if (arena.used != 0u) {
        kapa_log_prefix_u64("kapa-sweep: arena not drained, phase 1 used=",
                            (uint64_t) arena.used, "");
        kapa_exit(0xE6);
        return 0xE6;
    }

    /* ---- Phase 2 (R7): HTML/DOM/JS integration on the same runtime ---- */
    {
        uint32_t doc;
        uint32_t marker = 0u;
        const SWEEP_DOM_NODE* marker_node = (const SWEEP_DOM_NODE*) 0;
        const char* attr = (const char*) 0;
        char text[SWEEP_HTML_TEXT_MAX];

        doc = sweep_dom_parse(&g_dom, kapa_dom_fixture, KAPA_DOM_FIXTURE_LEN);
        if (doc == 0u || g_dom.document == 0u) {
            kapa_log("kapa-sweep: dom parse failed");
            kapa_exit(0xD0);
            return 0xD0;
        }
        kapa_log_prefix_u64("kapa-sweep: dom parsed, nodes=",
                            (uint64_t) g_dom.count, "");

        kapa_log("kapa-sweep: dom heap creating");
        ctx = duk_create_heap(fs_duk_alloc, fs_duk_realloc, fs_duk_free,
                              &rt, NULL);
        if (ctx == NULL) {
            kapa_log("kapa-sweep: duktape heap creation failed");
            kapa_exit(0xE3);
            return 0xE3;
        }
        if (!kapa_dom_js_install(ctx, &g_dom)) {
            kapa_log("kapa-sweep: dom js binding failed");
            duk_destroy_heap(ctx);
            kapa_exit(0xD1);
            return 0xD1;
        }
        kapa_log("kapa-sweep: dom js bound");

        rc = duk_peval_string(ctx, KAPA_JS_DOM_SCRIPT);
        result = (rc == 0) ? duk_safe_to_string(ctx, -1) : (const char*) 0;
        if (rc != 0 || result == NULL ||
            result[0] != 'r' || result[1] != '7' || result[2] != '-') {
            kapa_log("kapa-sweep: dom js execution failed");
            duk_destroy_heap(ctx);
            kapa_dom_js_release();
            kapa_exit(0xD2);
            return 0xD2;
        }
        duk_pop(ctx);
        kapa_log("kapa-sweep: dom js ok result=r7-ok");

        /* Independent C-side verification of the JavaScript mutation. */
        marker = sweep_dom_find_by_id(&g_dom, g_dom.document, "marker");
        marker_node = sweep_dom_get(&g_dom, marker);
        attr = marker_node ? sweep_dom_get_attr(marker_node, "data-id")
                           : (const char*) 0;
        text[0] = '\0';
        if (marker != 0u) {
            sweep_dom_text_content(&g_dom, marker, text, sizeof(text));
        }
        if (marker == 0u || marker_node == (const SWEEP_DOM_NODE*) 0 ||
            attr == (const char*) 0 || strcmp(attr, "r7-marker") != 0 ||
            strcmp(text, "r7-js-mutated") != 0) {
            kapa_log("kapa-sweep: dom c-side verification failed");
            duk_destroy_heap(ctx);
            kapa_dom_js_release();
            kapa_exit(0xD3);
            return 0xD3;
        }
        kapa_log("kapa-sweep: dom verified (text=r7-js-mutated)");

        duk_destroy_heap(ctx);
        kapa_dom_js_release();
        kapa_log("kapa-sweep: dom destroyed");
        if (arena.used != 0u) {
            kapa_log_prefix_u64("kapa-sweep: arena not drained, phase 2 used=",
                                (uint64_t) arena.used, "");
            kapa_exit(0xE6);
            return 0xE6;
        }
    }

    sweep_runtime_shutdown(&rt);
    kapa_log("kapa-sweep: shutdown ok");

    kapa_exit(0x5A);
    return 0x5A; /* unreachable: the exit syscall retired this thread */
}