# M1a-4 — the C emitter, the run loop, the loader: the first executable C emulator

**Unit:** `HANDOFF=v86c`, unit M1a-4 (coder). **Deliverables:** `gen/interpreter_c.js` (the C
emitter over `v86/gen/x86_table.js`), `src/cpu/interpreter_gen.c` (its output),
`src/cpu/cpu.{h,c}` (the run loop), `src/cpu/exec_boundary.{h,c}` (the M1c/M2 seat),
`src/cpu/instructions_0f.{h,c}` (the first 0F slice), `src/main.c` (the loader and the two
output documents), and the `Makefile` wiring. `v86/` read-only; `tests/nasm_run.py` touched
in exactly two lines (§10.3).

**Bottom line — THE MILESTONE IS GREEN.** `diff <(node oracle/run.mjs tests/test.img
1000000) <(build/v86c tests/test.img 1000000)` is **EMPTY**: the C binary's transcript is
byte-identical to the JS reference's, from a from-scratch build, including the 561-instruction
count and the 4 KiB window's sha256. `make nasm` runs its C leg via `--actual` → **exit 0,
21 passed / 0 failed / 102 skipped**, every skip named with the symbol the binary itself
printed. `make selftest` (2968 checks, 0 failures) and `make oracle` still exit 0. The
generator is reproducible (run twice → empty diff) and is a real `make` prerequisite.

Raw evidence for every command, with exit codes: **`results/m1a4-gates.txt`**.

---

## 1. What changed (every file)

