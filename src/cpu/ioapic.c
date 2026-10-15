/*
 * ioapic.c -- the IOAPIC.  Port of v86/src/rust/cpu/ioapic.rs (316 lines); see ioapic.h
 * for the bounds of the guest-controlled `ioregsel` index and the set_irq/clear_irq
 * early returns.  The upstream file is "http://download.intel.com/design/chipsets/
 * datashts/29056601.pdf"; every dropped check is an upstream dbg_assert! named in a
 * comment (dbg.rs:63-67 compiles those out of the pinned release -- the G4-S2 policy).
 *
 * On this single-CPU machine every delivered entry routes to the one local APIC
 * (ioapic.rs:125's apic::route call); the destination fields are carried but, as
 * upstream's route TODO says, not yet acted on.
 */

#include "ioapic.h"

#include <string.h>

#include "apic.h"
#include "cpu_state.h" /* cpu->acpi_enabled */

/* ioapic.rs:8-9 */
#define IOREGSEL 0u
#define IOWIN    0x10u

/* ioapic.rs:16 */
#define IOAPIC_ID 0

static ioapic_state_t ioapic_instance;

void ioapic_init(void)
{
    memset(&ioapic_instance, 0, sizeof(ioapic_instance));

    for (int i = 0; i < IOAPIC_IRQ_COUNT; i++) {
        ioapic_instance.ioredtbl_config[i] = IOAPIC_CONFIG_MASKED;
    }
    ioapic_instance.ioapic_id = IOAPIC_ID;
}

/* ------------------------------------------------------------------ *
 * the line -> vector path -- ioapic.rs:93-140
 * ------------------------------------------------------------------ */

static void check_irq(ioapic_state_t *ioapic, apic_state_t *apic, uint8_t irq)
{
    uint32_t mask = 1u << irq;

    if ((ioapic->irr & mask) == 0) {
        return;
    }

    /* irq is in [0, IOAPIC_IRQ_COUNT) at every call site (set_irq/clear_irq's own
     * bound and remote_eoi's fixed loop), so this index is in range. */
    uint32_t config = ioapic->ioredtbl_config[irq];

    if ((config & IOAPIC_CONFIG_MASKED) == 0) {
        uint8_t delivery_mode = (uint8_t)((config >> 8) & 7);
        uint8_t destination_mode = (uint8_t)((config >> 11) & 1);
        uint8_t vector = (uint8_t)(config & 0xFF);
        uint8_t destination = (uint8_t)(ioapic->ioredtbl_destination[irq] >> 24);
        bool is_level =
            (config & IOAPIC_CONFIG_TRIGGER_MODE_LEVEL) == IOAPIC_CONFIG_TRIGGER_MODE_LEVEL;

        if ((config & IOAPIC_CONFIG_TRIGGER_MODE_LEVEL) == 0) {
            ioapic->irr &= ~mask;
        }
        else {
            ioapic->ioredtbl_config[irq] |= IOAPIC_CONFIG_REMOTE_IRR;

            if ((config & IOAPIC_CONFIG_REMOTE_IRR) != 0) {
                /* upstream: dbg_log!("No route: level interrupt and remote IRR still set")
                 * -- the config was just OR'd above, so this arm fires whenever the entry
                 * ALREADY had remote IRR set.  Upstream's log is debug-only; the return
                 * is the behaviour. */
                return;
            }
        }

        if (delivery_mode == 0 /* IOAPIC_DELIVERY_FIXED */ ||
            delivery_mode == 1 /* IOAPIC_DELIVERY_LOWEST_PRIORITY */) {
            apic_route(apic, vector, delivery_mode, is_level, destination, destination_mode);
        }
        else {
            /* upstream: dbg_assert!(false, "TODO") -- dropped; the release delivers
             * nothing for the other modes (no route, no fault). */
        }

        ioapic->ioredtbl_config[irq] &= ~IOAPIC_CONFIG_DELIVS;
    }
}

void ioapic_remote_eoi(apic_state_t *apic, uint8_t vector)
{
    for (uint8_t i = 0; i < IOAPIC_IRQ_COUNT; i++) {
        uint32_t config = ioapic_instance.ioredtbl_config[i];

        if ((uint8_t)(config & 0xFF) == vector &&
            (config & IOAPIC_CONFIG_REMOTE_IRR) != 0) {
            ioapic_instance.ioredtbl_config[i] &= ~IOAPIC_CONFIG_REMOTE_IRR;
            check_irq(&ioapic_instance, apic, i);
        }
    }
}

void ioapic_set_irq(uint8_t i)
{
    if (i >= IOAPIC_IRQ_COUNT) {
        /* upstream: dbg_assert!(false, "Bad irq: {}", i) then return -- the RETURN is
         * unconditional (ioapic.rs:145-148), so the release keeps the bound. */
        return;
    }

    apic_state_t *apic = apic_get_apic();
    uint32_t mask = 1u << i;

    if ((ioapic_instance.irq_value & mask) == 0) {
        ioapic_instance.irq_value |= mask;

        uint32_t config = ioapic_instance.ioredtbl_config[i];
        if ((config & (IOAPIC_CONFIG_TRIGGER_MODE_LEVEL | IOAPIC_CONFIG_MASKED)) ==
            IOAPIC_CONFIG_MASKED) {
            /* edge triggered and masked */
            return;
        }

        ioapic_instance.irr |= mask;

        check_irq(&ioapic_instance, apic, i);
    }
}

