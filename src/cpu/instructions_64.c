/*
 * instructions_64.c -- THE 64-BIT INSTRUCTION BODIES (slice S5b).
 *
 * DESIGN OF RECORD: results/x86-64-plan.md ss3.6 (the dispatch stays GENERATED, the semantics
 * stay hand-written), ss6's S5 row ("a mov/add subset"), and results/x86-64-s5.md ss3 (what
 * this file does and does not own).  The dispatch that calls these is generated from
 * gen/x86_table64.js; this file is the SEMANTIC half, in the same relationship to the
 * generated switch that instructions.c has for the 32-bit table (docs/architecture.md ss7).
 *
 * ------------------------------------------------------------------ *
 * THE THREE RULES THAT MAKE THIS FILE SMALL AND ITS ABSENCES LOUD
 *
 * 1. A BODY EXISTS ONLY FOR WHAT THE SLICE IMPLEMENTS.  An overlay encoding whose body is
 *    missing emits `unimplemented_instruction("instructions::instr64_X")` from the generated
 *    switch -- a NAMED STOP, never a 32-bit body quietly doing the work.  That is how the
 *    corpus family's RED state was mechanical before the semantics landed (plan ss2.1.7b).
 * 2. EVERY BODY IS REACHED ONLY THROUGH A 0x2NN DISPATCH KEY, i.e. only in 64-bit mode with
 *    the operand size the body is for.  The bodies do not re-test the mode; the KEY is the
 *    test, and `--long-mode off` cannot produce that key (dispatch_os_index, modrm.c).
 * 3. A 64-BIT WRITE GOES THROUGH write_reg64, A 32-BIT ONE THROUGH write_reg32 (which zeroes
 *    the high half).  That one line is the whole of the "32-bit operations zero-extend"
 *    rule, and it is why `mov eax, -1` after `mov rax, 0xFFFF...0000` produces
 *    0x00000000FFFFFFFF in the corpus's mov-zero-extend-32 case.
 *
 * ------------------------------------------------------------------ *
 * WHAT IT DOES NOT OWN, so a reader does not read a silence as a promise
 *
 *   movsxd (0x63), movzx/movsx, lea, cmov, setcc, xchg, the jcc rel family, call/ret/jmp rel32,
 *   push/pop of r/m, leave, iretq                                    -- S7
 *   adc/sbb/and/or/sub/test/cmp/inc/dec/neg/not, the imm8/imm32 ALU groups -- S6
 *   mul/imul/div/idiv, the shifts and rotates, bt/bts/btr/btc, bsf/bsr, bswap, shld/shrd -- S8
 *   the string/bit/atomic ops including cmpxchg16b                   -- S9
 *   x87/MMX/SSE, including every instruction whose addressing is the only thing long mode
 *   changes                                                          -- S10
 *   SYSCALL/SYSRET/SWAPGS, the FS/GS-base MSRs, the 64-bit frame     -- S11
 *
 * The stack here is the exception worth naming: push/pop/pushf/popf ARE in this slice because
 * the corpus's shared prologue and epilogue use them in every one of the 57 images, and
 * because their long-mode operand size is 64 BY DEFAULT (an overlay `default64` entry) -- a
 * 32-bit body there would push four bytes and stay in sync, which is the silent class.
 */

#include "instructions_64.h"

#include "arith.h" /* S6: add64/sub64/... -- the ONE set of flag cores at OPSIZE_64 */
#include "cpu.h"
#include "flags.h"
#include "instructions.h" /* S5a: run_prefix_instruction, handle_irqs (declared there) */
#include "memory.h"
#include "misc_instr.h"
#include "modrm.h"
#include "paging.h"

/* ================================================================== *
 * THE REX PREFIX (0x40-0x4F in long mode)
 * ================================================================== */

/* Called from the generated switch's 0x2NN arm for a 0x40-0x4F byte (gen/x86_table64.js's
 * `prefix64` entries), which dispatch_os_index selects for those bytes whenever the machine is
 * in 64-bit mode.
 *
 * THREE THINGS, in this order, and the order is the SDM's:
 *   1. a legacy prefix AFTER a REX cancels it -- implemented in the other direction by
 *      instructions.c's prefixes_cancel_rex(), which every legacy prefix handler calls;
 *   2. the nibble is RECORDED (W R X B, bits 8-11) together with the present bit, because REX
 *      0x40 sets no bits at all and must still be distinguishable from "no REX" (it changes
 *      AH/CH/DH/BH into SPL/BPL/SIL/DIL);
 *   3. the rest of the instruction is re-dispatched with the REX visible in cpu->prefixes,
 *      exactly as the 0x66/0x67 handlers re-dispatch.
 *
 * A SECOND REX before the opcode is not an error on hardware -- the last one wins -- and that
 * falls out of overwriting the nibble. */
void long_mode_rex_prefix(int32_t byte)
{
	/* the prefix byte's own low nibble is the REX payload; the top nibble is fixed 0x4 */
	cpu->prefixes = (uint16_t)((cpu->prefixes & ~(uint16_t)PREFIX_REX_MASK) |
	                           (uint16_t)PREFIX_REX_PRESENT |
	                           (uint16_t)((uint32_t)(byte & 0x0F) << 8));
	run_prefix_instruction();
	cpu->prefixes = 0;
}

/* ================================================================== *
 * MOV -- the r/m forms (0x88/0x89/0x8A/0x8B)
 *
 * The 8-bit pair (88/8A) exists here for a reason that is NOT width: in 64-bit mode a byte
 * operand's REGISTER is selected differently with and without a REX byte (AH/CH/DH/BH vs
 * SPL/BPL/SIL/DIL, and R8B-R15B with REX.B), and the index the generated switch passes is
 * already REX-extended -- so `40 B6 AA` reaches instr64_B6 with index 6 and the accessor maps
 * it to SIL.  These bodies are the same one-liners as the 32-bit ones for that reason.
 * ================================================================== */

