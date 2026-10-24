# x86-64: there is no 32-bit load at that site — the value is the function's ARGUMENT

**Status: MEASUREMENT ONLY (one 256 MiB boot with both breakpoints and the slot watch; no source change,
no fix). The brief's premise is REFUTED: the instruction the previous unit assumed at
`0xffffffff819e42b5` does not exist in the executed path, and the value in `rbx` is `rdi` — an argument
the caller passed, not a load at all.** Claims tagged MEASURED / READ / INTERPRETATION.

## 1. The two-link read (MEASURED, 256 MiB firmware, slide `0x27800000`)

    break slot 0 0xffffffffa91e42b5+0x1 hits=0        ← the address the width hypothesis named: NEVER EXECUTED
    break slot 1 0xffffffffa91e42bc+0x1 hits=3457     ← executed 3457 times
    BREAK 1 rax=0x0 rbx=0xffffd1fac0003b82 rdi=0xffffd1fac0003b82 …

**Slot 0 never fires**: there is no instruction executed at `0xffffffff819e42b5`. The disassembly of the
function (READ, `/tmp/vmlinux.bin`):

    ffffffff819e429e: push %rbx
    ffffffff819e429f: mov  %rdi,%rbx            ; ← rbx = THE ARGUMENT
    ffffffff819e42a2: sub  $0x2a8,%rsp
    ffffffff819e42a9: mov  %gs:0x28,%rax        ; the stack canary (64-bit, and not the value in question)
    ffffffff819e42b2: mov  %rax,0x2a0(%rsp)
    ffffffff819e42ba: xor  %eax,%eax
    ffffffff819e42bc: lea  0x6(%rsp),%rbp
    ffffffff819e42c1: lea  0x8(%rsp),%rdi
    ffffffff819e42c6: movq $0x0,0x6(%rsp)       ; ← the fault

So `rbx` is `rdi` — the caller's argument — and the only `%gs:` read in the prologue is the **64-bit**
canary load at `…42a9`. **The "32-bit load of a 64-bit field" hypothesis is refuted at this site**: the
previous unit's `mov %gs:0x199e0(%rip),%ebx` belongs to a DIFFERENT function (`cpu_init`'s neighbourhood at
link `0xffffffff8107e8b5`), and this unit conflated the two disassemblies — corrected here.

## 2. The per-cpu slot watch (MEASURED, with a stated caveat)

`V86C_WATCH='0xf8199e0:8'` (physical derived as `gs_base − (gs_base & ~0x3fffffff) + 0x199e0`, with
`gs_base = 0xffff8eaa8f800000`): **17 hits, every one `value=0x0`** from a memset at
`rip=0xffffffffaa8b36be`. **No stack-top value is ever written there**, so that slot is not the producer —
*provided* the derived physical is right (it assumes `page_offset_base = gs_base & ~0x3fffffff`, which this
run does not print; the zero-only pattern is consistent with a per-cpu area being zeroed, so the address is
probably right, but it is not proven).

## 3. What the value is, then

`0xffffd1fac0003b82` arrives in `rdi` from the caller (at the break the `rbx = rdi` assignment has already
happened). It is a 64-bit value passed whole — no truncation is visible at this function — and it sits
0x3b82 above a 1 GiB boundary, i.e. the same shape as the faulting instance (0x382). **So the term that
places the stack is one link further up: the caller, and what it passes in `rdi`.**

## 4. No fix

Nothing at this site is wrong: no load, no width, no truncation. The defect (if any) is in whoever computes
the argument.

## The ONE next measurement

**The caller and its argument.** At the same breakpoint, read the return address off the stack
(`V86C_PEEK_SP`, the frame at `rsp + 0x2a8 + 8` region) and disassemble backwards from it to the function's
entry — that names the function that passes `rdi`; then read what it loaded `rdi` from (a task field, a
per-cpu read, or a `current` dereference) and compare that link with QEMU's at the same instruction, where
the value is `0xffffc90000013b81` and lands 0x13b81 into its region.