void ioapic_clear_irq(uint8_t i)
{
    if (i >= IOAPIC_IRQ_COUNT) {
        /* upstream: dbg_assert!(false, "Bad irq: {}", i) then return -- as set_irq. */
        return;
    }

    uint32_t mask = 1u << i;

    if ((ioapic_instance.irq_value & mask) == mask) {
        ioapic_instance.irq_value &= ~mask;

        uint32_t config = ioapic_instance.ioredtbl_config[i];
        if ((config & IOAPIC_CONFIG_TRIGGER_MODE_LEVEL) != 0) {
            ioapic_instance.irr &= ~mask;
        }
    }
}

/* ------------------------------------------------------------------ *
 * the register block -- ioapic.rs:200-316
 * ------------------------------------------------------------------ */

static int32_t ioapic_read32_internal(ioapic_state_t *ioapic, uint32_t addr)
{
    switch (addr) {
        case IOREGSEL:
            return (int32_t)ioapic->ioregsel;

        case IOWIN:
            switch (ioapic->ioregsel) {
                case 0:
                    return (int32_t)(ioapic->ioapic_id << 24);

                case 1:
                    /* version */
                    return (int32_t)(0x11u | (uint32_t)(IOAPIC_IRQ_COUNT - 1) << 16);

                case 2:
                    /* arbitration id */
                    return (int32_t)(ioapic->ioapic_id << 24);

                default:
                    if (ioapic->ioregsel >= IOAPIC_FIRST_IRQ_REG &&
                        ioapic->ioregsel < IOAPIC_LAST_IRQ_REG) {
                        /* THE BOUND (ioapic.h): the branch test above makes
                         * sel - 0x10 <= 0x2F, so irq <= 23 and both indices below are
                         * in range for ioredtbl_*[IOAPIC_IRQ_COUNT]. */
                        uint8_t irq = (uint8_t)((ioapic->ioregsel - IOAPIC_FIRST_IRQ_REG) >> 1);
                        uint32_t index = ioapic->ioregsel & 1;

                        if (index != 0) {
                            return (int32_t)ioapic->ioredtbl_destination[irq];
                        }
                        else {
                            return (int32_t)ioapic->ioredtbl_config[irq];
                        }
                    }
                    /* upstream: dbg_assert!(false, "IOAPIC register read outside of range
                     * {:x}", reg) -- dropped; the release reads 0. */
                    return 0;
            }
            break;

        default:
            /* upstream: dbg_assert!(false, "Unaligned or oob IOAPIC memory read: {:x}",
             * addr) -- dropped; the release reads 0. */
            return 0;
    }
}

int32_t ioapic_read32(uint32_t addr)
{
    if (!cpu->acpi_enabled) {
        return 0;
    }
    return ioapic_read32_internal(&ioapic_instance, addr);
}

static void ioapic_write32_internal(ioapic_state_t *ioapic, apic_state_t *apic, uint32_t addr,
                                    uint32_t value)
{
    switch (addr) {
        case IOREGSEL:
            ioapic->ioregsel = value;
            break;

        case IOWIN:
            switch (ioapic->ioregsel) {
                case 0:
                    ioapic->ioapic_id = (value >> 24) & 0x0F;
                    break;

                case 1:
                case 2:
                    /* upstream: dbg_log!("IOAPIC Invalid write: {}", ioapic.ioregsel) --
                     * a logged no-op in the release. */
                    break;

                default:
                    if (ioapic->ioregsel >= IOAPIC_FIRST_IRQ_REG &&
                        ioapic->ioregsel < IOAPIC_LAST_IRQ_REG) {
                        /* THE BOUND, as the read arm above: irq <= 23 here. */
                        uint8_t irq = (uint8_t)((ioapic->ioregsel - IOAPIC_FIRST_IRQ_REG) >> 1);
                        uint32_t index = ioapic->ioregsel & 1;

                        if (index != 0) {
                            ioapic->ioredtbl_destination[irq] = value & 0xFF000000u;
                        }
                        else {
                            uint32_t old_value = ioapic->ioredtbl_config[irq];
                            ioapic->ioredtbl_config[irq] =
                                (value & ~IOAPIC_CONFIG_READONLY_MASK) |
                                (old_value & IOAPIC_CONFIG_READONLY_MASK);

                            check_irq(ioapic, apic, irq);
                        }
                    }
                    /* upstream: dbg_assert!(false, "IOAPIC register write outside of range
                     * {:x} <- {:x}", reg, value) -- dropped; the release discards. */
                    break;
            }
            break;

        default:
            /* upstream: dbg_assert!(false, "Unaligned or oob IOAPIC memory write: {:x}
             * <- {:x}", addr, value) -- dropped; the release discards. */
            break;
    }
}

void ioapic_write32(uint32_t addr, uint32_t value)
{
    if (!cpu->acpi_enabled) {
        return;
    }
    ioapic_write32_internal(&ioapic_instance, apic_get_apic(), addr, value);
}
