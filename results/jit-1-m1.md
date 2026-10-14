# JIT-1 (M1) — the x64 emitter, ONE recipe, the block compiler, and the uop_run branch

**Unit:** `HANDOFF=v86c-jit`, unit **JIT-1** (coder), milestone **M1**. **Inputs:**
`/tmp/brief-v86c-jit-1.txt`, memory nodes `handoff-v86c-jit-ctx` / `-plan` /
`-plan-artifact-1` / `-result`, `results/jit-plan.md` (**§8's CORRECTION is binding**: the
spike's C ≈ 54–80× is a PER-SHAPE harness ratio and predicts nothing end-to-end; the planning
range is 1.3–1.6× and the refutation rule is the number that decides), `results/jit-spike.md`
(the feeder decision: **our own encoder** — reversed by nothing, re-measured by nothing, and
adopted), and the code.

Claims are tagged **MEASURED** (command + raw output in `results/jit-1/`), **READ** (from
source, with file:line) or **INTERPRETATION**.

---

## 0. Bottom line

The JIT exists, compiles the uop cache's own blocks, and **`--jit off` and `--jit on` produce
byte-identical machines**: `make`, `make selftest` (counts unchanged), `make nasm` 118/118 in
all four (cache × jit) legs, `make transcript-diff` in all four legs, the 200 M boot's final
state and serial sha, and the 1.5 G boot's final state — plus `--jit off` reproduces the
**pre-change artifacts byte for byte** (the spike-era transcript *and* the pre-spike binary).
`make bench` with `--jit on` is **not** faster (best-of-7 ratio 0.999–1.001 over three full runs) — as the brief said it
would not be — and **the reason is not "one recipe"**: it is measured in §4 and it is the
single most important thing this unit hands to M2.

| | |
|---|---|
| files added | `src/cpu/jit_x64.{c,h}` (637 lines, the encoder), `src/cpu/jit_codegen.{c,h}` (932 lines, the recipe table + the block compiler), `src/cpu/jit_codegen_selftest.c` (1204 lines, 363 checks) |
| files changed | `src/cpu/jit_mem.{c,h}`, `src/cpu/uop_cache.c`, `src/main.c`, `Makefile` |
| the recipe | ONE: `89 /r` with mod = 3 — `mov r32, r32` (`instr32_89_reg`), 8,663,791 of the benchmark's 181,299,249 counted bodies |
| the exit contract | the executor's own four outcomes; the compiled block returns one and **all** of the tail/lookup/re-decode logic stays in `uop_run`'s existing C |
| mutation rig | **10/10 arms CAUGHT**; control + restoration PASS (`results/jit-1-mut-rig.py`) |
| aarch64 | the backend compiles out; all six files compile clean for `aarch64-linux-cosmo-cc -Werror` |
| dynamic coverage | **0.32 %** of the benchmark's uops ran inside a compiled block; 7.16 % of *those* ran as emitted code (§4 — this is the M2 finding, not a rounding error) |

---

## 1. What was built

### 1.1 The encoder — `src/cpu/jit_x64.{c,h}`

**Our own encoder, as the spike's Q2 recommended** (`results/jit-spike.md` §3: both routes emit
the same 27-byte block byte for byte; DynASM costs a vendored Lua interpreter as a *build-time*
tool and, decisively, selects instructions at **build** time while a block compiler decides at
**run** time). This is the spike's prototype grown into what the compiler needs: **labels with
forward references and rel32 patching** (the spike had only rel8 and only what its fixed block
needed), the 64-bit stack forms, and the two-operand-subtract form the budget test needs.

The vocabulary **as built** (**40 emitters** + the label machinery; every one is called by
the compiler or pinned byte-for-byte by the selftest):

