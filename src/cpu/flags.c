/*
 * flags.c -- the lazy-flag getters and setters.
 *
 * Ported verbatim from v86/src/rust/cpu/misc_instr.rs:8-66 (getters), arith.rs:12-103
 * (the add/adc/sub/sbb/inc/dec cores) and cpu.rs:1911-1918 / 4478-4520 (eflags in/out).
 * The two dbg_assert!s in getcf (misc_instr.rs:10-11) become DBG_ASSERT.
 */

#include "flags.h"

/* ------------------------------------------------------------------ *
 * getters -- misc_instr.rs:8-66
 * ------------------------------------------------------------------ */

bool getcf(void)
{
    if (cpu->flags_changed & 1) {
        int32_t m = opsize_to_mask(cpu->last_op_size);
        /* misc_instr.rs:10-11: last_op1/last_result must fit the last op size, i.e.
         * "a carry into the flags from beyond the operand" is a porting bug. */
        DBG_ASSERT((uint32_t)cpu->last_op1 <= (uint32_t)m);
        DBG_ASSERT((uint32_t)cpu->last_result <= (uint32_t)m);

        int32_t sub_mask = cpu->flags_changed >> 31;

        /* sub: last_op1 < last_result;  add: last_result < last_op1 (misc_instr.rs:16-21) */
        return (uint32_t)(cpu->last_result ^ sub_mask) < (uint32_t)(cpu->last_op1 ^ sub_mask);
    }
    else {
        return (cpu->flags & 1) != 0;
    }
}

bool getpf(void)
{
    if (cpu->flags_changed & FLAG_PARITY) {
        /* inverted lookup table on the low nibble (misc_instr.rs:25) */
        return (0x9669 << 2 >> ((cpu->last_result ^ cpu->last_result >> 4) & 15)) & FLAG_PARITY;
    }
    else {
        return (cpu->flags & FLAG_PARITY) != 0;
    }
}

bool getaf(void)
{
    if (cpu->flags_changed & FLAG_ADJUST) {
        bool is_sub = (cpu->flags_changed & FLAG_SUB) != 0;
        int32_t last_op2 = (cpu->last_result - cpu->last_op1) * (is_sub ? -1 : 1);
        return ((cpu->last_op1 ^ last_op2 ^ cpu->last_result) & FLAG_ADJUST) != 0;
    }
    else {
        return (cpu->flags & FLAG_ADJUST) != 0;
    }
}

bool getzf(void)
{
    if (cpu->flags_changed & FLAG_ZERO) {
        return (((~cpu->last_result & (cpu->last_result - 1)) >> cpu->last_op_size) & 1) != 0;
    }
    else {
        return (cpu->flags & FLAG_ZERO) != 0;
    }
}

bool getsf(void)
{
    if (cpu->flags_changed & FLAG_SIGN) {
        return ((cpu->last_result >> cpu->last_op_size) & 1) != 0;
    }
    else {
        return (cpu->flags & FLAG_SIGN) != 0;
    }
}

bool getof(void)
{
    if (cpu->flags_changed & FLAG_OVERFLOW) {
        int32_t is_sub = (uint32_t)cpu->flags_changed >> 31;

        /* add: (a ^ result) & (b ^ result);  sub: the b-xor-1 variant (misc_instr.rs:58-62) */
        int32_t b_xor_1_if_sub = (cpu->last_result - cpu->last_op1) - is_sub;
        return (((cpu->last_op1 ^ cpu->last_result) & (b_xor_1_if_sub ^ cpu->last_result)) >>
                cpu->last_op_size) &
               1;
    }
    else {
        return (cpu->flags & FLAG_OVERFLOW) != 0;
    }
}

/* ------------------------------------------------------------------ *
 * setters -- arith.rs:12-103
 * ------------------------------------------------------------------ */

int32_t flags_add(int32_t dest_operand, int32_t source_operand, int32_t op_size)
{
    int32_t res = (dest_operand + source_operand) & opsize_to_mask(op_size);
    cpu->last_op1 = dest_operand;
    cpu->last_result = res;
    cpu->last_op_size = op_size;
    cpu->flags_changed = FLAGS_ALL;
    return res;
}

int32_t flags_adc(int32_t dest_operand, int32_t source_operand, int32_t op_size)
{
    /* the incoming carry is read FIRST: it comes from the previous operation's
     * lazy state, which this call is about to overwrite (arith.rs:23) */
    int32_t cf = getcf() ? 1 : 0;
    int32_t res = (dest_operand + source_operand + cf) & opsize_to_mask(op_size);
    cpu->last_op1 = dest_operand;
    cpu->last_result = res;
    cpu->last_op_size = op_size;
    cpu->flags_changed = FLAGS_ALL & ~FLAG_CARRY & ~FLAG_ADJUST & ~FLAG_OVERFLOW;
    /* the parenthesisation below is upstream's precedence, spelled out because C's
     * -Wparentheses flags a bare mix of & and | */
    cpu->flags = (cpu->flags & ~FLAG_CARRY & ~FLAG_ADJUST & ~FLAG_OVERFLOW) |
                 ((dest_operand ^ ((dest_operand ^ source_operand) & (source_operand ^ res))) >>
                  op_size & FLAG_CARRY) |
                 ((dest_operand ^ source_operand ^ res) & FLAG_ADJUST) |
                 (((source_operand ^ res) & (dest_operand ^ res)) >> op_size << 11 &
                  FLAG_OVERFLOW);
    return res;
}

