/*
 * vga_screen.c -- the three rendering adapters and the document writer.
 *
 * THE SEAM'S THREE IMPLEMENTATIONS, each a port of one browser adapter:
 *
 *   headless  browser/dummy_screen.js  (134 lines) -- the DEFAULT (decision 1).  It keeps
 *             the character grid and nothing else: put_char stores the BYTE, no colours, no
 *             pixel buffer, no font.  That is upstream's shape, and it is what makes a
 *             headless run "a VGA that draws nothing" while its register behaviour is real.
 *   ansi      browser/ansi_screen.js   (175 lines) -- decision 3, the see-it-working adapter:
 *             one row of truecolor escapes, printed at the stop (the port has no UI to poll
 *             get_text_screen, so vga_screen_ansi_flush is its reader).
 *   document  the lane's own (decision 2): the same put_char stream, recorded with its flags
 *             and palette colours, and serialized into the byte-comparable document vga.h
 *             specifies.  This is the adapter the two-leg gate runs both machines through.
 *
 * THE FLAG CONSTANTS ARE PER-ADAPTER, AND THAT IS LOAD-BEARING.  screen.js:30-34 defines
 * FLAG_BLINKING = 0x01 and FLAG_FONT_PAGE_B = 0x02 on the CANVAS adapter only; dummy_screen.js
 * and ansi_screen.js define NEITHER, so on them `this.screen.FLAG_BLINKING` is `undefined`
 * and vga.js:848-849's flags expression collapses to 0.  Reproduced rather than fixed
 * (reproducing it is what keeps the document's flags byte equal to the reference leg's): the
 * headless adapter here has no flag constants, the ANSI one has none, the DOCUMENT one
 * DEFINES both -- because the reference leg's recorder in oracle/run-vga.mjs defines them
 * too, and it is standing in for a canvas adapter.
 *
 * Everything here is reached only through the seam, so the device code never learns which
 * adapter is installed.
 */

#include "vga.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VGA_FLAG_BLINKING 0x01
#define VGA_FLAG_FONT_PAGE_B 0x02

/* The text-grid caps.  The reference has none -- it resizes a typed array -- but cols and
 * rows are guest-visible numbers (U1 recomputes them from the CRTC's offset register), so a
 * C adapter needs a hard bound rather than an allocation a guest can drive.  256 x 128 is
 * twice the largest text mode this lane targets (80x25 default, 132x60 for SVGA text) and
 * the cap is REPORTED when it is hit, never a silent truncation. */
#define VGA_TEXT_MAX_COLS 256
#define VGA_TEXT_MAX_ROWS 128
#define VGA_TEXT_MAX_CELLS (VGA_TEXT_MAX_COLS * VGA_TEXT_MAX_ROWS)

/* One adapter kind byte, so a document write can prove it holds a document adapter without
 * a second registry. */
#define VGA_ADAPTER_MAGIC 0x56474130u /* "VGA0" */

/* Every adapter's state starts with the magic and the kind's own fields. */
typedef struct vga_adapter_common {
    uint32_t magic;
    bool is_graphical;
    int32_t cursor_row;
    int32_t cursor_col;
    int32_t graphical_width;
    int32_t graphical_height;
    int32_t cols;
    int32_t rows;
    bool warned_bounds;
} vga_adapter_common_t;

static void adapter_bounds_warning(vga_adapter_common_t *c, const char *what, int32_t row,
                                   int32_t col)
{
    /* ONE line per adapter (a redraw of a large grid would otherwise print per cell).  The
     * reference's typed-array store out of range is a silent no-op in a release build; this
     * port drops the cell too, and says so once -- the write_blob / kernel-loader style. */
    if (c->warned_bounds) return;
    c->warned_bounds = true;
    fprintf(stderr, "v86c: vga: adapter dropped %s(%d,%d): outside the %dx%d text grid\n", what,
            (int)row, (int)col, (int)c->cols, (int)c->rows);
}

/* ================================================================== *
 * 1. THE HEADLESS ADAPTER -- browser/dummy_screen.js
 * ================================================================== */
typedef struct {
    vga_adapter_common_t common;
    uint8_t text_data[VGA_TEXT_MAX_CELLS]; /* dummy_screen.js's text_mode_data: chr only */
} headless_state_t;

static void headless_set_mode(vga_screen_t *screen, bool graphical)
{
    headless_state_t *s = screen->opaque;
    s->common.is_graphical = graphical;
}