```
mov r32,r32 · mov r32,[base+disp] · mov [base+disp],r32 · mov r32,imm32 · mov r64,imm64
mov byte [base+disp],imm8 · mov dword [base+disp],imm32
add/sub/and/or/xor/cmp r32,r32 · add r32,imm32 · cmp r32,imm32
add [base+disp],r32 · add qword [base+disp],r64 · add dword [base+disp],imm8 · sub r32,[base+disp]
cmp dword [base+disp],r32 · cmp dword [base+disp],imm32 · cmp byte [base+disp],imm8
test r32,r32 · test r32,imm32 · test r8,imm8 · xchg r32,r32
inc/dec/not/neg r32 · ror/shl/shr/sar r32,imm8
push/pop r64 · sub/add r64,imm8 · call r64 · ret
jmp/jcc/call to a LABEL (rel32, patched by jx64_finish)
```

**What is NOT there, stated rather than stubbed** (the project's rule: a seat that is never
called is indistinguishable from an absent one): `movzx`; the 8/16-bit load/store forms; the
**SIB indexed** operand `[base + index*scale + disp]`; the shift-by-CL forms; `lea`. None is
emitted in M1, and the plan's ~30-mnemonic target is a plan target, not an achieved count. The
same file is **architecture-neutral byte emission** — it compiles (and its byte pins run) on
aarch64; what is x86-64-only is the compiler that feeds it.

Sizes are chosen by value (`disp8` when it fits, `disp32` otherwise; `RSP`/`R12` as a base gets
its SIB byte; `RBP`/`R13` with disp 0 gets `mod=01`), because a **block has to fit in one 4 KiB
page** — see §4, where that stops being a detail and becomes the binding constraint.

**Ground truth for the pins**: every sequence is assembled by the host's own `as --64` and
disassembled with `objdump -d -M intel`, and the pins were transcribed from those dumps
(`results/jit-1/g1-encoder-pins.txt`, with the two forms where `as` prefers a *shorter* encoding
than the emitter — `add edi,imm32` and `xchg eax,ecx` — pinned to the emitter's own choice, the
same instruction either way).

### 1.2 The recipe — ONE, and why this one

**`89 /r` with mod = 3: `mov r32, r32`** — the REG-form of the port's `instr32_89_reg`
(`instructions.c:964`: `write_reg32(r2, read_reg32(r))`, called as `fn(modrm & 7, g)` under the
REG0 shape).

Why: it is the simplest shape that proves the whole path **and nothing else**. It reads and
writes no memory, touches no flags (so the lazy-flag machinery — the plan's §9 risk 1, the
highest-risk part of any JIT — is not on the path), cannot fault, cannot move the instruction
pointer, and cannot end a reference batch. What it *does* exercise is everything M1 exists to
pin: the register contract and its spill rule, the per-uop ip/previous_ip/prefixes stores, the
pending staircase, the shared checks routine, and all four exit kinds. And it is genuinely hot:
`instr32_89_reg` is **8,663,791** of the benchmark's 181,299,249 counted instruction bodies
(`results/perf-5/gprof-flat-after.txt`).

**DIVERGENCE from the plan (stated, with its reason).** The plan's §6 "smallest M1" names
`mov r32, r/m32` **with the inlined TLB fast path**. M1 builds the *register* form instead, per
the brief's own words ("a no-flags, non-memory, non-faultable move/lea-like form is the natural
choice"), because the memory form drags the TLB fast path (§4.3) and its fault semantics into a
milestone whose whole point is to pin the *contract* before any of that exists. The memory
forms' uops are still executed — **called**, through `uop_materialize_addr`, exactly as the plan
says non-inlined shapes are. **MEASURED consequence**: the boot's `--jit on` run inlines 45,645
uops and calls 591,916 (§4) — no TLB path exists in M1 at all.

The table is keyed by the uop's `fn` pointer (the plan's §7 step 2 preference: **no `uop_t`
struct change**) and binary-searched **at compile time only**; it also checks the SHAPE, so a
body reached through a shape its recipe does not describe falls back instead of being
mis-called.

### 1.3 The block compiler — `src/cpu/jit_codegen.{c,h}`

