#!/usr/bin/env node
/*
 * gen/interpreter_c.js -- the C interpreter dispatcher generator.
 *
 * The C-port counterpart of v86/gen/generate_interpreter.js (497 lines), which emits
 * src/rust/gen/interpreter{,0f}.rs.  It reads v86/gen/x86_table.js -- the reference's
 * instruction table, imported UNCHANGED and never copied -- and writes the two dispatch
 * switches (the base table and the 0F table) to src/cpu/interpreter_gen.c.
 *
 *     node gen/interpreter_c.js          # -> src/cpu/interpreter_gen.c
 *
 * Everything mirrored from the upstream generator, with the site it comes from:
 *
 *   gen_read_imm_call (38-77)              -> read_imm8/8s/16/32s/read_moffs, and the
 *                                             `size = (op.os || opcode%2==1) ? v : 8` rule
 *   make_instruction_name (87-101)         -> the C symbol name, VERBATIM (the hand-written
 *                                             src/cpu/instructions*.c files are named by it)
 *   gen_instruction_body (104-183)         -> the 66/F2/F3 prefix split
 *   gen_instruction_body_after_prefix      -> the fixed_g `switch (modrm_byte >> 3 & 7)`
 *   gen_instruction_body_after_fixed_g     -> the `_mem`/`_reg` split at 0xC0, the imm
 *                                             threading, task_switch_test[_mmx](), and the
 *                                             after_block_boundary() postfix
 *   gen_table (339-450)                    -> the 256-case switch over opcode|(is_32<<8), the
 *                                             separate 0F runner, and the os -> 0xNN/0x1NN
 *                                             case split
 *
 * THE THREE C-SIDE DIFFERENCES.
 *
 * 1. Error plumbing.  Upstream wraps every fallible call as
 *    `match expr { Ok(v) => v, Err(()) => return }` (macro `return_on_pagefault!`).  In C the
 *    faultable helpers set `cpu->pagefault` and return a sentinel (src/cpu/cpu_state.h), so a
 *    call whose *value* is used is hoisted into a local followed by `if (cpu->pagefault)
 *    return;` -- the shape the M1a-1 report fixed and documented.  The generated code never
 *    reads the sentinel.
 *
 * 2. Unported instructions.  Upstream generates a call to `instructions::instr...`, and the
 *    Rust would fail to compile if the name did not exist -- a loud, correct failure.  The C
 *    port is built incrementally: the 0F table (M1b), the FPU group D8..DF (M1b) and the SSE
 *    entries are not written yet.  So the emitter SCOPES THE AVAILABILITY: it reads the
 *    `instr*` DEFINITIONS out of src/cpu/*.c (excluding the generated file and the selftest),
 *    and for an encoding with no definition it emits
 *
 *        unimplemented_instruction("instructions_0f::instr32_0FB6_mem");
 *        return;
 *
 *    instead of a call that would not link.  The dispatch case is still generated (so the
 *    opcode is *classified*, not silently mis-decoded), and hitting it stops the machine and
 *    names the missing symbol -- never a silent wrong instruction.  The emitter prints the
 *    counts, so "how much of the table is live" is a measurement, not a guess.
 *
 * 3. The prefix fallthrough carries NO check.  Upstream emits `dbg_assert!((prefixes_ & ...)
 *    == 0)` as the first line of the arm that runs when the guest's prefix combination is
 *    not the one this opcode defines -- and `dbg_assert!` is `debug_assert!` (dbg.rs:63),
 *    COMPILED OUT of the pinned release reference, so the reference simply executes the
 *    UNPREFIXED body for e.g. `F2 0F 6F C1`, `F3 0F 71 D0 01`, `66 0F 52 C1`.  The C port
 *    emits no check there either (gen_instruction_body, `else_body`): the condition is a
 *    property of the GUEST's input rather than an invariant of this port, and aborting on
 *    reachable guest bytes is a divergence from the reference, not a stop the port relies
 *    on.  Every internal invariant keeps its DBG_ASSERT; an unported body reached through
 *    the fallthrough still stops by NAME.
 */

import fs from "node:fs";
import path from "node:path";
import url from "node:url";

import x86_table from "../v86/gen/x86_table.js";

