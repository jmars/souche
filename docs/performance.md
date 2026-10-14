# Performance — the measured arc, and its instruments' limits

The workload for every number below is the same: **200,095,214 guest instructions** of the Alpine
i386 kernel boot (`--bzimage tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0"`), which
is what `make bench` pins. Ratios are quoted with the host load, because a number measured at load 1
is not comparable to one measured at load 30.

| stage | time | rate | what did it |
| --- | --- | --- | --- |
| the interpreter, as first built | 6.26 s | ~32 MIPS | — |
| **+ step 1** (MEASURED) | 3.97 s | ~50 MIPS | the codegen flags cosmocc's wrapper was injecting, re-enabled (1.235× alone) and a **unity translation unit** (1.259× on top) |
| **+ the uop cache** (MEASURED) | ~2.84 s | ~70 MIPS | decoded blocks + the invalidation hooks + M3/M4 tuning |
| **+ the JIT** (MEASURED) | ~1.68 s | ~119 MIPS | compiling those blocks; 91% dynamic inlined |
| **+ chaining, the platform probe** (MEASURED) | ~1.55 s | ~129 MIPS | a block's exit entering the next block's entry directly |
| **+ the audit's candidates** (MEASURED) | **~0.83 s** | **~240 MIPS** | skipping the exit checks for provably-continuing recipes (−30%), the shift recipes (−5.5%), folding the mapped-range test into the TLB mask (−3.6%) |

**About 7.5× end to end**, with the M0 transcript byte-identical to the reference at every step and
every guest boot unchanged. The check that matters for the last row — the exit contract was touched —
is an interleaved A/B against the previous commit: the same instruction count (200,095,214) and the
same final state, at 1.44/1.41 s → 0.83/0.93 s.

---

## 1. What paid, and why it did

- **The cosmocc wrapper's flags.** `cosmocc -v` shows it injecting `-fno-optimize-sibling-calls`,
  `-fno-inline-functions-called-once`, `-fno-schedule-insns2` and a patchable-entry prologue. The
  tree lands its own flags last: **`-foptimize-sibling-calls` alone closed the entire
  cosmocc-vs-glibc gap** (MEASURED — the cosmocc build is not slower than the host build for any
  other reason; it was this).
- **A unity translation unit.** cosmocc has **no LTO** (`-flto` refuses: *"LTO support has not been
  enabled in this configuration"*), so cross-TU inlining had to come structurally: `gen/unity_c.py`
  builds the shipped core as one TU. That is also why PERF-1's win later *inverted*
  (see §3).
- **The uop cache, then the JIT.** The profile said the interpreter was **per-operand-call bound**
  (337M `read_imm8` calls for 200M instructions), not memory-bound — so the win was in deleting
  calls, which is what decoding once and then compiling those blocks does.
- **The audit's three candidates.** Each measured alone on a pinned core, interleaved, with a
  measured noise floor.

## 2. What was measured out (and stays out)

A negative result is a result, and this project records them:

- **lazy-flag register caching** — gross ceiling 0.8–1.3% end-to-end; the two host registers it needs
  cost more than that.
- **eviction** for the block cache — 0 clears, arena peak 18.4%, block peak 9%.
- **the plan's own M3 loop fast path** — 0%.
- **inlining the address materialisation** — 0%, and 1.0% *slower* in the isolation arm.
- **a register-carried `uop_pending`** — **+3.0% slower**: the same global-RMW shape as a win, with
  the opposite sign, because it must survive the indirect call.
- **direct-patch chaining** — the reviewer's own 9–18% projection measured **2.7%** with a new rdtsc
  bracket. (Chaining itself paid 17% by a different route.)
- **the per-TU link (F3)** — proposed at −24%, but on the post-F1 tree the unity TU is faster for
  **both** paths (`--jit on` 1.066×, `--jit off` 1.35×), so its premise was refuted and the
  gain-vs-regression tension it created does not exist.
- **device work** — the timer/IRQ fan-out is 1,840 calls per boot (~0.1%); softfloat is called once:
  neither is worth touching.

## 3. What the instruments cannot see (measured, not assumed)

- **`gprof` is now blind to ~85% of the run.** Generated code has no symbol to attribute to: 0.30 s
  of a 1.9 s run is attributable. Its self-times are distorted ~7×; **only its call counts are
  usable**, and it needs a host-cc scratch build because cosmocc never writes `gmon.out`.
- **The A/B noise floor is 0.24% pinned and 4–5% unpinned.** Every ratio quoted above is
  core-pinned and says so. An unpinned measurement of a 3% candidate is noise.
- **There is no `perf` and no `valgrind` on this host.**
- The tree carries an **rdtsc-instrumented variant** (`results/jit-6/make-rdtsc-variant.py`) for the
  dispatch/interior split. The chaining review found three defects in it — two fixed, and one
  published number that was **not reproducible** from the script as published. Read
  `results/jit-6r-review.md` before trusting a number from it.
- **The benchmark is one kernel boot.** A disk-heavy, console-heavy, self-modifying or
  reboot-heavy guest would weight the candidates differently — say so before generalising.

## 4. Where the remaining time goes

After the audit's landings, the memory model is the floor: `translate_address` and the
`safe_read*`/`safe_write*` routers are paid by **every** engine, and the JIT's inlining stops at the
same boundary (it calls them, rather than re-implementing them). The JIT is at 97.67% dynamic inlined
coverage; the residue is the shapes that are not worth a recipe. Projecting further would need a
measurement this section does not have.
