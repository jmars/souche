/*
 * machine.c -- the machine's device layer: the wiring (cpu.js:1176-1266), the timer poll
 * (cpu.js:1924-1941) and the two IRQ entry points (cpu.rs:4596-4610).  See machine.h.
 *
 * THE DEVICE SET, in cpu.js's order, is exactly the one this unit ports:
 *
 *     io_init()                      the port map and the memory-map dispatch (io.js)
 *     pci_init()                     the bus itself + 00:00.0 and 00:01.0
 *     acpi_init()                    only when cpu->acpi_enabled (cpu.js:1181-1185)
 *     rtc_init(); rtc_fill_cmos()    the CMOS RTC + its bring-up
 *     dma_init()                     the 8237
 *     ps2_init(device_bus)           the 8042 + keyboard + mouse
 *     uart_init(device_bus)          M2-2: COM1 at 0x3F8 (uart.c)
 *     pit_init(device_bus)           the 8254
 *
 * Absent, and therefore NOT devices on this machine: the VGA (and its LFB), COM2-COM4 (the
 * reference's settings.uart1/2/3 arms -- uart_attach exists for them and nothing enables
 * them by default), the parallel port, the floppy controller, the IDE controller, the
 * network cards, virtio and the sound card -- the following M2 units.  Their ports are UNCLAIMED, and an unclaimed
 * port answers exactly as the pinned reference's empty entry does -- 0xFF/0xFFFF/-1 on a
 * read, a discarded write (io.h D1, D-F1/M2-1F) -- with `V86C_LOG_PORTS` printing each such
 * access to stderr as a diagnostic.  M2-1 stopped on them instead; M2-1F made that a
 * divergence from the reference and removed it, because the first boot's first POST code
 * writes port 0x80.
 */

#include "machine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../cpu/cpu.h"      /* unimplemented_instruction */
#include "../cpu/cpu_state.h"
#include "../cpu/instructions.h" /* handle_irqs */
#include "../cpu/jit_mem.h"  /* REBOOT-1: jit_enabled / jit_abort_flag */
#include "../cpu/pic.h"      /* set_irq / clear_irq, pic_reset */
#include "acpi.h"
#include "dma.h"
#include "ide.h"
#include "io.h"
#include "pci.h"
#include "pit.h"
#include "ps2.h"
#include "rtc.h"
#include "uart.h"
#include "virtio_console.h"

bus_connector_t *machine_bus = NULL;
bus_connector_t *machine_host_bus = NULL;

/* M2-4: the machine's virtio console, if it has one (machine_config.virtio_console).  It is
 * static because its bus listeners keep POINTERS into it (bus.h stores the this_value), so it
 * must outlive the machine; machine_init() re-initialises it in place. */
static virtio_console_t machine_virtio_console;

virtio_console_t *machine_virtio_console_device(void)
{
    return machine_config.virtio_console ? &machine_virtio_console : NULL;
}

/* The disk slots the machine is built with.  Zeroed is "no disk, no media" -- which is what
 * the reference's own default settings give, except for the CD-ROM: cpu.js:1226 makes the
 * secondary channel's master an ATAPI device UNCONDITIONALLY, so machine_init fills that
 * slot in below even when the caller left it zeroed. */
machine_config_t machine_config = { { NULL, 0, false }, { NULL, 0, false }, { NULL, 0, true },
                                    false };

static bool machine_ready = false;

bool machine_is_ready(void) { return machine_ready; }

