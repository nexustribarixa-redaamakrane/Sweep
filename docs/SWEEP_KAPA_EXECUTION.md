# Sweep KAPA Execution Architecture (R2, R3/R4 evidence update, R5 implementation)

**Date:** 2026-11-12 (R2 analysis; R3/R4 evidence refresh; R5 implementation)  
**Scope:** R2 audit/specification **plus the R5 implementation**: a controlled KAPA execution path is implemented in the reduced sweep-kernel profile and boot-demonstrated in QEMU (`docs/SWEEP_R5_REPORT.md`). The no-fake-execution and no-claimed-isolation invariants are preserved.

## 1. Baseline (current, re-verified for R4)

- R1/R5 artifacts present: `sweepwinkrnl.owx`, `sweepwinkrnl.pe`, `build/sweep-kernel/` (106 `.o`); full `openwinkrnl.*` preserved. Exact sizes/timestamps/hashes are recorded in `docs/SWEEP_R5_REPORT.md`.
- `make sweep-kernel` **success**; `make qemu-sweep-kernel` **PASS** against the real multiboot image (`build-generated/sweep_kernel_qemu_boot.log`) and `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` **PASS** against the KAPA self-test image (`build-generated/sweep_kapa_qemu_boot.log`).
- `make sweep-tests` **1007 passed, 0 failed** (engine 24, services 18, text 37, browser 39, HTML 40, CSS 208, JS 187, document 258, resource 15, runtime 60, render 121).
- `make hosttest` **1424 passed, 0 failed** (1392 baseline plus 32 executor-seam assertions; Service Bridge built-ins `main.ping`, `main.echo`, `main.log`, `main.runtime_info`).
- `make sweep-freestanding` **exit 0**: freestanding Duktape object + libc/math/printf shims + arena + runtime seam link with **zero undefined hosted symbols** (`nm -u` empty) and pass a smoke test in a host harness. Not linked into the kernel or executed via KAPA.
- OpenWindows remains upstream/reference, read-only; no source deletion; reduced artifacts use distinct names.

## 2. KDB/KAPA execution architecture audit (source-level)

Audited: `inc/ow_kdb.h`, `kdb/kdb_internal.h`, `kdb/kdb_core.c`, `kdb/kdb_registry.c`, `kdb/kdb_loader.c`, `kdb/kdb_assoc.c`, `kdb/kdb_manager.c`, `kdb/kdb_bridge.c`, `kdb/kdb_exec.c`, `kdb/kdb_sweep.c`, `core/main.c`, `ps/owx_loader.c`, `inc/ow_ps.h`, `docs/KDB-KAPA.md`.

