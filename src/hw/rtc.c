/*
 * rtc.c -- the CMOS RTC.  See rtc.h for the time base and for what is stubbed.
 */

#include "rtc.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "../cpu/cpu_state.h"
#include "../cpu/instructions.h" /* microtick */
#include "io.h"
#include "machine.h"

static rtc_state_t rtc_storage;
rtc_state_t *rtc = NULL;

/* ------------------------------------------------------------------ *
 * THE RTC's TIME BASE -- the D-F2 VIRTUAL clock, on a FIXED epoch
 *
 * Upstream reads `Date.now()` here (the host's wall clock in ms since the
 * epoch), and until unit M2-5-DET this port did the same with CLOCK_REALTIME.
 * That was HOST ENTROPY at a site a guest can READ: the CMOS time registers
 * returned the host's current UTC time, so a guest reading registers 0x00-0x09 or
 * 0x32 saw a value that changes between boots -- §7.3's requirement is that the
 * same image and cmdline boot BYTE-IDENTICALLY (results/m2-5-determinism.md,
 * tests/gate-m2-5-determinism.sh).  Upstream has the same nondeterminism; it is a
 * port-level fix for a port-level requirement.
 *
 * The replacement is the port's OWN clock, M2-1F's D-F2 (src/cpu/
 * exec_boundary.c): virtual MILLISECONDS at 1 guest instruction = 1 microsecond,
 * `microtick() = instruction_counter / 1000.0`, the SAME unit the PIT and the
 * PM timer already consume.  A guest-visible clock is therefore a pure function
 * of the guest's own instruction stream, and an idle guest's RTC stops -- the
 * consequence D-F2 already declares for the rest of the machine.
 *
 * THE EPOCH is a fixed constant, not the machine's reset instant, and that is
 * deliberate: `new Date(ms)` is decoded straight into the CMOS registers by
 * utc_fields() below, so resetting the epoch to 0 would report 1970-01-01 to
 * the guest (a date at the very edge of what an RTC driver accepts as valid).
 * RTC_EPOCH_MS keeps the guest's date plausible AND independent of the host.
 * ------------------------------------------------------------------ */
#define RTC_EPOCH_MS 1704067200000.0 /* 2024-01-01T00:00:00Z */

static double rtc_now_ms(void)
{
    return RTC_EPOCH_MS + microtick();
}

/* Howard Hinnant's days_from_civil: the days between 1970-01-01 and y-m-d (proleptic
 * Gregorian).  Used for the alarm scheduling, where upstream calls Date.UTC -- a libc
 * timegm() is a GNU extension and the port's target is an APE on several libcs, so the
 * arithmetic is written out. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Date.UTC(year, month, day, hours, minutes, seconds) in ms -- rtc.js:277-283 */
static double date_utc_ms(int year, int month, int day, int hours, int minutes, int seconds)
{
    return (double)days_from_civil(year, (unsigned)(month + 1), (unsigned)day) * 86400000.0 +
           (double)(hours * 3600 + minutes * 60 + seconds) * 1000.0;
}

/* the fields of `new Date(ms)` the RTC reads (rtc.js:225-267), in UTC */
static void utc_fields(double ms, struct tm *out)
{
    time_t seconds = (time_t)floor(ms / 1000.0);
    gmtime_r(&seconds, out);
}

/* ------------------------------------------------------------------ *
 * the ports -- rtc.js:74-91
 * ------------------------------------------------------------------ */
static void port70_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    /* rtc.js:77-81 */
    rtc->cmos_index = value & 0x7F;
    rtc->nmi_disabled = (value >> 7) & 0xFF;
}

static int32_t port71_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return rtc_cmos_port_read();
}

static void port71_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    rtc_cmos_port_write(value);
}

void rtc_init(void)
{
    rtc = &rtc_storage;
    memset(&rtc_storage, 0, sizeof(rtc_storage));

    rtc->rtc_time = rtc_now_ms();
    rtc->last_update = rtc->rtc_time;

    /* rtc.js:61-69: the constructor's init values, verbatim.  `periodic_interrupt_time`
     * corresponds to the default value for cmos_a. */
    rtc->periodic_interrupt_time = 1000.0 / 1024.0;
    rtc->cmos_a = 0x26;
    rtc->cmos_b = 2;
    rtc->cmos_c = 0;

    io_register_write(0x70, rtc, port70_write, NULL, NULL);
    io_register_write(0x71, rtc, port71_write, NULL, NULL);
    io_register_read(0x71, rtc, port71_read, NULL, NULL);
}

