# SECFIX-1 — the security audit's findings: the PCI blocker, the bounds/assert class, the JIT hash

**Unit:** `HANDOFF=v86c-audit`, SECFIX-1 (coder). **Authority:** `results/audit-1-security.md`
(B1, S1, S2, S3, N1–N3) and `results/audit-2-jit.md`'s SHOULD-FIX 1 + NICE b + NICE d (A2-SF).
**Tree:** a49a887 + this unit's changes (uncommitted; the orchestrator commits).
**Prefix artifact:** the audited binary, pinned at `/tmp/secfix1-prefix-v86c`
(sha256 `868ea1f4206c6307ed6aae1442a80f6580291bf63e5f49af77fc44ea08651ed5`), plus the audited
sources materialized at `/tmp/secfix1-prefix-src` (`git archive a49a887`) so every C-level probe
runs against BOTH trees — the asymmetry is measured, never claimed.

**Verdict: B1 fixed with the demonstrated flip; S1 swept — 9 guest-reachable abort sites resolved
at their own sites (one demonstrated on a two-instruction image, two more on the audit's exact
virtio attack), 3 candidate sites verified NOT guest-reachable and left as internal contracts;
S2's three sinks bounded; S3 clamped; A2-SF fixed the sibling's way with a named full-table
refusal; both cheap audit-2 nice-to-haves taken. Full G5 battery green with selftest counts
IDENTICAL to the audited tree on all seven suites.**

Files changed: `src/hw/pci.c`, `src/hw/io.c`, `src/hw/virtio.c`, `src/hw/virtio_console.c`,
`src/hw/dma.c`, `src/hw/ide.c`, `src/cpu/memory.c` (+ `.h`), `src/cpu/cpu.c`, `src/cpu/modrm.c`,
`src/cpu/cpu_selftest.c`, `src/cpu/jit_mem.c` (+ `.h`), `src/cpu/jit_x64.h`,
`src/cpu/jit_codegen.c`. New: `results/secfix1-probe.{c,sh,txt}` (before/after captures),
`results/secfix1-pci-misalign.asm`, `results/secfix1-g1g2-captures.txt`,
`results/secfix1-g5-*.txt`. Raw before/after probe evidence:
`results/secfix1-probe-before.txt` / `-after.txt`.

---

## B1 — BLOCKER: `pci_set_io_bars` bounded (the demonstrated flip)

**Fix** (`src/hw/pci.c`, `pci_set_io_bars`): both the `from` clear and the `to` install are now
guarded `>= 0x1000 && < IO_PORT_COUNT`, and the `empty_entry` sanity read/assert happens only
in range. **The chosen bound is a DISCARD, and why:** upstream's `this.io_ports[to+i] = entry`
grows its JS array past 0x10000, and its dispatch indexes `ports[port & 0xFFFF]` (io.js:314-390;
x86 ports are 16-bit) — the grown entries are unreachable, so upstream *observably discards* the
write. The port's dispatch already masks the same way (`io.c`'s `io_ports[(uint32_t)port &
0xFFFF]`), so `< IO_PORT_COUNT` reproduces the reference's exact answer. A clamp-to-the-top was
rejected: it would answer differently from the reference (the guest would see a BAR half-moved).

**G1 asymmetry** (the audit's probe/control pair, `/tmp/audit1/p2.img` + `ctl.img`, single
changed variable = the BAR value):

| | probe (BAR→0xFFFE) | control (BAR→0xB400) |
|---|---|---|
| BEFORE (pinned binary) | reads mmap block 20 as **0x10** (EBP low byte) — the guest changed the host's dispatch answers | 0xFF |
| AFTER (final tree) | **0xFF — identical to the control**; the control's whole fixture is byte-identical pre/post | 0xFF |

**Class check** (`io_ports[...]` writes and guest-indexed fixed arrays, beyond the site):

- `io.c`'s consecutive shims index `io_ports[port+1..port+3]` at *dispatch* time; the single-port
  registrars' bound is a DBG_ASSERT. **N2 taken**: `io_register_{read,write}_consecutive` now
  refuse a registration whose `+3` tail would leave the table (hard bound + stderr note). The
  only callers are PCI's own fixed 0xCF8/0xCFC — verified by grep — so no hot path changes and
  no BAR-relocated device carries a consecutive shim.
