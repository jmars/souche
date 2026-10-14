/*
 * minibios.asm -- the boot firmware shim both machines run for the bzImage boot gate.
 *
 * WHY THIS FILE EXISTS: v86 cannot boot a Linux bzImage on its own.  `load_kernel`
 * (v86/src/kernel.js:42-179) writes the kernel's real-mode setup area, the protected-mode
 * kernel, the boot params and the command line, and returns an OPTION ROM
 * (`make_linux_boot_rom`, kernel.js:181-234) whose own header says "This rom will be
 * executed by seabios after its initialisation".  cpu.js:1024-1035 only pushes it onto
 * `this.option_roms`, from where SEABIOS fetches it through the fw_cfg port and executes it
 * (cpu.js:1127-1160).  So the reference's bzImage path needs a BIOS; this port has none
 * (seabios needs the VGA and the IDE controller this port deliberately has not ported, and
 * the M2-2 brief forbids starting them).
 *
 * THE RESOLUTION: BOTH machines run THIS binary -- the same assembled bytes -- as their
 * firmware, and each side's own `load_kernel` (the reference's JS on one side, its C port
 * src/hw/kernel.c on the other) writes the kernel image and the option ROM.  The host puts
 * the option ROM at 0xC0000 (which is where seabios copies it) and resets the CPU; this
 * firmware scans for it, checks its 0xAA55 signature and checksum, and far-calls its entry
 * -- which is what seabios does with it.  Everything after that point (the kernel's own
 * real-mode setup, the switch to protected mode, the whole operating system) is the kernel
 * running on the machine under test.  NO SEABIOS BEHAVIOUR IS CLAIMED: the INT services
 * below are this file's own, they are documented at each handler, and both machines get
 * exactly the same bytes.
 *
 * LAYOUT (a real PC BIOS's shape): 64 KiB at 0xF0000-0xFFFFF, executed with CS=0xF000 and
 * tied to the reset vector at 0xFFFF0 (offset 0xFFF0 here), so the LINK ADDRESS IS 0 and
 * every symbol in this file is a 16-bit segment offset.  The IVT (0x0000) and the BIOS data
 * area (0x0400) are set up at entry; then the option ROM is found and called.
 *
 * REGISTER DISCIPLINE: the only caller of these handlers is the kernel's own `intcall`
 * (arch/x86/boot/bioscall.S), which loads the full register set from its `biosregs`
 * argument before the INT, copies the whole post-handler register file into `oreg`, and
 * then restores DS, ES, GS, FS and FLAGS *from what the handler left*.  So: FS and GS
 * must be preserved (the kernel's real-mode video code keeps a segment in FS), DS and ES
 * are re-established by intcall itself, and every other register may be clobbered -- the
 * documented outputs are the only thing a caller reads back.
 *
 * WHAT THIS IS NOT: no POST, no disk, no keyboard, no scrolling, no EBDA tables, no VGA.
 * Each of those is named in the unit report (results/m2-2-boot.md) with what a guest sees.
 */

	.intel_syntax noprefix
	.code16

	.set	SEG,          0xF000	/* the segment this image is executed with */
	/* The machine's memory top.  DEFAULT 64 MiB -- the geometry both sides use for the
	 * M2-2 boot.  M2-3 builds a SECOND copy of this firmware at 80 MiB
	 * (`make minibios-mem`, `as --defsym MEM_TOP=0x5000000`) because an initrd is a
	 * thing the kernel must be told about: upstream's load_kernel puts it at the fixed
	 * 64 MiB boundary (kernel.js:48-50), so a machine that carries an initramfs must
	 * report more than 64 MiB in this map or the kernel refuses the initrd
	 * ("initrd extends beyond end of memory ... disabling initrd").  The firmware is
	 * still ONE source file and the two legs are handed the SAME assembled bytes. */
	.ifndef MEM_TOP
	.set	MEM_TOP,      0x4000000
	.endif

	/* BIOS data area */
	.set	BDA_EBDA,     0x40E
	.set	BDA_EQUIP,    0x410
	.set	BDA_MEMSIZE,  0x413
	.set	BDA_MODE,     0x449
	.set	BDA_COLS,     0x44A
	.set	BDA_CURSOR,   0x450
	.set	BDA_CURSHAPE, 0x460
	.set	BDA_TICKS,    0x46C
	.set	BDA_ROWS,     0x484
	.set	BDA_FONTSIZE, 0x485

	.set	VIDEO_SEG,    0xB800	/* the colour text buffer (the port's VGA window) */
	.set	UART_TX,      0x3F8
	.set	UART_LSR,     0x3FD

	.text

