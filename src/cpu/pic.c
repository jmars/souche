/*
 * pic.c -- the 8259 PIC, the interrupt poll and the PIC's six programming ports (plus the
 * two ELCR ports).  See pic.h for what is real, what is stubbed and what a guest observes.
 *
 * M2-1 HANDOVER (HANDOFF=v86c): the `io_port_*` bridge that used to live at the bottom of
 * this file MOVED to src/hw/io.c, which is now the device layer.  This file keeps the six
 * ports' register accessors, declared in pic.h, and io.c intercepts them in front of its
 * port map -- upstream's own call order (cpu.rs:4613-4649).  The only bridge behaviour that
 * stayed behind is `handle_irqs` (below), which is the poll the PIC ports' writes trigger.
 *
 * TRANSCRIPTION RULES: `dbg_assert!` is `debug_assert!` (dbg.rs:63), compiled out of the
 * pinned release reference, so the asserts in pic.rs are DROPPED -- they are all "this
 * guest byte combination is not implemented" or "this cannot happen", and aborting on
 * reachable guest bytes is a divergence from the reference, not one of the port's stops.
 * `dbg_log!` has no channel in the port and is dropped, its text kept as a comment.
 *
 * DIVERGENCE (the one structural difference): upstream guards its PIC state with a Mutex
 * (`PIC: Mutex<Pic>`, pic.rs:47) because the JS half of v86 reads/writes the same state
 * from another thread.  The C port has no JS peer and one thread, so the state is a plain
 * file-static struct.  The LAYOUT is kept identical to pic.rs:14-40 (plus the dummy
 * byte) because the state image is part of the machine's observable state upstream; the
 * C port exposes no state-image accessor, so nothing reads it yet.
 */

#include "pic.h"

#include <stdio.h>

#include "cpu.h" /* the M1c/M2 stop (no longer used here; io.c owns it) */
#include "cpu_state.h"
#include "instructions.h" /* handle_irqs' declaration */
#include "paging.h"       /* call_interrupt_vector's declaration */
#include "string.h"       /* the in/out instructions' side of the bridge (declarations only) */
#include <string.h>       /* pic_reset's memset (REBOOT-1); the quoted string.h above is
                           * the port's own header, not libc's */

typedef struct pic0 {
    uint8_t irq_mask;
    uint8_t irq_map;
    uint8_t isr; /* in-service register: interrupts currently being serviced */
    uint8_t irr; /* interrupt request register: interrupts that have been requested */

    bool master;
    uint8_t dummy; /* upstream keeps this for the state image's layout */

    bool expect_icw4;
    uint8_t state;
    bool read_isr;
    bool auto_eoi;

    uint8_t elcr;

    uint8_t irq_value;
    bool special_mask_mode;
} pic0_t;

typedef struct pic {
    pic0_t master;
    pic0_t slave;
} pic_t;

/* pic.rs:47-92 -- the initial state, verbatim (`irq_map` 0 is upstream's documented
 * bogus default: both controllers mapped to 0 until the BIOS initializes them). */
static pic_t pic = {
    .master = { .irq_mask = 0, .irq_map = 0, .isr = 0, .irr = 0, .irq_value = 0,
                .expect_icw4 = false, .state = 0, .read_isr = false, .auto_eoi = false,
                .special_mask_mode = false, .elcr = 0, .master = true, .dummy = 0 },
    .slave = { .irq_mask = 0, .irq_map = 0, .isr = 0, .irr = 0, .irq_value = 0,
               .expect_icw4 = false, .state = 0, .read_isr = false, .auto_eoi = false,
               .special_mask_mode = false, .elcr = 0, .master = false, .dummy = 0 },
};

