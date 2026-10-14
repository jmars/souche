# JIT-6 — THE STEP-8 HEADROOM: block chaining, lazy-flag register caching, and the platform probe

**Unit:** `HANDOFF=v86c-jit`, unit **JIT-6** (coder). **Inputs:** the brief (`/tmp/brief-v86c-jit-6.txt`),
`results/jit-plan.md` §7 step 8 (the three items and the chaining constraint), §6/§8 (win + refutation
rules), §9 (risks), `results/perf-4-uop-exec.md` (the staircase), `results/perf-5-tune.md` (the
measure-each-change-alone method), `results/jit-fix-1r-review.md`, `results/reboot-fix-1.md`.
**Memory:** `handoff-v86c-jit-*`. **Raw evidence:** `results/jit-6/`. **Gates:** `results/jit-6-gates.sh`.

Claims are tagged **MEASURED** (run on this host this session, command and raw output in `results/jit-6/`),
**READ** (source, with file:line) or **INTERPRETATION** (marked as such).

---

## 0. Bottom line

**One of the three step-8 items paid, one is a correctness landing with no perf claim, and one is a
measured negative.**

| step-8 item | verdict | number |
|---|---|---|
| **block chaining** | **LANDED** | **−17.0 % best / −15.5 % median** on the unit's benchmark (base 1.804 s → 1.498 s best, 1.859 → 1.570 median; 133.5 MIPS best), with the staircase counters byte-identical |
| **platform probe** | **LANDED** (correctness/honesty, no perf claim) | init-time reserve+publish+execute canary; the forced-failure path is a tested one (the machine runs the workload byte-identically with the JIT disabled) |
| **lazy-flag register caching** | **NOT BUILT — measured negative** | gross ceiling ≈ 0.8–1.3 % end-to-end; the two host registers it needs do not exist without mandatory costs that already exceed the ceiling (§4) |

The order followed the unit's own re-measurement, not the plan's (the plan listed the three in no
order and predates the JIT): the exit-kind distribution and an rdtsc dispatch/interior split ranked
chaining first before anything was touched.

---

## 1. G0 — the re-measurement, and the attribution method

### 1.1 Baseline and noise floor (MEASURED)

`results/perf-5-ab.sh` (the PERF-5 interleaved rig, identity-asserted: 200,095,214 instructions, the
pinned terminal state, the serial sha `fae4924b…` on every run), binaries in `results/jit-6/`:

| run | best | median |
|---|---|---|
| `--jit off` (uop cache) | 3.014 s | 3.022 s |
| `--jit on` (the shipped default, base tree) | 1.764 s | 1.820 s |
| ratio | **1.708×** | **1.662×** |

 reproducing the orchestrator's ~1.7×. Noise floor, same binary in both arms (3 interleaved pairs):
**0 % best / 2.9 % median** — with a 14-core `qemu-system-x86` on the host for part of the session
(load 18.3 at start; recorded in `results/jit-6/`), so every candidate delta below is an
interleaved best-vs-best comparison, and the medians are quoted with that floor.

### 1.2 The attribution instrument (scratch build — the shipped tree is never instrumented)

There is no perf and no valgrind here and gprof cannot see generated code, so the split came from
the project's own method: **a scratch tree with rdtsc brackets, A/B'd against the same binary**
(`results/jit-6/make-rdtsc-variant.py` builds it; the pair cost is calibrated separately at
**54 cycles/pair** by `results/jit-6/rdtsc-pair-cost.c` and subtracted):

- `cycle_internal` brackets `uop_run` (~238,547 calls — negligible distortion);
- `uop_run`'s compiled arm brackets the segment **between consecutive `jit_execute_block` calls** —
  one bracket pair per block run, whose own cost is the calibrated constant, subtracted;
- the compiled interior itself is never bracketed.

**MEASURED** (200 M boot, instrumented): `t_uoprun` 7.146 G cycles, dispatch segments
1.080 G raw − 15.3 M × 54 = **253 M cycles = 3.5 %**, interior **6.89 G = 96.5 %**. And the exit-kind
counter that had been accumulated since M1 but never printed (printed now, `jit: exits:` line):

```
jit: exits: return 234,330, tail 15,078,685, abort 10, block-end 4,217
```

**TAIL is 98.44 % of block executions** — the dispatch path (epilogue spill/stats fold → C tail →
hash probe → `jit_block_enter` → prologue reload) runs ~15.08 M times per boot at ~13.1 guest
instructions per block. That is what ranked **chaining first**. (The same instrument says where the
REST of the time is — see §5 — the interior's per-uop checks routine, which is the *next* headroom
and is not a step-8 item.)

