# Sweep R3 Continuation Report

> **Historical document.** R4 later completed the hosttest run and the freestanding
> Duktape object build. Where this report says hosttest was not completed or that
> freestanding porting is blocked, `docs/SWEEP_R4_REPORT.md` supersedes it.

Date: 2026-11-11 (continuation work in `C:\Users\KARIMABENDA\Documents\Sweep`, git `master`, uncommitted/untracked working tree).  
Scope: Service Bridge hardening/tests, KAPA execution feasibility/source trace, Sweep runtime portability audit/foundations, external dependency reproducibility, and documentation/evidence updates.

## Classification legend
- **Implemented and tested:** code present with direct execution evidence in this continuation.
- **Implemented but not fully run:** code/tests present but full execution evidence not obtained in this continuation.
- **Partially implemented:** useful progress, incomplete against mission target.
- **Investigated but not implemented:** source/audit/doc evidence only.
- **Blocked by missing primitives:** cannot be safely completed with existing kernel contracts/primitives.
- **Deferred:** planned follow-up.

## 1. Baseline verification (actual)
- Read/inspected: `docs/SWEEP_KERNEL_PROFILE.md`, `docs/SWEEP_KAPA_INTEGRATION.md`, `docs/SWEEP_KAPA_EXECUTION.md`, `docs/SWEEP_SERVICE_BRIDGE.md`, `docs/SWEEP_BOOT_VALIDATION.md`, `docs/SWEEP_RUNTIME_PORTABILITY.md`, `SWEEP.md`, `inc/ow_kdb.h`, `kdb/kdb_internal.h`, `kdb/kdb_bridge.c`, `kdb/kdb_exec.c`, `kdb/*`, `ps/ps.c`, `ps/owx_loader.c`, `inc/ow_ps.h`, `hal/*` timing/transition surfaces, `Makefile`, `sweep/sweep_services.*`, `sweep/tests/sweep_services_test.c`, `tools/qemu_sweep_kernel_test.ps1`, boot assets.
- R3 additions present/verified in source: `OW_KDB_SERVICE_LOG` (`0x4B440003`, `main.log`), `OW_KDB_SERVICE_RUNTIME_INFO` (`0x4B440004`, `main.runtime_info`) in `inc/ow_kdb.h`; handlers/registration/attribution in `kdb/kdb_bridge.c`; internal call attribution in `kdb/kdb_internal.h`.
- Full kernel build: `make -j 2` **PASS** (2026-11-11 15:29:26) → `openwinkrnl.owx` 1,646,592 B, `openwinkrnl.pe` 2,790,932 B, `openwinkrnl.chk` 65 B.
- Reduced kernel build: `make sweep-kernel` **PASS** (2026-11-11 15:29:25) → `sweepwinkrnl.owx` 1,515,520 B, `sweepwinkrnl.pe` 2,399,016 B, `sweepwinkrnl.chk` 65 B, `build/sweep-kernel/` 104 `.o` (101 base objects).
- Sweep host tests: `make sweep-tests` **PASS 920/0** (all suites; render 121/0). Baseline maintained.
- Reduced QEMU boot: `make qemu-sweep-kernel` **PASS** with fresh log `build-generated/sweep_kernel_qemu_boot.log` (3,294 B, 2026-11-11 15:29:42). Asserted: `[STARTED] HAL Init`, `[KDB] registry online:`, `[KDB] service bridge online: 4 built-in service(s)`, `sweep-kernel profile: KDB/KAPA substrate ready`, `kernel boot complete in ...`, `[SWEEP-KERNEL] Reduced profile idle (no native Sweep execution).` Forbidden VFS/USFS/network/vip/sentinel/syscall/shell/owinit markers absent.
- OpenWindows host tests (`C:\Users\KARIMABENDA\Documents\OpenWindows`): **not rerun** (historical 1418/0, R1). No sibling-repo modifications.

## 2. Workstream C — Service Bridge hardening (`main.log`, `main.runtime_info`)

