/*
 * arith.c -- the ALU semantics.  Port of v86/src/rust/cpu/arith.rs (1206 lines).
 *
 * Read this together with arith.h: the ownership boundary (the add/sub cores live in
 * flags.c), the `<<` porting convention, and the naming rule.  Every function below is
 * a transcription of the same-named upstream function; the only edits are the ones the
 * C language forces (left shifts on uint32_t, parenthesisation, and the two explicit
 * INT_MIN/-1 guards that Rust's `checked_div` gives for free).
 */

#include "arith.h"

#include "cpu_state.h"
#include "flags.h"
#include "memory.h"
#include "paging.h"

/* arith.rs:6 -- `31 - x.leading_zeros()`.  Zero is unreachable at both call sites
 * (bsf masks to the lowest set bit, bsr is guarded by != 0); leading_zeros() == 32
 * would make it -1, and that is what this returns for 0. */
static int32_t int_log2(int32_t x)
{
    uint32_t u = (uint32_t)x;
    int32_t r = 31;
    if (u == 0) {
        return -1;
    }
    while (((u >> r) & 1) == 0) {
        r--;
    }
    return r;
}

/* ------------------------------------------------------------------ *
 * the per-size public wrappers over the flags_* cores -- arith.rs:57-128
 * ------------------------------------------------------------------ */

int32_t add8(int32_t x, int32_t y)
{
    DBG_ASSERT(x >= 0 && x < 0x100);
    DBG_ASSERT(y >= 0 && y < 0x100);
    return flags_add(x, y, OPSIZE_8);
}
int32_t add16(int32_t x, int32_t y)
{
    DBG_ASSERT(x >= 0 && x < 0x10000);
    DBG_ASSERT(y >= 0 && y < 0x10000);
    return flags_add(x, y, OPSIZE_16);
}
int32_t add32(int32_t x, int32_t y) { return flags_add(x, y, OPSIZE_32); }
int32_t sub8(int32_t x, int32_t y) { return flags_sub(x, y, OPSIZE_8); }
int32_t sub16(int32_t x, int32_t y) { return flags_sub(x, y, OPSIZE_16); }
int32_t sub32(int32_t x, int32_t y) { return flags_sub(x, y, OPSIZE_32); }
int32_t adc8(int32_t x, int32_t y) { return flags_adc(x, y, OPSIZE_8); }
int32_t adc16(int32_t x, int32_t y) { return flags_adc(x, y, OPSIZE_16); }
int32_t adc32(int32_t x, int32_t y) { return flags_adc(x, y, OPSIZE_32); }
int32_t sbb8(int32_t x, int32_t y) { return flags_sbb(x, y, OPSIZE_8); }
int32_t sbb16(int32_t x, int32_t y) { return flags_sbb(x, y, OPSIZE_16); }
int32_t sbb32(int32_t x, int32_t y) { return flags_sbb(x, y, OPSIZE_32); }

void cmp8(int32_t x, int32_t y)
{
    DBG_ASSERT(x >= 0 && x < 0x100);
    DBG_ASSERT(y >= 0 && y < 0x100);
    flags_sub(x, y, OPSIZE_8);
}
void cmp16(int32_t x, int32_t y)
{
    DBG_ASSERT(x >= 0 && x < 0x10000);
    DBG_ASSERT(y >= 0 && y < 0x10000);
    flags_sub(x, y, OPSIZE_16);
}
void cmp32(int32_t x, int32_t y) { flags_sub(x, y, OPSIZE_32); }

int32_t inc8(int32_t x) { return flags_inc(x, OPSIZE_8); }
int32_t inc16(int32_t x) { return flags_inc(x, OPSIZE_16); }
int32_t inc32(int32_t x) { return flags_inc(x, OPSIZE_32); }
int32_t dec8(int32_t x) { return flags_dec(x, OPSIZE_8); }
int32_t dec16(int32_t x) { return flags_dec(x, OPSIZE_16); }
int32_t dec32(int32_t x) { return flags_dec(x, OPSIZE_32); }

int32_t not8(int32_t x)
{
    /* upstream is `!x` in RUST, where `!` is the BITWISE complement (C's `~`).
     * C's `!` is logical-not -- writing it here would make `not al` produce 0/1. */
    return ~x;
}
int32_t neg8(int32_t x) { return flags_neg(x, OPSIZE_8); }
int32_t neg16(int32_t x) { return flags_neg(x, OPSIZE_16); }
int32_t neg32(int32_t x) { return flags_neg(x, OPSIZE_32); }

/* ------------------------------------------------------------------ *
 * mul / imul -- arith.rs:130-272
 * ------------------------------------------------------------------ */

