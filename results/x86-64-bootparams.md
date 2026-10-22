# x86-64: the boot_params, field for field — and the init_size hypothesis REFUTED

**Status: MEASUREMENT ONLY apart from one default-off toolkit addition. The boot_params are dumped and
compared; the `init_size`-vs-window hypothesis is REFUTED by the tree's own 80 MiB firmware; the
divergence is elsewhere and the next measurement is named.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The boot_params the port hands the kernel (MEASURED)

New toolkit surface (default off): `V86C_BOOTPARAMS=<linear address>` with `V86C_DUMP_ON_STOP=1`
prints the standard zero-page fields. On the pinned kernel at `rsi = 0x84790`:

    v86c dbg: boot_params @0x84790 e820_entries=4 alt_mem_k=0x0 setup_sects=39
    v86c dbg:   code32_start=0x100000 ramdisk_image=0x0 ramdisk_size=0x0 ext_ramdisk=0x0/0x0
                cmd_line_ptr=0x8e000 ext_cmd=0x0 cmdline_size=0x7ff
    v86c dbg:   heap_end_ptr=0xde00 initrd_addr_max=0x7fffffff kernel_alignment=0x200000
                relocatable=1 pref_address=0x1000000 init_size=0x3f97000
    v86c dbg:   e820[0] base=0x0       len=0x9fc00   type=1
    v86c dbg:   e820[1] base=0x9fc00   len=0x400     type=2
    v86c dbg:   e820[2] base=0xf0000   len=0x10000   type=2
    v86c dbg:   e820[3] base=0x100000  len=0x3f00000 type=1
    v86c dbg:   e820[4] base=0x0       len=0x0       type=0   (the table is 128 entries; 4 are used)

Against QEMU's same-image boot (READ of its serial, and the standard layout for the rest):

| field | the port | QEMU | verdict |
|---|---|---|---|
| `e820_entries` | **4** | 8 (its serial lists 8 regions) | same shape, different count — the port's map has fewer *reserved* holes, which is harmless |
| `e820` usable region | `[0x100000, 0x4000000)` = **63 MiB** | `[0x100000, 0xffe0000)` ≈ 255 MiB | **the port's machine is 64 MiB by firmware geometry** |
| `code32_start` | `0x100000` | `0x100000` (inferred: QEMU's `-kernel` load address) | same |
| `init_size` | **`0x3F97000` = 63.6 MiB** | the same kernel → the same value (READ from the image's setup header, not the firmware) | same |
| `pref_address` | `0x1000000` | same (from the image) | same |
| `kernel_alignment` | `0x200000` | same (from the image) | same |
| `relocatable_kernel` | `1` | same (from the image) | same |
| `cmdline_size` | `0x7ff` | same (from the image) | same |
| `cmd_line_ptr` | `0x8e000` | QEMU's own (inferred) | both non-zero and inside RAM |
| `ramdisk_image` / `ramdisk_size` | **0 / 0** | 0 / 0 (no initrd on either side) | same |
| `heap_end_ptr` | `0xde00` | (inferred) | the port's stub sets it; the decompressor resets its own heap from `%rbx` |

**Note which fields the firmware can get wrong and which it cannot:** `init_size`, `pref_address`,
`kernel_alignment`, `relocatable_kernel` and `cmdline_size` are read from the **kernel image's own setup
header**, so they cannot differ between the two machines for the same image (MEASURED: they match).
The fields the *firmware* writes are the **E820**, the **cmdline pointer** and the heap/stack pointers.

## 2. The `init_size`-vs-window hypothesis, and its REFUTATION (MEASURED)

The tempting arithmetic: `init_size` is 63.6 MiB and the port's usable region above 1 MiB is 63 MiB, so
a 64 MiB aligned window cannot fit — the kernel would find no slot and disable KASLR.

**MEASURED, and it is wrong:** the tree already builds a second firmware at 80 MiB
(`build/minibios-80m.bin`, `MEM_TOP = 0x5000000`, made for the M2-3 initrd case). Running the same
kernel with it (`--bios build/minibios-80m.bin --memory 256`) gives a **79 MiB** usable region — a
64 MiB window fits — and the kernel **still** prints

    Physical KASLR disabled: no suitable memory region!

and stops at the same recursion bound (`eip=0x87A77246`, `cr3=0x30ea000`). **So the window size is not
the cause**, and the 64 MiB firmware geometry is not the defect this unit was hunting.

## 3. Where the divergence must be (INTERPRETATION + the next measurement)

The port's serial carries `Invalid physical address chosen!` *before* the fallback — `slots_fetch_random`
rejecting the address it picked. That is a **validity check inside the kernel**, not a slot search, and
the fields it reads are the ones the firmware writes: the **E820** and the **cmdline pointer** (and the
`mem_avoid` it builds from the decompressor's own `output`, `input_data` and the cmdline).

**The next measurement, named:** instrument `choose_random_location`'s three inputs as the port hands
them over — `input_data`, `input_len`, `output`, `output_len`, `needed_size`, and the `mem_avoid` array
the decompressor builds (`mem_avoid[0..3]`: the kernel image, the cmdline, the initrd, the heap) — and
compare them with QEMU's for the same image. The toolkit's `V86C_BOOTPARAMS` already gives the
boot_params half; the other half is the decompressor's own stack frame, which a `V86C_CHAIN`-style dump
of `%rbx`-relative locals would reach.

## 4. Why no fix is claimed

The firmware fields the port writes are **not shown to be wrong**: the E820 is the same shape as QEMU's,
the cmdline pointer is inside RAM, and the fields that would have to be wrong (`init_size`,
`pref_address`) come from the *image*, not the firmware — and they match. A minibios change would also
touch **every** guest gate with a **pinned byte** transcript, which the rule forbids on a hypothesis.

## 5. Verification (MEASURED; one default-off toolkit addition)

    make selftest                          rc 0 — all suites green
    make nasm                              rc 0 — Passed 118/118 tests
    tests/perf_bench.sh                    rc 0 — instr=200095214 (the tool present, off)
    tests/gate-x86-64.sh                   rc 0 — 12 passed, 0 failed
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757

No pin moved. The rest of the battery was not re-run because nothing functional changed — the QEMU
unit's full battery on the same tree is the record for it.

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/debug.c` | `V86C_BOOTPARAMS=<addr>`: the standard zero-page fields + the first six E820 entries, printed with `DUMP_ON_STOP` |
