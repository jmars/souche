/*
 * jit_mem.h -- THE EXECUTABLE-MEMORY SUBSTRATE (unit JIT-SPIKE, HANDOFF=v86c-jit).
 *
 * WHAT THIS IS.  The JIT's code pages and its block table.  It is deliberately the
 * DESIGN-INDEPENDENT half of a JIT: it owns HOW a compiled block is stored, executed and
 * invalidated, and knows nothing about what a block computes or how its bytes are produced.
 * Neither assembler candidate (LuaJIT's DynASM and our own x64 encoder -- results/jit-spike.md
 * ss3) is imported or depended on here; a caller reserves a page, writes bytes into it,
 * publishes it, and binds it under a key.
 *
 * WHO CALLS IT.  Nothing on the shipped path: `jit_enabled` is false unless `--jit on` is
 * given (src/main.c), and every entry point below is a no-op while it is false.  The gate is
 * src/cpu/jit_selftest.c (`make jit-selftest`).
 *
 * KEYING, AND WHY IT IS THE UOP CACHE'S KEY.  A compiled block is keyed by the SAME triple
 * the decoded-block cache uses -- (physical start, virtual start, guest state flags) -- with
 * the same mixing function (`jit_mem.c`'s jit_hash_pos is uop_cache.c's uop_hash_pos).  That
 * is not a coincidence to be unaware of: it is what makes the two caches' invalidation
 * provably the SAME invalidation (see below), and it is why a JIT here inherits the uop
 * cache's already-solved problem of keying by PHYSICAL address (results/perf-2-blockcache-plan.md
 * ss3) instead of re-deriving it.
 *
 * THE INVALIDATION IS THE UOP CACHE'S, NOT A SECOND ONE.  PERF-FIX-3 restored the dirty-page
 * hooks at every guest-RAM write site (memory.c's write8/16/32 no-mmap arms and write_blob,
 * paging.c's safe_write* arms, string.c's fast paths) and they all call
 * `uop_invalidate_page_fast` (src/cpu/uop_cache.h:385), which tests the per-page bitmap
 * `uop_page_code[page]`.  This layer does NOT add write-site hooks of its own.  It:
 *   1. SHARES that bitmap -- jit_cache_bind sets `uop_page_code[page] = 1` for the page a
 *      compiled block covers, so a write to a page that holds only a JIT block (and no uop
 *      block) still takes the fast test's `!= 0` arm;
 *   2. is forwarded to from `uop_invalidate_page` itself (src/cpu/uop_cache.c), before that
 *      function's per-page early return, so a page with a compiled block and no decoded block
 *      is still reached.
 * The chain is therefore: guest write -> the PERF-FIX-3 hook -> uop_invalidate_page ->
 * jit_code_invalidate_page.  CONSEQUENCE, STATED BECAUSE IT IS A REAL CONSTRAINT: the JIT
 * rides the uop cache's structures, so `--jit on` needs the uop cache initialized (it is on
 * by default; jit_mem_init calls uop_cache_init itself rather than assuming), and `--jit on
 * --uop-cache off` is refused rather than silently running unaudited.
 *
 * THE PLATFORM CAVEATS, AT THE DEFINITION AND NOT IMPLIED.  The W^X path is
 *        mmap(PROT_READ|PROT_WRITE) -> write bytes -> mprotect(PROT_READ|PROT_EXEC)
 * and:
 *   * LINUX (this host, inside a cosmocc APE): VERIFIED -- see jit_mem.c's
 *     `jit_mem_platform_verified` and the selftest, which really executes a published page.
 *   * macOS / Apple Silicon: UNVERIFIED.  Apple requires MAP_JIT with
 *     pthread_jit_write_protect_np for a W^X JIT (and hardens against PROT_WRITE +
 *     PROT_EXEC mappings); this file does NOT do that, and no macOS host was available to
 *     test on.  An APE is a polyglot, so the SAME binary reaching macOS is exactly the case
 *     that would be untested -- it is stated here rather than assumed away.
 *   * Windows: UNVERIFIED, same reason (VirtualProtect / FlushInstructionCache are not
 *     invoked; cosmocc maps mprotect onto VirtualProtect, which is not the same sequence
 *     Apple/Windows JIT guidance prescribes).
 *   * I-CACHE COHERENCE: on x86-64 the instruction cache is coherent with the data cache in
 *     hardware, so no explicit flush is needed after publishing -- that is an x86-64
 *     property, NOT a portability claim about aarch64, which DOES need `__builtin___clear_cache`.
 *     The aarch64 posture is to compile this whole layer out (below), so the question does not
 *     arise there.
 *
 * THE aarch64 POSTURE.  V86C_JIT_X64 is 0 on any non-x86-64 target: the file then reduces to
 * the flag and the no-op entry points, so the tree still builds and `make` still produces the
 * same APE pair, and `--jit on` reports that the JIT is unavailable on this architecture
 * (leaving the uop cache as the engine) instead of failing to build or silently doing nothing.
 * This is the honest default the unit's context names: a JIT is x86-64-only unless a second
 * backend is written.
 */

