# x86-64: the GS store — the displacement measured, both candidates REFUTED, and a defect in my own tool

**Status: the toolkit answered (1) and exposed a defect in itself on the way; (2) the displacement is
`0x63DB9F96`, so NEITHER candidate the last report named survives; (3) the port's computed address
`0x31558` is a THIRD shape and is not yet explained; no fix is claimed.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The instruction, byte for byte (MEASURED — and the tool's own bug)

The ring (extended from 8 to 16 bytes for this question) gives the instruction stream around the
store, and the first run's watch attributed the fault to `rip=0xFFFFFFFF9C2775C2` — **which is one
instruction too late**. The hook used `cpu->instruction_pointer`, and at a fault the cursor has already
advanced past the faulting instruction. **Fixed** (the hook now reports `cpu->previous_ip`, with the
measurement in the file), and the re-run gives:

    v86c dbg: WATCH F 0x31558 write rip=0xffffffff9c2775ba (the access FAULTED, no store)
    ring:  eip=0xffffffff9c2775b7 bytes 0f 20 e0 65 48 89 05 96 9f db 63 e8 18 fc ff ff …
    bases cs=0x0 ss=0x0 ds=0x0 fs=0x0 gs=0xffffffff9c242000

So the faulting instruction is `65 48 89 05 96 9F DB 63` = **`mov %rax, %gs:[0x63DB9F96]`** at
`0xFFFFFFFF9C2775BA`, with `gs_base = 0xFFFFFFFF9C242000` (MEASURED, printed by the dump's new bases
line).

## 2. Both candidates REFUTED (MEASURED)

* **(a) "the `65` prefix never reaches `modrm_resolve64`'s `segment_prefix`"** — refuted as the
  *explanation of `0x31558`*: with a base of `0xFFFFFFFF9C242000` and a displacement of
  `0x63DB9F96`, **no** base handling produces `0x31558`. The architecturally correct address is
  `0xFFFFFFFFFFFFBF96` (64-bit add: `0x9C242000 + 0x63DB9F96 = 0xFFFFBF96`, high half all ones —
  canonical, and *not* what the port faulted on).
* **(b) "the displacement really is `0x31558 − gs_base`"** — refuted directly: the displacement is
  `0x63DB9F96`, a positive 1.6 GB value, not a large negative one.

**So `0x31558` is a THIRD shape**, and the honest statement is that neither of the two shapes the last
report offered is the one the port computed.

## 3. What the third shape could be, and the next measurement (INTERPRETATION + instrument)

Candidates, none measured: the port may have resolved the displacement through a path that reads it
with the **wrong width or sign** (e.g. `read_imm32s` on the wrong bytes, or an imm8/imm16 path that
consumed part of it), or the store may be a *different* store than the ring's last entry (the ring
records 16 bytes per boundary, and `0x31558` is small enough to be a *second* access inside the same
instruction — there is none in this encoding), or the base was added and the result truncated to 32
bits at a boundary that is not the resolver's (the walk's `WALK64_TABLE` reads are bounded, so a
truncation would surface as a *different* fault, not this one).

**The next measurement is exact and the toolkit already carries it:** watch the **resolved** address —
`V86C_WATCH` on the address the port hands the resolver — and print, inside `modrm_resolve64` for a
GS-prefixed operand, `base`, `offset` and their sum. One flag plus one line, and the third shape
becomes a number.

## 4. Verification (MEASURED; code changed — the toolkit only)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green (the toolkit arm included)
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214 (tools present, off)
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (83/83 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed

No pin moved. The changes are the toolkit's (the ring widened to 16 bytes; the fault hook's rip
corrected to `previous_ip`), not the machine's.

## 5. Files touched

| file | change |
|---|---|
| `src/cpu/debug.c` / `debug.h` | the ring keeps **16** bytes per boundary and prints them; the comment records why |
| `src/cpu/cpu.c` | the ring records 16 bytes |
| `src/cpu/paging.c` | **the fault hook reports `cpu->previous_ip`**, not `instruction_pointer` — MEASURED as an off-by-one-instruction in the tool itself |

**Open, with the instrument named:** §3's resolved-address probe (base, offset, sum, inside
`modrm_resolve64` for a GS-prefixed operand); the GS/FS base sweep the brief asked for (§2 of the
scope) once the single instruction is understood; the recursion-bound arm; the high-RSP arm; `iretq`.
