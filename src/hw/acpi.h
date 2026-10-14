/*
 * acpi.h -- the PIIX4 ACPI controller: the PM1 status/enable registers, the ACPI status
 * register, the power-management timer and the four general-purpose event registers.
 *
 * Port of v86/src/acpi.js (216 lines).  It IS reachable without a chipset: upstream creates
 * it only when `settings.acpi` is set (cpu.js:1181-1185), and the machine here does the
 * same -- cpu->acpi_enabled, which the loader never sets.  The device is therefore absent
 * from the APE's default machine, exactly as it is absent from the pinned reference under
 * oracle/run.mjs's configuration, and the selftest creates it deliberately (acpi_init) to
 * exercise it.  MEASURED: the reference's own default for `settings.acpi` is undefined, so
 * `acpi_enabled[0] = +undefined` is 0 in its Uint8Array view -- the ACPI device is NOT
 * created in the oracle's machine either.
 *
 * THE PM TIMER's clock is this port's microtick (MILLISECONDS of the deterministic virtual
 * clock at 1 guest instruction = 1 microsecond -- exec_boundary.c's D-F2), as everywhere
 * else in M2-1, with upstream's imprecision offset kept verbatim (acpi.js:184-206): the
 * offset exists because a browser's clock can repeat a value, and this port's clock really
 * does repeat between two reads in one batch, so the mechanism is load-bearing here for
 * the same reason.
 */

#ifndef V86C_HW_ACPI_H
#define V86C_HW_ACPI_H

#include <stdint.h>

typedef struct acpi_state {
    int32_t timer_last_value;
    int32_t timer_imprecision_offset;

    uint32_t status;
    uint32_t pm1_status;
    uint32_t pm1_enable;
    int32_t last_timer;

    uint8_t gpe[4];
} acpi_state_t;

extern acpi_state_t *acpi;

/* Requires pci_init(): the constructor registers the device on the PCI bus
 * (acpi.js:44-46). */
void acpi_init(void);

/* acpi.js:136-155 -- the device's timer callback (cpu.js:1932 calls it when acpi_enabled).
 * Always returns 100 (upstream's TODO). */
double acpi_timer(double now);

/* acpi.js:184-206 */
int32_t acpi_get_timer(double now);

#endif /* V86C_HW_ACPI_H */
