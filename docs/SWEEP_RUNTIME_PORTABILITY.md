# Sweep Runtime Portability Audit (R2, R4 update)

**Date:** 2026-11-11 (R2 audit; R4 implementation/evidence update)  
**Scope:** Source/documentation audit of `sweep/` browser/runtime components and their readiness for freestanding/native KAPA integration.

**R5 status (2026-11-12):** the controlled KAPA execution mechanism is implemented and boot-demonstrated (`docs/SWEEP_R5_REPORT.md`): a minimal `kapa_test.owx` self-test runs at CPL3 end to end.

**R7 status (2026-11-14):** the freestanding bundle the KAPA image links — and the runtime itself — now executes the HTML parser and DOM with real JavaScript ARE QEMU-demonstrated at CPL3: `kapa_sweep.owx` parses a canned fixture, builds a real bounded DOM, and JS queries/mutates the actual document; verified independently in C through the DOM C API. See `docs/SWEEP_R7_REPORT.md` and `build-generated/sweep_kapa_dom_qemu_boot.log`. The startup ABI, service backend and CIS gate are unchanged. The browser (CSS/layout/render) remains unpackaged.

## 1. Component inventory and dependency classification

Based on repository structure, implementation (`sweep/*.c,h`), tests, `sweep/third_party/duktape`, and component docs:

| Component | Files (representative) | Classification | Notes |
|---|---|---|---|
| Engine/URL/viewport/paint encoding | `sweep_engine.*` | **Freestanding-compatible (bounded C logic)** in core algorithms; built/tested in the hosted test environment | No OS services required by logic; platform wiring still needed for native use. |
| Service handlers (host contract) | `sweep_services.*`, `sweep_kdb_register.c` | **Freestanding-compatible logic / kernel-service-dependent registration** | Bounded byte-buffer handlers; `SweepRegisterKdbServices` links against KDB APIs. |
| Text primitives | `sweep_text.*` | **Freestanding-compatible** (bounded) | Pure processing over provided buffers. |
| Browser core/session glue | `sweep_browser.*` | **Mostly freestanding-compatible**, with dependencies on provided resource/time/runtime abstractions | Host-tested; no direct OS I/O in core logic. |
| HTML parser/DOM foundations | `sweep_html.*` | **Freestanding-compatible** (bounded parsing/structures) | Needs allocator/runtime hooks for native use. |
| CSS tokenizer/parser/cascade/layout subset | `sweep_css.*` | **Freestanding-compatible** (bounded) | Compute-only over inputs. |
| Document lifecycle/pipeline | `sweep_document.*` | **Freestanding-compatible core; runtime-service-dependent integration** | Depends on injected providers (resource/font/timing hooks). |
| Resource provider (local/root-confined) | `sweep_resource.*` | **Now runtime-mediated**: no stdio in the policy path; the hosted file backend is a replaceable `SWEEP_RUNTIME` implementation (`sweep_runtime_host.c`) | Native/KAPA mode must supply a root-confined runtime backend; no arbitrary FS. |
| Software renderer + SUF glyph provider | `sweep_render.*` | **Freestanding-compatible rendering logic** given buffers/fonts; external SUF parser/font data dependency | Host tests compile SUF parser from `$(SUF_DIR)/src/suf_parser.c`. Presentation target remains simulated/host-side. |
| JavaScript runtime integration | `sweep_js*.{h,c}` + `sweep/third_party/duktape/*` | **Hosted build for tests; freestanding object build exists** | Duktape 2.7.0 builds with `-ffreestanding -nostdlib` via `sweep/freestanding/` over `SWEEP_RUNTIME`; not linked into the kernel image. |
| Tests/harness | `sweep/tests/*`, `sweep/freestanding/*_host_main.c` | **Hosted (toolchain/libc/test executables)** | Not runtime components for a KAPA image. |

## 2. Injected runtime seam (implemented R4)

**Files:** `sweep/sweep_runtime.h`, `sweep/sweep_runtime.c`, `sweep/sweep_runtime_host.c`.

- Engine code talks to an explicit `SWEEP_RUNTIME` instead of calling `malloc`/`realloc`/`free`, a clock, a logger, or a file reader directly.
- **Mandatory ops:** `alloc`, `free`, `monotonic_ms`. **Optional ops:** `realloc`, `diag`, `read_resource`, `init`, `shutdown`; a missing optional op fails deterministically (NULL/false/0), never falls back to libc.
- Documented semantics: `realloc(NULL, n)` = `alloc(n)`; `realloc(p, 0)` frees and returns NULL; a runtime without `realloc` fails **without freeing the caller's block**; `read_resource` writes at most `out_cap-1` bytes plus NUL and rejects `out_cap < 2` or a backend that reports `produced >= out_cap`.
- `sweep_resource.c` now performs all byte reads through `sweep_runtime_read_resource`; the legacy convenience API uses `sweep_runtime_host_default()`, and the explicit API is `sweep_resource_fetch_runtime`.
- Hosted behaviour is preserved by `sweep_runtime_host_ops()`; a kernel/KAPA host would supply its own ops (not implemented).

**Evidence:** `sweep/tests/sweep_runtime_test.c` (60 passed, 0 failed) covers binding validation, missing mandatory/optional ops, alloc/realloc/free routing and edge cases, bounded reads/overflow/backend contract violations, deadlines, host backend file reads, and resource fetch through an injected runtime.

