# x86-64: `regs->ip` was never 0 — the #UD is `swapgs`, and SWAPGS was not implemented

**Status: THE PREMISE CORRECTED AND ONE REAL GAP CLOSED. The stop was not a `ret` popping an
unwritten slot: `pt_regs.ip` sits at **+0x80**, not the +0x78 the previous unit read (that is
`orig_ax`), and the frame's real ip is `0xffffffff89a01223` — the instruction **`0f 01 f8` =
`swapgs`** in the kernel's exception-return path. `SWAPGS` was simply not implemented in the port,
so **every** exception that returned through that path killed the machine. It is implemented now,
and the kernel proceeds past it to a #PF on a per-cpu read through the direct map.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The frame, read correctly (MEASURED)

`pt_regs` is `r15,r14,r13,r12,rbp,rbx,r11,r10,r9,r8,rax,rcx,rdx,rsi,rdi,orig_ax,ip,cs,flags,sp,ss`,
so `ip` is at **+0x80** (the previous unit read `+0x78`, which is `orig_ax`, and saw 0). With
`regs` at `0xffffffff8a803e78` (phys `0x2a03e78`):

    PEEK 0x2a03ef8  = 0xffffffff89a01223   ← ip
    PEEK 0x2a03f00  = 0x10                 ← cs (kernel)
    PEEK 0x2a03f08  = 0x02                 ← flags

Disassembled from the image at the link address `0xffffffff81c01223`:

    ffffffff81c011f9  pop %r15 ; pop %r14 ; pop %r13 ; pop %r12 ; pop %rbp ; pop %rbx
    ffffffff81c01203  pop %r11 ; pop %r10 ; pop %r9 ; pop %r8 ; pop %rax ; pop %rcx
    ffffffff81c0120d  pop %rdx ; pop %rsi ; pop %rdi
    ffffffff81c01210  add $0x8,%rsp
    ffffffff81c01219  testb $0x4,0x20(%rsp)
    ffffffff81c0121e  jne 0xffffffff81c01222
    ffffffff81c01220  iretq
    ffffffff81c01222  push %rdi
    ffffffff81c01223  0f 01 f8        ← SWAPGS   (the #UD)

That is the kernel's `restore_regs_and_return_to_kernel` tail: pop everything, test the return CS,
and either `iretq` or `swapgs` first. **So the `ret` was fine and the ring's epilogue
(`pop r12…pop rbp; jmp <ret thunk>`) was a red herring — the fault is three instructions later, in
the exception return.**

## 2. The gap and the fix (MEASURED)

`src/cpu/instructions_0f.c`'s `instr32_0F01_7_reg` — the `0F 01 /7` slot — called `trigger_ud()`
unconditionally. `0F 01 F8` (`/7`, `rm=0`) is `SWAPGS`, and the port already had every field it
needs: `segment_offsets[GS]` (what `wrmsr 0xC0000101` writes) and `kernel_gs_base`
(MSR `0xC0000102`).

**Landed:** in long mode, `0F 01 /7` with `rm=0` exchanges them; `rm=1` (RDTSCP) stays a named
stop, and outside long mode the slot stays #UD. `test_swapgs` is the arm — register-only, so it fits
that harness at all — and it fails against the pre-fix build (the values do not move; the call
raises #UD).

**The asymmetry, same guest** (MEASURED):

| | before | after |
|---|---|---|
| stop | #UD (`trapnr 6`) at `0xffffffff89a01223` = `swapgs` | no #UD; the return path runs |
| then | `early_fixup_exception` panic | `mov %cr3,%rdi; and $~0x1800,%rdi; mov %rdi,%cr3` then `mov %gs:0x19000,%rdi` |
| new stop | — | #PF, `cr2 = 0xffff88800001fb40`, halted by the recursion guard |
| instructions | 4,475,556,480 | 4,475,485,559 |

## 3. The new stop, named (MEASURED)

The #PF is on `mov %gs:0x19000,%rdi` — a per-cpu read through the direct map (`gs_base =
0xffff888000006b40`, so the address is `gs_base + 0x19000`). The demand-repair is **not** refusing
this time: the chain exists —

    PEEK 0x30ea888 (PML4[273]) = 0x30ec063   → early_dynamic_pgts[0]
    PEEK 0x30ec000 (PUD[0])    = 0x30ecd063  → a PMD table at 0x30ecd000

— and what halts the machine is `early_recursion_flag`: `PEEK 0x312c000 = 3`, and
`0xffffffff8ae9c800` is `ja → the halt loop` (`> 2`). So **three nested exceptions** were taken
before the guard fired: the handler's own path faulted.

**The next fork, with the instrument in place:** break on `early_idt_handler_common`'s entry
(`0xffffffff8ae77120`, which does `incl early_recursion_flag`) and count the hits, `PEEK
0x312c000` at each, and find whether the decrement at the handler's exit is reached — if the port
never executes it, the flag accumulates and the third fault halts a machine that was repairing
correctly.

**The calibrated target is NOT reached**: no first `printk`, so nothing was added to
`gate-x86-64.sh`.

## 4. Verification (MEASURED; **code changed**, full battery, by EXIT CODE)

    make / selftest / nasm / nasm64        rc 0 (selftest: all suites green, incl. test_swapgs)
    make bench                             200095214
    make transcript-diff                   byte-identical to the reference
    make determinism                       two consecutive runs byte-identical
    gate-m2-5-determinism                  4,000,096,757 both runs, serials byte-identical
    gate-x86-64 12/0 · gate-long-mode 57/0 · gate-long-mode-sys 5/0 · gate-x86-64-diff 13/0
    gate-nasm64 13/0 · gate-reboot-1 39/0 · gate-pae-1 11/0 · gate-pae-l3 0 FAILING
    gate-vga-0..4                          0 FAILING each

**No pin moved** — as it must not: `SWAPGS` is reachable only in long mode, and outside it the slot
still raises #UD exactly as before.

## 5. Files touched

| file | change |
|---|---|
| `src/cpu/instructions_0f.c` | `0F 01 /7 rm=0` in long mode is SWAPGS (swaps `segment_offsets[GS]` and `kernel_gs_base`); `rm=1` and non-long-mode stay #UD |
| `src/cpu/m1c_selftest.c` | `test_swapgs` |
| `results/x86-64-retzero.md` | this report |
| `TODO.md` | §8 |