/* ================================================================== *
 * entry -- the reset vector at 0xFFF0 jumps here
 * ================================================================== */
bios_start:
	jmp	init

/* ------------------------------------------------------------------ *
 * serial diagnostics -- COM1, polled.  The bytes this prints are part of the
 * comparison: both machines must print the same ones.
 * ------------------------------------------------------------------ */
putc:					/* AL = byte.  Preserves AX, CX and DX. */
	push	ax
	push	cx
	push	dx
	mov	cl, al			/* the character survives the status poll */
	mov	dx, UART_LSR
1:	in	al, dx
	test	al, 0x20		/* TX holding register empty */
	jz	1b
	mov	al, cl
	mov	dx, UART_TX
	out	dx, al
	pop	dx
	pop	cx
	pop	ax
	ret

puts:					/* DS:SI = NUL-terminated string.  Preserves AX/SI/DX. */
	push	ax
	push	si
1:	mov	al, [si]
	test	al, al
	jz	2f
	call	putc
	inc	si
	jmp	1b
2:	pop	si
	pop	ax
	ret

put_hex_byte:				/* AL = byte.  Preserves AX/SI/DX. */
	push	ax
	push	si
	mov	ah, al
	shr	al, 4
	call	1f
	mov	al, ah
	call	1f
	pop	si
	pop	ax
	ret
1:	and	al, 0x0F
	cmp	al, 10
	jb	2f
	add	al, 'a' - 10
	jmp	3f
2:	add	al, '0'
3:	jmp	putc

put_hex_word:				/* AX = word.  Preserves AX/DX. */
	push	ax
	mov	al, ah
	call	put_hex_byte
	pop	ax
	jmp	put_hex_byte

crlf:
	mov	al, 0x0D
	call	putc
	mov	al, 0x0A
	jmp	putc

/* ------------------------------------------------------------------ *
 * THE FLAGS A BIOS CALL RETURNS ARE IN THE IRET FRAME, NOT IN THE FLAGS REGISTER.
 * `int n` pushed the caller's FLAGS; `iret` pops them; so a handler that sets CF or ZF
 * with `stc`/`or` LOSES that bit on the way out.  The kernel reads the returned flags --
 * `intcall` (bioscall.S) copies EFLAGS out of the post-INT register file, and
 * arch/x86/boot/memory.c:52 breaks the E820 chain on CF while initregs sets CF=1 in every
 * call -- so a handler that must report success has to clear CF IN THE FRAME:
 *
 *     push bp / mov bp,sp -> [bp+0]=saved BP, [bp+2]=IP, [bp+4]=CS, [bp+6]=FLAGS
 *
 * (The kernel's other probes -- EDD, APM, MCA -- read CF=1 as "not supported", which is
 * why every one of them "worked" before this was found: the caller's own CF=1 was being
 * read back.  MEASURED: without this, detect_memory_e820() breaks on its first call and
 * the kernel falls back to the int 15h/88h path -- the reference's log said "BIOS-88:
 * [mem 0x0-0x9efff] usable" instead of "BIOS-e820: ...".)
 * ------------------------------------------------------------------ */
iret_cf0:
	push	bp
	mov	bp, sp
	and	word ptr [bp + 6], 0xFFFE	/* CF = 0 */
	pop	bp
	iret

iret_cf1:
	push	bp
	mov	bp, sp
	or	word ptr [bp + 6], 1		/* CF = 1 */
	pop	bp
	iret

iret_zf1_cf0:					/* the "no key pending" answer (tty.c:78-86) */
	push	bp
	mov	bp, sp
	and	word ptr [bp + 6], 0xFFBE
	or	word ptr [bp + 6], 0x40		/* ZF = 1 */
	pop	bp
	iret

/* ------------------------------------------------------------------ *
 * init
 * ------------------------------------------------------------------ */
