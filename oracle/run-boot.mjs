#!/usr/bin/env node
/*
 * oracle/run-boot.mjs -- the REFERENCE leg of the M2-2 bzImage boot gate.
 *
 *     run-boot.mjs <kernel> <minibios> <instruction-limit> [cmdline]
 *
 * Boots a real i386 Linux bzImage under the *published* reference implementation (the npm
 * package `v86`, 0.5.471+g94faf3d -- the pin M0 established) and writes the guest's serial
 * console output to stdout, byte for byte, so it can be diffed against the C port's
 * `build/v86c --bzimage ...`.
 *
 * WHAT IS THE REFERENCE HERE, AND WHAT IS NOT:
 *
 *  - The KERNEL'S BRING-UP IS THE REFERENCE'S OWN CODE.  `settings.bzimage` is passed to
 *    the published V86 starter, which hands it to `CPU.prototype.init`
 *    (v86/src/cpu.js:1024-1035), which calls `load_kernel` (v86/src/kernel.js:42-179): the
 *    real-mode setup block at 0x80000, the protected-mode kernel at 0x100000, the boot
 *    params, the command line, the initrd placement, and the option ROM.  Nothing about the
 *    kernel image or its boot parameters is re-implemented in this script.
 *  - The FIRMWARE IS NOT THE REFERENCE'S.  v86 has no BIOS: `load_kernel`'s option ROM is
 *    fetched by seabios through fw_cfg and executed by it (cpu.js:1127-1160; the ROM's own
 *    comment, kernel.js:183).  Seabios is not available here and could not run on the C
 *    port (it needs the VGA and the IDE controller that port has not got), so BOTH machines
 *    run the same purpose-built 16-bit shim -- tests/minibios.asm, assembled to
 *    build/minibios.bin -- and this script does exactly the two things seabios does with a
 *    kernel boot ROM: it copies it to 0xC0000 and it resets the CPU into the firmware.  The
 *    firmware then scans for the ROM, checks its signature and checksum, and far-calls it.
 *  - So the comparison is: the same kernel bytes, in the same memory layout, produced by
 *    each side's own port of `load_kernel`, on two machines running the same firmware
 *    binary.  Everything the kernel does after the option ROM's 8-instruction prologue is
 *    the guest running on the machine under test.
 *
 * STDOUT IS THE SERIAL CONSOLE and nothing else; every diagnostic goes to stderr, so
 * `run-boot.mjs ... > a.txt` and `build/v86c --bzimage ... > b.txt` are comparable.
 */

import fs from "node:fs";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import path from "node:path";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PKG = path.join(HERE, "node_modules", "v86", "build");

/* Fixed machine geometry: 64 MiB, the same number tests/minibios.asm reports in its E820
 * map and the number the C port is configured with.  `--memory` overrides it for a boot
 * that must hold an initrd (see the flag's comment below); both legs take the same
 * number, and the firmware's own E820 map is what reports it to the guest. */
let MEMORY_SIZE = 64 * 1024 * 1024;

/* WHERE THE OPTION ROM IS PUT: 0xC0000 is where seabios copies a fw_cfg option ROM, and
 * where the firmware's scan (0xC0000-0xEFFFF, 2 KiB steps) looks for it. */
const OPTION_ROM_ADDRESS = 0xC0000;

/* WHERE v86's load_bios puts the firmware: `start = 0x100000 - bios.byteLength`
 * (cpu.js:1706-1709).  The reset vector at 0xFFFF0 must land inside it. */
const BIOS_TOP = 0x100000;

const USAGE = "usage: run-boot.mjs <kernel> <minibios> <instruction-limit> [cmdline]" +
	" [--hda FILE] [--initrd FILE] [--memory MIB] [--virtio-console FILE]" +
	" [--virtio-console-in FILE]";

/* The four positional arguments are unchanged (M2-2's contract); the optional flags are
 * added for the M2-3 disk gate.  `--hda` is the same name the C port's boot mode takes,
 * so one command line describes both legs. */
