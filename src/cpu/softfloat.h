/*
 * softfloat.h -- the 80-bit extended-precision arithmetic core's interface.
 *
 * Port of v86/src/rust/softfloat.rs (286 lines).  That file is NOT an implementation:
 * it is a thin Rust wrapper around Berkeley SoftFloat's `extF80M_*` entry points
 * (softfloat.rs:1-32 declares them `extern "C"`), plus a `#[repr(C)] struct F80`
 * (softfloat.rs:47-56) whose layout IS Berkeley's `extFloat80M`.
 *
 * So the port has two pieces:
 *
 *   1. THE ENGINE, reused verbatim: v86/lib/softfloat/softfloat.c, a 32501-line
 *      amalgamation of SoftFloat Release 3e (8086-SSE specialization), compiled from
 *      the READ-ONLY upstream tree with upstream's own defines (v86/Makefile:242-248).
 *      It is a third-party C source; the port does not edit it.  Its entry points and
 *      its three global knobs are declared below.
 *
 *   2. THE WRAPPER, this file's `f80_*` functions: softfloat.rs's `impl F80` method by
 *      method -- the value conversions, the arithmetic operators, the comparisons, the
 *      rounding-mode/precision setters, and the exception-flag translation
 *      (softfloat.rs:189-194).  `src/cpu/softfloat.c` defines them.
 *
 * THE TYPES ACROSS THE BOUNDARY (stated because it is a real ABI decision, not
 * decoration).  The amalgamation's types (softfloat_types.h, softfloat.c:397-424) are
 *
 *     typedef struct { uint32_t v; } float32_t;
 *     typedef struct { uint64_t v; } float64_t;
 *     struct extFloat80M { uint64_t signif; uint16_t signExp; };
 *
 * In the System V amd64 and AAPCS64 ABIs a single-member uint struct and the
 * corresponding plain unsigned integer are passed and returned in the SAME register
 * (INTEGER class; both are 8 bytes or fewer, no coercion difference) -- which is exactly
 * why upstream's Rust FFI can declare `fn f32_to_extF80M(src: i32)` and
 * `fn extF80M_to_f32(...) -> i32` and get away with it (softfloat.rs:24-27).  The C port
 * declares the engine's SmallFloat entry points with the ENGINE'S OWN struct types (the
 * two typedefs below, and N2 in results/r4f-fixes.md for why the plain-integer spelling
 * that stood here was formally UB), so the ABI assumption is now made only where it is
 * unavoidable: the wrapper hands the plain `uint32_t`/`uint64_t` bit patterns in and out
 * at the four call sites in softfloat.c, and a selftest case round-trips f32 and f64
 * through F80 and back so the assumption is MEASURED, not assumed.  `f80_t`
 * (cpu_state.h) is declared, not aliased, for the same reason (its `_Static_assert`s pin
 * size 16 and `sign_exponent`@8, the two facts `extFloat80_t` needs).
 *
 * DIVERGENCE FROM UPSTREAM, ROUNDING: softfloat.rs has no state of its own; the
 * rounding mode and precision live in the C globals below, and `set_control_word`
 * (fpu.c) is the only writer.  At reset neither `reset_cpu` nor `cpu.js` sets them, and
 * the compiled-in defaults (near-even, precision 80) are exactly what the reset control
 * word 0x37F asks for -- asserted by the selftest rather than left as a coincidence.
 */

#ifndef V86C_SOFTFLOAT_H
#define V86C_SOFTFLOAT_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu_state.h" /* f80_t, F80_ZERO, F80_ONE */

/* ------------------------------------------------------------------ *
 * THE ENGINE'S ENTRY POINTS (v86/lib/softfloat/softfloat.c).
 *
 * Declared exactly as the amalgamation defines them -- including the SmallFloat
 * single-member-uint types, which are declared here as the same struct types the engine's
 * own prototypes use (softfloat.c:397-399) rather than as the plain integers those structs
 * are ABI-identical to.  N2: declaring them with plain integers was a cross-TU
 * incompatible-type redeclaration, i.e. formally UB in C; the two typedefs below remove it
 * at no ABI cost (a single-member uint struct and that uint are both INTEGER class, one
 * register, on SysV amd64 and AAPCS64) and the selftest's ABI round-trip case keeps the
 * claim MEASURED.  Only the entry points softfloat.rs actually uses are declared; the rest
 * of the library is compiled but unreferenced.
 * ------------------------------------------------------------------ */

