/* results/perf-fix-2-probe.asm -- the second form of the 16-bit ip class.
 *
 * Unit PERF-FIX-2 (HANDOFF=v86c-perf), ITEM A's sweep.  Assembled and run exactly like
 * results/perf-fix-probe.asm (as --32, tests/test.ld, objcopy -O binary), so both legs of
 * the milestone contract run it: the pinned JS reference (`node oracle/run.mjs <img>
 * <limit>`) and the C port (`./build/v86c <img> <limit>`).
 *
 * WHAT IS UNDER TEST.  Upstream guards three FAR control transfers with a bare
 * `dbg_assert!(false)` under a guest-input guard:
 *
 *     instructions.rs:1001-1005  instr32_9A       (callf ptr16:32)
 *          if !*protected_mode || vm86_mode() {
 *              if 0 != new_ip as u32 & 0xFFFF0000 { dbg_assert!(false); }
 *          }
 *     instructions.rs:2462-2469  instr32_FF_3_mem (callf m16:32)
 *     instructions.rs:2490-2494  instr32_FF_5_mem (jmpf  m16:32)
 *
 * `dbg_assert!` is `debug_assert!` (dbg.rs:63) -- COMPILED OUT of the pinned release
 * reference, which therefore far_jumps with the FULL 32-bit new_ip.  The port shipped a
 * live `DBG_ASSERT(((uint32_t)new_ip & 0xFFFF0000u) == 0)` at each site, so ordinary guest
 * bytes aborted the machine where the reference carries on.  These are the same class as
 * PERF-FIX's 14 (instructions.rs:1319..2474), in the class's SECOND FORM (a
 * dbg_assert!(false) rather than a dbg_assert!(cond)); they are not among PERF-FIX's 14,
 * which is what "one grep's inventory, not a tree-wide sweep" meant.
 *
 * HOW THE GUEST GETS INTO 16-BIT MODE is PERF-FIX's probe's construction, unchanged (clear
 * CR0.PE with `mov cr0`, then take far_jump's REAL-MODE arm to selector 0xF010: CS base
 * 0xF0100, is_32 = false, and the code at linear 0x10003x is reached with a legitimate
 * ip ~ 0x103x).
 *
 * THE FWORD IS PLACED IN LOW MEMORY ON PURPOSE.  The m16:32 operands must be read with a
 * 16-bit-reachable address: MEASURED (this probe's first form, and identically in both
 * legs), `67`-prefixed disp32 addressing in this 16-bit code segment is decoded by BOTH
 * the reference and the port as a 16-bit displacement (the legs agreed to the byte: eip
 * 0x31000, esp 0xf8), so a disp32 cannot reach the image.  The 32-bit prologue therefore
 * STORES the fword at linear 0x800 (`mov dword [0x800], ...` with DS base 0) and the
 * 16-bit sequence reads it with `disp16 = 0x800` (DS base 0, MEASURED: the same store form
 * at 0x101800 lands exactly at 0x101800).
 *
 * THE NEW_IP VALUE.  0x10120 has a NONZERO high half (>= 0x10000) -- that is exactly what
 * the three sites test -- and it is small enough that the landing address
 * 0xF0100 + 0x10120 = 0x100220 is INSIDE the image (the pad: a store, then hlt), so the
 * AFTER leg reaches a state that can be compared with the reference byte for byte instead
 * of running off into RAM.  0x330 is the same arithmetic with a < 64 KiB new_ip, for the
 * control mode.
 *
 * Modes (one image each):
 *
 *   MODE=6  `66 FF 1E <disp16=0x800>` -- instr32_FF_3_mem (callf m16:32), fword = {0x10120,
 *           0xF010}.  Lands at 0x100220 (the pad: a store, then hlt).
 *   MODE=7  `66 9A <ip32> <cs16>` -- instr32_9A (callf ptr16:32).  Same landing.
 *   MODE=8  `66 FF 2E <disp16=0x800>` -- instr32_FF_5_mem (jmpf m16:32), a JUMP not a call,
 *           so the pad is reached with no stack effect.
 *   MODE=9  THE CONTROL: the same instruction as MODE=6 with new_ip = 0x330 (< 0x10000),
 *           so even the PRE-FIX binary must not abort -- it isolates the abort to the VALUE
 *           and not to the opcode or the mode.  It lands at 0xFFFF0 + 0x330 = 0x100320.
 *
 * The store after each sequence lands in the transcript's hashed window (0x101800), so
 * "the machine executed past the sequence" is visible in the transcript, not only in the
 * exit status.
 */

	.intel_syntax noprefix

	.ifndef MODE
	.set MODE, 6
	.endif

	/* the far pointer under test: MODE=9 is the control (new_ip < 0x10000) */
	.if MODE == 9
	.set FW_IP, 0x330
	.set FW_CS, 0xFFFF		/* base 0xFFFF0 -> landing 0x100320 */
	.else
	.set FW_IP, 0x10120		/* high half NONZERO: what the three sites test */
	.set FW_CS, 0xF010		/* base 0xF0100 -> landing 0x100220 */
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
	mov	eax, 0x12345678
	/* the fword the m16:32 forms read: linear 0x800, DS base 0 (protected mode) */
	mov	dword ptr [0x800], FW_IP
	mov	word ptr [0x804], FW_CS
	mov	ecx, cr0
	and	ecx, ~1			/* clear CR0.PE: protected_mode = false */
	mov	cr0, ecx
	/* the far jump: real-mode arm -> switch_cs_real_mode(0xF010), is_32 = false */
	.byte	0xEA
	.long	start16 - 0xF0100	/* the offset under the NEW CS base 0xF0100 */
	.word	0xF010

	/* linear 0x10003x, ip ~ 0x103x, is_32 = false -- 16-bit code from here.  DS's base is
	 * 0 here (MEASURED), so the disp16 below is a linear address. */
start16:
	.code16
	.if MODE == 6
	.byte	0x66, 0xFF, 0x1E, 0x00, 0x08	/* callf fword [0x800] */
	.elseif MODE == 8
	.byte	0x66, 0xFF, 0x2E, 0x00, 0x08	/* jmpf  fword [0x800] */
	.elseif MODE == 9
	.byte	0x66, 0xFF, 0x1E, 0x00, 0x08	/* callf fword [0x800] (small new_ip) */
	.elseif MODE == 7
	.byte	0x66, 0x9A			/* callf ptr16:32 */
	.long	FW_IP				/* new_ip -- high half NONZERO */
	.word	FW_CS				/* new_cs */
	.endif
	.byte	0x66, 0xB8, 0x78, 0x56, 0x34, 0x12	/* mov eax, 0x12345678 */
	.byte	0x66, 0x67, 0xA3, 0x00, 0x18, 0x10, 0x00	/* mov [0x101800], eax */
	hlt

	.code32
	.org	0x200			/* linear 0x100220: MODE=6/7/8's landing pad */
	.code16
	.byte	0x66, 0xB8, 0x78, 0x56, 0x34, 0x12
	.byte	0x66, 0x67, 0xA3, 0x00, 0x18, 0x10, 0x00
	hlt

	.code32
	.org	0x300			/* linear 0x100320: MODE=9's landing pad */
	.code16
	.byte	0x66, 0xB8, 0x78, 0x56, 0x34, 0x12
	.byte	0x66, 0x67, 0xA3, 0x00, 0x18, 0x10, 0x00
	hlt
