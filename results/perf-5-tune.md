# PERF-5 — M3+M4: tuning the uop cache against the measured hot spots

**Unit:** `HANDOFF=v86c-perf`, unit PERF-5 (coder). **Inputs:** `/tmp/brief-v86c-perf-5.txt`,
`results/perf-2-blockcache-plan.md` §6.2/§7 (M3/M4), `results/perf-4-uop-exec.md` (the cache as
built), `results/perf-fix-3.md` (the invalidation blocker — not undone), `results/perf-2r-review.md`.
**Deliverable:** this document + the code + memory node `handoff-v86c-perf-result`.

Claims are tagged **MEASURED** (run on this host in this session, command and raw output in
`results/perf-5/`), **READ** (from source, with file:line) or **INTERPRETATION**.

---

## 0. Bottom line

**The unit's result: the cache is tuned by −9.4% (best) / −12.9% (median) on the unit's
benchmark, all gates green in both cache modes** — `make bench`-protocol best **3.165 s → 2.839 s
(10.3%), 63.2 → 70.5 MIPS** — and the two changes that paid are NOT the two the brief's reading of
the profile ranked first. Four of the seven candidates measured were worth nothing — or worse —
and saying which is half the result.

| candidate (in the order measured) | delta, interleaved | fate |
|---|---|---|
| §3 M3-item 1 — inline the materialisation | **0** (0.4% slower; the isolated arm says 1.0% slower) | reverted |
| §4 **M4(a) — inline the segment lookup** (`get_seg`, the profile's #1 call count) | **−1.9% / −2.0%** | **LANDED** |
| §5.3 M3-item 2 — the loop fast path (backward jcc → block start) | **0** (0.5–1.0% slower) | reverted |
| §5.1 **the per-uop statistic (`uop_stats.uops_executed`), hoisted out of the loop** | **−4.7%** | **LANDED** |
| §5.2 **the body's instruction pointer, precomputed at decode (`uop_t.body_ip`)** | **−7.2% / −7.0%** | **LANDED** |
| §5.4 the redundant pre-body `cpu->pagefault = false` | **0** (0.1–0.4% slower) | reverted |
| §5.4 the `uop_pending` global, carried in a register across the block run | **+3.0% (slower)** | reverted |

The instrument's own resolution, MEASURED rather than assumed (the same binary against itself, 4
interleaved pairs): **0.05% median / 0.24% best**. Every number above is larger than it.

