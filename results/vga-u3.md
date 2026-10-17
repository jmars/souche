# VGA-U3 — the planar 16-colour modes, proven arm by arm

HANDOFF=v86c-vga, unit **U3** (the plan's §7 U3; U0/U1/U2's records are `results/vga-u0.md`,
`results/vga-u1.md`, `results/vga-u2.md`). Rung: `engineer`.

**Claims in this document are marked MEASURED / READ / INTERPRETATION.** An unmarked sentence
is MEASURED on this host with the binary named.

---

## 1. What U3 is, and the discipline it is named for

U3 proves the planar 16-colour path that U2 ported WHOLE but left UN-GATED
(`results/vga-u2.md` §8's list): write modes 1-3 with the latches, rotate counts != 0, the
0x18 logical ops, setreset + setreset_enable, the partial bitmask, chain-2 (odd/even), the
shift modes 0x00/0x20 with pel_width 0, the attribute_mode-0x80 colorset arm, read mode 0
`plane_read` 1-3, read mode 1 colour compare, odd/even host read, and the row-substitution
arm (`crtc_mode & 3 != 3`).

The discipline this unit is named for (R1): **an arm that no image reaches is an arm nobody
has checked.** Every sub-arm U3 touches is reached by an image *or* a hw_selftest probe, and
for each one a mutation is shown that flips it (or the arm is stated plainly as unreachable).
The image set mixes read modes 0/1 with write modes 0-3 and both shift modes on one frame,
because R1's failure mode is a sequence-dependent divergence a homogeneous image cannot see.

| file | lines | what it is |
| --- | ---: | --- |
| `src/hw/vga.h` | +~10 | `VGA_DEST_BUFFER_MAX_PIXELS` 1 MiB -> 2 MiB (the true maximum is 1,052,640, not 526 K) |
| `src/hw/hw_selftest.c` | +396 | case 9e (`test_vga_planar`): 18 probes, one per sub-arm, each hand-computed from vga.js |
| `tests/vga-planar12.asm` | 336 | mode 0x12: all four write modes + rotate/logical/setreset/bitmask + colorset on ONE frame |
| `tests/vga-planar4.asm` | 197 | packed shift 0x20 (the CGA 4-colour interleave) |
| `tests/vga-planar0d.asm` | 205 | mode 0x0D (320x200x16, word mode — a different addr-to-pixel shift) |
| `tests/vga-planarsub.asm` | 184 | the row-substitution arm across three rows |
| `tests/gate-vga-3.sh` | 167 | the differential gate (D1-D4) over all four images |
| `Makefile` | +29 | the four image build rules |

`vga.c` is **unchanged** by U3 — U2's port of the planar pipeline was byte-exact against the
pinned reference, and U3's job was to *prove* it, not to rewrite it.

---

## 2. The one code change U3 did need: the dest-buffer bound

`VGA_DEST_BUFFER_MAX_PIXELS` was `1u << 20` with a comment claiming the largest VGA dest
buffer is ~526 K pixels. MEASURED against the reference's own geometry arithmetic, that claim
was wrong: `virtual_width = offset<<4` (max 255 -> 4080) and `virtual_height = ceil(0x20000 /
bytes_per_line)`, whose product peaks at `4080 * ceil(131072/510) = 1,052,640` pixels when
`crtc_mode` bit 6 halves `bytes_per_line` (`offset<<2 >> 1`); **mode 12h's real 640x1639 =
1,048,960 pixels is close to that peak and exceeds the old 1,048,576 bound by 384**. The
reference's wasm `image_data` grows on demand and has no such fixed bound, so mode 12h was
silently *refused* by the C port (a silent divergence: the reference renders it, the port
reports an error and zeroes the buffer). Raised to `2u << 20` (2 MiB pixels, 8 MiB static)
with the corrected comment (the true maximum, not mode 12h's, is what the comment names). The
raise does not touch any existing gate's output (mode 13h's 320x410 and the text grid are far
below the bound); the hw_selftest R2 probe's canary tail re-derives from the macro, so it
stays sharp.

---

## 3. The differential (D1): four images, byte-identical on both legs

Each image self-programs a planar mode, draws, reads back in-guest, and halts; the gate runs
the same image at limit 2,000,000 through this port and the pinned reference and `cmp`s the
two documents (header *and* payload).

| image | mode | screen / virtual | document bytes | sha256 (C leg) |
| --- | --- | --- | ---: | --- |
| `vga-planar12.img` | 0x12, byte mode, shift 0x00 | 640x480 / 640x1639 | 4,196,302 | `8fb8a1f4…` |
| `vga-planar4.img` | packed shift 0x20 | 640x200 / 640x410 | 1,050,061 | `19478c93…` |
| `vga-planar0d.img` | 0x0D, word mode, shift 0x00 | 320x200 / 320x1639 | 2,098,382 | `2e87467c…` |
| `vga-planarsub.img` | substitution arm | 32x256 / 32x32768 | 4,194,762 | `034335f3…` |

`sh tests/gate-vga-3.sh` -> **0 FAILING assertion(s)**: D1 byte-identical on all four, D2 the
non-triviality + named-pixel checks, D3 each leg twice byte-identical, D4 both legs halted
after the image's own read-back.

### What the named pixels pin (each hand-computed from vga.js, then matched on the document)

- **planar12** (colorset ON, `color_select` 0x05, so source colour c -> DAC[0x50+c]):
  pixel (0,0) = green `0,255,0` (source 5 -> DAC[0x55]); (4,0) = red `255,0,0` (source 15 ->
  DAC[0x5F]); (16,0) = blue `0,0,255` (source 1 -> DAC[0x51], the write-mode-2 expand); (68,0)
  = white `255,255,255` (source 10 -> DAC[0x5A], the logical-XOR arm).  **The colorset arm is
  proven by these pixels landing in DAC[0x50-0x5F], not DAC[0-15].**
- **planar4** (packed shift): (0,0)=cyan, (1,0)=brown, (2,0)=light blue, (3,0)=light red,
  (4,0)=black — byte0=0xE4/byte2=0x1B unpack to colours 3,6,9,12,0.
- **planar0d** (word mode): (0,0)=magenta, (4,0)=white, (8,0)=blue — column B lands in pixels
  8-15, *not* 16-23, because mode 0x0D's `crtc_mode` 0xA3 has bit 6 clear and the
  addr-to-pixel shift is 2.
- **planarsub** (substitution): (0,0)=magenta (row 0), (32,0)=magenta (row 1 via bit 13),
  (64,0)=light green (row 2 via bit 14) — the multi-row bit-13/14 reassembly.

---

## 4. The selftest probes (case 9e) — one per sub-arm, each with a mutation that flips it

`vga_program_planar()` programs a small 32x16 planar mode (chain-2, shift 0x00, write mode 0,
`crtc_mode` 0x43 = byte mode, identity `dac_map`, the classic 16-colour DAC palette).  Each
probe drives the device through the ports and the 0xA0000 window, fills the buffer, and reads
the dest buffer back (`vga_dest_buffer`) or the window (`read8`).  The expected values are
hand-computed from vga.js's arithmetic.

**A probe must fail when the arithmetic is wrong.** For each probe, the mutation below was
applied to `vga.c`, `build/hw_selftest` rebuilt, and the failure count measured, then the
mutation reverted (915 checks total):

| probe (arm) | mutation | failures before revert |
| --- | --- | ---: |
| P-W0 chain-2 odd/even write + shift-0x00 replot | `plane_select = 0x5 << (addr&1)` -> `0xA` | 40 |
| P-W1 write mode 1 (latch copy) | `plane_dword = latch` -> `vga_apply_feed(value)` | 2 |
| P-W2 write mode 2 (expand) | expand lane `<< 8` -> `<< 16` | 2 |
| P-W3 write mode 3 dispatch | `write_mode = planar_mode & 3` -> route 3 to 0 | 2 |
| P-W3 write mode 3 bitmask arm | `bitmask &= feed(value)` -> `bitmask &= ~feed(value)` | 3 |
| P-ROTATE rotate count 1 | `count = planar_rotate_reg & 7` -> `count = 0` | 3 |
| P-LOGICAL-XOR (0x18) | `^ latch` -> `\| latch` | 1 |
| P-LOGICAL-AND (0x08) | `& latch` -> `data_dword` (replace) | 2 |
| P-LOGICAL-OR (0x10) | `\| latch` -> `data_dword` (replace) | 2 |
| P-SETRESET setreset + enable | `data_dword \|= enable & setreset` -> `data_dword \|= 0` | 2 |
| P-BITMASK partial bitmask | `\|= ~bitmask & latch` -> `\|= bitmask & latch` | 5 |
| P-WBM partial plane_write_bm | `plane_select &= plane_write_bm` -> `&= 0xF` | 2 |
| P-DACMAP non-identity dac_map + partial CPE | `dac_map[color16]` -> `color16` | 2 |
| P-READ0 read mode 0 plane_read 1-3 | `plane << 16` -> `plane << 15` | 4 |
| P-READ1 read mode 1 dispatch | `planar_mode & 0x08` -> `0` | 1 |
| P-READ1 read mode 1 complement | drop the `~` | 2 |
| P-ODDEVEN odd/even host read | `plane = a & 1` -> `(a & 1) ^ 1` | 2 |
| P-COLORSET attribute_mode 0x80 | `colorset \|= color_select << 4 & 0x30` -> `colorset \|= 0` | 1 |
| P-SHIFT20 packed shift 0x20 | swap byte0/byte2 in the 4 shift_loads | 4 |
| P-SUBSTITUTION row substitution | `addr = (row & 3) << 13` -> `<< 14` | 2 |

`hw_selftest` is now **915 checks, 0 failures, 0 skipped** (U2 round 2 was 842; +73 over the
fourteen original probes + the three arms the review named + the two dispatch-sharpness fixes).

### A test-design defect this unit caught in itself, worth recording

The first draft of `vga_dac_color()` used a 3-bit palette (`(i&1),(i&2),(i&4)`), so DAC entries
8-15 repeated 0-7, and the P-W2 probe asserted colour 9 while the actual pixel was colour 1 —
it **passed vacuously** because DAC[9] == DAC[1] == red. The discipline caught it (the probe's
comment and the measured pixel disagreed); the fix was a 16-DISTINCT classic palette and the
correct colour-1 expectation. This is the AGENTS.md lesson "a test that cannot fail is not a
test" — the *probe* was the broken instrument, not the device.

### Round 2: the reviewer's three un-gated arms, added with their own mutation arms

The review's grep-verified residual list (partial `plane_write_bm`, logical AND/OR, non-identity
`dac_map`) is now closed with four probes — P-WBM, P-LOGICAL-AND, P-LOGICAL-OR, P-DACMAP — each
with a mutation that fails before and passes after (the 2/2/2/2 rows above).  The same pass also
flagged two *dispatch-blind* probes (a mutation in the dispatch arm would not have failed them),
and both are now dispatch-sharp: P-W3 was re-derived with `setreset = 0x01` so write mode 0 would
replicate the value into plane 2 (colour 5) where write mode 3 leaves it 0 (colour 1) — the
`write_mode` -> 0 routing now FAILS 2 checks; P-READ1 was re-derived with `plane0 = 0x5A` so its
0x0A answer is not plane 0's value — the `planar_mode & 0x08` -> `0` routing now FAILS 1 check.

