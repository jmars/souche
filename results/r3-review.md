# R3 review — gen/interpreter_c.js vs v86/gen/generate_interpreter.js

**Verdict: no blockers, no should-fix transcription defects.**

The C emitter is a faithful transcription of the upstream generator. Every function the
brief named was read line-by-line against upstream, and its emitted output was compared
shape-for-shape against **upstream's own generated Rust** (regenerated in a sandbox from the
shared `x86_table.js` — v86 does not check the generated files in, so this was the only way
to diff emitted shapes against the reference's actual output).

| upstream function | verdict |
| --- | --- |
| `gen_read_imm_call` (38-77) | identical: the `size = (os \|\| opcode%2==1) ? v : 8` rule, the imm8/imm8s/immaddr/imm1632-imm16-imm32 branch structure with the same asserts |
| `make_instruction_name` (87-101) | identical scheme, incl. the prefix asserts and the `module::` split (C strips the module for one flat namespace — no base encoding carries a 0F/66 prefix, so no collision) |
| `gen_instruction_body` (104-183) | prefix classification, the F2/F3-on-non-0F and 66-on-non-0F asserts, the if/else-if/else chain, the else-arm `DBG_ASSERT((prefixes_ & ...) == 0)` + no-prefix body — all identical. The `uses_modrm` gate is a documented availability-aware deviation; its only observable effect is on STOP paths (see N2-adjacent note below) |
| `gen_instruction_body_after_prefix` (185-232) | the `fixed_g` switch on `modrm_byte >> 3 & 7`, grouping/sorting identical, `default: trigger_ud()` (the `dbg_log!` is dropped, as everywhere) |
| `gen_instruction_body_after_fixed_g` (234-337) | the `_mem`/`_reg` split at `modrm_byte < 0xC0`, **resolve-before-immediate order** (the unit's found bug stays fixed), `custom_modrm_resolve`/`ignore_mod`/`fixed_g` argument lists identical, `extra_imm8/16` threading identical, `task_switch_test[_mmx]` placement identical, `after_block_boundary()` placement identical (incl. the inside-the-case placement for 0F01 and the per-entry placement for prefix groups) |
| `gen_table` (339-450) | the `opcode \| (is_32 << 8)` switch, the `os` → `0xNN`/`0x1NN` split, label pairing, the 0F runner, `default: DBG_ASSERT(false)` — identical |

## The argument-evaluation-order class (what the brief said to hunt)

Verified dead on every path:

- **mem branch**: `modrm_resolve` first, then the immediate — matching Rust's left-to-right
  argument evaluation (`instrX_mem(match modrm_resolve... {...}, ..., match read_imm...)`).
  Checked live in 0x80/0x81, 0xC7, 0x8F, 0xF6, 0xA4, 0F01, 0F10, 0FB6 and forced-live in
  0F71/0FB8.
- **reg branch**: immediate read per-branch, after the (no-op) register operand — same order.
- **non-e branch**: imm then extra_imm8/16 — same order (C8, 9A, EA compared live).
- No `custom_modrm_resolve` entry carries an immediate in the whole table (only 0x8D/0x8F),
  so the raw-`modrm_byte` pass cannot collide with an imm read.

## The unexercised paths, forced live

The unit's stated risk was that `custom_modrm_resolve`, `ignore_mod`, `fixed_g`+immediate,
`extra_imm8/16` and the F2/F3 chains take the STOP path, so no test validates the emitted
call shape. Two answers:

1. `extra_imm8/16` and `custom_modrm_resolve` **are already live** in the checked-in file
   (C8, 9A, EA, 8D, 8F are implemented base opcodes) and match upstream exactly.
2. For the STOP-only 0F shapes I injected **57 stub definitions** into a sandbox copy and
   regenerated. The forced-live shapes for ignore_mod (0F20-23), no-e block_boundary
   (0F24-27), fixed_g (0F01), and the F2/F3/66 chains (0F10, 0F7C, 0FB8) are
   line-equivalent to upstream's generated Rust, and the regenerated file compiles clean
   under `cosmocc -std=gnu11 -O2 -Wall -Wextra -Werror`.

The sandbox regeneration is **byte-identical** to the checked-in `interpreter_gen.c`
(reproducibility gate holds). The Makefile rule's prerequisites
(`$(GENERATOR) $(X86_TABLE) $(CPU_SRC) $(CPU_HDR)`, Makefile:90) make a stale generated
file impossible — adding an instruction regenerates the dispatcher.

## The brief's three runtime questions

(a) **Prefix combination hitting the fallthrough arm**: `DBG_ASSERT((prefixes_ & ...) == 0)`
then the no-prefix body. Upstream's `dbg_assert!` is compiled out of the shipped wasm, so the
reference *executes* the unprefixed encoding; the C build ships without `-DNDEBUG`
(Makefile:25), so the assert **aborts**. See N1.

(b) **`fixed_g` modrm group value out of range**: `default: trigger_ud()` →
`raise_exception(CPU_EXCEPTION_UD)` (exec_boundary.c:54-73) — a *named* stop with eip reset
to `previous_ip`. Loud. ✓

(c) **Unimplemented opcode dispatched**: `unimplemented_instruction(name)` sets
`interpreted_unimplemented` (cpu.c:49-54), consumed in `interpreted_run` (cpu.c:101),
`do_many_cycles_native`, and `main_loop`; main.c reports the symbol name. Every site is
named, nothing continues with stale state. ✓

Prefix machinery on the dispatch path verified live: `instr_66/67/F2/F3`
(instructions.c:615-620) set the bits, `run_prefix_instruction` re-dispatches, then clears —
identical to cpu.rs:3366-3376; `instr16_0F/instr32_0F` (instructions.c:178-189) =
instructions.rs:123-124 verbatim; `run_instruction0f_16/32` (instructions_0f.c:31-32) =
cpu.rs:3132-3133 verbatim. The generated prefix arms therefore actually fire: the
REP-string group 0xA4 is emitted line-for-line like upstream and is live.

## Findings

### NICE-TO-HAVE N1 — the prefix-fallthrough assert aborts where the reference executes

`gen/interpreter_c.js:364` (vs generate_interpreter.js:158-163). A guest executing an
encoding whose prefix the table does not define (e.g. `F2 0F B8`, `F3 0F 71`) falls through
to the no-prefix body in the shipped reference wasm, but aborts the shipped C binary
(assertions on: CFLAGS has no `-DNDEBUG`). It is a loud stop, it matches upstream *debug*
builds and the port's documented `DBG_ASSERT` policy (cpu_state.h:30-36) — but it is
reachable from ordinary guest code and diverges from the pinned reference on that input.
Options: ship the APE with `-DNDEBUG` (making `DBG_ASSERT` match the reference it is
compared against), or accept and document the reachable case at the assert site. The
latter is recommended; either is defensible.

### NICE-TO-HAVE N2 — dead pagefault check in the custom_modrm_resolve mem branch

`gen/interpreter_c.js:247-249` emits `if (cpu->pagefault) return;` unconditionally when
implemented, but for `custom_modrm_resolve` entries (0x8D, 0x8F) no faultable call precedes
it in that branch (`modrm_resolve` is skipped, the raw `modrm_byte` is passed). Harmless
today (the dispatcher clears `pagefault` once per instruction, cpu.c:97), but it reads as
if it guarded the omitted resolve and would mask a fault if a future edit moved a faultable
read above it. Gate the emission on `!encoding.custom_modrm_resolve`, or comment it. Fold
into the next regeneration; not worth churning the generated file alone.

### Notes (not findings)

- 0F 24/25/26/27 dispatch **without consuming a modrm byte**, exactly like upstream
  (`instr_0F24()` = `undefined_instruction` → #UD, then `after_block_boundary()`). When M1b
  lands them, the stub must be `void instr_0F24(void)` — the emitter (proved by injection)
  produces exactly that shape.
- `static void instr_660FEF` (instructions_0f.c:116) is invisible to the availability scan
  (`^void` regex) — correct, since its signature differs from the emitter's convention; the
  public `instr_660FEF_mem/_reg` carry the convention and are picked up. Any future
  `static void instr*` helper is likewise invisible by design.
- The 28 base-table prefix entries are all REP-string forms (F2/F3 6C-6F, A4-A7, AA-AF);
  these are the only base-table prefix arms, and 0xA4's emitted shape is upstream-identical.
- `pxor` (66 0F EF) is fully live — scan-visible `_mem/_reg` forms, sse guard included — and
  is exercised by every nasm test's shared preamble.

## Coverage

- **Read in full**: `gen/interpreter_c.js` (508 lines), `v86/gen/generate_interpreter.js`
  (497 lines).
- **Sampled against upstream's regenerated Rust**: 0x80/0x81, 0xC7, 0x8F, 0xF6, 0x8D, 0xC8,
  0x9A, 0xEA, 0xA0-A3, 0x0F/0x10/0x11, 0xA4, 0F01, 0F71, 0F10, 0F7C, 0FB8, 0F20-27,
  0FB6/B7/BE/BF.
- **Table invariants** (scripted): group homogeneity in `os`/`fixed_g`; the mixed-`e` groups
  are exactly 0F7C/0F7D; no 0F38/0F3A entries; prefix asserts cannot misfire.
- **Stub experiment**: 57 injected definitions force all STOP-only shapes live; regenerated
  output is line-equivalent to upstream and compiles under `-Werror`.
- **Not checked**: the 839 implemented instruction *bodies* (R2's surface), D8-DF
  (deferred), PAE (trapped), the nasm/oracle gates (explicitly not re-run).

File sha256: `gen/interpreter_c.js`
3ee0c23f7c3e33de97312f3ff207f92068039404f3d6f563b6516820084018e6 ·
`v86/gen/generate_interpreter.js`
03086327194b672f3041399279234bc26c535cd2304452514d54b6f706f4931b ·
`src/cpu/interpreter_gen.c`
bb53cf87e7969d27277e6d7c549bf7e1a6571b0b826386b32dfcb87e693c6f93 ·
`src/cpu/instructions_0f.c`
0947a2537e08c8b7e38e866cb746e7f1127a0bbec3f3765f2fe15c5126baae0c ·
`src/cpu/cpu.c`
b4bd2dde9ad6467eb0da22793080c34a00028693d88df6841ac717f33c59c501
