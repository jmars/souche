# x86-64: the mid-instruction reading was wrong — the rsp jump happens INSIDE an instruction slot

**Status: MEASURED, no fix. This supersedes results/x86-64-midinsn.md §3-4: the guest's text at
`0xffffffff815bdf65` IS a valid instruction boundary (the kernel patches its own text), so the
"resume at a non-boundary address" reading is WITHDRAWN. What remains, measured, is a stack-pointer
change that no instruction at that address can make. Claims tagged MEASURED / READ / INTERPRETATION.

## 1. The boundary question is settled (MEASURED)

Two runs with the guest's own bytes in view:

    BREAK 0 0xffffffffa8d3df65 (hit 1) ... rsp=0xffffd1fac056ba58 cr2_64=0xffff8eaa88a00000
    RSPLOW ... rip=0xffffffffa8d3df65 rsp=0x58ba56c0 delta=-0xffffd1fa679c6398

and the ring shows the patched stream decoding cleanly, each instruction starting where the previous
one ends:

    df57  89 44 24 18              mov %eax,0x18(%rsp)
    df5b  41 0f cd                 bswap %r13d
    df5e  89 44 24 68              mov %eax,0x68(%rsp)
    df62  8b 46 10                 mov 0x10(%rsi),%eax
    df65  41 0f cc                 bswap %r12d          <- rsp is still 0xffffd1fac056ba58 here
    df68  0f cd                    bswap %ebp
    df6a  44 89 6c 24 6c           mov %r13d,0x6c(%rsp) <- faults, on the wrong stack

The image's bytes at `df65` (`00 00 00 48 89 fb` from objdump) are NOT what the guest executes: the
kernel's alternatives/static-call machinery has rewritten this code (the `bswap` stream is the
in-kernel variant), which is normal and expected. **My earlier "the entry point is the finding" claim
was an artifact of comparing the guest's bytes against the unpatched image.**

## 2. What is actually wrong (MEASURED)

`bswap %r12d` cannot change `rsp`, and neither can `mov 0x10(%rsi),%eax` before it. Yet after that
instruction slot `rsp = 0x58ba56c0` — an address that is neither the kernel stack
(`0xffffd1fac056ba58`) nor reachable from it by a push. The reviewer's RSPLOW-attribution note
(their m4) is the shape that explains it: **the tick that records the event runs after
`run_instruction`, so anything the *delivery* did inside the slot is attributed to the instruction.**
So a fault was delivered in that slot and the delivery left `rsp` at `0x58ba56c0`.

What the slot's stores show (MEASURED, `WATCH V -> P` on the stack page):

    WATCH V 0xffffd1fac056ba70 -> P 0x284da70  value=0xffffffff80000000  rip=…df5b
    WATCH V 0xffffd1fac056bac0 -> P 0x284dac0  value=0xffffffff80000000  rip=…df62
    (frame for that stack page = 0x284d000)

`0x58ba56c0` is not the low 32 bits of `0xffffd1fac056ba58` (`0xc056ba58`) nor of the frame
(`0x284d000`), so the earlier "truncated stack address" shape does NOT explain this one. It is a value
that must come from somewhere else in the slot — a register, a TSS/IST field, or a mis-executed
delivery.

## 3. The ONE next measurement

**Log the register file at the jump, not just rsp.** Extend the rsp tool with a large-delta event
(`V86C_RSPJUMP=K`, default off, K = threshold, default e.g. `0x1000`): when one instruction slot
leaves `rsp` differing from the rsp it started with by more than K, print `rip`, the entry and exit
`rsp`, the delta, and the **full register file, cs/ss, cr2_64 and cpl** — and record it into the same
ring, so the last K survive to the stop. One such line names the source register or the delivery that
wrote `rsp`, and its `cr2_64` says whether a fault happened in that slot.

That is a small addition to `src/cpu/debug.c` + the seat at `src/cpu/cpu.c:204` (the same seat RSPLOW
uses, which already has the entry rsp available if the tool keeps it), with a selftest arm that fails
against a variant which records only the delta.
