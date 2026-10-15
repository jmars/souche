# ACPI-6R — focused review of the `_PRT` (commit f426bc5)

**Reviewer:** UNIT ACPI-6R, `HANDOFF=v86c-acpi`. **Under review:** the `_PRT` commit
f426bc5 (`src/hw/acpi_tables.{c,h}`, `src/hw/hw_selftest.c` probes 12-15, the report
`results/acpi-6.md`) against the two port-side halves it must agree with
(`src/hw/pci.c`'s raise/lower pair, `tests/minibios.asm`'s `pci_irq_route`). The unit
asked for this review on the argument that its entry is exactly this lane's
false-declaration class — **the argument was right: the re-derivation found a false
declaration, but in the AML's length encodings, not the fields the unit pinned.**

**Files reviewed (sha256 at review time):**

```
70fb042be9acf54130873fe2dc96bb3926b8e045217224201e79174888338513  src/hw/acpi_tables.c
12f59cff45d5762e08e9a761d6e1588a2f763e45be907b03c2e30c38dce7bf4d  src/hw/acpi_tables.h
5ebc9808f21c1c179c664a19806c416a28f0a4f7c4ca3d9b94b6c5bf35cd025a  src/hw/pci.c
579b806ea2edf959596949fc7710bec87332618bb60d613cf7bd727fb14fb482  tests/minibios.asm
31926240b841fe8c06d2e98d483b5c67273ec3ede3f7a7fd9910628ed0bcf134  src/hw/virtio.c
1adac1e6db427f595874800009babaca09ce5dd5fdffa5e237801496af503cc7  src/cpu/apic.c
3dcd9ce7755d94afa050effc85b4285eeaa6d667bf2616ff4e50742e85ffa1ae  src/hw/hw_selftest.c
59dcbd40402a19869a5d83a014ede6dd7b058123c76fd6d02d79e3a85b003373  src/hw/machine.c
(ioapic lives at src/cpu/ioapic.c, reviewed; tree byte-unchanged: git status clean at f426bc5)
```

## Findings

### SF1 (SHOULD-FIX, top) — every `PkgLength` in `pci0_aml` is under-counted by its own encoding size

`src/hw/acpi_tables.c:179-221`. The builder encoded the packages' **content** length;
the AML convention — settled below from real iasl output and ACPICA's own source —
counts **from the first PkgLength byte, encoding INCLUDED, opcode excluded**.

| package | builder bytes | value | correct | decoded includes-end | true end |
| --- | --- | --- | --- | --- | --- |
| ScopeOp | `10 4B 06` | 107 | **109** (`10 4D 06`) | 108 | 110 |
| DeviceOp | `5B 82 43 06` | 99 | **101** (`5B 82 45 06`) | 108 | 110 |
| BufferOp (_CRS) | `11 34` | 52 | **53** (`11 35`) | 89 | 90 |
| _PRT outer PackageOp | `12 0D 01` | 13 | **14** (`12 0E 01`) | 109 | 110 |
| _PRT inner PackageOp | `12 0A 04` | 10 | **11** (`12 0B 04`) | 109 | 110 |

(Array length 110 = DSDT 146 − 36, mechanically re-derived from the source; every
verify() offset `aml[36..109]` confirmed against the bytes.)

**The convention, two independent authorities:**

1. **Real iasl output** — `v86/bios/seabios.bin` carries a compiled DSDT at 0x167DF
   (4405 bytes). Its top level is a chain of ScopeOps: `10 49 04` (PkgLength 73, 2-byte
   form) ends at 74 where `10 22 5F 53 42 5F` (Scope `\_SB`) begins; that (34) ends at
   109 where `10 4E 15 …` begins — **eight consecutive chained ScopeOps, every boundary
   a clean opcode start under includes-encoding**. Under the builder's convention the
   first "next op" would start at 76 = mid-namestring (0x5F), nonsense. A FieldOp
   `5B 81 0B …` in the same table decodes 1 byte + 10 content = 11 ✓ the same rule.
2. **ACPICA's decoder** — `psargs.c`'s `AcpiPsGetNextPackageEnd` returns
   `Start + PackageLength` where `Start = ParserState->Aml` positioned **after the
   opcode** (`psloop.c:373-376` advances by `OpcodeLength` before the call). Package
   length therefore includes its own encoding bytes.

