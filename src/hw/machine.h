/*
 * machine.h -- the machine: the device layer's wiring and its two services.
 *
 * Port of the three parts of v86/src/cpu.js that have no JS device file of their own:
 *
 *   - CPU.prototype.init's device block (cpu.js:1176-1266), i.e. `load_devices` -- the
 *     order the devices are created in and the bus they share;
 *   - CPU.prototype.run_hardware_timers (cpu.js:1924-1941) -- the poll main_loop's timer
 *     seat calls; this replaces exec_boundary.c's M1c stub, which returned its argument;
 *   - device_raise_irq / device_lower_irq (cpu.rs:4596-4610), which pic.h deliberately
 *     left unported ("M2's device layer adds them").
 *
 * This file is the ONLY place the device set is assembled, so the port has one answer to
 * "what machine is this" instead of one per caller.  The selftest's fresh machine is
 * machine_init() again.
 *
 * NOT in this machine, and therefore not devices: the VGA, the floppy controller, virtio-net
 * /virtio-9p/virtio-balloon, the APIC/IOAPIC and the sound card -- the later M2 units.
 * THE VIRTIO CONSOLE IS here when machine_config.virtio_console is set (M2-4): the reference
 * creates it only for `settings.virtio_console` (cpu.js:1253), so the default machine has
 * none and the M0 transcript machine stays identical to the reference's.  A guest that touches one of their
 * ports gets an unclaimed entry's answer (io.h D1), never a fabricated value.  COM1 IS here
 * (M2-2, uart.c); COM2-COM4 are the reference's settings.uart1/2/3 arms and are attached only
 * when a caller asks (uart_attach).
 *
 * M2-3 (HANDOFF=v86c) ADDED THE IDE CONTROLLER (src/hw/ide.c) to that set, in cpu.js's own
 * order (after the UART, before the PIT), because the reference's machine ALWAYS has one:
 * cpu.js:1226 gives the secondary channel an ATAPI CD-ROM unconditionally, so the IDE
 * function 00:1e.0 is registered even when no disk is attached (MEASURED in
 * results/m2-2-js-serial.txt, where the reference's kernel enumerates it and this port's did
 * not).  What is OPTIONAL is the disk: machine_config's three slots are the reference's
 * settings.hda/hdb/cdrom, and a zeroed machine_config is "no disk, no media".
 */

#ifndef V86C_HW_MACHINE_H
#define V86C_HW_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "ide.h"
#include "virtio_console.h"

/* cpu.js:1219-1228's settings: the three disk slots the machine is built with.  A slot
 * whose `buffer` is NULL and whose `is_cdrom` is false is the reference's MISSING device;
 * the CD-ROM slot is always an ATAPI device (`is_cdrom` true) and its buffer is its media.
 * main.c fills this BEFORE machine_init(); a caller that leaves it zeroed gets a machine
 * with no disks, which is what the reference's own defaults give. */
typedef struct machine_config {
    ide_disk_t hda;
    ide_disk_t hdb;
    ide_disk_t cdrom;

    /* M2-4: the reference's `settings.virtio_console` (cpu.js:1253-1256).  The virtio console
     * is created ONLY when this is set -- unlike the IDE controller, which the reference
     * always creates.  The default is false, which is what the reference's default settings
     * give, and it is what keeps the M0 transcript machine identical to the reference's. */
    bool virtio_console;
} machine_config_t;

extern machine_config_t machine_config;

/* io_init() + the device set, in cpu.js's order (pci, acpi if enabled, rtc, dma, ps2,
 * uart0, ide, pit).  Requires cpu->memory_size to be set (main.c does cpu_state_init() +
 * allocate_memory() first).  Calling it again re-creates a fresh machine. */
void machine_init(void);

/* REBOOT-1: the machine reboot path -- the port's cpu.reboot_internal()
 * (cpu.js:939-965).  MAY ONLY BE CALLED FROM A DEVICE HANDLER INSIDE A GUEST PORT
 * WRITE (the two trigger sites: pci.c's 0xCF9 dance and ps2.c's 0x64 0xFE command).
 *
 * What it does, in the reference's order (cpu.js:939-965): reset_cpu(), then the
 * devices' own resets.  The reference names virtio_9p / virtio_console / virtio_net /
 * ps2; this port has the console (when the machine has one) and the PS/2 of those.
 * THE PORT RESETS MORE THAN THE REFERENCE NAMES -- the PIC and the PIT, and re-runs
 * the RTC bring-up -- because the reference's boot re-entry re-runs SEABIOS,
 * whose POST reprograms all of those before any guest code runs, while this port's
 * firmware (tests/minibios.asm) re-runs but programs none of them.  The devices it
 * does NOT reset (dma, ide, pci, acpi, and the RTC's battery-backed CMOS clock) are
 * the reference's own answer, each with its stale-state note in the file comment
 * below and in results/reboot-1.md.  reset_memory() is NOT called: guest memory and
 * the disk survive, which is what makes an in-place reboot useful (cpu.js:969-972 is
 * a separate method the reference itself does not call here).
 *
 * What it does NOT do, deliberately: clear the uop/JIT code caches and re-enter the
 * boot.  The caches CANNOT be cleared here -- machine_reboot runs mid-block, and the
 * executor and the compiled code hold pointers into the arena the clear recycles.
 * Instead this function raises cpu->reboot_requested (and the compiled block's own
 * abort line, jit_abort_flag, when the JIT is on); the run loop unwinds to main.c,
 * which clears the caches at the block boundary and then re-establishes the boot for
 * the boot mode in use. */
void machine_reboot(void);

/* False until machine_init has run -- the M1c no-device machine.  run_hardware_timers
 * reports that state to its caller rather than inventing a timer. */
bool machine_is_ready(void);

/* cpu.js:1924-1941.  Drives the PIT and the RTC, and -- when acpi_enabled is set --
 * the ACPI timer AND the APIC timer (cpu.js:1931-1935's single arm over both), then
 * returns the smallest of their next-interrupt times. */
double run_hardware_timers(bool acpi_enabled, double t);

/* cpu.rs:4596-4610.  The devices' only way to interrupt: raise/lower the IRQ line, run
 * the IOAPIC's half of the same line when acpi_enabled is set, then run the poll
 * (handle_irqs -- which, with nothing pending at the PIC, asks the local APIC next,
 * cpu.rs:4578-4582). */
void device_raise_irq(uint8_t i);
void device_lower_irq(uint8_t i);

/* M2-4: the machine's virtio console device, or NULL when the machine has none (the
 * reference's own default: cpu.js:1253 creates it only for `settings.virtio_console`).  The
 * counters the gate reads live on it (virtio_console.h). */
virtio_console_t *machine_virtio_console_device(void);

/* The two ends of the device bus (upstream's `device_bus` and the host/UI connector it is
 * paired with).  The devices hold `machine_bus`; a later unit's host input path sends
 * "keyboard-code" / "mouse-delta" / ... into `machine_host_bus`. */
extern bus_connector_t *machine_bus;
extern bus_connector_t *machine_host_bus;

#endif /* V86C_HW_MACHINE_H */
