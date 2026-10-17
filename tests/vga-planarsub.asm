/* tests/vga-planarsub.asm -- VGA-U3's ROW-SUBSTITUTION image (HANDOFF=v86c-vga).
 *
 * Reaches the planar row-substitution arm (crtc_mode & 3 != 3) -- the arm the standard modes
 * 0x12/0x0D/0x04 do NOT exercise (they all use crtc_mode & 3 = 3) -- and proves it end to end.
 * crtc_mode 0x40 keeps byte mode (addr shift 3) but clears bits 1-0, so the substitution arm
 * runs.  With offset 2 the virtual width is 32, so a plane address with bit 13/14 set maps to a
 * DIFFERENT screen row: window addr 0x2000 (bit 13) is row 1 -> pixel 32, addr 0x4000 (bit 14)
 * is row 2 -> pixel 64.  Three columns, one per row, then read back through read mode 0 and
 * halt.  This is the differential proof of the signed `>>` vs JS zero-fill `>>>` in the
 * substitution arm: because the operands (pixel_addr/row/col) are never negative there, the two
 * are identical -- a divergence would show up here as a document diff.
 *
 *   row 0 (addr 0x0000): 0xFF/0x0F -> p0=FF p1=0F p2=FF p3=0F -> pixels 0-3 = 5, 4-7 = 15
 *   row 1 (addr 0x2000): 0xFF/0x0F -> pixels 32-35 = 5, 36-39 = 15
 *   row 2 (addr 0x4000): 0x55/0xAA -> p0=55 p1=AA p2=55 p3=AA -> pixels 64-67 = 10, 68-71 = 5
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

/* ---- sequencer + misc ---- */
	SEQ	0x00, 0x01
	mov	dx, 0x3C2
	mov	al, 0x63
	out	dx, al
	SEQ	0x00, 0x03
	SEQ	0x01, 0x01
	SEQ	0x02, 0x0F
	SEQ	0x03, 0x00
	SEQ	0x04, 0x02		/* CHAIN 2 (odd/even) */

/* ---- the CRTC: byte mode (bit 6) + bits 1-0 = 0 -> substitution arm ---- */
	CRTC	0x11, 0x00
	CRTC	0x01, 0x03		/* hdisp 3 -> 4 chars -> 32 px */
	CRTC	0x02, 0x04
	CRTC	0x09, 0x00		/* max scan line 0 */
	CRTC	0x12, 0x3F		/* vdisp 63 -> 64 scans */
	CRTC	0x15, 0x40
	CRTC	0x13, 0x02		/* offset 2 -> virtual_width 32 */
	CRTC	0x17, 0x40		/* byte mode, substitution (crtc_mode & 3 = 0) */

/* ---- the graphics controller: planar shift 0x00, write mode 0 ---- */
	GC	0x00, 0x00
	GC	0x01, 0x00
	GC	0x02, 0x00
	GC	0x03, 0x00
	GC	0x04, 0x00
	GC	0x05, 0x00
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

/* ---- the DAC: the classic 16-colour palette ---- */
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

/* ---- row 0 / row 1 / row 2, each a chain-2 column ---- */
	mov	edi, 0xA0000
	mov	byte ptr [edi], 0xFF	/* row 0, planes 0,2 */
	mov	byte ptr [edi + 1], 0x0F	/* row 0, planes 1,3 */

	mov	edi, 0xA2000		/* bit 13 -> row 1 */
	mov	byte ptr [edi], 0xFF
	mov	byte ptr [edi + 1], 0x0F

	mov	edi, 0xA4000		/* bit 14 -> row 2 */
	mov	byte ptr [edi], 0x55
	mov	byte ptr [edi + 1], 0xAA

/* ---- in-guest read-back: read mode 0 plane 0 of each row's plane address ---- */
	GC	0x04, 0x00
	mov	edi, 0xA0000
	mov	al, byte ptr [edi]
	cmp	al, 0xFF		/* row 0 plane0 */
	jne	bad
	mov	edi, 0xA2000
	mov	al, byte ptr [edi]
	cmp	al, 0xFF		/* row 1 plane0 */
	jne	bad
	mov	edi, 0xA4000
	mov	al, byte ptr [edi]
	cmp	al, 0x55		/* row 2 plane0 */
	jne	bad

halt_loop:
	hlt
	jmp	halt_loop

bad:
	jmp	bad
