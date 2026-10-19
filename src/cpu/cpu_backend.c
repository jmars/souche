/*
 * cpu_backend.c -- the engine seam's instances, its resolver and the dispatcher.
 *
 * See cpu_backend.h for the contract, the asymmetry of `auto` versus the three explicit
 * names, and the list of what this slice deliberately does NOT land.  This file is small on
 * purpose: it holds the three instances, the two directions between "engine" and "the two
 * existing flags", and cpu_backend_dispatch().
 *
 * S0'S GATE IS THAT NOTHING ELSE MOVES.  The three instances are therefore the EXISTING
 * functions -- interpreted_run and uop_run -- not re-implementations of them, and the only
 * new code on the run path is the mode query and one indirect call per reference batch (a
 * few cycles against a batch's thousands of instructions).
 */

#include "cpu_backend.h"

#include <stdio.h>
#include <string.h>

#include "cpu.h"        /* interpreted_run, unimplemented_instruction */
#include "jit_mem.h"    /* jit_enabled, jit_mem_init -- the compiled path's readiness */
#include "uop_cache.h"  /* uop_run, uop_cache_init, uop_cache_clear, uop_stats_print */

/* ------------------------------------------------------------------ *
 * the instances
 * ------------------------------------------------------------------ */

static bool backend_interp_init(void);
static bool backend_uop_init(void);
static bool backend_uopjit_init(void);
static void backend_uop_run(uint32_t phys_addr);
static void backend_clear_caches(void);

/* interp: the generated switch, one instruction at a time (cpu.c:126).  Its init only has to
 * settle the compiled-path question, which is a REFUSAL this engine inherits rather than an
 * allocation it makes -- see backend_interp_init.
 *
 * S5a: NOT `const` ANY MORE.  This is the one backend that gains a capability at run time
 * (`--long-mode`, cpu_backend_enable_long_mode below): the capability word is the CONTRACT
 * (ss3.3), so it is the contract's own field that changes rather than a second global that
 * every reader would have to consult.  The other two stay const -- an engine that cannot run
 * a mode must not be able to claim it by accident. */
static cpu_backend_t backend_interp = {
	"interp",
	CPU_MODES_32BIT,
	backend_interp_init,
	interpreted_run,
	backend_clear_caches,
	uop_stats_print,
};

/* uop: uop_run with the compiled path disabled FOR THIS INSTANCE (ss3.4's table).  The
 * wrapper is the enforcement: `jit_enabled` is the switch the decode-time compiler reads
 * (uop_cache.c:995), so an instance that owns "no compiled path" has to hold it down for the
 * duration of its own run rather than trust a global that a later flag or a fallback could
 * have moved.  Cost: two loads and two stores per REFERENCE BATCH. */
static const cpu_backend_t backend_uop = {
	"uop",
	CPU_MODES_32BIT,
	backend_uop_init,
	backend_uop_run,
	backend_clear_caches,
	uop_stats_print,
};

/* uopjit: uop_run with the compiled path enabled -- the shipped default, and the function
 * itself: there is nothing to hold down, so there is nothing to wrap. */
static const cpu_backend_t backend_uopjit = {
	"uopjit",
	CPU_MODES_32BIT,
	backend_uopjit_init,
	uop_run,
	backend_clear_caches,
	uop_stats_print,
};

static const cpu_backend_t *const backend_all[] = {
	&backend_interp,
	&backend_uop,
	&backend_uopjit,
};

#define BACKEND_COUNT ((uint32_t)(sizeof(backend_all) / sizeof(backend_all[0])))

/* ------------------------------------------------------------------ *
 * the two directions between an instance and the two existing flags
 *
 * `engine_for_flags` is the derivation `auto` performs (writes nothing); `force_flags` is what
 * an EXPLICIT name does (so the rest of the machine agrees with the engine that was named).
 * Both directions exist because the selftest's round trip is what proves a selection is not a
 * no-op: flags -> engine -> the same engine.
 * ------------------------------------------------------------------ */

