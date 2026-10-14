# M2-5 — SLICE 3: THE SHARED IMAGE (i386 kernel + initramfs carrying a busybox + a 64 MiB ATA disk)

Slice 3 is the artifact set both v86 lanes consume, and the native lane's gate
for it: **one kernel** (slice 1's `vmlinuz-i386-v86guest`), **one initramfs**
(`fx-demo/image/mkinitramfs.sh` + `build-store.sh`, now carrying a static i386
busybox at `/usr/bin/busybox`), and **one 64 MiB ATA disk image**, booted on
souche's own CLI (`build/v86c`) **and** on the pinned JS reference
(`oracle/run-boot.mjs`).

THE HEADLINE: **ARM B — the BLANK disk, the arm the in-guest mkfs exists for, and
the one slice 2 had to record as BLOCKED — IS GREEN.** The guest formats the disk
itself with the busybox built for it, mounts what it made, seeds the store onto
it, and reaches `fx-init: boot-ok v3`. The user's decision (source-build a static
i386 busybox with the `mkfs.ext2` applet) is what closed it: every prebuilt i386
busybox that can be pinned answers `mkfs.ext2: applet not found`.

The gate is `souche/tests/gate-m2-5-guest.sh`; its raw evidence is
`results/m2-5-guest-*.txt`.

---

## 1. The artifact set, and its pins

| artifact | sha256 | bytes | where it comes from |
|---|---|---|---|
| `tests/images/vmlinuz-i386-v86guest` | `f0278a8f…` | 1,253,888 | slice 1 (`fx-demo/kbuild/build-v86guest.sh`) |
| `tests/images/guest-initrd-fxdisk.xz` | `accc806d937260ed5abffafe0530efc701ebaf0383cb22cfc25b2680bf8e4d02` | 3,570,068 | `fx-demo/image/build-store.sh` + `image/mkinitramfs.sh -B` (§3) |
| `tests/images/busybox-i386-v86guest` | `fce38d5b7a492b86d719a77a07b189661132981dac1ef98c8435c087118586e0` | 2,307,404 | `fx-demo/kbuild/build-busybox.sh` (§2) |
| `tests/images/disk-fmt-64m.img` | `a2f0f6daac00c518727d7bf2981c2f6208dfe93e4973b06ffb2dc6c65ca0460c` | 67,108,864 | the pinned busybox's own `mkfs.ext2 -F` on 64 MiB of zeros |
| the blank disk (ARM B's input) | `3b6a07d0d404fab4e23b6d34bc6696a6a312dd92821332385e5af7c01c421351` | 67,108,864 | `truncate -s 64M` — reproducible by construction, so no artifact is shipped |
| `tests/images/vmlinuz-i386-lean-control` | `25be0d53…` | 795,136 | slice 1's control (fx-demo's shipped `web/bzImage`) |

The cmdline is one string, and its two flags are part of the machine, not taste
(slice 1, re-measured there on identical cmdlines):

```
console=hvc0 console=ttyS0 tsc=unstable idle=poll \
rdinit=/fx/store/54ce1971a82e89d78a83e3f805893b4e1651d714c62e69fda2c711ff8d3f2b69-fx-init/fx-init \
fx.store=/fx/store
```

`idle=poll` because both legs otherwise stop at an idle HLT mid-boot (the M0
stop condition); `tsc=unstable` because the emulated TSC is not kept in sync.
`console=hvc0` comes FIRST so that `ttyS0` stays `/dev/console` (the LAST
`console=` wins), which is where fx-init's own messages go — that is why the
`disk store mounted` line is on the serial stream and the kernel's log is on
both.

**The disk image is NOT byte-reproducible, and the gate says so rather than
pretending**: busybox's `mkfs.ext2` writes a random UUID and `time(NULL)`-derived
timestamps (MEASURED in `util-linux/mkfs_ext2.c`: `generate_uuid(sb->s_uuid)`,
`timestamp = time(NULL)`). So the *artifact* is pinned and the *recipe* (the
pinned busybox, `-F`, 64 MiB of zeros) is what is recorded. The blank disk needs
no artifact at all: `truncate` reproduces its bytes exactly.

