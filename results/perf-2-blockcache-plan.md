# PERF-2 — plan: a decoded-block / uop-cache interpreter for the v86 C port

**Unit:** `HANDOFF=v86c-perf`, unit PERF-2 (planner, plan-only). **Input:** the mission
context (`handoff-v86c-perf-ctx`) and `/tmp/brief-v86c-perf-plan.txt`.
**Deliverable:** this document + memory node `handoff-v86c-perf-plan`.

Every claim below is tagged **VERIFIED** (run or computed on this host), **READ** (from
source, with file:line), or **INFERRED**. Files this plan relies on were hashed at
planning time (PERF-1 is editing the tree concurrently); the hashes are in §10.

---

## 0. Bottom line

Build a **physically-keyed block cache of decoded instructions ("uops")**, where the
block is a linear decode run terminated by the reference's own *mode-change*
instructions, execution is a tight indirect-call loop over a uop array, and **the
reference's interpreter batch structure is reproduced exactly at two levels the guest
can see** (the instruction-counter staircase and the re-synchronization at mode-change
instructions). Invalidation is **restored at the exact upstream call sites the port's
comments mark as dropped** (per-page dirty hooks on every RAM write path), plus a
current-block SMC abort. **No TLB hooks are needed** — a real simplification versus
upstream, and a stated one (§4.4).

Expected win: **1.3–1.9x on the boot benchmark, central estimate ~1.5x** (6.26 s →
~4.2 s), which would put the C interpreter **at or past the pinned reference's JIT**
(4.90 s). The reasoning is the gprof call counts (§6). A legitimate alternative result
is "don't build it" — §6.4 prices the cheaper options; the evidence says build it, but
gated on PERF-1's measured result.

---

## 1. What was verified this pass

| Fact | How | Where |
|---|---|---|
| The interpreter is per-operand-call bound: 337M read_imm8, 200M get_phys_eip, 212M read_reg32, 119M write_reg32, 83M+83M modrm_resolve/resolve_modrm32, 57M resolve_sib over 200M instrs | READ (orchestrator's gprof counts, context) | handoff-v86c-perf-ctx |
| The boot baseline still runs: 200,095,214 instructions, clean exit, serial "in_hlt=0 … eip=0x21972c1" | **VERIFIED** (re-ran `./build/v86c --bzimage tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0"`, exit 0, 6 s) | this pass |
| The dispatcher is two generated 512-way switches; every case **re-reads** modrm/immediates and re-resolves the address per execution | READ | src/cpu/interpreter_gen.c, gen/interpreter_c.js:195–296 |
| **Every non-custom modrm instruction ends the interpreter's batch** (`instruction_postfix`'s `!custom && e` clause) — upstream's own shape | READ | gen/interpreter_c.js:187–190, mirror of v86/gen/generate_interpreter.js |
| jcc/jmp/loop carry `block_boundary:1` **and** `no_block_boundary_in_interpreted:1`; call/ret/hlt/string-reps/mov-sreg/pop-ss/CR moves do **not** have the no_bib escape | READ | v86/gen/x86_table.js:837 (jcc), 289–292 (call/jmp), 179–180 (ret), 0xF4 (hlt), 0x0F22 (mov cr) |
| The write paths dropped upstream's dirty hooks at marked sites | READ | src/cpu/memory.c:348, 361, 378; src/cpu/paging.c:634–635; src/cpu/string.c:25 |
| Upstream dirties JIT code from **safe_write\*** when `!can_skip_dirty_page` (i.e. when TLB_HAS_CODE) | READ | v86/src/rust/cpu/cpu.rs:3941–3956 |
| `cpu->state_flags` (the 4-bit cached-state key) **is kept current** on every segment/CS/cpl change | READ | src/cpu/cpu_state.c:99–108, exceptions.c:833+ (switch_seg), exceptions.c:230/289/473/760, instructions_0f.c:6475/6505 |
| Only **one** instruction does a runtime nested dispatch: `sti` (instr_FB); prefix bytes and 0F are decode-time | READ | rg over src/cpu: run_instruction callers = cpu.c:119, instructions.c:68/182/188/2166 |
| The nasm corpus contains **no self-modifying test** | **VERIFIED** | `ls v86/tests/nasm/ | rg -i "self|smc|code"` → empty |
| Upstream's cache key is `(phys_address, CachedStateFlags{is_32,ss32,cpl3,flat_segs})` | READ | v86/src/rust/jit.rs:369–396 |

---

## 2. What exactly is decoded, and into what (brief Q1)

### 2.1 The object decoded

