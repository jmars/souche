# x86-64: `F3 REX.W 0F B8 /r` = `POPCNT r64, r/m64` — the 64-bit count, and the corpus arms that are RED before it

**Status: landed. `case 0x2B8` is no longer `unimplemented_instruction`, the native corpus carries
two cases that are RED before the change and GREEN after it, and the F3-required rule survives at
64 bits as its own fault case. Claims tagged MEASURED / READ.**

## 1. The instruction, and how the guest named it (MEASURED / READ)

The lane's blocker, from the amd64 boot on the pinned image with `V86C_RING` naming the instruction:

    ring newest boundary eip=0xffffffffa8d2c93d bytes: 49 23 3c f2 f3 48 0f b8 c7 41 01 c0 ...
    v86c: unimplemented: long mode: 0F B8 has no 64-bit arm in gen/x86_table64.js
    (4,598,496,963 instructions; the guest has printed through ITS/FXSAVE and is in a bitmap walk)

`f3 48 0f b8 c7` is `popcnt rax, rdi`. The port implemented the 16- and 32-bit POPCNT bodies
(`instr16_F30FB8_*` / `instr32_F30FB8_*`, `src/cpu/instructions_0f.c:1754`) and had no 64-bit arm,
so the 0F switch's `case 0x2B8` was a named stop.

The F3 rule is the point of the encoding pair, and it is stated in that same source block: the plain
`0F B8` and `0F B9` are **#UD** and only `F3 0F B8` is popcnt. In long mode POPCNT counts **all 64
bits** of the source (SDM: `POPCNT r64, r/m64`), and its flag rule is the same at every width — CF,
PF, AF, ZF, SF and OF are **CLEARED**, and ZF is **SET** when the source is zero.

> **A DIVERGENCE FROM THE BRIEF, MEASURED, NOT RECONCILED.** The brief said "POPCNT writes no flags
> — assert the flags in the fixture are unchanged". That is not what the SDM says, not what the
> port's existing 32-bit `popcnt()` does (`src/cpu/arith.c:1446`: `cpu->flags &= ~FLAGS_ALL`, then
> ZF when `v == 0`), and not what the **host** does — the arm below compares the flags against the
> native leg, which is real hardware, and it is green. So the arm asserts the SDM's rule instead,
> and the "flags left alone" body the brief describes is measured as a **mutant** that fails (§3).

## 2. The change (READ)

