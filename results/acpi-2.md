# ACPI-2 — the firmware tables, gated on a consumer that can read them

**Unit:** `HANDOFF=v86c-acpi`, UNIT ACPI-2 (`coder`). **Deliverables:** `src/hw/acpi_tables.{c,h}`
(the RSDP + RSDT/XSDT + FADT + FACS + DSDT + MADT builder, `--acpi`-gated, default OFF),
the two publish sites in `src/main.c`'s boot path, the MADT-mirroring pin map in
`src/hw/machine.c`'s raise/lower path, the hw_selftest tables group (+39 checks), the
G2 initramfs (`tests/mk-initramfs-acpi.sh` → `tests/images/guest-initramfs-acpi.img`),
this report, and the raw captures `results/acpi-2-*`.

**Bottom line.** The tables work at the level they were built for: with `--acpi`, the
Alpine kernel's own ACPICA finds the RSDP at its first scan candidate, loads every table
by name, parses the MADT, discovers our IOAPIC with our addresses, takes the classic
IRQ0→GSI2 override, and switches the machine from virtual wire to symmetric I/O mode —
the BEFORE line *"APIC: ACPI MADT or MP tables are not detected"* is gone and the
"Using IOAPIC for interrupt routing" path is taken. Default-off is byte-identical by
construction and by gate; determinism holds with the flag on (the ON pair is
byte-identical at 808,195,989 instructions; the m2-5 determinism gate is rc 0 both
plain and under the `--acpi` wrapper). The selftest's tables group is able to fail in
five separate ways and caught three real bugs during development.

**What is NOT done: the last inch of G2.** The kernel-side route is proven; the
`/proc/interrupts` label evidence from *userspace* is not, because of a REAL FINDING this
unit measured and did not fix (F1 below): under `--acpi`, userspace console writes never
reach the UART — init blocks on its first tty write and the kernel goes quiet — while the
kernel's own polled printk to the same port streams fine, and the OFF arm prints
everything. A second finding (F2) is that an ACPI-aware kernel enumerates ZERO PCI
devices against the empty DSDT, so virtio/IDE vanish from the machine. Both are
firmware/delivery-level and need the next rung's headroom; everything needed to continue
is in §5.

Every claim is tagged **MEASURED** (run here, command/capture named), **READ** (artifact,
with a path) or **INTERPRETATION**.

---

## 1. What was built

| file | what it is |
| --- | --- |
| `src/hw/acpi_tables.c` / `.h` | the builder: one 0x28C-byte blob at 0xE0000 — RSDP (36, rev 2, both checksums, RSDT+XSDT pointers), RSDT (44: FADT+MADT, 32-bit entries), XSDT (52: the same, 64-bit), FADT (268, rev 5.1, **hardware-reduced**), FACS (64, no checksum — the spec gives it none), DSDT (36, header-only empty namespace), MADT (76: LAPIC + IOAPIC + the IRQ0→GSI2 override, PCAT_COMPAT). `build()` is a pure function of the constants and of `APIC_MEM_ADDRESS`/`IOAPIC_MEM_ADDRESS`; `publish()` is one memcpy; `verify()` is the selftest's oracle over any candidate blob |
| `src/main.c` | two publish sites under `if (acpi)`: the initial firmware copy and the reboot re-entry's (the firmware tables are firmware — boot 2 carries them exactly as boot 1 did) |
| `src/hw/machine.c` | `device_raise_irq`/`device_lower_irq` feed the IOAPIC the PIN the MADT publishes: the PIT's line lands on pin 2 (`IOAPIC_PIN_PIT`), everything else identity. The PIC half is untouched. This is a **declared divergence from upstream** — cpu.rs:4599 raises identity because no upstream guest can read a MADT; the moment ours does, the kernel unmasks pin 2 for its timer and pin 0 would go silent |
| `src/hw/hw_selftest.c` | the tables group (+39 checks, 670 total): placement/stride, `verify()` over the blob, the kernel's own 16-step scan over published guest RAM, the RSDP→RSDT/XSDT→FACP/APIC walk out of guest memory, the MADT fields vs the LIVE apic/ioapic ID registers, the pin-2 delivery + pin-0-silence + pin-4 identity, and five able-to-fail corruptions (§4) |
| `Makefile` | `acpi_tables.{c,h}` join `HW_SRC`/`HW_HDR` (own object, -Werror; the shipped link takes it via HW_OBJ) |
| `tests/mk-initramfs-acpi.sh` → `tests/images/guest-initramfs-acpi.img` | the G2 initramfs: the tree's own WORKING reboot image tree with `./init` replaced (the report of two from-scratch dead ends is in the script's header) |
| `TODO.md` §1.1 | the cross-lane note: the v86guest kernel's config delta (`CONFIG_ACPI=y`/`CONFIG_X86_MPPARSE=y` AND `CONFIG_X86_IO_APIC=y`), with its evidence |

