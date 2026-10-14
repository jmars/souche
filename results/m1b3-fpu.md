# M1b-3 — softfloat.c, fpu.c and the 160 D8..DF entry points

Unit: M1b-3 (coder), handoff `v86c`.  Scope list: `results/m1b-scope-fpu.txt` (160 names).
Everything below is MEASURED unless marked INTERPRETATION or PROJECTION.

---

## 1. What landed

| file | lines | what it is |
| --- | --- | --- |
| `src/cpu/softfloat.h` | 191 | the engine's entry points + the F80 API's declarations + the seven F80 constants. |
| `src/cpu/softfloat.c` | 274 | the F80 wrapper: a port of `v86/src/rust/softfloat.rs` (286 lines), one C function per Rust item. |
| `src/cpu/fpu.h` | 194 | the x87 interface, incl. the **cross-unit contract**. |
| `src/cpu/fpu.c` | 1217 | the x87 implementation: `v86/src/rust/cpu/fpu.rs` (913 lines) + `misc_instr.rs:369-428` (`fxsave`/`fxrstor`). |
| `src/cpu/instructions.c` | 2353 → 2658 | **+305 lines**: the 160 D8..DF entry points, in a delimited section at the tail. No line above it was touched. |
| `src/cpu/instructions.h` | 919 → 1092 | **+173 lines**: the 160 prototypes (required — see finding F-3), plus two stale "NOT HERE" notes corrected. |
| `src/cpu/exec_boundary.c` | 262 → 224 | the three FPU accessors it seated are **deleted** (its own note required that of M1b); `fwait` **stays** (finding F-5). |
| `src/cpu/exec_boundary.h` | — | the three declarations moved to `fpu.h`. |
| `src/cpu/cpu_selftest.c` | +4 test functions | the x87 cases (gate G2) + a `#include "fpu.h"`. |
| `Makefile` | +30 lines, 3 lines amended | the vendor object, `fpu.c`/`softfloat.c` in `CPU_SRC`/`SELFTEST_SRC`, and **`-Wl,--gc-sections` on the two links** (see D-10). |
| `tests/nasm_skip_c.txt` | −26 entries | the FPU tests come off the C skip list. |

**Wrote nothing under `v86/`** (read-only, and the vendor object is compiled from it in place).
`tests/test.img` is unchanged: sha256 `1a597ac159554271a380493a19aa52ca0b65481cdb6c979c407684c9c96e737a`.

The cross-unit contract is exact:

```c
void fpu_set_tag_word(uint16_t tag_word);   /* fpu.rs:444, uint16_t per the contract */
void fxsave(int32_t addr);                  /* misc_instr.rs:369 */
void fxrstor(int32_t addr);                 /* misc_instr.rs:397 */
```

---

## 2. The F80 representation decision (the plan's open question)

