# x86-64: a 256 MiB firmware removes the decompressor's fallback — and the stop does not move

**Status: ONE FIRMWARE VARIANT ADDED (Makefile only, default untouched), the hypothesis TESTED, and it is
PARTIALLY CONFIRMED: the decompressor now accepts a physical region (both KASLR-fallback lines are GONE)
and the kernel prints one more line — but it stops at the SAME instruction, so the stack crossing is NOT
downstream of the failed placement.** Claims tagged MEASURED / READ / INTERPRETATION.

## 1. The variant (READ, Makefile only)

`MINIBIOS_256_MEM_TOP := 0x10000000` + a `$(BUILD)/minibios-256m.bin` rule (same recipe as the 80 MiB
variant, same 64 KiB size check) + a `minibios-256m` target. **The default `MEM_TOP` is untouched and the
embedded firmware's bytes are unchanged** — the amd64 leg is handed this file with `--bios`, exactly as
the disk gate is handed the 80 MiB one.

## 2. The test (MEASURED, `--bios build/minibios-256m.bin --memory 256 --long-mode on`)

**Both KASLR-fallback lines are GONE** (`grep` for `Invalid physical address|Physical KASLR|KASLR
disabled` → nothing; the 64 MiB firmware printed `Invalid physical address chosen!` and `Physical KASLR
disabled: no suitable memory region!` every time). So the decompressor **does** accept a physical region
with a map that matches the machine — the memory-map half of the root cause is CONFIRMED.

The kernel then runs one line further and stops:

    [   75.009686] clocksource: tsc-early: mask: 0xffffffffffffffff max_cycles: 0x1cd42e4dffb, max_idle_ns: 881590591483 ns
    [   75.034451] Calibrating delay loop (skipped), value calculated using timer frequency.. 2000.00 BogoMIPS (lpj=4000000)
    [   75.036785] Last level iTLB entries: 4KB 64, 2MB 64, 4MB 64
    [   75.036785] Last level dTLB entries: 4KB 64, 2MB 0, 4MB 64, 1GB 0
    [   75.036785] Spectre V1 : Mitigation: usercopy/swapgs barriers and __user pointer sanitization
    [   75.036785] Spectre V2 : Mitigation: Retpolines          ← NEW: the 64 MiB firmware never printed this

    v86c boot: 4590902204 instructions, unimplemented=1, eip=0xa91e42c6 cr3=0x7010000   (was 4590876590, eip=0x897e42c6)

## 3. The stop is the same one (MEASURED, the dump)

    state rip=0xffffffffa91e42c6 cr2=0xbfffff90 cr3=0x7010000 idtr=0xfffffe0000000000
    regs rsp=0xffffd1fabfffffc0 rbx=0xffffd1fac0000382 rbp=0xffffd1fabfffffc6

Slide `0x27800000`, so the rip is link **`0xffffffff819e42c6`** again — `movq $0x0,0x6(%rsp)` — and the
registers have the same shape: `rsp` and the stack top straddle a 1 GiB boundary
(`0xffffd1fac0000000`), i.e. **the same stack crossing, at a different address, with a placement the
decompressor accepted.**

## 4. What that means

- **CONFIRMED**: the memory map WAS the reason the decompressor refused every candidate
  (`results/x86-64-ptcensus.md`), and a 256 MiB firmware fixes exactly that.
- **REFUTED**: the crossing is **not** downstream of the failed placement — with a placement the
  decompressor accepts, the task stack still crosses a 1 GiB boundary whose lower half the kernel never
  maps. The KASLR-fallback line was a symptom of one machine-input bug, not the cause of the stop.

## 5. Gates (the change is a new Makefile rule; nothing existing re-pointed)

    make rc 0 (rebuilds v86c, no other target touched) · transcript BYTE-IDENTICAL ✓
    gate-x86-64 14/0 ✓ (still on the DEFAULT firmware, as before)
    the 32-bit gates are untouched by construction — the default firmware's bytes are unchanged.

**The gate was deliberately NOT re-pointed to the new firmware**: the stop did not move, so the honest
state is "better firmware, same defect", and re-pointing the amd64 gate belongs with the unit that fixes
the crossing (it will then want the KASLR-absent assertion this firmware makes possible).

## The ONE next measurement

**Why a task stack is placed across a 1 GiB boundary at all.** The crossing is now independent of the
decompressor, so the next question is the stack's own allocation: at the stop, `rsp` is 0x3c2 bytes below
`0xffffd1fac0000000` and the stack top 0x382 above it — read the value the kernel stored as this task's
stack pointer (`cpu_current_top_of_stack`/`thread.sp`, watchable through the per-cpu slot at `gs:0x199e0`
that `cpu_init`'s prologue reads) and compare it with the vmalloc area's mapped 1 GiB regions: if the
stack top is exactly at a region boundary, the port is placing or switching to a stack the kernel did not
build there.
