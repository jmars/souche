# R1F — applying the R1 review findings (F1 / F2 / F3)

**Unit:** `HANDOFF=v86c`, unit R1F (`coder`). Authority: `results/r1-review.md`,
`handoff-v86c-review`. Files touched: `src/cpu/memory.{c,h}`, `src/cpu/cpu_selftest.c`,
`tests/mkfixture.c`, `oracle/nasm_state.mjs` — and nothing else (string.c /
instructions.c / gen/interpreter_c.js untouched; `v86/` read-only).

**Status: all gates exit 0, and F1's regression probe fails before the fix and passes
after (the asymmetry is the proof).** One deviation from the reviewer's *prescribed*
fix, with the measurement that forced it — §F1 below.

---

## F1 — the unclaimed-memory read default (SHOULD-FIX)

### The reviewer's prescription was right about the value and wrong about its scope — MEASURED

The reviewer's fix was one line each: `device_read_default8` → `0xFF`,
`device_read_default32` → `-1`, "in both tables". Applied literally it also changes the
**VGA window (0xA0000-0xBFFFF) and the LFB**, because `in_mapped_range`
(`memory.h:74-77`) routes those to the *same* device hook. Those ranges read the
zeroed VGA buffer at boot, and a blanket `0xFF`/`-1` would make the C leg return `0xFF`
at `0xB8000` and `-1` at `0xE0000000` where the reference returns `0` — trading the
divergence F1 names for a new one the brief's own note warns about.

**MEASURED, pinned npm reference, same `cpu.read8/read16/read32s` entry points the C
port mirrors** (`oracle/node_modules/v86` 0.5.471+g94faf3d, 2 MiB, oracle config):

```
addr 0x00200000  read8=0xff  read16=0xffff  read32=0xffffffff   <- unclaimed (F1)
addr 0x00300000  read8=0xff  read16=0xffff  read32=0xffffffff   <- unclaimed (F1)
addr 0x000b8000  read8=0x00  read16=0x0000  read32=0x00000000   <- VGA window  (NOT 0xFF)
addr 0x000bffff  read8=0x00  read16=0x0000  read32=0x00000000   <- VGA window  (NOT 0xFF)
addr 0xe07fffff  read8=0x00  read16=0x0000  read32=0x00000000   <- last LFB byte
addr 0xe0800000  read8=0xff  read16=0xffff  read32=0xffffffff   <- one past the LFB
```

The 8 MiB LFB is grounded in the reference source, not just the review's citation:
`v86/src/cpu.js:1193` unconditionally constructs
`new VGAScreen(..., settings.vga_memory_size || 8 * 1024 * 1024)`.

### The fix actually applied

`src/cpu/memory.c`: the two default read hooks now return the io.js:40-61 values for
*unclaimed* addresses and keep `0` for the two ranges the reference's always-present VGA
claims (the window and the 8 MiB LFB), via one predicate
`device_default_is_vga_blob(addr)`; write defaults stay no-ops. `src/cpu/memory.h:47-55`:
the wrong two-claim justification is replaced with the io.js:40-61 basis, the measured
values, and the VGA carve-out. `-Werror -Wall -Wextra` clean.

### G1 — the asymmetry (the proof)

Regression probe in `test_memory_router` (`src/cpu/cpu_selftest.c:796-814`): the
unclaimed values, plus the VGA-window/LFB preservation, plus the LFB boundary.

**PRE-FIX** (`device_read_default8/32` restored to `return 0` — the original source,
reconstructed byte-identically, `/tmp/memory.c.prefix`):

```
FAIL src/cpu/cpu_selftest.c:799: read8(MEM_SIZE + 0x100) == 0xFF (got 0x0, want 0xff)
FAIL src/cpu/cpu_selftest.c:800: read16(MEM_SIZE + 0x100) == 0xFFFF (got 0x0, want 0xffff)
FAIL src/cpu/cpu_selftest.c:801: read32s(MEM_SIZE + 0x100) == -1 (got 0x0, want 0xffffffffffffffff)
FAIL src/cpu/cpu_selftest.c:803: read8(MEM_SIZE + 0x100) == 0xFF (got 0x0, want 0xff)
FAIL src/cpu/cpu_selftest.c:804: read32s(MEM_SIZE + 0x100) == -1 (got 0x0, want 0xffffffffffffffff)
FAIL src/cpu/cpu_selftest.c:814: read32s(0xE0800000u) == -1 (got 0x0, want 0xffffffffffffffff)
FAIL src/cpu/cpu_selftest.c:828: read32s(MEM_SIZE + 0x100) == -1 (got 0x0, want 0xffffffffffffffff)
cpu_selftest: 2415 checks, 7 failures                                   EXIT=1 (make EXIT=2)
```

The six VGA-window/LFB checks (lines 807-812) PASS in this same run — the asymmetry is
specific to the unclaimed fallback, which is what makes the carve-out testable.

**POST-FIX**: `cpu_selftest: 2415 checks, 0 failures` — EXIT=0.

