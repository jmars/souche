# x86-64: the SS slot holds a RETURN ADDRESS — and the port reaches the *second* `copy_bootdata`

**Status: MEASUREMENT. The question "what should be in the early stub's SS slot" is answered, and the
answer is that **nothing puts anything there**: for a same-privilege exception hardware pushes no
SS, so the slot the kernel's return path reads holds whatever the stack held — here,
`copy_bootdata`'s **return address**. Whether the `swapgs` branch is taken then depends on bit 2 of
that return address's low byte, and the two call sites of `copy_bootdata` differ in exactly that bit.
QEMU takes the first call site and `iretq`s; the port reaches the **second** call site and takes the
`swapgs`.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. QEMU as the reference (MEASURED)

`-d int` needs TCG (KVM logs nothing — the previous attempt's empty log was the instrument's
silence, not a guest fact). Same image, `-m 256`, `earlyprintk=serial nokaslr` (so QEMU's slide is 0
and its addresses are link addresses):

    qemu:     0: v=0e e=0000 i=0 cpl=0 IP=0010:ffffffff83077246 pc=ffffffff83077246 \
                 SP=0000:ffffffff82a03f30 CR2=ffff888000014790

* **the same instruction** as the port's first fault (`0xffffffff83077246`, `copy_bootdata`'s
  `rep movsd`), error code 0, `cpl=0`;
* **`SP=0xffffffff82a03f30`** — and the port's `rsp` at that instruction, de-slid, is
  `0xffffffff82a03f30` **exactly** (`BREAK 0xffffffff8ae77246 … rsp=0xffffffff8a803f30`, slide
  `0x7E00000`);
* QEMU then takes three more #PFs (`0xffffffff8309c396`, `0xffffffff8309c3b3`, `0xffffffff819eb952`,
  all repaired on demand) and **boots to the banner**;
* **QEMU never faults at `pc=ffffffff81c0123a`** — the `mov %gs:0x19000` — **zero hits**. So on
  hardware the `swapgs` branch is *not* taken.

## 2. The stub's own expectation (READ, and it agrees with the port)

Vector 14's stub, from the image:

    ffffffff8307707e  push $0xe                    ← the vector only
    ffffffff83077080  jmp  0xffffffff83077120

No SS/RSP — correct, the CPL does not change. And `early_idt_handler_common` pushes **fourteen**
registers (`push rsi`, then `rdx, rcx, rax, r8…r11, rbx, rbp, r12…r15`), writing `rdi` into the
frame's first slot (`mov %rdi,0x8(%rsp)` = `orig_ax`). So the restore path's `pop ×15` + `add rsp,8`
lands **exactly** on `ip`, and the port's measured `rsp` at that point (`F+0x10`, the rip slot)
confirms the alignment. **The frame is not the defect**: `ip`, `cs`, `flags` are all correct on the
port, and the runtime bytes at `0xffffffff81c01214` are **identical to the image** (no alternatives
patch), so the code is not the defect either.

## 3. What the SS slot actually contains (MEASURED)

`0x20(%rsp)` at `0xffffffff89a01219` resolves to `F+0x30` — the word **above** the pre-exception
`SP`, i.e. the caller's frame:

    F+0x28 = 0x84790              ← copy_bootdata's `push %rbx` (the boot_params pointer)
    F+0x30 = 0xffffffff8ae7760d   ← copy_bootdata's RETURN ADDRESS

So the kernel's `testb $4` is reading a **return address** as an SS selector. On hardware that is
equally true — but the return address's low byte differs per call site, and bit 2 is what the branch
tests:

| call site | return address | bit 2 of low byte | branch |
|---|---|---|---|
| `0xffffffff83077555` — `x86_64_start_kernel` (QEMU's fault) | `…8307755a` | `0x5a` → **clear** | `iretq` ✓ |
| `0xffffffff83077608` — `x86_64_start_reservations` (the port's fault) | `…8307760d` | `0x0d` → **set** | `swapgs` ✗ |

**That is the whole divergence at this link**, and it is confirmed by the port's own stack: the
return address it left is `0xffffffff8ae7760d` (link `0xffffffff8307760d`), which is the instruction
after the call at `0xffffffff83077608` — the *second* `copy_bootdata` call site.

## 4. Why the `swapgs` then kills the machine (MEASURED)

`swapgs` with `gs_base = 0xffffffff8ae42000` and **`kernel_gs = 0`** sends `GS.base` to 0, so the
very next instruction's `%gs:0x19000` resolves to the bare `0x19000`; `early_make_pgtable(0x19000)`
computes `physaddr = 0x19000 - 0xffff888000000000` (wrapped, `>= MAXMEM`) and refuses — the
demand-repair only knows direct-map addresses — the kernel panics, and the third nested fault trips
`early_recursion_flag > 2`, which halts **silently**. And the image contains **no write of MSR
`0xC0000102` at all** (`mov ecx,0xc0000102` appears zero times), so `kernel_gs = 0` is faithful and a
`swapgs` on this path can never be harmless.

## 5. The finding, and the next fork

**The kernel's early return path reads a return address as an SS selector** — fragile, but that is
the kernel's own code and hardware survives it by luck of the call site. The **port's** divergence is
that it reaches the *second* call site at all: that call is guarded by
`if (!boot_params.hdr.version)`, so `boot_params.hdr.version` was still **0** after the first
`copy_bootdata` — i.e. **the first copy did not land**.

**Next fork, named and instrumented:** why the first `copy_bootdata`'s `memcpy` did not copy. Its
source read was repaired and retried (the demand map is present — `PML4[273] = 0x30ec063`), and its
destination `&boot_params` = `0xffffffff8b117600` has a **present** 2 MiB page (`PDE[88] =
0x032001e3`, measured in `results/x86-64-dmap.md`). The instrument is `V86C_WATCH` on the first 16
bytes of `&boot_params` (phys `0x3317600`) — if nothing writes them, the `rep movsd` did not run its
iteration; if it does write, the value is what to compare with the source at `0xffff888000084790`.

**The calibrated target is NOT reached**: no first `printk`, so nothing was added to
`gate-x86-64.sh`.

## 6. Verification (MEASURED; **no source change** — instruments only, so the subset)

    make                     rc 0
    make selftest            rc 0 — all suites green
    gate-x86-64.sh           12 passed, 0 failed
    make bench               200095214

No pin moved. The full battery was not re-run because nothing under `src/`, `gen/` or `tests/`
changed this unit — `git status` shows only this report and `TODO.md`.

## 7. Files touched

| file | change |
|---|---|
| `results/x86-64-ssslot.md` | this report |
| `TODO.md` | §8 |
