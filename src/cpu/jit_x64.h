/*
 * jit_x64.h -- OUR OWN x86-64 ENCODER: the byte emitter the JIT's block compiler feeds.
 *
 * WHY OUR OWN, AND NOT DYNASM.  Measured by unit JIT-SPIKE (results/jit-spike.md ss3,
 * results/jit-spike/g2-feeder.txt): LuaJIT's DynASM and a hand-written encoder emit the SAME
 * 27-byte block, byte for byte, so the vendored route buys no output quality; it costs a Lua
 * interpreter as a build-time tool (minilua.c, 7,771 vendored lines + 125 KB of Lua/C, +4 KB of
 * .text in the APE) against 304 lines of our own C and no build-time tool.  The decisive
 * FUNCTIONAL reason is the same one the spike recorded: DynASM selects instructions at BUILD
 * time (one fixed action stream per `dasm_put` call site) with runtime operands, while a block
 * compiler decides at RUN time which instruction to emit for which uop.  This file is the
 * spike's prototype (results/jit-spike/x64enc.{c,h}) grown into the emitter the real compiler
 * needs: LABELS with forward references and rel32 patching (the spike had only rel8), the
 * widths and the memory forms the block prologue/epilogue and the checks need.
 *
 * WHAT IT IS NOT.  Not an assembler and not a general instruction set: it is the vocabulary
 * `src/cpu/jit_codegen.c` actually emits, plus nothing.  Every function here is called by the
 * compiler or pinned byte-for-byte by src/cpu/jit_codegen_selftest.c; there are no stubs
 * pretending to be mnemonics (the project's rule: a seat that is never called is
 * indistinguishable from an absent one).  The plan's ~30-mnemonic target is NOT fully
 * populated in M1 and that is stated rather than papered over: the forms M2's recipes need
 * (movzx, the 8/16-bit load/store forms, LEAs on a computed register, the shift-by-CL forms)
 * arrive WITH the recipes that emit them -- results/jit-1-m1.md lists what exists and what
 * does not.
 *
 * IT IS ARCHITECTURE-NEUTRAL CODE.  This file emits BYTES; it does not execute them and it
 * does not depend on the host being x86-64, so it compiles (and its byte pins run) on the
 * aarch64 leg too.  What is x86-64-only is the COMPILER that uses it
 * (src/cpu/jit_codegen.c, compiled out under V86C_JIT_X64 == 0, jit_mem.h), which is where
 * the aarch64 posture lives.
 *
 * HOW A CALLER USES IT.
 *
 *     jx64_t e;
 *     jx64_init(&e, scratch, sizeof scratch);
 *     int l_end = jx64_label(&e);
 *     jx64_bind(&e, l_end);
 *     ...
 *     jx64_jmp_label(&e, l_end);      // 32-bit displacement, patched by jx64_finish
 *     if (!jx64_finish(&e)) -> the block does not fit / a label was unbound: DO NOT PUBLISH
 *
 * Every emit function is a no-op once the buffer is full (`jx64_overflow`), so a caller
 * cannot read past the end of its buffer by forgetting a size check; the size check is
 * `jx64_finish` (or `jx64_overflow`/`jx64_len`).  `jx64_finish` resolves every recorded fixup
 * and returns false if a label was never bound or a displacement does not fit in 32 bits.
 */

#ifndef V86C_JIT_X64_H
#define V86C_JIT_X64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * registers.  RAX..R15 == 0..15; the low eight are EAX..EDI in a 32-bit form.
 * ------------------------------------------------------------------ */
enum {
	JX_RAX = 0, JX_RCX, JX_RDX, JX_RBX, JX_RSP, JX_RBP, JX_RSI, JX_RDI,
	JX_R8, JX_R9, JX_R10, JX_R11, JX_R12, JX_R13, JX_R14, JX_R15
};

/* the condition codes (the low nibble of a 0x7x jcc / 0F 8x) */
enum {
	JX_CC_O = 0, JX_CC_NO, JX_CC_B, JX_CC_AE, JX_CC_E, JX_CC_NE, JX_CC_BE, JX_CC_A,
	JX_CC_S, JX_CC_NS, JX_CC_P, JX_CC_NP, JX_CC_L, JX_CC_GE, JX_CC_LE, JX_CC_G
};

/* bounds.  The block compiler's label count is bounded by the per-uop failure continuations
 * (M2: 2 per recipe uop -- the slow trampoline and its tail -- UOP_MAX_BLOCK_UOPS == 64) plus
 * the block's own stubs and the shared per-block routines (M2's uop_exec fallback, the store
 * hook and the two flag getters). */
