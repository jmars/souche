/* tests/reviewfix-rtc.asm -- the AUDIT-1R SF1 ASYMMETRY image (reviewfix-1, G1).
 *
 * Same multiboot recipe as tests/reboot2x.asm (as --32 + ld -T tests/test.ld +
 * objcopy -O binary).  SIX port writes:
 *
 *     index 0x01 (seconds-alarm) := 0x1A   -- low nibble 10, a value the real
 *                                           hardware never stores but a guest can
 *     index 0x0B (status B)      := 0x20   -- alarm-interrupt enable, bit 2 CLEAR
 *                                           (=> BCD mode => decode_time runs
 *                                           bcd_unpack on the alarm bytes)
 *
 * The status-B write's `cmos_b & 0x20` arm (rtc.c, rtc_cmos_port_write) calls
 * decode_time(cmos_data[1]) -> bcd_unpack(0x1A) -> low = 10.
 *
 * PRE-FIX: DBG_ASSERT(low < 10) -- SIGABRT, rc 134, before the verdict.
 * POST-FIX (asserts dropped, matching the pinned release where rtc.js:221-222 are
 * compiled out): the wrong-but-defined `low + 10*high` (= 20) stands in for the alarm
 * seconds, the write path completes, status C is read to lower the IRQ line, and the
 * machine runs on to the GOLD verdict -- the release's own behaviour at this site.
 *
 * Registers at the final hlt: EAX = the verdict echo, EBX = the verdict
 * (0x600D600D = GOLD; the --fixture leg prints it in EBX as line 6, the same
 * convention tests/gate-reboot-1.sh checks).
 */

	.intel_syntax noprefix

	.section .mbheader, "a", @progbits
	.align 4
	.long	0x1BADB002			/* magic    */
	.long	0x00010000			/* flags: address table */
	.long	-(0x1BADB002 + 0x00010000)	/* checksum */
	.long	0x00100000			/* header_addr  */
	.long	0x00100000			/* load_addr    */
	.long	0				/* load_end_addr: 0 = the whole file */
	.long	0x00101000			/* bss_end_addr */
	.long	0x00100020			/* entry_addr   */

	.section .text
	.code32
	.globl _start
_start:
	cld
	mov	esp, 0x102000

	/* the SF1 trigger: alarm register, then status B with the alarm-enable bit */
	mov	al, 0x01
	out	0x70, al
	mov	al, 0x1A
	out	0x71, al
	mov	al, 0x0B
	out	0x70, al
	mov	al, 0x20
	out	0x71, al		/* PRE-FIX: SIGABRT inside rtc_cmos_port_write */

	/* ran on (the release's behaviour): read status C, lowering IRQ8's line */
	mov	al, 0x0C
	out	0x70, al
	in	al, 0x71
	movzx	eax, al
	mov	[0x101004], eax		/* witness: status C as read */

	mov	ebx, 0x600D600D
	mov	[0x101000], ebx
	mov	eax, [0x101000]
	hlt