/* REBOOT-1: the same state again, for machine_reboot().  Upstream's
 * reboot_internal does NOT reset the PIC -- it does not need to, because its boot
 * re-entry re-runs SEABIOS, whose POST reprograms both controllers (ICWs, IMR) before
 * any guest code runs.  This port's firmware (tests/minibios.asm) re-runs too but
 * programs neither PIC nor PIT (it hands the PIC masked only at FIRST boot -- this
 * all-unmasked initial state -- and the kernel masks it itself, pm.c:36-42), so
 * boot 2 must start from the same PIC state boot 1 started from, not from boot 1's
 * own remapping (a stale remap + any line a raiser still drives delivers into the
 * firmware's IVT: a silent-wrong boot 2). */
void pic_reset(void)
{
    memset(&pic, 0, sizeof pic);
    pic.master.master = true;
    pic.slave.master = false;
}

/* pic.rs:136-137 -- `irq_mask.ilog2()` with the `dbg_assert!(irq_mask != 0)` that
 * precedes it upstream.  Written as a loop rather than __builtin_ctz so the port does not
 * depend on a compiler builtin for a value it can compute portably. */
static uint8_t lowest_bit_index(uint8_t x)
{
    uint8_t i = 0;
    while (((x >> i) & 1) == 0) {
        i++;
    }
    return i;
}

/* pic.rs:102-144 -- the priority rule: the highest-priority unmasked request, unless a
 * higher-or-equal-priority interrupt is already in service. */
static bool pic0_get_irq(pic0_t *self, uint8_t *irq_out)
{
    uint8_t enabled_irr = (uint8_t)(self->irr & self->irq_mask);

    if (enabled_irr == 0) {
        return false;
    }

    uint8_t negated = (uint8_t)(-enabled_irr);
    uint8_t irq_mask = (uint8_t)(enabled_irr & negated);
    uint8_t special_mask = self->special_mask_mode ? self->irq_mask : 0xFF;

    if (self->isr != 0 && ((uint8_t)(self->isr & (uint8_t)(-self->isr)) & special_mask) <= irq_mask) {
        /* wait for eoi of higher or same priority interrupt */
        return false;
    }

    DBG_ASSERT(irq_mask != 0);
    uint8_t irq_number = lowest_bit_index(irq_mask);
    DBG_ASSERT(irq_mask == 1 << irq_number);

    *irq_out = irq_number;
    return true;
}

static uint32_t pic0_port0_read(const pic0_t *self)
{
    return (uint32_t)(self->read_isr ? self->isr : self->irr);
}

static uint32_t pic0_port1_read(const pic0_t *self)
{
    return (uint32_t)(uint8_t)~self->irq_mask; /* `!mask as u32`: an 8-bit NOT */
}

static void pic_check_irqs_slave(pic_t *self);

static void pic_set_irq(pic_t *self, uint8_t i)
{
    uint8_t mask = (uint8_t)(1 << (i & 7));
    pic0_t *dev = i < 8 ? &self->master : &self->slave;
    if ((dev->irq_value & mask) == 0 || (dev->elcr & mask) != 0) {
        dev->irr |= mask;
        dev->irq_value |= mask;
        if (i >= 8) {
            pic_check_irqs_slave(self);
        }
    }
}

static void pic_clear_irq(pic_t *self, uint8_t i)
{
    uint8_t mask = (uint8_t)(1 << (i & 7));
    pic0_t *dev = i < 8 ? &self->master : &self->slave;
    dev->irq_value &= (uint8_t)~mask;
    if ((dev->elcr & mask) != 0) {
        dev->irr &= (uint8_t)~mask;
        if (i >= 8) {
            pic_check_irqs_slave(self);
        }
    }
}

/* pic.rs:282-290 -- the slave's request is forwarded to the master as IRQ 2. */
static void pic_check_irqs_slave(pic_t *self)
{
    uint8_t ignored;
    if (pic0_get_irq(&self->slave, &ignored)) {
        pic_set_irq(self, 2);
    }
    else {
        pic_clear_irq(self, 2);
    }
}

