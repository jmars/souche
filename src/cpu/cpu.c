/*
 * cpu.c -- the interpreter run loop.
 *
 * Port of v86/src/rust/cpu/cpu.rs:3160-3172 (get_phys_eip), 3302-3312
 * (jit_run_interpreted), 3135-3300 (cycle_internal), 3417-3423
 * (do_many_cycles_native) and 3378-3415 (main_loop), MINUS ALL JIT -- see cpu.h for the
 * exact list of what that drops (tlb_code, the CachedCode state table, the hotness
 * counter, the profiler/opstats counters, the DEBUG `in_jit` flag).
 *
 * The two C-side divergences, both forced by the port's shape and both recorded in the
 * unit report:
 *
 *  D1  the fault plumbing.  Upstream threads page faults as Result<T,()>; this port uses
 *      the set-only `cpu->pagefault` of cpu_state.h.  The flag's LIVE WINDOW is therefore
 *      exactly one `run_instruction` dispatch: it is cleared at the top of cycle_internal,
 *      immediately before the dispatch, and -- R5F, R5's B1 -- immediately AFTER it.
 *      Never inside a helper or an instruction body.
 *
 *      The post-dispatch clear is the one that keeps the port faithful, and the reason is
 *      upstream's shape rather than a preference: upstream's `return_on_pagefault!`
 *      expands to `return` from the GENERATED `run`, so a fault -- delivered or not --
 *      cannot outlive the dispatch that raised it (cpu.rs:3321 calls
 *      `run_instruction(opcode | ...)` as a plain statement; the loop tail's
 *      `return_on_pagefault!(get_phys_eip())` at 3335 can therefore only ever fire on the
 *      FETCH's own error).  Without the post-dispatch clear the flag survives to that tail
 *      and the tail's abort fires on a *delivered* fault: `interpreted_run` returned early
 *      through the tail's fetch check, `instruction_counter += i` was skipped, and the
 *      assert at the end of cycle_internal fired --
 *      on the FIRST delivered fault whose handler shares the faulting instruction's page
 *      (the case a small kernel hits constantly), while a handler on another page hid it
 *      behind the loop's page-boundary break.  Every consumer of the instruction's fault
 *      lives inside the dispatch (the generated code's checks and exceptions.c's), so
 *      there is no reader of the flag between the dispatch's return and the fetch.
 *
 *  D2  `unimplemented_instruction` (gen/interpreter_c.js) stops the machine.  Upstream
 *      cannot express "this instruction has no implementation" -- the Rust would not
 *      compile -- so the port's incremental state needs a stop that is not a guest
 *      exception.  It is checked in the two loops and reported by the caller.
 *
 * A faithful quirk worth naming: upstream's `return_on_pagefault!(get_phys_eip())` at the
 * BOTTOM of the interpreted loop returns from the function *before*
 * `*instruction_counter += i`, so a mid-run fault loses the count of the instructions
 * already executed in that run.  That is reproduced here (the bare `return`) rather than
 * "fixed" -- it is unobservable without paging (M0's image and the nasm corpus both run
 * with CR0.PG clear), and changing it would be new semantics, not a port.
 */

#include "cpu.h"

#include <stdio.h>
#include <string.h>

#include "cpu_backend.h" /* S0: the engine seam -- cycle_internal's engine half routes here */
#include "cpu_state.h"
#include "debug.h"
#include "instructions.h"
#include "memory.h"
#include "modrm.h"
#include "misc_instr.h"
#include "paging.h"
#include "uop_cache.h" /* PERF-4/M2: the decoded-block cache, the plan's ss8 step 5 */

bool jit_block_boundary = false;

bool interpreted_unimplemented = false;
/* 384, not 160: call_interrupt_vector's gate-type stop carries the IDT base/limit, the
 * descriptor, the faulting rip's bytes and cr2/cr3 (exceptions.c), and at 160 the message was
 * CUT MID-EVIDENCE -- MEASURED in results/x86-64-idt64.md, where the reported instruction
 * bytes ended at "8e d0 8e e0 8".  The larger buffer is only ever touched by a stop. */
static char unimplemented_name[384];

