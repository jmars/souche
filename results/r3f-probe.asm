/* results/r3f-probe.asm -- the N1 probe guest (unit R3F).
 *
 * A flat 32-bit image at 0x100000, assembled exactly like tests/test.asm (as --32,
 * tests/test.ld, objcopy -O binary), so BOTH legs of the milestone contract can run it:
 * the pinned JS reference (`node oracle/run.mjs <img> <limit>`) and the C port
 * (`./build/v86c <img> <limit>`).
 *
 * THE SEQUENCE UNDER TEST is the PREFIX FALLTHROUGH arm of the generated dispatcher: an
 * encoding whose prefix combination the instruction table does not define for that opcode,
 * so the arm runs the group's UNPREFIXED body.  MEASURED on the reference: `F2 0F 6F C1`
 * completes -- the reference's dbg_assert! is debug_assert! and is compiled out of the
 * shipped release build -- while pre-R3F the C port ABORTED inside that arm.
 *
 * Build one variant per mode (the rig does this):
 *
 *   --defsym MODE=1  STRAY:   F2 0F 6F C1          -> the fallthrough arm (4 bytes)
 *   --defsym MODE=2  INERT:   3E 0F 6F C1          -> the SAME arm, same length, reached
 *                                                    by a prefix the arm's mask does not
 *                                                    name (a segment override) -- so this
 *                                                    arm aborted pre-R3F on nothing.  The
 *                                                    control for MODE=1: equal length,
 *                                                    equal instruction count, so the pair
 *                                                    isolates the STRAY PREFIX (MEASURED:
 *                                                    the two reference transcripts are
 *                                                    identical except the image hash).
 *   --defsym MODE=0  PLAIN:   0F 6F C1             -> the same encoding with no prefix
 *                                                    (3 bytes; the eip differs by one)
 *   --defsym MODE=4  STORE:   F2 0F 7F 05 <disp32> -> a fallthrough whose plain body
 *                                                    writes MEMORY, which is how the
 *                                                    premise "the reference EXECUTES the
 *                                                    unprefixed body" is measured rather
 *                                                    than assumed: mm0 is loaded from eax
 *                                                    first, and the 8 bytes land in the
 *                                                    hashed window.
 *   --defsym MODE=5  BYTES:   A3 <disp32>          -> the same 8 bytes written by an
 *                                                    ordinary GPR store, i.e. the target
 *                                                    state MODE=4 must reproduce.
 *
 * What the reference does with MOVQ is NOT invisible (MEASURED: the x87 stack-empty bit and
 * tag word move), which is why results/r3f-inject.sh's stand-in body does not reproduce the
 * reference's transcript and says so.  The stray prefix is what is inert here; MOVQ is not.
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
	mov	eax, 0x11111111

	.if MODE == 5
	mov	[0x101000], eax		/* the 8 bytes at 0x101000, written by a GPR store */
	.elseif MODE == 4
	movd	mm0, eax
	.byte	0xF2, 0x0F, 0x7F, 0x05, 0x00, 0x10, 0x10, 0x00	/* F2 0F 7F /0 -> plain body: store mm0 */
	.elseif MODE == 1
	.byte	0xF2, 0x0F, 0x6F, 0xC1	/* F2 0F 6F C1: stray F2 -> fallthrough arm */
	.elseif MODE == 2
	.byte	0x3E, 0x0F, 0x6F, 0xC1	/* 3E 0F 6F C1: inert segment prefix, same arm */
	.else
	.byte	0x0F, 0x6F, 0xC1	/* 0F 6F C1: the unprefixed encoding */
	.endif

	mov	[0x101100], eax		/* a store AFTER the sequence (reached only if the
					 * machine did not stop at it) */
	hlt
