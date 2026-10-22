# x86-64: the 64-bit far return — widened, and it exposes the port's 32-bit stack path

**Status: `far_return`/`instr64_CB` widened to 64 bits with the SDM's eight-byte frame and pop
order. The ≥ 4 GiB stop is GONE. The kernel's next RETF runs with a stack pointer at a KERNEL VIRTUAL
ADDRESS, which the port's 32-bit stack path cannot carry — measured as a HOST SEGFAULT first, then
turned into a named stop.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. What was widened, against the SDM (READ + MEASURED)

* `far_return(int32_t eip, …)` → **`far_return(int64_t eip, …, bool is_64)`** (exceptions.c,
  instructions.h). The 16/32-bit callers pass `is_64 = false` and a zero-extended eip, so their
  behaviour is unchanged.
* **The same-CPL frame**: `adjust_stack_reg(2 * 8 + stack_adjust)` for `is_64` — RIP and CS are
  eight bytes each. `instr64_CB` reads the frame itself and passes `stack_adjust = 0`.
* **The privilege-change path** (SDM, RET in 64-bit mode): the frame is RIP(8), CS(8), then **RSP(8)
  and SS(8) — after RIP and CS**, which is the order the 32-bit path does not have to spell out
  because its RSP/SS sit at +8/+12. The new arm reads RSP at `stack_adjust + 16` and SS at
  `stack_adjust + 24` as eight-byte fields, and **stops by name** if the new RSP is ≥ 4 GiB (the
  port's stack register path is 32-bit — see §3).
* `instr64_CB` no longer bounds the target: `far_return((int64_t)ip, cs, 0, true, true)`.

**The corpus cannot carry the arm**, MEASURED by construction and by the same reason `mov cr4` could
not: the corpus's **native leg runs at CPL 3**, and a `retf`/`iretq` to a ring-0 code segment #GPs
there, so the two legs cannot compare. The arm is a selftest one (`m1c_selftest`,
`test_far_return_64bit_frame`): a 4-level fixture, an eight-byte frame holding
`0xFFFFFFFF9C27759C` and `SEL_CODE2` (L=1), then `instr64_CB`; it asserts the **full 64-bit target**,
the CS, and that **RSP moved by 16** — the two things a four-byte frame gets wrong.

**MEASURED, and stated rather than dressed up:** that arm **passes** with the fix but also passed
under the mutation that restores the 32-bit frame, so **its discriminating power is NOT
established** — it is a seat, not an instrument. The instrument for this unit is the guest (§3) plus
the fact that the *old* named stop is gone; the honest next step is to make the arm bite (e.g. by
asserting through a path that the mutation changes) or to record it as a gap.

## 2. The acceptance test, first attempt: a HOST SEGFAULT (MEASURED)

With the widening in place the guest ran to the kernel's **third** RETFQ and the host **crashed with
no message at all** (`Segmentation fault (core dumped)`, 139, no `HOST FAULT` dump). A probe at
`instr64_CB` printed the three RETFQs the kernel executes:

    PROBE retfq: ip=0x100266            cs=0x10 cpl=0 rsp=0x4f56ff0          <- the decompressor's trampoline
    PROBE retfq: ip=0x1000043           cs=0x10 cpl=0 rsp=0x2a03f48         <- startup_64's own
    PROBE retfq: ip=0xffffffff9c27759c  cs=0x10 cpl=0 rsp=0xffffffff9bc03f40 <- the KERNEL's, on a high stack

**The kernel's stack pointer is a kernel VIRTUAL address above 4 GiB.** The port's stack path is
32-bit (`get_stack_pointer` = `get_seg_ss() + read_reg32(ESP) + offset`), so the frame read went to
`0x9BC03F40` — past the end of 256 MiB of guest RAM — and `safe_read64_addr` read out of bounds:
a host crash, not a guest fault. **That is the real finding of this unit**, and it is a much larger
gap than the far-return frame: the port's *stack* cannot carry a 64-bit RSP.

## 3. The stop as it stands now (MEASURED)

`instr64_CB` now checks `read_reg64(ESP) > 0xFFFFFFFF` first and stops **by name**:

    v86c boot: 4475480768 instructions, in_hlt=0, exception=-1, unimplemented=1,
               eip=0x88e0015a cs=0x10 cr0=0x80050033 cr3=0x30ea000
    v86c: unimplemented: long mode: a far return with RSP at or above 4 GiB
          (the port's stack path is 32-bit: get_stack_pointer reads ESP)

Same instruction count as the crash, no host fault, the kernel still on **its own page tables**
(`cr3=0x30ea000`) at `eip=0x88e0015a`. **A crash became a named stop — that is progress in the
port's contract even though the guest does not advance.**

## 4. The next unit, named

**Make the stack path 64-bit.** The concrete pieces, all measured or read here:
`get_stack_pointer` (misc_instr.c:115 — `read_reg32(ESP)`), `set_stack_reg`, `push32/pop32/push16/pop16`
and every `safe_read*/safe_write*` call that adds `get_seg_ss()` to a 32-bit RSP. The instrument is
the same one this unit used: a `retfq`/`push`/`call` whose RSP is above 4 GiB (the kernel supplies
one for free at instruction 4,475,480,768), plus a selftest arm asserting that a push/pop at
`RSP = 0xFFFFFFFF9BC03F40` moves the *full* register.

**Also still open from this unit:** `iretq` was **not** widened. `iret` is a single `iret(bool is_16)`
with the 32-bit frame; the 64-bit form needs RIP(8)/CS(8)/RFLAGS(8) and, on a privilege change,
RSP(8)/SS(8) after them. The guest has not reached an `iretq` yet, so it is not the blocker — and it
is recorded rather than claimed.

## 5. Verification (MEASURED; code changed, so the FULL battery)

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

**The four M0 pins did not move** — the evidence that the widened `far_return` is inert outside long
mode. `cpu_selftest`'s `far_return` recording stub was widened with it (`is_64` explicitly unused);
no assertion count changed.

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/exceptions.c` | `far_return(int64_t eip, …, bool is_64)`: the eight-byte same-CPL frame and the 64-bit privilege-change frame (RSP/SS after RIP/CS), with a named stop for a new RSP ≥ 4 GiB |
| `src/cpu/instructions.h` | the new signature and its reason |
| `src/cpu/instructions.c` | the four 16/32-bit callers pass `is_64 = false` |
| `src/cpu/instructions_64.c` | `instr64_CB`: the 64-bit target, the frame popped there, and the RSP ≥ 4 GiB named stop |
| `src/cpu/m1c_selftest.c` | `test_far_return_64bit_frame` (a seat; §1 records that it does not bite) |
| `src/cpu/cpu_selftest.c` | the `far_return` recording stub widened |
