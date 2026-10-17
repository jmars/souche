/*
 * vga.c -- the VGA device: U0's seats plus U1's TEXT-MODE SUBSET (HANDOFF=v86c-vga).
 *
 * U0 landed the 8 MiB LFB, the 0xA0000 window (as a read-0/discard stand-in), PCI 00:12.0,
 * and the adapter installation.  U1 lands the reference's text mode on top:
 *
 *   - the CRTC (0x3D4/0x3D5 and the 0x3B4/0x3B5 monochrome aliases): start address,
 *     offset, line compare, cursor, and update_vga_size -> set_size_text;
 *   - the attribute controller (0x3C0/0x3C1): the index/data flip-flop, the internal
 *     palette (dac_map[0..15]), attribute_mode (bit 0 flips graphical_mode),
 *     color_plane_enable, horizontal_panning, color_select;
 *   - the DAC (0x3C6-0x3C9): dac_mask, dac_state, the read/write indices, and the 6-bit
 *     expand + byte-lane rotation into vga256_palette;
 *   - vga_memory_write_text_mode / text_mode_redraw / put_char (the flags + palette math),
 *     update_cursor / update_cursor_scanline, set_size_text;
 *   - the 0xA0000-0xBFFFF window's real handlers (vga_memory_read / vga_memory_write);
 *   - the misc output register (0x3C2/0x3CC) and the input-status flip-flop reset
 *     (0x3DA/0x3BA).
 *
 * Every arithmetic detail is the REFERENCE'S OWN, reproduced rather than "corrected" -- the
 * two places a silent "fix" would cost a byte-exact document are the DAC 6-bit expand
 * (vga.js:1680-1684: `b = v & 1; v = v<<2 | b<<1 | b`, NOT the textbook `v<<2|v>>4`) and
 * the CRTC read swap (0x0C returns start_address's LOW byte, 0x0D its HIGH byte, while the
 * writes are unswapped -- vga.js:1902-1907).  Both are pinned by the hw_selftest and the
 * differential gate.
 *
 * NOT HERE (named so nobody reads a silent gap as a prefix): mode 13h and the graphical
 * buffer (U2), the planar reads/writes/latches (U3), dispi/SVGA/bank switching (U4).  In U1
 * the sequencer (0x3C4/0x3C5) and graphics controller (0x3CE/0x3CF) ports are UNCLAIMED:
 * their register values are the constructor defaults (plane_write_bm 0xF, plane_read 0,
 * miscellaneous_graphics_register 0), so the window decode always selects memory space 0
 * (0xA0000) and the text write path always runs.  graphical_mode is only reachable through
 * the attribute controller's bit 0, and U1's gates never set it (the document refuses it,
 * as does the reference leg).
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

/* THE ISA WINDOW'S STORAGE (vga.js:377): four 64 KiB planes in one 256 KiB block, a
 * SEPARATE buffer from the LFB.  The reference stores text mode LINEARLY in this block
 * (vga_memory_write_text_mode writes `vga_memory[addr]`, text_mode_redraw reads it back
 * the same way) and reaches the font through plane 2; the plane VIEWS (64 KiB each) matter
 * only to the planar paths U3 owns and to the read latches below. */
static uint8_t vga_planes[VGA_PLANE_MEMORY_SIZE];

/* vga.js:38-55: the four host memory spaces, selected by miscellaneous_graphics_register
 * bits 3-2.  U1 decodes the default (index 0, 0xA0000); the register that changes the
 * select is U2's graphics controller, so in U1 the select is always 0. */
static const uint32_t vga_host_memory_space_start[VGA_HOST_MEMORY_SPACE_COUNT] = {
    0xA0000, 0xA0000, 0xB0000, 0xB8000,
};
static const uint32_t vga_host_memory_space_size[VGA_HOST_MEMORY_SPACE_COUNT] = {
    0x20000, 0x10000, 0x8000, 0x8000,
};

/* A DIAGNOSTIC, guest-invisible by construction: nothing below changes a return value or a
 * device state.  `V86C_TRACE_VGA=1` prints every window access, in the established style of
 * `V86C_TRACE_ACPI` (memory.c) and `V86C_LOG_PORTS` (io.c).  Gating the window's accesses
 * cannot be done with V86C_LOG_PORTS: that one names UNCLAIMED PORTS, and the window is a
 * memory range. */
static bool vga_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("V86C_TRACE_VGA");
        enabled = v != NULL && v[0] != '\0' && v[0] != '0';
    }
    return enabled != 0;
}

/* ------------------------------------------------------------------ *
 * the device methods the register decode drives (vga.js:841-1375's text half)
 * ------------------------------------------------------------------ */

/* vga.js:1066-1103: a scan line (line_compare, or the vertical total) converted to a
 * screen row, honouring double-scanning and the odd/even row-scan-counter substitution. */
