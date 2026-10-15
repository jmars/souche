/*
 * acpi_tables.c -- the firmware-table builder.  See acpi_tables.h for what the set is,
 * where it lives, what is deliberately left out, and why it is deterministic.
 *
 * THE SHAPES AND OFFSETS BELOW ARE THE ACPI SPECIFICATION'S OWN (ACPI 6.x, the tables
 * chapter): the generic 36-byte SDT header, the RSDP's two checksum halves, the FADT's
 * field offsets (which is why the file is full of absolute spec offsets rather than a
 * struct -- a struct would let the compiler pad, and the checksum makes padding a lie).
 * Field values are chosen for THIS machine and named at each site.
 */

#include "acpi_tables.h"

#include <string.h>

#include "../cpu/cpu_state.h" /* APIC_MEM_ADDRESS / IOAPIC_MEM_ADDRESS -- the MADT's
                               * addresses are these, not free constants */

/* the OEM strings: six and eight bytes exactly, space-padded */
#define ACPI_OEM_ID "V86CVM"
#define ACPI_OEM_TABLE_ID "V86CFW  "

/* ------------------------------------------------------------------ *
 * little-endian stores (kernel.c's wr16/wr32 shape, suffixed so the two files'
 * file-local names cannot meet even in one diagnostic)
 * ------------------------------------------------------------------ */
