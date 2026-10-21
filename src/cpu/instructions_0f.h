/*
 * instructions_0f.h -- the 0F table's interface.
 *
 * Mirrors instructions.h's discipline: the prototypes are the names the emitter
 * (gen/interpreter_c.js, via make_instruction_name) generates calls to, so the emitter and
 * this file cannot disagree without the compiler saying so.  M1b (plan ss7 step 13)
 * completes both the table and this header; the emitter's "unimplemented" count is the
 * measure of how much is left.
 *
 * The `_mem` form's first argument is the resolved address, the `_reg` form's the low
 * modrm bits; the second argument is `modrm_byte >> 3 & 7` in both.  As everywhere else in
 * the port a fallible call sets cpu->pagefault and returns the sentinel, so the caller
 * checks the flag before using the value.
 *
 * M1b-1 (HANDOFF=v86c) ported every 0F group that is NOT MMX/SSE, 359 of the 390 symbols
 * results/m1b-scope-non-sse.txt names.  M1c DECLARED THE LAST 31 (bottom of this header,
 * defined at the bottom of instructions_0f.c); this note is kept as the record of WHY each
 * of them had to wait for an M1c helper:
 *   - 0F 00 /2../5 (lldt/ltr/verr/verw, 16) and 0F 02/03 (lar/lsl, 8): the body is
 *     defined over cpu.rs's segment-descriptor API (lookup_segment_selector,
 *     cpu.rs:2640-2740), which misc_instr.h:20-27 already records as M1c's (exceptions.c)
 *     -- freezing that interface here would invent M1c's contract, so the emitter keeps
 *     stopping on those 24 encodings by name.
 *   - 0F 30/31/32 (wrmsr/rdtsc/rdmsr, 3), 0F 34/35 (sysenter/sysexit, 2), 0F C7 /6
 *     (rdrand, 2): cpu.rs's read_tsc/set_tsc (cpu_state.h:381 defers them to M1c's run
 *     loop), update_cs_size/cpl_changed (cpu.rs:3000-3006) and js::get_rand_int.
 *   - 0F AE /0,/1 (fxsave/fxrstor, 2) are the CONTRACT below: M1b-3 defines them.
 *
 * With M1c's additions the emitter's census reaches ZERO unimplemented call sites
 * (MEASURED: 1922 call sites, 1922 implemented, 0 unimplemented -- see
 * results/m1c-exceptions.md).
 */

#ifndef V86C_INSTRUCTIONS_0F_H
#define V86C_INSTRUCTIONS_0F_H

#include <stdint.h>

/* ------------------------------------------------------------------ *
 * THE CROSS-UNIT CONTRACT (M1b-1 / M1b-3).
 *
 * instructions_0f.rs imports these from `crate::cpu::fpu` (instructions_0f.rs:21) and
 * `crate::cpu::misc_instr` (instructions_0f.rs:22-25) -- upstream's fxsave/fxrstor live in
 * misc_instr.rs but are defined entirely over fpu.c's helpers.  ** fpu.c (M1b-3, concurrent
 * with this unit) DEFINES them; this unit only declares and calls them. **  They are NOT
 * stubbed here: a silent stub of x87 state save/restore is exactly the class of wrong this
 * port refuses (a guest that runs fsave/frstor would silently keep the wrong FPU state
 * instead of stopping).
 *
 * Every signature is upstream's (misc_instr.rs:369-428, fpu.rs):
 *     fpu_set_tag_word  fpu.rs          -- 0F 77 emms (M1b-2's file)
 *     fxsave/fxrstor    misc_instr.rs:369-428 -- 0F AE /0 and /1 (this file)
 * ------------------------------------------------------------------ */
void fpu_set_tag_word(uint16_t tag_word);
void fxsave(int32_t addr);
void fxrstor(int32_t addr);

/* ------------------------------------------------------------------ *
 * The table.  Grouped by the 0F opcode byte, in upstream's file order.
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:3664-3679 -- movzx */
void instr16_0FB6_mem(int64_t addr, int32_t r);
void instr16_0FB6_reg(int32_t r1, int32_t r);
void instr32_0FB6_mem(int64_t addr, int32_t r);
void instr32_0FB6_reg(int32_t r1, int32_t r);
void instr16_0FB7_mem(int64_t addr, int32_t r);
void instr16_0FB7_reg(int32_t r1, int32_t r);
void instr32_0FB7_mem(int64_t addr, int32_t r);
void instr32_0FB7_reg(int32_t r1, int32_t r);

/* instructions_0f.rs:4885-4893 -- pxor (66 0F EF), the nasm corpus's shared prologue */
void instr_660FEF_reg(int32_t r1, int32_t r2);
void instr_660FEF_mem(int64_t addr, int32_t r);

/* instructions_0f.rs:3789-3804 -- movsx */
void instr16_0FBE_mem(int64_t addr, int32_t r);
void instr16_0FBE_reg(int32_t r1, int32_t r);
void instr32_0FBE_mem(int64_t addr, int32_t r);
void instr32_0FBE_reg(int32_t r1, int32_t r);
void instr16_0FBF_mem(int64_t addr, int32_t r);
void instr16_0FBF_reg(int32_t r1, int32_t r);
void instr32_0FBF_mem(int64_t addr, int32_t r);
void instr32_0FBF_reg(int32_t r1, int32_t r);


/* instructions_0f.rs -- 0F 00 */
void instr16_0F00_0_mem(int64_t);
void instr16_0F00_0_reg(int32_t);
void instr16_0F00_1_mem(int64_t);
void instr16_0F00_1_reg(int32_t);

/* instructions_0f.rs -- 0F 01 */
void instr16_0F01_0_mem(int64_t);
void instr16_0F01_0_reg(int32_t);
void instr16_0F01_1_mem(int64_t);
void instr16_0F01_1_reg(int32_t);
void instr16_0F01_2_mem(int64_t);
void instr16_0F01_2_reg(int32_t);
void instr16_0F01_3_mem(int64_t);
void instr16_0F01_3_reg(int32_t);
void instr16_0F01_4_mem(int64_t);
void instr16_0F01_4_reg(int32_t);
void instr16_0F01_6_mem(int64_t);
void instr16_0F01_6_reg(int32_t);
void instr16_0F01_7_mem(int64_t);
void instr16_0F01_7_reg(int32_t);

/* instructions_0f.rs -- 0F 40 */
void instr16_0F40_mem(int64_t, int32_t);
void instr16_0F40_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 41 */
void instr16_0F41_mem(int64_t, int32_t);
void instr16_0F41_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 42 */
void instr16_0F42_mem(int64_t, int32_t);
void instr16_0F42_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 43 */
void instr16_0F43_mem(int64_t, int32_t);
void instr16_0F43_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 44 */
void instr16_0F44_mem(int64_t, int32_t);
void instr16_0F44_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 45 */
void instr16_0F45_mem(int64_t, int32_t);
void instr16_0F45_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 46 */
void instr16_0F46_mem(int64_t, int32_t);
void instr16_0F46_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 47 */
void instr16_0F47_mem(int64_t, int32_t);
void instr16_0F47_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 48 */
void instr16_0F48_mem(int64_t, int32_t);
void instr16_0F48_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 49 */
void instr16_0F49_mem(int64_t, int32_t);
void instr16_0F49_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4A */
void instr16_0F4A_mem(int64_t, int32_t);
void instr16_0F4A_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4B */
void instr16_0F4B_mem(int64_t, int32_t);
void instr16_0F4B_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4C */
void instr16_0F4C_mem(int64_t, int32_t);
void instr16_0F4C_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4D */
void instr16_0F4D_mem(int64_t, int32_t);
void instr16_0F4D_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4E */
void instr16_0F4E_mem(int64_t, int32_t);
void instr16_0F4E_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4F */
void instr16_0F4F_mem(int64_t, int32_t);
void instr16_0F4F_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 80 */
void instr16_0F80(int32_t);

/* instructions_0f.rs -- 0F 81 */
void instr16_0F81(int32_t);

/* instructions_0f.rs -- 0F 82 */
void instr16_0F82(int32_t);

