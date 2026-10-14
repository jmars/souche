/*
 * exec_boundary.c -- the machine boundary's two non-device functions (and fwait).
 * See exec_boundary.h for what this file used to be and why these are the ones left.
 *
 * WHAT IS FIXED HERE, AND WHAT A GUEST OBSERVES (the brief asks for exactly this):
 *   - `microtick` is a DETERMINISTIC VIRTUAL CLOCK, not a wall clock.  Upstream's is the
 *     host wall clock in MILLISECONDS (main.js:223-240: performance.now / Date.now /
 *     process.hrtime); a real clock would make the run nondeterministic, which the oracle
 *     contract forbids.  The port's clock is a pure function of the guest's own instruction
 *     stream -- D-F2, declared at the definition below: 1 guest instruction = 1 MICROSECOND
 *     of virtual time, so `microtick() = instruction_counter / 1000.0` (MILLISECONDS, the
 *     unit every consumer is written for).
 *     M2-1's devices consume THIS SAME UNIT (src/hw/pit.c multiplies it by the PIT's
 *     1193.18 kHz to get ticks, src/hw/acpi.c scales it for the PM timer, src/hw/rtc.c takes
 *     it modulo 1000 ms), so the whole machine advances one emulated MICROSECOND per
 *     instruction.  What a guest observes as a consequence: a PIT programmed to 65536 ticks
 *     gets its IRQ0 after ~54925 instructions (~54.9 ms of virtual time), and the TSC
 *     (cpu.rs:4365, microtick()*TSC_RATE) advances 1000 counts per instruction -- 1 GHz,
 *     the rate cpuid advertises.  The RTC is on this clock too (M2-5-DET: src/hw/rtc.c's
 *     rtc_now_ms() is RTC_EPOCH_MS + microtick(), where upstream reads Date.now(), so the
 *     CMOS time registers are a function of the instruction stream rather than of the host
 *     wall clock -- §7.3's last native-lane item).
 *   - `run_hardware_timers` IS NO LONGER DEFINED HERE.  M2-1 (HANDOFF=v86c) wired the
 *     seat: src/hw/machine.c owns it now and drives the PIT, the RTC and (when the
 *     machine has one) the ACPI timer, exactly as cpu.js:1924-1941 does.  The header
 *     comment of machine.c is the contract; this file keeps only the two functions that
 *     have nothing to do with devices.  The M1c behaviour -- a machine with NO device
 *     layer returns its argument unchanged -- survives INSIDE machine.c (its
 *     `!machine_ready` arm), which is why m1c_selftest.c still passes unchanged.
 *   - `cpu_event_halt` does nothing: upstream tells the device layer the CPU halted, and
 *     this port's device layer (src/hw/machine.c) has no use for the notification -- the
 *     halt itself is `cpu->in_hlt`, which main_loop and main.c read, and which
 *     pic.c's pic_call_irq clears.  UNTIL M2-1 that meant `sti; hlt` idled forever (no
 *     device could raise an IRQ); with the machine assembled it means what it means on
 *     the reference -- the poll `run_hardware_timers` runs the PIT and the RTC, the PIT
 *     raises IRQ0 when counter 0 rolls over, and the handler wakes the CPU.  A machine
 *     with NO device layer (machine_init was never called: the selftest's) still idles
 *     forever, and that is the M0 oracle's machine (its V86 config registers no
 *     devices).
 *   - `fwait` (fpu.rs:803) is upstream's empty body, verbatim; fpu.h's header note records
 *     why its definition sits here rather than in fpu.c.
 */

#include "exec_boundary.h"

#include "cpu_state.h"

/* ------------------------------------------------------------------ *
 * D-F2 (HANDOFF=v86c unit M2-1F): THE DETERMINISTIC VIRTUAL CLOCK, 1 INSTRUCTION = 1 us
 *
 * Upstream's `microtick` is THE HOST WALL CLOCK in milliseconds (main.js:223-240:
 * performance.now, Date.now, or process.hrtime), so upstream's timing is nondeterministic
 * by construction.  This port cannot be: the build path must be reproducible
 * (results/cpu-port-plan.md ss7.3) and the oracle contract is "the same image, the same
 * transcript".  The clock is therefore VIRTUAL and DETERMINISTIC -- a pure function of the
 * guest's own instruction stream.
 *
 * THE SCALE (this is the decision, stated once): ONE GUEST INSTRUCTION IS ONE MICROSECOND of
 * virtual time --
 *
 *     microtick() = cpu->instruction_counter / INSTRUCTIONS_PER_MILLISECOND   [MILLISECONDS]
 *
 * which is MILLISECONDS, upstream's unit and the unit all three consumers are written for:
 * pit.js:117-140 multiplies the clock by OSCILLATOR_FREQ (1193.18 kHz -> ticks, which is
 * only dimensionally right if the clock is ms), acpi.js:161 scales it by
 * PMTIMER_FREQ_SECONDS/1000, and rtc.js:290 takes `microtick() % 1000` as "the last
 * millisecond of a second".
 *
 * MEASURED CONSEQUENCE OF THE SCALE (M2-1's finding F2, now closed): M1c's seat returned the
 * RAW instruction counter, i.e. ONE MILLISECOND per instruction -- 1000x fast -- so a 10 ms
 * PIT reload fired every 10 instructions and the guest lived in its timer handler.  With
 * this scale the same 10 ms reload (0x2E9C = 11932 ticks) fires at 10001 instructions
 * (hw_selftest asserts exactly that number), a 65536-tick reload at ~54925, and the TSC
 * (cpu.rs:4365: microtick()*TSC_RATE, TSC_RATE = 1e6 = 1 GHz, cpu.rs:295-296) counts 1000
 * counts per instruction -- the three consumers are consistent with each other for the
 * first time.
 *
 * GUEST-VISIBLE CONSEQUENCE, declared rather than hidden: the machine's time base is the
 * guest's own execution.  An IDLE guest's clock STOPS, where upstream's keeps running
 * against the wall clock; and "one guest instruction per microsecond" is a time base, not a
 * performance promise.  A guest that measures elapsed time (rdtsc, the PIT, the PM timer)
 * sees a machine whose time advances exactly as fast as it executes -- deterministic, and
 * byte-identical run to run.
 * ------------------------------------------------------------------ */
#define INSTRUCTIONS_PER_MILLISECOND 1000.0

double microtick(void)
{
	return (double)cpu->instruction_counter / INSTRUCTIONS_PER_MILLISECOND;
}

void cpu_event_halt(void)
{
	/* upstream: the device layer is told the CPU halted (cpu.rs:41).  With no devices it
	 * has nothing to do; the halt itself is `cpu->in_hlt`, which the caller reads. */
}

/* ------------------------------------------------------------------ *
 * fpu.c's (M1b) FWAIT.  Upstream's body is empty (fpu.rs:803), so this seat is
 * bit-identical to fpu.c's would-be definition; the MOVE is blocked by
 * cpu_selftest.c's 9B recording stub (that file supplies its own `fwait`, and fpu.c is
 * in the selftest's source set) -- see results/m1b3-fpu.md's handoff item.
 * ------------------------------------------------------------------ */
void fwait(void)
{
}