void unimplemented_instruction(const char *name)
{
	/* S5a: THE FIRST STOP WINS.  A stop is raised as the CAUSE of an abort, and the abort's
	 * CONSEQUENCE can raise a second one -- a long-mode walk stops by name ("a 1 GiB page is
	 * S11's"), aborts the instruction through cpu->pagefault, and the page-fault DELIVERY then
	 * stops too ("idtr_size too small for the vector").  Before this guard the second stop
	 * overwrote the first and the machine reported the SYMPTOM: MEASURED on gate-long-mode.sh's
	 * 1 GiB / 2 MiB mutation probes, which found no walk stop at all.  The machine stops either
	 * way; what changes is that the reason a reader gets is the first one. */
	if (interpreted_unimplemented) return;
	v86c_dbg_on_stop(name);
	interpreted_unimplemented = true;
	snprintf(unimplemented_name, sizeof(unimplemented_name), "%s", name);
}

const char *unimplemented_instruction_name(void)
{
	return unimplemented_name;
}

/* cpu.rs:1901 */
void after_block_boundary(void)
{
	jit_block_boundary = true;
}

/* cpu.rs:3160-3172.  `eip_phys` caches the *host* address of the current code page, as
 * the XOR of the guest physical page and the virtual page (`translate_address_fetch(eip)
 * ^ eip`), so one XOR yields the physical address of any eip inside the page.  The port
 * keeps the trick; only the "host" in "host address" is gone (translate_address returns
 * a guest physical address here -- paging.h). */
uint32_t get_phys_eip(void)
{
	/* S3: the eip is 64 bits now and this function generalises UNCHANGED (plan ss4's entry
	 * for instruction_pointer): the cache is still `translate_fetch(eip) ^ eip`, whose XOR
	 * cancels the page bits, so one XOR of a later eip in the same page yields the physical
	 * address.  The eip's page compare is done at the field's own width; the RESULT is a
	 * `uint32_t` and is projected ONCE, at the mem8 boundary below (plan ss5.1). */
	uint64_t eip = cpu->instruction_pointer;

	if ((eip & ~(uint64_t)0xFFFu) != cpu->last_virt_eip) {
		uint32_t phys = translate_address_fetch(eip);
		if (cpu->pagefault) return 0;
		cpu->eip_phys = (uint64_t)phys ^ eip;
		cpu->last_virt_eip = eip & ~(uint64_t)0xFFFu;
	}

	/* THE PROJECTION (plan ss5.1): the physical model is 32 bits wide, so a fetch address
	 * is the low half of the cache XOR.  In 32-bit mode the two are the same value by
	 * construction -- the high half is asserted zero at the batch boundary (cpu_state.h's
	 * cpu_assert_address_widening) -- so this cast is the model's bound made explicit, not a
	 * truncation of a live 64-bit address (that case is S11's named refusal). */
	uint32_t phys_addr = (uint32_t)(cpu->eip_phys ^ eip);
	/* AUDIT-1 S1: the old DBG_ASSERT(!in_mapped_range(phys_addr)) was guest-reachable (a
	 * guest points EIP at the VGA hole with one jmp; MEASURED rc 134 on the eip-hole
	 * image) and it conflated two cases.  A fetch whose physical address is inside guest
	 * RAM -- the VGA hole included -- is one the RELEASE reference executes from the raw
	 * bytes (memory::mem8.offset(..)), which is exactly what the return value feeds; the
	 * assert is gone and the raw read stands.  A fetch PAST the guest's RAM the release
	 * does not survive either (the nasm suite's pinned jump fixture is its SIGSEGV), so
	 * this port names that instead of reading host memory. */
	if (phys_addr >= cpu->memory_size) {
		char stop[160];
		snprintf(stop, sizeof(stop),
		         "instruction fetch at physical 0x%x, past the end of guest RAM"
		         " (0x%x) -- the release reference crashes here",
		         (unsigned)phys_addr, (unsigned)cpu->memory_size);
		unimplemented_instruction(stop);
	}
	return phys_addr;
}

/* cpu.rs:3302-3312 (jit_run_interpreted), with the JIT checks dropped.
 *
 * NOT `static` SINCE M2 (PERF-4): src/cpu/uop_cache.c's executor is a REPLACEMENT for this
 * loop (plan ss8 step 5), and it falls back to this exact function for a whole reference
 * batch whenever a block cannot be decoded -- so the body must stay reachable and, more
 * importantly, must stay EXACTLY this: the fallback's counter update and its break
 * conditions are the reference's. */