**M1a-1 was right and nothing had to change.** `f80_t { uint64_t mantissa; uint16_t
sign_exponent; }` (cpu_state.h:230-240) *is* Berkeley's `extFloat80M { uint64_t signif;
uint16_t signExp; }` (softfloat.c:407, little-endian arm), 16 bytes with `sign_exponent`@8 —
both pinned by the existing `_Static_assert`s, and MEASURED again by passing
`f80_to_f64(cpu->fpu_st[i])` to the engine and getting the corpus's numbers back.

Consequences of that decision, used as designed and not worked around:

* `fpu.rs`'s `F80` values map to `f80_t` by value (`sizeof == 16`, two eightbytes, no hidden
  allocation); the engine's pointer convention is fed addresses of local copies.
* The constants: `F80_ZERO`/`F80_ONE` were already in cpu_state.h; the other seven
  (`LN_10`, `LN_2`, `PI`, `LOG2_E`, `INDEFINITE_NAN`, `POS_INFINITY`, `NEG_INFINITY`) are in
  softfloat.h beside the arithmetic they feed.

---

## 3. THE ENGINE IS NOT THE RUST FILE (finding F-1, the unit's real shape)

The brief says `softfloat.c` is "port `v86/src/rust/softfloat.rs` (286 lines): the 80-bit
extended arithmetic core".  READ against the artifact, that file contains **no arithmetic**:
it is a thin wrapper whose `extF80M_*` calls are `extern "C"` (softfloat.rs:1-32).  The
arithmetic is **Berkeley SoftFloat Release 3e**, vendored READ-ONLY at
`v86/lib/softfloat/softfloat.c` — a **32501-line** amalgamation of the 8086-SSE
specialization, and the object `v86/Makefile:242-248` links into the wasm.

So this unit has two pieces, and the report names them separately because they fail
differently:

1. **the engine**, reused verbatim: compiled from the read-only tree with **upstream's own
   defines** (`-DSOFTFLOAT_FAST_INT64 -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16
   -DSOFTFLOAT_FAST_DIV64TO32`) so the reference's fast-int64 paths are the ones that run.
   MEASURED: it compiles clean under cosmocc, 146640-byte object, no warnings.
2. **the wrapper**, `src/cpu/softfloat.c`: softfloat.rs's `impl F80` item by item — the
   conversions, the seven operators, the comparisons, the rounding/precision setters, and
   the exception-flag translation.

**The engine does not link on ELF as shipped** (finding F-6, and the only Makefile surgery
this unit needed).  Four undefined references:

```
softfloat.c:(.text+0x3f15): undefined reference to `softfloat_shiftRightJam128Extra'
softfloat.c:(.text+0x42d9/.text+0x4396): the same, from softfloat_addMagsF128
softfloat.c:(.text+0x4ef7): undefined reference to `softfloat_shiftRightJam256M'
```

Root cause, MEASURED in the amalgamation: primitives are emitted with the `#ifndef X /
#define X X` idiom, and for exactly these two the `#define` line is **not** commented out
(softfloat.c:1792-1793, :1816-1817) while `softfloat_shiftRightJam128`'s *is*
(softfloat.c:1777).  So their definitions at :4504/:4612 are skipped and no definition is
emitted at any `INLINE_LEVEL`.  They are reachable **only** from `float128` routines
(`softfloat_roundPackToF128`, `softfloat_addMagsF128`, `softfloat_mulAddF128`), which no
`extF80`/`f32`/`f64` path — and nothing this port calls — can reach.  Upstream never sees
the problem because the vendor object is compiled with `-ffunction-sections -fdata-sections`
into a **wasm** linked with GC; on ELF the same needs `-ffunction-sections
-fdata-sections` on the object plus `--gc-sections` on the link.  Applied; the four F128
functions are then simply not in the binary (INTERPRETATION: they are dead in the reference
too, by the same reachability argument).

---

## 4. Divergences from upstream (every one, with its reason)

