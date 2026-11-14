# Sweep Kernel Profile (R1, R6 evidence update)

**Date:** 2026-11-13 (R1 profile; R3 boot validation; R4 verification refresh; R5 controlled KAPA; R6 freestanding-runtime KAPA)  
**Status:** Implemented and built/boot-validated as a separate reduced OpenWindows-derived kernel target/profile. The reduced profile now arms CIS and hosts **two controlled KAPA self-tests** (R5 minimal probe `kapa_test.owx`; R6 full freestanding runtime `kapa_sweep.owx`, plus the R6 fault payload). The default `sweep-kernel` boot remains the KDB-ready idle profile. **The Sweep browser engine (HTML/CSS/DOM) does not run yet** — the R6 image runs the runtime foundation (Duktape + seam + arena) only.

## 1. Baseline (current, re-verified for R6)

- Repo/branch: `C:\Users\KARIMABENDA\Documents\Sweep`, git `master`, no commits yet, working tree uncommitted.
- Full kernel build (`make -B -j 2`): **success**; `openwinkrnl.owx` 1,662,976 B, `openwinkrnl.pe` 2,824,181 B, `openwinkrnl.chk` 66 B (R6 run; SHA-256 `6fb140a25ef8aa13ecc4ad4cdd61a30772d78534381b1a813c0b644eb92071f0`).
- Reduced Sweep kernel build (`make sweep-kernel`): **success**; `sweepwinkrnl.owx` 1,531,904 B, `sweepwinkrnl.pe` 2,435,709 B, `build/sweep-kernel/` 106 `.o` files (103 base objects + `kdb_exec_ps.o`, `kdb_syscall.o`).
- Reduced-profile QEMU boot (`make qemu-sweep-kernel`): **PASS** via the real multiboot image; log `build-generated/sweep_kernel_qemu_boot.log`. Proves substrate readiness.
- KAPA self-test QEMU boot (`make qemu-sweep-kapa-test CIS_DEV_TRUST=1`): **PASS** (R5); KAPA runtime QEMU boot (`make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1`): **PASS**; KAPA fault QEMU boot (`make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1`): **PASS** (R6); logs under `build-generated/`.
- Sweep host tests (`make sweep-tests`): **1007 passed, 0 failed**.
- Kernel hosttest (`make hosttest`, this repo): **1424 passed, 0 failed**; kernel image integrity verified against the matching `.chk`; Service Bridge built-ins and the R5 executor-seam suite tested.
- Freestanding Duktape (`make sweep-freestanding`): **exit 0**, smoke `rc=0 status=ok`, arena peak 118,480 B, 0 outstanding, relocatable `nm -u` closure **OK**. Also built into `kapa_sweep.owx` (R6) and executed at CPL3.
- External sibling dependencies referenced by build/test: `../OpenWindows-Essentials`, `../superunicode`, `../vip`, `../OpenWindows-Storage`. Not vendored; roots are configurable (see §4.1).

## 2. Build profiles (actual)

| Profile | Make target | Output artifacts | Intermediate objects | Notes |
|---|---|---|---|---|
| Full OpenWindows (preserved) | `all` / default `$(TARGET)` | `openwinkrnl.owx` (1,662,976 B, R6), `openwinkrnl.pe` (2,824,181 B), `openwinkrnl.chk` (66 B), SHA-256 `6fb140a2…` | existing in-tree object rules/paths preserved | Default behavior and artifact names preserved |
| Sweep Kernel (reduced, R1) | `sweep-kernel` | `sweepwinkrnl.owx` (1,531,904 B, R6), `sweepwinkrnl.pe` (2,435,709 B), `sweepwinkrnl.chk`, SHA-256 `c310d1cf…` | `build/sweep-kernel/` (separate; no overwrite of full-target intermediates/outputs) | Distinct, independently compiled image/profile |
| Sweep KAPA self-test (R5) | `qemu-sweep-kapa-test` | `build/sweep-kapa/kapa_test.owx` + same reduced image with `-DOW_SWEEP_KAPA_SELFTEST` | `build/sweep-kapa/` | Minimal KAPA probe |
| Sweep KAPA runtime self-test (R6) | `qemu-sweep-kapa-runtime-test` | `build/sweep-kapa-runtime/kapa_sweep.owx` + same reduced image with `-DOW_SWEEP_KAPA_RUNTIME_SELFTEST` | `build/sweep-kapa-runtime/` | Actual freestanding runtime (Duktape) at CPL3 |
| Sweep KAPA fault self-test (R6) | `qemu-sweep-kapa-fault-test` | `build/sweep-kapa-fault/kapa_fault.owx` + same reduced image with `-DOW_SWEEP_KAPA_FAULT_SELFTEST` | `build/sweep-kapa-fault/` | Controlled CPL3 fault attributed to the instance |

