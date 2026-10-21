/*
 * interpreter_gen.c -- GENERATED FILE, DO NOT EDIT.
 *
 * Emitted by gen/interpreter_c.js from v86/gen/x86_table.js (imported unchanged).
 * The whole file is a function of those two inputs: re-running the generator on the
 * same tree reproduces it byte for byte.
 *
 * READ THIS AND src/cpu/instructions.c's header TOGETHER.  The engine of this port is
 * the reference's own instruction table: the emitter (like v86/gen/generate_interpreter.js)
 * is a mechanical projection of x86_table.js, so an opcode's semantics cannot be
 * re-decided by hand -- only its *implementation* can be missing.
 *
 * A case whose instruction has no definition in the port's src/cpu/ sources yet calls
 * unimplemented_instruction(...) and returns: the opcode is still classified (never
 * mis-decoded), and hitting it stops the machine with the missing symbol named.
 *
 * THE FALLTHROUGH ARMS DO NOT ASSERT, deliberately.  Where a prefix group's arms are
 * guarded by the PREFIX_* bits of cpu->prefixes, the final 'else' -- reached when the
 * guest's prefix combination is not one this opcode defines (MEASURED: F2 0F 6F, F3 0F 71,
 * 66 0F 52) -- runs the UNPREFIXED body with no check.  Upstream emits a dbg_assert! there,
 * but dbg_assert! is debug_assert! and is COMPILED OUT of the pinned release reference,
 * which therefore EXECUTES that body; the C port matches the reference rather than
 * upstream's debug build, because the condition is a property of the guest's bytes and
 * aborting on them would be a divergence, not one of the port's stops.  An unported body
 * reached this way still stops by name, exactly as the unprefixed encoding does.
 * See gen/interpreter_c.js, C-side difference 3.
 *
 * 2231 call sites, 50 of them unimplemented at the time this
 * file was generated (2185 instr* symbols defined in the port's sources).
 */

#include "cpu.h"
#include "cpu_state.h"
#include "instructions.h"
#include "instructions_0f.h"
#include "instructions_64.h"
#include "misc_instr.h"
#include "modrm.h"
#include "paging.h"

/* cpu.rs:3131 -- the base-table switch.  `opcode` is
 * `read_imm8() | (is_osize_32() << 8)` (cpu.rs:3367, cpu.rs:3168): the low byte is the
 * opcode, bit 8 the operand size, which is exactly the generator's 0xNN / 0x1NN split. */
