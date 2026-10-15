# ACPI-6 — the `_PRT`: declaring the PCI interrupt routing the machine actually has

**Unit:** `HANDOFF=v86c-acpi`, UNIT ACPI-6 (coder). **Input:** the brief's measured
mechanism (pci.c's PIRQ lookup at interrupt time; minibios's one programmed route),
results/acpi-5.md (the two warn lines), results/acpi-1r-review.md N3. THIS FILE IS
APPENDED AS THE WORK PROGRESSES.

## Status

- [x] Design decision: STATIC `_PRT`, one entry (the decision + its evidence below)
- [x] G5 the `_CRS` bus-number window (the `[Firmware Bug]: no secondary bus range` fix)
- [x] AML + verifier rewrite (acpi_tables.c); MADT moved 0x280 -> 0x2C0 (the DSDT grew
      past it); blob length 0x2CC -> 0x30C (all consumers read the macros)
- [x] G3 verify() pins: the entry decoded, its parent_pin arithmetic RECOMPUTED, the GSI
      pinned to VIRTIO_IRQ; probes 12/13/14/15 able-to-fail
- [x] selftest 690 checks 0 failures (682 + exactly the 4 probes' 2 CHECKs each)
- [x] make rc 0; make nasm 118/118; make transcript-diff byte-identical (flag off)
- [x] make gate-reboot-1 39/39 (flag off)
- [x] G1/G2 the virtio ON arm under `--acpi` (warn lines gone; `11-fasteoi` = the route used)
- [x] G2 the `--acpi` reboot arm (DISK PERSIST OK)
- [x] G4 the determinism gate at its pinned count (4,000,024,062); aarch64 AArch64

## THE DESIGN DECISION: static, not link devices

**What is declared** — one `_PRT` entry on PCI0:

```
Package (0x04) { 0x000CFFFF, Zero, Zero, 0x0B }
```

address 0x000CFFFF = device 12 (the virtio console at 00:0c.0), ALL functions (pci.c's
routing is function-agnostic: `device = pci_id >> 3`); pin Zero = INTA; source Zero =
the STATIC form (Linux: `gsi = source_index`, no link object); source_index 11 = the
value `tests/minibios.asm`'s `pci_irq_route` writes to the PIIX3 router byte 0x63
(VIRTIO_IRQ) — the same value it writes to the device's interrupt-line byte.

**Why the entry is TRUE of this machine:** `src/hw/pci.c`'s `pci_raise_irq` computes
`parent_pin = (pin + device - 1) & 3` (for 00:0c.0 INTA: (0 + 11) & 3 = 3) and reads the
ISA bridge's router byte `0x60 + parent_pin` at interrupt time — 0x63. The firmware
programs 0x63 = 11 and deliberately leaves 0x60-0x62 at 0 (the IDE raises on the legacy
lines 14/15 directly — `ide.c` never calls `pci_raise_irq` — so its measured behaviour
is untouched BY CONSTRUCTION). The entry declares exactly the one route the firmware
programs; every other (device, pin) pair would be an F1-class false declaration and is
not declared.

**Why STATIC and not the LNKA-D link devices:** (1) a link-device `_SRS` hands the
guest a write path into 0x60+n — it invites the kernel's link allocator (and
acpi_irq_balance) to program the three bytes whose ZERO state is a measured lane
property, and its allocation choices would have to be re-measured on every gate; the
static entry names the line the firmware itself programs, so ACPI and the hardware
agree because both quote the same firmware constant. (2) Linux 6.6 reads a static entry
as `gsi = entry->index` and registers it LEVEL/LOW (drivers/acpi/pci_irq.c
acpi_pci_irq_enable's defaults — verified in the fetched v6.6 source, `/tmp` copy
shown in the memory record); a link device would have needed the same trigger decision
anyway (seabios's links use IRQNoFlags = edge; QEMU's modern piix uses level in _PRS —
either way the kernel programs the entry, and our ioapic takes both). (3) LEVEL/LOW is
safe for delivery on this machine: the virtio line is LOWERED when the guest driver
reads the ISR register (`virtio.c` isr_status_read -> pci_lower_irq), so a level
redirection entry cannot re-fire into a storm.