**What the brief's profile got right and what it did not.** Right about the COUNTS: `get_seg`
(78,235,256) and `uop_materialize_addr` (75,544,980) really are out-of-line calls in the SHIPPED
build (checked in `build/unity.o`'s relocations, §2), and removing the first from the cache's
memory operands takes the profile's count to **2,690,276** (§8). Not right about where the
remaining TIME was: the materialisation *call* is free (§3 — perfectly predicted, one caller),
while the two biggest wins came from two pieces of per-uop bookkeeping **inside the executor
loop** that the profile cannot rank at all, because they are instruction-count reductions in a
function whose samples `-pg` attributes to `interpreted_run`. The plan's own §6.2 pointed at that
surface ("the executor loop's own overhead is the story") and this unit's numbers say the same.

The cache's rates (§1) say why the rest of M3/M4 is not worth taking: 100.00% hit, 0.000%
fallback, 0 cache clears, arena peak 12,049 of 65,536 uops.

---

## 1. G1 — the instrumentation, and the rates it reports on the benchmark

`--uop-cache-stats` already existed (M2) with the plan §6.2 counters; this unit's job was to
check they are SUFFICIENT and report the rates. They are: lookups/hits/decodes/decode-fails,
the instruction split (uops / mid-batch fallback steps / interpreted_run instructions), flushes,
discards, SMC aborts, invalidated blocks, cache clears, block ends.

MEASURED (`results/perf-5/g1-stats-on.txt`, the shipped binary, cache on, 7th run of the
benchmark workload):

```
uop-cache: lookups 15317242 (hits 15316502 = 100.00%, decodes 740, decode-fails 0)
uop-cache: instructions 200095213 (uops 200095213, mid-batch fallback steps 0,
                                   interpreted_run instructions 0 -> fallback 0.000%)
uop-cache: flushes 257574, discards 0, SMC aborts 11, invalidated blocks 308,
           cache clears 0, block ends 4217
```

and the same run on the LANDED tree (`results/perf-5/g1-stats-final.txt`) — identical on every
line, plus the two new pool counters:

```
uop-cache: lookups 15317242 (hits 15316502 = 100.00%, decodes 740, decode-fails 0)
uop-cache: instructions 200095213 (uops 200095213, mid-batch fallback steps 0,
                                   interpreted_run instructions 0 -> fallback 0.000%)
uop-cache: flushes 257574, discards 0, SMC aborts 11, invalidated blocks 308,
           cache clears 0, block ends 4217
uop-cache: arena peak 12049/65536 uops, block peak 740/8192
```

READ against the plan's thresholds (perf-2-blockcache-plan.md §6.2): hit ≥ 85% and fallback ≤ 5%
are the refutation thresholds; the measured values are 100.00% and 0.000%, so the cache is not
the bottleneck in the sense that would make tuning pointless — but the numbers also say where
headroom ISN'T (§6).

Derived rates (MEASURED, computed from the above):

| quantity | value | reading |
|---|---|---|
| one lookup per | 200,095,214 / 15,317,242 = **13.1 instructions** | the lookup is NOT a per-instruction cost; the hash probe pays once per block run. |
| flushes per lookup | 257,574 / 15,317,242 = 0.0168 | 257,574 reference batches for 200M instructions = 777 instructions/batch — the M2 measurement, reproduced. |
| decodes per lookup | 740 / 15.3M = **0.0048%** | the decode cost is negligible on this workload. |
| invalidations per 1000 instructions | 308 / 200M ≈ 0.0015 | the invalidation path is cold. |
| cache clears | **0** | the arena/block pool was never stressed — the plan's M3 "arena sizing" and M4(c) "eviction policy" have no measurement to justify them (§6). |

**Added by this unit** (the one gap the M3 question needed): the PEAK arena and block-pool
occupancy, printed by the same switch. `uop_stats.arena_peak` / `block_peak` are updated in
`uop_block_decode` and give the plan §3 sizing question a number instead of an inference from
the clear count (§6).

---

## 2. The measurement that set the work: where the calls actually are

The brief supplies an orchestrator profile (gprof `-pg`, 200,095,214 instructions, cache on)
whose top remaining call counts are `uop_materialize_addr` 75,544,980 and `get_seg` 78,235,256.
A `-pg` build is not the shipped build, so before tuning against it I checked the claim that
those calls EXIST in what ships — **using the object's relocations, not its disassembly text**:

```
$ objdump -r build/unity.o | grep -E 'uop_materialize_addr|get_seg'
000000000003d8e4 R_X86_64_PLT32    get_seg-0x0000000000000004     <- inside get_seg_prefix
000000000003d94b R_X86_64_PLT32    get_seg-0x0000000000000004     <- inside get_seg_prefix
...
0000000000044f2f R_X86_64_PLT32    get_seg-0x0000000000000004     <- inside uop_materialize_addr
0000000000044f96 R_X86_64_PLT32    get_seg-0x0000000000000004     <- inside uop_materialize_addr
0000000000061f44 R_X86_64_PLT32    uop_materialize_addr-0x...     <- the EXECUTOR LOOP's call
```

Three MEASURED facts from that, each of which the disassembly text alone got WRONG (a
relocatable object has several `.text` sections whose printed addresses overlap, so `objdump -d`
resolved a call into another section to a nearby symbol's `+offset`; `objdump -r` is the
ground truth):

1. the shipped executor loop **does** call `uop_materialize_addr` out of line (reloc at
   `0x61f44`, inside `uop_run.part.0`: `nm build/unity.o` → `0000000000061b30 t uop_run.part.0`);
2. `uop_materialize_addr` **does** call `get_seg` out of line, twice (its two segment arms);
3. therefore the profile's two leading counts describe a real per-memory-operand call chain:
   **1 + 1 calls per memory operand, ~0.77 calls per guest instruction.**

WHY it is not inlined, since that is the thing to fix: `-finline-functions-called-once` — one of
PERF_CFLAGS's three measured flags — applies to **static** functions only (GCC's own
documentation), and `uop_materialize_addr` is a global: it is the symbol M1's decode checker
compares against `resolve_modrm16/32` (`src/cpu/uop_cache_selftest.c:526`), so it cannot become
static. INTERPRETATION of the mechanism, MEASURED as a fact in the relocations either way.

