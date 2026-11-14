# Sweep JavaScript Subsystem (Phase 3)

Status: **Implemented (host-test-only)** with Duktape 2.7.0. This document describes what exists in the tree today, its boundaries, and what is not claimed. Categories used below: **Implemented**, **Partial**, **Planned**, **Blocked**.

## 1. Engine selection

**Implemented:** Duktape 2.7.0, vendored in `sweep/third_party/duktape/` as the single amalgamated `duktape.c` + `duktape.h` + `duk_config.h`, MIT License (see `LICENSE.txt`). Source: upstream release tarball published at `https://github.com/svaarala/duktape/releases/download/v2.7.0/duktape-2.7.0.tar.xz`.

Why Duktape (evidence-based, recorded here so the choice can be audited):
- C embedding surface: one translation unit, no external dependencies, C89-compatible source that compiles under the project's C99 baseline.
- Embedded deployment: designed for small heaps; custom allocator entry points make a hard memory ceiling possible (Sweep reserves 1 MiB per context by default).
- Deterministic teardown: heap destroy is explicit and complete; one heap per document context gives strong global/DOM/lifecycle isolation without shared-globals leakage.
- Execution bounds: Duktape's interrupt counter + exec-timeout hook allow a wall-clock deadline (Sweep patches `duk_config.h` with `DUK_USE_INTERRUPT_COUNTER` and `DUK_USE_EXEC_TIMEOUT_CHECK(udata) sweep_js_engine_timeout_check((udata))`).
- ECMAScript coverage: ES5.1 plus selected ES6 features. This is sufficient for the mediated browser API surface (DOM access as plain objects, functions, prototypes, `JSON`, `RegExp`, `Array`, etc.) and is what the `[engine]` tests exercise against a real engine — not a mock.
- Replaceable behind a narrow boundary: all Duktape types are hidden behind opaque `SWEEP_JS_ENGINE` in `sweep_js_engine.{h,c}`; DOM/events/browser code never includes engine headers.

Alternatives considered and set aside for this phase: engines that require C++ build lanes, larger runtimes, external dependencies, or network/runtime libraries would complicate the host-only, C99, `-Werror` build. They remain **Planned** as future evaluations only if Duktape's ES coverage or performance becomes a demonstrated blocker; no other engine is vendored or claimed.

**Freestanding status (R4, partial):** a freestanding Duktape build now exists under `sweep/freestanding/` — the vendored engine compiles with `-ffreestanding -nostdlib -fno-builtin`, allocates only through `SWEEP_RUNTIME`/an arena, and links with **zero undefined hosted symbols** (`make sweep-freestanding`). It passes a smoke script in a host harness. It is still **not linked into the kernel image and not executed by QEMU/KAPA**, so JS remains host-test-only for the kernel target. The math/printf shims are minimal approximations, not full libm/CRT. See `docs/SWEEP_RUNTIME_PORTABILITY.md` §3.

## 2. Architecture

| Module | Responsibility |
|---|---|
| `sweep/sweep_js_engine.{h,c}` | Engine adapter. Opaque heap, custom allocator with ceiling (`memory_used`/`memory_limit`), value-stack helpers, property/stash/accessor/prototype helpers, cfunc trampoline (max 96, deduplicated by function pointer), bounded eval with value/error/`is_undefined`, wall-clock deadline, hides the engine-specific `Duktape` global. Only file that includes engine headers. |
| `sweep/sweep_js.{h,c}` | Runtime + per-document contexts. Bounded contexts (max 8), config (`memory_limit`, `timeout_ms`, `text_mode`, `fetch` provider), error classification, inline `<script>` collection/execution, external fetch via provider, MIME filtering, defer/async observation stats. |
| `sweep/sweep_js_dom.{h,c}` | Mediated DOM bindings over the existing `sweep_html` DOM (no second DOM). Wrapper cache with hidden node id, prototypes, getters/setters, tree mutation, queries, `innerHTML` via fragment parse + bounded serialization. |
| `sweep/sweep_js_events.{h,c}` | `Event` constructor/object, bounded listener storage (32 listeners total per context), capture/target/bubble dispatch, `preventDefault`/`stopPropagation`/`stopImmediatePropagation`, recursion guard (depth 4). |
| `sweep/sweep_js_text.{h,c}` | Explicit CESU-8/UTF-8 boundary conversion with `WEB_COMPAT` / `NATIVE_SU` modes (see §4). |
| `sweep/sweep_js_internal.h` | Shared context/listener definitions and text-boundary helpers across DOM/events/runtime. |
| `sweep/sweep_html.{h,c}` (existing) | DOM source of truth. Extended for script integration: raw-text tokenizer state for `script`/`style`, mutation/fragment/serialization/query helpers, revision/dirty flags, hard bounds checks, detaching (non-freeing) removal so wrappers cannot use-after-free. |
| `sweep/sweep_css.{h,c}` | Reused for `querySelector`/`querySelectorAll` via `sweep_css_parse_selector` (single selector; comma lists rejected as unsupported). |

