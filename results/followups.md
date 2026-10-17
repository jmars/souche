# Follow-ups: the two stated residuals closed (PAE task-switch atomicity + the SVGA lifecycle arms)

Two residuals the earlier units stated rather than hid, closed in one unit.  Each fix has an
asymmetry arm (a mutation that fails before and passes after), and every pin named in the
brief was re-run by exit code.  All claims are MEASURED on this host unless marked READ.

## ITEM 1 — the hardware task switch validates its PDPTE before any register commit (PAE-1 review F3)

**The residual** (review F3, `results/pae-1.md`): the PAE-1 unit reordered `load_pdpte` to
validate-before-commit at MOV CR0/CR3/CR4, but the hardware task switch in
`src/cpu/exceptions.c` was the ONE `set_cr3` site left out — it committed TR, the segments
and the instruction pointer *before* `set_cr3`, and set CR0.TS *after* it, so a switch to a
TSS whose CR3 points at a bad PDPT delivered #GP in a half-switched context (new segments,
old IP).

**The fix** (READ against the live tree):
- `src/cpu/paging.c`: `load_pdpte` is split into a static `read_pdpt` (the read + the two
  guest-reachable #GP(0) checks, no side effects) plus `load_pdpte` (calls it, then installs
  `reg_pdpte`) and a new `validate_pdpte` (calls it and discards the entries).  Behaviour of
  the three existing call sites is unchanged (same mask, same checks, same commit).
- `src/cpu/paging.h`: `validate_pdpte` declared.
- `src/cpu/exceptions.c`: after `new_cr3` is read, and *before* the first register commit
  (`cpu->flags &= ~FLAG_VM`), the switch now runs
  `if (PAE && PG && !validate_pdpte(new_cr3)) return;`.  On a bad PDPT the #GP lands with
  TR/segments/IP/registers/CR3/CR0 all untouched; `set_cr3` at the end re-validates an
  unchanged PDPT (idempotent) and commits CR3 + the TLB clear as the last commit.

`validate_pdpte` rather than `load_pdpte` is deliberate: it must **not** install the new
PDPTE cache while the new TSS is still being read (hardware reads the whole TSS under the
OLD page tables and only then loads CR3).  The honest limit of that choice is below.

**The probe** (`src/cpu/m1c_selftest.c`, `test_task_switch_pae_atomicity` — this is the
selftest that links the real `exceptions.c`/`paging.c`, so it drives `do_task_switch`
directly; no in-tree PAE guest uses hardware task switching).  It builds a PAE identity map
(PDPT[0]→PD, PD[0] = a 2 MiB page at 0), a #GP gate at vector 13, loads the old TSS, and
switches to a new TSS whose `TSR_CR3` points at a PDPT whose entry 0 has the high half set
(`0x0000000100000001`).  It asserts the #GP was delivered (CS = SEL_CODE2, IP = HANDLER_EIP,
ESP = STACK_TOP-16) **and** that no register commit happened: `sreg[TR]==0x20`,
`segment_offsets[TR]==TSS_OLD`, `cr[3]==PAE_PDPT`, `CR0.TS==0`, EAX/ECX still the old task's.

**The asymmetry arm** (MEASURED): mutating the hoisted guard to `if (0 && …)` (the pre-fix
shape — no early validation) and rebuilding gives **6 failures**, exactly the six
discriminating checks:

```
FAIL m1c_selftest.c: ESP == STACK_TOP-16 (got 0x6ef0)   <- the #GP frame used the NEW task's stack
FAIL m1c_selftest.c: sreg[TR] == 0x20 (got 0x28)
FAIL m1c_selftest.c: segment_offsets[TR] == TSS_OLD (got 0x6000)
FAIL m1c_selftest.c: cr[0] & CR0_TS == 0 (got 0x8)
FAIL m1c_selftest.c: EAX == 0xAAAA0000 (got 0xBBBB1111)
FAIL m1c_selftest.c: ECX == 0x00001111 (got 0xCCCC2222)
```

The half-switch is visible directly: the new task's ESP (0x6F00) received the #GP frame, its
TR/segments/registers/TS were committed.  Note `cr[3]` is asserted but is **not** a
discriminator: even the mutated switch leaves CR3 old, because `set_cr3`'s own `load_pdpte`
faults before committing CR3 — only the earlier commits leak.

