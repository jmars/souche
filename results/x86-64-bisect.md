# x86-64: the serial bisect — the divergence is the kernel's FIRST printk, and the PML4 entry it needs was never written by the kernel

**Status: MEASUREMENT ONLY. The first divergence line is named; the access behind it is named; and the
measurement shows that the one write that should have made the kernel's page tables valid never
happened — its only writer in the whole run is the decompressor.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The first line where the two serials diverge (MEASURED)

Both at 256 MiB, the same image and cmdline, `earlyprintk=serial console=ttyS0`:

* **QEMU** (`/tmp/q-serial.txt`, 23,978 bytes) begins

      1  Probing EDD (edd=off to disable)... ok
      2  [    0.000000] Linux version 6.1.0-50-amd64 (debian-kernel@lists.debian.org) … Debian 6.1.176-1

* **the port** (`/tmp/xp2.txt`) begins

      1  minibios: option ROM at c000
      2  Probing EDD (edd=off to disable)... ok
      3  Invalid physical address chosen!
      4  Physical KASLR disabled: no suitable memory region!

**The first divergence is QEMU's line 2 — the decompressed kernel's FIRST `printk`.** QEMU prints it;
the port never does. And the discriminator unit proved that the two KASLR lines are the *normal* path at
a small size (QEMU prints them too at 80 MiB and boots on), so the port and QEMU agree up to and
including the decompressor's KASLR messages and diverge at the kernel's first line of console output.

**READ:** the kernel prints `Linux version …` from `start_kernel` → `setup_arch`'s
`pr_info("%s", linux_banner)` — i.e. **after** `console_init`-equivalent early console setup, after
`__startup_64`'s page-table fixup and after `secondary_startup_64` has installed the kernel's own CR3.
So the port's kernel dies **between the handover and that first `printk`** — the same window the
memsize unit's instruction counts put it in.

## 2. The named access behind it (MEASURED — `V86C_WATCH` on the PML4 entry)

The kernel runs with `cr3 = 0x30EA000`, and its `early_top_pgt` — the static page table the kernel
fixes up and loads — lives there. A watch on **`0x30EA000`, the first PML4 entry, 8 bytes**:

    v86c dbg: WATCH V 0x30ea000 width=1 value=0x15 rip=0x4f399c0
    v86c dbg: WATCH V 0x30ea001 width=1 value=0x00 rip=0x4f399c0
    v86c dbg: WATCH V 0x30ea002 width=1 value=0x18 rip=0x4f399c0
    v86c dbg: WATCH V 0x30ea003 width=1 value=0x00 rip=0x4f399c0
    v86c dbg: WATCH V 0x30ea004 width=1 value=0xe8 rip=0x4f399c0
    v86c dbg: WATCH V 0x30ea005 width=1 value=0xff rip=0x4f399c0
    v86c dbg: WATCH V 0x30ea006 width=1 value=0x15 rip=0x4f399c0
    v86c dbg: WATCH V 0x30ea007 width=1 value=0x00 rip=0x4f399c0

**Every write to that entry in the whole run comes from `rip = 0x4F399C0` — the DECOMPRESSOR's output
flush** (byte by byte, the values are image data: `0x0015FFE800180015`). **The kernel never writes it.**
So the entry the kernel's 4-level walk reads as its PML4[0] is *decompressed image bytes*, not the
fixup `__startup_64` is supposed to store there — which is exactly why the walk fails at a later level
(`cr2 = 0x8B1100E0`, the IDT descriptor) and why the delivery cannot proceed.

**INTERPRETATION, and it is the crux:** the kernel's `__startup_64` **page-table fixup did not run (or
did not write)**. That is a named, single-instruction question: `__startup_64`'s stores to
`early_top_pgt` (and the `mov %rax, %cr3` that follows) — the port's ring can name the instruction it
did instead.

## 3. Why no fix is claimed here

The measurement names the missing write, not the defect: *why* `__startup_64`'s stores did not land is
the next question, and it has three shapes — the fixup code was skipped (a branch on a wrong flag), it
wrote to a different address (an address computation, the class this lane has hit four times), or the
kernel's own `cr3` is not the table it fixed up (a CR3/CR4 write ordering). **Each has a different fix
and two of them would change the machine.** The toolkit answers it in one run: `V86C_WATCH` on the
`early_top_pgt` symbol's physical address **plus** `V86C_RING`, so the run shows both the stores the
kernel *did* make and the instruction that preceded the stop.

## 4. Verification (MEASURED; **no functional change**)

    make selftest                          rc 0 — all suites green
    tests/gate-x86-64.sh                   rc 0 — 12 passed, 0 failed
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757

`git status` shows nothing under `src/`, `gen/` or `tests/`. No pin moved. The rest of the battery was
not re-run because nothing functional changed — the QEMU unit's full battery on the same tree is the
record for it.

## 5. Files touched

| file | change |
|---|---|
| — | **none.** READ of the two serials; one port run with `V86C_WATCH` on the PML4 entry. |
