/*
 * softfloat.c -- the F80 wrapper: softfloat.rs's API over Berkeley SoftFloat.
 *
 * See softfloat.h for what this file is and is not.  Every function below is one
 * softfloat.rs item (the citation is in the header); there are no algorithms here --
 * the arithmetic is the vendored engine's.  What this file DOES own, and what a
 * transcription slip would break, is:
 *
 *   - the argument/return marshalling into the engine's pointer convention;
 *   - the two "operator" methods that are NOT single engine calls: `Rem`
 *     (softfloat.rs:253-265 -- quotient/truncate/subtract, NOT extF80M_rem, which
 *     upstream leaves commented out) and the `Neg`/`Abs` bit twiddles;
 *   - the exception-flag translation (softfloat.rs:189-194).
 *
 * NAMING DIVERGENCE: the Rust `impl F80` methods are `of_i32`/`to_f64`/`add`/... reached
 * as `F80::of_i32(x)` or `a + b`; C has no namespace or operators, so they are
 * `f80_of_i32`/`f80_to_f64`/`f80_add`.  The two-argument comparisons return an int
 * (F80_CMP_*) where Rust returns `Option<Ordering>`.
 */

#include "softfloat.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * the engine's knobs (softfloat.rs:169-194)
 * ------------------------------------------------------------------ */

void f80_set_rounding_mode(int mode)
{
    softfloat_roundingMode = (uint_fast8_t)mode;
}

void f80_set_precision(int precision)
{
    extF80_roundingPrecision = (uint_fast8_t)precision;
}

/* softfloat.rs:189-193, verbatim:
 *     f >> 4 & 1 | f >> 1 & 4 | f << 3 & 16
 * i.e. SoftFloat's invalid (16) -> x87 IE (bit 0), infinite/zero-divide (8) -> x87 ZE
 * (bit 2), underflow (2) -> x87 UE (bit 4).  NOTE the translation DROPS SoftFloat's
 * inexact (1) and DROPS overflow (4); that is upstream's mapping, reproduced, not fixed
 * -- the x87 PE/OE bits are simply not raised by the reference either. */
uint8_t f80_get_exception_flags(void)
{
    uint_fast8_t f = softfloat_exceptionFlags;
    return (uint8_t)(((f >> 4) & 1) | ((f >> 1) & 4) | ((f << 3) & 16));
}

void f80_clear_exception_flags(void)
{
    softfloat_exceptionFlags = 0;
}

/* ------------------------------------------------------------------ *
 * inspection (softfloat.rs:90-91)
 * ------------------------------------------------------------------ */

bool f80_sign(f80_t x) { return (x.sign_exponent >> 15) == 1; }

int16_t f80_exponent(f80_t x)
{
    return (int16_t)(((int16_t)x.sign_exponent & 0x7FFF) - 0x3FFF);
}

/* ------------------------------------------------------------------ *
 * conversions in (softfloat.rs:93-114, 119-123)
 *
 * `of_*` ignores the exception flags (upstream's F80::of_* never touches them; the
 * fpu.c callers that care call f80_clear_exception_flags/f80_get_exception_flags around
 * the call, exactly as fpu.rs:88-116 does).
 * ------------------------------------------------------------------ */

f80_t f80_of_i32(int32_t src)
{
    f80_t x = F80_ZERO;
    i32_to_extF80M(src, &x);
    return x;
}

f80_t f80_of_i64(int64_t src)
{
    f80_t x = F80_ZERO;
    i64_to_extF80M(src, &x);
    return x;
}

f80_t f80_of_f32(uint32_t src)
{
    f80_t x = F80_ZERO;
    /* N2: the engine's parameter is the struct type; the plain bit pattern goes in. */
    f32_to_extF80M((float32_t){ src }, &x);
    return x;
}

f80_t f80_of_f64(uint64_t src)
{
    f80_t x = F80_ZERO;
    f64_to_extF80M((float64_t){ src }, &x);
    return x;
}

/* softfloat.rs:115 `F80::of_f64x` (private upstream; public here because fpu.rs:142/493
 * build F80 constants with `F80::of_f64(f64::to_bits(v))`) and :119 `to_f64x`. */
f80_t f80_of_f64x(double src)
{
    uint64_t bits;
    memcpy(&bits, &src, sizeof(bits));
    return f80_of_f64(bits);
}

/* ------------------------------------------------------------------ *
 * conversions out (softfloat.rs:117-125)
 *
 * to_i32/to_i64 read softfloat_roundingMode (the `RoundingMode` in force), so they use
 * the current knob rather than a literal; truncate_* are the explicit-minMag forms used
 * by FISTTP.
 * ------------------------------------------------------------------ */

int32_t f80_to_i32(f80_t x) { return (int32_t)extF80M_to_i32(&x, softfloat_roundingMode, false); }
int64_t f80_to_i64(f80_t x) { return (int64_t)extF80M_to_i64(&x, softfloat_roundingMode, false); }
int32_t f80_truncate_to_i32(f80_t x) { return (int32_t)extF80M_to_i32(&x, F80_ROUND_MINMAG, false); }
int64_t f80_truncate_to_i64(f80_t x) { return (int64_t)extF80M_to_i64(&x, F80_ROUND_MINMAG, false); }

uint32_t f80_convert_to_f32(f80_t x) { return extF80M_to_f32(&x).v; }
uint64_t f80_convert_to_f64(f80_t x) { return extF80M_to_f64(&x).v; }

