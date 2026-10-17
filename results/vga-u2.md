# VGA-U2 — mode 13h, and the graphical document both legs produce byte-for-byte

HANDOFF=v86c-vga, unit **U2** (the plan is `handoff-v86c-vga-plan` §7's U2; U0/U1's records
are `results/vga-u0.md` and `results/vga-u1.md`). Rung: `engineer`.

**Claims in this document are marked MEASURED / READ / INTERPRETATION / PROJECTION.** An
unmarked sentence is MEASURED on this host with the binary named.

---

## 1. What U2 is, and at the same length what it is not

U2 lands the reference's **mode 13h and the graphical document** on top of U1's text mode:
the sequencer, the graphics controller, the graphical_mode flip, the shift modes 0x40/0x60,
`vga_replot`/`vga_redraw` (with the dest-buffer clamp quirk), the dest buffer, and
`set_size_graphical`/`update_vga_size`. It does **not** land the planar 16-colour latch
paths or the 0x00/0x20 shift modes *as a gated, proven feature* — those are U3's — but see
§6.1 for why they are ported here anyway.

| file | lines | what it is |
| --- | ---: | --- |
| `src/hw/vga.h` | 370 | + the sequencer/graphics register file, the diff ranges, the dest-buffer bound and getter |
| `src/hw/vga.c` | 1,750 | + the sequencer/graphics ports, `vga_memory_write_graphical`, read modes 0/1, the pixel pipeline (`vga_replot`/`vga_redraw`/`vga_screen_fill_buffer`), `set_size_graphical`/`update_vga_size`/`set_font_page` |
| `src/hw/vga_screen.c` | 1,047 | + the graphical document payload, `vga_screen_is_graphical` |
| `src/hw/hw_selftest.c` | +152 | case 9c (probes P1/P3/P5) |
| `tests/vga-mode13.asm` | 216 | the differential image: 16-colour ramp + edge cells, read modes 0/1 read-back |
| `tests/gate-vga-2.sh` | 157 | the gate (D1 differential, D2 non-triviality, D3 determinism, D4 the guest verdict) |
| `oracle/run-vga.mjs` | +~40 | the ImageData polyfill + the graphical serialization |

The three arithmetic details that a silent "fix" would have broken, all pinned:

