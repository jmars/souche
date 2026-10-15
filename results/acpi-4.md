# ACPI-4 — F1: the userspace console under `--acpi` (root cause + fix)

**Unit:** `HANDOFF=v86c-acpi`, UNIT ACPI-4 (`engineer`, fresh session after ACPI-3 died
twice on a provider fault). **Deliverable:** F1's root cause with the discriminating
evidence, the fix, F2 (if budget), F3's verdict, the gate table, divergences.

**Note on authorship:** the fix itself was ALREADY IN THE TREE as the dead session's
uncommitted `src/hw/acpi_tables.c` diff (FADT hardware-reduced → non-reduced + real PM
blocks); this unit *verified* it against the guest, proved the causality by mutation
(§1.2/§1.3), and confirmed the mechanism against the kernel's own boot lines. The tree I
received had `git status` = 1 modified file (`src/hw/acpi_tables.c`, +38/−13) and the
re-initramfs script's output. Nothing in the diff was signed off anywhere; its claim
("hardware-reduced ⇒ nr_legacy_irqs 0 ⇒ enable_IO_APIC bails before programming a single
redirection entry") is now MEASURED TRUE, byte for byte (§1.3): in the broken arm the
kernel writes ZERO `redir[...]` entries — not level-shaped ones, NONE — so the UART's
single THRI raise lands on the still-masked entry and is dropped (`EDGE LOST`), and the
interrupt-driven tty path never wakes. The brief's ranked suspect (a), the uart's
raise/lower shape, is exonerated: the same raise/lower code delivers 10 interrupts on
the same pin once the kernel is told the machine has a legacy layer.

## 1. F1 — root cause

### 1.1 The signature, and what was already measured

ON arm (old, reduced FADT): dmesg streams to `Run /init as init process` then silence;
init's echoes produce ZERO bytes; machine idles; `preallocated irqs: 0` in the boot log
(results/acpi-2-f1-console-freeze-serial.txt:76). OFF arm prints everything;
`preallocated irqs: 16` (results/acpi-2-g2-off.txt:69).

### 1.2 The A/B, this unit (MEASURED, runs below)

Build with the tree's FADT change → guest prints the init's COMPLETE output, and its own
`/proc/interrupts` shows `  4:   10   IO-APIC   4-edge      ttyS0` — irq 4 routed to
IOAPIC pin 4, EDGE-triggered, counting delivered interrupts. The ioapic trace shows the
kernel programming `redir[4] <- 00000824 (vec 24, edge, unmasked)`.

Mutation (the flag alone flipped back to hardware-reduced, PM blocks kept): see §1.3.

**The G2 oracle, applied literally** (ON must print everything OFF prints): the init
section of both arms diffed — every `ACPI-INIT:` echo line, the full
`/proc/interrupts`, the dmesg grep and the ttyS0 probe line are present in both. The
diff's only hunks are the intended machine differences: the irq label lines
(`XT-PIC` → `IO-APIC  4-edge` etc. — that substitution IS the deliverable), and the
dmesg grep's content (the ON arm carries the ACPI discovery lines the OFF arm cannot
have; the OFF arm's `ACPI BIOS Error (bug): A valid RSDP was not found` is the flag-off
contrast). Raw: /tmp capture pairs kept at results/acpi-4-*.txt (this unit).

### 1.3 The mutation (G5): the reduced flag alone, PM blocks kept

`wr32le(fadt + 112, (1u << 0) | (1u << 20))` — only the flag restored; `make`, guest
rerun with the trace. Result (MEASURED):

- the console dies again with the exact F1 signature: `preallocated irqs: 0`
  (/tmp/acpi4-mut.txt:76), dmesg streams to `Run /init as init process` and ZERO
  ACPI-INIT lines ever appear;
- the kernel writes **zero** `redir[...]` redirection entries in the whole run (the
  fixed arm writes entries for pins 1,2,4,5..12,23 — `redir[4] <- 00000824 (vec 24,
  edge, unmasked)`);