#ifndef V86C_JIT_MEM_H
#define V86C_JIT_MEM_H

#include <stdbool.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64)
#define V86C_JIT_X64 1
#else
#define V86C_JIT_X64 0
#endif

/* `--jit on|off`.  FALSE unless the flag says otherwise: the shipped default, and the reason
 * every existing gate can stay byte-identical. */
extern bool jit_enabled;

/* THE SMC ABORT CHANNEL, AS A BYTE THE COMPILED BLOCK READS DIRECTLY (jit_codegen.c).
 *
 * uop_invalidate_page's own `uop_abort_pending` is the executor's copy of this signal and is
 * what the INTERPRETED loop tests; a compiled block cannot call a function to ask (that would
 * clobber the guest registers it holds), so the flag is exposed as a byte at a known address
 * and the emitted code reads it at every uop boundary.  SEQUENCE 0 -> 1 only: set by the
 * invalidator, cleared by jit_block_exit.  It is the same event as uop_abort_pending's (both
 * are set when a write lands on the page the executing block came from) -- see jit_mem.c's
 * jit_code_invalidate_page and uop_cache.c's uop_invalidate_page. */
extern uint8_t jit_abort_flag;

/* THE SIZE OF ONE CODE PAGE: the unit of the W^X transition.  Named here because the substrate
 * owns it and the block compiler (jit_codegen.c) sizes its emission buffer from it. */
#define JIT_CODE_PAGE_BYTES 4096u

/* M2's FIT RULE.  M1 published one block per code page and refused any block that did not fit
 * in 4096 bytes; MEASURED (results/jit-1-m1.md ss4), that refused 190 of the boot's 740 blocks
 * -- the HOT ones, mean 47.4 uops, holding 99.7 % of the boot's executions -- so only 0.32 % of
 * the boot ran compiled.  M2 reserves a CONTIGUOUS RUN of pages for a block that needs it
 * (jit_code_reserve_span), so the binding constraint becomes this cap rather than one page.
 * Four pages = 16 KiB, which is a whole 64-uop block at the measured cost of M2's emission
 * (results/jit-2-m2.md has the numbers); a block that needs more than this is still refused
 * rather than truncated. */
#define JIT_CODE_MAX_BLOCK_BYTES (4u * JIT_CODE_PAGE_BYTES)

/* The pool (jit_mem.c).  ONE BLOCK PER CODE PAGE, deliberately: it makes the W^X transition
 * and the free page-exact, which is what the invalidation discipline needs, and it is honest
 * about packing nothing -- a real design packs blocks and reference-counts the page.
 *
 * SIZED BY MEASUREMENT (JIT-PLAN ss5.1): the benchmark alone decodes 740 blocks
 * (results/perf-5-tune.md ss1), and a block whose code is invalidated is recompiled into a
 * fresh page, so the spike's 256 pages were smaller than one run's needs.  2048 pages = 8 MiB
 * of address space, committed lazily by the one mmap.  On exhaustion the compiler STOPS
 * (jit_code_reserve returns NULL, counted in jit_stats.lowmem_failures) and the affected
 * blocks execute as uop cache -- a slowdown, never a wrong answer. */
