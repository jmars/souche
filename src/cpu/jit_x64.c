/*
 * jit_x64.c -- the encoder.  See jit_x64.h for what it is, what it is not, and who calls it.
 *
 * THE REX RULE, IN ONE PLACE, because it is the part of an x86-64 encoder that is easy to get
 * subtly wrong: REX = 0x40 | W<<3 | R<<2 | X<<1 | B, where R and B are the HIGH BITS of the
 * modrm.reg field and the modrm.rm field (or of the register in the opcode byte for B8+rd) and
 * X is the high bit of a SIB index.  A 32-bit encoding that touches r8..r15 needs a REX byte
 * EVEN WITH W = 0, and a REX byte is MANDATORY for the 64-bit forms -- while for an 8-bit form
 * a REX byte is what selects SPL/BPL/SIL/DIL over AH/CH/DH/BH, so the 8-bit emitters here
 * always emit one.
 *
 * THE ONE ADDRESSING FORM.  Every memory operand this vocabulary emits is `[base + disp]`.
 * `base & 7 == 4` (RSP/R12) is the encoding's own special case: rm = 100 selects a SIB byte,
 * whose index field 100 means "no index" and whose base field 100 means RSP/R12 -- so it costs
 * exactly one extra byte and this file writes it.  `base & 7 == 5` (RBP/R13) cannot use mod = 00
 * (that encoding means RIP-relative), so a zero displacement is emitted as mod = 01 disp8 = 0.
 * Both cases are exercised: the block compiler's own frame slot IS `[rsp + disp]` and its
 * state accesses are `[rbp + disp]`.
 *
 * SIZES ARE THE POINT.  A block has to fit in one 4 KiB code page (jit_codegen.c's fit rule),
 * so the displacement form is chosen by value -- disp8 when it fits, disp32 otherwise -- and
 * nothing here emits a longer form "for safety".  jx64_dump and the selftest's byte pins are
 * what keep that honest.
 */

#include "jit_x64.h"

#include <stdio.h>

void jx64_init(jx64_t *e, uint8_t *buf, size_t cap)
{
	e->buf = buf;
	e->cap = cap;
	e->n = 0;
	e->overflow = false;
	e->labels = 0;
	e->fixups = 0;
	for (int i = 0; i < JX64_MAX_LABELS; i++) e->label_at[i] = JX64_UNBOUND;
}

size_t jx64_len(const jx64_t *e) { return e->n; }
bool jx64_overflow(const jx64_t *e) { return e->overflow; }

const char *jx64_last_error(const jx64_t *e)
{
	if (e->overflow) return "buffer full";
	for (int i = 0; i < e->fixups; i++) {
		int l = e->fix_label[i];
		if (l < 0 || l >= JX64_MAX_LABELS) return "invalid label id";
		if (e->label_at[l] == JX64_UNBOUND) return "unbound label";
	}
	for (int l = 0; l < e->labels; l++) {
		if (e->label_at[l] == JX64_UNBOUND) return "unbound label";
	}
	return "ok";
}

int jx64_label(jx64_t *e)
{
	if (e->labels >= JX64_MAX_LABELS) {
		e->overflow = true;
		return -1;
	}
	return e->labels++;
}

void jx64_bind(jx64_t *e, int label)
{
	if (label < 0 || label >= JX64_MAX_LABELS) {
		e->overflow = true;
		return;
	}
	e->label_at[label] = e->n;
}

/* ------------------------------------------------------------------ *
 * the raw emitters
 * ------------------------------------------------------------------ */

static void put(jx64_t *e, uint8_t b)
{
	if (e->n >= e->cap) {
		e->overflow = true;
		return;
	}
	e->buf[e->n++] = b;
}

static void put32(jx64_t *e, uint32_t v)
{
	for (int i = 0; i < 4; i++) put(e, (uint8_t)((v >> (8 * i)) & 0xFF));
}