/* instructions_0f.rs -- 0F 83 */
void instr16_0F83(int32_t);

/* instructions_0f.rs -- 0F 84 */
void instr16_0F84(int32_t);

/* instructions_0f.rs -- 0F 85 */
void instr16_0F85(int32_t);

/* instructions_0f.rs -- 0F 86 */
void instr16_0F86(int32_t);

/* instructions_0f.rs -- 0F 87 */
void instr16_0F87(int32_t);

/* instructions_0f.rs -- 0F 88 */
void instr16_0F88(int32_t);

/* instructions_0f.rs -- 0F 89 */
void instr16_0F89(int32_t);

/* instructions_0f.rs -- 0F 8A */
void instr16_0F8A(int32_t);

/* instructions_0f.rs -- 0F 8B */
void instr16_0F8B(int32_t);

/* instructions_0f.rs -- 0F 8C */
void instr16_0F8C(int32_t);

/* instructions_0f.rs -- 0F 8D */
void instr16_0F8D(int32_t);

/* instructions_0f.rs -- 0F 8E */
void instr16_0F8E(int32_t);

/* instructions_0f.rs -- 0F 8F */
void instr16_0F8F(int32_t);

/* instructions_0f.rs -- 0F A0 */
void instr16_0FA0(void);

/* instructions_0f.rs -- 0F A1 */
void instr16_0FA1(void);

/* instructions_0f.rs -- 0F A3 */
void instr16_0FA3_mem(int64_t, int32_t);
void instr16_0FA3_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F A4 */
void instr16_0FA4_mem(int64_t, int32_t, int32_t);
void instr16_0FA4_reg(int32_t, int32_t, int32_t);

/* instructions_0f.rs -- 0F A5 */
void instr16_0FA5_mem(int64_t, int32_t);
void instr16_0FA5_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F A8 */
void instr16_0FA8(void);

/* instructions_0f.rs -- 0F A9 */
void instr16_0FA9(void);

/* instructions_0f.rs -- 0F AB */
void instr16_0FAB_mem(int64_t, int32_t);
void instr16_0FAB_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F AC */
void instr16_0FAC_mem(int64_t, int32_t, int32_t);
void instr16_0FAC_reg(int32_t, int32_t, int32_t);

/* instructions_0f.rs -- 0F AD */
void instr16_0FAD_mem(int64_t, int32_t);
void instr16_0FAD_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F AF */
void instr16_0FAF_mem(int64_t, int32_t);
void instr16_0FAF_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B1 */
void instr16_0FB1_mem(int64_t, int32_t);
void instr16_0FB1_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B2 */
void instr16_0FB2_mem(int64_t, int32_t);
void instr16_0FB2_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B3 */
void instr16_0FB3_mem(int64_t, int32_t);
void instr16_0FB3_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B4 */
void instr16_0FB4_mem(int64_t, int32_t);
void instr16_0FB4_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B5 */
void instr16_0FB5_mem(int64_t, int32_t);
void instr16_0FB5_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B8 */
void instr16_0FB8_mem(int64_t, int32_t);
void instr16_0FB8_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BA */
void instr16_0FBA_4_mem(int64_t, int32_t);
void instr16_0FBA_4_reg(int32_t, int32_t);
void instr16_0FBA_5_mem(int64_t, int32_t);
void instr16_0FBA_5_reg(int32_t, int32_t);
void instr16_0FBA_6_mem(int64_t, int32_t);
void instr16_0FBA_6_reg(int32_t, int32_t);
void instr16_0FBA_7_mem(int64_t, int32_t);
void instr16_0FBA_7_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BB */
void instr16_0FBB_mem(int64_t, int32_t);
void instr16_0FBB_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BC */
void instr16_0FBC_mem(int64_t, int32_t);
void instr16_0FBC_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BD */
void instr16_0FBD_mem(int64_t, int32_t);
void instr16_0FBD_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F C1 */
void instr16_0FC1_mem(int64_t, int32_t);
void instr16_0FC1_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F C7 */
void instr16_0FC7_1_mem(int64_t);
void instr16_0FC7_1_reg(int32_t);
void instr16_0FC7_6_mem(int64_t);

/* instructions_0f.rs -- 0F B8 */
void instr16_F30FB8_mem(int64_t, int32_t);
void instr16_F30FB8_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 00 */
void instr32_0F00_0_mem(int64_t);
void instr32_0F00_0_reg(int32_t);
void instr32_0F00_1_mem(int64_t);
void instr32_0F00_1_reg(int32_t);

/* instructions_0f.rs -- 0F 01 */
void instr32_0F01_0_mem(int64_t);
void instr32_0F01_0_reg(int32_t);
void instr32_0F01_1_mem(int64_t);
void instr32_0F01_1_reg(int32_t);
void instr32_0F01_2_mem(int64_t);
void instr32_0F01_2_reg(int32_t);
void instr32_0F01_3_mem(int64_t);
void instr32_0F01_3_reg(int32_t);
void instr32_0F01_4_mem(int64_t);
void instr32_0F01_4_reg(int32_t);
void instr32_0F01_6_mem(int64_t);
void instr32_0F01_6_reg(int32_t);
void instr32_0F01_7_mem(int64_t);
void instr32_0F01_7_reg(int32_t);

/* instructions_0f.rs -- 0F 40 */
void instr32_0F40_mem(int64_t, int32_t);
void instr32_0F40_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 41 */
void instr32_0F41_mem(int64_t, int32_t);
void instr32_0F41_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 42 */
void instr32_0F42_mem(int64_t, int32_t);
void instr32_0F42_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 43 */
void instr32_0F43_mem(int64_t, int32_t);
void instr32_0F43_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 44 */
void instr32_0F44_mem(int64_t, int32_t);
void instr32_0F44_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 45 */
void instr32_0F45_mem(int64_t, int32_t);
void instr32_0F45_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 46 */
void instr32_0F46_mem(int64_t, int32_t);
void instr32_0F46_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 47 */
void instr32_0F47_mem(int64_t, int32_t);
void instr32_0F47_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 48 */
void instr32_0F48_mem(int64_t, int32_t);
void instr32_0F48_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 49 */
void instr32_0F49_mem(int64_t, int32_t);
void instr32_0F49_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4A */
void instr32_0F4A_mem(int64_t, int32_t);
void instr32_0F4A_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4B */
void instr32_0F4B_mem(int64_t, int32_t);
void instr32_0F4B_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4C */
void instr32_0F4C_mem(int64_t, int32_t);
void instr32_0F4C_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4D */
void instr32_0F4D_mem(int64_t, int32_t);
void instr32_0F4D_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4E */
void instr32_0F4E_mem(int64_t, int32_t);
void instr32_0F4E_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 4F */
void instr32_0F4F_mem(int64_t, int32_t);
void instr32_0F4F_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 80 */
void instr32_0F80(int32_t);

/* instructions_0f.rs -- 0F 81 */
void instr32_0F81(int32_t);

/* instructions_0f.rs -- 0F 82 */
void instr32_0F82(int32_t);

/* instructions_0f.rs -- 0F 83 */
void instr32_0F83(int32_t);

/* instructions_0f.rs -- 0F 84 */
void instr32_0F84(int32_t);

/* instructions_0f.rs -- 0F 85 */
void instr32_0F85(int32_t);

/* instructions_0f.rs -- 0F 86 */
void instr32_0F86(int32_t);

/* instructions_0f.rs -- 0F 87 */
void instr32_0F87(int32_t);

/* instructions_0f.rs -- 0F 88 */
void instr32_0F88(int32_t);

/* instructions_0f.rs -- 0F 89 */
void instr32_0F89(int32_t);

/* instructions_0f.rs -- 0F 8A */
void instr32_0F8A(int32_t);

/* instructions_0f.rs -- 0F 8B */
void instr32_0F8B(int32_t);

/* instructions_0f.rs -- 0F 8C */
void instr32_0F8C(int32_t);

/* instructions_0f.rs -- 0F 8D */
void instr32_0F8D(int32_t);

/* instructions_0f.rs -- 0F 8E */
void instr32_0F8E(int32_t);

