# ACPI-1 — THE FIRMWARE TABLE BUILDER: answered at the capability gate

**Unit:** `HANDOFF=v86c-acpi`, UNIT ACPI-1 (`coder`). **Outcome: the unit STOPS at G1, as its
brief directs** — the pinned guest kernel can consume NEITHER ACPI tables NOR MP tables, and
cannot route through an I/O APIC at all; "do not build tables nothing can read" applies. The
tree is UNTOUCHED (verified: `git status` clean at `80d6e66`, APIC-1's commit), so every
default-off byte-identity property holds trivially and no regression gate is meaningful.

Every claim is tagged **MEASURED** (run here, command stated), **READ** (artifact, with a
path) or **INTERPRETATION**.

---

## 1. THE CAPABILITY ANSWER (G1) — the brief's stop condition is met

The pinned guest kernel `tests/images/vmlinuz-i386-v86guest` (Linux **6.12.19**, i386,
sha256 `f0278a8f…` MEASURED) has **NEITHER `CONFIG_ACPI` NOR `CONFIG_X86_MPPARSE`**, and in
addition **no I/O-APIC support whatsoever** — three independent sources agree:

### Source 1 — the kernel's OWN config (the decisive artifact, hash-tied to the pinned bzImage)

The brief said the config "is not in this tree"; it IS, inside the staged kernel tarball the
project's own pin file names. `tests/baliveau-kernel-pin.txt` (READ) pins the kernel by the
tarball `tests/images/fixpoint-kernel-6.12.19-v86guest.tar.gz` (`tar_sha256 59719941…`,
MEASURED: matches on disk) and records `config_path config` — **the tarball carries the
kernel's own config under the name `config`** (the brief's bounded search looked for the
fx-demo-side name `config-i386-v86guest.txt`, which is why it missed this).

