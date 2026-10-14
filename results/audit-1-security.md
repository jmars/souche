# AUDIT-1 — SECURITY: host memory safety against guest-controlled values

**Reviewer:** audit-1 (reviewer, HANDOFF=v86c-audit) · **Date:** 2026-10-14 · **Tree:** clean at
commit `a49a887` (all sha256 below taken from the reviewed working tree).

**Threat model (from the brief).** The guest is untrusted code by construction; the emulator is a
single host process with no sandbox of its own. The question is not "can the guest do what the
guest wants" — it can — but **can a guest make the emulator read or write host memory it should
not, and how far is that from control-flow hijack rather than a crash**. Along that line: a guest
that crashes the host process is a finding (a guest image is data an operator may have been
handed); a guest that can corrupt the emulator's own dispatch structures is a different one.

**Verdict: 1 BLOCKER (demonstrated with a probe), 3 SHOULD-FIX (1 demonstrated), 3
NICE-TO-HAVE.** The port's memory-safety story — *every access is bounded by the memory router*
(`in_mapped_range`) — **holds** for the CPU core's own reads, the GDT/IDT/LDT/TSS machinery and
the loaders. It is broken in three places: the PCI io-BAR write path (B1), the two blob sinks whose
only bounds are assertions (S1/S2), and one ATAPI arm that missed its twin's clamp (S3).

Two probes were run from `/tmp` (the tree is untouched; the emulator was the already-built
`build/v86c`). A demonstrated crash is quoted; what was only argued is labelled.

---

## B1 — BLOCKER (demonstrated): `pci_set_io_bars` writes past `io_ports[]` into the mmap dispatch table

**Where:** `src/hw/pci.c:451-470`.

```c
void pci_set_io_bars(pci_bar_t *bar, int32_t from, int32_t to)
{
    uint32_t count = bar->size;
    for (uint32_t i = 0; i < count; i++) {
        if (from + (int32_t)i >= 0x1000) {
            io_ports[from + (int32_t)i] = io_create_empty_entry();   // (a)
        }
        io_port_entry_t entry = bar->entries[i];
        io_port_entry_t empty_entry = io_ports[to + (int32_t)i];     // (b) OOB READ
        ...DBG_ASSERT(empty_entry...)...
        if (to + (int32_t)i >= 0x1000) {
            io_ports[to + (int32_t)i] = entry;                       // (c) OOB WRITE
        }
    }
}
```

**The guest-controlled input.** One 32-bit PCI config write: `out 0xCF8` selects any io-BAR
dword of any PCI function, `out 0xCFC` supplies the value. `pci_write32` (`pci.c:365-371`) takes
the io arm with `to = written & ~1 & 0xFFFF` — **any even value up to 0xFFFE, no alignment to the
BAR size, no upper bound**. The only guard in the loop is `>= 0x1000`; the array is
`io_ports[0x10000]` (`io.h:74`). With `bar->size` = 16 (IDE BAR4) and `to` = 0xFFFE, entries
2..15 write past the end; with `bar->size` = 64 (virtio common-config BAR0) the overrun is up to
63 entries ≈ 3.5 KB.