- `pci_write32`'s `pci_bars[bar_nr]` was already bounded (`bar_nr < pci_bars_count`) by an
  earlier unit; re-verified.
- ~~`pci_get_io_space`'s registration-time read loop keeps its assert (host-driven).~~
  **[CORRECTED in REVIEWFIX-1 (N5, AUDIT-1R): there is no `pci_get_io_space` anywhere in
  `src/`** — upstream's `PCI.prototype.get_io_space` was never ported (devices hardcode
  their ports), so there is nothing to bound and this bullet cited a nonexistent site.
  The sweep's substance stands.]

**Divergence from the reference:** none observable — the discard *is* the reference's answer.

## S1 — the assert-abort class: swept, 9 sites resolved, 3 verified not-guest-reachable

The resolution rule (established by REBOOT-FIX-1 and R3F, applied per site): a guest-reachable
assert is resolved **at its site, matching what the pinned RELEASE reference does** — never by
`-DNDEBUG`.

1. **The fetch asserts (demonstrated).** `cpu.c`'s `get_phys_eip`, `interpreted_run`'s entry, and
   `modrm.c`'s `read_imm8` each asserted `!in_mapped_range(phys_addr)` on a value a guest
   controls with one `jmp`. The assert conflated two cases, now separated at each site:
   an in-RAM fetch (the VGA hole included) is one the release executes **from the raw bytes**
   (`memory::mem8.offset(..)`) — the assert is gone and the raw read stands; a fetch **past the
   guest's RAM** the release does not survive either (the nasm suite's pinned `jump` fixture is
   its SIGSEGV, `tests/nasm_skip.txt:33`), so the port names that with the tree's named stop
   instead of reading host memory. `jump` is in BOTH skip lists, so `make nasm` is untouched.
   **G2 asymmetry** (`/tmp/audit1/eip-hole.img`, `jmp 0xB0000`): BEFORE rc 134
   (`cpu.c:100: assert(!in_mapped_range(phys_addr))`) in all three configs; AFTER rc 3 with the
   fixture printed in all three configs, and the final state **IDENTICAL to the pinned JS
   reference leg** (`oracle/nasm_state.mjs`: eip 0xE1000, eax 0x2BADB002 in all four runs).
2. **The virtio notify paths** (`virtio_console.c`): a notify on a queue the driver never
   configured hit `virtqueue_has_request`'s `avail_addr != 0` assert — one port write. Both
   notify loops now guard with `virtqueue_is_configured` and drop with a named note. **Measured**
   (probe `virtio3`): BEFORE rc 134 at `virtio.c:1163`, AFTER rc 0 with the drop note.
3. **The audit's exact attack** (probe `virtio4`): configure only queue 3, post one 8-byte
   DEVICE_READY packet, notify — the handler answers through `console_send_event`, which popped
   the never-configured queue 2. The three control sends (`send_event`/`send_name`/
   `send_window_size`) now carry the guard `console_control_queue_ready` — the same shape
   `console_resize` and `console_input_bytes` already had (which is upstream's own guard where
   upstream HAS one). **Measured**: BEFORE rc 134 at `virtio.c:1254` (`pop_request`), AFTER rc 0
   with four named drops (one per port).
4. **`virtqueue_push_reply`'s `num_staged_replies < queue->size`** (probe `virtio5`): the guest
   writes `queue_size = 0` through the common-config field *after* configuring (the field layer
   clamps only the upper bound), posts a buffer, notifies — the first staged reply failed
   `0 < 0`. Assert dropped (release proceeds; the used-ring write is router-bounded).
   **Measured**: BEFORE rc 134 at `virtio.c:1272`, AFTER rc 0 with the reply staged.
5. **`console_notify_output`'s buffer is now zero-filled** (`calloc`, upstream's
   `new Uint8Array` semantics) — invisible while every read filled the buffer, load-bearing once
   the S2 clamp can under-fill (see S2; this closes the host-heap-leak-into-the-sink arm).
6. **Verified NOT guest-reachable, left as internal contracts** (probe steps 1–2 exercise the
   field layer on both trees and pass on both): `virtqueue_enable`'s assert
   (`common_queue_enable_write` only calls it on a configured queue), `virtqueue_set_size`'s two
   asserts (`common_queue_size_write` rounds a non-power-of-2 up and clamps to size_supported
   first). Each site carries the sweep's verdict as a comment. `virtqueue_has_request` /
   `count_requests` / `pop_request` / `flush_replies` asserts stay: every caller now guards.