static int32_t vga_scan_line_to_screen_row(vga_device_t *vga, uint32_t scan_line)
{
    if (vga->max_scan_line & 0x80) scan_line >>= 1;
    uint32_t repeat = 1u + (uint32_t)(vga->max_scan_line & 0x1F);
    scan_line = (scan_line + repeat - 1) / repeat; /* Math.ceil */
    if (!(vga->crtc_mode & 0x1)) scan_line <<= 1;
    if (!(vga->crtc_mode & 0x2)) scan_line <<= 1;
    return (int32_t)scan_line;
}

/* vga.js:1368-1375.  `visible` is `!disabled && start < end` (the reference passes it as
 * the third argument; the seam names the VALUE, not the adapters' parameter name). */
static void vga_update_cursor_scanline(vga_device_t *vga)
{
    int32_t disabled = vga->cursor_scanline_start & 0x20;
    int32_t max = vga->max_scan_line & 0x1F;
    int32_t start = vga->cursor_scanline_start & 0x1F;
    int32_t end = vga->cursor_scanline_end & 0x1F;
    if (start > max) start = max;
    if (end > max) end = max;
    bool visible = !disabled && start < end;
    vga->screen->update_cursor_scanline(vga->screen, start, end, visible);
}

/* vga.js:937-957.  The cursor may be reported OUT OF BOUNDS (the reference notes "is
 * allowed to be out of bounds") -- the adapter just records the row/col. */
static void vga_update_cursor(vga_device_t *vga)
{
    int32_t max_cols = vga->max_cols;
    if (vga->offset_register * 2 > max_cols) max_cols = vga->offset_register * 2;
    int32_t row, col;
    if (vga->cursor_address >= vga->start_address) {
        uint32_t memory_start = vga->cursor_address - vga->start_address;
        row = (int32_t)(memory_start / (uint32_t)max_cols);
        col = (int32_t)(memory_start % (uint32_t)max_cols);
    }
    else {
        row = (int32_t)(vga->cursor_address / (uint32_t)max_cols) +
              vga_scan_line_to_screen_row(vga, (uint32_t)vga->line_compare);
        col = (int32_t)(vga->cursor_address % (uint32_t)max_cols);
    }
    vga->screen->update_cursor(vga->screen, row, col);
}

/* vga.js:1144-1151.  max_cols/max_rows are the DEVICE's fields (the document header reads
 * them); the adapter's set_size_text is what sizes the recording grid.  U0 had no caller;
 * U1's caller is update_vga_size below. */
static void vga_set_size_text(vga_device_t *vga, int32_t cols, int32_t rows)
{
    vga->max_cols = cols;
    vga->max_rows = rows;
    vga->screen->set_size_text(vga->screen, cols, rows);
}

/* vga.js:2582-2598.  The glyph SHAPES are the canvas adapter's; the document adapter's
 * set_font_bitmap is a no-op (vga_screen.c), but the call must still be MADE -- it is the
 * reference's, and a later unit's adapter may read it. */
static void vga_set_font_bitmap(vga_device_t *vga, bool font_plane_dirty)
{
    int32_t height = vga->max_scan_line & 0x1F;
    if (height && !vga->graphical_mode) {
        bool width_9px = !(vga->clocking_mode & 0x8) && !(vga->clocking_mode & 0x1);
        bool width_dbl = (vga->clocking_mode & 0x8) != 0;
        bool copy_8th_col = (vga->attribute_mode & 0x4) != 0;
        vga->screen->set_font_bitmap(vga->screen, height + 1, width_9px, width_dbl,
                                     copy_8th_col, vga_planes + 2 * VGA_BANK_SIZE,
                                     font_plane_dirty);
    }
}

/* vga.js:1279-1357, TEXT BRANCH ONLY.  The graphical branch (U2's mode-13h size) is
 * unreachable in U1: graphical_mode only flips through the attribute controller's bit 0,
 * and no U1 gate sets it. */
static void vga_update_vga_size(vga_device_t *vga)
{
    if (vga->svga_enabled) return;

    int32_t horizontal_characters = 1 + vga->horizontal_display_enable_end;
    if (vga->horizontal_blank_start < horizontal_characters) {
        horizontal_characters = vga->horizontal_blank_start;
    }
    int32_t vertical_scans = 1 + vga->vertical_display_enable_end;
    if (vga->vertical_blank_start < vertical_scans) {
        vertical_scans = vga->vertical_blank_start;
    }

    /* Don't update while either dimension is still unconfigured (a register written in
     * isolation leaves the other at its 0 constructor value). */
    if (!horizontal_characters || !vertical_scans) return;

    if (vga->graphical_mode) {
        /* U2: the mode-13h size path. */
        return;
    }

    if (vga->max_scan_line & 0x80) vertical_scans >>= 1;
    int32_t height = vertical_scans / (1 + (vga->max_scan_line & 0x1F));
    if (horizontal_characters && height) {
        vga_set_size_text(vga, horizontal_characters, height);
    }
}

/* vga.js:841-880: the full grid, collapsed from final device state into put_char calls.
 * This is what the reference harness replays at the document stop (oracle/run-vga.mjs:198)
 * AND what update_layers runs on every CRTC/attribute write in text mode. */