| Capability | Classification | Evidence |
|---|---|---|
| KAPA descriptor registration / unregistration | **Implemented and operational (management)** | `OwKdbRegisterKernel`/`OwKdbRegistryStore` (`kdb_registry.c`), uniqueness/name/image metadata checks, slot-based static registry. |
| Descriptor lookup/discovery/listing | **Implemented and operational** | `OwKdbFindKernel`, `OwKdbGetKernel`, `OwKdbGetKernelByIndex`, `OwKdbKernelCount`. |
| Compatibility assessment | **Implemented and operational (policy)** | `OwKdbAssessCompatibility`: validates descriptor, x86-64 arch, kind/ABI/format combinations (OpenWindows kinds require OWX; Linux/custom require ELF/RAW but are assessed unsupported for loader adapter in build), enforces private memory and unprivileged privilege model, returns explicit verdict/reason/status. |
| Image format validation (OWX) | **Implemented and operational (validation front-end)** | `kdb_loader.c: prepare_owx` requires supported OpenWindows kind, OWX format, non-null/non-zero image unless declarative (then `OW_ERR_UNSUPPORTED`), `ImageSize >= OWX_HEADER_SIZE`, `OwOwxImageIsLoadable` (header/format/arch/image_size/sections/target checks + self-contained/no imports/section count/entry point), `OwOwxImageIsUsable` (whole-image CRC32c checks as implemented in `ps/owx_loader.c`), non-zero header entry point. |
| OWX loading into runnable execution context | **Missing (for KDB instances)** | KDB loader computes resident page count (`ceil(ImageSize/OW_PAGE_SIZE)`), returns header `entry_point`/`preferred_base`; it **does not** allocate/map pages, relocate/map sections, create address space, publish VADs, charge process frame run, or install an executable image for an instance (`kdb_loader.c`, `kdb_manager.c`). |
| Image memory accounting | **Implemented (logical/static accounting)** | `OwKdbLoadInstance` charges `ResidentPages` against `PageLimit` (default 1024 pages) and records `ResidentPages/PageLimit/EntryPoint/MappedBase`; no physical page mapping owned by KDB instance object. |
| Application admission | **Implemented (management)** | Association creation requires valid app name, known/capability bits, existing kernel, compatible descriptor (`kdb_assoc.c`). |
| Capability assignment | **Implemented (management)** | `OW_KDB_CAP_CALL/REGISTER_SERVICE/ADMIN` stored on association; bridge enforces required capabilities and ownership/admin rules. |
| Application association | **Implemented and operational** | Create/get/find/disassociate with one-instance-per-association invariant and kernel-in-use protection. |
| Instance creation (`OwKdbLoadInstance`) | **Implemented (management/validated admission record)** | Validates association/kernel/compatibility/loader prep, checks budget, allocates slot, assigns stable isolation domain (`OwKdbManagerNextDomain`), sets state `READY`, links association↔instance. |
| Instance lifecycle transitions | **Implemented (logical state machine)** | `READY/STOPPED -> RUNNING` via start (idempotent/error cases handled), running/ready -> stopped, fault releases page charge/clears association link/drops owned services/marks `FAULTED`, destroy cleans up slot (`kdb_manager.c`). **Not actual CPU execution.** |
| Scheduling/process/thread integration (for KAPA instance) | **Missing** | No KDB code creates PS process/thread, no context switch/quantum wiring, no scheduler registration for instance execution. Manager/exec only manage metadata/state. |
| Memory ownership (concrete runtime) | **Partially implemented (logical only)** | Page budget/resident counts and release on fault/destroy tracked; no instance-owned mapped memory regions, stacks, heaps, or address-space objects managed by KDB. |
| Entry-point handling | **Partially implemented (metadata only)** | Entry/base extracted from OWX header and stored in instance/launch plan; never used to transfer control. |
| Fault handling (execution faults) | **Missing for runtime execution; implemented for management faults** | `OwKdbFaultInstance` performs deterministic logical cleanup (resources released, services dropped, association unlinked). No trap/exception capture from executing KAPA code, no fault-to-instance attribution from CPU exceptions. |
| Exit and cleanup | **Partially implemented (management cleanup)** | Destroy/fault cleanup paths exist; no application exit ABI/return path from executing code. |
| Service Bridge request processing | **Implemented and operational (bounded in-kernel)** | `OwKdbBridgeCall`: validates caller association/capabilities/target state, copies request into KDB-owned buffer, dispatches to handler with KDB-owned response buffer and capacity limits, checks produced length, copies response back only on success, tracks call counts; built-ins `main.ping`/`main.echo`. |

### 2.1 Execution-context investigation

| Question | Finding | Evidence |
|---|---|---|
| Valid application execution context? | **No (for KDB/KAPA instance).** | No creation of runnable context in `kdb_exec.c`/`kdb_manager.c`; `OwKdbExecuteInstance` only validates/builds plan and calls optional backend. PS can create user contexts for normal processes (`OwPsCreateProcess`, `OwPsLoadImage`, `OwPsLaunchUserImage*`, per-process `Pml4Phys`/`FrameRun`/VADs in `inc/ow_ps.h`, `ps/owx_loader.c`, `ps/ps.c`), but KDB does not integrate them. |
| Well-defined entry point? | **Metadata only.** | `kdb_loader.c` extracts header `entry_point`; `kdb_manager.c` stores it; `kdb_exec.c` validates non-zero in plan. No control transfer. |
| Application-owned memory region? | **No (runtime-mapped).** Logical budget only. | KDB tracks `ResidentPages/PageLimit`; `ps/owx_loader.c` shows real mapping path exists for PS processes (frame charging, section mapping, NX policy, VAD publication, trampoline) but is not invoked by KDB. |
| Valid stack and calling convention? | **No (for KDB instance).** | No stack allocation/mapping for KAPA instances in KDB. PS launcher builds user stack/argv and sets entry registers/return trampoline for user processes (`OwPsLaunchUserImageEx`, `OwPsBuildArgv`, `OwPsCreateUserThreadFrame`), but not wired to KDB. |
| Controlled transition into application execution? | **No.** | `kdb_exec.c`: if no backend registered returns `OW_ERR_UNSUPPORTED` with state unchanged; if backend registered, calls backend and on success/failure does not create CPU execution itself. `OwKdbExecutionAvailable()` false by default. |
| Return/exit mechanism? | **No (KAPA ABI).** | PS user images get exit trampoline (`PS_EXIT_THREAD` syscall path) in `ps/owx_loader.c`; KDB instances have no such runtime binding. |
| Fault-reporting mechanism (execution)? | **No.** | CPU/page/interrupt fault attribution to KDB instance execution context absent. Logical `OwKdbFaultInstance` exists. |
| Cleanup mechanism? | **Yes for management resources/state.** | `OwKdbFaultInstance`/`OwKdbDestroyInstance` release logical charge, drop services, unlink association, clear slot. No mapped runtime resources to unmap because none created by KDB. |
| Scheduling relationship with existing executive? | **No.** | No thread creation/enqueueing for KDB instance; PS scheduler operates on PS threads (`OwPs*`). |

