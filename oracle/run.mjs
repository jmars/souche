#!/usr/bin/env node
/*
 * oracle/run.mjs -- the reference oracle for the v86-to-C port.
 *
 * Boots a flat test image under the *published* reference implementation (the
 * npm package `v86`) and writes a canonical machine-state transcript to stdout.
 * The same argument surface is what the C binary must implement:
 *
 *     run.mjs <image> <instruction-limit>
 *
 * Reference implementation: npm v86 0.5.471 (package version string
 * "0.5.471+g94faf3d"), build/libv86.mjs + build/v86.wasm. The image is loaded
 * by the reference's own multiboot loader (CPU.prototype.load_multiboot), the
 * same path v86's own tests/nasm/run.js uses, so the machine bring-up is the
 * reference's code and not a re-implementation. See README.md for the exact
 * initial-state contract this implies for the C side.
 *
 * Stop condition: the guest's HLT, with the instruction limit as a hard upper
 * bound. NOT wall-clock: main_loop() runs the interpreter in chunks of
 * LOOP_COUNTER (= 100003) instructions and only returns at a chunk boundary,
 * so an exact "stop at instruction N" is not expressible through the public
 * API -- the achieved count is reported instead, and it is deterministic.
 */

import fs from "node:fs";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import path from "node:path";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PKG = path.join(HERE, "node_modules", "v86", "build");

// Fixed machine geometry. MEASURED: the memory window and stack top are the
// brief's geometry (code at 0x100000, data window and stack inside
// [0x101000, 0x102000)), see README.md.
const MEMORY_SIZE = 2 * 1024 * 1024;      // 2 MiB, as in v86/tests/nasm/run.js
const LOAD_ADDRESS = 0x100000;
const ENTRY_ADDRESS = 0x100020;           // LOAD_ADDRESS + sizeof(multiboot header)
const WINDOW_ADDRESS = 0x101000;
const WINDOW_BYTES = 4096;

const [image_path, limit_arg] = process.argv.slice(2);
if(!image_path || !limit_arg)
{
    console.error("usage: run.mjs <image> <instruction-limit>");
    process.exit(2);
}
const instruction_limit = Number(limit_arg);
if(!Number.isInteger(instruction_limit) || instruction_limit <= 0)
{
    console.error("instruction limit must be a positive integer, got: " + limit_arg);
    process.exit(2);
}

const image = fs.readFileSync(image_path);
const image_sha256 = crypto.createHash("sha256").update(image).digest("hex");

// A node Buffer is a view into a shared pool: handing Buffer.buffer to the
// loader would pass the pool's bytes at the wrong offset. Slice an exact
// ArrayBuffer instead (this is a trap v86's own tests/nasm/run.js steps into
// for images smaller than the pool).
const image_ab = image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength);
const header = new DataView(image_ab);
if(header.getUint32(0, true) !== 0x1BADB002)
{
    console.error("image does not start with a multiboot header");
    process.exit(2);
}
if(header.getUint32(16, true) !== LOAD_ADDRESS || header.getUint32(28, true) !== ENTRY_ADDRESS)
{
    console.error("image multiboot header does not match the fixed load/entry contract");
    process.exit(2);
}

const wasm_bytes = fs.readFileSync(path.join(PKG, "v86.wasm"));
const { V86 } = await import(path.join(PKG, "libv86.mjs"));

const emulator = new V86({
    autostart: false,
    memory_size: MEMORY_SIZE,
    disable_jit: true,          // interpreter only: the C port is an interpreter
    log_level: 0,
    net_device: { type: null }, // no network device
    // wasm_fn instead of wasm_path: in node there is no fetch() of a relative path
    wasm_fn: (imports) => WebAssembly.instantiate(wasm_bytes, imports).then(r => r.instance.exports),
});

await new Promise(resolve => emulator.bus.register("emulator-loaded", resolve));

const cpu = emulator.v86.cpu;
let exception = null;
emulator.cpu_exception_hook = function(n)
{
    exception = n;
    return true;
};