/* AUDIT-2 NICE-b: 16 fixed labels + up to 3 per uop (l_slow + l_tail + l_ok) means a
 * 59-uop all-mem-write block exhausted 192 labels while still fitting the 16 KiB byte
 * cap, and the refusal was then mis-counted as a FIT refusal.  256 puts label exhaustion
 * past the byte cap's own reach (~85 uops of the fattest recipe at ~200 B/uop exceeds
 * 16 KiB first), so refused_fit is honest again.  The struct grows ~512 B; nothing else
 * reads it. */
#define JX64_MAX_LABELS 256
#define JX64_MAX_FIXUPS 512

/* a label that has not been bound yet */
#define JX64_UNBOUND ((size_t)-1)

typedef struct jx64 {
	uint8_t *buf;
	size_t cap;
	size_t n;
	bool overflow;          /* sticky: the first write past `cap` */

	int labels;             /* labels allocated so far (jx64_label returns 0..labels-1) */
	size_t label_at[JX64_MAX_LABELS];

	int fixups;
	size_t fix_off[JX64_MAX_FIXUPS];   /* where the rel32 field starts */
	int fix_label[JX64_MAX_FIXUPS];
} jx64_t;

/* ------------------------------------------------------------------ *
 * the buffer, the labels and the patching
 * ------------------------------------------------------------------ */

void jx64_init(jx64_t *e, uint8_t *buf, size_t cap);

size_t jx64_len(const jx64_t *e);      /* bytes emitted (valid even on overflow) */
bool jx64_overflow(const jx64_t *e);   /* true if anything was dropped */

/* Allocate a label.  Returns its id, or -1 when JX64_MAX_LABELS is exhausted (the caller
 * must then abandon the block: emitting with a bad label id is refused, not undefined). */
int jx64_label(jx64_t *e);
void jx64_bind(jx64_t *e, int label);        /* bind a label at the CURRENT offset */
void jx64_jmp_label(jx64_t *e, int label);   /* E9 rel32 */
void jx64_call_label(jx64_t *e, int label);  /* E8 rel32 */
void jx64_jcc_label(jx64_t *e, int cc, int label); /* 0F 8x rel32 */

/* Resolve every fixup.  False means: the buffer overflowed, a label was never bound, a label
 * id was invalid, or a relative displacement does not fit in int32 -- in EVERY case the
 * caller must not publish or execute the buffer. */
bool jx64_finish(jx64_t *e);

/* ------------------------------------------------------------------ *
 * the vocabulary
 *
 * Naming is `<mnemonic>_<operation-size>...`: r32 = 32-bit register, m8 = a byte in memory,
 * r64 = a 64-bit register (64-bit forms carry REX.W).  A memory operand is always
 * `[base + disp]`, where `base` may be any register (JX_RSP and JX_R12 are handled: they
 * need the SIB byte, and the 3-byte disp32 form is chosen automatically when disp8 does not
 * fit).  An ABSOLUTE address is not a special form -- materialise it with mov_r64_imm64 into
 * a scratch register first, which is also what makes the emitted block position-independent
 * (the compiler knows the real address at compile time; nothing is relocated at load time).
 * ------------------------------------------------------------------ */

/* data movement */
void jx64_mov_r32_r32(jx64_t *e, int dst, int src);
void jx64_mov_r32_m32(jx64_t *e, int dst, int base, int32_t disp);
void jx64_mov_m32_r32(jx64_t *e, int base, int32_t disp, int src);
void jx64_mov_r32_imm32(jx64_t *e, int dst, uint32_t imm);
void jx64_mov_r64_imm64(jx64_t *e, int dst, uint64_t imm); /* MOVABS */
void jx64_mov_m8_imm8(jx64_t *e, int base, int32_t disp, uint8_t imm);
void jx64_mov_m32_imm32(jx64_t *e, int base, int32_t disp, uint32_t imm);

/* ALU, 32-bit.  `add_m32_r32`/`add_m64_r64` are the memory-destination forms (the stats
 * accounting writes straight into the counters); `sub_r32_m32` is `sub r32, [base+disp]`. */
