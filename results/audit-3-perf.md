# AUDIT-3 — PERFORMANCE: where the time goes now, and what is cheaply removable

**Unit:** `HANDOFF=v86c-audit`, unit AUDIT-3 (reviewer). **Inputs:** `/tmp/brief-v86c-audit-3.txt`,
`results/perf-5-tune.md` (the A/B discipline), `results/jit-6-headroom.md` + `results/jit-6r-review.md`
+ `results/jit-6f.md` (the chaining work and the attribution instrument's known defects),
`results/perf-1-inlining.md` (the unity TU and the codegen flags), `results/jit-plan.md` §3/§7.
**Deliverable:** this document + memory node `handoff-v86c-audit3`. **The tree was not modified** —
every variant was built in the scratch copy `/tmp/audit3` (plus `/tmp/a3-rdtsc`, `/tmp/v86c-perf5-pg`),
the project's established method. Raw A/B logs, stats dumps and instrument outputs are quoted below
with their scratch paths; the scratch trees are re-creatable from this document's patches, which are
described in enough detail to re-apply (the two largest are transcribed verbatim in §F1/§F2's
mechanism descriptions — they are small).

Claims are tagged **MEASURED** (this host, this session, command or A/B rig output cited),
**READ** (source, file:line) or **PROJECTION** (marked, with its arithmetic). Load is quoted per
block — the host was shared with two concurrent reviewers (load 0.9–9.2 across the session; every
A/B below is interleaved *and pinned to one core*, see §I.3, which is what makes the numbers
comparable despite that).

---

## 0. Bottom line

The benchmark is the same 200,095,214-instruction Alpine kernel boot every prior unit used, with
the shipped binary at **best 1.437 s / median 1.45 s (≈ 139 MIPS)** in this session's pinned A/B
blocks (the orchestrator's 1.527–1.592 s was measured at load ~6; do not compare absolute times
across loads — the *ratios* below are same-conditions interleaved and are the transferable numbers).

**Three candidates were implemented in scratch and measured ALONE, interleaved, pinned,
identity-asserted on every run** (200,095,214 instructions, terminal state `eip=0x21972c1`,
serial sha `fae4924b…`); one build-config candidate was measured the same way:

| # | candidate | measured alone (best/median ratio vs the shipped binary) | effort | verdict |
|---|---|---|---|---|
| **F1** | **skip the per-uop `call l_checks` for provably-continuing recipes** (§F1) | **0.7005x / 0.7017x (−30%)** | small (one function in jit_codegen.c) | **rank 1 — take it** |
| **F3** | **per-TU link instead of the unity TU, with the JIT on** (§F3) | **0.7578x / 0.7587x (−24%)** | build config — **but** it regresses `--jit off` 1.34x | **rank 2 — needs a split decision, not a straight revert** |
| **F2** | **the missing shift recipes** (`shl/shr r32, cl` — D3 /4 /5; the plan's own M3, never landed) (§F2) | **0.9322x / 0.9259x (−6.8%)** | small-medium (recipes + matrix arms) | **rank 3 — take it** |
| **F4** | **fold `in_mapped_range` into the TLB mask** (§F4) | **0.9644x / 0.9632x (−3.6%)** | tiny (2 lines in jit_codegen.c) | **rank 4 — take it** |
| — | **F1+F2+F4 stacked** | **0.5929x / 0.5917x (−41%)**: 1.437 s → **0.852 s best ≈ 234 MIPS** | the three together | all suites green in scratch (§0.1) |

And the **negative results** (a candidate that does not pay is a result — PERF-5's standard):

| candidate | verdict |
|---|---|
| direct-patch chaining (QEMU-style: patch the chain stub to a direct `jmp` after first link) | **refuted as a big win by measurement**: the whole per-link C probe is **net 138.3 M cycles ≈ 2.7% of the clean run** (§N1). My own first-principles projection before measuring it was 9–18% — wrong, and the bracket is why we know. ≤3% for real unlink machinery is not worth a unit on this workload. |
| the C dispatch path per block entry (exit continuation, loop top, hash probe, enter) | **already dead**: net 60.0 M cycles ≈ **1.2%** of the run (§N2) — chaining finished this job. |
| the device/timer fan-out (`run_hardware_timers`, PIT/RTC/ACPI, `handle_irqs`) | **MEASURED counts**: 1,840 `run_hardware_timers`/`pit_timer`/`rtc_timer` + 1,872 `handle_irqs` per 200 M instructions (once per `do_many_cycles_native`, i.e. per 100,003 instructions — cpu.c:219-226, faithful to cpu.rs:3398-3410). Order 0.1% of the run. Nothing in it is doing work a device needs less of. |
| `getenv`-gated diagnostics on hot paths | **none left** (READ): the three device logs (io.c:71-78, ide.c:268-274, virtio.c:26-34) cache the environment in a static on first use; the rest (JITCG_DUMP ×2, UOP_TRACE3, V86C_JIT_PROBE_FAIL, V86C_BOOT_DUMP) are compile-time or init-time. The 30%-cost class from PERF-FIX is closed. |
| softfloat codegen for the FPU path | **1 call per boot** (perf-1 §7.2 MEASURED then; still true — `fpu_finit`/`fstcw` at boot only). Binary size only; zero runtime on any workload that does not execute x87. |
| per-call `malloc`/`memcpy` on device paths | **none per-instruction** (READ): every hit is init/attach (uart.c:100/535, kernel.c:280) or per-transfer (ide.c:294/319, virtio_console.c:198-215, dma.c:292). A console-heavy guest pays one malloc per virtio buffer, not per byte. |
| lazy-flag register caching | already a measured negative in JIT-6 §4 (ceiling 0.8–1.3%); unchanged. |

### 0.1 What validates the scratch variants (and what does not)

Every timed run of every variant was identity-asserted by the A/B rig (`results/perf-5-ab.sh`:
instruction count, terminal state, serial sha — all runs in every table row above show
`instr=ok state=ok serial=ok`). Beyond that, in the scratch: **`make selftest` is green on the
F1 stack** — all suites, including `jit_codegen_selftest: 3006 checks, 0 failures` (the per-recipe
differential matrix) and `uop_cache_selftest: 7,934,349 checks` (`/tmp/audit3/stack-selftest.log`).
**Not run in scratch** (the landing unit must): the nasm corpus in all three legs, the 1.5 G boot,
the reboot gate, the mutation rig, the aarch64 compile, and — specific to F2 — **matrix arms for
the two new recipes**: the 3006-check count did not grow with them, i.e. the differential matrix
does not yet cover `shl/shr r32, cl`; their validation today is the boot identity plus the
staircase counters (byte-identical: flushes 257,574, exits return 234,330 / tail 5,227 / abort 10 /
block-end 4,217, chain links 15,073,458 — same numbers as the shipped tree on every variant).

---

## I. The instrument: what it can and cannot see (brief Q1)

### I.1 gprof is now blind to ~85% of the run — MEASURED

The `-pg` build (host cc, the `results/perf-5-gprof.sh` method, run from the scratch so the real
tree's `results/` stayed clean) completes the boot with the exact identity
(`/tmp/audit3/results/perf-5/gprof-run.txt`: 200,095,214 instructions, `eip=0x21972c1`) in
**1.888–1.932 s pinned** — but gprof's flat profile accounts for only **0.30 s** of it. The
missing ~1.6 s is time in **JIT-generated code, which has no symbol and is dropped from the
profile entirely**. Consequences:

* the **call counts are still valid** (mcount counts entries) and they are the load-bearing output
  — this is where §F2's finding came from;
* the **self-times are doubly useless now** (the ~7x `-pg` distortion PLUS the 85% blind spot);
  `jit_chain_target`'s "53% of sampled C time" in the flat profile is mostly **mcount's own
  overhead on 15 M calls** — the rdtsc bracket (§N1) is what sized it honestly;
* a "profile" of this engine without the rdtsc instrument sees only the fallback path.

### I.2 The rdtsc attribution instrument (with the chain bracket this audit added)

`results/jit-6/make-rdtsc-variant.py` (the JIT-6F-corrected version — the brief's warning about
the non-reproducible published number applies to the PRE-fix script; the tree's copy IS the fixed
one) was extended with a bracket around `jit_chain_target` (a wrapper that renames the body
`jit_chain_target_impl`; scratch `/tmp/audit3/make-rdtsc-audit3.py` + `add-chain-patch.py`).
One instrumented run, identity-asserted, `J6_PAIR_COST=54`:

```
j6: t_uoprun 5927859000 cyc / 238547 calls; t_dispatch_raw 73200645 cyc / 243783 brackets
j6: pair_cost 54 -> dispatch_est 60036363 cyc, interior_est 5867822637 cyc
j7: t_chain_raw 952260820 cyc / 15073731 links (net 138279346) = 2.33% of t_uoprun
```

t_uoprun is inflated by this audit's own 15.07 M extra bracket pairs (5.93 G vs the 5.098 G
JIT-6F measured on the unextended instrument; 15.07 M × 54 ≈ 0.81 G — the arithmetic closes), so
the honest clean-run figures are: **dispatch ≈ 60.0 M ≈ 1.2%, chain probe ≈ 138.3 M ≈ 2.7%,
everything else ≈ 96% inside the compiled blocks.** Known limits, inherited plus new: the
54-cycle pair cost is tight-loop-calibrated and lfence costs more in situ, so the *net* figures
are **upper bounds** on the bracketed quantities (JIT-6R's finding, unchanged); the dispatch
bracket excludes prologue/epilogue (lower bound — JIT-6F's correction); the chain bracket
excludes the stub's spill/reload around the call (maybe +30–40% on top of 2.7%, PROJECTION).

### I.3 The A/B noise floor moved — and pinning recovered it — MEASURED

The same binary against itself, unpinned, 6 interleaved pairs: **best ratio 1.037x, median
1.052x** — a ~4–5% apparent delta from noise alone (bimodal run times 1.36–1.40 vs 1.45–1.53;
the host runs the `powersave` governor and shared CPUs with concurrent reviewers at load 0.9–9.2).
That is **20x worse than PERF-5's 0.05%/0.24% floor** and would have swallowed F2 and F4 whole.
Pinning both arms to one fixed core (`taskset -c 8`, the rig patched in scratch to do this)
collapses it: six consecutive pinned runs of the shipped binary = 1.464/1.439/1.429/1.431/1.430/1.429
(≈0.3% spread after the first). **Every ratio in §0 is pinned+interleaved.** Rule for the next
unit: on this host, pin and interleave, and distrust any sub-2% unpinned delta absolutely.

### I.4 What no instrument here can see

icache/branch-predictor effects (no perf on this host — the command that would, if it ever
appears: `perf stat -e L1-icache-load-misses,branch-misses,instructions,cycles ./build/v86c …`),
and anything inside the generated code below the ~10 ms granularity of a whole-boot A/B.

---

## F1 — RANK 1: skip the per-uop checks call for provably-continuing recipes (−30% MEASURED)

**READ.** Every uop's emitted sequence ends with three argument materialisations and
`call l_checks` (jit_codegen.c:1509-1511 pre-patch) into a shared ~24-instruction routine
(jit_codegen.c:1211-1295) that re-derives steps 6–11 of the executor loop. For the great majority
of uops — plain inlined recipes — every one of those steps is provably a no-op **at compile time**:

* step 6 (`interpreted_unimplemented`): only C bodies set it (`unimplemented_instruction`,
  cpu.c:66); a recipe path runs no C body.
* step 7 (`ran && (FLUSH || jit_block_boundary)`): the FLUSH bit is a decode-time uop flag
  (excluded); `jit_block_boundary` is set only by `after_block_boundary()` (cpu.c:78), called only
  from interpreter bodies — every called uop runs its own checks, which clear it (uop_cache.c:1494).
* step 8 (page-cross): `(ip ^ u->eip) & ~0xFFF` with `ip == u->eip + u->len` for a non-control
  recipe — **a compile-time constant** (the criterion excludes the page-crossing minority).
* step 9 (iteration limit): its second conjunct `u->eip >= ip` is false whenever `len ≥ 1`.
* step 10 (BLOCK_END): a decode-time flag (excluded).
* step 11 (divergence): `ip == u->eip + u->len` by the same non-control argument.

The scratch patch (one function, `jitcg_emit_uop`): compute
`cont = rec && !(FLUSH|BLOCK_END flags) && kind ∉ {JCC, JMP, LEA} && ((u->eip ^ (u->eip+u->len)) & ~0xFFF) == 0`;
for `cont` uops emit the recipe + counters + `inc pending` and **no `mov edi/esi/r10`, no
`call l_checks`**. JCC/JMP keep the checks (they must test divergence); LEA keeps them
(conservative: its `body_ip` is FETCH_INTERNAL and the recipe corrects `ip` itself — it could be
added after a dedicated look). A recipe **with a slow arm** keeps the full checks on the *slow*
path (the C body can set anything) and skips them only on the fast path — the patch restructures
exactly that split.

**MEASURED.** 4 pinned interleaved pairs vs the shipped binary, every run identity-asserted
(`/tmp/audit3`, `results/perf-5-ab.sh ./bin-base ./bin-c3`):

```
base         n=4 best 1.439 median 1.440 worst 1.472
c3-checkskip n=4 best 1.008 median 1.010 worst 1.021     ratio 0.7005x / 0.7017x
```

Full separation (worst-B < best-A). The staircase counters are byte-identical (§0.1); the
emitted code *shrank* (mean 173.5 → 169.4 B/uop; the stack with F2+F4 reaches 162.1). `make
selftest` green in scratch including the codegen differential matrix.

**Why it is this big (INTERPRETATION):** the removed sequence is ~27 of the ~34 fixed per-uop
instructions — the single largest block of emitted work, larger than most recipes themselves.
This is the same class PERF-5 §5 found in the C executor ("per-uop bookkeeping the profile cannot
rank") one engine generation later, and it is JIT-6 §5's "the checks routine is the largest single
item" now measured instead of inferred.

**Gate for a landing unit:** all existing gates in both cache modes + jit-on legs; add a mutation
arm (invert the `cont` criterion to always-false → must produce the shipped binary's behaviour;
make it always-true → a staircase-sensitive guest must fail); run the 1.5 G boot and the reboot
gate; keep the proof obligations above as the comment at the criterion.

---

## F2 — RANK 3: the shift recipes were never landed (−6.8% MEASURED for the two hot forms)

**MEASURED (gprof call counts, the -pg build, identity-asserted).** Of the 17,721,995 called uops
(8.86% of dynamic), the composition is:

| body | calls / 200 M | share of called |
|---|---|---|
| `instr32_D3_4_reg` (shl r32, cl) | 7,232,434 | 40.8% |
| `instr32_D3_5_reg` (shr r32, cl) | 5,823,606 | 32.9% |
| `instr32_D1_5_reg` (shr r32, 1) | 1,036,400 | 5.8% |
| `string_instruction` (rep movsw et al.) | 2,690,978 | 15.2% |
| `instr32_FF_1_mem` (dec dword [m]) | 501,316 | 2.8% |
| long tail | ~1.4 M | ~8% |

**The plan's own M3 ("shifts (shl/shr/sar) recipes", jit-plan.md §7 step 7; its §3 coverage table
prices shifts at 7.8% of dynamic bodies) never landed** — the recipe table
(jit_codegen.c:834-943) has 88 entries, none a shift. The two by-`cl` forms alone are 73.7% of
the called toll.

**Scratch implementation (measured):** recipes for `shl/shr r32, cl` keyed on the REGF0 shapes.
The transcription is direct because the native shift *is* the C body's semantics: the body masks
`read_reg8(CL) & 31` (instructions.c:1720/1722) = the hardware's 5-bit count mask for 32-bit
operands in 64-bit mode; arith.c's `b = dest >> (32-count) & 1` (shl32) / `dest >> (count-1) & 1`
(shr32) is exactly native CF; shr32's OF is the ORIGINAL's sign bit (arith.c:860-863),
shl32's OF is `(b ^ sign(result)) << 11` (arith.c:831-837); `count == 0` stores NOTHING (branch
around the whole tail). PF/ZF/SF stay lazy via `last_result`/`flags_changed` — the same stores
the other ALU recipes make.

**MEASURED.** 4 pinned pairs, identity-asserted: **0.9322x best / 0.9259x median** (1.446→1.348
best). Dynamic inlined coverage **91.14% → 97.67%** (called 17.72 M → 4.67 M — exactly the two
bodies' counts). Staircase counters byte-identical.

**Gate for a landing unit:** per-recipe differential matrix arms (the current matrix does NOT
cover them — the 3006-check count did not change, so the recipes' only validation today is the
boot identity + counters); then the remaining forms (sar by-cl; the imm forms C1/D1 /4 /5 /7 at
~1.06 M calls ≈ +0.5% PROJECTION; the 8/16-bit legs; the mem forms with the invalidation hook).
The string ops (15.2% of called) are a separate, bigger recipe (an inline element loop) worth
~2% PROJECTION — its own little unit, not a rider.

---

## F3 — RANK 2: the unity TU is now a 24% pessimization on the default path (MEASURED, with a tension)

PERF-1's unity TU was the structural win of the interpreter era (1.259x on top of the flags).
With the JIT as the default engine, **the same sources linked per-TU (the pre-PERF-1 link shape)
are 24% FASTER**:

```
base(unity) n=4 best 1.437 median 1.442      (labelled "unity" in the log's summary lines)
per-TU      n=4 best 1.089 median 1.094      ratio 0.7578x / 0.7587x     (--jit on, default)
```

…while **`--jit off` (the uop-cache engine) inverts hard**: unity 2.920–2.954 s vs per-TU
3.912–3.917 s — **unity is 1.34x better for the C executor**, exactly the PERF-1 mechanism still
at work there. So the build now pays for both engines with one link shape, and the shape that
suits the *default* engine is the opposite of the one PERF-1 shipped. **The aarch64 APE runs the
uop engine** (the JIT is compiled out) — a straight revert to per-TU would regress aarch64 by
~1.34x (PROJECTION from the x86 `--jit off` proxy; not measured on aarch64, none bootable here).

**MECHANISM (INTERPRETATION — not located):** with the hot path in generated code, the unity TU's
remaining effect on the default path is on the C the generated code *calls* (the shared stub →
`uop_exec_uop`, `jit_chain_target`, `jit_execute_block`) and on `uop_run`'s dispatch layer —
plausibly worse code layout/inlining decisions in a 55 k-line TU (the audit's attempt to bisect
which file's separation pays was cut short: the shipped Makefile's object pattern rules do not
fire for an ad-hoc split; the bisect needs its own rule and is the follow-up's first step).

**Also found here (rig rot):** `results/perf-1/revert-attribution.sh` — PERF-1's re-runnable
attribution arm — **no longer runs**: it predates the uop cache and links without `$(UOP_OBJ)`
(undefined `uop_base_cases`/`uop_descs`/…; fixed ad-hoc in scratch by adding `$(UOP_OBJ)` and
`$(UOP_OBJ)` to the prerequisites). A re-runnable rig that has rotted silently is worth a line in
whatever replaces it.

**Gate for a landing unit:** decide the split (e.g. per-TU for the JIT cluster, unity for the
executor/bodies cluster the uop engine needs — the bisect decides where the line is), then all
gates in BOTH engines and the aarch64 compile, with the A/B run `--jit on` AND `--jit off`.

---

## F4 — RANK 4: fold `in_mapped_range` into the TLB mask (−3.6% MEASURED, 2 lines)

**READ.** The TLB entry already carries `TLB_IN_MAPPED_RANGE`, set at fill time from
`in_mapped_range(high)` (paging.c:249-261) — but the inlined memory path tests the mapped range
again from the *physical address*, per access: `cmp memory_size` + the device-window
`lea/cmp` (jit_codegen.c:352-358), 5–6 instructions before every inlined load and store. Adding
`TLB_IN_MAPPED_RANGE` to the mask (requiring it CLEAR) is the *same predicate at page
granularity* — both the `memory_size` bound and the 0xA0000..0xC0000 window are page multiples —
so the two re-derivations can be deleted.

**MEASURED.** 4 pinned pairs, identity-asserted: **0.9644x / 0.9632x**. The C helpers'
`in_mapped_range(phys)` re-tests (safe_read*/safe_write* in paging.c:627-760 and the
SAFE_READ_WRITE* macros) are the same fold for the *uop engine and the called bodies* — not
measured here (they need the entry, not the phys, plumbed out of `translate_address`); PROJECTION
~1-2% on `--jit off`, unmeasured.

---

## N — Negative results, with their numbers

**N1 — direct-patch chaining: the ceiling is ~2.7%, not worth the unlink machinery (this
audit's own projection refuted).** The rdtsc bracket around `jit_chain_target` (§I.2): 15.07 M
links, raw 952.3 M cycles, **net 138.3 M ≈ 2.7% of the clean 5.1 G-cycle run** (upper bound — the
pair-cost subtraction is tight-loop-calibrated). A patched direct `jmp` would still need the
per-link bookkeeping (`jit_cur_page`/`jit_cur_block` updates are LOAD-BEARING — jit-6f.md §1.2),
an unlink list on invalidation, per-patch W^X flips, and a policy change to the staircase counter
contract (patched links stop bumping `uop_stats.lookups/hits`, which JIT-6 deliberately kept
exact). Note the per-block structure makes the *patch count* small (one stub per block ≈ 740) —
the cost is the machinery, not the patches. Declined on measurement.

**N2 — the C dispatch between blocks: 60.0 M cycles ≈ 1.2%.** The exit continuation, the loop
top, the uop-cache hash probe and `jit_block_enter` together — chaining (JIT-6) finished this;
what remains is bounded by the 243,784 C dispatches per boot. No further dispatch-path work is
worth a unit.

**N3 — the checks routine's remaining consumers are small (PROJECTION):** the two `movabs`
materialisations inside `l_checks` (`&interpreted_unimplemented`, `&jit_block_boundary`) execute
per *called* uop only after F1; folding them into frame slots is worth <0.5%. Not measured
separately — it rides along with F1's landing.

**N4 — block prologue/epilogue and chain entry:** executed 243,784 + 15.07 M times respectively,
~10 instructions each for the chain entry (the D1 clear + frame slots, jit_codegen.c:1300-1401)
— order 1-2% combined (PROJECTION from instruction counts; below the pain threshold of the
instrument once F1 lands).

---

## Coverage, and what this benchmark does not measure (brief Q7)

**This audit's coverage:** one workload (the Alpine kernel boot, 200,095,214 instructions), one
host (x86-64, cosmocc, powersave governor), load 0.9–9.2 quoted per block, JIT on unless stated.
Every number is a ratio from a pinned interleaved A/B unless marked PROJECTION.

Workloads the benchmark does not exercise, and how the candidates would look there:

* **aarch64** (the APE's second half): runs the uop engine, where the unity TU is worth 1.34x —
  F3's straight revert would regress it; F1/F2/F4 do nothing for it (they are codegen), and the
  uop-engine equivalent of F4 (the C-side TLB fold) is the one candidate that would help it.
* **FPU/SSE-heavy guest** (the boot executes zero x87 after init): softfloat is cold forever on
  this benchmark; an FPU-bound guest would put the *called* toll in float bodies — none have
  recipes; F2's win would shrink, the fallback toll would dominate.
* **disk-heavy guest** (IDE DMA / virtio-blk traffic): the device fan-out stays per-100k-cycle
  (N-table) but per-transfer memcpy/malloc costs (ide.c:294/319) become visible; none of F1–F4
  change it.
* **self-modifying code** (11 SMC aborts / 308 invalidations on the whole boot): SMC-heavy guests
  re-decode and re-compile — F1/F2 shrink the *steady-state* block, and the recompile cost is
  unchanged; a worst case would compress all four candidates' wins.
* **a rebooting guest** (the reboot gate's shape): re-entry clears both caches; steady-state wins
  scale with the fraction of time at steady state.

**Transferability:** F1's −30% and F2's −6.8% should transfer to any JIT-bound workload (they
remove fixed per-uop cost); F3 is a host-toolchain property (cosmocc/gcc 14, no LTO); F4's −3.6%
scales with memory-operand density.

---

## Raw evidence (scratch paths; the scratch trees are the record — re-create with the described patches)

| what | where |
|---|---|
| baseline identity + current-tree stats (91.14% inlined, exits, links, 173.5 B/uop) | `/tmp/audit3/base-stats.txt` |
| noise floor, unpinned 6 pairs (1.037x/1.052x) | the rig run of 12:18, transcript in the session log; reproduced pinned 1.429–1.431 ×6 |
| F1 A/B (`base` vs `c3-checkskip`) + `make selftest` green | `results/perf-5-ab.sh ./bin-base ./bin-c3 4`; `/tmp/audit3/c3-selftest.log` |
| F2 A/B (`c1-shifts`) | `sh results/perf-5-ab.sh ./bin-base ./bin-c1 4` (12:22 block) |
| F4 A/B (`c5-tlb`) | 12:52 block, summary in §F4 |
| stack A/B (`c1+c3+c5`, 5 pairs) + selftest green | 12:57 block; `/tmp/audit3/stack-selftest.log`, `/tmp/audit3/stack-stats.txt` |
| F3 A/Bs (`--jit on` 4 pairs; `--jit off` 3+3 runs) | 12:47/12:50 blocks |
| the gprof method + counts | `/tmp/audit3/results/perf-5/gprof-{flat,run,build}.txt` |
| the rdtsc instrument + the chain bracket | `/tmp/audit3/make-rdtsc-audit3.py`, `add-chain-patch.py`, `/tmp/audit3/rdtsc-audit3{,-2}.txt` |
| the variant patches | `git diff` of `/tmp/audit3` vs `a49a887` (scratch only); F2's emitters are transcribed in §F2, F1's criterion in §F1, F4's is two lines |

sha256 (first 16) of every file relied on, at `a49a887`:
`src/cpu/jit_codegen.c 184a286e557d6003` · `src/cpu/jit_mem.c 04eb11179f4224b5` ·
`src/cpu/uop_cache.c c3bf72bd813b3dad` · `src/cpu/cpu.c 9d924181bea42ad0` ·
`src/cpu/paging.c a6a0f1ed2e7d7249` · `src/cpu/arith.c 2ecc7ce171c439cb` ·
`src/cpu/instructions.c 0d6900dcac7d09d5` · `src/hw/machine.c 405e562478babcc0` ·
`src/hw/io.c 2a17a177f16ef30b` · `Makefile 38672415989a6dd2` · `gen/unity_c.py 1252984f83175ba1` ·
`results/perf-5-tune.md 9410093cb0b8d3ab` · `results/perf-5-ab.sh a91dd0ed34950994` ·
`results/perf-5-gprof.sh ff8bffb08a5e900a` · `results/jit-6-headroom.md 19c52aa97c505b76` ·
`results/jit-6r-review.md 0db10d82fa1cd5ff` · `results/jit-6f.md 03c4f28f80a8b41e` ·
`results/perf-1-inlining.md 6fdb7a791653f310` · `results/jit-plan.md 71e383743aea6c85` ·
`results/jit-6/make-rdtsc-variant.py 9472877434ad83b0`.

One fossil worth fixing while nearby (READ): paging.c:249-253 still says "There is no JIT, so
there is no code page to track: false, and TLB_HAS_CODE is never set" — the port HAS a JIT now;
the bit is genuinely never set (code protection rides the per-store bitmap instead, correctly),
but the comment describes a tree that no longer exists.
