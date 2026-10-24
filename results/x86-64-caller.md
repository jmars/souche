# x86-64: the stack top is the caller's FIRST ARGUMENT — and the tool that would name the caller is silent

**Status: MEASUREMENT (two port boots on the 256 MiB firmware, one QEMU attempt; no source change, no
fix). The value is not derived at this site at all: it is `arg1` of a 4-argument function, identical on both
machines, so the differing input is upstream — and this unit could not read the return address because the
toolkit's `V86C_PEEK` has gone silent.** Claims tagged MEASURED / READ / INTERPRETATION.

## 1. The function and its argument (MEASURED + READ)

    ffffffff819e4290: push %r13
    ffffffff819e4292: mov  %rcx,%r13        ; arg4
    ffffffff819e4295: push %r12
    ffffffff819e4297: mov  %rsi,%r12        ; arg2
    ffffffff819e429a: mov  %rdx,%rsi        ; arg3
    ffffffff819e429d: push %rbp
    ffffffff819e429e: push %rbx
    ffffffff819e429f: mov  %rdi,%rbx        ; ← arg1 — the stack top, 0xffffd1fac0003b82
    ffffffff819e42a2: sub  $0x2a8,%rsp
    ffffffff819e42a9: mov  %gs:0x28,%rax    ; canary
    ffffffff819e42c6: movq $0x0,0x6(%rsp)   ; ← the fault

A **four-argument** function whose `arg1` is the value in question. No register-load produces it here, so
"which linear address does the port compute" is not a question this frame can answer: it is handed in.

## 2. The return address is readable — but the instrument is not speaking (MEASURED)

`V86C_CHAIN` on the return-address slot `0xffffd1fac0000270` resolves it into the **mapped** upper region:

    PML4E 0x1000067 → PDPTE 0x11b6067 → PDE 0x11b7067 → PTE 0x800000000f802063  (frame 0xf802000)
    ⇒ the return address sits at physical 0xf802270

**`V86C_PEEK='0xf802270:16'` printed nothing — the third run in a row in which the PEEK line has been
silent** (it printed normally in earlier units with the same syntax, e.g. the nodemask run). That is a
TOOLKIT defect, recorded here so it is fixed rather than worked around: **an instrument's silence is not
evidence**, and nothing in this report rests on the missing eight bytes.

The QEMU leg of the same read (`m<addr>,8` over the gdbstub for its return address at
`0xffffc90000013a78`) **timed out** in this unit — no datum, not a result.

## 3. The one-sentence answer

**The port's `0x…c0003b82` and QEMU's `0x…00013b81` are the same KIND of quantity — `arg1` of the same
function, entered at the same `rip` on both machines — so the difference is NOT a base the port computes at
this site; it is an input that reaches the caller, and this unit could not read the caller's identity
because the return address needs the PEEK that has gone silent.**

## 4. No fix

There is nothing at this site to change: no load, no width, no base arithmetic. The defect — if the
argument's provenance turns out to be the port's — is one link up and unnamed.

## The ONE next measurement

**Fix `V86C_PEEK` first** (it is the only instrument that can read a physical slot at a stop, and it has
been silent for three units while `V86C_WATCH`, `V86C_CHAIN`, `V86C_BREAK` and `V86C_RING` all still
speak), then read `0xf802270` at the same break for the return address and disassemble backwards from it to
name the caller — and read the same slot on QEMU through its gdbstub, where the frame is
`0xffffc90000013a78`. With the caller named, the argument's provenance is one more link and the port's
share of it becomes visible.