void jx64_add_r32_r32(jx64_t *e, int dst, int src);
void jx64_add_r32_imm32(jx64_t *e, int dst, int32_t imm);
void jx64_add_m32_r32(jx64_t *e, int base, int32_t disp, int src);
void jx64_add_m64_r64(jx64_t *e, int base, int32_t disp, int src);
void jx64_add_m32_imm8(jx64_t *e, int base, int32_t disp, int32_t imm);
void jx64_sub_r32_r32(jx64_t *e, int dst, int src);
void jx64_sub_r32_m32(jx64_t *e, int dst, int base, int32_t disp);
void jx64_and_r32_r32(jx64_t *e, int dst, int src);
void jx64_or_r32_r32(jx64_t *e, int dst, int src);
void jx64_xor_r32_r32(jx64_t *e, int dst, int src);
void jx64_cmp_r32_r32(jx64_t *e, int a, int b);
void jx64_cmp_r32_imm32(jx64_t *e, int a, int32_t imm);
void jx64_cmp_m32_r32(jx64_t *e, int base, int32_t disp, int src);
void jx64_cmp_m8_imm8(jx64_t *e, int base, int32_t disp, uint8_t imm);
void jx64_cmp_m32_imm32(jx64_t *e, int base, int32_t disp, int32_t imm);
void jx64_test_r32_r32(jx64_t *e, int a, int b);
void jx64_test_r32_imm32(jx64_t *e, int a, uint32_t imm);
void jx64_test_r8_imm8(jx64_t *e, int r, uint8_t imm);
void jx64_xchg_r32_r32(jx64_t *e, int a, int b);

/* inc/dec and the shifts.  The shift forms take an 8-bit immediate count (C1 /digit ib);
 * the CL forms are NOT here (no recipe uses them yet -- see the header). */
void jx64_inc_r32(jx64_t *e, int r);
void jx64_dec_r32(jx64_t *e, int r);
void jx64_ror_r32_imm8(jx64_t *e, int r, uint8_t imm);
void jx64_shl_r32_imm8(jx64_t *e, int r, uint8_t imm);
void jx64_shr_r32_imm8(jx64_t *e, int r, uint8_t imm);
void jx64_sar_r32_imm8(jx64_t *e, int r, uint8_t imm);
void jx64_not_r32(jx64_t *e, int r);
void jx64_neg_r32(jx64_t *e, int r);

/* the stack (64-bit: the 32-bit forms would zero the upper half of RSP) and the block's own
 * frame adjustments */
void jx64_push_r64(jx64_t *e, int r);
void jx64_pop_r64(jx64_t *e, int r);
void jx64_sub_r64_imm8(jx64_t *e, int r, int32_t imm);
void jx64_add_r64_imm8(jx64_t *e, int r, int32_t imm);

/* control */
void jx64_call_r64(jx64_t *e, int r);
void jx64_jmp_r64(jx64_t *e, int r); /* JIT-6: FF /4 -- the chain link's indirect transfer */
void jx64_ret(jx64_t *e);

/* ------------------------------------------------------------------ *
 * M2: ONE OPERAND IN EVERY FORM THE RECIPES NEED
 *
 * The M1 vocabulary above is positional (`mov r32, [base+disp]`) because that is all the
 * prologue, the checks routine and one register-move recipe needed.  A recipe compiler needs
 * the same instruction for four operand shapes -- a register, `[base+disp]`, `[base+index*s+
 * disp]` and the absolute `[disp]` -- and enumerating each instruction x each shape would be
 * four times the vocabulary.  So the M2 emitters take this descriptor instead:
 *
 *     jx_op_reg(r)                      -> mod = 3 (a register operand)
 *     jx_op_mem(base, index, scale, d)  -> `[base + index*scale + disp]`
 *
 * with `index < 0` meaning "no index" and `base < 0` meaning "absolute" (`[disp32]`).  A base
 * of RSP/R12 gets the SIB byte it requires and RBP/R13 gets mod = 01 disp8 = 0 (mod = 00 means
 * RIP-relative); both rules live in ONE place (jit_x64.c's mem_operand_full).
 *
 * `scale` is 0..3 (the encoded shift), so `index*scale` is `index << scale` -- the same
 * convention uop_t's own `scale` field uses (a UOP_ADDR_SIB operand carries 0..3).
 * ------------------------------------------------------------------ */

typedef struct jx_operand {
	bool mem;
	int base;   /* register (mem == false), or the base; < 0 == no base */
	int index;  /* < 0 == no index */
	int scale;  /* 0..3 */
	int32_t disp;
} jx_operand_t;

static inline jx_operand_t jx_op_reg(int r)
{
	jx_operand_t o = { false, r, -1, 0, 0 };
	return o;
}

static inline jx_operand_t jx_op_mem(int base, int index, int scale, int32_t disp)
{
	jx_operand_t o = { true, base, index, scale, disp };
	return o;
}

