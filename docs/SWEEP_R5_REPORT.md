# Sweep R5 Report - Controlled KAPA Execution in the Reduced Sweep Kernel

**Date:** 2026-11-12  
**Status:** **Complete.** The R5 blockers B1-B5/B8 are closed by implemented, boot-demonstrated code. The reduced sweep-kernel profile now loads, trust-gates, dispatches and retires a real KAPA application (`kapa_test.owx`) on a real QEMU CPU, with KDB lifecycle attribution end to end.

## 1. Scope

Evolve the R1-R4 reduced profile into an independently buildable OpenWindows-derived kernel that hosts Sweep as a KAPA application via a *controlled* execution path: no fake executor, no direct kernel call into an unmanaged payload, no logical `RUNNING` presented as CPU execution, no host simulation as execution proof, CIS trust preserved, and no unrestricted kernel authority for KAPAs.

Non-goals (preserved from R4): no browser engine in the kernel, no general userspace, no arbitrary third-party KAPA, no MMU/CPU redesign, no physical presentation/compositor, no HTTP/HTTPS, no Duktape replacement, no Unicode redesign.

## 2. What was implemented

### 2.1 Reduced-profile KAPA gateway (`kdb/kdb_syscall.c`, new)

Vector `0x80` (reused IDT gate, DPL3, unchanged full-profile dispatcher untouched). In the sweep profile it routes `int $0x80` to `OwKdbSyscallDispatch` instead of `OwSyscallDispatch`:

