# M1a-3 — string ops and the base instruction set

**Unit:** `HANDOFF=v86c`, unit M1a-3 (coder). **Deliverables:** `src/cpu/string.{h,c}` (port of
`v86/src/rust/cpu/string.rs`, 701 lines), `src/cpu/instructions.{h,c}` (port of
`v86/src/rust/cpu/instructions.rs`, 2504 lines, base opcodes), the extension of
`src/cpu/cpu_selftest.c`, and the Makefile wiring. `v86/` read-only; `gen/`, `tests/` and
`oracle/` untouched (`tests/test.img` sha256 still `1a597ac1…e737a`, `gen/` still holds only
`README.md`).

**Bottom line.** `make selftest` → RC=0, **2968 checks, 0 failures** (M1a-2 left it at 2415);
`make` → RC=0 with all nine CPU translation units clean under `-Wall -Wextra -Werror`;
`make oracle` → RC=0; `make nasm` → RC=0 (`[+] Passed 113/113 tests`, 10 pre-existing skips).
Eleven single-line mutations of this unit's semantics make the suite FAIL and restore green
(`results/m1a3-gates.txt`; two further candidate mutations were caught by `-Werror` instead —
reported as such, not counted). The instruction-function **names and argument arities are
cross-checked against the emitter's own naming function run over the real `x86_table.js`:
821 base-table symbols, all 821 present, zero arity mismatches** (measured; §3).

---

## 1. What changed (every file)

| File | Lines | What it is |
| --- | --- | --- |
| `src/cpu/string.h` | 110 | the 48 exported `movs/stos/lods/cmps/scas/ins/outs` wrappers (`_rep`/`_repz`/`_repnz`/`_no_rep`), plus the **port-I/O boundary** (`io_port_read*`/`io_port_write*`, `test_privileges_for_io`) this unit is the first caller of. |
| `src/cpu/string.c` | 531 | `string.rs:25-701`: one `count_until_end_of_page` helper, one `string_instruction` with the whole fast/slow-path structure, 48 one-line wrappers. |
| `src/cpu/instructions.h` | 919 | the boundary declarations (§5) + **831 prototypes generated from the definitions in `instructions.c`**, so the header cannot drift from them. |
| `src/cpu/instructions.c` | 2353 | `instructions.rs`'s base table function by function (637 of the 831 are one-liners), the two prefix re-dispatch helpers (`run_prefix_instruction`, `segment_prefix_op`), and the `D8..DF` deferral marker. |
| `src/cpu/paging.h` | 106 → 205 | **`SAFE_READ_WRITE8/16/32`** — the port of `safe_read_write*` (cpu.rs:4052/4073/4101), upstream's `&|x| expr` closure as a statement macro — and `call_interrupt_vector`'s `Option<i32>` error code spelled faithfully. |
| `src/cpu/paging.c` | 750 (1 line) | the one `call_interrupt_vector` call site gains the `true` "error code present". |
| `src/cpu/memory.{h,c}` | 120 → 122, +15 | **`memcpy_into_svga_lfb`** (memory.rs:254-264): dropped by M1a-1 because nothing called it; `string.c`'s movs fast path is its first caller. |
| `src/cpu/misc_instr.{h,c}` | 140 → 147, 607 → 688 | **`get_real_eip`** (cpu.rs:4300-4304) and **`popa16`/`popa32`** (cpu.rs:3054-3078) — see F-3/F-4. |
| `src/cpu/cpu_selftest.c` | 2531 → 4166 | the `call_interrupt_vector` stub widened, 19 new recording stand-in definitions for the boundaries this unit is the first caller of, and 10 new gate functions. |
| `Makefile` | 124 → 128 | `string.c`/`instructions.c` added to `CPU_SRC` and to the object rule's header prerequisites. |
| `results/m1a3-gates.txt` | 194 | the raw output of every gate below (G1–G5) and the G2 mutation table. |
| `results/m1a3-{namecheck.mjs,argcheck.mjs,bulkcheck.py}` | 203 | the three cross-check rigs §3 and §9 rely on, kept so the measurements can be re-run. |

## 2. What is ported

* **`string.rs` in full** (all 48 exported wrappers, the fast path, the slow path, the
  `rep`-prefix state via `Rep`, the direction flag, the segment overrides, the ASIZE mask,
  the per-page iteration limit and the `instruction_pointer = previous_ip` rewind).
* **`instructions.rs`, the base table: 831 functions** — every one-byte opcode the generator
  routes to the module `instructions`: 00-FF minus the `D8..DF` group (§5).