/* pic.rs:175-243 -- the command port (0x20/0xA0).  `index` is 0 for the master. */
static void pic_port0_write(pic_t *self, uint8_t index, uint8_t v)
{
    pic0_t *dev = index == 0 ? &self->master : &self->slave;
    if ((v & 0x10) != 0) {
        /* xxxx1xxx -- icw1 */
        dev->isr = 0;
        dev->irr = 0;
        dev->irq_mask = 0xff;
        dev->irq_value = 0;
        dev->auto_eoi = true;

        dev->expect_icw4 = (v & 1) != 0;
        /* upstream dbg_assert!(v & 2 == 0, "unimplemented: single mode") dropped */
        /* upstream dbg_assert!(v & 8 == 0, "unimplemented: level mode") dropped */
        dev->state = 1;
    }
    else if ((v & 8) != 0) {
        /* xxx01xxx -- ocw3 */
        if ((v & 2) != 0) {
            dev->read_isr = (v & 1) != 0;
        }
        if ((v & 4) != 0) {
            /* upstream dbg_assert!(false, "unimplemented: polling") dropped */
        }
        if ((v & 0x40) != 0) {
            dev->special_mask_mode = (v & 0x20) == 0x20;
        }
    }
    else {
        /* xxx00xxx -- ocw2, end of interrupt */
        uint8_t eoi_type = (uint8_t)(v >> 5);

        if (eoi_type == 1) {
            /* non-specific eoi.  Upstream's `dev.isr - 1` is a WRAPPING u8 subtraction
             * (release Rust wraps; a debug build would panic), reproduced here. */
            dev->isr = (uint8_t)(dev->isr & (uint8_t)(dev->isr - 1));
        }
        else if (eoi_type == 3) {
            /* specific eoi */
            dev->isr &= (uint8_t)~(1 << (v & 7));
        }
        else if (eoi_type == 6) {
            /* os2 v4, freebsd -- `lowest priority` is a no-op upstream */
        }
        else {
            /* unknown eoi */
            /* upstream dbg_assert!(false) dropped */
            dev->isr = (uint8_t)(dev->isr & (uint8_t)(dev->isr - 1));
        }

        if (index == 1) {
            pic_check_irqs_slave(self);
        }
    }
}

/* pic.rs:245-280 -- the data port (0x21/0xA1). */
static void pic_port1_write(pic_t *self, uint8_t index, uint8_t v)
{
    pic0_t *dev = index == 0 ? &self->master : &self->slave;
    if (dev->state == 0) {
        if (dev->expect_icw4) {
            /* icw4 */
            dev->expect_icw4 = false;
            dev->auto_eoi = (v & 2) != 0;
            /* upstream dbg_assert!(v & 0x10 == 0, "unimplemented: nested mode") dropped */
            /* upstream dbg_assert!(v & 1 == 1, "unimplemented: 8086/88 mode") dropped */
        }
        else {
            /* ocw1 */
            dev->irq_mask = (uint8_t)~v;

            if (index == 1) {
                pic_check_irqs_slave(self);
            }
        }
    }
    else if (dev->state == 1) {
        /* icw2 */
        dev->irq_map = v;
        dev->state += 1;
    }
    else if (dev->state == 2) {
        /* icw3 */
        dev->state = 0;
    }
}

/* ------------------------------------------------------------------ *
 * pic.rs:294-331 + 333-365 -- the acknowledge
 * ------------------------------------------------------------------ */
static int32_t acknowledge_irq_slave(void)
{
    uint8_t irq;
    if (!pic0_get_irq(&pic.slave, &irq)) {
        return -1;
    }

    if (pic.slave.irr == 0) {
        /* upstream dbg_assert!(false) then None */
        return -1;
    }

    uint8_t mask = (uint8_t)(1 << irq);

    if ((pic.slave.elcr & mask) == 0) {
        /* not in level mode */
        pic.slave.irr &= (uint8_t)~mask;
    }

    if (!pic.slave.auto_eoi) {
        pic.slave.isr |= mask;
    }

    {
        uint8_t ignored;
        DBG_ASSERT(!pic0_get_irq(&pic.slave, &ignored));
    }
    pic_clear_irq(&pic, 2);

    return (int32_t)pic.slave.irq_map | irq;
}