Today, per instruction execution, the generated dispatcher (src/cpu/interpreter_gen.c,
from gen/interpreter_c.js) does: read opcode (`read_imm8`), read modrm byte, `modrm_resolve`
(read sib/disp **from the instruction stream**, read base/index registers, add the
segment base via `get_seg_prefix`), read immediates **from the stream**, then call
`instr_XX_mem(addr, g, imm)` / `instr_XX_reg(r, g, imm)` / `instr_XX(imm)`.

Everything except the register values and the segment base is a pure function of the
instruction bytes + the mode. A **uop** is that decoded residue:

```c
/* src/cpu/uop_cache.h (new) */
typedef struct uop {
    void     (*fn)(void);   /* instr_XX_mem/_reg/plain -- the semantic body, unchanged */
    uint8_t  shape;         /* which generic executor: plain0/1/2, reg0/1/2, mem0/1/2,
                               ud, fetch_internal (8D/8F/guard) */
    uint8_t  pfx;           /* the instruction's accumulated prefix byte (re-materialised) */
    uint8_t  seg;           /* segment choice: DS, SS, or UOP_SEG_NONE (lea: no base) */
    uint8_t  base, index;   /* register indices, UOP_REG_NONE, or UOP_REG_ABSDISP */
    uint8_t  scale;         /* 0..3 */
    uint8_t  g;             /* modrm >> 3 & 7 (fixed_g selection made at decode) */
    uint8_t  flags;         /* UOP_BLOCK_END (bb && !no_bib), UOP_FETCH_INTERNAL */
    uint8_t  ts_guard;      /* task_switch_test[_mmx] must run at execute time */
    uint32_t disp;          /* displacement or absolute (i32) */
    uint32_t imm, imm2;     /* immediate values, read ONCE */
    uint32_t eip;           /* instruction start (virtual) -- previous_ip source */
    uint16_t pfx_len;       /* prefix-byte count (for fetch_internal positioning) */
    uint16_t off;           /* byte offset of the opcode within the block */
} uop_t;                    /* ~44–48 B; layout tuned in M3 */
```

The per-encoding knowledge that builds a uop is **generated**, not hand-written: a new
emitter pass (`gen/interpreter_uop.js`, §8 step 1) reads the same
`v86/gen/x86_table.js` (unchanged) and emits, per encoding, (a) an entry in a decode
descriptor table `{ fn, shape, imm widths, block_end, ts_guard }` indexed by the same
`opcode | (is_32 << 8)` key the dispatcher uses, and (b) the decode-time arm selection
(the 66/F2/F3 prefix if-chain and the fixed_g switch) reproduced from
`gen_instruction_body` (gen/interpreter_c.js:299–412) — the same selection logic, so the
two generators cannot drift without a diff in the shared selection code.

### 2.2 The block

A **block** = one linear decode run, in a single virtual page (which is therefore a
single *physical* page — translation is contiguous), terminated by:

1. an instruction whose table entry has `block_boundary && !no_block_boundary_in_interpreted`
   — **the mode/mapping-changing set**: call, ret, hlt, int/iret, string-reps,
   mov-sreg (0x8E), pop ss/ds/es, lss/lfs/lgs, pusha/popa, enter, in/out, mov cr0–4
   (0F22), invlpg (0F01/7), far transfers, sysexit/sysenter, fwait, … — READ from
   x86_table.js (§1). **These end the block because the reference ends its batch there,
   and the batch break is the reference's re-synchronization point** (see §5.1: it is
   how the reference guarantees the next fetch uses the post-change mapping and mode);
2. an encoding with no uop descriptor yet (per-opcode enablement lever — §7);
3. the virtual page end (blocks never span pages);
4. a decode cap (64 uops / 240 bytes) — OUR cap, semantics-free (§5.3);

and it **continues through** jcc/jmp/loop (`no_bib` — the reference's interpreter also
runs through them, cpu.c:132–138) and through ordinary modrm instructions (the
`!custom && e` clause of `instruction_postfix` is interpreter-batch-only and is NOT a
block end — see §5.1 for why that is safe).

An instruction whose bytes cross the page end (start page ≠ end page) is the block's
last uop; the block is registered in **both** physical pages for invalidation.

### 2.3 Execution

The executor is a hand-written loop over the uop array, using **generic executors per
signature shape** (plain0/1/2, reg0/1/2, mem0/1/2, ud, fetch_internal — ~12 tiny
functions in src/cpu/uop_cache.c), not one generated function per encoding. The uop
carries the `instr_XX_*` function pointer; the generic executor materializes the
operands and calls it:

