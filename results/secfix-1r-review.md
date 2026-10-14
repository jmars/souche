# SECFIX-1R — independent review of the security fixes (commit cc6f0c0)

**Unit:** `HANDOFF=v86c-audit`, SECFIX-1R (reviewer). **Under review:** `results/secfix-1.md`
(the SECFIX-1 unit) against `results/audit-1-security.md` / `results/audit-2-jit.md`, on commit
cc6f0c0. Read-only; every claim below is grounded in the code read, with the upstream file and
line it was checked against.

**Verdict: the LANDED fixes are correct and reference-faithful — B1's upstream argument was
verified at its source and holds; all nine assert resolutions match the pinned release's
behaviour at their sites; the blob clamps reproduce `mem8.set`/`subarray` semantics exactly;
the JIT hash insert is a correct tombstone scheme. Two NEW guest-reachable members of the S1
class were found in files the sweep did not touch (`rtc.c`, `acpi.c`) — the sweep's
"exhaustive over `src/hw/`'s assert sites" claim is FALSE. 0 blockers in the landed code;
2 SHOULD-FIX (new sweep findings), 1 SHOULD-FIX (an undeclared divergence), 3 NICE.**

---

## B1 — the port-table bound: CORRECT, and the upstream argument HOLDS at its source

The unit's argument was attacked exactly where the brief asked. Findings, all READ:

1. **`io.js` itself does NOT mask** — `port_write8/16/32` and `port_read8/16/32`
   (v86/src/io.js:312-402) index `this.ports[port_addr]` directly. The guarantee lives in the
   *callers*: every dispatch site passes a port ≤ 0xFFFF — the DX forms read the register as
   16-bit (`let port = read_reg16(DX)`, v86/src/rust/cpu/instructions.rs:2096-2119), and the
   immediate forms (E4-E7) take an imm8. The wasm imports (browser/starter.js:74-79) route
   straight into these six functions. So a `ports[i]` with i ≥ 0x10000 is written by
   `set_io_bars` but read by NOTHING: no iteration over `ports` exists anywhere in v86/src
   (only indexing), and `get_port_description` takes the dispatch addr.
2. **`set_io_bars` upstream** (v86/src/pci.js:546-571): both arms guard only `≥ 0x1000`,
   exactly as the port's did. `to`/`from` are already masked `& ~1 & 0xFFFF` by the caller
   (pci.js:429-430), so `to+i` ≤ 0x1003D — the grown entries are unreachable, i.e. the
   observable behaviour of the reference is a discard.
3. **The straddling head is handled right**: for `to = 0xFFFE` the entries at 0xFFFE/0xFFFF
   are `< IO_PORT_COUNT` and ARE written (the port's bound is `to+i < 0x10000`, not `to <`),
   matching upstream; only the i ≥ 2 tail is discarded — and that tail is observable nowhere
   in either machine, including on a later `from`-arm clear of the same BAR (also unobservable
   in both). The `from` arm is bounded identically. `pci_register_device`'s snapshot loop
   keeps a DBG_ASSERT (`port + j < IO_PORT_COUNT`) — registration-time, host-controlled BAR
   defaults, a genuine contract.
4. **The port's dispatch masks identically** (`io_ports[(uint32_t)port & 0xFFFF]`, io.c:415
   through 484 — all six entry points), so the discard reproduces the reference's answer at
   every reachable port.

**Divergence: none observable.** G1's flip (0x10 → 0xffffff == control) is consistent with
this reading. The class sweep's other two bullets check out with one citation error (N5
below).

## S1 — the nine assert resolutions: each matches the pinned release at its site

Checked per site against the upstream code path for the same guest input:

1. **Fetch asserts** (cpu.c `get_phys_eip`/`interpreted_run`, modrm.c `read_imm8`). The
   release reference's fetch reads raw linear memory (`mem8.offset(..)`), *not* the mmap
   hooks — and upstream's `in_mapped_range` (rust/cpu/memory.rs:75-78) is the VGA hole ∪
   ≥ memory_size. So an in-RAM fetch (VGA hole included) executes the raw bytes in the
   release, which is what the port now does; a fetch past the view the release does not
   survive (the pinned `jump` fixture is its SIGSEGV — tests/nasm_skip.txt:33), and the port
   names that stop instead of reading host memory. G2's state equality (all three configs
   identical to the JS reference leg) is the right proof shape and is captured.
2. **`pci_write8`'s range assert** — upstream pci.js:294-297's dbg_assert compiled out; the
   release performs `space[addr] = written` unconditionally. The port now does exactly that.
3. **`pci_write16` alignment** — release: falls through to `space[addr >>> 1] = written`, an
   aliased in-bounds write (addr masked to 0xFF → u16 index ≤ 0x7F). Port matches. The
   BAR-range `[0x10,0x2C)` early *return* (a real control-flow drop, not an assert) was
   already ported and is untouched — correct.
