# VGA-U1 — the text-mode device, and the differential that closes the colour lane

HANDOFF=v86c-vga, unit **U1** (the plan is `handoff-v86c-vga-plan` §7's U1; U0's record is
`results/vga-u0.md`). Rung: `engineer`.

**Claims in this document are marked MEASURED / READ / INTERPRETATION / PROJECTION.** An
unmarked sentence is MEASURED on this host with the binary named.

---

## 1. What U1 is, and at the same length what it is not

U1 lands the reference's **text-mode decode** on top of U0's seats: the CRTC, the attribute
controller, the DAC, the text-mode window handlers, and the redraw/`put_char` arithmetic. It
does **not** land the sequencer (`0x3C4`/`0x3C5`) or the graphics controller
(`0x3CE`/`0x3CF`) — those are U2/U3's, and their register values are the constructor defaults
(`plane_write_bm 0xF`, `plane_read 0`, `miscellaneous_graphics_register 0`), so the window
decode always selects memory space 0 (`0xA0000`) and the text write path always runs.

| file | lines | what it is |
| --- | ---: | --- |
| `src/hw/vga.h` | 296 | the seam + the register file (the constructor defaults, `vga.js:78-304`) + the document-cell reader |
| `src/hw/vga.c` | 1,140 | the CRTC/attribute/DAC ports, `vga_memory_write_text_mode`, `text_mode_redraw`, `put_char`, `update_vga_size`→`set_size_text`, the window handlers, the init sequence |
| `src/hw/vga_screen.c` | 1,019 | + `vga_screen_document_cell` (the recorder's reader, for the selftest probes) |
| `src/hw/hw_selftest.c` | +135 | case 9's flip sites + case 9b (probes P2/P4) |
| `tests/vga-text.asm` | 140 | the differential image: draws cells with **bg != fg**, a non-zero cursor, one blinking cell |
| `tests/gate-vga-1.sh` | 146 | the gate (D1 differential, D2 non-triviality, D3 determinism, D4 the guest verdict) |

The two arithmetic details that a silent "fix" would have broken, both pinned:

1. **The DAC 6-bit expand is the reference's own, not the textbook `v<<2|v>>4`.**
   `vga.js:1680-1684` is `b = v & 1; v = v<<2 | b<<1 | b` — so `0x3F → 0xFF` but `1 → 7`,
   not `4`. Reproduced exactly (`vga.c` `vga_port3C9_write`); a "corrected" expand diverges
   the document's palette by up to 3 per component.
2. **The CRTC start-address read-back is swapped** (`vga.js:1902-1907`): `0x0C` returns
   `start_address & 0xFF` (the LOW byte) and `0x0D` returns `start_address >> 8` (the HIGH
   byte), while the writes are unswapped. Reproduced, pinned in `hw_selftest` case 9b.

---

## 2. What the machine now answers differently, and why each is the reference's own

| surface | before U1 | after U1 | evidence |
| --- | --- | --- | --- |
| the `0xA0000`-`0xBFFFF` window | read 0, discard the write (U0's stand-in) | `vga_memory_write` stores text mode **linearly** and `vga_memory_read` returns it — a write-then-read at `0xB8000` returns the byte (space 0 → `vga_planes[0x18000]`) | `hw_selftest` case 9's flipped flip sites; the image's own in-guest read-back |
| the CRTC ports `0x3D4`/`0x3D5` + `0x3B4`/`0x3B5` | unclaimed (`0xFF`) | the register file, with the start-address/offset/line-compare/cursor writes driving `update_vga_size`→`set_size_text`, `update_layers`, `update_cursor` | case 9b; the gate's 8×4 grid |
| the attribute controller `0x3C0`/`0x3C1` | unclaimed | the index/data flip-flop, `dac_map[0..15]`, `attribute_mode` (bit 0 flips `graphical_mode`), `color_plane_enable`, `horizontal_panning`, `color_select` | case 9b's `dac_map` identity + blink arm |
| the DAC `0x3C6`-`0x3C9` | unclaimed | `dac_mask`, `dac_state`, the read/write indices, `vga256_palette` with the 6-bit expand + byte-lane rotation | case 9b's palette 0xFF0000/0x00FF00 |
| the misc/status ports `0x3C2`/`0x3CC`/`0x3CA`/`0x3DA`/`0x3BA` | unclaimed | the misc output register and the input-status flip-flop reset | case 9b (the `0x3DA` read resets the attribute flip-flop) |

The **sequencer and graphics controller stay unclaimed** — that is a decision, not a gap:
their registers are the constructor defaults and the text decode never needs them to move.

---

## 3. The differential (D1), and what makes it non-trivial

`tests/vga-text.asm` self-programs an **8×4** text mode (hdisp=7, hblank=8, max_scan_line=3,
vdisp=15, vblank=16, offset=4) through the DAC/attribute/CRTC ports, draws 12 cells with
**bg != fg** across 7 distinct DAC colours, one **blinking** cell (attribute_mode bit 3 + an
attribute with bit 7), a **non-zero cursor** (address 3 → row 0, col 3) and a visible cursor
scanline (0..3), then reads two cells back and halts only on a match.

MEASURED, `tests/vga-text.img` (sha256 `87f21948…`, 526 bytes) at limit 1,000,000:

```
C leg: 760 bytes, 31d82dc85d7a21ab819262c45f100019d0f827c28fdab3c075f207bf147efb4b
JS leg: 760 bytes, 31d82dc85d7a21ab819262c45f100019d0f827c28fdab3c075f207bf147efb4b
cmp: identical
```

header: `{"bpp":32,"cols":8,"cursor_col":3,"cursor_row":0,"cursor_scanline":{"end":3,
"start":0,"visible":true},"format":1,"graphical_mode":false,"image_bytes":526,
"image_sha256":"87f21948…","instruction_count":283,"instruction_limit":1000000,"mode":"text",
"payload_bytes":320,"rows":4,…}`

the payload, decoded: row 0 `VGA-U1OK` with every bg != fg, row 1 `test` + four zero cells,
one blinking `*` at (2,0) (`flags=0x01, bg=0x000000, fg=0xffff00`), the rest zeros.

`sh tests/gate-vga-1.sh` → **0 FAILING assertion(s)**: D1 the two documents are
byte-identical, D2 the grid/cursor/scanline/non-trivial-chars/bg!=fg/blink counts, D3 each
leg twice byte-identical, D4 both legs halt after the image's own read-back.

**This is the gate that closes U0's M2′ finding** (`results/vga-u0.md` §6): on a VGA-blind
image every cell is `bg=fg=0`, so a colour-lane transposition was invisible to the D0
differential. On this image 13 cells have `bg != fg` and the differential compares their
bytes, so a transposition now FAILS.

---

## 4. The selftest probes (case 9b) — each FAILS on the pre-fix shape

The pre-fix shape is U0 (ports unclaimed, window discard). Every probe below drives the
DEVICE through the document adapter swapped in as the reference harness swaps its recorder
(`oracle/run-vga.mjs:151`), so it pins the exact `(chr, flags, bg, fg)` `put_char` received,
not a collapsed document.

| probe | what it drives | what it pins |
| --- | --- | --- |
| P2 | a window write of `'A'` + attr `0x21` against `dac_map` identity and palette 1=red/2=green | `put_char` got `('A', flags=0, bg=0x00FF00, fg=0xFF0000)` — the `dac_mask & dac_map[color>>4 & mask]` / `[color & mask]` decode, bg then fg |
| P2 blink | `attribute_mode=0x08` + a window write of attr `0x87` | `flags=0x01` (FLAG_BLINKING on the document adapter), bg mask narrowed to 3 bits (bg=0, fg=palette[7]=0) |
| P4 | `start_address=2`, `line_compare=4` against a 4×4 grid | row 0 reads `vga_planes[2*start]` (`'X'`), the split-screen row 1 resets addr to 0 and reads linearly (`'Y'`, `'B'`) |
| quirk | `0x3D4=0x0C`/`0x0D` reads of `0x3D5` | the swapped start-address read-back |

`hw_selftest` is now **801 checks, 0 failures, 0 skipped** (U0 was 778).

---

## 5. The guest gates — the boundary re-verified, and nothing moved

U0 measured that the kernel boot touches the VGA (24 CRTC port accesses + ~8k window bytes,
the firmware's `INT 10h` teletype at `0xB84C0`). U1 **claimed** the CRTC ports, so their
answers changed from `0xFF` to the device's — the brief's "THING 1". The guest gates were
re-run on U1's final binary and the boundary held:

| gate | result |
| --- | --- |
| `make` | rc 0 |
| `make selftest` | rc 0 — `hw_selftest: 801 checks, 0 failures, 0 skipped` |
| `make nasm` | rc 0 — `Passed 118/118` |
| `make transcript-diff` | rc 0 — byte-identical |
| `make determinism` | rc 0 |
| `make bench` | rc 0 — `instr=200095214` on all 7 runs, 235 MIPS at best (U0 was ~239, inside the host's band) |
| `make minibios-mem` + `sh tests/gate-reboot-1.sh` | rc 0 — `39 passed, 0 failed` |
| `sh tests/gate-m2-5-determinism.sh` | rc 0 — **both runs 4,000,096,757**, serial sha `8d360dfc…` byte-identical |
| `sh tests/gate-m2-5-guest.sh` | rc 0 — `0 FAILING` (the C-vs-JS serial multiset holds) |
| `sh tests/gate-pae-1.sh` | rc 0 — `11 passed, 0 failed` |
| `sh tests/gate-pae-l3.sh` | rc 0 — `0 FAILING` |
| `sh tests/gate-vga-0.sh` | rc 0 — D0 still byte-identical on the VGA-blind image; S3's count still the pin |
| **`sh tests/gate-vga-1.sh`** (new) | rc 0 — §3 |

**The pinned number did not move**: the m2-5 determinism count is still **4,000,096,757** on
both runs (the U0 pin), and the serial sha (`8d360dfc…`) matches the committed
`results/m2-5-det-run1-serial.txt`. The kernel's `vgacon` probe now receives the reference's
CRTC answers, which is what the JS leg always answered — the legs CONVERGE, and no count or
serial line moved, so there is nothing to re-pin. (One expected reading change in
`gate-vga-0.sh`'s S3: its "VGA port accesses" count went 24 → 0 because `V86C_LOG_PORTS`
names only UNCLAIMED ports; the window-access count (8,083) and the instruction-count pin
(200,095,214) are the real falsifiers and both held.)

---

## 6. Deviations from the plan (each named, each with its reason)

1. **The replay is `set_size_text` + `text_mode_redraw` + `update_cursor` +
   `update_cursor_scanline`** (`vga.c` `vga_write_document`), not just the three calls the
   reference harness makes. The extra `set_size_text(max_cols, max_rows)` is the reference's
   own set_state text branch (`vga.js:557`) and a C-only bookkeeping step: the reference
   recorder is a `(row,col)`-keyed Map with no grid, so its `set_size_text` is a no-op, but
   the C document adapter must know its grid size before `put_char` will record into it.
   It changes no document byte (the cells it would otherwise drop are the zero cells the
   writer emits for an unsized grid anyway) — verified byte-identical on `tests/test.img`.
2. **`vga_screen_document_cell`** was added to the seam's document adapter — a reader for the
   recorder, so the selftest probes can pin `(chr, flags, bg, fg)` without serializing a
   document. It is the document adapter's analogue of `vga_screen_headless_text`.
3. **The sequencer/graphics ports stay unclaimed** (U0's plan text said "the full port map
   answering the reference values" landed in U0; U0 deferred it and the brief's later-unit
   rule keeps them U2/U3's). Named in `vga.h` so the gap is not read as a prefix.

---

## 7. What U2 inherits (the flip sites, so the next unit is not guessing)

- The **sequencer** (`0x3C4`/`0x3C5`) and **graphics controller** (`0x3CE`/`0x3CF`) ports are
  unclaimed and their register values are the constructor defaults; U2 owns them, including
  the `miscellaneous_graphics_register` memory-space select (which makes `0xB8000` the real
  text window instead of the space-0 alias U1 uses).
- `graphical_mode` only flips through the attribute controller's bit 0 (`vga_port3C0_write`
  case 0x10); the graphical document is refused by both legs by name (U2's).
- `update_vga_size`'s graphical branch and `complete_redraw`/`complete_replot`'s graphical
  arms are stubs returning in text mode; U2 fills them (mode 13h, `set_size_graphical`,
  the dest buffer, `update_buffer`).
- The `vga_plane_byte` bound (a text-redraw address past 256 KiB reads 0) is the R4 hard
  bound; the graphical pixel buffer will need the same care.