static void headless_set_size_text(vga_screen_t *screen, int32_t cols, int32_t rows)
{
    headless_state_t *s = screen->opaque;
    /* dummy_screen.js: `if(cols === text_mode_width && rows === text_mode_height) return;`
     * then a FRESH typed array -- i.e. a resize CLEARS the grid. */
    if (cols == s->common.cols && rows == s->common.rows) return;
    if (cols <= 0 || rows <= 0 || cols > VGA_TEXT_MAX_COLS || rows > VGA_TEXT_MAX_ROWS) {
        fprintf(stderr, "v86c: vga: headless set_size_text(%d,%d) refused: the port's grid is"
                        " bounded at %dx%d\n",
                (int)cols, (int)rows, VGA_TEXT_MAX_COLS, VGA_TEXT_MAX_ROWS);
        s->common.cols = 0;
        s->common.rows = 0;
        return;
    }
    memset(s->text_data, 0, sizeof(s->text_data));
    s->common.cols = cols;
    s->common.rows = rows;
}

static void headless_set_size_graphical(vga_screen_t *screen, int32_t width, int32_t height,
                                        int32_t virtual_width, int32_t virtual_height)
{
    headless_state_t *s = screen->opaque;
    (void)virtual_width;
    (void)virtual_height;
    s->common.graphical_width = width;
    s->common.graphical_height = height;
}

static void headless_set_font_page(vga_screen_t *screen, int32_t page_a, int32_t page_b)
{
    (void)screen;
    (void)page_a;
    (void)page_b;
}

static void headless_set_font_bitmap(vga_screen_t *screen, int32_t height, bool width_9px,
                                     bool width_dbl, bool copy_8th_col, const uint8_t *bitmap,
                                     bool bitmap_changed)
{
    (void)screen;
    (void)height;
    (void)width_9px;
    (void)width_dbl;
    (void)copy_8th_col;
    (void)bitmap;
    (void)bitmap_changed;
}

static void headless_put_char(vga_screen_t *screen, int32_t row, int32_t col, uint8_t chr,
                              int32_t flags, uint32_t bg, uint32_t fg)
{
    headless_state_t *s = screen->opaque;
    (void)flags; /* dummy_screen.js ignores the blink/font-page flags: it stores the char */
    (void)bg;
    (void)fg;
    if (row < 0 || row >= s->common.rows || col < 0 || col >= s->common.cols) {
        adapter_bounds_warning(&s->common, "put_char", row, col);
        return;
    }
    s->text_data[row * s->common.cols + col] = chr;
}

static void headless_update_buffer(vga_screen_t *screen, const vga_layer_t *layers,
                                   int32_t count)
{
    (void)screen;
    (void)layers;
    (void)count;
}

static void headless_update_cursor(vga_screen_t *screen, int32_t row, int32_t col)
{
    headless_state_t *s = screen->opaque;
    s->common.cursor_row = row;
    s->common.cursor_col = col;
}

static void headless_update_cursor_scanline(vga_screen_t *screen, int32_t start, int32_t end,
                                            bool visible)
{
    (void)screen;
    (void)start;
    (void)end;
    (void)visible;
}

static void headless_clear_screen(vga_screen_t *screen) { (void)screen; }

static void headless_clear_text_state(vga_screen_t *screen)
{
    headless_state_t *s = screen->opaque;
    /* dummy_screen.js sets text_mode_width/height to null (the next set_size_text then
     * rebuilds the grid); the port uses 0, and keeps the bytes, which nothing reads while
     * cols/rows are 0. */
    s->common.cols = 0;
    s->common.rows = 0;
}

static vga_screen_t headless_screen;
static headless_state_t headless_opaque;

vga_screen_t *vga_screen_headless_init(void)
{
    memset(&headless_opaque, 0, sizeof(headless_opaque));
    headless_opaque.common.magic = VGA_ADAPTER_MAGIC;

    headless_screen.set_mode = headless_set_mode;
    headless_screen.set_size_text = headless_set_size_text;
    headless_screen.set_size_graphical = headless_set_size_graphical;
    headless_screen.set_font_page = headless_set_font_page;
    headless_screen.set_font_bitmap = headless_set_font_bitmap;
    headless_screen.put_char = headless_put_char;
    headless_screen.update_buffer = headless_update_buffer;
    headless_screen.update_cursor = headless_update_cursor;
    headless_screen.update_cursor_scanline = headless_update_cursor_scanline;
    headless_screen.clear_screen = headless_clear_screen;
    headless_screen.clear_text_state = headless_clear_text_state;
    headless_screen.opaque = &headless_opaque;

    /* dummy_screen.js's last line: `this.set_size_text(80, 25)` -- the constructor's own
     * default, which is what makes the adapter usable before any device call. */
    headless_set_size_text(&headless_screen, 80, 25);
    return &headless_screen;
}

const uint8_t *vga_screen_headless_text(vga_screen_t *screen, int32_t *cols, int32_t *rows)
{
    if (screen != &headless_screen) return NULL;
    headless_state_t *s = screen->opaque;
    if (cols != NULL) *cols = s->common.cols;
    if (rows != NULL) *rows = s->common.rows;
    if (s->common.cols == 0 || s->common.rows == 0) return NULL;
    return s->text_data;
}

