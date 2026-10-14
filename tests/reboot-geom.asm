/* tests/reboot-geom.asm -- the SF2 ASYMMETRY image (reboot-fix-1, G2).
 *
 * Same multiboot recipe as tests/reboot2x.asm (as --32 + ld -T tests/test.ld +
 * objcopy -O binary), same window layout and verdict codes, ONE trigger variant
 * (PS/2, trigid 2).  Built with a --defsym PADS=n pad knob:
 *
 *     make tests/reboot-geom.img   (bakes the measured PADS)
 *
 * WHAT IT IS FOR: cycle_internal's progress DBG_ASSERT (src/cpu/cpu.c) is
 * guest-reachable across a reboot when the TRIGGERING batch's instruction count
 * equals the cumulative count at that cycle's entry -- then the post-unwind
 * counter (the reset zeroed it; the unwind re-adds only the trigger batch's own
 * count) equals `initial_instruction_counter` with uop_pending == 0, and with
 * DBG_ASSERT live (no -DNDEBUG, by design) the process ABORTS instead of
 * rebooting.  This image bakes that geometry, MEASURED (results/reboot-fix-1.md):
 *
 *   the preamble's cycle boundaries are its I/O instructions -- every `out`
 *   carries UOP_DESC_FLUSH (gen table, E6/EE iflags 0x01), so each ends a
 *   reference batch: the cycles are 0→10, 10→12, 12→14, 14→16, 16→17, and the
 *   trigger cycle enters at counter 17.  The trigger batch is [the marker C7
 *   store, the jmp -- neither ends a cycle: MEASURED] + PADS nops + [mov al,
 *   out 0x64,al] = PADS + 4 instructions.  PADS = 13 makes it 17 = 17 exactly:
 *   the assert fires, in all three execution configs (MEASURED, pre-fix).
 *
 * PRE-FIX: the process dies on DBG_ASSERT at cpu.c (rc 134, all three configs).
 * POST-FIX (the assert gains `|| cpu->reboot_requested`): the reboot proceeds,
 * boot 2 reads the witnesses back, verdict GOLD.
 *
 * FIXED WINDOW WORDS: identical to tests/reboot2x.asm's table (0x101000..), so
 * tests/reboot_window_model.py 2 applies unchanged.  Registers at the final hlt:
 * EAX = the marker as read back, EBX = the verdict.
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

	/* boot 1 (count word 0) or boot 2 (count word 1) */
	mov	eax, [0x101800]
	test	eax, eax
	jnz	boot2

/* ------------------------------------------------------------------ *
 * boot 1: 19 instructions, then the trigger batch
 * ------------------------------------------------------------------ */
boot1:
	mov	dword ptr [0x101800], 1		/* 6 */
	mov	dword ptr [0x101020], 2		/* 7: trigid 2 (ps2.js:834-837) */
	mov	ecx, 0xDEADBEEF			/* 8: the CPU-reset witness */

	mov	al, 0x60			/* 9  */
	out	0x64, al			/* 10: select the PS/2 command register */
	mov	al, 0x65			/* 11 */
	out	0x60, al			/* 12: command register := 0x65 (non-default) */

	mov	al, 0x34			/* 13 */
	out	0x43, al			/* 14: PIT ch0 mode 2, both bytes */
	mov	al, 0xFF			/* 15 */
	out	0x40, al			/* 16 */
	out	0x40, al			/* 17 */

	mov	dword ptr [0x101000], 0x31415926 /* 18: THE MARKER */

	jmp	.trigger			/* 19: BLOCK_END+FLUSH -- the cycle ends HERE.
						 * The next cycle enters at counter 19. */

	/* THE TRIGGER BATCH: PADS + 2 instructions, no cycle-ender inside. */
.trigger:
	.rept	PADS
	nop
	.endr
	mov	al, 0xFE			/* the PS/2 CPU-reboot command */
	out	0x64, al			/* ps2.js:834-837: machine_reboot() */
1:	jmp	1b				/* never reached on the modelled path */

/* ------------------------------------------------------------------ *
 * boot 2: read the witnesses back, then halt (as tests/reboot2x.asm)
 * ------------------------------------------------------------------ */
boot2:
	mov	dword ptr [0x101800], 2

	mov	al, 0xFF
	out	0x21, al
	out	0xA1, al

	mov	[0x10100C], ecx

	mov	eax, [0x101000]
	mov	[0x101004], eax
	mov	eax, [0x101020]
	mov	[0x101024], eax

	mov	ax, cs
	movzx	eax, ax
	mov	[0x101018], eax
	mov	eax, cr0
	mov	[0x10101C], eax

	mov	al, 0x20
	out	0x64, al
	in	al, 0x60
	movzx	eax, al
	mov	[0x101010], eax

	mov	al, 0x00
	out	0x43, al
	in	al, 0x40
	mov	[0x101014], al
	in	al, 0x40
	mov	[0x101015], al

	mov	ebx, 0x600D600D
	cmp	dword ptr [0x101004], 0x31415926
	je	1f
	mov	ebx, 0xFACD0001			/* memory did not survive */
1:	cmp	dword ptr [0x10100C], 0
	je	1f
	mov	ebx, 0xFACD0002			/* the CPU was not reset */
1:	cmp	dword ptr [0x101010], 0x05
	je	1f
	mov	ebx, 0xFACD0003			/* the PS/2 was not reset */
1:	cmp	word ptr [0x101014], 0
	je	1f
	mov	ebx, 0xFACD0004			/* the PIT was not reset */
1:	cmp	dword ptr [0x101018], 0x08
	je	1f
	mov	ebx, 0xFACD0005			/* not the multiboot entry state */
1:	cmp	dword ptr [0x10101C], 1
	je	1f
	mov	ebx, 0xFACD0006			/* not protected mode at entry */
1:
	mov	eax, [0x101004]
	mov	[0x101008], ebx
	hlt
