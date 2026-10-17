/*
 * vga.h -- the VGA device and its RENDERING SEAM.
 *
 * Port of v86/src/vga.js's device/rendering split, unit VGA-U0 (HANDOFF=v86c-vga).  The
 * reference proves the split: the device (vga.js, 2,612 lines) talks to a `screen` object
 * through exactly ELEVEN methods (vga.js:70 `this.screen = screen`; every one of the 26
 * call sites is `this.screen.<method>`), and the RENDERING is then an adapter decision --
 * v86/src/browser/starter.js:307-319 picks ScreenAdapter (canvas) / ANSIScreenAdapter
 * (terminal text) / DummyScreenAdapter (headless) purely from `options.screen`.
 *
 * WHAT THIS UNIT LANDS, and what it deliberately does NOT:
 *
 *   - THE SEAM (vga_screen_t, below): the 11 methods upstream's device actually calls, plus
 *     an opaque state pointer.  destroy/pause/continue/set_scale/get_text_screen are NOT
 *     here: they are UI-side, and grep over vga.js finds no call to any of them.
 *   - THREE ADAPTERS (vga_screen.c): the headless port of browser/dummy_screen.js (the
 *     default, decision 1 of the lane), the ANSI text adapter (a port of
 *     browser/ansi_screen.js, decision 3), and the DOCUMENT adapter, which records the
 *     put_char stream into a grid and serializes it (decision 2: the verified path).
 *   - THE DEVICE'S U0 SEATS: the 8 MiB vga_mem8 (memory.c's already-ported LFB arms become
 *     live), the 0xA0000 window registered through io_mmap_register, and the PCI function
 *     00:12.0 (vga.js:235-241 pci_space, pci_id 0x12 << 3, one 8 MiB memory BAR).
 *
 * NOT HERE, AND NAMED SO NOBODY READS A SILENT GAP AS A PREFIX: the register file and every
 * decode that uses it are the later units' -- text mode's CRTC/attribute/DAC and the redraw
 * and put_char arithmetic (U1), mode 13h and the graphical buffer (U2), the planar
 * reads/writes/latches (U3), and dispi/SVGA/bank switching/the real dirty bitmap (U4).  In
 * U0 the device therefore answers:
 *   - the 0xA0000-0xBFFFF window: 0 on every read, discarding every write -- which is
 *     EXACTLY what this machine answered before the device existed (memory.c's default
 *     table, the R1F carve-out) and what a zeroed VRAM answers at boot.  A WRITE-THEN-READ
 *     at the window still reads 0 here; the reference stores the byte.  That difference is
 *     U1's to close (vga_memory_write_text_mode), and results/vga-u0.md records it as the
 *     one guest-visible stand-in this unit leaves behind.
 *   - every VGA I/O PORT: UNCLAIMED, i.e. the reference empty entry's 0xFF/0xFFFF/-1 with
 *     `V86C_LOG_PORTS` naming the access.  Claiming a port here would mean inventing a
 *     register's value, which is precisely what a later unit must not inherit as truth.
 *   - the device's fields do carry the reference's CONSTRUCTOR DEFAULTS (vga.js:78-304:
 *     max_cols 80, max_rows 25, graphical_mode false, svga_enabled false, svga_bpp 32, the
 *     four dimensions 0).  Those are initial state, not register semantics; the units that
 *     make them change are each named on the field.
 *
 * THE ADAPTER IS INSTALLED BY machine_init() from machine_config.vga_screen (the reference's
 * own shape: the device always exists, `settings.screen` only picks the output).  There is
 * no --vga device flag: see results/vga-u0.md and the plan's Q4.
 */