/* ================================================================== *
 * 2. THE ANSI ADAPTER -- browser/ansi_screen.js
 *
 * Index 0: ASCII code, index 1: background colour, index 2: foreground colour, three int32
 * per cell.  The rendering is ansi_screen.js's get_text_row: a truecolor escape only when a
 * cell's colour differs from the PREVIOUS cell's on that row, and ANSI_RESET at the end.
 * ================================================================== */
typedef struct {
    vga_adapter_common_t common;
    int32_t text_data[VGA_TEXT_MAX_CELLS * 3];
} ansi_state_t;

#define ANSI_CHARACTER_INDEX 0
#define ANSI_BG_COLOR_INDEX 1
#define ANSI_FG_COLOR_INDEX 2
#define ANSI_TEXT_BUF_COMPONENT_SIZE 3

static void ansi_set_mode(vga_screen_t *screen, bool graphical)
{
    ansi_state_t *s = screen->opaque;
    s->common.is_graphical = graphical;
}

static void ansi_set_size_text(vga_screen_t *screen, int32_t cols, int32_t rows)
{
    ansi_state_t *s = screen->opaque;
    if (cols == s->common.cols && rows == s->common.rows) return;
    if (cols <= 0 || rows <= 0 || cols > VGA_TEXT_MAX_COLS || rows > VGA_TEXT_MAX_ROWS) {
        fprintf(stderr, "v86c: vga: ansi set_size_text(%d,%d) refused: the port's grid is"
                        " bounded at %dx%d\n",
                (int)cols, (int)rows, VGA_TEXT_MAX_COLS, VGA_TEXT_MAX_ROWS);
        s->common.cols = 0;
        s->common.rows = 0;
        return;
    }
    memset(s->text_data, 0, sizeof(s->text_data));
    s->common.cols = cols;
    s->common.rows = rows;
}

static void ansi_set_size_graphical(vga_screen_t *screen, int32_t width, int32_t height,
                                    int32_t virtual_width, int32_t virtual_height)
{
    ansi_state_t *s = screen->opaque;
    (void)virtual_width;
    (void)virtual_height;
    s->common.graphical_width = width;
    s->common.graphical_height = height;
}

static void ansi_set_font_page(vga_screen_t *screen, int32_t page_a, int32_t page_b)
{
    (void)screen;
    (void)page_a;
    (void)page_b;
}

static void ansi_set_font_bitmap(vga_screen_t *screen, int32_t height, bool width_9px,
                                 bool width_dbl, bool copy_8th_col, const uint8_t *bitmap,
                                 bool bitmap_changed)
{
    (void)screen;
    (void)height;
    (void)width_9px;
    (void)width_dbl;
    (void)copy_8th_col;
    (void)bitmap;
    (void)bitmap_changed;
}

static void ansi_put_char(vga_screen_t *screen, int32_t row, int32_t col, uint8_t chr,
                          int32_t flags, uint32_t bg, uint32_t fg)
{
    ansi_state_t *s = screen->opaque;
    (void)flags; /* ansi_screen.js ignores the flags the same way dummy_screen.js does */
    if (row < 0 || row >= s->common.rows || col < 0 || col >= s->common.cols) {
        adapter_bounds_warning(&s->common, "put_char", row, col);
        return;
    }
    int32_t p = ANSI_TEXT_BUF_COMPONENT_SIZE * (row * s->common.cols + col);
    s->text_data[p + ANSI_CHARACTER_INDEX] = chr;
    s->text_data[p + ANSI_BG_COLOR_INDEX] = (int32_t)bg;
    s->text_data[p + ANSI_FG_COLOR_INDEX] = (int32_t)fg;
}

static void ansi_update_buffer(vga_screen_t *screen, const vga_layer_t *layers, int32_t count)
{
    (void)screen;
    (void)layers;
    (void)count;
}

static void ansi_update_cursor(vga_screen_t *screen, int32_t row, int32_t col)
{
    ansi_state_t *s = screen->opaque;
    s->common.cursor_row = row;
    s->common.cursor_col = col;
}

static void ansi_update_cursor_scanline(vga_screen_t *screen, int32_t start, int32_t end,
                                        bool visible)
{
    (void)screen;
    (void)start;
    (void)end;
    (void)visible;
}

static void ansi_clear_screen(vga_screen_t *screen) { (void)screen; }

static void ansi_clear_text_state(vga_screen_t *screen)
{
    ansi_state_t *s = screen->opaque;
    s->common.cols = 0;
    s->common.rows = 0;
}

static vga_screen_t ansi_screen;
static ansi_state_t ansi_opaque;