| # | Where | Divergence | Why |
| --- | --- | --- | --- |
| D-1 | `softfloat.c` | the arithmetic is the **vendored amalgamation compiled as its own object**, not re-derived C | it is the reference's own arithmetic; re-writing 32k lines of SoftFloat would be a new engine, and the brief asks for a port. See §3. |
| D-2 | `softfloat.h` | the SmallFloat entry points are declared with the **plain integer types** (`uint32_t`/`uint64_t`), not their `struct { uint32_t v; }` / `struct { uint64_t v; }` (softfloat.c:397-400) | a single-member uint struct and the corresponding unsigned integer are passed and returned in the **same register** in the SysV amd64 and AAPCS64 ABIs — which is exactly the licence upstream's own Rust FFI takes (`fn f32_to_extF80M(src: i32)`, softfloat.rs:24-27). Declaring two differently-tagged structs across TUs is the same class of UB with none of the clarity, so the port follows upstream and **MEASURES** the assumption instead: `test_fpu_softfloat_roundtrip` round-trips 8 f32 and 8 f64 bit patterns (incl. subnormals, infinities, a quiet NaN and ±0). |
| D-3 | `softfloat.c`/`fpu.c` | the Rust *methods* `F80::to_f32`/`to_f64` are named `f80_convert_to_f32`/`f80_convert_to_f64` | C has one namespace, and `fpu.rs:104-116` defines `#[no_mangle] fn f80_to_f32`/`f80_to_f64` (the status-word-raising wrappers, part of upstream's wasm surface). The names collide; the **`#[no_mangle]` ones keep the upstream spelling** and the methods take `_convert_`. |
| D-4 | `fpu.c` | `fpu_set_tag_word` takes `uint16_t`, not the Rust's `i32` | the M1b cross-unit contract fixes the signature (every caller reads a 16-bit word: `fldenv16/32`, `fxrstor`). |
| D-5 | `fpu.c` `fpu_unimpl` | `dbg_assert!(false)` is **dropped**; only `trigger_ud()` is ported | `dbg_assert!` is `debug_assert!` (dbg.rs:63), compiled out of the released reference — so the reference **delivers #UD** there and a live C `assert` would be a divergence from it, not a stricter check. |
| D-6 | `fpu.c` | `dbg_log!`/`dbg_trace!` dropped; `fpu_frstor16`/`fsave16`'s log lines become `(void)addr` | M1a-2's D19 (no log channel). |
| D-7 | `fpu.c` | upstream's `.unwrap()` at caller-pre-checked buffers keeps the call-and-use shape; `DBG_ASSERT(!cpu->pagefault)` is added at the one site whose **name** claims it (`fpu_fldm80_without_fault`) | upstream panics there (`.unwrap()`), a release C build would push a sentinel — neither is reachable through a checker, and M1a-2's D18 records this convention. Stated once in `fpu.h` rather than repeated at eleven sites. |
| D-8 | `fpu.c` `fpu_fprem` | the `intel_compatibility = false` **dead second arm is kept**, and the Rust `match` on `Option<Ordering>` is a switch on `F80_CMP_*` | deleting upstream's dead arm hides that it exists; the `Option` becomes an `int` because C has no sum type (the GT arm is empty exactly where upstream's `Some(Greater) => {}` is). |
| D-9 | `softfloat.c` | `f80_of_f64x` is **public** (private in the Rust) | `fpu.rs:142/493` build F80 constants with `F80::of_f64(f64::to_bits(v))`; exporting the one helper is smaller than duplicating it. |
| D-10 | `Makefile` | `SF_OBJ` (a new, **unwarned** rule for third-party C) and `-Wl,--gc-sections` on **both** link lines | §3. This is the only change this unit makes that affects a line another unit also uses; it is additive (a flag) and needed by both binaries, `build/v86c` and `cpu_selftest`. |
| D-11 | `exec_boundary.c` → `fpu.c` | `fpu_load_status_word`, `fpu_load_tag_word`, `fpu_get_sti_f64` **moved**; and the seat's byte-comparison placeholder for `is_finite` is replaced by the real `f80_eq`-based test | the seat's own comment required it ("M1b must DELETE all three from here when fpu.c lands"). The placeholder was exact only for the F80 zero; the port is exact for every value. |

**Divergences from M1a-1's `f80_t`: NONE.** The representation was read first, verified against
`extFloat80M`, and used as chosen (§2). No change was needed or made.

---

## 5. Findings on the brief and the plan (F-1..F-7)

* **F-1** — §3: `softfloat.rs` is a wrapper; the arithmetic is the vendored 32501-line
  SoftFloat. The brief's line count describes the smaller half of the work.
* **F-2** — the brief's `fpu.c` scope (fpu.rs + `fxsave`/`fxrstor`) is right, and M1a-2's
  deferral is exactly as its report says.
* **F-3** — **`instructions.h` had no declarations for the 160 names, and the generated
  dispatcher includes `instructions.h`.** In C11/-Werror an implicit declaration is an
  error, so declaring them was a hard requirement of landing the group, not a nicety; the
  brief does not mention it. 160 prototypes added (cross-checked: all 160 scope names are
  defined in `instructions.c` and declared in `instructions.h` — MEASURED, 160/160 both).
* **F-4** — **The R1 init-value parity claim is not checkable at the end of the suite.**
  M1b-1's 0F AE test (`cpu_selftest.c:4717`) drives `fxrstor`, which calls `set_control_word`
  — so by the time any FPU test runs, `softfloat_roundingMode` is whatever the buffer at
  0x2000 happened to hold (MEASURED: `2`, not the compiled-in `0`). The check was moved to
  the **first statement after `cpu_state_init()` in `main()`**, which is the only point at
  which it reads the compiled-in values. This is a cross-unit ordering fact, worth knowing
  before anyone else writes a "defaults" assertion in that file.
