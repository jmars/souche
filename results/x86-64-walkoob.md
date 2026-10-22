# x86-64: the host SIGSEGV — localised further, NOT fixed, and the kernel now runs in high memory

**Status: the unit's scope (find and fix the out-of-bounds read) is NOT achieved. What it did achieve
is a sharper localisation and a large, measured advance: the stack frame reads TRANSLATE correctly,
and the amd64 kernel now executes at KERNEL VIRTUAL ADDRESSES. The crash is now pinned to the fetch
after a `far_return` to `0xFFFFFFFF9C27759C`, and the next measurement is named with its instrument.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The frame reads are NOT the crash — they translate (MEASURED)

A probe inside `safe_read64_addr` for high addresses printed, at the kernel's RETFQ:

    PROBE s64 addr=0xffffffff9bc03f40 low12=0xf40
    PROBE s64 translated p=0x2a03f40 memsz=0x10000000
    PROBE s64 addr=0xffffffff9bc03f48 low12=0xf48
    PROBE s64 translated p=0x2a03f48 memsz=0x10000000

So the widened stack path of the previous unit **works**: the 64-bit RSP translates through the
kernel's own page tables to `0x2A03F40`, inside the 256 MiB of RAM. The last unit's hypothesis — "the
crash is inside `safe_read64_addr(0xFFFFFFFF9BC03F40)`" — is **REFUTED**, and that is the honest
correction this unit's first measurement produces.

## 2. The kernel is now running at KERNEL VIRTUAL ADDRESSES (MEASURED)

A probe at `interpreted_run` for high eips:

    PROBE run: eip=0xffffffff9a2000e3 phys=0x10000e3 memsz=0x10000000
    PROBE run: eip=0xffffffff9a2000e5 phys=0x10000e5
    PROBE run: eip=0xffffffff9a2000e7 phys=0x10000e7
    PROBE run: eip=0xffffffff9a2000fa phys=0x10000fa

`0xFFFFFFFF9A2000E3` translates to `0x10000E3` — **the kernel's own text at 16 MiB**, reached through
the kernel's page tables. This is the first time the guest has executed at a kernel virtual address in
this lane, and it is a direct consequence of the stack unit's widening.

## 3. Where the crash is, and why it is not yet pinned (MEASURED + one INTERPRETATION)

The last probe to fire before the death is a `far_return` to **`0xFFFFFFFF9C27759C`** with `csbase=0`
(the kernel's own `retfq`), and no `interpreted_run` line follows it. So the crash is at the **fetch**
of that target — `translate_address_fetch(0xFFFFFFFF9C27759C)` → the 4-level walk — and **not** in the
stack arithmetic or the frame read.

**Why the walk's own dump did not settle it, stated plainly:** the probe lines for that last address
were **lost**. With stderr redirected to a file the probe output is block-buffered, and a SIGSEGV
discards the buffer — MEASURED as an inconsistency between two runs of the *same* binary: one printed
`PROBE far_return target: eip=0xffffffff9c27759c`, the next did not, and in a third the walk's
`enter`/`exit` lines for that address were absent while earlier ones appeared. **The instrument, not
the guest, is what failed here.**

**The next measurement, with its instrument named:** re-run the same probe set with
`setvbuf(stderr, NULL, _IONBF, 0)` (or an explicit `fflush` after each probe), so the walk's
`enter`/`exit` line for `0xFFFFFFFF9C27759C` survives the crash. That line answers the only question
left: does the walk **translate** (and the crash is downstream, in the fetch guard or the TLB store)
or does it **fault** (and the crash is in the #PF delivery)? Both are one line away.

## 4. Why no fix is claimed, and what the fix must be

The brief's rule is right and was followed: **a bound, not a refusal.** Nothing was changed in the walk
this unit, because the measurement did not yet prove which of the two shapes it is:

* if the walk **translates** and the crash is downstream, the fix is in the fetch path — the physical
  address the walk returns must be checked against `cpu->memory_size` **before** `mem8[phys]`, which
  `interpreted_run` already does for its own entry but which a *return into a new batch* may bypass;
* if the walk **faults**, the fix is in the delivery — and the kernel's bringup IDT **does** have a
  #PF gate (MEASURED in the earlier unit: `idt[14]` non-null, handler `0x4f3f870`), so a fault should
  become the guest's #PF, not a host death.

**Either way the outcome must be one of: the translation, or the guest's #PF.** A host crash is not
acceptable, and it is not being papered over with a stop that would fire on a valid kernel address.

## 5. Verification (MEASURED; **no functional change this unit** — probes added, run, removed)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (83/83 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed

`git status` shows the tree exactly as the previous unit left it (its stack-path widening is intact);
the probes were removed and the build re-verified. No pin moved. The bench, reboot, VGA ×5, pae-1 and
pae-l3 were not re-run because nothing functional changed — the previous unit's full battery on the
same tree is the record for them.

## 6. Files touched

| file | change |
|---|---|
| — | **none.** Probes went into `src/cpu/paging.c`, `src/cpu/cpu.c` and `src/cpu/exceptions.c`, were run, and were removed; the build was re-verified. |

**Still open:** the crash of §3 (the next unit's first job, with the unbuffered instrument named); the
selftest arm for a high-RSP stack (the previous unit's stated gap); `iretq` (untouched, the guest has
not reached one).
