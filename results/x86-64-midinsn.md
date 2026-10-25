# x86-64: the guest is now in subsystem init — and the new stop starts MID-INSTRUCTION

**Status: MEASURED, no fix in this unit. Three ISA blockers cleared in sequence (results/x86-64-setcc.md,
-testrax.md, -popcnt64.md, -0f64.md) and the pinned amd64 guest has moved from a printk/oops stack death
at 4,590,902,204 instructions to a new class of stop at 4,767,022,744. The new stop is a double-fault
cascade whose first cause is a **resume at a non-instruction-boundary address**. Claims tagged
MEASURED / READ / INTERPRETATION.

## 1. Where the guest is (MEASURED, one boot each, in order)

| after | instructions | serial reaches |
|---|---|---|
| `setcc` fix (`7d72b15`) | 4,564,309,411 | `x86/fpu: x87 FPU will use FXSAVE` |
| `TEST rax, imm32` (`8d09c2c`) | 4,598,496,963 | + SMEP/ITS/FXSAVE |
| `POPCNT r64` (`bc3b832`) | 4,604,942,499 | + LSM, AppArmor, TOMOYO, smpboot: SMP disabled |
| `SHLD/SHRD imm8` (`26006ee`) | 4,767,022,744 | + clocksource tsc, rtc_cmos, keyrings, zbud, integrity, x509 |

Each blocker was named by one 3-minute boot (V86C_RING names the in-progress bytes, `tools/kallsyms.py`
names the functions, `V86C_INTS`+cr2 names the faults), and each fix carried a native-corpus arm that is
RED before and green after. **The pattern is now cheap: boot, read the ring, land the arm.**

## 2. The new stop (MEASURED)

    STOP (a fault raised while delivering a fault: ... re-entered four times)
    eip=0xffffffffa8d3df6a  cr2_64=0x58ba5690  rsp=0x58ba56c0  cr4=0x6b0
    INTS 2127 vec=14 err=0x2 depth=0 from_rip=0xa8d3df6a rsp=0x58ba56c0 cr2=0x58ba572c
    INTS 2128.. vec=14 err=0x2 depth=1.. from_rip=0xa8d3df6a cr2=0x58ba5690 (rsp-0x30: the push)

`V86C_RSPLOW` puts the transition on exactly one instruction:

    RSPLOW 29446631 rip=0xffffffffa8d3de57 rsp=0xffffd1fac056ba58 delta=-0x170
    RSPLOW 29446632 rip=0xffffffffa8d3df65 rsp=0x58ba56c0   delta=-0xffffd1fa679c6398

So `rsp` leaves the vmalloc stack (`0xffffd1fa…`, the kernel-stack region) for `0x58ba56c0` at the
instruction that *starts* at link `0xffffffff815bdf65`.

## 3. Why that address is the finding (READ, disassembly of the pinned image)

    ffffffff815bdf61:  48 8d 54 24 06   lea 0x6(%rsp),%rdx     <- 5 bytes, ends at df66
    ffffffff815bdf66:  be 40 00 00 00   mov $0x40,%esi
    ffffffff815bdf6b:  48 89 fb         mov %rdi,%rbx

**`0xffffffff815bdf65` is the LAST BYTE of the `lea`.** Execution began an instruction there, which
means control arrived at an address that is not an instruction boundary in the image. The ring at the
stop shows the guest's own bytes at `0xa8d3df68` as `0f cd 44 89 6c 24 6c` (`bswap %ebp; mov %r13d,0x6c(%rsp)`)
where the image has `00 00 00 48 89 fb` — so **the guest has patched its own text** (alternatives /
static calls / ftrace), which is normal and expected; the port's decode of the patched bytes is not the
question here. The question is the ENTRY POINT.

**INTERPRETATION (the hypothesis the next measurement must refute):** the port resumed mid-instruction
after a fault. `trigger_pagefault` sets `cpu->instruction_pointer = cpu->previous_ip` (paging.c:165) —
the resume address on a delivered fault is `previous_ip`, and if that field is stale by even one
instruction the guest resumes inside the previous instruction's bytes. The `rsp` then takes whatever
the garbage decode produces (`0x58ba56c0`), and every later access faults on an unmapped low address.

## 4. The ONE next measurement

**Catch the jump into `0xffffffff815bdf65`.** Two cheap arms, both measurement-only:
1. `V86C_BREAK='0xffffffffa8d3df65:1'` with `V86C_BREAK_MAX=1 V86C_BREAK_RING=1 V86C_RING=16` — the
   break fires exactly when execution arrives at that address, and the ring then holds the 16
   instruction boundaries that led to it: the last one names the transfer (a `ret`, a `jmp`, or a
   fault resume) and its target.
2. Watch `cpu->previous_ip` versus the instruction stream around the preceding fault: run with
   `V86C_WATCH_F` armed on the vmalloc stack region (`0xffffd1fac0000000:0x8000`) so the faults before
   the jump print their `rip`; the fault immediately before `0x…df65` names the instruction whose
   `previous_ip` was wrong.

If the ring shows a `ret`/`jmp` landing there, the guest's patched text and the port's decode disagree
(a code-modification visibility bug, and `uop_invalidate_page_fast` is where to look). If it shows a
fault resume, the defect is `previous_ip`'s value at delivery, and the same class as the `setcc`
narrowing: one wrong field, found by one boot.
