/*
 * pit.c -- the 8254 programmable interval timer.  See pit.h for the clock's unit and for
 * exactly which upstream behaviours are dropped.
 */

#include "pit.h"

#include <math.h>
#include <string.h>

#include "../cpu/cpu_state.h"
#include "../cpu/instructions.h" /* microtick */
#include "io.h"
#include "machine.h"

/* pit.js:15 -- the oscillator in kHz. */
#define OSCILLATOR_FREQ 1193.1816666

static pit_state_t pit_storage;
pit_state_t *pit = NULL;

/* JS `x & 1` on a double: ToInt32 semantics (truncate, wrap modulo 2^32, reinterpret).
 * pit.js:120 computes `(now * (1000 * 1000 / 15000)) & 1`, where `now` is the emulator
 * clock -- a double.  MEASURED: the value is exact for every integer clock this port can
 * produce, but the C cast of an out-of-range double is undefined, so the conversion is
 * written out. */
static int32_t js_to_int32(double x)
{
    if (!isfinite(x)) return 0;
    double m = fmod(trunc(x), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

/* ------------------------------------------------------------------ *
 * the ports -- pit.js:115-147
 * ------------------------------------------------------------------ */

static int32_t port61_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    double now = microtick();
    /* pit.js:120-124: the refresh-toggle bit and counter 2's output.  JS's `bool << 5` is
     * the boolean's 0/1. */
    int32_t ref_toggle = js_to_int32(now * (1000.0 * 1000.0 / 15000.0)) & 1;
    bool counter2_out = pit_did_rollover(2, now);
    return ref_toggle << 4 | (counter2_out ? 1 : 0) << 5;
}

static void port61_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    /* pit.js:127-135 */
    /* pit.js:127-135 sends the two messages with NO value. */
    if (value & 1) {
        bus_send(pit->bus, "pcspeaker-enable", NULL, 0);
    }
    else {
        bus_send(pit->bus, "pcspeaker-disable", NULL, 0);
    }
}

static int32_t port40_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return pit_counter_read(0);
}

static int32_t port41_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return pit_counter_read(1);
}

static int32_t port42_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return pit_counter_read(2);
}

static void port40_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    pit_counter_write(0, value);
}

static void port41_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    pit_counter_write(1, value);
}

static void port42_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    pit_counter_write(2, value);
    BUS_SEND(pit->bus, "pcspeaker-update", pit->counter_mode[2], pit->counter_reload[2]);
}

static void port43_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    pit_port43_write(value);
}

void pit_init(bus_connector_t *bus)
{
    pit = &pit_storage;
    memset(&pit_storage, 0, sizeof(pit_storage));
    pit->bus = bus;

    io_register_read(0x61, pit, port61_read, NULL, NULL);
    io_register_write(0x61, pit, port61_write, NULL, NULL);

    io_register_read(0x40, pit, port40_read, NULL, NULL);
    io_register_read(0x41, pit, port41_read, NULL, NULL);
    io_register_read(0x42, pit, port42_read, NULL, NULL);

    io_register_write(0x40, pit, port40_write, NULL, NULL);
    io_register_write(0x41, pit, port41_write, NULL, NULL);
    io_register_write(0x42, pit, port42_write, NULL, NULL);

    io_register_write(0x43, pit, port43_write, NULL, NULL);
}

/* ------------------------------------------------------------------ *
 * the timer -- pit.js:100-160
 * ------------------------------------------------------------------ */

double pit_timer(double now, bool no_irq)
{
    double time_to_next_interrupt = 100;

    /* counter 0 produces interrupts */
    if (!no_irq) {
        if (pit->counter_enabled[0] && pit_did_rollover(0, now)) {
            double diff = now - pit->counter_start_time[0];
            double diff_in_ticks = floor(diff * OSCILLATOR_FREQ);

            pit->counter_start_value[0] = (uint16_t)pit_get_counter_value(0, now);
            pit->counter_start_time[0] = diff < 0 ? now : pit->counter_start_time[0] +
                                                              diff_in_ticks / OSCILLATOR_FREQ;

            /* upstream (pit.js:124-131): lower the line, then raise it -- "necessary since
             * browsers may sleep longer than necessary ... and clear the irq". */
            device_lower_irq(0);
            device_raise_irq(0);

            uint8_t mode = pit->counter_mode[0];
            if (mode == 0) {
                pit->counter_enabled[0] = 0;
            }
        }
        else {
            device_lower_irq(0);
        }

        if (pit->counter_enabled[0]) {
            double diff = now - pit->counter_start_time[0];
            double diff_in_ticks = floor(diff * OSCILLATOR_FREQ);
            /* XXX: to simplify (pit.js:135) */
            double ticks_missing = (double)pit->counter_start_value[0] - diff_in_ticks;
            time_to_next_interrupt = ticks_missing / OSCILLATOR_FREQ;
        }
    }

    return time_to_next_interrupt;
}

