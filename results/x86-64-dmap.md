# x86-64: the direct map is DEMAND-MAPPED — and the port's repair runs but does not stick

**Status: MEASUREMENT + ONE FIX, AND A CORRECTION TO THE BRIEF'S PREMISE. The brief asked which
`__startup_64` loop writes `PML4[273]`. None does: in this kernel the direct map is built **on
demand by the early #PF handler**, so `PML4[273]` being absent *before* the fault is **correct**.
The port executes that repair (`__early_make_pgtable` is entered), the repair does not take effect,
the same fault recurs, and `early_dynamic_pgts` is exhausted until `reset_early_page_tables()` wipes
`early_top_pgt[0..510]` again — after which `early_fixup_exception` panics.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The premise corrected (READ, from the source of *this* image — 6.1.176)

`arch/x86/kernel/head64.c` (fetched at tag `v6.1.176`, i.e. the kernel in
`tests/images/vmlinuz-6.1.0-50-amd64`, not `v6.1`):

    static bool __init early_make_pgtable(unsigned long address)
    {
        unsigned long physaddr = address - __PAGE_OFFSET;
        pmdval_t pmd = (physaddr & PMD_MASK) + early_pmd_flags;
        return __early_make_pgtable(address, pmd);
    }

    void __init do_early_exception(struct pt_regs *regs, int trapnr)
    {
        if (trapnr == X86_TRAP_PF && early_make_pgtable(native_read_cr2()))
            return;
        ...
        early_fixup_exception(regs, trapnr);
    }