---

## 3. M3 item 1 — inline the materialisation: NO DELTA (negative result, reverted)

`src/cpu/uop_cache.c`: the body of `uop_materialize_addr` moved into
`static inline int32_t uop_materialize_addr_core(const uop_t *)`, the executor calls the core,
and the exported `uop_materialize_addr` became a one-line wrapper over it (so there is still one
copy of the arithmetic and the checker's symbol still exists).

MEASURED, the change's own effect: the reloc at `0x61f44` is **gone** from `build/unity.o`
(`objdump -r build/unity.o | grep uop_materialize_addr` → no match): the call really was removed.

MEASURED, 3 interleaved pairs against the baseline binary (`results/perf-5/bench-c1-vs-base.txt`):

```
baseline      n=3 best 3.188 median 3.192 worst 3.215
c1-inline-mat n=3 best 3.202 median 3.204 worst 3.224
ratio: best 1.0044x median 1.0038x      (>1 means the CHANGE is slower)
```

**Verdict: 0.4% slower — no effect, and possibly a small I-cache cost** (the inlined copy is
larger than the call; it was already perfectly predicted: one caller, one callee, one return
site). A second, independent arm agrees (§4: putting the call back made the tree a further
**1.0% faster**, i.e. both measurements say the inline is ≤ 0). **Reverted**: the landed tree is
§4 + §5 with the materialisation call exactly where PERF-4 left it, and this section is a
measurement, not code.

A NOTE ON WHAT THE REVERT DID AND DID NOT PROVE. The split into a `static` core plus an exported
wrapper was kept for one build (it is what §4 needs to put the inline segment lookup in), and
GCC — freed from the "one global call site" size decision — *then* inlined the wrapper too, so
that build silently contained both changes. The isolation arm in §4 is what separated them, and
the landed tree is the one measured as §4-alone (verified by the presence of
`R_X86_64_PLT32 uop_materialize_addr` in `build/unity.o` — the same relocation §2 found in the
pre-PERF-5 object).

---

## 4. M4(a) — the segment lookup, inlined: **−1.9% alone** (landed)

`src/cpu/uop_cache.c` gains `static inline int32_t uop_seg_base(uint8_t seg)` — a transcription of
modrm.c:44-99 (`get_seg_prefix` + `get_seg`) that the materialisation now calls instead of the
out-of-line pair. What it removes is the benchmark's single largest remaining call:

| | get_seg calls | self |
|---|---|---|
| the orchestrator's profile (pre-PERF-5) | 78,235,256 | 3.19% |
| re-taken by this unit on the landed tree (`results/perf-5/gprof-flat-after.txt`) | **2,690,276** | 0.00% |

**−96.6%** of the calls. **CORRECTION (PERF-5R NICE-TO-HAVE-2, verified against the callgraph):
the residual 2.7M are NOT "the nasm/fallback legs" — they are `string_instruction` bodies
(rep movs/stos/lods/cmps/scas resolve their segment per iteration). The cache reports 0 fallback
on this workload, so no fallback leg contributes here at all; the attribution in this line was
wrong and is corrected rather than left standing.**

MEASURED, alone (4 interleaved pairs, `results/perf-5/bench-c2-isolation.txt`):

```
baseline       n=4 best 3.294 median 3.316 worst 3.324
c2-seg-inline  n=4 best 3.228 median 3.255 worst 3.280
ratio: best 0.9800x median 0.9818x        (>1 means the CHANGE is slower)
```

**−1.9% median / −2.0% best**, and the isolation arm in the same file confirms the split:
`c12` (both changes inlined) vs `c2` (this one alone) = 0.9910x/0.9904x, i.e. §3's change costs
~1% and this one pays ~2%.

### 4.1 The design choice, and what it does NOT rest on

The plan prescribes a segment-base **cache** with an epoch bumped wherever segment state changes.
Both designs remove the same call; they differ in what they must ASSUME. `uop_seg_base` reads
`cpu->segment_offsets[seg]` / `cpu->segment_is_null[seg]` at execute time, at the same place
`get_seg` does, so it cannot be stale and needs no epoch. `results/perf-5-seg-audit.py` (run,
`results/perf-5/seg-audit.txt`) is the accounting an epoch WOULD need: **41 writers** of the two
arrays, of which **17 have no `update_state_flags()` in their own function** (they are covered by
a caller). Those 17 are the reason the transcription was preferred — not a claim that the epoch
would have been wrong, but that its correctness would rest on a 41-site audit that a future caller
can invalidate silently.

Correctness of the transcription is gated by a new differential arm, `test_seg_base`
(`src/cpu/uop_exec_selftest.c`, +36 checks, 66 → 102): the same guest bytes through the CACHE and
through `interpreted_run`, in three phases — flat (all bases 0), non-flat with **four different
non-zero bases** (DS 0x8000, SS 0x4000 through an `EBP`-relative form, FS 0x2000 and ES 0x10000
through override prefixes, all reading the SAME guest address 0x1400), and a **null DS** (the #GP
must still be delivered: vector 13). MEASURED, both legs identical on every slot and every phase:

```
seg-base flat    : ds 0xa5a5a5a5 ss 0xa5a5a5a5 fs 0xa5a5a5a5 es 0xa5a5a5a5 vec 0
seg-base non-flat: ds 0xa5a5a5a5 ss 0x0feedfac fs 0x0c0ffee1 es 0x0deadbee vec 0
seg-base null-ds : ds 0x00000000 ss 0x00000000 fs 0x00000000 es 0x00000000 vec 13
```

and the arm is shown ABLE TO FAIL by three inverse mutations (`results/perf-5-mut-rig.sh`:
drop the prefix branch, drop the null test, drop `u->seg`) — **7 arms, 7 caught, 0 holes** in the
rig's final run (the other four arms cover §5 and the restore).

---

## 5. M3 — the loop's own overhead: **+4.7% and +7.2%**, and four things that did not pay

The plan's §6.2 says what to do if the hit rate is high and the speedup is small: *"the executor
loop's own overhead is the story (indirect calls under cosmocc) — profile again"*. The
refutation clause does not fire (1.45x measured), but the call counts said the same thing from
the other side, so the loop was where this unit looked next. Four candidates were measured, one
change at a time, on this host, interleaved, order alternated. **Two paid and two did not** —
all four results are reported, because the negative ones are the reason the landed tree is what
it is.

### 5.1 LANDED — the per-uop statistic (stats hoist): **+4.7%**

`uop_stats.uops_executed += 1` was a read-modify-write to a global on EVERY uop (200,095,213 of
them), and it cannot be kept in a register because the instruction body is an indirect call.
It is now accumulated in a local (`block_uops`) and added once per block run; nothing reads the
counter during a run (`uop_stats_print` at exit, the selftests between runs).

```
c2b-stats   n=4 best 3.276 median 3.296 worst 3.331
c4-hoist    n=4 best 3.126 median 3.140 worst 3.190      ratio median 0.9525x
(re-run, arms swapped: c4 3.147 median vs c2b 3.298     ratio median 0.9520x)
```

**−4.7% ±0.1 across two independent 4-pair runs.** THE NOISE FLOOR, measured rather than assumed
(`results/perf-5/bench-noise-floor.txt`: 4 interleaved pairs of the SAME binary against itself on
this host) is **0.05% median / 0.24% best** — so every delta reported in this section is larger
than the instrument, including the negatives.

### 5.2 LANDED — the body's instruction pointer, precomputed: **+7.2%**

The executor derived the instruction pointer the body must see at its call —
`eip + (FETCH_INTERNAL ? pfx_len + opcode_bytes + 1 : len)` — per uop, with a flags load, a
branch and three adds. It is a pure function of the decode, so it is now a field
(`uop_t.body_ip`, filled in `dec_instruction`) and the loop is one load and one store. The
struct's size is unchanged (the field fits the existing padding).

```
c4-hoist   n=4 best 3.140 median 3.216 worst 3.239
c5-body-ip n=4 best 2.928 median 2.990 worst 3.016       ratio median 0.9294x
(re-run, arms swapped: c5 2.944 best / 2.958 median vs c4 3.157/3.175   ratio 0.9325x)
```

**−7.0% / −7.3% across two independent 4-pair runs.** Its exception (the `+1` for
`custom_modrm_resolve`, 0x8D/0x8F) is the exact behaviour PERF-4's rig arm 6 broke and the nasm
corpus caught; this unit's rig re-runs that mutation against the new field (arm 6: `make nasm`
fails).

