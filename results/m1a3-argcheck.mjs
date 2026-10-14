// M1a-3 argument-shape cross-check: how many arguments the generator passes to each
// base-table instruction call, vs the parameter count declared in src/cpu/instructions.h.
// The generator's argument construction is reproduced from generate_interpreter.js:234-337.
// Run: node results/m1a3-argcheck.mjs   (prints one line per mismatch)
// Argument-shape cross-check: how many arguments does the generator pass to each base-table
// instruction call, vs how many parameters the C header declares?  The generator's own
// argument construction is reproduced from v86/gen/generate_interpreter.js:234-337.
import x86_table from "/home/jaye/futaie/souche/v86/gen/x86_table.js";
import fs from "node:fs";

const hex=(n,pad)=>n.toString(16).toUpperCase().padStart(pad,"0");
function make_instruction_name(encoding, size) {
    const suffix = encoding.os ? String(size) : "";
    const opcode_hex = hex(encoding.opcode & 0xFF, 2);
    const first_prefix = (encoding.opcode & 0xFF00) === 0 ? "" : hex(encoding.opcode >> 8 & 0xFF, 2);
    const second_prefix = (encoding.opcode & 0xFF0000) === 0 ? "" : hex(encoding.opcode >> 16 & 0xFF, 2);
    const fixed_g_suffix = encoding.fixed_g === undefined ? "" : `_${encoding.fixed_g}`;
    const module = first_prefix === "0F" || second_prefix === "0F" ? "instructions_0f" : "instructions";
    return `${module}::instr${suffix}_${second_prefix}${first_prefix}${opcode_hex}${fixed_g_suffix}`;
}
function gen_read_imm_call(op, size_variant) {
    let size = (op.os || op.opcode % 2 === 1) ? size_variant : 8;
    if(op.imm8 || op.imm8s || op.imm16 || op.imm1632 || op.imm32 || op.immaddr) {
        /* every branch returns an imm call that the generator counts as ONE argument */
        return "imm";
    }
    return undefined;
}
const expect = new Map(); // name -> arg count
for(const o of x86_table) {
    if((o.opcode & 0xFF00) === 0x0F00) continue;         /* 0F table -> instructions_0f */
    for(const size of (o.os ? [16, 32] : [undefined])) {
        const n = make_instruction_name(o, size).split("::")[1];
        
        const imm = gen_read_imm_call(o, size);
        if(o.ignore_mod) { expect.set(n, 2); continue; }
        let memArgs = 0, regArgs = 0;
        if(o.e) { memArgs = 1; regArgs = 1; }
        if(o.fixed_g === undefined) { if(o.e) { memArgs++; regArgs++; } }
        if(imm) { memArgs++; regArgs++; }
        if(o.extra_imm16 || o.extra_imm8) { if(!imm) throw new Error("extra imm without imm"); memArgs++; regArgs++; }
        expect.set(n + (o.e ? "_mem" : ""), memArgs);
        if(o.e) expect.set(n + "_reg", regArgs);
    }
}
/* the header's parameter counts */
const hdr = fs.readFileSync("/home/jaye/futaie/souche/src/cpu/instructions.h","utf8");
const have = new Map();
for(const line of hdr.split("\n")) {
    const m = /^(?:void|int32_t|bool) (\w+)\(([^)]*)\);$/.exec(line);
    if(!m) continue;
    const args = m[2].trim() === "void" || m[2].trim() === "" ? 0 : m[2].split(",").length;
    have.set(m[1], args);
}
let bad = 0, missing = 0;
for(const [n, cnt] of expect) {
    if(!have.has(n)) { missing++; continue; }   /* printed by name only if -v */
    if(have.get(n) !== cnt) { console.log("ARITY MISMATCH:", n, "generator passes", cnt, "header declares", have.get(n)); bad++; }
}
console.log("base-table calls checked:", expect.size,
            "| arity mismatches:", bad,
            "| declared in the header:", expect.size - missing,
            "| not declared (the D8..DF group deferred to M1b):", missing);
