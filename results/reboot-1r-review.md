# REBOOT-1R — independent review of the reboot path (results/reboot-1r-review.md)

Reviewer unit: `HANDOFF=v86c-reboot`, REBOOT-1R (reviewer, read-only). Subject: REBOOT-1
(results/reboot-1.md). Memory node: `handoff-v86c-reboot-review`. Every file's sha256 is
recorded on the memory node; the tree state reviewed is the one the orchestrator gated
(21/21, kernel arm PASS).

**VERDICT: no blockers. 2 SHOULD-FIX (one contract gap in the compiled-path unwind, one
guest-reachable assert corner), 3 NICE-TO-HAVE.** The core design — request/unwind/re-entry
split, device table, loader divergence, cache clear — is sound and honestly documented; every
reference citation I was asked to check is correct; the two judgment calls I could attack
(UART exclusion, keys-would-be-safe) both held up against the code.

## SHOULD-FIX

### SF1 — the compiled JIT_EXIT_TAIL arm has no reboot check: a trigger executed from page 0xFF000 runs boot-2 instructions inside the stale batch
`src/cpu/uop_cache.c:1369-1377` (against `src/cpu/jit_codegen.c`'s `l_checks` steps 8/11 and
`src/cpu/jit_mem.c:552-559`).

The compiled block's four exits are handled at uop_cache.c:1338-1377: BLOCK_END returns,
RETURN returns, ABORT checks `cpu->reboot_requested` FIRST and unwinds (REBOOT-1's own
disambiguation) — but the TAIL arm (the fall-through at 1369) goes straight to
`previous_ip = ip; phys = get_phys_eip(); continue;`, and the top of the loop has no reboot
check either.

When the trigger port write fires inside a compiled block, `machine_reboot` sets
`jit_abort_flag`, but whether the block reports ABORT is decided by `l_checks`, not by the
flag: step 8 (the page-cross test, jit_codegen.c:1254-1258) compares the RESET eip
(0xFFFF0, page 0xFF000) with the trigger uop's `u->eip`. Different page — every shipped boot
(minibios code at low offsets of the 64 KiB firmware region, kernels at 0x100000+, gate
images at 0x100000) — the xor is nonzero and the block exits RETURN: safe, which is why all
three configs are byte-identical. SAME page — guest code executing the trigger from
0xFF000-0xFFFFF, which the port's own firmware IMAGE legally spans (main.c:635: "the firmware
fills 0xF0000-0xFFFFF", and the reset vector 0xFFFF0 is inside that page) — step 8 does not
fire, step 11 (eip+len != ip) sends TAIL, and `jit_block_exit` (jit_mem.c:552-559, called
unconditionally by `jit_execute_block`, jit_codegen.c:1653-1659) has ALREADY consumed and
cleared `jit_abort_flag`. The `continue` then does a fresh lookup at the reset vector, decodes
and compiles a block there, and executes it — boot-2 instructions run INSIDE the pre-reboot
uop_run call, before main.c's `reboot_prepare_reentry` clears the caches and re-establishes
the boot. That violates the contract all three REBOOT-1 comments state ("no guest
instruction of the next boot may run before the boot state is re-established", cpu.c:251-256,
uop_cache.c:1442-1446). Concrete harm: the reset-vector prologue executes twice (once inside
the stale batch, once after the re-entry), so any firmware prologue write (boot counting, e820
setup) doubles; `previous_ip` is also clobbered to 0xFFFF0 by the tail store (harmless today).
The interpreted executor is immune — its per-uop reboot check (uop_cache.c:1441) precedes the
page-cross/TAIL tests — so this is a compiled-path-only gap. INTERPRETATION from code (the
corner needs a page-0xFF000 trigger; I did not build such an image), but the ordering is
mechanical, not speculative.

**Fix (one check, covers every exit kind):** in `uop_run`'s compiled arm, immediately after
`jit_codegen_stats.exits[jit_why & 7]++;` (uop_cache.c:1336):

```c
			if (cpu->reboot_requested) {
				/* REBOOT-1: on ANY exit kind, not just ABORT — the TAIL
				 * arm would otherwise continue at the reset eip, and
				 * jit_block_exit has already consumed jit_abort_flag. */
				uop_flush();
				return;
			}
```

(The existing ABORT-specific check at 1355 then becomes dead-but-harmless; keep or fold it.)
Stored as `handoff-v86c-reboot-fix-1`.

### SF2 — cycle_internal's progress DBG_ASSERT can fire on a rebooting batch: a guest-reachable abort, not a reboot
`src/cpu/cpu.c:195-196`.

