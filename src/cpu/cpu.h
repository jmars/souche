/*
 * cpu.h -- the interpreter's run loop and the machine boundary it sits on.
 *
 * Port of v86/src/rust/cpu/cpu.rs:3131-3140 (run_instruction*),
 * 3135-3300 (cycle_internal), 3302-3312 (jit_run_interpreted), 3378-3415 (main_loop),
 * 3417-3423 (do_many_cycles_native), 1901 (after_block_boundary).
 *
 * THE ONE STRUCTURAL DIFFERENCE FROM UPSTREAM: there is no JIT.  cycle_internal's
 * tlb_code lookup, the CachedCode state table, the RUN_FROM_CACHE / RUN_INTERPRETED_*
 * decision, `jit_increase_hotness_and_maybe_compile`, `get_phys_eip`'s
 * `CHECK_JIT_STATE_INVARIANTS`, the profiler/opstats counters and the DEBUG `in_jit`
 * flag are all dropped -- what remains is upstream's interpreter arm, and
 * `jit_run_interpreted` is the whole body of the loop.  The name is kept as
 * `interpreted_run` so a reader can see it is the same function.
 *
 * `jit_block_boundary` keeps upstream's name: it is set by after_block_boundary()
 * (which the GENERATED dispatcher calls, so the name is part of the emitter's contract)
 * and read by the loop's break test.
 */

#ifndef V86C_CPU_H
#define V86C_CPU_H

#include <stdbool.h>
#include <stdint.h>

/* cpu.rs:78, 81, 293 */
#define INTERPRETER_ITERATION_LIMIT 100001u
#define TIME_PER_FRAME 1.0
#define LOOP_COUNTER 100003

/* cpu.rs:300 -- set by after_block_boundary(), cleared at the top of each interpreted
 * run.  JIT-only upstream *in name*, but the interpreter's break test reads it too. */
extern bool jit_block_boundary;

/* ------------------------------------------------------------------ *
 * The stop condition for "the emulator cannot execute this" (the emitter's
 * unimplemented_instruction, gen/interpreter_c.js).
 *
 * It is deliberately NOT the page-fault channel (cpu_state.h): a page fault is an
 * emulated guest event with a defined delivery; this is the emulator itself being
 * incomplete.  It stops the machine -- so a run that reached an unported opcode produces
 * NO state rather than a plausible wrong one -- and records the upstream-qualified symbol
 * for the caller to report.
 * ------------------------------------------------------------------ */
extern bool interpreted_unimplemented;
void unimplemented_instruction(const char *name);
const char *unimplemented_instruction_name(void);

/* ------------------------------------------------------------------ *
 * cpu.rs:3131-3133 (the two generated dispatchers; the emitter defines them)
 * ------------------------------------------------------------------ */
void run_instruction(int32_t opcode);
void run_instruction0f(int32_t opcode);

/* cpu.rs:1901 */
void after_block_boundary(void);

/* THE LOOP_COUNTER WINDOW'S ABSOLUTE END (PERF-4/M2).  do_many_cycles_native's loop test is
 * `cpu->instruction_counter - initial < LOOP_COUNTER` (cpu.rs:3417-3423), and the reference
 * evaluates it once per INTERPRETER BATCH -- because cycle_internal IS one batch there.  A
 * uop cache runs several reference batches inside one cycle_internal, so it must stop at the
 * same place the reference's per-batch test would have, or the outer loop overshoots the
 * window and main_loop runs the hardware timers at a different instruction count (which the
 * guest can see: MEASURED, the 200M boot ends 2 instructions early).  do_many_cycles_native
 * publishes the window's end here; src/cpu/uop_cache.c reads it after every batch it ends.
 * Only the cache reads it -- with --uop-cache off this variable is written and never read,
 * and the interpreter's behaviour is bit-for-bit what it was. */
extern uint32_t cpu_cycle_budget_end;

/* cpu.rs:3160-3312 */
uint32_t get_phys_eip(void);
void cycle_internal(void);
void do_many_cycles_native(void);

/* THE REFERENCE'S OWN INTERPRETED BATCH (cpu.rs:3302-3312).  Exposed since M2 (PERF-4):
 * src/cpu/uop_cache.c's uop cache is a REPLACEMENT for this call inside cycle_internal (the
 * plan's ss8 step 5) and falls back to it VERBATIM for a reference batch it cannot decode. */
void interpreted_run(uint32_t phys_addr);

/* cpu.rs:3378-3415 */
double main_loop(void);

/* ------------------------------------------------------------------ *
 * cpu.rs:4357-4412 -- the timestamp counter (M1c).
 *
 * The TSC is a function of the timer layer's `microtick` (cpu.rs:4365's
 * `js::microtick() * TSC_RATE`), which is why it lands with the run loop: in this port
 * microtick is a DETERMINISTIC VIRTUAL CLOCK -- MILLISECONDS at 1 guest instruction = 1
 * microsecond (exec_boundary.c, D-F2) -- so read_tsc is deterministic AND scaled right:
 * with TSC_RATE = 1e6 (1 GHz) it counts 1000 counts per instruction, the rate cpuid
 * advertises.  The reference's clock is the host wall clock, so its TSC is neither
 * deterministic nor comparable run to run.
 * `set_tsc` is WRMSR's IA32_TIME_STAMP_COUNTER handler, NOT a reset-time call: see
 * cpu_state.c's note on reset_cpu's set_tsc(0,0).
 * ------------------------------------------------------------------ */
#define TSC_RATE 1000000.0 /* cpu.rs:295 */
uint64_t read_tsc(void);
void set_tsc(uint32_t low, uint32_t high);
void store_current_tsc(void);

#endif /* V86C_CPU_H */