- Profile macro: `OW_SWEEP_KERNEL` (used for profile-aware compilation in selected sources).
- Explicit object manifest: `SWEEP_KERNEL_BASE_OBJS` (101 base objects reported by `make sweep-kernel-report`) composed from selected groups; 13 object groups excluded from reduced link (`VFS_OBJS`, `NET_OBJS`, `SENT_OBJS`, `SHELL_OBJS`, `USERMODE_OBJS`, `VIP_OBJS`, `STOR_COMMON_OBJS`, `OWFS_OBJS`, `USFS_OBJS`, `STOR_CORE_OBJS`, `RC_OBJS`, `SYSCALL_OBJS`, `KAPI_OBJS`).
- Included groups (R1): `CORE_OBJS`, `HAL_OBJS`, `PS_OBJS`, `SYNC_OBJS`, `DPC_OBJS`, `KDB_OBJS`, `DIAG_OBJS`, `CIS_OBJS`, `LIB_OBJS`, `BANCODE_OBJS`, `SUC_OBJS`, `SUAS_OBJS`, `SUTS_OBJS`, `SUTR_OBJS`, `SUST_OBJS`, `SUF_OBJS`.
- Packaging: `python tools/owx_pack.py sweepwinkrnl.pe sweepwinkrnl.owx --subsystem 0x01` (CIS block: none in produced image, same as unsigned/dev packaging path used here).
- **No excluded subsystem source files deleted in R1**.

## 3. Subsystem matrix (audited, R1 implementation)

Legend: **REQUIRED** (included), **CONDITIONAL**, **EXCLUDED** (from reduced link/profile), **UNRESOLVED**.

| Subsystem | Classification (R1) | Rationale / evidence |
|---|---|---|
| `core/` (`main`, `object`, `memory`, `pgfault`, `alpc`, `runlevel`) | REQUIRED (profile-aware) | Boot/executive/memory/object/fault/runlevel. `core/main.c` gated with `OW_SWEEP_KERNEL` to skip full userspace/VFS/storage/network/sentinel/shell/syscall handoff and end in KDB-ready idle/halt. `core/runlevel.c` gated to avoid net/rc dependencies in sweep profile transitions/shutdown paths. |
| `hal/` | REQUIRED | HAL/IDT/timer/interrupts/ACPI primitives. `hal/idt.c` routes the syscall vector to the reduced `OwKdbSyscallDispatch` (restricted KAPA gateway) in the sweep profile; the full dispatcher stays the only implementation elsewhere. |
| `diag/` + `bancode/` | REQUIRED | Diagnostics/BANcode reporting used by boot/KDB. |
| `lib/` | REQUIRED | Core utilities and crypto/helpers used by CIS/loader/kernel paths. |
| `ps/` (`ps`, `switch.S`, `owx_loader`) | REQUIRED | Process/thread/scheduler and OWX loader/validation front-end relevant to KDB/image handling foundations. |
| `sync/`, `dpc/` | REQUIRED/CONDITIONAL | Executive synchronization and deferred work retained in reduced profile. |
| `kdb/` | REQUIRED | KAPA management plane (registry, compatibility, loader validation/accounting, associations, instances, Service Bridge, Sweep descriptor registration). |
| `cis/` | REQUIRED (included, armed in R5) | CIS engine links for trusted-image validation; **R5 arms it in the reduced boot before the executor runs anything**, and it is the gate every KAPA payload must pass (`[CIS] TRUSTED` in the KAPA log). A default build has zero anchors and refuses every image. |
| `syscall/` + `kapi/` | EXCLUDED (R1 profile) | The full syscall gateway/userspace ABI path is not linked; vector 0x80 reaches the restricted KAPA gateway instead (`OwKdbSyscallDispatch`). |
| `usermode/`, `shell/`, `vfs/`, `storage/` (`owdisk`/OWFS/USFS/common), `net/`, `sentinel/`, `vip/`, `rc/` | EXCLUDED (R1 profile) | Not started/required for minimal KDB-ready substrate; full paths remain in source and full target. |
| SuperUnicode `sucs/suas/suts/sutr/sust/suf` | INCLUDED (R1, conservative) | Retained to satisfy kernel-side dependencies and keep profile buildable without fine-grained per-symbol pruning; further reduction is conditional on dependency proof. |
| R7 payload artifacts | INCLUDED (R6/R7 evidence builds only, not default `sweep-kernel`) | `kapa_sweep.owx` and `kapa_fault.owx` and `kapa_test.owx` are built in their own object trees under `build/sweep-kapa*/`; only the dedicated `qemu-sweep-kapa-*-test` targets link them into the sweep-kernel substrate image. The default `sweep-kernel` boot remains the KDB-ready idle profile and carries no KAPA payload. |

