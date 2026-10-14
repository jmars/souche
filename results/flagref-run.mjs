/* preserved from /tmp/flagref (the M1a-1 reference cross-check) */
/* Run the generated flag-case image under the *reference* npm v86 and print the
 * masked EFLAGS it materialized for each case.  Read-only use of the pinned package
 * in the tree's oracle/node_modules (the same recipe as oracle/run.mjs). */
import fs from "node:fs";
import path from "node:path";

const PKG = "/home/jaye/futaie/souche/oracle/node_modules/v86/build";
const MEMORY_SIZE = 2 * 1024 * 1024;
const BASE = 0x101000;
const CASES = 24;

const image = fs.readFileSync("/tmp/flagref/flagcases.img");
const image_ab = image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength);
const wasm_bytes = fs.readFileSync(path.join(PKG, "v86.wasm"));
const { V86 } = await import(path.join(PKG, "libv86.mjs"));

const emulator = new V86({
    autostart: false, memory_size: MEMORY_SIZE, disable_jit: true, log_level: 0,
    net_device: { type: null },
    wasm_fn: (imports) => WebAssembly.instantiate(wasm_bytes, imports).then(r => r.instance.exports),
});
await new Promise(resolve => emulator.bus.register("emulator-loaded", resolve));
const cpu = emulator.v86.cpu;
let exception = null;
emulator.cpu_exception_hook = function (n) { exception = n; return true; };
cpu.reboot_internal();
cpu.reset_memory();
cpu.load_multiboot(image_ab);

let prev = -1;
for (;;) {
    cpu.main_loop();
    const count = cpu.instruction_counter[0];
    if (cpu.in_hlt[0] || count >= 1000000 || count === prev) break;
    prev = count;
}
if (exception !== null) { console.error("guest exception " + exception); process.exit(1); }
if (!cpu.in_hlt[0]) { console.error("no hlt"); process.exit(1); }

const MASK = 0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x800; /* CF PF AF ZF SF OF */
const dv = new DataView(cpu.mem8.buffer, cpu.mem8.byteOffset, cpu.mem8.byteLength);
for (let i = 0; i < CASES; i++) {
    const flags = dv.getUint32(BASE + 4 * i, true);
    console.log("CASE " + String(i).padStart(2) + " ref=0x" + (flags & MASK).toString(16).padStart(3, "0"));
}
console.log("reference instruction count: " + cpu.instruction_counter[0]);
