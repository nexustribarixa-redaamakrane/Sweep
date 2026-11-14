# Sweep Kernel Boot Validation (R3, R5/R6/R7 KAPA evidence)

**Date:** 2026-11-14 (R3 harness; R4 fresh re-run; R5/R6/R7 KAPA evidence)  
**Primary R3 target:** reduced profile multiboot test image built from `build/sweep-kernel/` objects (`SWEEP_KERNEL_OBJS`, `OW_SWEEP_KERNEL`) via Makefile target `qemu-sweep-kernel`.  
**R5 target:** minimal `kapa_test.owx` self-test — `make qemu-sweep-kapa-test CIS_DEV_TRUST=1`, log `build-generated/sweep_kapa_qemu_boot.log`.  
**R6 targets:** the actual freestanding runtime — `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1`, log `build-generated/sweep_kapa_runtime_qemu_boot.log`; and the controlled CPL3 fault payload — `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1`, log `build-generated/sweep_kapa_fault_qemu_boot.log`.  
**R7 target:** the actual HTML parser + DOM — `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1`, log `build-generated/sweep_kapa_dom_qemu_boot.log`.
Related reduced artifact: `sweepwinkrnl.owx` (rebuilt 2026-11-14 13:47, 1,531,904 B, SHA-256 `7c699ddc`.
## 1. Environment and tooling

- Host OS: Windows (`win32`).
- QEMU: `qemu-system-x86_64` available (used by harness).
- Repository boot assets: `boot/qboot.S`, `boot/qemu_entry.c`, `boot/qemu.ld`, `Makefile` sweep QEMU rules/targets, `tools/qemu_sweep_kernel_test.ps1`.
- Harness: `tools/qemu_sweep_kernel_test.ps1` builds/runs the sweep multiboot image with serial socket capture to a deterministic log, bounded wait/timeout behavior, milestone assertions, and forbidden-marker checks. Exit status reflects pass/fail.

## 2. R3 boot validation (actual, fresh re-verified)

| Attempt | Command (actual) | Result | Evidence |
|---|---|---|---|
| Reduced-profile QEMU multiboot boot | `make qemu-sweep-kernel` (`C:\Users\KARIMABENDA\Documents\Sweep`) | **PASS** | `build-generated/sweep_kernel_qemu_boot.log` (3,721 B, 2026-11-12 12:34; R5 re-run). Asserted: `[STARTED] HAL Init`, `[KDB] registry online:`, `[KDB] service bridge online: 4 built-in service(s)`, `sweep-kernel profile: KDB/KAPA substrate ready`, `kernel boot complete in ...`, `[SWEEP-KERNEL] Reduced profile idle (no native Sweep execution).` Forbidden markers absent. |
| Full/reduced kernel artifact builds | `make -B -j 2`, `make sweep-kernel` | **PASS** | `openwinkrnl.owx` 1,662,976 B (2026-11-13 19:55, SHA-256 `6fb140a25ef8aa13ecc4ad4cdd61a30772d78534381b1a813c0b644eb92071f0`), `sweepwinkrnl.owx` 1,531,904 B (2026-11-13 20:04), `build/sweep-kernel/` 106 objs; R5 artifact hashes are recorded in `docs/SWEEP_R5_REPORT.md` (pre-relocation-fix builds) |
| R3 direct `-kernel sweepwinkrnl.owx` attempt (historical) | `qemu-system-x86_64 -kernel sweepwinkrnl.owx ...` | **No captured serial transcript** | Recorded in R2; not a proof of boot. Superseded for reduced-profile validation by multiboot harness above. |
| R5 KAPA self-test (CIS-gated, `CIS_DEV_TRUST=1`) | `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | **PASS** | `build-generated/sweep_kapa_qemu_boot.log` (4,298 B, 2026-11-12). Asserted: KDB instance load, `[CIS] sweep: TRUSTED`, CPL3 entry, dispatch-attributed `RUNNING`, `kapa-r5-entry-ok`, `exit 90`, `state=stopped`. |
| R6 freestanding-runtime KAPA (CIS-gated, `CIS_DEV_TRUST=1`) | `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` | **PASS** | `build-generated/sweep_kapa_runtime_qemu_boot.log` (4,595 B, 2026-11-13 20:05). Asserted: KDB instance load, `[CIS] sweep: TRUSTED`, CPL3 entry, dispatch-attributed `RUNNING`, `kapa-sweep: runtime initialized`, `script 1 ok result=45`, `script 2 ok (duktape smoke)`, `shutdown ok`, `state=stopped fault=0x0 exit=90`; all excluded-subsystem/refusal/fault markers absent. |
| R6 controlled CPL3 fault KAPA (`CIS_DEV_TRUST=1`) | `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` | **PASS** | `build-generated/sweep_kapa_fault_qemu_boot.log` (4,436 B, 2026-11-13 20:05). Asserted: KDB instance load, `[CIS] sweep: TRUSTED`, CPL3 entry, dispatch-attributed `RUNNING`, attributed bridge call right up to the fault (`kapa-fault: about to fault deliberately`), `[PF] NOT-MAPPED err=5 cr2=0`, `[KDB] instance 1 faulted (0x1); resources released`, `state=faulted fault=0x1 exit=0`; all excluded-subsystem/refusal/non-fault markers absent. |

### R6 relocation evidence

`build-generated/sweep_kapa_runtime_qemu_boot.log` proves the OWX relocation pass, which R5's minimal images never needed:

```
[OWX] applied 211 base relocation(s)
```

The relocations are applied after CIS authentication and before any code runs; the image is authenticated byte-for-byte before any fix-up lands in the mapped process pages.

## 2.1 R5 KAPA controlled-execution evidence (historical, still passing)

Log excerpt (complete chain, in boot order) — unchanged and still asserted by `make qemu-sweep-kapa-test CIS_DEV_TRUST=1`:

```
[KDB] instance 1: app 'sweep-kapa' loaded 'sweep' (3 page(s), domain 1)
[CIS] sweep: TRUSTED, signed by dev, licence accepted
[PS] sweep: image entered at CPL3, rsp 0x41FEFF8, exit 0x4005000
[KDB] instance 1: backend prepared process 1 (thread 1) for 'sweep'
[KDB] instance 1: bound thread resumed on CPU (state=RUNNING)
[KDB][LOG] assoc=1 target=0 len=16: kapa-r5-entry-ok
[EX] tid=1
[PS] sweep: process terminated (PID 1, exit 90)
[SWEEP-KAPA] selftest instance 1 finished (state=stopped fault=0x0 exit=90)
```

Classification: this is **genuine CPU execution of the KAPA payload** inside its own per-instance address space. It is **not** evidence about isolation, MMU/CPU boundaries, second kernels or the Sweep browser runtime — none of those are claimed.

## 2.2 R6 freestanding-runtime KAPA evidence (actual, fresh)

Complete chain from `build-generated/sweep_kapa_runtime_qemu_boot.log`:

```
[KDB] instance 1: app 'sweep-kapa-runtime' loaded 'sweep' (100 page(s), domain 1)
[PS] sweep: process created (PID 1)
[CIS] sweep: TRUSTED, signed by dev, licence accepted
[OWX] applied 211 base relocation(s)
[PS] sweep image loaded: 7 section(s), 152 page(s) charged, entry 0x4000360 (bias 0xFFFFFFFEC3FFF000), exit 0x4097000
[PS] sweep: USER thread 1 created, ring3 entry 0x4000360 stack 0x41FEFF8
[PS] sweep: image entered at CPL3, rsp 0x41FEFF8, exit 0x4097000
[KDB] instance 1: bound thread resumed on CPU (state=RUNNING)
[KDB][LOG] assoc=1 target=0 len=51: kapa-sweep: runtime initialized, arena 196608 bytes
[KDB][LOG] assoc=1 target=0 len=33: kapa-sweep: script 1 ok result=45
[KDB][LOG] assoc=1 target=0 len=39: kapa-sweep: script 2 ok (duktape smoke)
[KDB][LOG] assoc=1 target=0 len=23: kapa-sweep: shutdown ok
[EX] tid=1
[PS] sweep: process terminated (PID 1, exit 90)
[SWEEP-RUNTIME] selftest instance 1 finished (state=stopped fault=0x0 exit=90)
```

Classification: **the actual freestanding Sweep runtime — the unmodified Duktape engine over the R4 `SWEEP_RUNTIME` seam — is compiled into `kapa_sweep.owx`, CIS-verified, loaded as a charged KDB instance, entered at CPL3 by the real scheduler, executes JavaScript (numeric result verified and printed as `result=45`; the full freestanding Duktape smoke passed), destroys its heap, asserts its arena is empty, shuts down and exits with a defined status.** It is **not** evidence about browser content, presentation, isolation or second kernels — none are claimed.

## 3. Observed reduced-profile boot sequence (VM-observed, R3)

From captured serial log and source/profile (`core/main.c` with `OW_SWEEP_KERNEL`):
- HAL/boot initialization proceeds to `[STARTED] HAL Init` and kernel boot-complete milestone.
- **KDB initialized:** `[KDB] registry online:` and `[KDB] service bridge online: 4 built-in service(s)` observed (includes `main.ping`, `main.echo`, `main.log`, `main.runtime_info`).
- Reduced-profile marker observed: `sweep-kernel profile: KDB/KAPA substrate ready`.
- End state: `[SWEEP-KERNEL] Reduced profile idle (no native Sweep execution).` (scheduler/idle halt, no userspace handoff).

**Classification:** sequence **implemented in source/profile and VM-observed** for asserted milestones. This validates reduced-profile boot/init/KDB substrate readiness, **not** native Sweep KAPA CPU execution.

## 4. Excluded subsystem verification

- **Dynamic (QEMU, asserted absent):** `[VFS] OWFS: primary database mounted`, `[VFS] USFS: secure volume mounted`, `[NET] router online`, `[VIP] UniVIP online`, `[SENT] active defense armed`, `[SYSC] system call table installed`, `handing off to shell 'sh' as init process`, `[OWINIT] state `, `[OWINIT] userspace survey:`, `OpenWindows DEBUG SYSTEM CONTROL SHELL`.
- **Static/profile evidence:** excluded init/services not invoked in `OW_SWEEP_KERNEL` path; excluded object groups not linked into reduced image (see `docs/SWEEP_KERNEL_PROFILE.md`).

## 5. KDB validation status

- **Runtime/VM evidence:** KDB registry and service bridge online markers captured in reduced-kernel boot log (4 builtins). In the R5/R6 boots, KDB loads/instances/associate/bridge-execute the KAPA payloads on the CPU and the lifecycle transitions are observed end to end (`RUNNING` on dispatch, `STOPPED` on attributed exit, `FAULTED` on attributed CPL3 fault); `main.log` attribution (`assoc=1 target=0`) is visible in both R5 and R6 logs.
- **Execution:** genuine KAPA execution is implemented and demonstrated at two levels: the minimal `kapa_test.owx` self-test (R5) and the actual freestanding Sweep runtime (Duktape) executing JavaScript and computing a verified result (R6). **No Sweep browser (HTML/CSS/DOM) is executed, and no other KAPA application has been executed through this path.**

## 6. Limitations

- QEMU boot tests prove reduced-profile boot, KDB/bridge initialization milestones, and the two controlled KAPA payloads (R5 self-test and R6 runtime). They do **not** prove browser execution, arbitrary KAPA images, second kernels or hardware isolation.
- `Date.now()` is deterministic-zero inside the KAPA build; `main.runtime_info` is the KAPA's monotonic clock.
- Direct `-kernel` loading of `sweepwinkrnl.owx` remains unverified/unsupported by this harness path (historical R2 attempt had no serial output).
- No MMU/CPU/VM isolation is implemented for KAPA execution; the per-instance address space comes from PS primitives.
- Bump-arena monotonicity applies to terminated processes' physical frames and page-table pages.

## 7. Follow-up

- Extend the KAPA harness to a payload that walks further into the engine (e.g., parses a canned HTML fragment with `Sweep*CreateDocument`) to prove the browser foundation under the same path.
- Add a native resource backend for `SWEEP_RUNTIME.read_resource` so KAPA payloads can load real files under policy.
- Keep build/profile artifact checks and forbidden-marker checks in regression runs (`make qemu-sweep-kernel`, `make qemu-sweep-kapa-test CIS_DEV_TRUST=1`, `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1`, `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1`). 


## R7 KAPA evidence (2026-11-14, fresh)

Complete chain from `build-generated/sweep_kapa_dom_qemu_boot.log`:

```
[     42.673] [BOOT] kernel boot complete in 42673 ms (no shell/userspace handoff)...
[FINISHED] Boot Complete (code 0x11AC09)
[PS] boot context captured (thread 0)
[FINISHED] PS Scheduler (code 0x11AC10)
[PS] scheduler started (100 Hz)
[KDB] instance 1: app 'sweep-kapa-runtime' loaded 'sweep' (107 page(s), domain 1)
[PS] sweep: process created (PID 1)
[CIS] sweep: TRUSTED, signed by dev, licence accepted
[OWX] applied 211 base relocation(s)
[FINISHED] OWX Image Load (code 0x11AC12)
[PS] sweep image loaded: 7 section(s), 349 page(s) charged, entry 0x4000360 (bias 0xFFFFFFFEC3FFF000), exit 0x415C000
[PS] sweep: USER thread 1 created, ring3 entry 0x4000360 stack 0x41FEFF8
[PS] sweep: image entered at CPL3, rsp 0x41FEFF8, exit 0x415C000
[KDB] instance 1: backend prepared process 1 (thread 1) for 'sweep'
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
[EX] tid=1
[PS] sweep: process terminated (PID 1, exit 90)
[SWEEP-RUNTIME] selftest instance 1 finished (state=stopped fault=0x0 exit=90)
[HAL] BROADCASTING IPI_FREEZE_SMP TO ALL CORES
```

Classification: **genuine Sweep HTML parser + bounded DOM + Duktape JavaScript at CPL3.** JavaScript queried and mutated the actual DOM inside `kapa_sweep.owx`; the mutation was independently verified through the C DOM API. QEMU boot and 58 assertions pass (`make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1`). Not evidence of isolation or of a complete browser. See `docs/SWEEP_R7_REPORT.md`.
