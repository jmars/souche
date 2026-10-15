# TODO — the v86 C port: what is left

**Status: the port is COMPLETE for its declared scope.** The CPU core is fully ported (census 0
unimplemented call sites), the devices the baliveau v86 lane needs are in, an x86-64 JIT is
shipped ON by default, and a real i386 Linux kernel — including the baliveau guest, which reaches
`fx-init: boot-ok v3` — boots on it. Every claim in this file is tagged MEASURED / READ /
INTERPRETATION, as the rest of the tree is.

**Gates at the time of writing** (orchestrator-verified, clean build): `make` rc 0 · `make nasm`
118/118 in both cache and JIT modes · the M0 transcript byte-identical to the pinned JS reference
(`8bb8eca5…`) · `make selftest` green (~8M checks across six suites) · `make bench` ~1.68 s /
~119 MIPS on the 200M-instruction kernel boot.

---

## 0. Recently closed (kept as history, not as work)

### The machine reboot path — DONE (REBOOT-1 + review + REBOOT-FIX-1)
Shipped: `machine_reboot()` (`src/hw/machine.c`) — `reset_cpu()` then the devices in the
reference's own order (virtio_console, ps2), plus pic/pit/rtc on the firmware-difference
argument (minibios programs neither; the review confirmed the table leak-free) — the two
trigger sites (`src/hw/pci.c:99`, `src/hw/ps2.c:620`) now reboot, and each boot path re-enters
itself (bzImage re-runs its loader, multiboot rebuilds the handoff, the firmware path re-runs
bring-up). The unwind: the interpreted executors check `reboot_requested` per uop, the
compiled arm checks it on every exit kind (REBOOT-FIX-1), `reboot_prepare_reentry` clears the
code caches between the unwind and boot 2's first instruction. Gated: `tests/gate-reboot-1.sh`
39/39 including a real i386 Linux guest rebooting itself with the disk write surviving, and
two asymmetry images (`tests/reboot-geom.asm`: the guest-reachable progress-assert corner,
MEASURED aborting pre-fix; `tests/reboot-fwpage.asm`: the same-page-trigger tripwire).
Report: results/reboot-1.md; review: results/reboot-1r-review.md (no blockers);
fixes: results/reboot-fix-1.md.

