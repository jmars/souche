# x86-64: QEMU boots the same image — so the triple-fault window is the PORT's, not the image's

**Status: the second reference settles it. QEMU (KVM) boots the pinned amd64 kernel with the same
cmdline and memory all the way to its root-mount panic, so the window the port stops in is the PORT's.
The QEMU leg is now a gate step. A second amd64 image was NOT obtained, and that is recorded as a gap
with the reason.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The QEMU leg (MEASURED — KVM, the fast path)

READ (`tests/gate-m2-5.sh`, gate 1b) for the invocation shape (its `-machine pc -m $MEM -enable-kvm
-no-reboot -display none … -serial file:` arm and its `timeout 120`).  The same shape on the pinned
image, the same cmdline and memory the port gets, **no initrd** (the port gets none):

    timeout 130 qemu-system-x86_64 -machine pc -m 256 -enable-kvm -no-reboot -display none \
        -kernel tests/images/vmlinuz-6.1.0-50-amd64 \
        -append 'console=ttyS0 earlyprintk=serial' -serial file:/tmp/q-serial.txt

**Result (23,978 bytes of serial): the kernel BOOTS.**

    Probing EDD (edd=off to disable)... ok
    [    0.000000] Linux version 6.1.0-50-amd64 (debian-kernel@lists.debian.org) … Debian 6.1.176-1
    [    0.000000] Command line: console=ttyS0 earlyprintk=serial
    [    0.000000] NX (Execute Disable) protection: active
    [    0.000000] Hypervisor detected: KVM
    …
    [    0.911874] Kernel Offset: 0x6400000 from 0xffffffff81000000 (relocation range: …)
    [    0.913126] ---[ end Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0) ]---

**The panic is the correct end for this cmdline** (no initrd → no root fs). The kernel reached
`mount_block_root` through `kernel_init` — i.e. **it ran its own code, set up its own tables and its
own IDT, and handed over to `init`'s failure path.**

## 2. What that settles (MEASURED → the last unit's conclusion is REFUTED)

`results/x86-64-firstfault2.md` concluded that the guest *"cannot proceed past its own triple-fault on
THIS image"* and that the honest next step was *"a different guest or a different expectation"*.
**That is refuted by this measurement:** the same image, same cmdline, same memory boots under a second
independent reference. **The triple-fault window is the PORT's, not the image's** — the port diverges
somewhere in the kernel's early bring-up, and QEMU's serial is now the reference sequence to bisect
against (the port's own serial stops at the decompressor's KASLR line, so the comparison starts where
the kernel's first `printk` would land).

**INTERPRETATION, offered as the working hypothesis and not a finding:** the port's kernel runs with
`Physical KASLR disabled: no suitable memory region!` (its own serial) while QEMU's runs with a
`Kernel Offset` — so the two take **different early-boot paths**, and the port's path is the one that
faults. That is the first thing to compare.

## 3. The gate step (MEASURED — stable, with the tree's flake discipline)

`tests/gate-x86-64.sh` gains a section E with three steps and one able-to-fail arm:

    step "qemu-boots-the-same-image"      grep 'Linux version 6.1.0-50-amd64' in the QEMU serial
    step "qemu-reaches-the-root-mount"    grep 'Unable to mount root fs' in the QEMU serial
    must_fail "qemu-able-to-fail"         the same grep against the PORT's boot log (it must not match)

**The assertion is on the SERIAL, not on qemu's exit code**, because a QEMU run under load can time
out and a timeout is not a regression — the tree's own rule. `gate-x86-64.sh`: **12 passed, 0 failed**
(was 9).

## 4. The second image: NOT obtained, and why (gap, stated)

The tree holds **one** amd64 kernel (`tests/images/vmlinuz-6.1.0-50-amd64`); the other pinned images
are i386 (`vmlinuz-i386*`, `vmlinuz-6.1.0-50-686-pae`). Obtaining a second amd64 kernel means a download
with its own provenance pin (URL + sha256) and a size decision about sitting beside the existing
images — the same discipline as the PAE-L3 pin. **It is not done in this unit, and the reason is
stated rather than implied:** §1's measurement makes it **no longer the next question** — the second
reference for the SAME image answered it. The second image would answer a different question (does the
port's divergence reproduce on a kernel with a different early-boot ordering), and it is the natural
follow-up to §2's bisect rather than a substitute for it.

## 5. Verification (MEASURED; code changed — the gate only)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214
    tests/gate-x86-64.sh                   rc 0 — 12 passed, 0 failed   ← the QEMU leg is now a step
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (84/84)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-pae-l3.sh all               rc 0 — 0 FAILING
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed
    tests/gate-vga-{0,1,2,3,4}.sh          rc 0 each — 0 FAILING

**Every pin unmoved** (the change is a gate step, not the machine).

## 6. Files touched

| file | change |
|---|---|
| `tests/gate-x86-64.sh` | section E: the QEMU leg as three steps + an able-to-fail arm, asserting the SERIAL (a timeout is not a regression) |
