/* results/perf-fix-probe.asm -- the ITEM 2 probe guest (unit PERF-FIX).
 *
 * A flat 32-bit image at 0x100000, assembled like results/r3f-probe.asm and
 * results/r4f-probe.asm (as --32, tests/test.ld, objcopy -O binary), so BOTH legs of the
 * milestone contract can run it: the pinned JS reference
 * (`node oracle/run.mjs <img> <limit>`) and the C port (`./build/v86c <img> <limit>`).
 *
 * THE SEQUENCE UNDER TEST is a 66-PREFIXED (32-bit operand size) control transfer
 * executed in a 16-BIT CODE SEGMENT.  That combination is reachable from ordinary guest
 * bytes and it falsifies the invariant upstream's 14 `dbg_assert!(*is_32 || value <
 * 0x10000)` sites (instructions.rs:1319..2474) encode.  Those are `debug_assert!`s,
 * COMPILED OUT of the pinned release reference, so the reference simply executes; the C
 * port shipped them live (no -DNDEBUG) and ABORTED where the reference carries on:
 * MEASURED pre-fix on MODE=1 (perf-3's finding 3).
 *
 * HOW THE GUEST GETS INTO 16-BIT MODE (both legs, identically): clear CR0.PE (`mov cr0`,
 * cpu.rs:2931 -- set_cr0 only sets protected_mode, it leaves is_32 alone), then take the
 * far jump's REAL-MODE arm (`far_jump`, cpu.rs:1128), which is `switch_cs_real_mode` +
 * `update_cs_size(false)` -- i.e. is_32 = false, CS base = selector << 4.
 *
 * THE FAR JUMP IS CHOSEN SO THE 16-BIT ip IS A LEGITIMATE < 0x10000 VALUE: selector
 * 0xF010 gives CS base 0xF0100, and the target's offset is computed at assembly time as
 * `start16 - 0xF0100`, so the code at linear 0x10003x is reached with ip ~ 0x103x and the
 * probe's own instruction is the ONLY thing that can produce a >= 64 KiB value.
 *
 * Modes (the rig assembles one image per mode):
 *
 *   MODE=0  NO PROBE: the 16-bit mode switch, a 32-bit store, hlt.  The control for "the
 *           switch itself is handled identically by both legs and the machine runs on".
 *   MODE=1  THE BRIEF'S BYTES: `66 E8 24 78 56 34` -- call rel32 (+0x34567824) at the
 *           32-bit operand size, in 16-bit mode -> instructions.c's instr32_E8 site.
 *   MODE=2  THE OTHER ARM OF THE SAME CHECK: `E8 00 00 00 00` in 32-BIT mode, i.e. the
 *           same instruction with is_32 TRUE, where the check short-circuits... except
 *           that in 32-bit mode `E8` is the 32-bit form (instr32_E8) whose check cannot
 *           fire; the point of the mode is that BOTH legs stay in 32-bit mode and the
 *           probe is inert.
 *   MODE=3  THE SAME INSTRUCTION, A SMALL VALUE: `66 E8 00 00 00 00` in 16-bit mode ->
 *           the value stays < 0x10000, so even the PRE-FIX binary must not abort.  This
 *           isolates the abort to the VALUE (perf-3's own reading), not to the mode or
 *           the opcode.
 *   MODE=4  A CONTROLLED IN-IMAGE LANDING: `66 FF D0` (call eax) with eax = 0x10100 in
 *           16-bit mode -> the check is on `data` (instructions.c:2279, the FF /2
 *           helper), data >= 0x10000 so the pre-fix binary aborts, and the target
 *           linear address 0xF0100 + 0x10100 = 0x100200 is INSIDE the image, where a
 *           pad runs a store and hlt -- so the AFTER leg can be compared to the
 *           reference byte for byte on the state the guest then reaches.
 *
 * The store after the sequence lands in the transcript's hashed window (0x101800), so
 * "the machine executed past the sequence" is visible in the transcript and not only in
 * the exit status.
 */

	.intel_syntax noprefix

	.ifndef MODE
	.set MODE, 1
	.endif

	.section .mbheader, "a", @progbits
	.align 4
	.long	0x1BADB002
	.long	0x00010000
	.long	-(0x1BADB002 + 0x00010000)
	.long	0x00100000		/* header_addr */
	.long	0x00100000		/* load_addr   */
	.long	0
	.long	0x00101000		/* bss_end_addr */
	.long	0x00100020		/* entry_addr  */

	.section .text
	.code32
	.globl _start
_start:
	cld

	.if MODE == 2 || MODE == 5
	/* 32-bit mode controls.  MODE=2: `E8` here is the 32-bit form (is_32 true, the
	 * check's short-circuit arm); call the next instruction, i.e. a no-op transfer.
	 * MODE=5: a HUGE eip set entirely in 32-BIT mode -- NOT one of the 14 sites and
	 * not affected by ITEM 2 (no 66 prefix, no 16-bit code segment).  It is here to
	 * show what the port does when the guest fetches outside RAM at all, in both the
	 * pre-fix and the post-fix binary (see results/perf-fix.md ITEM 2, the cpu.c:100
	 * finding: the diverges-from-the-reference behaviour is PRE-EXISTING). */
	.if MODE == 2
	.byte	0xE8, 0x00, 0x00, 0x00, 0x00
	.else
	mov	eax, 0x3557785A		/* >= cpu->memory_size: outside guest RAM */
	jmp	eax
	.endif
	mov	eax, 0x12345678
	mov	[0x101800], eax		/* a store AFTER the sequence */
	hlt
	.else
	mov	eax, 0x12345678
	mov	ecx, cr0
	and	ecx, ~1			/* clear CR0.PE: protected_mode = false */
	mov	cr0, ecx
	/* the far jump: real-mode arm -> switch_cs_real_mode(0xF010), is_32 = false */
	.byte	0xEA
	.long	start16 - 0xF0100	/* the offset under the NEW CS base 0xF0100 */
	.word	0xF010

	/* linear 0x10003x, ip ~ 0x103x, is_32 = false -- 16-bit code from here */
start16:
	.code16
	.if MODE == 1
	.byte	0x66, 0xE8, 0x24, 0x78, 0x56, 0x34	/* call rel32 -> ip += 0x34567824 */
	.elseif MODE == 3
	.byte	0x66, 0xE8, 0x00, 0x00, 0x00, 0x00	/* call rel32 +0: value stays < 0x10000 */
	.elseif MODE == 4
	.byte	0x66, 0xB8, 0x00, 0x01, 0x01, 0x00	/* mov eax, 0x10100 (>= 0x10000) */
	.byte	0x66, 0xFF, 0xD0			/* call eax -> linear 0xF0100+0x10100 */
	.elseif MODE == 0
	/* no probe: the mode switch alone */
	.endif
	.byte	0x66, 0xB8, 0x78, 0x56, 0x34, 0x12	/* mov eax, 0x12345678 */
	.byte	0x66, 0x67, 0xA3, 0x00, 0x18, 0x10, 0x00	/* mov [0x101800], eax */
	hlt

	.code32
	.org	0x200			/* linear 0x100200: MODE=4's landing pad */
	.if MODE == 4
	.code16
pad:
	.byte	0x66, 0xB8, 0x78, 0x56, 0x34, 0x12
	.byte	0x66, 0x67, 0xA3, 0x00, 0x18, 0x10, 0x00
	hlt
	.endif
	.endif
