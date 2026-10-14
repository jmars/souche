# PERF-5R — independent review of the PERF-5 M3/M4 tuning (uop_seg_base, body_ip, staircase, test_seg_base)

**Reviewer unit:** `HANDOFF=v86c-perf`, unit PERF-5R. **Reviewed change:** the PERF-5 landed
tree (results/perf-5-tune.md + src/cpu/uop_cache.{c,h} + src/cpu/uop_exec_selftest.c +36).
**Memory node:** `handoff-v86c-perf-review4` (relates `reviews` → `handoff-v86c-perf-result`).
**Mode:** pure code-reading, per the brief — gates/benches/rigs are orchestrator-verified and
were NOT re-run; every file was read on disk before review.

## Verdict

**The three landed diffs and the new gate arm are CORRECT. NO BLOCKER, NO SHOULD-FIX.**
Four NICE-TO-HAVEs, all report/comment wording — no code change is required.

| item | verdict |
|---|---|
| 1. `uop_seg_base` (uop_cache.c:365-388) | faithful, **stateless** transcription of modrm.c:43-57 + 81-100 — needs no epoch, cannot go stale |
| 2. `uop_t.body_ip` precomputed at decode | decode-invariant **by construction** (guaranteed, not workload-observed) |
| 3. staircase bookkeeping | unchanged from the PERF-FIX-3 tree; only the stats hoist landed, counters reproduce exactly |
| 4. `test_seg_base` (+36, 66→102) | a true differential, able to fail (3 inverse mutations), rig covers all three landed diffs |
| M4(c) eviction refusal | stands: 0 clears + peak counters verified actually updated (18.4%/9%) |
| M4(b)/M3-fast-path refusal | stands on the measured +0.5-1.0% wrong-direction result |

## Item 1 — `uop_seg_base` (the highest-risk item)

Checked character for character against modrm.c:

* **prefix branch**: `prefix != 0` → `SEG_PREFIX_ZERO` returns 0 *without touching the null
  check*; else `seg = prefix - 1` — identical to `get_seg_prefix`. `prefix ∈ {1..6}`
  (dec_prefix stores `seg+1`, instructions.c:99-105's own `segment_prefix_op` REPLACES the
  field the same way), so the `(uint8_t)` cast truncates nothing.
* **default branch**: falls through with the uop's decode-time `seg`. `u->seg` is always DS or
  SS for every materialised operand: `dec_extract_modrm` sets `u->seg = DS` first
  (uop_cache.c:292) and only the SS-default forms override it; `UOP_SEG_NONE` (0xFF) can never
  reach `uop_materialize_addr` (only MEM shapes call it, and FETCH_INTERNAL 0x8D/0x8F takes the
  `arg0 = modrm` branch instead), so the `DBG_ASSERT(seg < 8)` (the uint8_t equivalent of
  `get_seg`'s `segment >= 0 && segment < 8`) never fires.
* **null arm**: identical — same `DBG_ASSERT(seg != CS && seg != SS)`, `trigger_gp(0)`, then
  `cpu->pagefault = true`, then the 0 sentinel, in the same order.
* **fault contract**: `uop_materialize_addr`'s `if (cpu->pagefault) return 0;` after the lookup
  (uop_cache.c:423-427) mirrors `get_seg_prefix_ds/ss` exactly (0 sentinel, never "0 + offset"),
  and the offset arithmetic runs *before* the lookup in both copies.
* **prefix visibility at execute time**: the executor rematerialises `cpu->prefixes = u->pfx`
  before `uop_exec` (uop_cache.c:1288) and nothing touches prefixes between there and the
  materialisation, so `uop_seg_base` reads the same 0x07 bits `get_seg_prefix` reads at the
  same moment in the reference.

**The epoch question, answered:** the inline version is stateless — it reads
`cpu->segment_offsets[]` / `cpu->segment_is_null[]` at execute time, at the same moment and
from the same place `get_seg` does. Nothing is cached, so there are no invalidation sites to
name and no epoch to maintain. The plan's M4(a) epoch (and the 41-writer/17-uncovered audit in
results/perf-5/seg-audit.txt that motivated the refusal) would have been strictly more
machinery, not more correctness. The design's correctness does not depend on the audit.

Bonus coverage the unit did not need to claim: the pre-existing M1 checker's `check_addr`
(uop_cache_selftest.c:513-533) rematerialises `cpu->prefixes = u->pfx` on **both** sides and
compares `uop_materialize_addr` (now containing the transcription) against `modrm_resolve` on
1,426,777 extractions — so the copy is differentially tied to modrm.c far beyond the new +36.

## Item 2 — `body_ip` precomputed at decode

