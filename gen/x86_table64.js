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
 * AN ENCODING WITH NO ENTRY HERE HAS NO 64-BIT FORM IN THIS BUILD, and that is not a
 * fall-through: the emitter simply emits no 0x2NN arm, so `dispatch_os_index` can never
 * produce 2 for it (see the note on `default64` below) and the instruction is decoded with its
 * 32-bit body -- or, for the subset S5 does not own, with a body that STOPS BY NAME.  The
 * distinction matters: `movsxd` (0x63) has no entry YET and stops by name; `add` (0x01/0x03)
 * has one in S6.  The census the generator prints is how many of the port's encodings have an
 * arm -- "how much of long mode is live" as a measurement, not a mood.
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

	/* ---------------- invalid in long mode ---------------- */
	{ opcode: 0x9A, invalid64: true }, /* call far ptr16:32 -- the SDM makes it #UD in 64-bit */
	{ opcode: 0xEA, invalid64: true }, /* jmp far ptr16:32  -- ditto (the 32-bit mode form the
	                                    * corpus's stub uses runs in COMPATIBILITY mode, where it
	                                    * is still valid: that is plan correction GAP 2) */

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
	{ opcode: 0x01 }, /* add r/m, r */
	{ opcode: 0x03 }, /* add r, r/m */
	{ opcode: 0x31 }, /* xor r/m, r */
	{ opcode: 0x33 }, /* xor r, r/m */

	/* ---------------- the stack and the stop, all default-64 ---------------- */
	{ opcode: 0x50, default64: true }, /* push r0 */
	{ opcode: 0x51, default64: true },
	{ opcode: 0x52, default64: true },
	{ opcode: 0x53, default64: true },
	{ opcode: 0x54, default64: true },
	{ opcode: 0x55, default64: true },
	{ opcode: 0x56, default64: true },
	{ opcode: 0x57, default64: true },
	{ opcode: 0x58, default64: true }, /* pop r0 */
	{ opcode: 0x59, default64: true },
	{ opcode: 0x5A, default64: true },
	{ opcode: 0x5B, default64: true },
	{ opcode: 0x5C, default64: true },
	{ opcode: 0x5D, default64: true },
	{ opcode: 0x5E, default64: true },
	{ opcode: 0x5F, default64: true },
	{ opcode: 0x68, default64: true }, /* push imm32 (sign-extended to 64 in long mode) */
	{ opcode: 0x6A, default64: true }, /* push imm8  (ditto) */
	{ opcode: 0x9C, default64: true }, /* pushf -> pushfq */
	{ opcode: 0x9D, default64: true }, /* popf  -> popfq */
	{ opcode: 0xF4 },                 /* hlt: no operand, but the corpus's epilogue needs it and
	                                   * its 32-bit body is mode-independent (cpu->in_hlt).  Here
	                                   * so the 0x2NN arm exists and the census counts it. */
	{ opcode: 0xEB, default64: true }, /* jmp rel8: the target arithmetic is 64-bit in long mode
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
	{ opcode: 0x8F, default64: true }, /* pop r/m64                 -- S7 */
	{ opcode: 0xC2, default64: true }, /* ret imm16                 -- S7 */
	{ opcode: 0xC3, default64: true }, /* ret                       -- S7 */
	{ opcode: 0xCA, default64: true }, /* retf imm16                -- S7/S11 */
	{ opcode: 0xCB, default64: true }, /* retf                      -- S7/S11 */
	{ opcode: 0xCF, default64: true }, /* iretd -> iretq            -- S11 */
	{ opcode: 0xE8, default64: true }, /* call rel32                -- S7 */
	{ opcode: 0xE9, default64: true }, /* jmp rel32                 -- S7 */
	{ opcode: 0xFF, default64: true }, /* inc/dec/call/callf/jmp/jmpf/push -- S6/S7 */
];

export default x86_table64;