/* instructions_0f.rs -- 0F 8F */
void instr32_0F8F(int32_t);

/* instructions_0f.rs -- 0F A0 */
void instr32_0FA0(void);

/* instructions_0f.rs -- 0F A1 */
void instr32_0FA1(void);

/* instructions_0f.rs -- 0F A3 */
void instr32_0FA3_mem(int64_t, int32_t);
void instr32_0FA3_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F A4 */
void instr32_0FA4_mem(int64_t, int32_t, int32_t);
void instr32_0FA4_reg(int32_t, int32_t, int32_t);

/* instructions_0f.rs -- 0F A5 */
void instr32_0FA5_mem(int64_t, int32_t);
void instr32_0FA5_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F A8 */
void instr32_0FA8(void);

/* instructions_0f.rs -- 0F A9 */
void instr32_0FA9(void);

/* instructions_0f.rs -- 0F AB */
void instr32_0FAB_mem(int64_t, int32_t);
void instr32_0FAB_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F AC */
void instr32_0FAC_mem(int64_t, int32_t, int32_t);
void instr32_0FAC_reg(int32_t, int32_t, int32_t);

/* instructions_0f.rs -- 0F AD */
void instr32_0FAD_mem(int64_t, int32_t);
void instr32_0FAD_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F AF */
void instr32_0FAF_mem(int64_t, int32_t);
void instr32_0FAF_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B1 */
void instr32_0FB1_mem(int64_t, int32_t);
void instr32_0FB1_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B2 */
void instr32_0FB2_mem(int64_t, int32_t);
void instr32_0FB2_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B3 */
void instr32_0FB3_mem(int64_t, int32_t);
void instr32_0FB3_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B4 */
void instr32_0FB4_mem(int64_t, int32_t);
void instr32_0FB4_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B5 */
void instr32_0FB5_mem(int64_t, int32_t);
void instr32_0FB5_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B8 */
void instr32_0FB8_mem(int64_t, int32_t);
void instr32_0FB8_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BA */
void instr32_0FBA_4_mem(int64_t, int32_t);
void instr32_0FBA_4_reg(int32_t, int32_t);
void instr32_0FBA_5_mem(int64_t, int32_t);
void instr32_0FBA_5_reg(int32_t, int32_t);
void instr32_0FBA_6_mem(int64_t, int32_t);
void instr32_0FBA_6_reg(int32_t, int32_t);
void instr32_0FBA_7_mem(int64_t, int32_t);
void instr32_0FBA_7_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BB */
void instr32_0FBB_mem(int64_t, int32_t);
void instr32_0FBB_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BC */
void instr32_0FBC_mem(int64_t, int32_t);
void instr32_0FBC_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F BD */
void instr32_0FBD_mem(int64_t, int32_t);
void instr32_0FBD_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F C1 */
void instr32_0FC1_mem(int64_t, int32_t);
void instr32_0FC1_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F C7 */
void instr32_0FC7_1_mem(int64_t);
void instr32_0FC7_1_reg(int32_t);
void instr32_0FC7_6_mem(int64_t);

/* instructions_0f.rs -- 0F B8 */
void instr32_F30FB8_mem(int64_t, int32_t);
void instr32_F30FB8_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 04 */
void instr_0F04(void);

/* instructions_0f.rs -- 0F 05 */
void instr_0F05(void);

/* instructions_0f.rs -- 0F 06 */
void instr_0F06(void);

/* instructions_0f.rs -- 0F 07 */
void instr_0F07(void);

/* instructions_0f.rs -- 0F 08 */
void instr_0F08(void);

/* instructions_0f.rs -- 0F 09 */
void instr_0F09(void);

/* instructions_0f.rs -- 0F 0A */
void instr_0F0A(void);

/* instructions_0f.rs -- 0F 0B */
void instr_0F0B(void);

/* instructions_0f.rs -- 0F 0C */
void instr_0F0C(void);

/* instructions_0f.rs -- 0F 0D */
void instr_0F0D(void);

/* instructions_0f.rs -- 0F 0E */
void instr_0F0E(void);

/* instructions_0f.rs -- 0F 0F */
void instr_0F0F(void);

/* instructions_0f.rs -- 0F 18 */
void instr_0F18_mem(int64_t, int32_t);
void instr_0F18_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 19 */
void instr_0F19_mem(int64_t, int32_t);
void instr_0F19_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 1A */
void instr_0F1A(void);

/* instructions_0f.rs -- 0F 1B */
void instr_0F1B(void);

/* instructions_0f.rs -- 0F 1C */
void instr_0F1C_mem(int64_t, int32_t);
void instr_0F1C_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 1D */
void instr_0F1D_mem(int64_t, int32_t);
void instr_0F1D_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 1E */
void instr_0F1E_mem(int64_t, int32_t);
void instr_0F1E_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 1F */
void instr_0F1F_mem(int64_t, int32_t);
void instr_0F1F_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 20 */
void instr_0F20(int32_t, int32_t);

/* instructions_0f.rs -- 0F 21 */
void instr_0F21(int32_t, int32_t);

/* instructions_0f.rs -- 0F 22 */
void instr_0F22(int32_t, int32_t);

/* instructions_0f.rs -- 0F 23 */
void instr_0F23(int32_t, int32_t);

/* instructions_0f.rs -- 0F 24 */
void instr_0F24(void);

/* instructions_0f.rs -- 0F 25 */
void instr_0F25(void);

/* instructions_0f.rs -- 0F 26 */
void instr_0F26(void);

/* instructions_0f.rs -- 0F 27 */
void instr_0F27(void);

/* instructions_0f.rs -- 0F 33 */
void instr_0F33(void);

/* instructions_0f.rs -- 0F 36 */
void instr_0F36(void);

/* instructions_0f.rs -- 0F 37 */
void instr_0F37(void);

/* instructions_0f.rs -- 0F 38 */
void instr_0F38(void);

/* instructions_0f.rs -- 0F 39 */
void instr_0F39(void);

/* instructions_0f.rs -- 0F 3A */
void instr_0F3A(void);

/* instructions_0f.rs -- 0F 3B */
void instr_0F3B(void);

/* instructions_0f.rs -- 0F 3C */
void instr_0F3C(void);

/* instructions_0f.rs -- 0F 3D */
void instr_0F3D(void);

/* instructions_0f.rs -- 0F 3E */
void instr_0F3E(void);

/* instructions_0f.rs -- 0F 3F */
void instr_0F3F(void);

/* instructions_0f.rs -- 0F 78 */
void instr_0F78(void);

/* instructions_0f.rs -- 0F 79 */
void instr_0F79(void);

/* instructions_0f.rs -- 0F 7A */
void instr_0F7A(void);

/* instructions_0f.rs -- 0F 7B */
void instr_0F7B(void);

/* instructions_0f.rs -- 0F 7C */
void instr_0F7C(void);

/* instructions_0f.rs -- 0F 7D */
void instr_0F7D(void);

/* instructions_0f.rs -- 0F 90 */
void instr_0F90_mem(int64_t, int32_t);
void instr_0F90_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 91 */
void instr_0F91_mem(int64_t, int32_t);
void instr_0F91_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 92 */
void instr_0F92_mem(int64_t, int32_t);
void instr_0F92_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 93 */
void instr_0F93_mem(int64_t, int32_t);
void instr_0F93_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 94 */
void instr_0F94_mem(int64_t, int32_t);
void instr_0F94_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 95 */
void instr_0F95_mem(int64_t, int32_t);
void instr_0F95_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 96 */
void instr_0F96_mem(int64_t, int32_t);
void instr_0F96_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 97 */
void instr_0F97_mem(int64_t, int32_t);
void instr_0F97_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 98 */
void instr_0F98_mem(int64_t, int32_t);
void instr_0F98_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 99 */
void instr_0F99_mem(int64_t, int32_t);
void instr_0F99_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 9A */
void instr_0F9A_mem(int64_t, int32_t);
void instr_0F9A_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 9B */
void instr_0F9B_mem(int64_t, int32_t);
void instr_0F9B_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 9C */
void instr_0F9C_mem(int64_t, int32_t);
void instr_0F9C_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 9D */
void instr_0F9D_mem(int64_t, int32_t);
void instr_0F9D_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 9E */
void instr_0F9E_mem(int64_t, int32_t);
void instr_0F9E_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 9F */
void instr_0F9F_mem(int64_t, int32_t);
void instr_0F9F_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F A2 */
void instr_0FA2(void);

