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

**PRODUCER (2026-10-21): `page_offset_base` IS WRITTEN CORRECTLY — AND THE HALVES ENTRY BELOW IS
RETRACTED.** `results/x86-64-producer.md`. The instrument had to be fixed first (MEASURED): `V86C_WATCH`
on the global's virtual address, on its neighbours and on two direct-map aliases all reported **nothing**
while a PEEK of the same physical slot showed the value change inside one run — every body goes through
`safe_write*`, so the silence was the instrument's, not the guest's. **Landed:** the physical writers
(`write8/16/32`, `write64_no_mmap_or_dirty_check`, `mmap_write64`) now call `v86c_dbg_watch_p` (one load
and one branch when the toolkit is off). `V86C_WATCH=0x2413d60:8` then speaks:
`WATCH P 0x2413d60 width=4 value=0xffffffffc0000000 rip=0xffffffff8ae9ef45` and
`WATCH P 0x2413d64 width=4 value=0xffffffffffff8c16 rip=0xffffffff8ae9ef45` — two 32-BIT stores of the two
halves, rip link `0xffffffff8309ef45`, inside `kernel_randomize_memory`
(`div %rcx; and $0xffffffffc0000000,%rdx; lea (%rdx,%rbx,1),%rax; mov %rax,(%rcx)` at link
`0xffffffff8309ef31..0x8309ef42`), i.e. `page_offset_base = __PAGE_OFFSET_BASE_L4 + ((rand % n) &
PUD_MASK)`. **RETRACTION:** `0xffff8c16c0000000` **IS CORRECT** — it is PUD-aligned, a PUD is 1 GiB, so
the low 30 bits are zero and **bits 30–31 MAY be set**; the HALVES entry's "a legitimate value must have a
ZERO low 32 bits" is WRONG and its "wrong low half" conclusion is withdrawn. **WHAT IS ACTUALLY WRONG
(MEASURED, and it is not the randomizer):** the *physical* addresses are truncated to **24 bits** — the
per-cpu base uses `0x00fd4000` where `0xc0fd4000` is required, and the direct-map PDE maps `0x00e00000`
where `0xc0e00000` is required — and those physical addresses (`0xc0e00000`, `0xc0fd4000` ≈ 3.2 GB) are
also **beyond the guest's 256 MiB RAM**, which is the second finding: `__alloc_bootmem_node_nopanic`
(link `0xffffffff8309d94f`) returns a physical address outside RAM. **NEXT FORK, two questions, not a
body:** (1) where bits 31..24 are dropped on the way to the PTE (`0xc0e00000 → 0x00e00000`); (2) why the
allocator hands back an address above RAM. **No fix claimed, no source change to the machine** — only the
toolkit; make/selftest/gate-x86-64/bench green, bench 200,095,214.

**HALVES (2026-10-21) — *RETRACTED IN PART, see PRODUCER above*: the "wrong low half" reading of
`page_offset_base` is withdrawn; the recursion chain it measured is still the record.**
`page_offset_base`'S LOW HALF IS WRONG — ONE 32-BIT HALF EXPLAINS THE WHOLE
RECURSION.** `results/x86-64-halves.md`. MEASURED: `V86C_WATCH` on `__per_cpu_offset[0]` (runtime VA
`0xffffffff8aa393e0`) logs ONE write, `value=0xffff8c16c0fd4000 rip=0xffffffff8ae9d9c6`, so the value
is wrong BEFORE the `wrmsr` (shape (a)/(b), not a mis-read EDX). The writer is link
`0xffffffff8309d9be` (`mov %r13,-0x7d3c6c20(,%rax,8)`) after `add %rax,%r13`, and at the `add`
`%rax = 0xfd4000` (the physical offset, no high bits) while `PEEK 0x2413d60` gives
**page_offset_base = 0xffff8c16c0000000** — and `0xffff8c16c0000000 + 0xfd4000 = 0xffff8c16c0fd4000`,
i.e. the stored per-cpu base is the CORRECT sum of the STORED base. **The base is the defect:** the
kernel randomized it, and the value must be 1 GiB-aligned with a ZERO low 32 bits
(`0xffff8c1600000000`); memory holds `0xc0000000` there instead. That wrong base shifts every entry
of the randomized direct map by `0xc0000000`: `V86C_BREAK_CHAIN=0xffff8c16c0fd4000` shows
`PDE = 0x8000000000e000e3` → phys `0x00e00000` where `0xc0e00000` is required (and
`0xc0e00000 - 0xc0000000 = 0x00e00000` exactly). So: the per-cpu area (phys `0xc0fd4000`, a
legitimate 192 MiB memblock placement) is mapped at phys `0xfd4000` → the per-cpu `cpu_number` read
returns garbage `0x1e00` → `__per_cpu_offset[0x1e00]` is out of bounds → `%rdx = 0` → the fault at
`0x1e08` → the delivery reads the CS descriptor at `gdtr+0x10` = `gs_base + 0xb010` = `0xc0a0b010`,
unmapped for the same reason → four deliveries → the bound. **NEXT FORK, one measurement:**
`V86C_WATCH=0xffffffff82413d60:8` (the runtime VA of the global) names the writer and its value — a
32-bit store where a 64-bit one belongs, a halves swap, or a carry. **No fix claimed.** No source
change: make/selftest/gate-x86-64/bench green, bench 200,095,214.