```c
static void exec_mem0(uop_t *u) {                    /* instr_XX_mem(addr, g) */
    int32_t addr = uop_addr(u);                      /* get_seg + regs[base] + regs[index]<<scale + disp */
    if (cpu->pagefault) return;                      /* same abort channel as modrm_resolve */
    ((void (*)(int32_t, int32_t))u->fn)(addr, u->g);
}
```

`uop_addr` replaces modrm_resolve's execute-time work with the *uniform* expression over
the pre-extracted descriptor — eliminating `resolve_modrm32`/`resolve_sib`/`modrm_resolve`
from the hot path (~223M calls per 200M instructions), while the decode-time extraction
reuses the *same* logic (§8 step 3 refactor — one copy of the addressing semantics).

The run loop (skeleton — the load-bearing piece, stored as the unit's artifact):

```c
/* inside cycle_internal, after get_phys_eip() succeeded */
blk = block_lookup_or_decode(phys, cpu->state_flags & CACHED_MASK_ALL);
if (!blk) { interpreted_run(phys); return; }         /* fallback, §5 */
for (;;) {
    u = &arena[blk->uops + idx];
    if (uop_abort_pending) return;                   /* SMC: pending NOT flushed (§4.2) */
    cpu->previous_ip = u->eip;                       /* the reference's loop-tail update */
    cpu->prefixes = u->pfx;                          /* re-materialise the instruction's prefixes */
    if (u->flags & UOP_FETCH_INTERNAL)               /* 8D/8F/guard: bodies re-read the stream */
        cpu->instruction_pointer = u->eip + u->pfx_len + u->opcode_bytes;
    cpu->pagefault = false;                          /* D1's dispatch-entry clear (cpu.c:118) */
    exec[u->shape](u);
    cpu->pagefault = false;                          /* D1's post-dispatch clear (cpu.c:126) */
    cpu->prefixes = 0;
    pending += 1;
    if (interpreted_unimplemented) return;
    if (jit_block_boundary) { flush_pending(); jit_block_boundary = false;
                              if (u->flags & UOP_BLOCK_END) return; }
    if ((u->eip ^ cpu->instruction_pointer) & ~0xFFFu) { flush_pending(); return; }
    idx += 1;                                        /* eip mismatch vs next uop => control flow */
    if (idx >= blk->uop_count) return;               /* block end */
    if (u->eip + u->len != cpu->instruction_pointer) return;   /* jcc taken / ret / … : re-lookup */
    if (pending >= INTERPRETER_ITERATION_LIMIT && u->eip >= cpu->instruction_pointer) {
        flush_pending(); return;                     /* cpu.c:134–136, same predicate */
    }
}
```

Every check in that loop is a reproduction of a line in the reference's
`interpreted_run` (cpu.c:104–149); `flush_pending()` is §5.2.

---

## 3. The cache: key, lookup, growth

**Key:** `(phys_eip of the block start, cpu->state_flags & CACHED_MASK_ALL)` — upstream's
own key (jit.rs:369–396, `CachedStateFlags` = is_32 | ss32 | cpl3 | flat_segs).
VERIFIED the port keeps `state_flags` current on every segment/cpl/cs-size change
(§1) — the key needs no new maintenance.

**Structures** (all in src/cpu/uop_cache.c, plain C, aarch64-safe — §9):

- `uop_t arena[]` — fixed, sized at init (M3 tunes; start 64K uops ≈ 3 MB).
- block headers `{phys_start, phys_len, state, uop_begin, uop_count, next, page_b}`.
- an **open-addressing hash table** (16K slots) keyed by (phys_start, state), full-key
  compare on probe — collisions cannot alias a block.
- a **per-physical-page registry**: flat array `page_head[memory_size >> 12]` of block
  lists (64 KiB for the 64 MiB boot config, 2 MB worst case for a 2 GiB guest) — O(1)
  invalidation per dirty page.
- **overflow policy: clear the whole cache** (free all blocks, keep the uop arena
  pointer reset). Correctness is unaffected (decode is a pure function of the bytes);
  cost is a re-decode wave. M3 measures clear frequency on the boot; if it clears more
  than ~once per 10M instructions, add FIFO eviction — not before the number says so.

Lookup at block start is one hash probe. The tight-loop fast path (backward jcc to the
block start) is M3: before leaving the executor, one compare `target_phys == blk->phys_start`
→ `idx = 0` without a hash lookup (M2 correctness does not need it; the lookup hit is
correct either way).

---

## 4. Invalidation — the honest answer (brief Q2)

The port dropped upstream's code-invalidation machinery deliberately
(results/cpu-port-plan.md §3: `tlb_code`, `TLB_HAS_CODE`, `tlb_set_has_code*`). This
plan restores an **equivalent of the dirty-page half only**. The inventory is exact
because the port's comments mark every dropped call site:

### 4.1 The write sites that must invalidate (restore the dropped hooks)

| Site (port) | Upstream | What to add |
|---|---|---|
| memory.c:342–351 `write8` non-mmap arm | memory.rs:194 `jit_dirty_page` | `uop_invalidate_page(phys)` |
| memory.c:355–364 `write16` | memory.rs:209 `jit_dirty_cache_small` | `uop_invalidate_page(phys)` + page+1 if the write straddles (the existing comments: "can't happen when paging is enabled") |
| memory.c:372–381 `write32` | memory.rs:223 | same |
| paging.c:638–650 `safe_write8` (the `write8_no_mmap_or_dirty_check` call) | cpu.rs:3941–3956 `if !can_skip_dirty_page { jit_dirty_page }` | `uop_invalidate_page(phys)` — **the port collapsed the can_skip_dirty split; always invalidate** (cheap: bitmap check + short list walk; the can_skip optimisation only matters for the JIT's bigger structures) |
| paging.c:652–693 `safe_write16/32`, 695–750 `safe_write64/128` | cpu.rs:3958–4040 | same (64/128 non-straddle arms) |
| string.c:276–309 movs/stos/ins fast paths (`write*_no_mmap_or_dirty_check`, `memcpy/memset_no_*`) | string.rs:248 `jit_dirty_page(phys_dst)` | `uop_invalidate_page(phys_dst)` (+ straddling pages) |
| memory.c:63–77 `write_blob` | cpu.js memory.rs:74 area `jit_dirty_cache(offset, offset+length)` | invalidate the range (loader-time; cheap) |
| paging.c:188/217/223 A/D-bit side-effect `write8`s | cpu.rs same, through `write8` | covered **by hooking write8 itself** |

Anything writing `mem8` by another route is a bug to be found by the check: **rg
`no_mmap_or_dirty_check|memset_no|memcpy_no|mem8\[` over src/ and reconcile each call
site against this table** — that reconciliation IS step 4's gate (a missed site = stale
code executed = silent wrong behavior; the oracle only catches it if the workload
self-modifies, and VERIFIED: no nasm test does — hence the dedicated SMC selftest,
step 6).

### 4.2 The currently-executing block (SMC)

The reference re-fetches every instruction, so a guest write into the page it is
executing from is visible at the next instruction. A cached block would keep executing
stale uops. Fix: the invalidation hook also checks the write against the **current
block's registered pages** (a global `{phys_start, phys_len, page_b}`); overlap sets
`uop_abort_pending`, the executor exits at the next uop boundary, and — crucially —
**does not flush `pending`** (§5.2: the write is invisible to the reference's batch,
which continues through it; the re-entry re-decodes from the current eip and sees the
new bytes). Over-aborting (a write behind the current eip in the same page) is correct,
just less fast.

### 4.3 Mode/mapping changes — covered by the block shape, not by hooks

Every instruction that can change the fetch mapping or the execution mode is a
`block_boundary && !no_bib` table entry (READ §1: mov cr0–4, invlpg, far transfers,
mov sreg, pop ss, iret, …) and therefore a block end; the next block is keyed by the
post-change translation and state byte. No TLB-flush hooks needed — **because the key is
physical**, a remapping changes the phys address the lookup uses; upstream needed
`clear_tlb_code` on invlpg/CR3/CR0 (cpu.rs:4458–4473) only because its entry points are
keyed by *virtual* address. This is the single biggest simplification versus upstream,
and it is worth stating as a claim to be tested: step 6's differential gate runs the
boot (heavy paging) cache-on vs cache-off.

### 4.4 What is NOT restored, and why that is safe

- `TLB_HAS_CODE` / `tlb_set_has_code` (cpu.rs:2514+): upstream marks pages with compiled
  code so the *JIT write fast path* can abort the translated module. Our executor is
  not native code; the SMC check (§4.2) is the whole need.
- `clear_tlb_code` on TLB flushes: §4.3.
- `jit_dirty_page_ctx`'s whole wasm-table bookkeeping (jit.rs:2242–2340): no codegen.
- `translate_address_write_jit`'s SMC exit (cpu.rs:1975–1990): §4.2's equivalent.

**Known residual divergence** (documented, cold path): a *page-crossing* instruction
whose table entry carries a `task_switch_test`/`sse` guard. The reference runs the guard
*before* fetching the operand bytes (gen/interpreter_c.js:199–202), so with CR0.TS set
it delivers #NM without touching the next page; a decode-time read of that next page
would deliver #PF instead. The decoder therefore **never caches a block whose crossing
uop is guard-flagged** — it falls back to `interpreted_run` for the whole block, which
executes with the reference's exact order. (Non-crossing guard-flagged uops are fine:
the in-page modrm read cannot fault; the uop runs the guard at execute time and
materializes only if it passes.)

