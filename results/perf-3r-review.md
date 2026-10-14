# PERF-3R — review of PERF-FIX-3 (the invalidation blocker + two should-fixes) and PERF-FIX-2 (the assert sweep + the benchmark)

HANDOFF=v86c-perf, unit PERF-3R (reviewer).  Under review: `results/perf-fix-3.md` (+ the code
in `src/cpu/paging.h`, `src/cpu/memory.c`, `src/cpu/uop_cache.c`, `src/cpu/uop_exec_selftest.c`,
`results/perf-fix-3-recon-check.py`, `results/perf-4-recon.sh`, `results/perf-fix-3-mut-rig.py`)
and `results/perf-fix-2.md` (+ `src/cpu/instructions.c`, `tests/perf_bench.sh`,
`results/perf-fix-2-sweep.py`, `results/perf-fix-2-verdicts.py`, `results/perf-fix-2-sweep-verdicts.tsv`).
Context read per the brief (ctx / result / review2 / plan nodes + `results/perf-2r-review.md`).

## 0. Verdict

**Both units' fixes are correct and the invalidation class is now genuinely gated.**  The
BLOCKER-1 macro hook is right in every arm (straddle arms verified to route through the hooked
`write8`; no double hook; invalidate precedes the write).  The straddle-hook conditions are
correct and can't wrap.  The 2b fix matches the generated dispatcher's resolve-fault `return`
exactly, and **the 2a refutation is CONFIRMED at all three levels** (upstream generator, the
port's generator, the generated artifact): the review's prescribed reorder really would have
introduced #GP-where-#NM — refusing it was right.  The cpu.c:100 KEEP verdict is sound
(`in_mapped_range` genuinely stands in for the wasm bounds trap; both measured legs support it).
The three far-transfer drops have verbatim upstream provenance and measured asymmetry with a
byte-identical reference transcript.  The benchmark can fail on three independent pins and does.

**One SHOULD-FIX finding (report-only, no code change): the recorded "residual" in
perf-fix-3.md §3/G4 misreads the reference — the generated dispatcher's postfix runs AFTER the
body call, unconditionally, so for a mid-body fault the reference DOES end the batch and the
executor already matches it.**  A later staircase unit would otherwise build a paging fixture to
fix a non-defect.  Details below.

No BLOCKER.  Findings:

---

## SHOULD-FIX-1 (report correction — perf-fix-3.md §3/G4's "residual" does not exist)

**file:** `results/perf-fix-3.md:199-213` (the paragraph "A RESIDUAL OF THE SAME CLASS …").

**The claim:** a fault raised *inside the instruction body* (e.g. `SAFE_READ_WRITE32`'s write
translation faulting under paging) on a FLUSH-flagged encoding "returns from the INSTRUCTION
body, so the reference's postfix is skipped there too" — therefore the executor's `ran=true →
flush` diverges and "needs a probe per shape".

**The artifact:** the postfix is NOT part of the instruction body.  In the generated dispatcher
it is emitted after the body call, unconditionally —
`src/cpu/interpreter_gen.c:12165-12169`: `instr_0FB0_mem(addr, modrm_byte >> 3 & 7); … }
after_block_boundary();`.  Same structure in upstream's generator
(`v86/gen/generate_interpreter.js`: `instruction_postfix` is pushed after the mem/reg call, and
the guard/`return`s that skip it are the dispatcher's OWN `return`s, not the body's).  A body
that faults via a helper and returns still reaches the postfix — the reference ends the batch.
The executor's current predicate is therefore exact: flush iff the body ran, and the body does
not run in exactly the two cases where the dispatcher's own `return` skips the postfix
(guard-fail, resolve-fault — the 2b case).  Mid-body fault: reference runs the postfix, executor
flushes.  **Match in all three sub-cases; there is no residual.**

**Fix:** rewrite the paragraph (and the "flagged for the reviewer with that reasoning" sentence
in §6 of `results/perf-fix-3.md`) to state that mid-body faults are already faithful, citing
interpreter_gen.c's call-then-postfix structure.  No code change; this is a record a future unit
is explicitly invited to act on, and it would send the staircase owner to build a paging fixture
for a non-divergence.

(Not a defect, noted for completeness: the same paragraph's parenthetical "(the flush still
lands after it)" reasoning and the stated limit of the 2b pin are correct as far as they go — the
2b pin is indeed a bookkeeping counter, and a guest-visible discrimination for 2b itself is
probably unbuildable because the handler's iret is itself a batch end.)

---

## NICE-TO-HAVE-1 (the classifier's teeth, one more shape)

**file:** `results/perf-fix-3-recon-check.py:32`.

`WRITER_DIRECT = re.compile(r"mem8\s*\[[^\]]*\]\s*=[^=]|…")` misses **compound-assignment
stores**: `mem8[i] &= v`, `|=`, `+=`, `-=`, `^=`, `<<=`, `>>=`, `mem8[i]++`.  Such a write
exists in the tree today (`src/cpu/m1c_selftest.c:808`, exempt by file so harmless), but a
future non-selftest compound write would pass the gate silently — precisely the failure mode the
gate exists to catch (a new writer shape that is a write but not in the list).  One-line fix:
add `|\s*(\+=|-=|\|=|&=|\^=|<<=|>>=|\+\+|--)` to the alternation.

## NICE-TO-HAVE-2 (the hook check is presence-only, and one printed label is wrong)

**file:** `results/perf-fix-3-recon-check.py:141` (hook window) and `:87-96` (enclosing_function).

(a) The hook test `any(HOOK.search(lines[j]) for j in range(i-8, i))` accepts a hook on a
DIFFERENT write (no address match) and even a hook named in a COMMENT (the comment-skip applies
only to the write line, not the scanned window).  A new writer with an unrelated hook 7 lines
above passes.  Suggested: skip comment lines in the window scan (mirror the write line's own
skip).  Address-matching is overkill for a gate whose job is "make new shapes FAIL until
classified"; comment-skip alone closes the cheap false-positive.
(b) The printed verdict for `src/hw/kernel.c:238` names `die` as the enclosing function — the
matcher latched onto a multi-line `die("…")` call's closing `);` as a function head.  Cosmetic
only (the exemption is by TU and IS sound: every kernel.c writer is pre-guest or a loader error
stop; the reboot path does not exist), but the output names a function the hit is not in.
Suggested: require the matched head to also open a brace on the same line, or accept `)` only at
a line with no leading content.

## NICE-TO-HAVE-3 (for the orchestrator, before PERF-5 spends effort — item C of the brief)

The fresh `-pg` reading is **internally inconsistent as stated**.  "interpreted_run at 46% self"
with the cache ON cannot coexist with M2's measured 0 fallback on this exact workload
(`bench-stats-on.txt`: fallback_runs 0, fallback_instr 0, decode-fails 0) — interpreted_run is
only reached through the fallback path, and the count (200,095,214) plus the resolve-collapse
(83M→7.5M) show the cache WAS working in the profiled run.  The ctx's own rule also says -pg
self-times are distorted ~7x and only call counts are usable; 46% is a self-time.  The
count half of the reading IS coherent — get_phys_eip 15.3M ≈ M2's 15,317,241 block lookups (one
per uop_run entry), and uop_materialize_addr 75.5M / get_seg 78.2M ≈ 0.38/0.39 per instruction
≈ the mem-uop fraction of the mix, with get_seg ≈ materialize + residual callers — so the
materialisation/segment target is likely right.  **Before spending effort:** re-profile with
`--uop-cache-stats` in the SAME invocation and read the fallback counters, and read gprof's CALL
column (not self time) for interpreted_run.  If calls > 0 in the -pg build but 0 in the shipped
build, the per-TU -pg build is the anomaly and the profile is not representative of the shipped
unity build.

