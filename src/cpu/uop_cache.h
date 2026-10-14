/*
 * uop_cache.h -- the DECODE side of the decoded-block ("uop") cache: the uop and block
 * structures, the generated decode-descriptor tables, and the decode walk.
 *
 * DESIGN OF RECORD: results/perf-2-blockcache-plan.md (PERF-2), ss2 (what is decoded into
 * what), ss8 steps 1-3 (the files) and ss7's M1-M4 (the milestones).  **PERF-5R NICE-TO-HAVE-1:
 * this header described M1 only ("NO executor, NO cache, NO invalidation, NO --uop-cache flag")
 * long after it stopped being true.**  As shipped, this file carries the decoder AND the
 * executor/cache/invalidation interface: M1 (PERF-3) was the decode side, M2 (PERF-4) added the
 * executors, the cache and the pending/flush staircase and wired it into cycle_internal behind
 * `--uop-cache on|off` (default ON), and PERF-FIX-3 restored the dirty-page hooks at the dropped
 * write sites.  What this file still does NOT contain is the interpreter's own dispatch: the
 * fallback path is `interpreted_run` in src/cpu/cpu.c, and `--uop-cache off` is exactly it.
 *
 * WHAT A UOP IS (plan ss2.1).  Every per-instruction execution today re-reads the opcode,
 * the modrm byte, the sib/disp bytes and the immediates out of the instruction stream, then
 * re-resolves the address from the register file and the segment base.  Everything except the
 * register values and the segment base is a pure function of the instruction bytes and the
 * mode.  A uop is that decoded residue: the semantic body's address (`fn`, the unchanged
 * `instr_XX_*` body), the operand *shape* (which generic executor may call it), the
 * accumulated prefix byte, and the extracted address/immediate fields.
 *
 * A BLOCK (plan ss2.2) is one linear decode run terminating at one of:
 *   1. an instruction whose table entry is `block_boundary && !no_block_boundary_in_interpreted`
 *      -- the mode/mapping-changing set (the reference's own batch break: call/ret/hlt/int/
 *      iret/string-reps/mov-sreg/pop-ss/lss/far transfers/sysenter/mov cr/invlpg/...);
 *   2. an encoding with no uop descriptor (the per-opcode enablement lever, plan ss7) --
 *      including the `fixed_g` default arm, which the interpreter answers with trigger_ud();
 *   3. the virtual page end;
 *   4. the decode cap (UOP_MAX_BLOCK_UOPS / UOP_MAX_BLOCK_BYTES) -- OUR cap, semantics-free.
 * and it continues THROUGH jcc/jmp/loop (`no_block_boundary_in_interpreted`: the reference's
 * interpreter runs through them too) and through ordinary modrm instructions.
 *
 * DIVERGENCE FROM THE PLAN, STATED AND MEASURED (see results/perf-3-uop-decode.md ss4): the
 * plan's ss2.2 makes an instruction whose bytes CROSS the page end the block's last uop.  This
 * decoder does NOT include a crossing instruction in a block at all -- the block ends BEFORE
 * it.  Reason: decoding the next page's bytes needs that page's translation, and the port's
 * only translation entry point (translate_address_fetch) delivers a fault and writes page-table
 * A-bits as side effects; a decode-time peek would therefore be observable to the guest.  The
 * cost is one fallback per page-crossing instruction (the interpreter runs it, exactly as the
 * reference does); the benefit is that the decoder never touches state.  Consequence: the
 * plan's ss4.4 guard-crossing residual does not exist in this design (a crossing instruction is
 * never cached, guard-flagged or not), and this file's uop has no `pfx_len`-based crossing case.
 *
 * PORTABILITY (plan ss9 risk 7).  No JIT, no self-modifying code, no computed goto: data
 * structures and function pointers only, so the aarch64 APE posture is unchanged.
 */

#ifndef V86C_UOP_CACHE_H
#define V86C_UOP_CACHE_H

#include <stdint.h>

#include "cpu_state.h"
#include "modrm.h" /* PREFIX_* -- the prefix bits the uop's `pfx` byte carries */

/* ------------------------------------------------------------------ *
 * limits (plan ss2.2 item 4)
 * ------------------------------------------------------------------ */
