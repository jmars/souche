/*
 * cpu_backend.h -- THE ENGINE SEAM: the CPU backend contract, and the three engines the
 * port already has, behind it.
 *
 * DESIGN OF RECORD: results/x86-64-plan.md ss3.2 (the contract), ss3.3 (what is contract and
 * what is a backend's own business), ss3.4 (how the three existing engines land on it) and
 * ss6's S0 row (the slice this file lands in, INERT).  Read those before changing the shape:
 * the contract is deliberately SMALL, and everything a later slice wants to add is checked
 * against "is this the cascade's, or the machine's?" first.
 *
 * THE PRECEDENT IS THIS TREE'S OWN: src/hw/vga.h:129-153 -- a struct of methods, static
 * adapters, one of them the default.  The CPU seam is drawn the same way.
 *
 * ------------------------------------------------------------------ *
 * THE SHAPE IS ONE ENGINE AXIS OVER A CASCADE, NOT THREE PEERS (the plan's C3).
 *
 * `uop_run` CONTAINS the compiled-block path: a decoded block whose `code != NULL` is
 * executed by jit_execute_block inside uop_run (src/cpu/uop_cache.c:1334-1420), and the JIT's
 * exit kinds are uop_run's own (src/cpu/jit_codegen.h:44-48).  So the JIT is not a third
 * engine beside the interpreter and the uop cache; it is a property of a block record.  The
 * three INSTANCES below are therefore:
 *
 *   interp   run_batch = interpreted_run            real / 16-prot / 32-prot
 *   uop      run_batch = uop_run, compiled path OFF the same three modes
 *   uopjit   run_batch = uop_run, compiled path ON  the same three modes
 *
 * Two of them share a substrate; a contract drawn as "three interchangeable engines" would
 * have forced re-deriving that layering.  Nothing in this file knows what a uop is.
 *
 * ------------------------------------------------------------------ *
 * WHAT IS CONTRACT, AND WHAT IS NOT (ss3.3, one line each -- the table is the plan's).
 *
 *   CONTRACT: the reference's batch structure and the counter staircase.  The guest's clock
 *     is `instruction_counter / 1000` and it is guest-observable (docs/architecture.md ss2),
 *     so a backend that flushes a batch somewhere else MOVES the guest's clock.
 *   CONTRACT: cpu->in_hlt, cpu->reboot_requested, the stop channel (unimplemented_instruction
 *     + its name) and cpu->pagefault.  They are cpu_state_t fields or cpu.c's, not a
 *     backend's: they are the machine's only ways to stop.
 *   IMPLEMENTATION DETAIL: uop_pending, uop_page_code, jit_abort_flag, jit_block_boundary,
 *     the arena/pool/recipes.  A backend's own business; the dispatcher must never read them.
 *   SHARED MODEL, OUTSIDE THE CONTRACT: mem8, memory_devices, the TLB, cpu->cr[], the flags,
 *     the register file.  A backend IMPLEMENTS instruction semantics over the model; it does
 *     not own it.  That is what makes a KVM backend possible at all (results/kvm-plan.md ss1).
 *
 * ------------------------------------------------------------------ *
 * THE MODE REFUSAL (ss3.3's invariant, stated so a checker can hold it):
 *
 *   cpu_backend_dispatch() asks cpu_backend_can(cpu_mode_of(cpu)) BEFORE calling run_batch,
 *   and a refusal is a NAMED STOP ("cpu backend 'interp' cannot run long mode"), never a
 *   silent path.
 *
 * The premise that makes a per-BATCH check sufficient rather than per-instruction is a
 * property of the reference (READ v86/gen/x86_table.js): every instruction that can change
 * the mode (MOV CR*, MOV sreg, WRMSR, SYSCALL, far transfers, ...) is a `block_boundary`, so
 * it ends the batch and the dispatcher re-checks at the next cycle_internal.  That premise is
 * S2's to pin with a probe; S0 lands the seat, INERT (see "WHAT IS NOT LANDED HERE" below).
 *
 * ------------------------------------------------------------------ *
 * SELECTION, AND WHY `auto` IS ASYMMETRIC.
 *
 * The machine's two existing switches stay EXACTLY what they were, names, defaults and all
 * (`--uop-cache on|off` = src/cpu/uop_cache.h's `uop_cache_enabled`, `--jit on|off` =
 * src/cpu/jit_mem.h's `jit_enabled`), because gate scripts invoke them and a changed
 * spelling would be a changed gate.  They are now the ENGINE'S IDENTITY:
 *
 *   cpu_backend_select("auto")     derives the engine FROM the two booleans, writes NOTHING.
 *                                  This is the shipped default and the spelling the two
 *                                  existing flags go through, which is why every pre-seam
 *                                  invocation -- `--uop-cache off`, `--jit off`, `--uop-cache
 *                                  off --jit on` -- keeps its exact behaviour AND its exact
 *                                  stderr (jit_mem_init's two refusals are unchanged).
 *   cpu_backend_select("interp")   `--cpu-engine interp`: FORCES both booleans
 *   cpu_backend_select("uop")      `--cpu-engine uop`:    (off/off, on/off, on/on) so the
 *   cpu_backend_select("uopjit")   `--cpu-engine uopjit`: rest of the machine agrees with
 *                                  the engine that was named.  The explicit name WINS over
 *                                  the two aliases whatever their order on the command line.
 *
 * An unknown name returns NULL; the caller refuses BY NAME (src/main.c).  A seat that cannot
 * be selected is a seat nobody calls -- which is why S0 ships the flag and why the selftest
 * below pins that each of the three selections really resolves.
 *
 * ------------------------------------------------------------------ *
 * WHAT IS NOT LANDED HERE, STATED SO ITS ABSENCE IS NOT READ AS A PROMISE:
 *
 *   - `modes` carries no CPU_MODE_LONG bit and cpu_mode_of() cannot return one: EFER_LME is
 *     not defined anywhere in this tree yet (plan finding 5), so long mode is S2's and S5's.
 *     The refusal seat above is therefore INERT in S0 BY CONSTRUCTION -- it cannot fire,
 *     because no engine declares a mode the machine can be in.  S2 owns the probe that makes
 *     it fire, and CPUID's long-mode bits are S2's to gate on the same declaration (ss3.5).
 *   - `init` is the engine's own readiness, not a factory: the machine is a singleton
 *     (cpu_state.h) and every backend returns a pointer to static storage.
 */