The precompute (uop_cache.c:607-609) is exact:

* the old runtime derivation used the same fields (PERF-4 ss2.3 documents
  `pfx_len + opcode_bytes + 1` for custom_modrm_resolve, instruction END otherwise);
* the loop invariant that makes a precomputed-from-`u->eip` value equal a runtime-from-ip value
  is **guaranteed**: blocks are always entered at idx 0 with `ip == uops[0].eip` (there is no
  mid-block re-entry — M4(b) was refused), and the loop continues past uop k only when
  `ip == u->eip + u->len == uops[k+1].eip` (uop_cache.c:1349);
* nothing runtime feeds it: not `cpu->flags`, not segment state, not the runtime prefix byte
  (only the decode-time prefix *count* `pfx_len`), not `is_32` — opcode_bytes/len/key are
  decode-time, and the block key (`state_flags & CACHED_MASK_ALL`, includes is_32) makes
  re-entry under a different mode impossible. **Guaranteed, not merely observed on this
  workload.**
* the one exception (+1 for 0x8D/0x8F) is preserved, and mut arm 6 breaks exactly it to make
  `make nasm` fail (lea-nop/leave16/32/push) — the rig re-runs PERF-4's arm against the *new*
  field.
* struct-size claim verified: 48 bytes before (43 used + 5 pad) and after (47 used + 1 pad);
  body_ip sits at offset 43 (misaligned by 3 — harmless on x86_64/aarch64, no cache-line split).

## Item 3 — the staircase bookkeeping diff

The landed accounting diff is **only** the stats hoist: `block_uops` accumulated locally,
`uop_stats.uops_executed += block_uops` published once per block run (uop_cache.c:1364) on
every exit path including UOP_LEFT_ABORT; `block_uops += 1` happens after the abort-pending
check, so an aborted-entry uop is not double-counted across the re-decode. Nothing reads the
counter during a run (stats print at exit, selftests between `run_machine` calls).

The staircase proper is the PERF-FIX-3 text: cpu.c is byte-identical to what review2 hashed
(8b46c959…), and the counters reproduce exactly pre/post tuning (flushes 257,574, discards 0,
SMC aborts 11, invalidated 308, clears 0, block ends 4,217, uops == instructions ==
200,095,213). Clause-by-clause against interpreted_run (cpu.c:111-156), re-verified:
unconditional `pending += 1` == `i += 1` (the unimplemented instruction is counted in both);
`previous_ip = u->eip` before the body == the reference's tail value at dispatch; the
null-segment fault path (uop_seg_base → `trigger_gp` rewinds ip to previous_ip and delivers)
leaves `ran == false` so a FLUSH-flagged faulting uop does **not** flush (SHOULD-FIX-2b
preserved) — and the same-page/different-page handler continuation matches the reference
(different page → page-cross flush; same page → line 1349 reads a transfer → UOP_LEFT_TAIL →
fresh lookup at the handler, pending kept). The §5.4 `uop_pending`-register experiment was
reverted as reported.

## Item 4 — `test_seg_base` and the mutation rig

* **Differential**: yes. Pass 0 runs the same guest bytes through the cache; pass 1 through
  `interpreted_run`, which exercises modrm.c's `get_seg_prefix_ds/ss` + `get_seg` — the exact
  thing the transcription duplicates. Plus absolute `want[]` expectations (agreement on a
  shared bug still fails), plus a load-bearing "really ran through the cache" assertion:
  `uops[0] > 0` AND `fallback_steps == 0`, with `machine_reset` doing `uop_cache_clear()` +
  `uop_stats_reset()` per pass (uop_exec_selftest.c:193-194), so no stale-block hit or
  cross-pass stat can fake the differential.
* **Phases**: flat; non-flat with four distinct bases reaching the same 0x1400 (DS 0x8000
  default, SS 0x4000 via EBP, FS 0x2000 and ES 0x10000 via overrides); null-DS → #GP vector 13.
* **Can fail**: yes — mut arms 1/2/3 (drop the prefix branch, disable the null check, drop
  `u->seg`) each flip it, measured rc=1 (results/perf-5/mut-rig.txt).
* **Rig covers all three landed diffs**: arms 1/2/3 = uop_seg_base, arm 5 = stats hoist
  (caught by the uops-delta assertion), arm 6 = body_ip (caught by `make nasm`), arm 7 =
  restore; control green, 7 caught / 0 holes, both files restored byte-identical.

## Refusal spot-checks (a wrong refusal costs as much as a wrong change)

