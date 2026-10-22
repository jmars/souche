# x86-64: the GS base was dropped for RIP-relative operands — fixed, and the kernel runs on

**Status: the exact measurement was made with the toolkit, the defect is FIXED, the arm is measured to
fail against the pre-fix binary, and the guest moved to a new named stop.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The three numbers (MEASURED — the toolkit, not a hand probe)

`V86C_WATCH=0x31558:1 V86C_RING=8 V86C_DUMP_ON_STOP=1` on the pinned kernel, after the fault hook was
corrected to report `previous_ip` (the last unit's own tool defect):

    WATCH F 0x31558 write rip=0xffffffff9c2775ba
    ring:  eip=0xffffffff9c2775b7 bytes 0f 20 e0 65 48 89 05 96 9f db 63 …
    bases cs=0 ss=0 ds=0 fs=0 gs=0xffffffff9c242000

so the instruction is **`65 48 89 05 96 9F DB 63` = `mov %rax, %gs:[0x63DB9F96]`** at
`0xFFFFFFFF9C2775BA`, and the three numbers are:

| quantity | value |
|---|---|
| the GS base in the machine's state | `0xFFFFFFFF9C242000` |
| the displacement decoded from the instruction | `0x63DB9F96` |
| **RIP-next + displacement** | `0x9C2775C2 + 0x63DB9F96` = **`0x100031558`** |

**And `0x100031558`'s low 32 bits are exactly `0x31558`** — the address the port faulted on.

## 2. What the port computed, and the defect (MEASURED + READ)

READ (`src/cpu/modrm.c`, `modrm_resolve64`): the RIP-relative branch ended with

    if (rip_relative) {
        /* RIP-relative addresses do not take a segment base (DS is implied) */
        return offset;
    }

— which is right for the implied DS (base 0 in 64-bit mode) and **wrong for an FS/GS override**. The
SDM (Vol 2, 2.1.6) says the override selects the segment for **any** memory operand, RIP-relative
included; in 64-bit mode only FS and GS carry a base.

So the port returned `0x100031558` — and the access then used `0x31558`. **The three numbers settle
which half of the smell was which:** the base was **not added at all** (candidate (i)), and the
address that reached the access was the low 32 bits of the correct sum — which is the *consequence* of
the fault report's `int32_t`/`uint32_t` shape (`trigger_pagefault`'s `addr` reached the watch as
`0x31558`), not a separate narrowing on the store path (the store path is `int64_t` end to end:
`int64_t addr = modrm_resolve(...)` → `instr64_89_mem(int64_t)` → `safe_write64_addr(int64_t)`).

## 3. The fix

    int32_t pf = cpu->prefixes & PREFIX_MASK_SEGMENT;
    if (pf != 0 && pf != SEG_PREFIX_ZERO) {
        int32_t sg = pf - 1;
        if (sg == FS || sg == GS) return offset + (int64_t)cpu->segment_offsets[sg];
    }
    return offset;

The `SEG_PREFIX_ZERO` (LEA's sentinel) is excluded, so LEA keeps computing an address with no base —
which is its semantics — and CS/DS/ES/SS stay at 0 in 64-bit mode.

## 4. The arm (MEASURED to fail before, pass after)

**The corpus cannot carry it:** the native leg would have to set *its own* FS/GS base, and the compared
window would then depend on the host's segment state — the same class of reason `mov cr4` could not be
a corpus case. The arm is `m1c_selftest`'s `test_rip_relative_segment_base`: it puts a disp32 at the
cursor, sets `segment_offsets[GS] = 0xFFFFFFFF9C242000`, and asserts the resolver returns
`RIP-next + disp + base` for a `65` prefix, and `RIP-next + disp` for `3E` (DS) and for no prefix.

    with the fix:      PASS
    with the mutation (the early return restored):
        FAIL modrm_resolve64(0x05) == (int64_t)(0x2004 + 0x1000) + 0xFFFFFFFF9C242000ull
             (got 0x3004, want 0xffffffff9c245004)

## 5. The acceptance test (MEASURED)

| | before | after |
|---|---|---|
| stop | `#PF` at `0x31558`, then the delivery recursion bound | **`long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)`** |
| rip | 0xFFFFFFFF9C2775BA | **0xFFFFFFFF9C2771F2** |
| cr3 | 0x30ea000 | 0x30ea000 (the kernel's own tables) |

**The kernel no longer faults on its percpu store** — it runs past it and reaches a stop this port
named in the rep-string unit: a `0x66`-prefixed string op (`66 A5`), which the overlay's `narrow64`
guard refuses by name. **That is the next unit:** implement the 16-bit string forms in long mode (they
are reachable now) or lift the guard with an arm — the overlay comment says why it stopped ("the arms
split on F2/F3 only").

**So: the guest does not yet run its own code end to end, but it is now past the fault that stopped
it, on its own page tables, in its own text, at a stop the port chose to make.**

## 6. Verification (MEASURED; code changed, so the FULL battery)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green (cpu 3888, m1c 272, hw 975,
                                           uop_cache 7934363, uop_exec 102, jit 68, jit_codegen 3570)
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

**Every pin unmoved.**

## 7. Files touched

| file | change |
|---|---|
| `src/cpu/modrm.c` | `modrm_resolve64`'s RIP-relative branch adds the FS/GS base (SEG_PREFIX_ZERO excluded; CS/DS/ES/SS stay 0) |
| `src/cpu/m1c_selftest.c` | `test_rip_relative_segment_base` — the arm of §4, with the reason the corpus cannot carry it in the file |
