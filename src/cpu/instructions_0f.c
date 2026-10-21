/*
 * instructions_0f.c -- the 0F opcode table: every group that is NOT MMX/SSE.
 *
 * Port of v86/src/rust/cpu/instructions_0f.rs (5300 lines).  M1b-1 (HANDOFF=v86c)
 * implemented 359 of the 390 symbols results/m1b-scope-non-sse.txt names -- see
 * results/m1b1-0f-nonsse.md for the scope actually implemented and the divergence table
 * that accounts for every symbol that is still missing (0F 00 /2../5, 0F 02/03, 0F 30-32,
 * 0F 34/35, 0F C7 /6).  The file was created one unit earlier than plan ss7 step 13
 * assigns it, for tests/test.asm's sake: movzx/movsx (0F B6/B7/BE/BF) are what made the
 * M0 transcript reachable, and pxor (66 0F EF) is the shared prologue of all 123 nasm
 * test sources.  Those three slices are kept at the bottom of this file, unchanged.
 *
 * The names are the emitter's (gen/generate_interpreter.js:87-101): `os` encodings split
 * into the 16- and 32-bit cases, so `instr16_0FB6_mem` / `instr32_0FB6_reg` / ...  Every
 * remaining 0F encoding is emitted by gen/interpreter_c.js as an unimplemented call that
 * NAMES the missing symbol; the emitter's count is the measure of what is left.
 *
 * The two wrappers run_instruction0f_16/32 are upstream's cpu.rs:3132-3133 verbatim: the
 * 0F switch is the same function for both, with the operand size carried in bit 8 for the
 * 32-bit form.  instructions.h declares them; instructions.c's instr16_0F/instr32_0F are
 * their only callers.
 *
 * PORTING NOTES (the classes of silent-wrong this file had to be careful about):
 *   - `*cr` is `cpu->cr[0]`, `*sreg.offset(X)` is `cpu->sreg[X]` (cpu_state.h ss2).  The
 *     offsets are gone; the ORDER of the members is upstream's.
 *   - `return_on_pagefault!(x)` is "call x, then `if (cpu->pagefault) return;` before the
 *     value is used" (cpu_state.h's plumbing note) -- the fault check sits exactly where
 *     upstream's `?` short-circuits, so the fault path's partial effects match.
 *   - upstream's `.unwrap()` on a result it has already proved fault-free becomes
 *     DBG_ASSERT(!cpu->pagefault), the misc_instr.c rule.
 *   - dbg_log! has no channel in the port and is DROPPED, its text kept as a comment.
 *   - dbg_assert!(false, ...) inside undefined_instruction/unimplemented_sse is dropped
 *     rather than turned into DBG_ASSERT: it is debug_assert! (dbg.rs:63), i.e. compiled
 *     out of the pinned release reference, and its condition is a property of the GUEST's
 *     BYTES.  Aborting on those would be a divergence from the reference, not one of the
 *     port's stops, which is the same ruling the emitter records for the prefix
 *     fallthrough (interpreter_gen.c's header, "THE FALLTHROUGH ARMS DO NOT ASSERT").
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cpu_backend.h" /* S2: CPUID's long-mode declaration follows the capability (ss3.5.1) */
#include "instructions_0f.h"

#include "arith.h"
#include "cpu.h"
#include "cpu_state.h"
#include "exceptions.h" /* M1c: the segment-descriptor API, load_tr/load_ldt, update_cs_size */
#include "misc_instr.h"
#include "paging.h"

/* Sign extension through uint32_t: the M1a-2 D5 rule.  Rust's `x << 16 >> 16` shifts into
 * the sign bit, which is UNDEFINED BEHAVIOUR in C on an int32_t, so the shift is done on
 * the unsigned pattern and cast back -- bit-exact, the same spelling instructions.c uses.
 * Two users in this file: movsx (0F BE/BF) and the 16-bit memory forms of the bt group
 * (0F A3/AB/B3/BB), whose bit offset upstream computes as `read_reg16(r) << 16 >> 16`. */
#define SEX8(x)  ((int32_t)((uint32_t)(x) << 24) >> 24)
#define SEX16(x) ((int32_t)((uint32_t)(x) << 16) >> 16)

/* ------------------------------------------------------------------ *
 * upstream's two local stop helpers (instructions_0f.rs:3-10)
 * ------------------------------------------------------------------ */

/* undefined_instruction(): dbg_assert!(false, "Undefined instructions") then trigger_ud().
 * The assert is compiled out of the reference (see the file header), so the C body is the
 * #UD alone. */
static void undefined_instruction(void) { trigger_ud(); }

/* unimplemented_sse(): the same shape -- the reference raises #UD for the encoding. */
static void unimplemented_sse(void) { trigger_ud(); }

/* cpu.rs:3132-3133 */
void run_instruction0f_16(int32_t opcode) { run_instruction0f(opcode); }
void run_instruction0f_32(int32_t opcode) { run_instruction0f(opcode | 0x100); }
/* S7: the third arm.  The generated 0F switch gains `case 0x2NN:` cases only for the second
 * bytes gen/x86_table64.js's 0F-qualified table lists, so an unlisted byte can never select a
 * 64-bit arm here -- and this wrapper is reached ONLY through instructions_64.c's instr64_0F,
 * which only a long-mode dispatch of the escape reaches. */
void run_instruction0f_64(int32_t opcode) { run_instruction0f(opcode | 0x200); }

/* ------------------------------------------------------------------ *
 * 0F 00 -- sldt, str
 *
 * /2../5 (lldt, ltr, verr, verw) were M1b-1's deferral: their bodies are defined over
 * cpu.rs's segment-descriptor API (lookup_segment_selector, cpu.rs:2640-2740).  M1c landed
 * them, with 0F 02/03 and the rest of the deferral list -- their definitions are at the
 * BOTTOM of this file, under "THE LAST 31 SYMBOLS OF THE 0F TABLE".
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:35-42 -- sldt */
void instr16_0F00_0_mem(int32_t addr)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	safe_write16(addr, (int32_t)cpu->sreg[LDTR]);
	if (cpu->pagefault) return;
}
void instr32_0F00_0_mem(int32_t addr) { instr16_0F00_0_mem(addr); }

void instr16_0F00_0_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg16(r, (int32_t)cpu->sreg[LDTR]);
}
void instr32_0F00_0_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg32(r, (int32_t)cpu->sreg[LDTR]);
}

/* instructions_0f.rs:63-70 -- str */
void instr16_0F00_1_mem(int32_t addr)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	safe_write16(addr, (int32_t)cpu->sreg[TR]);
	if (cpu->pagefault) return;
}
void instr32_0F00_1_mem(int32_t addr) { instr16_0F00_1_mem(addr); }

void instr16_0F00_1_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg16(r, (int32_t)cpu->sreg[TR]);
}
void instr32_0F00_1_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg32(r, (int32_t)cpu->sreg[TR]);
}

/* ------------------------------------------------------------------ *
 * 0F 01 -- sgdt, sidt, lgdt, lidt, smsw, lmsw, invlpg   (instructions_0f.rs:200-324)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:200-206 */
static void sgdt(int32_t addr, int32_t mask)
{
	writable_or_pagefault(addr, 6);
	if (cpu->pagefault) return;
	safe_write16(addr, (int32_t)cpu->gdtr_size);
	DBG_ASSERT(!cpu->pagefault); /* upstream's .unwrap() */
	/* S3: the base is 64-bit now; upstream's `offset & mask` is 32-bit arithmetic, so the
	 * projection is explicit here rather than an implicit narrowing at safe_write32 */
	safe_write32(addr + 2, (int32_t)((uint32_t)cpu->gdtr_offset & (uint32_t)mask));
	DBG_ASSERT(!cpu->pagefault); /* upstream's .unwrap() */
}
void instr16_0F01_0_mem(int32_t addr) { sgdt(addr, 0xFFFFFF); }
void instr32_0F01_0_mem(int32_t addr) { sgdt(addr, -1); }
void instr16_0F01_0_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr32_0F01_0_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}

/* instructions_0f.rs:215-221 */
static void sidt(int32_t addr, int32_t mask)
{
	writable_or_pagefault(addr, 6);
	if (cpu->pagefault) return;
	safe_write16(addr, (int32_t)cpu->idtr_size);
	DBG_ASSERT(!cpu->pagefault); /* upstream's .unwrap() */
	safe_write32(addr + 2, (int32_t)((uint32_t)cpu->idtr_offset & (uint32_t)mask));
	DBG_ASSERT(!cpu->pagefault); /* upstream's .unwrap() */
}
void instr16_0F01_1_mem(int32_t addr) { sidt(addr, 0xFFFFFF); }
void instr32_0F01_1_mem(int32_t addr) { sidt(addr, -1); }
void instr16_0F01_1_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr32_0F01_1_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}

/* instructions_0f.rs:230-241 */
static void lgdt(int32_t addr, int32_t mask)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}
	int32_t size = safe_read16(addr);
	if (cpu->pagefault) return;
	/* S11/S12 GAP CLOSED: in 64-bit mode LGDT/LIDT take a TEN-BYTE operand -- a 2-byte limit and
	 * an EIGHT-byte base (SDM).  Reading a 32-bit base here kept only the low half of a kernel's
	 * high-half IDT address, so the descriptor read came from the wrong page and
	 * `call_interrupt_vector` saw garbage -- which is exactly the amd64 guest's
	 * `invalid gate type` stop (results/x86-64-idt64.md). */
	int64_t offset;
	if (long_mode_64()) {
		uint32_t low = (uint32_t)safe_read32s(addr + 2);
		uint32_t high;
		if (cpu->pagefault) return;
		high = (uint32_t)safe_read32s(addr + 6);
		if (cpu->pagefault) return;
		offset = (int64_t)(((uint64_t)high << 32) | low);
	}
	else {
		offset = (int64_t)safe_read32s(addr + 2);
		if (cpu->pagefault) return;
	}
	cpu->gdtr_size = size;
	/* widened in S3: a base with bit 31 set is an ADDRESS above 2 GiB, not a negative
	 * number, so it must not sign-extend into the 64-bit field */
	cpu->gdtr_offset = (uint64_t)offset & (uint64_t)(uint32_t)mask;
}
void instr16_0F01_2_mem(int32_t addr) { lgdt(addr, 0xFFFFFF); }
void instr32_0F01_2_mem(int32_t addr) { lgdt(addr, -1); }
void instr16_0F01_2_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr32_0F01_2_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}

/* instructions_0f.rs:250-262 */
static void lidt(int32_t addr, int32_t mask)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}
	int32_t size = safe_read16(addr);
	if (cpu->pagefault) return;
	/* S11/S12 GAP CLOSED: in 64-bit mode LGDT/LIDT take a TEN-BYTE operand -- a 2-byte limit and
	 * an EIGHT-byte base (SDM).  Reading a 32-bit base here kept only the low half of a kernel's
	 * high-half IDT address, so the descriptor read came from the wrong page and
	 * `call_interrupt_vector` saw garbage -- which is exactly the amd64 guest's
	 * `invalid gate type` stop (results/x86-64-idt64.md). */
	int64_t offset;
	if (long_mode_64()) {
		uint32_t low = (uint32_t)safe_read32s(addr + 2);
		uint32_t high;
		if (cpu->pagefault) return;
		high = (uint32_t)safe_read32s(addr + 6);
		if (cpu->pagefault) return;
		offset = (int64_t)(((uint64_t)high << 32) | low);
	}
	else {
		offset = (int64_t)safe_read32s(addr + 2);
		if (cpu->pagefault) return;
	}
	cpu->idtr_size = size;
	cpu->idtr_offset = (uint64_t)offset & (uint64_t)(uint32_t)mask;
}
void instr16_0F01_3_mem(int32_t addr) { lidt(addr, 0xFFFFFF); }
void instr32_0F01_3_mem(int32_t addr) { lidt(addr, -1); }
void instr16_0F01_3_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr32_0F01_3_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}

/* instructions_0f.rs:265-279 -- smsw.  Note both _mem forms write 16 bits. */
void instr16_0F01_4_reg(int32_t r) { write_reg16(r, cpu->cr[0]); }
void instr32_0F01_4_reg(int32_t r) { write_reg32(r, cpu->cr[0]); }
void instr16_0F01_4_mem(int32_t addr)
{
	safe_write16(addr, cpu->cr[0] & 0xFFFF);
	if (cpu->pagefault) return;
}
void instr32_0F01_4_mem(int32_t addr) { instr16_0F01_4_mem(addr); }

/* instructions_0f.rs:282-288 -- lmsw is instructions_0f's own (not cpu.rs's) */
static void lmsw(int32_t new_cr0)
{
	new_cr0 = (cpu->cr[0] & ~15) | (new_cr0 & 15);
	if (cpu->protected_mode) {
		/* lmsw cannot be used to switch back */
		new_cr0 |= CR0_PE;
	}
	set_cr0(new_cr0);
}
void instr16_0F01_6_reg(int32_t r)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}
	lmsw(read_reg16(r));
}
void instr32_0F01_6_reg(int32_t r) { instr16_0F01_6_reg(r); }
void instr16_0F01_6_mem(int32_t addr)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}
	int32_t v = safe_read16(addr);
	if (cpu->pagefault) return;
	lmsw(v);
}
void instr32_0F01_6_mem(int32_t addr) { instr16_0F01_6_mem(addr); }

/* instructions_0f.rs:312-326 -- invlpg */
void instr16_0F01_7_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr32_0F01_7_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr16_0F01_7_mem(int32_t addr)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}
	invlpg(addr);
}
void instr32_0F01_7_mem(int32_t addr) { instr16_0F01_7_mem(addr); }

/* ------------------------------------------------------------------ *
 * 0F 04-0F -- undefined encodings, clts, invd, wbinvd, UD2, prefetch-nop
 *
 * The undefined ones are NOT left to the emitter's unimplemented stop:  upstream's
 * undefined_instruction() raises #UD, and a guest (or a nasm test) that reaches one must
 * see #UD, not a stopped machine.  0F 05 is one of them -- this pinned reference has no
 * syscall (instructions_0f.rs:415); sysenter/sysexit are 0F 34/35.
 * ------------------------------------------------------------------ */

void instr_0F04(void) { undefined_instruction(); }
void instr_0F05(void) { undefined_instruction(); }

/* instructions_0f.rs:417-431 -- clts */
void instr_0F06(void)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
	}
	else {
		cpu->cr[0] &= ~CR0_TS;
	}
}
void instr_0F07(void) { undefined_instruction(); }

/* instructions_0f.rs:433-436 -- invd */
void instr_0F08(void) { undefined_instruction(); }

/* instructions_0f.rs:438-448 -- wbinvd: no caches are modelled */
void instr_0F09(void)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
	}
	else {
		/* wbinvd */
	}
}
void instr_0F0A(void) { undefined_instruction(); }

/* instructions_0f.rs:449-454 -- UD2 */
void instr_0F0B(void) { trigger_ud(); }

void instr_0F0C(void) { undefined_instruction(); }

/* instructions_0f.rs:456-463 -- nop (upstream calls undefined_instruction: 0F 0D is a
 * prefetch hint with a modrm byte; the reference raises #UD) */
void instr_0F0D(void) { undefined_instruction(); }
void instr_0F0E(void) { undefined_instruction(); }
void instr_0F0F(void) { undefined_instruction(); }

/* ------------------------------------------------------------------ *
 * 0F 18-1F -- prefetch hints and reserved nops
 * ------------------------------------------------------------------ */

void instr_0F18_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	/* reserved nop */
}
void instr_0F18_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	/* prefetch -- nop for us */
}
void instr_0F19_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
}
void instr_0F19_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
}
void instr_0F1A(void) { undefined_instruction(); }
void instr_0F1B(void) { undefined_instruction(); }
void instr_0F1C_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
}
void instr_0F1C_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
}
void instr_0F1D_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
}
void instr_0F1D_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
}
void instr_0F1E_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
}
void instr_0F1E_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
}
void instr_0F1F_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
}
void instr_0F1F_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
}

/* ------------------------------------------------------------------ *
 * 0F 20-27 -- the control- and debug-register moves
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:727-751 -- mov r32, crN */
void instr_0F20(int32_t r, int32_t creg)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}

	switch (creg) {
	case 0:
		write_reg32(r, cpu->cr[0]);
		break;
	case 2:
		write_reg32(r, cpu->cr[2]);
		break;
	case 3:
		write_reg32(r, cpu->cr[3]);
		break;
	case 4:
		write_reg32(r, cpu->cr[4]);
		break;
	default:
		undefined_instruction();
		break;
	}
}

/* instructions_0f.rs:753-779 -- mov r32, drN */
void instr_0F21(int32_t r, int32_t dreg_index)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}

	if (dreg_index == 4 || dreg_index == 5) {
		if ((cpu->cr[4] & CR4_DE) != 0) {
			trigger_ud();
			return;
		}
		else {
			/* DR4 and DR5 refer to DR6 and DR7 respectively */
			dreg_index += 2;
		}
	}
	/* S1: dreg[] is int64_t; the move is still a 32-bit one, so the low half is what the
	 * guest sees (read_reg32 of a wider field would truncate anyway -- this spells it). */
	write_reg32(r, (int32_t)(uint32_t)cpu->dreg[dreg_index]);
}

/* instructions_0f.rs:781-829 -- mov crN, r32 */
void instr_0F22(int32_t r, int32_t creg)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}

	int32_t data = read_reg32(r);
	switch (creg) {
	case 0:
		set_cr0(data);
		break;
	case 2:
		cpu->cr[2] = data;
		break;
	case 3:
		set_cr3(data);
		break;
	case 4: {
		if (((uint32_t)data &
		     ((1u << 11 | 1u << 12 | 1u << 15 | 1u << 16 | 1u << 19) | 0xFFC00000u)) != 0) {
			/* dbg_log!("trigger_gp: Invalid cr4 bit") dropped */
			trigger_gp(0);
			return;
		}
		else {
			int32_t old_cr4 = cpu->cr[4];
			/* validate BEFORE commit: a #GP from load_pdpte leaves CR4 (and the
			 * TLB) uncommitted -- the guest's #GP handler sees the state hardware
			 * would (MOV CR4 never re-reads PDPTEs on hardware; this is the
			 * emulator's reg_pdpte refresh). */
			/* S2 (plan ss3.5.2, C5): the SDM's OTHER trigger for IA-32e -- CR4.PAE
			 * set while CR0.PG and EFER.LME are -- reaches the same load_pdpte() with
			 * the same PML4-as-PDPT mistake.  Refused by name before the validation
			 * read, and CR4 is left uncommitted.  (The plan names two sites, this is
			 * the third; refusing at two of three would leave the class open.) */
			if ((data & CR4_PAE) != 0 && (old_cr4 & CR4_PAE) == 0 &&
			    (cpu->cr[0] & CR0_PG) != 0 && (cpu->efer & EFER_LME) != 0 &&
			    cpu_backend_refuse_long_mode("CR4.PAE")) {
				return;
			}
			if ((data & CR4_PAE) != 0 &&
			    ((old_cr4 ^ data) & (CR4_PAE | CR4_PGE | CR4_PSE | CR4_SMEP)) != 0 &&
			    (cpu->cr[0] & CR0_PG) != 0 && !load_pdpte(cpu->cr[3])) {
				return;
			}
			if (((old_cr4 ^ data) & (CR4_PGE | CR4_PSE | CR4_PAE)) != 0) {
				full_clear_tlb();
			}
			cpu->cr[4] = data;
			/* S3 MODE BOUNDARY: the second of the three transitions (CR4.PAE set
			 * while CR0.PG && EFER.LME -- the one the plan does not name and this port
			 * refuses above).  After the commit, on the state the write produced. */
			cpu_assert_address_widening("MOV CR4");
		}
		break;
	}
	default:
		undefined_instruction();
		break;
	}
}

/* instructions_0f.rs:831-864 -- mov drN, r32 */
void instr_0F23(int32_t r, int32_t dreg_index)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}

	if (dreg_index == 4 || dreg_index == 5) {
		if ((cpu->cr[4] & CR4_DE) != 0) {
			trigger_ud();
			return;
		}
		else {
			/* DR4 and DR5 refer to DR6 and DR7 respectively */
			dreg_index += 2;
		}
	}
	cpu->dreg[dreg_index] = (uint32_t)read_reg32(r); /* zero-extended (S1) */
}

void instr_0F24(void) { undefined_instruction(); }
void instr_0F25(void) { undefined_instruction(); }
void instr_0F26(void) { undefined_instruction(); }
void instr_0F27(void) { undefined_instruction(); }

/* ------------------------------------------------------------------ *
 * 0F 33, 0F 36/37 and the 0F 38-3F escape range
 *
 * 0F 30/31/32 (wrmsr/rdtsc/rdmsr) and 0F 34/35 (sysenter/sysexit) are NOT ported -- see
 * the header and the unit report.  Everything else in this range is an undefined
 * encoding (reported through unimplemented_sse() upstream, which is #UD in the
 * reference).
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:1359-1362 -- rdpmc */
void instr_0F33(void) { undefined_instruction(); }
void instr_0F36(void) { undefined_instruction(); }

/* instructions_0f.rs:1425-1428 -- getsec */
void instr_0F37(void) { undefined_instruction(); }

void instr_0F38(void) { unimplemented_sse(); }
void instr_0F39(void) { unimplemented_sse(); }
void instr_0F3A(void) { unimplemented_sse(); }
void instr_0F3B(void) { unimplemented_sse(); }
void instr_0F3C(void) { unimplemented_sse(); }
void instr_0F3D(void) { unimplemented_sse(); }
void instr_0F3E(void) { unimplemented_sse(); }
void instr_0F3F(void) { unimplemented_sse(); }

/* ------------------------------------------------------------------ *
 * 0F 40-4F -- cmovcc   (instructions_0f.rs:1446-1573)
 *
 * Upstream unrolls all 64 forms (16 conditions x {mem,reg} x {16,32}) over misc_instr.c's
 * cmovcc16/cmovcc32; the C text is the same 64 one-liners in the same order, so the two can
 * be read side by side.  The condition column is upstream's: O NO B NB Z NZ BE NBE S NS P
 * NP L NL LE NLE.
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:1446-1573 -- cmovcc16/cmovcc32.  The memory form reads BEFORE the
 * condition is applied (upstream: `cmovccX(test_Y(), return_on_pagefault!(safe_readZ(addr)),
 * r)`), so a faulting cmovcc leaves the destination register untouched and returns. */

/* instructions_0f.rs -- 0F 40 (o) */
void instr16_0F40_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_o(), x, r);
}
void instr16_0F40_reg(int32_t r1, int32_t r) { cmovcc16(test_o(), read_reg16(r1), r); }
void instr32_0F40_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_o(), x, r);
}
void instr32_0F40_reg(int32_t r1, int32_t r) { cmovcc32(test_o(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 41 (no) */
void instr16_0F41_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_o(), x, r);
}
void instr16_0F41_reg(int32_t r1, int32_t r) { cmovcc16(!test_o(), read_reg16(r1), r); }
void instr32_0F41_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_o(), x, r);
}
void instr32_0F41_reg(int32_t r1, int32_t r) { cmovcc32(!test_o(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 42 (b) */
void instr16_0F42_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_b(), x, r);
}
void instr16_0F42_reg(int32_t r1, int32_t r) { cmovcc16(test_b(), read_reg16(r1), r); }
void instr32_0F42_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_b(), x, r);
}
void instr32_0F42_reg(int32_t r1, int32_t r) { cmovcc32(test_b(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 43 (nb) */
void instr16_0F43_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_b(), x, r);
}
void instr16_0F43_reg(int32_t r1, int32_t r) { cmovcc16(!test_b(), read_reg16(r1), r); }
void instr32_0F43_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_b(), x, r);
}
void instr32_0F43_reg(int32_t r1, int32_t r) { cmovcc32(!test_b(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 44 (z) */
void instr16_0F44_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_z(), x, r);
}
void instr16_0F44_reg(int32_t r1, int32_t r) { cmovcc16(test_z(), read_reg16(r1), r); }
void instr32_0F44_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_z(), x, r);
}
void instr32_0F44_reg(int32_t r1, int32_t r) { cmovcc32(test_z(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 45 (nz) */
void instr16_0F45_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_z(), x, r);
}
void instr16_0F45_reg(int32_t r1, int32_t r) { cmovcc16(!test_z(), read_reg16(r1), r); }
void instr32_0F45_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_z(), x, r);
}
void instr32_0F45_reg(int32_t r1, int32_t r) { cmovcc32(!test_z(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 46 (be) */
void instr16_0F46_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_be(), x, r);
}
void instr16_0F46_reg(int32_t r1, int32_t r) { cmovcc16(test_be(), read_reg16(r1), r); }
void instr32_0F46_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_be(), x, r);
}
void instr32_0F46_reg(int32_t r1, int32_t r) { cmovcc32(test_be(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 47 (nbe) */
void instr16_0F47_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_be(), x, r);
}
void instr16_0F47_reg(int32_t r1, int32_t r) { cmovcc16(!test_be(), read_reg16(r1), r); }
void instr32_0F47_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_be(), x, r);
}
void instr32_0F47_reg(int32_t r1, int32_t r) { cmovcc32(!test_be(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 48 (s) */
void instr16_0F48_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_s(), x, r);
}
void instr16_0F48_reg(int32_t r1, int32_t r) { cmovcc16(test_s(), read_reg16(r1), r); }
void instr32_0F48_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_s(), x, r);
}
void instr32_0F48_reg(int32_t r1, int32_t r) { cmovcc32(test_s(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 49 (ns) */
void instr16_0F49_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_s(), x, r);
}
void instr16_0F49_reg(int32_t r1, int32_t r) { cmovcc16(!test_s(), read_reg16(r1), r); }
void instr32_0F49_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_s(), x, r);
}
void instr32_0F49_reg(int32_t r1, int32_t r) { cmovcc32(!test_s(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 4A (p) */
void instr16_0F4A_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_p(), x, r);
}
void instr16_0F4A_reg(int32_t r1, int32_t r) { cmovcc16(test_p(), read_reg16(r1), r); }
void instr32_0F4A_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_p(), x, r);
}
void instr32_0F4A_reg(int32_t r1, int32_t r) { cmovcc32(test_p(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 4B (np) */
void instr16_0F4B_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_p(), x, r);
}
void instr16_0F4B_reg(int32_t r1, int32_t r) { cmovcc16(!test_p(), read_reg16(r1), r); }
void instr32_0F4B_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_p(), x, r);
}
void instr32_0F4B_reg(int32_t r1, int32_t r) { cmovcc32(!test_p(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 4C (l) */
void instr16_0F4C_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_l(), x, r);
}
void instr16_0F4C_reg(int32_t r1, int32_t r) { cmovcc16(test_l(), read_reg16(r1), r); }
void instr32_0F4C_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_l(), x, r);
}
void instr32_0F4C_reg(int32_t r1, int32_t r) { cmovcc32(test_l(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 4D (nl) */
void instr16_0F4D_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_l(), x, r);
}
void instr16_0F4D_reg(int32_t r1, int32_t r) { cmovcc16(!test_l(), read_reg16(r1), r); }
void instr32_0F4D_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_l(), x, r);
}
void instr32_0F4D_reg(int32_t r1, int32_t r) { cmovcc32(!test_l(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 4E (le) */
void instr16_0F4E_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(test_le(), x, r);
}
void instr16_0F4E_reg(int32_t r1, int32_t r) { cmovcc16(test_le(), read_reg16(r1), r); }
void instr32_0F4E_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(test_le(), x, r);
}
void instr32_0F4E_reg(int32_t r1, int32_t r) { cmovcc32(test_le(), read_reg32(r1), r); }

/* instructions_0f.rs -- 0F 4F (nle) */
void instr16_0F4F_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	cmovcc16(!test_le(), x, r);
}
void instr16_0F4F_reg(int32_t r1, int32_t r) { cmovcc16(!test_le(), read_reg16(r1), r); }
void instr32_0F4F_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	cmovcc32(!test_le(), x, r);
}
void instr32_0F4F_reg(int32_t r1, int32_t r) { cmovcc32(!test_le(), read_reg32(r1), r); }