* **F-5** — **`fwait`'s ownership.** `instructions.h:78` says fpu.c owns it (`fpu.rs:803`).
  Moving it means editing cpu_selftest.c's counting 9B stub and the M1a-3 test that reads
  it — a middle-of-file edit to a file another unit owns concurrently this pass. Upstream's
  body is **empty**, so the seat in `exec_boundary.c` is bit-identical to fpu.c's
  definition; it stays, and the file records why. **This is a labelling gap, not a
  functional one** — flagged here for the unit that next owns that file.
* **F-6** — §3: the amalgamation does not link on ELF as shipped; four undefined refs.
* **F-7** — `tests/nasm_skip_c.txt` is **shared** by this unit and M1b-1 (both are told to
  regenerate it). Only the 26 FPU lines were removed, by name; the 0F entries were left
  alone. Concurrently, M1b-1 also regenerated the file (76 → 63 skips between two of my
  runs), so the counts in §6 have a timestamp.

---

## 6. GATES

Raw output: `results/m1b3-gates.txt`. Mutations: `/tmp` is gone, so the transcript is
inlined in §6.3 (it was captured verbatim).

### G1 `make nasm` — the FPU tests off the C skip list

```
[+] Passed 47/47 tests.
[!] 0 of 123 selected test(s) produced no state from the actual leg, 76 were skipped (tests/nasm_skip_c.txt).
RC=0
```

**DELTA against the 21/21 + 102 baseline:** `21/21 passed, 102 skipped` → **`47/47 passed, 76
skipped`**. Exactly **26 tests came off the list and every one passes**: `f2xm1 fchs fdecstp
fdiv fdiv-zero fdivr fincstp fprem fprem-distinct fprem1 fptan fpu_m80 frndint fsave_frstor
fscale fsincos fsqrt fst-st-empty-tag fstenv fstm80 fstsw fxtract fxtract-zero fyl2x
fyl2x-zero fyl2xp1`. Each was previously skipped for `unimplemented instructions::instr…D9/D9/D8/DB/DF…`.

**Every one of the plan §5 target tests passes**: `fpu_m80`, `fstm80`, `fdiv-zero`,
`fsave_frstor`, `fstenv`, `fstsw`, `f2xm1`, `fyl2x` (`fyl2x`, `fyl2x-zero`, `fyl2xp1`),
`fprem` (+ `fprem1`, `fprem-distinct`), `fsqrt`, `frndint`, `fscale`, `fxtract`
(+ `fxtract-zero`), `fptan`, `fsincos` — plus `fchs`, `fdecstp`, `fincstp`,
`fst-st-empty-tag`.

**The still-failing list from the FPU group: EMPTY.** For completeness, a full
`--skip-list /dev/null` run (123 tests) found **no FPU failure**; its 63 failures are:

* **61 × no state — `unimplemented instructions_0f::…`** (the MMX/SSE 0F encodings, M1b-1's
  scope; e.g. `pxor`/`paddb`/`movq` stop at `instr_0F6F_mem`). **Neither a tolerance nor a
  port gap in this unit** — another unit's unimplemented opcodes.
* **2 × `pop_esp`, `pop_esp2`** — the **pre-existing P0 class-B gap**: the native fixture
  expects a `#GP` from a store to address 0, which v86 maps to plain guest RAM (the
  reference has the same gap). Not a port defect, not a tolerance.

No test failed for a tolerance reason, and no test was papered over.

### G2 `make selftest` — new x87 cases

```
cpu_selftest: 3349 checks, 0 failures
RC=0
```

Four new test functions (`test_fpu_words_and_stack`, `test_fpu_softfloat_roundtrip`,
`test_fpu_env_save_restore`, `test_fpu_rounding_and_status`) covering: the tag word
(`set`/`load` round trips, the "only 3 means empty" rule), TOP, the status word's TOP
re-application, push/pop direction, **both stack-fault paths** (read of an empty slot →
C1 cleared + SF|I + indefinite NaN; push onto a full stack → C1 set + SF|I), the f32/f64/
i32/i64 round trips (the D-2 ABI proof), the F80 constants against their doubles, the
operators, `fstenv32`/`fldenv32`, `fsave32`/`frstor32` (incl. `fsave`'s implicit `finit`),
`FXSAVE`/`FXRSTOR` (the full 512-byte image incl. xmm and mxcsr, and the `#gp` on an
invalid mxcsr), all four rounding modes checked **functionally** (ceil(2.5)=3, floor(−2.5)=−3),
the precision control (1.0+2⁻³⁰ is exact at P80 and rounds to 1.0 at P32), `fcom`/`fcomi`/
`fucomi`'s result flags, `ftst`/`fxam`, the ZE fault, the integer stores (round vs truncate,
and `#I` on an out-of-range store), `fbstp`'s digit order and sign byte, `fprem`'s
quotient bits, `fsqrt` and `f2xm1`.