1. **The address-to-pixel shift count** (`vga.js:1032-1048`): mode 13h's `underline 0x40 +
   crtc_mode 0xA3 + attribute_mode 0x41` make the shift **0**, so a pixel's plane byte is at
   the SAME address the chain-4 write stored it. A textbook "corrected" count would scatter
   the pixels.
2. **The dest-buffer clamp quirk** (`vga.js:2463-2507`): `vga_redraw` writes an `Int32Array`
   of `virtual_width*virtual_height`, but its loop end is clamped to
   `VGA_PIXEL_BUFFER_SIZE-1` — so a pixel past the dest buffer is a JS typed-array
   OUT-OF-RANGE NO-OP. Reproduced exactly as a bounded skip (`if (pixel_addr < buffer_length)`).
3. **The DAC 6-bit expand is the reference's own** (`vga.js:1680-1684`): `b = v & 1;
   v = v<<2 | b<<1 | b` — so `0x1F → 0x7F`, not `0x55`. The gate's edge pixels (purple
   `127,0,127`) pin this: a textbook expand would give `85,0,85` and FAIL.

---

## 2. What the machine now answers differently, and why each is the reference's own

| surface | before U2 | after U2 | evidence |
| --- | --- | --- | --- |
| `0x3C4`/`0x3C5` (sequencer) | unclaimed (`0xFF`) | the register file — clocking mode, plane write mask, character map select, memory mode (Chain 4 = bit 3); reads answer `0x01/0x02/0x03/0x04/0x12` | case 9c's chain-4 read-back |
| `0x3CE`/`0x3CF` (graphics controller) | unclaimed | the planar write pipeline (set/reset, rotate, bitmap, write modes 0-3), `plane_read`, `planar_mode` (0x40/0x60 shift modes), the memory-space select, colour compare/don't-care | case 9c's read-mode-1 colour-compare arm |
| `attribute_mode` bit 0 | flips `graphical_mode` (U1) but the graphical branch was a stub | the flip now reaches `set_mode` on the adapter and recomputes 320x200x(320x410 virtual) | case 9c P5 |
| the `0xA0000` window, graphical | write discarded (U1's stub) | `vga_memory_write_graphical` (chain-4), reads through read modes 0 and 1 | the image's own in-guest read-back |
| the graphical document | refused by both legs by name | `mode="graphical"`, payload = 320x410x4 raw RGBA dest bytes | the D1 differential below |

---

## 3. The differential (D1), and what makes it non-trivial

`tests/vga-mode13.asm` self-programs **320x200x256 chain-4** through the standard sequence,
loads a 16-entry DAC palette, paints `byte[A] = A & 0xF` over all 64,000 pixels, then
overwrites **five pixel-column/row edge cells** — `(0,0)=0x0E`, `(319,0)=0x0F`,
`(0,1)=0x0D`, `(160,100)=0x0B`, `(319,199)=0x0C` — and reads back through read mode 0
(chain-4) and read mode 1 (colour compare) before halting.

MEASURED, `tests/vga-mode13.img` (sha256 `53ac9573…`, 1,001 bytes) at limit 2,000,000:

```
C leg:  525,263 bytes, 142e3a96b3cc47522a0c954c671ba4f33ebc70b795cc754c1bbfe62371909cfc
JS leg: 525,263 bytes, 142e3a96b3cc47522a0c954c671ba4f33ebc70b795cc754c1bbfe62371909cfc
cmp: identical
```

header: `{"bpp":32,"cols":80,…,"graphical_mode":true,"instruction_count":384383,…,
"mode":"graphical","payload_bytes":524800,"rows":200,"screen_height":200,
"screen_width":320,"virtual_height":410,"virtual_width":320}` — the instruction count is
compared, so a guest-visible register divergence fails even when the pixels match.

The payload (320x410 RGBA) decodes to a 16-colour screen (16 distinct RGBA values) with the
exact edge pixels — `(0,0)=127,0,127`, `(319,0)=0,127,127`, `(0,1)=255,127,0`,
`(160,100)=87,87,87`, `(319,199)=168,168,168` — each the reference's own 6-bit expand.

`sh tests/gate-vga-2.sh` → **0 FAILING assertion(s)**: D1 byte-identical, D2 the
16-colour/edge-pixel/geometry non-triviality, D3 each leg twice byte-identical, D4 both legs
halted after the image's own read-mode 0/1 read-back.

**This is the gate that is the point of the unit**: a blank or single-colour framebuffer, a
transposed colour lane, or a wrong pixel stride all FAIL D2 here, not silently pass.

---

## 4. The selftest probes (case 9c) — each FAILS on the pre-fix shape

| probe | what it drives | what it pins |
| --- | --- | --- |
| P5 | the mode-13h register sequence, then `0x3C0` index 0x10 = 0x41 | `graphical_mode` flipped, `set_mode` reached the adapter (`vga_screen_is_graphical`), dims 320x200x(320x410) |
| P3 | four window bytes `1,2,3,4`, then `vga_screen_fill_buffer` | dest pixels 0..3 are red/green/blue/white (the 0x40 shift mode's nibble pairing), pixels 4..7 black |
| P1 | DAC entry 5 = red (0x3F,0,0), pixel 0 = 5, fill | `vga256_palette[5] == 0xFF0000` and dest pixel 0 = `255,0,0,255` (the byte lanes) |
| read mode 1 | colour don't-care 0xF, compare 0xF over a 4-pixel 0xFF run | `read8(0xA0008) == 0xFF`, a non-matching run `!= 0xFF` |

`hw_selftest` is now **831 checks, 0 failures, 0 skipped** (U1 was 801).

**ASYMMETRY PROVEN** (the probes are not tautologies): mutating `vga_redraw`'s clamp to drop
pixel 0 (`pixel_addr != 0 && pixel_addr < buffer_length`) made case 9c FAIL (2 checks: P1's
and P3's pixel-0 assertions) and made `gate-vga-2.sh` D1 FAIL (the two documents differ).
Reverted; the binary's sha256 returned to `495e3285…`.

---

## 5. The guest gates — the boundary re-verified, and nothing moved

U0 measured that the kernel boot touches the VGA (24 CRTC port accesses + ~8k window bytes,
the firmware's `INT 10h` teletype at `0xB84C0`). U2 CLAIMED the sequencer and graphics
controller ports on top of U1's CRTC/attribute/DAC, so the kernel's `vgacon` probe now
receives the reference's answers across a wider port range. The guest gates were re-run on
U2's final binary and the boundary held:

| gate | result |
| --- | --- |
| `make` | rc 0 (binary sha256 `495e3285…`, rebuilt after the last source edit) |
| `make selftest` | rc 0 — `hw_selftest: 831 checks, 0 failures, 0 skipped` |
| `make nasm` | rc 0 — `Passed 118/118` |
| `make transcript-diff` | rc 0 — byte-identical |
| `make determinism` | rc 0 |
| `make bench` | rc 0 — `instr=200095214` on all 7 runs, 240.50 MIPS at best |
| `sh tests/gate-vga-0.sh` | rc 0 |
| `sh tests/gate-vga-1.sh` | rc 0 (D1 still byte-identical on the U1 text image) |
| **`sh tests/gate-vga-2.sh`** (new) | rc 0 — §3 |
| `make minibios-mem` + `sh tests/gate-reboot-1.sh` | rc 0 — `39 passed, 0 failed` |
| `sh tests/gate-m2-5-determinism.sh` | rc 0 — **both runs 4,000,096,757** |
| `sh tests/gate-m2-5-guest.sh` | rc 0 — `0 FAILING` |
| `sh tests/gate-pae-1.sh` | rc 0 — `11 passed, 0 failed` |
| `sh tests/gate-pae-l3.sh` | rc 0 — `0 FAILING` |

**The pinned number did not move**: the m2-5 determinism count is still **4,000,096,757** on
both runs (the U0 pin), the serial sha is unchanged, and no serial line moved. Claiming the
sequencer and graphics-controller ports — the ports a real guest's `vgacon`/mode probe would
touch — moved nothing, which is the claim that needed checking (the brief's THING 2).

---

## 6. Deviations from the plan (each named, each with its reason)

1. **The planar write pipeline, the read modes 0/1, and all four replot shift modes are
   ported WHOLE, not just the mode-13h subset.** The plan scopes U2 to "shift modes
   0x40/0x60" and assigns the write modes/latches/0x00/0x20 shifts to U3. But in the
   reference those are ONE `switch` each (`vga_memory_write_graphical`, `vga_memory_read`,
   `vga_replot`), and splitting them would leave a partial function with a silent-wrong
   divergence surface — the plan's own R1. U2's gate exercises write mode 0 + chain-4 +
   shift 0x40 + the 8-bit redraw; U3's gate will PROVE the rest (the plan's segmentation is
   about which guest-visible feature is gated by which unit, not about splitting a single
   function). U3's implementer should treat the un-gated arms as ported-but-unproven.
2. **`oracle/run-vga.mjs` now polyfills `ImageData`.** The plan's Q4/d says "call
   `vga.screen_fill_buffer()` then serialize `vga.image_data.data`", but the reference's
   `set_size_graphical` creates `image_data` ONLY when `typeof ImageData !== "undefined"`
   (vga.js:1174) — and node has no `ImageData`. Measured: without the polyfill, a graphical
   run leaves `image_data` null and `screen_fill_buffer` throws on `.data`. The polyfill is a
   thin wrapper over the wasm `Uint8ClampedArray` (the reference's own browser primitive),
   NOT a reimplementation of pixel math — the harness still replays the device's own
   `screen_fill_buffer`/`vga_replot`/`vga_redraw`. This is a plan error caught by measuring,
   not by editing prose.
3. **The dest-buffer clamp quirk is reproduced as a bounded skip** (`if (pixel_addr <
   buffer_length)`), not as an overrun or a "corrected" clamp — the R4 rule (a guest can
   reach it, so it must never be an abort or an unbounded index).
4. **`tests/vga-mode13.asm` sets sequencer memory mode (SR4) to `0x08`, not `0x06`.** The
   reference's chain-4 test is `sequencer_memory_mode & 0xC ∈ {0x8, 0xC}` (vga.js:723-727),
   i.e. bit 3 — some references quote 0x06 for SR4, but 0x06 lacks bit 3 and would NOT chain
   in the reference. 0x08 is the reference's chain-4 encoding.
5. **The document adapter's text grid still refuses an 80x200 intermediate size** (the
   U0/U1 256x128 bound) while the image programs its CRTC in text mode before the
   graphical flip. It is REPORTED (stderr), it is a no-op for the graphical document (the
   payload is the dest buffer, and the header's cols/rows come from the device, not the
   adapter), and the byte-identical D1 proves it changes nothing. Flagged for U3 or a future
   80x200 text gate: a guest that SETTLES in an 80x200 text mode would hit the bound, which
   the reference (a Map recorder) would not.

## 7. What U3 inherits (the flip sites, so the next unit is not guessing)

- The planar write modes 1-3, the read mode 1 colour-compare, the odd/even (chain-2)
  addressing, and the 0x00/0x20 shift modes are **ported but un-gated** in `vga.c` (§6.1):
  U3's job is to prove them with images self-programming 0x12/0x0D/0x0E and gate-vga-3.sh.
- `svga_enabled` is still always false; the `vga_memory_read`/`write` svga arms and the
  dispi registers (0x1CE/0x1CF) are U4's.
- The `vga_redraw` 4-bit path (`attribute_mode & 0x40 == 0`, `dac_map`/`color_plane_enable`)
  is ported and shares the same clamp quirk as the 8-bit path.
- `vga_addr_to_pixel`'s row-substitution arm (crtc_mode & 3 != 3) is ported and only reached
  by planar panning — U3 should verify its int32 semantics against the reference's JS (the
  reviewer flags the C's signed `>>` vs the JS zero-fill `>>>` there; mode 13h never takes
  the arm).

## 8. Round 2 — the reviewer's four should-fix defects, applied and gated

The review accepted U2's gated claim and found **four should-fix defects in the UN-GATED
arms**, one of them guest-reachable-host-abort-class. Each was applied as the exact hunk
(A-G) on `handoff-v86c-vga-u2-fix-1`, and each is now gated by a new hw_selftest case 9d
probe that FAILS on the pre-fix shape and passes after.

### The fixes (all byte-exact against the reference's JS semantics)

| # | finding | fix (hunk) | where |
| --- | --- | --- | --- |
| 1 | **DIVISION BY ZERO** — `vga_addr_to_pixel`'s substitution arm and `vga_replot` divided by `virtual_width`, which is 0 by default (crtc_mode defaults to 0): a guest flipping attribute_mode bit 0 with unprogrammed CRTC then writing 0xA0000 SIGFPEs ~4 instructions in | guard the divisions (`row=0,col=0` when vw==0, matching JS `Infinity|0=0`/`NaN`) and guard the write path's `partial_replot` call with `virtual_width > 0` (JS: NaN comparisons no-op) | `vga.c:392-393` (B), `:801-802` (C), `:681` (A) |
| 2 | **OOB STATIC WRITES** — negative diff-range starts (start_address > addr through the substitution arm) reach `vga_replot`/`vga_redraw` and index `vga_pixel_buffer`/`vga_dest_bytes` negatively; JS typed-array writes are no-ops, C corrupts statics | `if (start < 0) start = 0;` at the top of both loops (the negative iterations are the JS no-op) | `vga.c:788` (D), `:885` (E) |
| 3 | **OOB READ** — read mode 0 with `plane_read >= 4` indexed ~16.7 MiB past the 256 KiB planes static (JS yields 0) | `if (plane > 3) return 0;` before the final read-mode-0 return | `vga.c:743` (F) |
| 4 | **SILENT DIVERGENCE** — the dest buffer was not re-zeroed on a size change, where the reference allocates a fresh zeroed wasm region (a mid-run CRTC offset write leaves stale pixels in C) | `memset(vga_dest_bytes, 0, sizeof(vga_dest_bytes))` inside the `needs_update` block | `vga.c:448` (G) |

### The gates (hw_selftest case 9d, `test_vga_planar_defects`)

| probe | drives | fails before / passes after |
| --- | --- | --- |
| R1 | fresh machine, `0x3C0=0x10,0x01` (graphical, no CRTC dims), then `write8(0xA0000, 0x42)` | **SIGFPE (exit 136, "Floating point exception")** / clean |
| R2 | mode 13h, `crtc_mode=0xCC` + `start_address=0xFFFF`, canary the dest static's unused tail, poison + fill | dest-tail canary clobbered (`FAIL … intact`) / clean |
| R3 | mode 13h, no chain-4, `plane_read=4`, a one-past-planes canary via the LFB base | leaks the adjacent static's `0x5A` / reads 0 |
| R4 | mode 13h, DAC[5]=red, pixel 0=5, fill, then a mid-run `CRTC 0x13=0x30` resize | stale red (`0xFF`) / freshly zeroed (`0x00`) |

`hw_selftest` is now **842 checks, 0 failures, 0 skipped** (U2 round 1 was 831). The asymmetry
arms are MEASURED, not asserted: reverting A+B+C reproduces the SIGFPE (exit 136); reverting
D+E makes R2 FAIL; reverting F makes R3 FAIL (`0x5A` leaked); reverting G makes R4 FAIL
(`0xFF` stale). Restored after each; the final binary's sha256 is `5417ae88…`.

### Honesty about the un-gated list after Round 2

What mode 13h exercises and gate-vga-2 + case 9c PROVE: write mode 0 identity, chain-4
write+read, shift 0x40, pel_width 8-bit, the plain `addr<<shift` arm (shift 0), the DAC
6-bit expand + byte lanes, read mode 1 colour compare, the clamp's skip region, full-range
redraw. What case 9d's R1-R4 now PROVE (the defect arms): the div-by-zero guard, the
negative-start clamp, the `plane_read>=4` read, and the resize re-zero. **Still unproven,
and U3's gate must cover** (the reviewer's finding-3 note): write modes 1/2/3 with
`latch_dword`, rotate count != 0 and the 0x18 logical ops, setreset + setreset_enable,
partial `plane_write_bm` masks, chain-2 (odd/even) write, shift 0x00/0x20 with pel_width=0
(4-bit redraw through `dac_map` + `color_plane_enable`), attribute_mode 0x80 colorset
(mask 0xCF/0x30, color_select 0xC0), the substitution arm's negative/NaN semantics and the
C signed `>>` vs JS zero-fill `>>>` on negative values, odd/even host read
(`planar_mode & 0x10`), read mode 0 `plane_read` 1-3, `palette_source=0` / clocking 0x20
clear path, panning + split screen IF an adapter ever consumes layers, and CRTC offset
changes mid-run beyond the resize path R4 covers.

One caveat worth recording honestly: R2/R3's canaries read the device's STATIC layout (the
dest tail is one-past-`vga_planes` in this build's `.bss` order), which is compiler/linker
dependent. The `if (start < 0)`/`if (plane > 3)` fixes are the load-bearing guarantee (each
is byte-exact against the reference and reviewed); the canaries are a build-local
demonstration that the pre-fix code really did write/read out of bounds. If the linker ever
reorders the VGA statics, the R2/R3 probes would pass vacuously and should be re-derived
from `&vga_dest_bytes`/`&vga_planes` — the div-by-zero (R1) and resize (R4) probes are
layout-independent and stay sharp.

