# x86-64: THE GUEST BOOTS — the amd64 kernel runs to `kernel_init` and panics only for a missing root

**Status: MEASURED, one boot, no port stop. The pinned Debian `6.1.0-50-amd64` kernel now executes its
WHOLE boot sequence on the port: every initcall, PID 1 (`swapper/0`) started, the partition scan run,
and then — with no root filesystem on the command line — the kernel panics for exactly that reason.
`unimplemented=0`.** Claims tagged MEASURED / READ / INTERPRETATION.

## The boot (MEASURED)

    ./build/v86c --bzimage tests/images/vmlinuz-6.1.0-50-amd64 6000000000 --memory 256 \
        --bios build/minibios-256m.bin --cmdline 'console=ttyS0 earlyprintk=serial' \
        --serial /tmp/xi.txt --long-mode on
    -> unimplemented=0, 6,000,055,314 instructions (the budget), no named stop
    serial: 283 lines / 18,031 bytes, ending

    [ 1158.072042] clk: Disabling unused clocks
    [ 1158.137326] List of all partitions:
    [ 1158.142643] No filesystem could mount root, tried:
    [ 1158.153510] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)
    [ 1158.162330] CPU: 0 PID: 1 Comm: swapper/0 Not tainted 6.1.0-50-amd64 #1  Debian 6.1.176-1

That is the kernel's own end-of-boot path: ACPI/APIC (PIC mode), clocksource TSC, keyrings, LSM
(AppArmor/TOMOYO/LSM-eBPF), zswap/zbud, integrity/IMA/EVM, x509, SMP enumeration (1 CPU), MCE, the
partition scan, `clk` cleanup — and `PID 1` with `Comm: swapper/0`, i.e. `kernel_init` running as
init. **Nothing in that sequence is the port refusing anything.**

## How it got here (this session's chain, each link MEASURED)

| fix | what it removed |
|---|---|
| `7d72b15` `setcc_mem` int32_t→int64_t | a truncated address that faulted inside the oops printer and recursed until the stack was gone |
| `8d09c2c` `REX.W A9` (`test rax, imm32`) | the next missing 64-bit body |
| `bc3b832` `F3 REX.W 0F B8` (`popcnt r64`) | the next one (a bitmap walk) |
| `26006ee` `0F A4/AC` (`shld/shrd imm8`) | the next one |
| `2e27e09` 49 narrowing sites + a `>4 GiB` corpus seat | the whole `setcc` class (fxsave, 43 x87 helpers, lgdt/lidt, maskmov, the `ins` pre-check) |
| `5372f97` … | the instruments that named the last two |
| `b4b634d` BSWAP honours REX.B | **the last one**: `41 0F CC` (`bswap r12d`) was executing `bswap ESP`, byte-swapping the stack pointer in place |

The last bug is worth its own line because of how it was caught: `V86C_RSPJUMP` recorded
`deliv=0`, `rsp_in=0xffffd1fac056ba58`, `rsp_out=0x58ba56c0`, and the identity
`bswap32(0xc056ba58) == 0x58ba56c0` named the instruction — no delivery, no truncation, just the wrong
register. The lane's own rewrite rule for opcode-implied registers (`instructions.c:1400-1413`) had
missed the BSWAP family.

## The gate is now the strongest it has been (MEASURED)

`tests/gate-x86-64.sh`'s old step asserted the stop it used to end on ("a fault raised while delivering
a fault"). That stop is gone, so the step now asserts the boot's END: the root-mount panic present **and**
the log free of any `unimplemented` stop — so a regression to an earlier stop, a silent hang, or a new
refusal all fail here. Both directions are `must_fail`-checked against the messages that must NOT appear.

## What is next, and what it needs (INTERPRETATION)

`VFS: Unable to mount root fs` is the boundary between the kernel and USERSPACE. The next milestone is
`kernel_init` exec'ing `/init`, which needs a root filesystem:

* an **initramfs** (`--initrd`) is the cheap route — the existing fixtures are 32-bit i386 images, and
  an amd64 kernel can exec them only through IA-32 compatibility mode, which is a NAMED GAP in this port
  (long mode's CPL-3 path exists; the compatibility-mode entry from a 64-bit kernel is untested);
* a **64-bit root image** avoids that: a small amd64 busybox/initramfs would take the next step without
  touching the compatibility-mode question at all. That is the recommended next unit.