| file | change |
|---|---|
| `gen/x86_table64.js` | one 0F-overlay entry, `{ opcode: 0xB8 }`, beside `0xBC`/`0xBD` (bsf/bsr), with the measured guest stop as its stated reason |
| `src/cpu/arith.c` / `arith.h` | `int64_t popcnt64(int64_t v)` — `popcnt()`'s flag path over `__builtin_popcountll`; the **full 64 bits** are counted |
| `src/cpu/instructions_64.c` / `.h` | `instr64_F30FB8_mem` / `_reg` (popcnt: the memory form only READS, the destination register is written) and `instr64_0FB8_mem` / `_reg` (**trigger_ud()**, the plain form's #UD) |
| `src/cpu/interpreter_gen.c` | **REGENERATED** — never hand-edited: `node gen/interpreter_c.js` from the input tables |
| `gen/nasm64_gen.py` | two cases, `alu-popcnt-64` and `alu-popcnt-nof3` (§3) |
| `tests/gate-nasm64.sh`, `tests/gate-long-mode.sh`, `tests/gate-x86-64-diff.sh` | the corpus count lines (86 → 88, alu 15 → 17) and the fault-case exclude/filter lists (§4) |

The generator could express this **without a change**, which is why no generator source moved: an
`{ opcode }` 0F-overlay entry makes `gen_64_arm_0f` run the SAME `gen_instruction_body(encoding, 64)`
that produced the 16/32-bit arms, so the F3 prefix split is emitted at size 64 too — the arm keeps
the rule rather than re-deciding it:

```c
case 0x2B8:
{
	int32_t modrm_byte = read_imm8();
	if (cpu->pagefault) return;
	uint8_t prefixes_ = cpu->prefixes;
	if ((prefixes_ & PREFIX_F3) != 0) {
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr64_F30FB8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_F30FB8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	else {
		... instr64_0FB8_mem / instr64_0FB8_reg ...     /* trigger_ud() */
	}
}
```

**Census, MEASURED both ways** (the emitter prints it on every build). The `long mode:` pair counts
the **BASE** overlay (`x86_table64`), not the 0F one, so it does **not** move; the call-site pair
does:

| | call sites | implemented | unimplemented | names defined | long mode: entries / routed / unimplemented |
|---|---|---|---|---|---|
| before (HEAD) | 2295 | 2253 | 42 | 2257 | 159 / 119 / 42 |
| after | **2299** | **2257** | 42 | **2261** | 159 / 119 / 42 |

The four new call sites are the two F3 bodies and the two #UD bodies, and all four are
**implemented** — that is why the unimplemented count is unchanged. The `tests/gate-long-mode.sh`
census line therefore needed **no** edit; the corpus count lines did (§4).

`gen/interpreter_uop.js` / `uop_gen.c` are deliberately untouched: the uop cache refuses to decode
long mode and delegates the batch to the interpreter (S13, `uop_cache.c`).

## 3. The corpus arms, with the asymmetry MEASURED

Both cases are in the `alu` family of `gen/nasm64_gen.py` and **generated** into
`tests/nasm64/` — no hand-writing into build products.

### 3a. `alu-popcnt-64` — the count, the width, the memory form, and the flags

Disassembled (MEASURED, `objdump -d build/nasm64/alu-popcnt-64.elf`) — the first encoding is the
guest's own `f3 48 0f b8`:

    90030: 48 b8 01 00 00 00 00 00 00 80   movabs rax, 0x8000000000000001
    90041: 48 83 f9 00                     cmp    rcx, 0            (SF=1, PF=1: NONZERO flags before the popcnt)
    90045: f3 48 0f b8 c0                  popcnt rax, rax           <-- 2 at 64 bits, 1 at 32
    9004a: 9c / 8f 04 25 00 02 10 00       pushfq; pop [0x100200]    (must be 0 after the mask)
    90065: 48 bb 03 00 00 00 00 00 00 80   movabs rbx, 0x8000000000000003
    9007a: f3 48 0f b8 db                  popcnt rbx, rbx           <-- 3; result 3 has EVEN parity
    9009a: 48 c7 c2 ff ff ff ff            mov    rdx, -1
    900a1: f3 48 0f b8 d2                  popcnt rdx, rdx           <-- 64 at 64 bits, 32 at 32
    900a6: f3 48 0f b8 3d c1 00 00 00      popcnt rdi, [rip+body_data0]   <-- the r/m64 MEMORY form,
                                                                         0xF0F0F0F0F0F0F0F0 = 32 (16 at 32)
    900b6: f3 4d 0f b8 c8                  popcnt r9, r8             <-- REX.R destination, REX.B source
    900c8: f3 48 0f b8 f6                  popcnt rsi, rsi           <-- source 0: ZF set, everything else cleared

Every intermediate flags word is stored (AF dropped is unnecessary here — POPCNT *defines* all six
bits — so the mask is the full `0x8D5`) and every GPR is stored by the shared epilogue, so the
"the memory form only reads" half and the destination-register writes are both compared.

The flags stores are the **second discrimination**, and they are two separate traps:

- `cmp rcx, 0` on `rcx = -1` leaves **SF=1, PF=1** (0x84). The correct body CLEARS both; a body that
  did not touch the flags keeps 0x84 — measured as mutant B below.
- `0x8000000000000003` has **three** set bits, so a body that derived the flags from the *result*
  (any generic ALU path) would leave **PF=1** from the odd count 3. The SDM's rule leaves it clear.

### 3b. `alu-popcnt-nof3` — the plain `0F B8` must STAY #UD at 64 bits

    90030: 48 b8 00 00 00 00 01 00 00 00   movabs rax, 0x100000000     (the witness: a 2^32 literal)
    9003a: 48 0f b8 c7                     REX.W 0F B8 /r with NO F3 -> #UD

The bytes are written out (`.byte 0x48, 0x0f, 0xb8, 0xc7`) because no assembler will emit the form.
This is the arm that would catch a 64-bit entry that routed the whole `0x2B8` case to the popcnt
body: such a body would **execute**, and the case would stop being a fault. The case's own witness
is the 2^32 literal (a fault's rip is width-independent, so the ENCODING carries the
discrimination — the same rule `ctrl-ud2` states).

### 3c. The asymmetry, before → after (MEASURED, both runs with `--long-mode on`)

| | before the change | after the change |
|---|---|---|
| verdict | `RED alu-popcnt-64: unimplemented: long mode: 0F B8 has no 64-bit arm in gen/x86_table64.js` · `RED alu-popcnt-nof3: <same>` | `green` (2/2) |
| SUMMARY | `total=2 green=0 red_unimplemented=2 mismatch=0 error=0` | `total=2 green=2 red_unimplemented=0 mismatch=0 error=0` |
| exit code | **1** | **0** |

The "before" run was measured by stashing the five source files, regenerating the dispatcher from
the stashed tables and rebuilding HEAD's instruction set (the corpus `.asm` files do not depend on
the source), then restoring with `git stash pop` and regenerating again. The expected values are the
HOST's: the same fixtures through `build/mkfixture64` (the native leg) are green.

### 3d. The mutants — the cases bite a WRONG body, not just a stop (MEASURED)

