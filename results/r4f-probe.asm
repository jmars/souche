/* results/r4f-probe.asm -- the N3 probe guest (unit R4F).
 *
 * A flat 32-bit image at 0x100000, assembled like results/r3f-probe.asm (as --32,
 * tests/test.ld, objcopy -O binary), so BOTH legs of the milestone contract can run it:
 * the pinned JS reference (`node oracle/run.mjs <img> <limit>`) and the C port
 * (`./build/v86c <img> <limit>`).
 *
 * THE SEQUENCE UNDER TEST is a MISALIGNED FXSAVE/FXRSTOR operand -- reachable from
 * ordinary guest bytes, e.g. `fxsave [eax]` with eax & 0xF != 0.  Upstream guards it with
 * `dbg_assert!(addr & 0xF == 0, "TODO: #gp")` (misc_instr.rs:370/398), and `dbg_assert!`
 * is `debug_assert!`, COMPILED OUT of the pinned release reference -- so the reference
 * simply executes (real hardware would #gp; upstream's own TODO says so).  Pre-R4F the C
 * port emitted a LIVE DBG_ASSERT at that site, so this guest ABORTED the port where the
 * reference writes.
 *
 * Modes (the rig assembles one image per mode):
 *
 *   --defsym MODE=1  MISALIGNED FXSAVE: eax = 0x101001; `0F AE /0` -> the assert site
 *   --defsym MODE=2  ALIGNED CONTROL:   eax = 0x101000; the same instruction, so the pair
 *                                       isolates the ALIGNMENT (equal length, equal
 *                                       instruction count, same x87/SSE state saved)
 *   --defsym MODE=3  MISALIGNED FXRSTOR: eax = 0x101001; `0F AE /1` -> fxrstor's twin site
 *   --defsym MODE=0  NO FXSAVE:         the control for "the store after the sequence is
 *                                       reached", i.e. the machine did not stop
 *
 * The store after the sequence lands in the hashed window, so "the port executed past it"
 * is visible in the transcript and not only in the exit status.
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
	mov	eax, 0x12345678		/* the value the trailing store writes */

	.if MODE == 1
	mov	edx, 0x101001		/* 0x101001 & 0xF == 1 */
	fxsave	[edx]
	.elseif MODE == 2
	mov	edx, 0x101000		/* aligned: the control */
	fxsave	[edx]
	.elseif MODE == 3
	mov	edx, 0x101001
	fxrstor	[edx]
	.endif

	mov	[0x101800], eax		/* a store AFTER the sequence (reached only if the
					 * machine did not stop at it) */
	hlt