/* pit.js:162-197 */
int32_t pit_get_counter_value(int32_t i, double now)
{
    DBG_ASSERT(i >= 0 && i < 3);
    if (!pit->counter_enabled[i]) {
        return 0;
    }

    double diff = now - pit->counter_start_time[i];
    double diff_in_ticks = floor(diff * OSCILLATOR_FREQ);

    double value = (double)pit->counter_start_value[i] - diff_in_ticks;

    uint32_t reload = pit->counter_reload[i];

    /* pit.js:186 `value %= reload` -- reachable only with a zero reload, where JS yields
     * NaN (stored as 0 by the Uint16Array).  counter_write turns a written 0 into 0xFFFF
     * and is the only writer of counter_reload, so reload is never 0 for an enabled
     * counter: DBG_ASSERT states the invariant instead of porting the NaN. */
    DBG_ASSERT(reload != 0);
    if (value >= (double)reload) {
        value = fmod(value, (double)reload);
    }
    else if (value < 0) {
        /* JS: value = value % reload + reload; C's % has the same sign rule. */
        value = fmod(value, (double)reload) + (double)reload;
    }

    return (int32_t)value;
}

/* pit.js:199-214 */
bool pit_did_rollover(int32_t i, double now)
{
    DBG_ASSERT(i >= 0 && i < 3);
    double diff = now - pit->counter_start_time[i];

    if (diff < 0) {
        /* pit.js:203-208: "should only happen after restore_state".  Upstream returns true
         * WITHOUT touching the timer (the warning names "resetting" but the timer is only
         * reset by the caller, pit_timer). */
        return true;
    }
    double diff_in_ticks = floor(diff * OSCILLATOR_FREQ);

    return pit->counter_start_value[i] < diff_in_ticks;
}

/* pit.js:216-247 */
int32_t pit_counter_read(int32_t i)
{
    DBG_ASSERT(i >= 0 && i < 3);
    uint8_t latch = pit->counter_latch[i];

    if (latch) {
        pit->counter_latch[i]--;
        if (latch == 2) {
            return pit->counter_latch_value[i] & 0xFF;
        }
        return pit->counter_latch_value[i] >> 8;
    }

    uint8_t next_low = pit->counter_next_low[i];
    if (pit->counter_mode[i] == 3) {
        pit->counter_next_low[i] ^= 1;
    }

    int32_t value = pit_get_counter_value(i, microtick());
    if (next_low) {
        return value & 0xFF;
    }
    return value >> 8;
}

/* pit.js:249-283 */
void pit_counter_write(int32_t i, int32_t value)
{
    DBG_ASSERT(i >= 0 && i < 3);
    if (pit->counter_next_low[i]) {
        pit->counter_reload[i] = (uint16_t)((pit->counter_reload[i] & ~0xFFu) | (uint32_t)value);
    }
    else {
        pit->counter_reload[i] =
            (uint16_t)((pit->counter_reload[i] & 0xFFu) | (uint32_t)value << 8);
    }

    if (pit->counter_read_mode[i] != 3 || !pit->counter_next_low[i]) {
        if (!pit->counter_reload[i]) {
            pit->counter_reload[i] = 0xFFFF;
        }
        /* depends on the mode, should actually happen on the first tick */
        pit->counter_start_value[i] = pit->counter_reload[i];
        pit->counter_enabled[i] = 1;
        pit->counter_start_time[i] = microtick();
    }

    if (pit->counter_read_mode[i] == 3) {
        pit->counter_next_low[i] ^= 1;
    }
}

/* pit.js:285-355 */
void pit_port43_write(int32_t reg_byte)
{
    int32_t mode = reg_byte >> 1 & 7;
    int32_t binary_mode = reg_byte & 1;
    int32_t i = reg_byte >> 6 & 3;
    int32_t read_mode = reg_byte >> 4 & 3;

    if (i == 3) {
        /* pit.js:293-297: the read-back command.  Upstream logs "Unimplemented read back"
         * and returns; the log channel does not exist in the port, so the return is the
         * whole behaviour. */
        return;
    }

    if (read_mode == 0) {
        /* latch */
        pit->counter_latch[i] = 2;
        int32_t value = pit_get_counter_value(i, microtick());
        pit->counter_latch_value[i] = (uint16_t)(value ? value - 1 : 0);
        return;
    }

    if (mode >= 6) {
        /* 6 and 7 are aliased to 2 and 3 */
        mode &= ~4;
    }

    if (read_mode == 1) {
        /* lsb */
        pit->counter_next_low[i] = 1;
    }
    else if (read_mode == 2) {
        /* msb */
        pit->counter_next_low[i] = 0;
    }
    else {
        /* first lsb then msb */
        pit->counter_next_low[i] = 1;
    }

    if (i == 0) {
        device_lower_irq(0);
    }

    /* pit.js:333-341: modes 0, 2 and 3 are the implemented ones; 1, 4 and 5 log
     * "Unimplemented counter mode" upstream and are still stored below -- the port keeps
     * exactly that (stored, not refused). */
    (void)mode;
    (void)binary_mode;

    pit->counter_mode[i] = (uint8_t)mode;
    pit->counter_read_mode[i] = (uint8_t)read_mode;

    if (i == 2) {
        BUS_SEND(pit->bus, "pcspeaker-update", pit->counter_mode[2], pit->counter_reload[2]);
    }
}
