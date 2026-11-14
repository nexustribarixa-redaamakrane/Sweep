# Sweep R4 Report — Runtime Seam, Freestanding Duktape, Verification

**Date:** 2026-11-11  
**Repository:** `C:\Users\KARIMABENDA\Documents\Sweep` (git `master`, no commits; working tree uncommitted)  
**Scope:** R4 continuation of the OpenWindows-derived reduced kernel profile that is intended to host Sweep as a KAPA application.

**Bottom line:** R4 added an explicit injectable runtime seam for Sweep, a genuinely freestanding (zero hosted symbols) Duktape object build, profile-aware external dependency checking, and a full clean re-verification of every build/test/boot baseline. **Sweep still does not execute natively on the kernel/KAPA path.** There is no runnable instance context, entry ABI, scheduler binding, instance-attributed fault/exit path, or registered execution backend. That remains the central open blocker and is documented as such everywhere.

---

## 1. Exact commands and outcomes (fresh R4 runs)

All commands were run from `C:\Users\KARIMABENDA\Documents\Sweep` on 2026-11-11. Logs are under `build-generated/`.

| # | Command | Exit | Result | Log |
|---|---|---|---|---|
| 1 | `make -B -j 2` | 0 | Full OpenWindows kernel image built and packed | `build-generated/build_full_r4.log` |
| 2 | `make -B sweep-kernel` | 0 | Reduced Sweep kernel image built and packed | `build-generated/sweep_kernel_build_r4.log` |
| 3 | `make -B sweep-tests` | 0 | **1007 passed, 0 failed** across 11 suites | `build-generated/sweep_tests_r4.log` |
| 4 | `make -B hosttest` | 0 | **1392 passed, 0 failed** (includes Service Bridge + kernel integrity) | `build-generated/hosttest_r4.log` |
| 5 | `make qemu-sweep-kernel` | 0 | **PASS** — real multiboot QEMU boot with all assertions | `build-generated/sweep_kernel_qemu_boot.log` |
| 6 | `make -B sweep-freestanding` | 0 | Smoke `rc=0 status=ok`; arena peak 118,480 B; 0 bytes outstanding; relocatable symbol closure OK | `build-generated/freestanding_build.log` |
| 7 | `make check-external-deps` (profiles `full`, `sweep-kernel`, `sweep-tests`, `freestanding`) | 0 | All profile key-file checks OK with default roots | console |
| 8 | `make check-external-deps VIP_ROOT=../vip-does-not-exist` | 2 (expected) | Fails clearly with `MISSING ../vip-does-not-exist/univip_fvip.h` | console |

### Suite breakdown for `make sweep-tests` (1007/0)

| Suite | Passed | Failed |
|---|---|---|
| engine | 24 | 0 |
| services | 18 | 0 |
| text | 37 | 0 |
| browser | 39 | 0 |
| HTML | 40 | 0 |
| CSS | 208 | 0 |
| JS | 187 | 0 |
| document | 258 | 0 |
| resource | 15 | 0 |
| runtime seam (new R4) | 60 | 0 |
| render | 121 | 0 |
| **Total** | **1007** | **0** |

### QEMU boot assertions (all PASS)

From `build-generated/sweep_kernel_qemu_boot.log` (3,294 B):

- `[STARTED] HAL Init`, `[KDB] registry online:`, `[KDB] service bridge online: 4 built-in service(s)`
- `[BOOT] sweep-kernel profile: KDB/KAPA substrate ready`
- `[BOOT] kernel boot complete in ...`
- `[SWEEP-KERNEL] Reduced profile idle (no native Sweep execution).`
- Absent as required: VFS/OWFS mount, USFS mount, NET router, VIP online, sentinel active, syscall table, shell handoff, owinit handoff/userspace survey, debug shell banner.

This proves reduced-profile boot/init and KDB/KAPA substrate readiness. It does **not** prove KAPA application execution.

---

## 2. Artifacts (post-R4, exact)

