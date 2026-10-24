# x86-64: nokaslr at 12 G does not reach the VFS panic — it deadlocks on a spinlock; the KASLR stop is a truncation

**Status: MEASUREMENT ONLY (three boots, no source change). The configuration that does not stop by name
does not reach the calibrated target either: at 12 G instructions it is spinning on a SPINLOCK, at the
same instruction and with the serial frozen at the same line. And the KASLR stop symbolicated to a
stack-address TRUNCATION, not to the decompressor's failed physical placement.** Claims tagged
MEASURED / READ / INTERPRETATION.

## 1. nokaslr at a 12 G budget (MEASURED)

    v86c boot: 12000004859 instructions, in_hlt=0, exception=-1, unimplemented=0, eip=0x81a4f6e5

**It runs out again — no named stop** (`unimplemented=0`, rc 0, 3m41s wall). The serial's last six lines
are byte-for-byte the 6 G run's, ending:

    [   74.169983] APIC: Keep in PIC mode(8259)
    [   74.181450] clocksource: tsc-early: mask: 0xffffffffffffffff …
    [   74.203734] Calibrating delay loop (skipped), value calculated using timer frequency.. 2000.00 BogoMIPS (lpj=4000000)
    [   74.206068] Last level iTLB entries: 4KB 64, 2MB 64, 4MB 64
    [   74.206068] Last level dTLB entries: 4KB 64, 2MB 0, 4MB 64, 1GB 0
    [   74.206068] Spectre V1 : Mitigation: usercopy/swapgs barriers and __user pointer sanitization

The budget doubled and **not one serial byte moved** — the guest is not progressing, it is looping. The
loop is (READ, `objdump` at `0xffffffff81a4f6e5`, which is a link address under `nokaslr`):

    ffffffff81a4f6e0: mov    $0x1,%edx
    ffffffff81a4f6e5: mov    (%rbx),%eax        ; ← the eip at the budget exit
    ffffffff81a4f6e7: test   %eax,%eax
    ffffffff81a4f6e9: jne    0xffffffff81a4f6fe ; pause, and round again
    ffffffff81a4f6eb: lock cmpxchg %edx,(%rbx)
    ffffffff81a4f6f1: jne    0xffffffff81a4f6e5

That is `native_queued_spin_lock`'s acquisition loop, and it never succeeds: the word at `(%rbx)` is
non-zero and, on a one-CPU machine, nobody else can release it. **So the configuration to push does not
reach the VFS panic (`Unable to mount root fs`) — it DEADLOCKS on a spinlock at ~74 s of boot**, and the
next step for it is to identify the lock, not a longer budget. (This is also why it must NOT be added to
`tests/gate-x86-64.sh` as a VFS-panic step: the console stops at the Spectre V1 line in this
configuration too.)

## 2. The KASLR stop, symbolicated (MEASURED)

Slide recovery: this run's dump prints `idtr=0xfffffe0000000000` (the CPU-entry-area alias, which does not
slide), so the slide comes from the kernel-image addresses the two configurations print for the SAME
symbol — the `nokaslr` run's dump has `idtr=0xffffffff83310000` and a KASLR run's `0xffffffff8b110000`:
**slide = 0x7E00000**. With it, the stop `0xffffffff897e42c6` is link **`0xffffffff819e42c6`**, and the
disassembly there is consistent with the run's registers (`rbp = rsp + 6`):

    ffffffff819e42c6: movq   $0x0,0x6(%rsp)     ; ← rsp = 0xffffd206bfffffc0, so the write is …ffc6
    ffffffff819e42cf: movq   $0x0,0x298(%rsp)
    ffffffff819e42ea: rep stos %rax,(%rdi)

and the state at the stop: `cr2=0xbfffff90`, `rsp=0xffffd206bfffffc0`, `cr3=0x2a10000`, `gs_base=0xffff8c16c0a00000`.

**`cr2 = 0xbfffff90` is the LOW 32 BITS of a stack address** (`0xffffd206bfffff90`) — the same 32-bit
truncation class this lane has now fixed three times, on a stack the `nokaslr` layout never touches.

## 3. One sentence each

- **The KASLR stop is a real kernel path the port breaks, not the failed physical placement**: the guest
  faults on a stack address that has been truncated to 32 bits, which is a port defect, and the KASLR
  layout is only what makes the kernel take that path.
- **The `nokaslr` configuration's stop is a different defect entirely**: a spinlock that is never
  released, on a one-CPU machine, after the Spectre V1 mitigation line.

## The ONE next measurement

**Identify the lock.** Break at `0xffffffff81a4f6e5` under `nokaslr` (a ≥5 G budget; the loop is entered
at ~4.5 G) and read `rbx` — the lock's address names the object, and the caller (`rip` on the stack, via
`V86C_PEEK_SP`) names the code that took it and never released it. That is the configuration that makes
progress, so it is the one whose defect buys the most.
