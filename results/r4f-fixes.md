# R4F — the MMX comparison the oracle could not make, and R4's N1/N2/N3

**Unit:** `HANDOFF=v86c`, unit R4F (coder). **Mandate:** land the repair that makes the nasm
differential runner's MMX-register comparison able to fail (the load-bearing item, G0), and
close `results/r4-review.md`'s three nice-to-haves (N1 citations, N2 softfloat types, N3 the
guest-reachable assert in fxsave/fxrstor).

**Verdict.** All four landed. The oracle's MMX branch is now **reachable and able to report a
mismatch** — MEASURED by asymmetry, not asserted: a one-line MMX mutation that the shipped
gate passed at **118/118** now FAILS at **117/118** naming `mm0.int32[1]` / `mm1.int32[0]`,
and the pristine port is still **118/118**. N3 is resolved the way R3's twin was (the check
dropped at its own site, never `-DNDEBUG`), MEASURED by a guest probe that aborted the
reviewed pre-fix binary (**rc=134**) and now executes with a transcript **byte-identical to
the pinned reference's**. N1's 73 stale citations are corrected and re-derivable from `./v86`
by a script that reproduces the corrected file byte-for-byte from the artifact R4 reviewed.
N2's two typedefs remove the formally-UB integer redeclarations — **at four call sites, not
zero** (the brief's/review's "zero cost" claim is wrong on contact with the code; see §3).

Every claim below is **MEASURED** (raw output: `results/r4f-gates.txt`), **READ** (from the
pinned clone `./v86`) or **INFERRED**, marked as such.

| gate | what | result |
| --- | --- | --- |
| G0/G1 | the MMX mutation on the repaired oracle | **FAILS 117/118**, names the mm fields; restored → 118/118 |
| G0/G2 | the branch is reachable | run.js's predicate: **0** tests take it; repaired: **7**; the gate itself prints `[mm-branch]` for each |
| G0 corpus | pristine port with the repair | **118/118, 0 failed, 5 skipped** (baseline 118/118, 5 skipped → **delta 0**) |
| G3 | `make`, `make selftest`, `make nasm`, `make nasm-js`, `make transcript-diff` | all **rc=0**; 3522 checks/0 failures; 118/118; 113/113; **empty diff** |
| G4 N1 | citations | 73 corrected; post-state **0 mispointed** (`results/r4f-cites.py`), reproducible byte-for-byte |
| G4 N2 | the SmallFloat declarations | now the engine's own struct types; header + 4 call sites build clean; gates unchanged |
| G4 N3 | the guest probe | pre-fix **rc=134 (SIGABRT)** at both sites → **rc=0**, transcript == reference; no count moved |

Files changed: `tests/nasm_run.py` (G0), `src/cpu/instructions_0f.c` (N1),
`src/cpu/softfloat.h` + **`src/cpu/softfloat.c`** (N2 — the second one is outside the brief's
file list, reported in §3), `src/cpu/fpu.c` (N3). `tests/nasm_skip_c.txt` is **unchanged** (no count moved). `v86/` was
never written (READ-ONLY). No commit.