**RECURSION (2026-10-21): THE FAULT-DELIVERY RECURSION IS A PER-CPU BASE WITH A WRONG HIGH HALF.**
`results/x86-64-recursion.md`. The ring names the code (link `0xffffffff812ef8e5`, slide still
`0x7E00000`): `mov %gs:0x7ed3ec94,%eax` (a per-cpu read), `cltq`, `mov -0x7d3c6c20(,%rax,8),%rdx`
(`__per_cpu_offset[cpu]`), `lea 0x1e00(%rdx),%rax`, and the faulting `cmp %edi,0x1e08(%rdx)` with
**`%rdx = 0`** — a NULL per-cpu pointer, read at `0x1e08`. The DELIVERY's own fault is at
`cr2 = 0xc0a0b010` = **`gdtr + 0x10`**: the port could not read the CS descriptor in the GDT, so the
delivery faulted four times and the bound fired. `gdtr = gs_base + 0xb000` exactly — the GDT is the
per-cpu `gdt_page`, so ONE value produces both. **THE REFERENCE** (QEMU's gdbstub at the same
instruction, nokaslr): `rax = 0xffff88800ffb6e00`, `rdx = 0xffff88800ffb5000`, and `rsp`/`rdi`/`rbp`/
`rsi` identical to the port's — hardware's per-cpu pointer is valid and its `cpu_number` read returned
0; the port's returned `0x1e00`. **THE DEFECT:** the port's per-cpu base is
`0xffff8c16 c0a00000` where the direct map of that same allocation is `0xffff8880 c0a00000` — the LOW
half is its own legitimate 192 MiB `memblock` placement (QEMU's is `0xffb5000`, equally legitimate),
the HIGH half is wrong. So it is a **32-bit-halves defect in the per-cpu base's high half**, not an
allocation or mapping error. **NEXT FORK:** break on the store to `__per_cpu_offset[0]` and on the
`wrmsr(MSR_GS_BASE)` that follows and read both — the wrong high half is already in the computed value
(INTERPRETATION: the GDT address derives from it), so the producer is `setup_per_cpu_areas`'
arithmetic, not the MSR write alone. **No fix claimed** (three shapes fit and the machine is not
changed on a hypothesis). The kernel still reaches its banner and ~30 s of boot; `gate-x86-64` 14/0
with the banner gated. No source change: make/selftest/gate-x86-64/bench green, bench 200,095,214.

**F7 (2026-10-21): TEST r/m64, imm32 LANDED — AND THE KERNEL NOW RUNS 30 SECONDS OF BOOT.**
`results/x86-64-f7.md`. `instr64_F7_0_mem`/`_reg` over the port's existing `test64()`, with the
immediate SIGN-EXTENDED to 64 bits (SDM) and TEST's flags (CF/OF cleared, SF/ZF/PF from the result,
AF UNDEFINED). Corpus arm `alu/test-imm32-64` separates a sign-extended immediate (SF=1) from a
zero-extended one (SF=0) and compares `0x8C5` so the undefined AF is dropped; RED against the pre-fix
binary, green now (`make nasm64` 13/0, all 85 cases agreeing). **READ, and it will bite the next
"implement N bodies" unit: the interpreter generator finds bodies by SCANNING THE SOURCE TEXT, so a
macro-generated body is invisible — MEASURED, my first attempt with a `DEFINE_BITTEST64` macro left
the census at 44 unimplemented and the call sites as stops; write the bodies out.** THE GUEST: from
the banner (4,488,210,693 instructions) to **4,519,091,709** and a serial reading
`[   29.866809] percpu: cpu 0 has no node -1 or node-local memory` / `[   29.876706] percpu:
Embedded 62 pages/cpu …` — through `start_kernel` into `setup_per_cpu_areas`, ~30 SECONDS of kernel
boot. **THE NEXT STOP IS NOT A BODY**: `a fault raised while delivering a fault: the delivery
re-entered four times` at `eip=0x890ef901 cr3=0x2a10000` — the fault-delivery recursion bound, i.e. a
walk/frame question, so this unit STOPS there per its own rule. Next fork: which access faults at
`0xffffffff890ef901` and why its delivery re-enters (`V86C_CHAIN`/`V86C_PEEK_SP`; the 64-bit frame is
no longer a suspect). GATES: `gate-x86-64` **14/0** with the banner gated and the stop step re-pointed
to the new stop; long-mode 57/0; diff 13/0; the rest green. PINS THAT MOVED, with reasons: the corpus
count 84 -> 85 (a case ADDED, in three gates), the overlay census 44 -> 42 unimplemented (two gaps
CLOSED), the alu family and the diff legs to `green=13/84 red_unimplemented=1` (the new case is a
named stop in those legs' configurations). A blanket edit of mine briefly broke the muldiv pin and was
restored.

**VA (2026-10-21): QEMU FAULTS TOO — THE DIVERGENCE IS AFTER THE REPAIR, AND IT IS
`MSR_KERNEL_GS_BASE`.** `results/x86-64-va.md`. The brief's premise is corrected with QEMU's own
`-d int` log: QEMU's FIRST exception is `v=0e e=0000 cpl=0 IP=0010:ffffffff83077246
SP=0000:ffffffff82a03f30 CR2=ffff888000014790` — the SAME instruction as the port, and its CR2 is
the direct map of ITS OWN boot_params (physical 0x14790; the port's is 0x84790). So both fault on the
same access and both repair it (QEMU then takes three more demand #PFs and boots; the port's
`PML4[273] = 0x30ec063` was measured). The "inside vs outside the fixed early range" arithmetic does
NOT distinguish them: both addresses are low memory inside that range, and the direct map is
demand-built anyway. Nor does the call site: the unconditional `copy_bootdata` in
`x86_64_start_kernel` (link `0xffffffff83077608`) runs BEFORE `x86_64_start_reservations`' guarded one
(`0xffffffff83077555`) on any boot — MEASURED on the port (only that site is hit) and QEMU's fault-0
SP equals the port's rsp at the same instruction, de-slid, exactly. So the branch input is the SAME
on both (return address `0x…8307760d`, bit 2 SET) and **the `swapgs` is taken on hardware too**;
QEMU survives it because `MSR_KERNEL_GS_BASE` is NOT 0 there, while on the port
`gs_base = 0xffffffff8ae42000, kernel_gs = 0` (MEASURED), so the swap zeroes GS.base and the percpu
read resolves to the bare `0x19000`, which the demand-repair cannot map. The kernel DOES write MSR
`0xC0000102` — **30 sites, encoded `mov edi,0xc0000102; call wrmsr`** (a search for
`mov ecx,0xc0000102` finds none: the encoding differs) — but ALL are in main `.text`, later than this
point. **NEXT FORK:** `V86C_BREAK` on those sites in the early call tree of `x86_64_start_kernel`
(`sme_early_init`/`kasan_early_init`/`idt_setup_early_handler`/`tdx_early_init`), with
`instructions_0f.c:6452` (the port's `wrmsr` reader) as the check: if one executes before the
`swapgs`, the defect is a `wrmsr`; if none does, hardware reaches one the port skips. **The first
`printk` is still not reached.** No source change: make/selftest/gate-x86-64/bench green, bench
200,095,214. Operational note: `-d cpu,int` wrote a 23 GB log in 3.5 minutes and filled /tmp; `-d int`
alone is the right instrument at this scale.

**BRANCH (2026-10-21): ONLY THE UNCONDITIONAL `copy_bootdata` RUNS, AND THE BRANCH IS BIT 2 OF A
RETURN ADDRESS.** `results/x86-64-branch.md`. MEASURED with `V86C_BREAK` on both call sites in one
run: slot 1 (`0xffffffff8ae77608`, `x86_64_start_kernel`'s UNCONDITIONAL `copy_bootdata`) is hit
(rdi = 0xffff888000084790 = `__va(real_mode_data)`); slot 0 (`0xffffffff8ae77555`,
`x86_64_start_reservations`' `if (!boot_params.hdr.version)`-guarded call) is **never hit**. READ:
`0x83077544` is the guarded site (`cmpw $0,[boot_params.hdr.version]; jne; add page_offset_base,rdi;
call copy_bootdata; jmp`) and `0x830775b2` starts `x86_64_start_kernel` (`mov %cr4,%rax; mov
%rax,%gs:0x31558` = `cr4_init_shadow`). The branch that decides the rest of the boot is
`testb $4,0x20(%rsp)` in `restore_regs_and_return_to_kernel`, whose input is `copy_bootdata`'s
RETURN ADDRESS: the unconditional call's is `…8307760d` (bit 2 SET → `swapgs` → GS.base 0 →
`gs:0x19000` → demand-repair refuses → panic), the guarded call's is `…8307755a` (bit 2 CLEAR →
`iretq`). `kernel_gs = 0` at the swapgs and no `.init` code writes MSR `0xC0000102`, so the branch
must never be taken — which is what QEMU's log shows (zero faults at `pc=ffffffff81c0123a`).
**What that implies:** QEMU's fault must be the GUARDED site, so on QEMU the unconditional
`copy_bootdata`'s direct-map read did NOT fault while the port's did. **The one-line discriminator is
QEMU's stack word at physical `0x2a03f38`** (the return address above its `SP = 0xffffffff82a03f30`
from its own `-d int` log): `…760d` refutes the implication, `…755a` confirms it. NOT OBTAINED — the
host has no `gdb` and the monitor cannot be timed into that window; the method is in the report §5.
**No fix claimed** (no fix is proven; the machine is not changed on a hypothesis). **The first
`printk` is still not reached.** No source change: make/selftest/gate-x86-64/bench green, bench
200,095,214, no pin moved.

**SSSLOT (2026-10-21): THE SS SLOT HOLDS A RETURN ADDRESS — AND THE PORT REACHES THE SECOND
`copy_bootdata`.** `results/x86-64-ssslot.md`. QEMU as the reference (TCG; KVM logs nothing) gives the
hardware-shaped first fault: `v=0e e=0000 cpl=0 IP=0010:ffffffff83077246 SP=0000:ffffffff82a03f30
CR2=ffff888000014790` — the SAME instruction as the port, and its SP matches the port's rsp de-slid
EXACTLY; QEMU never faults at the `gs:0x19000` read (zero hits) and boots. The stub is `push $0x0e;
jmp common` (no SS/RSP, correct for a same-privilege exception) and the common handler pushes
FOURTEEN registers with rdi written into the frame's first slot, so the restore path's `pop x15` +
`add rsp,8` lands exactly on `ip` — the frame is NOT the defect (ip/cs/flags are correct, and the
runtime bytes at `0xffffffff81c01214` are identical to the image, so no alternatives patch). **The
answer:** `testb $4,0x20(%rsp)` reads the word ABOVE the pre-exception SP — `copy_bootdata`'s RETURN
ADDRESS — and bit 2 of its low byte decides. QEMU's fault is from the call at `0xffffffff83077555`
(return address `…5a`, bit 2 CLEAR → `iretq`); the port's is from the call at `0xffffffff83077608`
(return address `…0d`, bit 2 SET → `swapgs`), and `kernel_gs` is 0 — the image contains NO write of
MSR `0xC0000102` at all — so GS.base becomes 0, the percpu read resolves to the bare `0x19000`, the
demand-repair refuses, and three nested faults halt the machine. **So the port's divergence is that
it reaches the SECOND call site at all:** that call is guarded by `if (!boot_params.hdr.version)`,
so `boot_params.hdr.version` was still 0 after the first copy — **the first `copy_bootdata` did not
copy**. NEXT FORK: why — its source read was repaired and retried, and its destination
`&boot_params` (`0xffffffff8b117600`) has a present 2 MiB page (`PDE[88] = 0x032001e3`); instrument
`V86C_WATCH` on phys `0x3317600` (the first 16 bytes of `&boot_params`). **The first `printk` is
still not reached.** No source change this unit (instruments only): make/selftest/gate-x86-64/bench
green, bench 200,095,214, no pin moved.

**NEST (2026-10-21): THE NESTED EXCEPTIONS ARE ONE `swapgs` ON AN UNFILLED SS SLOT.** `results/x86-64-nest.md`.
MEASURED end to end. Four handler entries (`BREAK 0xffffffff8ae77120`, all vector 0x0e #PF) with the
new rsp-relative VIRTUAL dump `V86C_PEEK_SP`: rip `0xffffffff8ae77246` (copy_bootdata's direct-map
read, `cr2_64 = 0xffff888000084790` CORRECT), then `0xffffffff89a0123a` (`mov %gs:0x19000,%rdi`,
`cr2_64 = 0x19000` — the BARE offset), then `0xffffffff88f4bb09` twice (a `call` in the panic print,
`cr2_64 = 0x1fb40`). The pushed frame is RFLAGS/CS/RIP + errcode — SDM-correct for a same-privilege
exception, so a wrong *push* is ruled out, and the resolver does add the segment base. The immediate
cause: the early stub's tail is `decl early_recursion_flag; jmp 0xffffffff81c011f9`
(`restore_regs_and_return_to_kernel`, READ from the image), and that path branches on
`testb $4,0x20(%rsp)` — the **SS slot**. MEASURED at the branch: ip/cs/flags correct, then
`sp = 0x84790` and `ss = 0xffffffff8ae7760d`, which are copy_bootdata's OWN register values — the
restore path is reading two slots PAST the frame the stub built. Bit 2 of `0x0d` is set, so the
kernel takes its user-return branch: `swapgs` with `gs_base = 0xffffffff8ae42000` and
`kernel_gs = 0`, so `GS.base` becomes 0, the next `%gs:0x19000` resolves to the bare `0x19000`,
`early_make_pgtable(0x19000)` refuses (physaddr wraps, `>= MAXMEM` — the demand-repair only knows
direct-map addresses), the kernel panics, and the third nested fault trips
`early_recursion_flag > 2`, which halts **silently**. **NO FIX IS CLAIMED**: two readings fit and the
measurements do not separate them — (1) the early stub is meant to push SS/RSP and the port
mis-executes those two pushes (the cheaper hypothesis, and the shape this lane has paid for twice), or
(2) the kernel relies on `sp`/`ss` being stale and QEMU's stack happens to hold a value with bit 2
clear. **Next fork:** the stub's push list (link `0xffffffff8307715c`…`0xffffffff83077171`) against
pt_regs' 21 slots, and the same slot's content on QEMU. Instrument added: `V86C_PEEK_SP` (rsp-relative
virtual dump — the kernel stack is not identity-mapped) and `gs_base`/`kernel_gs`/`fs_base` in the
break dump. **The first `printk` is still not reached.** Full battery green, every pin unmoved.

**RETZERO (2026-10-21): THE #UD IS `SWAPGS` — NOT A `ret` POPPING ZERO.** `results/x86-64-retzero.md`.
**The premise is corrected:** `pt_regs.ip` is at **+0x80**, not the +0x78 the previous unit read
(that slot is `orig_ax`), so `regs->ip` was never 0. MEASURED from the frame: ip =
`0xffffffff89a01223`, and the image at that link address is **`0f 01 f8` = `swapgs`**, in the
kernel's `restore_regs_and_return_to_kernel` tail (`pop r15..rdi; add rsp,8; testb $4,0x20(%rsp);
jne; iretq; push rdi; swapgs`). The ring's epilogue and the shared `ret` thunk were a red herring:
the fault is three instructions later. **SWAPGS was simply not implemented** — `instr32_0F01_7_reg`
raised #UD unconditionally — so every exception returning through that path killed the machine.
**FIXED** (`src/cpu/instructions_0f.c`): `0F 01 /7` with `rm=0` in long mode exchanges
`segment_offsets[GS]` with `kernel_gs_base` (the fields the MSRs already use); `rm=1` (RDTSCP) and
non-long-mode stay #UD. Arm: `test_swapgs` (register-only; fails against the pre-fix build).
**ASYMMETRY, measured:** before, halt in `early_fixup_exception` with trapnr 6 at `swapgs`; after, no
#UD — the return path runs (`mov %cr3,%rdi; and $~0x1800,%rdi; mov %rdi,%cr3`, then
`mov %gs:0x19000,%rdi`) and the new stop is a **#PF at that per-cpu read**, `cr2 =
0xffff88800001fb40` (`gs_base = 0xffff888000006b40 + 0x19000`), halted by the recursion guard
(`PEEK 0x312c000 = 3`; `0xffffffff8ae9c800` is `ja` → halt), i.e. **three nested exceptions**. The
direct-map chain for that address EXISTS (`PML4[273]=0x30ec063 → PUD[0]=0x30ecd063`), so the repair
is not refusing. **Next fork:** why the handler's own path faults — break on
`early_idt_handler_common`'s entry (`0xffffffff8ae77120`, which does `incl early_recursion_flag`),
count hits, `PEEK 0x312c000` at each, and find whether the decrement at the handler's exit is
reached. **The first `printk` is still not reached.** Full battery green, every pin unmoved.

**REPAIR (2026-10-21): THE DIRECT-MAP REPAIR NOW WORKS — A 32-BIT CR2 WAS REFUSING IT.**
`results/x86-64-repair.md`. Candidate (a) of the fork, confirmed with BOTH SIDES of the comparison
visible: `__early_make_pgtable` (link `0xffffffff83077283`) opens with
`physaddr = address - page_offset_base`, `cmp MAXMEM`, `jae → return false`; MEASURED before the fix,
the entry got `rdi = 0x84790` and the `cmp` saw `rdx = 0x778000084790` against
`rax = 0x400000000000` — so it refused, `PML4[273]` stayed 0 and the panic was the #PF (trapnr 14).
The address was truncated because the guest's `native_read_cr2()` (`48 0f 20 d7`) went through a
32-bit read: `cr2_64` was CORRECT (probe: `cr2_64=0xffff888000084790`) but the body's REX.W test
could never fire — `cpu->prefixes` is 0 by the time the `0F 20` body runs, MEASURED. **FIXED** in
`src/cpu/instructions_0f.c`: a long-mode read of CR2 returns the whole `cr2_64` unconditionally (the
SDM's default operand size for `MOV CR` in IA-32e is 64 bits), and a long-mode WRITE takes a 64-bit
source (`read_reg64`) — the mirror defect. **ASYMMETRY, measured:** entry `rdi=0xffff888000084790`,
guard `rdx=0x84790` passes, the store at `0x8ae77456` targets `rsi=0xffffffff8aeea888`
(`early_top_pgt + 273*8`) with `rax=0x30ec063`, and **`PEEK 0x30ea888 = 0x30ec063`** — `PML4[273]`
mapped to `early_dynamic_pgts[0]` (`0x30ec000`). `copy_bootdata`'s memcpy retries and succeeds, and
the kernel is past the fault that stopped every unit since `results/x86-64-reach2.md`. **What is
left, named:** the stop has moved to a **#UD** (trapnr 6) with the frame's `regs->ip = 0`, taken at a
shared `ret` thunk after a function epilogue — the next fork is "what does that `ret` pop", with
`V86C_BREAK` on the thunk and `V86C_PEEK` of the popped slot. **The first `printk` is still not
reached.** Unit arm: `test_cr2_64bit_read` (register-only, no paging; fails against the pre-fix
build). NAMED GAP: the `0x66`-prefixed 32-bit CR2 read in long mode cannot be distinguished (the
prefix state does not reach the body). Full battery green, every pin unmoved.

**DMAP (2026-10-21): THE DIRECT MAP IS DEMAND-MAPPED — THE REPAIR RUNS AND DOES NOT STICK.**
`results/x86-64-dmap.md`. **CORRECTION to the premise of the unit before it:** no `__startup_64` loop
writes `PML4[273]`. `READ` from the source of *this* image (tag `v6.1.176`):
`x86_64_start_kernel` calls `reset_early_page_tables()` (memsetting `early_top_pgt[0..510]`,
deliberately killing everything but the kernel map) and then `copy_bootdata(__va(real_mode_data))`;
the #PF that read takes is *supposed* to happen and `do_early_exception` is supposed to repair it
with `early_make_pgtable(native_read_cr2())`, building the 2 MiB entry and returning so the memcpy
retries. So `PML4[273] == 0` before the fault is CORRECT. **MEASURED:** the port does enter the
repair — `V86C_BREAK` on `0xffffffff8ae770c0:0x200` hits `0xffffffff8ae771df`
(`reset_early_page_tables`) from `__early_make_pgtable`'s `again:` path with
`rdx=0xffffffff8aeea000` and `rcx=0x3fe` (the 510-entry memset) — so it repairs, the mapping does not
take, the same fault recurs, `early_dynamic_pgts` is exhausted, the reset wipes the table again, and
`early_fixup_exception` panics. The panic string, READ from the image at link `0xffffffff82346118`,
is `PANIC: early exception 0x%02x IP %lx:%lx error %lx cr2 0x%lx`; MEASURED at that panic:
`regs->cs = 0x10` (not the cs branch), `early_recursion_flag = 1` (not the >2 branch), `trapnr = 0x0e`
— reached because `fixup_exception` returned 0 for a genuine #PF. **FIXED (a precondition):**
`cr2` was 32 bits (`int32_t cr[8]`, with the gap in its own comment) while the repair's first act is
`address - __PAGE_OFFSET`; a `uint64_t cr2_64` now carries the whole linear address, set where a #PF
is raised and returned by a REX.W `mov r64, cr2` in long mode, with `cr[2]` untouched so no 32-bit
path can move. It did **not** change this run (4,475,558,722 instructions, `in_hlt=1`), so CR2's
width is not the binding constraint on the path taken. **Next fork, instrumented:** the two guards
(`physaddr >= MAXMEM`, `read_cr3_pa() != __pa_nodebug(early_top_pgt)`) and the `*pgd_p = …` stores at
link `0xffffffff83077456`/`0xffffffff830774ce`, with `V86C_WATCH=0x30ea888` on `PML4[273]` to see
whether an entry is ever installed and then lost. **The first `printk` is still not reached.** Full
battery green, every pin unmoved. NAMED GAP: no unit arm for the CR2 widening (its probe is
integration-level).

**HIGHPF (2026-10-21): THE STOP WAS THE #PF DELIVERY, NOT THE FAULT.** `results/x86-64-highpf.md`.
The instruction at `0xFFFFFFFF8AE77246` is `copy_bootdata`'s `rep movsd` (link
`0xffffffff83077246`, `&boot_params` relocated to `0xffffffff8b117600`, source `__va(0x84790)` =
`0xffff888000084790`); the high-half text maps correctly (`PDE[87] = 0x30001e3`, 2 MiB at phys
`0x3000000`, and the fetched bytes match the image's). **The first fault is the SOURCE read of the
direct map** (`WATCH F 0xffff888000084790 read`), and every delivery then faulted at `0x8b1100e0` —
`IDT[14]` read at a **truncated 32-bit base**, because `lgdt`/`lidt` applied the *operand-size* mask
(`0xFFFFFF` / `-1`) to the eight-byte base long mode had just loaded. **FIXED** in all four of
`lgdt`/`lidt`/`sgdt`/`sidt` (`src/cpu/instructions_0f.c`). MEASURED asymmetry, same instrument and
guest: `idtr` `0x8b110000` → **`0xffffffff8b110000`**, `gdtr` likewise, and the run changes from
"unimplemented: a fault raised while delivering a fault" (rc 1) to a **delivered** #PF with the
kernel's own handler running (rc 0, `in_hlt=1`, `exception=-1`, +74,218 instructions). **What is
left, named:** nothing in the port's run ever maps the direct map — `V86C_WATCH=0x30ea888:8` over the
whole boot shows 18 writes and every one is the decompressor's — so `copy_bootdata`'s `__va()` read
faults, while QEMU (no early exception at all) boots. The kernel's on-demand builder is
`__early_make_pgtable` (`physaddr = address - __PAGE_OFFSET`, guarded by
`read_cr3_pa() == __pa_nodebug(early_top_pgt)`, which holds here); its callers are the next fork, with
`V86C_BREAK` on its entry and `V86C_WATCH` on the `early_dynamic_pgts` pages as the instrument.
**The first `printk` is still not reached.** Full battery green, every pin unmoved. NAMED GAP: the
`lgdt`/`lidt` arm is integration-level only — every long-mode fixture in `m1c_selftest.c` needs the
IA-32e entry edge, which that harness refuses, so a unit arm is owed.

**REACH (2026-10-21): the port DOES execute `__startup_64`.** `results/x86-64-reach.md`.  MEASURED
with a new EXECUTION watch (`V86C_BREAK`, with registers, ring-at-hit, chain and physical peek):
the kernel's entry at `0x1000000` runs `sme_enable` → `verify_cpu` → `__startup_64(physaddr, bp)`
→ `sme_postprocess_startup` → the CR3 formation, and `__startup_64`'s fixup loops and its
`phys_base` tail store (at phys `0x2a1a010`) execute.  Its return of 0 is CORRECT for a non-SME
machine, and the CR3 it forms (`0x30EA000`) is the value the kernel's own arithmetic calls for
(`early_top_pgt - __START_KERNEL_map`, with the decompressor's `+0x7E00000` relocation cancelled by
the kernel's `-0x7E00000` global).  So the stop is NOT a missing `__startup_64` and NOT a wrong
CR3.  What is missing is the *page-table stores*: a 24 KiB watch over `early_top_pgt` and the
dynamic page tables sees only the decompressor.  **And the previous units' "no writer" reasoning
was unsound** — the per-store hook lived in the non-`_addr` `safe_write*`, so every long-mode store
and every bulk copy was invisible; the toolkit now has `v86c_dbg_watch_range` for bulk copies (the
`*_addr` half was closed concurrently in `paging.c`).  Named next forks: `%r10` at `0x1000613`
(where the identity-map loop stores) and the decompressor's `output`/delta divergence
(`0x8E00000` chosen, `0x1000000` used).  NOTE: `make selftest` is RED on a concurrent writer's
unfinished `test_watch_sees_long_mode_store`; the collision is recorded in the report, §7.

**REACH-2 (2026-10-21): THE STORES WERE NEVER MISSING — THE WATCH WAS BLIND.** `results/x86-64-reach2.md`.
CORRECTION to the paragraph above, which stands as the record of what that run could see.  The
`*_addr` half was NOT closed when the paragraph was written: `V86C_WATCH` could not see *any*
long-mode store, because every 64-bit store body calls `safe_write*_addr()` directly.  With the hook
moved into the four `_addr` twins (`src/cpu/paging.c`), the same watch on the same guest shows
`__startup_64`'s identity-map stores landing correctly: `early_top_pgt[0] = early_top_pgt[1] =
0x30ec063` (present, RW, pointing at `early_dynamic_pgts[0]`), written by `rip=0x10007bb`/`0x10007c0`.
MEASURED asymmetry: before, a 24 KiB window over the early page tables logged 24,589 writes, **all
from the decompressor**; after, the kernel's two `pgd` stores appear.  So `__startup_64` runs, its
fixup lands, and `cr3 = 0x30EA000` is correct — the last three units' "the kernel never writes
`early_top_pgt`" is VOID (a control establishes attributability, never exclusivity, and this
control's domain was the wrong store path).  The guest's stop is unchanged and is now known NOT to
be a page-table defect: `eip=0xFFFFFFFF8AE77246`, `cr3=0x30EA000`, 4,475,484,504 instructions,
serial ending at the decompressor's KASLR lines; **the kernel's first `printk` is not reached.**
Named next fork: the #PF at that high-half address, whose chain the toolkit can now report
correctly for the first time.  `make selftest` is GREEN; full battery green, every pin unmoved
(bench 200,095,214, determinism 4,000,096,757, transcript byte-identical).

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

**THE STARTUP64 UNIT: __startup_64's FIXUP NEVER RUNS -- NOT EVEN THE HIGH-HALF ENTRY IS WRITTEN
(2026-10-19).**  `results/x86-64-startup64.md` is its report; MEASUREMENT ONLY.  THE RIGHT TEST, and why
the bisect unit's was not damning on its own: a kernel running at 0xFFFFFFFF9C27... lives in the HIGH
half, i.e. early_top_pgt[511] at 0x30EAFF8 -- if the fixup had run, THAT entry would carry a present,
kernel-owned PDPT pointer.  A V86C_WATCH on it shows **every byte written by rip=0x4F399C0, the
DECOMPRESSOR's output flush, and by nothing else** (bytes 0x0010000500000018, low bit 0 = NOT PRESENT).
So THE KERNEL NEVER WRITES early_top_pgt AT ALL -- not the low entry (bisect unit) and not the high one --
therefore __startup_64's page-table fixup DOES NOT RUN, and the kernel is nevertheless executing at
0xFFFFFFFF9C27..., an address whose PML4 entry is not present, so those fetches are served by the 64-bit
TLB (translations cached before the tables were replaced -- the same stale-TLB shape firstfault recorded).
RULED OUT: (b) a store with a wrongly computed address (there is no store at all), (c) a store with a
wrong width or segment, and any mis-sized/mis-addressed write to the watched entries (the watch is
byte-granular).  WHAT IS LEFT: (a) or (d) -- the port never REACHES __startup_64 (control flow diverges
earlier; the ring's ret-thunk entries show it reaches C, not necessarily the fixup), or it reaches the
routine and BRANCHES PAST the fixup on a flag or compare the port gets wrong (the S8-rotate/jz class).
NEXT MEASUREMENT: does the port execute __startup_64 at all? -- the ring widened to keep more boundaries
plus a V86C_WATCH on the whole early_top_pgt PAGE with a kernel-rip filter (grep -v 0x4f3 on the watch
output); if no kernel instruction wrote anywhere in that page, the ring's eip sequence around the stop
names the branch the port took instead.  Subset gates green, no pin moved.

**THE BISECT UNIT: THE DIVERGENCE IS THE KERNEL'S FIRST printk, AND THE PML4 ENTRY IT NEEDS WAS NEVER
WRITTEN BY THE KERNEL (2026-10-19).**  `results/x86-64-bisect.md` is its report; MEASUREMENT ONLY.
(1) THE FIRST DIVERGENCE LINE (both at 256 MiB, same image and cmdline, earlyprintk+console on):
QEMU's line 2 is `[0.000000] Linux version 6.1.0-50-amd64 (debian-kernel@lists.debian.org) ... Debian
6.1.176-1`; the port never prints it. The port and QEMU agree up to and INCLUDING the decompressor's
KASLR messages (which the discriminator unit proved are the normal path at a small size), so the port's
kernel dies BETWEEN the handover and that first printk -- READ: that line comes from setup_arch's
pr_info(linux_banner), AFTER __startup_64's page-table fixup and after the kernel installed its own CR3.
(2) THE NAMED ACCESS BEHIND IT: a V86C_WATCH on 0x30EA000 (the first PML4 entry, 8 bytes, at the
kernel's cr3) shows EVERY write to that entry in the whole run coming from **rip=0x4F399C0, the
DECOMPRESSOR's output flush** (byte by byte, values are image data: 0x0015FFE800180015).  THE KERNEL
NEVER WRITES IT -- so the entry its 4-level walk reads as PML4[0] is decompressed image bytes, not the
fixup __startup_64 is supposed to store, which is exactly why the walk fails at a later level
(cr2=0x8B1100E0, the IDT descriptor) and the delivery cannot proceed.  INTERPRETATION (the crux):
**__startup_64's page-table fixup did not run, or did not write** -- a named single-instruction question
(its stores to early_top_pgt, and the mov %rax,%cr3 that follows).  (3) NO FIX CLAIMED: the measurement
names the MISSING WRITE, not the defect, and the three shapes (the fixup was skipped by a wrong branch;
it wrote elsewhere -- the address-computation class this lane has hit four times; or cr3 is not the table
it fixed up -- a CR3/CR4 ordering) have different fixes and two of them would change the machine.  NEXT:
V86C_WATCH on the early_top_pgt physical address PLUS V86C_RING, so one run shows both the stores the
kernel DID make and the instruction before the stop.  Subset gates green, no pin moved.

**THE MEMSIZE UNIT: MEMORY SIZE DOES NOT CHANGE THE PORT'S PATH -- THE DIVERGENCE IS SIZE-INDEPENDENT
(2026-10-19).**  `results/x86-64-memsize.md` is its report; MEASUREMENT ONLY.  THE AXIS VARIED FOR THE
FIRST TIME IN THIS LANE, and the answer is a FINDING: at **80 MiB** (with build/minibios-80m.bin) the
port stops at `eip=0x87A77246`, cr3=0x30ea000, 4,475,484,647 instructions with the SAME class of stop as
at **256 MiB** (`eip=0x8AE77246`, 4,475,484,504) -- the #PF delivery recursion -- while QEMU at 80
reaches its OOM panic (`System is deadlocked on memory`) and at 256 its root mount
(`VFS: Unable to mount root fs`).  The two port addresses differ only because the kernel's physical
layout follows the memory size; the STOP is the same class and the instruction count agrees to within
143.  SO THE DIVERGENCE IS SIZE-INDEPENDENT and is NOT the 32-bit physical model, the mmap holes, the
initrd placement (none on either side) or the firmware's heap/stack pointers (heap_end_ptr=0xDE00 at
both) -- it is the same early-kernel fault: a #PF arriving while the kernel's own IDT page is unmapped,
which the recursion bound names.  THE CALIBRATED ACCEPTANCE TEST IS STATED AND NOT MET: at 80 MiB the
port should reach QEMU's OOM panic, at 256 the root mount; it reaches NEITHER, stopping before its first
printk reaches the serial -- so there is NO 'first real amd64 kernel panic' milestone and none is
claimed.  A gate step asserting it is deliberately NOT added (a step asserting the PORT'S stop would be
the 'assert the stop' pattern this lane has spent three units correcting).  NEXT MEASUREMENT (the only
one left on this axis): did the kernel ever TRY to print? -- V86C_WATCH on the serial port's I/O writes
(0x3F8) with V86C_RING + V86C_DUMP_ON_STOP at the stop, separating 'died before console_init' from
'printed but the port's UART dropped it'.  Subset gates green, no pin moved.

**THE DISCRIMINATOR UNIT: QEMU AT 80 MiB PRINTS THE SAME TWO KASLR MESSAGES, SO THE DIVERGENCE IS NOT
KASLR (2026-10-19).**  `results/x86-64-discriminator.md` is its report; MEASUREMENT ONLY, one QEMU run.
`qemu-system-x86_64 -machine pc -m 80 -enable-kvm -no-reboot -display none -kernel
tests/images/vmlinuz-6.1.0-50-amd64 -append 'console=ttyS0 earlyprintk=serial' -serial file:` prints,
from the top: `Probing EDD ... ok`, **`Invalid physical address chosen!`**, **`Physical KASLR disabled:
no suitable memory region!`**, then `Linux version 6.1.0-50-amd64 ...`, `Memory KASLR using RDTSC...`,
`Kernel Offset: 0x19600000`, and `---[ end Kernel panic - not syncing: System is deadlocked on memory
]---` (the correct end for 80 MiB).  SO THREE UNITS OF HYPOTHESIS ARE CLOSED: (i) the two KASLR messages
are the NORMAL early-boot path at a small memory size, NOT a port divergence -- QEMU shows them too and
boots; (ii) results/x86-64-kaslr.md's 'the port's kernel rejects the address it chose' and
results/x86-64-bootparams.md's init_size-vs-window arithmetic are both beside the point; (iii) the 80 MiB
firmware run that still printed the same lines was not evidence of anything either.  THE DIVERGENCE IS
LATER: QEMU gets from those messages to its own panic; the port gets from the same messages to a #PF whose
delivery re-enters four times (eip=0x87A77246, cr3=0x30ea000) -- and the port DOES enter the kernel (it
runs at eip=0x87A.../0x8AE... on the kernel's own tables), so it dies BETWEEN the handover and
console_init, before its first printk reaches the serial.  NEXT MEASUREMENT: V86C_RING +
V86C_DUMP_ON_STOP at the stop, plus a V86C_WATCH on the serial port's I/O writes (0x3F8) to see whether
the kernel ever TRIED to print -- which distinguishes 'died before console_init' from 'printed but the
port's UART dropped it', two very different defects.  (2) A second amd64 image was NOT needed: the
discriminator answered what it was for, and the discipline for adding one is recorded in
results/x86-64-qemu.md ss4.  Subset gates green, no pin moved.

**THE BOOTPARAMS UNIT: THE FIELDS MEASURED, AND THE init_size HYPOTHESIS REFUTED (2026-10-19).**
`results/x86-64-bootparams.md` is its report.  NEW TOOLKIT SURFACE (default off):
`V86C_BOOTPARAMS=<addr>` with V86C_DUMP_ON_STOP prints the standard zero-page fields.  THE DUMP (at
rsi=0x84790): e820_entries=4, e820[3]=[0x100000,0x4000000) type 1 (a 63 MiB usable region),
code32_start=0x100000, cmd_line_ptr=0x8e000, heap_end_ptr=0xde00, initrd_addr_max=0x7fffffff,
kernel_alignment=0x200000, relocatable=1, pref_address=0x1000000, **init_size=0x3F97000 (63.6 MiB)**.
WHICH FIELDS THE FIRMWARE CAN GET WRONG AND WHICH IT CANNOT: init_size, pref_address,
kernel_alignment, relocatable_kernel and cmdline_size are read from the KERNEL IMAGE's own setup header,
so they cannot differ between the two machines for the same image -- and they MATCH; the fields the
firmware writes are the E820, the cmdline pointer and the heap/stack pointers.  THE TEMPTING ARITHMETIC
AND ITS REFUTATION (MEASURED): init_size 63.6 MiB vs a 63 MiB usable region looks like the cause, but the
tree ALREADY builds an 80 MiB firmware (build/minibios-80m.bin, MEM_TOP=0x5000000) and running the same
kernel with `--bios build/minibios-80m.bin --memory 256` gives a 79 MiB usable region -- a 64 MiB window
fits -- and the kernel STILL prints 'Physical KASLR disabled: no suitable memory region!' and stops at
the same recursion bound (eip=0x87A77246).  SO THE WINDOW SIZE IS NOT THE CAUSE.  WHERE THE DIVERGENCE
MUST BE: the port's serial carries 'Invalid physical address chosen!' BEFORE the fallback --
slots_fetch_random rejecting the address it picked -- which is a validity check inside the kernel, not a
slot search, and the fields it reads are the ones the FIRMWARE writes (the E820 and the cmdline pointer,
plus the mem_avoid the decompressor builds from output/input_data/cmdline).  NEXT MEASUREMENT:
instrument choose_random_location's three inputs as the port hands them over (input_data, input_len,
output, output_len, needed_size and the mem_avoid array) and compare with QEMU's.  NO FIX CLAIMED: the
firmware fields the port writes are not shown to be wrong, and a minibios change would touch every guest
gate with a pinned byte transcript.  Subset gates green, no pin moved.

**THE KASLR UNIT: THE E820 IS THE RIGHT SHAPE, AND THE PORT'S KERNEL REJECTS THE ADDRESS IT CHOSE
(2026-10-19).**  `results/x86-64-kaslr.md` is its report; MEASUREMENT ONLY, no code change.  (1) THE
MAP AS THE GUEST SEES IT (READ minibios.asm:803-807 + both serials): minibios publishes FOUR entries --
[0,0x9FC00) usable, [0x9FC00,+0x400) reserved, [0xF0000,+0x10000) reserved, [0x100000, MEM_TOP) usable
-- and QEMU's for the same 256 MiB is the SAME SHAPE (one big usable region from 1 MiB to nearly the
top, the same low reserved holes).  So THE E820 IS NOT TOO FRAGMENTED and is not the defect: a ~255 MiB
contiguous usable region exists in both, and the memory-size discriminator is already answered by the
QEMU unit's own run (QEMU at -m 256, the SAME memory the port gets, chose Kernel Offset 0x6400000 --
it found a region; the port did not).  (2) THE LINE THAT DISTINGUISHES THE PATHS (MEASURED): the port's
serial carries `Invalid physical address chosen!` followed by `Physical KASLR disabled: no suitable
memory region!`, and QEMU's carries NEITHER.  The first is slots_fetch_random's message: the kernel DID
find candidate slots and then rejected the physical address it picked -- so the port's kernel is not
failing to FIND a slot, it is failing a VALIDITY CHECK on the address it chose.  INTERPRETATION: that
check is against the kernel's mem_avoid/valid-region bookkeeping, built from the BOOT_PARAMS the
firmware hands over -- and the port's minibios and QEMU differ there, not in the E820.  (3) NO FIX
CLAIMED, deliberately: the E820 is the same shape as QEMU's (a 'proper map' would change nothing
wrong), a minibios change touches EVERY guest gate and the 32-bit transcript is a pinned BYTE (the
rule says stop and publish rather than move it on a hypothesis), and the distinguishing line points at
the boot_params.  NEXT MEASUREMENT: dump the boot_params the port hands the kernel (at %rsi = 0x84790,
which the toolkit's dump already prints) and compare field for field against QEMU's -- e820_entries,
the e820 table's first entries, hdr.code32_start, hdr.init_size, hdr.pref_address and the cmdline
pointer.  Subset gates green, no pin moved.

**THE QEMU UNIT: THE SAME IMAGE BOOTS UNDER QEMU, SO THE TRIPLE-FAULT WINDOW IS THE PORT'S
(2026-10-19).**  `results/x86-64-qemu.md` is its report.  THE SECOND REFERENCE, READ from
gate-m2-5.sh's gate-1b arm and run on the pinned image with the SAME cmdline and memory (no initrd):
`qemu-system-x86_64 -machine pc -m 256 -enable-kvm -no-reboot -display none -kernel
tests/images/vmlinuz-6.1.0-50-amd64 -append 'console=ttyS0 earlyprintk=serial' -serial file:...`.
**IT BOOTS** -- 23,978 bytes of serial, `Linux version 6.1.0-50-amd64 … Debian 6.1.176-1`, `NX
protection: active`, `Hypervisor detected: KVM`, through `kernel_init` to
`---[ end Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0) ]---`, which
is the CORRECT end for a cmdline with no initrd.  **SO THE LAST UNIT'S CONCLUSION IS REFUTED**: the
guest CAN proceed on this image; the triple-fault window is the PORT's, not the image's, and QEMU's
serial is now the reference sequence to bisect against (the port's serial stops at the decompressor's
KASLR line).  WORKING HYPOTHESIS (INTERPRETATION, not a finding): the port's kernel runs with
`Physical KASLR disabled: no suitable memory region!` while QEMU's runs with `Kernel Offset:
0x6400000` -- the two take DIFFERENT early-boot paths, and the port's is the one that faults; that is
the first thing to compare.  THE GATE: gate-x86-64.sh gains a section E (three steps + an able-to-fail
arm) asserting the SERIAL, not qemu's exit code, because a timeout under load is not a regression --
**gate-x86-64 12/0** (was 9).  THE SECOND IMAGE IS NOT OBTAINED and the reason is stated: the tree holds
ONE amd64 kernel (the others are i386); obtaining a second means a download with its own provenance pin
(URL + sha256) and a size decision, and §1's measurement makes it no longer the NEXT question -- it
would answer a different one (does the divergence reproduce on a kernel with a different early-boot
ordering) and is the natural follow-up to the bisect.  Full battery green, every pin unmoved.

**THE FIRSTFAULT2 UNIT: THE FIRST FAULT IS STILL MASKED, AND THE EVIDENCE SAYS THE GUEST'S OWN STATE IS
THE CAUSE (2026-10-19).**  `results/x86-64-firstfault2.md` is its report.  THE TOOL (code change,
default off): `V86C_FAULT_DELIVERY_BOUND=<N>` -- read in v86c_dbg_init, held in
v86c_dbg_delivery_bound (default **4**, the shipped value), read by call_interrupt_vector's recursion
bound; UNSET = byte-identical behaviour and every pin unmoved.  WHAT IT MEASURED, and it did NOT
converge -- which is itself the finding: (i) **0x9C517600, the rep movsq destination the ring pointed
at, does NOT fault** -- the watch on it never fired, so that lead is dead; (ii) **raising the bound to
16 does not reveal the first fault either** -- the deliveries nest deeper and EVERY one fails at the
SAME descriptor read (0x9C5100E0, the vector-14 gate), because the delivery cannot complete while the
IDT page is unmapped, so no depth exposes what started it.  The instrument the last report proposed
cannot answer the question BY CONSTRUCTION; the stop's message text ("re-entered four times") is a
literal string that now reads slightly wrong when the bound is overridden -- recorded, not reworded.
WHICH SHAPE: the FIRST one.  With PML4E[0] = 0 (measured last unit, port and hand walk agreeing) and
the fault being a READ of the IDT descriptor, the state is "the kernel is between two table switches,
its own IDT not yet mapped"; hardware would #PF -> #DF -> no #DF gate -> TRIPLE FAULT -> RESET, so THE
PORT'S SEAT IS RIGHT and NOTHING IN THE MACHINE SHOULD CHANGE.  The guest cannot proceed past its own
triple-fault on THIS image: the honest next step is a DIFFERENT GUEST OR A DIFFERENT EXPECTATION, not
another stop or another widening.  A REAL answer would need the delivery to be able to complete (the
guest's state, not the port's) or a HARDWARE CROSS-CHECK -- the QEMU leg the plan's ss5 names as the
second reference, a unit of its own.  Full battery green, every pin unmoved.

**THE PF2 UNIT: THE GUEST'S PML4E[0] IS ZERO, SO THE IDT IS GENUINELY UNMAPPED (2026-10-19).**
`results/x86-64-pf2.md` is its report; MEASUREMENT ONLY, made entirely with the toolkit (no hand
probes, no code change).  (1) THE FIRST FAULT: V86C_RING gives the kernel's own stream -- `mov rdi,
0x9C517600; mov ecx, 0x400; mov rsi, rbx; rep movsq` (f3 a5, a 0x400-qword copy), then `call`, then the
faulting access; V86C_WATCH says it is a **READ of 0x9C5100E0**, and idtr = 0x9C510000/4095 makes that
exactly **idtr_base + 14*16 -- the IDT descriptor for vector 14, read by the delivery itself**.  So the
first fault is not a guest data access at all: it is the delivery reading its own gate.  (2) IS THE
TABLE GENUINELY ABSENT?  YES -- V86C_CHAIN (the firstfault unit's method) reads the table bytes
physically: `chain va=0x9c5100e0 cr3=0x30ea000 idx=0/2/226/272` and **`PML4E @0x30ea000 = 0x0`**.  The
top-level entry for the entire low half is NOT PRESENT, and the port's own walk agrees -- so NO stride,
index, frame-mask or truncation defect is involved.  (Compare the firstfault unit's reading of the
SAME cr3: PML4E = 0x30EC063 -- THE TABLES CHANGED between then and now, which is what early-boot table
construction looks like.)  (3) THE DELIVERY: the IDT page is genuinely unmapped, so the delivery's own
descriptor read faults and the recursion bound stops the machine; hardware's sequence here is #PF ->
the vector-14 gate read from an unmapped page -> #DF -> no #DF gate -> TRIPLE FAULT -> RESET, so the
port's named stop is the CORRECT SEAT (the deliver unit's conclusion, reconfirmed at a second address
with a second IDT base).  NOTHING IS PAPERED OVER: the port does not substitute its own fault handling
for a guest fault.  THE QUESTION THAT REMAINS: why is PML4E[0] zero at that instant, and what raised
the FIRST fault (the watch sees only the delivery's descriptor read -- the original fault is masked by
the recursion bound)?  The instrument: the toolkit with the bound lifted to 1 in a debug build, or a
V86C_WATCH on the first trigger_pagefault address (the ring's last entries point at the `rep movsq`
destination 0x9C517600).  That distinguishes "the kernel is between two table switches and its own IDT
is not yet mapped" (a guest transient; hardware resets) from "the port mis-executed the table
construction" (a port defect).  Subset gates green, no pin moved.

**THE STR16 UNIT: THE 16-BIT STRING FORMS ARE REAL -- THE GUARD LIFTED, AND THE KERNEL GETS 11,059
INSTRUCTIONS FURTHER (2026-10-19).**  `results/x86-64-str16.md` is its report.  WHICH SHAPE: the
narrow64 guard's own message named its fear ("66 A5 would otherwise run MOVSQ -- the arms split on
F2/F3 only"), and READ of interpreter_gen.c's `case 0xA5:` shows the 0x66 arm calls the **16-bit
bodies** (instr16_A5 -> movsw_rep -> string_instruction(..., STR_W, ...)), which go through the
long-mode string path the rep unit built.  The fear was misplaced: dispatch_os_index returns **0** for
a 0x66 prefix, so the 0x2NN arm (the MOVSQ one) CANNOT be selected.  So the GUARD WAS LIFTED, not the
bodies duplicated, with that reasoning in the overlay.  ARM: new corpus case **str/word-forms-66**
(the corpus CAN carry it -- a string op is unprivileged): 66 F3 A5 = rep movsw over three elements of
a qword, then rep stosw, then repe cmpsw, RSI/RDI/RCX stored in the window; the width is the
discriminator.  MEASURED: 84/84 green with the guard lifted, and with the guard restored
`total=84 green=83 red_unimplemented=1` (the case RED).  ACCEPTANCE: the kernel is PAST the stop --
4,475,473,445 -> **4,475,484,504 instructions (+11,059)** -- and now stops on the recursion bound
again at eip=0x8AE77246, cr3=0x30ea000: a #PF whose delivery faults again.  NEXT: that #PF, with the
toolkit (V86C_WATCH + V86C_DUMP_ON_STOP + V86C_CHAIN) and the deliver unit's IDT question.  Full
battery green (one gate-m2-5 run reported 9 FAILING in its QEMU leg -- empty QEMU serial -- and the
immediate re-run was 0 FAILING; the port's own leg and the transcript passed in both, and it is
recorded rather than hidden).  Corpus 83 -> 84, str 4 -> 5, overlay census unchanged.

**THE GSFIX UNIT: THE FS/GS BASE WAS DROPPED FOR RIP-RELATIVE OPERANDS -- FIXED, ARM MEASURED, GUEST
MOVED ON (2026-10-19).**  `results/x86-64-gsfix.md` is its report.  THE THREE NUMBERS (toolkit, after
the last unit's previous_ip fix): the instruction is `65 48 89 05 96 9F DB 63` = `mov %rax,
%gs:[0x63DB9F96]` at 0xFFFFFFFF9C2775BA, gs_base = 0xFFFFFFFF9C242000, disp = 0x63DB9F96, and
**RIP-next + disp = 0x9C2775C2 + 0x63DB9F96 = 0x100031558, whose LOW 32 BITS ARE EXACTLY 0x31558** --
the address the port faulted on.  THE DEFECT (READ modrm.c): the RIP-relative branch of
modrm_resolve64 ended with `return offset;` and a comment saying "RIP-relative addresses do not take a
segment base (DS is implied)" -- right for the implied DS (base 0 in 64-bit mode), WRONG for an FS/GS
override, which the SDM (Vol 2, 2.1.6) says selects the segment for ANY memory operand.  So the base
was NOT added at all (candidate (i)); the low-32-bit shape is the FAULT REPORT's width, not a separate
narrowing -- the store path is int64_t end to end (int64_t addr = modrm_resolve -> instr64_89_mem ->
safe_write64_addr).  FIX: add segment_offsets[FS/GS] in the RIP-relative branch, excluding
SEG_PREFIX_ZERO (LEA's sentinel) so LEA keeps computing with no base; CS/DS/ES/SS stay 0.  ARM: the
corpus CANNOT carry it (the native leg would have to set its own FS/GS base and the compared window
would depend on the host's segment state -- the mov cr4 reason again), so it is m1c_selftest's
test_rip_relative_segment_base, MEASURED to fail with the early return restored
(`got 0x3004, want 0xffffffff9c245004`) and pass with the fix.  ACCEPTANCE: the kernel no longer faults
on its percpu store -- it reaches a NEW stop this port named in the rep-string unit: **`long mode: the
16-bit operand form of a string op in long mode is not implemented (see 0xA5)`** at rip 0xFFFFFFFF9C2771F2,
cr3 0x30ea000.  NEXT: implement the 16-bit string forms in long mode (now reachable) or lift the
overlay's narrow64 guard with an arm.  Full battery green, every pin unmoved.

**THE GSBASE UNIT: THE DISPLACEMENT MEASURED -- BOTH CANDIDATES REFUTED, AND A DEFECT IN MY OWN TOOL
(2026-10-19).**  `results/x86-64-gsbase.md` is its report.  (1) The ring (widened from 8 to 16 bytes
for this question) gives the faulting instruction as `65 48 89 05 96 9F DB 63` = **`mov %rax,
%gs:[0x63DB9F96]`** at **0xFFFFFFFF9C2775BA** with gs_base 0xFFFFFFFF9C242000 -- AND THE FIRST RUN
ATTRIBUTED IT TO 0xFFFFFFFF9C2775C2, ONE INSTRUCTION TOO LATE, because the fault hook used
cpu->instruction_pointer when at a fault the cursor has already advanced.  FIXED: the hook now reports
cpu->previous_ip, with the measurement in the file.  (2) BOTH CANDIDATES REFUTED: with base
0xFFFFFFFF9C242000 and disp 0x63DB9F96, NO base handling produces 0x31558 -- the architecturally
correct address is 0xFFFFFFFFFFFFBF96 -- and the disp is a positive 1.6 GB value, not 0x31558-gs_base.
So 0x31558 IS A THIRD SHAPE, and neither of the two shapes the last report offered is the one the port
computed.  (3) NEXT MEASUREMENT, exact and already carried by the toolkit: watch the RESOLVED address
and print inside modrm_resolve64, for a GS-prefixed operand, the base, the offset and their sum -- one
flag plus one line and the third shape becomes a number.  NO FIX CLAIMED.  Gates green, no pin moved
(the changes are the toolkit's, not the machine's).

**THE DEBUGTOOLKIT UNIT: THE TOOLS EXIST, DEFAULT-OFF, AND THEY ANSWERED THE 0x31558 QUESTION
(2026-10-19).**  `results/x86-64-debugtoolkit.md` is its report and `docs/testing.md` §2.6 documents the
surface.  FOUR TOOLS behind one env-var family, all no-ops when unset: `V86C_WATCH=<addr>:<len>[,…]`
(writes -- VIRTUAL via safe_write*, FAULTING via trigger_pagefault -- because a write that faults never
reaches a store, and PHYSICAL), `V86C_RING=<N>` (the last N instruction boundaries, dumped on any stop),
`V86C_DUMP_ON_STOP=1` (registers, cr0-cr4, cr2, cr3, efer, idtr/gdtr, cpl, the SEGMENT BASES) and
`V86C_CHAIN=<va>` (the PML4E/PDPTE/PDE/PTE chain -- the walk this lane hand-wrote three times).  Output
is `v86c dbg:` on stderr and UNBUFFERED (setvbuf + an fflush on the stop path), so the two evidence
losses this lane already paid for cannot recur.  COST WHEN OFF is a load and a branch per hook:
**MEASURED -- tests/perf_bench.sh still reports instr=200095214 with the tools compiled in.**  ARM:
m1c_selftest's test_debug_toolkit_fires turns the tools on WITHOUT the environment and checks the
tool's own COUNTER (4 = watch + fault + ring + stop), i.e. the "who reads this on the deciding path"
question asked of the instrument itself.  THE ACCEPTANCE TEST ANSWERED THE QUESTION:
`V86C_WATCH=0x31558:1 V86C_RING=16 V86C_DUMP_ON_STOP=1` printed `WATCH F 0x31558 write
rip=0xffffffff9c2775c2 (the access FAULTED, no store)` and the ring gave the instruction stream, whose
last entry is `0f 20 e0 65 48 89 05 96` = **`mov rax,cr4; mov %rax,%gs:[disp32]`** -- a PERCPU store
reached from the kernel's cr4 bookkeeping.  The machine's GS base is CORRECTLY
0xffffffff9c242000, yet the faulting address is 0x31558 -- the RAW DISPLACEMENT, not gs_base+disp.
INTERPRETATION: the GS base was not added for this access (the 65 prefix not reaching
modrm_resolve64's segment_prefix, or the disp really is 0x31558-gs_base) -- the two are told apart by
printing the instruction's disp32 and the resolved address, one flag away now.  FULL BATTERY green,
every pin unmoved including the bench with the tools present.

**THE FIRSTFAULT UNIT: ALL THREE QUESTIONS ANSWERED -- THE FIRST FAULT IS A WRITE TO 0x31558 AND THE
IDT PAGE IS GENUINELY UNMAPPED (2026-10-19).**  `results/x86-64-firstfault.md` is its report;
MEASUREMENT ONLY (probes removed; tree = commit 801cf8e).  (1) A probe at trigger_pagefault's first
three entries: `PF#1 addr=0x31558 present=0 write=1 user=0 fetch=0 rip=0xffffffff9c2775c2` -- **THE
FIRST #PF IS A WRITE to 0x31558**, a LOW address, so the fault is NOT in the fetch path the earlier
units suspected (the walk for 0xFFFFFFFF9C27759C translates); `PF#2/#3` are the IDT descriptor at
0x9BC040E0, i.e. the cascade the deliver unit measured.  (2) THE CHAIN, walked by hand from cr3
0x30EA000: `va=0x9bc03000 i4=0 i3=2 i2=222 i1=3 e4=0x30ec063 e3=0x0 ...` and `va=0x9bc04000 ... i1=4
e4=0x30ec063 e3=0x0` -- **THE PDPTE IS ZERO FOR BOTH PAGES**, so the IDT page is GENUINELY UNMAPPED and
there is NO port off-by-one in the stride, index arithmetic or frame mask.  AND IT CORRECTS AN EARLIER
READING: the stack page 0x9BC03000 is ALSO absent, yet the walk for 0xFFFFFFFF9BC03F40 returned
pf=0/frame 0x2A03F40 -- **that translation was a 64-bit-TLB HIT, not a walk** (architecturally
permitted: the TLB is not coherent and the guest must invalidate), so "the stack translates" was a
cache hit.  (3) The IDT base 0x9BC04000 sits immediately above the boot stack, which is exactly where
a .data/.bss IDT table belongs, so it is plausible and NOT mis-computed (READ head64.c:625-637: the
IDT is loaded before the first C call).  IS THE SEAT CORRECT?  YES: hardware's sequence at this state
is #PF -> the vector-14 gate is read from an unmapped page -> #DF -> no #DF gate (idt[8] zero,
measured) -> TRIPLE FAULT -> RESET; the port stops with the reason named and the state intact.  Adding
a reset path is a POLICY question and would destroy the evidence this chain used -- it is recorded as
a KNOWN DIVERGENCE (the port cannot reproduce a machine that reboots in a loop), not implemented.
NEXT, with instruments: the store to 0x31558 (instruction bytes at rip 0xFFFFFFFF9C2775C2, the
destination register, the value) -- is it the kernel writing where it expects the DECOMPRESSOR's
identity map to still cover, or a consequence of an earlier mis-execution? -- and the stale-TLB
reading of (2).  Subset gates re-run green, no pin moved.