/* ------------------------------------------------------------------ *
 * rtc.js:140-197 -- the timer
 * ------------------------------------------------------------------ */
double rtc_timer(double now, bool legacy_mode)
{
    (void)legacy_mode;
    (void)now; /* the parameter is what upstream OVERWRITES below -- see the comment */

    /* rtc.js:141-145: upstream OVERWRITES the `time` argument with Date.now() ("XXX"),
     * so the argument is still ignored -- but the value it is overwritten WITH is now
     * this port's own clock, rtc_now_ms() (D-F2's virtual time on a fixed epoch), not
     * Date.now()'s host time.  See the time-base block above. */
    double time = rtc_now_ms();
    rtc->rtc_time += time - rtc->last_update;
    rtc->last_update = time;

    if (rtc->periodic_interrupt && rtc->next_interrupt < time) {
        device_raise_irq(8);
        rtc->cmos_c |= 1 << 6 | 1 << 7;

        rtc->next_interrupt +=
            rtc->periodic_interrupt_time *
            ceil((time - rtc->next_interrupt) / rtc->periodic_interrupt_time);
    }
    else if (rtc->next_interrupt_alarm && rtc->next_interrupt_alarm < time) {
        device_raise_irq(8);
        rtc->cmos_c |= 1 << 5 | 1 << 7;
        rtc->next_interrupt_alarm = 0;
    }
    else if (rtc->update_interrupt && rtc->update_interrupt_time < time) {
        device_raise_irq(8);
        rtc->cmos_c |= 1 << 4 | 1 << 7;
        rtc->update_interrupt_time = time + 1000; /* 1 second */
    }

    double t = 100;
    if (rtc->periodic_interrupt && rtc->next_interrupt) {
        t = fmin(t, fmax(0, rtc->next_interrupt - time));
    }
    if (rtc->next_interrupt_alarm) {
        t = fmin(t, fmax(0, rtc->next_interrupt_alarm - time));
    }
    if (rtc->update_interrupt) {
        t = fmin(t, fmax(0, rtc->update_interrupt_time - time));
    }

    return t;
}

/* ------------------------------------------------------------------ *
 * BCD -- rtc.js:199-241
 * ------------------------------------------------------------------ */
static int32_t bcd_pack(int32_t n)
{
    int32_t i = 0;
    int32_t result = 0;

    while (n) {
        int32_t digit = n % 10;
        result |= digit << (4 * i);
        i++;
        n = (n - digit) / 10;
    }

    return result;
}

static int32_t bcd_unpack(int32_t n)
{
    int32_t low = n & 0xF;
    int32_t high = n >> 4 & 0xF;
    /* rtc.js:220-222 */
    DBG_ASSERT(n < 0x100); /* cannot fire: callers pass cmos_data[], a uint8_t */
    /* AUDIT-1R SF1 (the S1 re-sweep): upstream's `dbg_assert(low < 10);
     * dbg_assert(high < 10)` (rtc.js:221-222) are compiled out of the pinned release,
     * whose behaviour for a nibble > 9 is the wrong-but-defined `low + 10*high` -- and
     * the value is a GUEST byte: an alarm-register write (0x71 with index 0x01..0x05)
     * followed by a status-B write with bit 5 set reaches here through
     * rtc_cmos_port_write's `cmos_b & 0x20` arm -> decode_time.  Both asserts are
     * dropped matching the release; the arithmetic is unchanged. */
    return low + 10 * high;
}

static int32_t encode_time(int32_t t)
{
    if (rtc->cmos_b & 4) return t; /* binary mode */
    return bcd_pack(t);
}

static int32_t decode_time(int32_t t)
{
    if (rtc->cmos_b & 4) return t; /* binary mode */
    return bcd_unpack(t);
}

/* ------------------------------------------------------------------ *
 * rtc.js:243-334 -- the CMOS register read
 * ------------------------------------------------------------------ */