4. **`pci_write16` ROM-range** — same shape, aliased write at index 0x18. Port matches.
5. **`pci_write32` alignment** — release: unaligned addr takes the BAR arm with
   `bar_nr = (addr-0x10)>>2` bounded by `pci_bars_count` in the port (upstream by the JS
   array read); `space[addr>>2] = written|1` stores the FULL 32-bit value. Verified the port's
   BAR arm still stores `written | 1` (not the masked `to`) — an easy divergence to introduce
   and it was not introduced.
6. **virtio notify loops / control sends / push_reply** — the guards are `is_configured`
   (drops with a named note) and `console_control_queue_ready` (is_configured && has_request).
   The declared divergence (the release pops garbage chains and runs on; no defined
   observable) is the audit's own N3 shape and is declared. push_reply's dropped assert: the
   release proceeds and the used-ring write is router-bounded — verified.
7. **The three "NOT guest-reachable" verdicts verified independently**:
   `common_queue_enable_write` calls `virtqueue_enable` only under `virtqueue_is_configured`;
   `common_queue_size_write` rounds to a power of two and clamps to `size_supported` — and
   `data = 0` trips NEITHER of `set_size`'s asserts (`0 & -1 == 0` passes the power-of-2
   assert; `0 ≤ size_supported` passes the clamp), so the guest's queue_size=0 attack lands
   on push_reply (where it was resolved), not here. The restored asserts are genuine
   contracts.
8. **`console_notify_output`'s calloc** — this is required, not cosmetic: with the S2 clamp a
   past-the-end descriptor copies fewer bytes than `length`, and `get_next_blob` still
   advances `dest_offset` by the FULL `read_length` (matching upstream's
   `dest_offset += read_length` on the empty view), so `got == length` and the ZERO tail is
   delivered to the sink exactly as upstream's `bus.send(name, buffer)` delivers the whole
   zero-filled `Uint8Array` (virtio_console.js:98-100). Both `get_next_blob` callers zero-fill
   their buffers; the host-heap-to-sink arm is closed.

## S2 — the blob clamps: semantics verified against `mem8.set`/`subarray`

- **`write_blob`**: upstream cpu.js:299-311 is `mem8.set(blob, offset)` behind two compiled-out
  asserts. `set` on a view THROWS for `offset+len > view.length` — including a straddle — and
  SUCCEEDS with raw bytes for an in-view target (the VGA hole is in-view: linear memory is
  contiguous there; `in_mapped_range` is a *device-hook* concept the raw `set` bypasses). The
  port's bound is `memory_size` (the view length) with a raw `memcpy`: full write when inside
  (hole included), tail-clamp + drop when straddling/outside. That is exactly the declared
  divergence ("the release dies there; the port clamps") with no off-by-one
  (`offset >= memory_size` → drop; `offset + length > memory_size` → keep
  `memory_size - offset` bytes; `uop_invalidate_range` now covers the written range only —
  correct, the dropped tail writes nothing).
- **`virtio_guest_read_blob`**: `subarray` CLAMPS (empty when `offset ≥ length`, truncated on
  a straddle) — the port returns `available` and the caller still advances by the full length,
  so the missing bytes are the pre-filled zeros. Matches the release's clamp semantics
  exactly; no host bytes reach the sink (probe: 16 zero bytes).
- **`dma_do_read`**: the twin's guard, same shape and placement as `dma_do_write`'s, before
  the `channel_addr` update and `write_blob`. The audit's asymmetry is closed. Dead today
  (no floppy) as stated.

## S3 — clamped, latent; one undeclared divergence (SF3 below)

`interface_atapi_read_dma` clamps `byte_count` to `buffer_length - start` (no underflow: the
`start >= buffer_length` reject precedes it), matching the PIO arm's own clamp shape. The
reachability verdict was re-verified after reading the CLI: `main.c` has no `--cdrom` and
nothing assigns media to `machine_config.cdrom` — `ide->buffer` is NULL on the always-present
ATAPI device. LATENT is correct.