#define UOP_MAX_BLOCK_UOPS  64
#define UOP_MAX_BLOCK_BYTES 240

/* ------------------------------------------------------------------ *
 * the uop
 * ------------------------------------------------------------------ */

/* `seg`: the DEFAULT segment of a memory operand (DS or SS) -- the segment-override prefix
 * is applied at execute time by get_seg_prefix(seg), exactly as modrm_resolve does.  A
 * register-form operand has no address and carries UOP_SEG_NONE. */
#define UOP_SEG_NONE 0xFF

/* `base`/`index`: a register index 0..7 (EAX..EDI, and the same indices for the 16-bit
 * register file: 3 = BX, 5 = BP, 6 = SI, 7 = DI), or UOP_REG_NONE. */
#define UOP_REG_NONE 0xFF

/* `ts_guard` -- the generated body's guard (gen/interpreter_c.js:200-203).  Its FAILURE
 * returns from the generated body, which means the postfix does NOT run either: a
 * guard-failed instruction is not a reference batch end.  The executor reproduces that
 * (uop_exec returns 0), and it is why the two kinds are distinguished. */
#define UOP_TS_NONE 0
#define UOP_TS_GUARD 1
#define UOP_TS_MMX   2

/* `addr_kind`: how the extracted fields materialise into an address */
#define UOP_ADDR_NONE 0 /* no memory operand */
#define UOP_ADDR_16   1 /* 16-bit addressing: (base + index + disp) & 0xFFFF, then + seg */
#define UOP_ADDR_32   2 /* 32-bit, no sib: base + disp (base may be UOP_REG_NONE: absolute) */
#define UOP_ADDR_SIB  3 /* 32-bit sib: base + index << scale + disp */

/* `flags` */
#define UOP_FLAG_BLOCK_END      (1u << 0) /* bb && !no_bib: the block ends AFTER this uop */
#define UOP_FLAG_FETCH_INTERNAL (1u << 1) /* the body re-reads the stream (8D/8F lea/pop) */
#define UOP_FLAG_TS_GUARD       (1u << 2) /* task_switch_test[_mmx]() runs at execute time */
#define UOP_FLAG_IGNORE_MOD     (1u << 3) /* modrm read, its 2 mod bits ignored (0F 20-24) */
#define UOP_FLAG_FLUSH          (1u << 4) /* M2: the interpreter's after_block_boundary()
                                           * runs for this instruction -- a reference BATCH
                                           * ends here (see UOP_DESC_FLUSH) */

/* `shape`: which generic-executor signature calls `fn`.  The value is a function of the
 * descriptor's flags and the decoded modrm, so it is derived (uop_shape_of) rather than
 * stored in the descriptor -- but it is what the M2 executor switches on. */
#define UOP_SHAPE_PLAIN0 0              /* fn() */
#define UOP_SHAPE_PLAIN1 1              /* fn(imm) */
#define UOP_SHAPE_PLAIN2 2              /* fn(imm, imm2) */
#define UOP_SHAPE_MEM0   3              /* fn(addr, g) */
#define UOP_SHAPE_MEM1   4              /* fn(addr, g, imm) */
#define UOP_SHAPE_MEMF0  5              /* fn(addr)      (fixed_g) */
#define UOP_SHAPE_MEMF1  6              /* fn(addr, imm) (fixed_g) */
#define UOP_SHAPE_REG0   7              /* fn(r1, g) */
#define UOP_SHAPE_REG1   8              /* fn(r1, g, imm) */
#define UOP_SHAPE_REGF0  9              /* fn(r1)        (fixed_g) */
#define UOP_SHAPE_REGF1  10             /* fn(r1, imm)   (fixed_g) */
#define UOP_SHAPE_UD     11             /* trigger_ud(): the decoder emits no UD uop (a
                                         * no-descriptor instruction ends the block and the
                                         * interpreter runs it); the shape is a total seat */
#define UOP_SHAPE_COUNT  12