static double f80_to_f64x(f80_t x)
{
    uint64_t bits = f80_convert_to_f64(x);
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/* ------------------------------------------------------------------ *
 * the libm family (softfloat.rs:127-142)
 *
 * Upstream converts F80 -> f64, then calls RUST's f64::cos/sin/tan/atan/atan2/hypot-free
 * log2/ln/powf, then converts back.  The C port calls the host <math.h> instead: the
 * reference's own path is the platform double libm, and cosmocc's libm is the host's.
 * MEASURED equivalence is what the nasm FPU corpus checks (the 7-significant-digit
 * tolerance of run.js:54-60 exists for exactly this rounding), so a libm difference shows
 * up there rather than being hidden here.
 * ------------------------------------------------------------------ */

f80_t f80_cos(f80_t x) { return f80_of_f64x(cos(f80_to_f64x(x))); }
f80_t f80_sin(f80_t x) { return f80_of_f64x(sin(f80_to_f64x(x))); }
f80_t f80_tan(f80_t x) { return f80_of_f64x(tan(f80_to_f64x(x))); }
f80_t f80_atan(f80_t x) { return f80_of_f64x(atan(f80_to_f64x(x))); }
f80_t f80_atan2(f80_t y, f80_t x) { return f80_of_f64x(atan2(f80_to_f64x(y), f80_to_f64x(x))); }
f80_t f80_log2(f80_t x) { return f80_of_f64x(log2(f80_to_f64x(x))); }
f80_t f80_ln(f80_t x) { return f80_of_f64x(log(f80_to_f64x(x))); }
f80_t f80_two_pow(f80_t x) { return f80_of_f64x(pow(2.0, f80_to_f64x(x))); }

/* softfloat.rs:143-152: round uses softfloat_roundingMode, trunc uses minMag (1). */
f80_t f80_round(f80_t x)
{
    f80_t r = F80_ZERO;
    extF80M_roundToInt(&x, softfloat_roundingMode, false, &r);
    return r;
}
f80_t f80_trunc(f80_t x)
{
    f80_t r = F80_ZERO;
    extF80M_roundToInt(&x, F80_ROUND_MINMAG, false, &r);
    return r;
}

f80_t f80_sqrt(f80_t x)
{
    f80_t r = F80_ZERO;
    extF80M_sqrt(&x, &r);
    return r;
}

/* ------------------------------------------------------------------ *
 * sign / magnitude (softfloat.rs:136-141, 229-236)
 * ------------------------------------------------------------------ */

f80_t f80_abs(f80_t x)
{
    x.sign_exponent &= (uint16_t)~0x8000;
    return x;
}

f80_t f80_neg(f80_t x)
{
    x.sign_exponent ^= 1 << 15;
    return x;
}

/* ------------------------------------------------------------------ *
 * the arithmetic operators (softfloat.rs:213-251)
 * ------------------------------------------------------------------ */

f80_t f80_add(f80_t x, f80_t y)
{
    f80_t r = F80_ZERO;
    extF80M_add(&x, &y, &r);
    return r;
}

f80_t f80_sub(f80_t x, f80_t y)
{
    f80_t r = F80_ZERO;
    extF80M_sub(&x, &y, &r);
    return r;
}

f80_t f80_mul(f80_t x, f80_t y)
{
    f80_t r = F80_ZERO;
    extF80M_mul(&x, &y, &r);
    return r;
}

f80_t f80_div(f80_t x, f80_t y)
{
    f80_t r = F80_ZERO;
    extF80M_div(&x, &y, &r);
    return r;
}

/* softfloat.rs:253-265.  The `extF80M_rem` route is COMMENTED OUT upstream (it rounds to
 * nearest); the live definition is quotient-truncate-subtract, and the fpu.c callers
 * (FPREM/FPREM1) depend on that difference -- FPREM1's ieee flag picks round-vs-trunc at
 * the CALLER (fpu.rs:494), the `rem` operator itself is always truncation. */
f80_t f80_rem(f80_t x, f80_t y)
{
    f80_t quot = f80_trunc(f80_div(x, y));
    return f80_sub(x, f80_mul(quot, y));
}

/* ------------------------------------------------------------------ *
 * comparisons (softfloat.rs:160-167, 196-210, 267-286)
 * ------------------------------------------------------------------ */

bool f80_is_finite(f80_t x)
{
    /* softfloat.rs:160-163: `self != POS_INFINITY && self != NEG_INFINITY` -- the
     * equality test, reproduced rather than replaced by an exponent test, because it is
     * upstream's own (weaker) definition. */
    return !f80_eq(x, F80_POS_INFINITY) && !f80_eq(x, F80_NEG_INFINITY);
}

bool f80_is_nan(f80_t x)
{
    /* softfloat.rs:164-167: `self != self` */
    return !extF80M_eq(&x, &x);
}

bool f80_eq(f80_t x, f80_t y) { return extF80M_eq(&x, &y); }
bool f80_lt(f80_t x, f80_t y) { return extF80M_lt(&x, &y); }
bool f80_lt_quiet(f80_t x, f80_t y) { return extF80M_lt_quiet(&x, &y); }

static int cmp_from_lt(bool lt_ab, bool lt_ba, bool eq_ab)
{
    if (lt_ab) return F80_CMP_LT;
    if (lt_ba) return F80_CMP_GT;
    if (eq_ab) return F80_CMP_EQ;
    return F80_CMP_UNORDERED;
}

int f80_partial_cmp(f80_t x, f80_t y) /* softfloat.rs:270-285 */
{
    return cmp_from_lt(f80_lt(x, y), f80_lt(y, x), f80_eq(x, y));
}

int f80_partial_cmp_quiet(f80_t x, f80_t y) /* softfloat.rs:196-210 */
{
    return cmp_from_lt(f80_lt_quiet(x, y), f80_lt_quiet(y, x), f80_eq(x, y));
}