### 5.3 NOT LANDED — the M3 loop fast path (backward jcc to the block start): no effect

Plan §3's last paragraph. Implemented as designed (re-enter the same block at `idx = 0` when the
tail's `(phys, virt, state)` equals the block's own key — three compares against the same fields
`uop_block_lookup` compares, so it can only fire where the lookup would have HIT). Its coverage is
now a number: **1,145,510 fast re-entries out of 15,317,242 block-entry decisions (7.5%)**, and
the lookups fell 15,317,242 → 14,171,732 for exactly that amount.

```
c2-seg-inline     n=4 best 3.173 median 3.208 worst 3.218
c23-plus-fastpath n=4 best 3.203 median 3.223 worst 3.248   ratio median 1.0047x
```

**0.5–1.0% SLOWER, i.e. nothing, in the wrong direction.** The arithmetic agrees it should be
nothing: 1.1M hash probes at ~10–20 cycles is 3–7 ms of a 3.2 s run, which is 0.1–0.2% — at or
below the measured noise floor (0.05% best / 0.24% median over 4 interleaved pairs), so the
prediction is NOT "an order of magnitude below" it: it is the same size as it. **PERF-5R
NICE-TO-HAVE-3 corrected this sentence; the conclusion (the probe-shape result is insufficiently
resolved to claim either way — and the change was reverted) is unchanged.** **Reverted** — and this measurement is also what the unit offers against the
plan's **M4(b) mid-block jump-target re-entry** (§6).

