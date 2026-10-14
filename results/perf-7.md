# PERF-7 — the audit's ranked candidates, landed: F1+F2+F4 stacked at −40.5%, F3 resolved by measurement

**Unit:** `HANDOFF=v86c-audit`, unit PERF-7 (coder). **Inputs:** `/tmp/brief-v86c-perf-7.txt`,
`results/audit-3-perf.md` (the specification — its measured ratios, method and caveats),
`results/jit-plan.md` §4/§6/§7 step 8, `results/jit-6-headroom.md` + `results/jit-6f.md`,
`results/perf-5-tune.md` (the A/B discipline), `results/secfix-1.md` (not undone: every
count in the gate table below is IDENTICAL to the audited tree's except this unit's own
matrix growth). **Raw evidence:** `results/perf-7/` (every A/B block, the gate logs, the
mutation rig's four transcripts, the instrument run, the repaired attribution rig's rerun).
**Memory:** `handoff-v86c-audit3-result`.

Claims are tagged **MEASURED** (this host, this session, the raw block cited in
`results/perf-7/`), **READ** (source, file:line) or **INTERPRETATION** (marked).

---

## 0. Bottom line

The rig is `results/perf-7-ab.sh` — `perf-5-ab.sh` plus the audit-3 §I.3 pin (`taskset -c 8`;
unpinned noise on this host was 1.037x/1.052x, pinned collapses to ~0.3%). Every run of every
block below is identity-asserted by the rig: 200,095,214 instructions, terminal state
`eip=0x21972c1 cs=0x10 cr0=0x60000011 cr3=0x0`, serial sha `fae4924b…`. Load quoted per block.

**The noise floor, measured (not assumed):** the same binary in both arms, 4 pinned interleaved
pairs → **best 1.0036x / median 1.0043x** (`results/perf-7/noise-floor.txt`), load 1.1–1.4.
Baseline this session: **1.400–1.432 s best / 1.405–1.432 median** across the blocks
(load 0.3–1.9 throughout).