**What is overwritten.** Each written entry is `io_port_entry_t` — six function pointers plus a
`device` pointer (56 bytes). MEASURED (`nm build/v86c.com.dbg`): `io_ports` @ `0x933480`,
`io_mmap` @ `0xcb3480`; the gap is `0x380000` = **exactly** `0x10000 × 56` — `io_mmap` follows
`io_ports` with no gap. `io_mmap` is the memory-mapped-device **indirect-call dispatch table**
(`io.c:163-187`: `fn(addr)` / `fn(addr, value)` on every access to the VGA hole, the LFB window
or any address ≥ `memory_size`). The 56-byte entries over 32-byte `io_mmap_entry_t` slots place
**saved `device` pointers (host data addresses) into called slots** at attacker-chosen offsets —
the guest chooses `to` and therefore the phase of the overlay. Minimum impact: crash. Worst case:
type-confused indirect calls of known code pointers with guest-chosen arguments, plus 784 B–3.5 KB
of function-pointer-bearing corruption of host dispatch state. Not direct arbitrary code
execution (every written value is one of the port's own pointers) — but this is control-flow-
adjacent memory corruption, the class this audit was asked to calibrate.

**Reachability.** The IDE controller with its io BAR4 (size 16) is registered in **every**
machine — `machine.c:103-105` passes the (always-present, possibly empty) ATAPI slot
unconditionally, so `has_secondary` is always true (`ide.c:2236-2245`). No CLI flag needed.
The virtio BAR0 (size 64) additionally exists whenever `--virtio-console` is given (baliveau's
console path).

**Demonstration** (probe `/tmp/audit1/pci-bar2.asm`, 66-byte multiboot image, default machine):

```
mov dx, 0xCF8 ; mov eax, 0x8000F020 ; out dx, eax   ; select 00:1e.0 (bdf 0xF0) dword 0x20 = BAR4
mov dx, 0xCFC ; mov eax, 0x00FFFFFE ; out dx, eax   ; to = 0xFFFE (low bit set = io)
mov al, [0xA0000]  ; esi <- 0x00 (unchanged)
mov al, [0x200000] ; edi <- 0xFF (indistinguishable from default)
mov al, [0x280000] ; ebp <- 0x10   <<< CONTROL: 0xFF
```

Control image is byte-identical except the BAR value is `0x0000B401` (the BAR's own current
value, `to` = 0xB400, in bounds) — the single changed variable is the guest's BAR value.
**Result: the read of mmap block 20 answers 0x10 after the probe write and 0xFF in the control
run** — the guest changed the emulator's dispatch answers with one 32-bit config write.
Reproduced under the interpreter (`--jit off --uop-cache off`), the uop cache and the JIT alike.

**Upstream comparison.** `v86/src/pci.js:519-549` has the same loop — but `this.io_ports[to+i] =
entry` on a JS array **grows the array** past 0x10000, and those entries are unreachable because
the dispatch reads `io_ports[port & 0xFFFF]`. Upstream observably **discards** this write; the
port turned it into an unbounded host write. (The port knows this class: `pci_bdf_is_absent`
at `pci.c:213` was added for exactly the "16-bit index into a 256-entry array" sibling.)

**Fix.** Bound both writes — `if (to + i < IO_PORT_COUNT)` and the same for `from` — which
reproduces upstream's observable semantics (the write is dropped), and drop or make safe the
`empty_entry` OOB read at (b).

---

## S1 — SHOULD-FIX (demonstrated): guest-triggered `assert()` aborts where the reference keeps running

`assert()` is live in the shipped binary: `CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra`
(`Makefile:26`), no `-DNDEBUG` anywhere, and `DBG_ASSERT` maps to `assert()` (`cpu_state.h:36-43`;
`Makefile:428` says the selftests rely on it). Every guest-reachable `DBG_ASSERT` on a
data-dependent path is therefore a **guest-controlled kill of the host process**.

**Demonstrated** (`/tmp/audit1/eip-hole.asm`, a two-instruction image: `jmp 0xB0000`):

```
$ ./build/v86c /tmp/audit1/eip-hole.img 100
./src/cpu/cpu.c:100: assert(!in_mapped_range(phys_addr)) failed
Aborted (core dumped)          # rc 134
```

The guest points EIP into the VGA hole; instruction fetch translates to a mapped-range physical
address and `modrm.c:121` / the inlined `cpu.c:100` assert fires. Upstream reads `mem8` at that
offset in its wasm linear memory (zeros) and executes them — no host crash.

**Same class, confirmed by code reading:**

- `src/hw/virtio.c:1254-1255` — `virtqueue_pop_request` asserts `avail_addr != 0` and
  `has_request`. `virtio_console.c:116/131/147` (`console_send_event`/`send_name`/
  `send_window_size`) pop `queues[2]` **without** checking it: a malicious guest that configures
  queue 3, posts an 8-byte `DEVICE_READY` control packet and never configures queue 2 aborts the
  machine. Upstream's `dbg_assert` is compiled out; it reads garbage and runs on.
- `src/hw/virtio.c:69-70` (`virtio_guest_read_blob`) and `src/cpu/memory.c:72-73` (`write_blob`)
  — a virtio descriptor with `addr_low ≥ memory_size` (fully guest-chosen) asserts. See S2.

REBOOT-FIX-1 already resolved this exact class once (`cpu.c:195`'s progress assert aborted on a
guest-controlled batch geometry). The sweep needs finishing: guest-reachable, data-dependent
`DBG_ASSERT`s should degrade (discard/return, as `ide.c:514-526` already does for the data-port
overrun) rather than abort. Each residual site is a DoS, not corruption — but it is a guest
image (data) killing the operator's process.

---

## S2 — SHOULD-FIX: the two blob sinks have no non-assert bounds (the `-DNDEBUG` distance)

**Where:** `src/cpu/memory.c:64-82` (`write_blob`) and `src/hw/virtio.c:66-73`
(`virtio_guest_read_blob`):

```c
static const uint8_t *virtio_guest_read_blob(uint32_t offset, uint32_t length)
{
    if (length) {
        DBG_ASSERT(!in_mapped_range(offset));              // the ONLY bound
        DBG_ASSERT(!in_mapped_range(offset + length - 1));
    }
    return mem8 + offset;                                  // then memcpy'd by the caller
}
```

`write_blob` is the same shape. Both are reached with **guest-chosen offsets**: every virtio
descriptor chain (`virtq_chain_get_next_blob` `virtio.c:1373`, `virtq_chain_set_next_blob`
`virtio.c:1409`) passes `buf->addr_low` straight through, and `dma_do_read` (`dma.c:240`) passes
the DMA page/addr registers. Today these are S1-class assert-aborts. Under `-DNDEBUG` (nothing in
the build prevents `CFLAGS=-DNDEBUG make`) they become **silent unbounded host OOB access**: the
read arm leaks host heap bytes into the console-output sink; the write arm writes device data at
a guest-chosen host address — heap corruption. The tree already contains both correct patterns to
copy: the loader's bounded `write_blob` (`kernel.c:56-73`: clamp + log) and the named stop
(`ide.c:816-825`, `dma.c:273`).

**Also:** `dma_do_read` (`dma.c:224-242`) calls `write_blob` with a fully guest-controlled DMA
address and **no** `memory_size` guard — its twin `dma_do_write` has one (`dma.c:273`). Dead code
today (the floppy is not ported; only the selftest calls it), but the asymmetry is the bug: add
the same guard.

---

## S3 — SHOULD-FIX (latent): ATAPI DMA read misses the clamp its PIO twin has

**Where:** `src/hw/ide.c:1179-1205` (`interface_atapi_read_dma`).

```c
if (ide->buffer == NULL || start >= ide->buffer_length) { ...reject... }
...
interface_data_set(ide, ide->buffer + start, byte_count);   // memcpy of byte_count bytes
```

`byte_count = count * sector_size` with `count` taken from the guest's READ_12 CDB (a full
32-bit big-endian field; wraps at 32 bits) and `start < buffer_length` is the **only** check —
unlike the PIO twin `interface_atapi_read` which clamps
`byte_count = min(byte_count, buffer_length - start)` (`ide.c:1159-1160`). The memcpy source runs
past the end of the host buffer holding the disk image, and the copied bytes are then delivered
into guest RAM by the PRD walk — an OOB host **read** with a guest-chosen length and the contents
disclosed to the guest. Upstream is safe because `SyncBuffer.get` builds
`new Uint8Array(this.buffer, start, len)`, which **throws** out-of-range
(`v86/src/buffer.js:35-39`).

