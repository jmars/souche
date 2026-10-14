# M1b-2 — the MMX/SSE groups of the 0F table, and `sse_instr.c`

**Unit:** `HANDOFF=v86c`, unit M1b-2 (coder). **Deliverables:** `src/cpu/sse_instr.{c,h}` (new),
the 533 MMX/SSE call sites `results/m1b-scope-mmx-sse.txt` names appended to
`src/cpu/instructions_0f.c`, and MMX/SSE unit cases in `src/cpu/cpu_selftest.c`.

Every claim below is marked **MEASURED** (a command was run on this host; its raw output is in
`results/m1b2-gates.txt`), **READ** (from the reference source) or **INFERRED**.

---

## 1. Bottom line

**MEASURED.** The emitter's census moved from **564 unimplemented call sites** to **31**, i.e.
exactly 533 fewer — the same 533 the scope file names, and **all 31 that remain are M1b-1's
documented M1c-owned deferrals** (0F 00 /2../5, 0F 02/03, 0F 30-32, 0F 34/35, 0F C7 /6 — this
file touches none of them). `make`, `make transcript-diff`, `make selftest` and `make nasm` are
all exit 0, and the nasm C leg rose from **60/60 passed, 63 skipped** to **118/118 passed,
0 failed, 5 skipped** — a delta of **+58 passed / −58 skipped** (§6).

**One finding qualifies all of that, and it is the most important thing in this report:** the
nasm corpus's *MMX-register* comparison is **dead code** — an MMX-register mutation passes
`make nasm` unchanged. §7 measures it, and measures that the port passes the corpus **118/118
with the comparison repaired**. The 58 tests are real evidence about *decode*, *control flow*
and *the XMM/GPR/memory* results; they are **not**, as shipped, evidence about the MMX registers.

---

## 2. Files

| file | what |
| --- | --- |
| `src/cpu/sse_instr.c` | **new, 479 lines** — `sse_instr.rs` (361 lines) plus `transition_fpu_to_mmx` (cpu.rs:4201-4207) |
| `src/cpu/sse_instr.h` | **new, 85 lines** — its interface and the MMX/x87 aliasing note |
| `src/cpu/instructions_0f.c` | 1871 → **6013 lines**; **+4142, all append-only** — 533 emitter-visible definitions + 212 `static` base bodies; M1b-1's 377 definitions are untouched |
| `src/cpu/instructions_0f.h` | 838 → **1380 lines**: +533 prototypes |
| `src/cpu/cpu_selftest.c` | +480 lines in one new function `test_instr_0f_mmxsse()` + its call |
| `Makefile` | **three added lines**: `src/cpu/sse_instr.c` in `CPU_SRC` and `SELFTEST_SRC`, `src/cpu/sse_instr.h` in `CPU_HDR` |
| `tests/nasm_skip_c.txt` | regenerated (5 entries; §6) |

`v86/` was not modified (READ-ONLY). `tests/nasm_run.py` and `oracle/` were not modified — the
§7 experiment that patches the harness ran in a **scratch copy**. No commit.

`src/cpu/cpu_selftest.c` is shared (M1b-1 and M1b-3 both wrote it) but **no peer was live this
pass** (MEASURED: `fx-agent-presence` reported only the orchestrator and a reviewer), and every
edit was a read-modify-write of the current file.

---

## 3. The scope actually implemented

**MEASURED** (`comm -12` of the 533 scope symbols against the emitter's remaining stops →
**empty**): every symbol `results/m1b-scope-mmx-sse.txt` names is defined, in the two shapes the
emitter emits:

