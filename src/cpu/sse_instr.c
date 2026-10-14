/*
 * sse_instr.c -- port of v86/src/rust/cpu/sse_instr.rs (361 lines) plus
 * cpu.rs:4201-4207 (`transition_fpu_to_mmx`).  See sse_instr.h for the scope, the
 * MMX/x87 aliasing note and the fault convention.
 *
 * THREE MECHANICAL RULES, each of which is a place a silent slip would live:
 *
 *   1. Rust's `<<` on a signed integer is WRAPPING; C's is UNDEFINED BEHAVIOUR when the
 *      shifted-in bits change the sign (`1 << 31` on an `int`).  Every left shift over a
 *      possibly-negative lane is therefore done on the UNSIGNED pattern and cast back --
 *      the M1a-2 D5 rule, the same spelling instructions.c and instructions_0f.c use.
 *      The shifts that cannot overflow (u16/u32 lanes promoted to `int`, shift < 16) are
 *      left in the promoted form and named where the arithmetic is not obvious.
 *   2. Rust's `x as i32 >> n` on a NEGATIVE value is an ARITHMETIC shift.  C's `>>` on a
 *      signed type is implementation-defined; the port relies on it being arithmetic
 *      (gcc/clang are, and arith.c already does -- arith.c:131,207), which is why the
 *      sign-fill arms below are plain `>>` rather than a hand-rolled helper.
 *   3. Rust's float->int `as` SATURATES nothing: it is `x as i64 as i32`, i.e. convert to
 *      64-bit then truncate the bits.  The range check in front of it is what keeps the
 *      C conversion in range (out-of-range float->int is UB in C, a panic in Rust).
 *
 * The lane arrays are read out of the u64/u128 words with explicit shifts rather than a
 * `memcpy` into a `uint16_t[4]`: the reg128 union is 16-byte aligned but the arrays are
 * not, and the shift form is the same bit pattern with no aliasing question at all.
 */

#include "sse_instr.h"

#include <math.h>
#include <string.h>

#include "fpu.h"        /* fpu_set_tag_word -- the MMX/x87 aliasing transition */
#include "misc_instr.h" /* MXCSR_RC_SHIFT */
#include "paging.h"

/* ------------------------------------------------------------------ *
 * cpu.rs:4201-4207 -- the MMX/x87 aliasing transition
 * ------------------------------------------------------------------ */

/* "Set the fpu tag word to valid and the top-of-stack to 0 on mmx instructions."
 * fpu_set_tag_word(0) marks all eight slots non-empty; fpu_stack_ptr = 0 re-anchors
 * st(0).  MEASURED against the reference: this is what moves the tag word and the
 * stack-empty byte that R3F's `F2 0F 7F` probe reads back (results/r3f-fixes.md ss3.3). */
void transition_fpu_to_mmx(void)
{
    fpu_set_tag_word(0);
    cpu->fpu_stack_ptr = 0;
}

/* ------------------------------------------------------------------ *
 * sse_instr.rs:4-27 -- the mov* family
 * ------------------------------------------------------------------ */

void mov_r_m64(int32_t addr, int32_t r)
{
    uint64_t data = read_mmx64s(r);
    safe_write64(addr, data);
    if (cpu->pagefault) return;
    transition_fpu_to_mmx();
}

void movl_r128_m64(int32_t addr, int32_t r)
{
    uint64_t data = read_xmm64s(r);
    safe_write64(addr, data);
    if (cpu->pagefault) return;
}

void mov_r_r128(int32_t r1, int32_t r2)
{
    reg128 data = read_xmm128s(r2);
    write_xmm_reg128(r1, data);
}

void mov_r_m128(int32_t addr, int32_t r)
{
    reg128 data = read_xmm128s(r);
    safe_write128(addr, data);
    if (cpu->pagefault) return;
}

void mov_rm_r128(reg128 source, int32_t r)
{
    write_xmm_reg128(r, source);
}

void movh_r128_m64(int32_t addr, int32_t r)
{
    reg128 data = read_xmm128s(r);
    safe_write64(addr, data.u64[1]);
    if (cpu->pagefault) return;
}

/* ------------------------------------------------------------------ *
 * sse_instr.rs:29-63 -- pand, pandn, pxor, por
 * ------------------------------------------------------------------ */