static void vga_text_mode_redraw(vga_device_t *vga);

/* vga.js:848-880's per-cell decode, shared by text_mode_redraw and
 * vga_memory_write_text_mode.  The two FLAG constants are read OFF THE ADAPTER (they exist
 * on the document/canvas adapter only -- screen.js:30-34), which is load-bearing for the
 * flags byte. */
static void vga_put_char(vga_device_t *vga, int32_t row, int32_t col, uint8_t chr,
                         uint8_t color)
{
    bool blink_enabled = (vga->attribute_mode & 8) != 0;
    bool blinking = blink_enabled && (color & 0x80);
    bool font_page_b = vga->font_page_ab_enabled && !(color & 0x08);
    int32_t flags = (blinking ? vga_screen_flag_blinking(vga->screen) : 0) |
                    (font_page_b ? vga_screen_flag_font_page_b(vga->screen) : 0);
    int32_t fg_color_mask = vga->font_page_ab_enabled ? 7 : 0xF;
    int32_t bg_color_mask = blink_enabled ? 7 : 0xF;
    uint32_t bg = vga->vga256_palette[vga->dac_mask &
                                      vga->dac_map[(color >> 4) & bg_color_mask]];
    uint32_t fg = vga->vga256_palette[vga->dac_mask &
                                      vga->dac_map[color & fg_color_mask]];
    vga->screen->put_char(vga->screen, row, col, chr, flags, bg, fg);
}

/* A bounded read of the 256 KiB linear block.  text_mode_redraw's address is derived from
 * guest-writable registers (start_address, offset, max_cols/max_rows), so a pathological
 * CRTC setup can run it past the block -- the reference's typed-array read answers 0 there
 * and so does this (the R4 hard bound: never a guest-reachable overrun). */
static uint8_t vga_plane_byte(int32_t addr)
{
    if ((uint32_t)addr >= VGA_PLANE_MEMORY_SIZE) return 0;
    return vga_planes[addr];
}

static void vga_text_mode_redraw(vga_device_t *vga)
{
    int32_t split_screen_row = vga_scan_line_to_screen_row(vga, (uint32_t)vga->line_compare);
    int32_t row_offset = (vga->offset_register * 2 - vga->max_cols) * 2;
    if (row_offset < 0) row_offset = 0;

    int32_t addr = (int32_t)(vga->start_address << 1);
    for (int32_t row = 0; row < vga->max_rows; row++) {
        if (row == split_screen_row) addr = 0;
        for (int32_t col = 0; col < vga->max_cols; col++) {
            uint8_t chr = vga_plane_byte(addr);
            uint8_t color = vga_plane_byte(addr | 1);
            vga_put_char(vga, row, col, chr, color);
            addr += 2;
        }
        addr += row_offset;
    }
}

/* vga.js:885-932.  The window write's text-mode arm: store the byte, then recompute the
 * cell and send put_char -- the same decode text_mode_redraw uses, on the one touched cell. */
static void vga_memory_write_text_mode(vga_device_t *vga, int32_t addr, int32_t value)
{
    vga_planes[addr] = (uint8_t)value;

    int32_t max_cols = vga->max_cols;
    if (vga->offset_register * 2 > max_cols) max_cols = vga->offset_register * 2;
    int32_t row, col;
    if ((addr >> 1) >= (int32_t)vga->start_address) {
        int32_t memory_start = (addr >> 1) - (int32_t)vga->start_address;
        row = memory_start / max_cols;
        col = memory_start % max_cols;
    }
    else {
        int32_t memory_start = addr >> 1;
        row = memory_start / max_cols +
              vga_scan_line_to_screen_row(vga, (uint32_t)vga->line_compare);
        col = memory_start % max_cols;
    }

    if (col >= vga->max_cols || row >= vga->max_rows) return;

    uint8_t chr, color;
    if (addr & 1) {
        color = (uint8_t)value;
        chr = vga_planes[addr & ~1];
    }
    else {
        chr = (uint8_t)value;
        color = vga_planes[addr | 1];
    }
    vga_put_char(vga, row, col, chr, color);
}

/* vga.js:960-977.  In text mode the complete redraw IS the text redraw; the graphical arm
 * (svga_mark_dirty / the pixel-buffer diff) is U2's and unreachable in U1. */
static void vga_complete_redraw(vga_device_t *vga)
{
    if (vga->graphical_mode) {
        return; /* U2: svga_mark_dirty / diff_addr_min..max */
    }
    vga_text_mode_redraw(vga);
}

/* vga.js:984-997.  A no-op in text mode (its guard returns before touching the pixel
 * buffer), which is the only mode U1 can reach. */
static void vga_complete_replot(vga_device_t *vga)
{
    if (!vga->graphical_mode || vga->svga_enabled) return;
    /* U2: diff_plot_min..max = 0..VGA_PIXEL_BUFFER_SIZE + complete_redraw */
}