void instr64_88_mem(int64_t addr, int32_t r)
{
	/* an 8-bit operand needs no 64-bit read-modify-write: one translate + one store */
	safe_write8_addr(addr, read_reg8(r));
}
void instr64_88_reg(int32_t r1, int32_t r) { write_reg8(r1, read_reg8(r)); }

void instr64_89_mem(int64_t addr, int32_t r)
{
	safe_write64_addr(addr, read_reg64(r));
}
void instr64_89_reg(int32_t r1, int32_t r) { write_reg64(r1, read_reg64(r)); }

void instr64_8A_mem(int64_t addr, int32_t r)
{
	int32_t x = safe_read8_addr(addr);
	if (cpu->pagefault) return;
	write_reg8(r, x);
}
void instr64_8A_reg(int32_t r1, int32_t r) { write_reg8(r, read_reg8(r1)); }

void instr64_8B_mem(int64_t addr, int32_t r)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, x);
}
void instr64_8B_reg(int32_t r1, int32_t r) { write_reg64(r, read_reg64(r1)); }

/* ================================================================== *
 * MOV -- the immediate forms
 * ================================================================== */

/* B8+r with REX.W: the ONE 8-byte immediate in the instruction set.  The register index is the
 * opcode's low three bits plus REX.B, which is why there are eight bodies and not one.
 *
 * SPELLED OUT, not macro-generated, for a mechanical reason worth knowing: the emitter's
 * availability scan reads definitions with `^void <name>(` at the START OF A LINE (gen/
 * interpreter_c.js), so a definition produced by a macro is INVISIBLE to it -- the eight arms
 * stayed "unimplemented" in the generated switch while the bodies existed, which is the
 * loud-but-wrong state the scan exists to prevent.  A body is a one-liner; eight one-liners. */
void instr64_B8(int64_t imm) { write_reg64(reg_index_b(0), (uint64_t)imm); }
void instr64_B9(int64_t imm) { write_reg64(reg_index_b(1), (uint64_t)imm); }
void instr64_BA(int64_t imm) { write_reg64(reg_index_b(2), (uint64_t)imm); }
void instr64_BB(int64_t imm) { write_reg64(reg_index_b(3), (uint64_t)imm); }
void instr64_BC(int64_t imm) { write_reg64(reg_index_b(4), (uint64_t)imm); }
void instr64_BD(int64_t imm) { write_reg64(reg_index_b(5), (uint64_t)imm); }
void instr64_BE(int64_t imm) { write_reg64(reg_index_b(6), (uint64_t)imm); }
void instr64_BF(int64_t imm) { write_reg64(reg_index_b(7), (uint64_t)imm); }

/* B0+r: mov r8, imm8.  In long mode the same REX.B rule selects r8b-r15b, and the REX
 * PRESENCE rule turns AH into SPL -- both live in cpu_state.c's byte accessor. */
void instr64_B0(int64_t imm) { write_reg8(reg_index_b(0), (int32_t)imm); }
void instr64_B1(int64_t imm) { write_reg8(reg_index_b(1), (int32_t)imm); }
void instr64_B2(int64_t imm) { write_reg8(reg_index_b(2), (int32_t)imm); }
void instr64_B3(int64_t imm) { write_reg8(reg_index_b(3), (int32_t)imm); }
void instr64_B4(int64_t imm) { write_reg8(reg_index_b(4), (int32_t)imm); }
void instr64_B5(int64_t imm) { write_reg8(reg_index_b(5), (int32_t)imm); }
void instr64_B6(int64_t imm) { write_reg8(reg_index_b(6), (int32_t)imm); }
void instr64_B7(int64_t imm) { write_reg8(reg_index_b(7), (int32_t)imm); }

/* C6 /0: mov r/m8, imm8 */
void instr64_C6_0_mem(int64_t addr, int64_t imm)
{
	safe_write8_addr(addr, (int32_t)imm);
}
void instr64_C6_0_reg(int32_t r1, int64_t imm) { write_reg8(r1, (int32_t)imm); }

/* C7 /0 with REX.W: mov r/m64, imm32 SIGN-EXTENDED.  This is the form a compiler emits for a
 * small 64-bit constant (`mov rsp, 0x102000` is `48 C7 C4 ...`, MEASURED), so it is load
 * bearing for the corpus's prologue; the sign extension is the part that differs from B8+r. */
void instr64_C7_0_mem(int64_t addr, int64_t imm)
{
	safe_write64_addr(addr, (uint64_t)imm);
}
void instr64_C7_0_reg(int32_t r1, int64_t imm) { write_reg64(r1, (uint64_t)imm); }

/* A0-A3: the moffs forms.  In 64-bit mode the OFFSET is 8 bytes whatever the operand size is --
 * which is why the body takes an int64_t even on the 0x1NN arm (misc_instr.c's read_moffs reads
 * 8 bytes in long mode; this slice's 0x2NN arms get the full 64-bit address, and the 0x1NN arm
 * names the >4 GiB case rather than truncating it). */
void instr64_A0(int64_t addr)
{
	int32_t x = safe_read8_addr(addr);
	if (cpu->pagefault) return;
	write_reg8(reg_index_b(0), x);
}
void instr64_A1(int64_t addr)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(reg_index_b(0), x);
}
void instr64_A2(int64_t addr)
{
	safe_write8_addr(addr, read_reg8(reg_index_b(0)));
}
void instr64_A3(int64_t addr)
{
	safe_write64_addr(addr, read_reg64(reg_index_b(0)));
}