---

## 5. The guest gates — the boundary re-verified, and nothing moved

U3 did not claim any new port (the planar registers are the sequencer/graphics ports U2
already claimed), so the expected result is *no movement*, but the brief's THING 2 requires
the re-run and the pins published, not re-pinned:

| gate | result |
| --- | --- |
| `make` | rc 0 (binary sha256 `c6a98020…`) |
| `make selftest` | rc 0 — `hw_selftest: 896 checks, 0 failures, 0 skipped` |
| `make nasm` | rc 0 — `Passed 118/118` |
| `make transcript-diff` | rc 0 — byte-identical |
| `make determinism` | rc 0 |
| `make bench` | rc 0 — `instr=200095214` on all 7 runs, 231.32 MIPS best |
| `sh tests/gate-vga-0.sh` | rc 0 |
| `sh tests/gate-vga-1.sh` | rc 0 (D1 still byte-identical — U1's text path NOT weakened) |
| `sh tests/gate-vga-2.sh` | rc 0 (D1 still byte-identical — U2's mode-13h path NOT weakened) |
| **`sh tests/gate-vga-3.sh`** (new) | rc 0 — §3 |
| `make minibios-mem` + `sh tests/gate-reboot-1.sh` | rc 0 — `39 passed, 0 failed` |
| `sh tests/gate-m2-5-determinism.sh` | rc 0 — **both runs 4,000,096,757** (the pin DID NOT MOVE) |
| `sh tests/gate-m2-5-guest.sh` | rc 0 — `0 FAILING` |
| `sh tests/gate-pae-1.sh` | rc 0 — `11 passed, 0 failed` |
| `sh tests/gate-pae-l3.sh` | rc 0 — `0 FAILING` |

Nothing moved: no serial line, no count, no pin. The determinism count is still
**4,000,096,757** on both runs, and the bench pin **200,095,214** held on all 7 runs.

---

## 6. Deviations and honest notes

1. **The substitution arm's signed `>>` vs JS zero-fill `>>>` is PROVABLY INERT.** The review
   flagged it (`results/vga-u2.md` §7). READ, the only `>>>` in the substitution region is in
   `vga_replot` (`vga.c`): `pixel_addr >>> addr_shift`, `(row*vw+col) >>> addr_shift`, and
   `row >>>= 1/2`. Every one of those operands is non-negative in every reachable case:
   `pixel_addr` starts at `diff_plot_min & ~0xF` clamped to `>= 0` (the R2 fix), so
   `pixel_addr >= 0`; `row = pixel_addr / virtual_width >= 0` (guarded `virtual_width > 0`);
   `col = pixel_addr - virtual_width*row >= 0`. `vga_addr_to_pixel` has no `>>>` at all — its
   `/` and `%` are C truncation-toward-zero and sign-of-dividend, which are exactly JS's
   `| 0` and `%`.  So C's signed `>>` and JS's `>>>` agree on every value the arm can receive.
   The P-SUBSTITUTION probe (multi-row, non-degenerate) and `vga-planarsub.img` both prove the
   arm's *arithmetic* is byte-exact; the signedness is a non-divergence by construction.
2. **The colorset arm is a global redraw attribute**, so it remaps the *whole* document, not
   just the region drawn after it. `vga-planar12.asm` therefore turns it on LAST and programs
   DAC[0x50-0x5F] so the remapped document stays non-trivial (5 distinct colours) *and* proves
   the remap (the named pixels land in DAC[0x50-0x5F]).
3. **`vga-planar4.asm` is a packed-shift exercise, not a literal mode-0x04 table.** The literal
   0x04 table uses chain-2, which writes the SAME byte to planes 0 and 2 and cannot produce the
   full 2-bits-per-plane interleave that pins the 0x20 arithmetic; the image chains 4 (SR4 =
   0x08) so four distinct plane bytes can be written, then sets `planar_mode` 0x20. This is the
   CGA 4-colour interleave the 0x20 shift implements.
4. **Mode 0x0E is not given its own image.** 0x0D and 0x0E differ only in the CRTC
   offset/vdisp (320 vs 640 wide); the geometry arithmetic is already pinned by 0x12's
   640x480 byte-mode, 0x0D's 320x200 word-mode, 0x04's 640x410 (underline 0x40 -> addr shift
   1), and the selftest's 32-wide offset-2 geometry. A separate 0x0E image reaches no arm those
   do not.
5. **The un-gated list is now empty, and this is stated precisely, not vaguely.** The review's
   grep-verified residual (partial `plane_write_bm`, logical AND/OR, non-identity `dac_map`) is
   now probed (§4's Round-2 subsection, each with a mutation arm), so the inherited un-gated
   list IS empty for the arms U3 owns.  What remains for U4 is unchanged: dispi/SVGA/bank
   switching and the real dirty bitmap (`svga_enabled` is still always false; the
   `vga_memory_read/write` svga arms and the dispi registers are U4's).
6. **Remaining mitigated blind spots, recorded not hidden** (the reviewer's list, each now
   closed or honestly residual): P-READ1 dispatch-blind and P-W3 dispatch-blind are FIXED
   (§4 Round-2).  P-ODDEVEN's EVEN-address assertion is *inherently* a sanity check — the
   odd/even arm (`plane = a & 1; a &= ~1`) is byte-identical to a normal `plane_read 0` read on
   even addresses, so no probe can make that assertion exercise the arm; the ODD-address
   assertion (0x55, distinct from plane 0's 0xAA) is the arm's actual pin.  The gate's D2 DIMS
   check was a whitelist; it is now a per-image assertion (`check_image` takes the expected
   `WxH` and compares exactly), so a wrong size cannot slip through a whitelist.

---

## 7. What U4 inherits

- The planar path is fully gated; U4's SVGA/VBE layer builds on the same `vga_replot`/
  `vga_redraw` and the memory.c LFB seats, none of which U3 touched.
- The only code change U3 made (the dest-buffer bound) is a *strengthening* toward the
  reference (mode 12h now renders instead of being refused); U4's SVGA modes (up to 256 MiB
  LFB) will need their own bound policy, which is U4's to decide.
