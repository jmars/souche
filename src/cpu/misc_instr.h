/*
 * misc_instr.h -- the misc instruction helpers: the condition predicates, jumps and
 * loops, cmov/setcc, the stack-pointer helpers and the whole push/pop family, enter,
 * lss, xchg/bswap, and the CR0/CR4 task-switch tests.
 *
 * Port of v86/src/rust/cpu/misc_instr.rs (613 lines) plus these cpu.rs helpers:
 *   2633-2640 is_osize_32/is_asize_32   -- see modrm.h (they live with the prefixes)
 *   4210-4275 task_switch_test / task_switch_test_mmx / set_mxcsr
 *   4300-4320 get_stack_reg/set_stack_reg, get_reg_asize/set_reg_asize/decr_ecx_asize
 *   4292-4300 read_moffs
 * The brief names the first two groups explicitly; the asize-family helpers follow
 * because the loop instructions in this file and modrm_resolve both need them, and
 * read_moffs is the immaddr reader the emitter calls for the moffs forms.
 *
 * NOT PORTED HERE (the honest gap, stated rather than silently dropped -- see the unit
 * report's divergence table):
 *   - fxsave (misc_instr.rs:369-396) and fxrstor (397-428): defined entirely in terms
 *     of fpu.c's fpu_load_m80/fpu_store_m80/fpu_load_status_word/fpu_set_status_word/
 *     set_control_word, i.e. M1b.  Their caller is instructions_0f (0F AE /0,/1).
 *   - lar (458-503), lsl (505-559), verr (561-584), verw (586-613): defined entirely in
 *     terms of cpu.rs's segment-descriptor API (SegmentSelector/SegmentDescriptor,
 *     cpu.rs:381-446) and lookup_segment_selector (cpu.rs:2640-2740), which the plan
 *     (ss7 step 17) assigns to M1c's exceptions.c -- freezing that C interface here
 *     would be inventing M1c's contract.  Their callers are instructions_0f (0F 00 /4,
 *     /5, 0F 02, 0F 03).
 * The flag getters of misc_instr.rs:8-66 are in flags.c (M1a-1, P-NOT-1) and are not
 * redeclared here; the test_* predicates below call them.
 */

#ifndef V86C_MISC_INSTR_H
#define V86C_MISC_INSTR_H

#include "cpu_state.h"
#include "modrm.h" /* is_asize_32/is_osize_32 + read_imm16/read_imm32s (read_moffs) */

/* MXCSR bits (cpu.rs:255-258) */
#define MXCSR_MASK     0xFFFF
#define MXCSR_FZ       (1 << 15)
#define MXCSR_DAZ      (1 << 6)
#define MXCSR_RC_SHIFT 13

/* ---- the condition predicates (misc_instr.rs:75-92) ---- */
bool test_o(void);
bool test_b(void);
bool test_z(void);
bool test_s(void);
bool test_p(void);
bool test_be(void);
bool test_l(void);
bool test_le(void);
bool test_no(void);
bool test_nb(void);
bool test_nz(void);
bool test_ns(void);
bool test_np(void);
bool test_nbe(void);
bool test_nl(void);
bool test_nle(void);

/* ---- jumps, loops, cmov, setcc (misc_instr.rs:94-131, 364-367) ---- */
void jmp_rel16(int32_t rel16);
void jmpcc16(bool condition, int32_t imm16);
void jmpcc32(bool condition, int32_t imm32);
void loope16(int32_t imm8s);
void loopne16(int32_t imm8s);
void loop16(int32_t imm8s);
void jcxz16(int32_t imm8s);
void loope32(int32_t imm8s);
void loopne32(int32_t imm8s);
void loop32(int32_t imm8s);
void jcxz32(int32_t imm8s);
void cmovcc16(bool condition, int32_t value, int32_t r);
void cmovcc32(bool condition, int32_t value, int32_t r);
void setcc_reg(bool condition, int32_t r);
void setcc_mem(bool condition, int32_t addr);

