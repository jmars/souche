/*
 * fpu.h -- the x87 front-end: the register stack, the tag/status/control words, the
 * environment save/restore, the conversions, and FXSAVE/FXRSTOR.
 *
 * Port of v86/src/rust/cpu/fpu.rs (913 lines), plus the two FPU helpers that live in
 * v86/src/rust/cpu/misc_instr.rs:369-428 (`fxsave`/`fxrstor`) -- M1a-2 deliberately did
 * not port them (results/m1a2-arith-modrm-misc.md, the NOT-PORTED list) because they are
 * defined entirely in terms of this file's `fpu_load_m80`/`fpu_store_m80`/
 * `fpu_load_status_word`/`fpu_set_status_word`/`set_control_word`, and the three read-only
 * accessors were seated in exec_boundary.c with the note "M1b must DELETE all three from
 * here when fpu.c lands" -- done: fpu_load_status_word, fpu_load_tag_word and
 * fpu_get_sti_f64 now live here, and fpu.c is their only definition.
 *
 * THE CROSS-UNIT CONTRACT (results/m1b3-fpu.md): instructions_0f.c (M1b-1) calls
 *     void fpu_set_tag_word(uint16_t tag_word);
 *     void fxsave(int64_t addr);
 *     void fxrstor(int64_t addr);
 * with exactly these signatures.  The two FXSAVE forms are 0F AE /0 and /1.
 *
 * FAULT CONVENTION: as everywhere in the port, a fallible helper SETS `cpu->pagefault` and
 * returns a SENTINEL (F80_ZERO for an f80_t, 0 for an integer) that the caller must not
 * use; the generated dispatcher and instructions.c check the flag immediately.  Where
 * upstream writes `.unwrap()` it is because the CALLER already range-checked the buffer
 * (`readable_or_pagefault`/`writable_or_pagefault`), so the helper's own reads cannot
 * fault; the port keeps that shape (call, use the value) and names the pre-check in a
 * comment at the site rather than re-checking (upstream would panic, a release C build
 * would push a sentinel -- and neither can be reached through a checked caller).
 *
 * NOT IN THIS FILE, deliberately: `fwait` (fpu.rs:803).  Upstream's body is empty and the
 * seat that already provides it (exec_boundary.c) is bit-identical to it; moving it would
 * mean editing cpu_selftest.c's 9B recording stub, a file another unit owns concurrently
 * this pass.  Recorded as a handoff item in the unit report, not silently skipped.
 */

#ifndef V86C_FPU_H
#define V86C_FPU_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h"
#include "softfloat.h"

/* the status-word bits (fpu.rs:8-23) */
#define FPU_C0            0x100
#define FPU_C1            0x200
#define FPU_C2            0x400
#define FPU_C3            0x4000
#define FPU_RESULT_FLAGS  (FPU_C0 | FPU_C1 | FPU_C2 | FPU_C3)

#define FPU_EX_I   (1 << 0) /* invalid operation */
#define FPU_EX_D   (1 << 1) /* denormal operand */
#define FPU_EX_Z   (1 << 2) /* zero divide */
#define FPU_EX_O   (1 << 3) /* overflow */
#define FPU_EX_U   (1 << 4) /* underflow */
#define FPU_EX_P   (1 << 5) /* precision */
#define FPU_EX_SF  (1 << 6)

/* ---- the stack (fpu.rs:25-85) ---- */
void fpu_write_st(int32_t index, f80_t value);
f80_t fpu_get_st0(void);
void fpu_stack_fault(void);
void fpu_zero_fault(void);
void fpu_underflow_fault(void);
bool fpu_sti_empty(int32_t i);
f80_t fpu_get_sti(int32_t i);
double fpu_get_sti_f64(int32_t i); /* fpu.rs:82-85 -- the M0 transcript's x87 data reader */

/* ---- conversions (fpu.rs:88-158) ---- */
f80_t f32_to_f80(int32_t v);
f80_t f64_to_f80(uint64_t v);
int32_t f80_to_f32(f80_t x);
uint64_t f80_to_f64(f80_t x);
f80_t i32_to_f80(int32_t v);
f80_t i64_to_f80(int64_t v);

f80_t fpu_load_i16(int64_t addr);
f80_t fpu_load_i32(int64_t addr);
f80_t fpu_load_i64(int64_t addr);
f80_t fpu_load_m32(int64_t addr);
f80_t fpu_load_m64(int64_t addr);
f80_t fpu_load_m80(int64_t addr);

/* ---- words (fpu.rs:160-164, 444-454, 582-597) ---- */
uint16_t fpu_load_status_word(void);
void fpu_set_status_word(uint16_t sw);
void fpu_set_tag_word(uint16_t tag_word);
int32_t fpu_load_tag_word(void);
void set_control_word(uint16_t cw);