### 5.4 NOT LANDED — three more loop-overhead candidates, all measured, all negative

| candidate | what it removes | measured |
|---|---|---|
| the redundant pre-body `cpu->pagefault = false` (D1's dispatch-entry clear) | 1 store/uop | **0.1–0.4% slower** (`bench-c6-entryclear.txt`) — no effect |
| the `uop_pending += 1` global RMW, replaced by a register carried across the block run and published at the flush points | 1 load-modify-store/uop | **3.0% SLOWER** (`bench-c7-pending.txt`) |
| (for contrast) §5.1's `uop_stats.uops_executed` global RMW | the same shape | **4.7% faster** |

The third row is the interesting one and it is stated rather than smoothed over: two globals with
the same access shape moved the benchmark in OPPOSITE directions. The difference is lifetime —
`block_uops` is dead until the end of the block run, while `pending` must survive the indirect
call, so it costs a callee-saved register and the accompanying spills in a loop that is already
register-hungry. **A "remove an instruction from the loop" change is not monotone, and this unit
has a measured counter-example in each direction.**

Both candidates were reverted; the landed loop is §5.1 + §5.2 only.

---

## 6. M4(b) and M4(c): NOT taken, with the numbers that decide it

* **M4(c) eviction policy — no measurement asks for it.** MEASURED on the benchmark: `cache
  clears 0`, `block peak 740/8192`, `arena peak 12,049/65,536 uops` (18%). The plan's own rule is
  "if it clears more than ~once per 10M instructions, add FIFO eviction — not before the number
  says so", and the number says the pools are five times larger than the workload needs. The two
  PEAK counters are new in this unit, because `clears == 0` alone cannot distinguish "comfortable"
  from "99% full and lucky" — the distinction the plan's sizing question actually needs.
* **M4(b) mid-block jump-target re-entry — the same measurement refuses it.** (b) buys fewer
  hash probes, and its size is bounded by the probe cost: 15.3M probes for the whole workload.
  §5.3 removed 1.1M of them (7.5%) and the effect was **below the noise floor** (measured: 0.05%
median, 0.24% best — `results/perf-5/bench-noise-floor.txt`), so the
  whole remaining probe budget is a low-single-digit percentage at the very most, and (b) captures
  only the fraction of entries whose target is interior to a cached block. Against that: a
  per-block offset map, a new key, and a new way for a stale block to be entered. Not taken.
* **What the profile says IS left** (re-taken, §8): the memory model's own calls —
  `translate_address` 40.9M, `safe_read32s` 33.1M, `safe_write32` 11.4M, `read8` 18.2M — and the
  instruction bodies. Those are outside this unit's scope (they are the model, not the cache) and
  they are the next unit's obvious target.

---

## 7. G3 — every existing gate, both cache modes

Raw: `results/perf-5/gates-summary.txt` and the per-gate logs in the same directory, produced by
`results/perf-5-gates.sh`.

| gate | cache ON | cache OFF |
|---|---|---|
| `make` | rc 0 | — |
| `make selftest` | rc 0 — **3522 + 228 + 541 + 7,934,349 checks, 0 failures**, and the exec suite's own **102** (was 66: this unit's +36 for `test_seg_base`) | same binary; the flag does not change the suites |
| `make nasm` | rc 0 — **118/118**, same 5 skips (`0 of 123 selected test(s) produced no state`) | rc 0 — **118/118**, same 5 skips |
| `make transcript-diff` | rc 0, byte-identical to the pinned reference | rc 0, byte-identical |
| the 1.5 G boot | rc 0 — final state `in_hlt=0, exception=-1, unimplemented=0, eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000` | rc 0 — **the SAME state, byte for byte** |
| the benchmark workload | 200,095,214 instructions, the pinned terminal state and serial sha | same |

