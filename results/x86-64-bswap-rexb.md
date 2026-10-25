# x86-64: REX.B on `0F C8+r` BSWAP — the eight bodies that byte-swapped ESP, and the class sweep

**Status: landed, gates green, the guest was NOT re-run (the orchestrator owns that run).** The
amd64 guest's measured `rsp` jump — `rsp_in=0xffffd1fac056ba58` → `rsp_out=0x58ba56c0` inside ONE
instruction slot, `deliv=0` — is `bswap32(0xc056ba58)`, and the guest's own text at that rip is
`41 0f cc` = `bswap %r12d`. The port executed `bswap %esp`. The eight 16/32-bit `0F C8-CF` bodies
hard-coded `EAX`..`EDI`; they now go through `reg_index_b(n)` like every other opcode-implied
register. The class sweep below is the part that matters beyond this instance: it names every
"+r" family, its reachability in long mode, and the ONE in-class hit this unit did **not** fix
(`41 90`, §4.1).

Every claim is tagged **MEASURED** (this tree, this run), **READ** (a source file I read),
**INTERPRETATION**, or **PROJECTION**.

---

## 1. The measurement that named the defect (MEASURED by the orchestrator, quoted)

One amd64 boot with the new `V86C_RSPJUMP` tool (results/x86-64-rspjump-tool.md):

    RSPJUMP 5092 rip=0xffffffffa8d3df65 rsp_in=0xffffd1fac056ba58 rsp_out=0x58ba56c0
                 delta=-0xffffd1fa679c6398 deliv=0
      r12=0x0 r13=0x0 ... r4=0x58ba56c0   cr2_64=0xffff8eaa88a00000

Three readings, each from the line itself:

* `deliv=0` — no interrupt was delivered in that slot, so an INSTRUCTION moved `rsp`.
* `bswap32(0xc056ba58) == 0x58ba56c0` — the low half of `rsp` was BYTE-SWAPPED in place, and the
  high half was zeroed (that is `write_reg32`'s zero-extension, i.e. the port's 32-bit register
  write).
* the guest's patched text at `0xffffffffa8d3df65` is `41 0f cc`.

So the guest and the port disagreed about which register `41 0F CC` writes. The guest is right.

## 2. The defect (READ)

`src/cpu/instructions_0f.c:1934-1941`, before this unit:

```c
void instr_0FC8(void) { bswap(EAX); }   ...   void instr_0FCC(void) { bswap(ESP); }
```

Hard-coded register lvalues, no `reg_index_b`. The chain that reaches them in long mode (READ):

1. `dispatch_os_index_0f` (src/cpu/modrm.c:683-691): in 64-bit mode, REX.W → 2, `0x66` → 0,
   otherwise **1**. `41 0F CC` has REX.B and no REX.W, so it answers 1.
2. The generated dispatcher's `case 0x1CC:` (src/cpu/interpreter_gen.c:17989-17991) calls
   `instr_0FCC()` — the 32-bit body. Only `case 0x2CC:` (REX.W) reaches `instr64_0FCC()`, which
   already used `reg_index_b` (src/cpu/instructions_64.c:1811).
3. `bswap(int32_t r)` (src/cpu/misc_instr.c:699) is `write_reg32(r, bswap32(read_reg32(r)))`, so
   the write zero-extends into `rsp`'s high half — exactly the measured `0x58ba56c0`.

**Why the class rewrite missed it.** results/x86-64-idt64.md §2.7 enumerated the class as three
families — `instr_B0..B7`, `instr16/32_B8..BF`, `instr16/32_91..97` — and declared `0x50-0x5F` safe
because the overlay's `default64` entries route them to the 64-bit bodies. That reasoning is
**base-space** reasoning: it holds because `dispatch_os_index` consults `long_mode_default64`, whose
list is the base space's. The 0F space has **no** default-64 group and its own dispatch entry point
(`dispatch_os_index_0f`, modrm.c:676-682, which says so in its own comment), so every 0F `+r`
family's 16/32-bit arm is reachable in long mode. `0F C8-CF` is the only such family in the 0F
space. INTERPRETATION: this is the generalisable lesson — the class enumeration was a list of
BASE-space families, and it was read as a list of families.

## 3. The fix (MEASURED by the corpus, §5)

