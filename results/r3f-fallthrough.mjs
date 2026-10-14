// R3F: the PREFIX-FALLTHROUGH reachability sweep.
//
// Question the N1 fix has to answer honestly: "a probe guest executing the fallthrough
// sequence must ... produce the same state as the reference for that input."  Whether that
// is possible TODAY is a property of the table and of the port's own sources, so it is
// counted here rather than asserted: for every (opcode, prefix combination) that reaches the
// generated `else` arm with a NON-ZERO prefix mask (the arm the pre-R3F assert aborted in),
// is the UNPREFIXED body -- the thing that arm runs, and the thing the reference executes --
// IMPLEMENTED in src/cpu/*.c?
//
// The grouping and the prefix classification are copied from gen/interpreter_c.js
// (group_by_opcode + gen_instruction_body), which mirrors generate_interpreter.js:339-450,
// and the availability scan is the emitter's own contract (a `^void <name>(` definition).
// Run: node results/r3f-fallthrough.mjs

import x86_table from "/home/jaye/futaie/souche/v86/gen/x86_table.js";
import fs from "node:fs";
import path from "node:path";

const CPU_DIR = "/home/jaye/futaie/souche/src/cpu";
const hex = (n, pad) => n.toString(16).toUpperCase().padStart(pad, "0");

/* the emitter's collect_defined_instructions() */
const defined = new Set();
for (const file of fs.readdirSync(CPU_DIR).sort()) {
	if (!file.endsWith(".c") || file === "interpreter_gen.c" || file === "cpu_selftest.c") continue;
	const text = fs.readFileSync(path.join(CPU_DIR, file), "utf8");
	const re = /^void[ \t]+(instr[A-Za-z0-9_]*)[ \t]*\(/gm;
	let m;
	while ((m = re.exec(text)) !== null) defined.add(m[1]);
}

/* the emitter's make_instruction_name() */
function make_instruction_name(encoding, size) {
	const suffix = encoding.os ? String(size) : "";
	const opcode_hex = hex(encoding.opcode & 0xFF, 2);
	const first_prefix = (encoding.opcode & 0xFF00) === 0 ? "" : hex(encoding.opcode >> 8 & 0xFF, 2);
	const second_prefix = (encoding.opcode & 0xFF0000) === 0 ? "" : hex(encoding.opcode >> 16 & 0xFF, 2);
	const fixed_g_suffix = encoding.fixed_g === undefined ? "" : `_${encoding.fixed_g}`;
	const module = first_prefix === "0F" || second_prefix === "0F" ? "instructions_0f" : "instructions";
	return `${module}::instr${suffix}_${second_prefix}${first_prefix}${opcode_hex}${fixed_g_suffix}`;
}
const c_symbol = q => q.replace(/^[A-Za-z0-9_]+::/, "");

const by_opcode = Object.create(null), by_opcode0f = Object.create(null);
for (const entry of x86_table) {
	let opcode = entry.opcode;
	if ((opcode & 0xFF00) === 0x0F00) { opcode &= 0xFF; (by_opcode0f[opcode] = by_opcode0f[opcode] || []).push(entry); }
	else { opcode &= 0xFF; (by_opcode[opcode] = by_opcode[opcode] || []).push(entry); }
}

const PREFIX_BITS = { 66: "PREFIX_66", F2: "PREFIX_F2", F3: "PREFIX_F3" };
const rows = [];
for (const [table, map] of [["base", by_opcode], ["0F", by_opcode0f]]) {
	for (const key of Object.keys(map)) {
		const encodings = map[key];
		const encoding = encodings[0];
		const has = { 66: [], F2: [], F3: [] }, no_prefix = [];
		for (const e of encodings) {
			if ((e.opcode >>> 16) === 0x66) has["66"].push(e);
			else if ((e.opcode >>> 8 & 0xFF) === 0xF2 || (e.opcode >>> 16) === 0xF2) has.F2.push(e);
			else if ((e.opcode >>> 8 & 0xFF) === 0xF3 || (e.opcode >>> 16) === 0xF3) has.F3.push(e);
			else no_prefix.push(e);
		}
		const arms = (has["66"].length ? 1 : 0) + (has.F2.length ? 1 : 0) + (has.F3.length ? 1 : 0);
		if (arms === 0 || no_prefix.length === 0) continue;

		/* the mask the emitted assert compared, i.e. the prefixes whose presence in a
		 * NON-matching combination reaches the else arm with a non-zero value */
		const mask = encoding.sse ? ["66", "F2", "F3"] : ["F2", "F3"];
		const stray = mask.filter(p => !has[p].length);
		if (!stray.length) continue;

		const plain = no_prefix[0];
		const sizes = plain.os ? [16, 32] : [undefined];
		const symbols = [];
		let implemented = false;
		for (const size of sizes) {
			const q = make_instruction_name(plain, size);
			const suffixes = plain.e ? (plain.ignore_mod ? [""] : ["_mem", "_reg"]) : [""];
			for (const s of suffixes) {
				symbols.push(c_symbol(q) + s);
				if (defined.has(c_symbol(q) + s)) implemented = true;
			}
		}
		rows.push({
			table,
			opcode: hex(plain.opcode, 4),
			sse: !!encoding.sse,
			arms: Object.entries(has).filter(([, v]) => v.length).map(([p, v]) => p + "x" + v.length).join(",") || "-",
			stray,
			symbols,
			implemented,
		});
	}
}

rows.sort((a, b) => (a.table + a.opcode).localeCompare(b.table + b.opcode));
console.log(`PREFIX-GROUP FALLTHROUGH INPUTS (reachable from guest bytes; the arm R3/N1 changed)`);
console.log(`  groups with a prefix arm and a no-prefix body : ${rows.length}`);
console.log(`  distinct stray prefix combinations             : ${rows.reduce((n, r) => n + r.stray.length, 0)}`);
console.log(`  of those, whose STRAY prefix is 66             : ${rows.filter(r => r.stray.includes("66")).length}`);
console.log(`  of those, whose STRAY prefix is F2             : ${rows.filter(r => r.stray.includes("F2")).length}`);
console.log(`  of those, whose STRAY prefix is F3             : ${rows.filter(r => r.stray.includes("F3")).length}`);
const impl = rows.filter(r => r.implemented);
console.log(`  whose UNPREFIXED body is IMPLEMENTED in src/cpu: ${impl.length}`);
console.log("");
if (!impl.length) {
	console.log("  => EVERY reachable fallthrough runs a body the port has not written yet, so the");
	console.log("     port STOPS BY NAME at it (unimplemented_instruction); the reference EXECUTES");
	console.log("     that body.  That divergence is the UNPORTED BODY (M1b), not the fallthrough:");
	console.log("     the same encoding without the stray prefix stops in exactly the same place.");
	console.log("");
}
console.log(`  ${"table".padEnd(5)} ${"opcode".padEnd(7)} ${"sse".padEnd(4)} ${"arms".padEnd(14)} ${"stray".padEnd(9)} unimplemented body`);
for (const r of rows) {
	console.log(`  ${r.table.padEnd(5)} ${("0x" + r.opcode).padEnd(7)} ${String(r.sse).padEnd(4)} ${r.arms.padEnd(14)} ${r.stray.join("+").padEnd(9)} ${r.symbols.join(" ")}`);
}
console.log("");
console.log("  NOTE the two rows the probe uses: 0x0F6F (stray F2 = 'F2 0F 6F C1', results/r3f-probe.asm");
console.log("  MODE=1) and 0x0F7F (stray F2, MODE=4).");
