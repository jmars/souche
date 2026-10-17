/* tests/vga-planar12.asm -- VGA-U3's planar differential image (HANDOFF=v86c-vga).
 *
 * Self-programs VGA mode 0x12 (640x480x16, chain-2 odd/even, planar shift 0x00) and draws a
 * row of EIGHT 8-pixel columns, each produced by a DIFFERENT write-mode/rotate/logical/
 * setreset/bitmask configuration, so the whole planar write pipeline is exercised on ONE
 * frame (the R1 discipline: a sequence-dependent divergence is what a homogeneous image
 * cannot see).  It then reads the plane bytes back through read mode 0 (plane_read 0-3),
 * read mode 1 (colour compare) and odd/even host read, and -- LAST -- turns on the
 * attribute_mode-0x80 colorset arm, which remaps the WHOLE redraw (it is a global redraw
 * attribute, so it is applied after the read-back and left ON for the final document).
 * With color_select 0x05, source colour c maps to DAC entry 0x50+c, so the gate pins the
 * colorset by asserting a source-5 pixel lands in DAC[0x55], NOT DAC[5].
 *
 * Column map (chain-2: even addr -> planes 0,2; odd addr -> planes 1,3; plane byte A ->
 * pixels [A*8, A*8+7], since mode 0x12's crtc_mode 0xE3 gives the addr-to-pixel shift 3 and
 * no row substitution).  SOURCE colours before the colorset remap:
 *
 *   A (addr 0x000, write mode 0, rotate 0):  0xFF/0x0F -> p0=FF p1=0F p2=FF p3=0F
 *        pixels 0-3 = colour 5, 4-7 = colour 15
 *   B (addr 0x002, write mode 2 expand):     0x0B/0x05 -> p0=FF p1=00 p2=00 p3=00
 *        pixels 16-23 = colour 1
 *   C (addr 0x004, write mode 3 setreset 0x0F): 0x0F -> p0=0F p2=0F p1=0 p3=0
 *        pixels 32-35 = 0, 36-39 = 5
 *   D (addr 0x006, write mode 0, rotate 1):  0x01 -> 0x80 -> p0=80 p2=80
 *        pixel 48 = 5, 49-55 = 0
 *   E (addr 0x008, write mode 0, logical XOR, latch 0xFF): 0x0F -> p0=F0 p1=FF p2=F0 p3=FF
 *        pixels 64-67 = 15, 68-71 = 10
 *   F (addr 0x00A, write mode 0, setreset 0x05 + enable 0x0F): 0x00 -> p0=FF p2=FF
 *        pixels 80-87 = 5
 *   G (addr 0x00C, write mode 0, bitmask 0x0F, latch 0xFF): 0x00 -> p0=F0 p2=F0 p1=FF p3=FF
 *        pixels 96-99 = 15, 100-103 = 10
 *   H (addr 0x010, write mode 1 latch copy): latch=0x55AA55AA -> p0=AA p2=AA
 *        pixels 128,130,132,134 = 5
 *
 * THE DAC is the classic 16-colour palette (0x2A -> 0xA8, 0x15 -> 0x57, 0x3F -> 0xFF through
 * the reference's 6-bit expand).  The SOURCE colours are {0,1,5,10,15}; after colorset with
 * color_select 0x05 they land in DAC[0x50..0x5F], so DAC[0x51]=blue, DAC[0x55]=green,
 * DAC[0x5A]=white, DAC[0x5F]=red make the remapped document distinctive.  dac_map is identity.
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

/* write al to the window at offset edi (0xA0000 + edi); the caller preserves edi */
.macro STORE off, val
	mov	edi, 0xA0000 + \off
	mov	byte ptr [edi], \val
.endm

_start:
	cld
	mov	esp, 0x102000

/* ---- mode 0x12: sequencer + misc ---- */
	SEQ	0x00, 0x01
	mov	dx, 0x3C2
	mov	al, 0xE3
	out	dx, al
	SEQ	0x00, 0x03
	SEQ	0x01, 0x01		/* clocking mode: 8 dots */
	SEQ	0x02, 0x0F		/* map mask: all planes */
	SEQ	0x03, 0x00		/* character map select */
	SEQ	0x04, 0x02		/* memory mode: CHAIN 2 (odd/even) */