int32_t rtc_cmos_port_read(void)
{
    int32_t index = rtc->cmos_index;
    struct tm f = {0};

    /* rtc.js:225-267 reads `new Date(this.rtc_time)`'s UTC fields for the seven time
     * registers and for the century. */
    if ((index >= CMOS_RTC_SECONDS && index <= CMOS_RTC_YEAR) || index == CMOS_CENTURY ||
        index == CMOS_CENTURY2) {
        utc_fields(rtc->rtc_time, &f);
    }

    switch (index) {
    case CMOS_RTC_SECONDS: return encode_time(f.tm_sec);
    case CMOS_RTC_MINUTES: return encode_time(f.tm_min);
    case CMOS_RTC_HOURS:
        /* TODO: 12 hour mode (rtc.js:255) */
        return encode_time(f.tm_hour);
    case CMOS_RTC_DAY_WEEK:
        /* getUTCDay() is 0-based; the RTC's day-of-week is 1-based (Sunday = 1) */
        return encode_time(f.tm_wday + 1);
    case CMOS_RTC_DAY_MONTH: return encode_time(f.tm_mday);
    case CMOS_RTC_MONTH: return encode_time(f.tm_mon + 1);
    case CMOS_RTC_YEAR: return encode_time((f.tm_year + 1900) % 100);
    case CMOS_CENTURY:
    case CMOS_CENTURY2:
        /* rtc.js:324-327: `getUTCFullYear() / 100 | 0` -- truncating division */
        return encode_time((f.tm_year + 1900) / 100);

    case CMOS_STATUS_A:
        /* rtc.js:269-277: update-in-progress for one millisecond every second.  microtick
         * is this port's emulator clock in MILLISECONDS -- the deterministic virtual clock
         * at 1 guest instruction = 1 microsecond (exec_boundary.c, D-F2) -- so `% 1000` is
         * "the last millisecond of a second", as upstream's `% 1000` on its wall clock.
         * NOTE the RTC's TIME registers below read the SAME D-F2 clock (rtc_now_ms), so
         * this register and those agree: the whole device is instruction-driven, and the
         * guest sees the same value twice for the same instruction stream. */
        if (fmod(microtick(), 1000.0) >= 999.0) {
            return rtc->cmos_a | 0x80;
        }
        return rtc->cmos_a;

    case CMOS_STATUS_B:
        return rtc->cmos_b;

    case CMOS_STATUS_C: {
        /* rtc.js:289-300: reading C lowers the IRQ line and clears the flags */
        device_lower_irq(8);
        int32_t c = rtc->cmos_c;
        rtc->cmos_c &= ~0xF0;
        return c;
    }

    case CMOS_STATUS_D:
        return 1 << 7; /* CMOS battery charged */

    case CMOS_DIAG_STATUS:
        return rtc->cmos_diag_status;

    default:
        /* rtc.js:331-333 */
        return rtc->cmos_data[rtc->cmos_index];
    }
}

/* ------------------------------------------------------------------ *
 * rtc.js:336-377 -- the CMOS register write
 * ------------------------------------------------------------------ */
void rtc_cmos_port_write(int32_t data_byte)
{
    switch (rtc->cmos_index) {
    case 0xA:
        /* rtc.js:340-345.  `32768 >> ((cmos_a & 0xF) - 1)` -- JS masks the shift count to
         * 5 bits, so a low nibble of 0 shifts by 31 and the divisor is 0 (1000/0 = +inf
         * upstream, +inf here).  Written with the mask so the C shift is defined. */
        rtc->cmos_a = (uint8_t)(data_byte & 0x7F);
        rtc->periodic_interrupt_time =
            1000.0 / (double)(uint32_t)(32768u >> (uint32_t)(((rtc->cmos_a & 0xF) - 1) & 31));
        break;

    case 0xB:
        rtc->cmos_b = (uint8_t)data_byte;
        if (rtc->cmos_b & 0x80) {
            /* remove update interrupt flag */
            rtc->cmos_b &= 0xEF;
        }
        if (rtc->cmos_b & 0x40) {
            rtc->next_interrupt = rtc_now_ms();
        }

        if (rtc->cmos_b & 0x20) {
            double now = rtc_now_ms();
            struct tm fields;
            utc_fields(now, &fields);

            int32_t seconds = decode_time(rtc->cmos_data[CMOS_RTC_SECONDS_ALARM]);
            int32_t minutes = decode_time(rtc->cmos_data[CMOS_RTC_MINUTES_ALARM]);
            int32_t hours = decode_time(rtc->cmos_data[CMOS_RTC_HOURS_ALARM]);

            double alarm_date = date_utc_ms(fields.tm_year + 1900, fields.tm_mon, fields.tm_mday,
                                            hours, minutes, seconds);
            rtc->next_interrupt_alarm = alarm_date;
        }

        if (rtc->cmos_b & 0x10) {
            rtc->update_interrupt_time = rtc_now_ms();
        }
        break;

    case CMOS_DIAG_STATUS:
        rtc->cmos_diag_status = (uint8_t)data_byte;
        break;

    case CMOS_RTC_SECONDS_ALARM:
    case CMOS_RTC_MINUTES_ALARM:
    case CMOS_RTC_HOURS_ALARM:
        rtc_cmos_write(rtc->cmos_index, data_byte);
        break;

    default:
        break;
    }

    /* rtc.js:373-374 */
    rtc->update_interrupt = (rtc->cmos_b & 0x10) == 0x10 && (rtc->cmos_a & 0xF) > 0;
    rtc->periodic_interrupt = (rtc->cmos_b & 0x40) == 0x40 && (rtc->cmos_a & 0xF) > 0;
}