static void wr16le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void wr32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void wr64le(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* the whole range sums to zero (mod 256) once the checksum byte is in place */
static uint8_t sum8(const uint8_t *p, uint32_t len)
{
	uint8_t sum = 0;
	for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
	return sum;
}

static void put_checksum(uint8_t *p, uint32_t len)
{
	p[9] = (uint8_t)(0u - sum8(p, len));
}

/* the generic SDT header every table but the RSDP/FACS starts with */
static void put_sdt_header(uint8_t *p, const char *signature, uint32_t length, uint8_t revision)
{
	memcpy(p, signature, 4);
	wr32le(p + 4, length);
	p[8] = revision;
	memcpy(p + 10, ACPI_OEM_ID, 6);
	memcpy(p + 16, ACPI_OEM_TABLE_ID, 8);
	wr32le(p + 24, 1); /* oem revision */
	memcpy(p + 28, "V86C", 4); /* creator id */
	wr32le(p + 32, 1); /* creator revision */
}

/* a GAS naming nothing: space 0 (system memory), width 0, address 0 -- ACPICA reads a
 * zero-length GAS as "this register does not exist", which is the truth here */
static void put_gas_none(uint8_t *p)
{
	memset(p, 0, 12);
}

void acpi_tables_build(uint8_t out[ACPI_TABLES_LENGTH])
{
	memset(out, 0, ACPI_TABLES_LENGTH);

	const uint32_t rsdt_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_RSDT;
	const uint32_t xsdt_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_XSDT;
	const uint32_t fadt_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_FADT;
	const uint32_t facs_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_FACS;
	const uint32_t dsdt_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_DSDT;
	const uint32_t madt_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_MADT;

	/* ---------------- FACS (no checksum; the spec gives it none) -------------- */
	uint8_t *facs = out + ACPI_OFF_FACS;
	memcpy(facs, "FACS", 4);
	wr32le(facs + 4, 64);
	facs[32] = 1; /* version */

	/* ---------------- DSDT: the PCI host bridge ----------------
	 * ACPI-2 shipped this table header-only ("an empty namespace"), which an ACPI-aware
	 * guest reads as: this platform has no PCI -- the kernel printed "Using host bridge
	 * windows from ACPI" and enumerated ZERO devices, so the IDE and every other PCI
	 * function vanished under --acpi (results/acpi-4.md F2).  The minimum that roots
	 * the bus is one PNP0A03 device under \_SB with _BBN=0 and a _CRS, which is
	 * exactly what this AML is:
	 *
	 *     DefinitionBlock ("", "DSDT", 2, "V86CVM", "V86CFW  ", 2)
	 *     {
	 *         Scope (\_SB) { Device (PCI0)
	 *         {
	 *             Name (_HID, EISAID ("PNP0A03"))
	 *             Name (_BBN, Zero)
	 *             Name (_CRS, ResourceTemplate ()
	 *             {
	 *                 WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, 16,
	 *                          0x0000, 0x0000, 0x0CF7, 0x0000, 0x0CF8)
	 *                 WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, 16,
	 *                          0x0000, 0x0D00, 0xFFFF, 0x0000, 0xF300)
	 *             })
	 *         } }
	 *     }
	 *
	 * What is DELIBERATELY LEFT OUT, and what each omission costs:
	 *   - a memory window: no device on this machine has an MMIO BAR (the host bridge
	 *     and the PIIX pair are BAR-less; the IDE's bus-master window is IO), and a
	 *     window whose base must sit above RAM would depend on --memory, which the
	 *     tables' determinism rule forbids (build() takes no machine state).  A future
	 *     MMIO device would print "no compatible bridge window" and need this added.
	 *   - _PRT: no device here uses a PCI INTx (the IDE's interrupts are the ISA 14/15,
	 *     routed by the MADT's ISA default), so the routing table would be unbacked
	 *     content; a future INTx device would get "can't find routing information".
	 * The encoding is the specification's own (ACPI 6.x: ScopeOp 10, DeviceOp 5B 82,
	 * NameOp 08, the EISAID pack, BufferOp 11, the Word Address Space descriptor 88
	 * with its flag bytes, EndTag 79) -- no compiler on the host, so every byte below
	 * is written against the spec tables and checked by verify() + the guest's ACPICA.
	 * The EISAID trap, for the next hand-encoder: the packed PNP id's LETTER bits sit
	 * in the LOW half of the dword (ACPICA renders a numeric _HID by byte-swapping
	 * it first -- exutils.c acpi_ex_eisa_id_to_string), so PNP0A03 is 0x030AD041, and
	 * the mirror-image 0x41D00A03 boots looking like "@XJD041" (MEASURED, §3).
	 * Revision 2 = AML 2.0. */
	static const uint8_t pci0_aml[] = {
	    0x10, 0x46, 0x04,          /* ScopeOp, PkgLength(70): the 2-byte form, low
	                           * nibble in 0x46 bits[3:0], high bits in the follow byte */
	    0x5F, 0x53, 0x42, 0x5F,    /* "_SB_" */
	    0x5B, 0x82, 0x3F,          /* DeviceOp, PkgLength(63), "PCI0": 1-byte form, the value itself */
	    0x50, 0x43, 0x49, 0x30,    /* "PCI0" */
	    0x08, 0x5F, 0x48, 0x49, 0x44, /* NameOp "_HID" */
	    0x0C, 0x41, 0xD0, 0x0A, 0x03, /* DWordPrefix, 0x030AD041 = EISAID PNP0A03 */
	    0x08, 0x5F, 0x42, 0x42, 0x4E, /* NameOp "_BBN" */
	    0x00,                      /* ZeroOp */
	    0x08, 0x5F, 0x43, 0x52, 0x53, /* NameOp "_CRS" */
	    0x11, 0x24,                /* BufferOp, PkgLength(36 = the size operand + 34 data) */
	    0x0A, 0x22,                /* the buffer's size operand is a PREFIXED AML INTEGER
	                           * (psargs.c reads an operand: BytePrefix, then 34) -- a
	                           * raw word here reads as length 0 and the kernel rejects
	                           * _CRS with -EIO (MEASURED, §3) */
	    /* WordIO: 0x0000-0x0CF7 -- the legacy ISA ports below the config mechanism */
	    0x88, 0x0D, 0x00,          /* Word Address Space descriptor, length 13 */
	    0x01, 0x0C, 0x03,          /* type IO; Min+Max fixed, positive decode; full range */
	    0x00, 0x00,                /* granularity */
	    0x00, 0x00,                /* minimum 0x0000 */
	    0xF7, 0x0C,                /* maximum 0x0CF7 */
	    0x00, 0x00,                /* translation offset */
	    0xF8, 0x0C,                /* length 0x0CF8 */
	    /* WordIO: 0x0D00-0xFFFF -- the window the kernel assigns PCI IO BARs from */
	    0x88, 0x0D, 0x00, 0x01, 0x0C, 0x03,
	    0x00, 0x00,                /* granularity */
	    0x00, 0x0D,                /* minimum 0x0D00 */
	    0xFF, 0xFF,                /* maximum 0xFFFF */
	    0x00, 0x00,                /* translation offset */
	    0x00, 0xF3,                /* length 0xF300 */
	    0x79, 0x00,                /* EndTag (the checksum byte is ignored for _CRS) */
	};
	uint8_t *dsdt = out + ACPI_OFF_DSDT;
	put_sdt_header(dsdt, "DSDT", 36 + (uint32_t)sizeof(pci0_aml), 2);
	memcpy(dsdt + 36, pci0_aml, sizeof(pci0_aml));
	put_checksum(dsdt, 36 + (uint32_t)sizeof(pci0_aml));

	/* ---------------- FADT, revision 5, NON-reduced ----------------
	 * ACPI-2 shipped this table hardware-reduced (FADT flags bit 20).  ACPI-4 measured
	 * what that declaration does to a modern guest: it takes the platform at its word
	 * that there is no PC-AT legacy layer -- nr_legacy_irqs() becomes 0 (the boot's own
	 * "preallocated irqs: 0"), the kernel wires ZERO IOAPIC redirection entries, and
	 * every device edge is lost (results/acpi-4.md, the F1 root cause; the old arm's
	 * "preallocated irqs: 0" is in results/acpi-2-f1-console-freeze-serial.txt:76).
	 * The fix is the honest declaration: this machine HAS the 8259 pair, the ISA lines
	 * and the PIIX4 PM registers, so the FADT now says so -- WBINVD only, plus the real
	 * PM blocks below (the fixed events ACPICA initializes once hw-reduced is clear). */
	uint8_t *fadt = out + ACPI_OFF_FADT;
	put_sdt_header(fadt, "FACP", 268, 5);
	wr32le(fadt + 36, facs_addr);  /* FIRMWARE_CTRL */
	wr32le(fadt + 40, dsdt_addr);  /* DSDT */
	fadt[45] = 0;                  /* preferred PM profile: unspecified */
	wr16le(fadt + 46, 9);          /* SCI_INT: the PIIX4 device's line */
	wr32le(fadt + 48, 0);          /* SMI_CMD = 0: no SMI transition exists -- ACPICA
	                                * reads "already in ACPI mode" */
	fadt[96] = 0xFF;               /* p_lvl2_lat: 0xFFFF = not supported */
	fadt[97] = 0xFF;
	fadt[98] = 0xFF;               /* p_lvl3_lat */
	fadt[99] = 0xFF;
	wr16le(fadt + 109, 0x0003);    /* IAPC_BOOT_ARCH: legacy devices (bit 0) and the
	                                * 8042 (bit 1) -- both true of this machine */
	wr32le(fadt + 112, (1u << 0)); /* WBINVD -- and NOT hardware-reduced (see above) */
	put_gas_none(fadt + 116);      /* reset register: RESET_REG_SUPPORTED clear */
	fadt[131] = 1;                 /* FADT minor version: 5.1 */
	wr64le(fadt + 132, facs_addr); /* X_FIRMWARE_CTRL */
	wr64le(fadt + 140, dsdt_addr); /* X_DSDT */

	/* the PM blocks -- the PIIX4 device's own registers (src/hw/acpi.c acpi_init):
	 * PM1a_EVT at 0xB000 (status 16-bit + enable 16-bit at +2), PM1a_CNT at 0xB004
	 * (16-bit), PM_TMR at 0xB008 (32-bit, 24-bit counter, 3.579545 MHz), GPE0 at
	 * 0xAFE0 (four 8-bit registers).  These are the addresses the device already
	 * answers at; the FADT names them and ACPICA initializes its fixed events on
	 * them instead of failing with AE_BAD_ADDRESS (the 36-byte header-only FADT
	 * of ACPI-2 named nothing).  The X_ GAS copies (+148..+267) stay zero: a zero
	 * extended field defers to these V1 fields, which is the pair ACPICA reads. */
	wr32le(fadt + 56, 0xB000);     /* PM1a_EVT_BLK */
	wr32le(fadt + 60, 0);          /* PM1b_EVT_BLK: absent */
	wr32le(fadt + 64, 0xB004);     /* PM1a_CNT_BLK */
	wr32le(fadt + 68, 0);          /* PM1b_CNT_BLK: absent */
	wr32le(fadt + 72, 0);          /* PM2_CNT_BLK: absent */
	wr32le(fadt + 76, 0xB008);     /* PM_TMR_BLK */
	wr32le(fadt + 80, 0xAFE0);     /* GPE0_BLK */
	wr32le(fadt + 84, 0);          /* GPE1_BLK: absent */
	fadt[88] = 4;                  /* PM1_EVT_LEN: 2-byte status + 2-byte enable */
	fadt[89] = 2;                  /* PM1_CNT_LEN: the 16-bit control register */
	fadt[90] = 0;                  /* PM2_CNT_LEN: absent */
	fadt[91] = 4;                  /* PM_TMR_LEN: 32-bit port, 24-bit counter */
	fadt[92] = 4;                  /* GPE0_BLK_LEN: the four 8-bit registers */
	fadt[93] = 0;                  /* GPE1_BLK_LEN: absent */
	fadt[94] = 0;                  /* GPE1_BASE: absent */
	fadt[95] = 0;                  /* CST_CNT: no C-states */
	put_checksum(fadt, 268);

	/* ---------------- MADT ("APIC") ----------------
	 * One LAPIC, one IOAPIC, and the classic ISA override: the PIT's output line is
	 * the IOAPIC's input pin 2 (IRQ0 -> GSI 2), which is also how machine.c's raise
	 * path feeds the device -- the table describes the wiring that exists. */
	uint8_t *madt = out + ACPI_OFF_MADT;
	put_sdt_header(madt, "APIC", 76, 1);
	wr32le(madt + 36, APIC_MEM_ADDRESS); /* the LAPIC register block */
	wr32le(madt + 40, 1);                /* flags: PCAT_COMPAT (the 8259 pair exists) */

	madt[44] = 0; /* type 0: processor local APIC */
	madt[45] = 8; /* length */
	madt[46] = 0; /* processor UID */
	madt[47] = 0; /* APIC ID -- apic.c's reset value, and what its ID register reads */
	wr32le(madt + 48, 1); /* flags: enabled */

	madt[52] = 1; /* type 1: I/O APIC */
	madt[53] = 12; /* length */
	madt[54] = 0; /* IOAPIC ID -- ioapic.c's IOAPIC_ID, its ID register's reset value */
	madt[55] = 0; /* reserved */
	wr32le(madt + 56, IOAPIC_MEM_ADDRESS);
	wr32le(madt + 60, 0); /* GSI base: the only IOAPIC, so GSI == its own pin number */

	madt[64] = 2; /* type 2: interrupt source override */
	madt[65] = 10; /* length */
	madt[66] = 0; /* bus: ISA */
	madt[67] = 0; /* source: ISA IRQ 0, the PIT */
	wr32le(madt + 68, 2); /* -> GSI 2 (IOAPIC pin 2) */
	wr16le(madt + 72, 0); /* flags: bus default = edge, active-high (the PIT is edge) */
	/* +74..+75: zero pad, taking the table to a 4-byte multiple */
	put_checksum(madt, 76);

	/* ---------------- RSDT / XSDT: the FADT and the MADT ----------------
	 * Both exist because the RSDP is revision 2 and ACPICA prefers the XSDT when it
	 * names one; keeping both pointers alive makes the set readable by either
	 * preference (and by `acpi=noxsdt`-class overrides). */
	uint8_t *rsdt = out + ACPI_OFF_RSDT;
	put_sdt_header(rsdt, "RSDT", 44, 1);
	wr32le(rsdt + 36, fadt_addr);
	wr32le(rsdt + 40, madt_addr);
	put_checksum(rsdt, 44);

	uint8_t *xsdt = out + ACPI_OFF_XSDT;
	put_sdt_header(xsdt, "XSDT", 52, 1);
	wr64le(xsdt + 36, fadt_addr);
	wr64le(xsdt + 44, madt_addr);
	put_checksum(xsdt, 52);

	/* ---------------- RSDP: the entry the scan finds ----------------
	 * Revision 2: both checksums are checked (the byte sum over the first 20, then
	 * the extended one over all 36), and both root-table pointers are live. */
	uint8_t *rsdp = out + ACPI_OFF_RSDP;
	memcpy(rsdp, "RSD PTR ", 8);
	memcpy(rsdp + 9, ACPI_OEM_ID, 6); /* the RSDP's oem id sits at +9: its own checksum
	                                   * byte is at +8, unlike the SDT headers' */
	rsdp[15] = 2; /* revision -- every field BEFORE the checksums, which cover them */
	wr32le(rsdp + 16, rsdt_addr);
	wr32le(rsdp + 20, 36); /* length */
	wr64le(rsdp + 24, xsdt_addr);
	rsdp[8] = (uint8_t)(0u - sum8(rsdp, 20));
	rsdp[32] = (uint8_t)(0u - sum8(rsdp, 36));
	/* +33..+35: reserved, zeroed above */
}

void acpi_tables_publish(uint8_t *mem8)
{
	uint8_t blob[ACPI_TABLES_LENGTH];
	acpi_tables_build(blob);
	memcpy(mem8 + ACPI_TABLES_ADDRESS, blob, ACPI_TABLES_LENGTH);
}

/* ------------------------------------------------------------------ *
 * the verifier: every claim the builder makes, checked the way ACPICA's early walk
 * would check it (signature, length, checksum, pointer) plus the fields this machine's
 * actual configuration pins down (the MMIO addresses, the GSI base, the honest
 * non-reduced FADT shape and the PM blocks the PIIX4 device really answers at).
 * Written over ANY candidate blob so the selftest can corrupt one
 * and watch the check that must catch it fail.
 * ------------------------------------------------------------------ */
static bool sdt_ok(const uint8_t *p, const char *signature, uint32_t length, uint8_t revision)
{
	if (memcmp(p, signature, 4) != 0) return false;
	if (p[4] != (uint8_t)length || p[5] != (uint8_t)(length >> 8) ||
	    p[6] != (uint8_t)(length >> 16) || p[7] != (uint8_t)(length >> 24))
		return false;
	if (p[8] != revision) return false;
	return sum8(p, length) == 0;
}

bool acpi_tables_verify(const uint8_t *blob)
{
	/* --- the RSDP, both checksum halves --- */
	const uint8_t *rsdp = blob + ACPI_OFF_RSDP;
	if (memcmp(rsdp, "RSD PTR ", 8) != 0) return false;
	if (sum8(rsdp, 20) != 0) return false;
	if (rsdp[15] != 2) return false;
	if (rsdp[20] != 36) return false;
	if (sum8(rsdp, 36) != 0) return false;
	const uint32_t rsdt_addr = (uint32_t)rsdp[16] | (uint32_t)rsdp[17] << 8 |
	                           (uint32_t)rsdp[18] << 16 | (uint32_t)rsdp[19] << 24;
	uint64_t xsdt_addr64 = 0;
	for (int i = 0; i < 8; i++) xsdt_addr64 |= (uint64_t)rsdp[24 + i] << (8 * i);
	if (rsdt_addr != ACPI_TABLES_ADDRESS + ACPI_OFF_RSDT) return false;
	if (xsdt_addr64 != (uint64_t)(ACPI_TABLES_ADDRESS + ACPI_OFF_XSDT)) return false;

	/* --- the root tables: both point at exactly the FADT and the MADT --- */
	const uint8_t *rsdt = blob + ACPI_OFF_RSDT;
	const uint8_t *xsdt = blob + ACPI_OFF_XSDT;
	const uint32_t fadt_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_FADT;
	const uint32_t madt_addr = ACPI_TABLES_ADDRESS + ACPI_OFF_MADT;
	if (!sdt_ok(rsdt, "RSDT", 44, 1)) return false;
	if (!sdt_ok(xsdt, "XSDT", 52, 1)) return false;
	if (rsdt[36] != (uint8_t)fadt_addr || rsdt[37] != (uint8_t)(fadt_addr >> 8) ||
	    rsdt[38] != (uint8_t)(fadt_addr >> 16) || rsdt[39] != (uint8_t)(fadt_addr >> 24))
		return false;
	if (rsdt[40] != (uint8_t)madt_addr || rsdt[41] != (uint8_t)(madt_addr >> 8) ||
	    rsdt[42] != (uint8_t)(madt_addr >> 16) || rsdt[43] != (uint8_t)(madt_addr >> 24))
		return false;
	for (int i = 0; i < 8; i++) {
		/* the pointer is 32-bit, the shift must be: (uint32_t >> 32) is undefined,
		 * and i reaches 4 */
		if (xsdt[36 + i] != (uint8_t)((uint64_t)fadt_addr >> (8 * i))) return false;
		if (xsdt[44 + i] != (uint8_t)((uint64_t)madt_addr >> (8 * i))) return false;
	}

	/* --- the FACS and the DSDT the FADT names --- */
	const uint8_t *facs = blob + ACPI_OFF_FACS;
	const uint8_t *dsdt = blob + ACPI_OFF_DSDT;
	if (memcmp(facs, "FACS", 4) != 0 || facs[4] != 64) return false;
	if (!sdt_ok(dsdt, "DSDT", 109, 2)) return false;

	/* --- the DSDT's AML: the PCI host bridge (results/acpi-4.md F2) ---
	 * The byte layout is the builder's pci0_aml[]; the checks walk it structurally --
	 * Scope(\_SB), Device(PCI0), the _HID EISAID, and the two WordIO windows -- so a
	 * corrupted or mis-encoded bridge is refused here, before any guest has to. */
	const uint8_t *aml = dsdt + 36;
	const uint32_t aml_len = 109u - 36u;
	if (aml_len != 73) return false;
	static const uint8_t scope_dev[] = {
	    0x10, 0x46, 0x04,          /* ScopeOp, PkgLength(70) */
	    '_', 'S', 'B', '_',
	    0x5B, 0x82, 0x3F,          /* DeviceOp, PkgLength(63) */
	    'P', 'C', 'I', '0',
	    0x08, '_', 'H', 'I', 'D',  /* NameOp _HID */
	    0x0C, 0x41, 0xD0, 0x0A, 0x03, /* EISAID PNP0A03 = 0x030AD041 */
	};
	if (memcmp(aml, scope_dev, sizeof(scope_dev)) != 0) return false;
	if (aml[24] != 0x08 || memcmp(aml + 25, "_BBN", 4) != 0 || aml[29] != 0x00)
		return false; /* _BBN = Zero: bus 0 */
	if (aml[30] != 0x08 || memcmp(aml + 31, "_CRS", 4) != 0) return false;
	if (aml[35] != 0x11 || aml[36] != 0x24) return false; /* BufferOp, PkgLength(36) */
	if (aml[37] != 0x0A || aml[38] != 34) return false; /* the size operand: 34 */
	/* the two Word Address Space descriptors (tag 0x88, length 13, type IO, Min+Max
	 * fixed positive decode, full range) and the EndTag, at their fixed offsets */
	static const uint8_t wordio_header[] = { 0x88, 0x0D, 0x00, 0x01, 0x0C, 0x03 };
	if (memcmp(aml + 39, wordio_header, 6) != 0) return false;
	uint32_t min1 = (uint32_t)aml[47] | (uint32_t)aml[48] << 8;
	uint32_t max1 = (uint32_t)aml[49] | (uint32_t)aml[50] << 8;
	uint32_t len1 = (uint32_t)aml[53] | (uint32_t)aml[54] << 8;
	if (min1 != 0x0000 || max1 != 0x0CF7 || len1 != 0x0CF8) return false;
	if (memcmp(aml + 55, wordio_header, 6) != 0) return false;
	uint32_t min2 = (uint32_t)aml[63] | (uint32_t)aml[64] << 8;
	uint32_t max2 = (uint32_t)aml[65] | (uint32_t)aml[66] << 8;
	uint32_t len2 = (uint32_t)aml[69] | (uint32_t)aml[70] << 8;
	if (min2 != 0x0D00 || max2 != 0xFFFF || len2 != 0xF300) return false;
	if (aml[71] != 0x79) return false; /* EndTag */

	/* --- the FADT: the honest NON-reduced shape, pointing at the FACS/DSDT --- */
	const uint8_t *fadt = blob + ACPI_OFF_FADT;
	if (!sdt_ok(fadt, "FACP", 268, 5)) return false;
	uint32_t flags = (uint32_t)fadt[112] | (uint32_t)fadt[113] << 8 |
	                 (uint32_t)fadt[114] << 16 | (uint32_t)fadt[115] << 24;
	/* NOT hardware-reduced: the machine HAS the full PC-AT layer (PIC, PIT, ISA UART),
	 * and a modern guest takes the reduced bit as a platform declaration -- it bypasses
	 * the legacy PIC, wires zero IOAPIC redirection entries, and every device edge is
	 * lost (results/acpi-4.md, the F1 root cause).  WBINVD is the one flag set. */
	if ((flags & (1u << 20)) != 0) return false; /* HARDWARE-REDUCED must be CLEAR */
	if ((flags & (1u << 0)) == 0) return false;  /* WBINVD */
	if (fadt[48] != 0 || fadt[49] != 0 || fadt[50] != 0 || fadt[51] != 0)
		return false; /* SMI_CMD = 0 */
	/* the PM blocks: the addresses the PIIX4 device answers at (src/hw/acpi.c's
	 * io_register_* calls) with their real widths -- naming a block the device does
	 * not implement would be the same lie the reduced flag was */
	uint32_t pm1a_evt = (uint32_t)fadt[56] | (uint32_t)fadt[57] << 8 |
	                    (uint32_t)fadt[58] << 16 | (uint32_t)fadt[59] << 24;
	uint32_t pm1a_cnt = (uint32_t)fadt[64] | (uint32_t)fadt[65] << 8 |
	                    (uint32_t)fadt[66] << 16 | (uint32_t)fadt[67] << 24;
	uint32_t pm_tmr = (uint32_t)fadt[76] | (uint32_t)fadt[77] << 8 |
	                  (uint32_t)fadt[78] << 16 | (uint32_t)fadt[79] << 24;
	uint32_t gpe0 = (uint32_t)fadt[80] | (uint32_t)fadt[81] << 8 |
	                (uint32_t)fadt[82] << 16 | (uint32_t)fadt[83] << 24;
	if (pm1a_evt != 0xB000) return false;
	if (pm1a_cnt != 0xB004) return false;
	if (pm_tmr != 0xB008) return false;
	if (gpe0 != 0xAFE0) return false;
	if (fadt[88] != 4) return false; /* PM1_EVT_LEN: 16-bit status + 16-bit enable */
	if (fadt[89] != 2) return false; /* PM1_CNT_LEN: the 16-bit control register */
	if (fadt[91] != 4) return false; /* PM_TMR_LEN: the 32-bit timer port */
	if (fadt[92] != 4) return false; /* GPE0_BLK_LEN: the four 8-bit registers */
	uint64_t x_facs = 0, x_dsdt = 0;
	for (int i = 0; i < 8; i++) {
		x_facs |= (uint64_t)fadt[132 + i] << (8 * i);
		x_dsdt |= (uint64_t)fadt[140 + i] << (8 * i);
	}
	if (x_facs != (uint64_t)(ACPI_TABLES_ADDRESS + ACPI_OFF_FACS)) return false;
	if (x_dsdt != (uint64_t)(ACPI_TABLES_ADDRESS + ACPI_OFF_DSDT)) return false;
	if (fadt[36] != (uint8_t)(ACPI_TABLES_ADDRESS + ACPI_OFF_FACS) ||
	    fadt[37] != (uint8_t)((ACPI_TABLES_ADDRESS + ACPI_OFF_FACS) >> 8))
		return false; /* the 32-bit FIRMWARE_CTRL agrees with X_FIRMWARE_CTRL */
	if (fadt[40] != (uint8_t)(ACPI_TABLES_ADDRESS + ACPI_OFF_DSDT) ||
	    fadt[41] != (uint8_t)((ACPI_TABLES_ADDRESS + ACPI_OFF_DSDT) >> 8))
		return false;

	/* --- the MADT: the addresses the port's devices answer at --- */
	const uint8_t *madt = blob + ACPI_OFF_MADT;
	if (!sdt_ok(madt, "APIC", 76, 1)) return false;
	uint32_t lapic_addr = (uint32_t)madt[36] | (uint32_t)madt[37] << 8 |
	                      (uint32_t)madt[38] << 16 | (uint32_t)madt[39] << 24;
	if (lapic_addr != APIC_MEM_ADDRESS) return false;
	/* entry 0: the LAPIC, enabled, id 0 (apic.c's reset value) */
	if (madt[44] != 0 || madt[45] != 8 || madt[47] != 0) return false;
	if (madt[48] != 1) return false;
	/* entry 1: the IOAPIC at its MMIO address, GSI base 0, id 0 */
	if (madt[52] != 1 || madt[53] != 12 || madt[54] != 0) return false;
	uint32_t ioapic_addr = (uint32_t)madt[56] | (uint32_t)madt[57] << 8 |
	                       (uint32_t)madt[58] << 16 | (uint32_t)madt[59] << 24;
	uint32_t gsi_base = (uint32_t)madt[60] | (uint32_t)madt[61] << 8 |
	                    (uint32_t)madt[62] << 16 | (uint32_t)madt[63] << 24;
	if (ioapic_addr != IOAPIC_MEM_ADDRESS) return false;
	if (gsi_base != 0) return false;
	/* entry 2: the PIT's line is IOAPIC pin 2, edge (flags 0 = bus default) */
	if (madt[64] != 2 || madt[65] != 10 || madt[66] != 0 || madt[67] != 0) return false;
	uint32_t ovr_gsi = (uint32_t)madt[68] | (uint32_t)madt[69] << 8 |
	                   (uint32_t)madt[70] << 16 | (uint32_t)madt[71] << 24;
	if (ovr_gsi != 2) return false;
	if (madt[72] != 0 || madt[73] != 0) return false;

	return true;
}
