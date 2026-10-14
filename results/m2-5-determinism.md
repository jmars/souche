# M2-5-DET — §7.3 determinism, the NATIVE lane (HANDOFF=v86det)

**THE CLAIM, MEASURED:** the same image and the same cmdline booted twice on souche's own
CLI produce **byte-identical serial and virtio transcripts** *and* execute **the same number
of guest instructions**.  The gate is `tests/gate-m2-5-determinism.sh`; its inputs are
slice 3's own pinned artifacts and cmdline, re-verified by `tests/gate-m2-5-guest.sh pin`
before anything boots.

**THE ASYMMETRY (this is the whole result, and it is not the one the brief predicted):**

| | serial (`sha256`) | virtio (`sha256`) | instructions executed |
| --- | --- | --- | --- |
| **PRE-FIX** (frozen build `73a28df1…`, two boots) | identical `29c332ff…` | identical `3b9d566c…` | **4000057327 vs 4000057221 — DIFFER** |
| **POST-FIX** (frozen build `52c9fd01…`, two boots) | identical `29c332ff…` | identical `3b9d566c…` | **4000053925 vs 4000053925 — IDENTICAL** |

The PRE-FIX pair above was re-run a second time to leave the raw files under `results/`
(§9): `4000068327` vs `4000019789` — the transcripts byte-identical again
(`29c332ff…` / `3b9d566c…`), the counts apart again.  **Six** pre-fix boots in all
(`4000032959`, `4000060383`, `4000057327`, `4000057221`, `4000068327`, `4000019789`): six
different counts, one transcript.  Post-fix, **eight** boots (three of them from
independently built binaries of the final source, one from an earlier build) all read
`4000053925`.

The brief expected the pre-fix tree to show a **transcript** diff.  MEASURED, it does not:
the guest's printed transcript was *already* stable (it is stable across binaries and hours —
see §5).  The residual host entropy was real but **invisible to the transcript**: it changed
the guest's *instruction stream*, not its output.  Closing it required a second instrument
(the instruction count the port itself prints), which is the diff the gate now catches.  A
transcript-only gate declares the pre-fix tree reproducible.

## 1. What was wrong: the two host-entropy sites, both in the port's own code

### (1) `src/hw/rtc.c` — the RTC read the host clock (`Date.now()` upstream, `clock_gettime(CLOCK_REALTIME)` here)

* **Consumers (all of them):** `rtc_init` (`rtc->rtc_time`/`last_update`), `rtc_timer`
  (the `time` upstream overwrites the argument with — this is the *interrupt scheduling*
  clock: `next_interrupt`, `next_interrupt_alarm`, `update_interrupt_time`), and
  `rtc_cmos_port_write`'s register-B arm (bits 6/5/4).
* **Guest-visible in two ways.** (a) Registers 0x00-0x09 and 0x32 return `new
  Date(rtc_time)`'s UTC fields, i.e. the HOST's current wall clock.  (b) An armed RTC
  interrupt (IRQ8) fires when the HOST clock passes the deadline, so the interrupt is
  delivered at a **wall-clock-dependent instruction** of the guest's stream.
* **The fix:** `rtc_now_ms() = RTC_EPOCH_MS + microtick()` — M2-1F's D-F2 virtual clock
  (src/cpu/exec_boundary.c: 1 guest instruction = 1 microsecond, `microtick()` in
  MILLISECONDS), the same unit the PIT and the PM timer already consume, on a **fixed**
  epoch `RTC_EPOCH_MS = 1704067200000.0` (2024-01-01T00:00:00Z).
  *Why a fixed epoch rather than 0:* the value is decoded straight into the CMOS registers,
  so an epoch of 0 would report **1970-01-01** to the guest — the very edge of what an RTC
  driver accepts as a valid date.  A fixed 2024 epoch keeps the guest's date plausible while
  being a pure function of the instruction stream.
* **MEASURED, the fix does not change one byte of the transcript** (§5): the guest prints
  no absolute date, so only the *schedule* moved.

### (2) `src/cpu/instructions_0f.c` — `rdrand_value()` read `/dev/urandom`

* **Decision: ALWAYS the seeded generator, with a FIXED seed; `/dev/urandom` is gone.**  The
  guest can observe RDRAND (an i386 Linux kernel executes `rdrand ecx` in
  `random_init_early` and mixes the result into its CRNG), and "usually the same" is not
  "the same".  A fixed seed rather than a per-run seed, because a per-run seed is host
  entropy by another route.