init:
	cli
	cld
	xor	ax, ax
	mov	ds, ax
	mov	es, ax
	mov	ss, ax
	mov	sp, 0x7C00		/* the traditional boot stack, below 0x7E00 */

	/* zero the IVT and the BIOS data area (0x0000-0x03FF, 0x0400-0x04FF) */
	xor	ax, ax
	xor	di, di
	mov	cx, 0x200		/* 512 words = 0x400 bytes */
	rep	stosw

	/* every vector points at the loud "undefined interrupt" handler... */
	mov	ax, SEG
	mov	ds, ax
	xor	ax, ax
	mov	es, ax
	xor	di, di
	mov	cx, 256
1:	mov	ax, offset undefined_int
	stosw
	mov	ax, SEG
	stosw
	loop	1b

	/* ...then the ones this firmware implements */
	mov	ax, SEG
	mov	ds, ax
	mov	es, ax
	mov	bx, offset ivt_table
2:	mov	si, [bx]		/* the vector */
	mov	di, [bx + 2]		/* its handler's offset */
	shl	si, 2
	xor	ax, ax
	mov	es, ax
	mov	es:[si], di
	mov	ax, SEG
	mov	es:[si + 2], ax
	add	bx, 4
	cmp	bx, offset ivt_table_end
	jb	2b

	/* the BIOS data area a PC has at this point */
	xor	ax, ax
	mov	es, ax
	mov	word ptr es:[BDA_EBDA], 0x9FC0	/* EBDA pointer (nothing lives there) */
	mov	word ptr es:[BDA_EQUIP], 0x0021	/* one floppy, 80x25 colour */
	mov	word ptr es:[BDA_MEMSIZE], 640	/* base memory in KiB */
	mov	byte ptr es:[BDA_MODE], 3	/* 80x25 text */
	mov	word ptr es:[BDA_COLS], 80
	mov	word ptr es:[BDA_CURSOR], 0
	mov	word ptr es:[BDA_CURSHAPE], 0x0607
	mov	dword ptr es:[BDA_TICKS], 0
	mov	byte ptr es:[BDA_ROWS], 24	/* rows - 1 */
	mov	byte ptr es:[BDA_FONTSIZE], 16

	/* interrupts are NOT enabled: this firmware installs no hardware IRQ handler and
	 * the kernel masks the PIC itself (pm.c:36-42) before it takes over. */

/* ------------------------------------------------------------------ *
 * scan for option ROMs (0xC0000-0xEFFFF, 2 KiB granularity, 0xAA55 + a zero
 * checksum over the block its size byte declares) and far-call the first one.  This is
 * what seabios does with the kernel's boot ROM; the host places it here.
 * ------------------------------------------------------------------ */
scan_roms:
	call	chipset_init
	mov	ax, 0xC000
	mov	es, ax
	xor	bx, bx
scan_next:
	cmp	word ptr es:[bx], 0xAA55
	jne	scan_advance
	mov	cl, es:[bx + 2]		/* the block length in 512-byte units */
	test	cl, cl
	jz	scan_advance
	xor	ch, ch
	shl	cx, 8			/* * 256 words */
	xor	si, si
	xor	al, al
1:	add	al, es:[bx + si]		/* two bytes per iteration: CX counts words */
	add	al, es:[bx + si + 1]
	add	si, 2
	loop	1b
	test	al, al			/* the checksum must be zero mod 256 */
	jnz	scan_advance
	jmp	rom_found
scan_advance:
	add	bx, 0x800
	cmp	bx, 0xE000
	jb	scan_next
	/* nothing bootable: LOUD, because a silent halt is the failure mode this unit
	 * exists to avoid */
	mov	ax, SEG
	mov	ds, ax
	mov	si, offset msg_no_rom
	call	puts
	jmp	halt

