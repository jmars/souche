/* tests/vga-lfb.asm -- VGA-U4's differential image (HANDOFF=v86c-vga).
 *
 * The image that makes the D2 gate NON-TRIVIAL on the SVGA/LFB document: it self-programs a
 * Bochs VBE mode (320x200x32) through the DISPI interface (0x1CE/0x1CF, which upstream
 * registers as 16-BIT ports -- vga.js:360-364 -- so every access here is `out dx, ax` /
 * `in ax, dx`), enables the linear framebuffer at 0xE0000000, paints a base colour across
 * the whole 64,000-pixel screen, overwrites five named pixels with distinct colours, then
 * exercises BANK SWITCHING through the 0xA0000 window (bank 1) before reading everything
 * back in-guest and halting only on a match.
 *
 * THE MACHINE IS TESTS/TEST.ASM'S: a multiboot header at offset 0 (header/load 0x100000,
 * entry 0x100020), flattened with `objcopy -O binary` by tests/test.ld.  No interrupts, no
 * paging, no FPU.
 *
 * THE 32bpp LFB BYTE ORDER is BGRX (blue at byte 0), the reference's own: the fill reads
 * the LFB dword and shuffles it to RGBA (`dword<<16 | dword>>16&0xFF | dword&0xFF00 |
 * 0xFF000000`), so a dest-RED pixel is the LFB dword 0x00FF0000 (R in byte 2), NOT 0x000000FF.
 *
 * The in-guest read-back HALTS only on a match: the version, the width, the five named
 * pixels, and the bank-switched byte.  A mismatch spins forever, so a leg that stores,
 * banks, or decodes wrong fails the gate's own "guest must halt" precondition.
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

/* dispi(idx, val): the 16-bit index/data pair, 0x1CE then 0x1CF.  The reference registers
 * these as write16-only (vga.js:360-364), so the 16-bit `out dx, ax` is what programs them. */
.macro DISPI idx, val
	mov	dx, 0x1CE
	mov	ax, \idx
	out	dx, ax
	mov	dx, 0x1CF
	mov	ax, \val
	out	dx, ax
.endm

/* dispi_read(idx, dst): write the index, then `in ax, dx` the data register (16-bit read). */
.macro DISPI_READ idx, dst
	mov	dx, 0x1CE
	mov	ax, \idx
	out	dx, ax
	mov	dx, 0x1CF
	in	ax, dx
	mov	\dst, ax
.endm

_start:
	cld
	mov	esp, 0x102000

/* ---- the dispi interface: the constructor's version, then 320x200x32, enable | LFB ---- */
	DISPI_READ 0, ax		/* dispi index 0: svga_version */
	cmp	ax, 0xB0C5
	jne	bad

	DISPI	1, 320			/* width */
	DISPI	2, 200			/* height */
	DISPI	3, 32			/* bpp */
	DISPI	4, 0x41			/* enable | LFB (bit 6); bit 7 clear -> clear the LFB */

	DISPI_READ 1, ax		/* read the width back */
	cmp	ax, 320
	jne	bad
	DISPI_READ 6, ax		/* the virtual width (screen_width once enabled) */
	cmp	ax, 320
	jne	bad

/* ---- the base fill: a mid-gray 32bpp dword over all 64,000 pixels ---- */
	mov	edi, 0xE0000000
	mov	ecx, 320 * 200
	mov	eax, 0x00404040	/* B=0x40 G=0x40 R=0x40 (gray) */
1:	mov	[edi], eax
	add	edi, 4
	dec	ecx
	jnz	1b

/* ---- the named pixels (32bpp BGRX dwords), the row-0/199 and col-0/319 edges ---- */
	mov	dword ptr [0xE0000000 + 4 * (0 * 320 + 0)],   0x00FF0000	/* (  0,  0) red    */
	mov	dword ptr [0xE0000000 + 4 * (0 * 320 + 319)], 0x0000FF00	/* (319,  0) green  */
	mov	dword ptr [0xE0000000 + 4 * (1 * 320 + 0)],   0x000000FF	/* (  0,  1) blue   */
	mov	dword ptr [0xE0000000 + 4 * (100 * 320 + 160)], 0x00FFFFFF	/* (160,100) white  */
	mov	dword ptr [0xE0000000 + 4 * (199 * 320 + 319)], 0x00FFFF00	/* (319,199) yellow */

/* ---- bank switching: bank 1 through the 0xA0000 window, verified via the LFB ---- */
	DISPI	5, 1			/* bank 1 -> LFB byte offset 0x10000 */
	mov	byte ptr [0xA0000], 0x5A
	mov	al, [0xE0000000 + 0x10000]
	cmp	al, 0x5A
	jne	bad
	mov	al, [0xA0000]		/* the banked read-back */
	cmp	al, 0x5A
	jne	bad
	mov	al, [0xE0000000]	/* bank 0's byte 0 (pixel 0's blue) is untouched */
	cmp	al, 0x5A
	je	bad
	DISPI	5, 0			/* bank 0 again */

/* ---- in-guest read-back of the five named pixels, through the LFB ---- */
	mov	eax, [0xE0000000 + 4 * (0 * 320 + 0)]
	cmp	eax, 0x00FF0000
	jne	bad
	mov	eax, [0xE0000000 + 4 * (0 * 320 + 319)]
	cmp	eax, 0x0000FF00
	jne	bad
	mov	eax, [0xE0000000 + 4 * (1 * 320 + 0)]
	cmp	eax, 0x000000FF
	jne	bad
	mov	eax, [0xE0000000 + 4 * (100 * 320 + 160)]
	cmp	eax, 0x00FFFFFF
	jne	bad
	mov	eax, [0xE0000000 + 4 * (199 * 320 + 319)]
	cmp	eax, 0x00FFFF00
	jne	bad

halt_loop:
	hlt
	jmp	halt_loop

bad:					/* a wrong store/bank/decode: never halt */
	jmp	bad