int32_t flags_sub(int32_t dest_operand, int32_t source_operand, int32_t op_size)
{
    int32_t res = (dest_operand - source_operand) & opsize_to_mask(op_size);
    cpu->last_op1 = dest_operand;
    cpu->last_result = res;
    cpu->last_op_size = op_size;
    cpu->flags_changed = FLAGS_ALL | FLAG_SUB; /* bit 31 selects sub in the getters */
    return res;
}

int32_t flags_sbb(int32_t dest_operand, int32_t source_operand, int32_t op_size)
{
    int32_t cf = getcf() ? 1 : 0;
    int32_t res = (dest_operand - source_operand - cf) & opsize_to_mask(op_size);
    cpu->last_op1 = dest_operand;
    cpu->last_result = res;
    cpu->last_op_size = op_size;
    cpu->flags_changed =
        (FLAGS_ALL & ~FLAG_CARRY & ~FLAG_ADJUST & ~FLAG_OVERFLOW) | FLAG_SUB;
    cpu->flags = (cpu->flags & ~FLAG_CARRY & ~FLAG_ADJUST & ~FLAG_OVERFLOW) |
                 ((res ^ ((res ^ source_operand) & (source_operand ^ dest_operand))) >>
                  op_size & FLAG_CARRY) |
                 ((dest_operand ^ source_operand ^ res) & FLAG_ADJUST) |
                 (((source_operand ^ dest_operand) & (res ^ dest_operand)) >> op_size << 11 &
                  FLAG_OVERFLOW);
    return res;
}

int32_t flags_inc(int32_t dest_operand, int32_t op_size)
{
    /* inc/dec preserve CF (arith.rs:115): materialize it, then mark everything but
     * bit 0 stale */
    cpu->flags = (cpu->flags & ~1) | (getcf() ? 1 : 0);
    int32_t res = (dest_operand + 1) & opsize_to_mask(op_size);
    cpu->last_op1 = dest_operand;
    cpu->last_result = res;
    cpu->last_op_size = op_size;
    cpu->flags_changed = FLAGS_ALL & ~1;
    return res;
}

int32_t flags_dec(int32_t dest_operand, int32_t op_size)
{
    cpu->flags = (cpu->flags & ~1) | (getcf() ? 1 : 0);
    int32_t res = (dest_operand - 1) & opsize_to_mask(op_size);
    cpu->last_op1 = dest_operand;
    cpu->last_result = res;
    cpu->last_op_size = op_size;
    cpu->flags_changed = (FLAGS_ALL & ~1) | FLAG_SUB;
    return res;
}

/* ------------------------------------------------------------------ *
 * eflags in/out -- cpu.rs:1911-1918, 4478-4520
 * ------------------------------------------------------------------ */

int32_t get_eflags(void)
{
    /* goes through the getters, never reads `flags` for the arithmetic bits */
    return (cpu->flags & ~FLAGS_ALL) |
           (getcf() ? 1 : 0) |
           (getpf() ? 1 : 0) << 2 |
           (getaf() ? 1 : 0) << 4 |
           (getzf() ? 1 : 0) << 6 |
           (getsf() ? 1 : 0) << 7 |
           (getof() ? 1 : 0) << 11;
}

void update_eflags(int32_t new_flags)
{
    int32_t dont_update = FLAG_RF | FLAG_VM | FLAG_VIP | FLAG_VIF;
    int32_t clear = ~FLAG_VIP & ~FLAG_VIF & FLAGS_MASK;
    if (cpu->flags & FLAG_VM) {
        /* other case needs to be handled in popf or iret (cpu.rs:4486) */
        DBG_ASSERT(getiopl() == 3);
        dont_update |= FLAG_IOPL;
        /* don't clear vip or vif */
        clear |= FLAG_VIP | FLAG_VIF;
    }
    else {
        if (!cpu->protected_mode) {
            DBG_ASSERT(cpu->cpl == 0);
        }
        if (cpu->cpl != 0) {
            /* cpl > 0: cannot update iopl */
            dont_update |= FLAG_IOPL;
            if ((int32_t)cpu->cpl > getiopl()) {
                /* cpl > iopl: cannot update the interrupt flag */
                dont_update |= FLAG_INTERRUPT;
            }
        }
    }
    cpu->flags =
        ((new_flags ^ ((cpu->flags ^ new_flags) & dont_update)) & clear) | FLAGS_DEFAULT;
    /* the whole flag word was just materialized, so nothing is stale any more */
    cpu->flags_changed = 0;

    /* FLAG_TRAP is not supported: log it and drop it (cpu.rs:4515-4518).  The wasm
     * reference dbg_logs; the C port has no log channel yet, so the clear is the
     * only observable part -- the log line is a divergence to note, not a silence. */
    cpu->flags &= ~FLAG_TRAP;
}
