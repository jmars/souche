# ACPI-7 — the PkgLength under-count fixed and the rule pinned

**Unit:** HANDOFF=v86c-acpi, ACPI-7 (coder). **Authority:** `results/acpi-6r-review.md`
(SF1 + N1 + N2), the reviewer's settled fix code on `handoff-v86c-acpi-fix-1`.
**State at finish:** all gates green; tree diff confined to `src/hw/{acpi_tables.c,hw_selftest.c}`
plus the new `results/acpi-7*` files (the reviewer's untracked `acpi-6r-review.md` untouched).

## 1. The fix (SF1) — every PkgLength now counts from its own first byte

The AML convention (iasl output and ACPICA `psargs.c`'s `AcpiPsGetNextPackageEnd`
agree): a `PkgLength` counts **from its first byte, its own encoding INCLUDED, the
opcode EXCLUDED** — `pkg_end = first_pkglen_byte + PkgLength`. The builder had encoded
the CONTENT length, so all five packages in `pci0_aml` declared extents one or two
bytes short. Corrected bytes in `src/hw/acpi_tables.c` (`pci0_aml`, the builder's own
comments now state the includes-encoding reading):

| package | before | after | decoded | true end |
| --- | --- | --- | --- | --- |
| ScopeOp `\_SB` | `10 4B 06` (107) | `10 4D 06` (**109**) | 109 = 107 content + 2 encoding | 110 |
| DeviceOp PCI0 | `5B 82 43 06` (99) | `5B 82 45 06` (**101**) | 101 = 99 content + 2 encoding | 110 |
| BufferOp `_CRS` | `11 34` (52) | `11 35` (**53**) | 53 = 52 content + 1 encoding | 90 |
| `_PRT` outer Package | `12 0D 01` (13) | `12 0E 01` (**14**) | 14 = 13 content + 1 encoding | 110 |
| `_PRT` inner Package | `12 0A 04` (10) | `12 0B 04` (**11**) | 11 = 10 content + 1 encoding | 110 |

Verified independently of the reviewer: a python decode of the array AS COMMITTED
re-derives all five values from the bytes and asserts `first_pkglen_byte + decoded ==
true end` for every package (all five OK; the buffer's end lands exactly on the `_PRT`
NameOp at 90, the rest run to `aml_len` = 110). One correction to the review's
reproduction snippet: its shift `(k<<3)-4` is right only for the first follow byte;
the correct AML shift is `4*k` (first follow byte <<4, second <<8, third <<12, per
ACPICA's decoder) — immaterial here, every length in this table is a 1- or 2-byte
form, which is why the review's ends came out right anyway.

## 2. The rule is pinned STRUCTURALLY, not just by value (G1's stronger instrument)

`acpi_tables_verify()` now opens its DSDT section with a walk that decodes each of the
five PkgLengths (the real AML decoder shape: bits 7:6 = follow-byte count, 6-bit small
form, else low nibble + follow bytes at bit 4k) and asserts
`first_pkglen_byte + decoded == true end` against `{110, 110, 90, 110, 110}`. It is
placed **before** the byte-value pins, so a length mutation is caught by the RULE
first. This catches the CLASS — the original defect (all five lengths under-counted by
their own encoding size) was invisible to every byte-value pin that existed.

**Able-to-fail proof (probe 16 + instrument):** probe 16 restores the scope's
content-length encoding (`0x4D -> 0x4B` = 109 -> 107, checksum repaired) and asserts
`!acpi_tables_verify(copy)`. An instrumented scratch build of `verify()` (print in the
seat, `/tmp/acpi7/ctx`, real source otherwise) attributes the rejection to the seat
itself, before any byte-value pin:

```
clean verify: 1
SEAT FIRED i=0 decoded=107 end=110
probe-16 blob verify: 0
```

The five hard-coded pins that carried the old bytes were updated to the corrected ones
(`scope_dev[]` 0x4D/0x45, `aml[37]` 0x35, `aml[96]` 0x0E, `aml[99]` 0x0B) — they now
agree with the walk.

## 3. N1 — probe 17: the `source` field is now covered

`aml[107]`, the static-vs-link discriminator (Zero = static GSI entry), had no
able-to-fail probe. Probe 17 flips `0x00 -> 0x01` (ZeroOp -> OneOp) with the checksum
repaired; caught by verify's `aml[107] != 0x00` pin (acpi_tables.c).

## 4. N2 — probe 13's comment reattributed

The comment claimed verify's computed `parent_pin` check catches the device-13
mutation; in fact the `prt_addr != 0x000CFFFF` equality fires first (the arithmetic is
a genuine second catcher). Comment corrected in place; the safety claim is unchanged.

## 5. Gate table (exit codes + raw evidence)

| gate | result |
| --- | --- |
| `make` | **rc 0** (-Werror clean) |
| `make selftest` | all suites green; hw **694 checks, 0 failures** (= 690 + exactly probes 16+17, 2 CHECKs each; nothing else moved) |
| `make nasm` | **118/118** passed, rc 0 |
| `make transcript-diff` | byte-identical, rc 0 (flag off) |
| `sh tests/gate-reboot-1.sh` | **39 passed 0 failed**, rc 0 |
| `sh tests/gate-m2-5-determinism.sh all` | **rc 0**, 4,000,024,062 == both runs; run1-serial vs HEAD differs only in `[timestamp]` prefixes, run1-virtio byte-identical to HEAD (both pin files restored to HEAD — content unchanged) |
| `build/v86c.aarch64.elf` | ELF64, **Machine AArch64** (readelf -h), rebuilt by this session's `make` link (mtime alongside `build/v86c`) |
| G2 guest, virtio ON arm | **rc 0**: discovery lines identical to before (section 6); `11: 26 IO-APIC 11-fasteoi virtio0`; hvc0 enabled, `VIRTIO-INPUT16` read back, 2460 hvc0 bytes; zero `Firmware Bug` / `can't derive` lines (`results/acpi-7-virtio-on-serial.txt`, `results/acpi-7-virtio-on-hvc0.txt`) |
| G2 guest, `--acpi` reboot arm | **rc 0**: `BOOT1: marker written` -> PS/2 reboot -> `BOOT2: marker read back from /dev/sda sector 8: DISK PERSIST OK` (`results/acpi-7-reboot-on-serial.txt:335,668`) |

**ON-arm serial vs acpi-6's, timestamps stripped:** 18 changed lines, ALL inter-run
jitter (hrtimer ns, quirk usecs, RTC seconds, sched_clock ns, LOC counts, input-device
numbering, one identical-line pair from CR handling) — no content difference from the
fix. **Reboot arm vs acpi-6's:** same events at the same line numbers (273/316/608/651/668);
the residue is the pre-existing soft-lockup's register dump (random registers/PID) —
`watchdog: BUG: soft lockup` and the IDE's `can't derive routing for PCI INT A` are
both PRE-EXISTING in `results/acpi-6-reboot-on-serial.txt` at the same lines, and the
latter is the orchestrator's own correction in acpi-6.md (the IDE's route is
deliberately undeclared).

## 6. The kernel's table-discovery lines, before/after

BEFORE (`results/acpi-6-virtio-on-serial.txt`):

```
[    7.369908] ACPI: RSDP 0x00000000000E0000 000024 (v02 V86CVM)
[    7.380245] ACPI: XSDT 0x00000000000E0080 000034 (v01 V86CVM V86CFW   00000001 V86C 00000001)
[    7.394921] ACPI: FACP 0x00000000000E00C0 00010C (v05 V86CVM V86CFW   00000001 V86C 00000001)
[    7.411582] ACPI: DSDT 0x00000000000E0210 000092 (v02 V86CVM V86CFW   00000001 V86C 00000001)
[    9.070485] IOAPIC[0]: apic_id 0, version 17, address 0xfec00000, GSI 0-23
[    9.093776] ACPI: Using ACPI (MADT) for SMP configuration information
[   64.690098] ACPI: Using IOAPIC for interrupt routing
[   64.760098] ACPI: PCI Root Bridge [PCI0] (domain 0000 [bus 00-ff])
```

AFTER (`results/acpi-7-virtio-on-serial.txt`) — **the same lines, same addresses and
lengths** (the DSDT's declared length was already correct at 0x92 = 146; the fix
changed 5 bytes INSIDE it, nothing a discovery line prints):

```
[    7.369908] ACPI: RSDP 0x00000000000E0000 000024 (v02 V86CVM)
[    7.380245] ACPI: XSDT 0x00000000000E0080 000034 (v01 V86CVM V86CFW   00000001 V86C 00000001)
[    7.394921] ACPI: FACP 0x00000000000E00C0 00010C (v05 V86CVM V86CFW   00000001 V86C 00000001)
[    7.411582] ACPI: DSDT 0x00000000000E0210 000092 (v02 V86CVM V86CFW   00000001 V86C 00000001)
[    9.070485] IOAPIC[0]: apic_id 0, version 17, address 0xfec00000, GSI 0-23
[    9.093776] ACPI: Using ACPI (MADT) for SMP configuration information
[   64.690098] ACPI: Using IOAPIC for interrupt routing
[   64.760098] ACPI: PCI Root Bridge [PCI0] (domain 0000 [bus 00-ff])
```

The guest still works under `--acpi`: the route is derived AND used (26 interrupts
through the IO-APIC), hvc0 is up end to end, the reboot arm persists the disk marker.

## 7. Divergences

- **Probe numbering:** the brief's N1 says "add probe 16" for the source flip; the
  reviewer's settled fix code (`handoff-v86c-acpi-fix-1`) numbers the PkgLength probe
  16 and the source probe 17. I follow the fix code's numbering — G1's SF1 probe
  (16 = PkgLength) and N1's probe (17 = source) both exist; only the labels differ
  from the brief's.
- **G2 external reading:** `iasl`/`acpiexec`/`acpidump` do NOT exist on this host
  (`command -v`, rc 1). The well-formedness claim rests on (a) the structural walk in
  verify(), (b) my own python decode of the committed bytes (section 1), and (c) the
  reviewer's seabios.bin reference walk (the convention's origin, unchanged).
- **Review-reproduction snippet corrected:** the review's python shift `(k<<3)-4` is
  wrong for the second/third follow bytes; the correct shift is `4*k` (ACPICA's
  decoder). No conclusion changes — all five lengths use the 1- or 2-byte form.
- **`make bench` NOT RUN:** the change is 5 AML bytes + verify/selftest code, no
  shipped-loop surface touched (the flag-off determinism gate pins the run path);
  consistent with acpi-2/4/5/6's precedent of stating this explicitly.

## 8. Reproduction

```sh
make && make selftest && make nasm && make transcript-diff
sh tests/gate-reboot-1.sh && sh tests/gate-m2-5-determinism.sh all
# G2 virtio ON arm (minibios-mem was up to date; run it before any guest gate):
make minibios-mem
./build/v86c --bzimage tests/images/vmlinuz-i386 3000000000 --cmdline "console=hvc0 console=ttyS0" \
  --initrd tests/images/guest-initramfs-virtio.img --bios build/minibios-80m.bin --memory 80 \
  --virtio-console /tmp/vio-on.txt --virtio-console-in results/m2-4-virtio-in.txt \
  --serial /tmp/von.txt --acpi
# G2 reboot arm:
./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0 lapic" \
  --hda tests/images/disk.img --initrd tests/images/guest-initramfs-reboot.img \
  --memory 80 --bios build/minibios-80m.bin --acpi --serial /tmp/reboot-on.txt
# the convention, against the committed array (decode + true-end assert):
python3 - <<'EOF'
import re
src = open("src/hw/acpi_tables.c").read()
body = re.search(r'pci0_aml\[\] = \{(.*?)\n\t\};', src, re.S).group(1)
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
body = re.sub(r"'(.)'", lambda m: str(ord(m.group(1))), body)
aml = bytes(int(t, 16) for t in body.split(',') if t.strip())
def pkglen(b, i):
    lead = b[i]; extra = lead >> 6
    v = lead & (0x0F if extra else 0x3F)
    for k in range(1, extra+1): v |= b[i+k] << (4*k)
    return v
for n, s, e in [("Scope",1,110),("Device",9,110),("Buffer",37,90),
                ("PRT-outer",96,110),("PRT-inner",99,110)]:
    assert s + pkglen(aml, s) == e, n
print("all five PkgLengths land exactly on their true ends")
EOF
```