/* sse_instr.rs:29-40 */
void pand_r128(reg128 source, int32_t r)
{
    /* pand xmm, xmm/m128 */
    /* XXX: Aligned access or #gp */
    reg128 destination = read_xmm128s(r);
    reg128 result;
    memset(&result, 0, sizeof(result));
    result.u64[0] = source.u64[0] & destination.u64[0];
    result.u64[1] = source.u64[1] & destination.u64[1];
    write_xmm_reg128(r, result);
}

/* sse_instr.rs:41-52 */
void pandn_r128(reg128 source, int32_t r)
{
    /* pandn xmm, xmm/m128 */
    /* XXX: Aligned access or #gp */
    reg128 destination = read_xmm128s(r);
    reg128 result;
    memset(&result, 0, sizeof(result));
    result.u64[0] = source.u64[0] & ~destination.u64[0];
    result.u64[1] = source.u64[1] & ~destination.u64[1];
    write_xmm_reg128(r, result);
}

/* sse_instr.rs:53-64 */
void pxor_r128(reg128 source, int32_t r)
{
    /* pxor xmm, xmm/m128 */
    /* XXX: Aligned access or #gp */
    reg128 destination = read_xmm128s(r);
    reg128 result;
    memset(&result, 0, sizeof(result));
    result.u64[0] = source.u64[0] ^ destination.u64[0];
    result.u64[1] = source.u64[1] ^ destination.u64[1];
    write_xmm_reg128(r, result);
}

/* sse_instr.rs:65-76 */
void por_r128(reg128 source, int32_t r)
{
    /* por xmm, xmm/m128 */
    /* XXX: Aligned access or #gp */
    reg128 destination = read_xmm128s(r);
    reg128 result;
    memset(&result, 0, sizeof(result));
    result.u64[0] = source.u64[0] | destination.u64[0];
    result.u64[1] = source.u64[1] | destination.u64[1];
    write_xmm_reg128(r, result);
}

/* ------------------------------------------------------------------ *
 * sse_instr.rs:65-178 -- the shifts
 *
 * Every one of these reads its destination, writes the result and (for the MMX forms)
 * runs transition_fpu_to_mmx().  The CLAMP differs per instruction and is upstream's:
 * psrlw/psraw treat a count > 15 as 16 (a full shift-out, sign-filling for psraw),
 * psrld/psrad a count > 31 as 31, psllw/psllq a count out of range as ZERO, psrlq a
 * count > 63 as zero.  That asymmetry is not a slip: it is read off the source.
 * ------------------------------------------------------------------ */

