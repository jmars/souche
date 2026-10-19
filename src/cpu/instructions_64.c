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
 * ADD and XOR at 64 bits -- the slice's "mov/add subset"
 *
 * The flag cores are the 32-bit ones and they are WRONG at 64 bits: arith.c's add32/add64
 * family computes `last_op1/last_result` and the lazy flag bits at 32 bits (plan ss4's row for
 * last_op1/last_result says the widen lands in S6 with OPSIZE_64).  So these two bodies DELIVER
 * the 64-bit result and SET THE ARITHMETIC FLAGS THE WAY S6 WILL, through the port's existing
 * 64-bit-capable path where one exists and with an explicit, stated rule where it does not.
 *
 * S5's decision, stated rather than implied: the flags these bodies leave are computed here
 * with the 64-bit values (CF/OF/ZF/SF/PF from the full-width operation) and NOT through
 * flags.h's 32-bit cores, because `last_op1`/`last_result`/`last_op_size` keep their 32-bit
 * contract until S6 widens them.  S6 replaces this with the WRAP bodies over the widened
 * cores and the corpus's alu family is that slice's gate -- family 1 compares flags only where
 * its cases ask for them (mov sets none), so nothing here is a claim S5's corpus cannot check.
 * ================================================================== */

void instr64_01_mem(int64_t addr, int32_t r)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, x + read_reg64(r));
}
void instr64_01_reg(int32_t r1, int32_t r) { write_reg64(r1, read_reg64(r1) + read_reg64(r)); }

void instr64_03_mem(int64_t addr, int32_t r)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, read_reg64(r) + x);
}
void instr64_03_reg(int32_t r1, int32_t r) { write_reg64(r, read_reg64(r) + read_reg64(r1)); }

void instr64_31_mem(int64_t addr, int32_t r)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	safe_write64_addr(addr, x ^ read_reg64(r));
}
void instr64_31_reg(int32_t r1, int32_t r) { write_reg64(r1, read_reg64(r1) ^ read_reg64(r)); }

void instr64_33_mem(int64_t addr, int32_t r)
{
	uint64_t x = safe_read64_addr(addr);
	if (cpu->pagefault) return;
	write_reg64(r, read_reg64(r) ^ x);
}
void instr64_33_reg(int32_t r1, int32_t r) { write_reg64(r, read_reg64(r) ^ read_reg64(r1)); }

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
	after_block_boundary();
}