/* ------------------------------------------------------------------ *
 * CHIPSET INIT: enable the PIIX3's two IDE channels.
 *
 * WHY THIS IS HERE, and what it costs to leave it out (M2-3, HANDOFF=v86c): the Linux
 * kernel's ata_piix driver REFUSES to probe a channel whose "IDE enable" bit is clear.
 * piix_pata_prereset calls pci_test_config_bits(pdev, &piix_enable_bits[port]) -- PCI
 * config byte 0x41 bit 0x80 for the primary channel, byte 0x43 bit 0x80 for the secondary
 * (drivers/ata/ata_piix.c) -- and returns -ENOENT when the bit is zero, after which
 * libata's EH reports the port as disabled and the DEVICE IS NEVER PROBED.  Nothing is
 * printed either: the boot probe sets ATA_EHI_QUIET (libata-core.c's ata_port_probe), so
 * the only symptom is a missing "ata1.00: ..." line and no block device.
 *
 * A real BIOS programs those bits.  Seabios -- which is what the browser lane boots with,
 * and the reason a disk is visible there -- does exactly these two writes
 * (src/fw/pciinit.c:233-237):
 *
 *     pci_config_writew(bdf, 0x40, 0x8000); // enable IDE0
 *     pci_config_writew(bdf, 0x42, 0x8000); // enable IDE1
 *
 * Same two writes, same width, same bdf (bus 0, dev 0x1E = the IDE function upstream
 * registers).  A 16-bit config write of 0x8000 at byte offset 0x40 sets byte 0x41 to 0x80
 * and leaves byte 0x40 at 0; at 0x42 it sets byte 0x43.  MEASURED effect: without this,
 * NEITHER machine's guest kernel ever touches an IDE port (results/m2-3-ide.md 4); with
 * it, ata_piix probes the disk and the kernel's own dmesg reports it.
 * ------------------------------------------------------------------ */
chipset_init:
	call	pci_irq_route
	mov	dx, 0xCF8
	mov	eax, 0x8000F040		/* CONFIG_ADDRESS: bus 0, dev 0x1E, fn 0, dword 0x40 */
	out	dx, eax
	mov	dx, 0xCFC
	mov	ax, 0x8000		/* 0xCFC+0 -> config byte 0x41 |= 0x80 (IDE0 enable) */
	out	dx, ax
	mov	dx, 0xCFE
	mov	ax, 0x8000		/* 0xCFC+2 -> config byte 0x43 |= 0x80 (IDE1 enable) */
	out	dx, ax
	ret

/* ------------------------------------------------------------------ *
 * PCI INTERRUPT ROUTING for the virtio device (M2-4, HANDOFF=v86c).
 *
 * WHY THIS IS HERE: v86's PCI interrupt path reads TWO config-space bytes, and NEITHER is
 * written by anything but a BIOS.
 *
 *   1. the DEVICE's interrupt-line byte (config 0x3C).  Linux's PCI enumeration reads it
 *      (`pci_read_irq`, drivers/pci/probe.c: pin from 0x3D, then the line from 0x3C) and
 *      that value is `dev->irq`, i.e. what the driver passes to request_irq().  Upstream
 *      v86 leaves it 0, so a Linux guest requests IRQ 0 -- the PIT's.
 *   2. the PIIX3 ISA bridge's PIRQ router, bytes 0x60-0x63.  v86's
 *      PCI.prototype.raise_irq computes the vector a device's interrupt is delivered on
 *      from `this.isa_bridge_space8[0x60 + parent_pin]` (pci.js:581-583, and src/hw/pci.c's
 *      pci_raise_irq): parent_pin = (pin + device) & 3, device = (bdf >> 3) - 1.  Upstream
 *      v86's ISA bridge config space is 64 bytes and ZERO beyond 0x3F, so every PCI device's
 *      interrupt is delivered on IRQ 0.  A real BIOS programs these registers -- seabios does
 *      it as part of `pci_bios_init_device` -- and that is the only reason the browser lane's
 *      IRQs work.
 *
 * MEASURED, and the reason this unit needed it: with the routing left alone, the guest's
 * virtio console receives NO interrupt, and the driver's console path waits (in
 * __send_to_port, drivers/char/virtio_console.c) for the host to consume each buffer.
 *
 * THE VALUES.  The virtio console is PCI 00:12.0 (pci_id 0x0C << 3, virtio.js:66), INTA
 * (config 0x3D = 1).  v86's raise_irq arithmetic gives parent_pin = (0 + 11) & 3 = 3, i.e.
 * PIRQD = config byte 0x63 -- so 0x63 must hold the SAME vector as the device's interrupt
 * line.  IRQ 11 is what that pair is set to, on both machines (this firmware is the same
 * bytes on both legs).
 *
 * WHAT IS DELIBERATELY LEFT ALONE: every other device's route.  0x60-0x62 stay 0, so the IDE
 * controller keeps the exact routing M2-3 measured (its interrupts are delivered on IRQ 0
 * and its driver -- which asks for the hardcoded IRQ 14 -- never sees one; libata works by
 * polling).  Changing that would move the M2-3 gate's measured behaviour, which this unit
 * must not do.
 *
 * THE PROBE: an absent device answers -1 to a config read (pci_query's own arm, "required by
 * freebsd-9.1"), so the write happens only when a device is really there.  That keeps the
 * M2-2 boot (no virtio device) byte-for-byte unaffected: the reads happen, nothing is
 * written, and a read has no guest-visible effect.
 * ------------------------------------------------------------------ */
	.set	VIRTIO_IRQ,   11	/* the vector, shared by the line and the route */
	.set	VIRTIO_BDF,   (12 << 11) /* bus 0, dev 12 (0x0C), fn 0 -- pci_id 0x0C << 3 */
	.set	ISA_BDF,      (1 << 11)  /* bus 0, dev 1, fn 0 -- the PIIX3 ISA bridge */