---

## VERIFIED-OK (the rest, checked on disk — no findings)

* **BLOCKER-1 macro** (`src/cpu/paging.h:151-221`): `phys_addr_` is the right address in every
  arm.  Non-straddle arm: hook precedes `write*_no_mmap_or_dirty_check` (before the write —
  correct for the abort semantics).  Straddle arms: enter on `phys & 0xFFF == 0xFFF` (16) /
  `>= 0xFFD` (32), route through `virt_boundary_write16/32` → `write8` (both hooked, one hook
  per page touched — the `src/cpu/paging.c` implementations call plain `write8`, not the raw
  store) → **no double hook, no unhooked page**.  The read arm needs nothing (reads hit RAM;
  only execution goes through blocks).  The macro family's `phys&0xFFF == virt&0xFFF` property
  holds (4K/4M pages are aligned), so the straddle test can't misfire; both straddle arms are
  upstream-verbatim anyway.  The `#include "uop_cache.h"` is benign: it pulls only cpu_state.h +
  modrm.h, no cycle back to paging.h, and the `uop_invalidate_page_fast` inline is self-contained
  (bitmap externs declared in the same header; a not-yet-initialised cache reads len 0 → no-op).
* **SHOULD-FIX-1** (`src/cpu/memory.c:374-404`): conditions correct (`== 0xFFF` for 16-bit;
  `>= 0xFFD` for 32-bit); `addr + 1`/`addr + 3` cannot wrap because the non-mmap arm implies
  `addr < memory_size`.  Both comments now state the truth.  Pinned by `straddle_case(false|true)`
  which asserts the invalidation delta AND the observable (0x3000 stays 0x11), with pre-fix legs
  measured failing in the mut-rig (arms 2/3).
