# APIC-1R — independent review of the LOCAL APIC and the IOAPIC

**Unit:** `HANDOFF=v86c-apic`, UNIT APIC-1R (reviewer). **Inputs:** `results/apic-1.md` + the
`results/apic-1-*` evidence, the code on disk, and the references `v86/src/rust/cpu/apic.rs`
(658) / `ioapic.rs` (316). **Verdict: SHIP.** The one open device question — the unreproduced
SIGBUS — is **solved, and the device is exonerated**: the crash was the binary being rebuilt
under the live process, reproduced with the exact core signature. Four findings below, none a
code defect; two are record-keeping actions.

Every claim is tagged **MEASURED** (run here, command in §4), **READ** (source, with path) or
**INTERPRETATION**.

---

## 1. Finding 1 — the SIGBUS is solved: host-side, not the device (resolves the unit's open blocker)

The unit flagged one unreproduced SIGBUS/BUS_ADRERR on the first boot-2 run of the
reboot-with-`--acpi` workload (its 23:23 binary) and offered a wild-access hypothesis
(`mmap_write32`'s new mid-block `handle_irqs`). **That hypothesis is refuted, with the
mechanism demonstrated:**

- **MEASURED, from the kept core itself** (systemd coredump, pid 48872; parsed here, not
  taken from the unit's report): `SIGBUS`, `si_code = BUS_ADRERR`,
  **`si_addr == RIP == 0x40e010`**. The core's `NT_FILE` note shows `0x40e010` inside the
  **file-backed executable mapping of `build/v86c`** (`0x400000-0x4bb000`, backed by
  `/home/jaye/futaie/souche/build/v86c`), `rsp` mid-stack. That is an **instruction-fetch
  fault at the process's own valid text address** — a lost backing page, not a wild data
  access. A wild access from a C helper (the unit's hypothesis) faults at a *data* address
  with `si_addr != RIP`; it cannot produce this signature.
- **MEASURED, reproduction with asymmetry** (same binary bytes — a `/tmp` copy of the
  current `build/v86c`; same workload and command line as the soak):
  - **Control**, no interference: `rc=0`, boot 2 prints `DISK PERSIST OK`, `disk.img`
    untouched (mtime unchanged). Runtime ~112 s.
  - **Probe**: identical, except the backing file is rewritten in place at `+40 s`
    (emulating a relink under the live process — the ape loader file-maps the binary, and
    cold text pages fault in from the file). The process **died within ~1 s of the rewrite**
    with `SIGBUS/BUS_ADRERR`, **`si_addr = 0x40e010`** — the *exact* incident signature
    (same binary layout ⇒ same cold page) — with its serial tail in the same boot-2
    module-loading region (`thermal_sys`) where the incident died.
- **READ, why the mechanism fits the timeline**: no artifact from 23:20–23:30 survives (the
  unit's later builds overwrote everything), so the relink itself cannot be timestamped; but
  the unit's own record shows it started the first run and then immediately moved through
  rebuilds (the deferred-`handle_irqs` experiment, `-O1 -g`, `-O2 -g`, …). INTERPRETATION,
  now backed by a demonstrated mechanism: the first run was live when `build/v86c` was
  relinked; the crash is the rebuild, not the code.

**Consequences.** The 21 clean passes are not "21 lucky samples against a 1-in-22 gremlin"
— there is no in-process defect to sample. The unit's decision not to guess-fix was right,
and the upstream-faithful `handle_irqs()` call sites (memory.rs:284-289) should stay exactly
as they are.

**ACTION (SHOULD-FIX, record correction):** append a correction to `results/apic-1.md` §6 —
the record currently leaves the refuted wild-access reading as the leading hypothesis, and a
future reader would either avoid the mid-block poll site on superstition or "fix"
upstream-faithful code. State: `si_addr == RIP` in the file-backed text mapping (incident
core); rewrite-under-run reproduces the identical signature while the control survives;
device exonerated. Adopt the operational rule that follows: **long guest gates run a COPY of
the binary (or no build overlaps a gate)** — this review's own runs did.

## 2. Finding 2 — SHOULD-FIX (record hygiene): the clean-pass count disagrees

`results/apic-1.md` and the result node say 21 clean passes with **16** on the exact current
binary; `results/apic-1-soak.txt` says "21 clean passes on post-B1 builds (**15** on the
exact current binary)". One of the two miscounts. Append the true count (this review adds
one more clean control pass, on the 00:37 rebuild `6e15fd8b…`, which belongs to neither
count).

## 3. Finding 3 — NICE-TO-HAVE (comment accuracy): `apic.c:483-487`

The comment says the recursive `acknowledge_irq_internal` call "runs in the release too"
upstream. It does not: upstream's `dbg_assert!` is `debug_assert!` (`v86/src/rust/dbg.rs:63-67`),
which does **not** evaluate its argument when debug-assertions are off — only upstream's
*debug* build runs the recursion. The port runs it unconditionally; that is safe (READ,
verified: the inner call always exits at the `highest_isr >= highest_irr` guard before any
side effect — recursion depth 2, no state change), but the comment misstates the reference's
release behaviour. Reword.

## 4. Finding 4 — NICE-TO-HAVE: `apic.c:396` drops an assert without its comment

Upstream's `dbg_assert!(apic.timer_last_tick <= now)` (apic.rs:509, the periodic non-skip
arm) is dropped silently, while every other dropped check in the unit keeps its message as
a comment (§5 divergence 2's own policy). Add the one-line comment.

---

## 5. What was checked and found CLEAN

- **THE BOUNDS (brief item 3).** `ioregsel`'s "the branch is the bound" claim is TRUE:
  with `0x10 <= sel < 0x40`, `irq = (sel − 0x10) >> 1 <= 23` at both index sites
  (`ioapic.c:179,234`), and the range test is the same constant pair upstream's
  `IOAPIC_FIRST_IRQ_REG..IOAPIC_LAST_IRQ_REG` pattern encodes. The rig's M5 (relax the read
  arm to `<=`) is a genuine out-of-bounds catch, not a tautology: `ioredtbl_config[24]`
  *is* `ioredtbl_destination[0]` in the struct, and the selftest programs that neighbour to
  `0x55000000` first so the corrupted read is a neighbour's value (READ the rig; the unit's
  recorded run shows 2 failing assertions, restore green). `set_irq`/`clear_irq` return
  before any store (`ioapic.c:106,133` — the early return is not inside the dropped assert,
  exactly as upstream ioapic.rs:145-148/176-179). The APIC's irr/isr/tmr word numbers derive
  from switch constants; the delivery bit-index is a `u8 >> 5` (in [0,7] for every value of
  the type); the divider shift is two masked fragments in [0,7]. Both structs preserve
  upstream's `repr(C)` offsets (READ, field-for-field) — state-serialisation fidelity holds.
