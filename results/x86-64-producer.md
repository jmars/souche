# x86-64: the producer named — and the previous unit's conclusion retracted

**Status: MEASUREMENT + ONE TOOLKIT FIX, AND A RETRACTION OF MY OWN PREVIOUS REPORT.
`page_offset_base` is written **correctly**; `results/x86-64-halves.md`'s "wrong low half" reading
was wrong. What is actually wrong is downstream: the **physical** addresses are truncated to 24 bits,
and they are also **beyond the guest's RAM**, which is a second finding.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The instrument had to be fixed first (MEASURED)

`V86C_WATCH` on the global's virtual address, on its neighbours, and on two plausible direct-map
aliases all reported **nothing** — while a PEEK of the same physical slot, early and late in **one
run**, showed the value change:

    PEEK 0x2413d60 at copy_bootdata (early):  0xffff888000000000
    PEEK 0x2413d60 at the per-cpu store (late): 0xffff8c16c0000000

Every instruction body goes through `safe_write*` (checked: no body writes RAM directly), so the
silence was the **instrument's**: the watch hooked the virtual store path only. **Landed:** the
physical writers (`write8/16/32`, `write64_no_mmap_or_dirty_check`, `mmap_write64`) now call
`v86c_dbg_watch_p` when the toolkit is on — one load and one branch when off.

## 2. The producer (MEASURED)

`V86C_WATCH=0x2413d60:8` (the **physical** address) now speaks:

    WATCH P 0x2413d60 width=4 value=0xffffffffc0000000 rip=0xffffffff8ae9ef45
    WATCH P 0x2413d64 width=4 value=0xffffffffffff8c16 rip=0xffffffff8ae9ef45

Two **32-bit** stores of the two halves, from `rip` link `0xffffffff8309ef45`. The code that builds
the value (link `0xffffffff8309ef31`…`0x8309ef42`):

    div %rcx ; mov (%r14),%rcx
    and $0xffffffffc0000000,%rdx      ← PUD_MASK
    lea (%rdx,%rbx,1),%rax            ← + __PAGE_OFFSET_BASE_L4
    mov %rax,(%rcx)

That is `kernel_randomize_memory`: `page_offset_base = __PAGE_OFFSET_BASE_L4 + ((rand % n) &
PUD_MASK)` — **and the port computes it correctly.**

## 3. RETRACTION (MEASURED)

`results/x86-64-halves.md` said a legitimate randomized `page_offset_base` "must be 1 GiB-aligned
with a **zero low 32 bits**" and called `0xc0000000` in the low half a defect. **That is wrong.** A
PUD is 1 GiB = `0x40000000`, so a PUD-aligned address has its low **30** bits zero and **bits 30–31
may be set**. `0xffff8c16c0000000` is a legitimate value (`__PAGE_OFFSET_BASE_L4 + 3 GiB`), the store
is correct, and the "wrong low half" conclusion is withdrawn. The half of that report which stands is
its **measurement chain** — the per-cpu base, the `cpu_number` garbage, the NULL per-cpu pointer, the
`gdtr + 0x10` delivery fault — all of which were measured, not inferred.

## 4. What is actually wrong (MEASURED)

Two facts, both from that same chain:

1. **The physical addresses are truncated to 24 bits.** The per-cpu base's physical offset is
   `0x00fd4000` where `0xc0fd4000` is required, and the direct-map PDE maps `0x00e00000` where
   `0xc0e00000` is required. In both cases the top byte (`0xc0`, bits 31…24) is dropped — and
   `0xc0e00000 & 0x00FFFFFF = 0x00e00000` exactly.
2. **Those physical addresses are outside the guest's RAM.** `0xc0e00000` and `0xc0fd4000` are 3.2 GB
   in a 256 MiB guest, so the bootmem allocation behind them returned a physical address that does
   not exist. **INTERPRETATION:** (2) is the upstream fact and (1) is a consequence — a phys beyond
   the model's 32-bit RAM bound is exactly the sort of value this lane's earlier units have seen the
   *port* clamp or project, and a 24-bit drop is what a byte-level or 8-bit-shift path produces.

**So the next fork is the allocator, not the randomization:** why `__alloc_bootmem_node_nopanic`
(link `0xffffffff8309d94f`, from `results/x86-64-halves.md`'s own disassembly) returns `0xc0fd4000`
in a 256 MiB guest, and where the top byte is dropped on the way to the page tables.

## 5. Verification (MEASURED; **toolkit code changed**, full battery, by EXIT CODE)

    make / selftest / nasm / nasm64        rc 0 (selftest: all suites green; nasm64 13/0)
    make bench                             200095214
    make transcript-diff                   byte-identical to the reference
    make determinism                       two consecutive runs byte-identical
    gate-x86-64                            14 passed, 0 failed   (the banner gated)
    gate-long-mode 57/0 · gate-long-mode-sys 5/0 · gate-x86-64-diff 13/0
    gate-reboot-1 39/0 · gate-pae-1 11/0 · gate-pae-l3 0 FAILING · gate-vga-0..4 0 FAILING

**No pin moved** — the added hooks are behind `v86c_dbg_watch_on`, which the gates leave off.

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/memory.c` | the physical write hook (`write8/16/32`, `write64_no_mmap_or_dirty_check`, `mmap_write64`) |
| `results/x86-64-producer.md` | this report, including the retraction |
| `TODO.md` | §8 |