## 3. Execution feasibility decision

**Decision: Path B — Existing primitives support only part of the required context; specific, non-trivial extensions are required for safe genuine KAPA execution.**

**Source-level rationale:**
- **Present reusable primitives:** OWX structural/content validation and metadata extraction (`ps/owx_loader.c`, `kdb_loader.c`), CIS verification gate in PS loader path, PS process/address-space/VAD/frame-run/thread/context/stack/argv/entry-trampoline mechanisms, scheduler/timer/interrupt infrastructure, KDB compatibility/registry/association/lifecycle/bridge/accounting and executor seam.
- **Missing integration/semantics:** KDB does not map or own executable image memory for instances; no KDB-to-PS (or equivalent) runnable context creation; no KAPA-specific entry ABI/parameters/service handle/environment contract; no instance-bound fault capture/attribution; no defined return/exit path into KDB lifecycle; `RUNNING` in KDB is a logical eligibility state (bridge target must be RUNNING) not evidence of CPU execution (`kdb_manager.c`, `kdb_bridge.c`); executor backend slot exists but is intentionally unpopulated by default and does not itself imply MMU/CPU execution (`kdb_exec.c`, `ow_kdb.h`).
- **Safety/architecture constraints:** compatibility policy requires private memory and unprivileged privilege model for associated kernels (`kdb_registry.c`), but no hardware/MMU/CPU isolation is implemented for foreign execution; syscall/userspace services are excluded from reduced sweep profile boot; integrating execution must not expose unrestricted kernel/device/memory access or conflate logical and hardware isolation.

**Exact blockers (R2):**
1. No defined and implemented KAPA application entry ABI (parameters, service/runtime handle, stack/register contract, exit/fault signaling) compatible with KDB and target toolchain.
2. No KDB-owned/runtime-owned image mapping + execution-context creation path (address space/thread/stack/trampoline or equivalent) bound to instance lifecycle and budgets.
3. No verified executor backend that safely transitions a validated plan into controlled execution and reports completion/faults back to KDB without faking state.
4. No instance-attributed fault/exit handling and resource teardown for executing code.
5. Reduced sweep-kernel profile omits syscall/VFS/storage/network/usermode/shell services; required runtime service surface for Sweep (resource mediation, input, timers as service, presentation) is unspecified/unimplemented.
6. No runnable, freestanding-compatible Sweep KAPA binary/image and no demonstrated end-to-end load/start/execute/bridge interaction.

## 4. Sweep KAPA executable contract (specification, R2)

This contract is **specified, not implemented/tested end-to-end**. It should reuse existing OWX/KDB/PS concepts where compatible.

### 4.1 Image identity and metadata (proposed, OWX/KDB-aligned)
- **Identity:** application name (bounded, NUL-terminated, matches association/app identity), associated kernel name/descriptor ID, target arch `x86-64`.
- **Format:** OWX1 (`OW_KDB_FORMAT_OWX`) for OpenWindows-derived kernels/apps in this architecture; existing header fields (`format_version`, `header_size`, `image_size`, `section_count`, `entry_point`, `preferred_base`, `subsystem`, CRCs) remain authoritative for format/integrity.
- **Versioning:** image format version from OWX header; KAPA ABI version and app version to be carried via an explicit manifest/descriptor extension (preferred: bounded KDB/Sweep descriptor metadata or a dedicated, validated manifest section/blob referenced by policy) — **not yet implemented**.
- **Capabilities:** required/optional capabilities declared explicitly and admitted only if granted by association/policy (`OW_KDB_CAP_*` baseline; Sweep-specific service capabilities to be defined).
- **Memory requirements:** minimum/maximum resident pages, stack pages, heap/workspace budget, all bounded and admission-checked against instance/page limits.
- **Integrity:** existing OWX CRC/loadability/usability checks; CIS/provenance policy as applicable to trusted admission path (to be defined per execution model).