**Accepted residuals (stated, not hidden):**
- the UART is NOT reset (matches the reference; a reboot-time re-init would leak a device and
  five listeners per boot — machine.c's header carries the argument);
- the bzImage re-entry RE-RUNS the loader because a self-decompressing kernel destroys its own
  compressed image in RAM (a measured divergence from "kernel not re-loaded"; boot-1 writes
  into the initrd region are lost, disk buffer untouched);
- the JIT-cache clear stands on the reference's behaviour plus the
  uop_cache_clear→jit_code_clear_all pair; it is not image-visible (the mutation rig's M5
  honestly NOT-CAUGHT);
- the JIT bind-table-full stderr noise (~1.6M lines per double 4G boot) is pre-existing.

---

---

## 1. Deliberately not ported (the devices, and PAE)

Each of these is *absent by decision*, not by omission, and each currently **stops loudly by name**
rather than producing a wrong value — so the cost of deferring is discovery, not risk.

### 1.1 APIC / IOAPIC — deferred; NOT needed by any guest we run today
*(Unit APIC-1 has since shipped the LAPIC/IOAPIC behind `--acpi`, and ACPI-2 the firmware
tables; this historical section records why the seams existed before either landed.)*

- **CROSS-LANE RECOMMENDATION (ACPI-2, MEASURED):** the baliveau v86guest kernel
  (`tests/images/vmlinuz-i386-v86guest`, 6.12.19) can never route through the I/O APIC — its
  config (in the pinned tarball, hash-tied to the bzImage) has `# CONFIG_ACPI is not set`,
  no `CONFIG_X86_MPPARSE`, no `CONFIG_X86_IO_APIC` (nor `CONFIG_SMP`/`CONFIG_X86_UP_APIC`):
  `NR_IRQS: 16`, every `/proc/interrupts` line is `XT-PIC`, with or without firmware tables.
  If that lane ever wants IO-APIC routing, its config needs `CONFIG_ACPI=y` (or
  `CONFIG_X86_MPPARSE=y`) **and** `CONFIG_X86_IO_APIC=y`; the consumer to copy the gate from is
  `results/acpi-2.md`, which proves the route end-to-end on the tree's OTHER kernel,
  `tests/images/vmlinuz-i386` (Alpine 6.6, ACPI+MPPARSE+IO_APIC in). Evidence:
  `results/acpi-1.md` §1 (the three-source capability answer), `results/acpi-2.md` (the route).
- **WHAT `--acpi` STILL NEEDS:**
  * ~~**F1 — userspace console writes never reach the UART under `--acpi`.**~~ **FIXED
    (ACPI-4, commit 7bf6bd2).** It was NOT the UART: ACPI-2 had shipped the **FADT with its
    hardware-reduced flag set**, which tells the kernel the platform has no legacy 8259/PIC — false
    for this machine, so the kernel dropped the legacy IRQ paths and the IRQ-driven tty write never
    landed (the polled printk kept working, which is why it looked like a device bug;
    `preallocated irqs: 0` was the tell). The fix is one FADT flags word, and the machine now takes
    the IO-APIC route end to end: the init's full output is identical in the ON and OFF arms, and
    the guest's own `/proc/interrupts` shows `4: 10 IO-APIC 4-edge ttyS0`. **The lesson: a wrong
    declaration in firmware is indistinguishable, from the guest side, from a broken device — the
    kernel behaves correctly for the machine it was TOLD it is, so table CONTENT is part of the
    emulated machine and needs the same scepticism as a device register.**
  * **F2 — ACPI-aware PCI enumeration finds ZERO devices: STILL OPEN.** The minimal DSDT declares no
    PCI root bridge (`_HID` PNP0A03 + the resource descriptors a kernel reads), so virtio and IDE
    vanish under `--acpi` — and it is also what blocks the reboot arm (F3 below). This is the one
    remaining item.
  * **F3 — the reboot arm: RE-TESTED AND BENIGN in both arms** (ACPI-4); its real blocker is F2.
  * Note the session that fixed F1 (ACPI-4) **stalled after writing its findings** — the provider
    fault (`reasoning_content`) that has killed five sessions — so the orchestrator verified its
    fix against its own captures, ran the battery and committed it. Its report is
    `results/acpi-4.md` as it stood at the stall point.
- **Size.** READ from the reference: `v86/src/rust/cpu/apic.rs` 658 lines + `ioapic.rs` 316 = 974
  lines of Rust → roughly **1000–1200 lines of C plus wiring** — one solid unit, the same shape as
  the PIC (402 Rust → 491 C) or the PIT (359 JS → ~250 C). Contents: the LAPIC's 4 KiB register
  block + `apic_timer` (a third tick source) + the delivery path (`route`/`deliver`/`highest_irr`/
  `acknowledge_irq`), and the IOAPIC's indirect register block + routing table + `set_irq`/
  `clear_irq`.
- **The seams already exist.** The port kept `APIC_MEM_ADDRESS`/`IOAPIC_MEM_ADDRESS`, the mmio
  routing points, the `apic_enabled`/`acpi_enabled` state, the CPUID APIC bit and the
  `IA32_APIC_BASE` MSR. The two places the IOAPIC would be called **stop by name**:
  `src/hw/machine.c:149` (`device_raise_irq: ioapic::set_irq`) and `:159` (`device_lower_irq`).
- **NOT needed today — MEASURED, from the guest's own dmesg** (both the Alpine boot and the
  baliveau guest): *"no APIC, boot with the \"lapic\" boot parameter to force-enable it."* Linux
  found no local APIC, fell back to the 8259, and booted to `fx-init: boot-ok v3`.
- **It becomes worth building when**: a guest kernel *requires* a LAPIC (the `lapic` boot parameter
  forces that path, and some configs drop the 8259 fallback), or someone wants the LAPIC timer as
  the tick source instead of the PIT.
- **Recommended first slice if it is ever built**: *presence plus the timer* (the LAPIC register
  block + `apic_timer`) — **not** the full IOAPIC routing, because single-CPU means the routing
  table's only consumer is the PIT/RTC/ACPI IRQ fan-out the port already drives through `set_irq`.
  INTERPRETATION: that is a smaller job than the 974-line total and would satisfy every guest we
  can currently name.

### 1.2 VGA — deferred; the one device the browser lane has that this port lacks
- **Size.** READ: `v86/src/vga.js` is 2,612 lines — **the largest single unported device**, and the
  only one a *visual* browser-lane demo needs and the native lane does not.
