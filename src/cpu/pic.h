/*
 * pic.h -- the 8259 programmable interrupt controller, the interrupt poll, and the
 * device layer's port-I/O bridge.
 *
 * Port of v86/src/rust/cpu/pic.rs (402 lines) plus cpu.rs:4573-4649 (handle_irqs,
 * pic_call_irq, and io_port_read8/read16/read32 / io_port_write8/16/32 with the PIC's six
 * ports intercepted).  Plan ss7 step 18.
 *
 * WHAT IS REAL AND WHAT IS STUBBED (read this before reading the code):
 *   - the PIC itself is a faithful port: the two 8259s, their ICW/OCW state machine, the
 *     mask/ISR/IRR/ELCR registers and the acknowledge priority rule, plus the six
 *     programming ports (0x20/0x21/0xA0/0xA1) and the two ELCR ports (0x4D0/0x4D1);
 *   - NOTHING EVER RAISES AN IRQ.  A guest's programming writes take effect and are
 *     readable, but a device-level interrupt cannot occur until M2's device layer calls
 *     set_irq (the PIT, the keyboard, ...).  What a guest observes: `sti; hlt` after
 *     programming the PIC idles forever -- no timer interrupt is ever delivered, so a
 *     guest that waits for one makes no progress.  That is the honest consequence of
 *     having no devices, and it is the same machine the M0 oracle runs (no devices);
 *   - the non-PIC ports STOP the machine with the port named (the M1c/M2 device-layer
 *     seat, moved here from exec_boundary.c: cpu.rs:4613-4649's `js::io_port_*` arms are
 *     the device layer, which does not exist yet).  A guest that touches the PIT/RTC/
 *     keyboard ports reports the port instead of reading a fabricated value.
 *
 * `device_raise_irq`/`device_lower_irq` (cpu.rs:4596-4610) are NOT ported here: their
 * only callers are v86's javascript devices and their body also drives the IOAPIC, which
 * is M2's (apic.rs/ioapic.rs are deferred).  M2-1's device layer defines them in
 * src/hw/machine.c.
 *
 * M2-1 HANDOVER (HANDOFF=v86c): the `io_port_*` bridge that used to close this unit's
 * pic.c MOVED to src/hw/io.c, which owns the port map now.  What stays HERE is the six
 * programming ports' register accessors (declared below) and the two ELCR ports' -- io.c
 * intercepts them in front of its map, which is upstream's call order.
 */

#ifndef V86C_PIC_H
#define V86C_PIC_H

#include <stdbool.h>
#include <stdint.h>

/* pic.rs:367-385 -- the device layer's two entry points (the JS exports upstream). */
void set_irq(uint8_t i);
void clear_irq(uint8_t i);

/* pic.rs:294-331 -- the poll's acknowledge.  Upstream returns Option<u8>; the port
 * returns the interrupt number and -1 for None (no in-range value can collide). */
int32_t pic_acknowledge_irq(void);

/* handle_irqs (cpu.rs:4573) is DEFINED here; its declaration is instructions.h's, next to
 * the other boundaries instructions.c calls (POPF/STI/HLT are its first callers).
 * io_port_read8/16/32 and io_port_write8/16/32 (cpu.rs:4613-4649) are declared in
 * string.h, for the same reason (in/out are theirs), and DEFINED in src/hw/io.c (M2-1). */

/* REBOOT-1: the initial state again (pic.rs:47-92) -- machine_reboot()'s reset for
 * both controllers.  See pic.c for why the port's reboot resets the PIC where the
 * reference's does not (its boot re-entry re-runs seabios, ours does not program
 * the PIC at all). */
void pic_reset(void);

/* ------------------------------------------------------------------ *
 * The six programming ports and the two ELCR ports -- pic.rs:361-385, promoted from
 * pic.c's statics to the module's interface by M2-1 so src/hw/io.c's bridge (the
 * replacement for the `io_port_*` definitions that used to sit at the bottom of pic.c)
 * can intercept them in front of its port map.  Nothing else may call them.
 * ------------------------------------------------------------------ */
uint32_t port20_read(void);
uint32_t port21_read(void);
uint32_t portA0_read(void);
uint32_t portA1_read(void);
uint32_t port4D0_read(void);
uint32_t port4D1_read(void);
void port20_write(uint8_t value);
void port21_write(uint8_t value);
void portA0_write(uint8_t value);
void portA1_write(uint8_t value);
void port4D0_write(uint8_t value);
void port4D1_write(uint8_t value);

#endif /* V86C_PIC_H */