| File | Lines | What it is |
| --- | --- | --- |
| `gen/interpreter_c.js` | 508 | the C emitter: `v86/gen/x86_table.js` → `src/cpu/interpreter_gen.c`. Mirrors `v86/gen/generate_interpreter.js` (497 lines) function for function (§2). |
| `src/cpu/interpreter_gen.c` | 14580 | GENERATED. The two dispatch switches (`run_instruction`, `run_instruction0f`): 1922 call sites, 839 implemented, 1083 unimplemented, over 843 `instr*` names found in the sources. |
| `src/cpu/cpu.h` | 67 | the run loop's interface + the constants (`LOOP_COUNTER`, `INTERPRETER_ITERATION_LIMIT`, `TIME_PER_FRAME`), the `unimplemented_instruction` stop channel. |
| `src/cpu/cpu.c` | 199 | `get_phys_eip`, `interpreted_run` (upstream's `jit_run_interpreted` minus the JIT), `cycle_internal`, `do_many_cycles_native`, `main_loop`, `after_block_boundary`. |
| `src/cpu/exec_boundary.{h,c}` | 30 / 262 | THE M1c/M2 SEAT: the exception record + delivery stop, the segment machinery, the PIC/FPU no-ops, the timer layer, port I/O, `test_privileges_for_io`, and the three FPU accessors the transcript needs. |
| `src/cpu/instructions_0f.{h,c}` | 45 / 131 | the first 0F slice — `movzx`/`movsx` (0F B6/B7/BE/BF) and `pxor` (66 0F EF) + the two `run_instruction0f_16/32` wrappers. |
| `src/main.c` | 678 | the image loader (multiboot address table + ELF), the run driver, sha256, and the TWO documents: the M0 oracle-contract transcript, and (under `--fixture`) the gdb-extract-def document. |
| `Makefile` | +45 | the generator rule (`src/cpu/interpreter_gen.c` from the emitter + `x86_table.js` + the CPU sources), the APE now links the whole core, `transcript-diff`, `nasm` → the C leg, `nasm-js` → the reference leg, the `build/v86c-nasm` launcher. |
| `tests/nasm_skip_c.txt` | 126 | the C leg's OWN skip list (102 entries, reasons = measured). |
| `tests/nasm_run.py` | 2 lines | the skip-list label in the summary (§10.3). |
| `results/m1a4-gates.txt` | 438 | every gate command, its output and its exit code. |
| `results/transcript.c.txt`, `results/transcript.js.txt` | 56 each | the two transcripts the milestone diffs. |

## 2. The emitter (`gen/interpreter_c.js`)

**It imports `v86/gen/x86_table.js` UNCHANGED and never copies or edits it** — one import
statement, `import x86_table from "../v86/gen/x86_table.js"`. Everything else is a
transcription of `v86/gen/generate_interpreter.js`, function by function:

| upstream | mirrored |
| --- | --- |
| `gen_read_imm_call` (38-77) | `read_imm8`/`8s`/`16`/`32s`/`read_moffs`, and the `size = (op.os \|\| opcode%2==1) ? v : 8` rule |
| `make_instruction_name` (87-101) | **verbatim**, so the hand-written `src/cpu/instructions*.c` names are the emitter's names; the module prefix is only stripped (`instructions::instr_0A` → `instr_0A`, `instructions_0f::instr32_0FB6` → `instr32_0FB6`) because C has one namespace and no base encoding carries a 0F/66 prefix |
| `gen_instruction_body` (104-183) | the 66/F2/F3 prefix split into `if/else-if/else` over `PREFIX_66`/`PREFIX_F2`/`PREFIX_F3` (modrm.h's own constants) with the upstream `dbg_assert` as `DBG_ASSERT` |
| `gen_instruction_body_after_prefix` (185-232) | the `fixed_g` `switch (modrm_byte >> 3 & 7)` with `default: trigger_ud();` |
| `gen_instruction_body_after_fixed_g` (234-337) | the `_mem`/`_reg` split at `modrm_byte < 0xC0`, the `ignore_mod` form, the imm threading incl. `extra_imm8`/`extra_imm16`, `task_switch_test()`/`task_switch_test_mmx()` for the SSE guard, `after_block_boundary()` for the block-boundary postfix |
| `gen_table` (339-450) | the 256-case switch over `opcode \| (is_32 << 8)`, the separate 0F runner, the `{os:1}` → `0xNN`/`0x1NN` case split, `default: DBG_ASSERT(false)` |

**The two C-side differences, both forced and both documented in the file.**

1. **Error plumbing.** Upstream inlines `match expr { Ok(o) => o, Err(()) => return }` as an
   *argument expression*. C cannot, so the emitter hoists each fallible read into a local
   followed by `if (cpu->pagefault) return;` — the landed M1a-1 contract, not a second one
   (no new flag, no new sentinel).

   **AND THE HOIST ORDER IS SEMANTIC — this was the unit's one real bug.** Upstream's mem
   call is `instrX_mem(modrm_resolve(modrm_byte), modrm>>3&7, imm)` and Rust evaluates
   arguments left to right, so the **displacement is consumed before the immediate**. My
   first emitter hoisted the immediate above the branch, so for
   `66 c7 05 10 10 10 00 34 12` (`movw $0x1234,0x101010`) the immediate was read out of the
   displacement bytes and the store address resolved from what followed: the store went to
   `0x12340010` (above `memory_size`, so `in_mapped_range` dropped it). The instruction
   **length stayed balanced**, so the stream re-synchronised and every register, the eip, the
   instruction count and the other 4095 bytes of the window matched — **only the memory
   effect was wrong**, which is exactly the failure class this gate exists for. Fixed by
   emitting the immediate read per branch, after the resolve.

2. **Unported instructions.** Upstream's generated Rust calls `instructions::instr…` and
   fails to *compile* if the name is absent — a loud, correct failure. The C port is built
   incrementally, so the emitter **scopes availability**: it reads the `instr*`
   *definitions* out of `src/cpu/*.c` (excluding the generated file and the unit test) and,
   for an encoding with no definition, emits

   ```c
   unimplemented_instruction("instructions_0f::instr32_0FB6_mem");
   return;
   ```

   The dispatch case is still generated (the opcode is **classified**, never mis-decoded),
   and reaching it stops the machine and names the missing symbol — never a silent wrong
   instruction. The file's header prints the census, so "how much of the table is live" is a
   measurement, not a guess.

**The census (measured, printed by the emitter and in `results/m1a4-gates.txt`):**

| | count |
| --- | --- |
| call sites (base + 0F, 16/32 splits, `_mem`/`_reg`) | 1922 |
| implemented | 839 |
| unimplemented | 1083 |
| distinct `instr*` symbols still missing | 592 |
| — of which base-table | 160 (exactly the `D8..DF` FPU group, matching M1a-3's deferral list) |
| — of which `0F` | 923 call sites |

## 3. The run loop (`src/cpu/cpu.c`)

`get_phys_eip` (cpu.rs:3160-3172), `interpreted_run` (= `jit_run_interpreted`, 3302-3312),
`cycle_internal` (3135-3300), `do_many_cycles_native` (3417-3423), `main_loop` (3378-3415),
`after_block_boundary` (1901).

**What was dropped, exactly:** the `tlb_code` lookup and the whole `CachedCode` decision
(cpu.rs:3135-3200), the `RUN_FROM_CACHE`/`RUN_INTERPRETED_*` profiler counters,
`CHECK_JIT_STATE_INVARIANTS`, `jit_increase_hotness_and_maybe_compile`,
`jit::jit_find_cache_entry`, the `in_jit` debug flag and `is_near_end_of_page`. What remains
is upstream's *interpreter* arm, which is why the function is `interpreted_run`.

**The page-fault flag's clear sites.** Upstream has no flag: it threads `Result<T,()>`. This
port's set-only `cpu->pagefault` is therefore cleared at exactly the two places a *new*
instruction attempt begins — the top of `cycle_internal` and immediately before each
`run_instruction` dispatch — and never inside a helper or an instruction body. (Measured:
clearing only inside `interpreted_run` would let a fault from `get_phys_eip` at the top of
the *next* `cycle_internal` be read as stale, and the loop would make no progress.)

**A faithful quirk, reproduced rather than "fixed":** upstream's
`return_on_pagefault!(get_phys_eip())` at the *bottom* of the loop returns from the function
before `*instruction_counter += i`, so a mid-run fault loses the count of the instructions
already executed in that run. The port does the same. It is unobservable without paging (the
M0 image and the nasm corpus both run with `CR0.PG` clear) — stated so a later unit does not
"repair" it.

**The stop channel.** `unimplemented_instruction(name)` sets `interpreted_unimplemented` and
records the name; the two loops break on it and the caller reports it. It is deliberately
*not* the page-fault channel: a page fault is an emulated guest event with defined delivery,
whereas this is the emulator itself being incomplete. A run that reaches one produces **no
state**, never a plausible wrong one.

**`microtick` is not a wall clock** (exec_boundary.c). Upstream's is (µs); a real clock would
make the run nondeterministic, which the oracle contract forbids, and a **constant** would
make `main_loop`'s `now - start > TIME_PER_FRAME` test dead — leaving `main_loop` spinning
forever on a machine that cannot progress. The stub's unit is therefore the guest
instruction: deterministic, monotone, and it turns the yield check into a real batch
boundary (one `do_many_cycles_native` batch per `main_loop` call, with the caller's own loop
doing the batching). The reported transcript is unaffected: the stop is HLT or the limit, and
the count is the same however the batches are cut. **MEASURED:** on a purpose-built
non-halting image the C leg reports `reached 200002` and the JS reference reports
`reached 200002, in_hlt=0` — the `INTERPRETER_ITERATION_LIMIT`/`LOOP_COUNTER`/page-break
accounting is identical to the reference's, down to the number.

## 4. The loader and the two documents (`src/main.c`)

**The loader is the reference's own bring-up, transcribed:** `CPU.prototype.load_multiboot` /
`load_multiboot_option_rom` (cpu.js:1288-1570) and the ELF branch (elf.js:98-183,
cpu.js:1455-1515). Both branches are implemented, because M0's image uses the address table
and the nasm corpus is ELF:

* scan the first 8192 bytes for `0x1BADB002`, verify the checksum, then the flags domain
  (`{0,1,16}` — cpu.js:1350's `dbg_assert`);
* **address table:** `file_start = offset - (header_addr - load_addr)`, `load_end_addr == 0`
  ⇒ the whole buffer from `file_start`, `write_blob` at `load_addr`;
* **ELF:** the entry point plus the `PT_LOAD` walk, with the `entrypoint == elf.header.entry
  && vaddr <= entrypoint && vaddr + memsz > entrypoint` adjustment;
* the bring-up state of README §4: `CR0=1`, `protected_mode`, `EFLAGS=FLAGS_DEFAULT`,
  `is_32`/`stack_size_32`, segments 0..5 (offsets 0, limits `0xFFFFFFFF`, `CS=0x08`, others
  `0x10`), `EIP = get_seg_cs() + entrypoint`, `EAX=0x2BADB002`, `EBX=0x7C00`, and the
  multiboot-info word at `0x7C00`.

Three honest deviations, each stated at the site: an **unknown ELF program-header type is a
hard error** rather than the reference's `console.assert` (which is a no-op in a release
build — continuing there is precisely a silent mis-load); a **multiboot memory-map request**
(header flag bit 1) is refused rather than fabricated, since the map is built from the M2
device layer's MMIO map and neither of our images sets the bit; and the address-table
branch's `load_end_addr == 0` case is bounded to the file.

**Document (a), the transcript**, is the README §6 contract byte for byte: keys sorted at
every level, 2-space indent, one trailing newline, every machine-state word a lowercase
quoted `0x`-padded string, decimal counts, no host-specific field. Two of its details are
*the reference's own formatter*, reproduced rather than improved: `cpu.x87.data` entries carry
a **doubled** `0x` prefix (`"0x000000000x00000000"`) because `f64_hex` concatenates two
`hx()` results; and the GPR keys sort to `eax, ebp, ebx, …` (`ebp < ebx`). sha256 is
self-contained FIPS 180-4 (the APE's libc has no digest).

**Document (b), the fixture**, is the same gdb-extract-def document `tests/mkfixture.c` and
`oracle/nasm_state.mjs` print (2116 values), so one parser and one comparison serve all
three legs. It is a **separate mode** (`--fixture`) rather than a second binary because the
corpus is ELFs at their own addresses while the transcript's `load_address`/`entry_address`
are the *fixed M0 contract* (which the transcript path validates exactly as run.mjs does).
The 2-argument CLI the oracle contract fixes is unchanged.

**MEASURED:** on the M0 image, `--fixture` output is **byte-identical to
`node oracle/nasm_state.mjs`'s** (15646 bytes), not just close. That is a second, independent
gate on the fixture emitter.

## 5. The M1c/M2 seat (`exec_boundary.c`) — the deferred-symbol table

Every symbol the landed CPU files call that a later unit owns, in one visibly-incomplete
file, each marked either **(a) a semantically-correct no-op for the machine this port can run**
or **(b) a STOP that records why and halts**. M1c/M2 must **replace** these definitions, not
extend them.

| Symbol(s) | Owner | Here |
| --- | --- | --- |
| `trigger_de/ud/nm/gp`, `call_interrupt_vector` | M1c `exceptions.c` | **(b)** rewind `eip = previous_ip` (upstream's order), set `guest_exception`, stop. **No IDT lookup, no privilege check, no stack frame.** |
| `switch_seg`, `far_jump`, `far_return`, `iret16/32` | M1c `exceptions.c` | **(b)** stop, named |
| `handle_irqs` | M1c `pic.c` | **(a) no-op** — see below |
| `fwait` | M1b `fpu.c` | **(a) no-op** — nothing can be pending yet |
| `microtick`, `run_hardware_timers`, `cpu_event_halt` | M2 timer layer | **(a)** the deterministic stub of §3 |
| `io_port_read8/16/32`, `io_port_write8/16/32` | M1c/M2 device layer | **(b)** stop with the port named |
| `test_privileges_for_io` | M1c | **fast path ported in full** (`!protected_mode \|\| cpl <= iopl`); the TSS/IOP-permission slow path stops |
| `fpu_load_status_word`, `fpu_load_tag_word`, `fpu_get_sti_f64` | M1b `fpu.c` | the two word functions are **ported in full** (pure functions of landed state: measured `tag_word=0xffff`, `status_word=0x0000` at reset, as README §6 says). `fpu_get_sti_f64` is the softfloat conversion — exact for the only values the port can produce (the F80 zero, every slot's reset value) and a **stop** otherwise |

**The trap found on contact: `handle_irqs` must not be a stopping stub.** `main_loop` calls it
after *every* batch, so a stopping stub there kills every run — the whole corpus would report
"no state" for a reason that has nothing to do with the test. With no PIC and no devices
there is never a pending interrupt, which is what upstream's PIC also delivers on a device-less
machine, so it is a correct no-op. (This is the "a stub in a hot path" pattern from the
project's own working discipline.)

**MEASURED — the seat lets the port report faults the release reference cannot.** P0's class A
(5 tests) exists because `cpu_exception_hook` is DEBUG-gated in the npm release build, so the
reference panics or reports nothing. The C leg records the vector, rewinds the eip and prints
the fixture, so those tests are **run and compared** (and 5 of them pass, §7.3).

## 6. The gates — results (raw evidence: `results/m1a4-gates.txt`)

| gate | command | result |
| --- | --- | --- |
| **G0** | `rm -rf /tmp/m1a4-build && make BUILD=/tmp/m1a4-build` | **exit 0** — the generator runs, all 13 CPU TUs compile clean under `-Wall -Wextra -Werror`, the APE links |
| **G1** | `make transcript-diff` | **exit 0**; `diff results/transcript.js.txt results/transcript.c.txt` **EMPTY** |
| **G1b** | `diff <(./build/v86c tests/test.img 1000000) <(/tmp/m1a4-build/v86c tests/test.img 1000000)` | empty — the from-scratch build agrees with itself |
| **G1c** | `./build/v86c tests/test.img 1000000 --fixture` vs `node oracle/nasm_state.mjs …` | **byte-identical** (15646 B) |
| **G1d** | limits 10 / 100 / 1000 | byte-identical to the reference at each |
| **G2** | `make nasm` (C leg via `--actual`) | **exit 0** — `Passed 21/21 tests`, 0 produced no state, **102 skipped** (all named with reasons) |
| **G2-NC1** | one perturbed *expected* field (`mov16`'s `cpu.reg32[0]`) | **exit 1**, reports `cpu.reg32[0] Actual: 0xcafea551 Expected: 0xdade9511` |
| **G2-NC2** | the two class-B entries removed from the skip list | **exit 1**, reports both `pop_esp` mismatches |
| **G2-NC3** | same filtered test, no perturbation | `Passed 1/1`, exit 0 — the report is not always-on |
| **G3** | `make selftest` | **exit 0** — `cpu_selftest: 2968 checks, 0 failures` |
| **G3b** | `make oracle` | **exit 0** — transcript matches `tests/model.py` |
| **G4** | `node gen/interpreter_c.js` twice + `diff` | **EMPTY**; sha256 `bb53cf87…93c6f93` both times |
| **G4b** | `rm src/cpu/interpreter_gen.c && make` | rebuilt byte-identical; `touch gen/interpreter_c.js && make` regenerates (the rule's prerequisites are real) |
| **G5** | the exit-code contract, two purpose-built images | #DE: C rc=1 / JS rc=1, both 0 bytes on stdout. Non-halting: C rc=1 / JS rc=1, both 0 bytes, **both report `reached 200002`** |

## 7. The nasm corpus against the C leg

### 7.1 Counts (MEASURED, `make nasm`)

| | count |
| --- | --- |
| selected | 123 |
| **passed** | **21** |
| failed | 0 |
| skipped (named, with reasons in `tests/nasm_skip_c.txt`) | 102 |
| produced no state **outside** the skip list | 0 |

The 21 that pass, compared field for field with the **native hardware fixture** (`reg32`,
the 8 KiB window, `eflags & MASK_ARITH`, the FPU/mmx branch, the exception kind and its eip):

```
arith-optimisations  double-segment-prefix  idiv16-overflow  idiv32-overflow  idiv8-overflow
indirect-call  indirect-jump  jcxz  lea-nop  leave16  leave32  loop  loopnz  loopz
mov16  mov_sreg  mov_sreg_ud1  mov_sreg_ud2  push  pushf  ret-imm
```

### 7.2 Every still-skipped test, by reason class

| reason class | count | tests |
| --- | --- | --- |
| **M1b MMX/SSE `0F` table** | 62 | `cmp16-setbe, cmpxchg8b, comiss, cvtpi2ps, cvtps2pi, cvttps2pi, emms, maskmovdqu, maskmovq, movaps, movd, movntpd, movntps, movq, packssdw, packsswb, packuswb, paddb, paddd, paddsb, paddsw, paddusb, paddusw, paddw, pand, pandn, pcmpeqb, pcmpeqd, pcmpeqw, pcmpgtb, pcmpgtd, pcmpgtw, pmaddwd, pmulhw, pmullw, pmuludq, por, pshufw, pslld, psllq, psllw, psrad, psraw, psrld, psrlq, psrlw, psubb, psubd, psubsb, psubsw, psubusb, psubusw, psubw, punpckhbw, punpckhdq, punpckhwd, punpcklbw, punpckldq, punpcklwd, pxor, shufps-edgecase, ucomiss` |
| **M1b FPU (the `D8..DF` group)** | 26 | `f2xm1, fchs, fdecstp, fdiv, fdiv-zero, fdivr, fincstp, fprem, fprem-distinct, fprem1, fptan, fpu_m80, frndint, fsave_frstor, fscale, fsincos, fsqrt, fst-st-empty-tag, fstenv, fstm80, fstsw, fxtract, fxtract-zero, fyl2x, fyl2x-zero, fyl2xp1` |
| **M1b other `0F`** (bit ops / cmpxchg / setcc) | 11 | `btc-imm, btc-reg16, btc-reg32, btr-imm, btr-reg16, btr-reg32, bts-imm, bts-reg16, bts-reg32, cmpxchg, cmpxchg-setbe` |
| **emulator model — address 0 (or ≥ `memory_size`) is RAM**, the reference has the same gap (P0 class B) | 3 | `jump, pop_esp, pop_esp2` |

Each line in `tests/nasm_skip_c.txt` carries that test's **exact measured reason** — the
symbol the binary printed in a run made with `--skip-list /dev/null`, e.g.
`btc-imm  unimplemented instructions_0f::instr16_0FBA_7_mem`. The list is *generated* from
that run, not guessed, and it shrinks by itself: adding a definition to
`instructions_0f.c`/`fpu.c` regenerates the dispatcher, replaces the "unimplemented" call
with a real one, and the test runs.

**The two `0F`-less subtleties, stated because a skip list is where they could hide:**
* `btc-*`/`btr-*`/`bts-*` are **base-table** bit instructions, but the shared footer uses
  `cmpxchg` (`0F B1`), so they stop there — the reason names `0FB1` or the bit op, whichever
  came first, and both are M1b.
* `jump`'s reason is an **assert**, not an unimplemented opcode: the guest jumps past the
  2 MiB of guest RAM and the port's `dbg_assert(!in_mapped_range(phys_addr))` — upstream's
  own assert, which its release wasm compiles out — fires. The reference keeps executing at
  `0x42b001` instead (P0 class B).

**A short list of the *most common* blockers, so the cost of the next unit is visible:**
`instr_0F6F_mem` (`movq mm, mm/m64`) alone blocks **47** tests; `instr32_D9_5_reg` (`fldcw`,
FPU) blocks **15**; `instr_DB_0_mem` (`fcmovnb`, FPU) blocks **5**. Even the prologue is
enough to make most of these worth nothing until their own body's opcodes land — which is why
the list is dominated by two M1b files rather than by M1a-4 defects.

### 7.3 P0's 10 reference-leg gaps — the shrink the brief asks for, measured

P0's `tests/nasm_skip.txt` lists 10 tests the **reference** leg could not run (5 class A:
the release build's DEBUG-gated exception hook; 5 class B: hardware faults the flat memory
model does not produce). Against the C leg:

| class | test | C leg |
| --- | --- | --- |
| A | `idiv8-overflow` | **RUNS AND PASSES** (the port reports #DE with the fixture's eip) |
| A | `idiv16-overflow` | **RUNS AND PASSES** |
| A | `idiv32-overflow` | **RUNS AND PASSES** |
| A | `mov_sreg_ud1` | **RUNS AND PASSES** |
| A | `mov_sreg_ud2` | **RUNS AND PASSES** |
| B | `pop_esp` | still skipped — but now for a *stated, classified* reason, as a failure the runner **reports** the moment the entry is removed (NC2) |
| B | `pop_esp2` | same |
| B | `jump` | skipped for the assert reason above |
| B | `maskmovdqu`, `maskmovq` | still blocked by unported SSE (`instr_0F6F_mem`) |

**5 of the reference's 10 blind spots are gone** — run and compared, not skipped. The other
5 remain, and the C leg's list is *larger* than P0's because it also has to state M1b's
unported table; §7.2 is that accounting, not a hiding place (§6's NC1–NC3 are the proof the
gate can fail).

## 8. Divergences from the brief and the plan (reported loudly)

1. **`pxor` (66 0F EF) and `movzx`/`movsx` are ported early — outside this unit's assigned
   files.** Not scope drift, a *measured necessity*: `tests/test.asm` uses `movzx`/`movsx`,
   so G1's byte-identical transcript is unreachable without them; and **every one of the 123
   nasm sources opens with the same preamble ending in `pxor xmm0, xmm0`**, so without it
   the entire corpus stops on one opcode in a preamble and `make nasm` is vacuous (measured:
   0 run, 123 skipped for the same reason — the exact "skip list as a hiding place" the brief
   warns about). 20 symbols total, in `src/cpu/instructions_0f.c`, with the reason written at
   the top of the file. Everything else in M1b's 875-line `instructions_0f.rs` and the whole
   `sse_instr.rs`/`fpu.rs`/`softfloat.rs` remain unimplemented and named.
2. **`--fixture` is a mode of the same binary, not a third positional argument.** The oracle
   contract's `v86c <image> <instruction-limit>` is unchanged and is the default; the fixture
   document needs the corpus's own load addresses, which contradict the transcript's *fixed*
   load/entry validation, so the two cannot share one validation path. `build/v86c-nasm` is
   *not* a second binary: it is a one-line `sh` launcher (§10.4).
3. **The `0F` dispatcher is one function (`run_instruction0f`) plus upstream's two
   wrappers** (`run_instruction0f_16/32` in `instructions_0f.c`, verbatim cpu.rs:3132-3133).
   `instructions.h` said the wrappers belong to "the 0F table's own translation unit", which
   is where they are — that file just exists earlier than the plan's step 13.
4. **The plan's step 19 assigns `src/cpu/cpu.c` to M1c; this unit owns it.** The brief
   overrides the plan here (deliverable 2), and the file is written as the plan's step 19
   describes (minus all JIT).
5. **`exec_boundary.c` is a new file the plan does not name.** It is where M1c's
   `exceptions.c`/`pic.c` and M2's timer/device layer will land; every symbol in it is
   labelled with its owner and every one is either a correct no-op or a stop. This is the
   "build the seat, put the honest current value in it, state the gap" pattern — not a
   second implementation.
6. **`no_halt` is not observable for the C leg.** The runner's `no_halt` flag is set only by
   the JS leg's exit 3; the port **stops at the fault** instead of patching a `hlt` over it
   and running on (which is what `nasm_state.mjs` does), so "did not halt" is not something
   the C leg can report through the existing runner. It is not hidden in a skip: an
   exception in fixture mode prints the document with `(signal SIGxxx)` and exits 0, and a
   non-halting guest exits 3 → "produced no state". Both paths are exercised in G5.
7. **A non-halting guest and the limit.** `main_loop` runs a whole `LOOP_COUNTER` chunk, so a
   limit *smaller than the program* does not bind: the C leg halts and reports 561 at limit
   10, 100 and 1000 — **and the reference does exactly the same** (G7, byte-identical at each
   limit). This is upstream's chunking, not a port choice.
8. **The GPR key order and the doubled `0x` in `cpu.x87.data` are the reference's own
   formatter** (§4), reproduced because a byte diff is the gate.

## 9. What M1b and M1c must do next (in this harness's terms)

* **M1b** — `src/cpu/instructions_0f.c` (the remaining 923 call sites: MMX/SSE first, since
  their preambles appear in 62 tests), `src/cpu/fpu.c` + `softfloat.c` (the 160 `D8..DF`
  symbols and the 26 FPU tests), and the `lar/lsl/verr/verw`/`fxsave` gaps M1a-2/M1a-3 left.
  Each definition lands → the emitter's census moves → the tests come off
  `tests/nasm_skip_c.txt` **by themselves**.
* **M1c** — `exceptions.c` must *replace* `exec_boundary.c`'s exception block with real
  delivery (IDT, privilege, stack frame, `iret` re-execution) and `pic.c` `handle_irqs`;
  `tests/nasm_skip.txt`'s class A then becomes reachable for the reference leg too.
* **The one thing to read before touching the emitter**: §2's argument-evaluation-order rule.

## 10. Notes on the harness and the tree

1. **`v86/` was never written to.** The emitter imports `x86_table.js`; nothing under `v86/`
   was copied, edited, or `touch`ed (the dependency wiring is tested by touching
   `gen/interpreter_c.js`, which is ours).
2. **`build/` is shared** (a live orchestrator and other agents work in this directory), so
   the from-scratch build proof uses a private `BUILD=/tmp/m1a4-build` rather than
   `make clean`.
3. **`tests/nasm_run.py`, exactly two lines changed**, both in the summary:
   `"%d were skipped (tests/nasm_skip.txt)."` → `(…, args.skip_list)` and
   `"Skipped %d test(s) (tests/nasm_skip.txt):"` → `(…, args.skip_list)`. Reason: with the C
   leg's own list in force the gate was printing the *other* file's name, i.e. the summary
   would have misdescribed which skip list produced the counts. No comparison logic, stage
   handling or parsing changed.
4. **`build/v86c-nasm` is a build product**, generated by the Makefile: an APE is a polyglot
   the kernel refuses to exec (ENOEXEC) whose re-exec fallback lives in a *shell*, so
   `tests/nasm_run.py`'s direct `subprocess.run` cannot start it, while `./build/v86c` in a
   terminal can. The launcher is that shell (`exec ./build/v86c "$@"`), and **MEASURED** to
   be equivalent to the `.com.dbg` ELF cosmo emits alongside (byte-identical output).
5. **Determinism.** Two consecutive C runs are byte-identical; the transcript has no
   host-specific field; `microtick` is not a clock (§3).
6. **CURRENCY — another agent edited `src/cpu/instructions.c` mid-unit.** Its hash is now
   `cee6c629…4090`; `results/r2-review.md` (a read-only review of M1a-3's files, written
   during this unit) records `b4651193…6288` for the same file, and its mtime (23:41) is
   after this unit's first build. **No M1a-4 evidence is from before that edit:** the final
   all-green sweep (make, transcript-diff, nasm, selftest, oracle, determinism — 6/6 exit 0)
   and the whole of `results/m1a4-gates.txt` were produced after it, and
   `results/m1a4-gates.txt` ends with a sha256 of every file this unit's evidence depends on,
   `instructions.c` included. `string.c`'s hash is unchanged from the review's
   (`70f7f841…f44`), and `v86/` is untouched (mtimes still 21:14). The owner of that edit is
   not this unit; it is recorded here because a concurrent writer in the same tree is a
   finding to report, not a practice to extend.

## 11. Claims, marked

* **MEASURED (by running on this host):** the empty G1 diff, at the default limit and at
  10/100/1000; the from-scratch build's agreement; the fixture mode's byte-identity with
  `nasm_state.mjs` (15646 B); `make nasm` = 21 passed / 0 failed / 102 skipped, exit 0, with
  every skip reason printed; the three negative controls (exit 1 on a perturbed expected
  field, exit 1 on a removed skip, `Passed 1/1` unperturbed); `make selftest` 2968/0 and
  `make oracle` exit 0; the generator's run-twice empty diff and the delete-and-rebuild
  identity; the emitter's census (1922/839/1083, 592 distinct symbols, 160 base `D8..DF`);
  the #DE and non-halting exit codes with **both legs reporting "reached 200002"**; the
  `handle_irqs`-in-the-hot-path trap; the argument-order bug and its exact symptom
  (`0x101010` zero, `0x10100c` = `0x4fa9` instead of `0xaaad`, everything else matching);
  tag/status word `0xffff`/`0x0000` at reset.
* **READ from the reference:** `v86/gen/generate_interpreter.js` (all 497 lines),
  `v86/gen/x86_table.js`, `cpu.rs`'s run loop and constants, `cpu.js`'s multiboot/ELF loaders,
  `elf.js`, `fpu.rs`'s three accessors, `sse_instr.rs:53-61`, and `oracle/run.mjs` /
  `oracle/nasm_state.mjs` / `tests/nasm_run.py` for the two documents and the comparison.
* **INFERRED, and flagged as such:** that the 102-entry skip list's *classes* (FPU / MMX-SSE /
  other `0F`) are the right grouping for the next unit's planning (the grouping is my
  reading of the symbol names; the counts are measured); that the two class-B tests' faults
  are exactly the host-MMU gap and not a second port defect (the values and the memory model
  are measured; the attribution to the host MMU is P0's reading, reproduced here). The pure
  arithmetic of "which test needs which opcode next" is not modelled: the blockers are the
  *first* stop each test reaches, so a test's full requirement is never larger than stated,
  but it can be (and for the bit-op tests is) larger than the one symbol named.