## 3. Freestanding Duktape (implemented at object level, R4)

**Location:** `sweep/freestanding/` (vendored `sweep/third_party/duktape` is unmodified).

- **Config override:** `duk_config_freestanding.h` is force-included before `duktape.h`/`duktape.c`, reuses the vendored generated config, then drops Windows date/time providers and `DUK_USE_PROVIDE_DEFAULT_ALLOC_FUNCTIONS`, and installs Sweep hooks (date, fatal, `__builtin_setjmp/longjmp`).
- **libc substitute:** `sweep_freestanding_libc.c` defines memory/string primitives, math routines, a bounded printf/scanf subset, and the Duktape hooks. The math and printf implementations are **minimal approximations**, not a correctly-rounded libm or full CRT.
- **Allocator:** `sweep_fs_arena.c` is a bounded first-fit arena implementing the runtime alloc/realloc/free contract; Duktape is created with `duk_create_heap` over `SWEEP_RUNTIME`, never `duk_create_heap_default`.
- **Verification:** `make sweep-freestanding` compiles the object set with `-ffreestanding -nostdlib -fno-builtin`, runs a smoke script (arrays, objects, JSON, regexp, exceptions, number formatting, `Date.now`), reports arena peak/outstanding bytes, then relocates the objects with `gcc -r -nostdlib` and requires `nm -u` to be empty.

**Result (R4):** exit 0; smoke `rc=0 status=ok`; arena peak 118,480 B; 0 bytes outstanding; **symbol closure OK**. Log: `build-generated/freestanding_build.log`.

**Caveats (must not be overstated):**
- This is an **object-level build and host-harness smoke test**. Duktape is not linked into `sweepwinkrnl`, not executed in QEMU, and not executed through KDB/KAPA.
- `sweep_fs_date_now_ms()` returns 0 (deterministic, no wall clock); the fatal handler traps; there is no file/stdio provider.
- Math/printf shims are approximations; production use would need vetted implementations or a tested libm/CRT subset.

## 4. Minimum runtime abstractions (status)

| Abstraction | Purpose | Status |
|---|---|---|
| Allocator/deallocator (+ realloc semantics) | Bounded memory for parser/DOM/CSS/JS/renderer structures | **Implemented** as `SWEEP_RUNTIME` + freestanding arena |
| Monotonic clock / deadline checks | Timeouts, scheduling hooks, JS execution budgets | **Implemented** in the seam (`monotonic_ms`, `sweep_runtime_deadline_reached`); kernel-backed source not wired |
| Resource provider interface | Controlled, mediated fetch of HTML/CSS/JS/assets/fonts | **Implemented** over the runtime seam; a root-confined native backend is **not** implemented |
| Logging/diagnostics sink | Bounded runtime diagnostics via Service Bridge | **Partial**: `main.log` exists in KDB; the Sweep runtime `diag` op is host-only |
| Input event provider | Deterministic event delivery to document/JS/engine | **Planned** |
| Surface/presentation provider | Bounded pixel buffer submission/present handoff | **Planned/blocked by platform API** |
| Font/text provider | SUF/SuperUnicode text shaping/render inputs via controlled interfaces | **Partially available as components; service integration planned** |

## 5. Separation and security boundaries

- HTML/CSS/JS/DOM/layout/paint logic must remain in **application/runtime space** (Sweep app/runtime), not moved into the kernel.
- Kernel provides only **controlled, explicit, bounded** services; no direct device/framebuffer/kernel-memory/FS access from untrusted content or scripts.
- SuperUnicode native invariants (SUCS PUA/surrogate handling boundaries) must be preserved across runtime/service interfaces.
- JS/web content cannot select the runtime, the root, or absolute paths; resource policy remains root-confined and extension-bounded.

## 6. Readiness summary

| Area | Readiness |
|---|---|
| Pure browser algorithms (parse/layout/paint/text/doc logic) | **High (host-tested)**; porting depends on remaining abstractions |
| Runtime seam (alloc/time/diag/resource) | **Implemented and tested (host)**; R6 wires alloc/time/diag to a KAPA backend (arena + gateway) |
| JS engine (Duktape) freestanding | **Object build + smoke + symbol closure OK; R6-executed at CPL3 (`kapa_sweep.owx`); R7 parser/DOM executed at CPL3** — HTML parse → bounded DOM → Duktape mutation → C-side verification; still not the browser |
| Resource/I/O mediation for native | **Partial**: runtime interface exists; kernel-backed backend (`read_resource`) missing |
| Native KAPA execution/integration | **Demonstrated (R5 mechanism, R6 runtime)**: `kapa_sweep.owx` executes JS at CPL3 |
| Physical presentation | **Not ready (no platform API)** |

**Conclusion:** Sweep is architecturally separable, host-validated, freestanding, and now proven executable by the reduced kernel as a genuine KAPA application with HTML/DOM (R7). It is **not yet the browser** — `kapa_sweep.owx` runs the runtime foundation plus the bare parser/DOM with one canned fixture — not HTML fetching, CSS. What remains for a natively executable browser is engine packaging (B7) plus a native resource backend, not the execution mechanism — which is proven end to end at three levels (minimal probe, runtime, parser/DOM).