7. **pci.c's four config-write asserts** (the class sweep found these; the audit had not named
   them): `pci_write8`'s "not landing on a BAR/ROM", `pci_write16`'s alignment + ROM-range, and
   `pci_write32`'s alignment asserts are all transcribed from upstream's own `dbg_assert`s
   (pci.js:337/348/368/379), compiled out of the pinned release, whose behaviour is the aliased
   in-bounds shadow write each following line already performs (`addr` is masked to 0xFF, so
   every index is ≤ 0x7F of 128 u16 / 0x3F of 64 dwords). Dropped; **measured** with
   `results/secfix1-pci-misalign.asm` (write8 into BAR4, misaligned write32 at 0x21, misaligned
   write16 at 0x31): BEFORE rc 134 on arm A, AFTER rc 0 with the machine running all three arms
   into its hlt loop.

**Divergences declared:** for the notify/sends/enable paths the release build pops garbage
chains and runs on (there is no defined observable to match — a garbage reply into a used ring
the driver never posted to); the port drops with a named note instead. This is the audit's own
N3 recommendation and the port's established guard shape. **[Extended in REVIEWFIX-1 (SF3,
AUDIT-1R): S3's ATAPI DMA clamp is a declared divergence too — upstream's PIO arm clamps but
its DMA arm DIES (`SyncBuffer.get` throws); the port clamps both arms, because the C analogue
of "not clamping" is the out-of-bounds `memcpy` itself, not a throw. Declared at the site
(`ide.c`, the AUDIT-1 S3 comment) and in S3's section below.]**

## S2 — the blob sinks bounded

- **`write_blob`** (`memory.c`): the kernel loader's shape (`kernel.c:56`) — what falls outside
  guest memory is dropped (fully or as a tail clamp) and said on stderr; the bound is
  `cpu->memory_size` (the reference's own view length), *not* `in_mapped_range`, so a descriptor
  aimed at the VGA hole writes the same raw bytes the release's wasm view would. Upstream's
  release **dies** on this input (`mem8.set` throws RangeError — MEASURED in node:
  `new Uint8Array(16).set(new Uint8Array(4), 20)` is a RangeError); the port clamps and
  continues. **Divergence declared**: robustness against the audit's threat model (a guest image
  is data an operator may be handed); the reference's death is its bug, not a defined observable.
  **Measured** (probe `write`): BEFORE rc 134 at `memory.c:72`, AFTER rc 0 with the straddle
  clamped to the exact bytes.
- **`virtio_guest_read_blob`** (`virtio.c`) now returns the available count — upstream's
  `subarray` CLAMPS (MEASURED in node: `subarray(20,24)` on a 16-byte array is empty; a
  straddling one is truncated), so the read arm answers "fewer/zero bytes", never host memory.
  The caller still advances by the full length (upstream's `dest_offset += read_length` runs on
  the empty view), and `console_notify_output`'s zero-fill (S1 item 5) is what the gap then
  holds. **Chosen and stated: the read arm matches the release's clamp semantics exactly.**
  **Measured** (probe `read`): BEFORE rc 134 at `virtio.c:69`, AFTER rc 0 with 16 zero bytes to
  the sink and no host bytes.
- **`dma_do_read`** (`dma.c`) gains the twin's guard (`dma_do_write`'s shape and placement, same
  named stop) — the audit's asymmetry-is-the-bug point. Dead today (the floppy is unported; only
  the selftest calls it, in range), and the selftest stays green.

## S3 — the ATAPI DMA read clamped