/* instructions_0f.rs -- 0F A6 */
void instr_0FA6(void);

/* instructions_0f.rs -- 0F A7 */
void instr_0FA7(void);

/* instructions_0f.rs -- 0F AA */
void instr_0FAA(void);

/* instructions_0f.rs -- 0F AE */
void instr_0FAE_0_mem(int64_t);
void instr_0FAE_0_reg(int32_t);
void instr_0FAE_1_mem(int64_t);
void instr_0FAE_1_reg(int32_t);
void instr_0FAE_2_mem(int64_t);
void instr_0FAE_2_reg(int32_t);
void instr_0FAE_3_mem(int64_t);
void instr_0FAE_3_reg(int32_t);
void instr_0FAE_4_mem(int64_t);
void instr_0FAE_4_reg(int32_t);
void instr_0FAE_5_mem(int64_t);
void instr_0FAE_5_reg(int32_t);
void instr_0FAE_6_mem(int64_t);
void instr_0FAE_6_reg(int32_t);
void instr_0FAE_7_mem(int64_t);
void instr_0FAE_7_reg(int32_t);

/* instructions_0f.rs -- 0F B0 */
void instr_0FB0_mem(int64_t, int32_t);
void instr_0FB0_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F B9 */
void instr_0FB9(void);

/* instructions_0f.rs -- 0F C0 */
void instr_0FC0_mem(int64_t, int32_t);
void instr_0FC0_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F C8 */
void instr_0FC8(void);

/* instructions_0f.rs -- 0F C9 */
void instr_0FC9(void);

/* instructions_0f.rs -- 0F CA */
void instr_0FCA(void);

/* instructions_0f.rs -- 0F CB */
void instr_0FCB(void);

/* instructions_0f.rs -- 0F CC */
void instr_0FCC(void);

/* instructions_0f.rs -- 0F CD */
void instr_0FCD(void);

/* instructions_0f.rs -- 0F CE */
void instr_0FCE(void);

/* instructions_0f.rs -- 0F CF */
void instr_0FCF(void);

/* instructions_0f.rs -- 0F FF */
void instr_0FFF(void);

/* instructions_0f.rs -- 0F 7C */
void instr_660F7C_mem(int64_t, int32_t);
void instr_660F7C_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 7D */
void instr_660F7D_mem(int64_t, int32_t);
void instr_660F7D_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 7C */
void instr_F20F7C_mem(int64_t, int32_t);
void instr_F20F7C_reg(int32_t, int32_t);

/* instructions_0f.rs -- 0F 7D */
void instr_F20F7D_mem(int64_t, int32_t);
void instr_F20F7D_reg(int32_t, int32_t);

/* ------------------------------------------------------------------ *
 * THE MMX/SSE GROUPS (M1b-2, HANDOFF=v86c)
 *
 * The 533 symbols results/m1b-scope-mmx-sse.txt names, appended to M1b-1's block.
 * The arm/condition tables, the lane orders and the operand orders are upstream's;
 * see results/m1b2-mmx-sse.md for the divergence table and the gates.
 * ------------------------------------------------------------------ */