/* rtc.js:379-386 */
uint8_t rtc_cmos_read(int32_t index)
{
    DBG_ASSERT(index < 128);
    return rtc->cmos_data[index];
}

/* rtc.js:393-407 */
void rtc_cmos_write(int32_t index, int32_t value)
{
    DBG_ASSERT(index < 128);
    rtc->cmos_data[index] = (uint8_t)value;
}

/* ------------------------------------------------------------------ *
 * cpu.js:1644-1711 -- CPU.prototype.fill_cmos
 *
 * The `settings` dependency is fixed to this machine's configuration: boot_order =
 * BOOT_ORDER_CD_FIRST (cpu.js:1646's `settings.boot_order || BOOT_ORDER_CD_FIRST`) and no
 * `fastboot` (so the 0x3f write is not made).  Everything else is a function of
 * cpu->memory_size.
 * ------------------------------------------------------------------ */
void rtc_fill_cmos(void)
{
    int32_t boot_order = BOOT_ORDER_CD_FIRST;

    /* bootflag 1, high nibble, lowest priority; low nibble disables the floppy signature
     * check (1) */
    rtc_cmos_write(CMOS_BIOS_BOOTFLAG1, 1 | (boot_order >> 4 & 0xF0));
    /* bootflag 2, both nibbles, high and middle priority */
    rtc_cmos_write(CMOS_BIOS_BOOTFLAG2, boot_order & 0xFF);

    /* 640k or less if less memory is used */
    rtc_cmos_write(CMOS_MEM_BASE_LOW, 640 & 0xFF);
    rtc_cmos_write(CMOS_MEM_BASE_HIGH, 640 >> 8);

    int32_t memory_above_1m = 0; /* in k */
    if (cpu->memory_size >= 1024 * 1024) {
        memory_above_1m = (int32_t)((cpu->memory_size - 1024 * 1024) >> 10);
        if (memory_above_1m > 0xFFFF) memory_above_1m = 0xFFFF;
    }

    rtc_cmos_write(CMOS_MEM_OLD_EXT_LOW, memory_above_1m & 0xFF);
    rtc_cmos_write(CMOS_MEM_OLD_EXT_HIGH, memory_above_1m >> 8 & 0xFF);
    rtc_cmos_write(CMOS_MEM_EXTMEM_LOW, memory_above_1m & 0xFF);
    rtc_cmos_write(CMOS_MEM_EXTMEM_HIGH, memory_above_1m >> 8 & 0xFF);

    int32_t memory_above_16m = 0; /* in 64k blocks */
    if (cpu->memory_size >= 16 * 1024 * 1024) {
        memory_above_16m = (int32_t)((cpu->memory_size - 16 * 1024 * 1024) >> 16);
        if (memory_above_16m > 0xFFFF) memory_above_16m = 0xFFFF;
    }
    rtc_cmos_write(CMOS_MEM_EXTMEM2_LOW, memory_above_16m & 0xFF);
    rtc_cmos_write(CMOS_MEM_EXTMEM2_HIGH, memory_above_16m >> 8 & 0xFF);

    /* memory above 4G (not supported by this emulator) */
    rtc_cmos_write(CMOS_MEM_HIGHMEM_LOW, 0);
    rtc_cmos_write(CMOS_MEM_HIGHMEM_MID, 0);
    rtc_cmos_write(CMOS_MEM_HIGHMEM_HIGH, 0);

    rtc_cmos_write(CMOS_EQUIPMENT_INFO, 0x2F);
    rtc_cmos_write(CMOS_BIOS_SMP_COUNT, 0);
}