---

## 2. The busybox: source-built, because nothing pinnable carries the applet

`fx-demo/kbuild/build-busybox.sh` (NEW). It downloads
`busybox-1.36.1.tar.bz2` and verifies sha256 `b8cc24c9…`, then builds inside
`debian:stable` with `gcc-multilib` + `libc6-dev-i386`:

```sh
make ARCH=i386 defconfig
# CONFIG_STATIC=y            (the image ships no dynamic loader)
# CONFIG_MKFS_EXT2=y         (the applet fx-init hardcodes)
make ARCH=i386 CC="gcc -m32" -j$(nproc)
```

**The applet is upstream's own config, not a patch.** busybox 1.36.1 provides
`mkfs.ext2` as an alias applet of its `mke2fs` implementation
(`util-linux/mkfs_ext2.c`: `config MKFS_EXT2 / bool "mkfs.ext2" / Alias to
"mke2fs"`, plus `APPLET_ODDNAME(mkfs.ext2, mkfs_ext2, …)`). It is `default y`
in `defconfig`, and a FATAL config gate asserts `STATIC/MKFS_EXT2/MKE2FS/RM/CP/
MV/SYNC` are `=y` after `oldconfig` — the lesson slice 1 paid for with
`CONFIG_BLOCK` is that a dropped symbol is silent.

Post-build, the recipe reads its own output back rather than trusting the flags:
`file(1)` → `ELF 32-bit … Intel i386 … statically linked`, `readelf -d` → **0
NEEDED**, `--list` carries all five applets, and — because an i386 static ELF
runs natively on this x86-64 host — **`mkfs.ext2 -F` is RUN** on a scratch image
with the ext2 magic read back.

**DETERMINISM: MEASURED, byte-identical.** Two COLD container builds, separate
trees, concurrent:

```
fce38d5b7a492b86d719a77a07b189661132981dac1ef98c8435c087118586e0  bba/busybox-i386
fce38d5b7a492b86d719a77a07b189661132981dac1ef98c8435c087118586e0  bbb/busybox-i386
```

SCOPE of that claim, stated plainly: it is measured between two builds in gcc
14.2.0-19 / ld 2.44 from `debian:stable`, a MOVING tag. The pin is over a recipe
*and its input tarball*; downstream should trust the pinned bytes.

### 2.1 TWO BUILD FAILURES THE RECIPE NOW CARRIES THE REASON FOR

1. **`defconfig`'s `tc` does not compile against any current kernel.**
   `networking/tc.c: … 'TC_CBQ_MAXPRIO' undeclared` — the CBQ scheduler the
   applet's descriptors come from was REMOVED from Linux. `TC` is turned off with
   that reason in the script; nothing on any boot path uses it.
2. **`yes "" | make oldconfig` exits 141 under `set -o pipefail`** (the writer
   gets SIGPIPE when make has read enough). The script now runs `oldconfig <
   /dev/null`. The same class bit the applet-presence loop: `busybox --list |
   grep -q` SIGPIPEs busybox the moment grep matches, and the first version
   "failed" on a **different applet in each run with two byte-identical
   binaries** — the list is captured once now.

---

## 3. The image: `mkinitramfs.sh` gains `-B`

`fx-demo/image/mkinitramfs.sh` had exactly the delta that made this slice
impossible, and its own header said why: **"NO busybox"** — the host's is
x86-64/glibc and cannot execute in the guest, and nothing on the *boot* path
needs a shell (the activated buildfile is `Mkdir/Copy/Chmod/Rm/Symlink`). The
premise was right and the conclusion was wrong: the boot path needs no shell, but
**fx-init's DISK path execs `/usr/bin/busybox`** (`run_bb`, init.zig:2221-2231)
for `mkfs.ext2`, `rm`, `cp`, `mv`, `sync`. So:

* `-B FILE` installs a static i386 busybox as `/usr/bin/busybox` (the ABI is
  asserted where it is *built*; the script only requires the file to exist);
* the "NO busybox" delta comment is **corrected in place**, in the script's
  header AND in `docs/BUILD.md` (§4 delta 2 + a new §2.5 for the recipe).