* Out of `cpu.rs`, only what those two files are the *first caller* of: `safe_read_write*`,
  `get_real_eip`, `memcpy_into_svga_lfb`, `popa16/popa32`, `run_prefix_instruction`,
  `segment_prefix_op`. Nothing landed is redefined.

## 3. Naming-scheme compliance (the emitter's contract)

`gen/generate_interpreter.js:87-101` `make_instruction_name` is kept verbatim:

```
    `${module}::instr${encoding.os ? size : ""}_${second_prefix}${first_prefix}${opcode_hex}${fixed_g_suffix}`
```

For the base table `module == "instructions"`, `first_prefix ∈ {"", "F2", "F3"}` (no base
encoding has `0F` — those are `instructions_0f.c`'s, M1b) and `second_prefix == ""` (`66`
occurs only on `0F`-prefixed encodings). **So the C symbol is the upstream Rust name with the
module path stripped**, which is what `instructions.c` defines: `instructions::instr16_F3A5`
→ `instr16_F3A5`. `string.rs`'s names (`movsb_rep`, …) are not opcode functions and are
called by `instructions.c`'s `instr_A4`/`instr_F2A4`/… one-liners, exactly as upstream.

**MEASURED, not asserted.** A throwaway script copies `make_instruction_name` verbatim
(importing that module would run the generator) and imports the real `v86/gen/x86_table.js`:

* it enumerates **821 distinct base-table symbols** the emitter will call (`_mem`/`_reg`
  split, the `os` → `16`/`32` case split, `fixed_g` suffixes, the `ignore_mod` form);
* all 821 exist in `instructions.c` — **0 missing**;
* it also reports **0 arity mismatches** against `instructions.h`'s parameter lists, using
  the generator's own argument construction (modrm fields first, immediate last, the second
  modrm field omitted for `fixed_g` entries);
* the 13 C-only symbols it does *not* name are exactly the helpers upstream also keeps
  non-opcode: `arpl`, the `FF /2` and `/4` helper wrappers, `instr_8C_check_sreg`,
  `push16_reg`/`push32_reg`, and the three predicates — plus my two prefix re-dispatch
  functions.

Raw output in `results/m1a3-gates.txt`. The prototype block in `instructions.h` is generated
from the definitions, so a name or signature cannot drift inside this unit without the
cross-check noticing.