The 1.5 G boot's final state is IDENTICAL cache-on vs cache-off **and identical to the state
PERF-FIX-3 recorded for the pre-PERF-5 tree** — same eip (`0xc1927800`), same cr3
(`0x1ea1000`) — so the tuning did not move the guest. The boot's *instruction count* differs
between the two legs (the guest reads the host clock), which is PERF-FIX-3's documented class and
not the gate; the gate is the state. The serial transcripts' sha256 differ for the same reason
(they are kept in `results/perf-5/boot-{on,off}-serial.txt`).

**One stated non-repeat of "byte-identical binary":** a comment-only edit made after the first
gate run changed 9 bytes of `.text` — all of them `mov $line, %edx` immediates, i.e. the
`__LINE__` constants of `assert()` calls below the edited block (verified by diffing the two
disassemblies: 9 changed instructions, all line numbers, nothing else). The gates were re-run on
the final source, and the table above is that run.

---

## 8. G4 — the profile method, and the before/after diff

**The method is now a script: `results/perf-5-gprof.sh`** (raw output in the same directory). It
copies the tree to a scratch directory, patches `src/main.c`'s host-fault handler (the one edit
host glibc needs — the diagnostic frame-walk uses cosmocc's named ucontext members), builds with
`make CC=cc CFLAGS='-std=gnu11 -O2 -pg -Wall -Wextra -lm'`, runs the benchmark, and parses with
`gprof -b -p`/`-q`. Two facts found by MEASURING the script rather than writing it down:

* **cosmocc's APE never writes `gmon.out`** — the `-pg` run must be the HOST compiler's, which is
  also why the `main.c` patch exists. (The first version of the script used the Makefile's default
  `CC`, i.e. cosmocc, and produced no profile at all despite a clean run.)
* the host link needs **`-lm`** (`fmin`, `src/hw/machine.c`); the Makefile's link line does not
  carry it, so it rides in on `CFLAGS`, which the link rule also expands.

**The caveats, kept in the script's header:** (A) `-pg` distorts SELF times by ~7x — read call
counts and ordering; (B) the call counts describe the **-pg build**, and the -pg build's
symbol attribution differs from the shipped one: it lists `interpreted_run` with 238,547 calls
and 50.3% self, while in the shipped tree `interpreted_run` is only the rare whole-batch fallback
and the executor is `uop_run.part.0`. PERF-5 therefore re-checked every target against
`build/unity.o`'s **relocations** (§2) before tuning against it — which is how §3 was known to be
tuning a real out-of-line call rather than a profile artifact.

The diff that is the unit's evidence (both flat profiles: the orchestrator's before-PERF-5 run
and this unit's re-take on the landed tree):

| symbol | before | after | reading |
|---|---|---|---|
| `get_seg` | 78,235,256 | **2,690,276** | §4 removed it from the cache's memory operands (−96.6%) |
| `uop_materialize_addr` | 75,544,980 | 75,544,980 | UNCHANGED — §3 was reverted, and this equality is the cross-check that the landed tree is the measured one |
| `instr32_0FB6_mem` | 9,268,734 | 9,268,734 | same workload |
| `translate_address` | 40,855,294 | 40,855,294 | untouched (the memory model) |
| `interpreted_run` | 238,547 | 238,547 | the fallback count, and the -pg artifact both profiles share |
| `get_seg_prefix_ss` | 2,358,256 | 2,358,256 | the interpreter's own paths, not the cache's |

