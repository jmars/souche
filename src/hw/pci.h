/*
 * pci.h -- the PCI bus: config-space addressing on 0xCF8/0xCFC, the device registry, and
 * the BAR machinery (I/O BARs move port-map entries; memory BARs are read-only).
 *
 * Port of v86/src/pci.js (603 lines).  The BUS itself is what this unit needs (BIOS/OS
 * enumeration of 00:00.0 and 00:01.0, which are the two devices upstream registers in its
 * constructor); `virtio` lands on top of it in M2-4.
 *
 * WHAT A GUEST SEES, and the two named limits:
 *
 *  - config space for the host bridge (00:00.0, the 440FX PMC with seabios' PAM0 register),
 *    the ISA bridge (00:01.0, the PIIX3) and -- when the machine has one -- ACPI
 *    (00:07.0).  Reading an absent device returns -1 in the response register and 0 in the
 *    status register (pci_query's `else` arm, "required by freebsd-9.1" for the
 *    out-of-range case);
 *  - an 8- or 16-BIT WRITE to an absent device's config space STOPS the machine.  Those two
 *    arms dereference the undefined space first (`new Uint8Array(this.device_spaces[bdf]
 *    .buffer)`, pci.js:328, and .Uint16Array, pci.js:354) and throw a TypeError, so there is
 *    no upstream behaviour to reproduce -- the port names the bdf instead of segfaulting.
 *    A 32-BIT WRITE is a DIFFERENT CASE and this header used to say otherwise: pci.js:381-390
 *    reads `var space = this.device_spaces[bdf];` with no dereference and `if(!space) return;`
 *    -- upstream IGNORES it, and so does the port now (CORRECTED IN M2-2: the stop it used to
 *    have ended the first real kernel boot, at instruction 659158271; see
 *    results/m2-2-boot.md);
 *  - `cpu.reboot_internal()` on a write of 0x06 to 0xCF9's high byte ("CPU reboot via PCI",
 *    pci.js:126-131) REBOOTS THE MACHINE (REBOOT-1): machine_reboot() (machine.c) resets
 *    the CPU and the devices, and the run loop re-enters the boot (main.c); memory and
 *    the disk survive, as in the reference;
 *  - MEMORY BARs cannot be moved (upstream logs "Changing memory bar not supported,
 *    ignored" and resets the register to its default, pci.js:459-473) -- kept verbatim,
 *    including the message's behaviour: the register is reset, nothing is logged.
 */

#ifndef V86C_HW_PCI_H
#define V86C_HW_PCI_H

#include <stdint.h>

#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8 /* pci.js:12 */
#define PCI_CONFIG_DATA 0xCFC    /* pci.js:13 */

/* The device index space.  Upstream's tables are initialised for 0..255 (pci.js:37-38) and
 * looked up with a 16-bit value (pci.js:290/329/353/384), so everything above 255 is a
 * MISSING entry there -- i.e. an absent device, not an out-of-range index.  This port keeps
 * the same 256 entries and CHECKS the index (pci.c's pci_bdf_is_absent) because a C array
 * would read 260 KiB past its end instead of returning `undefined` (MEASURED: that is a
 * SIGSEGV on the Linux kernel's bus-255 config scan). */
#define PCI_DEVICE_COUNT 256
#define PCI_SPACE_DWORDS 64 /* 256 bytes, pci.js:63-66 */

/* The per-bar entry pool: `bar.size` port-map entries are copied out of here at
 * registration (pci.js:493-517).  A limit, stated: 4096 entries total, which is more than
 * every device upstream registers together (virtio's largest bar is 0x20). */
#define PCI_BAR_ENTRY_POOL 4096

/* pci.js's device object: pci_id (the bdf's device<<3 | function), the 64-byte config space
 * in lspci's byte notation, the bars (an array that may be empty, in which case
 * pci_bars == NULL and pci_bars_count == 0) and a name for the diagnostics. */
typedef struct pci_bar {
    uint32_t size;          /* a power of two (pci.js:450 asserts it) */
    int32_t original_bar;   /* filled in by register_device */
    io_port_entry_t *entries; /* bar->size entries, from the pool */
} pci_bar_t;

typedef struct pci_device {
    int32_t pci_id;
    const uint8_t *pci_space; /* 64..256 bytes (pci.js:64-66 asserts only ">= 64") */
    int32_t pci_space_length;
    pci_bar_t *pci_bars;
    int32_t pci_bars_count;
    const char *name;

    /* the two fields pci_write32's 0x30 arm uses (pci.js:486-505) */
    uint32_t pci_rom_size;
    int32_t pci_rom_address;
} pci_device_t;

typedef struct pci_state {
    uint8_t pci_addr[4];
    uint8_t pci_value[4];
    uint8_t pci_response[4];
    uint8_t pci_status[4];

    int32_t *device_spaces[PCI_DEVICE_COUNT]; /* 64 dwords each */
    const pci_device_t *devices[PCI_DEVICE_COUNT];

    /* pci.js:108-119: the ISA bridge's config space as bytes -- raise_irq reads the
     * PIRQ->IRQ map out of it (pci.js:581, 596). */
    uint8_t *isa_bridge_space8;
} pci_state_t;

extern pci_state_t *pci;

void pci_init(void);

/* pci.js:487-517 -- returns the device's config space (the caller, ACPI, ignores it). */
int32_t *pci_register_device(pci_device_t *device);

/* pci.js:519-549 -- move an I/O bar's port-map entries from one base to another */
void pci_set_io_bars(pci_bar_t *bar, int32_t from, int32_t to);

/* pci.js:552-604 -- the two IRQ paths (the PIIX3's PIRQ routing table at 0x60) */
void pci_raise_irq(int32_t pci_id);
void pci_lower_irq(int32_t pci_id);

/* pci.js:237-287 -- the config-space query (public for the selftest's read/write cases) */
void pci_query(void);
void pci_write8(int32_t address, int32_t written);
void pci_write16(int32_t address, int32_t written);
void pci_write32(int32_t address, int32_t written);

/* The four little-endian dword views over the pci_addr/pci_response/pci_status byte arrays
 * (pci.js:25-29's Int32Array views). */
int32_t pci_addr32(void);
int32_t pci_response32(void);
void pci_set_response32(int32_t value);
int32_t pci_status32(void);
void pci_set_status32(int32_t value);

#endif /* V86C_HW_PCI_H */
