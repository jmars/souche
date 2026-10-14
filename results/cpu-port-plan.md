# P1 — implementation plan: the CPU-core C port (M1)

**Unit:** `HANDOFF=v86c`, unit P1. **Deliverable:** the file-level plan for M1, the
interpreter-only C port of v86's Rust CPU core (`v86/src/rust/cpu`, 18.2k lines total,
read-only reference at `./v86`). Every claim is tagged VERIFIED (run on this host),
READ (from the reference source), or INFERRED.

**Bottom line.** The port is mechanical in shape — one C file per upstream Rust file, a
`cpu_state_t` struct replacing the absolute-offset state block, a ~400-line C emitter that
reuses `v86/gen/x86_table.js` unchanged, and a per-instruction parity oracle that was
**built and validated end-to-end during this planning pass**: all 123 upstream nasm test
files translate, assemble, link, and produce native ground-truth fixtures on this host
(no nasm, no gdb — a ptrace harness replaces gdb), and the JS reference (the npm `v86`
build pinned by M0, which is exactly the clone's master commit) boots the translated
images with a state that matches the native fixtures. The main risks are lazy-flags
semantics, partial-execution page faults, and the 386 segment machinery — each has an
identified upstream source and a specific test gate.

---

## 1. The instruction table → C: reuse `x86_table.js`, write a new C emitter

**Decision: (a)** — reuse `v86/gen/x86_table.js` (875 lines) **unchanged** as the single
source of truth for opcode semantics, and write a new emitter `gen/interpreter_c.js` that
plays the role of `gen/generate_interpreter.js` (497 lines) for C. The table is the
port's biggest lever: re-deriving ~800 opcode encodings by hand is exactly the class of
error the oracle exists to catch, at a cost of months. Reuse costs nothing: the table is
plain JS data imported with one `import x86_table from "../v86/gen/x86_table.js"`.

What the C emitter must reproduce from `gen/generate_interpreter.js` (READ, lines
1–497; the whole file is the specification for the emitter):

| Upstream function | Lines | Semantics to mirror |
| --- | --- | --- |
| `gen_read_imm_call` | 38–77 | imm8/imm8s/imm16/imm1632/imm32/immaddr → which read call; the operand-size rule `size = (op.os \|\| opcode%2==1) ? size_variant : 8` |
| `make_instruction_name` | 87–101 | the C function naming scheme `instr{16\|32}_{66\|F2\|F3}{0F}{xx}_{g}_{mem\|reg}` — keep it verbatim so the hand-written C files line up with the emitter |
| `gen_instruction_body` | 104–183 | 66/F2/F3 prefix splitting: emit `if (prefixes & PREFIX_66) {...} else if ...` blocks; the `else` arm asserts no prefix |
| `gen_instruction_body_after_prefix` | 185–232 | `fixed_g` group instructions: switch on `modrm_byte >> 3 & 7`, default `trigger_ud()` |
| `gen_instruction_body_after_fixed_g` | 234–337 | the modrm `_mem`/`_reg` split (`modrm_byte < 0xC0`), `modrm_resolve` call, imm arg threading, `task_switch_test()/task_switch_test_mmx()` guard for sse entries, `after_block_boundary()` postfix for block-boundary entries |
| `gen_table` | 339–450 | the 256-case switch over `opcode | (is_32 << 8)` for the base table and the separate `run()` for 0F; operand-size case splitting: `{os:1}` entries get case `0xNN` (16-bit) and `0x1NN` (32-bit) |

The one semantic difference to handle in C: the Rust error plumbing. Upstream wraps every
page-faultable call as `match x { Ok(v) => v, Err(()) => return }`
(`gen/generate_interpreter.js:30-33`, macro `return_on_pagefault!` in
`v86/src/rust/paging.rs:3-16`). In C: helpers (`safe_read*`, `read_imm*`, `modrm_resolve`)
set a per-CPU fault flag and return a sentinel; the emitter prints
`v = safe_read32s(addr); if (cpu->pagefault) return;` for every site. One global
`cpu` pointer is fine (single-CPU scope, matching the upstream globals).

Emitter size: **~350–450 lines of JS** (the 497 upstream lines minus the 0F-table
duplication, minus `rust_ast.js` — 78 lines of Rust-AST printing replaced by plain string
emission, no tree). The emitter writes `src/cpu/interpreter_gen.c` (one switch calling
into the hand-written C files) — the analog of upstream's generated
`src/rust/gen/interpreter.rs` + `interpreter0f.rs` (generated at build time by
`make` per `v86/Makefile:199-202`; the generated files are NOT in the clone — only
`mod.rs`, READ: `v86/src/rust/gen/mod.rs`).

**Cost if ignored:** the port hand-writes a dispatch table — 800 opcode-semantics
decisions re-made by hand, each silently wrong until the oracle catches it. Reuse is
unambiguous.

## 2. The CPU state layout in C: a named struct, in `global_pointers.rs` field order

**VERIFIED facts.** Upstream keeps the entire CPU state at hardcoded absolute byte
offsets inside one flat wasm linear memory, because the JS half (`v86/src/cpu.js`) and
the Rust half (`v86/src/rust/cpu/global_pointers.rs`) share that memory and address the
state **by offset, not by name**. **There is no generator for the layout**: the offsets
are hand-duplicated — Rust at `global_pointers.rs:3-65` (reg32@64, last_op_size@96,
flags_changed@100, state_flags@108, last_result@112, flags@120, segment_access_bytes@512,
instruction_pointer@556, cr@580, sreg@668, dreg@684, segment_offsets@736,
segment_limits@768, protected_mode@800, is_32@804, stack_size_32@808, memory_size@812,
mxcsr@824, reg_xmm@832, fpu_stack_ptr@1032, fpu_control_word@1036, fpu_status_word@1040,
fpu_st@1152, efer@1280, pat@1288 …), JS at `v86/src/cpu.js:66-218` (same numbers). A
search of `gen/*.js` (the only generator directory) and the Rust tree found no offset
emitter — the two files are kept in sync by discipline (searched: no sync test exists).

**Decision.** The C port has no JS peer, so the offsets are **irrelevant** — drop them.
Use a plain C struct `cpu_state_t` whose members are declared **in `global_pointers.rs`
order** (this keeps the 1:1 port mechanical: `*cr.offset(3)` becomes `cpu->cr[3]`), plus:

- register indices from `v86/src/rust/regs.rs:1-30` (EAX=0 … EDI=7; ES=0 … GS=5; the
  AH/CH/DH/BH indices 4–7 for byte regs),
- the lazy-flags block (`flags`, `flags_changed`, `last_op_size`, `last_op1`,
  `last_result`) and the cached-state byte (`v86/src/rust/state_flags.rs:3-26`:
  is_32/ssize_32/cpl3/flat-segs bits) as plain members,
- **initialization values from the JS constructor** `v86/src/cpu.js:180-199`
  (VERIFIED): `fpu_control_word = 0x37F`, `fpu_status_word = 0`, `fpu_stack_empty = 0xFF`,
  `fpu_stack_ptr = 0`, `mxcsr` (initialized via `ldmxcsr` path), `fpu_ip/dp = 0`, and
  the reset state from `CPU.prototype.reboot_internal` (cpu.js:1540-1570: cr0, eip,
  segment selectors, `FLAGS_DEFAULT = 2`).
- `reg128` becomes `union reg128` exactly as `v86/src/rust/cpu/cpu.rs:56-69`
  (`repr(C)`, `repr(align(16))` → C `__attribute__((aligned(16)))` union of 8/16/32/64-bit
  lanes — needed for xmm and the 16-byte fpu_st slots: JS reads `fpu_st` as 4×i32 per
  register, `cpu.js:176`).
- `F80` from `v86/src/rust/softfloat.rs` (an 80-bit value stored in a 16-byte-aligned
  slot; check its exact repr when porting — READ softfloat.rs:1-40 before deciding the C
  representation; the fixture format only needs a double conversion, which is
  `(double)(long double)` on this host, VERIFIED in the harness).

Not in the struct: `tlb_data` (1M × i32, `cpu.rs:329`) — a separate 4 MiB array;
`mem8` (separate allocation, §3); `tlb_code` — dropped (JIT-only).

**Gate:** a unit test asserting the reset function produces the exact init values above
and the flag getters match a hand-computed table (§4).

## 3. Memory + paging + TLB

**READ/VERIFIED model.** `v86/src/rust/cpu/memory.rs:103-352` is the memory router: guest
RAM is `mem8` (allocated by `allocate_memory`, memory.rs:24-38); every read/write first
checks `in_mapped_range(addr)` (`addr >= 0xA0000 && addr < 0xC0000 || addr >=
*memory_size`, memory.rs:64-66) — the fast path is a plain unaligned load/store, the slow
path routes to VGA LFB / APIC / IOAPIC / the `mmap_*` JS imports (the device layer).
`memory_size` lives in the state block (global_pointers.rs:812).

Paging: `v86/src/rust/cpu/cpu.rs` —
`translate_address` (2009-2036: TLB probe, then `do_page_walk` on miss),
`do_page_walk` (2062-2336: CR0.PG check, 4 MiB PSE pages and PAE 2 MiB pages, A/D-bit
side effects via `memory::write8`, reserved-bit checks, the `valid_tlb_entries` bookkeeping
with `VALID_TLB_ENTRY_MAX = 10000` at 2272-2292), the TLB entry format (bits
`TLB_VALID/READONLY/NO_USER/IN_MAPPED_RANGE/GLOBAL/HAS_CODE/NO_EXEC`, cpu.rs:261-267; the
entry stores `host_phys ^ guest_addr` so one load yields the host address — keep the same
trick in C or use a struct; either is fine for an interpreter),
`full_clear_tlb`/`clear_tlb` (2338-2385; clear_tlb preserves global pages),
`invlpg` (4468-4476), `set_cr0`/`set_cr3` (2931-2978: CR0.PG/PE gating, TLB flushes,
`load_pdpte` for PAE), and `safe_read*`/`safe_write*` (3538-3568, 3941-4040: page-crossing
splits at `addr & 0xFFF` thresholds, then `memory::read*`).

**Key invariant (VERIFIED):** v86 never tracks guest writes to page tables. `safe_write*`
→ `memory::write*` → `jit::jit_dirty_cache_small` (jit.rs:2362-2376) invalidates only JIT
code, never `tlb_data`. Correctness rests on the guest's own TLB-shootdown discipline
(invlpg / CR3 reload), exactly like real hardware. The C port must preserve this: TLB
invalidation happens **only** in invlpg, set_cr0 (PG/WP change), set_cr3, and the
clear_tlb paths — adding "smart" write tracking would be new semantics, not a port.

**Decisions.**
- **PSE: keep.** 4 MiB pages are one arm of `do_page_walk` and i386 Linux uses them for
  its identity map at boot. Trivial to port.
- **PAE: defer to M2.** The baliveau guest is i386 (non-PAE) Linux. Port `reg_pdpte`
  storage, stub `load_pdpte`/PAE arms behind an explicit "PAE unimplemented" trap —
  never silently misinterpret 64-bit PTEs. (Upstream itself asserts 32-bit-only PDPTEs,
  cpu.rs:2987-2997.)
- **APIC/IOAPIC mmio routing: defer.** Keep `in_mapped_range` and the APIC/IOAPIC address
  constants (cpu.rs:296-ish), stub the handlers; i386 Linux can run the legacy 8259 path
  (§8 R8).
- **JIT-only pieces drop:** `tlb_code`, `TLB_HAS_CODE`, `tlb_set_has_code*`,
  `translate_address_*_jit`, `safe_read*_slow_jit`, `jit_dirty_*` calls. The interpreter
  fast path (`memory::read*` after one `translate_address`) ports 1:1.
- **C-specific care:** `read32_no_mmap_check` (memory.rs:135) does an unconditional
  4-byte load; `memory_size` is the only bound. Allocate `mem8` as `memory_size` rounded
  to 4 KiB **plus 16 bytes slack** so the last-page reads cannot fault (upstream's wasm
  linear memory over-allocates the same way; in C it is a real segfault otherwise).

The brief's "safe_read/safe_write fast paths unrepresentable in naive C" concern applies
to the **JIT** (the single-load TLB entry + `safe_read_jit_slow`, `codegen.rs:642-700`),
which is out of scope; the interpreter path is ordinary C.

**Gate:** a C unit test driving hand-built 32-bit and 4 MiB page tables through
`translate_address`/`safe_read` (present/absent/read-only/user/supervisor/page-crossing),
asserting both the values and the fault flag + `trigger_pagefault` side effects (cr2,
eip reset).

## 4. Lazy flags: the exact invariants a C port must preserve

**READ.** `v86/src/rust/cpu/misc_instr.rs:8-66` (the getters), `v86/src/rust/cpu/arith.rs`
(the setters), `get_eflags` at cpu.rs:1911-1918, `update_eflags` at cpu.rs:4478-4520.

The machinery: `flags` holds the materialized arithmetic flags; `flags_changed` is a
bitmask of **stale** flags; `last_op_size`, `last_op1`, `last_result` record the last
arithmetic op; **bit 31 of `flags_changed` is `FLAG_SUB`** — the add-vs-sub semantic
selector used by the CF/AF getters (misc_instr.rs:12-22, 34-43). Invariants:

1. Every getter checks `flags_changed` first and computes from the three `last_*` fields
   (CF: `(last_result ^ sub_mask) < (last_op1 ^ sub_mask)`; AF: reconstruct `op2 =
   (last_result - last_op1) * ±1` then `op1 ^ op2 ^ result & 0x10`; PF: the
   `0x9669 << 2 >> (…) & 4` parity trick on the low nibble; ZF/SF/OF: shifts of
   `last_result`/xor patterns).
2. Every arithmetic writer either updates `flags` **and clears the bit in
   `flags_changed`**, or sets the bit. `add`/`sub`/`cmp`/`neg` set `FLAGS_ALL`
   (arith.rs:13-21, 29-37, 74-77, 98-103); `adc`/`sbb` **read `getcf()` first**, then
   compute CF/AF/OF eagerly into `flags` and mark only the rest stale (arith.rs:22-45,
   48-65); `inc`/`dec` **preserve CF** by copying `getcf()` into `flags` then marking
   `FLAGS_ALL & ~1` (arith.rs:114-136); `mul`/`imul`/`div`/`idiv` have their own partial
   patterns (arith.rs:138-230); shifts/rotates (arith.rs:231+) update CF/OF eagerly and
   mark ZF/SF/PF stale.
3. `get_eflags` materializes: `flags & !FLAGS_ALL | getcf() | getpf()<<2 | getaf()<<4 |
   getzf()<<6 | getsf()<<7 | getof()<<11` (cpu.rs:1911-1918). Anything that reads flags
   goes through the getters — never reads `flags` directly.

This is the classic silent-wrong-codegen surface: a missed `flags_changed` update makes
a *later, unrelated* instruction see a stale flag. The nasm corpus is what catches it —
every test ends by pushing the flags and masking to 0x8ff, and the fixtures compare
`eflags & MASK_ARITH` (run.js:53, 416). Port the fields verbatim; port the two
`dbg_assert!`s in the getters (misc_instr.rs:10-11) as C assertions in debug builds.

**Gate:** a C unit test table of hand-computed (op, a, b) → {CF,PF,AF,ZF,SF,OF} cases
(the same values the M0 model and the nasm fixtures already pin down), plus the full
nasm arith subset once the dispatch exists (§7).

## 5. FPU + SSE: what M1 must contain

The baliveau guest is **i386 Linux + i386 LuaJIT** (mission brief). Requirements:

- **x87 is MANDATORY in M1.** The i386 kernel saves/restores FPU state on context switch
  with `fnsave`/`frstor` and reports FPU via CPUID; i386 glibc does its double math in
  x87; an i386 kernel without a working FPU is a dead end. Port
  `v86/src/rust/cpu/fpu.rs` (913 lines — the instruction front-end, stack/tag management,
  env save/restore, status-word handling) + `v86/src/rust/softfloat.rs` (286 lines — the
  80-bit arithmetic core). The nasm fpu tests are the gate: `fpu_m80`, `fstm80`,
  `fdiv-zero`, `fsave_frstor`, `fstenv`, `fstsw`, `f2xm1`, `fyl2x`, `fprem`, `fsqrt`,
  `frndint`, `fscale`, `fxtract`, `fptan`, `fsincos` — all present in the 123-file corpus
  and all translated/assembled/fixtured in this planning pass.
- **SSE: include `sse_instr.rs` (361 lines) in M1, sequenced last.** LuaJIT's i386
  backend prefers SSE2 for floating point (with x87 fallbacks), and the cost of the port
  is small; the corpus has ~40 SSE/MMX tests (`movaps`, `movntps`, `padd*`, `pand`,
  `maskmovq`, `comiss`, `ucomiss`, `cvt*`). If M1 time-boxes, SSE is the designated
  deferral — the emulator is still a valid 386+87 without it.
- **`task_switch_test()` / `task_switch_test_mmx()` (CR0.TS/EM → #NM)** must be in M1 —
  the emitter's SSE guard depends on it, and the guest toggles CR0.TS.

## 6. The oracle: per-opcode parity, without nasm or gdb — VALIDATED THIS PASS

The upstream oracle (nasm → native gdb run → fixture; boot in v86; compare) is
`v86/tests/nasm/{create_tests.js,gen_fixtures.js,run.js,gdb-extract-def}`. This host has
no nasm/gdb/java, so the plan replaces each piece; **every replacement was built and
verified during this planning pass** (artifacts in `/tmp/oracle-exp/`, copies in the
memory graph, artifact nodes `handoff-v86c-cpuplan-artifact-1/2`):

1. **Translation** — `nasm2gas.py` (90 lines): NASM → GNU as `.intel_syntax noprefix`.
   Surface measured over the corpus: 2 `%include`s, 3 single-arg `%macro`s (inlined),
   `db/dw/dd/dq`, `align`, `section`, `global`, `equ`, `resb`, trailing-`h` hex, size
   prefixes on memory operands, `dd 1000.0` float literals, nasm float constants
   (`__QNaN__` etc.), hex-floats (`0x80000001.0`), bare no-operand `fdiv`/`fdivr`/`fmul`,
   `st1` operands, `tword`→`tbyte`, and the gas-intel traps (`mov esp, symbol` is a
   *memory* operand in gas → `OFFSET`; register names must not be hex-converted).
   **VERIFIED: 123/123 translate, assemble (`as --32`), and link (`ld -m elf_i386
   --section-start=.bss=0x100000 --section-start=.text=0x80000
   --section-start=.multiboot=0x20000 -e _start`) — the exact upstream geometry from
   `create_tests.js:127-130`.**
2. **Native ground truth without gdb** — `mkfixture.c` (155 lines): ptrace-run the ELF,
   place `int3` at the `loop` symbol, catch SIGFPE/SIGILL/SIGSEGV/SIGBUS, and emit the
   `gdb-extract-def` JSON (2116 values: 8 GPRs, eip, 8×st, 16×mm words, 32×xmm words,
   8 KiB of BSS, eflags, ftag, fstat). **VERIFIED: 123/123 fixtures generated**
   (113 breakpoint / 3 SIGFPE / 2 SIGILL / 5 SIGSEGV), values spot-checked correct
   (esp=0x101FD4, masked flags 0x46 in memory, pi in fpu fixtures). Host-kernel quirks
   catalogued and worked around (each cost a measurement): `hlt` in ring 3 traps with
   SIGSEGV/si_code=SI_KERNEL (hence the int3 breakpoint at `loop`, which *replaces* the
   hlt — never executed); compat-tracee `PTRACE_GETREGS`/`GETFPREGS` return the **x86_64**
   layouts (512-byte fxsave-format fpregs incl. xmm+mxcsr — no fxsave payload needed);
   `PEEKTEXT` at non-4-aligned addresses reads one byte late and EIOs on the last word
   of a mapping → aligned pokes + `process_vm_readv` for bulk reads; the exec stop needs
   the two-stop wait (SIGSTOP then exec SIGTRAP); the x87 tag word is the abridged FXSAVE
   ftw with the kernel's `| 0xffff0000` (`convert_from_fxsr`), and `gdb-extract-def`'s
   `$ftag >> (2*(top+i)) & 3 == 2` "invalid" test is the 2-bit-shift artifact of the
   abridged word — reproduced mechanically.
3. **The JS reference leg** — M0's pinned npm build (`v86@0.5.471+g94faf3d`,
   `oracle/package.json`) **is exactly the clone's master commit** (`git log` HEAD
   `94faf3d`, VERIFIED), so its `load_multiboot` ELF branch is the code read at
   `v86/src/cpu.js:1455-1515`. M0's exact recipe (wasm_fn instantiate, `disable_jit`,
   `reset_memory` + `load_multiboot` + bounded `main_loop()` calls) **boots the
   translated ELF images**: VERIFIED on `arith-optimisations.img` — 22 instructions,
   eip at the hlt, esp=0x101FD4, all GPRs 0 — matching the native fixture on every field
   run.js compares (run.js:340-500: regs, memory window, `eflags & MASK_ARITH`,
   fpu/status with `FPU_STATUS_MASK`, exception kind; eip is compared only for exception
   cases). Caveat (VERIFIED): unpinned npm `v86` installs 0.5.470, whose load_multiboot
   rejects these ELFs — the pin is load-bearing; never float it.
