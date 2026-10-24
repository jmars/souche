# x86-64: `REX.W A9 imm32` = `TEST rax, imm32` — the accumulator form, and the corpus case that was already red for it

**Status: landed. `case 0x2A9` is no longer `unimplemented_instruction`, the native corpus carries a
case that is RED before the change and GREEN after it, and the change moves the corpus's LAST red
case to green as well. Claims tagged MEASURED / READ.**

## 1. The instruction, and why the guest is here (MEASURED / READ)

The lane's blocker, from the previous unit's run (results/x86-64-setcc.md §4):

    v86c: unimplemented: long mode: 0xA9 has no 64-bit form in gen/x86_table64.js
    eip=0xa8879d3d (link 0xffffffff810f9d3d), 4,564,309,411 instructions

`A9` is **`TEST AL/AX/EAX/RAX, imm`** — the *accumulator* form, and a **different encoding** from the
`F7 /0` group member this lane already had (`instr64_F7_0`): no ModRM byte, so one byte shorter,
which is why gas picks it for `test rax, imm`. In a 64-bit code segment with REX.W it is
**`TEST rax, imm32`**: AND the FULL 64 bits of RAX with the **sign-extended** imm32, set SF/ZF/PF,
clear OF/CF, leave AF undefined, store nothing. The 16-bit (`0xA9`) and 32-bit (`0x1A9`) arms
already existed; only the `0x2A9` arm was a named stop.

## 2. The file-level change (READ)

Four files, each the smallest change of its kind:

| file | change |
|---|---|
| `gen/x86_table64.js` | one overlay entry, `{ opcode: 0xA9 }`, in the S6 ALU block beside `0x85`/`0xF7`, with the measured guest stop as its stated reason |
| `src/cpu/instructions_64.c` | `instr64_A9(int64_t imm) { test64((int64_t)read_reg64(0), imm); }` — **the same flag path as `instr64_F7_0`** (`test64`, arith.c:441 → `arith_and(..., OPSIZE_64)`); no new flag code, no new AF convention |
| `src/cpu/instructions_64.h` | the declaration, beside `instr64_85_*` |
| `src/cpu/interpreter_gen.c` | **REGENERATED** — never hand-edited: `node gen/interpreter_c.js` from the input tables |

The regenerated arm, in the generator's own shape (`read_imm32s` = the sign-extending read, exactly
as the sibling accumulator forms `0x05`/`0x25`/`0x3D` use):

```c
case 0x2A9:
{
    int64_t imm = read_imm32s();
    if (cpu->pagefault) return;
    instr64_A9(imm);
}
```

The generator could express this without a change: an `{ opcode }` overlay entry emits `case 0x2NN:`
calling the derived `instr64_<hex>` name, and `gen_read_imm_call` already answers `read_imm32s()`
for a non-`imm64` `imm1632` encoding at operand size 64. The emitter's own census moved
**158 entries / 118 routed / 42 unimplemented call sites → 159 / 119 / 42** (MEASURED: the arm was
a bare NAMED-STOP case before, not an unimplemented call site, so only the first two numbers rise).

`gen/interpreter_uop.js` / `uop_gen.c` are deliberately untouched: the uop cache refuses to decode
long mode and delegates the batch to the interpreter (S13, `uop_cache.c`).

## 3. The corpus arm, with the asymmetry MEASURED

Added to the case table in `gen/nasm64_gen.py` (`alu` family) and **generated** into
`tests/nasm64/alu-test-rax-imm32.asm` — no hand-writing into build products. Its body, disassembled
(MEASURED, `objdump -d build/nasm64/alu-test-rax-imm32.elf`), is three A9 encodings and one F7
contrast — A9 is RAX-only, so only RAX can reach it:

    48 b8 00 00 00 00 01 00 00 00   movabs rax, 0x100000000
    48 a9 ff ff ff ff               test   rax, -1              <-- 64-bit: ZF=0 (32-bit: EAX=0, ZF=1)
    48 b8 00 00 00 80 00 00 00 00   movabs rax, 0x80000000
    48 a9 00 00 00 80               test   rax, 0xFFFFFFFF80000000  <-- 64-bit: 0, ZF=1 (32-bit: 0x80000000, ZF=0)
    48 b8 00 00 00 00 00 00 00 80   movabs rax, 0x8000000000000000
    48 a9 ff ff ff ff               test   rax, -1              <-- 64-bit: SF=1 (32-bit: EAX=0, SF=0)
    48 bb 88 77 66 55 44 33 22 11   movabs rbx, 0x1122334455667788
    48 f7 c3 00 00 00 80            test   rbx, 0xFFFFFFFF80000000  (the F7 /0 contrast, not A9)
    48 b8 ... 48 a9 00 00 00 80     test   rax, 0xFFFFFFFF80000000  (the compared final flags)

Each test's flags are stored into the compared window (AF masked off: `0x8C5`, because AF is
UNDEFINED after TEST — the same mask the sibling `alu-test-imm32-64` case uses), and every RAX above
is stored too, so TEST's "stores nothing" is part of what is compared.

