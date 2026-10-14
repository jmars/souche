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
 * `op_size` is one of OPSIZE_8/16/32 (a bit position, see cpu_state.h).
 */
int32_t flags_add(int32_t dest_operand, int32_t source_operand, int32_t op_size);
int32_t flags_adc(int32_t dest_operand, int32_t source_operand, int32_t op_size);
int32_t flags_sub(int32_t dest_operand, int32_t source_operand, int32_t op_size);
int32_t flags_sbb(int32_t dest_operand, int32_t source_operand, int32_t op_size);
int32_t flags_inc(int32_t dest_operand, int32_t op_size);
int32_t flags_dec(int32_t dest_operand, int32_t op_size);
/* arith.rs:98: `neg` is literally sub(0, x, op_size). */
static inline int32_t flags_neg(int32_t dest_operand, int32_t op_size)
{
    return flags_sub(0, dest_operand, op_size);
}

/* cpu.rs:1911-1918 -- materialize an eflags word from `flags` + the getters. */
int32_t get_eflags(void);

/* cpu.rs:4478-4520 -- load an eflags word from POPF/IRET, honouring the bits that
 * the current privilege level is not allowed to change. */
void update_eflags(int32_t new_flags);

/* arith.rs:7 -- (2 << op_size) - 1.  Written unsigned because upstream relies on
 * i32 shift-out-then-wrap for OPSIZE_32 (2 << 31 == 0, so the mask is -1); C's signed
 * << at the width is undefined behaviour, u32's is not. */
static inline int32_t opsize_to_mask(int32_t op_size)
{
    return (int32_t)((2u << op_size) - 1u);
}

#endif /* V86C_FLAGS_H */