* **SHOULD-FIX-2b** (`src/cpu/uop_cache.c:979` + the caller's gate at `:1210`): `return false`
  on materialise fault; the flush gate `ran && (FLUSH || jit_block_boundary)` — matches the
  dispatcher's `if (cpu->pagefault) return;` (before the postfix) exactly.  `uop_pending += 1`
  stays unconditional, matching the reference's loop counter.
* **SHOULD-FIX-2a refutation, verified at all three levels the unit named:**
  `v86/gen/generate_interpreter.js:233-251` puts the guard in `instruction_prefix`;
  `gen/interpreter_c.js:200-203` emits `if (!task_switch_test()) { return; }` before
  `int32_t addr = modrm_resolve(...)`; the generated `src/cpu/interpreter_gen.c` has the guard
  before the resolve in the cases cited.  The review's prescription was backwards; refusing it
  with a falsifying pin (#NM 7 vs #GP 13, cache-on/off) was the right call.
* **The four new pins** (`src/cpu/uop_exec_selftest.c:301-651`): `test_smc_rmw` uses a real RMW
  patch (`xor byte [mem], imm` → `instr_80_6_mem`, descriptor 0x45/0x00 at `src/cpu/uop_gen.c:274`
  — neither block end nor flush point, as claimed) and asserts the abort AND the invalidation AND
  the observable; `machine_reset` clears the cache between tests so no cross-test pollution;
  `test_guard_order` asserts the uop path was taken (uops_executed delta) before reading the
  vector; `test_faulted_flush` names each of the four expected batch ends with their table
  derivation and asserts the guest's rdtsc equality with the stated limit.
* **The classifier** (`results/perf-fix-3-recon-check.py`): runs green on the current tree
  (123 hits, 19 hooked, 104 exempt, 0 FAIL, exit 0).  The macro bodies in paging.h are checked
  (the hook line precedes the store).  Exemptions verified: the `*_no_mmap_or_dirty_check`
  definitions are the stores themselves; kernel.c/main.c writers are loader-time or loader error
  stops (reboot does not exist); selftest files are exempt by design (library default OFF).  The
  scanned shapes miss only what §NICE-TO-HAVE-1/2 name plus the stated function-pointer/alias
  blind spots (both measured empty today: no asm blocks in src, no `&mem8[` aliases, the only
  handed-out pointer is const and used as a memcpy source).  `results/perf-4-recon.sh` invokes it
  on `src/` and exits with its code — arm 1 of the mut-rig measured rc 1 on the pre-fix tree.
* **cpu.c:100 KEEP** (perf-fix-2 §1.2): sound.  `in_mapped_range(addr)` =
  VGA window or `>= memory_size` (`src/cpu/memory.h:93-96`), so `!in_mapped_range` is exactly
  "inside the mem8 allocation and not the LFB" — the assert genuinely stands in for the wasm
  bounds trap, which is release-live in the reference.  The two measured legs (one line deleted →
  same stop at cpu.c:112; both deleted → SIGSEGV) plus the reference's own trap/hang support the
  verdict.  KEEP is right and the brief's remedy is refuted by its own measurement.
* **The three far-transfer drops** (`src/cpu/instructions.c:1164-1172, 2329-2336, 2358-2367`):
  upstream provenance verified verbatim (`v86/src/rust/cpu/instructions.rs:1001-1005,
  2462-2469, 2490-2494` — `dbg_assert!(false)` under `!protected_mode || vm86_mode()`).
  The drops are clean (assert removed, class comment at the site), and the asymmetry is measured
  (before rc=134 at each site, after rc=0, transcript byte-identical to the reference in all four
  probe modes including the <64KiB control).  The value feeds `instruction_pointer =
  get_seg_cs() + eip` in both implementations — safe to allow.
* **The sweep** (`results/perf-fix-2-sweep.py`): runs rc 0 on the current tree — 340
  invocations, 120 conditions, 47 upstream-matched, 0 un-verdicted (raw grep = 356 incl. comment
  mentions, consistent).  The TSV's one blank-verdict row is the comment line, not a hole.  The
  17 DROP rows are absent from the TSV by construction (the sites are gone); a re-introduced
  dropped assert appears as a new un-verdicted group and trips rc 3.  Spot-checked KEEP(c)
  reasoning holds (shift counts masked `imm & 31` at the call sites, `instructions.c:1414+`;
  the F2/F3 prefix asserts measured unreachable).
* **The benchmark** (`tests/perf_bench.sh`): can fail, and does — three independent pins
  (count, terminal state, serial sha256), each extraction strict (a changed stderr format fails
  loudly), each failure message names what changed and why it matters; G3 measured rc=1 on all
  three perturbed arms.  The pin premise is refuted with attribution to M2's sti bug, and the
  header now states what the count is (a batch boundary) and is not (a guest-visible claim).
* **Item C call-count half** (for the orchestrator's benefit): coherent as stated — see
  NICE-TO-HAVE-3.  `uop_materialize_addr` (`src/cpu/uop_cache.c`) is register reads +
  `get_seg_prefix`, no resolve calls hide inside it; the 75.5M/78.2M pair is consistent with a
  ~38% mem-uop mix.

## COVERAGE

READ IN FULL: `src/cpu/paging.h` (223), the write region of `src/cpu/memory.c` (330-440),
`src/cpu/uop_cache.c:870-1030` and `:1150-1260` (invalidation + executors + flush gate),
`src/cpu/uop_cache.h:295-385`, `src/cpu/uop_exec_selftest.c:301-651` (the four pins + IDT
machinery), `results/perf-fix-3-recon-check.py` (162), `tests/perf_bench.sh` (200),
`results/perf-fix-3.md` (307), `results/perf-fix-2.md` (347).  READ IN PART (the regions that
matter): `src/cpu/instructions.c` (the three dropped sites + the class header), `src/cpu/cpu.c`
(95-120), `src/cpu/paging.c` (safe_write64/128 arms, virt_boundary_*), `gen/interpreter_c.js`
(195-278), `v86/gen/generate_interpreter.js` (228-332), `src/cpu/interpreter_gen.c` (the
call-then-postfix structure at 12155-12175 and 140-185), `src/cpu/modrm.c` (108-130),
`results/perf-4-recon.sh` (invocation + section E), `results/perf-fix-3-mut-rig.py` (arms +
restore), `results/perf-fix-2-sweep.py` / `-verdicts.py` / `-sweep-verdicts.tsv` (structure +
verdict census).  INVENTORIED BY GREP: compound-assign mem8 stores, asm blocks, pointer aliases
of mem8, strcpy/memmove writers, all plain router callers' hooks, every classifier hit's verdict.
NOT RE-RUN (orchestrator-verified per the brief): make/nasm/selftest/transcript-diff/bench, the
boots, the mut-rig and probe rig executions — their scripts' structure was read instead.
The `-pg` profile itself was not re-run (no gmon.out for the fresh numbers exists in the tree or
/tmp; the Oct 12 `/tmp/v86prof` artifact is the pre-cache baseline).

**Files that moved during the review: none.**  All hashes matched the units' recorded final
states at read time (paging.h `78e1ed03…`, memory.c `5ed5786f…`, uop_cache.c `602f887d…`,
uop_exec_selftest.c `5db14ae9…`, cpu.c `8b46c959…` (untouched), instructions.c `0d6900dc…`,
perf_bench.sh `d08ea576…`); PERF-5's in-flight edits to uop_cache.{c,h}/cpu.c were not observed
on disk during this pass.