Called from the ONE place a block comes into existence (`uop_cache.c`'s `uop_block_decode`), so
the compiled code is attached to the record the executor is about to use. The register contract
is the plan's §4.1: guest EAX..EDI in RAX/RCX/RDX/RBX/R12–R15, RBP = `&cpu`, R8 =
`&jit_abort_flag`, R9 = pending, RDI/RSI/R10/R11 free — and **the spill rule is uniform**: all
eight guest registers *and* the pending count go to `cpu_state`/the global before ANY C call and
come back after, which is what lets every `instr_XX_*` body work unchanged.

Layout of one compiled block:

```
prologue                    6 pushes + 24-byte frame (RSP 16-byte aligned at every call)
                            RBP=&cpu, the 8 guest regs loaded, R9=pending, R8=&abort,
                            the frame slots zeroed, pagefault=false (the D1 entry clear, hoisted)
uop 0 … uop n-1             per uop: abort check · stores · body (recipe | C call) · counts ·
                            `call l_checks` (the executor's steps 6–11)
jmp l_kind_tail             the executor's `idx >= count` (the block ran out of uops)
l_checks                    THE SHARED CHECKS ROUTINE: the executor's steps 6–11, ONCE per block
l_flush_return, l_kind_*    the exit stubs (they write the kind into a frame slot)
l_epilogue                  pending back to its global, the 8 guest regs spilled,
                            uop_stats.uops_executed and the coverage counters folded in,
                            EAX = the exit kind, pops, ret
```

**Every check is the executor's, in the executor's order** (`uop_cache.c`'s block loop):
`interpreted_unimplemented` → flush+RETURN; `ran && (FLUSH || jit_block_boundary)` → flush,
clear, budget; the page-cross test; the iteration limit; the block end; the divergence test
(against `eip + len`, **not** body_ip — a `custom_modrm_resolve` body consumes its own sib/disp);
and finally the fall-through. The load-bearing detail that makes a SHARED routine exact: the
per-uop arguments are EDI = the uop's flags (`bit0 FLUSH, bit1 BLOCK_END, bit2 "the body ran"`),
ESI = `eip + len`, R10D = `eip` — and the routine's own exits first `add rsp, 8`, because it was
`call`ed while the exit stubs run at the block's frame level.

The four **exit kinds** are `uop_run`'s own outcomes (`uop_cache.c`'s `UOP_LEFT_RETURN/TAIL/
ABORT` plus the block end): the compiled block returns one, and `uop_run`'s **existing C code**
does all the tail/next-lookup/SMC-re-decode work — the plan's central claim, that the parts not
reimplemented cannot be wrong.

### 1.4 The wiring

* `uop_cache.c`: `uop_cblock_t` gains a `uint8_t *code` (a **private** struct in the `.c`, so
  this is not the plan's step-2 `uop_t` change); `uop_block_decode` compiles (behind
  `jit_enabled`); `uop_run` branches on `blk->code != NULL` **reading the pointer the record
  already carries — no second hash on the run path** — and the compile-time path *does* consult
  the substrate's keyed table first, so a block that outlives its uop-cache record (a cache
  clear) is **found and reused** rather than recompiled into a second page.
* `uop_clear_internal` now calls `jit_code_clear_all()` — **a correctness requirement, not
  housekeeping**: that clear zeroes the shared `uop_page_code` bitmap, and a compiled block left
  alive would no longer be watched by the PERF-FIX-3 write sites, so a later guest write to its
  page would reach neither registry and the stale code would run.
