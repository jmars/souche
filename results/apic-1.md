# APIC-1 — the LOCAL APIC and the IOAPIC

**Unit:** `HANDOFF=v86c-apic`, UNIT APIC-1 (`coder`). **Deliverables:** `src/cpu/apic.{c,h}` +
`src/cpu/ioapic.{c,h}` (the ports), the four `memory.c` stubs replaced by the real entry points,
the timer fan-out, the IRQ delivery path and the PIC ack, the `--acpi` CLI flag (DEFAULT OFF),
`results/apic-1.md` (this file), `results/apic-1-gates.txt` + the G2 serial captures + the
mutation rig `results/apic-1-mut-rig.sh`.

**Bottom line.** Both devices are ported in full (apic.rs 658 lines + ioapic.rs 316 →
apic.c 490 + ioapic.c 268), wired exactly where upstream wires them, gated by a `--acpi` flag
whose default is byte-identical to the pre-unit tree, and proved by the guest kernel's own
dmesg: with the flag on and `lapic` on the kernel command line, the same i386 Linux image that
prints *"Local APIC disabled by BIOS -- you can enable it with \"lapic\""* / *"no APIC, boot
with the \"lapic\" boot parameter to force-enable it"* prints *"Found and enabled local
APIC!"*, maps our window (*"mapped APIC to ffffc000 (fee00000)"*) and reads back OUR register
values (*"APIC ID: 00000000"*, *"APIC VERSION: 00050014"* — apic.c's own constants). The
IO-APIC leg is an honest finding: no bootable guest here can discover it, because neither
upstream v86 nor this port builds the firmware tables (MADT/MP) its discovery requires — the
device itself is delivered and gated. All gates green; one incident (a single unreproduced
SIGBUS, §6) is reported rather than hidden.

Every claim below is tagged **MEASURED** (run here, command in §7 or the raw captures),
**READ** (source, with a path; `v86/…` is the read-only reference) or **INTERPRETATION**.

---

## 1. What was built

| file | lines | upstream | what it is |
| --- | --- | --- | --- |
| `src/cpu/apic.c` / `.h` | 490 / 110 | `v86/src/rust/cpu/apic.rs` (658) | the LAPIC: the 4 KiB register block (ID/version/TPR/EOI/ICR/LVT/IRR/ISR/TMR/spurious/destination/error), `apic_timer`, `route`/`deliver`/`highest_irr`/`highest_isr`/`acknowledge_irq`, the 256-bit register helpers |
| `src/cpu/ioapic.c` / `.h` | 268 / 73 | `v86/src/rust/cpu/ioapic.rs` (316) | the IOAPIC: the indirect register block (IOREGSEL/IOWIN), the 24-entry redirection table, `check_irq`, `remote_eoi`, `set_irq`/`clear_irq` |
| `src/cpu/memory.c` | modified | `memory.rs:232-320` | the four stubs DELETED (apic.c/ioapic.c define the signatures memory.h already declared); the two mmio write arms now call `handle_irqs()` after the write, as memory.rs:284-289 does |
| `src/hw/machine.c` | modified | cpu.js:1924-1941, cpu.rs:4596-4610 | `run_hardware_timers` folds `apic_timer(t)` into the single `acpi_enabled` arm (cpu.js:1931-1935's composition); `device_raise_irq`/`device_lower_irq` run the IOAPIC half of the line instead of their named stop; `machine_init` calls `apic_init`/`ioapic_init` (upstream's statics are module-created — for the shipped binary machine_init runs once per process, the same freshness); the reboot table's acpi/apic entries rewritten (§4) |
| `src/cpu/pic.c` | modified | cpu.rs:4573-4584 | `handle_irqs`: the `else if (cpu->acpi_enabled)` arm asks `apic_acknowledge_irq()` (returns -1 for upstream's None) and delivers through the same `pic_call_irq` |
| `src/main.c` | modified | cpu.js:1014 | the boot-mode `--acpi` flag: sets `cpu->acpi_enabled = true` once, before `machine_init`, with no reset (upstream's byte at state offset 552 is not in reset_cpu either) |
| `src/cpu/cpu_selftest.c` | modified | — | the recording bridge gains the four mmio entry points (this binary does not link the device files; the same arrangement as its `io_port_*` bridge) |
| `src/hw/hw_selftest.c` | modified | — | the APIC group (G3, +80 checks: 631 total) |
| `Makefile` | modified | — | `CPU_SRC`/`CPU_HDR` gain the four files; they join the shipped unity TU automatically |

The two device files sit in `src/cpu/`, not `src/hw/`, for the same reason `pic.c` does there:
upstream's `apic.rs`/`ioapic.rs` live in the CPU crate next to `pic.rs`, and M2-1's
one-C-file-per-upstream-file rule is per upstream directory.

## 2. THE THREE DECISIONS

### Decision 1 — HOW IT IS ENABLED: `--acpi`, DEFAULT OFF, the reference's own composition

Upstream gates the whole machine on `settings.acpi` (cpu.js:1014:
`this.acpi_enabled[0] = +settings.acpi`), and the SAME flag creates the PIIX4 ACPI device
(cpu.js:1181-1185), sets the CPUID APIC bit, answers the `IA32_APIC_BASE` MSR and drives the
IOAPIC halves and the APIC ack. The port had the state (`cpu->acpi_enabled`) but no writer; the
brief's recommendation is a CLI flag mirroring the knob, and that is what landed: **`--acpi`
(boot mode), setting the flag once before the machine is assembled.** It is the reference's own
composition — one knob, the same arms — and DEFAULT OFF, because every existing gate's guest
runs the 8259 machine and that must not move. Enabling by default would have changed every
guest's observable behaviour at once; that decision stays with the orchestrator.

**G1, DEFAULT-OFF BYTE-IDENTITY — MEASURED, the first gate run:** `make transcript-diff`
byte-identical to the pinned JS reference; `make nasm` 118/118; `make selftest` all suites rc 0
(hw_selftest 631 checks, 0 failures — the count grew by the APIC group only); `make
determinism` rc 0; `sh tests/gate-reboot-1.sh` 39/39; `make bench` median 0.897/0.908 s against
the tree's own recorded median 0.895 (perf-7) — within noise; aarch64 compiles of the five
touched TUs rc 0 at -Werror. Raw: `results/apic-1-gates.txt`. The reason identity holds by
construction, not just by measurement: with the flag off, `apic_read32`/`apic_write32`/
`ioapic_read32`/`ioapic_write32` take upstream's own early returns (apic.rs:112/279,
ioapic.rs:194/244 — read 0, write discarded, exactly what the stubs answered), the timer
fan-out's new arm is inside `if (acpi_enabled)`, the IOAPIC raise halves are inside `if
(cpu->acpi_enabled)`, and `handle_irqs`'s second arm is `else if (cpu->acpi_enabled)`. The one
unconditional change is `handle_irqs()` after an APIC/IOAPIC-window mmio write
(memory.rs:284-289 — upstream's own unconditional call); a guest only reaches it by writing
0xFEE00000-0xFEE00FFF or 0xFEC00000-0xFEC0001F, which no existing gate's guest does.

### Decision 2 — THE CLOCK: the same D-F2 virtual clock, no host time

`apic_timer` consumes `microtick()` (exec_boundary.c: `instruction_counter / 1000.0`
milliseconds) at upstream's `APIC_TIMER_FREQ = 1e6` counts per millisecond — **1000 ticks per
guest instruction at divider /1, the same rate as the TSC** (m2-1f measured "1000 counts per
instruction"; the constants agree because upstream's comment says to keep them in sync,
apic.rs:8). No host time is read anywhere in apic.c or ioapic.c. MEASURED, pinned either side
of each boundary in hw_selftest's APIC group: count 2000 at /1 fires between instruction +1 and
+2; at /16 between +31 and +32; the periodic /1 arm advances `timer_last_tick` by exactly the
period (apic.rs:508's path) and the /16 resynchronise arm takes `last_tick` to now
(apic.rs:495-506 — with /16 one period's tick count already exceeds 2× the count, so upstream's
own units-mixed heuristic takes the skip arm every fire there; reproduced, not "fixed").

**Determinism with the flag ON — both pairs byte-identical.** (a) `sh
tests/gate-m2-5-determinism.sh all` under a wrapper that appends `--acpi` to every invocation:
rc 0, serial AND virtio transcripts byte-identical, the SAME instruction count
(4 000 024 062 vs 4 000 024 062) — raw: `results/apic-1-det-gate-acpi.txt`. (b) The
timer-actually-firing pair: the Alpine `vmlinuz-i386` + `lapic` (the kernel arms the LAPIC
timer — hw_selftest pins the delivery machinery, and this is the boot whose dmesg names it
enabled) booted twice on fresh disk copies: `cmp` says BYTE-IDENTICAL, no normalization
(raw: `results/apic-1-det-apic-lapic-diff.txt`; this kernel boot never prints the host-epoch
audit lines — those came from the initramfs boot — so the RTC's documented host-clock
nondeterminism, m2-1f §3.3, does not surface here).

### Decision 3 — THE REBOOT: not reset, exactly as the reference's statics

Upstream's `reboot_internal` (cpu.js:939-967) names no APIC/IOACIC reset, and `apic.rs`/`ioapic.rs`
have no reset at all — the statics survive. reset_cpu does not clear `apic_enabled` (offset 548)
or `acpi_enabled` (552) upstream, and the port's reset does not either (verified both sides).
So `machine_reboot` resets neither device, and the reboot table's entry was REWRITTEN (it said
the ACPI device "has no state to reset" — false once the flag exists). What a guest sees on
boot 2 with `--acpi`:

- **The clock rewinds** (reset_cpu zeroes the instruction counter microtick derives from), and
  both timers already carry the anticipation arms for exactly that: apic.c's `timer_last_tick >
  now` reset (apic.rs:474-478 — upstream's own arm, written for restore_state and hit by our
  rewind) and acpi.c's overshot-PM-timer arm (whose guest-reachable assert was already dropped
  for precisely this reboot case, acpi.c's LATENT note). MEASURED by the boot-2 legs below: no
  misfire, no abort.
- **An LVT/IOAPIC entry boot 1 left unmasked keeps delivering into boot 2** until the guest
  re-masks it — which a re-booting kernel's LAPIC re-init does. Upstream ships the same
  semantics; this is what the reference's guests live with.
- **MEASURED, the boot-2 evidence:** the reboot gate's real guest (Linux + `guest-initramfs-
  reboot.img`, PS/2-controller reboot) run WITH `--acpi` and `lapic`: boot 1 writes the disk
  marker, reboots, boot 2 reads it back — `BOOT2: marker read back from /dev/sda sector 8:
  DISK PERSIST OK`, rc 0. **CORRECTED (APIC-1R finding 2): this sentence said "a 10-run soak plus
  6 earlier clean runs" = 16 on the current binary, which does not reconcile with the evidence
  file.** `results/apic-1-soak.txt` records its session as 10 runs on the plain-make tree
  (`sha ab9c410c…`) and, as its session total, **21 clean passes across all post-incident builds,
  15 of them on the exact current binary**. The 15 is the figure the evidence supports; the 16 was
  the report's own arithmetic. The ONE exception to all of it is the incident in §6, which APIC-1R
  has since solved as an instruction-fetch fault in a binary that was being REWRITTEN WHILE
  RUNNING (a concurrent rebuild), not a device defect.

## 3. THE BOUNDS (the audit's class — every guest-controlled index, and where its bound is)

- **`ioregsel` is the fully guest-controlled one** (a 32-bit write to the IOWIN window offset
  stores any value, ioapic.rs:254). Both register dispatches index `ioredtbl_config[24]` /
  `ioredtbl_destination[24]` ONLY inside the branch testing
  `IOAPIC_FIRST_IRQ_REG <= ioregsel < IOAPIC_LAST_IRQ_REG` (0x10 ≤ sel < 0x40); there
  `irq = (sel − 0x10) >> 1` ≤ 23 **by the branch condition alone** — the bound is the branch,
  and the derivation is restated at both index sites (ioapic.c read and write arms). MEASURED
  asymmetrically: mutation M5 of the rig relaxes the read arm to `<=` — the out-of-bounds read
  aliases `ioredtbl_destination[0]` (the next u32 in the struct), which the selftest
  deliberately programs to 0x55000000 first so the corrupted answer is a NEIGHBOUR'S VALUE, not
  a coincidental zero — caught, 2 failing assertions.
- **`set_irq`/`clear_irq`: `i >= IOAPIC_IRQ_COUNT` returns BEFORE any store** (ioapic.rs:145-148,
  176-179 — the early return is NOT inside the dbg_assert, so the release keeps it). Mutations
  M1/M7 attack the delivery instead; the raise-bound is checked by call (ioapic_set_irq(24/200)
  then the IRR reads empty).
- **The APIC register block:** the router masks the offset `& ~3` inside
  [APIC_MEM_ADDRESS, +0x1000); the register dispatch is a switch over CONSTANTS; the
  irr/isr/tmr word numbers are each derived from one of eight switch constants
  (`(addr − 0x100) >> 4` et al.) and therefore in [0, 7] by construction. The delivery
  bit-index is a u8 vector, so `bit >> 5` lands in [0, 7] for every value the type can hold —
  including upstream's release behaviour of delivering on an "invalid" vector
  (apic.rs:562-564's dbg_assert is compiled out; the bit is set anyway).
- **The timer divider:** `divide_shift = (value & 0b11) | ((value & 0b1000) >> 1)` is two
  masked fragments — in [0, 0b111] by construction — so `timer_divider_shift` ∈ {0..7} and
  `1u << shift` never overflows. Mutation M3 (the table wrong) is caught at the boundary.
- **`register_get_highest_bit`** iterates a fixed 4×u64 join of the fixed 8×u32 arrays; no
  guest input reaches a bound.

Upstream's guest-reachable `dbg_assert!`s (the unknown-register arms of both read/write
dispatches, "read-only register", the invalid-vector TODO, "Bad irq") are DROPPED at their
sites with the message text kept as comments — the pinned release compiles them out
(dbg.rs:63-67) and proceeds; the G4-S2 policy. The release behaviour is what the port does:
read 0 / discard / early-return.

## 4. THE GATES

| gate | result | raw |
| --- | --- | --- |
| G1 default-off byte-identity | transcript-diff byte-identical · nasm 118/118 · selftest all suites rc 0 · determinism rc 0 · reboot gate 39/39 · bench median within noise · aarch64 rc 0 | `results/apic-1-gates.txt` |
| G2 the external consumer | OFF arm: *"Local APIC disabled by BIOS…"* + *"no APIC, boot with the \"lapic\" boot parameter to force-enable it."* + *"APIC: Keep in PIC mode(8259)"* — the 8259 path, as today. ON arm (`--acpi lapic`): *"Found and enabled local APIC!"* + *"mapped APIC to ffffc000 (fee00000)"* + the kernel printing OUR values: *"APIC ID: 00000000"*, *"APIC VERSION: 00050014"*. The IO-APIC leg: *"APIC: ACPI MADT or MP tables are not detected"* → *"Switch to virtual wire mode setup with no configuration"* — the finding, §5. Device IRQs stay on the 8259 in both arms. | `results/apic-1-g2-{off,on,on-apicdebug}-serial.txt` |
| G3 device selftest | the APIC group: register round-trips with upstream's write masks, the timer's fire-at boundaries at /1 and /16, the IPI self-shorthand, an IOAPIC redirection entry delivering through `device_raise_irq`'s real body (with the 8259 masked, the kernel's own arrangement), EOI clearing ISR, the level remote-EOI re-delivery, and each bound refusing — 80 checks | hw_selftest 631 checks, 0 failures |
| G4 no regression | the G1 set plus `make` rc 0 and the unity-symbol check (the new files fit the shipped TU, -Werror clean) | `results/apic-1-gates.txt` |
| G5 able to fail | 7 mutations, each alone: the delivery path (2), the timer (2), EOI, remote-EOI, and the IOAPIC bound — ALL VISIBLE, every restore green. One first attempt was INVALID (mutating the write arm corrupted an entry the selftest never re-read) and was replaced by the read-arm mutation + the 0x55 neighbour programming — itself the lesson in what an assertion can see. | `results/apic-1-mut-rig.sh` |

## 5. EVERY DIVERGENCE FROM UPSTREAM, WITH ITS REASON

1. **`apic_init`/`ioapic_init` exist and are called from `machine_init`.** Upstream's APIC/IOAPIC
   are `static Mutex`es created with the wasm module; this port gives them an explicit init at
   the one place a machine is brought up. For the shipped binary machine_init runs once per
   process — the same freshness. `machine_reboot` deliberately does NOT call them, so the
   across-reboot behaviour is upstream's (§2, decision 3).
2. **The dbg_assert!s are dropped at their sites** with the message text kept (§3) — the pinned
   release compiles them out; the code is the release behaviour.
3. **`acknowledge_irq` returns int32_t with −1 for None**, and the recursive self-check call
   keeps its shape but its result is unasserted — the invariant above the site (the bit just
   moved to ISR is ≥ every remaining IRR bit) makes the recursion return None with no side
   effect, so the assert cannot fail; a cannot-fail check is not a check (M2-1 D13).
4. **dbg_log!/dbg_trace! dropped everywhere** (the port has no log channel; M2-1 D12) — their
   text survives as comments where it names a behaviour ("Bad eoi: No isr set", "Not delivered:
   irr already set", the IOAPIC "Invalid write", the APIC skipping-interrupts warning).
5. **The `--acpi` flag itself** is a port-side knob (upstream's is a library setting,
   `settings.acpi`); it mirrors the knob's composition exactly, one flag, same arms.
6. **The mutexes are gone** (single-threaded port, M2-1's bus precedent) — `get_apic()`'s
   try-lock unwrap becomes a plain accessor, upstream's `&mut Apic` parameters are kept as
   `apic_state_t *` parameters at every internal boundary (route/check_irq/remote_eoi),
   including across the apic.c ↔ ioapic.c line, which is why the unity guard (no unit static
   referenced across units) passes unchanged.
7. **`register_get_highest_bit` spells out the u64 join** instead of transmuting
   `[u32; 8] → [u64; 4]` (C has no transmute; the join is little-endian by construction).

## 6. WHAT THIS DOES **NOT** MAKE WORK — and one reported incident

- **The IO-APIC is not guest-DISCOVERABLE, and that is a firmware gap, not a device gap.**
  i386 Linux finds an IO-APIC only through an ACPI MADT or an MP table. Upstream v86 has
  NEITHER: `acpi.js` is the PIIX4 PM device only (no table builder), and there is no `mps.js`
  in the tree at all (MEASURED: `ls v86/src/*.js`). Our guest says exactly that:
  *"APIC: ACPI MADT or MP tables are not detected"*, and stays in
  *"virtual wire mode … with no configuration"* — device IRQs on the 8259 in both arms. Making
  the IO-APIC guest-reachable means building firmware tables (an MP floating pointer + table in
  `tests/minibios.asm`, or an ACPI RSDT/MADT builder) — a FIRMWARE deliverable, out of this
  unit's scope, and the next natural slice if the IO-APIC routing is ever to be exercised by a
  guest rather than by the selftest.
- **`/proc/interrupts` is unreachable in this tree's guests**: the bootable images are
  shell-free by decision (the nobb record in gate-m2-5-guest.sh's header), so there is no
  userspace to cat it. The kernel's own dmesg is the testimony instead.
- **The LAPIC timer as the guest's clockevent** is NOT claimed: the ON-arm dmesg names the
  local APIC enabled and our register values read back, but the kernel's clockevent choice
  (LAPIC timer vs PIT) is silent either way, and I did not measure it. What is measured is the
  delivery machinery (the selftest's timer cases) and the guest's use of the device's registers.
- **INCIDENT, reported not hidden:** the FIRST boot-2 run of the reboot-with-`--acpi` workload
  (the unit's first binary, 23:23) died once with SIGBUS/BUS_ADRERR, RIP at a binary-text
  function entry, ~89 s in, after boot 2 was deep into module loading. The same workload then
  passed on every later build: the deferred-`handle_irqs` experiment (×1, confounded — see
  below), `-O1 -g` (×1), `-O2 -g` (×1), two probe links off those objects (×2), and the plain
  rebuilt tree ×16 (6 direct + the 10-run soak, `results/apic-1-soak.txt`). A same-source
  rebuild does not reproduce it, and no layout-preserving perturbation does either; the one
  binary that crashed cannot be regenerated byte-for-byte (cosmocc embeds no build id I can
  pin). MEASURED facts kept: the core (systemd coredumpctl, pid 48872 — SIGBUS BUS_ADRERR,
  RIP 0x40e010 at a function prologue, rsp inside the stack mapping), the stderr (only the
  pre-existing JIT bind-table noise), the serial tail. INTERPRETATION, offered as a hypothesis
  for a reviewer: a layout-sensitive wild access — the newest mid-block state change in the
  tree is `mmap_write32`'s `handle_irqs()` after an APIC-window write, which can deliver an
  interrupt from inside a compiled block's C helper; the pre-existing mid-block delivery sites
  are either block-ending by construction (sti, uop_cache's forced block end) or effectively
  never fire mid-block (a PIC EOI with a line already pending). I did NOT "fix" this by
  changing code — with no reproduction, any change is a guess; the gate evidence and the soak
  are the honest state. The one experiment worth naming for a reviewer: deferring the two
  `handle_irqs()` calls in memory.c made the run pass ONCE, but that pass is confounded (the
  change diverges from upstream AND was a single sample against a one-sample failure).

## 7. RAW EVIDENCE

- `results/apic-1-gates.txt` — make / selftest (all suites) / nasm / transcript-diff /
  determinism, exit codes included.
- `results/apic-1-mut-rig.sh` — the G5 rig (run: all 7 mutations visible, restores green).
- `results/apic-1-g2-off-serial.txt` / `-on-serial.txt` / `-on-apicdebug-serial.txt` — the two
  G2 legs and the `apic=debug` leg with the kernel reading our ID/VERSION.
- `results/apic-1-det-gate-acpi.txt` — `gate-m2-5-determinism.sh all` under the `--acpi`
  wrapper (the byte-identical pair).
- `results/apic-1-det-apic-lapic-diff.txt` — the timer-driven pair's diff (host-epoch audit
  lines only).
- `results/apic-1-soak.txt` — the boot-2 soak (the reboot-with-`--acpi` legs).