void mul8(int32_t source_operand)
{
    int32_t result = source_operand * read_reg8(AL);
    write_reg16(AX, result);
    cpu->last_result = result & 255;
    cpu->last_op_size = OPSIZE_8;
    /* upstream compares the SIGNED product against 256: a negative product (a source
     * with the high bit set) therefore takes the else arm and sets CF/OF */
    if (result < 256) {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    else {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
}

void imul8(int32_t source_operand)
{
    int32_t result = source_operand * (((int32_t)((uint32_t)read_reg8(AL) << 24)) >> 24);
    write_reg16(AX, result);
    cpu->last_result = result & 255;
    cpu->last_op_size = OPSIZE_8;
    if (result > 127 || result < -128) {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    else {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
}

void mul16(uint32_t source_operand)
{
    uint32_t result = source_operand * (uint32_t)read_reg16(AX);
    uint32_t high_result = result >> 16;
    write_reg16(AX, (int32_t)result);
    write_reg16(DX, (int32_t)high_result);
    cpu->last_result = (int32_t)(result & 0xFFFF);
    cpu->last_op_size = OPSIZE_16;
    if (high_result == 0) {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    else {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
}

void imul16(int32_t source_operand)
{
    int32_t result = source_operand * (((int32_t)((uint32_t)read_reg16(AX) << 16)) >> 16);
    write_reg16(AX, result);
    write_reg16(DX, result >> 16);
    cpu->last_result = result & 0xFFFF;
    cpu->last_op_size = OPSIZE_16;
    if (result > 32767 || result < -32768) {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    else {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
}

int32_t imul_reg16(int32_t operand1, int32_t operand2)
{
    operand1 = ((int32_t)((uint32_t)operand1 << 16)) >> 16;
    operand2 = ((int32_t)((uint32_t)operand2 << 16)) >> 16;
    int32_t result = operand1 * operand2;
    cpu->last_result = result & 0xFFFF;
    cpu->last_op_size = OPSIZE_16;
    if (result > 32767 || result < -32768) {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    else {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    return result;
}

void mul32(int32_t source_operand)
{
    int32_t dest_operand = read_reg32(EAX);
    uint64_t result = (uint64_t)(uint32_t)dest_operand * (uint64_t)(uint32_t)source_operand;
    int32_t result_low = (int32_t)result;
    int32_t result_high = (int32_t)(result >> 32);
    write_reg32(EAX, result_low);
    write_reg32(EDX, result_high);
    cpu->last_result = result_low;
    cpu->last_op_size = OPSIZE_32;
    if (result_high == 0) {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    else {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
}

void imul32(int32_t source_operand)
{
    int32_t dest_operand = read_reg32(EAX);
    int64_t result = (int64_t)dest_operand * (int64_t)source_operand;
    int32_t result_low = (int32_t)result;
    int32_t result_high = (int32_t)((uint64_t)result >> 32);
    write_reg32(EAX, result_low);
    write_reg32(EDX, result_high);
    cpu->last_result = result_low;
    cpu->last_op_size = OPSIZE_32;
    if (result_high == result_low >> 31) {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    else {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
}

int32_t imul_reg32(int32_t operand1, int32_t operand2)
{
    int64_t result = (int64_t)operand1 * (int64_t)operand2;
    int32_t result_low = (int32_t)result;
    int32_t result_high = (int32_t)((uint64_t)result >> 32);
    cpu->last_result = result_low;
    cpu->last_op_size = OPSIZE_32;
    if (result_high == result_low >> 31) {
        cpu->flags &= ~1 & ~FLAG_OVERFLOW;
    }
    else {
        cpu->flags |= 1 | FLAG_OVERFLOW;
    }
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    return result_low;
}

int32_t xadd8(int32_t source_operand, int32_t reg)
{
    int32_t tmp = read_reg8(reg);
    write_reg8(reg, source_operand);
    return flags_add(source_operand, tmp, OPSIZE_8);
}
int32_t xadd16(int32_t source_operand, int32_t reg)
{
    int32_t tmp = read_reg16(reg);
    write_reg16(reg, source_operand);
    return flags_add(source_operand, tmp, OPSIZE_16);
}
int32_t xadd32(int32_t source_operand, int32_t reg)
{
    int32_t tmp = read_reg32(reg);
    write_reg32(reg, source_operand);
    return flags_add(source_operand, tmp, OPSIZE_32);
}

int32_t cmpxchg8(int32_t data, int32_t r)
{
    cmp8(read_reg8(AL), data);
    if (getzf()) {
        return read_reg8(r);
    }
    else {
        write_reg8(AL, data);
        return data;
    }
}
int32_t cmpxchg16(int32_t data, int32_t r)
{
    cmp16(read_reg16(AX), data);
    if (getzf()) {
        return read_reg16(r);
    }
    else {
        write_reg16(AX, data);
        return data;
    }
}
int32_t cmpxchg32(int32_t data, int32_t r)
{
    cmp32(read_reg32(EAX), data);
    if (getzf()) {
        return read_reg32(r);
    }
    else {
        write_reg32(EAX, data);
        return data;
    }
}

/* ------------------------------------------------------------------ *
 * BCD -- arith.rs:307-399
 * ------------------------------------------------------------------ */

void bcd_daa(void)
{
    int32_t old_al = read_reg8(AL);
    bool old_cf = getcf();
    bool old_af = getaf();
    cpu->flags &= ~1 & ~FLAG_ADJUST;
    if ((old_al & 15) > 9 || old_af) {
        write_reg8(AL, read_reg8(AL) + 6);
        cpu->flags |= FLAG_ADJUST;
    }
    if (old_al > 153 || old_cf) {
        write_reg8(AL, read_reg8(AL) + 96);
        cpu->flags |= 1;
    }
    cpu->last_result = read_reg8(AL);
    cpu->last_op_size = OPSIZE_8;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_ADJUST & ~FLAG_OVERFLOW;
}

void bcd_das(void)
{
    int32_t old_al = read_reg8(AL);
    bool old_cf = getcf();
    cpu->flags &= ~1;
    if ((old_al & 15) > 9 || getaf()) {
        write_reg8(AL, read_reg8(AL) - 6);
        cpu->flags |= FLAG_ADJUST;
        cpu->flags = (cpu->flags & ~1) | (old_cf ? 1 : 0) | (old_al < 6 ? 1 : 0);
    }
    else {
        cpu->flags &= ~FLAG_ADJUST;
    }
    if (old_al > 153 || old_cf) {
        write_reg8(AL, read_reg8(AL) - 96);
        cpu->flags |= 1;
    }
    cpu->last_result = read_reg8(AL);
    cpu->last_op_size = OPSIZE_8;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_ADJUST & ~FLAG_OVERFLOW;
}

void bcd_aad(int32_t imm8)
{
    int32_t result = read_reg8(AL) + read_reg8(AH) * imm8;
    cpu->last_result = result & 255;
    write_reg16(AX, cpu->last_result);
    cpu->last_op_size = OPSIZE_8;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_ADJUST & ~FLAG_OVERFLOW;
    cpu->flags &= ~1 & ~FLAG_ADJUST & ~FLAG_OVERFLOW;
    if (result > 0xFFFF) {
        cpu->flags |= 1;
    }
}

void bcd_aam(int32_t imm8)
{
    /* ascii adjust after multiplication */
    if (imm8 == 0) {
        trigger_de();
    }
    else {
        int32_t temp = read_reg8(AL);
        write_reg8(AH, temp / imm8);
        write_reg8(AL, temp % imm8);
        cpu->last_result = read_reg8(AL);
        cpu->last_op_size = OPSIZE_8;
        cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_ADJUST & ~FLAG_OVERFLOW;
        cpu->flags &= ~1 & ~FLAG_ADJUST & ~FLAG_OVERFLOW;
    }
}

void bcd_aaa(void)
{
    if ((read_reg8(AL) & 15) > 9 || getaf()) {
        write_reg16(AX, read_reg16(AX) + 6);
        write_reg8(AH, read_reg8(AH) + 1);
        cpu->flags |= FLAG_ADJUST | 1;
    }
    else {
        cpu->flags &= ~FLAG_ADJUST & ~1;
    }
    write_reg8(AL, read_reg8(AL) & 15);
    cpu->flags_changed &= ~FLAG_ADJUST & ~1;
}

void bcd_aas(void)
{
    if ((read_reg8(AL) & 15) > 9 || getaf()) {
        write_reg16(AX, read_reg16(AX) - 6);
        write_reg8(AH, read_reg8(AH) - 1);
        cpu->flags |= FLAG_ADJUST | 1;
    }
    else {
        cpu->flags &= ~FLAG_ADJUST & ~1;
    }
    write_reg8(AL, read_reg8(AL) & 15);
    cpu->flags_changed &= ~FLAG_ADJUST & ~1;
}

/* ------------------------------------------------------------------ *
 * and / or / xor -- arith.rs:400-438
 *
 * Upstream's private cores are `and`/`or`/`xor`, which are C keywords; the `arith_`
 * prefix is the whole rename.  Note that (like upstream) they do NOT mask to op_size.
 * ------------------------------------------------------------------ */

static int32_t arith_and(int32_t dest_operand, int32_t source_operand, int32_t op_size)
{
    int32_t result = dest_operand & source_operand;
    cpu->last_result = result;
    cpu->last_op_size = op_size;
    cpu->flags &= ~1 & ~FLAG_OVERFLOW & ~FLAG_ADJUST;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW & ~FLAG_ADJUST;
    return result;
}
static int32_t arith_or(int32_t dest_operand, int32_t source_operand, int32_t op_size)
{
    int32_t result = dest_operand | source_operand;
    cpu->last_result = result;
    cpu->last_op_size = op_size;
    cpu->flags &= ~1 & ~FLAG_OVERFLOW & ~FLAG_ADJUST;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW & ~FLAG_ADJUST;
    return result;
}
static int32_t arith_xor(int32_t dest_operand, int32_t source_operand, int32_t op_size)
{
    int32_t result = dest_operand ^ source_operand;
    cpu->last_result = result;
    cpu->last_op_size = op_size;
    cpu->flags &= ~1 & ~FLAG_OVERFLOW & ~FLAG_ADJUST;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW & ~FLAG_ADJUST;
    return result;
}

int32_t and8(int32_t x, int32_t y) { return arith_and(x, y, OPSIZE_8); }
int32_t and16(int32_t x, int32_t y) { return arith_and(x, y, OPSIZE_16); }
int32_t and32(int32_t x, int32_t y) { return arith_and(x, y, OPSIZE_32); }
void test8(int32_t x, int32_t y) { arith_and(x, y, OPSIZE_8); }
void test16(int32_t x, int32_t y) { arith_and(x, y, OPSIZE_16); }
void test32(int32_t x, int32_t y) { arith_and(x, y, OPSIZE_32); }
int32_t or8(int32_t x, int32_t y) { return arith_or(x, y, OPSIZE_8); }
int32_t or16(int32_t x, int32_t y) { return arith_or(x, y, OPSIZE_16); }
int32_t or32(int32_t x, int32_t y) { return arith_or(x, y, OPSIZE_32); }
int32_t xor8(int32_t x, int32_t y) { return arith_xor(x, y, OPSIZE_8); }
int32_t xor16(int32_t x, int32_t y) { return arith_xor(x, y, OPSIZE_16); }
int32_t xor32(int32_t x, int32_t y) { return arith_xor(x, y, OPSIZE_32); }

/* ------------------------------------------------------------------ *
 * rotate -- arith.rs:440-637
 *
 * Every `<<` below is on uint32_t (arith.h's convention); every `>>` is upstream's
 * signed (arithmetic) shift.
 * ------------------------------------------------------------------ */

int32_t rol8(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    count &= 7;
    int32_t result = (int32_t)(((uint32_t)dest_operand << count) |
                               (uint32_t)(dest_operand >> (8 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result & 1) |
                 ((((uint32_t)result << 11) ^ ((uint32_t)result << 4)) & FLAG_OVERFLOW);
    return result & 0xFF;
}

int32_t rol16(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    count &= 15;
    int32_t result = (int32_t)(((uint32_t)dest_operand << count) |
                               (uint32_t)(dest_operand >> (16 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result & 1) |
                 ((((uint32_t)result << 11) ^ (uint32_t)(result >> 4)) & FLAG_OVERFLOW);
    return result & 0xFFFF;
}

int32_t rol32(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result =
        (int32_t)(((uint32_t)dest_operand << count) | ((uint32_t)dest_operand >> (32 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result & 1) |
                 ((((uint32_t)result << 11) ^ (uint32_t)(result >> 20)) & FLAG_OVERFLOW);
    return result;
}

int32_t rcl8(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    count %= 9;
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)(((uint32_t)dest_operand << count) |
                               ((uint32_t)(getcf() ? 1 : 0) << (count - 1)) |
                               (uint32_t)(dest_operand >> (9 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 8 & 1) |
                 ((((uint32_t)result << 3) ^ ((uint32_t)result << 4)) & FLAG_OVERFLOW);
    return result & 0xFF;
}

int32_t rcl16(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    count %= 17;
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)(((uint32_t)dest_operand << count) |
                               ((uint32_t)(getcf() ? 1 : 0) << (count - 1)) |
                               (uint32_t)(dest_operand >> (17 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 16 & 1) |
                 (((uint32_t)(result >> 5) ^ (uint32_t)(result >> 4)) & FLAG_OVERFLOW);
    return result & 0xFFFF;
}

int32_t rcl32(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)(((uint32_t)dest_operand << count) |
                               ((uint32_t)(getcf() ? 1 : 0) << (count - 1)));
    if (count > 1) {
        result = (int32_t)((uint32_t)result | ((uint32_t)dest_operand >> (33 - count)));
    }
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    /* upstream extracts CF from dest_operand (bit 32-count), not from result */
    int32_t b = (int32_t)((uint32_t)dest_operand >> (32 - count) & 1);
    cpu->flags = ((cpu->flags & ~1 & ~FLAG_OVERFLOW) | b) |
                 (((b << 11) ^ (uint32_t)(result >> 20)) & FLAG_OVERFLOW);
    return result;
}

int32_t ror8(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    count &= 7;
    int32_t result = (int32_t)((uint32_t)(dest_operand >> count) |
                               ((uint32_t)dest_operand << (8 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 7 & 1) |
                 ((((uint32_t)result << 4) ^ ((uint32_t)result << 5)) & FLAG_OVERFLOW);
    return result & 0xFF;
}

int32_t ror16(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    count &= 15;
    int32_t result = (int32_t)((uint32_t)(dest_operand >> count) |
                               ((uint32_t)dest_operand << (16 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 15 & 1) |
                 (((uint32_t)(result >> 4) ^ (uint32_t)(result >> 3)) & FLAG_OVERFLOW);
    return result & 0xFFFF;
}

int32_t ror32(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result =
        (int32_t)(((uint32_t)dest_operand >> count) | ((uint32_t)dest_operand << (32 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 31 & 1) |
                 (((uint32_t)(result >> 20) ^ (uint32_t)(result >> 19)) & FLAG_OVERFLOW);
    return result;
}

int32_t rcr8(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    count %= 9;
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)((uint32_t)(dest_operand >> count) |
                               ((uint32_t)(getcf() ? 1 : 0) << (8 - count)) |
                               ((uint32_t)dest_operand << (9 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 8 & 1) |
                 ((((uint32_t)result << 4) ^ ((uint32_t)result << 5)) & FLAG_OVERFLOW);
    return result & 0xFF;
}

int32_t rcr16(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    count %= 17;
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)((uint32_t)(dest_operand >> count) |
                               ((uint32_t)(getcf() ? 1 : 0) << (16 - count)) |
                               ((uint32_t)dest_operand << (17 - count)));
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 16 & 1) |
                 (((uint32_t)(result >> 4) ^ (uint32_t)(result >> 3)) & FLAG_OVERFLOW);
    return result & 0xFFFF;
}

int32_t rcr32(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)(((uint32_t)dest_operand >> count) |
                               ((uint32_t)(getcf() ? 1 : 0) << (32 - count)));
    if (count > 1) {
        result = (int32_t)((uint32_t)result | ((uint32_t)dest_operand << (33 - count)));
    }
    cpu->flags_changed &= ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (dest_operand >> (count - 1) & 1) |
                 (((uint32_t)(result >> 20) ^ (uint32_t)(result >> 19)) & FLAG_OVERFLOW);
    return result;
}

/* ------------------------------------------------------------------ *
 * div / idiv -- arith.rs:638-753
 *
 * Rust's `checked_div` returns None for a zero divisor AND for INT_MIN / -1; C's
 * division is UB in the second case, so both are spelled out.
 * ------------------------------------------------------------------ */

void div8(uint32_t source_operand)
{
    if (source_operand == 0) {
        trigger_de();
        return;
    }
    uint32_t target_operand = (uint32_t)read_reg16(AX);
    uint32_t result = target_operand / source_operand;
    if (result >= 0x100) {
        trigger_de();
        return;
    }
    write_reg8(AL, (int32_t)result);
    write_reg8(AH, (int32_t)(target_operand % source_operand));
}

void idiv8(int32_t source_operand)
{
    if (source_operand == 0) {
        trigger_de();
        return;
    }
    int32_t target_operand = ((int32_t)((uint32_t)read_reg16(AX) << 16)) >> 16;
    int32_t result = target_operand / source_operand;
    if (result >= 0x80 || result < -0x80) {
        trigger_de();
        return;
    }
    write_reg8(AL, result);
    write_reg8(AH, target_operand % source_operand);
}

bool div16_without_fault(uint32_t source_operand)
{
    uint32_t target_operand = (uint32_t)(read_reg16(AX) | read_reg16(DX) << 16);
    if (source_operand == 0) {
        return false;
    }
    uint32_t result = target_operand / source_operand;
    if (result >= 0x10000) {
        return false;
    }
    write_reg16(AX, (int32_t)result);
    write_reg16(DX, (int32_t)(target_operand % source_operand));
    return true;
}

void div16(uint32_t source_operand)
{
    if (!div16_without_fault(source_operand)) {
        trigger_de();
    }
}

bool idiv16_without_fault(int32_t source_operand)
{
    int32_t target_operand = read_reg16(AX) | read_reg16(DX) << 16;
    if (source_operand == 0 || (target_operand == INT32_MIN && source_operand == -1)) {
        return false; /* checked_div -> None (zero divisor or the INT_MIN/-1 overflow) */
    }
    int32_t result = target_operand / source_operand;
    if (result >= 0x8000 || result < -0x8000) {
        return false;
    }
    write_reg16(AX, result);
    write_reg16(DX, target_operand % source_operand);
    return true;
}

void idiv16(int32_t source_operand)
{
    if (!idiv16_without_fault(source_operand)) {
        trigger_de();
    }
}

bool div32_without_fault(uint32_t source_operand)
{
    uint64_t source = (uint64_t)source_operand;
    uint32_t target_low = (uint32_t)read_reg32(EAX);
    uint32_t target_high = (uint32_t)read_reg32(EDX);
    uint64_t target_operand = ((uint64_t)target_high << 32) | (uint64_t)target_low;
    if (source == 0) {
        return false;
    }
    uint64_t result = target_operand / source;
    if (result > 0xFFFFFFFF) {
        return false;
    }
    uint64_t modulo = target_operand % source;
    write_reg32(EAX, (int32_t)result);
    write_reg32(EDX, (int32_t)modulo);
    return true;
}

void div32(uint32_t source_operand)
{
    if (!div32_without_fault(source_operand)) {
        trigger_de();
    }
}

bool idiv32_without_fault(int32_t source_operand)
{
    int64_t source = (int64_t)source_operand;
    uint32_t target_low = (uint32_t)read_reg32(EAX);
    uint32_t target_high = (uint32_t)read_reg32(EDX);
    /* upstream shifts in i64 (`(target_high as i64) << 32`), which overflows the sign
     * bit for a high word >= 2^31 -- the same bit pattern, without C's UB */
    int64_t target_operand = (int64_t)(((uint64_t)target_high << 32) | (uint64_t)target_low);
    if (source == 0 || (target_operand == INT64_MIN && source == -1)) {
        return false;
    }
    int64_t result = target_operand / source;
    if (result < -0x80000000LL || result > 0x7FFFFFFFLL) {
        return false;
    }
    int64_t modulo = target_operand % source;
    write_reg32(EAX, (int32_t)result);
    write_reg32(EDX, (int32_t)modulo);
    return true;
}

void idiv32(int32_t source_operand)
{
    if (!idiv32_without_fault(source_operand)) {
        trigger_de();
    }
}

/* ------------------------------------------------------------------ *
 * shift -- arith.rs:755-996
 * ------------------------------------------------------------------ */

int32_t shl8(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)((uint32_t)dest_operand << count);
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_8;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 8 & 1) |
                 ((((uint32_t)result << 3) ^ ((uint32_t)result << 4)) & FLAG_OVERFLOW);
    return result & 0xFF;
}

int32_t shl16(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)((uint32_t)dest_operand << count);
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_16;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result >> 16 & 1) |
                 (((uint32_t)(result >> 5) ^ (uint32_t)(result >> 4)) & FLAG_OVERFLOW);
    return result & 0xFFFF;
}

int32_t shl32(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)((uint32_t)dest_operand << count);
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_32;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    int32_t b = dest_operand >> (32 - count) & 1;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | b |
                 (((b ^ (result >> 31)) << 11) & FLAG_OVERFLOW);
    return result;
}

int32_t shr8(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = dest_operand >> count;
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_8;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (dest_operand >> (count - 1) & 1) |
                 (((dest_operand >> 7 & 1) << 11) & FLAG_OVERFLOW);
    return result;
}

int32_t shr16(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = dest_operand >> count;
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_16;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (dest_operand >> (count - 1) & 1) |
                 (dest_operand >> 4 & FLAG_OVERFLOW);
    return result;
}

int32_t shr32(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)((uint32_t)dest_operand >> count);
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_32;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) |
                 (int32_t)(((uint32_t)dest_operand >> (count - 1)) & 1) |
                 ((dest_operand >> 20) & FLAG_OVERFLOW);
    return result;
}

int32_t sar8(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result;
    if (count < 8) {
        result = ((int32_t)((uint32_t)dest_operand << 24)) >> (count + 24);
        /* of is zero */
        cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (dest_operand >> (count - 1) & 1);
    }
    else {
        result = ((int32_t)((uint32_t)dest_operand << 24)) >> 31;
        cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result & 1);
    }
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_8;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    return result & 0xFF;
}

int32_t sar16(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result;
    if (count < 16) {
        result = ((int32_t)((uint32_t)dest_operand << 16)) >> (count + 16);
        cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (dest_operand >> (count - 1) & 1);
    }
    else {
        result = ((int32_t)((uint32_t)dest_operand << 16)) >> 31;
        cpu->flags = (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (result & 1);
    }
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_16;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    return result & 0xFFFF;
}

int32_t sar32(int32_t dest_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = dest_operand >> count;
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_32;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags =
        (cpu->flags & ~1 & ~FLAG_OVERFLOW) | (int32_t)(((uint32_t)dest_operand >> (count - 1)) & 1);
    return result;
}

int32_t shrd16(int32_t dest_operand, int32_t source_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result;
    if (count <= 16) {
        result = (int32_t)((uint32_t)(dest_operand >> count) |
                           ((uint32_t)source_operand << (16 - count)));
        cpu->flags = (cpu->flags & ~1) | (dest_operand >> (count - 1) & 1);
    }
    else {
        result = (int32_t)(((uint32_t)dest_operand << (32 - count)) |
                           (uint32_t)(source_operand >> (count - 16)));
        cpu->flags = (cpu->flags & ~1) | (source_operand >> (count - 17) & 1);
    }
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_16;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~FLAG_OVERFLOW) |
                 (((uint32_t)(result ^ dest_operand) >> 4) & FLAG_OVERFLOW);
    return result & 0xFFFF;
}

int32_t shrd32(int32_t dest_operand, int32_t source_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)(((uint32_t)dest_operand >> count) |
                               ((uint32_t)source_operand << (32 - count)));
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_32;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = ((cpu->flags & ~1 & ~FLAG_OVERFLOW) |
                  (int32_t)(((uint32_t)dest_operand >> (count - 1)) & 1)) |
                 ((uint32_t)(result ^ dest_operand) >> 20 & FLAG_OVERFLOW);
    return result;
}

int32_t shld16(int32_t dest_operand, int32_t source_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result;
    if (count <= 16) {
        result = (int32_t)(((uint32_t)dest_operand << count) |
                           ((uint32_t)source_operand >> (16 - count)));
        cpu->flags = (cpu->flags & ~1) |
                     (int32_t)(((uint32_t)dest_operand >> (16 - count)) & 1);
    }
    else {
        result = (int32_t)((uint32_t)(dest_operand >> (32 - count)) |
                           ((uint32_t)source_operand << (count - 16)));
        cpu->flags =
            (cpu->flags & ~1) | (int32_t)(((uint32_t)source_operand >> (32 - count)) & 1);
    }
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_16;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~FLAG_OVERFLOW) | (((cpu->flags & 1) ^ (result >> 15 & 1)) << 11);
    return result & 0xFFFF;
}

int32_t shld32(int32_t dest_operand, int32_t source_operand, int32_t count)
{
    DBG_ASSERT(count >= 0 && count < 32);
    if (count == 0) {
        return dest_operand;
    }
    int32_t result = (int32_t)(((uint32_t)dest_operand << count) |
                               ((uint32_t)source_operand >> (32 - count)));
    cpu->last_result = result;
    cpu->last_op_size = OPSIZE_32;
    cpu->flags_changed = FLAGS_ALL & ~1 & ~FLAG_OVERFLOW;
    cpu->flags = (cpu->flags & ~1) | (int32_t)(((uint32_t)dest_operand >> (32 - count)) & 1);
    if (count == 1) {
        cpu->flags = (cpu->flags & ~FLAG_OVERFLOW) | (((cpu->flags & 1) ^ (result >> 31 & 1)) << 11);
    }
    else {
        cpu->flags &= ~FLAG_OVERFLOW;
    }
    return result;
}

/* ------------------------------------------------------------------ *
 * bit test -- arith.rs:998-1047
 * ------------------------------------------------------------------ */

void bt_reg(int32_t bit_base, int32_t bit_offset)
{
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
}
int32_t btc_reg(int32_t bit_base, int32_t bit_offset)
{
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
    /* the register forms are always called with bit_offset masked to 15/31 by the
     * caller (instructions_0f.rs:3420-3422), but `1 << 31` is still UB on signed int */
    return bit_base ^ (int32_t)(1u << bit_offset);
}
int32_t bts_reg(int32_t bit_base, int32_t bit_offset)
{
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
    return bit_base | (int32_t)(1u << bit_offset);
}
int32_t btr_reg(int32_t bit_base, int32_t bit_offset)
{
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
    return bit_base & ~(int32_t)(1u << bit_offset);
}

void bt_mem(int32_t virt_addr, int32_t bit_offset)
{
    int32_t bit_base = safe_read8(virt_addr + (bit_offset >> 3));
    if (cpu->pagefault) {
        return;
    }
    bit_offset &= 7;
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
}
void btc_mem(int32_t virt_addr, int32_t bit_offset)
{
    uint32_t phys_addr = translate_address_write(virt_addr + (bit_offset >> 3));
    if (cpu->pagefault) {
        return;
    }
    int32_t bit_base = read8(phys_addr);
    bit_offset &= 7;
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
    write8(phys_addr, bit_base ^ (int32_t)(1u << bit_offset));
}
void btr_mem(int32_t virt_addr, int32_t bit_offset)
{
    uint32_t phys_addr = translate_address_write(virt_addr + (bit_offset >> 3));
    if (cpu->pagefault) {
        return;
    }
    int32_t bit_base = read8(phys_addr);
    bit_offset &= 7;
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
    write8(phys_addr, bit_base & ~(int32_t)(1u << bit_offset));
}
void bts_mem(int32_t virt_addr, int32_t bit_offset)
{
    uint32_t phys_addr = translate_address_write(virt_addr + (bit_offset >> 3));
    if (cpu->pagefault) {
        return;
    }
    int32_t bit_base = read8(phys_addr);
    bit_offset &= 7;
    cpu->flags = (cpu->flags & ~1) | (bit_base >> bit_offset & 1);
    cpu->flags_changed &= ~1;
    write8(phys_addr, bit_base | (int32_t)(1u << bit_offset));
}

/* ------------------------------------------------------------------ *
 * bit scan / popcount -- arith.rs:1049-1125
 * ------------------------------------------------------------------ */

int32_t bsf16(int32_t old, int32_t bit_base)
{
    cpu->flags_changed = FLAGS_ALL & ~FLAG_ZERO & ~FLAG_CARRY;
    cpu->flags &= ~FLAG_CARRY;
    cpu->last_op_size = OPSIZE_16;
    if (bit_base == 0) {
        cpu->flags |= FLAG_ZERO;
        cpu->last_result = bit_base;
        /* not defined in the docs, but the value doesn't change on my intel machine */
        return old;
    }
    else {
        cpu->flags &= ~FLAG_ZERO;
        cpu->last_result = int_log2(-bit_base & bit_base);
        return cpu->last_result;
    }
}
int32_t bsf32(int32_t old, int32_t bit_base)
{
    cpu->flags_changed = FLAGS_ALL & ~FLAG_ZERO & ~FLAG_CARRY;
    cpu->flags &= ~FLAG_CARRY;
    cpu->last_op_size = OPSIZE_32;
    if (bit_base == 0) {
        cpu->flags |= FLAG_ZERO;
        cpu->last_result = bit_base;
        return old;
    }
    else {
        cpu->flags &= ~FLAG_ZERO;
        cpu->last_result = int_log2(-bit_base & bit_base);
        return cpu->last_result;
    }
}
int32_t bsr16(int32_t old, int32_t bit_base)
{
    cpu->flags_changed = FLAGS_ALL & ~FLAG_ZERO & ~FLAG_CARRY;
    cpu->flags &= ~FLAG_CARRY;
    cpu->last_op_size = OPSIZE_16;
    if (bit_base == 0) {
        cpu->flags |= FLAG_ZERO;
        cpu->last_result = bit_base;
        return old;
    }
    else {
        cpu->flags &= ~FLAG_ZERO;
        cpu->last_result = int_log2(bit_base);
        return cpu->last_result;
    }
}
int32_t bsr32(int32_t old, int32_t bit_base)
{
    cpu->flags_changed = FLAGS_ALL & ~FLAG_ZERO & ~FLAG_CARRY;
    cpu->flags &= ~FLAG_CARRY;
    cpu->last_op_size = OPSIZE_32;
    if (bit_base == 0) {
        cpu->flags |= FLAG_ZERO;
        cpu->last_result = bit_base;
        return old;
    }
    else {
        cpu->flags &= ~FLAG_ZERO;
        cpu->last_result = int_log2(bit_base);
        return cpu->last_result;
    }
}

int32_t popcnt(int32_t v)
{
    cpu->flags_changed = 0;
    cpu->flags &= ~FLAGS_ALL;
    if (v != 0) {
        return __builtin_popcount((uint32_t)v);
    }
    else {
        cpu->flags |= FLAG_ZERO;
        return 0;
    }
}

/* ------------------------------------------------------------------ *
 * the saturating pack helpers -- arith.rs:1127-1206
 * ------------------------------------------------------------------ */

uint8_t saturate_sw_to_ub(uint16_t v)
{
    uint16_t ret = v;
    if (ret >= 32768) {
        ret = 0;
    }
    else if (ret > 255) {
        ret = 255;
    }
    return (uint8_t)ret;
}

uint8_t saturate_sw_to_sb(int32_t v)
{
    DBG_ASSERT(((uint32_t)v & 0xFFFF0000u) == 0);
    int32_t ret = v;
    if (ret > 65408) {
        ret = ret & 255;
    }
    else if (ret > 32767) {
        ret = 128;
    }
    else if (ret > 127) {
        ret = 127;
    }
    DBG_ASSERT(((uint32_t)ret & 0xFFFFFF00u) == 0);
    return (uint8_t)ret;
}

uint16_t saturate_sd_to_sw(uint32_t v)
{
    uint32_t ret = v;
    if (ret > 4294934528u) {
        ret = ret & 0xFFFF;
    }
    else if (ret > 0x7FFFFFFFu) {
        ret = 32768;
    }
    else if (ret > 32767) {
        ret = 32767;
    }
    DBG_ASSERT((ret & 0xFFFF0000u) == 0);
    return (uint16_t)ret;
}

int8_t saturate_sd_to_sb(uint32_t v)
{
    uint32_t ret = v;
    if (ret > 0xFFFFFF80u) {
        ret = ret & 255;
    }
    else if (ret > 0x7FFFFFFFu) {
        ret = 128;
    }
    else if (ret > 127) {
        ret = 127;
    }
    DBG_ASSERT((ret & 0xFFFFFF00u) == 0);
    return (int8_t)ret;
}

int32_t saturate_sd_to_ub(int32_t v)
{
    int32_t ret = v;
    if (ret < 0) {
        ret = 0;
    }
    DBG_ASSERT(((uint32_t)ret & 0xFFFFFF00u) == 0);
    return ret;
}

uint8_t saturate_ud_to_ub(uint32_t v)
{
    uint32_t ret = v;
    if (ret > 255) {
        ret = 255;
    }
    DBG_ASSERT((ret & 0xFFFFFF00u) == 0);
    return (uint8_t)ret;
}

uint16_t saturate_uw(uint32_t v)
{
    uint32_t ret = v;
    if (ret > 0x7FFFFFFFu) {
        ret = 0;
    }
    else if (ret > 0xFFFF) {
        ret = 0xFFFF;
    }
    DBG_ASSERT((ret & 0xFFFF0000u) == 0);
    return (uint16_t)ret;
}
