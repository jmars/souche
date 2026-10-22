# x86-64: the first fault is still masked — and the evidence says the guest's own state is the cause

**Status: the debug-only bound override is added (default off, shipped behaviour unchanged); the
measurement it was added for did NOT converge, and the reason is itself the finding. The evidence
supports the FIRST of the two shapes: the guest's tables map nothing in the low half and its IDT is
unmapped, so hardware would `#DF` and reset — the port's seat is right and there is nothing to fix in
the machine.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The tool (code change, default off)

`V86C_FAULT_DELIVERY_BOUND=<N>` — read once in `v86c_dbg_init`, held in
`v86c_dbg_delivery_bound` (default **4**, the shipped value), and read by the recursion bound in
`call_interrupt_vector`. **Unset = byte-identical behaviour**; the four M0 pins and the transcript are
the measurement of that (§5). It is a *debug* override: it exists so an investigation can let the
first delivery complete instead of being masked by the bound.

## 2. What the override measured (MEASURED — and it did not converge)

    $ V86C_WATCH=0x9c517600:8,0x9c5100e0:1 V86C_FAULT_DELIVERY_BOUND=16 V86C_RING=8 \
      V86C_DUMP_ON_STOP=1 ./build/v86c … --long-mode on

    v86c dbg: WATCH F 0x9c5100e0 read rip=0xffffffff9c277246   (×5, the delivery's descriptor read)
    v86c dbg: STOP (a fault raised while delivering a fault: … re-entered four times …)
    v86c dbg: state rip=0xffffffff9c277246 cr2=0x9c5100e0 cr3=0x30ea000
                     idtr=0x9c510000/4095 gdtr=0x9c24d000/127 cpl=0

**Two results, one of them negative and both worth having:**

* **`0x9C517600` — the `rep movsq` destination the ring pointed at — does NOT fault.** The watch on it
  never fired, so the ring's last entries are not the first fault's instruction; that lead is dead.
* **Raising the bound does not reveal the first fault either.** At bound 16 the deliveries nest deeper
  and every one of them fails at the *same* descriptor read (`0x9C5100E0`, the vector-14 gate). The
  reason is structural, not a tool limitation: **the delivery cannot complete while the IDT page is
  unmapped**, so no amount of depth exposes what started it — the first fault's own delivery is the one
  that dies, every time.

**INTERPRETATION, stated as one:** the instrument the last report proposed cannot answer the question
by construction, and the honest move is to say so rather than to keep turning the knob. The stop's
message text ("re-entered four times") is a literal string and now reads slightly wrong when the bound
is overridden — recorded here rather than silently reworded.

## 3. Which shape the evidence supports (MEASURED + INTERPRETATION)

The last unit measured the chain: **`PML4E[0] @0x30EA000 = 0x0`** — the guest's top-level entry for the
entire low half is absent — and the port's own walk agrees, so no stride/index/frame-mask/truncation
defect is involved. This unit adds that the fault is a **read of the IDT descriptor** and that the
`rep movsq` destination is fine.

So the state is: **the kernel's page tables do not map its own IDT, and a fault arrives.** That is the
**first** shape — *"the kernel is between two table switches, its own IDT not yet mapped"*. On hardware
the sequence is `#PF` → the vector-14 gate read from an unmapped page → `#DF` → the bringup IDT has no
`#DF` gate → **triple fault → reset**. **The port's named stop is the correct seat**, and it is the
second measurement (after the deliver unit) to say so.

**Nothing in the machine should change for this.** The guest cannot proceed past its own triple-fault
on THIS image: the honest next step is a **different guest or a different expectation** (e.g. a kernel
built without the early-boot window this one faults in, or accepting that the decompressor-to-kernel
handover of this Debian image is beyond a 32-bit-physical port), not another stop or another widening.

## 4. What a *real* answer would need (named, not attempted)

To see the first fault, the delivery must be able to complete — i.e. the IDT page must be mapped. That
is the guest's state, not the port's. The alternative instrument is a **hardware cross-check**: run the
same image on a real machine (or a reference emulator with long mode) with the same early-boot
conditions and observe whether it also triple-faults. That is the QEMU leg the plan's §5 names as the
second reference, and it is a unit of its own.

## 5. Verification (MEASURED; code changed — the debug override only)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (84/84)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-pae-l3.sh all               rc 0 — 0 FAILING
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed
    tests/gate-vga-{0,1,2,3,4}.sh          rc 0 each — 0 FAILING

**Every pin unmoved** — the override is default-off and the shipped path is unchanged.

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/debug.h` / `debug.c` | `v86c_dbg_delivery_bound` (default 4) and `V86C_FAULT_DELIVERY_BOUND` |
| `src/cpu/exceptions.c` | the recursion bound reads the variable instead of the literal 4 |