void instr_0F10_reg(int32_t r1, int32_t r2);
void instr_0F10_mem(int64_t addr, int32_t r);
void instr_F30F10_reg(int32_t r1, int32_t r2);
void instr_F30F10_mem(int64_t addr, int32_t r);
void instr_660F10_reg(int32_t r1, int32_t r2);
void instr_660F10_mem(int64_t addr, int32_t r);
void instr_F20F10_reg(int32_t r1, int32_t r2);
void instr_F20F10_mem(int64_t addr, int32_t r);
void instr_0F11_reg(int32_t r1, int32_t r2);
void instr_0F11_mem(int64_t addr, int32_t r);
void instr_F30F11_reg(int32_t rm_dest, int32_t reg_src);
void instr_F30F11_mem(int64_t addr, int32_t r);
void instr_660F11_reg(int32_t r1, int32_t r2);
void instr_660F11_mem(int64_t addr, int32_t r);
void instr_F20F11_reg(int32_t r1, int32_t r2);
void instr_F20F11_mem(int64_t addr, int32_t r);
void instr_0F12_mem(int64_t addr, int32_t r);
void instr_0F12_reg(int32_t r1, int32_t r2);
void instr_660F12_reg(int32_t _r1, int32_t _r);
void instr_660F12_mem(int64_t addr, int32_t r);
void instr_F20F12_reg(int32_t r1, int32_t r2);
void instr_F20F12_mem(int64_t addr, int32_t r);
void instr_F30F12_reg(int32_t r1, int32_t r2);
void instr_F30F12_mem(int64_t addr, int32_t r);
void instr_0F13_mem(int64_t addr, int32_t r);
void instr_0F13_reg(int32_t _r1, int32_t _r2);
void instr_660F13_reg(int32_t _r1, int32_t _r);
void instr_660F13_mem(int64_t addr, int32_t r);
void instr_0F14_reg(int32_t r1, int32_t r2);
void instr_0F14_mem(int64_t addr, int32_t r);
void instr_660F14_reg(int32_t r1, int32_t r2);
void instr_660F14_mem(int64_t addr, int32_t r);
void instr_0F15_reg(int32_t r1, int32_t r2);
void instr_0F15_mem(int64_t addr, int32_t r);
void instr_660F15_reg(int32_t r1, int32_t r2);
void instr_660F15_mem(int64_t addr, int32_t r);
void instr_0F16_mem(int64_t addr, int32_t r);
void instr_0F16_reg(int32_t r1, int32_t r2);
void instr_660F16_mem(int64_t addr, int32_t r);
void instr_660F16_reg(int32_t _r1, int32_t _r2);
void instr_F30F16_reg(int32_t r1, int32_t r2);
void instr_F30F16_mem(int64_t addr, int32_t r);
void instr_0F17_mem(int64_t addr, int32_t r);
void instr_0F17_reg(int32_t _r1, int32_t _r2);
void instr_660F17_mem(int64_t addr, int32_t r);
void instr_660F17_reg(int32_t _r1, int32_t _r2);
void instr_0F28_reg(int32_t r1, int32_t r2);
void instr_0F28_mem(int64_t addr, int32_t r);
void instr_660F28_reg(int32_t r1, int32_t r2);
void instr_660F28_mem(int64_t addr, int32_t r);
void instr_0F29_mem(int64_t addr, int32_t r);
void instr_0F29_reg(int32_t r1, int32_t r2);
void instr_660F29_mem(int64_t addr, int32_t r);
void instr_660F29_reg(int32_t r1, int32_t r2);
void instr_0F2A_reg(int32_t r1, int32_t r2);
void instr_0F2A_mem(int64_t addr, int32_t r);
void instr_660F2A_reg(int32_t r1, int32_t r2);
void instr_660F2A_mem(int64_t addr, int32_t r);
void instr_F20F2A_reg(int32_t r1, int32_t r2);
void instr_F20F2A_mem(int64_t addr, int32_t r);
void instr_F30F2A_reg(int32_t r1, int32_t r2);
void instr_F30F2A_mem(int64_t addr, int32_t r);
void instr_0F2B_reg(int32_t _r1, int32_t _r2);
void instr_0F2B_mem(int64_t addr, int32_t r);
void instr_660F2B_reg(int32_t _r1, int32_t _r2);
void instr_660F2B_mem(int64_t addr, int32_t r);
void instr_0F2C_mem(int64_t addr, int32_t r);
void instr_0F2C_reg(int32_t r1, int32_t r2);
void instr_660F2C_mem(int64_t addr, int32_t r);
void instr_660F2C_reg(int32_t r1, int32_t r2);
void instr_F20F2C_reg(int32_t r1, int32_t r2);
void instr_F20F2C_mem(int64_t addr, int32_t r);
void instr_F30F2C_mem(int64_t addr, int32_t r);
void instr_F30F2C_reg(int32_t r1, int32_t r2);
void instr_0F2D_reg(int32_t r1, int32_t r2);
void instr_0F2D_mem(int64_t addr, int32_t r);
void instr_660F2D_reg(int32_t r1, int32_t r2);
void instr_660F2D_mem(int64_t addr, int32_t r);
void instr_F20F2D_reg(int32_t r1, int32_t r2);
void instr_F20F2D_mem(int64_t addr, int32_t r);
void instr_F30F2D_reg(int32_t r1, int32_t r2);
void instr_F30F2D_mem(int64_t addr, int32_t r);
void instr_0F2E_reg(int32_t r1, int32_t r2);
void instr_0F2E_mem(int64_t addr, int32_t r);
void instr_660F2E_reg(int32_t r1, int32_t r2);
void instr_660F2E_mem(int64_t addr, int32_t r);
void instr_0F2F_reg(int32_t r1, int32_t r2);
void instr_0F2F_mem(int64_t addr, int32_t r);
void instr_660F2F_reg(int32_t r1, int32_t r2);
void instr_660F2F_mem(int64_t addr, int32_t r);
void instr_0F50_reg(int32_t r1, int32_t r2);
void instr_0F50_mem(int64_t _addr, int32_t _r1);
void instr_660F50_reg(int32_t r1, int32_t r2);
void instr_660F50_mem(int64_t _addr, int32_t _r1);
void instr_0F51_reg(int32_t r1, int32_t r2);
void instr_0F51_mem(int64_t addr, int32_t r);
void instr_660F51_reg(int32_t r1, int32_t r2);
void instr_660F51_mem(int64_t addr, int32_t r);
void instr_F20F51_reg(int32_t r1, int32_t r2);
void instr_F20F51_mem(int64_t addr, int32_t r);
void instr_F30F51_reg(int32_t r1, int32_t r2);
void instr_F30F51_mem(int64_t addr, int32_t r);
void instr_0F52_reg(int32_t r1, int32_t r2);
void instr_0F52_mem(int64_t addr, int32_t r);
void instr_F30F52_reg(int32_t r1, int32_t r2);
void instr_F30F52_mem(int64_t addr, int32_t r);
void instr_0F53_reg(int32_t r1, int32_t r2);
void instr_0F53_mem(int64_t addr, int32_t r);
void instr_F30F53_reg(int32_t r1, int32_t r2);
void instr_F30F53_mem(int64_t addr, int32_t r);
void instr_0F54_reg(int32_t r1, int32_t r2);
void instr_0F54_mem(int64_t addr, int32_t r);
void instr_660F54_reg(int32_t r1, int32_t r2);
void instr_660F54_mem(int64_t addr, int32_t r);
void instr_0F55_reg(int32_t r1, int32_t r2);
void instr_0F55_mem(int64_t addr, int32_t r);
void instr_660F55_reg(int32_t r1, int32_t r2);
void instr_660F55_mem(int64_t addr, int32_t r);
void instr_0F56_reg(int32_t r1, int32_t r2);
void instr_0F56_mem(int64_t addr, int32_t r);
void instr_660F56_reg(int32_t r1, int32_t r2);
void instr_660F56_mem(int64_t addr, int32_t r);
void instr_0F57_reg(int32_t r1, int32_t r2);
void instr_0F57_mem(int64_t addr, int32_t r);
void instr_660F57_reg(int32_t r1, int32_t r2);
void instr_660F57_mem(int64_t addr, int32_t r);
void instr_0F58_reg(int32_t r1, int32_t r2);
void instr_0F58_mem(int64_t addr, int32_t r);
void instr_660F58_reg(int32_t r1, int32_t r2);
void instr_660F58_mem(int64_t addr, int32_t r);
void instr_F20F58_reg(int32_t r1, int32_t r2);
void instr_F20F58_mem(int64_t addr, int32_t r);
void instr_F30F58_reg(int32_t r1, int32_t r2);
void instr_F30F58_mem(int64_t addr, int32_t r);
void instr_0F59_reg(int32_t r1, int32_t r2);
void instr_0F59_mem(int64_t addr, int32_t r);
void instr_660F59_reg(int32_t r1, int32_t r2);
void instr_660F59_mem(int64_t addr, int32_t r);
void instr_F20F59_reg(int32_t r1, int32_t r2);
void instr_F20F59_mem(int64_t addr, int32_t r);
void instr_F30F59_reg(int32_t r1, int32_t r2);
void instr_F30F59_mem(int64_t addr, int32_t r);
void instr_0F5A_reg(int32_t r1, int32_t r2);
void instr_0F5A_mem(int64_t addr, int32_t r);
void instr_660F5A_reg(int32_t r1, int32_t r2);
void instr_660F5A_mem(int64_t addr, int32_t r);
void instr_F20F5A_reg(int32_t r1, int32_t r2);
void instr_F20F5A_mem(int64_t addr, int32_t r);
void instr_F30F5A_reg(int32_t r1, int32_t r2);
void instr_F30F5A_mem(int64_t addr, int32_t r);
void instr_0F5B_reg(int32_t r1, int32_t r2);
void instr_0F5B_mem(int64_t addr, int32_t r);
void instr_660F5B_reg(int32_t r1, int32_t r2);
void instr_660F5B_mem(int64_t addr, int32_t r);
void instr_F30F5B_reg(int32_t r1, int32_t r2);
void instr_F30F5B_mem(int64_t addr, int32_t r);
void instr_0F5C_reg(int32_t r1, int32_t r2);
void instr_0F5C_mem(int64_t addr, int32_t r);
void instr_660F5C_reg(int32_t r1, int32_t r2);
void instr_660F5C_mem(int64_t addr, int32_t r);
void instr_F20F5C_reg(int32_t r1, int32_t r2);
void instr_F20F5C_mem(int64_t addr, int32_t r);
void instr_F30F5C_reg(int32_t r1, int32_t r2);
void instr_F30F5C_mem(int64_t addr, int32_t r);
void instr_0F5D_reg(int32_t r1, int32_t r2);
void instr_0F5D_mem(int64_t addr, int32_t r);
void instr_660F5D_reg(int32_t r1, int32_t r2);
void instr_660F5D_mem(int64_t addr, int32_t r);
void instr_F20F5D_reg(int32_t r1, int32_t r2);
void instr_F20F5D_mem(int64_t addr, int32_t r);
void instr_F30F5D_reg(int32_t r1, int32_t r2);
void instr_F30F5D_mem(int64_t addr, int32_t r);
void instr_0F5E_reg(int32_t r1, int32_t r2);
void instr_0F5E_mem(int64_t addr, int32_t r);
void instr_660F5E_reg(int32_t r1, int32_t r2);
void instr_660F5E_mem(int64_t addr, int32_t r);
void instr_F20F5E_reg(int32_t r1, int32_t r2);
void instr_F20F5E_mem(int64_t addr, int32_t r);
void instr_F30F5E_reg(int32_t r1, int32_t r2);
void instr_F30F5E_mem(int64_t addr, int32_t r);
void instr_0F5F_reg(int32_t r1, int32_t r2);
void instr_0F5F_mem(int64_t addr, int32_t r);
void instr_660F5F_reg(int32_t r1, int32_t r2);
void instr_660F5F_mem(int64_t addr, int32_t r);
void instr_F20F5F_reg(int32_t r1, int32_t r2);
void instr_F20F5F_mem(int64_t addr, int32_t r);
void instr_F30F5F_reg(int32_t r1, int32_t r2);
void instr_F30F5F_mem(int64_t addr, int32_t r);
void instr_0F60_reg(int32_t r1, int32_t r2);
void instr_0F60_mem(int64_t addr, int32_t r);
void instr_660F60_reg(int32_t r1, int32_t r2);
void instr_660F60_mem(int64_t addr, int32_t r);
void instr_0F61_reg(int32_t r1, int32_t r2);
void instr_0F61_mem(int64_t addr, int32_t r);
void instr_660F61_reg(int32_t r1, int32_t r2);
void instr_660F61_mem(int64_t addr, int32_t r);
void instr_0F62_reg(int32_t r1, int32_t r2);
void instr_0F62_mem(int64_t addr, int32_t r);
void instr_660F62_reg(int32_t r1, int32_t r2);
void instr_660F62_mem(int64_t addr, int32_t r);
void instr_0F63_reg(int32_t r1, int32_t r2);
void instr_0F63_mem(int64_t addr, int32_t r);
void instr_660F63_reg(int32_t r1, int32_t r2);
void instr_660F63_mem(int64_t addr, int32_t r);
void instr_0F64_reg(int32_t r1, int32_t r2);
void instr_0F64_mem(int64_t addr, int32_t r);
void instr_660F64_reg(int32_t r1, int32_t r2);
void instr_660F64_mem(int64_t addr, int32_t r);
void instr_0F65_reg(int32_t r1, int32_t r2);
void instr_0F65_mem(int64_t addr, int32_t r);
void instr_660F65_reg(int32_t r1, int32_t r2);
void instr_660F65_mem(int64_t addr, int32_t r);
void instr_0F66_reg(int32_t r1, int32_t r2);
void instr_0F66_mem(int64_t addr, int32_t r);
void instr_660F66_reg(int32_t r1, int32_t r2);
void instr_660F66_mem(int64_t addr, int32_t r);
void instr_0F67_reg(int32_t r1, int32_t r2);
void instr_0F67_mem(int64_t addr, int32_t r);
void instr_660F67_reg(int32_t r1, int32_t r2);
void instr_660F67_mem(int64_t addr, int32_t r);
void instr_0F68_reg(int32_t r1, int32_t r2);
void instr_0F68_mem(int64_t addr, int32_t r);
void instr_660F68_reg(int32_t r1, int32_t r2);
void instr_660F68_mem(int64_t addr, int32_t r);
void instr_0F69_reg(int32_t r1, int32_t r2);
void instr_0F69_mem(int64_t addr, int32_t r);
void instr_660F69_reg(int32_t r1, int32_t r2);
void instr_660F69_mem(int64_t addr, int32_t r);
void instr_0F6A_reg(int32_t r1, int32_t r2);
void instr_0F6A_mem(int64_t addr, int32_t r);
void instr_660F6A_reg(int32_t r1, int32_t r2);
void instr_660F6A_mem(int64_t addr, int32_t r);
void instr_0F6B_reg(int32_t r1, int32_t r2);
void instr_0F6B_mem(int64_t addr, int32_t r);
void instr_660F6B_reg(int32_t r1, int32_t r2);
void instr_660F6B_mem(int64_t addr, int32_t r);
void instr_0F6C_mem(int64_t _addr, int32_t _r);
void instr_0F6C_reg(int32_t _r1, int32_t _r2);
void instr_660F6C_reg(int32_t r1, int32_t r2);
void instr_660F6C_mem(int64_t addr, int32_t r);
void instr_0F6D_mem(int64_t _addr, int32_t _r);
void instr_0F6D_reg(int32_t _r1, int32_t _r2);
void instr_660F6D_reg(int32_t r1, int32_t r2);
void instr_660F6D_mem(int64_t addr, int32_t r);
void instr_0F6E_reg(int32_t r1, int32_t r2);
void instr_0F6E_mem(int64_t addr, int32_t r);
void instr_660F6E_reg(int32_t r1, int32_t r2);
void instr_660F6E_mem(int64_t addr, int32_t r);
void instr_0F6F_reg(int32_t r1, int32_t r2);
void instr_0F6F_mem(int64_t addr, int32_t r);
void instr_660F6F_reg(int32_t r1, int32_t r2);
void instr_660F6F_mem(int64_t addr, int32_t r);
void instr_F30F6F_reg(int32_t r1, int32_t r2);
void instr_F30F6F_mem(int64_t addr, int32_t r);
void instr_0F70_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_0F70_mem(int64_t addr, int32_t r, int32_t imm);
void instr_660F70_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_660F70_mem(int64_t addr, int32_t r, int32_t imm);
void instr_F20F70_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_F20F70_mem(int64_t addr, int32_t r, int32_t imm);
void instr_F30F70_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_F30F70_mem(int64_t addr, int32_t r, int32_t imm);
void instr_0F71_2_reg(int32_t r, int32_t imm8);
void instr_0F71_4_reg(int32_t r, int32_t imm8);
void instr_0F71_6_reg(int32_t r, int32_t imm8);
void instr_0F71_2_mem(int64_t _addr, int32_t _r);
void instr_0F71_4_mem(int64_t _addr, int32_t _r);
void instr_0F71_6_mem(int64_t _addr, int32_t _r);
void instr_660F71_2_reg(int32_t r, int32_t imm8);
void instr_660F71_4_reg(int32_t r, int32_t imm8);
void instr_660F71_6_reg(int32_t r, int32_t imm8);
void instr_660F71_2_mem(int64_t _addr, int32_t _r);
void instr_660F71_4_mem(int64_t _addr, int32_t _r);
void instr_660F71_6_mem(int64_t _addr, int32_t _r);
void instr_0F72_2_reg(int32_t r, int32_t imm8);
void instr_0F72_4_reg(int32_t r, int32_t imm8);
void instr_0F72_6_reg(int32_t r, int32_t imm8);
void instr_0F72_2_mem(int64_t _addr, int32_t _r);
void instr_0F72_4_mem(int64_t _addr, int32_t _r);
void instr_0F72_6_mem(int64_t _addr, int32_t _r);
void instr_660F72_2_reg(int32_t r, int32_t imm8);
void instr_660F72_4_reg(int32_t r, int32_t imm8);
void instr_660F72_6_reg(int32_t r, int32_t imm8);
void instr_660F72_2_mem(int64_t _addr, int32_t _r);
void instr_660F72_4_mem(int64_t _addr, int32_t _r);
void instr_660F72_6_mem(int64_t _addr, int32_t _r);
void instr_0F73_2_reg(int32_t r, int32_t imm8);
void instr_0F73_6_reg(int32_t r, int32_t imm8);
void instr_0F73_2_mem(int64_t _addr, int32_t _r);
void instr_0F73_6_mem(int64_t _addr, int32_t _r);
void instr_660F73_2_reg(int32_t r, int32_t imm8);
void instr_660F73_6_reg(int32_t r, int32_t imm8);
void instr_660F73_3_mem(int64_t _addr, int32_t _r);
void instr_660F73_6_mem(int64_t _addr, int32_t _r);
void instr_660F73_2_mem(int64_t _addr, int32_t _r);
void instr_660F73_7_mem(int64_t _addr, int32_t _r);
void instr_660F73_3_reg(int32_t r, int32_t imm8);
void instr_660F73_7_reg(int32_t r, int32_t imm8);
void instr_0F74_reg(int32_t r1, int32_t r2);
void instr_0F74_mem(int64_t addr, int32_t r);
void instr_660F74_reg(int32_t r1, int32_t r2);
void instr_660F74_mem(int64_t addr, int32_t r);
void instr_0F75_reg(int32_t r1, int32_t r2);
void instr_0F75_mem(int64_t addr, int32_t r);
void instr_660F75_reg(int32_t r1, int32_t r2);
void instr_660F75_mem(int64_t addr, int32_t r);
void instr_0F76_reg(int32_t r1, int32_t r2);
void instr_0F76_mem(int64_t addr, int32_t r);
void instr_660F76_reg(int32_t r1, int32_t r2);
void instr_660F76_mem(int64_t addr, int32_t r);
void instr_0F77(void);
void instr_0F7E_reg(int32_t r1, int32_t r2);
void instr_0F7E_mem(int64_t addr, int32_t r);
void instr_660F7E_reg(int32_t r1, int32_t r2);
void instr_660F7E_mem(int64_t addr, int32_t r);
void instr_F30F7E_mem(int64_t addr, int32_t r);
void instr_F30F7E_reg(int32_t r1, int32_t r2);
void instr_0F7F_mem(int64_t addr, int32_t r);
void instr_0F7F_reg(int32_t r1, int32_t r2);
void instr_660F7F_mem(int64_t addr, int32_t r);
void instr_660F7F_reg(int32_t r1, int32_t r2);
void instr_F30F7F_mem(int64_t addr, int32_t r);
void instr_F30F7F_reg(int32_t r1, int32_t r2);
void instr_0FC2_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_0FC2_mem(int64_t addr, int32_t r, int32_t imm);
void instr_660FC2_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_660FC2_mem(int64_t addr, int32_t r, int32_t imm);
void instr_F20FC2_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_F20FC2_mem(int64_t addr, int32_t r, int32_t imm);
void instr_F30FC2_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_F30FC2_mem(int64_t addr, int32_t r, int32_t imm);
void instr_0FC3_reg(int32_t _r1, int32_t _r2);
void instr_0FC3_mem(int64_t addr, int32_t r);
void instr_0FC4_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_0FC4_mem(int64_t addr, int32_t r, int32_t imm);
void instr_660FC4_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_660FC4_mem(int64_t addr, int32_t r, int32_t imm);
void instr_0FC5_mem(int64_t _addr, int32_t _r, int32_t _imm8);
void instr_0FC5_reg(int32_t r1, int32_t r2, int32_t imm8);
void instr_660FC5_mem(int64_t _addr, int32_t _r, int32_t _imm8);
void instr_660FC5_reg(int32_t r1, int32_t r2, int32_t imm8);
void instr_0FC6_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_0FC6_mem(int64_t addr, int32_t r, int32_t imm);
void instr_660FC6_reg(int32_t r1, int32_t r2, int32_t imm);
void instr_660FC6_mem(int64_t addr, int32_t r, int32_t imm);
void instr_0FD0(void);
void instr_0FD1_reg(int32_t r1, int32_t r2);
void instr_0FD1_mem(int64_t addr, int32_t r);
void instr_660FD1_reg(int32_t r1, int32_t r2);
void instr_660FD1_mem(int64_t addr, int32_t r);
void instr_0FD2_reg(int32_t r1, int32_t r2);
void instr_0FD2_mem(int64_t addr, int32_t r);
void instr_660FD2_reg(int32_t r1, int32_t r2);
void instr_660FD2_mem(int64_t addr, int32_t r);
void instr_0FD3_reg(int32_t r1, int32_t r2);
void instr_0FD3_mem(int64_t addr, int32_t r);
void instr_660FD3_reg(int32_t r1, int32_t r2);
void instr_660FD3_mem(int64_t addr, int32_t r);
void instr_0FD4_reg(int32_t r1, int32_t r2);
void instr_0FD4_mem(int64_t addr, int32_t r);
void instr_660FD4_reg(int32_t r1, int32_t r2);
void instr_660FD4_mem(int64_t addr, int32_t r);
void instr_0FD5_reg(int32_t r1, int32_t r2);
void instr_0FD5_mem(int64_t addr, int32_t r);
void instr_660FD5_reg(int32_t r1, int32_t r2);
void instr_660FD5_mem(int64_t addr, int32_t r);
void instr_0FD6_mem(int64_t _addr, int32_t _r);
void instr_0FD6_reg(int32_t _r1, int32_t _r2);
void instr_660FD6_mem(int64_t addr, int32_t r);
void instr_660FD6_reg(int32_t r1, int32_t r2);
void instr_F20FD6_mem(int64_t _addr, int32_t _r);
void instr_F20FD6_reg(int32_t r1, int32_t r2);
void instr_F30FD6_mem(int64_t _addr, int32_t _r);
void instr_F30FD6_reg(int32_t r1, int32_t r2);
void instr_0FD7_mem(int64_t _addr, int32_t _r);
void instr_0FD7_reg(int32_t r1, int32_t r2);
void instr_660FD7_mem(int64_t _addr, int32_t _r);
void instr_660FD7_reg(int32_t r1, int32_t r2);
void instr_0FD8_reg(int32_t r1, int32_t r2);
void instr_0FD8_mem(int64_t addr, int32_t r);
void instr_660FD8_reg(int32_t r1, int32_t r2);
void instr_660FD8_mem(int64_t addr, int32_t r);
void instr_0FD9_reg(int32_t r1, int32_t r2);
void instr_0FD9_mem(int64_t addr, int32_t r);
void instr_660FD9_reg(int32_t r1, int32_t r2);
void instr_660FD9_mem(int64_t addr, int32_t r);
void instr_0FDA_reg(int32_t r1, int32_t r2);
void instr_0FDA_mem(int64_t addr, int32_t r);
void instr_660FDA_reg(int32_t r1, int32_t r2);
void instr_660FDA_mem(int64_t addr, int32_t r);
void instr_0FDB_reg(int32_t r1, int32_t r2);
void instr_0FDB_mem(int64_t addr, int32_t r);
void instr_660FDB_reg(int32_t r1, int32_t r2);
void instr_660FDB_mem(int64_t addr, int32_t r);
void instr_0FDC_reg(int32_t r1, int32_t r2);
void instr_0FDC_mem(int64_t addr, int32_t r);
void instr_660FDC_reg(int32_t r1, int32_t r2);
void instr_660FDC_mem(int64_t addr, int32_t r);
void instr_0FDD_reg(int32_t r1, int32_t r2);
void instr_0FDD_mem(int64_t addr, int32_t r);
void instr_660FDD_reg(int32_t r1, int32_t r2);
void instr_660FDD_mem(int64_t addr, int32_t r);
void instr_0FDE_reg(int32_t r1, int32_t r2);
void instr_0FDE_mem(int64_t addr, int32_t r);
void instr_660FDE_reg(int32_t r1, int32_t r2);
void instr_660FDE_mem(int64_t addr, int32_t r);
void instr_0FDF_reg(int32_t r1, int32_t r2);
void instr_0FDF_mem(int64_t addr, int32_t r);
void instr_660FDF_reg(int32_t r1, int32_t r2);
void instr_660FDF_mem(int64_t addr, int32_t r);
void instr_0FE0_reg(int32_t r1, int32_t r2);
void instr_0FE0_mem(int64_t addr, int32_t r);
void instr_660FE0_reg(int32_t r1, int32_t r2);
void instr_660FE0_mem(int64_t addr, int32_t r);
void instr_0FE1_reg(int32_t r1, int32_t r2);
void instr_0FE1_mem(int64_t addr, int32_t r);
void instr_660FE1_reg(int32_t r1, int32_t r2);
void instr_660FE1_mem(int64_t addr, int32_t r);
void instr_0FE2_reg(int32_t r1, int32_t r2);
void instr_0FE2_mem(int64_t addr, int32_t r);
void instr_660FE2_reg(int32_t r1, int32_t r2);
void instr_660FE2_mem(int64_t addr, int32_t r);
void instr_0FE3_reg(int32_t r1, int32_t r2);
void instr_0FE3_mem(int64_t addr, int32_t r);
void instr_660FE3_reg(int32_t r1, int32_t r2);
void instr_660FE3_mem(int64_t addr, int32_t r);
void instr_0FE4_reg(int32_t r1, int32_t r2);
void instr_0FE4_mem(int64_t addr, int32_t r);
void instr_660FE4_reg(int32_t r1, int32_t r2);
void instr_660FE4_mem(int64_t addr, int32_t r);
void instr_0FE5_reg(int32_t r1, int32_t r2);
void instr_0FE5_mem(int64_t addr, int32_t r);
void instr_660FE5_reg(int32_t r1, int32_t r2);
void instr_660FE5_mem(int64_t addr, int32_t r);
void instr_0FE6_mem(int64_t _addr, int32_t _r);
void instr_0FE6_reg(int32_t _r1, int32_t _r2);
void instr_660FE6_mem(int64_t addr, int32_t r);
void instr_660FE6_reg(int32_t r1, int32_t r2);
void instr_F20FE6_mem(int64_t addr, int32_t r);
void instr_F20FE6_reg(int32_t r1, int32_t r2);
void instr_F30FE6_mem(int64_t addr, int32_t r);
void instr_F30FE6_reg(int32_t r1, int32_t r2);
void instr_0FE7_mem(int64_t addr, int32_t r);
void instr_0FE7_reg(int32_t _r1, int32_t _r2);
void instr_660FE7_reg(int32_t _r1, int32_t _r2);
void instr_660FE7_mem(int64_t addr, int32_t r);
void instr_0FE8_reg(int32_t r1, int32_t r2);
void instr_0FE8_mem(int64_t addr, int32_t r);
void instr_660FE8_reg(int32_t r1, int32_t r2);
void instr_660FE8_mem(int64_t addr, int32_t r);
void instr_0FE9_reg(int32_t r1, int32_t r2);
void instr_0FE9_mem(int64_t addr, int32_t r);
void instr_660FE9_reg(int32_t r1, int32_t r2);
void instr_660FE9_mem(int64_t addr, int32_t r);
void instr_0FEA_reg(int32_t r1, int32_t r2);
void instr_0FEA_mem(int64_t addr, int32_t r);
void instr_660FEA_reg(int32_t r1, int32_t r2);
void instr_660FEA_mem(int64_t addr, int32_t r);
void instr_0FEB_reg(int32_t r1, int32_t r2);
void instr_0FEB_mem(int64_t addr, int32_t r);
void instr_660FEB_reg(int32_t r1, int32_t r2);
void instr_660FEB_mem(int64_t addr, int32_t r);
void instr_0FEC_reg(int32_t r1, int32_t r2);
void instr_0FEC_mem(int64_t addr, int32_t r);
void instr_660FEC_reg(int32_t r1, int32_t r2);
void instr_660FEC_mem(int64_t addr, int32_t r);
void instr_0FED_reg(int32_t r1, int32_t r2);
void instr_0FED_mem(int64_t addr, int32_t r);
void instr_660FED_reg(int32_t r1, int32_t r2);
void instr_660FED_mem(int64_t addr, int32_t r);
void instr_0FEE_reg(int32_t r1, int32_t r2);
void instr_0FEE_mem(int64_t addr, int32_t r);
void instr_660FEE_reg(int32_t r1, int32_t r2);
void instr_660FEE_mem(int64_t addr, int32_t r);
void instr_0FEF_reg(int32_t r1, int32_t r2);
void instr_0FEF_mem(int64_t addr, int32_t r);
void instr_0FF0(void);
void instr_0FF1_reg(int32_t r1, int32_t r2);
void instr_0FF1_mem(int64_t addr, int32_t r);
void instr_660FF1_reg(int32_t r1, int32_t r2);
void instr_660FF1_mem(int64_t addr, int32_t r);
void instr_0FF2_reg(int32_t r1, int32_t r2);
void instr_0FF2_mem(int64_t addr, int32_t r);
void instr_660FF2_reg(int32_t r1, int32_t r2);
void instr_660FF2_mem(int64_t addr, int32_t r);
void instr_0FF3_reg(int32_t r1, int32_t r2);
void instr_0FF3_mem(int64_t addr, int32_t r);
void instr_660FF3_reg(int32_t r1, int32_t r2);
void instr_660FF3_mem(int64_t addr, int32_t r);
void instr_0FF4_reg(int32_t r1, int32_t r2);
void instr_0FF4_mem(int64_t addr, int32_t r);
void instr_660FF4_reg(int32_t r1, int32_t r2);
void instr_660FF4_mem(int64_t addr, int32_t r);
void instr_0FF5_reg(int32_t r1, int32_t r2);
void instr_0FF5_mem(int64_t addr, int32_t r);
void instr_660FF5_reg(int32_t r1, int32_t r2);
void instr_660FF5_mem(int64_t addr, int32_t r);
void instr_0FF6_reg(int32_t r1, int32_t r2);
void instr_0FF6_mem(int64_t addr, int32_t r);
void instr_660FF6_reg(int32_t r1, int32_t r2);
void instr_660FF6_mem(int64_t addr, int32_t r);
void instr_0FF7_mem(int64_t _addr, int32_t _r);
void instr_0FF7_reg(int32_t r1, int32_t r2);
void instr_660FF7_mem(int64_t _addr, int32_t _r);
void instr_660FF7_reg(int32_t r1, int32_t r2);
void instr_0FF8_reg(int32_t r1, int32_t r2);
void instr_0FF8_mem(int64_t addr, int32_t r);
void instr_660FF8_reg(int32_t r1, int32_t r2);
void instr_660FF8_mem(int64_t addr, int32_t r);
void instr_0FF9_reg(int32_t r1, int32_t r2);
void instr_0FF9_mem(int64_t addr, int32_t r);
void instr_660FF9_reg(int32_t r1, int32_t r2);
void instr_660FF9_mem(int64_t addr, int32_t r);
void instr_0FFA_reg(int32_t r1, int32_t r2);
void instr_0FFA_mem(int64_t addr, int32_t r);
void instr_660FFA_reg(int32_t r1, int32_t r2);
void instr_660FFA_mem(int64_t addr, int32_t r);
void instr_0FFB_reg(int32_t r1, int32_t r2);
void instr_0FFB_mem(int64_t addr, int32_t r);
void instr_660FFB_reg(int32_t r1, int32_t r2);
void instr_660FFB_mem(int64_t addr, int32_t r);
void instr_0FFC_reg(int32_t r1, int32_t r2);
void instr_0FFC_mem(int64_t addr, int32_t r);
void instr_660FFC_reg(int32_t r1, int32_t r2);
void instr_660FFC_mem(int64_t addr, int32_t r);
void instr_0FFD_reg(int32_t r1, int32_t r2);
void instr_0FFD_mem(int64_t addr, int32_t r);
void instr_660FFD_reg(int32_t r1, int32_t r2);
void instr_660FFD_mem(int64_t addr, int32_t r);
void instr_0FFE_reg(int32_t r1, int32_t r2);
void instr_0FFE_mem(int64_t addr, int32_t r);
void instr_660FFE_reg(int32_t r1, int32_t r2);
void instr_660FFE_mem(int64_t addr, int32_t r);