`cycle_internal` captures `initial_instruction_counter` (cpu.c:173), the reboot mid-batch
zeroes the counter inside `reset_cpu`, and the unwind path then FLUSHES: post-reboot
`instruction_counter == uop_pending` (uop paths, uop_cache.c:1296/1356/1447) or `== i`
(interpreted path, cpu.c:158) — i.e. the trigger batch's own instruction count — and
`uop_pending == 0`. The assert demands `counter != initial || uop_pending != 0`, so it fires
iff the trigger batch's count equals the cumulative count at cycle entry (initial == i).
Batch geometry is guest-controlled: an image whose boot runs N instructions to a batch
boundary and then N more to the trigger hits it exactly (e.g. batch1 = 7 instructions ending
in a jcc, batch2 = 7 instructions ending in the trigger `out`). The shipped builds keep
DBG_ASSERT live (Makefile CFLAGS has no -DNDEBUG; cpu_state.h:36-42 maps it to `assert`), so
the process aborts instead of rebooting — the same guest-reachable-DBG_ASSERT class this
project has twice resolved on principle (R3F, S2). None of the current gate images hit the
coincidence (the two-boot image's batches are 7 then ~10; the runaway image re-enters at
counter 0), which is why 21/21 passes. INTERPRETATION from code; the arithmetic is exact but
I did not construct the image.

**Fix:** make the assert reboot-tolerant — the reboot IS progress:

```c
	DBG_ASSERT(cpu->instruction_counter != initial_instruction_counter
	           || uop_pending != 0
	           || cpu->reboot_requested);
```

Stored as `handoff-v86c-reboot-fix-2`.

## NICE-TO-HAVE

- **N1 — `uop_stats.fallback_instr` wraps to ~4e9 across a reboot** (uop_cache.c:1267-1270):
  `cpu->instruction_counter - before` with `before` pre-reboot and the counter post-reset is
  an unsigned wrap. Diagnostics only; guard with `if (!cpu->reboot_requested)`.
- **N2 — TODO.md §1.4 is stale** (TODO.md:57-82 still says the two sites "stop the machine"
  and the behaviour is MISSING). The unit flagged this itself; it is still the pre-fix text.
- **N3 — cpu_state.c's citation "switch_seg(SS, 0x30) (cpu.rs:4757)"** (cpu_state.c:217) is
  off by five lines — 4757 is `jit_clear_cache_js`; switch_seg is 4752. Pre-existing, not
  this unit's (machine.c cites 4757 CORRECTLY for the clear), but since REBOOT-1's whole
  cache-clear argument leans on that line, the neighbouring wrong citation invites confusion.

## What I attacked and could not break (the brief's six questions)

1. **THE UNWIND.** Traced every hop for all four trigger sites-in-code: interpreted executor
   (per-uop check at uop_cache.c:1441 precedes the page-cross/TAIL/staircase tests), the
   mid-batch fallback step (1292, before its boundary tests), interpreted_run (cpu.c:140, and
   `counter += i` still runs), compiled ABORT (1355) and RETURN/BLOCK_END. Committed state at
   the break: pending flushed (0), counter = batch count (reset-then-flush), `previous_ip`/
   `instruction_pointer` keep the RESET values (the store to `body_ip` happens only at the
   TOP of the next iteration, which the break skips; the compiled epilogue's reg32 spill-back
   happens AFTER the reload already picked up the reset values, so it writes reset values
   back). `reset_cpu` does not clear `reboot_requested`, and zeroes ECX, so a `rep outs`
   trigger loop self-terminates after one machine_reboot (a second call is idempotent
   memset-based state). do_many_cycles_native's flag-in-condition closes the batch even where
   the zeroed counter would not underflow (cpu.c:211-216). main_loop returns promptly
   (cpu.c:257) — and its trailing `handle_irqs` (cpu.c:244) is safe post-reset: FLAGS_DEFAULT
   is bit 1, not IF (cpu_state.h:68), and pic_reset emptied the PIC. `in_hlt` at trigger is
   unreachable (both triggers are port writes). Lost-request paths: none found — except SF1's
   TAIL corner, where the request is not lost but deferred past boot-2 instructions. SF2 is
   the one place the unwind itself can die.
2. **THE CACHE CLEAR.** Citation verified in the read-only tree: cpu.rs:4757 IS
   `jit::jit_clear_cache_js()` (and cpu.js:939-965's reboot_internal/reset_memory split is as
   quoted). Placement argument is sound: machine_reboot fires inside a device write inside
   uop_exec inside the executor loop, which holds `uops`/`blk` pointers into the arena
   uop_clear_internal recycles (slot-0 reuse at uop_cache.c:956); the clear at the main.c
   block boundary (reboot_prepare_reentry, main.c:411-416) runs before any boot-2 instruction
   in both drivers. The keys-safe claim holds mechanically: the decoder captures only bytes +
   the state byte; segment bases are read at EXECUTE time (uop_seg_base, uop_cache.c:372-395)
   and paging is applied in the bodies; reset changes no RAM — so M5's NOT-CAUGHT verdict is
   the expected result, and the clear stands on the reference's behaviour plus the
   uop_cache_clear→jit_code_clear_all correctness pair (uop_cache.c:843-849), as documented.