vga_screen_t *vga_screen_ansi_init(void)
{
    memset(&ansi_opaque, 0, sizeof(ansi_opaque));
    ansi_opaque.common.magic = VGA_ADAPTER_MAGIC;

    ansi_screen.set_mode = ansi_set_mode;
    ansi_screen.set_size_text = ansi_set_size_text;
    ansi_screen.set_size_graphical = ansi_set_size_graphical;
    ansi_screen.set_font_page = ansi_set_font_page;
    ansi_screen.set_font_bitmap = ansi_set_font_bitmap;
    ansi_screen.put_char = ansi_put_char;
    ansi_screen.update_buffer = ansi_update_buffer;
    ansi_screen.update_cursor = ansi_update_cursor;
    ansi_screen.update_cursor_scanline = ansi_update_cursor_scanline;
    ansi_screen.clear_screen = ansi_clear_screen;
    ansi_screen.clear_text_state = ansi_clear_text_state;
    ansi_screen.opaque = &ansi_opaque;

    ansi_set_size_text(&ansi_screen, 80, 25); /* ansi_screen.js's own last line */
    return &ansi_screen;
}

/* The cp437 -> UTF-8 table, from the reference's own data: v86/src/lib.js's CHARMAPS.cp437
 * (lib.js:782), which get_charmap returns for a falsey encoding.  Each entry is the glyph's
 * UTF-8 bytes, so `cp437[chr]` prints what the reference's browser adapter would draw.
 * Generated from that literal rather than transcribed -- a hand-typed 256-entry table is
 * exactly the sort of thing that is wrong in three places forever. */
static const char *const cp437_to_utf8[256] = {
    " ",
    "\xe2\x98\xba",
    "\xe2\x98\xbb",
    "\xe2\x99\xa5",
    "\xe2\x99\xa6",
    "\xe2\x99\xa3",
    "\xe2\x99\xa0",
    "\xe2\x80\xa2",
    "\xe2\x97\x98",
    "\xe2\x97\x8b",
    "\xe2\x97\x99",
    "\xe2\x99\x82",
    "\xe2\x99\x80",
    "\xe2\x99\xaa",
    "\xe2\x99\xab",
    "\xe2\x98\xbc",
    "\xe2\x96\xba",
    "\xe2\x97\x84",
    "\xe2\x86\x95",
    "\xe2\x80\xbc",
    "\xc2\xb6",
    "\xc2\xa7",
    "\xe2\x96\xac",
    "\xe2\x86\xa8",
    "\xe2\x86\x91",
    "\xe2\x86\x93",
    "\xe2\x86\x92",
    "\xe2\x86\x90",
    "\xe2\x88\x9f",
    "\xe2\x86\x94",
    "\xe2\x96\xb2",
    "\xe2\x96\xbc",
    " ",
    "!",
    "\x22",
    "#",
    "$",
    "%",
    "&",
    "'",
    "(",
    ")",
    "*",
    "+",
    ",",
    "-",
    ".",
    "/",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    ":",
    ";",
    "<",
    "=",
    ">",
    "?",
    "@",
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
    "I",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "P",
    "Q",
    "R",
    "S",
    "T",
    "U",
    "V",
    "W",
    "X",
    "Y",
    "Z",
    "[",
    "\x5c",
    "]",
    "^",
    "_",
    "`",
    "a",
    "b",
    "c",
    "d",
    "e",
    "f",
    "g",
    "h",
    "i",
    "j",
    "k",
    "l",
    "m",
    "n",
    "o",
    "p",
    "q",
    "r",
    "s",
    "t",
    "u",
    "v",
    "w",
    "x",
    "y",
    "z",
    "{",
    "|",
    "}",
    "~",
    "\xe2\x8c\x82",
    "\xc3\x87",
    "\xc3\xbc",
    "\xc3\xa9",
    "\xc3\xa2",
    "\xc3\xa4",
    "\xc3\xa0",
    "\xc3\xa5",
    "\xc3\xa7",
    "\xc3\xaa",
    "\xc3\xab",
    "\xc3\xa8",
    "\xc3\xaf",
    "\xc3\xae",
    "\xc3\xac",
    "\xc3\x84",
    "\xc3\x85",
    "\xc3\x89",
    "\xc3\xa6",
    "\xc3\x86",
    "\xc3\xb4",
    "\xc3\xb6",
    "\xc3\xb2",
    "\xc3\xbb",
    "\xc3\xb9",
    "\xc3\xbf",
    "\xc3\x96",
    "\xc3\x9c",
    "\xc2\xa2",
    "\xc2\xa3",
    "\xc2\xa5",
    "\xe2\x82\xa7",
    "\xc6\x92",
    "\xc3\xa1",
    "\xc3\xad",
    "\xc3\xb3",
    "\xc3\xba",
    "\xc3\xb1",
    "\xc3\x91",
    "\xc2\xaa",
    "\xc2\xba",
    "\xc2\xbf",
    "\xe2\x8c\x90",
    "\xc2\xac",
    "\xc2\xbd",
    "\xc2\xbc",
    "\xc2\xa1",
    "\xc2\xab",
    "\xc2\xbb",
    "\xe2\x96\x91",
    "\xe2\x96\x92",
    "\xe2\x96\x93",
    "\xe2\x94\x82",
    "\xe2\x94\xa4",
    "\xe2\x95\xa1",
    "\xe2\x95\xa2",
    "\xe2\x95\x96",
    "\xe2\x95\x95",
    "\xe2\x95\xa3",
    "\xe2\x95\x91",
    "\xe2\x95\x97",
    "\xe2\x95\x9d",
    "\xe2\x95\x9c",
    "\xe2\x95\x9b",
    "\xe2\x94\x90",
    "\xe2\x94\x94",
    "\xe2\x94\xb4",
    "\xe2\x94\xac",
    "\xe2\x94\x9c",
    "\xe2\x94\x80",
    "\xe2\x94\xbc",
    "\xe2\x95\x9e",
    "\xe2\x95\x9f",
    "\xe2\x95\x9a",
    "\xe2\x95\x94",
    "\xe2\x95\xa9",
    "\xe2\x95\xa6",
    "\xe2\x95\xa0",
    "\xe2\x95\x90",
    "\xe2\x95\xac",
    "\xe2\x95\xa7",
    "\xe2\x95\xa8",
    "\xe2\x95\xa4",
    "\xe2\x95\xa5",
    "\xe2\x95\x99",
    "\xe2\x95\x98",
    "\xe2\x95\x92",
    "\xe2\x95\x93",
    "\xe2\x95\xab",
    "\xe2\x95\xaa",
    "\xe2\x94\x98",
    "\xe2\x94\x8c",
    "\xe2\x96\x88",
    "\xe2\x96\x84",
    "\xe2\x96\x8c",
    "\xe2\x96\x90",
    "\xe2\x96\x80",
    "\xce\xb1",
    "\xc3\x9f",
    "\xce\x93",
    "\xcf\x80",
    "\xce\xa3",
    "\xcf\x83",
    "\xc2\xb5",
    "\xcf\x84",
    "\xce\xa6",
    "\xce\x98",
    "\xce\xa9",
    "\xce\xb4",
    "\xe2\x88\x9e",
    "\xcf\x86",
    "\xce\xb5",
    "\xe2\x88\xa9",
    "\xe2\x89\xa1",
    "\xc2\xb1",
    "\xe2\x89\xa5",
    "\xe2\x89\xa4",
    "\xe2\x8c\xa0",
    "\xe2\x8c\xa1",
    "\xc3\xb7",
    "\xe2\x89\x88",
    "\xc2\xb0",
    "\xe2\x88\x99",
    "\xc2\xb7",
    "\xe2\x88\x9a",
    "\xe2\x81\xbf",
    "\xc2\xb2",
    "\xe2\x96\xa0",
    " ",
};

