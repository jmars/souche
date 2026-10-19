/*
 * modrm.h -- the addressing interface: the prefix decoder, the segment-offset
 * accessors, the instruction-fetch (immediate) path and modrm_resolve.
 *
 * This is the interface the emitter (M1a-3, plan ss1) and every memory-operand
 * instruction call, so the signatures and the fault behaviour are stated here.
 *
 * Ported from v86/src/rust/cpu/modrm.rs (133 lines) plus these cpu.rs functions:
 *   2584-2631 read_imm8/read_imm8s/read_imm16/read_imm32s   (the fetch path)
 *   2633-2640 is_osize_32/is_asize_32                       (prefix decode)
 *   3083-3116 get_seg_cs/get_seg_ss/segment_prefix/get_seg_prefix(+_ds/_ss)
 *   3122-3130 modrm_resolve
 *   2918-2931 get_seg
 * The plan's step 8 assigns modrm_resolve and get_seg_prefix to this unit; the fetch
 * path follows them because modrm's disp8/disp16/disp32 are read through it, and
 * paging.c explicitly left read_imm* to "the fetch path of a later unit"
 * (paging.c:446).  It is a deliberate divergence from upstream's file layout (all of
 * the above live in cpu.rs) and it is recorded in the unit report.
 *
 * FAULT BEHAVIOUR (the cpu_state.h contract): every function below that can fault
 * sets cpu->pagefault and returns the sentinel 0; the caller checks the flag before
 * using the value and returns from the instruction.  `get_seg`'s #gp (a null segment)
 * uses the *same* flag: upstream returns `Err(())` there -- "abort this instruction
 * after the interrupt was delivered" -- and cpu->pagefault is the port's only abort
 * channel.  It is set AFTER trigger_gp() has delivered the interrupt.
 */

#ifndef V86C_MODRM_H
#define V86C_MODRM_H

#include "cpu_state.h"

/* prefix.rs:1-16.  Upstream keeps them in their own module; the port puts them here,
 * next to the two functions that decode them. */
#define PREFIX_REPZ          0x08
#define PREFIX_REPNZ         0x10
#define PREFIX_MASK_REP      (PREFIX_REPZ | PREFIX_REPNZ)
#define PREFIX_MASK_OPSIZE   0x20
#define PREFIX_MASK_ADDRSIZE 0x40
#define PREFIX_66            PREFIX_MASK_OPSIZE
#define PREFIX_67            PREFIX_MASK_ADDRSIZE
#define PREFIX_F2            PREFIX_REPNZ
#define PREFIX_F3            PREFIX_REPZ
#define SEG_PREFIX_ZERO      7
#define PREFIX_MASK_SEGMENT  0x07

/* S5a (plan ss3.6.5): REX, in the widened `cpu->prefixes`.  The byte's nibble is
 * W R X B in bits 11 10 9 8 and PRESENT is bit 12 -- `40` (no bits set) is a legal REX and
 * must still be distinguishable from "no REX", which is the whole reason for the fifth bit.
 *
 * WHAT EACH BIT EXTENDS, so a reader does not have to re-derive it:
 *   B  the modrm r/m field, the SIB base, and the opcode's register (`B8+r`, `50+r`)
 *   X  the SIB index
 *   R  the modrm reg field
 *   W  the operand size: 64-bit, i.e. dispatch key os_index == 2 (modrm.c's
 *      dispatch_os_index, the 0x2NN case of the generated switch)
 *
 * The SDM's cancel rule ("a legacy prefix after REX cancels the REX") is implemented in the
 * prefix handler, and it is why every 32-bit prefix instruction in instructions.c CLEARS the
 * REX bits as well as its own: a 0x66 seen after a 0x48 leaves neither. */
#define PREFIX_REX_B         0x0100
#define PREFIX_REX_X         0x0200
#define PREFIX_REX_R         0x0400
#define PREFIX_REX_W         0x0800
#define PREFIX_REX_PRESENT   0x1000
#define PREFIX_REX_NIBBLE    0x0F00
#define PREFIX_REX_MASK      (PREFIX_REX_PRESENT | PREFIX_REX_NIBBLE)

/* The two register-index extensions, as one line each: the generated dispatcher passes
 * `reg_index_b(modrm_byte) | 0` style expressions instead of a raw `& 7` mask (gen/
 * interpreter_c.js), so an index is REX-extended AT THE ONE PLACE the field is formed rather
 * than in 1922 bodies.  Outside long mode the REX bits are clear and both are the identity. */
static inline int32_t reg_index_b(int32_t r) { return r | ((cpu->prefixes & PREFIX_REX_B) ? 8 : 0); }
static inline int32_t reg_index_r(int32_t r) { return r | ((cpu->prefixes & PREFIX_REX_R) ? 8 : 0); }
static inline int32_t reg_index_x(int32_t r) { return r | ((cpu->prefixes & PREFIX_REX_X) ? 8 : 0); }

/* "the machine is in IA-32e 64-bit mode" -- ONE predicate, so the three consumers (the
 * dispatch key, the modrm resolver, the memory accessors) cannot disagree about the mode. */
static inline bool long_mode_64(void) { return (cpu->efer & EFER_LMA) != 0 && cpu->is_64; }