**THE DELIVER UNIT: THE DELIVERY FAULTS AT ITS FIRST ACCESS -- THE KERNEL'S IDT PAGE IS UNMAPPED
(2026-10-19).**  `results/x86-64-deliver.md` is its report; NO FUNCTIONAL CHANGE (probes only, removed;
the tree is commit 801cf8e).  A probe at the FIRST thing the delivery does -- the IDT descriptor read
-- printed, unbuffered, `PROBE desc-read: vector=14 lin=0x9bc040e0 phys=0x0 pf=1` four times and then
the recursion bound fired.  SO THE DESCRIPTOR READ ITSELF FAULTS, and the three candidates the last
unit named are all REFUTED BEFORE THEY ARE REACHED: (a) the address computation is arithmetically
right (`0x9BC04000 + 14*16`), it is the TRANSLATION that fails; (b) the frame push never runs
(`writable_or_pagefault`/`push64` are not reached, and both were widened by stack64); (c) the
CPL-changing arm is not entered (`cpl=0`).  WHAT THAT MEANS: the IDT base is 0x9BC04000 and the boot
stack that DOES translate sits at 0xFFFFFFFF9BC03F40 -- the page immediately BELOW it -- so the
kernel's identity map covers the stack page and stops at the page boundary, leaving the IDT page
unmapped.  On hardware the same sequence raises #DF, and the bringup IDT has gates only for #PF and
#VC (measured earlier: idt[14] non-null, idt[8] zero), so a #DF there is a TRIPLE FAULT -> RESET.
INTERPRETATION: the port now behaves as the architecture does at this point -- it stops with the reason
named where the hardware resets -- and the recursion bound is what makes that visible instead of fatal.
NOTHING WAS CHANGED, deliberately: the brief's rule is that the outcome must be the guest's own fault
handling, and widening the delivery would not deliver anything on an unmapped IDT, only recurse again.
NEXT, three measurable questions: (1) WHAT WAS THE FIRST FAULT -- probe trigger_pagefault's addr/cr2 on
the first entry (the walk for 0xFFFFFFFF9C27759C TRANSLATES, so the #PF came from a different access);
(2) IS THE IDT PAGE MAPPED IN THE GUEST'S TABLES OR IS THE PORT'S WALK WRONG -- dump the
PML4E/PDPTE/PDE/PTE chain for 0x9BC03000 vs 0x9BC04000 and compare; (3) if the guest really faults
before its IDT is mapped, WHAT PUT IT THERE -- startup_64_load_idt loads bringup_idt_descr BEFORE the
first C call, so an unmapped IDT is either the kernel's own ordering or a consequence of an earlier
mis-execution.  Subset gates re-run green, no pin moved.

**THE WALKOOB2 UNIT: THE INSTRUMENT FIXED, THE QUESTION ANSWERED, THE HOST CRASH BOUNDED (2026-10-19).**
`results/x86-64-walkoob2.md` is its report.  (1) THE INSTRUMENT: `setvbuf(stderr, NULL, _IONBF, 0)` at
the top of main -- the second time this lane has lost evidence to its own tooling (the 160-byte stop
message was the first) -- and the lost lines came back.  (2) THE ANSWER, MEASURED: **THE WALK
TRANSLATES** -- `walk enter/exit addr=0xffffffff9c27759c fetch=1 -> entry=0x307759f pf=0`, i.e. frame
0x3077000, inside RAM -- so the stack64 report's "inside safe_read64_addr" and the walkoob report's
"the walk's PML4-511 path" are BOTH REFUTED.  The crash is in the **#PF DELIVERY**: the kernel's own
IDT is at 0x9BC04000 (a linear address above 4 GiB), each delivery faults again, and
`call_interrupt_vector` RECURSED WITHOUT BOUND until the HOST'S STACK OVERFLOWED (the silent SIGSEGV
explained).  (3) THE FIX, a bound on the EMULATOR's recursion, not a refusal of the guest's deliveries:
`call_interrupt_vector` is now a thin wrapper over `call_interrupt_vector_inner` with a delivery-depth
bound of 4; hardware answers a fault during a fault with #DF (and during #DF with a triple fault) and
this port must not recurse into the host stack.  MEASURED: the run that used to die silently now ends
`4475480778 instructions ... unimplemented=1 eip=0x8ae775ba cr3=0x30ea000` with the named stop "a
fault raised while delivering a fault: the delivery re-entered four times (hardware would raise #DF;
the port stops rather than recursing)".  **The guest does NOT yet run its own code.**  NEXT: WHY the
delivery faults -- probe inside the delivery for the descriptor address and its translated physical
address, the stack pointer and the writable_or_pagefault result, and which read/write sets
cpu->pagefault (candidates: the descriptor read's address computation, the frame push onto a stack at
0xFFFFFFFF9BC03F40, or the CPL-changing arm that still stops by name).  THE ARM IS NOT WRITTEN and is
recorded as a gap: the bound's failure mode WAS measured (silent SIGSEGV before, named stop after) but
that is the guest, not a reproducible arm; a fixture whose fault delivery itself faults is the next
unit's work.  Full battery green, four M0 pins unmoved, no assertion count changed.