### Implementation (present)
- `main.ping` (`0x4B440001`) and `main.echo` (`0x4B440002`) preserved (registration, bounds, capabilities, behavior unchanged).
- `main.log` (`0x4B440003`, owner 0, `OW_KDB_CAP_CALL`, max request 128, max response 0): request bytes copied into bridge/KDB-owned buffer; handler copies bounded message to local NUL-terminated buffer and emits attributed log via `ow_kprintf` with fixed format (`assoc/target/len`) — **message treated as data, no format-string interpretation of request**. Response length 0 on success.
- `main.runtime_info` (`0x4B440004`, owner 0, `OW_KDB_CAP_CALL`, max request 0, max response 8): returns 8-byte little-endian `uint64_t` from `OwHalUptimeMs()` (real HAL monotonic uptime primitive, ms). Insufficient response capacity refused.
- Bridge call path (`OwKdbBridgeCall`, `kdb/kdb_bridge.c`): validates caller association, target instance existence/running/ownership or admin where applicable, service lookup/ownership, required capabilities, request/response bounds/capacity, copies request into KDB-owned buffer, provides handler with KDB-owned response buffer, checks produced length, copies back only on success, updates bridge/instance accounting. Call attribution (`CallerAssociationId`, `TargetInstanceId`) set before handler dispatch and cleared after dispatch for in-progress handler use (`OwKdbBridgeGetCallAttr`).

### Focused tests (hosttest harness updates)
- **Implemented but not fully run in this continuation:** extended `test_kdb()` in `tools/hosttest/host_boot.c` to reflect 4 built-in services and add focused cases for `main.log`/`main.runtime_info`:
  - registration/count expectations updated (4 builtins), service IDs/caps/bounds exercised through call contracts.
  - `main.log`: bounded message, empty message, maximum-length (128), oversized request refused, CAP_CALL enforcement, unknown caller association refused, success after calls (attribution cleanup path exercised).
  - `main.runtime_info`: valid empty request with 8-byte LE response, repeat call with `OwHalDelayMs(5)` showing monotonic non-decreasing uptime (`t2 >= t1` under host HAL), insufficient response capacity refused, non-empty request refused (MaxRequest 0), CAP_CALL enforcement, unknown caller association refused.
  - `Makefile` hosttest source list fixed to include `kdb/kdb_exec.c` (required for `OwKdbExecReset` linkage in updated KDB core).
- `make hosttest` (Sweep host kernel harness) was started after fixes but **did not complete within the execution window available in this continuation** (long-running compile+run harness). Direct hosttest pass/fail count for the updated suite is therefore **not newly captured** here. KDB bridge/kernel builds and QEMU boot remain green.

### Audit notes
- Logging: bounded copy, NUL-terminated local buffer, fixed `ow_kprintf` format string (request bytes not used as format). Attribution sourced from bridge call state (single-threaded kernel API contract per `inc/ow_kdb.h`); attribution cleared after handler return in current call flow.
- Runtime info: uses real `OwHalUptimeMs()` (64-bit ms uptime from HAL), not wall-clock time. Serialization explicit little-endian.
- Host-side Sweep services (`sweep.url.admit` `0x53570001`, `sweep.viewport` `0x53570002`, `sweep.paint.append` `0x53570003`) remain host-tested handler contracts only; **not proof of native KDB wiring/execution**.

## 3. Workstream B — KAPA execution source trace and feasibility

### Source trace (investigated)
- PS/loader primitives inspected: `ps/owx_loader.c` (`OwPsLoadImage`, OWX header/structure/self-contained checks, CIS-trusted gate, mapping/VAD/frame-run/permissions flow, last CIS verdict), `ps/ps.c` (`OwPsCreateProcess`, address-space/PML4/frame-run/VAD/user heap/stack state, `OwPsBuildArgv`/`OwPsStampEntryArgs`, `OwPsLaunchUserImageEx`/`OwPsCreateUserThreadFrame`, stack/guard mapping, eager stack frame seeding via physical frame addresses, user thread enqueue, `OwPsYield`/`OwPsSchedulerTick`/`OwPsScheduleNext`/`OwPsWakeThread`, `OwPsExitThread`/`OwPsTerminateProcess`/cleanup semantics), `inc/ow_ps.h`.
- KDB execution seam: `kdb/kdb_exec.c`/`inc/ow_kdb.h` (`OwKdbBuildLaunchPlan`, executor registration slot, `OwKdbExecutionAvailable`, `OwKdbExecuteInstance` validation/handoff/fault behavior), manager/instance lifecycle and state ownership.
- Reduced profile boot (`core/main.c`, `hal/idt.c`, `core/runlevel.c`): syscall dispatch gated to `OW_ERR_UNSUPPORTED` in sweep profile; reduced profile ends in KDB-ready scheduler+idle halt (no userspace handoff).

