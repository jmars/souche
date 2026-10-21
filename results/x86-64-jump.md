# x86-64: the jump — the amd64 kernel ENTERS its own code, and dies on its 56th instruction

**Status: MEASUREMENT ONLY. No source file changed. The guest reaches `head_64.S`'s `jmp *%rax`,
enters the decompressed kernel at 0x1000000, executes 56 instructions, and stops on a #GP delivered
into the NULL IDT the stub left behind. Whether that #GP is a port defect is NOT established, and
nothing was "fixed" on a guess.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The jump fires (MEASURED)

    PROBE JUMP at instruction 4475477636: eip=0x1000000 rsp=0x2a03f58 rsi=0x84790
          cr0=0x80050033 cr3=0x4f76000 bytes: 48 8d 25 51 3f a0 01 48 8d 3d f2 ff ff ff b9 01 …

READ (`arch/x86/kernel/head_64.S:64-82`, v6.1) — that is `startup_64`, the decompressed kernel's
entry, instruction for instruction:

    leaq  (__end_init_task - FRAME_SIZE)(%rip), %rsp     ; 48 8d 25 … -> rsp = 0x2A03F58 (MEASURED)
    leaq  _text(%rip), %rdi                              ; 48 8d 3d f2 ff ff ff
    movl  $MSR_GS_BASE, %ecx                             ; b9 01 … (0xC0000101)
    …
    wrmsr
    pushq %rsi
    call  startup_64_setup_env

`%rsi = 0x84790` is the real-mode `boot_params` pointer the boot protocol hands over, and `%rsp`
lands exactly where `__end_init_task - FRAME_SIZE` computes to. **The port carried the guest through
the whole decompressor, `parse_elf` and `handle_relocations` and delivered control to the kernel's
own entry with the registers the protocol specifies.**

## 2. The 56 instructions it executes (MEASURED)

The after-jump trace (last 24 recorded, bytes from the relocated image):

| eip | bytes | READ as |
|---|---|---|
| 0x1001b64 | `48 c1 ea 10 48 c1 e2 30 48 89 e5 48` | `shr rdx,16; shl rdx,48; mov rbp,rsp; …` |
| 0x1001b73 | `65 48 8b 04 25 28 00 00 00` | `mov rax, gs:0x28` — **the stack canary, via the GS base `wrmsr` just wrote** |
| 0x1001b9f | `48 c1 e0 20 66 89 f0 48 c1 ee 20` | the `startup_gdt_descr`/IDT fixups (`fixup_pointer`) |
| 0x1001bc6 | `65 48 2b 04 25 28 00 00 00 75 06 c9` | `sub rax, gs:0x28; jne <fail>; leave` — **the canary CHECK PASSES** |
| 0x1001bd2 | `e9 59 01 e0 00` | `jmp 0x1E01D30` — the tail call |
| 0x1e01d30 | `c3 cc 00 00 …` | `ret`, then `int3` padding, then a large zero region |

So the kernel ran the end of `startup_64_setup_env` (READ `head64.c:625-637`, whose last statement is
`startup_64_load_idt(physbase)` — the tail call), and the **stack-protector canary it reads through
`%gs:0x28` matched** (`gs_base = 0x3042000`, MEASURED), which is itself evidence that the MSR write
and the GS-based addressing work in long mode.

## 3. The named stop: a #GP on the tail call's `ret`, into a NULL IDT (MEASURED)

    PROBE around the tail-call target 0x1e01d30 (-64..+32):
      cc cc cc … cc cc c3 cc 66 66 2e 0f 1f 84 00 00 00 00 00 0f 1f 00 c3 cc 00 00 00 …
    PROBE gs_base=0x3042000 idtr base=0x0 limit=0 cpl=0
    PROBE regs at stop: rax=0 rbx=0x2a1a020 rcx=0xc0000101 rdx=0x0307000000000000
                        rsi=0 rdi=0x2a04000 rbp=0x2a04000 rsp=0x2a03f34 r8=0x4e8714c
    v86c: guest raised cpu exception 13 after 4475477692 instructions

* The tail-call target `0x1E01D30` holds `c3` (a bare `ret`) at the **end of a 0xCC padding run**,
  followed by `66 66 2e 0f 1f 84 00 …` (multi-byte NOPs) and then a large **zero** region.