/* ------------------------------------------------------------------ *
 * 0F 78-7D -- the 0F 78-7D holes, plus haddpd/haddps/hsubpd/hsubps
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:3004-3015 etc -- the plain 0F 78..7D encodings are not SSE
 * instructions in this table: upstream routes them through unimplemented_sse(), which is
 * #UD in the reference. */
void instr_0F78(void) { unimplemented_sse(); }
void instr_0F79(void) { unimplemented_sse(); }
void instr_0F7A(void) { unimplemented_sse(); }
void instr_0F7B(void) { unimplemented_sse(); }
void instr_0F7C(void) { unimplemented_sse(); }
void instr_0F7D(void) { unimplemented_sse(); }

/* instructions_0f.rs:3018-3094 -- haddpd/haddps/hsubpd/hsubps (66/F2 0F 7C/7D).
 *
 * These four ARE SSE3 instructions, but the scope split (results/m1b-scope-notes.md: the
 * MMX/SSE list is "0F 10-17, 28-2F, 50-5F, 60-6F, 70-77, 7E/7F, C2-C6, D0-FE") puts 0F 7C
 * and 0F 7D in THIS unit's file, and they are plain float/double arithmetic over reg128 --
 * no softfloat, no x87, no sse_instr.rs -- so they are ported here rather than left as a
 * hole between the two scope lists.  REPORTED as an assignment, not a preference.
 *
 * The pair-wise sum/difference ORDER is upstream's and is not commutative: haddpd writes
 * {dest0+dest1, src0+src1} into the SAME destination register it read. */
static void instr_660F7C(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	result.f64[0] = destination.f64[0] + destination.f64[1];
	result.f64[1] = source.f64[0] + source.f64[1];
	write_xmm_reg128(r, result);
}
void instr_660F7C_reg(int32_t r1, int32_t r2) { instr_660F7C(read_xmm128s(r1), r2); }
void instr_660F7C_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F7C(x, r);
}

/* instructions_0f.rs:3035-3050 -- haddps */
static void instr_F20F7C(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	result.f32[0] = destination.f32[0] + destination.f32[1];
	result.f32[1] = destination.f32[2] + destination.f32[3];
	result.f32[2] = source.f32[0] + source.f32[1];
	result.f32[3] = source.f32[2] + source.f32[3];
	write_xmm_reg128(r, result);
}
void instr_F20F7C_reg(int32_t r1, int32_t r2) { instr_F20F7C(read_xmm128s(r1), r2); }
void instr_F20F7C_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F20F7C(x, r);
}

/* instructions_0f.rs:3052-3066 -- hsubpd */
static void instr_660F7D(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	result.f64[0] = destination.f64[0] - destination.f64[1];
	result.f64[1] = source.f64[0] - source.f64[1];
	write_xmm_reg128(r, result);
}
void instr_660F7D_reg(int32_t r1, int32_t r2) { instr_660F7D(read_xmm128s(r1), r2); }
void instr_660F7D_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F7D(x, r);
}

/* instructions_0f.rs:3079-3093 -- hsubps */
static void instr_F20F7D(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	result.f32[0] = destination.f32[0] - destination.f32[1];
	result.f32[1] = destination.f32[2] - destination.f32[3];
	result.f32[2] = source.f32[0] - source.f32[1];
	result.f32[3] = source.f32[2] - source.f32[3];
	write_xmm_reg128(r, result);
}
void instr_F20F7D_reg(int32_t r1, int32_t r2) { instr_F20F7D(read_xmm128s(r1), r2); }
void instr_F20F7D_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F20F7D(x, r);
}

/* ------------------------------------------------------------------ *
 * 0F 80-8F -- jcc rel16/rel32   (instructions_0f.rs:3162-3193)
 *
 * The immediate is read by the generated dispatcher and passed in, exactly as upstream's
 * signature -- so the displacement is consumed before anything else in the body, the
 * M1a-4 argument-evaluation-order rule.
 * ------------------------------------------------------------------ */

void instr16_0F80(int32_t imm) { jmpcc16(test_o(), imm); }   /* jo */
void instr32_0F80(int32_t imm) { jmpcc32(test_o(), imm); }

void instr16_0F81(int32_t imm) { jmpcc16(!test_o(), imm); }   /* jno */
void instr32_0F81(int32_t imm) { jmpcc32(!test_o(), imm); }

void instr16_0F82(int32_t imm) { jmpcc16(test_b(), imm); }   /* jb */
void instr32_0F82(int32_t imm) { jmpcc32(test_b(), imm); }

void instr16_0F83(int32_t imm) { jmpcc16(!test_b(), imm); }   /* jnb */
void instr32_0F83(int32_t imm) { jmpcc32(!test_b(), imm); }

void instr16_0F84(int32_t imm) { jmpcc16(test_z(), imm); }   /* jz */
void instr32_0F84(int32_t imm) { jmpcc32(test_z(), imm); }

void instr16_0F85(int32_t imm) { jmpcc16(!test_z(), imm); }   /* jnz */
void instr32_0F85(int32_t imm) { jmpcc32(!test_z(), imm); }

void instr16_0F86(int32_t imm) { jmpcc16(test_be(), imm); }   /* jbe */
void instr32_0F86(int32_t imm) { jmpcc32(test_be(), imm); }

void instr16_0F87(int32_t imm) { jmpcc16(!test_be(), imm); }   /* jnbe */
void instr32_0F87(int32_t imm) { jmpcc32(!test_be(), imm); }

void instr16_0F88(int32_t imm) { jmpcc16(test_s(), imm); }   /* js */
void instr32_0F88(int32_t imm) { jmpcc32(test_s(), imm); }

void instr16_0F89(int32_t imm) { jmpcc16(!test_s(), imm); }   /* jns */
void instr32_0F89(int32_t imm) { jmpcc32(!test_s(), imm); }

void instr16_0F8A(int32_t imm) { jmpcc16(test_p(), imm); }   /* jp */
void instr32_0F8A(int32_t imm) { jmpcc32(test_p(), imm); }

void instr16_0F8B(int32_t imm) { jmpcc16(!test_p(), imm); }   /* jnp */
void instr32_0F8B(int32_t imm) { jmpcc32(!test_p(), imm); }

void instr16_0F8C(int32_t imm) { jmpcc16(test_l(), imm); }   /* jl */
void instr32_0F8C(int32_t imm) { jmpcc32(test_l(), imm); }

void instr16_0F8D(int32_t imm) { jmpcc16(!test_l(), imm); }   /* jnl */
void instr32_0F8D(int32_t imm) { jmpcc32(!test_l(), imm); }

void instr16_0F8E(int32_t imm) { jmpcc16(test_le(), imm); }   /* jle */
void instr32_0F8E(int32_t imm) { jmpcc32(test_le(), imm); }

void instr16_0F8F(int32_t imm) { jmpcc16(!test_le(), imm); }   /* jnle */
void instr32_0F8F(int32_t imm) { jmpcc32(!test_le(), imm); }


/* ------------------------------------------------------------------ *
 * 0F 90-9F -- setcc r/m8   (instructions_0f.rs:3195-3226)
 *
 * The second parameter is the modrm `reg` field, unused by both bodies (upstream names it
 * `_`); it is kept so the emitter's two-argument call lines up.
 * ------------------------------------------------------------------ */

void instr_0F90_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_o(), r); }  /* seto */
void instr_0F90_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_o(), addr); }

void instr_0F91_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_o(), r); }  /* setno */
void instr_0F91_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_o(), addr); }

void instr_0F92_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_b(), r); }  /* setb */
void instr_0F92_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_b(), addr); }

void instr_0F93_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_b(), r); }  /* setnb */
void instr_0F93_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_b(), addr); }

void instr_0F94_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_z(), r); }  /* setz */
void instr_0F94_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_z(), addr); }

void instr_0F95_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_z(), r); }  /* setnz */
void instr_0F95_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_z(), addr); }

void instr_0F96_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_be(), r); }  /* setbe */
void instr_0F96_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_be(), addr); }

void instr_0F97_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_be(), r); }  /* setnbe */
void instr_0F97_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_be(), addr); }

void instr_0F98_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_s(), r); }  /* sets */
void instr_0F98_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_s(), addr); }

void instr_0F99_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_s(), r); }  /* setns */
void instr_0F99_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_s(), addr); }

void instr_0F9A_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_p(), r); }  /* setp */
void instr_0F9A_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_p(), addr); }

void instr_0F9B_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_p(), r); }  /* setnp */
void instr_0F9B_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_p(), addr); }

void instr_0F9C_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_l(), r); }  /* setl */
void instr_0F9C_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_l(), addr); }

void instr_0F9D_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_l(), r); }  /* setnl */
void instr_0F9D_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_l(), addr); }

void instr_0F9E_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(test_le(), r); }  /* setle */
void instr_0F9E_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(test_le(), addr); }

void instr_0F9F_reg(int32_t r, int32_t _r2) { (void)_r2; setcc_reg(!test_le(), r); }  /* setnle */
void instr_0F9F_mem(int32_t addr, int32_t _r2) { (void)_r2; setcc_mem(!test_le(), addr); }


/* ------------------------------------------------------------------ *
 * 0F A0-AF -- push/pop fs, cpuid, bt, shld, the obsolete cmpxchg, push/pop gs,
 *             rsm, bts, shrd, the 0F AE group, imul
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:3228-3254 -- push fs / pop fs */
void instr16_0FA0(void)
{
	push16((int32_t)cpu->sreg[FS]);
	if (cpu->pagefault) return;
}
void instr32_0FA0(void)
{
	push32_sreg(FS);
	if (cpu->pagefault) return;
}
void instr16_0FA1(void)
{
	int32_t v = safe_read16(get_stack_pointer(0));
	if (cpu->pagefault) return;
	if (!switch_seg(FS, v)) {
		return;
	}
	else {
		adjust_stack_reg(2);
		return;
	}
}
void instr32_0FA1(void)
{
	int32_t v = safe_read32s(get_stack_pointer(0)) & 0xFFFF;
	if (cpu->pagefault) return;
	if (!switch_seg(FS, v)) {
		return;
	}
	else {
		adjust_stack_reg(4);
		return;
	}
}

/* cpu.rs:296-298 -- TSC_RATE=1e6 and cpuid_level=0x16; config.rs:3
 * VMWARE_HYPERVISOR_PORT=true.  cpu.rs:4761's set_cpuid_level (the JS hook that lowers the
 * level for Windows NT) is not ported -- the C port has no JS half -- so the level is the
 * constant. */
#define TSC_RATE 1000000.0
#define VMWARE_HYPERVISOR_PORT 1
static const int32_t cpuid_level = 0x16;

/* instructions_0f.rs:3256-3419 -- cpuid.  Every leaf and every constant is upstream's; the
 * dbg_log! calls (and the trailing "cpuid: eax=..." block) are dropped. */
void instr_0FA2(void)
{
	int32_t eax = 0;
	int32_t ecx = 0;
	int32_t edx = 0;
	int32_t ebx = 0;

	uint32_t level = (uint32_t)read_reg32(EAX);

	switch (level) {
	case 0:
		/* maximum supported level (default 0x16, overwritten to 2 as a workaround for
		 * Windows NT) */
		eax = cpuid_level;
		ebx = 0x756E6547; /* Genu */
		edx = 0x49656E69; /* ineI */
		ecx = 0x6C65746E; /* ntel */
		break;

	case 1: {
		eax = 3 | 7 << 4 | 6 << 8; /* pentium3 */
		ebx = 1 << 16 | 8 << 8;    /* cpu count, clflush size */
		ecx = 1 << 0 | 1 << 23 | 1 << 30; /* sse3, popcnt, rdrand */
		int32_t vme = 0 << 1;
		if (VMWARE_HYPERVISOR_PORT) {
			ecx |= (int32_t)0x80000000u; /* hypervisor */
		}
		edx = 1 | /* fpu */
		      vme | 1 << 3 | 1 << 4 | 1 << 5 | 1 << 6 | /* vme, pse, tsc, msr, pae */
		      1 << 8 | 1 << 11 | 1 << 13 | 1 << 15 | 1 << 16 | 1 << 19 | /* cx8, sep,
		                                                                     pge, cmov,
		                                                                     pat, clflush */
		      1 << 23 | 1 << 24 | 1 << 25 | 1 << 26; /* mmx, fxsr, sse1, sse2 */

		if (cpu->acpi_enabled) {
			edx |= 1 << 9; /* apic */
		}
		break;
	}

	case 2:
		/* Taken from http://siyobik.info.gf/main/reference/instruction/CPUID */
		eax = 0x665B5001;
		ebx = 0;
		ecx = 0;
		edx = 0x007A7000;
		break;

	case 4:
		/* from my local machine */
		switch (read_reg32(ECX)) {
		case 0:
			eax = 0x00000121;
			ebx = 0x01c0003f;
			ecx = 0x0000003f;
			edx = 0x00000001;
			break;
		case 1:
			eax = 0x00000122;
			ebx = 0x01c0003f;
			ecx = 0x0000003f;
			edx = 0x00000001;
			break;
		case 2:
			eax = 0x00000143;
			ebx = 0x05c0003f;
			ecx = 0x00000fff;
			edx = 0x00000001;
			break;
		default:
			break;
		}
		break;

	case 5:
		/* from my local machine */
		eax = 0x40;
		ebx = 0x40;
		ecx = 3;
		edx = 0x00142120;
		break;

	case 7:
		if (read_reg32(ECX) == 0) {
			eax = 0;     /* maximum supported sub-level */
			ebx = 1 << 9; /* enhanced REP MOVSB/STOSB */
			ecx = 0;
			edx = 0;
		}
		break;

	case 0x80000000:
		/* maximum supported extended level; other registers are reserved */
		eax = (int32_t)0x80000008u;
		break;

	case 0x80000001: {
		int32_t vme = 0 << 1;
		edx = 1 | /* fpu */
		      vme | 1 << 3 | 1 << 4 | 1 << 5 | 1 << 6 | /* vme, pse, tsc, msr, pae */
		      1 << 8 | 1 << 13 | 1 << 15 | 1 << 20 |   /* cx8, pge, cmov, nx */
		      1 << 23 | 1 << 24;                       /* mmx, fxsr */
		/* S2 (plan ss3.5.1): THE DECLARATION FOLLOWS THE CAPABILITY.  CPUID's long-mode
		 * bits are returned only by a backend that can RUN long mode, so the bits and
		 * the refusal cannot disagree -- which is what makes a guest kernel's own CPUID
		 * check a gate on the SEAM rather than on the decoder.  Today every engine
		 * declares CPU_MODES_32BIT, so this adds nothing: EDX stays 0x0190A179, exactly
		 * as the pinned reference has it, and the battery does not move.
		 *
		 * bit 29 lm, bit 11 syscall.  DELIBERATELY NOT bit 26 pdpe1gb, which the plan's
		 * ss3.5.1 list includes: that bit declares 1 GiB PAGES, and this port does not
		 * have them (they are S11's).  Coupling it to "long mode is available" instead of
		 * to "1 GiB pages work" is exactly the false-declaration class (an ACPI-FADT
		 * table describing a device that is not there) the same plan paragraph is
		 * arguing against; it goes in with the code that implements it. */
		if (cpu_backend_can(CPU_MODE_LONG)) {
			edx |= 1 << 29; /* lm */
			edx |= 1 << 11; /* syscall */
		}
		break;
	}

	case 0x40000000:
		/* hypervisor */
		if (VMWARE_HYPERVISOR_PORT) {
			ebx = 0x61774D56; /* VMwa */
			ecx = 0x4D566572; /* reVM */
			edx = 0x65726177; /* ware */
		}
		break;

	case 0x80000008:
		/* S2 (plan ss3.5.1): the LINEAR width is 48 only for a backend that can run long
		 * mode; the PHYSICAL width is the port's own 32-bit bound (plan ss5.1) either
		 * way and is not a function of the backend.  A 32-bit machine reporting a 36-bit
		 * linear width here would be the same false declaration as `lm` with no long
		 * mode: 80000008h is the leaf a 64-bit kernel reads to size its page tables. */
		eax = 32 | ((cpu_backend_can(CPU_MODE_LONG) ? 48 : 32) << 8);
		ebx = 0;
		ecx = 0;
		edx = 0;
		break;

	case 0x15:
		eax = 1; /* denominator */
		ebx = 1; /* numerator */
		ecx = (int32_t)(uint32_t)(TSC_RATE * 1000.0); /* core crystal clock freq in Hz */
		DBG_ASSERT(ecx > 0);
		/* (TSC frequency = core crystal clock frequency * EBX/EAX) */
		break;

	case 0x16:
		eax = (int32_t)(uint32_t)floor(TSC_RATE / 1000.0); /* core base freq in MHz */
		ebx = (int32_t)(uint32_t)floor(TSC_RATE / 1000.0); /* core max freq in MHz */
		ecx = 10; /* bus (reference) frequency in MHz */

		/* 16-bit values */
		DBG_ASSERT(eax < 0x10000);
		DBG_ASSERT(ebx < 0x10000);
		DBG_ASSERT(ecx < 0x10000);
		break;

	default:
		/* dbg_log!("cpuid: unimplemented eax: {:x}", x) dropped */
		break;
	}

	write_reg32(EAX, eax);
	write_reg32(ECX, ecx);
	write_reg32(EDX, edx);
	write_reg32(EBX, ebx);
}

/* instructions_0f.rs:3420-3423 -- bt */
void instr16_0FA3_reg(int32_t r1, int32_t r2) { bt_reg(read_reg16(r1), read_reg16(r2) & 15); }
void instr16_0FA3_mem(int32_t addr, int32_t r) { bt_mem(addr, SEX16(read_reg16(r))); }
void instr32_0FA3_reg(int32_t r1, int32_t r2) { bt_reg(read_reg32(r1), read_reg32(r2) & 31); }
void instr32_0FA3_mem(int32_t addr, int32_t r) { bt_mem(addr, read_reg32(r)); }

/* instructions_0f.rs:3424-3453 -- shld (the memory form is a read-modify-write: the
 * SAFE_READ_WRITE* macro is the port's spelling of upstream's `safe_read_write*(addr,
 * &|x| ...)` closure, paging.h:115-140). */
void instr16_0FA4_mem(int32_t addr, int32_t r, int32_t imm)
{
	SAFE_READ_WRITE16(addr, shld16(x_, read_reg16(r), imm & 31));
}
void instr16_0FA4_reg(int32_t r1, int32_t r, int32_t imm)
{
	write_reg16(r1, shld16(read_reg16(r1), read_reg16(r), imm & 31));
}
void instr32_0FA4_mem(int32_t addr, int32_t r, int32_t imm)
{
	SAFE_READ_WRITE32(addr, shld32(x_, read_reg32(r), imm & 31));
}
void instr32_0FA4_reg(int32_t r1, int32_t r, int32_t imm)
{
	write_reg32(r1, shld32(read_reg32(r1), read_reg32(r), imm & 31));
}
void instr16_0FA5_mem(int32_t addr, int32_t r)
{
	SAFE_READ_WRITE16(addr, shld16(x_, read_reg16(r), read_reg8(CL) & 31));
}
void instr16_0FA5_reg(int32_t r1, int32_t r)
{
	write_reg16(r1, shld16(read_reg16(r1), read_reg16(r), read_reg8(CL) & 31));
}
void instr32_0FA5_mem(int32_t addr, int32_t r)
{
	SAFE_READ_WRITE32(addr, shld32(x_, read_reg32(r), read_reg8(CL) & 31));
}
void instr32_0FA5_reg(int32_t r1, int32_t r)
{
	write_reg32(r1, shld32(read_reg32(r1), read_reg32(r), read_reg8(CL) & 31));
}

/* instructions_0f.rs:3455-3460 -- obsolete cmpxchg (os/2) */
void instr_0FA6(void) { trigger_ud(); }
void instr_0FA7(void) { undefined_instruction(); }

/* instructions_0f.rs:3461-3487 -- push gs / pop gs */
void instr16_0FA8(void)
{
	push16((int32_t)cpu->sreg[GS]);
	if (cpu->pagefault) return;
}
void instr32_0FA8(void)
{
	push32_sreg(GS);
	if (cpu->pagefault) return;
}
void instr16_0FA9(void)
{
	int32_t v = safe_read16(get_stack_pointer(0));
	if (cpu->pagefault) return;
	if (!switch_seg(GS, v)) {
		return;
	}
	else {
		adjust_stack_reg(2);
		return;
	}
}
void instr32_0FA9(void)
{
	int32_t v = safe_read32s(get_stack_pointer(0)) & 0xFFFF;
	if (cpu->pagefault) return;
	if (!switch_seg(GS, v)) {
		return;
	}
	else {
		adjust_stack_reg(4);
		return;
	}
}

/* instructions_0f.rs:3489-3492 -- rsm */
void instr_0FAA(void) { undefined_instruction(); }

/* instructions_0f.rs:3494-3504 -- bts */
void instr16_0FAB_reg(int32_t r1, int32_t r2)
{
	write_reg16(r1, bts_reg(read_reg16(r1), read_reg16(r2) & 15));
}
void instr16_0FAB_mem(int32_t addr, int32_t r) { bts_mem(addr, SEX16(read_reg16(r))); }
void instr32_0FAB_reg(int32_t r1, int32_t r2)
{
	write_reg32(r1, bts_reg(read_reg32(r1), read_reg32(r2) & 31));
}
void instr32_0FAB_mem(int32_t addr, int32_t r) { bts_mem(addr, read_reg32(r)); }

/* instructions_0f.rs:3505-3534 -- shrd */
void instr16_0FAC_mem(int32_t addr, int32_t r, int32_t imm)
{
	SAFE_READ_WRITE16(addr, shrd16(x_, read_reg16(r), imm & 31));
}
void instr16_0FAC_reg(int32_t r1, int32_t r, int32_t imm)
{
	write_reg16(r1, shrd16(read_reg16(r1), read_reg16(r), imm & 31));
}
void instr32_0FAC_mem(int32_t addr, int32_t r, int32_t imm)
{
	SAFE_READ_WRITE32(addr, shrd32(x_, read_reg32(r), imm & 31));
}
void instr32_0FAC_reg(int32_t r1, int32_t r, int32_t imm)
{
	write_reg32(r1, shrd32(read_reg32(r1), read_reg32(r), imm & 31));
}
void instr16_0FAD_mem(int32_t addr, int32_t r)
{
	SAFE_READ_WRITE16(addr, shrd16(x_, read_reg16(r), read_reg8(CL) & 31));
}
void instr16_0FAD_reg(int32_t r1, int32_t r)
{
	write_reg16(r1, shrd16(read_reg16(r1), read_reg16(r), read_reg8(CL) & 31));
}
void instr32_0FAD_mem(int32_t addr, int32_t r)
{
	SAFE_READ_WRITE32(addr, shrd32(x_, read_reg32(r), read_reg8(CL) & 31));
}
void instr32_0FAD_reg(int32_t r1, int32_t r)
{
	write_reg32(r1, shrd32(read_reg32(r1), read_reg32(r), read_reg8(CL) & 31));
}

/* ------------------------------------------------------------------ *
 * 0F AE -- the 0F AE group (instructions_0f.rs:3536-3599)
 *
 * /0 fxsave and /1 fxrstor are the CROSS-UNIT CONTRACT (instructions_0f.h): fpu.c (M1b-3)
 * defines them and this file only calls them -- no stub, because a stub of x87 state
 * save/restore would SILENTLY keep the wrong FPU state on a guest that runs fsave/frstor.
 * /4../6 _mem (xsave/xrstor/xsaveopt) are undefined_instruction() upstream.
 * ------------------------------------------------------------------ */
void instr_0FAE_0_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr_0FAE_0_mem(int32_t addr) { fxsave(addr); }
void instr_0FAE_1_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr_0FAE_1_mem(int32_t addr) { fxrstor(addr); }

/* instructions_0f.rs:3544-3558 -- ldmxcsr */
void instr_0FAE_2_reg(int32_t _r)
{
	(void)_r;
	unimplemented_sse();
}
void instr_0FAE_2_mem(int32_t addr)
{
	int32_t new_mxcsr = safe_read32s(addr);
	if (cpu->pagefault) return;
	if ((new_mxcsr & ~MXCSR_MASK) != 0) {
		trigger_gp(0);
		return;
	}
	else {
		set_mxcsr(new_mxcsr);
		return;
	}
}

/* instructions_0f.rs:3560-3599 -- stmxcsr */
void instr_0FAE_3_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr_0FAE_3_mem(int32_t addr)
{
	safe_write32(addr, cpu->mxcsr);
	if (cpu->pagefault) return;
}

void instr_0FAE_4_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr_0FAE_4_mem(int32_t _addr)
{
	(void)_addr;
	/* xsave */
	undefined_instruction();
}
void instr_0FAE_5_reg(int32_t _r)
{
	(void)_r;
	/* lfence */
}
void instr_0FAE_5_mem(int32_t _addr)
{
	(void)_addr;
	/* xrstor */
	undefined_instruction();
}
void instr_0FAE_6_reg(int32_t _r)
{
	(void)_r;
	/* mfence */
}
void instr_0FAE_6_mem(int32_t _addr)
{
	(void)_addr;
	/* xsaveopt */
	undefined_instruction();
}
void instr_0FAE_7_reg(int32_t _r)
{
	(void)_r;
	/* sfence */
}
void instr_0FAE_7_mem(int32_t addr)
{
	/* clflush: no hardware caches are modelled, but the operand must pass the same
	 * address translation and permission checks as a byte load. */
	translate_address_read(addr);
	if (cpu->pagefault) return;
}

/* instructions_0f.rs:3600-3617 -- imul reg, r/m */
void instr16_0FAF_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, imul_reg16(read_reg16(r), x));
}
void instr16_0FAF_reg(int32_t r1, int32_t r) { write_reg16(r, imul_reg16(read_reg16(r), read_reg16(r1))); }
void instr32_0FAF_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	write_reg32(r, imul_reg32(read_reg32(r), x));
}
void instr32_0FAF_reg(int32_t r1, int32_t r) { write_reg32(r, imul_reg32(read_reg32(r), read_reg32(r1))); }

