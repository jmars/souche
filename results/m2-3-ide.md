# M2-3 — the IDE/ATA disk: `hda`, proved by the guest kernel's own driver

**Unit:** `HANDOFF=v86c`, unit M2-3 (coder). **Deliverables:** `src/hw/ide.{c,h}` (the port of
`v86/src/ide.js`, 2917 lines), the disk wiring in `src/main.c` and `src/hw/machine.{c,h}`, the
firmware's chipset bring-up in `tests/minibios.asm`, `tests/mkdisk.py` + `tests/images/disk.img`,
the guest initramfs the gate needs, the `ide` group in `src/hw/hw_selftest.c`, and the
reference leg's `--hda`/`--initrd`/`--memory` options in `oracle/run-boot.mjs`.

**Bottom line: the guest kernel's own `ata_piix`/`libata` driver DISCOVERS, IDENTIFIES and READS
the disk on BOTH machines, and the kernel's dmesg says so.** Same model string, same sector count,
same partition table, and — the strongest form of the claim — the kernel's byte-for-byte read-back
of the disk is identical on both:

```
   the pinned reference (npm v86 0.5.471+g94faf3d)          this port (build/v86c)
   [   18.076] ata2.00: ATAPI: v86 ATAPI CD-ROM, 1.00, max MWDMA2     (identical)
   [   18.078] ata1.00: ATA-0: v86 ATA HD, 1.00, max MWDMA2           (identical)
   [   18.079] ata1.00: 8192 sectors, multi 0: LBA48                  (identical)
   [   18.092] scsi 0:0:0:0: Direct-Access ATA v86 ATA HD n/a PQ: 0 ANSI: 5   (identical)
   [   18.121] sd 0:0:0:0: [sda] 8192 512-byte logical blocks: (4.19 MB/4.00 MiB)  (identical)
   [   18.127] scsi 1:0:0:0: CD-ROM SONY CD-ROM CDU-1000 1.1a PQ: 0 ANSI: 1 CCS  (identical)
   [   18.183]  sda: sda1                                             (identical)
   [   18.190] sd 0:0:0:0: [sda] Attached SCSI disk                    (identical)
   # dd if=/dev/sda bs=512 count=64 | md5sum
   87d5ec3151caf04d90236947e6f09874                                    (identical)
```