typedef struct uop {
	void (*fn)(void);      /* the semantic body, unchanged (instr_XX_mem/_reg/plain) */
	uint32_t eip;          /* instruction start (virtual) -- the previous_ip source */
	uint32_t disp;         /* displacement, or the absolute address (disp16/disp32 forms) */
	int32_t imm, imm2;     /* the immediates, read ONCE at decode time */
	uint8_t shape;         /* one of UOP_SHAPE_* */
	uint8_t pfx;           /* the instruction's accumulated prefix byte, re-materialised */
	uint8_t seg;           /* UOP_SEG_NONE, DS or SS */
	uint8_t base, index;   /* register indices or UOP_REG_NONE */
	uint8_t scale;         /* 0..3 (sib only) */
	uint8_t g;             /* modrm >> 3 & 7, or UOP_REG_NONE when the encoding has no g */
	uint8_t flags;         /* UOP_FLAG_* */
	uint8_t ts_guard;      /* UOP_TS_*: which task_switch_test[_mmx]() the body needs */
	uint8_t addr_kind;     /* UOP_ADDR_* */
	uint8_t modrm;         /* the raw modrm byte (M2): the reg shapes call
	                        * fn(modrm & 7, modrm >> 3 & 7) and the ignore_mod form
	                        * (0F 20-24) calls fn(modrm & 7, g) -- neither is expressible
	                        * from `base`/`g`, which M1 left at UOP_REG_NONE for a
	                        * register-form operand. */
	uint16_t len;          /* total instruction length in bytes */
	uint16_t pfx_len;      /* prefix-byte count */
	uint16_t opcode_bytes; /* 1, or 2 when the 0F escape was consumed */
	uint16_t off;          /* byte offset of the OPCODE within the block */
	uint32_t body_ip;      /* PERF-5/M3: the instruction pointer the BODY must see at its
	                        * call -- `eip + len`, or `eip + pfx_len + opcode_bytes + 1` for
	                        * a custom_modrm_resolve encoding (0x8D/0x8F), which is handed the
	                        * raw modrm byte and reads the sib/disp itself.  Decode is the only
	                        * place that knows which.  (MEASURED: the executor derived it per
	                        * uop with a branch before this field existed.) */
} uop_t;

/* ------------------------------------------------------------------ *
 * the block (M1: the decode result, not yet an arena object)
 * ------------------------------------------------------------------ */

/* why the decode of a block stopped (the framing evidence) */
#define UOP_STOP_NONE            0 /* no block was produced at all */
#define UOP_STOP_PAGE_END        1 /* the next instruction would leave the virtual page */
#define UOP_STOP_CAP             2 /* UOP_MAX_BLOCK_UOPS / UOP_MAX_BLOCK_BYTES */
#define UOP_STOP_NO_DESC         3 /* an encoding with no uop descriptor */
#define UOP_STOP_BLOCK_END       4 /* a block_boundary && !no_bib instruction ended it */
#define UOP_STOP_NOT_RAM         5 /* the code window is not guest RAM (never decoded) */

typedef struct uop_block {
	uint32_t phys_start; /* physical address of the first instruction */
	uint32_t virt_start; /* the virtual address it was decoded from */
	uint16_t phys_len;   /* bytes covered -- never more than one virtual page */
	uint8_t state;       /* cpu->state_flags & CACHED_MASK_ALL (the M2 lookup key half) */
	uint8_t uop_count;
	uint8_t stop;        /* UOP_STOP_* -- why the walk stopped where it did */
	uint8_t pad;
	uint32_t stop_eip;   /* the virtual eip the walk stopped at */
} uop_block_t;

/* ------------------------------------------------------------------ *
 * the generated descriptor tables (src/cpu/uop_gen.c, gen/interpreter_uop.js)
 * ------------------------------------------------------------------ */

/* immediate kinds -- exactly gen_read_imm_call's outcomes (gen/interpreter_c.js:115-130) */
#define UOP_IMM_NONE  0
#define UOP_IMM_8     1 /* read_imm8() */
#define UOP_IMM_8S    2 /* read_imm8s() */
#define UOP_IMM_16    3 /* read_imm16() */
#define UOP_IMM_32S   4 /* read_imm32s() */
#define UOP_IMM_MOFFS 5 /* read_moffs() */

#define UOP_IMM2_NONE 0
#define UOP_IMM2_8    1 /* extra_imm8: read_imm8() */
#define UOP_IMM2_16   2 /* extra_imm16: read_imm16() */

