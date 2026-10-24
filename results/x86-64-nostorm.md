# x86-64: NO delivery storm — 125 exceptions in the whole boot, and the "caller" was a tail jump

**Status: MEASURED with the new `V86C_INTS` trace (commit `5a39eb9`). The hypothesis the last
report handed to this unit — that the guest nests interrupt deliveries ~9 deep and that this is
what eats its 16 KiB stack — is REFUTED. The delivery cascade at the stop is a CONSEQUENCE of the
stack overflow, not its cause. And the return address that three previous units read off the stack
does not name the caller of the function they were breaking in, because the function is reached
through a TAIL JUMP.** Claims tagged MEASURED / READ / INTERPRETATION.

## 1. The delivery census (MEASURED, one boot)

    V86C_INTS=60 V86C_FAULT_DELIVERY_BOUND=8 V86C_DUMP_ON_STOP=1
    -> INTS total=125 max_depth=7 printed=60
       INTS vectors: vec=16 x51  vec=19 x32  vec=14 x30  vec=21 x8  vec=22 x2  vec=48 x2

**One hundred and twenty-five deliveries in 4,590,902,204 instructions.** The largest counts are
vectors 0x10, 0x13, 0x0e — the kernel's own exception vectors in early boot — and every printed
line carried `depth=0`. `max_depth=7` occurs only at the end, in the cascade below.

**What this rules out (MEASURED):** the ~0x700-per-level descent is not nested delivery. Nine
levels of delivery nesting would have shown as nine lines with climbing `depth`; the trace shows
one delivery every ~37 million instructions and depth 0 throughout the descent's region of the
boot. The earlier INTERPRETATION in results/x86-64-stackoverflow.md §5 is withdrawn.

## 2. The stop is a consequence, not a cause (MEASURED)

At the stop the port refuses an 8-deep cascade (`FAULT_DELIVERY_BOUND=8`; the stop text still says
"four times" because the message is hard-coded — READ, src/cpu/exceptions.c:557). The first fault of
that cascade is the guest's own store at `0xffffd1fabfffffc6` (results/x86-64-stackoverflow.md §2),
and every delivery of a #PF pushes an IA-32e frame **onto the same stack that just overflowed**, so
each delivery faults again. That is the textbook double-fault spiral, and it is downstream of the
overflow. **Anyone reading this lane later: the cascade is not the bug, the descent is.**

## 3. The "caller" reading was wrong: the function is entered by a TAIL JUMP (MEASURED, READ)

Disassembly of the pinned image (`/tmp/vmlinux.bin`, link base `0xffffffff81000000`, slide
`0x27800000`):

    ffffffff819e74b3  e8 28 f8 ff ff   call 0xffffffff819e6ce0
    ffffffff819e74b8  ...              <- the value read at rsp, three units running

The call at `0x...74b3` calls **`0x819e6ce0`**, not `0x819e4290`. So `0x819e74b8` is *that*
function's return address, still on the stack when `0x819e6ce0` reaches its body at
`0x819e4290` — i.e. `0x819e4290` is reached by a **jump**, not by a call, and everything the earlier
reports inferred from "the caller is one link up" (results/x86-64-caller.md, -twoload.md,
-peekfix.md) is one indirection looser than stated: the value at `rsp` at that break belongs to
`0x819e6ce0`'s caller, not to `0x819e4290`'s.

`0x819e4290` itself (READ, disassembly): `push %r13; mov %rcx,%r13; push %r12; mov %rsi,%r12;
mov %rdx,%rsi; push %rbp; push %rbx; mov %rdi,%rbx; sub $0x2a8,%rsp; mov %gs:0x28,%rax (canary);
movq $0x0,0x6(%rsp); lea 0x6(%rsp),%rbp; lea 0x8(%rsp),%rdi; rep stos (0x298 bytes zeroed)` — a
four-argument function that zeroes a 0x298-byte stack structure. `0xffffffff81a3d470` (its canary
failure path) is `__stack_chk_fail` by its callers' shape, which is a usable anchor for a symbol
pass.

## 4. Symbols are the next blocker, and the map on disk is a decoy (MEASURED)

    /tmp/debx/boot/System.map-6.1.0-50-amd64:
    ffffffffffffffff B The real System.map is in the linux-image-<version>-dbg package

READ: the pinned `.deb` ships a placeholder map, so the repeated frames (`0x81087329` a canary-check
epilogue, `0x8114bb15` a `preempt_count` + call pair, `0x812029cb`, `0x81a3d3aa` inside the #PF
handler, `0x81c00c02` inside the #PF IDT stub) cannot be named from it. The image itself carries
kallsyms (it is part of `.rodata`), so a kallsyms parser over `/tmp/vmlinux.bin` is the cheap route
to names; the `-dbg` ddeb is the heavy one.

## 5. The ONE next measurement

**Find what moves `rsp` down.** Not a read of the stack (stale words are indistinguishable from live
ones: §3 is the proof that a value at `rsp` is not necessarily what it looks like), but a record of
the pushes themselves. Add `V86C_RSPLOW=K` (env-gated, default off): in the interpreter's per-
instruction seat (the same place `v86c_dbg_break_hit` is called, src/cpu/cpu.c:199-203) keep a
**ring of the last K events in which `cpu->reg64[4]` reached a new minimum**, each carrying `rip`,
`rsp`, and the delta from the previous low; dump the ring on stop. The tail of that ring names the
exact instruction sequence that walks the stack off the bottom of the region — the descending step
pattern as `rip` values — which is what three units of stack reading could not settle. With the
ring in hand, name those rips with the kallsyms parser and the recursion is identified.

**Second, cheap, in the same unit:** the STOP dump prints only the 32-bit `cr2` (src/cpu/debug.c,
the `state rip=...` line) while every BREAK dump prints `cr2_64`. The stop is where the cascade
starts, so print `cr2_64` there too.
