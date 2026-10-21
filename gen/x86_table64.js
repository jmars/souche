/*
 * gen/x86_table64.js -- THE 64-BIT OVERLAY: the authored, SPARSE declaration of which
 * encodings have a long-mode arm, and which of them CHANGE MEANING in long mode.
 *
 * DESIGN OF RECORD: results/x86-64-plan.md ss3.6 (three candidates, one chosen), ss2.1.6
 * (the corpus is generated from a declaration written from the SDM's opcode map, NOT from the
 * bodies), ss6's S5 row (this slice), and results/x86-64-s5.md ss2 (what it landed).
 *
 * ------------------------------------------------------------------ *
 * WHY AN OVERLAY AND NOT A SECOND TABLE
 *
 * The dispatch in this port is GENERATED from v86's 32-bit x86_table.js (docs/architecture.md
 * ss7: the dispatch is generated, the semantics are not).  Long mode does not re-define 873
 * encodings; it re-defines a few, and adds a third OPERAND-SIZE ARM to the ones that have a
 * 64-bit form.  So this file lists only what long mode touches, keyed by the base opcode, and
 * gen/interpreter_c.js emits from it:
 *
 *   - a `case 0x2NN:` arm beside the existing `0xNN`/`0x1NN` pair, routing os_index == 2 (the
 *     REX.W operand size) to `instr64_*` bodies -- which are HAND-WRITTEN, like every other
 *     semantic body in this tree;
 *   - the REX prefix cases (0x40-0x4F), which are INC/DEC at 32 bits and a PREFIX in long
 *     mode.  They cannot be "another arm": the byte must be consumed and the stream
 *     re-dispatched, so all three of their case labels route to the prefix handler.
 *
 * AN ENCODING WITH NO ENTRY HERE gets no 0x2NN arm and no guard, so `dispatch_os_index` can
 * never select a 64-bit arm for it.  WHAT HAPPENS THEN DEPENDS ON THE OPERAND SIZE, and the
 * first version of this header claimed more than the file can deliver (round-2 finding 2):
 *
 *   - WITH REX.W the key is 2, the switch has no `case 0x2NN` for it, and the emitter's
 *     NAMED-STOP case fires ("long mode: 0xNN has no 64-bit form in gen/x86_table64.js").
 *     That is the no-fall-through guarantee.
 *   - WITHOUT REX.W the key is 0 or 1 and the instruction runs its 32-BIT ARM.  That is
 *     CORRECT for every encoding whose 64-bit behaviour does not differ (the whole reason the
 *     mov family's non-REX forms are green), and it is SILENT-WRONG for the ones where
 *     "different" is the point -- which is why `invalid64` and `stop64` exist and why their
 *     guard is emitted on the 0xNN and 0x1NN arms too.  An encoding that changes MEANING in
 *     64-bit mode (0x63, 0xE3) or ceases to exist (the SDM's removal list) MUST be listed
 *     here; an encoding that merely gains a 64-bit form need not be.
 *
 * The census the generator prints is how many of the port's encodings have an arm -- "how much
 * of long mode is live" as a measurement, not a mood.
 *
 * ------------------------------------------------------------------ *
 * THE FOUR KINDS OF ENTRY
 *
 *   { opcode }                     the encoding has a 64-bit form whose arguments the emitter
 *                                  can thread itself (the same modrm/immediate shape as the
 *                                  32-bit one) -> emit `case 0x2NN:` calling `instr64_<name>`.
 *                                  The NAME is derived exactly as the 16/32-bit ones are
 *                                  (`make_instruction_name`'s size field), which is why a body
 *                                  is `instr64_89_mem`, `instr64_C7_0_reg` and so on.
 *
 *   { opcode, entry64: "NAME" }    the 64-bit form's BODY has a different name from the
 *                                  derived one (a hand-written entry point that does not fit
 *                                  the shape), and the emitter calls NAME with the same
 *                                  arguments as its 32-bit sibling.
 *
 *   { opcode, prefix64: true }     the opcode is a PREFIX in long mode (0x40-0x4F = REX).  All
 *                                  three case labels route to the prefix handler with the
 *                                  byte; the 16/32-bit INC/DEC arms are untouched.
 *
 *   { opcode, invalid64: true }    the encoding is INVALID in long mode (0x9A/0xEA far call/jmp
 *                                  immediate, 0xC4/0xC5 VEX).  The 0x2NN arm raises #UD.
 *
 * EVERY entry may also carry:
 *
 *   default64: true                the encoding's operand size in long mode is 64 even with no
 *                                  REX.W -- push/pop/pushf/popf/call/ret/jmp/near-ret and
 *                                  MOVSXD (SDM Vol 2's "default 64-bit operand size" group).
 *                                  The emitter puts these in one generated predicate,
 *                                  `long_mode_default64(opcode)`, which the dispatch key's
 *                                  os_index computation consults -- the opcode is known there,
 *                                  and it is the ONLY thing that can know it.
 *
 *                                  WITHOUT THIS, A 64-BIT PUSH WOULD BE A SILENT 32-BIT ONE:
 *                                  `push r0` is 0x50 with no REX, so os_index would be 1 and
 *                                  the 32-bit body would push four bytes and keep the stream
 *                                  in sync -- the exact "wrong length, no symptom" class this
 *                                  slice exists to avoid.
 *
 * ------------------------------------------------------------------ *
 * WHAT IS DELIBERATELY ABSENT (each one a NAMED STOP until its slice, so the gap is never a
 * silently-executed 32-bit instruction)
 *
 *   0x63 MOVSXD          S7 (control flow / movsxd / movzx / movsx / lea)
 *   0x69/0x6B IMUL       S8
 *   0x80-0x83, 0x84/85, 0x86/87, ALU groups   S6
 *   0x8D LEA             S7
 *   0x98/0x99 CWDE/CDQ   S7
 *   0xA4-0xAF string ops S9
 *   0xC0/0xC1, 0xD0-0xD3 shifts             S8
 *   0xC2/0xC3/0xCA/0xCB/0xCC/0xCD/0xCF      S7 (ret/iret in 64-bit mode pop 8 bytes)
 *   0xE8 CALL, 0xE9 JMP, 0x70-0x7F Jcc, 0xF6/0xF7, 0xFE/0xFF   S7
 *   0x0F 05 SYSCALL and the whole 0F space  S11
 *   x87/MMX/SSE (0xD8-0xDF, 0x0F 10-17 ...) S10
 *
 * The list is the S6-S11 slices' own rows of results/x86-64-plan.md ss6, not an independent
 * judgement: a reader comparing the two should find them agree.
 */

