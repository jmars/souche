# x86-64: only the UNCONDITIONAL `copy_bootdata` runs — the branch is bit 2 of a return address

**Status: MEASUREMENT. The control-flow bisect the brief asked for is done: the port executes
**one** of the two `copy_bootdata` call sites — the unconditional one in `x86_64_start_kernel` — and
the branch that decides the rest of the boot is `testb $4` on that call's **return address**. No fix
is claimed: the discriminator that would name which side is wrong (QEMU's stack word) needs a
breakpoint tool this host does not have, and the method is recorded in §5.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. Which call sites execute (MEASURED)

`V86C_BREAK=0xffffffff8ae77555:1,0xffffffff8ae77608:1`, `BREAK_MAX=2` each, one run:

    BREAK 1 0xffffffff8ae77608 (hit 1) rax=0x0 rbx=0x84790 rcx=0x0 rdx=0xffffffff8a803eb8
                                      rsi=0x1f0 rdi=0xffff888000084790 rbp=0xa0 rsp=0xffffffff8a803f40

**Slot 1 is hit; slot 0 is never hit.** So the port executes the call at `0xffffffff8ae77608` — and
never reaches the other one, because it dies inside this invocation.

## 2. Which function each site is in (READ)

    0xffffffff83077544  cmpw $0x0,0xffffffff83317806   ← boot_params.hdr.version
    0xffffffff8307754c  jne 0xffffffff8307755a
    0xffffffff8307754e  add [page_offset_base],%rdi    ← __va(real_mode_data)
    0xffffffff83077555  call 0xffffffff8307722e        ← SITE A (guarded)
    0xffffffff8307755a  call 0xffffffff83077686
    0xffffffff8307755f  jmp  0xffffffff83077e46

    0xffffffff830775b2  push %rbp ; push %rbx ; mov %rdi,%rbx
    0xffffffff830775b7  mov %cr4,%rax
    0xffffffff830775ba  mov %rax,%gs:0x31558          ← cr4_init_shadow()
    …
    0xffffffff83077608  call 0xffffffff8307722e        ← SITE B (unconditional, x86_64_start_kernel)

So **site A** is `x86_64_start_reservations`'s `if (!boot_params.hdr.version) copy_bootdata(...)` and
**site B** is `x86_64_start_kernel`'s unconditional `copy_bootdata(__va(real_mode_data))` — the
*first* of the two in program order. **The port dies in the first one.**

## 3. The condition the branch actually tests (MEASURED)

From `results/x86-64-ssslot.md` and this unit's frames: `restore_regs_and_return_to_kernel`'s
`testb $4,0x20(%rsp)` resolves to the word **above** the pre-exception SP, which is
`copy_bootdata`'s **return address**:

| call site | return address | low byte | bit 2 | branch |
|---|---|---|---|---|
| B — `0xffffffff83077608` (unconditional; **the port's**) | `…8307760d` | `0x0d` | **set** | `swapgs` → GS.base = 0 → `gs:0x19000` → refuse → panic |
| A — `0xffffffff83077555` (guarded; **QEMU's, by elimination**) | `…8307755a` | `0x5a` | clear | `iretq` → boot continues |

`kernel_gs = 0` at that `swapgs` (MEASURED), and the image contains no write of MSR `0xC0000102` in
`.init` at all (the 30 sites are all in the main text, i.e. later), so a `swapgs` on this path can
never be harmless — **the branch must not be taken**, which is exactly what QEMU's log shows (zero
faults at `pc=ffffffff81c0123a`).

## 4. What that implies, and why it is the real finding

QEMU takes the **guarded** site and the port the **unconditional** one. The guarded site is reached
only when `boot_params.hdr.version == 0`, i.e. **after a successful unconditional copy the guarded
call is skipped**. So QEMU's fault must be the guarded one, and therefore on QEMU the
**unconditional** `copy_bootdata`'s direct-map read **did not fault** — while the port's did
(`cr2 = 0xffff888000084790`, MEASURED).

**That is the divergence to chase**: not the frame, not the stub, not the return address (both are
the kernel's own code and both are faithful), but **whether the first direct-map read of
`real_mode_data` faults**. QEMU's log shows it faulting *once* somewhere (fault 0, `cr2 =
0xffff888000014790`), so QEMU's mapping was already present for the unconditional call and the fault
came later — or QEMU's fault 0 *is* the unconditional call and the return-address reasoning above is
wrong.

## 5. The one-line discriminator, and why it is not in this report

The two readings in §4 are separated by exactly one word: **QEMU's stack word at physical
`0x2a03f38`** — the return address above QEMU's own pre-exception `SP` (`0xffffffff82a03f30`, from
its `-d int` log). If it is `0xffffffff8307760d`, QEMU also faulted at the unconditional call and the
divergence is elsewhere; if it is `0xffffffff8307755a`, QEMU's unconditional call did not fault and
the divergence is the first demand-map repair.

**Not obtainable here:** the host has no `gdb`, and QEMU's monitor cannot be timed into a
milliseconds-wide window (KVM boots past it before a socket client can issue `stop`). The method for
whoever has `gdb`:

    qemu-system-x86_64 -s -S -kernel tests/images/vmlinuz-6.1.0-50-amd64 -m 256 -nographic \
        -append 'console=ttyS0 earlyprintk=serial nokaslr'
    gdb> target remote :1234
    gdb> hbreak *0xffffffff83077246
    gdb> continue
    gdb> x/4gx $rsp

**The calibrated target is NOT reached**: no first `printk`, so nothing was added to
`gate-x86-64.sh`.

## 6. Verification (MEASURED; **no source change** — instruments only, so the subset)

    make                     rc 0
    make selftest            rc 0 — all suites green
    gate-x86-64.sh           12 passed, 0 failed
    make bench               200095214

No pin moved. `git status` shows only this report and `TODO.md`.

## 7. Files touched

| file | change |
|---|---|
| `results/x86-64-branch.md` | this report |
| `TODO.md` | §8 |
