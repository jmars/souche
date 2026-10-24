# x86-64: the KASLR stop is NOT a stack-address truncation — the stack's PDPTE is ABSENT

**Status: MEASUREMENT, AND IT REFUTES THE BRIEF'S PREMISE FOR (A). No fix landed — there is nothing to
widen. The store's address is the FULL 64-bit `rsp+6`; the fault is a genuine unmapped-page fault because
the page-table entry for the kernel stack was never installed.** Claims tagged MEASURED / READ /
INTERPRETATION.

## 1. The fault is at the full 64-bit address (MEASURED, `V86C_WATCH`)

    WATCH F 0xffffd206bfffffc6 write rip=0xffffffff897e42c6 (the access FAULTED, no store)
    WATCH F 0xffffd206bfffff90 write rip=0xffffffff897e42c6 … (the delivery's own frame push, and the recursion)

The **first** fault is a write at `0xffffd206bfffffc6` = `rsp + 6` with `rsp = 0xffffd206bfffffc0` — the
exact operand of the instruction the stop names (`movq $0x0,0x6(%rsp)`). **The address is 64 bits wide and
correct.** `cr2 = 0xbfffff90` in the stop dump is the documented 32-bit CR2 projection of the *last*
fault (a delivery re-entry 0x36 bytes lower), which is what made it look like a truncation.

## 2. Why it faults: the page tables have no entry for the stack (MEASURED, `V86C_CHAIN`)

    chain va=0xffffd206bfffffc6 cr3=0x2a10000 idx=420/26/511/511
      PML4E  @0x2a10d20 = 0x100067          (present, -> table 0x100000)
      PDPTE @0x1000d0   = 0x0  (ABSENT)

The 1 GiB region holding the kernel stack has **no PDPTE**. So `rsp` points into memory the kernel never
mapped — or mapped and lost. This is a page-table/population defect (or a bogus stack pointer), not an
address-width defect.

## 3. Why (A) is the wrong pick, in one line

**(A) is refuted: there is no 32-bit truncation on this path — the store's effective address is the full
`0xffffd206bfffffc6` and the fault is a missing page-table entry, so the "same class, one-line fix"
hypothesis does not hold and no fix is landed here; (B) — the `nokaslr` spinlock — remains the next unit
as the brief says.**

## 4. What this does NOT establish

The `nokaslr` configuration never reaches this code, so nothing here says whether the missing PDPTE is
the kernel's own lazy-vmalloc state or the port's loss of a table write. That is the next measurement's
job, and it is cheap: the table lives at physical `0x100000`, entry `0x1000d0`.

## The ONE next measurement

**Watch `0x1000d0` for the write that should install that PDPTE** (`V86C_WATCH='0x1000d0:8'`, KASLR
configuration): if the kernel never writes it, its lazy `vmalloc_fault` path did not run (a delivery
defect, and the recursion is the symptom); if it writes a wrong value, the defect is in whatever body
stored it and the writer's `rip` names it. The `nokaslr` spinlock (B) stays queued behind this.