/* ------------------------------------------------------------------ *
 * 0F B0-B5 -- cmpxchg, lss/lfs/lgs, btr
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:3620-3626 -- cmpxchg.  The AL/AX/EAX comparison and the flag update
 * live in arith.c's cmpxchg8/16/32 (arith.rs); the memory forms are read-modify-writes. */
void instr_0FB0_reg(int32_t r1, int32_t r2) { write_reg8(r1, cmpxchg8(read_reg8(r1), r2)); }
void instr_0FB0_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, cmpxchg8(x_, r)); }
void instr16_0FB1_reg(int32_t r1, int32_t r2) { write_reg16(r1, cmpxchg16(read_reg16(r1), r2)); }
void instr16_0FB1_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, cmpxchg16(x_, r)); }
void instr32_0FB1_reg(int32_t r1, int32_t r2) { write_reg32(r1, cmpxchg32(read_reg32(r1), r2)); }
void instr32_0FB1_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, cmpxchg32(x_, r)); }

/* instructions_0f.rs:3629-3635 -- lss/lfs/lgs */
void instr16_0FB2_reg(int32_t _unused, int32_t _unused2)
{
	(void)_unused;
	(void)_unused2;
	trigger_ud();
}
void instr16_0FB2_mem(int32_t addr, int32_t r) { lss16(addr, r, SS); }
void instr32_0FB2_reg(int32_t _unused, int32_t _unused2)
{
	(void)_unused;
	(void)_unused2;
	trigger_ud();
}
void instr32_0FB2_mem(int32_t addr, int32_t r) { lss32(addr, r, SS); }

/* instructions_0f.rs:3637-3647 -- btr */
void instr16_0FB3_reg(int32_t r1, int32_t r2)
{
	write_reg16(r1, btr_reg(read_reg16(r1), read_reg16(r2) & 15));
}
void instr16_0FB3_mem(int32_t addr, int32_t r) { btr_mem(addr, SEX16(read_reg16(r))); }
void instr32_0FB3_reg(int32_t r1, int32_t r2)
{
	write_reg32(r1, btr_reg(read_reg32(r1), read_reg32(r2) & 31));
}
void instr32_0FB3_mem(int32_t addr, int32_t r) { btr_mem(addr, read_reg32(r)); }

void instr16_0FB4_reg(int32_t _unused, int32_t _unused2)
{
	(void)_unused;
	(void)_unused2;
	trigger_ud();
}
void instr16_0FB4_mem(int32_t addr, int32_t r) { lss16(addr, r, FS); }
void instr32_0FB4_reg(int32_t _unused, int32_t _unused2)
{
	(void)_unused;
	(void)_unused2;
	trigger_ud();
}
void instr32_0FB4_mem(int32_t addr, int32_t r) { lss32(addr, r, FS); }
void instr16_0FB5_reg(int32_t _unused, int32_t _unused2)
{
	(void)_unused;
	(void)_unused2;
	trigger_ud();
}
void instr16_0FB5_mem(int32_t addr, int32_t r) { lss16(addr, r, GS); }
void instr32_0FB5_reg(int32_t _unused, int32_t _unused2)
{
	(void)_unused;
	(void)_unused2;
	trigger_ud();
}
void instr32_0FB5_mem(int32_t addr, int32_t r) { lss32(addr, r, GS); }

/* ------------------------------------------------------------------ *
 * 0F B8/B9 -- popcnt (F3 0F B8) and the plain encodings, UD2
 *
 * The plain 0F B8 and 0F B9 are #UD: F3 0F B8 is the popcnt the table maps, and anything
 * without the F3 prefix is invalid on this CPU model (upstream trigger_ud()/instr_0FB9).
 * ------------------------------------------------------------------ */
void instr16_0FB8_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr16_0FB8_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr16_F30FB8_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, popcnt(x));
}
void instr16_F30FB8_reg(int32_t r1, int32_t r) { write_reg16(r, popcnt(read_reg16(r1))); }
void instr32_0FB8_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr32_0FB8_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr32_F30FB8_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	write_reg32(r, popcnt(x));
}
void instr32_F30FB8_reg(int32_t r1, int32_t r) { write_reg32(r, popcnt(read_reg32(r1))); }

/* instructions_0f.rs:3697-3700 -- UD2 */
void instr_0FB9(void) { trigger_ud(); }

/* ------------------------------------------------------------------ *
 * 0F BA -- the bt/bts/btr/btc immediate group (instructions_0f.rs:3701-3740)
 *
 * /4 bt, /5 bts, /6 btr, /7 btc.  The immediate is masked with 15 (16-bit) or 31 (32-bit)
 * by the INSTRUCTION, not by the arith.c core -- the register forms of 0F A3/AB/B3/BB mask
 * the register operand the same way.
 * ------------------------------------------------------------------ */
void instr16_0FBA_4_reg(int32_t r, int32_t imm) { bt_reg(read_reg16(r), imm & 15); }
void instr16_0FBA_4_mem(int32_t addr, int32_t imm) { bt_mem(addr, imm & 15); }
void instr16_0FBA_5_reg(int32_t r, int32_t imm) { write_reg16(r, bts_reg(read_reg16(r), imm & 15)); }
void instr16_0FBA_5_mem(int32_t addr, int32_t imm) { bts_mem(addr, imm & 15); }
void instr16_0FBA_6_reg(int32_t r, int32_t imm) { write_reg16(r, btr_reg(read_reg16(r), imm & 15)); }
void instr16_0FBA_6_mem(int32_t addr, int32_t imm) { btr_mem(addr, imm & 15); }
void instr16_0FBA_7_reg(int32_t r, int32_t imm) { write_reg16(r, btc_reg(read_reg16(r), imm & 15)); }
void instr16_0FBA_7_mem(int32_t addr, int32_t imm) { btc_mem(addr, imm & 15); }
void instr32_0FBA_4_reg(int32_t r, int32_t imm) { bt_reg(read_reg32(r), imm & 31); }
void instr32_0FBA_4_mem(int32_t addr, int32_t imm) { bt_mem(addr, imm & 31); }
void instr32_0FBA_5_reg(int32_t r, int32_t imm) { write_reg32(r, bts_reg(read_reg32(r), imm & 31)); }
void instr32_0FBA_5_mem(int32_t addr, int32_t imm) { bts_mem(addr, imm & 31); }
void instr32_0FBA_6_reg(int32_t r, int32_t imm) { write_reg32(r, btr_reg(read_reg32(r), imm & 31)); }
void instr32_0FBA_6_mem(int32_t addr, int32_t imm) { btr_mem(addr, imm & 31); }
void instr32_0FBA_7_reg(int32_t r, int32_t imm) { write_reg32(r, btc_reg(read_reg32(r), imm & 31)); }
void instr32_0FBA_7_mem(int32_t addr, int32_t imm) { btc_mem(addr, imm & 31); }

/* instructions_0f.rs:3742-3752 -- btc */
void instr16_0FBB_reg(int32_t r1, int32_t r2)
{
	write_reg16(r1, btc_reg(read_reg16(r1), read_reg16(r2) & 15));
}
void instr16_0FBB_mem(int32_t addr, int32_t r) { btc_mem(addr, SEX16(read_reg16(r))); }
void instr32_0FBB_reg(int32_t r1, int32_t r2)
{
	write_reg32(r1, btc_reg(read_reg32(r1), read_reg32(r2) & 31));
}
void instr32_0FBB_mem(int32_t addr, int32_t r) { btc_mem(addr, read_reg32(r)); }

/* instructions_0f.rs:3753-3788 -- bsf/bsr */
void instr16_0FBC_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, bsf16(read_reg16(r), x));
}
void instr16_0FBC_reg(int32_t r1, int32_t r) { write_reg16(r, bsf16(read_reg16(r), read_reg16(r1))); }
void instr32_0FBC_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	write_reg32(r, bsf32(read_reg32(r), x));
}
void instr32_0FBC_reg(int32_t r1, int32_t r) { write_reg32(r, bsf32(read_reg32(r), read_reg32(r1))); }
void instr16_0FBD_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, bsr16(read_reg16(r), x));
}
void instr16_0FBD_reg(int32_t r1, int32_t r) { write_reg16(r, bsr16(read_reg16(r), read_reg16(r1))); }
void instr32_0FBD_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	write_reg32(r, bsr32(read_reg32(r), x));
}
void instr32_0FBD_reg(int32_t r1, int32_t r) { write_reg32(r, bsr32(read_reg32(r), read_reg32(r1))); }

/* ------------------------------------------------------------------ *
 * 0F C0/C1 -- xadd   (instructions_0f.rs:3806-3812)
 * ------------------------------------------------------------------ */
void instr_0FC0_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE8(addr, xadd8(x_, r)); }
void instr_0FC0_reg(int32_t r1, int32_t r) { write_reg8(r1, xadd8(read_reg8(r1), r)); }
void instr16_0FC1_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE16(addr, xadd16(x_, r)); }
void instr16_0FC1_reg(int32_t r1, int32_t r) { write_reg16(r1, xadd16(read_reg16(r1), r)); }
void instr32_0FC1_mem(int32_t addr, int32_t r) { SAFE_READ_WRITE32(addr, xadd32(x_, r)); }
void instr32_0FC1_reg(int32_t r1, int32_t r) { write_reg32(r1, xadd32(read_reg32(r1), r)); }

/* ------------------------------------------------------------------ *
 * 0F C7 -- the cmpxchg8b group (instructions_0f.rs:3971-4018)
 *
 * /1 is cmpxchg8b; /6 is rdrand, whose REGISTER form IS ported now (see the group at the
 * rdrand definition, and the correction recorded there -- as #UD it broke the bzImage boot
 * gate) and whose MEMORY form is #UD, as upstream's own body for it is.  /1 with a register
 * operand is #UD.
 * ------------------------------------------------------------------ */
void instr16_0FC7_1_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr32_0FC7_1_reg(int32_t _r)
{
	(void)_r;
	trigger_ud();
}
void instr16_0FC7_1_mem(int32_t addr)
{
	/* cmpxchg8b */
	writable_or_pagefault(addr, 8);
	if (cpu->pagefault) return;
	uint64_t m64 = safe_read64s(addr);
	DBG_ASSERT(!cpu->pagefault); /* upstream's .unwrap() */
	int32_t m64_low = (int32_t)m64;
	int32_t m64_high = (int32_t)(m64 >> 32);
	if (read_reg32(EAX) == m64_low && read_reg32(EDX) == m64_high) {
		cpu->flags |= FLAG_ZERO;
		safe_write64(addr,
		             (uint64_t)(uint32_t)read_reg32(EBX) |
		                 ((uint64_t)(uint32_t)read_reg32(ECX) << 32));
		DBG_ASSERT(!cpu->pagefault); /* upstream's .unwrap() */
	}
	else {
		cpu->flags &= ~FLAG_ZERO;
		write_reg32(EAX, m64_low);
		write_reg32(EDX, m64_high);
	}
	cpu->flags_changed &= ~FLAG_ZERO;
}
void instr32_0FC7_1_mem(int32_t addr) { instr16_0FC7_1_mem(addr); }

void instr16_0FC7_6_mem(int32_t _addr)
{
	(void)_addr;
	trigger_ud();
}
void instr32_0FC7_6_mem(int32_t _addr)
{
	(void)_addr;
	trigger_ud();
}

/* ------------------------------------------------------------------ *
 * 0F C8-CF -- bswap   (instructions_0f.rs:4021-4035)
 * ------------------------------------------------------------------ */
void instr_0FC8(void) { bswap(EAX); }
void instr_0FC9(void) { bswap(ECX); }
void instr_0FCA(void) { bswap(EDX); }
void instr_0FCB(void) { bswap(EBX); }
void instr_0FCC(void) { bswap(ESP); }
void instr_0FCD(void) { bswap(EBP); }
void instr_0FCE(void) { bswap(ESI); }
void instr_0FCF(void) { bswap(EDI); }

/* instructions_0f.rs:5296-5300 -- Windows 98 */
void instr_0FFF(void)
{
	/* dbg_log!("#ud: 0F FF") dropped */
	trigger_ud();
}

/* ------------------------------------------------------------------ *
 * THE THREE EARLY SLICES (M1a-4), unchanged
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:3664-3679 -- movzx */
void instr16_0FB6_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read8(addr);
	if (cpu->pagefault) return;
	write_reg16(r, x);
}
void instr16_0FB6_reg(int32_t r1, int32_t r) { write_reg16(r, read_reg8(r1)); }
void instr32_0FB6_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read8(addr);
	if (cpu->pagefault) return;
	write_reg32(r, x);
}
void instr32_0FB6_reg(int32_t r1, int32_t r) { write_reg32(r, read_reg8(r1)); }

void instr16_0FB7_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, x);
}
void instr16_0FB7_reg(int32_t r1, int32_t r) { write_reg16(r, read_reg16(r1)); }
void instr32_0FB7_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg32(r, x);
}
void instr32_0FB7_reg(int32_t r1, int32_t r) { write_reg32(r, read_reg16(r1)); }

/* instructions_0f.rs:3789-3804 -- movsx.  Sign extension through uint32_t, the M1a-2 D5
 * rule (Rust's `x << 24 >> 24` shifts into the sign bit, which is UB in C on an int32_t);
 * the SEX8/SEX16 macros are defined at the top of this file because the bt group's 16-bit
 * memory forms need SEX16 as well. */
void instr16_0FBE_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read8(addr);
	if (cpu->pagefault) return;
	write_reg16(r, SEX8(x));
}
void instr16_0FBE_reg(int32_t r1, int32_t r) { write_reg16(r, SEX8(read_reg8(r1))); }
void instr32_0FBE_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read8(addr);
	if (cpu->pagefault) return;
	write_reg32(r, SEX8(x));
}
void instr32_0FBE_reg(int32_t r1, int32_t r) { write_reg32(r, SEX8(read_reg8(r1))); }

void instr16_0FBF_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, SEX16(x));
}
void instr16_0FBF_reg(int32_t r1, int32_t r) { write_reg16(r, SEX16(read_reg16(r1))); }
void instr32_0FBF_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg32(r, SEX16(x));
}
void instr32_0FBF_reg(int32_t r1, int32_t r) { write_reg32(r, SEX16(read_reg16(r1))); }

/* ------------------------------------------------------------------ *
 * THE SSE SLICE THE nasm CORPUS'S SHARED PROLOGUE FORCES
 *
 * Every one of the 123 v86/tests/nasm test sources opens with the SAME preamble, and its
 * last instruction is `pxor xmm0, xmm0` (66 0F EF /r).  Without it NO test reaches its
 * own code: the whole corpus stops at one opcode in the preamble, which would make the
 * differential gate vacuous (0 run, 123 skipped for the same reason).  So this slice is
 * the corpus's own requirement, not an arbitrary early start on M1b -- reported as such.
 *
 * The rest of sse_instr.rs (361 lines) stays M1b's; every other SSE encoding the emitter
 * reaches still stops with its own name, and each such stop is a per-test skip reason in
 * the gate's output.
 * ------------------------------------------------------------------ */

/* sse_instr.rs:53-61 */
static void instr_660FEF(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	result.u64[0] = source.u64[0] ^ destination.u64[0];
	result.u64[1] = source.u64[1] ^ destination.u64[1];
	write_xmm_reg128(r, result);
}
void instr_660FEF_reg(int32_t r1, int32_t r2) { instr_660FEF(read_xmm128s(r1), r2); }
void instr_660FEF_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FEF(x, r);
}

/* The MMX/SSE helpers live in their own translation unit (sse_instr.c); the include sits
 * here rather than in the file's header block so that this unit's edit to instructions_0f.c
 * stays APPEND-ONLY (M1b-1's 377 definitions above are untouched). */
#include "sse_instr.h"

/* ================================================================== *
 * M1b-2 (HANDOFF=v86c): THE MMX/SSE GROUPS OF THE 0F TABLE
 *
 * Appended to M1b-1's 377 definitions, which are untouched above (results/m1b1-0f-nonsse.md
 * is their report; the brief is an APPEND-ONLY edit).  Scope: the 533 call sites
 * results/m1b-scope-mmx-sse.txt names -- 0F 10-17, 28-2F, 50-5F, 60-6F, 70-77, 7E/7F,
 * C2-C6 and D0-FE, plus the 66/F2/F3 arms of those opcodes.  Ported 1:1 from
 * v86/src/rust/cpu/instructions_0f.rs; the symbol names are the emitter's exactly
 * (gen/generate_interpreter.js:87-101, mirrored by gen/interpreter_c.js:make_instruction_name).
 *
 * THE SHAPE OF THIS HALF OF THE TABLE, and the reason the base bodies are `static`:
 * the emitter only ever calls the `_mem` and `_reg` forms (plus the three bare names
 * 0F 77 / 0F D0 / 0F F0 -- emms and the two unimplemented_sse opcodes).  Every base body
 * (`instr_0F58(reg128 source, int32_t r)`) is called only from those two wrappers, so it
 * is declared `static`, exactly as M1b-1 did for its hadd/hsub pairs.  That is not
 * cosmetic: gen/interpreter_c.js's availability scan looks for `^void[ \t]+instr...(`,
 * so a `static` body cannot be mistaken for an emitter symbol the port has implemented.
 *
 * PORTING RULES (the same three M1b-1 recorded; all three have real users below):
 *   - Rust's `<<` on a SIGNED integer wraps; C's is UNDEFINED BEHAVIOUR when the
 *     shifted-in bits change the sign.  Every left shift over a possibly-negative lane is
 *     done on the unsigned pattern and cast back (the M1a-2 D5 rule).
 *   - Rust's `>>` on a signed integer is ARITHMETIC; in C that is implementation-defined
 *     and the port relies on gcc/clang (arith.c:131,207 already do).
 *   - Rust's float->int `as` is convert-then-truncate, not saturation; the range check in
 *     front of it is what keeps C's (UB) conversion in range.
 *   - `return_on_pagefault!(f(x))` becomes a local + `if (cpu->pagefault) return;` +
 *     the call, so the fault leaves the same partial effects upstream's `?` does.
 *   - `dbg_log!` is dropped, its text kept as a comment (no log channel in the port).
 * ================================================================== */

/* ------------------------------------------------------------------ *
 * 0F 10-17 -- the mov/unpck family (instructions_0f.rs:465-698)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:465-472 -- movups xmm, xmm/m128 */
static void instr_0F10(reg128 source, int32_t r) { mov_rm_r128(source, r); }
void instr_0F10_reg(int32_t r1, int32_t r2) { instr_0F10(read_xmm128s(r1), r2); }
void instr_0F10_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F10(x, r);
}

/* instructions_0f.rs:473-486 -- movss xmm, xmm/m32 */
void instr_F30F10_reg(int32_t r1, int32_t r2)
{
	reg128 data = read_xmm128s(r1);
	write_xmm32(r2, (int32_t)data.u32[0]);
}
void instr_F30F10_mem(int32_t addr, int32_t r)
{
	int32_t data = safe_read32s(addr);
	if (cpu->pagefault) return;
	write_xmm128(r, data, 0, 0, 0);
}

/* instructions_0f.rs:487-494 -- movupd xmm, xmm/m128 */
static void instr_660F10(reg128 source, int32_t r) { mov_rm_r128(source, r); }
void instr_660F10_reg(int32_t r1, int32_t r2) { instr_660F10(read_xmm128s(r1), r2); }
void instr_660F10_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F10(x, r);
}

/* instructions_0f.rs:495-506 -- movsd xmm, xmm/m64 */
void instr_F20F10_reg(int32_t r1, int32_t r2)
{
	reg128 data = read_xmm128s(r1);
	write_xmm64(r2, data.u64[0]);
}
void instr_F20F10_mem(int32_t addr, int32_t r)
{
	uint64_t data = safe_read64s(addr);
	if (cpu->pagefault) return;
	write_xmm128_2(r, data, 0);
}

/* instructions_0f.rs:507-511 -- movups xmm/m128, xmm */
void instr_0F11_reg(int32_t r1, int32_t r2) { mov_r_r128(r1, r2); }
void instr_0F11_mem(int32_t addr, int32_t r) { mov_r_m128(addr, r); }

/* instructions_0f.rs:512-523 -- movss xmm/m32, xmm */
void instr_F30F11_reg(int32_t rm_dest, int32_t reg_src)
{
	reg128 data = read_xmm128s(reg_src);
	write_xmm32(rm_dest, (int32_t)data.u32[0]);
}
void instr_F30F11_mem(int32_t addr, int32_t r)
{
	reg128 data = read_xmm128s(r);
	safe_write32(addr, (int32_t)data.u32[0]);
	if (cpu->pagefault) return;
}

/* instructions_0f.rs:524-531 -- movupd xmm/m128, xmm */
void instr_660F11_reg(int32_t r1, int32_t r2) { mov_r_r128(r1, r2); }
void instr_660F11_mem(int32_t addr, int32_t r) { mov_r_m128(addr, r); }

/* instructions_0f.rs:532-543 -- movsd xmm/m64, xmm */
void instr_F20F11_reg(int32_t r1, int32_t r2)
{
	reg128 data = read_xmm128s(r2);
	write_xmm64(r1, data.u64[0]);
}
void instr_F20F11_mem(int32_t addr, int32_t r)
{
	uint64_t data = read_xmm64s(r);
	safe_write64(addr, data);
	if (cpu->pagefault) return;
}

/* instructions_0f.rs:544-556 -- movlps xmm, m64 / movhlps xmm, xmm */
void instr_0F12_mem(int32_t addr, int32_t r)
{
	uint64_t data = safe_read64s(addr);
	if (cpu->pagefault) return;
	write_xmm64(r, data);
}
void instr_0F12_reg(int32_t r1, int32_t r2)
{
	reg128 data = read_xmm128s(r1);
	write_xmm64(r2, data.u64[1]);
}

/* instructions_0f.rs:547-552 -- movlpd xmm, m64 (the register form is #UD) */
void instr_660F12_reg(int32_t _r1, int32_t _r)
{
	(void)_r1;
	(void)_r;
	trigger_ud();
}
void instr_660F12_mem(int32_t addr, int32_t r)
{
	uint64_t data = safe_read64s(addr);
	if (cpu->pagefault) return;
	write_xmm64(r, data);
}

/* instructions_0f.rs:563-581 -- movddup xmm1, xmm2/m64 */
static void instr_F20F12(uint64_t source, int32_t r)
{
	reg128 result;
	result.u64[0] = source;
	result.u64[1] = source;
	write_xmm_reg128(r, result);
}
void instr_F20F12_reg(int32_t r1, int32_t r2) { instr_F20F12(read_xmm64s(r1), r2); }
void instr_F20F12_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F12(x, r);
}

/* instructions_0f.rs:568-580 -- movsldup xmm1, xmm2/m128 */
static void instr_F30F12(reg128 source, int32_t r)
{
	reg128 result;
	result.u32[0] = source.u32[0];
	result.u32[1] = source.u32[0];
	result.u32[2] = source.u32[2];
	result.u32[3] = source.u32[2];
	write_xmm_reg128(r, result);
}
void instr_F30F12_reg(int32_t r1, int32_t r2) { instr_F30F12(read_xmm128s(r1), r2); }
void instr_F30F12_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F30F12(x, r);
}

/* instructions_0f.rs:581-590 -- movlps m64, xmm / movlpd xmm/m64, xmm */
void instr_0F13_mem(int32_t addr, int32_t r) { movl_r128_m64(addr, r); }
void instr_0F13_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_660F13_reg(int32_t _r1, int32_t _r)
{
	(void)_r1;
	(void)_r;
	trigger_ud();
}
void instr_660F13_mem(int32_t addr, int32_t r) { movl_r128_m64(addr, r); }

/* instructions_0f.rs:593-626 -- unpcklps / unpcklpd */
static void instr_0F14(uint64_t source, int32_t r)
{
	/* XXX: Aligned access or #gp */
	uint64_t destination = read_xmm64s(r);
	write_xmm128(r,
	             (int32_t)destination,
	             (int32_t)source,
	             (int32_t)(destination >> 32),
	             (int32_t)(source >> 32));
}
void instr_0F14_reg(int32_t r1, int32_t r2) { instr_0F14(read_xmm64s(r1), r2); }
void instr_0F14_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F14(x, r);
}
static void instr_660F14(uint64_t source, int32_t r)
{
	/* XXX: Aligned access or #gp */
	uint64_t destination = read_xmm64s(r);
	write_xmm128(r,
	             (int32_t)destination,
	             (int32_t)(destination >> 32),
	             (int32_t)source,
	             (int32_t)(source >> 32));
}
void instr_660F14_reg(int32_t r1, int32_t r2) { instr_660F14(read_xmm64s(r1), r2); }
void instr_660F14_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_660F14(x, r);
}

/* instructions_0f.rs:627-661 -- unpckhps / unpckhpd */
static void instr_0F15(reg128 source, int32_t r)
{
	/* XXX: Aligned access or #gp */
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)destination.u32[2],
	             (int32_t)source.u32[2],
	             (int32_t)destination.u32[3],
	             (int32_t)source.u32[3]);
}
void instr_0F15_reg(int32_t r1, int32_t r2) { instr_0F15(read_xmm128s(r1), r2); }
void instr_0F15_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F15(x, r);
}
static void instr_660F15(reg128 source, int32_t r)
{
	/* XXX: Aligned access or #gp */
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)destination.u32[2],
	             (int32_t)destination.u32[3],
	             (int32_t)source.u32[2],
	             (int32_t)source.u32[3]);
}
void instr_660F15_reg(int32_t r1, int32_t r2) { instr_660F15(read_xmm128s(r1), r2); }
void instr_660F15_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F15(x, r);
}

/* instructions_0f.rs:662-689 -- movhps xmm, m64 / movlhps xmm, xmm / movhpd / movshdup */
static void instr_0F16(uint64_t source, int32_t r) { cpu->reg_xmm[r].u64[1] = source; }
void instr_0F16_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F16(x, r);
}
void instr_0F16_reg(int32_t r1, int32_t r2) { instr_0F16(read_xmm64s(r1), r2); }
void instr_660F16_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F16(x, r);
}
void instr_660F16_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
static void instr_F30F16(reg128 source, int32_t r)
{
	reg128 result;
	result.u32[0] = source.u32[1];
	result.u32[1] = source.u32[1];
	result.u32[2] = source.u32[3];
	result.u32[3] = source.u32[3];
	write_xmm_reg128(r, result);
}
void instr_F30F16_reg(int32_t r1, int32_t r2) { instr_F30F16(read_xmm128s(r1), r2); }
void instr_F30F16_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F30F16(x, r);
}

