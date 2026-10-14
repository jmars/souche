# JIT-FIX-2 — the jcc array-slot swap pairs: the claim re-measured, the matrix hole closed

**Unit:** `HANDOFF=v86c-jit` JIT-FIX-2 (coder), the last item of `results/jit-fix-1r-review.md`
(NICE-TO-HAVE-1(b)). **Files changed:** `src/cpu/jit_codegen_selftest.c` (8 new cases),
`results/jit-fix-1-gates.sh` (4 new G2 arms + one printed-but-ungated row now gated).
`src/cpu/jit_codegen.c` is **byte-unchanged** (`5ce8386b…`); the JIT default is still OFF.

**Verdict in one line.** The reviewer's *remedy* is right and is implemented (the recipe matrix had
no CF=0,ZF=1 case, so all four array-slot swaps it named passed every suite), but the reviewer's
*premise about the whole gate is REFUTED by measurement*: the gate already caught all four — not in
any suite, but through `tests/perf_bench.sh`'s pinned 200 M `--jit on` leg, whose row is `note`d and
moves the script's exit code. §1 is that refutation, §2 the case set and its per-pair attribution,
§3 a second, independent hole this unit found in the same script (a 1.5 G verdict printed but never
gated), §4-§5 the gates.

---

## 1. THE CLAIM, RE-ESTABLISHED BY MEASUREMENT — and where it is wrong

### 1a. What is TRUE: the recipe MATRIX is blind to all four swaps (the brief's premise)

Swapping the two positional arrays' condition slots — `[2]↔[6]` (jb↔jbe) and `[3]↔[7]` (jae↔ja),
in `jitcg_jcc8_fns` and in `jitcg_jcc32_fns` — on the pre-fix tree:

| variant (`results/jit-fix-2/jit_codegen.c.*`, generator `mkvariants.py`) | `make jit-codegen-selftest` |
| --- | --- |
| `jcc8-swap-bbe` | **rc=0**, `jit_codegen_selftest: 2778 checks, 0 failures` |
| `jcc8-swap-aea` | **rc=0**, 2778 / 0 |
| `jcc32-swap-bbe` | **rc=0**, 2778 / 0 |
| `jcc32-swap-aea` | **rc=0**, 2778 / 0 |

(evidence `G1-<variant>.txt`; each restored afterwards, `cmp` clean). The same for every other
suite-shaped leg, on the **all-four** variant (`G1-allswap-summary.txt`): `jit-selftest` rc=0;
`make nasm` rc=0 and the two extra nasm legs rc=0 (`[!] 0 of 123 selected test(s) produced no state`,
5 skipped) — so the corpus is blind too, per-variant as well (`G1c-jitlegs.sh`).

