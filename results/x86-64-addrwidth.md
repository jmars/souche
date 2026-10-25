# x86-64: the address-width sweep (`int32_t addr` callees) and the corpus's first >4 GiB MEMORY seat

**Status: landed. Every 64-bit-effective-address narrowing the reviewer's sweep named on the
guest's reachable paths is widened; the native corpus can now SEE the class, and the two new arms
are MEASURED red against the old signatures and green after. The seat itself was broken: the
generator's above-4-GiB window was arithmetically wrong and had never been exercised. Claims
tagged MEASURED / READ / INTERPRETATION.**

Inputs: `results/x86-64-setcc.md` (the class), `fx-agent-memory read review-x8664-isa-4changes`
(the sweep this unit executes), `results/x86-64-0f64.md` (the census corrected in §6).

## 1. The class, and what "widen it" means here

A function takes `int32_t addr` while its caller resolved a 64-bit effective address as `int64_t`;
the narrowing truncates, and the callee's accessor then sign-extends the 32-bit value. In long mode
that is silent wrong memory — or a spurious `#PF` at a sign-extended address — for anything above or
below the low 4 GiB. `setcc_mem` was fixed this way in `7d72b15`; the reviewer then swept the class
and found the same shape in ~50 more places. I verified each call site before changing it (the
reviewer's line numbers were accurate at `9d998b5`).

## 2. What was widened (MEASURED)

| where | count | what |
|---|---|---|
| `src/cpu/fpu.h` | 43 declarations (+2 in the header's cross-unit contract comment) | the whole x87 memory family: `fpu_load_i16/i32/i64`, `fpu_load_m32/m64/m80`, the 22 `fpu_fildm*`/`fpu_fist*`/`fpu_fldm*`/`fpu_fst*`/`fpu_fbstp`/`fpu_store_m*` helpers, `fpu_fldcw`, `fpu_fldenv16/32`, `fpu_fstenv16/32`, `fpu_fsave16/32`, `fpu_frstor16/32`, `fpu_fstcw`, `fpu_fnstsw_mem`, and **`fxsave`/`fxrstor`** |
| `src/cpu/fpu.c` | 43 definitions | the same set |
| `src/cpu/instructions_0f.c` | 6 | `sgdt` `sidt` `lgdt` `lidt` (`:156/192/222/278`, called from the `0F 01` `int64_t` bodies) and `maskmovq`/`maskmovdqu` (`:5901/5928`) |
| `src/cpu/instructions_0f.h` | 2 | a **second, conflicting** declaration of `fxsave`/`fxrstor` (`:55-56`) that still said `int32_t` — it would have been a compile error once `fpu.h` moved, and it is a declaration the sweep's line list did not mention |
| `src/cpu/string.c` | 1 cast | `:199` the INS write **pre-check**: `writable_or_pagefault((int32_t)(es + (int64_t)dst), size_bytes)` → the full address (the callee already takes `uint64_t`; the store below it, `string64_write`, always used the full address, so the pre-check faulted on a page the store never touches — a spurious `#PF` with a truncated `cr2` for an INS buffer above 4 GiB) |

**Doing it as a mechanical sweep, and how I proved none was missed.** The x87 family was changed
with one substitution over both files (`sed -i 's/\bint32_t addr\b/int64_t addr/g' src/cpu/fpu.h
src/cpu/fpu.c`), never one at a time. The `\b` matters: `uint32_t` contains `int32_t` as a
substring, so an unanchored substitution would have rewritten the physical model's `uint32_t`
parameters too — and `rg 'int32_t addr'` counts them, which is why the proof grep below is
word-anchored.

    $ rg -n '\bint32_t addr' src/cpu/fpu.h src/cpu/fpu.c            -> 0 lines
    $ rg -n '\bint32_t addr' src/cpu/instructions_0f.c \
                             src/cpu/instructions_0f.h              -> 0 lines
    $ rg -n '\bint32_t addr' src/cpu/string.c                       -> 0 lines

and the only `\bint32_t addr` left anywhere under `src/cpu` is:

    src/cpu/paging.c:180        static int32_t do_page_walk(int32_t addr, ...)   <- the non-long-mode arm
    src/cpu/misc_instr.c:284/293/332/341   push{16,32}_ss{16,32}_mem            <- cpu_selftest callers only
    src/cpu/misc_instr.c:570/586           lss16 / lss32                        <- see below
    src/cpu/cpu_selftest.c:6321/6450       test-local `int32_t addr` scratch
    (plus comments in modrm.c:605, paging.h:150, instructions_64.h:19)

**Nothing was widened that the reviewer verified as correct** — `memory.h`'s `uint32_t` family
(the 32-bit physical model), `do_page_walk(int32_t)` (the non-long-mode arm), `read_moffs`'s
`int32_t` return, `push*_ss*_mem`, `count_until_end_of_page`. I re-read each of those call paths and
agree; none is reachable with a 64-bit effective address in long mode.

**One candidate the sweep did not list, checked and left alone (READ):** `lss16`/`lss32`
(`misc_instr.c:570/586`) take `int32_t addr` and are called from `instr16/32_0FB2/B4/B5_mem(int64_t
addr)`. They are **not reachable in long mode**: `case 0x2B2`/`0x2B4`/`0x2B5` in the generated
dispatcher (`interpreter_gen.c:16789/16867/16903`) are all-stop arms —
`unimplemented_instruction("long mode: 0F B2 has no 64-bit arm in gen/x86_table64.js")`. If a later
unit lands those arms, the two bodies must be widened with it; naming it here so that is a step in
that unit rather than a rediscovery.

## 3. The corpus seat (the structural cause) — and the stub it needed

`gen/nasm64_gen.py` defined `HIGH_BASE = 0x100000000` and its comment claimed a case's access
through a 64-bit effective address "is visible in the window it compares". The reviewer MEASURED
that no case ever used it. That is true, and **the reason is that the mapping could not have
worked**: the stub wrote `PML4[1] -> PDPT2[0] -> PD2[0]`, a 2 MiB page at physical `0x100000`, and
called that linear `0x100000000`. Three arithmetic errors (MEASURED, computed):

1. **PML4 index is `addr >> 39`.** `0x100000000 >> 39 = 0`, so linear 4 GiB lives in **PML4[0]** —
   the entry the low identity map already owns — and its **PDPT index is 4**. `PML4[1]` begins at
   `0x8000000000` (512 GiB), so the window was mapped 128× too high.
2. **A 2 MiB page's base must be 2 MiB aligned.** `0x100000 & 0x1FFFFF = 0x100000`, i.e. bit 20 is
   set: that PDE was a reserved-bit `#PF` even at the linear address it actually named.
3. The tables at `0x10B000`/`0x10C000`/`0x10D000` were **outside the stub's zeroing range**
   (`PML4..PT_END = 0x104000..0x10B000`), so entries 1..511 of the "2 MiB" PD were whatever the
   model's RAM happened to hold.

Fixed as: zeroing extended to `PT2_END`, and the window is now
**`PML4[0] -> PDPT[4] -> PD2[0] -> PT2`**, `PT2` identity-mapping physical `0x100000..0x108000`
(8 × 4 KiB — the compared window plus the 8 KiB above it a case may use as a scratch). No 2 MiB
page, no alignment assumption.

**The native leg needed the same window, or the seat is one-legged.** `tests/mkfixture64.c` mapped
`[0x80000, 0x103000)` only; a case reaching above 4 GiB would `SIGSEGV` there and the comparison
would read it as a `#GP` — proving nothing. The image now lives in a **shared memfd mapped twice**:
at the link base, and again at `0x100000000` with file offset `0x80000`, so host address
`0x100000000 + x` **is** host address `0x100000 + x` — the emulated leg's alias, reproduced. A
narrowing therefore lands somewhere else in BOTH legs' compared window rather than faulting.

### 3a. The two arms

| arm | what it does | which function it catches |
|---|---|---|
| `wide-high-mem-store` | `mov r9, 0x100000100; mov qword [r9], rax; mov rcx, [r9]; ... [r9+8]` — a plain 64-bit store **and** load through an EA above 4 GiB, with the read-back stored into the low window | the store/load leaf the resolver hands its EA to: `safe_write64_addr` (measured, below), i.e. the whole `modrm_resolve -> safe_write64(_addr)` chain |
| `wide-high-mem-fxsave` | `fninit; fld1; fldz; fxsave [r9]` at `0x100002000`, a plain `rep movsq` copy of the image to a second high address, `fninit`, `fxrstor [r10]` from **the copy**, then `fstp` the two restored values into the window | **`fxsave` / `fxrstor`** — this unit's blocker (`fpu.c:1148/1192`, reached from `instr_0FAE_0_mem`/`_1_mem`, `instructions_0f.c:1581/1587`; the FPU encodings have no REX.W form so every long-mode `0F AE /0,/1` runs them) |

The fxsave arm went through one failed design first, and the failure is worth recording: with
`fxsave [r9]` and `fxrstor [r9]` at the **same** address, the arm was **GREEN under the old
`int32_t` signature** — both accesses truncated to the same `0x2000` and round-tripped consistently.
A case whose two sides narrow together cannot see a narrowing. The copy through a second high
address (a path that does not narrow in either leg) is what makes the restore's input depend on the
save's address.

## 4. The asymmetry, MEASURED both ways

All runs `--long-mode on`, corpus filter `high-mem`, `build/nasm64/` cleared first (the
stale-`.img` trap is real — `results/x86-64-0f64.md` §5).

**(A) `fxsave`/`fxrstor` restored to `int32_t`** (fpu.c:1148/1192, fpu.h:191-192,
instructions_0f.h:55-56 — the tree's own pre-fix code), rebuilt, run:

    SUMMARY: total=2 green=1 red_unimplemented=0 mismatch=1 error=0   (exit 1)
    wide-high-mem-fxsave.img: 3 field(s)
        mem[100104]  Actual: 0x7ff80000   Expected: 0x0
        mem[10010C]  Actual: 0x7ff80000   Expected: 0x3ff00000
        mm7.int32[1] Actual: 0x0          Expected: 0xffffffff80000000

i.e. the restore read the wrong page, found zeros, and the `fstp`s stored the **indefinite QNaN**
instead of `0.0` and `1.0`. Restored → `total=2 green=2 mismatch=0`, exit 0.

**(B) `wide-high-mem-store` against a narrowing of its leaf.** No narrowing exists on the plain
store path at HEAD (that is why the arm is green there), so the mutant reintroduces the class
exactly one frame up: `safe_write64_addr(int32_t addr, ...)` (`paging.c:1544`, `paging.h:166`),
rebuilt:

    SUMMARY: total=2 green=0 red_unimplemented=0 mismatch=2 error=0   (exit 1)
    wide-high-mem-store.img: mem[100100] Actual: 0x0 Expected: 0x55667788
                             mem[100104] Actual: 0x0 Expected: 0x11223344
                             mem[100108] / mem[10010C] / mem[100118] / mem[10011C] ... 0x0

— the store landed at physical `0x100` (outside the compared window) and the load read zeros back.
Restored → green. (An earlier attempt narrowed `safe_write64`, which the REX.W store path does not
call — `instr64_89_mem` calls `safe_write64_addr` directly. That attempt was GREEN and is recorded
so the next reader does not repeat it.)

**Whole corpus after the change (MEASURED):** `total=91 green=91 red_unimplemented=0 mismatch=0
error=0` with `--long-mode on` — the stub rewrite did not disturb any of the 89 existing cases, and
the fixtures' expected values are the host leg's, not hand-written.

## 5. What the seat does and does not buy

- It **does** make every future `int32_t addr` callee on a memory path visible: any of them turns
  one of these two arms red without further work, because the operand is above 4 GiB and the
  compared window is the alias target.
- It **does not** cover a narrowing that only affects *widths* rather than addresses (the SHLD/SHRD
  and POPCNT families' subject), and it does not cover `#PF`-only paths (a spurious fault with a
  truncated `cr2`, like `string.c:199`): a faulting access in the native leg is a `SIGSEGV` and the
  comparison then only sees the exception kind. `string.c:199` was therefore found by the reviewer's
  reading, not by an arm — stated so its absence is a gap, not a silence.

## 6. Census correction: `results/x86-64-0f64.md` §1 (MEASURED)

The report's headline **45 fully routed / 3 partial / 208 stops** does not match the generated
dispatcher at HEAD. **Method:** split `run_instruction0f` (`src/cpu/interpreter_gen.c`) into its 256
`case 0x2NN:` blocks and classify each by whether it contains an `unimplemented_instruction(` call at
all (none → fully routed; some alongside a real body → mixed; every branch a stop → all-stop). A
script over the generated file, not an eyeball. **Result at `9d998b5`: 256 arms, 47 / 2 / 207.**

- `45` was the pre-`A4`/`AC` count never refreshed (45 + 2 = 47).
- The enumerated "45" list carried **48** names *and* included `0x7E`, which the same table classed
  partial. `0x7E` is not fully routed; the correct list has 47.
- `0x2D6` was called partial with "the plain/`F2` arms call real bodies" — **refuted**: all eight of
  its branches are `unimplemented_instruction("instructions_0f::instr64_{66,F2,F3,}0FD6_{mem,reg}")`.
  MOVQ's whole `0xD6` arm is a stop, and a next unit trusting the old row would have believed half
  of MOVQ r64 landed.

The corrected table, the 47-name list, the `2A/7E` vs `D6` split in §1a and the "count for the next
unit" line (207 stops, ~197 unreachable) are all in `results/x86-64-0f64.md`, each marked as a
correction of this unit's.

## 7. Fixture-prose corrections (MEASURED; the VALUES never moved)

Four statements in `gen/nasm64_gen.py` (and the two restated in `results/x86-64-0f64.md` §3) were
arithmetically wrong. They are corrected as findings — the expected values are the host leg's and
were always green, so nothing was reworded to match a weaker implementation:

| statement | was | MEASURED truth |
|---|---|---|
| `muldiv-shld-shrd-imm8`, `shrd rax, rdx, 0x20` | `0x1122334499AABBCC`, "shifted-in bits come from rdx's **high** half" | `0x5566778899AABBCC`; the shifted-in bits are rdx's **low** half — the source is shifted LEFT by `64-count`, so `rdx << 32 = 0x5566778800000000` lands on top |
| same case, the r/m64 memory arm | `0x0080000000000000 \| 0xFF = 0x00800000000000FF` | `0xFF80000000000000` — the stated formula ORs the source in at the **bottom**, which is not SHRD's rule |
| same case, the count-0 arm | "the `cmp` above leaves ZF=0/SF=1" | `cmp rax, rax` leaves **ZF=1/SF=0** (the comment described `cmp rax, rdx`). The arm still discriminates, on ZF (a flag-setting body reports ZF=0 from result `0xDEADBEEFCAFEBABE`) |
| `alu-test-rax-imm32` arm 2 | "ANDs to ZERO at 64 bits (ZF=1)" | `0x80000000 & 0xFFFFFFFF80000000 = 0x80000000` at **both** widths: ZF=0 in both, and the discriminating flag is **SF** (0 at 64 bits, 1 at 32). Arm 1 still catches the 32-bit body on ZF |

## 8. Gates (ONE pass at the end, every verdict by EXIT CODE, no `-DNDEBUG`)

The edited sources were `touch`ed first — the reviewer's finding that a stale `build/unity.o` can
make `make` skip a real rebuild (the shipped binary is the unity TU) is real and was MEASURED twice
in the 0F unit.

| gate | rc | evidence |
|---|---|---|
| `make` | 0 | unity TU relinked |
| `make selftest` | 0 | all suites green |
| `make nasm64` | 0 | 13/0 gate steps; corpus **91** cases (was 89), wide family 9 (was 7) |
| `make transcript-diff` | 0 | byte-identical to the reference |
| `make bench` | 0 | pin still **200,095,214** |
| `sh tests/gate-long-mode.sh` | 0 | 57 passed, 0 failed |
| `sh tests/gate-x86-64-diff.sh` | 0 | 13 passed, 0 failed |

Three count lines moved with the corpus/stub, each MEASURED rather than guessed:

- `gate-nasm64.sh` (red census 89→91, not-red 91) and `gate-long-mode.sh` (OFF census 89→91, whole
  corpus 91) and `gate-x86-64-diff.sh` (91 cases each) — the two new arms.
- **`gate-x86-64-diff.sh`'s `jit-no-long-mode-blocks` 10 → 11**: the stub's 32-bit phase gained one
  basic block with the window's fill loop. The step's PROPERTY is that the count is a
  pre-long-mode constant, so I measured it at three limits — `blocks compiled 11` at 500 000,
  2 000 000 and 20 000 000 instructions — before changing the literal.
- **A gate finding, not just a count:** `gate-long-mode.sh`'s `patch pte-above-4gib` rewrites the
  stub's `c7 47 04 00 00 00 00` (`mov dword ptr [edi+4], 0`) to set every page frame above 4 GiB,
  and it requires that byte pattern to occur **exactly once**. My first version of the fill loop
  used the same instruction, so the gate refused the image as ambiguous (MEASURED: `pattern
  c7470400000000 occurs 2 times`). The fix keeps the gate's target unique — the high dword now comes
  from a zeroed register (`mov dword ptr [edi+4], edx`) — rather than loosening the gate.

The amd64 guest was **not** run (the orchestrator reruns it after this lands). Nothing was committed
or pushed; `v86/` was not written.
