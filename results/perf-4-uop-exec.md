# PERF-4 — M2: the executor, the cache, the invalidation and the staircase

**Unit:** `HANDOFF=v86c-perf`, unit PERF-4 (coder), **milestone M2** — make M1's decoder RUN.
**Inputs:** `/tmp/brief-v86c-perf-4.txt`, `results/perf-2-blockcache-plan.md` (design of record),
`results/perf-3-uop-decode.md` (M1). **Deliverable:** this document + the code + memory node
`handoff-v86c-perf-result`.

Every claim below is **MEASURED** (run on this host in this session), **READ** (from source,
with file:line) or **INTERPRETATION**; where the code disagrees with the plan, the
disagreement is in §5 rather than coded around.

---

## 0. Bottom line

The uop cache executes. It is **ON by default in the shipped binary** (`--uop-cache off` is the
pre-M2 interpreter), and on the unit's benchmark it is **1.42x** (best 1.44x) with a **100.00%
block hit rate** and a **0.000% fallback rate** — inside the plan's predicted 1.3–1.9x band,
so the plan's §6.2 REFUTATION RULE does **not** fire.

| | |
|---|---|
| `make bench` (7 runs, `tests/perf_bench.sh`) — `results/perf-4/bench-7-on.txt`, `-off.txt` | OFF best **4.761 s** / median 4.875 s (42.03 MIPS) → ON best **3.275 s** / median 3.303 s (61.10 MIPS) = **1.454x** (best) / **1.476x** (median) |
| the same, 3 interleaved pairs with the order alternating INSIDE each pair (`results/perf-4/bench-interleaved.txt`) | OFF best 4.720 / median 4.771 → ON best 3.275 / median 3.370 = **1.441x** / 1.415x |
| the workload identity | **all six runs executed exactly 200,095,214 instructions** — the same count as `--uop-cache off` and as PERF-1's pinned baseline |
| the hit / fallback rates (`results/perf-4/bench-stats-on.txt`) | lookups 15,317,241, **hits 15,316,502 = 100.00%**, decodes 739, decode-fails 0; **fallback 0.000%** (0 mid-batch steps, 0 interpreted_run instructions) |
| `make selftest` | rc 0 — **3522 + 228 + 541** checks (the brief's counts, unchanged) + M1's 7,934,349 + the new `uop_exec_selftest`'s **48**, 0 failures |
| `make nasm` | rc 0, **118/118**, same 5 skips — **cache ON and cache OFF** |
| `make transcript-diff` | rc 0, byte-identical to the reference — **cache ON and cache OFF** |
| the M2-5 kernel boot (serial + virtio) | **cache ON: a 0-line diff against the pinned M2-5 record** (`results/m2-5-c-serial.txt`, `-virtio.txt`); cache OFF: one line differs — `/proc/interrupts`' `XT-PIC timer` count, the line M2-5 documents as not reproducible run to run (its own record: 874/873/875/868) |
| the invalidation reconciliation (**G1**) | the two lists reconcile to **empty** modulo the documented LOADER-TIME class (§3) |
| the M2 suite (**G2**) | the SMC selftest, the staircase pin (with an sti-equivalence arm) and the state-key pin; **6 of 6 mutations caught**, control green (`results/perf-4-mut-rig.sh`, `results/perf-4/mut-rig.txt`) |
| the corpus could not have caught the invalidation half | VERIFIED in M1 and re-confirmed: no nasm test self-modifies, so the SMC selftest is the only provable gate for it (plan §7) |

**Three real defects were found by the gates, not by reasoning** (§5): the body-entry
instruction pointer for a `custom_modrm_resolve` uop was one byte short (4 nasm tests), the
same substitution was missing for the *fixed_g* shapes (the 80 MiB kernel boot aborted
477M instructions in), and `sti`'s block-end return dropped its batch's count for the whole
boot (a 2-instruction counter loss, found by the benchmark's instruction-count pin).

---

## 1. What was built

| file | what |
|---|---|
| `src/cpu/uop_cache.c` | **+672 lines** (623 → 1295): the generic executors, the arena/hash/page-registry cache, the pending/flush staircase, the SMC abort, the invalidation entry points and the counters. M1's decoder is untouched except two additive fields (below). |
| `src/cpu/uop_cache.h` | **+126** (259 → 385): `uop_t.modrm`, `uop_desc_t.iflags`, `UOP_DESC_FLUSH` / `UOP_DESC_GUARD_MMX`, `UOP_TS_*`, the M2 API, `uop_page_code` + the inline hot-path test, `uop_pending`. |
| `src/cpu/uop_exec_selftest.c` | **NEW, 604 lines**: the M2 gate suite (§4 G2). |
| `gen/interpreter_uop.js` | +19: the **second descriptor flags byte** — the interpreter's own postfix predicate as data (see §2.1). |
| `src/cpu/uop_gen.c` | regenerated (5558 lines; the descriptor table gains the byte). |
| `src/cpu/cpu.c` / `cpu.h` | `interpreted_run` loses `static` (the cache falls back to it), `cpu_cycle_budget_end` (§2.4), the branch in `cycle_internal`, and the progress assert. |
| `src/cpu/memory.c`, `paging.c`, `string.c` | the invalidation hooks at the plan's §4.1 sites (§3). |
| `src/cpu/cpu_selftest.c` | four link-time seats (`interpreted_run`, `jit_block_boundary`, `interpreted_unimplemented`, `get_phys_eip`, `cpu_cycle_budget_end`) — this binary deliberately has no run loop. |
| `src/main.c` | `--uop-cache on|off` and `--uop-cache-stats` (stderr, so stdout stays the transcript); the shipped default is ON. |
| `Makefile` | `uop_cache.c` joins `$(CPU_SRC)`, `uop_gen.c` joins `$(UNITY_SRC)`, the new suite joins `selftest`, the selftest links that name the decoder drop the now-duplicate `$(UOP_C_OBJ)`, `uop-unity` becomes `$(UNITY_OBJ)` itself. |

`src/cpu/modrm.c`, `instructions.c`, `instructions_0f.c` are **NOT touched** — the plan's §8
step 3 split was NOT taken (the brief marks it optional; §5.5 says why, and M1's report §3.2
predicted this).

---

## 2. The executor as built (and where it differs from the plan's skeleton)

### 2.1 The flush predicate is EMITTED, not derived

`instruction_postfix` (gen/interpreter_c.js:187-190) is
`(block_boundary && !no_block_boundary_in_interpreted) || (!custom && e)`, and that is what
decides where the interpreter's `instruction_counter += i` happens — i.e. where the GUEST's
clock (rdtsc, the PIT, the PM timer) steps. It cannot be re-derived from what M1 emitted: of
the table's modrm encodings, **520 are `custom` + `e`** (`0x00`'s entry is
`{opcode:0, custom:1, e:1}`), so "has a modrm byte" is not the predicate. The emitter
therefore gained a second flags byte (`UOP_DESC_FLUSH`) computed from the **same expression**,
and a `UOP_DESC_GUARD_MMX` bit for `task_switch_test_mmx` vs `task_switch_test` (different
functions: CR0.EM raises #UD in one and #NM in the other, misc_instr.c:656-682).

MEASURED effect: **257,574 reference batches for 200,095,214 instructions** — 777
instructions per batch. A cache that assumed "every modrm instruction ends a batch" would be
wrong by a factor of ~20.

### 2.2 `pending` is a global, and the plan was right about that

The plan's §5.2 says `pending` accumulates "across cycle_internal returns". I first argued it
could not (cycle_internal's `instruction_counter != initial_instruction_counter` assert and
`do_many_cycles_native`'s window test both read the counter) and made it a local — **and that
was wrong**, in a way the benchmark's instruction-count pin caught: `sti` is a **block end that
is not a reference batch end** (M1's §5.2 finding: the table's `0xFB` entry has `custom_sti`
and no `block_boundary`, so `after_block_boundary` never fires for it, but a uop after it would
execute its shadow instruction twice). MEASURED on the boot:

```
reference:  R 80299->8029d  …  the batch is [the two sti's, the instructions after them] = i 4
            counter 3144 -> 3149   (the sti's own body adds one of those: instructions.c:2179)
cache (pending local):    returns at the first sti with pending 2 and never counts it
            => every counter after that point is 2 low, for 200M instructions
```

So `uop_pending` is now a global, `cycle_internal`'s progress check accepts *either* kind of
progress (`instruction_counter` moved **or** `uop_pending != 0`, cpu.c), and a block-end
return that is not a batch end leaves the count pending for the next call — which is also
where the reference's batch continues. This is the plan's shape, and the staircase pin now has
an explicit arm for it (§4 G2, "the sti equivalence arm").

### 2.3 The generic executors

Twelve shapes, switched once per uop (plan §2.3). Two things the plan's skeleton does not
carry, both MEASURED as defects before they were fixed:

* **the body-entry instruction pointer.** At the call, the reference's pointer is at the
  instruction's END (the dispatcher read the opcode and the modrm, `modrm_resolve` consumed the
  sib/disp, then the immediate). The single exception is a `custom_modrm_resolve` body
  (0x8D lea / 0x8F pop), which is handed the **raw modrm byte** and reads the sib/disp itself:
  there the pointer must be `pfx_len + opcode_bytes + 1`. The plan's skeleton
  (`+ u->opcode_bytes`, no `+1`) is **wrong**, and a no-displacement `lea` (which reads nothing)
  then leaves the pointer one byte short of the instruction's end, the executor's own
  `u->eip + u->len != pointer` test reads that as a control transfer, and every following block
  is decoded from the middle of an instruction. MEASURED: 4 failing nasm tests (lea-nop, leave16,
  leave32, push).
* **the memory shapes' first argument.** The substitution above is invisible in the SHAPE: a
  *fixed_g* `custom_modrm_resolve` encoding takes MEMF0 exactly like a non-fixed_g one takes
  MEM0. Handling it only in the MEM0 arm left `0x8F` (`pop r/m32` — the one fixed_g
  custom_modrm_resolve encoding) calling `instr32_8F_0_mem(<linear address>)`, i.e. the port's
  addressing switch run on 32 bits of address, which tripped its own
  `default: DBG_ASSERT(false)`. MEASURED: the 80 MiB kernel boot aborted **477M instructions
  in**. The substitution is now done once, before the shape switch, for every memory shape.

### 2.4 The LOOP_COUNTER window

`do_many_cycles_native`'s test (`instruction_counter - initial < LOOP_COUNTER`) is evaluated
once per **reference batch** in the port, because `cycle_internal` IS one batch there. A cache
that ends several batches inside one `cycle_internal` would overshoot the window, and
`main_loop` would then run the hardware timers at a different instruction count — which the
guest can see. `do_many_cycles_native` now publishes the window's end
(`cpu_cycle_budget_end`, cpu.h) and the cache stops at the batch end that closes it, i.e. at
exactly the instruction the reference's own test would have stopped at. With `--uop-cache off`
the variable is written and never read.

### 2.5 The cache itself

Arena 64K uops (3 MB), 8192 block records, a 16384-slot open-addressing hash with linear
probing and **lazy deletion** (a dead slot does not end a probe, and an insert reuses the first
dead one), and a per-physical-page registry (`uop_page_head`, 2 B/page) with a parallel
`uop_page_code` byte that is the **hot-path test**: a write to a page that has never held a
decoded block is one load and a branch (`uop_invalidate_page_fast`, inlined at every write
site). Overflow clears the whole cache (plan §3's policy); MEASURED on the boot: **0 clears**.

The lookup key is `(phys_start, virt_start, state_flags & CACHED_MASK_ALL)`. **`virt_start` is
part of the key on purpose**: the plan's key is upward-compatible with upstream's
`(phys, CachedStateFlags)` tuple, but two virtual addresses can map to one physical page, and a
block decoded from one of them must not be executed through the other (its uops carry virtual
eips — `previous_ip` and the executor's own advance test read them). MEASURED on the benchmark:
739 decodes for 15.3M lookups, i.e. the alias case is not exercised there; the state-key half
is pinned by G2.

---

## 3. The invalidation, and the reconciliation (the milestone's central evidence)

`results/perf-4-recon.sh` → `results/perf-4/reconciliation.txt` prints **both lists** from the
tree as it stands and the reconciliation between them. The hook sites, by the plan's §4.1
table:

| plan §4.1 site | as built |
|---|---|
| memory.c `write8/16/32` non-mmap arms | `uop_invalidate_page_fast(addr)` before each (memory.c:358/375/395). The one-page argument is in the comments: a straddling write goes through `virt_boundary_write16/32`, whose two `write8`'s hook both pages. |
| `write_blob` | `uop_invalidate_range(offset, length)` (memory.c:79) — the one writer that can span pages. **This is also every device DMA write into guest RAM**: `dma.c:240`, `ide.c:829` and `virtio.c:1409` all call the CPU's `write_blob`. |
| paging.c `safe_write8/16/32/64/128` | `uop_invalidate_page_fast(phys_addr)` in every non-mmap arm (paging.c:655/680/702/732/760). Upstream's `can_skip_dirty_page` split was collapsed by the port when it dropped the JIT; the hook is therefore **unconditional** — and that is the safe direction, because `can_skip_dirty_page` is exactly the "does this page hold code" optimisation that `uop_invalidate_page_fast` already performs. |
| string.c's movs/stos/ins fast paths | `uop_invalidate_range` for the two bulk forms (movs, stosb — these write a whole run in one call) and `uop_invalidate_page_fast` for the four single-element forms (string.c:280/300/312/318/322). The `movs`-into-SVGA arm has no hook because the LFB is not guest RAM. |
| paging.c's A/D-bit `write8`s | **covered by hooking `write8` itself** (as the plan says). |

The reconciliation is empty modulo one class, stated rather than hidden:

* **LOADER-TIME writers** — `zero_memory` (a raw `memset`, memory.c:48), main.c's loader and
  boot-firmware copies (main.c:233/994/998) and `src/hw/kernel.c`'s own static `write_blob`
  + cmdline byte (kernel.c:72/238). They run **before the guest executes anything**, and the
  cache is initialised lazily on the first `uop_run`, so its tables are empty and the hook
  would be a no-op for them anyway. INTERPRETATION, and it is the one place the argument is not
  mechanical: the invariant is "the loader finishes before `cpu->instruction_pointer` is first
  executed", which holds for both entry points (main.c's `run` and the boot path).
* **`vga_mem8`'s writers** are the VGA LFB, not guest RAM. A block can never be registered
  there: `uop_block_decode` refuses when `in_mapped_range(phys)` (M1's contract, inherited from
  `get_phys_eip`'s own assert).
* **The TEST harnesses** write guest RAM with plain `mem8[..] = ..` (hw_selftest.c, m1c's
  R5F imagery, uop_exec_selftest's programs). MEASURED, and it is why the library's default is
  OFF: linking `cpu_m1c_selftest` against a cache that defaults ON **segfaults**, because its
  R5F test rewrites the code page directly between `cycle_internal` calls and the stale block
  then executes. The shipped binary's default is ON (`src/main.c`); the selftests keep the
  interpreter. This is a stated divergence from "the flag defaults on everywhere", and the
  reason is that a harness bypassing the port's write paths is not a machine.

**The SMC abort (plan §4.2)** is the other half: the hook also compares the written page with
the executing block's page and sets `uop_abort_pending`, the executor exits at the **next uop
boundary** with its count KEPT, and the driver re-decodes the current bytes (forcing a decode,
because a lookup that hit the dead block would re-run stale uops forever). MEASURED on the
benchmark: 10 aborts, 230 blocks invalidated.

### 3.1 What is NOT restored, and why that is safe (the plan's §4.4)

* **`TLB_HAS_CODE` / `tlb_set_has_code*`** — upstream marks a TLB entry as holding code so the
  JIT's write fast path can abort a *translated module*. There is no native code here; the SMC
  abort (§4.2) plus the page hook is the whole need. Not restored, stated.
* **`clear_tlb_code` on TLB flushes** — unnecessary because the key is PHYSICAL: a remap
  changes the physical address the lookup uses. Upstream needs it only because its entry points
  are keyed by virtual address. **This is the plan's single biggest simplification and the one
  claim that most deserves a test**: the 200M-instruction kernel boot (heavy paging, 44
  `full_clear_tlb`/`clear_tlb` sites) is byte-identical cache-on vs the pinned cache-off
  record, which is the differential the plan asks for in step 6. MEASURED: `uop_stats.clears`
  is 0 and the boot's blocks survive the whole run.
* **`jit_dirty_page_ctx`'s wasm-table bookkeeping and `translate_address_write_jit`'s SMC
  exit** — no codegen, no wasm table; §4.2 is the equivalent.
* **the plan's §4.4 guard-crossing residual** does not exist in this design, because M1's
  decoder refuses to cache a *page-crossing* instruction at all (uop_cache.h's divergence 2);
  a crossing instruction is run by the reference's own loop body, mid-batch, by `uop_step`.
  MEASURED on the boot: 0 mid-batch fallback steps, i.e. this path is rare there — which is
  also why the staircase pin constructs a crossing `rdtsc` explicitly rather than hoping one
  appears.

---

## 4. The gates

### G1 — the invalidation reconciliation

`bash results/perf-4-recon.sh` → `results/perf-4/reconciliation.txt` (both lists + the
classification). The gate is the empty diff between "every writer of guest RAM" and "a hook, or
a stated loader-time writer" — **empty** modulo the class in §3. Raw evidence pasted there.

### G2 — the M2 suite, each pin shown ABLE TO FAIL

`bash results/perf-4-mut-rig.sh` → `results/perf-4/mut-rig.txt`: **arms 6, caught 6, holes 0**,
control arm green.

| arm | one mutation (two, for the state key — see below) | verdict |
|---|---|---|
| 0 | control: the unmutated tree | GREEN (rc 0) |
| 1 | `uop_invalidate_page`'s page bound always returns | CAUGHT — `ram32(0x3000) == 0x2A` got 0 |
| 2 | the SMC abort alone (`uop_abort_pending = false`) | CAUGHT — the same assertion (the page is unlinked, but the RUNNING block keeps executing its stale uops) |
| 3 | the mid-block flush removed (`jit_block_boundary` only) | CAUGHT — `staircase sample 0: cache-on 0, cache-off 4` |
| 4 | the sti block-end return drops its pending count | CAUGHT — `staircase sample 14: cache-on 84, cache-off 87` |
| 5 | the state byte dropped from the lookup's key | CAUGHT — `ram32(0x3004) == 0x00010001` got 0x20000 (the 16-bit excursion ran the 32-bit block). **Two lines**, and that is the minimal mutation that produces a wrong hit: the state feeds the key in two places (the hash position and the full-key comparison) and dropping **either one alone** is masked by the other — MEASURED both ways (with only the comparison dropped, the probe starts at a different slot and misses; with only the hash term dropped, the comparison rejects it). |
| 6 | the `custom_modrm_resolve` body-entry pointer (`+1` → `+0`) | CAUGHT by the **nasm** leg — `0 of 4 selected test(s) produced no state` for `lea-nop|push|leave16` |

The three pins themselves:

* **the SMC selftest** — an image whose first instruction patches the immediate of the
  instruction after it, in its own executing page, and whose result (0x2A) is only right if the
  page was invalidated **and** the running block aborted. MEASURED: 1 abort, 1 invalidated
  block; and the same image with `--uop-cache off` gives the same 0x2A (so the cache's answer is
  the interpreter's).
* **the staircase pin** — cache-on vs cache-off `rdtsc` traces over a stream with a jcc loop,
  mid-block flush points, a page-crossing `rdtsc`, and an `sti`. The trace is the guest's OWN
  view of `instruction_counter` (`read_tsc` → `microtick` → `counter/1000`), so it sees
  **mid-batch** reads — the only place a cache that flushes at block ends differs from the
  reference. It is decoded **exactly, with no tolerance**, on this argument: `read_tsc` returns
  `counter*1000 + extra` with `extra <= tsc_resolution - 1`, and `tsc_resolution` is the
  smallest observed difference between two consecutive reads, which is always a multiple of
  1000 — and a WARMUP image (a page-crossing `rdtsc`, whose batch is one instruction, followed
  by another read) drives it to exactly 1000 before any measurement, so `stored / 1000` is the
  counter. MEASURED warmup: reads 2000 / 3000, delta 1000. The pin also asserts that it TOOK
  the mid-batch case (at least one pair of consecutive samples carries the same counter) and
  that the first sample is 4 — the only batch that has completed at that point, derivable from
  the image.
* **the state-key pin** — the same physical instructions at 0x1300 executed in a 32-bit and
  then a 16-bit code segment must give 0x00010001 and 0x00010000 (a key that ignored the state
  gives 0x00020000 twice), the second pass must HIT (hits >= 2), and both excursions must have
  produced their own blocks (decodes >= 5).

### G3 — the byte-identity suite, cache OFF and cache ON

| gate | cache OFF | cache ON |
|---|---|---|
| `make selftest` | rc 0: 3522 + 228 + 541 + 7,934,349 + 48 checks, 0 failures (`results/perf-4/selftest-final.txt`) | same binary; the suite's own runs are unaffected by the flag |
| `make nasm` | 118/118, same 5 skips | 118/118, same 5 skips |
| `make transcript-diff` (M0 image) | rc 0, byte-identical to the reference | rc 0, byte-identical to the reference |
| the M2-5 boot, serial + virtio | virtio byte-identical; serial differs from the pin in **one** line (`/proc/interrupts` `XT-PIC timer`: 872 vs 874) | virtio **byte-identical**; serial **0 lines of diff against the pin** |
| the bench workload | 200,095,214 instructions | 200,095,214 instructions |

The boot's single-line class is M2-5's own documented one (its record has 874/873/875/868 for
the same binary): the guest reads the host clock, so that count is not reproducible run to
run. **Stated honestly**: the cache-ON run landing exactly on the pin's value, while the
cache-OFF run landed 2 below it, is within that spread and I cannot attribute it — one sample
per arm is what I have.

### G4 — the benchmark, with the refutation verdict

* **`make bench` (7 runs each, the unit's own protocol)**: cache OFF best **4.761 s** / median
  **4.875 s** (42.03 MIPS, all 7 runs at 200,095,214 instructions) → cache ON best **3.275 s** /
  median **3.303 s** (61.10 MIPS, all 7 runs at 200,095,214 instructions) = **1.454x** best /
  **1.476x** median (`results/perf-4/bench-7-on.txt`, `-off.txt`, raw samples appended to
  `bench-samples.txt`).
* **3 interleaved pairs, order alternating inside the pair** (PERF-1's rule, which controls for
  host drift better than two sequential blocks): OFF 4.720 / 4.771 / 4.804 → ON 3.370 / 3.275 /
  3.398 = **1.441x** best, **1.415x** median, and **all six runs at 200,095,214 instructions**
  (`results/perf-4/bench-interleaved.txt`).
* Both are inside the plan's expected 1.3–1.9x (central ~1.5x) ✓ and are measured against the
  ~3.9–4.8 s cache-off baselines this host has produced all unit.
* hit rate **100.00%**, fallback **0.000%** — far inside the plan's ≥ 85% / ≤ 5% refutation
  thresholds, so the design DOES pay and the rule does not fire.
* 10 SMC aborts, 230 invalidated blocks, 0 cache clears, 4,217 block ends (i.e. blocks are long:
  the interpreter's batch structure does not chop them).

---

## 5. Findings — every divergence from the plan, with its measurement

1. **The plan's §2.3 skeleton is wrong about the body-entry pointer** (§2.3 above; the +1 for
   `custom_modrm_resolve`). MEASURED: 4 nasm tests.
2. **The plan's §2.3 executor sketch cannot express the fixed_g `custom_modrm_resolve` case** —
   `0x8F` is that case and only that case. MEASURED: the boot's `DBG_ASSERT` at modrm.c:461,
   477M instructions in.
3. **`sti` forces the plan's §5.2 "pending across returns" to be real**, and my first
   implementation's argument against it was wrong (§2.2). MEASURED: a 2-instruction counter
   loss for a whole boot (200,095,212 vs 200,095,214) and a serial diff (861 vs the pin's 874 on
   the /proc/interrupts line) that BOTH went away with the fix — i.e. the defect was not
   cosmetic, it moved the guest's clock.
4. **The plan's §5.2 names a case that is unreachable, and it is unreachable in the reference
   too**: "a fetch fault at block entry discards pending". The tail fetch faults only when the
   instruction's page differs from `last_virt_eip`, and (a) the page-cross test breaks the batch
   first, and (b) the only instructions that invalidate `last_virt_eip` (CR0/CR3 writes,
   `invlpg`, far transfers, `pop ss`, iret, a cpl change) are all reference batch ends. The
   discard path is implemented and counted (`uop_stats.discards`), and it is **0 on every gate
   run** — which is the evidence for the claim, not a substitute for it. The plan's phrase "at
   block entry" is also wrong: an entry-time fetch fault returns from `cycle_internal` BEFORE
   the counter is touched, and involves no `pending` at all.
5. **The plan's §8 step 3 modrm split was NOT taken** (and M1 predicted this): `modrm.c` is
   untouched, so the addressing semantics exist twice and M1's checker's (b) comparison
   (1,426,777 extractions against the port's own resolver) is what ties them. M3 should still
   take the split; this unit's executor does not need it.
6. **`--uop-cache`'s default lives in the program, not the library** (§3, the test-harness
   class). Stated because it is a real asymmetry between "what ships" and "what the selftests
   exercise", and because the alternative (default ON in the library) is MEASURED to segfault
   the M1c suite.
7. **`uop-unity` is now `$(UNITY_OBJ)`** — the two files are IN the shipped unity TU, so the
   M1 target that built a *separate* "the unit they will join" TU has nothing left to compare.
   The `unity-symbols` gate the concurrent PERF-FIX unit added covers the same ground directly.
8. The plan's §6.1 arithmetic predicted 1.6–1.9x from the removed calls and stated the honest
   spread as 1.3–1.9x; MEASURED 1.42x, i.e. the low-middle of the band — consistent with its
   own caveat that "the uncounted per-call work and the uop materialization can eat half the
   theoretical win".

---

## 6. What M3 should do next

1. **Take the plan's §8 step 3 split** and delete the decoder's second copy of the addressing
   semantics (M1's report §7 item 4 asks for exactly this).
2. The plan's §3 loop fast path (a backward `jcc` to the block start → `idx = 0` without a hash
   lookup) is where the remaining per-batch lookup cost is; MEASURED, the lookup is 15.3M calls
   for 200M instructions.
3. `uop_stats.aborts == 10` and 230 invalidations on the boot are small; the SMC hook's cost on
   the write path (a load and a branch for a page with no code) is inside the 1.42x.
4. The arena/block pool sizing was never stressed (0 clears); measure the clear frequency on a
   longer boot before adding eviction.
5. The staircase pin's three arms are the model for anything M3 adds: a counter observation the
   guest itself makes, decoded exactly, with an equivalence arm whose difference is derivable.

---

## 7. Raw evidence

| file | what |
|---|---|
| `results/perf-4/bench-interleaved.txt` | the interleaved ON/OFF runs, order alternating, with the instruction count of every run |
| `results/perf-4/bench-stats-on.txt` | the cache's own counters on the benchmark (hits, decodes, flushes, aborts, invalidations, clears) |
| `results/perf-4/bench-samples.txt` | the `perf_bench.sh` raw samples that were taken while the sti defect was still present (they are the *first* measurement of it) |
| `results/perf-4/selftest-final.txt` | `make selftest`'s complete output, 0 FAIL lines |
| `results/perf-4/boot-on-*.txt`, `boot-off-*.txt` | both legs of the M2-5 boot (serial, virtio, stderr) + `boot-serial.diff` |
| `results/perf-4/reconciliation.txt` | G1's two lists |
| `results/perf-4/mut-rig.txt` | G2's per-arm verdicts |
| `results/perf-4-mut-rig.sh`, `results/perf-4-recon.sh` | the two rigs |
