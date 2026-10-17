#!/usr/bin/env node
/*
 * oracle/run-vga.mjs -- the REFERENCE leg of the VGA document (VGA-U0, decision 2).
 *
 *     node oracle/run-vga.mjs <image> <instruction-limit> <output-path>
 *
 * The C leg is `build/v86c <image> <limit> --vga-doc <output-path>`, and the gate between
 * them is `cmp` of the two files: same image, same stop, byte-identical document.
 *
 * THE MACHINE IS run.mjs's, EXACTLY.  Same npm package (v86 0.5.471+g94faf3d), same 2 MiB
 * memory, same multiboot bring-up, same interpreter-only run, same stop condition (the
 * guest's HLT, with the instruction limit as a hard upper bound, reported at a main_loop
 * chunk boundary).  Only the SCREEN differs -- and in the reference that is a pluggable
 * object: `this.screen` is vga.js:70's constructor argument, every one of the device's 26
 * rendering call sites is `this.screen.<method>`, and v86/src/browser/starter.js:307-319
 * picks the adapter from `options.screen`.  Our harness passes none, so the reference's
 * DummyScreenAdapter is installed at construction (MEASURED: decision 1's "headless is the
 * default" is upstream's own shape as well).
 *
 * WHY THE RECORDER IS SWAPPED IN AND THE GRID IS REPLAYED AT THE STOP.
 *   (a) The swap CANNOT happen before construction: the device object does not exist until
 *       "emulator-loaded".  `vga.screen = recorder` after that REDIRECTS every subsequent
 *       call -- MEASURED on this pinned build: no error, and zero post-swap calls on a
 *       VGA-blind image, which proves the constructor-time work already ran.
 *   (b) The harness therefore never hand-seeds the grid.  At the stop it calls the DEVICE'S
 *       OWN public methods to replay the current state through the recorder:
 *         vga.text_mode_redraw()          -> put_char for every cell (vga.js:841-880)
 *         vga.update_cursor()             -> update_cursor (vga.js:937-957)
 *         vga.update_cursor_scanline()    -> update_cursor_scanline (vga.js:1369-1375)
 *       The device does its own palette/attribute math; the harness reimplements NOTHING.
 *       A replay is what makes the document a function of the DEVICE'S STATE rather than of
 *       the call stream, which is what the C leg can also produce.
 *   (c) The recorder DEFINES screen.FLAG_BLINKING / FLAG_FONT_PAGE_B -- it stands in for the
 *       canvas adapter, which is the only browser adapter that defines them
 *       (screen.js:30-34).  That is load-bearing for the flags byte (vga.js:848-849).
 *
 * GRAPHICAL MODES ARE NOT DOCUMENTED HERE (nor by the C leg): the payload would be the dest
 * buffer's pixels and unit U2 owns that path.  This leg refuses them by name.
 *
 * The document format is src/hw/vga.h's: a magic line, one line of canonical JSON, the raw
 * payload.
 */

import fs from "node:fs";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import path from "node:path";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PKG = path.join(HERE, "node_modules", "v86", "build");

const MEMORY_SIZE = 2 * 1024 * 1024; // run.mjs:36
const LOAD_ADDRESS = 0x100000;
const ENTRY_ADDRESS = 0x100020;      // LOAD_ADDRESS + sizeof(multiboot header)

const [image_path, limit_arg, output_path] = process.argv.slice(2);
if(!image_path || !limit_arg || !output_path)
{
    console.error("usage: run-vga.mjs <image> <instruction-limit> <output-path>");
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
    disable_jit: true,
    log_level: 0,
    net_device: { type: null },
    wasm_fn: (imports) => WebAssembly.instantiate(wasm_bytes, imports).then(r => r.instance.exports),
});

await new Promise(resolve => emulator.bus.register("emulator-loaded", resolve));

const cpu = emulator.v86.cpu;
const vga = cpu.devices.vga;
if(!vga)
{
    console.error("the reference's machine has no VGA device: the pinned build is not the one"
        + " this leg was verified against");
    process.exit(1);
}

// THE RECORDER: the eleven methods the device calls (vga.h's seam), plus the two flag
// constants the canvas adapter defines.  Cells are recorded per (row, col); the grid is
// sized at the stop from the DEVICE's max_cols/max_rows, so a cell the device never sent is
// all zeros -- exactly what the C writer emits for an unsized/unwritten cell.
const recorded_chars = new Map();
let cursor_row = 0;
let cursor_col = 0;
let scanline_start = 0;
let scanline_end = 0;
let scanline_visible = false;

