# x86-64: QEMU at 80 MiB prints the SAME two KASLR messages — so the divergence is NOT KASLR

**Status: MEASUREMENT ONLY. The cleanest discriminator the last three units named has been run, and it
settles them: QEMU at the same small geometry prints `Invalid physical address chosen!` and `Physical
KASLR disabled: no suitable memory region!` too, and then boots. Those messages are the NORMAL
early-boot path at a small memory size, not a divergence — and the port's stop is somewhere later.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The discriminator (MEASURED — QEMU/KVM, `-m 80`, same image and cmdline, no initrd)

    timeout 130 qemu-system-x86_64 -machine pc -m 80 -enable-kvm -no-reboot -display none \
        -kernel tests/images/vmlinuz-6.1.0-50-amd64 \
        -append 'console=ttyS0 earlyprintk=serial' -serial file:/tmp/q80.txt

QEMU's serial, from the top:

    Probing EDD (edd=off to disable)... ok
    Invalid physical address chosen!
    Physical KASLR disabled: no suitable memory region!
    [    0.000000] Linux version 6.1.0-50-amd64 … Debian 6.1.176-1
    [    0.000000] Command line: console=ttyS0 earlyprintk=serial
    [    0.000000] BIOS-provided physical RAM map: …
    … Memory KASLR using RDTSC... / Poking KASLR using RDTSC...
    [    1.182055] Kernel Offset: 0x19600000 from 0xffffffff81000000 …
    [    1.182055] ---[ end Kernel panic - not syncing: System is deadlocked on memory ]---

**QEMU prints THE SAME TWO LINES the port does**, and then boots: physical KASLR is disabled, *virtual*
KASLR (`Kernel Offset: 0x19600000`) runs instead, and the kernel reaches its own OOM panic
(`System is deadlocked on memory`, the correct end for 80 MiB).

## 2. What that settles (MEASURED → three units of hypothesis are closed)

* **The two KASLR messages are the NORMAL early-boot path at a small memory size**, not a port
  divergence. The port's serial has shown them since the jump unit, and QEMU at the same geometry shows
  them too.
* **`results/x86-64-kaslr.md`'s "the port's kernel rejects the address it chose" and
  `results/x86-64-bootparams.md`'s `init_size`-vs-window arithmetic are both beside the point**:
  `Invalid physical address chosen!` is a *normal* message from `slots_fetch_random` at this size, and
  the port's kernel does exactly what QEMU's does with it. The 80 MiB firmware run (which still printed
  the same lines) was therefore not evidence of anything either.
* **The divergence is LATER than KASLR.** QEMU gets from these messages to its own panic; the port gets
  from the same messages to a #PF whose delivery re-enters four times (`eip=0x87A77246`,
  `cr3=0x30ea000`). **That is where the bisect now belongs**, and it is the same class the deliver/pf2
  units measured: a fault arriving while the kernel's own IDT is not mapped.

## 3. The next measurement, named

**Bisect the two serials from the last line they share.** QEMU's next line after the KASLR messages is
`[ 0.000000] Linux version …` (the decompressed kernel's first `printk`). The port's serial stops at the
KASLR messages — because the port's *decompressor* never reaches the `jmp *%rax` handover this time?
No: MEASURED, the port does enter the kernel (the last several units ran at `eip=0x87A…`/`0x8AE…` with
`cr3=0x30ea000`, i.e. the kernel's own tables). So the port's kernel runs and dies **before its first
`printk` reaches the serial** — i.e. between the handover and `console_init`.

**The instrument:** the toolkit's `V86C_RING` + `V86C_DUMP_ON_STOP` at the stop, and a `V86C_WATCH` on
the serial port's I/O writes (`0x3F8`) to see whether the kernel ever *tried* to print — that
distinguishes "the kernel died before `console_init`" from "the kernel printed but the port's UART
dropped it", which are very different defects.

## 4. Why (2) — a second image — was not needed, and what it would still buy

The discriminator answered the question (2) was there to answer, so no second image was fetched: the
port's kernel behaves as QEMU's does at the same size, and the divergence is later. A second amd64 image
would still be valuable for the **bisect** (a kernel whose early path is simpler), and the discipline
for adding one is recorded in `results/x86-64-qemu.md` §4 (URL + sha256, a size decision, or a
fetch-and-verify script with the pin in the gate).

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
| — | **none.** One QEMU run. |