**[CORRECTED — JIT-6F. Three things about the instrument and the number above.]**

1. **What the bracket really measures (reviewer finding 2).** The brackets sit around
   `jit_execute_block`, so the "dispatch segment" is the C code **between** compiled blocks — the
   exit-kind handling, the loop top and the hash probe — and it **EXCLUDES the prologue and the
   epilogue** (the epilogue in the tail of `call code`, the prologue inside `jit_block_enter` + the
   block's own top), both of which run *inside* the bracket, plus `jit_block_enter`'s page
   identification. Chaining removes the epilogue + C tail + probe + enter + prologue, so
   `t_dispatch` is a **LOWER bound** on the dispatch cost the chain deletes. **"interior 96.5 %"
   must NOT be read as "dispatch is only 3.5 %."** The 17 % chain win is not predicted by, and does
   not contradict, the split — the split simply brackets something narrower than what the chain
   elides. (To bracket the prologue/epilogue too the brackets would have to move into the emitted
   block; no decision in this unit turns on the split, so it is documented rather than rebuilt.)

2. **The first segment included process-start-to-first-block time.** `j6_t_seg` is a static
   initialised to 0, so the first `t_dispatch += now - t_seg` charged the whole
   process-start-to-first-compiled-block interval to dispatch. Fixed: the first segment is dropped
   (`j6_seg_init`).

3. **`dispatch_raw = t_dispatch + j6_t_seg` grafted the host's TSC-since-boot onto the total.**
   `j6_t_seg` at print time is the *absolute* rdtsc value at the start of the last (still
   in-flight) segment, not a duration — so the raw total is only ≈ the true one on a host whose
   TSC reads near zero at process start. **MEASURED on this host (JIT-6F, the script as published
   before the fix, run on the current chained tree): `t_dispatch_raw` read `949,061,756,707,190`
   cycles (≈9.49e14) — the TSC-since-boot term, not a dispatch measurement.** The fixed script sums
   only **complete** segments and drops the first and last. **The 1.080 G raw total above is
   therefore NOT reproducible from the script as published on a host with a nonzero start TSC**
   (it was measured on a tree whose TSC evidently began near zero, and — note — on the **pre-chain**
   tree, where the dispatch ran 15.3 M times). Corrected measurement on the **chained** tree
   (JIT-6F, `--jit-stats`, raw in `results/jit-6/rdtsc-instrument-jit-6f.txt`): `t_uoprun`
   5.098 G cycles, `t_dispatch_raw` **77.6 M** over **243,783** brackets → net **64.5 M = 1.27 %**,
   interior **5.03 G = 98.7 %**. That is a *different tree* (244 K brackets, the C dispatch the
   chain did not remove) from the 15.3 M-bracket pre-chain 3.5 %, so the two are not directly
   comparable — the comparison that ranks work is the identity-asserted A/B (§2.2), not this split.

---

## 2. Block chaining — LANDED

### 2.1 The mechanism, and the plan's constraint

At the checks routine's tail arm (the executor's step 11: control flow left the block, or the block
ran out of uops — the TAIL outcome only; RETURN/ABORT/BLOCK_END never chain), the block now runs a
**chain stub**: abort-flag test → ABI spill of the registers a C call may clobber (guest EAX/ECX/EDX,
the pending count R9, the abort-flag address R8) → `call jit_chain_target` → reload → `jmp` the
target's **chain entry** (a second entry point, emitted after the epilogue, which does only the D1
pagefault clear and this block's frame slots, then jumps into the uop stream).

`jit_chain_target` (src/cpu/jit_mem.c) is the enter/exit-and-key bookkeeping the plan's constraint
demands, per link:

- **the translation is the fetch cache read, never a walk.** `get_phys_eip()` on a same-page tail is
  a pure cache read of `(last_virt_eip, eip_phys)`; on a miss it WALKS, and a walk can deliver #PF
  and discard the batch. The chain takes ONLY the cache-hit arm — by construction the same physical
  address the C tail would compute — and bails to the plain TAIL exit on a miss, where the
  walk-and-fault contract runs once, exactly as before. A fallback after a faulting walk would run
  the walk twice and deliver the fault twice; the gate for this is the miss arm of
  `test_chain_key` (G1e).