static void ansi_write_truecolor(FILE *out, uint32_t color)
{
    /* ansi_screen.js's hex_to_ansi_truecolor: `2;R;G;B` from 0x00RRGGBB. */
    fprintf(out, "2;%u;%u;%u", (unsigned)((color & 0xFF0000) >> 16),
            (unsigned)((color & 0x00FF00) >> 8), (unsigned)(color & 0x0000FF));
}

void vga_screen_ansi_flush(vga_screen_t *screen)
{
    if (screen != &ansi_screen) return;
    ansi_state_t *s = screen->opaque;
    if (s->common.cols == 0 || s->common.rows == 0) {
        /* ansi_screen.js's get_text_screen over a null grid yields an empty screen; the
         * port says which state it is in rather than printing nothing at all. */
        fprintf(stderr, "ansi: no text size set; nothing to print\n");
        return;
    }
    for (int32_t row = 0; row < s->common.rows; row++) {
        int32_t previous_bg = -1;
        int32_t previous_fg = -1;
        bool have_previous = false;
        for (int32_t col = 0; col < s->common.cols; col++) {
            int32_t p = ANSI_TEXT_BUF_COMPONENT_SIZE * (row * s->common.cols + col);
            uint8_t chr = (uint8_t)s->text_data[p + ANSI_CHARACTER_INDEX];
            uint32_t bg = (uint32_t)s->text_data[p + ANSI_BG_COLOR_INDEX];
            uint32_t fg = (uint32_t)s->text_data[p + ANSI_FG_COLOR_INDEX];
            /* ansi_screen.js compares against the previous cell's colour, with `null`
             * meaning "nothing emitted yet"; -1 is this port's null and `have_previous` keeps
             * a real -1 colour from aliasing it (there is none, but the flag is cheaper than
             * the argument). */
            if (!have_previous || bg != (uint32_t)previous_bg) {
                fprintf(stderr, "\x1B[48;");
                ansi_write_truecolor(stderr, bg);
                fputc('m', stderr);
                previous_bg = (int32_t)bg;
            }
            if (!have_previous || fg != (uint32_t)previous_fg) {
                fprintf(stderr, "\x1B[38;");
                ansi_write_truecolor(stderr, fg);
                fputc('m', stderr);
                previous_fg = (int32_t)fg;
            }
            have_previous = true;
            fputs(cp437_to_utf8[chr], stderr);
        }
        fputs("\x1B[0m\n", stderr); /* ANSI_RESET, as get_text_row ends every row */
    }
}