**Unreachable today only because** `machine_config.cdrom` is never given media — no CLI flag
attaches a CD image to the always-present ATAPI device (`main.c` has no `--cdrom`;
`machine.c:66`). The first such flag makes it live. **Fix:** apply the PIO arm's clamp.

---

## Nice-to-have

- **N1** `src/hw/ide.c:242-254` — `data_write16/32` have no `data_capacity` checks while their
  read twins do (`ide.c:228-240`); safety rests entirely on the
  `data_end ≤ data_length ≤ data_capacity` invariant across the command state machine (including
  the ATAPI `byte_count` arms at `ide.c:563-571`). Cheap symmetry fix.
- **N2** `src/hw/io.c:351-379` — `io_register_*_consecutive` register `port+1..port+3` bounded
  only by `DBG_ASSERT(port < 0x10000)`; a registration at ≥ 0xFFFD would write `io_ports` OOB.
  Every current caller is a fixed low port — make it a hard bound before a future device
  forgets.
- **N3** `src/hw/virtio_console.c:116/131/147` — the unguarded `queues[2]` pops (S1's example
  site); guarding them fixes the abort and documents the "no buffer posted" case.

---

## Surfaces checked and found bounded (the positive results)

- **CPU core reads.** Every `read*/write*` router access is gated by `in_mapped_range`
  (`memory.c:218-514`); a guest PTE/PSE entry can name *any* 32-bit physical page
  (`paging.c:192/227`) and the router routes everything ≥ `memory_size` (and the 0xA0000 window)
  to device hooks. The `+16`-byte allocation slack (`memory.c:33-34`) covers the
  `*_no_mmap_check` tails' worst case — a 16-byte read at `memory_size − 1` overreads the logical
  end by 15 bytes and stays inside the rounded-up allocation (arithmetic verified). The plan's
  §3 promise holds for the fetch/operand/`string.c` fast paths (the latter explicitly gated
  `!in_mapped_range` before its unchecked accesses, `string.c:181/188`).