| # | candidate | alone (best/median ratio) | verdict |
|---|---|---|---|
| **F1** | skip the per-uop `call l_checks` for provably-continuing recipes | **0.7010x / 0.7007x (−30%)**, load 1.05, 5 pairs, full separation (worst-B 1.003 < best-A 1.418) | **LANDED** — the audit's number reproduces to three digits |
| **F2** | the shift recipes (`shl/shr r32, cl`, D3 /4 and /5 reg) + their matrix arms | **0.9446x / 0.9490x (−5.5% / −5.1%)**, load 1.8, 5 pairs, full separation | **LANDED** — same direction, mid-single digits; the audit's −6.8% was measured on a49a887, this tree carries SECFIX-1 |
| **F4** | fold `in_mapped_range` into the TLB mask | **0.9639x / 0.9657x (−3.6% / −3.4%)**, load 0.9, 5 pairs | **LANDED** — the audit's −3.6% reproduces |
| **stack** | F1+F2+F4 | **0.5949x / 0.5949x (−40.5%)**: 1.430 → 0.850 s best ≈ **235 MIPS**, re-measured after the last source change at **0.5944x / 0.5950x** | **all three together, gates green** |
| **F3** | the per-TU link (PERF-1's unity TU "has inverted") | `--jit on`: unity **1.066x FASTER** (0.855 vs 0.912 median); `--jit off`: unity 1.35x faster (2.756 vs 3.707) | **NOT LANDED — and the audit's tension DISSOLVES: on the tree F1 creates, both engines prefer unity. Keep unity.** (§F3) |

`make bench` protocol on the final tree: best **0.820 s / median 0.895 s = 244.02 MIPS at best**
(`results/perf-7/f-bench.log`) — the shipped tree's pinned 1.457–1.470 s is **1.76–1.79x
faster** by the same protocol.

**The audit's projection arithmetic held end to end:** it projected 0.5929x/0.5917x for the
stack; this unit measured 0.5949x/0.5944x. Every staircase counter on the stacked boot is
byte-identical to the shipped tree's (§4).

---

## F1 — the checks-skip criterion, as written down (rank 1, −30%)

The criterion lives at the emission site, `src/cpu/jit_codegen.c` (`jitcg_uop_continues`,
immediately above `jitcg_emit_uop`), with the proof obligations in the comment there. It is a
property of the TABLE, and each clause retires one executor step:

| clause | retires | mechanical ground |
|---|---|---|
| `rec != NULL && rec->kind != 0` | — | a CALLED uop runs a C body, which can set anything steps 6–11 test |
| no `UOP_FLAG_FLUSH`/`UOP_FLAG_BLOCK_END` | steps 7 (first disjunct) and 10 | decode-time uop flags |
| kind ∉ {JCC, JMP, LEA} | steps 8/9/11 (they need `ip == eip+len`) | these three are EXACTLY the recipes that write `cpu->instruction_pointer` (READ: `rg 'JC_OFF(instruction_pointer)'` over the emitters finds only `rec_jcc`, `rec_jmp`, `rec_lea`, plus `jitcg_emit_stores`' own step-3 store). A fourth ip-writing kind would fail the matrix's ip comparison; the exclusion and the emitters agree |
| `((u->eip ^ (u->eip+u->len)) & ~0xFFF) == 0` | step 8 (page cross) | for a recipe, `ip` stays at the step-3 store `body_ip`, and `body_ip == eip + len` for every non-custom-modrm uop (the +1 exception is 0x8D/0x8F — LEA/POP, not admitted). NOT vacuous: an instruction may END at its page's last byte (the decoder only drops instructions whose BYTES leave the page, `uop_cache.c` DEC_PAGE_END); for those the checks run and step 8 flushes |
| (steps 6/7-second-disjunct/9) | `interpreted_unimplemented`, `jit_block_boundary`, the iteration limit | set only behind C bodies (`cpu.c:66`, `cpu.c:78`, sti's nested dispatch); a recipe's FAST path runs no C body — its gates route every failure to the SLOW arm, which keeps the full checks (the emission re-enters `l_checks` there, counts first, exactly the executor's count-then-check order) |

**The elision battery (the brief's G2 differential for the ELIDED shapes):**
`test_continuation_elision` in `src/cpu/jit_codegen_selftest.c`:

* (a) six consecutive recipe uops in one block: the compile-time counter
  (`uops_checks_elided`, new) must read exactly 6, dynamic inlined 6, called 1 (the hlt), and
  the FULL state — `instruction_counter` and `uop_pending` included, where a wrongly-elided
  flush or a missed pending count would show — identical across all three engines.
* (b) THE EXCLUDED SHAPE: `add eax, ecx` placed to END at the page boundary (eip 0x1FFE,
  len 2): exactly ONE recipe uop in the page keeps its checks (the counter reads
  `inlined − 1` = 4090 of 4091 on the probe), and the state matches everywhere.
* The OTHER excluded shapes are driven by existing pins: JCC/JMP/LEA by the matrix's
  jcc/jmp/lea cases and `test_tail_exit`; FLUSH by `test_staircase`'s cmpxchg arm;
  BLOCK_END by its sti arm.
* **The flag clause's invariant pin:** the landing scan over all 740 generated descriptors
  found **no recipe encoding carrying `UOP_DESC_FLUSH` or `UOP_DESC_BLOCK_END`** — so that
  clause has no firing instance today, and a mutation of it would be a test that cannot fail.
  Its instrument is instead pin (d) in `test_recipe_table`: any future table entry over a
  flagged descriptor fails `make selftest` and names the clause.

**The asymmetry rig** (`results/perf-7-mut-rig.sh`, raw in `results/perf-7/mut-*.txt`):

| arm | mutation | suite result |
|---|---|---|
| control | none | **3525 checks, 0 failures** (the rig can see green) |
| mut1 | criterion → `return false` | **3 failures** — the engage pins (elision not firing is VISIBLE) |
| mut2 | the kind exclusion dropped (JCC/JMP/LEA elided) | **139 failures** — the jcc/jmp/lea matrix cases and the tail pins |
| mut4 | F2's count mask dropped (count test reads unmasked CL) | **11 failures** — the CL=0x20-masked-to-0 arm |

Overall rc 0: every arm was caught.

## F2 — the shift recipes and their matrix arms (rank 3, −5.5%)

`rec_shift_r32_cl` (`src/cpu/jit_codegen.c`) transcribes `instr32_D3_4_reg`/`instr32_D3_5_reg`
(`write_reg32(r1, shl32/shr32(read_reg32(r1), read_reg8(CL) & 31))`) with arith.c:803–865's
flag semantics. The load-bearing decisions, all stated at the emitter:

* **The count is the guest's own CL** — the body masks `& 31`, and the native 32-bit shift
  masks its count register to 5 bits internally: the same number, so no count
  materialisation. The count-0 test reads the SAME masked count.
* **count == 0 stores NOTHING** (the body's early return: no write-back, no flag state) — the
  `jz` skips the whole tail.
* CF is the native shift's own CF (`setc` immediately after — the last bit out is arith.c's
  `b`); OF per direction from the values the body uses (`(b ^ sign(result)) << 11` for shl;
  the ORIGINAL's sign bit for shr, which is why the original sits in JC_VAL across the shift).
* PF/ZF/SF stay lazy via `last_result`/`flags_changed` — the same stores the other ALU
  recipes make.
* Register forms only: the mem forms carry the invalidation hook and the straddle gates and
  are a separate look (the audit's own remaining-work note).

**The matrix arms (the audit's explicit caveat, closed):** the codegen matrix GREW
**3006 → 3525 checks** (`results/perf-7/f-selftest.log`; every other suite count identical).
The new arms: both directions composed (repeat 2) and pinned (repeat 1) with the values
chosen so CF and OF land NON-trivially (`setup_matrix_shift`: `shl eax,cl` at CL=5 gives
CF=1 OF=1; `shr edx,cl` on a negative original gives CF=1 OF=1), the **count-0 case**
(CL=0: `flags_changed` must STILL be 0), **the 5-bit mask case** (CL=0x20 masked to 0 —
the same pin), the mask's other end (CL=0xff → count 31), a lazy-flag-state shift followed
by `jae`, and a recipe/called/shift mixed block. Plus the two table pins keeping the entries
findable (a dropped entry is a 74%-of-called-toll slowdown — invisible to every state gate).

## F4 — the TLB-mask fold (rank 4, −3.6%)

`jitcg_emit_mem_prep` adds `TLB_IN_MAPPED_RANGE` to the fast-path mask and deletes the
per-access re-derivation (`cmp memory_size` + the device-window `lea/cmp`, five instructions
per inlined access). **Why the fold is exact, not approximate:** the entry carries the bit
from fill time, `in_mapped_range(page_base)` (paging.c:249–257); `memory_size` is always
page-aligned (`main.c`: `mib << 20`) and the 0xA0000..0xC0000 window is two page multiples,
so `in_mapped_range(addr)` is constant across each page — the bit IS the predicate the
deleted code re-derived. Cross-checked against the audit's note and against upstream: v86's
own JIT mask (`v86/src/rust/codegen.rs` `gen_safe_read`/`gen_safe_write`) masks off exactly
the must-be-clear bits including the mapped-range bit. A slow-path hit re-runs the C helper,
which re-tests `in_mapped_range(phys)` itself — the fold moves WHERE the predicate is
evaluated, never its answer. The existing `test_tlb_fold_bits` pin (entry bits + the
declined/read-only decision + inlined engagement) passes unchanged.

## F3 — the unity TU: measured, NOT landed, and the tension dissolves (G4)

The audit measured, on a49a887 (pre-F1): per-TU `--jit on` **0.758x** (−24%) but
`--jit off` unity **1.34x better**, aarch64 = the uop engine → a policy trade-off it declined
to land. This unit re-measured BOTH legs on the tree the unit actually creates (F1+F2+F4
landed), same rig, identity-asserted:

| leg | unity (stack tree) | per-TU (stack tree) | ratio |
|---|---|---|---|
| `--jit on` (default), 4 pairs, load 0.35 | 0.855 / 0.855 | 0.906 / 0.912 | **unity 1.066x faster** (`ab-f3-jiton.txt`) |
| `--jit off`, 3 pairs, load 0.33 | 2.734 / 2.756 | 3.692 / 3.707 | **unity 1.35x faster** (`ab-f3-jitoff.txt`) |
| repaired `results/perf-1/revert-attribution.sh`, 1 run | 0.820–0.855 | 0.914 | consistent (`revert-attribution-rerun.txt`) |

**The audit's −24% does not survive F1.** MEASURED here: once the per-uop checks call is
elided (F1), the unity-vs-per-TU comparison on the JIT path INVERTS BACK to unity +6.6%.
INTERPRETATION (marked, not located — the audit's own bisect of which file's separation paid
was never done): the audit's per-TU win ran through the code the generated blocks CALL —
`uop_exec_uop`, `jit_chain_target` — whose toll F1 cut from 17.7M to 4.67M calls per boot;
what remains of that path is too small for the link shape to matter, and the executor-side
inlining PERF-1 shipped still pays for itself. The `--jit off` inversion (unity 1.35x) is
EXACTLY the audit's number and PERF-1's mechanism — untouched, as expected: F1/F2/F4 are
codegen-side and change nothing for the C executor.

**The options, with the numbers attached:**

1. **Keep unity (RECOMMENDED — this is also what the tree already is).** Both engines prefer
   it on the measured tree: the default path by 6.6%, the uop engine (and aarch64, which runs
   the uop engine — the JIT is compiled out) by 35%. Cost: none; this is the shipped shape.
2. Split into two clusters (per-TU for the JIT's callees, unity for the executor). Would buy
   at most the 6.6% the measurement says is NOT there (per-TU is SLOWER on the stacked tree) —
   refuted by measurement, do not build. Its build cost is a second object list and a bisect
   rig (the audit's follow-up) for a negative return.
3. Gate the unity per engine (`--jit off` links unity, default links per-TU): would cost the
   uop engine 35% and the default 6.6% — strictly worse than option 1 on both legs.
4. Anything better found: the only remaining link-shaped idea the measurement leaves open is
   the audit's bisect (which file's separation could pay) — but its premise (per-TU pays on
   the JIT path) is now false, so the bisect has nothing to find.

**aarch64 (no APE bootable here, so this is the proxy, stated as such):** the aarch64 build
runs the uop engine; the x86 `--jit off` leg is the audit's own proxy for it and it says unity
is worth **1.35x** there. The four JIT files compile aarch64 rc 0 in both the unity and the
per-TU shape (the link shape is a host-link decision; the aarch64 compile is per-file).

**Decision requested: none — option 1 is the status quo.** F3 is a measurement and a
refutation of the audit's rank-2 premise, not a landed change.

## The extra items the brief named

* **`results/perf-1/revert-attribution.sh` rig rot — FIXED.** Reproduced first (the link died
  on `uop_arms/uop_entries/uop_descs/uop_0f_cases undefined` — it predates the uop cache and
  linked without `$(UOP_OBJ)`; the audit's exact finding), then repaired: `$(UOP_OBJ)` on the
  target line and the link line, with the repair documented in the script's header. It now
  runs to a verified benchmark block (`results/perf-7/revert-attribution-rerun.txt`, per-TU
  0.914 s, identity-asserted).
* **The "There is no JIT" comment fossil at `src/cpu/paging.c:249` — FIXED** (comment-only):
  the text now says the port HAS a JIT, that code protection rides the per-store bitmaps
  (uop_page_code / PERF-FIX-3's hooks) and NOT `TLB_HAS_CODE`, and names the condition under
  which that would change.
* **The attribution instrument's two defects from `results/jit-6r-review.md` — NOT still
  there.** The tree's `results/jit-6/make-rdtsc-variant.py` is sha256
  `9472877434ad83b0…` — byte-identical to the copy JIT-6F recorded as the fix: the first
  segment is dropped (`j6_seg_init`), only complete segments are summed (the TSC-grafting
  defect), and the prologue/epilogue exclusion is stated in the instrument's own printed NOTE
  (the reviewer's accepted alternative). MEASURED: the tree's copy hashes exactly the
  recorded fix; nothing to do. The instrument was then run once on the STACKED tree
  (identity-asserted, `results/perf-7/j6-run54.txt`, J6_PAIR_COST=54):
  `t_uoprun` 3.017 G cycles, dispatch 58.4 M ≈ **1.9%**, interior ≈ 98% — the between-block C
  dispatch the chain did not remove is the same ~60 M absolute as the audit measured pre-stack;
  its share grew because the interior halved. Where the remaining time is: inside the compiled
  blocks (memory model + the ~4.7M called bodies), exactly as the audit's §I.2 said.

## Gates (G3) — the final tree, raw logs in `results/perf-7/f-*.log`

| gate | result |
|---|---|
| `make minibios-mem` | rc 0 |
| `make` | rc 0 |
| `make selftest` | rc 0 — cpu 3522 / m1c 228 / hw 551 (0 skipped) / uop_cache 7,934,349 / uop_exec 102 / jit 68 / **jit_codegen 3525 (was 3006: F2's arms + F1's battery + the flag-clause pin — the ONLY count that moved, and it moved UP)** |
| `make nasm` | rc 0, **118/118**, 0 of 123 produced no state, 5 skipped (the pinned skip lists, incl. SECFIX-1's `jump`) |
| `make transcript-diff` | rc 0 — byte-identical to the JS reference |
| `--jit off` vs `--jit on`, `tests/test.img 1000000` | serial outputs byte-identical to each other |
| `sh tests/gate-m2-5-determinism.sh` | rc 0, **0 FAILING**, both runs 4,000,024,062 instructions, transcripts byte-identical (the absolute count is the guest-clock class PERF-5 §7 documents; the gate asserts run1 == run2 and passed) |
| `sh tests/gate-reboot-1.sh` | rc 0, **39 passed, 0 failed** |
| `make bench` | rc 0 — best **0.820 s / median 0.895 s = 244.02 MIPS** at best, every run verified |
| aarch64 (cosmocc's own driver line, `-Werror`, jit_mem/jit_x64/jit_codegen/uop_cache) | rc 0 × 4 |

**The staircase, byte-identical to the shipped tree** (`results/perf-7/stack-stats.txt`,
200 M boot, `--uop-cache-stats --jit-stats`): flushes **257,574**, invalidated blocks **308**,
block ends **4,217**, SMC aborts **11** (uop side) / exits abort **10**, exits return
**234,330** / tail **5,227**, chain links **15,073,458** — every number equal to the audit's
§0.1 shipped-tree record. Lookups 15,317,515 vs 15,317,242 (+273: the chain's known
double-count on a missed tail, disclosed in jit-6r-review §2.3, pre-existing). Dynamic
inlined **97.67%** (195,429,258 / called 4,665,955) — exactly the audit's F2 projection
("91.14% → 97.67%; called 17.72 M → 4.67 M"). Mean emitted size **162.3 B/uop** (the audit's
stack predicted 162.1). Elision engagement on the boot: **6,817 of 8,330 recipe uops emitted
without the checks call**.

## Negative results and non-takebacks

* **F3's per-TU link (rank 2): REFUTED on the tree this unit creates** — the audit's −24%
  premise does not survive F1 (§F3). Not landed; unity stays. This is the unit's one
  headline negative, and it is a good one: the audit's policy dilemma (24% vs 34%) does not
  exist on the post-F1 tree.
* **No candidate needed reverting:** all three paid above the 0.36% noise floor with full
  A/B separation, so nothing here repeats PERF-5's four-decline pattern. The only number that
  came in below the audit's projection is F2 (−5.5% vs −6.8% best): the audit measured on
  a49a887, this tree carries SECFIX-1; the direction and the scale (mid-single digits,
  13x the floor) reproduce.
* **mut3 (mutating the criterion's FLUSH/BLOCK_END clause) deliberately NOT run:** the
  landing scan found no recipe encoding carrying either flag, so the mutation would change
  nothing and prove nothing — a test that cannot fail. The clause's instrument is the
  invariant pin instead (§F1). This is the rig's own documented decision, not an omission.

## What this tree does NOT claim

* F1's −30% is measured on ONE workload (the 200 M Alpine boot). The audit's transferability
  note stands: it removes fixed per-uop cost, so it should transfer to any JIT-bound
  workload, compress on SMC-heavy or reboot-heavy shapes, and do nothing for aarch64.
* The F1 criterion's step-7 argument leans on "a recipe's fast path runs no C body" — true by
  construction of the emitter switch today, pinned by the matrix (which runs every recipe
  differentially against the executor) and by the engage pins. A future recipe that calls
  into C on its FAST path must keep the checks (its table entry would need `slow = true`
  treatment in the criterion) — the comment at `jitcg_uop_continues` says so.
* `instruction_counter`'s absolute value in the determinism gate (4,000,024,062 this run vs
  4,000,053,925 pinned at JIT-6F) is the guest-reads-the-host-clock class PERF-5 §7
  documents; the gate's own assertion (both runs EQUAL, transcripts byte-identical) passed.

## Raw evidence, file by file (`results/perf-7/`)

| file | what |
|---|---|
| `noise-floor.txt` | the instrument's resolution: same binary, 4 pinned pairs (1.0036x/1.0043x) |
| `ab-f1.txt`, `ab-f2.txt`, `ab-f4.txt` | each candidate ALONE, 5 pinned pairs, identity-asserted |
| `ab-stack.txt`, `ab-final.txt` | the stack, before and after the last (comment-only) source change |
| `ab-f3-jiton.txt`, `ab-f3-jitoff.txt` | F3's two legs on the stacked tree |
| `stack-stats.txt` | the 200 M boot's staircase counters + coverage + elision engagement |
| `j6-run54.txt` | the rdtsc instrument on the stacked tree (dispatch 1.9%) |
| `mut-control.txt`, `mut-mut1.txt`, `mut-mut2.txt`, `mut-mut4.txt` | the asymmetry rig's four transcripts |
| `f-make.log`, `f-selftest.log`, `f-nasm.log`, `f-transcript.log`, `f-det.log`, `f-reboot.log`, `f-bench.log` | the gate set on the final tree |
| `revert-attribution-rerun.txt` | the repaired PERF-1 attribution rig, running |
| `../perf-7-ab.sh`, `../perf-7-mut-rig.sh` | the two rigs, re-runnable |