/* ================================================================== *
 * 3. THE DOCUMENT ADAPTER -- the lane's own recorder (decision 2)
 *
 * It defines the two flag constants (see the file header) and keeps what the reference's
 * canvas adapter keeps for a text mode: the character, the flags and the two palette
 * colours.  The payload's layout is vga.h's.
 * ================================================================== */
typedef struct {
    vga_adapter_common_t common;
    uint8_t chr[VGA_TEXT_MAX_CELLS];
    uint8_t flags[VGA_TEXT_MAX_CELLS];
    uint32_t bg[VGA_TEXT_MAX_CELLS];
    uint32_t fg[VGA_TEXT_MAX_CELLS];
    int32_t scanline_start;
    int32_t scanline_end;
    bool scanline_visible;
} document_state_t;

static void document_set_mode(vga_screen_t *screen, bool graphical)
{
    document_state_t *s = screen->opaque;
    s->common.is_graphical = graphical;
}

static void document_set_size_text(vga_screen_t *screen, int32_t cols, int32_t rows)
{
    document_state_t *s = screen->opaque;
    if (cols == s->common.cols && rows == s->common.rows) return;
    if (cols <= 0 || rows <= 0 || cols > VGA_TEXT_MAX_COLS || rows > VGA_TEXT_MAX_ROWS) {
        fprintf(stderr, "v86c: vga: document set_size_text(%d,%d) refused: the port's grid is"
                        " bounded at %dx%d\n",
                (int)cols, (int)rows, VGA_TEXT_MAX_COLS, VGA_TEXT_MAX_ROWS);
        s->common.cols = 0;
        s->common.rows = 0;
        return;
    }
    memset(s->chr, 0, sizeof(s->chr));
    memset(s->flags, 0, sizeof(s->flags));
    memset(s->bg, 0, sizeof(s->bg));
    memset(s->fg, 0, sizeof(s->fg));
    s->common.cols = cols;
    s->common.rows = rows;
}

static void document_set_size_graphical(vga_screen_t *screen, int32_t width, int32_t height,
                                        int32_t virtual_width, int32_t virtual_height)
{
    document_state_t *s = screen->opaque;
    (void)virtual_width;
    (void)virtual_height;
    s->common.graphical_width = width;
    s->common.graphical_height = height;
}

static void document_set_font_page(vga_screen_t *screen, int32_t page_a, int32_t page_b)
{
    (void)screen;
    (void)page_a;
    (void)page_b;
}

static void document_set_font_bitmap(vga_screen_t *screen, int32_t height, bool width_9px,
                                     bool width_dbl, bool copy_8th_col, const uint8_t *bitmap,
                                     bool bitmap_changed)
{
    /* The glyph SHAPES are the canvas adapter's; a document records what the device sent to
     * the screen, not pixels.  U1 is the first unit that calls this (set_font_bitmap is
     * driven by max_scan_line, vga.js:2585-2598). */
    (void)screen;
    (void)height;
    (void)width_9px;
    (void)width_dbl;
    (void)copy_8th_col;
    (void)bitmap;
    (void)bitmap_changed;
}

static void document_put_char(vga_screen_t *screen, int32_t row, int32_t col, uint8_t chr,
                              int32_t flags, uint32_t bg, uint32_t fg)
{
    document_state_t *s = screen->opaque;
    if (row < 0 || row >= s->common.rows || col < 0 || col >= s->common.cols) {
        adapter_bounds_warning(&s->common, "put_char", row, col);
        return;
    }
    int32_t cell = row * s->common.cols + col;
    s->chr[cell] = chr;
    s->flags[cell] = (uint8_t)flags;
    s->bg[cell] = bg;
    s->fg[cell] = fg;
}

static void document_update_buffer(vga_screen_t *screen, const vga_layer_t *layers,
                                   int32_t count)
{
    /* U2's (the graphical path).  Recorded as "not read", so a document written from a
     * graphical mode refuses rather than pretending a text payload is the screen. */
    (void)screen;
    (void)layers;
    (void)count;
}

static void document_update_cursor(vga_screen_t *screen, int32_t row, int32_t col)
{
    document_state_t *s = screen->opaque;
    s->common.cursor_row = row;
    s->common.cursor_col = col;
}

static void document_update_cursor_scanline(vga_screen_t *screen, int32_t start, int32_t end,
                                            bool visible)
{
    document_state_t *s = screen->opaque;
    s->scanline_start = start;
    s->scanline_end = end;
    s->scanline_visible = visible;
}

