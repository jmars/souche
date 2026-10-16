# u64-jit — widen the guest instruction counter, and the JIT's block-table-full fix

Handoff: `handoff-v86c-u64-jit-ctx`.  Two follow-ups PAE-L3 found, done as one unit:
**item A** widen the guest instruction counter `u32 -> u64`, **item B** the JIT's
block-table-full behaviour.  Every claim tagged MEASURED / READ / INTERPRETATION; every gate
is its EXIT CODE.

## TL;DR

- **Item A landed.**  `cpu->instruction_counter` is now a `u64`; the C port boots the pinned
  Debian 686-pae kernel past 4 G and prints the TRUE instruction count.  The acceptance test
  is `sh tests/gate-pae-l3.sh full` (a new explicit arm): it runs BOTH legs past 4 G and
  asserts the initramfs's own `PAE-INIT` lines — `pae` and `nx` advertised, in the guest's
  own `/proc/cpuinfo` words.  The 3 G arm stays the fast default.
- **Item B landed (the minimum fix, not a table resize).**  The block-table-full refusal is
  printed ONCE, counted in `jit_stats.block_table_full` (reported in the stats line), and the
  two stale "JIT is off unless --jit on" comments in `jit_mem.h` are corrected.
- **The table-size question was MEASURED, not assumed, and recorded as a measured negative**
  (§3): growing the table just moves the wall (block-table-full -> code-pool-exhausted), and
  growing both makes the boot *slower*, because the marginal compiled blocks are cold.  The
  graceful "interpret the refused block" policy already sits near the measured optimum.

## Item A — the u32 counter, widened to u64

### What it was (READ)

