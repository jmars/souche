# VGA-U4 — SVGA/VBE, the LFB, and the real dirty bitmap

HANDOFF=v86c-vga, unit **U4** (the plan's §7 U4, the LAST unit of the lane; U0/U1/U2/U3's
records are `results/vga-u0.md` .. `results/vga-u3.md`). Rung: `engineer`.

**Claims in this document are marked MEASURED / READ / INTERPRETATION.** An unmarked
sentence is MEASURED on this host with the binary named.

---

## 1. What U4 is, and the one requirement it is named for

U4 lands the reference's **SVGA/VBE layer** on top of U3's proven planar path: the Bochs
dispi interface (0x1CE/0x1CF), the `svga_*` register file, bank switching through the
0xA0000 window, the 8/15/16/24/32-bpp LFB→RGBA fill, the LFB at 0xE0000000 (made real by
the dirty bitmap), and the **real `svga_mark_dirty` dirty bitmap** replacing the stub at
`src/cpu/memory.c` (the rust `svga_fill_pixel_buffer` port).

The requirement this unit is named for is **OUTPUT INVARIANCE**: the dirty bitmap exists
only so a *later* unit can skip recomputing unchanged pixels, while the reference's own
redraw is a full recompute.  If making the bitmap real changed a single document byte
against the reference, that would be a finding, not a new pin.  It does not — by
construction (§3) and by the byte-identical gate (§4).

| file | lines | what it is |
| --- | ---: | --- |
| `src/hw/vga.h` | +~30 | the dispi register file fields, `VGA_MAX_XRES/YRES/BPP`, the dest-buffer bound (2 MiPixels -> 2560×1600) |
| `src/hw/vga.c` | +~240 | the dispi ports (16-bit), banked window arms, the bpp-8 palette loop, `svga_mark_dirty` call sites, the deferred-disable commit |
| `src/cpu/memory.c` | +~178 | the dirty bitmap + `vga_mark_dirty`/`svga_mark_dirty`/`svga_fill_pixel_buffer` (the rust port) |
| `src/cpu/memory.h` | +~15 | the two declarations the seat requires (next to the existing `vga_mark_dirty`) |
| `src/hw/hw_selftest.c` | +~196 | case 9f (`test_vga_svga`): 8 probes, one per sub-arm, each with a measured mutation |
| `tests/vga-lfb.asm` | 132 | the differential image: dispi + LFB + bank switch, reads back in-guest |
| `tests/gate-vga-4.sh` | 146 | the differential gate (D1-D4) |
| `Makefile` | +10 | the `tests/vga-lfb.img` build rule |

`vga_screen.c` is **unchanged**: the document writer already serializes the dest buffer
raw RGBA bytes, which is exactly what the SVGA fill writes — the graphical document needs
no new code, only a real fill.

---

## 2. What the machine now answers differently (each the reference's own)