/* descriptor flags */
#define UOP_DESC_HAS_MODRM  (1u << 0) /* the encoding is `e`: a modrm byte follows */
#define UOP_DESC_IGNORE_MOD (1u << 1) /* `ignore_mod`: both operands are registers */
#define UOP_DESC_FIXED_G    (1u << 2) /* the reg field selects the encoding */
#define UOP_DESC_FETCH_INT  (1u << 3) /* `custom_modrm_resolve`: the body re-reads the stream */
#define UOP_DESC_TS_GUARD   (1u << 4) /* `task_switch_test` / `sse` */
#define UOP_DESC_BLOCK_END  (1u << 5) /* `block_boundary && !no_bib` */
#define UOP_DESC_HAS_IMM    (1u << 6)
#define UOP_DESC_HAS_IMM2   (1u << 7)

/* A descriptor's fn_* pointer is the port's own `instr_XX_*` symbol -- or NULL, which means
 * the emitter found no DEFINITION for it (gen/interpreter_c.js:96-107's availability scan,
 * run against the same sources): the interpreter emits `unimplemented_instruction(...)` for
 * that form, and this decoder emits NO DESCRIPTOR, so the block falls back to it. */
/* THE INTERPRETER'S OWN POSTFIX PREDICATE, as a second flags byte (M2).  The first byte's
 * eight bits were full, and this one cannot be folded into it: it is
 * `(block_boundary && !no_block_boundary_in_interpreted) || (!custom && e)`
 * (gen/interpreter_c.js:187-190), and the second clause is FALSE for 520 of the table's
 * modrm encodings.  It is what decides where the interpreter flushes its
 * instruction_counter -- i.e. where the guest's clock (microtick, TSC, PIT, PM timer) is
 * incremented -- so the executor must be told, not have to guess. */
#define UOP_DESC_FLUSH     (1u << 0)
/* the guard is task_switch_test_mmx() rather than task_switch_test() -- they are NOT the
 * same function: with CR0.EM set the mmx form raises #UD and the other #NM
 * (misc_instr.c:656-682).  The emitter selects it from the encoding's `sse` marker, exactly
 * as interpreter_c.js:200-203 does. */
#define UOP_DESC_GUARD_MMX (1u << 1)

typedef struct uop_desc {
	void (*fn_plain)(void);
	void (*fn_mem)(void);
	void (*fn_reg)(void);
	uint8_t flags;   /* UOP_DESC_* */
	uint8_t iflags;  /* UOP_DESC_FLUSH */
	uint8_t imm;
	uint8_t imm2;
} uop_desc_t;

/* one (prefix arm, fixed_g) combination of one dispatch case */
#define UOP_FIXED_G_NONE 0xFF

typedef struct uop_entry {
	uint8_t fixed_g;  /* 0..7, or UOP_FIXED_G_NONE */
	uint16_t desc;    /* index into uop_descs */
} uop_entry_t;

/* one prefix arm of a case: the generated dispatcher's if-chain, as data.  `cond` is 0 for
 * the FALLTHROUGH arm (the `else` of that if-chain -- reached whenever none of the emitted
 * prefix conditions matched) and otherwise a PREFIX_* bit the accumulated prefixes must have. */
typedef struct uop_arm {
	uint8_t cond;
	uint8_t n;     /* entries in this arm (fixed_g combinations) */
	uint16_t first; /* index into uop_entries */
} uop_arm_t;

/* one dispatch case: `opcode | (is_osize_32() << 8)`, in the base table or the 0F table.
 * `reads_modrm` is gen/interpreter_c.js:358-366's `uses_modrm`: the modrm byte is read for
 * every e-encoding of the case EXCEPT one whose only e-encodings are ignore_mod forms with no
 * implementation -- and it must agree with the interpreter, because the byte is fetched either
 * way and its presence is part of the instruction's length. */
typedef struct uop_case {
	uint16_t first_arm;
	uint8_t n_arms;
	uint8_t reads_modrm;
} uop_case_t;

extern const uop_case_t uop_base_cases[512];
extern const uop_case_t uop_0f_cases[512];
extern const uop_arm_t uop_arms[];
extern const uop_entry_t uop_entries[];
extern const uop_desc_t uop_descs[];
extern const char *const uop_desc_names[];
extern const uint32_t uop_arm_count;
extern const uint32_t uop_entry_count;
extern const uint32_t uop_desc_count;

