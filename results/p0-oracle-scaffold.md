# P0 — the nasm differential oracle, in the tree, with a `make` gate (findings)

**Unit:** `HANDOFF=v86c`, unit **P0**. **Scope:** `tests/`, `oracle/`, `Makefile`, `results/`.
`v86/` is read-only and was never written to; no git commits. Every claim below is
tagged **MEASURED** (run on this host, with the command or the file that holds the raw
output), **READ** (from the reference source, with `file:line`) or **INFERRED** (and
then it says so).

**Status:** G1–G4 all exit 0. `make nasm` = **113/113 pass, 0 fail, 10 skips, exit 0** —
and the 10 skips are *labelled reference blind spots*, not hidden failures: the
unfiltered run (all 123 tests) is archived in `results/p0-nasm-refrun-raw.txt` and is
reproducible with one command (§7). Four defects in the planner's artifacts were found
by measurement, fixed, and their effect attributed by re-running the corpus (§6).

---

## 1. What moved, and the proof that it is the same thing

Everything the plan §6 describes already existed in `/tmp/oracle-exp/`. What P0 did was
make it a first-class part of the tree, and — this is the part that earned its keep —
validate it against upstream's own emulator, which is where the defects were.

| in the tree | from | relation to the planner's artifact |
|---|---|---|
| `tests/nasm2gas.py` | `/tmp/oracle-exp/nasm2gas.py` | cleaned up; **MEASURED equivalent**: for all 123 tests, `as --32` + `ld` on the old and new outputs give **byte-identical `.img` files** (`diff` of the two md5 lists is empty; the only textual difference is one trailing blank line) |
| `tests/mkfixture.c` | `/tmp/oracle-exp/mkfixture.c` | copied, then **two measured fixes** (§6.3, §6.4) |
| `tests/nasm_run.py` | — (new) | the differential runner, the skip list, the stages |
| `oracle/nasm_state.mjs` | — (new) | the JS-reference leg: boots one image under M0's pin and prints a **fixture** (the plan named three tools; the reference leg needs a driver, and making it emit the gdb-extract-def format is what lets one parser and one comparison serve both legs) |
| `tests/nasm_skip.txt` | — (new) | the per-test skip list, with reasons (§7) |
| `Makefile` | — | `nasm-translate`, `nasm-fixtures`, `nasm` (+ `build/mkfixture` via `HOSTCC`, since it is a ptrace host binary, not an APE) |

