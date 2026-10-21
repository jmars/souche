# x86-64: the post-decompression path — `parse_elf`'s byte-wise relocation, and a CORRECTION to the last report

**Status: measured; NO PORT DEFECT FOUND; one claim in `results/x86-64-wide.md` is REFUTED and
corrected here. No functional change was made (the port is byte-for-byte the last unit's, and the
whole battery is re-run on the probe-free build).**

The brief asked to get the guest past the decompressor into the real kernel's entry: measure first,
then fix what the port gets wrong. The measurement says the guest **is** past the decompression, is
inside `parse_elf`'s relocation, and is progressing — it is simply copying ~57 MB **one byte at a
time**, because that is what the kernel's own decompressor `memmove` does. There was nothing to fix,
and the honest deliverable is the measurement, the corrected claim, and the named next step.

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. CORRECTION: "the decompression completes (8,388,607 bytes)" was measuring my scan

`results/x86-64-wide.md` §3 reported the decompression complete because *"the output buffer's last
non-zero byte reaches 0x17FFFFF (all 8,388,607 bytes)"*. **That number was the ceiling of my own
scan**, which searched `0x1000000..0x1800000` — 8 MiB, chosen when the buffer was assumed to be the
kernel image. Re-measured with the scan widened to 32 MiB (MEASURED, same binary):