`cpu->instruction_counter` was a `u32` (cpu_state.h:279, upstream's `*mut u32` at wasm offset
664, global_pointers.rs:39).  It is the port's VIRTUAL CLOCK: `microtick() =
instruction_counter / 1000.0` (exec_boundary.c:87), and the TSC, PIT, PM timer and RTC all
derive from it.  It therefore WRAPPED at 4,294,967,296 instructions, jumping the guest's
clock and TSC backwards mid-run — and a `--limit` above 4 G could not even be expressed
(`parse_positive_uint` capped at `0xFFFFFFFF`).

### The asymmetry (both arms, MEASURED, the same kernel + initramfs + machine)

| arm | invocation | result |
| --- | --- | --- |
| PRE-FIX (frozen `69c5c214…`) | `--bzimage … 4294967295` (the max expressible u32) | `v86c boot: 2334378027 instructions, in_hlt=1` — the WRAPPED count.  True = 2^32 + 2,334,378,027 = **6,629,345,323**.  6 `PAE-INIT` serial lines. |
| POST-FIX (this tree) | `--bzimage … 7000000000` | `v86c boot: 6984274170 instructions, in_hlt=1` — the TRUE count.  All `PAE-INIT` lines present. |

Both reach userspace and halt (`in_hlt=1`).  The two totals differ by ~355 M instructions;
**INTERPRETATION** (mechanism named, not instrumented): the wrap jumps `microtick` backwards
~71.6 minutes of guest time, which changes the RTC/PIT/TSC-driven scheduling and therefore
the instruction stream the guest executes — the wrap does not merely misreport the count, it
changes the boot.

The reference leg's own counter is a u32 and wraps too: `sh tests/gate-pae-l3.sh full-js`
prints `instructions: 2154727871` (2^32 + 2,154,727,871 = 6,449,695,167) for the same
evidence.  The two legs' counts are therefore NOT compared across the boundary; the serial
`PAE-INIT` evidence is what the gate compares.

### The one site a mechanical widening would have broken silently

The budget window (`do_many_cycles_native`'s LOOP_COUNTER window) is tested by WRAPPING
SIGNED-DIFFERENCE arithmetic: `cpu_cycle_budget_end` is the u32 window end, and both
`uop_budget_reached` (uop_cache.c) and the JIT's emitted budget check (jit_codegen.c) test
`(int32)(counter - budget_end) >= 0` — the reference's u32 `wrapping_sub` semantics
(cpu.rs:3419), correct ONLY while both operands live in the same 32-bit domain.

**What was done (the deliberate shape):** `instruction_counter` became a `u64`, and
`cpu_cycle_budget_end` STAYED a `u32` — the window end is `(uint32_t)(initial + LOOP_COUNTER)`,
and `uop_budget_reached` casts the counter's LOW 32 bits back into the 32-bit domain:
`(int32_t)((uint32_t)cpu->instruction_counter - cpu_cycle_budget_end) >= 0`.  The low 32 bits
wrap exactly as the old u32 counter did, so the staircase is unchanged for any counter value.
The `(uint32_t)` cast is load-bearing: without it, a u64 minus a u32 truncates the WRONG bits
to `int32`, and the window never closes past 2^32.  The JIT's emitted budget check reads only
the low 32 bits (`mov r32`), so it stays correct UNCHANGED; only the JIT's FLUSH
(`jitcg_emit_flush`, jit_codegen.c) was widened — from a 32-bit load/add/store (which would
advance only the low 32 bits and leave the high 32 stuck at zero, the exact silent wrap this
widening removes) to a single 64-bit `add [counter], r9` (R9 holds the pending count with its
upper 32 bits zero: every writer is a 32-bit op, which zero-extends).

### Files touched (item A)

`src/cpu/cpu_state.h` (the type), `src/cpu/cpu.c` (the u64 window; `cpu_cycle_budget_end`
stays u32), `src/cpu/uop_cache.c` (`uop_budget_reached`'s cast), `src/cpu/jit_codegen.c`
(`jitcg_emit_flush` 64-bit), `src/main.c` (limit + `run_result_t.count` + `parse_positive_uint`
+ every `%u` -> `%llu` print), `tests/gate-pae-l3.sh` (the `full` arm).

## Item B — the block-table-full fix

### The fix

`jit_mem.c`'s block-table-full arm printed `jit: bind: the block table is full (2048)` PER
BIND and recycled the span.  It now increments `jit_stats.block_table_full`, prints ONCE
(a sticky `reported` flag), and the stats line reports the running total beside
`pool exhausted` / `hash full`.  The two stale comments in `jit_mem.h` ("JIT off unless
--jit on") are corrected to the truth: `jit_enabled` is TRUE by default (jit_mem.c:42, since
JIT-FIX-2).

### The asymmetry (both arms, MEASURED, the 3 G PAE boot)

| arm | stderr | refusal lines | stats |
| --- | --- | --- | --- |
| PRE-FIX (`69c5c214…`) | 4,342,344 B | **103,354** `block table is full` lines | (no counter existed) |
| POST-FIX (this tree) | 1,589 B | **1** line | `… pool exhausted 0, hash full 0, block table full 103354` |

The two runs execute the same workload (identical `blocks compiled 96382`, `invalidations
4222`, `execs`, serial); only the reporting changed.

## 3. Grow the table? MEASURED, and it does not pay

The brief's decision rule: measure whether a larger table makes the 6.1 boot faster, and
what it costs.  A/B on the 3 G PAE boot, three configurations, each 2 runs (the shipped
binary rebuilt per config, `jit_mem.h` constants restored by `cmp` after each arm):

| config (blocks / hash slots / code pages) | wall (s) | block-table-full | pool exhausted | blocks compiled |
| --- | --- | --- | --- | --- |
| **shipped** 2048 / 4096 / 4096 (16 MiB pool) | 27.40, 26.38, 26.71 | **103,354** | 0 | 96,382 |
| blocks doubled 4096 / 8192 / 4096 | 24.98, 24.59 | 0 | **40,064** | 159,672 |
| blocks + pool doubled 4096 / 8192 / 8192 (32 MiB pool) | 25.68, 25.12 | 15,524 | 0 | 184,212 |

READ from the table: doubling the block table alone is ~7% faster (27.4 -> 24.8 s) but the
wall MOVES — the 40,064 refusals become `pool exhausted` (the 16 MiB code-page pool fills
instead).  Doubling the pool too removes that wall, but the boot is then SLOWER than
blocks-only (25.4 vs 24.8 s) and the block table fills AGAIN (15,524) — the 24,540 extra
blocks it compiled are COLD: their compile cost exceeds their execution savings.  The
shipped 2048-block table is within ~7-9% of the measured optimum, and its graceful
degradation (refuse + interpret) is the right policy.  Cost of the doubling, for the record:
+~56 KiB block table (2048 -> 4096 x 28 B), +8 KiB hash, +16 MiB of LAZY address space for the
pool.  VERDICT: **NOT SHIPPED — a measured negative** in the tree's own style (as JIT-6
recorded lazy-flag caching).

## Gates

Each by exit code.  `make minibios-mem` was already satisfied; the binary's sha256 was
checked after every source rebuild.

| gate | result |
| --- | --- |
| `make` | **rc 0**; binary sha `69c5c214…` -> `b8519c7c…` |
| `make selftest` | **rc 0** — `selftest: all suites green` (jit_codegen_selftest 3525 checks / 0 failures: the 64-bit flush reproduces the reference batch structure) |
| `make nasm` | **rc 0** — 118/118 |
| `make transcript-diff` | **rc 0** — byte-identical (the `%llu` count is byte-identical to `%u` below 2^32) |
| `make determinism` | **rc 0** |
| `make bench` | **rc 0** — the 200,095,214-instruction pin holds, every run |
| `sh tests/gate-reboot-1.sh` | **rc 0** — 39/39 |
| `sh tests/gate-m2-5-determinism.sh all` | **rc 0** — run1/run2 serials + virtio byte-identical, counts 4,000,024,062 == 4,000,024,062 (**the pinned count did not move**) |
| `sh tests/gate-m2-5-guest.sh all` | **rc 0** — 0 failing assertions |
| `sh tests/gate-pae-1.sh` | **rc 0** — 11/0 |
| `sh tests/gate-pae-l3.sh all` | **rc 0** — 3 G both legs, 0 failing |
| `sh tests/gate-pae-l3.sh full-c` | **rc 0** — C leg past 4 G, TRUE count 6,984,274,170, all `PAE-INIT` lines |
| `sh tests/gate-pae-l3.sh full-js` | **rc 0** — reference leg past 4 G, all `PAE-INIT` lines |

## Round 2 — the review's should-fix and nice-to-haves (handoff-v86c-u64-jit-review)

The review cleared both load-bearing invariants and found one truncation the widening had
introduced, plus a pre-existing mislabel.  All three are now closed.

**The truncation (should-fix, fixed).**  `uop_cache.c`'s interpreted-fallback tally read
`uint32_t before = cpu->instruction_counter` and then
`uop_stats.fallback_instr += cpu->instruction_counter - before`.  Past 2^32 the u64 minuend
minus the zero-extended u32 subtrahend overcounts by exactly 2^32 per fallback run, so the
reported `interpreted_run instructions` and the `fallback %` lied.  `before` is now `uint64_t`.
MEASURED asymmetry, the full past-4G run with `--uop-cache-stats`:

| arm | `interpreted_run instructions` | `instructions` total | `fallback` |
| --- | --- | --- | --- |
| BEFORE (round-1 `b8519c7c…`) | 137,438,953,539 (= 32 x 2^32 + 67) | 144,422,947,432 | 95.165% |
| AFTER (this tree) | 67 | 6,983,993,960 | 0.009% |

The 32 x 2^32 overcount is exactly 32 fallback runs past the wrap, each overcounting by 2^32
(the review's own arithmetic).  After the fix the stats total reconciles with the counter
(6,983,993,960 vs 6,984,274,170 — the residual is the stats tally's own uop/instruction
granularity, not a wrap).

**The mislabel (pre-existing, corrected — a pure rename, no semantic change).**
`jit_codegen_stats.refused_pool` is DOCUMENTED in its own comment as "the substrate refused a
page / the bind" — it counts reserve-NULL, publish-fail AND bind-refusal, not just pool
exhaustion.  Its NAME and the stats print label (`pool`) said otherwise, so the
block-table-full bind refusal (now also tallied under its true name in
`jit_stats.block_table_full`) landed in two lines, one mislabeled.  READ: no test or report
consumes `refused_pool` (the selftest checks `refused_fit` only); it is print-only.  Renamed
`refused_pool` -> `refused_substrate` and the print label `pool` -> `substrate`; the counter
still counts exactly the same three arms.  The per-reason breakdown lives in `jit_stats_t`.

**The arithmetic typo (fixed).**  `2^32 + 2,334,378,027` is 6,629,345,323 (not 6,629,374,323),
and `2^32 + 2,154,727,871` is 6,449,695,167 (not 6,449,724,167) — both corrected above.  The
derived ~355 M figure (6,984,274,170 - 6,629,345,323 = 354,928,847) and the ~56 KiB block-table
cost (2048 x 28 B) were re-derived and are consistent.

**The systematic sweep (the review asked for it).**  Every site in `src/` that reads the
counter was re-grepped for the "u64 counter minus a narrower captured value" shape.  The only
run-path truncation was the fallback tally above.  The remaining 32-bit reads are DELIBERATE
(the budget window's low-32-bits domain — invariant 1) or u64 (`cpu.c`, `main.c`); every stats
counter that accumulates the counter or a difference (`uop_stats_t`, `jit_stats_t`,
`jit_codegen_stats_t`) is `uint64_t`.  The `uint32_t start/save/ic0` locals in the SELFTESTS
are loop bounds on counters the tests keep provably below 2^32 — not lying instruments.

## Write scope honoured

`v86/` untouched (READ-ONLY).  `src/hw/` untouched.  `tests/` only `gate-pae-l3.sh` (the
kernel image and initramfs are PAE-L3's, left alone).  `results/*` only this report plus the
gate's own full-arm serial captures (`results/pae-l3-full-*-serial.txt`, the `-stderr.txt`
are already gitignored); the m2-5 gate's churned captures were restored to HEAD after the
battery ran.  No commit.
