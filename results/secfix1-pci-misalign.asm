/* SECFIX-1's probe for the S1 class sweep's pci.c sites: a guest kills the pre-fix host
 * process with TWO port writes -- select any BAR dword of the IDE function (00:1e.0) with
 * an 8-bit config write aimed INTO the BAR range (pci_write8's dbg_assert), or do a
 * misaligned 16/32-bit config write (pci_write16/write32's dbg_asserts).  All three
 * asserts are transcribed from upstream's own dbg_asserts, which its release build
 * compiles out; the release then performs the write.  The post-fix port does the same.
 *
 * The image parks the config-address register's echo (pci->pci_addr) low bytes in ESI/EDI
 * via a config READ afterwards, then hlt, so the fixture shows the write landed and the
 * machine ran on.  Built with the Makefile's own AS/LD/OBJCOPY flags; driven as
 *   v86c <img> <limit> --fixture
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
_start:
	cld

	/* arm A (write8 into a BAR): select 00:1e.0 dword 0x20 (BAR4), then a BYTE write
	 * at it -- pre-fix: pci.c:290's assert aborts. */
	mov dx, 0xCF8
	mov eax, 0x8000F020
	out dx, eax
	mov dx, 0xCFC
	mov al, 0x5A
	out dx, al

	/* arm B (misaligned write32): a new address with offset 0x21 (inside the BAR range
	 * AND misaligned) -- pre-fix: pci.c:324's assert aborts. */
	mov dx, 0xCF8
	mov eax, 0x8000F021
	out dx, eax
	mov dx, 0xCFC
	mov eax, 0xA5A5A5A5
	out dx, eax

	/* arm C (misaligned write16): offset 0x31 (the ROM dword, misaligned) -- pre-fix:
	 * pci.c:300's assert aborts. */
	mov dx, 0xCF8
	mov eax, 0x8000F031
	out dx, eax
	mov dx, 0xCFC
	mov ax, 0x5AA5
	out dx, ax

	/* the machine ran on: read the dword back (aligned) and park it in ESI, then halt */
	mov dx, 0xCF8
	mov eax, 0x8000F020
	out dx, eax
	mov dx, 0xCFC
	in eax, dx
	mov esi, eax

	cli
1:	hlt
	jmp 1b
