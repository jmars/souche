/* acpi.c -- the PIIX4 ACPI controller.  See acpi.h. */

#include "acpi.h"

#include <math.h>
#include <string.h>

#include "../cpu/cpu_state.h"
#include "../cpu/instructions.h" /* microtick */
#include "io.h"
#include "machine.h"
#include "pci.h"

static acpi_state_t acpi_storage;
acpi_state_t *acpi = NULL;

/* acpi.js:18-21 */
#define PMTIMER_FREQ_SECONDS 3579545

/* acpi.js:24-33: the device's 64-byte PCI config space, verbatim (00:07.0, an Intel
 * 82371AB/EB/MB PIIX4 ACPI, revision 08). */
static const uint8_t acpi_pci_space[64] = {
    0x86, 0x80, 0x13, 0x71, 0x07, 0x00, 0x80, 0x02, 0x08, 0x00, 0x80, 0x06, 0x00, 0x00, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x01, 0x00, 0x00,
};

/* ------------------------------------------------------------------ *
 * the ports -- acpi.js:48-135
 *
 * NOTE the widths upstream registers: 0xB000 and 0xB002 are 16-bit only, 0xB004 is 8- and
 * 16-bit, 0xB008 is 32-bit only, and the four GPE registers are 8-bit only.  A guest that
 * reads a register at a width the device did not register gets the loud stop (io.h D2) --
 * upstream's DEBUG build asserts there and its release build throws.
 * ------------------------------------------------------------------ */
static int32_t portB000_read16(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return (int32_t)acpi->pm1_status;
}

static void portB000_write16(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    acpi->pm1_status &= ~(uint32_t)value;
}

static int32_t portB002_read16(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return (int32_t)acpi->pm1_enable;
}

static void portB002_write16(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    acpi->pm1_enable = (uint32_t)value;
}

static int32_t portB004_read8(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return (int32_t)(acpi->status & 0xFF);
}

static int32_t portB004_read16(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return (int32_t)acpi->status;
}

static void portB004_write16(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    acpi->status = (uint32_t)value;
}

static int32_t portB008_read32(void *device, int32_t port)
{
    (void)device;
    (void)port;
    /* acpi.js:96-103: the value is masked to 24 bits */
    return acpi_get_timer(microtick()) & 0xFFFFFF;
}

static int32_t gpe_read8(void *device, int32_t port)
{
    (void)device;
    /* acpi.js:106-130 registers 0xAFE0..0xAFE3 in order */
    return acpi->gpe[port - 0xAFE0];
}

static void gpe_write8(void *device, int32_t port, int32_t value)
{
    (void)device;
    acpi->gpe[port - 0xAFE0] = (uint8_t)value;
}

void acpi_init(void)
{
    acpi = &acpi_storage;
    memset(&acpi_storage, 0, sizeof(acpi_storage));

    /* acpi.js:24-46: the device, registered on the PCI bus */
    static pci_device_t acpi_device = {
        .pci_id = 0x07 << 3,
        .pci_space = acpi_pci_space,
        .pci_space_length = (int32_t)sizeof(acpi_pci_space),
        .pci_bars = NULL,
        .pci_bars_count = 0,
        .name = "acpi",
        .pci_rom_size = 0,
        .pci_rom_address = 0,
    };
    pci_register_device(&acpi_device);

    acpi->status = 1;
    /* acpi.js:51-56: timer_last_value/imprecision start at 0 and last_timer is the first
     * read of the (zero) emulator clock. */
    acpi->last_timer = acpi_get_timer(microtick());

    io_register_read(0xB000, acpi, NULL, portB000_read16, NULL);
    io_register_write(0xB000, acpi, NULL, portB000_write16, NULL);

    io_register_read(0xB002, acpi, NULL, portB002_read16, NULL);
    io_register_write(0xB002, acpi, NULL, portB002_write16, NULL);

    io_register_read(0xB004, acpi, portB004_read8, portB004_read16, NULL);
    io_register_write(0xB004, acpi, NULL, portB004_write16, NULL);

    io_register_read(0xB008, acpi, NULL, NULL, portB008_read32);

    for (int32_t i = 0; i < 4; i++) {
        io_register_read(0xAFE0 + i, acpi, gpe_read8, NULL, NULL);
        io_register_write(0xAFE0 + i, acpi, gpe_write8, NULL, NULL);
    }
}

/* acpi.js:136-155 */
double acpi_timer(double now)
{
    int32_t timer = acpi_get_timer(now);
    bool highest_bit_changed = ((timer ^ acpi->last_timer) & (1 << 23)) != 0;

    if ((acpi->pm1_enable & 1) && highest_bit_changed) {
        acpi->pm1_status |= 1;
        device_raise_irq(9);
    }
    else {
        device_lower_irq(9);
    }

    acpi->last_timer = timer;
    return 100; /* TODO (acpi.js:153) */
}

/* acpi.js:184-206 */
int32_t acpi_get_timer(double now)
{
    int32_t t = (int32_t)round(now * (PMTIMER_FREQ_SECONDS / 1000.0));

    if (t == acpi->timer_last_value) {
        /* don't go past 1ms */
        if (acpi->timer_imprecision_offset < PMTIMER_FREQ_SECONDS / 1000) {
            acpi->timer_imprecision_offset++;
        }
    }
    else {
        /* acpi.js:196: dbg_assert!(t > this.timer_last_value) -- AUDIT-1R SF2 (the S1
         * re-sweep).  [PREMISE CORRECTED after the acpi-1r review's N2: this comment
         * called the rewind path "LATENT -- the device is constructed only when
         * cpu->acpi_enabled is set, a flag with NO writer in this port, so no boot
         * ever registers 0xB008".  That flag writer EXISTS now: the `--acpi` CLI flag
         * (src/main.c) sets it, machine.c constructs the device under it, and every
         * --acpi boot registers 0xB008 -- the path below is LIVE, not latent.]  The
         * assert-drop reasoning is unchanged and GUEST-REACHABLE under --acpi: D-F2's
         * microtick() derives from instruction_counter, which reset_cpu zeroes, while
         * ACPI state persists (machine_reboot deliberately resets only
         * pic/pit/rtc/ps2/virtio_console, mirroring the reference) -- so the first
         * PM-timer read after a guest-initiated reboot (0xCF9 or 0xFE) has
         * t < timer_last_value and the assert aborted the machine.  The release
         * reference compiles the assert out even in DEBUG (its clock is the monotonic
         * wall clock and it cannot reach the rewind at all); the graceful overshoot
         * arm below -- the same anticipation the PIT carries for exactly this
         * clock-goes-backward case (pit.c's `diff < 0` arm) -- is the behaviour, so
         * the assert is dropped and the arm stands. */
        int32_t previous_timer = acpi->timer_last_value + acpi->timer_imprecision_offset;

        /* don't go back in time */
        if (previous_timer <= t) {
            acpi->timer_imprecision_offset = 0;
            acpi->timer_last_value = t;
        }
        /* else: upstream logs a "Warning: Overshot pmtimer, waiting" and changes nothing */
    }

    return acpi->timer_last_value + acpi->timer_imprecision_offset;
}