**MEASURED — a second reader for the bulk.** `results/m1a3-bulkcheck.py` normalises the Rust
and the C of every function that is a *single statement* in both (the closure/`return_on_pagefault!`
forms are not textually comparable, so they are excluded) and compares them token by token:
**493 functions compared, 18 "mismatches", all of them one of the divergences already stated
below (D11's `SEX8`/`SEX16` for Rust's `x << n >> n`, the `uint32_t` cast on mul/div, D12's
`~` for Rust's bitwise `!`, the `(void)` casts for unused parameters) or a normaliser
artifact — 0 unexplained.** This is the check that covers a swapped operand or a wrong
register index in a one-liner, which a slice-based test can miss. The three check scripts are
kept under `results/` so the measurement can be re-run (`python3 results/m1a3-bulkcheck.py`,
`node results/m1a3-namecheck.mjs`, `node results/m1a3-argcheck.mjs`).

## 4. The one name the emitter must define (the other direction)

`instructions.c` calls `run_instruction(opcode)` (`instr_66/67/F0/F2/F3/FB` and
`run_prefix_instruction`). Upstream's `cpu.rs:3131` is
`run_instruction(opcode) = gen::interpreter::run(opcode)`; the C emitter's generated
`src/cpu/interpreter_gen.c` **must define `void run_instruction(int32_t opcode)`** with the
convention the generator already uses — the low byte is the opcode, bit 8 is the operand
size (`read_imm8() | is_osize_32() << 8`, matching the generator's `0xNN` / `0x1NN` case
split). This is the only name this unit needs from the emitter; `instructions.h` declares it.

Also needed by the emitter's own output (NOT by this unit — recorded so it is not discovered
late): `after_block_boundary()` (the generator's `instruction_postfix`) and
`run_instruction0f_16/32` (declared in `instructions.h`, defined with the 0F table, M1b —
upstream's `_32` adds `0x100`, so the wrapper belongs to its owner, not to me).

## 5. THE DEFERRED SYMBOL LINKAGE TABLE (nothing is silently missing)

Every symbol this unit calls that it does not define, with its owner. Each is declared in
`instructions.h`/`string.h` with the same comment, and `cpu_selftest.c` supplies a recording
definition so the test binary links (the arrangement M1a-1 made for `call_interrupt_vector`).

| Symbol | Upstream | Owner | First caller here |
| --- | --- | --- | --- |
| `run_instruction` | cpu.rs:3131 | the emitter unit (plan §1 step 12) | `instr_66…FB`, `run_prefix_instruction` |
| `run_instruction0f_16/32` | cpu.rs:3132-3133 | M1b (the 0F table) | `instr16_0F` / `instr32_0F` |
| `far_jump` | cpu.rs:1128 | M1c `exceptions.c` | `instr16_9A`, `instr32_EA`, `FF /3`, `/5` |
| `far_return` | cpu.rs:1507 | M1c `exceptions.c` | `instr16_CA`/`CB`, `instr32_CA`/`CB` |
| `iret16` / `iret32` | cpu.rs:507-508 | M1c `exceptions.c` | `instr16_CF` / `instr32_CF` |
| `call_interrupt_vector` | cpu.rs:799 | M1c `exceptions.c` | `instr_CC`, `instr_CD`, `instr_CE` |
| `switch_seg` | cpu.rs:2676 | M1c `exceptions.c` | `instr16_07`/`17`/`1F`, `instr_8E_*` |
| `handle_irqs` | cpu.rs:4573 | M1c `pic.c` | `popf` (9D), `sti` (FB), `hlt` (F4) |
| `microtick`, `run_hardware_timers`, `cpu_event_halt` | cpu.rs:37-41 (`js::`) | the timer/halt layer (M1c; plan §7 step 19) | `instr_F4` |
| `io_port_read8/16/32`, `io_port_write8/16/32`, `test_privileges_for_io` | cpu.rs:4613-4649, 3009 | the device layer (M1c/M2, `pic.c` + the io bridge) | `string.c` ins/outs, `instr_E4..EF`, `ECX/EDX` forms |
| `fwait` | fpu.rs:803 | M1b `fpu.c` | `instr_9B` (only the wait; the CR0.MP\|CR0.TS test is here) |
| `trigger_de/ud/nm/gp`, `*_or_pagefault`, `safe_read*`/`safe_write*`, `translate_address*`, `virt_boundary_*` | — | M1a-1/M1a-2, landed | — |

**`D8..DF` — 160 functions DEFERRED TO M1b** (the §2 exclusion). `instructions.rs:1643-2003`
is the x87 entry-point block; every function in it is a one-line call into `fpu.rs` whose
arguments are `fpu.rs`/`softfloat.rs` types (`f80_t`, `fpu_load_m32`'s fault contract, and the
seven `F80::*` constants: `ONE`, `LN_10/LN_2`, `LOG2_E`, `PI`, `LN_2/LN_10`, `LN_2`,
`ZERO`). Porting them here would mean freezing 81 `fpu_*` signatures and seven constant
values from outside the file that owns them — the refusal M1a-2 recorded for
`lar/lsl/verr/verw`. They are BASE-table opcodes, so **the emitter will generate calls to
all 160 names**; M1b defines them (plan §7 steps 14-16). The exact list, for completeness
checking:

```
  instr16_D9_0_mem        instr16_D9_0_reg        instr16_D9_1_mem        instr16_D9_1_reg
  instr16_D9_2_mem        instr16_D9_2_reg        instr16_D9_3_mem        instr16_D9_3_reg
  instr16_D9_4_mem        instr16_D9_4_reg        instr16_D9_5_mem        instr16_D9_5_reg
  instr16_D9_6_mem        instr16_D9_6_reg        instr16_D9_7_mem        instr16_D9_7_reg
  instr16_DD_0_mem        instr16_DD_0_reg        instr16_DD_1_mem        instr16_DD_1_reg
  instr16_DD_2_mem        instr16_DD_2_reg        instr16_DD_3_mem        instr16_DD_3_reg
  instr16_DD_4_mem        instr16_DD_4_reg        instr16_DD_5_mem        instr16_DD_5_reg
  instr16_DD_6_mem        instr16_DD_6_reg        instr16_DD_7_mem        instr16_DD_7_reg
  instr32_D9_0_mem        instr32_D9_0_reg        instr32_D9_1_mem        instr32_D9_1_reg
  instr32_D9_2_mem        instr32_D9_2_reg        instr32_D9_3_mem        instr32_D9_3_reg
  instr32_D9_4_mem        instr32_D9_4_reg        instr32_D9_5_mem        instr32_D9_5_reg
  instr32_D9_6_mem        instr32_D9_6_reg        instr32_D9_7_mem        instr32_D9_7_reg
  instr32_DD_0_mem        instr32_DD_0_reg        instr32_DD_1_mem        instr32_DD_1_reg
  instr32_DD_2_mem        instr32_DD_2_reg        instr32_DD_3_mem        instr32_DD_3_reg
  instr32_DD_4_mem        instr32_DD_4_reg        instr32_DD_5_mem        instr32_DD_5_reg
  instr32_DD_6_mem        instr32_DD_6_reg        instr32_DD_7_mem        instr32_DD_7_reg
  instr_D8_0_mem          instr_D8_0_reg          instr_D8_1_mem          instr_D8_1_reg
  instr_D8_2_mem          instr_D8_2_reg          instr_D8_3_mem          instr_D8_3_reg
  instr_D8_4_mem          instr_D8_4_reg          instr_D8_5_mem          instr_D8_5_reg
  instr_D8_6_mem          instr_D8_6_reg          instr_D8_7_mem          instr_D8_7_reg
  instr_DA_0_mem          instr_DA_0_reg          instr_DA_1_mem          instr_DA_1_reg
  instr_DA_2_mem          instr_DA_2_reg          instr_DA_3_mem          instr_DA_3_reg
  instr_DA_4_mem          instr_DA_4_reg          instr_DA_5_mem          instr_DA_5_reg
  instr_DA_6_mem          instr_DA_6_reg          instr_DA_7_mem          instr_DA_7_reg
  instr_DB_0_mem          instr_DB_0_reg          instr_DB_1_mem          instr_DB_1_reg
  instr_DB_2_mem          instr_DB_2_reg          instr_DB_3_mem          instr_DB_3_reg
  instr_DB_4_mem          instr_DB_4_reg          instr_DB_5_mem          instr_DB_5_reg
  instr_DB_6_mem          instr_DB_6_reg          instr_DB_7_mem          instr_DB_7_reg
  instr_DC_0_mem          instr_DC_0_reg          instr_DC_1_mem          instr_DC_1_reg
  instr_DC_2_mem          instr_DC_2_reg          instr_DC_3_mem          instr_DC_3_reg
  instr_DC_4_mem          instr_DC_4_reg          instr_DC_5_mem          instr_DC_5_reg
  instr_DC_6_mem          instr_DC_6_reg          instr_DC_7_mem          instr_DC_7_reg
  instr_DE_0_mem          instr_DE_0_reg          instr_DE_1_mem          instr_DE_1_reg
  instr_DE_2_mem          instr_DE_2_reg          instr_DE_3_mem          instr_DE_3_reg
  instr_DE_4_mem          instr_DE_4_reg          instr_DE_5_mem          instr_DE_5_reg
  instr_DE_6_mem          instr_DE_6_reg          instr_DE_7_mem          instr_DE_7_reg
  instr_DF_0_mem          instr_DF_0_reg          instr_DF_1_mem          instr_DF_1_reg
  instr_DF_2_mem          instr_DF_2_reg          instr_DF_3_mem          instr_DF_3_reg
  instr_DF_4_mem          instr_DF_4_reg          instr_DF_5_mem          instr_DF_5_reg
  instr_DF_6_mem          instr_DF_6_reg          instr_DF_7_mem          instr_DF_7_reg
```

## 6. Divergences from the upstream Rust (every one)

Porting rules carried in from M1a-2 §5 (D3/D5/D19/D21) are applied without restating them
here; the entries below are the new ones.

| # | Where | Divergence | Why |
| --- | --- | --- | --- |
| D1 | `string.c` | `memory::memcpy_into_svga_lfb` is ported into `memory.c`, not called as an import | it is memory.rs:254's function and memory.c's file; M1a-1 left it out because nothing called it. `map4k`-style testability: the LFB branch is reachable in the selftest by installing a `vga_mem8` — not exercised here, see §10. |
| D2 | `string.c` | the `can_skip_dirty_page`/`skip_dirty_page` bookkeeping is dropped (`string.rs:155, 168, 175, 181, 247-249`) | it exists only to gate `jit::jit_dirty_page`, which memory.c already dropped; kept would be `-Wunused-but-set-variable` under `-Werror`. Control flow unchanged. |
| D3 | `string.c` | the per-page local is `count_until_page_end`; the helper keeps upstream's name `count_until_end_of_page` | C has one namespace: upstream shadows the helper with a local of the same name (string.rs:160 vs 195), legal only because Rust evaluates the initializer before the binding exists. |
| D4 | `string.c` | the `Instruction`/`Size`/`Rep` enums are `STR_*`-prefixed C enums; the `match`es inside the iteration loop are `if/else` chains | C enum constants have file scope; and `break` inside a C `switch` inside the loop would break the switch, not the loop (Rust's `break` breaks the loop) — the if/else form preserves upstream's control flow exactly. |
| D5 | `string.c` | `(a < b ? a : b)` instead of `u32::min`; explicit parens round `(dst + increment) & asize_mask` | Rust's precedence for `&` is below `+` (same as C), so the parens are documentation, not a change. |
| D6 | `paging.h` | `safe_read_write8/16/32` are STATEMENT MACROS taking the operation inline (`SAFE_READ_WRITE8(addr, add8(x_, read_reg8(r)))`) instead of taking a `&dyn Fn(i32) -> i32` closure | C has no closures, and ~120 call sites pass a one-expression lambda. The macro body is upstream's body verbatim, including the *fault returns from the whole instruction* — valid because the call is the entire body of every caller (upstream's `return_on_pagefault!` returns from `safe_read_write8`, whose remaining code is nothing; the effect is identical, and the selftest probes it). `addr_expr` is evaluated twice in the boundary branch, as upstream does. |
| D7 | `paging.h` | `call_interrupt_vector` gains `bool has_error_code` | upstream's `error_code: Option<i32>` controls only whether an error code is pushed (cpu.rs:1051, 1889), and INT3/INT n/INTO pass `None`. M1a-1's bare-`int32_t` declaration could not express "absent" and said "M1c may extend it"; this unit is the first caller that needs the absent case, so the Option is spelled instead of a sentinel a real code could collide with. `paging.c`'s one call site (always present) and the selftest stub were updated. **M1c must define this signature.** |
| D8 | `instructions.c` | the `D8..DF` entry points are not here (§5) | see §5. |
| D9 | `instructions.c` | `run_prefix_instruction` / `segment_prefix_op` (cpu.rs:3366-3376) live here rather than in a run-loop file | their only callers anywhere in the port are this file's prefix instructions; they need nothing but `read_imm8`, `is_osize_32` and `run_instruction`. Declared in `instructions.h` for M1c's `cpu.c`. |
| D10 | `instructions.c` | every `write_regX(reg, return_on_pagefault!(popX()))` is split into a local + a fault check before the write | upstream's `?` returns before the register write; writing first would make a faulting pop corrupt the destination register. Same shape for `push16_reg`/`push32_reg`, the moffs forms and the RMW read forms. |
| D11 | `instructions.c` | sign extension is spelled through `uint32_t` (`SEX8`/`SEX16`) | Rust's `read_reg8(AL) << 24 >> 24` (cbw) and `x << 16 >> 16` (imul8/16) shift into the sign bit, which is UB in C (M1a-2's D5 rule). |
| D12 | `instructions.c` | `instr_F6_2_mem` writes `~x_ & 0xFF` | upstream's `!x & 0xFF` is Rust's BITWISE not; C's `!` is logical (M1a-2's D3 found the same trap in `not8`). `!x_` is also rejected by `-Werror` in this context (measured, §7 M5) — the C spelling is the same bits and compiles. |
| D13 | `instructions.c` | the two dead `if false && …` VME branches in `instr_FA/FB_without_fault` are transcribed **with** their dead condition | upstream keeps the seat (cpu.rs's unimplemented virtual-8086 extension) behind a literal `false`; a later VME unit should find it. It is unreachable and the compiler does not warn. |
| D14 | `instructions.c` | `instr16_8D_mem`/`instr32_8D_mem` do not propagate the fault (`if (!cpu->pagefault) write_regN(...)`) | upstream is `if let Ok(addr) = modrm_resolve(...)`, which deliberately swallows the `Err` — the address fault is still delivered by the flag, and nothing follows but the prefix clear. A rewrite to early-return would be a semantic change. |
| D15 | `instructions.c` | every `let (a, b) = …; if !switch_seg(...) { return }` sequence is written as explicit locals with a fault check between them | e.g. `instr16_FF_3_mem` reads `new_ip` then `new_cs`; upstream's `return_on_pagefault!` short-circuits between them. |
| D16 | `instructions.c` | `dbg_log!`/`dbg_assert!`/`in_jit`/`profiler::*` dropped or `DBG_ASSERT` (M1a-2's D19); `instr_62_*` (BOUND) is `DBG_ASSERT(false)` with `(void)`-ed parameters | upstream's body is `dbg_log!` + `dbg_assert!(false)`: a no-op in release, a panic in debug. Same here (the selftest runs with asserts ON, so it must not call it — stated in §10). |
| D17 | `instructions.c` | `instr_D4/D5` (aam/aad) take the immediate the generator reads; `instr_D6` writes `-(int32_t)getcf()` | `-(getcf() as i32)` has the same value (0 / -1 → 0x00 / 0xFF in the byte); the cast is explicit because `write_reg8` takes `int32_t`. |
| D18 | `misc_instr.c`, `memory.c` | `get_real_eip`, `popa16`/`popa32`, `memcpy_into_svga_lfb` are added to files this unit does not own | each is the cpu.rs/memory.rs function next to the ones that file already owns (`get_stack_reg`, `pusha16/32`, the `*_no_mmap_*` family); the alternative is a duplicate definition later. See F-3/F-4. |

## 7. Findings about the brief, the plan and upstream

* **F-1 — the brief's "base opcodes only (the 0F table is M1b's)" does not partition
  `instructions.rs`.** `D8..DF` are base-table opcodes whose handlers are 360 lines of
  `fpu.rs` calls. The brief names `fxsave/fxrstor` (M1b) and `lar/lsl/verr/verw` (M1c) as
  things "instructions.rs calls" — measured, it calls **none** of them (they are in
  `instructions_0f.rs`/`misc_instr.rs`). What it does call is 81 `fpu_*` functions. Handled
  as §5 describes; the emitter unit and M1b need that list.
* **F-2 — two landed units' omissions surfaced here**, exactly as the "a seat that is never
  called" pattern predicts: `memcpy_into_svga_lfb` (memory.rs:254, M1a-1) and
  `popa16`/`popa32` (cpu.rs:3054-3078, M1a-2 ported `pusha` only) had **no definition
  anywhere** in `src/cpu/`. Both are recorded in `misc_instr.h`/`memory.h` as M1a-3
  additions. Neither would have been caught by reading the port again — only by writing its
  first caller.
* **F-3 — the `safe_read_write*` family did not exist.** M1a-2's F-2 predicted this
  ("M1a-3's `safe_read_write*` will need `virt_boundary_*`"). Confirmed: they are the **only**
  callers of `virt_boundary_read16/read32s`/`virt_boundary_write16/32`, so P-NOT-3's
  correction is now load-bearing and tested (§9, gate 10).
* **F-4 — `get_real_eip` is a cpu.rs helper the brief does not mention** and that
  `instructions.rs` uses 14 times (and M1c's `far_jump`/`call_interrupt_vector`/
  `do_task_switch` use too, cpu.rs:1049-1716). Ported into `misc_instr.c` next to
  `get_stack_reg`, where upstream keeps it.
* **F-5 — the mutation gate caught the compiler, not the test, twice.** Two candidate
  mutations of `string.c`'s iteration limit were *build* failures under `-Werror`
  (`unused variable 'a'/'b'`, `variable 'rep_cmp_finished' set but not used`) — a
  `-Werror`-caught mutation is not asymmetry evidence, and both are recorded as such in
  `results/m1a3-gates.txt` rather than counted. A third form of the same mutation compiles
  and fails 4 checks.
* **F-6 — the first run of my own tests found 12 wrong *expectations* of mine**, all in
  behaviour I had read and mis-derived: the backward-movs range is `[ESI-(n-1), ESI]` (not
  `[ESI, ESI+n)`); `push16` on a 32-bit stack moves SP by **2** and writes 2 bytes
  (`misc_instr.rs:156-161`, not the 32-bit slot I assumed); `sti` reads its opcode *after*
  clearing `prefixes`, so the operand size is 32-bit whatever the 66 prefix. Recorded because
  "the port is wrong" was the wrong first hypothesis each time: the tests are the only thing
  that distinguished the two readings.
* **F-7 — `-Werror` and `dbg_assert` interact**: `instr_62_*` (BOUND) and `instr_F1` (INT1)
  are `DBG_ASSERT(false)`; with the selftest built without `-DNDEBUG` they abort the process,
  so they **cannot be called** from the tests. They are also no-ops upstream in the shipped
  wasm. Stated rather than silently skipped.

## 8. The oracle: what the `--actual` leg must emit, and what this unit cannot do

**Read (not assumed):** `tests/nasm_run.py:328-341` runs `--actual` as a command list with
`{img}`/`{limit}` substituted, requires **exit code 0**, and parses stdout with
`extract_json`; `decode_actual` (nasm_run.py:231-255) then requires a fixture-shaped document
of exactly **2116 array entries**: `reg32[8]`, `eip[1]`, `fpu_regs[8]`, `mmx[16]`,
`xmm[32]`, `memory[2048]` (the 8 KiB window at `BSS = 0x100000`, 4 bytes per entry),
`eflags[1]` (masked with `MASK_ARITH`), one filler, `fpu_status[1]`.

**MEASURED against the `tests/` sources, the requirements this unit's files cannot meet
alone** (they are integration requirements, not defects):

1. **A dispatcher and a loader.** `run_instruction`/`run_instruction0f_16/32` do not exist
   until the emitter unit lands (plan §1 step 12), and nothing here executes an instruction
   stream: `cpu.c`'s run loop, the `eip_phys`/`pagefault` clear-per-instruction protocol and
   the image loader are M1c (step 19) and M0's `main.c`. **This unit cannot pass the
   `--actual` gate**; the brief says so, and it is the reason the gate above is the selftest.
2. **A `f80_t`-typed read of the FPU/MMX/XMM registers** for the document's `fpu_regs`/`mmx`/
   `xmm` slices — the state fields exist (`cpu_state.h`) but the *serialiser* is M0's
   transcript writer / the integration unit; nothing in `string.c`/`instructions.c` writes it.
3. **The 10 skips in `tests/nasm_skip.txt` are pre-existing and reference-side**
   (measured: the reference panics on an empty IDT for `#DE`/`#UD`, or diverges from the
   native fixture). They are **not** this unit's to shrink: they need M1c's
   `call_interrupt_vector`/IDT and M1b's SSE. Reporting them here so the emitter unit does
   not read a skip list as "the C port is fine".
4. **One requirement this unit DOES meet, verifiable now:** the `_mem`/`_reg`/`fixed_g`
   call shapes and names the generated file must print (measured, §3) — the emitter can be
   written against `instructions.h` without guessing.

## 9. Gates (raw output in `results/m1a3-gates.txt`)

**G1 — `make selftest` → RC=0, 2968 checks, 0 failures** (M1a-2: 2415). The ten new
functions and what they pin:

* `test_string_movs` — rep movsb through the fast path (ES is the destination, DS the source,
  the registers advance, ECX drains); **the per-page iteration limit and the
  `instruction_pointer = previous_ip` rewind**, twice (source-bounded and destination-bounded);
  DF=1 with the moved range `[ESI-15, ESI]`; the 16-bit slow path including the wrap; and the
  ASIZE mask applied to the *register read* (upper halves ignored).
* `test_string_cmps_scas` — rep cmps stops at the first difference with the surviving
  comparison's CF/ZF/SF; repnz stops at the first equality; rep scas both ways; the no-rep
  forms leave ECX alone.
* `test_string_stos_lods` — rep stosb/`stosd` with the page-bounded stop, rep lodsw/lodsb
  (the last unit survives), no-rep forms.
* `test_string_ins_outs` — rep ins/outs through the port-I/O seat, the IOPL abort (port and
  registers untouched), the IN/OUT instructions both ways.
* `test_instr_alu_memory` — the RMW memory forms and the 02/03-reversed direction, the
  hoisted-read forms of the same family (12/2B) and the **cmp family's operand order**
  (38/39 are `mem - reg`, 3A/3B are `reg - mem`, so a swap inverts CF — checked both ways),
  the 80/81/82/83 immediate groups (incl. the 16-bit `& 0xFFFF` masking), 84/85, imul with
  an immediate (69/6B, incl. a faulting read leaving the destination register alone), F6/F7
  not/neg/mul/div, the C0/C1/D0-D3 shift groups, inc/dec preserving CF.
* `test_instr_mov_xchg_lea` — xchg both directions, mov r/m↔reg, mov sreg both ways (#ud for
  `seg >= 6`), **lea's SEG_PREFIX_ZERO** (with DS ≠ 0, so the two readings differ), pop r/m16.
* `test_instr_control` — call/jmp/ret/retf, the FF /2,/3,/4,/5,/6 group, int3/int/into
  delivering with **no error code**, iret, and the 0F escape's re-dispatch.
* `test_instr_flags_and_prefixes` — fwait/#NM, pushf/popf (incl. the #gp and the 0xFCFFFF
  image), sahf/lahf, clc/stc/cmc, cld/std, cli/sti (incl. the #gp and the shadow
  re-dispatch), hlt, aam/aad/salc/xlat, cbw/cwde/cwd/cdq, test imm, jcc, the loop family.