---

## 5. The fallback shape and the accounting (brief Q3)

### 5.1 When a block is abandoned, and what happens on each transition

- **Cache miss / no descriptor / guard-crossing / decoder fault:** fall back to the
  existing `interpreted_run` (cpu.c:104–149) for that block — the reference's own path,
  untouched. Per-opcode enablement is exactly this lever: any block containing an
  encoding without a uop descriptor runs interpreted (§7).
- **Key mismatch (state_flags differ):** the lookup misses; a new block is decoded and
  cached. Old blocks for the old state byte remain but are unreachable until the mode
  returns — bounded by the arena cap, evicted by the clear-on-overflow. **No eager
  mode-change invalidation exists or is needed** — correctness follows from the key.
- **Guest write to a cached page (§4.1):** the page's block list is freed (unlinked from
  the hash). Next execution decodes fresh bytes.
- **Write into the executing block (§4.2):** abort the cached run, keep pending, re-enter
  at the current eip.
- **Segment-limit/CS/SS/cpl change:** block end by table shape (the change-maker is a
  flush-point instruction); next lookup re-keys.
- **`sti` (instr_FB, instructions.c:2145–2164):** the ONLY instruction with a runtime
  nested dispatch (it executes one shadow instruction and `handle_irqs()` inside its
  body). It must be a block end (the next uop would double-execute the shadow
  instruction). It is: block_boundary, no no_bib — and step 3 adds it to the decoder's
  forced-end set with a comment, because it is the one entry where the table flag and
  the cache's need coincide for a different reason.
