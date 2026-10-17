/* tests/vga-text.asm -- VGA-U1's differential image (HANDOFF=v86c-vga).
 *
 * The image that makes the D1 gate NON-TRIVIAL: it self-programs a small text mode and
 * draws cells whose background and foreground DIFFER (and whose attributes vary), so the
 * document's colour lane -- the lane U0's review found UNTESTED because a VGA-blind image
 * has every cell bg=fg=0 -- is actually exercised.  It also sets a NON-ZERO cursor and a
 * visible cursor scanline, so the cursor fields are exercised too.
 *
 * THE MACHINE IS TESTS/TEST.ASM'S: a multiboot header at offset 0 (header/load 0x100000,
 * entry 0x100020), flattened with `objcopy -O binary` by tests/test.ld.  No interrupts, no
 * paging, no FPU.  It programs, in order:
 *
 *   - the DAC (0x3C8/0x3C9): palette entries 1..7 become red/green/blue/white/cyan/
 *     magenta/yellow (the reference's 6-bit expand maps 0x3F to 0xFF);
 *   - the attribute controller's internal palette (0x3C0): dac_map[0..15] = identity, so
 *     an attribute byte's two nibbles index the DAC directly;
 *   - the CRTC (0x3D4/0x3D5): an 8x4 grid (hdisp=7, hblank=8, max_scan_line=3 -> 4
 *     scans/row, vdisp=15 -> 16 scans -> 4 rows, offset=4 -> offset*2=8=max_cols), start
 *     address 0, a cursor at address 3 (row 0, col 3) with scanlines 0..3;
 *   - the cells: 12 char/attr pairs written to the 0xA0000 window (memory space 0, the
 *     reference's default), each attr with bg nibble != fg nibble.
 *
 * It then reads two cells back (the window's vga_memory_read) and HALTS only if they match;
 * a mismatch spins forever, so a leg that stores or decodes wrong fails the gate's own
 * "guest must halt" precondition rather than passing with a wrong document.
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

/* ---- the DAC: 7 distinct colours ---- */
	DAC	1, 0x3F, 0x00, 0x00	/* red    */
	DAC	2, 0x00, 0x3F, 0x00	/* green  */
	DAC	3, 0x00, 0x00, 0x3F	/* blue   */
	DAC	4, 0x3F, 0x3F, 0x3F	/* white  */
	DAC	5, 0x00, 0x3F, 0x3F	/* cyan   */
	DAC	6, 0x3F, 0x00, 0x3F	/* magenta */
	DAC	7, 0x3F, 0x3F, 0x00	/* yellow */

/* ---- the attribute controller's internal palette: dac_map = identity ---- */
	xor	ebx, ebx
	mov	dx, 0x3C0
1:	mov	al, bl			/* index (bit 5 clear -> palette_source 0) */
	out	dx, al
	mov	al, bl			/* data: dac_map[i] = i */
	out	dx, al
	inc	bl
	cmp	bl, 16
	jb	1b

/* ---- the CRTC: an 8x4 grid, a visible cursor at (row 0, col 3) ---- */
	CRTC	0x01, 7			/* 8 columns */
	CRTC	0x02, 8			/* horizontal blank start */
	CRTC	0x09, 3			/* max scan line: 4 scans/row */
	CRTC	0x12, 15		/* 16 scans -> 4 rows */
	CRTC	0x15, 16		/* vertical blank start */
	CRTC	0x13, 4			/* offset: 8 words/row = max_cols */
	CRTC	0x0C, 0			/* start address hi */
	CRTC	0x0D, 0			/* start address lo */
	CRTC	0x0A, 0			/* cursor scanline start */
	CRTC	0x0B, 3			/* cursor scanline end (visible) */
	CRTC	0x0E, 0			/* cursor address hi */
	CRTC	0x0F, 3			/* cursor address lo -> row 0, col 3 */

/* ---- the cells: char at even, attr at odd; attr = bg nibble | fg nibble ---- */
	mov	word ptr [0xA0000 +  0], 0x1256	/* 'V' red-on-green   */
	mov	word ptr [0xA0000 +  2], 0x2147	/* 'G' green-on-red   */
	mov	word ptr [0xA0000 +  4], 0x3441	/* 'A' blue-on-white  */
	mov	word ptr [0xA0000 +  6], 0x432D	/* '-' white-on-blue  */
	mov	word ptr [0xA0000 +  8], 0x1555	/* 'U' red-on-cyan    */
	mov	word ptr [0xA0000 + 10], 0x2631	/* '1' green-on-magenta */
	mov	word ptr [0xA0000 + 12], 0x374F	/* 'O' blue-on-yellow */
	mov	word ptr [0xA0000 + 14], 0x714B	/* 'K' yellow-on-red  */
	mov	word ptr [0xA0010 +  0], 0x1274	/* 't' red-on-green   */
	mov	word ptr [0xA0010 +  2], 0x2465	/* 'e' green-on-white */
	mov	word ptr [0xA0010 +  4], 0x3173	/* 's' blue-on-red    */
	mov	word ptr [0xA0010 +  6], 0x4674	/* 't' white-on-magenta */

/* ---- one BLINKING cell, so the flags lane is exercised too: attribute_mode bit 3 set,
 *      then an attribute with bit 7 set -> FLAG_BLINKING (0x01) ---- */
	mov	dx, 0x3C0
	mov	al, 0x10		/* attribute controller index 0x10 */
	out	dx, al
	mov	al, 0x08		/* attribute_mode = blink enabled */
	out	dx, al
	mov	word ptr [0xA0020 +  0], 0x872A	/* '*' at (2,0): blink + fg=dac_map[7] */

/* ---- in-guest read-back: the window stores what it was written ---- */
	movzx	eax, byte ptr [0xA0000]
	cmp	eax, 0x56		/* 'V' */
	jne	bad
	movzx	eax, byte ptr [0xA0001]
	cmp	eax, 0x12
	jne	bad
	movzx	eax, byte ptr [0xA0010]
	cmp	eax, 0x74		/* 't' */
	jne	bad
	movzx	eax, byte ptr [0xA0011]
	cmp	eax, 0x12
	jne	bad

halt_loop:
	hlt
	jmp	halt_loop

bad:					/* a wrong store/decode: never halt */
	jmp	bad