/* ================================================================== *
 * S6: THE ALU FAMILY AT 64 BITS, WITH ITS FLAGS
 *
 * results/x86-64-plan.md ss6's S6 row.  Every body here DELIVERS the 64-bit result and SETS
 * THE ARITHMETIC FLAGS, through the port's ONE set of flag cores at `OPSIZE_64`
 * (arith.h's note; the cores themselves are S1's widened `flags_add/adc/sub/sbb/inc/dec` and
 * the now-widened `arith_and/or/xor`).  There is deliberately no second flag path: a 64-bit
 * carry computed by a parallel implementation is exactly the thing that gets fixed in one
 * width and broken in the other.
 *
 * WHICH FLAG RULES ARE VERIFIED, AND WHERE (the honest inventory -- the corpus is the only
 * oracle there is for this layer, plan ss2.1):
 *
 *   CF   alu-add64-carry-chain (`0xFFFFFFFF + 1` at 64 bits sets CF and the adc chain adds
 *        it), alu-adc-chain-wide (two chains from 0xFFFFFFFFFFFFFFFF), alu-sub-sbb-64
 *        (borrow out of 64)                                  -- VERIFIED
 *   OF   alu-inc-dec-64 (`inc 0x7FFFFFFFFFFFFFFF` overflows; `dec 0` does not),
 *        alu-sign-boundary-32-64, alu-neg-not-64 (`neg 0x8000000000000000` overflows) -- VERIFIED
 *   SF   alu-test-cmp-64, alu-sign-boundary-32-64, alu-neg-not-64                   -- VERIFIED
 *   ZF   alu-test-cmp-64 (`test rax,rax` with rax != 0 and `cmp rax,rcx` with rcx = 0),
 *        alu-logic-64                                                                -- VERIFIED
 *   PF   carried in every pushfq those cases store (the mask the comparison uses is
 *        run.js's MASK_ARITH: CF|PF|AF|ZF|SF|OF)                                     -- VERIFIED
 *   AF   the same word, on the cases whose low nibble carries (`add rax,1` from 0xFFFFFFFF
 *        carries out of bit 3)                                                       -- VERIFIED
 *   the 32-vs-64 OPERAND-SIZE boundary: alu-op32-vs-op64 does the same `add` at both widths
 *        and compares the 64-bit register AND the flags -- the case that would catch a body
 *        that lost the zero-extension rule (write_reg32 zeroes the high half)        -- VERIFIED
 *
 * WHAT IS **NOT** COVERED, named here rather than implied (results/x86-64-s6.md ss5): the
 * imm32 ALU group (0x81) and the accumulator-immediate forms (0x05/0x0D/0x15/0x1D/0x25/0x2D/
 * 0x35/0x3D) have bodies in NO slice yet -- they stop by name -- and no corpus case exercises
 * AF on a LOGIC operation (the SDM leaves AF undefined there, which is why the corpus may not
 * contain one: plan ss2.1.5's "out of reach by rule").
 *
 * ONE STATED DIVERGENCE from the reference's shape, and it is unobservable in every run this
 * corpus can make: the reference's SAFE_READ_WRITE* macros TRANSLATE THE WRITE ADDRESS FIRST,
 * so a faulting store leaves the flags untouched; these bodies compute the operation (and
 * therefore the flags) and then store, so a faulting store leaves the flags set.  A fault
 * aborts the instruction either way and S5's walk names its own faults, so the difference can
 * only be seen by a #PF handler that reads flags mid-instruction, which no corpus case can
 * build.  Stating it because the alternative -- a per-body write pre-translation for 32
 * bodies -- buys nothing measurable today.
 * ================================================================== */

/* 0x01 add r/m64, r64 */
void instr64_01_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)add64(x, (int64_t)read_reg64(r)));
}
void instr64_01_reg(int32_t r1, int32_t r)
{
	write_reg64(r1, (uint64_t)add64((int64_t)read_reg64(r1), (int64_t)read_reg64(r)));
}
/* 0x03 add r64, r/m64 */
void instr64_03_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)add64((int64_t)read_reg64(r), x));
}
void instr64_03_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (uint64_t)add64((int64_t)read_reg64(r), (int64_t)read_reg64(r1)));
}

/* 0x09 or r/m64, r64 */
void instr64_09_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)or64(x, (int64_t)read_reg64(r)));
}
void instr64_09_reg(int32_t r1, int32_t r)
{
	write_reg64(r1, (uint64_t)or64((int64_t)read_reg64(r1), (int64_t)read_reg64(r)));
}
/* 0x0B or r64, r/m64 */
void instr64_0B_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)or64((int64_t)read_reg64(r), x));
}
void instr64_0B_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (uint64_t)or64((int64_t)read_reg64(r), (int64_t)read_reg64(r1)));
}

/* 0x11 adc r/m64, r64 -- adc READS CF FIRST (flags.h's invariant), which adc64 does */
void instr64_11_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)adc64(x, (int64_t)read_reg64(r)));
}
void instr64_11_reg(int32_t r1, int32_t r)
{
	write_reg64(r1, (uint64_t)adc64((int64_t)read_reg64(r1), (int64_t)read_reg64(r)));
}
/* 0x13 adc r64, r/m64 */
void instr64_13_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)adc64((int64_t)read_reg64(r), x));
}
void instr64_13_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (uint64_t)adc64((int64_t)read_reg64(r), (int64_t)read_reg64(r1)));
}