/* ---- arithmetic (fpu.rs:166-250, 463-466, 667-675) ---- */
void fpu_fadd(int32_t target_index, f80_t val);
void fpu_fsub(int32_t target_index, f80_t val);
void fpu_fsubr(int32_t target_index, f80_t val);
void fpu_fmul(int32_t target_index, f80_t val);
void fpu_fdiv(int32_t target_index, f80_t val);
void fpu_fdivr(int32_t target_index, f80_t val);

/* ---- compare / move (fpu.rs:173-252, 599-642, 693-770) ---- */
void fpu_fclex(void);
void fpu_fcmovcc(bool condition, int32_t r);
void fpu_fcom(f80_t y);
void fpu_fcomi(int32_t r);
void fpu_fcomip(int32_t r);
void fpu_fcomp(f80_t val);
void fpu_fucom(int32_t r);
void fpu_fucomi(int32_t r);
void fpu_fucomip(int32_t r);
void fpu_fucomp(int32_t r);
void fpu_fucompp(void);
void fpu_pop(void);
void fpu_push(f80_t x);
void fpu_ffree(int32_t r);
void fpu_fxch(int32_t i);
void fpu_fst(int32_t r);
void fpu_fstp(int32_t r);

/* ---- load / store (fpu.rs:254-256, 324-415, 456-460, 606-637) ---- */
void fpu_fildm16(int64_t addr);
void fpu_fildm32(int64_t addr);
void fpu_fildm64(int64_t addr);
void fpu_fistm16(int64_t addr);
void fpu_fistm16p(int64_t addr);
void fpu_fisttpm16(int64_t addr);
void fpu_fistm32(int64_t addr);
void fpu_fistm32p(int64_t addr);
void fpu_fisttpm32(int64_t addr);
void fpu_fistm64p(int64_t addr);
void fpu_fisttpm64(int64_t addr);
void fpu_fldm32(int64_t addr);
void fpu_fldm64(int64_t addr);
void fpu_fldm80(int64_t addr);
void fpu_fldm80_without_fault(int64_t addr);
void fpu_fst80p(int64_t addr);
void fpu_fstm32(int64_t addr);
void fpu_fstm32p(int64_t addr);
void fpu_fstm64(int64_t addr);
void fpu_fstm64p(int64_t addr);
void fpu_store_m32(int64_t addr, f80_t x);
void fpu_store_m64(int64_t addr, f80_t x);
void fpu_store_m80(int64_t addr, f80_t x);
void fpu_fbstp(int64_t addr);

/* ---- conversions to integer (fpu.rs:313-410) ---- */
int16_t fpu_convert_to_i16(f80_t f);
int32_t fpu_convert_to_i32(f80_t f);
int64_t fpu_convert_to_i64(f80_t f);
int16_t fpu_truncate_to_i16(f80_t f);
int32_t fpu_truncate_to_i32(f80_t f);
int64_t fpu_truncate_to_i64(f80_t f);
void fpu_invalid_arithmetic(void);

/* ---- control/env (fpu.rs:272-309, 412-439, 521-580) ---- */
void fpu_finit(void);
void fpu_fldcw(int64_t addr);
void fpu_fldenv16(int64_t addr);
void fpu_fldenv32(int64_t addr);
void fpu_fstenv16(int64_t addr);
void fpu_fstenv32(int64_t addr);
void fpu_fsave16(int64_t addr);
void fpu_fsave32(int64_t addr);
void fpu_frstor16(int64_t addr);
void fpu_frstor32(int64_t addr);
void fpu_fstcw(int64_t addr);
void fpu_fnstsw_mem(int64_t addr);
void fpu_fnstsw_reg(void);
void fpu_unimpl(void);

/* ---- transcendentals (fpu.rs:677-912) ---- */
void fpu_fprem(bool ieee);
void fpu_ftst(void);
void fpu_fxam(void);
void fpu_fyl2x(void);
void fpu_fyl2xp1(void);
void fpu_fxtract(void);
void fpu_fchs(void);
void fpu_fabs(void);
void fpu_f2xm1(void);
void fpu_fptan(void);
void fpu_fpatan(void);
void fpu_fsqrt(void);
void fpu_fsincos(void);
void fpu_frndint(void);
void fpu_fscale(void);
void fpu_fsin(void);
void fpu_fcos(void);
void fpu_fdecstp(void);
void fpu_fincstp(void);

/* ---- misc_instr.rs:369-428, deferred by M1a-2 to this unit ---- */
void fxsave(int64_t addr);
void fxrstor(int64_t addr);

#endif /* V86C_FPU_H */