| Artifact | Size (bytes) | Last write (local) | SHA-256 |
|---|---|---|---|
| `openwinkrnl.owx` (full) | 1,646,592 | 2026-11-11 20:43:42 | `acf57def798508bc6819033f8b7c9568d9d7bb0c2de6390edc12d1b71fae01cb` |
| `openwinkrnl.pe` (full) | 2,789,018 | 2026-11-11 20:43:40 | — |
| `openwinkrnl.chk` (full) | 66 | 2026-11-11 20:43:42 | matches the `.owx` hash above |
| `sweepwinkrnl.owx` (reduced) | 1,515,520 | 2026-11-11 20:45:03 | `4b9e95dd3675a05bb6c5c019aa3c11dce560afa84b7d4a3b7ec8b50999dcad9f` |
| `sweepwinkrnl.pe` (reduced) | 2,399,486 | 2026-11-11 20:45:02 | — |
| `sweepwinkrnl.chk` (reduced) | 66 | 2026-11-11 20:45:03 | matches the reduced `.owx` hash above |
| `build/sweep-kernel/` | 104 `.o` files | — | 101 base objects in the profile manifest |

Both `.chk` files were verified against their images (`checksum verified` in the packer output; hosttest independently verifies kernel integrity against `openwinkrnl.chk`).

---

## 3. What R4 implemented

### 3.1 Injectable Sweep runtime seam (`SWEEP_RUNTIME`)

**Files:** `sweep/sweep_runtime.h`, `sweep/sweep_runtime.c`, `sweep/sweep_runtime_host.c`; integrated into `sweep/sweep_resource.h/.c`.

- Engine/runtime code now performs allocation, deallocation, time reads, diagnostics, and resource byte reads through an explicit `SWEEP_RUNTIME` vtable instead of calling hosted libc/stdio directly.
- Mandatory ops: `alloc`, `free`, `monotonic_ms`. Optional ops: `realloc`, `diag`, `read_resource`, `init`, `shutdown`. A missing optional op fails deterministically (NULL/false/0); there is no silent libc fallback.
- Documented semantics include `realloc(NULL,n)` = `alloc(n)`, `realloc(p,0)` frees and returns NULL, and the rule that a runtime without `realloc` fails **without freeing the caller's block**.
- `sweep_runtime_read_resource` requires `out_cap >= 2`, rejects `produced >= out_cap`, and NUL-terminates.
- `sweep_resource.c` routes all byte reads through `sweep_runtime_read_resource`. The legacy convenience API uses the hosted backend; the explicit API is `sweep_resource_fetch_runtime`.
- Hosted behavior is preserved through `sweep_runtime_host_ops()`; a kernel/KAPA backend is not implemented.

**Tests:** `sweep/tests/sweep_runtime_test.c` — 60 passed / 0 failed (binding validation, missing mandatory/optional ops, alloc/realloc/free routing and edge cases, bounded reads/overflow/backend contract violations, deadlines, host backend file reads, resource fetch through an injected runtime).

### 3.2 Freestanding Duktape (object level)

**Location:** `sweep/freestanding/` (vendored `sweep/third_party/duktape` is unmodified).

- `duk_config_freestanding.h` is force-included before `duktape.h`/`duktape.c`, reuses the vendored generated config, then removes Windows date/time providers and `DUK_USE_PROVIDE_DEFAULT_ALLOC_FUNCTIONS`, and installs Sweep hooks (date, fatal, `__builtin_setjmp/longjmp`).
- `sweep_freestanding_libc.c` supplies memory/string primitives, math routines, a bounded printf/scanf subset, and the Duktape hooks.
- `sweep_fs_arena.c` is a bounded first-fit arena implementing the runtime alloc/realloc/free contract; Duktape is created with `duk_create_heap` over `SWEEP_RUNTIME` (never the default allocator entry point).
- `duktape_freestanding_smoke.c` exercises arrays, objects, JSON, regexp, exceptions, number formatting, and `Date.now`.
- `make sweep-freestanding` compiles with `-ffreestanding -nostdlib -fno-builtin -mno-stack-arg-probe`, runs the smoke test via a host harness, then relocates the objects with `gcc -r -nostdlib` and requires `nm -u` to be empty.

**Result:** exit 0; smoke `rc=0 status=ok`; arena high-water mark 118,480 B; 0 bytes outstanding; **freestanding symbol closure: OK**.