typedef struct { uint32_t v; } float32_t; /* softfloat.c:397 */
typedef struct { uint64_t v; } float64_t; /* softfloat.c:398 */

void extF80M_add(const f80_t *x, const f80_t *y, f80_t *out);
void extF80M_sub(const f80_t *x, const f80_t *y, f80_t *out);
void extF80M_mul(const f80_t *x, const f80_t *y, f80_t *out);
void extF80M_div(const f80_t *x, const f80_t *y, f80_t *out);
void extF80M_sqrt(const f80_t *x, f80_t *out);
void extF80M_roundToInt(const f80_t *x, uint_fast8_t rounding_mode, bool raise_inexact, f80_t *out);

bool extF80M_eq(const f80_t *x, const f80_t *y);
bool extF80M_lt(const f80_t *x, const f80_t *y);
bool extF80M_lt_quiet(const f80_t *x, const f80_t *y);

int_fast32_t extF80M_to_i32(const f80_t *x, uint_fast8_t rounding_mode, bool raise_inexact);
int_fast64_t extF80M_to_i64(const f80_t *x, uint_fast8_t rounding_mode, bool raise_inexact);
void i32_to_extF80M(int32_t src, f80_t *out);
void i64_to_extF80M(int64_t src, f80_t *out);
void f32_to_extF80M(float32_t src, f80_t *out); /* softfloat.c:566, :16183 */
void f64_to_extF80M(float64_t src, f80_t *out); /* softfloat.c:601, :18965 */
float32_t extF80M_to_f32(const f80_t *x);       /* softfloat.c:665, :24424 */
float64_t extF80M_to_f64(const f80_t *x);       /* softfloat.c:666, :24538 */

/* softfloat.rs:29-31's three mutable globals; softfloat.c:10366-10371 defines them.
 * THREAD_LOCAL is empty in this build (platform.h), so they are plain globals. */
extern uint_fast8_t softfloat_roundingMode;
extern uint_fast8_t extF80_roundingPrecision;
extern uint_fast8_t softfloat_exceptionFlags;

/* softfloat.c:448-455, softfloat.c:623's precision values. */
enum {
    F80_ROUND_NEAR_EVEN = 0,
    F80_ROUND_MINMAG    = 1,
    F80_ROUND_MIN       = 2,
    F80_ROUND_MAX       = 3,
};
enum {
    F80_PREC_P80 = 80,
    F80_PREC_P64 = 64,
    F80_PREC_P32 = 32,
};

/* ------------------------------------------------------------------ *
 * THE REMAINING F80 CONSTANTS (softfloat.rs:61-88).
 *
 * F80_ZERO and F80_ONE are in cpu_state.h (they are also the reset-value constants);
 * the rest live here with the arithmetic they feed.
 * ------------------------------------------------------------------ */
#define F80_LN_10          ((f80_t){ .mantissa = 0x935D8DDDAAA8B000ull, .sign_exponent = 0x4000 })
#define F80_LN_2           ((f80_t){ .mantissa = 0xB17217F7D1CF7800ull, .sign_exponent = 0x3FFE })
#define F80_PI             ((f80_t){ .mantissa = 0xC90FDAA22168C000ull, .sign_exponent = 0x4000 })
#define F80_LOG2_E         ((f80_t){ .mantissa = 0xB8AA3B295C17F000ull, .sign_exponent = 0x3FFF })
#define F80_INDEFINITE_NAN ((f80_t){ .mantissa = 0xC000000000000000ull, .sign_exponent = 0x7FFF })
#define F80_POS_INFINITY   ((f80_t){ .mantissa = 0x8000000000000000ull, .sign_exponent = 0x7FFF })
#define F80_NEG_INFINITY   ((f80_t){ .mantissa = 0x8000000000000000ull, .sign_exponent = 0xFFFF })

