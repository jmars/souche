/*
 * acpi_tables.h -- the FIRMWARE TABLES: RSDP + RSDT/XSDT + FADT + FACS + DSDT + MADT,
 * built as one contiguous blob and published into guest RAM under `--acpi`.
 *
 * Upstream has NO builder to port (acpi.js is the PIIX4 PM device only, and there is no
 * mps.js in the tree -- MEASURED in unit APIC-1, results/apic-1.md section 6), so this
 * file is written from the ACPI 6.x specification, like tests/minibios.asm was written
 * from the BIOS/E820 conventions: it is the firmware half of the machine, not a device.
 * ACPICA walks RSDP -> RSDT/XSDT -> the tables; for the kernel to take the IO-APIC path
 * it needs the FADT (usable ACPI at all), the DSDT the FADT points at, and the MADT
 * (the LAPIC, the IOAPIC, and the ISA overrides).  What is DELIBERATELY LEFT OUT, and
 * what each omission costs:
 *   - the SMI command port (SMI_CMD=0: "already in ACPI mode" -- no SMI transition
 *     exists to enter one), PM1b, PM2_CNT and GPE1 (the device implements no such
 *     registers), sleep states (the FADT's S-state fields stay 0: S0 only), and the
 *     reset register (no ACPI reset -- the machine's reboot is the 0xCF9 port).  What
 *     the FADT DOES name is every fixed-hardware block the PIIX4 device really
 *     implements (PM1a_EVT 0xB000, PM1a_CNT 0xB004, PM_TMR 0xB008, GPE0 0xAFE0),
 *     and it declares the machine NON-reduced: hardware-reduced is a PLATFORM
 *     declaration a modern guest acts on (it bypasses the legacy PIC and wires zero
 *     IOAPIC redirection entries -- every ISA device edge is then lost), and this
 *     machine has the full PC-AT layer (results/acpi-4.md, the F1 root cause).
 *   - an HPET, an MCFG, an SRAT, a second LAPIC, an NMI entry: there is no such device
 *     in this machine (the RTC lives on the 8259 path, PCI has no ECAM, the machine has
 *     one CPU), and a table describing absent hardware is a lie ACPICA would act on.
 *   - the MP-table route entirely: Linux prefers ACPI where both exist (arch/x86's
 *     acpi_boot_table_init runs before smp_scan_config and disables the MPS scan), so
 *     publishing both would be two answers to one question; ACPI's route is also the
 *     one this machine's APIC state can describe without a second table format.
 *
 * WHERE THE BLOB LIVES, and why that address: 0xE0000-0xEFFFF is free in this machine
 * (the firmware fills 0xF0000-0xFFFFF, the option ROM sits at 0xC0000, the boot params
 * at 0x80000, the command line at 0x8E000 -- src/main.c's boot constants) AND it is in
 * NEITHER of minibios's E820 usable entries: 0xA0000-0xEFFFF is an e820 GAP, which the
 * kernel treats as reserved, so the guest cannot allocate over the tables.  It is also
 * the FIRST window ACPICA's RSDP scan walks (0xE0000-0xFFFFF, 16 bytes at a time), so
 * the RSDP sits at the window's first 16-byte step and the scan finds it at once.
 *
 * DETERMINISM: build() is a pure function of the constants below and of the two MMIO
 * addresses cpu_state.h already names -- no host clock, no allocation, no ordering
 * dependency -- so the tables are boot state exactly like the embedded firmware, and
 * publish() is one memcpy.  The default-off path never calls it (main.c's boot mode),
 * so a boot without --acpi is byte-identical to the pre-unit tree.
 */

#ifndef V86C_HW_ACPI_TABLES_H
#define V86C_HW_ACPI_TABLES_H

#include <stdbool.h>
#include <stdint.h>

/* the free window's base -- see the header comment */
#define ACPI_TABLES_ADDRESS 0xE0000u

/* every structure's offset within the blob, and the blob's length.  Each structure
 * starts on a 16-byte step (the RSDP's scan stride; the tables only need 4/8, but one
 * stride for everything is checkable in one line), and the structures are disjoint. */
#define ACPI_OFF_RSDP 0x000u /* 36 bytes: both checksums, RSDT + XSDT pointers */
#define ACPI_OFF_RSDT 0x040u /* 44 bytes: the FADT + MADT, 32-bit entries */
#define ACPI_OFF_XSDT 0x080u /* 52 bytes: the same two, 64-bit entries */
#define ACPI_OFF_FADT 0x0C0u /* 268 bytes: revision 5, NON-reduced (see above) */
#define ACPI_OFF_FACS 0x1D0u /* 64 bytes: no checksum (the spec gives it none) */
#define ACPI_OFF_DSDT 0x210u /* 146 bytes: the PCI host bridge + _PRT -- see the builder */
#define ACPI_OFF_MADT 0x2C0u /* 76 bytes: LAPIC + IOAPIC + the IRQ0->GSI2 override.
                              * Moved from 0x280 when the DSDT grew past it (the _PRT
                              * and the _CRS bus window, ACPI-6): each structure keeps
                              * its 16-byte stride, and every consumer reads the
                              * placement THROUGH these macros or the RSDT/XSDT
                              * pointers, so the move is invisible outside. */
#define ACPI_TABLES_LENGTH 0x30Cu

/* Fill `out` (ACPI_TABLES_LENGTH bytes) with the whole set.  Idempotent; does not read
 * or write machine state. */
void acpi_tables_build(uint8_t out[ACPI_TABLES_LENGTH]);

/* acpi_tables_build + one memcpy at ACPI_TABLES_ADDRESS.  The caller (main.c's boot
 * path, under --acpi) guarantees mem8 is at least ACPI_TABLES_ADDRESS +
 * ACPI_TABLES_LENGTH bytes.  Called again on a guest reboot's re-entry, next to the
 * firmware/option-ROM copies that re-entry re-runs. */
void acpi_tables_publish(uint8_t *mem8);

/* The selftest's oracle over ANY blob (the builder's own, or a published one, or a
 * deliberately corrupted copy): signatures, lengths, every checksum, the pointers'
 * targets, and the MADT's fields against the MMIO addresses and GSI numbering the
 * port's APIC/IOAPIC actually use.  The PLACEMENT properties (16-byte stride, the free
 * window, the scan finding the RSDP) live in guest RAM, so they are the selftest's
 * checks over published memory, not this function's. */
bool acpi_tables_verify(const uint8_t *blob);

#endif /* V86C_HW_ACPI_TABLES_H */
