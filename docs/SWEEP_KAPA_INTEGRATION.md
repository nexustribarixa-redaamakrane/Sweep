# Sweep KAPA Integration Architecture (R1, R6 evidence update)

**Date:** 2026-11-14 (R7 integration)  
**Status:** Controlled KAPA execution **implemented and demonstrated at three levels** in the reduced sweep-kernel profile: (R5) the minimal `kapa_test.owx` self-test; (R6) the **actual freestanding Sweep runtime** (`kapa_sweep.owx`); (R7) the **actual Sweep HTML parser + DOM** executing inside that runtime at CPL3 with JavaScript mutating the real document (`docs/SWEEP_R7_REPORT.md`; `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1`, 58 assertions). The Sweep **browser** (CSS/layout/rendering) is **not** packaged or executed. R1 produced the KDB-ready reduced kernel substrate; R3 added the validated boot harness and Service Bridge MVP; R4 added the injectable Sweep runtime seam and freestanding Duktape object build; R5 closed the execution blockers (B1-B5, B8) for the minimal payload; R6 packaged the runtime as `kapa_sweep.owx` and demonstrated JavaScript execution at CPL3; **R7 compiled the HTML parser and DOM into `kapa_sweep.owx` and drove them from JavaScript at CPL3**. See `docs/SWEEP_KAPA_EXECUTION.md`, `docs/SWEEP_SERVICE_BRIDGE.md`, `docs/SWEEP_BOOT_VALIDATION.md`, `docs/SWEEP_RUNTIME_PORTABILITY.md`, `docs/SWEEP_R5_REPORT.md`, `docs/SWEEP_R6_REPORT.md`, `docs/SWEEP_R7_REPORT.md`.

## 1. KDB/KAPA status today (source-verified)

From `inc/ow_kdb.h`, `kdb/*`, `docs/KDB-KAPA.md`, `core/main.c`:

| Capability | Status | Evidence/notes |
|---|---|---|
| Descriptor registration | Implemented (management) | Registry APIs, `OwKdbRegisterKernel`, Sweep declarative registration via `OwKdbRegisterSweep` during `OwKdbInitialize()` |
| Application discovery/lookup | Implemented | Find/get/list registry and association queries |
| Compatibility validation/policy | Implemented | `OwKdbAssessCompatibility` verdicts (arch/ABI/format/memory/privilege/kind/descriptor) |
| Image verification (OWX front-end) | Implemented (validation/accounting) | Loader validates OWX structure/CRC32c-style checks and resident-page accounting (per docs/loader APIs); not equivalent to full executed runtime |
| Application admission/association | Implemented | `OwKdbAssociateApplication`, capability bits, association state |
| Instance creation/lifecycle (logical) | Implemented (management) | Load/start/stop/fault/destroy state machine, page budget, logical isolation domain, ownership cleanup |
| Resource accounting | Implemented (logical) | Page limits/charges, stats |
| Service Bridge calls | Implemented (bounded, in-kernel) | Explicit service IDs, KDB-owned request/response copies, capability/ownership/length checks, 4 built-in services (`main.ping`, `main.echo`, `main.log`, `main.runtime_info`) with per-call attribution; hosttest-verified |
| Actual executable image loading into runnable context | **Implemented (R5)** | PS process + per-process PML4/frame-run/VADs; CIS-gated loader; CPL3 entry; KDB binding; demonstrated in QEMU (minimal payload) |
| Actual second-kernel / app execution and runtime lifecycle | **Runtime execution demonstrated (R6)** | Dispatch-attributed `RUNNING`, `main.log` bridge calls from CPL3, real JS execution with verified results, attributed exit, `STOPPED` transition observed (`kapa_sweep.owx`). **No second kernel, no browsing content and no browser engine executed.** |

**Isolation:** logical/resource-ownership only (domain IDs, page budgets, service namespaces), **not** hardware/MMU/CPU isolation.

