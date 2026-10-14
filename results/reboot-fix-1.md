# REBOOT-FIX-1 — the reboot review's two SHOULD-FIXes and three nits (results/reboot-fix-1.md)

Unit: `HANDOFF=v86c-reboot`, REBOOT-FIX-1 (coder). Subject: results/reboot-1r-review.md's
SF1 + SF2 and its three NICE-TO-HAVEs. Gates: results/reboot-fix-1-gates.txt (raw).
Memory: `handoff-v86c-reboot-result` (append), `handoff-v86c-reboot-fix-1`/`-2` (the review's
prescriptions — both corrected on contact with the code, one in premise, one not at all).

**VERDICT: SF2 fixed and CAUGHT by a new image (abort → reboot, all three configs, MEASURED).
SF1's guest-reachable corner does NOT exist on this tree — the review's mechanism fails at the
decoder's descriptor table — the prescribed check is applied anyway as a contract completion,
guarded by a new tripwire image instead of an asymmetry image (one cannot be built; why below).
All three NICE-TO-HAVEs taken. Gates: gate-reboot-1 39/39 (21 baseline + 18 new), kernel arm
PASS, selftest counts unchanged, nasm 118/118, transcript-diff byte-identical, determinism at
the pinned 4,000,053,925, bench verified-clause green, aarch64 relinked.**

## What changed (files)

- `src/cpu/cpu.c` — cycle_internal's progress DBG_ASSERT gains `|| cpu->reboot_requested`
  (SF2). The comment carries the arithmetic and the reference disposition (below).
- `src/cpu/uop_cache.c` — (a) the compiled arm gains the review's generic reboot check over
  EVERY exit kind, immediately after the exits tally (SF1 — contract completion; the comment
  states the measured reach); (b) N1: `uop_stats.fallback_instr` skips the unsigned-wrap
  across a rebooting fallback batch.
- `src/cpu/cpu_state.c` — N3: the `switch_seg(SS, 0x30)` citation is cpu.rs:**4752**, not
  4757 (4757 is `jit::jit_clear_cache_js()`, which machine.c cites correctly). Verified
  against v86/src/rust/cpu/cpu.rs:4748-4757.
- `tests/reboot-geom.asm` (NEW) + Makefile rule (`--defsym PADS=13` baked) — the SF2
  asymmetry image.
- `tests/reboot-fwpage.asm` (NEW) + Makefile rule — the SF1 tripwire image.
- `tests/gate-reboot-1.sh` — two new arms (9 checks each; 21 → 39).
- `TODO.md` §1.4 — rewritten as DONE (N2).

## SF2 — CONFIRMED, fixed, and caught: the progress assert aborts instead of rebooting

The review's arithmetic is right, and it is MEASURED, not just derived. With the trigger
batch's instruction count equal to the cumulative count at that cycle's entry, the post-unwind
counter equals `initial_instruction_counter` with `uop_pending == 0`, and the live DBG_ASSERT
aborts the process. The flush that produces the equality happens on every execution path, and
the instrumented run (a throwaway print at the assert, reverted) shows it directly —
`initial=48 counter=2 pending=0 reboot=1` for the fwpage image's stub batch in the JIT config:

- JIT on: the trigger `out` is a FLUSH uop, so the block's own step-7 flush (or step 8's,
  when the reset moved eip cross-page — every shipped boot) ends the batch at the trigger;
- uop path, JIT off: the per-uop reboot check (uop_cache.c:1441) precedes the staircase and
  flushes;
- pure interpreted: `interpreted_run` breaks on the flag (cpu.c:140) and `counter += i`.

So `counter == trigger-batch-count` in all three configs, and the assert fires iff that count
equals the cycle-entry count — batch geometry a guest controls.

**The image** (`tests/reboot-geom.asm`): the preamble's cycle boundaries are its I/O
instructions (every `out` carries UOP_DESC_FLUSH — gen table, E6/EE iflags 0x01 — MEASURED
cycle structure 0→10→12→14→16→17), so the trigger cycle enters at counter 17; the trigger
batch is [the marker store, a jmp — neither ends a cycle, MEASURED] + PADS nops + [mov al +
the trigger `out`] = PADS + 4. **PADS = 13 makes 17 = 17.**

**Pre-fix** (assert conjunct stashed, nothing else): rc 134, `assert(... || uop_pending != 0)
failed` at cpu.c — **all three configs** (reboot-fix-1-gates.txt). **Post-fix**: rc 0, verdict
GOLD, the whole-window model matches, in all three configs.

**The reference disposition** (the brief asks which behaviour matches): upstream's assert is
`dbg_assert!` → `debug_assert!` (v86/src/rust/dbg.rs:63), so the release reference has NO
assert there and simply proceeds through the reboot — the behaviour this fix keeps. The
reference's *debug* build carries the same latent abort in the same corner (its
`reset_cpu` also zeroes `instruction_counter` mid-batch, cpu.rs:3342 adds the batch back);
the port ships asserts live by design, so the port must exempt the reboot explicitly. The
reboot-tolerant conjunct is the site-local resolution in the project's standing style.

## SF1 — the prescribed corner is UNREACHABLE; fix applied as contract completion; tripwire instead of asymmetry