#define JIT_CODE_PAGES  4096u  /* 16 MiB of executable code (VA only: the pool is one lazy
                                * MAP_ANONYMOUS mmap).  M1's 2048 held 550 pages at peak-279
                                * live; a block that spans pages takes several at once, so the
                                * pool is doubled rather than measured down to the edge. */
#define JIT_MAX_BLOCKS  2048u
#define JIT_HASH_SLOTS  4096u

typedef struct jit_stats {
	uint64_t pages_alloc;      /* pages taken from the pool */
	uint64_t blocks_compiled;  /* bind() calls that installed a block */
	uint64_t blocks_live;
	uint64_t lookups;
	uint64_t hits;
	uint64_t misses;
	uint64_t invalidations;    /* blocks dropped by a guest write */
	uint64_t deferred_frees;   /* pages recycled only after the executing block returned */
	uint64_t wx_flips;         /* mprotect transitions, both directions */
	uint64_t execs;            /* jit_block_enter() calls */
	uint64_t lowmem_failures;  /* reserve() when the pool was exhausted */
	uint64_t hash_full;        /* bind() refused: the block hash had neither an EMPTY nor a
	                            * DEAD slot (audit-2 A2-SF; unreachable with live blocks
	                            * capped below JIT_HASH_SLOTS, kept as a named refusal) */
} jit_stats_t;

extern jit_stats_t jit_stats;

/* ------------------------------------------------ the substrate's own lifecycle */

/* Initialize the pool.  Returns false if there is no x86-64 backend, if the uop cache cannot be
 * initialized (it owns the shared dirty-page bitmap and the invalidation entry point), or if a
 * code page cannot be allocated.  Reports the reason on stderr in every case -- NO SILENT STUB. */
bool jit_mem_init(void);
void jit_mem_shutdown(void);

/* The platform/caveat string, printed by `--jit on` and by jit_stats_print: what is VERIFIED
 * here and what is not (see the header comment). */
const char *jit_mem_caps(void);
void jit_stats_print(void);

/* ------------------------------------------------ reserving, publishing, binding */

/* x86-64 only: reserve ONE writable code page.  NULL means the pool is exhausted (counted in
 * jit_stats.lowmem_failures) -- not a silent failure. */
uint8_t *jit_code_reserve(void);

/* M2: reserve `len` bytes' worth of CONTIGUOUS writable code pages (a run, so the published
 * block is one function in one mapping).  NULL means no run that long is free, or the pool is
 * exhausted -- counted in jit_stats.lowmem_failures, never a silent failure and never a
 * truncated block. */
uint8_t *jit_code_reserve_span(uint32_t len);

/* W^X flip: the page holding [buf, buf+len) becomes PROT_READ|PROT_EXEC.  `len` must not
 * escape the page reserved by jit_code_reserve.  Returns `buf` for the caller to call, or NULL
 * if the mprotect failed (jit_mem.c reports the errno). */
uint8_t *jit_code_publish(uint8_t *buf, uint32_t len);

/* Bind a published block under the uop cache's key.  Sets the SHARED dirty-page bit for the
 * physical page the block covers.  False if the state is not one uop_cache would key on (the
 * caller is expected to have gotten it from cpu->state_flags & CACHED_MASK_ALL). */
bool jit_cache_bind(uint32_t phys_start, uint32_t virt_start, uint8_t state,
                    uint8_t *code, uint32_t len);

/* The lookup the executor would do at a block entry.  NULL means "not compiled (any more)". */
uint8_t *jit_cache_lookup(uint32_t phys_start, uint32_t virt_start, uint8_t state);

/* ------------------------------------------------ the ONE invalidation path */