* `jit_mem.{c,h}`: `JIT_CODE_PAGE_BYTES` named; the pool **256 → 2048 pages** (the plan's §5.1
  sizing: the benchmark alone decodes 740 blocks — MEASURED here: 550 pages used, peak 279 live);
  the abort flag exposed as a **byte at a known address** (`jit_abort_flag`) because a compiled
  block cannot call a function to ask; `jit_code_clear_all`; and `jit_block_enter`'s block
  lookup made **O(1)** via the code page's own back-pointer (`jit_cpage_t.block`) — the spike's
  linear scan over the block table would be 2048 iterations on *every* compiled block execution
  (the spike's own comment invites the back-pointer).
* `main.c`: `--jit-stats` prints the coverage counters; `Makefile`: the two sources join
  `$(CPU_SRC)`/`$(CPU_HDR)` (so the shipped unity TU and every link get them), the ONE link that
  takes `uop_cache.c` as a separate object gets them as sources, and the new suite is in
  `make selftest` **and** has its own target.

---

## 2. The gates, as run

Raw evidence: `results/jit-1/gates-summary.txt` and the files beside it; the script is
`results/jit-1-gates.sh` (run at load 0.57/0.83/4.10, 2026-10-13T11:59Z).

### G1 — the recipe runs compiled, and the equivalence is shown able to fail

`make selftest`'s new suite, **363 checks, 0 failures** (`results/jit-1/gates-selftest.txt`). It
compares, for every program, the **full `cpu_state_t`**, the pending count, the delivered vector
and the whole 2 MiB of guest RAM across **three** arms: the interpreter (`--uop-cache off`), the
uop cache, and the compiled block. The assertion that gives the rest meaning:

```
blocks compiled 1 in 1 attempts; uops_with_recipe 4 / without 3;
inlined 4 / called 3   (the compiled path was REALLY taken)
```

and the pins, each with the mutation arm that makes it fail (`results/jit-1/mut-rig.txt`):

| pin | what it measures | arm that CAUGHT it |
|---|---|---|
| state differential | the recipe's own result (`eax = edx`, `ecx = eax`, the stored word) in all three arms | recipe direction swapped; spill spills one register; reload reloads one register |
| SMC abort | the running block stops at its next uop boundary and the PATCHED instruction then runs (`0x2A`, aborts 1, invalidations 1) | abort check never taken |
| fault / early return | CR0.TS + a DS-null FPU mem form delivers **#NM (7)** — a materialisation-first implementation delivers #GP (13): the guard-order pin; and a DS-null plain mem form delivers **#GP (13)** | materialisation-fault check dropped |
| the `ran` gate | a **FLUSH-flagged** uop whose materialisation FAULTED is **not** a batch end (`uop_stats.flushes` equal in both engines: 2) | materialisation-fault check dropped (flushes 3 vs 2) |
| staircase | three `rdtsc` readings (0/4000/5000), the exact counter (10) and `uop_pending` (0) identical in all three arms; flushes equal | flush drops the counter add |
| prefix / previous_ip | `fs:[0x1400]` with FS's base non-zero (the override lives in `cpu->prefixes` at execute time) reads the FS word; `previous_ip == 0x100C` — asserted directly, not only via the state diff | prefixes store dropped; previous_ip store dropped |
| exit kinds round-trip | `exits[RETURN] = 4, [TAIL] = 1, [ABORT] = 1, [BLOCK_END] = 12`, produced by a page-crossing `jmp`, a taken `jnz`, the SMC write and `hlt` | page-cross mask broken |
| THE SHAPE MATRIX | one instruction per SHAPE family (PLAIN0/1, REG0 ×2, REGF0/1, MEM0, MEMF1, MEMF0, and `lea`'s custom_modrm_resolve/raw-modrm path) in a compiled block: the VALUES are asserted (eax 0xdeadbeef, ecx 0xffff8000, edx 0x22222223, [0x3104] 0x12345678, [0x3108] 42) **and** the full state matches both other arms — the called path's argument mapping measured, not inspected | *(the shape matrix is its own pin; a per-recipe operand matrix is M2's)* |
| the encoder | 42 byte-pinned sequences + the label patcher + an unbound label must FAIL + an over-full buffer must be reported | encoder takes REX.R from the wrong bit |

**`results/jit-1-mut-rig.py`: 10/10 arms CAUGHT, control PASS, restoration byte-identical PASS.**
(The rig's own first attempt at the abort arm used `JX_CC_NO`, which is JNO — taken whenever
OF == 0, i.e. always — and the machine **livelocked**: 25 minutes at 100 % CPU with the block
aborting at every uop and `uop_run` re-decoding forever. That is a finding, not just a rig bug:
see §5.)

### G2 — byte-identity, every gate, twice, in both cache modes

| gate | legs | result |
|---|---|---|
| `make` | — | rc 0; `build/v86c` sha256 `bb32733b9257e419…` |
| `make selftest` | — | rc 0; **3522 / 228 / 541 / 7934349 / 102 UNCHANGED** + the new 283 |
| `make nasm` | (cache on, jit off) · `--uop-cache off` · `--jit on` · `--uop-cache off --jit on` (refused, falls back) | **rc 0 in all four**, 118/118, 5 skipped, 0 no-state |
| `make transcript-diff` | cache on/off × jit off/on | rc 0 in all four (C transcript **byte-identical to the JS reference**) |
| `--jit off` vs no flag | — | byte-identical (stdout **and** stderr) |
| `--jit off` vs the **pre-change** artifacts | spike-era transcript (`results/jit-spike/transcript-c-jitoff.txt`) and the **pre-spike binary** (`/tmp/jitspike/v86c-baseline`) | **`cmp` rc 0 against both** |
| the 200 M boot | off / on / cache-off | identical final state; **serial sha `fae4924b…` in all three** (the benchmark's own pinned sha) |
| the 1.5 G boot | on / off / onjit | **identical final state** in all three: `in_hlt=0, exception=-1, unimplemented=0, eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000` |
| `make bench` | `--jit off` vs `--jit on`, 7 runs each, **two full gate runs** | run A: off best 2.405 / median 2.444, on best 2.407 / median 2.515 → **ratio best 0.999, median 0.972**; runs B and C (the frozen-tree re-runs): off best 2.405 / median 2.498-2.498, on best 2.402-2.407 / median 2.515 → **ratio best 1.001-1.001, median 0.991-0.993**. Over all three runs the best-of-7 ratio is 0.999-1.001 (indistinguishable from 1.000) and the median ranges 0.97-0.99: **no measurable change**, as the brief predicted |
| aarch64 | 6 files, `aarch64-linux-cosmo-cc … -Werror` | **rc 0 for all six** |

**One gate that is NOT byte-identity, measured rather than assumed**: the 1.5 G boot's *serial
bytes* differ between two identical `--jit off` runs (`bac97364…` vs `7e1fbbb5…`; first
difference: `[ 61.367678] devtmpfs` vs `[ 61.370276]`), because the guest reads the **host**
clock (`rtc.c`'s `CLOCK_REALTIME`) — so the serial stream is not a gate at 1.5 G, and the final
machine state is (`results/jit-1/gates-1.5g-serial-nondeterminism.txt`; the spike's own summary
shows three different shas across on/off/onjit too). The 200 M boot's serial IS deterministic
and is compared.

### G3 — the contract pins can fail

See G1's table: five arms target the fault return (`ran`), the staircase, and the
prefix/previous_ip stores specifically, and each is CAUGHT.

### G4 — the coverage counters agree with the descriptors compiled

Asserted in the suite (4 recipe + 3 non-recipe compile-time, 4 inlined + 3 called dynamic for
the one program), and printed by `--jit-stats`:

```
jit: blocks compiled 550 in 740 attempts (reused 0); refused: fit 190 (8998 uops), pool 0, empty 0
jit: block sizes: compiled 3051 uops in 887655 bytes (mean 290.9 B/uop, 5.5 uops/block);
     refused-for-fit 8998 uops in 190 blocks (mean 47.4)
jit: uops executed inlined 45645 / called 591916 -> 7.16% dynamic inlined
     (compile-time: 617 with a recipe, 11432 without)
```

### G5 — aarch64

rc 0 for `jit_mem.c`, `uop_cache.c`, `jit_selftest.c`, `jit_x64.c`, `jit_codegen.c`,
`jit_codegen_selftest.c`. `V86C_JIT_X64 == 0` compiles the whole backend out (`jit_codegen.c`
reduces to stubs that return NULL), `--jit on` reports unavailability, and the uop cache is the
engine — unchanged from the spike's posture.

---

## 3. The recipe's coverage on the three workloads (what the gates actually exercise)

| workload | blocks decoded | compiled | refused for fit | dynamic inlined | uops through compiled blocks |
|---|---|---|---|---|---|
| `tests/test.img` (M0 transcript) | 4 | 1 | 3 (149 uops, mean 49.7) | 0.87 % | 115 |
| the nasm corpus (123 images) | — | **73** | — | **1 uop** | 131,957 |
| the 200 M kernel boot | 740 | 550 | 190 (8998 uops, mean 47.4) | 7.16 % | 637,561 = **0.32 % of 200,095,213** |

**READ THE SECOND ROW, because it bounds the nasm gate's power honestly**: `make nasm --jit on`
is 118/118 and compares the FULL final state against the JS reference, so it *does* prove the
compiled path's contract on the 73 blocks it compiles — but those blocks run ~1 inlined uop and
131,957 called uops in total, because the corpus's blocks are 22–37 uops and **do not fit in one
code page**. The strong M1 evidence is therefore the codegen suite (G1) plus the 200 M / 1.5 G
boots, not the nasm leg.

---

## 4. THE FINDING: the fit rule, not the recipe count, is what bounds M1 — and what M2 must fix

**MEASURED** (G4's numbers, plus `results/jit-1/gates-jit-coverage.txt`):

* a compiled block costs **≈ 550–580 bytes of fixed emission** (prologue + epilogue + the shared
  checks routine + the four exit stubs; MEASURED two ways: the suite's two 2-uop blocks came to
  913 B each, and the boot's compiled blocks average 1614 B for 5.5 uops) and **~190 bytes per
  CALLED uop**, of which the spill (41) + reload (51) + the four state stores (28) + the call
  (13) + the per-uop checks call (21) are the bulk; an inlined (recipe) uop costs ~50;
* so one 4 KiB page holds a block of **at most ~19 called uops**;
* and the boot's 740 blocks split cleanly: **550 small ones (mean 5.5 uops) compiled**; **190 big
  ones (mean 47.4 uops) refused**, and those 190 hold **8,998 of the 12,049 arena uops** and
  **99.7 % of the dynamic uop executions**.

That is why `make bench --jit on` is not faster, and it is a *sizing* result, not a dispatch
result: the blocks that compile are the cold ones. The plan's §3 coverage target (**≥ 85 %
dynamic inlined after M2**) is unreachable by adding recipes alone if the hot blocks never
compile.

**What this implies for M2, quantified**: a 47-uop hot block needs ≈ 8900 B at the called-uop
cost and **FITS at ≈ 2900 B** if its uops are inlined (580 + 47 × ~50) — i.e. **M2's inlining is
also the fix for the fit rule**, provided the recipes cover the hot shapes (the plan's own
census: mov/lea 32.6 %, ALU 26.9 %, jcc 14.9 %, movzx 9.9 %, inc/dec 3.6 %). The other three
levers, in ascending cost: shrink the fixed ~580 B (the checks routine is one per block and could
become a per-process thunk that returns the kind); let a block span contiguous pages (the pool IS
one mmap — the substrate's one-block-one-page rule is what forbids it); compile only blocks that
both fit and are hot (the plan's "no hotness gate" is what forced the fit test onto the compile
path at all).

The coverage instrument for measuring each of those already exists and is asserted by the suite:
`refused_fit`, `refused_fit_uops`, `compiled_uops`, `bytes_emitted`, `uops_with(out)_recipe`,
`inlined_uops`, `called_uops`, `exits[4]`.

---

## 5. Divergences from the plan, each with its measured reason

1. **The recipe is the register form, not the TLB-fast-path memory form** — §1.2 above.
2. **The emitter's vocabulary is the set M1 emits**, not the plan's ~30 mnemonics (§1.1): six
   listed forms do not exist yet. No stubs.
3. **The executor's steps 6–11 are emitted ONCE per block (a shared routine), not per uop.** The
   plan's artifact-1 skeleton has them per uop. Reason, measured: per-uop checks cost ~80–120 B
   each, which pushes a 16-uop block past the 4 KiB page and would have cut the compile rate
   further; the shared routine also puts the executor's ORDER in one auditable place rather than
   in N emissions.
4. **The D1 entry clear is hoisted to the prologue** (plus one after every CALLED body) instead
   of being emitted per uop, and is **not** emitted for the recipe. Argument, not measurement:
   the post-body clear runs after every uop that can set the flag, so the flag is false at every
   uop boundary — which is exactly the state the executor's per-uop entry clear establishes. A
   recipe cannot fault and nothing reads the flag between the clears.
5. **The `prefixes` store is skipped when `u->pfx == 0`** (both the entry store and the
   post-clear): by the decode precondition (`cpu->prefixes == 0` at block entry, asserted by
   `uop_decode_block`) and by the previous uop's post-clear, the value is already 0.
6. **The plan's M3 elisions are partially taken in M1** — the two above, for the one provably
   fault-free uop class (the recipe) only. `previous_ip` and `body_ip` are emitted for EVERY uop
   (the plan's step 7 would skip them too); that elision is deferred, as the plan staged it.
7. **The executed/inlined counts live in the block's stack frame, not in a register.** The plan's
   §4.1 puts the executed count in r10 — but r10 is caller-saved, so every C call clobbers it and
   it would need a spill/reload of its own; the frame slots are incremented in place
   (`add dword [rsp], 1`, 4 bytes) and are read once in the epilogue.
8. **The exit kind comes back in EAX and is counted in `uop_run`** (`jit_codegen_stats.exits[]`,
   indexed by the kind, 3 instructions per compiled block execution). This is instrumentation the
   plan does not ask for; it is what makes "the exit kinds round-trip" a measurement, in the
   suite and on a real boot.
9. **`jit_block_enter`/`exit` are called from the dispatch site in `uop_run`**, not from the
   compiled block's prologue (the wording in `jit_mem.h`). Observably equivalent (single-threaded:
   no guest write can occur between the enter and the call) and cheaper (no call in the prologue).
10. **`uop_stats.flushes` is maintained by the compiled path** (2 extra instructions per flush
    site), so the two engines' tallies stay comparable. Without it the staircase would have had
    no independent instrument at all.
11. **Substrate changes beyond the plan's "pool 256 → 2048"**: `JIT_CODE_PAGE_BYTES` named;
    `jit_abort_flag` exposed as a symbol (a compiled block cannot call `jit_abort_pending()`);
    `jit_code_clear_all()` (item §1.4's correctness requirement); `jit_block_enter`'s O(1)
    back-pointer.
12. **The `uop_cblock_t.code` field** (§1.4) — the plan's step 4 lists both options; the private
    struct field is taken because the run path must not hash twice, and `uop_t` is untouched.
13. **`uop_cache.c`'s `--jit off` arm is a NULL test on a field that is always NULL when the flag
    is off**, so the un-jitted machine pays one predictable branch per block — MEASURED as part
    of the gates: `--jit off` is byte-identical to the pre-change artifacts (§2).

---

## 6. What M1 does NOT do (and what M2 must add, in order)

**Not done, at the definitions**: no TLB fast path; no inlined address materialisation (every
memory operand goes through the `uop_materialize_addr` CALL); no lazy-flag inlining; no jcc
intra-block branches; no `movzx` **as a recipe** (it is CALLED, and its REG0 shape is in the
shape matrix); no multi-page blocks; no hotness gate; no block chaining (`jit_block_enter/exit`
are the only substrate discipline used); no macOS/Windows W^X probe beyond the substrate's.

The plan's §6 differential MATRIX is **half** built: the per-SHAPE matrix (one instruction of
every shape family, values asserted + full-state compared) is in M1; the per-RECIPE operand
matrix (registers × addresses × `flags_changed` states) is M2's, and it must be an EXTENSION of
`src/cpu/jit_codegen_selftest.c` rather than a second file. Also not covered by the suite, named
rather than implied: `UOP_SHAPE_PLAIN2` and `UOP_SHAPE_UD` (the decoder emits neither for a
program this fixture can build — PLAIN2 is the two-immediate form and UD is the shape table's
totality seat) and the `ts_guard`-PASSES path (the fault pin covers the guard failing).

**M2, in order, each with the instrument it must move:**

1. the mov/lea and movzx recipes, then the ALU family (+ the mandatory
   `uop_invalidate_page_fast` hook after every inlined store), then inc/dec, then jcc with
   intra-block branches — measuring `inlined_uops/called_uops` and **`refused_fit_uops`** after
   each step, because the second is the number that decides whether the hot blocks compile at
   all (§4);
2. `refused_fit` is the early warning: if it does not fall as recipes land, the emission must
   shrink (fixed cost, or multi-page blocks) BEFORE the coverage target can be met;
3. the TLB fast path + inlined address materialisation (the plan's §4.3) — this is where the
   remaining per-uop cost is (the profile's translate/safe_read/safe_write calls);
4. the lazy-flag inlining **with** the per-recipe operand matrix and the device-range pin, since
   a wrong flag mask is silently wrong everywhere;
5. the robustness item the rig found: **a permanently-taken abort check livelocks the machine**
   (`uop_run`'s ABORT path re-decodes, the block aborts again, forever). The shipped predicate
   cannot do that (the flag is set by an invalidation and cleared at `jit_block_exit`), but
   M2's block chaining and any new abort source should get a no-progress guard (e.g. the same
   block aborting at the same eip twice in a row → run that block with the C executor).

---

## 7. Files, hashes, and where the evidence is

```
NEW   src/cpu/jit_x64.c            455   0b1dd1e9216d4b06   src/cpu/jit_x64.h            182  cf24993482c3f843
NEW   src/cpu/jit_codegen.c        801   dea56c6e85dc2fca   src/cpu/jit_codegen.h        131  d6eff4d409cba17d
NEW   src/cpu/jit_codegen_selftest.c 1204 d2c484b67bae008a
CHG   src/cpu/jit_mem.c  091bf2341b94d78a   src/cpu/jit_mem.h  5eb04898e9c6a153
CHG   src/cpu/uop_cache.c 8144cc1bf3a29d42  src/main.c 1ca45e81b38adf58  Makefile 9c67af118ff37a21
      (sha256, first 16 -- the tree the final gate run measured; build/v86c = bb32733b9257e419 at that run)
```

Evidence (all in `results/jit-1/`): `gates-summary.txt` (the G2 table), `gates-*.txt` per gate,
`gates-jit-coverage.txt` (§3), `g1-encoder-pins.txt` + `enc.s`/`e2.s` (the byte pins' ground
truth), `mut-rig.txt` (10/10), `boot200m-*.txt` / `boot-1.5g-*.txt`, `bench-*.samples` +
`gates-bench-ratio.txt`, `gates-1.5g-serial-nondeterminism.txt` (§2). Scripts:
`results/jit-1-gates.sh`, `results/jit-1-mut-rig.py`.

Re-verification of the plan's §10 file hashes before the first edit: **`uop_cache.c/.h`,
`jit_mem.c/.h`, `cpu.c`, `cpu_state.h`, `flags.c`, `paging.c/.h` matched the plan's hashes**;
`src/main.c` and `Makefile` had **changed since planning** (`1896b45df0ba33d8` vs the plan's
`199af2da676ecaeb`, and `0cabcb92b090cbbe` vs `a8c4e4f06e71841b`) — the SPIKE landed them while
the plan was being written, exactly as the plan predicted. Nothing in the plan depended on those
two files' bytes.