**Precision/caveats:**
- This is an **object-level build and host-harness smoke test**. Duktape is **not** linked into `sweepwinkrnl`/`openwinkrnl`, **not** executed in QEMU, and **not** executed through KDB/KAPA.
- `sweep_fs_date_now_ms()` returns 0 (deterministic; no wall clock); the fatal handler traps; there is no file/stdio provider.
- Math/printf shims are minimal approximations, not a correctly-rounded libm or full CRT.

### 3.3 Profile-aware external dependency checking

**Files:** `Makefile`, `CMakeLists.txt`.

- `ESSENTIALS_ROOT`, `SUPERUNICODE_ROOT`, `VIP_ROOT`, `STORAGE_ROOT` are configurable cache/default variables with the original sibling-checkout defaults preserved.
- `make check-external-deps EXTERNAL_DEPS_PROFILE=<full|sweep-kernel|sweep-tests|freestanding>` checks a small key-file set per profile and reports the exact missing path. It is a standalone target, not a prerequisite, so existing target behavior is unchanged.
- `.DEFAULT_GOAL := all` was added because the new target was defined before `all` and had accidentally become the default goal.
- Hardcoded Sweep-test and QEMU-entry include/source roots were replaced with `$(SUF_DIR)`/`$(SWEEP_KERNEL_CFLAGS)`.

### 3.4 Service Bridge MVP (R3, re-verified R4)

**Files:** `kdb/kdb_bridge.c`, `kdb/kdb_internal.h`, `inc/ow_kdb.h`, `tools/hosttest/host_boot.c`.

- `main.ping` and `main.echo` preserved.
- Added `main.log` (`0x4B440003`, bounded attributed log; request bytes are emitted as data, never interpreted as a format string; requires `OW_KDB_CAP_CALL`).
- Added `main.runtime_info` (`0x4B440004`, 8-byte little-endian `uint64_t` monotonic uptime in ms from `OwHalUptimeMs()`; not wall-clock time; requires `OW_KDB_CAP_CALL`).
- Per-call attribution via `OwKdbBridgeGetCallAttr`.
- Hosttest coverage: empty/normal/maximum/oversized requests, missing capability, unknown caller/target, repeated calls, no format interpretation, attribution cleanup on success and failure, handler failure/overrun propagation, insufficient response capacity, non-empty request refusal.

---

## 4. Classifications

### Implemented and tested
- Reduced `sweep-kernel` profile build and packaging (`make sweep-kernel`).
- Full OpenWindows target preserved and rebuilt (`make -j 2`).
- Reduced-profile QEMU boot validation with real multiboot image and assertion script.
- Service Bridge MVP built-ins `main.log` and `main.runtime_info`, plus preserved `main.ping`/`main.echo`, with hosttest coverage.
- Injectable Sweep runtime seam (`SWEEP_RUNTIME`) and its 60-test suite.
- Runtime-mediated bounded resource reads.
- Freestanding Duktape object build, smoke test, and hosted-symbol closure check.
- Profile-aware external dependency checking for four profiles.
- Full regression verification: 1007/0 Sweep tests, 1392/0 kernel hosttest.

### Implemented, not executed end-to-end
- Freestanding Duktape: builds and links standalone, passes its host-harness smoke test, but is not linked into or executed by the kernel/QEMU/KAPA.
- Reduced kernel boot: validated as a booting substrate, not as a host of a running Sweep application.

### Partial
- Service Bridge: a real bounded capability-checked call plane exists, but there is no Sweep-specific runtime service ABI (kernel-backed allocator/time/resource/presentation services).
- Runtime seam: host and arena backends exist; no kernel-backed backend.
- Isolation: logical/resource accounting only; no MMU/CPU/VM isolation claim.
- Presentation: host-simulated only; no physical framebuffer/compositor/window API.

### Investigated, not implemented
- Genuine KAPA CPU execution for Sweep. Source-traced PS/loader/scheduler/KDB seam; documented as Path B (management plane/executor seam exists, no default backend, no runnable context, no control transfer, no scheduler integration, no instance-attributed execution fault/exit).
- `OwKdbExecutionAvailable()` is false; `OwKdbExecuteInstance()` returns `OW_ERR_UNSUPPORTED`. Logical KDB `RUNNING` is metadata, not CPU execution.

