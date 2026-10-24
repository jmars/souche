# x86-64: QEMU's stack does NOT cross the 1 GiB boundary — the port's stack pointer is the defect

**Status: MEASUREMENT (one port boot with the 256 MiB firmware, one QEMU boot at the same geometry and
the same instruction; no source change, no fix). The decisive comparison answers the brief's question: the
kernel's placement is fine on a reference machine, so the crossing is the PORT's stack pointer.** Claims
tagged MEASURED / READ / INTERPRETATION.

## 1. The same instruction on both machines (MEASURED)

Breakpoint at link **`0xffffffff819e42c6`** (`movq $0x0,0x6(%rsp)`), same kernel, 256 MiB, `nokaslr` on
QEMU (so the link address is directly reachable):

| | `rsp` | 1 GiB region base | offset of `rsp` | stack top | offset of top |
|---|---|---|---|---|---|
| **QEMU** | `0xffffc900000137c8` | `0xffffc90000000000` | **`0x137c8`** | `0xffffc90000013b81` | `0x13b81` |
| **port** | `0xffffd1fabfffffc0` | `0xffffd1fa80000000` | **`0x3fffffc0`** | `0xffffd1fac0000382` | `0x382` (in the NEXT region) |

**QEMU's stack sits ~80 KiB INTO its 1 GiB region, with its top 0x3b9 above `rsp` — no crossing. The
port's `rsp` is 0x40 bytes BELOW the boundary and its top is 0x382 bytes ABOVE it — a stack that
straddles two 1 GiB regions, only the upper of which the kernel maps.** (The port's `rsp` is also ~0x40
from the very bottom of its region, whereas QEMU's is 0x137c8 in: the two stacks are not at the same
offset within their region at all.)

## 2. The chains (MEASURED, `V86C_CHAIN`, port)

    va=0xffffd1fac0000000  idx=419/491/0/0     (the UPPER region — the stack's top)
      PML4E @0x7010d18 = 0x1000067   PDPTE @0x1000f58 = 0x11b6067
      PDE   @0x11b6000 = 0x11b7067   PTE   @0x11b7000 = 0x800000000f802063  (present, frame 0xf802000)

The upper region is mapped all the way to a 4 KiB page; the lower one (`…bfffffc0`, index 490) is the one
whose PDPTE the census proved is **never written** (`results/x86-64-ptcensus.md`, `-stacksrc.md`).

## 3. Instrument note (an instrument's silence is not evidence)

The per-cpu-slot watch was aimed at physical `0xa199e0` on the assumption that the per-cpu area sits at
`page_offset_base + 0xa00000`; this run's `gs_base` is `0xffff8eaa8f800000`, a different
`page_offset_base` from the earlier runs, and **the watch produced no hits at all** — the address was
wrong, not the guest silent. Nothing in this report rests on it.

## 4. The answer

**QEMU does not cross; the port does, at the same instruction, with the same kernel and the same
geometry. So the kernel's stack placement is not at fault — the PORT's stack pointer is, and (2) of the
brief is the right next question: its producer.** (The kernel's placement being fine also retires the
"this image cannot boot on either machine" branch.)

## The ONE next measurement

**The producer of that stack pointer.** `cpu_init`'s prologue reads the per-cpu slot at `gs:0x199e0` (the
disassembly at link `0xffffffff819e42b5` is `mov %gs:0x7ef9b124(%rip),%ebx` → effective offset `0x199e0`,
and the operand is **32-bit**, so the port's load of that field is the first thing to check against the
field's real width). Watch that slot for the store that sets it — **at the right physical address** this
time: `phys = (gs_base − page_offset_base) + 0x199e0`, with both values taken from the same run's dump —
and read the rip and value of every write. If the value stored is already straddling, the defect is in
whatever computed it (`thread.sp`/`copy_thread`); if the stored value is sane and the load truncates it,
the defect is the load.
