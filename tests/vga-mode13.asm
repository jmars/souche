/* tests/vga-mode13.asm -- VGA-U2's differential image (HANDOFF=v86c-vga).
 *
 * The image that makes the D2 gate NON-TRIVIAL on the GRAPHICAL document: it self-programs
 * VGA mode 13h (320x200x256, chain-4) through direct register writes, loads a 16-entry DAC
 * palette, paints a 16-colour repeating ramp across the whole 64,000-pixel screen, then
 * overwrites several pixel-column/row EDGE cells with known values -- so a blank or
 * single-colour framebuffer, or a wrong pixel stride, cannot pass either the document
 * differential or the image's own in-guest read-back.
 *
 * THE MACHINE IS TESTS/TEST.ASM'S: a multiboot header at offset 0 (header/load 0x100000,
 * entry 0x100020), flattened with `objcopy -O binary` by tests/test.ld.  No interrupts, no
 * paging, no FPU.
 *
 * THE MODE-13h SEQUENCE is the standard one (osdev's VGA Hardware register dump), with the
 * one reference-load-bearing detail called out: the sequencer memory mode register (SR4)
 * MUST set bit 3 (Chain 4) -- the reference's write/read path checks
 * `sequencer_memory_mode & 0xC` in {0x8, 0xC} (vga.js:723-727), so 0x08 chains the window
 * into a linear 256 KiB plane space where byte address == pixel index.  The attribute mode
 * register (0x41) sets bit 0 (graphical) AND bit 6 (8-bit PEL width), which together with
 * underline 0x40 + crtc_mode 0xA3 make the address-to-pixel shift 0 (vga.js:1032-1048) -- the
 * replot then reads each pixel's plane byte at the same address the chain-4 write stored it.
 *
 *   DAC palette 0..15: 16 DISTINCT colours (the 6-bit components expand through the
 *   reference's own `b = v & 1; v = v<<2 | b<<1 | b`, so 0x3F -> 0xFF and 0x15 -> 0x55).
 *
 *   The pattern: byte[A] = A & 0xF for A in 0..63999 (a 16-colour ramp), then
 *     pixel (  0,  0) = 0x0E   pixel (319,  0) = 0x0F   (the row-0 column EDGE)
 *     pixel (  0,  1) = 0x0D   pixel (319,199) = 0x0C   (the last pixel)
 *     pixel (160,100) = 0x0B   (an interior cell, off the ramp)
 *   -- the column-0/column-319 and row-0/row-199 boundaries are all exercised.
 *
 * The in-guest read-back HALTS only on a match: read mode 0 (chain-4) reads four of those
 * edge pixels back, and read mode 1 (colour compare, don't-care 0xF, compare 0xF) proves the
 * planar compare path on a 4-pixel run written to 0xFF.  A mismatch spins forever, so a leg
 * that stores or decodes wrong fails the gate's own "guest must halt" precondition.
 */

	.intel_syntax noprefix

	.section .mbheader, "a", @progbits
	.align 4
	.long	0x1BADB002		/* magic    */
	.long	0x00010000		/* flags: address table */
	.long	-(0x1BADB002 + 0x00010000)	/* checksum */
	.long	0x00100000		/* header_addr  */
	.long	0x00100000		/* load_addr    */
	.long	0			/* load_end_addr: 0 = the whole file */
	.long	0x00101000		/* bss_end_addr */
	.long	0x00100020		/* entry_addr   */

	.section .text
	.code32
	.globl _start

/* seq(idx, val): the index/data pair as ONE 16-bit write -- the VGA idiom, which the
 * consecutive write16 shim splits into index=al, data=ah. */
.macro SEQ idx, val
	mov	dx, 0x3C4
	mov	ax, (\idx) | ((\val) << 8)
	out	dx, ax
.endm

/* gc(idx, val): ditto through 0x3CE/0x3CF */
.macro GC idx, val
	mov	dx, 0x3CE
	mov	ax, (\idx) | ((\val) << 8)
	out	dx, ax
.endm

/* crtc(reg, value): the index/data pair, 0x3D4/0x3D5 */
.macro CRTC reg, val
	mov	dx, 0x3D4
	mov	al, \reg
	out	dx, al
	mov	dx, 0x3D5
	mov	al, \val
	out	dx, al
.endm

/* dac(index, r, g, b): 0x3C8 then three 0x3C9 writes, 6-bit components */
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

_start:
	cld
	mov	esp, 0x102000

/* ---- the sequencer: async reset, misc output, then the mode-13h values ---- */
	SEQ	0x00, 0x01		/* synchronous reset on */
	mov	dx, 0x3C2
	mov	al, 0x63		/* misc output: dot clock etc. */
	out	dx, al
	SEQ	0x00, 0x03		/* reset off */
	SEQ	0x01, 0x01		/* clocking mode: 8 dots */
	SEQ	0x02, 0x0F		/* map mask: all planes */
	SEQ	0x03, 0x00		/* character map select */
	SEQ	0x04, 0x08		/* memory mode: CHAIN 4 (bit 3) */

