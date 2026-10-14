/*
 * x64enc.c -- the prototype encoder.  See x64enc.h for the scope and the reason it is small.
 *
 * THE REX RULE, IN ONE PLACE, because it is the part of an x86-64 encoder that is easy to get
 * subtly wrong: REX = 0x40 | W<<3 | R<<2 | X<<1 | B.  R and B are the HIGH BITS of, respectively,
 * the modrm.reg field and the modrm.rm field (or of the register in the opcode byte for B8+rd),
 * and X is the high bit of the SIB index.  A 32-bit encoding that touches r8..r15 needs a REX
 * byte EVEN WITH W = 0, and a REX byte is mandatory for the 64-bit forms.
 */

#include "x64enc.h"

#include <stdio.h>

void e_init(enc_t *e, uint8_t *buf, size_t cap)
{
	e->buf = buf;
	e->cap = cap;
	e->n = 0;
	e->err = 0;
}

static void put(enc_t *e, uint8_t b)
{
	if (e->n >= e->cap) {
		e->err = 1;
		return;
	}
	e->buf[e->n++] = b;
}

static void rex(enc_t *e, int w, int r, int x, int b)
{
	int rr = (r >> 3) & 1, xx = (x >> 3) & 1, bb = (b >> 3) & 1;
	if (w || rr || xx || bb) put(e, (uint8_t)(0x40 | (w << 3) | (rr << 2) | (xx << 1) | bb));
}