4. **Per-opcode differential test** = for each `.asm`: translate → assemble → link →
   native fixture (mkfixture) → JS-v86 state (M0 recipe) → compare with run.js's exact
   logic. The runner (`tests/nasm_run.py`) supports a per-opcode skip list that shrinks
   as instructions land, so the suite is the continuous gate, not an end-of-unit one.
   The 123 checked-in files are the M1 corpus; upstream's on-demand `gen_*.asm` tests
   (`create_tests.js`, 4 configs × 5 tests × every table opcode) are a stretch target —
   their emitted syntax (`db`/`dw`/`dd` blobs) is within the translator's surface.

## 7. File-level steps, in order

Tools first (P0 — the oracle scaffold, independent of the CPU work), then the CPU in
three slices, each independently gated.

**P0 — oracle scaffold (reuses the validated /tmp/oracle-exp artifacts)**
1. `tests/nasm2gas.py` — copy of the translator. **Gate:** all 123 files translate +
   assemble + link.
2. `tests/mkfixture.c` — copy of the ptrace harness. **Gate:** 123/123 fixtures
   regenerate byte-identical.
3. `tests/nasm_run.py` — fixture↔JS-v86 differential runner (run.js comparison logic,
   skip list). **Gate:** run all 123 against the *JS reference itself* first — this
   validates the comparison against upstream's own emulator before a line of C exists;
   expected outcome: near-total pass (known upstream FPU-status-mask fuzzes aside).