const recorder = {
    FLAG_BLINKING: 0x01,
    FLAG_FONT_PAGE_B: 0x02,

    set_mode(graphical) { void graphical; },
    set_size_text(cols, rows) { void cols; void rows; },
    set_size_graphical(width, height, virtual_width, virtual_height)
    {
        void width; void height; void virtual_width; void virtual_height;
    },
    set_font_page(page_a, page_b) { void page_a; void page_b; },
    set_font_bitmap(height, width_9px, width_dbl, copy_8th_col, bitmap, bitmap_changed)
    {
        void height; void width_9px; void width_dbl; void copy_8th_col; void bitmap;
        void bitmap_changed;
    },
    put_char(row, col, chr, flags, bg_color, fg_color)
    {
        recorded_chars.set(row + "," + col, [chr & 0xFF, flags & 0xFF, bg_color >>> 0, fg_color >>> 0]);
    },
    update_buffer(layers) { void layers; },
    update_cursor(row, col) { cursor_row = row; cursor_col = col; },
    update_cursor_scanline(start, end, visible)
    {
        scanline_start = start;
        scanline_end = end;
        scanline_visible = visible;
    },
    clear_screen() {},
    clear_text_state() { recorded_chars.clear(); },
};

// The swap, after construction (see (a) above).
vga.screen = recorder;

let exception = null;
emulator.cpu_exception_hook = function(n)
{
    exception = n;
    return true;
};

cpu.reboot_internal();
cpu.reset_memory();
cpu.load_multiboot(image_ab);

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
    console.error("guest raised cpu exception " + exception + " after " + instruction_count
        + " instructions; no document produced");
    process.exit(1);
}
if(!cpu.in_hlt[0])
{
    console.error("guest did not halt within " + instruction_limit + " instructions (reached "
        + instruction_count + ", in_hlt=" + cpu.in_hlt[0] + ")");
    process.exit(1);
}
if(vga.graphical_mode)
{
    console.error("refusing to write a GRAPHICAL document: graphical_mode is set and its"
        + " payload is the dest buffer's pixels, which unit U2 owns");
    process.exit(1);
}

// THE REPLAY, from the device's own state.
vga.text_mode_redraw();
vga.update_cursor();
vga.update_cursor_scanline();

const cols = vga.max_cols;
const rows = vga.max_rows;
const payload_bytes = rows * cols * 10;

const payload = Buffer.alloc(payload_bytes);
let written = 0;
for(let row = 0; row < rows; row++)
{
    for(let col = 0; col < cols; col++)
    {
        const cell = recorded_chars.get(row + "," + col);
        if(cell)
        {
            payload.writeUInt8(cell[0], written);
            payload.writeUInt8(cell[1], written + 1);
            payload.writeUInt32LE(cell[2], written + 2);
            payload.writeUInt32LE(cell[3], written + 6);
        }
        // a cell the device never sent stays zero -- see the recorder's note
        written += 10;
    }
}

const document_header = {
    bpp: vga.svga_bpp,
    cols: cols,
    cursor_col: cursor_col,
    cursor_row: cursor_row,
    cursor_scanline: { end: scanline_end, start: scanline_start, visible: scanline_visible },
    format: 1,
    graphical_mode: vga.graphical_mode,
    image_bytes: image.byteLength,
    image_sha256: image_sha256,
    instruction_count: instruction_count,
    instruction_limit: instruction_limit,
    mode: "text",
    payload_bytes: payload_bytes,
    rows: rows,
    screen_height: vga.screen_height,
    screen_width: vga.screen_width,
    svga_enabled: vga.svga_enabled,
    virtual_height: vga.virtual_height,
    virtual_width: vga.virtual_width,
};

// Canonical: keys sorted at every level, no whitespace (run.mjs:184-196's form, which is
// what makes this header byte-comparable with the C writer's hand-built line).
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

const header_line = canon(document_header);
// A guard against the one silent divergence this leg cannot see: the C writer sorts its keys
// by hand, so a boolean or a number formatting differently (true vs 1) would show up as a
// document diff with no explanation.  The keys are asserted here, the bytes there.
if(JSON.stringify(Object.keys(document_header).sort()) !== JSON.stringify(Object.keys(document_header)))
{
    console.error("document_header's keys are not in sorted order; the C writer's are");
    process.exit(1);
}

fs.writeFileSync(output_path, Buffer.concat([
    Buffer.from("v86c-vga-doc/1\n", "ascii"),
    Buffer.from(header_line + "\n", "ascii"),
    payload,
]));

emulator.destroy();
process.exit(0);