`87d5ec3151caf04d90236947e6f09874` is the host's own md5 of `tests/images/disk.img`'s first 64
sectors (`tests/mkdisk.py`'s documented pattern). Neither machine's log is our model of the
device: both are a real 6.6 kernel's `libata` parsing our IDENTIFY packet and reading our sectors.

**Three things had to be found and fixed before that was possible**, and each one is a finding
about the tree rather than about this unit's new code:

1. **The Alpine netboot kernel has NO ATA driver at all** (§4.1). Measured in the decompressed
   kernel: no `libata`, no `ata_piix`, no IDE subsystem; and its netboot initramfs ships no ATA
   module. So the gate needs the kernel's OWN module (from the matching `modloop-lts`) loaded
   through an initramfs — which in turn needs a machine bigger than 64 MiB, because upstream's
   `load_kernel` puts the initrd at the fixed 64 MiB boundary.
2. **The firmware never enabled the PIIX3's IDE channels** (§4.2) — and libata REFUSES to probe a
   channel whose enable bit is clear, silently (`ATA_EHI_QUIET`). This is the unit's central
   finding: it is why the reference's own M2-2 log shows the IDE *controller* and no disk, and it
   is reproducible on upstream v86 as it stands. The fix is in the shared firmware (as seabios
   does it), not in the device model.
3. **Two real bugs of this port**, found by the gate's own userspace and by the selftest (§4.3):
   the privilege-changing `iret32`/`far_return` nulled **CS and SS** along with the four data
   segments (upstream's list is `[ES, DS, FS, GS]`), which aborted the port at the first return to
   userspace (`src/cpu/exceptions.c`, a `src/cpu/` change — **stated loudly here and in the file,
   and it was a real blocker**); and the IDENTIFY packet was ten words short (a structural length
   check now catches it).

All gates green: `make` (from scratch), `make selftest` (**445** checks, 0 failures, 0 skipped —
3522 + 228 + 445), `make nasm` (118/118), `make transcript-diff` (**EMPTY**), `make oracle`,
`make determinism`, and the asymmetry rig (`results/m2-3-mut-rig.sh` → `results/m2-3-mut.txt`).
Raw evidence: `results/m2-3-gates.txt`, and the two gate transcripts
(`results/m2-3-c-serial.txt`, `results/m2-3-js-serial.txt`).

---

## 1. Files, and the JS → C mapping

| C file | lines | upstream | lines | what it is |
| --- | --- | --- | --- | --- |
| `src/hw/ide.c` | 2315 | `v86/src/ide.js` | 2917 | the ATA/ATAPI controller: both channels, four device slots, the task file, PIO and bus-master DMA, IDENTIFY DEVICE/PACKET DEVICE, the READ/WRITE commands, the status/error state machine, the two IRQ lines, and the full ATAPI command set |
| `src/hw/ide.h` | 129 | — | — | the mapping, the deferral decision and its reversal, the ATAPI command constants the selftest drives |
| `src/hw/machine.{c,h}` | +40 | `cpu.js:1219-1228` | — | `machine_config` (the three disk slots = the reference's `settings.hda/hdb/cdrom`) and the IDE controller in the device set, in cpu.js's own order (after the UART, before the PIT) |
| `src/main.c` | +60 | — | — | the boot mode's `--hda FILE` and `--memory MIB` |
| `src/hw/hw_selftest.c` | +470 | — | — | the `ide` group (107 new checks: geometry, IDENTIFY, PIO and DMA reads, the error status, the ATAPI command set) |
| `tests/minibios.asm` | +55 | — | — | `chipset_init`: the PIIX3's IDE-enable bits (§4.2) |
| `tests/mkdisk.py` | 94 | — | — | the deterministic disk image (an MBR + a per-sector pattern) |
| `oracle/run-boot.mjs` | +40 | — | — | `--hda`, `--initrd`, `--memory` for the reference leg |
| `Makefile` | +20 | — | — | `minibios-mem` (the 80 MiB firmware variant) and `ide.c`/`ide.h` in the device-layer lists |

Files **modified** outside this unit's own new code, each with its reason:

| file | change |
| --- | --- |
| `src/cpu/exceptions.c` | **the iret32 and far_return data-segment loops** (upstream `for reg in [ES, DS, FS, GS]`) — the port's transcription iterated `ES..GS` INCLUSIVE, i.e. it also nulled CS and SS. **This is a `src/cpu/` change and the brief forbids `src/cpu/` unless "a REAL blocker forces it" — it is one, MEASURED**: the first `iret32` back to userspace set `segment_is_null[SS]`, and the port then aborted at `src/cpu/modrm.c:89`'s `DBG_ASSERT(segment != CS && segment != SS)` on the guest's next stack access (§4.3). The regression probe is in `src/cpu/m1c_selftest.c`. |
| `src/cpu/m1c_selftest.c` | one new group: the privilege-changing `iret32` nulls the four DATA segments and leaves CS and SS alone. It **fails before the fix** (proven by running the rig against the pre-fix loop) and passes after. |
| `tests/minibios.asm` | `MEM_TOP` becomes overridable (`.ifndef`) and `chipset_init` is called before the option-ROM scan. The DEFAULT build's bytes are unchanged except for the added routine; M2-2's boot is re-measured in §5/G4. |

`v86/` was read only.

---

## 2. The JS → C mapping, in the shape the port uses everywhere

The port is a transcription with the JS receiver dropped (`IDEChannel.prototype.read_status` →
`channel_read_status`, `IDEInterface.prototype.ata_command` → `interface_ata_command`), so every
function can be read side by side with `ide.js`. The mapping decisions worth stating:

* **The port map's callbacks replace the JS closures.** Upstream registers a different closure per
  register and per width; `io.h`'s callbacks take `(device, port, value)`, so one function per
  width per register is what the closures become (`port_data_read8/16/32`, `port_status_read8`, …).
* **The two synchronous read paths are inlined.** `read_buffer(start, length, cb)` exists upstream
  to bridge an ASYNCHRONOUS disk buffer; this port's buffer is a host pointer and both gate legs
  use upstream's own `async: false` (`SyncBuffer`), where the callback runs before `read_buffer`
  returns. `ata_read_sectors` and `atapi_read` therefore call `interface_data_set` where upstream
  calls `read_buffer` — the same sequence of effects, named in the code.
* **The transfer buffer is a growable host allocation** (`interface_data_allocate_noclear`), which
  is upstream's `new Uint8Array(len + 3 & ~3)` semantics; `data_read16/32` assemble their result
  from bytes so the port does not depend on the host's byte order (the APE also runs on aarch64).
* **`dbg_log` has no seat** (the port has no log channel, the M1b-1 rule); the text is kept where
  it carries information. **`get_state`/`set_state` and `eject`/`set_cdrom` are not ported** —
  the same two absences `uart.h` names for the UART (no state-save machinery; nothing in this
  machine changes media at run time).

---

## 3. The device, as the machine presents it

The reference's machine ALWAYS has an IDE controller (cpu.js:1226 gives the secondary channel an
ATAPI CD-ROM unconditionally), so `machine_init()` builds one in **every** mode, including the M0
transcript mode — where the reference has one too. What is optional is the DISK:

| slot | reference | this port |
| --- | --- | --- |
| primary master | `settings.hda` | `--hda FILE` (boot mode); otherwise absent |
| primary slave | `settings.hdb` | `machine_config.hdb` (no CLI; a later unit's call) |
| secondary master | **always** `{is_cdrom: true, buffer: settings.cdrom}` | always an ATAPI device, no media |

* `has_primary` is false with no `--hda`, exactly as upstream's `ide_config[0][0]` is `undefined`;
  the primary channel's ports (0x1F0-0x1F7, 0x3F6) are then **unclaimed** and answer as io.h D1
  says (0xFF/0xFFFF/-1) — MEASURED in the selftest, and in the M0 transcript mode's machine.
* The PCI function 00:1e.0 is registered with upstream's own bytes (vendor 8086, device 7010,
  class 0x010180, prog-if 0x80, BAR0-4 from the channels' bases, INTA) — the kernel enumerates it
  in both machines' logs (`pci 0000:00:1e.0: [8086:7010] type 00 class 0x010180`).
* The geometry upstream derives from the disk's own MBR (`get_disk_geometry`) is what this port
  derives, and it writes the same CMOS registers (`fill_cmos`'s disk fields) that a BIOS reads.
  The 4 MiB gate image's MBR declares 16 heads / 63 sectors, so the geometry comes from the MBR
  (`TRANSLATION_NONE`), and the IDENTIFY packet reports 8/16/63 with 8192 sectors.

---

## 4. THE FINDINGS (each one measured; each cost a real run)

### 4.1 The Alpine netboot kernel has no ATA driver, so the gate needs its own module and an initramfs

MEASURED (`python3` + `zlib` over `tests/images/vmlinuz-i386`): the 16.3 MiB decompressed kernel
contains **no** `libata`, `ata_piix`, `ata_generic`, `sata_*`, `sd_mod`, `virtio_blk` or IDE
subsystem strings — the only `piix` substring in it is `acpi_processor_errata_piix4`, and
`vmlinuz-lts` is the NETBOOT kernel, whose storage drivers are all modules loaded from an
initramfs it does not get here. Alpine's netboot `initramfs-lts` (fetched for this unit) carries
`virtio_blk.ko`, `loop.ko`, `scsi_mod.ko` — and **no ATA module**.

Consequence, measured on the reference with `--hda` before any of §4.2 was found: the kernel
prints its PCI line for 00:1e.0 and **cannot ever print an `ata` line**, whatever the device does.

So the gate loads the kernel's OWN driver: `ata_piix.ko` (and `libata.ko`, `scsi_mod.ko`,
`scsi_common.ko`, `sd_mod.ko`, `t10-pi.ko`, `crc64*.ko`) from Alpine's `modloop-lts`
(`6.6.134-0-lts`, the module version of the very kernel `tests/images/vmlinuz-i386` is), inside a
1.4 MiB initramfs. That in turn forces the machine size: upstream's `load_kernel` places the initrd
at the fixed 64 MiB boundary and says so in its own comment ("the minimum memory size is 64 MB plus
the size of the initrd", `kernel.js:48-50`), so the gate runs an **80 MiB** machine — and the
firmware, which is what reports memory to the guest, is built for it
(`make minibios-mem` → `build/minibios-80m.bin`, one overridable constant) and handed to BOTH legs
(`--bios` / the positional argument), so the two machines still run identical firmware.

The guest's initramfs (`tests/images/guest-initramfs.img`, 1,387,258 bytes,
sha256 `5cf489d7c935dfb7955843a15ae6b989a188854f30f1461727307a45a843cffe`) mounts proc/sys/dev,
`modprobe`s the two modules and reports the kernel's own view, and **never sleeps**: the harness
stops the machine at the first HLT (M0's stop condition, kept), and libata identifies its devices
from an ASYNCHRONOUS EH thread — a guest that goes idle before that thread runs sees no disk. Its
waits are busy waits in userspace, which is what a real machine does while its drivers probe.
MEASURED: without them the reference's log stops at `ata2: PATA max MWDMA2` and the port's at the
same line; with them both reach `sda1`.

### 4.2 THE CENTRAL FINDING: upstream's PIIX3 config space never enables its IDE channels, and libata refuses a disabled channel SILENTLY

Measured, with the port's own `V86C_LOG_IDE` access trace (§6) and then reproduced on the
reference: **neither machine's guest kernel touched a single IDE port.** Not one register read or
write, ever, even with a disk attached and the driver loaded — while /proc/interrupts showed
`14: ata_piix 0` (the driver had *requested* the IRQ) and /sys/class/scsi_host showed the two
hosts registered and idle.

The cause is in the kernel's own source, and it is a rule about the CHIPSET rather than about the
device:

* `ata_piix.c`'s `piix_pata_prereset()` calls
  `pci_test_config_bits(pdev, &piix_enable_bits[port_no])` — **PCI config byte 0x41 bit 0x80 for the
  primary channel, byte 0x43 bit 0x80 for the secondary** — and returns `-ENOENT` when the bit is
  clear. libata then treats the port as disabled and never probes the device behind it.
* Upstream v86's PIIX3 config space (`ide.js:324-350`, transcribed byte for byte in `ide.c`) leaves
  every byte from 0x40 to 0x5F ZERO. So with upstream's model, as it stands, a Linux guest's
  `ata_piix` will not touch the disk.
* Nothing is printed when this happens: `ata_port_probe()` sets
  `ehi->flags |= ATA_EHI_NO_AUTOPSY | ATA_EHI_QUIET` (`libata-core.c:5835-5852`), so a boot probe
  that finds nothing is SILENT. That is why the M2-2 log looked like a device-set difference and
  not like this.
* A real BIOS programs those bits, and **seabios — which is what the browser lane boots with, and
  the reason a disk is visible there — does exactly two writes**
  (`src/fw/pciinit.c:233-237`):

  ```c
  static void piix_ide_setup(struct pci_device *pci, void *arg)
  {
      u16 bdf = pci->bdf;
      pci_config_writew(bdf, 0x40, 0x8000); // enable IDE0
      pci_config_writew(bdf, 0x42, 0x8000); // enable IDE1
  }
  ```

**The fix therefore belongs in the FIRMWARE, not in the device model** — and this port's firmware
(`tests/minibios.asm`) does no chipset init at all, which is why both machines' kernels saw no
disk. `chipset_init` now performs seabios's two writes (bus 0, dev 0x1E, the same widths, the same
bdf) before the option ROM is entered. Both legs share the bytes, so both get it by construction.

MEASURED asymmetry (the rig's M6, `results/m2-3-mut.txt`): with the SECOND of the two writes
deleted, the kernel's dmesg keeps `ata1: PATA max MWDMA2` — and loses `ata2: PATA max MWDMA2`,
`ata2.00: ATAPI: v86 ATAPI CD-ROM`, and the whole `/dev/sda` chain on that channel. The finding is
one line of assembly.

**This is a finding about upstream v86, not only about the port** (§7): with the pinned reference's
own machine, a real Linux kernel cannot see a hard disk either. Any consumer of the v86 lane that
expects `/dev/hda` (the baliveau requirement this unit serves) has to supply these bits — through
its firmware, or by fixing `ide.js`'s config space upstream.

### 4.3 Two real bugs of this port, found by the gate

**(a) `iret32`/`far_return` nulled CS and SS (`src/cpu/exceptions.c`).** Upstream's
privilege-changing return does:

```rust
for reg in [ES, DS, FS, GS] { ... if dpl < *cpl { *segment_is_null.offset(reg) = true; *sreg.offset(reg) = 0; } }
```

The port had transcribed that as `for (int reg = ES; reg <= GS; reg++)`, which in v86's index order
(ES=0, CS=1, SS=2, DS=3, FS=4, GS=5) also visits CS and SS. MEASURED consequence: on the first
`iret32` back to userspace the port zeroed `sreg[SS]`/`segment_is_null[SS]`, and the guest's next
stack access hit `src/cpu/modrm.c:89`'s `DBG_ASSERT(segment != CS && segment != SS)` and aborted the
machine at instruction 833,855,606 (the guest was inside the initramfs's `/init`, at
`eip=0xc192858d`). The reference, whose `dbg_assert` is compiled out and whose list is the four data
segments, runs on. This is a `src/cpu/` change and the brief's own "only if a REAL blocker forces
it" arm; it is stated in the file, here, and on the result node.

**(b) The IDENTIFY packet was ten words short.** `ide.js:2645-2760`'s byte list is exactly 204
entries (102 words) — MEASURED by counting them out of the JS source — and every field above word 0
is addressed by its index, so a miscount silently shifts the lot. The port's model-number padding
was one `W16` loop of 10 where upstream has 20 words. It was caught by a **structural check added
to `interface_create_identify_packet`** (`if (at != 204) { … exit(1); }`), which is now part of the
code, and its effect was visible in the gate itself: the first real boot reported
`8064 sectors, CHS 8/16/63, max PIO2` where the correct packet gives
`8192 sectors, multi 0: LBA48, max MWDMA2`.

### 4.4 The ATAPI deferral: MEASURED, then REVERSED

The brief allows deferring ATAPI/CD-ROM loudly if the baliveau disk path does not need it. The
measurement says it does: with the deferral in place (a loud stop naming the CDB), the guest
kernel's probe of the secondary channel reaches `PACKET` + `INQUIRY` **before any media exists**,
and the stop ends the machine — so `/dev/sda` can never appear behind it. A half-port would have
been fatal here, which is why `atapi_handle`, `atapi_check_condition_response`, `atapi_read`,
`atapi_read_dma` and `do_atapi_dma` are now ported in full (the dispatch, every CDB arm, the sense
state, the media-change sequence). The kernel's own log then shows the device the way upstream's
does: `scsi 1:0:0:0: CD-ROM SONY CD-ROM CDU-1000 1.1a PQ: 0 ANSI: 1 CCS` — the port's INQUIRY
response byte for byte.

What is STILL out of scope, and what a guest observes: `v86/src/iso9660.js` (341 lines) is not
ported — it builds a CD image for tests out of a directory listing; it does not drive the device.
No caller wires a CD image (`machine_config.cdrom` has no CLI flag), so the ATAPI drive is always
the reference's "no media" device, and every medium-dependent CDB answers CHECK CONDITION /
NOT READY / MEDIUM NOT PRESENT exactly as upstream's does.

### 4.5 The clock, seen from the device side (a consequence of M2-1F's policy)

`/proc/interrupts` in the guest, on the port: `0: 11615 XT-PIC timer` after ~1033 guest seconds —
i.e. ~11 Hz where the kernel asked for HZ=250. The port's policy (M2-1F's D-F2: one instruction =
one microsecond) makes the guest's clock advance with INSTRUCTIONS, and the PIT's period is
therefore reached at an instruction distance, not in host time; the kernel's own timers and its
scheduler's preemption points inherit that. The visible consequence for this unit: kernel
workqueue work (libata's `async_schedule(async_port_probe)`, the SCSI EH thread's turn) lands
hundreds of millions of instructions after it is queued — the port's probe lines appear at guest
`t=725` where the reference's appear at `t=18`. It is a clock effect and not a device effect: at
the same *point of the guest's execution* the two machines do the same thing, and the device state
the two kernels end with is identical.

---

## 5. THE GATES

### G1 — the external oracle: the kernel's own dmesg, both machines

Machine, both: 80 MiB (a consequence of §4.1), the same firmware bytes
(`build/minibios-80m.bin`), `tests/images/disk.img` as `hda`, `tests/images/guest-initramfs.img` as
the initrd, `console=ttyS0`.

```
port:      ./build/v86c --bzimage tests/images/vmlinuz-i386 4000000000 --cmdline "console=ttyS0" \
               --hda tests/images/disk.img --initrd tests/images/guest-initramfs.img \
               --bios build/minibios-80m.bin --memory 80
reference: node oracle/run-boot.mjs tests/images/vmlinuz-i386 build/minibios-80m.bin 4000000000 \
               "console=ttyS0" --hda tests/images/disk.img \
               --initrd tests/images/guest-initramfs.img --memory 80
```

**The kernel's own lines** (full transcripts: `results/m2-3-c-serial.txt` 294 lines,
`results/m2-3-js-serial.txt` 265 lines):

```
   the reference                                              this port
   [   17.058] libata version 3.00 loaded.            [  673.206] libata version 3.00 loaded.
   [   17.888] scsi host0: ata_piix                   [  722.581] scsi host0: ata_piix
   [   17.900] scsi host1: ata_piix                   [  723.404] scsi host1: ata_piix
   [   17.908] ata1: PATA max MWDMA2 cmd 0x1f0 ctl 0x3f6 bmdma 0xb400 irq 14   (identical)
   [   17.908] ata2: PATA max MWDMA2 cmd 0x170 ctl 0x376 bmdma 0xb408 irq 15   (identical)
   [   18.076] ata2.00: ATAPI: v86 ATAPI CD-ROM, 1.00, max MWDMA2              (identical)
   [   18.078] ata1.00: ATA-0: v86 ATA HD, 1.00, max MWDMA2                    (identical)
   [   18.079] ata1.00: 8192 sectors, multi 0: LBA48                           (identical)
   [   18.092] scsi 0:0:0:0: Direct-Access ATA v86 ATA HD n/a PQ: 0 ANSI: 5    (identical)
   [   18.121] sd 0:0:0:0: [sda] 8192 512-byte logical blocks: (4.19 MB/4.00 MiB) (identical)
   [   18.127] scsi 1:0:0:0: CD-ROM SONY CD-ROM CDU-1000 1.1a PQ: 0 ANSI: 1 CCS (identical)
   [   18.183]  sda: sda1                                                      (identical)
   [   18.190] sd 0:0:0:0: [sda] Attached SCSI disk                             (identical)
   /proc/partitions: 8 0 4096 sda / 8 1 1024 sda1                              (identical)
   dd if=/dev/sda bs=512 count=64 | md5sum
   87d5ec3151caf04d90236947e6f09874                                    87d5ec3151caf04d90236947e6f09874
```

**Every divergence, with its cause** (there are two kinds and no others):

| # | divergence | cause (MEASURED) |
| --- | --- | --- |
| D1 | the guest TIMESTAMPS differ by two orders of magnitude (`t≈18` on the reference, `t≈725` on the port) | M2-1F's D-F2 clock policy, restated in §4.5: the port's guest time is instruction-driven (1 instruction = 1 µs), the reference's is the host wall clock. The instruction counts to the same point differ for the same reason (`results/m2-3-gates.txt`); the SEQUENCE of the kernel's lines is identical. |
| D2 | the reference's line ORDER differs by one position: `scsi 1:0:0:0: CD-ROM` appears after `sd 0:0:0:0: [sda] Write Protect` on the port and before it on the reference | the same clock difference, seen from the scheduler: the two channels' async probes and the SCSI scan interleave differently when the timer rate differs (§4.5). Both orders are the same set of lines; both machines end with `sda1` and the same md5. |
| D3 | the gate's disk image is 4 MiB, not a formatted filesystem | `tests/mkdisk.py`: the gates are about the CONTROLLER. The MBR makes the kernel's own partition parsing observable (`sda: sda1`), and the per-sector pattern makes the bytes observable (the md5 above). A filesystem would add a second thing that can fail without adding a fact about the device. |
| D4 | the machine is 80 MiB and carries an initramfs; M2-2's boot was 64 MiB with none | §4.1: the kernel has no built-in ATA driver and upstream's `load_kernel` puts an initrd at the fixed 64 MiB boundary. Both legs take the same numbers, so the comparison is unchanged; M2-2's own boot is re-verified as it was (G4). |

### G2 — the selftest, and the proof that it can FAIL

`make selftest` → `hw_selftest: 445 checks, 0 failures, 0 skipped` (338 → 445; the new group is
107 checks). It drives the device through **the CPU's own `out`/`in` instruction bodies**
(`instr_E6/E4`, `instr16_ED`, `instr32_ED`) — the path a kernel's driver takes — and asserts:

* the machine's device set (`ide_channel_is_present`) with a disk, and with none (then the primary
  channel's ports are unclaimed);
* the geometry the MBR implies (CMOS 8/16/63 + the disk-type registers + `TRANSLATION_NONE`), and
  the FALLBACK route on a second image whose MBR says 8 heads / 32 sectors (both routes are built
  so they disagree, which is what makes the MBR arm testable);
* the PCI function's config space (vendor/device/class/BAR0-4/INTA) **and the two enable bits
  upstream leaves clear** (so §4.2's device-side fact is asserted, not assumed);
* IDENTIFY DEVICE: word 0, the geometry words, the sector count, the DMA/version words, and the
  three ATA strings (byte-swapped as the ATA encoding is, NUL-padded as upstream's fill quirk
  leaves them);
* READ SECTORS: the exact bytes of a patterned sector at 8-, 16- and 32-bit widths; sector 0's MBR
  (0x55AA, type 0x83); a multi-sector read where the second sector is the next one (the LBA
  advance); `status = 0xFF` outside the disk (upstream's own error arm); an unimplemented command
  answering ABORT (error 0x04, status DRDY|ERR);
* READ DMA through a PRD table in guest memory — **the path a real kernel uses** — landing the
  disk's bytes where the table says;
* the ATAPI device's identity (its signature registers and its IDENTIFY PACKET DEVICE packet), the
  PACKET + CDB protocol, the INQUIRY response, REQUEST SENSE, and CHECK CONDITION/NOT READY/MEDIUM
  NOT PRESENT for a medium-dependent CDB with no media.

**G2's asymmetry: `results/m2-3-mut-rig.sh` → `results/m2-3-mut.txt`, six single-line mutations,
each alone, each rebuilt, each required to fail, then restored and required green.** All six were
VISIBLE; the before/after pair is the proof.

### G3/G4 — the project's gates, raw in `results/m2-3-gates.txt`

| gate | result |
| --- | --- |
| `make clean && make` | rc=0 (from scratch) |
| `make selftest` | rc=0 — `cpu_selftest: 3522 checks, 0 failures` (**unchanged**), `cpu_m1c_selftest` (228 + the new iret32 probe), `hw_selftest: 445 checks, 0 failures, 0 skipped` (338 → 445) |
| `make oracle` | rc=0 — the M0 transcript still matches `tests/model.py` |
| `make transcript-diff` | rc=0, diff **EMPTY** — the IDE controller is now in the M0 machine and the C transcript is still byte-identical to the reference's |
| `make nasm` | rc=0 (the differential corpus) |
| `make determinism` | rc=0 — two consecutive reference runs byte-identical |
| G4: the M2-2 boot | rc=0, 294 lines, 4 `BIOS-e820` rows, `Kernel panic … VFS: Unable to mount root fs` present, and **zero** `ata`/`sda` lines (no disk attached, no ATA driver built in) — the same final state as M2-2's report (`eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000`) |

---

## 6. The instrument that made §4.2 findable: `V86C_LOG_IDE`

`src/hw/ide.c` carries a gated access trace (like `io.h`'s `V86C_LOG_PORTS`): with `V86C_LOG_IDE`
set, every IDE register access prints its port, width, direction and value to stderr. It DECIDES
NOTHING — no return value, device or port-map byte depends on it — and it exists because the
kernel's own probe is silent by design (§4.2), so the only way to see what a real driver does to
the device is to watch the device's own port. It is what produced the "zero accesses" measurement,
and the same trace with the firmware fix shows the whole libata sequence (reset, IDENTIFY, the
512-word data reads, the DMA PRD walk).

---

## 7. Divergences from upstream, deliberately, and what they cost

| site | divergence | reason / guest-visible cost |
| --- | --- | --- |
| `src/hw/ide.c` | the ATAPI command set IS ported, but the ISO9660 image builder is not | §4.4. Nothing drives a CD image in this machine, so a guest sees upstream's "no media" behaviour for every medium-dependent CDB. |
| `src/hw/ide.c` | `START_STOP_UNIT`'s eject clears `buffer`/`buffer_length` (upstream nulls a JS buffer reference) | C has no garbage-collected null; the machine's disks are `machine_config`'s pointers, which the interface must not `free`. The observable effect is upstream's: the media is gone and `medium_changed` reports it. |
| `src/hw/ide.c` | the DMA PRD walk REFUSES a table that describes more than the transfer, and a PRD address outside guest RAM stops with the address named | upstream's `write_blob(data.subarray(offset, offset + prd_count), addr)` under-fills silently and `mem8.subarray` past the end yields a short array whose assert is compiled out; the port names both. No gate reaches either (a kernel's PRD table is exactly the transfer size). |
| `src/hw/ide.c` | `data_pointer + length - 1 >= data_end` is a rate-limited stderr note, not an abort | upstream's `dbg_assert` there is compiled out of the pinned release and the read still returns the buffer's bytes; the port keeps the release's behaviour and says so. |
| `src/hw/ide.c` | the unimplemented-ATA-command arm and the `0xF0` vendor command keep upstream's ABORT and name the command on stderr | upstream's `dbg_assert(false, …)` is compiled out of the release, so the guest's observable behaviour IS the abort. |
| `src/hw/ide.c` | `if (at != 204) exit(1)` in `create_identify_packet` | a structural check with no upstream counterpart; it is the check that found §4.3(b). |
| `src/hw/ide.c` | `read_buffer`'s cancellation bookkeeping is a named no-op | §2: a synchronous buffer has nothing in flight at a cancellation point (upstream's own `SyncBuffer` behaviour). |
| `src/hw/ide.{c,h}` | no `get_state`/`set_state`, no `dbg_log`, no `current_line`-style DEBUG bookkeeping | the port has no state-save and no log channel (`uart.h`'s precedent). |
| `tests/minibios.asm` | `chipset_init` (two PCI config writes) | §4.2 — a real BIOS's chipset init, absent from this firmware until now, and the reason no kernel could see the disk. |
| `src/main.c` | `--hda`, `--memory` (both boot-mode only) | the M0 oracle contract's two-argument form is untouched; the transcript mode's machine still gets its IDE controller (as the reference does). |
| `oracle/run-boot.mjs` | `--hda`, `--initrd`, `--memory` | the reference leg takes the same three settings; its four positional arguments are unchanged. |

---

## 8. Handoff notes

* **The firmware is the seam, and this unit added a chipset step to it** (§4.2). Any later unit that
  needs another device visible to a real driver should look for the chipset/PCI configuration the
  driver checks BEFORE concluding the device model is wrong — that is what cost this unit most.
* **The `V86C_LOG_IDE` trace is the instrument to reach for** when a kernel silently ignores a
  device: a kernel's boot probe is allowed to be quiet, and "no message" is not "no probe".
* **For the baliveau lane (the consumer this unit serves):** `/dev/hda` now works in this port —
  the kernel's own `sd` driver binds, `sda1` appears, and the bytes read back are the disk's. Two
  caveats are upstream v86's, not the port's: the *browser* lane gets the IDE channels enabled only
  because seabios does it (a purpose-built firmware must do it too — `tests/minibios.asm`'s
  `chipset_init` is the working example), and v86's IDE has no `virtio-blk`/CD image wiring, so a
  guest's root filesystem must come from the disk image itself.
* **The clock policy (M2-1F D-F2) is what to watch for anything asynchronous.** A guest kernel that
  waits on a workqueue, an async probe or a timer-driven state machine takes hundreds of millions of
  instructions to get its turn on the port where the reference needs host milliseconds; both gates
  in this unit had to keep the guest BUSY to see the effect. `baliveau`'s boot timing may need the
  same treatment.
