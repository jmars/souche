#!/usr/bin/env node
/*
 * gen/interpreter_uop.js -- the uop-cache DECODE-TABLE generator.
 *
 * The second emitter pass over the same v86/gen/x86_table.js (imported UNCHANGED and never
 * copied), and the mirror of gen/interpreter_c.js over the SAME table: where interpreter_c.js
 * emits the two dispatch switches that the interpreter EXECUTES, this emits, per dispatch
 * case, the descriptor a DECODER needs -- what to fetch, which body to call, and where the
 * block ends.  Neither generator re-decides an opcode's semantics; both are mechanical
 * projections of the table, so they cannot disagree about the table without disagreeing
 * visibly in the generated files.
 *
 *     node gen/interpreter_uop.js          # -> src/cpu/uop_gen.c
 *
 * WHAT IS EMITTED, and the interpreter_c.js site it mirrors:
 *
 *   gen_instruction_body (gen_instruction_c.js:328-412)   -> one uop_case_t per
 *       `opcode | (is_osize_32() << 8)`: the case's prefix arms, in the ORDER the generated
 *       if-chain tests them (66, F2, F3) with the fallthrough LAST, and the case's
 *       `reads_modrm` (the `uses_modrm` of interpreter_c.js:358-366 -- the modrm byte is part
 *       of the instruction's length, so the decoder must fetch exactly when the interpreter
 *       does).
 *   gen_instruction_body_after_prefix (:299-325)          -> one uop_arm_t per arm and one
 *       uop_entry_t per (arm, fixed_g): the `switch (modrm_byte >> 3 & 7)` as data.  A g the
 *       arm does not define has NO entry, which is the generated `default: trigger_ud()`.
 *   gen_instruction_body_after_fixed_g (:195-296)         -> one uop_desc_t per entry: the
 *       body's address per form (`fn_plain`/`fn_mem`/`fn_reg`), the immediate kinds
 *       (gen_read_imm_call, :115-130), and the flags (block_end from `instruction_postfix`,
 *       :187-190; the task_switch_test/sse guard; custom_modrm_resolve; ignore_mod).
 *
 * THE AVAILABILITY SCAN IS THE SAME SCAN, VERBATIM.  A descriptor's fn pointer is the port's
 * own `instr_XX_*` symbol, or NULL when no emulator source DEFINES it (interpreter_c.js:96-107;
 * the same exclusions, the same regex).  The interpreter emits `unimplemented_instruction(...)`
 * for that form; this decoder emits no descriptor, so the block ENDS there and the interpreter
 * runs it -- which is exactly the per-opcode enablement lever of the plan (ss2.2 item 2, ss7).
 * The scan is duplicated rather than shared because the two files must stay independently
 * runnable; `make uop-gen-check` (see the Makefile) runs both and compares their verdicts.
 *
 * REPRODUCIBILITY.  The emitted file is a pure function of x86_table.js, the emitter and the
 * set of `instr*` definitions in src/cpu/*.c -- like interpreter_gen.c, it is written only
 * when the bytes change (a byte-identical rewrite is not a rebuild), and it contains nothing
 * host-specific.
 */

import fs from "node:fs";
import path from "node:path";
import url from "node:url";

import x86_table from "../v86/gen/x86_table.js";

const HERE = url.fileURLToPath(new URL(".", import.meta.url));
const ROOT = path.join(HERE, "..");
const CPU_DIR = path.join(ROOT, "src", "cpu");
const OUT_FILE = path.join(CPU_DIR, "uop_gen.c");

function fail(message) {
	throw new Error("interpreter_uop: " + message);
}

/* ------------------------------------------------------------------ *
 * THE AVAILABILITY SCAN -- interpreter_c.js:96-107, copied verbatim
 * (same exclusions, same regex; keeping them in step is what makes the two
 * generated files agree about which encodings exist)
 * ------------------------------------------------------------------ */