/* ------------------------------------------------------------------ *
 * the decode interface
 * ------------------------------------------------------------------ */

/* Decode one block starting at cpu->instruction_pointer, into `out` (at most `cap` uops).
 *
 * PRECONDITION: cpu->prefixes == 0 -- the reference's DBG_ASSERT at the top of its
 * interpreted loop (cpu.c:116), which is also the block-entry invariant of plan ss9 risk 4.
 * Returns the number of uops written.  0 means NO BLOCK (there is no descriptor for the first
 * instruction, the code is not RAM, the fetch would leave the page, or the fetch faulted):
 * the caller MUST fall back to the existing interpreted_run, which is the reference's own
 * path and handles the case identically.
 *
 * The walk reads bytes only from the first virtual page (plan ss2.2 divergence, file header),
 * and reads no registers at all -- the modrm fields are EXTRACTED, not resolved.  It can
 * therefore not fault, not deliver anything, and not change cpu state: on success the only
 * cpu field it may touch is the eip_phys translation cache (through get_phys_eip, which is
 * what the caller uses anyway). */
int uop_decode_block(uop_t *out, int cap, uint32_t virt_start, uint32_t phys_start,
                     uop_block_t *blk);

/* Materialise a memory operand's linear address from the extracted fields: the execute-time
 * half of the plan's ss8 step 3 split, and the thing the checker compares against
 * resolve_modrm16/32.  Reads the segment base through get_seg_prefix(u->seg), so it CAN fault
 * (a null segment raises #gp): it returns 0 with cpu->pagefault set in that case, exactly as
 * modrm_resolve does.  The caller must have cpu->prefixes == u->pfx (the executor's
 * re-materialisation, plan ss2.3). */
int32_t uop_materialize_addr(const uop_t *u);

/* The shape of a decoded uop, from its descriptor flags and its decoded modrm form. */
uint8_t uop_shape_of(const uop_desc_t *desc, bool is_mem);

/* JIT (M2): the executor's OWN per-uop worker, exposed for the compiled block's fallback.
 *
 * `uop_run`'s block loop uses it for every uop it does not inline, and a compiled block uses
 * the SAME function for the uops it has no recipe for -- which is what makes the fallback the
 * C implementation rather than a second transcription of it (jit_codegen.c's header).  The
 * contract is the loop's own: the caller has already stored `previous_ip`,
 * `instruction_pointer` and `prefixes` for this uop, and `pagefault` is false on entry; the
 * return value is the executor's `ran` (false for a failed task-switch guard and for a failed
 * address materialisation -- the two cases where the body did NOT run). */
bool uop_exec_uop(const uop_t *u);

/* the last decode's stopping evidence, for the selftest's report */
extern int uop_last_stop;
extern uint32_t uop_last_stop_eip;

const char *uop_stop_name(int stop);

/* ==================================================================== *
 * M2 (unit PERF-4): THE EXECUTOR, THE CACHE AND THE INVALIDATION
 *
 * The plan's ss2.3 (the generic executors), ss3 (the cache), ss4 (the invalidation) and
 * ss5.2 (the counter staircase).  Everything here is inert unless `uop_cache_enabled` is
 * set, so the shipped interpreter is unchanged when the cache is off.
 * ==================================================================== */

/* The `--uop-cache on|off` switch.  OFF is the pre-M2 interpreter: `uop_run` is not called
 * at all and `uop_invalidate_page` returns immediately -- so the cache-off transcript must
 * be byte-identical to the pre-M2 one (the plan's own gate, §8 step 5). */
extern bool uop_cache_enabled;

/* The run loop: cycle_internal calls this IN PLACE OF interpreted_run when the cache is
 * enabled.  `phys_addr` is get_phys_eip()'s result for cpu->instruction_pointer.
 *
 * It is a REPLACEMENT, not a wrapper: the reference's batch structure is preserved inside
 * (the counter is flushed at every reference batch end -- see the file), and it never
 * returns with uops already executed but unaccounted for, which is what makes
 * cycle_internal's `instruction_counter != initial_instruction_counter` assert and
 * do_many_cycles_native's LOOP_COUNTER test still mean what they mean. */
void uop_run(uint32_t phys_addr);