/* vga.js:1301-1378, TEXT HALF.  In text mode update_layers is exactly text_mode_redraw; the
 * two early returns (svga_enabled; no virtual/screen width) are both true-by-construction
 * in U1, so the graphical layer composition is U2's. */
static void vga_update_layers(vga_device_t *vga)
{
    if (!vga->graphical_mode) vga_text_mode_redraw(vga);
    if (vga->svga_enabled) return;
    if (!vga->virtual_width || !vga->screen_width) return;
    /* U2: palette-source/screen-disable clear + the layer list */
}

/* ------------------------------------------------------------------ *
 * the window handlers -- vga.js:565-662 (vga_memory_read / vga_memory_write)
 *
 * The svga_enabled (banked mode) arms are U4's: svga_enabled is always false in U1 (no
 * dispi), so they are unreachable and not implemented.
 * ------------------------------------------------------------------ */
static int32_t vga_memory_read(uint32_t addr)
{
    vga_device_t *vga = &vga_storage;
    if (vga_trace_enabled()) {
        fprintf(stderr, "trace: vga window read8  %05x\n", (unsigned)addr);
    }

    int32_t space = vga->miscellaneous_graphics_register >> 2 & 0x3;
    int32_t a = (int32_t)addr - (int32_t)vga_host_memory_space_start[space];
    if (a < 0 || a >= (int32_t)vga_host_memory_space_size[space]) return 0;

    /* the read latches (vga.js:588-592).  Each plane VIEW is 64 KiB, so an address past it
     * reads as the JS typed array's out-of-range 0 -- reproduced here. */
    vga->latch_dword = 0;
    if (a < (int32_t)VGA_BANK_SIZE) {
        vga->latch_dword = (uint32_t)vga_planes[a] |
                           (uint32_t)vga_planes[1 * VGA_BANK_SIZE + a] << 8 |
                           (uint32_t)vga_planes[2 * VGA_BANK_SIZE + a] << 16 |
                           (uint32_t)vga_planes[3 * VGA_BANK_SIZE + a] << 24;
    }

    /* read mode 0 (read mode 1, planar_mode & 8, is U3's).  Text mode stores linearly and
     * reads plane 0 (plane_read & 3); the chain-4/odd-even arms are U2/U3's. */
    int32_t plane = vga->plane_read;
    if (!vga->graphical_mode) plane &= 0x3;
    return vga_planes[(uint32_t)(plane << 16) | (uint32_t)a];
}

static void vga_memory_write(uint32_t addr, int32_t value)
{
    vga_device_t *vga = &vga_storage;
    if (vga_trace_enabled()) {
        fprintf(stderr, "trace: vga window write8 %05x <- %02x\n", (unsigned)addr,
                (unsigned)(value & 0xFF));
    }

    int32_t space = vga->miscellaneous_graphics_register >> 2 & 0x3;
    int32_t a = (int32_t)addr - (int32_t)vga_host_memory_space_start[space];
    if (a < 0 || a >= (int32_t)vga_host_memory_space_size[space]) return;

    if (vga->graphical_mode) {
        /* U2: vga_memory_write_graphical */
        return;
    }
    if (!(vga->plane_write_bm & 0x3)) {
        if (vga->plane_write_bm & 0x4) {
            /* U3: write to plane 2 (the font bitmap).  plane2 is a 64 KiB VIEW, so an
             * address past it is a no-op in the reference (typed-array out of range). */
            if (a < (int32_t)VGA_BANK_SIZE) vga_planes[2 * VGA_BANK_SIZE + a] = (uint8_t)value;
        }
        return;
    }
    vga_memory_write_text_mode(vga, a, value);
}

/* ------------------------------------------------------------------ *
 * the I/O port handlers (vga.js:1406-2060's text half)
 *
 * Each write handler is `(void *device, int32_t port, int32_t value)` and each read is
 * `(void *device, int32_t port)` -- the port-map shape (io.h, D3).  `port` is unused in all
 * of them (the handler already knows its port), which is why it is cast away.
 * ------------------------------------------------------------------ */
#define VGA_DEV(d) ((vga_device_t *)(d))

/* forward declarations: the read16/write16 shims call their neighbours' 8-bit handlers
 * (vga.js's read16/write16 helpers do the same). */
static int32_t vga_port3C1_read(void *device, int32_t port);
static void vga_port3C9_write(void *device, int32_t port, int32_t value);
static void vga_port3D5_write(void *device, int32_t port, int32_t value);

static int32_t vga_port3C0_read(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    return (vga->attribute_controller_index | vga->palette_source) & 0xFF;
}

static int32_t vga_port3C0_read16(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    return vga_port3C0_read(vga, 0) | (vga_port3C1_read(vga, 0) << 8 & 0xFF00);
}

static int32_t vga_port3C1_read(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    if (vga->attribute_controller_index < 0x10) {
        /* the JS `dac_map[-1] & 0xFF` is `undefined & 0xFF` == 0 */
        if (vga->attribute_controller_index < 0) return 0;
        return vga->dac_map[vga->attribute_controller_index] & 0xFF;
    }
    switch (vga->attribute_controller_index) {
    case 0x10: return vga->attribute_mode;
    case 0x12: return vga->color_plane_enable;
    case 0x13: return vga->horizontal_panning;
    case 0x14: return vga->color_select;
    default: break;
    }
    return 0xFF;
}

