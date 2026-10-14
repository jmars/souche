#!/usr/bin/env node
/*
 * oracle/nasm_state.mjs -- the JS-reference leg of the nasm differential oracle.
 *
 * Runs one translated nasm test image (an i386 ELF, built by tests/nasm_run.py at
 * upstream's geometry) under the *pinned* reference implementation -- oracle/node_modules/v86
 * ("0.5.471+g94faf3d", the clone's HEAD commit, see oracle/package.json) -- and prints the
 * machine state in the SAME fixture format the native harness (tests/mkfixture.c) and
 * upstream's gdb script (v86/tests/nasm/gdb-extract-def) produce:
 *
 *     (signal SIGSEGV)           <- optional: the x86 exception the guest took
 *     ---BEGIN JSON---
 *     [ 8 reg32, eip, 8 st, 16 mm words, 32 xmm words, 2048 mem words, eflags, ftag, fstat ]
 *     ---END JSON---
 *
 * The comparison the fixture feeds is run.js's own (v86/tests/nasm/run.js:340-510), so the
 * two legs are interchangeable as far as tests/nasm_run.py is concerned. The CPU exception is
 * printed as the signal name run.js's extract_json maps back to an exception (0 -> DE ->
 * SIGFPE, 6 -> UD -> SIGILL, 13 -> GP -> SIGSEGV, 14 -> PF -> SIGBUS); the emulator's
 * exception hook, the hlt patch, the stop-on-halt and the state reads are transcribed from
 * run.js's worker (run.js:324-380) with M0's bounded main_loop() drive (oracle/run.mjs:104-114).
 *
 * Exit: 0 = reached halt, 3 = did not halt within the instruction limit (fixture still
 * printed, so the caller can report the mismatch rather than lose the run).
 */

import fs from "node:fs";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import path from "node:path";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PKG = path.join(HERE, "node_modules", "v86", "build");

const MEMORY_SIZE = 2 * 1024 * 1024;
const BSS = 0x100000;                       // run.js:38
const STACK_TOP = 0x102000;                 // run.js:39

const [image_path, limit_arg = "1000000"] = process.argv.slice(2);
if(!image_path)
{
    console.error("usage: nasm_state.mjs <image> [instruction-limit]");
    process.exit(2);
}
const instruction_limit = Number(limit_arg);
if(!Number.isInteger(instruction_limit) || instruction_limit <= 0)
{
    console.error("instruction limit must be a positive integer, got: " + limit_arg);
    process.exit(2);
}

// Buffer -> exact ArrayBuffer: a Buffer is a view into a shared pool, so
// Buffer.buffer + byteOffset is what the loader must see (run.js:261 gets this wrong).
const image = fs.readFileSync(image_path);
const image_ab = image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength);

const wasm_bytes = fs.readFileSync(path.join(PKG, "v86.wasm"));
const { V86 } = await import(path.join(PKG, "libv86.mjs"));

const emulator = new V86({
    autostart: false,
    memory_size: MEMORY_SIZE,
    disable_jit: true,          // the C port is an interpreter; compare interpreter to interpreter
    log_level: 0,
    wasm_fn: (imports) => WebAssembly.instantiate(wasm_bytes, imports).then(r => r.instance.exports),
});

await new Promise(resolve => emulator.bus.register("emulator-loaded", resolve));

const cpu = emulator.v86.cpu;

// run.js:333-356 -- map the vector, patch a hlt over the faulting instruction so execution
// can continue to a halt, record it.
// DECLARED DIVERGENCE FROM UPSTREAM: run.js:329-341 recognises only {0: DE, 6: UD,
// 13: GP} and aborts (console.error + exit 1) on any other vector.  The 14 -> SIGBUS
// entry is a deliberate extension -- the C leg checks #PF against the native fixture
// and the reference takes no #PF on any corpus test, and no corpus fixture produces
// SIGBUS, so it is unreachable today.  If a future corpus test made the *reference*
// take #PF, this leg would record-and-compare where upstream would abort.
const EXCEPTION_SIGNAL = { 0: "SIGFPE", 6: "SIGILL", 13: "SIGSEGV", 14: "SIGBUS" };
const recorded_exceptions = [];
let unexpected_exception = null;

