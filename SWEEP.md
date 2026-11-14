# Sweep - OpenWindows-derived associated kernel (KAPA reference application)

Sweep is the primary reference associated-kernel/application target of KDB/KAPA: an OpenWindows-derived reduced kernel profile intended to host Sweep as a KAPA application (browser runtime) through controlled services. This tree was copied from the OpenWindows kernel source and is being reduced/integrated incrementally.

**Status (honest, 2026-11-14, R7):**
- **Native HTML+DOM execution inside the freestanding KAPA runtime implemented and QEMU-demonstrated.** A canned HTML fixture is parsed by Sweep's real HTML parser, built into Sweep's real bounded DOM, queried and mutated by JavaScript executing at CPL3, and the mutation is independently verified through the C DOM API. See `docs/SWEEP_R7_REPORT.md` and `build-generated/sweep_kapa_dom_qemu_boot.log`. `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1` asserts the whole chain (58 assertions).
- **Native KAPA execution of the real freestanding Sweep runtime (R6):** `kapa_sweep.owx` — CIS-gated, PIE, 211 base relocations; Duktape evaluates verified JavaScript at CPL3; arena drains to zero; attributed `main.log`/`main.runtime_info`. `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` passes.
- **Controlled KAPA fault attribution (R6):** independent payload `kapa_fault.owx` — CPL3 fault attributed to its instance, FAULTED transition observed. `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` passes.
- **Host-tested Sweep browser/runtime foundations:** `make sweep-tests` **1007 passed, 0 failed**; JS/CSS/DOM editor suite all green.
- **Kernel profiles (re-verified):** full `make -B -j 2` **success** (`openwinkrnl.owx` 1,662,976 B, sha256 `6fb140a2…`); reduced `make sweep-kernel` **success** (`sweepwinkrnl.owx` 1,531,904 B, sha256 `7c699ddc…`, 106 `.o`).
- **Reduced-kernel boot validation:** `make qemu-sweep-kernel` **PASS**.
- **Service Bridge:** builtins `main.ping`/`main.echo`/`main.log`/`main.runtime_info`; `make hosttest` **1424 passed, 0 failed**. The KAPA runtime calls `main.log`/`main.runtime_info` from CPL3 with attribution.
- **Runtime seam + freestanding Duktape (R4/R6):** `SWEEP_RUNTIME` seam; `make sweep-freestanding` **exit 0** with `nm -u` closure OK. R6 links those exact objects into `kapa_sweep.owx`; R7 adds the HTML parser and DOM to it.
- **R7 artifacts:** `kapa_sweep.owx` 434,410 B (entry RVA `0x1360`, console subsystem, sha256 `1d99971b…`); `kapa_fault.owx` 8,426 B.
- **OWX relocation fix:** the loader applies PE DIR64 relocations for images carrying an `OWX_SECTION_RELOC` section (`type=0x05`); images without one take the unchanged path; CIS authentication order unchanged.
- **External deps:** configurable roots, all four profiles **OK**; bogus root fails clearly.
- **Not implemented/claimed:** Sweep browser runtime (CSS/layout/render) as an OWX image; native OWFS resource service (`read_resource`); JS execution watchdog (interrupt counter exists but the KAPA has no advancing clock during JS — see `docs/SWEEP_R7_REPORT.md` §10);MMU/CPU/VM isolation; second kernel; arbitrary third parties. No sibling-repo modifications, no destructive source deletion.

## R7 implementation status and documentation

- `docs/SWEEP_R7_REPORT.md` - **R7 evidence: real Sweep HTML parser + DOM + Duktape JavaScript at CPL3** — canned fixture parsed, JS queries/mutates the actual DOM, verified independently in C, QEMU-demonstrated (58 assertions); OWFS/watchdog/reclamation investigations (source-backed), artifact hashes.
- `docs/SWEEP_R6_REPORT.md` - R6 evidence: the real freestanding Sweep runtime executes JavaScript at CPL3; OWX relocation fix, KAPA startup ABI, service backend, fault attribution.
- `docs/SWEEP_R5_REPORT.md` - R5 evidence: controlled KAPA execution (minimal payload, blocker closure B1-B5/B8).
- `docs/SWEEP_R4_REPORT.md` - R4 classifications (historical baseline).
- `docs/SWEEP_R3_CONTINUATION_REPORT.md` - prior continuation evidence (historical).
- `docs/SWEEP_KAPA_EXECUTION.md` - audit, feasibility, blocker table (R5/R6/R7 annotations).
- `docs/SWEEP_SERVICE_BRIDGE.md` - bridge/builtins + reduced-profile gateway + R6 usage from CPL3.
- `docs/SWEEP_BOOT_VALIDATION.md` - QEMU harness; R3/R5/R6/R7 boot evidence.
- `docs/SWEEP_RUNTIME_PORTABILITY.md` - runtime seam, freestanding Duktape, R6/R7 packaging.
- `docs/SWEEP_KERNEL_PROFILE.md` - reduced profile build/boot status and dependency matrix.
- `docs/SWEEP_KAPA_INTEGRATION.md` - integration status (R6 runtime execution demonstrated; R7 DOM demonstrated).
- `docs/KDB-KAPA.md` - in-tree KDB/KAPA scope and limitations.

## Build/test baselines (R7)

| Item | Result |
|---|---|
| `make -B -j 2` (full kernel) | **Success** |
| `make sweep-kernel` | **Success** |
| `make sweep-tests` | **1007 passed, 0 failed** |
| `make hosttest` (this repo) | **1424 passed, 0 failed** |
| `make qemu-sweep-kernel` | **PASSED** (`build-generated/sweep_kernel_qemu_boot.log`) |
| `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | **PASSED** (`build-generated/sweep_kapa_qemu_boot.log`) |
| `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` | **PASSED** (`build-generated/sweep_kapa_runtime_qemu_boot.log`) |
| `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` | **PASSED** (`build-generated/sweep_kapa_fault_qemu_boot.log`) |
| `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1` | **PASSED** (`build-generated/sweep_kapa_dom_qemu_boot.log`, 58 assertions) |
| `make sweep-freestanding` | **Exit 0**, smoke ok, symbol closure OK |
| `make check-external-deps` (all profiles) | **OK** with default roots; missing root fails clearly |

## Known limitations

- No Sweep browser (CSS/layout/render/navigation) is packaged as an OWX KAPA application; the R7 `kapa_sweep.owx` carries the runtime foundation (Duktape + `SWEEP_RUNTIME` + arena) and the bare HTML parser + DOM, and executes a defined JS snippet against a real fixture — not the browser.
- No MMU/CPU/VM isolation anywhere in this profile; "trusted" means a CIS verdict, never a hardware boundary.
- Bump-arena monotonicity: frames and private page-table pages of terminated processes are not recycled (VAD nodes are recycled; frame-run commit charge is released); KAPA churn is bounded by explicit limits, not by a free-list allocator.
- No real presentation/display API; Sweep-specific runtime services beyond `main.log`/`main.runtime_info` are not wired; `read_resource` has no KAPA backend (OWFS excluded from the reduced profile by design — see report §9); no timers; no second kernel; arbitrary third-party KAPAs remain out of scope.
- `Date.now()` is deterministic-zero in the KAPA build (libc shim). No JS execution budget enforcement while a script runs away inside the KAPA (investigation in report §10; R8 proposal included).
- External sibling dependencies required (roots configurable/checked), not vendored. 