* **M4(c) eviction**: the refusal rests on the NEW peak counters, and they are real —
  uop_block_decode:960-961 updates both high-water marks after every arena/block allocation
  (benchmark: arena 12,049/65,536 = 18.4%, blocks 740/8,192 = 9%). With 0 clears, the plan's
  own rule reads "refuse". Stands.
* **M3 fast path → argument against M4(b)**: bench-c3-fastpath.txt shows the fast path
  measured **+0.5-1.0% slower** (best 1.0095x / median 1.0047x) for removing 7.5% of the
  lookups (15,317,242 − 1,145,510 = 14,171,732, internally consistent). The whole-probe-budget
  ceiling (~1-2% even if every probe were removed) caps what (b) could pay, and (b) adds a
  per-block offset map and a new stale-entry class. Refusal justified.

## Findings

**NICE-TO-HAVE-1** (stale file headers, pre-existing but in the exact files this unit edited):
src/cpu/uop_cache.c:1-9 and src/cpu/uop_cache.h:1-10 still say "MILESTONE M1: decode only.
There is no executor, no cache, no invalidation and no `--uop-cache` flag in this file" — false
since M2, and the unit's own uop_seg_base/body_ip comments sit under a header denying the
executor exists. One-paragraph header update.

**NICE-TO-HAVE-2** (report §4/§8, wrong attribution): "the residual 2.7M are the interpreter's
own paths (the nasm/fallback legs and get_seg_prefix_ds/ss callers)" — the after-profile
callgraph (results/perf-5/gprof-callgraph-after.txt:449) shows 2,690,275 of the 2,690,276
get_seg calls come from `string_instruction` (the string-op *bodies*, which run as uops), 1
from instr32_41; nasm does not run in this profile. The substantive claim (not the cache's
memory operands) holds.

**NICE-TO-HAVE-3** (report §5.3/§6 wording): "an order of magnitude below the noise floor"
overstates — 1.1M probes × 10-20 cycles ≈ 3-7 ms of 3.2 s ≈ 0.1-0.2%, the same order as the
measured 0.05% median / 0.24% best noise floor. The refusal actually rests on the measured
wrong-direction result, which is the strong evidence; say so.

**NICE-TO-HAVE-4** (test_seg_base coverage, report-only): CS/SS/GS override-prefix values and
the SEG_PREFIX_ZERO arm are not in the +36. They share the exercised branch (and
SEG_PREFIX_ZERO is unreachable from dec_prefix — `seg+1 ∈ {1..6}`), and the M1 checker's 1.4M
random extractions now cover the general branch, so this is belt-and-braces; a one-line note
would preempt the next reviewer asking.

## Coverage

READ IN FULL: src/cpu/uop_cache.c (1431 lines — decode walk, dec_prefix, dec_extract_modrm16/
sib/32, dec_instruction, uop_seg_base, uop_materialize_addr, hash/arena/invalidation, uop_exec,
uop_run, stats print), src/cpu/uop_cache.h (struct layout + header), modrm.c:1-110
(get_seg_prefix / get_seg_prefix_ds/ss / get_seg / segment_prefix) plus the resolve
default-segment regions it cites, uop_exec_selftest.c:188-195 (machine_reset) + 1001-1140
(test_seg_base + main), cpu.c:95-196 (interpreted_run / cycle_internal), instructions.c:99-105
(segment_prefix_op) + instr_66, exceptions.c:104-140 (trigger_gp), results/perf-5-tune.md
(438 lines), and results/perf-5/{bench-c3-fastpath, bench-c4-hoist, bench-c5-bodyip,
bench-c7-pending, bench-final-summary, bench-noise-floor, g1-stats-final, seg-audit, mut-rig,
gprof-callgraph-after, boot-on/off-stderr}.txt plus results/perf-5-mut-rig.sh. INVENTORIED BY
GREP: uop_seg_base / body_ip / arena_peak readers and writers, uop_stats.uops_executed
readers, get_seg callers in the after profile, uop_cache_clear in the selftest. NOT RE-RUN
(orchestrator-verified per the brief): all gates, benches, rigs, boots, profiles.

sha256 of reviewed files: uop_cache.c 5a788555485b6190383c5f7253287dd481390619e62b841cb678e7b118be340a ·
uop_cache.h f9efa55e46ec6b4dc44a30f6e4796846984f5308ffb2e8873b010726e9e385cd ·
modrm.c 48c5e9bd2163e33efa1fb7d2a3184181f6eb520dd83c8997d9ae3672c5b33237 ·
uop_exec_selftest.c 8897059ea25015ed17f49e29177ad76a9dc3c342707c6b6f47ed536a2bb0c372 ·
cpu.c 8b46c9597f756221ed0b385a8fe51576a348b0ff983a27856b218765898043fc (identical to review2's
record).