MEASURED: `tar xzf tests/images/fixpoint-kernel-6.12.19-v86guest.tar.gz` yields `vmlinuz`
(sha256 `f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd` — **identical to
the pinned `tests/images/vmlinuz-i386-v86guest`** and to the pin's own `vmlinuz_sha256_gate`)
and `config` (sha256 `f81de909…`). The config is therefore **authoritative for the pinned
kernel**, not a guess:

| symbol | config line | value |
| --- | --- | --- |
| `CONFIG_ACPI` | `config:373` | `# CONFIG_ACPI is not set` |
| `CONFIG_X86_MPPARSE` | (absent from the file entirely) | not set — its Kconfig deps are unmet |
| `CONFIG_SMP` | `config:230` | `# CONFIG_SMP is not set` |
| `CONFIG_X86_UP_APIC` | `config:290` | `# CONFIG_X86_UP_APIC is not set` |
| `CONFIG_X86_IO_APIC` | (absent from the file entirely) | not set — deps unmet |

(Provenance per the pin file: `make ARCH=i386 tinyconfig` + a lean fragment adding
PCI/virtio/ATA/ext4 only. `tar tzf` MEASURED: the archive holds exactly `vmlinuz` + `config`.)

### Source 2 — the kernel's own boot output (already in the tree's records)

`results/m2-5-c-serial.txt` (the pinned kernel, the boot that prints today's
`/proc/interrupts` via the guest's own init — READ `tests/init-v86gate.sh:63`):

- `NR_IRQS: 16, nr_irqs: 16, preallocated irqs: 16` — on i386, NR_IRQS is 16 exactly when
  the I/O-APIC machinery is not linked (with it, NR_IRQS is 224+). **INTERPRETATION** backed
  by source 3 below.
- ZERO ACPI lines anywhere in the boot. The contrast case is measured: a kernel WITH
  `CONFIG_ACPI=y` (Alpine 6.6, `results/apic-1-g2-on-serial.txt:7-8`) prints ACPICA's
  `"ACPI: Early table checksum verification disabled"` and
  `"ACPI BIOS Error (bug): A valid RSDP was not found (20230628/tbxfroot-222)"` — i.e. the
  RSDP scan runs and is audible when compiled in. The v86guest kernel prints nothing of the
  sort, because there is nothing compiled in to scan.
- Every device IRQ line in `/proc/interrupts` is `XT-PIC` (`11: N XT-PIC virtio0` et al.) —
  the brief's "today" state, which is UNFIXABLE by firmware on this kernel (source 3).

### Source 3 — the decompressed kernel image itself

MEASURED: the bzImage's payload (XZ, `payload_offset 0x79`, `payload_length 0x1299c0`,
extracted per the boot protocol at `(setup_sects+1)*512 + payload_offset`) decompresses to a
**3 269 868-byte prefix** (the stream is truncated at that point — `xz` exits 1). The prefix
still covers every string-bearing section: the boot's own report (`m2-5-c-serial.txt`) sizes
the image at `1917K kernel code, 635K rwdata, 404K rodata, 228K init, 208K bss` — text +
rodata + rwdata + init = **3 184 KiB = 3 260 416 B ≤ 3 269 868 B decompressed**, and the
missing tail is bss (no strings). String search on the prefix:

- `"IO-APIC"` — **ABSENT**. `"IOAPIC"` — absent. `"found SMP MP-table"` — **ABSENT**.
  All `"MPTABLE:"` strings — absent. ACPICA markers (`"ACPI: "`, `"RSD PTR "`,
  `"A valid RSDP"`) — **ABSENT**.
- The params `lapic` / `nolapic` / `noapic` — **ABSENT** as standalone strings, i.e. the
  `"lapic"` boot parameter does not exist in this kernel (`CONFIG_X86_UP_APIC=n`).
  The message `"no APIC, boot with the \"lapic\" boot parameter…"` IS present and prints —
  a dead message whose force-enable path is not compiled (INTERPRETATION, consistent with
  config:290).
- **CONTROL:** `"XT-PIC"` IS present (offset 0x215e22) — the `/proc/interrupts` renderer is
  linked and labels everything XT-PIC. A string that must exist in the same .rodata exists;
  the ones that would exist only with ACPI/MPPARSE/IO-APIC do not. The same-prefix search
  that finds the control cannot be missing the others by truncation (arithmetic above).

**VERDICT (G1):** the pinned kernel needs, at minimum, **`CONFIG_ACPI=y` (which brings
ACPICA and the MADT discovery) or `CONFIG_X86_MPPARSE=y`, AND `CONFIG_X86_IO_APIC=y`** —
the last one is one rung deeper than the brief named, and it is the one that decides the
GATE: without it `/proc/interrupts` can never show `IO-APIC` on this kernel, with or without
tables. `CONFIG_SMP=y` (or `CONFIG_X86_UP_APIC` + `CONFIG_X86_UP_IOAPIC`, the UP pair) is
the other shape the same capability takes. The kernel is built deterministically by the peer
lane (`fx-demo/kbuild/build-v86guest.sh` + `config-i386-v86guest.txt` per the pin file), so
a config delta there is a small, pinned, reproducible change — that is the prerequisite for
THIS unit's builder to have any consumer.

---

## 2. WHAT WAS BUILT — nothing, BY THE BRIEF'S OWN RULE

The brief is explicit: "If the kernel has NEITHER `CONFIG_X86_MPPARSE` NOR `CONFIG_ACPI`,
that IS the answer: the builder is the port-side half, and the kernel needs a config change —
say exactly which symbol, with the evidence, and **stop; do not build tables nothing can
read**." That condition is measured (three sources above). G3 (a builder selftest) and G4
(the builder's no-regression set) gate work that does not exist; the G4 gate that remains
meaningful — no regression — is satisfied by **the tree being byte-unchanged** (MEASURED:
`git status` clean at `80d6e66`; no file in `src/`, `tests/`, or `Makefile` was touched by
this unit). G2 is unreachable on the pinned kernel per §1 and was not run.

## 3. WHAT THE SUCCESSOR UNIT NEEDS — measured now, so the re-plan is one step

### 3a. The layout facts (MEASURED from the tree; the placement problem is already solved)

- Firmware fills `0xF0000-0xFFFFF` (`src/main.c:635`, 64 KiB embedded minibios); option ROM
  512 B at `0xC0000` (`kernel_make_linux_boot_rom`); boot params at `0x80000`, cmdline at
  `0x8E000` (`src/hw/kernel.h`), protected-mode kernel at `0x100000`, initrd at `0x4000000`.
- **`0xE0000-0xEFFFF` is free** — and it is in NEITHER E820 entry: minibios's E820
  (`tests/minibios.asm:798-808`) describes `0-0x9FBFF usable / 0x9FC00-0x9FFFF reserved /
  0xF0000-0xFFFFF reserved / 0x100000-top usable`, so `0xA0000-0xEFFFF` is an e820 GAP, which
  Linux treats as reserved (the boot log confirms: only the four minibios entries appear,
  `results/m2-5-c-serial.txt`). Tables placed there cannot be allocated over by the guest.
- The EBDA base is `0x9FC0` (`minibios.asm:237`, BDA 0x40E) with nothing living there, and
  that 1 KiB is E820-reserved — the MP floating pointer's other legal home if the MP route is
  ever taken (Linux's `smp_scan_config` does NOT scan `0xE0000-0xEFFFF`; the floating pointer
  must be in the EBDA or `0xF0000-0xFFFFF`, while the MP config TABLE itself can live in the
  free window). For ACPI, Linux/ACPICA scans exactly `0xE0000-0xFFFFF` plus the EBDA's first
  KiB, so **RSDP at 0xE0000 (16-byte aligned) with RSDT/XSDT+FADT+MADT behind it is the
  natural layout**, written by a host-side memcpy under `--acpi` only (the same shape as the
  option-ROM copy at `src/main.c:1108`), a pure function of the configuration — determinism
  and default-off identity by construction.

### 3b. A minimal FADT (INTERPRETATION, for the successor; nothing was built)

Linux's early path needs: header (signature `"FACP"`, correct length class + checksum,
OEM ID), `dsdt`/`facs` addresses (a hardware-reduced revision-5 FADT with the
`ACPI_HW_REDUCED_ACPI` flag may carry `facs=0` and skip PM1/GPE blocks entirely — the shape
tiny hypervisors use; alternatively a 36-byte FACS + a trivial DSDT), `pm_profile`-irrelevant
fields zeroed, and `sci_int=9`/`smi_cmd=0` (no SMI -> no ACPI-mode entry needed; MADT parsing
happens before and independently of ACPI mode). What can be LEFT OUT: every PM block, GPE
registers, X_PM1a/_b, sleep control/status — Linux warns and continues for boot-time MADT use.
The guest's own dmesg is the oracle for exactly how far "minimal" can go; the RSDP needs BOTH
checksums (the extended one when revision ≥ 2).

### 3c. THE ALTERNATIVE CONSUMER ALREADY IN THE TREE: `tests/images/vmlinuz-i386` (Alpine 6.6)

APIC-1 gated its LAPIC work against this kernel, and it is a **full-capability consumer
today** — MEASURED by decompressing its gzip payload (16 320 144 B) and string-searching:

- `CONFIG_X86_MPPARSE=y` — `"found SMP MP-table at [mem %#010lx-%#010lx]"` (0xb9eb55),
  `"MPTABLE: checksum error!"`, `"Bus #%d is ISA"`.
- `CONFIG_ACPI=y` — and its RSDP scan RUNS: the tree's own capture
  `results/apic-1-g2-on-serial.txt:7-8` shows ACPICA scanning and failing to find a table.
- `CONFIG_X86_IO_APIC=y` — `"ENABLING IO-APIC IRQs"` (0xb53f50),
  `"IOAPIC[%d]: Preconfigured routing entry…"` (0xb9fc89), the `IO-APIC-` label.
- The `lapic` / `noapic` / `acpi` boot parameters all exist.

INTERPRETATION: with ACPI tables published in `0xE0000-0xEFFFF`, this kernel would discover
LAPIC + IO-APIC from the MADT and route device IRQs through the I/O APIC — Linux prefers
ACPI when both routes exist — which is G2 end-to-end WITHOUT touching the peer lane's kernel.
The choice (rebuild the v86guest config vs. re-pin G2 onto the Alpine kernel) is the
orchestrator's; both prerequisites are now measured.

## 4. EVERY DIVERGENCE / OMISSION, WITH ITS REASON

- No builder, no selftest, no flag change, no gate run (G2/G3/G4): the brief's own stop
  condition, measured. The only commands executed against the tree are read-only
  (decompressions, `strings`, `git status`, `sha256sum`); the working tree is unchanged.
- The capability question is answered from the kernel's config artifact rather than only
  from candidate-table boots. The candidate-table route (build an RSDP, boot, read dmesg)
  would have measured the same thing one boot later: the v86guest kernel has no ACPI code to
  print any message (source 3), so silence would have been the result either way — and the
  config is hash-tied to the exact bzImage, which is stronger evidence than a silent boot.

## 5. WHAT STILL DOES NOT WORK

- The IO-APIC remains guest-undiscoverable on the pinned kernel (config, §1) — the state
  APIC-1 left, now root-caused to the kernel's build rather than to missing firmware tables
  alone: **even with tables, the pinned kernel could not route through it.**
- The LAPIC, likewise, cannot be force-enabled by the pinned kernel (`lapic` param absent;
  `CONFIG_X86_UP_APIC=n`) — APIC-1's LAPIC dmesg evidence was always the Alpine kernel's.
- Both unlock with the same peer-lane config delta (§1), or by moving G2 to the Alpine
  kernel (§3c). The port side of the work (§3a) is measured and ready; the builder itself
  stays unbuilt until a consumer exists, per the brief.

## 6. RAW EVIDENCE (this unit's own commands)

- `tar tzf tests/images/fixpoint-kernel-6.12.19-v86guest.tar.gz` → `vmlinuz`, `config`;
  tar sha256 `59719941c73993ed4931cc48c9b9a3218bf38e2b42c3051c5fd4d69a79d853ef` (= the pin).
  Extracted vmlinuz sha256 `f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd`
  (= `tests/images/vmlinuz-i386-v86guest`, = the pin's `vmlinuz_sha256_gate`). Config sha256
  `f81de9090c829316e4d7abb116ad8a88f0ca571c41e341caf609d8ef25881b75`; the symbol lines are
  quoted verbatim in §1's table (config lines 230, 290, 373).
- v86guest payload: XZ-decompressed prefix 3 269 868 B (stream truncated; coverage
  arithmetic in §1 source 3). Searches: `"IO-APIC"`, `"IOAPIC"`, `"found SMP MP-table"`,
  `"MPTABLE"`, ACPI markers, `lapic`/`noapic` params — all absent; `"XT-PIC"` at 0x215e22
  present (control).
- Alpine kernel `tests/images/vmlinuz-i386` (6.6.134-0-lts SMP): gzip payload → 16 320 144 B
  complete; `"found SMP MP-table at"` at 0xb9eb55, `"MPTABLE: OEM ID"` etc., ACPI RSDP-scan
  strings, `"ENABLING IO-APIC IRQs"` at 0xb53f50, `IOAPIC[%d]` strings, `lapic`/`acpi`
  params — all present.
- `git status --porcelain` → empty at `80d6e66` (APIC-1's commit).
