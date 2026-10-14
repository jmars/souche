/*
 * rtc.h -- the CMOS real-time clock (the MC146818 the BIOS and the OS read the date and
 * the machine's configuration from).
 *
 * Port of v86/src/rtc.js (424 lines) plus CPU.prototype.fill_cmos (cpu.js:1644-1711),
 * which is the RTC's bring-up -- it seeds the CMOS registers the BIOS reads the memory
 * size and the boot order from -- and has no other home in the port.  Its `settings`
 * dependency is fixed to the M2-1 machine's configuration (boot_order =
 * BOOT_ORDER_CD_FIRST, no fastboot), stated at the call site.
 *
 * THE CLOCK.  Upstream reads `Date.now()` -- the HOST wall clock in milliseconds since
 * the epoch -- for both the RTC's own time base and the periodic/update interrupt
 * scheduling, and that is HOST ENTROPY at a site a guest can READ (the CMOS time
 * registers), which §7.3's reproducibility requirement forbids.  Unit M2-5-DET
 * replaced it with the port's OWN clock, M2-1F's D-F2 (src/cpu/exec_boundary.c): virtual
 * MILLISECONDS at 1 guest instruction = 1 microsecond, on a FIXED epoch
 * (2024-01-01T00:00:00Z, src/hw/rtc.c's RTC_EPOCH_MS).  A guest that reads CMOS
 * 0x00-0x09 or 0x32 therefore sees a value that is a pure function of its own
 * instruction stream -- the same value on a second boot of the same image, which is what
 * tests/gate-m2-5-determinism.sh asserts byte for byte (results/m2-5-determinism.md).
 * This was the ONE ported device whose guest-visible state was not a function of the
 * instruction stream; with it closed, so is §7.3's native-lane item.
 *
 * WHAT IS STUBBED: nothing that upstream implements.  The four TODOs at rtc.js:213-218
 * (interrupt on update, countdown, letting the BIOS set values, 12-hour mode) are
 * upstream's own, and the port keeps their behaviour: an update interrupt is scheduled by
 * register B and fires (rtc.js:266-273 implements it), a guest write to the time registers
 * is accepted and ignored (the default arm of cmos_port_write), and hour 0x04 is always
 * 24-hour (rtc.js:255).
 */

#ifndef V86C_HW_RTC_H
#define V86C_HW_RTC_H

#include <stdbool.h>
#include <stdint.h>

/* rtc.js:9-45 */
#define CMOS_RTC_SECONDS 0x00
#define CMOS_RTC_SECONDS_ALARM 0x01
#define CMOS_RTC_MINUTES 0x02
#define CMOS_RTC_MINUTES_ALARM 0x03
#define CMOS_RTC_HOURS 0x04
#define CMOS_RTC_HOURS_ALARM 0x05
#define CMOS_RTC_DAY_WEEK 0x06
#define CMOS_RTC_DAY_MONTH 0x07
#define CMOS_RTC_MONTH 0x08
#define CMOS_RTC_YEAR 0x09
#define CMOS_STATUS_A 0x0a
#define CMOS_STATUS_B 0x0b
#define CMOS_STATUS_C 0x0c
#define CMOS_STATUS_D 0x0d
#define CMOS_DIAG_STATUS 0x0e
#define CMOS_RESET_CODE 0x0f
#define CMOS_FLOPPY_DRIVE_TYPE 0x10
#define CMOS_DISK_DATA 0x12
#define CMOS_EQUIPMENT_INFO 0x14
#define CMOS_MEM_BASE_LOW 0x15
#define CMOS_MEM_BASE_HIGH 0x16
#define CMOS_MEM_OLD_EXT_LOW 0x17
#define CMOS_MEM_OLD_EXT_HIGH 0x18
#define CMOS_DISK_DRIVE1_TYPE 0x19
#define CMOS_DISK_DRIVE2_TYPE 0x1a
#define CMOS_DISK_DRIVE1_CYL 0x1b
#define CMOS_DISK_DRIVE2_CYL 0x24
#define CMOS_MEM_EXTMEM_LOW 0x30
#define CMOS_MEM_EXTMEM_HIGH 0x31
#define CMOS_CENTURY 0x32
#define CMOS_MEM_EXTMEM2_LOW 0x34
#define CMOS_MEM_EXTMEM2_HIGH 0x35
#define CMOS_CENTURY2 0x37
#define CMOS_BIOS_BOOTFLAG1 0x38
#define CMOS_BIOS_DISKTRANSFLAG 0x39
#define CMOS_BIOS_BOOTFLAG2 0x3d
#define CMOS_MEM_HIGHMEM_LOW 0x5b
#define CMOS_MEM_HIGHMEM_MID 0x5c
#define CMOS_MEM_HIGHMEM_HIGH 0x5d
#define CMOS_BIOS_SMP_COUNT 0x5f

/* rtc.js:47-50 */
#define BOOT_ORDER_CD_FIRST 0x123
#define BOOT_ORDER_HD_FIRST 0x312
#define BOOT_ORDER_FD_FIRST 0x321

/* rtc.js:53-95 */
typedef struct rtc_state {
    int32_t cmos_index;
    uint8_t cmos_data[128];

    double rtc_time;   /* ms since the FIXED epoch (rtc.c's RTC_EPOCH_MS), not the host's */
    double last_update;

    double next_interrupt;
    double next_interrupt_alarm;

    bool periodic_interrupt;
    double periodic_interrupt_time;

    uint8_t cmos_a;
    uint8_t cmos_b;
    uint8_t cmos_c;

    uint8_t cmos_diag_status;
    uint8_t nmi_disabled;

    bool update_interrupt;
    double update_interrupt_time;
} rtc_state_t;

extern rtc_state_t *rtc;

void rtc_init(void);

/* rtc.js:146-197 -- the device's timer callback (cpu.js:1927 calls it with legacy_mode
 * false).  Returns the time in ms until the next scheduled interrupt. */
double rtc_timer(double now, bool legacy_mode);

/* rtc.js:379-407 -- CPU.prototype.fill_cmos (cpu.js:1644-1711), the CMOS bring-up. */
void rtc_fill_cmos(void);

/* rtc.js:379-407 (the two public accessors upstream's fill_cmos and the alarm path use) */
uint8_t rtc_cmos_read(int32_t index);
void rtc_cmos_write(int32_t index, int32_t value);

/* rtc.js:243-377 -- the two port 0x71 handlers (public for the same reason as the PIT's
 * state machine: they are declared above their use in port71_read). */
int32_t rtc_cmos_port_read(void);
void rtc_cmos_port_write(int32_t data_byte);

#endif /* V86C_HW_RTC_H */
