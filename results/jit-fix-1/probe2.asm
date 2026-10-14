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
 * JIT-FIX-1's PROBE: the high-byte register helpers, both directions.
 * Register values are chosen so the pre-fix garbage BITS LAND SOMEWHERE:
 * with 0x1111C0DE in EAX and 0x22223456 in EBX the OR of the corrupted high
 * half (0x2222) into the destination's (0x1111) changes the value, so a
 * dead mask cannot hide behind a coincidence (MEASURED: 0x9ABCDEF0 in EAX
 * with 0x12345678 in EBX DID hide it -- see results/jit-fix-1.md).
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

	/* --- the READ path: movzx r32, <high byte> (0F B6 /r, rm 4..7) --- */
	mov	eax, 0x9ABCDEF0		/* AH = 0xDE, bits 24..31 = 0x9A */
	movzx	esi, ah			/* must be 0x000000DE */
	mov	[0x101000], esi
	mov	ecx, 0x55663488		/* CH = 0x34 */
	movzx	edi, ch			/* must be 0x00000034 */
	mov	[0x101004], edi
	movzx	ebx, dh			/* EDX = 0xC0DE1234 -> DH = 0x12 */
	mov	[0x101008], ebx

	/* --- the WRITE path: <high byte> <- <byte reg> (88 /r) ---
	 * EAX = 0x1111C0DE, EBX = 0x22223456: pre-fix the read arm returns
	 * (EBX >> 8) unmasked = 0x00222234, and the write arm's dead 0xFF mask
	 * lets 0x22 OR into EAX's bits 16..31 -> 0x333334DE instead of 0x111134DE */
	mov	eax, 0x1111C0DE
	mov	ebx, 0x22223456		/* BH = 0x34, BL = 0x56 */
	mov	ah, bh			/* must leave EAX = 0x111134DE */
	mov	[0x10100C], eax
	mov	ecx, 0x4444A5B6
	mov	edx, 0x8888ABCD		/* DH = 0xAB */
	mov	ch, dh			/* must leave ECX = 0x4444ABB6 */
	mov	[0x101010], ecx
	mov	bl, ch			/* low-byte dest: only the low byte is used */
	mov	[0x101014], ebx
	mov	dl, ah			/* low-byte dest, high-byte source */
	mov	[0x101018], edx
	hlt
