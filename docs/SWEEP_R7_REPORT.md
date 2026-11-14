# Sweep R7 Report - Native HTML and DOM Integration Inside the Freestanding KAPA Runtime

**Date:** 2026-11-14  
**Status:** **Complete.** The actual Sweep HTML parser and DOM implementation are compiled into the freestanding KAPA runtime and drive a real document through JavaScript at CPL3. One canned HTML fixture is parsed by `sweep_dom_parse`; JS queried, mutated and read back through the real DOM APIs; the C side verified the mutation independently through the DOM C API. A second phase alone is re-runnable: the R6 runtime path still works unchanged.

## 1. R6 baseline verification (reproduced 2026-11-14)

| Check | R6 recorded | Reproduced (2026-11-14, this tree) |
|---|---|---|
| `make -B -j 2` | Success | **Success** (`openwinkrnl.owx` 1,662,976 B, SHA-256 `6fb140a…`, unchanged) |
| `make sweep-kernel` | Success | **Success** (`sweepwinkrnl.owx` 1,531,904 B, SHA-256 `7c699ddc…`) |
| `make sweep-tests` | 1007/0 | **1007 passed, 0 failed** |
| `make hosttest` | 1424/0 | **1424 passed, 0 failed** |
| `make sweep-freestanding` | exit 0 | **Exit 0**, `nm -u` closure OK |
| `make check-external-deps` (4 profiles) | PASS | **All OK** |
| `make emergency` | PASS | **PASS** |
| `make qemu-sweep-kernel` | PASS | **PASS** |
| `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | PASS | **PASS** |
| `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` | PASS | **PASS** (R6 runtime preserved; R7 extends the image) |
| `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` | PASS | **PASS** (the R6 fault attribution payload, unchanged) |
| `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1` | - | **PASS** (58 assertions) |

Artifact hashes and sizes are in §12. No sibling repositories were modified; the working tree retains the R5/R6 delivery, plus the R7 changes listed in §17.

## 2. HTML parser inventory (Workstream A)

| Component | File(s) | Notes |
|---|---|---|
| Tokenizer | `sweep/sweep_html.c`, `sweep_html.h` | `SWEEP_HTML_TOKENIZER`, `SWEEP_HTML_TOKEN`, `sweep_html_tokenizer_init/next_token`; bounded 1024-byte text buffer, 48-byte names, attribute caps 16 × 256 B |
| DOM | same `sweep_html.h` | `SWEEP_DOM`: inline `SWEEP_DOM_NODES_MAX = 128` nodes, `SWEEP_DOM_CHILDREN_MAX = 32`, generation counter per slot, `SWEEP_DOM_SERIALIZE_MAX = 8192` |
| Attr/name limits | `sweep_html.h` | `SWEEP_HTML_NAME_MAX 48`, `SWEEP_HTML_ATTR_MAX 16`, `SWEEP_HTML_ATTR_VAL_MAX 256`, `SWEEP_HTML_TEXT_MAX 1024` |
| Tokenizer states | `sweep_html.h` (`SWEEP_HTML_DATA` … `SWEEP_HTML_RAW_CLOSE`) | real state machine incl. raw-text elements and comments |
| Mutation API | `sweep_html.c` | create/text/element/comment, append/insert/remove, set_attr/remove_attr, set_text_content, innerHTML set/get, dirty/mutation tracking |
| Find APIs | `sweep_html.c` | `sweep_dom_get`, `sweep_dom_find_element`, `sweep_dom_find_by_id`, `sweep_dom_get_attr`, `sweep_dom_text_content`, `sweep_dom_node_generation` |
| Freestanding surface | `sweep_html.h` includes only `sweep_text.h`, plus stdbool/stddef/stdint | no malloc anywhere in the DOM/tokenizer; all inline |
| Host tests | `sweep/tests/sweep_html_test.c`, `sweep/tests/sweep_document_test.c` | 40 + 258 assertions pass; DOM semantics preserved |
| Third-party | none | parser is Sweep's own |

Parser input model: a caller-provided `const char*` + `size_t len` — **no file or network dependency**, exactly the canned-fixture shape R7 needs. The implementation is self-contained (no `stdio.h`, no `stdlib.h`, no dynamic allocation) and uses bounded stack buffers. It is not incremental, but the milestone does not require incremental parsing.

Tokenizer/DOM include behaviour and caps are preserved byte-for-byte; this milestone changed neither.

## 3. DOM inventory

- Node types: DOCUMENT / ELEMENT / TEXT / COMMENT (`SWEEP_DOM_DOCUMENT=1`). Lifecycle: `sweep_dom_parse` initializes, parses from caller buffer, returns document node id; `sweep_dom_init` for reuse; nodes carry `generation` bumped per slot recycle.
- `SWEEP_DOM` carries `document` id, `revision`, dirty flags, mutation details (kind/attr/class/id). Bounded inline; nothing calls malloc or free.
- Text/serialize buffers are caller-supplied caps; there is no global static.
- Safety: `sweep_dom_get` bounds-checks `id` against `SWEEP_DOM_NODES_MAX` (1..128); tree mutations keep nodes detached or reclaimed consistently (slot freed + generation bumped).

## 4. Freestanding dependency classification (Workstream A.C)

| Dependency | Class | Action |
|---|---|---|
| `sweep_html.c` / `sweep_text.c` need `string.h`, `ctype.h` | Already inline bounded | Provided freestanding `string.h`/`ctype.h` headers in `sweep/freestanding/` (ASCII-only, documented as deliberate) |
| `ctype` functions | Hosted typeclass subset | Added `tolower/toupper/isalpha/isalnum/isdigit/isspace/isctrl` to `sweep_freestanding_libc.c` (ASCII-only; not a Unicode redesign) |
| Allocator | `SWEEP_RUNTIME` (R4) + arena | KAPA image wired alloc/realloc/free to the 192 KiB arena already carried |
| `strcmp`, `memset`, `memcpy`, `strlen`, `strstr` | Freestanding shim | Already present; signatures relied upon by the DOM and binding code |
| `sweep_document.c` / `sweep_document.h` | Hosted; uses `stdio.h`/`stdlib.h`/`sweep_js.h` | **Deferred for R7** — not compiled into the image (the milestone needs only the bare DOM) |
| `sweep_js_dom.c` (host binding) | Hosted; uses `ctype.h`/`stdlib.h` `sweep_css.h` | **Deferred** by design; replaced by a **narrow** KAPA binding (`kapa_dom_js.c`) as specified |

## 5. HTML parser KAPA integration (Workstream B)

`sweep/sweep_html.c` and `sweep/sweep_text.c` are compiled directly into `kapa_sweep.owx` with the same flags as the rest of the runtime payload (PIE, `-fpie`, no hosted libc). The parser was changed not at all — behavior is inherited verbatim.

Canned fixture: `sweep/kapa_sweep/kapa_dom_fixture.h` — a tiny document with a doctype, `<html>`, `<head><title>`, `<body>`, and a nested container `<div id="marker" data-id="r7-marker" class="probe">hello <span class="inner">sweep</span> dom</div>`. It is a test fixture, not a second parser or a trimmed engine.

Fixture properties verified in the **C DOM parser directly** before wiring the JavaScript bridge:

- Node ids bound in correct order; `id` attribute `marker`, `data-id` `r7-marker`, `class` `probe` preserved;
- Text content is `hello sweep dom` (span split accounted for by `sweep_dom_text_content`);
- `sweep_dom_find_by_id(dom, dom->document, "marker")` resolves correctly;
- `sweep_dom_node_generation` id-gen matches for wrappers.

## 6. DOM KAPA integration (Workstream C)

`SWEEP_DOM g_dom;` lives in the image's `.bss` (inline node table ~762 KB; R6 frame-run raised to 384 frames to accommodate it; see §11). Lifecycle per boot:

1. `sweep_dom_parse(&g_dom, fixture, len)` → initializes, parses, returns root id.
2. Parser limits honoured: `SWEEP_DOM_NODES_MAX 128`, `SWEEP_DOM_CHILDREN_MAX 32` — fixture needs ~20 nodes. Bounded; no dynamic allocation in the parser.
3. TL;DR of verification surface in the image (all real DOM APIs, no re-implementation): `sweep_dom_parse`, `sweep_dom_get`, `sweep_dom_get_attr`, `sweep_dom_text_content`, `sweep_dom_set_text_content`, `sweep_dom_destroy`-equivalent (inline table; nothing to free; `sweep_dom_init` resets).
4. Unsupported (by design, not silently): `innerHTML` reading/serialization paths are not called by the R7 test; no claim beyond the implemented subset.

## 7. JavaScript-to-DOM bindings (Workstream D)

`sweep/kapa_sweep/kapa_dom_js.c` (with `.h`) — a narrow, application-side adapter between the real Duktape context and the real DOM. No DOM symbols reach the kernel.

| JS API | Backing C call | Behavior |
|---|---|---|
| `document.getElementById(id)` | `sweep_dom_find_by_id(dom, dom.document, id)` | returns element wrapper or null |
| `element.getAttribute(name)` | `sweep_dom_get_attr(node, name)` | string or null; names must be non-empty |
| `element.textContent` (get) | `sweep_dom_text_content(dom, id, buf, cap)` | concatenated descendant text |
| `element.setTextContent(v)` | `sweep_dom_set_text_content(dom, id, v)` | v coerced to string; >1023 bytes refused via RangeError |
| `element.nodeName` (get) | direct node read | parser's lowercased tag name (as stored) |
| `element.children` (get) | iterate `node->children[]`, filter ELEMENT, wrap | Array of element wrappers, validated |

Wrapper lifetime (the safety-critical part): every wrapper object carries two **non-writable, non-enumerable, non-configurable** hidden number properties — `__sweep_node` (DOM slot id) and `__sweep_gen` (slot generation). Every access re-validates both against the DOM (`sweep_dom_get` bounds check + `sweep_dom_node_generation(dom, id) == gen`); a stale wrapper (slot reclaimed and reused) throws a TypeError instead of following a recycled node. Wrappers are never cached by the engine; JS-provided handles are never trusted. No C pointer is exposed to JS.

Errors: all JavaScript errors are thrown as normal Duktape TypeErrors/RangeErrors with bounded fixed messages ("getAttribute: expected name string", "stale DOM element wrapper", etc.); no JS-provided byte sequence is ever interpreted as anything but a string; no malloc/realloc on the binding path; attribute/text names bounded by `SWEEP_HTML_NAME_MAX` (read only); the binding itself is less than 6 KB of code.

Binding registration: `kapa_dom_js_install(ctx, &g_dom)` installs the hidden `__kapa_proto` stash on the global (non-enumerable, non-configurable) and a **`document`** global (non-config, non-enumerable; scripts may see it but not clobber it). Verified property slots taken: exactly `document` and `__kapa_proto` plus the DOM's wrapper hidden props — no other globals are added.

## 8. End-to-end QEMU evidence (Workstream E)

`build-generated/sweep_kapa_dom_qemu_boot.log` (5,012 B, 2026-11-14 13:50) and `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1` → **PASSED** (58 assertions). Markers, in boot order:

```
[KDB] instance 1: app 'sweep-kapa-runtime' loaded 'sweep' (107 page(s), domain 1)
[CIS] sweep: TRUSTED, signed by dev, licence accepted
[OWX] applied 211 base relocation(s)
[PS] sweep: image entered at CPL3, rsp 0x41FEFF8, exit 0x415C000
[KDB] instance 1: bound thread resumed on CPU (state=RUNNING)
[KDB][LOG] assoc=1 target=0 len=51: kapa-sweep: runtime initialized, arena 196608 bytes
[KDB][LOG] assoc=1 target=0 len=33: kapa-sweep: script 1 ok result=45
[KDB][LOG] assoc=1 target=0 len=39: kapa-sweep: script 2 ok (duktape smoke)
[KDB][LOG] assoc=1 target=0 len=32: kapa-sweep: dom parsed, nodes=20
[KDB][LOG] assoc=1 target=0 len=29: kapa-sweep: dom heap creating
[KDB][LOG] assoc=1 target=0 len=24: kapa-sweep: dom js bound
[KDB][LOG] assoc=1 target=0 len=34: kapa-sweep: dom js ok result=r7-ok
[KDB][LOG] assoc=1 target=0 len=45: kapa-sweep: dom verified (text=r7-js-mutated)
[KDB][LOG] assoc=1 target=0 len=25: kapa-sweep: dom destroyed
[KDB][LOG] assoc=1 target=0 len=23: kapa-sweep: shutdown ok
[PS] sweep: process terminated (PID 1, exit 90)
[SWEEP-RUNTIME] selftest instance 1 finished (state=stopped fault=0x0 exit=90)
```

The JS program executed in `kapa_sweep.c` performs: `document.getElementById('marker')` → asserts `el.nodeName === 'div'`, `getAttribute('data-id') === 'r7-marker'`, `getAttribute('class') === 'probe'`, `textContent === 'hello sweep dom'`, `children.length === 1`, `children[0].nodeName === 'span'`, then `el.setTextContent('r7-js-mutated')`, verifies `textContent` round-trips, returns `'r7-ok'`. After the JS returns, the C side re-verifies through the DOM C API: `sweep_dom_find_by_id(...)` resolves the marker; `sweep_dom_get_attr(n, "data-id") == "r7-marker"`; `sweep_dom_text_content(...)` produces the mutated text. Heap destroyed, arena drained to zero, runtime shutdown, exit 0x5A.

## 9. OWFS resource provider investigation (Workstream F)

**Contract audit.** `sweep_resource.h` (header-only, host-only provider + runtime-explicit variant): bounded path validation, root confinement, MIME allowlist by extension (.js/.mjs/.css/.html/.htm/.txt), 64 KiB per-resource cap (`SWEEP_RESOURCE_MAX_BYTES`), no traversal (`..` rejected, absolute paths, drive letters, backslashes), ASCII-only unknown-extension sniff guard; `sweep_resource_fetch_runtime` mediates everything through `rt->read_resource` — it has **no stdio dependency by itself**. That is the shape a native/KAPA backend must fill.

**Reduced-profile reality.** The reduced sweep-kernel profile intentionally excludes **VFS, storage (owdisk/OWFS/USFS/common), network, sentinel, shell, usermode, RC**. The KAPA gateway (`kdb/kdb_syscall.c`) exposes only:

- `OW_SYS_PS_GET_PID` (returns 1 or -1),
- `OW_SYS_PS_EXIT_THREAD` (terminate self),
- `OW_SYS_UART_WRITE` (debug console),
- `OW_SYS_KDB_BRIDGE` (bounded request copy → builtins: `main.ping`/`main.echo`/`main.log`/`main.runtime_info`; `TargetInstanceId` must be 0).

**There is no file service of any kind in this profile**, and the OWFS implementation compiled into the full profile requires storage bring-up (`storage/owdisk.c`, `vfs.c`, OWFS volume formatters) which is excluded here by design (that *is* the reduced profile's meaning). Re-including it would turn the reduced profile back into the full profile and grant file access to KAPAs without an explicit, root-confined policy surface.

**Therefore: an OWFS-backed `read_resource` for KAPAs is blocked in R7 as scoped.** Implementing it today would amount to:

1. Adding `OWFS` + `STORAGE_COMMON` + `owdisk` + VFS object groups to the sweep link, plus a storage boot init phase — not a small change, breaks R1/R4 exclusions, and requires re-running the full resource-root policy story; or
2. Implementing a *new* `main.resource_read` builtin backed by a **canned in-kernel blob** (like the KAPA image itself) — technically possible, deliberately out of scope (no resource contract owner, no root confinement story in the reduced profile), and contrary to "reduce, don't grow the profile".

What **is** verified present and sufficient: the runtime boundary (`SWEEP_RUNTIME.read_resource`) already exists and fails deterministically (NULL/uninitialized → false); porting it to a KAPA backend later is a single, local change. R7 recommendations §20 propose the design for R8.

## 10. JavaScript execution budgets and watchdog (Workstream G)

**Finding (source-backed, dispositive): during a JS runaway the KAPA has no advancing clock.**

- `DUK_USE_EXEC_TIMEOUT_CHECK(udata)` is defined (vendored `duk_config.h:2968`) and mapped to `sweep_js_engine_timeout_check` in `sweep/freestanding/sweep_freestanding_libc.c`, which today returns **0 always**.
- `DUK_USE_INTERRUPT_COUNTER` is defined; the interpreter decrements per opcode and calls `duk__executor_interrupt` at zero, which calls the timeout hook. The *mechanism* is present and correctly wired at the Duktape layer.
- But `g_kernel_uptime_ms` in `hal/htl.c` advances **only inside `OwHalDelayMs()`** — the busy-wait spin loop used for boot pacing. The PIT IRQ0 advances no uptime counter. During a JS runaway (no `OwHalDelayMs` call), the monotonic clock is frozen at whatever value it had at KAPA entry, `sweep_runtime_monotonic_ms` returns that constant forever, `sweep_runtime_deadline_reached(now, deadline)` can never flip for any future `deadline`. The interrupt hook therefore never fires — observed QEMU evidence: an infinite-loop probe script executed 100+ simulated seconds (a full QEMU deadline) without interruption.

**The R6/R7 design thus already contains the interrupt plumbing** (one `#undef DUK_USE_EXEC_TIMEOUT_CHECK` away from disabling it, or one function body away from enabling it), but enabling a timeout requires a clock that advances without the script yielding. The cheapest correct clock source is an IRQ0-driven increment of `g_kernel_uptime_ms` (or a TSC-deadline conversion) — HAL surgery that must land with the boot pacing de-duplicated from the IRQ path to keep phase timings honest.

