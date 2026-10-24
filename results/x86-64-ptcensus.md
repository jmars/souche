# x86-64: the census says NO — the kernel never asks for that entry, so the crossing is the layout's

**Status: MEASUREMENT (a complete census of every page-table store, one KASLR boot, plus the QEMU
comparison; no source change, no fix). The answer to the brief's question is NO: there is no call with the
missing slot as its destination — the kernel never asks for it — so the stack really does cross into a
1 GiB region this configuration never maps, and hardware would #DF too.** Claims tagged MEASURED / READ /
INTERPRETATION.

## 1. The census (MEASURED, break at the generic `set_pXd` store, **all** calls)

`V86C_BREAK=0xffffffff88e7e5f0` (link `0xffffffff8107e5f0`, `mov %rsi,(%rdi)`), `V86C_BREAK_MAX=2600`:

    break slot 0 0xffffffff88e7e5f0+0x1 hits=2531        ← every page-table store in the whole run
    ... exactly ONE of the 2531 has a destination in physical 0x100000..0x101000:
    rsi=0x4b6067  rdi=0xffff8c16c01000d8                 ← PDPT index 27, value 0x4b6067

(The destination is a direct-map virtual address; `rdi - page_offset_base (0xffff8c16c0000000)` is the
physical slot.)

**There is NO call with `rdi = 0xffff8c16c01000d0`** — PDPT index 26, the entry the stack needs. The
kernel does not ask for it, so nothing is dropped: this is not a store the port lost or mis-addressed.

## 2. What that means, plainly

The task stack straddles the 1 GiB boundary at `0xffffd206c0000000` (`rsp = 0xffffd206bfffffc0` below it,
the stack top `0xffffd206c0000382` above it), the kernel maps only the upper region, and the lower half
therefore faults on the first store. **Hardware would take the same fault and, because the frame push
lands on the same unmapped page, the same double fault** — which is exactly what the port names
(`… hardware would raise #DF; the port stops rather than recursing`, `results/x86-64-deliver2.md`). So
**the KASLR configuration's stop is a KERNEL-side consequence of this layout, not a CPU defect.**

## 3. The layout's cause: the decompressor's failed physical KASLR (MEASURED)

    port, KASLR configuration:   Invalid physical address chosen!
                                 Physical KASLR disabled: no suitable memory region!
    QEMU, same kernel, 256 MiB:  (no such line) — and at its panic: Kernel Offset: 0x31200000 …

The port's decompressor tries to place the kernel physically, finds **no region it accepts**, prints
`Invalid physical address chosen!` and falls back to a layout of its own. QEMU's decompressor places it
without complaint and QEMU's kernel goes on to the root-mount panic. **So the crossing is downstream of
that failed placement: the port's memory map does not offer the decompressor a window it accepts, and the
resulting layout is what puts a task stack across a 1 GiB boundary the kernel then leaves unmapped.**

## 4. What this is, in one line

**A machine-input finding, not a CPU defect**: the KASLR configuration's stop is the kernel's own
consequence of a physical placement the port's memory map forced on it (`Invalid physical address chosen!
→ Physical KASLR disabled: no suitable memory region!`), and the next unit is the memory map the
decompressor reads, not the CPU.

## The ONE next measurement

**What the decompressor reads when it chooses a physical address** — in `choose_random_location`
(`arch/x86/boot/compressed/kaslr.c`), the `mem_avoid`/`process_e820_entries` walk over the E820 the
firmware publishes: run the KASLR configuration with `V86C_WATCH` on the e820 table the kernel reads
(`boot_params.e820_table`, the same 0x2d0 slot the E820 unit measured) and on the decompressor's
candidate addresses, and read the first address it rejects — the rejection reason names the E820 entry
that made every candidate invalid, and that entry is the machine input to fix.