void machine_init(void)
{
    io_init();

    bus_reset();
    bus_create(&machine_host_bus, &machine_bus);

    pci_init();

    if (cpu->acpi_enabled) {
        acpi_init();
    }

    rtc_init();
    rtc_fill_cmos();

    dma_init();

    ps2_init(machine_bus);

    /* cpu.js:1201: `this.devices.uart0 = new UART(this, 0x3F8, device_bus)`, in the device
     * block's own order (after the PS/2, before the IDE and the PIT).  uart1/uart2/uart3 are
     * created only when settings.uart1/2/3 are set (cpu.js:1203-1213), which by default they
     * are not -- src/hw/uart.h has the second channel's constructor for a later unit. */
    uart_init(machine_bus);

    /* cpu.js:1219-1228: the IDE controller.  The secondary channel's master is ALWAYS an
     * ATAPI device in the reference (`ide_config[1][0] = { is_cdrom: true, buffer:
     * settings.cdrom }`), so the controller -- and its PCI function -- exists in every
     * configuration; only the disks are optional. */
    ide_init(machine_bus, machine_config.hda.buffer != NULL ? &machine_config.hda : NULL,
             machine_config.hdb.buffer != NULL ? &machine_config.hdb : NULL,
             &machine_config.cdrom);

    /* cpu.js:1253-1256: `if(settings.virtio_console) this.devices.virtio_console =
     * new VirtioConsole(this, device_bus)`.  It sits in cpu.js's device block AFTER the IDE
     * and BEFORE the balloon/net, so it is created here in that order.  Nothing is created
     * when the setting is false, which is the reference's own default. */
    if (machine_config.virtio_console) {
        if (!virtio_console_init(&machine_virtio_console, machine_bus)) {
            unimplemented_instruction("machine_init: virtio_console_init refused its options");
        }
    }

    pit_init(machine_bus);

    machine_ready = true;
}

/* REBOOT-1: cpu.js:939-965, reboot_internal().  See machine.h for the contract and
 * the device-by-device decision; results/reboot-1.md holds the evidence.
 *
 * ORDER: reset_cpu first (the reference's own order), then the device resets, then
 * the request flag.  The reference's `this.fw_value = []` has no counterpart here:
 * the port has no fw_cfg state (its firmware reads the option ROM out of guest RAM
 * at 0xC0000, tests/minibios.asm:252-258), so there is nothing to clear.
 *
 * THE DEVICES, one decision each (the reference names only the virtios and the PS/2):
 *   - reset_cpu          cpu.rs:4671-4760: the reset state, the reset vector at
 *                        0xFFFF0, and -- the one line the port's interpreter-only
 *                        past dropped -- cpu.rs:4757's jit::jit_clear_cache_js().
 *                        That clear CANNOT run here (machine_reboot is mid-block;
 *                        the executor holds arena pointers), so main.c runs it
 *                        (uop_cache_clear, which drops the compiled blocks with the
 *                        decoded ones) after the unwind, BEFORE boot 2 executes.
 *   - pic_reset          NOT named upstream (seabios's POST reprograms the PICs at
 *                        boot 2; this port's firmware does not touch them).  Without
 *                        it, boot 2 runs the firmware under boot 1's remap and IMR:
 *                        any line a raiser still drives delivers into the firmware's
 *                        IVT -- the silent-wrong class.
 *   - pit_init           NOT named upstream (seabios programs the PIT in its POST;
 *                        the firmware does not).  Boot 1's tick rate must not fire
 *                        into boot 2's firmware bring-up.
 *   - rtc_init +         NOT named upstream; the boot-2 firmware re-reads the RTC
 *     rtc_fill_cmos      as the BIOS bring-up left it.  The CMOS clock is FIXED-
 *                        EPOCH virtual time (D-F2), so the re-run is deterministic.
 *   - virtio_console     the reference NAMES it (cpu.js:948-951) -- when the machine
 *                        has one (machine_config.virtio_console).
 *   - ps2                the reference NAMES it (cpu.js:952-955).
 *   - uart0 NOT reset, on TWO grounds, both measured: (a) the reference does not
 *     reset it and the port's UART raises only from guest port writes and host
 *     input events (uart.c: every uart_raise_interrupt call site is in
 *     data_received / set_modem_status_bit / write_data / port1_write8) -- no poll,
 *     no timer -- so a stale IER cannot fire into boot 2 uninvited; and (b) the
 *     port's uart_attach ALLOCATES (uart.c) and bus_register APPENDS (bus.c), so a
 *     reboot-time re-init leaks a device and five listeners per boot -- the
 *     runaway-reboot image reaches bus.c:41's 64-listener assert before the
 *     driver's MAX_REBOOTS guard.  (Upstream's boot-2 firmware re-inits COM1 in
 *     its POST; this port's does not, and the kernel's 8250 driver re-inits it
 *     before unmasking IRQ4.)
 * NOT reset, mirroring the reference -- each survives a CPU reset there too:
 *   - dma   no live timer, no interrupt line of its own; the firmware and the kernel
 *           reprogram the channels they use before any transfer.
 *   - ide   the disk and the controller state survive, exactly as the reference's
 *           IDEController does (cpu.js:943-958 names no IDE reset -- the class does
 *           not have one upstream).  A guest rebooting mid-command leaves the drive
 *           busy; recovery is boot 2's own probe discipline (libata softresets at
 *           probe), the same arrangement the reference's browser lane boots with.
 *   - pci   the config space persists (the firmware's IDE-enable writes re-run
 *           idempotently); the 0xCF9 trigger's own byte is NOT stored, exactly as
 *           upstream's handler returns before `pci_addr[1] = out_byte`.
 *   - acpi  the device is not constructed in this port's boots (cpu->acpi_enabled is
 *           false and no CLI flag sets it), so it has no state to reset.
 *   - the CMOS RTC's time fields persist through rtc_init's re-run only as far as
 *         the fixed-epoch virtual clock makes them identical anyway; nothing else
 *         in the RTC is observable across the reboot.
 * MEMORY IS NOT RESET: reset_memory() (cpu.js:969-972) is a separate method the
 * reference's reboot_internal does not call, and that property is the point of an
 * in-place reboot -- the gate image proves the marker survives. */
