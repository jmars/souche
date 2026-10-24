# x86-64: the PDPTE slot is ZEROED and never populated — and the fault is never delivered

**Status: MEASUREMENT ONLY (one KASLR boot with the watch + ring, no source change). The slot IS written
— twice, with zeros, by a memset — and no populate write ever follows; the ring proves the kernel's #PF
handler never executes a single instruction, so the lazy fault path could not have filled it.** Claims
tagged MEASURED / READ / INTERPRETATION.

## 1. Everyone who writes `0x1000d0` (MEASURED, `V86C_WATCH='0x1000d0:8'`, whole run)

    WATCH P 0x1000d0 width=4 value=0x0 rip=0xffffffff897eb95d
    WATCH P 0x1000d4 width=4 value=0x0 rip=0xffffffff897eb95d

**Two writes, both zero, both from the same `rip` — and that is all.** (The watch reports 4-byte
granularity because the port's 64-bit physical store goes through two 32-bit stores; the two halves are
one 8-byte store.) Slide `0x7E00000`, so the writer is link **`0xffffffff819eb95d`**:

    ffffffff819eb950: mov %rax,(%rdi)
    ffffffff819eb955: mov %rax,0x8(%rdi)
    ffffffff819eb95d: mov %rax,0x18(%rdi)      ; ← the rip the watch names, rax = 0
    ffffffff819eb971: lea 0x40(%rdi),%rdi
    ffffffff819eb975: jne 0xffffffff819eb950

an unrolled zeroing loop (`memset`). **So the PDPTE slot is cleared by the kernel and never installed** —
this is the brief's case (3) with the populate write MISSING, not wrong.

## 2. The fault is never delivered (MEASURED, `V86C_RING=64` at the stop)

The ring's newest entries are the faulting function's own prologue:

    eip=0xffffffff897e42ba  bytes 31 c0 48 8d 6c 24 06 48 8d 7c 24 08 48 c7 44 24 …
    eip=0xffffffff897e42bc  bytes 48 8d 6c 24 06 48 8d 7c 24 08 48 c7 44 24 06 00 …
    eip=0xffffffff897e42c1  bytes 48 8d 7c 24 08 48 c7 44 24 06 00 00 00 00 48 c7 …

The last completed instruction is the `lea` at `…42c1`; the next one (`movq $0x0,0x6(%rsp)` at `…42c6`)
faults. **Not one instruction of a fault handler appears in the ring** — the kernel's #PF handler never
ran, so its lazy `vmalloc_fault` path could not have populated the PDPTE, and the delivery recursion is
what stops the machine.

## 3. What that leaves

Two facts, in order: (i) the kernel clears the PDPTE for the stack's 1 GiB region and never writes it —
which on real hardware is fine *because the #PF handler fills it lazily*; (ii) **the #PF is never
delivered here**, so the mechanism that would have fixed the mapping cannot run. The recursion bound is
the symptom the lane has met twice before; this is the third, and now with a ring that shows the handler
never starting.

## The ONE next measurement

**The delivery of a #PF whose frame push targets the same unmapped page**: break at the delivery entry
(`call_interrupt_vector`'s #PF path) on the first fault at `0xffffd206bfffffc6` and read what it does —
the gate it fetches from `idtr` (`0xfffffe0000000000`, the CPU-entry-area alias), whether the gate is
present, and where the frame push goes — because a fault handler that pushes its frame onto the same
unmapped stack cannot run, and the port must say which of those two it is (a #DF-worthy recursion the
hardware would take, or a delivery defect of its own).