**What works today:** host-side `sweep_tests` still has full deadline testing coverage (`Sweep runtime tests 60/0`, includes `sweep_runtime_deadline_reached`), which is the guard for the runtime-enforced budget on a normal host. The KAPA image has the same guarantee *if* the clock is live.

**Proposed R8 implementation (detailed in §20):** wire `OwPsSchedulerTick()` (the 100 Hz PIT IRQ0 handler) to advance `g_kernel_uptime_ms` by its period (deduplicating the `OwHalDelayMs` advance path), re-enable `DUK_USE_EXEC_TIMEOUT_CHECK` with a `SWEEP_FS_KAPA`-only branch calling `sweep_runtime_monotonic_ms`, arm a deadline per `duk_peval_string` call, and add a QEMU probe payload that must be interrupted deterministically. Effort: small; risk: scheduling pacing accounting; gated behind `OW_SWEEP_KERNEL` so full OpenWindows is untouched.

## 11. Arena/frame reclamation investigation (Workstream H)

**What R5/R6/R7 established works:**

- `OwPsTerminateProcess` (R6) recycles the process VAD tree back to the pool and releases the frame-run commit charge (frames zeroed). VADs no longer strand across KAPA cycles (`ps_free_vad_tree`, bounded by the AVL height).
- The KAPA image's BSS pages (arena + `SWEEP_DOM g_dom` — inline 762 KB) are eagerly mapped and charged, as are all image pages; the frame run is sized at **384 pages** for the sweep profile (`inc/ow_memory.h`, `OW_SWEEP_KERNEL`-gated, full profile unchanged at 64). QEMU evidence: `image loaded: 7 section(s), 349 page(s) charged`.

