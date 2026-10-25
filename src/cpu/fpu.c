/*
 * fpu.c -- the x87 implementation: port of v86/src/rust/cpu/fpu.rs (913 lines) plus
 * misc_instr.rs:369-428 (fxsave/fxrstor).  See fpu.h for the scope, the cross-unit
 * contract and the fault convention.
 *
 * The port is a transcription with three mechanical rules, each of which is a place a
 * slip would be silent -- they are called out at the sites too:
 *
 *   1. `a + i & 7` in Rust is `(a + i) & 7` (`as`/`+` bind tighter than `&`).  Every
 *      stack-index computation is written with the parentheses explicit.
 *   2. Rust's `!x` on an integer is the BITWISE complement; the mask stores below are
 *      `& (uint16_t)~BIT`, never C's `!` (the M1a-2 report's not8 bug is the precedent).
 *   3. A Rust match on `Option<Ordering>` becomes a switch on f80_partial_cmp[_quiet]'s
 *      F80_CMP_* value (softfloat.h), with the GT arm deliberately empty exactly where
 *      upstream's `Some(Greater) => {}` is.
 *
 * `dbg_log!`/`dbg_trace!` are dropped (M1a-2's D19: no log channel), and upstream's
 * `dbg_assert!(false)` in fpu_unimpl is dropped rather than made a C `assert`: it is a
 * `debug_assert!` (dbg.rs:63), compiled out of the release reference, so the reference
 * DELIVERS #UD there and a live C assert would diverge from it.
 */

#include "fpu.h"

#include <string.h>

#include "misc_instr.h" /* set_mxcsr, MXCSR_MASK */
#include "paging.h"

/* ------------------------------------------------------------------ *
 * the register stack (fpu.rs:25-85)
 * ------------------------------------------------------------------ */

void fpu_write_st(int32_t index, f80_t value)
{
    DBG_ASSERT(index >= 0 && index < 8);
    cpu->fpu_st[index] = value; /* the PHYSICAL slot, not the logical index */
}

void fpu_stack_fault(void)
{
    /* TODO: Interrupt (upstream's comment) */
    cpu->fpu_status_word |= FPU_EX_SF | FPU_EX_I;
}

void fpu_zero_fault(void)
{
    cpu->fpu_status_word |= FPU_EX_Z;
}

void fpu_underflow_fault(void)
{
    cpu->fpu_status_word |= FPU_EX_U;
}

f80_t fpu_get_st0(void)
{
    DBG_ASSERT(cpu->fpu_stack_ptr < 8);
    if (0 != (cpu->fpu_stack_empty >> cpu->fpu_stack_ptr & 1)) {
        cpu->fpu_status_word &= (uint16_t)~FPU_C1;
        fpu_stack_fault();
        return F80_INDEFINITE_NAN;
    }
    return cpu->fpu_st[cpu->fpu_stack_ptr];
}

bool fpu_sti_empty(int32_t i)
{
    DBG_ASSERT(i >= 0 && i < 8);
    i = (i + cpu->fpu_stack_ptr) & 7;
    return 0 != (cpu->fpu_stack_empty >> i & 1);
}

f80_t fpu_get_sti(int32_t i)
{
    DBG_ASSERT(i >= 0 && i < 8);
    i = (i + cpu->fpu_stack_ptr) & 7;
    if (0 != (cpu->fpu_stack_empty >> i & 1)) {
        cpu->fpu_status_word &= (uint16_t)~FPU_C1;
        fpu_stack_fault();
        return F80_INDEFINITE_NAN;
    }
    return cpu->fpu_st[i];
}

/* fpu.rs:82-85.  The M0 transcript's and the nasm fixture's x87 data reader: the logical
 * st(i) converted to f64.  Upstream calls it "only used for debugging"; here it is also
 * the value the oracle compares, so it is exact for every F80, not just zero. */
