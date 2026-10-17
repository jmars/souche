/* tests/vga-planar4.asm -- VGA-U3's PACKED SHIFT image (HANDOFF=v86c-vga).
 *
 * Exercises shift mode 0x20 (the packed/interleaved CGA 4-colour shift, modes 4h/5h) end to
 * end.  The literal mode-0x04 table uses chain-2 (odd/even), which writes the SAME byte to
 * planes 0 and 2 and so cannot produce the full 2-bits-per-plane interleave that pins the
 * 0x20 arithmetic -- this image instead chains 4 (SR4 = 0x08) so four DISTINCT plane bytes
 * can be written, then sets planar_mode 0x20.  Both legs run the same bytes, so the document
 * is byte-identical; the packed shift unpacks byte0/byte2 into pixels 0-3 and byte1/byte3
 * into pixels 4-7 (2 bits per pixel, MSB-first):
 *
 *   byte0 = 0xE4, byte2 = 0x1B -> pixels 0,1,2,3 = 3, 6, 9, 12
 *   byte1 = 0x00, byte3 = 0x00 -> pixels 4..7 = 0
 *
 * The geometry is mode 0x04's (offset 40 -> virtual_width 640, underline 0x40 -> bytes per
 * line 320 -> virtual_height 410, vdisp 0x8F -> 200 scans), so the document is 640x410.  The
 * in-guest read-back (read mode 0 through chain-4) verifies the four plane bytes landed where
 * written before halting.
 */

	.intel_syntax noprefix

	.section .mbheader, "a", @progbits
	.align 4
	.long	0x1BADB002
	.long	0x00010000
	.long	-(0x1BADB002 + 0x00010000)
	.long	0x00100000
	.long	0x00100000
	.long	0
	.long	0x00101000
	.long	0x00100020

	.section .text
	.code32
	.globl _start

.macro SEQ idx, val
	mov	dx, 0x3C4
	mov	ax, (\idx) | ((\val) << 8)
	out	dx, ax
.endm

.macro GC idx, val
	mov	dx, 0x3CE
	mov	ax, (\idx) | ((\val) << 8)
	out	dx, ax
.endm

.macro CRTC reg, val
	mov	dx, 0x3D4
	mov	al, \reg
	out	dx, al
	mov	dx, 0x3D5
	mov	al, \val
	out	dx, al
.endm

.macro DAC idx, r, g, b
	mov	dx, 0x3C8
	mov	al, \idx
	out	dx, al
	mov	dx, 0x3C9
	mov	al, \r
	out	dx, al
	mov	al, \g
	out	dx, al
	mov	al, \b
	out	dx, al
.endm

.macro AC idx, val
	mov	dx, 0x3C0
	mov	al, \idx
	out	dx, al
	mov	al, \val
	out	dx, al
.endm

_start:
	cld
	mov	esp, 0x102000

/* ---- sequencer + misc (mode 0x04), but SR4 = chain 4 for distinct plane writes ---- */
	SEQ	0x00, 0x01
	mov	dx, 0x3C2
	mov	al, 0x63
	out	dx, al
	SEQ	0x00, 0x03
	SEQ	0x01, 0x09		/* clocking mode (mode 0x04) */
	SEQ	0x02, 0x0F
	SEQ	0x03, 0x00
	SEQ	0x04, 0x08		/* CHAIN 4 (one plane per byte) */