/* instructions_0f.rs:690-698 -- movhps m64, xmm / movhpd m64, xmm */
void instr_0F17_mem(int32_t addr, int32_t r) { movh_r128_m64(addr, r); }
void instr_0F17_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_660F17_mem(int32_t addr, int32_t r) { movh_r128_m64(addr, r); }
void instr_660F17_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}

/* ------------------------------------------------------------------ *
 * 0F 28-2F -- movaps/movapd, the conversions, ucomiss/ucomisd/comiss/comisd
 * (instructions_0f.rs:866-1180), plus the helpers they share
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:866-884 -- movaps/movapd xmm, xmm/m128 */
/* XXX: aligned read or #gp, upstream's TODO */
static void instr_0F28(reg128 source, int32_t r) { mov_rm_r128(source, r); }
void instr_0F28_reg(int32_t r1, int32_t r2) { instr_0F28(read_xmm128s(r1), r2); }
void instr_0F28_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F28(x, r);
}
/* Note: Same as movdqa (660F6F) */
static void instr_660F28(reg128 source, int32_t r) { mov_rm_r128(source, r); }
void instr_660F28_reg(int32_t r1, int32_t r2) { instr_660F28(read_xmm128s(r1), r2); }
void instr_660F28_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F28(x, r);
}

/* instructions_0f.rs:885-897 -- movaps/movapd m128, xmm (and the register forms) */
void instr_0F29_mem(int32_t addr, int32_t r)
{
	reg128 data = read_xmm128s(r);
	/* XXX: Aligned write or #gp */
	safe_write128(addr, data);
	if (cpu->pagefault) return;
}
void instr_0F29_reg(int32_t r1, int32_t r2) { mov_r_r128(r1, r2); }
void instr_660F29_mem(int32_t addr, int32_t r)
{
	reg128 data = read_xmm128s(r);
	/* XXX: Aligned write or #gp */
	safe_write128(addr, data);
	if (cpu->pagefault) return;
}
void instr_660F29_reg(int32_t r1, int32_t r2) { mov_r_r128(r1, r2); }

/* instructions_0f.rs:907-931 -- cvtpi2ps / cvtpi2pd.
 *
 * `std::mem::transmute` between u64 and [i32; 2] (and between [f32; 2] and u64) is a
 * little-endian bit reinterpretation, spelled here through the reg128 union rather than
 * a memcpy: same bytes, no aliasing or alignment question. */
static void instr_0F2A(uint64_t source, int32_t r)
{
	/* Note: Casts here can fail -- XXX: Should round according to round control */
	reg128 in, out;
	in.u64[0] = source;
	out.f32[0] = (float)in.i32[0];
	out.f32[1] = (float)in.i32[1];
	write_xmm64(r, out.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F2A_reg(int32_t r1, int32_t r2) { instr_0F2A(read_mmx64s(r1), r2); }
void instr_0F2A_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F2A(x, r);
}
/* These casts can't fail */
static void instr_660F2A(uint64_t source, int32_t r)
{
	reg128 in, result;
	in.u64[0] = source;
	result.f64[0] = (double)in.i32[0];
	result.f64[1] = (double)in.i32[1];
	write_xmm_reg128(r, result);
}
/* NOTE the transition is in the REGISTER wrapper here, not in the body (and the memory
 * form has none at all): that is upstream's layout, kept as written. */
void instr_660F2A_reg(int32_t r1, int32_t r2)
{
	instr_660F2A(read_mmx64s(r1), r2);
	transition_fpu_to_mmx();
}
void instr_660F2A_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_660F2A(x, r);
}
/* This cast can't fail */
static void instr_F20F2A(int32_t source, int32_t r) { write_xmm_f64(r, (double)source); }
void instr_F20F2A_reg(int32_t r1, int32_t r2) { instr_F20F2A(read_reg32(r1), r2); }
void instr_F20F2A_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_F20F2A(x, r);
}
/* Note: This cast can fail -- XXX: Should round according to round control */
static void instr_F30F2A(int32_t source, int32_t r) { write_xmm_f32(r, (float)source); }
void instr_F30F2A_reg(int32_t r1, int32_t r2) { instr_F30F2A(read_reg32(r1), r2); }
void instr_F30F2A_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_F30F2A(x, r);
}

/* instructions_0f.rs:961-972 -- movntps/movntpd m128, xmm */
void instr_0F2B_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_0F2B_mem(int32_t addr, int32_t r)
{
	/* XXX: Aligned write or #gp */
	mov_r_m128(addr, r);
}
void instr_660F2B_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_660F2B_mem(int32_t addr, int32_t r)
{
	/* XXX: Aligned write or #gp */
	mov_r_m128(addr, r);
}

/* instructions_0f.rs:974-1030 -- the truncating conversions */
static void instr_0F2C(uint64_t source, int32_t r)
{
	reg128 in, out;
	in.u64[0] = source;
	out.i32[0] = sse_convert_with_truncation_f32_to_i32(in.f32[0]);
	out.i32[1] = sse_convert_with_truncation_f32_to_i32(in.f32[1]);
	write_mmx_reg64(r, out.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F2C_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F2C(x, r);
}
void instr_0F2C_reg(int32_t r1, int32_t r2) { instr_0F2C(read_xmm64s(r1), r2); }

static void instr_660F2C(reg128 source, int32_t r)
{
	reg128 out;
	out.i32[0] = sse_convert_with_truncation_f64_to_i32(source.f64[0]);
	out.i32[1] = sse_convert_with_truncation_f64_to_i32(source.f64[1]);
	write_mmx_reg64(r, out.u64[0]);
	transition_fpu_to_mmx();
}
void instr_660F2C_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F2C(x, r);
}
void instr_660F2C_reg(int32_t r1, int32_t r2) { instr_660F2C(read_xmm128s(r1), r2); }

static void instr_F20F2C(uint64_t source, int32_t r)
{
	reg128 in;
	in.u64[0] = source;
	write_reg32(r, sse_convert_with_truncation_f64_to_i32(in.f64[0]));
}
void instr_F20F2C_reg(int32_t r1, int32_t r2) { instr_F20F2C(read_xmm64s(r1), r2); }
void instr_F20F2C_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F2C(x, r);
}

static void instr_F30F2C(float source, int32_t r)
{
	write_reg32(r, sse_convert_with_truncation_f32_to_i32(source));
}
void instr_F30F2C_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F2C(x, r);
}
void instr_F30F2C_reg(int32_t r1, int32_t r2) { instr_F30F2C(read_xmm_f32(r1), r2); }

/* instructions_0f.rs:1031-1068 -- the rounding conversions */
static void instr_0F2D(uint64_t source, int32_t r)
{
	reg128 in, out;
	in.u64[0] = source;
	out.i32[0] = sse_convert_f32_to_i32(in.f32[0]);
	out.i32[1] = sse_convert_f32_to_i32(in.f32[1]);
	write_mmx_reg64(r, out.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F2D_reg(int32_t r1, int32_t r2) { instr_0F2D(read_xmm64s(r1), r2); }
void instr_0F2D_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F2D(x, r);
}

static void instr_660F2D(reg128 source, int32_t r)
{
	reg128 out;
	out.i32[0] = sse_convert_f64_to_i32(source.f64[0]);
	out.i32[1] = sse_convert_f64_to_i32(source.f64[1]);
	write_mmx_reg64(r, out.u64[0]);
	transition_fpu_to_mmx();
}
void instr_660F2D_reg(int32_t r1, int32_t r2) { instr_660F2D(read_xmm128s(r1), r2); }
void instr_660F2D_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F2D(x, r);
}
static void instr_F20F2D(uint64_t source, int32_t r)
{
	reg128 in;
	in.u64[0] = source;
	write_reg32(r, sse_convert_f64_to_i32(in.f64[0]));
}
void instr_F20F2D_reg(int32_t r1, int32_t r2) { instr_F20F2D(read_xmm64s(r1), r2); }
void instr_F20F2D_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F2D(x, r);
}
static void instr_F30F2D(float source, int32_t r) { write_reg32(r, sse_convert_f32_to_i32(source)); }
void instr_F30F2D_reg(int32_t r1, int32_t r2) { instr_F30F2D(read_xmm_f32(r1), r2); }
void instr_F30F2D_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F2D(x, r);
}

/* instructions_0f.rs:1079-1103 -- ucomiss xmm1, xmm2/m32 */
static void instr_0F2E(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	cpu->flags_changed = 0;
	cpu->flags &= ~FLAGS_ALL;
	if (destination == source) {
		cpu->flags |= FLAG_ZERO;
	}
	else if (destination < source) {
		cpu->flags |= FLAG_CARRY;
	}
	else if (destination > source) {
		/* all flags cleared */
	}
	else {
		/* TODO: Signal on SNaN */
		cpu->flags |= FLAG_ZERO | FLAG_PARITY | FLAG_CARRY;
	}
}
void instr_0F2E_reg(int32_t r1, int32_t r2) { instr_0F2E(read_xmm_f32(r1), r2); }
void instr_0F2E_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_0F2E(x, r);
}

/* instructions_0f.rs:1104-1129 -- ucomisd xmm1, xmm2/m64 */
static void instr_660F2E(uint64_t source, int32_t r)
{
	reg128 in, arg;
	arg.u64[0] = read_xmm64s(r);
	in.u64[0] = source;
	double destination = arg.f64[0];
	double src = in.f64[0];
	cpu->flags_changed = 0;
	cpu->flags &= ~FLAGS_ALL;
	if (destination == src) {
		cpu->flags |= FLAG_ZERO;
	}
	else if (destination < src) {
		cpu->flags |= FLAG_CARRY;
	}
	else if (destination > src) {
		/* all flags cleared */
	}
	else {
		/* TODO: Signal on SNaN */
		cpu->flags |= FLAG_ZERO | FLAG_PARITY | FLAG_CARRY;
	}
}
void instr_660F2E_reg(int32_t r1, int32_t r2) { instr_660F2E(read_xmm64s(r1), r2); }
void instr_660F2E_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_660F2E(x, r);
}

/* instructions_0f.rs:1130-1154 -- comiss xmm1, xmm2/m32 */
static void instr_0F2F(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	cpu->flags_changed = 0;
	cpu->flags &= ~FLAGS_ALL;
	if (destination == source) {
		cpu->flags |= FLAG_ZERO;
	}
	else if (destination < source) {
		cpu->flags |= FLAG_CARRY;
	}
	else if (destination > source) {
		/* all flags cleared */
	}
	else {
		/* TODO: Signal on SNaN or QNaN */
		cpu->flags |= FLAG_ZERO | FLAG_PARITY | FLAG_CARRY;
	}
}
void instr_0F2F_reg(int32_t r1, int32_t r2) { instr_0F2F(read_xmm_f32(r1), r2); }
void instr_0F2F_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_0F2F(x, r);
}

/* instructions_0f.rs:1155-1180 -- comisd xmm1, xmm2/m64 */
static void instr_660F2F(uint64_t source, int32_t r)
{
	reg128 in, arg;
	arg.u64[0] = read_xmm64s(r);
	in.u64[0] = source;
	double destination = arg.f64[0];
	double src = in.f64[0];
	cpu->flags_changed = 0;
	cpu->flags &= ~FLAGS_ALL;
	if (destination == src) {
		cpu->flags |= FLAG_ZERO;
	}
	else if (destination < src) {
		cpu->flags |= FLAG_CARRY;
	}
	else if (destination > src) {
		/* all flags cleared */
	}
	else {
		/* TODO: Signal on SNaN or QNaN */
		cpu->flags |= FLAG_ZERO | FLAG_PARITY | FLAG_CARRY;
	}
}
void instr_660F2F_reg(int32_t r1, int32_t r2) { instr_660F2F(read_xmm64s(r1), r2); }
void instr_660F2F_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_660F2F(x, r);
}

/* ------------------------------------------------------------------ *
 * 0F 50-57 -- movmsk, sqrt, rcp, and/andn/or/xor (instructions_0f.rs:1576-1782)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:1576-1585 -- movmskps r, xmm */
void instr_0F50_reg(int32_t r1, int32_t r2)
{
	reg128 source = read_xmm128s(r1);
	int32_t data = (int32_t)((source.u32[0] >> 31) |
	                         ((source.u32[1] >> 31) << 1) |
	                         ((source.u32[2] >> 31) << 2) |
	                         ((source.u32[3] >> 31) << 3));
	write_reg32(r2, data);
}
void instr_0F50_mem(int32_t _addr, int32_t _r1)
{
	(void)_addr;
	(void)_r1;
	trigger_ud();
}
/* instructions_0f.rs:1586-1595 -- movmskpd r, xmm */
void instr_660F50_reg(int32_t r1, int32_t r2)
{
	reg128 source = read_xmm128s(r1);
	int32_t data = (int32_t)((source.u32[1] >> 31) | ((source.u32[3] >> 31) << 1));
	write_reg32(r2, data);
}
void instr_660F50_mem(int32_t _addr, int32_t _r1)
{
	(void)_addr;
	(void)_r1;
	trigger_ud();
}

/* instructions_0f.rs:1596-1647 -- sqrtps/sqrtpd/sqrtsd/sqrtss */
/* XXX: Should round according to round control */
static void instr_0F51(reg128 source, int32_t r)
{
	reg128 result;
	result.f32[0] = sqrtf(source.f32[0]);
	result.f32[1] = sqrtf(source.f32[1]);
	result.f32[2] = sqrtf(source.f32[2]);
	result.f32[3] = sqrtf(source.f32[3]);
	write_xmm_reg128(r, result);
}
void instr_0F51_reg(int32_t r1, int32_t r2) { instr_0F51(read_xmm128s(r1), r2); }
void instr_0F51_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F51(x, r);
}
static void instr_660F51(reg128 source, int32_t r)
{
	reg128 result;
	result.f64[0] = sqrt(source.f64[0]);
	result.f64[1] = sqrt(source.f64[1]);
	write_xmm_reg128(r, result);
}
void instr_660F51_reg(int32_t r1, int32_t r2) { instr_660F51(read_xmm128s(r1), r2); }
void instr_660F51_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F51(x, r);
}
static void instr_F20F51(uint64_t source, int32_t r)
{
	reg128 in;
	in.u64[0] = source;
	write_xmm_f64(r, sqrt(in.f64[0]));
}
void instr_F20F51_reg(int32_t r1, int32_t r2) { instr_F20F51(read_xmm64s(r1), r2); }
void instr_F20F51_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F51(x, r);
}
static void instr_F30F51(float source, int32_t r) { write_xmm_f32(r, sqrtf(source)); }
void instr_F30F51_reg(int32_t r1, int32_t r2) { instr_F30F51(read_xmm_f32(r1), r2); }
void instr_F30F51_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F51(x, r);
}

/* instructions_0f.rs:1648-1701 -- the rsqrt/rcp pair (upstream's first comment says
 * "rcpps" on the 1/sqrt body and "rsqrtss" on 1/x; both bodies are ported, not the
 * labels) */
static void instr_0F52(reg128 source, int32_t r)
{
	reg128 result;
	result.f32[0] = 1.0f / sqrtf(source.f32[0]);
	result.f32[1] = 1.0f / sqrtf(source.f32[1]);
	result.f32[2] = 1.0f / sqrtf(source.f32[2]);
	result.f32[3] = 1.0f / sqrtf(source.f32[3]);
	write_xmm_reg128(r, result);
}
void instr_0F52_reg(int32_t r1, int32_t r2) { instr_0F52(read_xmm128s(r1), r2); }
void instr_0F52_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F52(x, r);
}
static void instr_F30F52(float source, int32_t r) { write_xmm_f32(r, 1.0f / sqrtf(source)); }
void instr_F30F52_reg(int32_t r1, int32_t r2) { instr_F30F52(read_xmm_f32(r1), r2); }
void instr_F30F52_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F52(x, r);
}
static void instr_0F53(reg128 source, int32_t r)
{
	reg128 result;
	result.f32[0] = 1.0f / source.f32[0];
	result.f32[1] = 1.0f / source.f32[1];
	result.f32[2] = 1.0f / source.f32[2];
	result.f32[3] = 1.0f / source.f32[3];
	write_xmm_reg128(r, result);
}
void instr_0F53_reg(int32_t r1, int32_t r2) { instr_0F53(read_xmm128s(r1), r2); }
void instr_0F53_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F53(x, r);
}
static void instr_F30F53(float source, int32_t r) { write_xmm_f32(r, 1.0f / source); }
void instr_F30F53_reg(int32_t r1, int32_t r2) { instr_F30F53(read_xmm_f32(r1), r2); }
void instr_F30F53_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F53(x, r);
}

/* instructions_0f.rs:1702-1781 -- andps/andpd, andnps/andnpd, orps/orpd, xorps/xorpd.
 * These four ARE sse_instr.c's pand/pandn/por/pxor (which read the destination and
 * write it back); the base body is a one-line forward, exactly as upstream's is. */
static void instr_0F54(reg128 source, int32_t r) { pand_r128(source, r); }
void instr_0F54_reg(int32_t r1, int32_t r2) { instr_0F54(read_xmm128s(r1), r2); }
void instr_0F54_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F54(x, r);
}
static void instr_660F54(reg128 source, int32_t r) { pand_r128(source, r); }
void instr_660F54_reg(int32_t r1, int32_t r2) { instr_660F54(read_xmm128s(r1), r2); }
void instr_660F54_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F54(x, r);
}
static void instr_0F55(reg128 source, int32_t r) { pandn_r128(source, r); }
void instr_0F55_reg(int32_t r1, int32_t r2) { instr_0F55(read_xmm128s(r1), r2); }
void instr_0F55_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F55(x, r);
}
static void instr_660F55(reg128 source, int32_t r) { pandn_r128(source, r); }
void instr_660F55_reg(int32_t r1, int32_t r2) { instr_660F55(read_xmm128s(r1), r2); }
void instr_660F55_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F55(x, r);
}
static void instr_0F56(reg128 source, int32_t r) { por_r128(source, r); }
void instr_0F56_reg(int32_t r1, int32_t r2) { instr_0F56(read_xmm128s(r1), r2); }
void instr_0F56_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F56(x, r);
}
static void instr_660F56(reg128 source, int32_t r) { por_r128(source, r); }
void instr_660F56_reg(int32_t r1, int32_t r2) { instr_660F56(read_xmm128s(r1), r2); }
void instr_660F56_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F56(x, r);
}
static void instr_0F57(reg128 source, int32_t r) { pxor_r128(source, r); }
void instr_0F57_reg(int32_t r1, int32_t r2) { instr_0F57(read_xmm128s(r1), r2); }
void instr_0F57_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F57(x, r);
}
static void instr_660F57(reg128 source, int32_t r) { pxor_r128(source, r); }
void instr_660F57_reg(int32_t r1, int32_t r2) { instr_660F57(read_xmm128s(r1), r2); }
void instr_660F57_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F57(x, r);
}

/* ------------------------------------------------------------------ *
 * 0F 58-5F -- add/mul/cvt/sub/min/div/max (instructions_0f.rs:1783-2222)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:1783-1838 -- addps/addpd/addsd/addss */
static void instr_0F58(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f32[0] = source.f32[0] + destination.f32[0];
	result.f32[1] = source.f32[1] + destination.f32[1];
	result.f32[2] = source.f32[2] + destination.f32[2];
	result.f32[3] = source.f32[3] + destination.f32[3];
	write_xmm_reg128(r, result);
}
void instr_0F58_reg(int32_t r1, int32_t r2) { instr_0F58(read_xmm128s(r1), r2); }
void instr_0F58_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F58(x, r);
}
static void instr_660F58(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f64[0] = source.f64[0] + destination.f64[0];
	result.f64[1] = source.f64[1] + destination.f64[1];
	write_xmm_reg128(r, result);
}
void instr_660F58_reg(int32_t r1, int32_t r2) { instr_660F58(read_xmm128s(r1), r2); }
void instr_660F58_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F58(x, r);
}
static void instr_F20F58(uint64_t source, int32_t r)
{
	reg128 a, b;
	uint64_t destination = read_xmm64s(r);
	a.u64[0] = source;
	b.u64[0] = destination;
	write_xmm_f64(r, a.f64[0] + b.f64[0]);
}
void instr_F20F58_reg(int32_t r1, int32_t r2) { instr_F20F58(read_xmm64s(r1), r2); }
void instr_F20F58_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F58(x, r);
}
static void instr_F30F58(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	float result = source + destination;
	write_xmm_f32(r, result);
}
void instr_F30F58_reg(int32_t r1, int32_t r2) { instr_F30F58(read_xmm_f32(r1), r2); }
void instr_F30F58_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F58(x, r);
}

/* instructions_0f.rs:1839-1894 -- mulps/mulpd/mulsd/mulss */
static void instr_0F59(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f32[0] = source.f32[0] * destination.f32[0];
	result.f32[1] = source.f32[1] * destination.f32[1];
	result.f32[2] = source.f32[2] * destination.f32[2];
	result.f32[3] = source.f32[3] * destination.f32[3];
	write_xmm_reg128(r, result);
}
void instr_0F59_reg(int32_t r1, int32_t r2) { instr_0F59(read_xmm128s(r1), r2); }
void instr_0F59_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F59(x, r);
}
static void instr_660F59(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f64[0] = source.f64[0] * destination.f64[0];
	result.f64[1] = source.f64[1] * destination.f64[1];
	write_xmm_reg128(r, result);
}
void instr_660F59_reg(int32_t r1, int32_t r2) { instr_660F59(read_xmm128s(r1), r2); }
void instr_660F59_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F59(x, r);
}
static void instr_F20F59(uint64_t source, int32_t r)
{
	reg128 a, b;
	uint64_t destination = read_xmm64s(r);
	a.u64[0] = source;
	b.u64[0] = destination;
	write_xmm_f64(r, a.f64[0] * b.f64[0]);
}
void instr_F20F59_reg(int32_t r1, int32_t r2) { instr_F20F59(read_xmm64s(r1), r2); }
void instr_F20F59_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F59(x, r);
}
static void instr_F30F59(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	float result = source * destination;
	write_xmm_f32(r, result);
}
void instr_F30F59_reg(int32_t r1, int32_t r2) { instr_F30F59(read_xmm_f32(r1), r2); }
void instr_F30F59_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F59(x, r);
}

/* instructions_0f.rs:1895-1940 -- the four conversion pairs */
static void instr_0F5A(uint64_t source, int32_t r)
{
	reg128 in, result;
	in.u64[0] = source;
	result.f64[0] = (double)in.f32[0];
	result.f64[1] = (double)in.f32[1];
	write_xmm_reg128(r, result);
}
void instr_0F5A_reg(int32_t r1, int32_t r2) { instr_0F5A(read_xmm64s(r1), r2); }
void instr_0F5A_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F5A(x, r);
}
/* XXX: These conversions are lossy and should round according to the round control */
static void instr_660F5A(reg128 source, int32_t r)
{
	reg128 result;
	result.f32[0] = (float)source.f64[0];
	result.f32[1] = (float)source.f64[1];
	result.f32[2] = 0.0f;
	result.f32[3] = 0.0f;
	write_xmm_reg128(r, result);
}
void instr_660F5A_reg(int32_t r1, int32_t r2) { instr_660F5A(read_xmm128s(r1), r2); }
void instr_660F5A_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F5A(x, r);
}
/* XXX: This conversions is lossy and should round according to the round control */
static void instr_F20F5A(uint64_t source, int32_t r)
{
	reg128 in;
	in.u64[0] = source;
	write_xmm_f32(r, (float)in.f64[0]);
}
void instr_F20F5A_reg(int32_t r1, int32_t r2) { instr_F20F5A(read_xmm64s(r1), r2); }
void instr_F20F5A_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F5A(x, r);
}
static void instr_F30F5A(float source, int32_t r) { write_xmm_f64(r, (double)source); }
void instr_F30F5A_reg(int32_t r1, int32_t r2) { instr_F30F5A(read_xmm_f32(r1), r2); }
void instr_F30F5A_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F5A(x, r);
}

/* instructions_0f.rs:1941-1995 -- cvtdq2ps / cvtps2dq / cvttps2dq */
/* XXX: Should round according to round control */
static void instr_0F5B(reg128 source, int32_t r)
{
	reg128 result;
	/* XXX: Precision exception */
	result.f32[0] = (float)source.i32[0];
	result.f32[1] = (float)source.i32[1];
	result.f32[2] = (float)source.i32[2];
	result.f32[3] = (float)source.i32[3];
	write_xmm_reg128(r, result);
}
void instr_0F5B_reg(int32_t r1, int32_t r2) { instr_0F5B(read_xmm128s(r1), r2); }
void instr_0F5B_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F5B(x, r);
}
static void instr_660F5B(reg128 source, int32_t r)
{
	reg128 result;
	/* XXX: Precision exception */
	result.i32[0] = sse_convert_f32_to_i32(source.f32[0]);
	result.i32[1] = sse_convert_f32_to_i32(source.f32[1]);
	result.i32[2] = sse_convert_f32_to_i32(source.f32[2]);
	result.i32[3] = sse_convert_f32_to_i32(source.f32[3]);
	write_xmm_reg128(r, result);
}
void instr_660F5B_reg(int32_t r1, int32_t r2) { instr_660F5B(read_xmm128s(r1), r2); }
void instr_660F5B_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F5B(x, r);
}
static void instr_F30F5B(reg128 source, int32_t r)
{
	reg128 result;
	result.i32[0] = sse_convert_with_truncation_f32_to_i32(source.f32[0]);
	result.i32[1] = sse_convert_with_truncation_f32_to_i32(source.f32[1]);
	result.i32[2] = sse_convert_with_truncation_f32_to_i32(source.f32[2]);
	result.i32[3] = sse_convert_with_truncation_f32_to_i32(source.f32[3]);
	write_xmm_reg128(r, result);
}
void instr_F30F5B_reg(int32_t r1, int32_t r2) { instr_F30F5B(read_xmm128s(r1), r2); }
void instr_F30F5B_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F30F5B(x, r);
}

/* instructions_0f.rs:1996-2051 -- subps/subpd/subsd/subss.
 * NOTE the operand ORDER: destination - source (the reverse of add/mul). */
static void instr_0F5C(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f32[0] = destination.f32[0] - source.f32[0];
	result.f32[1] = destination.f32[1] - source.f32[1];
	result.f32[2] = destination.f32[2] - source.f32[2];
	result.f32[3] = destination.f32[3] - source.f32[3];
	write_xmm_reg128(r, result);
}
void instr_0F5C_reg(int32_t r1, int32_t r2) { instr_0F5C(read_xmm128s(r1), r2); }
void instr_0F5C_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F5C(x, r);
}
static void instr_660F5C(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f64[0] = destination.f64[0] - source.f64[0];
	result.f64[1] = destination.f64[1] - source.f64[1];
	write_xmm_reg128(r, result);
}
void instr_660F5C_reg(int32_t r1, int32_t r2) { instr_660F5C(read_xmm128s(r1), r2); }
void instr_660F5C_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F5C(x, r);
}
static void instr_F20F5C(uint64_t source, int32_t r)
{
	reg128 a, b;
	uint64_t destination = read_xmm64s(r);
	a.u64[0] = destination;
	b.u64[0] = source;
	write_xmm_f64(r, a.f64[0] - b.f64[0]);
}
void instr_F20F5C_reg(int32_t r1, int32_t r2) { instr_F20F5C(read_xmm64s(r1), r2); }
void instr_F20F5C_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F5C(x, r);
}
static void instr_F30F5C(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	float result = destination - source;
	write_xmm_f32(r, result);
}
void instr_F30F5C_reg(int32_t r1, int32_t r2) { instr_F30F5C(read_xmm_f32(r1), r2); }
void instr_F30F5C_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F5C(x, r);
}