static void vga_port3C0_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);

    if (vga->attribute_controller_index == -1) {
        vga->attribute_controller_index = value & 0x1F;
        if (vga->palette_source != (value & 0x20)) {
            vga->palette_source = value & 0x20;
            vga_update_layers(vga);
        }
    }
    else if (vga->attribute_controller_index < 0x10) {
        vga->dac_map[vga->attribute_controller_index] = (uint8_t)value;
        if (!(vga->attribute_mode & 0x40)) {
            vga_complete_redraw(vga);
        }
        vga->attribute_controller_index = -1;
    }
    else {
        switch (vga->attribute_controller_index) {
        case 0x10:
            if (vga->attribute_mode != value) {
                int32_t previous_mode = vga->attribute_mode;
                vga->attribute_mode = value;
                /* the svga deferred-disable is U4 (svga_enabled is always false in U1) */
                bool is_graphical = (value & 0x1) != 0;
                if (!vga->svga_enabled && vga->graphical_mode != is_graphical) {
                    vga->graphical_mode = is_graphical;
                    vga->screen->set_mode(vga->screen, vga->graphical_mode);
                }
                if ((previous_mode ^ value) & 0x40) {
                    vga_complete_replot(vga);
                }
                vga_update_vga_size(vga);
                vga_complete_redraw(vga);
                vga_set_font_bitmap(vga, false);
            }
            break;
        case 0x12:
            if (vga->color_plane_enable != value) {
                vga->color_plane_enable = value;
                vga_complete_redraw(vga);
            }
            break;
        case 0x13:
            if (vga->horizontal_panning != value) {
                vga->horizontal_panning = value & 0xF;
                vga_update_layers(vga);
            }
            break;
        case 0x14:
            if (vga->color_select != value) {
                vga->color_select = value;
                vga_complete_redraw(vga);
            }
            break;
        default:
            break;
        }
        vga->attribute_controller_index = -1;
    }
}

static void vga_port3C2_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    vga->miscellaneous_output_register = value;
}

static int32_t vga_port3C6_read(void *device, int32_t port)
{
    (void)port;
    return VGA_DEV(device)->dac_mask;
}

static void vga_port3C6_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    if (vga->dac_mask != value) {
        vga->dac_mask = value;
        vga_complete_redraw(vga);
    }
}

static int32_t vga_port3C7_read(void *device, int32_t port)
{
    (void)port;
    return VGA_DEV(device)->dac_state;
}

static void vga_port3C7_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    vga->dac_color_index_read = value * 3;
    vga->dac_state &= 0x0;
}

static int32_t vga_port3C8_read(void *device, int32_t port)
{
    (void)port;
    return VGA_DEV(device)->dac_color_index_write / 3 & 0xFF;
}

static void vga_port3C8_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    vga->dac_color_index_write = (value & 0xFF) * 3;
    vga->dac_state |= 0x3;
}

static void vga_port3C8_write16(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    vga_port3C8_write(vga, 0, value & 0xFF);
    vga_port3C9_write(vga, 0, value >> 8 & 0xFF);
}

/* vga.js:1671-1696.  THE 6-BIT EXPAND IS THE REFERENCE'S OWN, not the textbook
 * `v<<2|v>>4`: `b = v & 1; v = v<<2 | b<<1 | b` (vga.js:1680-1684).  A "corrected" expand
 * here would diverge the document's palette by up to 3 per component. */
static void vga_port3C9_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    int32_t index = vga->dac_color_index_write / 3 & 0xFF;
    int32_t offset = vga->dac_color_index_write % 3;
    uint32_t color = vga->vga256_palette[index];
    int32_t color_byte = value;

    if ((vga->dispi_enable_value & 0x20) == 0) {
        color_byte &= 0x3F;
        int32_t b = color_byte & 1;
        color_byte = color_byte << 2 | b << 1 | b;
    }

    if (offset == 0) {
        color = (color & ~0xFF0000u) | ((uint32_t)color_byte << 16);
    }
    else if (offset == 1) {
        color = (color & ~0xFF00u) | ((uint32_t)color_byte << 8);
    }
    else {
        color = (color & ~0xFFu) | (uint32_t)color_byte;
    }

    if (vga->vga256_palette[index] != color) {
        vga->vga256_palette[index] = color;
        vga_complete_redraw(vga);
    }
    vga->dac_color_index_write = (vga->dac_color_index_write + 1) % 768;
}

static int32_t vga_port3C9_read(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    int32_t index = vga->dac_color_index_read / 3 & 0xFF;
    int32_t offset = vga->dac_color_index_read % 3;
    uint32_t color = vga->vga256_palette[index];
    int32_t color8 = (int32_t)(color >> (2 - offset) * 8 & 0xFF);
    vga->dac_color_index_read = (vga->dac_color_index_read + 1) % 768;
    if (vga->dispi_enable_value & 0x20) return color8;
    return color8 >> 2;
}

