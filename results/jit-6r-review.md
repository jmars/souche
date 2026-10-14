# JIT-6R REVIEW — block chaining + platform probe (unit JIT-6, HANDOFF=v86c-jit)

Reviewer pass, read-only. sha256 of every file reviewed is in the memory node
(`handoff-v86c-jit-review3`). Verdict: **the chain is correct on the gate evidence and on a
line-by-line read of the staircase; one should-fix** (a false claim + dead code whose
correctness rests on an undocumented invariant) and two nice-to-haves. No blocker: no
guest-visible wrong-answer path was found on the shipped tree.

---

## THE STAIRCASE — does a chained jump skip any bookkeeping? (brief Q1)

The chain site is the checks routine's tail arm (jit_codegen.c:1290-1295), reached only AFTER
step 11 has decided "control flow left the block". Per link, the C dispatch round-trip runs
(a) state recompute → (b) uop_block_lookup → (c) decode-on-miss → (d) uop_cur_block /
uop_abort_pending set → (e) jit_block_enter → (f) prologue → (g) body → (h) epilogue →
(i) exit continuation (previous_ip, get_phys_eip, pagefault→discard) → loop top. The chain
replaces that with: chain stub (abort test, ABI spill, `call jit_chain_target`, reload,
`jmp` target chain entry) → target chain entry (D1 clear + frame slots) → target body.

Per site, what still runs vs what is elided, and why the elision is unobservable:

| bookkeeping | runs on a link? | how |
|---|---|---|
| state key (mode/cpl/segs) | YES | recomputed in jit_chain_target (jit_mem.c:566) from live `state_flags` — the same mask uop_run's loop top uses (CACHED_MASK_ALL = IS_32\|SS32\|CPL3\|FLAT_SEGS) |
| uop_stats.lookups/hits | YES | bumped in jit_chain_target (jit_mem.c:573, 582); a missed tail double-counts one lookup (chain probe + the C lookup) — disclosed as the +273 in the report §2.3 |
| instruction_counter / flush points | YES, unchanged | folds only inside blocks (jitcg_emit_flush, step 7); the elided dispatch path never flushes — the TAIL continues the batch in both engines |
| uop_pending | YES | rides R9, spilled/reloaded around the C call (jit_codegen.c:1345, 1354); stored to the global once at the run's final epilogue |
| budget window / iteration limit / page-cross | YES, unchanged | all inside the checks routine, per uop, unchanged |
| exit-kind protocol | YES | chain fires only from TAIL; RETURN/ABORT/BLOCK_END never chain (a flush ends the batch, a block end returns, the abort test falls back); the final block's exit kind is the run's |
| SMC abort | YES | stub abort test (jit_codegen.c:1338-1339) covers the window after the last uop's checks; per-uop abort test at the top of every target uop; jit_cur_page/jit_cur_block updated per link (jit_mem.c:588-589) so the deferred recycle and the abort page test stay correct |
| previous_ip | ELIDED — unobservable | between links it holds the source's last eip instead of the target's; nothing can fault in the window: the chain never walks (the walk is the C tail's, and only the C tail's), and the chain entry emits only plain stores; every block's first uop re-stores previous_ip before its body |
| get_phys_eip | ELIDED on hit — bit-exact replacement | jit_chain_target's hit arm (jit_mem.c:563-565) is get_phys_eip's own cache-hit arm verbatim (same `& ~0xFFF` gate vs last_virt_eip, same `eip_phys ^ virt`); on a miss the C tail runs the walk once — never twice, so #PF is never double-delivered |
| uop_cur_block / uop_abort_pending | NOT updated per link — page-equivalent | uop_cur_block stays the ENTRY block for the whole run; its page test and the abort counter fire exactly when jit_abort_flag does **because** every link is on the entry block's page (the chain's same-page gate). See finding 1: this invariant is load-bearing and unstated |
| stats folds (uops_executed / inlined / called) | ELIDED per link — totals identical | the chain entry deliberately does not re-zero the frame counters; the final epilogue folds the whole run once; sums equal the per-block folds |
| jit_block_enter / execs counter | per-link bookkeeping done in jit_chain_target; `jit_stats.execs` not bumped per link — disclosed in §2.2 |

**Answer to the sharpest question: no staircase bookkeeping is skipped.** The counter/flush
shape lives entirely inside the compiled blocks and the run's single exit continuation; the
elided path contains none of it. The identical flushes (257,574), aborts (11), block ends
(4,217) and the serial sha corroborate — a same-count reordering would have moved the flush
boundaries, and the flush count pins them.

## THE CHAIN'S VALIDITY CONDITIONS (brief Q2)