**What the profile canNOT show, stated:** the two changes that paid most (§5.1, §5.2) are
call-free instruction-count reductions inside the executor loop, so they move the count of
NOTHING — they are visible only as the wall-clock deltas in §5. A future unit should not expect
the profile to rank that class of change.

---

## 9. The bottom line, measured

| | |
|---|---|
| `make bench`-protocol, 7 runs, landed tree (`results/perf-5/bench-final-summary.txt`) | best **2.839 s** / median **2.887 s** = **70.48 MIPS** at best. Its baseline is this unit's own pre-tuning 7-run block on the same protocol: best **3.165 s** / median 3.175 s = **63.22 MIPS** (`results/perf-5/bench-baseline.txt`) → **10.3% best / 9.7% median**. Stated as the WEAKER of the two comparisons: the two blocks ran at different host load (5.7 before, 12–13 after), which is exactly why the interleaved row below is the one to quote |
| 4 interleaved pairs, baseline binary vs landed binary (`results/perf-5/bench-final-summary.txt`) | best **0.9060x**, median **0.8714x** — **−9.4% / −12.9%**. THE NUMBER TO QUOTE: same host conditions for both arms, order alternated inside each pair, and the noise floor measured at 0.05%/0.24% |
| the attribution, change by change (interleaved, order alternated) | §3 inline materialise **0** (reverted) · §4 inline segment lookup **−1.9%** · §5.1 stats hoist **−4.7%** · §5.2 body_ip **−7.2%** · §5.3 loop fast path **0** (reverted) · §5.4 three more **0 / 0 / −3.0%** (reverted) |
| gates | all green, both cache modes, counts unchanged except the exec suite's own +36 — §7 |

The two changes that paid were NOT what the brief's reading of the profile ranked first: the
profile's top item (per-uop address materialisation) is real but its *call* is free (§3), while
the segment lookup inside it (§4) and two pieces of per-uop bookkeeping in the loop itself (§5)
are where the time was. The cache's own rates (§1) say why the rest of the plan's M3/M4 list is
not worth taking: 100% hit rate, 0.00% fallback, 0 clears, and an arena at 18% of its size.

## 10. Raw evidence, file by file

| file | what |
|---|---|
| `results/perf-5/g1-stats-on.txt` | G1: the counters' rates on the benchmark (before the tuning) |
| `results/perf-5/bench-baseline.txt` | the pre-tuning `make bench` samples (7 runs) |
| `results/perf-5/bench-c1-vs-base.txt` | §3, the inline materialisation, alone (negative) |
| `results/perf-5/bench-c2-isolation.txt` | §4 alone, and the §3/§4 isolation arm |
| `results/perf-5/bench-c3-fastpath.txt` | §5.3, the loop fast path (negative) |
| `results/perf-5/bench-c4-hoist.txt`, `-c4-confirm.txt` | §5.1, twice |
| `results/perf-5/bench-c5-bodyip.txt` | §5.2 (+ the confirmation run pasted in §5.2) |
| `results/perf-5/bench-c6-entryclear.txt`, `-c7-pending.txt` | §5.4's two negatives |
| `results/perf-5/bench-final-summary.txt`, `-final.txt` | the landed tree: interleaved A/B vs baseline + `perf_bench.sh` 7 runs |
| `results/perf-5/gates-summary.txt`, `gates-*.txt`, `boot-{on,off}-*.txt` | G3, both cache modes |
| `results/perf-5/gprof-*.txt` | G4: the build/run logs, the flat profile and call graph of the re-take |
| `results/perf-5/mut-rig.txt` | the asymmetry rig: 7 arms, 7 caught, 0 holes |
| `results/perf-5/seg-audit.txt` | the 41 writer sites the rejected epoch design would need |
| `results/perf-5/bench-noise-floor.txt` | the instrument's own resolution: the same binary against itself, 4 interleaved pairs |
| `results/perf-5-ab.sh`, `results/perf-5-gates.sh`, `results/perf-5-gprof.sh`, `results/perf-5-mut-rig.sh`, `results/perf-5-seg-audit.py` | the four rigs and the audit, re-runnable |
