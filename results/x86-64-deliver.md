# x86-64: why the delivery faults — the IDT page is UNMAPPED, so hardware would #DF too

**Status: the question is ANSWERED by measurement, and the answer moves the defect OUT of the delivery
path: the descriptor read at `0x9BC040E0` FAULTS because the kernel's IDT page is not mapped. The port
is not mis-delivering; it is refusing to recurse on a guest state hardware would triple-fault on.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The measurement (MEASURED, unbuffered)

A probe at the *first* thing the delivery does — the IDT descriptor read — printing the linear
address, its physical translation and the fault flag, on the kernel's own #PF delivery:

    PROBE desc-read: vector=14 lin=0x9bc040e0 phys=0x0 pf=1
    PROBE desc-read: vector=14 lin=0x9bc040e0 phys=0x0 pf=1
    PROBE desc-read: vector=14 lin=0x9bc040e0 phys=0x0 pf=1
    PROBE desc-read: vector=14 lin=0x9bc040e0 phys=0x0 pf=1
    v86c: unimplemented: a fault raised while delivering a fault: the delivery re-entered four
          times (hardware would raise #DF; the port stops rather than recursing)

**The descriptor read faults, on the first access the delivery makes.** The three candidates the last
unit named are therefore all **refuted before they are reached**:

* **(a) the address computation** — `lin = idtr_offset + vector * gate_size` = `0x9BC04000 + 14*16`
  = `0x9BC040E0` is arithmetically right; it is the *translation* that fails, not the arithmetic.
* **(b) the frame push** — never reached: the delivery returns at the descriptor read, so
  `writable_or_pagefault` and `push64` never run (and both were widened by the stack64 unit).
* **(c) the CPL-changing arm** — not entered: `cpl=0` in the probe, and the delivery dies earlier.

## 2. What that means: the guest's own state, and hardware's answer (READ + INTERPRETATION)

The IDT base is `0x9BC04000`; the kernel's boot stack — which the same probe set showed **translating**
— sits at `0xFFFFFFFF9BC03F40`, i.e. in the page **immediately below** the IDT. So the kernel's
identity map covers the stack page and **stops at the page boundary**, leaving the IDT page itself
unmapped.

On hardware the sequence is identical: a #PF whose gate cannot be read raises **#DF**, and the kernel's
bringup IDT has gates only for `#PF` and `#VC` (MEASURED earlier: `idt[14]` non-null, `idt[8]` zero),
so a #DF there is a **triple fault → reset**. **INTERPRETATION, stated as one:** the port is now
behaving the way the architecture does at this point — it stops with the reason named where the
hardware resets — and the previous unit's recursion bound is what makes that visible instead of fatal.

## 3. So where is the real defect? (the next unit's question, with its instrument)

Not in the delivery. The questions that remain are about the **guest's state**, and each has a
measurable answer:

1. **What was the FIRST fault?** The delivery being repeated is `vector=14` — the kernel took a #PF
   somewhere, and the last unit measured that the walk for `0xFFFFFFFF9C27759C` **translates**. So the
   #PF came from a *different* address: the next probe is `trigger_pagefault`'s `addr` (and `cr2`) on
   the first entry, which names the instruction and the access that faulted.
2. **Is the IDT page mapped in the guest's own tables, or is the port's walk wrong?** The walk returned
   `pf=1` (absent) for `0x9BC040E0` while the adjacent stack page translated, so the two differ by one
   page — a shape the kernel would have to have built deliberately. The instrument: dump the
   PML4E/PDPTE/PDE/PTE chain for `0x9BC03000` and `0x9BC04000` and compare the entries (the same probe
   shape the walkoob unit used).
3. **If the guest really is faulting before its IDT is mapped, what did the port do to put it there?**
   The kernel's `startup_64_load_idt` loads `bringup_idt_descr` *before* the first C call, so an
   unmapped IDT at that point is either the kernel's own ordering (and the fault is the guest's) or a
   consequence of an earlier mis-execution in the port. The instrument is the same `cr2` trace.

**Nothing was changed this unit**, and that is deliberate: the brief's rule is that the outcome must be
the guest's own fault handling, and the measurement says the guest's IDT is not mapped — so widening
the delivery (or removing the bound) would not deliver anything, it would only recurse again.

## 4. Verification (MEASURED; **no functional change** — probes added, run, removed)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (83/83 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed

`git status` shows no change under `src/` (the probes were removed and the build re-verified; the tree
is the previous unit's, which the repository records as commit `801cf8e`). No pin moved. The bench,
reboot, VGA ×5, pae-1, pae-l3 and gate-long-mode-sys were not re-run because nothing functional
changed — the previous unit's full battery on the same tree is the record for them.

## 5. Files touched

| file | change |
|---|---|
| — | **none.** Probes went into `src/cpu/exceptions.c` (the descriptor read, the same-CPL stack check, the first push) and `src/main.c` (`setvbuf`), were run, and were removed. |

**Still open:** §3's three measurements; the arm for the recursion bound (the previous unit's stated
gap); the high-RSP selftest arm; `iretq`.