**The honest caveat, stated instead of hidden:** "GSI 11" is true while minibios's
programming stands — it writes 0x63 only when a virtio console is seated at 00:0c.0,
and the entry is consulted only when such a device exists to look it up (the kernel
matches `_PRT` entries by the device's address; an absent device never consults).
The verify() pin recomputes the parent_pin arithmetic from the entry's OWN fields and
pins the GSI to 11, so any future edit that moves the route to an unprogrammed byte
(or changes the firmware's constant without this one) refuses before any guest boots.

**What the guest now programs: NOTHING NEW.** The static route gives the guest no
object whose methods write the PIIX router bytes. The kernel's only new act is calling
`acpi_register_gsi(11, level, low)` — programming the IOAPIC's pin-11 redirection entry
(LEVEL, active-low) instead of leaving pin 11 to the ISA-fallback edge path. 0x60-0x63
keep their firmware values; the entry is read, never written.

## G5 — the bus-number window

PCI0's `_CRS` gains `WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
0x0000, 0x0000, 0x00FF, 0x0000, 0x0100)`: the config mechanism's bus field is 8 bits
(0x0CF8 bits 23:16) and every value decodes, so 0x00-0xFF is the true window. Byte
shape per QEMU's aml-build.c (type byte 0x02, the same fixed/decode flags as the IO
windows, type-specific flags 0 = reserved for bus numbers). This is the pre-existing
`[Firmware Bug]: no secondary bus range in _CRS` line's fix — GATE RESULT BELOW.

## The pins (G3)

- the `_PRT` bytes: NameOp "_PRT", the outer package (1 entry), the inner package
  (4 elements), the DWordPrefix address, pin Zero, source Zero, BytePrefix GSI.
- the ARITHMETIC: `prt_addr` is decoded from the entry and `parent_pin =
  (pin + (prt_addr >> 16) - 1) & 3` is RECOMPUTED — it must be 3 (PIRQD, 0x63); the
  function field must be 0xFFFF; the GSI byte must be 11.
- the bus window: type 0x02, min 0, max 0xFF, len 0x100.
- able-to-fail probes (hw_selftest.c, each with the checksum REPAIRED so the FIELD
  check is what catches it): #12 GSI 11->10; #13 address device 12->13 (parent_pin
  becomes 0 = PIRQA, the unprogrammed byte); #14 pin INTA->INTB (parent_pin 0 again);
  #15 bus-window end 0xFF->0x7F.

## Gates

| gate | command | result |
| --- | --- | --- |
| build | `make` | rc 0, -Werror clean |
| selftest | `make selftest` | all suites green; **hw 690 checks, 0 failures** (682 + 8 = exactly probes 12-15; nothing else moved) |
| nasm | `make nasm` | **118/118**, rc 0 |
| transcript-diff | `make transcript-diff` | byte-identical (flag off), rc 0 |
| reboot gate | `sh tests/gate-reboot-1.sh` | **39 passed, 0 failed**, rc 0 |
| determinism | `sh tests/gate-m2-5-determinism.sh all` | **rc 0**; the two runs executed the SAME instruction count (**4,000,024,062**, acpi-5's pinned count, unchanged) |
| aarch64 | make's own cosmocc link | `build/v86c.aarch64.elf` rebuilt this session: ELF64, **Machine AArch64** (readelf -h), statically linked; `build/hw_selftest.aarch64.elf` ditto |

## G1 — the before/after (results/acpi-6-virtio-on-serial.txt vs results/acpi-5-virtio-on-serial.txt)

BEFORE (acpi-5, lines 283-284):

```
[  503.117841] virtio-pci 0000:00:0c.0: can't derive routing for PCI INT A
[  503.129284] virtio-pci 0000:00:0c.0: PCI INT A: no GSI
```

AFTER (this unit's ON arm): **the VIRTIO's two lines are GONE** **CORRECTION (orchestrator, MEASURED): the IDE's equivalent line is NOT gone and is not meant to be** — `ata_piix 0000:00:1e.0: can't derive routing for PCI INT A` still prints in the reboot arm (`results/acpi-6-reboot-on-serial.txt:316,651`, once per boot), because the `_PRT` declares exactly ONE route (the one the firmware programs) and the IDE's is deliberately undeclared — the IDE raises on the legacy lines 14/15 and never consults a PIRQ byte, so it has no GSI to derive. Counting: virtio arm 0, G2 arm 0, reboot arm 2. The sentence above was scoped to the virtio in the capture it quotes but read as if it covered every device.) VIRTIO's lines are gone (`rg -c "can't derive routing"`:
old 2, new 0). The derivation's visible signature is the IOAPIC entry the kernel now
programs through the `_PRT` route — the no-`_PRT` arm never reached
`acpi_register_gsi` at all, so the trigger mode CHANGED:

