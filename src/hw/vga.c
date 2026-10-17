/*
 * vga.c -- the VGA device's U0 seats: the 8 MiB VRAM, the 0xA0000 window, the PCI
 * function 00:12.0, and the adapter installation.  See vga.h for what this unit does and
 * (at length) what it does not.
 *
 * WHAT THE MACHINE ANSWERS DIFFERENTLY AFTER THIS FILE, and why each difference is the
 * reference's own answer rather than a new one:
 *
 *   1. THE LFB (0xE0000000, 8 MiB).  memory.c's arms have been here since M1a but were dead
 *      (`vga_memory_size` was 0, so in_svga_lfb was false everywhere and the hook table's
 *      carve-out answered).  With the device installed they are live: a read of the LFB now
 *      comes out of the zeroed VRAM (the same 0 it answered before) and a WRITE STORES --
 *      which the reference does too (memory.rs:232-320's four LFB write arms).  A
 *      write-then-read at 0xE0000000 therefore newly returns what was written.  MEASURED
 *      before the change: it returned 0 (the write was discarded); the reference stored it.
 *   2. THE 0xA0000 WINDOW is registered by the device instead of falling through to
 *      memory.c's default table, so U1 has one place to install the real handlers.  Its
 *      answers do NOT change in U0: read 0, discard the write (the default table's own
 *      answers for these blocks, io.c:242-253 D4).
 *   3. PCI gains 00:12.0 (0x1234:0x1111, class 0x000300, one 8 MiB prefetchable memory BAR
 *      at 0xE0000008, ROM 64 KiB at 0xFEB00000).  The reference's machine has always had
 *      it (vga.js:389 `cpu.devices.pci.register_device(this)`), so the two legs CONVERGE.
 *   4. NO I/O PORT IS CLAIMED.  Every VGA port (0x3B4-0x3DF, 0x1CE/0x1CF) stays unclaimed
 *      and answers the reference empty entry's 0xFF/0xFFFF/-1, exactly as before this unit
 *      -- the register file is U1-U4's, and answering a made-up value here would be the
 *      false-declaration class the tree has already paid for.
 */

#include "vga.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu/memory.h" /* vga_mem8 / vga_memory_size / VGA_LFB_ADDRESS (memory.h) */
#include "io.h"
#include "pci.h"

/* The device is a singleton, exactly as every other src/hw device is. */
static vga_device_t vga_storage;
vga_device_t *vga_device(void) { return &vga_storage; }

/* THE VRAM.  Static, not malloc'd: machine_init() re-runs it in place (the selftest's
 * machine_reset does that ~18 times per run, and a malloc per machine would be a leak the
 * reference does not have).  The +16 bytes of tail slack are this port's, not upstream's:
 * memory.c's read64s/read128 at the last byte of the window issue a 16-byte load
 * (`memcpy(&v, vga_mem8 + (addr - VGA_LFB_ADDRESS), sizeof(v))`), which lands inside wasm
 * linear memory upstream and would run past a fixed C array here -- the same argument, and
 * the same 16 bytes, as mem8's allocation in memory.c:44-50. */
static uint8_t vga_vram[VGA_DEFAULT_VRAM_SIZE + 16];

/* ------------------------------------------------------------------ *
 * the 0xA0000 window -- vga.js:384-387's io.mmap_register
 *
 * U0 ANSWERS WHAT THIS MACHINE ALREADY ANSWERED: 0 on every read, discard on every write.
 * The reference's handlers (vga_memory_read / vga_memory_write) store into a 256 KiB plane
 * block and then decode the access by mode (text: vga_memory_write_text_mode; planar: the
 * write-mode/setreset/rotate arithmetic; SVGA: the bank offset), and the read side runs the
 * read mode / latches / chain arithmetic.  NONE of that is U0's -- the unit map is U1
 * (text), U3 (planar), U4 (SVGA banks) -- so the handlers here are the honest stand-in: the
 * SAME answers this machine gave before the device existed, when these blocks fell through
 * to memory.c's default table.  The one guest-visible consequence is recorded in vga.h: a
 * write-then-read at the window answers 0 here where the reference answers the byte.
 * ------------------------------------------------------------------ */
static bool vga_trace_enabled(void)
{
    /* A DIAGNOSTIC, guest-invisible by construction: nothing below changes a return value or
     * a device state.  `V86C_TRACE_VGA=1` prints every window access, in the established
     * style of `V86C_TRACE_ACPI` (memory.c) and `V86C_LOG_PORTS` (io.c).  Gating the
     * window's accesses cannot be done with V86C_LOG_PORTS: that one names UNCLAIMED PORTS,
     * and the window is a memory range. */
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("V86C_TRACE_VGA");
        enabled = v != NULL && v[0] != '\0' && v[0] != '0';
    }
    return enabled != 0;
}

static int32_t vga_window_read8(uint32_t addr)
{
    if (vga_trace_enabled()) {
        fprintf(stderr, "trace: vga window read8  %05x -> 0 (U0 stand-in)\n", (unsigned)addr);
    }
    return 0;
}

static void vga_window_write8(uint32_t addr, int32_t value)
{
    if (vga_trace_enabled()) {
        fprintf(stderr, "trace: vga window write8 %05x <- %02x (U0 stand-in, discarded)\n",
                (unsigned)addr, (unsigned)(value & 0xFF));
    }
    (void)addr;
    (void)value;
}

