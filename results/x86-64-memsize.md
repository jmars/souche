# x86-64: memory size does not change the port's path — the divergence is size-independent

**Status: MEASUREMENT ONLY. The memory-size axis has been varied for the first time in this lane, and
the answer is a finding rather than a configuration detail: the port stops at the SAME class of stop at
80 and at 256 MiB, while QEMU reaches a *late* milestone at both. So the divergence is not a
memory-model size defect — it is the same early-kernel fault.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The port at three sizes, against QEMU at the same sizes (MEASURED)

| memory | QEMU (KVM, same image + cmdline) | the port |
|---|---|---|
| 80 MiB | KASLR messages → `Kernel Offset: 0x19600000` → **`Kernel panic - not syncing: System is deadlocked on memory`** (its OOM end) | **`a fault raised while delivering a fault: the delivery re-entered four times`** at `eip=0x87A77246`, `cr3=0x30ea000`, 4,475,484,647 instructions |
| 256 MiB | KASLR → full boot → **`VFS: Unable to mount root fs`** (its correct end) | **the same class of stop** at `eip=0x8AE77246`, `cr3=0x30ea000`, 4,475,484,504 instructions |

The port's serial in both cases ends at the same two lines (`Invalid physical address chosen!`,
`Physical KASLR disabled: no suitable memory region!`) which §2 of the discriminator unit proved are
the *normal* early-boot path at a small size — QEMU prints them too at 80 MiB and boots on.

**The two port addresses differ (`0x87A…` at 80, `0x8AE…` at 256) because the kernel's physical layout
follows the memory size — the *stop* is the same class at both, and the instruction count is the same to
within 143 instructions.**

## 2. What the axis shows (MEASURED → a finding, not a detail)

**Memory size is not the constraint.** The divergence is **size-independent**, so it is *not*:

* the **32-bit physical model** (a larger size would have exposed a bound),
* the **mmap holes** (they scale with `--memory`),
* the **initrd placement** (there is no initrd on either side),
* the **heap/stack pointers the firmware publishes** (`heap_end_ptr = 0xDE00` at both sizes).

It is the same early-kernel fault at both sizes: a #PF arriving while the kernel's own IDT page is
unmapped, which the recursion bound turns into a named stop. **That is the whole of what this axis can
say, and it says it clearly.**

## 3. The calibrated acceptance test, stated properly (and NOT met)

The brief's calibration is right and the tree should carry it: **at 80 MiB the port should reach the
same OOM panic QEMU does; at 256 MiB, the same root-mount end.** The port reaches **neither** — it stops
some 4.47 G instructions in, in the kernel's own early code, before its first `printk` reaches the
serial.

**So there is no "first real amd64 kernel panic on this port" milestone to report, and none is claimed.**
The calibration is recorded in `results/x86-64-discriminator.md` and here, and a gate step asserting it
is **deliberately not added**: a step that asserted the *port's* stop would be exactly the
"assert the stop" pattern this lane has spent three units correcting.

## 4. The next measurement (unchanged and now the only one left on this axis)

**Did the kernel ever TRY to print?** `V86C_WATCH` on the serial port's I/O writes (`0x3F8`) with
`V86C_RING` + `V86C_DUMP_ON_STOP` at the stop. That separates the two shapes the discriminator unit
named: *"died before `console_init`"* (the kernel never wrote to 0x3F8) from *"printed but the port's
UART dropped it"* (writes happened and the serial file is missing them — a completely different defect,
and one the toolkit can prove in a single run).

## 5. Verification (MEASURED; **no functional change**)

    make selftest                          rc 0 — all suites green
    tests/gate-x86-64.sh                   rc 0 — 12 passed, 0 failed
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757

`git status` shows nothing under `src/`, `gen/` or `tests/`. No pin moved. The rest of the battery was
not re-run because nothing functional changed — the QEMU unit's full battery on the same tree is the
record for it.

## 6. Files touched

| file | change |
|---|---|
| — | **none.** Two port runs (80 MiB with `build/minibios-80m.bin`, 256 MiB with the default firmware) and the QEMU runs already on record. |
