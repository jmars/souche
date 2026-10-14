/*
 * arith.h -- the ALU semantics: the mul/div families, the shift/rotate families, the
 * bit-test family, and the per-size PUBLIC wrappers over the add/sub cores.
 *
 * Port of v86/src/rust/cpu/arith.rs (1206 lines).  Two ownership boundaries (M1a-1's
 * P-NOT-1, BINDING on this file):
 *
 *   - the add/adc/sub/sbb/inc/dec/neg CORES live in flags.c as flags_add/.../flags_dec
 *     (plus flags_neg); this file owns only the per-size public wrappers (add8/16/32,
 *     adc8/16/32, sub8/16/32, sbb8/16/32, cmp8/16/32, inc8/16/32, dec8/16/32, not8,
 *     neg8/16/32), each a one-liner over the matching core.  DO NOT redefine the cores.
 *   - opsize_to_mask (arith.rs:7) is flags.h's; arith.rs's own copy is NOT duplicated.
 *
 * NAMING: the C symbol is the upstream Rust name (`arith::add8` -> `add8`), so the
 * emitter's module-qualified call maps by stripping the module.  The only renames are
 * the private cores `and`/`or`/`xor` (C keywords, spelled arith_and/arith_or/arith_xor)
 * and everything the landed foundation already owns (flags_*).
 *
 * PORTING CONVENTION -- `<<`: every LEFT shift whose operand can be negative, or whose
 * result can exceed INT32_MAX, is written on uint32_t and cast back.  Upstream's Rust
 * `<<` is defined on the two's-complement bit pattern (bits that shift out are simply
 * discarded -- no overflow panic), whereas C's `<<` on a signed value is undefined
 * behaviour in exactly those cases (e.g. imul8's `read_reg8(AL) << 24`, imul_reg16's
 * `operand1 << 16`).  The cast is bit-exact; it is not a semantic change.
 * RIGHT shifts stay signed (`dest_operand >> count`): C's `>>` on a negative value is
 * implementation-defined, but gcc and clang both define it as arithmetic, which is
 * what Rust's i32 `>>` is.  No right shift in this file can exceed 31.
 */

#ifndef V86C_ARITH_H
#define V86C_ARITH_H

#include <stdbool.h>
#include <stdint.h>

/* ---- per-size public wrappers over the flags_* cores (arith.rs:57-128) ---- */
int32_t add8(int32_t x, int32_t y);
int32_t add16(int32_t x, int32_t y);
int32_t add32(int32_t x, int32_t y);
int32_t adc8(int32_t x, int32_t y);
int32_t adc16(int32_t x, int32_t y);
int32_t adc32(int32_t x, int32_t y);
int32_t sub8(int32_t x, int32_t y);
int32_t sub16(int32_t x, int32_t y);
int32_t sub32(int32_t x, int32_t y);
int32_t sbb8(int32_t x, int32_t y);
int32_t sbb16(int32_t x, int32_t y);
int32_t sbb32(int32_t x, int32_t y);
void cmp8(int32_t x, int32_t y);
void cmp16(int32_t x, int32_t y);
void cmp32(int32_t x, int32_t y);
int32_t inc8(int32_t x);
int32_t inc16(int32_t x);
int32_t inc32(int32_t x);
int32_t dec8(int32_t x);
int32_t dec16(int32_t x);
int32_t dec32(int32_t x);
int32_t not8(int32_t x);
int32_t neg8(int32_t x);
int32_t neg16(int32_t x);
int32_t neg32(int32_t x);

/* ---- mul / imul (arith.rs:130-272) ---- */
void mul8(int32_t source_operand);
void imul8(int32_t source_operand);
void mul16(uint32_t source_operand);
void imul16(int32_t source_operand);
int32_t imul_reg16(int32_t operand1, int32_t operand2);
void mul32(int32_t source_operand);
void imul32(int32_t source_operand);
int32_t imul_reg32(int32_t operand1, int32_t operand2);
int32_t xadd8(int32_t source_operand, int32_t reg);
int32_t xadd16(int32_t source_operand, int32_t reg);
int32_t xadd32(int32_t source_operand, int32_t reg);
int32_t cmpxchg8(int32_t data, int32_t r);
int32_t cmpxchg16(int32_t data, int32_t r);
int32_t cmpxchg32(int32_t data, int32_t r);

/* ---- BCD (arith.rs:307-399) ---- */
void bcd_daa(void);
void bcd_das(void);
void bcd_aad(int32_t imm8);
void bcd_aam(int32_t imm8);
void bcd_aaa(void);
void bcd_aas(void);