The review's mechanism: a trigger executed from page 0xFF000 leaves the compiled block via
step 11's TAIL ("eip + len != ip"), whose arm continues at the reset eip with
`jit_block_exit` having already consumed `jit_abort_flag` — boot-2 instructions inside the
stale batch. **Wrong on contact with the code at one step: the trigger is never mid-block.**
The reboot triggers are port writes, and the generated descriptors for the port writes carry
BOTH block-ending bits:

- `out imm8, al` (E6, the PS/2 trigger): uop_gen.c:632 — flags **0x60** (bit 5 =
  UOP_DESC_BLOCK_END), iflags **0x01** (UOP_DESC_FLUSH);
- `out dx, al` (EE, the PCI trigger): uop_gen.c:646 — flags **0x20**, iflags **0x01**;
- `outs` (6E/6F): 0x20 / 0x01 likewise.

So the decoder ENDS the block at the trigger (UOP_STOP_BLOCK_END, uop_cache.c:711-714 — the
trigger is the block's last uop by construction), and the reference batch ends with it (step
7's flush; the interpreted path's per-uop reboot check at :1441 fires even earlier).
Step 11's divergence test and the TAIL arm are reachable only from a MID-block trigger, and
no reboot trigger is ever mid-block. MEASURED end to end: the new fwpage image (below)
executes the trigger from page 0xFF000 with the reset vector overwritten by a marker stub —
exactly the review's construction — and the marker stays 0, the verdict is GOLD, and the
window model matches in all three configs, BEFORE as well as after the check. The instrumented
unwind shows the same-page JIT exit as step 7's flush (counter = the batch's own 2, pending 0)
followed by step 10's BLOCK_END — never TAIL.

Per the brief's own rule ("if the arm cannot be built with the existing harness, say so with
the reason rather than asserting the fix by inspection"): the G1 asymmetry arm **cannot be
built** — not because the harness is too weak but because the failure mode does not exist on
this tree; there is no pre-fix failing transcript to paste, and the honest gate is different:

1. **The fix is applied anyway** (the review's one-check prescription, uop_cache.c, after the
   exits tally). It is the contract all three REBOOT-1 comments state; today it costs one
   well-predicted branch per block exit and changes NO guest-visible behaviour (the unwind's
   committed state is identical — the flush it adds on the step-8/BLOCK_END exits adds 0 to a
   counter the batch already flushed, and on a hypothetical future TAIL it is the difference
   between the contract and a violation). What it protects against is concrete: any future
   trigger outside the port-write class (an ACPI sleep path, a triple-fault handler, a
   descriptor-table regen that drops `block_boundary` from an I/O encoding) would hit the
   TAIL arm exactly as the review derived.
2. **The tripwire** (`tests/reboot-fwpage.asm`): boot 1 overwrites the reset vector
   (0xFFFF0..0xFFFFF — plain RAM here; the multiboot re-entry does not re-copy the firmware,
   main.c multiboot_set_state) with a 16-bit stub that stores a marker into the hashed window
   at 0x101028 and spins; it copies a trigger stub to 0xFF000 and jumps there. If ANY future
   change makes the trigger stop ending its block/batch, the stale batch decodes the
   overwritten vector, the marker lands, and the arm fails as verdict **0xFACD0007** + a
   broken window hash — the exact signature the review predicted. GOLD today, in all three
   configs, before and after (reboot-fix-1-gates.txt).

The review's own INTERPRETATION flag ("I did not build such an image") is what held: the
ordering analysis was mechanical but stopped one descriptor short. Recorded as the project's
seventh prescription-corrected-on-contact.

## NICE-TO-HAVEs

- **N1** — `uop_stats.fallback_instr` guarded with `if (!cpu->reboot_requested)`: a rebooting
  fallback batch's count is not added (its baseline no longer exists; the unsigned difference
  would wrap to ~4e9). Diagnostic only, as the review said.
- **N2** — TODO.md §1.4 rewritten: the reboot path is DONE (shipped in REBOOT-1, reviewed with
  no blockers, this unit closed the two SHOULD-FIXes), with the accepted residuals listed.
- **N3** — cpu_state.c's citation corrected to cpu.rs:4752 (the review's line number for the
  comment was 217; the comment sits at 207 — same site, fixed there).

## Gates

`sh tests/gate-reboot-1.sh` → **39 passed, 0 failed** (21 baseline + the two new arms' 18);
`REBOOT_KERNEL_ARM=1` → **PASS** (a real i386 Linux guest reboots itself, boot 2 reads boot 1's
disk marker back); the mutation rig reproduces its baseline exactly (M1/M2/M3 CAUGHT, M5
honestly NOT-CAUGHT — the review's expected result); `make`, `make selftest` (all suites, every
count unchanged), `make nasm` 118/118, `make transcript-diff` byte-identical, the determinism
gate rc 0 at the same pinned 4,000,053,925 instructions, `make bench` rc 0 with its verified
clause green (125.53/117.98 MIPS vs 122.53/120.18 baseline — host noise, the gate is the
verified clause), the aarch64 APE halves relinked fresh. Raw: results/reboot-fix-1-gates.txt.

One process note kept for the record: the first kernel-arm run Bus-errored because it ran
CONCURRENTLY with the mutation rig, which rebuilds build/v86c mid-flight — the gate executed
a binary being rewritten. Re-run alone: PASS. One writer per lane, again.