## 2. Target execution architecture for Sweep as KAPA application

### 2.1 Principles
- Sweep browser engine (HTML/CSS/JS/DOM/layout/paint, resource policy) remains **application/runtime** code; it must not be moved into kernel merely for convenience.
- Kernel/KDB provides controlled, explicit services (Service Bridge / KAPA contracts), bounded inputs/outputs, capability checks, and resource ownership.
- No unrestricted kernel/memory/device/framebuffer access to scripts or web content.

### 2.2 Required contracts (concrete, to be implemented/validated)

| Area | Required definition | Current state / gap |
|---|---|---|
| Application image format | OWX/KAPA-compatible image with manifest/entry metadata accepted by KDB loader/validator | OWX validation front-end exists and is exercised (KAPA self-test loads an OWX console-subsystem image) |
| Loader responsibilities | Validate, map/account pages, set up entry/address space/context per KAPA model, enforce policy/budgets | **Implemented (R5)**: per-instance process/PML4/frames/VADs, image mapped via CIS-gated loader, stack + exit trampoline, KDB binding |
| Entry-point contract | Defined ABI (registers/params, environment/services handle, memory layout, return/fault semantics) | **Defined and working (R5)**: Microsoft x64 `.owx` contract (RCX=argc, RDX=argv, R8=envp), exit via `OW_SYS_PS_EXIT_THREAD`, faults attributed to the instance on CPL3 |
| Memory ownership | Caller/kernel/instance ownership, page budgets, no leaks/dangling across faults/destroy | **Implemented (R5)**: frames released via frame run on exit/fault; VAD tree recycled; page budget enforced by admission; bump-arena physical pages not recycled (documented) |
| Application lifecycle | Admitted → loaded/ready → running → stop/fault → cleanup; observable via KDB | **Working (R5)**: `READY -> RUNNING -> STOPPED` observed in QEMU; `FAULTED` path implemented + host-tested (not yet QEMU-exercised) |
| Fault handling | Trap/fault isolation to instance, deterministic cleanup, reporting; no kernel compromise | **Implemented (R5)**: CPL3 faults classified in `core/pgfault.c` and attributed to the instance; kernel-CPL faults are not laundered into KAPA faults |
| Scheduling | How/where Sweep instance executes (thread/context integration), preemption/timers, deadlines | **Implemented (R5)**: bound thread resumes via the normal PS scheduler; `RUNNING` is attributed at dispatch; exit returns control |
| Service Bridge interface | Service IDs, schemas, capability gates, bounds, sync/async behavior, timeouts | Bridge mechanism exists; Sweep-specific services **mostly undefined/unimplemented** |
| Capability/resource boundaries | Least privilege, no raw device/memory/kernel handles to untrusted content | Model defined in principle; enforcement for real Sweep runtime **gap** |

### 2.3 Proposed smallest compatible extensions (design-level)
- Define a **Sweep KAPA runtime ABI** aligned with existing OWX/KDB conventions (entry, service-call trampoline/handle, memory grants, fault exit).
- Extend KDB loader/exec path from validated/admitted to **runnable context creation** only with proven-safe primitives (separate from management APIs).
- Specify minimal **Sweep service set** over existing Service Bridge first (memory/timers/logging/resource fetch mediation hooks, presentation handoff stubs) before adding complex services.
- Keep full repaint/reference paths and host simulation as correctness baselines until real presentation/service paths exist.

## 3. Sweep runtime dependency classification (current)