int32_t pic_acknowledge_irq(void)
{
    uint8_t irq;
    if (!pic0_get_irq(&pic.master, &irq)) {
        return -1;
    }

    if (pic.master.irr == 0) {
        /* upstream dbg_assert!(false) then None */
        return -1;
    }

    uint8_t mask = (uint8_t)(1 << irq);

    if ((pic.master.elcr & mask) == 0) {
        /* not in level mode */
        pic.master.irr &= (uint8_t)~mask;
    }

    if (!pic.master.auto_eoi) {
        pic.master.isr |= mask;
    }

    {
        uint8_t ignored;
        DBG_ASSERT(!pic0_get_irq(&pic.master, &ignored));
    }

    if (irq == 2) {
        return acknowledge_irq_slave();
    }
    return (int32_t)pic.master.irq_map | irq;
}

/* ------------------------------------------------------------------ *
 * pic.rs:367-402 -- the device interface and the ports
 * ------------------------------------------------------------------ */
void set_irq(uint8_t i)
{
    DBG_ASSERT(i < 16);
    pic_set_irq(&pic, i);
}

void clear_irq(uint8_t i)
{
    DBG_ASSERT(i < 16);
    pic_clear_irq(&pic, i);
}

uint32_t port20_read(void) { return pic0_port0_read(&pic.master); }
uint32_t port21_read(void) { return pic0_port1_read(&pic.master); }
uint32_t portA0_read(void) { return pic0_port0_read(&pic.slave); }
uint32_t portA1_read(void) { return pic0_port1_read(&pic.slave); }

void port20_write(uint8_t v) { pic_port0_write(&pic, 0, v); }
void port21_write(uint8_t v) { pic_port1_write(&pic, 0, v); }

void portA0_write(uint8_t v) { pic_port0_write(&pic, 1, v); }
void portA1_write(uint8_t v) { pic_port1_write(&pic, 1, v); }

uint32_t port4D0_read(void) { return pic.master.elcr; }
uint32_t port4D1_read(void) { return pic.slave.elcr; }
void port4D0_write(uint8_t v) { pic.master.elcr = v; }
void port4D1_write(uint8_t v) { pic.slave.elcr = v; }

/* ------------------------------------------------------------------ *
 * cpu.rs:4573-4594 -- the poll.  `handle_irqs` is instructions.h's declaration (POPF/STI
 * and HLT are its callers); hlt handling is pic_call_irq's `*in_hlt = false`.
 * ------------------------------------------------------------------ */
static void pic_call_irq(uint8_t interrupt_nr)
{
    /* XXX: What if called after instruction (port IO) */
    cpu->previous_ip = cpu->instruction_pointer;
    if (cpu->in_hlt) {
        /* upstream: js::stop_idling() -- the JS half's "wake the worker".  The C port has
         * no JS peer and no device layer to wake, so the only state that means anything is
         * `in_hlt`, cleared here exactly as upstream clears it.  cpu_event_halt (the
         * JS-facing counterpart, called BY hlt) is NOT called on the way out. */
        cpu->in_hlt = false;
    }
    call_interrupt_vector((int32_t)interrupt_nr, false, false, 0);
}

void handle_irqs(void)
{
    if ((cpu->flags & FLAG_INTERRUPT) != 0) {
        int32_t irq = pic_acknowledge_irq();
        if (irq >= 0) {
            pic_call_irq((uint8_t)irq);
        }
        else if (cpu->acpi_enabled) {
            /* apic.rs (M2): acknowledge_irq.  acpi_enabled is false for every machine
             * this unit can run -- the loader never sets it -- so the arm is unreachable
             * until M2 lands the APIC. */
        }
    }
}