#ifndef V86C_HW_VGA_H
#define V86C_HW_VGA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The device's own default VRAM size: cpu.js:1193's `settings.vga_memory_size ||
 * 8 * 1024 * 1024`, and the value MEASURED on the pinned reference's vga object at boot
 * (libv86 0.5.471+g94faf3d: vga.vga_memory_size == 8388608). */
#define VGA_DEFAULT_VRAM_SIZE 0x800000u

/* The window the device registers at vga.js:384: io.mmap_register(0xA0000, 0x20000, ...) --
 * the 128 KiB ISA aperture.  (The LFB is a SEPARATE buffer in the reference --
 * vga.js:368 `svga_memory` -- reached through memory.rs's in-wasm arms, which this port
 * already has in memory.c.) */
#define VGA_WINDOW_ADDRESS 0xA0000u
#define VGA_WINDOW_SIZE 0x20000u

/* One rectangle of the framebuffer, as update_buffer takes it (vga.js:2562-2575 and
 * 2575/2575's `this.layers`; the fields are the object literal screen_fill_buffer builds).
 * `buffer` is the reference's `image_data` (an ImageData over the 32-bit dest buffer) --
 * U0 has no dest buffer, so no adapter reads it, and U2 fills it in. */
typedef struct vga_layer {
    const uint32_t *buffer;
    int32_t screen_x;
    int32_t screen_y;
    int32_t buffer_x;
    int32_t buffer_y;
    int32_t buffer_width;
    int32_t buffer_height;
} vga_layer_t;

typedef struct vga_screen vga_screen_t;

/* THE SEAM: the eleven methods vga.js calls on its screen.  The names, the argument order
 * and the types are upstream's (screen.js / ansi_screen.js / dummy_screen.js); `bg` and `fg`
 * are the palette colours put_char receives (u32 ARGB-ish ints in the reference --
 * vga.js:864-874 reads them out of vga256_palette), `flags` carries the adapter-defined
 * FLAG_BLINKING / FLAG_FONT_PAGE_B bits (see vga_screen_flag_blinking below: the reference
 * defines them on the CANVAS adapter only, which is why the headless adapter has neither and
 * collapses the expression to 0 -- vga.js:848-849's behaviour, reproduced, not fixed).
 *
 * ONE NAMING QUIRK, so a reader of vga.js does not trip on it: vga.js computes
 * `visible = !disabled && start < end` and passes it as the third argument
 * (vga.js:1369-1375), while dummy_screen.js / ansi_screen.js name that parameter `max`.  The
 * seam's name (`visible`) is the value, not the adapters' parameter name. */
struct vga_screen {
    void (*set_mode)(vga_screen_t *screen, bool graphical);
    void (*set_size_text)(vga_screen_t *screen, int32_t cols, int32_t rows);
    void (*set_size_graphical)(vga_screen_t *screen, int32_t width, int32_t height,
                               int32_t virtual_width, int32_t virtual_height);
    void (*set_font_page)(vga_screen_t *screen, int32_t page_a, int32_t page_b);
    void (*set_font_bitmap)(vga_screen_t *screen, int32_t height, bool width_9px,
                            bool width_dbl, bool copy_8th_col, const uint8_t *bitmap,
                            bool bitmap_changed);
    void (*put_char)(vga_screen_t *screen, int32_t row, int32_t col, uint8_t chr,
                     int32_t flags, uint32_t bg, uint32_t fg);
    void (*update_buffer)(vga_screen_t *screen, const vga_layer_t *layers, int32_t count);
    void (*update_cursor)(vga_screen_t *screen, int32_t row, int32_t col);
    void (*update_cursor_scanline)(vga_screen_t *screen, int32_t start, int32_t end,
                                   bool visible);
    void (*clear_screen)(vga_screen_t *screen);
    void (*clear_text_state)(vga_screen_t *screen);

    void *opaque;
};

/* The three adapters the lane ships.  Each returns a pointer to static storage (the machine
 * is a singleton, like every other device in src/hw), so the device's `screen` pointer stays
 * valid for the process's life and a second machine_init() re-initialises it in place. */
typedef enum {
    VGA_SCREEN_HEADLESS = 0, /* the default: a port of browser/dummy_screen.js */
    VGA_SCREEN_ANSI = 1,     /* --vga-ansi: a port of browser/ansi_screen.js */
    VGA_SCREEN_DOC = 2,      /* --vga-doc FILE: the recording adapter (decision 2) */
} vga_screen_kind_t;

vga_screen_t *vga_screen_headless_init(void);
vga_screen_t *vga_screen_ansi_init(void);
vga_screen_t *vga_screen_document_init(void);

/* The see-it-working dump for --vga-ansi: the port has no UI to poll get_text_screen
 * (upstream's browser starter does), so the run's stop prints the adapter's text screen to
 * STDERR -- never stdout, which the M0 transcript owns.  A no-op for the other adapters. */
void vga_screen_ansi_flush(vga_screen_t *screen);

/* The per-adapter flag constants vga.js:848-849 reads off its screen object: FLAG_BLINKING
 * (0x01) and FLAG_FONT_PAGE_B (0x02) on the DOCUMENT adapter (the canvas adapter's stand-in,
 * whose reference-leg recorder defines them too), and 0 on the headless and ANSI adapters --
 * v86/src/browser/screen.js:30-34 defines them on the canvas adapter ONLY, so upstream's
 * dummy/ansi screens make the expression collapse to 0.  Reproduced, not fixed.  U1 is the
 * reader (the text-mode flush); U0 defines the seam. */
int32_t vga_screen_flag_blinking(vga_screen_t *screen);
int32_t vga_screen_flag_font_page_b(vga_screen_t *screen);

/* The headless adapter's raw character grid (a port of dummy_screen.js's get_text_screen /
 * get_text_row source, which upstream's UI and tests both read).  Returns NULL when the
 * adapter is not the headless one, or when no text size has been set yet. */
const uint8_t *vga_screen_headless_text(vga_screen_t *screen, int32_t *cols, int32_t *rows);

/* ------------------------------------------------------------------ *
 * THE DOCUMENT -- decision 2's verified path
 *
 * One file, three parts:
 *
 *     v86c-vga-doc/1\n                     the magic line
 *     {canonical JSON}\n                    the header (keys sorted, no whitespace)
 *     <payload_bytes>                       the raw payload
 *
 * TEXT payload (rows * cols records, row-major, 10 bytes each):
 *     chr u8, flags u8, bg u32 LE, fg u32 LE -- the put_char call stream collapsed to the
 *     final grid.  A cell the adapter never received a put_char for is all zeros.
 *
 * WHY RAW + JSON AND NOT PPM/PGM: the device's own buffer is 32-bit ARGB (an ImageData over
 * the dest buffer, vga.js:2512-2516) so a PPM would discard alpha and force both legs to
 * implement an identical transform -- a divergence surface for zero gain; and headless TEXT
 * mode has no pixel buffer at all in the reference (MEASURED: image_data is false after a
 * text-mode run), so the put_char grid is the only lossless text document.  No charmap
 * (cp437->Unicode stays out: raw chr bytes are host-independent), no timestamps, no pointer
 * values -- the document is a pure function of (image bytes, limit, machine geometry).
 *
 * GRAPHICAL DOCUMENTS ARE U2's and this writer REFUSES them rather than emitting a
 * half-truth: the payload there is virtual_width*virtual_height*4 raw RGBA bytes of the
 * dest buffer, which U0 does not have (there is no mode-13 path to reach it with either).
 * ------------------------------------------------------------------ */
typedef struct vga_doc_meta {
    const char *image_sha256; /* 64 lowercase hex chars: main.c's digest of the image */
    uint64_t image_bytes;
    uint64_t instruction_limit;
    uint64_t instruction_count;
    bool graphical_mode;
    bool svga_enabled;
    int32_t svga_bpp;
    int32_t screen_width;
    int32_t screen_height;
    int32_t virtual_width;
    int32_t virtual_height;
    int32_t cols;
    int32_t rows;
} vga_doc_meta_t;

/* Writes the document to `path`.  False (with a stderr line) on a non-document adapter, a
 * graphical mode (U2's), an unwritable path, or a payload larger than the writer's grid.
 * The file is written whole or not at all: it goes to "<path>.tmp" and is renamed. */
bool vga_screen_document_write(vga_screen_t *screen, const char *path,
                               const vga_doc_meta_t *meta);

/* ------------------------------------------------------------------ *
 * the device
 * ------------------------------------------------------------------ */
typedef struct vga_device {
    /* the reference's always-present device object: the adapter it renders through, and
     * the fields the document header is taken from. */
    vga_screen_t *screen;
    vga_screen_kind_t screen_kind;

    uint32_t vga_memory_size; /* vga.js:216-228 (clamped to [256 KiB, 256 MiB]) */

    int32_t max_cols; /* vga.js:89-99: 80 x 25 at construction */
    int32_t max_rows;

    /* vga.js:171-180: the constructor's values.  U1 makes them move (the attribute
     * controller's bit 0 flips graphical_mode, the dispi registers set svga_enabled and
     * the dimensions, update_vga_size recomputes screen_width/height); every reader in
     * this unit is the document header, which is why the seat is here and empty. */
    bool graphical_mode;
    bool svga_enabled;
    int32_t svga_bpp;
    int32_t screen_width;
    int32_t screen_height;
    int32_t virtual_width;
    int32_t virtual_height;

    bool installed;
} vga_device_t;

/* cpu.js:1193's slot (after the DMA, before the PS/2): allocate the 8 MiB VRAM, register the
 * 0xA0000 window and the PCI function 00:12.0, install `screen`.  Requires io_init() and
 * pci_init() to have run (machine_init's order).  Idempotent: a second call re-initialises
 * the same storage -- the machine is a singleton. */
void vga_init(vga_screen_t *screen, vga_screen_kind_t kind);

vga_device_t *vga_device(void);

/* main.c's --vga-doc at the run's stop: fill the header's device half from the device and
 * hand the document to the installed adapter.  `image_sha256` is the transcript's own
 * digest (main.c already computes it; the port keeps ONE sha256 implementation). */
bool vga_write_document(const char *path, const char *image_sha256, uint64_t image_bytes,
                        uint64_t instruction_limit, uint64_t instruction_count);

#endif /* V86C_HW_VGA_H */