Eight bodies, one token each, in the file's own vocabulary — `bswap()` already takes a register
INDEX (misc_instr.c:699), so no new helper was introduced:

```c
void instr_0FC8(void) { bswap(reg_index_b(0)); }   ...   void instr_0FCF(void) { bswap(reg_index_b(7)); }
```

* **In 32-bit mode `reg_index_b` is the identity** (src/cpu/modrm.h:73:
  `r | ((cpu->prefixes & PREFIX_REX_B) ? 8 : 0)`, and in legacy/compat mode `0x41` is `INC ECX`,
  never a REX byte), so the 32-bit machine is bit-identical. The proof is §6's pins:
  `make transcript-diff` is byte-identical and `make bench` still reads **200,095,214**.
* The **16-bit operand form** (`66 0F CC`) reaches the same body through `case 0xC8`. The SDM
  leaves BSWAP's result **undefined** for a 16-bit operand size, so there is no correct behaviour
  to implement; the 32-bit swap is kept (which is also what hardware does in practice). This is
  also why there is **no** 16-bit corpus arm: the corpus admits only outcomes the SDM states.
* The **uop/uopjit engines get the fix for free**: the uop descriptor table calls the same bodies
  (src/cpu/uop_gen.c:1117-1121), and long mode never reaches the JIT — every long-mode batch
  delegates to the interpreter (src/cpu/uop_cache.c:1332-1336, READ).

## 4. The class sweep (READ + one MEASURED arm)

The question asked of every hit: **is the hard-coded register one the OPCODE encodes in its low
three bits, on a path reachable with REX.B live?** Reachability was read from the generated
dispatcher and the two dispatch-key functions, not guessed.

### 4.1 Opcode-implied "+r" families — the class proper

| opcode family | 16/32-bit bodies | REX.B | reachable in long mode with REX.B? | verdict |
|---|---|---|---|---|
| `0x40-0x4F` INC/DEC r32 | `instr32_40..4F`, hard-coded | dropped | **NO.** `dispatch_os_index` returns 2 for `0x40-0x4F` *before* any prefix test (modrm.c:657-659) → `case 0x240..0x24F` → `long_mode_rex_prefix` (interpreter_gen.c:1698-1711) | NOTE (unreachable) |
| `0x50-0x5F` PUSH/POP r | `instr32_50..5F`, hard-coded | n/a | **NO.** `long_mode_default64` (long_mode_gen.c:21-36) → `instr64_5x` with `reg_index_b` (instructions_64.c:830-887); the `0x66` form is a NAMED STOP in long mode (interpreter_gen.c:1944-1952) | NOTE (unreachable, and guarded by name) |
| `0x90` XCHG/NOP | `instr_90(void) {}` | n/a | **YES.** `41 90` is `xchg r8d, eax`; the port does nothing (and `48 90`, a legal NOP, is a named stop at interpreter_gen.c:4691-4694) | **BLOCKER — in class, NOT fixed here** (see below) |
| `0x91-0x97` XCHG r, eax | `instr16/32_91..97` → `reg_index_b(n)` | kept | YES | NOTE (already correct; MEASURED by the existing `ctrl-rexb-implied-register`) |
| `0xB0-0xB7` MOV r8, imm8 | `instr_B0..B7` → `reg_index_b(n)` (instructions.c:1414-1421) | kept | YES | NOTE (already correct) |
| `0xB8-0xBF` MOV r32, imm32 | `instr32_B8..BF` → `reg_index_b(n)` (instructions.c:1423-1437) | kept | YES | NOTE (already correct) |
| `0x0F C8-CF` BSWAP r32 | `instr_0FC8..CF`, hard-coded | dropped | **YES** — `dispatch_os_index_0f` answers 1 with no REX.W | **BLOCKER — FIXED (§3, MEASURED §5)** |

**§4.1's one live hit, stated plainly.** `41 90` (`xchg r8d, eax`) is reachable in long mode, and
`instr_90` is an empty body, so the port silently executes a no-op where hardware swaps `eax` and
`r8d`. It is the SAME class (an opcode-implied register dropped when REX.B is present), but it is
**not one of the eight bodies this unit was briefed to fix**, and it is the REFERENCE's own
behaviour — v86 `src/rust/cpu/instructions.rs:973` is `pub unsafe fn instr_90() {}`. Fixing it is a
divergence from the reference and therefore needs its own corpus arm (the tree's rule: no fix
without an instrument that fails before it), so it is reported here rather than patched:
`instr_90` should become `if (cpu->prefixes & PREFIX_REX_B) xchg32r(reg_index_b(0));` with a
`ctrl` corpus case carrying `41 90` plus a value in `r8d` and `eax`. **Recommendation: its own
unit.** Severity: it cannot move `rsp`, so it is not the guest's blocker, but it is silent-wrong.

