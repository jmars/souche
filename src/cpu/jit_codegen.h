/*
 * jit_codegen.h -- the RECIPE TABLE and the BLOCK COMPILER: the uop array of a decoded block
 * becomes a native x86-64 function.
 *
 * WHAT IS COMPILED, FROM WHAT.  Not a new block abstraction: the uop array the uop cache
 * already decoded (uop_cache.h:118-147), whose framing, physical key, invalidation chain and
 * pending/flush staircase are all solved at the block level already (results/jit-plan.md ss2).
 * The compiler is a pure function of (the uop array, the block's state byte), and it is called
 * from the ONE place a block comes into existence (uop_cache.c's uop_block_decode), so a
 * compiled block is attached to the record the executor is about to use.
 *
 * THE CALLING CONTRACT.  `jit_compile_block` returns NULL when the block cannot be compiled --
 * it does not fit in one code page, the pool is exhausted, or the program is not built for
 * x86-64 -- and NULL means "execute this block with the uop executor", which is always correct.
 * There is no partial block: nothing is published unless the whole emission succeeded.
 *
 * THE EXECUTION CONTRACT.  A published block is `int (*)(void)`: it runs the block's uops and
 * returns one of JIT_EXIT_* -- the same four outcomes uop_run's own loop has (uop_cache.c's
 * UOP_LEFT_RETURN / _TAIL / _ABORT, plus the block-end return).  It does NOT implement the
 * reference's loop tail, the next-block lookup, or the SMC re-decode: uop_run's existing C
 * code does all of that (jit_execute_block is the whole of the JIT's side of it, and it calls
 * the substrate's block-enter/block-exit discipline around the call).
 *
 * aarch64: THIS FILE COMPILES TO STUBS (V86C_JIT_X64 == 0, jit_mem.h).  jit_compile_block
 * returns NULL, jit_execute_block is never called, the stats stay zero, and --jit on reports
 * unavailability from the substrate.  The uop cache remains the engine, exactly as before the
 * JIT existed.
 */

#ifndef V86C_JIT_CODEGEN_H
#define V86C_JIT_CODEGEN_H

#include <stdbool.h>
#include <stdint.h>

#include "uop_cache.h" /* uop_t -- the compile input */

/* ------------------------------------------------------------------ *
 * THE EXIT KINDS.  These are uop_run's own outcomes, numbered here so the compiled block's
 * `ret` value means the same thing the executor's `why` local means.  UOP_LEFT_RETURN ==
 * "a reference batch ended and the counter was flushed"; TAIL == "the reference's loop tail
 * (previous_ip, get_phys_eip) then a fresh lookup"; ABORT == "self-modifying code, re-decode
 * at the current eip and KEEP the pending count".
 * ------------------------------------------------------------------ */
#define JIT_EXIT_RETURN    1
#define JIT_EXIT_TAIL      2
#define JIT_EXIT_ABORT     3
#define JIT_EXIT_BLOCK_END 4 /* a block end that is NOT a reference batch end (sti) */

/* ------------------------------------------------------------------ *
 * THE COVERAGE COUNTERS (the brief's G4).
 *
 * `inlined_uops` / `called_uops` are DYNAMIC: how many executed uops ran as emitted code and
 * how many ran through a C body call.  `uops_with_recipe` / `uops_without_recipe` are STATIC
 * (compile-time, per emitted uop): what the recipe table matched.  The difference between the
 * pairs is the point -- a run can have 100% of its uops "called" because the recipe table
 * matched nothing, and the static counters are what say so.
 * ------------------------------------------------------------------ */