- **THE CLOCK AND THE STAIRCASE (item 2).** No host time anywhere in `apic.c`/`ioapic.c`;
  the timer consumes `microtick()` only. `run_hardware_timers` folds `apic_timer` in under
  the single `acpi_enabled` arm and composes `fmin(fmin(pit,rtc), fmin(acpi,apic))` —
  upstream's `Math.min` quartet (cpu.js:1924-1941, READ); with the flag off `apic_time`
  stays the constant 100, so default-off identity holds by construction (and G1 measured
  it). The `t` return is **discarded** by every consumer (`main_loop`'s caller,
  `instr_F4`), so the staircase's only guest-visible face is *when* timers fire — which is
  what both byte-identical pairs measure. **What the pairs do NOT cover:** `--acpi` under
  `--jit off` / `--uop-cache off` (every `--acpi` run used the default JIT-on config), and
  APIC-on vs APIC-off transcript identity (not expected; the dmesg differs by design).
- **THE REBOOT INTERACTION (item 4).** Upstream's `reboot_internal` (cpu.js:939-967, READ)
  names no APIC/IOAPIC reset; `reset_cpu` does not clear `acpi_enabled`/`apic_enabled`
  (READ: `cpu_state.c` never references them) — the port matches on both sides. The clock
  rewind lands in the `timer_last_tick > now` arms (`apic.c:163,373`); the rewritten
  reboot-table comment in `machine.c` is accurate. The live-state-across-reboot semantics
  (unmasked LVT entries keep delivering until the guest re-masks) is upstream's own.