**THE WALKOOB UNIT: THE CRASH IS LOCALISED FURTHER, NOT FIXED -- AND THE KERNEL NOW RUNS IN HIGH
MEMORY (2026-10-19).**  `results/x86-64-walkoob.md` is its report; NO FUNCTIONAL CHANGE (probes only).
The last unit's hypothesis is **REFUTED BY MEASUREMENT**: the frame reads are NOT the crash -- a probe
inside safe_read64_addr printed `0xffffffff9bc03f40 -> phys 0x2a03f40` and `0xffffffff9bc03f48 ->
0x2a03f48`, both inside the 256 MiB of RAM, so the widened stack path WORKS.  A probe at
interpreted_run printed `eip=0xffffffff9a2000e3 phys=0x10000e3` (and 0xe5/0xe7/0xfa): **THE KERNEL IS
NOW EXECUTING AT KERNEL VIRTUAL ADDRESSES**, mapped through its own page tables to its own text at
16 MiB -- the first time this lane has done that.  The crash is at the FETCH of a `far_return` to
**0xFFFFFFFF9C27759C** (csbase 0): no interpreted_run line follows it.  WHY IT IS NOT YET PINNED, said
plainly: the probe lines for that last address were LOST -- with stderr redirected the probe output is
block-buffered and a SIGSEGV discards it (MEASURED as inconsistency between runs of the same binary:
one printed the far_return target line, the next did not, a third lost the walk's enter/exit lines).
THE INSTRUMENT FAILED, NOT THE GUEST.  NEXT MEASUREMENT, with its instrument named: re-run the same
probes with `setvbuf(stderr, NULL, _IONBF, 0)` (or an fflush after each), so the walk's enter/exit line
for 0xFFFFFFFF9C27759C survives -- that line answers the only question left (translate, and the crash
is downstream in the fetch guard/TLB store; or fault, and the crash is in the #PF delivery; the
kernel's bringup IDT DOES have a #PF gate, measured earlier).  THE FIX MUST BE A BOUND, not a refusal:
the outcome must be the translation or the guest's #PF, never a host death.  Gates re-run on the
probe-free tree: make, selftest (all suites green), nasm 118/118, transcript byte-identical,
determinism 4,000,096,757, gate-x86-64 9/0, gate-long-mode 57/0, gate-long-mode-sys 5/0,
gate-x86-64-diff 13/0, gate-nasm64 13/0 -- no pin moved.

**THE STACK64 UNIT: THE STACK PATH IS 64-BIT, AND THE CRASH MOVED INTO THE PAGE WALK (2026-10-19).**
`results/x86-64-stack64.md` is its report.  `misc_instr.c`'s four stack helpers each gained a LONG-MODE
arm AHEAD of their untouched 16/32-bit one: `get_stack_pointer`/`get_stack_reg` return **int64_t**
(`get_seg_ss() + read_reg64(ESP) + offset`), `set_stack_reg` takes int64_t, `adjust_stack_reg` writes
RSP at 64 bits -- and the full-width address flows into the resolver unchanged because `safe_*` have
taken int64_t since the "widen the bodies" unit.  MEASURED (probe, since removed): the kernel's third
RETFQ now computes `rsp64=0xffffffff9bc03f40 addr=0xffffffff9bc03f40` -- the address the resolver
receives IS the 64-bit RSP (it was 0x9bc03f40).  THE GUARD: the last unit's `instr64_CB` RSP>=4 GiB
stop is **REMOVED, not disabled** -- with the stack widened it would be a FALSE STOP on a valid guest
state; the physical bound stays where it belongs (`interpreted_run`'s fetch guard and the walk's
WALK64_TABLE/frame bound, paging.c:448-457), and the distinction is LINEAR vs PHYSICAL: a 64-bit
linear address is now legal, a physical address outside RAM is still a named stop.  THE GUEST STILL
DIES: a HOST SIGSEGV with no message at the same instruction (4475480800 budget), but the probe proves
the stack arithmetic is no longer the cause, so the crash is inside
`safe_read64_addr(0xFFFFFFFF9BC03F40)` -- the 4-level walk on the kernel's OWN page tables
(cr3=0x30ea000), the PML4-index-511 path.  THE CORPUS CANNOT CARRY THE ARM, for a DIFFERENT reason
than mov cr4: a high stack is not privileged, but the harness maps only the image at 0x80000 and the
compared window at 0x100000..0x102000, and a case's stack must be inside that window for the two legs
to agree, so a stack above 4 GiB needs harness aliasing -- the arm belongs in the selftest and is
**NOT WRITTEN YET** (a stated gap).  NEXT: instrument `do_page_walk64` for 0xFFFFFFFF9BC03F40 (each
level's entry address, its value, the frame it produces, which bound it passed) -- a host crash is a
defect in this port's contract, so it is the next unit's FIRST job; `iretq` remains untouched.
Full battery green, four M0 pins unmoved.

**THE FARRET UNIT: THE 64-BIT FAR RETURN IS WIDENED, AND IT EXPOSES THE PORT'S 32-BIT STACK PATH
(2026-10-19).**  `results/x86-64-farret.md` is its report.  `far_return` now takes an **int64_t eip**
plus an `is_64` frame selector: the same-CPL frame is `2*8 + stack_adjust`, and the PRIVILEGE-CHANGE
path reads **RSP(8) at +16 and SS(8) at +24 -- AFTER RIP and CS**, the SDM's 64-bit RET order (the
32-bit path never had to spell that out).  `instr64_CB` no longer bounds the target, so the
`>= 4 GiB` stop inherited from the idt64 unit is GONE.  THE CORPUS CANNOT CARRY THE ARM (its native
leg is CPL 3, where a retf/iretq to a ring-0 CS #GPs -- the same reason `mov cr4` could not), so the
arm is m1c_selftest's `test_far_return_64bit_frame` (an eight-byte frame with RIP 0xFFFFFFFF9C27759C,
asserting the full 64-bit target and that RSP moved by 16).  HONESTY: that arm PASSES but also passed
under the mutation that restores the 32-bit frame, so **its discriminating power is NOT established**
-- a seat, not an instrument.  THE ACCEPTANCE TEST FIRST CRASHED THE HOST: with the widening the
kernel's THIRD RETFQ runs at `rsp = 0xFFFFFFFF9BC03F40` -- a kernel VIRTUAL stack -- and the port's
stack path (`get_stack_pointer` -> `read_reg32(ESP)`) truncates it to 0x9BC03F40, past the end of
256 MiB of RAM, so `safe_read64_addr` read out of bounds and the process died with SIGSEGV and NO
message (MEASURED: the three RETFQs are 0x100266/0x1000043/0xffffffff9c27759c with those rsps).
`instr64_CB` now checks `read_reg64(ESP) > 0xFFFFFFFF` first and STOPS BY NAME, so the crash is a
named stop at the same instruction count (4,475,480,768, cr3=0x30ea000, eip=0x88e0015a).  THE NEXT
UNIT IS **THE 64-BIT STACK PATH**: `get_stack_pointer` (misc_instr.c:115), `set_stack_reg`,
push32/pop32/push16/pop16 and every safe_* call that adds `get_seg_ss()` to a 32-bit RSP; the
instrument is the kernel's own high-stack RETFQ.  ALSO STILL OPEN: **`iretq` was NOT widened** --
`iret` is one `iret(bool is_16)` with the 32-bit frame; the 64-bit form needs RIP(8)/CS(8)/RFLAGS(8)
and, on a privilege change, RSP(8)/SS(8) after them.  The guest has not reached an iretq yet, so it is
recorded rather than claimed.  Full battery green, four M0 pins unmoved.

**THE PDPTE UNIT: THE CR4 WRITE'S RELOAD WAS TOO EAGER, FIXED, AND THE KERNEL NOW RUNS ON ITS OWN
TABLES (2026-10-19).**  `results/x86-64-pdpte.md` is its report.  READ of instructions_0f.c `case 4:`
plus paging.c's read_pdpt gives the answer, and it is (ii) AND (i): (ii) the port reloaded the PDPTEs
whenever `old ^ new` touched PAE|PGE|PSE|SMEP, while the SDM reloads them only when **CR4.PAE is SET**
(a PGE/PSE/SMEP change only flushes the TLB) -- the kernel's write is PGE-only (0x20 -> 0xA0, old^new
= 0x80); and (i) in long mode the entry at CR3 is a **PML4E**, and read_pdpt judges it by the 32-bit
PAE PDPTE rules (bits 63:32 must be zero, 0x1E6 reserved), so a legal PML4E (PWT bit 3, PS bit 7, NX
bit 63) made the write #GP.  Candidate (iii) is REFUTED: the table is the kernel's own 4-level PML4.
FIX: reload only on PAE 0->1, and never in long mode (do_page_walk64 reads CR3 directly; reg_pdpte is
not consulted).  THE ARM: the corpus cannot carry it (`mov cr4` is privileged and the native leg runs
at CPL 3 -- MEASURED, the first attempt reported `Exception actual None / expected GP`), so the arm is
the existing cpu_selftest assertion that encoded the DEFECT ("the CR4 write path faults atomically
too"), corrected to the SDM rule with its reason in the file; MEASURED asymmetry with the old trigger
restored: 2 failures (`exc_vector got 0xd want -1`, `cr[4] got 0x20 want 0x30`), 0 with the fix.
**The pin moves 3889 -> 3888** (three assertions became two), stated not absorbed.  An m1c arm was
added too and PASSES but does NOT discriminate (it passes under the mutation) -- recorded as a seat,
not an instrument.  ACCEPTANCE TEST: the CR4 #GP is GONE, the kernel switches to its OWN page tables
(cr3 0x4f76000 -> **0x30ea000**) and runs 110 instructions further (4,475,480,658 -> 4,475,480,768),
stopping on a PRE-EXISTING named gap of this port: `long mode: a RETF target at or above 4 GiB`
(instr64_CB / far_return's int32_t eip, results/x86-64-idt64.md).  NEXT: widen far_return to a 64-bit
eip (both its int32_t consequences are named stops today; the CPL-change path reads a 32-bit ESP/SS
frame too).  Full battery green, four M0 pins unmoved.

**THE JZ UNIT: THE BRANCH IS INNOCENT, THE LA57 MYSTERY IS SOLVED, AND THE STOP IS A DIFFERENT #GP
(2026-10-19).**  `results/x86-64-jz.md` is its report; MEASUREMENT ONLY, no source change.  The probe
printed `cpu->flags`, `flags_changed`, the GETTERS (`getzf()`, `test_z()`) and the lazy operands at
every eip from 0x1000083 to 0x1000099: at the `jz` **getzf=1 and test_z=1** with `changed=0xC4`
(stale ZF), `last_result=0`, `last_op_size=31` -- the lazy getter computes `(~0 & (0-1)) >> 31 & 1 = 1`
correctly.  THE FLAG MACHINERY IS FINE.  The previous unit's fall-through trace was taken BEFORE the
RIP-relative fix, when the `test` read 0x2413D6C (four bytes early, nonzero) and left ZF clear; with
the fix the branch is TAKEN, so `or ecx,0x1000` is SKIPPED and **CR4 is written as 0xA0 (PAE|PGE), not
0x10A0** (INTERPRETATION from the measured branch -- the register was not re-read).  **CORRECTION:
results/x86-64-la57.md's "this defect is real and is NOT the LA57 cause" was WRONG -- it WAS the
cause.**  All three original candidates (wrong CPUID, the kernel writing the variable, the flags read
wrongly) are REFUTED.  THE STOP THAT REMAINS IS A DIFFERENT ONE: `mov cr4` with 0xA0 still #GPs at
eip=0x10000a1 (idtr base 0x2a04000 limit 511, vector 13's descriptor 0).  The port's reserved-bit mask
does not contain bits 5/7, so the #GP comes from the step AFTER it: READ instructions_0f.c `case 4:`
-- when `old ^ new` differs in PAE|PGE|PSE|SMEP the port RELOADS THE PDPTEs (the SDM's MOV-to-CR4
rule) and #GPs on a reserved bit; here `old ^ new = 0x80` (PGE) so that path runs.  NEXT MEASUREMENT:
log which branch of `case 4:` raises it and dump the PML4E/PDPTE it reads (cr3, the entry's address
and value, which bit is called reserved), plus a corpus case doing `mov cr4, 0xA0` in long mode from a
clean state -- if that #GPs in isolation the defect is reproduced without the kernel.

**THE LA57 UNIT: TWO CANDIDATES REFUTED, ONE REAL DEFECT FIXED, THE BRANCH STILL OPEN (2026-10-19).**
`results/x86-64-la57.md` is its report.  (1) THE WRITE TRACE (32-byte watch over 0x2413D60..D7F,
every change logged with its eip): the variable's address in the gp report was WRONG TWICE (hand
arithmetic; `F7 05 disp32 imm32` is TEN bytes, so the base is 0x1000099 and the target is
**0x2413D70**, now derived from the bytes at run time and printed).  The only writers are the
decompressor's output flush and a `rep movsq` memcpy that ZEROES it; at the CR4 write the probe reads
linear 0x2413D70 -> phys 0x2413D70, `safe_read32s = 0x0`, mem8 = 00 00 00 00.  So the kernel's
`__pgtable_l5_enabled` IS ZERO and candidate 3 (the kernel decided LA57) is REFUTED.  (2) THE CPUID
DUMP through the port's own instr_0FA2: cpuid(0,0) max level 0x16; cpuid(7,0) ecx=0 (LA57 bit16 = 0);
cpuid(7,1) all zeros -- candidate 1 (a wrong CPUID answer) is REFUTED.  The eip trace at the CR4 write
still shows the `jz` at 0x1000099 FALLING THROUGH to `or ecx,0x1000`, with the operand measured 0 and
the address measured correct, so the remaining suspect is THE FLAGS THE `jz` READS.  THE THIRD
MEASUREMENT: a probe at 0x1000099 printing cpu->flags and cpu->flags_changed right after the test,
plus a corpus case reproducing the kernel's exact RIP-relative-with-immediate test against a zero
dword.  ONE REAL DEFECT FELL OUT AND IS FIXED: the RIP-relative base was FOUR BYTES EARLY for any
encoding with a trailing immediate (`modrm_resolve` runs before the immediate is read) -- now
`modrm_resolve_imm(modrm_byte, imm_bytes)` with the size passed by the emitter (158 arms); arm
`ctrl/rip-relative-with-imm`, MEASURED to fail with the term removed (7 fields, r8 0x2222 vs 0x1111)
and pass with it (83/83 green).  It is NOT the LA57 cause: the guest runs 4 instructions further
(4,475,480,658) and stops at the SAME `mov cr4, rcx`.  Full battery green, four M0 pins unmoved,
corpus 81 -> 83, ctrl 18 -> 20.

**THE GP UNIT: THE DETERMINISTIC #GP IS LOCALISED, FIXED, AND THE GUEST MOVES ON (2026-10-19).**
`results/x86-64-gp.md` is its report.  The three named measurements were done in one run (2m13s):
(a) THE STACK at 0x1001BD2 shows the stub's frame INTACT (saved RBP 0x2a04000 at 0x2a03f28, the return
address **0x100083a** into head_64.S at 0x2a03f30, `pushq %rsi` = 0x84790 at 0x2a03f50) but **RSP =
0x2a03f2c after `leave` — four bytes short of 0x2a03f30** — so the `ret` popped the qword at
0x2a03f2c, which straddles the two saved words and is **0x0100083A00000000: NON-CANONICAL** -> #GP(0)
into the stub's NULL IDT.  THE DEFECT: `0xC9 LEAVE` had **no 64-bit body** (it is not in
`long_mode_default64()`, so the 32-bit `instr32_C9` ran and popped a FOUR-byte EBP).  FIXED:
`instr64_C9` (RSP=RBP then an EIGHT-byte pop, read before write) + a base-table overlay entry
`{ opcode: 0xC9, default64: true, narrow64 }`; the arm is `ctrl/leave-64`, MEASURED to fail before
(6 fields MISMATCHED with the overlay entry reverted) and pass after (81/81 green).  THE ACCEPTANCE
TEST: the guest runs **2,962 instructions further** (4,475,477,692 -> 4,475,480,654) and stops at a
NEW, named stop: **`mov cr4, rcx` at eip=0x10000A1 with rcx=0x10A0** (PAE|PGE|**LA57**), where the port
#GPs because it treats CR4 bit 12 as reserved — and the idtr has changed from base 0 limit 0 to
**base 0x2A04000 limit 511**, the kernel's OWN bringup IDT, which is `startup_64_setup_env`'s last act
and therefore proof that it COMPLETED.  The LA57 bit came from the kernel's own
`test dword [0x2143D6F], 1` (`__pgtable_l5_enabled`) finding it set, although CPUID leaf 7 ECX is 0 in
the port and the trampoline left no LA57 — NOT claimed as a defect; the FOURTH MEASUREMENT is a
watchpoint on 0x2143D6F plus the stub's `mov %cr4,%rax` result.  (b) THE RELOCATION IS FAITHFUL: the
entry code at 0x1000000 is byte-identical to the pre-copy source at 0x1200000 for 29 bytes (one byte
at offset 29 differs — recorded, not explained); (c) the program-header capture read at the wrong
offset (the header was intact at 300 M AND 500 M), so it is a re-measurement, not an inference.  Full
battery green, four M0 pins unmoved, corpus 80 -> 81, ctrl 17 -> 18, overlay 157/117 -> 158/118.

**THE JUMP UNIT: THE AMD64 KERNEL ENTERS ITS OWN CODE AND DIES ON ITS 56th INSTRUCTION (2026-10-19).**
`results/x86-64-jump.md` is its report; MEASUREMENT ONLY, no source file changed.  The jump FIRES:
at instruction **4,475,477,636** control reaches `eip=0x1000000` with `rsp=0x2a03f58 rsi=0x84790
cr0=0x80050033 cr3=0x4f76000` and the bytes `48 8d 25 51 3f a0 01 48 8d 3d f2 ff ff ff b9 01 ...` --
READ as `startup_64` in arch/x86/kernel/head_64.S:64-82 (`leaq (__end_init_task-FRAME_SIZE)(%rip),%rsp;
leaq _text(%rip),%rdi; movl $MSR_GS_BASE,%ecx; ...; wrmsr; pushq %rsi; call startup_64_setup_env`).
The kernel then runs 56 instructions: the tail of `startup_64_setup_env`, INCLUDING a stack-canary
read through `%gs:0x28` that MATCHES (so the MSR write and GS-based addressing work in long mode),
then `leave; jmp 0x1E01D30`.  THE NAMED STOP: 0x1E01D30 holds a bare `c3` at the end of a 0xCC
padding run followed by multi-byte NOPs and a large zero region; that `ret` raises **#GP (vector 13)**
-- a non-canonical popped address -- delivered into the **NULL IDT** the stub installs before the jump
(`idtr base=0 limit=0`, READ misc.c:463-465), so the machine stops at 4,475,477,692 instructions with
`exception=13 unimplemented=1 eip=0x0`.  On hardware the same #GP would triple-fault, so it is a real
fault, but WHAT makes the popped address bad is NOT established: the stack words at rsp=0x2a03f34 read
`0x0478000000000000 0x0100000000000000 0x0100003600000000 0x084790000000000`, which look BYTE-SHIFTED
against plausible values (0x4780000, 0x1000000, 0x1000036xx, 0x84790) -- offered as a hypothesis, NOT
a finding.  WALL CLOCK (MEASURED, three runs): 112 s / 114 s / 118 s for 4.475 G instructions =
**~38-40 M instructions/s**, so the reloc unit's "~25 min for a 5 G boot" estimate was **wrong by
~13x**: 5 G instructions is ~2m10s, and no run reaches its 5 G budget because the #GP stops it first.
NEXT MEASUREMENT (named, ~2 min each): (a) the stack at 0x1001BD2 before the tail call and the exact
word the `ret` pops; (b) a BYTE-COMPARE of a known code region before and after `parse_elf`
(0x1000000..0x1001000 vs the decompressed image at output+0x200000) -- the prime hypothesis is that
the in-place, overlapping relocation misplaced the image, and the `cc`-then-`ret` shape is what a
mis-shifted copy would produce; (c) decode the five program headers BEFORE parse_elf runs (afterwards
the copy overwrites the ELF header, which is why this unit's late-run header dump was garbage).

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