/* 0x19 sbb r/m64, r64 */
void instr64_19_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)sbb64(x, (int64_t)read_reg64(r)));
}
void instr64_19_reg(int32_t r1, int32_t r)
{
	write_reg64(r1, (uint64_t)sbb64((int64_t)read_reg64(r1), (int64_t)read_reg64(r)));
}
/* 0x1B sbb r64, r/m64 */
void instr64_1B_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)sbb64((int64_t)read_reg64(r), x));
}
void instr64_1B_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (uint64_t)sbb64((int64_t)read_reg64(r), (int64_t)read_reg64(r1)));
}

/* 0x21 and r/m64, r64 */
void instr64_21_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)and64(x, (int64_t)read_reg64(r)));
}
void instr64_21_reg(int32_t r1, int32_t r)
{
	write_reg64(r1, (uint64_t)and64((int64_t)read_reg64(r1), (int64_t)read_reg64(r)));
}
/* 0x23 and r64, r/m64 */
void instr64_23_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)and64((int64_t)read_reg64(r), x));
}
void instr64_23_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (uint64_t)and64((int64_t)read_reg64(r), (int64_t)read_reg64(r1)));
}

/* 0x29 sub r/m64, r64 */
void instr64_29_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)sub64(x, (int64_t)read_reg64(r)));
}
void instr64_29_reg(int32_t r1, int32_t r)
{
	write_reg64(r1, (uint64_t)sub64((int64_t)read_reg64(r1), (int64_t)read_reg64(r)));
}
/* 0x2B sub r64, r/m64 */
void instr64_2B_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)sub64((int64_t)read_reg64(r), x));
}
void instr64_2B_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (uint64_t)sub64((int64_t)read_reg64(r), (int64_t)read_reg64(r1)));
}

/* 0x31 xor r/m64, r64 */
void instr64_31_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)xor64(x, (int64_t)read_reg64(r)));
}
void instr64_31_reg(int32_t r1, int32_t r)
{
	write_reg64(r1, (uint64_t)xor64((int64_t)read_reg64(r1), (int64_t)read_reg64(r)));
}
/* 0x33 xor r64, r/m64 */
void instr64_33_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)xor64((int64_t)read_reg64(r), x));
}
void instr64_33_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (uint64_t)xor64((int64_t)read_reg64(r), (int64_t)read_reg64(r1)));
}

/* 0x39 cmp r/m64, r64 -- the SUBTRACTION whose result is discarded (cmp64 does the flags) */
void instr64_39_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	cmp64(x, (int64_t)read_reg64(r));
}
void instr64_39_reg(int32_t r1, int32_t r)
{
	cmp64((int64_t)read_reg64(r1), (int64_t)read_reg64(r));
}
/* 0x3B cmp r64, r/m64 */
void instr64_3B_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	cmp64((int64_t)read_reg64(r), x);
}
void instr64_3B_reg(int32_t r1, int32_t r)
{
	cmp64((int64_t)read_reg64(r), (int64_t)read_reg64(r1));
}

/* 0x85 test r/m64, r64 -- the AND whose result is discarded */
void instr64_85_mem(int64_t addr, int32_t r)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	test64(x, (int64_t)read_reg64(r));
}
void instr64_85_reg(int32_t r1, int32_t r)
{
	test64((int64_t)read_reg64(r1), (int64_t)read_reg64(r));
}

/* ------------------------------------------------------------------ *
 * 0x83: THE IMM8 GROUP, /0 add /1 or /2 adc /3 sbb /4 and /5 sub /6 xor /7 cmp.
 *
 * The immediate is a SIGN-EXTENDED imm8 at every operand size, which is why the emitter hands
 * these bodies an `int64_t imm` that `read_imm8s` sign-extended: `add rax, -1` is `83 C0 FF`.
 * The other half of the group (0x81, the imm32 form) has no body yet and stops by name.
 *
 * SPELLED OUT, not macro-generated: the emitter's availability scan reads definitions with
 * `^void <name>(` at the START OF A LINE (S5 measured this the hard way with MOV_IMM64 -- the
 * bodies existed and the arms stayed named stops), and sixteen one-line bodies read better
 * than a macro at this size anyway.
 * ------------------------------------------------------------------ */

void instr64_83_0_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)add64(x, imm));
}

void instr64_83_0_reg(int32_t r1, int64_t imm)
{
	write_reg64(r1, (uint64_t)add64((int64_t)read_reg64(r1), imm));
}

void instr64_83_1_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)or64(x, imm));
}

void instr64_83_1_reg(int32_t r1, int64_t imm)
{
	write_reg64(r1, (uint64_t)or64((int64_t)read_reg64(r1), imm));
}

void instr64_83_2_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)adc64(x, imm));
}

void instr64_83_2_reg(int32_t r1, int64_t imm)
{
	write_reg64(r1, (uint64_t)adc64((int64_t)read_reg64(r1), imm));
}

void instr64_83_3_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)sbb64(x, imm));
}

void instr64_83_3_reg(int32_t r1, int64_t imm)
{
	write_reg64(r1, (uint64_t)sbb64((int64_t)read_reg64(r1), imm));
}

void instr64_83_4_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)and64(x, imm));
}

void instr64_83_4_reg(int32_t r1, int64_t imm)
{
	write_reg64(r1, (uint64_t)and64((int64_t)read_reg64(r1), imm));
}

void instr64_83_5_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)sub64(x, imm));
}

void instr64_83_5_reg(int32_t r1, int64_t imm)
{
	write_reg64(r1, (uint64_t)sub64((int64_t)read_reg64(r1), imm));
}

void instr64_83_6_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)xor64(x, imm));
}

void instr64_83_6_reg(int32_t r1, int64_t imm)
{
	write_reg64(r1, (uint64_t)xor64((int64_t)read_reg64(r1), imm));
}

