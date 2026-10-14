# M2-5 — SLICE 1: the shared v86 guest kernel (one i386 kernel, ATA + virtio-console)

**Unit:** `HANDOFF=v86guest`, slice 1 (the kernel). **Deliverables:** the build recipe and
its resolved config (`fx-demo/kbuild/build-v86guest.sh`, `config-i386-v86guest.txt`), the
gate's own kernel/initramfs/init under `tests/`, `tests/baliveau-kernel-pin.txt`, the gate
`tests/gate-m2-5.sh`, and this write-up. Raw evidence: `results/m2-5-gates.txt` (the whole
gate, one capture), `results/m2-5-{c,js}-serial.txt`,
`results/m2-5-{c,js}-virtio.txt`, `results/m2-5-leg-diff.txt`, the failure arm's
`results/m2-5-lean{c,js}-*.txt`, and `results/m2-5-qemu-*.txt`.

**§7's probe matrix has its OWN raw evidence (ADDED in round 2, review finding S1):**
`results/m2-5-probe-rig.sh` re-runs every cmdline in §7's tables — including the FAILING
ones and the first-run state — and keeps the raw serial / virtio / stderr of EVERY run as
`results/m2-5-probe-{c,js}-<cmdline>-{serial,virtio,stderr}.txt` (13 runs, keystroke-free:
`sh results/m2-5-probe-rig.sh`), with the table regenerated from those files into
`results/m2-5-probe-matrix.txt`. Nothing else changed: the kernel, the recipe, the gate and
every gate assertion are untouched by this addition.

**Bottom line: ONE i386 kernel boots the same guest on the C port AND on the pinned JS
reference with an ATA disk AND a virtio console, byte-identical vmlinuz from two cold
container builds, zero modules.** The guest kernel's own `libata` drives the disk
(`ata1.00: ATA-0: v86 ATA HD`, `sda`, `sda1`), the guest reads the patterned disk through
`/dev/sda` and gets **m2-3's pinned md5 back**, the kernel brings up `/dev/hvc0`
(`printk: legacy console [hvc0] enabled`, `11: N XT-PIC virtio0`), and a byte written by
the HOST is read back through the device:

```
   the pinned JS reference (npm v86 0.5.471+g94faf3d)          this port (build/v86c)
   ata1.00: ATA-0: v86 ATA HD, 1.00, max MWDMA2                (identical)
   ata1.00: 8192 sectors, multi 0: LBA48                       (identical)
    sda: sda1                                                  (identical)
   sd 0:0:0:0: [sda] Attached SCSI disk                        (identical)
   printk: legacy console [hvc0] enabled                       (identical)
   11:  29  XT-PIC  virtio0                                    (identical device, line)
   md5 of /dev/sda's first 64 sectors: 87d5ec3151caf04d90236947e6f09874   (identical)
   read back from /dev/hvc0: [VIRTIO-INPUT16]                  (identical)
   GUEST-INIT: end / reboot: System halted                     (identical)
```

All gates green: `sh tests/gate-m2-5.sh all` → **0 failing assertion(s)** (pins 7/7, gate
1b 10/10, gate 1a 14/14 on the C leg and 10/10 on the reference leg — the reference leg's
10 are a SUBSET of the C leg's 14, see §5 — the failure arm's
7 absence assertions + 3 "the same init ran" on the C leg and 4 on the reference leg).