**The asymmetry, before → after (MEASURED, both runs with `--long-mode on`):**

| | before the change | after the change |
|---|---|---|
| verdict | `RED alu-test-rax-imm32: unimplemented: long mode: 0xA9 has no 64-bit form in gen/x86_table64.js` | `green` |
| SUMMARY | `total=1 green=0 red_unimplemented=1 mismatch=0 error=0` | `total=1 green=1 red_unimplemented=0 mismatch=0 error=0` |
| exit code | **1** | **0** |

The expected values are the HOST's: the same fixture run through `build/mkfixture64` (the native
leg) is `green=1 red_unimplemented=0 mismatch=0 error=0`, rc 0.

**The second arm — the case bites a WRONG body, not just a stop (MEASURED).** With `instr64_A9`
mutated to the 32-bit body, `test32((int32_t)read_reg64(0), (int32_t)imm)`, the case MISMATCHES
(rc 1) on six fields, and the fields are exactly the three discrimination points plus the final
flags slot:

    mem[100200]  0x44 vs 0x4     (PF|ZF vs PF:      test rax,-1 on 0x100000000)
    mem[100208]  0x84 vs 0x4     (SF|PF vs PF:      test rax,0xFFFFFFFF80000000 on 0x80000000)
    mem[100210]  0x44 vs 0x84    (PF|ZF vs SF|PF:   test rax,-1 on 0x8000000000000000)
    mem[100080]  0x44 vs 0x4     (the compared final flags)

Body restored; `git diff src/cpu/instructions_64.c` is the intended +12 lines.

**A FINDING this unit did not expect, MEASURED: the corpus had ALREADY named this gap.** The
whole corpus was 84 green + **1 red** before this change, and the red case was
`alu-test-imm32-64` — S6's own `test rax, 0xFFFFFFFF80000000`, which gas encodes as `48 a9`.
Measured directly, by rebuilding HEAD's instruction set (the three source files stashed, the
dispatcher regenerated from the stashed tables) and running the corpus with `--long-mode on`:

    RED alu-test-imm32-64: unimplemented: long mode: 0xA9 has no 64-bit form in gen/x86_table64.js
    RED alu-test-rax-imm32: unimplemented: long mode: 0xA9 has no 64-bit form in gen/x86_table64.js
    [+] SUMMARY: total=86 green=84 red_unimplemented=2 mismatch=0 error=0      (rc 1)

so the pre-existing red case is the same encoding, and the corpus's red census at HEAD was 1 of 85.
A scan of every `build/nasm64/*.elf` for a `48 a9` encoding finds **exactly two** cases: that
pre-existing one (1 occurrence) and this unit's new one (4). REX.W A9 therefore turns the corpus's
last red case green as well: **84+1 → 86+0** (MEASURED, `tests/gate-long-mode.sh` step E).

The excursion was restored before the gates below: `git stash pop` returned the three files,
`node gen/interpreter_c.js` regenerated the dispatcher, and `cmp` proves
`src/cpu/interpreter_gen.c` is **byte-identical** to the state the gates ran on (`git diff --stat`
is the same 12-file list, of which three are foreign dirty files this unit never touched).

## 4. Gates — each by EXIT CODE, one pass at the end

| gate | rc | evidence |
|---|---|---|
| `make` | **0** | `build/v86c` linked, `-Werror`, no `-DNDEBUG` |
| `make selftest` | **0** | `cpu_selftest: 3888 checks, 0 failures` · `cpu_m1c_selftest: 414 checks, 0 failures` · `selftest: all suites green` |
| `make nasm64` | **0** | `gate-nasm64: 13 passed, 0 failed`; the corpus is **86 cases** (was 85) and the OFF census is `86 x unimplemented: EFER.LME asks for IA-32e ...` |
| `make transcript-diff` | **0** | `milestone: the C transcript is byte-identical to the reference's` — the 32-bit machine is unmoved |
| `make bench` | **0** | `verified (every run): 200095214 instructions` — **the pin did not move** |

Two gates were edited because the case count rose (a slice that lands a case edits the counts):
`tests/gate-nasm64.sh` (85→86 in the OFF census and the not-red arm), `tests/gate-long-mode.sh`
(85→86 OFF, 84+1→86+0 with the flag ON, alu family 13+1→15+0, the overlay census line), and
`tests/gate-x86-64-diff.sh` (84+1→86+0). Both re-run green by exit code: `gate-long-mode: 57 passed,
0 failed` rc 0; `gate-x86-64-diff: 13 passed, 0 failed` rc 0.

## 5. What this does NOT claim

- The amd64 guest was **not** run (the brief reserves it). This unit's evidence is the native
  corpus, whose instrument is this port's own authored tests (plan C1's weaker-instrument caveat).
- The `0x2A8`/`0xA8` (`TEST AL, imm8`) sibling stays a named stop: no corpus case and no guest stop
  reaches it.
- `tests/nasm64/alu-test-imm32-64.asm`'s own witness text still describes only the F7 width rule;
  it is now also the second user of the A9 body, which §3 measures rather than assumes.