/* ---- the CRTC: unlock, then the mode-0x12 table ---- */
	CRTC	0x11, 0x00
	CRTC	0x00, 0x5F
	CRTC	0x01, 0x4F		/* hdisp 79 -> 80 chars -> 640 px */
	CRTC	0x02, 0x50
	CRTC	0x03, 0x82
	CRTC	0x04, 0x54
	CRTC	0x05, 0x80
	CRTC	0x06, 0xBF
	CRTC	0x07, 0x1F		/* overflow: vdisp bit 8 */
	CRTC	0x08, 0x00
	CRTC	0x09, 0x40		/* max scan line 0 (no repeat) -> 480 rows */
	CRTC	0x0A, 0x00
	CRTC	0x0B, 0x00
	CRTC	0x0C, 0x00
	CRTC	0x0D, 0x00
	CRTC	0x0E, 0x00
	CRTC	0x0F, 0x00
	CRTC	0x10, 0xEA
	CRTC	0x11, 0x0C
	CRTC	0x12, 0xDF		/* vdisp 479 -> 480 scans */
	CRTC	0x13, 0x28		/* offset 40 -> virtual_width 640 */
	CRTC	0x14, 0x00		/* underline: word mode */
	CRTC	0x15, 0xE7
	CRTC	0x16, 0x04
	CRTC	0x17, 0xE3		/* byte mode (bit 6), no row substitution */

/* ---- the graphics controller: write mode 0, shift 0x00, memory space 0 ---- */
	GC	0x00, 0x00		/* set/reset */
	GC	0x01, 0x00		/* set/reset enable */
	GC	0x02, 0x00		/* colour compare */
	GC	0x03, 0x00		/* rotate 0, replace */
	GC	0x04, 0x00		/* read map 0 */
	GC	0x05, 0x00		/* shift 0x00, write mode 0 */
	GC	0x06, 0x05		/* misc: graphics + memory map A0000 (64K) */
	GC	0x07, 0x0F		/* colour don't care */
	GC	0x08, 0xFF		/* bit mask all */

/* ---- the attribute controller: identity palette, plane enable, graphical ---- */
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
	AC	0x12, 0x0F		/* colour plane enable: all 4 planes */
	AC	0x10, 0x01		/* attribute_mode: graphical, 4-bit */

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

/* ---- column A: write mode 0 identity, chain-2 ---- */
	STORE	0x000, 0xFF
	STORE	0x001, 0x0F

/* ---- column B: write mode 2 (expand) ---- */
	GC	0x05, 0x02
	STORE	0x002, 0x0B
	STORE	0x003, 0x05

/* ---- column C: write mode 3 (setreset 0x0F) ---- */
	GC	0x00, 0x0F
	GC	0x05, 0x03
	STORE	0x004, 0x0F

/* ---- column D: write mode 0, rotate 1 ---- */
	GC	0x00, 0x00		/* clear setreset */
	GC	0x03, 0x01		/* rotate count 1 */
	GC	0x05, 0x00		/* write mode 0 */
	STORE	0x006, 0x01

/* ---- column E: write mode 0, logical XOR, latch 0xFF ---- */
	GC	0x03, 0x00		/* rotate 0, replace */
	STORE	0x008, 0xFF		/* set all four planes at 0x008 */
	STORE	0x009, 0xFF
	mov	edi, 0xA0008		/* read -> latch = 0xFFFFFFFF */
	mov	al, byte ptr [edi]
	GC	0x03, 0x18		/* XOR */
	STORE	0x008, 0x0F

/* ---- column F: write mode 0, setreset 0x05 + enable 0x0F ---- */
	GC	0x03, 0x00		/* replace */
	GC	0x00, 0x05		/* setreset */
	GC	0x01, 0x0F		/* enable all */
	STORE	0x00A, 0x00		/* overridden by setreset -> p0/p2 = 0xFF */

/* ---- column G: write mode 0, bitmask 0x0F, latch 0xFF ---- */
	GC	0x00, 0x00
	GC	0x01, 0x00
	STORE	0x00C, 0xFF		/* set all planes, then latch */
	STORE	0x00D, 0xFF
	mov	edi, 0xA000C
	mov	al, byte ptr [edi]
	GC	0x08, 0x0F		/* bitmask: low nibble from data, high from latch */
	STORE	0x00C, 0x00

