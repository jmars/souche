# x86-64: the gate IS present — the failure is the FRAME PUSH, which hardware would also #DF on

**Status: MEASUREMENT ONLY (three KASLR boots, no source change). The CPU-entry-area mapping exists and
the #PF gate is present and mapped; the recursion is the frame push landing on the same unmapped stack
page. The defect is upstream of the delivery, where it has been for two units.** Claims tagged
MEASURED / READ / INTERPRETATION.

## 1. The gate, walked (MEASURED, `V86C_CHAIN` on the gate address)

`idtr = 0xfffffe0000000000`, so the #PF gate is at `idtr + 14*16 = 0xfffffe00000000e0`:

    chain va=0xfffffe00000000e0 cr3=0x2a10000 idx=508/0/0/0
      PML4E  @0x2a10fe0 = 0x4422067                  (present -> table 0x4422000)
      PDPTE @0x4422000 = 0x4410067                  (present -> table 0x4410000)
      PDE   @0x4410000 = 0xfb3067                   (present -> table 0xfb3000, no PS)
      PTE   @0xfb3000  = 0x8000000003310161         (PRESENT, NX, frame 0x3310000, flags 0x161)

**The CPU-entry-area mapping the brief suspected is there, and the gate is present** — four levels, a real
4 KiB frame. So the brief's hypothesis (2) — a missing entry-area mapping — is **refuted**, and the
delivery's gate fetch is not what fails.

## 2. What does fail: the frame push (MEASURED, the watch's own sequence from the previous unit)

    WATCH F 0xffffd206bfffffc6 write rip=0xffffffff897e42c6   ; the stack store (rsp+6)
    WATCH F 0xffffd206bfffff90 write rip=0xffffffff897e42c6   ; then this, 0x36 bytes lower, forever

`0xffffd206bfffff90 = rsp - 0x30` — **exactly the #PF frame push** (5 qwords plus alignment) onto the
same page that is unmapped. The handler cannot start because its own frame cannot be written, and the
port's bound stops after four re-entries.

## 3. The honest answer, plainly

**Hardware would also #DF here**: a #PF frame cannot be pushed onto the faulting page, so the second
fault escalates to a double fault — and the port stops **by name**, saying so
(`a fault raised while delivering a fault: the delivery re-entered four times (hardware would raise #DF;
the port stops rather than recursing)`). That is the port behaving as documented, not a new defect.
**The real defect is upstream and unchanged: the kernel stack's page was never mapped** — its PDPTE is
zeroed and never populated (`results/x86-64-pdpte2.md`), so the first store to the stack faults at all.

## 4. Instrument note (an instrument's silence is not evidence)

The second run asked for the gate's 16 bytes with `V86C_PEEK='0x33100e0:16'` and **the PEEK line did not
print**, although the same instrument printed fine in earlier units with the same syntax. That is the
instrument, not the guest: the PTE's presence and frame are already measured by the chain, so nothing in
this report rests on the missing bytes. It is recorded here so a later unit does not read the absence as
a fact about the gate.

## 5. No fix

Neither (2) nor (3) proves a port defect at the delivery: the gate is present and the escalation is what
the SDM says. Landing anything here would be the "fix you cannot prove".

## The ONE next measurement

**Where the stack's page-table entry should have come from.** The kernel clears the PDPTE (a `memset` of
the table page at `0x100000`) and then never writes a non-zero value into `0x1000d0` — so either the
populating store went to a different address (watch the whole table page `0x100000:0x1000` for non-zero
writes and compare the addresses the kernel *intends*), or the kernel never intended to populate it and
the stack pointer itself is wrong (trace where `rsp = 0xffffd206bfffffc0` came from: the task's
`thread.sp`/`cpu_current_top_of_stack` path). That single run distinguishes "a store the port lost or
mis-addressed" from "a bogus stack pointer".