/* ---- the CRTC: mode 0x04 ---- */
	CRTC	0x11, 0x00
	CRTC	0x00, 0x5F
	CRTC	0x01, 0x4F
	CRTC	0x02, 0x50
	CRTC	0x03, 0x82
	CRTC	0x04, 0x54
	CRTC	0x05, 0x80
	CRTC	0x06, 0xBF
	CRTC	0x07, 0x1F
	CRTC	0x08, 0x00
	CRTC	0x09, 0x41
	CRTC	0x0A, 0x00
	CRTC	0x0B, 0x00
	CRTC	0x0C, 0x00
	CRTC	0x0D, 0x00
	CRTC	0x0E, 0x00
	CRTC	0x0F, 0x00
	CRTC	0x10, 0x9C
	CRTC	0x11, 0x8E
	CRTC	0x12, 0x8F		/* vdisp 143 -> 144 scans -> screen_height 200 */
	CRTC	0x13, 0x28		/* offset 40 */
	CRTC	0x14, 0x40		/* underline: byte mode -> bytes per line 320 */
	CRTC	0x15, 0x96
	CRTC	0x16, 0xB9
	CRTC	0x17, 0xA3		/* no row substitution */

/* ---- the graphics controller: packed shift 0x20, write mode 0 ---- */
	GC	0x00, 0x00
	GC	0x01, 0x00
	GC	0x02, 0x00
	GC	0x03, 0x00
	GC	0x04, 0x00
	GC	0x05, 0x20		/* packed shift mode */
	GC	0x06, 0x05
	GC	0x07, 0x0F
	GC	0x08, 0xFF

/* ---- the attribute controller: identity palette, graphical ---- */
	AC	0x00, 0x00
	AC	0x01, 0x01
	AC	0x02, 0x02
	AC	0x03, 0x03
	AC	0x04, 0x04
	AC	0x05, 0x05
	AC	0x06, 0x06
	AC	0x07, 0x07
	AC	0x08, 0x08
	AC	0x09, 0x09
	AC	0x0A, 0x0A
	AC	0x0B, 0x0B
	AC	0x0C, 0x0C
	AC	0x0D, 0x0D
	AC	0x0E, 0x0E
	AC	0x0F, 0x0F
	AC	0x12, 0x0F
	AC	0x10, 0x01

/* ---- the DAC: the classic 16-colour palette (pixels 0,3,6,9,12 are used) ---- */
	DAC	0,  0x00, 0x00, 0x00
	DAC	1,  0x00, 0x00, 0x2A
	DAC	2,  0x00, 0x2A, 0x00
	DAC	3,  0x00, 0x2A, 0x2A
	DAC	4,  0x2A, 0x00, 0x00
	DAC	5,  0x2A, 0x00, 0x2A
	DAC	6,  0x2A, 0x15, 0x00
	DAC	7,  0x2A, 0x2A, 0x2A
	DAC	8,  0x15, 0x15, 0x15
	DAC	9,  0x15, 0x15, 0x3F
	DAC	10, 0x15, 0x3F, 0x15
	DAC	11, 0x15, 0x3F, 0x3F
	DAC	12, 0x3F, 0x15, 0x15
	DAC	13, 0x3F, 0x15, 0x3F
	DAC	14, 0x3F, 0x3F, 0x15
	DAC	15, 0x3F, 0x3F, 0x3F

/* ---- chain-4: four DISTINCT plane bytes at plane addr 0 ---- */
	mov	edi, 0xA0000
	mov	byte ptr [edi], 0xE4	/* plane 0 */
	mov	byte ptr [edi + 1], 0x00	/* plane 1 */
	mov	byte ptr [edi + 2], 0x1B	/* plane 2 */
	mov	byte ptr [edi + 3], 0x00	/* plane 3 */

/* ---- in-guest read-back: read mode 0 through chain 4 reads each plane ---- */
	GC	0x04, 0x00		/* read map 0 (chain 4 overrides to addr & 3) */
	mov	al, byte ptr [edi]
	cmp	al, 0xE4		/* plane 0 */
	jne	bad
	mov	al, byte ptr [edi + 2]
	cmp	al, 0x1B		/* plane 2 */
	jne	bad
	mov	al, byte ptr [edi + 1]
	cmp	al, 0x00		/* plane 1 */
	jne	bad
	mov	al, byte ptr [edi + 3]
	cmp	al, 0x00		/* plane 3 */
	jne	bad

halt_loop:
	hlt
	jmp	halt_loop

bad:
	jmp	bad