static int32_t vga_port3CC_read(void *device, int32_t port)
{
    (void)port;
    return VGA_DEV(device)->miscellaneous_output_register;
}

static int32_t vga_port3CA_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return 0;
}

static int32_t vga_port3D4_read(void *device, int32_t port)
{
    (void)port;
    return VGA_DEV(device)->index_crtc;
}

static void vga_port3D4_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    VGA_DEV(device)->index_crtc = value;
}

static void vga_port3D4_write16(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    vga_port3D4_write(vga, 0, value & 0xFF);
    vga_port3D5_write(vga, 0, value >> 8 & 0xFF);
}

static void vga_port3D5_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    switch (vga->index_crtc) {
    case 0x1:
        if (vga->horizontal_display_enable_end != value) {
            vga->horizontal_display_enable_end = value;
            vga_update_vga_size(vga);
        }
        break;
    case 0x2:
        if (vga->horizontal_blank_start != value) {
            vga->horizontal_blank_start = value;
            vga_update_vga_size(vga);
        }
        break;
    case 0x7: {
        int32_t previous = vga->vertical_display_enable_end;
        vga->vertical_display_enable_end &= 0xFF;
        vga->vertical_display_enable_end |= (value << 3 & 0x200) | (value << 7 & 0x100);
        if (previous != vga->vertical_display_enable_end) vga_update_vga_size(vga);
        vga->line_compare = (vga->line_compare & 0x2FF) | (value << 4 & 0x100);
        int32_t previous_vblank = vga->vertical_blank_start;
        vga->vertical_blank_start = (vga->vertical_blank_start & 0x2FF) | (value << 5 & 0x100);
        if (previous_vblank != vga->vertical_blank_start) vga_update_vga_size(vga);
        vga_update_layers(vga);
        break;
    }
    case 0x8:
        vga->preset_row_scan = value;
        vga_update_layers(vga);
        break;
    case 0x9: {
        int32_t previous_max = vga->max_scan_line;
        vga->max_scan_line = value;
        vga->line_compare = (vga->line_compare & 0x1FF) | (value << 3 & 0x200);
        int32_t previous_vblank = vga->vertical_blank_start;
        vga->vertical_blank_start = (vga->vertical_blank_start & 0x1FF) | (value << 4 & 0x200);
        if (((previous_max ^ vga->max_scan_line) & 0x9F) || previous_vblank != vga->vertical_blank_start) {
            vga_update_vga_size(vga);
        }
        vga_update_cursor_scanline(vga);
        vga_update_layers(vga);
        vga_set_font_bitmap(vga, false);
        break;
    }
    case 0xA:
        vga->cursor_scanline_start = value;
        vga_update_cursor_scanline(vga);
        break;
    case 0xB:
        vga->cursor_scanline_end = value;
        vga_update_cursor_scanline(vga);
        break;
    case 0xC:
        if ((vga->start_address >> 8 & 0xFF) != (uint32_t)value) {
            vga->start_address = (vga->start_address & 0xFF) | (uint32_t)(value << 8);
            vga_update_layers(vga);
            if (~vga->crtc_mode & 0x3) vga_complete_replot(vga);
        }
        break;
    case 0xD:
        if ((vga->start_address & 0xFF) != (uint32_t)value) {
            vga->start_address = (vga->start_address & 0xFF00) | (uint32_t)value;
            vga_update_layers(vga);
            if (~vga->crtc_mode & 0x3) vga_complete_replot(vga);
        }
        break;
    case 0xE:
        vga->cursor_address = (vga->cursor_address & 0xFF) | (uint32_t)(value << 8);
        vga_update_cursor(vga);
        break;
    case 0xF:
        vga->cursor_address = (vga->cursor_address & 0xFF00) | (uint32_t)value;
        vga_update_cursor(vga);
        break;
    case 0x12:
        if ((vga->vertical_display_enable_end & 0xFF) != value) {
            vga->vertical_display_enable_end = (vga->vertical_display_enable_end & 0x300) | value;
            vga_update_vga_size(vga);
        }
        break;
    case 0x13:
        if (vga->offset_register != value) {
            vga->offset_register = value;
            vga_update_vga_size(vga);
            if (~vga->crtc_mode & 0x3) vga_complete_replot(vga);
        }
        break;
    case 0x14:
        if (vga->underline_location_register != value) {
            int32_t previous = vga->underline_location_register;
            vga->underline_location_register = value;
            vga_update_vga_size(vga);
            if ((previous ^ value) & 0x40) vga_complete_replot(vga);
        }
        break;
    case 0x15:
        if ((vga->vertical_blank_start & 0xFF) != value) {
            vga->vertical_blank_start = (vga->vertical_blank_start & 0x300) | value;
            vga_update_vga_size(vga);
        }
        break;
    case 0x17:
        if (vga->crtc_mode != value) {
            int32_t previous = vga->crtc_mode;
            vga->crtc_mode = value;
            vga_update_vga_size(vga);
            if ((previous ^ value) & 0x43) vga_complete_replot(vga);
        }
        break;
    case 0x18:
        vga->line_compare = (vga->line_compare & 0x300) | value;
        vga_update_layers(vga);
        break;
    default:
        if (vga->index_crtc < (int32_t)sizeof(vga->crtc)) {
            vga->crtc[vga->index_crtc] = (uint8_t)value;
        }
        break;
    }
}