**Second probe** (`test_task_switch_tss_old_tables`): the PAE *success* path — the new task's
PDPT is good but remaps 0..2 MiB to 0x100000.., and the switch still loads the fixture's
registers correctly (completes under PAE+PG).  No gate or probe otherwise reaches a PAE task
switch.

**Honest limit** (MEASURED, stated, not hidden): substituting `load_pdpte` for
`validate_pdpte` (the plausible "simplification") passes the whole suite — `do_task_switch`'s
old-TSS save and the `new_cr3` read warm the TSS's page in the TLB under the old map before
the PDPTE cache could change, so the later TSS reads hit the stale old entry either way.  The
only observable difference is a TSS straddling a 4 KiB page with its post-CR3 fields in a
cold page, which no probe drives.  `validate_pdpte` is kept for faithfulness (TSS read under
the old tables, no premature cache install), not because a test forces it.

## ITEM 2 — the SVGA disable and same-size re-enable arms (VGA-U4 review item 7)

**The residual** (review, `results/vga-u4.md` §7 items 6-8): the disable arm
(`vga.c:1711-1717`, `was_enabled && !svga_enabled`) and the same-size re-enable arm (dispi
index-4 with unchanged dims, whose `needs_update` is false so the dest buffer is NOT
re-zeroed) are composed of U1-U3-gated pieces but had no U4 image or probe driving the full
sequence.

