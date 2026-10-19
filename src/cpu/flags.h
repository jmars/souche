/*
 * flags.h -- the lazy-flag machinery.
 *
 * Upstream layout (READ): `flags` holds the materialized arithmetic flags,
 * `flags_changed` is a bitmask of STALE flags, and `last_op_size`/`last_op1`/
 * `last_result` record the last arithmetic operation.  BIT 31 OF flags_changed IS
 * FLAG_SUB -- the add-vs-sub selector the CF/AF/OF getters use
 * (misc_instr.rs:12-22, 34-43, 55-62).
 *
 * Invariants this header exists to state (plan ss4):
 *   - every getter checks flags_changed FIRST and computes from the last_* fields;
 *   - every arithmetic writer either updates `flags` AND clears the bit in
 *     flags_changed, or sets the bit;
 *   - adc/sbb read getcf() FIRST, then compute CF/AF/OF eagerly;
 *   - inc/dec preserve CF;
 *   - anything that needs real flags goes through the getters or get_eflags() --
 *     never reads `flags` directly;
 *   - a missed flags_changed update makes a LATER, unrelated instruction see a stale
 *     flag (the silent-wrong surface the nasm corpus catches).
 */

#ifndef V86C_FLAGS_H
#define V86C_FLAGS_H

#include "cpu_state.h"

/* getters -- misc_instr.rs:8-66, ported verbatim */
bool getcf(void);
bool getpf(void);
bool getaf(void);
bool getzf(void);
bool getsf(void);
bool getof(void);

/*
 * setters -- arith.rs:12-136.  Upstream's three-argument cores are `add`, `adc`,
 * `sub`, `sbb`, `inc`, `dec` (private) with per-size public wrappers add8/16/32
 * etc.  The `flags_` prefix marks the core: the wrappers are arith.c's (M1a step 7),
 * which will call these; the names differ only to satisfy C's single namespace.
 * `op_size` is one of OPSIZE_8/16/32/64 (a bit position, see cpu_state.h).
 *
 * S1 (results/x86-64-plan.md ss4): THE OPERANDS AND THE RESULT ARE int64_t.  The 32-bit
 * wrappers in arith.c pass int32_t and truncate the result on the way back -- bit-exact,
 * because every core masks its result to `opsize_to_mask(op_size)`.  Widening the
 * PARAMETERS rather than adding a second set of `flags_add64` is what keeps ONE
 * implementation of the flag rules: a second copy is where a 64-bit carry would one day
 * be fixed and a 32-bit one broken.
 */
int64_t flags_add(int64_t dest_operand, int64_t source_operand, int32_t op_size);
int64_t flags_adc(int64_t dest_operand, int64_t source_operand, int32_t op_size);
int64_t flags_sub(int64_t dest_operand, int64_t source_operand, int32_t op_size);
int64_t flags_sbb(int64_t dest_operand, int64_t source_operand, int32_t op_size);
int64_t flags_inc(int64_t dest_operand, int32_t op_size);
int64_t flags_dec(int64_t dest_operand, int32_t op_size);
/* arith.rs:98: `neg` is literally sub(0, x, op_size). */
static inline int64_t flags_neg(int64_t dest_operand, int32_t op_size)
{
    return flags_sub(0, dest_operand, op_size);
}

/* cpu.rs:1911-1918 -- materialize an eflags word from `flags` + the getters. */
int32_t get_eflags(void);

/* cpu.rs:4478-4520 -- load an eflags word from POPF/IRET, honouring the bits that
 * the current privilege level is not allowed to change. */
void update_eflags(int32_t new_flags);

/* arith.rs:7 -- (2 << op_size) - 1.  Written unsigned because upstream relies on
 * shift-out-then-wrap for OPSIZE_32 (2 << 31 == 0, so the mask is -1); C's signed
 * << at the width is undefined behaviour, unsigned's is not.
 *
 * S1: SAME TRICK, ONE WIDTH UP.  2u << 64 would be undefined (a shift count equal to the
 * width) and is why OPSIZE_64 is 63 and this returns int64_t: 2ull << 63 discards the
 * shifted-out bit and gives 0, so the -1 makes all 64 bits, exactly as 2u << 31 gives the
 * 32-bit mask.  For OPSIZE_8/16/32 the value is unchanged (0x7F/0x7FFF/0xFFFFFFFF). */
static inline int64_t opsize_to_mask(int32_t op_size)
{
    return (int64_t)((2ull << op_size) - 1ull);
}

/* S1: THE LAZY OPERANDS' CANONICAL FORM.  last_op1/last_result are int64_t now, and every
 * writer stores the value MASKED TO ITS OPERAND WIDTH -- i.e. zero-extended, the same form
 * the (masked) cores produce and the same form the JIT's spill writes (an 8-byte store of
 * a zero-extended 32-bit host register).  Canonicalising matters for exactly one reason
 * that is not cosmetic: the JIT and the interpreter must agree BYTE FOR BYTE about the
 * state, and jit_codegen_selftest's whole-struct memcmp is what enforces it -- the first
 * widening failed there with 40 differences, every one of them a negative int32 that the C
 * path had sign-extended and the JIT had not.
 *
 * The canonical form changes no flag: every getter examines bits at or below
 * `last_op_size` (and getcf masks both operands to the width itself), so the two
 * representations are indistinguishable to them -- which is why this is a storage rule and
 * not a semantic one. */
static inline int64_t last_value(int64_t value, int32_t op_size)
{
    return value & opsize_to_mask(op_size);
}

/* store a lazy result AND its size, in one place, so a setter cannot store one without
 * the other. */
static inline void set_last_result(int64_t value, int32_t op_size)
{
    cpu->last_result = last_value(value, op_size);
    cpu->last_op_size = op_size;
}

#endif /* V86C_FLAGS_H */