static void vga_port3D5_write16(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    vga_port3D5_write(vga, 0, value & 0xFF);
}

static int32_t vga_port3D5_read(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    switch (vga->index_crtc) {
    case 0x1: return vga->horizontal_display_enable_end;
    case 0x2: return vga->horizontal_blank_start;
    case 0x7:
        return (vga->vertical_display_enable_end >> 7 & 0x2) |
               (vga->vertical_blank_start >> 5 & 0x8) |
               (vga->line_compare >> 4 & 0x10) |
               (vga->vertical_display_enable_end >> 3 & 0x40);
    case 0x8: return vga->preset_row_scan;
    case 0x9: return vga->max_scan_line;
    case 0xA: return vga->cursor_scanline_start;
    case 0xB: return vga->cursor_scanline_end;
    /* vga.js:1902-1907: the reads are SWAPPED relative to the writes (0x0C returns the LOW
     * byte, 0x0D the HIGH byte) -- the reference's own quirk, reproduced, not fixed. */
    case 0xC: return (int32_t)(vga->start_address & 0xFF);
    case 0xD: return (int32_t)(vga->start_address >> 8);
    case 0xE: return (int32_t)(vga->cursor_address >> 8);
    case 0xF: return (int32_t)(vga->cursor_address & 0xFF);
    case 0x12: return vga->vertical_display_enable_end & 0xFF;
    case 0x13: return vga->offset_register;
    case 0x14: return vga->underline_location_register;
    case 0x15: return vga->vertical_blank_start & 0xFF;
    case 0x17: return vga->crtc_mode;
    case 0x18: return vga->line_compare & 0xFF;
    default: break;
    }
    if (vga->index_crtc < (int32_t)sizeof(vga->crtc)) return vga->crtc[vga->index_crtc];
    return 0;
}

static int32_t vga_port3D5_read16(void *device, int32_t port)
{
    (void)port;
    return vga_port3D5_read(device, 0);
}

