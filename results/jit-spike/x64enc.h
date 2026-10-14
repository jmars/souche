/*
 * x64enc.h -- JIT-SPIKE's PROTOTYPE of "our own x64 encoder" (Q2 option b).
 *
 * The question it exists to answer is not "can we write an encoder" (obviously yes) but WHAT ONE
 * COSTS: how many lines, how many bytes per instruction, and how the call site READS next to
 * LuaJIT's DynASM for the same block (results/jit-spike.md ss3).  It is therefore deliberately
 * NOT a general assembler: it is the instruction vocabulary the spike's block needs, with the
 * addressing forms a JIT's memory operands need, and nothing else.  The comparison's other half,
 * the same block through DynASM, is results/jit-spike/dynasm-shape.dasc.
 *
 * SCOPE (what it does NOT do, so the "bytes" comparison is honest): no 8/16-bit forms, no SSE,
 * no REX.W variants beyond `mov r64, imm64` and the 64-bit load the TLB fast path needs, no
 * relocation/label abstraction beyond an 8-bit patch (a real encoder needs rel32 for blocks
 * over 127 bytes -- the spike's blocks are shorter than that and the limitation is stated).
 */

#ifndef V86C_SPIKE_X64ENC_H
#define V86C_SPIKE_X64ENC_H

#include <stddef.h>
#include <stdint.h>

/* register numbers, both halves: RAX..R15 == 0..15 (the low 8 are EAX..EDI in 32-bit forms) */
enum { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

/* condition codes (the low nibble of a 0x7x jcc) */
enum { CC_O = 0, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP,
       CC_L, CC_GE, CC_LE, CC_G };

typedef struct enc {
	uint8_t *buf;
	size_t cap;
	size_t n;
	int err; /* sticky: the first overflow, so no emitter can write out of bounds silently */
} enc_t;

void e_init(enc_t *e, uint8_t *buf, size_t cap);

/* data movement */
void e_mov_r32_r32(enc_t *e, int dst, int src);                        /* 89 /r          */
void e_mov_r32_mem(enc_t *e, int dst, int base, int32_t disp);         /* 8B /r, [base+d] */
void e_mov_r64_mem_sib(enc_t *e, int dst, int base, int index, int scale, int32_t disp);
void e_mov_r32_mem_sib(enc_t *e, int dst, int base, int index, int32_t disp);
void e_movzx_r32_m8(enc_t *e, int dst, int base, int index, int32_t disp); /* 0F B6 /r  */
void e_mov_r64_imm64(enc_t *e, int dst, uint64_t imm);                 /* REX.W B8+rd    */
void e_mov_r32_imm32(enc_t *e, int dst, uint32_t imm);                 /* B8+rd          */

/* ALU, 32-bit, register-register (the flags are the HOST's: on x86-64 they are the guest's) */
void e_add_r32_r32(enc_t *e, int dst, int src); /* 01 /r */
void e_sub_r32_r32(enc_t *e, int dst, int src); /* 29 /r */
void e_and_r32_r32(enc_t *e, int dst, int src); /* 21 /r */
void e_xor_r32_r32(enc_t *e, int dst, int src); /* 31 /r */
void e_cmp_r32_r32(enc_t *e, int dst, int src); /* 39 /r */
void e_test_r64_r64(enc_t *e, int a, int b);    /* 85 /r */
void e_dec_r32(enc_t *e, int r);                /* FF /1 */
void e_inc_r32(enc_t *e, int r);                /* FF /0 */
void e_shr_r32_imm8(enc_t *e, int r, int imm);  /* C1 /5 */
void e_and_r32_imm32(enc_t *e, int r, int32_t imm); /* 81 /4 */
void e_add_r32_imm32(enc_t *e, int r, int32_t imm); /* 81 /0 */

/* the block prologue/epilogue a native JIT needs: save/restore a HOST callee-saved register it
 * uses to hold a guest register (SysV: RBX, RBP, R12-R15) */
void e_push_r64(enc_t *e, int r);               /* 50+rd */
void e_pop_r64(enc_t *e, int r);                /* 58+rd */

/* control */
size_t e_jcc_rel8(enc_t *e, int cc);            /* returns the offset of the rel8 byte */
size_t e_jmp_rel8(enc_t *e);                    /* EB cb */
void e_patch_rel8(enc_t *e, size_t off, size_t target); /* rel = target - (off + 1) */
void e_ret(enc_t *e);                           /* C3 */

/* diagnostics, so the prototype's output can be compared byte for byte with DynASM's */
void e_dump(const char *label, const uint8_t *buf, size_t n);

#endif