### 3.1 WHAT THE ARCHIVE CHECK PROVES, AND WHY A GREP WAS NOT ENOUGH

`sh tests/gate-m2-5-guest.sh pin` walks the cpio newc headers of BOTH concatenated
segments and identifies facts by ENTRY NAME and CONTENT:

```
PASS  exactly ONE fx-init store dir in the image
PASS  the store dir is the pinned one (/fx/store/54ce1971…-fx-init)
PASS  the image's /usr/bin/busybox is the pinned build (by content sha256)
PASS  the busybox entry's size is the built artifact's
PASS  /bin/init -> the store dir the cmdline names
```

**MEASURED, and the reason the walk exists**: `grep -a usr/bin/busybox` matches
an image built **WITHOUT** `-B` too, because fx-init's own binary embeds the
string `"/usr/bin/busybox"` (`run_bb`'s execv path). The count was 1 for the
image with no busybox in it and 2 with it — a grep-shaped check would have
"passed" a broken image.

**FALSIFICATION PROBE (run, not asserted):** the same gate against an image built
without `-B` → `3 FAILING assertion(s)`, including `got 'ABSENT' want
'fce38d5b…'`. The check fires.

### 3.2 Sizes, and the store-hash coupling

| image | bytes |
|---|---|
| `mkinitramfs.sh` without `-B` (this store, `-b fx-core`) | 2,514,712 |
| with `-B` (the shipped artifact) | 3,570,068 |
| the busybox's cost in the archive (its 2.3 MB, xz'd) | **+1,055,356** |

Given the same store the archive is **byte-reproducible** (a rebuild gave
`accc806d…` again — `--reproducible --renumber-inodes` + `SOURCE_DATE_EPOCH`,
the 5th v86 delta).