## 4. Dependency graph & coupling hotspots

- `core/main.c`: central boot coupling (HAL/memory/object/diag/runlevel/PS/KDB + full userspace/storage/VFS/CIS integrity/network/vip/sentinel/syscall/shell). **R1 mitigation:** `OW_SWEEP_KERNEL` guards boot phases and helpers so reduced profile does not call excluded subsystems.
- `core/runlevel.c`: transition/shutdown hooks referenced `net`/`rc` APIs; **R1 mitigation:** sweep-profile guards in teardown/set paths.
- `hal/idt.c`: syscall interrupt vector dispatch referenced `OwSyscallDispatch`; **R1 mitigation:** sweep-profile returns `OW_ERR_UNSUPPORTED` for syscall vector.
- `kdb/*` + `ps/owx_loader.c`: KDB management depends on executive primitives and OWX validation; no real Sweep execution context.
- External sibling repos: required by existing build/test include paths (Essentials/SuperUnicode/VIP/Storage). Reduced profile build completed in current environment with these dependencies present.

### 4.1 External dependency roots and profile matrix (R4)

Roots are configurable with preserved defaults (`Makefile` and `CMakeLists.txt`):

| Variable | Default | Used for |
|---|---|---|
| `ESSENTIALS_ROOT` | `../OpenWindows-Essentials` | SUF parser/font sources/headers, owinit artifact, hosted KDB test support |
| `SUPERUNICODE_ROOT` | `../superunicode` | `sutf`, `sust`, `superunicode_extended`, `suas/suts/sutr` sources/headers |
| `VIP_ROOT` | `../vip` | `univip_fvip` (full profile/hosttest) |
| `STORAGE_ROOT` | `../OpenWindows-Storage` | `common`/`owfs`/`usfs` (full profile/hosttest) |

`make check-external-deps EXTERNAL_DEPS_PROFILE=<profile>` checks key files and fails with the exact missing path. It is standalone, so default target behavior is unchanged.

| Profile | Required roots (key files) | Key files checked | Notes |
|---|---|---|---|
| `full` (default) | Essentials + SuperUnicode + VIP + Storage | 18 | Full image and `hosttest` |
| `sweep-kernel` | Essentials + SuperUnicode | 10 | Reduced image; VIP/Storage heads are not compiled |
| `sweep-tests` | Essentials | 3 | SUF parser used by document/render tests |
| `freestanding` | none | 0 | Vendored Duktape + local shims only |

Validated R4: all four profiles report OK with defaults; explicit override of all roots reports OK; a bogus `VIP_ROOT` fails with `MISSING ../vip-does-not-exist/univip_fvip.h` before compilation. CMake exposes the same roots as cache `PATH` variables.


## 5. Boot architecture (implemented in sweep profile)

- Preserved/initialized: HAL, ACPI, memory manager, object manager, diagnostics/runlevel, PS initialization, **KDB initialization** (with explicit success/warn reporting), **CIS initialization (R5: armed in the reduced profile, security-critical, must be live before the executor maps any KAPA image)**, boot-complete reporting.
- Omitted in sweep profile: VFS mount, storage/OWFS-USFS bring-up, userspace survey/owinit handoff, boot-time CIS/integrity enforcement tied to the full userspace model (the CIS *engine* itself is armed - it validates executables; the full-profile enforcement *phase* integration for userspace admission remains absent), network, UniVIP, sentinel, syscall gateway (except vector 0x80 routed to the reduced `OwKdbSyscallDispatch`), shell, RC shutdown sequence/userspace launch.
- End state (default boot): scheduler capture/start and idle halt loop (`[SWEEP-KERNEL] Reduced profile idle (no native Sweep execution).`).
- End state (R6 `OW_SWEEP_KAPA_RUNTIME_SELFTEST` build): KDB loads, trust-gates, dispatches, and retires the **actual freestanding Sweep runtime** (`kapa_sweep.owx` — Duktape executing verified JavaScript) through the PS executor; the instance is observed `RUNNING` on dispatch and `STOPPED` on its attributed exit (`exit=90`).
- End state (R6 `OW_SWEEP_KAPA_FAULT_SELFTEST` build): the same path ends in an attributed CPL3 fault (`state=faulted fault=0x1`), proving instance-attributed fault capture in QEMU.
- **Image format note (R6):** OWX1 accepted a `.reloc`-bearing image through a new `OWX_SECTION_RELOC` (0x05) fix-up pass in `OwPsLoadImage`; images without it are byte-identical to before.

