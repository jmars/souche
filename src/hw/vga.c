/*
 * vga.c -- the VGA device: U0's seats, U1's text mode, U2's MODE 13h (HANDOFF=v86c-vga).
 *
 * U0 landed the 8 MiB LFB, the 0xA0000 window, PCI 00:12.0, and the adapter installation.
 * U1 landed the reference's text mode: the CRTC, the attribute controller, the DAC, the text
 * window handlers, and the redraw/put_char arithmetic.  U2 lands the reference's mode 13h
 * and the graphical document:
 *
 *   - the sequencer (0x3C4/0x3C5): clocking_mode, plane_write_bm, character_map_select,
 *     sequencer_memory_mode (whose bit 3 -- Chain 4 -- is what mode 13h turns on);
 *   - the graphics controller (0x3CE/0x3CF): the planar write pipeline (set/reset, rotate,
 *     bitmap, write modes 0-3), plane_read, planar_mode (the 0x40/0x60 shift modes), the
 *     miscellaneous graphics register (memory space select), color compare/don't care;
 *   - the graphical_mode flip (attribute_mode bit 0) feeding update_vga_size's graphical
 *     branch -> set_size_graphical -> the dest buffer;
 *   - vga_memory_write_graphical / the read modes 0 and 1 window arms;
 *   - vga_replot + vga_redraw (with the reference's DEST-BUFFER CLAMP QUIRK: a pixel past
 *     virtual_width*virtual_height is a typed-array no-op, reproduced as a bounded skip) and
 *     vga_screen_fill_buffer, which the document writer calls ONCE at the stop.
 *
 * Every arithmetic detail is the REFERENCE'S OWN, reproduced rather than "corrected" -- the
 * three places a silent "fix" would cost a byte-exact document are the DAC 6-bit expand
 * (vga.js:1680-1684: `b = v & 1; v = v<<2 | b<<1 | b`, NOT the textbook `v<<2|v>>4`), the
 * CRTC read swap (0x0C returns start_address's LOW byte, 0x0D its HIGH byte --
 * vga.js:1902-1907), and the address-to-pixel shift count (vga.js:1032-1048, load-bearing
 * for chain-4).  All three are pinned by the hw_selftest and the differential gate.
 *
 * NOT HERE (named so nobody reads a silent gap as a prefix): the planar 16-colour modes'
 * LATCH paths and shift modes 0x00/0x20 are U3's (ported whole here -- the replot/redraw/
 * write switches are ONE function each in the reference -- but U3's gate proves them), and
 * dispi/SVGA/bank switching/the real dirty bitmap are U4's (svga_enabled is always false).
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

/* THE PIXEL BUFFER (vga.js:382) -- vga_replot's output, 8 pixels per plane byte, indexed by
 * pixel address.  Then THE DEST BUFFER (vga.js:1176-1180 image_data.data over the dest wasm
 * buffer): virtual_width*virtual_height RGBA bytes, what vga_redraw writes and what the
 * graphical document serializes.  Both static, like every other device buffer. */
static uint8_t vga_pixel_buffer[VGA_PIXEL_BUFFER_SIZE];
static uint8_t vga_dest_bytes[VGA_DEST_BUFFER_MAX_PIXELS * 4];
static int32_t vga_dest_pixels; /* virtual_width * virtual_height, 0 = unsized */

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

/* forward declarations: the graphical methods call each other and the text methods call
 * complete_redraw/replot from below. */
static void vga_complete_redraw(vga_device_t *vga);
static void vga_complete_replot(vga_device_t *vga);
static void vga_update_layers(vga_device_t *vga);
static void vga_update_vertical_retrace(vga_device_t *vga);
static void vga_set_font_page(vga_device_t *vga);
static void vga_set_size_graphical(vga_device_t *vga, int32_t width, int32_t height,
                                   int32_t virtual_width, int32_t virtual_height, int32_t bpp);
static void vga_replot(vga_device_t *vga);
static void vga_redraw(vga_device_t *vga);
static int32_t vga_addr_to_pixel(vga_device_t *vga, int32_t addr);
static int32_t vga_bytes_per_line(vga_device_t *vga);
static void vga_partial_redraw(vga_device_t *vga, int32_t min, int32_t max);
static void vga_partial_replot(vga_device_t *vga, int32_t min, int32_t max);

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

/* vga.js:2601-2612.  Computes font_page_ab_enabled (the put_char flags reader) from the
 * character map select and replays the page pair through the adapter (a no-op on every
 * adapter this lane ships) + complete_redraw. */