**M1a — the arithmetic spine** (gates are unit tests until the dispatch lands, then the
nasm subset)
4. `src/cpu/cpu_state.h`, `src/cpu/cpu_state.c` — the struct (§2), reset/init, register
   accessors, state_flags + `update_state_flags` (cpu.rs:3346-3356), flag getters (§4).
   Mirrors `global_pointers.rs` + `regs.rs` + `state_flags.rs` + cpu.js:66-218.
5. `src/cpu/memory.c` — allocation + the read/write routers (memory.rs:103-352), device
   hook table replacing the `mmap_*` imports, VGA/APIC/IOAPIC stubs.
6. `src/cpu/paging.c` — TLB + `translate_address*` + `do_page_walk` (PSE only) +
   `safe_read*/safe_write*` + `trigger_pagefault` + `invlpg` + `clear_tlb` +
   `set_cr0/cr3` (cpu.rs:1955-2336, 2338-2385, 2461-2512, 2931-2997, 3538-3568,
   3941-4040, 4468-4476).
7. `src/cpu/arith.c` + `src/cpu/flags.c` — arith.rs (1206) + misc_instr.rs:8-66.
8. `src/cpu/modrm.c` — modrm.rs (133) + cpu.rs:3122-3130 (`modrm_resolve`,
   `get_seg_prefix`).
