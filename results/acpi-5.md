# ACPI-5 — the review's three false declarations, each with a verify() pin

**Unit:** `HANDOFF=v86c-acpi`, UNIT ACPI-5 (coder). **Input:** results/acpi-1r-review.md
(SF1-SF3, N1-N3). **Method:** fix each false field in `src/hw/acpi_tables.c`, pin each in
`acpi_tables_verify()`, prove each pin able to fail with a checksum-repaired mutation probe
in `hw_selftest.c` (the F1 lesson: a field declared and unverified is how the reduced flag
shipped), then the full gate battery. THIS FILE IS APPENDED AS THE WORK PROGRESSES.

## Status

- [x] SF1 IAPC_BOOT_ARCH 0x0003 -> 0x0007 (NO_VGA set)
- [x] SF2 FADT minor 1 -> 0 (268 bytes is the 5.0 length)
- [x] SF3 CENTURY 0 -> 0x32 (rtc.c's CMOS_CENTURY)
- [x] verify() pins + able-to-fail probes
- [x] N1 PROC_C1, N2 the two falsified comments
- [x] N3 virtio-console under --acpi: PROVEN with the tree's OWN capable image
      (tests/images/guest-initramfs-virtio.img — the review's "no capable image" is
      outdated): ON 2595 hvc0 bytes + `11: 26 IO-APIC 11-edge virtio0`, OFF 2391
- [x] Gates

## The fixes (all in src/hw/acpi_tables.c, builder + verifier)

| # | field | was | now | why the old value was a false declaration |
| --- | --- | --- | --- | --- |
| SF1 | IAPC_BOOT_ARCH (fadt+109, 16-bit) | 0x0003 | **0x0007** | bit 2 NO_VGA clear declared a VGA present; machine.c's own header names the VGA absent |
| SF2 | FADT minor version (fadt+131) | 1 | **0** | 268 bytes IS ACPICA's ACPI_FADT_V5_SIZE = revision 5.0; "5.1" on a 5.0 length misstates the table's own format |
| SF3 | CENTURY (fadt+108) | 0 | **0x32** | rtc.c implements the century register at CMOS 0x32 (CMOS_CENTURY, answered from the fixed-epoch clock); 0 declares it absent, and Linux's rtc-cmos takes its century index FROM this field |
| N1 | FADT flags (fadt+112) | WBINVD | **WBINVD \| PROC_C1** | this CPU halts (every idle guest parks in in_hlt=1); honesty, not function |

Checksums: all writes precede put_checksum(fadt, 268) — recomputed, not patched.

## The verify() pins (src/hw/acpi_tables.c, acpi_tables_verify)

- `fadt[108] != 0x32` → refuse (CENTURY)
- `fadt[109..110] != 0x0007` → refuse (IAPC_BOOT_ARCH, exact value)
- `fadt[131] != 0` → refuse (minor version 5.0)
- PROC_C1 added beside the WBINVD flag pin: `(flags & (1u<<2)) == 0` → refuse

## The able-to-fail half (src/hw/hw_selftest.c, test_acpi_tables)

Probes 9/10/11, each: mutate the field to the OLD false value (0x0003 / 0 / 1), REPAIR the
FADT checksum (assert the repair made the checksum GOOD), then CHECK(!acpi_tables_verify) —
so a refusal below is the FIELD check working, not a broken checksum. Same shape as the
existing probes 2/3/6/7/8.

**Measured:** `make selftest` → **hw_selftest: 682 checks, 0 failures** (all suites green).
The acpi-tables group's CHECK count went 45 → 51 (git: exactly my +6, in exactly my three
files; the group grew by the three probes' two CHECKs each and nothing else moved).

## G2 — the three fields are correct in a real boot (results/acpi-5-g2-on-serial.txt)

`make && make minibios-mem`, then the review's §5 ON-arm command (same kernel, same
initramfs, same 4e9 limit). What the guest itself said about the changed fields:

- `ACPI: FACP 0x00000000000E00C0 00010C (v05 ...)` — loads whole, **00010C = 268 bytes**,
  no complaint about the revision pair (declared 5.0, length 268: consistent).
