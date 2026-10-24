# x86-64: the virtual peek INJECTED a guest exception — an instrument that changed the run

**Status: MEASURED, fix landed and proved by asymmetry. `V86C_PEEK_SP` walked the guest's page
tables with the side-effecting `translate_address_read()`, which on a missing page calls
`trigger_pagefault()` (`src/cpu/paging.c:201-215`) and `trigger_pagefault()` **delivers the #PF**
(`call_interrupt_vector`, `paging.c:139-170`). So a peek that ran off an unmapped page pushed a real
IA-32e exception frame onto the guest's own stack, mid-diagnostic. The dump now uses
`translate_address_read_no_side_effects()` (`paging.c:760`), which sets the flag and delivers
nothing. Claims tagged MEASURED / READ.

## 1. The observation (MEASURED)

Two runs of the SAME binary and configuration, differing ONLY in the peek's length:

| run | `V86C_PEEK_SP` | break hits at `0xffffffffa91e4290` | stop |
|---|---|---|---|
| w5 | `8` | 3457 | `unimplemented`, rip `0xffffffffa91e42c6`, **4,590,902,204 instructions** |
| w7 | `2048` | 69 | **rc 0**, `unimplemented=0`, rip `0xffffffffa924f6e5`, **6,000,054,617 instructions** |

A debug read cannot change a deterministic machine — so this was a defect, not noise. w7's stop dump
carried the fingerprint: `cr2_64=0xffffd1fac0004000`, which is the TOP of the guest's 16 KiB stack
region — an address nothing in the guest had faulted on, and exactly where a 2048-byte dump starting
near the stack top runs off the end of the region.

## 2. The mechanism (READ)

`dump_peek_sp()` saved and restored `cpu->pagefault` and its comment claimed "A fault here is
swallowed". It is not: the side-effecting walk **delivers**. `do_page_walk(..., side_effects=true,
...)` calls `trigger_pagefault(...)` on every absent/invalid level, and `trigger_pagefault` records
CR2, invalidates the TLB entry and calls `call_interrupt_vector(CPU_EXCEPTION_PF, ...)` — the guest's
#PF handler runs, on the guest's stack, with the guest's `rip` set to `previous_ip`. Restoring
`cpu->pagefault` afterwards undoes none of that. The `no_side_effects` variant was already in the
tree (`paging.c:760`) and does set `cpu->pagefault`; it just does not deliver and does not write the
A/D bits.

**This also revises the toolkit's trap list: an instrument can be wrong in the OTHER direction — not
silent, but loud in a way that changes the subject.** Every reading in this lane taken through
`V86C_PEEK_SP` is only as good as whether it stayed inside a mapped page; the ones in
results/x86-64-stackoverflow.md §5 used 8 bytes at frame entry, which is inside, and their
instruction counts match the uninstrumented runs (4,590,902,204).

## 3. The fix, and its asymmetry (MEASURED)

`src/cpu/debug.c`: `dump_peek_sp()` now walks with `translate_address_read_no_side_effects()`. The
peek cap also moved 64→4096 / 128→4096 bytes (`dump_peek`, `dump_peek_sp`): the ancestor chain of a
nesting guest is a page deep, and 128 bytes read none of it.

The proof is the asymmetry, run after the fix with w7's **exact** command line:

    V86C_BREAK='0xffffffffa91e4290:1' V86C_BREAK_MAX=3500 V86C_PEEK_SP=2048 V86C_DUMP_ON_STOP=1
    -> 3457 break hits, rc 1, unimplemented=1, rip 0xffffffffa91e42c6, 4,590,902,204 instructions

Byte-for-byte the trajectory of the runs that used no peek at all. Before the fix, the same command
produced 69 hits and a 6,000,054,617-instruction run. `make` rc 0, `make selftest` rc 0.

**No test arm accompanies this**: an arm would need a paging fixture that faults and a captured
exception state, and the guest run above already fails without the fix and passes with it — that is
the asymmetry, and it is pinned by the configuration in this file.

## 4. The ancestor chain, read safely (MEASURED, w8 = the fixed run)

`V86C_PEEK_SP=2048` at the function's entry (`rsp=0xffffd1fac0000288`), 2048 bytes with no fault;
kernel-text values, ascending:

    +0x0000 0xffffffffa91e74b8   the caller (one site, every one of the 3457 calls)
    +0x0058 a891dbb1  +0x0078 a891d9aa  +0x0130 a891dffc  +0x0188 a91fcdb3
    +0x01c8 a891eb8d  +0x01e8 a91f0d3d  +0x0200 a91f0d5c  +0x0218 a8830d63
    +0x0280 a88364ad  +0x02a0 a8887302  +0x02b0 a894bb15  +0x02b8 a8a029cb
    +0x0320 a923d3aa  +0x0348 a9400c02
    +0x0620 a8887329  +0x0630 a894bb15  +0x0638 a8a029cb
    +0x06a0 a923d3aa  +0x06c8 a9400c02   <-- the SAME five frames again, +0x380 higher
    +0x0750 a891eb8d  ...

**The chain is self-similar: `a88873xx → a894bb15 → a8a029cb → a923d3aa → a9400c02` repeats with a
stride of `0x380`, and it never unwinds.** That is the shape of a recursion through a fixed code
path, two of whose levels make the `0x700` descent measured in results/x86-64-stackoverflow.md §5 —
i.e. the guest is looping through the same handler path and consuming stack at every iteration until
the 16 KiB region is gone. `a9400c02` is the deepest repeated frame and the widest stride.

Symbols could not be attached yet: the pinned `.deb`'s `boot/System.map-6.1.0-50-amd64` is a decoy
file whose only line is *"The real System.map is in the linux-image-<version>-dbg package"*
(READ, `/tmp/debx/boot/System.map-6.1.0-50-amd64`). Naming these frames needs either the `-dbg`
package or a kallsyms parse of the image.

## 5. The ONE next measurement

**Name the deliveries.** The repeated chain is consistent with a fault recursion whose handler path
re-enters itself, and the toolkit is still blind to exception delivery — it cannot say which vector,
from which `rip`, at which depth. Add `V86C_INTS=N` (env-gated, default off): one line per delivery
at the delivery seat in `src/cpu/exceptions.c`, carrying **vector, `is_int`, error code, `cpl`,
`delivery_depth`, the interrupted `rip` (`cpu->previous_ip`), `rsp` before and after the frame push,
and a running count**, bounded by `N`. Run the same configuration and read the last 40 lines: if
vector 14 (or 13/8) repeats with `delivery_depth` climbing and `rsp` falling by ~0x380, the recursion
is fault-driven and the fix is in what the guest's handler does not clear — and the four repeated
frames name the site. If delivery is flat (depth 0/1 throughout), the recursion is the guest's own
call chain and `a9400c02` is where to disassemble.