The reference is **the tree's M0 pin**, `oracle/node_modules/v86` = `0.5.471+g94faf3d`
(the clone's HEAD), used for both oracles rather than a second copy.

## 2. The pipeline

```
v86/tests/nasm/<t>.asm                                 (READ-ONLY source of truth)
  python3 tests/nasm2gas.py            -> build/nasm/<t>.s      .intel_syntax noprefix
  as --32 -mx86-used-note=no           -> build/nasm/<t>.o
  ld -m elf_i386 -z noseparate-code \
     --section-start=.bss=0x100000 --section-start=.text=0x80000 \
     --section-start=.multiboot=0x20000 -e _start  -> build/nasm/<t>.img   (an ELF)
  build/mkfixture <t>.img <loop addr>  -> build/nasm/<t>.fixture  (native ground truth)
  node oracle/nasm_state.mjs <t>.img <limit>       -> the ACTUAL state, same format
  tests/nasm_run.py compares them with run.js's own logic (v86/tests/nasm/run.js:384-510)
```

* Geometry is upstream's (`v86/tests/nasm/create_tests.js:127-130`); `<loop addr>` comes
  from the `loop` symbol in the linked ELF (the `int3` breakpoint replaces the `hlt`).
* The **fixture is gdb-extract-def's 2116-value document** — 8 GPRs, eip, 8 st, 16 mm
  words, 32 xmm words, 2048 words of BSS `[0x100000,0x102000)`, eflags, ftag, fstat —
  wrapped in `---BEGIN JSON---`/`---END JSON---`, with gdb's unquoted `inf`/`-inf`/`nan`
  tokens and the `(signal SIGxxx)` marker `run.js`'s `extract_json` reads (run.js:89-135).
  The JS-reference leg emits the same document with the v86 exception mapped back to the
  signal name (0→SIGFPE, 6→SIGILL, 13→SIGSEGV, 14→SIGBUS), so `tests/nasm_run.py`
  applies one parser and one comparison to both legs.
* `tests/nasm_run.py` transcribes run.js:384-510: `reg32[0..7]`, the 8 KiB memory window,
  `eflags & MASK_ARITH` (run.js:49), the `fpu_tag != FPU_TAG_ALL_INVALID` branch
  (run.js:423), the FPU status word under `FPU_STATUS_MASK` (run.js:51), the floats at
  `FP_COMPARISON_SIGNIFICANT_DIGITS = 7` with NaN/±inf equal to themselves (run.js:54-66),
  and for an exception fixture **only** the exception kind and its eip (run.js:489-510).
  Failure names and `format_value` are upstream's too, so a failure here means what it
  means upstream.

## 3. The gates — raw evidence

Full transcript with every command's exit code: **`results/p0-gates.txt`**.

| gate | command | result | evidence |
|---|---|---|---|
| **G1** | `make nasm-translate` | **exit 0**, `123/123` translated+assembled+linked | `results/p0-gates.txt` |
| **G2** | `make nasm-fixtures` | **exit 0**, `123/123` fixtures, mix **113 breakpoint / 3 SIGFPE / 2 SIGILL / 5 SIGSEGV** — the planner's measured mix, reproduced | same |
| **G3** | `make nasm` (C port absent: the JS reference vs the native fixtures) | **exit 0**, `Passed 113/113 tests`, 10 labelled skips | same |
| **G4** | `make`, `make oracle` | **exit 0** each (M0's gate still passes, M1a's CPU objects still build) | same |

Runtime: G1 ≈ 10 s, G2 ≈ 4 s, G3 ≈ 11 s (123 assemblies, 123 ptrace runs, 123 node
boots) — cheap enough to be a continuous gate.

Regression on the deliverable's own equivalence: `tests/nasm2gas.py`'s output through the
whole toolchain is byte-identical to the planner's translator's output for all 123 tests
(§1) — i.e. the cleanup is provably behaviour-free, not "looks the same".

## 4. The negative control — the gate can fail

`results/p0-negative-control.txt`. One fixture, **one field** changed, everything else
asserted byte-identical, and the runner must report it — once per comparison path the
113 passes rest on:

```
NEGATIVE CONTROL 1/3 -- one GPR field (cpu.reg32[0] = eax): 0 -> 0x10203040
	cpu.reg32[0]
	Actual:   0x0
	Expected: 0x10203040                                          -> exit 1

NEGATIVE CONTROL 2/3 -- one x87 field (st0): "invalid" -> 1.5
	st0
	Actual:   nan
	Expected: 1.5                                                 -> exit 1

NEGATIVE CONTROL 3/3 -- the exception kind: the fixture is made to claim "(signal SIGFPE)"
	exception eip
	Actual:   0x8003d
	Expected: 0x8003c
	Exception
	Actual:   None
	Expected: DE                                                  -> exit 1
```

Contrast measured on the same fixture set: flipping *no* field gives `Passed 1/1`, so the
report is not a runner that always complains.

## 5. What the oracle does and does not compare (MEASURED)

`results/p0-coverage-notes.txt`. Two honest limits, both found while making G3 pass —
neither is hidden by the pass:

1. **The mm words are never compared.** run.js takes its mm branch only when the
   fixture's tag word equals `FPU_TAG_ALL_INVALID` (0xAAAA, run.js:50,423). All 123
   fixtures carry `0xffff` in the tag word's upper half (the kernel's i387 tag word is
   16 bits of tag plus those 16 bits), so `fpu_tag == 0xAAAA` is unreachable and the st
   path is always taken. mm-visible effects are still gated — through the memory window
   — which is why `maskmov*`, `ps*`, `pack*`, `punpck*` fail when the port is wrong.
   **MEASURED for our fixtures; the (*whether upstream's gdb-produced fixtures behave
   the same*) part is INFERRED — gdb is absent.**