* Executing that `ret` raises **#GP (vector 13)** — i.e. the word it popped is not a canonical
  address (a #PF would have set cr2 and vector 14).
* It is delivered into **`idtr base=0x0 limit=0`**, the NULL IDT `cleanup_exception_handling()`
  installs immediately before returning to `head_64.S` (READ `misc.c:463-465`). The machine
  therefore stops: `exception=13, unimplemented=1, eip=0x0`, at **4,475,477,692 instructions**.

**On hardware the same #GP would triple-fault**, so this is a genuine fault and not a port artefact —
but *what* makes the popped address bad is not established (§4).

## 4. What is NOT established, and the stack evidence (MEASURED + INTERPRETATION)

    PROBE stack @0x2a03f34: [0]=0x0478000000000000 [8]=0x0100000000000000
                            [16]=0x0100003600000000 [24]=0x084790000000000 [32]=0x0

**INTERPRETATION, offered as a hypothesis and not as a finding:** those four words look *byte-shifted*
relative to plausible values this boot has already produced — `0x4780000` (the decompressor's
relocation target), `0x1000000` (`_text`), `0x1000036xx`, `0x84790` (`boot_params`) all appear with
their bytes displaced by one. A one-byte displacement is not what a wrong push/pop *size* produces
(that shifts by 4 or 8), and I could not attribute it to any instruction the port executed. **I am
not claiming it as a defect.**

Two candidate explanations, neither tested:

1. **The relocated image is wrong.** `parse_elf` moves `output + p_offset` → `p_paddr` *in place*, and
   segment 0's `p_offset` is 0x200000 against `p_paddr` 0x1000000 — a 2 MB downward shift of a 26 MB
   segment inside the same buffer, with five heavily overlapping segments. If the port's execution of
   the stub's own `memmove` differs from hardware's for an overlapping range, the code at 0x1E01D30
   would be the wrong bytes. **The `cc`-padding-then-`ret` shape is exactly what a mis-shifted copy
   would produce at a function's boundary.**
2. **The tail call is fine and the stack word is the problem** — i.e. something in
   `startup_64_setup_env`'s frame was written wrong.

**THE NEXT MEASUREMENT, named and cheap (~2 minutes each):**
(a) dump the stack at `0x1001BD2` *before* the tail call, and the exact word the `ret` at
`0x1E01D30` pops; (b) **byte-compare a known code region before and after `parse_elf`** — e.g.
`0x1000000..0x1001000` versus the decompressed image at `output + 0x200000` — which tests
explanation 1 directly; (c) decode the five program headers *before* `parse_elf` runs (they are gone
afterwards: the copy overwrites the ELF header, which is why this unit's late-run header dump read
garbage and had to be discarded).

## 5. The wall clock (MEASURED — this is the number the brief asked for)

Three full runs of the pinned kernel with a 5 G budget, each ending at the #GP above:

| run | wall clock | instructions executed | rate |
|---|---|---|---|
| A (jump trace) | 112 s | 4,475,477,692 | 40.0 M instr/s |
| B (after-jump trace) | 114 s | 4,475,477,692 | 39.3 M instr/s |
| C (stack + target dump) | 118 s | 4,475,477,692 | 37.9 M instr/s |

**So the last report's estimate — "a 5 G-instruction boot is ~25 minutes of wall clock" — was wrong
by about 13×: the measured rate is ~38-40 M instructions/s, and 5 G instructions is ~2m10s.** The
run never reaches its 5 G budget in any case, because it stops at the #GP after 4.475 G.

## 6. Verification (MEASURED; every gate by exit code)

**No source file changed** — `git status` reports nothing under `src/`, `gen/` or `tests/`; the
probes were added to `src/cpu/cpu.c` and `src/main.c`, run three times, and both files were restored
from git before the gates.

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green (cpu 3889, m1c 272, hw 975,
                                           uop_cache 7934363, uop_exec 102, jit 68, jit_codegen 3570)
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    make nasm                              rc 0 — Passed 118/118 tests

No pin moved, no gate tightened. The rest of the battery (bench, reboot, VGA ×5, pae-1, pae-l3,
gate-long-mode, gate-long-mode-sys, gate-x86-64-diff, make nasm64) was **not** re-run because nothing
functional changed — and that is exactly what the brief allows for a measurement-only unit.

## 7. Files touched

| file | change |
|---|---|
| — | **none.** Probes were added to `src/cpu/cpu.c` and `src/main.c`, run, and removed; both were restored from git and the build re-verified. |