void run_instruction(int32_t opcode)
{
	switch(opcode)
	{
	case 0x00:
	case 0x100:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_00_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_00_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x200:
	{
		unimplemented_instruction("long mode: 0x00 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x01:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_01_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_01_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x101:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_01_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_01_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x201:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_01_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_01_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x02:
	case 0x102:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_02_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_02_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x202:
	{
		unimplemented_instruction("long mode: 0x02 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x03:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_03_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_03_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x103:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_03_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_03_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x203:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_03_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_03_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x04:
	case 0x104:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_04(imm);
	}
	break;
	case 0x204:
	{
		unimplemented_instruction("long mode: 0x04 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x05:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_05(imm);
	}
	break;
	case 0x105:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_05(imm);
	}
	break;
	case 0x205:
	{
		unimplemented_instruction("long mode: 0x05 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x06:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x06 (push es) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_06();
	}
	break;
	case 0x106:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x06 (push es) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_06();
	}
	break;
	case 0x206:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x06 (push es) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x07:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x07 (pop es) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_07();
		after_block_boundary();
	}
	break;
	case 0x107:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x07 (pop es) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_07();
		after_block_boundary();
	}
	break;
	case 0x207:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x07 (pop es) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x08:
	case 0x108:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_08_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_08_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x208:
	{
		unimplemented_instruction("long mode: 0x08 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x09:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_09_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_09_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x109:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_09_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_09_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x209:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_09_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_09_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x0A:
	case 0x10A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x20A:
	{
		unimplemented_instruction("long mode: 0x0A has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x10B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x20B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x0C:
	case 0x10C:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_0C(imm);
	}
	break;
	case 0x20C:
	{
		unimplemented_instruction("long mode: 0x0C has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0D:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0D(imm);
	}
	break;
	case 0x10D:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0D(imm);
	}
	break;
	case 0x20D:
	{
		unimplemented_instruction("long mode: 0x0D has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x0E (push cs) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_0E();
	}
	break;
	case 0x10E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x0E (push cs) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_0E();
	}
	break;
	case 0x20E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x0E (push cs) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x0F:
	{
		instr16_0F();
	}
	break;
	case 0x10F:
	{
		instr32_0F();
	}
	break;
	case 0x20F:
	{
		instr64_0F();
	}
	break;
	case 0x10:
	case 0x110:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_10_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_10_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x210:
	{
		unimplemented_instruction("long mode: 0x10 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x11:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_11_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_11_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x111:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_11_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_11_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x211:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_11_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_11_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x12:
	case 0x112:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_12_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_12_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x212:
	{
		unimplemented_instruction("long mode: 0x12 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x13:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_13_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_13_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x113:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_13_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_13_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x213:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_13_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_13_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x14:
	case 0x114:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_14(imm);
	}
	break;
	case 0x214:
	{
		unimplemented_instruction("long mode: 0x14 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x15:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_15(imm);
	}
	break;
	case 0x115:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_15(imm);
	}
	break;
	case 0x215:
	{
		unimplemented_instruction("long mode: 0x15 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x16:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x16 (push ss) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_16();
	}
	break;
	case 0x116:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x16 (push ss) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_16();
	}
	break;
	case 0x216:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x16 (push ss) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x17:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x17 (pop ss) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_17();
		after_block_boundary();
	}
	break;
	case 0x117:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x17 (pop ss) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_17();
		after_block_boundary();
	}
	break;
	case 0x217:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x17 (pop ss) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x18:
	case 0x118:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_18_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_18_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x218:
	{
		unimplemented_instruction("long mode: 0x18 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x19:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_19_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_19_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x119:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_19_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_19_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x219:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_19_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_19_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1A:
	case 0x11A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_1A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_1A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x21A:
	{
		unimplemented_instruction("long mode: 0x1A has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_1B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_1B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x11B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_1B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_1B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x21B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_1B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_1B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1C:
	case 0x11C:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_1C(imm);
	}
	break;
	case 0x21C:
	{
		unimplemented_instruction("long mode: 0x1C has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1D:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_1D(imm);
	}
	break;
	case 0x11D:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_1D(imm);
	}
	break;
	case 0x21D:
	{
		unimplemented_instruction("long mode: 0x1D has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x1E (push ds) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_1E();
	}
	break;
	case 0x11E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x1E (push ds) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_1E();
	}
	break;
	case 0x21E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x1E (push ds) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x1F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x1F (pop ds) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_1F();
		after_block_boundary();
	}
	break;
	case 0x11F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x1F (pop ds) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_1F();
		after_block_boundary();
	}
	break;
	case 0x21F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x1F (pop ds) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x20:
	case 0x120:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_20_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_20_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x220:
	{
		unimplemented_instruction("long mode: 0x20 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x21:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_21_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_21_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x121:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_21_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_21_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x221:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_21_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_21_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x22:
	case 0x122:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_22_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_22_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x222:
	{
		unimplemented_instruction("long mode: 0x22 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x23:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_23_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_23_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x123:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_23_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_23_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x223:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_23_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_23_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x24:
	case 0x124:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_24(imm);
	}
	break;
	case 0x224:
	{
		unimplemented_instruction("long mode: 0x24 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x25:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_25(imm);
	}
	break;
	case 0x125:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_25(imm);
	}
	break;
	case 0x225:
	{
		unimplemented_instruction("long mode: 0x25 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x26:
	case 0x126:
	{
		instr_26();
	}
	break;
	case 0x226:
	{
		unimplemented_instruction("long mode: 0x26 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x27:
	case 0x127:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x27 (daa) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr_27();
	}
	break;
	case 0x227:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x27 (daa) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x28:
	case 0x128:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_28_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_28_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x228:
	{
		unimplemented_instruction("long mode: 0x28 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x29:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_29_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_29_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x129:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_29_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_29_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x229:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_29_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_29_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2A:
	case 0x12A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_2A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_2A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x22A:
	{
		unimplemented_instruction("long mode: 0x2A has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_2B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_2B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x12B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_2B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_2B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x22B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_2B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_2B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2C:
	case 0x12C:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_2C(imm);
	}
	break;
	case 0x22C:
	{
		unimplemented_instruction("long mode: 0x2C has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2D:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_2D(imm);
	}
	break;
	case 0x12D:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_2D(imm);
	}
	break;
	case 0x22D:
	{
		unimplemented_instruction("long mode: 0x2D has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2E:
	case 0x12E:
	{
		instr_2E();
	}
	break;
	case 0x22E:
	{
		unimplemented_instruction("long mode: 0x2E has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2F:
	case 0x12F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x2F (das) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr_2F();
	}
	break;
	case 0x22F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x2F (das) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x30:
	case 0x130:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_30_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_30_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x230:
	{
		unimplemented_instruction("long mode: 0x30 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x31:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_31_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_31_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x131:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_31_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_31_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x231:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_31_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_31_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x32:
	case 0x132:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_32_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_32_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x232:
	{
		unimplemented_instruction("long mode: 0x32 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x33:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_33_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_33_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x133:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_33_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_33_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x233:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_33_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_33_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x34:
	case 0x134:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_34(imm);
	}
	break;
	case 0x234:
	{
		unimplemented_instruction("long mode: 0x34 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x35:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_35(imm);
	}
	break;
	case 0x135:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_35(imm);
	}
	break;
	case 0x235:
	{
		unimplemented_instruction("long mode: 0x35 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x36:
	case 0x136:
	{
		instr_36();
	}
	break;
	case 0x236:
	{
		unimplemented_instruction("long mode: 0x36 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x37:
	case 0x137:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x37 (aaa) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr_37();
	}
	break;
	case 0x237:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x37 (aaa) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x38:
	case 0x138:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_38_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_38_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x238:
	{
		unimplemented_instruction("long mode: 0x38 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x39:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_39_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_39_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x139:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_39_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_39_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x239:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_39_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_39_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x3A:
	case 0x13A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_3A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_3A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x23A:
	{
		unimplemented_instruction("long mode: 0x3A has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_3B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_3B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x13B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_3B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_3B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x23B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_3B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_3B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x3C:
	case 0x13C:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_3C(imm);
	}
	break;
	case 0x23C:
	{
		unimplemented_instruction("long mode: 0x3C has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3D:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_3D(imm);
	}
	break;
	case 0x13D:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_3D(imm);
	}
	break;
	case 0x23D:
	{
		unimplemented_instruction("long mode: 0x3D has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3E:
	case 0x13E:
	{
		instr_3E();
	}
	break;
	case 0x23E:
	{
		unimplemented_instruction("long mode: 0x3E has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3F:
	case 0x13F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x3F (aas) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr_3F();
	}
	break;
	case 0x23F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x3F (aas) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x40:
	{
		instr16_40();
	}
	break;
	case 0x140:
	{
		instr32_40();
	}
	break;
	case 0x240:
	{
		long_mode_rex_prefix(0x40);
		return;
	}

	case 0x41:
	{
		instr16_41();
	}
	break;
	case 0x141:
	{
		instr32_41();
	}
	break;
	case 0x241:
	{
		long_mode_rex_prefix(0x41);
		return;
	}

	case 0x42:
	{
		instr16_42();
	}
	break;
	case 0x142:
	{
		instr32_42();
	}
	break;
	case 0x242:
	{
		long_mode_rex_prefix(0x42);
		return;
	}

	case 0x43:
	{
		instr16_43();
	}
	break;
	case 0x143:
	{
		instr32_43();
	}
	break;
	case 0x243:
	{
		long_mode_rex_prefix(0x43);
		return;
	}

	case 0x44:
	{
		instr16_44();
	}
	break;
	case 0x144:
	{
		instr32_44();
	}
	break;
	case 0x244:
	{
		long_mode_rex_prefix(0x44);
		return;
	}

	case 0x45:
	{
		instr16_45();
	}
	break;
	case 0x145:
	{
		instr32_45();
	}
	break;
	case 0x245:
	{
		long_mode_rex_prefix(0x45);
		return;
	}

	case 0x46:
	{
		instr16_46();
	}
	break;
	case 0x146:
	{
		instr32_46();
	}
	break;
	case 0x246:
	{
		long_mode_rex_prefix(0x46);
		return;
	}

	case 0x47:
	{
		instr16_47();
	}
	break;
	case 0x147:
	{
		instr32_47();
	}
	break;
	case 0x247:
	{
		long_mode_rex_prefix(0x47);
		return;
	}

	case 0x48:
	{
		instr16_48();
	}
	break;
	case 0x148:
	{
		instr32_48();
	}
	break;
	case 0x248:
	{
		long_mode_rex_prefix(0x48);
		return;
	}

	case 0x49:
	{
		instr16_49();
	}
	break;
	case 0x149:
	{
		instr32_49();
	}
	break;
	case 0x249:
	{
		long_mode_rex_prefix(0x49);
		return;
	}

	case 0x4A:
	{
		instr16_4A();
	}
	break;
	case 0x14A:
	{
		instr32_4A();
	}
	break;
	case 0x24A:
	{
		long_mode_rex_prefix(0x4A);
		return;
	}

	case 0x4B:
	{
		instr16_4B();
	}
	break;
	case 0x14B:
	{
		instr32_4B();
	}
	break;
	case 0x24B:
	{
		long_mode_rex_prefix(0x4B);
		return;
	}

	case 0x4C:
	{
		instr16_4C();
	}
	break;
	case 0x14C:
	{
		instr32_4C();
	}
	break;
	case 0x24C:
	{
		long_mode_rex_prefix(0x4C);
		return;
	}

	case 0x4D:
	{
		instr16_4D();
	}
	break;
	case 0x14D:
	{
		instr32_4D();
	}
	break;
	case 0x24D:
	{
		long_mode_rex_prefix(0x4D);
		return;
	}

	case 0x4E:
	{
		instr16_4E();
	}
	break;
	case 0x14E:
	{
		instr32_4E();
	}
	break;
	case 0x24E:
	{
		long_mode_rex_prefix(0x4E);
		return;
	}

	case 0x4F:
	{
		instr16_4F();
	}
	break;
	case 0x14F:
	{
		instr32_4F();
	}
	break;
	case 0x24F:
	{
		long_mode_rex_prefix(0x4F);
		return;
	}

	case 0x50:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_50();
	}
	break;
	case 0x150:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_50();
	}
	break;
	case 0x250:
	{
		instr64_50();
	}
	break;
	case 0x51:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_51();
	}
	break;
	case 0x151:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_51();
	}
	break;
	case 0x251:
	{
		instr64_51();
	}
	break;
	case 0x52:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_52();
	}
	break;
	case 0x152:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_52();
	}
	break;
	case 0x252:
	{
		instr64_52();
	}
	break;
	case 0x53:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_53();
	}
	break;
	case 0x153:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_53();
	}
	break;
	case 0x253:
	{
		instr64_53();
	}
	break;
	case 0x54:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_54();
	}
	break;
	case 0x154:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_54();
	}
	break;
	case 0x254:
	{
		instr64_54();
	}
	break;
	case 0x55:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_55();
	}
	break;
	case 0x155:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_55();
	}
	break;
	case 0x255:
	{
		instr64_55();
	}
	break;
	case 0x56:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_56();
	}
	break;
	case 0x156:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_56();
	}
	break;
	case 0x256:
	{
		instr64_56();
	}
	break;
	case 0x57:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_57();
	}
	break;
	case 0x157:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_57();
	}
	break;
	case 0x257:
	{
		instr64_57();
	}
	break;
	case 0x58:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_58();
	}
	break;
	case 0x158:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_58();
	}
	break;
	case 0x258:
	{
		instr64_58();
	}
	break;
	case 0x59:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_59();
	}
	break;
	case 0x159:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_59();
	}
	break;
	case 0x259:
	{
		instr64_59();
	}
	break;
	case 0x5A:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_5A();
	}
	break;
	case 0x15A:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_5A();
	}
	break;
	case 0x25A:
	{
		instr64_5A();
	}
	break;
	case 0x5B:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_5B();
	}
	break;
	case 0x15B:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_5B();
	}
	break;
	case 0x25B:
	{
		instr64_5B();
	}
	break;
	case 0x5C:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_5C();
	}
	break;
	case 0x15C:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_5C();
	}
	break;
	case 0x25C:
	{
		instr64_5C();
	}
	break;
	case 0x5D:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_5D();
	}
	break;
	case 0x15D:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_5D();
	}
	break;
	case 0x25D:
	{
		instr64_5D();
	}
	break;
	case 0x5E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_5E();
	}
	break;
	case 0x15E:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_5E();
	}
	break;
	case 0x25E:
	{
		instr64_5E();
	}
	break;
	case 0x5F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_5F();
	}
	break;
	case 0x15F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_5F();
	}
	break;
	case 0x25F:
	{
		instr64_5F();
	}
	break;
	case 0x60:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x60 (pusha) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_60();
		after_block_boundary();
	}
	break;
	case 0x160:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x60 (pusha) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_60();
		after_block_boundary();
	}
	break;
	case 0x260:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x60 (pusha) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x61:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x61 (popa) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr16_61();
		after_block_boundary();
	}
	break;
	case 0x161:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x61 (popa) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr32_61();
		after_block_boundary();
	}
	break;
	case 0x261:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x61 (popa) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x62:
	case 0x162:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x62 (bound) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_62_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_62_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x262:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x62 (bound) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x63:
	case 0x163:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x63 without REX.W is MOVSXD at 16 or 32 bits, which is not implemented (ARPL does not exist in 64-bit mode)");
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_63_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_63_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x263:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_63_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_63_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x64:
	case 0x164:
	{
		instr_64();
	}
	break;
	case 0x264:
	{
		unimplemented_instruction("long mode: 0x64 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x65:
	case 0x165:
	{
		instr_65();
	}
	break;
	case 0x265:
	{
		unimplemented_instruction("long mode: 0x65 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x66:
	case 0x166:
	{
		instr_66();
	}
	break;
	case 0x266:
	{
		unimplemented_instruction("long mode: 0x66 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x67:
	case 0x167:
	{
		instr_67();
	}
	break;
	case 0x267:
	{
		unimplemented_instruction("long mode: 0x67 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x68:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_68(imm);
	}
	break;
	case 0x168:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_68(imm);
	}
	break;
	case 0x268:
	{
		int64_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr64_68(imm);
	}
	break;
	case 0x69:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm16();
			if (cpu->pagefault) return;
			instr16_69_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm16();
			if (cpu->pagefault) return;
			instr16_69_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x169:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm32s();
			if (cpu->pagefault) return;
			instr32_69_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm32s();
			if (cpu->pagefault) return;
			instr32_69_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x269:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			int64_t imm = read_imm32s();
			if (cpu->pagefault) return;
			instr64_69_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int64_t imm = read_imm32s();
			if (cpu->pagefault) return;
			instr64_69_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x6A:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_6A(imm);
	}
	break;
	case 0x16A:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_6A(imm);
	}
	break;
	case 0x26A:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_6A(imm);
	}
	break;
	case 0x6B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm8s();
			if (cpu->pagefault) return;
			instr16_6B_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm8s();
			if (cpu->pagefault) return;
			instr16_6B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x16B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm8s();
			if (cpu->pagefault) return;
			instr32_6B_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm8s();
			if (cpu->pagefault) return;
			instr32_6B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x26B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			int64_t imm = read_imm8s();
			if (cpu->pagefault) return;
			instr64_6B_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int64_t imm = read_imm8s();
			if (cpu->pagefault) return;
			instr64_6B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x6C:
	case 0x16C:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr_F26C();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr_F36C();
			after_block_boundary();
		}
		else {
			instr_6C();
			after_block_boundary();
		}
	}
	break;
	case 0x26C:
	{
		unimplemented_instruction("long mode: 0x6C has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6D:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr16_F26D();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr16_F36D();
			after_block_boundary();
		}
		else {
			instr16_6D();
			after_block_boundary();
		}
	}
	break;
	case 0x16D:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr32_F26D();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr32_F36D();
			after_block_boundary();
		}
		else {
			instr32_6D();
			after_block_boundary();
		}
	}
	break;
	case 0x26D:
	{
		unimplemented_instruction("long mode: 0x6D has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6E:
	case 0x16E:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr_F26E();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr_F36E();
			after_block_boundary();
		}
		else {
			instr_6E();
			after_block_boundary();
		}
	}
	break;
	case 0x26E:
	{
		unimplemented_instruction("long mode: 0x6E has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6F:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr16_F26F();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr16_F36F();
			after_block_boundary();
		}
		else {
			instr16_6F();
			after_block_boundary();
		}
	}
	break;
	case 0x16F:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr32_F26F();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr32_F36F();
			after_block_boundary();
		}
		else {
			instr32_6F();
			after_block_boundary();
		}
	}
	break;
	case 0x26F:
	{
		unimplemented_instruction("long mode: 0x6F has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x70:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_70(imm);
	}
	break;
	case 0x170:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_70(imm);
	}
	break;
	case 0x270:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_70(imm);
	}
	break;
	case 0x71:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_71(imm);
	}
	break;
	case 0x171:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_71(imm);
	}
	break;
	case 0x271:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_71(imm);
	}
	break;
	case 0x72:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_72(imm);
	}
	break;
	case 0x172:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_72(imm);
	}
	break;
	case 0x272:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_72(imm);
	}
	break;
	case 0x73:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_73(imm);
	}
	break;
	case 0x173:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_73(imm);
	}
	break;
	case 0x273:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_73(imm);
	}
	break;
	case 0x74:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_74(imm);
	}
	break;
	case 0x174:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_74(imm);
	}
	break;
	case 0x274:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_74(imm);
	}
	break;
	case 0x75:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_75(imm);
	}
	break;
	case 0x175:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_75(imm);
	}
	break;
	case 0x275:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_75(imm);
	}
	break;
	case 0x76:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_76(imm);
	}
	break;
	case 0x176:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_76(imm);
	}
	break;
	case 0x276:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_76(imm);
	}
	break;
	case 0x77:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_77(imm);
	}
	break;
	case 0x177:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_77(imm);
	}
	break;
	case 0x277:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_77(imm);
	}
	break;
	case 0x78:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_78(imm);
	}
	break;
	case 0x178:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_78(imm);
	}
	break;
	case 0x278:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_78(imm);
	}
	break;
	case 0x79:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_79(imm);
	}
	break;
	case 0x179:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_79(imm);
	}
	break;
	case 0x279:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_79(imm);
	}
	break;
	case 0x7A:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_7A(imm);
	}
	break;
	case 0x17A:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_7A(imm);
	}
	break;
	case 0x27A:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_7A(imm);
	}
	break;
	case 0x7B:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_7B(imm);
	}
	break;
	case 0x17B:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_7B(imm);
	}
	break;
	case 0x27B:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_7B(imm);
	}
	break;
	case 0x7C:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_7C(imm);
	}
	break;
	case 0x17C:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_7C(imm);
	}
	break;
	case 0x27C:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_7C(imm);
	}
	break;
	case 0x7D:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_7D(imm);
	}
	break;
	case 0x17D:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_7D(imm);
	}
	break;
	case 0x27D:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_7D(imm);
	}
	break;
	case 0x7E:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_7E(imm);
	}
	break;
	case 0x17E:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_7E(imm);
	}
	break;
	case 0x27E:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_7E(imm);
	}
	break;
	case 0x7F:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_7F(imm);
	}
	break;
	case 0x17F:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_7F(imm);
	}
	break;
	case 0x27F:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_7F(imm);
	}
	break;
	case 0x80:
	case 0x180:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_80_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x280:
	{
		unimplemented_instruction("long mode: 0x80 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x81:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_81_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x181:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_81_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x281:
	{
		unimplemented_instruction("long mode: 0x81 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x82:
	case 0x182:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x82 is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_82_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x282:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x82 is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x83:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr16_83_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x183:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr32_83_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x283:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_0_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_1_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_2_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_3_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_4_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_5_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_6_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_7_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8s();
				if (cpu->pagefault) return;
				instr64_83_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x84:
	case 0x184:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_84_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_84_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x284:
	{
		unimplemented_instruction("long mode: 0x84 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x85:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_85_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_85_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x185:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_85_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_85_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x285:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_85_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_85_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x86:
	case 0x186:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_86_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_86_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x286:
	{
		unimplemented_instruction("long mode: 0x86 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x87:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_87_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_87_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x187:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_87_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_87_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x287:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_87_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_87_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x88:
	case 0x188:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_88_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_88_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x288:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_88_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_88_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x89:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_89_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_89_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x189:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_89_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_89_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x289:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_89_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_89_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x8A:
	case 0x18A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_8A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_8A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x28A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_8A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_8A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x8B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_8B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_8B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x18B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_8B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_8B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x28B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_8B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_8B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x8C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_8C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_8C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x18C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_8C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_8C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x28C:
	{
		unimplemented_instruction("long mode: 0x8C has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			instr16_8D_mem(modrm_byte, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_8D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x18D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			instr32_8D_mem(modrm_byte, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_8D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x28D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			instr64_8D_mem(modrm_byte, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_8D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x8E:
	case 0x18E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_8E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_8E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x28E:
	{
		unimplemented_instruction("long mode: 0x8E has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				instr16_8F_0_mem(modrm_byte);
			}
			else {
				instr16_8F_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x18F:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				instr32_8F_0_mem(modrm_byte);
			}
			else {
				instr32_8F_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x28F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				instr64_8F_0_mem(modrm_byte);
			}
			else {
				instr64_8F_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x90:
	case 0x190:
	{
		instr_90();
	}
	break;
	case 0x290:
	{
		unimplemented_instruction("long mode: 0x90 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x91:
	{
		instr16_91();
	}
	break;
	case 0x191:
	{
		instr32_91();
	}
	break;
	case 0x291:
	{
		unimplemented_instruction("long mode: 0x91 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x92:
	{
		instr16_92();
	}
	break;
	case 0x192:
	{
		instr32_92();
	}
	break;
	case 0x292:
	{
		unimplemented_instruction("long mode: 0x92 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x93:
	{
		instr16_93();
	}
	break;
	case 0x193:
	{
		instr32_93();
	}
	break;
	case 0x293:
	{
		unimplemented_instruction("long mode: 0x93 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x94:
	{
		instr16_94();
	}
	break;
	case 0x194:
	{
		instr32_94();
	}
	break;
	case 0x294:
	{
		unimplemented_instruction("long mode: 0x94 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x95:
	{
		instr16_95();
	}
	break;
	case 0x195:
	{
		instr32_95();
	}
	break;
	case 0x295:
	{
		unimplemented_instruction("long mode: 0x95 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x96:
	{
		instr16_96();
	}
	break;
	case 0x196:
	{
		instr32_96();
	}
	break;
	case 0x296:
	{
		unimplemented_instruction("long mode: 0x96 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x97:
	{
		instr16_97();
	}
	break;
	case 0x197:
	{
		instr32_97();
	}
	break;
	case 0x297:
	{
		unimplemented_instruction("long mode: 0x97 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x98:
	{
		instr16_98();
	}
	break;
	case 0x198:
	{
		instr32_98();
	}
	break;
	case 0x298:
	{
		unimplemented_instruction("long mode: 0x98 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x99:
	{
		instr16_99();
	}
	break;
	case 0x199:
	{
		instr32_99();
	}
	break;
	case 0x299:
	{
		unimplemented_instruction("long mode: 0x99 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9A:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x9A (call far ptr16:32) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		int32_t imm2 = read_imm16();
		if (cpu->pagefault) return;
		instr16_9A(imm, imm2);
		after_block_boundary();
	}
	break;
	case 0x19A:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x9A (call far ptr16:32) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		int32_t imm2 = read_imm16();
		if (cpu->pagefault) return;
		instr32_9A(imm, imm2);
		after_block_boundary();
	}
	break;
	case 0x29A:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0x9A (call far ptr16:32) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0x9B:
	case 0x19B:
	{
		instr_9B();
		after_block_boundary();
	}
	break;
	case 0x29B:
	{
		unimplemented_instruction("long mode: 0x9B has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9C:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_9C();
	}
	break;
	case 0x19C:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_9C();
	}
	break;
	case 0x29C:
	{
		instr64_9C();
	}
	break;
	case 0x9D:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_9D();
	}
	break;
	case 0x19D:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_9D();
	}
	break;
	case 0x29D:
	{
		instr64_9D();
	}
	break;
	case 0x9E:
	case 0x19E:
	{
		instr_9E();
	}
	break;
	case 0x29E:
	{
		unimplemented_instruction("long mode: 0x9E has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9F:
	case 0x19F:
	{
		instr_9F();
	}
	break;
	case 0x29F:
	{
		unimplemented_instruction("long mode: 0x9F has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA0:
	case 0x1A0:
	{
		int32_t imm = read_moffs();
		if (cpu->pagefault) return;
		instr_A0(imm);
	}
	break;
	case 0x2A0:
	{
		int64_t imm = read_moffs64();
		if (cpu->pagefault) return;
		instr64_A0(imm);
	}
	break;
	case 0xA1:
	{
		int32_t imm = read_moffs();
		if (cpu->pagefault) return;
		instr16_A1(imm);
	}
	break;
	case 0x1A1:
	{
		int32_t imm = read_moffs();
		if (cpu->pagefault) return;
		instr32_A1(imm);
	}
	break;
	case 0x2A1:
	{
		int64_t imm = read_moffs64();
		if (cpu->pagefault) return;
		instr64_A1(imm);
	}
	break;
	case 0xA2:
	case 0x1A2:
	{
		int32_t imm = read_moffs();
		if (cpu->pagefault) return;
		instr_A2(imm);
	}
	break;
	case 0x2A2:
	{
		int64_t imm = read_moffs64();
		if (cpu->pagefault) return;
		instr64_A2(imm);
	}
	break;
	case 0xA3:
	{
		int32_t imm = read_moffs();
		if (cpu->pagefault) return;
		instr16_A3(imm);
	}
	break;
	case 0x1A3:
	{
		int32_t imm = read_moffs();
		if (cpu->pagefault) return;
		instr32_A3(imm);
	}
	break;
	case 0x2A3:
	{
		int64_t imm = read_moffs64();
		if (cpu->pagefault) return;
		instr64_A3(imm);
	}
	break;
	case 0xA4:
	case 0x1A4:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr_F2A4();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr_F3A4();
			after_block_boundary();
		}
		else {
			instr_A4();
		}
	}
	break;
	case 0x2A4:
	{
		unimplemented_instruction("long mode: 0xA4 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA5:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (66 A5 would otherwise run MOVSQ -- the arms split on F2/F3 only)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr16_F2A5();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr16_F3A5();
			after_block_boundary();
		}
		else {
			instr16_A5();
		}
	}
	break;
	case 0x1A5:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (66 A5 would otherwise run MOVSQ -- the arms split on F2/F3 only)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr32_F2A5();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr32_F3A5();
			after_block_boundary();
		}
		else {
			instr32_A5();
		}
	}
	break;
	case 0x2A5:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr64_F2A5();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr64_F3A5();
			after_block_boundary();
		}
		else {
			instr64_A5();
		}
	}
	break;
	case 0xA6:
	case 0x1A6:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr_F2A6();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr_F3A6();
			after_block_boundary();
		}
		else {
			instr_A6();
			after_block_boundary();
		}
	}
	break;
	case 0x2A6:
	{
		unimplemented_instruction("long mode: 0xA6 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA7:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr16_F2A7();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr16_F3A7();
			after_block_boundary();
		}
		else {
			instr16_A7();
			after_block_boundary();
		}
	}
	break;
	case 0x1A7:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr32_F2A7();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr32_F3A7();
			after_block_boundary();
		}
		else {
			instr32_A7();
			after_block_boundary();
		}
	}
	break;
	case 0x2A7:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr64_F2A7();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr64_F3A7();
			after_block_boundary();
		}
		else {
			instr64_A7();
			after_block_boundary();
		}
	}
	break;
	case 0xA8:
	case 0x1A8:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_A8(imm);
	}
	break;
	case 0x2A8:
	{
		unimplemented_instruction("long mode: 0xA8 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA9:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_A9(imm);
	}
	break;
	case 0x1A9:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_A9(imm);
	}
	break;
	case 0x2A9:
	{
		unimplemented_instruction("long mode: 0xA9 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAA:
	case 0x1AA:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr_F2AA();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr_F3AA();
			after_block_boundary();
		}
		else {
			instr_AA();
		}
	}
	break;
	case 0x2AA:
	{
		unimplemented_instruction("long mode: 0xAA has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAB:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr16_F2AB();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr16_F3AB();
			after_block_boundary();
		}
		else {
			instr16_AB();
		}
	}
	break;
	case 0x1AB:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr32_F2AB();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr32_F3AB();
			after_block_boundary();
		}
		else {
			instr32_AB();
		}
	}
	break;
	case 0x2AB:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr64_F2AB();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr64_F3AB();
			after_block_boundary();
		}
		else {
			instr64_AB();
		}
	}
	break;
	case 0xAC:
	case 0x1AC:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr_F2AC();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr_F3AC();
			after_block_boundary();
		}
		else {
			instr_AC();
		}
	}
	break;
	case 0x2AC:
	{
		unimplemented_instruction("long mode: 0xAC has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAD:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr16_F2AD();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr16_F3AD();
			after_block_boundary();
		}
		else {
			instr16_AD();
		}
	}
	break;
	case 0x1AD:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr32_F2AD();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr32_F3AD();
			after_block_boundary();
		}
		else {
			instr32_AD();
		}
	}
	break;
	case 0x2AD:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr64_F2AD();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr64_F3AD();
			after_block_boundary();
		}
		else {
			instr64_AD();
		}
	}
	break;
	case 0xAE:
	case 0x1AE:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr_F2AE();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr_F3AE();
			after_block_boundary();
		}
		else {
			instr_AE();
		}
	}
	break;
	case 0x2AE:
	{
		unimplemented_instruction("long mode: 0xAE has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAF:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr16_F2AF();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr16_F3AF();
			after_block_boundary();
		}
		else {
			instr16_AF();
		}
	}
	break;
	case 0x1AF:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a string op in long mode is not implemented (see 0xA5)");
			return;
		}
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr32_F2AF();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr32_F3AF();
			after_block_boundary();
		}
		else {
			instr32_AF();
		}
	}
	break;
	case 0x2AF:
	{
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F2) != 0) {
			instr64_F2AF();
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			instr64_F3AF();
			after_block_boundary();
		}
		else {
			instr64_AF();
		}
	}
	break;
	case 0xB0:
	case 0x1B0:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B0(imm);
	}
	break;
	case 0x2B0:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B0(imm);
	}
	break;
	case 0xB1:
	case 0x1B1:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B1(imm);
	}
	break;
	case 0x2B1:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B1(imm);
	}
	break;
	case 0xB2:
	case 0x1B2:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B2(imm);
	}
	break;
	case 0x2B2:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B2(imm);
	}
	break;
	case 0xB3:
	case 0x1B3:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B3(imm);
	}
	break;
	case 0x2B3:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B3(imm);
	}
	break;
	case 0xB4:
	case 0x1B4:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B4(imm);
	}
	break;
	case 0x2B4:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B4(imm);
	}
	break;
	case 0xB5:
	case 0x1B5:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B5(imm);
	}
	break;
	case 0x2B5:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B5(imm);
	}
	break;
	case 0xB6:
	case 0x1B6:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B6(imm);
	}
	break;
	case 0x2B6:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B6(imm);
	}
	break;
	case 0xB7:
	case 0x1B7:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_B7(imm);
	}
	break;
	case 0x2B7:
	{
		int64_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr64_B7(imm);
	}
	break;
	case 0xB8:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_B8(imm);
	}
	break;
	case 0x1B8:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_B8(imm);
	}
	break;
	case 0x2B8:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_B8(imm);
	}
	break;
	case 0xB9:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_B9(imm);
	}
	break;
	case 0x1B9:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_B9(imm);
	}
	break;
	case 0x2B9:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_B9(imm);
	}
	break;
	case 0xBA:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_BA(imm);
	}
	break;
	case 0x1BA:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_BA(imm);
	}
	break;
	case 0x2BA:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_BA(imm);
	}
	break;
	case 0xBB:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_BB(imm);
	}
	break;
	case 0x1BB:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_BB(imm);
	}
	break;
	case 0x2BB:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_BB(imm);
	}
	break;
	case 0xBC:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_BC(imm);
	}
	break;
	case 0x1BC:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_BC(imm);
	}
	break;
	case 0x2BC:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_BC(imm);
	}
	break;
	case 0xBD:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_BD(imm);
	}
	break;
	case 0x1BD:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_BD(imm);
	}
	break;
	case 0x2BD:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_BD(imm);
	}
	break;
	case 0xBE:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_BE(imm);
	}
	break;
	case 0x1BE:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_BE(imm);
	}
	break;
	case 0x2BE:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_BE(imm);
	}
	break;
	case 0xBF:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_BF(imm);
	}
	break;
	case 0x1BF:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_BF(imm);
	}
	break;
	case 0x2BF:
	{
		int64_t imm = read_imm64();
		if (cpu->pagefault) return;
		instr64_BF(imm);
	}
	break;
	case 0xC0:
	case 0x1C0:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C0_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2C0:
	{
		unimplemented_instruction("long mode: 0xC0 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_C1_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1C1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_2_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_2_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_3_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_3_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_C1_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2C1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_0_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_1_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_C1_2_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_C1_2_reg");
				return;
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_C1_3_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_C1_3_reg");
				return;
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_4_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_5_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_C1_6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_C1_6_reg");
				return;
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_7_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C1_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xC2:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_C2(imm);
		after_block_boundary();
	}
	break;
	case 0x1C2:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr32_C2(imm);
		after_block_boundary();
	}
	break;
	case 0x2C2:
	{
		unimplemented_instruction("instructions::instr64_C2");
		return;
	}
	break;
	case 0xC3:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr16_C3();
		after_block_boundary();
	}
	break;
	case 0x1C3:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		instr32_C3();
		after_block_boundary();
	}
	break;
	case 0x2C3:
	{
		instr64_C3();
		after_block_boundary();
	}
	break;
	case 0xC4:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xC4 (les) is VEX-encoded in 64-bit mode, and VEX is not implemented");
			trigger_ud();
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_C4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_C4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x1C4:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xC4 (les) is VEX-encoded in 64-bit mode, and VEX is not implemented");
			trigger_ud();
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_C4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_C4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x2C4:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xC4 (les) is VEX-encoded in 64-bit mode, and VEX is not implemented");
			trigger_ud();
			return;
		}
	}
	break;
	case 0xC5:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xC5 (lds) is VEX-encoded in 64-bit mode, and VEX is not implemented");
			trigger_ud();
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_C5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_C5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x1C5:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xC5 (lds) is VEX-encoded in 64-bit mode, and VEX is not implemented");
			trigger_ud();
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_C5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_C5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x2C5:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xC5 (lds) is VEX-encoded in 64-bit mode, and VEX is not implemented");
			trigger_ud();
			return;
		}
	}
	break;
	case 0xC6:
	case 0x1C6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C6_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_C6_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2C6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C6_0_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_C6_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xC7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_C7_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_C7_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1C7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_C7_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_C7_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2C7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr64_C7_0_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr64_C7_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xC8:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		int32_t imm2 = read_imm8();
		if (cpu->pagefault) return;
		instr16_C8(imm, imm2);
		after_block_boundary();
	}
	break;
	case 0x1C8:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		int32_t imm2 = read_imm8();
		if (cpu->pagefault) return;
		instr32_C8(imm, imm2);
		after_block_boundary();
	}
	break;
	case 0x2C8:
	{
		unimplemented_instruction("long mode: 0xC8 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC9:
	{
		instr16_C9();
	}
	break;
	case 0x1C9:
	{
		instr32_C9();
	}
	break;
	case 0x2C9:
	{
		unimplemented_instruction("long mode: 0xC9 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xCA:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_CA(imm);
		after_block_boundary();
	}
	break;
	case 0x1CA:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr32_CA(imm);
		after_block_boundary();
	}
	break;
	case 0x2CA:
	{
		unimplemented_instruction("instructions::instr64_CA");
		return;
	}
	break;
	case 0xCB:
	{
		instr16_CB();
		after_block_boundary();
	}
	break;
	case 0x1CB:
	{
		instr32_CB();
		after_block_boundary();
	}
	break;
	case 0x2CB:
	{
		unimplemented_instruction("instructions::instr64_CB");
		return;
	}
	break;
	case 0xCC:
	case 0x1CC:
	{
		instr_CC();
		after_block_boundary();
	}
	break;
	case 0x2CC:
	{
		unimplemented_instruction("long mode: 0xCC has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xCD:
	case 0x1CD:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_CD(imm);
		after_block_boundary();
	}
	break;
	case 0x2CD:
	{
		unimplemented_instruction("long mode: 0xCD has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xCE:
	case 0x1CE:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xCE (into) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr_CE();
		after_block_boundary();
	}
	break;
	case 0x2CE:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xCE (into) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0xCF:
	{
		instr16_CF();
		after_block_boundary();
	}
	break;
	case 0x1CF:
	{
		instr32_CF();
		after_block_boundary();
	}
	break;
	case 0x2CF:
	{
		unimplemented_instruction("instructions::instr64_CF");
		return;
	}
	break;
	case 0xD0:
	case 0x1D0:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_0_mem(addr);
			}
			else {
				instr_D0_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_1_mem(addr);
			}
			else {
				instr_D0_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_2_mem(addr);
			}
			else {
				instr_D0_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_3_mem(addr);
			}
			else {
				instr_D0_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_4_mem(addr);
			}
			else {
				instr_D0_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_5_mem(addr);
			}
			else {
				instr_D0_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_6_mem(addr);
			}
			else {
				instr_D0_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D0_7_mem(addr);
			}
			else {
				instr_D0_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2D0:
	{
		unimplemented_instruction("long mode: 0xD0 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_0_mem(addr);
			}
			else {
				instr16_D1_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_1_mem(addr);
			}
			else {
				instr16_D1_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_2_mem(addr);
			}
			else {
				instr16_D1_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_3_mem(addr);
			}
			else {
				instr16_D1_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_4_mem(addr);
			}
			else {
				instr16_D1_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_5_mem(addr);
			}
			else {
				instr16_D1_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_6_mem(addr);
			}
			else {
				instr16_D1_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D1_7_mem(addr);
			}
			else {
				instr16_D1_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1D1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_0_mem(addr);
			}
			else {
				instr32_D1_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_1_mem(addr);
			}
			else {
				instr32_D1_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_2_mem(addr);
			}
			else {
				instr32_D1_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_3_mem(addr);
			}
			else {
				instr32_D1_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_4_mem(addr);
			}
			else {
				instr32_D1_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_5_mem(addr);
			}
			else {
				instr32_D1_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_6_mem(addr);
			}
			else {
				instr32_D1_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D1_7_mem(addr);
			}
			else {
				instr32_D1_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2D1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D1_0_mem(addr);
			}
			else {
				instr64_D1_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D1_1_mem(addr);
			}
			else {
				instr64_D1_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_D1_2_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_D1_2_reg");
				return;
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_D1_3_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_D1_3_reg");
				return;
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D1_4_mem(addr);
			}
			else {
				instr64_D1_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D1_5_mem(addr);
			}
			else {
				instr64_D1_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_D1_6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_D1_6_reg");
				return;
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D1_7_mem(addr);
			}
			else {
				instr64_D1_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xD2:
	case 0x1D2:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_0_mem(addr);
			}
			else {
				instr_D2_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_1_mem(addr);
			}
			else {
				instr_D2_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_2_mem(addr);
			}
			else {
				instr_D2_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_3_mem(addr);
			}
			else {
				instr_D2_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_4_mem(addr);
			}
			else {
				instr_D2_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_5_mem(addr);
			}
			else {
				instr_D2_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_6_mem(addr);
			}
			else {
				instr_D2_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D2_7_mem(addr);
			}
			else {
				instr_D2_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2D2:
	{
		unimplemented_instruction("long mode: 0xD2 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_0_mem(addr);
			}
			else {
				instr16_D3_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_1_mem(addr);
			}
			else {
				instr16_D3_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_2_mem(addr);
			}
			else {
				instr16_D3_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_3_mem(addr);
			}
			else {
				instr16_D3_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_4_mem(addr);
			}
			else {
				instr16_D3_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_5_mem(addr);
			}
			else {
				instr16_D3_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_6_mem(addr);
			}
			else {
				instr16_D3_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D3_7_mem(addr);
			}
			else {
				instr16_D3_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1D3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_0_mem(addr);
			}
			else {
				instr32_D3_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_1_mem(addr);
			}
			else {
				instr32_D3_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_2_mem(addr);
			}
			else {
				instr32_D3_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_3_mem(addr);
			}
			else {
				instr32_D3_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_4_mem(addr);
			}
			else {
				instr32_D3_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_5_mem(addr);
			}
			else {
				instr32_D3_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_6_mem(addr);
			}
			else {
				instr32_D3_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D3_7_mem(addr);
			}
			else {
				instr32_D3_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2D3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D3_0_mem(addr);
			}
			else {
				instr64_D3_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D3_1_mem(addr);
			}
			else {
				instr64_D3_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_D3_2_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_D3_2_reg");
				return;
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_D3_3_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_D3_3_reg");
				return;
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D3_4_mem(addr);
			}
			else {
				instr64_D3_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D3_5_mem(addr);
			}
			else {
				instr64_D3_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_D3_6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_D3_6_reg");
				return;
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_D3_7_mem(addr);
			}
			else {
				instr64_D3_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xD4:
	case 0x1D4:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xD4 (aam) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_D4(imm);
		after_block_boundary();
	}
	break;
	case 0x2D4:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xD4 (aam) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0xD5:
	case 0x1D5:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xD5 (aad) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_D5(imm);
	}
	break;
	case 0x2D5:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xD5 (aad) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0xD6:
	case 0x1D6:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xD6 (salc) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		instr_D6();
	}
	break;
	case 0x2D6:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xD6 (salc) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0xD7:
	case 0x1D7:
	{
		instr_D7();
	}
	break;
	case 0x2D7:
	{
		unimplemented_instruction("long mode: 0xD7 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD8:
	case 0x1D8:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_0_mem(addr);
			}
			else {
				instr_D8_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_1_mem(addr);
			}
			else {
				instr_D8_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_2_mem(addr);
			}
			else {
				instr_D8_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_3_mem(addr);
			}
			else {
				instr_D8_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_4_mem(addr);
			}
			else {
				instr_D8_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_5_mem(addr);
			}
			else {
				instr_D8_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_6_mem(addr);
			}
			else {
				instr_D8_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_D8_7_mem(addr);
			}
			else {
				instr_D8_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2D8:
	{
		unimplemented_instruction("long mode: 0xD8 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD9:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_0_mem(addr);
			}
			else {
				instr16_D9_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_1_mem(addr);
			}
			else {
				instr16_D9_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_2_mem(addr);
			}
			else {
				instr16_D9_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_3_mem(addr);
			}
			else {
				instr16_D9_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_4_mem(addr);
			}
			else {
				instr16_D9_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_5_mem(addr);
			}
			else {
				instr16_D9_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_6_mem(addr);
			}
			else {
				instr16_D9_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_D9_7_mem(addr);
			}
			else {
				instr16_D9_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1D9:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_0_mem(addr);
			}
			else {
				instr32_D9_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_1_mem(addr);
			}
			else {
				instr32_D9_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_2_mem(addr);
			}
			else {
				instr32_D9_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_3_mem(addr);
			}
			else {
				instr32_D9_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_4_mem(addr);
			}
			else {
				instr32_D9_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_5_mem(addr);
			}
			else {
				instr32_D9_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_6_mem(addr);
			}
			else {
				instr32_D9_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_D9_7_mem(addr);
			}
			else {
				instr32_D9_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2D9:
	{
		unimplemented_instruction("long mode: 0xD9 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDA:
	case 0x1DA:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_0_mem(addr);
			}
			else {
				instr_DA_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_1_mem(addr);
			}
			else {
				instr_DA_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_2_mem(addr);
			}
			else {
				instr_DA_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_3_mem(addr);
			}
			else {
				instr_DA_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_4_mem(addr);
			}
			else {
				instr_DA_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_5_mem(addr);
			}
			else {
				instr_DA_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_6_mem(addr);
			}
			else {
				instr_DA_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DA_7_mem(addr);
			}
			else {
				instr_DA_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2DA:
	{
		unimplemented_instruction("long mode: 0xDA has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDB:
	case 0x1DB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_0_mem(addr);
			}
			else {
				instr_DB_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_1_mem(addr);
			}
			else {
				instr_DB_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_2_mem(addr);
			}
			else {
				instr_DB_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_3_mem(addr);
			}
			else {
				instr_DB_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_4_mem(addr);
			}
			else {
				instr_DB_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_5_mem(addr);
			}
			else {
				instr_DB_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_6_mem(addr);
			}
			else {
				instr_DB_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DB_7_mem(addr);
			}
			else {
				instr_DB_7_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2DB:
	{
		unimplemented_instruction("long mode: 0xDB has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDC:
	case 0x1DC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_0_mem(addr);
			}
			else {
				instr_DC_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_1_mem(addr);
			}
			else {
				instr_DC_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_2_mem(addr);
			}
			else {
				instr_DC_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_3_mem(addr);
			}
			else {
				instr_DC_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_4_mem(addr);
			}
			else {
				instr_DC_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_5_mem(addr);
			}
			else {
				instr_DC_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_6_mem(addr);
			}
			else {
				instr_DC_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DC_7_mem(addr);
			}
			else {
				instr_DC_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2DC:
	{
		unimplemented_instruction("long mode: 0xDC has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_0_mem(addr);
			}
			else {
				instr16_DD_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_1_mem(addr);
			}
			else {
				instr16_DD_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_2_mem(addr);
			}
			else {
				instr16_DD_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_3_mem(addr);
			}
			else {
				instr16_DD_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_4_mem(addr);
			}
			else {
				instr16_DD_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_5_mem(addr);
			}
			else {
				instr16_DD_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_6_mem(addr);
			}
			else {
				instr16_DD_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_DD_7_mem(addr);
			}
			else {
				instr16_DD_7_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1DD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_0_mem(addr);
			}
			else {
				instr32_DD_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_1_mem(addr);
			}
			else {
				instr32_DD_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_2_mem(addr);
			}
			else {
				instr32_DD_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_3_mem(addr);
			}
			else {
				instr32_DD_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_4_mem(addr);
			}
			else {
				instr32_DD_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_5_mem(addr);
			}
			else {
				instr32_DD_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_6_mem(addr);
			}
			else {
				instr32_DD_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_DD_7_mem(addr);
			}
			else {
				instr32_DD_7_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2DD:
	{
		unimplemented_instruction("long mode: 0xDD has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDE:
	case 0x1DE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_0_mem(addr);
			}
			else {
				instr_DE_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_1_mem(addr);
			}
			else {
				instr_DE_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_2_mem(addr);
			}
			else {
				instr_DE_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_3_mem(addr);
			}
			else {
				instr_DE_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_4_mem(addr);
			}
			else {
				instr_DE_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_5_mem(addr);
			}
			else {
				instr_DE_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_6_mem(addr);
			}
			else {
				instr_DE_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DE_7_mem(addr);
			}
			else {
				instr_DE_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2DE:
	{
		unimplemented_instruction("long mode: 0xDE has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDF:
	case 0x1DF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_0_mem(addr);
			}
			else {
				instr_DF_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_1_mem(addr);
			}
			else {
				instr_DF_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_2_mem(addr);
			}
			else {
				instr_DF_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_3_mem(addr);
			}
			else {
				instr_DF_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_4_mem(addr);
			}
			else {
				instr_DF_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_5_mem(addr);
			}
			else {
				instr_DF_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_6_mem(addr);
			}
			else {
				instr_DF_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_DF_7_mem(addr);
			}
			else {
				instr_DF_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2DF:
	{
		unimplemented_instruction("long mode: 0xDF has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE0:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_E0(imm);
	}
	break;
	case 0x1E0:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_E0(imm);
	}
	break;
	case 0x2E0:
	{
		unimplemented_instruction("long mode: 0xE0 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE1:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_E1(imm);
	}
	break;
	case 0x1E1:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_E1(imm);
	}
	break;
	case 0x2E1:
	{
		unimplemented_instruction("long mode: 0xE1 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE2:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_E2(imm);
	}
	break;
	case 0x1E2:
	{
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_E2(imm);
	}
	break;
	case 0x2E2:
	{
		unimplemented_instruction("long mode: 0xE2 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE3:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xE3 is JRCXZ in 64-bit mode (RCX, not ECX), and the jcc family is S7's");
			return;
		}
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_E3(imm);
	}
	break;
	case 0x1E3:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xE3 is JRCXZ in 64-bit mode (RCX, not ECX), and the jcc family is S7's");
			return;
		}
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_E3(imm);
	}
	break;
	case 0x2E3:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xE3 is JRCXZ in 64-bit mode (RCX, not ECX), and the jcc family is S7's");
			return;
		}
	}
	break;
	case 0xE4:
	case 0x1E4:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_E4(imm);
		after_block_boundary();
	}
	break;
	case 0x2E4:
	{
		unimplemented_instruction("long mode: 0xE4 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE5:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr16_E5(imm);
		after_block_boundary();
	}
	break;
	case 0x1E5:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr32_E5(imm);
		after_block_boundary();
	}
	break;
	case 0x2E5:
	{
		unimplemented_instruction("long mode: 0xE5 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE6:
	case 0x1E6:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr_E6(imm);
		after_block_boundary();
	}
	break;
	case 0x2E6:
	{
		unimplemented_instruction("long mode: 0xE6 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE7:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr16_E7(imm);
		after_block_boundary();
	}
	break;
	case 0x1E7:
	{
		int32_t imm = read_imm8();
		if (cpu->pagefault) return;
		instr32_E7(imm);
		after_block_boundary();
	}
	break;
	case 0x2E7:
	{
		unimplemented_instruction("long mode: 0xE7 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE8:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_E8(imm);
		after_block_boundary();
	}
	break;
	case 0x1E8:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_E8(imm);
		after_block_boundary();
	}
	break;
	case 0x2E8:
	{
		int64_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr64_E8(imm);
		after_block_boundary();
	}
	break;
	case 0xE9:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_E9(imm);
	}
	break;
	case 0x1E9:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_E9(imm);
	}
	break;
	case 0x2E9:
	{
		int64_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr64_E9(imm);
	}
	break;
	case 0xEA:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xEA (jmp far ptr16:32) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		int32_t imm2 = read_imm16();
		if (cpu->pagefault) return;
		instr16_EA(imm, imm2);
		after_block_boundary();
	}
	break;
	case 0x1EA:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xEA (jmp far ptr16:32) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		int32_t imm2 = read_imm16();
		if (cpu->pagefault) return;
		instr32_EA(imm, imm2);
		after_block_boundary();
	}
	break;
	case 0x2EA:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: 0xEA (jmp far ptr16:32) is invalid in 64-bit mode");
			trigger_ud();
			return;
		}
	}
	break;
	case 0xEB:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr16_EB(imm);
	}
	break;
	case 0x1EB:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr32_EB(imm);
	}
	break;
	case 0x2EB:
	{
		int64_t imm = read_imm8s();
		if (cpu->pagefault) return;
		instr64_EB(imm);
	}
	break;
	case 0xEC:
	case 0x1EC:
	{
		instr_EC();
		after_block_boundary();
	}
	break;
	case 0x2EC:
	{
		unimplemented_instruction("long mode: 0xEC has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xED:
	{
		instr16_ED();
		after_block_boundary();
	}
	break;
	case 0x1ED:
	{
		instr32_ED();
		after_block_boundary();
	}
	break;
	case 0x2ED:
	{
		unimplemented_instruction("long mode: 0xED has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xEE:
	case 0x1EE:
	{
		instr_EE();
		after_block_boundary();
	}
	break;
	case 0x2EE:
	{
		unimplemented_instruction("long mode: 0xEE has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xEF:
	{
		instr16_EF();
		after_block_boundary();
	}
	break;
	case 0x1EF:
	{
		instr32_EF();
		after_block_boundary();
	}
	break;
	case 0x2EF:
	{
		unimplemented_instruction("long mode: 0xEF has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF0:
	case 0x1F0:
	{
		instr_F0();
	}
	break;
	case 0x2F0:
	{
		unimplemented_instruction("long mode: 0xF0 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF1:
	case 0x1F1:
	{
		instr_F1();
	}
	break;
	case 0x2F1:
	{
		unimplemented_instruction("long mode: 0xF1 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF2:
	case 0x1F2:
	{
		instr_F2();
	}
	break;
	case 0x2F2:
	{
		unimplemented_instruction("long mode: 0xF2 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF3:
	case 0x1F3:
	{
		instr_F3();
	}
	break;
	case 0x2F3:
	{
		unimplemented_instruction("long mode: 0xF3 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF4:
	case 0x1F4:
	{
		instr_F4();
		after_block_boundary();
	}
	break;
	case 0x2F4:
	{
		instr64_F4();
		after_block_boundary();
	}
	break;
	case 0xF5:
	case 0x1F5:
	{
		instr_F5();
	}
	break;
	case 0x2F5:
	{
		unimplemented_instruction("long mode: 0xF5 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF6:
	case 0x1F6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F6_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F6_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F6_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F6_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F6_2_mem(addr);
			}
			else {
				instr_F6_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F6_3_mem(addr);
			}
			else {
				instr_F6_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F6_4_mem(addr);
			}
			else {
				instr_F6_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F6_5_mem(addr);
			}
			else {
				instr_F6_5_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F6_6_mem(addr);
			}
			else {
				instr_F6_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F6_7_mem(addr);
			}
			else {
				instr_F6_7_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2F6:
	{
		unimplemented_instruction("long mode: 0xF6 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_F7_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_F7_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_F7_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm16();
				if (cpu->pagefault) return;
				instr16_F7_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_F7_2_mem(addr);
			}
			else {
				instr16_F7_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_F7_3_mem(addr);
			}
			else {
				instr16_F7_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_F7_4_mem(addr);
			}
			else {
				instr16_F7_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_F7_5_mem(addr);
			}
			else {
				instr16_F7_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_F7_6_mem(addr);
			}
			else {
				instr16_F7_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_F7_7_mem(addr);
			}
			else {
				instr16_F7_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1F7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_F7_0_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_F7_0_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_F7_1_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm32s();
				if (cpu->pagefault) return;
				instr32_F7_1_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_F7_2_mem(addr);
			}
			else {
				instr32_F7_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_F7_3_mem(addr);
			}
			else {
				instr32_F7_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_F7_4_mem(addr);
			}
			else {
				instr32_F7_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_F7_5_mem(addr);
			}
			else {
				instr32_F7_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_F7_6_mem(addr);
			}
			else {
				instr32_F7_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_F7_7_mem(addr);
			}
			else {
				instr32_F7_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2F7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_F7_0_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_F7_0_reg");
				return;
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_F7_1_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_F7_1_reg");
				return;
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_F7_2_mem(addr);
			}
			else {
				instr64_F7_2_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_F7_3_mem(addr);
			}
			else {
				instr64_F7_3_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_F7_4_mem(addr);
			}
			else {
				instr64_F7_4_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_F7_5_mem(addr);
			}
			else {
				instr64_F7_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_F7_6_mem(addr);
			}
			else {
				instr64_F7_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_F7_7_mem(addr);
			}
			else {
				instr64_F7_7_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xF8:
	case 0x1F8:
	{
		instr_F8();
	}
	break;
	case 0x2F8:
	{
		unimplemented_instruction("long mode: 0xF8 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF9:
	case 0x1F9:
	{
		instr_F9();
	}
	break;
	case 0x2F9:
	{
		unimplemented_instruction("long mode: 0xF9 has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFA:
	case 0x1FA:
	{
		instr_FA();
	}
	break;
	case 0x2FA:
	{
		unimplemented_instruction("long mode: 0xFA has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFB:
	case 0x1FB:
	{
		instr_FB();
	}
	break;
	case 0x2FB:
	{
		unimplemented_instruction("long mode: 0xFB has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFC:
	case 0x1FC:
	{
		instr_FC();
	}
	break;
	case 0x2FC:
	{
		unimplemented_instruction("long mode: 0xFC has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFD:
	case 0x1FD:
	{
		instr_FD();
	}
	break;
	case 0x2FD:
	{
		unimplemented_instruction("long mode: 0xFD has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFE:
	case 0x1FE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_FE_0_mem(addr);
			}
			else {
				instr_FE_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_FE_1_mem(addr);
			}
			else {
				instr_FE_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2FE:
	{
		unimplemented_instruction("long mode: 0xFE has no 64-bit form in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFF:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_FF_0_mem(addr);
			}
			else {
				instr16_FF_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_FF_1_mem(addr);
			}
			else {
				instr16_FF_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_FF_2_mem(addr);
			}
			else {
				instr16_FF_2_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_FF_3_mem(addr);
			}
			else {
				instr16_FF_3_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_FF_4_mem(addr);
			}
			else {
				instr16_FF_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_FF_5_mem(addr);
			}
			else {
				instr16_FF_5_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_FF_6_mem(addr);
			}
			else {
				instr16_FF_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1FF:
	{
		if (long_mode_64()) {
			unimplemented_instruction("long mode: the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)");
			return;
		}
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_FF_0_mem(addr);
			}
			else {
				instr32_FF_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_FF_1_mem(addr);
			}
			else {
				instr32_FF_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_FF_2_mem(addr);
			}
			else {
				instr32_FF_2_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_FF_3_mem(addr);
			}
			else {
				instr32_FF_3_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_FF_4_mem(addr);
			}
			else {
				instr32_FF_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_FF_5_mem(addr);
			}
			else {
				instr32_FF_5_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_FF_6_mem(addr);
			}
			else {
				instr32_FF_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2FF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_FF_0_mem(addr);
			}
			else {
				instr64_FF_0_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_FF_1_mem(addr);
			}
			else {
				instr64_FF_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_FF_2_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_FF_2_reg");
				return;
			}
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_FF_3_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_FF_3_reg");
				return;
			}
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_FF_4_mem(addr);
			}
			else {
				instr64_FF_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions::instr64_FF_5_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions::instr64_FF_5_reg");
				return;
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_FF_6_mem(addr);
			}
			else {
				instr64_FF_6_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	default:
		DBG_ASSERT(false);
		break;
	}
}

/* The 0F-table switch, same convention.  run_instruction0f_16/32 (cpu.rs:3132-3133)
 * -- the wrappers instructions.c's instr16_0F/instr32_0F call -- live in
 * instructions_0f.c and are the only callers. */
void run_instruction0f(int32_t opcode)
{
	switch(opcode)
	{
	case 0x00:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F00_0_mem(addr);
			}
			else {
				instr16_0F00_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F00_1_mem(addr);
			}
			else {
				instr16_0F00_1_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F00_2_mem(addr);
			}
			else {
				instr16_0F00_2_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F00_3_mem(addr);
			}
			else {
				instr16_0F00_3_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F00_4_mem(addr);
			}
			else {
				instr16_0F00_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F00_5_mem(addr);
			}
			else {
				instr16_0F00_5_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x100:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F00_0_mem(addr);
			}
			else {
				instr32_0F00_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F00_1_mem(addr);
			}
			else {
				instr32_0F00_1_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F00_2_mem(addr);
			}
			else {
				instr32_0F00_2_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F00_3_mem(addr);
			}
			else {
				instr32_0F00_3_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F00_4_mem(addr);
			}
			else {
				instr32_0F00_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F00_5_mem(addr);
			}
			else {
				instr32_0F00_5_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x200:
	{
		unimplemented_instruction("long mode: 0F 00 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x01:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F01_0_mem(addr);
			}
			else {
				instr16_0F01_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F01_1_mem(addr);
			}
			else {
				instr16_0F01_1_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F01_2_mem(addr);
			}
			else {
				instr16_0F01_2_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F01_3_mem(addr);
			}
			else {
				instr16_0F01_3_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F01_4_mem(addr);
			}
			else {
				instr16_0F01_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F01_6_mem(addr);
			}
			else {
				instr16_0F01_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0F01_7_mem(addr);
			}
			else {
				instr16_0F01_7_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x101:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F01_0_mem(addr);
			}
			else {
				instr32_0F01_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F01_1_mem(addr);
			}
			else {
				instr32_0F01_1_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 2:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F01_2_mem(addr);
			}
			else {
				instr32_0F01_2_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 3:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F01_3_mem(addr);
			}
			else {
				instr32_0F01_3_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F01_4_mem(addr);
			}
			else {
				instr32_0F01_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F01_6_mem(addr);
			}
			else {
				instr32_0F01_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0F01_7_mem(addr);
			}
			else {
				instr32_0F01_7_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x201:
	{
		unimplemented_instruction("long mode: 0F 01 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x02:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F02_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F02_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x102:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F02_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F02_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x202:
	{
		unimplemented_instruction("long mode: 0F 02 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x03:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F03_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F03_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x103:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F03_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F03_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x203:
	{
		unimplemented_instruction("long mode: 0F 03 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x04:
	case 0x104:
	{
		instr_0F04();
		after_block_boundary();
	}
	break;
	case 0x204:
	{
		unimplemented_instruction("long mode: 0F 04 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x05:
	case 0x105:
	{
		instr_0F05();
		after_block_boundary();
	}
	break;
	case 0x205:
	{
		unimplemented_instruction("long mode: 0F 05 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x06:
	case 0x106:
	{
		instr_0F06();
		after_block_boundary();
	}
	break;
	case 0x206:
	{
		unimplemented_instruction("long mode: 0F 06 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x07:
	case 0x107:
	{
		instr_0F07();
		after_block_boundary();
	}
	break;
	case 0x207:
	{
		unimplemented_instruction("long mode: 0F 07 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x08:
	case 0x108:
	{
		instr_0F08();
		after_block_boundary();
	}
	break;
	case 0x208:
	{
		unimplemented_instruction("long mode: 0F 08 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x09:
	case 0x109:
	{
		instr_0F09();
		after_block_boundary();
	}
	break;
	case 0x209:
	{
		unimplemented_instruction("long mode: 0F 09 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0A:
	case 0x10A:
	{
		instr_0F0A();
		after_block_boundary();
	}
	break;
	case 0x20A:
	{
		unimplemented_instruction("long mode: 0F 0A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0B:
	case 0x10B:
	{
		instr_0F0B();
		after_block_boundary();
	}
	break;
	case 0x20B:
	{
		unimplemented_instruction("long mode: 0F 0B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0C:
	case 0x10C:
	{
		instr_0F0C();
		after_block_boundary();
	}
	break;
	case 0x20C:
	{
		unimplemented_instruction("long mode: 0F 0C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0D:
	case 0x10D:
	{
		instr_0F0D();
		after_block_boundary();
	}
	break;
	case 0x20D:
	{
		unimplemented_instruction("long mode: 0F 0D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0E:
	case 0x10E:
	{
		instr_0F0E();
		after_block_boundary();
	}
	break;
	case 0x20E:
	{
		unimplemented_instruction("long mode: 0F 0E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x0F:
	case 0x10F:
	{
		instr_0F0F();
		after_block_boundary();
	}
	break;
	case 0x20F:
	{
		unimplemented_instruction("long mode: 0F 0F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x10:
	case 0x110:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F10_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F10_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F10_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F10_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F10_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F10_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F10_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F10_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x210:
	{
		unimplemented_instruction("long mode: 0F 10 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x11:
	case 0x111:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F11_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F11_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F11_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F11_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F11_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F11_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F11_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F11_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x211:
	{
		unimplemented_instruction("long mode: 0F 11 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x12:
	case 0x112:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F12_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F12_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F12_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F12_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F12_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F12_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F12_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F12_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x212:
	{
		unimplemented_instruction("long mode: 0F 12 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x13:
	case 0x113:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F13_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F13_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F13_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F13_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x213:
	{
		unimplemented_instruction("long mode: 0F 13 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x14:
	case 0x114:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F14_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F14_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F14_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F14_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x214:
	{
		unimplemented_instruction("long mode: 0F 14 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x15:
	case 0x115:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F15_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F15_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F15_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F15_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x215:
	{
		unimplemented_instruction("long mode: 0F 15 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x16:
	case 0x116:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F16_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F16_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F16_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F16_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F16_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F16_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x216:
	{
		unimplemented_instruction("long mode: 0F 16 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x17:
	case 0x117:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F17_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F17_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F17_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F17_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x217:
	{
		unimplemented_instruction("long mode: 0F 17 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x18:
	case 0x118:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F18_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F18_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x218:
	{
		unimplemented_instruction("long mode: 0F 18 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x19:
	case 0x119:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F19_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F19_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x219:
	{
		unimplemented_instruction("long mode: 0F 19 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1A:
	case 0x11A:
	{
		instr_0F1A();
		after_block_boundary();
	}
	break;
	case 0x21A:
	{
		unimplemented_instruction("long mode: 0F 1A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1B:
	case 0x11B:
	{
		instr_0F1B();
		after_block_boundary();
	}
	break;
	case 0x21B:
	{
		unimplemented_instruction("long mode: 0F 1B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1C:
	case 0x11C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F1C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F1C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x21C:
	{
		unimplemented_instruction("long mode: 0F 1C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1D:
	case 0x11D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F1D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F1D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x21D:
	{
		unimplemented_instruction("long mode: 0F 1D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1E:
	case 0x11E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F1E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F1E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x21E:
	{
		unimplemented_instruction("long mode: 0F 1E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x1F:
	case 0x11F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F1F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F1F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x21F:
	{
		unimplemented_instruction("long mode: 0F 1F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x20:
	case 0x120:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		instr_0F20(modrm_byte & 7, modrm_byte >> 3 & 7);
		after_block_boundary();
	}
	break;
	case 0x220:
	{
		unimplemented_instruction("long mode: 0F 20 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x21:
	case 0x121:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		instr_0F21(modrm_byte & 7, modrm_byte >> 3 & 7);
		after_block_boundary();
	}
	break;
	case 0x221:
	{
		unimplemented_instruction("long mode: 0F 21 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x22:
	case 0x122:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		instr_0F22(modrm_byte & 7, modrm_byte >> 3 & 7);
		after_block_boundary();
	}
	break;
	case 0x222:
	{
		unimplemented_instruction("long mode: 0F 22 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x23:
	case 0x123:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		instr_0F23(modrm_byte & 7, modrm_byte >> 3 & 7);
		after_block_boundary();
	}
	break;
	case 0x223:
	{
		unimplemented_instruction("long mode: 0F 23 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x24:
	case 0x124:
	{
		instr_0F24();
		after_block_boundary();
	}
	break;
	case 0x224:
	{
		unimplemented_instruction("long mode: 0F 24 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x25:
	case 0x125:
	{
		instr_0F25();
		after_block_boundary();
	}
	break;
	case 0x225:
	{
		unimplemented_instruction("long mode: 0F 25 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x26:
	case 0x126:
	{
		instr_0F26();
		after_block_boundary();
	}
	break;
	case 0x226:
	{
		unimplemented_instruction("long mode: 0F 26 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x27:
	case 0x127:
	{
		instr_0F27();
		after_block_boundary();
	}
	break;
	case 0x227:
	{
		unimplemented_instruction("long mode: 0F 27 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x28:
	case 0x128:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F28_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F28_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F28_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F28_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x228:
	{
		unimplemented_instruction("long mode: 0F 28 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x29:
	case 0x129:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F29_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F29_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F29_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F29_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x229:
	{
		unimplemented_instruction("long mode: 0F 29 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2A:
	case 0x12A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F2A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F2A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F2A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F2A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F2A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F2A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F2A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F2A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x22A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_660F2A_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_660F2A_reg");
				return;
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_F20F2A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr64_F20F2A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_F30F2A_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_F30F2A_reg");
				return;
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_0F2A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr64_0F2A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2B:
	case 0x12B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F2B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F2B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F2B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F2B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x22B:
	{
		unimplemented_instruction("long mode: 0F 2B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2C:
	case 0x12C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F2C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F2C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F2C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F2C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F2C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F2C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F2C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F2C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x22C:
	{
		unimplemented_instruction("long mode: 0F 2C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2D:
	case 0x12D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F2D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F2D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F2D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F2D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F2D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F2D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F2D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F2D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x22D:
	{
		unimplemented_instruction("long mode: 0F 2D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2E:
	case 0x12E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F2E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F2E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F2E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F2E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x22E:
	{
		unimplemented_instruction("long mode: 0F 2E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x2F:
	case 0x12F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F2F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F2F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F2F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F2F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x22F:
	{
		unimplemented_instruction("long mode: 0F 2F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x30:
	case 0x130:
	{
		instr_0F30();
		after_block_boundary();
	}
	break;
	case 0x230:
	{
		unimplemented_instruction("long mode: 0F 30 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x31:
	case 0x131:
	{
		instr_0F31();
	}
	break;
	case 0x231:
	{
		unimplemented_instruction("long mode: 0F 31 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x32:
	case 0x132:
	{
		instr_0F32();
		after_block_boundary();
	}
	break;
	case 0x232:
	{
		unimplemented_instruction("long mode: 0F 32 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x33:
	case 0x133:
	{
		instr_0F33();
		after_block_boundary();
	}
	break;
	case 0x233:
	{
		unimplemented_instruction("long mode: 0F 33 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x34:
	case 0x134:
	{
		instr_0F34();
		after_block_boundary();
	}
	break;
	case 0x234:
	{
		unimplemented_instruction("long mode: 0F 34 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x35:
	case 0x135:
	{
		instr_0F35();
		after_block_boundary();
	}
	break;
	case 0x235:
	{
		unimplemented_instruction("long mode: 0F 35 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x36:
	case 0x136:
	{
		instr_0F36();
		after_block_boundary();
	}
	break;
	case 0x236:
	{
		unimplemented_instruction("long mode: 0F 36 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x37:
	case 0x137:
	{
		instr_0F37();
		after_block_boundary();
	}
	break;
	case 0x237:
	{
		unimplemented_instruction("long mode: 0F 37 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x38:
	case 0x138:
	{
		instr_0F38();
		after_block_boundary();
	}
	break;
	case 0x238:
	{
		unimplemented_instruction("long mode: 0F 38 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x39:
	case 0x139:
	{
		instr_0F39();
		after_block_boundary();
	}
	break;
	case 0x239:
	{
		unimplemented_instruction("long mode: 0F 39 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3A:
	case 0x13A:
	{
		instr_0F3A();
		after_block_boundary();
	}
	break;
	case 0x23A:
	{
		unimplemented_instruction("long mode: 0F 3A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3B:
	case 0x13B:
	{
		instr_0F3B();
		after_block_boundary();
	}
	break;
	case 0x23B:
	{
		unimplemented_instruction("long mode: 0F 3B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3C:
	case 0x13C:
	{
		instr_0F3C();
		after_block_boundary();
	}
	break;
	case 0x23C:
	{
		unimplemented_instruction("long mode: 0F 3C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3D:
	case 0x13D:
	{
		instr_0F3D();
		after_block_boundary();
	}
	break;
	case 0x23D:
	{
		unimplemented_instruction("long mode: 0F 3D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3E:
	case 0x13E:
	{
		instr_0F3E();
		after_block_boundary();
	}
	break;
	case 0x23E:
	{
		unimplemented_instruction("long mode: 0F 3E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x3F:
	case 0x13F:
	{
		instr_0F3F();
		after_block_boundary();
	}
	break;
	case 0x23F:
	{
		unimplemented_instruction("long mode: 0F 3F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x40:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F40_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F40_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x140:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F40_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F40_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x240:
	{
		unimplemented_instruction("long mode: 0F 40 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x41:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F41_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F41_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x141:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F41_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F41_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x241:
	{
		unimplemented_instruction("long mode: 0F 41 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x42:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F42_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F42_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x142:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F42_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F42_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x242:
	{
		unimplemented_instruction("long mode: 0F 42 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x43:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F43_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F43_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x143:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F43_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F43_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x243:
	{
		unimplemented_instruction("long mode: 0F 43 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x44:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F44_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F44_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x144:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F44_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F44_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x244:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0F44_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0F44_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x45:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F45_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F45_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x145:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F45_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F45_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x245:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0F45_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0F45_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x46:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F46_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F46_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x146:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F46_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F46_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x246:
	{
		unimplemented_instruction("long mode: 0F 46 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x47:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F47_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F47_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x147:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F47_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F47_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x247:
	{
		unimplemented_instruction("long mode: 0F 47 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x48:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F48_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F48_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x148:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F48_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F48_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x248:
	{
		unimplemented_instruction("long mode: 0F 48 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x49:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F49_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F49_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x149:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F49_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F49_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x249:
	{
		unimplemented_instruction("long mode: 0F 49 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x4A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F4A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F4A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x14A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F4A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F4A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x24A:
	{
		unimplemented_instruction("long mode: 0F 4A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x4B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F4B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F4B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x14B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F4B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F4B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x24B:
	{
		unimplemented_instruction("long mode: 0F 4B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x4C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F4C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F4C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x14C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F4C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F4C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x24C:
	{
		unimplemented_instruction("long mode: 0F 4C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x4D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F4D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F4D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x14D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F4D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F4D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x24D:
	{
		unimplemented_instruction("long mode: 0F 4D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x4E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F4E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F4E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x14E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F4E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F4E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x24E:
	{
		unimplemented_instruction("long mode: 0F 4E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x4F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0F4F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0F4F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x14F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0F4F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0F4F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x24F:
	{
		unimplemented_instruction("long mode: 0F 4F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x50:
	case 0x150:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F50_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F50_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F50_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F50_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x250:
	{
		unimplemented_instruction("long mode: 0F 50 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x51:
	case 0x151:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F51_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F51_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F51_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F51_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F51_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F51_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F51_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F51_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x251:
	{
		unimplemented_instruction("long mode: 0F 51 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x52:
	case 0x152:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F52_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F52_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F52_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F52_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x252:
	{
		unimplemented_instruction("long mode: 0F 52 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x53:
	case 0x153:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F53_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F53_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F53_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F53_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x253:
	{
		unimplemented_instruction("long mode: 0F 53 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x54:
	case 0x154:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F54_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F54_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F54_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F54_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x254:
	{
		unimplemented_instruction("long mode: 0F 54 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x55:
	case 0x155:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F55_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F55_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F55_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F55_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x255:
	{
		unimplemented_instruction("long mode: 0F 55 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x56:
	case 0x156:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F56_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F56_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F56_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F56_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x256:
	{
		unimplemented_instruction("long mode: 0F 56 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x57:
	case 0x157:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F57_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F57_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F57_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F57_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x257:
	{
		unimplemented_instruction("long mode: 0F 57 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x58:
	case 0x158:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F58_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F58_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F58_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F58_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F58_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F58_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F58_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F58_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x258:
	{
		unimplemented_instruction("long mode: 0F 58 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x59:
	case 0x159:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F59_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F59_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F59_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F59_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F59_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F59_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F59_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F59_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x259:
	{
		unimplemented_instruction("long mode: 0F 59 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x5A:
	case 0x15A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F5A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F5A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F5A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F5A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F5A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F5A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F5A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F5A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x25A:
	{
		unimplemented_instruction("long mode: 0F 5A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x5B:
	case 0x15B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F5B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F5B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F5B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F5B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F5B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F5B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x25B:
	{
		unimplemented_instruction("long mode: 0F 5B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x5C:
	case 0x15C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F5C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F5C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F5C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F5C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F5C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F5C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F5C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F5C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x25C:
	{
		unimplemented_instruction("long mode: 0F 5C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x5D:
	case 0x15D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F5D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F5D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F5D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F5D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F5D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F5D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F5D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F5D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x25D:
	{
		unimplemented_instruction("long mode: 0F 5D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x5E:
	case 0x15E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F5E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F5E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F5E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F5E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F5E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F5E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F5E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F5E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x25E:
	{
		unimplemented_instruction("long mode: 0F 5E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x5F:
	case 0x15F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F5F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F5F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F5F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F5F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F5F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F5F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F5F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F5F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x25F:
	{
		unimplemented_instruction("long mode: 0F 5F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x60:
	case 0x160:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F60_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F60_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F60_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F60_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x260:
	{
		unimplemented_instruction("long mode: 0F 60 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x61:
	case 0x161:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F61_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F61_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F61_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F61_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x261:
	{
		unimplemented_instruction("long mode: 0F 61 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x62:
	case 0x162:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F62_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F62_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F62_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F62_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x262:
	{
		unimplemented_instruction("long mode: 0F 62 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x63:
	case 0x163:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F63_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F63_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F63_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F63_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x263:
	{
		unimplemented_instruction("long mode: 0F 63 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x64:
	case 0x164:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F64_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F64_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F64_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F64_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x264:
	{
		unimplemented_instruction("long mode: 0F 64 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x65:
	case 0x165:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F65_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F65_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F65_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F65_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x265:
	{
		unimplemented_instruction("long mode: 0F 65 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x66:
	case 0x166:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F66_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F66_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F66_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F66_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x266:
	{
		unimplemented_instruction("long mode: 0F 66 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x67:
	case 0x167:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F67_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F67_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F67_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F67_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x267:
	{
		unimplemented_instruction("long mode: 0F 67 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x68:
	case 0x168:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F68_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F68_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F68_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F68_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x268:
	{
		unimplemented_instruction("long mode: 0F 68 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x69:
	case 0x169:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F69_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F69_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F69_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F69_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x269:
	{
		unimplemented_instruction("long mode: 0F 69 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6A:
	case 0x16A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F6A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F6A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F6A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F6A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x26A:
	{
		unimplemented_instruction("long mode: 0F 6A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6B:
	case 0x16B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F6B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F6B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F6B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F6B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x26B:
	{
		unimplemented_instruction("long mode: 0F 6B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6C:
	case 0x16C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F6C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F6C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F6C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F6C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x26C:
	{
		unimplemented_instruction("long mode: 0F 6C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6D:
	case 0x16D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F6D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F6D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F6D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F6D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x26D:
	{
		unimplemented_instruction("long mode: 0F 6D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x6E:
	case 0x16E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F6E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F6E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F6E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F6E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x26E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_660F6E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr64_660F6E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_0F6E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr64_0F6E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x6F:
	case 0x16F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F6F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F6F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F6F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F6F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F6F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F6F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x26F:
	{
		unimplemented_instruction("long mode: 0F 6F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x70:
	case 0x170:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660F70_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660F70_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F20F70_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F20F70_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F30F70_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F30F70_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0F70_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0F70_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
	}
	break;
	case 0x270:
	{
		unimplemented_instruction("long mode: 0F 70 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x71:
	case 0x171:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			switch (modrm_byte >> 3 & 7) {
			case 2:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F71_2_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F71_2_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 4:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F71_4_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F71_4_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 6:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F71_6_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F71_6_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			default:
				trigger_ud();
				break;
			}
		}
		else {
			switch (modrm_byte >> 3 & 7) {
			case 2:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F71_2_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F71_2_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 4:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F71_4_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F71_4_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 6:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F71_6_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F71_6_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			default:
				trigger_ud();
				break;
			}
		}
	}
	break;
	case 0x271:
	{
		unimplemented_instruction("long mode: 0F 71 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x72:
	case 0x172:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			switch (modrm_byte >> 3 & 7) {
			case 2:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F72_2_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F72_2_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 4:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F72_4_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F72_4_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 6:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F72_6_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F72_6_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			default:
				trigger_ud();
				break;
			}
		}
		else {
			switch (modrm_byte >> 3 & 7) {
			case 2:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F72_2_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F72_2_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 4:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F72_4_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F72_4_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 6:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F72_6_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F72_6_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			default:
				trigger_ud();
				break;
			}
		}
	}
	break;
	case 0x272:
	{
		unimplemented_instruction("long mode: 0F 72 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x73:
	case 0x173:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			switch (modrm_byte >> 3 & 7) {
			case 2:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_2_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_2_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 3:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_3_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_3_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 6:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_6_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_6_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 7:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_7_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_660F73_7_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			default:
				trigger_ud();
				break;
			}
		}
		else {
			switch (modrm_byte >> 3 & 7) {
			case 2:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F73_2_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F73_2_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			case 6:
			{
				if (!task_switch_test_mmx()) {
					return;
				}
				if (modrm_byte < 0xC0) {
					int32_t addr = modrm_resolve(modrm_byte);
					if (cpu->pagefault) return;
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F73_6_mem(addr, imm);
				}
				else {
					int32_t imm = read_imm8();
					if (cpu->pagefault) return;
					instr_0F73_6_reg(reg_index_b(modrm_byte & 7), imm);
				}
			}
				break;
			default:
				trigger_ud();
				break;
			}
		}
	}
	break;
	case 0x273:
	{
		unimplemented_instruction("long mode: 0F 73 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x74:
	case 0x174:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F74_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F74_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F74_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F74_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x274:
	{
		unimplemented_instruction("long mode: 0F 74 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x75:
	case 0x175:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F75_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F75_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F75_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F75_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x275:
	{
		unimplemented_instruction("long mode: 0F 75 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x76:
	case 0x176:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F76_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F76_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F76_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F76_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x276:
	{
		unimplemented_instruction("long mode: 0F 76 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x77:
	case 0x177:
	{
		if (!task_switch_test_mmx()) {
			return;
		}
		instr_0F77();
	}
	break;
	case 0x277:
	{
		unimplemented_instruction("long mode: 0F 77 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x78:
	case 0x178:
	{
		instr_0F78();
		after_block_boundary();
	}
	break;
	case 0x278:
	{
		unimplemented_instruction("long mode: 0F 78 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x79:
	case 0x179:
	{
		instr_0F79();
		after_block_boundary();
	}
	break;
	case 0x279:
	{
		unimplemented_instruction("long mode: 0F 79 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x7A:
	case 0x17A:
	{
		instr_0F7A();
		after_block_boundary();
	}
	break;
	case 0x27A:
	{
		unimplemented_instruction("long mode: 0F 7A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x7B:
	case 0x17B:
	{
		instr_0F7B();
		after_block_boundary();
	}
	break;
	case 0x27B:
	{
		unimplemented_instruction("long mode: 0F 7B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x7C:
	case 0x17C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F7C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F7C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F7C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F7C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			instr_0F7C();
			after_block_boundary();
		}
	}
	break;
	case 0x27C:
	{
		unimplemented_instruction("long mode: 0F 7C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x7D:
	case 0x17D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F7D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F7D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20F7D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20F7D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			instr_0F7D();
			after_block_boundary();
		}
	}
	break;
	case 0x27D:
	{
		unimplemented_instruction("long mode: 0F 7D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x7E:
	case 0x17E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F7E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F7E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F7E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F7E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F7E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F7E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x27E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_660F7E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr64_660F7E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_F30F7E_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_F30F7E_reg");
				return;
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_0F7E_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_0F7E_reg");
				return;
			}
		}
	}
	break;
	case 0x7F:
	case 0x17F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660F7F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660F7F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30F7F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30F7F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0F7F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0F7F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x27F:
	{
		unimplemented_instruction("long mode: 0F 7F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x80:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F80(imm);
	}
	break;
	case 0x180:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F80(imm);
	}
	break;
	case 0x280:
	{
		unimplemented_instruction("long mode: 0F 80 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x81:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F81(imm);
	}
	break;
	case 0x181:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F81(imm);
	}
	break;
	case 0x281:
	{
		unimplemented_instruction("long mode: 0F 81 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x82:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F82(imm);
	}
	break;
	case 0x182:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F82(imm);
	}
	break;
	case 0x282:
	{
		unimplemented_instruction("long mode: 0F 82 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x83:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F83(imm);
	}
	break;
	case 0x183:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F83(imm);
	}
	break;
	case 0x283:
	{
		unimplemented_instruction("long mode: 0F 83 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x84:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F84(imm);
	}
	break;
	case 0x184:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F84(imm);
	}
	break;
	case 0x284:
	{
		unimplemented_instruction("long mode: 0F 84 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x85:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F85(imm);
	}
	break;
	case 0x185:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F85(imm);
	}
	break;
	case 0x285:
	{
		unimplemented_instruction("long mode: 0F 85 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x86:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F86(imm);
	}
	break;
	case 0x186:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F86(imm);
	}
	break;
	case 0x286:
	{
		unimplemented_instruction("long mode: 0F 86 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x87:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F87(imm);
	}
	break;
	case 0x187:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F87(imm);
	}
	break;
	case 0x287:
	{
		unimplemented_instruction("long mode: 0F 87 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x88:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F88(imm);
	}
	break;
	case 0x188:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F88(imm);
	}
	break;
	case 0x288:
	{
		unimplemented_instruction("long mode: 0F 88 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x89:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F89(imm);
	}
	break;
	case 0x189:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F89(imm);
	}
	break;
	case 0x289:
	{
		unimplemented_instruction("long mode: 0F 89 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8A:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F8A(imm);
	}
	break;
	case 0x18A:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F8A(imm);
	}
	break;
	case 0x28A:
	{
		unimplemented_instruction("long mode: 0F 8A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8B:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F8B(imm);
	}
	break;
	case 0x18B:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F8B(imm);
	}
	break;
	case 0x28B:
	{
		unimplemented_instruction("long mode: 0F 8B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8C:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F8C(imm);
	}
	break;
	case 0x18C:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F8C(imm);
	}
	break;
	case 0x28C:
	{
		unimplemented_instruction("long mode: 0F 8C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8D:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F8D(imm);
	}
	break;
	case 0x18D:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F8D(imm);
	}
	break;
	case 0x28D:
	{
		unimplemented_instruction("long mode: 0F 8D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8E:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F8E(imm);
	}
	break;
	case 0x18E:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F8E(imm);
	}
	break;
	case 0x28E:
	{
		unimplemented_instruction("long mode: 0F 8E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x8F:
	{
		int32_t imm = read_imm16();
		if (cpu->pagefault) return;
		instr16_0F8F(imm);
	}
	break;
	case 0x18F:
	{
		int32_t imm = read_imm32s();
		if (cpu->pagefault) return;
		instr32_0F8F(imm);
	}
	break;
	case 0x28F:
	{
		unimplemented_instruction("long mode: 0F 8F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x90:
	case 0x190:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F90_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F90_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x290:
	{
		unimplemented_instruction("long mode: 0F 90 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x91:
	case 0x191:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F91_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F91_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x291:
	{
		unimplemented_instruction("long mode: 0F 91 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x92:
	case 0x192:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F92_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F92_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x292:
	{
		unimplemented_instruction("long mode: 0F 92 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x93:
	case 0x193:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F93_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F93_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x293:
	{
		unimplemented_instruction("long mode: 0F 93 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x94:
	case 0x194:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F94_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F94_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x294:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0F94_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0F94_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x95:
	case 0x195:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F95_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F95_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x295:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0F95_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0F95_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x96:
	case 0x196:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F96_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F96_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x296:
	{
		unimplemented_instruction("long mode: 0F 96 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x97:
	case 0x197:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F97_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F97_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x297:
	{
		unimplemented_instruction("long mode: 0F 97 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x98:
	case 0x198:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F98_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F98_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x298:
	{
		unimplemented_instruction("long mode: 0F 98 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x99:
	case 0x199:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F99_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F99_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x299:
	{
		unimplemented_instruction("long mode: 0F 99 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9A:
	case 0x19A:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F9A_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F9A_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x29A:
	{
		unimplemented_instruction("long mode: 0F 9A has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9B:
	case 0x19B:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F9B_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F9B_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x29B:
	{
		unimplemented_instruction("long mode: 0F 9B has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9C:
	case 0x19C:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F9C_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F9C_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x29C:
	{
		unimplemented_instruction("long mode: 0F 9C has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9D:
	case 0x19D:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F9D_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F9D_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x29D:
	{
		unimplemented_instruction("long mode: 0F 9D has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9E:
	case 0x19E:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F9E_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F9E_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x29E:
	{
		unimplemented_instruction("long mode: 0F 9E has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0x9F:
	case 0x19F:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0F9F_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0F9F_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x29F:
	{
		unimplemented_instruction("long mode: 0F 9F has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA0:
	{
		instr16_0FA0();
	}
	break;
	case 0x1A0:
	{
		instr32_0FA0();
	}
	break;
	case 0x2A0:
	{
		unimplemented_instruction("long mode: 0F A0 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA1:
	{
		instr16_0FA1();
		after_block_boundary();
	}
	break;
	case 0x1A1:
	{
		instr32_0FA1();
		after_block_boundary();
	}
	break;
	case 0x2A1:
	{
		unimplemented_instruction("long mode: 0F A1 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA2:
	case 0x1A2:
	{
		instr_0FA2();
	}
	break;
	case 0x2A2:
	{
		unimplemented_instruction("long mode: 0F A2 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FA3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FA3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1A3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FA3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FA3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2A3:
	{
		unimplemented_instruction("long mode: 0F A3 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr16_0FA4_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr16_0FA4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x1A4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr32_0FA4_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr32_0FA4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x2A4:
	{
		unimplemented_instruction("long mode: 0F A4 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FA5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FA5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1A5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FA5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FA5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2A5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FA5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FA5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xA6:
	case 0x1A6:
	{
		instr_0FA6();
		after_block_boundary();
	}
	break;
	case 0x2A6:
	{
		unimplemented_instruction("long mode: 0F A6 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA7:
	case 0x1A7:
	{
		instr_0FA7();
		after_block_boundary();
	}
	break;
	case 0x2A7:
	{
		unimplemented_instruction("long mode: 0F A7 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA8:
	{
		instr16_0FA8();
	}
	break;
	case 0x1A8:
	{
		instr32_0FA8();
	}
	break;
	case 0x2A8:
	{
		unimplemented_instruction("long mode: 0F A8 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xA9:
	{
		instr16_0FA9();
		after_block_boundary();
	}
	break;
	case 0x1A9:
	{
		instr32_0FA9();
		after_block_boundary();
	}
	break;
	case 0x2A9:
	{
		unimplemented_instruction("long mode: 0F A9 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAA:
	case 0x1AA:
	{
		instr_0FAA();
	}
	break;
	case 0x2AA:
	{
		unimplemented_instruction("long mode: 0F AA has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FAB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FAB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1AB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FAB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FAB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2AB:
	{
		unimplemented_instruction("long mode: 0F AB has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr16_0FAC_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr16_0FAC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x1AC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr32_0FAC_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
		}
		else {
			int32_t imm = read_imm8();
			if (cpu->pagefault) return;
			instr32_0FAC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
		}
	}
	break;
	case 0x2AC:
	{
		unimplemented_instruction("long mode: 0F AC has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FAD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FAD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1AD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FAD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FAD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2AD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FAD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FAD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xAE:
	case 0x1AE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 0:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_0_mem(addr);
			}
			else {
				instr_0FAE_0_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 1:
		{
			if (!task_switch_test()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_1_mem(addr);
			}
			else {
				instr_0FAE_1_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 2:
		{
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_2_mem(addr);
			}
			else {
				instr_0FAE_2_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 3:
		{
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_3_mem(addr);
			}
			else {
				instr_0FAE_3_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_4_mem(addr);
			}
			else {
				instr_0FAE_4_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_5_mem(addr);
			}
			else {
				instr_0FAE_5_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_6_mem(addr);
			}
			else {
				instr_0FAE_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FAE_7_mem(addr);
			}
			else {
				instr_0FAE_7_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2AE:
	{
		unimplemented_instruction("long mode: 0F AE has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xAF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FAF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FAF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1AF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FAF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FAF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2AF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FAF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FAF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xB0:
	case 0x1B0:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0FB0_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0FB0_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x2B0:
	{
		unimplemented_instruction("long mode: 0F B0 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xB1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FB1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FB1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1B1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FB1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FB1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2B1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FB1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FB1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xB2:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FB2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FB2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x1B2:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FB2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FB2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x2B2:
	{
		unimplemented_instruction("long mode: 0F B2 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xB3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FB3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FB3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1B3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FB3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FB3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2B3:
	{
		unimplemented_instruction("long mode: 0F B3 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xB4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FB4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FB4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x1B4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FB4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FB4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x2B4:
	{
		unimplemented_instruction("long mode: 0F B4 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xB5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FB5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FB5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x1B5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FB5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FB5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x2B5:
	{
		unimplemented_instruction("long mode: 0F B5 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xB6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FB6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FB6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1B6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FB6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FB6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2B6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FB6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FB6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xB7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FB7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FB7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1B7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FB7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FB7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2B7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FB7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FB7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xB8:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F3) != 0) {
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_F30FB8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr16_F30FB8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0FB8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr16_0FB8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x1B8:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_F3) != 0) {
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_F30FB8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr32_F30FB8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0FB8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr32_0FB8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x2B8:
	{
		unimplemented_instruction("long mode: 0F B8 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xB9:
	case 0x1B9:
	{
		instr_0FB9();
		after_block_boundary();
	}
	break;
	case 0x2B9:
	{
		unimplemented_instruction("long mode: 0F B9 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xBA:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr16_0FBA_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1BA:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_4_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_5_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_6_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_7_mem(addr, imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr32_0FBA_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2BA:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 4:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_4_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_4_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 5:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_5_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_5_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_6_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_6_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		case 7:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_7_mem(addr, imm);
			}
			else {
				int64_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr64_0FBA_7_reg(reg_index_b(modrm_byte & 7), imm);
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xBB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FBB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FBB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1BB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FBB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FBB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2BB:
	{
		unimplemented_instruction("long mode: 0F BB has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xBC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FBC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FBC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1BC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FBC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FBC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2BC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FBC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FBC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xBD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FBD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FBD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1BD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FBD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FBD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2BD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FBD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FBD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xBE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FBE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FBE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1BE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FBE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FBE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2BE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FBE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FBE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xBF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FBF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FBF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1BF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FBF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FBF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2BF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FBF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FBF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xC0:
	case 0x1C0:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0FC0_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0FC0_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
		after_block_boundary();
	}
	break;
	case 0x2C0:
	{
		unimplemented_instruction("long mode: 0F C0 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr16_0FC1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr16_0FC1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x1C1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr32_0FC1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr32_0FC1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2C1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int64_t addr = modrm_resolve64(modrm_byte);
			if (cpu->pagefault) return;
			instr64_0FC1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr64_0FC1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0xC2:
	case 0x1C2:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC2_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F20FC2_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F20FC2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F30FC2_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_F30FC2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC2_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
	}
	break;
	case 0x2C2:
	{
		unimplemented_instruction("long mode: 0F C2 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC3:
	case 0x1C3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		if (modrm_byte < 0xC0) {
			int32_t addr = modrm_resolve(modrm_byte);
			if (cpu->pagefault) return;
			instr_0FC3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
		}
		else {
			instr_0FC3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
		}
	}
	break;
	case 0x2C3:
	{
		unimplemented_instruction("long mode: 0F C3 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC4:
	case 0x1C4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC4_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC4_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
	}
	break;
	case 0x2C4:
	{
		unimplemented_instruction("long mode: 0F C4 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC5:
	case 0x1C5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC5_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC5_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
	}
	break;
	case 0x2C5:
	{
		unimplemented_instruction("long mode: 0F C5 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC6:
	case 0x1C6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC6_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_660FC6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC6_mem(addr, reg_index_r(modrm_byte >> 3 & 7), imm);
			}
			else {
				int32_t imm = read_imm8();
				if (cpu->pagefault) return;
				instr_0FC6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7), imm);
			}
		}
	}
	break;
	case 0x2C6:
	{
		unimplemented_instruction("long mode: 0F C6 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xC7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0FC7_1_mem(addr);
			}
			else {
				instr16_0FC7_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr16_0FC7_6_mem(addr);
			}
			else {
				instr16_0FC7_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x1C7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0FC7_1_mem(addr);
			}
			else {
				instr32_0FC7_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr32_0FC7_6_mem(addr);
			}
			else {
				instr32_0FC7_6_reg(reg_index_b(modrm_byte & 7));
			}
			after_block_boundary();
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0x2C7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		switch (modrm_byte >> 3 & 7) {
		case 1:
		{
			if (modrm_byte < 0xC0) {
				int64_t addr = modrm_resolve64(modrm_byte);
				if (cpu->pagefault) return;
				instr64_0FC7_1_mem(addr);
			}
			else {
				instr64_0FC7_1_reg(reg_index_b(modrm_byte & 7));
			}
		}
			break;
		case 6:
		{
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_0FC7_6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_0FC7_6_reg");
				return;
			}
		}
			break;
		default:
			trigger_ud();
			break;
		}
	}
	break;
	case 0xC8:
	case 0x1C8:
	{
		instr_0FC8();
	}
	break;
	case 0x2C8:
	{
		instr64_0FC8();
	}
	break;
	case 0xC9:
	case 0x1C9:
	{
		instr_0FC9();
	}
	break;
	case 0x2C9:
	{
		instr64_0FC9();
	}
	break;
	case 0xCA:
	case 0x1CA:
	{
		instr_0FCA();
	}
	break;
	case 0x2CA:
	{
		instr64_0FCA();
	}
	break;
	case 0xCB:
	case 0x1CB:
	{
		instr_0FCB();
	}
	break;
	case 0x2CB:
	{
		instr64_0FCB();
	}
	break;
	case 0xCC:
	case 0x1CC:
	{
		instr_0FCC();
	}
	break;
	case 0x2CC:
	{
		instr64_0FCC();
	}
	break;
	case 0xCD:
	case 0x1CD:
	{
		instr_0FCD();
	}
	break;
	case 0x2CD:
	{
		instr64_0FCD();
	}
	break;
	case 0xCE:
	case 0x1CE:
	{
		instr_0FCE();
	}
	break;
	case 0x2CE:
	{
		instr64_0FCE();
	}
	break;
	case 0xCF:
	case 0x1CF:
	{
		instr_0FCF();
	}
	break;
	case 0x2CF:
	{
		instr64_0FCF();
	}
	break;
	case 0xD0:
	case 0x1D0:
	{
		if (!task_switch_test_mmx()) {
			return;
		}
		instr_0FD0();
		after_block_boundary();
	}
	break;
	case 0x2D0:
	{
		unimplemented_instruction("long mode: 0F D0 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD1:
	case 0x1D1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D1:
	{
		unimplemented_instruction("long mode: 0F D1 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD2:
	case 0x1D2:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D2:
	{
		unimplemented_instruction("long mode: 0F D2 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD3:
	case 0x1D3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D3:
	{
		unimplemented_instruction("long mode: 0F D3 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD4:
	case 0x1D4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D4:
	{
		unimplemented_instruction("long mode: 0F D4 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD5:
	case 0x1D5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D5:
	{
		unimplemented_instruction("long mode: 0F D5 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD6:
	case 0x1D6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20FD6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20FD6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30FD6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30FD6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x2D6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_660FD6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_660FD6_reg");
				return;
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_F20FD6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_F20FD6_reg");
				return;
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_F30FD6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_F30FD6_reg");
				return;
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				unimplemented_instruction("instructions_0f::instr64_0FD6_mem");
				return;
			}
			else {
				unimplemented_instruction("instructions_0f::instr64_0FD6_reg");
				return;
			}
		}
	}
	break;
	case 0xD7:
	case 0x1D7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D7:
	{
		unimplemented_instruction("long mode: 0F D7 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD8:
	case 0x1D8:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D8:
	{
		unimplemented_instruction("long mode: 0F D8 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xD9:
	case 0x1D9:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FD9_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FD9_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FD9_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FD9_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2D9:
	{
		unimplemented_instruction("long mode: 0F D9 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDA:
	case 0x1DA:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FDA_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FDA_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FDA_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FDA_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2DA:
	{
		unimplemented_instruction("long mode: 0F DA has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDB:
	case 0x1DB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FDB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FDB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FDB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FDB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2DB:
	{
		unimplemented_instruction("long mode: 0F DB has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDC:
	case 0x1DC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FDC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FDC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FDC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FDC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2DC:
	{
		unimplemented_instruction("long mode: 0F DC has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDD:
	case 0x1DD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FDD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FDD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FDD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FDD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2DD:
	{
		unimplemented_instruction("long mode: 0F DD has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDE:
	case 0x1DE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FDE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FDE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FDE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FDE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2DE:
	{
		unimplemented_instruction("long mode: 0F DE has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xDF:
	case 0x1DF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FDF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FDF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FDF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FDF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2DF:
	{
		unimplemented_instruction("long mode: 0F DF has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE0:
	case 0x1E0:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE0_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE0_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE0_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE0_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E0:
	{
		unimplemented_instruction("long mode: 0F E0 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE1:
	case 0x1E1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E1:
	{
		unimplemented_instruction("long mode: 0F E1 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE2:
	case 0x1E2:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E2:
	{
		unimplemented_instruction("long mode: 0F E2 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE3:
	case 0x1E3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E3:
	{
		unimplemented_instruction("long mode: 0F E3 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE4:
	case 0x1E4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E4:
	{
		unimplemented_instruction("long mode: 0F E4 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE5:
	case 0x1E5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E5:
	{
		unimplemented_instruction("long mode: 0F E5 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE6:
	case 0x1E6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F2) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F20FE6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F20FE6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else if ((prefixes_ & PREFIX_F3) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_F30FE6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_F30FE6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x2E6:
	{
		unimplemented_instruction("long mode: 0F E6 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE7:
	case 0x1E7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
			after_block_boundary();
		}
	}
	break;
	case 0x2E7:
	{
		unimplemented_instruction("long mode: 0F E7 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE8:
	case 0x1E8:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E8:
	{
		unimplemented_instruction("long mode: 0F E8 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xE9:
	case 0x1E9:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FE9_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FE9_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FE9_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FE9_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2E9:
	{
		unimplemented_instruction("long mode: 0F E9 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xEA:
	case 0x1EA:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FEA_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FEA_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FEA_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FEA_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2EA:
	{
		unimplemented_instruction("long mode: 0F EA has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xEB:
	case 0x1EB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FEB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FEB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FEB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FEB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2EB:
	{
		unimplemented_instruction("long mode: 0F EB has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xEC:
	case 0x1EC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FEC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FEC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FEC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FEC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2EC:
	{
		unimplemented_instruction("long mode: 0F EC has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xED:
	case 0x1ED:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FED_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FED_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FED_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FED_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2ED:
	{
		unimplemented_instruction("long mode: 0F ED has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xEE:
	case 0x1EE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FEE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FEE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FEE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FEE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2EE:
	{
		unimplemented_instruction("long mode: 0F EE has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xEF:
	case 0x1EF:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FEF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FEF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FEF_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FEF_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2EF:
	{
		unimplemented_instruction("long mode: 0F EF has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF0:
	case 0x1F0:
	{
		if (!task_switch_test_mmx()) {
			return;
		}
		instr_0FF0();
		after_block_boundary();
	}
	break;
	case 0x2F0:
	{
		unimplemented_instruction("long mode: 0F F0 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF1:
	case 0x1F1:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF1_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF1_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F1:
	{
		unimplemented_instruction("long mode: 0F F1 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF2:
	case 0x1F2:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF2_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF2_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F2:
	{
		unimplemented_instruction("long mode: 0F F2 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF3:
	case 0x1F3:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF3_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF3_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F3:
	{
		unimplemented_instruction("long mode: 0F F3 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF4:
	case 0x1F4:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF4_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF4_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F4:
	{
		unimplemented_instruction("long mode: 0F F4 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF5:
	case 0x1F5:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF5_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF5_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F5:
	{
		unimplemented_instruction("long mode: 0F F5 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF6:
	case 0x1F6:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF6_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF6_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F6:
	{
		unimplemented_instruction("long mode: 0F F6 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF7:
	case 0x1F7:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF7_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF7_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F7:
	{
		unimplemented_instruction("long mode: 0F F7 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF8:
	case 0x1F8:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF8_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF8_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F8:
	{
		unimplemented_instruction("long mode: 0F F8 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xF9:
	case 0x1F9:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FF9_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FF9_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FF9_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FF9_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2F9:
	{
		unimplemented_instruction("long mode: 0F F9 has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFA:
	case 0x1FA:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FFA_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FFA_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FFA_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FFA_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2FA:
	{
		unimplemented_instruction("long mode: 0F FA has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFB:
	case 0x1FB:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FFB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FFB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FFB_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FFB_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2FB:
	{
		unimplemented_instruction("long mode: 0F FB has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFC:
	case 0x1FC:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FFC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FFC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FFC_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FFC_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2FC:
	{
		unimplemented_instruction("long mode: 0F FC has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFD:
	case 0x1FD:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FFD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FFD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FFD_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FFD_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2FD:
	{
		unimplemented_instruction("long mode: 0F FD has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFE:
	case 0x1FE:
	{
		int32_t modrm_byte = read_imm8();
		if (cpu->pagefault) return;
		uint8_t prefixes_ = cpu->prefixes;
		if ((prefixes_ & PREFIX_66) != 0) {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_660FFE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_660FFE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
		else {
			if (!task_switch_test_mmx()) {
				return;
			}
			if (modrm_byte < 0xC0) {
				int32_t addr = modrm_resolve(modrm_byte);
				if (cpu->pagefault) return;
				instr_0FFE_mem(addr, reg_index_r(modrm_byte >> 3 & 7));
			}
			else {
				instr_0FFE_reg(reg_index_b(modrm_byte & 7), reg_index_r(modrm_byte >> 3 & 7));
			}
		}
	}
	break;
	case 0x2FE:
	{
		unimplemented_instruction("long mode: 0F FE has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	case 0xFF:
	case 0x1FF:
	{
		instr_0FFF();
		after_block_boundary();
	}
	break;
	case 0x2FF:
	{
		unimplemented_instruction("long mode: 0F FF has no 64-bit arm in gen/x86_table64.js");
		return;
	}
	break;
	default:
		DBG_ASSERT(false);
		break;
	}
}