pci_irq_route:
	/* is there a virtio device at 00:12.0? */
	mov	dx, 0xCF8
	mov	eax, 0x80000000 | VIRTIO_BDF
	out	dx, eax
	mov	dx, 0xCFC
	in	eax, dx
	cmp	eax, 0xFFFFFFFF		/* absent: nothing to route */
	je	1f

	/* its interrupt line, config byte 0x3C -- what the guest asks for */
	mov	dx, 0xCF8
	mov	eax, 0x80000000 | VIRTIO_BDF | 0x3C
	out	dx, eax
	mov	dx, 0xCFC
	mov	al, VIRTIO_IRQ
	out	dx, al

	/* the route v86's raise_irq will look up for it: PIRQD, config byte 0x63 of 00:01.0 */
	mov	dx, 0xCF8
	mov	eax, 0x80000000 | ISA_BDF | 0x60
	out	dx, eax
	mov	dx, 0xCFC + 3
	mov	al, VIRTIO_IRQ
	out	dx, al
1:
	ret

	/* the call site: BEFORE the option ROM is entered, i.e. before the kernel the ROM
	 * loads does anything at all.  (Seabios programs the chipset as part of its own
	 * bring-up, which is earlier still.) */

rom_found:
	mov	ax, es
	mov	cs:[rom_call_seg], ax	/* patch `lcall seg:3` */
	mov	ax, SEG
	mov	ds, ax
	mov	si, offset msg_rom
	call	puts
	mov	ax, es
	call	put_hex_word
	call	crlf
	call	rom_call_site
	/* an option ROM that RETURNS instead of booting is not bootable */
	mov	ax, SEG
	mov	ds, ax
	mov	si, offset msg_no_boot
	call	puts
	jmp	halt

halt:
	cli
1:	hlt
	jmp	1b

/* ------------------------------------------------------------------ *
 * the undefined-interrupt handler: prints the vector (read out of the INT
 * instruction the caller executed) and halts.  A missing handler is a STOP, not a
 * silent iret.
 * ------------------------------------------------------------------ */
undefined_int:
	mov	bp, sp			/* SS:BP -> [IP, CS, FLAGS] of the interrupt frame */
	push	ax
	push	bx
	push	ds
	push	si
	mov	ax, SEG
	mov	ds, ax
	mov	si, [bp]		/* the interrupted IP */
	mov	ax, [bp + 2]		/* the interrupted CS */
	mov	ds, ax
	mov	bl, [si - 1]		/* the byte after the 0xCD of `int n' */
	mov	ax, SEG
	mov	ds, ax
	mov	si, offset msg_undef
	call	puts
	mov	al, bl
	call	put_hex_byte
	call	crlf
	pop	si
	pop	ds
	pop	bx
	pop	ax
	jmp	halt

/* ================================================================== *
 * INT 10h -- video.  This machine has no VGA device (M2 defers it), so the text
 * buffer at 0xB8000 is the reference's zeroed VGA window: a teletype write lands in
 * the window and is visible nowhere.  What the handler owes the kernel is the
 * bookkeeping the real-mode setup READS BACK -- the mode, the cursor, the BDA fields
 * -- not the pixels.
 * ================================================================== */
int10_handler:
	mov	ax, SEG
	mov	ds, ax
	jmp	video_dispatch		/* ends in an iret; FS/GS untouched */

/* AH = function.  Clobbers AX/BX/CX/DX/ES; DS holds SEG. */
video_dispatch:
	mov	bl, ah
	cmp	bl, 0x00
	je	vid_set_mode
	cmp	bl, 0x01
	je	vid_set_cursor_shape
	cmp	bl, 0x02
	je	vid_set_cursor
	cmp	bl, 0x03
	je	vid_get_cursor
	cmp	bl, 0x0E
	je	vid_teletype
	cmp	bl, 0x0F
	je	vid_get_mode
	cmp	bl, 0x1A
	je	vid_dcc
	cmp	bl, 0x12
	je	vid_ega_config
