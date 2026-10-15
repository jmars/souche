/*
 * apic.c -- the LOCAL APIC.  Port of v86/src/rust/cpu/apic.rs (658 lines); see apic.h
 * for the enable-flag pair and the clock.  apic.rs is "See Intel's System Programming
 * Guide"; the structure follows the upstream file function for function, and every
 * dropped check is an upstream dbg_assert! named in a comment (dbg.rs:63-67 compiles
 * those out of the pinned release -- the G4-S2 policy, kept at each site).
 *
 * THE BOUNDS (unit APIC-1, the guest-controlled-index class): every register offset
 * arrives from the router at memory.c already masked to `& ~3` inside
 * [APIC_MEM_ADDRESS, +APIC_MEM_SIZE); the register dispatch is a switch over
 * CONSTANTS, so the only guest-derived indices are the irr/isr/tmr word numbers --
 * each computed from one of eight switch constants (`(addr - 0x100) >> 4` et al.) and
 * therefore in [0, 7] by construction, with the derivation stated at the arm.  The
 * timer's divider shift is masked arithmetic bounded by 7 (see the 0x3E0 arm); the
 * delivery bit-index is a u8 vector, so `>> 5` lands in [0, 7] for every value the
 * type can hold.  There is no guest-controlled index into any C array that is not
 * bounded by the branch that computes it.
 */

#include "apic.h"

#include <string.h>

#include "cpu_state.h"     /* cpu->acpi_enabled */
#include "instructions.h"  /* microtick */
#include "ioapic.h"        /* IOAPIC_CONFIG_* and remote_eoi (apic.rs:3) */

static apic_state_t apic_instance;

/* apic.rs:547-579.  Used by the write path and the timer before its definition. */
static void apic_deliver(apic_state_t *apic, uint8_t vector, uint8_t mode, bool is_level);

/* apic.rs:80-104 -- the static's initial values.  Upstream's Mutex unwraps to the
 * same single instance for the one-CPU machine this port is. */
void apic_init(void)
{
    memset(&apic_instance, 0, sizeof(apic_instance));

    apic_instance.timer_divider_shift = 1;
    apic_instance.lvt_timer = APIC_CONFIG_MASKED;
    apic_instance.lvt_thermal_sensor = APIC_CONFIG_MASKED;
    apic_instance.lvt_perf_counter = APIC_CONFIG_MASKED;
    apic_instance.lvt_int0 = APIC_CONFIG_MASKED;
    apic_instance.lvt_int1 = APIC_CONFIG_MASKED;
    apic_instance.lvt_error = APIC_CONFIG_MASKED;
    apic_instance.spurious_vector = 0xFE;
    apic_instance.destination_format = UINT32_MAX;
}

apic_state_t *apic_get_apic(void) { return &apic_instance; }

/* ------------------------------------------------------------------ *
 * the 256-bit register helpers -- apic.rs:639-658
 * ------------------------------------------------------------------ */

static bool register_get_bit(const uint32_t v[8], uint8_t bit)
{
    return (v[bit >> 5] & (1u << (bit & 31))) != 0;
}

static void register_set_bit(uint32_t v[8], uint8_t bit) { v[bit >> 5] |= 1u << (bit & 31); }

static void register_clear_bit(uint32_t v[8], uint8_t bit) { v[bit >> 5] &= ~(1u << (bit & 31)); }

/* apic.rs:646-658 -- transmutes [u32; 8] to [u64; 4] and takes word.ilog2(); the
 * C form spells the little-endian join out.  No highest bit -> -1 (upstream None). */