double fpu_get_sti_f64(int32_t i)
{
    uint64_t bits;
    double value;
    i = (i + cpu->fpu_stack_ptr) & 7;
    bits = f80_convert_to_f64(cpu->fpu_st[i]); /* the METHOD (fpu.rs:84), not fpu.rs:111's wrapper */
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* ------------------------------------------------------------------ *
 * conversions (fpu.rs:88-158)
 * ------------------------------------------------------------------ */

f80_t f32_to_f80(int32_t v)
{
    f80_t x;
    f80_clear_exception_flags();
    x = f80_of_f32((uint32_t)v);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

f80_t f64_to_f80(uint64_t v)
{
    f80_t x;
    f80_clear_exception_flags();
    x = f80_of_f64(v);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

int32_t f80_to_f32(f80_t v)
{
    int32_t x;
    f80_clear_exception_flags();
    x = (int32_t)f80_convert_to_f32(v);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

uint64_t f80_to_f64(f80_t v)
{
    uint64_t x;
    f80_clear_exception_flags();
    x = f80_convert_to_f64(v);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

f80_t i32_to_f80(int32_t v) { return f80_of_i32(v); }
f80_t i64_to_f80(int64_t v) { return f80_of_i64(v); }

f80_t fpu_load_i16(int64_t addr)
{
    int32_t v = safe_read16(addr);
    if (cpu->pagefault) return F80_ZERO;
    return f80_of_i32((int16_t)v); /* safe_read16 zero-extends; upstream re-signs it */
}

f80_t fpu_load_i32(int64_t addr)
{
    int32_t v = safe_read32s(addr);
    if (cpu->pagefault) return F80_ZERO;
    return f80_of_i32(v);
}

f80_t fpu_load_i64(int64_t addr)
{
    uint64_t v = safe_read64s(addr);
    if (cpu->pagefault) return F80_ZERO;
    return f80_of_i64((int64_t)v);
}

f80_t fpu_load_m32(int64_t addr)
{
    f80_t x;
    int32_t raw;
    f80_clear_exception_flags();
    raw = safe_read32s(addr);
    if (cpu->pagefault) return F80_ZERO;
    x = f80_of_f32((uint32_t)raw);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

f80_t fpu_load_m64(int64_t addr)
{
    f80_t x;
    uint64_t raw;
    f80_clear_exception_flags();
    raw = safe_read64s(addr);
    if (cpu->pagefault) return F80_ZERO;
    x = f80_of_f64(raw);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

f80_t fpu_load_m80(int64_t addr)
{
    f80_t x;
    uint64_t mantissa = safe_read64s(addr);
    int32_t sign_exponent;
    if (cpu->pagefault) return F80_ZERO;
    sign_exponent = safe_read16(addr + 8);
    if (cpu->pagefault) return F80_ZERO;
    /* TODO: Canonical form (upstream's comment -- the 80-bit image is taken verbatim) */
    x.mantissa = mantissa;
    x.sign_exponent = (uint16_t)sign_exponent;
    return x;
}

/* ------------------------------------------------------------------ *
 * words (fpu.rs:160-164, 444-454, 582-597)
 * ------------------------------------------------------------------ */

uint16_t fpu_load_status_word(void)
{
    DBG_ASSERT(cpu->fpu_stack_ptr < 8);
    return (uint16_t)((cpu->fpu_status_word & ~(uint16_t)(7 << 11)) |
                      ((uint16_t)cpu->fpu_stack_ptr << 11));
}

void fpu_set_status_word(uint16_t sw)
{
    cpu->fpu_status_word = (uint16_t)(sw & ~(uint16_t)(7 << 11));
    cpu->fpu_stack_ptr = (uint8_t)(sw >> 11 & 7);
}

/* fpu.rs:444-450.  The signature is the M1b cross-unit contract's (uint16_t, not the
 * Rust's i32): every caller reads a 16-bit word. */
void fpu_set_tag_word(uint16_t tag_word)
{
    int i;
    cpu->fpu_stack_empty = 0;
    for (i = 0; i < 8; i++) {
        bool empty = ((tag_word >> (2 * i)) & 3) == 3;
        cpu->fpu_stack_empty |= (uint8_t)(empty << i);
    }
}

int32_t fpu_load_tag_word(void)
{
    int32_t tag_word = 0;
    int i;
    for (i = 0; i < 8; i++) {
        f80_t value = cpu->fpu_st[i];
        if (0 != (cpu->fpu_stack_empty >> i & 1)) {
            tag_word |= 3 << (i << 1); /* empty */
        }
        else if (f80_eq(value, F80_ZERO)) {
            tag_word |= 1 << (i << 1); /* zero */
        }
        else if (!f80_is_finite(value)) {
            tag_word |= 2 << (i << 1); /* invalid: infinite or NaN */
        }
    }
    return tag_word;
}

/* fpu.rs:282-309 */
void set_control_word(uint16_t cw)
{
    int rc;
    int precision_control;

    cpu->fpu_control_word = cw;

    rc = cw >> 10 & 3;
    switch (rc) {
    case 0: f80_set_rounding_mode(F80_ROUND_NEAR_EVEN); break;
    case 1: f80_set_rounding_mode(F80_ROUND_MIN); break;    /* Floor */
    case 2: f80_set_rounding_mode(F80_ROUND_MAX); break;    /* Ceil */
    case 3: f80_set_rounding_mode(F80_ROUND_MINMAG); break; /* Trunc */
    default: DBG_ASSERT(false); f80_set_rounding_mode(F80_ROUND_NEAR_EVEN); break; /* unreachable */
    }

    precision_control = cw >> 8 & 3;
    switch (precision_control) {
    case 0: f80_set_precision(F80_PREC_P32); break;
    case 1: f80_set_precision(F80_PREC_P80); break; /* undefined */
    case 2: f80_set_precision(F80_PREC_P64); break;
    case 3: f80_set_precision(F80_PREC_P80); break;
    default: DBG_ASSERT(false); f80_set_precision(F80_PREC_P80); break; /* unreachable */
    }
}

/* ------------------------------------------------------------------ *
 * arithmetic (fpu.rs:166-250, 463-466, 667-675)
 * ------------------------------------------------------------------ */

void fpu_fadd(int32_t target_index, f80_t val)
{
    f80_t st0;
    f80_clear_exception_flags();
    st0 = fpu_get_st0();
    fpu_write_st((cpu->fpu_stack_ptr + target_index) & 7, f80_add(st0, val));
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

void fpu_fsub(int32_t target_index, f80_t val)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st((cpu->fpu_stack_ptr + target_index) & 7, f80_sub(st0, val));
}

void fpu_fsubr(int32_t target_index, f80_t val)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st((cpu->fpu_stack_ptr + target_index) & 7, f80_sub(val, st0));
}

void fpu_fmul(int32_t target_index, f80_t val)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st((cpu->fpu_stack_ptr + target_index) & 7, f80_mul(st0, val));
}

void fpu_fdiv(int32_t target_index, f80_t val)
{
    f80_t st0;
    f80_clear_exception_flags();
    st0 = fpu_get_st0();
    fpu_write_st((cpu->fpu_stack_ptr + target_index) & 7, f80_div(st0, val));
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

void fpu_fdivr(int32_t target_index, f80_t val)
{
    f80_t st0;
    f80_clear_exception_flags();
    st0 = fpu_get_st0();
    fpu_write_st((cpu->fpu_stack_ptr + target_index) & 7, f80_div(val, st0));
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

/* ------------------------------------------------------------------ *
 * compare / move (fpu.rs:173-252, 599-642, 693-770)
 * ------------------------------------------------------------------ */

void fpu_fclex(void) { cpu->fpu_status_word = 0; }

void fpu_fcmovcc(bool condition, int32_t r)
{
    /* outside of the condition is correct: a stack fault happens even if the condition is
     * not fulfilled (upstream's comment) */
    f80_t x = fpu_get_sti(r);
    if (fpu_sti_empty(r)) {
        fpu_write_st(cpu->fpu_stack_ptr, F80_INDEFINITE_NAN);
    }
    else if (condition) {
        fpu_write_st(cpu->fpu_stack_ptr, x);
        cpu->fpu_stack_empty &= (uint8_t)~(1 << cpu->fpu_stack_ptr);
    }
}

void fpu_fcom(f80_t y)
{
    f80_t x;
    int cmp;
    f80_clear_exception_flags();
    x = fpu_get_st0();
    cpu->fpu_status_word &= (uint16_t)~FPU_RESULT_FLAGS;
    cmp = f80_partial_cmp(x, y);
    if (cmp == F80_CMP_LT) {
        cpu->fpu_status_word |= FPU_C0;
    }
    else if (cmp == F80_CMP_EQ) {
        cpu->fpu_status_word |= FPU_C3;
    }
    else if (cmp == F80_CMP_UNORDERED) {
        cpu->fpu_status_word |= FPU_C0 | FPU_C2 | FPU_C3;
    }
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

void fpu_fcomi(int32_t r)
{
    f80_t x;
    f80_t y;
    int cmp;
    f80_clear_exception_flags();
    x = fpu_get_st0();
    y = fpu_get_sti(r);
    cpu->flags_changed = 0;
    cpu->flags &= ~FLAGS_ALL;
    cmp = f80_partial_cmp(x, y);
    if (cmp == F80_CMP_LT) {
        cpu->flags |= FLAG_CARRY;
    }
    else if (cmp == F80_CMP_EQ) {
        cpu->flags |= FLAG_ZERO;
    }
    else if (cmp == F80_CMP_UNORDERED) {
        cpu->flags |= FLAG_CARRY | FLAG_PARITY | FLAG_ZERO;
    }
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

void fpu_fcomip(int32_t r)
{
    fpu_fcomi(r);
    fpu_pop();
}

void fpu_pop(void)
{
    DBG_ASSERT(cpu->fpu_stack_ptr < 8);
    cpu->fpu_stack_empty |= (uint8_t)(1 << cpu->fpu_stack_ptr);
    cpu->fpu_stack_ptr = (cpu->fpu_stack_ptr + 1) & 7;
}

void fpu_fcomp(f80_t val)
{
    fpu_fcom(val);
    fpu_pop();
}

void fpu_push(f80_t x)
{
    cpu->fpu_stack_ptr = (cpu->fpu_stack_ptr - 1) & 7;
    if (0 != (cpu->fpu_stack_empty >> cpu->fpu_stack_ptr & 1)) {
        cpu->fpu_status_word &= (uint16_t)~FPU_C1;
        cpu->fpu_stack_empty &= (uint8_t)~(1 << cpu->fpu_stack_ptr);
        fpu_write_st(cpu->fpu_stack_ptr, x);
    }
    else {
        cpu->fpu_status_word |= FPU_C1;
        fpu_stack_fault();
        fpu_write_st(cpu->fpu_stack_ptr, F80_INDEFINITE_NAN);
    }
}

void fpu_ffree(int32_t r)
{
    cpu->fpu_stack_empty |= (uint8_t)(1 << ((cpu->fpu_stack_ptr + r) & 7));
}

void fpu_fxch(int32_t i)
{
    f80_t sti = fpu_get_sti(i);
    fpu_write_st((cpu->fpu_stack_ptr + i) & 7, fpu_get_st0());
    fpu_write_st(cpu->fpu_stack_ptr, sti);
}

void fpu_fst(int32_t r)
{
    int32_t index = (cpu->fpu_stack_ptr + r) & 7;
    fpu_write_st(index, fpu_get_st0());
    if (0 == ((cpu->fpu_stack_empty >> cpu->fpu_stack_ptr) & 1)) {
        cpu->fpu_stack_empty &= (uint8_t)~(1 << index);
    }
}

void fpu_fstp(int32_t r)
{
    fpu_fst(r);
    fpu_pop();
}

void fpu_fucom(int32_t r)
{
    f80_t x;
    f80_t y;
    int cmp;
    f80_clear_exception_flags();
    x = fpu_get_st0();
    y = fpu_get_sti(r);
    cpu->fpu_status_word &= (uint16_t)~FPU_RESULT_FLAGS;
    cmp = f80_partial_cmp_quiet(x, y);
    if (cmp == F80_CMP_LT) {
        cpu->fpu_status_word |= FPU_C0;
    }
    else if (cmp == F80_CMP_EQ) {
        cpu->fpu_status_word |= FPU_C3;
    }
    else if (cmp == F80_CMP_UNORDERED) {
        cpu->fpu_status_word |= FPU_C0 | FPU_C2 | FPU_C3;
    }
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

void fpu_fucomi(int32_t r)
{
    f80_t x;
    f80_t y;
    int cmp;
    f80_clear_exception_flags();
    x = fpu_get_st0();
    y = fpu_get_sti(r);
    cpu->flags_changed = 0;
    cpu->flags &= ~FLAGS_ALL;
    cmp = f80_partial_cmp_quiet(x, y);
    if (cmp == F80_CMP_LT) {
        cpu->flags |= FLAG_CARRY;
    }
    else if (cmp == F80_CMP_EQ) {
        cpu->flags |= FLAG_ZERO;
    }
    else if (cmp == F80_CMP_UNORDERED) {
        cpu->flags |= FLAG_CARRY | FLAG_PARITY | FLAG_ZERO;
    }
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

void fpu_fucomip(int32_t r)
{
    fpu_fucomi(r);
    fpu_pop();
}

void fpu_fucomp(int32_t r)
{
    fpu_fucom(r);
    fpu_pop();
}

void fpu_fucompp(void)
{
    fpu_fucom(1);
    fpu_pop();
    fpu_pop();
}

/* ------------------------------------------------------------------ *
 * load / store (fpu.rs:254-256, 324-415, 456-460, 552-637)
 * ------------------------------------------------------------------ */

void fpu_fildm16(int64_t addr)
{
    f80_t x = fpu_load_i16(addr);
    if (cpu->pagefault) return;
    fpu_push(x);
}

void fpu_fildm32(int64_t addr)
{
    f80_t x = fpu_load_i32(addr);
    if (cpu->pagefault) return;
    fpu_push(x);
}

void fpu_fildm64(int64_t addr)
{
    f80_t x = fpu_load_i64(addr);
    if (cpu->pagefault) return;
    fpu_push(x);
}

void fpu_finit(void)
{
    set_control_word(0x37F);
    cpu->fpu_status_word = 0;
    cpu->fpu_ip = 0;
    cpu->fpu_dp = 0;
    cpu->fpu_opcode = 0;
    cpu->fpu_stack_empty = 0xFF;
    cpu->fpu_stack_ptr = 0;
}

void fpu_invalid_arithmetic(void) { cpu->fpu_status_word |= FPU_EX_I; }

int32_t fpu_convert_to_i32(f80_t f)
{
    int32_t x;
    f80_clear_exception_flags();
    x = f80_to_i32(f);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

int64_t fpu_convert_to_i64(f80_t f)
{
    int64_t x;
    f80_clear_exception_flags();
    x = f80_to_i64(f);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

int16_t fpu_convert_to_i16(f80_t f)
{
    int32_t st0 = fpu_convert_to_i32(f);
    if (st0 < -0x8000 || st0 > 0x7FFF) {
        fpu_invalid_arithmetic();
        return -0x8000;
    }
    return (int16_t)st0;
}

int32_t fpu_truncate_to_i32(f80_t f)
{
    int32_t x;
    f80_clear_exception_flags();
    x = f80_truncate_to_i32(f);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

int64_t fpu_truncate_to_i64(f80_t f)
{
    int64_t x;
    f80_clear_exception_flags();
    x = f80_truncate_to_i64(f);
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
    return x;
}

int16_t fpu_truncate_to_i16(f80_t f)
{
    int32_t st0 = fpu_truncate_to_i32(f);
    if (st0 < -0x8000 || st0 > 0x7FFF) {
        fpu_invalid_arithmetic();
        return -0x8000;
    }
    return (int16_t)st0;
}

void fpu_fistm16(int64_t addr)
{
    int16_t v;
    writable_or_pagefault(addr, 2);
    if (cpu->pagefault) return;
    v = fpu_convert_to_i16(fpu_get_st0());
    safe_write16(addr, v & 0xFFFF);
}

void fpu_fistm16p(int64_t addr)
{
    fpu_fistm16(addr);
    if (cpu->pagefault) return;
    fpu_pop();
}

void fpu_fisttpm16(int64_t addr)
{
    int16_t v;
    writable_or_pagefault(addr, 2);
    if (cpu->pagefault) return;
    v = fpu_truncate_to_i16(fpu_get_st0());
    safe_write16(addr, v & 0xFFFF);
    fpu_pop();
}

void fpu_fistm32(int64_t addr)
{
    int32_t v;
    writable_or_pagefault(addr, 4);
    if (cpu->pagefault) return;
    v = fpu_convert_to_i32(fpu_get_st0());
    safe_write32(addr, v);
}

void fpu_fistm32p(int64_t addr)
{
    fpu_fistm32(addr);
    if (cpu->pagefault) return;
    fpu_pop();
}

void fpu_fisttpm32(int64_t addr)
{
    int32_t v;
    writable_or_pagefault(addr, 4);
    if (cpu->pagefault) return;
    v = fpu_truncate_to_i32(fpu_get_st0());
    safe_write32(addr, v);
    fpu_pop();
}

void fpu_fistm64p(int64_t addr)
{
    int64_t v;
    writable_or_pagefault(addr, 8);
    if (cpu->pagefault) return;
    v = fpu_convert_to_i64(fpu_get_st0());
    safe_write64(addr, (uint64_t)v);
    fpu_pop();
}

void fpu_fisttpm64(int64_t addr)
{
    int64_t v;
    writable_or_pagefault(addr, 8);
    if (cpu->pagefault) return;
    v = fpu_truncate_to_i64(fpu_get_st0());
    safe_write64(addr, (uint64_t)v);
    fpu_pop();
}

void fpu_fldm32(int64_t addr)
{
    f80_t x = fpu_load_m32(addr);
    if (cpu->pagefault) return;
    fpu_push(x);
}

void fpu_fldm64(int64_t addr)
{
    f80_t x = fpu_load_m64(addr);
    if (cpu->pagefault) return;
    fpu_push(x);
}

void fpu_fldm80(int64_t addr)
{
    f80_t x = fpu_load_m80(addr);
    if (cpu->pagefault) return;
    fpu_push(x);
}

void fpu_fldm80_without_fault(int64_t addr)
{
    /* Named "without_fault" because the CALLER checked (instructions.rs's DB_5_mem does
     * `readable_or_pagefault(addr, 10)` first); upstream still `.unwrap()`s, so a fault
     * here is an upstream panic -- the assert below is the port saying the same thing. */
    f80_t x = fpu_load_m80(addr);
    DBG_ASSERT(!cpu->pagefault);
    fpu_push(x);
}

void fpu_store_m32(int64_t addr, f80_t x)
{
    f80_clear_exception_flags();
    safe_write32(addr, (int32_t)f80_convert_to_f32(x)); /* the METHOD: x.to_f32() */
    if (cpu->pagefault) return;
    cpu->fpu_status_word |= (uint16_t)f80_get_exception_flags();
}

void fpu_store_m64(int64_t addr, f80_t x)
{
    safe_write64(addr, f80_convert_to_f64(x)); /* the METHOD: x.to_f64() */
}

void fpu_store_m80(int64_t addr, f80_t f)
{
    /* writable_or_pagefault must have been called by the caller! (upstream's comment) */
    safe_write64(addr, f.mantissa);
    safe_write16(addr + 8, f.sign_exponent);
}

void fpu_fstm32(int64_t addr)
{
    fpu_store_m32(addr, fpu_get_st0());
}

void fpu_fstm32p(int64_t addr)
{
    fpu_store_m32(addr, fpu_get_st0());
    if (cpu->pagefault) return;
    fpu_pop();
}

void fpu_fstm64(int64_t addr)
{
    fpu_store_m64(addr, fpu_get_st0());
}

void fpu_fstm64p(int64_t addr)
{
    /* XXX: writable_or_pagefault before get_st0 (upstream's comment) */
    fpu_store_m64(addr, fpu_get_st0());
    if (cpu->pagefault) return;
    fpu_pop();
}

void fpu_fst80p(int64_t addr)
{
    writable_or_pagefault(addr, 10);
    if (cpu->pagefault) return;
    fpu_store_m80(addr, fpu_get_st0());
    fpu_pop();
}

void fpu_fbstp(int64_t addr)
{
    f80_t st0 = fpu_get_st0();
    int64_t signed_value = f80_to_i64(st0);
    /* i64::unsigned_abs, written to avoid C's UB on -INT64_MIN */
    uint64_t x = (signed_value < 0) ? ((uint64_t)(-(signed_value + 1)) + 1) : (uint64_t)signed_value;

    if (x <= 9999999999999999999ull) {
        int i;
        for (i = 0; i <= 8; i++) {
            uint64_t low = x % 10;
            x /= 10;
            uint64_t high = x % 10;
            x /= 10;
            safe_write8(addr + i, (int32_t)(high << 4 | low));
        }
        safe_write8(addr + 9, f80_sign(st0) ? 0x80 : 0);
    }
    else {
        fpu_invalid_arithmetic();
        safe_write64(addr + 0, 0xC000000000000000ull);
        safe_write16(addr + 8, 0xFFFF);
    }
    fpu_pop();
}

/* ------------------------------------------------------------------ *
 * control / environment (fpu.rs:272-309, 412-439, 521-580, 612-613)
 * ------------------------------------------------------------------ */

void fpu_fldcw(int64_t addr)
{
    int32_t word = safe_read16(addr);
    if (cpu->pagefault) return;
    set_control_word((uint16_t)word);
}

/* fpu.rs:417-427.  The PROTECTED-MODE layout; the whole 14-byte block was range-checked
 * by the caller (instructions.rs's D9_4_mem), which is why upstream unwraps. */
void fpu_fldenv16(int64_t addr)
{
    set_control_word((uint16_t)safe_read16(addr));
    fpu_set_status_word((uint16_t)safe_read16(addr + 2));
    fpu_set_tag_word((uint16_t)safe_read16(addr + 4));
    cpu->fpu_ip = safe_read16(addr + 6);
    cpu->fpu_ip_selector = safe_read16(addr + 8);
    cpu->fpu_dp = safe_read16(addr + 10);
    cpu->fpu_dp_selector = safe_read16(addr + 12);
}

void fpu_fldenv32(int64_t addr)
{
    set_control_word((uint16_t)safe_read16(addr));
    fpu_set_status_word((uint16_t)safe_read16(addr + 4));
    fpu_set_tag_word((uint16_t)safe_read16(addr + 8));
    cpu->fpu_ip = safe_read32s(addr + 12);
    cpu->fpu_ip_selector = safe_read16(addr + 16);
    cpu->fpu_opcode = safe_read16(addr + 18);
    cpu->fpu_dp = safe_read32s(addr + 20);
    cpu->fpu_dp_selector = safe_read16(addr + 24);
}

void fpu_fstenv16(int64_t addr)
{
    safe_write16(addr + 0, cpu->fpu_control_word);
    safe_write16(addr + 2, fpu_load_status_word());
    safe_write16(addr + 4, fpu_load_tag_word());
    safe_write16(addr + 6, cpu->fpu_ip & 0xFFFF);
    safe_write16(addr + 8, cpu->fpu_ip_selector);
    safe_write16(addr + 10, cpu->fpu_dp & 0xFFFF);
    safe_write16(addr + 12, cpu->fpu_dp_selector);
}

void fpu_fstenv32(int64_t addr)
{
    /* upstream adds the high half into the control/status/tag words and ORs it into the
     * data-pointer selector -- reproduced verbatim, including the add/or asymmetry */
    int32_t high_bits = (int32_t)0xFFFF0000u;
    safe_write32(addr + 0, high_bits + (int32_t)cpu->fpu_control_word);
    safe_write32(addr + 4, high_bits + (int32_t)fpu_load_status_word());
    safe_write32(addr + 8, high_bits + fpu_load_tag_word());
    safe_write32(addr + 12, cpu->fpu_ip);
    safe_write16(addr + 16, cpu->fpu_ip_selector);
    safe_write16(addr + 18, cpu->fpu_opcode);
    safe_write32(addr + 20, cpu->fpu_dp);
    safe_write32(addr + 24, high_bits | cpu->fpu_dp_selector);
}

void fpu_fstcw(int64_t addr)
{
    safe_write16(addr, cpu->fpu_control_word);
}

void fpu_fnstsw_mem(int64_t addr)
{
    safe_write16(addr, fpu_load_status_word());
}

void fpu_fnstsw_reg(void)
{
    write_reg16(AX, fpu_load_status_word());
}

void fpu_fsave16(int64_t addr)
{
    (void)addr; /* upstream logs "fsave16" (dbg_log, dropped) then treats it as unimpl */
    fpu_unimpl();
}

void fpu_fsave32(int64_t addr)
{
    int i;
    writable_or_pagefault(addr, 108);
    if (cpu->pagefault) return;
    fpu_fstenv32(addr);
    addr += 28;
    for (i = 0; i < 8; i++) {
        int32_t reg_index = (i + cpu->fpu_stack_ptr) & 7;
        fpu_store_m80(addr, cpu->fpu_st[reg_index]);
        addr += 10;
    }
    fpu_finit();
}

void fpu_frstor16(int64_t addr)
{
    (void)addr; /* upstream logs "frstor16" (dbg_log, dropped) then treats it as unimpl */
    fpu_unimpl();
}

void fpu_frstor32(int64_t addr)
{
    int i;
    readable_or_pagefault(addr, 28 + 8 * 10);
    if (cpu->pagefault) return;
    fpu_fldenv32(addr);
    addr += 28;
    for (i = 0; i < 8; i++) {
        int32_t reg_index = (cpu->fpu_stack_ptr + i) & 7;
        cpu->fpu_st[reg_index] = fpu_load_m80(addr); /* range pre-checked above */
        addr += 10;
    }
}

void fpu_unimpl(void)
{
    /* upstream is `dbg_assert!(false); trigger_ud();` -- the assert is a debug_assert,
     * compiled out of the release reference, so only the #UD is live behaviour */
    trigger_ud();
}

/* ------------------------------------------------------------------ *
 * transcendentals (fpu.rs:677-912)
 * ------------------------------------------------------------------ */

void fpu_fprem(bool ieee)
{
    /* false: Faster, fails nasmtests / true: Slower, fails qemutests (upstream's note);
     * intel_compatibility is false, so the second arm is DEAD in the reference too --
     * kept because deleting upstream's dead arm hides that it exists. */
    const bool intel_compatibility = false;

    f80_t st0 = fpu_get_st0();
    f80_t st1 = fpu_get_sti(1);
    f80_t d;
    f80_t fprem_quotient;

    if (f80_eq(st1, F80_ZERO)) {
        if (f80_eq(st0, F80_ZERO)) {
            fpu_invalid_arithmetic();
        }
        else {
            fpu_zero_fault();
        }
        fpu_write_st(cpu->fpu_stack_ptr, F80_INDEFINITE_NAN);
        return;
    }

    d = f80_abs(f80_sub(f80_log2(st0), f80_log2(st1)));
    if (!intel_compatibility || f80_lt(d, f80_of_f64x(64.0))) {
        int32_t q;
        fprem_quotient = f80_div(st0, st1);
        fprem_quotient = ieee ? f80_round(fprem_quotient) : f80_trunc(fprem_quotient);
        fpu_write_st(cpu->fpu_stack_ptr, f80_sub(st0, f80_mul(st1, fprem_quotient)));
        q = f80_to_i32(fprem_quotient);
        cpu->fpu_status_word &= (uint16_t)~(FPU_C0 | FPU_C1 | FPU_C3);
        if (0 != (q & 1)) {
            cpu->fpu_status_word |= FPU_C1;
        }
        if (0 != (q & 1 << 1)) {
            cpu->fpu_status_word |= FPU_C3;
        }
        if (0 != (q & 1 << 2)) {
            cpu->fpu_status_word |= FPU_C0;
        }
        cpu->fpu_status_word &= (uint16_t)~FPU_C2;
    }
    else {
        f80_t n = f80_of_f64x(32.0);
        f80_t scale = f80_two_pow(f80_sub(d, n));
        fprem_quotient = f80_div(st0, st1);
        fprem_quotient = ieee ? f80_round(fprem_quotient) : f80_trunc(fprem_quotient);
        fprem_quotient = f80_div(fprem_quotient, scale);
        fpu_write_st(cpu->fpu_stack_ptr,
                     f80_sub(st0, f80_mul(f80_mul(st1, fprem_quotient), scale)));
        cpu->fpu_status_word |= FPU_C2;
    }
}

void fpu_ftst(void)
{
    f80_t x = fpu_get_st0();
    cpu->fpu_status_word &= (uint16_t)~FPU_RESULT_FLAGS;
    if (f80_is_nan(x)) {
        cpu->fpu_status_word |= FPU_C3 | FPU_C2 | FPU_C0;
    }
    else if (f80_eq(x, F80_ZERO)) {
        cpu->fpu_status_word |= FPU_C3;
    }
    else if (f80_lt(x, F80_ZERO)) {
        cpu->fpu_status_word |= FPU_C0;
    }
    /* TODO: unordered (x is nan, etc) -- upstream's comment */
}

void fpu_fxam(void)
{
    f80_t x = fpu_get_st0();
    cpu->fpu_status_word &= (uint16_t)~FPU_RESULT_FLAGS;
    cpu->fpu_status_word |= (uint16_t)(f80_sign(x) << 9); /* C1 = the sign */
    if (0 != (cpu->fpu_stack_empty >> cpu->fpu_stack_ptr & 1)) {
        cpu->fpu_status_word |= FPU_C3 | FPU_C0;
    }
    else if (f80_is_nan(x)) {
        cpu->fpu_status_word |= FPU_C0;
    }
    else if (f80_eq(x, F80_ZERO)) {
        cpu->fpu_status_word |= FPU_C3;
    }
    else if (!f80_is_finite(x)) {
        cpu->fpu_status_word |= FPU_C2 | FPU_C0;
    }
    else {
        cpu->fpu_status_word |= FPU_C2;
    }
    /* TODO: Unsupported, Denormal -- upstream's comment */
}

void fpu_fyl2x(void)
{
    f80_t st0 = fpu_get_st0();
    f80_t st1;
    if (f80_lt(st0, F80_ZERO)) {
        fpu_invalid_arithmetic();
    }
    else if (f80_eq(st0, F80_ZERO)) {
        fpu_zero_fault();
    }
    st1 = fpu_get_sti(1);
    fpu_write_st((cpu->fpu_stack_ptr + 1) & 7, f80_div(f80_mul(st1, f80_ln(st0)), F80_LN_2));
    fpu_pop();
}

void fpu_fxtract(void)
{
    f80_t st0 = fpu_get_st0();
    if (f80_eq(st0, F80_ZERO)) {
        fpu_zero_fault();
        fpu_write_st(cpu->fpu_stack_ptr, F80_NEG_INFINITY);
        fpu_push(st0);
    }
    else {
        int16_t exp = f80_exponent(st0);
        f80_t significand;
        fpu_write_st(cpu->fpu_stack_ptr, f80_of_i32(exp));
        significand.sign_exponent = 0x3FFF;
        significand.mantissa = st0.mantissa;
        fpu_push(significand);
    }
}

void fpu_fchs(void)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st(cpu->fpu_stack_ptr, f80_neg(st0));
}

void fpu_fabs(void)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st(cpu->fpu_stack_ptr, f80_abs(st0));
}

void fpu_f2xm1(void)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st(cpu->fpu_stack_ptr, f80_sub(f80_two_pow(st0), F80_ONE));
}

void fpu_fptan(void)
{
    f80_t st0 = fpu_get_st0();
    /* if -pow(2.0, 63.0) < st0 && st0 < pow(2.0, 63.0) -- upstream's disabled range check */
    fpu_write_st(cpu->fpu_stack_ptr, f80_tan(st0));
    /* no bug: push constant 1 (upstream's comment) */
    fpu_push(F80_ONE);
    cpu->fpu_status_word &= (uint16_t)~FPU_C2;
}

void fpu_fpatan(void)
{
    f80_t st0 = fpu_get_st0();
    f80_t st1 = fpu_get_sti(1);
    fpu_write_st((cpu->fpu_stack_ptr + 1) & 7, f80_atan2(st1, st0));
    fpu_pop();
}

void fpu_fyl2xp1(void)
{
    /* fyl2xp1: y * log2(x+1) and pop */
    f80_t st0 = fpu_get_st0();
    f80_t st1 = fpu_get_sti(1);
    f80_t y = f80_div(f80_mul(st1, f80_ln(f80_add(st0, F80_ONE))), F80_LN_2);
    fpu_write_st((cpu->fpu_stack_ptr + 1) & 7, y);
    fpu_pop();
}

void fpu_fsqrt(void)
{
    f80_t st0 = fpu_get_st0();
    /* if st0 < 0.0 { fpu_invalid_arithmetic(); } -- upstream's disabled check */
    fpu_write_st(cpu->fpu_stack_ptr, f80_sqrt(st0));
}

void fpu_fsincos(void)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st(cpu->fpu_stack_ptr, f80_sin(st0));
    fpu_push(f80_cos(st0));
    cpu->fpu_status_word &= (uint16_t)~FPU_C2;
}

void fpu_frndint(void)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st(cpu->fpu_stack_ptr, f80_round(st0));
}

void fpu_fscale(void)
{
    f80_t st0 = fpu_get_st0();
    f80_t y = f80_mul(st0, f80_two_pow(f80_trunc(fpu_get_sti(1))));
    fpu_write_st(cpu->fpu_stack_ptr, y);
}

void fpu_fsin(void)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st(cpu->fpu_stack_ptr, f80_sin(st0));
    cpu->fpu_status_word &= (uint16_t)~FPU_C2;
}

void fpu_fcos(void)
{
    f80_t st0 = fpu_get_st0();
    fpu_write_st(cpu->fpu_stack_ptr, f80_cos(st0));
    cpu->fpu_status_word &= (uint16_t)~FPU_C2;
}

void fpu_fdecstp(void)
{
    cpu->fpu_stack_ptr = (cpu->fpu_stack_ptr - 1) & 7;
    cpu->fpu_status_word &= (uint16_t)~FPU_C1;
}

void fpu_fincstp(void)
{
    cpu->fpu_stack_ptr = (cpu->fpu_stack_ptr + 1) & 7;
    cpu->fpu_status_word &= (uint16_t)~FPU_C1;
}

/* ------------------------------------------------------------------ *
 * FXSAVE / FXRSTOR (misc_instr.rs:369-428, deferred to M1b by M1a-2)
 * ------------------------------------------------------------------ */

void fxsave(int64_t addr)
{
    int i;

    /* NO CHECK HERE.  Upstream's `dbg_assert!(addr & 0xF == 0, "TODO: #gp")`
     * (misc_instr.rs:370) is a DEBUG assert, COMPILED OUT of the pinned release reference,
     * which therefore just executes at a misaligned operand -- real hardware would #gp, as
     * upstream's own "TODO" says.  The port shipped it as a live DBG_ASSERT, so ordinary
     * guest bytes (`fxsave [eax]` with eax & 0xF != 0) aborted the machine where the
     * reference writes: R4's N3.  Resolved the way R3's twin was resolved at its own site
     * (gen/interpreter_c.js, "C-side difference 3"): the condition is a property of the
     * GUEST's input rather than an invariant of this port, so the check is dropped AT THIS
     * SITE, matching the release reference -- and NOT by -DNDEBUG, which would silence the
     * internal invariants the port relies on (e.g. src/cpu/cpu.c:83's stop, the one
     * `make nasm`'s `jump` test ends on).  Every internal invariant keeps its DBG_ASSERT.
     * Probe: results/r4f-n3.sh (misaligned operand: no abort, transcript == reference). */
    writable_or_pagefault(addr, 288);
    if (cpu->pagefault) return;

    safe_write16(addr + 0, cpu->fpu_control_word);
    safe_write16(addr + 2, fpu_load_status_word());
    safe_write8(addr + 4, (int32_t)((uint8_t)~cpu->fpu_stack_empty & 0xFF));
    safe_write16(addr + 6, cpu->fpu_opcode);
    safe_write32(addr + 8, cpu->fpu_ip);
    safe_write16(addr + 12, cpu->fpu_ip_selector);
    safe_write32(addr + 16, cpu->fpu_dp);
    safe_write16(addr + 20, cpu->fpu_dp_selector);

    safe_write32(addr + 24, cpu->mxcsr);
    safe_write32(addr + 28, MXCSR_MASK);

    for (i = 0; i < 8; i++) {
        int32_t reg_index = (i + cpu->fpu_stack_ptr) & 7;
        fpu_store_m80(addr + 32 + (i << 4), cpu->fpu_st[reg_index]);
    }

    /* "If the OSFXSR bit in CR4 is not set, the FXSAVE instruction may not save these
     * registers.  This behavior is implementation dependent." (upstream's comment:
     * they are saved unconditionally) */
    for (i = 0; i < 8; i++) {
        safe_write128(addr + 160 + (i << 4), cpu->reg_xmm[i]);
    }
}

void fxrstor(int64_t addr)
{
    int32_t new_mxcsr;
    int i;

    /* NO CHECK HERE -- fxrstor's twin of fxsave's site.  Upstream's
     * `dbg_assert!(addr & 0xF == 0, "TODO: #gp")` (misc_instr.rs:398) is compiled out of
     * the release reference, so the port matches it by not checking; see fxsave above for
     * the full reasoning (R4's N3) and why -DNDEBUG was not the route. */
    readable_or_pagefault(addr, 288);
    if (cpu->pagefault) return;

    new_mxcsr = safe_read32s(addr + 24);

    if (0 != (new_mxcsr & ~MXCSR_MASK)) {
        /* dbg_log!("#gp Invalid mxcsr bits") dropped */
        trigger_gp(0);
        return;
    }

    set_control_word((uint16_t)safe_read16(addr + 0));
    fpu_set_status_word((uint16_t)safe_read16(addr + 2));
    cpu->fpu_stack_empty = (uint8_t)~safe_read8(addr + 4);
    cpu->fpu_opcode = safe_read16(addr + 6);
    cpu->fpu_ip = safe_read32s(addr + 8);
    cpu->fpu_ip_selector = safe_read16(addr + 12);
    cpu->fpu_dp = safe_read32s(addr + 16);
    cpu->fpu_dp_selector = safe_read16(addr + 20);

    set_mxcsr(new_mxcsr);

    for (i = 0; i < 8; i++) {
        int32_t reg_index = (cpu->fpu_stack_ptr + i) & 7;
        cpu->fpu_st[reg_index] = fpu_load_m80(addr + 32 + (i << 4));
    }

    for (i = 0; i < 8; i++) {
        cpu->reg_xmm[i] = safe_read128s(addr + 160 + (i << 4));
    }
}