| instruction budget | last non-zero byte in `0x1000000..0x3000000` |
|---|---|
| 1.5 G (the last unit's run) | 0x17FFFFF — **the old scan's ceiling** |
| 2.4 G (this unit) | **0x2DFBC4F** (48.2 MB) |

So the decompressed image is **much larger than 8 MiB**, the old "extent" was pinned at the scan
limit exactly as a residual equal to the step size would be, and the previous report's "the whole
kernel" reading was an over-claim. It is corrected here rather than left standing, and the lesson is
the one this tree already carries: *a residual the same size as the scan step is measuring the scan.*

**What the image actually is (MEASURED, read out of the guest's buffer at 0x1000000):**

    ELF64, e_phoff 0x40, e_phnum 5
    phdr0  PT_LOAD  p_offset 0x200000  p_vaddr 0xFFFFFFFF81000000  p_paddr 0x1000000  p_filesz 0x18E6498 (26,107,032)
    phdr1  PT_LOAD  p_offset ...       p_paddr 0x2A00000           p_filesz 0x642000  (6,561,792)
    phdr2  PT_LOAD  p_paddr 0x3042000  p_filesz 0x35000            (217,088)
    phdr3  PT_LOAD  p_paddr 0x3077000  p_filesz 0x1989000          (26,775,552)
    phdr4  PT_NOTE  p_paddr 0x24BF4E0  p_filesz 0x1F8

A 26 MB kernel image with five segments whose `p_filesz` **sum to 59,661,968 bytes (~57 MB)** — the
segments overlap heavily, so the decompressor copies overlapping ranges of the same image.

---

## 2. What the guest is doing after the decompression (MEASURED)

READ (`arch/x86/boot/compressed/misc.c:456-465`, v6.1): after `__decompress` the stub runs

    parse_elf(output);                                    /* memmove each PT_LOAD into place */
    handle_relocations(output, output_len, virt_addr);     /* delta == 0 here -> immediate return */
    debug_putstr("done.\nBooting the kernel.\n");          /* COMPILED OUT: debug_putstr is a no-op
                                                            * unless CONFIG_X86_VERBOSE_BOOTUP */
    cleanup_exception_handling();                          /* loads a NULL IDT */
    return output;                                         /* head_64.S jumps to it */

`parse_elf` is `for each PT_LOAD: memmove(p_paddr, output + p_offset, p_filesz)` — and the kernel's
decompressor `memmove` copies **one byte per iteration**.

**The loop, identified (MEASURED):** `0x4F399A8..0x4F399E5`, called from `0x4F3A36E`
(`mov [rbx+0x7c], esi; mov edx, esi; mov rdi, r15; mov rsi, r14; call`), its state at
`0x4F58588`: `[0] = 0x1000000` (the buffer), `[0x10] = [0x18] = pos`, `[0x28] = 0xFFFFFFFF`
(the source's wrap limit), `[0x30] = 0x2000000`.

**The copy's destination index, sampled (MEASURED — monotone, no wrap):**

| instructions | destination index |
|---|---|
| 14 M | 0xF4277 (1.0 MB) |
| 27 M | 0x1E84B7 (2.0 MB) |
| 166 M | 0x3070C5 (3.2 MB) |
| 300 M | 0x415D5A (4.3 MB) |
| 1527 M | 0x139CA59 (20.5 MB) |
| 1800 M | 0x15912DB (22.6 MB) |
| 2400 M | 0x1DFBC4F (31.4 MB) |

**The conclusion is that the machine is progressing, not stuck**, and that the remaining cost is
arithmetic: ~57 MB of byte-wise `memmove` at the measured ~39 instructions per byte is ~2.3 G
instructions on top of the ~1.2 G the decompression itself costs. `unimplemented=0` at every budget
tried (40 M, 300 M, 1.5 G, 1.8 G, 2.4 G), no named stop, and no jump into the kernel yet.

**INTERPRETATION, stated as such:** the one field that looks wrong — `[0x28] = 0xFFFFFFFF`, the
limit at which the loop's source index wraps — is consistent with a 32-bit `-1` in a struct field
the guest wrote itself; it never fires because the source index stays below 4 GiB. I could not
attribute it to the port, and I am **not** claiming it as a defect.

---

## 3. Why there is no fix in this unit

The brief's instruction was "fix whatever the port gets wrong". Three candidate defects were raised
and each was **refuted by measurement** before any code was touched:

1. *"The copy runs past the end of the image — an unbounded `memmove`."* Refuted: the image is 26 MB
   and the segments sum to 57 MB; the destination index is inside the range `parse_elf` legitimately
   copies (§2). The apparent overflow was the 8 MiB scan cap of §1 again.
2. *"`p_filesz` is garbage, so `malloc`'d phdrs are bad."* Refuted: the five program headers read
   out of the guest's buffer are the kernel's real ones (§1) — 26 MB, 6.5 MB, 0x35000, 26 MB, 0x1F8.
3. *"The stub never reaches `parse_elf` because `Decompressing Linux…` is missing from the serial."*
   Refuted: READ (`arch/x86/boot/compressed/misc.h:61-77`) — `debug_putstr` is a **no-op** unless
   `CONFIG_X86_VERBOSE_BOOTUP`, so those messages are compiled out of the Debian kernel. The two
   lines that *do* appear (`Invalid physical address chosen!`, `Physical KASLR disabled: …`) come
   from `warn()`/`error()`, which use `putstr` directly.

**NAMED NEXT MEASUREMENT** (not a fix, and not a tightened gate): a **~5 G instruction** run to
catch the jump into the kernel — `parse_elf` should finish near 3.5 G and `head_64.S`'s
`jmp *%rax` then enters the decompressed kernel at 0x1000000+. The instrument is the one this unit
built: the `JUMPED INTO THE KERNEL` trigger (first `eip` inside `0x1000000..0x2000000`) plus the
destination-index series above. **The cost is the guest's own byte-wise `memmove`**, so the second
thing worth measuring is whether a boot can afford it at all (a 5 G-instruction boot is ~25 minutes
of wall clock at the observed rate).

---

## 4. Verification (MEASURED; every gate by exit code)

**No source file was changed by this unit** (`git status` reports no modification under `src/`: the
probes were added, run, and removed, and `src/cpu/cpu.c` / `src/main.c` were restored). The battery
below was re-run on the final, probe-free build.

    make                                   rc 0, no warnings (cosmocc -Wall -Wextra -Werror)
    make selftest                          rc 0 — cpu 3889/0, m1c 272/0, hw 975/0,
                                           uop_cache 7934363/0, uop_exec 102/0, jit 68/0,
                                           jit_codegen 3570/0
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (80/80 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-pae-l3.sh all               rc 0 — 0 FAILING
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed
    tests/gate-vga-{0,1,2,3,4}.sh          rc 0 each — 0 FAILING

No pin moved — the four M0 pins (transcript, determinism, bench, nasm) and every corpus/census pin
are exactly the last unit's, because nothing functional changed. **No gate was tightened**;
`gate-x86-64.sh`'s assertions are the last unit's and still pass at its 40 M budget (where the guest
is inside `parse_elf` at `eip=0x4f3a36e` with `unimplemented=0`).

---

## 5. Files touched

| file | change |
|---|---|
| — | **none functional.** Probes were added to `src/cpu/cpu.c` and `src/main.c`, run, and removed; both files were restored from git and the build re-verified. |

The measurement instruments themselves are recorded here rather than left in the tree: the
destination-index series, the loop's identity (`0x4F399A8` / caller `0x4F3A36E` / state at
`0x4F58588`), the program-header dump, and the widened extent scan. The next unit should rebuild the
first and last of those in minutes; leaving them in the port would be instrumentation in a machine
whose contract is byte-identical output.
