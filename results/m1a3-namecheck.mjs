// M1a-3 naming cross-check (AGENTS.md: "the pointer is not the thing").
// make_instruction_name is copied VERBATIM from v86/gen/generate_interpreter.js:87-101 --
// importing that module would RUN the generator -- and the table is the real
// v86/gen/x86_table.js.  Run: node results/m1a3-namecheck.mjs
// Cross-check of the C symbol names against gen/generate_interpreter.js's naming scheme.
// make_instruction_name is copied VERBATIM from v86/gen/generate_interpreter.js:87-101
// (importing that module would run its generator); the table is imported for real.
import x86_table from "/home/jaye/futaie/souche/v86/gen/x86_table.js";

const hex = (n, pad) => n.toString(16).toUpperCase().padStart(pad, "0");

function make_instruction_name(encoding, size)
{
    const suffix = encoding.os ? String(size) : "";
    const opcode_hex = hex(encoding.opcode & 0xFF, 2);
    const first_prefix = (encoding.opcode & 0xFF00) === 0 ? "" : hex(encoding.opcode >> 8 & 0xFF, 2);
    const second_prefix = (encoding.opcode & 0xFF0000) === 0 ? "" : hex(encoding.opcode >> 16 & 0xFF, 2);
    const fixed_g_suffix = encoding.fixed_g === undefined ? "" : `_${encoding.fixed_g}`;
    const module = first_prefix === "0F" || second_prefix === "0F" ? "instructions_0f" : "instructions";
    return `${module}::instr${suffix}_${second_prefix}${first_prefix}${opcode_hex}${fixed_g_suffix}`;
}

const base = [], f0f = [];
for(const o of x86_table) {
    const m = make_instruction_name(o, o.os ? 16 : undefined);
    const mod = m.split("::")[0];
    // base-table encodings: everything the generator routes to `instructions`
    (mod === "instructions" ? base : f0f).push({o, m});
}

const names = new Set();
for(const {o, m} of base) {
    for(const size of (o.os ? [16, 32] : [undefined])) {
        const n = make_instruction_name(o, size);
        if(o.ignore_mod) { names.add(n); continue; }
        if(o.e) { names.add(n + "_mem"); names.add(n + "_reg"); }
        else { names.add(n); }
    }
}
console.log("base encodings:", base.length, "0F encodings:", f0f.length);
console.log("distinct base-table C symbols the emitter will call:", names.size);
console.log(JSON.stringify([...names].sort()));