### Feasibility decision (source-backed)
- **Genuine KAPA CPU execution prototype: not safely feasible in this continuation (blocked by missing primitives/contracts).**
- Reusable primitives exist: OWX parsing/validation and CIS-gated image loading/mapping machinery, per-process address-space/frame-run/VAD/stack/thread/scheduler/resume/exit primitives, KDB management/launch-plan/executor seam and bridge.
- Key blockers/gaps:
  1. **No execution backend / binding:** no default executor; KDB instances not bound to PS process/thread/address space or scheduler context; `OwKdbExecuteInstance` cannot attribute real CPU execution lifecycle to an instance.
  2. **Image trust/payload contract:** `OwPsLoadImage` requires CIS-trusted verdict before mapping (refusal without partial mapping). A minimal controlled KAPA test payload needs a defined, trusted/admissible image/provenance path compatible with current CIS/policy and KDB ownership.
  3. **Entry/exit/ABI contract:** reduced profile has no syscall gateway for user->kernel exit/service ABI; controlled payload exit/result/fault signaling attributable to the correct KAPA instance is undefined.
  4. **Attribution and cleanup:** instance-attributed execution start, exit status, fault capture, and safe coordinated PS/KDB resource reclamation (process/thread/address-space/frame-run/VAD/services/instance state) are not implemented end-to-end.
  5. **State semantics:** logical KDB `READY/RUNNING/STOPPED/FAULTED` must not be conflated with CPU execution; genuine dispatch/entry proof requires independent evidence.

### Minimum changes required (next phase)
- Define minimal KAPA executable payload + ABI (entry, stack/args, exit/status, fault signaling, authorized bridge access) and a test-only trusted image provisioning path.
- Bind KDB instance lifecycle to a dedicated PS execution context (process/address space/thread ownership, validated mapping, scheduler admission) via executor backend.
- Implement controlled exit/fault traps and instance-attributed result/fault reporting with deterministic cleanup and accounting.
- Add independent tests proving real entry reached, controlled operation, defined exit/fault, correct attribution, and no leaks/double-free/UAF/stale callbacks.

## 4. Workstream D — Sweep runtime portability foundations

### Dependency audit (investigated, `sweep/`)
- Hosted/allocation: `malloc/calloc/realloc/free` in `sweep_js.c`, `sweep_js_engine.c` (Duktape alloc wrappers), `sweep_document.c`.
- Hosted I/O/files: `fopen/fread/fclose` in `sweep_resource.c`; `snprintf`/stdio usage in `sweep_document.c`.
- Timing: `clock()` in `sweep_js_engine.c` for Duktape deadline/interrupt handling.
- String/memory headers and libc assumptions present across engine/html/css/js/render/document/resource modules.
- Initial classification:
  - **Pure/mostly algorithmic:** parsing/layout/style/DOM/document/render logic in bounded modules (per-module proof still required).
  - **Replaceable runtime abstractions:** allocation/reallocation/free, monotonic time/deadline, diagnostics, resource reads, init/shutdown hooks.
  - **Hosted-only (current):** file-backed resource provider (`fopen/fread`), Duktape 2.7.0 hosted integration (`clock`, hosted allocator model, runtime config/teardown assumptions), stdio formatting paths.
  - **Kernel/KAPA-provided service candidates (future):** monotonic timing, logging/diagnostics, bounded resource mediation.

### Abstractions
- **Investigated/not yet integrated:** minimal runtime interface and host implementation should be introduced incrementally (alloc/realloc/free, monotonic time/deadlines, diagnostics, bounded resource reads, init/shutdown) with explicit ownership, bounded/deterministic failure behavior, and host-testable backends before wider refactor. No host behavior changed in this step.

### Duktape
- **Hosted, not freestanding.** Future freestanding/KAPA integration requires: custom allocator/reallocator/deallocator, heap/interrupt deadline source decoupled from `clock()`, removal/replacement of hosted I/O dependencies as needed, constrained config/build integration, explicit teardown. No freestanding claim.

## 5. Workstream E — External dependency reproducibility

### Audit
- External sibling roots referenced: `../OpenWindows-Essentials`, `../superunicode`, `../vip`, `../OpenWindows-Storage` (headers/sources/vendor SUF, storage/OWFS/USFS/common, VIP, essentials tools/libs/images as used by build/test flows).
- Usage spans compile-time includes/sources, link-time objects, generated/font/vendor inputs, and tooling/image checks depending on profile/target.