void interpreted_run(uint32_t phys_addr)
{
	/* AUDIT-1 S1: same resolution as get_phys_eip's (this loop's own fetch goes through
	 * that function at the tail).  An in-RAM address -- the VGA hole included -- runs the
	 * raw bytes, as the release reference does; past the guest's RAM the port names the
	 * stop instead of reading host memory. */
	if (phys_addr >= cpu->memory_size) {
		char stop[160];
		snprintf(stop, sizeof(stop),
		         "instruction fetch at physical 0x%x, past the end of guest RAM"
		         " (0x%x) -- the release reference crashes here",
		         (unsigned)phys_addr, (unsigned)cpu->memory_size);
		unimplemented_instruction(stop);
		/* S5a: AND STOP.  Naming the fetch and then falling into the loop below reads
		 * `mem8[phys_addr]` at an address PAST the guest's RAM -- a HOST out-of-bounds read,
		 * i.e. the port crashing where it has just said the reference crashes.  MEASURED: with
		 * a guest image whose page tables map the fetched page above RAM (the walk's own
		 * mutation probe, gate-long-mode.sh), this was a SIGSEGV of the HOST with no output at
		 * all.  `jit_block_boundary` and the rest of the entry are irrelevant once the machine
		 * is stopping, so the return is the whole fix. */
		return;
	}

	jit_block_boundary = false;
	int32_t i = 0;

	for (;;) {
		i += 1;
		uint64_t start_eip = cpu->instruction_pointer;
		int32_t opcode = (int32_t)mem8[phys_addr];
		/* through the fetch-cursor store (cpu_state.h), like every other producer of an
		 * eip: at 0xFFFFFFFF upstream's i32 wraps to 0 and a bare `+= 1` would instead
		 * carry into bit 32, which the batch-end invariant would rightly stop on */
		set_instruction_pointer(cpu->instruction_pointer + 1);
		DBG_ASSERT(cpu->prefixes == 0);
		/* the dispatch-entry clear: the instruction starts with the flag down (D1) */
		cpu->pagefault = false;
		/* S5a: THE THREE-VALUED KEY (plan ss3.6.1).  dispatch_os_index(opcode) is 0/1/2 for a
		 * 16/32/64-bit operand size and is a function of the OPCODE as well as the prefixes,
		 * because long mode's push/pop/pushf/popf/jmp default to a 64-bit operand size with no
		 * REX byte at all (modrm.c's note).  Outside 64-bit mode it is `is_osize_32() ? 1 : 0`
		 * -- the expression this line used to carry, so the 32-bit dispatch key is unchanged
		 * byte for byte. */
		/* the EXECUTION watch: before the instruction runs, so the registers it prints are
		 * the ones this instruction sees (results/x86-64-reach.md).  One load and one branch
		 * when the tools are off, the same shape as the ring hook below. */
		if (v86c_dbg_break_on) v86c_dbg_break_hit(start_eip);
		run_instruction(opcode | dispatch_os_index(opcode) << 8);
		/* the NEW-MINIMUM ring (results/x86-64-rsplow.md): AFTER the instruction, so the rsp
		 * recorded is the one the instruction LEFT -- a push or a `sub rsp` lowers it -- and
		 * start_eip is the instruction's own address, which is what a kallsyms lookup names.
		 * Recording BEFORE the instruction (where the break hook sits) would attribute every
		 * step to the instruction that follows it.  One load and one branch when off. */
		if (v86c_dbg_rsplow_on) v86c_dbg_rsplow_tick(start_eip, cpu->reg64[4]);
		if (v86c_dbg_ring_on) {
			uint8_t b[16];
			int k;
			for (k = 0; k < 16; k++) b[k] = (uint8_t)mem8[phys_addr + (uint32_t)k];
			v86c_dbg_ring_record(start_eip, b, 16);
		}
		/* and the SECOND clear, which ends the dispatch's fault window (D1 above): from
		 * here the only reader is the loop tail's fetch check, which must see only the
		 * fetch's own fault.  A DELIVERED fault -- flag set, handler entered, eip left at
		 * the handler -- must therefore not abort the run (R5's B1; the same-page-handler
		 * case died here).  `run_instruction` is the port's `gen::interpreter::run`, and
		 * upstream's `return` on error cannot outlive it either. */
		cpu->pagefault = false;
		DBG_ASSERT(cpu->prefixes == 0);

		/* the port's stop for an opcode with no implementation yet (this file's D2) */
		if (interpreted_unimplemented) break;
		/* REBOOT-1: the reference's batch ends here too -- a reboot moved eip to the
		 * reset vector, and the rest of this batch is not boot 1's stream. */
		if (cpu->reboot_requested) break;

		/* S3: the two eip tests are at the field's own width (they were `(uint32_t)` casts
		 * of an i32 field).  For every 32-bit value the comparison is the same one; the
		 * widening is what lets a 64-bit eip be tested without a projection. */
		if (jit_block_boundary || (start_eip >> 12) != (cpu->instruction_pointer >> 12)
			/* Limit the number of iterations, as jumps within the same page are not
			 * counted as block boundaries for the interpreter, but only on the next
			 * backwards jump */
			|| (i >= (int32_t)INTERPRETER_ITERATION_LIMIT && start_eip >= cpu->instruction_pointer)) {
			break;
		}

		cpu->previous_ip = cpu->instruction_pointer;
		phys_addr = get_phys_eip();
		/* upstream returns HERE, before the counter update (see the file header).  The
		 * flag can only be the FETCH's own: the dispatch's window ended above at the
		 * second clear, and get_phys_eip's own translate_address_fetch sets it here. */
		if (cpu->pagefault) return;
		/* S5a: the same host-OOB door as the entry's, one iteration later: get_phys_eip raises
		 * the named stop for a fetch PAST guest RAM and returns the address anyway, and the
		 * next iteration would index mem8 with it.  `break` (not `return`) so the batch's
		 * instruction count is kept -- the counter update below is upstream's and the
		 * instructions already executed are real. */
		if (interpreted_unimplemented) break;
	}

	cpu->instruction_counter += (uint32_t)i;
}