/* instructions_0f.rs:2052-2109 -- minps/minpd/minsd/minss.
 * sse_min takes f64 arguments and the f32 forms go through f64 and back, as upstream's
 * `as f64 ... as f32` does -- the double rounding is upstream's, kept. */
static void instr_0F5D(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f32[0] = (float)sse_min((double)destination.f32[0], (double)source.f32[0]);
	result.f32[1] = (float)sse_min((double)destination.f32[1], (double)source.f32[1]);
	result.f32[2] = (float)sse_min((double)destination.f32[2], (double)source.f32[2]);
	result.f32[3] = (float)sse_min((double)destination.f32[3], (double)source.f32[3]);
	write_xmm_reg128(r, result);
}
void instr_0F5D_reg(int32_t r1, int32_t r2) { instr_0F5D(read_xmm128s(r1), r2); }
void instr_0F5D_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F5D(x, r);
}
static void instr_660F5D(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f64[0] = sse_min(destination.f64[0], source.f64[0]);
	result.f64[1] = sse_min(destination.f64[1], source.f64[1]);
	write_xmm_reg128(r, result);
}
void instr_660F5D_reg(int32_t r1, int32_t r2) { instr_660F5D(read_xmm128s(r1), r2); }
void instr_660F5D_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F5D(x, r);
}
static void instr_F20F5D(uint64_t source, int32_t r)
{
	reg128 a, b;
	uint64_t destination = read_xmm64s(r);
	a.u64[0] = destination;
	b.u64[0] = source;
	write_xmm_f64(r, sse_min(a.f64[0], b.f64[0]));
}
void instr_F20F5D_reg(int32_t r1, int32_t r2) { instr_F20F5D(read_xmm64s(r1), r2); }
void instr_F20F5D_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F5D(x, r);
}
static void instr_F30F5D(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	float result = (float)sse_min((double)destination, (double)source);
	write_xmm_f32(r, result);
}
void instr_F30F5D_reg(int32_t r1, int32_t r2) { instr_F30F5D(read_xmm_f32(r1), r2); }
void instr_F30F5D_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F5D(x, r);
}

/* instructions_0f.rs:2110-2164 -- divps/divpd/divsd/divss (destination / source) */
static void instr_0F5E(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f32[0] = destination.f32[0] / source.f32[0];
	result.f32[1] = destination.f32[1] / source.f32[1];
	result.f32[2] = destination.f32[2] / source.f32[2];
	result.f32[3] = destination.f32[3] / source.f32[3];
	write_xmm_reg128(r, result);
}
void instr_0F5E_reg(int32_t r1, int32_t r2) { instr_0F5E(read_xmm128s(r1), r2); }
void instr_0F5E_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F5E(x, r);
}
static void instr_660F5E(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f64[0] = destination.f64[0] / source.f64[0];
	result.f64[1] = destination.f64[1] / source.f64[1];
	write_xmm_reg128(r, result);
}
void instr_660F5E_reg(int32_t r1, int32_t r2) { instr_660F5E(read_xmm128s(r1), r2); }
void instr_660F5E_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F5E(x, r);
}
static void instr_F20F5E(uint64_t source, int32_t r)
{
	reg128 a, b;
	uint64_t destination = read_xmm64s(r);
	a.u64[0] = destination;
	b.u64[0] = source;
	write_xmm_f64(r, a.f64[0] / b.f64[0]);
}
void instr_F20F5E_reg(int32_t r1, int32_t r2) { instr_F20F5E(read_xmm64s(r1), r2); }
void instr_F20F5E_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F5E(x, r);
}
static void instr_F30F5E(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	float result = destination / source;
	write_xmm_f32(r, result);
}
void instr_F30F5E_reg(int32_t r1, int32_t r2) { instr_F30F5E(read_xmm_f32(r1), r2); }
void instr_F30F5E_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F5E(x, r);
}

/* instructions_0f.rs:2165-2222 -- maxps/maxpd/maxsd/maxss */
static void instr_0F5F(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f32[0] = (float)sse_max((double)destination.f32[0], (double)source.f32[0]);
	result.f32[1] = (float)sse_max((double)destination.f32[1], (double)source.f32[1]);
	result.f32[2] = (float)sse_max((double)destination.f32[2], (double)source.f32[2]);
	result.f32[3] = (float)sse_max((double)destination.f32[3], (double)source.f32[3]);
	write_xmm_reg128(r, result);
}
void instr_0F5F_reg(int32_t r1, int32_t r2) { instr_0F5F(read_xmm128s(r1), r2); }
void instr_0F5F_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0F5F(x, r);
}
static void instr_660F5F(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.f64[0] = sse_max(destination.f64[0], source.f64[0]);
	result.f64[1] = sse_max(destination.f64[1], source.f64[1]);
	write_xmm_reg128(r, result);
}
void instr_660F5F_reg(int32_t r1, int32_t r2) { instr_660F5F(read_xmm128s(r1), r2); }
void instr_660F5F_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F5F(x, r);
}
static void instr_F20F5F(uint64_t source, int32_t r)
{
	reg128 a, b;
	uint64_t destination = read_xmm64s(r);
	a.u64[0] = destination;
	b.u64[0] = source;
	write_xmm_f64(r, sse_max(a.f64[0], b.f64[0]));
}
void instr_F20F5F_reg(int32_t r1, int32_t r2) { instr_F20F5F(read_xmm64s(r1), r2); }
void instr_F20F5F_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20F5F(x, r);
}
static void instr_F30F5F(float source, int32_t r)
{
	float destination = read_xmm_f32(r);
	float result = (float)sse_max((double)destination, (double)source);
	write_xmm_f32(r, result);
}
void instr_F30F5F_reg(int32_t r1, int32_t r2) { instr_F30F5F(read_xmm_f32(r1), r2); }
void instr_F30F5F_mem(int32_t addr, int32_t r)
{
	float x = safe_read_f32(addr);
	if (cpu->pagefault) return;
	instr_F30F5F(x, r);
}

/* ------------------------------------------------------------------ *
 * 0F 60-6B -- punpck/pack/pcmpgt (instructions_0f.rs:2223-2602)
 *
 * Every MMX form here reads the destination, writes the result and then runs
 * transition_fpu_to_mmx(); the XMM forms do not.  `u64::to_le_bytes`/`from_le_bytes` and
 * `std::mem::transmute(reg128)` are little-endian lane views, spelled through the reg128
 * union (cpu_state.h:196-207), whose members are exactly those views.
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:2223-2241 -- punpcklbw mm, mm/m32 */
static void instr_0F60(int32_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.i32[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u8[2 * i + 0] = destination.u8[i];
		result.u8[2 * i + 1] = src.u8[i];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F60_reg(int32_t r1, int32_t r2) { instr_0F60(read_mmx32s(r1), r2); }
void instr_0F60_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_0F60(x, r);
}
static void instr_660F60(reg128 source, int32_t r)
{
	reg128 destination, result;
	destination.u64[0] = read_xmm64s(r);
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 8; i++) {
		result.u8[2 * i + 0] = destination.u8[i];
		result.u8[2 * i + 1] = source.u8[i];
	}
	write_xmm_reg128(r, result);
}
void instr_660F60_reg(int32_t r1, int32_t r2) { instr_660F60(read_xmm128s(r1), r2); }
void instr_660F60_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F60(x, r);
}

/* instructions_0f.rs:2258-2289 -- punpcklwd mm, mm/m32 */
static void instr_0F61(int32_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.i32[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 2; i++) {
		result.u16[2 * i + 0] = destination.u16[i];
		result.u16[2 * i + 1] = src.u16[i];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F61_reg(int32_t r1, int32_t r2) { instr_0F61(read_mmx32s(r1), r2); }
void instr_0F61_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_0F61(x, r);
}
static void instr_660F61(reg128 source, int32_t r)
{
	reg128 destination, result;
	destination.u64[0] = read_xmm64s(r);
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u16[2 * i + 0] = destination.u16[i];
		result.u16[2 * i + 1] = source.u16[i];
	}
	write_xmm_reg128(r, result);
}
void instr_660F61_reg(int32_t r1, int32_t r2) { instr_660F61(read_xmm128s(r1), r2); }
void instr_660F61_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F61(x, r);
}

/* instructions_0f.rs:2291-2319 -- punpckldq mm, mm/m32 */
static void instr_0F62(int32_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, (destination & 0xFFFFFFFFull) | ((uint64_t)(uint32_t)source << 32));
	transition_fpu_to_mmx();
}
void instr_0F62_reg(int32_t r1, int32_t r2) { instr_0F62(read_mmx32s(r1), r2); }
void instr_0F62_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_0F62(x, r);
}
static void instr_660F62(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)destination.u32[0],
	             (int32_t)source.u32[0],
	             (int32_t)destination.u32[1],
	             (int32_t)source.u32[1]);
}
void instr_660F62_reg(int32_t r1, int32_t r2) { instr_660F62(read_xmm128s(r1), r2); }
void instr_660F62_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F62(x, r);
}

/* instructions_0f.rs:2321-2352 -- packsswb */
static void instr_0F63(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u8[i + 0] = saturate_sw_to_sb((int32_t)destination.u16[i]);
		result.u8[i + 4] = saturate_sw_to_sb((int32_t)src.u16[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F63_reg(int32_t r1, int32_t r2) { instr_0F63(read_mmx64s(r1), r2); }
void instr_0F63_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F63(x, r);
}
static void instr_660F63(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i + 0] = saturate_sw_to_sb((int32_t)destination.u16[i]);
		result.u8[i + 8] = saturate_sw_to_sb((int32_t)source.u16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660F63_reg(int32_t r1, int32_t r2) { instr_660F63(read_xmm128s(r1), r2); }
void instr_660F63_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F63(x, r);
}

/* instructions_0f.rs:2354-2383 -- pcmpgtb */
static void instr_0F64(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i] = (uint8_t)(((int32_t)destination.i8[i] > (int32_t)src.i8[i]) ? 255 : 0);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F64_reg(int32_t r1, int32_t r2) { instr_0F64(read_mmx64s(r1), r2); }
void instr_0F64_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F64(x, r);
}
static void instr_660F64(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = (uint8_t)(((int32_t)destination.i8[i] > (int32_t)source.i8[i]) ? 255 : 0);
	}
	write_xmm_reg128(r, result);
}
void instr_660F64_reg(int32_t r1, int32_t r2) { instr_660F64(read_xmm128s(r1), r2); }
void instr_660F64_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F64(x, r);
}

/* instructions_0f.rs:2385-2414 -- pcmpgtw */
static void instr_0F65(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i] = (uint16_t)((destination.i16[i] > src.i16[i]) ? 0xFFFF : 0);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F65_reg(int32_t r1, int32_t r2) { instr_0F65(read_mmx64s(r1), r2); }
void instr_0F65_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F65(x, r);
}
static void instr_660F65(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] = (uint16_t)((destination.i16[i] > source.i16[i]) ? 0xFFFF : 0);
	}
	write_xmm_reg128(r, result);
}
void instr_660F65_reg(int32_t r1, int32_t r2) { instr_660F65(read_xmm128s(r1), r2); }
void instr_660F65_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F65(x, r);
}

/* instructions_0f.rs:2416-2447 -- pcmpgtd */
static void instr_0F66(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 2; i++) {
		result.i32[i] = (destination.i32[i] > src.i32[i]) ? -1 : 0;
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F66_reg(int32_t r1, int32_t r2) { instr_0F66(read_mmx64s(r1), r2); }
void instr_0F66_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F66(x, r);
}
static void instr_660F66(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (destination.i32[0] > source.i32[0]) ? -1 : 0,
	             (destination.i32[1] > source.i32[1]) ? -1 : 0,
	             (destination.i32[2] > source.i32[2]) ? -1 : 0,
	             (destination.i32[3] > source.i32[3]) ? -1 : 0);
}
void instr_660F66_reg(int32_t r1, int32_t r2) { instr_660F66(read_xmm128s(r1), r2); }
void instr_660F66_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F66(x, r);
}

/* instructions_0f.rs:2449-2480 -- packuswb */
static void instr_0F67(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u8[i + 0] = saturate_sw_to_ub(destination.u16[i]);
		result.u8[i + 4] = saturate_sw_to_ub(src.u16[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F67_reg(int32_t r1, int32_t r2) { instr_0F67(read_mmx64s(r1), r2); }
void instr_0F67_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F67(x, r);
}
static void instr_660F67(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i + 0] = saturate_sw_to_ub(destination.u16[i]);
		result.u8[i + 8] = saturate_sw_to_ub(source.u16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660F67_reg(int32_t r1, int32_t r2) { instr_660F67(read_xmm128s(r1), r2); }
void instr_660F67_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F67(x, r);
}

/* instructions_0f.rs:2482-2513 -- punpckhbw */
static void instr_0F68(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u8[2 * i + 0] = destination.u8[i + 4];
		result.u8[2 * i + 1] = src.u8[i + 4];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F68_reg(int32_t r1, int32_t r2) { instr_0F68(read_mmx64s(r1), r2); }
void instr_0F68_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F68(x, r);
}
static void instr_660F68(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 8; i++) {
		result.u8[2 * i + 0] = destination.u8[i + 8];
		result.u8[2 * i + 1] = source.u8[i + 8];
	}
	write_xmm_reg128(r, result);
}
void instr_660F68_reg(int32_t r1, int32_t r2) { instr_660F68(read_xmm128s(r1), r2); }
void instr_660F68_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F68(x, r);
}

/* instructions_0f.rs:2515-2542 -- punpckhwd */
static void instr_0F69(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	result.u16[0] = destination.u16[2];
	result.u16[1] = src.u16[2];
	result.u16[2] = destination.u16[3];
	result.u16[3] = src.u16[3];
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F69_reg(int32_t r1, int32_t r2) { instr_0F69(read_mmx64s(r1), r2); }
void instr_0F69_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F69(x, r);
}
static void instr_660F69(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u16[2 * i + 0] = destination.u16[i + 4];
		result.u16[2 * i + 1] = source.u16[i + 4];
	}
	write_xmm_reg128(r, result);
}
void instr_660F69_reg(int32_t r1, int32_t r2) { instr_660F69(read_xmm128s(r1), r2); }
void instr_660F69_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F69(x, r);
}

/* instructions_0f.rs:2544-2570 -- punpckhdq */
static void instr_0F6A(uint64_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, (destination >> 32) | ((source >> 32) << 32));
	transition_fpu_to_mmx();
}
void instr_0F6A_reg(int32_t r1, int32_t r2) { instr_0F6A(read_mmx64s(r1), r2); }
void instr_0F6A_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F6A(x, r);
}
static void instr_660F6A(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)destination.u32[2],
	             (int32_t)source.u32[2],
	             (int32_t)destination.u32[3],
	             (int32_t)source.u32[3]);
}
void instr_660F6A_reg(int32_t r1, int32_t r2) { instr_660F6A(read_xmm128s(r1), r2); }
void instr_660F6A_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F6A(x, r);
}

/* instructions_0f.rs:2572-2603 -- packssdw */
static void instr_0F6B(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 2; i++) {
		result.u16[i + 0] = saturate_sd_to_sw(destination.u32[i]);
		result.u16[i + 2] = saturate_sd_to_sw(src.u32[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F6B_reg(int32_t r1, int32_t r2) { instr_0F6B(read_mmx64s(r1), r2); }
void instr_0F6B_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F6B(x, r);
}
static void instr_660F6B(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	memset(&result, 0, sizeof(result));
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i + 0] = saturate_sd_to_sw(destination.u32[i]);
		result.u16[i + 4] = saturate_sd_to_sw(source.u32[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660F6B_reg(int32_t r1, int32_t r2) { instr_660F6B(read_xmm128s(r1), r2); }
void instr_660F6B_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F6B(x, r);
}

/* ------------------------------------------------------------------ *
 * 0F 6C/6D -- punpcklqdq / punpckhqdq (instructions_0f.rs:2604-2644)
 * ------------------------------------------------------------------ */

void instr_0F6C_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0F6C_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
static void instr_660F6C(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)destination.u32[0],
	             (int32_t)destination.u32[1],
	             (int32_t)source.u32[0],
	             (int32_t)source.u32[1]);
}
void instr_660F6C_reg(int32_t r1, int32_t r2) { instr_660F6C(read_xmm128s(r1), r2); }
void instr_660F6C_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F6C(x, r);
}
void instr_0F6D_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0F6D_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
static void instr_660F6D(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)destination.u32[2],
	             (int32_t)destination.u32[3],
	             (int32_t)source.u32[2],
	             (int32_t)source.u32[3]);
}
void instr_660F6D_reg(int32_t r1, int32_t r2) { instr_660F6D(read_xmm128s(r1), r2); }
void instr_660F6D_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F6D(x, r);
}

/* ------------------------------------------------------------------ *
 * 0F 6E/6F -- movd / movq / movdqa / movdqu (instructions_0f.rs:2646-2700)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:2646-2664 -- movd mm, r/m32 */
static void instr_0F6E(int32_t source, int32_t r)
{
	write_mmx_reg64(r, (uint64_t)(uint32_t)source);
	transition_fpu_to_mmx();
}
void instr_0F6E_reg(int32_t r1, int32_t r2) { instr_0F6E(read_reg32(r1), r2); }
void instr_0F6E_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_0F6E(x, r);
}
static void instr_660F6E(int32_t source, int32_t r) { write_xmm128(r, source, 0, 0, 0); }
void instr_660F6E_reg(int32_t r1, int32_t r2) { instr_660F6E(read_reg32(r1), r2); }
void instr_660F6E_mem(int32_t addr, int32_t r)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_660F6E(x, r);
}

/* instructions_0f.rs:2665-2685 -- movq mm, mm/m64 */
static void instr_0F6F(uint64_t source, int32_t r)
{
	write_mmx_reg64(r, source);
	transition_fpu_to_mmx();
}
void instr_0F6F_reg(int32_t r1, int32_t r2) { instr_0F6F(read_mmx64s(r1), r2); }
void instr_0F6F_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F6F(x, r);
}
/* instructions_0f.rs:2676-2684 -- movdqa xmm, xmm/mem128 (XXX: aligned or #gp) */
static void instr_660F6F(reg128 source, int32_t r) { mov_rm_r128(source, r); }
void instr_660F6F_reg(int32_t r1, int32_t r2) { instr_660F6F(read_xmm128s(r1), r2); }
void instr_660F6F_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F6F(x, r);
}
/* instructions_0f.rs:2685-2692 -- movdqu xmm, xmm/m128 */
static void instr_F30F6F(reg128 source, int32_t r) { mov_rm_r128(source, r); }
void instr_F30F6F_reg(int32_t r1, int32_t r2) { instr_F30F6F(read_xmm128s(r1), r2); }
void instr_F30F6F_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F30F6F(x, r);
}

/* ------------------------------------------------------------------ *
 * 0F 70 -- pshufw / pshufd / pshuflw / pshufhw (instructions_0f.rs:2700-2764)
 *
 * `imm8 >> (2 * i) & 3` is `(imm8 >> (2 * i)) & 3` (Rust's `>>` binds tighter than `&`);
 * the lane selects are written with the parentheses explicit.
 * ------------------------------------------------------------------ */

/* pshufw mm1, mm2/m64, imm8 */
static void instr_0F70(uint64_t source, int32_t r, int32_t imm8)
{
	reg128 src, result;
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i] = src.u16[(imm8 >> (2 * i)) & 3];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F70_reg(int32_t r1, int32_t r2, int32_t imm) { instr_0F70(read_mmx64s(r1), r2, imm); }
void instr_0F70_mem(int32_t addr, int32_t r, int32_t imm)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F70(x, r, imm);
}
/* pshufd xmm, xmm/mem128, imm8 */
static void instr_660F70(reg128 source, int32_t r, int32_t imm8)
{
	write_xmm128(r,
	             (int32_t)source.u32[(imm8 & 3)],
	             (int32_t)source.u32[(imm8 >> 2) & 3],
	             (int32_t)source.u32[(imm8 >> 4) & 3],
	             (int32_t)source.u32[(imm8 >> 6) & 3]);
}
void instr_660F70_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_660F70(read_xmm128s(r1), r2, imm);
}
void instr_660F70_mem(int32_t addr, int32_t r, int32_t imm)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F70(x, r, imm);
}
/* pshuflw xmm, xmm/m128, imm8 */
static void instr_F20F70(reg128 source, int32_t r, int32_t imm8)
{
	uint32_t dword0 = (uint32_t)source.u16[imm8 & 3] |
	                  ((uint32_t)source.u16[(imm8 >> 2) & 3] << 16);
	uint32_t dword1 = (uint32_t)source.u16[(imm8 >> 4) & 3] |
	                  ((uint32_t)source.u16[(imm8 >> 6) & 3] << 16);
	write_xmm128(r,
	             (int32_t)dword0,
	             (int32_t)dword1,
	             (int32_t)source.u32[2],
	             (int32_t)source.u32[3]);
}
void instr_F20F70_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_F20F70(read_xmm128s(r1), r2, imm);
}
void instr_F20F70_mem(int32_t addr, int32_t r, int32_t imm)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F20F70(x, r, imm);
}
/* pshufhw xmm, xmm/m128, imm8 */
static void instr_F30F70(reg128 source, int32_t r, int32_t imm8)
{
	uint32_t dword2 = (uint32_t)source.u16[(imm8 & 3) | 4] |
	                  ((uint32_t)source.u16[((imm8 >> 2) & 3) | 4] << 16);
	uint32_t dword3 = (uint32_t)source.u16[((imm8 >> 4) & 3) | 4] |
	                  ((uint32_t)source.u16[((imm8 >> 6) & 3) | 4] << 16);
	write_xmm128(r,
	             (int32_t)source.u32[0],
	             (int32_t)source.u32[1],
	             (int32_t)dword2,
	             (int32_t)dword3);
}
void instr_F30F70_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_F30F70(read_xmm128s(r1), r2, imm);
}
void instr_F30F70_mem(int32_t addr, int32_t r, int32_t imm)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F30F70(x, r, imm);
}

/* ------------------------------------------------------------------ *
 * 0F 71/72/73 -- the immediate shifts (instructions_0f.rs:2765-2901)
 *
 * Every one of these forwards into sse_instr.c, whose bodies clamp the count themselves.
 * The `_mem` forms are #UD (the encoding has no memory operand) -- they are still
 * emitted by the dispatcher and must exist by name.
 * ------------------------------------------------------------------ */

/* psrlw/psraw/psllw mm, imm8 */
void instr_0F71_2_reg(int32_t r, int32_t imm8) { psrlw_r64(r, (uint64_t)imm8); }
void instr_0F71_4_reg(int32_t r, int32_t imm8) { psraw_r64(r, (uint64_t)imm8); }
void instr_0F71_6_reg(int32_t r, int32_t imm8) { psllw_r64(r, (uint64_t)imm8); }
void instr_0F71_2_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0F71_4_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0F71_6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}

/* psrlw/psraw/psllw xmm, imm8 */
void instr_660F71_2_reg(int32_t r, int32_t imm8) { psrlw_r128(r, (uint64_t)imm8); }
void instr_660F71_4_reg(int32_t r, int32_t imm8) { psraw_r128(r, (uint64_t)imm8); }
void instr_660F71_6_reg(int32_t r, int32_t imm8) { psllw_r128(r, (uint64_t)imm8); }
void instr_660F71_2_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_660F71_4_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_660F71_6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}

/* psrld/psrad/pslld mm, imm8 */
void instr_0F72_2_reg(int32_t r, int32_t imm8) { psrld_r64(r, (uint64_t)imm8); }
void instr_0F72_4_reg(int32_t r, int32_t imm8) { psrad_r64(r, (uint64_t)imm8); }
void instr_0F72_6_reg(int32_t r, int32_t imm8) { pslld_r64(r, (uint64_t)imm8); }
void instr_0F72_2_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0F72_4_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0F72_6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}

/* psrld/psrad/pslld xmm, imm8 */
void instr_660F72_2_reg(int32_t r, int32_t imm8) { psrld_r128(r, (uint64_t)imm8); }
void instr_660F72_4_reg(int32_t r, int32_t imm8) { psrad_r128(r, (uint64_t)imm8); }
void instr_660F72_6_reg(int32_t r, int32_t imm8) { pslld_r128(r, (uint64_t)imm8); }
void instr_660F72_2_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_660F72_4_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_660F72_6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}

/* psrlq/psllq mm, imm8 */
void instr_0F73_2_reg(int32_t r, int32_t imm8) { psrlq_r64(r, (uint64_t)imm8); }
void instr_0F73_6_reg(int32_t r, int32_t imm8) { psllq_r64(r, (uint64_t)imm8); }
void instr_0F73_2_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0F73_6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}

/* psrlq / psrldq / psllq / pslldq xmm, imm8 */
void instr_660F73_2_reg(int32_t r, int32_t imm8) { psrlq_r128(r, (uint64_t)imm8); }
void instr_660F73_6_reg(int32_t r, int32_t imm8) { psllq_r128(r, (uint64_t)imm8); }
void instr_660F73_3_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_660F73_6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_660F73_2_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_660F73_7_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}

/* psrldq xmm, imm8.  `shift == 0` is only reachable through `imm8 == 0`, which returns
 * before the shift by `64 - shift` would be defined -- the early return is load-bearing,
 * not an optimisation. */
void instr_660F73_3_reg(int32_t r, int32_t imm8)
{
	reg128 destination, result;
	uint32_t shift;
	memset(&result, 0, sizeof(result));
	if (imm8 == 0) {
		return;
	}
	destination = read_xmm128s(r);
	shift = (uint32_t)(imm8 > 15 ? 128 : imm8 << 3);
	if (shift <= 63) {
		result.u64[0] = destination.u64[0] >> shift | destination.u64[1] << (64 - shift);
		result.u64[1] = destination.u64[1] >> shift;
	}
	else if (shift <= 127) {
		result.u64[0] = destination.u64[1] >> (shift - 64);
		result.u64[1] = 0;
	}
	write_xmm_reg128(r, result);
}

/* pslldq xmm, imm8 -- the mirror of psrldq */
void instr_660F73_7_reg(int32_t r, int32_t imm8)
{
	reg128 destination, result;
	uint32_t shift;
	memset(&result, 0, sizeof(result));
	if (imm8 == 0) {
		return;
	}
	destination = read_xmm128s(r);
	shift = (uint32_t)(imm8 > 15 ? 128 : imm8 << 3);
	if (shift <= 63) {
		result.u64[0] = destination.u64[0] << shift;
		result.u64[1] = destination.u64[1] << shift | destination.u64[0] >> (64 - shift);
	}
	else if (shift <= 127) {
		result.u64[0] = 0;
		result.u64[1] = destination.u64[0] << (shift - 64);
	}
	write_xmm_reg128(r, result);
}