- **GDT/IDT/LDT/TSS.** Guest-installed tables feed only `translate_*` + `read*` calls
  (`exceptions.c:508-516` IDT, `790-825` descriptor lookup incl. the `> table_limit` check,
  `141-175` TSS stacks, `1831-1870` the I/O permission bitmap) — all router-bounded. `LGDT`/
  `LIDT`/`LLDT` contents cannot produce an unchecked host reach.
- **Loaders (attacker-controlled files).** The bzImage path's `write_blob` is fully bounded
  (`kernel.c:56-73`), the multiboot address-table and ELF branches bound `length` against the
  file and the load region against `memory_size`/`in_mapped_range` (`main.c:229-236, 262-342`);
  out-of-range segments are skipped with a warning (`main.c:320-324`).
- **Named stops verified as guards, not tripwires:** `dma.c:273` (before the memcpy),
  `ide.c:816` (before both PRD sinks; the `to_guest` arm is double-bounded by `ide.c:811` and
  `ide.c:745/780`), `pci.c:281` (bdf checked before every index), `virtio.c:1209-1219` (chain
  bound stops *before* the append; `read/write_buffers` are `[VIRTIO_MAX_CHAIN]` = 64, correctly
  sized), `jit_mem.c:505-517` (page checked before use; JIT lane).
- **`io_mmap[]` indexing** cannot overrun (`addr >> 17` ≤ 0x7FFF < 32768); RTC CMOS index is
  masked `& 0x7F` at the port (`rtc.c:87`); PS/2 commands are a fixed switch; the UART FIFO
  growth loop is bounded; `bus_register`/listener tables are fixed-size with asserts at
  construction time (not guest-reachable).
- **Diagnostics.** Every `getenv` gate is cached one-shot (`io.c:70-78`, `ide.c:265-273`,
  `virtio.c:28-34`) — none sits on a hot path; the JIT's `getenv` sites (`jit_mem.c:379/637`
  bounded by `dbg < 12`, `jit_codegen.c:1467/1713` dump-only) belong to the JIT lane. **No `%s`
  is fed a non-NUL-terminated guest buffer**: every device-layer `%s` takes a constant device
  name or a bounded `snprintf` product; guest byte sinks are `fputc`-per-byte
  (`main.c:734/783`).

## The vendored softfloat — what the patch does and does not imply