### Blocked (source-backed blockers B1–B5, B8 in `docs/SWEEP_KAPA_EXECUTION.md`)
- B1 runnable instance address space / image mapping.
- B2 entry ABI + stack + control transfer.
- B3 scheduler integration for a real `RUNNING` state.
- B4 instance-attributed execution faults/page faults.
- B5 return/exit path and deterministic cleanup.
- B8 enforced isolation/trust model for executing code.

### Planned for R5
- Close B1–B5/B8 with a single, minimal, safely scoped execution path.
- Add kernel-backed Sweep runtime ops/services (alloc/time/resource-read, then input/presentation handoff) with capability checks.
- Package the freestanding runtime as a validated OWX/KAPA app image and drive it through the execution path.
- Further prune only with dependency proof; preserve full OpenWindows behavior.

---

## 5. Modified / added files in R4

Build and configuration:
- `Makefile` — profile targets, freestanding target, configurable dependency roots, `check-external-deps`, `.DEFAULT_GOAL := all`, `$(SUF_DIR)`/`$(SWEEP_KERNEL_CFLAGS)` cleanup.
- `CMakeLists.txt` — configurable `SUPERUNICODE_ROOT`, `ESSENTIALS_ROOT`, `VIP_ROOT`, `STORAGE_ROOT` cache variables.

Sweep runtime:
- `sweep/sweep_runtime.h` (new)
- `sweep/sweep_runtime.c` (new)
- `sweep/sweep_runtime_host.c` (new)
- `sweep/sweep_resource.h`, `sweep/sweep_resource.c` (runtime-mediated)
- `sweep/tests/sweep_runtime_test.c` (new)

Freestanding Duktape:
- `sweep/freestanding/duk_config_freestanding.h` (new)
- `sweep/freestanding/sweep_freestanding_libc.h` (new)
- `sweep/freestanding/sweep_freestanding_libc.c` (new)
- `sweep/freestanding/sweep_fs_arena.h` (new)
- `sweep/freestanding/sweep_fs_arena.c` (new)
- `sweep/freestanding/duktape_freestanding_smoke.c` (new)
- `sweep/freestanding/freestanding_host_main.c` (new)

Kernel / tests (R3 work re-verified in R4):
- `kdb/kdb_bridge.c`, `kdb/kdb_internal.h`, `inc/ow_kdb.h`
- `tools/hosttest/host_boot.c`
- `tools/qemu_sweep_kernel_test.ps1`

Documentation (new/updated):
- `docs/SWEEP_R4_REPORT.md` (this file, new)
- `docs/SWEEP_RUNTIME_PORTABILITY.md`
- `docs/SWEEP_KERNEL_PROFILE.md`
- `docs/SWEEP_KAPA_EXECUTION.md`
- `docs/SWEEP_KAPA_INTEGRATION.md`
- `docs/SWEEP_SERVICE_BRIDGE.md`
- `docs/SWEEP_JS.md`
- `SWEEP.md`

`docs/SWEEP_R3_CONTINUATION_REPORT.md` is retained for historical context; its statements that hosttest was incomplete and freestanding Duktape was blocked are **superseded** by this report.

---

## 6. Remaining limitations (explicit)

- **No genuine Sweep KAPA execution.** No runnable instance context, entry ABI/stack binding, scheduler-dispatched execution, instance-attributed execution fault handling, or exit path. No execution backend is registered.
- **Freestanding Duktape is object-level only.** It is not in the kernel image and is not executed by QEMU or KAPA; math/printf shims are approximations and the date provider is deterministic-zero.
- **Reduced boot proves substrate readiness only.** It does not execute a KAPA payload.
- **No physical presentation.** No framebuffer/compositor/window API; presentation remains host-simulated.
- **No MMU/CPU/VM isolation** for executing code. Do not claim otherwise.
- **External sibling repositories are required** (Essentials, SuperUnicode, VIP, Storage). They are not vendored; their roots are configurable and checked.
- **No sibling-repo modifications.** `C:\Users\KARIMABENDA\Documents\OpenWindows` was not modified; no destructive source deletion was performed in this repo.
- The working tree is uncommitted (all files show as untracked).