* `test_instr_string_opcodes` — the moffs forms, the A4-AF wrappers (incl. a segment override
  reaching the source), **arpl**, the 66/67/F0/F2/F3 prefixes and the segment prefixes
  (observable through the dispatcher stub: the opcode argument and the prefix byte at call
  time).
* `test_instr_rmw_and_faults` — **the page-crossing RMW**: a 16-bit and a 32-bit
  read-modify-write across a virtual boundary whose two physical pages are *non-adjacent*,
  so only `virt_boundary_*` can produce the right value and the right carry; the
  fault-aborts-without-writing case; the `in_mapped_range` branch; the string fast path
  aborting before any register moves; the movs page-bounded stop followed by a faulting
  re-entry; and **the slow path's partial effects** (a mid-instruction fault leaves exactly
  the completed iterations: the data, the register advance, and count − completed).

**G2 — 11 mutations, each applied alone; the test FAILS and the restore is green**
(`results/m1a3-gates.txt`): M1 the ASIZE mask; M2 ES→DS as the destination segment;
M3 the rewind rule; M4 the 16-bit page-boundary test; M5 the F6/2 complement; M6 lea's
SEG_PREFIX_ZERO; M7 sti's prefix clear order; M8 INT n's error-code flag; M9 cbw's sign
extension; M10 arpl's write-back; M11 arpl's protected-mode #ud. Two first attempts
(F-5) were `-Werror` build failures and are **not** counted as asymmetry.