- the single uart raise is trace-caught in the act:
  `uart0 IER write 0f: THRI 0->1, re-throw` → `RAISE irq (iir=02 ier=0f ints=04)` →
  `ioapic SET pin 4 (line 0->1; entry masked, edge, vec 00)` → `EDGE LOST` → the kernel
  disables IER again (`IER write 00 (was 0f)`) and the tty write path never wakes.

So the flag IS the load-bearing bit, and the gate (the init-output comparison §1.2)
catches the breakage — fail-before/pass-after asymmetry proven on the deliverable
itself.

### 1.4 The mechanism, in the kernel's own code (READ: linux v6.6)

`arch/x86/kernel/acpi/boot.c`: `acpi_generic_reduced_hw_init()` — "Override x86_init
functions and bypass legacy PIC in hardware reduced ACPI mode" — sets
`legacy_pic = &null_legacy_pic` when ACPICA took the FADT's HW_REDUCED bit at its word
(our FADT claimed there is no PC-AT layer; the machine has a full one: PIC, PIT, ISA
UART). `nr_legacy_irqs()` then reads 0 (`early_irq_init`: "preallocated irqs: 0"),
`arch_early_ioapic_init()` flips `io_apic_irqs = ~0UL`, and the ISA-IRQ wiring loop of
`setup_IO_APIC_irqs()`/`enable_IO_APIC()` — the loop that would have programmed
`redir[4]` with the ISA-default EDGE trigger — runs for no pin. The FADT was the lie:
hardware-reduced is a *platform declaration*, not a convenience, and ACPICA and the
x86 irq layer both act on it. The OFF arm is unaffected (no tables are published).

The fix is therefore the honest FADT, which is what the dead session's diff already
wrote: WBINVD only (not hardware-reduced), the real PM blocks named at the PIIX4
device's own registered ports (PM1a_EVT 0xB000 len 4, PM1a_CNT 0xB004 len 2, PM_TMR
0xB008 len 4, GPE0 0xAFE0 len 4 — each matching src/hw/acpi.c's `io_register_*` calls),
SMI_CMD=0 ("already in ACPI mode"), IAPC_BOOT_ARCH legacy+8042 (both true). The guest's
own confirmations: `ACPI: PM-Timer IO Port: 0xb008`, `preallocated irqs: 16`,
`ACPI: Interpreter enabled`, `ACPI: PM: (supports S0)`.

## 2. F3 — the reboot arm's soft lockup: benign in BOTH arms; the arm's real blocker is F2

Retest AFTER the F1 fix (same tree, same 4e9 budget, same reboot image):

- **ON arm** (`--acpi`): boots, `GUEST-INIT: start` PRINTS (F1's fix holds on this arm
  too), the watchdog fires at `sha256_transform_blocks+0x544/0xa70` inside
  `finit_module` (busybox loading modules), the init SURVIVES it, waits 400 rounds for
  /sys/block/sda, finds nothing, ends `GUEST-INIT: NO DISK, nothing to prove`
  (results/acpi-4-reboot-serial.txt).
- **OFF arm** (no flag, results/acpi-4-reboot-off-serial.txt): enumerates
  `00:00.0 [8086:1237]`, `00:01.0 [8086:7000]`, `00:1e.0 [8086:7010]` (the legacy IDE
  quirk BARs and all), shows the IDENTICAL soft lockup at the SAME EIP in BOTH of its
  boots — and completes `DISK PERSIST OK` with two boots and a clean poweroff.

So the lockup is NOT F1's sibling and not a machine bug: it is the module signature
check's sha256 being instruction-slow on this interpreter — more than 22 guest-seconds
inside one kernel loop — a transient the init outlives in both arms (the OFF arm's two
boots each show it and each reaches the marker). MEASURED, discriminated by the
identical-EIP pair. What actually blocks the ON arm is **F2**: zero `pci 0000:` lines
under `--acpi` (no root bridge in the empty DSDT) → no IDE function → no /dev/sda →
the reboot arm's disk write can never be proven. Fix F2 and this arm is expected to
follow; the lockup line itself needs nothing.
