/* REVIEWFIX-1's PM-timer probe (AUDIT-1R SF2) -- the unit-level asymmetry plus the
 * control that pins the claim's true reachability.  Built and driven by
 * results/reviewfix1-pmtimer-probe.sh against a SOURCE ROOT parameter, so the SAME
 * probe runs against the live tree and against the pre-fix sources (414d194
 * materialized to a shadow) -- the before/after asymmetry is the proof, never a claim.
 *
 * Modes:
 *   pmtimer   THE ASYMMETRY.  Constructs the real device set (machine_init), then
 *             constructs ACPI the way a real boot NEVER does (acpi_init -- the device
 *             exists only when cpu->acpi_enabled, which has no writer in this port),
 *             runs the virtual clock up (timer_last_value goes large), then zeroes
 *             instruction_counter -- EXACTLY what reset_cpu does (cpu_state.c:176) --
 *             and reads the PM timer again, the first read of a boot 2.  PRE-FIX:
 *             DBG_ASSERT(t > timer_last_value) aborts (rc 134).  POST-FIX: the
 *             graceful overshoot arm keeps the stale value and the probe runs on.
 *   unclaimed THE CONTROL, and the correction to the review's premise: with the device
 *             constructed the way EVERY real boot constructs it (machine_init alone,
 *             no acpi_init), a guest read of 0xB008 through the REAL dispatch answers
 *             D-F1's unclaimed 0xFFFFFFFF on BOTH trees -- the assert is not
 *             guest-reachable in any boot this port can run today; it is LATENT, like
 *             S3's ATAPI clamp.  This mode must PASS (exit 0) pre-fix and post-fix.
 *
 * Exit: 0 = the claimed behaviour held; 134 = the assert (the pre-fix pmtimer leg).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu_state.h"
#include "cpu/instructions.h" /* microtick */
#include "cpu/memory.h"
#include "hw/acpi.h"
#include "hw/io.h"
#include "hw/machine.h"

#define GUEST_RAM (16u * 1024u * 1024u)

static void bringup(int with_acpi)
{
	cpu_state_init();
	if (mem8 == NULL && !allocate_memory(GUEST_RAM)) {
		fputs("probe: cannot allocate guest memory\n", stderr);
		exit(2);
	}
	cpu->memory_size = GUEST_RAM;
	zero_memory(0, cpu->memory_size);
	cpu->cr[0] = 1;
	cpu->protected_mode = true;
	cpu->is_32 = true;
	cpu->stack_size_32 = true;
	cpu->cpl = 0;

	machine_init(); /* the real device set; constructs ACPI only when acpi_enabled */
	if (with_acpi) acpi_init(); /* the construction NO real boot performs */
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fputs("usage: reviewfix1-pmtimer-probe <pmtimer|unclaimed>\n", stderr);
		return 2;
	}

	if (!strcmp(argv[1], "pmtimer")) {
		bringup(1);

		/* boot 1: the virtual clock runs up; the PM timer latches a large value.
		 * 400e6 instructions = 400e3 ms; t = round(400000 * 3579.545) ~ 1.43e9,
		 * inside int32 (past ~600e6 instructions the int32 t would wrap -- a
		 * pre-existing quirk of the unmasked internal value this probe stays
		 * clear of, not part of this finding). */
		cpu->instruction_counter = 400000000;
		int32_t boot1_timer = acpi_get_timer(microtick());
		if (boot1_timer == 0) {
			fputs("probe: boot 1 timer unexpectedly 0\n", stderr);
			return 1;
		}
		printf("boot 1: instruction_counter=%lld timer_last_value=%d\n",
		       (long long)cpu->instruction_counter, boot1_timer);

		/* THE REBOOT INTERACTION: reset_cpu zeroes the counter (cpu_state.c:176)
		 * while ACPI state persists (machine_reboot resets pic/pit/rtc/ps2/
		 * virtio_console only). */
		cpu->instruction_counter = 0;

		/* boot 2's first PM-timer read: t ~ 0 != timer_last_value, t < last.
		 * PRE-FIX the DBG_ASSERT aborts here; POST-FIX the overshoot arm runs. */
		int32_t boot2_timer = acpi_get_timer(microtick());
		printf("boot 2: read returned %d (stale last_value, the overshoot arm) -- "
		       "ran on\n",
		       boot2_timer);
		return 0;
	}

	if (!strcmp(argv[1], "unclaimed")) {
		bringup(0);

		/* the guest-visible truth in every real boot: 0xB008 is unregistered */
		int32_t v = io_portmap_read32(0xB008);
		printf("guest read 0xB008 with the machine's real device set: 0x%08x\n",
		       (unsigned)v);
		if (v != -1) {
			fputs("probe: 0xB008 answered something other than D-F1's unclaimed"
			      " 0xFFFFFFFF -- the premise of this control does not hold\n",
			      stderr);
			return 1;
		}
		return 0;
	}

	fputs("probe: unknown mode\n", stderr);
	return 2;
}
