# x86-64: the same instruction on both machines, `nokaslr` — QEMU reads 0, the port reads −1

**Status: MEASUREMENT (one port run + one QEMU run, no source change, no fix).** The link address
predicted in `results/x86-64-numa.md` was **off by 0x200000** — the real one is `0xffffffff812ef8e5`
(the reader) / `0xffffffff812ef8ec` (the `cltq`), confirmed by the `nokaslr` run itself, which stops at
`eip=0xffffffff812ef901`. Claims tagged MEASURED / READ / INTERPRETATION.

## The two values, side by side

    port (nokaslr)  rip=0xffffffff812ef8ec  rax=0xffffffff  rbx=0xffff888004426bc0 rdx=0x1 rsi=0x400 rdi=0x3 rbp=0xffffffff8314d320
    QEMU (nokaslr)  rip=0xffffffff812ef8ec  rax=0x00000000  rbx=0xffff88800ffab840 rdx=0x1 rsi=0x400 rdi=0x3 rbp=0xffffffff8314d320

MEASURED. Port: `V86C_BREAK` on `0xffffffff812ef8ec` (one hit), `nokaslr` in the cmdline, 256 MiB,
exit 1 at the same delivery fault, `4516977946` instructions. QEMU: `-S -gdb tcp:127.0.0.1:1234`,
`Z1,ffffffff812ef8ec,1` over the RSP socket (no gdb binary), `c`, `g` — one stop, registers above.

**Every register agrees except the two that must not: `rax` (the value loaded by
`mov %gs:0x76f3ec94(%rip),%eax` at `0xffffffff812ef8e5`) and `rbx` (a direct-map pointer).** Same
instruction, same code path, same arguments (`rdx=1 rsi=0x400 rdi=3 rbp=0xffffffff8314d320`), and the
same un-randomised `page_offset_base` (`0xffff888000000000`) on both machines.

## One sentence of interpretation

**QEMU reads `0` where the port reads `-1`, so the value is a machine-dependent per-cpu int and not a
port arithmetic error — the port's kernel believes it is running on no CPU/no node (`-1` is
`NUMA_NO_NODE`), and the input that supplies that identity is the ACPI cpu→node topology: the port's
DSDT is a 110-byte PCI0-only stub with no processor object and no `_PXM` and it publishes no SRAT
(`src/hw/acpi_tables.c:183-195`, and the header's own "an SRAT … is DELIBERATELY LEFT OUT"), while
QEMU's 0x219D-byte DSDT carries the processor namespace — so the device that could supply it is the
port's ACPI builder (a `\_SB` processor object with `_PXM`, and/or an SRAT).**

**Caveat, stated because it is not yet excluded:** QEMU publishes **no SRAT either** (`grep -c srat` = 0
in its serial, `results/x86-64-numa.md`), so the SRAT alone is not proven to be the missing piece; the
part QEMU demonstrably has and the port demonstrably lacks is the **DSDT processor namespace**. The
competing reading — that the slot is the per-cpu `cpu_number` and `-1` is kernel state no device
supplies — is not excluded either: the pointer array being indexed (`0xffffffff8aa393e0`) is the one
this lane's halves unit identified as `__per_cpu_offset[0]`, which would make the index a CPU id.

## What would settle it (the next measurement)

Symbolicate the two link addresses this lane now holds — the writer of the `-1`
(`0xffffffff83093b96`, `results/x86-64-drop.md` §c) and the reader (`0xffffffff812ef8e5`) — against the
kernel's own kallsyms (extractable from the decompressed `vmlinux` in the bzImage), which names the
per-cpu variable and the function in one step and turns the paragraph above into a single fact.