* This is a **port-level deviation from upstream** (upstream's `js::get_rand_int` is the
  platform CSPRNG), taken for a port-level requirement, and declared at the function.
* **NOT the cause of the count divergence, MEASURED** — see the control in §4.

### (3) `Date.now()` "in the network stack" — **VACUOUS here, verified, not assumed**

Neither NE2K nor virtio-net is in this machine, so there is no such site to patch:
`README.md:22` lists `sound (SB16), NE2K/virtio-net, floppy, modem, 9p` as NON-GOALS, and
`src/hw/machine.h:17` records the same for the assembled device set ("NOT in this machine,
and therefore not devices: the VGA, the floppy controller, virtio-net/virtio-9p/
virtio-balloon, the APIC/IOAPIC and the sound card").  A whole-`src/` sweep for the other
`Date.now()`-class imports finds **no remaining site**: after this unit,
`clock_gettime` appears in **no file under `src/`** (the only `<time.h>` user left is
`rtc.c`, for `gmtime_r`), the one other upstream `Date.now()` (`ps2.c:212`,
`last_mouse_packet`) is already the constant `0` and is upstream's own dead
rate-limiter comparison.

### The clock policy itself was already deterministic — VERIFIED, not assumed

D-F2 (`src/cpu/exec_boundary.c:49-83`) makes `microtick()` a pure function of
`cpu->instruction_counter`, and every device timer but the RTC already consumed it
(`pit.c`, `acpi.c`).  With the RTC moved onto it, **the entire machine's time base is the
guest's own instruction stream**.

## 2. The gate

```sh
# the port's own binary, built by the tree (the gate never runs make):
V86C=build/v86c FW=build/minibios-80m.bin sh tests/gate-m2-5-determinism.sh all
# or, to pin a FROZEN binary (what was done here, see §6):
V86C=/tmp/v86det/bin/v86c-fixed FW=/tmp/v86det/bin/minibios-80m.bin \
    sh tests/gate-m2-5-determinism.sh all
```

It re-runs slice 3's pins, boots the C leg **twice** (each ~60 s), and asserts:

1. the two **serial** transcripts are byte-identical (`cmp`),
2. the two **virtio** transcripts are byte-identical (`cmp`), and
3. the two runs executed the **same instruction count** (parsed from the port's own
   `v86c boot: N instructions` line).

No normalization, no filter: a line that legitimately differs is a FINDING, and the gate
prints it whole.  Nothing here weakens an existing assertion — (3) is *added*.

**The exact cmdline (both runs, byte-for-byte):**

```
console=hvc0 console=ttyS0 tsc=unstable idle=poll rdinit=/fx/store/54ce1971a82e89d78a83e3f805893b4e1651d714c62e69fda2c711ff8d3f2b69-fx-init/fx-init fx.store=/fx/store
```

**The exact inputs** (all sha256-pinned, re-verified by the gate): kernel
`tests/images/vmlinuz-i386-v86guest` `f0278a8f…`, initrd `tests/images/guest-initrd-fxdisk.xz`
`accc806d…`, disk `tests/images/disk-fmt-64m.img` `a2f0f6da…`, firmware
`build/minibios-80m.bin` `19c54f2d…`, and the machine's 80 MiB / limit 4000000000 (below
2**32-1, per the slice-3 note).

**The fail-before, as files** (`results/`, produced from the frozen pre-fix binary
`73a28df1…` on the pinned image/cmdline):

```
results/m2-5-det-prefix-run1-serial.txt  sha256 29c332ffe9611f745a547a6b2764cb7167b8cf2dca9bd4b020253264fa1e28e8
results/m2-5-det-prefix-run2-serial.txt  sha256 29c332ffe9611f745a547a6b2764cb7167b8cf2dca9bd4b020253264fa1e28e8
  -> cmp: identical (as are the two virtio transcripts)
results/m2-5-det-prefix-stderr-diff.txt  (4 lines, the ONLY difference between the runs)
  < v86c boot: 4000068327 instructions, ...
  > v86c boot: 4000019789 instructions, ...
```

## 3. The binaries

