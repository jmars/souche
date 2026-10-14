/* tests/reboot2x.asm -- the TWO-BOOT reboot gate image (REBOOT-1, G1).
 *
 * THE M0 HARNESS'S OWN RECIPE (tests/test.asm's multiboot header + as --32 +
 * ld -m elf_i386 -T tests/test.ld + objcopy -O binary), run in the TRANSCRIPT mode.
 * One source, two trigger variants, selected with a defsym:
 *
 *     make reboot-images      (or the two rules in the Makefile)
 *       tests/reboot-pci.img  -- as --32 --defsym TRIGGER_PS2=0
 *       tests/reboot-ps2.img  -- as --32 --defsym TRIGGER_PS2=1
 *
 * BOOT 1: writes a MARKER into guest memory, leaves NON-DEFAULT device state in
 * the PS/2 controller (command register 0x65; the reset default is 0x05) and the
 * PIT (channel 0 counting down from 0xFFFF), leaves 0xDEADBEEF in ECX as the
 * CPU-reset witness, and then fires ONE reboot trigger:
 *   - TRIGGER_PS2=0: the PCI dance (pci.js:122-131): 0x02 to arm pci_addr[1],
 *     then 0x06 -- (addr[1]&6)==2 && (out&6)==6 -> machine_reboot().
 *   - TRIGGER_PS2=1: 0xFE to port 0x64 (ps2.js:834-837) -> machine_reboot().
 *
 * A THIRD VARIANT, REBOOT_LOOP=1, never halts: boot 2 resets the boot-count word
 * and fires the trigger again -- the driver's MAX_REBOOTS guard must NAME the
 * runaway (tests/gate-reboot-1.sh runs it for exactly that die()).
 *
 * BOOT 2 (entered because the host re-established the multiboot entry after the
 * reboot): reads the witnesses back, writes them into the hashed 4 KiB window
 * at fixed words, and halts.  The transcript (window sha256 + the final GPRs)
 * therefore proves BOTH halves of the gate: MEMORY SURVIVED (the marker word is
 * still there) and THE MACHINE REALLY RE-ENTERED (the entry state is the
 * multiboot entry, ECX is the reset zero, the PS/2 command register is the reset
 * 0x05, the PIT is the reset disabled).  The verdict word's FAIL codes name the
 * first check that failed; --fixture prints the same verdict in EBX.
 *
 * FIXED WINDOW WORDS (0x101000..):
 *   0x101000  the marker (0x31415926, written by boot 1)
 *   0x101004  the marker as boot 2 reads it back
 *   0x101008  the verdict: 0x600D600D = all checks passed, else 0xFACD00nn
 *   0x10100C  ECX at boot-2 entry (0x00000000 = the CPU was reset)
 *   0x101010  the PS/2 command register at boot 2 (0x05 = it was reset)
 *   0x101014  the PIT channel-0 latched count at boot 2 (0x0000 = it was reset)
 *   0x101018  CS at boot-2 entry (0x08 = the multiboot entry state)
 *   0x10101C  CR0 at boot-2 entry (1 = protected mode, paging off)
 *   0x101020  the trigger site id (1 = PCI 0xCF9, 2 = PS/2 0x64 0xFE)
 *   0x101800  the boot count (0 -> 1 -> 2)
 *
 * Registers at the final hlt: EAX = the marker as read back, EBX = the verdict.
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

	/* which boot is this?  The word at 0x101800 survives the reboot (memory is
	 * not reset), so 0 = boot 1 and 1 = boot 2. */
	mov	eax, [0x101800]
	test	eax, eax
	jnz	boot2

/* ------------------------------------------------------------------ *
 * boot 1: leave the witnesses, then fire the trigger
 * ------------------------------------------------------------------ */
boot1:
	mov	dword ptr [0x101000], 0x31415926	/* THE MARKER */
	mov	dword ptr [0x101800], 1
.if TRIGGER_PS2
	mov	dword ptr [0x101020], 2			/* ps2.js:834-837 */
.else
	mov	dword ptr [0x101020], 1			/* pci.js:126-131 */
.endif
	mov	ecx, 0xDEADBEEF				/* the CPU-reset witness */

	/* PS/2: the command register must NOT be the reset default (0x05 = 1|4).
	 * out 0x64,0x60 selects the command register; out 0x60 writes it. */
	mov	al, 0x60
	out	0x64, al
	mov	al, 0x65
	out	0x60, al

	/* PIT: channel 0, MODE 2 (rate generator), both bytes, 0xFFFF us.  After a
	 * PIT reset the counter is disabled and a latch reads 0x0000.  Mode 2, not
	 * mode 0, is load-bearing here: the reboot rewinds the virtual clock (the
	 * instruction counter restarts), the reference's own diff<0 arm in
	 * pit_did_rollover reads that as a rollover, and its mode-0 arm DISABLES a
	 * mode-0 counter before boot 2's latch -- a mode-0 witness reads 0x0000 with
	 * or without the reset (MEASURED).  A mode-2 counter stays enabled. */
	mov	al, 0x34
	out	0x43, al
	mov	al, 0xFF
	out	0x40, al
	out	0x40, al

.if TRIGGER_PS2
	mov	al, 0xFE
	out	0x64, al			/* ps2.js:834-837: CPU reboot via PS2 */
.else
	/* 0xCF9 is above the 8-bit OUT's one-byte port field -- the DX form is
	 * mandatory here (`out 0xCF9, al` would assemble to port 0xF9, an
	 * unclaimed port, and be discarded: MEASURED, the first gate run). */
	mov	dx, 0xCF9
	mov	al, 0x02
	out	dx, al				/* arm: pci_addr[1] = 0x02 (RST_CPU) */
	mov	al, 0x06
	out	dx, al				/* pci.js:126-127: CPU reboot via PCI */
.endif
1:	jmp	1b				/* never reached: the reboot stops this stream */

/* ------------------------------------------------------------------ *
 * boot 2: read the witnesses back, then halt
 * ------------------------------------------------------------------ */
boot2:
.ifdef REBOOT_LOOP
	mov	dword ptr [0x101800], 0			/* make the next entry boot 1 again */
	jmp	boot1					/* trigger again: a runaway reboot loop */
.endif
	mov	dword ptr [0x101800], 2

	/* Mask both PICs FIRST: the probes below raise lines (the PS/2 command-
	 * register read pushes a byte and raises IRQ1), and the reset PIC maps
	 * them to vectors this empty-IDT protected-mode image cannot deliver. */
	mov	al, 0xFF
	out	0x21, al
	out	0xA1, al

	/* the CPU-reset witness: reset zeroes the GPRs, boot 1 left 0xDEADBEEF */
	mov	[0x10100C], ecx

	/* memory survived: the marker is still there */
	mov	eax, [0x101000]
	mov	[0x101004], eax
	mov	eax, [0x101020]
	mov	[0x101024], eax				/* the trigger id, as boot 2 sees it */

	/* the entry state: the multiboot entry's cs selector and cr0 */
	mov	ax, cs
	movzx	eax, ax
	mov	[0x101018], eax
	mov	eax, cr0
	mov	[0x10101C], eax

	/* the PS/2 command register: 0x05 = the reset happened, 0x65 = it did not */
	mov	al, 0x20
	out	0x64, al
	in	al, 0x60
	movzx	eax, al
	mov	[0x101010], eax

	/* the PIT channel 0: latch the count and read both bytes */
	mov	al, 0x00
	out	0x43, al
	in	al, 0x40
	mov	[0x101014], al
	in	al, 0x40
	mov	[0x101015], al

	/* the verdict: GOLD unless a check fails; the FAIL code names the check */
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