/* cpu.rs:3135-3300, JIT arms dropped.  What is left is the `else` arm: the interrupt the
 * JIT lookup could have short-circuited does not exist in an interpreter-only port. */
void cycle_internal(void)
{
	/* S3's high-half invariant, ONCE PER BATCH (cpu_state.h): the batch boundary is the one
	 * place every engine passes through, so a widened arithmetic that leaked bits above 32
	 * -- in any instruction, in either engine -- stops the machine BY NAME here instead of
	 * reaching a mem8 index as a wrong address.  Cost is one compare per batch: 7 fields
	 * against zero, and the mode test short-circuits it entirely in long mode (nothing can
	 * set EFER.LMA before S5, so nothing short-circuits today -- stated, not implied). */
	cpu_assert_address_widening("cycle_internal");

	uint64_t initial_eip = cpu->instruction_pointer;

	cpu->pagefault = false;
	cpu->previous_ip = initial_eip;

	uint32_t phys_addr = get_phys_eip();
	if (cpu->pagefault) return;

	uint64_t initial_instruction_counter = cpu->instruction_counter;

	/* S0 (results/x86-64-plan.md ss3.4): THE ENGINE HALF, THROUGH THE SEAM.
	 * cpu_backend_dispatch() is this call and nothing else: it asks whether the selected
	 * backend can run the machine's mode (a named stop if not -- inert in S0, because no
	 * engine declares a mode the machine can be in), then calls that backend's ->run_batch.
	 *
	 * The three instances are today's two functions plus one flag discipline (cpu_backend.c):
	 * `interp` IS interpreted_run, `uop` is uop_run with the compiled path held down,
	 * `uopjit` IS uop_run.  With the shipped default the dispatch therefore calls uop_run
	 * exactly where this line used to, and `--uop-cache off` reaches interpreted_run exactly
	 * where the old `else` did -- the seam is INERT, and its gate is the whole battery. */
	cpu_backend_dispatch(phys_addr);

	/* upstream's dbg_assert!(*instruction_counter != initial_instruction_counter) --
	 * the interpreted loop always executes at least one instruction, so this is the
	 * check that it did.
	 *
	 * WITH THE CACHE IT IS "progress was made", NOT "the counter moved": a reference BATCH can
	 * span several cycle_internal calls when a block ends without ending a batch (only `sti`
	 * can do that -- src/cpu/uop_cache.c's header), and there the counter legitimately stays
	 * put while instructions are executed.  uop_pending is that progress.  With --uop-cache
	 * off the second conjunct is dead and this is upstream's assert exactly.
	 *
	 * REBOOT-FIX-1: the third conjunct is the reboot.  reset_cpu zeroes the counter
	 * mid-batch and the unwind flushes only the trigger batch's own count, so the
	 * assert fires iff that count equals the cumulative count at this cycle's entry
	 * -- batch geometry a guest image controls (results/reboot-fix-1.md's geom
	 * image hits it exactly, MEASURED, aborting all three configs pre-fix).  The
	 * reboot IS progress: upstream's own assert is debug-only (dbg.rs maps
	 * dbg_assert! to debug_assert!) and its release build simply proceeds past the
	 * reboot, which is the behaviour this keeps while the port ships asserts live. */
	/* S3's high-half invariant, ALSO AT THE BATCH's END.  The entry call above sees the state
	 * a previous batch left behind; this one sees what THIS batch produced, which is the
	 * difference that matters for an engine whose leak is transient INSIDE a batch -- the
	 * compiled arm's ip arithmetic, whose leak a later uop's own ip store can repair before
	 * the batch ends.  MEASURED (the S3 unit's own falsifier): with the recipes' ip
	 * arithmetic back at its pre-widening width, the entry call sees nothing and this one
	 * names the field.  Two compares per batch: 7 fields against zero. */
	cpu_assert_address_widening("cycle_internal (batch end)");

	/* S5a: THE NAMED STOP IS PROGRESS TOO.  A BATCH can now end with nothing executed and no
	 * counter movement -- `interpreted_run` returns immediately when the fetch is past guest
	 * RAM, which it must (that path used to read `mem8[phys]` out of bounds; cpu.c's entry
	 * note has the measurement) -- and the machine is stopping, not spinning.  Without the
	 * fourth conjunct that legitimate stop ABORTED THE HOST in this assert. */
	DBG_ASSERT(cpu->instruction_counter != initial_instruction_counter
	           || uop_pending != 0
	           || cpu->reboot_requested
	           || interpreted_unimplemented);
	(void)initial_eip;
}