/* Called by uop_invalidate_page / uop_invalidate_range -- i.e. from the PERF-FIX-3 hook
 * points, through the uop cache's own entry points, never from a write site of its own.
 * Drops every block on the physical page, and (when the page is the one the executing block
 * is on) sets the abort and DEFERS the page's recycle until jit_block_exit: flipping a page
 * that is being executed to RW under the running code is undefined. */
void jit_code_invalidate_page(uint32_t phys_addr);
void jit_code_invalidate_range(uint32_t phys_addr, uint32_t length);

/* Drop EVERY compiled block and recycle its page.  Called from the uop cache's own clear
 * (uop_cache.c's uop_clear_internal), and it is a CORRECTNESS requirement rather than
 * housekeeping: that clear zeroes the shared `uop_page_code` bitmap, so a compiled block left
 * behind would no longer be watched by the PERF-FIX-3 write sites -- a later guest write to
 * its page would reach neither registry and the stale block would execute.  Dropping compiled
 * code on a cache clear is also the JIT pool's own overflow policy. */
void jit_code_clear_all(void);

/* ------------------------------------------------ the executing-block discipline */

/* A compiled block calls these around its body (the JIT's block prologue/epilogue).  They are
 * what makes the deferred-free rule above expressible; jit_block_exit returns true if an abort
 * is pending, i.e. the caller must not carry on into the next block. */
void jit_block_enter(uint8_t *code);
bool jit_block_exit(void);
bool jit_abort_pending(void);

/* ------------------------------------------------ JIT-6: BLOCK CHAINING
 *
 * A compiled block whose run ends in the executor's TAIL outcome (control flow left the
 * block, or the block ran out of uops) may enter the compiled block the tail target belongs
 * to DIRECTLY -- one jump -- instead of returning through the epilogue, uop_run's C dispatch
 * (get_phys_eip + the uop-cache lookup) and the next block's prologue.  THE PLAN'S OWN
 * CONSTRAINT (results/jit-plan.md ss7 step 8) is that the chain "must still go through
 * enter/exit and the staircase checks", and it does, in this exact sense:
 *
 *   * the per-uop checks (uop_cache.c's steps 6-11) are emitted per uop and run on EVERY
 *     chained block exactly as on a dispatch-entered one -- the chain site is only ever
 *     reached AFTER step 11 has already decided "tail";
 *   * the enter/exit bookkeeping still runs, once per link: jit_chain_target (below) is what
 *     a link calls, and it updates the executing-block record (jit_cur_page/jit_cur_block)
 *     that the deferred recycle and the SMC abort read -- so a write to the page of the block
 *     a chain moved execution INTO is still seen, and its page is still not recycled under it;
 *   * the staircase bookkeeping is untouched: the pending count rides R9 across the link,
 *     flushes still happen at the FLUSH uops (and end the chain -- a batch end is a RETURN
 *     exit, which never chains), the iteration limit and the budget window are per-uop checks
 *     that run as before, and the chain bumps the SAME uop_stats.lookups/hits counters the
 *     dispatch lookup it replaced used to bump.
 *
 * What a link ELIDES is therefore exactly: the source block's epilogue spill/stats fold, the
 * C tail, the uop-cache hash probe, jit_block_enter's page identification and the target's
 * prologue reload -- the dispatch cost, not any contract.  A link is attempted ONLY when:
 * the abort flag is clear (an SMC abort owns the re-decode), the fetch-translation cache
 * covers the target page (a cache-hit arm of get_phys_eip -- see jit_chain_target's comment
 * for why the chain never walks), and the (phys, virt, state) key names a LIVE compiled
 * block.  Every other case falls back to the normal TAIL exit, which is the pre-chain
 * behaviour byte for byte. */
uint8_t *jit_chain_target(void);
void jit_chain_bind(const uint8_t *code, uint32_t entry_off);

/* JIT-6: the block hash's test identity -- jit_selftest's chain-key gate searches a slot
 * collision with it.  An alias, deliberately: a divergent copy would silently un-teeth the
 * gate. */
uint32_t jit_test_hash(uint32_t phys, uint32_t virt, uint8_t state);

#endif /* V86C_JIT_MEM_H */