- **What exists**: the VGA window (`0xA0000-0xBFFFF`) and the 8 MiB LFB are routed through the
  device-hook table and read the zeroed VGA buffer (R1F's carve-out, MEASURED against the
  reference); the dirty-bitmap callback is stubbed.
- **It becomes worth building when** a graphical demo, or a guest that programs VGA registers, is
  wanted. Nothing on baliveau's console+disk path needs it.

### 1.3 PAE — deferred, and trapped rather than half-ported
- The port stores `reg_pdpte` and **traps explicitly** on the PAE arms of the page walk instead of
  misreading a 64-bit PTE. The guest is 32-bit non-PAE. Upstream itself asserts 32-bit-only PDPTEs.

## 2. JIT headroom (the plan's step 8) — CLOSED by JIT-6

`results/jit-plan.md` §7 step 8 was gated on §8's ship rule (≥1.3×); it passed, and all three items
have now been decided by MEASUREMENT (results/jit-6-headroom.md, results/jit-6r-review.md,
results/jit-6f.md):

| step-8 item | verdict |
| --- | --- |
| **block chaining** | **LANDED** — −17.0% best / −15.5% median against an interleaved baseline; the staircase is unchanged (identical flush/abort/block-end counts and the pinned instruction count 4,000,053,925) |
| **platform probe** | **LANDED** — a reserve/publish/execute canary at `jit_mem_init`; the failure path disables the JIT, reports it, and leaves the machine reference-identical |
| **lazy-flag register caching** | **NOT BUILT — a measured negative**: gross ceiling 0.8–1.3% end-to-end, and the two host registers it needs cost more than the ceiling |

Recorded because it is the durable lesson: the chaining review found the abort fallback correct only
because `jit_chain_target`'s **same-page gate** is load-bearing — the comment claimed a mechanism
that never ran and the seat it named was never called. The invariant is now written at the abort
site; widening the chain across pages would silently break both engines.

## 3. Portability caveats — stated, not hidden

- **The JIT is x86-64 only.** On aarch64 it is compiled out (`V86C_JIT_X64`) and the uop cache
  carries the machine; `--jit on` reports unavailability. A second backend is explicitly not
  planned. **The aarch64 APE therefore runs ~1.7× slower than the amd64 one** — PROJECTION from the
  measured JIT gain, not a measurement made on aarch64 hardware.
- **macOS / Windows W^X: PROBED, still UNVERIFIED on real hardware.** JIT-6 added the platform
  probe (a reserve/publish/execute canary at `jit_mem_init`); its failure path is TESTED, so an
  unsupported platform now disables the JIT, says so, and runs the uop cache instead of silently
  trying. What remains unverified is the SUCCESS path there — no macOS or Windows host has run it.
  Force the failure arm with `V86C_JIT_PROBE_FAIL=1`.

## 4. Instruments — known blind spots

- `tests/perf_bench.sh` is **blind to swapping two non-inlined jcc recipe slots** (MEASURED by
  JIT-FIX-2); the inlined slots and the *suite* differ, so the class is partly covered.
- The lesson the JIT lane paid for, kept here so it is not relearned: **the JIT's high-byte blocker
  shipped through every gate green** (118/118, byte-identical transcript, kernel boot, whole
  selftest suite). A second execution path needs instruments of its own, not just the ones the
  first path earned.

## 5. Documentation

- `README.md`'s Layout/Build sections were the M0-era text (they called `src/main.c` and `gen/`
  "placeholders"); **CORRECTED** — rewritten to describe the shipped port, with every command in it
  dry-run-verified to exist and the non-goals table fixed (it had listed the JIT as out of scope,
  which stopped being true when the native JIT landed). The rest of the README (the ORACLE
  CONTRACT, the boot path, the transcript) had stayed accurate throughout.

## 6. On the baliveau side (the consumer) — for reference, not this repo's work

`baliveau/todo.md` §7's v86 lane: the purpose-built i386 kernel ✓, `/dev/hda` (MEASURED: the device
is `/dev/sda`) in fx-init ✓, harness wiring with an in-place reboot proof ✓, and **determinism ✓**
(`results/m2-5-determinism.md`: the RTC was the last host-entropy site a guest could read, and it
now runs on this port's own virtual clock at a fixed epoch — post-fix boots are byte-identical in
serial, virtio **and instruction count** across 8 boots, where 6 pre-fix boots produced 6 different
counts).