void psrlw_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    int i;
    if (shift > 15) shift = 16;
    for (i = 0; i < 4; i++) {
        uint16_t lane = (uint16_t)(destination >> (16 * i));
        result |= (uint64_t)(uint16_t)((uint32_t)lane >> shift) << (16 * i);
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void psraw_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    int i;
    if (shift > 15) shift = 16;
    for (i = 0; i < 4; i++) {
        int16_t lane = (int16_t)(destination >> (16 * i));
        result |= (uint64_t)(uint16_t)((int32_t)lane >> shift) << (16 * i);
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void psllw_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    int i;
    if (shift <= 15) {
        for (i = 0; i < 4; i++) {
            uint16_t lane = (uint16_t)(destination >> (16 * i));
            /* lane promotes to int and shift <= 15, so this cannot reach int's sign bit */
            result |= (uint64_t)(uint16_t)(lane << shift) << (16 * i);
        }
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void psrld_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    int i;
    if (shift <= 31) {
        for (i = 0; i < 2; i++) {
            uint32_t lane = (uint32_t)(destination >> (32 * i));
            result |= (uint64_t)(lane >> shift) << (32 * i);
        }
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void psrad_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    int i;
    if (shift > 31) shift = 31;
    for (i = 0; i < 2; i++) {
        int32_t lane = (int32_t)(uint32_t)(destination >> (32 * i));
        result |= (uint64_t)(uint32_t)(lane >> shift) << (32 * i);
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void pslld_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    int i;
    if (shift <= 31) {
        for (i = 0; i < 2; i++) {
            uint32_t lane = (uint32_t)(destination >> (32 * i));
            result |= (uint64_t)(lane << shift) << (32 * i);
        }
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void psrlq_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    if (shift <= 63) {
        result = destination >> shift;
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void psllq_r64(int32_t r, uint64_t shift)
{
    uint64_t destination = read_mmx64s(r);
    uint64_t result = 0;
    if (shift <= 63) {
        result = destination << shift;
    }
    write_mmx_reg64(r, result);
    transition_fpu_to_mmx();
}

void psrlw_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    uint32_t dword0 = 0, dword1 = 0, dword2 = 0, dword3 = 0;
    if (shift <= 15) {
        dword0 = ((uint32_t)destination.u16[0] >> shift) |
                 (((uint32_t)destination.u16[1] >> shift) << 16);
        dword1 = ((uint32_t)destination.u16[2] >> shift) |
                 (((uint32_t)destination.u16[3] >> shift) << 16);
        dword2 = ((uint32_t)destination.u16[4] >> shift) |
                 (((uint32_t)destination.u16[5] >> shift) << 16);
        dword3 = ((uint32_t)destination.u16[6] >> shift) |
                 (((uint32_t)destination.u16[7] >> shift) << 16);
    }
    write_xmm128(r, (int32_t)dword0, (int32_t)dword1, (int32_t)dword2, (int32_t)dword3);
}

void psraw_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    int32_t shift_clamped = (int32_t)(shift > 15 ? 16 : (uint32_t)shift);
    uint32_t dword0, dword1, dword2, dword3;
    dword0 = ((uint32_t)((int32_t)destination.i16[0] >> shift_clamped) & 0xFFFF) |
             ((uint32_t)((int32_t)destination.i16[1] >> shift_clamped) << 16);
    dword1 = ((uint32_t)((int32_t)destination.i16[2] >> shift_clamped) & 0xFFFF) |
             ((uint32_t)((int32_t)destination.i16[3] >> shift_clamped) << 16);
    dword2 = ((uint32_t)((int32_t)destination.i16[4] >> shift_clamped) & 0xFFFF) |
             ((uint32_t)((int32_t)destination.i16[5] >> shift_clamped) << 16);
    dword3 = ((uint32_t)((int32_t)destination.i16[6] >> shift_clamped) & 0xFFFF) |
             ((uint32_t)((int32_t)destination.i16[7] >> shift_clamped) << 16);
    write_xmm128(r, (int32_t)dword0, (int32_t)dword1, (int32_t)dword2, (int32_t)dword3);
}

void psllw_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    uint32_t dword0 = 0, dword1 = 0, dword2 = 0, dword3 = 0;
    if (shift <= 15) {
        dword0 = (((uint32_t)destination.u16[0] << shift) & 0xFFFF) |
                 (((uint32_t)destination.u16[1] << shift) << 16);
        dword1 = (((uint32_t)destination.u16[2] << shift) & 0xFFFF) |
                 (((uint32_t)destination.u16[3] << shift) << 16);
        dword2 = (((uint32_t)destination.u16[4] << shift) & 0xFFFF) |
                 (((uint32_t)destination.u16[5] << shift) << 16);
        dword3 = (((uint32_t)destination.u16[6] << shift) & 0xFFFF) |
                 (((uint32_t)destination.u16[7] << shift) << 16);
    }
    write_xmm128(r, (int32_t)dword0, (int32_t)dword1, (int32_t)dword2, (int32_t)dword3);
}

void psrld_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    uint32_t dword0 = 0, dword1 = 0, dword2 = 0, dword3 = 0;
    if (shift <= 31) {
        dword0 = destination.u32[0] >> shift;
        dword1 = destination.u32[1] >> shift;
        dword2 = destination.u32[2] >> shift;
        dword3 = destination.u32[3] >> shift;
    }
    write_xmm128(r, (int32_t)dword0, (int32_t)dword1, (int32_t)dword2, (int32_t)dword3);
}

void psrad_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    int32_t shift_clamped = (int32_t)(shift > 31 ? 31 : (uint32_t)shift);
    write_xmm128(r,
                 destination.i32[0] >> shift_clamped,
                 destination.i32[1] >> shift_clamped,
                 destination.i32[2] >> shift_clamped,
                 destination.i32[3] >> shift_clamped);
}

void pslld_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    uint32_t dword0 = 0, dword1 = 0, dword2 = 0, dword3 = 0;
    if (shift <= 31) {
        dword0 = (uint32_t)destination.i32[0] << shift;
        dword1 = (uint32_t)destination.i32[1] << shift;
        dword2 = (uint32_t)destination.i32[2] << shift;
        dword3 = (uint32_t)destination.i32[3] << shift;
    }
    write_xmm128(r, (int32_t)dword0, (int32_t)dword1, (int32_t)dword2, (int32_t)dword3);
}

void psrlq_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    reg128 result;
    memset(&result, 0, sizeof(result));
    if (shift <= 63) {
        result.u64[0] = destination.u64[0] >> shift;
        result.u64[1] = destination.u64[1] >> shift;
    }
    write_xmm_reg128(r, result);
}