static void vga_set_font_page(vga_device_t *vga)
{
    static const int32_t linear_index_map[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
    int32_t vga_index_a = ((vga->character_map_select & 0x0C) >> 2) |
                          ((vga->character_map_select & 0x20) >> 3);
    int32_t vga_index_b = (vga->character_map_select & 0x03) |
                          ((vga->character_map_select & 0x10) >> 2);
    vga->font_page_ab_enabled = vga_index_a != vga_index_b;
    vga->screen->set_font_page(vga->screen, linear_index_map[vga_index_a],
                               linear_index_map[vga_index_b]);
    vga_complete_redraw(vga);
}

/* vga.js:1194-1271.  Both branches live now: text -> set_size_text, graphical -> the
 * mode-13h size (set_size_graphical + update_vertical_retrace + update_layers). */
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
        int32_t screen_width = horizontal_characters << 3;
        /* vga.js:1219-1221: offset is half the bytes/words/dwords per line, but the number
         * of pixels latched is 8 per character clock (4 for 8-bit PEL width). */
        int32_t virtual_width = vga->offset_register << 4;
        int32_t bpp = 4;

        if (vga->attribute_mode & 0x40) {
            screen_width >>= 1;
            virtual_width >>= 1;
            bpp = 8;
        }
        else if (vga->attribute_mode & 0x2) {
            bpp = 1;
        }

        int32_t screen_height = vga_scan_line_to_screen_row(vga, (uint32_t)vertical_scans);

        /* vga.js:1239-1248: the virtual height is however many rows fit in the host memory
         * space (index 0's 128 KiB, ALWAYS -- the reference reads [0] even when the memory
         * space select is a different one). */
        int32_t available_bytes = (int32_t)vga_host_memory_space_size[0];
        int32_t bytes_per_line = vga_bytes_per_line(vga);
        int32_t virtual_height = bytes_per_line
                                     ? (available_bytes + bytes_per_line - 1) / bytes_per_line
                                     : screen_height;

        vga_set_size_graphical(vga, screen_width, screen_height, virtual_width, virtual_height,
                               bpp);
        vga_update_vertical_retrace(vga);
        vga_update_layers(vga);
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

/* vga.js:1024-1048: the bytes per display line, from the offset register and the
 * underline/crtc clocking bits.  update_vga_size's virtual_height divides the host memory
 * space by this. */
static int32_t vga_bytes_per_line(vga_device_t *vga)
{
    int32_t bytes_per_line = vga->offset_register << 2;
    if (vga->underline_location_register & 0x40) bytes_per_line <<= 1;
    else if (vga->crtc_mode & 0x40) bytes_per_line >>= 1;
    return bytes_per_line;
}

/* vga.js:1032-1048: the address-to-pixel shift, counted in multiples of 0x40 then reduced
 * by >>>6.  Load-bearing for mode 13h (underline 0x40 + crtc_mode 0xA3 + attribute_mode
 * 0x41 -> 0, so a pixel's plane byte is at the SAME address -- which is what makes the
 * chain-4 write's `addr &= ~3` line up with the replot's read). */
static int32_t vga_addr_shift_count(vga_device_t *vga)
{
    int32_t shift_count = 0x80;
    shift_count += ~vga->underline_location_register & vga->crtc_mode & 0x40;
    shift_count -= vga->underline_location_register & 0x40;
    shift_count -= vga->attribute_mode & 0x40;
    return (int32_t)((uint32_t)shift_count >> 6);
}

/* vga.js:1050-1100: a plane address -> pixel address, undoing the CRTC's bit 13/14 row
 * substitution.  Used by the window write's partial_replot (through vga_addr_to_pixel) and
 * by update_layers' panning; mode 13h (crtc_mode & 3 == 3) takes the plain `addr << shift`
 * arm. */
static int32_t vga_addr_to_pixel(vga_device_t *vga, int32_t addr)
{
    int32_t shift_count = vga_addr_shift_count(vga);

    if (~vga->crtc_mode & 0x3) {
        int32_t pixel_addr = addr - (int32_t)vga->start_address;
        pixel_addr &= (vga->crtc_mode << 13) | ~0x6000;
        pixel_addr <<= shift_count;

        int32_t row = 0, col = 0;
        if (vga->virtual_width > 0) { /* vga.js: row = Infinity|0 = 0, col = NaN -- the NaN col makes the reassembled address substitution_bits | start_address; the caller (hunk A) never uses this value when virtual_width is 0. */
            row = pixel_addr / vga->virtual_width;
            col = pixel_addr % vga->virtual_width;
        }

        switch (vga->crtc_mode & 0x3) {
        case 0x2:
            row = (row << 1) | (addr >> 13 & 0x1);
            break;
        case 0x1:
            row = (row << 1) | (addr >> 14 & 0x1);
            break;
        case 0x0:
            row = (row << 2) | (addr >> 13 & 0x3);
            break;
        default: break;
        }

        return row * vga->virtual_width + col + ((int32_t)vga->start_address << shift_count);
    }

    return addr << shift_count;
}

/* vga.js:1154-1192.  The dest buffer (image_data) is (re)allocated only when the
 * dimensions change; the adapter's set_size_graphical records them either way.  The bpp
 * argument is the reference's (a bus message argument here); only the dimensions drive the
 * buffer. */
static void vga_set_size_graphical(vga_device_t *vga, int32_t width, int32_t height,
                                   int32_t virtual_width, int32_t virtual_height, int32_t bpp)
{
    if (virtual_width < 1) virtual_width = 1;
    if (virtual_height < 1) virtual_height = 1;

    bool needs_update = vga->screen_width != width || vga->screen_height != height ||
                        vga->virtual_width != virtual_width ||
                        vga->virtual_height != virtual_height;

    if (needs_update) {
        vga->screen_width = width;
        vga->screen_height = height;
        vga->virtual_width = virtual_width;
        vga->virtual_height = virtual_height;

        int32_t pixels = virtual_width * virtual_height;
        if (pixels > (int32_t)VGA_DEST_BUFFER_MAX_PIXELS) {
            /* the R4 hard bound: report and refuse, never a guest-reachable overrun. */
            fprintf(stderr, "v86c: vga: refusing a %dx%d dest buffer (bounded at %u pixels)\n",
                    (int)virtual_width, (int)virtual_height, VGA_DEST_BUFFER_MAX_PIXELS);
            vga->virtual_width = 0;
            vga->virtual_height = 0;
            vga_dest_pixels = 0;
            return;
        }
        vga_dest_pixels = pixels;
        memset(vga_dest_bytes, 0, sizeof(vga_dest_bytes)); /* vga.js allocates a FRESH (zeroed) wasm region on every size change, so pixels the redraw does not rewrite read as 0; the static must be re-zeroed to match. */
    }

    vga->screen->set_size_graphical(vga->screen, width, height, virtual_width, virtual_height);
    (void)bpp;
}

/* vga.js:1357-1366.  The latched start address updates here (the reference only latches on
 * a vertical retrace, which is the stop's fill and update_vga_size's graphical arm). */
static void vga_update_vertical_retrace(vga_device_t *vga)
{
    vga->port_3DA_value |= 0x8;
    if (vga->start_address_latched != vga->start_address) {
        vga->start_address_latched = vga->start_address;
        vga_update_layers(vga);
    }
}

/* vga.js:997-1017.  The diff ranges: register/window writes EXPAND them (or, on a complete
 * redraw/replot, reset them to the whole pixel buffer); the pixels are computed only when
 * vga_screen_fill_buffer runs at the stop. */
static void vga_partial_redraw(vga_device_t *vga, int32_t min, int32_t max)
{
    if (min < vga->diff_addr_min) vga->diff_addr_min = min;
    if (max > vga->diff_addr_max) vga->diff_addr_max = max;
}

static void vga_partial_replot(vga_device_t *vga, int32_t min, int32_t max)
{
    if (min < vga->diff_plot_min) vga->diff_plot_min = min;
    if (max > vga->diff_plot_max) vga->diff_plot_max = max;
    vga_partial_redraw(vga, min, max);
}

static void vga_reset_diffs(vga_device_t *vga)
{
    vga->diff_addr_min = (int32_t)vga->vga_memory_size;
    vga->diff_addr_max = 0;
    vga->diff_plot_min = (int32_t)vga->vga_memory_size;
    vga->diff_plot_max = 0;
}

/* vga.js:960-980. */
static void vga_complete_redraw(vga_device_t *vga)
{
    if (vga->graphical_mode) {
        /* U4 owns the svga_enabled arm (svga_mark_dirty). */
        vga->diff_addr_min = 0;
        vga->diff_addr_max = VGA_PIXEL_BUFFER_SIZE;
    }
    else {
        vga_text_mode_redraw(vga);
    }
}

/* vga.js:982-995. */
static void vga_complete_replot(vga_device_t *vga)
{
    if (!vga->graphical_mode || vga->svga_enabled) return;

    vga->diff_plot_min = 0;
    vga->diff_plot_max = VGA_PIXEL_BUFFER_SIZE;
    vga_complete_redraw(vga);
}

/* vga.js:1273-1355.  In text mode this is text_mode_redraw; in graphical mode it computes
 * the layer list (panning + split screen), which only the canvas adapter draws -- the
 * document adapter's update_buffer is a no-op, so the document depends on the dest buffer,
 * not on the layers.  The two early returns (svga_enabled; no virtual/screen width) and the
 * palette-source/screen-disable arm are reproduced for the device's own state. */
static void vga_update_layers(vga_device_t *vga)
{
    if (!vga->graphical_mode) vga_text_mode_redraw(vga);

    if (vga->svga_enabled) return;
    if (!vga->virtual_width || !vga->screen_width) return;

    if (!vga->palette_source || (vga->clocking_mode & 0x20)) {
        vga->screen->clear_screen(vga->screen);
        return;
    }

    int32_t pixel_panning = vga->horizontal_panning;
    if (vga->attribute_mode & 0x40) pixel_panning >>= 1;
    int32_t byte_panning = vga->preset_row_scan >> 5 & 0x3;
    int32_t pixel_addr_start =
        vga_addr_to_pixel(vga, (int32_t)vga->start_address_latched + byte_panning);
    int32_t start_buffer_row = pixel_addr_start / vga->virtual_width;
    int32_t start_buffer_col = pixel_addr_start % vga->virtual_width + pixel_panning;

    int32_t split_screen_row =
        vga_scan_line_to_screen_row(vga, (uint32_t)(1 + vga->line_compare));
    if (split_screen_row > vga->screen_height) split_screen_row = vga->screen_height;
    int32_t split_buffer_height = vga->screen_height - split_screen_row;

    /* The reference builds `this.layers` and calls screen.update_buffer(this.layers); every
     * adapter this lane ships treats update_buffer as a no-op, so the list itself is not
     * materialized -- the device's observable state (screen clear, retrace latch) is above. */
    (void)start_buffer_col;
    (void)start_buffer_row;
    (void)split_screen_row;
    (void)split_buffer_height;
}

/* ------------------------------------------------------------------ *
 * the planar write pipeline -- vga.js:743-839 (apply_*) and 675-741
 * (vga_memory_write_graphical)
 *
 * Ported WHOLE (all four write modes and both chains) rather than the mode-13h subset, so
 * the reference's single switch is not split into a partial function with a silent-wrong
 * divergence surface (the plan's R1).  Mode 13h exercises write mode 0 + chain 4; U3's gate
 * will exercise the rest.  A plane write whose address runs past the 64 KiB plane VIEW is
 * the reference's typed-array out-of-range no-op -- reproduced as a bounded drop.
 * ------------------------------------------------------------------ */
static int32_t vga_plane_view_byte(int32_t plane, int32_t a)
{
    if ((uint32_t)a >= VGA_BANK_SIZE) return 0;
    return vga_planes[(uint32_t)(plane << 16) | (uint32_t)a];
}

static uint32_t vga_apply_feed(int32_t data_byte)
{
    uint32_t dword = (uint32_t)data_byte;
    dword |= dword << 8;
    dword |= dword << 16;
    dword |= dword << 24;
    return dword;
}

static uint32_t vga_apply_expand(int32_t data_byte)
{
    uint32_t dword = (data_byte & 0x1) ? 0xFFu : 0u;
    dword |= (uint32_t)((data_byte & 0x2) ? 0xFFu : 0u) << 8;
    dword |= (uint32_t)((data_byte & 0x4) ? 0xFFu : 0u) << 16;
    dword |= (uint32_t)((data_byte & 0x8) ? 0xFFu : 0u) << 24;
    return dword;
}

static int32_t vga_apply_rotate(vga_device_t *vga, int32_t data_byte)
{
    uint32_t wrapped = (uint32_t)data_byte | ((uint32_t)data_byte << 8);
    uint32_t count = (uint32_t)vga->planar_rotate_reg & 0x7;
    return (int32_t)((wrapped >> count) & 0xFF);
}

static uint32_t vga_apply_setreset(vga_device_t *vga, uint32_t data_dword,
                                   uint32_t enable_dword)
{
    uint32_t setreset_dword = vga_apply_expand(vga->planar_setreset);
    data_dword |= enable_dword & setreset_dword;
    data_dword &= ~enable_dword | setreset_dword;
    return data_dword;
}

static uint32_t vga_apply_logical(vga_device_t *vga, uint32_t data_dword,
                                  uint32_t latch_dword)
{
    switch (vga->planar_rotate_reg & 0x18) {
    case 0x08: return data_dword & latch_dword;
    case 0x10: return data_dword | latch_dword;
    case 0x18: return data_dword ^ latch_dword;
    default: break;
    }
    return data_dword;
}

static uint32_t vga_apply_bitmask(uint32_t data_dword, uint32_t bitmask_dword,
                                  uint32_t latch_dword)
{
    uint32_t plane_dword = bitmask_dword & data_dword;
    plane_dword |= ~bitmask_dword & latch_dword;
    return plane_dword;
}

static void vga_memory_write_graphical(vga_device_t *vga, int32_t addr, int32_t value)
{
    int32_t write_mode = vga->planar_mode & 3;
    uint32_t bitmask = vga_apply_feed(vga->planar_bitmap);
    uint32_t setreset_dword = vga_apply_expand(vga->planar_setreset);
    uint32_t setreset_enable_dword = vga_apply_expand(vga->planar_setreset_enable);
    uint32_t latch = (uint32_t)vga->latch_dword;
    uint32_t plane_dword;

    switch (write_mode) {
    case 0:
        value = vga_apply_rotate(vga, value);
        plane_dword = vga_apply_feed(value);
        plane_dword = vga_apply_setreset(vga, plane_dword, setreset_enable_dword);
        plane_dword = vga_apply_logical(vga, plane_dword, latch);
        plane_dword = vga_apply_bitmask(plane_dword, bitmask, latch);
        break;
    case 1:
        plane_dword = latch;
        break;
    case 2:
        plane_dword = vga_apply_expand(value);
        plane_dword = vga_apply_logical(vga, plane_dword, latch);
        plane_dword = vga_apply_bitmask(plane_dword, bitmask, latch);
        break;
    case 3:
        value = vga_apply_rotate(vga, value);
        bitmask &= vga_apply_feed(value);
        plane_dword = setreset_dword;
        plane_dword = vga_apply_bitmask(plane_dword, bitmask, latch);
        break;
    default:
        plane_dword = 0;
        break;
    }

    int32_t plane_select = 0xF;
    switch (vga->sequencer_memory_mode & 0xC) {
    case 0x0: /* Odd/Even (chain 2) */
        plane_select = 0x5 << (addr & 0x1);
        addr &= ~0x1;
        break;
    case 0x8: /* Chain 4 */
    case 0xC:
        plane_select = 1 << (addr & 0x3);
        addr &= ~0x3;
        break;
    default: break;
    }

    plane_select &= vga->plane_write_bm;

    if ((uint32_t)addr < VGA_BANK_SIZE) {
        if (plane_select & 0x1) vga_planes[addr] = (uint8_t)(plane_dword & 0xFF);
        if (plane_select & 0x2) vga_planes[1 * VGA_BANK_SIZE + addr] = (uint8_t)(plane_dword >> 8 & 0xFF);
        if (plane_select & 0x4) vga_planes[2 * VGA_BANK_SIZE + addr] = (uint8_t)(plane_dword >> 16 & 0xFF);
        if (plane_select & 0x8) vga_planes[3 * VGA_BANK_SIZE + addr] = (uint8_t)(plane_dword >> 24 & 0xFF);
    }

    if (vga->virtual_width > 0) { /* vga.js: with virtual_width 0 vga_addr_to_pixel yields NaN, partial_replot's NaN comparisons leave the diff ranges untouched -- the same as not expanding. */
        int32_t pixel_addr = vga_addr_to_pixel(vga, addr);
        vga_partial_replot(vga, pixel_addr, pixel_addr + 7);
    }
}

/* ------------------------------------------------------------------ *
 * the window handlers -- vga.js:565-662 (vga_memory_read / vga_memory_write)
 *
 * The svga_enabled (banked mode) arms are U4's: svga_enabled is always false in U1/U2 (no
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

    /* the read latches (vga.js:583-586).  Each plane VIEW is 64 KiB, so an address past it
     * reads as the JS typed array's out-of-range 0 -- reproduced here. */
    vga->latch_dword = 0;
    if (a < (int32_t)VGA_BANK_SIZE) {
        vga->latch_dword = (uint32_t)vga_planes[a] |
                           (uint32_t)vga_planes[1 * VGA_BANK_SIZE + a] << 8 |
                           (uint32_t)vga_planes[2 * VGA_BANK_SIZE + a] << 16 |
                           (uint32_t)vga_planes[3 * VGA_BANK_SIZE + a] << 24;
    }

    if (vga->planar_mode & 0x08) {
        /* read mode 1 (vga.js:588-611): the colour compare / don't-care path. */
        int32_t reading = 0xFF;
        if (vga->color_dont_care & 0x1)
            reading &= vga_plane_view_byte(0, a) ^ ~((vga->color_compare & 0x1) ? 0xFF : 0x00);
        if (vga->color_dont_care & 0x2)
            reading &= vga_plane_view_byte(1, a) ^ ~((vga->color_compare & 0x2) ? 0xFF : 0x00);
        if (vga->color_dont_care & 0x4)
            reading &= vga_plane_view_byte(2, a) ^ ~((vga->color_compare & 0x4) ? 0xFF : 0x00);
        if (vga->color_dont_care & 0x8)
            reading &= vga_plane_view_byte(3, a) ^ ~((vga->color_compare & 0x8) ? 0xFF : 0x00);
        return reading;
    }

    /* read mode 0 (vga.js:612-635) */
    int32_t plane = vga->plane_read;
    if (!vga->graphical_mode) {
        plane &= 0x3;
    }
    else if (vga->sequencer_memory_mode & 0x8) {
        /* Chain 4 */
        plane = a & 0x3;
        a &= ~0x3;
    }
    else if (vga->planar_mode & 0x10) {
        /* Odd/Even host read */
        plane = a & 0x1;
        a &= ~0x1;
    }
    if (plane > 3) return 0; /* vga.js: vga_memory[plane << 16 | a] with plane >= 4 is past the 256 KiB typed array -> undefined -> the guest reads 0 (cpu.js:235-242). */
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
        vga_memory_write_graphical(vga, a, value);
        return;
    }
    if (!(vga->plane_write_bm & 0x3)) {
        if (vga->plane_write_bm & 0x4) {
            /* write to plane 2 (the font bitmap).  plane2 is a 64 KiB VIEW, so an address
             * past it is a no-op in the reference (typed-array out of range). */
            if (a < (int32_t)VGA_BANK_SIZE) vga_planes[2 * VGA_BANK_SIZE + a] = (uint8_t)value;
        }
        return;
    }
    vga_memory_write_text_mode(vga, a, value);
}