**Why every gate is green anyway (the pinned kernel tolerates it):** the truncated
tails are structurally benign here — the scope/device tails (108 vs 110) are interior
to the inner package's last element; the buffer loses the EndTag's checksum byte
(the spec says the OS ignores the `_CRS` EndTag checksum, and ACPICA zero-pads the
declared-but-missing byte); the `_PRT` packages' decoded end (109) cuts the LAST byte
(the GSI source_index's operand position) yet the element grammar still consumes it —
MEASURED: the kernel used GSI 11 (`11: 26 IO-APIC 11-fasteoi virtio0`). Nothing
observable breaks on this consumer.

**Why it is still a finding:** the packages DECLARE extents smaller than their actual
content — a false declaration of exactly the class this lane has shipped four of, and
a consumer that honours `pkg_end` strictly (a newer ACPICA, `iasl -dc`, another OS)
may mis-parse or reject the DSDT outright. **Lineage, honestly:** introduced by
ACPI-2 (the old 70/63/36 values obey the same wrong rule), missed by the acpi-1r
review (it checked the chain "sums exactly" — self-consistency, not the convention),
grown by ACPI-6, and extended with two new instances in the `_PRT` itself. verify()
has **no PkgLength seat at all**: `scope_dev[]` (acpi_tables.c:443-450) hard-codes
the wrong bytes `4B 06` / `43 06`, and the `aml[37]`/`aml[96]`/`aml[99]` pins do the
same.

**Fix — confirmed and boot-proven, not just proposed** (full code on
`handoff-v86c-acpi-fix-1`): the five byte values above, the five verify() pins that
hard-code them, the comments that state the wrong reading ("52 = the size operand +
50 data" describes the content, not the PkgLength), **plus a new verify check that
RECOMPUTES each PkgLength from the structure end** (scope from aml[1] must decode to
110−1, device from aml[9] to 110−9, buffer from aml[37] to 90−37, outer from aml[96]
to 110−96, inner from aml[99] to 110−99) and an able-to-fail probe (scope 109→107,
checksum repaired). **Proof of safety (reviewer-run, scratch tree `/tmp/acpi6r`, this
tree untouched):** patched build + `make minibios-mem` + the unit's own G2 boot
command → rc 0; DSDT still loads at 0xE0210 len 0x92; `11: 26 IO-APIC 11-fasteoi
virtio0`; hvc0 up with `VIRTIO-INPUT16` read back; zero `Firmware Bug` /
`can't derive` lines; the full serial differs from `results/acpi-6-virtio-on-serial.txt`
only in inter-run timing jitter (hrtimer ns, quirk usecs, RTC seconds).

### N1 (NICE-TO-HAVE) — probes 12-15 don't cover the `source` field

`src/hw/hw_selftest.c:1586-1615` covers GSI (`aml[109]`), address (`aml[104]`), pin
(`aml[106]`), bus-end (`aml[82]`) — but not **`aml[107]`, the static-vs-link
discriminator the whole design decision rests on**. A flip `0x00`→`0x01` (ZeroOp →
OneOp) with the checksum repaired is caught by verify()'s `aml[107] != 0x00`
(acpi_tables.c:500); add probe 16. The brief lists the source among the fields that
matter.

### N2 (NICE-TO-HAVE) — probe 13's comment misattributes its catcher

`src/hw/hw_selftest.c:1597-1600` says "the verify's COMPUTED parent_pin check is what
must catch it", but the `prt_addr != 0x000CFFFF` equality (acpi_tables.c:498) fires
first. The arithmetic (`(0 + 13 - 1) & 3 = 0 ≠ 3`) is a genuine second catcher, so the
safety claim holds — only the "who catches this" attribution is wrong.

## Verified true (the brief's five questions)

1. **The entry is TRUE in every configuration where it can be consulted.**
   Slot 12's only possible occupant is the virtio console (`virtio_console.c:471`
   hardcodes `pci_id 0x0C << 3`; the map is 0x00 host / 0x01 ISA / 0x07 PIIX4-PM /
   0x0C virtio / 0x1E IDE, all hardcoded). minibios's probe at 00:12.0
   (`minibios.asm:373-380`) is **presence-based** — it reads the vendor dword, not the
   device id — so `0x63 = 11` is programmed for ANY occupant of slot 12, including a
   future one; the entry's truth does not depend on the virtio specifically.
   No-virtio boots never consult it (the kernel walks `_PRT` per existing device;
   none exists at 0c.0, and `pci_irq_route` skips the write). Reboots: PCI config is
   not reset AND the firmware re-runs `chipset_init` → `pci_irq_route`. The multiboot
   path publishes no tables at all (the only `acpi_tables_publish` sites are the two
   bzImage ones, `main.c:1121/1170`), so no firmware-less boot can consult the `_PRT`.
   The `0xFFFF` function mask matches `pci.c` exactly: `device = pci_id >> 3` drops
   the function (`pci.c:499/512`), and raise `(pin_reg−1 + dev−1) & 3` and lower
   `(pin_reg + dev − 2) & 3` are algebraically identical — both read 0x63 for
   00:0c.0 INTA. The IDE (0x1E) and its legacy 14/15 lines are untouched by
   construction.
2. **The AML, byte by byte** — every element encoding is right (DWordPrefix address LE
   0x000CFFFF; ZeroOp pin; ZeroOp source; BytePrefix GSI; NumElements as raw bytes;
   EISAID 0x030AD041 and both WordIOs unchanged from the acpi-1r-verified tree), the
   WordBusNumber matches QEMU `aml_word_bus_number` byte-for-byte (`88 0D 00`, type
   0x02, MIF|MAF, type_flags 0, len = max−min+1 = 0x100), the DSDT header says 146 =
   36 + 110 exactly, and placement is exactly the macros: the kernel's own log names
   DSDT 0xE0210 (len 0x92) and APIC 0xE02C0 (len 0x4C), reservations disjoint, ONLY
   the MADT moved (no stale 0x280/0x2CC references anywhere; `apic.c:142/278` are the
   LAPIC's own registers). **The one defect is SF1 — the PkgLengths.**
3. **The level-delivery semantics are complete and correct.** `ioapic.c`: a level SET
   sets IRR + REMOTE_IRR and routes; a MASKED level entry PENDS (irr set; the guest's
   unmasking redir WRITE re-runs check_irq and delivers — `ioapic.c:166-178, 298-320`;
   only edge+masked is LOST, and that is no longer this entry); remote EOI clears
   REMOTE_IRR and re-checks, re-delivering iff IRR is still set = the line still
   asserted (one bounded re-fire, not a storm); CLEAR clears IRR for level only.
   `apic.c:246-258`: the LAPIC EOI consults the TMR bit and broadcasts
   `ioapic_remote_eoi` only for level vectors, and `apic_deliver` sets/clears TMR from
   the entry's trigger mode (`apic.c:490/493`) — the chain is whole.
   `virtio.c`: raise per event (`:1031`), `isr_status_read` lowers ALWAYS (`:457-463`);
   two events before the ISR read coalesce on the line (no edge needed — the F1
   mechanism cannot lose them); a re-raise between CLEAR and EOI defers to the EOI
   re-check. No lost interrupt, no stall path found.
4. **The static-vs-link decision is sound**, re-derived from the fetched Linux v6.6
   `drivers/acpi/pci_irq.c`: `source[0] == 0` → `entry->link` stays NULL →
   `gsi = entry->index`, then `acpi_register_gsi(gsi, ACPI_LEVEL_SENSITIVE,
   ACPI_ACTIVE_LOW)` (x86 defaults) — the fasteoi entry. No link object exists for the
   guest to `_SRS` into 0x60-0x62, so the zero-state property is preserved BY
   CONSTRUCTION. The honest caveat ("11 is true while minibios's programming stands")
   is stated where a reader finds it: the builder comment (`acpi_tables.c:145-153`)
   and the report's own section.
5. **The pins are real.** `dsdt_copy` aliases `copy` (`hw_selftest.c:1548`), so the
   checksum repairs are genuine — each probe fails on a FIELD check, not the checksum,
   and the orchestrator's 690/0 run is the positive arm. The unit's first
   address-encoding bug (LE 0x00000CFF) is the class probe 13 + the `prt_addr` pin
   would catch again. Coverage gap: the source field (N1).

## Coverage

Read in full: `acpi_tables.{c,h}` (all 110 AML bytes re-derived mechanically),
`src/cpu/ioapic.c`, `src/cpu/apic.c` (write/EOI/route paths), `pci.c`'s raise/lower
pair and the slot map, `machine.c`'s raise/lower + `ioapic_pin_of_isa_irq`,
`virtio.c`'s isr/raise/lower, `minibios.asm`'s `pci_irq_route` + `chipset_init`,
`hw_selftest.c`'s tables group and probes 1-15, `results/acpi-6.md`. Sampled:
`main.c` (publish sites, flag parse), `ide.h`, `acpi.c`'s device registration,
`virtio_console.c`'s options. Fetched for this review: ACPICA `psargs.c` + `psloop.c`
(PkgLength convention), Linux v6.6 `drivers/acpi/pci_irq.c` (static-entry semantics),
QEMU v8.2.0 `hw/acpi/aml-build.c` (WordBusNumber). Ran: the seabios DSDT convention
walk, the python re-derivation of `pci0_aml`, and a full `--acpi` guest boot on a
scratch `/tmp/acpi6r` build with the corrected PkgLengths (rc 0; serial diff vs the
unit's = timing jitter only). Not checked: bench, aarch64 (the orchestrator ran both).

## Reproduction

```sh
# the convention, from the tree's own vendored BIOS:
python3 - <<'EOF'
import struct
data = open("v86/bios/seabios.bin",'rb').read()
pos = data.find(b"DSDT"); ln = struct.unpack_from("<I", data, pos+4)[0]
aml = data[pos+36:pos+ln]
def pkglen(b,i):
    lead=b[i]; extra=lead>>6; v=lead&(0x0F if extra else 0x3F)
    for k in range(1,extra+1): v|=b[i+k]<<((k<<3)-4)
    return v, extra+1
i=0
while i < len(aml) and aml[i]==0x10 and i < 900:
    L,nb = pkglen(aml,i+1); print(f"ScopeOp @ {i} pkglen={L}({nb}B) -> end {i+1+L}: {aml[i+1+L:i+1+L+6].hex(' ')}"); i = i+1+L
EOF
# the fix's boot proof was: tar-copy the tree to /tmp (exclude build/.git), apply the
# 5 bytes + verify pins, make && make minibios-mem, then the unit's G2 command from
# results/acpi-6.md -- serial identical modulo timestamps.
```