Per-document isolation: one independent Duktape heap per `SWEEP_JS_CONTEXT`, bound to one `SWEEP_DOM`. Wrappers, listeners, and globals live in that heap only. Destroying a context frees its heap, wrappers, and listeners; the DOM itself is owned by the caller and is never freed by the JS layer. Same-origin/cross-origin policy is **Planned** (future); today isolation is by document/context, not by origin.

Engine-global hiding: after heap creation the adapter deletes the engine-specific `Duktape` global property. The engine pointer is kept in the heap global stash under `__sweep_engine_ptr`, which is not reachable from script code. Scripts see only standard ECMAScript built-ins plus the installed `document` and `Event` globals.

## 3. API coverage

### Implemented
- Runtime: `sweep_js_runtime_init` / `sweep_js_runtime_shutdown`, `sweep_js_context_create` / `sweep_js_context_destroy`, `sweep_js_evaluate` / `sweep_js_evaluate_cstr`, `sweep_js_run_document_scripts`, memory/timeout accessors, `sweep_js_context_last_error`.
- Document: `document`, `documentElement`, `body`, `title` (get/set), `getElementById`, `getElementsByTagName`, `querySelector`, `querySelectorAll`, `createElement`, `createTextNode`.
- Node/element: `id`, `className`, `textContent` (get/set), `innerHTML` (get/set), `tagName`, `parentNode`, `children`, `firstChild`, `lastChild`; `getAttribute`, `setAttribute`, `removeAttribute`, `appendChild`, `removeChild`, `insertBefore`, `querySelector`, `querySelectorAll`.
- Events: `new Event(type, {bubbles, cancelable})`, `type`/`target`/`currentTarget`/`bubbles`/`cancelable`/`defaultPrevented`, `preventDefault`, `stopPropagation`, `stopImmediatePropagation`, `addEventListener`, `removeEventListener`, `dispatchEvent` with capture/target/bubble phases.
- Script execution: inline classic `<script>` in document order; empty scripts skipped; non-JavaScript `type` skipped; `type="module"` reported as unsupported; external `<script src>` only via `SWEEP_JS_RESOURCE_FETCH` (local-resource provider) with MIME filtering.
- Errors: syntax/runtime/timeout/memory/limit/argument/unsupported/resource/engine statuses with bounded diagnostics; JS errors surface as catchable JS exceptions where the DOM/event operation raises (`TypeError`, `RangeError`, `Error`).

### Partial
- `innerHTML`: setter parses a fragment with the existing HTML parser; getter serializes with a bounded serializer (8192 bytes). Entities are handled for `&<>` on text nodes in serialization. Full HTML5 fragment algorithm, SVG/MathML, template contents, and full entity table are **not** implemented.
- `querySelector`/`querySelectorAll`: single selector only via the existing CSS selector parser/matcher. Comma-separated lists raise an unsupported `TypeError`. Pseudo-classes/elements follow the CSS engine's fail-closed behavior (never match) rather than browser semantics.
- `getElementsByTagName` / `querySelectorAll`: return **snapshot** arrays, not live `HTMLCollection`/`NodeList`. Mutating the DOM does not retro-update a previously returned array.
- Event dispatch on `document`: works as a dispatch target (sets `target` to the document wrapper). Full `Window` event model (`window` object, load/unload/resize) is **not** implemented.
- Text nodes: `createTextNode` + `appendChild`/`insertBefore` supported; `textContent` get/set covered. `childNodes` is intentionally absent (returns `undefined`) — see §7.

