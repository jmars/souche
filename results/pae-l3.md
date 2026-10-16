# PAE-L3 — a real CONFIG_X86_PAE=y kernel boots on both legs, and the gate

Handoff: `handoff-v86c-pae-l3-ctx` (the diagnosis brief) + `handoff-v86c-pae-plan`'s Q4
(Level 3 was defined there as "a CONFIG_X86_PAE=y kernel boot — the independent-consumer
evidence class; it proves ecosystem compatibility, not walk semantics").  Every claim tagged
MEASURED / READ / INTERPRETATION.

## TL;DR — the diagnosis the brief was looking for

The brief's blocker was **wrong**: the Debian 686-pae kernel does NOT hang, and the machine
is NOT missing anything.  **The 2G-instruction limit of the earlier measurement was too low.**
The kernel is XZ-compressed (CONFIG_KERNEL_XZ=y), and its decompression alone is **~2.34G
instructions** (MEASURED, both legs, the trace pins the decompressor→kernel transition at
instruction 2,343,783,531).  At 2G the run is still inside the LZMA2 decoder (eip 0x2117585,
cr0=0x60000011, cr3=0 — paging not yet on), which is exactly the state the brief reported as
"the hang".  It was the limit, not the machine.

With a 3G limit, **both legs reach a live 3-level walk**: the kernel prints
`NX (Execute Disable) protection: active` (a string that exists only in the
CONFIG_X86_PAE=y branch of arch/x86/kernel/setup.c; the non-PAE branch prints its inverse
"cannot be enabled: non-PAE kernel!", which the tree's Alpine kernel prints on this same
machine), and the final state is PG set (cr0=0x80050033) with cr3=0x1e98000.  That is the
decisive PAE evidence the brief asked to gate on.

## The vehicle (provenance, MEASURED)

- `.deb`: `linux-image-6.1.0-50-686-pae-unsigned_6.1.176-1_i386.deb`, 52,143,184 B,
  `https://deb.debian.org/debian/pool/main/l/linux/linux-image-6.1.0-50-686-pae-unsigned_6.1.176-1_i386.deb`,
  sha256 `db3fd09f60b9af71d41a276c3f6363c4002922a889d2900f1a232566339cc05c`.
- `ar x` + `tar xJf data.tar.xz ./boot/vmlinuz-6.1.0-50-686-pae` → the bzImage, 5,759,488 B,
  sha256 `c2ac6a2a88224f41e3006ff98a5e4f8a7af796806dc21f4afbaf83b99606925f` — the pinned
  `tests/images/vmlinuz-6.1.0-50-686-pae`.
- Its own config (same .deb): `CONFIG_X86_32=y`, `CONFIG_X86_PAE=y`,
  `CONFIG_PGTABLE_LEVELS=3`, `CONFIG_SMP=y`, `CONFIG_KERNEL_XZ=y`,
  `CONFIG_PHYSICAL_START=0x1000000`, `CONFIG_RELOCATABLE=y`, `CONFIG_RANDOMIZE_BASE=y`.

**Pinning decision (INTERPRETATION): commit the 5.76 MB vmlinuz, not a fetch script.**  The
tree already tracks a 7.7 MB kernel (`tests/images/vmlinuz-i386`) and its other guests, and
5.76 MB is below the weight threshold that moved the 64 MiB disk OUT of git.  A fetch script
would add a network dependency to a gate whose whole point is determinism; the committed
bytes are the pin.  The provenance above is recorded so the image can be re-derived.

## Write scope

- `tests/images/vmlinuz-6.1.0-50-686-pae` (the pinned kernel)
- `tests/mk-initramfs-pae.sh` + `tests/images/guest-initramfs-pae.img` (a userspace init that
  prints `/proc/cpuinfo`'s flags and powers off)
- `tests/gate-pae-l3.sh` (the gate)
- `.gitignore` (the gate's stderr: see the JIT note below)
- `TODO.md` §1.3, this report
- NOT: `src/` (no port change was needed), `v86/` (READ-ONLY), other `results/*`, no commit.

## The measurements

### 1. The "hang" was the limit (MEASURED, both legs)

`V86C_BOOT_TRACE=2` on the reference, limit 4G, `console=ttyS0 nokaslr` (the brief's own
repro), samples every 10M instructions: the eip stays in the relocated decompressor
(0x2116f00–0x21190ee, the LZMA2 decoder) from ~10M straight through to `@2333782504
eip=0x21190ee`, and the NEXT sample `@2343783531 eip=0xc1933e20 cs=0x60 cr0=0x80050033
cr3=0x1e98000` is the kernel with paging on.  The decompressor→kernel transition is at
2.34G.  At the brief's 2G limit the serial was still 30 bytes (only the firmware's own
line) because the kernel had not finished unpacking itself.  The C leg is the same: its
trace is identical at every sampled point.  (The `nokaslr`/`no-nokaslr` flag makes no
difference to this: the no-nokaslr trace shows the same 2.34G transition.)

### 2. PAE is live (MEASURED, both legs, the gate's assertions)

Both legs, limit 3G, `console=ttyS0`, the pinned kernel + the PAE initramfs:

| assertion | C leg | reference leg |
| --- | --- | --- |
| `NX (Execute Disable) protection: active` in the serial | PASS | PASS |
| `non-PAE kernel!` ABSENT (the inverse the Alpine kernel prints) | PASS | PASS |
| `BIOS-e820: [mem 0x0000000000100000-0x0000000004ffffff] usable` | PASS | PASS |
| PG set (cr0 bit 31) | PASS (`cr0=0x80050033`) | PASS (`cr0=0x80050033`) |
| cr3 non-zero (the 3-level walk in use) | PASS (`cr3=0x1e98000`) | PASS (`cr3=0x1e98000`) |

`sh tests/gate-pae-l3.sh c` and `.sh js` each report `0 FAILING assertion(s)` (exit 0).  The
two legs' serials are the same kernel lines; the only divergences are the tree's documented
ones (the v86 VGA model and the clock, results/m2-3).

### 3. Reaching userspace is expensive, not blocked (MEASURED, reference)

The reference reaches userspace — `PAE-INIT: begin`, `/proc/cpuinfo` flags
`… pae … nx …`, `PAE-INIT: cpuinfo advertises pae/nx`, `PAE-INIT: end`, then the init
`poweroff -f`s and the run ends `in_hlt=1`.  That run's counter printed 2,153,004,427, which
is **2^32 + 2.15G wrapped** (the reference's `instruction_counter` is a u32; its limit was
6.5G) — the full boot to /init is ~6.45G instructions, not 2.15G.

The cost is a **~64 s guest-time wait** between the last initcall line
(`evm: HMAC attrs: 0x1`, the end of `kernel_init_freeable`) and
`Freeing unused kernel image (initmem) memory` — `async_synchronize_full()` waiting for a
pending async device probe.  Both legs sit in that wait (their eips are in the same
interrupt/loop mix: `0xc1931640` is the ISA-IRQ-0 = timer entry, `0xc14bf881` the wait
loop), interrupted by the timer.  It is the SAME wait on both legs, so it is a machine
property, not a port divergence.  **The C port cannot reach userspace in one gate run only
because its instruction limit is a u32 (4,294,967,295 ceiling)** — the evidence above is
printed long before that ceiling, so the gate does not need to.

## What the gate does NOT claim (the honest seat)

The gate asserts the kernel's own PAE evidence (NX active, cr3 non-zero, PG set) at 3G.  It
does NOT assert `/proc/cpuinfo`'s `pae` flag, because that needs userspace and userspace
needs ~6.45G (past the C port's u32 limit, past the 64 s async wait).  The reference DID
reach userspace and print that flag (above), so it is measured, not claimed.  A follow-up
that lifts the C port's instruction counter to u64 would let this gate assert the
initramfs's `PAE-INIT` lines directly; that is a small, separate unit.

## Side observations (recorded, not in the gate's scope)

- **JIT block table fills during a full 6.1 boot (READ + MEASURED).**  The shipped default
  has the JIT on (`jit_mem.c:42 bool jit_enabled = true`; the "false unless --jit on" note in
  `jit_mem.h:11` is STALE — the value has been `true` since the JIT landed, a49a887), and
  the PAE kernel's code surface fills the 2048-entry block table; every later bind prints
  `jit: bind: the block table is full (2048)` — 107,412 lines / 4.5 MB in one 3G run.  The
  JIT degrades gracefully (the fallback is interpretation), so no PAE assertion depends on
  it, but the spam is why the gate's stderr is gitignored.  The Alpine 6.6 boot does not
  fill the table (its m2-5 stderr is 659 B).  Worth a JIT-lane look (grow the table, or
  print the refusal once).
- **The kernel's KASLR prints `Physical KASLR disabled: no suitable memory region!`** (READ,
  arch/x86/boot/compressed/kaslr.c) on BOTH legs, deterministically — the 80 MiB map's one
  RAM slot above the 16 MiB minimum and the initrd carve-out leave no 18 MiB window the
  avoid-list accepts.  It is cosmetic here: the kernel then decompresses to its fixed
  `pref_address` 0x1000000 and boots; it is not the stall.

## Round 2 — the initramfs is build-stable (the orchestrator's finding, closed)

The first version of `tests/mk-initramfs-pae.sh` was NOT reproducible, and the committed
image was not its output.  The orchestrator measured it (read the archives with a newc-header
parser): GNU cpio 2.15's `--reproducible` zeroes the ino/dev/uid/gid fields but **NOT the
mtime**, so the extraction-time mtime (`cpio -idm`) leaked into every build — the 39
differing bytes between two builds were exactly the mtime fields, and my earlier two matching
shas were a one-second coincidence (mtime has 1-second granularity and the script runs fast).

**The fix (MEASURED):** `find "$TMP" -exec touch -h -d @0 {} +` zeroes every entry's mtime
before archiving (kept alongside `cpio --reproducible` and `gzip -9n`).  `-h` is load-bearing:
the tree carries a dangling symlink (`sbin_modprobe -> ../bin/kmod`) and plain `touch`
dereferences it and fails.

**The asymmetry (both arms, MEASURED):**

| arm | builds | result |
| --- | --- | --- |
| PRE-FIX (no mtime zeroing) | two builds, 3 s apart | DIFFERENT sha256 — `f17c3147…` then `351b2347…` (orchestrator); `46800fcf…` then `ba6cfd37…` (mine) |
| POST-FIX (mtime zeroed) | three builds, 4–5 s apart | SAME sha256 — `2a4a29dc8dd900258135ee1543226f7d629dacaf560b95128951ae10e21e35a6` each time |

The gate's `SHA_INITRD` pin is now `2a4a29dc…`, and the pin arm passes both against the
committed image and against a fresh rebuild (`sh tests/mk-initramfs-pae.sh` then
`sh tests/gate-pae-l3.sh pin` → `0 FAILING assertion(s)`).  Both legs were re-run against the
rebuilt image and produce the SAME PAE evidence (NX active, PG set, cr3 non-zero).

## Gates run

| gate | result |
| --- | --- |
| `sh tests/gate-pae-l3.sh c` | **rc 0** — `### pae-l3 gate: 0 FAILING assertion(s)` (C leg, ~35 s) |
| `sh tests/gate-pae-l3.sh js` | **rc 0** — same (reference leg, ~90 s) |
| `sh tests/gate-pae-l3.sh` (pin) | **rc 0** — kernel + initramfs sha256 pins match, against the committed image AND a fresh rebuild |
| `make` | unchanged (no `src/` edit); the binary's sha256 was verified unchanged before and after the rebuild |