/* ------------------------------------------------------------------ *
 * the pixel pipeline -- vga.js:2337-2580 (vga_replot, vga_redraw,
 * screen_fill_buffer).  Reached ONLY from vga_screen_fill_buffer (the stop's replay); the
 * run's register/window writes expand the diff ranges instead (vga.js's browser frame loop
 * is the other caller, and this port has none).
 * ------------------------------------------------------------------ */

/* vga.js:2337-2455.  plane memory -> pixel buffer, honouring the shift mode (0x40/0x60 for
 * mode 13h; 0x00/0x20 are the planar shift modes U3's gate will exercise -- ported whole
 * because they are ONE switch in the reference).  A plane read past the 64 KiB VIEW reads as
 * the typed array's out-of-range 0, which makes pixel_buffer entries past 64 KiB zero. */
static void vga_replot(vga_device_t *vga)
{
    int32_t start = vga->diff_plot_min & ~0xF;
    if (start < 0) start = 0; /* vga.js: negative pixel_buffer writes are typed-array no-ops and the plane reads are undefined -> 0; starting at 0 is byte-exact. */
    int32_t end = vga->diff_plot_max | 0xF;
    if (end > (int32_t)VGA_PIXEL_BUFFER_SIZE - 1) end = (int32_t)VGA_PIXEL_BUFFER_SIZE - 1;

    int32_t addr_shift = vga_addr_shift_count(vga);
    int32_t addr_substitution = ~vga->crtc_mode & 0x3;
    int32_t shift_mode = vga->planar_mode & 0x60;
    bool pel_width = (vga->attribute_mode & 0x40) != 0;

    int32_t pixel_addr = start;
    while (pixel_addr <= end) {
        int32_t addr = pixel_addr >> addr_shift;
        if (addr_substitution) {
            int32_t row = 0, col = 0;
            if (vga->virtual_width > 0) { row = pixel_addr / vga->virtual_width; col = pixel_addr - vga->virtual_width * row; }

            switch (addr_substitution) {
            case 0x1:
                addr = (row & 0x1) << 13;
                row >>= 1;
                break;
            case 0x2:
                addr = (row & 0x1) << 14;
                row >>= 1;
                break;
            case 0x3:
                addr = (row & 0x3) << 13;
                row >>= 2;
                break;
            default: break;
            }

            addr |= ((row * vga->virtual_width + col) >> addr_shift) + (int32_t)vga->start_address;
        }

        int32_t byte0 = vga_plane_view_byte(0, addr);
        int32_t byte1 = vga_plane_view_byte(1, addr);
        int32_t byte2 = vga_plane_view_byte(2, addr);
        int32_t byte3 = vga_plane_view_byte(3, addr);

        uint8_t shift_loads[8] = { 0 };
        switch (shift_mode) {
        case 0x00: /* Planar Shift Mode */
            byte0 <<= 0;
            byte1 <<= 1;
            byte2 <<= 2;
            byte3 <<= 3;
            for (int i = 7; i >= 0; i--) {
                shift_loads[7 - i] = (uint8_t)((byte0 >> i & 1) | (byte1 >> i & 2) |
                                              (byte2 >> i & 4) | (byte3 >> i & 8));
            }
            break;
        case 0x20: /* Packed Shift Mode (modes 4h/5h) */
            shift_loads[0] = (uint8_t)((byte0 >> 6 & 0x3) | (byte2 >> 4 & 0xC));
            shift_loads[1] = (uint8_t)((byte0 >> 4 & 0x3) | (byte2 >> 2 & 0xC));
            shift_loads[2] = (uint8_t)((byte0 >> 2 & 0x3) | (byte2 >> 0 & 0xC));
            shift_loads[3] = (uint8_t)((byte0 >> 0 & 0x3) | (byte2 << 2 & 0xC));
            shift_loads[4] = (uint8_t)((byte1 >> 6 & 0x3) | (byte3 >> 4 & 0xC));
            shift_loads[5] = (uint8_t)((byte1 >> 4 & 0x3) | (byte3 >> 2 & 0xC));
            shift_loads[6] = (uint8_t)((byte1 >> 2 & 0x3) | (byte3 >> 0 & 0xC));
            shift_loads[7] = (uint8_t)((byte1 >> 0 & 0x3) | (byte3 << 2 & 0xC));
            break;
        case 0x40: /* 256-Color Shift Mode (modes 13h and unchained 256) */
        case 0x60:
            shift_loads[0] = (uint8_t)(byte0 >> 4 & 0xF);
            shift_loads[1] = (uint8_t)(byte0 >> 0 & 0xF);
            shift_loads[2] = (uint8_t)(byte1 >> 4 & 0xF);
            shift_loads[3] = (uint8_t)(byte1 >> 0 & 0xF);
            shift_loads[4] = (uint8_t)(byte2 >> 4 & 0xF);
            shift_loads[5] = (uint8_t)(byte2 >> 0 & 0xF);
            shift_loads[6] = (uint8_t)(byte3 >> 4 & 0xF);
            shift_loads[7] = (uint8_t)(byte3 >> 0 & 0xF);
            break;
        default: break;
        }

        if (pel_width) {
            for (int i = 0, j = 0; i < 4; i++, pixel_addr++, j += 2) {
                vga_pixel_buffer[pixel_addr] = (uint8_t)((shift_loads[j] << 4) | shift_loads[j + 1]);
            }
        }
        else {
            for (int i = 0; i < 8; i++, pixel_addr++) {
                vga_pixel_buffer[pixel_addr] = shift_loads[i];
            }
        }
    }
}