| surface | before U4 | after U4 | evidence |
| --- | --- | --- | --- |
| `0x1CE`/`0x1CF` (dispi) | unclaimed (`0xFF`) | the Bochs VBE register file, registered **16-bit-only** exactly as upstream (`vga.js:360-364` passes `undefined` for the 8-bit slot) | case 9f P-DISPI |
| the `0xA0000` window, SVGA | discarded (the U1 text store) | banked mode: `(addr-0xA0000 \| bank_offset)` selects a 64 KiB LFB bank, through the memory write path (so pages are marked dirty) | case 9f P-BANK + the image's in-guest read-back |
| the LFB at 0xE0000000 | live since U0 but untracked | writes now mark pages dirty; the fill reads them back as RGBA | case 9f P-BPP32/P-OFFSET |
| `svga_mark_dirty` | a `(void)addr;` stub | one bit per 4 KiB LFB page; `svga_fill_pixel_buffer` converts dirty pages and clears the bitmap | case 9f (the fill's output) + the byte-identical D1 |
| `svga_enabled` | always false | the dispi index-4 enable (with the Win9x deferred-disable and the `0x80` no-clear-mem arm) | case 9f P-DISPI |

---

## 3. OUTPUT INVARIANCE, by construction

The reference's single document-producing call is `screen_fill_buffer()` ONCE at the stop
(`oracle/run-vga.mjs`), which in SVGA mode calls `svga_fill_pixel_buffer(bpp, svga_offset)`.
That function iterates **only dirty pages** and then clears the bitmap.  Three facts make
the bitmap invisible to the document:

1. **The dest buffer is zeroed at allocation** — `vga_set_size_graphical`'s `needs_update`
   block zeroes `vga_dest_bytes` (the reference allocates a fresh zeroed wasm region), and
   `svga_mark_dirty()` runs in the same block (the reference's `vga.js:1181`).
2. **The first fill is therefore a full recompute** — every page is dirty, so "dirty pages
   only" == "all pages", and a skipped pixel (never written) reads the same zero a full
   recompute would produce.
3. **A re-enable with the same size re-uses the dest buffer** — `needs_update` is false, so
   the buffer is NOT re-zeroed and only newly-written pages are re-filled, exactly as the
   reference's `set_size_graphical` skips the realloc.  (Unreachable in a single-fill
   document, but the semantics match, so a later incremental-render unit is correct.)

So the dirty bitmap exists to skip work and is *provably* output-invariant: a skipped
recompute of an unchanged pixel is the only safe kind, and the unchanged pixel's value is
the allocation zero, which a full recompute also yields.

**The clear itself is MEASURED on the pinned reference, not assumed from the source.**  A
probe drove the pinned `libv86` (0.5.471+g94faf3d) through two back-to-back
`screen_fill_buffer()` calls with no write between them and read
`svga_dirty_bitmap_min_offset`/`max_offset` (wasm offsets 716/720) after each: fill 1 gave
`min=0x0, max=0x7fffff` (all pages dirty), fill 2 gave `min=0xffffffff, max=0xfff` — the
empty-bitmap sentinel, which proves fill 1 cleared the bitmap.  `v86/src/rust/cpu/vga.rs:160-162`
(at the pinned commit 94faf3d) is the source of that behaviour: `svga_fill_pixel_buffer`
ends with `for v in dirty_bitmap.iter_mut() { *v = 0 }`.  The port reproduces it exactly
(`svga_fill_clear_bitmap`), so a later incremental-render unit that reads the bitmap after a
fill sees the same clean state in both legs.

---

## 4. The differential (D1): byte-identical, the bitmap invisible

`tests/vga-lfb.img` self-programs 320x200x32 through dispi, paints the LFB, switches bank 1
through the window, reads everything back in-guest, and halts.  MEASURED at limit 2,000,000:

```
C leg:  256,460 bytes, dbc0eb85…  (payload 256,000 = 320*200*4)
JS leg: 256,460 bytes, dbc0eb85…
cmp: identical
```

header: `{"bpp":32,…,"graphical_mode":true,"instruction_count":256096,"mode":"graphical",
"payload_bytes":256000,"screen_height":200,"screen_width":320,"svga_enabled":true,…}` —
the `instruction_count` is compared, so a guest-visible register divergence fails even if
the pixels matched.

`sh tests/gate-vga-4.sh` → **0 FAILING assertion(s)**: D1 byte-identical, D2 the 7 distinct
colours + the five named pixels, D3 each leg twice byte-identical, D4 both legs halted.

The named pixels (each the reference's own 32bpp **BGRX**→RGBA shuffle, hand-computed):
`(0,0)`=red, `(319,0)`=green, `(0,1)`=blue, `(160,100)`=white, `(319,199)`=yellow, over a
0x40-gray base.

---

## 5. The selftest probes (case 9f) — one per sub-arm, each with a mutation

`test_vga_svga` drives the device through the dispi ports (16-bit `out`/`in`), writes the
LFB through the real memory router (which marks pages dirty), then
`vga_screen_fill_buffer` + `vga_dest_buffer`.  `hw_selftest` is now **960 checks, 0
failures, 0 skipped** (U3 was 915).

| probe (arm) | drives | mutation (applied, measured, reverted) | failures |
| --- | --- | --- | ---: |
| P-DISPI | the register round-trip, the enable, the derived dims + bank/memory-size reads | — (the enable/dispi wiring; the gate's instruction_count covers any cross-leg drift) | — |
| P-BPP32 | 3 LFB dwords → dest RGBA | `(dword >> 16) & 0xFF` -> `(dword >> 24) & 0xFF` (read the X byte as R) | 2 |
| P-BPP24 | 3 BGR pixels, unaligned 4-byte reads | `3 * i` -> `4 * i` (the pixel stride) | 2 |
| P-BPP16 | 5-6-5 BGR words | blue scale `/ 0x1F` -> `/ 0x3F` | 1 |
| P-BPP15 | 5-5-5 BGR words | blue shift `>> 10` -> `>> 11` | 1 |
| P-BPP8 | the palette loop (vga.js, NOT the rust fill) | palette index `vga_mem8[base+i]` -> `0` | 3 |
| P-BANK | bank 1 through the 0xA0000 window | `x \| bank_offset` -> `x` (ignore the bank) | 3 |
| P-OFFSET | `svga_offset_x` page flip | `svga_fill_pixel_buffer(bpp, svga_offset)` -> `(bpp, 0)` | 1 |

The two probes that a silent "fix" would have broken, pinned rather than hidden:

1. **The 15/16bpp extraction is BGR, not RGB.**  The reference reads `r = word & 0x1F`
   (LOW 5 bits), `b = word >> 11` (HIGH 5 bits), and builds the dest u32 as
   `r<<16 | g<<8 | b | 0xFF000000` — whose little-endian bytes are `[b,g,r,A]`, the
   OPPOSITE of the 32/24bpp shuffle.  A textbook RGB565 "fix" would diverge.  The probes
   assert the raw bytes, so the quirk is pinned.
2. **The 24bpp byte order is BGR too** (blue at byte 0, the 32bpp shuffle reused).  The
   first probe draft wrote RGB and the fill correctly produced blue — the PROBE was wrong,
   not the fill; the fix was the BGR fixture, not the device.

---

## 6. The guest gates — the boundary re-verified, and nothing moved

U4 made the dispi ports and the LFB's dirty bitmap real, and a self-programming dispi guest
now reaches them — so the brief's THING 2 requires the re-run, published not re-pinned:

| gate | result |
| --- | --- |
| `make` | rc 0 (binary sha256 `a8fdab69…`, rebuilt after the last source edit) |
| `make selftest` | rc 0 — `hw_selftest: 960 checks, 0 failures, 0 skipped` |
| `make nasm` | rc 0 — `Passed 118/118` |
| `make transcript-diff` | rc 0 — byte-identical |
| `make determinism` | rc 0 |
| `make bench` | rc 0 — `instr=200095214` (the bench pin held) |
| `sh tests/gate-vga-0.sh` | rc 0 |
| `sh tests/gate-vga-1.sh` | rc 0 (D1 still byte-identical — U1's text path NOT weakened) |
| `sh tests/gate-vga-2.sh` | rc 0 (D1 still byte-identical — U2's mode-13h path NOT weakened) |
| `sh tests/gate-vga-3.sh` | rc 0 (4 planar images byte-identical — U3 NOT weakened) |
| **`sh tests/gate-vga-4.sh`** (new) | rc 0 — §4 |
| `sh tests/gate-m2-5-determinism.sh` | rc 0 — **both runs 4,000,096,757** (the pin DID NOT MOVE) |
| `sh tests/gate-m2-5-guest.sh` | rc 0 — `0 FAILING` |
| `sh tests/gate-pae-1.sh` | rc 0 — `11 passed, 0 failed` |
| `sh tests/gate-pae-l3.sh` | rc 0 — `0 FAILING` |
| `make minibios-mem` + `sh tests/gate-reboot-1.sh` | rc 0 — `39 passed, 0 failed` |

Nothing moved: the determinism count is still **4,000,096,757** on both runs, the bench pin
**200,095,214** held, and no serial or count moved.  The guest-gate result files
(`results/m2-5-guest-armA-*`, `results/pae-l3-*-serial.txt`) were restored to HEAD after the
runs — they are gate artifacts, not U4 deliverables.

---

## 7. Deviations and honest notes

1. **`src/cpu/memory.h` gained the two declarations the seat requires** (`svga_mark_dirty`,
   `svga_fill_pixel_buffer`, plus the `svga_dest_bytes`/`svga_dest_pixels` externs).  The
   brief's write scope named `memory.c` only; the stub it replaces already had its
   declaration in `memory.h`, and a function defined in `memory.c` and called from `vga.c`
   needs a header home — this is the seat's own header, not an unrelated file.  Stated, not
   hidden.
2. **The dest-buffer bound rose 2 MiPixels -> 2560×1600 = 4,096,000** (16 MiB of RGBA).
   The old bound was explicitly non-SVGA (1,052,640 px); the reference's `MAX_XRES×MAX_YRES`
   clamp makes 4,096,000 the SVGA maximum, and the static is sized to the larger.  The U3
   bound's comment is preserved (the non-SVGA maximum is still what it names).
3. **The 24bpp fill's last page reads 2 bytes into the +16-byte VRAM slack** (the unaligned
   4-byte read at the last pixel), which is zeroed — the slack's purpose (same argument as
   `read64s` at the last LFB byte).  The reference's release is UB at the same point; the
   port bounds it to the slack.  Unreachable in the gates (a small mode's last LFB page maps
   past the dest buffer and is skipped), but safe.
4. **The dispi offset (`svga_offset`) page-skip is the reference's own.**  The rust fill
   computes `dest_offset = off/bytes_per_pixel - svga_offset` and skips a page whose
   `dest_offset < 0`; a non-page-aligned offset therefore leaves the window's first partial
   page black.  Reproduced exactly (a later incremental-render unit must not "fix" it, or
   the document diverges).  P-OFFSET uses a page-aligned offset (1024 px) so its
   expectation is unambiguous; the image uses offset 0.
5. **`svga_bank_offset` is stored as the bit pattern `value << 16`** and read back with an
   unsigned `>> 16` (the reference's `>>> 16`); the banked-window `x | bank_offset` where
   `x = addr - 0xA0000` is a 17-BIT offset (the window is 0x20000 bytes), so the window's
   first 64 KiB selects bank B and its SECOND 64 KiB ORs bit 16 in and selects bank B|1 —
   bank+1 for an even bank, an alias of the first 64 KiB only for an odd bank (e.g.
   `0x10000 | 2<<16 = 0x30000`).  The reference's own arithmetic, reproduced.
6. **The dispi enable's `svga_memory.fill(0)` and the deferred-disable (FLAG_VM) arm are
   ported** but neither is reached by the shipped images or probes (no VM86 guest, no
   same-size re-enable).  They are stated as ported-but-unexercised: the fill(0) path is the
   same `memset` the enable already does, and the deferred-disable is a two-line flag dance
   on the FLAG_VM arm of the dispi write + the 0x3C0 commit.  Not given their own mutation
   (a VM86 guest would be needed to reach them), which is the honest residual.
7. **The SVGA DISABLE arm and the same-size re-enable arm are ported but un-gated.**
   `vga.c:1711-1717` (the `was_enabled && !svga_enabled` arm: `graphical_mode =
   attribute_mode & 1`, `set_mode`, `update_vga_size`, `set_font_bitmap`, `complete_redraw`)
   and the dispi index-4 re-enable with an unchanged size (whose `needs_update` is false, so
   the dest buffer is NOT re-zeroed and only newly-written pages are re-filled) are composed
   of U1-U3-gated pieces, but they have no U4 image or probe that drives the full
   disable/re-enable sequence.  Stated, not hidden.
8. **The un-gated list for U4's arms is empty for what the brief names** — every bpp arm,
   the bank switch, and the offset have a measured mutation; P-DISPI's round-trip is pinned
   by the differential's `instruction_count` (a wrong dispi answer shifts the count and
   fails D1) rather than a separate mutation.  What that list does NOT cover is items 6 and
   7 above (the enable/disable lifecycle arms), which are the honest residual.

---

## 8. What the lane now ships, and what remains for any future VGA work

- The whole `vga.js` device is now ported and gated: text mode (U1), mode 13h + the
  graphical document (U2), the planar 16-colour path (U3), and SVGA/VBE + the LFB + the
  dirty bitmap (U4).  `TODO.md` §1.2 is marked SHIPPED.
- **The one honest residual** (a capability the reference has that this port deliberately
  does not): incremental redraw.  The dirty bitmap is now real and output-invariant, so a
  later unit MAY port the browser frame loop's partial `update_buffer` — but per the plan's
  §8-R2 it must stay output-invariant (a skipped recompute of an unchanged pixel is the only
  safe kind).  Nothing in this tree consumes the bitmap except the stop-time full fill.