Target compiled (live block in the hash), same full (phys, virt, state) key, same page as the
fetch cache (`last_virt_eip`) — all probed in jit_chain_target:571-593; any failure = NULL =
the plain TAIL exit, which is the pre-chain path byte for byte. Invalidation windows: a write
to the executing page sets jit_abort_flag (jit_mem.c:662-664) → the current block exits at its
next uop boundary → deferred recycle flushed at jit_block_exit; a write to a page the chain
already left recycles it immediately (safe: its code is no longer in flight — the stub's `jmp`
has retired before any guest store can run); a write to the target page BEFORE the link means
the block was unlinked → probe miss → re-decode. The reboot path: the trigger is a
FLUSH+BLOCK_END uop (never TAIL), and the stub's abort test catches the residual window; the
clear runs only after the run has returned. The fetch cache is invalidated on every TLB flush
(paging.c:51/64/90) and cpl_changed (exceptions.h:315), so a stale-translation chain is
refused by the same gate get_phys_eip itself uses. All sound.

## FINDINGS

### 1. SHOULD-FIX — the abort-geometry claim is false; the continuation falls back to the
entry block, and its correctness rests on an undocumented same-page invariant

`uop_cache.c:1391` is the only reader of `jit_exec_block_geom`; it runs after
`jit_execute_block` (jit_codegen.c:1753-1758), which calls `jit_block_exit` BEFORE returning —
and `jit_block_exit` unconditionally sets `jit_cur_block = JIT_PAGE_NONE` (jit_mem.c:732-733).
`jit_exec_block_geom` is gated on `jit_cur_block != JIT_PAGE_NONE` (jit_mem.c:619), so its true
branch is **unreachable**; `jit_exec_phys`/`jit_exec_virt` are write-only (set at
jit_mem.c:590-591 and 724-725, read nowhere). The ABORT continuation ALWAYS runs the
`else` fallback: `phys = blk->phys_start + (ip - blk->virt_start)` with `blk` = the ENTRY block.

That fallback is correct today only because jit_chain_target's same-page gate
(jit_mem.c:563-564) confines every link of a run to the entry block's page — then
`blk->phys_start + (ip - blk->virt_start)` is the right phys for any ip in the page,
regardless of which same-page block is executing. The comments and the report claim the
opposite of what runs: uop_cache.c:1384-1389 says "jit_exec_block_geom reads the substrate's
own last-entered record", and results/jit-6-headroom.md §2.1 says the continuation "re-derives
phys from the substrate's last-entered block (jit_exec_block_geom) instead of the entry block".
Both are false. This is the project's own named class — a seat that is never called is
indistinguishable from an absent seat — and it sets a trap: the obvious next optimisation
(cross-page chaining, i.e. adding the walk arm jit_chain_target explicitly refuses) would
silently make the fallback compute a wrong phys, and the code as written would do nothing to
save it, because the geometry path it appears to have does not run. The same-page invariant
is also what keeps the uop side correct (uop_cur_block's page test and uop_stats.aborts fire
exactly when jit_abort_flag does); it is enforced by construction but stated nowhere near the
abort site.

Fix (either, plus the documentation in both cases):
- minimal/true: correct the comment at uop_cache.c:1384-1389 and report §2.1 to state the real
  mechanism ("the fallback uses the ENTRY block's geometry, which is correct because the chain
  never leaves the entry page — the same-page gate in jit_chain_target is load-bearing for the
  abort path"), and delete the dead jit_exec_block_geom + jit_exec_phys/virt (incl. the
  aarch64 stub); or
- wire the read: have jit_execute_block capture (jit_exec_phys, jit_exec_virt) into a
  non-cleared record BEFORE jit_block_exit clears jit_cur_block, so the continuation genuinely
  reads the executing block's geometry (then it is correct today AND correct under a future
  cross-page chain — though the uop-side uop_cur_block page test would still need the same
  treatment). State the same-page invariant at the abort site regardless.

Not a blocker: no guest-visible wrong answer is reachable on the shipped tree (all links are
same-page, and for an unchained run the fallback is exact by definition).

### 2. NICE-TO-HAVE — the attribution instrument's dispatch segment excludes the prologue and
epilogue, so "dispatch 3.5 %" understates what chaining removes

The brackets in make-rdtsc-variant.py sit around `jit_execute_block` (before the call and
immediately after), and the prologue/epilogue run INSIDE the compiled block — so the
"dispatch segment" measures only the C code between blocks (exit continuation + loop top +
lookup + enter), not the epilogue's spill/stats-fold/pops or the next prologue's pushes/
reloads. Chaining removed 17 % by eliding epilogue+tail+lookup+enter+prologue — a number the
3.5 % split would not predict; the split is coherent for what it brackets, but §5's
decomposition should state the bracket boundary so "interior 96.5 %" is not misread as
"dispatch is only 3.5 %". Two smaller instrument notes: the first dispatch segment includes
process-start-to-first-block time (j6_t_seg starts at 0), and the 54-cycle pair cost is
calibrated in a tight loop, which under-estimates in-situ lfence cost (conservative for the
conclusion — it over-attributes to dispatch). None of this flips any decision: chaining's
number comes from the identity-asserted A/B, and the lazy-flag ceiling does not use the split.
Scratch-only, never shipped (the shipped sha256s are clean of the j6_ symbols).