/* THE INSTRUCTIONS EXECUTED SINCE THE LAST REFERENCE BATCH END (PERF-4/M2).  Only
 * cycle_internal's progress check reads it: a block end that is not a reference batch end
 * (sti, and only sti) returns to cycle_internal with this non-zero and the counter unmoved. */
extern uint32_t uop_pending;

/* THE INVALIDATION HOOK (plan ss4.1).  Called by every guest-RAM write path in the port --
 * memory.c's write8/16/32 no-mmap arms and write_blob, paging.c's safe_write* arms and
 * string.c's fast movs/stos/ins paths -- with the PHYSICAL address written.
 *
 * It (a) drops every block registered on that physical page and (b) if the page is the one
 * the currently executing block is on, sets the SMC abort the executor checks at its next
 * uop boundary (plan ss4.2).  Blocks are keyed PHYSICALLY and the write paths are
 * physical, so no virtual-to-physical work is needed here.
 *
 * Cheap by construction: the first test is one load from a per-page table that is zero for
 * every page that has never held a decoded block (all data pages), so the cost on the hot
 * write path is a load, a compare and a return. */
void uop_invalidate_page(uint32_t phys_addr);

/* The same hook for a WRITE RANGE (write_blob: plan ss4.1's last row) -- loader-time only,
 * so it may be less clever. */
void uop_invalidate_range(uint32_t phys_addr, uint32_t length);

/* Instrumentation (plan ss6.2's numbers: the hit rate, the fallback fraction, the block
 * count, the invalidation and cache-clear counts).  `uop_stats_*` are for the selftest and
 * `--uop-cache-stats`. */
typedef struct uop_stats {
	uint64_t lookups;        /* uop_run block lookups */
	uint64_t hits;           /* ... that a cached block answered */
	uint64_t decodes;        /* ... that decoded a new block */
	uint64_t decode_fails;   /* ... where the decoder produced no block */
	uint64_t uops_executed;  /* uops run through an executor */
	uint64_t fallback_steps; /* instructions run by the reference's own loop body (uop_step) */
	uint64_t fallback_runs;  /* full interpreted_run calls */
	uint64_t fallback_instr; /* the instructions those interpreted_run calls executed */
	uint64_t flushes;        /* reference batch ends reproduced */
	uint64_t discards;       /* fetch faults that dropped a batch's count (cpu.c:145) */
	uint64_t aborts;         /* SMC aborts (plan ss4.2) */
	uint64_t invalidations;  /* blocks dropped by a write */
	uint64_t clears;         /* whole-cache clears (arena/block overflow, plan ss3) */
	uint64_t block_ends;     /* uops that ended a block */
	uint64_t arena_peak;     /* M3: the high-water mark of arena uops in use (plan ss3's
	                          * sizing question -- `clears` alone cannot size an arena,
	                          * because a clear happens only AFTER the pool overflows) */
	uint64_t block_peak;     /* M3: the high-water mark of block records in use */
} uop_stats_t;

extern uop_stats_t uop_stats;
void uop_stats_reset(void);
void uop_stats_print(void);

/* THE HOT-PATH TEST, inlined at every write site (plan ss4.1).  A page that has never held
 * a decoded block -- i.e. every data page -- is one load and a predictable branch; only a
 * page that DOES hold code calls into the cache.  This is the plan's "cheap: bitmap check
 * + short list walk", with the bitmap hoisted into the caller so the call itself is not
 * paid on the 14M+ RAM writes the boot makes. */
extern uint8_t *uop_page_code;     /* one byte per physical page: nonzero iff blocks are
                                    * registered on it.  NULL until uop_cache_init(). */
extern uint32_t uop_page_code_len;

static inline void uop_invalidate_page_fast(uint32_t phys_addr)
{
	uint32_t page = phys_addr >> 12;
	if (page < uop_page_code_len && uop_page_code[page] != 0) {
		uop_invalidate_page(phys_addr);
	}
}

/* Build (or rebuild) the cache's fixed-size structures.  Called an the first run; exposed
 * for the selftest.  Returns false if a structure could not be allocated (the cache then
 * disables itself and every run falls back). */
bool uop_cache_init(void);
void uop_cache_clear(void);

#endif /* V86C_UOP_CACHE_H */