### 4.2 The modrm class — swept mechanically, uniform

Every register taken from a MODRM byte is extended by REX.B at the point of use, in all three
arms, and this was checked two ways: `rg -c "reg_index_b\(modrm_byte & 7\)"
src/cpu/interpreter_gen.c` → **860** call sites, and **every** `_reg(` call in the whole generated
dispatcher passes `reg_index_b` and/or `reg_index_r` (a filtered listing returns nothing) — so
there is no site left where a modrm-selected register reaches a body raw. This covers SETcc
(`0F 90-9F`), CMOVcc (`0F 40-4F`, all three operand sizes), MOVZX/MOVSX, the bit-test group,
SHLD/SHRD, the 0F 00/01/20-23 groups, `8F /0`, `FF /6`, `C6/C7 /0` and the x87/MMX/SSE arms' r/m
operands. NOTE — no defect.

### 4.3 Hard-coded registers that are NOT the class (the encoding fixes them; REX.B does not apply)

`0x05/0x0D/0x15/0x1D/0x25/0x2D/0x35/0x3D` and `0xA9` (accumulator-immediate ALU forms — no register
field at all, so REX.B has nothing to extend); `0x98/0x99` (CBW/CWDE/CDQ/CQO); `0F 31` RDTSC,
`0F A2` CPUID, `0F 30/32` WRMSR/RDMSR, `0F 34/35` SYSENTER/SYSEXIT (the `ESP/ECX/EDX` those bodies
name are the SDM's fixed operands), `0F C7 /6` CMPXCHG8B/CMPXCHG16B; SHLD/SHRD's `CL`; the string
ops' implicit `RSI/RDI/RCX/AL`. (`0F 05/07` are `undefined_instruction()` in this port, so they have
no register to drop.)
All NOTE — correct as written, and a `reg_index_b` there would be a defect.

### 4.4 What this sweep did NOT cover (stated, not implied)