**THE ONE THING THAT HAD TO BE FOUND (§7): the harness stops the machine at the FIRST HLT
(M0's stop condition, kept), and a UP 6.12 guest booted by v86 HALTS ITSELF MID-BOOT
unless the cmdline carries `idle=poll`** — the emulated TSC jumps, the tick is then not
programmed, and the run ends before userspace. The stop is the kernel's OWN idle `hlt`:
§7 resolves both legs' failing terminal eip (`0xc11db37e`) against the build's System.map to
`default_idle+0x12`, the instruction after `default_idle`'s `hlt`, so this is the machine
idling, not a port-specific code path. Measured on the SAME cmdline on both legs (§7,
round 2): `tsc=unstable` alone carries the C leg (both `console=ttyS0 tsc=unstable` and
`console=hvc0 console=ttyS0 tsc=unstable` PASS) and does NOT carry the reference leg (both
FAIL at the idle HLT). `idle=poll` alone carries both legs. The gate uses both, and **slice
3's gate cmdline needs `idle=poll` added** — the plan already has `tsc=unstable` there, and
on the reference leg that alone is not sufficient.

---

## 1. The artifact, and its pins

| what | where | sha256 |
| --- | --- | --- |
| vmlinuz (the kernel) | `tests/images/vmlinuz-i386-v86guest` (1,253,888 B) | `f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd` |
| the trimmed tarball (vmlinuz + its own config) | `tests/images/fixpoint-kernel-6.12.19-v86guest.tar.gz` (1,253,908 B) | `59719941c73993ed4931cc48c9b9a3218bf38e2b42c3051c5fd4d69a79d853ef` |
| the resolved config (2047 lines, 494 `=y`, **0 `=m`**) | `fx-demo/kbuild/config-i386-v86guest.txt` | — |
| the recipe | `fx-demo/kbuild/build-v86guest.sh` | — |
| the pin (fx-init's key set) | `tests/baliveau-kernel-pin.txt` | — |

`kernel_version` 6.12.19, banner `Linux version 6.12.19 (build@debian) (gcc (Debian
14.2.0-19) 14.2.0, GNU ld (GNU Binutils for Debian) 2.44) #1 2025-03-13 12:15:54 UTC`,
`CONFIG_X86_32=y`, `CONFIG_M686=y`, `CONFIG_X86_PAE`/`SMP`/`NET`/`UNIX`/`64BIT` all UNSET.

**THE ONE GAP, stated rather than papered over:** the plan's artifact home is an fx-demo
release asset, and publishing needs repo write access this implementer does not have, so
`tests/baliveau-kernel-pin.txt` carries `url PENDING-NOT-YET-PUBLISHED` with the tarball's
hash beside it. The tarball itself is a real file (the path above); the gate consumes the
vmlinuz, not the tarball. Publishing + replacing that one line is the orchestrator's step.

## 2. The recipe: the lean fragment VERBATIM, plus a second invocation

`fx-demo/kbuild/build-lean.sh` is the pinned lean control and is **not edited**. The new
recipe runs *its* `./scripts/config` invocation byte for byte — MEASURED: the 20-line
first invocation of the two files is IDENTICAL (`cmp`/python compare) — and then a SECOND
invocation carrying the delta, so a diff of the two fragment blocks is the whole review of
what this kernel adds:

* `BLOCK` + `BLK_DEV`, `PCI`, `SCSI`, `ATA`/`ATA_PIIX`/`ATA_GENERIC`, `BLK_DEV_SD`
* `VIRTIO`/`VIRTIO_MENU`/`VIRTIO_PCI`/`VIRTIO_CONSOLE`
* `EXT4_FS`/`EXT4_USE_FOR_EXT2`/`JBD2`/`FS_MBCACHE`/`CRC16` + the CRYPTO chain, copied as
  the exact `=y` symbol set of the pinned x86_64 config

### 2.1 THE FINDING THE RECIPE'S OWN ASSERTION CAUGHT: `CONFIG_BLOCK` is the parent nothing names

The first cold build FAILED the recipe's fatal assertion:

```
FATAL: not =y in the resolved config: SCSI ATA ATA_PIIX ATA_GENERIC BLK_DEV_SD EXT4_FS JBD2 FS_MBCACHE
```

`make tinyconfig`'s allnoconfig base leaves **`CONFIG_BLOCK` off** — MEASURED: `#
CONFIG_BLOCK is not set` is line 531 of the *lean control's own resolved config*, so the
kernel that ships today has no block layer at all (its `BLK_DEV_INITRD=y` survives because
initramfs does not depend on BLOCK). `BLOCK` is the parent of `drivers/scsi/Kconfig:23`'s
`config SCSI depends on BLOCK`, of `menuconfig ATA`, and of the whole `if BLOCK` filesystem
menu. **Setting a child does not turn a parent on**, so the plan's delta list alone
resolves every block-backed symbol to UNSET — silently, exactly the shape `build-lean.sh`'s
own comment documents for `UNIX`/`NET`. That is why the assertion block is fatal: it caught
this on the first build rather than at boot. The fix adds `--enable BLOCK --enable BLK_DEV`,
which is the *same rule* the plan gives for the ext4 chain — `CONFIG_BLOCK=y` is line 654
of the pinned x86_64 config.

### 2.2 DETERMINISM: MEASURED, byte-identical — not the pin-and-fetch fallback

Two COLD builds (separate source trees, separate `debian:stable` containers, run
concurrently) with `KBUILD_BUILD_TIMESTAMP`/`KBUILD_BUILD_USER`/`KBUILD_BUILD_HOST`
pinned:

| build | vmlinuz sha256 | tarball sha256 |
| --- | --- | --- |
| cold build 1 | `f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd` | `59719941c73993ed4931cc48c9b9a3218bf38e2b42c3051c5fd4d69a79d853ef` |
| cold build 2 | `f0278a8f2893eb93a8c857f3f2d5a589a5b9aa6a45033171bc5411fc10f658cd` | `59719941c73993ed4931cc48c9b9a3218bf38e2b42c3051c5fd4d69a79d853ef` |

`cmp` on the two vmlinuz files is clean. Both hashes were **also computed on the host**
over the two build directories (an implementer's in-container report is a claim; the host
`sha256sum` is the check). The plan allowed the pin-and-fetch model as a fallback: it is
**not needed**, and this write-up claims byte-identity only. The repack is the
house-deterministic one (`tar --sort=name --owner=0 --group=0 --numeric-owner
--mtime=@0`, `gzip -n`).

**THE SCOPE OF THAT CLAIM (stated, round 2):** byte-identity is measured between **these
two builds, in THIS toolchain** — the container tag is `debian:stable`, a MOVING tag, so a
rebuild from a later `debian:stable` (a different gcc/binutils) may legitimately produce a
different hash. The pins in `tests/baliveau-kernel-pin.txt` and in `tests/gate-m2-5.sh`
therefore pin the artifact by its bytes, not by a promise that the recipe reproduces those
bytes forever; the determinism claim is `f0278a8f…` **as built by gcc 14.2.0-19 / GNU ld
2.44 from debian:stable on 2026-10-12**, not an unconditional property of the recipe.

## 3. Gate 1c — the recipe's own assertions (green)

Every named symbol `=y`; `NET`/`UNIX`/`64BIT`/`X86_PAE`/`SMP` UNSET (the lean control's own
invariants, kept); **`=m lines: 0`** — the ZERO-MODULE invariant is fatal in the recipe, so
no image can silently need `/lib/modules`; the resolved config is shipped INSIDE the
tarball (`tar tzf …` → `vmlinuz`, `config`).

## 4. Gate 1b — qemu/KVM, the fast pre-check (`rc=124` = the timeout reaping an already
halted machine)

`qemu-system-x86_64 -m 80 -drive if=ide -device virtio-serial-pci -device virtconsole`
produces the same lines from the same files: `ata1: PATA max MWDMA2`, `ata1.00: ATA-7:
QEMU HARDDISK` (qemu's own disk model — the *device* differs, the kernel path is the same
one), `sda: sda1`, `sd 0:0:0:0: [sda] Attached SCSI disk`, `md5 … 87d5ec31…` (**the same
pinned md5, on a third host**), `console [hvc0] enabled`, `11: N XT-PIC virtio0`, and
1696 bytes of the kernel's log arriving through the virtio console. Its host end is a file,
so there is nothing to answer the sentinel with and the read-back is gate 1a's.

## 5. Gate 1a — the two-leg oracle, and the leg diff classified

Machine: 80 MiB, `build/minibios-80m.bin`, `--hda tests/images/disk.img` (mkdisk.py's
patterned MBR image), the new initramfs, `--virtio-console` + `--virtio-console-in`,
cmdline `console=hvc0 console=ttyS0 tsc=unstable idle=poll`, limit 3e9. Both legs halt
THEMSELVES at the guest's own `halt -f`:

| | instructions | terminal state |
| --- | --- | --- |
| C port | 579,728,752 | `in_hlt=1 eip=0xc101acb2 cs=0x60`, 1394 bytes out, 15 in, **inbound handshake fired** |
| reference | 580,515,118 | `in_hlt:1 eip:"0xc101acb2" cs:"0x60"`, 1394 bytes out, `virtio_console_in_sent:true` |

Same terminal eip and the same byte counts on both — the run ends at the same guest point,
not at the limit. **The terminal EIP is the reproducible part; the INSTRUCTION COUNT is
not** — against the build's System.map this eip is `stop_this_cpu+0x3e`, the instruction
after the halt loop's `hlt` (the guest's own `halt -f`), and it comes back identical on every
PASS run, while the count varies by tens of thousands between runs (§7's re-runs: the same
cmdline gave 579,728,752 in the gate capture and 579,621,977 / 580,645,197 in the probe
re-runs) because the guest reads the host clock. The counts in the tables are that one run's.

The device lines ARE identical; `results/m2-5-leg-diff.txt` is **34 diff lines — 10 hunk
headers, 2 `---` separators and 22 content lines** (the write-up said 42 in round 1; that
number was wrong, the file is unchanged) — and every content line is one of three
already-known classes (m2-3's/M2-4's divergence tables):

1. **`pci 0000:00:12.0` — the v86 VGA PCI function (14 content lines), present only on the
   reference leg.** Pre-existing and inherited: M2-4's own leg diff carries these same
   lines. The C port does not model the VGA device's PCI function; the kernel enumerates
   it in one leg and not the other. Not this slice's code (nothing in `src/` was touched).
2. **`sd 0:0:0:0: [sda] Attached SCSI disk` appears on a different side of ONE
   neighbouring line** (2 lines moved, twice = 4 content lines) — an interleaving difference of a line both
   legs emit, not a missing fact.
3. **`/proc/interrupts` counts** (2 lines changed = 4 content lines: `timer` 578 vs 875,
   `ttyS0` 213 vs 183) — the
   instruction-driven clock, m2-3's D1/D2 class.

**The two legs' ASSERTION SETS are not symmetric (stated, round 2 — "the same gate" would
overstate it).** The gate asserts **14** lines on the C leg and **10** on the reference leg,
and the JS leg's 10 are a strict SUBSET of the C leg's 14: `tests/gate-m2-5.sh`'s `leg_js`
block does not assert `GUEST-INIT: /dev/sda: present`, the
`GUEST-INIT: /proc/filesystems ext4 entries: 1` count, or the two virtio-console-stream
assertions. The asymmetry is in the ASSERTIONS, not in the evidence — the reference leg's own
transcripts CONTAIN all four (`results/m2-5-js-serial.txt`,
`results/m2-5-js-virtio.txt`) — and discrimination is not weakened, because the serial-side
assertions the JS leg does make are the kernel's own `libata`/SCSI/`hvc0`/`idle` facts. A
later round could drop the asymmetry by adding those four `want`s to `leg_js`; this round did
not change any gate assertion.

## 6. The failure arm — the pinned lean control, which needs no reconstruction

The SAME gate, same disk, same initramfs, same cmdline, kernel =
`tests/images/vmlinuz-i386-lean-control` (sha256 `25be0d53…` = fx-demo's shipped
`web/bzImage` in `SHA256SUMS` — the kernel the browser lane boots today):

* ABSENT, both legs: `ata1.00: ATA-`, `sd 0:0:0:0: [sda]`, `87d5ec3151caf04d90236947e6f09874`,
  `console [hvc0] enabled`, `XT-PIC virtio0`, the read-back — 7/7 PASS.
* And the negative is a MEASUREMENT, not a silence: the same init ran and reached its own
  probes on both legs — `GUEST-INIT: /dev/sda: absent`, `GUEST-INIT: /dev/sda1: absent`,
  `/proc/filesystems ext4 entries: 0`, `/dev/hvc*:` empty, `waited 5000 round(s) for
  /dev/hvc0`, `GUEST-INIT: end`. Both legs also halt at the same eip `0xc101399a`.

So the fail-before is the shipped kernel's own behaviour, and the pass-after is the new
kernel's, on the same machine and the same userspace.

## 7. THE FINDING: the guest idles into an HLT mid-boot, and the cmdline is part of the machine

The first gate-1a run used `console=hvc0 console=ttyS0` (m2-4's cmdline) and BOTH legs
stopped before userspace. **Re-run in round 2 and KEPT as raw evidence**
(`results/m2-5-probe-{c,js}-hvc-ttyS0-*`; the whole matrix below is regenerated from those
files by `results/m2-5-probe-rig.sh` into `results/m2-5-probe-matrix.txt`, ~18 s for all 13
runs):

| | instructions | state | serial |
| --- | --- | --- | --- |
| C port | 542,440,198 | `in_hlt=1 eip=0xc11db37e cs=0x60 cr0=0x80050033 cr3=0x1320000` | 109 lines, last = `sched_clock: Marking stable (…)` |
| reference | 479,427,193 | `in_hlt=1 eip=0xc11db37e cs=0x60 cr0=0x80050033 cr3=0x1320000` | 48 lines, last = `Mountpoint-cache hash table entries: 1024 …` |

**Both legs stop with `in_hlt=1` at the same eip, and that eip is the kernel's OWN idle
path.** Resolved against the System.map of the build that produced this vmlinuz
(`build1/linux-6.12.19/System.map`, vmlinux `a950b978…`), `0xc11db37e` is
**`default_idle+0x12` — the instruction after `default_idle`'s `hlt`** (`0xc11db37d hlt`;
`0xc11db37e cli`). So the stop is `default_idle`'s HLT: the CPU had nothing to run, and with
the tick not programmed it never woke. What the cr0/cr3 identity establishes is even less:
**MEASURED** — the reference leg stops 61 console lines EARLIER than the C leg yet reports
the SAME `cr0`/`cr3` (`0x80050033`/`0x1320000`), so those two registers are the same at two
different boot points and cannot evidence "the same point". The shared eip is the idle
address, i.e. a shared idle PATH — **it does not establish that the two legs stopped at the
same boot point**, and the re-runs show they did not: the C leg's serial
runs 109 lines to the guest's own `sched_clock: Marking stable (…)` line, while the reference
leg's serial is only its first 48 lines, ending at `Mountpoint-cache …` (a strict prefix of
the C leg's — MEASURED, `diff` of the two transcripts). INTERPRETATION: the boot CPU enters
`do_idle()` whenever `kernel_init` is not runnable, so WHICH boot point the first HLT lands
on depends on the emulated timer's timing, which differs between the two machines. The
mechanism the flags change is the missing tick: the emulated TSC jumps (the guest's own
`sched_clock: Marking stable (61459108999, 281323000)->(65858044000, -4117612001)` line), the
tick is then not programmed, and the run ends at the next idle HLT. (The plan's slice-3
cmdline already carries `tsc=unstable`, which is consistent with this being the known shape
of the problem in this lane.)

**CORRECTION (round 2, from the transcripts this round added).** Round 1 wrote: "the two
machines agree EXACTLY — the same eip, cs, cr0 and cr3 — so this is not a port divergence.
Both legs' last line is the guest's own `sched_clock` line." The first clause is true but its
eip is the SHARED idle address (`default_idle+0x12`), so it evidences a shared idle path, not
identical boot progress; and the "both legs' last line" clause is true only of the C leg —
the reference leg's serial never reaches the `sched_clock` line (it ends 61 lines earlier).
**The conclusion is unaffected**: both machines stop inside the same kernel idle function,
which no port-specific code path can turn into a spurious HLT, and the gate's fix is the same.

MEASURED, re-run in round 2 with the raw transcript kept for every row — the C leg
(`build/v86c`, limit 2e9), files `results/m2-5-probe-c-<slug>-{serial,virtio,stderr}.txt`:

| cmdline | result | terminal eip |
| --- | --- | --- |
| `console=ttyS0` | FAIL — stopped at the idle HLT | `default_idle+0x12` |
| `console=ttyS0 nohlt` | FAIL — `nohlt` alone does NOT fix it | `default_idle+0x12` |
| `console=ttyS0 tsc=unstable` | PASS | `stop_this_cpu+0x3e` |
| `console=hvc0 console=ttyS0 nohlt` | FAIL | `default_idle+0x12` |
| `console=hvc0 console=ttyS0 idle=poll` | PASS | `stop_this_cpu+0x3e` |
| `console=hvc0 console=ttyS0 tsc=unstable` | PASS | `stop_this_cpu+0x3e` |
| `console=hvc0 console=ttyS0 tsc=unstable nohlt` | PASS | `stop_this_cpu+0x3e` |

and on the REFERENCE leg (the pinned JS v86, limit 8e8), files
`results/m2-5-probe-js-<slug>-*.txt`:

| cmdline | result | terminal eip |
| --- | --- | --- |
| `console=hvc0 console=ttyS0 tsc=unstable` | **FAIL** (stopped at the idle HLT, 479e6) | `default_idle+0x12` |
| `console=ttyS0 tsc=unstable` | **FAIL** (479e6) | `default_idle+0x12` |
| `console=hvc0 console=ttyS0 idle=poll` | PASS | `stop_this_cpu+0x3e` |
| `console=hvc0 console=ttyS0 tsc=unstable idle=poll` | PASS | `stop_this_cpu+0x3e` |

**S2 (round 2): the `tsc=unstable` per-leg asymmetry is now a CONTROLLED measurement.**
Round 1 compared the C leg's PASS row `console=ttyS0 tsc=unstable` against the reference
leg's FAIL row `console=hvc0 console=ttyS0 tsc=unstable` — DIFFERENT console sets, so the
two runs were not comparable (the `hvc0` console, which probes the virtio driver mid-boot,
was a plausible confound). Round 2 ran **both legs on BOTH cmdlines**:

| cmdline (same on both legs) | C leg | reference leg |
| --- | --- | --- |
| `console=ttyS0 tsc=unstable` | PASS (578,461,778) | FAIL (479,405,364) |
| `console=hvc0 console=ttyS0 tsc=unstable` | PASS (579,621,977) | FAIL (479,455,708) |
| `console=hvc0 console=ttyS0 idle=poll` | PASS (583,824,244) | PASS (581,961,102) |

The asymmetry survives the control, on both console sets: **`tsc=unstable` alone carries the
C leg and does not carry the reference leg**, and `idle=poll` alone carries BOTH legs.

so the gate's cmdline is `console=hvc0 console=ttyS0 tsc=unstable idle=poll`:
`tsc=unstable` is the semantic fix (stop using the emulated TSC as the clocksource) and
`idle=poll` is what the harness's HLT stop condition requires of a guest that must not idle
into a HLT with no pending interrupt. **`tsc=unstable` alone is not sufficient — the
reference leg proves it, on the SAME cmdline — and slice 3's gate cmdline (which has
`tsc=unstable` and no `idle=poll`) will stop mid-boot on the reference leg and needs
`idle=poll` added.** Its console/flag prefix is measured directly above: the reference leg
FAILS on `console=ttyS0 tsc=unstable`, which is exactly slice 3's console and clocksource
choice.

## 8. The gate's own pieces (new, and why each is shaped this way)

| file | what it is |
| --- | --- |
| `tests/init-v86gate.sh` | the guest init: the kernel's own libata/PCI/SCSI lines, `/dev/sda`+`/dev/sda1`, `dd \| md5sum` of the patterned disk, `/proc/filesystems`, `/dev/hvc0`, m2-4's host→guest read-back handshake, then a clean `halt -f` |
| `tests/mkinitramfs-v86gate.py` | the initramfs builder: **ZERO modules** — no `/lib/modules`, no `modules.dep`, no kmod (m2-3/m2-4's Alpine-module initramfs is deliberately not reused: its modules are built for the Alpine 6.6 kernel and this kernel needs none); the Alpine i386 busybox+musl userspace from `ALPINE_ROOT=/tmp/irf`, the tree those units used |
| `tests/images/guest-initramfs-v86gate.img` | 915,741 B, sha256 `d3a9e3ec…` |
| `tests/v86gate-virtio-in.txt` | the 15 bytes the host answers the sentinel with: `VIRTIO-INPUT16\n` |
| `tests/gate-m2-5.sh` | the gate: `pin` (re-verify every sha256/md5 before running) / `qemu` / `souche` (both legs) / `lean` (the failure arm) / `all` |

The init's waits are CHEAP and bounded: the `/dev/hvc0` wait is a 5000-iteration shell spin
with no subprocess (the device is created by the kernel's own driver before userspace runs
— MEASURED `waited 0 round(s)` on every passing run) and the read-back is `read -t`, so a
machine whose host end is a plain file still reaches `end` instead of blocking.

## 9. What is NOT in this slice (named, per the plan)

* **Slice 2** (fx-init's `/dev/sda` disk discovery in `zig/src/init.zig`) — untouched. The
  new kernel makes `/dev/sda` reachable, which is exactly what slice 2 will consume.
* **Slice 3** (the shared image on souche: fx-demo's real initrd.xz + a disk image, the
  `fx-init: disk store mounted` gate) — untouched; the kernel is its input. Note §7.
* **Slice 4** (the browser lane wiring in fx-demo's `web/index.html` / `harness/*`) —
  untouched.
* **fx-demo/kbuild/build-lean.sh and web/bzImage** — not edited; the lean kernel is the
  failure arm, by its shipped bytes.
* **No commit, no push, no branch** — the plan's rule, kept. `fx-demo` and `fx-init` have
  working trees with the new recipe/config uncommitted; souche is not a git repo, so its
  evidence is these files.
* **What this slice does NOT prove:** that a guest can MOUNT an ext4 filesystem from the
  disk — this gate proves libata reads the device and that `ext4` is registered in
  `/proc/filesystems`. That is the plan's own choice of oracle, not an omission: the plan's
  gate 1a names "m2-3's exact oracle: ata_piix lines, sda1, dd|md5", so the disk arm is the
  patterned disk + the pinned md5. A mounted `/` is slice 3's gate 3a ("fx-init: disk store
  mounted"); the brief's phrase "a disk, /, a pinned md5" is read here through the plan's
  parenthetical, and a mounted root is the one thing a reader might have expected instead.

## 10. Open items

1. **`idle=poll` for slice 3** (§7) — the plan's slice-3 cmdline needs it; without it the
   reference leg stops at an idle HLT mid-boot.
2. **Publishing the tarball** (§1) — the `url` line in `tests/baliveau-kernel-pin.txt` is a
   placeholder until the release asset exists; the tarball is staged at
   `tests/images/fixpoint-kernel-6.12.19-v86guest.tar.gz` with its hash recorded.
3. **The VGA PCI function divergence** (§5.1) — pre-existing, inherited from M2-4, and NOT
   this slice's code; it is worth a line in the port's own divergence list.
4. **The lean control's `halt -f`** — the failure arm's legs halt at `eip=0xc101399a` on both
   legs (the lean control is a DIFFERENT kernel, so unlike the new kernel's eip this one is
   not resolved here — no map for it). It reaches `GUEST-INIT: end` + `reboot: System halted`
   on both legs, so its failure is the absence of the device lines and nothing else. Whether
   it would ALSO idle into an HLT without `idle=poll` is NOT measured — it was only ever run
   with the gate's cmdline, which carries the flag.
5. **The determinism claim's toolchain scope** (§2.2) — `f0278a8f…` is measured for gcc
   14.2.0-19 / ld 2.44 out of the MOVING `debian:stable` tag; a later rebuild may differ and
   the pin file's bytes, not the recipe, are what downstream should trust.