/* PERF-4/M2: the window's end, published for the uop cache (cpu.h).  The initial value is
 * unreachable so a cache run outside do_many_cycles_native -- the selftests drive
 * cycle_internal directly -- is not cut short by a stale window.
 *
 * U64-COUNTER: this stays u32 ON PURPOSE, even though instruction_counter is now u64.  The
 * window's end is compared to the counter's LOW 32 BITS in both uop_budget_reached
 * (uop_cache.c) and the JIT's emitted budget check (jit_codegen.c), using the same
 * wrapped-signed-difference test the reference's u32 wrapping_sub implies -- and that test
 * is only correct because BOTH operands live in the same 32-bit domain.  Keeping the end a
 * u32 (the low 32 bits of the counter plus LOOP_COUNTER, wrapped) preserves the domain; the
 * low 32 bits wrap exactly as the old u32 counter did, so the staircase is unchanged. */
uint32_t cpu_cycle_budget_end = 0xFFFFFFFFu;

/* cpu.rs:3417-3423 */
void do_many_cycles_native(void)
{
	uint64_t initial_instruction_counter = cpu->instruction_counter;
	cpu_cycle_budget_end = (uint32_t)(initial_instruction_counter + (uint32_t)LOOP_COUNTER);

	while ((cpu->instruction_counter - initial_instruction_counter) < (uint64_t)LOOP_COUNTER
		&& !cpu->in_hlt
		&& !interpreted_unimplemented
		&& !cpu->reboot_requested) {
		cycle_internal();
	}
}

/* cpu.rs:3378-3415.  The `js::` imports (microtick, run_hardware_timers) are the thin
 * timer layer's (exec_boundary.c, M2 -- see cpu.h). */
