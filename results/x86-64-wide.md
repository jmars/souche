# x86-64: the address is 64 bits all the way down — the "widen the bodies" sweep

**Status: landed. The 4 GiB bound is gone from the resolver and from every body that dereferenced
it, and the amd64 guest now DECOMPRESSES THE WHOLE KERNEL (MEASURED: the output buffer's last
non-zero byte is 0x17FFFFF, i.e. all 8,388,607 bytes of it) with no named stop at a 1.5 G
instruction budget.**

The unit's premise held exactly: a 64-bit instruction computed a 64-bit effective address and handed
it to a pair that could not carry it. What this unit found is that the pair was not one layer but
**four** — the resolver, the accessor family, the instruction bodies, and (found by the instrument,
not by reading) the **arith layer's bit-test helpers**.

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The sweep, layer by layer

### 1.1 `modrm_resolve` — the bound is removed (MEASURED)

`modrm_resolve` returned `int32_t` and, in long mode, refused any answer above 4 GiB with a named
stop whose own comment said why: *"every 32-bit-arm body takes an int32_t address"*. That sentence
was the bug report. It now returns **`int64_t`** and, in long mode, returns `modrm_resolve64`'s
answer whole. The LEA carve-out (a 32-bit LEA stores the LOW 32 BITS — the instruction's own
semantics) is kept, and `lea` never reaches the accessors.

### 1.2 The accessor family — widened, with the 32-bit machine unchanged (MEASURED)

Eleven functions took `int32_t addr`: `safe_read8/16/32s/f32/64s/128s`, `safe_write8/16/32/64/128`.
All now take **`int64_t`** and dispatch at the top:

    if (long_mode_64()) return safe_read32_addr(addr);   /* the S5 64-bit-address twin */
    ...the existing body, unchanged...

**Why that is byte-identical outside long mode:** every 32-bit caller passed an `int32_t`, and an
`int32_t` argument converted to `int64_t` sign-extends — so the body sees exactly the value it saw
before, and the page-straddle tests (`addr & 0xFFF`) read the same low bits. §5's four M0 pins are
the measurement.

The 128-bit and f32 forms had **no** 64-bit-address twin, so they got one here: `safe_read128s` is
two `safe_read64_addr` calls, `safe_write128` two `safe_write64_addr` calls, `safe_read_f32` a
`safe_read32_addr` plus the bit cast.

### 1.3 The instruction bodies — 1271 signatures (MEASURED)

Every `_mem` body and declaration in `instructions.c`, `instructions_0f.c`, `instructions_64.c` and
`sse_instr.c` now takes **`int64_t addr`**: 720 definitions and 551 declarations, plus the two local
`int32_t addr = modrm_resolve(...)` sites per custom body and the six `moffs` forms
(`get_seg_prefix_ds`). `instructions_64.c`/`sse_instr.c`'s bodies already took `int64_t` (S5's
widening); the 16/32-bit arms were the ones that did not.

### 1.4 The emitter — one emitted form, right in both machines (MEASURED)

`gen/interpreter_c.js` used to emit `int32_t addr = modrm_resolve(...)` for the 16/32 arms and
`int64_t addr = modrm_resolve64(...)` for the 64-bit ones. It now emits **one** form,
`int64_t addr = modrm_resolve(modrm_byte)`, because `modrm_resolve` is the mode-aware entry point:
in 32-bit mode it returns the 32-bit resolver's value (the wrap the model owns), in long mode the
64-bit one. The generated dispatcher is regenerated (736 `int32_t addr` sites gone).

### 1.5 The arith layer — FOUND BY THE INSTRUMENT, not by reading (MEASURED)

`bt_mem`, `bts_mem`, `btr_mem`, `btc_mem` in `arith.c` take `int32_t virt_addr` — one layer below
the bodies, invisible to a sweep that stops at the `_mem` signatures. The
`wide-strobits-mem-above-4g` case caught it on the first run:

    wide-strobits-mem-above-4g: exception rip actual 0x90040 / expected 0x9003a, Exception None / GP

i.e. the port executed the `bt` with no fault at all (the address had been truncated to 32 bits,
landing on mapped memory) while the host took #GP. All four now take `int64_t virt_addr`.