3. **THE DEVICE TABLE.** The additions are leak-free: pit_init/rtc_init re-register only I/O
   handlers, and io_register_read/write OVERWRITE slots (io.c:286-310) — idempotent;
   ps2_reset and virtio_console_reset are state-only (no bus_register); the UART claim is
   verified in code (uart_attach mallocs, uart.c:533-541; bus_register appends with the
   64-listener assert, bus.c:39-42) and every uart interrupt-raise site is guest-write or
   host-input driven (no timer, no poll) — a stale IER/THRI cannot fire uninvited, and the
   reference leaves the UART unreset too. IDE/DMA/PCI/ACPI mirror the reference; the PIC's
   firmware-difference justification is real (minibios programs neither PIC nor PIT,
   pic.c:73-89). I found no device left observably wrong for a boot-2 guest.
4. **THE LOADER DIVERGENCE.** The reasoning and the measurement hold: kernel, initrd, hda and
   firmware FILES are read exactly once before the run loop (main.c:981-999); kernel_load
   takes in-memory buffers and does no file I/O (kernel.c:153-156); the disk is
   machine_config.hda's host buffer, untouched by the reboot loop, and the kernel arm reads
   boot 1's marker back through the guest's own libata. The initrd REGION in guest RAM is
   re-written from the host buffer — the disclosed divergence (boot-1 writes into the initrd
   region are lost); stated, not hidden. Both run drivers loop (multiboot main.c:1341-1350,
   bzImage main.c:1128-1141); the third boot path (the guest firmware) is the bzImage
   driver's own bring-up and re-enters through the same loader re-run — each mode lands where
   it started.
5. **DETERMINISM.** The reboot path allocates nothing (all resets are memsets of static
   storage; uop_cache_clear reuses arenas), the RTC/PIT run on the fixed-epoch virtual clock,
   and the counter rewind is a pure function of the instruction stream; the tsc_* statics in
   cpu.c survive a reboot but are deterministic in both the port and the reference. Gates: two
   runs byte-identical (gate check 4 per variant) and m2-5 determinism unchanged.
6. **THE GATE.** reboot_window_model.py is a genuine third party: it predicts the ENTIRE 4 KiB
   window from the documented contract values (0x05 = ps2.js:149's `1| 4`, verified in the
   reference), while the asm reads actual device state and the transcript hashes actual
   memory — a whole-window hash that any unaccounted write (an interrupt frame under the
   stack top, a stray device write) breaks. The one shared-blind-spot risk (asm and model both
   misreading the same spec value) is anchored to the reference source and the mutation rig
   (M2/M3 CAUGHT with named verdicts). The runaway guard NAMES the loop (die at main.c:1133 +
   the gate greps for it) rather than spinning, and with the UART excluded nothing leaks
   across 16 reboots, so MAX_REBOOTS is what fires.

## Coverage

Read in full: src/hw/{machine.c,machine.h,bus.c,bus.h}, src/cpu/{cpu.c,uop_cache.c},
tests/{gate-reboot-1.sh,reboot2x.asm,reboot_window_model.py}, results/{reboot-1.md,
reboot-1-gates.txt}. Read the relevant regions (cited above): src/hw/{pci.c,ps2.c,uart.c,
pit.c,rtc.c,virtio_console.c,kernel.c}, src/cpu/{pic.c,jit_mem.c,jit_codegen.c,
jit_codegen.h,cpu_state.c,cpu_state.h}, src/main.c (drivers, run(), reboot_prepare_reentry,
file reads, multiboot_set_state), src/hw/hw_selftest.c (the two reboot seats), TODO.md §1.4,
src/hw/{pci.h,ps2.h}. Reference (read-only): cpu.rs:4740-4762, cpu.js:935-975, pci.js:120-140,
ps2.js:145-155+828-845. NOT checked: results/reboot-1-mut-rig.sh's script body (its verdicts
were read in the gates file only), the JIT recipe emission bodies (jit_codegen.c:300-1040 —
outside the reboot ordering question), minibios.asm beyond its layout comment, the kernel
arm's initramfs contents, and SF1/SF2 were not demonstrated by constructed images (marked
INTERPRETATION; both are exact-arithmetic code reads).