/* data movement, both directions and every width the recipes use */
void jx64_mov_r32_rm(jx64_t *e, int dst, const jx_operand_t *src);
void jx64_mov_rm_r32(jx64_t *e, const jx_operand_t *dst, int src);
void jx64_mov_r16_rm16(jx64_t *e, int dst, const jx_operand_t *src);
void jx64_mov_rm16_r16(jx64_t *e, const jx_operand_t *dst, int src);
void jx64_mov_rm8_r8(jx64_t *e, const jx_operand_t *dst, int src);
void jx64_movzx_r32_rm8(jx64_t *e, int dst, const jx_operand_t *src);
void jx64_movzx_r32_rm16(jx64_t *e, int dst, const jx_operand_t *src);
void jx64_lea_r32_rm(jx64_t *e, int dst, const jx_operand_t *src);

/* `add[digit] r/m32, r32` -- digit 0/1/2/3/4/5/6/7 == add/or/adc/sbb/and/sub/xor/cmp */
void jx64_alu_rm_r32(jx64_t *e, int digit, const jx_operand_t *dst, int src);
/* group 1 with an immediate: 83 /digit ib when `imm8`, else 81 /digit id */
void jx64_alu_rm_imm(jx64_t *e, int digit, const jx_operand_t *dst, int32_t imm, bool imm8);
/* `80 /digit ib` -- the 8-bit immediate form (test r/m8, imm8 and friends) */
void jx64_alu_rm8_imm8(jx64_t *e, int digit, const jx_operand_t *dst, uint8_t imm);
void jx64_inc_rm32(jx64_t *e, const jx_operand_t *o);
void jx64_dec_rm32(jx64_t *e, const jx_operand_t *o);
void jx64_not_rm32(jx64_t *e, const jx_operand_t *o);
void jx64_test_rm32_r32(jx64_t *e, const jx_operand_t *o, int src);
void jx64_test_rm32_imm32(jx64_t *e, const jx_operand_t *o, uint32_t imm);
void jx64_test_rm8_imm8(jx64_t *e, const jx_operand_t *o, uint8_t imm);
/* the shift-by-CL forms (D3 /digit): 4 shl, 5 shr, 7 sar */
void jx64_shift_rm32_cl(jx64_t *e, int digit, const jx_operand_t *o);

/* byte-register moves, for the 8-bit recipes' register operands.  `force` REX is the rule
 * (see the file header): without it rm = 4..7 is AH..BH, which is not what a host register
 * holds.  These take HOST register numbers (the guest byte register is a byte OF one). */
void jx64_mov_r8_r8(jx64_t *e, int dst, int src);
void jx64_movzx_r32_r8(jx64_t *e, int dst, int src);

/* the flag plumbing the recipes emit instead of calling flags.c: `setcc` for a getter, and
 * `bt r/m32, r32` -- CF = bit `b` of `a`, which is how the lazy ZF/SF getters read their
 * `last_op_size` shift without needing CL. */
void jx64_setcc_r8(jx64_t *e, int cc, int r);
void jx64_bt_r32_r32(jx64_t *e, int a, int b);

/* JIT-6: the 64-bit register forms the chain stub needs (a full host pointer moves out of
 * RAX before guest EAX is reloaded, then is NULL-tested). */
void jx64_mov_r64_r64(jx64_t *e, int dst, int src);
void jx64_test_r64_r64(jx64_t *e, int a, int b);

/* `mov r64, [base+disp]` -- the block's frame slots (two of them hold pointers the recipes
 * load once per access; the 32-bit form would zero the upper half). */
void jx64_mov_r64_m64(jx64_t *e, int dst, int base, int32_t disp);
void jx64_mov_m64_r64(jx64_t *e, int base, int32_t disp, int src);

/* the reverse-direction ALU: `<op> r32, r/m32` -- digit 0/1/2/3/4/5/6/7 == add/or/adc/sbb/and/
 * sub/xor/cmp with the r/m field as the SOURCE.  `add r32, [base+disp]` (the segment base added
 * to a materialised address) and `cmp r32, [base+disp]` (the mapped-range test) are the two the
 * compiler emits. */
void jx64_alu_r32_rm(jx64_t *e, int digit, int dst, const jx_operand_t *src);

/* diagnostics: print a byte dump to stdout (the selftest's byte pins and the debug aid) */
void jx64_dump(const char *label, const uint8_t *buf, size_t n);
const char *jx64_last_error(const jx64_t *e);

#endif /* V86C_JIT_X64_H */