---

## 2. The instrument: one case per family, and it must FAIL before it passes

A corpus case that uses an effective address above 4 GiB cannot simply read memory: nothing is
mapped above 4 GiB in the **native** leg, which runs the same body on the host. So each case uses a
**non-canonical** address (`0x800000000000`: bit 47 set, bits 63:48 clear), which raises **#GP** in
the guest and **SIGSEGV** in the host at the same rip — the same exception kind, which is exactly
what the harness's faulting-case comparison checks. Before the sweep the port stopped by name
("a 64-bit effective address above 4 GiB"), which the corpus reports as RED.

New family `wide` (7 cases, one per family the brief names):

| case | what it dereferences |
|---|---|
| `wide/mov-mem-above-4g` | `mov eax, [r9]` — the guest's own shape: a 32-bit-operand form whose address is 64 bits |
| `wide/alu-mem-above-4g` | `add dword ptr [r9], 0x10` — the RMW macro path |
| `wide/ctrl-mem-above-4g` | `xchg dword ptr [r9], ecx` |
| `wide/muldiv-mem-above-4g` | `imul eax, dword ptr [r9]` |
| `wide/strobits-mem-above-4g` | `bt dword ptr [r9], 3` — **the one that found §1.5** |
| `wide/simd-mem-above-4g` | `movdqu xmm0, [r9]` — the S10 accessor path |
| `wide/str-mem-above-4g` | `cmpsb` with **both** pointers above 4 GiB |

### 2.1 The able-to-fail arms are MEASURED, in two mutations

* **Mutation A — restore the 4 GiB bound in `modrm_resolve`** (keeping the widened types):
  `SUMMARY: total=80 green=74 red_unimplemented=6 mismatch=0`. The six modrm-borne cases go RED;
  `wide/str-mem-above-4g` stays green because the string path reaches memory through its own
  64-bit accessors and never calls `modrm_resolve` — stated rather than glossed.
* **Mutation B — restore the pre-unit string path** (`string_instruction`'s long-mode branch
  disabled, so RSI/RDI/RCX truncate): `wide-str-mem-above-4g` MISMATCHES (the truncated pointer
  read address 0, which is mapped, so no fault happened at all).

With both mutations reverted: **80/80 green** on the long-mode leg.

---

## 3. The guest: how far it gets, and the next thing to look at (MEASURED)

    v86c boot: 1500019675 instructions, in_hlt=0, exception=-1, unimplemented=0,
               eip=0x4f3a36e cs=0x10 cr0=0x80050033 cr3=0x4f76000

* **No named stop, no unimplemented instruction** at a 1.5 G budget (the run ends on the
  instruction limit).
* **The decompression COMPLETES**: the output buffer at 0x1000000 holds `7f 45 4c 46` (ELF) from
  early on, and the last non-zero byte climbs monotonically —
  2,326,906 bytes at 60 M · 3,442,638 at 200 M · 5,113,047 at 400 M · **8,388,607 (the whole 8 MiB
  window) at 1.5 G**. So the decompressor produced the entire kernel image.