**G3 — `make` → RC=0**, all nine TUs under `-Wall -Wextra -Werror`.
**G4 — `make oracle` → RC=0** (transcript still matches `tests/model.py`, 561 instructions,
`tests/test.img` untouched) and **`make nasm` → RC=0**: `[+] Passed 113/113 tests`,
`0 of 123 selected test(s) produced no state`, 10 skipped (pre-existing, §8.3).

## 10. Coverage gaps, stated rather than implied

* `instr_62_*` (BOUND) and `instr_F1` (INT1) are `DBG_ASSERT(false)` and cannot be called
  from an asserts-on binary (F-7); upstream they are no-ops in the shipped wasm.
* `instr_16/32_07`, `_17`, `_1F` (pop sreg), `_8E_*` (mov sreg) are tested through the
  `switch_seg` stub only — the real segment load is M1c's, so the *selector* plumbing is
  pinned, the segment effects are not.
* `memcpy_into_svga_lfb` (D1) is compiled and reachable but not exercised: the fast path's
  LFB branch needs a `vga_mem8` installed, and the VGA is M2. The branch is guarded by
  `in_svga_lfb`, which is false for every address while `vga_memory_size == 0` — the code
  path is a faithful port of memory.rs:254, and this is the weakest point in this unit.
* `instr_8F_0_mem`'s ESP-restore-on-fault branch and the `ins` slow path's
  `writable_or_pagefault`-before-port-read ordering are ported but not fault-probed (they
  need a page-table fixture per case; the movs/stos fault paths in gate 10 cover the shape).
* The FPU/`ins`/`outs` device semantics are not verifiable here at all (no FPU, no devices).

## 11. Open items handed on

* **The emitter unit** must define `void run_instruction(int32_t opcode)` (§4) and may rely on
  every name and arity in `instructions.h` (§3). It will also need
  `after_block_boundary()` (its own postfix) and, for the 0F table, M1b's
  `run_instruction0f_16/32`.
* **M1b** owns the 160 `D8..DF` entry points (§5 list) and `fwait` (fpu.rs:803).
* **M1c** owns `far_jump`/`far_return`/`iret16`/`iret32`/`handle_irqs`/`call_interrupt_vector`
  (with the widened `has_error_code` signature, D7), `switch_seg`, and the timer/halt
  interface; the `cpu.c` run loop clears `cpu->pagefault` once per instruction (the contract
  these files rely on).
* **The device layer** owns `io_port_*` and `test_privileges_for_io` (cpu.rs:4613-4649,
  3009-3055) — including the PIC's six intercepted ports and the TSS I/O-permission bitmap,
  neither of which is written here.
