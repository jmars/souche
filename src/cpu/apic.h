/*
 * apic.h -- the LOCAL APIC: the 4 KiB register block, its timer and the delivery
 * path (irr/isr/tmr, the LVT entries, EOI, acknowledge_irq).
 *
 * Port of v86/src/rust/cpu/apic.rs (658 lines).  It sits in src/cpu/ next to pic.c
 * for the same reason that file does: upstream's apic.rs is rust/cpu/apic.rs, and
 * the one-C-file-per-upstream-file mapping (m2-1's rule) is per upstream DIRECTORY
 * here -- apic.rs lives in the CPU crate, not with the JS devices in src/hw/.
 *
 * THE ENABLE FLAGS, kept exactly as upstream has them (a guest sees the pair):
 *   - `cpu->acpi_enabled` (upstream global_pointers' acpi_enabled, cpu.js:1014) is
 *     the MACHINE knob: it gates every apic/ioapic read32/write32 (apic.rs:112,
 *     279; ioapic.rs:194, 244), the CPUID APIC bit, the delivery paths and the
 *     timer fan-out.  In the reference it is set once from settings.acpi at CPU
 *     construction and NEVER reset (reset_cpu does not touch offset 552); this
 *     port's writer is the boot-mode `--acpi` CLI flag (src/main.c), DEFAULT OFF.
 *   - `cpu->apic_enabled` (offset 548) is the CPU knob, written only by WRMSR
 *     IA32_APIC_BASE (instructions_0f.c) and read back by RDMSR.  Nothing in the
 *     device reads it.
 *
 * THE CLOCK (unit APIC-1's decision 2): apic_timer consumes the SAME deterministic
 * virtual clock as the PIT and the PM timer -- exec_boundary.c's D-F2 microtick()
 * (milliseconds; one guest instruction = one microsecond), and APIC_TIMER_FREQ is
 * 1e6 counts per millisecond, i.e. one count per guest instruction per the
 * undivided timer.  No host time is read anywhere in this file.
 */

#ifndef V86C_CPU_APIC_H
#define V86C_CPU_APIC_H

#include <stdbool.h>
#include <stdint.h>

/* apic.rs:9 -- "should probably be kept in sync with TSC_RATE in cpu.rs" */
#define APIC_TIMER_FREQ 1.0e6

/* apic.rs:31, via ioapic::IOAPIC_CONFIG_MASKED (ioapic.rs:20) */
#define APIC_CONFIG_MASKED (1u << 16)

/* apic.rs:11-16 */
#define APIC_TIMER_MODE_MASK     (3u << 17)
#define APIC_TIMER_MODE_ONE_SHOT 0u
#define APIC_TIMER_MODE_PERIODIC (1u << 17)

/* The delivery modes apic.rs names (ioapic.rs:26-29 carries the same list). */
#define APIC_DELIVERY_FIXED          0
#define APIC_DELIVERY_LOWEST_PRIORITY 1
#define APIC_DELIVERY_NMI            4
#define APIC_DELIVERY_INIT           5

/*
 * The register block, apic.rs:53-104.  Layout named because the register offsets
 * below are the struct's own upstream offsets (the JS state serialiser depends on
 * them); this port keeps the struct as state, not as a mapped image.
 */
typedef struct apic_state {
    uint32_t apic_id;
    uint32_t timer_divider;
    uint32_t timer_divider_shift;
    uint32_t timer_initial_count;
    uint32_t timer_current_count;
    double timer_last_tick;
    uint32_t lvt_timer;
    uint32_t lvt_perf_counter;
    uint32_t lvt_int0;
    uint32_t lvt_int1;
    uint32_t lvt_error;
    uint32_t tpr;
    uint32_t icr0;
    uint32_t icr1;
    uint32_t irr[8];
    uint32_t isr[8];
    uint32_t tmr[8];
    uint32_t spurious_vector;
    uint32_t destination_format;
    uint32_t local_destination;
    uint32_t error;
    uint32_t read_error;
    uint32_t lvt_thermal_sensor;
} apic_state_t;

/* apic.rs:80-104's static initialiser: every LVT entry starts MASKED, the
 * spurious vector 0xFE, the destination format all ones, the divider shift 1. */
void apic_init(void);

/* apic.rs:106 -- the state's owner (upstream's get_apic()); ioapic.c's delivery
 * path takes it as a parameter, mirroring upstream's &mut Apic arguments. */
apic_state_t *apic_get_apic(void);

/* apic.rs:111-276 / 278-464, the mmio entry points memory.c routes to.  Both are
 * gated on cpu->acpi_enabled FIRST (apic.rs:112, 279): with the machine knob off
 * a read answers 0 and a write is discarded -- the reference's own behaviour with
 * settings.acpi unset. */
int32_t apic_read32(uint32_t addr);
void apic_write32(uint32_t addr, uint32_t value);

/* apic.rs:466-533 -- the timer, run_hardware_timers' fourth value. */
double apic_timer(double now);

/* apic.rs:535-545.  ioapic.c's check_irq is the other caller (as upstream's
 * ioapic.rs:125 calls apic::route). */
void apic_route(apic_state_t *apic, uint8_t vector, uint8_t mode, bool is_level,
                uint8_t destination, uint8_t destination_mode);

/* apic.rs:599-637, the PIC-side ack (cpu.rs:4579).  Returns -1 for upstream's
 * None -- pic.c's handle_irqs already speaks that convention. */
int32_t apic_acknowledge_irq(void);

#endif /* V86C_CPU_APIC_H */