9. `src/cpu/misc_instr.c` — misc_instr.rs (613): push/pop, `get_stack_pointer`,
   `adjust_stack_reg`, cpuid, etc.
10. `src/cpu/string.c` — string.rs (701): rep movs/stos/lods/cmps/scas/ins/outs.
11. `src/cpu/instructions.c` — port of instructions.rs (2504), base opcodes only.
12. `gen/interpreter_c.js` → generated `src/cpu/interpreter_gen.c` — the emitter (§1).
    **Gate (with 4–12): the nasm subset that excludes fpu/sse/segment-exception tests
    passes via tests/nasm_run.py with the skip list.**

**M1b — the 0F table, FPU, SSE**
13. `src/cpu/instructions_0f.c` — instructions_0f.rs (5300), non-FPU/SSE first.
14. `src/cpu/softfloat.c` — softfloat.rs (286) + the F80 representation.
15. `src/cpu/fpu.c` — fpu.rs (913) + the fpu helpers in misc_instr.rs.
16. `src/cpu/sse_instr.c` — sse_instr.rs (361) + `task_switch_test*` (cpu.rs:2999-3007).
    **Gate: full 123-test nasm corpus, no skip list (or an explicitly justified
    remainder).**

**M1c — execution, exceptions, interrupts, integration**
17. `src/cpu/exceptions.c` — trigger_de/ud/gp/nm/np/ss, `call_interrupt_vector`, iret16/32,
    far_jump/far_return, `do_task_switch`, `lookup_segment_selector`/`switch_seg`/
    `load_tr`/`load_ldt` (cpu.rs:473-1900, 2387-2460, 2638-3052).