const positional = [];
const flags = {};
{
	const argv = process.argv.slice(2);
	for (let i = 0; i < argv.length; i++) {
		if (argv[i] === "--hda" || argv[i] === "--initrd" || argv[i] === "--memory" ||
		    argv[i] === "--virtio-console" || argv[i] === "--virtio-console-in") {
			if (i + 1 >= argv.length) { console.error(USAGE); process.exit(2); }
			flags[argv[i]] = argv[++i];
		} else {
			positional.push(argv[i]);
		}
	}
}
const [kernel_path, minibios_path, limit_arg, cmdline_arg] = positional;
if (!kernel_path || !minibios_path || !limit_arg) {
	console.error(USAGE);
	process.exit(2);
}
const hda_path = flags["--hda"];
const initrd_path = flags["--initrd"];
/* M2-4: the virtio console.  `--virtio-console` is what the C port's boot mode calls it, and
 * it does the same two things on this side: it sets the reference's own `settings.virtio_console`
 * (cpu.js:1253-1256, the ONLY switch that creates the device -- the reference's default machine
 * has none) and names where the bytes the guest writes come out.  `-` is stdout, which is what
 * a boot whose cmdline carries ONLY `console=hvc0` wants: the kernel's log then arrives on the
 * virtio console and on nothing else. */
const virtio_console_path = flags["--virtio-console"];
const virtio_console_in_path = flags["--virtio-console-in"];
if (flags["--memory"] !== undefined) {
	/* The memory size is a machine geometry both legs must agree on; the default stays
	 * M2-2's 64 MiB.  An initrd needs more: load_kernel puts it at the 64 MiB boundary
	 * (kernel.js:48-50), so an initrd's last byte must be inside this number. */
	const mib = Number(flags["--memory"]);
	if (!Number.isInteger(mib) || mib <= 0) {
		console.error("--memory must be a positive integer count of MiB, got: " + flags["--memory"]);
		process.exit(2);
	}
	MEMORY_SIZE = mib * 1024 * 1024;
}
const instruction_limit = Number(limit_arg);
if (!Number.isInteger(instruction_limit) || instruction_limit <= 0) {
	console.error("instruction limit must be a positive integer, got: " + limit_arg);
	process.exit(2);
}
const cmdline = cmdline_arg === undefined ? "" : cmdline_arg;

/* A node Buffer is a view into a shared pool: .buffer would hand over the pool's bytes at
 * the wrong offset (the trap M0's run.mjs documents).  Slice exact ArrayBuffers. */
function read_exact(file_path)
{
	const buffer = fs.readFileSync(file_path);
	return buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength);
}

const kernel_ab = read_exact(kernel_path);
const minibios_ab = read_exact(minibios_path);
const kernel_sha256 = crypto.createHash("sha256").update(Buffer.from(kernel_ab)).digest("hex");

/* The disk and the initrd are the M2-3 additions: `hda` goes straight into the reference's
 * own IDEController (cpu.js:1221-1228 builds `ide_config[0][0] = {buffer: settings.hda}`),
 * and an initrd is what load_kernel puts in the boot params (kernel.js:157-164). */
let hda_ab = null;
let initrd_ab = null;
if (hda_path) {
	hda_ab = read_exact(hda_path);
	if (hda_ab.byteLength % 512 !== 0) {
		console.error("hda image is not a whole number of 512-byte sectors: " + hda_path);
		process.exit(2);
	}
}
if (initrd_path) {
	initrd_ab = read_exact(initrd_path);
}
const hda_sha256 = hda_ab
	? crypto.createHash("sha256").update(Buffer.from(hda_ab)).digest("hex") : null;

const header = new DataView(kernel_ab);
if (header.getUint16(0x1FE, true) !== 0xAA55)
{
	console.error("not a bzImage: no 0xAA55 boot signature at 0x1FE");
	process.exit(2);
}
if (header.getUint32(0x202, true) !== 0x53726448)
{
	console.error("not a bzImage: no 'HdrS' at 0x202");
	process.exit(2);
}
if (minibios_ab.byteLength !== 0x10000)
{
	console.error("minibios is not 64 KiB (" + minibios_ab.byteLength + " bytes): it must fill 0xF0000-0xFFFFF");
	process.exit(2);
}

/* the inbound half's payload (see the handshake below) */
let virtio_console_in = null;
if (virtio_console_in_path) {
	virtio_console_in = new Uint8Array(fs.readFileSync(virtio_console_in_path));
}

const wasm_bytes = fs.readFileSync(path.join(PKG, "v86.wasm"));
const { V86 } = await import(path.join(PKG, "libv86.mjs"));