2. **The exception branch cannot be exercised by the JS reference.** 10 fixtures carry a
   signal (3 SIGFPE / 2 SIGILL / 5 SIGSEGV). The pinned npm package is the **release**
   build, and `cpu_exception_hook` is DEBUG-gated there (READ: `v86/src/rust/cpu/cpu.rs:2441,
   2505, 3431, 3444, 3457, 3469` — `if DEBUG { if js::cpu_exception_hook(code) { return; } }`),
   so the hook the runner installs is never called: 5 of the 10 make the release wasm
   panic outright (`Unimplemented: #GP handler`, `cpu.rs:819`, because delivery runs into
   the absent IDT) and 5 report no exception at all. Upstream's own suite runs the DEBUG
   build (READ: `v86/tests/nasm/run.js:10-11`), which cannot be built here (needs
   java/closure-compiler and a wasm32 rust target). The branch stays in the runner — the
   C port *does* report exceptions — and the negative control proves the runner reports
   it (control 3/3 above).

## 6. Four defects in the planner's artifacts — measurement, fix, attribution

The plan states the pipeline was "VALIDATED THIS PASS"; G3 found it was validated on the
one image shape that cannot show these defects (the planner booted `arith-optimisations`,
which has no `.data` section and an all-zero x87/xmm state). Each defect below was
attributed by **re-running the corpus** with and without the fix — the counts are the
proof, not the argument.

| # | defect | measurement | fix | attribution |
|---|---|---|---|---|
| 1 | **63 images never started** in v86 | with ld's default `-z separate-code`, `.data` gets its own page-aligned PT_LOAD at file offset 0x2000 and ld puts the orphan `.multiboot` section after it, at 0x213c — past the 8192-byte window v86 scans (READ: `v86/src/cpu.js:1327-1338`, `MULTIBOOT_SEARCH_BYTES`). `load_multiboot` then falls through **without setting eip** (MEASURED: eip stays at the reset vector 0xffff0, esp=0x100, the guest wanders the BIOS area) | `ld -z noseparate-code` | 63/123 images broken → **0** (`results/p0-link-geometry.txt`); all 63 appear in the first run's failure list and no broken image passed |
| 2 | **4 more images** (psllw/psrlw/psllq/psrlq) | gas emits a `.note.gnu.property` section, ld gives it a page-aligned R-only PT_LOAD past 8192 and `.multiboot` follows it | `as -mx86-used-note=no` (nasm never emits that note) | 4/123 → **0**; also brings the image closer to upstream's nasm product |
| 3 | **the x87 state was read one rotation too far** | the 512-byte image `PTRACE_GETFPREGS` returns for a 32-bit tracee is the x86_64 fxsave layout **whose `st_space` slots already hold LOGICAL st(i)**, while the abridged FTW bits index PHYSICAL registers. `mkfixture.c` applied gdb-extract-def's `(top+i)&7` rotation *again* (and its tag test to the raw abridged byte, which also invented an `"invalid"` entry gdb would not print). MEASURED with the raw buffer (`results/p0-fpu-layout-rawdump.txt`) and arbitrated by the guest itself: `fnstenv` writes the guest's own cw/swd/TAG into the compared memory window, and both legs agree on them (`results/p0-fpu-harness-evidence.txt`) | read `st_space[4*i]` for logical st(i); build the full 16-bit tag word from the abridged one (`tag_of`: 0x7fff exponent → special, exponent 0 and zero mantissa → zero, else valid), which reproduces the guest's own 0x7fff tag word | **17 tests failed → 0** (the same 17 with the fix reverted: `results/p0-nasm-oldharness.txt`, 96/113) |
| 4 | the **status word** carried `\| 0xffff0000` (only the *tag* word gets the kernel's `convert_from_fxsr` OR; the guest's own `fnstsw` says 0x3800) | **fidelity only, NOT a comparison defect** — run.js masks the value with `FPU_STATUS_MASK` = 0xFDD5, whose bits are all below 16, so `0xffff3800 & 0xFDD5 == 0x3800`: MEASURED by running the whole corpus with the OR back in → **113/113 again** (`results/p0-nasm-fstat-variant.txt`) | `fstat = f.swd` | **none** — stated as inert so that nobody later mistakes it for a fix that made G3 pass |

Net: the first differential run, over 118 runnable tests, was **36 pass / 82 fail**. The
63 + 19 = 82 failures decompose exactly into 63 loader-broken images + 16 x87-ordering
tests + 3 reference blind spots (`results/p0-nasm-baseline.txt`, `results/p0-link-geometry.txt`).

