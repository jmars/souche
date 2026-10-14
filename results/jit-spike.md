# JIT-SPIKE — the ceiling measurement, the assembler decision, and the executable-memory substrate

**Unit:** `HANDOFF=v86c-jit`, unit **JIT-SPIKE** (coder). **Inputs:** `/tmp/brief-v86c-jit-spike.txt`,
memory node `handoff-v86c-jit-ctx`, `results/perf-2-blockcache-plan.md` (the cache's design of record),
`results/perf-4-uop-exec.md`, `results/perf-fix-3.md` (the invalidation hooks as built),
`results/perf-5-tune.md` (the measurement method), `results/m1a4-emitter-runloop.md`,
`results/perf-3-uop-decode.md`.

**This is a SPIKE, not the JIT.** It answers three questions with measurements and builds only the
design-independent half of the substrate. Claims are tagged **MEASURED** (command + raw output in
`results/jit-spike/`), **READ** (from source, with file:line) or **INTERPRETATION**.

---

## 0. Bottom line

| question | answer |
| --- | --- |
| **Q1 what is the ceiling** | On the boot's own hot shape, a hand-written native block runs the same work **~80× faster per guest instruction** than the shipped uop cache (~54× on the median round of a host at load 26); the pure-ALU shape **~144×**; with the page base provably folded, **~196×**. **BUT the measurement also kills the naive reading of it: the emulator's cost is PER GUEST INSTRUCTION, not per block — the same mix in a 12-uop block costs the same per instruction as in a 6-uop block (14.20 vs 13.72 ns/GI, i.e. within the host's noise). A JIT that removes only the block entry/lookup therefore buys ≈0, which is this unit's measurement of the ctx's prior finding rather than a restatement of it.** What a JIT must remove to collect the 80× is the per-operand call chain (`uop_materialize_addr` 75.5M calls, `get_seg` 78.2M, `translate_address` 40.9M on the boot) — i.e. **inlining the semantics**, exactly as memory says. |
| **Q2 which feeder** | **OUR OWN x64 ENCODER.** Both routes emit a 27-byte block that is **byte-for-byte identical** MEASURED, so DynASM buys no output quality; DynASM costs a vendored Lua interpreter (7,771 lines of third-party C + 125 KB of Lua/C) at build time and +4 KB of `.text` in the APE, against 304 lines of our own C and no build-time tool. |
| **Q3 the substrate** | `src/cpu/jit_mem.{c,h}` behind `--jit on\|off` (default OFF), wired to the **PERF-FIX-3 invalidation hooks through the uop cache's own entry point** (no second mechanism), with a selftest whose stale-block probe is **shown able to fail in 5/5 mutation arms**. All existing gates green in both cache modes with unchanged counts; `--jit off` reproduces the **pre-change binary's transcript byte for byte**. |

**The honest ceiling is a RANGE with a stated floor**: 54×–80× (memory shape), 142× (ALU shape),
180×–196× (folded page). §2.4 lists what the native arm removes, what it keeps, and which way each
tilts the ratio.

---

## 1. The shapes, and where they come from

The workload is the **boot the whole port already gates on** — `build/v86c --bzimage
tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0"`, the Alpine 6.6.134 i386 kernel,
200,095,214 guest instructions (`tests/perf_bench.sh`). Its per-instruction census is a **MEASURED**
profile of a `-pg` build of this port (`results/perf-5/gprof-flat-after.txt`; call counts are the
usable half of `-pg` on this host, self-times are distorted ~7× — `results/perf-fix-3.md`):

| call count | symbol | guest instruction |
| --- | --- | --- |
| 75,544,980 | `uop_materialize_addr` | (the executor's per-memory-operand call) |
| 40,855,294 | `translate_address` | (the per-operand page translation) |
| 33,064,149 | `safe_read32s` | the load helper |
| 25,325,695 | `instr32_8B_mem` | `mov r32, [mem]` |
| 22,221,684 | `translate_address_write_and_can_skip_dirty` | (write translation) |
| 11,447,324 | `safe_write32` | the store helper |
| 11,381,894 | `instr32_89_mem` | `mov [mem], r32` |
| 9,268,734 | `instr32_0FB6_mem` | `movzx r32, byte [mem]` |
| 7,978,672 | `instr32_29_reg` | `sub r32, r32` |
| 6,848,377 | `instr32_01_reg` | `add r32, r32` |
| 5,848,229 | `instr32_21_reg` | `and r32, r32` |
| 5,823,606 | `instr32_D3_5_reg` | `shr r32, cl` |
| 5,397,129 | `instr32_74` | `jz rel8` |
| 3,314,196 | `instr32_77` | `ja rel8` |
| 2,688,214 | `instr32_4A` | `dec edx` |
| 2,632,892 | `instr_F6_0_reg` | `test r/m8, r8` |

**READ, against the brief's brief:** the brief names `instr32_73`/`instr32_74` as the hot jcc's. The
record says `74` at 5.40M and `77` at 3.31M, and **`73` is not in the profile's top 40** — the shapes
below use `74`/`75`-class jcc's, which is what the boot actually spends.

The two shapes are built **out of the instructions the profile actually ranks highest**, so the
measurement is about the boot's real mix and not about an invented loop:

* **shape M (memory-carrying)** — 6 guest instructions/iteration, **2 read memory operands**
  (`mov r32,[mem]` + `movzx r32,byte [mem+4]`), an ALU pair, `dec`+`jnz`. 33% memory operands.
* **shape M2** — the same mix in a **12-instruction block** (only to measure the block-entry term).
* **shape A (pure ALU)** — the same loop control with **no memory operand at all**.

---

## 2. Q1 — the ceiling

### 2.1 How it was measured, and why that way

`results/jit-spike/ceiling.c` links the **real core** (per-TU, exactly as
`src/cpu/uop_exec_selftest.c` does) and the **real uop cache**, loads the shape into guest RAM under
the identity-mapped fixture, and drives it with `cycle_internal` — the machine's own entry point,
with `uop_cache_enabled = true` (the shipped default) and `false` beside it for context. In the same
process and interleaved round-by-round it runs the **hand-written native block** for the same shape
(`results/jit-spike/x64enc.c` emits it; it is a byte stream in an `mmap`'d page, `mprotect`'d RX).
Both are normalised to **ns per guest instruction**, so an arm may repeat the block to make its
window long enough to measure.

Three things make the numbers trustworthy rather than merely large:

1. **The harness's own correctness gates.** Every native block's result is compared with a C
   re-implementation **and** the emulator's own run of the same bytes. That is not ceremony: the
   first two versions of this harness were wrong and these checks caught both — (a) the native
   blocks used the host's **callee-saved RBX** for the guest's EBX without saving it, silently
   corrupting the C caller (`want` was 0 where the block had already run it); (b) shape M's
   `01 D8` is `add eax, ebx`, not `add ebx, eax` (**01 /r takes r/m32 as the destination**), which
   the emulator-vs-reference check caught. Both are now inline comments in the harness.
2. **An independent cross-check of the "current path" arm**: its best round here is **13.7 ns/GI
   (72.9 MIPS)**, and the project's own benchmark — 200M instructions of the real kernel, `make bench`,
   this unit's G4 runs — reports **71.2 MIPS best** (first run) and **68.6 MIPS best** (final run, on
   the same tree as the rows above), against PERF-5's recorded baseline of 70.5 MIPS. The harness
   measures the shipped path, not a lookalike.
3. **A note in the record** of the host's load (24–26 across all runs) and both the best and the
   median of nine interleaved rounds. The ceiling is quoted as a range because the two differ.

### 2.2 The measurement (**MEASURED**, `results/jit-spike/g1-ceiling.txt`)

1,000,000 iterations per round, 9 rounds, arms interleaved inside every round; native arms repeat the
block 512× (3.07 × 10⁹ guest instructions) so no arm's window is short enough for the host to fake a
number; host load 26.18/24.59/22.40 at the time of the run.

| arm | guest instr | best ms | **best ns/GI** | med ns/GI | best MIPS |
| --- | --- | --- | --- | --- | --- |
| M emulator (uop cache on) | 6,000,001 | 82.3 | **13.722** | 17.458 | 72.9 |
| M emulator (cache off) | 6,000,001 | 121.0 | 20.163 | 24.927 | 49.6 |
| M2 emulator (12-uop block) | 12,000,001 | 170.3 | **14.195** | 18.699 | 70.5 |
| M native inline-TLB | 3,072,000,000 | 524.3 | **0.171** | 0.323 | 5,859 |
| M native folded-page | 3,072,000,000 | 215.4 | **0.070** | 0.097 | 14,259 |
| A emulator (uop cache on) | 5,000,001 | 53.9 | **10.773** | 14.160 | 92.8 |
| A emulator (cache off) | 5,000,001 | 73.4 | 14.688 | 18.477 | 68.1 |
| A native | 2,560,000,000 | 191.3 | **0.075** | 0.100 | 13,382 |

**RATIOS** (per guest instruction; the native arms repeat the block, so only the normalised ratio means
anything):

| | from BEST round | from MEDIAN round |
| --- | --- | --- |
| shape M — emulator / native **inline-TLB** | **80.4×** | 54.1× |
| shape M — emulator / native **folded page** | **195.7×** | 180.9× |
| shape M — emulator / native, **cache OFF** denominator too | 117.9× | 76.7× |
| shape A — emulator / native | **144.2×** | 142.2× |

and, as context inside the same process: **the uop cache is worth 1.43× (M) / 1.30× (A) over the
interpreter** on these shapes, with `--uop-cache-stats`-equivalent rates of **100.00% hit, 0.000%
fallback** (`lookups 29400000 (hits 29399969)`, `decodes 31`) — i.e. the shape is entirely
cache-resident and the cache is not the thing being measured.

**The spread is the host, and it is stated rather than averaged away.** The emulator arms' best and
median differ by ~27%; the short native arms' by ~90%. At load 26 on 32 cores a 0.1–0.5 s window gets
descheduled, and a 4-wide native loop loses more to an SMT sibling than a call-bound interpreter does.
The best-vs-median ratio spread (80× vs 54×) is therefore a *host* uncertainty band, not a property of
either arm; the honest statement is **"≈54–80× on this host under this load"**, with the ALU shape's
142–144× being the tightest number in the table.

### 2.3 THE FINDING THAT MATTERS MORE THAN THE RATIO: the block entry is ~free

The M2 arm is shape M's mix with **twice the block length** (12 uops instead of 6). Its cost per guest
instruction is **14.20 ns/GI vs 13.72** — i.e. **the same, inside the noise** (and M2 is *slightly
richer* in memory operands per instruction: 5/12 vs 2/6, so the direction of the residual is not even
the one that would favour the claim).

**INTERPRETATION, and this is the unit's most useful result for the planner:** the uop cache's
per-block work (`uop_run`'s entry, the hash lookup, the block setup, the state-key computation)
already costs ≈ nothing per instruction on a warm block, and `--uop-cache-stats` agrees it pays once
per 13.1 instructions. So **a JIT that compiles the guest's dispatch — one native block per uop, each
native instruction a call or a jump into the existing C bodies — removes the part that is already
cheap and leaves the part that is not: the per-operand call chain** (`uop_materialize_addr` 75.5M,
`get_seg` 78.2M, `translate_address` 40.9M calls per 200M instructions; `results/perf-5-tune.md` §2
established the calls are real in the shipped object). That is the prior finding in the memory graph
("dispatch-only JIT buys nothing"), now measured **on this tree, on this shape**: ≈3% is what the block
entry is worth. The 80× requires inlining the semantics.

### 2.4 What the native arm removes, what it keeps, and how the ratio OVERSTATES a real JIT

**REMOVES (all of it real, and all of it what a full trace-specialising JIT removes):**

| # | removed | evidence it is real here |
| --- | --- | --- |
| 1 | the per-instruction dispatch (opcode re-fetch + table dispatch) | `interpreted_run`'s loop body |
| 2 | the per-block lookup and executor loop (`uop_run`, the hash probe, the uop walk) | the emulator arm's 100%-hit path |
| 3 | **every per-operand call**: `read_reg32`, `resolve_modrm`, `uop_materialize_addr`, `get_seg` | the profile's top three call counts |
| 4 | the **`translate_address` call per memory operand** — the native arm *inlines* the TLB read | 40.9M calls on the boot |
| 5 | the call/return pair around each instruction body | `read_imm8` alone is 337M calls |
| 6 | the per-instruction `instruction_counter` update | the native block never touches it |
| 7 | the reference-batch staircase bookkeeping (`uop_pending`, flush points) | §2.3's M2 arm |
| 8 | the lazy-flag machinery of the ALU bodies | the host's own 32-bit flags ARE the guest's |

**KEEPS (so the ratio is still a ceiling and not a fiction):** the same guest RAM through the same
`mem8` bytes (the inline-TLB arm really reads the page table it builds; a miss bails out of the block
with a partial result, and the harness's reference check would catch a spurious miss as a short sum);
the same register values; the same loop shape with the back edge inside the block; and the host's own
flags as the guest's (valid for 32-bit x86→x86-64 forms, which is what the shapes use).

**HOW IT OVERSTATES — the JIT-specific costs the block does NOT pay (each would be emitted per
iteration/back edge by a real JIT, so each *reduces* the real win):**

1. **The back edge has no check at all.** A real JIT's back edge must re-check the cache/state key
   (upstream v86 re-checks `tlb_code`/`TLB_HAS_CODE` and the state flags, cpu.rs:3137-3245), the
   SMC dirtiness bit, and the instruction budget — my loop carries one `dec`/`jnz` and nothing else.
2. **No instruction-counter staircase.** The counter is guest-observable (`rdtsc` → `microtick` →
   `instruction_counter/1000`, `exec_boundary.c`), and `results/perf-4-uop-exec.md`'s staircase pin
   exists precisely because the reference's batch structure is observable MID-BATCH. A real block
   must accumulate the count and flush it exactly where the reference does.
3. **No guards or bail-outs**: no `task_switch_test`/TS guard, no materialisation-fault arm, no exit
   reason, no re-entry. A miss here is one `ret`; a real JIT re-enters the block loop and the C fallback.
4. **No SMC/dirty-page discipline at all** in the measured path: nothing is invalidated, nothing is
   written. (The substrate half of this unit is where that cost lives — §4.)
5. **No segment basework**: the fixture is identity-mapped with base 0, and the native arm never adds
   a segment base (a real JIT either relies on the flat-segment state bit, which it must check, or adds it).
6. **The shapes are pure**: no faults, no page crossings, no `sti`, no device access, no branch into
   un-compiled code. The boot is not.
7. **The memory-operand count is not the boot's**: shape M is 33% memory operands; the boot's
   `8B_mem`+`0FB6_mem`+`89_mem` are ~23% of instructions. A shape richer in the expensive class
   measures a bigger per-instruction cost than the boot's average (13.7 vs the boot's 14.0 ns/GI is a
   coincidence of this pairing; the **cache-off** arm is the honest ratio's floor).

**HOW IT UNDERSTATES:** the folded arm (0.070 ns/GI) shows what a block could do with the page base
provably constant — an optimisation a real block compiler might find and my harness only asserts;
dead-flag and dead-store elimination are in the same class and are not attempted at all.

**AND WHAT THE 80× DOES NOT LICENCE.** It is a per-shape ceiling on a **compiled loop**, not a
whole-boot speed-up. The boot's 200M instructions include `rep movs`/`rep stos` bulk paths, device
work, interrupt entry, context switches and page-table walks — code a JIT either refuses to compile or
compiles badly — and the ceiling for a program is bounded by the fraction of its time in shapes like
these times the per-shape ratio, minus the fallback/exit costs above. The naive multiplication
(80 × 200M instructions) is exactly the projection this unit's brief warns about, and §2.3's 3%
per-block finding is why the projection collapses if the JIT does not inline.

---

## 3. Q2 — DynASM or our own x64 encoder

### 3.1 The prototype: one block, two routes (**MEASURED**, `results/jit-spike/g2-feeder.txt`)

The SAME shape-A block (guest EAX/EBX/ECX/EDI/ESI in host registers, the back edge inside the block,
the host's callee-saved RBX saved and restored, `n` arriving in EDI, the seed in ESI) was built both
ways and **executed**:

* `results/jit-spike/dynasm-shape.dasc` → `minilua dynasm.lua -c -o dynasm-shape.h dynasm-shape.dasc`
  → `results/jit-spike/dynasm-run.c` (the generated header + `dasm_proto.h` + `dasm_x86.h`);
* `results/jit-spike/enc-run.c` (our `x64enc.h`/`x64enc.c`, 304 lines total).

```
dynasm:  block is 27 bytes      53 41 89 f8 31 c0 31 db 89 f1 31 ff 01 d8 29 d9 31 f7 41 ff c8 75 f5 89 f8 5b c3
encoder: block is 27 bytes      53 41 89 f8 31 c0 31 db 89 f1 31 ff 01 d8 29 d9 31 f7 41 ff c8 75 f5 89 f8 5b c3
IDENTICAL: the two routes' full 27-byte streams are byte-for-byte equal
dynasm:  n=1..6 -> 0x12345/0x0 alternating, 6/6 MATCH      encoder: 6/6 MATCH
```

**The two routes produce identical code for this block**, so the decision cannot be made on output
quality — it is entirely a question of cost and of the runtime API.

### 3.2 Cost, measured and read

| | DynASM | our encoder |
| --- | --- | --- |
| source for the block | 15 `\|` lines (`.dasc`) | 15 C calls |
| **new third-party to vendor** | `minilua.c` **7,771 lines** + `dynasm.lua` 31,016 B + `dasm_x86.lua` 75,655 B + `dasm_proto.h` 2,062 B + `dasm_x86.h` 16,503 B = **125,236 B of Lua/C** | — |
| **build-time tool** | a Lua interpreter, built from vendored C: **3.8 s**, 960 KB APE (**MEASURED**); the DynASM step itself **7 ms** | none |
| build integration | 2 more Makefile rules (minilua, the generated header) + a generated-file prerequisite in every link that uses it | a normal TU, already in `$(CPU_SRC)` |
| **bytes in the APE** | the driver's `.text` **233,472 B** vs the encoder driver's **229,376 B** → **+4,096 B** for the `dasm_x86.h` runtime encoder (**MEASURED**, `size` on the two `.com.dbg` ELFs) | — |
| runtime API | the action list is fixed per `dasm_put` call site (READ: the generated `dynasm-shape.h:33` is a single `dasm_put(Dst, 0)` for the block) — dynamic instruction selection means **C conditions around pre-encoded streams**, i.e. every runtime variant is a stream chosen at build time | every instruction is chosen at runtime |
| ergonomics | the DSL is genuinely nicer to read (one line per instruction; labels `\|1:`/`<1` and branches handled; REX/modrm/SIB resolved for you) | explicit, verbose, and the caller must get modrm direction and REX right — the two bugs this harness itself made (§2.1) were exactly that class |
| failure mode found | `dasm_setup` memsets `lglabels`, which is NULL unless `dasm_setupglobal` ran first — **it faults under cosmocc**; not discoverable from the docs | our own bugs are our own |

`minilua` **does** build and run under cosmocc (3.8 s, one APE), and its output is byte-identical to
the host-built minilua's — so DynASM **fits** the one-toolchain build; the question is whether it is
worth what it adds.

### 3.3 Recommendation: **our own encoder**, and the conditions under which that reverses

**Recommend our own x64 encoder.** Evidence, in weight order:

1. **It buys nothing in output.** Same 27 bytes, byte-for-byte, for the block a JIT needs first.
2. **The project's posture — one toolchain, no new dependencies — is worth more than the ergonomics.**
   DynASM needs a Lua interpreter at build time (7,771 lines of vendored C) plus 125 KB of vendored
   Lua/C. This project already carries one build-time interpreter (`node`, for
   `gen/interpreter_c.js`); adding a second, for a code generator whose output we can produce
   ourselves in 304 lines, is a dependency the brief explicitly asks to weigh — and it weighs badly.
3. **The runtime API is the real argument.** A block compiler's defining act is choosing instructions
   at RUNTIME from the guest's bytes: which operand form, whether a flag computation is dead, which
   registers are free, whether a page base can be folded, when to bail. DynASM's model is
   build-time instruction selection with runtime operands (and runtime label/relocation resolution);
   a runtime decision becomes a C `if` around several pre-encoded streams, each of which costs action
   list bytes. That is fine for LuaJIT's hand-written, per-architecture bytecode translators — where
   the instruction sequence for each bytecode IS known at build time and the backend is a
   human-authored artefact of ~55 KB of `.lua` per architecture — and it is the wrong shape for a
   translator written as data-driven C that emits whatever the guest's bytes call for.
4. **The APE grows by 4 KB** for the encoder engine; ours is compiled in anyway, and our vocabulary
   is the vocabulary (mov/add/sub/cmp/jmp/jcc/call/ret + addressing) rather than x86's.

**What would reverse it** (state the condition, so the decision can be revisited on evidence rather
than re-argued): if the JIT acquires a **large** instruction vocabulary (SSE/MMX forms) or a **second
backend**, DynASM's per-architecture `.lua` (already vendored, already proven against a real JIT)
would probably be cheaper than hand-writing two encoders; and if the encoder stops being ~300 lines
and starts being ~3,000, its maintenance cost flips the comparison. Neither is true for the
vocabulary the two blocks here needed, and both are measurable later.

**NO SILENT STUB:** the rejected route is not deleted. `dynasm-shape.dasc` + `dynasm-run.c` remain in
`results/jit-spike/` and are buildable the moment someone wants them; `results/jit-spike/spike-build.sh`
documents their two-step build.

---

## 4. Q3 — the substrate in the tree

### 4.1 What was built

| file | what it is |
| --- | --- |
| `src/cpu/jit_mem.h` / `src/cpu/jit_mem.c` (158 + 483 lines) | THE EXECUTABLE-MEMORY LAYER: a W^X code-page pool (`mmap` RW → write → `mprotect` RX, and the reverse for recycling), a compiled-block table keyed **the same way uop blocks are keyed** (phys, virt, state — the same mixing function, `jit_hash_pos` is `uop_cache.c`'s `uop_hash_pos`), the invalidation entry points, and the executing-block discipline (abort flag + deferred recycle). |
| `src/cpu/jit_mem.h` (the aarch64 posture) | `V86C_JIT_X64` is 0 on any non-x86-64 target: the whole layer compiles out to a flag plus no-op entry points, and `--jit on` says so instead of failing. **MEASURED**: `aarch64-linux-cosmo-cc` compiles `jit_mem.c`, `uop_cache.c` and `jit_selftest.c` clean under `-Werror` (`results/jit-spike/gates-summary.txt`). |
| `src/cpu/jit_selftest.c` (442 lines) | THE GATE (`make jit-selftest`, and part of `make selftest`): the block runs, the **stale-block probe**, the pool's refusals, and the deferred recycle. |
| `results/jit-spike-mut-rig.sh` | the probe, **shown able to fail**: 5 mutation arms, 5/5 caught, plus a control and a restoration arm. |
| `src/cpu/uop_cache.c` (+9 lines) | the **single** invalidation mechanism: `uop_invalidate_page` forwards to `jit_code_invalidate_page` *before* its per-page early return, and `uop_invalidate_range` tests the shared bitmap instead of the uop registry. |
| `src/main.c` | `--jit on\|off` (default OFF) + `--jit-stats`; the init is deferred to after parsing so `--jit on --uop-cache off` is refused with a reason rather than raced. |
| `Makefile` | `jit_mem.c` joins `$(CPU_SRC)` (hence the shipped unity TU); `$(JIT_OBJ)` for the one link that takes `uop_cache.c` as a separate object; the `jit-selftest` target. |

**THE W^X PLATFORM CAVEATS ARE AT THE DEFINITION** (`jit_mem.h:` the comment block), not implied:
Linux VERIFIED (the selftest really publishes and executes a page here); **macOS/MAP_JIT and Apple
Silicon UNVERIFIED** (no `pthread_jit_write_protect_np`, no macOS host to test on — and an APE is one
binary for all three, so the untested platform is exactly the one a claim would be wrong about);
**Windows UNVERIFIED**; and the i-cache-coherence argument used to justify "no flush after publishing"
is stated as an **x86-64 property**, not a portability claim (aarch64 would need `__clear_cache`, and
that is why there is no aarch64 backend).

### 4.2 The invalidation: the SAME hooks, not a second mechanism

The property a JIT lives or dies by — *a code page written by the guest while its block is cached must
not be executed stale* — is served by the machinery PERF-FIX-3 already restored, reused rather than
duplicated:

```
guest write ──► the PERF-FIX-3 hook (memory.c/paging.c/string.c, uop_invalidate_page_fast)
                └─► uop_invalidate_page            (the ONE invalidation entry point)
                     ├─ the SMC abort for the executing DECODED block
                     ├─► jit_code_invalidate_page  (JIT-SPIKE: the same hook, forwarded)
                     └─ the uop registry unlink
```

Two deliberate design points, both load-bearing:

* **The shared dirty-page bitmap.** `jit_cache_bind` sets `uop_page_code[page]` — the uop cache's own
  bitmap — for the page a compiled block covers, so the write sites' inlined test
  (`uop_invalidate_page_fast`) takes the slow arm for a page that holds **only** a JIT block. Without
  this the JIT would be invisible to the hooks, and the mutation rig proves it: arm 4 removes that one
  line and the stale-block probe fails.
* **The forward call is before the early return**, because a page can hold a compiled block and no
  decoded block. Arm 1 removes it and the probe fails.

**Constraint, stated because it is real**: the JIT rides the uop cache's structures, so `--jit on`
requires the uop cache (`jit_mem_init` calls `uop_cache_init` itself and refuses `--uop-cache off`
with a reason). A JIT design that wanted independence from the uop cache would have to own its own
per-page bitmap and its own write-site test; that is a decision for the design unit, and it is written
down here rather than assumed away.

The **substrate does not own an encoder or a block compiler** — deliberately: that is Q2's decision,
and the layer is the part any design needs (`jit_mem.h`'s header says so). It also does not compile a
guest instruction; the selftest's "compiler" is a 17-byte 1:1 translation, enough to exercise the
cache/invalidation substrate without prejudging the compiler.

### 4.3 The selftest, and the probe shown ABLE TO FAIL

`make jit-selftest` → **29 checks, 0 failures** (`results/jit-spike/g3-jit-selftest.txt`):

1. **The block runs**: a 15-byte hand-written `sum` block is reserved (RW), published (RX) and
   **called**; its results are checked against a C loop (8/1/16 iterations). Without this the rest is
   theatre. (First version's own bug, kept as a comment: the block is `dec`/`jnz`, i.e. a do-while, so
   `n=0` runs 2³² iterations — MEASURED as a SIGSEGV before the case was removed.)
2. **The stale-block probe** — the fixture is `mov dword [0x3000], imm32` on a guest code page, a
   compiler that resolves the immediate into a native block, and a *patcher* the MACHINE actually
   runs (`mov dword [0x1006], 0x22`). The write therefore travels the real write path
   (`instr_C7_0_mem` → `write32` → the PERF-FIX-3 hook → `uop_invalidate_page` →
   `jit_code_invalidate_page`), which is the only way this test can catch a wiring mistake. It then
   asserts, **in the executor's own flow** (look up; compile only on a miss): the cached block is
   gone, and the re-run writes the NEW immediate. A stale execution would write `0x11`.
3. **The pool's refusals are loud**: publishing a foreign pointer, publishing twice, and exhausting
   the pool are each refused and counted (the pool is exhausted on purpose) — no silent stubs.
4. **The executing page's recycle is deferred**: invalidating the page of the block that is currently
   executing must not flip it to RW under it, so the drop is deferred to `jit_block_exit` while the
   abort is set — and the page is reused afterwards.

`results/jit-spike-mut-rig.sh` (**MEASURED**, `results/jit-spike/mut-rig.txt`):

| arm | mutation | verdict |
| --- | --- | --- |
| 0 | none (control) | PASS (exit 0) |
| 1 | the forward call in `uop_invalidate_page` removed | **CAUGHT** (exit 1; `invalidations` 0, lookup hit) |
| 2 | `jit_block_drop` left the block live in the hash | **CAUGHT** (exit 1 — and as a SIGSEGV: the stale block's page had already been recycled to RW, so executing it faults) |
| 3 | the guest-page registry link removed (**this unit's own shipped bug**, found by the probe) | **CAUGHT** (exit 1; `ram32(DATA_ADDR) 0x11, want 0x22`) |
| 4 | the shared dirty-page bit not set | **CAUGHT** (exit 1; stale 0x11 executed) |
| 5 | the executing page's recycle not deferred | **CAUGHT** (exit 1) |
| R | restored tree, rebuilt | PASS (exit 0) — no mutation left behind |

Arms 3 and 4 fail with **"got 0x11, want 0x22"**: the probe demonstrably catches *stale execution*,
not merely a missing counter. (The first version of the probe asserted only that the lookup missed and
then recompiled unconditionally; arm 2 was caught by the counter alone. The executor-shaped flow is
what turned it into a value check.)

---

## 5. G4 — the existing gates, both cache modes, counts unchanged

**MEASURED**, `results/jit-spike/gates-summary.txt` + the raw logs beside it. All commands are
PERF-5's (`results/perf-5-gates.sh`) so the counts are comparable.

**Every row below is from the FINAL tree** (after both fixes in this section), one run of
`results/jit-spike-gates.sh`, load 23.9/24.0/22.8 — `results/jit-spike/gates-summary.txt`:

| gate | rc | evidence |
| --- | --- | --- |
| `make` | 0 | `build/v86c` sha256 `cd931687b8b0c5d55d0dbfe2433afafa53f35736874df7ff3fa52947c862fc43` |
| `make selftest` | **0** | 3522 / 228 / 541 / 7,934,349 / 102 checks, **0 failures** — **every pre-existing count unchanged** (the ctx's own list) — plus the new `jit-selftest: 29 checks, 0 failures` |
| `make nasm` (cache on) | 0 | 0 of 123 produced no state, 5 skipped (`tests/nasm_skip_c.txt`), the 118/118 differential |
| nasm `--uop-cache off` | 0 | same |
| `make transcript-diff` (cache on) | 0 | byte-identical to the JS reference |
| transcript-diff `--uop-cache off` | 0 | byte-identical |
| **`--jit off` (no flag) vs the PRE-CHANGE BINARY** | 0 | `cmp` against `/tmp/jitspike/v86c-baseline` (sha256 `fe6e021d…`, copied **before** the first edit): **byte-identical M0 transcript** |
| **`--jit off` (explicit) vs the PRE-CHANGE BINARY** | 0 | byte-identical |
| `--jit off` vs no flag, **on stderr** | 0 | identical streams (the flag prints nothing) |
| **`--jit on` reports its caps** | 0 | `v86c: jit: x86-64 W^X code pages: … Linux VERIFIED (this build); macOS … UNVERIFIED …` |
| `--uop-cache off --jit on` refused **with the reason** | 0 | `jit: --jit needs the uop cache's invalidation hooks; --uop-cache off was also given. JIT off.` |
| 1.5 G boot, cache on / off / **`--jit on`** | 0,0,0 | final state IDENTICAL in all three: `in_hlt=0, exception=-1, unimplemented=0, eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000` |
| `make bench` | 0 | best 2.915 s (`68.64 MIPS`), median 3.621 s; every run verified `200095214` instructions, terminal state ok, serial sha ok (PERF-5's baseline on this workload: 2.839 s / 70.5 MIPS) |
| aarch64 compile of `jit_mem.c` / `uop_cache.c` / `jit_selftest.c` | 0,0,0 | `aarch64-linux-cosmo-cc … -Werror` |

### 5.1 TWO of this unit's own defects, both caught by running the gates rather than asserting them

**(a) `make selftest` rc=2** on the first pass: `cpu_selftest` takes `uop_cache.c` as a **separate
object** (`$(UOP_C_OBJ)`) rather than through `$(CPU_SRC)`, so adding `jit_mem.c` to `$(CPU_SRC)` left
that one link with an undefined `jit_code_invalidate_page`. Fixed with a new `$(JIT_OBJ)` variable on
that rule. **The shipped binary was unchanged by that fix** (same sha256 before and after), so it did
not invalidate any other gate's evidence. This is the same class the PERF-4/M2 Makefile note warns
about ("it would be both a source and an object").

**(b) `--jit on` WAS A NO-OP EVERYWHERE, and only the flag's own smoke test found it.** The init was
placed where the arguments are parsed — before the guest's memory exists — and `jit_mem_init` calls
`uop_cache_init`, which sizes the shared dirty-page bitmap from `mem8_alloc_size()`. Every
`--jit on` therefore failed with *"the uop cache could not be initialized"*, in both modes, and the
1.5 G boot's `--jit on` arm was testing a flag that did nothing. It is fixed by deferring the init to
just after `machine_init()` in each mode (the ordering problem the deferral was for — `--jit on
--uop-cache off` in either order — is still solved, and the refusal now prints its real reason).
**This is exactly the failure mode the substrate's own header warns about** ("a seat that is never
called is indistinguishable from an absent seat"), and it is why §5's smoke rows exist: three of them
now check that the flag reaches the layer, that the refusal names its cause, and that `--jit off`
prints nothing at all. The re-run above is the final tree with both fixes.

**"`--jit off` reproduces today's bytes exactly"** is read here as the **guest-observable bytes**
(the M0 transcript, the boot's final state, the serial transcript's sha256) and not as the APE's own
bytes — the APE necessarily differs once a flag and a layer are compiled into it, and the byte-level
claim that *is* checkable and stronger is the one above: the same input produces the same output as
the binary built before the change. Stated so the reading is on the record.

**"`--jit off` reproduces today's bytes exactly"** is read here as the **guest-observable bytes**
(the M0 transcript, the boot's final state, the serial transcript's sha256) and not as the APE's own
bytes — the APE necessarily differs once a flag and a layer are compiled into it, and the byte-level
claim that *is* checkable and stronger is the one above: the same input produces the same output as
the binary built before the change. Stated so the reading is on the record.

---

## 6. What this does NOT tell us

1. **It is not a projection of a boot speed-up.** §2.4's last paragraph: the ratios are per-shape
   ceilings on compiled loops. The fraction of the boot that is compilable, the exit/fallback cost,
   the counter staircase and the guards are all *unmeasured* here, and they are the terms that decide
   a real JIT's number.
2. **Nothing about the compiler.** No guest instruction was compiled by anything but the selftest's
   17-byte 1:1 translation. Register allocation, flag liveness, address folding, block chaining, the
   decision of what NOT to compile — all untouched.
3. **Nothing about the counter/flush staircase under a JIT** — the single hardest correctness property
   in this tree (`results/perf-4-uop-exec.md`'s staircase pin). The native arms do not implement it,
   which is why they are a ceiling.
4. **No macOS/Windows/aarch64 verification of the executable-memory path.** Stated at the definition;
   it is the one part of this unit that cannot be verified on this host.
5. **The invalidation substrate is wired, not exercised at scale**: the selftest's probe is a
   single-page, single-block, deterministic scenario. Whether the JIT's cache behaves under the boot's
   308 invalidations/200M instructions and 257k reference batches is a JIT question, not a substrate one.
6. **The pool's sizing is a spike's guess** (256 one-block pages, 1 MiB). Upstream's JIT packs blocks;
   the real design should size from a measurement like `uop_stats.arena_peak` did for the uop cache.
7. **The two shapes are two shapes.** The boot's mix is richer (stores, shifts, string ops, FPU/SSE,
   interrupts); a shape with stores would have exercised the *write* translation path
   (`translate_address_write_and_can_skip_dirty`, 22.2M calls) which shape M never touches.
8. **The host's load (24–26) is in every number**, and the best-vs-median spread is quoted rather than
   reduced; a quiet host would tighten the ratio, not change the ordering.

---

## 7. Raw evidence, file by file

| file | what it holds |
| --- | --- |
| `results/jit-spike/g1-ceiling.txt` | the Q1 run: the byte dumps of all three native blocks, the correctness checks, the 8-arm table with best/median, the ratios, the host load, the uop-cache rates |
| `results/jit-spike/g2-feeder.txt` | the Q2 run: both routes' byte streams and the diff, the APE sizes (`.text` 233,472 vs 229,376), the vendored-file sizes, minilua's cosmocc build time and the generated-header `cmp` |
| `results/jit-spike/g3-jit-selftest.txt` | `make jit-selftest`'s own output (29 checks, 0 failures) |
| `results/jit-spike/mut-rig.txt` + `run.[1-5|control|restored].txt` | the 5 mutation arms, the control and the restoration arm, with each arm's FAIL lines |
| `results/jit-spike/gates-summary.txt` + `gates-*.txt` | the G4 runs (make, selftest + the rerun, nasm both modes, transcript-diff both modes, the byte-identity `cmp`s against the pre-change binary, the three 1.5 G boots, bench, the aarch64 compiles) |
| `results/jit-spike/ceiling.c`, `x64enc.{c,h}`, `enc-run.c` | the Q1 harness and the Q2(b) encoder prototype |
| `results/jit-spike/dynasm-shape.dasc`, `dynasm-shape.h`, `dynasm-run.c` | the Q2(a) DynASM prototype (the generated header is checked in so the comparison can be read without building minilua) |
| `results/jit-spike/spike-build.sh` | how the two measurement binaries are built (they are NOT part of `make`) |
| `results/jit-spike-gates.sh`, `results/jit-spike-mut-rig.sh` | the two rigs |