| Dependency/component | Classification | Notes |
|---|---|---|
| HTML/DOM/CSS/layout/paint command generation | Sweep browser-engine component (app/runtime) | Implemented host-side in `sweep/` |
| JavaScript (Duktape integration) | Sweep browser-engine component + external runtime dependency | Hosted for tests; freestanding object build with zero hosted symbols exists (`sweep/freestanding/`), but is not kernel/KAPA-executed |
| Document lifecycle/invalidation | Sweep browser-engine component | Host-tested |
| Software renderer + SUF glyph provider (host) | Sweep browser-engine/trusted runtime component | Host-tested; uses SUF parser/assets |
| SuperUnicode transport/SUCS/SUF parsing usage | Browser-engine/runtime + kernel/service primitives (as needed) | Keep kernel primitives minimal/explicit |
| Resource provider (local, root-confined) | Trusted runtime/service boundary | No arbitrary FS/network from scripts; kernel mediation if integrated |
| Memory allocation/time/deadlines | KAPA application/runtime facility backed by kernel primitives/services | Implemented as the injected `SWEEP_RUNTIME` seam on host + freestanding arena; kernel-backed runtime ops not implemented |
| Input/event dispatch | Kernel primitive/service + runtime delivery | Not integrated for native KAPA Sweep |
| Presentation to display | Kernel/service/device abstraction (trusted) | **No real general-purpose OW presentation API identified**; simulation only |
| Networking | Optional kernel/service or runtime service | Not implemented for Sweep integration |

## 4. Minimum KDB Service Bridge services (future, to be specified)

| Service | Purpose | Exists today? | Key requirements |
|---|---|---|---|
| Memory/resource mgmt | Bounded alloc/grant/return within budget | Partial primitives elsewhere; KAPA-specific contract **no** | Ownership, bounds, no leaks, budget enforcement |
| Timers/scheduling hooks | Deadlines, ticks, wakeups | `main.runtime_info` exposes monotonic uptime (8-byte ms); no timer/deadline/wakeup service | Deterministic, bounded, capability-gated |
| Input/event delivery | Keyboard/pointer/system events to runtime | **No** (for Sweep) | Ordered, bounded queues, no raw device exposure |
| Storage/resource access | Controlled fetch of allowed assets/resources | **No** (KAPA Sweep service) | Root confinement, MIME/size limits, no script-chosen roots/devices |
| SuperUnicode text/font facilities | SUF/font/text services as needed | Primitives/libs exist in tree; service contract **no** | Immutable/validated buffers, bounds, no script font/device access |
| Rendering surface management | Create/map/submit bounded pixel surfaces | **No** (real) | Caller-owned/bounded formats, lifetime, no raw framebuffer export |
| Presentation handoff | Present composed frame to trusted target | Simulation boundary only | Real target API required; fail-safe |
| Networking (optional) | Policy-controlled network access | **No** | Explicit policy/capabilities, isolation from kernel |

## 5. Demonstration criteria (met for the runtime foundation and the parser/DOM path; not for CSS/rendering)

“Sweep runs natively on the reduced kernel” requires, at minimum: reduced kernel boots → KDB/KAPA ready → real Sweep KAPA image admitted/loaded/started → observable lifecycle and service-bridge interactions → (where applicable) controlled rendering/presentation path — all evidenced by tests/logs.

**R6/R7 where this stands:** the reduced kernel boots → KDB/KAPA ready → the **actual freestanding Sweep runtime** executes real JavaScript at CPL3 (R6) **and the actual Sweep HTML parser builds a real bounded DOM which JavaScript then queries and mutates in-place at CPL3, with the mutation independently verified via the C DOM API (R7)** — evidenced by `build-generated/sweep_kapa_runtime_qemu_boot.log`, `build-generated/sweep_kapa_dom_qemu_boot.log`, `build-generated/sweep_kapa_fault_qemu_boot.log` and asserted by `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1`, `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1`, `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1`. The Sweep **browser engine** (CSS, layout, rendering, navigation) is still **not** packaged or executed. Remaining gaps: packaging the full engine as an OWX image, a native resource backend rooted on OWFS (or a policy-confined canned builtin — see report §9), execution-budget enforcement for runaway scripts (report §10), frame reclamation (report §11) — not the execution mechanism, which is proven end to end at three levels (minimal probe, runtime, parser/DOM).