**BUT the reference's DMA arm does NOT clamp**: upstream's PIO arm clamps
(`byte_count = Math.min(byte_count, buffer.byteLength - start)`, ide.js) while the DMA arm
passes the raw count to `read_buffer` → `SyncBuffer.get` → `new Uint8Array(buffer, start,
len)` THROWS (buffer.js:35-39) — the release DIES on this input. The port now clamps both
arms. That is a divergence of exactly the class SECFIX-1 declares for `write_blob` ("the
reference's death is its bug"), and it is NOT declared in S3's section or the threat-model
accounting.

## A2-SF — the JIT hash insert: CORRECT

- Tombstone reuse cannot break live-key probe chains: both lookups (`jit_cache_lookup`,
  `jit_chain_target`) probe with "EMPTY ends the search, DEAD continues" — and the first DEAD
  the insert reuses always lies BEFORE the first EMPTY in the probe sequence, so it is inside
  every future search's prefix for that home slot.
- No stale-tombstone resurrection: DEAD is only written by `jit_block_drop` via the block's
  own `hash_slot`; the insert never unlinks a live entry (the refusal fires instead).
- The refusal is reachable in principle (all 4096 slots live — unreachable today with
  JIT_MAX_BLOCKS=2048), named on stderr and counted in `jit_stats.hash_full`, printed by
  `jit_stats_print`. The span is recycled per the JIT-FIX-1 ownership rule, matching the
  block-table-full arm. The unit's self-caught defect (refusal test rejecting a reusable
  DEAD) is fixed correctly: the refusal requires a non-zero, non-DEAD final slot.
- Full-sweep edge cases walked: all-DEAD table (inserts at first DEAD), exhausted sweep
  landing on a DEAD home slot (inserts at first DEAD), exhausted sweep all-live (refusal).

---

## Findings

### SF1 — SHOULD-FIX (new, sweep gap): `rtc.c`'s BCD asserts are guest-reachable — 3 port writes kill the host process

`src/hw/rtc.c:198-199` (`bcd_unpack`): `DBG_ASSERT(low < 10); DBG_ASSERT(high < 10);`
(via `decode_time`, rtc.c:209-213, called at rtc.c:313-315). The guest writes an alarm
register with a nibble > 9, then enables the alarm interrupt:

```
out 0x70, 0x01 ; out 0x71, 0x1A   ; seconds-alarm = 0x1A (low nibble 10)
out 0x70, 0x0B ; out 0x71, 0x20   ; status B: alarm-interrupt enable (BCD mode, bit 2 clear)
```

`rtc_cmos_port_write`'s `cmos_b & 0x20` arm runs `decode_time(cmos_data[1])` →
`bcd_unpack(0x1A)` → `low = 10` → **SIGABRT**. Upstream has the identical `dbg_assert`s
(rtc.js:220-222), compiled out of the pinned release, which computes `low + 10*high` and runs
on with a garbage alarm time. This is audit-1's S1 class verbatim, in a file whose assert
sites the report claims were swept exhaustively ("The S1 sweep is exhaustive over `src/hw/`'s
assert sites"). `n < 0x100` (rtc.c:197) cannot fire (cmos_data is `uint8_t`) — only the two
nibble asserts are live. **Fix:** drop `low < 10` / `high < 10` at the site with the
sweep-verdict comment (release semantics: the wrong-but-defined `low + 10*high` result),
keeping the arithmetic unchanged.

### SF2 — SHOULD-FIX (new, sweep gap): `acpi.c`'s monotonic assert is guest-reachable through the port's OWN reboot path

`src/hw/acpi.c:178`: `DBG_ASSERT(t > acpi->timer_last_value)` in `acpi_get_timer`. The port's
clock is `microtick() = instruction_counter / 1000` (exec_boundary.c:87-89, D-F2) and
`reset_cpu` zeroes `instruction_counter` (cpu_state.c:176) on every machine reboot — but ACPI
is in the reboot decision's NOT-RESET list. So after a guest-initiated reboot (0xCF9 or the
0xFE keyboard path — first-class features for baliveau), the first read of the PM-timer port
(0xB008, io_register_read at acpi.c:139 → `acpi_get_timer(microtick())`) has `t` small and
`t != timer_last_value` → the assert fires → **SIGABRT**. The release reference cannot reach
this even in DEBUG (its clock is the wall clock, monotonic across reboots), and in the pinned
release the assert is compiled out with a graceful "overshoot, waiting" arm — which the code
below the assert ALREADY implements. Note the asymmetry: `pit_did_rollover` has an explicit
`diff < 0` arm for exactly this clock-goes-backward case (pit.c:224-230) — the reboot-reset
clock was anticipated for the PIT and missed for the PM timer. **Fix:** drop the assert (keep
the arm), with a comment naming the D-F2-reboot interaction.

### SF3 — SHOULD-FIX (undeclared divergence): S3's clamp diverges from the release on the DMA arm and the report does not declare it

