# TODO — the v86 C port: what is left

**Status: the port is COMPLETE for its declared scope.** The CPU core is fully ported (census 0
unimplemented call sites), the devices the baliveau v86 lane needs are in, an x86-64 JIT is
shipped ON by default, and a real i386 Linux kernel — including the baliveau guest, which reaches
`fx-init: boot-ok v3` — boots on it. Every claim in this file is tagged MEASURED / READ /
INTERPRETATION, as the rest of the tree is.

**Gates at the time of writing** (orchestrator-verified, clean build): `make` rc 0 · `make nasm`
118/118 in both cache and JIT modes · the M0 transcript byte-identical to the pinned JS reference
(`8bb8eca5…`) · `make selftest` green (`hw_selftest` 694 checks / 0 failures plus the CPU, m1c, uop,
uop-exec, JIT and JIT-codegen suites) · `make bench` **~0.83 s / ~240 MIPS** on the 200M-instruction
kernel boot · `sh tests/gate-reboot-1.sh` 39/39 · `sh tests/gate-m2-5-determinism.sh all` rc 0 at the
pinned 4,000,024,062 instructions.

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

### The ACPI lane — DONE (ACPI-1 … ACPI-7)

Shipped, all behind `--acpi` (default OFF, and the flag-off machine is byte-identical throughout —
`make transcript-diff` is the first gate of every unit in the lane): the LAPIC + I/O APIC
(`src/cpu/apic.c`, `src/cpu/ioapic.c`), the firmware table set — **RSDP, RSDT, XSDT, FADT, FACS,
DSDT, MADT** at `0xE0000` (`src/hw/acpi_tables.c`, hand-encoded AML; v86 has no analogue) — and the
PCI routing declaration. End to end on the Alpine kernel: PCI enumeration reaches
`PCI root bridge [PCI0]`, the console's IRQ-driven writes land through the IO-APIC, the virtio
console binds and delivers (`11: 26 IO-APIC 11-fasteoi virtio0`), and the `--acpi` reboot arm writes
its disk marker, reboots through the PS/2 controller and reads it back.

The lane's own record, because each cost a wrong claim first:
- **A wrong DECLARATION in firmware is indistinguishable, from the guest side, from a broken device**
  (ACPI-4): the FADT's *hardware-reduced* flag told the kernel this platform had no legacy 8259, so
  the guest dropped its legacy IRQ paths — its console went silent while polled `printk` kept working,
  which read exactly like a UART bug. `preallocated irqs: 0` was the tell.
- **A length field that counts from the wrong byte is invisible to value pins** (ACPI-7): every
  PkgLength in the DSDT counted content instead of *from its own first byte*, so all five packages
  declared extents one or two bytes short. `acpi_tables_verify()` now decodes each one and asserts it
  lands on the true end, **before** any byte-value pin gets a vote.
- **`iasl`/`acpiexec`/`acpidump` do not exist on this host** — the well-formedness claim rests on that
  structural walk, a python re-derivation of the committed bytes, and the reviewer's seabios.bin
  reference walk, not on an external compiler.

**Accepted residual (stated, not hidden):** the DSDT declares no `_PRT` entry for the IDE, whose
interrupt line this machine genuinely does not route through a PIRQ byte — the reboot arm's
`can't derive routing for PCI INT A` for the IDE is that omission working as designed.
**Cross-lane note:** the baliveau v86guest kernel cannot use any of this — its config has no
`CONFIG_ACPI`, no `CONFIG_X86_MPPARSE` and no `CONFIG_X86_IO_APIC` (see §1.1).

---

---

## 1. Deliberately not ported (the devices)

Each of these is *absent by decision*, not by omission, and each currently **stops loudly by name**
rather than producing a wrong value — so the cost of deferring is discovery, not risk.

