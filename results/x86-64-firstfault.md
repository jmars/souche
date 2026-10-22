# x86-64: the three questions answered — the first fault is a WRITE to 0x31558, and the IDT page is genuinely unmapped

**Status: MEASUREMENT ONLY, no code change. All three questions are answered by their own probe, and
the answer is that the GUEST's own page tables do not map its IDT — hardware would #DF and reset there.
The port's named stop is the correct seat; the open question is why the guest's identity map is
incomplete, and it has a measured starting point.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. Question (1): what was the FIRST fault? (MEASURED)

A probe at the top of `trigger_pagefault`, printing the first three entries:

    PROBE PF#1 addr=0x31558 present=0 write=1 user=0 fetch=0 rip=0xffffffff9c2775c2
    PROBE PF#2 addr=0x9bc040e0 present=0 write=0 user=0 fetch=0 rip=0xffffffff9c2775ba
    PROBE PF#3 addr=0x9bc040e0 present=0 write=0 user=0 fetch=0 rip=0xffffffff9c2775ba

**The first #PF is a WRITE to `0x31558`** — a *low* address, `present=0`, `user=0`, `fetch=0` — at
`rip=0xFFFFFFFF9C2775C2` (kernel virtual). The known walk for `0xFFFFFFFF9C27759C` translates (measured
in the walkoob2 unit), so the fault is **not** in the fetch path the earlier units suspected: it is a
**store** to `0x31558`. Every later delivery is the `#PF` for the IDT descriptor at `0x9BC040E0`,
which is `PF#2`/`PF#3` — i.e. the cascade the deliver unit measured.

## 2. Question (2): the PML4E/PDPTE/PDE/PTE chain, dumped (MEASURED)

The same probe walks the guest's own tables by hand (cr3 = `0x30EA000`, entries read physically) for
the two adjacent pages:

    PROBE chain va=0x9bc03000 i4=0 i3=2 i2=222 i1=3 e4=0x30ec063 e3=0x0 e2=0x0 e1=...
    PROBE chain va=0x9bc04000 i4=0 i3=2 i2=222 i1=4 e4=0x30ec063 e3=0x0 e2=0x0 e1=...

**The PDPTE (level 3) is `0x0` — ABSENT — for BOTH pages.** So:

* the IDT page `0x9BC04000` is **genuinely unmapped** in the guest's tables — the deliver unit's
  `pf=1` was right, and **there is no port off-by-one in the stride, the index arithmetic or the
  frame mask**: the level-3 entry the walk reads is zero, exactly where a hand walk reads zero;
* the *stack* page `0x9BC03000` is **also** absent, yet the walk for `0xFFFFFFFF9BC03F40` returned
  `pf=0` and a frame of `0x2A03F40` earlier — **that translation came from the 64-bit TLB, not from a
  walk.** MEASURED as an inconsistency between two probes of the same run: the hand walk says absent,
  the port's `translate_address` says present. **INTERPRETATION:** the TLB holds a stale entry from
  before the kernel switched CR3/rewrote its tables, which is architecturally permitted (the TLB is
  not coherent and the guest must invalidate) — but it means the earlier "the stack translates" reading
  was a **cache hit**, and this report corrects it.

## 3. Question (3): guest's ordering or a port mis-execution? (READ + INTERPRETATION)

READ (`arch/x86/kernel/head64.c:625-637`, `startup_64_setup_env`): `native_load_gdt`, the data-segment
reloads, then `startup_64_load_idt(physbase)` — so the bringup IDT is installed **before** the first C
call, and its base is `fixup_pointer(&bringup_idt_descr, physbase)`. The measured base `0x9BC04000`
sits immediately above the boot stack (`__end_init_task - FRAME_SIZE` at `0xFFFFFFFF9BC03F40`), which
is exactly where a `.data`/`.bss` IDT table would be placed — so the base is **plausible and not
mis-computed**.

What is *not* answered, and is the next measurement: **the first fault is a store to `0x31558`**, an
address in the low identity region that the guest's current tables do not map. That store is either
(a) the kernel writing somewhere it expects the *decompressor's* identity map to still cover (the map
that was active before CR3 changed), or (b) a consequence of an earlier mis-execution in the port that
computed a wrong destination. **The instrument:** at `rip=0xFFFFFFFF9C2775C2`, dump the instruction
bytes, the register holding the destination and the value stored — the same shape as every other stop
this chain has named.

## 4. Is the seat correct? (the question the brief asks directly)

**Yes, and it is a deliberate seat rather than a defect.** Hardware's sequence at this state is
`#PF` (the store to `0x31558`) → the gate for vector 14 is read from an IDT whose page is unmapped →
`#DF` → the bringup IDT has no `#DF` gate (MEASURED earlier: `idt[14]` non-null, `idt[8]` zero) →
**triple fault → RESET**. The port stops with the reason named and the guest's state intact.

**Should it reset instead?** INTERPRETATION, and a policy question rather than a bug: the port has no
reset-on-triple-fault path today, and adding one would *hide* the state that made this chain
diagnosable. The honest recommendation is to keep the named stop and, separately, name the absence of
a triple-fault→reset behaviour as a **known divergence** (the port cannot reproduce a machine that
reboots in a loop). That is recorded here rather than implemented, because implementing it would
destroy the evidence this unit just used.

## 5. Verification (MEASURED; **no functional change** — probes added, run, removed)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (83/83 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed

`git status` shows nothing under `src/` (the probes were removed and the build re-verified; the tree is
commit `801cf8e`). No pin moved. The full battery was not re-run because nothing functional changed.

## 6. Files touched

| file | change |
|---|---|
| — | **none.** Probes went into `src/cpu/paging.c` (`trigger_pagefault`'s first three entries + the hand walk of both pages) and `src/main.c` (`setvbuf`), were run, and were removed. |

**Open, with instruments named:** §3's store to `0x31558` (the instruction bytes, the destination
register, the value); the stale-TLB reading of §2 (whether the guest invalidates, and whether the
port's TLB should be flushed on a page-table write it can see); the recursion-bound arm; the high-RSP
selftest arm; `iretq`.