void instr64_83_7_mem(int64_t addr, int64_t imm)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	cmp64(x, imm);
}

void instr64_83_7_reg(int32_t r1, int64_t imm)
{
	cmp64((int64_t)read_reg64(r1), imm);
}

/* ------------------------------------------------------------------ *
 * 0xF7: /2 NOT and /3 NEG, and 0xFF: /0 INC and /1 DEC.
 *
 * The two fixed_g groups whose reg field is the SUB-OPCODE take no register argument (the
 * emitter passes only the address, or the r/m register).  Why only two of 0xF7's eight: /0 and
 * /1 are `test r/m64, imm32` and /4../7 are MUL/IMUL/DIV/IDIV, which are S8's -- the
 * availability scan turns their arms into NAMED STOPS, so they cannot run silently.  NOT sets
 * no flags; INC/DEC preserve CF (flags.h's invariants).
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * 0x8F /0: POP r/m64.  The plan's S7 row owns push/pop of a REGISTER OR MEMORY operand, but
 * family 2's cases need this one to STORE RFLAGS into the compared window (`pushfq; pop qword
 * ptr [0x100080]`), and a slice whose own cases cannot run is not a slice.  Its long-mode
 * operand size is 64 BY DEFAULT (the overlay's `default64` entry), and it is the reason a body
 * here is not "S7 work leaking into S6": it is the instrument's own plumbing, named.
 * ------------------------------------------------------------------ */
void instr64_8F_0_mem(int32_t modrm_byte)
{
	/* THE ADDRESS IS RESOLVED BY THE BODY, because 0x8F is `custom_modrm_resolve` in the
	 * reference's table and the emitter therefore passes the RAW MODRM BYTE (S5's C6/C7 arms
	 * are the ordinary kind, which is why this one surprised the first version of this body:
	 * MEASURED, the case's rax came out 0x100080 -- an address -- with the pushfq/popf
	 * plumbing writing four bytes into physical 4).
	 *
	 * AND THE ORDER IS THE REFERENCE'S (instructions.c's instr32_8F_0_mem): ESP/RSP is
	 * updated BEFORE the address is resolved, so `pop qword [rsp+8]` uses the
	 * post-decrement value; it is restored for the READ, and applied after the write. */
	uint64_t rsp = read_reg64(ESP);
	/* ROUND 2, THE BLOCKER: THE PRE-ADJUST IS AN INCREMENT, NOT A DECREMENT.  POP moves the
	 * stack pointer UP; the reference's `adjust_stack_reg(4)` (v86 instructions.rs:955-969 and
	 * this tree's instr32_8F_0_mem) is a +, and the review's three sources agree.  Round 1
	 * wrote `rsp - 8` here, so `pop qword ptr [rsp]` resolved its address EIGHT BYTES BELOW
	 * the slot the pop actually reads -- a wrong STORE with no crash, which the corpus could
	 * not see until it had a case whose effective address depends on rsp (alu-pop-rsp).
	 * MEASURED: the case is red with this line's old direction and green with it. */
	write_reg64(ESP, rsp + 8);
	int64_t addr = modrm_resolve64(modrm_byte);
	if (cpu->pagefault) {
		write_reg64(ESP, rsp); /* a fault resets the stack pointer, as the reference does */
		return;
	}
	write_reg64(ESP, rsp);
	uint64_t v = safe_read64_addr((int64_t)rsp);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, v);
	if (cpu->pagefault) return;
	write_reg64(ESP, rsp + 8);
}
void instr64_8F_0_reg(int32_t r1)
{
	uint64_t rsp = read_reg64(ESP);
	uint64_t v = safe_read64_addr((int64_t)rsp);
	if (cpu->pagefault) return;
	write_reg64(ESP, rsp + 8);
	write_reg64(r1, v);
}

void instr64_F7_2_mem(int64_t addr)
{
	safe_write64_addr(addr, (uint64_t)not64((int64_t)safe_read64_addr(addr)));
}
void instr64_F7_2_reg(int32_t r1) { write_reg64(r1, (uint64_t)not64((int64_t)read_reg64(r1))); }
void instr64_F7_3_mem(int64_t addr)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)neg64(x));
}
void instr64_F7_3_reg(int32_t r1) { write_reg64(r1, (uint64_t)neg64((int64_t)read_reg64(r1))); }
void instr64_FF_0_mem(int64_t addr)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)inc64(x));
}
void instr64_FF_0_reg(int32_t r1) { write_reg64(r1, (uint64_t)inc64((int64_t)read_reg64(r1))); }
void instr64_FF_1_mem(int64_t addr)
{
	int64_t x = (int64_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, (uint64_t)dec64(x));
}
void instr64_FF_1_reg(int32_t r1) { write_reg64(r1, (uint64_t)dec64((int64_t)read_reg64(r1))); }

/* ================================================================== *
 * THE STACK -- push/pop/pushf/popf, all of them default-64
 *
 * In 64-bit mode the stack pointer is 64 bits and SS.B is IGNORED: a push decrements RSP by 8
 * and stores 8 bytes, a pop loads 8 and increments by 8.  The 16-bit `push imm` forms do not
 * exist in 64-bit mode (the overlay does not list 0x66 0x68/0x6A), and `push r/m` (0xFF /6) is
 * S7's.
 * ================================================================== */

static void push64(uint64_t value)
{
	uint64_t rsp = read_reg64(ESP);
	rsp -= 8;
	safe_write64_addr((int64_t)rsp, value);
	/* a fault leaves RSP un-decremented: the reference aborts the instruction with its
	 * partial effects kept, and the partial effect here IS the successful store */
	if (cpu->pagefault) return;
	write_reg64(ESP, rsp);
}

