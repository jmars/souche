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
	mov	ebx, 0xDEADBEEF
	mov	eax, 0x11223344
	mov	ah, bl		/* high-byte WRITE: must touch only bits 8..15 of eax */
	mov	[0x101000], eax
	mov	ecx, 0x55667788
	mov	edx, 0xC0DE1234
	mov	ch, dl		/* another high-byte write */
	mov	[0x101004], ecx
	mov	eax, 0x9ABCDEF0
	movzx	esi, ah		/* high-byte READ: must give 0x9A */
	mov	[0x101008], esi
	movzx	edi, ch		/* 0xXX from ecx after the write */
	mov	[0x10100C], edi
	hlt
