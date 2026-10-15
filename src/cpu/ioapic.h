/*
 * ioapic.h -- the IOAPIC: the indirect register block (IOREGSEL/IOWIN), the 24-entry
 * redirection table, and the device-side IRQ line interface.
 *
 * Port of v86/src/rust/cpu/ioapic.rs (316 lines); src/cpu/ for the same reason as
 * apic.c (upstream's ioapic.rs lives in the CPU crate, next to pic.rs).
 *
 * THE BOUNDS (unit APIC-1, the class the security audit named -- and the
 * redirection table is the case to check twice):
 *   - `ioregsel` is FULLY guest-controlled (a 32-bit write to the IOREGSEL window
 *     offset stores any value, ioapic.rs:254).  It is used to index
 *     `ioredtbl_config[24]` / `ioredtbl_destination[24]` ONLY inside the branch that
 *     tests `IOAPIC_FIRST_IRQ_REG <= ioregsel < IOAPIC_LAST_IRQ_REG`
 *     (0x10 <= sel < 0x40); there `irq = (sel - 0x10) >> 1` is in [0, 23] by the
 *     branch condition alone -- the bound is the branch, and the derivation is
 *     restated at both index sites.
 *   - `set_irq`/`clear_irq` take the IRQ number from the device layer; the
 *     `i >= IOAPIC_IRQ_COUNT` test returns BEFORE any store (ioapic.rs:145-148,
 *     176-179 -- the early return is not inside the dbg_assert, so it holds in the
 *     release build too).
 *   - everything else (the window offsets, the fixed registers 0/1/2) is a switch
 *     over constants.
 */

#ifndef V86C_CPU_IOAPIC_H
#define V86C_CPU_IOAPIC_H

#include <stdint.h>

#include "apic.h" /* apic_state_t -- the delivery path's parameter, as upstream's */

#define IOAPIC_IRQ_COUNT 24

#define IOAPIC_FIRST_IRQ_REG 0x10u
#define IOAPIC_LAST_IRQ_REG  (0x10u + 2u * IOAPIC_IRQ_COUNT) /* 0x40, exclusive */

/* ioapic.rs:18-24 */
#define IOAPIC_CONFIG_TRIGGER_MODE_LEVEL  (1u << 15)
#define IOAPIC_CONFIG_MASKED              (1u << 16)
#define IOAPIC_CONFIG_DELIVS              (1u << 12)
#define IOAPIC_CONFIG_REMOTE_IRR          (1u << 14)
#define IOAPIC_CONFIG_READONLY_MASK                                                       \
    (IOAPIC_CONFIG_REMOTE_IRR | IOAPIC_CONFIG_DELIVS | 0xFFFE0000u)

/* ioapic.rs:54-61 */
typedef struct ioapic_state {
    uint32_t ioredtbl_config[IOAPIC_IRQ_COUNT];
    uint32_t ioredtbl_destination[IOAPIC_IRQ_COUNT];
    uint32_t ioregsel;
    uint32_t ioapic_id;
    uint32_t irr;
    uint32_t irq_value;
} ioapic_state_t;

/* ioapic.rs:63-70: every redirection entry starts MASKED, id 0 ("must match value
 * in seabios"), no lines asserted. */
void ioapic_init(void);

/* ioapic.rs:77-91 -- the APIC's EOI path for level-triggered entries. */
void ioapic_remote_eoi(apic_state_t *apic, uint8_t vector);

/* ioapic.rs:142-191 -- the device layer's raise/lower (machine.c), upstream's
 * ioapic::set_irq / ioapic::clear_irq.  The ioapic_ prefix distinguishes them from
 * pic.c's set_irq/clear_irq, which upstream keeps in separate namespaces. */
void ioapic_set_irq(uint8_t i);
void ioapic_clear_irq(uint8_t i);

/* ioapic.rs:193-316, the mmio entry points memory.c routes to.  Gated on
 * cpu->acpi_enabled FIRST (ioapic.rs:194, 244), exactly like the APIC's. */
int32_t ioapic_read32(uint32_t addr);
void ioapic_write32(uint32_t addr, uint32_t value);

#endif /* V86C_CPU_IOAPIC_H */
