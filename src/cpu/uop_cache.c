/*
 * uop_cache.c -- the uop decoder: the decode walk (plan ss2.1), the block framing (ss2.2) and
 * the decode-time modrm extraction (ss8 step 3's split, taken as an EXTRACTION here -- see
 * results/perf-3-uop-decode.md ss3: src/cpu/modrm.c is NOT touched by this unit, so the
 * addressing semantics exist twice for now, and the checker's (b) enforces their equality).
 *
 * MILESTONE HISTORY (PERF-5R NICE-TO-HAVE-1: this header said "M1: decode only" long after it
 * stopped being true).  M1 (PERF-3) was decode-only.  M2 (PERF-4) added the executors, the
 * arena/hash cache, the pending/flush staircase and the SMC abort, and wired the cache into
 * cycle_internal behind `--uop-cache on|off` (default ON, --uop-cache off is the pre-M2 path).
 * PERF-FIX-3 restored the invalidation hooks at the dropped write sites.  THIS FILE therefore
 * holds the whole cache, not just the decoder; the decoder is the part described below, and its
 * correctness is still pinned by uop_cache_selftest.c against the interpreter's own semantics.
 *
 * WHAT MAKES THE DECODE FAITHFUL, and where each rule comes from:
 *
 *  - the byte order is the generated dispatcher's (gen/interpreter_c.js:328-412, mirrored by
 *    gen/interpreter_uop.js): modrm byte, then the modrm operand (sib/disp), then the
 *    immediate, then (for extra_imm*) the second immediate.  The order is semantic -- an
 *    immediate hoisted above a displacement read is the diverging instruction
 *    interpreter_c.js:225-233 records (MEASURED on `66 c7 05 ...`);
 *  - the case key is `opcode | (is_osize_32() << 8)` with is_osize_32() computed from the
 *    PREFIXES ACCUMULATED SO FAR (modrm.c:16), and the 0F escape's second byte is keyed the
 *    same way (instructions_0f.c:75-76: run_instruction0f_16/32 are the `0xNN` / `0x1NN`
 *    halves of the one generated 0F switch);
 *  - the prefix bytes are the port's own prefix instructions (instructions.c:606-627,
 *    1880-1918: instr_26/2E/36/3E/64/65/66/67/F0/F2/F3), accumulated exactly as those
 *    functions accumulate them: the segment prefixes REPLACE the segment field and the rest
 *    OR their bit in.  A prefix chain is ONE instruction here because the reference's nested
 *    run_prefix_instruction recursion only re-enters the dispatcher; the DISPATCH STATE (the
 *    accumulated prefixes and the resulting case key) is identical, and the reference's own
 *    loop counts it as one instruction (`i += 1` per iteration, cpu.c:112);
 *  - the instruction LENGTH is the interpreter's own: every byte the interpreter's fetch
 *    consumes is consumed here, in the same order, from the same page.  That is what the
 *    checker's (a) measures differentially against the interpreter itself.
 *
 * WHAT IT CANNOT DO, deliberately: read past the first virtual page (the block ends before a
 * crossing instruction -- uop_cache.h's header, plan ss2.2 divergence), read a register,
 * deliver anything, or change cpu state.  It reads only mem8, cpu->is_32 and the physical
 * address its caller already translated; it does not even call get_phys_eip, so it cannot
 * deliver a fault of its own.
 */

#include "uop_cache.h"

#include "jit_codegen.h" /* JIT-PLAN: the compiled block this cache's records can carry */
#include "jit_mem.h"      /* JIT-SPIKE: the executable-memory layer rides THIS function's hook */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"         /* interpreted_run, jit_block_boundary, INTERPRETER_ITERATION_LIMIT */
#include "exceptions.h"  /* trigger_ud (the UOP_SHAPE_UD seat) */
#include "instructions.h"/* run_instruction -- the reference's dispatch, used by uop_step */
#include "memory.h"  /* mem8, in_mapped_range */
#include "misc_instr.h"  /* task_switch_test / task_switch_test_mmx */
#include "modrm.h"   /* the PREFIX_* bits, DS/SS, get_seg_prefix, read_reg16/32 */
#include "paging.h"  /* cpu->segment_offsets and friends' declarations */

int uop_last_stop = UOP_STOP_NONE;
uint64_t uop_last_stop_eip = 0;

/* ------------------------------------------------------------------ *
 * the decode window: one virtual page, and nothing outside it
 * ------------------------------------------------------------------ */

/* S3: the cursor and the window's edges are 64-bit (the addresses they come from are), while
 * the PHYSICAL base stays 32-bit -- the model's bound (plan ss5.1).  The window is one
 * virtual page, so every arithmetic below is an offset inside 4 KiB, which is why the
 * projections to the mem8 index are exact rather than narrowings. */
static uint64_t dec_base_virt; /* the virtual address the window was opened at */
static uint32_t dec_base_phys; /* its physical address (translation is per page) */
static uint64_t dec_end_virt;  /* one past the page's last byte */
static uint64_t dec_eip;       /* the local decode cursor -- NEVER cpu->instruction_pointer */
static uint8_t dec_pfx;        /* the prefixes accumulated for the instruction being decoded */
static bool dec_overrun;       /* a read left the window: the instruction is dropped */

static int dec_fetch(uint64_t virt)
{
	if (virt < dec_base_virt || virt >= dec_end_virt) {
		dec_overrun = true;
		return 0;
	}
	/* the offset is inside the page (the window test above), so the sum is a 32-bit physical
	 * address by construction: this is the projection, not a truncation */
	return (int)mem8[dec_base_phys + (uint32_t)(virt - dec_base_virt)];
}

static int dec_read8(void)
{
	int b = dec_fetch(dec_eip);
	dec_eip += dec_overrun ? 0 : 1;
	return b;
}

/* modrm.c:16 is_osize_32()/is_asize_32(), over the DECODE cursor's prefixes rather than
 * cpu->prefixes (the decoder never writes that field -- plan ss9 risk 4's block-entry
 * invariant is that it is 0, which is also why is_osize_32's own body cannot be reused) */
static bool dec_osize_32(void)
{
	return cpu->is_32 != ((dec_pfx & PREFIX_MASK_OPSIZE) == PREFIX_MASK_OPSIZE);
}

static bool dec_asize_32(void)
{
	return cpu->is_32 != ((dec_pfx & PREFIX_MASK_ADDRSIZE) == PREFIX_MASK_ADDRSIZE);
}

static int32_t dec_read8s(void)
{
	int32_t v = dec_read8();
	return ((int32_t)((uint32_t)v << 24)) >> 24;
}

static int32_t dec_read16(void)
{
	int32_t low = dec_read8();
	int32_t high = dec_read8();
	return low | (high << 8);
}

static int32_t dec_read32s(void)
{
	int32_t low = dec_read16();
	int32_t high = dec_read16();
	return low | (high << 16);
}

/* misc_instr.c:198-208 read_moffs(), with the address size taken from the decode cursor */
static int32_t dec_read_moffs(void)
{
	if (dec_asize_32()) {
		return dec_read32s();
	}
	return dec_read16();
}

/* ------------------------------------------------------------------ *
 * the prefix bytes -- instructions.c's own prefix instructions, as one accumulator
 * ------------------------------------------------------------------ */