/* ---- the CRTC: unlock, then the mode-13h table ---- */
	CRTC	0x11, 0x00		/* clear the write-protect bit */
	CRTC	0x00, 0x5F
	CRTC	0x01, 0x4F		/* horizontal display enable end: 79 -> 80 chars */
	CRTC	0x02, 0x50		/* horizontal blank start: 80 */
	CRTC	0x03, 0x82
	CRTC	0x04, 0x54
	CRTC	0x05, 0x80
	CRTC	0x06, 0xBF
	CRTC	0x07, 0x1F
	CRTC	0x08, 0x00		/* preset row scan */
	CRTC	0x09, 0x41		/* max scan line */
	CRTC	0x0A, 0x00		/* cursor start */
	CRTC	0x0B, 0x00		/* cursor end */
	CRTC	0x0C, 0x00		/* start address hi */
	CRTC	0x0D, 0x00		/* start address lo */
	CRTC	0x0E, 0x00		/* cursor location hi */
	CRTC	0x0F, 0x00		/* cursor location lo */
	CRTC	0x10, 0x9C
	CRTC	0x11, 0x8E
	CRTC	0x12, 0x8F		/* vertical display enable end: 143 -> 144 scans */
	CRTC	0x13, 0x28		/* offset: 40 */
	CRTC	0x14, 0x40		/* underline location: byte mode */
	CRTC	0x15, 0x96		/* vertical blank start: 150 */
	CRTC	0x16, 0xB9
	CRTC	0x17, 0xA3		/* mode control */

/* ---- the graphics controller ---- */
	GC	0x05, 0x40		/* mode: 256-colour shift */
	GC	0x06, 0x05		/* misc: graphics mode + memory map A0000 (64K) */

/* ---- the attribute controller: attribute_mode = graphics | 8-bit PEL ---- */
	mov	dx, 0x3C0
	mov	al, 0x10
	out	dx, al
	mov	al, 0x41		/* bit 0 = graphical, bit 6 = 8-bit colour */
	out	dx, al

/* ---- the DAC: 16 distinct colours ---- */
	DAC	0,  0x00, 0x00, 0x00	/* black       */
	DAC	1,  0x3F, 0x00, 0x00	/* red         */
	DAC	2,  0x00, 0x3F, 0x00	/* green       */
	DAC	3,  0x00, 0x00, 0x3F	/* blue        */
	DAC	4,  0x3F, 0x3F, 0x3F	/* white       */
	DAC	5,  0x00, 0x3F, 0x3F	/* cyan        */
	DAC	6,  0x3F, 0x00, 0x3F	/* magenta     */
	DAC	7,  0x3F, 0x3F, 0x00	/* yellow      */
	DAC	8,  0x1F, 0x00, 0x00	/* dark red    */
	DAC	9,  0x00, 0x1F, 0x00	/* dark green  */
	DAC	10, 0x00, 0x00, 0x1F	/* dark blue   */
	DAC	11, 0x15, 0x15, 0x15	/* dark gray   */
	DAC	12, 0x2A, 0x2A, 0x2A	/* light gray  */
	DAC	13, 0x3F, 0x1F, 0x00	/* orange      */
	DAC	14, 0x1F, 0x00, 0x1F	/* purple      */
	DAC	15, 0x00, 0x1F, 0x1F	/* teal        */

/* ---- the ramp: byte[A] = A & 0xF for A in 0..63999 (64,000 / 16 = 4,000 cycles) ---- */
	mov	edi, 0xA0000
	xor	eax, eax
1:	mov	byte ptr [edi], al
	inc	edi
	inc	al
	and	al, 0x0F
	cmp	edi, 0xA0000 + 64000
	jb	1b

/* ---- the edge cells: row-0/row-199 and column-0/column-319 boundaries ---- */
	mov	byte ptr [0xA0000 +   0], 0x0E	/* (  0,  0) */
	mov	byte ptr [0xA0000 + 319], 0x0F	/* (319,  0) */
	mov	byte ptr [0xA0000 + 320], 0x0D	/* (  0,  1) */
	mov	byte ptr [0xA0000 + 100*320 + 160], 0x0B	/* (160,100) */
	mov	byte ptr [0xA0000 + 199*320 + 319], 0x0C	/* (319,199) */

/* ---- in-guest read-back, read mode 0 (chain-4): the window stored each pixel ---- */
	movzx	eax, byte ptr [0xA0000 +   0]
	cmp	eax, 0x0E
	jne	bad
	movzx	eax, byte ptr [0xA0000 + 319]
	cmp	eax, 0x0F
	jne	bad
	movzx	eax, byte ptr [0xA0000 + 320]
	cmp	eax, 0x0D
	jne	bad
	movzx	eax, byte ptr [0xA0000 + 199*320 + 319]
	cmp	eax, 0x0C
	jne	bad

/* ---- read mode 1 (colour compare): a 4-pixel run written to 0xFF must read 0xFF ---- */
	mov	byte ptr [0xA0000 + 8], 0xFF
	mov	byte ptr [0xA0000 + 9], 0xFF
	mov	byte ptr [0xA0000 + 10], 0xFF
	mov	byte ptr [0xA0000 + 11], 0xFF
	GC	0x07, 0x0F		/* colour don't care: all four planes */
	GC	0x02, 0x0F		/* colour compare: 0xF */
	GC	0x05, 0x48		/* planar mode |= read mode 1 (0x40 | 0x08) */
	movzx	eax, byte ptr [0xA0000 + 8]
	cmp	eax, 0xFF
	jne	bad
	movzx	eax, byte ptr [0xA0000 + 12]	/* pixels 12..15 = 0x0C..0x0F, not 0xFF */
	cmp	eax, 0xFF
	je	bad

halt_loop:
	hlt
	jmp	halt_loop

bad:					/* a wrong store/decode: never halt */
	jmp	bad