double main_loop(void)
{
	double start = microtick();

	if (cpu->in_hlt) {
		if ((cpu->flags & FLAG_INTERRUPT) != 0) {
			double t = run_hardware_timers(cpu->acpi_enabled, start);
			handle_irqs();
			if (cpu->in_hlt) {
				return t;
			}
		}
		else {
			/* dead */
			return 100.0;
		}
	}

	for (;;) {
		do_many_cycles_native();

		double now = microtick();
		double t = run_hardware_timers(cpu->acpi_enabled, now);
		handle_irqs();
		if (cpu->in_hlt) {
			return t;
		}
		if (interpreted_unimplemented) {
			return t;
		}
		/* REBOOT-1: return PROMPTLY on the reboot request -- the boot re-entry is
		 * host-side and boot-mode-specific (main.c), and no guest instruction of
		 * the next boot may run before the boot state is re-established and the
		 * code caches are cleared.  The do_many_cycles_native batch has already
		 * ended (reset_cpu zeroed the instruction counter, so the batch window
		 * underflows and closes right after the trigger). */
		if (cpu->reboot_requested) {
			return t;
		}

		if (now - start > TIME_PER_FRAME) {
			break;
		}
	}

	return 0.0;
}

/* ------------------------------------------------------------------ *
 * the timestamp counter -- cpu.rs:4357-4412
 *
 * The imprecise-browser workaround (TSC_ENABLE_IMPRECISE_BROWSER_WORKAROUND, true
 * upstream and kept true here) exists because a browser's performance.now() can return
 * the same value for several consecutive calls; the port's microtick can do the same for
 * MANY consecutive instructions of one interpreted batch (it counts instructions, and
 * the counter is updated once per batch), so the workaround is load-bearing here for a
 * different reason and is ported verbatim rather than dropped.  It is deterministic:
 * every input is a function of the instruction sequence.
 *
 * CONSEQUENCE FOR THE ORACLE, stated rather than hidden: the REFERENCE's rdtsc reads a
 * wall clock (js::microtick), so no fixture that executes rdtsc can be compared for its
 * TSC value; the nasm corpus does not contain one (MEASURED: no test in
 * v86/tests/nasm executes 0F 31), and the M0 image does not either.  The port's
 * value is reproducible run to run, which is the property the port's contract needs.
 * ------------------------------------------------------------------ */
static uint64_t tsc_last_value = 0;
static uint64_t tsc_resolution = 0xFFFFFFFFFFFFFFFFull; /* u64::MAX */
static uint64_t tsc_number_of_same_readings = 0;
static uint64_t tsc_speed = 1;
static uint64_t tsc_offset = 0;
/* upstream gates this on #[cfg(debug_assertions)] (cpu.rs:306-308); the port's DBG_ASSERT
 * is the same gate, so the variable is kept unconditionally and the assert is what
 * NDEBUG compiles out. */
static uint64_t tsc_last_extra = 0;

void set_tsc(uint32_t low, uint32_t high)
{
    uint64_t new_value = (uint64_t)low | ((uint64_t)high << 32);
    uint64_t current_value = read_tsc();
    tsc_offset = current_value - new_value;
}

uint64_t read_tsc(void)
{
    uint64_t value = (uint64_t)(microtick() * TSC_RATE) - tsc_offset;

    /* TSC_ENABLE_IMPRECISE_BROWSER_WORKAROUND is true (cpu.rs:302), so the rest runs */

    if (value == tsc_last_value) {
        /* If the clock returns the same value as last time, extrapolate based on the
         * number of reads between the last two changes */
        tsc_number_of_same_readings += 1;
        uint64_t extra = (tsc_number_of_same_readings * tsc_resolution) / tsc_speed;
        extra = extra < tsc_resolution - 1 ? extra : tsc_resolution - 1;
        tsc_last_extra = extra;
        return value + extra;
    }

    if (tsc_last_extra != 0) {
        /* upstream dbg_log!s the jump here; the log channel does not exist in the port */
        DBG_ASSERT(tsc_last_extra < tsc_resolution); /* "XXX: Overshot tsc" */
        tsc_last_extra = 0;
    }

    uint64_t d = value - tsc_last_value;
    /* upstream dbg_log!("rdtsc resolution: {}", d) dropped */
    if (d < tsc_resolution) {
        tsc_resolution = d;
    }
    tsc_last_value = value;
    if (tsc_number_of_same_readings != 0) {
        tsc_speed = tsc_number_of_same_readings;
        tsc_number_of_same_readings = 0;
    }

    return value;
}

void store_current_tsc(void) { cpu->current_tsc = read_tsc(); }