* **533 emitter-visible** `void instr_...` definitions — exactly the names
  `gen/generate_interpreter.js:87-101` produces (the scope file's names, verbatim);
* **212 `static` base bodies** — the ones the `_mem`/`_reg` pairs forward into, plus
  `maskmovq`/`maskmovdqu`. They are `static` because the emitter's availability scan
  (`gen/interpreter_c.js:93`, `^void[ \t]+instr...\(`) must not mistake a body for an
  implemented emitter symbol; M1b-1 set that precedent with its hadd/hsub pairs.

Three names are **not** suffixed and are `void`, because the emitter calls them bare: `instr_0F77`
(emms), `instr_0FD0` and `instr_0FF0` (both `unimplemented_sse`). MEASURED: those three are the
only scope entries with neither `_mem` nor `_reg`.

**Not** re-ported (already M1b-1's, at the bottom of the file, and deliberately left alone):
`instr_0F7A/0F7B/0F7C/0F7D`, `instr_660F7C/660F7D`, `instr_F20F7C/F20F7D`, `instr_660FEF`,
`instr_0FFF`. **MEASURED**: the closure of the 533 scope names contains none of them, so there
is no duplicate definition to collide with.

---

## 4. Divergences from upstream — each with its reason

The port is 1:1 except for these. Every one is a spelling forced by C; none changes a value.

1. **`instr_0F7F(r: i32) -> u64` is DROPPED.** `instructions_0f.rs:3150` defines
   `instr_0F7F(r) { read_mmx64s(r) }` and *nothing calls it* — not upstream, not here (both
   wrappers call `mov_r_m64` / `read_mmx64s` directly). A `static` that is never used is
   `-Werror=unused-function`, so it is removed with a comment at its site rather than kept dead.
2. **Signed overflow is formed on the `uint32_t` pattern** in `paddd`/`psubd` (both forms),
   `paddsw`'s i16 sum, `pmaddwd` (the sum of two `2^30` products reaches the sign bit) and
   `pmulhuw`. Rust's release profile (`v86/Cargo.toml`: no `overflow-checks`) makes these
   **wrap**, which C calls UNDEFINED BEHAVIOUR on a signed type. The uint32_t spelling is
   bit-identical and defined — the M1a-2 D5 rule, extended to `+`/`-`/`*`.
3. **Left shifts over possibly-negative lanes** (`pslld`, `psldq`'s halves) are done on the
   unsigned pattern and cast back — the same rule (C's `1 << 31` on an `int` is UB). Shifts that
   cannot reach the sign bit (u16/u32 lanes promoted to `int`, shift < 16) are left in the
   promoted form, with the arithmetic named at the site.
4. **`1.0 / x` is `1.0f / x`** in `instr_0F52/F30F52/F30F53`: Rust infers the literal from the
   `f32` operand; C needs the suffix to keep the division in `float`.
5. **`f64::from_bits` / `f32::from_bits` / `std::mem::transmute` between a `u64` and `[i32; 2]`
   or `[f32; 2]`** are read through the `reg128` union (cpu_state.h:196-207), whose members *are*
   those little-endian views. No `memcpy`, no aliasing question, no helper.
6. **`.abs()` on an i32** (`psadbw`) is a conditional negation: `i32::abs` has no C peer without
   pulling in `stdlib.h`, and it is *UB on INT_MIN* where Rust's is a wrapping negation. The
   operand is a byte difference (−255..255), so neither differs — but the conditional is the
   spelling that stays correct if it ever is not.
7. **`.unwrap()` on a proved-fault-free result becomes `DBG_ASSERT(!cpu->pagefault)`**
   (`maskmovq`/`maskmovdqu`'s `safe_write8`, whose caller pre-checks the whole 8/16-byte window
   with `writable_or_pagefault`) — the `misc_instr.c:14` rule.
8. **`dbg_log!` is dropped**, its text kept as a comment (no log channel in the port). No
   `dbg_log!` remains in this unit's half of the table (`instr_0FFF`'s is M1b-1's).
9. **`sse_comparison`'s `_ =>` arm becomes `default:`** returning the `op & 7 == 7` case. The
   arm is unreachable after `& 7`; Rust's `dbg_assert!(false)` in it is a `debug_assert!`
   (compiled out of the release reference), so a live C assert would diverge from the reference.
10. **The `-0x80000000` sentinel is written `(int32_t)0x80000000u`** — `-0x80000000` in C is a
    *unsigned* 2147483648 negated, and converting it back is implementation-defined. M1b-1's
    divergence 5, same reason.
11. **`transition_fpu_to_mmx` lives in `sse_instr.c`, not `misc_instr.c`.** Upstream has it in
    cpu.rs:4201-4207, adjacent to the `task_switch_test*`/`set_mxcsr` block that `misc_instr.h`
    already owns — but it is defined entirely in terms of this module's subject
    (`fpu_set_tag_word` + `cpu->fpu_stack_ptr`), it is called from this module and from every
    MMX instruction, and putting it in `misc_instr.c` would mean editing M1a-2's file.
    **REPORTED as a placement decision, not a preference.**
12. Nothing else diverges. The arm tables, the lane orders, the operand orders (notably `sub`/`div`
   being `destination − source` while `add`/`mul` are symmetric), the per-instruction shift
   clamps, the transition's *position* (in the body for `instr_0F2A`, in the `_reg` wrapper for
   `instr_660F2A` and *absent* from its `_mem` form, inside the value-returning body for
   `instr_0FD7`) and the fault guards are upstream's exactly.

---

## 5. Things worth a reader's attention (the near-misses)

* **`pmulhuw`'s product does not fit an i32** (0xFFFF × 0xFFFF = 4294836225) and the reference's
  release build wraps it. The port forms it in `uint32_t` and keeps the high half. **This is not
  a divergence and not a bug in the reference:** a shift right by 16 differs between the
  arithmetic and logical readings only *above bit 15*, and the lane keeps bits 0..15, so the
  port's answer is exactly the reference's — and exactly the correct `pmulhuw` (0xFFFE). An
  earlier draft of this unit's comment claimed the reference computed something *else*; it did
  not, and the comment was corrected rather than left as a plausible-sounding wrong cause.
* **`psrldq`/`pslldq`'s `if imm8 == 0 return;` is load-bearing, not an optimisation.** The
  `shift <= 63` arm shifts by `64 - shift`, which is UB for `shift == 0` — reachable only
  through `imm8 == 0`, which the early return catches. Stated at the site and pinned by a
  selftest case.
* **The tag word an MMX write leaves is "VALID", not "invalid"**, because softfloat's
  `is_finite` is upstream's *weak* definition (`x != ±inf`). **MEASURED**: after
  `movd mm3, eax` the tag word is `0x1515` (slot 3 = 0, valid) where it had been `0x3FFF`
  (slot 3 = 3, empty). Recorded because it is the opposite of what the 0xFFFF `sign_exponent`
  looks like it should mean.

---

## 6. Gates

Raw output and exit codes: **`results/m1b2-gates.txt`**.

| gate | command | exit | result |
| --- | --- | --- | --- |
| G1 | `make nasm` | **0** | **Passed 118/118, 0 failed, 5 skipped** (baseline 60/60, 63 skipped) → **+58 / −58** |
| G2 | `make selftest` | **0** | **3522 checks, 0 failures** (+173 this unit's) |
| G3 | `sh /tmp/m1b2-mut/rig.sh` | — | 5 arms, each fail-then-restored-green (§8) |
| G4a | `make BUILD=/tmp/m1b2-build` (empty dir) | **0** | 16 TUs + the vendor object + the link, clean under `-Wall -Wextra -Werror` |
| G4b | `make transcript-diff` | **0** | **empty diff** — the M0 transcript is still byte-identical |
| — | `node gen/interpreter_c.js` | **0** | 1922 call sites, **1891 implemented, 31 unimplemented** (was 564), 1895 names |

### G1's DELTA, and its attribution

Documented baseline (M1b-1's report): **60 passed / 0 failed / 63 skipped**.
Now: **118 passed / 0 failed / 5 skipped** — **+58 / −58**.

The attribution is **by name, from the skip list itself** (every entry carried the symbol the
binary printed). The old list had 63 entries; **60 of them named a symbol this unit defined**
(`instructions_0f::instr_0F6F_mem`, `instr_F30F6F_mem`, `instr_0F28_mem`, `instr_660F28_mem`,
`instr_0F6E_mem`, `instr_660F6E_mem`, `instr_0F2A_mem`, `instr_0F2D_mem`, `instr_0F77` — one
blocking stop each for the whole `pack*`/`padd*`/`pand*`/`pcmpeq*`/`pcmpgt*`/`pmul*`/`pshufw`/
`psra*`/`psrl*`/`psll*`/`psub*`/`punpck*`/`pxor`/`movaps`/`movd`/`movq`/`movnt*`/`comiss`/
`ucomiss`/`cvt*`/`emms`/`shufps-edgecase` family). Of those 60, **58 now pass** and **2**
(`maskmovq`, `maskmovdqu`) re-entered the list as **P0 class B** — the table below. No test
regressed and none was added.

The **5** that remain are **not** this unit's and are **not** shortfalls:

| test | why |
| --- | --- |
| `jump` | the fixture's jump reaches a region v86's flat 2 MiB model maps differently — a **pre-existing** `src/cpu/cpu.c:83` assert, unchanged since M1a-4 |
| `pop_esp`, `pop_esp2` | **P0 class B**, documented in M1a-4's list: the fixture's SIGSEGV is the host MMU on a store to address 0, which v86 maps to RAM |
| `maskmovq`, `maskmovdqu` | **P0 class B, and NEW here** — see below |

**`maskmovq`/`maskmovdqu` are new class-B instances, and that is MEASURED, not assumed.**
Both expect a `#GP` at a stack address; v86's flat model maps it to RAM, so neither the port nor
the reference faults. **MEASURED**: running the corpus against the **JS reference** with
`--skip-list /dev/null` gives the port's *exact* actual values —
`maskmovdqu` eip `0x80078` / no exception, `maskmovq` eip `0x8006e` / no exception, on both legs,
byte for byte. They are on the skip list with that reason, not passed silently.

### A note on the M0 transcript gate (G4b)

It is byte-identical (MEASURED), but **nothing in this unit establishes that the M0 image
executes a single MMX/SSE encoding** — the port has no instruction trace to read. G4b is a
**non-regression** gate of unknown coverage for this unit, and is not counted as evidence in §1.

---

## 7. FINDING: the nasm corpus never compares the MMX registers

**MEASURED.** Arm M5 of the rig (§8) swaps `punpcklbw`'s two operands in the **MMX** body
`instr_0F60`. The port's own `--fixture` output changes (verified directly: mm values
`-256, 255, 255, 134744064, …` become `16777215, -16777216, -16777216, 526344, …`) — and
`make nasm` still reports **Passed 118/118**.

Root cause, **READ** from the harness and **MEASURED** on a fixture:

* `v86/tests/nasm/run.js:423` chooses the comparison branch with
  `if (fpu_tag !== FPU_TAG_ALL_INVALID)`, where `FPU_TAG_ALL_INVALID = 0xAAAA` (run.js:50) —
  and `fpu_tag` is the **fixture's** field, not the emulator's.
* `tests/mkfixture.c:138,189` builds that field the way the kernel's `convert_from_fxsr` does —
  `unsigned ftag = 0xffff0000u;` with the tag pairs OR'd into the low half — and prints
  `(int)ftag`. For a fixture whose eight slots are all "invalid" that is **−21846 (0xFFFFAAAA),
  a negative int**, which is never `===` the positive `0xAAAA`.
* So the branch that compares `mm0..mm7` is **unreachable**, the other branch runs, and its own
  guard (`expected_fpu_regs[i] != "invalid"`) skips every register because the native fixture
  reports them all `"invalid"`. Net: for these 58 tests the comparison covers `reg32`, `xmm`,
  the 8 KiB memory window and `eflags` — **and nothing else**.

This is **inherited from upstream**, not introduced by the port: `tests/nasm_run.py` is a
faithful transcription of `run.js:384-510` (it is P0's file, and this unit did not touch it).

**The experiment that bounds it, MEASURED in a scratch tree** (`tests/nasm_run.py` alone patched
to `if (fpu_tag & 0xFFFF) != FPU_TAG_ALL_INVALID:`):

| port | corrected comparison |
| --- | --- |
| **pristine** | **Passed 118/118** |
| the same M5 mutation | **Passed 117/118 — `punpcklbw` FAILS** |

So the branch is genuinely dead as shipped, *and* — once it is reachable — **the port matches the
native (real-hardware) fixture on the MMX registers across the whole corpus**. That is a
materially stronger result than the pass count alone, and it is a **one-line change to a
harness file this unit does not own**: the orchestrator's call, not mine. Suggested fix and its
evidence are in `results/m1b2-gates.txt`.

---

## 8. G3 — asymmetry (the tests can FAIL)

Rig: `/tmp/m1b2-mut/rig.sh` (raw output in `results/m1b2-gates.txt`). It runs in a **scratch copy**
of the tree and restores each mutated file from the pristine source, verifying byte-identity.

| arm | the one-line mutation | BEFORE | AFTER (restored) |
| --- | --- | --- | --- |
| M1 | the MMX transition: `fpu_set_tag_word(0)` → `fpu_set_tag_word(0xFFFF)` (emms's value) | `make selftest` **rc=2**, 2 failures (`fpu_stack_empty` 0xff vs 0x00; tag word 0xffff vs 0x1515) | rc=0, 3522/0 |
| M2 | `pmulhuw`: the lane product's `>> 16` → `>> 8` | `make selftest` **rc=2**, 1 failure (0xfe00… vs 0xfffe…) | rc=0, 3522/0 |
| M3 | `sse_integer_round`: the ties-to-even test `diff == ±0.5` → `±0.75` | `make selftest` **rc=2**, 2 failures (2.5 → 3, −2.5 → −3) | rc=0, 3522/0 |
| M4 | `punpcklbw` (66 0F 60): the two operands exchanged | `make selftest` **rc=2**, 2 failures | rc=0, 3522/0 |
| M5 | `punpcklbw` (0F 60, **MMX**): the two operands exchanged | **NOT SEEN** — see §7 | — |
| M5H | M5 again, with the corpus's dead mm-register comparison **repaired** (§7) | `make nasm` **rc=2**, **117/118** (`punpcklbw` fails); pristine port 118/118 | rc=0, 118/118, byte-identical |

**M5 is stated rather than quietly dropped**: it is a real mutation that the shipped gate cannot
see, and finding that out is what produced §7. Two arms were re-targeted while the unit was in
flight, both for the same reason and both reported: the first M1 was `instr_0F6F_reg`, which no
selftest case drove, and the first M2 targeted the **xmm** `pmulhuw`, which the selftest did not
exercise either — the **MMX** cases were added to `test_instr_0f_mmxsse` and the arms re-run.

---

## 9. What the unit did NOT prove

* **The MMX registers are not checked by the differential gate as shipped** (§7). They *are*
  checked by `test_instr_0f_mmxsse`'s own values (computed independently — the lane arithmetic,
  the clamps, the tag word), and they *are* checked by the corpus once the §7 one-liner is
  applied — but that is a harness the port does not own.
* **The M0 transcript's coverage of this unit is not established** (§6).
* **`maskmovq`/`maskmovdqu`'s fault path is not exercised end to end**: the corpus's two tests
  are class-B skips, and the selftest does not drive `get_seg_prefix_ds`+
  `writable_or_pagefault`+`safe_write8` (it would need a full segment/page fixture). The port's
  bodies are 1:1 with the source and read line by line; nothing more is claimed.
* **No encoding in this scope was checked against real hardware except through the corpus**, and
  for the correlated fields only (§7).
* **INFERRED, not measured:** that the corpus is the *only* differential evidence available here
  (the native fixtures are generated by `build/mkfixture` on this host, so "real hardware" means
  *this* host's CPU).

---

## 10. How to re-run

```sh
make                                     # G4a: the APE
make transcript-diff                     # G4b: must print an EMPTY diff
make selftest                            # G2
make nasm                                # G1 (and the census in make's own output)
python3 tests/nasm_run.py --stage run \
    --actual "build/v86c-nasm {img} {limit} --fixture" --skip-list /dev/null   # the raw list
sh /tmp/m1b2-mut/rig.sh                  # G3 + §7 (scratch-tree copy; see the script's header)
```
