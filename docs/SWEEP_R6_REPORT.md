# Sweep R6 Report - Freestanding Runtime Packaging, Native KAPA Integration, and Runtime Execution Validation

**Date:** 2026-11-13  
**Status:** **Complete.** The actual freestanding Sweep runtime (Duktape 2.7.0 over `SWEEP_RUNTIME`, unmodified) is packaged as a genuine OWX KAPA application, CIS-signed, and executed at CPL3 through the R5-proven path: real JavaScript compiles, evaluates, returns a verified result, the heap is destroyed and the arena is drained to zero bytes. A second, independent controlled-fault payload proves CPL3 fault attribution to its KDB instance.

## 1. R5 baseline verification (reproduced 2026-11-13)

| Check | R5 recorded | Reproduced |
|---|---|---|
| `make -B -j 2` (full kernel) | Success | **Success** (`openwinkrnl.owx` 1,662,976 B, SHA-256 `6fb140a25ef8aa13ecc4ad4cdd61a30772d78534381b1a813c0b644eb92071f0`) |
| `make sweep-kernel` | Success | **Success** (`sweepwinkrnl.owx` 1,531,904 B, SHA-256 `c310d1cfe68ad3c82163e93dc1d0848d1446065c19ec9c504364540dcf9517f4`) |
| `make sweep-tests` | 1007 / 0 | **1007 passed, 0 failed** |
| `make hosttest` | 1424 / 0 | **1424 passed, 0 failed** |
| `make sweep-freestanding` | exit 0 | **Exit 0**, `nm -u` closure OK |
| `make check-external-deps` (4 profiles) | PASS | **All OK** (bogus profile fails as before) |
| `make qemu-sweep-kernel` | PASS | **PASS** |
| `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | PASS | **PASS** |

`openwinkrnl.owx` / `sweepwinkrnl.owx` and their `.chk` were rebuilt in R6 (the loader gained the relocation pass; the sweep profile's user stack is now 16 pages). Their hashes above are fresh.

## 2. Runtime source inventory (Workstream A)

| Component | Files | External deps | Notes |
|---|---|---|---|
| Sweep runtime seam | `sweep/sweep_runtime.{h,c}` | none | Injected `SWEEP_RUNTIME` + `SWEEP_RUNTIME_OPS`; mandatory alloc/free/monotonic_ms; read_resource intentionally absent in R6 |
| Freestanding libc shim | `sweep/freestanding/sweep_freestanding_libc.{c,h}` | none | memcpy/memset/math/printf subset; `sweep_fs_date_now_ms()` deterministic 0 (documented, unchanged) |
| Arena allocator | `sweep/freestanding/sweep_fs_arena.{c,h}` | none | First-fit arena; coalescing; GSI: `sweep_fs_arena_init/alloc/free/realloc` |
| Duktape (vendored, unmodified) | `sweep/third_party/duktape/duktape.c` | none beyond shim; compiled `-ffreestanding -nostdlib` with `duk_config_freestanding.h` | Hooks: `DUK_USE_DATE_GET_NOW`, `DUK_USE_FATAL_HANDLER` |
| Duktape freestanding config override | `sweep/freestanding/duk_config_freestanding.h` | none | Removes Windows date/time providers, default malloc; keeps semantics, changes no JS behavior |
| Freestanding smoke test (host) | `sweep/freestanding/duktape_freestanding_smoke.c`, `freestanding_host_main.c` | hosted libc (host only) | Proves the same object set the KAPA links |
| Host runtime backend | `sweep/sweep_runtime_host.c` | libc | Hosted only; never linked into the KAPA image |

ABI assumptions carried into the KAPA: Microsoft x64 `.owx` entry contract (RCX=argc, RDX=argv, R8=envp), RIP-relative code after the fix in §5, PE base relocations honoured by the OWX loader (§5).

## 3. Workstream B - KAPA runtime startup contract

**Entry:** `kapa_sweep_main(int argc, char** argv, char** envp)` at image RVA `0x1360` (MS x64 ABI, exactly like every `.owx` image; reached only through the R5 ring-3 iretq path). **Registers:** RCX/RDX/R8 as above. **Stack:** top-down from `OW_USER_STACK_TOP` (16 pages in the sweep profile, eagerly mapped), guard band below. **Image metadata:** via `kapa_test`-equivalent launch slot. **Services:** two only - `main.log` and `main.runtime_info` (8-byte LE uptime). **Exit:** `OW_GATE_PS_EXIT` syscall with a defined status; KDB lifecycle records it.

Failure handling (defined exit codes, all observed in QEMU before ever reaching them): `0xE0` bad startup args · `0xE1` arena init · `0xE2` runtime bind · `0xE3` Duktape heap create · `0xE4` script 1 eval/type/value · `0xE5` script 2 (Duktape smoke) · `0xE6` arena not drained · `0x5A` (90) success.

Initialization order is audited: startup args → arena (192 KiB static `.bss`) → runtime binding (runtime-populated ops table) → Duktape heap → scripts → heap destroy → arena-drain check → runtime shutdown → defined exit. Every failure path releases the process correctly and leaves the KDB instance attributed (`exit=1` observed for the R6 fault-debug runs before the loader fix).

## 4. Workstream C - Freestanding service backend (`kapa_sweep.c`)

The runtime client uses exactly two services through the restricted R5 gateway (`OW_SYS_KDB_BRIDGE` → `OW_KDB_SERVICE_LOG` / `OW_KDB_SERVICE_RUNTIME_INFO`), both capability-checked (`OW_KDB_CAP_CALL`), both bounded, both attributed: QEMU evidence shows `[KDB][LOG] assoc=1 target=0 len=...` for every runtime diagnostic line. Message bytes are data; no format directives are interpreted. The diagnostic path never passes KAPA-controlled strings as format strings, and every script result is verified before any exit. No new kernel services were needed or added.

## 5. Workstream D - `kapa_sweep.owx` packaging and the relocation fix

Image (CIS-signed for the QEMU evidence path only; the default remains unsigned and refused):

- **Size:** 405,738 B OWX (PE 512,137 B) · **SHA-256:** `4a7bfded2fa493ec38b3559e2ae6905330f7e6de75c6731986012f726813de8f` · matches `.chk`
- **Entry:** RVA `0x1360` → CPL3 `0x4000360` (bias `0xFFFFFFFEC3FFF000`) · **Subsystem:** 0x03 (console)
- **Sections:** 0x01 CODE (0x5AC00) · 0x03 DATA · 0x02 RDATA ×3 · 0x04 BSS (0x30040, arena + runtime state) · **0x05 RELOC** (0x200 B, PE `.reloc`)
- **Imports:** none · **Relocation fix:** 211 DIR64 slots rebased at load time (`[OWX] applied 211 base relocation(s)`)

**Why the loader had to change.** Hand-written OpenWindows images (owrs, owinitv, kapa_test) are RIP-relative end to end, which is what the OWX1 loader was built for. The freestanding runtime contains ~211 absolute 64-bit pointers in its read-only data (Duktape's dispatch tables plus the ops table). The image cannot run if those are not rebased. The OWX format already defined `OWX_SECTION_RELOC` (0x05); the PE packer now emits `.reloc` as that section instead of silently flattening it into RDATA, and `OwPsLoadImage` applies a DIR64-only relocation pass **after** CIS authentication and **before** any code runs:

- every entry target must land strictly inside the mapped image span;
- the target page must translate through the process's own PML4;
- the target page must be covered by a PRESENT+USER VAD of the process;
- non-DIR64 types are refused (`OW_ERR_UNSUPPORTED`); malformed blocks are refused (`OW_ERR_CORRUPT`);
- images with no `OWX_SECTION_RELOC` section take the unchanged fast path (all prior images unaffected).

This is the honest fix: the image is self-contained, CIS-authenticated byte-for-byte, and the fix-up is a deterministic function of the verified bytes and the load bias — the same property BSS zeroing already has, batched. No CIS weakening occurred; the dev trust anchor remains opt-in (`CIS_DEV_TRUST=1`, `--sign-dev`). The PE toolchain needs `-fpie`/`-fPIC` for this image class because its jump tables are unsuitable for `-fno-pie`; the payload is compiled as PIE.

## 6. Workstream E - Duktape executing in CPL3 (QEMU, actual evidence)

`build-generated/sweep_kapa_runtime_qemu_boot.log` (4,595 B) and `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` (**PASS**, 34 assertions):

```
[CIS] sweep: TRUSTED, signed by dev, licence accepted
[OWX] applied 211 base relocation(s)
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

