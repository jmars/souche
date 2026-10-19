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

/* cpu.rs:3122-3130 + modrm.rs:4-133 */
int32_t modrm_resolve(int32_t modrm_byte);
int32_t resolve_modrm16(int32_t modrm_byte);
int32_t resolve_modrm32(int32_t modrm_byte);
/* modrm.rs:37-66 -- upstream-dead: nothing calls it (READ: no reference to
 * resolve_modrm32_ outside its own definition).  Kept for 1:1 parity. */
int32_t resolve_modrm32_(int32_t modrm_byte);

#endif /* V86C_MODRM_H */