vdu_iret:
	iret

vid_set_mode:				/* AH=00h AL=mode */
	xor	bx, bx
	mov	es, bx
	mov	es:[BDA_MODE], al
	mov	word ptr es:[BDA_COLS], 80
	mov	byte ptr es:[BDA_ROWS], 24
	mov	byte ptr es:[BDA_FONTSIZE], 16
	mov	word ptr es:[BDA_CURSOR], 0
	iret

vid_set_cursor_shape:			/* AH=01h CH/CL = scan lines */
	xor	bx, bx
	mov	es, bx
	mov	es:[BDA_CURSHAPE], cx
	iret

vid_set_cursor:				/* AH=02h BH=page DH=row DL=col */
	xor	bx, bx
	mov	es, bx
	mov	es:[BDA_CURSOR], dl
	mov	es:[BDA_CURSOR + 1], dh
	xor	ax, ax
	iret

vid_get_cursor:				/* AH=03h -> DH=row DL=col, CX=shape */
	xor	bx, bx
	mov	es, bx
	mov	dl, es:[BDA_CURSOR]
	mov	dh, es:[BDA_CURSOR + 1]
	mov	cx, es:[BDA_CURSHAPE]
	xor	ax, ax
	iret

vid_get_mode:				/* AH=0Fh -> AL=mode AH=columns BH=page */
	xor	bx, bx
	mov	es, bx
	mov	al, es:[BDA_MODE]
	mov	ah, 80
	xor	bh, bh
	iret

vid_dcc:				/* AX=1A00h -> AL=1Ah, BL=08h (VGA, colour) */
	cmp	al, 0x00
	jne	vdu_iret
	mov	al, 0x1A
	mov	bx, 0x0008
	iret

vid_ega_config:				/* AX=1200h BL=10h: BL != 10h means EGA/VGA present */
	cmp	bl, 0x10
	jne	vdu_iret
	xor	bx, bx			/* BL=0 -> "EGA/VGA", then AX=1A00h says which */
	iret

vid_teletype:				/* AH=0Eh AL=char BH=page: write it at the cursor */
	xor	bx, bx
	mov	es, bx
	mov	di, word ptr es:[BDA_CURSOR]	/* DH = row, DL = col */
	cmp	al, 0x0D
	je	tt_cr
	cmp	al, 0x0A
	je	tt_lf
	cmp	al, 0x08
	je	tt_bs
	/* write char + attribute 7 at VIDEO_SEG:(row*80 + col)*2 */
	push	ax
	mov	ah, 80
	mov	al, dh
	mul	ah			/* AX = row * 80 */
	add	al, dl
	adc	ah, 0			/* + col */
	add	ax, ax			/* a text cell is two bytes */
	mov	bx, ax
	mov	ax, VIDEO_SEG
	mov	es, ax
	pop	ax			/* AL = the character */
	mov	ah, 0x07
	mov	word ptr es:[bx], ax
	/* cursor: advance the column, wrapping into the next row */
	xor	bx, bx
	mov	es, bx
	inc	byte ptr es:[BDA_CURSOR]
	cmp	byte ptr es:[BDA_CURSOR], 80
	jb	tt_done
tt_cr:
	xor	bx, bx
	mov	es, bx
	mov	byte ptr es:[BDA_CURSOR], 0
	cmp	byte ptr es:[BDA_CURSOR + 1], 24
	jae	tt_done
	inc	byte ptr es:[BDA_CURSOR + 1]
	jmp	tt_done
tt_lf:
	xor	bx, bx
	mov	es, bx
	cmp	byte ptr es:[BDA_CURSOR + 1], 24
	jae	tt_done			/* no scroll: the row counter stops at 24 */
	inc	byte ptr es:[BDA_CURSOR + 1]
	jmp	tt_done
tt_bs:
	xor	bx, bx
	mov	es, bx
	cmp	byte ptr es:[BDA_CURSOR], 0
	je	tt_done
	dec	byte ptr es:[BDA_CURSOR]
tt_done:
	xor	ax, ax
	iret

/* ================================================================== *
 * INT 11h / INT 12h -- the equipment word and base memory, from the BDA
 * ================================================================== */