/* ------------------------------------------------------------------ *
 * M1c (HANDOFF=v86c) -- THE LAST 31 SYMBOLS.
 *
 * 0F 00 /2../5 (lldt/ltr/verr/verw), 0F 02/03 (lar/lsl), 0F 30/31/32/34/35
 * (wrmsr/rdtsc/rdmsr/sysenter/sysexit) and 0F C7 /6's register form.  The bodies are at
 * the bottom of instructions_0f.c; the header's earlier note lists which helper each of
 * them was waiting for.
 * ------------------------------------------------------------------ */
void instr16_0F00_2_mem(int64_t addr);
void instr32_0F00_2_mem(int64_t addr);
void instr16_0F00_2_reg(int32_t r);
void instr32_0F00_2_reg(int32_t r);
void instr16_0F00_3_mem(int64_t addr);
void instr32_0F00_3_mem(int64_t addr);
void instr16_0F00_3_reg(int32_t r);
void instr32_0F00_3_reg(int32_t r);
void instr16_0F00_4_mem(int64_t addr);
void instr32_0F00_4_mem(int64_t addr);
void instr16_0F00_4_reg(int32_t r);
void instr32_0F00_4_reg(int32_t r);
void instr16_0F00_5_mem(int64_t addr);
void instr32_0F00_5_mem(int64_t addr);
void instr16_0F00_5_reg(int32_t r);
void instr32_0F00_5_reg(int32_t r);

void instr16_0F02_mem(int64_t addr, int32_t r);
void instr16_0F02_reg(int32_t r1, int32_t r);
void instr32_0F02_mem(int64_t addr, int32_t r);
void instr32_0F02_reg(int32_t r1, int32_t r);
void instr16_0F03_mem(int64_t addr, int32_t r);
void instr16_0F03_reg(int32_t r1, int32_t r);
void instr32_0F03_mem(int64_t addr, int32_t r);
void instr32_0F03_reg(int32_t r1, int32_t r);

void instr_0F30(void);
void instr_0F31(void);
void instr_0F32(void);
void instr_0F34(void);
void instr_0F35(void);

void instr16_0FC7_6_reg(int32_t _r);
void instr32_0FC7_6_reg(int32_t _r);

#endif /* V86C_INSTRUCTIONS_0F_H */
