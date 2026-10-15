# The perf-bench blind spot, re-measured and closed

**Unit:** orchestrator, 2026-10-15, on `TODO.md` §4's first bullet ("`tests/perf_bench.sh` is blind to
swapping two non-inlined jcc recipe slots"). **Files changed:** `tests/perf_bench.sh` (the fourth
pin), `results/jit-fix-1-gates.sh` (its `--jit off` leg now pins the all-zero line), `TODO.md` §4.
**No source file changed** — `src/cpu/jit_codegen.c` is `fec58189…` before and after, restored by
`cmp` after every mutant below.

**Verdict in one line:** the bullet was wrong in both directions — the bench **catches all four** of
the swaps it named (rc 1, on the instruction count alone), and the class it is actually blind to is
the one that crosses the **inline boundary**; that class is now pinned by the one line that moves for
it, and the pin is asymmetric (the mutant fails with it, and was invisible without it).

---

## 1. The instrument: what a "jcc recipe-slot swap" is

`jitcg_jcc8_fns`/`jitcg_jcc32_fns` (`src/cpu/jit_codegen.c`) are indexed **by the condition nibble**,
so swapping two entries substitutes one condition's recipe for another with no other trace in the
file. The variants are `results/jit-fix-2/mkvariants.py`'s, regenerated here from the current source
(each symbol asserted to occur exactly once); every variant is installed alone, and the source is
restored and byte-compared after each.

## 2. THE MATRIX (MEASURED, this tree, every row re-run today)

`repro-bench-matrix.sh` (bench) and `repro-suite-matrix.sh` (suites) are the scripts; the captures are
in this directory.

| variant | `jit_codegen_selftest` | `perf_bench.sh` 200 M |
| --- | --- | --- |
| `jb↔jbe` rel8 (`jcc8-swap-bbe`) | rc 1 — 16 failures | **rc 1** — 280,921 instructions at the limit |
| `jae↔ja` rel8 (`jcc8-swap-aea`) | rc 1 — 24 failures | **rc 1** — 200,097,867 |
| `jb↔jbe` rel32 (`jcc32-swap-bbe`) | rc 1 — 16 failures | **rc 1** — 530,745 |
| `jae↔ja` rel32 (`jcc32-swap-aea`) | rc 1 — 16 failures | **rc 1** — 542,688 |
| `jo↔jb` (`jcc8-swap-b0`, cross-class) | rc 1 — 3 failures | **rc 0 — BLIND** |
| `js↔jbe` (`jcc8-swap-s6`, cross-class) | rc 1 — 2 failures | **rc 0 — BLIND** |
| none (baseline) | rc 0 — 3525 checks, 0 failures | rc 0 — 200,095,214 (the pin) |

The four numbers in the bench column are **exactly** `results/jit-fix-2.md` §1b's (280,921 /
200,097,867 / 530,745 / 542,688), which is the point: that unit's claim that the bench sees them
reproduces on a tree two units later. What it did **not** say, and this matrix does, is which class
the bench cannot see: for `jo↔jb` and `js↔jbe` the instruction count, the terminal guest state *and*
the serial sha all sit exactly on their pins — the guest's behaviour is identical, so no
guest-visible pin can ever see it.

## 3. THE CLOSURE — the JIT's own inlining line

One line moves for that class, and it is not a guest-visible one:

```
baseline      jit: uops executed inlined 195429258 / called 4665955 -> 97.67% dynamic inlined (compile-time: 8330 with a recipe, 3719 without, …)
jo<->jb swap  jit: uops executed inlined 195416212 / called 4679001 -> 97.66% dynamic inlined (compile-time: 8310 with a recipe, 3739 without, …)
```

MEASURED stable over 5 consecutive baseline runs (identical to the digit). `tests/perf_bench.sh` now
runs the workload with `--jit-stats` and pins `inlined / called / with-a-recipe / without` =
`195429258/4665955/8330/3719`, overridable with `--expect-jit-stats I/C/R/N` — which is how a
`--jit off` leg pins `0/0/0/0` and so asserts that the JIT really did nothing (a run with the JIT off
reports the same instruction count at the limit, so the pre-existing pins could not tell).

**The asymmetry, both arms run:**

| arm | before the pin | after the pin |
| --- | --- | --- |
| `jo↔jb` installed | rc 0 — invisible | **rc 1** — `got "195416212 4679001 8310 3739"`, the three guest-visible pins passing |
| baseline | rc 0 | rc 0 — `make bench`, 7 runs, all four pins |
| `--jit off` with the default pin | rc 0 | rc 1 — `got "0 0 0 0"` (the JIT did nothing, and now that is asserted) |
| `--jit off` with `--expect-jit-stats 0/0/0/0` | rc 0 | rc 0 |

## 4. GATES

| gate | result |
| --- | --- |
| `make bench` (7 runs, the new pin active by default) | **rc 0**, all four pins per run |
| `make selftest` on the restored tree | **rc 0**, `jit_codegen_selftest: 3525 checks, 0 failures`, all suites green |
| the mutated tree | `make` rc 0 and every suite green under `jb↔jbe` etc. — i.e. the mutants that the *bench* catches are also suite-caught; the two cross-class mutants are suite-caught and were bench-invisible |
| `src/cpu/jit_codegen.c` after every arm | `cmp` clean against the saved copy; sha `fec58189…` unchanged |

**NOT re-run, stated rather than implied:** the whole `results/jit-fix-1-gates.sh` (it builds variant
binaries and runs 1.5 G boots). Its G4 `--jit off` invocation was updated to pass the zero pin, and
that arm was run directly here through an equivalent `--jit` wrapper binary.

## 5. WHAT THIS DOES NOT CLAIM

- **The bench is still not a substitute for the suite.** The pin localises nothing: it says the
  machine inlines differently, and `jit_codegen_selftest` is what names the wrong (condition, width).
  Both are in the gate now, and the failure text says so.
- **The fn-pointer pins are still a tautology** for swap detection
  (`jit_recipe_name(u.fn) == r->name` compares the same lookup twice — `results/jit-fix-2.md` §5).
  Not touched here; the recipe cases cover the class.
- **A cross-width swap of the same condition** remains un-read (the entries differ, the condition
  does not) — INTERPRETATION from the code, not measured here.
- **The pin must be updated deliberately** by any unit that legitimately changes the recipe tables;
  that is what a pin is for, and the failure text says it.