/* ---- column H: write mode 1 (latch copy) ---- */
	GC	0x08, 0xFF		/* restore full bitmask */
	STORE	0x00E, 0xAA		/* set a known latch */
	STORE	0x00F, 0x55
	mov	edi, 0xA000E
	mov	al, byte ptr [edi]	/* latch = 0x55AA55AA */
	GC	0x05, 0x01		/* write mode 1 */
	STORE	0x010, 0x00		/* copies latch bytes 0,2 = 0xAA into planes 0,2 */
	GC	0x05, 0x00		/* back to write mode 0 */

/* ---- in-guest read-back: read mode 0, planes 0-3 of column A ---- */
	GC	0x04, 0x00
	mov	edi, 0xA0000
	mov	al, byte ptr [edi]
	cmp	al, 0xFF		/* plane0 = 0xFF */
	jne	bad
	GC	0x04, 0x01
	mov	al, byte ptr [edi]
	cmp	al, 0x0F		/* plane1 = 0x0F */
	jne	bad
	GC	0x04, 0x02
	mov	al, byte ptr [edi]
	cmp	al, 0xFF		/* plane2 = 0xFF */
	jne	bad
	GC	0x04, 0x03
	mov	al, byte ptr [edi]
	cmp	al, 0x0F		/* plane3 = 0x0F */
	jne	bad

/* ---- read mode 0 plane 0 of columns B-H (the write-mode arms) ---- */
	GC	0x04, 0x00
	mov	edi, 0xA0002
	mov	al, byte ptr [edi]
	cmp	al, 0xFF		/* column B plane0 = 0xFF (expand) */
	jne	bad
	mov	edi, 0xA0004
	mov	al, byte ptr [edi]
	cmp	al, 0x0F		/* column C plane0 = 0x0F (setreset) */
	jne	bad
	mov	edi, 0xA0006
	mov	al, byte ptr [edi]
	cmp	al, 0x80		/* column D plane0 = 0x80 (rotate 1) */
	jne	bad
	mov	edi, 0xA0008
	mov	al, byte ptr [edi]
	cmp	al, 0xF0		/* column E plane0 = 0xF0 (XOR) */
	jne	bad
	mov	edi, 0xA000A
	mov	al, byte ptr [edi]
	cmp	al, 0xFF		/* column F plane0 = 0xFF (setreset+enable) */
	jne	bad
	mov	edi, 0xA000C
	mov	al, byte ptr [edi]
	cmp	al, 0xF0		/* column G plane0 = 0xF0 (bitmask) */
	jne	bad
	mov	edi, 0xA0010
	mov	al, byte ptr [edi]
	cmp	al, 0xAA		/* column H plane0 = 0xAA (latch copy) */
	jne	bad

/* ---- read mode 1 (colour compare) over column A: compare 0x0F, don't care 0xF ---- */
	GC	0x07, 0x0F
	GC	0x02, 0x0F
	GC	0x05, 0x08		/* read mode 1 */
	mov	edi, 0xA0000
	mov	al, byte ptr [edi]
	cmp	al, 0x0F		/* {FF,0F,FF,0F} vs 0x0F -> 0x0F */
	jne	bad

/* ---- odd/even host read: even addr -> plane 0, odd addr -> plane 1 ---- */
	GC	0x05, 0x10		/* odd/even host read (read mode 0) */
	mov	edi, 0xA0000
	mov	al, byte ptr [edi]
	cmp	al, 0xFF		/* even -> plane0 = 0xFF */
	jne	bad
	mov	edi, 0xA0001
	mov	al, byte ptr [edi]
	cmp	al, 0x0F		/* odd -> plane1 = 0x0F */
	jne	bad

/* ---- LAST: the colorset arm (attribute_mode 0x80 + color_select 0x05).  Global: it remaps
 * the WHOLE redraw, so source colour c now lands in DAC[0x50+c].  The four DAC entries the
 * columns reach are made distinctive so the document proves the remap. ---- */
	AC	0x10, 0x81		/* colorset on */
	AC	0x14, 0x05		/* color_select = 5 -> colorset 0x50 */
	DAC	0x51, 0x00, 0x00, 0x3F	/* blue    <- source colour 1  */
	DAC	0x55, 0x00, 0x3F, 0x00	/* green   <- source colour 5  */
	DAC	0x5A, 0x3F, 0x3F, 0x3F	/* white   <- source colour 10 */
	DAC	0x5F, 0x3F, 0x00, 0x00	/* red     <- source colour 15 */

halt_loop:
	hlt
	jmp	halt_loop

bad:
	jmp	bad