- **`hlt`:** block end by table shape (0xF4), so the executor never runs past it;
  do_many_cycles_native's `in_hlt` check (cpu.c:178–181) then idles the machine exactly
  as today.
- **`after_block_boundary()` / `jit_block_boundary`:** the uop executors call it exactly
  when the generated dispatcher does (the `instruction_postfix` predicate,
  gen/interpreter_c.js:187–190 — including the `!custom && e` clause). The executor
  *reads* the flag per uop as the flush signal (§5.2) but only ends the block on
  UOP_BLOCK_END. The flag's other reader (the fallback loop, cpu.c:132) sees it as
  today whenever the fallback runs.
- **`previous_ip`:** set per uop to the uop's eip before execution — the reference's
  loop-tail update (cpu.c:140), so every faulting body finds exactly the reference's
  value (trigger_pagefault uses it, paging.c:115).
- **`LOOP_COUNTER` batching (do_many_cycles_native):** untouched — it sits above
  cycle_internal (cpu.c:174–183).

### 5.2 The instruction-counter staircase — the part a naive cache gets wrong

`instruction_counter` is added **once per interpreter batch** (cpu.c:148), and the
batch ends at: jit_block_boundary (every non-custom modrm instruction + every flush-point
instruction), page-cross, iteration-limit. `microtick()` =
counter/1000 (exec_boundary.c:83–88) feeds the TSC, the PIT, and the ACPI timer, and
**the guest can read them mid-batch** (rdtsc; PIT/PM-timer port reads) — so the
counter's staircase is guest-observable state, and the cache must reproduce the
reference's staircase **value for value at every instruction boundary**. The rules
(executor skeleton, §2.3):

- `pending` accumulates executed uops across cache blocks and across cycle_internal
  returns **until a reference batch end** — the cache block is a *sub-batch*;
- **flush** (`instruction_counter += pending; pending = 0`) at: a uop whose exec set
  `jit_block_boundary` (covers BOTH postfix clauses — plain modrm instructions flush
  mid-block without ending it), a page-cross exit, and the iteration-limit break;