static void document_clear_screen(vga_screen_t *screen) { (void)screen; }

static void document_clear_text_state(vga_screen_t *screen)
{
    document_state_t *s = screen->opaque;
    /* ansi_screen.js / dummy_screen.js drop the grid here (the next set_size_text rebuilds
     * it).  The document adapter does the same, and ALSO drops the bytes: an instrument that
     * says "text state cleared" and then reports the old grid would be the lying-comment
     * class. */
    memset(s->chr, 0, sizeof(s->chr));
    memset(s->flags, 0, sizeof(s->flags));
    memset(s->bg, 0, sizeof(s->bg));
    memset(s->fg, 0, sizeof(s->fg));
    s->common.cols = 0;
    s->common.rows = 0;
}

static vga_screen_t document_screen;
static document_state_t document_opaque;

vga_screen_t *vga_screen_document_init(void)
{
    memset(&document_opaque, 0, sizeof(document_opaque));
    document_opaque.common.magic = VGA_ADAPTER_MAGIC;

    document_screen.set_mode = document_set_mode;
    document_screen.set_size_text = document_set_size_text;
    document_screen.set_size_graphical = document_set_size_graphical;
    document_screen.set_font_page = document_set_font_page;
    document_screen.set_font_bitmap = document_set_font_bitmap;
    document_screen.put_char = document_put_char;
    document_screen.update_buffer = document_update_buffer;
    document_screen.update_cursor = document_update_cursor;
    document_screen.update_cursor_scanline = document_update_cursor_scanline;
    document_screen.clear_screen = document_clear_screen;
    document_screen.clear_text_state = document_clear_text_state;
    document_screen.opaque = &document_opaque;

    /* The three adapters' constructors all end with set_size_text(80, 25) in the reference;
     * the document adapter is not a reference adapter (it stands in for the canvas one), so
     * it does NOT seed a grid here: an unseeded grid is what lets the writer prove the
     * difference between "the device never called set_size_text" (U0) and "it did" (U1). */
    return &document_screen;
}

/* ------------------------------------------------------------------ *
 * the document writer
 * ------------------------------------------------------------------ */
static void put_u32_le(FILE *out, uint32_t value)
{
    uint8_t bytes[4] = { (uint8_t)(value & 0xFF), (uint8_t)(value >> 8 & 0xFF),
                         (uint8_t)(value >> 16 & 0xFF), (uint8_t)(value >> 24 & 0xFF) };
    fwrite(bytes, 1, sizeof(bytes), out);
}

