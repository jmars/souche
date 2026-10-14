/*
 * exec_boundary.h -- the M2 DEVICE/TIMER SEAT, and nothing else.
 *
 * WHAT THIS FILE WAS.  Until M1c it declared the exception record and carried the whole
 * M1c/M2 boundary: the exception delivery, the segment machinery, the PIC poll, the
 * device port bridge and the timer layer.  M1c landed all but the last, so what remains
 * is
 *   - `microtick` and `cpu_event_halt` (cpu.rs:37-41's js:: imports), which M2-1 left
 *     here -- M2-1F (D-F2) fixed the clock's UNIT at the definition in exec_boundary.c:
 *     1 guest instruction = 1 MICROSECOND of virtual time, so microtick() is MILLISECONDS
 *     and is what the PIT, the ACPI PM timer and the TSC all consume; and
 *   - `run_hardware_timers` (cpu.rs's timer poll) is DECLARED in instructions.h and
 *     DEFINED in src/hw/machine.c since M2-1, together with device_raise_irq /
 *     device_lower_irq;
 *   - fwait (fpu.rs:803), which cannot move to fpu.c while cpu_selftest.c (a file this
 *     unit does not otherwise need to touch) supplies its own recording definition and
 *     fpu.c IS in that binary's source set.
 *
 * THE EXCEPTION RECORD MOVED TO exceptions.h (guest_exception, now defined by
 * exceptions.c): its meaning narrowed from "the vector the guest took" to "the vector of
 * a fault the machine stopped on because the delivery could not proceed", because
 * delivery EXISTS now.  main.c includes exceptions.h.
 */

#ifndef V86C_EXEC_BOUNDARY_H
#define V86C_EXEC_BOUNDARY_H

#include <stdint.h>

#include "cpu.h"  /* the timer layer's declarations are cpu.h's (via instructions.h) */
#include "fpu.h"  /* fwait's owner-of-record */

#endif /* V86C_EXEC_BOUNDARY_H */
