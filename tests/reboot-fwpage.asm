/* tests/reboot-fwpage.asm -- the SF1 ASYMMETRY image (reboot-fix-1, G1).
 *
 * Same multiboot recipe as tests/reboot2x.asm; PS/2 trigger (trigid 2).  It
 * points the SF1 corner of the compiled executor: a reboot trigger EXECUTED
 * FROM PAGE 0xFF000 -- the reset vector's own page (eip 0xFFFF0 after
 * reset_cpu, src/cpu/cpu_state.c:198; the port's firmware legally fills
 * 0xF0000-0xFFFFF, main.c BOOT_FIRMWARE_ADDRESS).  The compiled block's
 * page-cross check (jit_codegen.c l_checks step 8) compares the POST-RESET eip
 * against the trigger uop's eip: same page -> no fire; step 11 (eip+len != ip)
 * then exits JIT_EXIT_TAIL, whose arm continues the batch at the reset eip --
 * and jit_block_exit (jit_mem.c:542) has already consumed jit_abort_flag, so
 * nothing else stops it.  Boot-2 instructions then run INSIDE the stale
 * pre-reboot batch, before main.c's reboot_prepare_reentry clears the caches.
 *
 * HOW THE IMAGE MAKES THE CORNER VISIBLE: boot 1 OVERWRITES the reset vector
 * (0xFFFF0..0xFFFFF, plain RAM in this port) with a 16-bit stub that stores a
 * marker word into the hashed window at 0x101028 and spins, and copies a
 * trigger stub to 0xFF000 and jumps to it.  The guest firmware is NOT re-copied
 * on the multiboot re-entry (main.c multiboot_set_state rebuilds only the
 * handoff registers), so nothing else ever writes these bytes back.
 *
 * MEASURED VERDICT (results/reboot-fix-1.md): the corner this image aims at is
 * UNREACHABLE on the tree it shipped with.  The trigger port write is a
 * FLUSH + BLOCK_END uop (uop_gen.c's descriptors for E6/EE: flags 0x60,
 * iflags 0x01), so the decoded block ENDS at the trigger (UOP_STOP_BLOCK_END)
 * and the reference batch ends with it (step 7's flush -- MEASURED: the jit
 * unwind shows counter = the batch's own 2, pending = 0).  Step 11's TAIL exit
 * needs a MID-block trigger, and no reboot trigger is ever mid-block.  The run
 * is GOLD in all three configs both before and after the FIX-1 check; the
 * image's job is to TRIP if that invariant ever breaks -- a table regen or a
 * new trigger that stops ending its batch at the trigger would execute the
 * overwritten reset vector inside the stale batch and fail this gate as
 * verdict 0xFACD0007 + a broken window hash, the exact signature the review
 * predicted.
 *
 * FIXED WINDOW WORDS: tests/reboot2x.asm's table, PLUS
 *   0x101028  the stale-run marker (0 unless the stale batch ran boot-2 code).
 * tests/reboot_window_model.py 2 requires every unmodelled window word to be
 * zero, so the model alone already catches a set marker.  Registers at the
 * final hlt: EAX = the marker as read back, EBX = the verdict.
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

	mov	eax, [0x101800]
	test	eax, eax
	jnz	boot2

/* ------------------------------------------------------------------ *
 * boot 1: leave the witnesses, retarget the reset vector, fire from 0xFF000
 * ------------------------------------------------------------------ */
boot1:
	mov	dword ptr [0x101800], 1
	mov	dword ptr [0x101020], 2		/* ps2.js:834-837 */
	mov	dword ptr [0x101028], 0		/* ARM: the stale-run marker */
	mov	dword ptr [0x101000], 0x31415926 /* THE MARKER */
	mov	ecx, 0xDEADBEEF			/* the CPU-reset witness */

	/* the PS/2 and PIT witnesses, as tests/reboot2x.asm */
	mov	al, 0x60
	out	0x64, al
	mov	al, 0x65
	out	0x60, al

	mov	al, 0x34
	out	0x43, al
	mov	al, 0xFF
	out	0x40, al
	out	0x40, al

	/* THE RESET-VECTOR STUB, byte by byte (16-bit real mode at 0xFFFF0):
	 *   66 C7 05 28101000 EEC0FF5A -> mov dword [0x101028], 0xFFC05AEE
	 *   EB FE                      -> jmp $   (the stale batch ends at its
	 *                                           100001-iteration limit)
	 *   90 90 90                   -> pad */
	mov	byte ptr [0xFFFF0], 0x66
	mov	byte ptr [0xFFFF1], 0xC7
	mov	byte ptr [0xFFFF2], 0x05
	mov	byte ptr [0xFFFF3], 0x28
	mov	byte ptr [0xFFFF4], 0x10
	mov	byte ptr [0xFFFF5], 0x10
	mov	byte ptr [0xFFFF6], 0x00
	mov	byte ptr [0xFFFF7], 0xEE
	mov	byte ptr [0xFFFF8], 0x5A
	mov	byte ptr [0xFFFF9], 0xC0
	mov	byte ptr [0xFFFFA], 0xFF
	mov	byte ptr [0xFFFFB], 0xEB
	mov	byte ptr [0xFFFFC], 0xFE
	mov	byte ptr [0xFFFFD], 0x90
	mov	byte ptr [0xFFFFE], 0x90
	mov	byte ptr [0xFFFFF], 0x90

	/* THE TRIGGER STUB at 0xFF000 (32-bit protected, flat):
	 *   B0 FE        mov al, 0xFE
	 *   E6 64        out 0x64, al   <- THE TRIGGER, executed from page 0xFF000
	 *   BB 44332211  mov ebx, imm   <- keeps `out` mid-block (step 10 is not
	 *                                 the exit; the divergence test is)
	 *   EB FE        jmp $ */
	mov	byte ptr [0xFF000], 0xB0
	mov	byte ptr [0xFF001], 0xFE
	mov	byte ptr [0xFF002], 0xE6
	mov	byte ptr [0xFF003], 0x64
	mov	byte ptr [0xFF004], 0xBB
	mov	byte ptr [0xFF005], 0x44
	mov	byte ptr [0xFF006], 0x33
	mov	byte ptr [0xFF007], 0x22
	mov	byte ptr [0xFF008], 0x11
	mov	byte ptr [0xFF009], 0xEB
	mov	byte ptr [0xFF00A], 0xFE

	mov	eax, 0xFF000
	jmp	eax				/* execute the trigger from the
						 * reset vector's page */

/* ------------------------------------------------------------------ *
 * boot 2: read the witnesses back, then halt
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
1:	cmp	dword ptr [0x101028], 0
	je	1f
	mov	ebx, 0xFACD0007			/* boot-2 code ran in the stale
						 * pre-reboot batch (SF1) */
1:
	mov	eax, [0x101004]
	mov	[0x101008], ebx
	hlt