18. `src/cpu/pic.c` — pic.rs (402) + `handle_irqs` (cpu.rs:4573-4650) + hlt handling.
19. `src/cpu/cpu.c` — the run loop: `main_loop` (cpu.rs:3378-3415) + `cycle_internal`
    **minus all JIT** (3135-3300: keep `jit_run_interpreted`'s shape — per-instruction
    dispatch, page-crossing/iteration-limit block breaks — drop the tlb_code lookup),
    the `microtick`/`run_hardware_timers` imports replaced by the C timer layer
    (M0/PIT work already owns those interfaces).
20. `src/main.c` integration — the multiboot loader (raw M0 format + the ELF branch of
    cpu.js:1455-1515) and the M0 transcript contract (same JSON shape as oracle/run.mjs).
    **Gate: M0's transcript diff is byte-identical from the C binary; the nasm corpus
    still passes; then the baliveau boot attempt begins (out of M1's own gate).**

**Explicitly deferred to M2:** apic.rs, ioapic.rs, PAE, the VGA device, and everything
under `v86/src/rust/jit*.rs` / `codegen.rs` / `control_flow.rs` (JIT — non-goal).
`vga.rs` is only needed as far as the dirty-bitmap callbacks are — stub in M1.

Size estimate: ~14–16k lines of C (the 18.2k Rust lines port roughly 1:1, minus JIT
plumbing), ~450 lines of emitter JS, ~500 lines of test tooling.

## 8. Risks (evidence + cost)

- **R1 — state/init parity.** The offsets vanish in C, but the *initial values* do not
  (cpu.js:180-199, reboot_internal 1540-1570). Wrong `fpu_control_word` (0x37F) or
  `FLAGS_DEFAULT` is silently wrong rounding/IF. Cost: subtle guest misbehavior; the
  nasm fpu tests + the M0 transcript catch it.
- **R2 — lazy flags.** §4. Cost: the classic silent-wrong surface — a stale flag seen by
  a later instruction (LuaJIT's generated code lives and dies by flags). Catcher: the
  nasm corpus (every test ends with a pushf-mask compare) + the unit table.
- **R3 — exception/page-fault delivery order.** Upstream semantics (VERIFIED): a
  `safe_read` fault triggers `trigger_pagefault` *inside* `do_page_walk`
  (cpu.rs:2130-2136: cr2 set, `*instruction_pointer = *previous_ip`, vector delivered)
  and the faulting instruction **returns early with partial effects** (paging.rs
  `return_on_pagefault!`); the instruction re-executes after the handler's iret. The
  same Err-propagation applies *inside* `call_interrupt_vector` (cpu.rs:822-830: a fault
  while reading the IDT aborts the in-flight delivery and raises a new fault). The C
  port must reproduce "abort the current instruction/interrupt, deliver the fault" —
  not the cleaner-looking "undo the instruction". Cost of getting it wrong: Linux dies
  on its first fault or interrupt. Mitigation: port the early-return shape verbatim,
  gate on the nasm exception tests + a crafted IDT-fault test.
- **R4 — segment machinery.** GDT/LDT/TSS, call/task gates, iret privilege checks
  (cpu.rs:2638-2900, 473-1900). i386 Linux installs its own GDT/LDT/TSS in the first
  milliseconds — this is boot-critical, not a corner. Cost: no boot. The nasm tests
  cover only part of it (mov_sreg, indirect-jump/call); the M1c integration gate (Linux
  boot attempt) is the real one.
- **R5 — softfloat/F80.** 80-bit extended arithmetic with rounding modes, denormals, the
  C1/C3 status bits. Cost: glibc math wrong. The nasm fpu tests compare against *real
  hardware* with the 7-significant-digit tolerance and the `FPU_STATUS_MASK` exclusions
  (run.js:54-60) — port those exact tolerances, don't "fix" them.
- **R6 — oracle host quirks.** The ptrace/hlt/PEEK oddities catalogued in §6. Cost of an
  unhandled one: silently wrong fixtures (the eip/eflags misreads were real bugs caught
  by cross-checking against known values). Mitigation: the fixtures are validated
  against the JS reference *and* spot-checked against hand-computed values; the M0
  transcript remains an independent cross-check.
- **R7 — mem8 boundary.** Unconditional 4-byte reads at `memory_size - k`
  (memory.rs:135). Cost in C: an actual segfault. Mitigation: +16 bytes allocation slack
  (§3).
- **R8 — APIC deferral.** i386 Linux runs the legacy 8259 path when no local APIC is
  present, but modern kernels increasingly assume one. Cost if wrong: the boot gate
  stalls until M2's apic lands. Mitigation: implement the APIC *presence* probe honestly
  (absent, like the reference's `apic_enabled=false` path, cpu.js:147-150) and revisit
  at first boot.

## 9. What was verified vs read vs inferred

- **VERIFIED by running on this host:** the oracle pipeline end-to-end (§6: 123/123
  translation+assembly+link, 123/123 fixtures, JS-reference boot and state match on
  arith-optimisations.img, all kernel-quirk workarounds, the value-level checks — esp
  0x101FD4, memory flags 0x46, pi in FPU fixtures); the M0 pin equals the clone's HEAD
  commit; the unpinned-npm version skew.
- **READ from the reference:** all cited file:line sites (state offsets, memory/paging
  code, flag getters, generator structure, run.js comparison logic, gdb-extract-def
  format, load_multiboot ELF branch).
- **INFERRED (flagged as such in the text):** the PAE/APIC deferral being safe for the
  baliveau guest (depends on the guest kernel config — first-boot check, §8 R8), and the
  C line-count estimate.