/* vga.js:2463-2507.  pixel buffer -> dest buffer, through the internal palette (4-bit) or
 * straight through vga256_palette (8-bit).  THE DEST-BUFFER CLAMP QUIRK: the reference's
 * `buffer` is an Int32Array of virtual_width*virtual_height, and the loop's end is clamped
 * to VGA_PIXEL_BUFFER_SIZE-1 -- so a pixel_addr past the dest buffer is a typed-array
 * OUT-OF-RANGE NO-OP in JS.  Reproduced exactly: the write is skipped past the length. */
static void vga_redraw(vga_device_t *vga)
{
    int32_t start = vga->diff_addr_min;
    if (start < 0) start = 0; /* vga.js: negative dest writes are typed-array no-ops (pixel_buffer reads undefined) -- skipping them is byte-exact. */
    int32_t end = vga->diff_addr_max;
    if (end > (int32_t)VGA_PIXEL_BUFFER_SIZE - 1) end = (int32_t)VGA_PIXEL_BUFFER_SIZE - 1;

    int32_t buffer_length = vga->virtual_width * vga->virtual_height;

    int32_t mask = 0xFF;
    int32_t colorset = 0x00;
    if (vga->attribute_mode & 0x80) {
        mask &= 0xCF;
        colorset |= vga->color_select << 4 & 0x30;
    }

    if (vga->attribute_mode & 0x40) {
        /* 8-bit mode */
        for (int32_t pixel_addr = start; pixel_addr <= end; pixel_addr++) {
            int32_t color256 = (vga_pixel_buffer[pixel_addr] & mask) | colorset;
            uint32_t color = vga->vga256_palette[color256];
            if (pixel_addr < buffer_length) {
                uint32_t px = (color & 0xFF00u) | (color << 16) | (color >> 16) | 0xFF000000u;
                vga_dest_bytes[4 * pixel_addr + 0] = (uint8_t)(px & 0xFF);
                vga_dest_bytes[4 * pixel_addr + 1] = (uint8_t)(px >> 8 & 0xFF);
                vga_dest_bytes[4 * pixel_addr + 2] = (uint8_t)(px >> 16 & 0xFF);
                vga_dest_bytes[4 * pixel_addr + 3] = (uint8_t)(px >> 24 & 0xFF);
            }
        }
    }
    else {
        /* 4-bit mode */
        mask &= 0x3F;
        colorset |= vga->color_select << 4 & 0xC0;
        for (int32_t pixel_addr = start; pixel_addr <= end; pixel_addr++) {
            int32_t color16 = vga_pixel_buffer[pixel_addr] & vga->color_plane_enable;
            int32_t color256 = (vga->dac_map[color16] & mask) | colorset;
            uint32_t color = vga->vga256_palette[color256];
            if (pixel_addr < buffer_length) {
                uint32_t px = (color & 0xFF00u) | (color << 16) | (color >> 16) | 0xFF000000u;
                vga_dest_bytes[4 * pixel_addr + 0] = (uint8_t)(px & 0xFF);
                vga_dest_bytes[4 * pixel_addr + 1] = (uint8_t)(px >> 8 & 0xFF);
                vga_dest_bytes[4 * pixel_addr + 2] = (uint8_t)(px >> 16 & 0xFF);
                vga_dest_bytes[4 * pixel_addr + 3] = (uint8_t)(px >> 24 & 0xFF);
            }
        }
    }
}

