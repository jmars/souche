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

	/* ---------------- DSDT: an empty namespace ----------------
	 * header only, zero AML body: the parser ends the stream at once, and the
	 * namespace stays empty.  Nothing in this machine needs an AML object for boot
	 * (the MADT is a structured table, not AML), and an object we cannot back with
	 * device behaviour (_PRT, _Sx, _Pxx) would be the lie the header comment rules
	 * out.  Revision 2 = AML 2.0. */
	uint8_t *dsdt = out + ACPI_OFF_DSDT;
	put_sdt_header(dsdt, "DSDT", 36, 2);
	put_checksum(dsdt, 36);

	/* ---------------- FADT, revision 5, HARDWARE-REDUCED ----------------
	 * Fixed-hardware fields are zero except where a comment names a value: the
	 * reduced-hardware flag is what makes ACPICA skip the SCI/ACPI-mode machinery
	 * there is no device behind, and the two pointer pairs name the FACS and DSDT
	 * above (both widths -- a rev >= 3 FADT's 32-bit facs/dsdt fields are legacy
	 * copies, and ACPICA reads the X_ pair). */
	uint8_t *fadt = out + ACPI_OFF_FADT;
	put_sdt_header(fadt, "FACP", 268, 5);
	wr32le(fadt + 36, facs_addr);  /* FIRMWARE_CTRL */
	wr32le(fadt + 40, dsdt_addr);  /* DSDT */
	fadt[45] = 0;                  /* preferred PM profile: unspecified */
	wr16le(fadt + 46, 9);          /* SCI_INT: the PIIX4 device's line, though the
	                                * reduced-hardware mode never raises it */
	wr32le(fadt + 48, 0);          /* SMI_CMD = 0: no SMI, no ACPI-mode entry */
	fadt[96] = 0xFF;               /* p_lvl2_lat: 0xFFFF = not supported */
	fadt[97] = 0xFF;
	fadt[98] = 0xFF;               /* p_lvl3_lat */
	fadt[99] = 0xFF;
	wr16le(fadt + 109, 0x0003);    /* IAPC_BOOT_ARCH: legacy devices (bit 0) and the
	                                * 8042 (bit 1) -- both true of this machine */
	wr32le(fadt + 112, (1u << 0) | /* WBINVD */
	                  (1u << 20)); /* HARDWARE-REDUCED: the load-bearing flag */
	put_gas_none(fadt + 116);      /* reset register: RESET_REG_SUPPORTED clear */
	fadt[131] = 1;                 /* FADT minor version: 5.1 */
	wr64le(fadt + 132, facs_addr); /* X_FIRMWARE_CTRL */
	wr64le(fadt + 140, dsdt_addr); /* X_DSDT */
	/* +148..+267: the X_ GAS copies of the PM blocks and the sleep control/status --
	 * all zeroed above, which under the reduced-hardware flag is "absent" */
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
 * actual configuration pins down (the MMIO addresses, the GSI base, the reduced-
 * hardware shape).  Written over ANY candidate blob so the selftest can corrupt one
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
	if (!sdt_ok(dsdt, "DSDT", 36, 2)) return false;

	/* --- the FADT: the reduced-hardware shape, pointing at the FACS/DSDT --- */
	const uint8_t *fadt = blob + ACPI_OFF_FADT;
	if (!sdt_ok(fadt, "FACP", 268, 5)) return false;
	uint32_t flags = (uint32_t)fadt[112] | (uint32_t)fadt[113] << 8 |
	                 (uint32_t)fadt[114] << 16 | (uint32_t)fadt[115] << 24;
	if ((flags & (1u << 20)) == 0) return false; /* HARDWARE-REDUCED */
	if (fadt[48] != 0 || fadt[49] != 0 || fadt[50] != 0 || fadt[51] != 0)
		return false; /* SMI_CMD = 0 */
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