static const cpu_backend_t *engine_for_flags(void)
{
	if (!uop_cache_enabled) return &backend_interp;
	if (!jit_enabled) return &backend_uop;
	return &backend_uopjit;
}

static void force_flags(const cpu_backend_t *backend)
{
	uop_cache_enabled = backend != &backend_interp;
	jit_enabled = backend != &backend_interp && backend != &backend_uop;
}

/* ------------------------------------------------------------------ *
 * init: readiness, not a factory
 * ------------------------------------------------------------------ */

/* The interpreter allocates nothing.  What it cannot do is run the COMPILED path, and this
 * machine refuses that in exactly one place with one message: jit_mem_init's, for a JIT asked
 * for while the uop cache is off (the cache owns the dirty-page bitmap the compiled blocks'
 * invalidation needs).  Keeping that call here is what keeps `--uop-cache off`'s stderr
 * byte-identical to the pre-seam machine; the verdict is "no compiled path", which is not a
 * reason to refuse the ENGINE. */
static bool backend_interp_init(void)
{
	if (jit_enabled && !jit_mem_init()) jit_enabled = false;
	return true;
}

/* The decoded-block cache's structures.  uop_run builds them lazily too (`if (!uop_ready &&
 * !uop_cache_init())`, uop_cache.c:1252) with the same function and the same inputs -- doing
 * it here makes the failure a REFUSAL the machine can name and fall back from, instead of a
 * per-cycle silent fallback to the interpreter. */
static bool backend_uop_init(void)
{
	return uop_cache_init();
}

/* The executable-memory pool.  jit_mem_init builds the cache's bitmap itself (it is a
 * precondition, not an optimization), and returns the PLATFORM PROBE's verdict -- a false
 * here means "the compiled path cannot exist on this host", which is exactly the contract's
 * "refuse, and the machine picks the previous backend and says so". */
static bool backend_uopjit_init(void)
{
	return jit_mem_init();
}

static void backend_uop_run(uint32_t phys_addr)
{
	bool compiled_path_was_enabled = jit_enabled;
	jit_enabled = false;
	uop_run(phys_addr);
	jit_enabled = compiled_path_was_enabled;
}

/* The code caches are the MACHINE's, not one engine's: every guest write goes through the
 * same invalidation hook whatever engine is selected (src/cpu/memory.c, paging.c, string.c),
 * so re-entering the boot has to drop them for every engine.  This is the pre-seam
 * reboot_prepare_reentry body, moved behind the contract. */
static void backend_clear_caches(void)
{
	uop_cache_clear();  /* drops the compiled blocks too (uop_cache.c) */
	jit_abort_flag = 0; /* the abort's consumer may never have run */
}

/* ------------------------------------------------------------------ *
 * selection
 * ------------------------------------------------------------------ */

/* NULL until something selects; cpu_backend_current() then derives, WITHOUT latching, so a
 * caller that never selects still gets the flags' own answer. */
static const cpu_backend_t *backend_selected = NULL;

const cpu_backend_t *cpu_backend_select(const char *name)
{
	const cpu_backend_t *backend;

	if (name == NULL || name[0] == '\0' || strcmp(name, "auto") == 0) {
		/* the derivation: the two flags ARE the answer, and nothing is written */
		backend = engine_for_flags();
		backend_selected = backend;
		return backend;
	}

	backend = NULL;
	for (uint32_t i = 0; i < BACKEND_COUNT; i++) {
		if (strcmp(name, backend_all[i]->name) == 0) {
			backend = backend_all[i];
			break;
		}
	}
	if (backend == NULL) return NULL;

	cpu_backend_adopt(backend);
	return backend;
}

