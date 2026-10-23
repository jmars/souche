# x86-64: the recursion is a per-cpu base with a wrong HIGH HALF

**Status: MEASUREMENT, root cause localized to a 32-bit-halves defect in the per-cpu base. No fix is
claimed: the defect is in how the *high* half of one address is formed, and the measurement narrows
it to that value without yet naming the instruction that builds it.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The first fault (MEASURED)

`V86C_DUMP_ON_STOP=1 V86C_RING=40`, the ring's last boundaries (the kernel's slide is still
`0x7E00000`, established by matching the ring's bytes to the image):

    ffffffff890ef8e5  65 8b 05 94 ec f3 76   mov %gs:0x76f3ec94,%eax      ← per-cpu read
    ffffffff890ef8ec  48 98                  cltq
    ffffffff890ef8ee  55 89 fd               push %rbp ; mov %edi,%ebp
    ffffffff890ef8f1  48 8b 14 c5 e0 93 a3 8a  mov -0x7d3c6c20(,%rax,8),%rdx  ← __per_cpu_offset[cpu]
    ffffffff890ef8f9  53                     push %rbx
    ffffffff890ef8fa  48 8d 82 00 1e 00 00   lea 0x1e00(%rdx),%rax
    ffffffff890ef901  3b ba 08 1e 00 00      cmp %edi,0x1e08(%rdx)        ← THE FAULT, with %rdx = 0

`%rdx = 0` (MEASURED: `rdx=0x0` in the register dump), so `cmp %edi,0x1e08(%rdx)` reads address
`0x1e08` — a **NULL per-cpu pointer**. The instruction is at link `0xffffffff812ef901`
(`mov %gs:0x7ed3ec94` at `0xffffffff812ef8e5` is the image's own form; the runtime displacement is
relocated by the slide).

## 2. The delivery's own fault (MEASURED)

    state rip=0xffffffff890ef901 cr2=0xc0a0b010 cr3=0x2a10000 cr4=0xb0 efer=0xd01
          idtr=0xffffffff8b110000/4095 gdtr=0xffff8c16c0a0b000/127 cpl=0
    bases cs=0x0 ss=0x0 ds=0x0 fs=0x0 gs=0xffff8c16c0a00000
    regs  rax=0x1e00 rbx=0xffff8c16c4426bc0 rcx=0x0 rdx=0x0 rsi=0x400 rdi=0x3 rbp=0x3
          rsp=0xffffffff8a803ef0

`cr2 = 0xc0a0b010` is **`gdtr + 0x10`** — the delivery could not read the CS descriptor in the GDT,
so the delivery itself faulted, four times, and the bound fired. And **`gdtr = gs_base + 0xb000`**
exactly: the GDT is the per-cpu `gdt_page`, so both addresses come from **one value**.

## 3. The reference (MEASURED — QEMU's gdbstub at the same instruction)

| register | QEMU (nokaslr, link addresses) | port |
|---|---|---|
| `rax` | **0xffff88800ffb6e00** | `0x1e00` |
| `rdx` | **0xffff88800ffb5000** | `0x0` |
| `rdi` / `rbp` / `rsi` | `3` / `3` / `0x400` | `3` / `3` / `0x400` ✓ |
| `rsp` | `0xffffffff82a03ef0` | `0xffffffff8a803ef0` (de-slid: the same) ✓ |

So on hardware the per-cpu pointer is valid and the `cpu_number` read returned 0; on the port the
same read returned **`0x1e00`**, which then indexes `__per_cpu_offset[0x1e00]` out of bounds → `0`.

## 4. The defect (MEASURED)

    port:   gs_base = 0xffff8c16 c0a00000
    should:            0xffff8880 c0a00000    ← the direct map of the SAME allocation

The **low half is the port's own per-cpu allocation** (`0xc0a00000` = 192 MiB, inside the guest's
256 MiB — a legitimate `memblock` placement; QEMU's is `0xffb5000`, a different but equally
legitimate one). The **high half is wrong**: `0xffff8c16` where the direct map requires `0xffff8880`.

**So this is a 32-bit-halves defect in the per-cpu base's high half** — the oldest class in this
lane — and not an allocation, a mapping, or a delivery-shape error: every other register at the fault
matches QEMU, and the frame/IDT/GDT mechanics are consistent.

**INTERPRETATION:** `wrmsr(MSR_GS_BASE)` takes EDX:EAX, and the per-cpu base is also *stored* in
memory (`__per_cpu_offset[0]`, and the GDT address is derived from it), so the wrong high half is
already in the computed value rather than only in the MSR write — the next fork is the instruction
that builds that value, in `setup_per_cpu_areas`' arithmetic, not the `wrmsr` alone. That is a
hypothesis the next measurement settles (break on the store to `__per_cpu_offset[0]` and on the
`wrmsr` that follows, and read both).

## 5. Why no fix here

The defect is a *value* whose producer is not yet identified; the brief's own rule is not to change
the machine on a hypothesis, and a 32-bit-halves bug has at least three shapes here (a 32-bit
truncation in the arithmetic, a halves swap in a 64-bit move, or a mis-read EDX in `wrmsr`). Naming
the instruction first is one measurement; guessing costs a wrong fix in the per-cpu path, which every
later gate depends on.

**The calibrated target has moved a long way and is not lost:** the kernel still reaches its own
banner and ~30 s of boot (`percpu: Embedded 62 pages/cpu …` is the last serial line), and
`gate-x86-64` is **14/0** with the banner gated.

## 6. Verification (MEASURED; **no source change** — instruments only, so the subset)

    make                     rc 0
    make selftest            rc 0 — all suites green
    gate-x86-64.sh           14 passed, 0 failed
    make bench               200095214

No pin moved. `git status` shows only this report and `TODO.md`.

## 7. Files touched

| file | change |
|---|---|
| `results/x86-64-recursion.md` | this report |
| `TODO.md` | §8 |