cpu.reboot_internal();
cpu.reset_memory();
cpu.load_multiboot(image_ab);

// Bounded, synchronous run: main_loop() is exactly what the reference's own
// driver calls (v86/src/main.js: `const t = this.cpu.main_loop()`).
let previous_count = -1;
for(;;)
{
    cpu.main_loop();
    const count = cpu.instruction_counter[0];
    if(cpu.in_hlt[0] || count >= instruction_limit || count === previous_count)
    {
        break;
    }
    previous_count = count;
}

const instruction_count = cpu.instruction_counter[0];

if(exception !== null)
{
    console.error("guest raised cpu exception " + exception + " after " +
        instruction_count + " instructions; transcript not produced");
    process.exit(1);
}
if(!cpu.in_hlt[0])
{
    console.error("guest did not halt within " + instruction_limit +
        " instructions (reached " + instruction_count + ", in_hlt=" + cpu.in_hlt[0] + ")");
    process.exit(1);
}

const hx = (value, digits) => "0x" + (value >>> 0).toString(16).padStart(digits, "0");
const f64_hex = value =>
{
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, value, true);
    return hx(dv.getUint32(4, true), 8) + hx(dv.getUint32(0, true), 8);
};

const memory_window = cpu.mem8.subarray(WINDOW_ADDRESS, WINDOW_ADDRESS + WINDOW_BYTES);

const transcript = {
    cpu: {
        eip: hx(cpu.instruction_pointer[0], 8),
        eflags: hx(cpu.get_eflags(), 8),
        gpr: {
            eax: hx(cpu.reg32[0], 8), ebx: hx(cpu.reg32[3], 8),
            ecx: hx(cpu.reg32[1], 8), edx: hx(cpu.reg32[2], 8),
            esi: hx(cpu.reg32[6], 8), edi: hx(cpu.reg32[7], 8),
            ebp: hx(cpu.reg32[5], 8), esp: hx(cpu.reg32[4], 8),
        },
        control_registers: {
            cr0: hx(cpu.cr[0], 8), cr2: hx(cpu.cr[2], 8), cr3: hx(cpu.cr[3], 8),
        },
        segments: Object.fromEntries(["es", "cs", "ss", "ds", "fs", "gs"].map((name, i) => [name, {
            selector: hx(cpu.sreg[i], 4),
            base: hx(cpu.segment_offsets[i], 8),
            limit: hx(cpu.segment_limits[i], 8),
        }])),
        x87: {
            control_word: hx(cpu.fpu_control_word[0], 4),
            status_word: hx(cpu.fpu_load_status_word(), 4),
            tag_word: hx(cpu.fpu_load_tag_word(), 4),
            stack_ptr: hx(cpu.fpu_stack_ptr[0], 4),
            stack_empty: hx(cpu.fpu_stack_empty[0], 2),
            data: Array.from({ length: 8 }, (_, i) => f64_hex(cpu.fpu_get_sti_f64(i))),
        },
    },
    image: {
        bytes: image.byteLength,
        sha256: image_sha256,
        load_address: hx(LOAD_ADDRESS, 8),
        entry_address: hx(ENTRY_ADDRESS, 8),
    },
    instruction_count: instruction_count,
    instruction_limit: instruction_limit,
    memory_window: {
        address: hx(WINDOW_ADDRESS, 8),
        bytes: memory_window.length,
        sha256: crypto.createHash("sha256").update(memory_window).digest("hex"),
    },
};

// Canonical: keys sorted at every level, 2-space indent, one trailing newline.
function canon(value)
{
    if(Array.isArray(value))
    {
        return "[" + value.map(canon).join(",") + "]";
    }
    if(value !== null && typeof value === "object")
    {
        const keys = Object.keys(value).sort();
        return "{" + keys.map(k => JSON.stringify(k) + ":" + canon(value[k])).join(",") + "}";
    }
    return JSON.stringify(value);
}

process.stdout.write(JSON.stringify(JSON.parse(canon(transcript)), null, 2) + "\n");
emulator.destroy();
process.exit(0);