void vga_screen_fill_buffer(vga_device_t *vga)
{
    if (!vga->graphical_mode) {
        /* text mode: update the retrace latch and nothing else (vga.js:2511-2517). */
        vga_update_vertical_retrace(vga);
        return;
    }
    /* U4 owns the svga_enabled arm; the reference's wasm-resize check is a node no-op (no
     * ImageData -> the dest buffer never shrank), so the port skips it. */
    vga_replot(vga);
    vga_redraw(vga);
    vga_reset_diffs(vga);
    vga_update_vertical_retrace(vga);
}

const uint8_t *vga_dest_buffer(const vga_device_t *vga, int32_t *pixels)
{
    (void)vga;
    if (pixels != NULL) *pixels = vga_dest_pixels;
    if (vga_dest_pixels == 0) return NULL;
    return vga_dest_bytes;
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

/* ------------------------------------------------------------------ *
 * the sequencer -- 0x3C4 (index) / 0x3C5 (data), vga.js:1534-1612
 * ------------------------------------------------------------------ */
static int32_t vga_port3C4_read(void *device, int32_t port)
{
    (void)port;
    return VGA_DEV(device)->sequencer_index;
}

static void vga_port3C4_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    VGA_DEV(device)->sequencer_index = value;
}

static int32_t vga_port3C5_read(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    switch (vga->sequencer_index) {
    case 0x01: return vga->clocking_mode;
    case 0x02: return vga->plane_write_bm;
    case 0x03: return vga->character_map_select;
    case 0x04: return vga->sequencer_memory_mode;
    case 0x06: return 0x12; /* vga.js:1607-1608: the fixed sequencer id */
    default: break;
    }
    return 0;
}