## F2 — mkfixture's mm comment and ordering (NICE-TO-HAVE)

`tests/mkfixture.c:164-171`. The comment said "physical mm = low 64 bits of st" while
the loop read slots `0..7` directly, i.e. **logical** order — contradicting the file's
own measured layout (`126-136`: `st_space` slot `s` holds logical `st(s)` = physical
`(top+s)&7`, so physical register `p` lives at slot `(p-top)&7`). I fixed **both**: the
ordering now reads slot `(i - top) & 7`, and the comment says what is true.

This is a **behaviour change, not a comment fix** — but a latent one. Reachability is
UNCHANGED and remains **none**: the branch that consumes these words
(`tests/nasm_run.py:198-202`) is entered only when `fpu_tag == FPU_TAG_ALL_INVALID
(0xAAAA)`, and the reconstructed `ftag` always carries `0xffff0000` (`mkfixture.c:138`).
The reviewer's 0/123-fixture measurement is confirmed by construction. The *value* of the
fix is that the ordering now agrees with both legs that consume it — the reference
(`nasm_state.mjs:121` reads physical `fpu_st[0..7]`, from `run.js:376`) and the port
(`read_mmx64s(i) -> fpu_st[i]`, `src/cpu/cpu_state.c:48-50`) — so a later unit that makes
the branch reachable (emit the 16-bit tag word) gets a fixture that is right rather than
one that silently disagrees.

## G4 — behaviour changes get a probe

**F2**: `make nasm` is unchanged (the branch is unreachable), so the corpus cannot prove
the ordering. A direct probe that reproduces the loop's indexing decision on a synthetic
fxsave buffer (`/tmp/f2-order-probe.c`, top=3) shows the new indexing equals the
*physical* order and the old indexing does not:

```
top=3  slot s holds logical st(s), physical p at slot (p-top)&7
mm word p -> expected slot | OLD slot i | NEW slot (i-top)&7
  p=0 -> 5 | 0 | 5   (expected value 1005, new 1005, old 1000)
  p=3 -> 0 | 3 | 0   (expected value 1000, new 1000, old 1003)
  ...
F2 probe: PASS (new indexing == physical order; old != it)              EXIT=0
```

**F3**: `oracle/nasm_state.mjs:72-79` — **comment only, no behaviour change**. The
`14 -> SIGBUS` entry is declared as a deliberate divergence from `run.js:329-341` (which
recognises only `{0,6,13}` and aborts otherwise), stating why it is benign and what would
make it matter. Also declared here, in `results/r1f-fixes.md`.

## Gates

| Gate | Command | Result |
| --- | --- | --- |
| G1 | `make selftest` with pre-fix `memory.c` | **EXIT≠0**, the 7 F1 probe checks FAIL (above) |
| G1 | `make selftest` after the fix | **EXIT=0**, `cpu_selftest: 2415 checks, 0 failures` (2406 + 9 new) |
| G2 | `make selftest` | **EXIT=0**, `cpu_selftest: 2415 checks, 0 failures` |
| G3 | `make` (cosmocc, `-Werror -Wall -Wextra`) | **EXIT=0** |
| G3 | `make oracle` | **EXIT=0**, `check_transcript: transcript matches tests/model.py` |
| G3 | `make nasm` | **EXIT=0**, `Passed 113/113 tests`, 10 labelled skips — byte-unchanged vs baseline |
| G3 | `node --check oracle/nasm_state.mjs` | **OK** |
| G3 | `cc -Wall -Wextra tests/mkfixture.c` | **clean** |
| G4 | F2 probe (`/tmp/f2-order-probe.c`) | **EXIT=0**; F3 = comment only |

## Findings for the orchestrator

1. **The prescribed F1 fix was wrong in scope, not in value** (MEASURED above). The real
   fix scope-carves the two VGA ranges; the reviewer's blanket change would have
   introduced `0xFF`/`-1` at `0xB8000`/`0xE0000000` where the reference reads `0`. The
   reviewer's own correctness claims (`0xFF`/`-1` ≥ `memory_size`; `0` at `0xB8000`,
   `0xE0000000`) are both confirmed by direct measurement.
2. **A stand-in constant was introduced**: `VGA_DEFAULT_VRAM_SIZE` (`memory.c`) = 8 MiB,
   from `v86/src/cpu.js:1193`. It stands in for the reference's default
   `vga_memory_size`, which is `0` in this tree until M2 installs a real VGA. The LFB
   boundary is measurement-confirmed (`0xE07FFFFF` → 0, `0xE0800000` → `0xFF`), so M2
   should be able to replace the constant with `vga_memory_size` and delete the
   predicate.
3. **The scoped fix changes where the seam sits**: the "default device table" now
   emulates the reference's *boot VGA zeros* for two ranges as well as the unclaimed
   fallback. That is a design decision the reviewer did not prescribe, so this unit asks
   for an independent look (see REVIEW NEEDED).
