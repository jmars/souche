/* tests/reviewfix-pmtimer.asm -- the AUDIT-1R SF2 CONTROL image (reviewfix-1, G1).
 *
 * Same multiboot recipe as tests/reboot2x.asm (as --32 + ld -T tests/test.ld +
 * objcopy -O binary).  This image pins the guest-visible truth about the PM timer
 * across a reboot -- and it is a CONTROL, not an asymmetry: it must produce the SAME
 * verdict on the pre-fix and the post-fix binary, because the premise "the assert is
 * guest-reachable after a reboot" is FALSE in this port: the ACPI device is constructed
 * only when cpu->acpi_enabled (machine.c:82), a flag with NO writer (upstream sets it
 * from settings.acpi, cpu.js:1014; there is no CLI flag), so 0xB008 is never
 * registered and the guest's read answers D-F1's unclaimed 0xFFFFFFFF on BOTH boots.
 * The unit-level asymmetry (abort before / graceful arm after, with the device
 * constructed) lives in results/reviewfix1-pmtimer-probe.{c,sh}.
 *
 * BOOT 1: reads 0xB008 (witness), writes the marker, reboots through the PCI dance.
 * BOOT 2 (entered because the host re-established the multiboot entry; the counter was
 * reset, ACPI state -- if it existed -- persists): reads 0xB008 EARLY (the read that
 * would hit the rewound-clock assert if the device existed), reads the witnesses back,
 * verdict GOLD, halt.  EBX carries the verdict; --fixture prints it (line 6).
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

	/* boot 1 (marker word 0) or boot 2 (marker word set) */
	mov	eax, [0x101000]
	test	eax, eax
	jnz	boot2

boot1:
	/* read the PM timer the way a PM-aware guest would */
	mov	dx, 0xB008
	in	eax, dx
	mov	[0x101004], eax		/* witness: boot 1's answer (0xFFFFFFFF) */

	mov	dword ptr [0x101000], 0x31415926	/* THE MARKER (memory survives) */

	/* the PCI dance (pci.js:122-131): 0x02 to arm pci_addr[1], then 0x06.
	 * 0xCF9 is above the 8-bit OUT's one-byte port field -- the DX form is
	 * mandatory here (`out 0xCF9, al` would assemble to port 0xF9, an
	 * unregistered port). */
	mov	dx, 0xCF9
	mov	al, 0x02
	out	dx, al
	mov	al, 0x06
	out	dx, al		/* machine_reboot() */
2:	jmp	2b			/* never reached on the modelled path */

boot2:
	/* the read that would carry the rewound-clock interaction IF the device existed */
	mov	dx, 0xB008
	in	eax, dx
	mov	[0x101008], eax		/* witness: boot 2's answer (0xFFFFFFFF) */

	mov	ebx, 0x600D600D
	cmp	dword ptr [0x101000], 0x31415926
	je	1f
	mov	ebx, 0xFACD0001		/* memory did not survive */
1:	cmp	dword ptr [0x101004], 0xFFFFFFFF
	je	1f
	mov	ebx, 0xFACD0002		/* boot 1's read was not the unclaimed answer */
1:	cmp	dword ptr [0x101008], 0xFFFFFFFF
	je	1f
	mov	ebx, 0xFACD0003		/* boot 2's read was not the unclaimed answer */
1:	mov	eax, [0x101004]
	mov	[0x10100C], ebx
	hlt