/* The console: v86's UART sends "serial0-output-byte" on the device bus (uart.js:408), and
 * the starter's own SerialAdapter is what normally receives it (browser/serial.js).  Here
 * the receiver is a byte sink.  `emulator.bus` is the connector the starter registers its
 * listeners on (bus[0] of Bus.create; the emulator itself sends on bus[1]). */
const console_bytes = [];
let console_closed = false;

const emulator = new V86({
	autostart: false,
	memory_size: MEMORY_SIZE,
	disable_jit: true,
	log_level: 0,
	net_device: { type: null },
	bios: { buffer: minibios_ab, async: false },
	bzimage: { buffer: kernel_ab, async: false },
	hda: hda_ab ? { buffer: hda_ab, async: false } : undefined,
	initrd: initrd_ab ? { buffer: initrd_ab, async: false } : undefined,
	cmdline: cmdline,
	/* {type: "none"} is the starter's own "create the device, attach no adapter"
	 * (starter.js:347: a boolean option becomes {type: "none"}). */
	virtio_console: virtio_console_path ? { type: "none" } : undefined,
	wasm_fn: (imports) => WebAssembly.instantiate(wasm_bytes, imports).then(r => r.instance.exports),
});

emulator.bus.register("serial0-output-byte", (byte) => {
	if (!console_closed)
	{
		console_bytes.push(byte & 0xFF);
	}
});

/* THE VIRTIO CONSOLE'S OUTPUT, the same way the serial port's is taken above: upstream's
 * device sends one message per guest buffer carrying a Uint8Array ("virtio-console0-output-bytes",
 * virtio_console.js:100) and this pushes its bytes into a sink of the same shape the C port's
 * listener writes to (one byte at a time, because that is what the C port's bus carries --
 * virtio_console.h's D-VC1).  The BYTE STREAM is what both legs are compared on. */
const virtio_bytes = [];
let virtio_sink = null;
let virtio_window = "";
let virtio_in_sent = false;
let virtio_console_closed = false;
const VIRTIO_SENTINEL = "GUEST-INIT: waiting for host input";

if (virtio_console_path) {
	/* SYNCHRONOUS writes, and that is a measured requirement: this script ends with
	 * process.exit(0), which discards a createWriteStream's buffered bytes -- the first run
	 * of this gate left exactly ONE byte ("G") of 2689 on disk while reporting all 2689.
	 * `-` means "no file": the bytes then go to stdout, which is where the "-" flag's caller
	 * looks for them. */
	virtio_sink = virtio_console_path === "-" ? null : fs.openSync(virtio_console_path, "w");
	emulator.bus.register("virtio-console0-output-bytes", (data) => {
		if (virtio_console_closed) return;
		const bytes = new Uint8Array(data);
		virtio_bytes.push(...bytes);
		if (virtio_sink === null) process.stdout.write(Buffer.from(bytes));
		else fs.writeSync(virtio_sink, Buffer.from(bytes));
		/* the inbound handshake: the guest says it is listening, the host answers.  It is
		 * needed because upstream's own input path DROPS a byte that arrives with no posted
		 * receive buffer (virtio_console.js:213-215). */
		if (!virtio_console_in || virtio_in_sent) return;
		for (const byte of bytes) {
			virtio_window = (virtio_window + String.fromCharCode(byte)).slice(-VIRTIO_SENTINEL.length);
			if (virtio_window === VIRTIO_SENTINEL) {
				for (const input_byte of virtio_console_in) {
					emulator.bus.send("virtio-console0-input-bytes",
						new Uint8Array([input_byte]));
				}
				virtio_in_sent = true;
				break;
			}
		}
	});
}

await new Promise(resolve => emulator.bus.register("emulator-loaded", resolve));

const cpu = emulator.v86.cpu;

/* The firmware needs the option ROM at 0xC0000 (see the header).  It is the reference's
 * own load_kernel output, still sitting in cpu.option_roms; copy the bytes it produced. */
if (!cpu.option_roms || cpu.option_roms.length !== 1)
{
	console.error("expected exactly one option ROM in cpu.option_roms, got " +
		(cpu.option_roms ? cpu.option_roms.length : "none"));
	process.exit(2);
}
const option_rom = cpu.option_roms[0];
if (option_rom.name !== "genroms/kernel.bin")
{
	console.error("unexpected option ROM name: " + option_rom.name);
	process.exit(2);
}
if (process.env.V86C_NO_ROM)
{
	console.error("reference: NOT writing the option ROM (diagnostic)");
}
else
{
	cpu.write_blob(option_rom.data, OPTION_ROM_ADDRESS);
}

