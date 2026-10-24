# x86-64: PEEK was never broken — it is gated on a BREAK, and what it reads is ZEROS

**Status: INSTRUMENT EXPLAINED + ONE MEASUREMENT (two 256 MiB boots; no source change, no fix). `V86C_PEEK`
speaks as soon as a break fires; and the eight bytes it reads at the return-address slot are **all
zeros**, which is a new and larger fact than the caller's identity.** Claims tagged MEASURED / READ /
INTERPRETATION.

## 1. Why the instrument was silent (READ, `src/cpu/debug.c:287-289`)

    { const char *c = getenv("V86C_BREAK_CHAIN");
      if (c != NULL) dump_chain(...); }
    dump_peek();            /* ← INSIDE v86c_dbg_break_hit(), not v86c_dbg_on_stop() */
    dump_peek_sp();

**`dump_peek()`/`dump_peek_sp()` are called from the BREAK-hit handler**, so `V86C_PEEK` prints only when a
breakpoint fires. Every recent run that reported "PEEK silent" set `V86C_DUMP_ON_STOP` and a watch/chain but
**no `V86C_BREAK`** — the instrument was never armed, not broken. (The earlier nodemask runs that *did* print
PEEK also set a break; the correlation was in the data and I misread it as an instrument failure.)

**MEASURED, the fix is the invocation, not the code:** with `V86C_BREAK='0xffffffffa91e42c6:1'` (the
faulting instruction, one hit) and `V86C_PEEK='0xf802270:8'`:

    v86c dbg: PEEK phys 0xf802270 (8 bytes): 00 00 00 00 00 00 00 00

## 2. What the read says (MEASURED)

The return-address slot is `rsp + 0x2a8 + 0x20` (four pushes, `push r13/r12/rbp/rbx`, then `sub $0x2a8`) =
`0xffffd1fac0000288`; the chain maps that page to frame `0xf802000` (`PTE 0x800000000f802063`), so the bytes
are at physical `0xf802288`. A 64-byte window covering `0xf802270…0xf8022af` — the pushed registers, the
return address and the frame above it — reads **sixty-four zero bytes**:

    PEEK phys 0xf802270 (64 bytes): 00 00 … 00

**So the kernel's own pushes onto that stack are not present in the frame the page tables map for it.** The
canary store (`mov %gs:0x28,%rax; mov %rax,0x2a0(%rsp)` → `0xffffd1fac0000260`) is in the same page, and the
page is otherwise empty.

## 3. One sentence

**The differing input is not named yet, and the reason is new: the port's writes to that stack address do
not appear in the physical frame the walk resolves for it — so before any question about the argument's
provenance, the port's write path and its walk disagree about which frame that linear address lives in.**

## 4. No fix

The evidence does not yet separate three possibilities — a stale TLB entry on the write path, a walk that
resolves a different frame than the write used, or a stack the kernel never actually wrote to (i.e. the
return address is zero because the call never happened and the frame is stale). Landing anything here would
be a guess.

## The ONE next measurement

**Watch the frame the kernel writes, and compare it with the frame the walk resolves.** `V86C_WATCH` on
`0xf802000:0x1000` (the frame the PTE names) **and** `V86C_BREAK` at the function's entry
(`0xffffffffa91e4290`, one hit) with `V86C_PEEK` on `0xffffd1fac0000260:8` (the canary slot): if the watch
records no writes to `0xf802000` while the kernel is demonstrably pushing there, the write path and the
walk disagree and the `rip` of the writes that *do* occur names the frame actually used — which is the
defect, and it is one link from a fix.