* The loop that does the writing is identified, not guessed: `0x4f399a8..0x4f399e5`, entered with a
  sane counter in EDI (MEASURED entries: edi = 1, 8, 7, 6 …), its object at `[rax] = 0x1000000`
  (the output buffer), writing one byte per ~15 instructions — which is why the whole decompression
  costs ~1.2 G instructions. It is **slow, not stuck**: 300 M instructions between 100 M and 400 M
  change the stack region and advance the output extent (the first draft of this probe watched only
  the buffer's first 4 KiB and would have called it a hang; the extent scan is the instrument).

**The next thing to look at is named, not fixed:** after the decompression completes the guest is
still in the decompressor at `eip=0x4f3a36e` (the relocation/final-jump path), and the next unit's
first measurement should be the *relocation copy's destination range* — the same extent-scan
instrument applied to `0x4780000` (which the probe read once at 475,321 and did not move) and to the
decompressed kernel's entry point. There is **no stop to name yet**: the honest statement is that
the port now runs the guest past every stop it had, and the limit is the budget.

---

## 4. What this unit did NOT do (named gaps)

* **The decompressor's speed** is unchanged and is now the binding cost: one byte per ~15 emulated
  instructions, so a boot needs ~1.2 G instructions before the kernel's own code starts. Not a
  correctness gap; recorded because it sets the next unit's budget.
* **`far_return`'s 32-bit `eip`** (a RETF target ≥ 4 GiB and a CPL-changing RETF are named stops),
  `/3 callf`, `/5 jmpf`, the register-source `BT` form and the CPL-changing 64-bit interrupt
  delivery are all still open from earlier units; none of them is reachable from the current run.
* **The 4 GiB physical model.** A linear address above 4 GiB is now carried correctly, but the
  *physical* address space is still 32 bits (`plan ss5.1`), so a guest that maps a high linear
  address onto a physical page above 4 GiB will still stop — by name, at the walk, not here.

---

## 5. Verification (MEASURED; every gate by exit code)

    make                                   rc 0, no warnings (cosmocc -Wall -Wextra -Werror)
    make selftest                          rc 0 — cpu 3889/0, m1c 272/0, hw 975/0,
                                           uop_cache 7934363/0, uop_exec 102/0, jit 68/0,
                                           jit_codegen 3570/0
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (80/80 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-pae-l3.sh all               rc 0 — 0 FAILING
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed
    tests/gate-vga-{0,1,2,3,4}.sh          rc 0 each — 0 FAILING

**The four M0 pins did not move**: transcript `8bb8eca5…`, determinism 4,000,096,757 (both runs),
bench 200,095,214, `make nasm` 118/118. That is the measurement behind §1.2's claim that the widened
accessors are inert outside long mode, and behind §1.3's 1271 signature changes.

**Pins that moved, by construction, with the able-to-fail arms re-checked:**

| pin | was | now | why |
|---|---|---|---|
| corpus cases | 73 | **80** | the seven `wide` cases |
| corpus families | 7 | **8** | family `wide` (slice label `W`: outside the plan's slices) |
| `gate-nasm64.sh` perturb excludes | 3 faulting cases | **10** | a faulting case compares the exception, not the register file, so a perturbed register cannot fail it (MEASURED: 7 green with the old list) |
| `gate-x86-64.sh` step | `stop-is-the-64bit-address-bound` | **`no-named-stop`** + an able-to-fail arm asserting the bound's message is ABSENT | the bound stop no longer happens |

`gate-x86-64.sh`'s new step asserts `unimplemented=0` at its 40 M budget rather than the whole
decompression, and says so: the full extent needs ~1.5 G instructions (~9 minutes), so it is
**measured in this report and not asserted in the gate** — the alternative was a gate nobody runs.

---

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/modrm.c` / `.h` | `modrm_resolve` returns `int64_t`; the 4 GiB bound removed (the LEA carve-out kept) |
| `src/cpu/paging.c` / `.h` | the eleven accessors take `int64_t` and dispatch to their 64-bit-address twins; new 128-bit and f32 64-bit-address arms |
| `src/cpu/arith.c` / `.h` | `bt_mem`/`bts_mem`/`btr_mem`/`btc_mem` take `int64_t virt_addr` |
| `src/cpu/instructions.c` / `.h` | 720 `_mem` definitions + 551 declarations to `int64_t addr`; the custom bodies' local resolvers |
| `src/cpu/instructions_0f.c` / `.h` | the same for the 0F space |
| `src/cpu/instructions_64.c`, `sse_instr.c` / `.h` | the remaining `_mem`/address parameters |
| `gen/interpreter_c.js` | one emitted form: `int64_t addr = modrm_resolve(modrm_byte)` |
| `src/cpu/interpreter_gen.c` | regenerated (736 `int32_t addr` sites gone) |
| `gen/nasm64_gen.py`, `tests/nasm64/` | family `wide`, 7 cases; the stub also maps linear 4 GiB (kept as geometry, unused by the faulting cases) |
| `tests/gate-{x86-64,long-mode,x86-64-diff,nasm64}.sh` | the pins of §5, the perturb excludes, the replaced step |