### Configurable roots (partially implemented)
- `Makefile`: added configurable root variables with preserved defaults and refactored core path/CFLAGS/include derivations:
  - `ESSENTIALS_ROOT ?= ../OpenWindows-Essentials`
  - `SUPERUNICODE_ROOT ?= ../superunicode`
  - `VIP_ROOT ?= ../vip`
  - `STORAGE_ROOT ?= ../OpenWindows-Storage`
  - `SUTF_DIR/SUST_DIR/EXTSU_DIR/SUAS_DIR/SUF_DIR/VIP_DIR/STORAGE_COMMON_DIR/OWFS_DIR/USFS_DIR` and kernel/hosttest include paths derived from these roots (defaults preserve existing sibling-relative behavior).
- **Implemented and tested (default behavior):** `make -j 2`, `make sweep-kernel`, `make sweep-tests`, `make qemu-sweep-kernel` **PASS** with defaults.
- **Partially implemented/deferred:** complete per-target override coverage/validation across all tooling/scripts/packaging paths, explicit missing-dependency failure messages for every required input, and full dependency/profile matrix documentation.

## 6. Build/test/boot verification (exact, this continuation)
| Check | Command/context | Result |
|---|---|---|
| Full kernel build | `make -j 2` (`C:\Users\KARIMABENDA\Documents\Sweep`) | **PASS** (`openwinkrnl.owx` 1,646,592 B, 2026-11-11 15:29:26) |
| Reduced kernel build | `make sweep-kernel` | **PASS** (`sweepwinkrnl.owx` 1,515,520 B, 2026-11-11 15:29:25, `build/sweep-kernel/` 104 objs) |
| Sweep host tests | `make sweep-tests` | **PASS: 920 passed, 0 failed** (render 121/0) |
| Reduced-kernel QEMU boot (fresh) | `make qemu-sweep-kernel` | **PASS** (`build-generated/sweep_kernel_qemu_boot.log` 3,294 B, 2026-11-11 15:29:42; HAL/KDB/bridge(4)/profile/boot-complete/idle asserted; forbidden markers absent) |
| Sweep hosttest harness (KDB-focused additions) | `make hosttest` | **Not completed in this continuation** (long-running; updated tests compiled into harness but final result not captured) |
| OpenWindows host tests | `make hosttest` in `C:\Users\KARIMABENDA\Documents\OpenWindows` | **Not rerun** (historical 1418/0, R1) |

## 7. Modified/added files (actual)
- `inc/ow_kdb.h` — service IDs/docs for `main.log` and `main.runtime_info`.
- `kdb/kdb_internal.h` — bridge call attribution type/accessor.
- `kdb/kdb_bridge.c` — `main.log`/`main.runtime_info` handlers, builtin registration, attribution handling; preserved `main.ping`/`main.echo`.
- `tools/hosttest/host_boot.c` — focused KDB bridge test additions and service-count updates for 4 builtins.
- `Makefile` — hosttest links `kdb/kdb_exec.c`; configurable external dependency roots/default-derived paths/includes (preserved defaults); sweep QEMU targets present in working tree.
- `docs/SWEEP_R3_CONTINUATION_REPORT.md` — added (this report).

## 8. Remaining limitations
- No genuine KAPA CPU execution demonstrated (`RUNNING` logical only; no backend/binding/ABI/exit-fault attribution path). Booted/validated image != executed program.
- Sweep runtime/JS (Duktape 2.7.0) not freestanding/native; no physical presentation/compositor API.
- No MMU/CPU/VM isolation claim; security bounded to implemented logical checks/bounds/capabilities/ownership.
- Hosttest harness with new KDB bridge tests not fully executed to completion in this continuation (evidence gap).
- Dependency configurability and per-profile matrix/docs not fully completed across all targets/tooling.
- Existing R3 docs need full continuation-evidence refresh where not yet updated.

## 9. Recommended next phase
1. **Complete Workstream C evidence:** run updated `make hosttest` to completion (or a focused KDB-only harness) and capture exact pass/fail results; add any missing invalid-lifecycle/ownership/handler-failure attribution tests not yet covered.
2. **Workstream B prototype-or-blocker closure:** implement minimal trusted test-payload path + executor-backed PS/KDB binding only if safe; otherwise land a precise minimal-change execution contract/plan with required kernel API deltas.
3. **Workstream D incremental integration:** introduce `sweep_runtime` abstractions (alloc/time/diag/resource/init-shutdown), adopt in selected modules behind host backend, keep `make sweep-tests` 920/0; document Duktape freestanding boundary.
4. **Workstream E completion:** extend configurable roots to all relevant targets/scripts, add explicit missing-dependency checks and dependency/profile matrix, and refresh all R3/R3-continuation docs with final classified evidence.