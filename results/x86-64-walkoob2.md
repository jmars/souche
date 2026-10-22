# x86-64: the instrument fixed, the question answered — and the host crash became a named stop

**Status: (1) the instrument is fixed and the lost evidence is recovered; (2) the question is
ANSWERED by measurement — the walk TRANSLATES, the crash was the #PF delivery re-entering unboundedly
(a host stack overflow); (3) the fix is a BOUND on the emulator's recursion, measured to turn the host
death into a named stop. The guest does not yet run its own code: it stops by name at 4,475,480,778.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The instrument (MEASURED — the second time this lane lost evidence to its own tooling)

`setvbuf(stderr, NULL, _IONBF, 0)` at the top of `main`, and the same probe set re-run. The lines that
were lost to block buffering now survive the crash, and they answer the question:

    PROBE walk enter addr=0xffffffff9c27759c fetch=1 write=0
    PROBE walk exit  addr=0xffffffff9c27759c entry=0x307759f pf=0        <- TRANSLATES
    PROBE walk enter addr=0xffffffff9bc03f48 fetch=0 write=1
    PROBE walk exit  addr=0xffffffff9bc03f48 entry=0x2a03f55 pf=0
    PROBE walk enter addr=0xffffffff9bc03f40 fetch=0 write=1
    PROBE walk exit  addr=0xffffffff9bc03f40 entry=0x2a03f55 pf=0
    PROBE delivery vector=14 cpl=0 idtr base=0x9bc04000 limit=511
    PROBE delivery vector=14 cpl=0 idtr base=0x9bc04000 limit=511
    PROBE delivery vector=14 cpl=0 idtr base=0x9bc04000 limit=511   (and on, and on)

**The answer is: the walk TRANSLATES.** `0xFFFFFFFF9C27759C` → frame `0x3077000` (`entry=0x307759f`,
`pf=0`), inside RAM — so the last unit's "the crash is in the walk's PML4-index-511 path" is
**REFUTED**, and so is the `safe_read64_addr` hypothesis before it. **The crash is in the #PF
DELIVERY**, which then re-enters: the kernel's own IDT is at `0x9BC04000` — a linear address above
4 GiB — and each delivery faults again, so `call_interrupt_vector` recursed without bound and the
**host's stack overflowed** (SIGSEGV, no output — the same silent death, now explained).

## 2. The fix: a bound on the emulator's recursion (MEASURED)

`call_interrupt_vector` is now a thin wrapper over `call_interrupt_vector_inner` with a
**delivery-depth bound of 4**:

    if (delivery_depth >= 4) → named stop: "a fault raised while delivering a fault: the delivery
                               re-entered four times (hardware would raise #DF; the port stops
                               rather than recursing)"

**Why a bound and not a refusal:** the bound is on the *emulator's* recursion, not on the guest's
deliveries — a guest may nest up to four, far above anything the machine legitimately does, and the
stop names exactly what happened. Hardware answers a fault raised while delivering a fault with
**#DF**, and a fault during #DF with a triple fault; this port must not recurse into the host's stack,
and now it cannot. **MEASURED:** the same run that used to die silently now ends

    v86c boot: 4475480778 instructions, in_hlt=0, exception=-1, unimplemented=1,
               eip=0x8ae775ba cs=0x10 cr0=0x80050033 cr3=0x30ea000
    v86c: unimplemented: a fault raised while delivering a fault: the delivery re-entered four
          times (hardware would raise #DF; the port stops rather than recursing)

— no host fault, the guest's state intact, the reason named.

## 3. What is still wrong, and it is the next unit

**The delivery itself faults**, and that is the real defect this unit did not reach: the kernel's
bringup IDT at `0x9BC04000` is a *high* linear address, the walk translates it (the same probe shape
shows every high walk succeeding), and yet the delivery re-faults. The candidates, none yet measured:
the descriptor read's own address computation in `call_interrupt_vector`
(`cpu->idtr_offset + (uint32_t)(interrupt * gate_size)` — the offset is `uint64_t`, the addend is
`uint32_t`, so this one looks right), the frame **push** onto a stack at `0xFFFFFFFF9BC03F40`
(`push64` uses `read_reg64(ESP)` and `safe_write64_addr`, both widened), or the CPL-changing arm,
which still **stops by name** in long mode. **The next measurement** is a probe *inside* the delivery
printing, in order: the descriptor address and its translated physical address, the stack pointer and
the `writable_or_pagefault` result, and which of the delivery's reads/writes sets `cpu->pagefault`.

## 4. The arm: NOT written, and said so (MEASURED gap)

The brief asks for an arm measured to fail against the current binary. **It is not written.** The
honest statement of why: the bound's own failure mode *was* measured (the silent SIGSEGV before the
fix, the named stop after it), but that is the guest, not a reproducible arm; a selftest arm would need
a fixture whose fault delivery itself faults, and building that fixture is the next unit's work
alongside §3. Recording it as a gap rather than implying an instrument that does not exist.

## 5. Verification (MEASURED; code changed, so the FULL battery)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (83/83 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-pae-l3.sh all               rc 0 — 0 FAILING
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed
    tests/gate-vga-{0,1,2,3,4}.sh          rc 0 each — 0 FAILING

**The four M0 pins did not move.** No assertion count changed.

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/exceptions.c` | `call_interrupt_vector` split into a depth-bounded wrapper + `call_interrupt_vector_inner`; the bound's reason and the #DF comparison in the file |

**Still open:** why the delivery faults (§3, next measurement named); the arm of §4; the high-RSP
selftest arm from the stack64 unit; `iretq`.
