# x86-64: `setcc` truncated its address to 32 bits — the guest's blocker, found and fixed

**Status: the lane's blocker is NAMED and FIXED. `setcc_mem` took an `int32_t` address, so every
`setcc` with a memory operand truncated its 64-bit effective address (and `safe_write8` then
sign-extended the 32-bit value). In long mode the 0F dispatch routes `setcc` WITHOUT REX.W — which
is every `setcc` in a normal kernel — to the bodies that narrow here. The guest now runs PAST its
stop into a new, clean blocker. Claims tagged MEASURED / READ.**

## 1. How it was reached (the instrument chain, MEASURED)

1. `V86C_RSPLOW` (commit `ab3598d`) recorded the descent's pushes instead of reading stale stack
   words, and `tools/kallsyms.py` (commit `3769548`) named them: the descent is **printk's own
   formatting path**, `vsnprintf → pointer → symbol_string`, inside a **page-fault oops loop** —
   `asm_exc_page_fault → exc_page_fault → page_fault_oops → printk("%pS") → search_bpf_extables /
   search_module_extables` — repeating every `0x380` bytes until the 16 KiB stack was gone.
2. `cr2` on the `V86C_INTS` line (this unit) gave each delivery's fault address: the oops loop's
   faults were at `0xffffffffc0003b1b`, `0xffffffffc000379b`, … **each 0x380 lower and each equal to
   the guest's stack address truncated to 32 bits and sign-extended** (`rsp 0xffffd1fac0003b00 →
   cr2 0xffffffffc0003b1b`).
3. `V86C_WATCH` armed on both the real page and the truncated range printed
   `WATCH F 0xffffffffc0003b1b write rip=0xffffffffa891eb8d` — 16 times, never the full address — so
   the truncation is upstream of the walk, not in the report.
4. That rip is `_prb_read_valid+0x3d` = `sete 0x1b(%rsp)` (READ, disassembly of the pinned image).
   The store's target is inside a **mapped** page (`0xffffd1fac0003000`, PTE verified in
   results/x86-64-stackoverflow.md §4): it faulted only because its address was truncated.

## 2. The defect (READ)

    src/cpu/misc_instr.c:103   void setcc_mem(bool condition, int32_t addr)     <-- the narrowing
    src/cpu/misc_instr.c:105       safe_write8(addr, condition ? 1 : 0);

All sixteen `setcc` bodies (`src/cpu/instructions_0f.c:1165-1215`) take an `int64_t addr` and hand it
straight to `setcc_mem`, which narrows it. `safe_write8` takes `int64_t` and dispatches to
`safe_write8_addr` in long mode, so the narrowing was the whole bug — and because
`dispatch_os_index_0f` sends a `setcc` with no REX.W to os_index 1, the *32-bit* arm's body is what a
64-bit kernel executes.

## 3. The fix and its asymmetry (MEASURED)

`setcc_mem(bool, int64_t)` — the parameter type only, in `misc_instr.h` and `misc_instr.c` (with the
measured reason in the header comment). No body, no table, no dispatch change.

| | before the fix | after the fix |
|---|---|---|
| instructions | 4,590,902,204 | 4,564,309,411 |
| stop | fault raised while delivering a fault, `rip=0xffffffffa91e42c6` | `long mode: 0xA9 has no 64-bit form`, `eip=0xa8879d3d` |
| serial | ends at `Spectre V2 : Mitigation: Retpolines` | ends at `x86/fpu: x87 FPU will use FXSAVE` (10+ more lines: Spectre V2 RSB, Speculative Store Bypass, MDS, MMIO Stale Data, return thunk, ITS, FXSAVE) |

The count is **lower** after the fix and the boot is **further**: the failing path spent 26 M
instructions inside the oops recursion that no longer happens. That direction is the asymmetry —
less work, more boot — not a regression, and it is why the instruction count alone is not the gate.

Gates: `make` rc 0 · `make selftest` rc 0 · `make nasm64` 13/0 (the native corpus, which cannot catch
this class: it is identity-mapped into the first 4 MiB, so a truncated address equals the real one
there — stated so the missing arm is a named gap, not a silence).

## 4. The next blocker, named by the run itself (MEASURED)

    long mode: 0xA9 has no 64-bit form in gen/x86_table64.js
    eip=0xa8879d3d  (link 0xffffffff810f9d3d)

`REX.W A9 imm32` = **`TEST rax, imm32`**, the accumulator form. `case 0x2A9` in
`src/cpu/interpreter_gen.c` is `unimplemented_instruction(...)`; the 16/32-bit arms (`0xA9`, `0x1A9`)
exist. The lane's S-slice note "TEST r/m64,imm32" covered the `F7 /0` form, not this accumulator
form. This one the native corpus CAN carry: it touches no memory, and `test rax, 0xffffffff` with a
value above 4 GiB distinguishes a 64-bit test from a 32-bit one at any address.