#ifndef V86C_CPU_BACKEND_H
#define V86C_CPU_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"

/* FAMILIES OF EXECUTION MODE, as a capability bitmask: what a backend can RUN.
 *
 * The plan (ss9) settles why this is about MODES and not features: the KVM lane's backend
 * keeps the same contract while having no per-batch flush points and no 32-bit physical
 * bound, so a capability word naming features would be wrong for two of the four modes.
 *
 * The five values are the mode predicate's range (ss3.2's cpu_mode_of: real / 16-prot /
 * 32-prot / long / compat).  S0 declares the three a 32-bit machine can be in. */
#define CPU_MODE_REAL   (1u << 0) /* 16-bit real mode */
#define CPU_MODE_PROT16 (1u << 1) /* 16-bit protected mode */
#define CPU_MODE_PROT32 (1u << 2) /* 32-bit protected mode */
#define CPU_MODE_LONG   (1u << 3) /* IA-32e 64-bit mode (S2/S5) */
#define CPU_MODE_COMPAT (1u << 4) /* IA-32e compatibility mode (S2/S5) */

/* The three modes every engine in this tree runs today.  Named because it is ASSERTED (the
 * selftest requires each instance to declare exactly this, so an engine that quietly grew a
 * long-mode bit before S2 would be a finding, not a feature). */
#define CPU_MODES_32BIT (CPU_MODE_REAL | CPU_MODE_PROT16 | CPU_MODE_PROT32)

typedef struct cpu_backend {
	/* "interp", "uop", "uopjit" -- and one day "kvm".  The flag's spelling. */
	const char *name;

	/* CPU_MODE_* bitset: what this backend can RUN (not what it implements best). */
	uint32_t modes;

	/* Allocate what the engine needs.  false => this engine cannot run here: THE CALLER
	 * REFUSES IT BY NAME, PICKS THE PREVIOUS BACKEND AND SAYS SO (src/main.c's
	 * engine_init_if_needed) -- never a silent fallback. */
	bool (*init)(void);

	/* cycle_internal's engine half: run until one REFERENCE BATCH ends, then return.  The
	 * batch structure is the contract (see the header's "CONTRACT" list) -- this function
	 * may not invent its own flush points. */
	void (*run_batch)(uint32_t phys_addr);

	/* The code-invalidation entry point the machine uses when the guest's code pages are no
	 * longer valid (a guest reboot re-enters the boot: src/main.c's reboot_prepare_reentry).
	 * The caches themselves are the machine's, not one engine's -- the write hooks that
	 * invalidate them run for every engine. */
	void (*clear_caches)(void);

	/* --uop-cache-stats: the engine's counters, to stderr.  NULL = this engine has none of
	 * its own (the KVM lane's will be). */
	void (*print_stats)(void);
} cpu_backend_t;