/* ------------------------------------------------------------------ *
 * 0F 74-77 -- pcmpeqb/pcmpeqw/pcmpeqd and emms (instructions_0f.rs:2902-3000)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:2904-2933 -- pcmpeqb */
static void instr_0F74(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i] = (uint8_t)((destination.u8[i] == src.u8[i]) ? 255 : 0);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F74_reg(int32_t r1, int32_t r2) { instr_0F74(read_mmx64s(r1), r2); }
void instr_0F74_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F74(x, r);
}
static void instr_660F74(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = (uint8_t)((source.u8[i] == destination.u8[i]) ? 255 : 0);
	}
	write_xmm_reg128(r, result);
}
void instr_660F74_reg(int32_t r1, int32_t r2) { instr_660F74(read_xmm128s(r1), r2); }
void instr_660F74_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F74(x, r);
}

/* instructions_0f.rs:2935-2965 -- pcmpeqw */
static void instr_0F75(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i] = (uint16_t)((destination.i16[i] == src.i16[i]) ? 0xFFFF : 0);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F75_reg(int32_t r1, int32_t r2) { instr_0F75(read_mmx64s(r1), r2); }
void instr_0F75_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F75(x, r);
}
static void instr_660F75(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] =
		    (uint16_t)(((int32_t)source.u16[i] == (int32_t)destination.u16[i]) ? 0xFFFF : 0);
	}
	write_xmm_reg128(r, result);
}
void instr_660F75_reg(int32_t r1, int32_t r2) { instr_660F75(read_xmm128s(r1), r2); }
void instr_660F75_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F75(x, r);
}

/* instructions_0f.rs:2967-2996 -- pcmpeqd */
static void instr_0F76(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 2; i++) {
		result.i32[i] = (destination.i32[i] == src.i32[i]) ? -1 : 0;
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0F76_reg(int32_t r1, int32_t r2) { instr_0F76(read_mmx64s(r1), r2); }
void instr_0F76_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0F76(x, r);
}
static void instr_660F76(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 4; i++) {
		result.i32[i] = (source.u32[i] == destination.u32[i]) ? -1 : 0;
	}
	write_xmm_reg128(r, result);
}
void instr_660F76_reg(int32_t r1, int32_t r2) { instr_660F76(read_xmm128s(r1), r2); }
void instr_660F76_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660F76(x, r);
}

/* instructions_0f.rs:2997-3001 -- emms.
 *
 * emms sets the tag word to 0xFFFF -- i.e. all eight x87 slots EMPTY -- which is the
 * OPPOSITE end from the MMX transition above (fpu_set_tag_word(0)).  Both are upstream's,
 * and they meet at the same place: the register file is shared, the tag word records
 * which reading of it is in force. */
void instr_0F77(void) { fpu_set_tag_word(0xFFFF); }

/* ------------------------------------------------------------------ *
 * 0F 7E/7F -- movd/movq and movdqa/movdqu stores (instructions_0f.rs:3094-3160)
 *
 * (0F 7C/7D -- haddpd/haddps/hsubpd/hsubps -- are M1b-1's, at the bottom of this file.)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:3098-3120 -- movd r/m32, mm */
static int32_t instr_0F7E(int32_t r) { return (int32_t)read_mmx64s(r); }
void instr_0F7E_reg(int32_t r1, int32_t r2)
{
	write_reg32(r1, instr_0F7E(r2));
	transition_fpu_to_mmx();
}
void instr_0F7E_mem(int32_t addr, int32_t r)
{
	safe_write32(addr, instr_0F7E(r));
	if (cpu->pagefault) return;
	transition_fpu_to_mmx();
}
/* instructions_0f.rs:3114-3117 -- movd r/m32, xmm */
static int32_t instr_660F7E(int32_t r)
{
	uint64_t data = read_xmm64s(r);
	return (int32_t)data;
}
void instr_660F7E_reg(int32_t r1, int32_t r2) { write_reg32(r1, instr_660F7E(r2)); }
void instr_660F7E_mem(int32_t addr, int32_t r)
{
	safe_write32(addr, instr_660F7E(r));
	if (cpu->pagefault) return;
}
/* instructions_0f.rs:3118-3126 -- movq xmm, xmm/mem64 */
void instr_F30F7E_mem(int32_t addr, int32_t r)
{
	uint64_t data = safe_read64s(addr);
	if (cpu->pagefault) return;
	write_xmm128_2(r, data, 0);
}
void instr_F30F7E_reg(int32_t r1, int32_t r2) { write_xmm128_2(r2, read_xmm64s(r1), 0); }

/* instructions_0f.rs:3150-3160 -- movq mm/m64, mm.
 *
 * NOTE: upstream also defines `instr_0F7F(r: i32) -> u64 { read_mmx64s(r) }`, a
 * value-returning helper that has NO caller -- not here and not upstream (the two wrappers
 * below call mov_r_m64 / read_mmx64s directly).  It is dropped rather than carried as a
 * `static` that -Werror rejects as unused; recorded in results/m1b2-mmx-sse.md. */
void instr_0F7F_mem(int32_t addr, int32_t r) { mov_r_m64(addr, r); }
void instr_0F7F_reg(int32_t r1, int32_t r2)
{
	write_mmx_reg64(r1, read_mmx64s(r2));
	transition_fpu_to_mmx();
}
/* movdqa xmm/m128, xmm (XXX: aligned write or #gp) */
void instr_660F7F_mem(int32_t addr, int32_t r) { mov_r_m128(addr, r); }
void instr_660F7F_reg(int32_t r1, int32_t r2) { mov_r_r128(r1, r2); }
/* movdqu xmm/m128, xmm */
void instr_F30F7F_mem(int32_t addr, int32_t r) { mov_r_m128(addr, r); }
void instr_F30F7F_reg(int32_t r1, int32_t r2) { mov_r_r128(r1, r2); }

/* ------------------------------------------------------------------ *
 * 0F C2-C6 -- cmpps/cmppd/cmpsd/cmpss, movnti, pinsrw/pextrw, shufps/shufpd
 * (instructions_0f.rs:3814-3955)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:3814-3831 -- cmpps xmm, xmm/m128 */
static void instr_0FC2(reg128 source, int32_t r, int32_t imm8)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 4; i++) {
		result.i32[i] =
		    sse_comparison(imm8, (double)destination.f32[i], (double)source.f32[i]) ? -1 : 0;
	}
	write_xmm_reg128(r, result);
}
void instr_0FC2_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_0FC2(read_xmm128s(r1), r2, imm);
}
void instr_0FC2_mem(int32_t addr, int32_t r, int32_t imm)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0FC2(x, r, imm);
}
/* instructions_0f.rs:3832-3850 -- cmppd xmm, xmm/m128 (the mask is per 64-bit lane) */
static void instr_660FC2(reg128 source, int32_t r, int32_t imm8)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.i64[0] = sse_comparison(imm8, destination.f64[0], source.f64[0]) ? -1 : 0;
	result.i64[1] = sse_comparison(imm8, destination.f64[1], source.f64[1]) ? -1 : 0;
	write_xmm_reg128(r, result);
}
void instr_660FC2_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_660FC2(read_xmm128s(r1), r2, imm);
}
void instr_660FC2_mem(int32_t addr, int32_t r, int32_t imm)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FC2(x, r, imm);
}
/* instructions_0f.rs:3851-3870 -- cmpsd xmm, xmm/m64 */
static void instr_F20FC2(uint64_t source, int32_t r, int32_t imm8)
{
	reg128 a, b;
	uint64_t destination = read_xmm64s(r);
	a.u64[0] = destination;
	b.u64[0] = source;
	write_xmm64(r, sse_comparison(imm8, a.f64[0], b.f64[0]) ? (uint64_t)(int64_t)-1 : 0);
}
void instr_F20FC2_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_F20FC2(read_xmm64s(r1), r2, imm);
}
void instr_F20FC2_mem(int32_t addr, int32_t r, int32_t imm)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F20FC2(x, r, imm);
}
/* instructions_0f.rs:3871-3884 -- cmpss xmm, xmm/m32.
 * The source byte pattern is reinterpreted as f32 (`f32::from_bits(i32::cast_unsigned(..))`),
 * which is the reg128 union's f32 view; the _reg wrapper passes only the LOW 32 bits. */
static void instr_F30FC2(int32_t source, int32_t r, int32_t imm8)
{
	float destination = read_xmm_f32(r);
	reg128 in;
	float src;
	int32_t result;
	in.u32[0] = (uint32_t)source;
	src = in.f32[0];
	result = sse_comparison(imm8, (double)destination, (double)src) ? -1 : 0;
	write_xmm32(r, result);
}
void instr_F30FC2_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_F30FC2((int32_t)read_xmm64s(r1), r2, imm);
}
void instr_F30FC2_mem(int32_t addr, int32_t r, int32_t imm)
{
	int32_t x = safe_read32s(addr);
	if (cpu->pagefault) return;
	instr_F30FC2(x, r, imm);
}

/* instructions_0f.rs:3885-3889 -- movnti m32, r32 */
void instr_0FC3_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_0FC3_mem(int32_t addr, int32_t r)
{
	safe_write32(addr, read_reg32(r));
	if (cpu->pagefault) return;
}

/* instructions_0f.rs:3891-3932 -- pinsrw / pextrw */
static void instr_0FC4(int32_t source, int32_t r, int32_t imm8)
{
	reg128 destination;
	destination.u64[0] = read_mmx64s(r);
	destination.u16[imm8 & 3] = (uint16_t)source;
	write_mmx_reg64(r, destination.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FC4_reg(int32_t r1, int32_t r2, int32_t imm) { instr_0FC4(read_reg32(r1), r2, imm); }
void instr_0FC4_mem(int32_t addr, int32_t r, int32_t imm)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	instr_0FC4(x, r, imm);
}
static void instr_660FC4(int32_t source, int32_t r, int32_t imm8)
{
	reg128 destination = read_xmm128s(r);
	uint32_t index = (uint32_t)(imm8 & 7);
	destination.u16[index] = (uint16_t)(source & 0xFFFF);
	write_xmm_reg128(r, destination);
}
void instr_660FC4_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_660FC4(read_reg32(r1), r2, imm);
}
void instr_660FC4_mem(int32_t addr, int32_t r, int32_t imm)
{
	int32_t x = safe_read16(addr);
	if (cpu->pagefault) return;
	instr_660FC4(x, r, imm);
}
void instr_0FC5_mem(int32_t _addr, int32_t _r, int32_t _imm8)
{
	(void)_addr;
	(void)_r;
	(void)_imm8;
	trigger_ud();
}
void instr_0FC5_reg(int32_t r1, int32_t r2, int32_t imm8)
{
	reg128 data;
	data.u64[0] = read_mmx64s(r1);
	write_reg32(r2, (int32_t)data.u16[imm8 & 3]);
	transition_fpu_to_mmx();
}
void instr_660FC5_mem(int32_t _addr, int32_t _r, int32_t _imm8)
{
	(void)_addr;
	(void)_r;
	(void)_imm8;
	trigger_ud();
}
void instr_660FC5_reg(int32_t r1, int32_t r2, int32_t imm8)
{
	reg128 data = read_xmm128s(r1);
	uint32_t index = (uint32_t)(imm8 & 7);
	uint32_t result = data.u16[index];
	write_reg32(r2, (int32_t)result);
}

/* instructions_0f.rs:3933-3969 -- shufps / shufpd */
static void instr_0FC6(reg128 source, int32_t r, int32_t imm8)
{
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)destination.u32[imm8 & 3],
	             (int32_t)destination.u32[(imm8 >> 2) & 3],
	             (int32_t)source.u32[(imm8 >> 4) & 3],
	             (int32_t)source.u32[(imm8 >> 6) & 3]);
}
void instr_0FC6_reg(int32_t r1, int32_t r2, int32_t imm) { instr_0FC6(read_xmm128s(r1), r2, imm); }
void instr_0FC6_mem(int32_t addr, int32_t r, int32_t imm)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_0FC6(x, r, imm);
}
static void instr_660FC6(reg128 source, int32_t r, int32_t imm8)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.i64[0] = destination.i64[imm8 & 1];
	result.i64[1] = source.i64[(imm8 >> 1) & 1];
	write_xmm_reg128(r, result);
}
void instr_660FC6_reg(int32_t r1, int32_t r2, int32_t imm)
{
	instr_660FC6(read_xmm128s(r1), r2, imm);
}
void instr_660FC6_mem(int32_t addr, int32_t r, int32_t imm)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FC6(x, r, imm);
}

/* ------------------------------------------------------------------ *
 * 0F D0-E7 -- the MMX/SSE integer group, first half (instructions_0f.rs:4037-4665)
 *
 * SIGNED OVERFLOW: several of these bodies add or subtract i16/i32 lanes.  Rust's `+` on
 * a signed integer WRAPS in the reference's release profile (v86/Cargo.toml's
 * [profile.release] leaves overflow-checks at the default `false`), while the same
 * expression in C is UNDEFINED BEHAVIOUR.  Where a lane pair can actually reach the sign
 * bit (paddd/psubd/paddsw/pmaddwd/pmulhuw) the arithmetic is done on the uint32_t pattern
 * and cast back -- bit-identical to the reference, and defined.  Where it cannot
 * (paddb/psubb, the u16 lanes below 0x8000, pavgb's 9-bit sum) the plain form is kept.
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:4036 -- 0F D0 is unimplemented_sse (the bare name the emitter
 * calls: this opcode has no modrm operand, so there is no _mem/_reg pair) */
void instr_0FD0(void) { unimplemented_sse(); }

/* instructions_0f.rs:4038-4056 -- psrlw (and its XMM form, whose count is the source's
 * LOW 64 BITS -- `source.u64[0]`, not the whole register) */
static void instr_0FD1(uint64_t source, int32_t r) { psrlw_r64(r, source); }
void instr_0FD1_reg(int32_t r1, int32_t r2) { instr_0FD1(read_mmx64s(r1), r2); }
void instr_0FD1_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FD1(x, r);
}
static void instr_660FD1(reg128 source, int32_t r) { psrlw_r128(r, source.u64[0]); }
void instr_660FD1_reg(int32_t r1, int32_t r2) { instr_660FD1(read_xmm128s(r1), r2); }
void instr_660FD1_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FD1(x, r);
}

/* instructions_0f.rs:4057-4075 -- psrld */
static void instr_0FD2(uint64_t source, int32_t r) { psrld_r64(r, source); }
void instr_0FD2_reg(int32_t r1, int32_t r2) { instr_0FD2(read_mmx64s(r1), r2); }
void instr_0FD2_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FD2(x, r);
}
static void instr_660FD2(reg128 source, int32_t r) { psrld_r128(r, source.u64[0]); }
void instr_660FD2_reg(int32_t r1, int32_t r2) { instr_660FD2(read_xmm128s(r1), r2); }
void instr_660FD2_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FD2(x, r);
}

/* instructions_0f.rs:4076-4093 -- psrlq */
static void instr_0FD3(uint64_t source, int32_t r) { psrlq_r64(r, source); }
void instr_0FD3_reg(int32_t r1, int32_t r2) { instr_0FD3(read_mmx64s(r1), r2); }
void instr_0FD3_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FD3(x, r);
}
static void instr_660FD3(reg128 source, int32_t r) { psrlq_r128(r, source.u64[0]); }
void instr_660FD3_reg(int32_t r1, int32_t r2) { instr_660FD3(read_xmm128s(r1), r2); }
void instr_660FD3_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FD3(x, r);
}

/* instructions_0f.rs:4094-4118 -- paddq (u64 lanes: wrapping, no UB) */
static void instr_0FD4(uint64_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, source + destination);
	transition_fpu_to_mmx();
}
void instr_0FD4_reg(int32_t r1, int32_t r2) { instr_0FD4(read_mmx64s(r1), r2); }
void instr_0FD4_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FD4(x, r);
}
static void instr_660FD4(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.u64[0] = destination.u64[0] + source.u64[0];
	result.u64[1] = destination.u64[1] + source.u64[1];
	write_xmm_reg128(r, result);
}
void instr_660FD4_reg(int32_t r1, int32_t r2) { instr_660FD4(read_xmm128s(r1), r2); }
void instr_660FD4_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FD4(x, r);
}

/* instructions_0f.rs:4119-4149 -- pmullw (the low 16 bits of each lane product: the
 * u16*u16 product kept modulo 2^16, which C gets for free from the truncating store) */
static void instr_0FD5(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i] = (uint16_t)((uint32_t)destination.u16[i] * (uint32_t)src.u16[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FD5_reg(int32_t r1, int32_t r2) { instr_0FD5(read_mmx64s(r1), r2); }
void instr_0FD5_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FD5(x, r);
}
static void instr_660FD5(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] = (uint16_t)((uint32_t)destination.u16[i] * (uint32_t)source.u16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FD5_reg(int32_t r1, int32_t r2) { instr_660FD5(read_xmm128s(r1), r2); }
void instr_660FD5_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FD5(x, r);
}

/* instructions_0f.rs:4150-4180 -- movq xmm/m64, xmm and the movdq2q/movq2dq pair */
void instr_0FD6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0FD6_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_660FD6_mem(int32_t addr, int32_t r) { movl_r128_m64(addr, r); }
void instr_660FD6_reg(int32_t r1, int32_t r2) { write_xmm128_2(r1, read_xmm64s(r2), 0); }
void instr_F20FD6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
/* movdq2q mm, xmm */
void instr_F20FD6_reg(int32_t r1, int32_t r2)
{
	reg128 source = read_xmm128s(r1);
	write_mmx_reg64(r2, source.u64[0]);
	transition_fpu_to_mmx();
}
void instr_F30FD6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
/* movq2dq xmm, mm */
void instr_F30FD6_reg(int32_t r1, int32_t r2)
{
	uint64_t source = read_mmx64s(r1);
	reg128 result;
	result.u64[0] = source;
	result.u64[1] = 0;
	write_xmm_reg128(r2, result);
	transition_fpu_to_mmx();
}

/* instructions_0f.rs:4181-4206 -- pmovmskb.
 * NOTE the transition is inside the VALUE-RETURNING body, before the return: the reg
 * wrapper cannot express it (it is a single write_reg32 call). */
void instr_0FD7_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
static int32_t instr_0FD7(int32_t r1)
{
	reg128 x;
	int32_t result = 0;
	x.u64[0] = read_mmx64s(r1);
	for (int32_t i = 0; i < 8; i++) {
		result |= (int32_t)((uint32_t)x.u8[i] >> 7) << i;
	}
	transition_fpu_to_mmx();
	return result;
}
void instr_0FD7_reg(int32_t r1, int32_t r2) { write_reg32(r2, instr_0FD7(r1)); }
void instr_660FD7_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
static int32_t instr_660FD7(int32_t r1)
{
	reg128 x = read_xmm128s(r1);
	int32_t result = 0;
	for (int32_t i = 0; i < 16; i++) {
		result |= (int32_t)((uint32_t)x.u8[i] >> 7) << i;
	}
	return result;
}
void instr_660FD7_reg(int32_t r1, int32_t r2) { write_reg32(r2, instr_660FD7(r1)); }

/* instructions_0f.rs:4207-4236 -- psubusb */
static void instr_0FD8(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		uint8_t v = (uint8_t)saturate_sd_to_ub((int32_t)destination.u8[i] - (int32_t)src.u8[i]);
		result |= (uint64_t)v << (8 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FD8_reg(int32_t r1, int32_t r2) { instr_0FD8(read_mmx64s(r1), r2); }
void instr_0FD8_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FD8(x, r);
}
static void instr_660FD8(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = (uint8_t)saturate_sd_to_ub((int32_t)destination.u8[i] - (int32_t)source.u8[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FD8_reg(int32_t r1, int32_t r2) { instr_660FD8(read_xmm128s(r1), r2); }
void instr_660FD8_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FD8(x, r);
}

/* instructions_0f.rs:4237-4266 -- psubusw */
static void instr_0FD9(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		uint16_t v = saturate_uw((uint32_t)destination.u16[i] - (uint32_t)src.u16[i]);
		result |= (uint64_t)v << (16 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FD9_reg(int32_t r1, int32_t r2) { instr_0FD9(read_mmx64s(r1), r2); }
void instr_0FD9_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FD9(x, r);
}
static void instr_660FD9(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] = saturate_uw((uint32_t)destination.u16[i] - (uint32_t)source.u16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FD9_reg(int32_t r1, int32_t r2) { instr_660FD9(read_xmm128s(r1), r2); }
void instr_660FD9_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FD9(x, r);
}

/* instructions_0f.rs:4267-4297 -- pminub */
static void instr_0FDA(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i] = src.u8[i] < destination.u8[i] ? src.u8[i] : destination.u8[i];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FDA_reg(int32_t r1, int32_t r2) { instr_0FDA(read_mmx64s(r1), r2); }
void instr_0FDA_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FDA(x, r);
}
static void instr_660FDA(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = source.u8[i] < destination.u8[i] ? source.u8[i] : destination.u8[i];
	}
	write_xmm_reg128(r, result);
}
void instr_660FDA_reg(int32_t r1, int32_t r2) { instr_660FDA(read_xmm128s(r1), r2); }
void instr_660FDA_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FDA(x, r);
}

/* instructions_0f.rs:4298-4317 -- pand (mm) / pand (xmm) */
static void instr_0FDB(uint64_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, source & destination);
	transition_fpu_to_mmx();
}
void instr_0FDB_reg(int32_t r1, int32_t r2) { instr_0FDB(read_mmx64s(r1), r2); }
void instr_0FDB_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FDB(x, r);
}
static void instr_660FDB(reg128 source, int32_t r) { pand_r128(source, r); }
void instr_660FDB_reg(int32_t r1, int32_t r2) { instr_660FDB(read_xmm128s(r1), r2); }
void instr_660FDB_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FDB(x, r);
}

/* instructions_0f.rs:4318-4349 -- paddusb */
static void instr_0FDC(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		uint8_t v = saturate_ud_to_ub((uint32_t)destination.u8[i] + (uint32_t)src.u8[i]);
		result |= (uint64_t)v << (8 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FDC_reg(int32_t r1, int32_t r2) { instr_0FDC(read_mmx64s(r1), r2); }
void instr_0FDC_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FDC(x, r);
}
static void instr_660FDC(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = saturate_ud_to_ub((uint32_t)source.u8[i] + (uint32_t)destination.u8[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FDC_reg(int32_t r1, int32_t r2) { instr_660FDC(read_xmm128s(r1), r2); }
void instr_660FDC_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FDC(x, r);
}

/* instructions_0f.rs:4350-4380 -- paddusw */
static void instr_0FDD(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		uint16_t v = saturate_uw((uint32_t)destination.u16[i] + (uint32_t)src.u16[i]);
		result |= (uint64_t)v << (16 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FDD_reg(int32_t r1, int32_t r2) { instr_0FDD(read_mmx64s(r1), r2); }
void instr_0FDD_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FDD(x, r);
}
static void instr_660FDD(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] = saturate_uw((uint32_t)source.u16[i] + (uint32_t)destination.u16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FDD_reg(int32_t r1, int32_t r2) { instr_660FDD(read_xmm128s(r1), r2); }
void instr_660FDD_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FDD(x, r);
}

/* instructions_0f.rs:4381-4411 -- pmaxub */
static void instr_0FDE(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i] = src.u8[i] > destination.u8[i] ? src.u8[i] : destination.u8[i];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FDE_reg(int32_t r1, int32_t r2) { instr_0FDE(read_mmx64s(r1), r2); }
void instr_0FDE_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FDE(x, r);
}
static void instr_660FDE(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = source.u8[i] > destination.u8[i] ? source.u8[i] : destination.u8[i];
	}
	write_xmm_reg128(r, result);
}
void instr_660FDE_reg(int32_t r1, int32_t r2) { instr_660FDE(read_xmm128s(r1), r2); }
void instr_660FDE_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FDE(x, r);
}

/* instructions_0f.rs:4412-4432 -- pandn */
static void instr_0FDF(uint64_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, source & ~destination);
	transition_fpu_to_mmx();
}
void instr_0FDF_reg(int32_t r1, int32_t r2) { instr_0FDF(read_mmx64s(r1), r2); }
void instr_0FDF_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FDF(x, r);
}
static void instr_660FDF(reg128 source, int32_t r) { pandn_r128(source, r); }
void instr_660FDF_reg(int32_t r1, int32_t r2) { instr_660FDF(read_xmm128s(r1), r2); }
void instr_660FDF_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FDF(x, r);
}

/* instructions_0f.rs:4433-4463 -- pavgb.  `d + s + 1 >> 1` is `((d + s + 1) >> 1)`
 * (`+` binds tighter than `>>`); the sum is at most 511, so the promoted int is safe. */
static void instr_0FE0(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		uint8_t v = (uint8_t)(((int32_t)destination.u8[i] + (int32_t)src.u8[i] + 1) >> 1);
		result |= (uint64_t)v << (8 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FE0_reg(int32_t r1, int32_t r2) { instr_0FE0(read_mmx64s(r1), r2); }
void instr_0FE0_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE0(x, r);
}
static void instr_660FE0(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = (uint8_t)(((int32_t)destination.u8[i] + (int32_t)source.u8[i] + 1) >> 1);
	}
	write_xmm_reg128(r, result);
}
void instr_660FE0_reg(int32_t r1, int32_t r2) { instr_660FE0(read_xmm128s(r1), r2); }
void instr_660FE0_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE0(x, r);
}

/* instructions_0f.rs:4464-4482 -- psraw */
static void instr_0FE1(uint64_t source, int32_t r) { psraw_r64(r, source); }
void instr_0FE1_reg(int32_t r1, int32_t r2) { instr_0FE1(read_mmx64s(r1), r2); }
void instr_0FE1_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE1(x, r);
}
static void instr_660FE1(reg128 source, int32_t r) { psraw_r128(r, source.u64[0]); }
void instr_660FE1_reg(int32_t r1, int32_t r2) { instr_660FE1(read_xmm128s(r1), r2); }
void instr_660FE1_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE1(x, r);
}

/* instructions_0f.rs:4483-4501 -- psrad */
static void instr_0FE2(uint64_t source, int32_t r) { psrad_r64(r, source); }
void instr_0FE2_reg(int32_t r1, int32_t r2) { instr_0FE2(read_mmx64s(r1), r2); }
void instr_0FE2_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE2(x, r);
}
static void instr_660FE2(reg128 source, int32_t r) { psrad_r128(r, source.u64[0]); }
void instr_660FE2_reg(int32_t r1, int32_t r2) { instr_660FE2(read_xmm128s(r1), r2); }
void instr_660FE2_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE2(x, r);
}