bool vga_screen_document_write(vga_screen_t *screen, const char *path,
                               const vga_doc_meta_t *meta)
{
    if (meta->graphical_mode) {
        fprintf(stderr,
                "v86c: vga: refusing to write a GRAPHICAL document: its payload is the dest"
                " buffer's %dx%d pixels and unit U2 owns that path (results/vga-u0.md)\n",
                (int)meta->virtual_width, (int)meta->virtual_height);
        return false;
    }
    if (meta->cols <= 0 || meta->rows <= 0 || meta->cols > VGA_TEXT_MAX_COLS ||
        meta->rows > VGA_TEXT_MAX_ROWS) {
        fprintf(stderr, "v86c: vga: refusing to write a document for a %dx%d text grid"
                        " (the port's grid is bounded at %dx%d)\n",
                (int)meta->cols, (int)meta->rows, VGA_TEXT_MAX_COLS, VGA_TEXT_MAX_ROWS);
        return false;
    }

    /* The grid the device asked for, or zeros when the device never sized the adapter (U0:
     * no register path calls set_size_text, so every cell is "never drawn").  A grid of a
     * DIFFERENT size is not reinterpreted: a row-major grid indexed by another mode's width
     * would be silently wrong, so it is treated as absent and said so. */
    document_state_t *doc = NULL;
    if (screen != NULL && screen->opaque != NULL &&
        ((vga_adapter_common_t *)screen->opaque)->magic == VGA_ADAPTER_MAGIC &&
        screen == &document_screen) {
        doc = screen->opaque;
    }
    if (doc == NULL) {
        fprintf(stderr, "v86c: vga: the document adapter is not installed (--vga-doc without"
                        " selecting it)\n");
        return false;
    }
    bool have_grid = doc->common.cols == meta->cols && doc->common.rows == meta->rows;
    if (!have_grid && doc->common.cols != 0) {
        fprintf(stderr, "v86c: vga: the recorded grid is %dx%d but the device reports %dx%d;"
                        " writing a zero grid rather than reinterpreting it\n",
                (int)doc->common.cols, (int)doc->common.rows, (int)meta->cols, (int)meta->rows);
    }

    /* Whole-or-nothing: `<path>.tmp` then rename, so a failed write never leaves a document
     * a gate could compare. */
    size_t tmp_length = strlen(path) + 5;
    char *tmp_path = malloc(tmp_length);
    if (tmp_path == NULL) {
        fprintf(stderr, "v86c: vga: out of memory naming the document's temporary file\n");
        return false;
    }
    snprintf(tmp_path, tmp_length, "%s.tmp", path);

    FILE *out = fopen(tmp_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "v86c: vga: cannot open %s for writing\n", tmp_path);
        free(tmp_path);
        return false;
    }

    uint64_t payload_bytes = (uint64_t)meta->cols * (uint64_t)meta->rows * 10ull;

    /* The header.  Keys sorted (the M0 transcript's canonical form, oracle/run.mjs:184-196),
     * one line, no whitespace. */
    fputs("v86c-vga-doc/1\n", out);
    fprintf(out,
            "{\"bpp\":%d,\"cols\":%d,\"cursor_col\":%d,\"cursor_row\":%d,"
            "\"cursor_scanline\":{\"end\":%d,\"start\":%d,\"visible\":%s},"
            "\"format\":1,\"graphical_mode\":%s,\"image_bytes\":%llu,\"image_sha256\":\"%s\","
            "\"instruction_count\":%llu,\"instruction_limit\":%llu,\"mode\":\"text\","
            "\"payload_bytes\":%llu,\"rows\":%d,\"screen_height\":%d,\"screen_width\":%d,"
            "\"svga_enabled\":%s,\"virtual_height\":%d,\"virtual_width\":%d}\n",
            (int)meta->svga_bpp, (int)meta->cols, (int)doc->common.cursor_col,
            (int)doc->common.cursor_row, (int)doc->scanline_end, (int)doc->scanline_start,
            doc->scanline_visible ? "true" : "false", meta->graphical_mode ? "true" : "false",
            (unsigned long long)meta->image_bytes, meta->image_sha256,
            (unsigned long long)meta->instruction_count,
            (unsigned long long)meta->instruction_limit, (unsigned long long)payload_bytes,
            (int)meta->rows, (int)meta->screen_height, (int)meta->screen_width,
            meta->svga_enabled ? "true" : "false", (int)meta->virtual_height,
            (int)meta->virtual_width);

    /* The payload: rows*cols records of chr, flags, bg, fg -- row-major. */
    for (int32_t row = 0; row < meta->rows; row++) {
        for (int32_t col = 0; col < meta->cols; col++) {
            int32_t cell = row * meta->cols + col;
            uint8_t chr = 0;
            uint8_t flags = 0;
            uint32_t bg = 0;
            uint32_t fg = 0;
            if (have_grid) {
                chr = doc->chr[cell];
                flags = doc->flags[cell];
                bg = doc->bg[cell];
                fg = doc->fg[cell];
            }
            fputc(chr, out);
            fputc(flags, out);
            put_u32_le(out, bg);
            put_u32_le(out, fg);
        }
    }

    bool ok = fflush(out) == 0 && ferror(out) == 0;
    if (fclose(out) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "v86c: vga: writing %s failed\n", tmp_path);
        remove(tmp_path);
        free(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "v86c: vga: cannot rename %s to %s\n", tmp_path, path);
        remove(tmp_path);
        free(tmp_path);
        return false;
    }
    free(tmp_path);
    return true;
}

/* ------------------------------------------------------------------ *
 * the per-adapter flag constants -- vga.js:848-849 / 926-932
 *
 * vga.js builds put_char's `flags` as
 *
 *     (blinking ? this.screen.FLAG_BLINKING : 0) | (font_page_b ? this.screen.FLAG_FONT_PAGE_B : 0)
 *
 * i.e. it reads the constants OFF THE ADAPTER.  screen.js:30-34 defines them on the canvas
 * adapter and nowhere else, so on a dummy/ansi screen the reads are `undefined` and the
 * expression collapses to 0 -- a reference behaviour, reproduced rather than fixed (R9 of
 * the plan: it is easy to "fix" silently and it is load-bearing for a byte-exact flags
 * byte).  The two functions below are the seam's expression of that: 0 for the headless and
 * ANSI adapters, the constants for the document one (which stands in for the canvas
 * adapter, and whose reference-leg recorder defines them too -- oracle/run-vga.mjs).
 *
 * U1's text path is the reader (the flush-to-adapter site); U0 only defines it, so that the
 * question "does this adapter carry the flag constants?" has ONE answer in the code instead
 * of a per-unit recollection.
 * ------------------------------------------------------------------ */
int32_t vga_screen_flag_blinking(vga_screen_t *screen)
{
    return screen == &document_screen ? VGA_FLAG_BLINKING : 0;
}

int32_t vga_screen_flag_font_page_b(vga_screen_t *screen)
{
    return screen == &document_screen ? VGA_FLAG_FONT_PAGE_B : 0;
}