const HERE = url.fileURLToPath(new URL(".", import.meta.url));
const ROOT = path.join(HERE, "..");
const CPU_DIR = path.join(ROOT, "src", "cpu");
const OUT_FILE = path.join(CPU_DIR, "interpreter_gen.c");

/* util.js:11-19, inlined: the only thing the emitter took from that module is `hex`, and
 * importing it would drag in the argv-parsing helpers.  UPPERCASE, like upstream. */
function hex(n, pad) {
	pad = pad || 0;
	let s = n.toString(16).toUpperCase();
	while (s.length < pad) s = "0" + s;
	return s;
}

function fail(message) {
	throw new Error("interpreter_c: " + message);
}

/* ------------------------------------------------------------------ *
 * THE AVAILABILITY SCAN
 *
 * A name is "available" iff some emulator source (not the generated file, not the unit
 * test) DEFINES it.  Definitions only: a declaration in a header proves nothing about
 * linkage.  The scan is deliberately textual -- its contract is the same as the compiler's
 * (`void <name>(` at the start of a line), and any drift shows up as a missing symbol in
 * the generated file rather than a silent wrong call.
 * ------------------------------------------------------------------ */
function collect_defined_instructions() {
	const names = new Set();
	for (const file of fs.readdirSync(CPU_DIR).sort()) {
		if (!file.endsWith(".c")) continue;
		if (file === "interpreter_gen.c" || file === "cpu_selftest.c") continue;
		const text = fs.readFileSync(path.join(CPU_DIR, file), "utf8");
		const re = /^void[ \t]+(instr[A-Za-z0-9_]*)[ \t]*\(/gm;
		let m;
		while ((m = re.exec(text)) !== null) names.add(m[1]);
	}
	return names;
}

const defined = collect_defined_instructions();

/* ------------------------------------------------------------------ *
 * generate_interpreter.js:32-103, mirrored
 * ------------------------------------------------------------------ */

function gen_read_imm_call(op, size) {
	const imm_size = (op.os || op.opcode % 2 === 1) ? size : 8;

	if (op.imm8 || op.imm8s || op.imm16 || op.imm1632 || op.imm32 || op.immaddr) {
		if (op.imm8) return "read_imm8()";
		if (op.imm8s) return "read_imm8s()";
		if (op.immaddr) return "read_moffs()";
		if (!((op.imm1632 && imm_size === 16) || op.imm16)) {
			if (!((op.imm1632 && imm_size === 32) || op.imm32)) {
				fail("unreachable immediate kind: " + JSON.stringify(op));
			}
		}
		return ((op.imm1632 && imm_size === 16) || op.imm16) ? "read_imm16()" : "read_imm32s()";
	}
	return undefined;
}

/* generate_interpreter.js:87-101.  Returns the upstream-qualified Rust name; the C symbol
 * is the same string with the `module::` path stripped (one flat C namespace -- no base
 * encoding carries a 0F/66 prefix, so the two modules cannot collide). */
function make_instruction_name(encoding, size) {
	const suffix = encoding.os ? String(size) : "";
	const opcode_hex = hex(encoding.opcode & 0xFF, 2);
	const first_prefix = (encoding.opcode & 0xFF00) === 0 ? "" : hex(encoding.opcode >> 8 & 0xFF, 2);
	const second_prefix = (encoding.opcode & 0xFF0000) === 0 ? "" : hex(encoding.opcode >> 16 & 0xFF, 2);
	const fixed_g_suffix = encoding.fixed_g === undefined ? "" : `_${encoding.fixed_g}`;
	const module = first_prefix === "0F" || second_prefix === "0F" ? "instructions_0f" : "instructions";

	if (!(first_prefix === "" || first_prefix === "0F" || first_prefix === "F2" || first_prefix === "F3")) {
		fail("unexpected first prefix in " + JSON.stringify(encoding));
	}
	if (!(second_prefix === "" || second_prefix === "66" || second_prefix === "F2" || second_prefix === "F3")) {
		fail("unexpected second prefix in " + JSON.stringify(encoding));
	}

	return `${module}::instr${suffix}_${second_prefix}${first_prefix}${opcode_hex}${fixed_g_suffix}`;
}

function c_symbol(qualified) {
	return qualified.replace(/^[A-Za-z0-9_]+::/, "");
}

/* ------------------------------------------------------------------ *
 * small emission helpers
 * ------------------------------------------------------------------ */

let stat_calls = 0;
let stat_unimplemented = 0;

function indent(lines, n) {
	const pad = "\t".repeat(n === undefined ? 1 : n);
	return lines.map(line => (line === "" ? "" : pad + line));
}

/* The whole point of the availability scan: a name that is defined is called by name; a
 * name that is not is replaced by an explicit stop.  One or two lines, returned as a list
 * so the caller can indent them into whatever block it is emitting. */
function is_implemented(qualified, suffix) {
	return defined.has(c_symbol(qualified) + suffix);
}

function emit_call(qualified, suffix, args) {
	const symbol = c_symbol(qualified) + suffix;
	stat_calls++;
	if (defined.has(symbol)) {
		return [`${symbol}(${args.join(", ")});`];
	}
	stat_unimplemented++;
	return [`unimplemented_instruction("${qualified}${suffix}");`, "return;"];
}

/* generate_interpreter.js:234-237 */
function instruction_postfix(encoding) {
	return ((encoding.block_boundary && !encoding.no_block_boundary_in_interpreted) ||
		(!encoding.custom && encoding.e)) ? ["after_block_boundary();"] : [];
}

/* ------------------------------------------------------------------ *
 * generate_interpreter.js:231-349
 * ------------------------------------------------------------------ */
function gen_instruction_body_after_fixed_g(encoding, size) {
	const out = [];
	const qualified = make_instruction_name(encoding, size);

	if (encoding.task_switch_test || encoding.sse) {
		const condition = encoding.sse ? "!task_switch_test_mmx()" : "!task_switch_test()";
		out.push(`if (${condition}) {`, "\treturn;", "}");
	}

	const imm_read = gen_read_imm_call(encoding, size);

	/* An unimplemented encoding STOPS without reading its operands: the reads exist to
	 * feed the instruction, and with no instruction to feed there is nothing to keep in
	 * step (the machine halts, so the instruction pointer and the partial decode are not
	 * observed).  The alternative -- reading them and discarding -- is an unused local,
	 * which the port's -Werror rejects.  Stated because it is a real (if inert) difference
	 * in what the stop leaves behind. */
	const suffixes = encoding.e ? (encoding.ignore_mod ? [""] : ["_mem", "_reg"]) : [""];
	const implemented = suffixes.some(suffix => is_implemented(qualified, suffix));

	if (encoding.e) {
		if (encoding.ignore_mod) {
			/* has a modrm byte, but the 2 mod bits are ignored and both operands are always
			 * registers (0f20-0f24) */
			if (imm_read) fail("ignore_mod with an immediate value: " + JSON.stringify(encoding));
			out.push(...emit_call(qualified, "", ["modrm_byte & 7", "modrm_byte >> 3 & 7"]));
			if (implemented) out.push(...instruction_postfix(encoding));
			return out;
		}

		/* ARGUMENT EVALUATION ORDER IS SEMANTIC HERE.  Upstream's mem call is
		 *   instrX_mem(modrm_resolve(modrm_byte), modrm>>3&7, imm)
		 * and Rust evaluates arguments left to right, so the DISPLACEMENT is consumed
		 * before the immediate.  Hoisting the immediate above the branch would read the
		 * immediate out of the displacement bytes and then resolve the address from what
		 * follows -- a wrong store address, with the instruction length still balanced so
		 * the instruction stream stays in sync and only the memory effect is wrong
		 * (MEASURED: this was the one divergence of the first transcript diff,
		 * `66 c7 05 10 10 10 00 34 12`).  So the read is emitted per branch, in order. */
		const imm_hoist = imm_read ? [`int32_t imm = ${imm_read};`, "if (cpu->pagefault) return;"] : [];

		const mem_args = [];
		if (encoding.custom_modrm_resolve) {
			/* the instruction wants the raw modrm byte, not a resolved address */
			mem_args.push("modrm_byte");
		} else {
			mem_args.push("addr");
		}

		const reg_args = ["modrm_byte & 7"];
		if (encoding.fixed_g === undefined) {
			mem_args.push("modrm_byte >> 3 & 7");
			reg_args.push("modrm_byte >> 3 & 7");
		}
		if (imm_read) {
			mem_args.push("imm");
			reg_args.push("imm");
		}

		out.push("if (modrm_byte < 0xC0) {");
		/* `modrm_resolve` is a FAULTABLE read, and this check is the `return_on_pagefault!`
		 * for it; a custom_modrm_resolve encoding (0x8D/0x8F, the only two) never calls it
		 * -- the raw modrm byte is passed straight to the instruction -- so in THAT branch
		 * the check guarded nothing and would have hidden a fault raised above it (the R3
		 * review's N2).  The immediate's check travels WITH the immediate, below, so the
		 * read it guards is never left unchecked either way. */
		const resolves_modrm = !encoding.custom_modrm_resolve;
		if (resolves_modrm && implemented) {
			out.push("\tint32_t addr = modrm_resolve(modrm_byte);");
		}
		if (implemented) {
			if (resolves_modrm) out.push("\tif (cpu->pagefault) return;");
			out.push(...indent(imm_hoist, 1));
		}
		out.push(...indent(emit_call(qualified, "_mem", mem_args), 1));
		out.push("}", "else {");
		if (implemented) out.push(...indent(imm_hoist, 1));
		out.push(...indent(emit_call(qualified, "_reg", reg_args), 1));
		out.push("}");
		if (implemented) out.push(...instruction_postfix(encoding));
		return out;
	}

	const args = [];
	if (imm_read) {
		if (implemented) out.push(`int32_t imm = ${imm_read};`, "if (cpu->pagefault) return;");
		args.push("imm");
	}
	if (encoding.extra_imm16) {
		if (!imm_read) fail("extra_imm16 without an immediate: " + JSON.stringify(encoding));
		if (implemented) out.push("int32_t imm2 = read_imm16();", "if (cpu->pagefault) return;");
		args.push("imm2");
	} else if (encoding.extra_imm8) {
		if (!imm_read) fail("extra_imm8 without an immediate: " + JSON.stringify(encoding));
		if (implemented) out.push("int32_t imm2 = read_imm8();", "if (cpu->pagefault) return;");
		args.push("imm2");
	}

	out.push(...emit_call(qualified, "", args));
	if (implemented) out.push(...instruction_postfix(encoding));
	return out;
}

/* generate_interpreter.js:183-229 */
function gen_instruction_body_after_prefix(encodings, size) {
	const encoding = encodings[0];

	if (encoding.fixed_g !== undefined) {
		if (!encoding.e) fail("fixed_g without a modrm byte: " + JSON.stringify(encoding));

		/* group by opcode without prefix plus the middle bits of the modrm byte */
		const by_key = Object.create(null);
		for (const entry of encodings) {
			if (typeof entry.fixed_g !== "number") fail("fixed_g is not a number: " + JSON.stringify(entry));
			by_key[entry.opcode & 0xFFFF | entry.fixed_g << 16] = entry;
		}
		const cases = Object.values(by_key).sort((a, b) => a.fixed_g - b.fixed_g);

		const out = ["switch (modrm_byte >> 3 & 7) {"];
		for (const entry of cases) {
			out.push(`case ${entry.fixed_g}:`, "{");
			out.push(...indent(gen_instruction_body_after_fixed_g(entry, size), 1));
			out.push("}", "\tbreak;");
		}
		out.push("default:", "\ttrigger_ud();", "\tbreak;", "}");
		return out;
	}

	if (encodings.length !== 1) fail("expected exactly one encoding");
	return gen_instruction_body_after_fixed_g(encodings[0], size);
}

/* generate_interpreter.js:105-181 */
function gen_instruction_body(encodings, size) {
	const encoding = encodings[0];

	const has_66 = [];
	const has_f2 = [];
	const has_f3 = [];
	const no_prefix = [];

	for (const e of encodings) {
		if ((e.opcode >>> 16) === 0x66) has_66.push(e);
		else if ((e.opcode >>> 8 & 0xFF) === 0xF2 || (e.opcode >>> 16) === 0xF2) has_f2.push(e);
		else if ((e.opcode >>> 8 & 0xFF) === 0xF3 || (e.opcode >>> 16) === 0xF3) has_f3.push(e);
		else no_prefix.push(e);
	}

	if ((has_f2.length || has_f3.length) &&
		!((encoding.opcode & 0xFF0000) === 0 || (encoding.opcode & 0xFF00) === 0x0F00)) {
		fail("F2/F3 prefix on a non-0F encoding: " + JSON.stringify(encoding));
	}
	if (has_66.length && (encoding.opcode & 0xFF00) !== 0x0F00) {
		fail("66 prefix on a non-0F encoding: " + JSON.stringify(encoding));
	}

	const out = [];

	/* `modrm_byte` is read for every e-encoding EXCEPT when nothing in the group uses it:
	 * the ignore_mod encodings (0F 20-24) reference it only as a call argument, and an
	 * unimplemented call drops its arguments -- so declaring it there would be an unused
	 * local (-Werror).  Every other e-encoding tests it (`modrm_byte < 0xC0`, or the
	 * fixed_g switch), so the declaration is unconditional in practice. */
	const uses_modrm = encodings.some(e => {
		if (!e.e) return false;
		if (!e.ignore_mod) return true;
		const name = make_instruction_name(e, size);
		return is_implemented(name, "") || is_implemented(name, "_mem") || is_implemented(name, "_reg");
	});
	if (encoding.e && uses_modrm) {
		out.push("int32_t modrm_byte = read_imm8();", "if (cpu->pagefault) return;");
	}

	if (has_66.length || has_f2.length || has_f3.length) {
		if (!no_prefix.length) fail("no unprefixed encoding to fall through to: " + JSON.stringify(encoding));

		out.push("uint8_t prefixes_ = cpu->prefixes;");

		const blocks = [];
		if (has_66.length) blocks.push({ condition: "(prefixes_ & PREFIX_66) != 0", body: gen_instruction_body_after_prefix(has_66, size) });
		if (has_f2.length) blocks.push({ condition: "(prefixes_ & PREFIX_F2) != 0", body: gen_instruction_body_after_prefix(has_f2, size) });
		if (has_f3.length) blocks.push({ condition: "(prefixes_ & PREFIX_F3) != 0", body: gen_instruction_body_after_prefix(has_f3, size) });

		/* THE FALLTHROUGH ARM CARRIES NO CHECK -- C-side difference 3, and the reason is
		 * the reference itself.  Upstream emits, as this arm's first line,
		 *
		 *     dbg_assert!((prefixes_ & (PREFIX_F2 | PREFIX_F3)) == 0);   [| PREFIX_66 for sse]
		 *
		 * and `dbg_assert!` is `debug_assert!` (dbg.rs:63): COMPILED OUT of the pinned
		 * release reference.  So the reference EXECUTES this arm's body for a prefix
		 * combination the table does not define for this opcode, and the C port must do
		 * the same -- MEASURED (R3F): `F2 0F 6F C1`, `F3 0F 71 D0 01` and `66 0F 52 C1`
		 * all run to completion under the reference while aborting a C build whose
		 * DBG_ASSERT is live.  Those are ordinary guest bytes, not port bugs: the
		 * condition is a statement about the GUEST's input, not an invariant of this
		 * port (cpu_state.h's DBG_ASSERT policy), so it is the one guest-driven check
		 * the port drops -- every internal invariant stays live.
		 *
		 * Dropping it changes nothing about the port's own stops: the unprefixed body
		 * below is where the work is, an unported body still stops by NAME
		 * (unimplemented_instruction) exactly as the unprefixed encoding does, and an
		 * sse encoding's `task_switch_test_mmx()` guard still runs first. */
		const else_body = gen_instruction_body_after_prefix(no_prefix, size);

		blocks.forEach((block, index) => {
			out.push((index === 0 ? "if (" : "else if (") + block.condition + ") {");
			out.push(...indent(block.body, 1));
			out.push("}");
		});
		out.push("else {");
		out.push(...indent(else_body, 1));
		out.push("}");
	} else {
		out.push(...gen_instruction_body_after_prefix(encodings, size));
	}

	return out;
}

/* ------------------------------------------------------------------ *
 * generate_interpreter.js:339-450
 * ------------------------------------------------------------------ */

function group_by_opcode() {
	const by_opcode = Object.create(null);
	const by_opcode0f = Object.create(null);

	for (const entry of x86_table) {
		let opcode = entry.opcode;

		if ((opcode & 0xFF00) === 0x0F00) {
			opcode &= 0xFF;
			(by_opcode0f[opcode] = by_opcode0f[opcode] || []).push(entry);
		} else {
			opcode &= 0xFF;
			(by_opcode[opcode] = by_opcode[opcode] || []).push(entry);
		}
	}
	return { by_opcode, by_opcode0f };
}

function switch_cases(by_opcode) {
	const out = [];
	for (let opcode = 0; opcode < 0x100; opcode++) {
		const encoding = by_opcode[opcode];
		if (!encoding || !encoding.length) fail("no encoding for opcode " + hex(opcode, 2));

		const opcode_hex = hex(opcode, 2);
		const opcode_high_hex = hex(opcode | 0x100, 2);

		const blocks = [];
		if (encoding[0].os) {
			blocks.push({ labels: [`0x${opcode_hex}`], body: gen_instruction_body(encoding, 16) });
			blocks.push({ labels: [`0x${opcode_high_hex}`], body: gen_instruction_body(encoding, 32) });
		} else {
			blocks.push({ labels: [`0x${opcode_hex}`, `0x${opcode_high_hex}`], body: gen_instruction_body(encoding, undefined) });
		}

		for (const block of blocks) {
			for (const label of block.labels) out.push(`case ${label}:`);
			out.push("{");
			out.push(...indent(block.body, 1));
			out.push("}", "break;");
		}
	}
	return out;
}

function emit_switch(fn_name, doc, by_opcode) {
	const lines = [];
	lines.push(doc);
	lines.push(`void ${fn_name}(int32_t opcode)`);
	lines.push("{");
	lines.push("\tswitch(opcode)");
	lines.push("\t{");
	lines.push(...indent(switch_cases(by_opcode), 1));
	lines.push("\tdefault:");
	lines.push("\t\tDBG_ASSERT(false);");
	lines.push("\t\tbreak;");
	lines.push("\t}");
	lines.push("}");
	return lines;
}

function generate() {
	const { by_opcode, by_opcode0f } = group_by_opcode();

	const base = emit_switch(
		"run_instruction",
		"/* cpu.rs:3131 -- the base-table switch.  `opcode` is\n" +
		" * `read_imm8() | (is_osize_32() << 8)` (cpu.rs:3367, cpu.rs:3168): the low byte is the\n" +
		" * opcode, bit 8 the operand size, which is exactly the generator's 0xNN / 0x1NN split. */",
		by_opcode);

	const zero_f = emit_switch(
		"run_instruction0f",
		"/* The 0F-table switch, same convention.  run_instruction0f_16/32 (cpu.rs:3132-3133)\n" +
		" * -- the wrappers instructions.c's instr16_0F/instr32_0F call -- live in\n" +
		" * instructions_0f.c and are the only callers. */",
		by_opcode0f);

	const header = `/*
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
 * ${stat_calls} call sites, ${stat_unimplemented} of them unimplemented at the time this
 * file was generated (${defined.size} instr* symbols defined in the port's sources).
 */

#include "cpu.h"
#include "cpu_state.h"
#include "instructions.h"
#include "instructions_0f.h"
#include "misc_instr.h"
#include "modrm.h"
#include "paging.h"

`;

	const text = header + base.join("\n") + "\n\n" + zero_f.join("\n") + "\n";

	/* Reproducibility (the unit's G4): write only when the bytes change, and never
	 * include anything host-specific.  A byte-identical rewrite is not a rebuild. */
	const previous = fs.existsSync(OUT_FILE) ? fs.readFileSync(OUT_FILE, "utf8") : null;
	if (previous !== text) fs.writeFileSync(OUT_FILE, text);

	const unimplemented = stat_unimplemented;
	console.log(
		`[+] ${path.relative(ROOT, OUT_FILE)}: ${stat_calls} call sites, ` +
		`${stat_calls - unimplemented} implemented, ${unimplemented} unimplemented, ` +
		`${defined.size} names defined${previous === text ? " (unchanged)" : ""}`);
}

generate();