void machine_reboot(void)
{
    reset_cpu();

    pic_reset();

    pit_init(machine_bus);

    rtc_init();
    rtc_fill_cmos();

    if (machine_config.virtio_console) {
        virtio_console_reset(&machine_virtio_console);
    }

    ps2_reset();

    /* the run loop's unwinding request -- see cpu_state.h's field note.  The
     * compiled block that contains the triggering port write reads the abort line
     * at its next uop boundary and exits, the same channel an SMC write uses. */
    cpu->reboot_requested = true;
    if (jit_enabled) {
        jit_abort_flag = 1;
    }
}

/* cpu.js:1924-1941 */
double run_hardware_timers(bool acpi_enabled, double t)
{
    /* The M1c no-device machine: exec_boundary.c's seat returned its argument, and a
     * machine that was never assembled keeps that behaviour rather than inventing a
     * device.  (m1c_selftest.c never calls machine_init, so this is the arm it runs.) */
    if (!machine_ready) return t;

    double pit_time = pit_timer(t, false);
    double rtc_time = rtc_timer(t, false);

    double acpi_time = 100;
    if (acpi_enabled && acpi != NULL) {
        acpi_time = acpi_timer(t);
    }

    /* upstream also takes this.apic_timer(now)'s value when acpi_enabled; apic.rs is
     * deferred, so there is no APIC timer to ask. */
    return fmin(fmin(pit_time, rtc_time), acpi_time);
}

/* cpu.rs:4596-4610 */
void device_raise_irq(uint8_t i)
{
    set_irq(i);
    if (cpu->acpi_enabled) {
        /* upstream: ioapic::set_irq(i).  ioapic.rs is deferred, so a machine with
         * acpi_enabled STOPS here rather than raising an IRQ the IOAPIC half never gets. */
        unimplemented_instruction("device_raise_irq: ioapic::set_irq (ioapic.rs is M2)");
        return;
    }
    handle_irqs();
}

void device_lower_irq(uint8_t i)
{
    clear_irq(i);
    if (cpu->acpi_enabled) {
        unimplemented_instruction("device_lower_irq: ioapic::clear_irq (ioapic.rs is M2)");
        return;
    }
    handle_irqs();
}