static int32_t vga_port3DA_read(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    int32_t value = vga->port_3DA_value;
    if (!vga->graphical_mode) {
        if (vga->port_3DA_value & 1) vga->port_3DA_value ^= 8;
        vga->port_3DA_value ^= 1;
    }
    else {
        vga->port_3DA_value ^= 1;
        vga->port_3DA_value &= 1;
    }
    vga->attribute_controller_index = -1;
    return value;
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

    /* A FRESH MACHINE's VRAM and plane block are zeroed: cpu.js create_memory's wasm memory
     * starts zeroed and the vga.js:368 view is over it, while the 256 KiB plane block
     * (vga.js:377) is a fresh typed array.  (A REBOOT does not come through here, so both
     * survive a guest reboot exactly as the reference's do.) */
    memset(vga_vram, 0, sizeof(vga_vram));
    memset(vga_planes, 0, sizeof(vga_planes));

    vga->screen = screen;
    vga->screen_kind = kind;
    vga->vga_memory_size = VGA_DEFAULT_VRAM_SIZE;
    vga->max_cols = 80;
    vga->max_rows = 25;
    vga->graphical_mode = false;
    vga->svga_enabled = false;
    vga->svga_bpp = 32;
    vga->screen_width = 0;
    vga->screen_height = 0;
    vga->virtual_width = 0;
    vga->virtual_height = 0;

    /* the register file -- vga.js:78-304's constructor values */
    vga->index_crtc = 0;
    memset(vga->crtc, 0, sizeof(vga->crtc));
    vga->crtc_mode = 0;
    vga->horizontal_display_enable_end = 0;
    vga->horizontal_blank_start = 0;
    vga->vertical_display_enable_end = 0;
    vga->vertical_blank_start = 0;
    vga->underline_location_register = 0;
    vga->preset_row_scan = 0;
    vga->offset_register = 0;
    vga->line_compare = 0;
    vga->max_scan_line = 0;
    vga->start_address = 0;
    vga->start_address_latched = 0;
    vga->cursor_address = 0;
    vga->cursor_scanline_start = 0xE;
    vga->cursor_scanline_end = 0xF;

    vga->attribute_controller_index = -1;
    vga->palette_source = 0x20;
    vga->attribute_mode = 0;
    vga->color_plane_enable = 0;
    vga->horizontal_panning = 0;
    vga->color_select = 0;
    memset(vga->dac_map, 0, sizeof(vga->dac_map));

    vga->dac_mask = 0xFF;
    vga->dac_color_index_write = 0;
    vga->dac_color_index_read = 0;
    vga->dac_state = 0;
    memset(vga->vga256_palette, 0, sizeof(vga->vga256_palette));

    vga->plane_write_bm = 0xF;
    vga->plane_read = 0;
    vga->planar_mode = 0;
    vga->character_map_select = 0;
    vga->clocking_mode = 0;
    vga->sequencer_memory_mode = 0;
    vga->miscellaneous_graphics_register = 0;
    vga->font_page_ab_enabled = false;

    vga->miscellaneous_output_register = 0xFF;
    vga->port_3DA_value = 0xFF;
    vga->dispi_enable_value = 0;
    vga->latch_dword = 0;

    /* memory.c's LFB arms read these two; until this unit they were NULL/0 and every LFB
     * access took the hook table's carve-out instead.  The address constant is memory.h's
     * (memory.rs:50-56). */
    vga_mem8 = vga_vram;
    vga_memory_size = vga->vga_memory_size;

    /* THE PORTS (vga.js:328-365) -- the text-mode subset U1 owns.  The sequencer
     * (0x3C4/0x3C5) and graphics controller (0x3CE/0x3CF) stay UNCLAIMED (U2/U3): their
     * register values are the constructor defaults and never move in U1. */
    io_register_read(0x3C0, vga, vga_port3C0_read, vga_port3C0_read16, NULL);
    io_register_write(0x3C0, vga, vga_port3C0_write, NULL, NULL);
    io_register_read(0x3C1, vga, vga_port3C1_read, NULL, NULL);
    io_register_write(0x3C2, vga, vga_port3C2_write, NULL, NULL);

    io_register_read(0x3C6, vga, vga_port3C6_read, NULL, NULL);
    io_register_write(0x3C6, vga, vga_port3C6_write, NULL, NULL);
    io_register_read(0x3C7, vga, vga_port3C7_read, NULL, NULL);
    io_register_write(0x3C7, vga, vga_port3C7_write, NULL, NULL);
    io_register_read(0x3C8, vga, vga_port3C8_read, NULL, NULL);
    io_register_write(0x3C8, vga, vga_port3C8_write, vga_port3C8_write16, NULL);
    io_register_read(0x3C9, vga, vga_port3C9_read, NULL, NULL);
    io_register_write(0x3C9, vga, vga_port3C9_write, NULL, NULL);

    io_register_read(0x3CC, vga, vga_port3CC_read, NULL, NULL);
    io_register_read(0x3CA, vga, vga_port3CA_read, NULL, NULL);

    io_register_read(0x3D4, vga, vga_port3D4_read, NULL, NULL);
    io_register_write(0x3D4, vga, vga_port3D4_write, vga_port3D4_write16, NULL);
    io_register_read(0x3D5, vga, vga_port3D5_read, vga_port3D5_read16, NULL);
    io_register_write(0x3D5, vga, vga_port3D5_write, vga_port3D5_write16, NULL);
    io_register_read(0x3B4, vga, vga_port3D4_read, NULL, NULL);
    io_register_write(0x3B4, vga, vga_port3D4_write, vga_port3D4_write16, NULL);
    io_register_read(0x3B5, vga, vga_port3D5_read, vga_port3D5_read16, NULL);
    io_register_write(0x3B5, vga, vga_port3D5_write, vga_port3D5_write16, NULL);

    io_register_read(0x3DA, vga, vga_port3DA_read, NULL, NULL);
    io_register_read(0x3BA, vga, vga_port3DA_read, NULL, NULL);

    /* vga.js:384-387.  NULL read32/write32 means io_mmap_register installs its four-8-bit
     * shims (io.js:259-277), which is also what the reference's two-callback registration
     * gives. */
    io_mmap_register(VGA_WINDOW_ADDRESS, VGA_WINDOW_SIZE, vga_memory_read, vga_memory_write,
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

    /* THE REPLAY (oracle/run-vga.mjs:198-200): the reference leg replays the device's OWN
     * text state through its recorder at the stop -- text_mode_redraw, update_cursor,
     * update_cursor_scanline, in that order -- so the document is a function of final device
     * state rather than of the call stream.  The C leg replays the same three calls through
     * the installed adapter; the device does all the palette/attribute math.  (A graphical
     * mode is refused by the writer below, exactly as the reference leg refuses it by name
     * BEFORE its replay.)
     *
     * The set_size_text ahead of them is the reference's own set_state text branch
     * (vga.js:557) and a C-only bookkeeping step: the reference recorder is a row,col-keyed
     * Map with no grid, so its set_size_text is a no-op, but the C document adapter must
     * know its grid size before put_char will record into it (a put_char outside the
     * recorded grid is dropped).  It changes no document byte -- the cells it would have
     * dropped are the zero cells the writer emits for an unsized grid anyway. */
    if (!vga->graphical_mode) {
        vga_set_size_text(vga, vga->max_cols, vga->max_rows);
        vga_text_mode_redraw(vga);
        vga_update_cursor(vga);
        vga_update_cursor_scanline(vga);
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