Script 1 (`var r=0; for(var i=0;i<10;i++){r+=i;} r`) evaluated to **45**, retrieved via `duk_get_number`, verified numerically, and reported. Script 2 is the full R4 freestanding smoke (array map, JSON.parse, RegExp `/ab+c/.test`, `TypeError` catch, `0.1+0.2 > 0.3`, `String(123)`, `Date.now()` type check) executed by the same engine — its `'ok'` string was retrieved and verified. Heap destroyed, then `arena.used == 0` is asserted before shutdown (exit `0xE6` otherwise). The test *fails* on any stage failure by construction (distinct exit codes + distinct log markers + harness assertions).

## 7. Workstream F - Memory and resource lifecycle

- **Arena**: 192 KiB static `.bss` inside the image; `sweep_fs_arena_*` allocator; Duktape heap created with explicit arena hooks (`duk_create_heap`), never `duk_create_heap_default`. After `duk_destroy_heap`, `arena.used` is checked to be **0** in the passing run — no arena leak.
- **Frame run**: the sweep profile gets 16-page user stacks (`inc/ow_memory.h`, `OW_SWEEP_KERNEL`-gated; Duktape's parser recursion outgrew the default 2 pages — observed as an attributed fault `cr2=0x41FDFD8 rip=0x4005E0E` before the change, correctly attributed to the instance and its resources released).
- **Frame-run size**: the sweep profile's per-process run is 256 pages (image needs ~150; default profiles keep 64) — required by the ~150-page image; `OW_FRAME_RUN_PAGES` is profile-gated, full profile unaffected.
- **Teardown**: `OwPsTerminateProcess` releases the frame run (zeroed) and recycles the VAD tree to the pool (R5). Private page-table pages and bump-arena frames remain **not reclaimable** (documented limitation everywhere, still true).
- No runtime allocation can reach outside the KAPA's VADs: all demand mappings go through `ow_mem_walk` guardrails; the gateway refusals log `[KDB] gateway refused` if any escape is attempted (not observed).

## 8. Workstream G - Failure and fault testing

**Independent controlled-fault payload** (`sweep/kapa_fault/kapa_fault.c`), packaged as `kapa_fault.owx` (8,426 B, SHA-256 `43fcc3a410dc2d1e85f443f6a020167601c4b0dad4df55eb680c4288f887089c`, console subsystem, entry `0x40000FA`):

- Executes through the real KAPA path (KDB admit → CIS trust → CPL3 → dispatch → bridge log → **null-page dereference**).
- Producer evidence in `build-generated/sweep_kapa_fault_qemu_boot.log`:
  ```
  [KDB] instance 1: bound thread resumed on CPU (state=RUNNING)
  [KDB][LOG] assoc=1 target=0 len=39: kapa-fault: about to fault deliberately
  [PF] NOT-MAPPED err=5 cr2=0 rip=400011D cs=1B
  [KDB] instance 1 faulted (0x1); resources released
  [SWEEP-FAULT] selftest instance 1 finished (state=faulted fault=0x1 exit=0)
  ```
- Attribution is correct: `CR2=0`, CPL3 `cs=0x1B`, KDB instance FAULTED, resources released, scheduler and KDB lifecycle left consistent; kernel boot completes and freezes cleanly.
- `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` **PASS** (23 assertions).
- Failure codes for the runtime payload (`0xE0`-`0xE6`) are defined and, for the two paths reachable before CIS fixup work (0xE1, 0xE2), verified in the boot log before the C[IS]img was correct; the remaining codes are guaranteed by assertion construction, not exercised in QEMU.
- Startup-argument validation is in place (`argc < 1 || argv == NULL` → `0xE0`); the argument registers are consumed per the documented ABI and envp is treated as absent (never dereferenced).
- Capability denial is not exercised by a dedicated QEMU payload (the R5 gateway already proves refusal; the R6 runtime uses only granted capabilities by design).

**Not implemented / evidenced:** a dedicated QEMU payload for Duktape allocation failure (heap-create failure is `0xE3` and deterministic by code inspection, not boot-proven).

## 9. Workstream H - QEMU test integration

Existing targets preserved: `qemu-sweep-kernel`, `qemu-sweep-kapa-test CIS_DEV_TRUST=1` — both **PASS**. New targets:

- `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` → **PASS** (34 assertions; 11 required-chain markers + 23 absent-marker checks + `BROADCASTING IPI_FREEZE_SMP` marker).
- `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` → **PASS** (23 assertions; independent of the runtime test).
- Log assertions cover substrate (HAL/KDB/bridge/CIS/reduced profile), KAPA admission/trust/dispatch, runtime init, JS execution + result, bridge attribution, teardown, lifecycle, and absence of any excluded subsystem or refusal/fault marker. Both harnesses stop cleanly with exit 2 if run without the `CIS_DEV_TRUST=1` opt-in (fail-closed default). Serial logs preserved under `build-generated/`.

## 10. Workstream I - Build and regression verification (2026-11-13, fresh)

| Command | Result |
|---|---|
| `make -B -j 2` | **Success** |
| `make -B sweep-kernel` | **Success** (106 `.o`) |
| `make sweep-tests` | **1007 passed, 0 failed** |
| `make hosttest` | **1424 passed, 0 failed** |
| `make sweep-freestanding` | **Exit 0**, `nm -u` closure OK |
| `make check-external-deps` (full / sweep-kernel / sweep-tests / freestanding; bogus fails) | **All OK** |
| `make emergency` (OWX entry audit) | **PASS** (self-test + 13 targets) |
| `make qemu-sweep-kernel` | **PASS** |
| `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | **PASS** |
| `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` | **PASS** |
| `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` | **PASS** |

Full OpenWindows preserved alongside the reduced profile; no sibling repositories modified; `-std=c99 -Wall -Wextra -Werror` retained everywhere; default-untrusted build remains fail-closed (CIS arms with zero anchors and refuses every image).

## 11. Artifacts (sizes / SHA-256)

| Artifact | Size | SHA-256 / check |
|---|---:|---|
| `openwinkrnl.owx` | 1,662,976 B | `6fb140a25ef8aa13ecc4ad4cdd61a30772d78534381b1a813c0b644eb92071f0` (matches `.chk`) |
| `sweepwinkrnl.owx` | 1,531,904 B | `c310d1cfe68ad3c82163e93dc1d0848d1446065c19ec9c504364540dcf9517f4` (matches `.chk`) |
| `build/sweep-kapa-runtime/kapa_sweep.owx` | 405,738 B | `4a7bfded2fa493ec38b3559e2ae6905330f7e6de75c6731986012f726813de8f` (matches `.chk`); entry RVA `0x1360`; 0 imports |
| `build/sweep-kapa/kapa_test.owx` | 8,426 B | rebuilt, unchanged semantics |
| `build/sweep-kapa-fault/kapa_fault.owx` | 8,426 B | `43fcc3a410dc2d1e85f443f6a020167601c4b0dad4df55eb680c4288f887089c` (matches `.chk`) |
| `build-generated/sweep_kapa_runtime_qemu_boot.log` | 4,595 B | fresh run |
| `build-generated/sweep_kapa_fault_qemu_boot.log` | 4,436 B | fresh run |
| `build-generated/sweep_kernel_qemu_boot.log` | 3,721 B | fresh run |
| `build-generated/sweep_kapa_qemu_boot.log` | 4,298 B | fresh run |

## 12. Modified/created files (R6)

Created: `sweep/kapa_sweep/kapa_sweep.c`, `sweep/kapa_fault/kapa_fault.c`, `tools/qemu_sweep_kapa_runtime_test.ps1`, `tools/qemu_sweep_kapa_fault_test.ps1`, `docs/SWEEP_R6_REPORT.md`.  
Modified: `Makefile` (R6 runtime + fault build blocks, QEMU targets, `.PHONY`), `core/main.c` (generic KAPA self-test boot block with runtime/fault variants, failure diagnostics, zero-initialize and FAULTED pre-seed, `ow_mem.h` include), `inc/ow_memory.h` (profile-gated `OW_USER_STACK_PAGES` 16 in sweep, `OW_FRAME_RUN_PAGES` 256 in sweep), `ps/owx_loader.c` (PE DIR64 relocation pass, `.reloc` accepted), `tools/owx_pack.py` (`.reloc` kept and typed `OWX_SECTION_RELOC`), `docs/SWEEP_*.md`, `SWEEP.md`. The R5 QEMU harness and assertions were not weakened.

## 13. Remaining limitations (explicitly stated)

1. **No MMU/CPU/VM isolation.** Trust is a CIS verdict, never a hardware boundary; the KAPA shares the kernel's fault domain. No claim of stronger isolation is made or implied.
2. **Bump-arena monotonicity.** Physical frames and private page-table pages of terminated processes are not recycled (VAD nodes are; frame-run commit charge is released). KAPA churn is bounded by explicit limits (64 processes, 16 MB arena, 256-page runs), not by a free-list allocator.
3. **The Sweep browser runtime is not an OWX image yet.** `kapa_sweep.owx` runs the engine's foundation (Duktape + runtime seam + resource ABI skeleton), not HTML/CSS/DOM/rendering. No browser content is executed.
4. **`SWEEP_RUNTIME.read_resource` has no KAPA backend.** There is no file/resource service in R6; any engine path that needs one fails deterministically. A root-confined native resource backend is still unimplemented.
5. **`Date.now()` is deterministic-zero** in the KAPA build (libc shim returns 0; `main.runtime_info` is available and used for the runtime clock, not for Duktape's Date).
6. **Minimal KAPA service surface by design**: `main.ping`, `main.log`, `main.runtime_info`, `main.echo` + PS exit/PID. No timers, no allocator service, no input, no presentation.
7. **Duktape allocation-failure paths (`0xE3`) and capability-denial under QEMU** are not individually exercised in QEMU (deterministic by inspection/host tests; not boot-proven).
8. **Relocations:** only DIR64 is accepted in the OWX loader's relocation pass; any other PE relocation type is a hard `OW_ERR_UNSUPPORTED`. PIE code is mandatory for images with jump tables.
9. **No second kernel, no compositor, no browser UI, no networking.** Out of R6 scope as specified.
10. **Artifact stability.** `openwinkrnl.owx`/`sweepwinkrnl.owx` were rebuilt (loader + sweep-profile memory constants); CIS signatures and `.chk` values in the R4/R5 docs refer to the pre-R6 builds and are superseded by the hashes in §11.

## 14. Classification of deliverables (this milestone)

| Item | Class |
|---|---|
| Freestanding runtime audit (Workstream A) | **Implemented and documented** |
| KAPA runtime startup ABI (Workstream B) | **Implemented and demonstrated** (`kapa-sweep: runtime initialized` in QEMU) |
| Freestanding service backend (Workstream C) | **Implemented and demonstrated** (attributed `main.log`/`main.runtime_info` calls) |
| `kapa_sweep.owx` packaging (Workstream D) | **Implemented and validated** (CIS trust, load, relocation fix) |
| Duktape executing in CPL3 (Workstream E) | **Implemented and demonstrated** (script 1 numeric, script 2 smoke, verified teardown) |
| Memory/resource lifecycle (Workstream F) | **Audited and enforced** (arena drained to zero; frame-run teardown; documented limits) |
| Failure/fault testing (Workstream G) | **Implemented and demonstrated** (controlled CPL3 fault attributed; runtime failure codes defined) |
| QEMU integration (Workstream H) | **Implemented and passing** (3 harnesses preserved + 2 new, all PASS) |
| Regression (Workstream I) | **Green across the board** (see §10) |
| Browser engine in KAPA | **Not implemented** (out of scope; foundation only) |
| Native resource backend | **Not implemented** |
| Isolation beyond CIS verdict | **Not implemented** (explicit non-goal) |

## 15. R7 recommendations

1. **Package the browser engine core** (HTML parser + DOM) as a second section of the runtime image and execute `Sweep*CreateDocument` at CPL3 with a canned HTML fragment; promote B7 closure from "mechanism proven" to "engine proven".
2. **Native resource backend** for `SWEEP_RUNTIME.read_resource` rooted at a run-scoped, root-confined OWFS volume (then the runtime can parse real HTML files under policy).
3. **Timers** for script deadlines: reuse `main.runtime_info` or add a bounded `main.now` + deadline callback behind a per-KAPA capability; enables real JS watchdogs.
4. **Frame-run reclamation**: return terminated processes' private page-table pages and physical frames to the bump arena via a free-list (safe only with the existing frame-run ownership primitives), making repeated KAPA churn practical.
5. **Per-KAPA service namespaces beyond `TargetInstanceId == 0`**, once instance-owned services can be registered (the bridge already supports attribution).
6. **QEMU for Duktape allocation failure** (`0xE3`) and **capability-denial** scenarios with dedicated payloads, matching the fault payload's pattern.
7. **Relocation coverage**: extend the loader past DIR64 (or document it as permanently DIR64-only) after seeing whether real PE producers emit other types here.
8. **Keep the R5/R6 harnesses in CI** and refuse to weaken their assertions.

**Final principle respected:** the kernel provides the controlled execution environment; the KAPA owns application behavior; the browser remains application-side. R6 proves the environment is real, bounded, and attributable — nothing about presentation, isolation, or the completeness of the browser is claimed.