static void modrm(enc_t *e, int mod, int reg, int rm)
{
	put(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/* [base + disp], with the mod field chosen from the displacement's value */
static void mem_modrm(enc_t *e, int reg, int base, int32_t disp)
{
	if (disp == 0 && (base & 7) != 5) {
		modrm(e, 0, reg, base);            /* no displacement (base != rbp/r13) */
	}
	else if (disp >= -128 && disp <= 127) {
		modrm(e, 1, reg, base);
		put(e, (uint8_t)(disp & 0xFF));
	}
	else {
		modrm(e, 2, reg, base);
		for (int i = 0; i < 4; i++) put(e, (uint8_t)((disp >> (8 * i)) & 0xFF));
	}
}

/* [base + index*scale + disp]; scale is 1, 2, 4 or 8 */
static void sib_modrm(enc_t *e, int reg, int base, int index, int scale, int32_t disp)
{
	int ss = (scale == 8) ? 3 : (scale == 4) ? 2 : (scale == 2) ? 1 : 0;
	int nosib_base = (base == RSP || base == RBP || base == R12 || base == R13);
	if (disp == 0 && !nosib_base) {
		modrm(e, 0, reg, RSP);
		put(e, (uint8_t)((ss << 6) | ((index & 7) << 3) | (base & 7)));
	}
	else if (disp >= -128 && disp <= 127) {
		modrm(e, 1, reg, RSP);
		put(e, (uint8_t)((ss << 6) | ((index & 7) << 3) | (base & 7)));
		put(e, (uint8_t)(disp & 0xFF));
	}
	else {
		modrm(e, 2, reg, RSP);
		put(e, (uint8_t)((ss << 6) | ((index & 7) << 3) | (base & 7)));
		for (int i = 0; i < 4; i++) put(e, (uint8_t)((disp >> (8 * i)) & 0xFF));
	}
}

void e_mov_r32_r32(enc_t *e, int dst, int src)
{
	rex(e, 0, src, 0, dst);
	put(e, 0x89);
	modrm(e, 3, src, dst);
}

void e_mov_r32_mem(enc_t *e, int dst, int base, int32_t disp)
{
	rex(e, 0, dst, 0, base);
	put(e, 0x8B);
	mem_modrm(e, dst, base, disp);
}

void e_mov_r32_mem_sib(enc_t *e, int dst, int base, int index, int32_t disp)
{
	rex(e, 0, dst, index, base);
	put(e, 0x8B);
	sib_modrm(e, dst, base, index, 1, disp);
}

void e_mov_r64_mem_sib(enc_t *e, int dst, int base, int index, int scale, int32_t disp)
{
	rex(e, 1, dst, index, base);
	put(e, 0x8B);
	sib_modrm(e, dst, base, index, scale, disp);
}

void e_movzx_r32_m8(enc_t *e, int dst, int base, int index, int32_t disp)
{
	rex(e, 0, dst, index, base);
	put(e, 0x0F);
	put(e, 0xB6);
	if (index < 0) mem_modrm(e, dst, base, disp);
	else sib_modrm(e, dst, base, index, 1, disp);
}

void e_mov_r64_imm64(enc_t *e, int dst, uint64_t imm)
{
	rex(e, 1, 0, 0, dst);
	put(e, (uint8_t)(0xB8 + (dst & 7)));
	for (int i = 0; i < 8; i++) put(e, (uint8_t)((imm >> (8 * i)) & 0xFF));
}

void e_mov_r32_imm32(enc_t *e, int dst, uint32_t imm)
{
	rex(e, 0, 0, 0, dst);
	put(e, (uint8_t)(0xB8 + (dst & 7)));
	for (int i = 0; i < 4; i++) put(e, (uint8_t)((imm >> (8 * i)) & 0xFF));
}

/* the ALU group's register-register forms are one family: opcode, modrm(dst|src) */
static void alu_rr(enc_t *e, uint8_t op, int dst, int src)
{
	rex(e, 0, src, 0, dst);
	put(e, op);
	modrm(e, 3, src, dst);
}

void e_add_r32_r32(enc_t *e, int dst, int src) { alu_rr(e, 0x01, dst, src); }
void e_sub_r32_r32(enc_t *e, int dst, int src) { alu_rr(e, 0x29, dst, src); }
void e_and_r32_r32(enc_t *e, int dst, int src) { alu_rr(e, 0x21, dst, src); }
void e_xor_r32_r32(enc_t *e, int dst, int src) { alu_rr(e, 0x31, dst, src); }
void e_cmp_r32_r32(enc_t *e, int dst, int src) { alu_rr(e, 0x39, dst, src); }

void e_test_r64_r64(enc_t *e, int a, int b)
{
	rex(e, 1, b, 0, a);
	put(e, 0x85);
	modrm(e, 3, b, a);
}

/* the FF /digit group: 0 = inc, 1 = dec */
static void group_ff(enc_t *e, int digit, int r)
{
	rex(e, 0, 0, 0, r);
	put(e, 0xFF);
	modrm(e, 3, digit, r);
}

void e_dec_r32(enc_t *e, int r) { group_ff(e, 1, r); }
void e_inc_r32(enc_t *e, int r) { group_ff(e, 0, r); }

void e_shr_r32_imm8(enc_t *e, int r, int imm)
{
	rex(e, 0, 0, 0, r);
	put(e, 0xC1);
	modrm(e, 3, 5, r);
	put(e, (uint8_t)imm);
}

/* the 81 /digit group with a 32-bit immediate: 0 = add, 4 = and */
static void group_81(enc_t *e, int digit, int r, int32_t imm)
{
	rex(e, 0, 0, 0, r);
	put(e, 0x81);
	modrm(e, 3, digit, r);
	for (int i = 0; i < 4; i++) put(e, (uint8_t)((imm >> (8 * i)) & 0xFF));
}

void e_and_r32_imm32(enc_t *e, int r, int32_t imm) { group_81(e, 4, r, imm); }
void e_add_r32_imm32(enc_t *e, int r, int32_t imm) { group_81(e, 0, r, imm); }

void e_push_r64(enc_t *e, int r)
{
	if (r >= 8) put(e, 0x41);
	put(e, (uint8_t)(0x50 + (r & 7)));
}

void e_pop_r64(enc_t *e, int r)
{
	if (r >= 8) put(e, 0x41);
	put(e, (uint8_t)(0x58 + (r & 7)));
}

size_t e_jcc_rel8(enc_t *e, int cc)
{
	put(e, (uint8_t)(0x70 + (cc & 0xF)));
	size_t off = e->n;
	put(e, 0);
	return off;
}

size_t e_jmp_rel8(enc_t *e)
{
	put(e, 0xEB);
	size_t off = e->n;
	put(e, 0);
	return off;
}

void e_patch_rel8(enc_t *e, size_t off, size_t target)
{
	int rel = (int)target - (int)(off + 1);
	if (off >= e->n || rel < -128 || rel > 127) {
		e->err = 1;
		return;
	}
	e->buf[off] = (uint8_t)(rel & 0xFF);
}

void e_ret(enc_t *e) { put(e, 0xC3); }

void e_dump(const char *label, const uint8_t *buf, size_t n)
{
	printf("%s (%zu bytes):", label, n);
	for (size_t i = 0; i < n; i++) printf("%s%02x", (i % 16 == 0) ? "\n  " : " ", buf[i]);
	printf("\n");
	fflush(stdout);
}