/* ------------------------------------------------------------------ *
 * THE 0F-QUALIFIED 64-BIT ARMS (S7).  Keyed by the escape's SECOND byte; the emitter puts them
 * in the generated 0F switch as `case 0x2NN:` arms (the same three-valued key), reachable only
 * through instructions_64.c's instr64_0F, which keys the second byte with
 * dispatch_os_index_0f.  NOTHING ELSE in the 0F space has a 64-bit arm: an unlisted second byte
 * gets NO 0x2NN case and dispatch_os_index_0f therefore never selects one -- the 0F space's
 * 16/32-bit arms run exactly as they always did.
 *
 * The corpus's ctrl-* cases use cmovcc (44/45), setcc (94/95) and movzx/movsx (B6/B7/BE/BF);
 * every other second byte is a NAMED STOP in this build.
 * ------------------------------------------------------------------ */
const x86_table64_0f = [
	{ opcode: 0x44 }, /* cmove r64, r/m64 */
	{ opcode: 0x45 }, /* cmovne r64, r/m64 */
	{ opcode: 0x94 }, /* sete r/m8 */
	{ opcode: 0x95 }, /* setne r/m8 */
	{ opcode: 0xB6 }, /* movzx r64, r/m8 */
	{ opcode: 0xB7 }, /* movzx r64, r/m16 */
	{ opcode: 0xBE }, /* movsx r64, r/m8 */
	{ opcode: 0xBF }, /* movsx r64, r/m16 */
	/* S8: the 0F-qualified arithmetic/bit family (family 4's cases) */
	{ opcode: 0xAF }, /* imul r64, r/m64 */
	{ opcode: 0xA5 }, /* shld r/m64, r64, cl */
	{ opcode: 0xAD }, /* shrd r/m64, r64, cl */
	{ opcode: 0xBC }, /* bsf r64, r/m64 */
	{ opcode: 0xBD }, /* bsr r64, r/m64 */
	{ opcode: 0xBA }, /* the bt/bts/btr/btc imm8 group */
	{ opcode: 0xC8 }, /* bswap r64 (0F C8+r: eight opcodes, one body each) */
	{ opcode: 0xC9 },
	{ opcode: 0xCA },
	{ opcode: 0xCB },
	{ opcode: 0xCC },
	{ opcode: 0xCD },
	{ opcode: 0xCE },
	{ opcode: 0xCF },
];

