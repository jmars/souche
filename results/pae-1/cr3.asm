	.intel_syntax noprefix
	.section .mbheader, "a", @progbits
	.align 4
	.long	0x1BADB002
	.long	0x00010000
	.long	-(0x1BADB002 + 0x00010000)
	.long	0x00100000		/* header_addr  */
	.long	0x00100000		/* load_addr    */
	.long	0			/* load_end_addr */
	.long	0x00101000		/* bss_end_addr */
	.long	0x00100020		/* entry_addr   */
	.section .text
	.code32
	.globl _start
_start:
	mov	esp, 0x102000
	mov	eax, 0x1008		/* 4 KiB-aligned CR3 with PWT/PCD nibble set */
	mov	cr3, eax
	mov	ebx, 0x600D600D
	hlt
	.section .bss, "aw", @nobits
	.space 4096