void psllq_r128(int32_t r, uint64_t shift)
{
    reg128 destination = read_xmm128s(r);
    reg128 result;
    memset(&result, 0, sizeof(result));
    if (shift <= 63) {
        result.u64[0] = destination.u64[0] << shift;
        result.u64[1] = destination.u64[1] << shift;
    }
    write_xmm_reg128(r, result);
}

/* ------------------------------------------------------------------ *
 * sse_instr.rs:180-199 -- the comparison and min/max helpers
 * ------------------------------------------------------------------ */

bool sse_comparison(int32_t op, double x, double y)
{
    /* TODO: Signaling */
    switch (op & 7) {
        case 0: return x == y;
        case 1: return x < y;
        case 2: return x <= y;
        case 3: return isnan(x) || isnan(y);
        case 4: return x != y || isnan(x) || isnan(y);
        case 5: return x >= y || isnan(x) || isnan(y);
        case 6: return x > y || isnan(x) || isnan(y);
        default: return !isnan(x) && !isnan(y);
    }
}

/* "if both x and y are 0 or x is nan, y is returned" -- upstream's `if x < y { x } else
 * { y }`, NOT fmin: for x = -0.0, y = 0.0 the comparison is false and y is returned. */
double sse_min(double x, double y) { return x < y ? x : y; }
double sse_max(double x, double y) { return x > y ? x : y; }

/* ------------------------------------------------------------------ *
 * sse_instr.rs:224-282 / 298-330 -- the conversions
 * ------------------------------------------------------------------ */

int32_t sse_convert_with_truncation_f32_to_i32(float x)
{
    x = truncf(x);
    if (x >= -2147483648.0f && x < 2147483648.0f) {
        return (int32_t)(int64_t)x;
    }
    else {
        /* TODO: Signal */
        return (int32_t)0x80000000u;
    }
}

int32_t sse_convert_f32_to_i32(float x)
{
    double rounded = sse_integer_round((double)x);
    if (rounded >= -2147483648.0 && rounded < 2147483648.0) {
        return (int32_t)(int64_t)rounded;
    }
    else {
        /* TODO: Signal */
        return (int32_t)0x80000000u;
    }
}

int32_t sse_convert_with_truncation_f64_to_i32(double x)
{
    x = trunc(x);
    if (x >= -2147483648.0 && x < 2147483648.0) {
        return (int32_t)(int64_t)x;
    }
    else {
        /* TODO: Signal */
        return (int32_t)0x80000000u;
    }
}

int32_t sse_convert_f64_to_i32(double x)
{
    x = sse_integer_round(x);
    if (x >= -2147483648.0 && x < 2147483648.0) {
        return (int32_t)(int64_t)x;
    }
    else {
        /* TODO: Signal */
        return (int32_t)0x80000000u;
    }
}

/* sse_instr.rs:298-330 -- see fpu_integer_round.  `round` is Rust's
 * f64::round = C's round(): half away from zero.  mxcsr is masked to 16 bits by
 * set_mxcsr, so the unsigned shift below and upstream's `>>` on an i32 agree. */
double sse_integer_round(double f)
{
    int32_t rc = (int32_t)(((uint32_t)cpu->mxcsr >> MXCSR_RC_SHIFT) & 3);
    if (rc == 0) {
        /* Round to nearest, or even if equidistant */
        double rounded = round(f);
        double diff = rounded - f;
        if (diff == 0.5 || diff == -0.5) {
            rounded = 2.0 * round(f * 0.5);
        }
        return rounded;
    }
    else if (rc == 1 || (rc == 3 && f > 0.0)) {
        /* rc=3 is truncate -> floor for positive numbers */
        return floor(f);
    }
    else {
        return ceil(f);
    }
}