### 3. NICE-TO-HAVE — jit_mem_init's `jit_ready` early-true outlives a failed probe

After a failed probe, jit_ready stays true (set before jit_platform_probe at jit_mem.c:217),
so a second jit_mem_init call would return true despite jit_probe_ok == false. No live caller
re-inits (main.c gates on jit_enabled, which it sets false on failure), so this is latent —
one line (`if (jit_ready) return jit_probe_ok;` or clearing jit_ready on failure) removes the
trap. Cosmetic sibling: jit_stats.pages_alloc ends at 1 on every successful init (the probe's
page), which skews the "pages alloc" stats line.

## THE PLATFORM PROBE (brief Q3) — sound

Reserve → write canary → publish (W^X flip) → execute → recycle; `jit_probe_ok` goes false
first, so every failure arm (forced hook, no page, publish refused, wrong canary result) ends
in init returning false, main.c setting jit_enabled = false, and the caps line printed on BOTH
paths (main.c:891-896). No half-enabled state is reachable: compilation and execution are gated
on jit_enabled alone, and the probe runs before any block exists. The G4 gate proves the
failure path boots the corpus byte-identically on the uop cache. macOS/Windows remain
unverified-by-construction — the probe is the honest posture, and the caps line says so.

## THE NEGATIVE RESULT (brief Q4) — defensible, correctly labeled

The ceiling arithmetic (61 M flag-writing uops × ~2 stores + 30 M jcc × ~2 loads × 0.3-0.5
cycles ≈ 0.8-1.3 % of 7.15 G) checks out as a GROSS ceiling; the register claim is right in
effect: R8's replacement (per-uop `mov r11, imm64; cmp [r11],0` — the emitter has no
rip-relative) and R9's (pending to the frame, +1 memory RMW per uop) each cost ≈ the ceiling
before any win, and R10/RSI/R11 are live in the recipes that would benefit. The report labels
the register reasoning INTERPRETATION. A wrong refusal is not visible here: the margin is thin
but the plan's §9 risk-1 cost (silently wrong flags) is on the same side of the ledger. The
verdict stands; the door named (§5's checks-routine restructure) is the right re-entry point.

## THE GATES (brief Q5) — teeth verified

G1a (link never attempted) now prevents the attempt rather than mutating only the final jmp —
the engage pin fires and nothing else does; G1b (pending zeroed) fires on instruction_counter;
G1c (RCX/RDX swapped) fires on the stores; G1d (virt+state pair dropped) fires via the
collision decoy — and the script's reasoning that state-alone is provably unobservable (the
hash mixes state into the slot) is correct; G1e (cache gate dropped) fires via the garbage-key
decoy. test_chain holds ECX/EDX non-zero across links on purpose, so a dropped/swapped reload
cannot pass vacuously, and `blocks_compiled >= 2` pins the fallback side. The .fixed race is
resolved: the tree and the .fixed copies cmp byte-identical, and the restore rule is stated in
the script. The gates-summary's worst-rc row reads 1 mid-file (the aarch64 environment find)
with the green re-run appended — disclosed, and the aarch64 invocation is now the direct
driver call.

## COVERAGE

Read in full: src/cpu/jit_mem.c (824 lines), src/cpu/jit_mem.h, src/cpu/jit_codegen.c (the
chain stub, chain entry, checks routine, prologue/epilogue, frame layout, emit_uop, compile
block, stats, both aarch64 stubs; the M2-era recipe emitters sampled, not line-audited — not
this unit's change), src/cpu/uop_cache.c (uop_run's compiled arm + tails + ABORT continuation,
uop_block_lookup/decode, uop_invalidate_page, stats; not the decoder), src/cpu/cpu.c
get_phys_eip, src/main.c (jit init/caps/parse), src/cpu/jit_selftest.c test_chain_key + main,
src/cpu/jit_codegen_selftest.c test_chain + main, results/jit-6-headroom.md,
results/jit-6-gates.sh, results/jit-6/gates-summary.txt, results/jit-6/make-rdtsc-variant.py,
results/jit-6/rdtsc-pair-cost.c. Verified: tree == the .fixed copies (cmp), the fetch-cache
invalidation sites (paging.c:51/64/90, exceptions.h:315), CACHED_MASK_ALL's composition, and
the geometry reader's unreachability by call-order tracing. Could not check: macOS/Windows
probe behaviour (no host), the in-situ rdtsc pair cost (calibration is tight-loop), and the
peer lane's files (not this unit's). The gates were NOT re-run (orchestrator-verified per the
brief). No file moved under this review.