`interface_atapi_read_dma` now applies the PIO arm's clamp (`byte_count` to
`buffer_length - start`) right after the bounds reject, before the status/DRQ/report sequence.
**Divergence DECLARED (REVIEWFIX-1, SF3):** upstream's PIO arm clamps but its DMA arm **dies**
on this input (`SyncBuffer.get` → `new Uint8Array(buffer, start, len)` throws,
buffer.js:35-39); this port clamps **both** arms. Chosen rather than matched because the C
analogue of the un-clamped count is not a throw but the out-of-bounds `memcpy` itself —
"matching the release's death" is not expressible safely here — under the same rationale as
write_blob's declared divergence (the reference's death is its bug, not a defined observable).
Declared at the site too (`ide.c`, the AUDIT-1 S3 comment).
**Reachability verdict: LATENT, not a blocker** — `machine_config.cdrom` is never given media
(no CLI flag attaches it; `main.c` has no `--cdrom`), so `ide->buffer` is always NULL on the
always-present ATAPI device and the vulnerable line is unreachable today; the first `--cdrom`
flag makes it live. **Measured** (probe `atapi`): the real device via `machine_init` with media
whose end abuts a PROT_NONE guard page, driven through the real io-port dispatch (select,
features-DMA, PACKET, a READ_12 CDB for lba 3 / count 0x2000): BEFORE rc 139 (SIGSEGV — the
`interface_data_set` memcpy ran off the media into the guard page), AFTER rc 0 with the transfer
clamped.

## N1 — `ide.c` data_write16/32 carry the read twins' `data_capacity` bound