The engine is compiled **verbatim** from the read-only clone (`v86/lib/softfloat/softfloat.c`,
Berkeley SoftFloat 3e, 32,501 lines) with upstream's own defines
(`-DSOFTFLOAT_FAST_INT64 -DINLINE_LEVEL=5`, `Makefile:152-163`). The port's only patch is
**build-level**: `-ffunction-sections -fdata-sections` + `--gc-sections` drop *unused* functions
at link time. That implies **nothing about the vetting of the code that ships** — what remains
after GC is upstream's bytes, unmodified. What I did verify: the 275-line wrapper
(`src/cpu/softfloat.c`, read in full) marshals caller-owned 16-byte `f80_t` values (size/offset
pinned by `_Static_assert` in `cpu_state.h``, passes only pointer/struct literals into the
engine, and keeps no internal buffers; the engine's own table lookups
(`softfloat_countLeadingZeros8[byte]`, the `approxRecip*` tables) are bounded by construction and
indexed by engine-internal values. Guest reach is total (every x87/SSE op) with arbitrary bit
patterns as inputs — the use SoftFloat was built for. **Coverage limit, stated plainly:** I did
not line-audit the 32,501-line engine body; the claim above is about the port's integration of
it, not an audit of Berkeley's code.

## Coverage

Read in full: `src/hw/{io,dma,pci,machine,virtio_console}.c`, `src/cpu/{memory,paging,softfloat}.c`;
`src/hw/ide.c` 1-1250 + 2100-2315 (ATAPI command bodies 1250-2100 sampled via their
read/write/identify arms); `src/hw/virtio.c` 1-470 + 990-1430 (field boilerplate 470-990
sampled); `src/main.c` loaders + sinks; `src/hw/kernel.c`; `src/cpu/modrm.c` fetch/segment paths;
`src/cpu/string.c` fast path; `src/cpu/exceptions.c` targeted (descriptor lookup, interrupt
dispatch, ioperm, TSS). Sampled by scan: `ps2.c`, `uart.c`, `pit.c`, `rtc.c`, `acpi.c`, `bus.c`,
`instructions*.c`/`arith.c`/`misc_instr.c`/`fpu.c`/`sse_instr.c` (direct-`mem8` grep — only
router-checked sites), `jit_mem.c` 495-520. Not checked (other reviewers' lanes / not shipped):
`interpreter_gen.c`, `uop_gen.c`, `jit_codegen.c`, `jit_x64.c` (JIT and perf lanes), the
selftests, the softfloat engine body (characterized above).

sha256 of the reviewed files:

```
2a17a177f16ef30b7bf0c27f780f5f9061f0736ed516f377d640450024b8f021  src/hw/io.c
c4644ffd43060915129aae20e0d15d4f1553217f2fdb89dddda4b51a7f01ea0d  src/hw/dma.c
50cc8f5979a4bb255fbfda667e404078cabaef6918084dcd64fd670be21671da  src/hw/ide.c
9920ea9f06ef2fde977b64773f8c33e2d6947ae99032dd15b54bc276d7f06864  src/hw/virtio.c
ad7913d776d9816abfc6cf4287452210f2949d6226c33d978e7ebaec863b8e2b5  src/hw/virtio_console.c
0d10b45288a70807a82d794a2d0a56294de110dbfc376cf2135576647d6fdfaa  src/hw/pci.c
405e562478babcc017995bd39c47ac44ebb17700f3899a3b26875765fa963292  src/hw/machine.c
f89b39fd96d75b702412fb392650fb411007237d9679131bc3a07dfb1308e01b  src/hw/ps2.c
0b435bcfd34c54e872631f064a248af59ed39f86e3d2445cb7f5c57efe173d42  src/hw/uart.c
56ecf88ee38966973495cf892d0a52cc3424552a866e25d29ef13b5717292477  src/hw/pit.c
9ab2a370022ef2de2fb5240f727adacc301555a4a0465120e451223de6508b44  src/hw/rtc.c
57ea0a61935e6a77df2a70f23d0fd2da4702e7e65d3ec2b2a14fa6fe1cac705c  src/hw/acpi.c
bc34d821caff1f70c6cc62c87bcd02ada6daa8b75b4331e0186ba0c898e80400  src/hw/bus.c
61fceef81b651ae61bb128762b2477e99a2eccb94a746733f8322ae1d5f33893  src/hw/kernel.c
5ed5786fb9f2d211b6620c0d7d81efadc6bfb8846b6df56422647de65da28d76  src/cpu/memory.c
a6a0f1ed2e7d7249cc47fa7834cb65175bdcaba1894e2e91e2b8aeebed7c6542  src/cpu/paging.c
48c5e9bd2163e33efa1fb7d2a3184181f6eb520dd83c8997d9ae3672c5b33237  src/cpu/modrm.c
73bb9822af0ea1dfd264f234ae572dd51a37894e29d77cdacece1ddb86604ae6  src/cpu/string.c
5338a7b14c666186fc215f99eed62ef6d80c0f203496699afb4c386225d6331c  src/cpu/exceptions.c
f4b65eae57b48bb78e68190d9242337c2598839477d48a14f29bde9a6a89bd8b  src/cpu/softfloat.c
c4a712ab80f701edbddc84165072581a72bdb8f1dc7646c243b0f165d68d45e  src/main.c
```

Probes (kept in `/tmp/audit1/`, tree untouched): `pci-bar2.asm` + `ctl.asm` (the B1 probe and
its single-variable control), `eip-hole.asm` (the S1 demonstration), `pci-bar.asm` (the first
B1 sighting). Review node: `handoff-v86c-audit1` in the agent memory store.