/* Resolve a name and SELECT it ("" / "auto" derives from the two existing flags, see the
 * header).  Returns the selected backend, or NULL for a name this build does not have --
 * the caller's job is to refuse it BY NAME. */
const cpu_backend_t *cpu_backend_select(const char *name);

/* The selected backend.  Never NULL: before any selection it is the derivation of the flags
 * as they stand, so a caller that never selects (every selftest that drives a run loop
 * directly) still gets a defined answer. */
const cpu_backend_t *cpu_backend_current(void);

/* Can the SELECTED backend run every mode in `mode`?  (The mode-refusal query.) */
bool cpu_backend_can(uint32_t mode);

/* The mode the machine is IN, as one CPU_MODE_* bit (plan Appendix A).  REAL / PROT16 /
 * PROT32, plus -- from S2 -- LONG for any IA-32e machine: EFER.LMA is the machine's state
 * and it is set only once a backend can run long mode, but the 64-bit vs compat split
 * needs CS.L, which this tree does not decode yet (cpu_backend.c names the gap). */
uint32_t cpu_mode_of(const cpu_state_t *state);

/* S2 (plan ss3.5.2, C5): THE LONG-MODE REFUSAL, in the place the three transition writes
 * share.  Returns false when the selected backend can run long mode; otherwise records the
 * machine's NAMED STOP ("<what> asks for IA-32e, and cpu backend '<name>' cannot run long
 * mode") and returns true, which every caller must treat as "do not commit the write".
 *
 * The three call sites are the whole class: WRMSR of EFER carrying LME, a CR0 write setting
 * PG while LME is set, and a CR4 write setting PAE while PG and LME are.  Before S2 the
 * first was dead data and the other two reached load_pdpte() with a PML4 where a PDPT
 * belongs -- silently. */
bool cpu_backend_refuse_long_mode(const char *what);

/* THE ROUTING, IN ONE PLACE.  cycle_internal's engine half: the mode refusal, then the
 * selected backend's ->run_batch.  It is a FUNCTION rather than three lines inline in
 * cycle_internal for one reason: the selftest must be able to call EXACTLY what the
 * dispatcher calls.  A seat that cannot be invoked cannot be shown to be invoked, and an
 * untested seat is indistinguishable from an absent one (the plan's own warning). */
void cpu_backend_dispatch(uint32_t phys_addr);

/* S5a (plan ss6 S5): DECLARE LONG MODE ON THE ONE BACKEND THAT HAS IT, AND SELECT IT.
 *
 * Called once, from the argument parser, when the guest's image is allowed to enter IA-32e
 * (`--long-mode on`).  It ORs CPU_MODE_LONG | CPU_MODE_COMPAT into the interpreter's
 * capability word and adopts that engine, because a capability the running engine does not
 * have would be a false declaration (the ACPI-FADT lesson, made structural in ss3.5).  The
 * uop cache and the JIT deliberately do NOT gain the bits: their long-mode arms are S13's,
 * and their refusal is the correct current answer.
 *
 * It CANNOT FAIL TODAY -- there is nothing to allocate and no platform probe, so the body is an
 * OR and a `return true`; the return value exists so that a backend which one day must be built
 * has a refusal path already wired at both of main.c's call sites.  The query beside it is what
 * the gate asserts with. */
bool cpu_backend_enable_long_mode(void);
bool cpu_backend_long_mode_available(void);

/* The backend a refused one falls back to: uopjit -> uop -> interp -> NULL.  The chain is
 * the cascade's (ss3.4), not a preference list. */
const cpu_backend_t *cpu_backend_fallback(const cpu_backend_t *backend);

/* Switch to `backend` AND make the two engine flags agree with it (an explicit selection's
 * other half -- see the header's asymmetry note).  The refusal path uses it. */
void cpu_backend_adopt(const cpu_backend_t *backend);

/* The engines themselves, for a caller that wants to enumerate rather than name one (the
 * selftest's three-way loop).  `index` >= 3 returns NULL. */
const cpu_backend_t *cpu_backend_by_index(uint32_t index);

#endif /* V86C_CPU_BACKEND_H */