**What cannot be reclaimed in the current allocator (source-backed):**

| Resource | Status | Why |
|---|---|---|
| Physical frames (bump arena) | **Not reclaimable** | `g_PhysicalRam` is a 16 MiB static bump arena; `OwMemAllocatePages`/`OwMemAllocatePage` only advance `g_RamPointer`; there is no free list, no ownership record for individual frames, no way to return them to the arena. |
| Private page-table pages (PML4/PDPT/PD/PT, `OwMemAllocPageTable`) | **Not reclaimable** | Same allocator; nothing tracks them to free, and the kernel half (shared `pd0` at pdpt slots 1..3) **must not** be freed. Safe freeing would require a per-process census + a private-only chain walk (slot 0) with strict verification that no shared table is reached — never done, and never safe to half-do. |
| Process/thread table slots | **Never reused** | `OwPsCreateUserThreadFrame`/`OwPsExitThread` own slots permanently; reuse is not implemented and not claimed. |

**Whole-process reclamation design sketch (R8, deferred):**

1. Add a per-process page-table census: `OwMemFreeAddressSpace(Pml4)` walks pml4/pdpt slot 0's private pd→pt chain, returns the ~6-11 private table pages to a free table list. Shared pd0 is never entered.
2. Convert frame allocation for user addresses to a freelist allocator keyed on a free stack: if the frame is zero sentinel-tagged `OW_FRAME_FREE`, reuse it; else bump-allocate as today. Zeroing on free is already enforced by `OwMemRunReleaseAll`, so reuse is safe.
3. Reclaim the private pdpt[0]→pd→pt tree at terminate; assert no private table remains in the reuse census before returning the process slot.
4. Lift the hard bump-arena limit to "256 MB KAPA churn test" scale; until then the honest statement remains: **terminated KAPA instances release their commit charge and VAD pool slots; they do not return frames or page tables to the allocator**.
5. Do not recycle entries while any KDB instance binding is live (verified via `OwKdbExecPsGetBindingByThread`); the existing binding lookup provides the required membership test.