| role | sha256 | how built |
| --- | --- | --- |
| PRE-FIX | `73a28df1300770d6877afe6c19de30ef1f0c42435f35ab14415c6187bc35b9d2` | the tree before this unit's src edits |
| POST-FIX | `52c9fd012b83d23c5e5febecd5794f64cdfe13c8ad7831ef967865bf11590415` | after them |
| POST-FIX, **the tree as it stands** (four builds, three object dirs, all identical) | `2ee29c846da219dc88b5e5a05a52cf72301fab3b07cc7385d3cdaadc5d3bf703` | §3, §8(c)-(d) |
| POST-FIX, an earlier build of the same sources (peer's file in flight) | `b6c942c906e18cf58154ffb5037367072900355680a635b280489e920eef8fb4` | §8(d) |
| CONTROL (RTC host clock, RDRAND fixed) | `d7dafb178a68b59f1c95237540fe9096fbf098ccde9d07f20aed1fe610d31a03` | §4 |

**BUILD REPRODUCIBILITY, MEASURED — and it is a finding about the concurrent writer, not a
nonce.**  Four builds of my own final sources are **byte-identical**: `build-det2` (15:43),
`build-det` (15:52), `build-det3` (15:55), and `build-det4` (15:58 — the *same* sources with
one `rtc.c` **comment** line reverted, which shows a comment does not move this binary)
all give `2ee29c84…`.  Two EARLIER builds in the same window gave different bytes
(`52c9fd01…` 15:35 and `b6c942c9…` 15:41, 802474 of 2515871 bytes apart).  The only
source that could have differed between them is `src/cpu/jit_codegen.c` — the file the
concurrent peer session was editing, **inside the unity build** — so the difference is
attributed to the concurrent writer **by elimination, not by direct observation** (no copy
of the peer's file at those instants survives).  It does not touch this unit's claim:
`52c9fd01…`, `b6c942c9…` and `2ee29c84…` were each run through the gate and gave the SAME
answer (identical transcripts, `4000053925` instructions).

## 4. ATTRIBUTION: which site caused the divergence (the control)

A build with **the RTC back on the host clock and RDRAND kept fixed** (control
`d7dafb17…`) still diverges:

```
ctrl1 rc=0 4000097141 instructions
ctrl2 rc=0 4000077187 instructions     <- differ by 19954
CONTROL: the two serials are identical
CONTROL: the two virtios are identical
```

So with RDRAND's value fixed, the nondeterminism is **still there**, and the only remaining
difference from the fixed build is the RTC's time base — **the RTC was the cause; RDRAND was
not** (for this observable).  Note also the control's two runs both print their *identical*
transcripts while diverging by ~20k instructions, which is the transcript/count split again,
at a larger amplitude.

## 5. The guest's own wall-clock lines: **NONE differ, and this is measured three ways**

Risk (b) in the brief was "some guest lines are the KERNEL's own wall-clock-derived prints".
MEASURED, there are none in this transcript, and the evidence is stronger than a diff of two
adjacent runs:

* the PRE-FIX pair (60 s apart, host clock live) is byte-identical;
* the POST-FIX pair (a **different time base**, epoch 2024 instead of the host's 2026) is
  byte-identical to **itself** *and* to the PRE-FIX pair: `29c332ff…` / `3b9d566c…` both;
* that same `29c332ff…` / `3b9d566c…` pair is **byte-identical to slice 3's own recorded
  evidence** (`results/m2-5-guest-armA-c-serial.txt` / `-virtio.txt`, produced at 10:29 by a
  *third*, older binary of `build/v86c`) — and the regression re-run in §8(b) regenerated
  those two files from the *fixed* binary with the same bytes, so the identity is confirmed
  forwards and backwards.

So the printed transcript is a constant here: the kernel's `[    0.000000]` timestamps come
from the port's *monotonic* clock, which D-F2 already made deterministic, and nothing prints
an absolute date.  **The fix changes no transcript byte** — the observable it closes is the
instruction stream.

## 6. Honesty about the method: the tree had a SECOND writer

The brief says "one writer; nothing else is running".  MEASURED, that is **false**: a peer
coder session (`a490f8f9`, `HANDOFF=v86c-jit` / unit JIT-FIX-1, started 15:23) was live in
`/home/jaye/futaie/souche`, editing `src/cpu/jit_codegen.c` + `jit_codegen_selftest.c` and
running `make` (see `fx-agent-presence "$PWD"`).  Its first cost was real: my first run pair
had `build/v86c` **relinked underneath it mid-run** (mtime 15:27 → 15:29 → 15:30) and run 2
died with **SIGBUS/BUS_ADRERR** (`coredumpctl` 1789413) — writing to the mmap'd executable
while it is being replaced.

Two countermeasures, both of them the repo's own prescribed ones:

* **build privately**: `make BUILD=build-det -j8` (never touches `build/`, so it cannot
  corrupt the peer's build either), and
* **boot a frozen copy**: `cp build-det/v86c /tmp/v86det/bin/v86c-<tag>` and pass `V86C=` —
  which is exactly what `tests/gate-m2-5-guest.sh`'s header prescribes ("build/v86c is
  rebuilt by whoever is editing src/ … set V86C= to pin a frozen binary").

The file sets were disjoint throughout (mine: `src/hw/rtc.c`, `src/hw/rtc.h`,
`src/cpu/exec_boundary.c`, `src/cpu/instructions_0f.c`, `src/hw/hw_selftest.c`, `tests/`;
theirs: the two jit files), and mtimes were re-checked before each of my edits.  The peer's
edits *are* inside the unity build, so the binaries here carry whatever it had saved at
build time (JIT is OFF by default and is not on the boot path — and the binary hash is
recorded for each run above, so this is pinned, not assumed).

## 7. The RTC's selftest assertion had to move with it (and it is now EXACT)

`src/hw/hw_selftest.c`'s `test_rtc` used to compare the CMOS time registers against
`time(NULL)`/`gmtime_r` and accept either side of a second boundary — an assertion that
**passes with the host still feeding the RTC**, i.e. one that could not have caught this
unit's defect.  It now asserts hand-computed literals for two known instruction counts on
the fixed epoch:

* 123456789 instructions (123.456789 s of virtual time) → `2024-01-01T00:02:03Z` →
  sec `0x03`, min `0x02`, hour `0x00`, day-week `0x02` (a Monday), day `0x01`, month `0x01`,
  year `0x24`, century `0x20`;
* 3600000000 instructions (3600 s = exactly an hour) → `2024-01-01T01:00:00Z` → sec `0x00`,
  min `0x00`, hour `0x01` — the RTC **moves with the guest's own instruction count**, which
  a host-fed RTC cannot do at all.  (`instruction_counter` is a uint32 — the same shape the
  instruction limit has — so a whole day, 8.64e10 µs, is not reachable in one value.)

## 8. PROBE / REGRESSION RESULTS (all MEASURED — `results/m2-5-det-probe-and-regression.txt`)

**(a) THE SELFTEST'S ASYMMETRY: the RTC case FAILS with the host clock restored.**  Mutation
probe — `rtc_now_ms()` back to `clock_gettime(CLOCK_REALTIME)`, nothing else changed — gives
`rc=1` and **12 failing checks, all of them the new time-register literals**:

```
FAIL src/hw/hw_selftest.c:578: io_port_read8(0x71) == bcd_of(3)  (got 0x35, want 0x3)
FAIL src/hw/hw_selftest.c:580: io_port_read8(0x71) == bcd_of(2)  (got 0x42, want 0x2)
FAIL src/hw/hw_selftest.c:590: io_port_read8(0x71) == bcd_of(24) (got 0x26, want 0x24)
```

Source restored (sha256 `bb88b0e6…`) and rebuilt: `hw_selftest: 551 checks, 0 failures,
0 skipped`.  The new case is the thing that fails before the fix and passes after — the
asymmetry *is* the proof, and the case it replaced (host `time(NULL)`, "either side of a
second boundary") passed under this mutation.

**(b) THE BOOT GATE IS NOT BROKEN BY THE FIX — slice 3, re-run on the fixed binary.**
`V86C=v86c-fixed FW=…/minibios-80m.bin sh tests/gate-m2-5-guest.sh c` →
**`0 FAILING assertion(s)`**, all 16 ARM A assertions PASS (the ATA disk identified, the
64 MiB geometry, `rdinit` resolved, the store read from the cmdline, the EXT4 mount, the
disk-store verdict read back, the pivot, `fx> `, `boot-ok v3`, no ramfs fallback, no mkfs
failure, no re-seed, no panic, and the two virtio lines).  Its regenerated transcripts are
byte-identical to the pre-regression record (`29c332ff…` / `3b9d566c…`); the ONLY line that
moved in the gate's own evidence files is the instruction count in
`results/m2-5-guest-armA-c-stderr.txt` (`4000032959` → `4000053925`), and the earlier bytes
are kept at `/tmp/v86det/slice3-backup/`.

**(c) THE SECOND INDEPENDENT BUILD** (fresh object dir, `2ee29c84…`) booted on the same
image/cmdline: serial, virtio **and the full stderr** (instruction count included) are
byte-identical to the frozen binary's run 1.  Three boots of the fixed code — across two
independently built binaries — all stop at **4000053925 instructions**, and the fourth
sample (slice 3's re-run) is the same number again.

**(d) THE TREE AS IT STANDS passes the gate too**, twice — `make BUILD=build-det` from the
final source gave `b6c942c9…`, and a rebuild gave `2ee29c84…`; both pairs: serial
`29c332ff…` == `29c332ff…`, virtio `3b9d566c…` == `3b9d566c…`, instructions `4000053925` ==
`4000053925`, `0 FAILING assertion(s)`
(`results/m2-5-det-current-tree-pair.txt`, `results/m2-5-det-final-binary-pair.txt`).
That is **eight POST-FIX boots** in all — including three independently built binaries of
the final source — and every one of them reports `4000053925`.

## 9. Files, raw evidence, and how to reproduce

* **Changed (src/):** `src/hw/rtc.c` + `src/hw/rtc.h` (the time base, the epoch, every
  doc claim about it), `src/cpu/instructions_0f.c` (RDRAND), `src/cpu/exec_boundary.c` (the
  stale "the RTC is the one device that does NOT use this clock" note),
  `src/hw/hw_selftest.c` (the RTC case).
* **Added (tests/, results/):** `tests/gate-m2-5-determinism.sh`
  (`a6662d18…`), `results/m2-5-determinism.md` (this file).
* **Source hashes of the final state:** `src/hw/rtc.c` `9ab2a370…`,
  `src/hw/rtc.h` `77cf6c6f…`, `src/cpu/instructions_0f.c` `d3877a39…`,
  `src/cpu/exec_boundary.c` `ae440dba…`, `src/hw/hw_selftest.c` `82ca13fa…`.
* **Raw evidence** (under `results/`): `m2-5-det-run{1,2}-{serial,virtio,stderr}.txt` — the
  POST-FIX pair;
  `m2-5-det-prefix-pair.txt` (the PRE-FIX pair's full gate output: the shas and the two
  differing instruction counts), `m2-5-det-fixed-pair.txt` (the POST-FIX pair),
  `m2-5-det-control-and-second-build.txt` (§4's control + §8(c)'s second build),
  `m2-5-det-control-run{1,2}-stderr.txt` (the control's own count lines),
  `m2-5-det-probe-and-regression.txt` (§8's mutation probe and the slice-3 re-run),
  `m2-5-det-current-tree-pair.txt` + `m2-5-det-final-binary-pair.txt` (§8(d)'s two gate
  runs on the tree's own binaries), and the
  PRE-FIX pair itself `m2-5-det-prefix-run{1,2}-{serial,virtio,stderr}.txt` +
  `m2-5-det-prefix-stderr-diff.txt` (the whole fail-before, kept as files);
  `m2-5-det-control-stderr-diff.txt` (the control's count diff).
  The frozen binaries live at `/tmp/v86det/bin/v86c-{prefix,fixed,ctrl-rtc-host,fixed-b2}`
  (recorded by sha above; `/tmp` is volatile, so the shas and the counts are transcribed
  here and the pre-fix binary is reproducible by reverting the two hunks below).
* **A NOTE ON THE TREE:** this unit built into private object dirs (`make BUILD=build-det …`)
  so that a concurrent writer's `make` could never relink the binary under a running boot
  (§6); `build-det/` is left in place with the current tree's `v86c`
  (`2ee29c84…`), and the three duplicate dirs were removed.  Nothing under `build/` was
  written by this unit.
* **Reproduce the PRE-FIX binary:** revert the two hunks — `rtc_now_ms()` to
  `clock_gettime(CLOCK_REALTIME)` and `rdrand_value()` to the `/dev/urandom` arm — and
  `make BUILD=<dir> -j8`.

## 10. OUT OF SCOPE (named, not silently dropped)

**The BROWSER lane's half of §7.3** is a separate decision and is not in this unit: v86's JS
is the read-only reference (`./v86` is untouched by this unit) and fx-demo ships its
unmodified build, so its route is `restore_state` (or nothing).  Also out of scope, and
named: the agent path can never be hermetic — only the build path is (§7.3's own words).