- **THE DELIVERY PATH (item 5).** `handle_irqs` matches cpu.rs:4573-4584 arm for arm,
  including the spurious case (upstream `None` ⇔ port `-1` ⇒ no `pic_call_irq`) and the
  `else if (acpi_enabled)` gating on the *machine* knob. `device_raise_irq`/`lower` match
  cpu.rs:4596-4610. `mmap_write32`'s post-write `handle_irqs()` is upstream-unconditional
  (memory.rs:284-289, READ) and a flag-off guest reaching the window gets exactly
  upstream's behaviour (`apic_write32` early-returns; the poll runs — as upstream). 8/16-bit
  writes to the window are dropped on both sides (no `io.js`/`cpu.js` mmap registration
  upstream; default no-ops here). With the flag off the PIC path is byte-unchanged (G1 +
  the arms' structure).
- **THE IO-APIC LIMITATION (item 6).** TRUE: `acpi.js` is the 216-line PIIX4 PM device with
  no table content, no `mps.js` exists, and the guest prints *"ACPI MADT or MP tables are
  not detected"*. A MADT/MP builder is genuinely absent upstream too, so firmware tables
  are the (correctly-scoped) gap. The ported IOAPIC is **not dead code**: every
  `device_raise_irq` under `--acpi` drives `ioapic_set_irq`, so its line state and
  redirection table mutate with every device interrupt — it is just not guest-*discoverable*
  without tables.

## 6. COVERAGE

- **Read in full:** `src/cpu/apic.c`, `apic.h`, `ioapic.c`, `ioapic.h`, `src/cpu/memory.c`
  (sha `e05a7b90…` — the deferred-`handle_irqs` revert HELD), `v86/src/rust/cpu/apic.rs`,
  `ioapic.rs`.
- **Relevant regions read:** `pic.c` (handle_irqs/pic_call_irq), `machine.c`
  (machine_init / machine_reboot + the device table / run_hardware_timers /
  device_raise+lower_irq), `main.c` (`--acpi`, the run driver), `exec_boundary.c`
  (microtick), `cpu_state.c` (reset), `cpu.c` (main_loop), `instructions.c` (hlt),
  `hw_selftest.c` (`test_apic` + its `machine_reset`), `Makefile` (`CPU_SRC`/`CPU_HDR`),
  `memory.rs` (mmap arms), `cpu.js` (run_hardware_timers, mmap registrations, APIC state
  layout), `cpu.rs:4565-4615`, `dbg.rs:63-67`, `acpi.js`.
- **Deep, as the brief asked:** the incident core parsed directly (siginfo + prstatus +
  NT_FILE); the rewrite-under-run probe and its control (both on `/tmp` copies — the shared
  tree's `build/` was not touched by this review); `disk.img` confirmed unwritten by runs.
- **Not checked:** the 23:23 binary itself (unrecoverable; no 23:20-23:30 artifacts
  survive); the exact relink timeline (mechanism proven, timing inferred); the mutation rig
  not re-run (it mutates tree sources and rebuilds shared `build/` artifacts — and another
  lane rebuilt `build/v86c` at 00:37, sha now `6e15fd8b…` vs the unit's gated
  `ab9c410c…`; the rig was verified by reading it against the selftest's arms and by the
  unit's recorded 7/7 output); aarch64 legs (the unit's and the orchestrator's word).
- **File shas at review time** (sha256, first 16): `apic.c 105759810ea85195`,
  `apic.h 41aac3889d01fa70`, `ioapic.c 7fcdeff606bb62a0`, `ioapic.h 25186f2ec4688743`,
  `memory.c e05a7b90fe107ef8`, `pic.c 4e1e0ec13fa0a1e6`, `machine.c 91a770157631ec9a`,
  `main.c a5cde30c60b15b23`, `hw_selftest.c a628be73cafef32c`,
  `exec_boundary.c ae440dba0f96a836`, `cpu_state.c 37ddd24f7cfe97a4`.

## 7. RAW EVIDENCE (this review's own runs)

- Incident core: `/var/lib/systemd/coredump/core.\x2eape-1\x2e10.…48872.…zst`
  (`coredumpctl dump 48872`); probe core: same store, pid 243324 (00:47).
- Control: `/tmp/1r-ctl-serial.log` (rc=0, `DISK PERSIST OK`).
- Probe: `/tmp/1r-prb-serial.log` (dies in boot-2 `thermal_sys` module loading, same as the
  incident); stderr `Bus error (core dumped)`, rc=135.