## 7. Every test that does not pass, classified

`make nasm` skips 10 tests; the unfiltered run
(`python3 tests/nasm_run.py --stage run --skip-list /dev/null`, archived as
`results/p0-nasm-refrun-raw.txt`) shows all of them, and all 123 tests, as
`113 passed / 5 failed / 5 produced no state, exit 1`. The reasons are also printed by
the gate itself, so nothing here is discoverable only from this file:

* **class A — the reference produces no state (5):** `idiv8-overflow`, `idiv16-overflow`,
  `idiv32-overflow`, `mov_sreg_ud1`, `mov_sreg_ud2`. Reference panics
  `Unimplemented: #GP handler`. Cause and evidence in §5.2.
* **class B — the reference takes no fault where the hardware does (5):** `jump`,
  `maskmovdqu`, `maskmovq`, `pop_esp`, `pop_esp2`. Each: the fixture has a SIGSEGV at a
  specific eip; the reference reports **no exception** (it halts anyway, or in `jump`'s
  case keeps executing at eip 0x42b001, past the 2 MiB RAM, and never halts). v86's flat
  2 MiB memory + MMIO fallback is the natural suspect — **INFERRED, not measured**; what
  is MEASURED is the reference's behaviour and the fixture's expectation.

Both classes are properties of the **reference leg**, not of the comparison and not of the
port: `tests/nasm_skip.txt` says so per line, and the C leg should run with **its own**
`--skip-list` (M1c owns the exception path, and the port *can* report these faults).

## 8. Divergences from the plan, each with its measured reason

1. **`-z noseparate-code`** and **`as -mx86-used-note=no`** added to the geometry
   (plan §6.1 specifies the plain `ld` invocation). Reason: §6.1/§6.2 above — without them
   67 of 123 images never reach the guest, so the oracle cannot gate anything on those
   tests. `.text`, `.bss`, entry and the paddrs are unchanged; only file offsets move.
2. **`tests/mkfixture.c` is not a verbatim copy** (plan §6.2 says it is done and verified).
   Reason: §6.3/§6.4 above, with the guest's own `fnstenv` as arbiter. The file keeps its
   structure and every host-kernel workaround the planner catalogued (aligned
   `POKETEXT`, `process_vm_readv`, the two-stop exec wait, the x86_64 layouts).
3. **`oracle/nasm_state.mjs` is a fourth tool** (the plan names three). Reason: the JS leg
   needs a driver; emitting the fixture format (rather than a bespoke JSON) is what lets
   the runner use run.js's own comparison on both legs.