static bool pop64(uint64_t *value)
{
	uint64_t rsp = read_reg64(ESP);
	uint64_t x = safe_read64_addr((int64_t)rsp);
	if (cpu->pagefault) return false;
	rsp += 8;
	write_reg64(ESP, rsp);
	*value = x;
	return true;
}

/* push r0 -- 0x50+r has no immediate and no modrm, so the generated switch calls the body with
 * no arguments (see the emitted arms); the eight spellings carry the opcode's register. */
void instr64_50(void) {  push64(read_reg64(reg_index_b(0))); }
void instr64_51(void) {  push64(read_reg64(reg_index_b(1))); }
void instr64_52(void) {  push64(read_reg64(reg_index_b(2))); }
void instr64_53(void) {  push64(read_reg64(reg_index_b(3))); }
void instr64_54(void) {  push64(read_reg64(reg_index_b(4))); }
void instr64_55(void) {  push64(read_reg64(reg_index_b(5))); }
void instr64_56(void) {  push64(read_reg64(reg_index_b(6))); }
void instr64_57(void) {  push64(read_reg64(reg_index_b(7))); }

void instr64_58(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(0), v);
}
void instr64_59(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(1), v);
}
void instr64_5A(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(2), v);
}
void instr64_5B(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(3), v);
}
void instr64_5C(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(4), v);
}
void instr64_5D(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(5), v);
}
void instr64_5E(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(6), v);
}
void instr64_5F(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	write_reg64(reg_index_b(7), v);
}

/* push imm8 / push imm32: both SIGN-EXTEND to 64 bits, and that sign extension is the visible
 * difference from the 32-bit forms (the corpus's prologue pushes 0, which cannot show it; a
 * `push -1` stores 8 bytes of 0xFF in long mode and 4 at 32 bits). */
void instr64_6A(int64_t imm) { push64((uint64_t)imm); }
void instr64_68(int64_t imm) { push64((uint64_t)imm); }

/* pushfq / popfq: the stack half is 64 bits, the FLAGS inside are the port's 32-bit `flags`
 * word plus the upper half, which this model does not keep (plan ss4's flags row).  So the
 * pushed value is `flags` zero-extended and bit 1 (the constant one) set at 64 bits; popfq
 * takes the low 32 bits through the reference's own popf32 path so the masking, the IOPL rule
 * and the VM86 check are not re-decided here. */
void instr64_9C(void)
{
	if (instr_pushf_popf_check()) {
		trigger_gp(0);
		return;
	}
	/* VM and RF are cleared in the image stored on the stack (the reference's 32-bit body);
	 * the 64-bit form pushes EIGHT bytes of that same image, which is the difference. */
	push64((uint64_t)(uint32_t)(get_eflags() & 0xFCFFFF));
}
void instr64_9D(void)
{
	uint64_t v;
	int32_t old_eflags;
	if (instr_pushf_popf_check()) {
		trigger_gp(0);
		return;
	}
	old_eflags = cpu->flags;
	if (!pop64(&v)) return;
	/* the low 32 bits are the machine's whole flag word (plan ss4's flags row: the port keeps
	 * `flags` 32-bit and the upper half is not modelled), so the reference's own update path --
	 * its masking and its IOPL/VM86 rule -- decides what the pop installs */
	update_eflags((int32_t)(uint32_t)v);
	if ((old_eflags & FLAG_INTERRUPT) == 0 && (cpu->flags & FLAG_INTERRUPT) != 0) {
		handle_irqs();
	}
}

/* ================================================================== *
 * the stop and the jump
 * ================================================================== */

/* hlt: identical to the 32-bit body (it is a mode-independent machine state), present here
 * because the overlay gives it a 0x2NN arm -- the corpus's epilogue executes it in every
 * image, and a 64-bit dispatch of 0xF4 that fell through to a 32-bit arm would be a silent
 * wrong-length only if the arm did not exist; it is here so the arm exists and says so. */
void instr64_F4(void)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}
	cpu->in_hlt = true;
	after_block_boundary();
}

/* jmp rel8: the TARGET ARITHMETIC is 64-bit in long mode (the 32-bit body adds through an i32
 * and wraps; here the eip is a 64-bit linear address and a rel8 displacement is sign-extended
 * into it).  This is the one instruction in the slice whose 64-bit body differs from its 32-bit
 * twin only by width -- which is exactly why it needs its own body rather than a re-dispatch. */
void instr64_EB(int64_t imm)
{
	int64_t target = (int64_t)cpu->instruction_pointer + (int64_t)(int8_t)imm;
	set_instruction_pointer((uint64_t)target);
	/* S7: NO after_block_boundary -- the reference's table marks jmp rel8
	 * `block_boundary:1, no_block_boundary_in_interpreted:1`, which the emitter reads as "no
	 * postfix in the interpreter", and S5's version called it anyway (harmless: a batch end is
	 * invisible to the compared state, and family 1 stayed green).  The jcc bodies below follow
	 * the reference instead, so they and this body agree. */
}

/* ================================================================== *
 * S7 -- CONTROL FLOW AND STACK AT 64 BITS (plan ss6's S7 row)
 *
 * THE RULES THIS SLICE IS ABOUT, stated where a reader will look for them:
 *   - push/pop/call/ret move EIGHT bytes; RSP is 64 bits and SS.B is IGNORED;
 *   - a near CALL pushes the address of the NEXT instruction -- the fetch cursor AFTER the
 *     displacement has been consumed, which is what `cpu->instruction_pointer` already is by
 *     the time the body runs;
 *   - a near RET pops that value straight into the instruction pointer (cs base is 0 in
 *     64-bit mode, and this slice has no far returns: 0xC2/0xCA/0xCB stay named stops);
 *   - jcc/pc-rel jumps add the SIGN-EXTENDED displacement to the 64-bit cursor;
 *   - the register/index extensions are the same REX rules the mov family uses, because the
 *     emitter's reg_index_* helpers are the same expressions.
 *
 * THE DISCIPLINE (results/x86-64-s6.md ss9's): every body here names the corpus case that
 * proves it, and an encoding without a case is a NAMED STOP, not a body.
 * ================================================================== */