### 4.2 Entry ABI (proposed)
- **Calling convention:** explicit and toolchain-compatible (recommended: documented x64 ABI consistent with existing OWX user images/PS launcher; must state register/stack alignment and callee-saved expectations). **To be finalized against actual toolchain and runtime.**
- **Entry signature (conceptual):** `entry(context_ptr)` or equivalent fixed-parameter contract providing a read-only/bounded KAPA runtime context (instance/app IDs, ABI version, service-call entry/trampoline, memory/budget descriptors, monotonic tick/deadline query hook as available). No ambient kernel pointers.
- **Initial state:** valid stack within granted budget, defined register set, no privileged instructions/devices, no direct framebuffer/kernel memory access.
- **Return/exit:** normal return or explicit `kapa.exit(code)` service signals controlled termination; KDB transitions lifecycle to `EXITED` (proposed) and performs cleanup.
- **Faults:** synchronous/asynchronous faults attributed to instance where safely possible; KDB marks `FAULTED` with status and releases resources (no silent continuation).

### 4.3 Memory model (proposed)
- **Ownership:** image pages/runtime mappings owned by instance context; kernel/KDB owns control structures and enforces budgets.
- **Stack/heap:** predeclared or service-mediated allocations within budget; allocation failures return explicit errors (no overcommit/UB).
- **Alignment/lifetime:** page-aligned mappings; all mappings/capabilities revoked on stop/exit/fault/destroy.
- **Bounds:** all pointers/lengths/offsets from app validated; no unrestricted kernel memory exposure.

### 4.4 Lifecycle contract (proposed, distinguished from logical states)

| State | Meaning | Entry condition |
|---|---|---|
| DISCOVERED | Descriptor known | registered/found |
| VALIDATED | Compatible and image/metadata validated | policy + loader/manifest checks pass |
| ADMITTED | Association/capabilities accepted | association created/authorized |
| CREATED/LOADED | Resources accounted/mapped for runtime context | successful context/materialization (not current KDB load-only semantics) |
| READY | Runnable but not executing | context prepared, not scheduled/entered |
| RUNNING | **Genuinely executing** (CPU control transferred) | verified entry/dispatch into app context only |
| STOP_REQUESTED | Cooperative/controlled stop pending | stop requested |
| STOPPED | Not executing, resources retained as policy allows | controlled stop/quiesce |
| EXITED | Normal/explicit termination | app exit/return handled |
| FAULTED | Abnormal termination | attributed fault/validation/runtime failure |
| DESTROYED | All instance resources released | cleanup complete |

**Rules:** `RUNNING` must never be set by metadata admission alone. Invalid transitions fail safely. Cleanup is deterministic and idempotent where possible.

## 5. Missing primitives and blockers summary

- Runnable context creation/integration (KDB↔PS/executive), entry transition, stack/ABI, execution-capable backend.
- Instance-attributed fault/exit signaling and teardown for executing code.
- KAPA runtime Service Bridge surface for Sweep (see `docs/SWEEP_SERVICE_BRIDGE.md`).
- Freestanding/runtime-portable Sweep app image and platform services (see `docs/SWEEP_RUNTIME_PORTABILITY.md`).
- Trusted admission/provenance/signing policy alignment for executable KAPA artifacts.
- VM-validated boot behavior for reduced kernel beyond attempted direct-kernel run (see `docs/SWEEP_BOOT_VALIDATION.md`).

**R7 status (2026-11-14):** the parser and DOM are compiled into the KAPA image and drive a canned document through JavaScript at CPL3 - parse -> JS query/mutate -> independent C-side verification - QEMU-demonstrated (`docs/SWEEP_R7_REPORT.md`; `build-generated/sweep_kapa_dom_qemu_boot.log`; `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1`, 58 assertions). The Sweep **browser** (CSS/layout/rendering) is still not an OWX image; R7 proves the parser/DOM/JS path the browser will stand on.

**Classification:** execution architecture and contract - **Investigated and specified**; controlled execution path - **Implemented and boot-demonstrated (R6)**.

## 6. Source-backed execution blocker table (R3/R4 audit; R5 closure)

R5 registers the PS executor backend during `OwKdbInitialize()` (`kdb/kdb_exec_ps.c` via `OwKdbExecPsInitialize()`), so `OwKdbExecutionAvailable()` is true in both profiles. A missing executor would return `OW_ERR_UNSUPPORTED` without touching instance state; the host test suite verifies exactly that. The management plane never fabricates `RUNNING`: it is set only by the scheduler dispatch hook (`OwPsPrepResume` -> `OwKdbNotifyThreadDispatch`), observable in the boot log; before dispatch, an instance is always `READY`.