static void put64(jx64_t *e, uint64_t v)
{
	for (int i = 0; i < 8; i++) put(e, (uint8_t)((v >> (8 * i)) & 0xFF));
}

/* REX = 0x40 | W<<3 | R<<2 | X<<1 | B.  `force` emits it even when every bit is zero (the
 * 8-bit forms need that: a REX-less rm = 4..7 is AH..BH, not SPL..DIL). */
static void rex(jx64_t *e, int w, int r, int x, int b, bool force)
{
	int rr = (r >> 3) & 1, xx = (x >> 3) & 1, bb = (b >> 3) & 1;
	if (w || rr || xx || bb || force) {
		put(e, (uint8_t)(0x40 | (w << 3) | (rr << 2) | (xx << 1) | bb));
	}
}

static void modrm(jx64_t *e, int mod, int reg, int rm)
{
	put(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/* `[base + index*scale + disp]` -- the SIB form, and the ONE place the addressing rules live.
 * `index < 0` means no index (SIB index field 100, which is only "none" when REX.X is 0 --
 * JX_RSP must never be passed as an index, and this compiler never does: the guest register map
 * sends ESP to JX_R12).  `base < 0` means the absolute `[disp32]` (SIB base field 101, mod 00),
 * which is the encoding RIP-relative would otherwise claim. */
static void mem_operand_full(jx64_t *e, int reg, int base, int index, int scale, int32_t disp)
{
	bool has_index = index >= 0;
	bool has_base = base >= 0;
	int sib_base = has_base ? (base & 7) : 5; /* 5 == no base (disp32) at mod == 00 */
	bool sib = has_index || sib_base == 4 || !has_base;
	int rm = sib ? 4 : sib_base; /* rm == 100 is what says "a SIB byte follows" */
	int mod;

	if (!has_base) mod = 0;
	else if (disp == 0 && sib_base != 5) mod = 0;
	else if (disp >= -128 && disp <= 127) mod = 1;
	else mod = 2;

	modrm(e, mod, reg, rm);
	if (sib) {
		int idx = has_index ? (index & 7) : 4; /* 4 == no index */
		int sc = has_index ? (scale & 3) : 0;
		put(e, (uint8_t)((sc << 6) | (idx << 3) | sib_base));
	}
	if (!has_base) put32(e, (uint32_t)disp);
	else if (mod == 1) put(e, (uint8_t)(disp & 0xFF));
	else if (mod == 2) put32(e, (uint32_t)disp);
}

/* `[base + disp]`, with the mod field chosen from the displacement's value (and from the
 * RBP/R13 rule mod == 00 cannot express). */
static void mem_operand(jx64_t *e, int reg, int base, int32_t disp)
{
	mem_operand_full(e, reg, base, -1, 0, disp);
}

/* the ALU family's register/memory-first form: `<op> r/m32, r32` */
static void alu_rm_r(jx64_t *e, uint8_t op, int reg, int base, int32_t disp, bool to_mem)
{
	if (to_mem) {
		rex(e, 0, reg, 0, base, false);
		put(e, op);
		mem_operand(e, reg, base, disp);
	}
	else {
		rex(e, 0, reg, 0, base, false);
		put(e, op);
		modrm(e, 3, reg, base);
	}
}

/* group 1 with a 32-bit immediate: 81 /digit id */
static void group1_imm32(jx64_t *e, int digit, int base, int32_t disp, bool to_mem, int32_t imm)
{
	rex(e, 0, 0, 0, base, false);
	put(e, 0x81);
	if (to_mem) mem_operand(e, digit, base, disp);
	else modrm(e, 3, digit, base);
	put32(e, (uint32_t)imm);
}

/* ------------------------------------------------------------------ *
 * data movement
 * ------------------------------------------------------------------ */

void jx64_mov_r32_r32(jx64_t *e, int dst, int src)
{
	rex(e, 0, src, 0, dst, false);
	put(e, 0x89);
	modrm(e, 3, src, dst);
}

/* JIT-6: the 64-bit forms the chain stub needs -- the lookup's return value (a full host
 * pointer) is moved out of RAX before the guest EAX is reloaded from its spill slot, and
 * tested for NULL.  Encodings mirror the 32-bit forms above with REX.W. */
void jx64_mov_r64_r64(jx64_t *e, int dst, int src)
{
	rex(e, 1, src, 0, dst, false);
	put(e, 0x89);
	modrm(e, 3, src, dst);
}

void jx64_test_r64_r64(jx64_t *e, int a, int b)
{
	rex(e, 1, b, 0, a, false);
	put(e, 0x85);
	modrm(e, 3, b, a);
}

void jx64_mov_r32_m32(jx64_t *e, int dst, int base, int32_t disp)
{
	rex(e, 0, dst, 0, base, false);
	put(e, 0x8B);
	mem_operand(e, dst, base, disp);
}

void jx64_mov_m32_r32(jx64_t *e, int base, int32_t disp, int src)
{
	rex(e, 0, src, 0, base, false);
	put(e, 0x89);
	mem_operand(e, src, base, disp);
}

void jx64_mov_r32_imm32(jx64_t *e, int dst, uint32_t imm)
{
	rex(e, 0, 0, 0, dst, false);
	put(e, (uint8_t)(0xB8 + (dst & 7)));
	put32(e, imm);
}

void jx64_mov_r64_imm64(jx64_t *e, int dst, uint64_t imm)
{
	rex(e, 1, 0, 0, dst, false);
	put(e, (uint8_t)(0xB8 + (dst & 7)));
	put64(e, imm);
}

void jx64_mov_m8_imm8(jx64_t *e, int base, int32_t disp, uint8_t imm)
{
	rex(e, 0, 0, 0, base, false);
	put(e, 0xC6);
	mem_operand(e, 0, base, disp);
	put(e, imm);
}

void jx64_mov_m32_imm32(jx64_t *e, int base, int32_t disp, uint32_t imm)
{
	rex(e, 0, 0, 0, base, false);
	put(e, 0xC7);
	mem_operand(e, 0, base, disp);
	put32(e, imm);
}

/* ------------------------------------------------------------------ *
 * ALU
 * ------------------------------------------------------------------ */

void jx64_add_r32_r32(jx64_t *e, int dst, int src) { alu_rm_r(e, 0x01, src, dst, 0, false); }
void jx64_or_r32_r32(jx64_t *e, int dst, int src) { alu_rm_r(e, 0x09, src, dst, 0, false); }
void jx64_and_r32_r32(jx64_t *e, int dst, int src) { alu_rm_r(e, 0x21, src, dst, 0, false); }
void jx64_sub_r32_r32(jx64_t *e, int dst, int src) { alu_rm_r(e, 0x29, src, dst, 0, false); }
void jx64_xor_r32_r32(jx64_t *e, int dst, int src) { alu_rm_r(e, 0x31, src, dst, 0, false); }
void jx64_cmp_r32_r32(jx64_t *e, int a, int b) { alu_rm_r(e, 0x39, b, a, 0, false); }

void jx64_add_m32_r32(jx64_t *e, int base, int32_t disp, int src)
{
	alu_rm_r(e, 0x01, src, base, disp, true);
}

void jx64_add_m64_r64(jx64_t *e, int base, int32_t disp, int src)
{
	rex(e, 1, src, 0, base, false);
	put(e, 0x01);
	mem_operand(e, src, base, disp);
}

/* `add dword [base+disp], imm8` (83 /0 ib) -- the per-uop counters live in the block's own
 * frame slots, and this is the form that increments one in four bytes. */
void jx64_add_m32_imm8(jx64_t *e, int base, int32_t disp, int32_t imm)
{
	rex(e, 0, 0, 0, base, false);
	put(e, 0x83);
	mem_operand(e, 0, base, disp);
	put(e, (uint8_t)(imm & 0xFF));
}

/* `cmp dword [base+disp], r32` (39 /r) -- the shared checks routine compares the executor's
 * instruction pointer against the uop's expected end, which arrives in a register. */
void jx64_cmp_m32_r32(jx64_t *e, int base, int32_t disp, int src)
{
	rex(e, 0, src, 0, base, false);
	put(e, 0x39);
	mem_operand(e, src, base, disp);
}

/* `sub r32, [base+disp]` -- the reverse-direction form (2B /r), used by the budget test */
void jx64_sub_r32_m32(jx64_t *e, int dst, int base, int32_t disp)
{
	rex(e, 0, dst, 0, base, false);
	put(e, 0x2B);
	mem_operand(e, dst, base, disp);
}

void jx64_add_r32_imm32(jx64_t *e, int dst, int32_t imm)
{
	group1_imm32(e, 0, dst, 0, false, imm);
}

void jx64_cmp_r32_imm32(jx64_t *e, int a, int32_t imm)
{
	group1_imm32(e, 7, a, 0, false, imm);
}

void jx64_cmp_m8_imm8(jx64_t *e, int base, int32_t disp, uint8_t imm)
{
	rex(e, 0, 0, 0, base, false);
	put(e, 0x80);
	mem_operand(e, 7, base, disp);
	put(e, imm);
}

void jx64_cmp_m32_imm32(jx64_t *e, int base, int32_t disp, int32_t imm)
{
	group1_imm32(e, 7, base, disp, true, imm);
}

void jx64_test_r32_r32(jx64_t *e, int a, int b)
{
	rex(e, 0, b, 0, a, false);
	put(e, 0x85);
	modrm(e, 3, b, a);
}

void jx64_test_r32_imm32(jx64_t *e, int a, uint32_t imm)
{
	rex(e, 0, 0, 0, a, false);
	put(e, 0xF7);
	modrm(e, 3, 0, a);
	put32(e, imm);
}

void jx64_test_r8_imm8(jx64_t *e, int r, uint8_t imm)
{
	rex(e, 0, 0, 0, r, true); /* forced: without a REX byte rm = 4..7 is AH..BH */
	put(e, 0xF6);
	modrm(e, 3, 0, r);
	put(e, imm);
}

void jx64_xchg_r32_r32(jx64_t *e, int a, int b)
{
	rex(e, 0, b, 0, a, false);
	put(e, 0x87);
	modrm(e, 3, b, a);
}

/* ------------------------------------------------------------------ *
 * inc/dec, the shifts, not/neg -- the FF / F7 / C1 groups
 * ------------------------------------------------------------------ */

static void group_ff(jx64_t *e, int digit, int r)
{
	rex(e, 0, 0, 0, r, false);
	put(e, 0xFF);
	modrm(e, 3, digit, r);
}

static void group_f7(jx64_t *e, int digit, int r)
{
	rex(e, 0, 0, 0, r, false);
	put(e, 0xF7);
	modrm(e, 3, digit, r);
}

static void shift_imm8(jx64_t *e, int digit, int r, uint8_t imm)
{
	rex(e, 0, 0, 0, r, false);
	put(e, 0xC1);
	modrm(e, 3, digit, r);
	put(e, imm);
}

void jx64_inc_r32(jx64_t *e, int r) { group_ff(e, 0, r); }
void jx64_dec_r32(jx64_t *e, int r) { group_ff(e, 1, r); }
void jx64_not_r32(jx64_t *e, int r) { group_f7(e, 2, r); }
void jx64_neg_r32(jx64_t *e, int r) { group_f7(e, 3, r); }
void jx64_ror_r32_imm8(jx64_t *e, int r, uint8_t imm) { shift_imm8(e, 1, r, imm); }
void jx64_shl_r32_imm8(jx64_t *e, int r, uint8_t imm) { shift_imm8(e, 4, r, imm); }
void jx64_shr_r32_imm8(jx64_t *e, int r, uint8_t imm) { shift_imm8(e, 5, r, imm); }
void jx64_sar_r32_imm8(jx64_t *e, int r, uint8_t imm) { shift_imm8(e, 7, r, imm); }

/* ------------------------------------------------------------------ *
 * the stack, the frame, control
 * ------------------------------------------------------------------ */

void jx64_push_r64(jx64_t *e, int r)
{
	if (r >= 8) put(e, 0x41);
	put(e, (uint8_t)(0x50 + (r & 7)));
}

void jx64_pop_r64(jx64_t *e, int r)
{
	if (r >= 8) put(e, 0x41);
	put(e, (uint8_t)(0x58 + (r & 7)));
}

/* `sub r64, imm8` / `add r64, imm8` (83 /5 ib, 83 /0 ib with REX.W).  REX.W IS NOT OPTIONAL
 * here: the 32-bit form would zero the upper 32 bits of RSP. */
void jx64_sub_r64_imm8(jx64_t *e, int r, int32_t imm)
{
	rex(e, 1, 0, 0, r, false);
	put(e, 0x83);
	modrm(e, 3, 5, r);
	put(e, (uint8_t)(imm & 0xFF));
}

void jx64_add_r64_imm8(jx64_t *e, int r, int32_t imm)
{
	rex(e, 1, 0, 0, r, false);
	put(e, 0x83);
	modrm(e, 3, 0, r);
	put(e, (uint8_t)(imm & 0xFF));
}

void jx64_call_r64(jx64_t *e, int r)
{
	rex(e, 0, 0, 0, r, false);
	put(e, 0xFF);
	modrm(e, 3, 2, r);
}

/* JIT-6: an INDIRECT JMP (FF /4) -- the chain link's transfer into the target block's chain
 * entry.  The target address is a runtime value (the substrate's lookup returns it), so no
 * rel32 form can express it.  Same encoding shape as the call above, /4 instead of /2. */
void jx64_jmp_r64(jx64_t *e, int r)
{
	rex(e, 0, 0, 0, r, false);
	put(e, 0xFF);
	modrm(e, 3, 4, r);
}

void jx64_ret(jx64_t *e) { put(e, 0xC3); }

/* the operand descriptor's r/m field and its REX byte.  REX must be emitted BEFORE the opcode
 * and the modrm AFTER it, so the two halves are separate -- and the bits they need (X from the
 * index, B from the base or from a mod = 3 register) live in one place. */
static void rex_for_rm(jx64_t *e, int w, int reg, const jx_operand_t *o, bool force)
{
	rex(e, w, reg, o->index < 0 ? 0 : o->index, o->base < 0 ? 0 : o->base, force);
}

static void emit_rm(jx64_t *e, int reg, const jx_operand_t *o)
{
	if (!o->mem) {
		modrm(e, 3, reg, o->base);
		return;
	}
	mem_operand_full(e, reg, o->base, o->index, o->scale, o->disp);
}

void jx64_mov_r32_rm(jx64_t *e, int dst, const jx_operand_t *src)
{
	rex_for_rm(e, 0, dst, src, false);
	put(e, 0x8B);
	emit_rm(e, dst, src);
}

void jx64_mov_rm_r32(jx64_t *e, const jx_operand_t *dst, int src)
{
	rex_for_rm(e, 0, src, dst, false);
	put(e, 0x89);
	emit_rm(e, src, dst);
}

void jx64_mov_r16_rm16(jx64_t *e, int dst, const jx_operand_t *src)
{
	put(e, 0x66);
	rex_for_rm(e, 0, dst, src, false);
	put(e, 0x8B);
	emit_rm(e, dst, src);
}

void jx64_mov_rm16_r16(jx64_t *e, const jx_operand_t *dst, int src)
{
	put(e, 0x66);
	rex_for_rm(e, 0, src, dst, false);
	put(e, 0x89);
	emit_rm(e, src, dst);
}

void jx64_mov_rm8_r8(jx64_t *e, const jx_operand_t *dst, int src)
{
	rex_for_rm(e, 0, src, dst, true);
	put(e, 0x88);
	emit_rm(e, src, dst);
}

void jx64_movzx_r32_rm8(jx64_t *e, int dst, const jx_operand_t *src)
{
	rex_for_rm(e, 0, dst, src, false);
	put(e, 0x0F);
	put(e, 0xB6);
	emit_rm(e, dst, src);
}

void jx64_movzx_r32_rm16(jx64_t *e, int dst, const jx_operand_t *src)
{
	rex_for_rm(e, 0, dst, src, false);
	put(e, 0x0F);
	put(e, 0xB7);
	emit_rm(e, dst, src);
}

void jx64_lea_r32_rm(jx64_t *e, int dst, const jx_operand_t *src)
{
	rex_for_rm(e, 0, dst, src, false);
	put(e, 0x8D);
	emit_rm(e, dst, src);
}

void jx64_alu_rm_r32(jx64_t *e, int digit, const jx_operand_t *dst, int src)
{
	rex_for_rm(e, 0, src, dst, false);
	put(e, (uint8_t)((digit << 3) | 0x01));
	emit_rm(e, src, dst);
}

void jx64_alu_rm_imm(jx64_t *e, int digit, const jx_operand_t *dst, int32_t imm, bool imm8)
{
	if (imm8) {
		rex_for_rm(e, 0, 0, dst, false);
		put(e, 0x83);
		emit_rm(e, digit, dst);
		put(e, (uint8_t)(imm & 0xFF));
	}
	else {
		rex_for_rm(e, 0, 0, dst, false);
		put(e, 0x81);
		emit_rm(e, digit, dst);
		put32(e, (uint32_t)imm);
	}
}

void jx64_alu_rm8_imm8(jx64_t *e, int digit, const jx_operand_t *dst, uint8_t imm)
{
	rex_for_rm(e, 0, 0, dst, !dst->mem); /* force: an 8-bit REGISTER operand needs it */
	put(e, 0x80);
	emit_rm(e, digit, dst);
	put(e, imm);
}

void jx64_test_rm8_imm8(jx64_t *e, const jx_operand_t *o, uint8_t imm)
{
	rex_for_rm(e, 0, 0, o, !o->mem);
	put(e, 0xF6);
	emit_rm(e, 0, o);
	put(e, imm);
}

void jx64_inc_rm32(jx64_t *e, const jx_operand_t *o)
{
	rex_for_rm(e, 0, 0, o, false);
	put(e, 0xFF);
	emit_rm(e, 0, o);
}

void jx64_dec_rm32(jx64_t *e, const jx_operand_t *o)
{
	rex_for_rm(e, 0, 0, o, false);
	put(e, 0xFF);
	emit_rm(e, 1, o);
}

void jx64_not_rm32(jx64_t *e, const jx_operand_t *o)
{
	rex_for_rm(e, 0, 0, o, false);
	put(e, 0xF7);
	emit_rm(e, 2, o);
}

void jx64_test_rm32_r32(jx64_t *e, const jx_operand_t *o, int src)
{
	rex_for_rm(e, 0, src, o, false);
	put(e, 0x85);
	emit_rm(e, src, o);
}

void jx64_test_rm32_imm32(jx64_t *e, const jx_operand_t *o, uint32_t imm)
{
	rex_for_rm(e, 0, 0, o, false);
	put(e, 0xF7);
	emit_rm(e, 0, o);
	put32(e, imm);
}

void jx64_shift_rm32_cl(jx64_t *e, int digit, const jx_operand_t *o)
{
	rex_for_rm(e, 0, 0, o, false);
	put(e, 0xD3);
	emit_rm(e, digit, o);
}

void jx64_mov_r8_r8(jx64_t *e, int dst, int src)
{
	int r = src, b = dst;
	rex(e, 0, r, 0, b, true);
	put(e, 0x88);
	modrm(e, 3, r, b);
}

void jx64_movzx_r32_r8(jx64_t *e, int dst, int src)
{
	rex(e, 0, dst, 0, src, true);
	put(e, 0x0F);
	put(e, 0xB6);
	modrm(e, 3, dst, src);
}

void jx64_setcc_r8(jx64_t *e, int cc, int r)
{
	rex(e, 0, 0, 0, r, true);
	put(e, 0x0F);
	put(e, (uint8_t)(0x90 + (cc & 0xF)));
	modrm(e, 3, 0, r);
}

void jx64_bt_r32_r32(jx64_t *e, int a, int b)
{
	rex(e, 0, b, 0, a, false);
	put(e, 0x0F);
	put(e, 0xA3);
	modrm(e, 3, b, a);
}

void jx64_mov_r64_m64(jx64_t *e, int dst, int base, int32_t disp)
{
	jx_operand_t o = jx_op_mem(base, -1, 0, disp);
	rex_for_rm(e, 1, dst, &o, false);
	put(e, 0x8B);
	emit_rm(e, dst, &o);
}

void jx64_mov_m64_r64(jx64_t *e, int base, int32_t disp, int src)
{
	jx_operand_t o = jx_op_mem(base, -1, 0, disp);
	rex_for_rm(e, 1, src, &o, false);
	put(e, 0x89);
	emit_rm(e, src, &o);
}

void jx64_alu_r32_rm(jx64_t *e, int digit, int dst, const jx_operand_t *src)
{
	rex_for_rm(e, 0, dst, src, false);
	put(e, (uint8_t)((digit << 3) | 0x03));
	emit_rm(e, dst, src);
}

/* ------------------------------------------------------------------ *
 * labels and fixups
 * ------------------------------------------------------------------ */

static void add_fixup(jx64_t *e, size_t off, int label)
{
	if (e->fixups >= JX64_MAX_FIXUPS || label < 0 || label >= JX64_MAX_LABELS) {
		e->overflow = true;
		return;
	}
	e->fix_off[e->fixups] = off;
	e->fix_label[e->fixups] = label;
	e->fixups++;
}

void jx64_jmp_label(jx64_t *e, int label)
{
	put(e, 0xE9);
	size_t off = e->n;
	put32(e, 0);
	add_fixup(e, off, label);
}

void jx64_call_label(jx64_t *e, int label)
{
	put(e, 0xE8);
	size_t off = e->n;
	put32(e, 0);
	add_fixup(e, off, label);
}

void jx64_jcc_label(jx64_t *e, int cc, int label)
{
	put(e, 0x0F);
	put(e, (uint8_t)(0x80 + (cc & 0xF)));
	size_t off = e->n;
	put32(e, 0);
	add_fixup(e, off, label);
}

bool jx64_finish(jx64_t *e)
{
	if (e->overflow) return false;
	for (int i = 0; i < e->fixups; i++) {
		size_t off = e->fix_off[i];
		int l = e->fix_label[i];
		if (l < 0 || l >= JX64_MAX_LABELS) return false;
		size_t target = e->label_at[l];
		if (target == JX64_UNBOUND) return false;
		int64_t rel = (int64_t)target - (int64_t)(off + 4);
		if (rel < -2147483648LL || rel > 2147483647LL) return false;
		uint32_t v = (uint32_t)(int32_t)rel;
		for (int k = 0; k < 4; k++) e->buf[off + (size_t)k] = (uint8_t)((v >> (8 * k)) & 0xFF);
	}
	return true;
}

void jx64_dump(const char *label, const uint8_t *buf, size_t n)
{
	printf("%s (%zu bytes):", label, n);
	for (size_t i = 0; i < n; i++) printf("%s%02x", (i % 16 == 0) ? "\n  " : " ", buf[i]);
	printf("\n");
	fflush(stdout);
}