**Why the matrix cannot see it** (the brief's diagnosis, confirmed on the code): the emitted
condition is the **slot's** `cc` (`jit_codegen.c:968-981`: `e.aux = cc; e.fn = jitcg_jcc8_fns[cc]`),
so a swap makes each encoding run the *other's* condition. B(CF) and BE(CF||ZF) behave identically
when CF=0,ZF=0 and when CF=1 — they differ **only** when CF=0 && ZF=1; likewise AE(!CF) and
A(!CF && !ZF). The matrix's two flag states are CF=0,ZF=0 (`setup_matrix`) and CF=1,ZF=0
(`setup_matrix_lazy`), so no case separated those pairs. The twelve fn-pointer pins cannot either:
`CHECK(jit_recipe_name(u.fn) == r->name)` is the same lookup compared twice (a tautology by
construction).

### 1b. What is FALSE: "invisible to the whole gate"

The gate is not blind to them, and the leg that sees them is not one of the suites. `results/
jit-fix-1-gates.sh` runs `tests/perf_bench.sh`, which pins the 200 M `--jit on` boot on **three**
clauses per run (instruction count at the limit, terminal guest state, serial sha) and whose row is
`note`d, i.e. it moves the aggregate. Measured, two runs per variant (`G1d-bench200m.txt`):

| tree | instruction count at the 200 M limit | terminal state |
| --- | --- | --- |
| **fixed** (the pins) | `200095214` | `in_hlt=0, eip=0x21972c1 cs=0x10 cr0=0x60000011 cr3=0x0` |
| `jcc8-swap-bbe` | `280921` (**the guest halts**) | `in_hlt=1, eip=0x219a3c1` |
| `jcc8-swap-aea` | `200097867` | `in_hlt=0, eip=0x219a760` (≠ the pin) |
| `jcc32-swap-bbe` | `530745` (halts) | `in_hlt=1, eip=0x219a3c1` |
| `jcc32-swap-aea` | `542688` (halts) | `in_hlt=1, eip=0x219a3c1` |
| `all-swap` | `270070` (halts) | `in_hlt=1, eip=0x219a3c1` |

Both runs of every variant agree exactly, and the fixed tree reproduces the pins exactly. The
**serial sha is identical in every one of them** (`fae4924b…`, 30 bytes) — it is the count and the
guest state that fire, and the state clause (`in_hlt`/`eip`) is not a clock artefact. Directly
confirmed inside the script: on a `jcc8-swap-aea` baseline the full run's `bench --jit on` row reads
**rc=1** (`G1-swapped-gate-run.txt`).

Two more legs see them, one gated and one not:
- the **`--jit on` transcript vs the JS reference** (a `note`d row) catches `jcc8-swap-bbe` (and
  `all-swap`) and NOT the other three (`G1b-isolate-summary.txt`) — a property of what
  `tests/test.img` happens to execute, not a structural guarantee;
- the **1.5 G boot state** differs for all four (off `eip=0xc1927800` vs on `eip=0x219a760`, serial
  `7044a6f6` 17665 B vs `fae4924b` 30 B) — but that comparison was **printed and never `note`d**,
  i.e. it did not gate anything. That is §3.

### 1c. What the claim should have said

> The four array-slot swaps are invisible to the **recipe matrix, the twelve fn pins and the nasm
> corpus** (all rc=0 at 2778 checks / 0 failures), and to the transcript for three of the four. They
> are **not** invisible to the gate: the bench's 200 M `--jit on` row already fails on all four
> (deterministically, on the guest-visible terminal state), so the gate's exit code moves.

Corrected in place, marked, in `results/jit-fix-1.md` §4.1 and at the end of
`results/jit-fix-1r-review.md` (appended, not rewritten — the record is append-only).

## 2. THE CASES — smallest set that separates both pairs, in both arrays

`jit_codegen_selftest.c`'s jcc block gains **one body per (condition, width)**, eight cases, in the
existing style (byte array + `recipe_case` + explicit setup). The missing state is produced *in the
body* rather than by a new setup function, exactly as the brief prescribed: `cmp eax,eax` (EAX is
`0xC0DE` in `setup_matrix`) leaves **CF=0, ZF=1, OF=0, SF=0** — and a *lazy* flag state, so the
compound conditions' two-getter path runs too.

| body | encoding | state | separates |
| --- | --- | --- | --- |
| `cmp; jb rel8` (z1) | `72` | ZF=1 CF=0, not taken | **B ↔ BE** |
| `cmp; jbe rel8` (z2) | `76` | taken | **B ↔ BE** |
| `cmp; jae rel8` (z3) | `73` | taken | **AE ↔ A** |
| `cmp; ja rel8` (z4) | `77` | not taken | **AE ↔ A** |
| `cmp; jb rel32` (z5) | `0F 82` | not taken | **B ↔ BE** |
| `cmp; jbe rel32` (z6) | `0F 86` | taken | **B ↔ BE** |
| `cmp; jae rel32` (z7) | `0F 83` | taken | **AE ↔ A** |
| `cmp; ja rel32` (z8) | `0F 87` | not taken | **AE ↔ A** |

Both widths because there are **two** positional arrays with the same blindness. Counts:
`jit_codegen_selftest` **2778 → 2978** checks (+200 = 8 cases × 25), 0 failures; every other suite's
count is unchanged.

**ASYMMETRY, per pair and per width** (each swap installed alone; `make jit-codegen-selftest`):

| swap installed | rc | failures | cases that fail (nothing else does) |
| --- | --- | --- | --- |
| `jcc8-swap-bbe` | 2 | 16 | `cmp; jb rel8`, `cmp; jbe rel8` |
| `jcc8-swap-aea` | 2 | 16 | `cmp; jae rel8`, `cmp; ja rel8` |
| `jcc32-swap-bbe` | 2 | 16 | `cmp; jb rel32`, `cmp; jbe rel32` |
| `jcc32-swap-aea` | 2 | 16 | `cmp; jae rel32`, `cmp; ja rel32` |
| `all-swap` | 2 | 64 | all eight |

(evidence `G2-<variant>.txt`; the restored tree is `2978 checks, 0 failures`.) The failure is the
differential itself, not a hand-written constant: e.g. `cmp; jae rel8 (ZF=1 CF=0, taken): cpu_state
differs first at offset 68: 0x07 vs 0x05` — the compiled arm took the not-taken path.

**What remains.** Every one of the twelve inlined encodings now has a case, and the two pairs that
differ *only* in CF=0,ZF=1 are the only ones that were invisible, so no in-pair swap is left. The
class one step outside the brief — swapping a slot whose condition **is** inlined with one whose is
**not** (`[0]=jo`↔`[2]=jb`, `[8]=js`↔`[6]=jbe`) — is **already** caught, by two pre-existing
instruments and by neither of my cases: MEASURED `G1e-unsupported-swap.txt` —
`FAIL the recipe table has no jb rel8 (PLAIN1)` (the fn pin) and
`FAIL jb (materialised, CF=0): NOTHING WAS INLINED … inlined 0 called 2` (the matrix's
`inlined_uops` guard), 3 and 2 failures respectively. The bench is blind to these two (count and
state exactly at the pins), so the suite is the only thing that catches that class — worth knowing.
A swap between **two** unsupported slots is not read at all (`jitcg_build_recipes` skips
`!jc_cc_supported(cc)`), i.e. invisible by construction and harmless; and a cross-**width** swap of
the same condition leaves both entries with the same `cc`/`aux`/name, so the condition is unchanged
(INTERPRETATION from the code at `jit_codegen.c:966-982`, not measured).

## 3. SECOND FINDING (extension, beyond the brief): one verdict row was printed but never gated

`results/jit-fix-1-gates.sh`'s 1.5 G section printed "`1.5 G DIFFER: …`" and compared the two legs'
serials with a bare `cmp -s … && echo` — **no `note` call**, so the aggregate could not see it. That
is JIT-FIX-1R NICE-TO-HAVE-2's exact shape one level down from where the orchestrator fixed it ("a
verdict that nothing consumes"). MEASURED on the `jcc8-swap-aea` baseline
(`G1-swapped-gate-run.txt`): the legs print

```
1.5 G boot (off)    in_hlt=0, … eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000
1.5 G boot (onjit)  in_hlt=0, … eip=0x219a760  cs=0x10 cr0=0x60000011 cr3=0x0
1.5 G DIFFER: off="…0xc1927800…" onjit="…0x219a760…"
7044a6f6…  boot-1.5g-off-serial.txt          (17665 B)
fae4924b…  boot-1.5g-onjit-serial.txt        (30 B)
```

and the byte-identity `cmp` fails — with the row contributing nothing to `WORST_RC`. **FIXED:** that
comparison is now a `note`d row (*"1.5 G serial: --jit on == the default leg (byte-identical)"*),
so the leg that SEES a wrong condition also GATES it. Its teeth on real artefacts: on the fixed tree
the two serials are the same file content (`7044a6f6…` both, `gates-summary.txt` line "1.5 G serial:
… rc=0"); on the swapped tree they differ (`7044a6f6` vs `fae4924b` as printed above) → the row
would be rc=1. Note the limit of that measurement: inside a *full* run the four new G2 arms fire
first on the same baseline, so the row's own rc is what attributes it (rows are printed per row).

Deliberately NOT changed: the state-clause echo above it (the byte-identity of the serials is the
stronger invariant and implies the printed clause), and every other row of the script.

## 4. GATES

| gate | result |
| --- | --- |
| **G1** the claim re-established | **PARTLY REFUTED, with evidence** — §1: suite/nasm blind (2778/0, rc=0 ×4), bench's 200 M leg catches all four (rc=1 row), transcript catches one, 1.5 G boot state catches all four but was ungated |
| **G2** each new case able to fail | §2 table — 16 failures per swap, exactly its own two cases; `all-swap` 64; restored tree `2978 checks, 0 failures` |
| **G3** `make selftest`, all suites | rc 0, `selftest: all suites green`: cpu 3522/0, m1c 228/0, hw 551/0, uop_cache 7934349/0, uop_exec 102/0, jit **51/0**, jit_codegen **2978/0** (was 2778) — no other count moved |
| **G3** nasm | 118/118 in all three legs (default, `--uop-cache off`, `--jit on`) |
| **G3** transcripts | `make transcript-diff` rc 0; `--jit off` and `--jit on` byte-identical to the JS reference |
| **G3** 1.5 G boot | off and `--jit on` **identical** state (`eip=0xc1927800 …`) **and** byte-identical serials `7044a6f6…`; the new row rc=0 |
| **G3** aarch64 | 6/6 files rc 0 under `-Werror` |
| **G4** `bash results/jit-fix-1-gates.sh` | **exit 0, `WORST RC ACROSS ALL ROWS: 0`**, no `NO TEETH`/`RESTORE FAILED`/`UNEXPECTED FAILURE`/`VARIANT MISSING` row; the four new arms each read `FAILS as it must (rc=2)` + `restored (cmp vs the saved copy)` |
| **G5** bench | `--jit off` rc 0, `--jit on` rc 0: off best 2.713 / median 2.837 s, on best 1.571 / median 1.679 s → **1.727x best, 1.690x median** (unchanged by this unit) |

Raw: `results/jit-fix-2/G4-F5-gate-run.txt`, `results/jit-fix-1/gates-summary.txt` (the run's own
summary; the previous summary is preserved as `results/jit-fix-2/gates-summary-jit-fix-1-as-left.txt`
because the script truncates it).

## 5. WHAT THIS UNIT DOES NOT CLAIM

- **The bench is not a substitute for the matrix.** It gives an unattributed red light after a
  200 M boot, and its count clause is documented as host-clock-adjacent; the new cases localise the
  wrong pair and width in 0.2 s. Both are in the gate now.
- **The fn pins are still a tautology** for swap detection (`jit_recipe_name(u.fn) == r->name`). Not
  touched: a *direct* array pin would need `jitcg_jcc8_fns` exposed, and the matrix now covers the
  class. Named, not fixed.
- **The 1.5 G row's `off`-vs-`on` comparison is the only new invariant**, and it is a claimed
  invariant of the JIT (byte-identical output), measured stable twice; if it ever fails legitimately
  the gate goes red, which is the intent.
- **Serial sha is worthless as a swap detector** on the bench workload (identical 30 bytes in all six
  trees above) — the count and state clauses are what fire. Recorded so a reader does not conclude
  the sha pin has this power.
- `v86/` untouched, no git operations, JIT default still OFF (the orchestrator's decision), and the
  invalidation hooks / blocker fix untouched.

## 6. HOW TO REPRODUCE

```sh
python3 results/jit-fix-2/mkvariants.py results/jit-fix-2/jit_codegen.c.fixed results/jit-fix-2
bash results/jit-fix-2/g1-allswap.sh        # G1: the whole gate's legs on the all-four tree
bash results/jit-fix-2/g1b-isolate.sh       # G1: which single swap the transcript sees
bash results/jit-fix-2/g1c-jitlegs.sh       # G1: the --jit on legs, per variant
bash results/jit-fix-2/g1d-bench-pin.sh     # G1: the bench's 200 M pin, per variant (the catch)
bash results/jit-fix-2/g1e-unsupported.sh   # the class one step outside the brief
bash results/jit-fix-1-gates.sh             # G4+G5 (about 5 min) -> exit 0 / WORST RC 0
# the swapped-baseline run of §1c/§3 (same script, OUT redirected so results/jit-fix-1/ is not
# overwritten -- it must stay at results/ depth, its `cd $(dirname $0)/..` resolves from there):
bash results/gate-g1-swapped.sh             # artefacts in results/jit-fix-2/gate-g1/
```

## 7. FILES AND HASHES (`results/jit-fix-2/tree-shas-after.txt`)

| file | sha256 | changed by |
| --- | --- | --- |
| `src/cpu/jit_codegen.c` | `5ce8386b…` | **NO** — byte-identical to the reviewed blob |
| `src/cpu/jit_codegen_selftest.c` | `f7693d9f…` | yes (8 cases; was `da8c90c1…`) |
| `src/cpu/jit_mem.c` | `98af5ce6…` | no |
| `src/cpu/jit_x64.c` | `229724b5…` | no |
| `results/jit-fix-1-gates.sh` | `418c1770…` | yes (G2f–G2i, the gated 1.5 G row) |
| `results/jit-fix-1.md`, `results/jit-fix-1r-review.md` | appended, marked corrections | §1c |

