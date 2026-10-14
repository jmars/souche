# JIT-PLAN — the design for the real JIT on the v86 C port

**Unit:** `HANDOFF=v86c-jit`, unit **JIT-PLAN** (planner, plan-only). **Inputs:**
`/tmp/brief-v86c-jit-plan.txt`, memory node `handoff-v86c-jit-ctx`,
`results/perf-2-blockcache-plan.md` (the cache's design of record),
`results/perf-3-uop-decode.md`, `results/perf-4-uop-exec.md`, `results/perf-fix-3.md` (the
invalidation hooks as restored), `results/perf-5-tune.md` (the profile + measurement method),
`results/jit-spike.md` (concurrent unit — **its Q1/Q2 measurements were still pending when this
plan was written; the plan parameterizes them**), and the code itself.

Claims are tagged **VERIFIED** (run on this host), **READ** (from source, with file:line) or
**INFERRED**. Files this plan relies on were hashed at planning time (§10) — the SPIKE unit is
editing `src/` concurrently, so the implementer must re-hash before touching anything.

---

## 0. Bottom line

**Compile the uop array, not something new.** The JIT is an *alternate executor for the blocks
the uop cache already decodes*: same block framing, same physical key, same invalidation chain,
same pending/flush staircase — only the per-block execution changes, from the interpreted uop
loop to native x86-64 that keeps guest registers in host registers, inlines the TLB fast path
and the lazy-flag machinery for a hand-written core set (~40 encodings ≈ **95.7% of the
benchmark's dynamic instruction bodies**), and calls the existing `instr_XX_*` C bodies for
everything else. The fault/exit contract is reproduced by making the compiled block return
through `uop_run`'s existing exit paths, not by inventing new ones. aarch64 builds the JIT out
and falls back to the uop cache. The refutation rule is a number (§8).

---

## 1. What was verified this pass

| Fact | How | Where |
|---|---|---|
| The uop cache executes with 100.00% hit / 0.000% fallback / 0 clears; the hot-path per-memory-operand call chain (`uop_materialize_addr`→`get_seg`) is real in the shipped object; 740 blocks decoded, arena peak 12,049 uops | READ (perf-5 §1/§2) | results/perf-5-tune.md |
| The block is a linear uop array with framing (page end / cap / no-descriptor / block end) and a 4-bit `state_flags & CACHED_MASK_ALL` key half; uops carry everything execution needs (`fn`, `shape`, `imm`, `disp`, `base/index/scale`, `seg`, `g`, `modrm`, `body_ip`, `pfx`, flags) | READ | src/cpu/uop_cache.h:118-147, 161-170 |
| The executor loop is the contract the JIT must reproduce: per-uop previous_ip/prefixes/pagefault bracketing, pending/flush, budget, page-cross, iteration-limit, block-end, eip-divergence | READ | src/cpu/uop_cache.c:1282-1395 |
| The descriptor tables (1182 descs, 924 arms) are generated from `v86/gen/x86_table.js` and the decoder resolves a desc per uop (`e->desc` in `dec_instruction`) | READ | src/cpu/uop_cache.c:514-570, uop_cache.h:254-262 |
| The lazy-flag machinery is exactly 3 state fields (`last_op1`, `last_result`, `flags_changed`+`last_op_size`) + getters over them; `flags_add/sub/inc/dec/...` are tiny setters | READ | src/cpu/flags.c:12-169, cpu_state.h:251-255 |
| The TLB fast path is one load + one bit test: `entry = tlb_data[addr>>12]`; `(entry & (TLB_VALID|RO|NO_USER|NO_EXEC)) != TLB_VALID` → walk; else `phys = (entry & ~0xFFF) ^ addr`; the mapped-range bit is in the entry (`TLB_IN_MAPPED_RANGE`) | READ | src/cpu/paging.c:275-315 |
| Faults are delivered INLINE: `trigger_*` rewinds `instruction_pointer = previous_ip` and runs the handler inside the body; `cpu->pagefault` is the abort channel with the D1 window (cleared at dispatch entry AND exit) | READ | src/cpu/exceptions.c:86-118, cpu.c:118-133 |
| Only `sti` does a runtime nested dispatch (its shadow instruction can set `jit_block_boundary` via the generated postfix); no other body can | READ | handoff-v86c-jit-ctx + uop_cache.c:1146-1161 |
| The invalidation chain the JIT must ride already exists and is already forwarded: guest write → PERF-FIX-3 hooks → `uop_invalidate_page` → `jit_code_invalidate_page` (wired by the SPIKE); the substrate (`src/cpu/jit_mem.{c,h}`, `jit_selftest.c`, `--jit on|off` default off) is landed in the tree | READ | src/cpu/uop_cache.c (invalidation), src/cpu/jit_mem.h (whole header) |
| Dynamic shape distribution of the benchmark: **jcc 14.9%, ALU 26.9%, mov/lea 32.6%, movzx 9.9%, shifts 7.8%, inc/dec 3.6%** of 181.3M counted instr-body calls (200.1M instructions); the core set (no shifts) is **87.9%**, core+shifts **95.7%** | **VERIFIED** (python over results/perf-5/gprof-flat-after.txt; command in §9) | this pass |
| Upstream's JIT split: `generate_jit.js` emits only the dispatch/argument plumbing; ALL semantics live in hand-written `jit_instructions.rs` (7812 lines). No third emitter over the table is how upstream itself works | READ | v86/gen/generate_jit.js:321-398, v86/src/rust/jit_instructions.rs (7812 lines) |

---

## 2. Q1 — WHAT IS COMPILED, AND INTO WHAT

**The uop array of an existing cache block, into a native x86-64 function.** Reasons, each
grounded:

1. **Everything hard is already solved at the block level** — framing (uop_cache.h:23-32),
   the physically-keyed lookup, the invalidation chain, the fallback machinery, the counter
   staircase. A second abstraction (upstream-style entry-point blocks, virtual keys,
   `tlb_code`) would re-derive all of it for zero benefit. Upstream needed those because it
   had no decoded-block layer; this port does.
2. **The uop is a complete compile-time program.** Every field the codegen needs is in the
   uop (uop_cache.h:118-147); every *decision* the executor makes per uop (FLUSH, BLOCK_END,
   body_ip, guards) is known at decode time. Compilation is a pure function of the uop array +
   the state byte; nothing is re-derived from guest bytes.
3. **The exit contract exists already.** `uop_run`'s loop has exactly four ways to stop
   (uop_cache.c:1266-1268: batch return / tail / SMC abort) and the code after the block loop
   (1371-1395) handles them. The compiled block returns one of those, and *the existing C
   code* does the tail/lookup/abort work. The JIT cannot get the staircase wrong by
   construction in the parts it doesn't reimplement.

The compiled artifact is `void (*)(void)`: prologue (save callee-saved, load guest state),
the per-uop sequence (§3/§4), epilogue (spill, set exit kind, restore, ret).

**Compile-at-decode, no hotness gate.** The benchmark decodes 740 blocks; compiling every
block on decode is free of profiling machinery, makes `--jit on` a *deterministic full-JIT*
mode (which is also what the oracle needs, §6), and the pool cost is bounded (§5.1).

---

## 3. Q2 — WHAT IS INLINED, WHAT IS CALLED, AND THE COVERAGE TARGET

The prior-project finding (dispatch-only JIT buys nothing) is accepted: **the win requires
inlining, and the shape coverage target is a measured number.** Per-shape policy:

| shape (dynamic share) | policy | what the inline emits |
|---|---|---|
| mov r32↔r/m32, movzx/sx, lea (42.4%) | **INLINE, no flags** | inline address materialisation + TLB fast path (§4.1) + mem8 load/store; lea = address only |
| ALU reg/reg, reg/imm, reg/r/m32: add/or/and/sub/cmp/test (26.9%) | **INLINE, lazy flags** | compute result; store `last_op1`/`last_result`/`last_op_size`/`flags_changed` **exactly as the corresponding `flags_*` setter does** (flags.c:97-169); the result store |
| ALU r/m32, imm (RMW) (in the 26.9%) | **INLINE** | fast-path load + op + fast-path store **+ the `uop_invalidate_page_fast` hook after the store** (§5.1 — the inlined write MUST invalidate) |
| jcc rel8/rel32 (14.9%) | **INLINE** | inline the lazy getters (getcf/getzf/getsf/getof logic over the 3 state fields); intra-block target → direct branch to that uop's code; outside → set target eip, exit TAIL |
| inc/dec (3.6%) | **INLINE** | `flags_inc/dec` semantics (CF-preserving mask, flags.c:156-180) |
| shl/shr/sar reg,cl + imm forms (7.8%) | **M3 INLINE** (after M2 green) | flags.c shift semantics (count masking, OF cases); M2 falls back to the C body |
| push/pop/call/ret/jmp (block-enders, <2%) | **CALL the body** (block ends right after; nothing to gain) — jmp E9/EB inline-branch where target is in-block | the executor's exact args |
| string ops (1.5%) | **CALL** (self-contained loops) | same |
| everything else (~4-13%) | **CALL the body** | same |

**The fallback call is not the uop-cache cost.** For any not-inlined uop the compiled code
still materialises the address INLINE (base + index<<scale + disp + the seg-base load with the
null-segment check — PERF-5's `uop_seg_base` transcription, exposed), sets the args, and calls
`u->fn` with the executor's exact signature (uop_cache.c:1089-1132). What is eliminated for
called shapes is the executor loop and the materialisation call; what is eliminated for
inlined shapes is *also* the C body.

**COVERAGE TARGET (measured, §1):** the core set is **87.9%** of dynamic bodies without
shifts, **95.7%** with the three shift forms. Milestone rule: **M2 must inline ≥ 85% of
dynamically executed uops on the benchmark** (new `--jit-stats` counter: inlined vs called
uops); **M3 with shifts ≥ 93%**; **below 80% after M2 → not worth shipping, stop** (§8).

---

## 4. Q3 — GUEST REGISTERS, STATE, AND THE FAULT/EXIT CONTRACT

### 4.1 Register allocation (SysV, inside the compiled function)

| host | guest | notes |
|---|---|---|
| rax, rcx, rdx, rbx | EAX, ECX, EDX, EBX | rbx callee-saved |
| r12-r15 | ESP, EBP, ESI, EDI | callee-saved, saved in prologue |
| rbp | `&cpu` | loaded once |
| r8, r9, r10, r11 | temps | r9 = pending counter; r10 = executed-uop counter |
| memory | `last_op1/last_result/last_op_size/flags_changed` live in cpu_state | no register caching in M1/M2 (C bodies read them; spill would be worse) |

`tlb_data`/`mem8`/globals are referenced with link-time-patched absolute addresses (or
RIP-relative via recorded relocations — the emitter's business, §7). **Spill rule: before ANY
C call, spill all 8 guest regs + pending to cpu_state/global; reload after.** This is uniform,
and the per-call spill is not hot (calls are the fallback path).

### 4.2 The per-uop emission — the contract, in the executor's own order

For each uop (compile-time constants baked): the stored-IP bracketing (previous_ip = eip,
instruction_pointer = body_ip, prefixes = pfx), the D1 fault window (`pagefault = false`
before and after — the R5F contract, cpu.c:118-133), the instruction (inline recipe or C
call), `pending += 1`, then the checks **in the executor's order** (uop_cache.c:1317-1365):

1. after any C call: `interpreted_unimplemented` set → flush + exit RETURN; `jit_block_boundary`
   set → flush + clear + budget check (only `sti`'s nested dispatch can set it — sti is a
   block end, but the check is emitted after every C call, cheap and general);
2. FLUSH flag (baked): `instruction_counter += pending; pending = 0`; budget reached → exit RETURN;
3. divergence test `cpu->instruction_pointer != eip+len` — **emitted only after code that can
   diverge** (C calls, TLB slow-path calls): a fault rewinds eip to previous_ip (exceptions.c:
   86-118), so the test fires → exit TAIL → the existing C tail re-looks-up at the new eip.
   For pure-inlined instructions nothing can move eip, so the test is elided (per-recipe
   MAY_DIVERGE flag, §5.3);
4. iteration-limit predicate — on the fall-through path it is compile-time false (`eip <
   body_ip` always), so it is emitted **only in the divergence path** (diverged && pending ≥
   LIMIT && eip ≥ ip → flush + exit RETURN, else TAIL) — provably equivalent to the executor's
   order, argued in §9;
5. BLOCK_END: spill, exit (pending unflushed for sti — `uop_pending` global carries it, exactly
   as today);
6. SMC abort: one load of the shared abort global + branch per uop, before each instruction;
   abort → exit ABORT (pending kept — the reference's batch runs through the write).
   The page-cross check is NOT emitted: blocks never span pages by framing, and fall-through
   cannot leave the page (uop_cache.h:34-43).

Exit kinds map 1:1 onto `uop_run`'s existing tail (uop_cache.c:1371-1395): RETURN (batch
done, flushed), TAIL (previous_ip/`get_phys_eip`/discard-on-fault, then next block), ABORT
(re-decode at current eip, keep pending). **The compiled block implements none of the tail
logic itself.**

The executor's remaining global bookkeeping the compiled block must keep exact:
`uop_stats.uops_executed += executed_count` once per run (the PERF-5 hoist), executed count in
a register; `uop_pending` spilled to the global on every exit and reloaded at entry.

### 4.3 Inlined memory access — the TLB fast path (exact transcription)

READ: `entry = tlb_data[addr>>12]`; miss test `(entry & (TLB_VALID | TLB_READONLY_if_write |
TLB_NO_USER_if_cpl3 | TLB_IN_MAPPED_RANGE)) != TLB_VALID` → call the C slow path
(`safe_read32s`/`safe_write32` etc. — identical semantics incl. fault delivery); hit →
`phys = (entry & ~0xFFF) ^ addr`; `mem8` access. The cpl3/readonly bits are **constant-folded
per block** (cpl is in the state key; direction is per-access). Cross-page data accesses
(`addr & 0xFFF` near the page end) take the slow path — the same test `safe_read32s` does.
Within a block the TLB is stable: every mapping/mode change is a block end, and the key is
physical (perf-2 §4.3 — inherited, no TLB hooks added).

---

## 5. Q4 — INVALIDATION, SMC, AND THE CODE ARENA

**One invalidation mechanism, already restored and already forwarded** (VERIFIED in the tree:
`uop_invalidate_page` calls `jit_code_invalidate_page` before its early return, and
`jit_cache_bind` sets the shared `uop_page_code` bit — the SPIKE's wiring, jit_mem.h:23-38).
The JIT adds no write-site hooks of its own; the PERF-FIX-3 chain covers it. Consequences:

- **guest write → compiled block dropped + SMC abort**, via the same functions the uop cache
  uses; the abort flag is what the compiled block checks per uop (§4.2). A write into the
  executing block's page keeps `pending` (reference semantics) and re-decodes.
- **Inlined writes must invalidate.** An inlined RMW store writes `mem8[phys]` directly and
  therefore MUST call `uop_invalidate_page_fast(phys)` after the store — the C bodies pay this
  through the `SAFE_READ_WRITE*` macros (paging.h:167/191/217); the inline must not drop it.
  (The fast test is one bitmap load + branch — PERF-FIX-3 measured the hooks below resolution.)
- **Executing-block discipline:** a compiled page is recycled only when no block executes it —
  the SPIKE's `jit_block_enter/exit` deferred-free rule (jit_mem.h:149-156). The executor sets
  `uop_cur_block` before dispatching compiled code exactly as it does today, so the
  currently-executing-page test keeps working.
- **Arena under pressure:** the SPIKE's pool is 256 pages / one block per page; the benchmark
  alone decodes **740 blocks**, so the real JIT grows the pool to **2048 pages (8 MiB VA,
  lazily committed)** — a sizing the measured block count justifies — and on exhaustion
  **stops compiling** (new blocks execute as uop cache), printing `lowmem_failures`. Clear-all
  policy (the uop cache's own, perf-2 §3): full arena reset = mprotect RW, reset, re-flip RX
  (2 flips); chosen over reference-counting until a measurement asks for more.

---

## 6. Q5 — THE ORACLE

The existing gates (nasm 118/118, transcript-diff, M0 transcript, boot final state) compare
FINAL state, and the JIT is deterministic — so the strongest and cheapest gate is the port's
own: **every existing gate run twice, `--jit off` vs `--jit on`, byte-identical results and
counts.** Because `--jit on` compiles EVERY block (no hotness gate), the jit-on boot and
transcript exercise every compiled block the workload reaches — the "shadow parallel mode"
is unnecessary until/unless profile-gated compilation is added (it is not in this plan).

Three additions make it an oracle rather than a smoke test:

1. **Per-recipe differential matrix selftest** (new, §7 step 6): for every inlined recipe, run
   the instruction compiled vs through the uop executor over a matrix of operand values
   (registers × addresses × flags_changed states) and compare the FULL cpu state (regs, lazy
   flag fields, flags, memory). This catches recipe drift at `make selftest` time — the
   transcript cannot isolate a single recipe, this can. It is the gate that pins the
   "second execution path" maintenance risk.
2. **The mutation rig** (the project's established method): inverse mutations — flip a recipe's
   flag mask, break the TLB fast-path bit test, drop the RMW invalidation hook, delete a flush
   — must each fail a gate (asymmetry is the proof the gates can see the class).
3. **The existing pins, jit-on:** the SMC selftest and the staircase/sti pins
   (src/cpu/uop_exec_selftest.c) extended to run with `--jit on` — they are exactly the
   fault/counter contract pins, and they already exist.

**Smallest independently verifiable first milestone (M1):** the substrate (spike's) + the
emitter + **one recipe** (mov r32, r/m32 with the inlined TLB fast path) + the executor branch
behind `--jit on`, with: nasm 118/118 jit-on, transcript byte-identical jit-on, SMC + staircase
pins jit-on, `--jit off` byte-identical. This pins the entire fault/exit/counter contract
before any breadth; the recipes after it are each pinned by gate 1.

---

## 7. Q6 — FILE-LEVEL STEPS, IN ORDER

Step 0 is the SPIKE's (concurrent); the rest are the real JIT's.

| # | files | what | gate |
|---|---|---|---|
| 0 | (spike) src/cpu/jit_mem.{c,h}, jit_selftest.c, uop_cache.c (forward), main.c, Makefile | the substrate: W^X pages, bind/lookup, the shared invalidation chain, `--jit on|off` default off | `make jit-selftest`; `--jit off` byte-identical — **re-verify before landing anything** |
| 1 | **src/cpu/jit_x64.{c,h} (new, ~500-700 lines)** | the emitter: the ~30-mnemonic vocabulary (mov/add/sub/and/or/xor/cmp/test/inc/dec/shl/shr/sar/lea/jcc/jmp/call/ret/push/pop + addressing forms), labels/relocations, patching. **Feeder is the spike's Q2 decision, parameterized here:** if OUR-OWN (recommended: no new deps, tiny vocabulary, fits the rig culture — and DynASM needs minilua in the build), this file IS the encoder; if DYNASM, this file is the thin API over it and the build gains the minilua + dasm_x64.lua rule (vendored at baliveau/src/vendor/luajit/dynasm/). The recipe layer below is IDENTICAL either way — only the emit calls differ | encoder selftest: every emitted opcode executed-and-asserted (own encoder) or a generated-block smoke test (dynasm) |
| 2 | src/cpu/uop_cache.h:118-147 | **either** add `uint16_t desc_idx` to `uop_t` (filled at uop_cache.c:530-548; +2-8 B/uop in the arena — acceptable) **or, preferred, no struct change:** key recipes by `u->fn` pointer (fn pointers are unique per descriptor; the recipe map is a sorted {fn, recipe} array built once at init, binary-searched at compile time only) | `make selftest` (uop counts unchanged), `make nasm` both modes |
| 3 | **src/cpu/jit_codegen.c (new, M1 slice ~400 lines → M2/M3 ~1500-2500 lines)** | the recipe table + block compiler: walk the uop array, emit §4.2's per-uop sequence, the §4.1 spill/reload, the §4.3 fast path, the exit-kind epilogue; per-recipe MAY_DIVERGE flag; "does not fit in one code page" → keep uop execution. **NO third emitter over x86_table.js for M1-M3:** the descriptor tables exist and are consumed; recipes are hand-written per encoding. (Sizing vs upstream: their `generate_jit.js` emits only dispatch plumbing; all semantics are hand-written — 7812-line jit_instructions.rs. Ours: ~1/5 of that, because 12 executor shapes collapse the plumbing and only ~40 encodings get recipes. A `gen/` emitter becomes worth it only if coverage must reach 100% of descriptors — not in this plan.) | M1: `--jit on` + nasm 118/118 + transcript byte-identical + SMC/staircase pins |
| 4 | src/cpu/uop_cache.c (uop_run branch), src/cpu/jit_mem.h (pool size) | `blk->code ? run_compiled(blk) : uop loop` (one lookup still — the code pointer lives on the block record or the spike's bind maps into it; do not double-hash in the hot path); the exit-kind continuation (the existing tail); `--jit-stats` coverage counters (inlined vs called uops, per §3's target); pool 256 → 2048 pages | all gates, both flags, both cache modes; coverage counter prints |
| 5 | src/cpu/jit_codegen.c | M2: the core recipes — mov/movzx/lea, ALU reg/imm/r-m (+RMW with the invalidation hook), inc/dec, jcc with intra-block branches | §6 gate 1 (per-recipe matrix), the mutation rig, all gates jit-on, **coverage ≥ 85% measured** |
| 6 | src/cpu/jit_codegen_selftest.c (new) | the per-recipe differential matrix + the device-range pin (an access whose TLB entry has TLB_IN_MAPPED_RANGE must take the slow path) + the rig arms | `make selftest` (the matrix); rig catches 100% of arms |
| 7 | src/cpu/jit_codegen.c | M3: shifts (shl/shr/sar) recipes; re-measure; then the §4.2 elisions (skip ip/prefix stores for provably fault-free uops — only after M2's matrix is green, it is the safety net for elisions) | coverage ≥ 93%; bench numbers |
| 8 | src/cpu/jit_mem.c, src/cpu/main.c | platform probe (below); M4 only if §8 passes: block chaining (jcc to another compiled block's entry — must still go through enter/exit and the staircase checks), lazy-flag register caching | gates + bench |

---

## 8. Q7 — PORTABILITY, and Q8 — EXPECTED WIN + REFUTATION

**aarch64: CONFIRMED — build the JIT out, fall back to the uop cache.** The substrate already
does exactly this: `V86C_JIT_X64` is 0 on non-x86-64, the files reduce to no-op entry points,
`--jit on` reports unavailability, `make` still produces the same APE pair (jit_mem.h:59-64,
73-77). A second backend is explicitly NOT planned; the honest statement is in the header.
**macOS/Windows W^X: UNVERIFIED — runtime-probe it.** At `jit_mem_init`: reserve, publish and
execute a canary page; any failure disables the JIT for the run with the caps string printed
(Linux VERIFIED; macOS MAP_JIT/pthread_jit_write_protect_np and Windows VirtualProtect NOT
invoked — jit_mem.h:40-57). Never silently run the untested path.

**Expected win (tied to the spike's ceiling when it exists):** the spike measures ratio C of a
hand-written native block (guest regs in host regs, TLB fast path inlined) vs the uop cache on
the hot shape, with stated assumptions. **CORRECTION (orchestrator, after the spike landed with
C ≈ 54–80×): the formula this paragraph originally carried — "expectation ≈ 1 + (C − 1) ×
0.5…0.7" — is VOID; do not use it. C is a PER-SHAPE ratio measured inside a harness on a tight
synthetic loop (the emulator pays 13.7 ns per guest instruction there, the native block ~0.17),
not an end-to-end predictor: applied to C = 80 it would predict 40×, contradicting this same
section's bound two sentences below. THE BINDING EXPECTATION IS THE PROFILE-SIDE BOUND**, not C:
the real JIT must keep the SMC abort load, the counter/flush bookkeeping, the per-uop
previous_ip/prefixes stores, the spill/reload around fallback calls and the RMW invalidation
hook, and the memory model is paid by both paths. C is useful for exactly one thing — it shows
the per-shape gap is enormous, which is why INLINING and not dispatch is the whole game. The
honest planning range is therefore 1.3–1.6×, and if the spike's C had come out ≤ 1.3 the JIT
would not be worth its maintenance cost — report that instead of building breadth.
Profile-side bound (§1): inline everything hot and the remaining
cost is the memory model (translate/safe_read/safe_write = 40.9M + 33.1M + 11.4M calls) —
which the uop cache also pays — so a ceiling above ~1.6-1.8x is implausible regardless of C.

**REFUTATION RULE (the number that says stop):** after M2, best-of-7 `make bench` with
`--jit on` vs the uop cache's pinned best (2.839 s / 70.5 MIPS): **if the speedup is < 1.15x,
or dynamic inlined coverage < 80%, stop — do not ship, do not build M3/M4** (1.15x is 60x the
measured noise floor, perf-5 §5, and below the bar that pays for a second execution path).
If 1.15-1.3x: ship behind `--jit` default OFF and report the measured value honestly; default
ON only at ≥ 1.3x with all §6 gates green.

---

## 9. Q9 — RISKS (source, cost if wrong, mitigation)

1. **Lazy-flag inline correctness.** Source: getters interpret `flags_changed/last_op1/
   last_result` exactly (flags.c:12-169); a wrong mask or wrong sub/adc inversion is silently
   wrong flags. Cost: wrong guest branches everywhere, undetected by final-state gates if the
   workload re-masks. Mitigation: §6 gate 1's per-recipe matrix compares the full lazy state;
   a rig arm flips FLAG_SUB for `sub`.
2. **The fault/exit contract.** Source: R5F's D1 window (cpu.c:118-133), the trigger rewind
   (exceptions.c:86-118). Cost: wrong previous_ip/eip corrupts every delivered fault — the
   class this project has hit most often. Mitigation: M1 is a one-recipe contract milestone;
   the SMC/staircase pins run jit-on; MAY_DIVERGE elisions only where provably fault-free.
3. **The counter staircase.** Source: perf-2 §5.2; the sti 2-instruction loss (perf-4 §2.2).
   Cost: guest-visible clock divergence (PIT/rdtsc) — silent. Mitigation: pending-in-register
   with the spill-around-call rule; flush points baked from UOP_DESC_FLUSH; the
   iteration-limit-in-divergence-path equivalence (argued: on fall-through `eip < body_ip`
   makes the predicate compile-time false; the executor evaluates it before the divergence
   test, the compiled path after — the only case both fire is a backward divergence, where the
   executor's order is flush+RETURN and the compiled order tests the same predicate first —
   identical outcome); the existing staircase pin run jit-on.
4. **Inlined write bypasses invalidation.** Source: the RMW macros carry the hook
   (paging.h:167/191/217). Cost: stale compiled/decoded code executed — the SMC class. 
   Mitigation: the §5.1 mandatory hook after every inlined store; a rig arm deleting it must
   fail the SMC selftest.
5. **TLB fast-path bit test.** Source: paging.c:275-290; a dropped TLB_IN_MAPPED_RANGE or a
   bad constant fold (cpl3, readonly) reads device memory through mem8 or misses a #PF. Cost:
   wrong values, missed faults. Mitigation: exact transcription + the device-range pin (§7
   step 6).
6. **The invalidation gap class PERF-2R/PERF-FIX-3 found** (writer shapes the reconciliation
   cannot see: function-pointer writers, pointer aliases). Cost: stale code, shipped
   undetected. Mitigation: the JIT adds nothing new — it rides the same hooks, so the gap is
   inherited, not widened; the jit-on SMC selftest proves the chain end-to-end; new writer
   shapes stay a recon-gate concern, not a JIT one.
7. **Executable-memory management.** Source: W^X flips, the deferred-free rule
   (jit_mem.h:141-156). Cost: crash (executing a flipped/recycled page) or staleness. 
   Mitigation: enter/exit discipline, exhaustion → stop compiling (uop fallback), pool sized
   by the measured block count (740 → 2048 pages).
8. **Maintenance of a second execution path.** Source: upstream's own 7812-line hand-written
   semantics. Cost: an instr body fixed later without its recipe diverges. Mitigation: the
   per-recipe matrix pins recipe↔body at selftest time; recipes are per-encoding, so a body
   fix shows up as a matrix failure, not a convention.
9. **Spike concurrency.** Source: the spike is editing src/ now (jit_mem.c mtime 11:05 this
   pass). Cost: the implementer lands on a moving tree. Mitigation: re-hash §10 before every
   edit; the plan consumes the spike's landed interface, not its file contents.

---

## 10. File hashes at planning time (sha256, first 16)

```
40d7f3e87b609182  src/cpu/jit_mem.c          509e6e549cd9b8eb  src/cpu/jit_mem.h
7e43cc70c3a32802  src/cpu/uop_cache.c        44aa6325cad598e6  src/cpu/uop_cache.h
8b46c9597f756221  src/cpu/cpu.c              05dbf8ce26289c22  src/cpu/cpu_state.h
9ee0eb51d3e6e30b  src/cpu/flags.c            a6a0f1ed2e7d7249  src/cpu/paging.c
78e1ed03fda8e45c  src/cpu/paging.h           5ed5786fb9f2d211  src/cpu/memory.c
5338a7b14c666186  src/cpu/exceptions.c       4f7694d478f96dda  gen/interpreter_uop.js
f238f48eada86414  src/cpu/uop_gen.c          199af2da676ecaeb  src/main.c
a8c4e4f06e71841b  Makefile                   cd91551e71805e6d  results/perf-5/gprof-flat-after.txt
8c443bde85c1ca09  results/perf-5-tune.md     9410093cb0b8d3ab  results/perf-2-blockcache-plan.md
```

Coverage computation (VERIFIED, this pass): `python3` over
`results/perf-5/gprof-flat-after.txt` — regex `instr(16|32)_\w+` rows, group by opcode:
jcc 14.90%, ALU 26.91%, mov/lea 32.59%, movzx 9.86%, shifts 7.78%, inc/dec 3.62% of
181,299,249 counted body calls; core-minus-shifts union 87.88%, core+shifts 95.66%.
