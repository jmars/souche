/* tests/test.asm -- the M0 reference test program.
 *
 * GNU as, intel syntax, 32-bit. Linked at 0x100000 by tests/test.ld and
 * flattened with `objcopy -O binary`. The first 32 bytes are a multiboot
 * header carrying the address table (flags bit 16), so that the reference
 * implementation's own multiboot loader loads the flattened image verbatim
 * to 0x100000 and enters it at 0x100020 (MEASURED: v86/src/cpu.js:1426-1520
 * gives file_start = offset - (header_addr - load_addr) = 0 for header at
 * offset 0 with header_addr == load_addr).
 *
 * Real, deterministic 386-level computation: a counted sum loop, an ALU
 * chain over every logical/arithmetic op, shifts and rotates, a 16/8-bit
 * operand-size and segment-override pass, an LCG table fill and a checksum
 * loop over it, and a push/pop pass. Every result is stored into the hashed
 * 4 KiB window [0x101000, 0x102000). No interrupts, no I/O ports, no paging.
 *
 * Geometry: code at 0x100000, data window and stack inside
 * [0x101000, 0x102000) with STACK_TOP = 0x102000 (the brief's geometry,
 * mirroring v86/tests/nasm's BSS base and STACK_TOP shifted by 0x1000, which
 * is the space the code itself occupies -- see README.md).
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
_start:
	cld
	mov	esp, 0x102000
	xor	eax, eax
	xor	ebx, ebx
	xor	ecx, ecx
	xor	edx, edx
	xor	esi, esi
	xor	edi, edi
	xor	ebp, ebp

/* ---- 1. counted loop: eax = sum(1..100) = 5050 = 0x13BA ---- */
	mov	ecx, 100
1:	add	eax, ecx
	dec	ecx
	jnz	1b
	mov	dword ptr [0x101000], eax

/* ---- 2. ALU chain: sub/add/and/or/xor/not/neg on ebx ---- */
	mov	ebx, 0xFFFF0000
	sub	ebx, 1			/* 0xFFFEFFFF */
	add	ebx, 0x00000002		/* 0xFFFE0001 */
	and	ebx, 0x0FF00FF0		/* 0x0FF00000 */
	or	ebx, 0x0000000F		/* 0x0FF0000F */
	xor	ebx, 0x00FF00FF		/* 0x0F0F00F0 */
	not	ebx			/* 0xF0F0FF0F */
	neg	ebx			/* 0x0F0F00F1 */
	mov	dword ptr [0x101008], ebx

/* ---- 3. shifts and rotates: ecx ---- */
	mov	ecx, 0x80000001
	shl	ecx, 1			/* 0x00000002, CF=1 */
	ror	ecx, 3			/* 0x40000000 */
	sar	ecx, 5			/* 0x02000000 */
	shr	ecx, 4			/* 0x00200000 */
	rol	ecx, 8			/* 0x20000000 */
	mov	dword ptr [0x101004], ecx

/* ---- 4. 16/8-bit operand sizes and a segment override: edi ---- */
	mov	word ptr [0x101010], 0x1234
	mov	ax, word ptr es:[0x101010]
	add	ax, 0x0FED		/* ax = 0x2221 */
	mov	byte ptr [0x101012], 0xA5
	mov	dl, byte ptr [0x101012]
	movzx	edi, dl			/* edi = 0x000000A5 */
	movsx	edi, ax			/* edi = 0x00002221 */
	and	edi, 0x000FFFFF
	lea	edi, [edi + 4 * edi + 8]	/* edi = 5 * 0x2221 + 8 */
	mov	dword ptr [0x10100C], edi

/* ---- 5. LCG table fill; esi is the accumulator ---- */
	mov	esi, 1
	xor	edi, edi
	mov	ecx, 16
2:	imul	esi, esi, 1103515245
	add	esi, 12345
	mov	dword ptr [edi * 4 + 0x101040], esi
	inc	edi
	cmp	edi, ecx
	jb	2b

/* ---- 6. checksum loop over the table just filled: edx ---- */
	xor	edx, edx
	xor	edi, edi
	mov	ecx, 16
3:	mov	eax, dword ptr [edi * 4 + 0x101040]
	add	edx, eax
	xor	edx, eax
	rol	edx, 1
	inc	edi
	cmp	edi, ecx
	jb	3b
	mov	dword ptr [0x101014], edx

/* ---- 7. push/pop and the stack-top snapshot: ebp ---- */
	push	0x10203040
	push	edx
	mov	dword ptr [0x101018], esp
	pop	ebp
	pop	eax
	mov	ebp, esp
	mov	dword ptr [0x10101C], ebp

/* ---- 8. final flags from a known op, then halt ---- */
	cmp	ebp, 0x102000
halt_loop:
	hlt
	jmp	halt_loop