Artifacts: `results/r4f-gates.txt` (every gate's raw output and exit code),
`results/r4f-g0.sh`, `results/r4f-branch-count.py` (G1/G2),
`results/r4f-probe.asm` + `results/r4f-n3.sh` (N3), `results/r4f-cites.py` (the citation
invariant) and `results/r4f-cites-fix.py` (the corrections as data),
`results/r4f-fpu-prefix.c.gz` + `results/r4f-instructions_0f-prefix.c.gz` (the two reviewed
pre-fix artifacts, so both "before" sides are rebuilt from the reviewed file, not recalled).

---

## 1. G0 — the oracle's dead MMX branch

### 1.1 What was wrong, and which side is right

`tests/nasm_run.py` chose its comparison branch with upstream's own test
(`v86/tests/nasm/run.js:423`):

```python
if fpu_tag != FPU_TAG_ALL_INVALID:      # FPU_TAG_ALL_INVALID = 0xAAAA
```

`fpu_tag` is the **fixture's** field, printed by the native harness the way upstream's
gdb-extract-def prints it — signed `%d` (`tests/mkfixture.c:189`,
`v86/tests/nasm/gdb-extract-def:138`). The value it prints is `0xFFFF____`: the **low 16 bits
are the real abridged tag word** and the high half is the kernel's `convert_from_fxsr`
`| 0xffff0000` (mkfixture.c:138, a measured host-kernel quirk documented since P0). For an
all-empty tag word that is **−21846 (0xFFFFAAAA)** — a negative int that is never `===` the
positive 16-bit `0xAAAA`. So the `else` (mm) arm was **unreachable**, every test took the
`st` arm, and that arm's own `!= "invalid"` guard then skipped all eight registers, because a
fixture with an all-empty x87 stack reports them all `"invalid"`. **Net: mm0..mm7 were
compared by nothing.** MEASURED consequence (M1b-2's finding 2): `punpcklbw`'s MMX operands
swapped passed the whole corpus unchanged.

**I changed the COMPARISON side, not the fixture.** The fixture is right: its low half *is*
the tag word the emulator is being asked to reproduce, and its high half is a measured,
documented property of the kernel's FXSAVE→struct conversion — the same thing gdb's own
`$ftag` carries upstream (READ: `gdb-extract-def:138` prints `$ftag` with `%d`, so upstream
inherits the same dead branch). The defect is that the comparison reads a 32-bit signed value
and compares it against a **16-bit code**: `FPU_TAG_ALL_INVALID` = eight 2-bit "empty" slots,
i.e. a property of the tag word's low half. The repair masks the field to the 16 bits the
constant is defined over:

```python
if (fpu_tag & 0xFFFF) != FPU_TAG_ALL_INVALID:
```

That is upstream's intent restored (the mm arm is taken **exactly** when all eight x87 slots
are empty), it is not a loosening (the branch can and does report mismatches — §1.2), and the
pristine port still passes the corpus (§1.3).

### 1.2 G1 — the asymmetry (the proof)

Rig: `sh results/r4f-g0.sh`. One mutation, applied ALONE to the MMX body of `instr_0F60`
(the same arm M1b-2 used as its arm M5): the two operands exchanged.

```
  -  result.u8[2*i+0] = destination.u8[i];  result.u8[2*i+1] = src.u8[i];
  +  result.u8[2*i+0] = src.u8[i];          result.u8[2*i+1] = destination.u8[i];
```

| oracle | the mutation | pristine |
| --- | --- | --- |
| as shipped (M1b-2's arm M5) | **NOT SEEN** — the whole corpus passed | 118/118 |
| **repaired (this unit)** | **rc=2, 117/118**, `punpcklbw` FAILS: `mm0.int32[1]` actual `0xffffff` vs expected `0xffffff00`; `mm1.int32[0]` actual `0xff000000` vs expected `0xff` | **118/118** |

The restored file is byte-identical (`sha256` checked by the rig) and the corpus returns to
118/118. A comparison that never ran cannot name `mm0.int32[1]`: **this is the branch
evaluating.**

### 1.3 G2 — reachability, measured two ways

1. **Over the real fixtures** (`results/r4f-branch-count.py`, which imports the runner so
   there is one definition of `FPU_TAG_ALL_INVALID`, and evaluates BOTH predicates on every
   fixture the gate reads):

   | predicate | tests taking the mm branch |
   | --- | --- |
   | `fpu_tag != FPU_TAG_ALL_INVALID` (run.js:423) | **0** |
   | `(fpu_tag & 0xFFFF) != FPU_TAG_ALL_INVALID` (repaired) | **7** |

2. **Inside the gate** — an instrumented copy of `tests/nasm_run.py` (print at the mm arm,
   `/tmp`, tree untouched) run over the C leg prints one line per test that reaches it:
   `cvtps2pi`, `packuswb`, `pcmpgtb`, `punpcklbw`, `punpckldq`, `punpcklwd`, `pxor` —
   and still `Passed 118/118`.

### 1.4 The corpus does not go red, and the counts do not move

`make nasm` → **rc=0, 118/118, 0 produced no state, 5 skipped** — identical to the
pre-repair baseline (118/118, 5 skipped). The reference leg, with the same repaired
comparison, is also unchanged: `make nasm-js` → **rc=0, 113/113, 10 documented skips**. So
both legs *already satisfied* the mm comparison; the repair added a check, it did not relax
one. `tests/nasm_skip_c.txt` is consequently **not regenerated** — sha256 still
`48cf4cc5…9bb3ca`, and its five reasons (jump, maskmov*, pop_esp*) are untouched. **Delta: 0.**

### 1.5 The limit of this repair, stated rather than implied

Which tests get the mm comparison is decided by the fixture's tag word — upstream's design,
kept. Of the corpus's MMX-family tests only the 7 above have an all-empty x87 stack; the
others (psubb, punpckhbw, packssdw, …) carry slots the tag word marks non-empty, so they take
the `st` arm, where only the slots reported as values are compared and a write to a slot
marked "invalid" stays invisible — that is M1b-2's §7 note, and it is unchanged by this unit.
The repair closes the **dead branch**; it does not make the corpus cover every MMX register
in every test. Anyone reading "the MMX group is now verified" should read this paragraph.

---

## 2. N1 — the stale upstream citations in `src/cpu/instructions_0f.c`

R4 measured eight examples; re-deriving every one of the file's 191 citations against the
pinned clone shows the class is bigger and has a shape: **73 were wrong**, and the errors are
not typos — they are whole-region offsets.

* **before ~line 320: drift +81**, then **+320**, **+639/640**, then **+2189..+2382**
  (`cpuid` cited `1064-1228`; `instr_0FA2` is at **3256-3419** — the citation pointed at
  `cvtsd2si`), and one **+3640** (`0F FF` "Windows 98" cited `1656-1659`, which is inside
  `instr_0F52`; the real site is **5296-5300**).
* **in the MMX/SSE half: one-opcode slips** — e.g. `packsswb` cited `2290-2319`, which is
  `punpckldq`'s span, while `instr_0F63` is at **2321-2352**; and the whole `0F F1..F7` and
  `0F F8..FE` families were each cited one instruction late (`psllq` cited `4953-4977` =
  `pmuludq`'s neighbourhood; real **4935-4952**).
* **and seven that start inside the *following* instruction** (`movlpd` cited `557-562`,
  real `547-552`; `pslld` cited `4934-4952`, real `4916-4933`; the `2693?-...` uncertainty
  marker for `movdqu`, resolved: `instr_F30F6F` is at 2685-2692).

**How the fix is made reproducible.** The corrections are **data**: each entry names the
upstream function(s) the citation heads, and the script derives the line range from
`./v86` (first `pub unsafe fn` line .. the function's closing brace). No number is typed in.

```sh
python3 results/r4f-cites-fix.py --list       # the 73 entries and the ranges they derive
python3 results/r4f-cites-fix.py --verify-prefix results/r4f-instructions_0f-prefix.c.gz
#   applied to ... 73 citation(s) rewritten, 0 already correct
#   result is BYTE-IDENTICAL to the corrected src/cpu/instructions_0f.c
```

That `--verify-prefix` run **is** the asymmetry: the stored `.gz` is the file R4 reviewed
(sha256 `154dea68bb7f114a1e0218b96ef06cedf3ab52b4971380fd98792b36489638fe` — the sha this
tree carried when the unit started), and the table turns it into the current file exactly.

**The post-state, checked rather than claimed** (`results/r4f-cites.py`, which re-derives
every citation from the clone and needs no trust in this report):

| | count |
| --- | --- |
| citations whose start is **inside** the upstream span of the code beneath them | **107** |
| start in the ≤12-line preamble just above it — the file's (correct) habit of citing a shared helper or a `#[no_mangle]` line (`lmsw` cites the `lmsw` helper at 282-288, whose ported callers are at 291-309) | 70 |
| **MISPOINTED** (>12 lines from the code they head) | **0** |

The edit is comment-text only, proven mechanically: the comment-stripped source of the
pre-fix artifact and of the corrected file are **identical** (`results/r4f-gates.txt`, same
6013 lines). R4's note that "the sgdt/sidt/lgdt/lidt citations ARE right" holds under the
checker (they land inside the `0F 01` block, 196-326).

**One place where I did not follow R4's figure, and why.** R4 wrote "popcnt cited
`1489-1492`, real ~3681". The comment at that site reads `0F B9 … -- UD2` and the definition
directly beneath it is `instr_0FB9`, whose body is at **3697-3700** (MEASURED); the popcnt
definitions (`instr16_F30FB8_mem` 3684-3686, `instr32_F30FB8_reg` 3695…) sit just *above* it
in the same C region and carry no citation of their own, which is what the review's hedge was
tracking. The citation now points at the code its own comment names. (Not a contradiction of
the review — R4 marked it "~" — but the difference is recorded rather than smoothed over.)

**Finding on the review, for the record:** R4's examples were accurate but its taxonomy was
not — it called the citations "systematically stale" where 118 of the file's 191 citations
were already within a few lines of the code they head, and the staleness is confined to a
whole-region offset in one part of the file plus one-opcode slips in two MMX families. The
prescription (re-derive, do not delete) was right on contact.

---

## 3. N2 — the SoftFloat declarations: two typedefs, four call sites

R4: "typedef the two single-member structs in softfloat.h and use them in the six
declarations; **every call site stays textually identical**."

The diagnosis is right, the second half is **not**. READ: the engine defines
`void f32_to_extF80M( float32_t, extFloat80_t * )` and `float32_t extF80M_to_f32(...)`
(`v86/lib/softfloat/softfloat.c:566, :601, :665, :666`) while `softfloat.h` declared them over
`uint32_t`/`uint64_t` — a cross-TU incompatible-type redeclaration, formally UB. But:

* **A struct-typed prototype cannot be satisfied by an integer argument** in C; declaring the
  parameter as `float32_t` forces the four call sites in `src/cpu/softfloat.c` to say what
  they mean (`f32_to_extF80M((float32_t){ src }, &x)`; `extF80M_to_f32(&x).v`). The review's
  "zero cost" is a zero *ABI* cost and a four-line *source* cost.
* **It is four declarations, not six**: `i32_to_extF80M`/`i64_to_extF80M` already take plain
  `int32_t`/`int64_t` in the engine (`softfloat.c:502, :511`); only the f32/f64 ones cross the
  SmallFloat struct boundary.

Done: the two typedefs are declared in `softfloat.h` with the engine's own names and a
comment naming the engine's lines, the four declarations now name them, and the four call
sites construct/read the struct. **`src/cpu/softfloat.c` is nonetheless outside the brief's
file list** — reported as a finding: the brief inherited the review's "call sites unchanged"
premise, and the real fix cannot keep it.

**What is NOT removed, and why** (so this is not read as "the UB is gone"): the *pointer*
arguments still pass `f80_t *` where the engine takes `const extFloat80_t *`, i.e. the same
formally-UB class of distinct-type pointers to layout-identical structs. Removing that means
aliasing `f80_t` to `struct extFloat80M`, which lives in `cpu_state.h` (outside this unit's
scope and past this fix's blast radius). Stated, not hidden.

**There is no red/green probe for N2, and saying so is the honest answer**: the two spellings
are ABI-identical by construction (INTEGER class, one register, on SysV amd64 and AAPCS64),
which is *why* the compiler and the linker are both silent about the UB. The evidence is
therefore (i) textual — the declarations now name the same types the engine's own prototypes
do (both are printed side by side in `results/r4f-gates.txt`) — and (ii) the unchanged gates:
the from-scratch compile under `-Wall -Wextra -Werror` is clean, the selftest's f32/f64
round-trip "ABI proof" case passes (3522 checks, 0 failures), and the 47 nasm FPU tests +
the M0 transcript diff are unchanged.

---

## 4. N3 — the guest-reachable `DBG_ASSERT` in fxsave/fxrstor

**The class** (R4's N3): `misc_instr.rs:370/398` guard both instructions with
`dbg_assert!(addr & 0xF == 0, "TODO: #gp")` — a *debug* assert, COMPILED OUT of the pinned
release reference, which therefore just executes at a misaligned operand (real hardware would
`#gp`; upstream's own TODO says so). The port emitted it as a **live** `DBG_ASSERT`, so
ordinary guest bytes — `fxsave [eax]` with `eax & 0xF != 0` — aborted the machine where the
reference writes.

**Resolution, made consistent with R3's twin.** R3's N1 was the prefix-fallthrough assert;
the emitter resolved it *at the site* by not emitting the check (`gen/interpreter_c.js`,
"C-side difference 3"), explicitly **not** by `-DNDEBUG`. The reasoning transfers verbatim:
the condition is a property of the **guest's input**, not an invariant of the port, and the
shipped build must match the release reference there — while `-DNDEBUG` would silence the
port's own invariants (this is the same tree whose `make nasm` `jump` test ends on
`src/cpu/cpu.c:83`'s assert). So the two asserts are dropped **at their own sites** and
replaced by the reasoning; every internal invariant keeps its `DBG_ASSERT`.

### G4 — the probe

Rig: `sh results/r4f-n3.sh`. The guest: `results/r4f-probe.asm`, four modes assembled into
flat images (MODE=1 misaligned `fxsave`, MODE=3 misaligned `fxrstor`, MODE=2 the aligned
control, MODE=0 no fxsave), run on three artifacts — the **reviewed pre-fix binary rebuilt
from `results/r4f-fpu-prefix.c.gz`** (sha256 checked against R4's own recorded sha
`5987fa5c…42a096`), the shipped binary, and the pinned JS reference.

| mode | BEFORE (reviewed pre-fix fpu.c) | AFTER (the tree) | AFTER vs reference |
| --- | --- | --- | --- |
| 0 — no fxsave | rc=0, 4 instructions | rc=0, 4 instructions | **IDENTICAL** |
| 1 — misaligned `fxsave` | **rc=134 (SIGABRT)**, `fpu.prefix.c:1152: assert((addr & 0xF) == 0) failed`, **no transcript** | rc=0, 6 instructions, `eip=0x00100034` | **IDENTICAL** |
| 2 — aligned `fxsave` (control) | rc=0 | rc=0, 6 instructions | **IDENTICAL** |
| 3 — misaligned `fxrstor` | **rc=134 (SIGABRT)**, `fpu.prefix.c:1186: assert(...) failed` | rc=0, 6 instructions | **IDENTICAL** |

The guest reaches the store *after* the sequence (eip past it, same instruction count on both
legs) and the C transcript is byte-identical to the reference's in all four modes: the port
now does at a misaligned operand exactly what the release reference does.

**No other stop was silenced — stated as the observable, not as intent.** `make nasm` is
unchanged at **118/118, 5 skipped**, and the `jump` skip reason still reads
`src/cpu/cpu.c:83: assert(!in_mapped_range(phys_addr)) failed` — i.e. the stop the corpus
depends on is still live. A global `-DNDEBUG` would have moved that count.

---

## 5. Gates (exit codes; raw output in `results/r4f-gates.txt`)

| gate | command | rc | result |
| --- | --- | --- | --- |
| G3a | `rm -rf /tmp/r4f-build && make BUILD=/tmp/r4f-build` | **0** | 16 TUs + the vendor object + the link, clean under `-Wall -Wextra -Werror` |
| G3b | `make selftest` | **0** | **3522 checks, 0 failures** (unchanged) |
| G3c | `make nasm` | **0** | **118/118, 0 produced no state, 5 skipped** (unchanged) |
| G3c′ | `make nasm-js` | **0** | **113/113, 10 skipped** (unchanged) |
| G3d | `make transcript-diff` | **0** | **EMPTY diff** — the M0 transcript is still byte-identical |
| — | `node gen/interpreter_c.js` | **0** | 1922 call sites, 1891 implemented, **31 unimplemented** (unchanged) |
| G1 | `sh results/r4f-g0.sh` | 0 | mutation **117/118** naming `mm0/mm1`; restored 118/118 |
| G2 | `results/r4f-branch-count.py` + an instrumented gate | 0 | **0 → 7** tests take the mm branch; the gate prints `[mm-branch]` × 7 |
| G4-N1 | `results/r4f-cites.py` / `--verify-prefix` | **0** | 0 mispointed; the pre-fix artifact reproduces the corrected file **byte-for-byte** |
| G4-N2 | the from-scratch build (`-Werror`) | **0** | the header declares the engine's own struct types; selftest ABI case passes |
| G4-N3 | `sh results/r4f-n3.sh` | 0 | **134 → 0** at both sites, transcript == reference; counts unchanged |

---

## 6. What this unit did NOT prove

* **That the corpus now verifies the MMX registers across the MMX group** — it verifies them
  for the 7 tests whose fixture tag word is all-empty, which is upstream's branch design; §1.5
  is the honest boundary.
* **That `fpu.c`/`softfloat.h` were otherwise sound** — this unit changed three sites and R4's
  read stands as the review; nothing here re-reviews the files.
* **That no *other* guest-reachable `DBG_ASSERT` exists** in the tree. N3 was the one R4
  found; the same probe method (a guest that trips it) has not been run over the rest. This
  is a coverage statement, not a claim of absence.
* **That the misaligned-operand behaviour matches real hardware** — it matches the pinned
  release reference, which WRITES where hardware `#gp`s (upstream's own TODO). The port's
  choice was to match the reference; if the project ever wants hardware behaviour here, that
  is a deliberate divergence to make, not a bug this unit closed.
* **N2's pointer-type UB is removed** — it is reported, not fixed (§3).

## 7. How to re-run

```sh
sh results/r4f-g0.sh                    # G1 + G2 (+ G2's instrumented gate)
sh results/r4f-n3.sh                    # G4-N3 (needs ./build/v86c: run make first)
python3 results/r4f-cites.py            # the N1 invariant (rc=0 == 0 mispointed)
python3 results/r4f-cites-fix.py --list # the 73 corrections, ranges derived from ./v86
python3 results/r4f-cites-fix.py --verify-prefix results/r4f-instructions_0f-prefix.c.gz
make && make selftest && make nasm && make transcript-diff
```
