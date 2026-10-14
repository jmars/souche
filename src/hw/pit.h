/*
 * pit.h -- the 8254 programmable interval timer.
 *
 * Port of v86/src/pit.js (359 lines).  Three counters, the mode/read-mode/next-low state
 * machine of port 0x43, the latch command, and the periodic interrupt on counter 0.
 *
 * THE CLOCK, stated because it is the whole timer's unit: upstream reads `v86.microtick()`
 * and multiplies by OSCILLATOR_FREQ (kHz) to get ticks, which is only dimensionally right
 * if the clock is MILLISECONDS -- and upstream's is (main.js:223-240 reads the host wall
 * clock, whose performance.now/Date.now are milliseconds).
 *
 * M2-1F (D-F2) FIXED this port's unit at the source: exec_boundary.c's microtick is
 * MILLISECONDS of a DETERMINISTIC VIRTUAL CLOCK at 1 guest instruction = 1 MICROSECOND.
 * So this file's arithmetic is upstream's verbatim AND the scale is right: one PIT tick is
 * 1/OSCILLATOR_FREQ ms = 0.838 us = 0.838 instructions, and a guest that programs counter 0
 * to 65536 gets its IRQ0 after ~54925 instructions -- ~54.9 ms of virtual time, which is
 * what the real 8254 would deliver.  (M1c's seat, which this file consumed until M2-1F,
 * returned the RAW instruction counter -- one MILLISECOND per instruction, 1000x fast, so
 * that same reload fired every 10 instructions: M2-1's finding F2, now closed.)
 *
 * The clock is a pure function of the guest's instruction stream, which is what the
 * emulator's deterministic contract needs (a wall clock makes every run differ).
 *
 * WHAT IS STUBBED: nothing in the counter state machine.  Three upstream behaviours are
 * dropped because there is no device to tell: "pcspeaker-enable"/"-disable"/"pcspeaker-update"
 * (bus.js, no speaker), the counter-1 "Unimplemented timer1" log line (a dbg_log only), and
 * the read-back command (port 0x43 with i === 3), which upstream logs and returns on -- the
 * port keeps the early return, so a guest's read-back command is a silent no-op EXACTLY as
 * upstream.  A guest that programs counter 1 or 2 and expects a signal sees nothing, as
 * upstream.
 */

#ifndef V86C_HW_PIT_H
#define V86C_HW_PIT_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

/* pit.js:104-109 -- the typed arrays' widths are load-bearing (a Uint8Array store truncates
 * to 8 bits, a Uint16Array store to 16), so the C fields carry exactly those widths. */
typedef struct pit_state {
    double counter_start_time[3];  /* Float64Array(3) */
    uint16_t counter_start_value[3]; /* Uint16Array(3) */
    uint8_t counter_next_low[4];
    uint8_t counter_enabled[4];
    uint8_t counter_mode[4];
    uint8_t counter_read_mode[4];
    uint8_t counter_latch[4];
    uint16_t counter_latch_value[3];
    uint16_t counter_reload[3];

    bus_connector_t *bus;
} pit_state_t;

extern pit_state_t *pit;

void pit_init(bus_connector_t *bus);

/* pit.js:100-160 -- the device's timer callback (cpu.js:1926 calls it with no_irq false).
 * Returns the time until the next interrupt, in the clock unit above. */
double pit_timer(double now, bool no_irq);

/* pit.js:162-355 -- the counter state machine, public so the port handlers above can be
 * declared before them (and so the selftest can drive a counter without a port write). */
int32_t pit_get_counter_value(int32_t i, double now);
bool pit_did_rollover(int32_t i, double now);
int32_t pit_counter_read(int32_t i);
void pit_counter_write(int32_t i, int32_t value);
void pit_port43_write(int32_t reg_byte);

#endif /* V86C_HW_PIT_H */
