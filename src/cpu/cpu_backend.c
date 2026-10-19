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
 * allocation it makes -- see backend_interp_init. */
static const cpu_backend_t backend_interp = {
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
 * the mode predicate (the 32-bit half of ss3.3's; S2 adds long/compat)
 * ------------------------------------------------------------------ */

uint32_t cpu_mode_of(const cpu_state_t *state)
{
	if ((state->cr[0] & CR0_PE) == 0) return CPU_MODE_REAL;
	return state->is_32 ? CPU_MODE_PROT32 : CPU_MODE_PROT16;
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
