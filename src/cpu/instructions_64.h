/*
 * instructions_64.h -- the 64-bit instruction bodies (slice S5b), and the REX prefix.
 *
 * The dispatch that calls these is GENERATED (src/cpu/interpreter_gen.c, emitted by
 * gen/interpreter_c.js from v86/gen/x86_table.js + the authored gen/x86_table64.js), exactly
 * as the 32-bit bodies in instructions.h are called.  This header is the declaration half of
 * that contract: every symbol here is named by the emitted `0x2NN` arms, verbatim, and the
 * emitter's availability scan reads these DEFINITIONS out of src/cpu/instructions_64.c -- a
 * name that is declared but not defined therefore shows up as a NAMED STOP in the generated
 * switch rather than as a link error, which is the port's incremental-build rule.
 *
 * NAMING: `instr64_<opcode>` / `instr64_<opcode>_<fixed_g>` / `instr64_<opcode>_mem|_reg`,
 * with the same argument shapes as the `instr16_`/`instr32_` families beside them (the
 * emitter threads the arguments, the body only implements the semantics).
 *
 * ARGUMENT WIDTHS, the one place the 64-bit family differs: an address is `int64_t` (a
 * 64-bit linear address -- plan correction W1) and an immediate is `int64_t` (B8+r with REX.W
 * is the 8-byte immediate, and C7 /0 with REX.W is a sign-extended imm32 that lands in the
 * same slot).  A body that took an int32_t address would truncate silently, so the emitter
 * declares the local as int64_t for these arms and the compiler enforces the rest.
 */

#ifndef V86C_INSTRUCTIONS_64_H
#define V86C_INSTRUCTIONS_64_H

#include <stdbool.h>
#include <stdint.h>

/* THE REX PREFIX (0x40-0x4F in 64-bit mode).  Called from the generated switch's 0x2NN arm
 * for those sixteen bytes; it records the nibble in cpu->prefixes and re-dispatches the rest
 * of the instruction (modrm.h's PREFIX_REX_*). */
void long_mode_rex_prefix(int32_t byte);

/* the bodies.  Grouped by the family each one belongs to, so the file's shape is the slice's
 * own list of what it landed. */
void instr64_88_mem(int64_t addr, int32_t r);
void instr64_88_reg(int32_t r1, int32_t r);
void instr64_89_mem(int64_t addr, int32_t r);
void instr64_89_reg(int32_t r1, int32_t r);
void instr64_8A_mem(int64_t addr, int32_t r);
void instr64_8A_reg(int32_t r1, int32_t r);
void instr64_8B_mem(int64_t addr, int32_t r);
void instr64_8B_reg(int32_t r1, int32_t r);

void instr64_A0(int64_t addr);
void instr64_A1(int64_t addr);
void instr64_A2(int64_t addr);
void instr64_A3(int64_t addr);

void instr64_B0(int64_t imm);
void instr64_B1(int64_t imm);
void instr64_B2(int64_t imm);
void instr64_B3(int64_t imm);
void instr64_B4(int64_t imm);
void instr64_B5(int64_t imm);
void instr64_B6(int64_t imm);
void instr64_B7(int64_t imm);
void instr64_B8(int64_t imm);
void instr64_B9(int64_t imm);
void instr64_BA(int64_t imm);
void instr64_BB(int64_t imm);
void instr64_BC(int64_t imm);
void instr64_BD(int64_t imm);
void instr64_BE(int64_t imm);
void instr64_BF(int64_t imm);

void instr64_C6_0_mem(int64_t addr, int64_t imm);
void instr64_C6_0_reg(int32_t r1, int64_t imm);
void instr64_C7_0_mem(int64_t addr, int64_t imm);
void instr64_C7_0_reg(int32_t r1, int64_t imm);

void instr64_01_mem(int64_t addr, int32_t r);
void instr64_01_reg(int32_t r1, int32_t r);
void instr64_03_mem(int64_t addr, int32_t r);
void instr64_03_reg(int32_t r1, int32_t r);
void instr64_31_mem(int64_t addr, int32_t r);
void instr64_31_reg(int32_t r1, int32_t r);
void instr64_33_mem(int64_t addr, int32_t r);
void instr64_33_reg(int32_t r1, int32_t r);