static int32_t register_get_highest_bit(const uint32_t v[8])
{
    for (int i = 3; i >= 0; i--) {
        uint64_t word = (uint64_t)v[2 * i + 1] << 32 | v[2 * i];
        if (word != 0) {
            return (int32_t)(63u - (unsigned)__builtin_clzll(word)) | (i << 6);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * reads -- apic.rs:118-276
 * ------------------------------------------------------------------ */

static int32_t apic_read32_internal(apic_state_t *apic, uint32_t addr)
{
    switch (addr) {
        case 0x20:
            return (int32_t)apic->apic_id;

        case 0x30:
            /* version */
            return 0x50014;

        case 0x80:
            return (int32_t)apic->tpr;

        case 0xB0:
            /* write-only (written by DSL) */
            return 0;

        case 0xD0:
            return (int32_t)apic->local_destination;

        case 0xE0:
            return (int32_t)apic->destination_format;

        case 0xF0:
            return (int32_t)apic->spurious_vector;

        case 0x100: case 0x110: case 0x120: case 0x130:
        case 0x140: case 0x150: case 0x160: case 0x170: {
            /* the switch constants are 0x100 + 0x10 * index, so index is in [0, 7] */
            uint32_t index = (addr - 0x100) >> 4;
            return (int32_t)apic->isr[index];
        }

        case 0x180: case 0x190: case 0x1A0: case 0x1B0:
        case 0x1C0: case 0x1D0: case 0x1E0: case 0x1F0: {
            uint32_t index = (addr - 0x180) >> 4;
            return (int32_t)apic->tmr[index];
        }

        case 0x200: case 0x210: case 0x220: case 0x230:
        case 0x240: case 0x250: case 0x260: case 0x270: {
            uint32_t index = (addr - 0x200) >> 4;
            return (int32_t)apic->irr[index];
        }

        case 0x280:
            return (int32_t)apic->read_error;

        case 0x300:
            return (int32_t)apic->icr0;

        case 0x310:
            return (int32_t)apic->icr1;

        case 0x320:
            return (int32_t)apic->lvt_timer;

        case 0x330:
            return (int32_t)apic->lvt_thermal_sensor;

        case 0x340:
            return (int32_t)apic->lvt_perf_counter;

        case 0x350:
            return (int32_t)apic->lvt_int0;

        case 0x360:
            return (int32_t)apic->lvt_int1;

        case 0x370:
            return (int32_t)apic->lvt_error;

        case 0x3E0:
            /* divider */
            return (int32_t)apic->timer_divider;

        case 0x380:
            return (int32_t)apic->timer_initial_count;

        case 0x390: {
            double now = microtick();
            if (apic->timer_last_tick > now) {
                /* should only happen after restore_state -- or, on this clock, after
                 * a guest reboot (reset_cpu zeroes the instruction counter the clock
                 * is derived from).  Upstream logs a warning and rewinds. */
                apic->timer_last_tick = now;
            }
            double diff = now - apic->timer_last_tick;
            double diff_in_ticks_d =
                diff * APIC_TIMER_FREQ / (double)(1u << apic->timer_divider_shift);
            uint64_t diff_in_ticks = (uint64_t)diff_in_ticks_d;
            if (diff_in_ticks < (uint64_t)apic->timer_initial_count) {
                return (int32_t)(apic->timer_initial_count - (uint32_t)diff_in_ticks);
            }
            else {
                uint32_t mode = apic->lvt_timer & APIC_TIMER_MODE_MASK;
                if (mode == APIC_TIMER_MODE_PERIODIC) {
                    return (int32_t)(apic->timer_initial_count -
                                     (uint32_t)(diff_in_ticks %
                                                ((uint64_t)apic->timer_initial_count + 1)));
                }
                else if (mode == APIC_TIMER_MODE_ONE_SHOT) {
                    return 0;
                }
                else {
                    /* upstream: dbg_assert!(false, "apic unimplemented timer mode: {:x}",
                     * mode) -- dropped at the site; the release falls through to 0. */
                    return 0;
                }
            }
        }

        default:
            /* upstream: dbg_log!("APIC read {:x}", addr); dbg_assert!(false); -- the
             * release reads 0. */
            return 0;
    }
}

int32_t apic_read32(uint32_t addr)
{
    if (!cpu->acpi_enabled) {
        return 0;
    }
    return apic_read32_internal(apic_get_apic(), addr);
}

/* ------------------------------------------------------------------ *
 * writes -- apic.rs:285-464
 * ------------------------------------------------------------------ */

static void apic_write32_internal(apic_state_t *apic, uint32_t addr, uint32_t value)
{
    switch (addr) {
        case 0x20:
            apic->apic_id = value;
            break;

        case 0x30:
            /* version -- ignored */
            break;

        case 0x80:
            apic->tpr = value & 0xFF;
            break;

        case 0xB0: {
            int32_t highest_isr = register_get_highest_bit(apic->isr);
            if (highest_isr >= 0) {
                register_clear_bit(apic->isr, (uint8_t)highest_isr);
                if (register_get_bit(apic->tmr, (uint8_t)highest_isr)) {
                    /* Send eoi to all IO APICs */
                    ioapic_remote_eoi(apic, (uint8_t)highest_isr);
                }
            }
            else {
                /* upstream: dbg_log!("Bad eoi: No isr set") */
            }
        }
            break;

        case 0xD0:
            apic->local_destination = value & 0xFF000000u;
            break;

        case 0xE0:
            apic->destination_format = value | 0xFFFFFFu;
            break;

        case 0xF0:
            apic->spurious_vector = value;
            break;

        case 0x280:
            /* updated readable error register with real error */
            apic->read_error = apic->error;
            apic->error = 0;
            break;

        case 0x300: {
            uint8_t vector = (uint8_t)(value & 0xFF);
            uint8_t delivery_mode = (uint8_t)((value >> 8) & 7);
            uint8_t destination_mode = (uint8_t)((value >> 11) & 1);
            bool is_level = (value & IOAPIC_CONFIG_TRIGGER_MODE_LEVEL) ==
                            IOAPIC_CONFIG_TRIGGER_MODE_LEVEL;
            uint32_t destination_shorthand = (value >> 18) & 3;
            uint8_t destination = (uint8_t)(apic->icr1 >> 24);

            value &= ~(1u << 12);
            apic->icr0 = value;

            if (destination_shorthand == 0) {
                /* no shorthand */
                apic_route(apic, vector, delivery_mode, is_level, destination,
                           destination_mode);
            }
            else if (destination_shorthand == 1) {
                /* self */
                apic_deliver(apic, vector, APIC_DELIVERY_FIXED, is_level);
            }
            else if (destination_shorthand == 2) {
                /* all including self */
                apic_deliver(apic, vector, delivery_mode, is_level);
            }
            else if (destination_shorthand == 3) {
                /* all but self */
            }
            else {
                /* upstream: dbg_assert!(false) -- unreachable for a 2-bit field */
            }
        }
            break;

        case 0x310:
            apic->icr1 = value;
            break;

        case 0x320:
            /* TODO (upstream): check if unmasking and if this should trigger an
             * interrupt immediately */
            apic->lvt_timer = value;
            break;

        case 0x330:
            apic->lvt_thermal_sensor = value;
            break;

        case 0x340:
            apic->lvt_perf_counter = value;
            break;

        case 0x350:
            apic->lvt_int0 = value;
            break;

        case 0x360:
            apic->lvt_int1 = value;
            break;

        case 0x370:
            apic->lvt_error = value;
            break;

        case 0x3E0: {
            apic->timer_divider = value;

            uint32_t divide_shift = (value & 0b11) | ((value & 0b1000) >> 1);
            /* the masked field is in [0, 0b111] by construction (two 3-bit-or-narrower
             * pieces), so the shift below is in [0, 7] and 1u << shift never overflows */
            apic->timer_divider_shift = divide_shift == 0b111 ? 0 : divide_shift + 1;
        }
            break;

        case 0x380:
            apic->timer_initial_count = value;
            apic->timer_current_count = value;
            apic->timer_last_tick = microtick();
            break;

        case 0x390:
            /* upstream: dbg_assert!(false, "read-only register") -- dropped; the
             * release ignores the write. */
            break;

        default:
            /* upstream: dbg_log!("APIC write32 {:x} <- {:08x}", addr, value);
             * dbg_assert!(false); -- the release discards the write. */
            break;
    }
}

void apic_write32(uint32_t addr, uint32_t value)
{
    if (!cpu->acpi_enabled) {
        return;
    }
    apic_write32_internal(apic_get_apic(), addr, value);
}

/* ------------------------------------------------------------------ *
 * the timer -- apic.rs:469-533
 * ------------------------------------------------------------------ */

double apic_timer(double now)
{
    apic_state_t *apic = apic_get_apic();

    if (apic->timer_initial_count == 0 || apic->timer_current_count == 0) {
        return 100.0;
    }

    if (apic->timer_last_tick > now) {
        /* should only happen after restore_state -- see the 0x390 read arm: on this
         * clock it is also the guest-reboot rewind, and upstream's arm is the
         * behaviour (reset to now), not just a log. */
        apic->timer_last_tick = now;
    }

    double diff = now - apic->timer_last_tick;
    double diff_in_ticks_d = diff * APIC_TIMER_FREQ / (double)(1u << apic->timer_divider_shift);
    uint64_t diff_in_ticks = (uint64_t)diff_in_ticks_d;

    double time_per_interrupt =
        (double)apic->timer_initial_count * (double)(1u << apic->timer_divider_shift) /
        APIC_TIMER_FREQ;

    if (diff_in_ticks >= (uint64_t)apic->timer_initial_count) {
        uint32_t mode = apic->lvt_timer & APIC_TIMER_MODE_MASK;
        if (mode == APIC_TIMER_MODE_PERIODIC) {
            if (diff_in_ticks >= 2 * (uint64_t)apic->timer_initial_count) {
                /* upstream logs "APIC skipping N interrupts" and resynchronises */
                apic->timer_last_tick = now;
            }
            else {
                apic->timer_last_tick += time_per_interrupt;
                /* apic.rs:509: dbg_assert!(apic.timer_last_tick <= now).  DROPPED, like
                 * every other guest-reachable assert in this port (results/r3f-fixes.md,
                 * results/secfix-1.md): the value is host-side schedule arithmetic and a
                 * lapse in it must not abort a guest's boot.  Kept as a comment so the
                 * dropped check is visible where it belonged. */
            }
        }
        else if (mode == APIC_TIMER_MODE_ONE_SHOT) {
            apic->timer_current_count = 0;
        }
        else {
            /* upstream: dbg_assert!(false, "apic unimplemented timer mode: {:x}", mode)
             * -- dropped; the release delivers nothing here but still falls through to
             * the (masked-gated) deliver below, exactly as the control flow runs. */
        }

        if ((apic->lvt_timer & APIC_CONFIG_MASKED) == 0) {
            apic_deliver(apic, (uint8_t)(apic->lvt_timer & 0xFF), APIC_DELIVERY_FIXED, false);
        }
    }

    return apic->timer_last_tick + time_per_interrupt - now;
}

/* ------------------------------------------------------------------ *
 * delivery -- apic.rs:535-597
 * ------------------------------------------------------------------ */

void apic_route(apic_state_t *apic, uint8_t vector, uint8_t mode, bool is_level,
                uint8_t destination, uint8_t destination_mode)
{
    /* TODO (upstream) */
    (void)destination;
    (void)destination_mode;
    apic_deliver(apic, vector, mode, is_level);
}

static void apic_deliver(apic_state_t *apic, uint8_t vector, uint8_t mode, bool is_level)
{
    if (mode == APIC_DELIVERY_INIT) {
        /* TODO (upstream) */
        return;
    }

    if (mode == APIC_DELIVERY_NMI) {
        /* TODO (upstream) */
        return;
    }

    /* upstream: dbg_assert!(false, "TODO: Invalid vector: {:x}", vector) for
     * vector < 0x10 || vector == 0xFF -- dropped (the release proceeds and sets the
     * bit; bit >> 5 stays in [0, 7] for every u8). */

    if (register_get_bit(apic->irr, vector)) {
        /* upstream: dbg_log!("Not delivered: irr already set, vector={:02x}", vector) */
        return;
    }

    register_set_bit(apic->irr, vector);

    if (is_level) {
        register_set_bit(apic->tmr, vector);
    }
    else {
        register_clear_bit(apic->tmr, vector);
    }
}

static int32_t highest_irr(apic_state_t *apic) { return register_get_highest_bit(apic->irr); }

static int32_t highest_isr(apic_state_t *apic) { return register_get_highest_bit(apic->isr); }

static int32_t acknowledge_irq_internal(apic_state_t *apic)
{
    int32_t highest_irr_value = highest_irr(apic);
    if (highest_irr_value < 0) {
        return -1;
    }

    int32_t highest_isr_value = highest_isr(apic);
    if (highest_isr_value >= 0 && highest_isr_value >= highest_irr_value) {
        return -1;
    }

    if ((highest_irr_value & 0xF0) <= (int32_t)(apic->tpr & 0xF0)) {
        return -1;
    }

    register_clear_bit(apic->irr, (uint8_t)highest_irr_value);
    register_set_bit(apic->isr, (uint8_t)highest_irr_value);

    /* upstream (apic.rs:634): dbg_assert!(acknowledge_irq_internal(apic).is_none());
     * the recursive call sits INSIDE the dbg_assert, so upstream's RELEASE build does
     * not evaluate it at all — this port keeps the call (the port's asserts are live,
     * so its behaviour is upstream's DEBUG shape) because the invariant above (the bit
     * just moved to isr is >= every remaining irr bit) makes it return None with no
     * side effect, and it is depth-2 and non-recursive beyond that.  The assert itself
     * is dropped, not the call. */
    (void)acknowledge_irq_internal(apic);

    return highest_irr_value;
}

int32_t apic_acknowledge_irq(void) { return acknowledge_irq_internal(apic_get_apic()); }