typedef struct jit_codegen_stats {
	uint64_t compile_attempts;
	uint64_t blocks_compiled;
	uint64_t blocks_reused;       /* the compile-time lookup found a live block for the key */
	uint64_t refused_fit;         /* the emission did not fit in one code page */
	uint64_t refused_fit_uops;    /* ... and the uops those blocks held (the sizing evidence) */
	uint64_t refused_pool;        /* the subtrate refused a page / the bind */
	uint64_t refused_noblock;     /* the block had no uops to compile */
	uint64_t compiled_uops;       /* uops inside the blocks that WERE compiled */
	uint64_t bytes_emitted;       /* emitted code bytes, over the compiled blocks */
	uint64_t uops_with_recipe;    /* compile-time: uops the recipe table matched */
	uint64_t uops_without_recipe; /* compile-time: uops that will be called */
	uint64_t inlined_uops;        /* dynamic: executed uops that ran as emitted code */
	uint64_t called_uops;         /* dynamic: executed uops that ran through a C call */
	/* The exit kinds, counted by uop_run where the block's return value is read (indexed
	 * by JIT_EXIT_*, so `exits[JIT_EXIT_ABORT]` is "how many compiled blocks stopped on
	 * self-modifying code").  It is what makes "the exit kinds round-trip" a measurement
	 * rather than a claim -- including on a real boot, where the kind is not otherwise
	 * visible from outside. */
	uint64_t exits[8];
	/* JIT-6: chain LINKS taken -- one per TAIL that jumped straight into the next compiled
	 * block's chain entry instead of returning through the dispatch.  Counted in
	 * jit_chain_target (the only place a link succeeds), so it counts links, not runs: a
	 * run that chains N times adds N.  The instrument for "did chaining engage" (it must be
	 * > 0 on any workload whose hot loops loop within a page) and for the fallback rate,
	 * next to exits[JIT_EXIT_TAIL] (links / tail-exits-encountered is the engage rate). */
	uint64_t chains;
} jit_codegen_stats_t;

extern jit_codegen_stats_t jit_codegen_stats;

/* ------------------------------------------------------------------ *
 * the recipe table
 * ------------------------------------------------------------------ */

/* A recipe keys on the uop's `fn` pointer -- the plan's ss7 step 2 preference, and it exists
 * for one reason: it costs NO struct change.  `fn` is the descriptor table's own body pointer
 * (uop_gen.c), so it is unique per encoding, and the table is built once and binary-searched
 * AT COMPILE TIME ONLY (never on a run path: the hot path reads the code pointer that the
 * block record already carries). */
#include "jit_x64.h" /* jx64_t -- the recipe emitters take it */

typedef struct jit_recipe {
	void (*fn)(void);
	const char *name;
	uint8_t shape; /* the ONLY uop shape this recipe is valid for: the shape fixes the
	                * argument mapping (REG0 is `fn(modrm & 7, g)`), so a recipe that matched
	                * a body under the wrong shape would call it with the wrong operands. */
	uint8_t kind;  /* the emitter's own selector (jit_codegen.c's JIT_REC_*) */
	uint8_t aux;   /* the digit / the register / the condition code the emitter needs */
	bool slow;     /* the emission has failure paths, i.e. it needs the shared C fallback.
	                * MEASURED relevance (results/jit-2-m2.md): a memory recipe's gates can
	                * fail, so it allocates a slow trampoline; a register-only recipe cannot,
	                * and emitting the trampoline anyway was a pure size cost. */
} jit_recipe_t;

/* The table, and the lookup.  `jit_recipe_lookup` returns NULL for a uop with no recipe --
 * which is every uop except the one M1 inlines: those are CALLED, the plan's ss3 fallback.
 * It checks the SHAPE as well as the body pointer, so a body reached through a shape its
 * recipe does not describe falls back instead of being mis-called. */
const jit_recipe_t *jit_recipe_lookup(const uop_t *u);
const char *jit_recipe_name(void (*fn)(void));
uint32_t jit_recipe_count(void);

/* ------------------------------------------------------------------ *
 * the compiler and the executor's side of the call
 * ------------------------------------------------------------------ */

/* Compile one decoded block into executable code, or NULL (nothing published).
 *
 * `uops`/`count` are the block's uops; `phys`/`virt`/`state` are the uop cache's own key
 * triple, used to look up an ALREADY COMPILED block for the same key before compiling --
 * which is what makes a block survive a uop-cache clear without leaking a code page. */
uint8_t *jit_compile_block(const uop_t *uops, uint32_t count, uint32_t phys, uint32_t virt,
                           uint8_t state);

/* Run a compiled block: the substrate's enter/exit discipline around the call, returning the
 * block's JIT_EXIT_*.  The caller (uop_run) owns everything after that. */
int jit_execute_block(uint8_t *code);

/* `--jit-stats`: the coverage counters.  Prints NOTHING for a run in which the JIT was never
 * asked for, so no pre-existing invocation's stderr changes. */
void jit_codegen_stats_print(void);

/* true on a build with an x86-64 backend AND a compiled program below it */
bool jit_codegen_available(void);

#endif /* V86C_JIT_CODEGEN_H */