**The store dir hash is NOT stable across builds, and this round measured it
again**: two consecutive `build-store.sh` runs from the SAME, untouched tree gave
`54ce1971…` and `5da6d258…` for the `fx-init` package (slice 2 reported the same
class, and the shipped image's is `85adaec9…`). Consequences, and what the gate
does about them:

* the shipped `initrd.xz` is a **frozen pinned artifact**, not a per-run build;
* `rdinit=` is **parsed out of the archive** by the gate, never remembered, and
  the gate asserts the parsed path is the ONLY `fx-init` dir in the image and the
  target of `/bin/init`;
* a stale `rdinit=` fails loudly at the kernel (`rdinit not found`), so the
  coupling is a coordinated-rebuild chore, not a silent wrongness — but see
  §11 for the open item this really is.

---

## 4. GATE 3a, ARM A — a pre-formatted 64 MiB disk (C leg)

```
./build/v86c --bzimage tests/images/vmlinuz-i386-v86guest 4000000000 \
  --cmdline '<the cmdline in §1>' --hda <copy of disk-fmt-64m.img> \
  --initrd tests/images/guest-initrd-fxdisk.xz --bios build/minibios-80m.bin \
  --memory 80 --virtio-console results/m2-5-guest-armA-c-virtio.txt \
  --serial results/m2-5-guest-armA-c-serial.txt
```

4,000,019,080 instructions / 71 s wall, 231 serial lines, **37 PASS / 0 FAIL**.
The chain, with the line numbers in `results/m2-5-guest-armA-c-serial.txt`:

```
104: ata1.00: ATA-0: v86 ATA HD, 1.00, max MWDMA2        <- PIIX3 IDE, libata, /dev/sda
108: sd 0:0:0:0: [sda] 131072 512-byte logical blocks: (67.1 MB/64.0 MiB)
117: Run /fx/store/54ce1971…-fx-init/fx-init as init process   <- rdinit resolved
118: fx-init: store from kernel command line fx.store=/fx/store
120: EXT4-fs (sda): mounted filesystem bca8c481-… r/w without journal
225: fx-init: disk store mounted (current v3)              <- READ BACK OFF THE DISK
228: fx-init: warning: virtio control: open /dev/vport0p3: No such device or address
231: fx> fx-init: boot-ok v3
```

`EXT4-fs (sda): mounted filesystem … without journal` is the MOUNT of the
pre-formatted disk (no mkfs ran — the gate asserts `mkfs.ext2 FAILED` is ABSENT
and that `current v0` never appears, i.e. the disk was not silently re-seeded).
The `v3` in the last two lines is read from `/fx/disk/store/.db/snapshots/CURRENT`
**after** the seed copy wrote it — the write→read-back proof of §8.

The same lines arrive through the second console: the virtio stream carries
`sd 0:0:0:0: [sda]` and the `EXT4-fs (sda): mounted` line. (It does NOT carry
the string `hvc0`: `legacy console [hvc0] enabled` is printed as hvc0 *is*
registered, so it precedes this stream's first byte — MEASURED, and the gate
asserts the lines that are really there instead.)

---

## 5. GATE 3a, ARM B — a BLANK disk: THE GUEST FORMATS IT (this is the arm)

Input: 64 MiB of zeros (`3b6a07d0…`, asserted). 4,200,098,792 instructions /
74 s, 245 serial lines, **33 PASS / 0 FAIL**. The sequence, in order:

```
EXT4-fs (sda): VFS: Can't find ext4 filesystem      <- the blank disk refused the first mount
Filesystem label=                                  <- the GUEST's busybox mkfs.ext2, its own report
OS type: Linux
Block size=1024 (log=0)
16384 inodes, 65536 blocks
8 block groups
Superblock backups stored on blocks:
	8193, 24577, 40961, 57345
EXT4-fs (sda): mounted filesystem e1196236-… r/w without journal
ext4 filesystem being mounted at /fx/disk supports timestamps until 2038-01-19
   (… ~103 "cp: can't preserve times" lines: the seed copy of the store onto the new fs — busybox's
      cp -a cannot set times in this guest, harmless, and the same noise the x86_64 lane has …)
fx-init: disk store mounted (current v3)
fx> fx-init: boot-ok v3
```

This is the strongest form of the disk claim available in this harness: the
filesystem that gets mounted **did not exist before the run** (its input is
zeros), the guest created it with the busybox this slice built **for it**, the
kernel read the superblock back through the emulated ATA device, and the store
then came up on it and booted to the verdict. Slice 2's recorded gap
(`fx-init: disk: mkfs.ext2 FAILED (exit 127) — disk store disabled`) is closed.

The guest's mkfs produced the **same geometry** the host-side format did
(`16384 inodes, 65536 blocks`, 1024-byte blocks, the same superblock-backup
blocks) — the two are the same applet, on the same 64 MiB device.

### 5.1 THE INSTRUCTION LIMIT IS A uint32 ON *BOTH* MACHINES, AND ABOVE 2**32-1 IT IS SILENTLY UNENFORCEABLE

Found by trying to be generous, and worth recording because it caps ARM B — and
because it cost an hour before it was diagnosed:

* The C port **rejects the text**: `v86c: instruction limit must be a positive
  integer, got: 6000000000` (the limit is a `uint32_t`, `src/main.c`
  `parse_positive_uint`), and at exactly `4294967295` it **does not stop either**
  (MEASURED: 600 s run, no stop line, killed by the timeout).
* The reference (v86) **accepts and ignores it**: MEASURED, a 6e9 run booted
  normally — its live console (stderr, `V86C_BOOT_TRACE=1`) reaches
  `fx> fx-init: boot-ok v3` — and was **still running at 1 h 46 m**. v86 stores
  the count in a `Uint32Array`, so `count >= instruction_limit` is unreachable
  above 2**32-1. That run produced an EMPTY serial transcript, because
  `run-boot.mjs` writes the serial to stdout only at the end: **an over-limit run
  looks exactly like a hung one.**

So every limit in this gate is below 2\*\*32-1, and the two arms' measured costs sit
inside the ceiling: ARM A reaches `boot-ok` within 2.9e9 (bisected: the boot is at
`fx>` by 2.5e9 and `boot-ok` before 3.0e9), ARM B within 4.200e9 (in-guest mkfs +
the same seed + the same boot). The plan's "budget the instruction limit
generously, expect billions" is satisfiable — but only just, and neither leg can
be told to run longer than 2\*\*32-1 instructions at all.

---

## 6. THE CONTROL: the pinned lean kernel, same image, same disk

The asymmetry the plan names, and it needs no reconstruction: fx-demo's shipped
`web/bzImage` (sha256 `25be0d53…`). Same machine, same `initrd.xz`, same
pre-formatted disk, **28 PASS / 0 FAIL**:

```
PASS  no libata (no ATA driver in this kernel)
PASS  no ATA disk
PASS  no ext4 mount over an ATA disk
PASS  THE PINNED PRE-FIX LINE (byte-for-byte the browser lane's)
PASS  and the same image still boots (the negative is a measurement)
PASS  no disk store on the control kernel
PASS  the same initrd's fx-init ran (the control is not silence)
```

The line it prints is the one the browser lane prints today, quoted in
`fx-demo/docs/BOOT.md` and in the shipped image's own `evidence/proof-serial.txt`:

```
fx-init: disk store: no /dev/vda — using ramfs store
```

and the same image still reaches `fx-init: boot-ok v…` there — so the absence is
a measurement (no disk path ran), not a silence.

**THE PLAN EXPECTED "the dhake materialization lines" IN THE TRANSCRIPT; THERE
ARE NONE, and that is MEASURED on both the shipped image's own evidence and this
boot.** The chain from the kernel's `Run … as init process` to `fx-init: boot-ok`
is 13 lines with no `dhake` line in it (dhake runs silently; the only `dhake`
string in the shipped transcript is inside an `ls /bin` listing). The
materialization is evidenced instead by `pivoted to tmpfs root`, the `fx>`
console prompt, and `boot-ok` — which requires every service of the generation to
have started.

---

## 7. GATE 3b — the two legs, normalized

`sh tests/gate-m2-5-guest.sh diff`. Both legs: the same kernel, the same
`initrd.xz`, the same 80 MiB machine + `minibios-80m.bin`, the same disk bytes,
the same cmdline, the same 4e9 instruction limit. MEASURED wall time: **C leg
71 s, reference leg 87 s** (4,000,019,080 / 4,000,005,139 instructions,
238 serial lines each — the transcripts are the same length).

Normalization is m2-2's (timestamps stripped), plus ONE more mask that this
image makes necessary: the ext4 mount line carries a **random UUID**
(`mkfs.ext2` generates it), so the UUID is replaced with `<uuid>` — it is a
per-format value on one image, not a leg difference.

**The full diff is 13 lines and it classifies completely:**

```
74,77d73   pci 0000:00:12.0: [1234:1111] type 00 class 0x030000 …     <- v86's VGA
87,89d82   pci 0000:00:12.0: vgaarb: …                                <- device, which
                                                                         the port's
                                                                         machine has not got
118d111    ata2.00: ATAPI: v86 ATAPI CD-ROM, 1.00, max MWDMA2         <- ONE line, emitted
113a107                                                                  in a different ORDER
```

i.e. m2-3's named VGA divergence class, plus a single printk pair whose order
differs at bring-up. **No device, disk, store or verdict line is missing on
either leg** — which is the claim gate 3b makes, and it is asserted over the
MULTISET of those lines (sorted, with counts), so reordering cannot mask a
missing line:

```
PASS  the device/store lines are the same MULTISET on both legs (19 lines)
```

The 19 lines are (C leg; the reference's are the same multiset):

```
ata1: PATA max MWDMA2 cmd 0x1f0 ctl 0x3f6 bmdma 0xb400 irq 14 lpm-pol 0
ata2: PATA max MWDMA2 cmd 0x170 ctl 0x376 bmdma 0xb408 irq 15 lpm-pol 0
ata1.00: ATA-0: v86 ATA HD, 1.00, max MWDMA2
ata1.00: 131072 sectors, multi 0: LBA48
scsi 0:0:0:0: Direct-Access ATA v86 ATA HD n/a PQ: 0 ANSI: 5
ata2.00: ATAPI: v86 ATAPI CD-ROM, 1.00, max MWDMA2
sd 0:0:0:0: [sda] 131072 512-byte logical blocks: (67.1 MB/64.0 MiB)
sd 0:0:0:0: [sda] Write Protect is off
sd 0:0:0:0: [sda] Write cache: disabled, read cache: enabled, doesn't support DPO or FUA
sd 0:0:0:0: [sda] Preferred minimum I/O size 512 bytes
scsi 1:0:0:0: CD-ROM SONY CD-ROM CDU-1000 1.1a PQ: 0 ANSI: 1 CCS
sd 0:0:0:0: [sda] Attached SCSI disk
Run /fx/store/54ce1971…-fx-init/fx-init as init process
fx-init: store from kernel command line fx.store=/fx/store
fx-init: boot start store /fx/store
EXT4-fs (sda): mounted filesystem <uuid> r/w without journal. Quota mode: disabled.
fx-init: disk store mounted (current v3)
fx-init: pivoted to tmpfs root (magic 0x1021994)
fx> fx-init: boot-ok v3
```

**0 FAILING assertions.** Both legs also pass the full 16-assertion ARM A set
(`SHOULD the same lines be seen through the second console`: the virtio stream
carries `sd 0:0:0:0: [sda]` and the `EXT4-fs (sda): mounted` line on both).

---

## 8. WHAT PROVES A WRITE, AND WHAT CANNOT BE PROVEN HERE (MEASURED, and it changes slice 4)

**Neither leg flushes the disk buffer back to the host file.** The C port reads
the image with `read_file()` into memory (`src/main.c:903`, handed to the IDE
device as a buffer), and the reference passes `hda: {buffer: hda_ab, async:
false}` (`oracle/run-boot.mjs`; v86 `cpu.js:1221-1224` builds
`ide_config[0][0] = {buffer: settings.hda}`). MEASURED: after the ARM A run — in
which the guest mounted the disk, seeded ~30 MB of store into it and synced —
`disk-fmt-64m.img` is **byte-identical** (sha256 `a2f0f6da…` before and after).
The gate RECORDS this per arm and asserts nothing about it: a later port adding
write-back must not fail this gate.

What is assertable, and is asserted, is write→read-back **within a run**:

* `fx-init: disk store mounted (current vN)` is printed by
  `store_current_of(DISK_STORE)`, which opens `<disk>/store/.db/snapshots/CURRENT`
  **after** the seed copy wrote it through the emulated ATA device — the version
  printed came out of the disk;
* ARM B is the strong form: the filesystem it reads back is one the guest created
  from zeros in the same run.

**What is NOT claimed**: persistence across boots (impossible in this harness —
each leg starts from the file's bytes, and the file is never written) and
persistence across a browser page reload. **For slice 4 this is a design
constraint, not a footnote**: the plan's slice-4 assertion ("a command creates a
store-visible file, reload/reboot, assert it survives") cannot be met by
reloading the page — a reload re-fetches `disk.img` and starts from the shipped
bytes again. The browser harness must keep the SAME buffer and restart the
emulator in place (or persist the buffer itself).

---

## 9. The gate's own pieces

`sh tests/gate-m2-5-guest.sh [pin|c|js|armA|armB|lean|diff|all]`

* `pin` — every input's sha256, the archive probe (§3.1), the busybox run
  natively, and the disks' provenance. No boot; seconds.
* `c` / `js` — ARM A's two legs separately. MEASURED: 71 s and 87 s at a 4e9
  limit. (An earlier 6e9 limit never finished at all — see §5.1, which is where
  the `V86C_BOOT_TRACE` handle comes from.)
* `armB` — the blank-disk arm; `lean` — the control kernel.
* `diff` — gate 3b.
* **Every disk handed to an emulator is a COPY under `$WORK`**: a run cannot
  mutate `tests/images/`, and both legs of ARM A start from the same bytes.
* **The gate never runs `make`.** `V86C=` pins a frozen binary; the binary's
  sha256 is printed in the evidence.
* Every input path is env-overridable ONLY so a pin can be falsified against a
  different artifact (§3.1's probe).

**THE RUN RECORDED HERE, and the one to re-run:**
`sh tests/gate-m2-5-guest.sh all` with the tree's own defaults (`build/v86c`,
`build/minibios-80m.bin`) → **74 PASS / 0 FAIL, exit 0, 4 m 31 s**, against
`build/v86c` sha256
`639127b2771ca6ace540d602248d1a8e1dd33c77475c72b9dc022c59979bb36b` (the sha of
whatever binary the gate ran is always printed by `pin`). The per-arm numbers in
§4/§5/§7 come from the same gate; two runs differ in the instruction COUNT by a
few thousand (the guest reads the host clock: ARM A's C leg gave 4,000,019,080
then 4,000,029,445; ARM B 4,200,098,792 then 4,200,096,510) while the terminal
state is identical (`eip=0xc11de760`, the kernel's own idle path — slice 1's
finding, unchanged).

**CONCURRENCY, stated because it happened**: another live agent lane
(`HANDOFF=v86c-perf`) was editing `src/` and rebuilding `build/v86c` in this same
directory while this slice ran (it also removed `build/minibios-80m.bin` under a
`make`, which the gate now rebuilds via `make minibios-mem`). The evidence here
was produced with a **frozen byte-identical copy** of `build/v86c`
(sha256 `dc591a051b92f7a464ac8aaf71fb9d7eb1cac029a2cd3eaa48bcd1c9df0ce372`, equal
to `build/v86c` at freeze time), so the C-leg evidence names exactly one binary.
The gate defaults to `build/v86c` and prints its sha.

---

## 10. What is NOT in this slice (named, per the plan)

* **The browser wiring** (slice 4): `web/index.html`'s `hda:`, `harness/boot.mjs`
  + `drive.mjs`, `harness/assert.mjs`. Nothing in `fx-demo/web/` was touched: the
  shipped `bzImage`/`initrd.xz`/seabios and `SHA256SUMS` are exactly as they
  were, so the browser lane is unchanged and still prints the ramfs line.
* **The web asset swap** (`web/initrd.xz` ← this image, `SHA256SUMS` re-pin):
  slice 4's publish step. This slice made the *builder* capable of it (`-B`) and
  pinned the image in the native lane.
* **Persistence across boots or page reloads** (see §8 — not claimable here).
* **The `hvc0` control-channel seam**: fx-init scans `/dev/vport*p*`
  (`init.zig:3845-3862`) and v86's virtio console yields `/dev/hvc0`; the boot
  transcript says so (`virtio control: open /dev/vport0p3: No such device or
  address`). Named, not opened.
* **The `fx-agent`/agent-loop over a chardev**, and the fifo control port's
  in-guest protocol beyond what the boot needs.
* **`src/`**: no port change. The uint32 limit finding (§5.1) is reported, not
  fixed.

---

## 11. Open items

1. **The store dir hash moves between builds of an unchanged tree** (§3.2): two
   consecutive `build-store.sh` runs disagreed (`54ce1971…` vs `5da6d258…`), and
   this is now the third such measurement (slice 2 saw it three times). Every
   consumer pins `rdinit=` by that hash, so the instability is the reason the
   image must be a frozen artifact and the cmdline regenerated per image. The
   *cause* is not this slice's to find; the persistence of the phenomenon is
   worth a unit of someone's attention.
2. **The instruction limit is silently unenforceable above 2\*\*32-1 on BOTH
   machines** (§5.1): the port rejects such a limit as text, v86 accepts it and
   never stops, and a never-stopping reference run leaves an EMPTY transcript
   (the serial is written at the end). Both are harness-side; the port's half is
   `src/`-side and belongs to whoever owns the port — it is NOT touched here. The
   gate now documents the ceiling and never uses it.
3. **Publication owed**: slice 1's kernel tarball release asset (fx-demo repo
   write access) and now the busybox build — the recipe + pin is committed to the
   working trees, but nothing is published. Neither this slice nor slice 1 was
   allowed to push.
4. **The disk image is not byte-reproducible** (§1) because busybox's `mkfs.ext2`
   writes a random UUID and a wall-clock timestamp. If a reproducible ARM A input
   is ever needed, the UUID/timestamp fields would have to be normalized after
   formatting (not done: it is not needed by this gate, which pins the artifact).
5. **Slice 4's persistence assertion must be written against the in-memory buffer**
   (§8), not against a page reload.

---

## 12. APPENDED CORRECTION (2026-10-14, the nobb follow-ups): the image's contract changed under this gate

Everything §§1-11 above describes the BUSYBOX image and ARM B's IN-GUEST MKFS.
Both are superseded by the user's decision in unit nobb (fx-demo
`evidence/no-terminal-2026-10-13/`, baliveau `docs/baliveau-self-plan.md` §2):
the image's busybox carried `sh`, `ash` AND `hush` (MEASURED `--list`), which
made the project's own containment claim false, so the busybox AND the 63-binary
fx-core payload are OUT, fx-init's disk path is NATIVE, formatting is a BUILD
condition, and a BLANK disk now gets a LOUD REFUSAL.  The fxsh CONSOLE is
deferred the same way: it returns by installing fxsh **THROUGH THE SYSTEM DHALL
CONFIG as a package** (provides/src_of, oracle-attributable) — not by re-adding
a host-bindir blob.

**THE GATE WAS RE-PINNED, not left frozen.**  An arm testing a superseded
artifact is a finding, not a pass: `gate-m2-5-guest.sh` now boots
`tests/images/guest-initrd-shellfree.xz` (sha256
`df1da2e31ff9fdd66b2d97af68fbd35cc16999e0ee17d53759126d4cf7178665`, 829,216 B,
copied from fx-demo's rebuild).  The OLD image is FROZEN, pinned as SUPERSEDED
(`guest-initrd-fxdisk.xz`, `accc806d…`), because souche is not a git repo and
those bytes are the old contract's only copy; the pinned busybox FILE and its
applet checks stay as provenance of what the old image carried.  The archive
re-pin check FLIPPED with the contract: the image must now carry NO busybox
entry, NO fxsh, and NO usr/fx-core payload (the probe walks the cpio headers,
so a grep of fx-init's embedded strings cannot fool it).

**ARM B kept its structure and its asymmetry; what it asserts changed.**  It
used to prove the guest formatting zeros into a filesystem — the capability the
removal deleted — so asserting it would have kept proving a removed thing.  It
now asserts the NEW contract, mirroring fx-init's own G5 arm
(`fx-init/tests/qemu_disk_probe.sh`): the blank disk rejects the mount
(`Can't find ext4 filesystem`), fx-init REFUSES BY NAME (`is not a formatted
filesystem` / `must be pre-formatted at build time`), NOTHING is formatted
(`mkfs` absent, the old `inodes, 65536 blocks` headline now its own negative),
the device is REFUSED not missed (no silent `no /dev/vda` fallback), and THE
BOOT VERDICT still lands — `fx-init: boot-ok v3` from the ramfs store
(`ensure_disk_store` is non-fatal by design, init.zig:2453).  MEASURED on the
C leg: all 12 rows PASS.

**The console row went CONDITIONAL, not silent.**  ARM A's `fx> ` row asserts
exactly as before when the image carries fxsh; on the shell-free image it
prints a NAMED skip (the deferred item, verbatim) and does not count as a
failure.  The skip is decided by the same archive probe the pins use, so
restoring fxsh re-arms the row with no edit.

**RE-MEASURED, this day: `sh tests/gate-m2-5-guest.sh all` -> 0 FAILING
assertion(s), exit 0** — pin (both image pins), ARM A on BOTH legs (C port and
the pinned JS reference: mount, native seed, v3 read back from the disk, pivot,
boot-ok, no fallback, no re-seed), ARM B (the refusal, 12/12), LEAN (the pinned
ramfs line, same image), and gate 3b (the device/store lines the same MULTISET
on both legs; the ordered diff is again only printk interleaving, one
`ata2.00` line's position).  Raw evidence: `results/m2-5-guest-*` (this day's
run overwrote the per-arm transcripts; the busybox-era numbers live in §§3-9
and in the frozen image).