4. **The plan §6.3 claim that unpinned npm 0.5.470 "REJECTS these ELFs" is not reproduced.**
   MEASURED: `0.5.470+g6db8b15` (the copy in the planner's own `/tmp/oracle-exp/jsref`)
   boots the translated images and its differential result over the corpus equals
   0.5.471's on 116 of the 117 tests that ran under it (the pass set is identical: 113),
   the single difference being `jump`, which 0.5.470 panics on where 0.5.471 loops
   (`results/p0-nasm-refversion-0470.txt`). **So the pin is not load-bearing for this
   corpus**; the tree uses M0's pin anyway, because one
   reference for the project beats two. SCOPE: 117 of 123 tests; the other 6 produced no
   state under either version.
5. **"Expected near-total pass" for G3 — measured near-total**, with the 10 blind spots
   above classified rather than assumed away. The gate exits 0 only because they are
   *named in a committed file with reasons*, printed in the summary, and reproducible
   unfiltered in one command; a runner that could not report a mismatch would have failed
   this brief, and §4 is the proof that it can.
6. **The exception eip printed by the reference leg is the current eip** when no exception
   was recorded (it cannot know it missed one). That is faithful to what the release build
   exposes, and it is why the `exception eip` mismatch in class B is reported — a port
   that gets the fault right will be compared against the fixture's faulting eip properly.

## 9. What later units need (interfaces)

* **Point the runner at the port** with `--actual`:
  `python3 tests/nasm_run.py --stage run --actual "build/v86c-nasm {img} {limit}"` — the
  command must print the 2116-value fixture document on stdout (with `(signal SIGxxx)`
  when the guest took an exception) and exit 0. A non-zero exit is reported as
  *"produced no state"*, not as a mismatch. (Demonstrated working: §8.4's run passes an
  arbitrary command through that hook.)
* **Shrink `tests/nasm_skip.txt`**: one `NAME<2 spaces>reason` per line; every entry is
  printed with its reason. Give the C leg its own file if its gaps differ from the
  reference's.
* A mismatch's name/actual/expected are upstream's, so a failure maps 1:1 onto the port
  source (e.g. `fpu status word` → `fpu_load_status_word`, `mem[101FDC]` → the 8 KiB
  window at that address).
* Stages: `--stage translate|fixtures|run|all`. Every stage regenerates unconditionally —
  there is no mtime skipping, deliberately, so a stale `.s`/`.img`/`.fixture` can never be
  read as a result of the current sources. (A shared-`build/` hazard is real: another unit
  ran `make clean` mid-run during this unit, so the runner re-creates its work directory
  per test.)
* `build/mkfixture` is built by the Makefile with `HOSTCC` (plain `cc`), **never** cosmocc.

## 10. Raw evidence in the tree

| file | what |
|---|---|
| `results/p0-gates.txt` | G1–G4, every command with its exit code, from a clean `build/` |
| `results/p0-negative-control.txt` | the three negative controls, raw |
| `results/p0-nasm-refrun-raw.txt` | the unfiltered differential run (123 tests) |
| `results/p0-nasm-baseline.txt` | the first differential run (planner's geometry + harness): 36/118 |
| `results/p0-nasm-oldharness.txt` | fixed geometry + the planner's harness: 96/113 |
| `results/p0-nasm-fstat-variant.txt` | fixed everything, status-word OR back in: 113/113 (§6.4) |
| `results/p0-nasm-refversion-0470.txt` | the corpus under npm 0.5.470 instead of the pin (§8.4) |
| `results/p0-link-geometry.txt` | `.multiboot` file offset vs the 8192-byte window, three geometry variants |
| `results/p0-fpu-layout-rawdump.txt` | the raw 512-byte `PTRACE_GETFPREGS` buffer |
| `results/p0-fpu-harness-evidence.txt` | the x87 fixes, arbitrated by the guest's own `fnstsw`/`fnstenv` |
| `results/p0-coverage-notes.txt` | tag-word/mm and exception coverage, measured |

## 11. Claims, marked

* **MEASURED:** G1 123/123; G2 123/123 with the 113/3/2/5 mix; G3 113/113 pass + 10 labelled
  skips, exit 0; G4 both exit 0; the negative control reports on all three paths; the two
  translator outputs are byte-identical through `as`+`ld` for all 123 tests; the 63/4/0
  `.multiboot`-past-8192 counts; the `0xffff3800 & 0xFDD5 == 0x3800` inertness; the
  17-test x87 attribution (96/113 → 113/113); the reference's release-build exception
  blindness (5 panics, 5 no-exception); the 2116-value fixture shape; the unfiltered
  113/5/5/exit-1 accounting; 0.5.470 vs 0.5.471 on 117 tests.
* **READ:** run.js's comparison (384-510) and constants (44-60, 89-135, 423, 489-510);
  `v86/src/cpu.js:1327-1338` (8192-byte scan), `:1455-1535` (the ELF branch), `:1540-1570`
  (state set-up); `v86/src/rust/cpu/cpu.rs:819, 2441, 2505, 3431-3469` (DEBUG-gated hook);
  `v86/tests/nasm/create_tests.js:127-130`, `gdb-extract-def`, `gen_fixtures.js`;
  `v86/tests/nasm/run.js:10-11` (upstream runs the DEBUG build).
* **INFERRED (flagged where it appears):** that upstream's gdb-produced fixtures also
  carry the tag word's upper half (§5.1); that v86's flat 2 MiB memory explains class B's
  no-fault behaviour (§7); that the md5-identical `.img` files imply the cleaned
  translator is behaviour-free in general (it is measured for the whole 123-test corpus,
  which is the only surface that exists).
* **NOT DONE, deliberately:** building v86's DEBUG bundle (needs java + a wasm32 rust
  target) — that is what would let the reference exercise the exception branch; and
  injecting an IDT/handler into the guest so the reference could report a fault through
  memory (a real option, but it changes the guest environment and belongs with M1c's
  exception work, not with P0's "validate the comparison logic" gate).