function collect_defined_instructions() {
	const names = new Set();
	for (const file of fs.readdirSync(CPU_DIR).sort()) {
		if (!file.endsWith(".c")) continue;
		if (file === "interpreter_gen.c" || file === "cpu_selftest.c") continue;
		if (file === "uop_gen.c" || file === "uop_cache_selftest.c") continue;
		const text = fs.readFileSync(path.join(CPU_DIR, file), "utf8");
		const re = /^void[ \t]+(instr[A-Za-z0-9_]*)[ \t]*\(/gm;
		let m;
		while ((m = re.exec(text)) !== null) names.add(m[1]);
	}
	return names;
}

const defined = collect_defined_instructions();

/* ------------------------------------------------------------------ *
 * the naming contract -- interpreter_c.js:132-155, mirrored
 * ------------------------------------------------------------------ */
function hex(n, pad) {
	pad = pad || 0;
	let s = n.toString(16).toUpperCase();
	while (s.length < pad) s = "0" + s;
	return s;
}

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

function is_implemented(qualified, suffix) {
	return defined.has(c_symbol(qualified) + suffix);
}

/* ------------------------------------------------------------------ *
 * the immediate kinds -- interpreter_c.js:115-130, mirrored
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

const IMM_KIND = {
	"read_imm8()": "UOP_IMM_8",
	"read_imm8s()": "UOP_IMM_8S",
	"read_imm16()": "UOP_IMM_16",
	"read_imm32s()": "UOP_IMM_32S",
	"read_moffs()": "UOP_IMM_MOFFS",
};

/* ------------------------------------------------------------------ *
 * one descriptor per (encoding, size)
 * ------------------------------------------------------------------ */

let stat_forced_ends = 0;
let stat_forms = 0;
let stat_forms_missing = 0;
let stat_names_missing = new Set();

function resolve_form(qualified, suffix) {
	stat_forms++;
	if (defined.has(c_symbol(qualified) + suffix)) {
		return c_symbol(qualified) + suffix;
	}
	stat_forms_missing++;
	stat_names_missing.add(qualified + suffix);
	return null;
}

function describe(encoding, size) {
	const qualified = make_instruction_name(encoding, size);
	const imm_read = gen_read_imm_call(encoding, size);

	if (encoding.e && (encoding.extra_imm16 || encoding.extra_imm8)) {
		/* the generated e-branch threads `imm` only -- an extra immediate on a modrm
		 * encoding would be a missing argument in interpreter_gen.c too */
		fail("extra immediate on a modrm encoding: " + JSON.stringify(encoding));
	}

	let imm2 = null;
	if (encoding.extra_imm16) imm2 = "read_imm16()";
	else if (encoding.extra_imm8) imm2 = "read_imm8()";
	if (imm2 && !imm_read) fail("extra immediate without an immediate: " + JSON.stringify(encoding));

	let flags = 0;
	if (encoding.e) flags |= 1 << 0; /* UOP_DESC_HAS_MODRM */
	if (encoding.ignore_mod) flags |= 1 << 1;
	if (encoding.fixed_g !== undefined) flags |= 1 << 2;
	if (encoding.custom_modrm_resolve) flags |= 1 << 3;
	if (encoding.task_switch_test || encoding.sse) flags |= 1 << 4;
	/* THE FORCED BLOCK END, and a PLAN-VS-TABLE DISAGREEMENT worth stating: the plan
	 * (results/perf-2-blockcache-plan.md ss5.1) says `sti` "is: block_boundary, no no_bib".
	 * x86_table.js:329 says otherwise -- { opcode: 0xFB, custom: 1, custom_sti: 1, skip: 1 }
	 * carries NO block_boundary.  The plan's DECISION is right (instr_FB,
	 * instructions.c:2145-2164, executes a shadow instruction and handle_irqs() inside its own
	 * body -- the port's ONE runtime nested dispatch -- so a uop after it would execute the
	 * shadow instruction twice, plan ss5.1), so the end is FORCED here, from the table's own
	 * `custom_sti` marker, which is what marks exactly that body.  See
	 * results/perf-3-uop-decode.md ss5 for the measurement. */
	if (encoding.custom_sti) {
		flags |= 1 << 5;
		stat_forced_ends++;
	}
	else if (encoding.block_boundary && !encoding.no_block_boundary_in_interpreted) {
		flags |= 1 << 5;
	}
	if (imm_read) flags |= 1 << 6;
	if (imm2) flags |= 1 << 7;

	/* THE SECOND FLAGS BYTE: the interpreter's own POSTFIX predicate, verbatim from
	 * interpreter_c.js:187-190 --
	 *
	 *     (block_boundary && !no_block_boundary_in_interpreted) || (!custom && e)
	 *
	 * `after_block_boundary()` is what ends an INTERPRETER BATCH, and an interpreter batch is
	 * guest-visible: instruction_counter is added once per batch (cpu.c:148), microtick() is
	 * instruction_counter/1000 (exec_boundary.c:85-88) and the guest reads microtick through
	 * rdtsc and the PIT/PM-timer port reads.  A uop cache that flushes the counter at the
	 * wrong instruction boundary changes a value the guest can read.  This byte is the
	 * predicate AS DATA so the executor cannot re-derive it wrongly -- and it cannot be
	 * derived from the first byte: `!custom && e` is FALSE for 520 of x86_table.js's modrm
	 * encodings (0x00's entry is {opcode:0, custom:1, e:1}), so "has a modrm byte" is NOT the
	 * flush predicate (PERF-3's M1 report does not mention it because M1 never executes). */
	let iflags = 0;
	if ((encoding.block_boundary && !encoding.no_block_boundary_in_interpreted) || (!encoding.custom && encoding.e)) {
		iflags |= 1 << 0; /* UOP_DESC_FLUSH */
	}
	/* WHICH guard: `sse` selects task_switch_test_mmx(), everything else task_switch_test()
	 * (interpreter_c.js:200-203).  They are different functions -- with CR0.EM the mmx form
	 * raises #UD and the other #NM (misc_instr.c:656-682) -- so the executor cannot pick one. */
	if (encoding.sse) iflags |= 1 << 1; /* UOP_DESC_GUARD_MMX */

	let fn_plain = null;
	let fn_mem = null;
	let fn_reg = null;
	if (encoding.e) {
		if (encoding.ignore_mod) {
			/* 0F 20-24: the modrm byte is read, both operands are registers */
			if (imm_read) fail("ignore_mod with an immediate: " + JSON.stringify(encoding));
			fn_plain = resolve_form(qualified, "");
		}
		else {
			fn_mem = resolve_form(qualified, "_mem");
			fn_reg = resolve_form(qualified, "_reg");
		}
	}
	else {
		fn_plain = resolve_form(qualified, "");
	}

	/* the slots are `void (*)(void)` -- the plan's uop_desc_t -- because one slot type must
	 * hold every form's signature (fn(addr,g), fn(addr,g,imm), fn(addr), fn(addr,imm), ...);
	 * the M2 executor casts back to the signature uop_shape_of names.  The cast is explicit
	 * so the table is warning-free under -Wall -Wextra -Werror (gcc's own exception for
	 * `void (*)(void)` applies). */
	const cast = f => (f === null ? "NULL" : `(void (*)(void))${f}`);

	return {
		fn_plain: cast(fn_plain),
		fn_mem: cast(fn_mem),
		fn_reg: cast(fn_reg),
		flags,
		iflags,
		imm: imm_read === undefined ? "UOP_IMM_NONE" : IMM_KIND[imm_read],
		imm2: imm2 === null ? "UOP_IMM2_NONE" : (imm2 === "read_imm16()" ? "UOP_IMM2_16" : "UOP_IMM2_8"),
		name: qualified,
	};
}

/* ------------------------------------------------------------------ *
 * the case / arm / entry structure -- interpreter_c.js:299-412, mirrored
 * ------------------------------------------------------------------ */

/* interpreter_c.js:358-366 `uses_modrm` */
function uses_modrm(encodings, size) {
	return encodings.some(e => {
		if (!e.e) return false;
		if (!e.ignore_mod) return true;
		const name = make_instruction_name(e, size);
		return is_implemented(name, "") || is_implemented(name, "_mem") || is_implemented(name, "_reg");
	});
}

/* interpreter_c.js:299-325 `gen_instruction_body_after_prefix` */
function arm_entries(encodings, size) {
	const encoding = encodings[0];

	if (encoding.fixed_g !== undefined) {
		if (!encoding.e) fail("fixed_g without a modrm byte: " + JSON.stringify(encoding));
		const by_key = Object.create(null);
		for (const entry of encodings) {
			if (typeof entry.fixed_g !== "number") fail("fixed_g is not a number: " + JSON.stringify(entry));
			by_key[entry.opcode & 0xFFFF | entry.fixed_g << 16] = entry;
		}
		return Object.values(by_key).sort((a, b) => a.fixed_g - b.fixed_g)
			.map(entry => ({ fixed_g: entry.fixed_g, desc: describe(entry, size) }));
	}

	if (encodings.length !== 1) fail("expected exactly one encoding");
	return [{ fixed_g: null, desc: describe(encodings[0], size) }];
}

/* interpreter_c.js:328-412 `gen_instruction_body` */
function build_case(encodings, size) {
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

	const arms = [];
	if (has_66.length || has_f2.length || has_f3.length) {
		if (!no_prefix.length) fail("no unprefixed encoding to fall through to: " + JSON.stringify(encoding));
		if (has_66.length) arms.push({ cond: "PREFIX_66", entries: arm_entries(has_66, size) });
		if (has_f2.length) arms.push({ cond: "PREFIX_F2", entries: arm_entries(has_f2, size) });
		if (has_f3.length) arms.push({ cond: "PREFIX_F3", entries: arm_entries(has_f3, size) });
		arms.push({ cond: null, entries: arm_entries(no_prefix, size) });
	}
	else {
		arms.push({ cond: null, entries: arm_entries(encodings, size) });
	}

	return { arms, reads_modrm: uses_modrm(encodings, size) };
}

/* interpreter_c.js:418-461 `group_by_opcode` */
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

/* ------------------------------------------------------------------ *
 * emission
 * ------------------------------------------------------------------ */

const descs = [];
const desc_index = new Map();

function intern_desc(desc) {
	const key = `${desc.fn_plain}|${desc.fn_mem}|${desc.fn_reg}|${desc.flags}|${desc.iflags}|${desc.imm}|${desc.imm2}`;
	if (desc_index.has(key)) return desc_index.get(key);
	const index = descs.length;

	descs.push(desc);
	desc_index.set(key, index);
	return index;
}

const entries = []; /* {fixed_g, desc} */
const arms = [];    /* {cond, first, n} */
const cases = { base: [], zero_f: [] };

function emit_case_list(by_opcode, which) {
	const out = [];
	for (let opcode = 0; opcode < 0x100; opcode++) {
		const encodings = by_opcode[opcode];
		if (!encodings || !encodings.length) fail("no encoding for opcode " + hex(opcode, 2));

		/* interpreter_c.js:436-461 `switch_cases`: an `os` encoding gets the 16-bit case at
		 * 0xNN and the 32-bit case at 0x1NN; anything else serves both labels with one body */
		const keys = encodings[0].os ? [16, 32] : [null];
		for (const size of keys) {
			const built = build_case(encodings, size);
			const first_arm = arms.length;
			for (const arm of built.arms) {
				const first = entries.length;
				for (const entry of arm.entries) {
					entries.push(entry);
				}
				arms.push({ cond: arm.cond, first, n: arm.entries.length });
			}
			const labels = size === null ? [opcode, opcode | 0x100] : [size === 16 ? opcode : opcode | 0x100];
			/* the descriptor index per (arm, g) is interned as the entries are pushed */
			for (const label of labels) {
				cases[which][label] = { first_arm, n_arms: arms.length - first_arm, reads_modrm: built.reads_modrm };
			}
		}
	}
	return out;
}

/* the entries must carry descriptor INDICES, and interning happens as they are pushed, so the
 * push loop above rewrites them in place -- do it here, walking the arms after the fact. */
function intern_entries() {
	for (const arm of arms) {
		for (let i = 0; i < arm.n; i++) {
			const entry = entries[arm.first + i];
			entry.desc_index = intern_desc(entry.desc);
		}
	}
}

function case_lines(list, name) {
	const out = [];
	out.push(`const uop_case_t ${name}[512] = {`);
	for (let i = 0; i < 512; i++) {
		const c = list[i];
		if (!c) fail("case " + i + " of " + name + " was never built");
		out.push(`\t{ ${c.first_arm}, ${c.n_arms}, ${c.reads_modrm ? 1 : 0} },`);
	}
	out.push("};");
	return out;
}

function generate() {
	const { by_opcode, by_opcode0f } = group_by_opcode();

	emit_case_list(by_opcode, "base");
	emit_case_list(by_opcode0f, "zero_f");
	intern_entries();

	const lines = [];

	/* the descriptors */
	lines.push("/* the per-encoding descriptors.  A NULL form pointer means the port does not");
	lines.push(" * implement that form: the interpreter emits unimplemented_instruction() for it and");
	lines.push(" * the decoder emits no descriptor, so the block falls back. */");
	lines.push("const uop_desc_t uop_descs[] = {");
	for (const d of descs) {
		lines.push(`\t{ ${d.fn_plain}, ${d.fn_mem}, ${d.fn_reg}, 0x${hex(d.flags, 2)}, 0x${hex(d.iflags, 2)}, ${d.imm}, ${d.imm2} },`);
	}
	lines.push("};");
	lines.push("const uint32_t uop_desc_count = sizeof(uop_descs) / sizeof(uop_descs[0]);");
	lines.push("");

	lines.push("/* the diagnostic names, aligned with uop_descs (the interpreter writes the same");
	lines.push(" * qualified symbol into unimplemented_instruction's message) */");
	lines.push("const char *const uop_desc_names[] = {");
	for (const d of descs) {
		lines.push(`\t"${d.name}",`);
	}
	lines.push("};");
	lines.push("");

	lines.push("/* one entry per (arm, fixed_g); a g absent from an arm is the generated");
	lines.push(" * `default: trigger_ud()`, i.e. no descriptor */");
	lines.push("const uop_entry_t uop_entries[] = {");
	for (const e of entries) {
		lines.push(`\t{ ${e.fixed_g === null ? "UOP_FIXED_G_NONE" : e.fixed_g}, ${e.desc_index} },`);
	}
	lines.push("};");
	lines.push("const uint32_t uop_entry_count = sizeof(uop_entries) / sizeof(uop_entries[0]);");
	lines.push("");

	lines.push("/* one arm per prefix group of a case, in the generated if-chain's order: 66,");
	lines.push(" * F2, F3, then the FALLTHROUGH (cond 0) last */");
	lines.push("const uop_arm_t uop_arms[] = {");
	for (const a of arms) {
		lines.push(`\t{ ${a.cond === null ? "0" : a.cond}, ${a.n}, ${a.first} },`);
	}
	lines.push("};");
	lines.push("const uint32_t uop_arm_count = sizeof(uop_arms) / sizeof(uop_arms[0]);");
	lines.push("");

	lines.push(...case_lines(cases.base, "uop_base_cases"));
	lines.push("");
	lines.push(...case_lines(cases.zero_f, "uop_0f_cases"));

	const header = `/*
 * uop_gen.c -- GENERATED FILE, DO NOT EDIT.
 *
 * Emitted by gen/interpreter_uop.js from v86/gen/x86_table.js (imported unchanged, the same
 * table gen/interpreter_c.js reads).  The whole file is a function of that table, the emitter
 * and the set of \`instr*\` definitions in src/cpu/ sources: re-running the generator on the same
 * tree reproduces it byte for byte.
 *
 * This is the DECODE side of the uop cache (unit PERF-3, milestone M1): one descriptor per
 * encoding, saying what to fetch, which of the port's own instr_XX_* bodies to call, and
 * whether the instruction ends a block.  It is data, not code, so the decoder in
 * src/cpu/uop_cache.c can walk a byte stream without the interpreter's dispatch switch.
 *
 * ${stat_forced_ends} of the block ends is FORCED rather than taken from the table: sti's
 * table entry carries custom_sti but no block_boundary -- see the emitter and
 * results/perf-3-uop-decode.md ss5.
 *
 * ${descs.length} distinct descriptors, ${entries.length} (arm, fixed_g) entries,
 * ${arms.length} prefix arms, ${stat_forms} call forms of which ${stat_forms_missing} have no
 * definition in the port's sources (they get a NULL form pointer: no descriptor, block falls
 * back to the interpreter, which stops by name).
 */

#include "uop_cache.h"

/* The fn pointers are the port's own instr_XX_* symbols, so this file needs their
 * declarations -- the same set gen/interpreter_gen.c's own header pulls in (it emits CALLS
 * to them; this emits their ADDRESSES, which the compiler needs to see declared all the
 * same).  Nothing here includes or defines a body. */
#include "cpu_state.h"
#include "instructions.h"
#include "instructions_0f.h"
#include "misc_instr.h"
#include "modrm.h"

`;

	const text = header + lines.join("\n") + "\n";

	const previous = fs.existsSync(OUT_FILE) ? fs.readFileSync(OUT_FILE, "utf8") : null;
	if (previous !== text) fs.writeFileSync(OUT_FILE, text);

	console.log(
		`[+] ${path.relative(ROOT, OUT_FILE)}: ${descs.length} descriptors, ${entries.length} entries, ` +
		`${arms.length} arms, ${stat_forms} forms (${stat_forms_missing} unimplemented), ` +
		`${stat_forced_ends} forced block ends` +
		`${previous === text ? " (unchanged)" : ""}`);
}

/* `--dump` prints the decoded case/arm/entry structure instead of writing the file: the same
 * tables the decoder walks, in the form a reader can check against interpreter_gen.c. */
function dump() {
	const { by_opcode, by_opcode0f } = group_by_opcode();
	for (const [label, by_opcode_] of [["base", by_opcode], ["0f", by_opcode0f]]) {
		for (let opcode = 0; opcode < 0x100; opcode++) {
			const encodings = by_opcode_[opcode];
			if (!encodings) continue;
			const sizes = encodings[0].os ? [16, 32] : [null];
			for (const size of sizes) {
				const built = build_case(encodings, size);
				const key = (size === null ? opcode : (size === 16 ? opcode : opcode | 0x100));
				console.log(`${label} ${hex(key, 3)} size=${size === null ? "any" : size} modrm=${built.reads_modrm ? 1 : 0}`);
				for (const arm of built.arms) {
					console.log(`\tarm cond=${arm.cond === null ? "fallthrough" : arm.cond}`);
					for (const e of arm.entries) {
						console.log(`\t\tg=${e.fixed_g === null ? "-" : e.fixed_g} ${e.desc.name} ` +
							`plain=${e.desc.fn_plain} mem=${e.desc.fn_mem} reg=${e.desc.fn_reg} ` +
							`flags=0x${hex(e.desc.flags, 2)} imm=${e.desc.imm} imm2=${e.desc.imm2}`);
					}
				}
			}
		}
	}
}

if (process.argv[2] === "--dump") dump();
else generate();