const x86_table64 = [
	/* ---------------- 0x40-0x4F: REX, not INC/DEC ---------------- */
	{ opcode: 0x40, prefix64: true },
	{ opcode: 0x41, prefix64: true },
	{ opcode: 0x42, prefix64: true },
	{ opcode: 0x43, prefix64: true },
	{ opcode: 0x44, prefix64: true },
	{ opcode: 0x45, prefix64: true },
	{ opcode: 0x46, prefix64: true },
	{ opcode: 0x47, prefix64: true },
	{ opcode: 0x48, prefix64: true },
	{ opcode: 0x49, prefix64: true },
	{ opcode: 0x4A, prefix64: true },
	{ opcode: 0x4B, prefix64: true },
	{ opcode: 0x4C, prefix64: true },
	{ opcode: 0x4D, prefix64: true },
	{ opcode: 0x4E, prefix64: true },
	{ opcode: 0x4F, prefix64: true },

	/* ---------------- S7: CONTROL FLOW AND STACK AT 64 BITS ---------------- */
	/* 0x0F itself: in 64-bit mode the escape's SECOND byte is keyed by the same three-valued
	 * rule (modrm.c's dispatch_os_index_0f), and this entry is what makes the base switch's
	 * `case 0x20F` exist and call the 64-bit escape handler (instructions_64.c's instr64_0F).
	 * The 0F-qualified arms themselves are listed in the second table below. */
	{ opcode: 0x0F, entry64: "instr64_0F" },

	/* jcc rel8 (0x70-0x7F).  The 16 conditions are one line each over the port's OWN test_*
	 * predicates (misc_instr.h), and the target arithmetic is 64-bit: `ip + sign_extend(rel8)`.
	 * jcc rel32 (0F 80-8F) is NOT here -- no corpus case uses the rel32 form, so it stays a
	 * named stop (results/x86-64-s7.md ss6). */
	{ opcode: 0x70 }, { opcode: 0x71 }, { opcode: 0x72 }, { opcode: 0x73 },
	{ opcode: 0x74 }, { opcode: 0x75 }, { opcode: 0x76 }, { opcode: 0x77 },
	{ opcode: 0x78 }, { opcode: 0x79 }, { opcode: 0x7A }, { opcode: 0x7B },
	{ opcode: 0x7C }, { opcode: 0x7D }, { opcode: 0x7E }, { opcode: 0x7F },

	{ opcode: 0x63, narrow64: "0x63 without REX.W is MOVSXD at 16 or 32 bits, which is not"
	                          + " implemented (ARPL does not exist in 64-bit mode)" }, /* movsxd r64, r/m32 (S7) */
	{ opcode: 0x87 },  /* xchg r/m64, r64 (S7) */
	{ opcode: 0x8D },  /* lea r64, m (S7; custom_modrm_resolve, like the 32-bit body) */
	/* 0xC3 / 0xE8 / 0xE9 are ALREADY here as default-64 entries with no body (S5's shutter);
	 * S7 gives them their bodies, and 0xC2 / 0xCA / 0xCB / 0xCF stay named stops. */

	/* ------------------------------------------------------------------ *
	 * INVALID IN 64-BIT MODE: THE SDM'S OWN REMOVAL LIST
	 *
	 * Each of these is an encoding the SDM REMOVES in 64-bit mode (`#UD`), and each has a
	 * live 32-bit arm in this port -- so without an entry here a bare byte in a 64-bit CS
	 * would execute the 32-bit instruction.  The guard is applied to the 0xNN and 0x1NN arms
	 * as well as the 0x2NN one, because `dispatch_os_index` only answers 2 for these when a
	 * REX.W precedes them (round-2 finding 2, MEASURED: a bare `EA` reached `case 0x1EA`, the
	 * real far jump, where the SDM says #UD).
	 *
	 * The list is the SDM's, not a guess: 06/07/0E/16/17/1E/1F (PUSH/POP Sreg), 27/2F/37/3F
	 * (DAA/DAS/AAA/AAS -- the BCD group, absent from 64-bit mode), 60/61 (PUSHA/POPA), 62
	 * (BOUND), 82 (the alias of the imm8 ALU group), 9A/EA (the ptr16:32 far transfers), C4/C5
	 * (LES/LDS, whose bytes are VEX in 64-bit mode), CE (INTO), D4/D5 (AAM/AAD), D6 (SALC).
	 * ------------------------------------------------------------------ */
	{ opcode: 0x06, invalid64: true, why64: "0x06 (push es) is invalid in 64-bit mode" },
	{ opcode: 0x07, invalid64: true, why64: "0x07 (pop es) is invalid in 64-bit mode" },
	{ opcode: 0x0E, invalid64: true, why64: "0x0E (push cs) is invalid in 64-bit mode" },
	{ opcode: 0x16, invalid64: true, why64: "0x16 (push ss) is invalid in 64-bit mode" },
	{ opcode: 0x17, invalid64: true, why64: "0x17 (pop ss) is invalid in 64-bit mode" },
	{ opcode: 0x1E, invalid64: true, why64: "0x1E (push ds) is invalid in 64-bit mode" },
	{ opcode: 0x1F, invalid64: true, why64: "0x1F (pop ds) is invalid in 64-bit mode" },
	{ opcode: 0x27, invalid64: true, why64: "0x27 (daa) is invalid in 64-bit mode" },
	{ opcode: 0x2F, invalid64: true, why64: "0x2F (das) is invalid in 64-bit mode" },
	{ opcode: 0x37, invalid64: true, why64: "0x37 (aaa) is invalid in 64-bit mode" },
	{ opcode: 0x3F, invalid64: true, why64: "0x3F (aas) is invalid in 64-bit mode" },
	{ opcode: 0x60, invalid64: true, why64: "0x60 (pusha) is invalid in 64-bit mode" },
	{ opcode: 0x61, invalid64: true, why64: "0x61 (popa) is invalid in 64-bit mode" },
	{ opcode: 0x62, invalid64: true, why64: "0x62 (bound) is invalid in 64-bit mode" },
	{ opcode: 0x82, invalid64: true, why64: "0x82 is invalid in 64-bit mode" },
	{ opcode: 0x9A, invalid64: true, why64: "0x9A (call far ptr16:32) is invalid in 64-bit mode" },
	{ opcode: 0xC4, invalid64: true, why64: "0xC4 (les) is VEX-encoded in 64-bit mode, and VEX is not implemented" },
	{ opcode: 0xC5, invalid64: true, why64: "0xC5 (lds) is VEX-encoded in 64-bit mode, and VEX is not implemented" },
	{ opcode: 0xCE, invalid64: true, why64: "0xCE (into) is invalid in 64-bit mode" },
	{ opcode: 0xD4, invalid64: true, why64: "0xD4 (aam) is invalid in 64-bit mode" },
	{ opcode: 0xD5, invalid64: true, why64: "0xD5 (aad) is invalid in 64-bit mode" },
	{ opcode: 0xD6, invalid64: true, why64: "0xD6 (salc) is invalid in 64-bit mode" },
	{ opcode: 0xEA, invalid64: true, why64: "0xEA (jmp far ptr16:32) is invalid in 64-bit mode" },

	/* ------------------------------------------------------------------ *
	 * VALID IN 64-BIT MODE, BUT WITH DIFFERENT SEMANTICS: a NAMED STOP, NOT a #UD
	 *
	 * 0x63 is MOVSXD there and ARPL does not exist (the length is the same, so this is a
	 * silent-wrong-answer, not a desync); 0xE3 is JRCXZ, whose counter is RCX rather than
	 * ECX.  Both are S7's (control flow), so they stop by name -- the answer that keeps the
	 * machine honest without pretending the encoding is illegal.
	 * ------------------------------------------------------------------ */

	{ opcode: 0xE3, stop64: "0xE3 is JRCXZ in 64-bit mode (RCX, not ECX), and the jcc family is S7's" },

	/* ---------------- MOV, the S5 family ---------------- */
	/* 0x88/0x89/0x8A/0x8B: the r/m,r and r,r/m forms at 8/32/64.  The 64-bit bodies exist for
	 * BOTH widths: with REX.W the operand is 64-bit, and without it a 32-bit write must still
	 * zero-extend into the 64-bit register -- which the port's write_reg32 already does, so
	 * the 0x2NN arm is only emitted for the REX.W case and the 0x1NN arm keeps doing the
	 * zero-extension. */
	{ opcode: 0x88 }, /* mov r/m8, r8   (byte: REX changes the register, not the width) */
	{ opcode: 0x89 }, /* mov r/m, r */
	{ opcode: 0x8A }, /* mov r8, r/m8 */
	{ opcode: 0x8B }, /* mov r, r/m */
	{ opcode: 0xA0 }, /* mov AL/AX/EAX, moffs -- the OFFSET is 8 bytes in long mode whatever
	                   * the operand size is (read_moffs64), and the OFFSET is the only 8-byte
	                   * field here: the answer is AL/AX/EAX/RAX per the arm. */
	{ opcode: 0xA1 },
	{ opcode: 0xA2 },
	{ opcode: 0xA3 },
	/* MOV r, imm: B0-B7 (imm8), B8-BF (imm16/32, or imm64 with REX.W), C6/C7 (r/m, imm).  The
	 * 64-bit forms of B8+ and C7 /0 are the two places an immediate is EIGHT bytes wide
	 * (read_imm64). */
	{ opcode: 0xB0 },
	{ opcode: 0xB1 },
	{ opcode: 0xB2 },
	{ opcode: 0xB3 },
	{ opcode: 0xB4 },
	{ opcode: 0xB5 },
	{ opcode: 0xB6 },
	{ opcode: 0xB7 },
	/* B8-BF are the ONLY encodings whose immediate is EIGHT bytes wide in long mode
	 * (`imm64: true` -> read_imm64).  The flag is per-encoding and not a property of "64-bit
	 * operand size", which is the mistake this file made in its first version: `C7 /0` with
	 * REX.W is `mov r/m64, imm32` -- a FOUR-byte immediate SIGN-EXTENDED to 64 -- and reading
	 * eight bytes there desynchronises the stream by four with the instruction length still
	 * "balanced" in the decoder's eyes.  MEASURED on the first run of the mov family: rsp came
	 * out as 0x489d006a00102000 (the high half held the following instruction's bytes). */
	{ opcode: 0xB8, imm64: true },
	{ opcode: 0xB9, imm64: true },
	{ opcode: 0xBA, imm64: true },
	{ opcode: 0xBB, imm64: true },
	{ opcode: 0xBC, imm64: true },
	{ opcode: 0xBD, imm64: true },
	{ opcode: 0xBE, imm64: true },
	{ opcode: 0xBF, imm64: true },
	{ opcode: 0xC6 }, /* mov r/m8, imm8 */
	{ opcode: 0xC7 }, /* mov r/m, imm -- C7 /0 with REX.W is the sign-extended imm32 form */

	/* ---------------- XOR / ADD, the "mov/add subset"'s arithmetic ---------------- */
	/* S5's row says "a mov/add subset".  ADD (0x01/0x03) and XOR (0x31/0x33) are here because
	 * the corpus's SHARED PROLOGUE and every case body need them at 32 bits (the prologue's
	 * `xor eax,eax` and `xor r8d,r8d`) and the mov family's cases use `xor` at 64 bits; ADD is
	 * the row's own word and the S6 family's first three cases read `add` at 64 bits -- S6
	 * still owns adc/sbb/and/or/sub/test/cmp/inc/dec/neg/not and the WRAP bodies. */
	/* ---------------- S6: THE ALU FAMILY AT 64 BITS ---------------- *
	 * The plan's S6 row: add/adc/sub/sbb/and/or/xor/cmp/test/inc/dec/neg/not.  Every one of
	 * these DELIVERS its 64-bit result AND SETS THE ARITHMETIC FLAGS through the port's ONE
	 * set of flag cores at OPSIZE_64 (arith.h's note) -- the lazy operands, the canonical
	 * storage form and the getters are S1's, unchanged.
	 *
	 * THE ENCODINGS ARE THE ONES FAMILY 2 USES, and the rest are NAMED STOPS rather than
	 * implied: 0x81 (imm32 ALU group) and the accumulator-immediate forms 0x05/0x0D/0x15/
	 * 0x1D/0x25/0x2D/0x35/0x3D have no body here because no corpus case covers them, and an
	 * encoding with no instrument is not landed silently (results/x86-64-s6.md ss5 lists
	 * them).  0xF7's mul/imul/div/idiv are S8's, as are the shifts and rotates. */
	{ opcode: 0x01 }, /* add r/m64, r64 */
	{ opcode: 0x03 }, /* add r64, r/m64 */
	{ opcode: 0x09 }, /* or  r/m64, r64 */
	{ opcode: 0x0B }, /* or  r64, r/m64 */
	{ opcode: 0x11 }, /* adc r/m64, r64 */
	{ opcode: 0x13 }, /* adc r64, r/m64 */
	{ opcode: 0x19 }, /* sbb r/m64, r64 */
	{ opcode: 0x1B }, /* sbb r64, r/m64 */
	{ opcode: 0x21 }, /* and r/m64, r64 */
	{ opcode: 0x23 }, /* and r64, r/m64 */
	{ opcode: 0x29 }, /* sub r/m64, r64 */
	{ opcode: 0x2B }, /* sub r64, r/m64 */
	{ opcode: 0x31 }, /* xor r/m64, r64 */
	{ opcode: 0x33 }, /* xor r64, r/m64 */
	{ opcode: 0x39 }, /* cmp r/m64, r64 */
	{ opcode: 0x3B }, /* cmp r64, r/m64 */
	{ opcode: 0x83 }, /* the imm8 group: /0 add /1 or /2 adc /3 sbb /4 and /5 sub /6 xor /7 cmp */
	/* S8: the shift group (0xD1 by 1, 0xD3 by CL, 0xC1 by imm8) and the three-operand IMULs.
	 * RCL/RCR (/2,/3) stay NAMED STOPS: no corpus case, and their carry-in rule is its own
	 * instrument (results/x86-64-s8.md ss6). */
	{ opcode: 0xC1 },
	{ opcode: 0xD1 },
	{ opcode: 0xD3 },
	{ opcode: 0x69 }, /* imul r64, r/m64, imm32 */
	{ opcode: 0x6B }, /* imul r64, r/m64, imm8  */
	{ opcode: 0x85 }, /* test r/m64, r64 */
	{ opcode: 0xF7 }, /* the group: /2 not /3 neg (the rest are S8's) */

	/* ---------------- the stack and the stop, all default-64 ---------------- */
	{ opcode: 0x50, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* push r0 */
	{ opcode: 0x51, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x52, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x53, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x54, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x55, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x56, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x57, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x58, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* pop r0 */
	{ opcode: 0x59, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x5A, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x5B, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x5C, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x5D, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x5E, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x5F, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true },
	{ opcode: 0x68, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* push imm32 (sign-extended to 64 in long mode) */
	{ opcode: 0x6A, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* push imm8  (ditto) */
	{ opcode: 0x9C, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* pushf -> pushfq */
	{ opcode: 0x9D, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* popf  -> popfq */
	{ opcode: 0xF4 },                 /* hlt: no operand, but the corpus's epilogue needs it and
	                                   * its 32-bit body is mode-independent (cpu->in_hlt).  Here
	                                   * so the 0x2NN arm exists and the census counts it. */
	{ opcode: 0xEB, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* jmp rel8: the target arithmetic is 64-bit in long mode
	                                   * (the corpus's `jmp loop`) */

	/* ------------------------------------------------------------------ *
	 * DEFAULT-64 WITH NO BODY YET: THE SILENT-WRONG SHUTTER
	 *
	 * These encodings have a 64-BIT-DEFAULT operand size in long mode and no 64-bit arm in
	 * this slice, so their 0x2NN arm is emitted as a NAMED STOP per sub-opcode (instr64_*
	 * bodies that do not exist).  The alternative -- leaving them out of the overlay --
	 * would dispatch them to their 32-bit arms at os_index 1 and MIS-EXECUTE THEM SILENTLY
	 * with the length still balanced: `push qword ptr [mem]` (FF /6, no REX.W needed in 64-bit
	 * mode) would push FOUR bytes, `ret` would pop four, `call rel32` would push four.
	 *
	 * MEASURED as the reason this exists: `ctrl-push-pop-64` was the one MISMATCH left in the
	 * full-corpus run (green=17 red=39 mismatch=1) -- the 32-bit push ran and moved rsp by 4.
	 * The opcode-level flag is COARSE for the 0xFF group (/0 and /1 inc/dec are 32-bit
	 * default), and that coarseness is deliberate: it over-refuses (an `inc dword [mem]`
	 * stops by name instead of running), which is the safe direction, and S6/S7 replace it
	 * with per-sub-opcode arms.
	 * ------------------------------------------------------------------ */
	{ opcode: 0x8F, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* pop r/m64                 -- S7 */
	{ opcode: 0xC2, default64: true }, /* ret imm16                 -- S7 */
	{ opcode: 0xC3, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* ret                       -- S7 */
	{ opcode: 0xCA, default64: true }, /* retf imm16                -- S7/S11 */
	{ opcode: 0xCB, default64: true }, /* retf                      -- S7/S11 */
	{ opcode: 0xCF, default64: true }, /* iretd -> iretq            -- S11 */
	{ opcode: 0xE8, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* call rel32                -- S7 */
	{ opcode: 0xE9, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* jmp rel32                 -- S7 */
	{ opcode: 0xFF, narrow64: "the 16-bit operand form of a default-64 encoding in long mode is not implemented (its stack path runs through the 32-bit accessor, which would truncate RSP)", default64: true }, /* /0 inc and /1 dec are S6's bodies and /4 jmp r/m64
	                                   * is S7's; /2 call r/m, /3 callf, /5 jmpf and /6 push
	                                   * r/m stay NAMED STOPS -- automatic: the availability
	                                   * scan emits a stop for a sub-op with no body. */
];

export { x86_table64_0f };
export default x86_table64;