emulator.cpu_exception_hook = function(n)
{
    const signal = EXCEPTION_SIGNAL[n];
    if(signal === undefined)
    {
        unexpected_exception = n;
        return true;
    }
    const eip = cpu.instruction_pointer[0];
    cpu.write32(cpu.translate_address_system_read(eip), 0xF4F4F4F4);
    recorded_exceptions.push({ exception: signal, eip });
    return true;
};

let halted = false;
emulator.bus.register("cpu-event-halt", function() { halted = true; });

cpu.reboot_internal();
cpu.reset_memory();
cpu.load_multiboot(image_ab);

let previous_count = -1;
for(;;)
{
    cpu.main_loop();
    const count = cpu.instruction_counter[0];
    if(halted || cpu.in_hlt[0] || count >= instruction_limit || count === previous_count)
    {
        break;
    }
    previous_count = count;
}

if(unexpected_exception !== null)
{
    console.error("unexpected CPU exception: " + unexpected_exception);
    process.exit(1);
}

const at_halt = halted || cpu.in_hlt[0];

// run.js:375-380 -- the evaluated side of the comparison.
const fpu_regs = new Float64Array(8).map((_, i) => cpu.fpu_get_sti_f64(i));
const mmxs = new Int32Array(16).map((_, i) => cpu.fpu_st[(i & ~1) << 1 | (i & 1)]);
const xmms = cpu.reg_xmm32s;
const memory = new Int32Array(cpu.mem8.buffer, cpu.mem8.byteOffset + BSS, STACK_TOP - BSS >> 2);
const fpu_tag = cpu.fpu_load_tag_word();
const fpu_status = cpu.fpu_load_status_word();
const eflags = cpu.get_eflags();

const signal = recorded_exceptions.length ? recorded_exceptions[0].exception : null;
const eip = recorded_exceptions.length ?
    recorded_exceptions[0].eip : (cpu.instruction_pointer[0] | 0);

function float_value(v)
{
    if(v !== v) return "nan";
    if(v === Infinity) return "inf";
    if(v === -Infinity) return "-inf";
    return v.toString();
}

// The "invalid" test is gdb-extract-def's: an abridged tag word makes gdb bail out on an
// empty register, and run.js skips the comparison for those entries.
const top = fpu_status >> 11 & 7;

const out = [];
if(signal) out.push("(signal " + signal + ")\n");
out.push("---BEGIN JSON---\n[\n");
for(let i = 0; i < 8; i++) out.push("    " + (cpu.reg32[i] | 0) + ",\n");
out.push("    " + (eip | 0) + ",\n");
for(let i = 0; i < 8; i++)
{
    const phys = (top + i) & 7;
    if((fpu_tag >> 2 * phys & 3) === 2) out.push("    \"invalid\",\n");
    else out.push("    " + float_value(fpu_regs[i]) + ",\n");
}
for(let i = 0; i < 16; i++) out.push("    " + (mmxs[i] | 0) + ",\n");
for(let i = 0; i < 32; i++) out.push("    " + (xmms[i] | 0) + ",\n");
for(let i = 0; i < memory.length; i++)
{
    out.push("    " + (memory[i] | 0) + (i % 8 === 7 ? ",\n" : ", "));
}
out.push("    " + (eflags | 0) + ",\n");
out.push("    " + (fpu_tag | 0) + ",\n");
out.push("    " + (fpu_status | 0) + "\n");
out.push("]\n---END JSON---\n");

process.stdout.write(out.join(""));
emulator.destroy();

if(!at_halt)
{
    console.error("guest did not halt within " + instruction_limit + " instructions");
    process.exit(3);
}
process.exit(0);