int11_handler:
	xor	bx, bx
	mov	es, bx
	mov	ax, es:[BDA_EQUIP]
	jmp	iret_cf0

int12_handler:
	xor	bx, bx
	mov	es, bx
	mov	ax, es:[BDA_MEMSIZE]
	jmp	iret_cf0

/* ================================================================== *
 * INT 13h -- no disk: every function reports failure (CF=1).  That is what makes the
 * kernel's EDD probe and any legacy read decline instead of reading garbage.
 * ================================================================== */
int13_handler:
	mov	ax, 0x0100		/* AH=01h "invalid function" */
	jmp	iret_cf1

/* ================================================================== *
 * INT 15h -- the system services.  ES:DI is the CALLER'S buffer for E820 and is
 * therefore never used as scratch here.
 * ================================================================== */
int15_handler:
	cmp	ax, 0xE820
	je	sys_e820
	cmp	ax, 0x2400
	je	sys_a20
	cmp	ax, 0x2401
	je	sys_a20
	cmp	ax, 0x2402
	je	sys_a20
	cmp	ax, 0xEC00
	je	sys_ok
	cmp	ah, 0x88
	je	sys_ext_mem
	/* E801h, APM (5300h), MCA (C0h), IST (E980h) and everything else: not
	 * supported, CF=1 -- exactly the answer the kernel's callers accept
	 * (memory.c:80-82, pm.c: no-op, apm.c/edd.c: "not found"). */
sys_unsupported:
	jmp	iret_cf1

sys_ok:
	xor	ax, ax
	jmp	iret_cf0

/* A20: reported enabled -- and it IS, because neither machine models an A20 mask, so
 * the kernel's own a20_test_short passes before this is ever asked (a20.c:44-70). */
sys_a20:
	xor	ax, ax
	jmp	iret_cf0

/* AH=88h: extended memory in KiB above 1 MiB */
sys_ext_mem:
	mov	ax, (MEM_TOP - 0x100000) >> 10
	jmp	iret_cf0

/* AX=E820h: the memory map.  EDX must be 'SMAP', EBX is the continuation index, ES:DI
 * the destination, ECX the buffer length.  This is the one service the kernel ALWAYS
 * uses and cannot do without: detect_memory (arch/x86/boot/memory.c:116-123) calls
 * e820, then e801, then int 15h/88h, and a loader-supplied map is never consulted. */
sys_e820:
	cmp	edx, 0x534D4150
	jne	sys_unsupported
	cmp	ebx, 4			/* four entries; the fifth call ends the chain */
	jae	sys_e820_end
	push	ds
	push	si
	push	cx
	push	ax
	mov	ax, SEG
	mov	ds, ax
	mov	ax, bx
	mov	cx, 24
	mul	cx
	add	ax, offset e820_table
	mov	si, ax
	mov	cx, 5			/* 5 dwords = the 20 bytes the kernel copies */
	rep	movsd			/* DS:SI -> ES:DI (the caller's buffer) */
	pop	ax
	pop	cx
	pop	si
	pop	ds
	mov	eax, 0x534D4150
	mov	ecx, 20
	inc	ebx
	cmp	ebx, 4
	jb	1f
	xor	ebx, ebx		/* the last entry: end the chain */
1:	jmp	iret_cf0		/* CF=0: this entry is valid, the chain continues */

sys_e820_end:
	xor	ebx, ebx
	jmp	iret_cf1		/* CF=1: no more entries */

/* ================================================================== *
 * INT 16h -- keyboard.  No key is ever pending (the port's PS/2 is bus-driven and
 * nothing is attached to it), so AH=01h answers ZF=1 and AH=00h returns 0 rather than
 * blocking forever on a key that cannot arrive.  AH=02h (shift state) is 0; AH=03h and
 * AH=05h (repeat rate) are accepted and ignored.
 * ================================================================== */
int16_handler:
	xor	ax, ax			/* kbd_pending() is false (tty.c:78-86) */
	jmp	iret_zf1_cf0

/* ================================================================== *
 * INT 1Ah -- the time of day.  AH=00h (tick count) and AH=02h (the RTC read through
 * the CMOS -- both machines implement that device) matter: getchar_timeout
 * (tty.c:98-120) uses AH=02h to time out its 30-second video menu, so it must be able
 * to change.  The real hardware RTC is the source, not a counter invented here.
 * ================================================================== */