- **the full (phys, virt, state) key probe** over the substrate's own hash — the same probe
  `jit_cache_lookup` does; a dropped, stale or other-state block is simply a miss (fallback).
- **the executing-block bookkeeping per link**: `jit_cur_page`/`jit_cur_block` are updated to the
  TARGET, so the deferred-recycle rule (a page is never flipped under the block executing from it)
  and the SMC abort's "is the write inside the executing block's page" test both stay correct when
  execution has chained.
  **[CORRECTED — JIT-6F. The sentence that stood here was FALSE and described a seat that never
  ran.]** It read: "The ABORT continuation in `uop_run` re-derives `phys` from the substrate's
  last-entered block (`jit_exec_block_geom`) instead of the entry block — for an unchained run that
  is the same answer." MEASURED false by reading the call order (jit-6r-review.md finding 1):
  `jit_exec_block_geom` was gated on `jit_cur_block != JIT_PAGE_NONE`, but its only reader runs
  AFTER `jit_execute_block`, which calls `jit_block_exit` first — and `jit_block_exit` clears
  `jit_cur_block` unconditionally. So its true branch was unreachable and `jit_exec_phys`/
  `jit_exec_virt` were write-only: the ABORT continuation has ALWAYS run the `else` arm, i.e.
  `phys = blk->phys_start + (ip - blk->virt_start)` with `blk` = the **ENTRY block** — the record
  the run was entered with, NOT "the substrate's last-entered block". This is the project's own
  named class (a seat that is never called is indistinguishable from an absent seat).
  **The real mechanism, and why it is exact:** using the ENTRY block's geometry is correct for ANY
  ip a run can reach **because jit_chain_target's same-page gate confines every link of a run to
  the entry block's page** — so `blk->phys_start + (ip - blk->virt_start)` is the right phys for
  any ip in that page, whichever same-page block is executing. That invariant is LOAD-BEARING for
  the abort path (not merely an optimisation choice) and is ALSO what keeps the uop side correct:
  `uop_cur_block` stays the ENTRY block for the whole run, and its page test in `uop_invalidate_page`
  — and the `uop_stats.aborts` that goes with it — fire exactly when `jit_abort_flag` does only
  because every link is on its page. The live seat is therefore `jit_cur_page`/`jit_cur_block`
  (read by the deferred recycle, `jit_code_invalidate_page` and `uop_invalidate_page`); the geometry
  helper was never called and was DELETED in JIT-6F, together with the invariant written down at the
  abort site (src/cpu/uop_cache.c) and the cross-page trap named there. WIDENING THE CHAIN ACROSS
  PAGES (adding the walk arm `jit_chain_target` deliberately refuses) would silently break both the
  abort fallback and the uop-side page test.
- **the staircase instruments are kept honest**: the chain bumps the same `uop_stats.lookups/hits`
  the dispatch lookup it replaced used to bump, and the flushes/block-ends/abort counters are
  untouched by construction (the chain happens after step 11; flushes end batches, which are RETURN
  exits and never chain).

What a link elides is therefore exactly: the epilogue's spill + counter fold + pops, the C tail, the
uop-cache hash probe, `jit_block_enter`'s page identification, and the next prologue's pushes +
register reloads + imm64 materialisations — the dispatch cost, not any contract.

### 2.2 The delta (MEASURED, interleaved, identity-asserted on every run)

| arm | best | median |
|---|---|---|
| base (pre-chain) | 1.804 s | 1.859 s |
| final (chained + probe) | **1.498 s** | **1.570 s** |
| delta | **−17.0 %** | **−15.5 %** |

