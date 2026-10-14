# Security — the threat model, the audit, and what is verified clean

An emulator has an unusual threat model: **the guest is untrusted code by construction** — running
it is the whole point — and the emulator is a single host process with no sandbox of its own. So the
boundary is not "can the guest do what it wants inside the machine" but:

> **can a guest make the emulator read or write host memory it should not, and how far is that from
> control-flow hijack rather than a crash?**

Severity is calibrated on that line. A guest that can crash the host process is a finding — a guest
image is data an operator may have been handed. A guest that can drive the emulator's control flow
is a different one.

Two hardening choices are visible in the shipped design before any audit:

- **the fatal stops, not the silent values.** A missing implementation stops the machine and names
  the symbol (`unimplemented_instruction`); an access outside guest RAM stops and names the region
  (the DMA/IDE guards). The port prefers a stopped machine to a plausible wrong answer.
- **the JIT's instruction stream is not guest-derived** — see below, because that is the property
  the audit could not break.

---

## 1. The audit, and its findings

Three reviewers, one surface each (`results/audit-1-security.md`, `audit-2-jit.md`,
`audit-3-perf.md`); the fixes are `results/secfix-1.md`.

### 1.1 BLOCKER (demonstrated, fixed) — a PCI config write ran off the port table

`pci_set_io_bars` took `to = value & ~1 & 0xFFFF` from a **guest's 32-bit PCI config write to any io
BAR** (so `to` up to 0xFFFE) and its only guard was `to + i >= 0x1000` — **never an upper bound** —
while `count = bar->size` (16 for the IDE BAR4 that exists in *every* machine, up to 64 for virtio).

The consequence is the reason it was a blocker rather than a bug: **`io_mmap` — the mmap dispatch
table of function pointers — begins exactly `0x10000 × 56` bytes after `io_ports`, with zero gap**
(MEASURED in the shipped binary with `nm`; both are BSS neighbours). Up to ~3.5 KB of port entries
were therefore written into the dispatch table at guest-chosen offsets, i.e. **guest-supplied
pointers into a table the emulator calls through.**

The fix bounds both arms, and the bound is a **discard** — which reproduces upstream's *observable*
semantics, because upstream's JS array grows past `0x10000` where its 16-bit dispatch can never read
it, and this port's dispatch masks identically. Probe: the mmap-dispatch answer went `0x10`
(corrupted) → `0xffffff`, byte-identical to the control fixture.

**The class, not just the site**: upstream's containers are growable JS objects and a C array needs
its bound spelled out. The sweep covered `io.c`'s registration bound, `pci_bars`, and the remaining
`io_ports[...]` writes.

### 1.2 Guest-reachable asserts abort the process (fixed, nine sites)

Asserts are **live in the shipped build** by design (there is no `-DNDEBUG`), and a two-instruction
image (`jmp 0xB0000`) aborted the process (rc 134) where the pinned reference executes the zeros.
Nine sites were resolved at their own site to match the release reference — the fetch asserts,
virtio's notify/sends and `pop_request`, `push_reply`'s size-0 path, and **three pci.c config-write
asserts the audit had not named**. Probe: rc 134 → rc 3 with the final state **identical to the
pinned JS reference** in all three configurations.

`-DNDEBUG` is never the fix here: it would silence stops the rest of the port relies on. (This class
had been resolved once before, in the reboot work.)

### 1.3 The blob sinks had no non-assert bounds (fixed)

`memory.c`'s `write_blob` and `virtio.c`'s `virtio_guest_read_blob` are reached from guest
descriptors with arbitrary 32-bit addresses. Under asserts they aborted; **without them they were a
silent unbounded host out-of-bounds write/read — and the read arm leaked host bytes into the
guest-visible console sink.** Both are clamped now (the tree already had the correct pattern in
`kernel.c`), `dma_do_read` was given its twin's named stop, and one declared divergence is recorded:
where the release *dies* (`mem8.set` throws, measured in node), this port clamps.

### 1.4 Latent: the ATAPI DMA read's length (fixed)

`ide.c`'s ATAPI DMA path clamped the transfer's *start* but not its length — an out-of-bounds host
read of the disk-image buffer whose contents are then DMA'd back to the guest. Unreachable today
only because no CLI flag attaches CD media to the always-present ATAPI device; the PIO twin already
had the clamp. Probe: SIGSEGV → rc 0.

### 1.5 The JIT's data structures (fixed)

`jit_mem.c`'s hash insert reused no dead slots and detected no full table, unlike its sibling 100
lines away, so adversarial self-modifying-code churn could push binds past the table and **silently
overwrite a live slot** (wrong-answer-safe, because the full key is compared, but silent
data-structure corruption). Dead-slot reuse and a named full-table refusal are in; the probe shows
sentinel-lost → sentinel-live across 6,000 churns.

---

## 2. The claim the JIT audit attacked and could not break

**No emitted instruction opcode, register field or encoding is derived from guest-controlled data.**
The reviewer traced every guest-derived uop field to its use in the encoder: register fields select
only within the fixed 8-entry guest→host map, the scale is masked twice, immediates and displacements
are payloads, the segment index is bounded into an 8-element array, and the `fixed_g` digit is
refused above 7 wherever a recipe reads it. **The only guest→recipe path is the uop's own function
pointer** — the descriptor table's body pointer, the same one the interpreter calls.

Also verified on that surface: every emitter `put` is bounds-checked with a **sticky overflow** and
`finish` refuses before patching; **W^X holds end to end** (no window where a page is writable and
executable, `mem8` is never executed, and the deferred recycle, the chain path, the SMC abort and the
reboot's cache clear were each traced); the block key is a full `(phys, virt, state)` triple compare
with the privilege bit folded in; and the generator reproduces its committed output.

---

## 3. Verified clean (so the next reader does not re-derive it)

- **The CPU core's router story holds everywhere.** A guest page table can name any physical
  address; `in_mapped_range` catches it, and the 16-byte allocation slack covers the worst tail
  overread of the memory model.
- **GDT/IDT/LDT/TSS and the I/O-permission bitmap reads are all router-bounded.**
- **Both loaders clamp.** The bzImage, ELF and multiboot headers are attacker-controlled files and
  every field taken from them was checked.
- **Every "outside guest RAM" stop is a guard, not a tripwire** — the access cannot happen, rather
  than having happened before the message.
- **No `%s` is fed non-NUL-terminated guest data**, and no `getenv` diagnostic sits on a hot path
  (three were removed from one after a measured 30% cost).
- **The largest unvetted surface is named**: the vendored Berkeley SoftFloat amalgamation behind the
  x87/SSE path, which the port patched to link (`-ffunction-sections` + `--gc-sections`). It is
  third-party code that ships, and this file says so rather than implying it was reviewed.

## 4. What this document does not claim

No fuzzing, no sanitizer run under cosmocc (ASan does not link with it — UBSan does), and no
sandbox of its own. The port's containment story is the same as upstream's: **the virtual machine is
the boundary**, and any process that runs an untrusted guest with this emulator should treat the
emulator as part of the attack surface at the host's own privilege level. The audit above is a
targeted read of the guest-to-host paths, not a proof.
