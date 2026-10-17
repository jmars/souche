# VGA-U0 — the device's seats, the rendering seam, and the document

HANDOFF=v86c-vga, unit **U0** (the plan is `handoff-v86c-vga-plan` §7's U0; the lane's four
fixed decisions are `handoff-v86c-vga-ctx`). Rung: `coder`.

**Claims in this document are marked MEASURED / READ / INTERPRETATION / PROJECTION.** An
unmarked sentence in a table cell is MEASURED on this host with the binary named.

---

## 1. What U0 is, and — at the same length — what it is not

U0 lands the *shape* of the reference's VGA and proves the **document pipeline** end to end.
It lands **no register semantic at all**, and that is a decision, not a gap left by accident:
`v86/src/vga.js` is 2,612 lines and the decode it holds is where a silent-wrong port hurts
most, so it is split into U1 (text mode: CRTC/attribute/DAC, redraw, `put_char`, cursor) and
U2-U4 (mode 13h + the graphical buffer; the planar latches/write modes; dispi/SVGA/banks).
A half-ported register file in U0 would be exactly the false-declaration class this tree
keeps paying for.

What is here:

| file | lines | what it is |
| --- | ---: | --- |
| `src/hw/vga.h` | 233 | the seam (`vga_screen_t`: the 11 methods `vga.js` calls), the adapters' constructors, the document's format, the device's U0 state |
| `src/hw/vga.c` | 210 | the 8 MiB VRAM, the `0xA0000` window via `io_mmap_register`, PCI `00:12.0`, the adapter installation, the `--vga-doc` glue |
| `src/hw/vga_screen.c` | 1,000 | the three adapters (headless = `browser/dummy_screen.js`, ANSI = `browser/ansi_screen.js`, document = the lane's recorder) + the document writer + the reference's own cp437 table |
| `oracle/run-vga.mjs` | 280 | **the reference leg**: `run.mjs`'s machine + the recorder swap + the stop replay + the serialization |
| `tests/gate-vga-0.sh` | 214 | this unit's gate (D0-D3, and the `sweep` boundary gate) |

Wiring: `machine_init()` creates the device unconditionally in `cpu.js:1193`'s slot (after the
DMA, before the PS/2) — READ, `cpu.js:1176-1266`; `machine_config` gains `vga_screen` +
`vga_doc_path` (the reference's `settings.screen`); `src/main.c` parses `--vga-ansi` and
`--vga-doc FILE` and writes the document at the same stop the transcript is taken at;
`src/hw/hw_selftest.c` gains case 9.

**The four fixed decisions, as landed** (nothing here re-litigates them):
headless is the default adapter and a port of `dummy_screen.js`; the *document* is the verified
path and **both legs produce it** (§4); ANSI is the see-it-working adapter, text only; there is
no window, no SDL, no X11, no input path.

---

## 2. The seam

`vga_screen_t` is a struct of 11 function pointers plus an opaque state pointer, and it is
exactly the methods `vga.js` calls on `this.screen` — READ: all 26 call sites in `vga.js` are
`this.screen.<method>` (`set_mode`, `set_size_text`, `set_size_graphical`, `set_font_page`,
`set_font_bitmap`, `put_char`, `update_buffer`, `update_cursor`, `update_cursor_scanline`,
`clear_screen`, `clear_text_state`). `destroy` / `pause` / `continue` / `set_scale` /
`get_text_screen` are UI-side and are not in the seam.

The per-adapter **flag constants** are load-bearing and are reproduced, not fixed. READ:
`browser/screen.js:30-34` defines `FLAG_BLINKING = 0x01` and `FLAG_FONT_PAGE_B = 0x02` on the
canvas adapter **only**; `dummy_screen.js` and `ansi_screen.js` define neither, so
`vga.js:848-849`'s expression collapses to `0` on them. In the port that fact is a pair of
accessors (`vga_screen_flag_blinking` / `vga_screen_flag_font_page_b`): `0` for headless and
ANSI, `0x01` / `0x02` for the document adapter (which stands in for the canvas adapter, and
whose reference-leg recorder defines them too). U1 is the reader; case 9 asserts the values.

---

## 3. The device's seats, and what the machine now answers

| seat | before U0 | after U0 | evidence |
| --- | --- | --- | --- |
| the 8 MiB LFB at `0xE0000000` | dead: `vga_memory_size == 0`, `in_svga_lfb` false everywhere, every read answered `0` from the hook table's carve-out and every **write was discarded** | alive: `memory.c`'s already-ported arms read the VRAM and **store** | `hw_selftest` case 9 (write32 → read32 round-trip; the last byte + the `read128` into the array's 16-byte slack) |
| the `0xA0000` window | fell through to `memory.c`'s default table: read `0`, discard | the device owns the blocks (`io_mmap_register`, `vga.js:384`), and **still answers read `0` / discard the write** | case 9 asserts the stand-in, deliberately — U1 flips it |
| PCI `00:12.0` | absent | present: `0x1234:0x1111`, class `0x000300`, BAR0 `0xE0000008` (8 MiB, prefetchable memory BAR), ROM window `0xFEBE0000`, subsystem `0x1af4:0x1100` | the 64 config bytes are **MEASURED** off the pinned `libv86`'s own `vga.pci_space` (truncation included), not transcribed from the source — and they differ from the source's *field* `pci_rom_address = 0xFEB00000`: read the artifact, not the name |
| the VGA **I/O ports** | unclaimed (`0xFF`/`0xFFFF`/`-1`) | **still unclaimed** | the brief's rule: a port whose semantics belong to a later unit answers what the current model answers, with a comment saying whose it is (`vga.h`) |

Why no `--vga` flag (the plan's Q4, landed): the reference **always** creates the VGA
(`cpu.js:1193`) and `starter.js:307-319` only picks the adapter; the reference leg of every
two-leg gate in this tree already runs with the device. So the device is unconditional and
`--vga-ansi` / `--vga-doc` select the *output*.

**What moved guest-visibly, stated plainly:** an LFB write now persists (the reference's
answer); PCI enumeration gains `00:12.0`; the `0xA0000` window is unchanged.

---

## 4. The document, and both legs producing it

Format (`vga.h`): `v86c-vga-doc/1\n`, then one line of canonical JSON (keys sorted, no
whitespace), then the raw payload. TEXT payload = `rows*cols` 10-byte records
(`chr u8, flags u8, bg u32 LE, fg u32 LE`), row-major; a cell the device never drew is zeros.
No charmap, no timestamp, no pointer, no `payload_sha256` (see §7, deviation 1).

**C leg**: `build/v86c <image> <limit> --vga-doc FILE`. The document adapter records
`put_char` into a grid; the writer takes `cols`/`rows`/the mode fields from the **device** and
the cursor from the recorder, and writes `<path>.tmp` → `rename` (whole or nothing). In U0 no
register path calls `set_size_text`, so the file carries the reference's own post-boot state:
2000 zero records, cursor `0,0`, scanline `0,0,false`.

**Reference leg**: `node oracle/run-vga.mjs <image> <limit> <out>`. Our harness passes **no**
screen (`run.mjs` does not), so the reference's `DummyScreenAdapter` is installed at
construction; `vga.screen = recorder` **after** `emulator-loaded` redirects every subsequent
call (MEASURED on the pinned build: no error, zero post-swap calls on a VGA-blind image). The
grid is never hand-seeded: at the stop the harness calls the **device's own** public methods —
`text_mode_redraw()`, `update_cursor()`, `update_cursor_scanline()` — and the device does all
the palette/attribute math. MEASURED: `text_mode_redraw` emits 2000 `put_char(...,0,0,0,0)`
calls on `tests/test.img`, i.e. the reference's post-boot screen is all zeros too, which is why
the two legs agree **byte for byte** today.

MEASURED, `tests/test.img` at limit 1,000,000:

```
C leg: 20445 bytes, 8c3374d4d2b748a670dac208fcfaa3c757fd52009d3edd88877749c63058922d
JS leg: 20445 bytes, 8c3374d4d2b748a670dac208fcfaa3c757fd52009d3edd88877749c63058922d
cmp: identical
```

header: `{"bpp":32,"cols":80,"cursor_col":0,"cursor_row":0,"cursor_scanline":{"end":0,
"start":0,"visible":false},"format":1,"graphical_mode":false,"image_bytes":283,
"image_sha256":"1a597ac1…","instruction_count":561,"instruction_limit":1000000,"mode":"text",
"payload_bytes":20000,"rows":25,"screen_height":0,"screen_width":0,"svga_enabled":false,
"virtual_height":0,"virtual_width":0}`

**What this proves, and what it does not.** It proves the format, the stop point, the geometry,
the cursor/scanline fields, the instruction count, the image digest and the payload layout
agree between the two machines, and that each leg is deterministic. It proves **no** register
decode, palette, glyph or pixel — those are U1's and U2's asymmetries, each with its own image
(`tests/vga-text.asm`, `tests/vga-mode13.asm`). §6's M2 records what that costs today.

---

## 5. FINDING (for the orchestrator — not re-pinned here)

### F1 — the determinism gate's instruction count moved by +72,695, and the cause is PCI `00:12.0`

`sh tests/gate-m2-5-determinism.sh` is green (0 failing) on both trees, but its count is the
tree's pinned number and it **moved**:

| tree | binary sha256 | both runs report | serial capture |
| --- | --- | --- | --- |
| pre-U0 (`git stash` of this unit, rebuilt) | `4f1c849df5473ea7c83a63e9d7536f6829c6cfbe37bfcdf30fd5feaf8b12a1d3` | **4,000,024,062** — the pin recorded in `results/acpi-6.md`, `results/pae-1.md`, `results/perf-7.md` | the committed one |
| U0 (this tree) | `eba7d1c23b3dc94a76e4a29b979fb9264e1238be26fa1b95cb7bad649a254f1a` | **4,000,096,757** (identical in both runs) | +7 lines, 1 relocated |

Both measurements are MEASURED on this host, back to back, same cmdline/limit/inputs (80 MiB,
`v86guest` kernel, the gate's own `check` arm verifies the pins first).

The delta is the kernel **enumerating the device**, and it is the reference's own answer:

```diff
 pci 0000:00:0c.0: BAR 3 [io  0xb600-0xb60f]
+pci 0000:00:12.0: [1234:1111] type 00 class 0x030000 conventional PCI endpoint
+pci 0000:00:12.0: BAR 0 [mem 0xe0000000-0xe07fffff pref]
+pci 0000:00:12.0: ROM [mem 0xfebe0000-0xfebeffff pref]
+pci 0000:00:12.0: Video device with shadowed ROM at [mem 0x000c0000-0x000dffff]
 ...
+pci 0000:00:12.0: vgaarb: setting as boot VGA device
+pci 0000:00:12.0: vgaarb: bridge control possible
```

The rest of the serial capture is untouched: MEASURED, the whole diff of
`results/m2-5-det-run1-serial.txt` against the committed file is those **7 added lines plus one
relocated but text-identical line** (`printk: legacy console [hvc0] enabled`, which the insertion
shifts past), and nothing else — 8 `+` lines, 1 `-` line.

MEASURED, and it is the decisive half: the **JS reference leg already had all seven lines**, at
the same line numbers (`results/m2-5-guest-armA-js-serial.txt:74-77,87-89`) — the pre-U0 C leg
was the one that lacked them. The tree's own committed record of that gap is
`results/m2-5-guest-leg-diff.txt` at HEAD, whose entire first hunk is those lines:

```
74,77d73
< pci 0000:00:12.0: [1234:1111] type 00 class 0x030000 conventional PCI endpoint
< pci 0000:00:12.0: BAR 0 [mem 0xe0000000-0xe07fffff pref]
< pci 0000:00:12.0: ROM [mem 0xfebe0000-0xfebeffff pref]
< pci 0000:00:12.0: Video device with shadowed ROM at [mem 0x000c0000-0x000dffff]
87,89d82
< pci 0000:00:12.0: vgaarb: setting as boot VGA device
< pci 0000:00:12.0: vgaarb: bridge control possible
< pci 0000:00:12.0: vgaarb: VGA device added: decodes=io+mem,owns=io+mem,locks=none
```

After U0 the regenerated leg diff contains **no `00:12.0` line at all**: the two legs now agree
on the VGA device exactly, and the only differences left are the pre-existing console-ordering
ones. So this unit *converges* the two legs. The gate's C-vs-JS comparison passes unchanged
(`### GATE 3b … the same MULTISET on both legs (19 lines)`, 0 failing).

The same 7 lines appear in the other two serial captures this unit's gates rewrite
(`results/m2-5-guest-armA-*`, `results/pae-l3-*`); their remaining diff is the kernel's own
message ORDER and `[timestamp]` values shifting, which each gate's own normalization already
tolerates (the C-vs-JS multiset check, and PAE-L3's stripped-timestamp comparison) — MEASURED:
strip the `[timestamp]` prefix from `results/pae-l3-c-serial.txt`'s diff and every remaining
line occurs in both directions (225 `+` / 218 `-`, 7 of them the VGA lines).

**Per the brief the pin is NOT re-pinned here.** It is a decision for the orchestrator: the
count moved because the machine gained the reference's device, determinism holds, and every
gate is green; the number in the tree's records (`results/*.md`) now describes the pre-U0
machine.

Nothing else moved: `make bench` still asserts `200095214` instructions, the terminal state
and the serial sha256 on every run.

### F2 — the plan's "the pinned images never touch the VGA" is false for the kernel boot

MEASURED with the unit's two diagnostics (`V86C_LOG_PORTS=1 V86C_TRACE_VGA=1`):

| workload | VGA port accesses | window accesses |
| --- | ---: | ---: |
| `tests/test.img` (M0) | 0 | 0 |
| the whole nasm corpus, 118 images (the 5 skip-listed ones excluded) | 0 | 0 |
| the kernel boot (`--bzimage tests/images/vmlinuz-i386 200000000`) | **24** — all `0x3D4`/`0x3D5`, 12 each (the CRTC index/data pair) | **8,083**, including the **firmware's** `INT 10h` teletype writing characters at `0xB84C0` ("on!") |

So the boundary proof for the kernel boot is not "zero accesses" — it is **the same answers as
before**: the ports are still unclaimed and the window still reads 0/discards, which is why the
boot's count (200,095,214) and its serial sha256 did not move. `tests/gate-vga-0.sh sweep`
asserts exactly that, and prints the counts.

**This is the shape of U1's regression risk (the plan's R7):** once `vga_memory_write_text_mode`
lands, those teletype writes persist in VRAM and anything reading `0xB8000` back sees new
nonzero bytes. The kernel-boot gates must be re-verified at U1, and this document is the
before-picture.

---

## 6. Asymmetry probes (each FAILS on the pre-fix shape)

The pre-fix shape is the tree without the device; every probe below was run by mutating the
unit's own code, rebuilding, running the named instrument, and reverting (the binary's sha256
returned to its pre-mutation value each time — MEASURED, `41914bc5…` → mutated → `41914bc5…`).

| # | mutation | instrument | result |
| --- | --- | --- | --- |
| M1 | `vga_memory_size = 0` in `vga_init` (the pre-U0 state; `vga_mem8` still allocated) | `hw_selftest` case 9 | **FAILS**: aborts at `memory.c:450 assert(in_svga_lfb(dst_addr))` — the LFB arms are unreachable without the device |
| M2 | swap `bg`/`fg` in the C payload writer | `hw_selftest` case 9 | **FAILS** (rc 1) — the selftest reads the record bytes back |
| M2′ | the same mutation | `tests/gate-vga-0.sh doc` | **PASSES** — the D0 differential cannot see it: every cell on a VGA-blind image is `bg=fg=0`, so a transposition is a no-op. **The colour lane order of the payload is UNTESTED by the U0 differential**; it needs U1's text image with `bg != fg`. Recorded because a test that cannot fail is not a test. |
| M3 | `instruction_count + 1` in the header | `gate-vga-0.sh doc` | **FAILS** at byte 276 (`differ`) — the count field is compared, as the plan intended |
| M4 | 9-byte payload records (fg's high byte dropped) | `gate-vga-0.sh doc` **and** `hw_selftest` | **both FAIL** (18445-byte document; `cmp` differs) |

M1 is the unit's headline asymmetry: it is the *same* probe the pre-U0 tree fails by
construction, because the pre-U0 tree has no `vga_mem8` at all.

---

## 7. Deviations from the plan (each named, each with its reason)

1. **`payload_sha256` is not in the header.** The plan's §4 lists it. It would put a second
   implementation of a hashing primitive in the device layer (`main.c` owns the port's only
   sha256, and the document writer lives in `src/hw/`), for a value the gate's `cmp` of the
   whole file already covers. `payload_bytes` is kept and asserted. If the orchestrator wants
   the field, the cheapest honest way is to move `main.c`'s sha256 into a shared unit — a
   change to the M0 transcript's own file, which is why this unit did not do it.
2. **`--vga-ansi`'s reader is a stderr flush at the stop.** Upstream's browser starter *polls*
   `get_text_screen`; a batch binary has no poller. The flush goes to **stderr** so the M0
   transcript (stdout) cannot be contaminated. Not byte-gated (decision 3).
3. **The `0xA0000` window's writes are discarded in U0.** Storage without the decode
   (`vga_memory_write_text_mode`, the plane write modes, the chain/odd-even arithmetic) would
   be right for text and silently wrong for graphical — so the handlers answer what this
   machine answered before the device existed, and `vga.h` says so.
4. **No VGA I/O port is claimed.** The plan's U0 line "the full port map answering the
   reference values" is **not** in this unit: answering a register means implementing it, which
   is U1-U4, and the brief's rule for a later unit's port is "answer what the current model
   answers". Every VGA port is therefore unclaimed (`0xFF`/`0xFFFF`/`-1`) with a comment naming
   its owner.
5. **A graphical document is refused** by both legs, by name (U2's payload is the dest
   buffer's pixels).
6. **The text grid is bounded** at 256×128 cells (the reference resizes a typed array; `cols`/
   `rows` become guest-driven at U1, and a C adapter needs a bound, not an allocation a guest
   can drive). The refusal is reported, never silent.
7. **The VRAM is a static 8 MiB + 16 bytes.** The slack is `mem8`'s argument (`memory.c:44-50`):
   `read64s`/`read128` at the window's last byte issue a 16-byte load.
8. **`--vga-doc` is transcript-mode only.** A boot-mode stop is an instruction limit in a guest
   that never halts, and a framebuffer document there is U4's LFB image; `boot_main` refuses the
   flag by name rather than writing a document whose stop point nothing has pinned.
9. **`--vga-ansi`/`--vga-doc` are refused in `--fixture` mode** (the nasm differential's
   document has no machine geometry) — a silent no-op flag is a lie about what ran.

---

## 8. Gates (exit codes are the verdict)

All of them were run on the **final binary** of this unit,
`build/v86c` sha256 `eba7d1c23b3dc94a76e4a29b979fb9264e1238be26fa1b95cb7bad649a254f1a`, rebuilt
after the last source edit (`make` reported rc 0 and produced that sha).

| gate | result |
| --- | --- |
| `make` | rc 0 |
| `make selftest` | rc 0 — `hw_selftest: 777 checks, 0 failures, 0 skipped` (case 9 adds 83 to the previous 694) |
| `make nasm` | rc 0 — `[+] Passed 118/118 tests.` |
| `make transcript-diff` | rc 0 — the C transcript is byte-identical to the reference's (the flag-off machine's first gate) |
| `make determinism` | rc 0 — two oracle runs byte-identical |
| `make bench` | rc 0 — `instr=200095214 state=ok serial=ok jit-inline=ok` on all 7 runs |
| `make minibios-mem` + `sh tests/gate-reboot-1.sh` | rc 0 — `39 passed, 0 failed` |
| `sh tests/gate-m2-5-guest.sh` | rc 0 — `0 FAILING assertion(s)` (the C-vs-JS serial multiset unchanged) |
| `sh tests/gate-m2-5-determinism.sh` | rc 0 — `0 FAILING`; both runs 4,000,096,757 (§5 F1) |
| `sh tests/gate-pae-1.sh` | rc 0 — `11 passed, 0 failed` |
| `sh tests/gate-pae-l3.sh` | rc 0 — `0 FAILING assertion(s)` (both legs, NX active) |
| **`sh tests/gate-vga-0.sh`** (new) | rc 0 — `0 FAILING assertion(s)`: D0 two-leg document identical, the format's magic and `payload_bytes`, D1 both legs deterministic, D2 the transcript unchanged by `--vga-doc`, D3 `--vga-ansi` prints 25 rows, S1/S2 zero VGA access in the M0 image and the corpus, S3 the kernel boot's VGA access counts are the §F2 ones and its instruction count is still the pin |

### Performance (the plan's R5: the window entry on the hot path)

Interleaved-by-rebuild A/B on the pinned workload (`tests/perf_bench.sh --runs 7`), both
binaries measured back to back on this host:

| binary | best | median | MIPS at best |
| --- | ---: | ---: | ---: |
| pre-U0 (`4f1c849d…`) | 0.827 s | 0.830 s | 241.95 |
| U0 (`41914bc5…`) | 0.831 s | 0.866 s | 240.79 |
| U0 final (`eba7d1c2…`, the shipped binary of this unit) | 0.837 s | 0.867 s | 239.06 |
| U0, a later block on the same binary | 0.849 s | 0.991 s | 235.68 |

INTERPRETATION: the pinned metric (`MIPS at best`) moves −0.5% to −2.6% across the blocks, i.e.
inside this host's band — the U0 block's *own* range (0.831–0.870 s) brackets the pre-U0 median,
and a later block on the same binary gave a median of 0.991 s with one 1.234 s run (host noise).
A proper interleaved A/B was **not** run for U0: the workload's instruction count, terminal state
and serial sha256 all held, so there is no claim to make beyond "not measurable as a regression
at the best-run level". Mechanically: the registration adds no branch to a non-window access (the
`io_mmap` indirection is the one the default table already took), and the bench workload touches
the window 8,083 times in 200 M instructions.

---

## 9. What U1 inherits (the flip sites, so the next unit is not guessing)

- `hw_selftest` case 9's two window lines (`read8(0xB8000) == 0`, `read32s(0xA0000) == 0` after
  writes) are asserted **as the U0 stand-in**; U1 flips them to the written bytes. The comment
  at each site names the function that flips it.
- The device's `graphical_mode` / `svga_*` / `screen_width` / `virtual_*` / `max_cols` /
  `max_rows` fields carry the reference's **constructor** values (`vga.js:78-304`) and are read
  only by the document header; the units that make them move are named on the fields.
- The document adapter deliberately does **not** seed an 80×25 grid in its constructor (the
  three reference adapters do, via `set_size_text(80, 25)`): U1's first `set_size_text` call is
  then observable, and today's zero grid is the device-never-sized case.
- `vga_screen_flag_blinking`/`_font_page_b` exist for the flags expression `vga.js:848-849`
  builds; the text path is their reader.
- The kernel boot reads `0x3D4`/`0x3D5` and writes the teletype into `0xB84C0`+ (§F2): U1 must
  re-run **all** the guest gates, not just the new one.