| # | Required capability | Existing implementation (source) | Missing integration | Required change to close |
|---|---|---|---|---|
| B1 | Runnable instance address space / image mapping | PS process + OWX loader primitives: `OwPsCreateProcess`, `OwPsLoadImage`, per-process `Pml4Phys`/`FrameRun`/VADs (`inc/ow_ps.h`, `ps/owx_loader.c`); OWX validation in `kdb/kdb_loader.c` | KDB never creates a process/address space or maps image sections for an instance; `OwKdbLoadInstance` records only `ResidentPages`/`PageLimit`/`EntryPoint`/`MappedBase` | Add a KDB-owned instance context that materializes a PS (or dedicated) address space from a validated OWX image under the instance page budget, with explicit ownership and teardown |
| B2 | Entry ABI + stack + control transfer | PS user thread frame/trampoline helpers: `OwPsCreateUserThreadFrame`, `OwPsLaunchUserImageEx`, `OwPsBuildArgv` | No KAPA entry contract; no stack allocation or initial register/context build for a KDB instance; executor seam only (`kdb/kdb_exec.c`) | Define and document the KAPA entry signature/registers/stack contract, build the initial frame, and register a backend that performs the transition and reports the outcome |
| B3 | Scheduler integration (real `RUNNING`) | PS scheduler: `ps/ps.c`, `ps/switch.S`, run queues; timer/interrupt infrastructure | No PS thread is created or enqueued for a KDB instance; `RUNNING` is metadata only (`kdb/kdb_manager.c`) | Bind an instance context to a schedulable thread, define quantum/stop semantics, and set `RUNNING` only when control is actually dispatched |
| B4 | Instance-attributed execution faults | CPU/page-fault path (`core/pgfault.c`), logical `OwKdbFaultInstance` cleanup | No attribution of CPU exceptions/page faults to a KDB execution context; no fault-exit for executing app code | Add an execution record consulted by fault paths; stop the context, mark `FAULTED`, report status, and release resources exactly once |
| B5 | Return/exit path | PS exit trampoline / `PS_EXIT_THREAD` path for normal processes (`ps/owx_loader.c`) | No KAPA exit ABI or service; no return into KDB lifecycle from executing code | Add a `kapa.exit`-style service or return-to-kernel trampoline that transitions the instance to `EXITED` and performs deterministic cleanup |
| B6 | Sweep runtime service surface | Service Bridge: `OwKdbBridgeCall` with 4 built-ins (`main.ping`, `main.echo`, `main.log`, `main.runtime_info`), capability checks, per-call attribution, bounded copies (`kdb/kdb_bridge.c`, `kdb/kdb_internal.h`) | No Sweep-specific services (bounded resource read, allocation/workspace, timers, presentation); the Sweep runtime seam (`sweep/sweep_runtime*.c`) is host-only | Implement bounded kernel-backed runtime ops/service calls for alloc/time/resource-read, each capability-checked and instance-scoped; keep JS/browser logic out of the kernel |
| B7 | Runnable freestanding Sweep app image | Freestanding Duktape + shims + arena build and link with zero hosted symbols (`sweep/freestanding/`, `make sweep-freestanding`) | Not packaged into an OWX/KAPA image, not linked into the kernel, not executed in QEMU | Produce a validated OWX app image embedding the freestanding runtime, define its entry wrapper, and drive it through B1–B5 |
| B8 | Isolation/trust model for executing code | CIS verification gate in the PS loader path; private-memory/unprivileged compatibility policy (`kdb/kdb_registry.c`); reduced profile excludes syscall/usermode paths | No MMU/CPU isolation claim for executing KAPA code; reduced profile has no syscall gateway (`hal/idt.c` returns `OW_ERR_UNSUPPORTED`) | Choose and enforce a real isolation model (per-instance address space via PS is the only implemented mechanism) before enabling execution; never claim hardware isolation that is not enforced |

**Progress since R2:** B6 is partially closed - a bounded, capability-checked Service Bridge call plane with attribution and two new built-ins exists and is tested (hosttest). B7 is partially closed - the freestanding runtime now builds, links and smoke-tests without hosted symbols, **and (R6) is packaged as `kapa_sweep.owx` and executes real JavaScript at CPL3 with a defined exit and KDB attribution** (`docs/SWEEP_R6_REPORT.md`). **B1-B5 and B8 are closed** - the controlled KAPA execution path is implemented and boot-demonstrated in QEMU. The KAPA payload is now the genuine freestanding Sweep runtime, but still not the Sweep browser itself; a full-browser OWX image is the remaining step in B7.
 