### 1.1 APIC / IOAPIC — SHIPPED behind `--acpi` (kept as history; the lane's own entry is in §0)
*(Unit APIC-1 shipped the LAPIC/IOAPIC behind `--acpi`, ACPI-2 the firmware tables, and ACPI-4 … 7
closed F1/F2/F3 and the `_PRT` — this historical section records why the seams existed before any of
that landed, and the guest-compatibility fact below still stands.)*

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
  * ~~**F2 — ACPI-aware PCI enumeration finds ZERO devices.**~~ **FIXED (ACPI-4, commit 7bf6bd2 —
    implemented alongside F1 and reported only now, because the session that did both stalled after
    writing its F1 section).** The DSDT now carries one PNP0A03 root bridge under `\_SB` with
    `_BBN=0` and a `_CRS` covering the bus and the I/O and memory apertures. VERIFIED BY THE
    ORCHESTRATOR: under `--acpi` the Alpine kernel prints `PCI host bridge to bus 0000:00` and
    enumerates `00:00.0 [8086:1237]`, `00:01.0 [8086:7000]`, `00:07.0 [8086:7113]`, `00:1e.0
    [8086:7010]` — and the device set is the OFF arm's **plus** `00:07.0`, the PIIX4 ACPI function
    the machine only exposes when ACPI is on, which is exactly right.
  * ~~**F3 — the reboot arm soft-locks.**~~ **BENIGN (ACPI-4): not a machine bug.** The soft lockup
    is the module signature check's sha256 being instruction-slow on this interpreter (>22 guest
    seconds in one kernel loop); the OFF arm shows the IDENTICAL lockup at the SAME EIP in both its
    boots and still completes. Its real blocker was F2 — and with F2 fixed the ON arm now completes
    too: VERIFIED BY THE ORCHESTRATOR, `--acpi` + `lapic`, boot 1 writes the disk marker, reboots
    through the PS/2 controller, and `BOOT2: marker read back from /dev/sda sector 8: DISK PERSIST
    OK`.** So the `--acpi` lane is END TO END: tables -> IO-APIC routing -> console -> PCI ->
    a real guest rebooting itself with persistence. **AND the virtio console too (ACPI-5,
    results/acpi-5.md): the review's N3 gap — every virtio capture to date was image-limited,
    the committed initramfses carrying no virtio modules — is CLOSED with the tree's OWN
    capable image (tests/images/guest-initramfs-virtio.img, the M2-4 gate's): under `--acpi`
    the driver binds (`virtio0 1af4:0003`, hvc0 enabled), INTA delivers through the IO-APIC
    (`11: 26 IO-APIC 11-edge virtio0`), and 2595 bytes of the kernel's own log flow through
    hvc0 (OFF control 2391; captures results/acpi-5-virtio-{on,off}-*.txt). The honest cost,
    measured: the ON arm alone prints `can't derive routing for PCI INT A` / `PCI INT A: no
    GSI` (no _PRT, the acpi_tables.c comment's documented omission) — delivery works anyway
    via the PIIX PIRQ-programmed line, exactly the fallback that review measured.
    **[SUPERSEDED, ACPI-6]** the omission is now closed: the DSDT declares a static `_PRT`
    (the one route the firmware programs: 00:0c.0 INTA -> GSI 11, results/acpi-6.md); the
    two warn lines are GONE and the kernel registers the route itself (`11: 26 IO-APIC
    11-fasteoi virtio0` — level/low via acpi_register_gsi, the signature of the _PRT being
    USED; captures results/acpi-6-virtio-on-*.txt). The same unit fixed the pre-existing
    `[Firmware Bug]: no secondary bus range in _CRS` (a WordBusNumber window in PCI0's
    _CRS, now parsed: `root bus resource [bus 00-ff]`).
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

### 1.3 PAE — SHIPPED (PAE-1, results/pae-1.md; PAE-L3, results/pae-l3.md)
- The 3-level walk, the 64-bit PDPTE/PDE/PTE handling, the 2 MiB page and NX (EFER.NXE) are ported
  (upstream cpu.rs:2054-2336), and `load_pdpte`'s two guest-reachable asserts became `#GP(0)`
  delivered at the load site with validate-before-commit at all three call sites (MOV CR0/CR3/CR4).
  Gated by the PAE selftest battery, the F2 mutation probes, and a synthetic differential guest
  (`tests/pae-guest.img`, byte-identical under both legs).  The port previously trapped-and-aborted
  on the PAE arms rather than half-porting (M1); that trap is gone.
- **Level 3 (the deferred real-kernel boot) is now SHIPPED** (results/pae-l3.md,
  `sh tests/gate-pae-l3.sh`): a pinned Debian `CONFIG_X86_PAE=y` 686-pae kernel boots on BOTH
  legs and the gate asserts the guest's own PAE evidence -- `NX (Execute Disable) protection:
  active` (the PAE-only setup.c string) plus PG set and a non-zero cr3.  The earlier "hang" was a
  too-low instruction limit: the XZ decompression alone is ~2.34G instructions, so the 2G limit
  stopped the run mid-decompress.  Reaching userspace (/init, `/proc/cpuinfo` `pae`) needs ~6.45G
  (past a ~64 s `async_synchronize_full` wait).  The u32 instruction-counter ceiling that once
  blocked that run is GONE (results/u64-jit.md): the counter is now a u64, and the gate's `full`
  arm (`sh tests/gate-pae-l3.sh full`) runs both legs past 4 G and asserts the initramfs's own
  `PAE-INIT` lines directly.

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

- **JIT-lane follow-up found by PAE-L3 — now FIXED (results/u64-jit.md).** A full 6.1 kernel boot
  fills the JIT's 2048-entry block table; the refusal is now printed ONCE, counted in
  `jit_stats.block_table_full` (reported in the stats line), and the stale "JIT off unless
  `--jit on`" comments in `jit_mem.h` are corrected.  The table-size half was MEASURED and recorded
  as a measured negative: growing the table (2048 -> 4096 blocks, 4096 -> 8192 hash slots) is ~7%
  faster on the 3 G PAE boot but just moves the wall to the code-page pool (40,064 `pool exhausted`
  refusals), and growing the pool too is SLOWER (the marginal compiled blocks are cold).  The
  graceful "refuse + interpret" policy sits near the measured optimum.
- **`tests/perf_bench.sh` was blind to a recipe-table change; that blind spot is now CLOSED, and the
  claim that recorded it was wrong in both directions.** The jcc array-slot swaps are the instrument:
  `jitcg_jcc8_fns`/`jitcg_jcc32_fns` are indexed by the condition nibble, so swapping two entries
  substitutes one condition's recipe for another with no other trace. MEASURED on this tree
  (2026-10-15, `jit_codegen.c` `fec58189…`, every variant installed alone and re-restored by `cmp`):

  | variant | `jit_codegen_selftest` | `perf_bench.sh` 200 M |
  | --- | --- | --- |
  | `jb↔jbe` rel8 | 16 failures, rc 1 | **rc 1** — 280,921 instructions at the limit |
  | `jae↔ja` rel8 | 24 failures, rc 1 | **rc 1** — 200,097,867 |
  | `jb↔jbe` rel32 | 16 failures, rc 1 | **rc 1** — 530,745 |
  | `jae↔ja` rel32 | 16 failures, rc 1 | **rc 1** — 542,688 |
  | `jo↔jb` (cross-class) | 3 failures, rc 1 | **rc 0 — BLIND** |
  | `js↔jbe` (cross-class) | 2 failures, rc 1 | **rc 0 — BLIND** |
  | none (baseline) | rc 0, 3525 checks / 0 failures | rc 0, 200,095,214 |

  So the previous claim here — "blind to swapping two non-inlined jcc recipe slots; the inlined slots
  and the *suite* differ, so the class is partly covered" — inverted its own measurement: the bench
  **catches all four** of those swaps (rc 1, on the instruction count alone; results/jit-fix-2's
  counts reproduced exactly here), and what it is blind to is the class that crosses the **inline
  boundary** (an inlined condition's slot against an unsupported one's), where the guest's count,
  terminal state *and* serial sha all stay at their pins.
  **Closed** by pinning the JIT's own inlining line, the one thing that moves for that class:
  `195429258 / 4665955 / 8330 / 3719` (inlined / called / with-a-recipe / without) becomes
  `195416212 / 4679001 / 8310 / 3739` under `jo↔jb`. The pin is asymmetric — the mutant fails with it
  and was blind without it — and a `--jit off` leg pins `0/0/0/0` instead, so the instrument also
  asserts that the JIT really did nothing (`--expect-jit-stats`).
- The lesson the JIT lane paid for, kept here so it is not relearned: **the JIT's high-byte blocker
  shipped through every gate green** (118/118, byte-identical transcript, kernel boot, whole
  selftest suite). A second execution path needs instruments of its own, not just the ones the
  first path earned. **The corollary this unit adds:** a *performance* pin sees what the
  guest-visible pins cannot, and vice versa — which is why the bench now carries one of each.

## 5. Documentation

- `README.md`'s Layout/Build sections were the M0-era text (they called `src/main.c` and `gen/`
  "placeholders"); **CORRECTED** — rewritten to describe the shipped port, with every command in it
  dry-run-verified to exist and the non-goals table fixed (it had listed the JIT as out of scope,
  which stopped being true when the native JIT landed). The rest of the README (the ORACLE
  CONTRACT, the boot path, the transcript) had stayed accurate throughout.
- **The docs had drifted behind the ACPI lane, and did not say so** (found by the orchestrator's
  2026-10-15 inventory, then fixed): `docs/architecture.md` §5 listed only `acpi.c` even though
  `acpi_tables.c`, `src/cpu/apic.c` and `src/cpu/ioapic.c` had all shipped — the ACPI lane now has
  its own subsection there (the tables, the LAPIC/IOAPIC, the declaration-is-part-of-the-machine
  lesson, and the IDE `_PRT` omission). `README.md`'s device row and Layout now name the tables and
  the APIC behind `--acpi`, and this file's own gates header had still quoted the pre-ACPI-7
  `make bench` number (1.68 s / 119 MIPS) against the shipped **0.83 s / 240 MIPS**.
  The lesson is the general one: a lane that lands behind a default-OFF flag does not look like a
  doc change, so nothing prompts the update.

## 6. On the baliveau side (the consumer) — for reference, not this repo's work

`baliveau/todo.md` §7's v86 lane: the purpose-built i386 kernel ✓, `/dev/hda` (MEASURED: the device
is `/dev/sda`) in fx-init ✓, harness wiring with an in-place reboot proof ✓, and **determinism ✓**
(`results/m2-5-determinism.md`: the RTC was the last host-entropy site a guest could read, and it
now runs on this port's own virtual clock at a fixed epoch — post-fix boots are byte-identical in
serial, virtio **and instruction count** across 8 boots, where 6 pre-fix boots produced 6 different
counts).

## 7. Repository weight — the 64 MiB test disk, MATERIALISED not tracked

`tests/images/disk-fmt-64m.img` (67,108,864 bytes) was tracked in git and had crossed GitHub's 50 MB
blob warning. Git now tracks its `xz -9` instead — `tests/images/disk-fmt-64m.img.xz`, **10,240
bytes**, MEASURED — and `tests/disk-fmt.sh` materialises the raw image on first gate use, verifying
the decompressed bytes against the pin `a2f0f6da…0460c` *before* anything is put in place (temp file,
sha256 check, atomic rename), so a bad or truncated archive fails the gate loudly and leaves no
half-written image. `make disk-fmt` does it by hand; it is deliberately **not** a prerequisite of
`make`/`make all`, and `make -n all` proves the default build never references the disk.

Both consumers — `tests/gate-m2-5-determinism.sh` and `tests/gate-m2-5-guest.sh` — source the helper
three lines after their `DISK_FMT` default, so a fresh clone runs them unchanged, and the
`DISK_FMT=` override is still honoured (a caller's path is never materialised into).

**STATED GAP, not hidden:** the 67 MB blob is still reachable in git history, so the GitHub warning
is not fully retired by this change — retiring it needs a history rewrite and a force-push, which is
the repository owner's call, not a unit's. What this change fixes is the *tip*: no new clone, fetch
or pack in normal use has to carry it.

## 8. Long mode (64-bit guests) — **one day**, not a non-goal

Not implemented, and deliberately **not** listed as a non-goal: it is wanted one day. It is recorded
here so the next reader inherits the reconnaissance instead of re-deriving it.

**WHY IT IS A DIFFERENT KIND OF JOB: THE REFERENCE HAS NO LONG MODE (MEASURED).**
`v86/src/rust/` contains no `EFER_LME`, no `EFER_LMA`, no `CR4_LA57` and no `PML4` anywhere; the only
EFER bit upstream knows is `EFER_NXE` (`v86/src/rust/cpu/cpu.rs:244`), which PAE-1 wired up. Its
dispatch table is 32-bit by construction — `v86/gen/x86_table.js:78` encodes opcode `0x63` as `arpl`
(in long mode that byte is `MOVSXD`) and `0x40`–`0x4f` as `INC`/`DEC`, not REX prefixes — and this
port's dispatch (`src/cpu/interpreter_gen.c`, 14,727 generated lines / 1922 call sites) is
**generated from that table**. So there is no source for the 64-bit forms to be generated from.

**THE ANCHOR, NOT THE CODE, IS THE REAL COST.** Every claim this tree makes rests on the M0 contract:
a byte-identical transcript against the pinned `v86@0.5.471` (`oracle/run.mjs`, sha `8bb8eca5…`). A
reference with no long mode gives long mode **no oracle**, so the work would begin with acquiring and
justifying a second reference (QEMU's TCG? a 64-bit `nasm` corpus? that corpus upstream does not
have) — a new verification method, which is larger than the code it would verify.

**WHAT EXISTS THAT MAKES IT HARD (all READ from this tree).** The shapes are 32-bit end to end:
`int32_t reg32[8]` and `int32_t dreg[8]` (`src/cpu/cpu_state.h:249,287` — no R8–R15, no 64-bit GPRs),
`int32_t instruction_pointer` (`:263`), `do_page_walk(int32_t addr)` (`src/cpu/paging.c:119`), a TLB
indexed by a 32-bit virtual page, and a 32-bit physical address space throughout. The CPU is 58,579
lines, 20k of it generated.

**THE SKETCH, IF IT EVER STARTS.** REX and 64-bit operand-size decoding plus 64-bit semantics for the
whole instruction set (a hand-written table, since the generator has none); a widened register file
with the FS/GS base MSRs and long mode's flat segment model; **4-level paging with 48-bit virtual
addresses**, which changes the address type in the memory path, the TLB key, `memory.c`'s fast paths
and both JIT recipe classes; and the new system instructions (`SYSCALL`/`SYSRET`, `SWAPGS`, the
long-mode MSRs). INTERPRETATION: months, and larger than any lane in this file, because the 32-bit
core was *ported* with an oracle over ~10 units, while this would be *authored* without one.

**IT BECOMES WORTH STARTING WHEN** a guest that genuinely requires 64-bit appears in one of the futaie
lanes — and note that nothing does today: the baliveau v86guest kernel is 32-bit non-PAE
(`CONFIG_PGTABLE_LEVELS=2`), and the browser lane is v86 itself, which is equally 32-bit. The honest
shape then is a 64-bit core with its own oracle, not a mode bolted onto this one.
