# x86-64: the stack's own 1 GiB region is never populated — the kernel maps the one ABOVE it

**Status: MEASUREMENT ONLY (three port boots — 256 MiB firmware — plus the QEMU comparison from the
previous unit; no source change, no fix). The producer is not yet named: the per-cpu slot was never
observed being written, and the load's width is not proven. What IS new and decisive: in the 256 MiB run
the kernel writes exactly ONE PDPT entry in that row — the region ABOVE the stack's — and never the
stack's own.** Claims tagged MEASURED / READ / INTERPRETATION.

## 1. The arithmetic the brief points at (MEASURED)

| | stack top | 1 GiB region base | top − base |
|---|---|---|---|
| **QEMU** | `0xffffc90000013b81` | `0xffffc90000000000` | `0x13b81` |
| **port** | `0xffffd1fac0000382` | `0xffffd1fa80000000` | **`0x40000382`** — i.e. **exactly a 1 GiB boundary, +0x382** |

The port's stack top sits on the boundary `0xffffd1fac0000000` (= `region_base + 0x40000000`); its `rsp`
is 0x40 below it, in the **lower** region. QEMU's whole stack is 0x13b81 into one region.

## 2. The kernel never populates the stack's region (MEASURED, 256 MiB run)

Watching the PDPT row that holds both regions (`V86C_WATCH='0x1000f40:0x20'`, indices 488–491):

    WATCH P 0x1000f58 width=4 value=0x11b6067 rip=0xffffffffa887e5f3     ← index 491, the region ABOVE the stack

**Index 490 — the region containing `rsp` and the stack's own pages — is never written non-zero** (the
same shape as the 64 MiB run's index 27 vs 26, `results/x86-64-ptcensus.md`). The chain for the upper
region resolves to a real 4 KiB page (`PTE 0x800000000f802063`, frame `0xf802000`); the stack's region has
no PDPTE at all, so nothing in it can be walked.

## 3. The producer: what the evidence does and does not say

- The value at the fault is `rbx = 0xffffd1fac0000382`, i.e. **64-bit** — so whatever loaded it did not
  truncate it (the `mov %gs:0x199e0(%rip),%ebx` at link `0xffffffff819e42b5` that the earlier disassembly
  shows is a 32-bit form, so **either that is not the load that produced `rbx`, or the disassembly is of a
  different point in the function** — the width is NOT proven either way, and the brief's width hypothesis
  is therefore neither confirmed nor excluded).
- The per-cpu-slot watch at physical `0xa199e0` (derived as `gs_base − 0xa00000 + 0x199e0` with
  `gs_base = 0xffff8eaa8f800000`) produced **no hits at all** — the derivation rests on an assumed
  per-cpu offset of `0xa00000`, which this run does not print. **Instrument note, not evidence.**

## 4. No fix

The evidence names a *symptom* precisely (a stack top exactly on a 1 GiB boundary whose lower region is
never mapped) but not the term that put it there: the store that sets the per-cpu value was never
observed, and the load's width is unproven. Fixing anything here would be the "fix on a hypothesis" the
brief forbids.

## The ONE next measurement

**Read the per-cpu slot's value and catch the load, at the same code point.** Break at link
`0xffffffff819e42b5` (the `gs:`-relative load) **and** at `0xffffffff819e42bc` (the instruction after it)
with the dump on, in the 256 MiB configuration: the pair gives the loaded value and its width directly
(`rbx` before/after), and the dump's `gs_base` gives the slot's physical address with no assumption —
`phys = gs_base − page_offset_base + 0x199e0`, where `page_offset_base` is the value the *kernel itself*
prints in that run (it can be recovered from the dump's `gs_base` by subtracting the per-cpu offset the
same run's `__per_cpu_offset` table holds). With the slot's physical address known exactly, watch it for
the store that sets it and read that store's `rip` and value — that is the producer.