### G3 ASYMMETRY — 7 single-line mutations, each failing then restored green

Restoration verified by `diff -q` against the pre-mutation copies (**ALL RESTORED
IDENTICAL**), after which every gate was re-run green.

| # | file | mutation | instrument | result |
| --- | --- | --- | --- | --- |
| M1 | `fpu.c` | `fpu_push`: `(sp - 1) & 7` → `(sp + 1) & 7` | selftest | **FAILS, 14 failures** (e.g. `:4973 fpu_stack_ptr == 7 (got 0x1)`) |
| M2 | `fpu.c` | `fpu_set_tag_word`: `& 3 == 3` → `& 3 != 0` | selftest | **FAILS, 1** (`:4949 stack_empty == 0x00 (got 0x1)`) |
| M3 | `fpu.c` | `set_control_word`: `cw >> 10 & 3` → `& 1` | selftest | **FAILS, 4** (ceil/floor modes collapse) |
| M4 | `fpu.c` | `fpu_load_status_word`: `<< 11` → `<< 10` | selftest | **FAILS, 12** (TOP in the wrong bits, incl. the fstenv image) |
| M5 | `softfloat.c` | the flag translation `f >> 1 & 4` → `& 8` | selftest | **FAILS, 1** (`:5301 status & FPU_EX_Z`) |
| M6 | `fpu.c` | `fxrstor`: `~safe_read8(addr+4)` → `safe_read8(…)` | selftest | **FAILS, 2** (`:5190 stack_empty == 0x1F (got 0xe0)`) |
| M7 | `instructions.c` | `instr_D8_6_reg`: `fpu_fdiv` → `fpu_fmul` | **`make nasm`, filter `^fdiv$`** | **FAILS, `Passed 0/1`, `Failed 1`** |

**M7 is the one that matters for the 160 entry points, and it is an honest coverage
statement:** `make selftest` does **not** execute the D8..DF map (no selftest case calls
those functions), so M7 is caught by **G1**, the nasm differential oracle against real
hardware — not by G2. A mutation the test cannot see proves nothing, so the instrument is
named: **fpu.c's semantics are pinned by G2; the opcode→operation map is pinned by G1.**

### G4 `make` clean under `-Wall -Wextra -Werror`; the M0 transcript still empty

`make` RC=0 — and re-proved from scratch for this unit's objects
(`rm build/cpu/{fpu,softfloat,instructions}.o build/sf/softfloat.o` then `make`), which
recompiles `instructions.c`, `softfloat.c`, `fpu.c` under `-Wall -Wextra -Werror` with zero
output beyond the command lines, compiles the vendor amalgamation, and links the APE
(`MZqFpD` signature, runs RC=0).

`make transcript-diff` → `milestone: the C transcript is byte-identical to the reference's`
(RC=0). `make oracle` → `transcript matches tests/model.py (8 GPRs, EFLAGS, EIP, 561
instructions, 4096-byte window sha256)` (RC=0).

---

## 7. Coverage limits and open items (stated, not hidden)

* The **tag word's "zero" vs "valid" distinction** is not round-tripped by `fstenv`/
  `fldenv`, because upstream's model has one empty bit per slot and `fpu_set_tag_word` only
  reads the `== 3` case — both the reference and the port lose it. Asserted, not "fixed".
* The **`fprem` `intel_compatibility` arm** is dead code in the reference
  (`intel_compatibility = false`); the port keeps it and no test can reach it.
* **`fwait` remains in the `exec_boundary.c` seat** (F-5) — a labelling gap for the unit
  that next owns that file.
* **`tests/nasm_skip_c.txt` is shared** with M1b-1 (F-7); this unit removed only the 26 FPU
  lines.
* The `Makefile` now carries `-Wl,--gc-sections` on both link lines (D-10) — additive, but
  it is a shared line and the other unit editing that file should know it is deliberate.