- a fetch fault at block entry **discards pending** — the reference's quirk, reproduced
  (cpu.c:40–45: the tail-fetch fault returns before `+= i`; the loss is exactly the
  batch's own count, and since pending is flushed only at batch ends, the loss is
  exactly the reference's loss);
- the SMC abort keeps pending (§4.2).

Because the block ends at every flush-point instruction, the common case degenerates to
"flush at block end" — but the general rule above is what makes jcc/jmp (no_bib: no
flush), mid-block modrm flushes, and fetch-fault losses exact.

---

## 6. The expected win, honestly bounded (brief Q4)

### 6.1 The arithmetic

Per 200M instructions the profile counts (context) give, per instruction:

| Class | Count/instr | Fate under the block cache |
|---|---|---|
| read_imm8 (fetch opcode+modrm+imm) | 1.69 | **gone from the hot path** (decode-time only) |
| get_phys_eip (per-instr fetch translate) | 1.00 | **gone** (once per block entry) |
| resolve_modrm32 + modrm_resolve + resolve_sib | 1.04 | **gone from the hot path** (uniform `uop_addr` over the decoded descriptor) |
| translate_address (fetch-related share) | 0.20 | mostly gone |
| read_reg32 + write_reg32 (operands) | 1.66 | stays (semantic) — **PERF-1's target** |
| read32s + write32_no_mmap_or_dirty_check (memory model) | 0.31 | stays |
| the switch dispatch + prefix chains | uncounted | replaced by one indirect call per uop |

≈ **3.9 of ~5.8 counted calls/instr removed**; the remainder is the semantic bodies,
the register accessors, and the memory model. If cost scales with the remaining calls,
the projection is 6.26 s → **3.3–3.8 s (1.6–1.9x)**. The honest spread: the uncounted
per-call work (cosmocc -O2, no LTO, indirect-call overhead in the executor loop) and
the uop materialization (`get_seg` per memory operand — faultable, kept) can eat half
the theoretical win. **Stated range: 1.3–1.9x, central estimate ~1.5x → ~4.2 s**, which
ties-to-beats the pinned reference's own JIT (4.90 s) and clearly beats its interpreter
(5.33 s).

**Composition with PERF-1:** the two steps remove *different* calls (PERF-1: cross-TU
call overhead on the accessors that remain; PERF-2: the fetch/decode calls themselves).
Expected to compose multiplicatively over the disjoint parts; re-measure after each.

### 6.2 The measurement that confirms or refutes

Run the boot benchmark with the cache on, and print (M3's instrumentation): wall time,
**block hit rate**, **fallback fraction** (instructions executed via interpreted_run),
block count, invalidation count, cache-clear count. **Refutation rule:** if the hit
rate is < ~85% or the fallback fraction > ~5% of instructions, the cache is not the
bottleneck for this workload and the win will collapse toward ~1.1x — stop, report,
reconsider. If the hit rate is ≥ 95% but the speedup is < 1.2x, the executor loop's
own overhead is the story (indirect calls under cosmocc) — profile again before M3's
tuning.

### 6.3 The alternatives, priced (brief Q4)

| Option | Effort | Expected win | Verdict |
|---|---|---|---|
| (i) superinstructions for the hottest few | a pattern-scan mini-decoder in the dispatch — ~40–60% of the block cache's M2 work | captures the top 2–3 loop shapes; maybe 1.1–1.2x; no generality | **subsumed**: the block cache is the general mechanism; superinstructions are a special case of one-uop-per-pattern |
| (ii) fused fetch path | step 1's territory (structural inlining of read_imm8/get_phys_eip) | removes the cross-TU call cost, not the redundant work | **moot under the cache** (fetch leaves the hot path entirely); still worth doing for the fallback path |
| (iii) direct-threaded/computed-goto dispatch | ~30 min emitter change + flag | saves the switch's range check/dispatch; likely 5–10%, shrinks as the cache covers more | skip: the uop executor's indirect call is the same machinery, applied where it pays |
| (iv) wait for step 1's numbers | 0 | — | **yes, as the gate**: land M2 on PERF-1's tree and re-baseline §6.1's remaining-call accounting with PERF-1's measured result |

**Bottom line:** build the block cache, but sequence it after PERF-1 and treat §6.2's
refutation rule as a hard stop, not a formality.

---

## 7. The oracle's role: incremental validation (brief Q5)

The gates already exist and are differential: the nasm corpus (118/118, per-opcode
execution compared against native fixtures — the per-opcode enablement gate), the M0
transcript diff (byte-identical vs the pinned reference — the behavioral gate), and the
kernel boot (serial + the guest's own ATA md5 — the integration gate). The block cache
adds **one new gate**: every one of the three, run twice, `--uop-cache off` vs `on`,
outputs **byte-identical** — the staircase and all semantics are covered by that diff
alone, because the cache is deterministic.

**What a mismatch looks like:** a wrong decode shows up as an instruction_pointer
divergence at the first mis-decoded instruction → the transcript diverges there; a
missed invalidation site shows up only where the workload writes code — which is why
the dedicated SMC selftest (§8 step 6) exists: it is the only gate that *provably*
exercises §4.1/§4.2, since VERIFIED no nasm test self-modifies.

**Milestones, each independently verifiable:**

- **M1 (decode only, no execution):** the emitter's descriptor table + the hand decoder
  produce uop arrays. Gate: a decode-checker selftest that walks byte streams (the nasm
  corpus images + a hand table of every base opcode × 16/32, all SIB forms, prefix
  combos, page-crossing cases) and asserts per instruction: (a) the eip advance equals
  the interpreter's own advance over the same bytes (differential — no ground truth
  needed), (b) the extracted base/index/scale/disp/seg equals `resolve_modrm16/32`'s
  outputs, (c) block framing (page-end stop, crossing handling, flush-point ends).
  This is the **smallest first milestone**: it pins the decoder against the port's own
  existing semantics without executing anything through the cache.
- **M2 (executor + cache + invalidation):** the full path behind `--uop-cache`, all
  implemented opcodes, fallback for the rest. Gates: `make selftest`, `make nasm`,
  `make transcript-diff`, the boot (serial + md5), **each cache-off vs cache-on
  byte-identical**.
- **M3 (measure + tune):** §6.2's instrumentation; loop fast path; arena sizing.
  Gate: the benchmark numbers vs 6.26 s and the refutation rule.
- **M4 (only if M3 justifies):** segment-base caching (skip `get_seg` when flat + a
  segment epoch is unchanged), mid-block jump-target re-entry, eviction policy.

---

## 8. File-level steps, in order (brief Q6)

Order respects the concurrent PERF-1 (steps 3–6 touch its files — land them on
PERF-1's tree and re-check the §10 hashes first). Steps 1–2 touch only new files and
can proceed in parallel with PERF-1.

1. **`gen/interpreter_uop.js` (new, ~400–500 lines)** — second emitter pass, mirrors
   gen/interpreter_c.js:299–459 structure; imports `v86/gen/x86_table.js` unchanged;
   emits `src/cpu/uop_gen.c`: the descriptor table (per `opcode | (is_32<<8)`, 0F and
   base), the prefix if-chain and fixed_g selection as decode-time code, and the
   fn-pointer resolution (name → `instr_XX_*`, using the same availability scan as
   gen/interpreter_c.js:96–107 — an encoding without a definition gets "no descriptor"
   → block falls back). **Gate:** `make` + the emitter's own counts; uop_gen.c is a
   pure function of x86_table.js (byte-reproducible, like interpreter_gen.c:541).
2. **`src/cpu/uop_cache.h` + `src/cpu/uop_cache.c` (new, ~700 lines)** — the uop/block
   structs, arena, hash, page registry, generic executors, the decode walk (prefix loop
   mirroring instructions.c:63–77, 615–627, 1888–1894, 1907–1918: 66/67/F2/F3/F0 and the
   six segment prefixes), the block executor (§2.3), pending/flush (§5.2), SMC abort
   (§4.2), instrumentation counters. **Gate:** compiles clean `-Wall -Wextra -Werror`;
   M1's decode-checker selftest.
3. **`src/cpu/modrm.c` + `modrm.h`** — split `resolve_modrm32` (modrm.c:335–530),
   `resolve_modrm16`, `resolve_sib` into descriptor extraction (decode-time; no register
   reads, faultable only on disp/sib fetches) + uniform materialization
   (execute-time: regs + get_seg). `modrm_resolve` (567–574) becomes
   materialize(extract(…)) — byte-identical to today, **one** copy of the addressing
   semantics shared by interpreter and cache. **Gate:** `make nasm` + M1's (b).
4. **`src/cpu/memory.c`, `src/cpu/paging.c`, `src/cpu/string.c`** — restore the §4.1
   invalidation hooks at the comment-marked sites (memory.c:348/361/378, paging.c
   safe_write* arms, string.c:276–309, write_blob) + the §4.2 abort check. **Gate:** the
   §4.1 reconciliation (rg every `mem8`/`*_no_mmap_or_dirty_check` writer against the
   table — the gate is the *empty diff between the two lists*), `make nasm`.
5. **`src/cpu/cpu.c`** — cycle_internal (153–171) gains the lookup/execute/fallback
   branch behind a `uop_cache_enabled` flag; interpreted_run stays verbatim as the
   fallback; expose the counters. **Gate:** all gates, cache off = today's bytes
   (cache-off transcript byte-identical to the pre-change transcript).
6. **`src/cpu/uop_cache_selftest.c` (new) + Makefile** — the M1 decode checker, the SMC
   selftest (a tiny image that writes into its own executing page and asserts the
   modified instruction runs — the only provable §4.2 gate), the staircase pin
   (cache-on vs cache-off counter trace equality across a stream with jcc loops, modrm
   mid-block flushes, page-cross ends, and a mid-run fetch fault), the state-key pin
   (segment change ⇒ lookup miss). Wire uop_gen.c into `make` (Makefile CPU_SRC +
   generator rule). **Gate:** `make selftest`, `make nasm`, `make transcript-diff`, boot,
   all × cache-on/off.
7. **Benchmark instrumentation + report** — M3.

---

## 9. Risks, with source and cost (brief Q7)

1. **A missed write site (§4.1) = stale code, silently wrong.** Source: the hooks were
   dropped at 8+ sites; the port's comments mark them but nothing checks completeness.
   Cost: wrong guest execution on any self-modifying workload — and VERIFIED no gate
   exercises SMC, so it would ship undetected. Mitigation: the rg-reconciliation gate
   + the dedicated SMC selftest + the cache-on/off boot diff (the boot does write
   near-code regions — the A/D side effects through write8 at least).
2. **The counter staircase / TSC-PIT quantization.** Source: microtick is counter/1000
   (exec_boundary.c:83–88); the batch granularity IS the clock granularity. Cost if
   wrong: rdtsc/PIT/PM-timer reads diverge from the port's deterministic contract and
   potentially from the boot's serial diff. Mitigation: §5.2's exact rules + the
   staircase pin (step 6) + the byte-identical cache-on/off diffs.
3. **previous_ip / fault-eip semantics.** Source: cpu.c:140, paging.c:115. Cost: a
   wrong previous_ip corrupts every delivered fault's restart address. Mitigation:
   per-uop eip set before exec (§5.1); exercised by the boot (heavy paging) and pinned
   by M1's decode checker.
4. **Prefix/mode state.** Source: bodies read cpu->prefixes live (segment overrides,
   rep, asize — modrm.c:19–63, string.c). Cost: a stale or leaked prefix byte changes
   operand size/addressing silently. Mitigation: uop.pfx re-materialization + the
   DBG_ASSERT-equivalent (prefixes == 0 at block entry, mirroring cpu.c:116) +
   M1's (a) differential.
5. **Cache growth in a 2 GiB guest.** Source: mem8 is the full guest RAM (memory.c).
   Cost: uncontrolled growth = OOM in a long-running guest. Mitigation: everything is
   fixed-size sized at init (registry 2 MB worst case, arena ~3 MB, hash 16K slots),
   clear-on-overflow; measured clear frequency in M3.
6. **cosmocc codegen of the executor loop** (no LTO, indirect calls). Cost: the 1.3x
   floor of §6.1. Mitigation: M3 measures before tuning; the generic-executor shape
   (12 functions, uop fields in registers) is the structure most likely to compile
   well; if it does not, the fallback is generated per-encoding executors (bigger
   file, same design).
7. **aarch64 / APE posture.** The design introduces no JIT, no self-modifying code, no
   computed goto — only data structures and function pointers, portable C by
   construction. Cost if violated: the aarch64 APE target breaks. Mitigation: the
   constraint is stated in uop_cache.h's header comment; the APE build gate covers it.
8. **PERF-1 concurrent edits.** Source: the brief; hashes in §10. Cost: merge conflicts
   in modrm.c/paging.c/memory.c/string.c/cpu.c. Mitigation: steps 3–6 land on PERF-1's
   tree; re-hash before editing.
9. **The guard-crossing residual (§4.4)** — known, bounded, fallback-covered; cost if
   mishandled: an exception-order divergence on a cold path. Mitigation: the decoder's
   give-up rule + a comment naming the case.
10. **in_mapped_range code fetches.** get_phys_eip asserts code is never fetched from
    device ranges (cpu.c:99) — a pre-existing port invariant, not ours; the cache
    inherits it unchanged (blocks are only decoded at translated RAM addresses).

---

## 10. File hashes at planning time (sha256, first 12 chars)

```
42668edf380d  src/cpu/cpu.c          15916226b0c4  src/cpu/cpu.h
05dbf8ce2628  src/cpu/cpu_state.h    9ca61a23817a  src/cpu/memory.c
949942c6b762  src/cpu/paging.c       48c5e9bd2163  src/cpu/modrm.c
351b15f2a4dc  src/cpu/modrm.h        bcab21bad3ae  src/cpu/instructions.c
1dbad114ef66  src/cpu/instructions_0f.c  70f7f84193ad  src/cpu/string.c
5b46e29f2198  src/cpu/cpu_state.c    ba3ed9f6920f  src/cpu/interpreter_gen.c
c01a320897b8  gen/interpreter_c.js   272218337ad6  v86/gen/x86_table.js
7b585fdeca61  results/cpu-port-plan.md  45b1dfa60f35  Makefile
```
