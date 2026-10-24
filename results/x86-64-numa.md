# x86-64: the NUMA input, tested — the `-1` is NOT the missing SRAT

**Status: MEASUREMENT (two boots, no source change, no fix). The hypothesis this unit tested is
REFUTED: the port's kernel reads `-1` out of its per-cpu slot with ACPI **off and on**, and QEMU's
kernel — which also has **no SRAT** — never prints the warning at all. What the unit did find is that
the port's ACPI table set is now consumed by a 64-bit guest, intact, for the first time.** Claims
tagged MEASURED / READ / INTERPRETATION.

## Leg 1 — the port WITH `--acpi` (MEASURED, 256 MiB, 1m54s, exit 1)

    [    7.653116] ACPI: RSDP 0x00000000000E0000 000024 (v02 V86CVM)
    [    7.669069] ACPI: XSDT 0x00000000000E0080 000034 (v01 V86CVM V86CFW   00000001 V86C 00000001)
    [    7.690400] ACPI: FACP 0x00000000000E00C0 00010C (v05 V86CVM V86CFW   00000001 V86C 00000001)
    [    7.713458] ACPI: DSDT 0x00000000000E0210 000092 (v02 V86CVM V86CFW   00000001 V86C 00000001)
    [    7.734262] ACPI: FACS 0x00000000000E01D0 000040
    [    7.764550] ACPI: APIC 0x00000000000E02C0 00004C (v01 V86CVM V86CFW   00000001 V86C 00000001)
    [   10.628995] IOAPIC[0]: apic_id 0, version 17, address 0xfec00000, GSI 0-23
    [   10.660495] ACPI: Using ACPI (MADT) for SMP configuration information
    [   10.669508] smpboot: Allowing 1 CPUs, 0 hotplug CPUs
    [    7.852075] No NUMA configuration found
    [    7.858846] Faking a node at [mem 0x0000000000000000-0x000000000442ffff]
    [   29.812089] setup_percpu: NR_CPUS:8192 nr_cpumask_bits:1 nr_cpu_ids:1 nr_node_ids:1
    [   29.837006] percpu: cpu 0 has no node -1 or node-local memory
    [   29.846929] percpu: Embedded 62 pages/cpu s217088 r8192 d28672 u2097152

**The RSDP is found and the whole set parses** (RSDP→XSDT→FACP/DSDT/FACS/APIC, IOAPIC at
`0xfec00000` GSI 0-23, ACPI/MADT used for SMP) — the first time these tables have been offered to a
long-mode guest. **The `-1` message is unchanged** (`percpu: cpu 0 has no node -1`), and the run ends
at the **same stop** (a fault raised while delivering a fault), `4518902353` instructions vs
`4519091709` without ACPI — a different boot path, same wall.

## Leg 2 — QEMU at the same geometry (MEASURED, `-m 256 -kernel … -nographic`)

    [    0.049958] ACPI: RSDP 0x00000000000F52A0 000014 (v00 BOCHS )
    [    0.050384] ACPI: RSDT 0x000000000FFE2329 000034 (v01 BOCHS  BXPC     00000001 BXPC 00000001)
    [    0.052376] ACPI: APIC 0x000000000FFE2251 000078 (v03 BOCHS  BXPC     00000001 BXPC 00000001)
    [    0.052549] ACPI: HPET 0x000000000FFE22C9 000038 (v01 BOCHS  BXPC     00000001 BXPC 00000001)
    [    0.056125] No NUMA configuration found
    [    0.056216] Faking a node at [mem 0x0000000000000000-0x000000000ffdffff]
    [    0.072113] ACPI: Using ACPI (MADT) for SMP configuration information
    [    0.089340] setup_percpu: NR_CPUS:8192 nr_cpumask_bits:1 nr_cpu_ids:1 nr_node_ids:1
    [    0.095136] percpu: Embedded 62 pages/cpu s217088 r8192 d28672 u2097152

**No `SRAT` line on either machine** (`grep -c srat` = 0 in both serials), and QEMU prints the same
`No NUMA configuration found` + `Faking a node` — **but it never prints `cpu 0 has no node -1`.** It
boots on to `Kernel panic … VFS: Unable to mount root fs` (no root fs attached), i.e. straight through
the point where the port stops.

## The two sentences, side by side

Both machines publish **no SRAT** and both fake node 0, so the absence of an SRAT is *not* what makes
the port's kernel read `-1`; and offering the port's full ACPI set (`--acpi`) leaves the message and the
stop byte-for-byte where they were. The difference is therefore **not an ACPI/NUMA table the port
could publish** — the port's MADT already declares one LAPIC (uid 0, apic id 0, enabled), one IOAPIC and
the IRQ0→GSI2 override (`src/hw/acpi_tables.c:315-346`, READ), the same shape QEMU declares.

## What this leaves

The `-1` is kernel state that both machines' ACPI paths leave equal, so the next question is what the
kernel itself put there. `results/x86-64-drop.md` §c measured the writer: the slot at `gs+0x2e580` was
stored `0xffffffff` by the kernel at rip `0xffffffff8ae93b96`. Which per-cpu variable that slot is, is
still unidentified — and that is what the measurement below decides.

## The ONE next measurement

**The same instruction on both machines with `nokaslr`.** Boot the pinned kernel on the port with
`nokaslr` and `V86C_BREAK` on the reader's link address (`0xffffffff810ef8e5` in the KASLR'd run, so
re-derive it from the `nokaslr` run), then boot QEMU with `nokaslr` and break at the same link address
through its gdbstub — and compare the value the kernel reads into `eax` at that instruction. QEMU
reading `0` where the port reads `-1` puts the cause in a machine input the two still differ on (and
names it); both reading `-1` puts it in the kernel's own early state and moves the next unit off the
per-cpu path entirely.