**Mutant A, "counted the low half only"** — `instr64_F30FB8_*` calling the 32-bit `popcnt()` on
`(int32_t)`:

    mem[100000]  Actual 0x1   Expected 0x2     (rax: popcnt 0x8000000000000001)
    mem[100010]  Actual 0x20  Expected 0x40    (rdx: popcnt -1 = 64, not 32)
    mem[100018]  Actual 0x2   Expected 0x3     (rbx: popcnt 0x8000000000000003)
    mem[100038]  Actual 0x10  Expected 0x20    (rdi: the MEMORY form, 32 not 16)
    mem[100048]  Actual 0x20  Expected 0x40    (r9:  the REX.R/REX.B form)
    rc 1, 1/2 green          (alu-popcnt-nof3 stays green: it does not call the body)

**Mutant B, "the flags left alone"** — the same two bodies calling `__builtin_popcountll` directly,
no `popcnt64`, i.e. the body the brief's flag note describes:

    mem[100200]  Actual 0x84  Expected 0x0     (SF|PF of the preceding `cmp` survive)
    mem[100208]  Actual 0x84  Expected 0x0     (same trap, second site)
    mem[101FF8]  Actual 0x44  Expected 0x40    (the zero-source case: ZF kept, PF NOT cleared)
    rflags       Actual 0x4   Expected 0x0     (the mask store's operand differed -> the AND's parity)
    rc 1, 1/2 green

Body restored; `git diff src/cpu/instructions_64.c` is the intended +30 lines.

## 4. Gates — each by EXIT CODE, one pass at the end

| gate | rc | evidence |
|---|---|---|
| `make` | **0** | `build/v86c` linked, `-Werror`, no `-DNDEBUG` |
| `make selftest` | **0** | `cpu_selftest: 3888 checks, 0 failures` · `cpu_m1c_selftest: 414 checks, 0 failures` · `selftest: all suites green` |
| `make nasm64` | **0** | `gate-nasm64: 13 passed, 0 failed`; the corpus is **88 cases** (was 86), the alu family **17** (was 15) |
| `make transcript-diff` | **0** | `milestone: the C transcript is byte-identical to the reference's` — the 32-bit machine is unmoved |
| `make bench` | **0** | `verified (every run): 200095214 instructions` — **the pin did not move** |

Two gate scripts were edited because the case count rose and because one new case is a FAULT case:

- `tests/gate-nasm64.sh` — the OFF census `red_unimplemented=86 → 88`, the not-red arm
  `green=86 → 88`, and **`popcnt-nof3` added to the `--exclude` of the two perturb arms and to the
  `--filter` of the perturb-rip arm**. That last one is not cosmetic: the perturb arms assert every
  case MISMATCHES on a perturbed register word, and a fault case compares only its rip — the first
  `make nasm64` after the change **failed exactly there** (`gate-nasm64: 11 passed, 2 failed`,
  `perturb-reg`'s SUMMARY `total=78 green=1 mismatch=77`; re-running the gate with the exclude
  reverted reproduces it exactly — `FAIL perturb-reg`, `FAIL perturb-mem`, `11 passed, 2 failed` —
  and MEASURED after the fix by re-running the same arm filtered to `popcnt`: `alu-popcnt-64`
  mismatches on `reg64[0]` as required, while `alu-popcnt-nof3` stays GREEN because a fault case
  compares nothing but its rip). The perturb-rip arm, filtered to the fault cases, does see it:
  `alu-popcnt-nof3` mismatches on `exception rip`. That is the gate doing its job on a case it did
  not know about yet.
- `tests/gate-long-mode.sh` — OFF `86 → 88`, ON `green=86 → 88`, alu family `green=15 → 17`.
- `tests/gate-x86-64-diff.sh` — `green=86 → 88`, and its stale "(63 cases each)" echo corrected to 88.

Both re-run green by exit code: `gate-long-mode: 57 passed, 0 failed` rc 0; `gate-x86-64-diff: 13
passed, 0 failed` rc 0.

## 5. What this does NOT claim

- The amd64 guest was **not** run (the brief reserves it). This unit's evidence is the native
  corpus, whose instrument is this port's own authored tests (plan C1's weaker-instrument caveat) —
  plus the fact that its expected values come from real hardware, which is what settles §1's flags
  divergence.
- The 16-bit form of the same encoding (`66 F3 0F B8`) is untouched: a `0x66` prefix gives
  `dispatch_os_index_0f` key 0, so it keeps running `instr16_F30FB8_*`, as before.
- `0F B9` (the other half of the source comment's "plain 0F B8/B9 are #UD") stays a named stop at
  64 bits. No guest stop and no corpus case reaches it; it is named here so its absence is a gap
  rather than a silence.
- The corpus's `.asm` for `alu-popcnt-64` stores the flags with mask `0x8D5`; POPCNT defines all six
  of those bits, so unlike the AND/OR/XOR/TEST cases (0x8C5) no bit had to be dropped.