- **No new warning.** The ONE `[Firmware Bug]`-class line in the whole boot is
  `acpi PNP0A03:00: [Firmware Bug]: no secondary bus range in _CRS` — it is a function of
  the **DSDT's bridge object** (git: `pci0_aml` is byte-identical to HEAD's), i.e. it was
  already fired on the reviewed tree; it is the F2 bridge declining to name a
  SecondBusNr it does not have, and the kernel proceeds with bus 0, which is the truth.
  Reporters note: the acpi-1r review did not mention it; it is PRE-EXISTING, not this
  unit's regression, and it is honest (there are no PCI-PCI bridges).
- SF3's observable: `rtc_cmos rtc_cmos: registered as rtc0` → `setting system clock to
  2024-01-01T00:11:19 UTC` — rtc-cmos now takes the century from CMOS 0x32 (the register
  rtc.c answers) and lands on the fixed epoch, no warning, no date change.
- The F1 guard still holds: `preallocated irqs: 16`, the full init output prints through
  ttyS0 (`ACPI-INIT: ttyS0 probe line` is the last line — no freeze).

## N3 — virtio-console under `--acpi`: PROVEN, not declared (results/acpi-5-virtio-*)

The review's N3 said "no capable image exists in the tree" — **outdated**:
`tests/images/guest-initramfs-virtio.img` (the M2-4 gate's image, committed) carries
virtio.ko / virtio_ring.ko / virtio_pci*.ko / **virtio_console.ko for 6.6.134-0-lts** (the
kernel `tests/images/vmlinuz-i386` runs) — verified by extracting the cpio. So option (a):
the ON/OFF pair on the CURRENT tree, no new image needed, fully reproducible from the
command in §Reproduction (the image is the committed M2-4 artifact, built by
tests/mkinitramfs.py, sha-pinned by results/m2-4-virtio.md).

| arm | hvc0 bytes | driver | delivery |
| --- | --- | --- | --- |
| ON `--acpi` | **2595** | `virtio0 1af4:0003`, driver `virtio_console`, `console [hvc0] enabled` | `11: 26 IO-APIC 11-edge virtio0` |
| OFF control | **2391** | same bind | IRQ 11 (no IO-APIC labels — XT-PIC path) |

The ON arm's kernel log flowed THROUGH the virtio console (the hvc0 stream carries the
kernel's own dmesg, not a userspace echo). The byte-count difference (2595 vs 2391) is
honest: the ON arm's log contains the extra ACPI lines.

**The one new guest line under `--acpi` + a virtio driver** — the two lines
`virtio-pci 0000:00:0c.0: can't derive routing for PCI INT A` / `PCI INT A: no GSI`:
this is the no-_PRT omission becoming visible to a driver that ASKS (the review predicted
exactly this class for "a future INTx device"), and delivery works anyway — 26 interrupts
on IRQ 11 — via the PIIX PIRQ-programmed line, the same fallback the review measured. This
is the corrected `acpi_tables.c` _PRT comment's claim, now MEASURED end to end. A _PRT
would silence it; the lane still does not need one (the review's conclusion stands).

## Gates

| gate | command | result |
| --- | --- | --- |
| build | `make` | rc 0, -Werror clean |
| selftest | `make selftest` | all suites green; **hw 682 checks, 0 failures** (group CHECKs 45→51 = exactly the +6 probes; git diff confined to this unit's 3 files) |
| G1 able-to-fail | probes 9/10/11 in hw_selftest.c | each mutates its field to the OLD false value with the checksum REPAIRED (asserted good) and `CHECK(!acpi_tables_verify)` passes — the field check catches it, not the checksum |
| nasm | `make nasm` | **118/118**, rc 0 |
| transcript-diff | `make transcript-diff` | byte-identical (flag off), rc 0 |
| reboot gate | `sh tests/gate-reboot-1.sh` | **39 passed, 0 failed**, rc 0 |
| determinism | `sh tests/gate-m2-5-determinism.sh all` | **rc 0**; run1/run2 serials byte-identical, virtio byte-identical, counts equal (4,000,024,062 = 4,000,024,062); the tracked `results/m2-5-det-run*` pins were overwritten by the gate's own paths and came back **byte-identical to HEAD's** — the flag-off boot reproduces the committed evidence exactly (this also settles the count: acpi-2's recorded 4,000,006,484 predates acpi-4's fix landing; the HEAD-era pin is 4,000,024,062) |
| aarch64 (G4) | make's own cosmocc link (see below) | `build/v86c.aarch64.elf`: ELF64, **Machine AArch64** (readelf -h), statically linked; `build/hw_selftest.aarch64.elf` ditto (same 10:57 build that printed 682 on the x86 leg) |
| G2 guest | ON-arm boot, acpi-image | no new warning; FACP 268 B v05 loads; rtc_cmos century live; init completes (see above) |
| N3 pair | virtio-image ON/OFF | 2595/2391 hvc0 bytes, driver binds both arms, IRQ 11 delivers under `--acpi` |

## Divergences, incidents, honest notes

1. **`build/v86c.aarch64.elf` was destroyed and rebuilt.** The pre-existing artifact (the
   engineer lane's 10:57 side-product) was consumed by my first two aarch64 attempts: I
   assumed a hand-rolled cross build was needed, and the failed `ld` of attempt #2
   truncated the output file. Root cause of the confusion: **`cosmocc` builds BOTH arches
   on every executable link and leaves `${OUTPUT%.com}.aarch64.elf` beside the output**
   (bin/cosmocc:693) — there is no separate aarch64 rule because `make` already makes it.
   Repair: re-ran make's OWN link command (same argv as the Makefile's `$(BUILD)/v86c`
   rule) — rc 0 — which regenerated `build/v86c` and the `.aarch64.elf` side-product from
   the current objects. The artifact is now current-source, real AArch64. (For the record,
   the raw `aarch64-linux-cosmo-cc` remains broken for a from-scratch build — no internal
   include tree, no -lgcc/-lc — the acpi-2 measurement stands; the wrapper path is the
   working one.)
2. **`[Firmware Bug]: no secondary bus range in _CRS`** — pre-existing (DSDT-borne, see
   G2), not new, honest; flagged here because the review's "no warning fires" sentence was
   about the FADT widths and this line sits in the same boot's log.
3. `make bench` **NOT RUN** — same reason as acpi-2/acpi-4: the boot-path-only delta has
   no shipped-loop surface, and the brief's gate list does not include it.
4. The determinism gate writes its own named outputs (`results/m2-5-det-run*`); they
   reproduced HEAD's tracked bytes exactly, so nothing needed restoring.

## Reproduction

```sh
make && make selftest && make nasm && make transcript-diff
sh tests/gate-reboot-1.sh; sh tests/gate-m2-5-determinism.sh all
# G2 (ON arm, the changed fields' boot):
./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0" \
  --hda tests/images/disk.img --initrd tests/images/guest-initramfs-acpi.img \
  --memory 80 --bios build/minibios-80m.bin --acpi --serial /tmp/g2-on.txt
# N3 pair (the virtio-image boot, ON then OFF):
./build/v86c --bzimage tests/images/vmlinuz-i386 3000000000 --cmdline "console=hvc0 console=ttyS0" \
  --initrd tests/images/guest-initramfs-virtio.img --bios build/minibios-80m.bin --memory 80 \
  --virtio-console /tmp/vio-on.txt --virtio-console-in results/m2-4-virtio-in.txt \
  --serial /tmp/von.txt --acpi     # OFF: drop --acpi, change the outputs
# aarch64 (what make itself does):
cosmocc -std=gnu11 -O2 -Wall -Wextra -foptimize-sibling-calls \
  -finline-functions-called-once -fpatchable-function-entry=0 -Werror -I src -Wl,--gc-sections \
  -o build/v86c src/main.c build/minibios_embed.c build/unity.o build/hw/*.o build/sf/softfloat.o
file build/v86c.aarch64.elf   # ELF64, ARM aarch64
```