**Verdict for R7:** no safe implementation landable in this milestone; the 48-byte/VAD and frame-run improvements are the honest ceiling without a real allocator. Reclamation is specified as R8 work with the constraints above.

## 12. Build and regression verification (Workstream J — fresh, 2026-11-14, all commands run with this tree)

| Command | Result |
|---|---|
| `make -B -j 2` (full kernel) | **Success**, `openwinkrnl.owx` 1,662,976 B, sha256 `6fb140a25ef…` |
| `make sweep-kernel` | **Success**, `sweepwinkrnl.owx` 1,531,904 B, sha256 `7c699dddcb…` |
| `make sweep-tests` | **1007 passed, 0 failed** |
| `make hosttest` | **1424 passed, 0 failed** |
| `make sweep-freestanding` | **Exit 0**, symbol closure OK |
| `make check-external-deps` (full, sweep-kernel, sweep-tests, freestanding) | **All OK** (bogus profile fails as before) |
| `make emergency` | **PASS** (entry audit) |
| `make qemu-sweep-kernel` | **PASS** |
| `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | **PASS** |
| `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` | **PASS** (34 assertions) |
| `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` | **PASS** (23 assertions) |
| `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1` | **PASS** (58 assertions) |

## 13. Artifact hashes / sizes

| Artifact | Size | SHA-256 / check |
|---|---:|---|
| `openwinkrnl.owx` | 1,662,976 B | `6fb140a25ef8aa13ecc4ad4cdd61a30772d78534381b1a813c0b644eb92071f0` (matches `.chk`) |
| `sweepwinkrnl.owx` | 1,531,904 B | `7c699dddcb5de80fb9b4dd6d19eaff0b89c7815e0186e148ac512dec2a419a20` (matches `.chk`) |
| `build/sweep-kapa-runtime/kapa_sweep.owx` | 434,410 B | `1d99971b639205ce437532bf87193bfdc972570a244206a46e668ba76bd3aecb` (matches `.chk`); entry RVA `0x1360`; 0 imports |
| `build/sweep-kapa-test/kapa_fault.owx` | 8,426 B | `43fcc3a410dc2d1e85f443f6a020167601c4b0dad4df55eb680c4288f887089c` (matches `.chk`) |
| `build/sweep-kapa/kapa_test.owx` | 8,426 B | `cab69a8c3f4624962a2cb6280b241cb3a3f5deb0c890dd9147a847a9785efcee` (matches `.chk`) |
| `build-generated/sweep_kapa_dom_qemu_boot.log` | 5,012 B | fresh run |
| `build-generated/sweep_kapa_runtime_qemu_boot.log` | 4,595 B | fresh run |
| `build-generated/sweep_kapa_fault_qemu_boot.log` | 4,436 B | fresh run |
| `build-generated/sweep_kernel_qemu_boot.log` | 3,721 B | fresh run |
| `build-generated/sweep_kapa_qemu_boot.log` | 4,298 B | fresh run |

Image contents changed: the KAPA image now carries the HTML parser/DOM code (`sweep_html.c`/`sweep_text.c`) plus the R7 binding (`kapa_dom_js.c`) and fixture header; `.bss` grew by the `SWEEP_DOM g_dom` (~762 KB) → 349 pages charged (vs 152 in R6). The fault payload and R5 minimal probe are unchanged.

## 14. Modified files (R6 → R7)

Created: `sweep/kapa_sweep/kapa_dom_fixture.h`, `sweep/kapa_sweep/kapa_dom_js.c`, `sweep/kapa_sweep/kapa_dom_js.h`, `sweep/freestanding/ctype.h`, `sweep/freestanding/string.h`, `tools/qemu_sweep_kapa_dom_test.ps1`.  
Modified: `Makefile` (new `SWEEP_OTK_*` objects `-Isweep/freestanding` etc., new `qemu-sweep-kapa-dom-test` target), `core/main.c` (parameterized KAPA self-test boot block for 3 payloads, failure diagnostics), `inc/ow_memory.h` (`OW_FRAME_RUN_PAGES` sweep: 256 → 384; `OW_USER_STACK_PAGES` sweep: 2 → 16), `sweep/kapa_sweep/kapa_sweep.c` (Phase 2: DOM parse → JS → C-verify → teardown), `sweep/freestanding/sweep_freestanding_libc.{h,c}` (ASCII ctype subset; `sweep_js_engine_timeout_check` stays disarmed pending R8), `docs/*`, `SWEEP.md`.  
Not modified: full OpenWindows kernel semantics, CIS, `kdb/*`, `ps/*`, the DOM implementation itself, the tokenizer implementation.

## 15. Remaining R7 blockers / limitations

- Browser engine (CSS/layout/render/navigation) absent — by scope.
- No native OWFS-backed resource service (see §9); `read_resource` fails deterministically; no fixture files are needed for the R7 milestone.
- No JS execution budget enforcement inside a JS runaway on the KAPA (investigated; see §10).
- Whole-process memory reclamation not implemented (§11).
- `SWEEP_HTML_TEXT_MAX 1024` means textContent writes above 1023 bytes are refused, `innerHTML` unsupported; node/children/attr caps are the parser's (documented).
- The R7 QEMU test registers exactly two globals under object keys: `__kapa_proto` (hidden, non-enumerable/configurable/writable) and `document` (non-enumerable, non-configurable); plus hidden wrapper properties `__sweep_node`/`__sweep_gen` (non-enumerable/configurable/writable) on element wrappers. Documented in `kapa_dom_js.h`.

## 16. R7 blocker table revisited

| # | R6 status | R7 status |
|---|---|---|
| B1 Runnable instance address space / image mapping | Closed | Closed |
| B2 Entry ABI + stack + control transfer | Closed | Closed |
| B3 Scheduler binding / real RUNNING | Closed | Closed |
| B4 Instance-attributed execution faults | Implemented, not QEMU-exercised | **Closed** (`kapa_fault.owx` demonstrated; full chain asserted in QEMU) |
| B5 Return/exit path | Closed | Closed |
| B6 Service Bridge surface | Partial (minimal) | Partial — used as-is for R7 (`main.log` / `main.runtime_info`); no new services added (by design) |
| B7 Runnable freestanding Sweep app image | Partial | **Closed for the runtime foundation, not the engine** — `kapa_sweep.owx` executes JS→C DOM mutation; browser still absent |
| B8 Isolation/trust model | Closed (CIS-gated; no MMU/CPU isolation claimed) | Closed (same caveat, unchanged) |

## 17. What changed in the tree vs the R6 baseline

See §14. Also: `docs/SWEEP_HTML_DOM.md` (new), plus updates to `SWEEP.md`, `docs/SWEEP_R6_REPORT.md`, `docs/SWEEP_KAPA_EXECUTION.md`, `docs/SWEEP_KAPA_INTEGRATION.md`, `docs/SWEEP_SERVICE_BRIDGE.md`, `docs/SWEEP_BOOT_VALIDATION.md`, `docs/SWEEP_KERNEL_PROFILE.md`, `docs/SWEEP_RUNTIME_PORTABILITY.md`, `docs/SWEEP_JS.md`. No stray build artifacts: `r6work/`, `r7meas/`, `r6test2/`, `r6test/`, `build-generated/kapa_dom_check.owx` were deleted.

## 18. R8 recommendations

1. **JS watchdog** (§10): wire `OwPsSchedulerTick` to advance `g_kernel_uptime_ms` by its period; stop advancing it in `OwHalDelayMs` or dedupe; enable `DUK_USE_EXEC_TIMEOUT_CHECK` behind `SWEEP_FS_KAPA` with `sweep_runtime_monotonic_ms`; add a QEMU payload that infinite-loops and must exit `0xD4`.
2. **OWFS-backed read_resource** for `SWEEP_RUNTIME`: root-confined, MIME-allowlisted, path-traversal-proof (all enforced already by `sweep_resource.h`) — backed by a canned volume on the KAPA volume, or a native resource service with a tiny, audited size cap.
3. **Frame reclamation** (§11): per-process page-table census + private-chain free; frame freelist with sentinel-tag validation; process slot reuse policy.
4. **Second JS→DOM milestone**: `element.children` returning wrappers (already present) plus a real `<script src="fixture.js">` path once #2 lands; then CSS/layout as later phases.
5. **B8 upkeep**: before any non-test KAPA package ships, decide and document the MMU/CPU isolation story; keep CIS as the only trust source.
6. Keep `qemu-sweep-kapa-dom-test` (58 assertions) and the R6 harnesses in CI; refuse to weaken assertions.
7. **Docs**: keep `docs/SWEEP_R7_REPORT.md` + `docs/SWEEP_HTML_DOM.md` updated; add DOM-level host tests (sibling to `sweep_html_test.c`) for the KAPA binding.

**Final architectural principle respected:** the kernel provides the controlled execution environment; the KAPA owns application behavior; the HTML parser and DOM remain genuine Sweep application components. R7 proves **JavaScript executing inside a real KAPA can query and mutate a document constructed by Sweep's actual HTML parser and DOM, verified independently in QEMU** — not that Sweep is a browser.
