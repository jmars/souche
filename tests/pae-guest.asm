/* tests/pae-guest.asm -- the synthetic PAE guest (PAE-1, Level 2).
 *
 * Multiboot image in the tests/test.asm style: enters in 32-bit protected mode
 * (the multiboot loader's state: CS 0x08, flat segments, CR0=1, paging off).
 * Builds a PDPT/PD/PT, enables CR4.PAE + CR3 + CR0.PG, then exercises the ported
 * walk end to end:
 *   - every instruction fetch after paging-on runs through a 2 MiB mapping
 *     (PD[0] is a 2 MiB page -> physical 0, so virtual [0, 2M) is identity);
 *   - a stub is copied to physical 0x113000 and CALLed at virtual 0x200000, so it
 *     is fetched and executed through a 4 KiB mapping;
 *   - a 4 KiB data write/read (virtual 0x202000) and a 2 MiB data write/read
 *     (virtual 0x180000);
 *   - a 2 MiB high-composition DISCRIMINATOR: PD[2] maps virtual [0x400000, 0x600000)
 *     to physical 0x200000, so the PDE's bit 21 (1) differs from the access's bit 21
 *     (0); the write at 0x400000 must land 2 MiB away from the seeded canary at
 *     physical 0, which a PSE high-mask (0xFFC00000/0x3FF000) would clobber;
 *   - an NX #PF: JMP to virtual 0x201000 (the PTE has bit 63 set and EFER.NXE is
 *     on) faults with error code 0x11, caught by a minimal IDT whose handler records
 *     the code and redirects EIP past the NX page.
 *
 * WHAT THIS IMAGE DOES NOT COVER (all selftest-only, results/pae-1.md F2): the
 * 4 KiB arm's PTE reserved mask (high-half PTE), the PDE reserved mask, the PDPTE
 * not-present/reserved/#GP paths, and load_pdpte's #GP.  The pinned reference still
 * asserts on reserved PDPTEs, so those are unreachable here by design.
 *
 * The results land in the hashed window [0x101000, 0x102000):
 *   0x101000  4 KiB data round-trip (0x11223344)
 *   0x101004  2 MiB data round-trip through PD[0] identity (0x55667788)
 *   0x101008  the stub's EAX marker (0x4B4B4B4B) after the 4 KiB-page call
 *   0x10100C  the NX #PF error code the handler saw (0x11 expected)
 *   0x101010  the verdict (0x600D600D) / FAIL code 0xFACD00nn
 *   0x101014  A/D snapshot: PD[1] low dword after the 4 KiB data write
 *   0x101018  A/D snapshot: PT[0] low dword after the 4 KiB data write
 *   0x10101C  the 2 MiB bit-21 canary: 0xAAAAAAAA if the 0x400000 write went to
 *             physical 0x200000 (correct), 0x55667788 if it clobbered physical 0
 * EBX holds the verdict (read by --fixture).
 *
 * ONLY CLEAN PDPTEs are written (the pinned reference still asserts on reserved
 * PDPTEs); the #GP cases are selftest-only.
 */

	.intel_syntax noprefix

	.equ PDPT,   0x110000
	.equ PD,     0x111000
	.equ PT,     0x112000
	.equ PAGE4,  0x113000
	.equ NXPAGE, 0x114000
	.equ IDT,    0x115000
	.equ GDT,    0x116000
	.equ DATA4,  0x117000
	.equ GOLD,   0x600D600D

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
	mov	esp, 0x102000

	/* zero the table/IDT/stub region [0x110000, 0x118000) */
	xor	eax, eax
	mov	edi, 0x110000
	mov	ecx, 0x8000 / 4
	rep	stosd

	/* ---- build a minimal IDT: entry 14 = #PF -> pf_handler ---- */
	lea	eax, pf_handler
	mov	[IDT + 8 * 14 + 0], ax
	mov	word ptr [IDT + 8 * 14 + 2], 0x08
	mov	byte ptr [IDT + 8 * 14 + 5], 0x8E   /* P=1 DPL=0 32-bit interrupt gate */
	shr	eax, 16
	mov	[IDT + 8 * 14 + 6], ax
	lidt	[idtr]

	/* ---- build a flat GDT (the reference resolves the #PF gate's CS 0x08 here),
	 *      then reload the segments through it ---- */
	mov	dword ptr [GDT + 0], 0
	mov	dword ptr [GDT + 4], 0
	mov	dword ptr [GDT + 8], 0x0000FFFF      /* code 0x08: limit[15:0], base[15:0] */
	mov	dword ptr [GDT + 12], 0x00CF9A00     /* base[23:16]=0, access 0x9A, flags 0xCF */
	mov	dword ptr [GDT + 16], 0x0000FFFF     /* data 0x10 */
	mov	dword ptr [GDT + 20], 0x00CF9200     /* access 0x92, flags 0xCF */
	lgdt	[gdtr]
	ljmp	0x08, reload_cs