/* instructions_0f.rs:4502-4531 -- pavgw */
static void instr_0FE3(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		uint16_t v = (uint16_t)(((int32_t)destination.u16[i] + (int32_t)src.u16[i] + 1) >> 1);
		result |= (uint64_t)v << (16 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FE3_reg(int32_t r1, int32_t r2) { instr_0FE3(read_mmx64s(r1), r2); }
void instr_0FE3_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE3(x, r);
}
static void instr_660FE3(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	for (int32_t i = 0; i < 8; i++) {
		destination.u16[i] =
		    (uint16_t)(((int32_t)destination.u16[i] + (int32_t)source.u16[i] + 1) >> 1);
	}
	write_xmm_reg128(r, destination);
}
void instr_660FE3_reg(int32_t r1, int32_t r2) { instr_660FE3(read_xmm128s(r1), r2); }
void instr_660FE3_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE3(x, r);
}

/* instructions_0f.rs:4532-4562 -- pmulhuw.
 *
 * The two u16 lanes are widened to i32 and MULTIPLIED AS i32, so the product of two large
 * lanes does not fit an i32 (0xFFFF * 0xFFFF = 4294836225) and the reference's release
 * profile WRAPS it.  The port multiplies on the uint32_t pattern instead -- defined
 * wrapping in C, where the same i32 multiply would be UB -- and takes `prod >> 16`.
 * Those two are the same 16 bits: a shift right by 16 differs between the arithmetic and
 * logical readings only ABOVE bit 15, and the lane keeps only bits 0..15.  So the port
 * computes the exact pmulhuw result (0xFFFF * 0xFFFF -> 0xFFFE), which is also what the
 * wrapped reference computes; there is no divergence to preserve here. */
static void instr_0FE4(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		uint32_t prod = (uint32_t)destination.u16[i] * (uint32_t)src.u16[i];
		result |= (uint64_t)(uint16_t)(prod >> 16) << (16 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FE4_reg(int32_t r1, int32_t r2) { instr_0FE4(read_mmx64s(r1), r2); }
void instr_0FE4_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE4(x, r);
}
static void instr_660FE4(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		uint32_t prod = (uint32_t)source.u16[i] * (uint32_t)destination.u16[i];
		result.u16[i] = (uint16_t)(prod >> 16);
	}
	write_xmm_reg128(r, result);
}
void instr_660FE4_reg(int32_t r1, int32_t r2) { instr_660FE4(read_xmm128s(r1), r2); }
void instr_660FE4_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE4(x, r);
}

/* instructions_0f.rs:4563-4593 -- pmulhw.  The i16 lanes widen to at most 2^30, so this
 * one CANNOT wrap and the plain signed form is exact. */
static void instr_0FE5(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t result = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		int32_t prod = (int32_t)destination.i16[i] * (int32_t)src.i16[i];
		result |= (uint64_t)(uint16_t)(prod >> 16) << (16 * i);
	}
	write_mmx_reg64(r, result);
	transition_fpu_to_mmx();
}
void instr_0FE5_reg(int32_t r1, int32_t r2) { instr_0FE5(read_mmx64s(r1), r2); }
void instr_0FE5_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE5(x, r);
}
static void instr_660FE5(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		int32_t prod = (int32_t)destination.i16[i] * (int32_t)source.i16[i];
		result.u16[i] = (uint16_t)(prod >> 16);
	}
	write_xmm_reg128(r, result);
}
void instr_660FE5_reg(int32_t r1, int32_t r2) { instr_660FE5(read_xmm128s(r1), r2); }
void instr_660FE5_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE5(x, r);
}

/* instructions_0f.rs:4594-4599 -- the MMX form of 0F E6 is #UD */
void instr_0FE6_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
void instr_0FE6_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}

/* instructions_0f.rs:4600-4648 -- the three E6 conversions */
static void instr_660FE6(reg128 source, int32_t r)
{
	reg128 result;
	result.i32[0] = sse_convert_with_truncation_f64_to_i32(source.f64[0]);
	result.i32[1] = sse_convert_with_truncation_f64_to_i32(source.f64[1]);
	result.i32[2] = 0;
	result.i32[3] = 0;
	write_xmm_reg128(r, result);
}
void instr_660FE6_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE6(x, r);
}
void instr_660FE6_reg(int32_t r1, int32_t r2) { instr_660FE6(read_xmm128s(r1), r2); }

static void instr_F20FE6(reg128 source, int32_t r)
{
	reg128 result;
	/* XXX: Precision exception */
	result.i32[0] = sse_convert_f64_to_i32(source.f64[0]);
	result.i32[1] = sse_convert_f64_to_i32(source.f64[1]);
	result.i32[2] = 0;
	result.i32[3] = 0;
	write_xmm_reg128(r, result);
}
void instr_F20FE6_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_F20FE6(x, r);
}
void instr_F20FE6_reg(int32_t r1, int32_t r2) { instr_F20FE6(read_xmm128s(r1), r2); }

static void instr_F30FE6(uint64_t source, int32_t r)
{
	/* Note: Conversion never fails (i32 fits into f64) */
	reg128 result;
	result.f64[0] = (double)(int32_t)(uint32_t)source;
	result.f64[1] = (double)(int32_t)(uint32_t)(source >> 32);
	write_xmm_reg128(r, result);
}
void instr_F30FE6_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_F30FE6(x, r);
}
void instr_F30FE6_reg(int32_t r1, int32_t r2) { instr_F30FE6(read_xmm64s(r1), r2); }

/* instructions_0f.rs:4653-4665 -- movntq / movntdq */
void instr_0FE7_mem(int32_t addr, int32_t r) { mov_r_m64(addr, r); }
void instr_0FE7_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_660FE7_reg(int32_t _r1, int32_t _r2)
{
	(void)_r1;
	(void)_r2;
	trigger_ud();
}
void instr_660FE7_mem(int32_t addr, int32_t r) { mov_r_m128(addr, r); }

/* ------------------------------------------------------------------ *
 * 0F E8-FF -- the MMX/SSE integer group, second half (instructions_0f.rs:4666-5300)
 *
 * (66 0F EF -- pxor xmm -- is M1b-1's, at the bottom of this file; only the MMX 0F EF is
 * this unit's.  0F FF is M1b-1's too.)
 * ------------------------------------------------------------------ */

/* instructions_0f.rs:4666-4696 -- psubsb.  `as u32` on an i8/i16 SIGN-EXTENDS, which is
 * what makes the wrapping subtraction below reach the saturating helper as a 32-bit
 * two's-complement pattern -- `(uint32_t)(int32_t)x` in C is that same pattern. */
static void instr_0FE8(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		result.i8[i] = saturate_sd_to_sb((uint32_t)destination.i8[i] - (uint32_t)src.i8[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FE8_reg(int32_t r1, int32_t r2) { instr_0FE8(read_mmx64s(r1), r2); }
void instr_0FE8_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE8(x, r);
}
static void instr_660FE8(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.i8[i] = saturate_sd_to_sb((uint32_t)destination.i8[i] - (uint32_t)source.i8[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FE8_reg(int32_t r1, int32_t r2) { instr_660FE8(read_xmm128s(r1), r2); }
void instr_660FE8_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE8(x, r);
}

/* instructions_0f.rs:4697-4727 -- psubsw */
static void instr_0FE9(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i] = saturate_sd_to_sw((uint32_t)destination.i16[i] - (uint32_t)src.i16[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FE9_reg(int32_t r1, int32_t r2) { instr_0FE9(read_mmx64s(r1), r2); }
void instr_0FE9_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FE9(x, r);
}
static void instr_660FE9(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] = saturate_sd_to_sw((uint32_t)destination.i16[i] - (uint32_t)source.i16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FE9_reg(int32_t r1, int32_t r2) { instr_660FE9(read_xmm128s(r1), r2); }
void instr_660FE9_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FE9(x, r);
}

/* instructions_0f.rs:4728-4758 -- pminsw */
static void instr_0FEA(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.i16[i] = destination.i16[i] < src.i16[i] ? destination.i16[i] : src.i16[i];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FEA_reg(int32_t r1, int32_t r2) { instr_0FEA(read_mmx64s(r1), r2); }
void instr_0FEA_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FEA(x, r);
}
static void instr_660FEA(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.i16[i] = destination.i16[i] < source.i16[i] ? destination.i16[i] : source.i16[i];
	}
	write_xmm_reg128(r, result);
}
void instr_660FEA_reg(int32_t r1, int32_t r2) { instr_660FEA(read_xmm128s(r1), r2); }
void instr_660FEA_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FEA(x, r);
}

/* instructions_0f.rs:4759-4779 -- por (mm + xmm) */
static void instr_0FEB(uint64_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, source | destination);
	transition_fpu_to_mmx();
}
void instr_0FEB_reg(int32_t r1, int32_t r2) { instr_0FEB(read_mmx64s(r1), r2); }
void instr_0FEB_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FEB(x, r);
}
static void instr_660FEB(reg128 source, int32_t r) { por_r128(source, r); }
void instr_660FEB_reg(int32_t r1, int32_t r2) { instr_660FEB(read_xmm128s(r1), r2); }
void instr_660FEB_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FEB(x, r);
}

/* instructions_0f.rs:4780-4810 -- paddsb */
static void instr_0FEC(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		result.i8[i] = saturate_sd_to_sb((uint32_t)destination.i8[i] + (uint32_t)src.i8[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FEC_reg(int32_t r1, int32_t r2) { instr_0FEC(read_mmx64s(r1), r2); }
void instr_0FEC_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FEC(x, r);
}
static void instr_660FEC(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.i8[i] = saturate_sd_to_sb((uint32_t)destination.i8[i] + (uint32_t)source.i8[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FEC_reg(int32_t r1, int32_t r2) { instr_660FEC(read_xmm128s(r1), r2); }
void instr_660FEC_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FEC(x, r);
}

/* instructions_0f.rs:4811-4841 -- paddsw */
static void instr_0FED(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i] = saturate_sd_to_sw((uint32_t)destination.i16[i] + (uint32_t)src.i16[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FED_reg(int32_t r1, int32_t r2) { instr_0FED(read_mmx64s(r1), r2); }
void instr_0FED_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FED(x, r);
}
static void instr_660FED(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] = saturate_sd_to_sw((uint32_t)destination.i16[i] + (uint32_t)source.i16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FED_reg(int32_t r1, int32_t r2) { instr_660FED(read_xmm128s(r1), r2); }
void instr_660FED_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FED(x, r);
}

/* instructions_0f.rs:4842-4872 -- pmaxsw */
static void instr_0FEE(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.i16[i] = destination.i16[i] > src.i16[i] ? destination.i16[i] : src.i16[i];
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FEE_reg(int32_t r1, int32_t r2) { instr_0FEE(read_mmx64s(r1), r2); }
void instr_0FEE_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FEE(x, r);
}
static void instr_660FEE(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.i16[i] = destination.i16[i] > source.i16[i] ? destination.i16[i] : source.i16[i];
	}
	write_xmm_reg128(r, result);
}
void instr_660FEE_reg(int32_t r1, int32_t r2) { instr_660FEE(read_xmm128s(r1), r2); }
void instr_660FEE_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FEE(x, r);
}

/* instructions_0f.rs:4873-4883 -- pxor mm (66 0F EF is M1b-1's, above) */
static void instr_0FEF(uint64_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, source ^ destination);
	transition_fpu_to_mmx();
}
void instr_0FEF_reg(int32_t r1, int32_t r2) { instr_0FEF(read_mmx64s(r1), r2); }
void instr_0FEF_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FEF(x, r);
}

/* instructions_0f.rs:4895 -- 0F F0, like 0F D0, is a bare unimplemented_sse */
void instr_0FF0(void) { unimplemented_sse(); }

/* instructions_0f.rs:4896-4933 -- psllw */
static void instr_0FF1(uint64_t source, int32_t r) { psllw_r64(r, source); }
void instr_0FF1_reg(int32_t r1, int32_t r2) { instr_0FF1(read_mmx64s(r1), r2); }
void instr_0FF1_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF1(x, r);
}
static void instr_660FF1(reg128 source, int32_t r) { psllw_r128(r, source.u64[0]); }
void instr_660FF1_reg(int32_t r1, int32_t r2) { instr_660FF1(read_xmm128s(r1), r2); }
void instr_660FF1_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF1(x, r);
}

/* instructions_0f.rs:4916-4933 -- pslld */
static void instr_0FF2(uint64_t source, int32_t r) { pslld_r64(r, source); }
void instr_0FF2_reg(int32_t r1, int32_t r2) { instr_0FF2(read_mmx64s(r1), r2); }
void instr_0FF2_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF2(x, r);
}
static void instr_660FF2(reg128 source, int32_t r) { pslld_r128(r, source.u64[0]); }
void instr_660FF2_reg(int32_t r1, int32_t r2) { instr_660FF2(read_xmm128s(r1), r2); }
void instr_660FF2_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF2(x, r);
}

/* instructions_0f.rs:4935-4952 -- psllq */
static void instr_0FF3(uint64_t source, int32_t r) { psllq_r64(r, source); }
void instr_0FF3_reg(int32_t r1, int32_t r2) { instr_0FF3(read_mmx64s(r1), r2); }
void instr_0FF3_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF3(x, r);
}
static void instr_660FF3(reg128 source, int32_t r) { psllq_r128(r, source.u64[0]); }
void instr_660FF3_reg(int32_t r1, int32_t r2) { instr_660FF3(read_xmm128s(r1), r2); }
void instr_660FF3_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF3(x, r);
}

/* instructions_0f.rs:4954-4977 -- pmuludq */
static void instr_0FF4(uint64_t source, int32_t r)
{
	uint64_t destination = read_mmx64s(r);
	write_mmx_reg64(r, (uint64_t)(uint32_t)source * (uint64_t)(uint32_t)destination);
	transition_fpu_to_mmx();
}
void instr_0FF4_reg(int32_t r1, int32_t r2) { instr_0FF4(read_mmx64s(r1), r2); }
void instr_0FF4_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF4(x, r);
}
static void instr_660FF4(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	result.u64[0] = (uint64_t)source.u32[0] * (uint64_t)destination.u32[0];
	result.u64[1] = (uint64_t)source.u32[2] * (uint64_t)destination.u32[2];
	write_xmm_reg128(r, result);
}
void instr_660FF4_reg(int32_t r1, int32_t r2) { instr_660FF4(read_xmm128s(r1), r2); }
void instr_660FF4_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF4(x, r);
}

/* instructions_0f.rs:4979-5011 -- pmaddwd.
 * Each lane pair sums two products of i16 values, i.e. up to 2 * 2^30 = 2^31: the sum
 * CAN reach the sign bit, so it too is formed on the uint32_t pattern (see the file's
 * note on the reference's release-profile wrapping). */
static void instr_0FF5(uint64_t source, int32_t r)
{
	reg128 destination, src;
	int32_t mul0, mul1, mul2, mul3, low, high;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	mul0 = (int32_t)destination.i16[0] * (int32_t)src.i16[0];
	mul1 = (int32_t)destination.i16[1] * (int32_t)src.i16[1];
	mul2 = (int32_t)destination.i16[2] * (int32_t)src.i16[2];
	mul3 = (int32_t)destination.i16[3] * (int32_t)src.i16[3];
	low = (int32_t)((uint32_t)mul0 + (uint32_t)mul1);
	high = (int32_t)((uint32_t)mul2 + (uint32_t)mul3);
	write_mmx_reg64(r, (uint64_t)(uint32_t)low | ((uint64_t)(uint32_t)high << 32));
	transition_fpu_to_mmx();
}
void instr_0FF5_reg(int32_t r1, int32_t r2) { instr_0FF5(read_mmx64s(r1), r2); }
void instr_0FF5_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF5(x, r);
}
static void instr_660FF5(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 4; i++) {
		uint32_t a = (uint32_t)((int32_t)destination.i16[2 * i] * (int32_t)source.i16[2 * i]);
		uint32_t b =
		    (uint32_t)((int32_t)destination.i16[2 * i + 1] * (int32_t)source.i16[2 * i + 1]);
		result.i32[i] = (int32_t)(a + b);
	}
	write_xmm_reg128(r, result);
}
void instr_660FF5_reg(int32_t r1, int32_t r2) { instr_660FF5(read_xmm128s(r1), r2); }
void instr_660FF5_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF5(x, r);
}

/* instructions_0f.rs:5013-5044 -- psadbw */
static void instr_0FF6(uint64_t source, int32_t r)
{
	reg128 destination, src;
	uint64_t sum = 0;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		int32_t diff = (int32_t)destination.u8[i] - (int32_t)src.u8[i];
		sum += (uint64_t)(diff < 0 ? -diff : diff);
	}
	write_mmx_reg64(r, sum);
	transition_fpu_to_mmx();
}
void instr_0FF6_reg(int32_t r1, int32_t r2) { instr_0FF6(read_mmx64s(r1), r2); }
void instr_0FF6_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF6(x, r);
}
static void instr_660FF6(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	uint32_t sum0 = 0, sum1 = 0;
	for (int32_t i = 0; i < 8; i++) {
		int32_t d0 = (int32_t)destination.u8[i + 0] - (int32_t)source.u8[i + 0];
		int32_t d1 = (int32_t)destination.u8[i + 8] - (int32_t)source.u8[i + 8];
		sum0 += (uint32_t)(d0 < 0 ? -d0 : d0);
		sum1 += (uint32_t)(d1 < 0 ? -d1 : d1);
	}
	write_xmm128(r, (int32_t)sum0, 0, (int32_t)sum1, 0);
}
void instr_660FF6_reg(int32_t r1, int32_t r2) { instr_660FF6(read_xmm128s(r1), r2); }
void instr_660FF6_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF6(x, r);
}

/* instructions_0f.rs:5045-5081 -- maskmovq/maskmovdqu.
 *
 * The mask bit is tested on the BYTE's high bit (`mask[i] & 0x80`), the write goes through
 * safe_write8, and the `.unwrap()` is the port's DBG_ASSERT: the caller ran
 * writable_or_pagefault over the whole 8/16-byte window first, so none of the writes can
 * fault.  The transition runs after the loop, not per byte. */
void instr_0FF7_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
static void maskmovq(int32_t r1, int32_t r2, int32_t addr)
{
	reg128 source, mask;
	source.u64[0] = read_mmx64s(r2);
	mask.u64[0] = read_mmx64s(r1);
	for (int32_t i = 0; i < 8; i++) {
		if (0 != (mask.u8[i] & 0x80)) {
			safe_write8(addr + i, (int32_t)source.u8[i]);
			DBG_ASSERT(!cpu->pagefault);
		}
	}
	transition_fpu_to_mmx();
}
void instr_0FF7_reg(int32_t r1, int32_t r2)
{
	int32_t addr = get_seg_prefix_ds(get_reg_asize(EDI));
	if (cpu->pagefault) return;
	writable_or_pagefault(addr, 8);
	if (cpu->pagefault) return;
	maskmovq(r1, r2, addr);
}
void instr_660FF7_mem(int32_t _addr, int32_t _r)
{
	(void)_addr;
	(void)_r;
	trigger_ud();
}
static void maskmovdqu(int32_t r1, int32_t r2, int32_t addr)
{
	reg128 source = read_xmm128s(r2);
	reg128 mask = read_xmm128s(r1);
	for (int32_t i = 0; i < 16; i++) {
		if (0 != (mask.u8[i] & 0x80)) {
			safe_write8(addr + i, (int32_t)source.u8[i]);
			DBG_ASSERT(!cpu->pagefault);
		}
	}
}
void instr_660FF7_reg(int32_t r1, int32_t r2)
{
	int32_t addr = get_seg_prefix_ds(get_reg_asize(EDI));
	if (cpu->pagefault) return;
	writable_or_pagefault(addr, 16);
	if (cpu->pagefault) return;
	maskmovdqu(r1, r2, addr);
}

/* instructions_0f.rs:5084-5130 -- psubb */
static void instr_0FF8(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i] = (uint8_t)(destination.u8[i] - src.u8[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FF8_reg(int32_t r1, int32_t r2) { instr_0FF8(read_mmx64s(r1), r2); }
void instr_0FF8_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF8(x, r);
}
static void instr_660FF8(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = (uint8_t)(destination.u8[i] - source.u8[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FF8_reg(int32_t r1, int32_t r2) { instr_660FF8(read_xmm128s(r1), r2); }
void instr_660FF8_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF8(x, r);
}

/* instructions_0f.rs:5131-5161 -- psubw */
static void instr_0FF9(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.i16[i] = (int16_t)(destination.i16[i] - src.i16[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FF9_reg(int32_t r1, int32_t r2) { instr_0FF9(read_mmx64s(r1), r2); }
void instr_0FF9_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FF9(x, r);
}
static void instr_660FF9(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 8; i++) {
		result.i16[i] = (int16_t)(destination.i16[i] - source.i16[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FF9_reg(int32_t r1, int32_t r2) { instr_660FF9(read_xmm128s(r1), r2); }
void instr_660FF9_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FF9(x, r);
}

/* instructions_0f.rs:5147-5178 -- psubd (i32 lanes: uint32_t arithmetic, see the note) */
static void instr_0FFA(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 2; i++) {
		result.i32[i] = (int32_t)((uint32_t)destination.i32[i] - (uint32_t)src.i32[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FFA_reg(int32_t r1, int32_t r2) { instr_0FFA(read_mmx64s(r1), r2); }
void instr_0FFA_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FFA(x, r);
}
static void instr_660FFA(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	write_xmm128(r,
	             (int32_t)((uint32_t)destination.i32[0] - (uint32_t)source.i32[0]),
	             (int32_t)((uint32_t)destination.i32[1] - (uint32_t)source.i32[1]),
	             (int32_t)((uint32_t)destination.i32[2] - (uint32_t)source.i32[2]),
	             (int32_t)((uint32_t)destination.i32[3] - (uint32_t)source.i32[3]));
}
void instr_660FFA_reg(int32_t r1, int32_t r2) { instr_660FFA(read_xmm128s(r1), r2); }
void instr_660FFA_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FFA(x, r);
}

/* instructions_0f.rs:5180-5201 -- psubq (u64 lanes: wrapping, no UB) */
static void instr_0FFB(uint64_t source, int32_t r)
{
	write_mmx_reg64(r, read_mmx64s(r) - source);
	transition_fpu_to_mmx();
}
void instr_0FFB_reg(int32_t r1, int32_t r2) { instr_0FFB(read_mmx64s(r1), r2); }
void instr_0FFB_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FFB(x, r);
}
static void instr_660FFB(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	destination.u64[0] = destination.u64[0] - source.u64[0];
	destination.u64[1] = destination.u64[1] - source.u64[1];
	write_xmm_reg128(r, destination);
}
void instr_660FFB_reg(int32_t r1, int32_t r2) { instr_660FFB(read_xmm128s(r1), r2); }
void instr_660FFB_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FFB(x, r);
}

/* instructions_0f.rs:5203-5232 -- paddb */
static void instr_0FFC(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 8; i++) {
		result.u8[i] = (uint8_t)(destination.u8[i] + src.u8[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FFC_reg(int32_t r1, int32_t r2) { instr_0FFC(read_mmx64s(r1), r2); }
void instr_0FFC_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FFC(x, r);
}
static void instr_660FFC(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	for (int32_t i = 0; i < 16; i++) {
		result.u8[i] = (uint8_t)(destination.u8[i] + source.u8[i]);
	}
	write_xmm_reg128(r, result);
}
void instr_660FFC_reg(int32_t r1, int32_t r2) { instr_660FFC(read_xmm128s(r1), r2); }
void instr_660FFC_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FFC(x, r);
}

/* instructions_0f.rs:5234-5263 -- paddw */
static void instr_0FFD(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 4; i++) {
		result.u16[i] = (uint16_t)(destination.u16[i] + src.u16[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FFD_reg(int32_t r1, int32_t r2) { instr_0FFD(read_mmx64s(r1), r2); }
void instr_0FFD_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FFD(x, r);
}
static void instr_660FFD(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	reg128 result;
	/* `(d + s) & 0xFFFF` -- `+` binds tighter than `&` in upstream's expression */
	for (int32_t i = 0; i < 8; i++) {
		result.u16[i] = (uint16_t)(((int32_t)destination.u16[i] + (int32_t)source.u16[i]) & 0xFFFF);
	}
	write_xmm_reg128(r, result);
}
void instr_660FFD_reg(int32_t r1, int32_t r2) { instr_660FFD(read_xmm128s(r1), r2); }
void instr_660FFD_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FFD(x, r);
}

/* instructions_0f.rs:5280-5299 -- paddd (i32 lanes, uint32_t arithmetic) */
static void instr_0FFE(uint64_t source, int32_t r)
{
	reg128 destination, src, result;
	destination.u64[0] = read_mmx64s(r);
	src.u64[0] = source;
	for (int32_t i = 0; i < 2; i++) {
		result.i32[i] = (int32_t)((uint32_t)destination.i32[i] + (uint32_t)src.i32[i]);
	}
	write_mmx_reg64(r, result.u64[0]);
	transition_fpu_to_mmx();
}
void instr_0FFE_reg(int32_t r1, int32_t r2) { instr_0FFE(read_mmx64s(r1), r2); }
void instr_0FFE_mem(int32_t addr, int32_t r)
{
	uint64_t x = safe_read64s(addr);
	if (cpu->pagefault) return;
	instr_0FFE(x, r);
}
static void instr_660FFE(reg128 source, int32_t r)
{
	reg128 destination = read_xmm128s(r);
	int32_t dword0 = (int32_t)((uint32_t)destination.i32[0] + (uint32_t)source.i32[0]);
	int32_t dword1 = (int32_t)((uint32_t)destination.i32[1] + (uint32_t)source.i32[1]);
	int32_t dword2 = (int32_t)((uint32_t)destination.i32[2] + (uint32_t)source.i32[2]);
	int32_t dword3 = (int32_t)((uint32_t)destination.i32[3] + (uint32_t)source.i32[3]);
	write_xmm128(r, dword0, dword1, dword2, dword3);
}
void instr_660FFE_reg(int32_t r1, int32_t r2) { instr_660FFE(read_xmm128s(r1), r2); }
void instr_660FFE_mem(int32_t addr, int32_t r)
{
	reg128 x = safe_read128s(addr);
	if (cpu->pagefault) return;
	instr_660FFE(x, r);
}

/* ================================================================== *
 * M1c (HANDOFF=v86c) -- THE LAST 31 SYMBOLS OF THE 0F TABLE.
 *
 * The exact set the emitter's census still named after M1b-1 (see this file's header and
 * results/m1b1-0f-nonsse.md's divergence table): 0F 00 /2../5 (lldt/ltr/verr/verw, 16
 * encodings), 0F 02/03 (lar/lsl, 8), 0F 30/31/32/34/35 (wrmsr/rdtsc/rdmsr/sysenter/
 * sysexit, 5) and 0F C7 /6's register form (rdrand, 2).  Every body is upstream's
 * instructions_0f.rs, and every one of them was blocked on a helper M1c landed:
 * exceptions.h's segment-descriptor API + lookup_segment_selector + load_tr/load_ldt +
 * update_cs_size/cpl_changed, misc_instr.h's lar/lsl/verr/verw, and cpu.h's read_tsc/
 * set_tsc.  APPENDED, never interleaved: the 910 definitions above are M1b's reviewed
 * work.
 * ================================================================== */

/* ------------------------------------------------------------------ *
 * cpu.rs:214-248 -- the MSR numbers 0F 30/0F 32 dispatch on.
 *
 * Defined here rather than in cpu_state.h (where cpu.rs's shared constants live)
 * because these two instructions are the only users of this block in the port; the
 * spellings match upstream's `i32 = 0x...u32 as i32` exactly.  MSR_EFER and EFER_NXE
 * are NOT repeated: they are already in cpu_state.h and are simply cast in the case
 * labels below.
 * ------------------------------------------------------------------ */
#define IA32_TIME_STAMP_COUNTER ((int32_t)0x10)
#define IA32_PLATFORM_ID        ((int32_t)0x17)
#define IA32_APIC_BASE          ((int32_t)0x1B)
#define MSR_TEST_CTRL           ((int32_t)0x33)
#define IA32_FEAT_CTL           ((int32_t)0x3A)
#define IA32_SPEC_CTRL          ((int32_t)0x48)
#define IA32_BIOS_UPDT_TRIG     ((int32_t)0x79)
#define IA32_BIOS_SIGN_ID       ((int32_t)0x8B)
#define IA32_PMC0               ((int32_t)0xC1)
#define IA32_PMC1               ((int32_t)0xC2)
#define MSR_SMI_COUNT           ((int32_t)0x34)
#define MSR_PLATFORM_INFO       ((int32_t)0xCE)
#define MSR_TSX_FORCE_ABORT     ((int32_t)0x10F)
#define IA32_TSX_CTRL           ((int32_t)0x122)
#define IA32_MCU_OPT_CTRL       ((int32_t)0x123)
#define MISC_FEATURE_ENABLES    ((int32_t)0x140)
#define IA32_SYSENTER_CS        ((int32_t)0x174)
#define IA32_SYSENTER_ESP       ((int32_t)0x175)
#define IA32_SYSENTER_EIP       ((int32_t)0x176)
#define IA32_MCG_CAP            ((int32_t)0x179)
#define IA32_PERFEVTSEL0        ((int32_t)0x186)
#define IA32_PERFEVTSEL1        ((int32_t)0x187)
#define IA32_MISC_ENABLE        ((int32_t)0x1A0)
#define IA32_PAT                ((int32_t)0x277)
#define IA32_RTIT_CTL           ((int32_t)0x570)
#define MSR_PKG_C2_RESIDENCY    ((int32_t)0x60D)
#define IA32_KERNEL_GS_BASE     ((int32_t)0xC0000101u)
#define MSR_AMD64_LS_CFG        ((int32_t)0xC0011020u)
#define MSR_AMD64_DE_CFG        ((int32_t)0xC0011029u)
#define IA32_APIC_BASE_BSP      ((int32_t)1 << 8)
#define IA32_APIC_BASE_EXTD     ((int32_t)1 << 10)
#define IA32_APIC_BASE_EN       ((int32_t)1 << 11)

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:91-125 -- 0F 00 /2, /3: lldt, ltr
 * ------------------------------------------------------------------ */
void instr16_0F00_2_mem(int32_t addr)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
	}
	else if (cpu->cpl != 0) {
		trigger_gp(0);
	}
	else {
		int32_t selector = safe_read16(addr);
		if (cpu->pagefault) return;
		load_ldt(selector);
	}
}
void instr32_0F00_2_mem(int32_t addr) { instr16_0F00_2_mem(addr); }
void instr16_0F00_2_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
	}
	else if (cpu->cpl != 0) {
		trigger_gp(0);
	}
	else {
		load_ldt(read_reg16(r));
	}
}
void instr32_0F00_2_reg(int32_t r) { instr16_0F00_2_reg(r); }

void instr16_0F00_3_mem(int32_t addr)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
	}
	else if (cpu->cpl != 0) {
		trigger_gp(0);
	}
	else {
		int32_t selector = safe_read16(addr);
		if (cpu->pagefault) return;
		load_tr(selector);
	}
}
void instr32_0F00_3_mem(int32_t addr) { instr16_0F00_3_mem(addr); }
void instr16_0F00_3_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
	}
	else if (cpu->cpl != 0) {
		trigger_gp(0);
	}
	else {
		load_tr(read_reg16(r));
	}
}
void instr32_0F00_3_reg(int32_t r) { instr16_0F00_3_reg(r); }

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:127-160 -- 0F 00 /4, /5: verr, verw
 * ------------------------------------------------------------------ */
