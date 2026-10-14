/*
 * sse_instr.h -- the SSE/MMX helpers instructions_0f.rs imports as
 * `use crate::cpu::sse_instr::*`.
 *
 * Port of v86/src/rust/cpu/sse_instr.rs (361 lines): the mov* helpers, the
 * bitwise/shift group's MMX (r64) and SSE (r128) bodies, the comparison/min/max
 * helpers and the four f32/f64 -> i32 conversion helpers.  Upstream's
 * instructions_0f.rs is their only caller.
 *
 * `transition_fpu_to_mmx` is also here although it lives in cpu.rs:4201-4207 rather than
 * in sse_instr.rs: it is the MMX<->x87 ALIASING TRANSITION, it is defined entirely in
 * terms of this module's subject (fpu_set_tag_word + cpu->fpu_stack_ptr), and
 * sse_instr.rs's two MMX mov* helpers and every MMX instruction in instructions_0f.rs
 * call it.  misc_instr.h:6-11 lists the neighbouring cpu.rs helpers it already owns
 * (task_switch_test, task_switch_test_mmx, set_mxcsr); this one is NOT there, so putting
 * it in a file this unit owns avoids editing M1a-2's file.
 *   READ, upstream: `fpu_set_tag_word(0)` marks every x87 slot NON-EMPTY and
 *   `*fpu_stack_ptr = 0` puts st(0) at slot 0 -- i.e. after an MMX write the x87 stack
 *   reads as eight live registers.  That is upstream's behaviour, not a bug to fix here.
 *
 * FAULT CONVENTION: as everywhere in the port, a fallible helper sets `cpu->pagefault`
 * and returns a sentinel; `return_on_pagefault!` is spelled "call, then
 * `if (cpu->pagefault) return;` before the value is used", so the transition below (which
 * upstream performs only after a successful store) is likewise behind the check.
 */

#ifndef V86C_SSE_INSTR_H
#define V86C_SSE_INSTR_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"

/* cpu.rs:4201-4207 -- the MMX/x87 aliasing transition */
void transition_fpu_to_mmx(void);

/* sse_instr.rs:4-27 -- the mov* family */
void mov_r_m64(int32_t addr, int32_t r);
void movl_r128_m64(int32_t addr, int32_t r);
void mov_r_r128(int32_t r1, int32_t r2);
void mov_r_m128(int32_t addr, int32_t r);
void mov_rm_r128(reg128 source, int32_t r);
void movh_r128_m64(int32_t addr, int32_t r);

/* sse_instr.rs:29-63 -- pand/pandn/pxor/por over xmm */
void pand_r128(reg128 source, int32_t r);
void pandn_r128(reg128 source, int32_t r);
void pxor_r128(reg128 source, int32_t r);
void por_r128(reg128 source, int32_t r);

/* sse_instr.rs:65-178 -- the shifts.  The r64 forms take an MMX destination, the r128
 * forms an XMM one; `shift` is the raw `u64` upstream passes (a register count or an
 * imm8), NOT pre-clamped -- each body clamps it itself, and the clamping differs per
 * instruction (see the .c). */
void psrlw_r64(int32_t r, uint64_t shift);
void psraw_r64(int32_t r, uint64_t shift);
void psllw_r64(int32_t r, uint64_t shift);
void psrld_r64(int32_t r, uint64_t shift);
void psrad_r64(int32_t r, uint64_t shift);
void pslld_r64(int32_t r, uint64_t shift);
void psrlq_r64(int32_t r, uint64_t shift);
void psllq_r64(int32_t r, uint64_t shift);
void psrlw_r128(int32_t r, uint64_t shift);
void psraw_r128(int32_t r, uint64_t shift);
void psllw_r128(int32_t r, uint64_t shift);
void psrld_r128(int32_t r, uint64_t shift);
void psrad_r128(int32_t r, uint64_t shift);
void pslld_r128(int32_t r, uint64_t shift);
void psrlq_r128(int32_t r, uint64_t shift);
void psllq_r128(int32_t r, uint64_t shift);

/* sse_instr.rs:180-222 -- the comparison and min/max helpers */
bool sse_comparison(int32_t op, double x, double y);
double sse_min(double x, double y);
double sse_max(double x, double y);

/* sse_instr.rs:224-282 -- the conversions and their shared rounding helper */
int32_t sse_convert_with_truncation_f32_to_i32(float x);
int32_t sse_convert_f32_to_i32(float x);
int32_t sse_convert_with_truncation_f64_to_i32(double x);
int32_t sse_convert_f64_to_i32(double x);
double sse_integer_round(double f);

#endif /* V86C_SSE_INSTR_H */