## 6. Build & validation (R1/R3/R4/R5/R6)

| Item | Result |
|---|---|
| Reduced Sweep kernel build (R6) | **Success** (`make sweep-kernel`). `sweepwinkrnl.owx` 1,531,904 B, `sweepwinkrnl.pe` 2,435,709 B, `build/sweep-kernel/` 106 compiled `.o` files, SHA-256 `c310d1cf…`. |
| Full OpenWindows kernel build (R6) | **Success** (`make -B -j 2`). `openwinkrnl.owx` 1,662,976 B, `openwinkrnl.pe` 2,824,181 B, `openwinkrnl.chk` 66 B, SHA-256 `6fb140a2…`. |
| Sweep host tests (R6) | **1007 passed, 0 failed**. |
| Kernel hosttest (R6) | **1424 passed, 0 failed**, including Service Bridge tests and the R5 executor-seam suite (unregistration, stub success/failure, rollback). |
| Reduced-profile QEMU boot (R6 re-run) | **PASS**; log `build-generated/sweep_kernel_qemu_boot.log`. |
| KAPA self-test QEMU boot (R5) | **PASS**; `tools/qemu_sweep_kapa_test.ps1` asserts the full controlled-execution chain and absence of excluded-subsystem/refusal/fault markers. `make qemu-sweep-kapa-test CIS_DEV_TRUST=1`. |
| KAPA runtime QEMU boot (R6) | **PASS**; `tools/qemu_sweep_kapa_runtime_test.ps1` asserts the whole JS-execution chain (KDB load, CIS trust, CPL3 entry, dispatch-attributed `RUNNING`, runtime init, `script 1 ok result=45`, `script 2 ok (duktape smoke)`, `shutdown ok`, `exit 90`, `STOPPED`) and absence of excluded-subsystem/refusal/fault markers. `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1`. |
| KAPA fault QEMU boot (R6) | **PASS**; `tools/qemu_sweep_kapa_fault_test.ps1` asserts CPL3 fault attribution (attributed bridge call right up to the fault, `[PF] NOT-MAPPED`, `faulted (0x1)`, `state=faulted fault=0x1 exit=0`). `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1`. |
| Freestanding Duktape (R6) | **Exit 0**: smoke `rc=0 status=ok`, arena peak 118,480 B, 0 outstanding, `nm -u` closure OK. Also built into `kapa_sweep.owx`, which executes it at CPL3. |
| OWX relocation pass (R6) | **PASS**: images with `OWX_SECTION_RELOC` get their DIR64 slots rebased after CIS authentication, before any code runs; images without one are byte-identical to before. |

## 7. Limitations

- No Sweep browser (HTML/CSS/DOM) is packaged as an OWX KAPA application; `kapa_sweep.owx` is a JS-executing runtime foundation, not the product.
- Sweep browser/JS modules are host-tested; the freestanding Duktape object build is linked into `kapa_sweep.owx` and executes at CPL3.
- No hardware/MMU isolation, no second-kernel execution, no Linux/custom adapters; "trusted" is a CIS verdict, not a hardware boundary.
- No physical presentation/compositor/window API for arbitrary surfaces.
- Reduced profile excludes syscall/userspace/storage/VFS/network/sentinel/shell services; Sweep-specific KAPA runtime service contracts/execution ABI not implemented (a minimal 4-service gateway plus PS exit/PID is all the KAPA can reach in R5/R6).
- External sibling dependencies remain required/documented, not removed; roots are configurable and checked with `check-external-deps`.
- Bump-arena monotonicity: frames and private page-table pages of terminated processes are not recycled (VAD nodes are); KAPA churn is bounded by explicit limits, not by a free-list allocator.
- PIE + DIR64-only relocations are required for images with absolute tables; other PE relocation types are hard `OW_ERR_UNSUPPORTED`.
- `Date.now()` is deterministic-zero in the KAPA build.

## 8. Recommended next steps

- Package the browser core (HTML parser + DOM) as a section of the Sweep KAPA image and execute a canned fragment at CPL3; promote B7 closure from "mechanism proven" to "engine proven".
- Native resource backend for `SWEEP_RUNTIME.read_resource` rooted at a resource-scoped OWFS volume.
- Timers/watchdog for script deadlines behind per-KAPA capability, so JS execution budgets are real.
- Extend the relocation pass beyond DIR64 only if a real producer needs it.
- Frame-run reclamation via a free-list allocator grafted onto the existing frame-run primitives.
- Keep build/profile artifact checks and forbidden-marker checks in regression runs (`make qemu-sweep-kernel`, all four `qemu-sweep-kapa-*-test` targets).