void instr64_50(void);
void instr64_51(void);
void instr64_52(void);
void instr64_53(void);
void instr64_54(void);
void instr64_55(void);
void instr64_56(void);
void instr64_57(void);
void instr64_58(void);
void instr64_59(void);
void instr64_5A(void);
void instr64_5B(void);
void instr64_5C(void);
void instr64_5D(void);
void instr64_5E(void);
void instr64_5F(void);
void instr64_68(int64_t imm);
void instr64_6A(int64_t imm);
void instr64_9C(void);
void instr64_9D(void);

/* S6: the ALU family at 64 bits (arith.h carries the flag-core argument) */
void instr64_01_mem(int64_t addr, int32_t r);
void instr64_01_reg(int32_t r1, int32_t r);
void instr64_03_mem(int64_t addr, int32_t r);
void instr64_03_reg(int32_t r1, int32_t r);
void instr64_09_mem(int64_t addr, int32_t r);
void instr64_09_reg(int32_t r1, int32_t r);
void instr64_0B_mem(int64_t addr, int32_t r);
void instr64_0B_reg(int32_t r1, int32_t r);
void instr64_11_mem(int64_t addr, int32_t r);
void instr64_11_reg(int32_t r1, int32_t r);
void instr64_13_mem(int64_t addr, int32_t r);
void instr64_13_reg(int32_t r1, int32_t r);
void instr64_19_mem(int64_t addr, int32_t r);
void instr64_19_reg(int32_t r1, int32_t r);
void instr64_1B_mem(int64_t addr, int32_t r);
void instr64_1B_reg(int32_t r1, int32_t r);
void instr64_21_mem(int64_t addr, int32_t r);
void instr64_21_reg(int32_t r1, int32_t r);
void instr64_23_mem(int64_t addr, int32_t r);
void instr64_23_reg(int32_t r1, int32_t r);
void instr64_29_mem(int64_t addr, int32_t r);
void instr64_29_reg(int32_t r1, int32_t r);
void instr64_2B_mem(int64_t addr, int32_t r);
void instr64_2B_reg(int32_t r1, int32_t r);
void instr64_31_mem(int64_t addr, int32_t r);
void instr64_31_reg(int32_t r1, int32_t r);
void instr64_33_mem(int64_t addr, int32_t r);
void instr64_33_reg(int32_t r1, int32_t r);
void instr64_39_mem(int64_t addr, int32_t r);
void instr64_39_reg(int32_t r1, int32_t r);
void instr64_3B_mem(int64_t addr, int32_t r);
void instr64_3B_reg(int32_t r1, int32_t r);
void instr64_85_mem(int64_t addr, int32_t r);
void instr64_85_reg(int32_t r1, int32_t r);
void instr64_81_0_mem(int64_t addr, int64_t imm);
void instr64_81_0_reg(int32_t r1, int64_t imm);
void instr64_81_1_mem(int64_t addr, int64_t imm);
void instr64_81_1_reg(int32_t r1, int64_t imm);
void instr64_81_2_mem(int64_t addr, int64_t imm);
void instr64_81_2_reg(int32_t r1, int64_t imm);
void instr64_81_3_mem(int64_t addr, int64_t imm);
void instr64_81_3_reg(int32_t r1, int64_t imm);
void instr64_81_4_mem(int64_t addr, int64_t imm);
void instr64_81_4_reg(int32_t r1, int64_t imm);
void instr64_81_5_mem(int64_t addr, int64_t imm);
void instr64_81_5_reg(int32_t r1, int64_t imm);
void instr64_81_6_mem(int64_t addr, int64_t imm);
void instr64_81_6_reg(int32_t r1, int64_t imm);
void instr64_81_7_mem(int64_t addr, int64_t imm);
void instr64_81_7_reg(int32_t r1, int64_t imm);
void instr64_83_0_mem(int64_t addr, int64_t imm);
void instr64_83_0_reg(int32_t r1, int64_t imm);
void instr64_83_1_mem(int64_t addr, int64_t imm);
void instr64_83_1_reg(int32_t r1, int64_t imm);
void instr64_83_2_mem(int64_t addr, int64_t imm);
void instr64_83_2_reg(int32_t r1, int64_t imm);
void instr64_83_3_mem(int64_t addr, int64_t imm);
void instr64_83_3_reg(int32_t r1, int64_t imm);
void instr64_83_4_mem(int64_t addr, int64_t imm);
void instr64_83_4_reg(int32_t r1, int64_t imm);
void instr64_83_5_mem(int64_t addr, int64_t imm);
void instr64_83_5_reg(int32_t r1, int64_t imm);
void instr64_83_6_mem(int64_t addr, int64_t imm);
void instr64_83_6_reg(int32_t r1, int64_t imm);
void instr64_83_7_mem(int64_t addr, int64_t imm);
void instr64_83_7_reg(int32_t r1, int64_t imm);
void instr64_8F_0_mem(int64_t modrm_byte);
void instr64_8F_0_reg(int32_t r1);
void instr64_F7_2_mem(int64_t addr);
void instr64_F7_2_reg(int32_t r1);
void instr64_F7_3_mem(int64_t addr);
void instr64_F7_3_reg(int32_t r1);
void instr64_FF_0_mem(int64_t addr);
void instr64_FF_0_reg(int32_t r1);
void instr64_FF_1_mem(int64_t addr);
void instr64_FF_1_reg(int32_t r1);