/* ------------------------------------------------------------------ *
 * the PCI function 00:12.0 -- vga.js:235-256
 *
 * pci_space is upstream's array with upstream's own truncation: the four declaration bytes
 * for BAR0 are `VGA_LFB_ADDRESS >>> 8/16/24`, i.e. 0xE00000 / 0xE000 / 0xE0, and pci.js:499
 * copies them through `new Uint8Array(device.pci_space)`, which keeps the low byte of each.
 * MEASURED against the pinned reference (libv86 0.5.471+g94faf3d), so these 64 bytes are
 * the reference's, not a reading of its source.
 * ------------------------------------------------------------------ */
static const uint8_t vga_pci_space[64] = {
    0x34, 0x12, 0x11, 0x11, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbf, 0xfe, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x1a, 0x00, 0x11,
    0x00, 0x00, 0xbe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* vga.js:250-256: one bar, `size: this.vga_memory_size`, and `pci_rom_size = 0x10000`,
 * `pci_rom_address = 0xFEB00000`.  The BAR's declaration dword is 0xE0000008 -- bit 0 clear,
 * so it is a MEMORY bar, which pci_register_device's `type == 0` arm leaves alone (upstream's
 * "memory, not needed currently", pci.js:528-531).  That is why an 8 MiB bar cannot exhaust
 * PCI_BAR_ENTRY_POOL: the pool is only touched by I/O bars. */
static pci_bar_t vga_pci_bars[1] = {
    { .size = VGA_DEFAULT_VRAM_SIZE, .original_bar = 0, .entries = NULL },
};

static pci_device_t vga_pci_device = {
    .pci_id = 0x12 << 3, /* 00:12.0 */
    .pci_space = vga_pci_space,
    .pci_space_length = (int32_t)sizeof(vga_pci_space),
    .pci_bars = vga_pci_bars,
    .pci_bars_count = 1,
    .name = "vga",
    .pci_rom_size = 0x10000,
    .pci_rom_address = 0xFEB00000,
};

/* ------------------------------------------------------------------ *
 * init -- cpu.js:1193's slot
 * ------------------------------------------------------------------ */
void vga_init(vga_screen_t *screen, vga_screen_kind_t kind)
{
    vga_device_t *vga = &vga_storage;

    /* A FRESH MACHINE's VRAM is zeroed: cpu.js create_memory's wasm memory starts zeroed and
     * the vga.js:368 view is over it.  (A REBOOT is a different path and does not come
     * through here -- machine_reboot names no VGA reset, upstream's reboot_internal names
     * none either, so VRAM survives a guest reboot exactly as the reference's does.) */
    memset(vga_vram, 0, sizeof(vga_vram));

    vga->screen = screen;
    vga->screen_kind = kind;
    vga->vga_memory_size = VGA_DEFAULT_VRAM_SIZE;
    /* vga.js:89-99's constructor values; U1 recomputes them from the CRTC. */
    vga->max_cols = 80;
    vga->max_rows = 25;
    /* vga.js:171-180 + 289-305's constructor values; U1/U2/U4 make them move. */
    vga->graphical_mode = false;
    vga->svga_enabled = false;
    vga->svga_bpp = 32;
    vga->screen_width = 0;
    vga->screen_height = 0;
    vga->virtual_width = 0;
    vga->virtual_height = 0;

    /* memory.c's LFB arms read these two; until this unit they were NULL/0 and every LFB
     * access took the hook table's carve-out instead.  The address constant is memory.h's
     * (memory.rs:50-56). */
    vga_mem8 = vga_vram;
    vga_memory_size = vga->vga_memory_size;

    /* vga.js:384-387.  NULL read32/write32 means io_mmap_register installs its four-8-bit
     * shims (io.js:259-277), which is also what the reference's two-callback registration
     * gives. */
    io_mmap_register(VGA_WINDOW_ADDRESS, VGA_WINDOW_SIZE, vga_window_read8, vga_window_write8,
                     NULL, NULL);

    /* vga.js:389.  After pci_init() -- machine_init's order (cpu.js:1178 before :1193). */
    pci_register_device(&vga_pci_device);

    vga->installed = true;
}

/* ------------------------------------------------------------------ *
 * the document -- main.c's --vga-doc, at the run's stop
 * ------------------------------------------------------------------ */
bool vga_write_document(const char *path, const char *image_sha256, uint64_t image_bytes,
                        uint64_t instruction_limit, uint64_t instruction_count)
{
    vga_device_t *vga = &vga_storage;

    if (!vga->installed) {
        fprintf(stderr, "v86c: vga: no device installed; no document written\n");
        return false;
    }

    vga_doc_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.image_sha256 = image_sha256;
    meta.image_bytes = image_bytes;
    meta.instruction_limit = instruction_limit;
    meta.instruction_count = instruction_count;
    meta.graphical_mode = vga->graphical_mode;
    meta.svga_enabled = vga->svga_enabled;
    meta.svga_bpp = vga->svga_bpp;
    meta.screen_width = vga->screen_width;
    meta.screen_height = vga->screen_height;
    meta.virtual_width = vga->virtual_width;
    meta.virtual_height = vga->virtual_height;
    meta.cols = vga->max_cols;
    meta.rows = vga->max_rows;

    return vga_screen_document_write(vga->screen, path, &meta);
}