/* ------------------------------------------------------------------ *
 * S5a: --long-mode, THE ONE CAPABILITY THAT IS SWITCHED AT RUN TIME
 *
 * Plan ss6 S5's flag.  It exists because long mode is a property of the CPU MODEL but a
 * BACKEND CAPABILITY (ss3.3), and the two must be able to disagree: `--long-mode off` (the
 * default, and the shipped machine) is the 32-bit CPU, and a guest that asks for IA-32e is
 * refused by name.  `--long-mode on` declares the capability on the ONE backend that has an
 * implementation -- the interpreter -- which is also why turning it on SELECTS that engine:
 * a machine whose capability word says long mode and whose decoder is the 32-bit uop cache
 * would be a false declaration, the exact defect class ACPI-FADT cost this tree.
 *
 * The uop cache and the JIT do NOT get the bit.  Their long-mode arms are S13's, and until
 * then the refusal they produce is correct rather than a limitation to work around
 * (uop_cache.c's uop_run carries the same refusal a second time, at the decoder).
 *
 * IT DOES NOT SELECT THE ENGINE, and that is the seam's own asymmetry (cpu_backend.h): an
 * explicit `--cpu-engine` WINS over the flags whatever the argument order.  The caller
 * declares the capability here and adopts the interpreter only if no engine was NAMED; if one
 * was, it stands and the S2 transition refusal names it.  MEASURED as the defect that made
 * this split necessary: with the adopt inside this function, `--long-mode on --cpu-engine
 * uopjit` silently ran the INTERPRETER and never refused -- the flag moved the machine to an
 * engine the user did not ask for, which is the seam's false-declaration class.
 *
 * Returns false for an engine that has no long-mode implementation, so a caller can say so.
 * ------------------------------------------------------------------ */
bool cpu_backend_enable_long_mode(void)
{
	backend_interp.modes |= (CPU_MODE_LONG | CPU_MODE_COMPAT);
	return true;
}

bool cpu_backend_long_mode_available(void)
{
	return (backend_interp.modes & CPU_MODE_LONG) != 0;
}

const cpu_backend_t *cpu_backend_current(void)
{
	if (backend_selected == NULL) return engine_for_flags();
	return backend_selected;
}

void cpu_backend_adopt(const cpu_backend_t *backend)
{
	if (backend == NULL) return;
	backend_selected = backend;
	force_flags(backend);
}

const cpu_backend_t *cpu_backend_fallback(const cpu_backend_t *backend)
{
	if (backend == &backend_uopjit) return &backend_uop;
	if (backend == &backend_uop) return &backend_interp;
	return NULL;
}

const cpu_backend_t *cpu_backend_by_index(uint32_t index)
{
	if (index >= BACKEND_COUNT) return NULL;
	return backend_all[index];
}

bool cpu_backend_can(uint32_t mode)
{
	if (mode == 0) return true; /* vacuously: nothing was asked for */
	return (cpu_backend_current()->modes & mode) == mode;
}

/* ------------------------------------------------------------------ *
 * the mode predicate (ss3.3's, S2 adds IA-32e)
 *
 * Appendix A of the plan states the whole predicate; the port has every field for it
 * except one.  CS.L is a DESCRIPTOR bit (bit 21 of the type-specific dword, alongside D/B),
 * and this tree does not decode it yet -- it arrives with the far-transfer work in S11 --
 * so "long (64-bit)" and "long (compat)" are not distinguishable HERE.  The predicate
 * therefore answers CPU_MODE_LONG for every IA-32e machine, and the gap is stated rather
 * than papered over:
 *
 *   - it is CONSERVATIVE for the capability check, which is what this function feeds:
 *     compat and 64-bit mode need the same backend capability (one implementation runs
 *     both), so a backend that cannot run long mode is refused in either;
 *   - the S2 selftest pins the arms that exist, including the LMA arm, and the arm that
 *     cannot exist yet is named in results/x86-64-s1-s2.md.
 *
 * EFER.LMA is the machine's own state, not the guest's request: LME says "enter IA-32e when
 * paging is enabled" and LMA says "we are in it".  Only LMA is read here (Appendix A).
 *
 * S5a UPDATED THE SPLIT ABOVE: CS.L arrives in this slice (plan correction GAP 2 -- the
 * corpus's entry path cannot be decoded without it), so the predicate now answers LONG or
 * COMPAT from `is_64` and the "cannot distinguish" gap is closed.  What remains S11's is the
 * rest of the system layer, not the predicate.
 * ------------------------------------------------------------------ */

