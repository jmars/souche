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
 *     vga_init(screen)               VGA-U0: the VGA device, always (cpu.js:1193)
 *     ps2_init(device_bus)           the 8042 + keyboard + mouse
 *     uart_init(device_bus)          M2-2: COM1 at 0x3F8 (uart.c)
 *     pit_init(device_bus)           the 8254
 *
 * THE VGA IS ALWAYS PRESENT since VGA-U0, as it is upstream: what the machine's config
 * selects is only its RENDERING ADAPTER (headless by default).  Its I/O PORTS are still
 * unclaimed in U0 -- the register file is U1-U4's -- while its 0xA0000 window, its 8 MiB
 * LFB and its PCI function 00:12.0 are the device's; see src/hw/vga.h.
 *
 * Absent, and therefore NOT devices on this machine: COM2-COM4 (the
 * reference's settings.uart1/2/3 arms -- uart_attach exists for them and nothing enables
 * them by default), the parallel port, the floppy controller, the network cards, virtio-net
 * /balloon/9p and the sound card -- the later units.  Their ports are UNCLAIMED, and an unclaimed
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
#include "../cpu/apic.h"     /* apic_timer -- the timer fan-out's fourth value */
#include "../cpu/instructions.h" /* handle_irqs */
#include "../cpu/ioapic.h"   /* ioapic_set_irq/clear_irq -- the raise/lower path */
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
                                    false, VGA_SCREEN_HEADLESS, NULL };

static bool machine_ready = false;

bool machine_is_ready(void) { return machine_ready; }

void machine_init(void)
{
    /* APIC-1: the LOCAL APIC and the IOAPIC are module-level state upstream (apic.rs:80's
     * static Mutex, ioapic.rs:63's static Mutex -- created with the wasm module, not by
     * the device block).  This port gives them an explicit init at the one place a
     * machine is brought up, which for the shipped binary is once per process -- the
     * same freshness.  machine_reboot deliberately does NOT call this (the reboot
     * section below), so APIC/IOAPIC state survives a guest reboot exactly as the
     * reference's statics survive its reboot_internal. */
    apic_init();
    ioapic_init();

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

    /* cpu.js:1193: `this.devices.vga = new VGAScreen(this, device_bus, settings.screen,
     * settings.vga_memory_size || 8 * 1024 * 1024)` -- in the device block's own order,
     * after the DMA and BEFORE the PS/2, and unconditional.  The adapter is the reference's
     * `settings.screen`; machine_config.vga_screen is the port's reader of it (main.c's
     * --vga-ansi / --vga-doc).  vga.h states what the device answers in U0 and which unit
     * owns each of the rest. */
    {
        vga_screen_t *screen = NULL;
        switch (machine_config.vga_screen) {
        case VGA_SCREEN_ANSI: screen = vga_screen_ansi_init(); break;
        case VGA_SCREEN_DOC: screen = vga_screen_document_init(); break;
        case VGA_SCREEN_HEADLESS:
        default: screen = vga_screen_headless_init(); break;
        }
        vga_init(screen, machine_config.vga_screen);
    }

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
 *   - acpi  NOT reset.  The device (and, with it, the APIC/IOAPIC state -- all three are
 *           gated by cpu->acpi_enabled, APIC-1) is constructed only under `--acpi`;
 *           upstream's reboot_internal names none of them either, so a `--acpi` guest
 *           rebooting itself carries its LAPIC/IOAPIC state into boot 2 exactly as the
 *           reference's statics do.  Two boot-2 consequences, both upstream's own
 *           behaviour, named because they are visible: (a) the clock REWINDS (reset_cpu
 *           zeroes the instruction counter microtick derives from), and both timers'
 *           future-timestamp arms handle it -- apic.c's `timer_last_tick > now` reset and
 *           acpi.c's dropped-overshot-assert arm; (b) an LVT/IOAPIC entry boot 1 left
 *           unmasked keeps delivering into boot 2 until the guest re-masks it -- which a
 *           re-booting kernel's LAPIC re-init does (upstream ships the same semantics).
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

    /* cpu.js's own composition (cpu.js:1931-1935): both the ACPI timer and the APIC
     * timer fold in under the ONE acpi_enabled arm.  The APIC timer reads no device
     * object (upstream asks this.apic_timer, the CPU's own method over the static
     * APIC state), so unlike the ACPI half it needs no existence check. */
    double acpi_time = 100;
    double apic_time = 100;
    if (acpi_enabled) {
        if (acpi != NULL) {
            acpi_time = acpi_timer(t);
        }
        apic_time = apic_timer(t);
    }

    return fmin(fmin(pit_time, rtc_time), fmin(acpi_time, apic_time));
}

/* cpu.rs:4596-4610, with one board-wiring fact the upstream line cannot carry: the PIT's
 * output line reaches the IOAPIC's INPUT PIN 2, not pin 0 -- the classic ISA IRQ0 -> GSI 2
 * steering a real southbridge does, and the very override acpi_tables.c's MADT publishes.
 * Upstream raises ioapic::set_irq(i) identity (cpu.rs:4599) because no upstream guest ever
 * reads a MADT (there is no firmware builder); the moment ours does, a kernel that took the
 * override would unmask pin 2 for its timer and pin 0 would go silent.  The PIN numbers in
 * the IOAPIC are unchanged (the redirection table, the masks, the bounds) -- only which pin
 * an ISA line lands on.  The PIC half stays identity: the 8259's IRQ0 line IS line 0. */
#define IOAPIC_PIN_PIT 2u

static uint8_t ioapic_pin_of_isa_irq(uint8_t i)
{
    return i == 0 ? IOAPIC_PIN_PIT : i;
}

void device_raise_irq(uint8_t i)
{
    set_irq(i);
    if (cpu->acpi_enabled) {
        ioapic_set_irq(ioapic_pin_of_isa_irq(i));
    }
    handle_irqs();
}

void device_lower_irq(uint8_t i)
{
    clear_irq(i);
    if (cpu->acpi_enabled) {
        ioapic_clear_irq(ioapic_pin_of_isa_irq(i));
    }
    handle_irqs();
}