/* ------------------------------------------------------------------ *
 * CALL / RET / JMP -- the far-transfer stops S5 left by name, now bodies
 * ------------------------------------------------------------------ */

/* 0xE8 call rel32.  The corpus case is ctrl-call-ret-64; the wrong implementations it would
 * catch are a FOUR-byte push (the 32-bit arm's shape -- the return address and every
 * subsequent rsp would drift by 4) and a return address taken BEFORE the displacement was
 * consumed (an infinite loop, since the target would be re-entered). */
void instr64_E8(int32_t imm)
{
	push64(cpu->instruction_pointer); /* the next instruction's address: cs base is 0 here */
	if (cpu->pagefault) return;
	set_instruction_pointer(cpu->instruction_pointer + (int64_t)(int32_t)imm);
}

/* 0xC3 ret.  Same case; the wrong implementations it would catch are a FOUR-byte pop (the
 * return address would be half the pushed pair) and popping without advancing rsp. */
void instr64_C3(void)
{
	uint64_t v;
	if (!pop64(&v)) return;
	set_instruction_pointer(v); /* cs base is 0 in 64-bit mode */
}

/* 0xE9 jmp rel32 */
void instr64_E9(int32_t imm)
{
	set_instruction_pointer(cpu->instruction_pointer + (int64_t)(int32_t)imm);
}

/* 0xFF /6 push r/m64 -- ctrl-push-pop-64 (`push qword ptr [rip + body_data0]`).  The S5
 * shutter for this sub-op is now a body; /2 call r/m, /3 callf and /5 jmpf stay named stops
 * (no corpus case covers them, and each is a FAR transfer whose 64-bit form is S11's). */
void instr64_FF_6_mem(int64_t addr)
{
	uint64_t v = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	push64(v);
}
void instr64_FF_6_reg(int32_t r1) { push64(read_reg64(r1)); }

/* 0xFF /4 jmp r/m64 -- ctrl-jmp-indirect.  The wrong implementation it would catch is
 * truncating the target to 32 bits (the corpus's target is above nothing special, but the
 * register form carries a full 64-bit value).  /2 call r/m, /3 callf, /5 jmpf and /6 push r/m
 * stay named stops: no corpus case covers them. */
void instr64_FF_4_mem(int64_t addr)
{
	uint64_t v = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	set_instruction_pointer(v);
}
void instr64_FF_4_reg(int32_t r1) { set_instruction_pointer(read_reg64(r1)); }

/* ------------------------------------------------------------------ *
 * JCC REL8 -- 0x70-0x7F, sixteen one-liners over the port's OWN test_* predicates
 * (misc_instr.h); ctrl-jcc-64 (S6 side-effect green) and every other family-3 branch run
 * through these.  The target arithmetic is the 64-bit cursor plus the sign-extended rel8.
 * ------------------------------------------------------------------ */
/* SPELLED OUT, NOT MACRO-GENERATED, and the reason is MEASURED rather than stylistic: the
 * emitter's availability scan is TEXTUAL (`gen/interpreter_c.js` matches
 * `^void[ \t]+(instr...)[(]` at the start of a line), so a body produced by a macro is
 * INVISIBLE to it.  Round 1 wrote these sixteen with a JMPCC8 macro, the scan reported them
 * absent, every `case 0x27x:` emitted `unimplemented_instruction("instructions::instr64_7x")`,
 * and the review measured ZERO calls to instr64_7x in the tree -- sixteen bodies nobody could
 * reach and sixteen stops that refused the encoding.  (The same trap took MOV_IMM64 in S5 and
 * ALU_IMM8 in S6; three times is a rule, not an accident, which is why the report states it.)
 * The scan's contract is "a definition is a line that starts with `void <name>(`", and these
 * sixteen lines satisfy it. */