`results/jit-6/` has the binaries (`v86c-base` = the pre-unit tree, `v86c-chain` = chaining only,
`v86c-final` = the tree as landed). An earlier interleaved run measured base 1.831 → chain 1.518
(−17.1 %) — consistent. **One measurement story told in full (see §6's post-mortem):** an
intermediate "final" binary measured 1.877 s — it was built from a tree still carrying the G1a
mutation (a gates-script restore race, this unit's own doing): chains COUNTED but never transferred
(`jit_stats.execs` 15.3 M vs 244 K was the tell, plus +2 emitted bytes per block). Detected by the
A/B, diagnosed by the counters, recovered, re-measured. Engagement:
**15,073,458 links of 15,078,685 tails = 99.97 %** (the 0.03 % are the pre-warmup tails whose target
was not compiled yet, cross-page tails the fetch cache does not cover, and the post-clear rebuilds);
`jit_stats.execs` drops 15,317,242 → 243,784 (one enter per chain RUN).

### 2.3 The staircase is unchanged (G2) — counted, not argued

200 M boot, `--jit-stats --uop-cache-stats`, chained vs the pre-chain tree:

```
uop-cache: lookups 15,317,515 (hits 15,316,502 = 99.99%)   [pre-chain: 15,317,242 / 15,316,502]
uop-cache: flushes 257,574, discards 0, SMC aborts 11, invalidated blocks 308,
           cache clears 0, block ends 4,217                  [all byte-identical]
uop-cache: instructions 200,095,213 (uops 200,095,213, fallback 0.000%)
jit: uops executed inlined 182,373,218 / called 17,721,995 -> 91.14%  [byte-identical]
boot: 200,095,214 instructions, in_hlt=0, eip=0x21972c1 cs=0x10 cr0=0x60000011 cr3=0x0
serial: fae4924b13b07d441b0ad8458047d2f1d8abf0d1ec59876f44b88ef67d2d0b2a  [byte-identical]
```

Hits are IDENTICAL — every lookup that used to hit still hits (the chain's probes replace
`uop_run`'s 1:1); lookups moved +273 (the fallback tails probe once each where the old path's
geometry differed); the batch structure (flushes), the SMC behaviour and the dynamic uop split are
byte-identical. The pinned instruction count and serial sha are the bench's own rows.

### 2.4 The new gates, and their teeth (G1)

- `jit_selftest.c` `test_chain_key` (+10 checks): addresses `jit_chain_target` DIRECTLY with two
  published blocks bound at the SAME (phys, virt) under two state bytes and different chain-entry
  offsets — state S1 must return A's entry, S2 B's, a third state must miss, an
  uncovered-virtual must miss.
- `jit_codegen_selftest.c` `test_chain` (+27 checks): a dec/jnz self-loop whose second block is both
  chain source and target, run differentially in the interpreter / uop-cache / compiled arms; ECX
  and EDX are held NON-ZERO across every link on purpose (a dropped or swapped reload would be
  invisible with zeros — the vacuous-pass shape); assertions: full-state agreement, both stores, the
  engage pin `chains > 0`, and `blocks_compiled >= 2` (the fallback ran too).
- `results/jit-6-gates.sh` arms, each restored and cmp-verified:

| arm | reverts | must fail |
|---|---|---|
| G1a | the abort test inverted — the link is NEVER attempted | the engage pin (`links 0, tails 4`) — and the GUEST gates stay green (the chain is an optimisation, not a semantics) |
| G1b | the pending count reloaded as 0 | `instruction_counter` at the flush (`got 0x3, want 0x2`) |
| G1c | the RCX/RDX reloads swapped | the two stores (`reg32[1] 0xc0de vs 0x12345678`) |
| G1d | the helper's virt+state conjuncts | `test_chain_key` assertion 1 — the walk takes the collision DECOY |
| G1e | the helper's fetch-cache gate | `test_chain_key`'s miss arm — the garbage key hits block C |

Two arms needed honesty revisions before they had teeth, and the reason is the finding:

- **G1a v1 mutated only the final `jmp`** — the helper still ran and still counted `chains`, so
  the engage pin could not fire. The rewritten arm prevents the link from ever being ATTEMPTED.
- **G1d v1 deleted the state conjunct alone** — provably unobservable, not a layout accident:
  the hash MIXES THE STATE into the slot number, so a wrong-state entry is never in the queried
  probe chain at all; a decoy that survives the virt+phys compares would have to have A's exact
  key. The state compare is collision protection, and what the decoy gates is the PAIR deletion
  (also the dangerous class: entering a block compiled for the wrong guest state). The
  single-conjunct classes are the system gates' (nasm, the boots) — stated in the test's comment,
  the same honesty as JIT-FIX-1's cpl3-fold deferral.

---

## 3. The platform probe — LANDED (step 8's own first item)

`jit_platform_probe` (src/cpu/jit_mem.c), called by `jit_mem_init` before the pool is handed out:
**reserve** a page, write a canary (`mov eax, 0x2A5A; ret`), **publish** it (the W^X flip),
**execute** it — the execute is the part no mprotect success can prove — and recycle the page, so
the pool the run sees is pristine. On any failure (or `V86C_JIT_PROBE_FAIL=1`, the diagnostic hook)
`jit_mem_init` returns false: the JIT is **disabled for the run**, and `jit_mem_caps()` — printed
by `main.c` on the failure path too (a JIT-6 fix: previously the caps line printed only on SUCCESS,
so a probe failure was explained by the probe's own line but not by the caps summary) — says the
probe failed and why the probe exists. The machine continues on the uop cache.

What remains UNVERIFIED, unchanged: macOS (MAP_JIT / `pthread_jit_write_protect_np` is not used) and
Windows (`VirtualProtect` via cosmocc is not the guidance sequence) — there is no macOS/Windows host
here; the probe makes the APE honest about it instead of silently trying. The probe itself PASSED on
this Linux host (the caps line now says so).

**G4 — the failure path is a tested one:** `results/jit-6-gates.sh` runs the workload with
`V86C_JIT_PROBE_FAIL=1 --jit on` and asserts (a) the run completes, (b) the caps line reports
`PLATFORM PROBE FAILED`, (c) the arm names its hook, (d) the transcript is byte-identical to the JS
reference — the machine is correct with the JIT disabled.

---

## 4. Lazy-flag register caching — a measured NEGATIVE result (not built)

The brief's discipline is that a change that does not pay is recorded; this one was measured to a
ceiling BEFORE being built, and the ceiling is below the cost of building it:

- **The traffic it would remove.** The inlined flag writers are the group-1 ALU family + test/cmp
  (perf-4/plan census: 26.91 % of dynamic bodies) + inc/dec (3.62 %) ≈ 30.5 % of 200 M ≈ 61 M uops,
  each doing 5 flag stores (`last_op1`, `last_result`, `last_op_size`, `flags_changed`, plus the
  logic ops' `flags` RMW). Caching `last_op1`/`last_result` in host registers removes ~2 of 5 ≈
  122 M stores; the getters (jcc 14.9 %) save ~2 loads each ≈ 60 M loads. At ~0.3–0.5 cycles per
  store/load of throughput: **≈ 55–90 M cycles ≈ 0.8–1.3 % of the 7.15 G-cycle run** — GROSS
  ceiling, before any cost.
- **The two registers do not exist.** The contract holds all eight guest registers (RAX, RCX, RDX,
  RBX, R12–R15), RBP = &cpu, R8 = &abort flag, R9 = pending; R10/RSI/R11 are the recipe scratch
  (the ALU value register, the RMW physical address — live ACROSS the flag update — and the memory
  base), and RDI is the getters' return + the checks routine's argument. Every candidate pair
  requires mandatory costs: freeing R8 needs rip-relative/SIB-absolute addressing the emitter does
  not have; freeing R9 moves the pending count to the frame (+1 load per uop ≈ 200 M × 0.3 ≈ 60 M
  cycles ≈ 0.8 % — already the whole ceiling); R10/RSI/R11 are live by the recipes that would
  benefit. INTERPRETATION from the register contract (jit_codegen.c's header), stated as such.
- The plan's §9 risk 1 (a wrong mask or sub/adc inversion is silently wrong, read by a LATER
  unrelated instruction) is the cost side of the same ledger.

Verdict recorded per the perf-5 standard (four of its seven candidates measured nothing; saying so
was half the result). If a future unit re-opens this, the door is the checks-routine restructure
(§5), which changes the register-budget premise — not the caching itself.

---

## 5. Where the remaining time is (the attribution, stated for the next unit)

With chaining landed the dispatch segment is gone from the profile that matters (and note the
bracket boundary stated in §1.2's correction: the instrument's "dispatch" figure **excludes** the
prologue/epilogue and is a **lower bound**, so do not read it as the whole cost chaining removed);
the interior's
~34.5 cycles/uop (pre-chain measurement) decomposes approximately as: the recipes' own work, the
per-uop **checks routine CALL** (steps 6–11 — emitted once per block, `call`ed per uop, ~20+ insns
per uop including the abort test), the per-called-uop spill/reload + C body (8.86 % of uops),
and the inlined memory model (TLB fast path + access, inside the recipes). The checks routine is
the largest single item INTERPRETATION-wise and is **not** a step-8 item — naming it here is the
unit's attribution duty, the same way perf-5 named the executor loop's bookkeeping its profile could
not rank.

---

## 6. The gate table (G2/G3) — raw evidence in results/jit-6/, summary in results/jit-6/gates-summary.txt

| gate | result |
|---|---|
| `make` | rc 0 |
| `make selftest` — all suites, counts | cpu 3522 / m1c 228 / hw 551 / uop_cache 7,934,349 / uop_exec 102 / **jit 68 (was 51; +17 chain-key)** / codegen **3006 (was 2978; +28 chain)** — pre-existing counts UNCHANGED, 0 failures |
| `make nasm` — default / `--uop-cache off` / `--jit on` | 118/118 in all three legs |
| `make transcript-diff` + both legs vs the JS reference | rc 0, byte-identical |
| `make minibios-mem` | rc 0 (before the guest gates) |
| 1.5 G boot, off vs `--jit on` | final state identical, serial byte-identical |
| `sh tests/gate-reboot-1.sh` | 39 passed, 0 failed |
| aarch64 (`jit_mem.c`, `jit_x64.c`, `jit_codegen.c`, `uop_cache.c`) | rc 0 — **after an environment find**: the toolchain's standalone `aarch64-linux-cosmo-cc` is a ONE-LINE TEXT FILE (no shebang): the shell executes it as a script and DROPS EVERY ARGUMENT ("no input files"), and it never sets the sysroot; it fails on UNTOUCHED files. The gates invoke `aarch64-linux-cosmo-gcc` directly with cosmocc's own driver flags (`-fno-pie -nostdinc -isystem $COSMO/include -include libc/integral/normalize.inc`) — all four rc 0 |
| `--jit off` | byte-identical to the pre-JIT path (the bench's identity asserts + the transcript legs) |
| bench A/B | §0/§2.2 (interleaved, identity-asserted) |
| mutation arms G1a–G1e | each FAILS as it must, restored cmp-clean |
| probe failure path | reported, JIT disabled, transcript == JS reference |

**A post-mortem the next unit should read (the .fixed race).** Gates run 1 was invalidated by this
unit's own concurrency: the probe edits landed while the script's `make` was running. Worse, run 1's
G1a mutation SURVIVED its restore into the tree, and run 3's setup then saved `jit_codegen.c.fixed`
FROM the already-mutated file — so run 3's restores propagated a tree whose chains COUNTED but never
transferred. Every guest gate stayed green on it (exactly what G1a predicts: the chain is
semantically inert, and its absence is invisible to every guest gate by design). The A/B caught it
(1.877 s vs 1.498 s), `jit_stats.execs` (15,317,242 vs 243,784) and the emitted block sizes
(+2 B/block: `jmp rel32` vs `jmp r11`) diagnosed it. **Rule this cost something: a gates script that
saves `.fixed` copies must assert the tree is clean FIRST, and a measurement that disagrees with an
earlier measurement of "the same" tree is a finding, not noise.** The summary file
(`results/jit-6/gates-summary.txt`) is run 4's — the only rc=1 rows in it were the aarch64 rows,
re-run green from the patched script and appended (worst rc 0).

## 7. Files

- `src/cpu/jit_x64.{c,h}` — `jx64_jmp_r64` (FF /4), `jx64_mov_r64_r64`, `jx64_test_r64_r64` (the chain stub's needs; encodings mirror the 32-bit forms).
- `src/cpu/jit_mem.{c,h}` — `jit_chain_target` / `jit_chain_bind` + the executing-block statics (`jit_cur_page`/`jit_cur_block`; the never-called `jit_exec_block_geom` + its geometry statics were DELETED in JIT-6F — see §2.1's correction); `jit_platform_probe` + the caps line; the chaining contract in the header comment.
- `src/cpu/jit_codegen.{c,h}` — frame 56→72 B (the stub's spill slots), the chain stub + chain entry + `l_ck_tail` rewiring, the `chains` counter + the exits/links print, `jit_chain_bind` after a successful bind.
- `src/cpu/uop_cache.c` — the ABORT continuation, whose geometry is the ENTRY block's; JIT-6F documented the same-page invariant that makes that exact at the site and removed the dead geometry read (nothing else moved).
- `src/cpu/jit_selftest.c` (+10), `src/cpu/jit_codegen_selftest.c` (+27) — the two new gates.
- `results/jit-6-gates.sh`, `results/jit-6/` (binaries, instrument builder + calibration, gate logs).

Constraints honoured: the invalidation hooks, the fault contract and the staircase untouched (the chain rides them, counted); the JIT's default untouched; no second backend (aarch64 compiles the new paths out; the probe is x86-64-section code, stubs elsewhere); `v86/` untouched; no git operations. The peer lane's files (`fx-demo/`, `tests/`) were not touched by this unit.