uint32_t cpu_mode_of(const cpu_state_t *state)
{
	/* S5a: the split the S2 note below said was impossible -- CS.L is decoded now
	 * (`state->is_64`), so the two IA-32e modes are told apart instead of both answering
	 * CPU_MODE_LONG.  The capability check still treats them as one capability (both bits
	 * are declared together by cpu_backend_enable_long_mode): one implementation runs both,
	 * and a backend that cannot run IA-32e is refused in either. */
	if ((state->efer & EFER_LMA) != 0) return state->is_64 ? CPU_MODE_LONG : CPU_MODE_COMPAT;
	if ((state->cr[0] & CR0_PE) == 0) return CPU_MODE_REAL;
	return state->is_32 ? CPU_MODE_PROT32 : CPU_MODE_PROT16;
}

/* ------------------------------------------------------------------ *
 * S2 (ss3.5.2): THE LONG-MODE REFUSAL, IN ONE PLACE
 *
 * C5's defect was that EFER.LME was dead data: a guest wrote it, set CR0.PG with CR4.PAE,
 * and this port's load_pdpte read a PML4 as if it were a PDPT and walked 3 levels over
 * 4-level tables -- a plausible-looking wrong translation or a reserved-bit #PF, with
 * NOTHING in the machine naming which.  Three writes can start that transition, and all
 * three call this: WRMSR of EFER with LME, CR0 with PG (the SDM's trigger), and CR4 with
 * PAE (the same trigger when PG is already set).  Each caller passes the name of the write
 * it is performing, so the stop says what the guest asked for.
 *
 * The stop is the machine's NAMED STOP channel (unimplemented_instruction), the same one
 * the dispatcher's mode refusal uses -- not fatal(), which would abort the host instead of
 * ending the run with a reason.
 * ------------------------------------------------------------------ */

bool cpu_backend_refuse_long_mode(const char *what)
{
	const cpu_backend_t *backend = cpu_backend_current();

	if ((backend->modes & CPU_MODE_LONG) != 0) return false;

	char stop[160]; /* the machine's stop channel holds 160 (cpu.c) */
	snprintf(stop, sizeof(stop),
	         "%s asks for IA-32e, and cpu backend '%s' cannot run long mode"
	         " (no 4-level walk exists)", what, backend->name);
	unimplemented_instruction(stop);
	return true;
}

static const char *mode_name(uint32_t mode)
{
	switch (mode) {
	case CPU_MODE_REAL: return "16-bit real mode";
	case CPU_MODE_PROT16: return "16-bit protected mode";
	case CPU_MODE_PROT32: return "32-bit protected mode";
	case CPU_MODE_LONG: return "long mode";
	case CPU_MODE_COMPAT: return "compatibility mode";
	default: return "an unknown mode";
	}
}

/* ------------------------------------------------------------------ *
 * the dispatcher
 * ------------------------------------------------------------------ */

void cpu_backend_dispatch(uint32_t phys_addr)
{
	const cpu_backend_t *backend = cpu_backend_current();
	uint32_t mode = cpu_mode_of(cpu);

	if (!cpu_backend_can(mode)) {
		/* A NAMED STOP, never a silent path (ss3.3).  The machine produces NO state for a
		 * mode the selected engine cannot run -- the same discipline as the port's stop for
		 * an unported opcode. */
		char stop[160];
		snprintf(stop, sizeof(stop), "cpu backend '%s' cannot run %s", backend->name,
		         mode_name(mode));
		unimplemented_instruction(stop);
		return;
	}

	backend->run_batch(phys_addr);
}