/* S7: control flow and stack at 64 bits */
void instr64_0F(void);
void instr64_70(int32_t imm);
void instr64_71(int32_t imm);
void instr64_72(int32_t imm);
void instr64_73(int32_t imm);
void instr64_74(int32_t imm);
void instr64_75(int32_t imm);
void instr64_76(int32_t imm);
void instr64_77(int32_t imm);
void instr64_78(int32_t imm);
void instr64_79(int32_t imm);
void instr64_7A(int32_t imm);
void instr64_7B(int32_t imm);
void instr64_7C(int32_t imm);
void instr64_7D(int32_t imm);
void instr64_7E(int32_t imm);
void instr64_7F(int32_t imm);
void instr64_8D_mem(int64_t modrm_byte, int32_t r);
void instr64_8D_reg(int32_t _r, int32_t _r2);
void instr64_63_mem(int64_t addr, int32_t r);
void instr64_63_reg(int32_t r1, int32_t r);
void instr64_87_mem(int64_t addr, int32_t r);
void instr64_87_reg(int32_t r1, int32_t r);
void instr64_C3(void);
void instr64_CB(void);
void instr64_05(int64_t imm);
void instr64_0D(int64_t imm);
void instr64_15(int64_t imm);
void instr64_1D(int64_t imm);
void instr64_25(int64_t imm);
void instr64_2D(int64_t imm);
void instr64_35(int64_t imm);
void instr64_3D(int64_t imm);
void instr64_E8(int32_t imm);
void instr64_E9(int32_t imm);
void instr64_FF_6_mem(int64_t addr);
void instr64_FF_6_reg(int32_t r1);
void instr64_FF_2_mem(int64_t addr);
void instr64_FF_2_reg(int32_t r1);
void instr64_FF_4_mem(int64_t addr);
void instr64_FF_4_reg(int32_t r1);
/* S7: the 0F-qualified arms (the escape's second byte) */
void instr64_0F40_mem(int64_t addr, int32_t r);
void instr64_0F40_reg(int32_t r1, int32_t r);
void instr64_0F41_mem(int64_t addr, int32_t r);
void instr64_0F41_reg(int32_t r1, int32_t r);
void instr64_0F42_mem(int64_t addr, int32_t r);
void instr64_0F42_reg(int32_t r1, int32_t r);
void instr64_0F43_mem(int64_t addr, int32_t r);
void instr64_0F43_reg(int32_t r1, int32_t r);
void instr64_0F46_mem(int64_t addr, int32_t r);
void instr64_0F46_reg(int32_t r1, int32_t r);
void instr64_0F47_mem(int64_t addr, int32_t r);
void instr64_0F47_reg(int32_t r1, int32_t r);
void instr64_0F48_mem(int64_t addr, int32_t r);
void instr64_0F48_reg(int32_t r1, int32_t r);
void instr64_0F49_mem(int64_t addr, int32_t r);
void instr64_0F49_reg(int32_t r1, int32_t r);
void instr64_0F4A_mem(int64_t addr, int32_t r);
void instr64_0F4A_reg(int32_t r1, int32_t r);
void instr64_0F4B_mem(int64_t addr, int32_t r);
void instr64_0F4B_reg(int32_t r1, int32_t r);
void instr64_0F4C_mem(int64_t addr, int32_t r);
void instr64_0F4C_reg(int32_t r1, int32_t r);
void instr64_0F4D_mem(int64_t addr, int32_t r);
void instr64_0F4D_reg(int32_t r1, int32_t r);
void instr64_0F4E_mem(int64_t addr, int32_t r);
void instr64_0F4E_reg(int32_t r1, int32_t r);
void instr64_0F4F_mem(int64_t addr, int32_t r);
void instr64_0F4F_reg(int32_t r1, int32_t r);
void instr64_0F44_mem(int64_t addr, int32_t r);
void instr64_0F44_reg(int32_t r1, int32_t r);
void instr64_0F45_mem(int64_t addr, int32_t r);
void instr64_0F45_reg(int32_t r1, int32_t r);
/* setcc has NO reg operand, but v86's table marks the encoding `e` without fixed_g, so the
 * emitter threads the reg field anyway and the body ignores it -- the 32-bit body's exact
 * shape (instructions_0f.c's instr_0F94_mem(addr, _r2)). */