```
acpi-5 (fallback, ISA-edge):    11:         26   IO-APIC  11-edge      virtio0
acpi-6 (the _PRT, LEVEL/LOW):   11:         26   IO-APIC  11-fasteoi   virtio0
```

Linux 6.6's static-entry path is `gsi = source_index` then
`acpi_register_gsi(dev, gsi, ACPI_LEVEL_SENSITIVE, ACPI_ACTIVE_LOW)`
(drivers/acpi/pci_irq.c, acpi_pci_irq_enable's defaults — the fetched v6.6 source);
`fasteoi` IS the level entry. Same 26 interrupts, same GSI 11, driver bound
(`virtio0 1af4:0003`, virtio_console), hvc0 up, init completes.

## G5 — the bus-range fix

BEFORE: `[   64.719950] acpi PNP0A03:00: [Firmware Bug]: no secondary bus range in _CRS`
(acpi-5's serial, line 173 — the log's one Firmware-Bug-class line). AFTER: **GONE**
(`rg "Firmware Bug"`: old 1, new 0), and the kernel now prints the parsed window:

```
[   64.766765] pci_bus 0000:00: root bus resource [bus 00-ff]
```

## G2 — delivery is still real (three independent proofs)

| proof | evidence |
| --- | --- |
| virtio-console through the IO-APIC | `11: 26 IO-APIC 11-fasteoi virtio0`; hvc0 console enabled; 2460 hvc0 bytes (acpi-5: 2595 — the delta is the REMOVED warn lines' ~90 bytes plus inter-run kernel-timestamp drift in the hvc0 dmesg echo; the OFF path is pinned byte-identical by transcript-diff + the determinism gate instead) |
| the `--acpi` reboot arm | `BOOT1: marker written to /dev/sda sector 8, rebooting via the PS/2 controller` -> reboot -> `BOOT2: marker read back from /dev/sda sector 8: DISK PERSIST OK` (results/acpi-6-reboot-on-serial.txt:335,668) — the IDE's write path and the reboot path, both under `--acpi` with the new DSDT |
| the IDE's measured behaviour | UNCHANGED where it can be observed: `/proc/bus/pci/devices` still shows the IDE (00:07.1) on irq 9 (the v86 IDE model's own line byte; pin 0 — it never consults the `_PRT`, and `ide.c` raises on the legacy lines 14/15, never through a PIRQ byte). No gate moved. |

## What the guest now programs

Nothing new on the PIIX: the static entry gives the guest no `_SRS` method and no link
object, so bytes 0x60-0x63 keep their firmware values for the whole boot (the
deliberate 0x60-0x62 = 0 state is preserved BY CONSTRUCTION — the property the link
route would have put at the allocator's mercy). The one new guest-side act is the
KERNEL programming the IOAPIC's pin-11 redirection entry as LEVEL/active-low through
`acpi_register_gsi` — an entry it previously left to the ISA-fallback edge path. The
level entry cannot storm: the virtio line lowers when the driver reads the ISR
register (`virtio.c` isr_status_read -> `pci_lower_irq`), measured working above.

## Divergences, incidents, honest notes

1. **My own address-encoding bug, caught by the suite the same hour:** the first
   `_PRT` byte array wrote the address dword little-endian WRONG (FF 0C 00 00 =
   0x00000CFF) — `verify()` refused the builder's own blob at its `prt_addr` check.
   Fixed to FF FF 0C 00 = 0x000CFFFF; the pin and probe 13 pinned the corrected
   fields. The able-to-fail apparatus was run POSITIVELY first (it refused the real
   bug), which is the property the lane wants from it.

## Reproduction

```sh
make && make selftest && make nasm && make transcript-diff
sh tests/gate-reboot-1.sh; sh tests/gate-m2-5-determinism.sh all
# G1/G2/G5 (the virt-image boot, ON arm):
./build/v86c --bzimage tests/images/vmlinuz-i386 3000000000 --cmdline "console=hvc0 console=ttyS0" \
  --initrd tests/images/guest-initramfs-virtio.img --bios build/minibios-80m.bin --memory 80 \
  --virtio-console /tmp/vio-on.txt --virtio-console-in results/m2-4-virtio-in.txt \
  --serial /tmp/von.txt --acpi
# G2 (the --acpi reboot arm):
./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0 lapic" \
  --hda tests/images/disk.img --initrd tests/images/guest-initramfs-reboot.img \
  --memory 80 --bios build/minibios-80m.bin --acpi --serial /tmp/reboot-on.txt
```