As above: upstream's ATAPI PIO arm clamps, its DMA arm THROWS (the release dies); the port now
clamps both. The choice is defensible (same rationale as write_blob's declared divergence) but
`results/secfix-1.md`'s S3 section and its "Divergences declared" accounting do not mention
it. **Fix:** one sentence in S3's section (and optionally the site comment at ide.c:1204)
declaring "the release dies here (SyncBuffer.get throws); the port clamps like its own PIO
arm" — latent today behind the missing `--cdrom`.

### N4 — NICE: `write_blob`'s retained comment contradicts the fix 10 lines above it

`src/cpu/memory.c:100-102` still says "write_blob is the LOADER's path (src/main.c), so this
is loader-time only; it is the one site that can write across a page" — while the new S2
comment at memory.c:64-79 (correctly) says write_blob is guest-reachable, and the callers at
`dma.c:258` and `ide.c:835` (IDE bus-master DMA, live with `--hda`) prove it. The sentence
was already stale before this unit, but the unit edited this function and left the
contradiction. **Fix:** reword to "the loader's path and the DMA/IDE blob sinks".

### N5 — NICE: `secfix-1.md` cites a function that does not exist

"pci_get_io_space's registration-time read loop keeps its assert (host-driven)" — there is no
`pci_get_io_space` anywhere in `src/` (upstream `PCI.prototype.get_io_space` was never
ported; devices hardcode their ports). The class-check bullet cites a nonexistent site. The
sweep's substance is unaffected (there is nothing to bound). **Fix:** delete or correct the
bullet.

### N6 — NICE: the commit message undercounts the pci asserts

cc6f0c0's message says "THREE pci.c config-write asserts the audit had not named"; four were
dropped (write8's BAR/ROM range, write16's alignment, write16's ROM-range, write32's
alignment) and `results/secfix-1.md` itself says four. Commit-message-only; the report is the
accurate one.

---

## Coverage

**Read in full:** `src/hw/io.c` (all 574 lines), `v86/src/io.js` (all 471),
`v86/src/pci.js` 289-604, `src/hw/pci.c` 240-530 (the write paths, registration and
`pci_set_io_bars`), `src/hw/virtio_console.c` 106-330, `src/cpu/jit_mem.c` 411-540 (hash
lookup/insert/bind) + the A2-SF diff, `src/hw/dma.c` 215-300, `src/hw/rtc.c` (the
read/write/alarm paths), `src/hw/acpi.c` (the timer paths), the full cc6f0c0 diffs for
pci.c, io.c (via the tree), cpu.c, modrm.c, memory.c/.h, dma.c, ide.c, virtio.c,
virtio_console.c, jit_mem.c/.h, cpu_selftest.c.
**Targeted (the upstream code path for the same guest input):** v86/src/cpu.js
(write_blob/read_blob), v86/src/buffer.js:25-45, v86/src/ide.js (atapi_read/atapi_read_dma),
v86/src/rtc.js:200-235, v86/src/rust/cpu/instructions.rs:2015-2120 (the port-width proof),
v86/src/rust/cpu/memory.rs:75-90 (in_mapped_range), `src/hw/pit.c` (counter/reload asserts —
the reload invariant HOLDS: `counter_write` maps 0→0xFFFF and is the only writer),
`src/hw/virtio.c` (get/set_next_blob, the field-layer guards, notify paths,
`virtio_notify_config_changes`' callers — both pre-check DRIVER_OK, not guest-reachable).
**Evidence read:** results/secfix-1.md (all), results/secfix1-g1g2-captures.txt (all),
secfix1-probe-before/after.txt (skimmed), tests/nasm_skip.txt:33.
**Sampled (sweep triage, no reachability evidence found):** pic.c's internal asserts,
dma.c:48 (`channel_of` — dispatch table guarantees membership), bus.c, kernel.c (loader,
file-controlled), memory.c's memcpy asserts (audit-1 read memory.c in full).
**NOT checked (other lanes / stated limits accepted):** jit_codegen.c and the PERF-7 changes
(a concurrent reviewer's scope), the JIT generator and emitter, softfloat, exceptions.c's 95
asserts (the unit's own stated coverage limit), the actual building/running of probes
(read-only review; every flip cited was re-derived from the code and the captures).

**sha256 (first 16 hex) of reviewed files:** pci.c 5ebc9808f21c1c17, io.c 283bbaec0ed2674b,
virtio.c 31926240b841fe8c, virtio_console.c b6e819d26c8ea062, dma.c ef0e8d058d614feb,
ide.c 60455624eba66b7d, rtc.c 9ab2a370022ef2de, acpi.c 57ea0a61935e6a77,
pit.c 56ecf88ee3896697, memory.c e34cc0e545deffba, cpu.c 489a3d0cd36b90ff,
modrm.c 3019e5078866010e, jit_mem.c 4f07f26b6209832f, v86/src/io.js 2a3f62432d39ca9f,
v86/src/pci.js dfd1ef13049af39d, v86/src/cpu.js 688cc6867ebc82cf,
v86/src/ide.js e7cf7922b4be5b7e, v86/src/rtc.js 043310e9ca09f654,
v86/src/buffer.js 5df0b45051634f00, instructions.rs 011503ca5fd07410,
memory.rs b6e72b1ec56cf98e, results/secfix-1.md 211bc670b4b1a58a.
