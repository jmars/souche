# x86-64: the guest overflows its OWN 16 KiB stack — rsp is 0x40 below a mapped region

**Status: five boots, MEASURED. The previous unit's headline is REFUTED (the write path and the
walk do not disagree), and the guest's blocker is now a locatable shape: a monotone stack descent
of ~0x700 per level, ~9 levels, off the bottom of a correctly mapped `THREAD_SIZE` region.**
Claims tagged MEASURED / READ / INTERPRETATION. No source change in the guest's path (the one
change this unit needed was the instrument, commit `5178acd`).

## 0. The instrument that had to speak first (commit `5178acd`)

`V86C_WATCH` could not see ANY long-mode store's physical destination: the hook in
`safe_write{8,16,32,64}_addr` ran BEFORE `translate_address_write()` and reported only the linear
address, so a watch armed on a physical frame (the documented way to watch one) matched nothing.
The hook now runs after the walk, after the pagefault guard, and prints both:
`WATCH V 0x<va> -> P 0x<phys> width=.. value=.. rip=..` (selftest TEST 1h, three mutants fail,
the real one passes). **Trap 1 of results/x86-64-handoff.md §7, sixth instance** — and it is the
one that blocked the measurement results/x86-64-peekfix.md asked for.

## 1. The write path and the walk AGREE (MEASURED, refuting the previous unit)

`--bios build/minibios-256m.bin --memory 256`, `V86C_WATCH='0xffffd1fac0000260:0x30'` (the slot
range the frame uses) plus the physical frame, `V86C_BREAK` on the function's entry and on its
faulting instruction. 13,761 stores on that range, every one carrying its frame:

    WATCH V 0xffffd1fac0000280 -> P 0xf802280 width=8 value=0xffffffffa9b3332a rip=0xffffffffa91e4292
    WATCH V 0xffffd1fac0000278 -> P 0xf802278 width=8 value=0xffffd1fac0000380 rip=0xffffffffa91e4297
    WATCH V 0xffffd1fac0000270 -> P 0xf802270 width=8 value=0xffffd1fac0000360 rip=0xffffffffa91e429e
    WATCH V 0xffffd1fac0000268 -> P 0xf802268 width=8 value=0xffffffffa9b3332a rip=0xffffffffa91e429f
    WATCH V 0xffffd1fac0000260 -> P 0xf802260 width=8 value=0x44f9e1edfe931c00 rip=0xffffffffa91e42ba

The four prologue pushes and the canary land in the frame the walk resolves, value for value.

**Why the previous unit read sixty-four zeros (MEASURED):** `V86C_PEEK` prints from the BREAK
handler, and the break it used (`0xffffffffa91e42c6:1`) fired at **hit 1**, when this run's stack
was at `rsp=0xffffd1fac00037c0` — so the slot it read (`phys 0xf802260`) was not a slot the frame
was using at that moment. `PEEK SP` at the same break shows the live frame correctly at
`0xffffd1fac0003788`. The bytes were zeros because the address was not yet in use, not because a
store went elsewhere. **An instrument reading a correct value at the wrong time is the same trap as
an instrument that cannot speak.**

## 2. The fault is the function's own store, on an ABSENT PUD entry (MEASURED)

`V86C_WATCH='0xffffd1fabfffffc0:0x40'`, `V86C_CHAIN` at the stop:

    WATCH F 0xffffd1fabfffffc6 write rip=0xffffffffa91e42c6 (the access FAULTED, no store)
    chain va=0xffffd1fabfffffc0 cr3=0x7010000 idx=419/490/511/511
      PML4E  @0x7010d18 = 0x1000067
      PDPTE @0x1000f50 = 0x0  (ABSENT)

**Nothing ever stored to that page before the fault** — the page below the boundary is entered for
the first time by the access that faults. The access is `movq $0x0,0x6(%rsp)`, the function's own
frame initialisation.

## 3. The PUD entry was never written — and the watch proves it can see one (MEASURED)

`V86C_WATCH='0x1000f50:8,0x1000f58:8'` (slot 490 = the region containing the faulting page; slot
491 = the region above it, which IS mapped):

    WATCH V 0xffff8eaa81000f50 -> P 0x1000f50 width=8 value=0x0          rip=0xffffffffa91eb95d
    WATCH V 0xffff8eaa81000f58 -> P 0x1000f58 width=8 value=0x0          rip=0xffffffffa91eb961
    WATCH V 0xffff8eaa81000f58 -> P 0x1000f58 width=8 value=0x11b6067    rip=0xffffffffa887e5f3