static void vga_port3C5_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    switch (vga->sequencer_index) {
    case 0x01: {
        int32_t previous_clocking_mode = vga->clocking_mode;
        vga->clocking_mode = value;
        if ((previous_clocking_mode ^ value) & 0x20) vga_update_layers(vga);
        vga_set_font_bitmap(vga, false);
        break;
    }
    case 0x02: {
        int32_t previous_plane_write_bm = vga->plane_write_bm;
        vga->plane_write_bm = value;
        if (!vga->graphical_mode && (previous_plane_write_bm & 0x4) &&
            !(vga->plane_write_bm & 0x4)) {
            /* End of font plane 2 write access (vga.js:1569-1573). */
            vga_set_font_bitmap(vga, true);
        }
        break;
    }
    case 0x03: {
        int32_t previous_character_map_select = vga->character_map_select;
        vga->character_map_select = value;
        if (!vga->graphical_mode && previous_character_map_select != value) {
            vga_set_font_page(vga);
        }
        break;
    }
    case 0x04:
        vga->sequencer_memory_mode = value;
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ *
 * the graphics controller -- 0x3CE (index) / 0x3CF (data), vga.js:1727-1825
 * ------------------------------------------------------------------ */
static int32_t vga_port3CE_read(void *device, int32_t port)
{
    (void)port;
    return VGA_DEV(device)->graphics_index;
}

static void vga_port3CE_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    VGA_DEV(device)->graphics_index = value;
}