### The layout, structure by structure (all inside the free window 0xE0000–0xEFFFF)

Every structure sits on a 16-byte step (the RSDP scan's stride; the tables need only
4/8), the whole set is 652 bytes ending at 0xE028B, and each address's collisions are
named: the firmware fills 0xF0000–0xFFFFF (`src/main.c` BOOT_FIRMWARE_ADDRESS), the
option ROM sits at 0xC0000, boot params at 0x80000, cmdline at 0x8E000, and
0xA0000–0xEFFFF is an E820 GAP in minibios's map — which the kernel treats as reserved,
so the guest cannot allocate over the tables.

| offset | structure | length | why here |
| --- | --- | --- | --- |
| 0xE0000 | RSDP | 36 | the scan's FIRST candidate (ACPICA walks 0xE0000 upward in 16-byte steps); measured found at 0xE0000 |
| 0xE0040 | RSDT | 44 | 4-aligned, behind the RSDP, in the same page-neighbourhood (one early_ioremap window) |
| 0xE0080 | XSDT | 52 | ditto; both root tables stay live because the RSDP is rev 2 |
| 0xE00C0 | FADT | 268 | rev 5.1, hardware-reduced (FLAGS bit 20), `smi_cmd=0` (no SMI, no ACPI-mode entry), `sci_int=9` (the PIIX4 device's line, unused under reduced hardware), PM blocks/GASes all zero |
| 0xE01D0 | FACS | 64 | the FADT names it (both widths agree); no checksum field exists |
| 0xE0210 | DSDT | 36 | empty namespace: nothing in this machine needs AML for boot, and an object we cannot back with device behaviour (`_PRT`, `_Sx`) would be a lie — see F2 for what that honesty COSTS |
| 0xE0240 | MADT | 76 | LAPIC (id 0 = apic.c's reset value, enabled), IOAPIC (id 0 = ioapic.c's `IOAPIC_ID`, address `IOAPIC_MEM_ADDRESS`, GSI base 0), INT_SRC_OVR bus 0/IRQ 0→GSI 2 edge (bus default flags 0) |

**Deliberately left out**, and what each omission breaks: every PM block and the SMI
port (sleep and the ACPI-mode transition — nothing else); an HPET/MCFG/SRAT (no such
device; a table describing absent hardware is a lie ACPICA would act on); a second LAPIC
or an NMI entry (one CPU); the SCI override (reduced hardware raises no SCI); the entire
MP-table route (Linux prefers ACPI where both exist — publishing both is two answers to
one question — and the MPS floating pointer's legal homes do not include our free
window, results/acpi-1.md §3a).

### Why ACPI and not MP tables

The MP route is one floating pointer + one table, but Linux's `acpi_boot_table_init`
runs before the MPS scan and disables it when ACPI wins; ACPI is also the only route
whose MADT can name the LAPIC *and* the IOAPIC in one format the port's own constants
already describe. ACPI it is — MEASURED working end-to-end below.

## 2. G1 — default-off byte-identity

By construction: nothing outside `if (acpi)` calls `publish`; the flag gates it in the
boot path exactly where the firmware/option-ROM memcpys live; with the flag off, the
builder's code compiles in but no byte of guest RAM is touched. MEASURED:

- `make transcript-diff` — the C transcript is **byte-identical to the pinned JS
  reference** (`results/` gate run, this unit).
- `make nasm` — **118/118**.
- `make selftest` — all suites green; hw_selftest **670 checks, 0 failures, 0 skipped**
  (was 631; the +39 are the tables group only).
- `sh tests/gate-reboot-1.sh` — **39 passed, 0 failed**.
- `sh tests/gate-m2-5-determinism.sh all` — rc 0 (serial AND virtio byte-identical, the
  SAME instruction count 4,000,006,484), raw: `results/acpi-2-det-gates.txt`. NOTE: the
  gate hardcodes `results/m2-5-det-run*-serial.txt` as its output paths; this unit's runs
  overwrote those TRACKED pins with fresh runs — they were restored (`git checkout`) and
  the `--acpi`-wrapper outputs archived separately as
  `results/acpi-2-det-acpi-run{1,2}-serial.txt` (byte-identical pair, verified).
- The OFF leg of the G2 pair boots the same initramfs and prints everything through
  serial (`results/acpi-2-g2-off.txt`) — the machine without the flag behaves exactly as
  before, including XT-PIC labels and the *"MADT or MP tables are not detected"* line.

## 3. G2 — THE EXTERNAL CONSUMER (kernel-level route: PROVEN; userspace label: blocked by F1)

The kernel, its own words, with `--acpi` (`results/acpi-2-g2-smoke.txt`, the no-initrd
dmesg leg; the ON pair with the initramfs agrees throughout):

```
[    7.362061] ACPI: Early table checksum verification disabled
[    7.365098] ACPI: RSDP 0x00000000000E0000 000024 (v02 V86CVM)        <- the scan's first candidate
[    7.375435] ACPI: XSDT 0x00000000000E0080 000034 (v01 V86CVM V86CFW   00000001 V86C 00000001)
[    7.390111] ACPI: FACP 0x00000000000E00C0 00010C (v05 V86CVM V86CFW   00000001 V86C 00000001)
[    7.406415] ACPI: DSDT 0x00000000000E0210 000024 (v02 V86CVM V86CFW   00000001 V86C 00000001)
[    7.420765] ACPI: APIC 0x00000000000E0240 00004C (v01 V86CVM V86CFW   00000001 V86C 00000001)
[    8.732133] IOAPIC[0]: apic_id 0, version 17, address 0xfec00000, GSI 0-23   <- version 17 = our 0x11 register
[    8.741035] ACPI: INT_SRC_OVR (bus 0 bus_irq 0 global_irq 2 dfl dfl)         <- the classic override, taken
[    8.748867] ACPI: Using ACPI (MADT) for SMP configuration information
[   57.223206] APIC: Switch to symmetric I/O mode setup                          <- was "Switch to virtual wire..."
[   63.404499] ACPI: Using IOAPIC for interrupt routing
```

The BEFORE (`results/apic-1-g2-on-serial.txt:71`) is
`APIC: ACPI MADT or MP tables are not detected` → `Switch to virtual wire mode` — that
machine state no longer exists under `--acpi`. The lengths the kernel prints (0x10C=268,
0x24=36, 0x4C=76, 0x34=52) are the builder's own; the IOAPIC's version 17 it prints is
read from OUR IOAPIC's VERSION register. This is a different machine, not a different
log line.

**What is missing for the letter of G2: `/proc/interrupts` from userspace.** The OFF arm
produces it (`results/acpi-2-g2-off.txt`: the init's `cat /proc/interrupts` shows the
XT-PIC labels); the ON arm cannot, because of F1. The kernel-side IO-APIC evidence above
(IOAPIC[0], INT_SRC_OVR, symmetric I/O mode, "Using IOAPIC for interrupt routing") is
the route being taken; the per-IRQ label lines are exactly what F1 hides.

## 4. G3 — the selftest, and its able-to-fail half

The group (hw_selftest, +39): placement (stride/16, ascending disjoint offsets, the
window inside 0xE0000–0xEFFFF), `verify()` over the built blob (signatures, lengths,
every checksum, both root pointers, the FADT's reduced-hardware shape and FACS/DSDT
pointers, the MADT's addresses against `APIC_MEM_ADDRESS`/`IOAPIC_MEM_ADDRESS`, the GSI
base, the override), the DEFAULT-OFF predicate (a zeroed window fails the kernel's own
scan AND verify), publish + the 16-step scan finding the RSDP at exactly 0xE0000, the
pointer walk out of guest RAM, the MADT fields vs the LIVE device registers
(`read32s(APIC_MEM_ADDRESS+0x20)>>24 == MADT apic id`; ditto IOAPIC ID), and the
delivery mirror (pin 2 delivers the timer vector through `device_raise_irq(0)`'s real
body; pin 0 with its entry unmasked stays silent; irq 4 lands on pin 4).

Able to fail — each corruption applied to a COPY, the check that must catch it run over
the copy:

1. a flipped FADT body byte → checksum → `verify` refuses;
2. GSI base 0→1 **with the checksum repaired afterwards** → the FIELD check refuses (the
   checksum is asserted GOOD first, so the refusal is the field's);
3. LAPIC address 0xFEE00000→0xFEE01000, checksum repaired → refuses;
4. a corrupted RSDP signature → the kernel's scan finds NOTHING;
5. the whole set shifted +8 → the scan misses (16-byte stride) AND the RSDT pointer walk
   lands on non-table bytes.

These five caught three REAL bugs during development (each fixed): the builder wrote the
RSDP's revision/pointers AFTER its checksums; `verify()`'s XSDT loop shifted a uint32_t
by 32 (UB — x86 masks the count to 0, so a correct blob "mismatched" its own high
bytes); and the selftest's IOREGSEL arithmetic was off by one pin (0x12 selects entry 1,
pin 2's config half is 0x14).

## 5. G4 — regression set, and THE FINDINGS

| gate | result |
| --- | --- |
| `make` | rc 0, -Werror clean throughout (new TU included) |
| `make selftest` | all suites green (670 hw checks; counts otherwise unchanged) |
| `make nasm` | 118/118 |
| `make transcript-diff` | byte-identical |
| `sh tests/gate-reboot-1.sh` | 39/39 |
| `sh tests/gate-m2-5-determinism.sh` | rc 0 plain AND under the `--acpi` wrapper (§2) |
| determinism with `--acpi`, the Alpine guest | the ON pair byte-identical: `cmp results/acpi-2-g2-on-1.txt results/acpi-2-g2-on-2.txt` — identical, and BOTH runs end at the SAME 808,195,989 instructions (no normalization; the interrupt COUNTERS the init prints are deterministic too, since the virtual clock is instruction-driven) |
| `make bench` | **NOT RUN yet** — the F1 investigation consumed the unit's budget; the binary's non-unity additions are boot-path-only (one flag-tested call per boot), so no shipped-loop impact is even possible (INTERPRETATION, stated as such) |
| aarch64 | **NOT RUN — the host toolchain is degraded (MEASURED):** `aarch64-linux-cosmo-cc` is a 23-byte text file containing only the string `aarch64-linux-cosmo-gcc` (no shebang, no `"$@"` — arguments are silently dropped: "fatal error: no input files"); `aarch64-linux-cosmo-gcc` directly compiles but the GCC-internal aarch64 include tree is absent from the install (`~/.local/cosmo/lib/gcc/` holds only `libgomp.spec`; cosmo's own `stdint.h` → `libc/inttypes.h` defines only the `_least`/`_fast` types, so exact-width types do not resolve); no alternative cross-compiler on the host (`which clang zig aarch64-linux-gnu-gcc` all miss). Even `cosmocc -v` currently fails (`mkdir: cannot create directory '/dev/.aarch64'`). The new files are plain C11 with fixed-width types and explicit byte serialization — but that is a claim, not this gate |

### F1 — under `--acpi`, userspace console writes never reach the UART; init blocks (NOT FIXED)

- **Signature.** The ON boot's dmesg streams to serial all the way to
  `Run /init as init process` (`results/acpi-2-f1-console-freeze-serial.txt`, last line
  [235.101272]) — and then goes permanently silent. The init reaches its `exec sleep`
  (the machine idles: the run ends early, `in_hlt=1`, ~820M instructions, NO panic); its
  echoes, `cat /proc/interrupts` and the dmesg grep produce ZERO bytes on the wire. The
  same initramfs in the OFF arm prints all of it (`results/acpi-2-g2-off.txt`).
- **Discriminated.** A write-free init (`rdinit=/bin/dd` that fails on a missing
  `/dev/zero`) exits cleanly under `--acpi` with the kill-init panic printing normally
  (`results/acpi-2-f1-nowrite-dd-*`) — userspace and syscalls are alive; the freeze
  correlates with the TTY write path specifically. The kernel's POLLED printk console
  (same port, same UART) works the whole boot; the IRQ-driven tty write does not.
- **ttyS0 registers identically in both arms** (`serial8250: ttyS0 at I/O 0x3f8
  (irq = 4, base_baud = 115200) is a 16550A` in both captures) — the probe succeeds
  under `--acpi`; the write path is where it dies.
- **Interpretation, for the next rung.** The serial IRQ is irq 4 → IOAPIC pin 4 under
  the MADT route. Prime suspects, in order: (a) our uart's raise/lower pattern
  (`src/hw/uart.c:388-408` — `device_raise_irq(4)` on the THRI/RDI arms,
  `device_lower_irq(4)` in the else arm, i.e. a PIC-shaped line) meeting the IOAPIC's
  edge/level entry semantics once a guest actually unmasks the entry (APIC-1's selftests
  covered single raise/lower cycles, not the uart's rapid interleaving with a live ISR);
  (b) the kernel blocking on the THRE wakeup that our delivery answers differently
  through the LAPIC. The init's block (not spin) is consistent with a THRE wakeup that
  never arrives.
- **Why I stopped.** Three distinct dead ends on the console plumbing itself (the
  initramfs layout ×2 and the console-device redirection) were MEASURED before the
  discriminator runs isolated the freeze to the machine, not the initramfs: at that
  point the remaining work is device-level (uart×IOAPIC delivery semantics), which is
  the correctness-critical class this unit's brief routes upward.

### F2 — an ACPI-aware kernel enumerates ZERO PCI devices against the empty DSDT (NOT FIXED)

With `--acpi`, the kernel prints `PCI: Using host bridge windows from ACPI` /
`PCI: Using ACPI for IRQ routing` and then finds NO PCI devices (no `pci 0000:00:XX.0`
enumeration lines at all — OFF enumerates the PIIX pair and the IDE). The kernel looks
for its PCI host bridge as a PNP0A03/PNP0A08 object in the DSDT; ours is
deliberately empty (§1), so the bus itself is never rooted. The virtio console attaches
to nothing (`virtio console: 0 bytes out`), the IDE never probes. The fix is DSDT
content — a PNP0A03 host bridge with `_CRS` (and eventually `_PRT` for PCI INT routing) —
which is exactly the AML this unit's honesty rule refused to write blind: it must be
backed by what `pci.c` actually implements (the config-space mechanism, the two fixed
devices' BDFs) and gated against the guest, i.e. a slice of its own.

### F3 — the reboot arm under `--acpi` soft-lockups at module load (same class as F1, NOT FIXED)

`results/acpi-2-reboot-acpi-serial.txt`: the reboot initramfs boots under `--acpi`, the
init reaches kmod's `finit_module`, and the watchdog fires —
`BUG: soft lockup - CPU#0 stuck for 22s! [busybox:386]` at [421.8] with the syscall
trace through `__ia32_sys_finit_module`. The run then sat at its instruction limit.
`REBOOT_KERNEL_ARM` (no flag) passes 39/39 + the kernel arm on the same tree; APIC-1 ran
this workload with `--acpi` and `lapic` BEFORE the tables existed and got
`DISK PERSIST OK` (results/apic-1.md §2). The delta is this unit's; the lockup is
consistent with F1's class (userspace-triggered kernel paths through the new delivery),
but it was not root-caused here.

## 6. Every divergence from upstream, with its reason

1. **The builder exists** (upstream has none — acpi.js is the PIIX4 PM device only, no
   mps.js; results/apic-1.md §6). Written from the ACPI 6.x spec, like minibios.asm was
   written from the BIOS conventions: it is firmware, not a device port.
2. **The pin map in `device_raise_irq`/`device_lower_irq`** (machine.c): upstream raises
   `ioapic::set_irq(i)` identity (cpu.rs:4599). The MADT publishes IRQ0→GSI2 (the
   classic southbridge steering); delivery must agree or a kernel that takes the
   override loses its timer. The PIC half is untouched. The selftest pins both arms.
3. **`--acpi` now publishes firmware tables** — the flag itself is APIC-1's
   (`settings.acpi`'s composition); this unit adds the firmware half the reference
   never had, under the same flag, default OFF.
4. The hardware-reduced FADT (vs QEMU's non-reduced shape): no SMI port and no fixed
   hardware exist in this machine; reduced mode is what lets ACPICA boot without
   inventing them. `sci_int=9` is kept because the PIIX4 device's line really is 9.

## 7. What still does not work (the successor's list)

1. **F1** — the userspace console write path under `--acpi` (uart × IOAPIC delivery).
   Until it lands, `/proc/interrupts` evidence must come from a non-serial console (the
   initramfs already prefers hvc0) or the labels read from the kernel's own routing
   lines.
2. **F2** — PCI enumeration under ACPI (DSDT host bridge + `_CRS`/`_PRT`). Until it
   lands, virtio/IDE are invisible to an ACPI-aware kernel; the 8259-era guests are
   unaffected (they do not read ACPI).
3. **F3** — the reboot-arm lockup (likely F1's sibling).
4. **`make bench`** — owed by this unit, owed by no design change (§5).
5. **aarch64** — blocked by the HOST toolchain's degradation, not by the tree; the
   evidence is in §5 and the orchestrator may want to reinstall `~/.local/cosmo`
   regardless of this unit.

## 8. Raw evidence

- `results/acpi-2-g2-smoke.txt` — the no-initrd `--acpi` dmesg leg (the §3 block).
- `results/acpi-2-g2-on-{1,2}.{txt,err}` — the ON pair (initramfs, 4e9 each), byte-identical.
- `results/acpi-2-g2-off.{txt,err}` — the OFF arm: XT-PIC labels, the init's full output,
  and the *"MADT or MP tables are not detected"* line for contrast.
- `results/acpi-2-f1-console-freeze-{serial.txt,err}` — F1's signature (freeze at init's
  first write; the run's early `in_hlt` end).
- `results/acpi-2-f1-nowrite-dd-{serial.txt,err}` — F1's discriminator (a write-free init
  exits cleanly; no freeze).
- `results/acpi-2-reboot-acpi-{serial.txt,err}` — F3 (the watchdog lockup at
  `finit_module`).
- `results/acpi-2-det-gates.txt` — the m2-5 determinism gate, plain and `--acpi`-wrapped.
- `results/acpi-2-det-acpi-run{1,2}-serial.txt` — the wrapper runs' serial pair
  (byte-identical; archived because the gate's own output paths are tracked pins).
- `results/acpi-2-g2-smoke.err` and every other `.err` — trimmed to their final 3 lines
  (the rest was the pre-existing `jit: bind: the block table is full (2048)` noise).

## 9. Reproduction commands

```sh
make && make minibios-mem
# the tables, kernel-side (§3):
./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0" \
  --acpi --serial /tmp/on.txt            # dmesg: RSDP/XSDT/FACP/DSDT/APIC, IOAPIC[0], INT_SRC_OVR
# F1 (the freeze):
sh tests/mk-initramfs-acpi.sh
./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0" \
  --hda tests/images/disk.img --initrd tests/images/guest-initramfs-acpi.img \
  --memory 80 --bios build/minibios-80m.bin --acpi --serial /tmp/on-init.txt
#   -> the capture stops at "Run /init"; drop --acpi and everything prints.
```
