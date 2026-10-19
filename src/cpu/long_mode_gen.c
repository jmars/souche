/*
 * long_mode_gen.c -- GENERATED FILE, DO NOT EDIT.
 *
 * Emitted by gen/interpreter_c.js from gen/x86_table64.js's `default64` flags.  It lives
 * in its OWN translation unit rather than in interpreter_gen.c because `dispatch_os_index`
 * (modrm.c) needs it and the SELFTESTS do not link the generated dispatcher -- they supply
 * their own `run_instruction`.  One definition, every link: the alternative (a stub beside
 * the selftest's run_instruction) would be a second implementation that could disagree
 * with the overlay, which is the class gen/x86_table64.js exists to make impossible.
 *
 * WHAT IT ANSWERS: which encodings's long-mode operand size is 64 with NO REX.W (the
 * SDM's default-64 group: push/pop/pushf/popf/call/ret/jmp).  30 of them.
 */

#include <stdbool.h>
#include <stdint.h>

bool long_mode_default64(int32_t opcode)
{
	switch (opcode) {
	case 0x50: return true;
	case 0x51: return true;
	case 0x52: return true;
	case 0x53: return true;
	case 0x54: return true;
	case 0x55: return true;
	case 0x56: return true;
	case 0x57: return true;
	case 0x58: return true;
	case 0x59: return true;
	case 0x5A: return true;
	case 0x5B: return true;
	case 0x5C: return true;
	case 0x5D: return true;
	case 0x5E: return true;
	case 0x5F: return true;
	case 0x68: return true;
	case 0x6A: return true;
	case 0x8F: return true;
	case 0x9C: return true;
	case 0x9D: return true;
	case 0xC2: return true;
	case 0xC3: return true;
	case 0xCA: return true;
	case 0xCB: return true;
	case 0xCF: return true;
	case 0xE8: return true;
	case 0xE9: return true;
	case 0xEB: return true;
	case 0xFF: return true;
	default: return false;
	}
}