void instr64_0F94_mem(int64_t addr, int32_t _r2);
void instr64_0F94_reg(int32_t r1, int32_t _r2);
void instr64_0F95_mem(int64_t addr, int32_t _r2);
void instr64_0F95_reg(int32_t r1, int32_t _r2);
void instr64_0FB6_mem(int64_t addr, int32_t r);
void instr64_0FB6_reg(int32_t r1, int32_t r);
void instr64_0FB7_mem(int64_t addr, int32_t r);
void instr64_0FB7_reg(int32_t r1, int32_t r);
void instr64_0FBE_mem(int64_t addr, int32_t r);
void instr64_0FBE_reg(int32_t r1, int32_t r);
void instr64_0FBF_mem(int64_t addr, int32_t r);
void instr64_0FBF_reg(int32_t r1, int32_t r);

void instr64_F4(void);
void instr64_EB(int64_t imm);

#endif /* V86C_INSTRUCTIONS_64_H */

/* S8: mul/div/shifts/bit ops at 64 bits (the bodies the 0x2NN arms call) */
void instr64_F7_4_mem(int64_t addr);
void instr64_F7_4_reg(int32_t r1);
void instr64_F7_5_mem(int64_t addr);
void instr64_F7_5_reg(int32_t r1);
void instr64_F7_6_mem(int64_t addr);
void instr64_F7_6_reg(int32_t r1);
void instr64_F7_7_mem(int64_t addr);
void instr64_F7_7_reg(int32_t r1);
void instr64_0FAF_mem(int64_t addr, int32_t r);
void instr64_0FAF_reg(int32_t r1, int32_t r);
void instr64_69_mem(int64_t addr, int32_t r, int64_t imm);
void instr64_69_reg(int32_t r1, int32_t r, int64_t imm);
void instr64_6B_mem(int64_t addr, int32_t r, int64_t imm);
void instr64_6B_reg(int32_t r1, int32_t r, int64_t imm);
void instr64_C1_0_mem(int64_t addr, int64_t imm);
void instr64_C1_0_reg(int32_t r1, int64_t imm);
void instr64_C1_1_mem(int64_t addr, int64_t imm);
void instr64_C1_1_reg(int32_t r1, int64_t imm);
void instr64_C1_4_mem(int64_t addr, int64_t imm);
void instr64_C1_4_reg(int32_t r1, int64_t imm);
void instr64_C1_5_mem(int64_t addr, int64_t imm);
void instr64_C1_5_reg(int32_t r1, int64_t imm);
void instr64_C1_7_mem(int64_t addr, int64_t imm);
void instr64_C1_7_reg(int32_t r1, int64_t imm);
void instr64_D1_0_mem(int64_t addr);
void instr64_D1_0_reg(int32_t r1);
void instr64_D1_1_mem(int64_t addr);
void instr64_D1_1_reg(int32_t r1);
void instr64_D1_4_mem(int64_t addr);
void instr64_D1_4_reg(int32_t r1);
void instr64_D1_5_mem(int64_t addr);
void instr64_D1_5_reg(int32_t r1);
void instr64_D1_7_mem(int64_t addr);
void instr64_D1_7_reg(int32_t r1);
void instr64_D3_0_mem(int64_t addr);
void instr64_D3_0_reg(int32_t r1);
void instr64_D3_1_mem(int64_t addr);
void instr64_D3_1_reg(int32_t r1);
void instr64_D3_4_mem(int64_t addr);
void instr64_D3_4_reg(int32_t r1);
void instr64_D3_5_mem(int64_t addr);
void instr64_D3_5_reg(int32_t r1);
void instr64_D3_7_mem(int64_t addr);
void instr64_D3_7_reg(int32_t r1);
void instr64_0FA5_mem(int64_t addr, int32_t r);
void instr64_0FA5_reg(int32_t r1, int32_t r);
void instr64_0FAD_mem(int64_t addr, int32_t r);
void instr64_0FAD_reg(int32_t r1, int32_t r);
void instr64_0FBC_mem(int64_t addr, int32_t r);
void instr64_0FBC_reg(int32_t r1, int32_t r);
void instr64_0FBD_mem(int64_t addr, int32_t r);
void instr64_0FBD_reg(int32_t r1, int32_t r);
void instr64_0FBA_4_mem(int64_t addr, int64_t imm);
void instr64_0FBA_4_reg(int32_t r1, int64_t imm);
void instr64_0FBA_5_mem(int64_t addr, int64_t imm);
void instr64_0FBA_5_reg(int32_t r1, int64_t imm);
void instr64_0FBA_6_mem(int64_t addr, int64_t imm);
void instr64_0FBA_6_reg(int32_t r1, int64_t imm);
void instr64_0FBA_7_mem(int64_t addr, int64_t imm);
void instr64_0FBA_7_reg(int32_t r1, int64_t imm);
void instr64_0FC8(void);
void instr64_0FC9(void);
void instr64_0FCA(void);
void instr64_0FCB(void);
void instr64_0FCC(void);
void instr64_0FCD(void);
void instr64_0FCE(void);
void instr64_0FCF(void);