The write twins are live (the guest's data-port write path, `ide.c:608/611`) and indexed by the
guest-advanced `data_pointer`; they now carry the same check the read twins have instead of
trusting the state machine's `data_end ≤ data_length ≤ data_capacity` invariant.

## A2-SF — the JIT block-hash insert fixed the sibling's way, plus the named refusal

`jit_cache_bind`'s insert (`jit_mem.c`) now tracks the first DEAD slot and reuses it exactly as
`uop_hash_insert` does (`uop_cache.c`), and **refuses the bind** — counting into a new
`jit_stats.hash_full`, printed by `jit_stats_print` — when the scan finds neither EMPTY nor DEAD.
The refusal recycles the span per the JIT-FIX-1 ownership rule. The state is unreachable today
(live blocks cap at JIT_MAX_BLOCKS = 2048 < JIT_HASH_SLOTS = 4096); it is named rather than
silent. One defect in my own first cut was caught by the probe and fixed before shipping: the
refusal test ran after the DEAD-swap and rejected the reusable DEAD slot (a DEAD is non-zero);
the test now rejects only a LIVE entry.

**G4 asymmetry** (probe `hash`): one live sentinel block whose hash-home a colliding key lands
on (`jit_test_hash` finds the collision — 0xa53c); 6000 bind+invalidate churns saturate the
table with tombstones, then the colliding bind: BEFORE rc 1 — "SENTINEL LOST by the colliding
bind" (the live-slot overwrite is real); AFTER rc 0 — sentinel live, DEAD slots reused, the
colliding block findable.

**Audit-2 nice-to-haves taken (both cheap ones):**
- **NICE b**: `JX64_MAX_LABELS` 192 → 256 (`jit_x64.h`), which puts label exhaustion past the
  16 KiB byte cap's own reach (~85 uops of the fattest recipe exceeds the cap first), so a
  label-driven refusal can no longer be mis-counted as `refused_fit`. (The JITCG_DUMP getenv item
  is the performance audit's — untouched, as briefed.)
- **NICE d**: the `blocks_reused` comment (`jit_codegen.c`) no longer names the survival path
  that does not exist (`uop_clear_internal` drops the compiled blocks too; page invalidation
  drops both registries symmetrically) — the lookup is documented as harm-defensive, the counter
  identically 0 today.

## G5 — no regression (every gate on the FINAL tree)

**Concurrency disclosure:** a second unit ran in this directory during my session (its marker:
the §12 "nobb follow-ups" correction appended to `results/m2-5-baliveau-guest.md` at 13:14, plus
`tests/gate-m2-5-guest.sh` and `tests/images/guest-initrd-shellfree.xz` rewritten 12:59–13:06).
**`git status`'s `src/` modifications are exactly this unit's 15 files — no foreign source
change entered any build or gate I ran.** The working-tree modifications under
`results/m2-5-*` are that unit's re-pinned gate captures, regenerated when my determinism-gate
run invoked the (re-pinned) gate — I did not edit them by hand. The determinism gate PASSED
against the re-pinned guest image.

| gate | result |
|---|---|
| `make` | rc 0 (`-Werror` clean; the probe also builds clean) |
| `make selftest` | rc 0 — cpu 3522, m1c 228, hw 551 (0 skipped), uop_cache 7,934,349, uop_exec 102, jit 68, jit_codegen 3006 — **all seven counts IDENTICAL to a pre-fix run of the audited tree** (`/tmp/secfix1-prefix-selftest.txt`), 0 failures everywhere |
| `make nasm` | **118/118**, both modes (shipped default and `--uop-cache off`), same 5 skips |
| `make transcript-diff` | rc 0, byte-identical to the reference's |
| 1.5 G kernel boot | final state IDENTICAL across jit-on / jit-off / **the pre-fix binary**: `1500053915 instructions, in_hlt=0, exception=-1, unimplemented=0, eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000`; serial sha256 `7044a6f6…` identical in all three (matches perf-fix-3's pinned terminal state) |
| `sh tests/gate-reboot-1.sh` | **39/39**, rc 0 |
| determinism gate | rc 0, 0 failing assertions; the two runs agree with each other (4,000,024,062 = 4,000,024,062) and the inputs are the pinned ones. The count itself is NOT a pin — `results/m2-5-determinism.md` records six distinct historical values (the guest reads the host clock); stated rather than silently reconciled |
| `make bench` | rc 0, every run 200,095,214 instructions + the terminal-state clause + the serial sha256; 1.42–1.56 s (~137–145 MIPS) vs the brief's ~1.55 s / ~129 MIPS — within noise, on the fast side |
| aarch64 | `build/v86c.aarch64.elf` + the seven selftest halves relinked fresh from the current sources |

## Threat-model accounting (what a guest can still do)

A guest can point EIP at raw guest RAM (including the VGA hole) and execute whatever bytes are
there — the release reference's own behaviour, now reproduced. A fetch past guest RAM stops the
machine **by name** (`unimplemented_instruction`) — loud, bounded, non-corrupting. The remaining
device-path stops (`has_request`/`pop_request`'s asserts, the blob asserts' namesakes) are
internal contracts with every guest path guarded; each carries its sweep verdict at the site.
Nothing in this unit weakens an invalidation hook, the staircase, or the JIT default, and no
`-DNDEBUG` was introduced anywhere.

## Coverage limits, stated

- The S1 sweep is exhaustive over `src/hw/`'s assert sites and the CPU fetch/execute paths the
  audit named; `src/cpu/exceptions.c`'s 95 asserts (CPU-semantics "unreachable" markers and
  descriptor-machinery contracts) were sampled, not exhaustively proven unreachable — they sit
  behind the router-bounded reads audit-1 verified, and the port's own gates (nasm corpus, two
  Linux boots, 4 G determinism) exercise them constantly.
- The `--cdrom` reachability verdict for S3 is a code-read of `main.c`/`machine.c` (no media
  flag exists), not a fuzz of the CLI.
- The eip-hole AFTER comparison covers the final register state; the memory-window sha was not
  compared against the reference leg (the executed zeros only write through `add [eax],al` with
  the loader's eax, both legs identically).

## Corrections (REVIEWFIX-1, from the two review passes)

- **N6 (AUDIT-1R): commit cc6f0c0's message says "THREE pci.c config-write asserts the
  audit had not named"; FOUR were dropped** (`pci_write8`'s BAR/ROM range, `pci_write16`'s
  alignment, `pci_write16`'s ROM-range, `pci_write32`'s alignment — S1 item 7 above, which
  already said four). The commit is NOT amended (this project does not rewrite history);
  this correction is the record.
- **N5 (AUDIT-1R): the class-check bullet citing `pci_get_io_space` described a function
  that does not exist in the port** (upstream's `get_io_space` was never ported). Corrected
  in place above, marked there.
- **SF3 (AUDIT-1R): S3's clamp is a declared divergence** (upstream's DMA arm dies; the
  port clamps both arms) — now declared in S3's section, in the "Divergences declared"
  accounting, and at the site. See results/reviewfix-1.md for the full account.
- **SF1/SF2 (AUDIT-1R): two further guest-reachable asserts in files the sweep had not
  touched** (`rtc.c` bcd_unpack's nibble asserts; `acpi.c`'s PM-timer monotonic assert,
  reachable through the port's own reboot path) — fixed by REVIEWFIX-1, each with a
  demonstrated abort-before/run-on-after probe; see results/reviewfix-1.md.
