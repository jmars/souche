# x86-64: the "linear→physical 32-bit drop" — MEASURED, AND IT IS NOT THERE

**Status: MEASUREMENT ONLY (4 runs of the pinned amd64 kernel at 256 MiB, no source change, no fix).
The hypothesis this unit was sent to test is REFUTED: at the stop there is no 32-bit truncation of a
linear address, the walk is correct, and the frame is inside RAM. The stop is a NULL per-cpu pointer
dereference at `0x1e08`, and the `-1` behind it was written by the KERNEL itself.** Claims tagged
MEASURED / READ / INTERPRETATION.

All four runs are deterministic: `4519091709 instructions`, `eip=0x890ef901`, `cr3=0x2a10000` every
time (MEASURED, `/tmp/drop{,2,3,4}.out`), so the instrument set does not perturb the run.

## (a) The walk for `0xffff8c16c0a0b000` (MEASURED, `V86C_CHAIN`, cr3=0x2a10000)

| level | entry phys | value | reading |
|---|---|---|---|
| PML4E | `0x2a108c0` | `0x4400067` | present, table `0x4400000` |
| PDPTE | `0x44002d8` | `0x4401067` | present, table `0x4401000` (no PS → not 1 GiB) |
| PDE | `0x4401028` | `0x8000000000a000e3` | present, **PS=1 (2 MiB page)**, NX set, frame `0xa00000` |

The chain printer then printed `PTE @0xa00058 = 0x0 (ABSENT)`. **That line is the INSTRUMENT's, not the
guest's**: `src/cpu/debug.c:358` tests `entry[lvl-1]` for PS at `lvl==2` (the PDPTE, a 1 GiB page) and
**never tests the PDE at `lvl==3`**, so it walks through a 2 MiB page to a PTE that is not part of the
translation. The real translation of `0xffff8c16c0a0b010` is `0xa00000 + (va & 0x1fffff)` =
**`0xa0b010`**. Any earlier unit that read "PTE absent" off this printer should re-check it.

## (b) Is that frame inside RAM? (MEASURED)

**YES.** `0xa0b010` = 10.4 MiB, inside the `[0x100000, 0x4000000)` usable entry the E820 describes
(`results/x86-64-e820-port.md`). The per-cpu area is inside the machine's RAM.

## (c) The fault, and its operands (MEASURED)

`V86C_WATCH` on `0xffff8c16c0a00000:0x10000` printed **exactly one** fault in the whole run:

    WATCH F 0x1e08 read rip=0xffffffff890ef901 (the access FAULTED, no store)

So the stop's fault is a read at **linear `0x1e08`** by `cmp 0x1e08(%rdx),%edi` with **rdx=0x0,
rdi=0x3** (stop dump). **`cr2=0xc0a0b010` is STALE**: no fault occurred anywhere in the 64 KiB per-cpu
window during the run, and `cr2` is the last #PF's address projected to 32 bits — which
`src/cpu/paging.c:152-158` does **by design** (`cr2_64 = addr; cr[2] = (int32_t)addr`). The "24-bit
truncation" and the "3.2 GB phys" the previous units read off `cr2` are that projection plus an older
fault, not a live address. (INTERPRETATION: the older fault is at an address whose low 32 bits are
`0xc0a0b010` — `0xffff8c16c0a0b010` is excluded by the watch, so it is a different VA with the same low
half, e.g. the un-randomised direct map `0xffff8880c0a0b010`.)

Upstream, both break points hit **once** each:

    BREAK 0 0xffffffff890ef8ec (after `mov %gs:0x76f3ec94(%rip),%eax`)  rax=0xffffffff
    BREAK 1 0xffffffff890ef8f9 (after `mov __per_cpu_offset(,%rax,8),%rdx`) rax=0xffffffffffffffff rdx=0x0

i.e. the cpu index read from a per-cpu slot was **-1**; `cltq` made it `0xffffffffffffffff`;
`__per_cpu_offset[-1]` read the 8 bytes *before* the table (`0xffffffff8aa393d8`) = **0**; the kernel
then dereferenced `NULL + 0x1e08`.

**Where the `-1` came from (MEASURED, one more run): the KERNEL wrote it.** Watching the two candidate
physical addresses of that slot — `0xa2e580` (correct: `gs_base 0xffff8c16c0a00000 + 0x2e580`) and
`0x2e580` (what a GS-base-dropping store would hit) — gave **0 hits at `0x2e580`** and, at `0xa2e580`:
two zeroing passes (byte stores of `0`, rips `0x8aeb36be`/`0x8aeafc2f`) then
`WATCH P 0xa2e580 width=4 value=0xffffffff rip=0xffffffff8ae93b96`. Nothing ever wrote the base-dropped
address, so **the store side is not dropping the GS base either** — the port stored and loaded the same
slot faithfully at both ends. (INTERPRETATION, link addresses at the delta `0x7E00000` this lane's
producer node measured: reader `0xffffffff810ef8e5`, writer `0xffffffff83093b96`.)

## The answer: (iii), the kernel's own computation — with the port exonerated at the fault

- **(i) walk index arithmetic — NO.** Indices `280/91/5/11` are right; the PDE is a valid 2 MiB page;
  the frame `0xa0b010` is inside RAM.
- **(ii) a 32-bit truncation of the address handed to the walk — NO at the fault.** The port's
  RIP-relative+GS computation (`src/cpu/modrm.c:823-843`) yields `gs_base + 0x2e580` for both the
  kernel's store and its load, matching the physical slot exactly; the fault address `0x1e08` is exact
  and the operand is `rdx=0`.
- **(iii) the kernel's own computation — YES.** The kernel read a per-cpu slot it had itself set to
  `-1`, used that as a CPU index, and dereferenced the NULL it got back. **The port is faithful here.**

## The ONE next measurement

**Boot the pinned kernel on QEMU at 256 MiB with the same cmdline and compare the `percpu:` lines
(the port's serial prints `percpu: cpu 0 has no node -1 or node-local memory`, after
`No NUMA configuration found` / `Faking a node`, and its machine has no ACPI RSDP at all).** If QEMU
reaches the same code with the same `-1`, the kernel does this on a reference machine too and the next
defect is elsewhere; if QEMU's slot is `0`, the difference is an *input* our firmware does not publish
(cpu→node / SRAT), and the measurement becomes a diff of the two machines' ACPI/NUMA inputs — not of
the address path, which this unit has now cleared.