static int32_t vga_port3CF_read(void *device, int32_t port)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    switch (vga->graphics_index) {
    case 0x0: return vga->planar_setreset;
    case 0x1: return vga->planar_setreset_enable;
    case 0x2: return vga->color_compare;
    case 0x3: return vga->planar_rotate_reg;
    case 0x4: return vga->plane_read;
    case 0x5: return vga->planar_mode;
    case 0x6: return vga->miscellaneous_graphics_register;
    case 0x7: return vga->color_dont_care;
    case 0x8: return vga->planar_bitmap;
    default: break;
    }
    return 0;
}

static void vga_port3CF_write(void *device, int32_t port, int32_t value)
{
    (void)port;
    vga_device_t *vga = VGA_DEV(device);
    switch (vga->graphics_index) {
    case 0x0: vga->planar_setreset = value; break;
    case 0x1: vga->planar_setreset_enable = value; break;
    case 0x2: vga->color_compare = value; break;
    case 0x3: vga->planar_rotate_reg = value; break;
    case 0x4: vga->plane_read = value; break;
    case 0x5: {
        int32_t previous_planar_mode = vga->planar_mode;
        vga->planar_mode = value;
        if ((previous_planar_mode ^ value) & 0x60) {
            /* Shift mode modified.  Pixel buffer invalidated (vga.js:1771-1775). */
            vga_complete_replot(vga);
        }
        break;
    }
    case 0x6:
        if (vga->miscellaneous_graphics_register != value) {
            vga->miscellaneous_graphics_register = value;
            vga_update_vga_size(vga);
        }
        break;
    case 0x7: vga->color_dont_care = value; break;
    case 0x8: vga->planar_bitmap = value; break;
    default: break;
    }
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

    vga->sequencer_index = -1;
    vga->plane_write_bm = 0xF;
    vga->sequencer_memory_mode = 0;
    vga->clocking_mode = 0;
    vga->graphics_index = -1;
    vga->character_map_select = 0;
    vga->plane_read = 0;
    vga->planar_mode = 0;
    vga->planar_rotate_reg = 0;
    vga->planar_bitmap = 0xFF;
    vga->planar_setreset = 0;
    vga->planar_setreset_enable = 0;
    vga->miscellaneous_graphics_register = 0;
    vga->color_compare = 0;
    vga->color_dont_care = 0;
    vga->font_page_ab_enabled = false;

    vga->diff_addr_min = (int32_t)vga->vga_memory_size;
    vga->diff_addr_max = 0;
    vga->diff_plot_min = (int32_t)vga->vga_memory_size;
    vga->diff_plot_max = 0;

    vga->miscellaneous_output_register = 0xFF;
    vga->port_3DA_value = 0xFF;
    vga->dispi_enable_value = 0;
    vga->latch_dword = 0;

    /* The pixel and dest buffers are zeroed each fresh machine; the redraw writes every
     * in-bounds dest pixel, but zeroing keeps a partial replay deterministic (and matches
     * a fresh wasm dest allocation's zero-init on growth). */
    memset(vga_pixel_buffer, 0, sizeof(vga_pixel_buffer));
    memset(vga_dest_bytes, 0, sizeof(vga_dest_bytes));
    vga_dest_pixels = 0;

    /* memory.c's LFB arms read these two; until this unit they were NULL/0 and every LFB
     * access took the hook table's carve-out instead.  The address constant is memory.h's
     * (memory.rs:50-56). */
    vga_mem8 = vga_vram;
    vga_memory_size = vga->vga_memory_size;

    /* THE PORTS (vga.js:306-365).  The sequencer/graphics writes use the CONSECUTIVE form
     * so a 16-bit `out dx, ax` (the VGA programming idiom) lands as index=lo, data=hi --
     * exactly the reference's register_write_consecutive.  The reads are individual (a
     * 16-bit read of the index port answers the empty 0xFFFF in the reference, since only
     * read8 is registered). */
    io_register_read(0x3C0, vga, vga_port3C0_read, vga_port3C0_read16, NULL);
    io_register_write(0x3C0, vga, vga_port3C0_write, NULL, NULL);
    io_register_read(0x3C1, vga, vga_port3C1_read, NULL, NULL);
    io_register_write(0x3C2, vga, vga_port3C2_write, NULL, NULL);

    io_register_write_consecutive(0x3C4, vga, vga_port3C4_write, vga_port3C5_write, NULL, NULL);
    io_register_read(0x3C4, vga, vga_port3C4_read, NULL, NULL);
    io_register_read(0x3C5, vga, vga_port3C5_read, NULL, NULL);

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

    io_register_write_consecutive(0x3CE, vga, vga_port3CE_write, vga_port3CF_write, NULL, NULL);
    io_register_read(0x3CE, vga, vga_port3CE_read, NULL, NULL);
    io_register_read(0x3CF, vga, vga_port3CF_read, NULL, NULL);

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

    /* THE REPLAY.  Text: the reference leg replays the device's OWN text state through its
     * recorder at the stop -- set_size_text (the reference's set_state text branch,
     * vga.js:557, and this port's grid-seeding step), text_mode_redraw, update_cursor,
     * update_cursor_scanline, in that order (oracle/run-vga.mjs:198-200).  Graphical: the
     * reference leg calls vga.screen_fill_buffer() ONCE at the stop (the plan's Q4/d), which
     * is the ONE replot+redraw -- the run's register/window writes only expanded the diff
     * ranges.  The C leg replays the same call through the device. */
    if (!vga->graphical_mode) {
        vga_set_size_text(vga, vga->max_cols, vga->max_rows);
        vga_text_mode_redraw(vga);
        vga_update_cursor(vga);
        vga_update_cursor_scanline(vga);
    }
    else {
        vga_screen_fill_buffer(vga);
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
    meta.dest_buffer = vga_dest_buffer(vga, NULL);

    return vga_screen_document_write(vga->screen, path, &meta);
}