console.error("reference: kernel " + kernel_ab.byteLength + " bytes sha256 " + kernel_sha256);
console.error("reference: firmware at 0x" + (BIOS_TOP - minibios_ab.byteLength).toString(16) +
	", option ROM " + option_rom.data.length + " bytes at 0x" + OPTION_ROM_ADDRESS.toString(16) +
	", cmdline '" + cmdline + "'");
console.error("reference: memory " + (MEMORY_SIZE >> 20) + " MiB, hda " +
	(hda_ab ? hda_ab.byteLength + " bytes sha256 " + hda_sha256 : "none") + ", initrd " +
	(initrd_ab ? initrd_ab.byteLength + " bytes" : "none"));

/* Reset state check: the CPU must be at the reset vector with the firmware loaded, which is
 * what cpu.init left behind (cpu.js:1022 reset_cpu, before load_bios/load_kernel). */
console.error("reference: reset eip=0x" + (cpu.instruction_pointer[0] >>> 0).toString(16) +
	" cs=0x" + (cpu.sreg[1] >>> 0).toString(16));

let exception = null;
emulator.cpu_exception_hook = function(n)
{
	exception = n;
	return true;
};

/* Bounded, synchronous run: main_loop() is exactly what the reference's own driver calls
 * (main.js: `const t = this.cpu.main_loop()`), and it is the same shape M0's run.mjs uses.
 * The stop conditions: the guest halted (in_hlt), the instruction limit, or a main_loop()
 * that made no progress at all. */
const trace = process.env.V86C_BOOT_TRACE;
let printed_bytes = 0;
let last_sample = 0;
let previous_count = -1;
for (;;)
{
	cpu.main_loop();
	if (trace && console_bytes.length > printed_bytes)
	{
		process.stderr.write(Buffer.from(console_bytes.slice(printed_bytes)));
		printed_bytes = console_bytes.length;
	}
	const count = cpu.instruction_counter[0];
	if (trace === "2" && count - last_sample >= 10000000)
	{
		last_sample = count;
		console.error("reference: @" + count + " eip=0x" + (cpu.instruction_pointer[0] >>> 0).toString(16) +
			" cs=0x" + (cpu.sreg[1] >>> 0).toString(16) + " cr0=0x" + (cpu.cr[0] >>> 0).toString(16) +
			" cr3=0x" + (cpu.cr[3] >>> 0).toString(16) + " serial=" + console_bytes.length);
	}
	if (cpu.in_hlt[0] || count >= instruction_limit || count === previous_count)
	{
		break;
	}
	previous_count = count;
}

console_closed = true;

const state = {
	instructions: cpu.instruction_counter[0],
	limit: instruction_limit,
	in_hlt: cpu.in_hlt[0],
	eip: "0x" + (cpu.instruction_pointer[0] >>> 0).toString(16),
	cr0: "0x" + (cpu.cr[0] >>> 0).toString(16),
	cr3: "0x" + (cpu.cr[3] >>> 0).toString(16),
	cs: "0x" + (cpu.sreg[1] >>> 0).toString(16),
	exception: exception,
	serial_bytes: console_bytes.length,
	virtio_console_bytes: virtio_bytes.length,
	virtio_console_in_sent: virtio_in_sent,
};
console.error("reference: " + JSON.stringify(state));
if (process.env.V86C_DUMP)
{
	const hex = (addr, n) => Array.from(cpu.mem8.subarray(addr, addr + n))
		.map(b => b.toString(16).padStart(2, "0")).join(" ");
	console.error("mem 0xC0000 (option ROM): " + hex(0xC0000, 32));
	console.error("mem 0x400   (BDA):        " + hex(0x400, 32));
	console.error("mem 0x80000 (setup area): " + hex(0x80000, 32));
	console.error("mem 0x80200 (setup code): " + hex(0x80200, 32));
	console.error("mem 0xFFFF0 (reset vec):  " + hex(0xFFFF0, 16));
	console.error("IVT 0x10:  " + hex(0x40, 8) + "   IVT 0x15: " + hex(0x54, 8) + "   IVT 0x1A: " + hex(0x68, 8));
}

process.stdout.write(Buffer.from(console_bytes));
if (virtio_sink !== null && virtio_sink !== undefined) fs.closeSync(virtio_sink);
emulator.destroy();
process.exit(0);