and `x86_64_start_kernel` runs `reset_early_page_tables()` (which memsets
`early_top_pgt[0 .. PTRS_PER_PGD-2]`, deliberately killing everything but the kernel map) and then
`copy_bootdata(__va(real_mode_data))`. **So the direct-map read is *supposed* to fault**, and the
early #PF handler is supposed to build the 2 MiB mapping for exactly that address and return, so the
`memcpy` is retried. `__startup_64` builds only the identity map (`early_top_pgt[0]`, `[1]` — the
port's `0x30ec063` each, measured in `results/x86-64-reach2.md`) and the kernel map (`[511]`).

**INTERPRETATION:** `PML4[273] == 0` before the fault is not the defect. The defect is in the
*repair*.

## 2. The repair runs — and repeats (MEASURED)

`V86C_BREAK=0xffffffff8ae770c0:0x200` (the `__early_make_pgtable`/`reset_early_page_tables` region,
runtime = link + the kernel's `0x7E00000` slide):

    BREAK 0 0xffffffff8ae771df (hit 1) rax=0xa0 rbx=0x84790 rcx=0x27 rdx=0x0            rsp=0xffffffff8a803f38
    BREAK 0 0xffffffff8ae771e6 (hit 2) rax=0xa0 rbx=0x84790 rcx=0x27 rdx=0xffffffff8aeea000
    BREAK 0 0xffffffff8ae771ed (hit 4) rax=0x0  rbx=0x84790 rcx=0x3fe rdx=0xffffffff8aeea000

`0xffffffff8ae771df` is `reset_early_page_tables`, entered from `__early_make_pgtable`'s `again:`
path (its call site is link `0xffffffff83077385`, taken when
`next_early_pgt >= EARLY_DYNAMIC_PAGE_TABLES`), with `rdx = 0xffffffff8aeea000` — the *fixed-up*
`early_top_pgt` — and `rcx = 0x3fe` = 510 dwords = the `memset` that clears `early_top_pgt[0..510]`.

**So the port does not skip the repair.** It enters it, the mapping does not take, the same fault
recurs, `early_dynamic_pgts` is consumed, and the reset wipes the table again — a repair loop.

**An instrument correction, said plainly:** an earlier break on
`0xffffffff8ae77300:0x180` reported "`__early_make_pgtable` never entered". That range missed the
function's entry (it starts near link `0xffffffff83077100`, not `0x83077300`). The silence was the
instrument's, not the guest's — the exact failure this lane has paid for three times.

## 3. The panic, named exactly (MEASURED + READ)

The halt loop is reached from `early_fixup_exception`, whose format string — READ from the image at
link `0xffffffff82346118` — is

    "PANIC: early exception 0x%02x IP %lx:%lx error %lx cr2 0x%lx\n"

and at that panic: `regs->cs` (`PEEK` of the frame at phys `0x2a03f20`) = **0x10** (so it is *not*
the `cs != __KERNEL_CS` branch), the flag at `0xffffffff824191cc` = 0, `early_recursion_flag`
(`0xffffffff8312c000`) = **1** (so not the `> 2` recursion branch), `trapnr` = **0x0e**. So the
panic is reached because `fixup_exception` returned 0 for a genuine #PF — i.e. after the repair
declined.

## 4. The fix that landed, and what it did and did not do (MEASURED)

`cpu_state.h` carried the gap in its own comment — *"`int32_t cr[8]` … NOT widened"* — and
`paging.c` repeated it: *"CR2 is 32-bit in this model (ss5.1); long mode's 64-bit CR2 is S11's"*.
But the repair's **first** act is `address - __PAGE_OFFSET` with `address = native_read_cr2()`, so a
truncated CR2 makes `physaddr` underflow and the guard `physaddr >= MAXMEM` refuse. The port's
`cr2` read `0x84790` for a fault at `0xffff888000084790` (its low half).

**Landed:** a `uint64_t cr2_64` beside `cr[2]` (which keeps the 32-bit model's projection untouched,
so no 32-bit path can move), set at the one place a #PF is raised (`trigger_pagefault`), and
returned by a **REX.W** `mov r64, cr2` in long mode; a 32-bit read and a non-long-mode read still
get `cr[2]` exactly as before.

**What it did not do:** the run is unchanged (4,475,558,722 instructions, `in_hlt=1`). So CR2's
width is not the binding constraint on the path actually taken — the repair is entered and fails
afterwards (§2), which is the next question, not a claim that the CR2 fix was wrong. It is a
precondition of the repair working at all, and it is now true.

## 5. The next fork, named

`__early_make_pgtable`'s body is at link `0xffffffff830771df…0xffffffff830774f0`; the two guards are
`physaddr >= MAXMEM` and `read_cr3_pa() != __pa_nodebug(early_top_pgt)`, and the store that must
land is `*pgd_p = …` (link `0xffffffff83077456` / `0xffffffff830774ce` in the two arms). The
instrument is in place: `V86C_BREAK` on the store, `V86C_PEEK` of the `early_dynamic_pgts` pages
(`0x30ec000+`), and `V86C_WATCH` on `PML4[273]` (`0x30ea888`) to see whether an entry is ever
installed and then lost. The three candidates, in the order this chain has paid for them: the guard
refusing (cr2/phys_base/cr3 comparison), the store computing its destination wrongly, or the entry
landing and being wiped by the `reset_early_page_tables` that follows it.

## 6. Verification (MEASURED; **code changed**, full battery, by EXIT CODE)

    make / selftest / nasm / nasm64        rc 0 (selftest: all suites green)
    make bench                             200095214
    make transcript-diff                   byte-identical to the reference
    make determinism                       two consecutive runs byte-identical
    gate-m2-5-determinism                  4,000,096,757 both runs, serials byte-identical
    gate-x86-64 12/0 · gate-long-mode 57/0 · gate-long-mode-sys 5/0 · gate-x86-64-diff 13/0
    gate-nasm64 13/0 · gate-reboot-1 39/0 · gate-pae-1 11/0 · gate-pae-l3 0 FAILING
    gate-vga-0..4                          0 FAILING each

**No pin moved** — as designed: `cr[2]` is untouched on every 32-bit path, and `cr2_64` is only read
by a REX.W `mov` in long mode.

**The calibrated target is NOT reached**: no first `printk`, so nothing was added to
`gate-x86-64.sh`.

## 7. Files touched

| file | change |
|---|---|
| `src/cpu/cpu_state.h` | `uint64_t cr2_64` beside `cr[8]`, with the gap's reason |
| `src/cpu/cpu_state.c` | reset it |
| `src/cpu/paging.c` | set it at `trigger_pagefault` |
| `src/cpu/instructions_0f.c` | REX.W `mov r64, cr2` in long mode returns the whole address; `mov cr2, r` stores it |
| `results/x86-64-dmap.md` | this report |
| `TODO.md` | §8 |