**The probes** (`src/hw/hw_selftest.c`, `test_vga_svga`, driving the device through the real
16-bit dispi ports — no `vga.c` hook was needed):
- `P-DISABLE`: enable 320x200x32, then write dispi index 4 = 0x40 (enable bit clear).  Asserts
  `svga_enabled` flips false, and the ARM reverts `graphical_mode` to `attribute_mode & 1`
  (0 = text) with the adapter following (`vga_screen_is_graphical` false).  (The dims stay
  320x200 because the default all-zero CRTC makes `update_vga_size` early-return — the
  reference's own unconfigured-CRTC behaviour, so the dims are not a discriminator here.)
- `P-REENABLE`: enable, write LFB pixel 0 red, fill (full recompute, bitmap cleared), re-enable
  the SAME size (0x41 — the LFB is `fill(0)`-zeroed but `needs_update` is false so the dest
  buffer is NOT re-zeroed and the bitmap is NOT marked), write LFB pixel 1024 (page 1) green,
  fill.  Asserts `dest[0]` is the STALE red (page 0 not re-filled) and `dest[1024]` is green
  (page 1 re-filled) — the reference's own incremental-redraw shape.

**The asymmetry arms** (MEASURED):
- Disable: mutating the arm's guard to `if (0 && was_enabled && !svga_enabled)` → **2
  failures** (`!graphical_mode`, `!vga_screen_is_graphical`).
- Re-enable: mutating `needs_update` to always-true (`… || 1`, i.e. re-zero the dest + mark on
  every size call) → **1 failure** (`vga_px(dest,0)` reads 0 instead of the stale red).

## Verification — every pin from the brief, re-run by exit code (all exit 0, none moved)

- `make selftest` — green (includes `cpu_selftest`, `cpu_m1c_selftest` **258 checks/0**, `hw_selftest`
  **975 checks/0**, and the uop/jit suites).
- `make` — binary rebuilt; sha256 `2c64fd49a0b24829a94fcaba3483d33c0521977e6a0957bf46535d96c1d34155`
  (moved from the pre-change artifact, as required).
- `make nasm` — **118/118**.
- `make transcript-diff` — M0 transcript **byte-identical** to the pinned JS reference.
- `make determinism` — two oracle runs byte-identical.
- `sh tests/gate-m2-5-determinism.sh all` — **4,000,096,757** instructions on BOTH runs, serial
  and virtio transcripts byte-identical.
- `make bench` — **200,095,214** instructions on every run.
- `sh tests/gate-reboot-1.sh` — **39/39**.
- `sh tests/gate-m2-5-guest.sh` — 0 failing assertions.
- `sh tests/gate-pae-1.sh` — **11/11**.
- `sh tests/gate-pae-l3.sh` — 0 failing assertions on BOTH legs.
- `sh tests/gate-vga-0.sh` … `gate-vga-4.sh` — 0 failing assertions each (the five VGA gates).

## Round 2 — review findings F1/F2/F4 applied (F3 needs nothing)

The follow-ups review (`handoff-v86c-followups-review`) confirmed the hoist and found two
more defects plus two nice-to-haves.  This is what changed in round 2.

**F1 (should-fix, guest-triggerable host stop) — the switch is now MEMORY-atomic too.**  The
round-1 hoist sat after the old-TSS save, the old-busy clear, the new-busy SET and the
backlink write, so a faulted switch left the new task's TSS descriptor BUSY — a guest #GP
handler that fixed the PDPT and retried hit `panic_stop("…busy task")`, a host stop on a
retry hardware completes.  Fix: the `new_cr3` read + `validate_pdpte` moved to the TOP of
`do_task_switch`, above the save block and every descriptor write.  Re-derived the safety
claim myself: `validate_pdpte` → `read_pdpt` → `read64s` (`memory.c:467`) is a raw physical
load (`mem8`/`vga_mem8`, no `translate_address`, no pagefault, no reg_pdpte/TLB/CR/TR
writes), and `new_cr3` is a physical address, so the validation needs neither the old task's
page tables nor any register state — no virtual read anywhere in the chain.  Probe 6b now
also asserts, after the fault, the new TSS descriptor's busy bit is CLEAR and the old one
intact, and that a fix-then-retry COMPLETES (TR 0x28, no stop).

**F1 asymmetry** (MEASURED): moving the validation back below the busy/backlink writes (the
pre-F1 shape) gives **5 failures** — busy bit SET (0x2), old busy CLEARED (0x0),
`!interpreted_unimplemented` (the retry panicked), TR still 0x20, offset still 0x6100 — the
retry-then-panic reachable before and gone after, exactly as the review prescribed.

**F2 (should-fix) — the VGA probes now pin more of each arm.**
- `P-DISABLE` (a): the arm's central reachable effect (the revert + adapter mode) stays
  asserted, and two new `diff_addr_min/max` asserts pin the ORDER — `complete_redraw` must run
  with `graphical_mode` already reverted (the text branch leaves the diff range at
  `vga_memory_size,0`), so a redraw-before-the-revert (the ordering bug the U4 review named)
  is caught.  The arm's other three sub-effects are stated as unreachable in this fixture,
  with their reasons, in the probe comment (`update_vga_size` early-returns on the default
  CRTC; `set_font_bitmap(false)` is a no-op; `text_mode_redraw`'s cells can't be separated
  from a seed write's own `put_char`).
- `P-REENABLE` (b): after the stale-red check, the probe now writes page-0 pixel 1 blue and
  re-fills — `dest[0]` must read BLACK (the re-enable's `fill(0)` zeroed the LFB), RED only if
  that zeroing was lost.  This pins the case-4 `memset` the first probe left unobserved.

**F2 asymmetries** (MEASURED): reordering the disable arm's `complete_redraw` above the
revert → **2 failures** (`diff_addr_min` 0x0, `diff_addr_max` 0x80000); deleting the case-4
`fill(0)` memset → **1 failure** (`dest[0]` 0xff not 0x0).

**F4 (nice-to-have)**: the one sentence at `exceptions.c` (the `set_cr3` site) now records the
success-path double PDPT read and the pre-existing self-aliasing TOCTOU (upstream reads once
at the same late point; the hoist strictly narrows the window, never widens it).

**F3**: no change — the review CONFIRMED keeping `validate_pdpte` and the stated
validate-vs-load residual (the TLB-warming mechanism, and that only a page-straddling TSS
diverges).  The residual stays stated in probe 6c's header and above.

## Files touched

`src/cpu/paging.c`, `src/cpu/paging.h`, `src/cpu/exceptions.c`, `src/cpu/m1c_selftest.c`,
`src/hw/hw_selftest.c`, `TODO.md` (two lines: the "three call sites" residual is now four;
the U4 gate list names the two new lifecycle probes), and this file.  `src/hw/vga.c` was
mutated only transiently for the asymmetry arms and reverted — `git diff` shows no change to
it.  `v86/` untouched.