/* ---- and / or / xor / test (arith.rs:400-438) ---- */
int32_t and8(int32_t x, int32_t y);
int32_t and16(int32_t x, int32_t y);
int32_t and32(int32_t x, int32_t y);
void test8(int32_t x, int32_t y);
void test16(int32_t x, int32_t y);
void test32(int32_t x, int32_t y);
int32_t or8(int32_t x, int32_t y);
int32_t or16(int32_t x, int32_t y);
int32_t or32(int32_t x, int32_t y);
int32_t xor8(int32_t x, int32_t y);
int32_t xor16(int32_t x, int32_t y);
int32_t xor32(int32_t x, int32_t y);

/* ---- rotate / rotate-through-carry (arith.rs:440-637) ---- */
int32_t rol8(int32_t dest_operand, int32_t count);
int32_t rol16(int32_t dest_operand, int32_t count);
int32_t rol32(int32_t dest_operand, int32_t count);
int32_t rcl8(int32_t dest_operand, int32_t count);
int32_t rcl16(int32_t dest_operand, int32_t count);
int32_t rcl32(int32_t dest_operand, int32_t count);
int32_t ror8(int32_t dest_operand, int32_t count);
int32_t ror16(int32_t dest_operand, int32_t count);
int32_t ror32(int32_t dest_operand, int32_t count);
int32_t rcr8(int32_t dest_operand, int32_t count);
int32_t rcr16(int32_t dest_operand, int32_t count);
int32_t rcr32(int32_t dest_operand, int32_t count);

/* ---- div / idiv (arith.rs:638-753).  `without_fault` returns false instead of
 * raising #de; the wrapper raises it (trigger_de, exceptions.c/M1c). ---- */
void div8(uint32_t source_operand);
void idiv8(int32_t source_operand);
bool div16_without_fault(uint32_t source_operand);
void div16(uint32_t source_operand);
bool idiv16_without_fault(int32_t source_operand);
void idiv16(int32_t source_operand);
bool div32_without_fault(uint32_t source_operand);
void div32(uint32_t source_operand);
bool idiv32_without_fault(int32_t source_operand);
void idiv32(int32_t source_operand);

/* ---- shift (arith.rs:755-996) ---- */
int32_t shl8(int32_t dest_operand, int32_t count);
int32_t shl16(int32_t dest_operand, int32_t count);
int32_t shl32(int32_t dest_operand, int32_t count);
int32_t shr8(int32_t dest_operand, int32_t count);
int32_t shr16(int32_t dest_operand, int32_t count);
int32_t shr32(int32_t dest_operand, int32_t count);
int32_t sar8(int32_t dest_operand, int32_t count);
int32_t sar16(int32_t dest_operand, int32_t count);
int32_t sar32(int32_t dest_operand, int32_t count);
int32_t shrd16(int32_t dest_operand, int32_t source_operand, int32_t count);
int32_t shrd32(int32_t dest_operand, int32_t source_operand, int32_t count);
int32_t shld16(int32_t dest_operand, int32_t source_operand, int32_t count);
int32_t shld32(int32_t dest_operand, int32_t source_operand, int32_t count);

/* ---- bit test (arith.rs:998-1047) ---- */
void bt_reg(int32_t bit_base, int32_t bit_offset);
int32_t btc_reg(int32_t bit_base, int32_t bit_offset);
int32_t bts_reg(int32_t bit_base, int32_t bit_offset);
int32_t btr_reg(int32_t bit_base, int32_t bit_offset);
void bt_mem(int32_t virt_addr, int32_t bit_offset);
void btc_mem(int32_t virt_addr, int32_t bit_offset);
void btr_mem(int32_t virt_addr, int32_t bit_offset);
void bts_mem(int32_t virt_addr, int32_t bit_offset);

/* ---- bit scan / popcount (arith.rs:1049-1125) ---- */
int32_t bsf16(int32_t old, int32_t bit_base);
int32_t bsf32(int32_t old, int32_t bit_base);
int32_t bsr16(int32_t old, int32_t bit_base);
int32_t bsr32(int32_t old, int32_t bit_base);
int32_t popcnt(int32_t v);

/* ---- the saturating pack helpers used by instructions_0f (arith.rs:1127-1206) ---- */
uint8_t saturate_sw_to_ub(uint16_t v);
uint8_t saturate_sw_to_sb(int32_t v);
uint16_t saturate_sd_to_sw(uint32_t v);
int8_t saturate_sd_to_sb(uint32_t v);
int32_t saturate_sd_to_ub(int32_t v);
uint8_t saturate_ud_to_ub(uint32_t v);
uint16_t saturate_uw(uint32_t v);

#endif /* V86C_ARITH_H */