### Planned
- Same-origin/cross-origin context policy + CSP hooks once networking/origins exist.
- True defer/async scheduling (today defer/async are **observed, not scheduled** — scripts run synchronously in document order; see stats `saw_defer`/`saw_async`).
- Modules (`type="module"`), dynamic `import()`, workers, WebSocket/fetch/XHR bindings.
- Layout/paint invalidation consumers: DOM mutations set `revision` + node/style/layout/paint dirty state; no renderer consumes those flags yet.
- Freestanding-kernel JS build if a suitable engine surface is demonstrated.

### Blocked
- External `<script src>` over a real network: no TCP/TLS/DNS/socket path exists in this tree; external loading only works through an explicitly registered local-resource provider and must not be described as internet fetch.
- Kernel-image integration: hosted-libc dependency (see §1).
- Desktop presentation of JS-driven updates: no graphics/windowing/rasterizer path consumes the dirty flags yet.

### Explicitly unsupported (not stubs)
The following are absent, not fake-implemented: `childNodes`, `window` as a global object, `MouseEvent`/other event constructors beyond `Event`, `XMLHttpRequest`, `fetch`, `importScripts`, WebSQL/DOM Storage, `MutationObserver`, `ResizeObserver`, full ANSI-C / engine debug globals. Unsupported operations raise `TypeError`/`RangeError` or leave the property `undefined`; no misleading stub objects are returned.

## 4. SuperUnicode compatibility

Duktape stores strings in extended UTF-8 (CESU-8 for non-BMP: a surrogate pair encoded as two 3-byte sequences). Sweep's DOM stores web text as UTF-8 bytes. `sweep_js_text` makes the conversion explicit and mode-dependent; there is no silent native↔UTF-16 round-trip.

