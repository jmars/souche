# x86-64: the repair was refused by a 32-bit CR2 — the direct map now maps, and the kernel moves on

**Status: ONE FIX, VERIFIED BY ASYMMETRY. The demand-mapping repair `results/x86-64-dmap.md` left
broken is now WORKING and MEASURED: `PML4[273] = 0x30ec063`, pointing at `early_dynamic_pgts[0]`,
so `copy_bootdata`'s `memcpy` retries and succeeds and the kernel advances past the direct-map read.
The stop has moved from the #PF (trapnr 14) to a **#UD** (trapnr 6).**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. Candidate (a), with both sides of the comparison visible (MEASURED)

`__early_make_pgtable` (link `0xffffffff83077283`) opens with
`physaddr = address - page_offset_base`, `cmp MAXMEM`, `jae → return false`. Breaking at the entry
and at the `cmp`:

    BEFORE the fix
    BREAK 0 0xffffffff8ae77283 (entry)  rdi=0x84790            ← the ADDRESS, truncated
    BREAK 1 0xffffffff8ae772ac (cmp)    rdx=0x778000084790  rax=0x400000000000   → jae TAKEN
    PEEK 0x2413d60 (page_offset_base)   0xffff888000000000    ← correct
    PEEK 0x30ea888 (PML4[273])          0                   ← never mapped
    (the two store sites were never reached)

`rdx` is `address - page_offset_base` wrapped, and `0x778000084790 >= 0x400000000000`, so the guard
refused exactly as the source says. **Candidate (a) is the answer**, and the truncation is this
lane's oldest bug: a 32-bit field holding a 64-bit address.

## 2. Why the previous unit's CR2 widening did not help (MEASURED)

`cr2_64` was **correct** — a temporary probe inside the `0F 20 /2` body printed

    PROBE 0F20/2 long=1 prefixes=0x0 cr2_64=0xffff888000084790 cr2=0x84790

so the field held the whole address, but the read still returned `0x84790`: the body's
`long_mode_64() && (prefixes & PREFIX_REX_W)` test could **never** fire, because `cpu->prefixes` is
already 0 when the body runs for `48 0f 20 d7` (the `0F` escape's 32-bit dispatcher does not carry
the REX into the body). Keying on REX.W was wrong.

## 3. The fix (MEASURED, two parts, `src/cpu/instructions_0f.c`)

1. **`mov r, cr2` in long mode returns the whole `cr2_64`**, unconditionally — the SDM's default
   operand size for `MOV CR` in IA-32e is 64 bits, whatever the prefixes. (A `0x66`-prefixed
   32-bit CR2 read in long mode is a **named gap**: the prefix state does not reach this body, so
   it cannot be distinguished there.)
2. **`mov cr2, r` in long mode takes a 64-bit source** (`read_reg64`), not a truncated `read_reg32`
   — the mirror defect, caught by the arm below.

**The asymmetry, same instrument and guest** (MEASURED):

| | before | after |
|---|---|---|
| entry `rdi` | `0x84790` | **`0xffff888000084790`** |
| guard | `0x778000084790 >= MAXMEM` → refuse | **`0x84790 < MAXMEM` → passes** |
| store site `0x8ae77456` | never reached | `rsi = 0xffffffff8aeea888` (= `early_top_pgt + 273*8`), `rax = 0x30ec063` |
| `PEEK 0x30ea888` | `0` | **`0x30ec063`** — `PML4[273]` mapped to `early_dynamic_pgts[0]` (`0x30ec000`, first entry `0x30ecd063`) |
| stop | #PF panic, `trapnr = 0x0e` | **#UD panic, `trapnr = 0x06`** |
| instructions | 4,475,558,722 | 4,475,556,480 |

So the direct-map repair lands, the `memcpy` retries and completes, and the kernel is past the fault
that stopped every unit since `results/x86-64-reach2.md`.

**The unit arm** (`test_cr2_64bit_read`, `src/cpu/m1c_selftest.c`) is register-only — it needs no
paging, which is why it can live in that harness at all: outside long mode `mov eax, cr2` gives the
32-bit projection; in long mode `mov rax, cr2` gives the whole address; `mov cr2, rcx` stores the
whole thing. It **fails against the pre-fix build** (`0x84790` / `0x84123`) and passes now.

## 4. What is left, named: a #UD after the repair (MEASURED)

The halt now carries `trapnr = 6` (#UD) — `early_fixup_exception` panicking because
`fixup_exception` found no fixup for an undefined instruction — and the frame's `regs->ip` reads
**0**. The ring's last boundaries are a function epilogue (`pop r12; pop r13; pop r14; pop r15; pop
rbp; jmp …`) and the shared `ret` thunk, so the #UD is taken at a **return target**. The next fork is
therefore "what does that `ret` pop, and why is the target not an instruction" — the frame's
`regs->ip == 0` and the stack words at the `ret` are the evidence, with `V86C_BREAK` on the `ret`
thunk and `V86C_PEEK` of the popped slot as the instrument. **INTERPRETATION:** a zero `ip` suggests
a value that never got written rather than a mis-decoded instruction, which is why the pop's source
matters more than the #UD's address.

**The calibrated target is NOT reached**: no first `printk`, so nothing was added to
`gate-x86-64.sh`.

## 5. Verification (MEASURED; **code changed**, full battery, by EXIT CODE)

    make / selftest / nasm / nasm64        rc 0 (selftest: all suites green, incl. the new arm)
    make bench                             200095214
    make transcript-diff                   byte-identical to the reference
    make determinism                       two consecutive runs byte-identical
    gate-m2-5-determinism                  4,000,096,757 both runs, serials byte-identical
    gate-x86-64 12/0 · gate-long-mode 57/0 · gate-long-mode-sys 5/0 · gate-x86-64-diff 13/0
    gate-nasm64 13/0 · gate-reboot-1 39/0 · gate-pae-1 11/0 · gate-pae-l3 0 FAILING
    gate-vga-0..4                          0 FAILING each

**No pin moved** — as designed: outside long mode both CR2 paths take the unchanged `cr[2]` route.

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/instructions_0f.c` | long-mode `mov r, cr2` returns the whole `cr2_64`; long-mode `mov cr2, r` takes a 64-bit source |
| `src/cpu/debug.c` | the break dump prints `cr2_64` and `prefixes` (the two fields that named this defect) |
| `src/cpu/m1c_selftest.c` | `test_cr2_64bit_read` |
| `results/x86-64-repair.md` | this report |
| `TODO.md` | §8 |