void instr16_0F00_4_mem(int32_t addr)
{
	if (!cpu->protected_mode || vm86_mode()) {
		/* upstream dbg_log!("verr #ud") */
		trigger_ud();
		return;
	}
	int32_t selector = safe_read16(addr);
	if (cpu->pagefault) return;
	verr(selector);
}
void instr32_0F00_4_mem(int32_t addr) { instr16_0F00_4_mem(addr); }
void instr16_0F00_4_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	verr(read_reg16(r));
}
void instr32_0F00_4_reg(int32_t r) { instr16_0F00_4_reg(r); }

void instr16_0F00_5_mem(int32_t addr)
{
	if (!cpu->protected_mode || vm86_mode()) {
		/* upstream dbg_log!("verw #ud") */
		trigger_ud();
		return;
	}
	int32_t selector = safe_read16(addr);
	if (cpu->pagefault) return;
	verw(selector);
}
void instr32_0F00_5_mem(int32_t addr) { instr16_0F00_5_mem(addr); }
void instr16_0F00_5_reg(int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	verw(read_reg16(r));
}
void instr32_0F00_5_reg(int32_t r) { instr16_0F00_5_reg(r); }

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:329-380 -- 0F 02, 0F 03: lar, lsl
 * ------------------------------------------------------------------ */
void instr16_0F02_mem(int32_t addr, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		/* upstream dbg_log!("lar #ud") */
		trigger_ud();
		return;
	}
	int32_t selector = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, lar(selector, read_reg16(r)));
}
void instr16_0F02_reg(int32_t r1, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg16(r, lar(read_reg16(r1), read_reg16(r)));
}
void instr32_0F02_mem(int32_t addr, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	int32_t selector = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg32(r, lar(selector, read_reg32(r)));
}
void instr32_0F02_reg(int32_t r1, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg32(r, lar(read_reg16(r1), read_reg32(r)));
}

void instr16_0F03_mem(int32_t addr, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		/* upstream dbg_log!("lsl #ud") */
		trigger_ud();
		return;
	}
	int32_t selector = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg16(r, lsl(selector, read_reg16(r)));
}
void instr16_0F03_reg(int32_t r1, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg16(r, lsl(read_reg16(r1), read_reg16(r)));
}
void instr32_0F03_mem(int32_t addr, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	int32_t selector = safe_read16(addr);
	if (cpu->pagefault) return;
	write_reg32(r, lsl(selector, read_reg32(r)));
}
void instr32_0F03_reg(int32_t r1, int32_t r)
{
	if (!cpu->protected_mode || vm86_mode()) {
		trigger_ud();
		return;
	}
	write_reg32(r, lsl(read_reg16(r1), read_reg32(r)));
}

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:1183-1268 -- 0F 30: wrmsr
 *
 * The block lists the registers the port must retain.  The `dbg_log!`s are dropped, and so
 * are the `dbg_assert!`s (R5's S2, resolved R5F the way R4F resolved the fxsave/fxrstor
 * pair): they are `debug_assert!` (dbg.rs:63-67), COMPILED OUT of the pinned release
 * reference, which therefore USES THE UNCHECKED VALUE.  The earlier rule here -- "kept only
 * where they guard a value the code then USES" -- did not distinguish anything: a value the
 * release code uses is exactly a value the release code uses unchecked.  A guest writing
 * reserved EFER bits, or an APIC base with a nonzero high word / a relocated address / the
 * EXTD bit, aborted the shipped machine where the reference silently proceeds.  Upstream's
 * message text is kept as a comment at each site, as at fpu.c:1152/1197.
 * ------------------------------------------------------------------ */
void instr_0F30(void)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}

	int32_t index = read_reg32(ECX);
	int32_t low = read_reg32(EAX);
	int32_t high = read_reg32(EDX);

	switch (index) {
	/* S11: THE FOUR SEGMENT/MSR AREAS.  In long mode the FS and GS bases live in MSRs, not in
	 * descriptors, and the segment_offsets[] slots S3 widened are where this port keeps them --
	 * so `wrmsr(FS_BASE)` writes the base the address generator already reads, and a guest's
	 * `mov rax, fs:[0]` follows it.  KERNEL_GS_BASE is the other half of SWAPGS. */
	case (int32_t)MSR_FS_BASE: {
		cpu->segment_offsets[FS] = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
	}
		break;
	case (int32_t)MSR_GS_BASE: {
		cpu->segment_offsets[GS] = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
	}
		break;
	case (int32_t)MSR_KERNEL_GS_BASE: {
		cpu->kernel_gs_base = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
	}
		break;
	case (int32_t)MSR_STAR:
		cpu->star = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
		break;
	case (int32_t)MSR_LSTAR:
		cpu->lstar = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
		break;
	case (int32_t)MSR_FMASK:
		cpu->fmask = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
		break;
	case (int32_t)MSR_EFER: {
		uint64_t value = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
		/* upstream: dbg_assert!(value & !EFER_NXE == 0, "Unimplemented EFER bits") --
		 * dropped (R5's S2): the release reference stores the guest's value unchecked,
		 * and that is still what happens to every OTHER bit here.
		 *
		 * ... EXCEPT EFER.LME, which S2 makes a NAMED REFUSAL when the selected backend
		 * cannot run long mode (plan ss3.5.2, C5).  That is not a re-added dbg_assert:
		 * upstream's was compiled out of the shipped reference and would have aborted the
		 * host; this refuses the GUEST's transition by name and leaves EFER unwritten, so
		 * a guest that then sets CR0.PG cannot reach a 4-level table with a 3-level walk.
		 * The bit is read as the GUEST asked for it -- a write that clears LME is always
		 * allowed, on any backend (LMA is cleared by hardware when LME goes away, and this
		 * port never sets LMA on a backend that cannot run long mode at all). */
		if ((value & EFER_LME) != 0 && cpu_backend_refuse_long_mode("EFER.LME")) {
			return;
		}
		if (value != cpu->efer) {
			full_clear_tlb();
		}
		cpu->efer = value;
		/* S3 MODE BOUNDARY: the third transition, and the only write that can clear
		 * EFER.LMA as well as ask for it.  After the commit: if a long-mode machine (S5)
		 * ever drops back to a 32-bit mode with a non-zero high half in a widened address
		 * field, this is the seat that names it instead of letting the 32-bit walk read
		 * the low half of a 64-bit address. */
		cpu_assert_address_widening("WRMSR(EFER)");
	}
		break;
	case IA32_SYSENTER_CS: cpu->sysenter_cs = low & 0xFFFF; break;
	case IA32_SYSENTER_EIP: cpu->sysenter_eip = low; break;
	case IA32_SYSENTER_ESP: cpu->sysenter_esp = low; break;
	case IA32_FEAT_CTL: break;   /* linux 5.x */
	case MSR_TEST_CTRL: break;   /* linux 5.x */
	case IA32_APIC_BASE: {
		/* upstream's three dbg_assert!s here are dropped (S2):
		 *   dbg_assert!(high == 0, "Changing APIC address (high 32 bits) not supported")
		 *   dbg_assert!((address == 0 && !*acpi_enabled) || address == APIC_MEM_ADDRESS,
		 *               "Changing APIC address not supported")
		 *   dbg_assert!(low & IA32_APIC_BASE_EXTD == 0, "x2apic not supported")
		 * Upstream's `address` local existed only as the second assert's operand (it is
		 * computed and never used otherwise), so it goes with them: the release
		 * reference masks it out of `low`, and nothing else reads it. */
		cpu->apic_enabled = (low & IA32_APIC_BASE_EN) == IA32_APIC_BASE_EN;
	}
		break;
	case IA32_TIME_STAMP_COUNTER: set_tsc((uint32_t)low, (uint32_t)high); break;
	case IA32_BIOS_UPDT_TRIG: break; /* windows xp */
	case IA32_BIOS_SIGN_ID: break;
	case MISC_FEATURE_ENABLES: break; /* Linux 4 */
	case IA32_MISC_ENABLE: break;     /* Enable Misc. Processor Features */
	case IA32_MCG_CAP: break;         /* netbsd */
	case IA32_PERFEVTSEL0:      /* fallthrough */
	case IA32_PERFEVTSEL1: break; /* linux/9legacy */
	case IA32_PMC0:      /* fallthrough */
	case IA32_PMC1: break; /* linux */
	case IA32_PAT: {
		uint64_t value = ((uint64_t)(uint32_t)high << 32) | (uint64_t)(uint32_t)low;
		for (int i = 0; i < 8; i++) {
			uint8_t entry = (uint8_t)(value >> (8 * i));
			if (entry > 7 || entry == 2 || entry == 3) {
				trigger_gp(0);
				return;
			}
		}
		/* Cache timing and memory types are not modelled, but the register must retain
		 * the guest's selected types. */
		cpu->pat = value;
	}
		break;
	case IA32_SPEC_CTRL: break;      /* linux 5.19 */
	case IA32_TSX_CTRL: break;       /* linux 5.19 */
	case MSR_TSX_FORCE_ABORT: break; /* linux 5.19 */
	case IA32_MCU_OPT_CTRL: break;   /* linux 5.19 */
	case MSR_AMD64_LS_CFG: break;    /* linux 5.19 */
	case MSR_AMD64_DE_CFG: break;    /* linux 6.1 */
	default:
		/* upstream: dbg_log!("Unknown msr") + dbg_assert!(false) -- BOTH dropped: the
		 * reference (release wasm) silently ignores an unknown MSR. */
		break;
	}
}

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:1272-1285 -- 0F 31: rdtsc
 * ------------------------------------------------------------------ */
void instr_0F31(void)
{
	if (cpu->cpl == 0 || (cpu->cr[4] & CR4_TSD) == 0) {
		uint64_t tsc = read_tsc();
		write_reg32(EAX, (int32_t)tsc);
		write_reg32(EDX, (int32_t)(tsc >> 32));
	}
	else {
		trigger_gp(0);
	}
}

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:1288-1360 -- 0F 32: rdmsr
 * ------------------------------------------------------------------ */
void instr_0F32(void)
{
	if (cpu->cpl != 0) {
		trigger_gp(0);
		return;
	}

	int32_t index = read_reg32(ECX);

	int32_t low = 0;
	int32_t high = 0;

	switch (index) {
	case (int32_t)MSR_EFER: {
		uint64_t val = cpu->efer;
		low = (int32_t)val;
		high = (int32_t)(val >> 32);
	}
		break;
	case (int32_t)MSR_FS_BASE:
		low = (int32_t)cpu->segment_offsets[FS];
		high = (int32_t)(cpu->segment_offsets[FS] >> 32);
		break;
	case (int32_t)MSR_GS_BASE:
		low = (int32_t)cpu->segment_offsets[GS];
		high = (int32_t)(cpu->segment_offsets[GS] >> 32);
		break;
	case (int32_t)MSR_KERNEL_GS_BASE:
		low = (int32_t)cpu->kernel_gs_base;
		high = (int32_t)(cpu->kernel_gs_base >> 32);
		break;
	case (int32_t)MSR_STAR: low = (int32_t)cpu->star; high = (int32_t)(cpu->star >> 32); break;
	case (int32_t)MSR_LSTAR: low = (int32_t)cpu->lstar; high = (int32_t)(cpu->lstar >> 32); break;
	case (int32_t)MSR_FMASK: low = (int32_t)cpu->fmask; high = (int32_t)(cpu->fmask >> 32); break;
	case IA32_SYSENTER_CS: low = cpu->sysenter_cs; break;
	case IA32_SYSENTER_EIP: low = cpu->sysenter_eip; break;
	case IA32_SYSENTER_ESP: low = cpu->sysenter_esp; break;
	case IA32_TIME_STAMP_COUNTER: {
		uint64_t tsc = read_tsc();
		low = (int32_t)tsc;
		high = (int32_t)(tsc >> 32);
	}
		break;
	case IA32_FEAT_CTL: break; /* linux 5.x */
	case MSR_TEST_CTRL: break; /* linux 5.x */
	case IA32_PLATFORM_ID: break;
	case IA32_APIC_BASE: {
		if (cpu->acpi_enabled) {
			low = (int32_t)APIC_MEM_ADDRESS;
			if (cpu->apic_enabled) {
				low |= IA32_APIC_BASE_EN;
			}
		}
	}
		break;
	case IA32_BIOS_SIGN_ID: break;
	case MSR_PLATFORM_INFO: low = 1 << 8; break;
	case MISC_FEATURE_ENABLES: break;
	case IA32_MISC_ENABLE: low = 1 << 0; break; /* fast string */
	case IA32_RTIT_CTL: break;                  /* linux4 */
	case MSR_SMI_COUNT: break;
	case IA32_MCG_CAP: break;                   /* netbsd */
	case IA32_PERFEVTSEL0:      /* fallthrough */
	case IA32_PERFEVTSEL1: break;               /* linux/9legacy */
	case IA32_PMC0:      /* fallthrough */
	case IA32_PMC1: break;                      /* linux */
	case IA32_PAT: {
		low = (int32_t)cpu->pat;
		high = (int32_t)(cpu->pat >> 32);
	}
		break;
	case MSR_PKG_C2_RESIDENCY: break;
	case IA32_SPEC_CTRL: break;      /* linux 5.19 */
	case IA32_TSX_CTRL: break;       /* linux 5.19 */
	case MSR_TSX_FORCE_ABORT: break; /* linux 5.19 */
	case IA32_MCU_OPT_CTRL: break;   /* linux 5.19 */
	case MSR_AMD64_LS_CFG: break;    /* linux 5.19 */
	case MSR_AMD64_DE_CFG: break;    /* linux 6.1 */
	default:
		/* upstream: dbg_log!("Unknown msr") + dbg_assert!(false) -- dropped, as in
		 * instr_0F30 above */
		break;
	}

	write_reg32(EAX, low);
	write_reg32(EDX, high);
}

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:1364-1421 -- 0F 34, 0F 35: sysenter, sysexit
 *
 * Both load CS/SS straight from sysenter_cs (the fast system-call path's flat segments),
 * which is why they need update_cs_size/cpl_changed (exceptions.c) rather than switch_seg.
 * ------------------------------------------------------------------ */
void instr_0F34(void)
{
	/* sysenter */
	int32_t seg = cpu->sysenter_cs & 0xFFFC;
	if (!cpu->protected_mode || seg == 0) {
		trigger_gp(0);
		return;
	}
	else {
		cpu->flags &= ~FLAG_VM & ~FLAG_INTERRUPT;
		/* S3: the widened store, projected through the 32-bit MSR (upstream's value is an
		 * i32; a long-mode SYSENTER target is S11's) */
		set_instruction_pointer(cpu->sysenter_eip);
		write_reg32(ESP, cpu->sysenter_esp);
		cpu->sreg[CS] = (uint16_t)seg;
		cpu->segment_is_null[CS] = false;
		cpu->segment_limits[CS] = 0xFFFFFFFFu;
		cpu->segment_offsets[CS] = 0;
		cpu->segment_access_bytes[CS] = 0x80 | (0 << 5) | 0x10 | 0x08 | 0x02; /* P dpl0 S E RW */
		update_cs_size(true);
		cpu->cpl = 0;
		cpl_changed();
		cpu->sreg[SS] = (uint16_t)(seg + 8);
		cpu->segment_is_null[SS] = false;
		cpu->segment_limits[SS] = 0xFFFFFFFFu;
		cpu->segment_offsets[SS] = 0;
		cpu->segment_access_bytes[SS] = 0x80 | (0 << 5) | 0x10 | 0x02; /* P dpl0 S RW */
		cpu->stack_size_32 = true;
		update_state_flags();
		return;
	}
}

void instr_0F35(void)
{
	/* sysexit */
	int32_t seg = cpu->sysenter_cs & 0xFFFC;
	if (!cpu->protected_mode || cpu->cpl != 0 || seg == 0) {
		trigger_gp(0);
		return;
	}
	else {
		set_instruction_pointer(read_reg32(EDX));
		write_reg32(ESP, read_reg32(ECX));
		cpu->sreg[CS] = (uint16_t)((seg + 16) | 3); /* upstream: seg + 16 | 3, `+` binding tighter than `|` */
		cpu->segment_is_null[CS] = false;
		cpu->segment_limits[CS] = 0xFFFFFFFFu;
		cpu->segment_offsets[CS] = 0;
		cpu->segment_access_bytes[CS] = 0x80 | (3 << 5) | 0x10 | 0x08 | 0x02; /* P dpl3 S E RW */
		update_cs_size(true);
		cpu->cpl = 3;
		cpl_changed();
		cpu->sreg[SS] = (uint16_t)((seg + 24) | 3);
		cpu->segment_is_null[SS] = false;
		cpu->segment_limits[SS] = 0xFFFFFFFFu;
		cpu->segment_offsets[SS] = 0;
		cpu->segment_access_bytes[SS] = 0x80 | (3 << 5) | 0x10 | 0x02; /* P dpl3 S RW */
		cpu->stack_size_32 = true;
		update_state_flags();
		return;
	}
}

/* ------------------------------------------------------------------ *
 * instructions_0f.rs:3996-4018 -- 0F C7 /6: rdrand
 *
 * CORRECTED IN M2-2 (HANDOFF=v86c), and the correction is the port's, not upstream's: this
 * pair used to be `trigger_ud()` with the note that upstream "defines only the MEMORY form".
 * MEASURED against the source, upstream is the other way round --
 *
 *     instructions_0f.rs:3996-4004  instr16_0FC7_6_reg = { let rand = js::get_rand_int();
 *                                                          write_reg16(r, rand); *flags =
 *                                                          (*flags & !FLAGS_ALL) | 1;
 *                                                          *flags_changed = 0; }
 *     instructions_0f.rs:4006-4013  instr32_0FC7_6_reg = the same for 32 bits
 *     instructions_0f.rs:4016-4018  instr16_0FC7_6_mem / instr32_0FC7_6_mem = trigger_ud()
 *
 * and gen/x86_table.js:462 records exactly that (`mem_ud: 1` on the g=6 entry, whose REG
 * form is rdrand).  The memory form below is therefore right and the register form was a
 * transcription error.  IT IS A REAL BLOCKER, not a tidiness fix: an i386 Linux kernel's
 * random_init_early executes `rdrand ecx` when CPUID advertises RDRAND (this port's leaf 1
 * ECX bit 30 does, instructions_0f.c:1132), and with the old body the guest took an
 * invalid-opcode oops in the first seconds of its boot and never reached its console.  See
 * results/m2-2-boot.md.
 *
 * THE RANDOM VALUE IS NOT PART OF ANY CONTRACT: upstream's js::get_rand_int is the
 * platform CSPRNG (MEASURED in the pinned build, libv86.mjs: `ca=function(){crypto
 * .getRandomValues(a)...}` / `ca=function(){return a.randomBytes(4).readInt32LE(0)}`), so
 * upstream is non-deterministic here too.  Until unit M2-5-DET this port read the same
 * kind of source -- /dev/urandom -- and fell back to a seeded generator if it could not be
 * opened.
 *
 * M2-5-DET (HANDOFF=v86det) REMOVED THE HOST SOURCE: RDRAND is now ALWAYS the seeded
 * generator below, with a FIXED seed, and never /dev/urandom.  The reason is the one the
 * fallback's own comment already gave -- a machine whose whole point is reproducibility
 * cannot hand a guest host entropy -- and the requirement is stronger than "usually the
 * same": §7.3's gate boots the same image and cmdline twice and compares the transcripts
 * BYTE for BYTE (tests/gate-m2-5-determinism.sh), and RDRAND's value is observable to the
 * guest (an i386 Linux kernel executes `rdrand ecx` in random_init_early and mixes the
 * result into its CRNG).  A FIXED seed rather than a per-run one, because a per-run seed
 * is host entropy again by another route.  The value is a pure function of the machine's
 * own execution: the same sequence, in the same order, on every boot.
 *
 * The corpus has no rdrand test (tests/nasm_skip_c.txt, tests/nasm_skip.txt) and nothing
 * in the M0 contract or the M1b fixtures reads the register back; the boot gate's serial
 * output is identical with either value (MEASURED: the reference's own output is stable
 * across runs).  This is a port-level deviation from upstream, taken for a port-level
 * requirement, and it is declared rather than hidden.
 * ------------------------------------------------------------------ */
static int32_t rdrand_value(void)
{
	static uint32_t state = 0x2545F491u; /* the fixed seed: RDRAND is reproducible */

	state = state * 1103515245u + 12345u;
	return (int32_t)state;
}

void instr16_0FC7_6_reg(int32_t r)
{
	write_reg16(r, rdrand_value());
	/* *flags &= !FLAGS_ALL; *flags |= 1; *flags_changed = 0;  (Rust's `!` is bitwise NOT) */
	cpu->flags = (cpu->flags & ~FLAGS_ALL) | FLAG_CARRY;
	cpu->flags_changed = 0;
}
void instr32_0FC7_6_reg(int32_t r)
{
	write_reg32(r, rdrand_value());
	cpu->flags = (cpu->flags & ~FLAGS_ALL) | FLAG_CARRY;
	cpu->flags_changed = 0;
}

/* 0F C7 /6, MEMORY form, with REX.W: #UD, exactly as the 32-bit arm and the reference's
 * table (`mem_ud: 1` on the g=6 entry, instructions_0f.c's note above).  The body exists
 * because the emitter's stop path leaves the resolved address unused and the build treats
 * that as an error -- the seat has to be filled for the ARM to be complete, not for the
 * behaviour to change. */
void instr64_0FC7_6_mem(int64_t addr)
{
	(void)addr;
	trigger_ud();
}

/* 0F C7 /6 with REX.W: RDRAND r64.  The value is the SAME deterministic generator as the
 * 32-bit arm (the note above explains why the port removed the host source), composed from
 * two 32-bit draws LOW FIRST -- upstream has no 64-bit draw to copy (`js::get_rand_int()`
 * is 32 bits), so the composition is the port's own and is stated here rather than implied.
 * MEASURED as the amd64 guest's stop (results/x86-64-rep.md): the decompressor's KASLR code
 * executes `rdrand rax` at 0x4F3DD4C and the 64-bit arm did not exist. */
void instr64_0FC7_6_reg(int32_t r)
{
	uint64_t low = (uint32_t)rdrand_value();
	uint64_t high = (uint32_t)rdrand_value();
	write_reg64(r, low | (high << 32));
	cpu->flags = (cpu->flags & ~FLAGS_ALL) | FLAG_CARRY;
	cpu->flags_changed = 0;
}