/* ------------------------------------------------------------------ *
 * THE F80 API (softfloat.rs:90-211 + the operator impls).
 *
 * Naming: the Rust is `impl F80 { fn add(self, other: Self) -> Self }` etc., reached as
 * `a + b`; C has no operators, so every one is `f80_<name>`.  Values are passed BY VALUE
 * (f80_t is 16 bytes -- two eightbytes, no hidden allocation; the engine entry points take
 * the addresses of local copies).
 * ------------------------------------------------------------------ */

/* softfloat.rs:90-91 */
bool f80_sign(f80_t x);
int16_t f80_exponent(f80_t x);

/* softfloat.rs:93-125 */
f80_t f80_of_i32(int32_t src);
f80_t f80_of_i64(int64_t src);
f80_t f80_of_f32(uint32_t src);
f80_t f80_of_f64(uint64_t src);
f80_t f80_of_f64x(double src); /* softfloat.rs:115, private there, used by fpu.c's fpu.rs:142/493 */
int32_t f80_to_i32(f80_t x);
int64_t f80_to_i64(f80_t x);
/* softfloat.rs:117-118's `to_f32`/`to_f64`.  Named `_convert_` because fpu.c owns the
 * symbols `f80_to_f32`/`f80_to_f64` -- fpu.rs:104-116's `#[no_mangle]` wrappers, whose
 * names are part of upstream's wasm-surface. */
uint32_t f80_convert_to_f32(f80_t x);
uint64_t f80_convert_to_f64(f80_t x);
int32_t f80_truncate_to_i32(f80_t x);
int64_t f80_truncate_to_i64(f80_t x);

/* softfloat.rs:127-158 (the f64 libm ones cross into the host's libm on purpose: the
 * reference does the same, calling Rust's f64::cos/sin/... on the double-rounded value) */
f80_t f80_cos(f80_t x);
f80_t f80_sin(f80_t x);
f80_t f80_tan(f80_t x);
f80_t f80_atan(f80_t x);
f80_t f80_atan2(f80_t y, f80_t x);
f80_t f80_log2(f80_t x);
f80_t f80_ln(f80_t x);
f80_t f80_two_pow(f80_t x);
f80_t f80_round(f80_t x);
f80_t f80_trunc(f80_t x);
f80_t f80_sqrt(f80_t x);

/* softfloat.rs:136-141, 229-265 */
f80_t f80_abs(f80_t x);
f80_t f80_neg(f80_t x);
f80_t f80_add(f80_t x, f80_t y);
f80_t f80_sub(f80_t x, f80_t y);
f80_t f80_mul(f80_t x, f80_t y);
f80_t f80_div(f80_t x, f80_t y);
f80_t f80_rem(f80_t x, f80_t y);

/* softfloat.rs:160-167, 267-286.  The comparisons return a three-way-or-unordered value
 * where Rust returns `Option<Ordering>`: F80_LT / F80_EQ / F80_GT / F80_UNORDERED
 * (fpu.rs matches on the Option, so the C callers switch on this). */
enum { F80_CMP_LT = -1, F80_CMP_EQ = 0, F80_CMP_GT = 1, F80_CMP_UNORDERED = 2 };

bool f80_is_finite(f80_t x);
bool f80_is_nan(f80_t x);
bool f80_eq(f80_t x, f80_t y);
bool f80_lt(f80_t x, f80_t y);
bool f80_lt_quiet(f80_t x, f80_t y);
int f80_partial_cmp(f80_t x, f80_t y);       /* softfloat.rs:270-285 */
int f80_partial_cmp_quiet(f80_t x, f80_t y); /* softfloat.rs:196-210 */

/* softfloat.rs:169-187 */
void f80_set_rounding_mode(int mode); /* one of F80_ROUND_* */
void f80_set_precision(int precision); /* one of F80_PREC_* */

/* softfloat.rs:189-194.  get returns the BITS TRANSLATED to the x87 status-word
 * positions (that shift is upstream's, reproduced verbatim -- see softfloat.c). */
uint8_t f80_get_exception_flags(void);
void f80_clear_exception_flags(void);

#endif /* V86C_SOFTFLOAT_H */