/* ---- the stack pointer (misc_instr.rs:133-148 + cpu.rs:4300-4320) ---- */
int64_t get_stack_pointer(int32_t offset);
void adjust_stack_reg(int32_t adjustment);
int64_t get_stack_reg(void);
void set_stack_reg(int64_t value);
int32_t get_reg_asize(int32_t reg);
void set_reg_asize(bool addr_size_32, int32_t reg, int32_t value);
int32_t decr_ecx_asize(bool addr_size_32);

/* ---- push/pop (misc_instr.rs:150-280).  Every one of these can page-fault: it sets
 * cpu->pagefault and leaves the stack register UNCHANGED (upstream's `?` returns
 * before the register write). ---- */
void push16_ss16(int32_t imm16);
void push16_ss32(int32_t imm16);
void push16_ss16_mem(int32_t addr);
void push16_ss32_mem(int32_t addr);
void push16(int32_t imm16);
void push32_ss16(int32_t imm32);
void push32_ss32(int32_t imm32);
void push32_ss16_mem(int32_t addr);
void push32_ss32_mem(int32_t addr);
void push32(int32_t imm32);
void push32_sreg(int32_t i);
int32_t pop16(void);
int32_t pop16_ss16(void);
int32_t pop16_ss32(void);
int32_t pop32s(void);
int32_t pop32s_ss16(void);
int32_t pop32s_ss32(void);
void pusha16(void);
void pusha32(void);
/* cpu.rs:3054-3078 -- added by M1a-3 (M1a-2's push/pop family stopped at pusha; these are
 * the popa half, and instructions.c's instr16_61/instr32_61 are their first callers) */
void popa16(void);
void popa32(void);

/* ---- lar / lsl / verr / verw (misc_instr.rs:458-613) ----
 *
 * M1a-2 deferred these to M1c (see this header's "NOT PORTED HERE" note); M1c ported them
 * in misc_instr.c, where upstream has them, now that exceptions.h provides the
 * segment-descriptor API they are defined over. */
int32_t lar(int32_t selector, int32_t original);
int32_t lsl(int32_t selector, int32_t original);
void verr(int32_t selector);
void verw(int32_t selector);

/* ---- lss / enter (misc_instr.rs:282-362) ---- */
void lss16(int32_t addr, int32_t reg, int32_t seg);
void lss32(int32_t addr, int32_t reg, int32_t seg);
void enter16(int32_t size, int32_t nesting_level);
void enter32(int32_t size, int32_t nesting_level);

/* ---- exchange / byte swap (misc_instr.rs:430-456) ---- */
int32_t xchg8(int32_t data, int32_t r8);
int32_t xchg16(int32_t data, int32_t r16);
void xchg16r(int32_t r16);
int32_t xchg32(int32_t data, int32_t r32);
void xchg32r(int32_t r32);
void bswap(int32_t r);

/* ---- cpu.rs helpers this unit needs ---- */
int32_t read_moffs(void);
/* cpu.rs:4300-4304 -- added by M1a-3 (the cpu.rs-helper family above; instructions.c's
 * call/ret/ff-group and M1c's cpu.c are its callers) */
int32_t get_real_eip(void);
void set_mxcsr(int32_t new_mxcsr);
/* cpu.rs:4210-4218: CR0.EM|CR0.TS -> #NM.  MANDATORY for the emitter's SSE guard. */
bool task_switch_test(void);
/* cpu.rs:4256-4272: CR0.EM -> #UD, CR0.TS -> #NM (the FPU/MMX/SSE guard) */
bool task_switch_test_mmx(void);

/* ------------------------------------------------------------------ *
 * The M1c boundary.
 *
 * cpu.rs:2676-2900 (switch_seg) is exceptions.c's (plan ss7 step 17).  It is declared
 * here because lss16/lss32 are its first callers; M1c defines it, this unit does not.
 * trigger_de/ud/nm/gp are declared in cpu_state.h.
 * ------------------------------------------------------------------ */
bool switch_seg(int32_t reg, int32_t selector_raw);

#endif /* V86C_MISC_INSTR_H */