Slot 490 is zeroed once, when the PUD page is created, and **never written again**; slot 491 gets
its mapping from `0xffffffffa887e5f3`. The asymmetry is the control: the same watch sees the real
mapping one slot over, so the absence in 490 is a fact about the guest's page tables, not blindness.

## 4. The stack region is FOUR pages, fully mapped (MEASURED, four boots)

One chain per page, same configuration, `cr3=0x7010000`:

| page | PTE | frame |
|---|---|---|
| `0xffffd1fac0000000` | `0x800000000f802063` | `0xf802000` |
| `0xffffd1fac0001000` | `0x800000000f803063` | `0xf803000` |
| `0xffffd1fac0002000` | `0x800000000f804063` | `0xf804000` |
| `0xffffd1fac0003000` | `0x800000000f805063` | `0xf805000` |

A contiguous `0x4000` = 16 KiB = `THREAD_SIZE` stack region, entirely present, ending at
`0xffffd1fac0004000`. So the mapping is not the defect and the region is not one page: **`rsp` at
the stop, `0xffffd1fabfffffc0`, is `0x40` bytes BELOW the region's base, i.e. the guest has
overflowed its own stack.** QEMU's stack pointer for the same code path
(`0xffffc900000137c8`) is `0x838` into its region — the port's guest consumed ~16 KiB where the
reference consumed ~2 KiB.

## 5. The descent is monotone, ~0x700 per level, ~9 levels (MEASURED)

`V86C_BREAK='0xffffffffa91e4290:1'` (the function entry) with `V86C_BREAK_MAX=4000`: 3457 entries,
the minimum entry-`rsp` over time (the routine oscillates between two frames in between):

| hit | min rsp | level |
|---|---|---|
| 1 | `0xffffd1fac0003a88` | 0x578 used |
| 97 | `0xffffd1fac0003388` | |
| 292 | `0xffffd1fac0002be0` | |
| 580 | `0xffffd1fac00024e0` | |
| 1189 | `0xffffd1fac0001b08` | |
| 1717 | `0xffffd1fac0001408` | |
| 2366 | `0xffffd1fac0000bc8` | |
| 3086 | `0xffffd1fac00004c8` | |
| 3457 | `0xffffd1fac0000288` | 0x3d78 used, then `sub $0x2a8` overflows |

Between descents the entry `rsp` alternates `0xffffd1fac0002c18` / `0xffffd1fac0002be0` — a caller
and its callee — i.e. the function is called in a LOOP at a fixed depth while the depth FLOOR drops
by ~0x700 every ~300 entries and **never recovers**. A leak of ~0x700 per level with no return is
the shape of nested interrupt/exception delivery (both the hardware frame and the C handler's own
frame stay on the stack), nine levels of which exhaust 16 KiB. It is not plain recursion inside this
function: the descending steps are far larger than this function's `0x2c8` frame.

**INTERPRETATION (the hypothesis the next measurement must refute):** the port re-delivers an
interrupt (or the guest's handler faults in a way that nests a delivery) before the previous
delivery returns, and each level costs ~0x700 of stack. Nothing in this unit identifies the vector.

## 6. What this changes

- The lane's headline question moves from "the port's stack pointer is wrong" to "**the port's
  guest takes ~9 nested deliveries where the reference takes none**". The stack pointer is a
  symptom; the region, the tables and the write path are all correct (MEASURED, §1, §3, §4).
- `results/x86-64-peekfix.md` §3's one sentence ("the port's writes to that stack address do not
  appear in the physical frame the walk resolves") is **withdrawn**: they do appear, and the zeros
  were a hit-1 timing artifact (§1).

## 7. The ONE next measurement

**Name the deliveries.** The toolkit cannot see interrupt delivery at all, which is why this shape
had to be inferred from stack arithmetic: add a trace seat (env-gated, default off, same discipline
as the rest — `V86C_INTS=N`) that logs, for every delivered interrupt/exception: **vector, the
`rsp` at delivery, `cs`/`cpl`, the interrupted `rip`, and a running count**, from the one place in
`src/cpu/exceptions.c` where a gate is accepted, and bounded by `N`. Run it on the same
configuration and read the last ~40 lines. If the same vector (or a fault vector, 14, delivered
while delivering) repeats with `rsp` falling by ~0x700 each time, the storm is named, and the fix
is wherever that vector is left pending after the handler's EOI/ack. If instead the trace shows no
nested delivery at all, the ~0x700-per-level leak is in the guest's own call chain and the trace's
`rip` sequence names the loop.