- **WEB_COMPAT** (default): ECMAScript/UTF-16 code-unit semantics at the web boundary. CESU-8 surrogate pairs combine into one Unicode scalar for the DOM; unpaired surrogates become U+FFFD. UTF-8 scalars above U+FFFF encode to CESU-8 for the engine.
- **NATIVE_SU**: 16-bit values, including 0xD800–0xDFFF, are ordinary SUCS private-use values. They are **never** paired, rejected, or reinterpreted — per SUTR-0 §4.1 / SUTR-1 §1.2 these are valid PUA codepoints, and `sweep_utf8_decode`/`sweep_utf8_encode` (plus the document's CESU-aware transport) accept and round-trip the whole block in NATIVE_SU. Values above U+FFFF are not representable as one 16-bit engine character; helpers replace them (documented limitation, not a surrogate conversion).
- DOM storage remains UTF-8 bytes in both modes; the boundary helpers govern how engine characters map to DOM bytes and back. Glyph rendering is separate and unchanged: layout/paint still transport text as UTF-8 bytes; true native SUTF/SUCS byte transport through layout/paint is **Blocked / unimplemented** (documented limitation, not a conversion).

## 5. Security

- **Implemented boundary:** Scripts receive only the installed `document` and `Event` globals plus standard ECMAScript built-ins. There is no binding to KDB internals, kernel memory/execution, physical memory, arbitrary filesystem/devices, or OpenWindows management. Tests assert `typeof KDB`, `kernel`, `process`, `require`, `alert`, `Duktape`, `fs`, `socket`, `XMLHttpRequest`, etc. are `undefined`.
- **Validated transitions:** Every JS↔DOM transition validates node identity against the owning context's wrapper cache (hidden node id + prototype check). Plain objects, forged wrappers, and prototype-poisoned fakes are rejected with `TypeError`. Cycles (self/ancestor) are rejected; attribute names, tag names, selector strings, and event types are length-checked before reaching the DOM/CSS layers.
- **Bounded execution:** Per-context heap ceiling (default 1 MiB), wall-clock deadline (default 2000 ms, checked via Duktape's interrupt counter), 32 listeners total per context, dispatch depth 4, 65,536-byte source cap, 4,096-byte result cap, 256-byte error cap. Allocation failure, timeout, and recursion raise catchable errors; the browser process does not crash and other documents are unaffected.
- **Documented missing protections (not claimed):** No full sandbox, no origin/SOP enforcement, no CSP, no TLS/cert validation, no network authority, and no MMU/VM isolation. The heap ceiling and deadline bound resource use but are not a security sandbox; a malicious script inside its own context can still consume its full heap/time budget and perform any mediated DOM action within the caps. Do not present this subsystem as production sandboxing for untrusted web content; it is a bounded, mediated API surface for host-tested, same-context documents.

## 6. Build and tests

From the Sweep root:

```sh
make sweep-tests
```

This compiles the vendored Duktape amalgamation once with relaxed warnings (`-std=c99 -O2 -w -I sweep/third_party/duktape`, third-party code outside our `-Werror` surface) and compiles all Sweep JS sources plus the suite with `-std=c99 -Wall -Wextra -Werror -Isweep`. The vendored engine is not added to the kernel image; the kernel `make -j 2` target does not depend on Duktape.

Test ownership (labels in `sweep/tests/sweep_js_test.c`):
- `[adapter]` runtime/context lifecycle, limits, config defaults — **not ECMAScript**; verifies the harness, not JS semantics.
- `[engine]` real Duktape-backed ECMAScript (arithmetic, functions/closures, `JSON`, `RegExp`, try/catch, syntax/runtime/timeout/memory errors). These are genuine engine executions, not simulated results.
- `[dom]` mediated DOM bindings and error paths.
- `[events]` bounded dispatch semantics.
- `[text]` explicit SuperUnicode boundary, both modes, unit + end-to-end (engine string literal → DOM → result).
- `[script]` inline execution order, MIME filtering, external provider loading, defer/async observation.
- `[security]` host/network global absence and per-context isolation.

Current verified result (R7, 2026-11-14): Sweep suites **1007 passed, 0 failed** total (engine 24, services 18, text 37, browser 39, HTML 40, CSS 208, JS 187, document 258, resource 15, runtime 60, render 121). Kernel `make hosttest` (this repo): **1424 passed, 0 failed**. Full/reduced kernel builds succeed. The freestanding Duktape object build passes its smoke test with hosted-symbol closure OK — and (R6/R7) the same Duktape object set is packaged as `kapa_sweep.owx` and executes JavaScript at CPL3 in QEMU, including the DOM path (`script 1 ok result=45`, `script 2 ok (duktape smoke)`, `dom js ok result=r7-ok`), asserted by `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` and `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1`; see `docs/SWEEP_R6_REPORT.md`, `docs/SWEEP_R7_REPORT.md`.

## 7. Known limitations

- Caps: 128 nodes / 32 children / 16 attributes. Phase 4 added slot generations + subtree reclamation for `textContent`/`innerHTML` replacement (repeated `innerHTML` no longer exhausts the cap; stale wrappers/listeners are rejected by generation). `removeChild`-detached nodes stay re-appendable and un-reclaimed (whole-doc caps still apply to that path; see `docs/SWEEP_DOCUMENT_LIFECYCLE.md` §4).
- `childNodes` is absent by design (only element children via `children`, text via `textContent`/`firstChild`/`lastChild`). No `Node` interface hierarchy, no `ownerDocument`, no ranges, no shadow DOM.
- No `window` object: scripts run in a document-global scope; `window === this` semantics and browser chrome objects are not provided.
- Evaluation results are stringified and bounded (NUL-containing result strings truncate at the C boundary); callers needing binary-safe values must use DOM-side effects, not result strings.
- Engine strings in NATIVE_SU cannot represent scalars above U+FFFF faithfully (see §4).
- Listener state counts toward one 32-slot budget per context (not per node); dispatch during dispatch is depth-capped and throws past depth 4.

## 8. Next recommended phase

Wire DOM dirty/revision propagation to a real style/layout/paint consumer, add a local-resource provider implementation for file-based script loading (still no network fetch claim), prototype origin policy on top of context isolation, and carry the freestanding runtime into a validated KAPA image. The freestanding object build is proven; what remains is kernel-backed runtime ops and the KAPA execution path, not the engine port itself. Do not add fetch/modules/workers until a network/origin design exists.
