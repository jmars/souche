# ACPI-1R — independent review of the whole `--acpi` lane

**Unit:** `HANDOFF=v86c-acpi`, UNIT ACPI-R (reviewer). **Scope:** every table the lane
declares (RSDP/RSDT/XSDT/FADT/FACS/DSDT/MADT), the placement, the two fixes (F1 the
non-reduced FADT + PM blocks, F2 the DSDT root bridge), the pin wiring in machine.c, the
publish sites, and the lane's device-set shape. **Method:** a field-by-field spec read
against two independent authorities fetched for this review — ACPICA itself (tbfadt.c,
tbutils.c, rsaddr.c, actbl.h/actbl2.h) and QEMU's table builder (hw/acpi/aml-build.c,
include/hw/acpi/aml-build.h), plus Linux's flag definitions (include/acpi/actbl.h) — and
three guest boots on the CURRENT tree (ON-arm serial boot with the full init evidence;
ON+OFF virtio-boot pair; ON-arm reboot arm). The tree has no upstream differential, so the
spec-reading IS the instrument, per the brief.

**Verdict: NO BLOCKERS.** The load-bearing fabric is TRUE: every PM block the FADT names is
a port the device answers at (with ACPICA's own splitting semantics), the SCI, the MADT's
three entries, the hand-encoded DSDT AML (byte-verified against QEMU's encoder and
ACPICA's decoder), the placement, the checksums, and both fixes — F1's mechanism reconfirmed
independently, F2's windows live in the guest's own resource printout. **Three SHOULD-FIX
findings remain, all the exact F1 class — false declarations in fields the lane leaves
zero** — plus documentation drift and one honest coverage gap. Each fix is a one-liner plus
a verify() pin.

Files reviewed (sha256 at review time):

| file | sha256 |
| --- | --- |
| src/hw/acpi_tables.c | f5693db8c77b76349c5ab019a9deacf0303fe43c973e0aefaf0a18d79a9bdaf3 |
| src/hw/acpi_tables.h | 9ad4492128409261ffbdccd3622a2e48d95d9871cbda6bf9864b2c74e3118459 |
| src/hw/acpi.c | 8228b34e4d117e98844f08a15dadba7b0f812d2201ce2a6f894586376887e731 |
| src/hw/machine.c | 59dcbd40402a19869a5d83a014ede6dd7b058123c76fd6d02d79e3a85b003373 |
| src/main.c | 8ccd90e2ab07ccf83f5f8b40d84a580ea3bbfabe9aad67b390f35221c02f8d0 |
| src/hw/pci.c | 5ebc9808f21c1c179c664a19806c416a28f0a4f7c4ca3d9b94b6c5bf35cd025a |
| src/cpu/apic.c | 1adac1e6db427f595874800009babaca09ce5dd5fdffa5e237801496af503cc7 |
| src/cpu/ioapic.c | 99aa2c5bfa27839f284ff4f6135aeaefb2ecb320b5d6543e5982d3680052924e |
| src/hw/uart.c | 74b3c2eeccdaace41f641265c8be498dc09277c3058b07b871923ffd786cf7c1 |
| src/hw/hw_selftest.c | 0691e682067c567d6f83364b220562e26ee929635ae7b7a8de414ec7da69a8f0 |
| tests/minibios.asm | 579b806ea2edf959596949fc7710bec87332618bb60d613cf7bd727fb14fb482 |

---

## 1. Findings (severity-ranked)

### SHOULD-FIX — SF1: the FADT declares a VGA this machine does not have
`src/hw/acpi_tables.c:198-199` — `IAPC_BOOT_ARCH = 0x0003` (bit 0 legacy devices, bit 1
8042, both true). Bit 2 is `ACPI_FADT_NO_VGA` (Linux `include/acpi/actbl.h:262`: "It is not
safe to probe for VGA hardware"; set = no VGA). Leaving it clear declares VGA PRESENT —
false: `src/hw/machine.c:16-17`'s own header names the VGA absent, and TODO §1.2 prices it
as not ported. This is the F1 shape precisely (a false platform declaration a guest may act
on); it is benign only because today's guests are serial-only and a vgacon probe against
0xFF-reading ports fails safely. **Fix:** `wr16le(fadt + 109, 0x0007)` (add NO_VGA; bit 3
NO_MSI may be set with it — this machine has no MSI machinery at all), and pin the value in
`acpi_tables_verify()` (see §3).

### SHOULD-FIX — SF2: the FADT says minor version 5.1 but carries a 5.0-length table
`src/hw/acpi_tables.c:202` — `fadt[131] = 1` ("FADT minor version: 5.1") with
`put_sdt_header(fadt, "FACP", 268, 5)`. Per ACPICA's own size map
(`source/include/actbl.h:433-437`): `ACPI_FADT_V5_SIZE` = offset(HypervisorId) = **268
(revision 5.0)**; 5.1 requires **276**. A "5.1" FADT of 268 bytes is malformed by the spec's
revision table; ACPICA tolerates it (`acpi_tb_create_local_fadt` copies
`min(length, sizeof(ACPI_TABLE_FADT))` and zeroes the tail), so nothing in this guest
complains — but the table misstates its own format, and stricter parsers (iasl's table
validation, other OSPMs) do check. Nothing in the table needs 5.1 (HW_REDUCED, the X
fields, ARM_BOOT_ARCH-zero all predate it). **Fix:** `fadt[131] = 0` — declare ACPI 5.0,
which is exactly what 268 bytes is. (Alternative: extend to 276 with a zero HypervisorId;
minor=0 is the smaller diff.)

### SHOULD-FIX — SF3: `CENTURY = 0` is false — the RTC implements the century register
The FADT leaves offset 108 (`Century`) zero = "no century register in CMOS" (spec: "0 = not
supported"). The machine HAS one: `src/hw/rtc.h:68-71` defines `CMOS_CENTURY 0x32` /
`CMOS_CENTURY2 0x37`, and `src/hw/rtc.c:225-247` reads both from the fixed-epoch clock
(2024 → the register answers 20). Linux's `rtc-cmos` driver takes its century index FROM
`acpi_gbl_FADT.century`, so the guest never reads the register and window-guesses the year
instead — benign at the fixed epoch, false as a declaration. **Fix:** `fadt[108] = 0x32;`
and pin it in verify().

### NICE-TO-HAVE — N1: the flags could declare PROC_C1
`acpi_tables.c:200` — flags = WBINVD only. Bit 2 `PROC_C1` ("C1 supported") is clear, but
this CPU executes HLT (the guest parks in `in_hlt=1`; both arms of every run). Kernel
behaviour is identical either way (its default idle is also HLT), so this is honesty, not
function: `wr32le(fadt + 112, (1u << 0) | (1u << 2))` if maximal truth is wanted. Note the
F1 mutation probe writes `(1<<0)|(1<<20)` and is unaffected. PWR_BUTTON/SLP_BUTTON stay
clear (no buttons exist — true).

### NICE-TO-HAVE — N2: two comments have been falsified by the lane's own later work
- `src/hw/acpi.c:177-194` — the `acpi_get_timer` comment's premise is obsolete: it says the
  device "is constructed ONLY when cpu->acpi_enabled (machine.c:82), a flag with NO writer
  in this port … there is no CLI flag" and "no boot today ever registers 0xB008". `--acpi`
  exists (`src/main.c:961,1028-1030`), the construction site is now `machine.c:94-96`, and
  every `--acpi` boot registers 0xB008. The assert-drop REASONING (the clock rewind across
  a guest reboot) stands; the "latent because unreachable" framing does not.
- `src/hw/acpi_tables.c:126-128` — "no device here uses a PCI INTx" is false:
  virtio-console raises INTA (`src/hw/virtio.c:1028-1039` → `pci.c:498-514`'s PIIX3 PIRQ
  routing; pin=INTA at `virtio.c:874-875`). The CONCLUSION (no `_PRT` needed) survives and
  is even measured: `/proc/bus/pci/devices` in both of my arms shows `00:0c.0 … 0xb` — the
  kernel routes virtio INTA to IRQ 11 identically with and without the tables (its
  ACPI-with-no-`_PRT` path falls back to the PIRQ-programmed line). The comment should say
  that, not the false reason.

### NICE-TO-HAVE — N3 (coverage, not a defect): virtio-console FUNCTION under `--acpi` is unproven, and unprovable with the committed images
`guest-initramfs-acpi.img`/`guest-initramfs-reboot.img` contain no virtio modules (51-file
cpio: ata/scsi only) and `virtio_console` is a module in 6.6.134-0-lts — so every ON-arm
virtio capture to date, including mine (0 bytes out), is image-limited: the OFF control
gives 0 bytes too. The tables-side facts ARE proven (device enumerates as
`pci 0000:00:0c.0 [1af4:1043]`; BARs assigned at 0xb801/0xb901/0xb701/0xb601 — inside the
DSDT's window 2, the SAME addresses in both arms; IRQ 11 routed in both arms). But
"`--acpi` is end to end" (TODO @ 576170f) should not be read as covering virtio until a
guest with the driver actually pushes bytes through it — one boot with a virtio-capable
initramfs (the m2-4 evidence image no longer exists in the tree) or the peer lane's
`fx-demo` kernel config delta would close it.

---

## 2. What was checked and found TRUE (the negative results, with instruments)

**The FADT's fixed-hardware claims — every one pinned against ACPICA's own conversion
(`tbfadt.c`, fetched).** `PM1_EVT_LEN=4` is exactly right for this device: ACPICA splits
the block in halves ("Each is of length (Pm1EventLength/2)" — status 2 bytes at 0xB000,
enable 2 bytes at 0xB002, which is where `src/hw/acpi.c:133-134` registered them), and the
width-validation table's defaults match ours byte-for-byte (PM1a_EVT 32 bits, PM1a_CNT 16,
PM_TMR 32; GPE0 has no default) so not even a BIOS warning fires. `PM_TMR` 0xB008/len 4
with `TMR_VAL_EXT` clear matches `portB008_read32`'s `& 0xFFFFFF` (24-bit counter).
`SCI_INT=9` is the literal line `acpi_timer` raises (`acpi.c:155`), and the guest routed it
`9: 0 IO-APIC 9-fasteoi acpi` on pin 9 identity. `SMI_CMD=0` with zero ACPI_ENABLE/DISABLE
is the documented "already in ACPI mode" and is true (the PM registers are always live).
The X_ GASes left zero with V1 fields live is ACPICA's documented preferred path. Every
offset written (36/40/45/46/48/56..95/96-99/109/112/116/131/132/140) matches the
`ACPI_TABLE_FADT` struct order; every other byte stays zero and those zeros are true for
this machine (no PM1b/PM2/GPE1, no C-states — CST_CNT 0 and latencies 0xFFFF, no ACPI reset
— the machine's reboot really is 0xCF9/0xFE).

**The DSDT's hand-encoded AML — verified at the byte level against two authorities.** The
WordIO flag bytes `01 0C 03` are exactly QEMU's encoding of `WordIO (ResourceProducer,
MinFixed, MaxFixed, PosDecode, 16, …, TypeStatic)` (`aml-build.h`: `AML_MIN_FIXED=1<<2`,
`AML_MAX_FIXED=1<<3`, `AML_POS_DECODE=0`, `AML_ENTIRE_RANGE=3`) and ACPICA's decode agrees
(`rsaddr.c`: general-flags bit0 producer, bit1 decode, bits 2/3 min/max fixed; type-specific
bits 1:0 = ISA range). The PkgLength chain sums exactly (Scope 70, Device 63, Buffer 36
with the prefixed size operand 34 — the two traps the builder's comments document, both
real). The EISAID 0x030AD041 is proven by the guest's own namespace: it rendered
`PNP0A03:00` in `/sys/bus/acpi/devices`. The two windows are TRUE and LIVE: the kernel
printed `root bus resource [io 0x0000-0x0cf7 window]` + `[io 0x0d00-0xffff window]` (the
CF8/CFF config-mechanism hole is correct), and assigned the virtio/IDE IO BARs inside
window 2. The absent memory window is true — virtio's capabilities are all IO-BAR
(`use_mmio=false` at every site; the mmio capability asserts "not implemented" upstream
too), and no other device has an MMIO BAR.

**The MADT.** LAPIC id 0 = `apic_init`'s reset value (the selftest checks the LIVE
register), address 0xFEE00000; IOAPIC at `IOAPIC_MEM_ADDRESS` GSI base 0 — the device has
24 pins (`IOAPIC_IRQ_COUNT`) and the kernel printed `GSI 0-23`, `version 17` read from OUR
version register (0x11 | 23<<16). The one override (IRQ0→GSI2, flags 0 = ISA-default
edge/active-high) is the wiring `machine.c:264-269` actually performs, and both halves were
proven again in this review's run: `0: 98 IO-APIC 2-edge timer` in `/proc/interrupts` plus
the selftest's pin-2-delivers/pin-0-silent mirror. PCAT_COMPAT is true. The other ISA IRQs
identity-edge (guest labels: `1 i8042`, `4 ttyS0`, `8 rtc0`, `12 i8042`) — true.

**RSDP/FACS/checksums/placement.** Revision 2 with both checksums computed LAST (the
fields-after-checksum bug is gone); the selftest's `acpi_scan_rsdp` mirrors ACPICA's
predicate (signature, v1 sum over 20, rev 2, length 36, extended sum) and finds ours at the
first 16-byte step. The FACS is a consistent ACPI-2.0 shape (64 bytes, version 1, no
S4BIOS/global-lock claims — all true). 0xE0000-0xE02CB sits in minibios's E820 GAP
(`e820_table`: usable 0-0x9FC00, reserved EBDA, reserved 0xF0000+, usable 0x100000+) which
the kernel treats as reserved — and my run shows it going further:
`ACPI: Reserving FACP/DSDT/FACS/APIC table memory` — so the guest genuinely cannot
allocate over the named tables. The minibios option-ROM scan cannot false-positive on the
blob (0xE0000 starts "RS", not 0x55AA; the blob ends at 0xE02CB and the later 2 KiB steps
are zero). Publication is a pure function of constants, at both sites (initial boot +
reboot re-entry), idempotent.

**The two fixes, re-verified independently on the current tree (my runs).**
- F1: the full init output prints through serial, `preallocated irqs: 16`,
  `4: 10 IO-APIC 4-edge ttyS0` — the mutation probe (SF6-style: flag alone restored) is in
  the selftest as corruption #6 with the checksum repaired, so the regression seat exists.
- F2: root bridge found, windows decoded, `pci 0000:00:07.0 [8086:7113]` +
  the OFF arm's device set, IDE BARs assigned — and the REBOOT arm now completes
  `BOOT2: … DISK PERSIST OK` (2m21s run, scratch disk under /tmp; boot 2 re-found the RSDP
  at the same address after re-publication). F3's verdict (benign sha256 slowness,
  identical EIP both arms) matches: the lockups appear and are outlived.

**Lane shape / diagnostics.** Default-off is by construction (publish only under the flag;
the flag writes `cpu->acpi_enabled` once) and the orchestrator's flag-off gates are green;
my OFF control differs from the ON arm only in the intended machine lines. The PIIX4 ACPI
function appearing only under `--acpi` mirrors upstream's `cpu.js:1181-1185` construction
and is what makes the flag a coherent "machine with ACPI" rather than tables stapled on.
`V86C_TRACE_ACPI` caches its getenv in a function-local static (one call per process), is
decider-free, and matches the established `V86C_LOG_PORTS` style — free when off, keep.

---

## 3. A fix note that applies to all three SHOULD-FIXes

`acpi_tables_verify()` currently pins flags (WBINVD, NOT-reduced), SMI_CMD, the PM blocks,
pointers, MADT fields, and the DSDT bytes — but NOT `IAPC_BOOT_ARCH`, `CENTURY`, or the
minor version. Fixing SF1-SF3 without adding pins for them leaves the fields unpinned
(the selftest's corruption battery would not catch a regression), so each fix should land
with its verify() check: `fadt[109..110] == 0x0007` (or chosen value), `fadt[108] == 0x32`,
`fadt[131] == 0`.

## 4. Coverage

- **Read in full:** `src/hw/acpi_tables.{c,h}`, `src/hw/acpi.c`, `src/hw/machine.c`,
  `src/main.c` (flag parsing, both publish sites, the reboot loop), `src/cpu/ioapic.c`
  (set/clear/version/bounds), `hw_selftest.c`'s tables group (1330-1560),
  `src/hw/acpi.h`, `tests/minibios.asm`'s E820/int-15/ROM-scan legs, the relevant
  `pci.c` (BAR machinery, both PIRQ paths) and `virtio.c` (capabilities/BAR/IRQ facts),
  `rtc.c`/`rtc.h` (century), plus the four unit reports.
- **Sampled:** `uart.c` (trace guard + raise sites — the exonerated F1 suspect, unchanged
  in shape), `apic.c` (init/guards), `results/apic-1r-review.md`.
- **Fetched authorities (this review's instruments):** ACPICA `tbfadt.c`, `tbutils.c`,
  `rsaddr.c`, `actbl.h`, `actbl2.h`; QEMU `hw/acpi/aml-build.c` +
  `include/hw/acpi/aml-build.h`; Linux v6.6 `include/acpi/actbl.h`.
- **Guest instruments (current tree, binary sha-verified against sources, scratch disks
  under /tmp):** ON-arm serial boot (full init, /proc/interrupts, /proc/ioports,
  /proc/bus/pci/devices, the ACPI dmesg block); ON+OFF virtio-boot pair (both 0 virtio
  bytes — image-limited, see N3); ON-arm reboot arm (BOOT1 write → PS/2 reboot → BOOT2
  `DISK PERSIST OK`).
- **Not checked:** virtio-console function under `--acpi` (N3 — no capable image exists in
  the tree); `make bench` and the aarch64 build (not run; the reports state why — the
  aarch64 host toolchain is degraded).

## 5. Reproduction (my instruments)

```sh
make && make minibios-mem   # tree already current at review time
# F1/F2 evidence + /proc/interrupts + resource windows (this review's run):
./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0" \
  --hda tests/images/disk.img --initrd tests/images/guest-initramfs-acpi.img \
  --memory 80 --bios build/minibios-80m.bin --acpi --serial /tmp/on.txt
# virtio pair (image-limited, see N3): same + "console=hvc0 console=ttyS0" \
#   --virtio-console /tmp/vio.txt  — 0 bytes in BOTH arms
# reboot arm under --acpi (BOOT2 DISK PERSIST OK, 2m21s):
cp tests/images/disk.img /tmp/rb-disk.img && ./build/v86c --bzimage \
  tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0" --hda /tmp/rb-disk.img \
  --initrd tests/images/guest-initramfs-reboot.img --memory 80 \
  --bios build/minibios-80m.bin --acpi --serial /tmp/rb.txt
```