/* cpu.rs:2582 -- a compile-time constant upstream; false, so the cached-translation
 * fast path of read_imm16/read_imm32s is the live one */
#define DISABLE_EIP_TRANSLATION_OPTIMISATION 0

/* cpu.rs:2633-2640 */
bool is_osize_32(void);
bool is_asize_32(void);

/* cpu.rs:3083-3116 -- the segment offsets and the segment-override prefix.
 *
 * S3: get_seg_cs/get_seg_ss return uint64_t because their callers ADD an offset to the base
 * to form a FLAT LINEAR ADDRESS and store it in cpu->instruction_pointer (exceptions.c's
 * sixteen `get_seg_cs() + ...` sites, instructions.c's far transfers).  Returned as int32_t
 * a base of 0xFFFF0000 would sign-extend into the 64-bit field and produce an address the
 * 32-bit model cannot represent.  The remaining accessors stay int32_t: they feed the 32-bit
 * ADDRESSING FORMS (resolve_modrm32's base+index+disp), which is still 32-bit arithmetic --
 * upstream's own i32 wrap -- and the long-mode address generation is S5/S7's. */
uint64_t get_seg_cs(void);
uint64_t get_seg_ss(void);
int32_t segment_prefix(int32_t default_segment);
int32_t get_seg_prefix(int32_t default_segment);
int32_t get_seg_prefix_ds(int32_t offset);
int32_t get_seg_prefix_ss(int32_t offset);
int32_t get_seg(int32_t segment);

/* cpu.rs:2584-2631 -- the fetch path.  read_imm8/16/32 advance instruction_pointer by
 * 1/2/4 and are the only readers of the cached eip_phys translation. */
int32_t read_imm8(void);
int32_t read_imm8s(void);
int32_t read_imm16(void);
int32_t read_imm32s(void);

/* S5a: THE TWO 8-BYTE OPERANDS IN LONG MODE (plan ss3.6.5's read_imm64/UOP_IMM_64).  An
 * immediate is 8 bytes wide in exactly one instruction -- `MOV r64, imm64` (B8+r with REX.W) --
 * and an moffs OFFSET is 8 bytes wide whatever the operand size is, so both read the same
 * way: two 32-bit halves, LOW FIRST (the instruction stream's byte order is semantic: reading
 * the halves in the other order is a wrong immediate with the length still balanced).  The
 * uop cache's `UOP_IMM_64` kind is S13's, because that decoder refuses long mode. */
int64_t read_imm64(void);
int64_t read_moffs64(void);

/* cpu.rs:3122-3130 + modrm.rs:4-133 */
int32_t modrm_resolve(int32_t modrm_byte);

/* S5a: THE 64-BIT ADDRESS GENERATOR (plan correction W1).  modrm_resolve() above returns an
 * int32_t because the 32-bit forms wrap at 32 bits; a long-mode effective address is 64 bits
 * (RIP-relative, a 64-bit SIB base and index, a full-width displacement) and truncating it is
 * a wrong PAGE with no symptom -- so the 64-bit bodies take THIS resolver instead, and the
 * generated dispatcher declares their `addr` local as int64_t.
 *
 * The rules it adds to the 32-bit one: mod=00/rm=101 is RIP-relative (not disp32-absolute);
 * SIB base 101 with mod=00 is a disp32 with NO base; REX.B/REX.X extend the base and the
 * index (including the R12-as-index case, where 100b would mean "no index" only without
 * REX.X); and a 0x67 address-size override computes a 32-bit address that is ZERO-EXTENDED. */
int64_t modrm_resolve64(int32_t modrm_byte);

/* S5a: THE THREE-VALUED DISPATCH KEY (plan ss3.6.1).  Returns 0, 1 or 2 = the 16-, 32- or
 * 64-bit operand-size ARM of the generated switch, which is called as
 * `run_instruction(opcode | (dispatch_os_index(opcode) << 8))`.
 *
 * It is a FUNCTION of the opcode and not one line at each call site for a reason the corpus
 * makes concrete: in long mode some encodings default to a 64-bit operand size WITHOUT a REX
 * byte (push/pop/pushf/popf/jmp -- the SDM's default-64 group), and the opcode is the only
 * thing that knows.  The default-64 list is generated from gen/x86_table64.js
 * (long_mode_default64, in the generated file) so it cannot drift from the arms. */
int32_t dispatch_os_index(int32_t opcode);

/* S5a: the SDM's default-64 operand-size group, GENERATED from gen/x86_table64.js's
 * `default64` flags into the generated dispatcher (src/cpu/interpreter_gen.c).  It lives in the
 * generated file for the same reason the arms do: one declaration, one source, and a slice that
 * adds a default-64 encoding cannot forget to update a second table. */
bool long_mode_default64(int32_t opcode);
int32_t resolve_modrm16(int32_t modrm_byte);
int32_t resolve_modrm32(int32_t modrm_byte);
/* modrm.rs:37-66 -- upstream-dead: nothing calls it (READ: no reference to
 * resolve_modrm32_ outside its own definition).  Kept for 1:1 parity. */
int32_t resolve_modrm32_(int32_t modrm_byte);

#endif /* V86C_MODRM_H */