reload_cs:
	mov	ax, 0x10
	mov	ds, ax
	mov	es, ax
	mov	ss, ax
	mov	fs, ax
	mov	gs, ax

	/* ---- build the page tables ---- */
	/* PDPTE[0] -> PD */
	mov	dword ptr [PDPT + 0], (PD | 1)
	/* PD[0]: 2 MiB page -> physical 0 (identity for virtual [0, 2M)) */
	mov	dword ptr [PD + 0], (0x00000000 | 0x80 | 3)   /* PSE | RW | present */
	/* PD[1]: 4 KiB mapping -> PT */
	mov	dword ptr [PD + 8], (PT | 3)
	/* PD[2]: 2 MiB page -> physical 0x200000.  Its bit 21 (1) differs from the
	 * access's bit 21 (0) at virtual 0x400000 -- the discriminator for the 2 MiB
	 * high composition (see the round-trip below). */
	mov	dword ptr [PD + 16], (0x200000 | 0x80 | 3)   /* PSE | RW | present */
	/* PT[0] -> PAGE4, PT[1] -> NXPAGE (NX), PT[2] -> DATA4 */
	mov	dword ptr [PT + 0], (PAGE4 | 3)
	mov	dword ptr [PT + 8], (NXPAGE | 3)
	mov	dword ptr [PT + 12], 0x80000000             /* the NX bit (bit 63) */
	mov	dword ptr [PT + 16], (DATA4 | 3)

	/* ---- copy the 4 KiB stub into PAGE4 ---- */
	lea	esi, stub4k
	mov	edi, PAGE4
	mov	ecx, stub4k_end - stub4k
	rep	movsb

	/* ---- EFER.NXE (bit 11) ---- */
	mov	ecx, 0xC0000080
	mov	edx, 0
	mov	eax, 0x800
	wrmsr

	/* ---- CR4.PAE, CR3, CR0.PG|WP: paging turns on here ---- */
	mov	eax, cr4
	or	eax, 0x20
	mov	cr4, eax
	mov	eax, PDPT
	mov	cr3, eax
	mov	eax, cr0
	or	eax, 0x80000000 | 0x10000
	mov	cr0, eax
	/* the next fetch goes through PD[0]'s 2 MiB page */

	/* ---- 4 KiB data write/read ---- */
	mov	dword ptr [0x202000], 0x11223344
	mov	eax, [0x202000]
	mov	[0x101000], eax

	/* ---- 2 MiB data write/read ---- */
	mov	dword ptr [0x180000], 0x55667788
	mov	eax, [0x180000]
	mov	[0x101004], eax

	/* ---- the 2 MiB high-composition discriminator: seed physical 0, then write
	 *      through PD[2] (virtual 0x400000 -> physical 0x200000).  The CORRECT
	 *      masks give high = 0x200000 (the write is dropped, out of RAM); a PSE
	 *      mask gives high = 0x000000 and clobbers the canary. ---- */
	mov	dword ptr [0x000000], 0xAAAAAAAA
	mov	dword ptr [0x400000], 0x55667788
	mov	eax, [0x000000]
	mov	[0x10101C], eax

	/* ---- execute the stub through the 4 KiB mapping ---- */
	call	0x200000
	mov	[0x101008], eax          /* the stub returns 0x4B4B4B4B in EAX */

	/* ---- NX #PF: jump into the NX page; the handler redirects to after_nx ---- */
	jmp	0x201000
after_nx:

	/* ---- snapshot the A/D bits the walk set on the 4 KiB PDE/PTE ---- */
	mov	eax, [PD + 8]            /* PD[1] low dword (accessed set) */
	mov	[0x101014], eax
	mov	eax, [PT + 0]            /* PT[0] low dword (accessed + dirty) */
	mov	[0x101018], eax

	/* ---- the verdict ---- */
	mov	ebx, GOLD
	cmp	dword ptr [0x101000], 0x11223344
	je	1f
	mov	ebx, 0xFACD0001
1:	cmp	dword ptr [0x101004], 0x55667788
	je	1f
	mov	ebx, 0xFACD0002
1:	cmp	dword ptr [0x101008], 0x4B4B4B4B
	je	1f
	mov	ebx, 0xFACD0003
1:	cmp	dword ptr [0x10100C], 0x11
	je	1f
	mov	ebx, 0xFACD0004
1:	cmp	dword ptr [0x10101C], 0xAAAAAAAA
	je	1f
	mov	ebx, 0xFACD0005
1:	mov	[0x101010], ebx
	hlt

/* ---- the 4 KiB stub: returns a marker in EAX ---- */
stub4k:
	mov	eax, 0x4B4B4B4B
	ret
stub4k_end:

/* ---- the #PF handler: record the error code, redirect to the NX continuation ---- */
pf_handler:
	pop	eax                      /* the error code */
	mov	[0x10100C], eax
	lea	eax, after_nx            /* the faulting EIP is the NX target; skip to it */
	mov	[esp], eax
	iret

	.align 4
idtr:
	.word	256 * 8 - 1
	.long	IDT

	.align 4
gdtr:
	.word	3 * 8 - 1
	.long	GDT