void instr64_70(int32_t imm) { if (test_o()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_71(int32_t imm) { if (test_no()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_72(int32_t imm) { if (test_b()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_73(int32_t imm) { if (test_nb()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_74(int32_t imm) { if (test_z()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_75(int32_t imm) { if (test_nz()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_76(int32_t imm) { if (test_be()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_77(int32_t imm) { if (test_nbe()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_78(int32_t imm) { if (test_s()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_79(int32_t imm) { if (test_ns()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_7A(int32_t imm) { if (test_p()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_7B(int32_t imm) { if (test_np()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_7C(int32_t imm) { if (test_l()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_7D(int32_t imm) { if (test_nl()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_7E(int32_t imm) { if (test_le()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }
void instr64_7F(int32_t imm) { if (test_nle()) set_instruction_pointer(cpu->instruction_pointer + (int64_t)imm); }

/* ------------------------------------------------------------------ *
 * LEA (0x8D) -- the ONE instruction whose address computation is the RESULT.  It is
 * `custom_modrm_resolve` (the emitter passes the raw modrm byte), and no memory access
 * happens, so a faulting address does NOT fault: the reference suppresses only the register
 * write, exactly as the 32-bit body does.  ctrl-lea-64 (SIB with scale, and rip-relative).
 * ------------------------------------------------------------------ */
void instr64_8D_mem(int32_t modrm_byte, int32_t r)
{
	int64_t addr = modrm_resolve64(modrm_byte);
	if (!cpu->pagefault) {
		write_reg64(r, (uint64_t)addr);
	}
}
void instr64_8D_reg(int32_t _r, int32_t _r2)
{
	(void)_r;
	(void)_r2;
	trigger_ud(); /* lea with a register operand is #UD, as at 32 bits */
}

/* ------------------------------------------------------------------ *
 * MOVSXD (0x63) -- the 64-bit-only sign extension, and the encoding whose 32-bit meaning
 * (ARPL) does not exist in long mode.  ctrl-movsxd (0x80000000 sign-extends to
 * 0xFFFFFFFF80000000, which is the case a movzd-less port fails).
 * ------------------------------------------------------------------ */
void instr64_63_mem(int64_t addr, int32_t r)
{
	int32_t x = (int32_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (int64_t)x); /* the SIGN extension is the whole instruction */
}
void instr64_63_reg(int32_t r1, int32_t r)
{
	write_reg64(r, (int64_t)(int32_t)read_reg32(r1));
}

/* ------------------------------------------------------------------ *
 * XCHG (0x87) -- ctrl-xchg-64 (r8/r9, i.e. REX.B and REX.R both live).
 * ------------------------------------------------------------------ */
void instr64_87_mem(int64_t addr, int32_t r)
{
	uint64_t m = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	uint64_t reg = read_reg64(r);
	safe_write64_addr(addr, reg);
	if (cpu->pagefault) return;
	write_reg64(r, m);
}
void instr64_87_reg(int32_t r1, int32_t r)
{
	uint64_t a = read_reg64(r1);
	write_reg64(r1, read_reg64(r));
	write_reg64(r, a);
}

/* ------------------------------------------------------------------ *
 * THE 0F ESCAPE IN 64-BIT MODE, and the S7 subset of its space
 * ------------------------------------------------------------------ */

/* the escape's own body: read the second byte and key it with the 0F rule (modrm.c).  This is
 * `case 0x20F` -- the overlay's `entry64: "instr64_0F"` -- and it is the ONLY way a 0F-qualified
 * 0x2NN arm is reachable. */
void instr64_0F(void)
{
	int32_t opcode = read_imm8();
	if (cpu->pagefault) return;
	run_instruction0f_64(opcode);
}

/* the 64-bit cmovcc/setcc/movzx/movsx helpers, over the port's own condition predicates */
static void cmovcc64(bool condition, uint64_t value, int32_t r)
{
	if (condition) write_reg64(r, value);
}

/* 0F 44/45 -- cmovcc r64, r/m64.  The memory form reads BEFORE the condition is applied (the
 * reference's shape), so a faulting cmov leaves the destination untouched.  ctrl-cmov. */
void instr64_0F44_mem(int64_t addr, int32_t r)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	cmovcc64(test_z(), x, r);
}
void instr64_0F44_reg(int32_t r1, int32_t r) { cmovcc64(test_z(), read_reg64(r1), r); }
void instr64_0F45_mem(int64_t addr, int32_t r)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	cmovcc64(test_nz(), x, r);
}
void instr64_0F45_reg(int32_t r1, int32_t r) { cmovcc64(test_nz(), read_reg64(r1), r); }

/* 0F 94/95 -- setcc r/m8.  A BYTE store: the destination register keeps its other bytes, and
 * the REX-aware byte mapping (cpu_state.c) is what picks SPL/BPL/SIL/DIL over AH/CH/DH/BH.
 * ctrl-setcc. */
void instr64_0F94_mem(int64_t addr, int32_t _r2)
{
	(void)_r2;
	safe_write8_addr(addr, test_z() ? 1 : 0);
}
void instr64_0F94_reg(int32_t r1, int32_t _r2)
{
	(void)_r2;
	write_reg8(r1, test_z() ? 1 : 0);
}
void instr64_0F95_mem(int64_t addr, int32_t _r2)
{
	(void)_r2;
	safe_write8_addr(addr, test_nz() ? 1 : 0);
}
void instr64_0F95_reg(int32_t r1, int32_t _r2)
{
	(void)_r2;
	write_reg8(r1, test_nz() ? 1 : 0);
}

/* 0F B6/B7/BE/BF -- movzx/movsx r64, r/m8|16.  The extension is the whole instruction, and
 * ctrl-movzx-movsx drives all four through one source byte/word. */
void instr64_0FB6_mem(int64_t addr, int32_t r)
{
	int32_t x = safe_read8_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)(uint8_t)x);
}
void instr64_0FB6_reg(int32_t r1, int32_t r) { write_reg64(r, (uint64_t)(uint8_t)read_reg8(r1)); }
void instr64_0FB7_mem(int64_t addr, int32_t r)
{
	int32_t x = (int32_t)safe_read64_addr(addr); /* a 16-bit read; the straddle path is S7's too */
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)(uint16_t)x & 0xFFFFu);
}
void instr64_0FB7_reg(int32_t r1, int32_t r) { write_reg64(r, (uint64_t)(uint16_t)read_reg16(r1)); }
void instr64_0FBE_mem(int64_t addr, int32_t r)
{
	int32_t x = safe_read8_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)(int64_t)(int8_t)x);
}
void instr64_0FBE_reg(int32_t r1, int32_t r) { write_reg64(r, (uint64_t)(int64_t)(int8_t)read_reg8(r1)); }
void instr64_0FBF_mem(int64_t addr, int32_t r)
{
	int16_t x = (int16_t)safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, (uint64_t)(int64_t)x);
}
void instr64_0FBF_reg(int32_t r1, int32_t r) { write_reg64(r, (uint64_t)(int64_t)(int16_t)read_reg16(r1)); }