/* S9: string Q forms (five opcodes x {none, F3, F2}) and the atomic family */
/* the 64-bit stack push, exported for exceptions.c's 8-byte interrupt frame (this unit) */
void push64(uint64_t value);
void instr64_98(void); void instr64_99(void);
void instr64_0FC7_6_reg(int32_t r);
void instr64_0FC7_6_mem(int64_t addr);
void instr64_A5(void); void instr64_F3A5(void); void instr64_F2A5(void);
void instr64_A7(void); void instr64_F3A7(void); void instr64_F2A7(void);
void instr64_AB(void); void instr64_F3AB(void); void instr64_F2AB(void);
void instr64_AD(void); void instr64_F3AD(void); void instr64_F2AD(void);
void instr64_AF(void); void instr64_F3AF(void); void instr64_F2AF(void);
void instr64_0FB1_mem(int64_t addr, int32_t r);
void instr64_0FB1_reg(int32_t r1, int32_t r);
void instr64_0FC1_mem(int64_t addr, int32_t r);
void instr64_0FC1_reg(int32_t r1, int32_t r);
void instr64_0FC7_1_mem(int64_t addr);
void instr64_0FC7_1_reg(int32_t r1);

/* S10: the two SSE/MMX encodings whose operand WIDTH changes with REX.W */
void instr64_0F6E_reg(int32_t r1, int32_t r2);
void instr64_0F6E_mem(int64_t addr, int32_t r);
void instr64_0F2A_reg(int32_t r1, int32_t r2);
void instr64_0F2A_mem(int64_t addr, int32_t r);
void instr64_660F6E_reg(int32_t r1, int32_t r2);
void instr64_660F6E_mem(int64_t addr, int32_t r);
void instr64_F20F2A_reg(int32_t r1, int32_t r2);
void instr64_F20F2A_mem(int64_t addr, int32_t r);
void instr64_660F7E_reg(int32_t r1, int32_t r2);
void instr64_660F7E_mem(int64_t addr, int32_t r);
