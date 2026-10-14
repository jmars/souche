/*
 * pci.c -- the PCI bus.  See pci.h for the named limits (the absent-device write stop, the
 * reboot stop, the read-only memory bars).
 *
 * The `pci_addr32`/`pci_response32`/`pci_status32`/`pci_value32` views upstream builds with
 * `new Int32Array(this.pci_addr.buffer)` (pci.js:25-29) are little-endian dword views over
 * the byte arrays; the C port keeps the byte arrays as THE storage and does the dword
 * access with memcpy, so the two never disagree about the bytes.
 */

#include "pci.h"

#include <stdbool.h>

#include <stdio.h>
#include <string.h>

#include "../cpu/cpu.h" /* unimplemented_instruction */
#include "../cpu/cpu_state.h"
#include "machine.h" /* device_raise_irq / device_lower_irq (the two IRQ paths) */

static pci_state_t pci_storage;
pci_state_t *pci = NULL;

static int32_t device_space_storage[PCI_DEVICE_COUNT][PCI_SPACE_DWORDS];

static io_port_entry_t bar_entry_pool[PCI_BAR_ENTRY_POOL];
static int32_t bar_entry_pool_used;

/* ------------------------------------------------------------------ *
 * the dword views
 * ------------------------------------------------------------------ */
static int32_t load32(const uint8_t *bytes)
{
    int32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void store32(uint8_t *bytes, int32_t value) { memcpy(bytes, &value, sizeof(value)); }

int32_t pci_addr32(void) { return load32(pci->pci_addr); }
int32_t pci_response32(void) { return load32(pci->pci_response); }
void pci_set_response32(int32_t value) { store32(pci->pci_response, value); }
int32_t pci_status32(void) { return load32(pci->pci_status); }
void pci_set_status32(int32_t value) { store32(pci->pci_status, value); }

/* ------------------------------------------------------------------ *
 * the config-space ports -- pci.js:44-176
 * ------------------------------------------------------------------ */
static void portCFC_write8(void *device, int32_t port, int32_t value)
{
    (void)device;
    pci_write8(pci_addr32() + (port - PCI_CONFIG_DATA), value);
}

static void portCFC_write16(void *device, int32_t port, int32_t value)
{
    (void)device;
    pci_write16(pci_addr32() + (port - PCI_CONFIG_DATA), value);
}

static void portCFC_write32(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    pci_write32(pci_addr32(), value);
}

static int32_t portCFC_read8(void *device, int32_t port)
{
    (void)device;
    return pci->pci_response[port - PCI_CONFIG_DATA];
}

static int32_t portCF8_read8(void *device, int32_t port)
{
    (void)device;
    return pci->pci_status[port - PCI_CONFIG_ADDRESS];
}

static void portCF8_write8(void *device, int32_t port, int32_t out_byte)
{
    (void)device;
    switch (port - PCI_CONFIG_ADDRESS) {
    case 0:
        /* pci.js:164-176 */
        pci->pci_addr[0] = (uint8_t)(out_byte & 0xFC);
        break;
    case 1:
        if ((pci->pci_addr[1] & 0x06) == 0x02 && (out_byte & 0x06) == 0x06) {
            /* pci.js:126-131: "CPU reboot via PCI" -- cpu.reboot_internal() (the
             * file's line numbers moved with the seabios PAM0 block above it; the
             * header's pointer said 161-165).  REBOOT-1: the reboot path exists --
             * machine_reboot() (machine.c) resets the machine and requests the boot
             * re-entry; the run loop unwinds and main.c re-establishes the boot.
             * The trigger byte itself is NOT stored (upstream returns before
             * `pci_addr[1] = out_byte`). */
            machine_reboot();
            return;
        }
        pci->pci_addr[1] = (uint8_t)out_byte;
        break;
    case 2:
        pci->pci_addr[2] = (uint8_t)out_byte;
        break;
    case 3:
        pci->pci_addr[3] = (uint8_t)out_byte;
        pci_query();
        break;
    default:
        DBG_ASSERT(false);
        break;
    }
}

void pci_init(void)
{
    pci = &pci_storage;
    memset(&pci_storage, 0, sizeof(pci_storage));
    memset(device_space_storage, 0, sizeof(device_space_storage));
    memset(bar_entry_pool, 0, sizeof(bar_entry_pool));
    bar_entry_pool_used = 0;

    /* pci.js:44-176: the config-space port registrations.  The READS use the
     * consecutive-port helper (io.js:171-256), which installs the composed 16- and 32-bit
     * forms; the WRITES are registered explicitly because upstream does NOT compose them
     * there: 0xCFC has a real 32-bit handler and 0xCFE a real 16-bit one (pci.js:53-77),
     * which is the whole point of the config-data window. */
    io_register_read_consecutive(PCI_CONFIG_DATA, pci, portCFC_read8, portCFC_read8,
                                 portCFC_read8, portCFC_read8);
    io_register_write(PCI_CONFIG_DATA, pci, portCFC_write8, portCFC_write16, portCFC_write32);
    io_register_write(PCI_CONFIG_DATA + 1, pci, portCFC_write8, NULL, NULL);
    io_register_write(PCI_CONFIG_DATA + 2, pci, portCFC_write8, portCFC_write16, NULL);
    io_register_write(PCI_CONFIG_DATA + 3, pci, portCFC_write8, NULL, NULL);
    io_register_read_consecutive(PCI_CONFIG_ADDRESS, pci, portCF8_read8, portCF8_read8,
                                 portCF8_read8, portCF8_read8);
    io_register_write_consecutive(PCI_CONFIG_ADDRESS, pci, portCF8_write8, portCF8_write8,
                                  portCF8_write8, portCF8_write8);

    /* pci.js:180-215: the two devices upstream's constructor registers. */
    {
        /* 00:00.0 Host bridge: Intel Corporation 440FX - 82441FX PMC [Natoma] (rev 02).
         * 96 BYTES, not 64 (pci.js:167-182): seabios' PAM0 register is at offset 0x59,
         * OUTSIDE the 64-byte header, and the space being copied into is 256 bytes --
         * upstream's `dbg_assert(device.pci_space.length >= 64)` allows exactly this. */
        static const uint8_t host_bridge_space[96] = {
            0x86, 0x80, 0x37, 0x12, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x06,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        static pci_device_t host_bridge = {
            .pci_id = 0,
            .pci_space = host_bridge_space,
            .pci_space_length = (int32_t)sizeof(host_bridge_space),
            .pci_bars = NULL,
            .pci_bars_count = 0,
            .name = "82441FX PMC",
            .pci_rom_size = 0,
            .pci_rom_address = 0,
        };
        pci_register_device(&host_bridge);
    }

    {
        /* 00:01.0 ISA bridge: Intel Corporation 82371SB PIIX3 ISA [Natoma/Triton II] */
        static const uint8_t isa_bridge_space[64] = {
            0x86, 0x80, 0x00, 0x70, 0x07, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x06, 0x00, 0x00, 0x80,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };
        static pci_device_t isa_bridge = {
            .pci_id = 1 << 3,
            .pci_space = isa_bridge_space,
            .pci_space_length = (int32_t)sizeof(isa_bridge_space),
            .pci_bars = NULL,
            .pci_bars_count = 0,
            .name = "82371SB PIIX3 ISA",
            .pci_rom_size = 0,
            .pci_rom_address = 0,
        };
        int32_t *space = pci_register_device(&isa_bridge);
        pci->isa_bridge_space8 = (uint8_t *)space;
    }
}

/* ------------------------------------------------------------------ *
 * THE DEVICE INDEX NEEDS A BOUND, AND UPSTREAM'S DOES NOT
 *
 * Upstream indexes its tables with a 16-bit "bdf": `pci_addr[2] << 8 | pci_addr[1]` on the
 * query path (pci.js:290) and `address >> 8 & 0xFFFF` on the write paths (pci.js:329, 353,
 * 384) -- values up to 0xFFFF -- while the tables it indexes are INITIALISED ONLY FOR 0..255
 * (pci.js:37-38: `for(i = 0; i < 256; i++) this.device_spaces[i] = undefined`).  In JS any
 * index above 255 is simply `undefined`, i.e. AN ABSENT DEVICE: the query answers "-1 in the
 * response register, 0 in the status register" and a write throws (the `new
 * Uint8Array(undefined.buffer)` upstream never guards against).  The C port's tables are
 * the same 256 entries, so the index has to be CHECKED rather than used -- a 16-bit value
 * into a 256-entry array is a read of up to 260 KiB past its end.
 *
 * MEASURED (results/m2-2-boot.md): the Linux kernel's PCI scan writes config space on
 * bus 255 (address 0x80ff0000 -> bdf 0xff00, at instruction 550020331 of the boot), and the
 * unbounded index dereferenced a garbage pointer and SIGSEGVed the port.  Every access now
 * goes through this predicate, whose FALSE arm is upstream's `undefined` -- absent, not
 * absent-with-a-different-meaning.
 * ------------------------------------------------------------------ */
static bool pci_bdf_is_absent(int32_t bdf)
{
    return bdf < 0 || bdf >= PCI_DEVICE_COUNT;
}

/* ------------------------------------------------------------------ *
 * pci.js:237-287 -- the query
 * ------------------------------------------------------------------ */
void pci_query(void)
{
    int32_t bdf = pci->pci_addr[2] << 8 | pci->pci_addr[1];
    int32_t addr = pci->pci_addr[0] & 0xFC;
    /* dev = bdf >> 3 & 0x1F; fn = bdf & 7 */
    int32_t enabled = pci->pci_addr[3] >> 7;
    (void)enabled; /* upstream only logs it */

    int32_t *space = pci_bdf_is_absent(bdf) ? NULL : pci->device_spaces[bdf];

    if (space != NULL) {
        pci_set_status32((int32_t)0x80000000u);

        if (addr < PCI_SPACE_DWORDS * 4) {
            pci_set_response32(space[addr >> 2]);
        }
        else {
            /* required by freebsd-9.1 */
            pci_set_response32(0);
        }
    }
    else {
        pci_set_response32(-1);
        pci_set_status32(0);
    }
}

/* ------------------------------------------------------------------ *
 * pci.js:289-380 -- the writes
 * ------------------------------------------------------------------ */
/* The stand-in for upstream's TypeError, and ONLY for the two widths whose absent-device
 * path really does dereference the undefined space:
 *
 *     pci.js:324-329 (write8)   `var space = new Uint8Array(this.device_spaces[bdf].buffer);`
 *     pci.js:350-355 (write16)  `var space = new Uint16Array(this.device_spaces[bdf].buffer);`
 *
 * `this.device_spaces[bdf]` is `undefined` for an absent device (pci.js:37-38 initialises
 * indices 0..255 only), so upstream THROWS -- the reference emulator dies -- and there is no
 * behaviour to reproduce.  The port names the access instead.
 *
 * write32 IS NOT THE SAME CASE, and M2-1's report said it was: pci.js:381-390 reads
 * `var space = this.device_spaces[bdf];` with NO `.buffer` dereference and `if(!space)
 * return;` -- a write32 to an absent device is a SILENT NO-OP upstream.  MEASURED: the
 * Linux kernel's PCI scan writes config space to absent devices (bdf 0xf9, dev 31 fn 1, at
 * instruction 659158271 of the boot) and the port used to stop there; see results/m2-2-boot.md. */
static void pci_stop_absent(const char *what, int32_t address, int32_t bdf)
{
    char name[160];
    snprintf(name, sizeof(name),
             "pci_%s(address=0x%x, bdf=0x%x) to a device that does not exist (upstream "
             "dereferences the undefined config space and throws)",
             what, (unsigned)address, (unsigned)bdf);
    unimplemented_instruction(name);
}

void pci_write8(int32_t address, int32_t written)
{
    int32_t bdf = address >> 8 & 0xFFFF;
    int32_t addr = address & 0xFF;

    if (pci_bdf_is_absent(bdf) || pci->device_spaces[bdf] == NULL) {
        pci_stop_absent("write8", address, bdf);
        return;
    }

    uint8_t *space = (uint8_t *)pci->device_spaces[bdf];

    /* pci.js:294-297: the DEBUG assert that a 8-bit write is not landing on a BAR or the
     * ROM address register. */
    DBG_ASSERT(!((addr >= 0x10 && addr < 0x2C) || (addr >= 0x30 && addr < 0x34)));

    space[addr] = (uint8_t)written;
}

void pci_write16(int32_t address, int32_t written)
{
    int32_t bdf = address >> 8 & 0xFFFF;
    int32_t addr = address & 0xFF;

    DBG_ASSERT((address & 1) == 0);

    if (pci_bdf_is_absent(bdf) || pci->device_spaces[bdf] == NULL) {
        pci_stop_absent("write16", address, bdf);
        return;
    }

    uint16_t *space = (uint16_t *)pci->device_spaces[bdf];

    if (addr >= 0x10 && addr < 0x2C) {
        /* Bochs bios (pci.js:324-329): logged and refused */
        return;
    }

    DBG_ASSERT(!(addr >= 0x30 && addr < 0x34));

    space[addr >> 1] = (uint16_t)written;
}

void pci_write32(int32_t address, int32_t written)
{
    int32_t bdf = address >> 8 & 0xFFFF;
    int32_t addr = address & 0xFF;

    DBG_ASSERT((address & 3) == 0);

    if (pci_bdf_is_absent(bdf) || pci->device_spaces[bdf] == NULL) {
        /* pci.js:386-390: an absent device's write32 is IGNORED, not an error -- the one
         * absent-device arm upstream can reach without throwing (the `.buffer`
         * dereferences that throw are write8's and write16's, above). */
        return;
    }

    int32_t *space = pci->device_spaces[bdf];
    const pci_device_t *device = pci->devices[bdf];

    if (addr >= 0x10 && addr < 0x28) {
        int32_t bar_nr = (addr - 0x10) >> 2;
        pci_bar_t *bar = device->pci_bars != NULL && bar_nr < device->pci_bars_count
                             ? &device->pci_bars[bar_nr]
                             : NULL;

        if (bar != NULL && bar->size != 0) {
            /* pci.js:450-451 */
            DBG_ASSERT((bar->size & (bar->size - 1)) == 0);

            int32_t space_addr = addr >> 2;
            int32_t type = space[space_addr] & 1;

            if ((uint32_t)(written | 3 | (int32_t)(bar->size - 1)) == 0xFFFFFFFFu) {
                /* the size probe: report the bar's size */
                written = (int32_t)(~(bar->size - 1) | (uint32_t)type);

                if (type == 0) {
                    space[space_addr] = written;
                }
            }
            else {
                if (type == 0) {
                    /* memory: changing isn't supported yet, reset to the default
                     * (pci.js:464-477) */
                    space[space_addr] = bar->original_bar;
                }
            }

            if (type == 1) {
                /* io */
                int32_t from = space[space_addr] & ~1 & 0xFFFF;
                int32_t to = written & ~1 & 0xFFFF;
                pci_set_io_bars(bar, from, to);
                space[space_addr] = written | 1;
            }
        }
        else {
            space[addr >> 2] = 0;
        }
    }
    else if (addr == 0x30) {
        /* the expansion ROM address register (pci.js:486-505) */
        if (device->pci_rom_size) {
            if ((uint32_t)(written | 0x7FF) == 0xFFFFFFFFu) {
                space[addr >> 2] = (int32_t)(0u - device->pci_rom_size);
            }
            else {
                space[addr >> 2] = device->pci_rom_address;
            }
        }
        else {
            space[addr >> 2] = 0;
        }
    }
    else {
        /* including addr == 0x04, which upstream only logs (pci.js:507-513) */
        space[addr >> 2] = written;
    }
}

/* ------------------------------------------------------------------ *
 * pci.js:487-549 -- registration and the I/O bar machinery
 * ------------------------------------------------------------------ */
int32_t *pci_register_device(pci_device_t *device)
{
    DBG_ASSERT(device->pci_space != NULL);
    DBG_ASSERT(device->pci_id < PCI_DEVICE_COUNT);
    DBG_ASSERT(device->pci_bars_count == 0 || device->pci_bars != NULL);

    int32_t device_id = device->pci_id;

    int32_t *space = device_space_storage[device_id];
    DBG_ASSERT(space != NULL);
    DBG_ASSERT(device->pci_space_length >= 64 && device->pci_space_length <= PCI_SPACE_DWORDS * 4);
    /* pci.js:501-504: convert bytewise notation from lspci to double words.  The space is
     * 256 bytes and the device's array may be shorter (64) or longer (96 for the host
     * bridge, whose PAM0 is at 0x59): the Int32Array starts zeroed and only what the
     * device declares is copied. */
    memset(space, 0, PCI_SPACE_DWORDS * 4);
    memcpy(space, device->pci_space, (size_t)device->pci_space_length);
    pci->device_spaces[device_id] = space;
    pci->devices[device_id] = device;

    for (int32_t i = 0; i < device->pci_bars_count; i++) {
        pci_bar_t *bar = &device->pci_bars[i];
        if (bar->size == 0) continue;

        int32_t bar_base = space[4 + i];
        int32_t type = bar_base & 1;

        bar->original_bar = bar_base;
        bar->entries = NULL;

        if (type == 0) {
            /* memory, not needed currently */
        }
        else {
            DBG_ASSERT(type == 1);
            DBG_ASSERT(bar_entry_pool_used + (int32_t)bar->size <= PCI_BAR_ENTRY_POOL);
            bar->entries = &bar_entry_pool[bar_entry_pool_used];
            bar_entry_pool_used += (int32_t)bar->size;

            int32_t port = bar_base & ~1;
            for (uint32_t j = 0; j < bar->size; j++) {
                DBG_ASSERT(port + (int32_t)j < IO_PORT_COUNT);
                bar->entries[j] = io_ports[port + (int32_t)j];
            }
        }
    }

    return space;
}

/* pci.js:519-549 */
void pci_set_io_bars(pci_bar_t *bar, int32_t from, int32_t to)
{
    uint32_t count = bar->size;

    for (uint32_t i = 0; i < count; i++) {
        if (from + (int32_t)i >= 0x1000) {
            io_ports[from + (int32_t)i] = io_create_empty_entry();
        }

        io_port_entry_t entry = bar->entries[i];
        io_port_entry_t empty_entry = io_ports[to + (int32_t)i];
        (void)empty_entry; /* upstream asserts both exist */
        DBG_ASSERT(empty_entry.device == NULL || empty_entry.read8 != NULL ||
                   empty_entry.write8 != NULL);

        if (to + (int32_t)i >= 0x1000) {
            io_ports[to + (int32_t)i] = entry;
        }
    }
}

/* ------------------------------------------------------------------ *
 * pci.js:552-604 -- the two IRQ paths
 * ------------------------------------------------------------------ */
void pci_raise_irq(int32_t pci_id)
{
    int32_t *space = pci->device_spaces[pci_id];
    DBG_ASSERT(space != NULL);

    int32_t pin = ((space[0x3C >> 2] >> 8) & 0xFF) - 1;
    int32_t device = ((pci_id >> 3) - 1) & 0xFF;
    int32_t parent_pin = (pin + device) & 3;
    int32_t irq = pci->isa_bridge_space8[0x60 + parent_pin];

    device_raise_irq((uint8_t)irq);
}

void pci_lower_irq(int32_t pci_id)
{
    int32_t *space = pci->device_spaces[pci_id];
    DBG_ASSERT(space != NULL);

    int32_t pin = space[0x3C >> 2] >> 8 & 0xFF;
    int32_t device = (pci_id >> 3) & 0xFF;
    int32_t parent_pin = (pin + device - 2) & 3;
    int32_t irq = pci->isa_bridge_space8[0x60 + parent_pin];

    device_lower_irq((uint8_t)irq);
}