/* returns true when `byte` was a prefix, in which case `pfx` was updated */
static bool dec_prefix(uint8_t byte, uint8_t *pfx)
{
	switch (byte) {
	case 0x26: /* instr_26: segment_prefix_op(ES) -- the segment field is REPLACED, not OR'd */
	case 0x2E:
	case 0x36:
	case 0x3E:
	case 0x64:
	case 0x65: {
		/* instructions.c:66-70 segment_prefix_op: seg is ES/CS/SS/DS for 26/2E/36/3E and
		 * FS/GS for 64/65, and the stored value is `seg + 1` (SEG_PREFIX_ZERO is the
		 * seventh value -- modrm.c:31-57 segment_prefix / get_seg_prefix) */
		static const uint8_t seg_of[0x66] = {
			[0x26] = ES, [0x2E] = CS, [0x36] = SS, [0x3E] = DS, [0x64] = FS, [0x65] = GS,
		};
		*pfx = (uint8_t)((*pfx & ~PREFIX_MASK_SEGMENT) | (uint8_t)(seg_of[byte] + 1));
		return true;
	}
	case 0x66: /* instr_66 */
		*pfx |= PREFIX_MASK_OPSIZE;
		return true;
	case 0x67: /* instr_67 */
		*pfx |= PREFIX_MASK_ADDRSIZE;
		return true;
	case 0xF0: /* instr_F0 (lock): re-dispatches and sets NO bit */
		return true;
	case 0xF2: /* instr_F2 (repnz) */
		*pfx |= PREFIX_REPNZ;
		return true;
	case 0xF3: /* instr_F3 (repz) */
		*pfx |= PREFIX_REPZ;
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------ *
 * the decode-time modrm extraction -- modrm.c:198-517, mirrored field for field
 *
 * The interpreter's resolve_modrm16/32 read the sib/disp bytes and then ADD the register
 * values and the segment base; this reads the SAME bytes in the SAME order and stores the
 * REGISTER INDICES and the DISPLACEMENT, leaving the additions to uop_materialize_addr.
 * The 16-bit forms mask their sum to 16 bits -- which is why the mask lives in the
 * materialisation and not here: `(a + b + d) & 0xFFFF` is a property of the FORM.
 *
 * Register indices are the port's own (cpu_state.h: EAX..EDI = 0..7), which for the 16-bit
 * forms are used directly against read_reg16: BX = 3, BP = 5, SI = 6, DI = 7.
 * ------------------------------------------------------------------ */

static int dec_extract_modrm16(uop_t *u, uint8_t modrm)
{
	switch (modrm & 0xC7) { /* modrm.c:194 MODRM_MASK */
	case 0x00: u->base = 3; u->index = 6; break;              /* BX + SI */
	case 0x40: u->base = 3; u->index = 6; u->disp = (uint32_t)dec_read8s(); break;
	case 0x80: u->base = 3; u->index = 6; u->disp = (uint32_t)dec_read16(); break;
	case 0x01: u->base = 3; u->index = 7; break;              /* BX + DI */
	case 0x41: u->base = 3; u->index = 7; u->disp = (uint32_t)dec_read8s(); break;
	case 0x81: u->base = 3; u->index = 7; u->disp = (uint32_t)dec_read16(); break;
	case 0x02: u->base = 5; u->index = 6; u->seg = SS; break; /* BP + SI */
	case 0x42: u->base = 5; u->index = 6; u->seg = SS; u->disp = (uint32_t)dec_read8s(); break;
	case 0x82: u->base = 5; u->index = 6; u->seg = SS; u->disp = (uint32_t)dec_read16(); break;
	case 0x03: u->base = 5; u->index = 7; u->seg = SS; break; /* BP + DI */
	case 0x43: u->base = 5; u->index = 7; u->seg = SS; u->disp = (uint32_t)dec_read8s(); break;
	case 0x83: u->base = 5; u->index = 7; u->seg = SS; u->disp = (uint32_t)dec_read16(); break;
	case 0x04: u->base = 6; break;                            /* SI */
	case 0x44: u->base = 6; u->disp = (uint32_t)dec_read8s(); break;
	case 0x84: u->base = 6; u->disp = (uint32_t)dec_read16(); break;
	case 0x05: u->base = 7; break;                            /* DI */
	case 0x45: u->base = 7; u->disp = (uint32_t)dec_read8s(); break;
	case 0x85: u->base = 7; u->disp = (uint32_t)dec_read16(); break;
	case 0x06: u->disp = (uint32_t)dec_read16(); break;       /* absolute disp16, DS */
	case 0x46: u->base = 5; u->seg = SS; u->disp = (uint32_t)dec_read8s(); break;
	case 0x86: u->base = 5; u->seg = SS; u->disp = (uint32_t)dec_read16(); break;
	case 0x07: u->base = 3; break;                            /* BX */
	case 0x47: u->base = 3; u->disp = (uint32_t)dec_read8s(); break;
	case 0x87: u->base = 3; u->disp = (uint32_t)dec_read16(); break;
	default:
		/* modrm.c:325-330: unreachable -- every value of modrm & !0o070 is covered */
		DBG_ASSERT(false);
		return -1;
	}
	return 0;
}

static int dec_extract_sib(uop_t *u, uint8_t modrm)
{
	int sib = dec_read8();
	int r = sib & 7;
	int m = (sib >> 3) & 7;
	int s = (sib >> 6) & 3;
	int mod = modrm & 0xC0;

	if (r == 4) {           /* modrm.rs: base = ESP, SS */
		u->base = ESP;
		u->seg = SS;
	}
	else if (r == 5) {
		if (mod != 0x00) {  /* with an immediate: base = EBP, SS */
			u->base = EBP;
			u->seg = SS;
		}
		else {              /* without one: the base IS a disp32, and the segment is DS */
			u->disp = (uint32_t)dec_read32s();
		}
	}
	else {
		u->base = (uint8_t)r;
	}

	if (m != 4) {           /* index 4 is "no index" (modrm.rs: m == 4 -> offset 0) */
		u->index = (uint8_t)m;
		u->scale = (uint8_t)s;
	}

	if (mod == 0x40) {
		u->disp = (uint32_t)dec_read8s();
	}
	else if (mod == 0x80) {
		u->disp = (uint32_t)dec_read32s();
	}
	return 0;
}

static int dec_extract_modrm32(uop_t *u, uint8_t modrm)
{
	uint8_t rm = modrm & 7;
	int mod = modrm & 0xC0;

	if (rm == 4) { /* modrm.c:395-413: the sib forms apply their own segment */
		return dec_extract_sib(u, modrm);
	}

	if (mod == 0x00) {
		if (rm == 5) { /* absolute disp32, default segment DS (modrm.c:414-419) */
			u->disp = (uint32_t)dec_read32s();
			return 0;
		}
		u->base = rm;
		return 0;
	}

	if (rm == 5) { /* EBP-based, default segment SS (modrm.c:420-433) */
		u->base = EBP;
		u->seg = SS;
	}
	else {
		u->base = rm;
	}

	if (mod == 0x40) {
		u->disp = (uint32_t)dec_read8s();
	}
	else {
		u->disp = (uint32_t)dec_read32s();
	}
	return 0;
}

static void dec_extract_modrm(uop_t *u, uint8_t modrm)
{
	u->seg = DS;
	if (dec_asize_32()) {
		u->addr_kind = (modrm & 7) == 4 ? UOP_ADDR_SIB : UOP_ADDR_32;
		dec_extract_modrm32(u, modrm);
	}
	else {
		u->addr_kind = UOP_ADDR_16;
		dec_extract_modrm16(u, modrm);
	}
}

/* ------------------------------------------------------------------ *
 * the materialisation -- the execute-time half of the split
 *
 * modrm.c:332/465 end every non-sib arm at
 *     default_segment == DS ? get_seg_prefix_ds(offset) : get_seg_prefix_ss(offset)
 * i.e. the segment base is added AFTER the form's own arithmetic, and a faulting segment
 * lookup yields the 0 sentinel rather than "0 + offset" (modrm.c:59-70).
 *
 * WHY IT HAS A `static` CORE (PERF-5/M3).  MEASURED (orchestrator's gprof build of this tree,
 * cache on, 200,095,214 instructions) and re-MEASURED in the relocations of the SHIPPED unity
 * object (`R_X86_64_PLT32 uop_materialize_addr` at the executor loop's call site,
 * build/unity.o): the shipped build calls this function OUT OF LINE, once per memory operand --
 * 75.5M calls per 200M instructions, the second-largest remaining call count after
 * `get_seg`'s 78.2M.  The reason it is not inlined is mechanical: `-finline-functions-called-once`
 * (PERF_CFLAGS, the Makefile's measured win) applies to STATIC functions only, and this one is
 * a global -- it is the symbol M1's decode checker compares against resolve_modrm16/32
 * (uop_cache_selftest.c), so it must keep external linkage.
 *
 * The fix is therefore structural, not a tuning knob: the body lives in a `static inline` core
 * that the executor calls, and the exported function is a one-line wrapper over it -- so there
 * is still exactly ONE copy of the addressing arithmetic, and the symbol the checker links
 * against still exists and still does the same thing.  MEASURED (results/perf-5-tune.md G2):
 * the out-of-line call disappears from the shipped object's relocations and the benchmark
 * moves by the delta recorded there.
 * ------------------------------------------------------------------ */

/* THE SEGMENT LOOKUP, INLINE (PERF-5/M4a -- the plan's M4 first item, taken differently).
 *
 * THE MEASUREMENT IT ANSWERS: `get_seg` is the LARGEST remaining call count on the benchmark
 * -- 78,235,256 calls per 200,095,214 instructions (the orchestrator's gprof build of this
 * tree, cache on), i.e. ~0.39 per guest instruction, one per memory operand.  Two thirds of
 * those are made from this very function (the shipped object's relocations put two
 * `R_X86_64_PLT32 get_seg` sites inside uop_materialize_addr).
 *
 * WHY A TRANSCRIPTION RATHER THAN THE PLAN'S CACHE.  The plan (perf-2-blockcache-plan.md
 * ss7/M4) prescribes a segment-base CACHE: skip `get_seg` while a segment is flat and a
 * segment EPOCH is unchanged.  Both designs remove the same call, and the difference is what
 * each one has to ASSUME.  The cache assumes the epoch -- "no segment offset, no
 * segment_is_null flag, no CPL and no CS size changed since I cached this base" -- which the
 * tree would have to maintain at every writer of that state: results/perf-5-seg-audit.py
 * enumerates all 41 of them and shows 17 that have no update_state_flags() in their own
 * function (they are covered by a caller, which is exactly the kind of coverage that is
 * correct until one new caller is added).  This function assumes NOTHING: it reads
 * `cpu->segment_offsets[seg]` and `cpu->segment_is_null[seg]` at execute time, at the same
 * moment and from the same place get_seg does, so it cannot be stale.  It is a transcription
 * of modrm.c:44-99 and therefore duplicates 12 lines -- the shape M1 already took for the
 * modrm extraction (this file's header) -- and it is gated as a transcription should be: the
 * differential arm `test_seg_base` (src/cpu/uop_exec_selftest.c) runs a matrix of (default
 * segment, override prefix, null flag, non-zero base, #GP) through the cache and through
 * interpreted_run and requires identical results, and results/perf-5-mut-rig.sh shows that
 * arm failing on three separate inverse mutations of this function.
 *
 * THE FLAT SPECIAL CASE WOULD HAVE BEEN SMALLER AND WORSE: it would cover only DS/SS-defaulted
 * operands with no override prefix, and it would need its own argument that a null SS cannot
 * happen (get_seg's own DBG_ASSERT says it cannot -- modrm.c:88).  One code path, no new
 * invariant, every case.
 *
 * EQUIVALENCE, line for line, against modrm.c:44-99 (get_seg_prefix + get_seg): the prefix
 * field selects the segment (with SEG_PREFIX_ZERO -> the constant 0), else the uop's own
 * default; a null segment does `trigger_gp(0)` then sets cpu->pagefault and returns the 0
 * sentinel -- the port's only abort channel, and the reason this can fault.  Not approximated:
 * the same two DBG_ASSERTs are kept (a null CS/SS is a port bug, not a guest case). */
static inline int32_t uop_seg_base(uint8_t seg)
{
	int32_t prefix = cpu->prefixes & PREFIX_MASK_SEGMENT;

	if (prefix != 0) {
		if (prefix == SEG_PREFIX_ZERO) {
			return 0;
		}
		seg = (uint8_t)(prefix - 1);
	}

	DBG_ASSERT(seg < 8);
	/* read through a local: gcc -O2 cannot see the range the DBG_ASSERT establishes and warns
	 * about `cpu->segment_is_null[seg]` -- modrm.c:82-87 keeps the same idiom for the same
	 * reason (and it is a false positive: every caller passes an index in [0,7]) */
	bool *is_null = cpu->segment_is_null;
	if (is_null[seg]) {
		DBG_ASSERT(seg != CS && seg != SS);
		trigger_gp(0);
		cpu->pagefault = true;
		return 0;
	}
	/* S3: as get_seg (modrm.c) -- the slot is 64-bit, the addressing form's base is 32 */
	return (int32_t)(uint32_t)cpu->segment_offsets[seg];
}

int32_t uop_materialize_addr(const uop_t *u)
{
	int32_t offset = 0;

	switch (u->addr_kind) {
	case UOP_ADDR_NONE:
		break;
	case UOP_ADDR_16:
		if (u->base != UOP_REG_NONE) offset += read_reg16(u->base);
		if (u->index != UOP_REG_NONE) offset += read_reg16(u->index);
		offset = (offset + (int32_t)u->disp) & 0xFFFF;
		break;
	case UOP_ADDR_32:
		if (u->base != UOP_REG_NONE) offset += read_reg32(u->base);
		offset += (int32_t)u->disp;
		break;
	case UOP_ADDR_SIB:
		if (u->base != UOP_REG_NONE) offset += read_reg32(u->base);
		if (u->index != UOP_REG_NONE) {
			/* modrm.c:509 applies upstream's `read_reg32(m) << s` on i32 by shifting the
			 * u32 bit pattern (arith.h's `<<` convention) */
			offset += (int32_t)((uint32_t)read_reg32(u->index) << u->scale);
		}
		offset += (int32_t)u->disp;
		break;
	default:
		DBG_ASSERT(false);
		return 0;
	}

	/* get_seg_prefix can fault (a null segment raises #gp and sets cpu->pagefault): keep
	 * the same 0 sentinel and the same fault flag modrm_resolve's callers see.  The lookup
	 * itself is uop_seg_base's inline copy of modrm.c's -- see its own comment. */
	int32_t seg_base = uop_seg_base(u->seg);
	if (cpu->pagefault) {
		return 0;
	}
	return seg_base + offset;
}


uint8_t uop_shape_of(const uop_desc_t *desc, bool is_mem)
{
	if (!(desc->flags & UOP_DESC_HAS_MODRM)) {
		if (desc->flags & UOP_DESC_HAS_IMM2) return UOP_SHAPE_PLAIN2;
		if (desc->flags & UOP_DESC_HAS_IMM) return UOP_SHAPE_PLAIN1;
		return UOP_SHAPE_PLAIN0;
	}
	if (desc->flags & UOP_DESC_IGNORE_MOD) {
		return UOP_SHAPE_REG0;
	}
	bool has_imm = (desc->flags & UOP_DESC_HAS_IMM) != 0;
	bool fixed_g = (desc->flags & UOP_DESC_FIXED_G) != 0;
	if (is_mem) {
		if (fixed_g) return has_imm ? UOP_SHAPE_MEMF1 : UOP_SHAPE_MEMF0;
		return has_imm ? UOP_SHAPE_MEM1 : UOP_SHAPE_MEM0;
	}
	if (fixed_g) return has_imm ? UOP_SHAPE_REGF1 : UOP_SHAPE_REGF0;
	return has_imm ? UOP_SHAPE_REG1 : UOP_SHAPE_REG0;
}

const char *uop_stop_name(int stop)
{
	switch (stop) {
	case UOP_STOP_NONE: return "none";
	case UOP_STOP_PAGE_END: return "page-end";
	case UOP_STOP_CAP: return "cap";
	case UOP_STOP_NO_DESC: return "no-descriptor";
	case UOP_STOP_BLOCK_END: return "block-end";
	case UOP_STOP_NOT_RAM: return "not-ram";
	default: return "?";
	}
}

/* ------------------------------------------------------------------ *
 * the decode walk
 * ------------------------------------------------------------------ */

/* decode ONE instruction into `u`, with dec_eip at the OPCODE byte and dec_pfx accumulated.
 * `inst_start` is the eip of the instruction's first prefix byte (or of the opcode), i.e. the
 * reference's `previous_ip` for it.  Returns 0, or one of the two stop codes below: */
#define DEC_PAGE_END 1 /* a read left the page: the instruction is dropped, the block ends */
#define DEC_NODESC   2 /* no descriptor: the interpreter would stop by name / trigger_ud */

static int dec_instruction(uop_t *u, uint64_t inst_start)
{
	bool osize32 = dec_osize_32();

	u->eip = inst_start;
	u->pfx = dec_pfx;
	u->pfx_len = (uint16_t)(dec_eip - inst_start);

	int opcode = dec_read8();
	if (dec_overrun) return DEC_PAGE_END;

	const uop_case_t *cases;
	int key;
	if (opcode == 0x0F) {
		/* instructions.c:182-194 instr16_0F/instr32_0F: the escape reads one byte and
		 * re-dispatches into the ONE 0F switch (instructions_0f.c:75-76) */
		int second = dec_read8();
		if (dec_overrun) return DEC_PAGE_END;
		key = second | (osize32 ? 0x100 : 0);
		cases = uop_0f_cases;
		u->opcode_bytes = 2;
	}
	else {
		key = opcode | (osize32 ? 0x100 : 0);
		cases = uop_base_cases;
		u->opcode_bytes = 1;
	}

	const uop_case_t *c = &cases[key];

	/* interpreter_c.js:358-366: the modrm byte is read BEFORE the prefix arms are tested
	 * (gen_instruction_body), and whether it is read at all is a per-case property */
	uint8_t modrm = 0;
	if (c->reads_modrm) {
		modrm = (uint8_t)dec_read8();
		if (dec_overrun) return DEC_PAGE_END;
		u->g = (uint8_t)((modrm >> 3) & 7);
		u->modrm = modrm;
	}

	/* the arm: the generated if-chain's order (66, F2, F3, then the fallthrough) */
	const uop_arm_t *arm = NULL;
	for (int i = 0; i < c->n_arms; i++) {
		const uop_arm_t *a = &uop_arms[c->first_arm + i];
		if (a->cond == 0 || (dec_pfx & a->cond) != 0) {
			arm = a;
			break;
		}
	}
	DBG_ASSERT(arm != NULL);

	const uop_desc_t *desc = NULL;
	for (int i = 0; i < arm->n; i++) {
		const uop_entry_t *e = &uop_entries[arm->first + i];
		if (arm->n == 1 && e->fixed_g == UOP_FIXED_G_NONE) {
			desc = &uop_descs[e->desc];
			break;
		}
		if (e->fixed_g == u->g && e->fixed_g != UOP_FIXED_G_NONE) {
			desc = &uop_descs[e->desc];
			break;
		}
	}
	/* no entry for this g: the generated `default: trigger_ud(); break;` */
	if (desc == NULL) return DEC_NODESC;

	bool is_mem = false;
	if (desc->flags & UOP_DESC_HAS_MODRM) {
		if (desc->flags & UOP_DESC_IGNORE_MOD) {
			/* 0F 20-24: the 2 mod bits are ignored, both operands are registers */
			u->fn = desc->fn_plain;
		}
		else if (modrm < 0xC0) {
			is_mem = true;
			/* the operand bytes are consumed HERE, before the immediate -- the order
			 * interpreter_c.js:236-273 emits (and the reason that comment exists).  For
			 * a custom_modrm_resolve encoding (0x8D/0x8F) the interpreter's BODY does
			 * this read at execute time instead; the bytes consumed are the same and
			 * neither has an immediate, so the two orders coincide (the emitter throws
			 * on an extra immediate attached to a modrm encoding). */
			dec_extract_modrm(u, modrm);
			if (dec_overrun) return DEC_PAGE_END;
			u->fn = desc->fn_mem;
		}
		else {
			u->fn = desc->fn_reg;
		}
	}
	else {
		u->fn = desc->fn_plain;
	}

	/* the immediate(s) -- gen_read_imm_call's kind, read after the operand bytes */
	if (desc->flags & UOP_DESC_HAS_IMM) {
		switch (desc->imm) {
		case UOP_IMM_8: u->imm = dec_read8(); break;
		case UOP_IMM_8S: u->imm = dec_read8s(); break;
		case UOP_IMM_16: u->imm = dec_read16(); break;
		case UOP_IMM_32S: u->imm = dec_read32s(); break;
		case UOP_IMM_MOFFS: u->imm = dec_read_moffs(); break;
		default: DBG_ASSERT(false); return DEC_NODESC;
		}
		if (dec_overrun) return DEC_PAGE_END;
	}
	if (desc->flags & UOP_DESC_HAS_IMM2) {
		u->imm2 = desc->imm2 == UOP_IMM2_16 ? dec_read16() : dec_read8();
		if (dec_overrun) return DEC_PAGE_END;
	}

	/* the form the generated code would call: a NULL pointer is the interpreter's
	 * `unimplemented_instruction(...)`, which is a STOP -- so the block ends here */
	if (u->fn == NULL) {
		return DEC_NODESC;
	}

	u->shape = uop_shape_of(desc, is_mem);
	if (desc->flags & UOP_DESC_TS_GUARD) {
		u->ts_guard = (desc->iflags & UOP_DESC_GUARD_MMX) ? UOP_TS_MMX : UOP_TS_GUARD;
	}
	if (desc->flags & UOP_DESC_BLOCK_END) u->flags |= UOP_FLAG_BLOCK_END;
	if (desc->flags & UOP_DESC_FETCH_INT) u->flags |= UOP_FLAG_FETCH_INTERNAL;
	if (desc->flags & UOP_DESC_IGNORE_MOD) u->flags |= UOP_FLAG_IGNORE_MOD;
	if (desc->iflags & UOP_DESC_FLUSH) u->flags |= UOP_FLAG_FLUSH;
	if (u->ts_guard) u->flags |= UOP_FLAG_TS_GUARD;

	u->len = (uint16_t)(dec_eip - inst_start);
	/* THE INSTRUCTION POINTER AT THE BODY'S ENTRY, PRECOMPUTED (PERF-5/M3).  The executor used
	 * to derive it per uop -- `eip + (FETCH_INTERNAL ? pfx_len + opcode_bytes + 1 : len)` --
	 * with the one exception (a custom_modrm_resolve body, 0x8D/0x8F, is handed the raw modrm
	 * byte and reads the sib/disp itself, so its pointer sits just AFTER that byte; see the
	 * executor's own comment and results/perf-4-uop-exec.md ss2.3).  It is a pure function of
	 * the decode, so it belongs here: one store per execution instead of a branch and three
	 * adds.  The interpreter's own pointer at the call is the instruction's END. */
	u->body_ip = (u->flags & UOP_FLAG_FETCH_INTERNAL)
	                 ? u->eip + (uint32_t)u->pfx_len + (uint32_t)u->opcode_bytes + 1u
	                 : u->eip + (uint32_t)u->len;
	return 0;
}

int uop_decode_block(uop_t *out, int cap, uint64_t virt_start, uint32_t phys_start,
                     uop_block_t *blk)
{
	if (blk != NULL) {
		memset(blk, 0, sizeof *blk);
	}
	uop_last_stop = UOP_STOP_NONE;
	uop_last_stop_eip = virt_start;

	if (cap <= 0) return 0;

	/* the reference's block-entry invariant (cpu.c:116's DBG_ASSERT(cpu->prefixes == 0)) */
	if (cpu->prefixes != 0) {
		DBG_ASSERT(false);
		return 0;
	}

	/* the code window is the virtual page the block starts in, and it must be RAM: the same
	 * invariant get_phys_eip asserts for a single instruction (cpu.c:99) */
	if (in_mapped_range(phys_start)) {
		uop_last_stop = UOP_STOP_NOT_RAM;
		return 0;
	}

	dec_base_virt = virt_start;
	dec_base_phys = phys_start;
	/* S3: the page's end from the address's own width -- and THIS IS A DECLARED BEHAVIOUR
	 * CHANGE, not a no-op spelling fix (round 2's review, S2).
	 *
	 * The old expression was `(virt_start | 0xFFF) + 1` into a uint32_t.  For every
	 * `virt_start < 0xFFFFF000` that is this same number.  For `virt_start` in the LAST
	 * virtual page (0xFFFFF000..0xFFFFFFFF) it WRAPPED TO 0, so `dec_fetch` failed its
	 * `virt >= dec_end_virt` test on the FIRST byte, the walk overran, n == 0,
	 * uop_block_decode() returned NULL and the interpreter ran the instruction instead --
	 * every time, since the wrap was in the window's construction and not in the code.
	 * With the masked form the window is real, so a block at the last page now DECODES
	 * (and, with --jit on, compiles).
	 *
	 * WHY THAT IS A FIX AND WHAT IT COSTS.  The refused block was a fallback, and the
	 * fallback is the reference's own loop, so nothing was ever WRONG there -- the change is
	 * that a legal 32-bit guest which pages the last page into RAM and jumps to it now gets
	 * the cached/compiled engine, where it previously always got the interpreter.  The two
	 * engines are held byte-for-byte equal by the whole-state differentials, and the last
	 * page is now IN that differential rather than outside it (jit_codegen_selftest's
	 * test_last_page: three arms, a 4 KiB page table, and the decodes/compiles that show
	 * which arm ran).  What it could change if the engines ever disagreed there: the guest
	 * would now see the compiled arm's answer where it used to see the interpreter's --
	 * which is exactly what the new test is for, and why it is a gate rather than a note.
	 * The WINDOW itself did not widen: dec_fetch still refuses the byte past the page. */
	dec_end_virt = (virt_start & ~(uint64_t)0xFFF) + 0x1000u;
	dec_overrun = false;

	uint64_t eip = virt_start;
	int n = 0;
	int stop = UOP_STOP_PAGE_END;

	for (;;) {
		if (n >= cap || n >= UOP_MAX_BLOCK_UOPS) {
			stop = UOP_STOP_CAP;
			break;
		}
		if ((eip - virt_start) >= UOP_MAX_BLOCK_BYTES) {
			stop = UOP_STOP_CAP;
			break;
		}

		/* the prefix chain: one instruction in the reference, N dispatches of prefix
		 * bodies (instructions.c:606-627, 1880-1918) -- the state handed to the final
		 * dispatch is the same accumulated `pfx` either way.  It is accumulated PER
		 * INSTRUCTION: the reference clears cpu->prefixes after every loop iteration
		 * (cpu.c:127's DBG_ASSERT(cpu->prefixes == 0)), and a prefix that leaked into the
		 * next instruction would change its operand size -- MEASURED: the first version of
		 * this walk kept one accumulator across the block, and the checker's (a) caught it
		 * on the `05 imm32` at a stream's 9th byte (interpreter 3, decoder 5). */
		uint8_t pfx = 0;
		uint64_t inst_start = eip;
		int byte;
		for (;;) {
			byte = dec_fetch(eip);
			if (dec_overrun) {
				stop = UOP_STOP_PAGE_END;
				goto done;
			}
			if (!dec_prefix((uint8_t)byte, &pfx)) break;
			eip += 1;
		}

		dec_eip = eip;
		dec_pfx = pfx;
		uop_t *u = &out[n];
		memset(u, 0, sizeof *u);
		/* the sentinels the extraction and the materialisation distinguish from a real
		 * register index 0 (EAX) / segment 0 (ES): zeroed storage is NOT "no register" */
		u->base = UOP_REG_NONE;
		u->index = UOP_REG_NONE;
		u->g = UOP_REG_NONE;
		u->seg = UOP_SEG_NONE;
		u->addr_kind = UOP_ADDR_NONE;
		int r = dec_instruction(u, inst_start);
		if (r == DEC_PAGE_END) {
			stop = UOP_STOP_PAGE_END;
			goto done;
		}
		if (r == DEC_NODESC) {
			/* no descriptor: the uop is NOT part of the block, and the interpreter
			 * runs this instruction (plan ss2.2 item 2 / ss7's enablement lever) */
			stop = UOP_STOP_NO_DESC;
			goto done;
		}

		u->off = (uint16_t)(eip - virt_start);
		n += 1;
		eip = dec_eip;

		if (u->flags & UOP_FLAG_BLOCK_END) {
			stop = UOP_STOP_BLOCK_END;
			goto done;
		}
	}

done:
	if (blk != NULL) {
		blk->phys_start = phys_start;
		blk->virt_start = virt_start;
		blk->phys_len = (uint16_t)(eip - virt_start);
		blk->state = (uint8_t)(cpu->state_flags & CACHED_MASK_ALL);
		blk->uop_count = (uint8_t)n;
		blk->stop = (uint8_t)stop;
		blk->stop_eip = eip;
	}
	uop_last_stop = stop;
	uop_last_stop_eip = eip;
	return n;
}

/* ==================================================================== *
 * M2 (unit PERF-4): THE EXECUTOR, THE CACHE AND THE INVALIDATION
 *
 * Design of record: results/perf-2-blockcache-plan.md ss2.3 (the generic executors), ss3
 * (the cache), ss4 (the invalidation), ss5.2 (THE STAIRCASE -- the part a naive cache gets
 * wrong), ss7's M2.  M1 (results/perf-3-uop-decode.md) built the decoder above; this half
 * makes it RUN.
 *
 * THE STAIRCASE, PRECISELY.  `instruction_counter` is the port's virtual clock: it is added
 * ONCE PER INTERPRETER BATCH (cpu.c:148) and it is what microtick() divides
 * (exec_boundary.c:85-88), so it drives the TSC (rdtsc), the PIT and the PM timer -- all
 * guest-readable.  An interpreter batch ends at: an instruction whose generated postfix calls
 * after_block_boundary() (jit_block_boundary), a page-crossing instruction, the iteration
 * limit, and the `unimplemented` stop; and the loop's TAIL fetch fault DISCARDS the batch's
 * whole count (cpu.c:142-145 returns before `+= i`).
 *
 * A cache block SPANS many reference batches (the block ends at the table's mode-changing
 * instructions, not at the postfix), so the executor must flush the counter at every one of
 * those boundaries to reproduce the counter value the guest would read at every instruction
 * boundary.  The rules, and where they come from:
 *
 *   - `pending` counts the instructions of the CURRENT reference batch.  It is flushed
 *     (instruction_counter += pending) when a uop whose UOP_FLAG_FLUSH is set has run
 *     (cpu.c:132's jit_block_boundary), on a page-crossing uop (cpu.c:132's page test), and
 *     on the iteration limit (cpu.c:136);
 *   - a uop whose TS guard FAILS is NOT a flush point: the generated body `return`s before
 *     its postfix (gen/interpreter_c.js:200-203, :187-190), so after_block_boundary() never
 *     runs for it;
 *   - the tail-fetch fault DISCARDS pending (uop_cur_block's BX_DISCARD below);
 *   - the SMC abort KEEPS pending (plan ss4.2): the write is invisible to the reference's
 *     batch, which runs straight through it.
 *
 * WHY pending IS A LOCAL AND NOT A GLOBAL (a stated divergence from the plan's ss5.2).  The
 * plan has pending accumulate "across cycle_internal returns".  That cannot be done: both
 * cycle_internal's `DBG_ASSERT(instruction_counter != initial_instruction_counter)`
 * (cpu.c:169) and do_many_cycles_native's LOOP_COUNTER test (cpu.c:178) read the counter,
 * and a cycle_internal that returned with pending unflushed would move neither.  The
 * equivalent-and-observably-identical shape is used instead: uop_run never returns mid-batch.
 * It runs block after block (decoding the next one at a cap end, re-decoding after a
 * control-flow exit or an SMC abort) until a REFERENCE BATCH actually ends, and only then
 * returns to cycle_internal.  The counter's value at every instruction boundary is the same
 * either way -- which is the whole requirement -- and cycle_internal's own two checks keep
 * their meaning.  See results/perf-4-uop-exec.md ss2.
 * ==================================================================== */

/* OFF HERE, ON IN THE SHIPPED BINARY.  The switch is a property of the PROGRAM, not of the
 * library: src/main.c turns it on unless `--uop-cache off` is given (the M2 default), so
 * every gate that runs build/v86c exercises the cache -- while the SELFTEST BINARIES, which
 * link these sources and drive the machine from their own harnesses, keep the pre-M2
 * interpreter.  That matters beyond taste: those harnesses write guest RAM with plain
 * `mem8[..] = ..` (they build code images by hand), which is not a path the port's own write
 * hooks can see, so a cache running under them would execute stale blocks (MEASURED: linking
 * cpu_m1c_selftest against a cache that defaults ON segfaults -- its R5F test rewrites the
 * code page directly between cycle_internal calls).  In the SHIPPED machine every guest-RAM
 * write goes through memory.c/paging.c/string.c, whose hook coverage is the M2
 * reconciliation gate (results/perf-4-uop-exec.md ss3). */
bool uop_cache_enabled = false;
uop_stats_t uop_stats;

#define UOP_ARENA_UOPS 65536u /* plan ss3: "64K uops, ~3 MB" */
#define UOP_MAX_BLOCKS 8192u
#define UOP_HASH_SLOTS 16384u /* a power of two: the probe is `& (SLOTS-1)` */
#define UOP_HASH_EMPTY 0u
#define UOP_HASH_DEAD  0xFFFFu

typedef struct uop_cblock {
	uint32_t phys_start;
	uint64_t virt_start; /* S3: the block key's virtual half (uop_hash_pos folds both halves) */
	uint32_t uop_begin;   /* index into the arena */
	uint16_t phys_len;    /* bytes covered -- never crosses a page (the M1 block shape) */
	uint16_t next_page;   /* the page registry's link: block id + 1, 0 = end of list */
	uint16_t hash_slot;   /* where this block sits in the hash (for the unlink) */
	uint8_t state;        /* cpu->state_flags & CACHED_MASK_ALL */
	uint8_t uop_count;
	/* JIT-PLAN (the plan's ss8 step 4): the compiled block for THIS record, or NULL.  It
	 * lives on the block record and not in a second hash for one reason: the run path must
	 * NOT look the block up twice.  uop_run already found `blk`; the compiled code pointer
	 * is one load away from it, and jit_codegen.c's own keyed table is consulted at COMPILE
	 * time only (to find an already-compiled block for the key before emitting a new one). */
	uint8_t *code;
} uop_cblock_t;

static uop_t *uop_arena;
static uint32_t uop_arena_used;
static uop_cblock_t *uop_blocks;
static uint32_t uop_block_used;
static uint16_t *uop_hash;
static uint16_t *uop_page_head; /* per physical page: block id + 1, 0 = none */
static uint32_t uop_page_count;
uint8_t *uop_page_code;         /* the hot test the write sites read (uop_cache.h) */
uint32_t uop_page_code_len;
static bool uop_ready;

/* the currently executing block, for the SMC check (plan ss4.2), and the flag the hook sets */
static uop_cblock_t *uop_cur_block;
static bool uop_abort_pending;

void uop_stats_reset(void)
{
	memset(&uop_stats, 0, sizeof uop_stats);
}

static void uop_clear_internal(void)
{
	memset(uop_hash, 0, (size_t)UOP_HASH_SLOTS * sizeof uop_hash[0]);
	if (uop_page_head) {
		memset(uop_page_head, 0, (size_t)uop_page_count * sizeof uop_page_head[0]);
		memset(uop_page_code, 0, uop_page_code_len);
	}
	uop_arena_used = 0;
	uop_block_used = 0;
	/* THE COMPILED BLOCKS GO WITH THE DECODED ONES, and this is a CORRECTNESS requirement
	 * rather than tidiness: the memset above zeroes the shared `uop_page_code` bitmap, which
	 * is the ONLY thing that makes the PERF-FIX-3 write sites call the invalidator for a page
	 * whose blocks are compiled.  A compiled block left alive here would no longer be watched
	 * -- a later guest write to its page would reach neither registry and the stale code would
	 * run.  (A no-op while --jit is off.) */
	jit_code_clear_all();
}

void uop_cache_clear(void)
{
	if (!uop_ready) return;
	uop_clear_internal();
	uop_stats.clears++;
}

bool uop_cache_init(void)
{
	if (uop_ready) return true;

	uint32_t alloc = mem8_alloc_size();
	if (alloc == 0) return false;

	uop_arena = (uop_t *)malloc((size_t)UOP_ARENA_UOPS * sizeof(uop_t));
	uop_blocks = (uop_cblock_t *)malloc((size_t)UOP_MAX_BLOCKS * sizeof(uop_cblock_t));
	uop_hash = (uint16_t *)malloc((size_t)UOP_HASH_SLOTS * sizeof(uint16_t));
	uop_page_count = alloc >> 12;
	if (uop_page_count == 0) return false;
	uop_page_head = (uint16_t *)calloc((size_t)uop_page_count, sizeof(uint16_t));
	uop_page_code = (uint8_t *)calloc((size_t)uop_page_count, 1);
	uop_page_code_len = uop_page_count;

	if (uop_arena == NULL || uop_blocks == NULL || uop_hash == NULL ||
	    uop_page_head == NULL || uop_page_code == NULL) {
		/* NO SILENT STUB: the cache reports that it cannot run and every caller falls back
		 * to interpreted_run (uop_run's first act), i.e. the machine is the pre-M2 one. */
		fprintf(stderr, "uop_cache: out of memory: the uop cache is disabled\n");
		free(uop_arena); free(uop_blocks); free(uop_hash);
		free(uop_page_head); free(uop_page_code);
		uop_arena = NULL; uop_blocks = NULL; uop_hash = NULL;
		uop_page_head = NULL; uop_page_code = NULL;
		uop_page_count = 0; uop_page_code_len = 0;
		return false;
	}

	uop_arena_used = 0;
	uop_block_used = 0;
	uop_clear_internal();
	uop_ready = true;
	return true;
}

static inline uint32_t uop_hash_pos(uint32_t phys, uint64_t virt, uint8_t state)
{
	/* S3: the virtual half is 64 bits now, so BOTH halves are mixed -- a hash that took only
	 * the low half would put two blocks differing above 4 GiB in the same slot, which the
	 * lookup's full-width compare would then treat as a MISS and re-decode every time (not
	 * wrong, but a silent performance cliff and a cache that never hits). */
	uint32_t h = phys * 0x9E3779B1u + (uint32_t)virt * 0x85EBCA6Bu +
	             (uint32_t)(virt >> 32) * 0x27D4EB2Fu + ((uint32_t)state + 1u) * 0xC2B2AE35u;
	h ^= h >> 15;
	return h & (UOP_HASH_SLOTS - 1u);
}

/* Open addressing with linear probing and LAZY deletion: a dead slot does not terminate a
 * probe (only an empty one does), and an insert reuses the first dead slot it saw -- which
 * keeps a live entry reachable from its own hash position while letting an invalidated page
 * give its slots back. */
static uop_cblock_t *uop_block_lookup(uint32_t phys, uint64_t virt, uint8_t state)
{
	if (!uop_ready) return NULL;

	uop_stats.lookups++;
	uint32_t slot = uop_hash_pos(phys, virt, state);
	for (uint32_t n = 0; n < UOP_HASH_SLOTS; n++) {
		uint16_t v = uop_hash[slot];
		if (v == UOP_HASH_EMPTY) return NULL;
		if (v != UOP_HASH_DEAD) {
			uop_cblock_t *b = &uop_blocks[v - 1];
			if (b->phys_start == phys && b->virt_start == virt && b->state == state) {
				uop_stats.hits++;
				return b;
			}
		}
		slot = (slot + 1u) & (UOP_HASH_SLOTS - 1u);
	}
	return NULL;
}

static void uop_hash_insert(uop_cblock_t *b, uint32_t id)
{
	uint32_t slot = uop_hash_pos(b->phys_start, b->virt_start, b->state);
	uint32_t first_dead = UOP_HASH_SLOTS;

	for (uint32_t n = 0; n < UOP_HASH_SLOTS; n++) {
		uint16_t v = uop_hash[slot];
		if (v == UOP_HASH_EMPTY) break;
		if (v == UOP_HASH_DEAD && first_dead == UOP_HASH_SLOTS) first_dead = slot;
		slot = (slot + 1u) & (UOP_HASH_SLOTS - 1u);
	}
	if (uop_hash[slot] != UOP_HASH_EMPTY && first_dead != UOP_HASH_SLOTS) slot = first_dead;
	b->hash_slot = (uint16_t)slot;
	uop_hash[slot] = (uint16_t)(id + 1u);
}

/* Decode and install a block at (phys, virt).  NULL means the decoder produced no block:
 * the caller must run the instruction through the reference's own path. */
static uop_cblock_t *uop_block_decode(uint32_t phys, uint64_t virt, uint8_t state)
{
	if (uop_block_used >= UOP_MAX_BLOCKS ||
	    uop_arena_used + UOP_MAX_BLOCK_UOPS > UOP_ARENA_UOPS) {
		/* plan ss3's overflow policy: clear the whole cache.  Correctness is unaffected
		 * (decode is a pure function of the bytes); the cost is a re-decode wave, and the
		 * count is reported (uop_stats.clears) rather than hidden. */
		uop_clear_internal();
		uop_stats.clears++;
	}

	uop_t *out = &uop_arena[uop_arena_used];
	uop_block_t blk;
	int n = uop_decode_block(out, (int)UOP_MAX_BLOCK_UOPS, virt, phys, &blk);
	if (n <= 0) {
		uop_stats.decode_fails++;
		return NULL;
	}
	DBG_ASSERT(n <= (int)UOP_MAX_BLOCK_UOPS);
	DBG_ASSERT((phys >> 12) == ((phys + blk.phys_len - 1u) >> 12)); /* one page per block */

	uint32_t id = uop_block_used++;
	uop_cblock_t *b = &uop_blocks[id];
	b->phys_start = phys;
	b->virt_start = virt;
	b->uop_begin = uop_arena_used;
	b->phys_len = blk.phys_len;
	b->state = state;
	b->uop_count = (uint8_t)n;

	uop_arena_used += (uint32_t)n;

	/* the M3 sizing evidence (plan ss3): the high-water marks, not just the clear count --
	 * a clear happens only AFTER the pool has overflowed, so `clears == 0` alone cannot say
	 * how close the benchmark came to it (PERF-5's G1/G2, results/perf-5-tune.md ss6) */
	if (uop_arena_used > uop_stats.arena_peak) uop_stats.arena_peak = uop_arena_used;
	if (uop_block_used > uop_stats.block_peak) uop_stats.block_peak = uop_block_used;

	uint32_t page = phys >> 12;
	b->next_page = uop_page_head[page];
	uop_page_head[page] = (uint16_t)(id + 1u);
	uop_page_code[page] = 1;
	uop_hash_insert(b, id);

	/* JIT-PLAN: COMPILE THE BLOCK (plan ss2: compile at decode, no hotness gate, so that
	 * `--jit on` is a deterministic full-JIT mode and the oracle is total).  NULL means "the
	 * compiler declined" -- the emission does not fit in one code page, the code pool is
	 * exhausted, or this is not an x86-64 build -- and the block then executes exactly as it
	 * did before the JIT existed.  This is decode-time work, so it is off every run path. */
	b->code = NULL;
	if (jit_enabled) {
		/* S3: THE JIT'S OWN KEY STAYS 32-BIT.  jit_compile_block and every recipe take a
		 * 32-bit eip (jit_codegen.h), and the compiled code's ip comparisons read the low
		 * half of instruction_pointer -- exact while the high half is zero, which is the
		 * invariant outside long mode.  The projection is NAMED here so that the widening
		 * of the JIT (S13's recipes, reachable once S5 enters long mode) starts from a
		 * stated boundary instead of an implicit narrowing. */
		b->code = jit_compile_block(&uop_arena[b->uop_begin], (uint32_t)n, phys,
		                            (uint32_t)virt, state);
	}

	uop_stats.decodes++;
	return b;
}

/* ------------------------------------------------------------------ *
 * the invalidation (plan ss4.1) and the SMC abort (ss4.2)
 * ------------------------------------------------------------------ */

void uop_invalidate_page(uint32_t phys_addr)
{
	if (!uop_ready) return;
	uint32_t page = phys_addr >> 12;
	if (page >= uop_page_count) return;

	/* ss4.2 FIRST, and unconditionally: the write may be anywhere in the page the executing
	 * block is on (over-aborting is correct, just less fast).  Keeping `pending` is the
	 * point -- the reference's batch runs straight through the write. */
	if (uop_cur_block != NULL && (uop_cur_block->phys_start >> 12) == page) {
		uop_abort_pending = true;
		uop_stats.aborts++;
	}

	/* JIT-SPIKE: THE ONE INVALIDATION MECHANISM, REUSED -- not a second one.  A compiled block
	 * is dropped through THIS function's hook points (i.e. the PERF-FIX-3 write sites, via
	 * uop_invalidate_page_fast), which is why it is forwarded to from here and not from a
	 * write path of its own.  It is called BEFORE the registry's early return below because a
	 * page can hold a compiled block and NO decoded block: the registry test would otherwise
	 * skip it and the JIT would execute stale bytes.  It is a no-op while --jit is off (one
	 * load and a branch), so the shipped interpreter's cost here is unchanged. */
	jit_code_invalidate_page(phys_addr);

	uint16_t idp = uop_page_head[page];
	if (idp == 0) {
		/* no decoded block left, but the page may still have been marked by a compiled one
		 * (jit_cache_bind sets the same bit): clear the shared bit here, where BOTH
		 * registries are in view, rather than letting the JIT clear it and possibly
		 * unwatch a page that still holds decoded blocks. */
		uop_page_code[page] = 0;
		return;
	}

	uop_page_head[page] = 0;
	uop_page_code[page] = 0;
	while (idp != 0) {
		uop_cblock_t *b = &uop_blocks[idp - 1];
		idp = b->next_page;
		b->next_page = 0;
		b->uop_count = 0;
		/* lazy deletion: the hash slot becomes dead, so a probe for another key still
		 * walks past it and a later insert can reuse it */
		uop_hash[b->hash_slot] = UOP_HASH_DEAD;
		uop_stats.invalidations++;
	}
}

void uop_invalidate_range(uint32_t phys_addr, uint32_t length)
{
	if (!uop_ready || length == 0) return;
	uint32_t first = phys_addr >> 12;
	uint32_t last = (phys_addr + length - 1u) >> 12;
	for (uint32_t page = first; page <= last; page++) {
		if (page >= uop_page_count) break;
		/* JIT-SPIKE: the test is the SHARED bitmap, not the uop registry -- a page holding only
		 * a compiled block has no entry in uop_page_head and would be skipped.  For a
		 * uop-cache-only tree the two are the same predicate (they are set and cleared
		 * together), so this is not a behaviour change when --jit is off. */
		if (uop_page_code[page] != 0) uop_invalidate_page(page << 12);
	}
}

/* ------------------------------------------------------------------ *
 * the generic executors (plan ss2.3)
 * ------------------------------------------------------------------ */

/* Returns true when the BODY RAN.  false means the instruction's BODY DID NOT RUN -- either
 * the task_switch_test[_mmx] guard failed, or the address materialisation faulted.  In BOTH
 * cases the generated body `return`s (gen/interpreter_c.js:200-203 for the guard, :261-273 for
 * the resolve) BEFORE the call and before its postfix, so the instruction neither ran nor
 * ended a reference batch -- the caller must not flush.
 *
 * THE GUARD'S ORDER IS DELIBERATE AND IS THE REFERENCE'S (PERF-FIX-3): the guard is the
 * generated body's `instruction_prefix` and the fence is emitted BEFORE the mem call whose
 * argument performs the resolve -- upstream v86/gen/generate_interpreter.js:233-251 (the
 * prefix) + :315-330 (the call carrying `modrm_resolve`), and the port's own generated
 * interpreter (src/cpu/interpreter_gen.c:12010-12017 for fxsave, :4994-5011 for the FPU D8
 * group).  The review's SHOULD-FIX-2a proposed materialising FIRST, on the claim that the
 * reference resolves before it guards; that is the opposite of the artifact, and the
 * reorder would DELIVER #GP where the reference delivers #NM/#UD (measured both ways -- see
 * results/perf-fix-3.md's G4: the reorder is caught by the guard-order pin, the tree as
 * built is not).  The plan says the same thing in one sentence (perf-2-blockcache-plan.md
 * ss4.4: "the uop runs the guard at EXECUTE time and materializes only if it passes").
 *
 * WHAT WAS WRONG (SHOULD-FIX-2b, the half of the fix that stands) is the fault's RETURN: it
 * used to be `true`, which the caller read as "the body ran" and turned into uop_flush() for
 * a FLUSH-flagged encoding -- a reference batch end the reference does not have, because its
 * resolve-fault `return` skipped the postfix. */
static bool uop_exec(const uop_t *u)
{
	if (u->ts_guard != UOP_TS_NONE) {
		bool ok = u->ts_guard == UOP_TS_MMX ? task_switch_test_mmx() : task_switch_test();
		if (!ok) return false;
	}

	/* THE MEMORY SHAPES' FIRST ARGUMENT.  Normally it is the materialised linear address.
	 * For a custom_modrm_resolve encoding (0x8D lea, 0x8F pop) the generated dispatcher
	 * passes the RAW MODRM BYTE instead and the body does its own read
	 * (gen/interpreter_c.js:236-273), so the two differ -- and the difference is invisible
	 * in the shape, because a fixed_g encoding takes the MEMF0/MEMF1 shapes exactly like a
	 * non-fixed_g one takes MEM0/MEM1.  MEASURED (this cost the M2 boot gate its first
	 * run): handling the substitution only in the MEM0 arm left 0x8F -- the ONE fixed_g
	 * custom_modrm_resolve encoding, `pop r/m32`, rare enough to be reached 477M
	 * instructions into a kernel boot -- calling `instr32_8F_0_mem(<linear address>)`, i.e.
	 * resolve_modrm16/32 on 32 bits of address, which tripped the port's own
	 * `default: DBG_ASSERT(false)` in the addressing switch.  The substitution now happens
	 * once, before the shape switch, for every memory shape. */
	int32_t arg0 = 0;
	if (u->shape == UOP_SHAPE_MEM0 || u->shape == UOP_SHAPE_MEM1 ||
	    u->shape == UOP_SHAPE_MEMF0 || u->shape == UOP_SHAPE_MEMF1) {
		if (u->flags & UOP_FLAG_FETCH_INTERNAL) {
			arg0 = (int32_t)u->modrm;
		}
		else {
			arg0 = uop_materialize_addr(u);
			/* modrm_resolve's own return_on_pagefault! -- the body never runs, so its
			 * postfix never runs and a FLUSH-flagged encoding is NOT a batch end here
			 * (PERF-FIX-3/SHOULD-FIX-2b: returning `true` used to trigger uop_flush()
			 * at the caller for an instruction the reference's resolve-fault `return`
			 * skipped -- a mid-batch counter step a same-page handler's rdtsc can see).
			 * Nothing else reads the caller's `ran`, and the instruction is still counted
			 * (uop_pending += 1 at the caller is unconditional, like the reference's
			 * unconditional `i += 1`). */
			if (cpu->pagefault) return false;
		}
	}

	switch (u->shape) {
	case UOP_SHAPE_PLAIN0:
		((void (*)(void))u->fn)();
		break;
	case UOP_SHAPE_PLAIN1:
		((void (*)(int32_t))u->fn)(u->imm);
		break;
	case UOP_SHAPE_PLAIN2:
		((void (*)(int32_t, int32_t))u->fn)(u->imm, u->imm2);
		break;
	case UOP_SHAPE_MEM0:
		((void (*)(int32_t, int32_t))u->fn)(arg0, (int32_t)u->g);
		break;
	case UOP_SHAPE_MEM1:
		((void (*)(int32_t, int32_t, int32_t))u->fn)(arg0, (int32_t)u->g, u->imm);
		break;
	case UOP_SHAPE_MEMF0:
		((void (*)(int32_t))u->fn)(arg0);
		break;
	case UOP_SHAPE_MEMF1:
		((void (*)(int32_t, int32_t))u->fn)(arg0, u->imm);
		break;
	case UOP_SHAPE_REG0: /* modrm >> 3 & 7 is u->g, computed at decode */
		((void (*)(int32_t, int32_t))u->fn)((int32_t)(u->modrm & 7), (int32_t)u->g);
		break;
	case UOP_SHAPE_REG1:
		((void (*)(int32_t, int32_t, int32_t))u->fn)(
			(int32_t)(u->modrm & 7), (int32_t)u->g, u->imm);
		break;
	case UOP_SHAPE_REGF0:
		((void (*)(int32_t))u->fn)((int32_t)(u->modrm & 7));
		break;
	case UOP_SHAPE_REGF1:
		((void (*)(int32_t, int32_t))u->fn)((int32_t)(u->modrm & 7), u->imm);
		break;
	case UOP_SHAPE_UD:
		/* the decoder never emits it (a no-descriptor instruction ends the block and is run
		 * by the reference's own path), but the shape table is total */
		trigger_ud();
		break;
	default:
		DBG_ASSERT(false);
		break;
	}
	return true;
}

/* JIT (M2): the executor's per-uop worker, for the compiled block's fallback (uop_cache.h).
 * One line, and it exists so there is ONE implementation of the shape switch -- the compiled
 * block calls this rather than emitting a second copy of it. */
bool uop_exec_uop(const uop_t *u)
{
	return uop_exec(u);
}

/* ------------------------------------------------------------------ *
 * the run loop (plan ss2.3's skeleton, restructured per this file's header)
 * ------------------------------------------------------------------ */

/* THE REFERENCE BATCH'S PROGRESS, AND WHY IT IS A GLOBAL.
 *
 * `uop_pending` counts the instructions executed since the last reference BATCH end.  It is
 * flushed by uop_flush() and it must SURVIVE a return from uop_run -- which is the plan's ss5.2
 * sentence ("pending accumulates executed uops across cache blocks and across cycle_internal
 * returns until a reference batch end", results/perf-2-blockcache-plan.md), and it is not
 * decoration.  MEASURED, and it cost this unit a 2-instruction counter loss on the boot:
 * `sti` is a BLOCK end but NOT a reference batch end.  M1 forced UOP_DESC_BLOCK_END for it
 * (x86_table.js's entry has custom_sti and NO block_boundary, so instruction_postfix never
 * fires for it -- results/perf-3-uop-decode.md ss5.2), because instr_FB executes a shadow
 * instruction inside its own body and a uop after it would execute that shadow twice.  So at
 * an sti the cache must END THE BLOCK but must NOT flush the counter: the reference's batch
 * runs straight through the sti and the instructions after it.  With `pending` a LOCAL, the
 * block-end return dropped those instructions' count on the floor --
 * MEASURED: at ip 0x80299 the sti's batch was [sti, ...3 more] = i 4 in the reference
 * (counter 3144 -> 3149; the sti's own body contributes one of those, instructions.c:2179)
 * while the cache returned at the sti with pending 2 and never counted it, and every counter
 * after that point was 2 low -- for the whole boot (200,095,212 vs 200,095,214).
 *
 * The other consequence, stated because it is the reason the plan's shape is needed at all:
 * cycle_internal may now return with the counter unmoved (a block end that is not a batch
 * end).  Its progress check therefore accepts EITHER kind of progress -- see cpu.c. */
uint32_t uop_pending;

/* do_many_cycles_native's OWN break test, evaluated where the reference evaluates it: right
 * after a reference batch end (cpu.c:178).  See cpu.h's note on cpu_cycle_budget_end -- a
 * cache run ends several batches inside one cycle_internal, so without this the LOOP_COUNTER
 * window would overshoot and main_loop would run the hardware timers at a different
 * instruction count (which the guest can see).
 *
 * U64-COUNTER: the wrapped-signed-difference test below is the reference's u32
 * `wrapping_sub` semantics (cpu.rs:3419), and it is correct ONLY when both operands are in
 * the same 32-bit domain -- the counter's LOW 32 BITS minus the (u32) window end, both
 * wrapping.  The `(uint32_t)` cast is therefore LOAD-BEARING: widening the counter without
 * it would subtract a u64 from a u32 and truncate the WRONG bits to int32, and the budget
 * window would never close past 2^32.  The low 32 bits wrap exactly as the old u32 counter
 * did, so the staircase is unchanged for any counter value. */
static inline bool uop_budget_reached(void)
{
	return (int32_t)((uint32_t)cpu->instruction_counter - cpu_cycle_budget_end) >= 0;
}

/* the reference's `*instruction_counter += i` (cpu.c:148) */
static inline void uop_flush(void)
{
	cpu->instruction_counter += uop_pending;
	uop_pending = 0;
	uop_stats.flushes++;
}

/* THE DISCARD (cpu.c:142-145): the reference returns from its loop BEFORE `+= i`, so the whole
 * batch's count is lost.  Reproduced, and it is the only path that drops a count. */
static inline void uop_discard(void)
{
	uop_pending = 0;
	uop_stats.discards++;
}

void uop_run(uint32_t phys_addr)
{
	uint32_t phys = phys_addr;

	/* S5a (results/x86-64-plan.md ss6 S5, risk R1): THE DECODER'S LONG-MODE REFUSAL.
	 *
	 * This decoder reads the instruction stream with 32-BIT RULES: `0x40`-`0x4F` are
	 * INC/DEC (dec_prefix has no case for them), so a REX byte is consumed as a whole
	 * instruction and the LENGTH of everything after it is wrong from there on -- a
	 * desynchronised stream that still "runs", which is the silent channel the plan names
	 * as R1 and the reason this seat lands BEFORE any 64-bit byte can execute.
	 *
	 * THREE LAYERS, and this is the one that is a REFUSAL rather than a capability:
	 *   1. the capability word -- no engine but `interp` declares CPU_MODE_LONG, so the
	 *      seam's per-batch check (cpu_backend_dispatch) refuses the mode by name;
	 *   2. the transition -- cpu_backend_refuse_long_mode (S2) refuses the three writes
	 *      that would ENTER IA-32e when the selected engine cannot run it, so a machine
	 *      with the cache on never gets here at all;
	 *   3. THIS: the decoder refuses by name even if it is entered with EFER.LMA set
	 *      (a direct call, a future engine whose capability word is wrong, or a batch
	 *      that spans a mode change).  A seat that cannot be reached through today's
	 *      dispatch is still a seat that must not silently decode 64-bit bytes with
	 *      32-bit rules; cpu_selftest's S5 arm puts LMA there by hand and requires this
	 *      stop (the plan's "an untested seat is indistinguishable from an absent one").
	 * The 64-bit decode is S13's, and its gate is the engine differential. */
	if ((cpu->efer & EFER_LMA) != 0) {
		unimplemented_instruction(
			"uop cache: the 64-bit decoder is S13's; long mode runs on the interpreter only");
		return;
	}

	if (!uop_ready && !uop_cache_init()) {
		interpreted_run(phys);
		return;
	}

	/* interpreted_run's own per-batch reset (cpu.c:108).  It is the GENERATED dispatcher's
	 * verdict and the fallback's loop test reads it; the reference clears it in exactly this
	 * place, and nothing can leave it set across a re-entry (every reader clears it). */
	jit_block_boundary = false;

	for (;;) {
		uint8_t state = (uint8_t)(cpu->state_flags & CACHED_MASK_ALL);
		/* S3: the key is (phys, the full virtual address, state) -- the virtual half is no
		 * longer projected, so a 64-bit eip keys its own block */
		uop_cblock_t *blk = uop_block_lookup(phys, cpu->instruction_pointer, state);
		if (blk == NULL) blk = uop_block_decode(phys, cpu->instruction_pointer, state);

		if (blk == NULL) {
			/* THE CACHE CANNOT RUN THIS INSTRUCTION (plan ss5.1's fallback). */
			if (uop_pending == 0) {
				/* a reference batch starts here, so the reference's own loop is exactly
				 * the right thing -- verbatim, not a re-implementation */
				uint64_t before = cpu->instruction_counter;
				uop_stats.fallback_runs++;
				interpreted_run(phys);
				/* REBOOT-FIX-1/N1: a reboot inside the fallback zeroed the counter
				 * mid-run, so the unsigned difference would wrap (to ~4e9 pre-u64, to
				 * ~2^64 now).  The rebooted batch's count is not added: the baseline
				 * it would be measured against no longer exists. */
				if (!cpu->reboot_requested)
					uop_stats.fallback_instr += cpu->instruction_counter - before;
				return;
			}

			/* MID-BATCH: the reference's loop body for ONE instruction (cpu.c:111-146).
			 * The batch continues THROUGH this instruction, so nothing is flushed -- this
			 * is the page-crossing instruction and the no-descriptor instruction, the two
			 * the decoder refuses. */
			uop_stats.fallback_steps++;
			uop_pending += 1;
			uint64_t start_eip = cpu->instruction_pointer;
			int32_t opcode = (int32_t)mem8[phys];
			set_instruction_pointer(cpu->instruction_pointer + 1);
			cpu->pagefault = false;
			DBG_ASSERT(cpu->prefixes == 0);
			/* S5a: the same three-valued key as the interpreter's loop (modrm.c).  This path
			 * is unreachable in long mode -- uop_run refuses the mode at its entry -- so the
			 * call is the 32-bit answer here; it is the SAME FUNCTION so the two loops cannot
			 * disagree about the key's shape when S13 gives this decoder its arms. */
			run_instruction(opcode | dispatch_os_index(opcode) << 8);
			cpu->pagefault = false;

			if (interpreted_unimplemented) {
				uop_flush();
				return;
			}
			if (cpu->reboot_requested) {
				/* REBOOT-1: same verdict as the block path -- the CPU was reset
				 * out from under this batch (and possibly without a page cross,
				 * if the trigger ran from the firmware's own page). */
				uop_flush();
				return;
			}
			/* S3: both tests at the field's own width (identical for any 32-bit eip) */
			if (jit_block_boundary
				|| (start_eip >> 12) != (cpu->instruction_pointer >> 12)
				|| (uop_pending >= INTERPRETER_ITERATION_LIMIT
					&& start_eip >= cpu->instruction_pointer)) {
				jit_block_boundary = false;
				uop_flush();
				return;
			}
			cpu->previous_ip = cpu->instruction_pointer;
			phys = get_phys_eip();
			if (cpu->pagefault) {
				uop_discard();
				return;
			}
			if (uop_budget_reached()) {
				uop_flush();
				return;
			}
			continue;
		}

		/* ---------------- execute one block ---------------- */
		if (blk->code != NULL) {
			/* THE COMPILED BLOCK (plan ss8 step 4).  ONE lookup, already done: `blk` is the
			 * record the hash returned and `code` is a field of it -- no second hash on the
			 * run path.  The compiled block reproduces the executor's per-uop contract and
			 * returns one of ITS OWN four outcomes; the continuation below is the SAME C
			 * code the interpreted block run falls into, so the tail, the staircase and the
			 * SMC re-decode are not reimplemented (jit_codegen.h).
			 *
			 * `uop_cur_block` is set for the duration exactly as the interpreted path sets
			 * it, so uop_invalidate_page's "is the write inside the block that is running"
			 * test -- and the abort counter that goes with it -- see the same thing. */
			uop_cur_block = blk;
			uop_abort_pending = false;
			int jit_why = jit_execute_block(blk->code);
			uop_cur_block = NULL;
			jit_codegen_stats.exits[jit_why & 7]++;

			/* REBOOT-1/FIX-1: on ANY exit kind, not just ABORT.  MEASURED corner
			 * (results/reboot-fix-1.md): the trigger port write is a FLUSH +
			 * BLOCK_END uop (uop_gen.c's descriptors for E6/EE carry both), so
			 * today's reboot exits are step 7's flush + step 8's RETURN or step
			 * 10's BLOCK_END -- never TAIL, and the batch always ends at the
			 * trigger.  This check is the contract, not the current reach: the
			 * TAIL arm's continue would otherwise re-look-up at the reset eip
			 * with jit_block_exit having already consumed jit_abort_flag, and a
			 * future trigger outside the port-write class would run boot-2
			 * instructions inside the stale batch.  On BLOCK_END it also flushes
			 * a pending count the clear at re-entry would otherwise drop. */
			if (cpu->reboot_requested) {
				uop_flush();
				return;
			}

			if (jit_why == JIT_EXIT_BLOCK_END) {
				/* a block end that is NOT a reference batch end (sti): the count stays
				 * pending and the next call runs the batch's next instruction */
				uop_stats.block_ends++;
				return;
			}
			if (jit_why == JIT_EXIT_RETURN) return;

			if (jit_why == JIT_EXIT_ABORT) {
				/* REBOOT-1 FIRST: machine_reboot() raised jit_abort_flag to stop
				 * THIS compiled block -- the triggering port write is inside it,
				 * and the CPU was reset out from under the block.  The SMC
				 * continuation below re-derives phys from the OLD block's
				 * geometry, which is garbage for the reset eip (0xFFFF0), so
				 * the reboot unwinds out of the executor instead, exactly like
				 * the interpreted path's per-uop check below; main.c clears the
				 * caches and re-establishes the boot before anything else runs. */
				if (cpu->reboot_requested) {
					uop_flush();
					return;
				}
				/* plan ss4.2, exactly as the interpreted path: the hook has already
				 * unlinked this page's blocks (the compiled one included), so the next
				 * lookup re-decodes the CURRENT bytes at the current eip and the pending
				 * count is KEPT (the write is invisible to the reference's batch).
				 *
				 * THE GEOMETRY IS THE ENTRY BLOCK'S (`blk`, the record the run was
				 * entered with), and that is EXACT -- but it is LOAD-BEARING that it stay
				 * that way, so the reasoning is written down here rather than left to a
				 * helper's name.  A chain link can move execution to another compiled
				 * block, so `blk` is not necessarily the block executing this uop; what
				 * makes `blk->phys_start + (ip - blk->virt_start)` the right phys for ANY
				 * ip a run can reach is jit_chain_target's SAME-PAGE GATE (jit_mem.c):
				 * it refuses a link whose target is not on the page of the fetch cache's
				 * last translation, so every block a run reaches is on the entry block's
				 * page.  That same gate is ALSO what keeps the uop side correct:
				 * uop_cur_block stays the entry block for the whole run and its page test
				 * in uop_invalidate_page -- and the uop_stats.aborts that goes with it --
				 * fire exactly when jit_abort_flag does only because every link is on its
				 * page.  WIDENING THE CHAIN ACROSS PAGES (adding the walk arm
				 * jit_chain_target deliberately refuses) WOULD SILENTLY BREAK BOTH: this
				 * fallback would compute a phys from the wrong page.  Do not widen it
				 * without re-deriving the geometry from the actually-executing block. */
				cpu->previous_ip = cpu->instruction_pointer;
				/* the executed ip is inside the block's page, so the difference is a
				 * page offset: the projection lands on the 32-bit physical model */
				phys = blk->phys_start +
				       (uint32_t)(cpu->instruction_pointer - blk->virt_start);
				continue;
			}

			/* JIT_EXIT_TAIL: the reference's loop tail (cpu.c:140-145).  On a fault the
			 * whole batch's count is discarded -- the reference's own quirk. */
			cpu->previous_ip = cpu->instruction_pointer;
			phys = get_phys_eip();
			if (cpu->pagefault) {
				uop_discard();
				return;
			}
			continue;
		}

		const uop_t *uops = &uop_arena[blk->uop_begin];
		uint32_t count = blk->uop_count;
		uint32_t idx = 0;
		int why; /* where the block run stopped */

		#define UOP_LEFT_RETURN 0 /* a reference batch ended: the counter is flushed */
		#define UOP_LEFT_TAIL   1 /* the reference's loop tail: previous_ip + fetch */
		#define UOP_LEFT_ABORT  2 /* SMC (ss4.2): re-decode at the current eip, KEEP the count */

		uop_cur_block = blk;
		uop_abort_pending = false;

		/* THE PER-UOP STATISTIC IS ACCUMULATED IN A LOCAL AND ADDED ONCE PER BLOCK RUN
		 * (PERF-5/M3).  `uop_stats.uops_executed` used to be bumped with a read-modify-write
		 * to a global on EVERY uop -- MEASURED cost, not a guess: 200,095,213 uops on the
		 * benchmark, and the increment cannot be kept in a register because the body is an
		 * indirect call.  Nothing reads the counter during a run (uop_stats_print at exit,
		 * the selftests between runs), so folding it per block run is observably identical
		 * and removes 200M dependent stores.  results/perf-5-tune.md ss5 has the delta. */
		uint64_t block_uops = 0;

		for (;;) {
			const uop_t *u = &uops[idx];

			if (uop_abort_pending) {
				why = UOP_LEFT_ABORT;
				break;
			}

			block_uops += 1;
			cpu->previous_ip = u->eip;
			cpu->prefixes = u->pfx;
			/* THE INSTRUCTION POINTER AT THE BODY'S ENTRY is the reference's: the generated
			 * dispatcher reads the opcode (and the modrm byte), modrm_resolve consumes the
			 * sib/disp and the immediate is read after it, so at the call the pointer is at
			 * the instruction's END.
			 *
			 * THE ONE EXCEPTION is a custom_modrm_resolve body (0x8D lea / 0x8F pop): it is
			 * handed the RAW modrm byte -- gen/interpreter_c.js:236-273 puts `modrm_byte`
			 * in the mem call's first argument -- and reads the sib/disp itself, so its
			 * pointer must sit just AFTER the modrm byte, i.e. pfx_len + opcode_bytes + 1.
			 * MEASURED (this cost the M2 gate its first run): the +1 is not decoration.
			 * Without it a no-displacement lea (8D 12, which reads nothing) leaves the
			 * pointer one byte SHORT of the instruction's end, the executor's own
			 * `u->eip + u->len != pointer` test then reads that as a control transfer, and
			 * the next block is decoded from the middle of the instruction -- the
			 * misaligned-uop cascade the gate caught as 4 failing nasm tests (lea-nop,
			 * leave16/32, push).  The plan's own ss2.3 skeleton (which has
			 * `u->eip + u->pfx_len + u->opcode_bytes`) is the version that is wrong. */
			set_instruction_pointer(u->body_ip);
			cpu->pagefault = false; /* D1's dispatch-entry clear (cpu.c:118) */
			bool ran = uop_exec(u);
			cpu->pagefault = false; /* D1's post-dispatch clear (cpu.c:126) */
			cpu->prefixes = 0;
			uop_pending += 1;

			if (interpreted_unimplemented) {
				uop_flush();
				why = UOP_LEFT_RETURN;
				break;
			}
			if (cpu->reboot_requested) {
				/* REBOOT-1: the trigger was a port write INSIDE this block
				 * (machine_reboot reset the CPU out from under it), so the
				 * remaining uops are the pre-reboot stream.  End the block;
				 * main.c clears the caches and re-establishes the boot before
				 * any further guest instruction executes. */
				uop_flush();
				why = UOP_LEFT_RETURN;
				break;
			}
			if (ran && ((u->flags & UOP_FLAG_FLUSH) || jit_block_boundary)) {
				/* THE STAIRCASE: cpu.c:132's jit_block_boundary break */
				uop_flush();
				jit_block_boundary = false;
				if (uop_budget_reached()) {
					/* do_many_cycles_native stops here in the reference (cpu.c:178):
					 * this batch end is where its window closed */
					why = UOP_LEFT_RETURN;
					break;
				}
			}
			if (((u->eip ^ cpu->instruction_pointer) & ~(uint64_t)0xFFFu) != 0) {
				uop_flush(); /* cpu.c:132's page-cross break */
				why = UOP_LEFT_RETURN;
				break;
			}
			if (uop_pending >= INTERPRETER_ITERATION_LIMIT
				&& u->eip >= cpu->instruction_pointer) {
				uop_flush(); /* cpu.c:134-136 */
				why = UOP_LEFT_RETURN;
				break;
			}
			if (u->flags & UOP_FLAG_BLOCK_END) {
				/* THE BLOCK ENDS, BUT THE REFERENCE BATCH MAY NOT: this is the one uop
				 * with UOP_FLAG_BLOCK_END and no UOP_FLAG_FLUSH (sti -- see the file
				 * header), so the count stays pending and the batch's next instruction
				 * is executed by the next call.  Returning here is the whole point. */
				uop_stats.block_ends++;
				why = UOP_LEFT_RETURN;
				break;
			}
			if (u->eip + u->len != cpu->instruction_pointer) {
				/* control flow left the block (jcc taken, jmp, call, ret ...): the
				 * reference's loop tail, then a fresh lookup at the new eip */
				why = UOP_LEFT_TAIL;
				break;
			}
			idx += 1;
			if (idx >= count) {
				/* the block ran out of uops -- the CAP end.  The reference has no boundary
				 * here, so the batch continues: the tail, then the next block. */
				why = UOP_LEFT_TAIL;
				break;
			}
		}

		uop_stats.uops_executed += block_uops;
		uop_cur_block = NULL;

		if (why == UOP_LEFT_RETURN) return;

		if (why == UOP_LEFT_ABORT) {
			/* plan ss4.2: the write is inside the page this block is executing from.  The
			 * hook has already unlinked the page's blocks (this one included), so the next
			 * lookup misses and re-decodes the CURRENT bytes -- but force it, because a
			 * lookup that hit the dead block would re-run stale uops forever. */
			cpu->previous_ip = cpu->instruction_pointer;
			phys = blk->phys_start +
			       (uint32_t)(cpu->instruction_pointer - blk->virt_start);
			continue;
		}

		/* UOP_LEFT_TAIL: the reference's loop tail (cpu.c:140-145).  On a fault the whole
		 * batch's count is discarded -- the reference's own quirk. */
		cpu->previous_ip = cpu->instruction_pointer;
		phys = get_phys_eip();
		if (cpu->pagefault) {
			uop_discard();
			return;
		}

		#undef UOP_LEFT_RETURN
		#undef UOP_LEFT_TAIL
		#undef UOP_LEFT_ABORT
	}
}

void uop_stats_print(void)
{
	uint64_t executed = uop_stats.uops_executed + uop_stats.fallback_steps;
	uint64_t total = executed + uop_stats.fallback_instr;
	uint64_t lookups = uop_stats.lookups;

	fprintf(stderr,
		"uop-cache: lookups %llu (hits %llu = %.2f%%, decodes %llu, decode-fails %llu)\n",
		(unsigned long long)lookups,
		(unsigned long long)uop_stats.hits,
		lookups ? 100.0 * (double)uop_stats.hits / (double)lookups : 0.0,
		(unsigned long long)uop_stats.decodes,
		(unsigned long long)uop_stats.decode_fails);
	fprintf(stderr,
		"uop-cache: instructions %llu (uops %llu, mid-batch fallback steps %llu, "
		"interpreted_run instructions %llu -> fallback %.3f%%)\n",
		(unsigned long long)total,
		(unsigned long long)uop_stats.uops_executed,
		(unsigned long long)uop_stats.fallback_steps,
		(unsigned long long)uop_stats.fallback_instr,
		total ? 100.0 * (double)(uop_stats.fallback_steps + uop_stats.fallback_instr) / (double)total : 0.0);
	fprintf(stderr,
		"uop-cache: flushes %llu, discards %llu, SMC aborts %llu, invalidated blocks %llu, "
		"cache clears %llu, block ends %llu\n",
		(unsigned long long)uop_stats.flushes,
		(unsigned long long)uop_stats.discards,
		(unsigned long long)uop_stats.aborts,
		(unsigned long long)uop_stats.invalidations,
		(unsigned long long)uop_stats.clears,
		(unsigned long long)uop_stats.block_ends);
	/* PERF-5/M3: the pools' high-water marks -- plan ss3's sizing question, which the clear
	 * count alone cannot answer (a clear happens only AFTER the pool has overflowed, so
	 * `clears == 0` is compatible with an arena that is 99% full). */
	fprintf(stderr,
		"uop-cache: arena peak %llu/%u uops, block peak %llu/%u\n",
		(unsigned long long)uop_stats.arena_peak, UOP_ARENA_UOPS,
		(unsigned long long)uop_stats.block_peak, UOP_MAX_BLOCKS);
}