* **The SSE/MMX register file** (`xmm8-15`/`mm` via REX.B). Those registers are selected through
  MODRM (§4.2's mechanism), so they are not opcode-implied; the SIMD family's own corpus cases
  (`simd-*`) are the instrument, and this unit did not re-audit them.
* **The x87 `ST(i)` selection** — REX is ignored by x87, and the FPU stack is not a GPR.
* **The JIT/codegen paths** — long mode delegates to the interpreter (uop_cache.c:1332-1336), so
  there is no second implementation of these bodies to audit.
* **The 32-bit machine's behaviour** beyond `reg_index_b`'s identity: that is asserted by the
  transcript/bench pins (§6), not re-derived case by case.

## 5. The corpus arm and its asymmetry (MEASURED)

New case `ctrl-rexb-bswap` in `gen/nasm64_gen.py` (family `ctrl`, slice S7), generated into
`tests/nasm64/ctrl-rexb-bswap.asm`:

```asm
    mov r12d, 0xC056BA58      # the guest's measured low half
    mov r13d, 0x11223344
    mov eax, 0xDEADBEEF
    mov esp, 0xCAFEBABE       # rsp = 0x00000000CAFEBABE -- the witness that must NOT move
    bswap r12d                # 41 0F CC -- the guest's own instruction
    mov qword ptr [0x100200], rsp   # rsp IMMEDIATELY after it
    bswap r13d                # 41 0F CD -- the same rule, one opcode on
    bswap eax                 # 0F C8, no REX: the plain form must not regress
    cmp rax, rax              # the rflags element's flag-defining last instruction
```

The mid-body `rsp` snapshot is deliberate: with the defect, a SECOND bswap on `esp` would swap
`0xBEBAFECA` back to `0xCAFEBABE` and hide the damage at the end of the body. Encodings MEASURED
by `objdump -d --section=.body` on the assembled image: `41 0f cc` at body+0x46, `41 0f cd` at
+0x51, `0f c8` at +0x54.

**RED before (the fix reverted in the source, `touch`ed, rebuilt, `make` rc 0)** —
`python3 tests/nasm64_run.py --stage run --actual "build/v86c-nasm64 {img} {limit} --fixture64
--long-mode on" --filter rexb-bswap --expect counts --expect-counts
"green=1 red_unimplemented=0 mismatch=0 error=0"` → **rc 1**:

    [+] SUMMARY: total=1 green=0 red_unimplemented=0 mismatch=1 error=0
    [-] ctrl-rexb-bswap.img: 7 field(s)
        reg64[4] (rsp)   Actual: 0xbebafeca   Expected: 0xcafebabe
        reg64[12] (r12)  Actual: 0xc056ba58   Expected: 0x58ba56c0
        reg64[13] (r13)  Actual: 0x11223344   Expected: 0x44332211
        mem[100020]      Actual: 0xffffffffbebafeca  Expected: 0xffffffffcafebabe
        mem[100060]      Actual: 0xffffffffc056ba58  Expected: 0x58ba56c0
        mem[100068]      Actual: 0x11223344          Expected: 0x44332211
        mem[100200]      Actual: 0xffffffffbebafeca  Expected: 0xffffffffcafebabe
    [GATE FAILED] counts ...

`0xbebafeca == bswap32(0xcafebabe)` — the same shape as the guest's measured
`0x58ba56c0 == bswap32(0xc056ba58)`, and `r12` is left untouched. Both witnesses fire, and the
`bswap eax` field (`reg64[0]`, `mem[100000]`) is NOT in the mismatch list, which is the
no-regression half of the arm: the plain form was already correct.

**GREEN after (fix restored, `touch`ed, rebuilt)** — same command → **rc 0**:
`SUMMARY: total=1 green=1 red_unimplemented=0 mismatch=0 error=0`.

## 6. Gates (ONE pass, at the end, judged by EXIT CODE; edited sources `touch`ed first)

| gate | result |
|---|---|
| `make` | **rc 0** |
| `make selftest` | **rc 0** — `cpu_selftest 3888`, `cpu_m1c_selftest 504`, `hw_selftest 975`, `uop_cache_selftest 7,934,363`, `uop_exec_selftest 102`, `jit-selftest 68`, `jit_codegen_selftest 3570`; `selftest: all suites green` |
| `make nasm64` | **rc 0** — `gate-nasm64: 13 passed, 0 failed`; red census `green=0 red_unimplemented=92 mismatch=0 error=0` (91 → 92 with the new case) |
| `make transcript-diff` | **rc 0** — "the C transcript is byte-identical to the reference's" |
| `make bench` | **rc 0** — 7/7 runs `instr=200095214 state=ok serial=ok jit-inline=ok`; **the pin did not move** |
| `sh tests/gate-long-mode.sh` | **rc 0** — `58 passed, 0 failed` (57 → 58 with the new named step `ctrl-rexb-bswap`); whole corpus `green=92 red_unimplemented=0 mismatch=0 error=0`; ctrl family `20 → 21` |
| `sh tests/gate-x86-64-diff.sh` | **rc 0** — `13 passed, 0 failed`; all three engines agree with the host on all 92 cases |

Gate count lines updated (and why): `tests/gate-nasm64.sh` 91→92 (twice),
`tests/gate-long-mode.sh` 91→92 (twice) + ctrl family 20→21 + the new named step,
`tests/gate-x86-64-diff.sh` 91→92. No `-DNDEBUG`. Nothing committed, nothing pushed, `v86/`
untouched.

## 7. What this does and does not establish

* **Establishes (MEASURED):** `41 0F CC` now writes `r12d` and leaves `rsp` untouched, against host
  ground truth, with a RED-before/GREEN-after asymmetry on seven compared fields; the 32-bit
  machine is byte-identical (transcript) and performance-identical (bench pin).
* **Does NOT establish:** that the amd64 guest boots, or that `bswap %r12d` was its LAST blocker.
  The guest was not re-run by this unit — that run is the orchestrator's, and the next stop string
  is the next finding.
* **Does NOT establish:** that the class is closed. §4.1's `41 90` is a live in-class BLOCKER that
  this unit reports and does not fix, and §4.4 names the surfaces this sweep did not audit.