int1ah_handler:
	cmp	ah, 0x00
	je	1f
	cmp	ah, 0x02
	je	rtc_time
	cmp	ah, 0x04
	je	rtc_date
	jmp	iret_cf1
1:	xor	bx, bx
	mov	es, bx
	mov	dx, es:[BDA_TICKS]
	mov	cx, es:[BDA_TICKS + 2]
	jmp	iret_cf0

rtc_time:				/* DH=seconds, CL=minutes, CH=hours (all BCD) */
	mov	al, 0
	call	rtc_read
	mov	dh, al
	mov	al, 2
	call	rtc_read
	mov	cl, al
	mov	al, 4
	call	rtc_read
	mov	ch, al
	xor	ax, ax
	jmp	iret_cf0

rtc_date:				/* DL=day, DH=month, CL=year, CH=century (BCD) */
	mov	al, 7
	call	rtc_read
	mov	dl, al
	mov	al, 8
	call	rtc_read
	mov	dh, al
	mov	al, 9
	call	rtc_read
	mov	cl, al
	mov	al, 0x32
	call	rtc_read
	mov	ch, al
	xor	ax, ax
	jmp	iret_cf0

rtc_read:				/* AL = CMOS register -> AL = its value */
	push	bx
	push	dx
	mov	bl, al
	mov	dx, 0x8000		/* the update-in-progress wait is bounded */
1:	mov	al, bl
	out	0x70, al
	in	al, 0x71
	test	al, 0x80		/* register A bit 7: update in progress */
	jz	2f
	dec	dx
	jnz	1b
2:	mov	al, bl
	out	0x70, al
	in	al, 0x71
	pop	dx
	pop	bx
	ret

/* ================================================================== *
 * INT 19h -- reboot: there is nothing to reboot into, so this stops.
 * ================================================================== */
int19_handler:
	jmp	halt

/* ================================================================== *
 * the table the IVT is built from: vector, handler offset
 * ================================================================== */
ivt_table:
	.word	0x10, int10_handler
	.word	0x11, int11_handler
	.word	0x12, int12_handler
	.word	0x13, int13_handler
	.word	0x15, int15_handler
	.word	0x16, int16_handler
	.word	0x19, int19_handler
	.word	0x1A, int1ah_handler
ivt_table_end:

/* ================================================================== *
 * the E820 map this firmware reports: a 64 MiB machine with a 640 KiB base, an EBDA
 * hole and the 0xF0000-0xFFFFF firmware area reserved.  An entry is base(8), len(8),
 * type(4), attributes(4) -- 24 bytes, of which 20 are copied.
 * ================================================================== */
	.align	4
e820_table:
	.long	0x00000000, 0x00000000, 0x0009FC00, 0x00000000, 0x00000001, 0x00000000
	.long	0x0009FC00, 0x00000000, 0x00000400, 0x00000000, 0x00000002, 0x00000000
	.long	0x000F0000, 0x00000000, 0x00010000, 0x00000000, 0x00000002, 0x00000000
	.long	0x00100000, 0x00000000, MEM_TOP - 0x100000, 0x00000000, 0x00000001, 0x00000000

/* ------------------------------------------------------------------ *
 * the far-call site for the option ROM: `lcall seg:3`, with the segment patched when
 * the ROM is found (the ROM's entry is at offset 3, after its 0xAA55 and size byte).
 * ------------------------------------------------------------------ */
rom_call_site:
	.byte	0x9A
	.word	3
rom_call_seg:
	.word	0

/* ------------------------------------------------------------------ *
 * strings
 * ------------------------------------------------------------------ */
msg_no_rom:
	.asciz	"minibios: no option ROM found at 0xC0000-0xEFFFF\r\n"
msg_rom:
	.asciz	"minibios: option ROM at "
msg_no_boot:
	.asciz	"minibios: the option ROM returned without booting\r\n"
msg_undef:
	.asciz	"minibios: undefined interrupt 0x"

/* ------------------------------------------------------------------ *
 * the reset vector: 0xFFFF0 is offset 0xFFF0 in this image and the CPU is reset with
 * CS=0xF000 (cpu_state.c's reset_cpu, cpu.rs:4755), so this is where execution begins.
 * ------------------------------------------------------------------ */
	.org	0xFFF0
	.byte	0xEA			/* jmp far SEG:init */
	.word	init
	.word	SEG
	.org	0xFFFF
	.byte	0