- `OW_SYS_PS_GET_PID` (0x00F) - calling process id
- `OW_SYS_PS_EXIT_THREAD` (0x00E) - retire the calling thread
- `OW_SYS_UART_WRITE` (0x01B) - debug console write
- `OW_SYS_KDB_BRIDGE` (0x022) - Service Bridge call via `OW_SYS_KDB_BRIDGE_CALL`
  (magic `0x4B425247`, version 1, flags/reserved must be 0, `TargetInstanceId == 0`, the caller's association is derived from the live KDB thread binding, never from a caller-supplied identifier)
- everything else: refused.

User pointers are validated before any byte is copied: caller VAD tree (PRESENT+USER) + `OwMemTranslate` over the caller's own PML4 + guarded user window bounds; resident user pages only, otherwise refusal (no demand paging from kernel context). Responses are copied back through the same residency checks with an overrun check against the caller's `ResponseCapacity`.

### 2.2 PS-backed executor backend (`kdb/kdb_exec_ps.c`, new)

The single executor backend registered by `OwKdbInitialize()`:

`OwKdbExecuteInstance()` -> re-validated launch plan (`kdb_exec.c`) -> `kdb_rollback` -> `OwPsCreateProcess` -> `OwPsLoadImage` (CIS gate) -> `OwPsLaunchUserImageEx(..., Publish=false)` -> binding table (InstanceId/AssociationId/ProcessId/Tid) -> **publish** the thread -> scheduler dispatch hook.

Key design points:

- **No fake `RUNNING`.** Backend success only means a process/thread pair was prepared and enqueued. The instance leaves `READY` only in `OwKdbNotifyThreadDispatch()` (called from `OwPsPrepResume`, the one resume point every switch funnels through), which marks it `RUNNING` and logs it.
- **Attribution, not pointers.** The binding table stores identifiers; event-time resolution goes through `OwPsGetThreadByIndex`/`OwPsGetProcessById`, so a terminated process cannot leave a dangling KDB pointer.
- **Exit path.** CPL3 thread exits via `OW_SYS_PS_EXIT_THREAD` -> `OwPsExitThread` -> `OwKdbNotifyThreadExit` -> `OwKdbManagerFinish` (`STOPPED`, bridge services dropped, association unlinked) + `OwKdbExecPsAbandonInstance` (frame run released, VAD tree recycled).
- **Fault path.** Terminal CPL3 faults (page/guard classification) call `OwKdbNotifyThreadFault` from `core/pgfault.c` **before** any guard handling, only for CPL3 (CPL0 kernel faults are never laundered into KAPA faults), only when a live binding exists; otherwise the existing behavior is preserved byte for byte.
- **Abandon path.** Threads prepared but never dispatched are retired instead of stranded in the ready queue (`OwPsAbandonThread`).

### 2.3 Scheduling / publish race

The executor used to enqueue the CPL3 thread inside `OwPsCreateUserThreadFrame`, which is preemptable: a PIT tick could resume the payload **before the KDB binding existed**, so the gateway saw "no live KAPA binding" and the instance finished `READY`. Fixed by making thread publication an explicit caller decision:

- `OwPsLaunchUserImageEx(Proc, argc, argv, bool Publish)` - `Publish=false` leaves the entry thread `DORMANT`; `OwPsLaunchUserImage` passes `true` (classic behavior).
- The KDB executor publishes **after** binding registration, so the CPU can never reach the payload before the gateway can attribute it.
- The payload then executes, calls the Service Bridge from CPL3 and exits with a defined status (`0x5A`).

### 2.4 KAPA payload (`sweep/kapa_test/kapa_test.c`, new)

Minimal freestanding payload: builds one `OW_SYS_KDB_BRIDGE_CALL` on its user stack, calls `main.log` (bounded, no format-string interpretation) and `main.ping`, then exits with `0x5A`. Static/stack memory only; no libc, no heap. It is built by `make` as an OWX with **subsystem 0x03 (console)** - not the kernel/native subsystem, which is what latches the `CORE_KERNEL` copyleft policy into refusing a KAPA payload.

### 2.5 Sweep-kernel profile (R1) fixes needed by R5

- The reduced boot **now arms CIS** (`OwCisInitialize` + trust-anchor check) before `Boot Complete` - security-critical, and it must be live before the executor maps any KAPA image. Zero anchors arms it fail-closed and says so.
- The sweep gateway is wired in `hal/idt.c` (`OwKdbSyscallDispatch`) for the `OW_SWEEP_KERNEL` build only.
- The idle marker prints only outside the KAPA self-test build.
- `core/main.c` KAPA self-test boot block (compile-time gated on `OW_SWEEP_KAPA_SELFTEST`): find kernel `sweep`, attach the embedded `kapa_test.owx`, associate `sweep-kapa`, `OwKdbLoadInstance`, `OwKdbExecuteInstance`, wait for a terminal lifecycle state via `OwPsYield()`, log it. No full-profile images are built with this block.
- The KAPA payload is carried as data via `tools/embed_owx.py` (`kapa_test_image.h`).

### 2.6 Build system (`Makefile`)

`sweep-kernel` unchanged in name/outputs; new `qemu-sweep-kapa-test` target (+ `tools/qemu_sweep_kapa_test.ps1`) that requires the R5 dev-trust opt-in (`CIS_DEV_TRUST=1 make qemu-sweep-kapa-test`). Without the opt-in the target stops with exit 2 instead of running a guaranteed-refusal boot. CIS arming and the CIS `SUTS`/profile handling mean the default-untrusted build keeps CIS at zero anchors by default.

## 3. Evidence (actual, captured)

Reduced-profile KAPA boot (`build-generated/sweep_kapa_qemu_boot.log`, 4,298 B, 2026-11-12 12:34):

```
[KDB] instance 1: app 'sweep-kapa' loaded 'sweep' (3 page(s), domain 1)
[PS] sweep: process created (PID 1)
[CIS] sweep: TRUSTED, signed by dev, licence accepted
[FINISHED] OWX Image Load (code 0x11AC12)
[PS] sweep image loaded: 5 section(s), 6 page(s) charged, entry 0x4000067 (bias 0xFFFFFFFEC3FFF000), exit 0x4005000
[PS] sweep: USER thread 1 created, ring3 entry 0x4000067 stack 0x41FEFF8
[PS] sweep: image entered at CPL3, rsp 0x41FEFF8, exit 0x4005000
[KDB] instance 1: backend prepared process 1 (thread 1) for 'sweep'
[KDB] instance 1: bound thread resumed on CPU (state=RUNNING)
[KDB][LOG] assoc=1 target=0 len=16: kapa-r5-entry-ok
[EX] tid=1
[PS] sweep: process terminated (PID 1, exit 90)
[SWEEP-KAPA] selftest instance 1 finished (state=stopped fault=0x0 exit=90)
[HAL] BROADCASTING IPI_FREEZE_SMP TO ALL CORES
```

Every link in the chain is observed: KDB load -> CIS trust -> CPL3 entry -> backend prepare -> scheduler dispatch -> Service Bridge (`main.log` with `assoc=1 target=0`) -> defined exit (`0x5A` = 90) -> KDB `STOPPED` with `fault=0x0`. `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` asserts exactly these markers plus absence of all excluded subsystems and any gateway refusal/fault; it exits 0 with **PASS**.

Plain reduced profile (`build-generated/sweep_kernel_qemu_boot.log`, 3,721 B, 2026-11-12 12:34): unchanged boot behavior - KDB/bridge online, `[SWEEP-KERNEL] Reduced profile idle`, no KAPA markers. `make qemu-sweep-kernel` exits **PASS**.

## 4. Verification matrix (2026-11-12, all fresh)

| Command | Result |
|---|---|
| `make -B -j 2` (full kernel) | **Success**, `openwinkrnl.owx` 1,658,880 B, SHA-256 `ee900842b7b5db63c6663c6b51cb5166c7b6be955eff7b50ac232a52229ff1ca`, matches `.chk` |
| `make -B sweep-kernel` | **Success**, `sweepwinkrnl.owx` 1,527,808 B, SHA-256 `b0182a3296ba1ee992c8df71ee1073dbd80156f0e2bae7902207fe2c35f03fa7`, `build/sweep-kernel/` 106 `.o` |
| `make sweep-tests` | **1007 passed, 0 failed** (engine 24, services 18, text 37, browser 39, HTML 40, CSS 208, JS 187, document 258, resource 15, runtime 60, render 121) |
| `make hosttest` | **1424 passed, 0 failed** (1392 baseline + 32 new executor-seam assertions; +service bridge attribution) |
| `make sweep-freestanding` | **Exit 0**; `nm -u` closure OK; arena peak 118,480 B, 0 outstanding |
| `make check-external-deps` (full, sweep-kernel, sweep-tests, freestanding) | **OK** (captures 18/10/3/0 key files); bogus `VIP_ROOT` and unknown profile fail clearly |
| `make qemu-sweep-kernel` | **PASS** |
| `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | **PASS** (24 checks, 0 failures) |

`tools/hosttest/host_boot.c` gained executor-seam tests (stub backend success/failure, unregistration, no-handoff-means-not-RUNNING, real-backend rollback in host mode): all PASS, plus retained R3 bridge/attribution suite.

## 5. Blocker closure

| # | Blocker | Resolution |
|---|---|---|
| B1 | Runnable instance address space / image mapping | **Closed.** PS process with private PML4/frame-run/VADs; image mapped via CIS-gated loader; entry 0x4000067; user stack mapped; KDB knows PID/TID. |
| B2 | Entry ABI + stack + control transfer | **Closed.** Entry ABI = Microsoft x64 (MinGW `.owx`) contract of the existing PS user-thread frame; stack top-down from `OW_USER_STACK_TOP`, exit trampoline, `RCX=argc/RDX=argv/R8=envp`; transition reuses the existing ring-3 `ow_ps_resume_thread`/iretq machinery. |
| B3 | Scheduler binding / real RUNNING | **Closed.** `OwPsPrepResume` -> `OwKdbNotifyThreadDispatch` -> `OwKdbManagerMarkRunning` (observable `[KDB] ... state=RUNNING` log). Stop/exit path through `OwPsExitThread` -> `OwKdbNotifyThreadExit` -> `STOPPED`. |
| B4 | Instance-attributed execution faults | **Implemented (code), partially test-covered.** `core/pgfault.c` classifies CPL3 terminal faults before guard handling and attributes them via `OwKdbNotifyThreadFault` -> `OwKdbFaultInstance` -> `FAULTED`; kernel-CPL faults are never laundered into KAPA faults. The backend-failure -> `FAULTED` rollback path is host-tested; the CPL3 pgfault attribution itself is not yet exercised in host or QEMU (the payload does not fault). |
| B5 | Return/exit path | **Closed.** `OW_SYS_PS_EXIT_THREAD` (0x00E) on the reduced gateway; defined exit status `0x5A` observed; KDB `STOPPED` via the exit notification. |
| B8 | Isolation/trust model | **Closed (honestly scoped).** CIS-gated trusted admission for the image; per-instance address space via PS primitives; **no MMU/CPU isolation claim** - nothing in this profile prevents a KAPA from reading nothing it shouldn't, and nothing in this profile implements second-kernel/VM isolation; that remains a non-goal. |
| B6 | Service Bridge surface | **Partially closed (by design).** The reduced gateway exposes only `main.ping`/`main.log`/`main.runtime_info`/`main.echo` (+`OW_SYS_PS_EXIT_THREAD`/`OW_SYS_PS_GET_PID`); Sweep-specific runtime services remain unspecified by design. |
| B7 | Freestanding image as app | **Partial.** The freestanding Duktape build remains object-level/host-smoke-tested; `kapa_test.owx` proves the KAPA path with a 3-page minimal payload; packaging the full Sweep runtime as an OWX KAPA app is not done. |

## 6. Limitations (explicitly stated)

1. **No MMU/CPU/VM isolation is claimed or implemented.** The per-instance address space comes from PS primitives (private page directories, VAD trees, frame runs); there is no second-kernel, VM, VT-x/SVM, or sub-ring isolation. "Trusted" here means a CIS verdict, not a hardware boundary.
2. **Bump-arena limitation.** Physical frames and private page-table pages are never recycled into the allocator (the arena is monotonic). `OwPsTerminateProcess` releases the commit charge and now recycles the VAD tree to the pool, but the physical memory of terminated processes is not reusable by later ones. This bounds KAPA churn to the process/thread/page limits; it does not provide dynamic isolation.
3. **VAD free-list bounded.** The VAD tree recycle is pool-based and bounded by `OW_VAD_POOL_SIZE`; on pool exhaustion `OwMemCreateVad` returns 0 and launches fail safely.
4. **CIS dev anchors are opt-in only** (`CIS_DEV_TRUST=1`, `--sign-dev`, public dev seed, `cis/cis_keys_dev.h`). A default build arms CIS with zero anchors and refuses every image - the correct security default. No new bypass was introduced.
5. **No wall-clock time; no file/stdio/network/storage/presentation for the KAPA.** `main.runtime_info` is a monotonic 64-bit millisecond counter.
6. **Entry ABI is the MinGW/OWX ABI only.** SysV-entry images are not supported by this contract.
7. **The KAPA gateway is intentionally minimal.** Any new service or syscall added to it must go through the same binding/attribution/residency review as the existing four.
8. **Cross-instance Service Bridge calls are not exposed to KAPAs** (`TargetInstanceId` must be 0 in R5); the kernel-side bridge API remains for instance-owned services.

## 7. Modified/created files (R5)

Created: `kdb/kdb_exec_ps.c`, `kdb/kdb_syscall.c`, `sweep/kapa_test/kapa_test.c`, `tools/qemu_sweep_kapa_test.ps1`, `docs/SWEEP_R5_REPORT.md`.  
Modified: `Makefile`, `inc/ow_syscall.h`, `inc/ow_ps.h`, `inc/ow_kdb.h`, `emergency/ow_gate.h`, `hal/idt.c`, `core/pgfault.c`, `core/main.c`, `ps/ps.c`, `kdb/kdb_manager.c`, `kdb/kdb_internal.h`, `tools/hosttest/host_boot.c`, `docs/SWEEP_*.md`, `SWEEP.md`. `openwinkrnl.owx`/`sweepwinkrnl.owx` and their `.chk` were rebuilt; no sibling repositories or OpenWindows upstream files were modified. `kdb/kdb_exec.c`, `ps/owx_loader.c`, `syscall/dispatcher.c` and the full-profile boot path are byte-identical in intent: the sweep gateway is reached only behind `OW_SWEEP_KERNEL`.

