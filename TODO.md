# TODO — the v86 C port: what is left

**Status: the port is COMPLETE for its declared scope.** The CPU core is fully ported (census 0
unimplemented call sites), the devices the baliveau v86 lane needs are in, an x86-64 JIT is
shipped ON by default, and a real i386 Linux kernel — including the baliveau guest, which reaches
`fx-init: boot-ok v3` — boots on it. Every claim in this file is tagged MEASURED / READ /
INTERPRETATION, as the rest of the tree is.

**Gates at the time of writing** (orchestrator-verified, clean build): `make` rc 0 · `make nasm`
118/118 in both cache and JIT modes · the M0 transcript byte-identical to the pinned JS reference
(`8bb8eca5…`) · `make selftest` green (`hw_selftest` 778 checks / 0 failures plus the CPU, m1c, uop,
uop-exec, JIT and JIT-codegen suites) · `make bench` **~0.83 s / ~240 MIPS** on the 200M-instruction
kernel boot · `sh tests/gate-reboot-1.sh` 39/39 · `sh tests/gate-m2-5-determinism.sh all` rc 0 at
**4,000,096,757** instructions.

**That count moved on VGA-U0 and the move is a STRENGTHENING, not a re-pin to hide a regression**
(recorded here because it will be the next reader's question). It was 4,000,024,062; the delta is
+72,695, and it is the kernel enumerating the VGA's PCI function `00:12.0`, which the REFERENCE leg
always had and the C leg did not. MEASURED on both sides of the change and independently by the
review: the C leg's serial now carries the same 7 `00:12.0` lines the JS leg's serial always
carried, and the C-vs-JS leg diff (`results/m2-5-guest-leg-diff.txt`) shrank to message-ORDER jitter
alone — the legs converge. The count reproduces across runs. Two honest caveats: it is a **C-leg
determinism record** (the gate compares run1 against run2; there is no absolute pin in code), and it
is **not** a cross-leg count-equality claim — the JS leg has never been run at this exact config
(the plan's `--vga-doc` document diff is the cross-leg comparison for this lane, not the count).

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

### 1.2 VGA — SHIPPED (U1/U2/U3/U4; results/vga-u1.md, vga-u2.md, vga-u3.md, vga-u4.md)
- **Size.** READ: `v86/src/vga.js` is 2,612 lines — the largest single device in the reference. It
  is being ported in five units (the ladder `coder` → `engineer`), each with its own gate, because
  the decode is where a silent-wrong port costs most.
- **What exists now (U0+U1+U2+U3)**: the device is ALWAYS installed, as upstream (`cpu.js:1193`) —
  the 8 MiB VRAM (so `memory.c`'s LFB arms are live), PCI `00:12.0`, the RENDERING SEAM with three
  adapters (headless / ANSI / DOCUMENT), **U1's text-mode subset** (the CRTC, the attribute
  controller, the DAC, `text_mode_redraw`/`put_char`, the window's text handlers), **U2's mode
  13h + graphical document**, and **U3's planar 16-colour path, proven arm by arm**: read modes
  0/1, write modes 0-3 with rotate/expand/setreset/logical/bitmask, chain-2 odd/even, the shift
  modes 0x00/0x20, the latches, odd/even host read, the colorset arm, and the row-substitution
  arm — gated by `tests/gate-vga-3.sh` (four self-programming images, byte-identical on both legs)
  and the `hw_selftest` case 9e probes (14 probes, each with a measured fail-before/pass-after
  mutation).  The D2 differentials (`sh tests/gate-vga-1/2/3.sh`) prove both legs produce
  byte-identical documents on images that paint known patterns and read back in-guest.
- **What U4 added, and how it is gated**: the Bochs VBE dispi interface (0x1CE/0x1CF, registered
  16-bit-only exactly as upstream), `svga_enabled`/`svga_width`/`svga_height`/`svga_bpp`/
  `svga_bank_offset`/`svga_offset[_x/_y]`, bank switching through the 0xA0000 window, the
  8/15/16/24/32-bpp LFB->RGBA fill (the reference's own byte-lane shuffle and 5-6-5 / 5-5-5
  extraction, including its BGR order), the LFB at 0xE0000000 (U0's arms made live by the real
  dirty bitmap), and the REAL `svga_mark_dirty` dirty bitmap replacing the `memory.c` stub (the
  rust `svga_fill_pixel_buffer` port, one bit per 4 KiB page).  Gated by `tests/gate-vga-4.sh`
  (the self-programming `tests/vga-lfb.img`, byte-identical on both legs) and the `hw_selftest`
  case 9f probes (dispi round-trip, five bpp arms, the banked window, the svga_offset page flip,
  and the disable + same-size re-enable lifecycle arms -- each with a measured fail-before/
  pass-after mutation).  The lane's hardest requirement is
  OUTPUT INVARIANCE: the dirty bitmap must be invisible to the differential (the reference's own
  redraw is a full recompute), and the byte-identical D1 proves it by construction.
- **The guest-gate boundary re-verified at U1, U2 AND U3**: the kernel boot touches the VGA (the
  firmware's `INT 10h` teletype writes ~8k window bytes at `0xB84C0`), and U1/U2/U3 claimed the
  CRTC, sequencer and graphics-controller ports — so the ports' answers changed from `0xFF` to the
  device's.  The guest gates were re-run on U3's binary and **nothing moved**: the determinism
  count is still **4,000,096,757** (both runs), the bench pin **200,095,214** holds,
  `gate-m2-5-guest`, `gate-pae-1`, `gate-pae-l3` and `gate-reboot-1` are all green.  The legs
  CONVERGE (the kernel's `vgacon` probe now sees the reference's CRTC/sequencer/graphics answers,
  which is what the JS leg always answered).

### 1.3 PAE — SHIPPED (PAE-1, results/pae-1.md; PAE-L3, results/pae-l3.md)
- The 3-level walk, the 64-bit PDPTE/PDE/PTE handling, the 2 MiB page and NX (EFER.NXE) are ported
  (upstream cpu.rs:2054-2336), and `load_pdpte`'s two guest-reachable asserts became `#GP(0)`
  delivered at the load site with validate-before-commit at all four call sites (MOV CR0/CR3/CR4
  and the hardware task switch, whose PDPTE load is hoisted above every memory and register
  commit, so a bad-PDPT switch is fully atomic -- results/followups.md).
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

**S0 IS LANDED (2026-10-19): THE BACKEND SEAM, INERT.** `results/x86-64-plan.md` is the plan of
record (fourteen slices, S0-S13); `results/x86-64-s0.md` is S0's report.  What exists now: the
engine contract (`src/cpu/cpu_backend.h`: name, `modes`, init/run_batch/clear_caches/print_stats),
`cycle_internal` routing through the selected backend's `->run_batch`, the three existing engines
behind it as three INSTANCES of one axis (`interp` = `interpreted_run`, `uop` = `uop_run` with the
compiled path held down for that instance, `uopjit` = `uop_run` — the JIT is a property of a block
record, not a third peer), `--cpu-engine auto|interp|uop|uopjit` with `--uop-cache`/`--jit` kept as
ALIASES (every pre-seam invocation's behaviour and stderr unchanged), and the engine's identity
gated two ways: `cpu_selftest`'s `test_cpu_backend` (resolver, `run_batch` symbol by pointer
identity, the flags round trip, the dispatch actually called) and `make engine-identity` (the
binary's own counters: `lookups 0 / 130 / 133`, `compiled 0 / 0 / 4`).  The whole battery was
unmoved — transcript `8bb8eca5…`, determinism 4,000,096,757 both runs, bench 200,095,214, nasm
118/118, reboot 39/39, the five VGA gates, PAE-1 11/0, PAE-L3 both legs — and a 3G-instruction PAE
kernel boot is byte-identical across all four selections.

**S1 AND S2 ARE LANDED (2026-10-19): THE WIDENING (INERT) AND THE REFUSALS.**
`results/x86-64-s1-s2.md` is their report.  S1: the register file is `uint64_t reg64[8]`, the lazy
operands `int64_t`, `OPSIZE_64`/`CACHED_MASK_LONG` exist, the byte/word index formulas moved with
the element size, and `write_reg32` zeroes the high half (the JIT's spill agrees).  The lazy
operands gained a **canonical storage form** (masked to the operand width) because the JIT and the
interpreter are compared byte for byte and do not sign-extend alike.  Its gate is the whole battery
**plus a new poison probe** (`--poison`, `make poison-probe`): seed every widened field's high half
with `0xA5A5A5A5` and the M0 transcript must be byte-identical — it is.  S2: `cpu_mode_of` gained
its IA-32e arm, `EFER_LME`/`LMA`/`SCE` are defined, and the plan's **C5 is CLOSED**: all THREE
writes that can enter IA-32e (`WRMSR(EFER)` with LME, `CR0.PG`, `CR4.PAE` — the plan names two,
`MOV CR4` reaches the same `load_pdpte()`) now stop **by name** and commit nothing, and
`CPUID.80000001h:EDX`'s `lm`/`syscall` bits and `80000008h`'s linear width are gated on
`cpu_backend_can(CPU_MODE_LONG)` (today: clear, 32 — the pinned reference's own values).  Both
slices moved nothing else: transcript `8bb8eca5…`, determinism 4,000,096,757 both runs, bench
200,095,214 with the JIT inlining pins, nasm 118/118, reboot 39/39, the five VGA gates, PAE-1 11/0,
PAE-L3 both legs; `cpu_selftest` 3646 → 3846 checks.  NOT landed and named in the report: the
`EFER.LMA` transition (S5/S11), `CPU_MODE_COMPAT` (needs `CS.L`), `pdpe1gb` (deliberately not
declared before 1 GiB pages exist), R8-R15 (no field yet).

**S3 IS LANDED (2026-10-19): THE ADDRESS AND IP WIDENING (INERT).**  `results/x86-64-s3.md` is its
report.  `instruction_pointer`, `previous_ip`, `last_virt_eip`, `eip_phys`, `idtr_offset`,
`gdtr_offset` and `segment_offsets[8]` are 64-bit in place; `translate_address*` take a 64-bit
address and return the model's 32-bit physical one **at one named projection** (plan §5.1);
`get_seg_cs`/`get_seg_ss` and `seg_descriptor_base` are unsigned; the uop block key carries a 64-bit
virtual half (the hash folds both halves) and `uop_t.eip`/`body_ip` widened with it; the JIT's five
`eip` stores are 8-byte (a 4-byte store into an 8-byte field leaves a stale high half, and the
sign-extending `mov r/m64, imm32` is wrong for this value class).  The slice added the plan's **"the
high half is zero outside long mode" invariant** — `cpu_address_widening_leak()` names the offending
field, `cpu_assert_address_widening()` turns it into the machine's named stop, called at **all four mode
boundaries** (reset, CR0, CR4, EFER — each exercised through its guest-reachable body) **and at both
ends of every batch** — and extended the **poison probe** over the new fields (the two instruments are each other's control: the probe disarms the invariant, because it
seeds on purpose what the invariant forbids).  Every producer of a new eip goes through
`set_instruction_pointer()`, which projects outside long mode and does not in it; that is what keeps
the invariant a detector rather than a tautology.  The gate: the whole battery **unmoved** —
transcript `8bb8eca5…`, determinism 4,000,096,757 both runs, bench 200,095,214 with the JIT inlining
pins, nasm 118/118, reboot 39/39, the five VGA gates, PAE-1 11/0, PAE-L3 both legs — plus the poison
probe byte-identical and `cpu_selftest` 3846 → 3873 checks.  **THE WIDENING'S OWN FINDS, all caught
by existing instruments and worth carrying**: a **4-byte stride on the widened `segment_offsets[]`**
in the JIT's segment-base add (the compiled arm silently added the wrong segment's base — the prefix
pin went red in one engine only); a **width change nobody wrote** in the JIT's ip arithmetic
(all-`uint32` before, so a rel8 `-2` was two's complement, and widening `uop_t.eip` turned it into
`+4294967294`); and `eip_phys` **missing from `reset_cpu`**, which the new invariant named.  NOT
landed, named in the report: the JIT's ip *comparisons* and its own key stay 32-bit (S13), the
decoder's addressing forms and the stack helpers stay 32-bit (S5/S7), the page walk stays 32-bit
(S11), `prefixes`/`cr[2]`/`cr[3]` unchanged by decision.  THE ONE EXCEPTION TO "INERT", declared and
tested: the decode window's end used to wrap to 0 at the top virtual page (0xFFFFF000..0xFFFFFFFF),
so every block there was refused and the interpreter ran; it is now a real window, so a guest that
pages the last page into RAM gets the cached/compiled engine — `uop_cache_selftest`'s top-page arms
and `jit_codegen_selftest`'s three-arm `test_last_page` pin it (state identical, `decodes 0/1/1`,
and with the pre-fix expression restored the counters go to 0 while the state still agrees).

**S4 IS LANDED (2026-10-19): THE HOST-CORPUS INFRASTRUCTURE — AND ITS GATE IS NEGATIVE.**
`results/x86-64-s4.md` is its report.  What exists now: `tests/mkfixture64.c` (the native leg — it
`mmap`s the flat image at its link base `MAP_FIXED_NOREPLACE`, zeroes the GPRs and the FPU state, and
jumps to the body under ptrace, so **the harness is the loader** and no shipped `load_elf` code
changed), `tests/nasm64_run.py` (the differential runner: the 32-bit comparison's logic on the v2
layout), `gen/nasm64_gen.py` + `tests/nasm64/*.asm` (a **generated** 57-case corpus over the six ISA
families S5-S10 will land: mov 12, alu 10, ctrl 12, muldiv 11, strobits 6, simd 6 — every case
required to contain a construct that DISCRIMINATES 32-bit from 64-bit execution, and the generator
refuses one that does not), `tests/nasm64.ld` (`.stub@0x80000`, `.body@0x90000`, `.bss@0x100000`
NOLOAD), the hand-written `tests/nasm64_control.asm`, and `--fixture64` in `src/main.c` (one printer,
two documents: the 16-GPR/`rip` v2 array plus a `---BEGIN STATE---` control tail whose consumers are
S13's engine differential and the KVM lane's state translation; the 32-bit document's bytes are
unchanged, which §4's shape step pins rather than asserts).  **THE GATE (`make nasm64` = `tests/gate-nasm64.sh`, 11 steps, each one's exit code):** the v2
printer's shape (2116/2124 elements, same registers, R8-R15 zero, same rip, identical
FPU/memory/flags block); the harness's own control against an independent Python model (27/27
clauses — the `loop` breakpoint, the 8 KiB window, the x87 read, all predicted from the source's
constants); the **witness rule's own battery** (20 verdicts: the rule must refuse a wholly 32-bit
body); the comparison CAN pass (57/57 green with the native leg on both sides); **one perturbed
value per fixture is caught and NAMED** (a register word 55/55, a comparison-window word 55/55, the
fault cases' `rip` 2/2); and **the RED census AS COUNTS** — `green=0 red_unimplemented=57
mismatch=0 error=0`, every case stopping with a named `unimplemented:` (`EFER.LME asks for IA-32e …
cannot run long mode`), because the machine cannot execute 64-bit code yet and a green corpus today
would have tested nothing — plus that assertion's own **able-to-fail arm** (wrong counts must be
refused) and the **not-red arm** (a bare `--stage run` against a document-producing leg must return
a SUMMARY and green=57).  S4's own finds, all caught by the new instrument: the stub's `EFER.LME`
written as bit 0 (i.e. `SCE`), which made the port run the whole stub into the plan's **C5**
wrong-table walk and end in `SIGBUS`; the generator emitting each case's data BEFORE `loop:` (3 of
57 cases fell through into their `.quad` bytes); the trampoline's register allocation jumping to 0;
the host's inherited FPU state (the `memcpy`'s junk in `xmm0-15`).  **ROUND 2** (`results/x86-64-s4.md`
§9) fixed the review's six findings: a BLOCKER in the runner (`fixture_text` unbound, so a bare
`--stage run` would have died at the first case that stopped being RED — exactly gate step D's form,
i.e. the harness would have been dead on arrival for S5); **the round-1 x87 claim was WRONG and is
retracted** — the abridged FTW is not a dead field, it is AMD's **valid mask** (MEASURED: `fninit`
→ `0x00`, `fninit; fld1` → `0x80`, three pushes → `0xe0`), and round 1 read it with Intel's polarity
and built a value-derived rule on that mistake; the harness now **calibrates** the polarity at
start-up, names it on stderr, and derives the tag the way the architecture does, which makes the
shipped `simd-fild-fistp64` case report `0xAAAA` with the MMX branch FIRING (round 1 reported two
panels live and compared nothing); the slot-order comment was inverted (the code was right — slot
`i` is LOGICAL `st(i)`, the same convention as `mkfixture.c`); "the same `0xFFFFAAAA` as the 32-bit
lane" was false (MEASURED over the 123 fixtures: `0xFFFFFFFF` x49, `0x55AA` x22, `0xAAAA` x7 …); the
witness rule was **unsound** (it matched spellings, accepting a wholly 32-bit body and a
zero-padded literal, and refusing a decimal ≥ 2^32) and is now class-based with a battery that
refuses what it accepted, which made it strengthen 6 cases rather than delete them (57 → 57); and
the gate verdict now asserts the SUMMARY NUMBERS (with its own able-to-fail arm) instead of a
permissive shape.  The rest of the battery was unmoved — transcript `8bb8eca5…`, determinism
4,000,096,757 both runs, bench 200,095,214, nasm 118/118, reboot 39/39, the five VGA gates, PAE-1
11/0, PAE-L3 both legs.  NOT landed, named in the report: no 64-bit execution (57/57 RED), the stub
is executed by NEITHER leg until S5, R8-R15 are the seat and print zero, the corpus is 57 cases (the
plan's ~250 arrives family by family), and non-canonical `#GP` is deferred to S7 where a case can
express it.

**S5 IS LANDED (2026-10-19): THE FIRST LONG-MODE VERTICAL SLICE.**  `results/x86-64-s5.md` is its
report; it split the plan's S5 row into **S5a** (the mode, the walk, the dispatch, the refusals —
`tests/gate-long-mode.sh`, 23 passed / 0 failed) and **S5b** (the bodies — family 1 green).
`--long-mode on|off` (**default off**) declares `CPU_MODE_LONG|COMPAT` on the interpreter only and
adopts it when no engine was named; with it, the machine enters IA-32e through the corpus's own
stub, fetches 64-bit instructions, decodes REX, runs a 4 KiB 4-level page walk through a
64-bit-keyed TLB, and reproduces the **host CPU** for **12/12** of the corpus's `mov` family
(`green=12 red_unimplemented=0 mismatch=0 error=0`) — while the default machine is byte-identical
to S4's (transcript `8bb8eca5…`, and `make nasm64`'s census is still `green=0
red_unimplemented=57` **because the flag is off**, which is the requirement, not an oversight).
Whole corpus with the flag: `green=17 red_unimplemented=40 mismatch=0 error=0` — every case is
either host-verified or a NAMED stop naming the encoding or the slice that owns it.  What landed:
`reg64[16]` (S4's r8-r15 seat), `prefixes` → `uint16_t` + the REX nibble, the REX-aware byte
mapping (SPL/BPL/SIL/DIL vs AH/CH/DH/BH), `CS.L` + `CPU_MODE_COMPAT` (so the stub's compat-mode
`0xEA` far jump decodes and the 64-bit fetch begins after it), `EFER.LMA` on the `CR0.PG` edge,
the 4 KiB PML4 walk with **named stops** for 1 GiB / 2 MiB / a frame at or above 4 GiB (plan §5.1),
the generated **0x2NN overlay** (`gen/x86_table64.js`, 79 entries; an encoding with no arm is a
named stop, and a `REX.W` on one would otherwise have hit the switch's `DBG_ASSERT` — MEASURED as
39 host aborts), `dispatch_os_index` at all **four** call sites (the plan says three;
`instructions.c:2196` is the fourth), `read_imm64`/`read_moffs64`, the third resolver
`modrm_resolve64` with the widened `safe_*_addr` accessors, and `src/cpu/instructions_64.c` with
the 62 bodies the arms call.  The 23 remaining unimplemented call sites are the **default-64
encodings with no body yet** (ret/call rel32/jmp rel32/pop r/m/retf/iretq/the 0xFF group), which
stop **by name** rather than pushing four bytes where hardware pushes eight.  **PLAN CORRECTIONS
THIS SLICE OWES, all three found before the code**: `reg64[8]` could never hold r8-r15 (§4's
inventory is wrong about which slice widens the register file); `CS.L`/compatibility mode is
**S5's, not S11's** (after `CR0.PG` the machine is IA-32e with `CS.L=0`, and the `.byte 0xEA` far
jump is what loads the 64-bit CS — treating LMA as 64-bit immediately turns it into the overlay's
`#UD`); and the dispatch key has **four** call sites.  **THE 64-BIT ENGINE DIFFERENTIAL IS VACUOUS
AT S5 and this is asserted, not implied**: only `interp` declares the mode, and `uop`/`uopjit`
refuse it by name — S13 is where the differential becomes a real instrument.  **THE CORPUS IS THE
WHOLE PROOF FOR THE ISA LAYER** (v86 has no long mode, so green means "the tests we thought of
pass"), which is why the report's census line is the measurement and why family 1's gate uses the
**ACTUAL count (12)**, not the plan's projected "≥ 25".  SIX DEFECTS FOUND AND FIXED, each with
its arm: `imm64` is not a property of the 64-bit operand size (`C7 /0` and `0x68` are imm32 —
MEASURED as `rsp = 0x489d006a00102000` and 28 mismatches); a 64-bit body truncated into an
`int32_t` accessor is a wrong page (the `safe_*_addr` twins, and a mode-aware `modrm_resolve`);
the 4-level reserved mask is bits 52-62, not the PAE mask (a wrong FAULT is its own silent class);
a **host OOB read** on a fetch past guest RAM (`interpreted_run` named the stop and indexed
`mem8` anyway — a SIGSEGV of the host); a named stop overwritten by its own consequence (the first
name now wins); and a page-table read past guest RAM (bounded and named; the 32-bit PAE walk's
identical exposure is **pre-existing and named for S11**).  And four defects in S4's own
instruments, which the first executable run exposed (S4 had named the blind spot): the stub never
loaded **CR3**; it filled **four-byte** page-table entries where the walk reads eight; the native
leg inherited the parent's **x87 data** (`fninit` does not clear the slots, so a constant
`mm7.int32[1]` failed every case); and the generator's census line still claimed the overlay did
not exist.  The battery was unmoved — determinism 4,000,096,757 both runs, bench 200,095,214,
nasm 118/118, reboot 39/39, the five VGA gates, PAE-1 11/0, PAE-L3 both legs, transcript
`8bb8eca5…` — with `cpu_selftest` 3885 → 3889 and `uop_cache_selftest` 7934363 → 7934367 (S2's
predicate gained its COMPAT/LONG arms and the decoder gained its refusal arm).  NOT landed and
named in the report: the REX **cancel** rule is implemented and no corpus case measures it
(S6's), the `0F` space has no 64-bit arms (11 of the 40 red cases stop there), non-canonical
`#GP` is S7's, and the system layer is S11's.

**S6 IS LANDED (2026-10-19): THE ALU FAMILY AT 64 BITS, WITH ITS FLAGS.**  `results/x86-64-s6.md`
is its report.  The eleven ALU operations the row names (add/adc/sub/sbb/and/or/xor/cmp/test/
inc/dec/neg/not) now deliver their 64-bit results **and their flags** through the port's ONE set of
flag cores at `OPSIZE_64` — thirteen new wrappers in `arith.h`/`arith.c` over the S1-widened
`flags_add/adc/sub/sbb/inc/dec` and the now-widened `arith_and/or/xor` — and **9 of family 2's 10
cases are green against the HOST** (`green=9 red_unimplemented=1 mismatch=0 error=0`), the tenth
(`alu-div-zero`) being RED **by name** because `DIV` is S8's (the gate asserts the reason,
`instr64_F7_6`, not just the count).  Whole corpus with `--long-mode on`: **`green=28
red_unimplemented=29 mismatch=0 error=0`** (S5: 17/40); family 1 unmoved at 12/12; the default
(flag-off) machine unmoved at `green=0 red_unimplemented=57`.  Nineteen overlay entries and **58
bodies** landed, including `0x8F /0` `pop r/m64` — the `pushfq; pop qword ptr [slot]` plumbing
nine of the ten cases need to STORE their flags — and the four S5 `INSTR64_ALU_IS_S6` stops are
gone (their gate arms INVERTED on purpose: `rexw-xor` used to require the stop and now requires
the mutant to execute).  TWO OF THE GREENS ARE OUTSIDE FAMILY 2 and are named as such:
`ctrl-jcc-64` and `ctrl-setcc` (S7's) could not be green without 64-bit flag producers; S7 owns
keeping them green.  **LIVE DEFECT FOUND AND FIXED**: `arith_and/or/xor` took `int32_t` operands,
so a 64-bit `and rdx, rcx` was computed on the LOW 32 BITS of both and stored as the 64-bit result
(MEASURED by `alu-logic-64`); the cores are `int64_t` now and the 8/16/32-bit wrappers truncate on
the way back, pinned by the transcript and nasm 118/118.  **INSTRUMENT DEFECT FOUND AND FIXED**:
every flags word was compared UNMASKED in memory, and the two legs run at different privilege
levels (host CPL3 → IF always 1; the emulated CPL0 with the prologue's `pushfq/popfq` → IF 0), so
every family-2 case differed by exactly `0x200`; the generator now masks every flags store to the
contract's `MASK_ARITH` (0x8D5) in the slot AND in the stack copy `pushfq` leaves behind — the
alternative (setting IF in the prologue) was rejected because it leaves the emulated guest with
interrupts ENABLED in every corpus image.  Also measured: `0x8F` is `custom_modrm_resolve` in the
reference, so its body receives the RAW MODRM BYTE and must resolve the address itself in the
reference's order (the first version wrote the flags word into physical 4 and left an address in
RAX).  NOT landed, named in the report: the imm32 ALU group `0x81` and the accumulator-immediate
forms `0x05/0x0D/0x15/0x1D/0x25/0x2D/0x35/0x3D` have NO overlay entry (they stop by name) because
no corpus case grades them, `0xF7 /0,/1` likewise, `mul/imul/div/idiv` are S8's, AF on a LOGIC
operation is undefined and therefore not in the corpus, and a faulting store's flag behaviour
diverges from the reference in a way no run here can observe.

**S6 ROUND 2 (2026-10-19): the review's BLOCKER fixed — `pop r/m64` pre-adjusted RSP in the
DECREMENT direction, but POP moves the stack pointer UP** (three sources: the SDM's POP text, v86's
`adjust_stack_reg(4)` at `instructions.rs:955-969`, this tree's `instr32_8F_0_mem`), so
`pop qword ptr [rsp]` stored eight bytes below the slot it read — a wrong store with no crash.  The
corpus case that would have caught it exists now (`alu-pop-rsp`, whose effective address depends on
rsp), MEASURED red under the old direction and green under the fix, and it is a gate step of its
own.  Also fixed, each with its reason: **signed-overflow UB at nine flag-path sites** (the six
cores' sums, `getaf`'s subtraction AND its negation at `INT64_MIN`, `getof`'s, `getzf`'s — the same
class the tree already policed for `<<`), computed in `uint64_t`; a **flags store inserted between
`not` and the first `neg`** (round 1's "NOT sets no flags" row was vacuous — nothing observed the
non-effect); and **AF masked out (0x8C5) after LOGIC ops**, where the SDM leaves it undefined,
while arithmetic stores keep 0x8D5 — round 1 compared an undefined bit against the host and was
green only by empirical agreement.  Family 2 is now `green=10 red_unimplemented=1 mismatch=0
error=0` over **11 cases**, the whole corpus `green=29 red_unimplemented=29`, and the whole battery
is unmoved (transcript `8bb8eca5…`, determinism 4,000,096,757 both runs, bench 200,095,214, nasm
118/118, reboot 39/39, the five VGA gates, PAE-1 11/0, PAE-L3 both legs).

**S7 IS LANDED (2026-10-19): CONTROL FLOW AND STACK AT 64 BITS — ALL TWELVE FAMILY-3 CASES GREEN.**
`results/x86-64-s7.md` is its report.  The far-transfer seats S5 left by name are now bodies with
the case that proves each: `call rel32` pushes an **eight**-byte return address (the fetch cursor
after the displacement), `ret` pops eight straight into the 64-bit cursor, `jmp rel32/rel8`, and
`0xFF /4` (jmp r/m64) and `/6` (push r/m64 — S5's shutter) — while `ret imm16`, `retf`, `iretq`,
`call r/m64`, `callf` and `jmpf` STAY NAMED STOPS (no case covers them; each is a far transfer
whose 64-bit form is S11's).  jcc rel8 is sixteen one-liners over the port's own `test_*`
predicates.  **THE 0F ESCAPE GAINED ITS THIRD OPERAND-SIZE ARM**: `dispatch_os_index_0f` (modrm.c,
`REX.W→2 / 66→0 / else 1`), `run_instruction0f_64`, `instr64_0F`, and a 0F-qualified overlay table
(cmovcc 44/45, setcc 94/95, movzx/movsx B6/B7/BE/BF) whose arms live in the generated 0F switch —
with the base switch's NO-FALL-THROUGH guarantee added there too, because a REX.W before ANY 0F
instruction selects key 2 and a second byte without an arm hit the 0F switch's `DBG_ASSERT` and
**aborted the host on 10 corpus cases** (MEASURED, before the named-stop cases existed).  S5's
`stop64` for `0x63` became the real MOVSXD body (ARPL does not exist in long mode), and S5's
`after_block_boundary` on `jmp rel8` was removed as a divergence from the reference's table.  Whole
corpus with `--long-mode on`: **`green=37 red_unimplemented=21 mismatch=0 error=0`** — the 21 are
named stops for S8's `0xF7` group and shifts, S9's xadd/cmpxchg/bt/bsf and S10's SSE/movd/cvt
encodings; family 3 is 12/12, families 1/2 unmoved, the flag-off machine unmoved at 0/58.  NOT
landed, named in the report: `jcc rel32` (no case uses the rel32 form), `jcxz/loop*` (0xE3 stays a
named stop), `ret imm16`/`retf`/`iretq`/`call r/m`/`callf`/`jmpf` (S11's far transfers), the other
fourteen cmovcc/setcc conditions, and `xchg`'s MEMORY form (the body exists, no case grades it —
unproven, stated).

**S7 ROUND 2 (2026-10-19): two blockers and two should-fix from the review, all fixed.**
(BLOCKER 1) **The sixteen jcc rel8 bodies were DEAD CODE**: `JMPCC8` generated them and the
emitter's availability scan is TEXTUAL (`^void[ \t]+(instr…)[(]`), so the scan reported them
absent and every `0x27x` arm emitted a named stop — MEASURED as zero calls to `instr64_7x` in the
tree.  FIX: the sixteen are spelled out (the THIRD firing of this trap — `MOV_IMM64` in S5,
`ALU_IMM8` in S6 — so it is now stated as a rule: a body the emitter must see is a
`void <name>(…)` line at column 0, never a macro), with a gate step asserting 16 calls in the
GENERATED file and a new `ctrl-jcc-rexw` case that takes the `0x27x` arms on both sides of the
branch (a mutant that never branches turns it red).  (BLOCKER 2) S7 had **deleted S5 round 2's
guard on `0x63`**, so bare `63 /r` and `66 63 /r` executed ARPL, which does not exist in 64-bit
mode; a new overlay kind `narrow64` puts a NAMED STOP on the narrow arms (hardware makes them
movsxd r32/r16, which no case grades) while `REX.W` keeps the real body.  (SHOULD-FIX 3) The `0x66`
forms of the default-64 group fell through to their 16-bit arms; hardware HONOURS `66 50` as a
two-byte push, but the port's 16-bit stack path runs through `adjust_stack_reg`, i.e.
`write_reg32(ESP, …)`, which would ZERO RSP's high half — so the whole group is a NAMED STOP with
that reason.  (SHOULD-FIX 4) `ctrl-call-ret-64` was blind to a SYMMETRIC four-byte call+ret pair
(the prologue pre-zeroed the return slot): the case now pre-loads it with a sentinel, and a
4-byte-push mutant is MEASURED red on `mem[101FF8] = 0xFFFFFFFFDEADBEEF`.  Family 3 is **13/13**,
the whole corpus `green=38 red_unimplemented=21`, and the battery is unmoved.

**S8 IS LANDED (2026-10-19): MUL / DIV / SHIFTS AT 64 BITS — ALL ELEVEN FAMILY-4 CASES GREEN.**
`results/x86-64-s8.md` is its report.  The 64-bit `MUL`/`IMUL`/`DIV`/`IDIV` produce the full
**RDX:RAX 128-bit** result through `unsigned __int128`/`__int128` (the plan §8 open question answered
by the toolchain: both APE targets have it, and a hand-rolled 64×64→128 is where a carry is
dropped), with the **divide-error condition** intact and its priority kept — `#DE` when the divisor
is 0 or the quotient does not fit, fired **before** any register write, and `INT64_MIN / -1` held in
`__int128` so the bound check is the only guard.  The shift/rotate group works at 64 bits with the
**0x3F** count mask (not 0x1F), the count-0 flags-unchanged rule, and `SHLD`/`SHRD` taking their
shifted-in bits from the other 64-bit register.  **THE DEFINED-ONLY RULE IS APPLIED, NOT JUST
STATED**: every family-4 case now stores its flags word masked to the bits its own last operation
defines (MUL/IMUL `0x801`, BSF/BSR `0x40`, BT `0x1`, ROL/ROR `0x1`, shifts `0xC5`, SHLD/SHRD
`0x8C5`, DIV/IDIV **no store** — all six undefined) and ends with `cmp rax, rax`, a flag-defining
instruction that touches no register, because the document's `rflags` element is compared with
`MASK_ARITH` unconditionally; the undefined bits are therefore excluded BY RULE and named.
Whole corpus with `--long-mode on`: **`green=50 red_unimplemented=9 mismatch=0 error=0`**; family 4
**11/11**, family 2 now **11/0** (`alu-div-zero` reaches its `#DE` — DIV exists), families 1/3
unmoved, the flag-off machine unmoved.  A defect this slice found in its own first version, worth
carrying: `bsf_bsr_flags`/`bt64`/`rol64`/`ror64` wrote the flag value **and marked the bit stale**,
so the getter recomputed it from the PREVIOUS operation's lazy operands — a WRONG answer, not an
undefined one (MEASURED: ZF=1 after a `bsr` of `0x8000000000000000` against the host's ZF=0); the
fix is to MATERIALIZE (`flags_changed &= ~BIT`), the mirror image of the `flags_add` family.  The
symmetric mutant is measured too: a `mul64` writing the low half into both RAX and RDX turns
`mul-128` red.  NOT landed, named in the report: `RCL`/`RCR` (no case), the 8/16-bit MUL/DIV (the 66
forms are S7's `narrow64` stop), seven of bswap's eight register encodings and `BT`'s MEMORY form
(bodies exist, **no case grades them** — unproven), the register forms `0F A3/AB/B3/BB`, and
`SHLD`/`SHRD` by imm8.

**S8 ROUND 2 (2026-10-19): one blocker, four should-fix, two nice-to-have, all fixed.**
(BLOCKER 1) **`rol64`/`ror64` clobbered the lazy-flag operands**: both called `set_last_result`, so
a `sub`'s stale ZF/SF/PF/AF were recomputed from the ROTATE result — but the SDM says ROL/ROR leave
them UNAFFECTED.  The fix deletes the call (the 32-bit `rol32`/`ror32` never made it), and the new
`muldiv-rol-preserves-flags` case proves it — MEASURED red under the clobbering mutant, green under
the fix; its FIRST version could not fail (it rotated the just-zeroed register, so the mutant agreed
by accident), which is this lane's third "arm that cannot fail".  (SF 2) The **`BT` MEMORY forms
were wrong by construction**, not "unproven": the SDM does not mask a memory bit base (`addr +
(bit>>3)`, bit `& 7`), and round 1 masked `bit & 63`, so a guest-reachable imm8 in [64,255] picked
the wrong bit and BTS/BTR/BTC WROTE THE WRONG BYTE; the four `_mem` bodies now use the byte-granular
scheme (still un-instrumented by a case, and stated).  (SF 3) Two paths the report claimed and
nothing drove: the **imm32 three-operand IMUL** (`48 69` appeared in NO corpus image) and `C1 /1`
(ror by imm8) — both now encoded in the cases.  (SF 4) IDIV's **quotient-overflow `#DE`** had no
case, so a mutant with the range check deleted stayed green; `muldiv-idiv-overflow` (`INT64_MIN /
-1`) now fails it (MEASURED).  (SF 5) The **defined `OF`-at-count-1** rule was un-compared (every
case's last op had count != 1); count-1 snapshots were added to three cases.  (NTH 6/7) The report's
body count is **68, not 60**, §6 now names the **30 `_mem` bodies without cases** (and that no
`flags.c` diff exists in this slice), and `idiv64`'s `(__int128)high << 64` UB — the class
`idiv32`'s own comment exists to avoid — is built in `unsigned __int128`.  Family 4 is **13/13**,
the whole corpus `green=52 red_unimplemented=9`, and the battery is unmoved.

**S9 IS LANDED (2026-10-19): STRING, BIT AND ATOMIC AT 64 BITS — ALL SEVEN FAMILY-5 CASES GREEN.**
`results/x86-64-s9.md` is its report.  The 64-bit string ops (`movs`/`stos`/`lods`/`scas`/`cmps`
with their `rep`/`repz`/`repnz` forms over a 64-bit **RCX/RSI/RDI**) are **their own loop** in
`instructions_64.c`, not a widened `string.c` (that file's shared body runs addresses and count
through `int32_t`); `xadd`/`cmpxchg` and **`cmpxchg16b`** (16 bytes against RDX:RAX exchanging
RCX:RBX, with the SDM's **alignment `#GP`**) are in too.  Family 5 is **7/7** — the six plus a new
`bt-mem-imm8`, which is the **first instrument for the BT MEMORY form** S8's round 2 fixed.  A
defect this slice found in its own first version: the `cmpxchg16b` case compared a **PF** the SDM
leaves undefined (only ZF is defined) — the port's derived PF was 1, the host's 0, four fields
failed; fixed by masking to ZF and normalising the `rflags` element.  **AN OPEN FINDING, recorded
rather than papered over**: the BT **memory** `imm8 >= 64` rule is contested — S8's round 2 fixed
the bodies to a BYTE-granular bit string, the review's text says the offset DIV 64 selects the
QWORD, and **this host agreed with neither** (MEASURED: `bts qword [r8], 100` wrote `0x10` at
byte 4, i.e. as if masked to `0x3F`), so the shipped case uses bit 20, where all three readings
agree, and the `>= 64` behaviour needs a spec re-read before any of them is claimed.  Whole corpus
with `--long-mode on`: **`green=59 red_unimplemented=3 mismatch=0 error=0`** — the 3 are S10's SSE
encodings.  NOT landed, named in the report: the `rep` forms' ZF exit and the DF=1 direction (no
case), `cmpxchg`'s register form, bswap's other seven encodings (S8's gap), and the string ops'
fault partial-effects (out of the corpus by rule, a stated choice).

**S9 ROUND 2 (2026-10-19): two blockers and three should-fix.**
(BLOCKER 1) **The imm8 `_mem` BT bodies were unmasked**: the SDM ignores the immediate's high order
bits, so the offset is **masked to the operand width (`& 63`) and THEN byte-granular** — round 1
used an unmasked `bit >> 3`, so a guest-reachable imm8 in [64,255] read the wrong CF and wrote the
wrong byte for BTS/BTR/BTC.  The corpus arm is now **bit 100** (`100 & 63 = 36` → byte 4, bit 4 →
`0x10` at `0x100304`), MEASURED red with the mask removed; round 1's bit-20 arm was blind between
all three readings.  **The contrast is recorded**: the register-source form (`0F AB/B3/BB/A3`) is
the OPPOSITE rule (signed, byte-granular, unmasked) and must not copy this masking.
(BLOCKER 2) **The five BARE string forms clobbered RCX** — a no-rep `stosq`/`movsq`/`lodsq`/
`scasq`/`cmpsq` must not touch it (MEASURED: seeded rcx, host keeps it, port returned 0), and the
corpus was blind because its prologue zeroes RCX.  The count store is now guarded by
`rep != STR64_NONE` and `strobits-bare-string-preserves-rcx` SEEDS RCX.  (SF 3) `cmpxchg16b`'s
CF/PF/AF/SF/OF are **UNAFFECTED**, not undefined (MEASURED: a prior PF=1/CF=0 survive a mismatch):
ZF is materialised and the case compares the full `0x8D5` again.  (SF 4) Four artifact-contradicted
claims corrected — including **the commit message's own error**: it said S9 landed bswap's seven
encodings and shld/shrd, and this diff touches neither (shld/shrd are S8's; bswap's seven remain
un-instrumented).  (SF 5) §5 now names the register-source BT family, `A4/A6/AC/AE`, shld/shrd imm8
and cmpxchg16b's not-equal arm; and the **`66`-prefix fall-through recurred** (`66 A5` ran MOVSQ) —
closed by `narrow64` on the five string opcodes.  (NTH 6) the generator's case comment stated the
refuted rule as fact; corrected.  Family 5 is **8/8**, the whole corpus `green=60
red_unimplemented=3`, and the battery is unmoved.

**S10 IS LANDED (2026-10-19): x87/MMX/SSE/SSE2 IN LONG MODE — THE WHOLE 63-CASE CORPUS IS GREEN.**
`results/x86-64-s10.md` is its report.  The slice is the plan's "the addressing and the REX register
fields, semantics unchanged": the long-mode **addressing** was already shared (S6's mode-aware
`modrm_resolve` is what turned five of the six family-6 cases green), and what landed here is the
**operand WIDTH** a REX.W selects — six bodies for `0F 6E`/`66 0F 6E` (movq mm/xmm, r/m64),
`0F 2A`/`F2 0F 2A` (cvtsi2sd with a 64-bit integer source) and `0F 7E`/`66 0F 7E` (movq r/m64,
mm/xmm).  **ONE STATE CORRECTION**: `reg_xmm[8]` → `reg_xmm[16]` — long mode has sixteen SSE
registers and a REX.B/REX.R field selects 8-15, an index the eight-slot array could not hold, so
the accessors would have written OUT OF BOUNDS (a guest-reachable host corruption); the plan's §4
"`reg_xmm[8]` UNCHANGED" predates the REX register fields being reachable.  MEASURED asymmetry: a
mutant `movq r64, xmm` body writing reg32 (the 32-bit shape) turns `simd-movq-xmm64` red.  The
whole corpus with `--long-mode on` is now **`green=63 red_unimplemented=0 mismatch=0 error=0`**,
family 6 is 6/6, and the 32-bit FPU/MMX/SSE tests did NOT move (nasm 118/118, transcript
byte-identical).  NOT covered, named in the report: **xmm8-15 as OPERANDS** (representable, no case
— un-instrumented), the bare `0F 6E/7E/2A` forms and the mm-side memory forms (bodies, no cases),
and **every other SSE/SSE2 encoding** plus x87's own D8-DF (no 64-bit arm: with REX.W they stop by
name — a declared boundary, not an oversight).

**S11 IS LANDED IN PART (2026-10-19): THE SYSTEM LAYER — the walk and the MSR set, with the rest
NAMED AS GAPS.**  `results/x86-64-s11.md` is its report.  **Landed**: the walk now **composes 1 GiB
and 2 MiB pages** (S5 had named stops for both) with the 32-bit physical bound on each frame; a
**canonicality check at the walk's entry** raises **`#GP(0)`** — closing the defect S5 recorded, where
the index masks silently ALIASED a non-canonical address onto its low 48 bits and the identity map
made that alias a MAPPED page; and the **MSR set**: `FS_BASE`/`GS_BASE`/`KERNEL_GS_BASE` (the bases
write `segment_offsets[FS/GS]`, the slots S3 widened for exactly this — plus a pre-existing defect
fixed on the way: the port's old `IA32_KERNEL_GS_BASE` placeholder was defined as 0xC0000101, i.e.
GS_BASE's number) and `STAR`/`LSTAR`/`FMASK` (stored and read back so the SYSCALL seat exists).
**THE GATE IS A NEW `tests/gate-long-mode-sys.sh`, one MUTATION PROBE PER LANDED RULE** (5/0): a 1 GiB
PDPTE must run clean, a non-canonical address must fault with a CANONICAL control that runs clean, and
`wrmsr(FS_BASE)` must move the base the STATE tail reports — and **two of those probes were VACUOUS
in their first version and were fixed by measurement** (a moffs64 above 4 GiB hits the port's own
moffs bound before the walk; a "a document was printed" assertion is also true of a FAULTING run).
Two of `gate-long-mode.sh`'s arms were **INVERTED** accordingly (the 1 GiB/2 MiB named stops became
"must compose").  **NAMED GAPS, each with its missing instrument**: SYSCALL/SYSRET (MSRs modelled,
ENCODINGS not implemented — the probe needs a kernel-style image, which is S12's), **SWAPGS** (0F 01
/5 has NO entry in v86's table, so the generated dispatch cannot carry it — an emitter extension),
the **64-bit GDT/IDT/TSS/IST and interrupt frame** (the largest remaining piece, and S12's first
item), the long-mode TLB's page-flag split, the `#PF` I/D bits, and S9's register-source BT family.

**S12 IS LANDED IN PART (2026-10-19): THE amd64 GUEST BOOTS INTO LONG MODE AND STOPS AT A NAMED
PIECE.**  `results/x86-64-s12.md` is its report.  A **pinned Debian amd64 kernel** (the same 6.1.0-50
source the PAE-L3 lane pins, one architecture different; .deb URL + sha256 and the vmlinuz sha256
recorded, the 8.2 MB image committed beside `vmlinuz-i386`) boots on the port under `--long-mode on`,
**enters long mode, builds its own page tables and executes 64-bit code** — `eip=0x100208 cs=0x10
cr0=0x80050033 cr3=0x4f76000`, its own serial line `Probing EDD … ok` — and then stops at
**`call_interrupt_vector: invalid gate type`**, i.e. the **64-bit IDT gate / interrupt frame** that
S11 named as its first item.  That is the plan's own falsifier ("the corpus-green-but-guest-dead
case"), recorded with the guest's evidence and **not** a lowered gate.  **THE BOOT PATH NEEDED NO
CODE CHANGE** (plan §5.3's INTERPRETATION is now MEASURED: the kernel's own setup enters IA-32e
through the writes S2's refusal guards), so the binary did not move.  **A measured vehicle bound**:
with the default 64 MiB the kernel's PML4 lands at 0x4f76000, ABOVE guest RAM, and S11's table-read
bound stops it by name — the gate pins `--memory 256` and says why.  **THE GATE IS NEW:
`tests/gate-x86-64.sh` (7/0)** — the pin, the flag-off refusal, the progress assertions
(cs/cr3/serial) and **the honest stop asserted BY NAME** (a stop earlier is a regression; when the
interrupt frame lands that step is replaced by the userspace assertion, visibly).  NAMED GAPS before
userspace: the 64-bit IDT gate decode + interrupt frame (the stop; with the hypothesis — stated as
one — that the 64-bit offset composition, not the type test, is the suspect), the 64-bit GDT/TSS/IST
probes, SYSCALL/SYSRET and SWAPGS (S11's gaps), the amd64 initramfs and its evidence printing, and
the QEMU leg.

**S13 IS LANDED (2026-10-19): THE ENGINES — THE 64-BIT DIFFERENTIAL IS REAL.**  `results/x86-64-s13.md`
is its report and carries THE LANE'S FINAL STATE.  All three engines now RUN IA-32e: the interpreter
natively, and `uop`/`uopjit` by **DELEGATING each long-mode batch to the interpreter** (counted as
`long-mode batches delegated to the interpreter`, and printed — a claim with no counter would be a
gap), so the differential S5 recorded as VACUOUS is now an instrument: **each engine is 63/63 green
against the host, and the three engines' documents are BYTE-IDENTICAL** (v2 array + STATE tail) for
six cases spanning the six families, with an able-to-fail arm and a control that proves the counter
measures the MODE (without `--long-mode` it delegates zero).  **The new gate is
`tests/gate-x86-64-diff.sh` (13/0)**; `gate-long-mode.sh`'s three S5-era "the engines refuse" arms are
INVERTED (they must now RUN), and `uop_cache_selftest`'s S5 refusal arm is RETIRED with its reason
measured (a delegated run EXECUTES, so the hand-built sandbox crashed; the gate's counter pair on real
images is the stronger instrument) — `uop_cache_selftest` 7934367 → 7934363, stated not absorbed.
**THE PLAN'S FOURTEEN SLICES ARE ALL LANDED.**

**THE RELOC UNIT IS A MEASUREMENT: NO PORT DEFECT IN THE POST-DECOMPRESSION PATH (2026-10-19).**
`results/x86-64-reloc.md` is its report, and it CORRECTS the wide unit's headline: "the decompression
completes, all 8,388,607 bytes" was **my own 8 MiB scan ceiling**, not the image's end -- re-measured
with a 32 MiB scan, the last non-zero byte is at 0x2DFBC4F (48.2 MB) at a 2.4 G budget.  The
decompressed image is a real 26 MB ELF64 (e_phoff 0x40, e_phnum 5, phdr0 p_vaddr 0xffffffff81000000,
p_paddr 0x1000000, p_filesz 0x18E6498) whose five PT_LOAD segments' p_filesz SUM TO 59,661,968 bytes
(~57 MB, heavily overlapping).  The guest is inside `parse_elf`'s `memmove(p_paddr, output+p_offset,
p_filesz)`, one byte per iteration: the copy loop is 0x4F399A8..0x4F399E5, called from 0x4F3A36E,
state at 0x4F58588, and its destination index climbs MONOTONICALLY (1.0 MB at 14 M instructions,
4.3 MB at 300 M, 22.6 MB at 1.8 G, 31.4 MB at 2.4 G) -- PROGRESSING, NOT STUCK.  Three candidate
defects (an unbounded memmove, garbage p_filesz from a bad malloc, the stub never reaching parse_elf
because "Decompressing Linux..." is missing from the serial) were each REFUTED by measurement before
any code was touched; the third by reading misc.h: `debug_putstr` is a no-op unless
CONFIG_X86_VERBOSE_BOOTUP, so those messages are compiled out of the Debian kernel.  **NO SOURCE FILE
CHANGED**; the whole battery was re-run on the probe-free build and every pin is unmoved.
NEXT: a ~5 G instruction run to catch `head_64.S`'s `jmp *%rax` into the kernel (parse_elf should
finish near 3.5 G), with the cost named: the guest's byte-wise memmove is ~2.3 G instructions, so a
boot is ~5 G instructions / ~25 minutes of wall clock at the observed rate.

**THE WIDE UNIT IS LANDED: THE GUEST DECOMPRESSES THE WHOLE KERNEL (2026-10-19).**
`results/x86-64-wide.md` is its report.  The "widen the bodies" gap was **four layers**, not one:
`modrm_resolve` now returns `int64_t` and no longer bounds an answer above 4 GiB; the eleven
`safe_read*/safe_write*` accessors take `int64_t` and dispatch to their 64-bit-address twins (with
new 128-bit and f32 arms); 1271 `_mem` signatures went from `int32_t addr` to `int64_t addr`; the
emitter now emits ONE form (`int64_t addr = modrm_resolve(...)`) because the resolver is mode-aware;
and the arith layer's `bt/bts/btr/btc_mem` — one layer below the bodies — were found BY THE
INSTRUMENT (the `wide-strobits` case executed the `bt` with no fault at all, the address having been
truncated into mapped memory).  Seven new corpus cases (family `wide`), each dereferencing a
NON-CANONICAL address above 4 GiB so the guest takes #GP and the host SIGSEGV at the same rip; the
able-to-fail arms are MEASURED in two mutations (restoring the bound: 6 of 7 RED; restoring the
pre-unit string path: the 7th MISMATCHES).  THE GUEST: no named stop at a 1.5 G budget and the
decompression COMPLETE — the output buffer's last non-zero byte reaches 0x17FFFFF (all 8,388,607
bytes; monotone: 2.33 MB at 60 M instructions, 5.11 MB at 400 M).  The four M0 pins are unmoved.
Corpus 73 -> 80, families 7 -> 8.  NEXT: the decompressor's post-decompression relocation/final
jump (eip 0x4f3a36e), measured with the extent-scan instrument applied to 0x4780000 — there is no
stop to name yet, only a budget.

**THE REP UNIT IS LANDED: the guest DECOMPRESSES (2026-10-19).**  `results/x86-64-rep.md` is its
report.  Part (A): the stop reported as "rep movsb raises a #GP" is `repe cmpsb` (0xA6 is CMPS), and
the #GP was `get_seg`'s null-segment check, which 64-bit mode does not perform -- the string family
now has a real 64-bit path (RCX/RSI/RDI at 64 bits, 0x67 narrowing with zero-extending write-back,
DF, REX.W 8-byte forms, the repe/repne ZF exit, partial effects kept on a fault), S9's five
hand-written `str64_*` loops replaced by it, and four new corpus cases of which
`str/cmps-count-64` proves the counter is 64 bits (RCX = 0x100000002 exits on the first mismatch and
must leave 0x100000001).  The corpus caught a bug in the new code: the ES:DI side was keyed off the
STORE, so `repe cmpsb` compared against address 0.  Part (B) REFUTES the idt64 report's claim that
the stage-1 IDT is all-null: the guest's IDT (base 0x4f51050, limit 511) has REAL 16-byte gates at
vector 14 (#PF, handler 0x4f3f870) and vector 2, and the port read slot 7 for vector 14 because it
used the 32-bit **8-byte stride** over **16-byte** gates.  Fixed: the stride, the offset's high half,
and the 8-byte frame (RFLAGS/CS/RIP/error code, 8 bytes each) -- proven by a new `m1c_selftest`
arm (258 -> 272 checks) whose able-to-fail arm is MEASURED (reverting the stride fails four checks).
`gate-x86-64.sh`'s `stop-is-the-64bit-interrupt-frame` step is REPLACED as its own comment required:
the guest no longer stops there, its decompressor console is alive, and it stops at 30,184,930
instructions on the 64-bit address bound.  0x98/0x99 (CDQE/CQO) and `0F C7 /6` RDRAND r64 were
closed on the way (RDRAND r64 is what let the guest reach the decompressor).  Corpus 68 -> 73,
families 6 -> 7, overlay 155 -> 157.  The four M0 pins are unmoved.

**THE IDT64 UNIT IS LANDED AND ITS BRIEF'S HYPOTHESIS WAS WRONG (2026-10-19).**  `results/x86-64-idt64.md`
is its report.  The gate-type stop at `eip=0x100208` was **not** the 64-bit descriptor's offset
composition (S12 §3.3's hypothesis): the port was inventing a #GP on `mov ss, ax` with a NULL selector,
which is legal in 64-bit mode at CPL 0 (QEMU's `helper_load_seg` condition, the kernel's own
`head_64.S:334-356` entry).  Closing that and five further MEASURED defects (0x81 imm32 group, 0xCB
retfq, the LEA sentinel's host abort, a 32-bit LEA above 4 GiB, the cmovcc family, REX.B dropped on
opcode-implied registers, the accumulator-immediate forms, `FF /2` call r/m64) moved the pinned guest
from **30,594 to 626,389 instructions** — into the decompressor's C code.  **The 64-bit interrupt
frame is still NOT validated**: no deliverable interrupt has been reached (every stop is a fault whose
stage-1 IDT entry is null, where hardware triple-faults too), and `gate-x86-64.sh`'s
`stop-is-the-64bit-interrupt-frame` step asserts the STOP, not a frame.  Corpus 63 → 68 cases, all
green on all three engines; the four M0 pins are unmoved.

The named gaps the next session should take, in order: (1) **the jump into the decompressed
kernel** — `parse_elf`'s ~57 MB byte-wise memmove should finish near 3.5 G instructions, so a ~5 G
run with the "first eip inside 0x1000000..0x2000000" trigger catches `head_64.S`'s `jmp *%rax`; the
cost (a ~5 G-instruction boot) is the guest's own memmove, not the port's; (2) the 64-bit GDT/TSS/IST
paths, SYSCALL/SYSRET, then
SWAPGS (needs an authored dispatch arm — `0F 01 /5` has no reference-table entry); (3) the amd64
initramfs + the userspace milestone, with the QEMU leg as the second reference; (4) the engines' OWN
arms (the uop decoder's long mode, then the JIT's recipes) — the differential is the gate that would
judge them.  Still open from this unit: the 64-bit interrupt frame, `far_return`'s 32-bit `eip` (a
RETF target above 4 GiB and a CPL-changing RETF are named stops), `/3 callf` and `/5 jmpf`.

**ALL FOURTEEN SLICES (S0-S13) ARE LANDED.**  The rest of this section is the reconnaissance record
and is not made stale by S0-S13.

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
