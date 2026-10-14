# Architecture

How the port is put together, and where each decision came from. Claims are marked
**MEASURED** (run here, command in `results/`), **READ** (source, with a path — `v86/…` is the
read-only reference clone) or **INTERPRETATION**.

The shape of the whole thing: **one generated dispatcher, three engines over it, and one memory
model that all three pay for.**

---

## 1. The generated dispatch — the port's central decision

v86's instruction semantics live in a declarative table, `v86/gen/x86_table.js` (READ, 875 lines),
and upstream generates its interpreter *and* its JIT from it with `v86/gen/generate_*.js`. This port
does the same, with its own emitters:

| emitter | emits | size (MEASURED) |
| --- | --- | --- |
| `gen/interpreter_c.js` | `src/cpu/interpreter_gen.c` — the base and 0F dispatch switches | 14,727 lines |
| `gen/interpreter_uop.js` | `src/cpu/uop_gen.c` — the decode-descriptor tables the cache and the JIT consume | 5,558 lines |

The table is **imported unchanged and never copied**, so an opcode's semantics cannot be re-decided
by hand — only its *implementation* can be missing, and a missing one stops the machine with the
symbol named rather than executing something plausible. The census is printed by the generator:
**0 unimplemented call sites** (MEASURED).

Both generated files are pure functions of the table plus the set of `instr*` definitions present in
`src/`; re-running an emitter on the same tree reproduces its output byte for byte (MEASURED).

## 2. The three engines

They are layered, and each is a fallback for the one above:

```
cycle_internal (src/cpu/cpu.c)
  ├─ uop cache ON?  ──> compiled block?  ──> the JIT executes host code        (--jit on|off)
  │                     else ──> the uop executor walks decoded uops          (--uop-cache on|off)
  └─ else ──> interpreted_run: the generated switch, one instruction at a time
```

- **The interpreter** (`interpreter_gen.c` + the hand-written `instructions*.c`) is the reference
  path: whatever the other two do must match it.
- **The uop cache** decodes a guest basic block ONCE into a compact array of uops (function pointer +
  decoded operands + a segment choice + the flags the descriptor demands), keyed **physically**
  `(phys, virt, state)` — a key that includes the mode bits, so a block can never be reused under a
  different machine state.
- **The JIT** compiles those same blocks to x86-64 with our own encoder (`src/cpu/jit_x64.c`) and a
  hand-written recipe per descriptor (`src/cpu/jit_codegen.c`). It is ON by default and worth ~1.7×
  over the cache at **97.67% dynamic inlined coverage** (MEASURED).

**Why the JIT's instruction stream is not guest-derived** — the property the security audit attacked
and could not break (see `docs/security.md`): the *opcodes* come from the recipes, which are written
against descriptors from `x86_table.js`. The guest chooses operands, displacement, scale/index and
control flow, never an encoding. The only guest→recipe path is the uop's own function pointer.

### The staircase — the invariant that constrains all three

`microtick()` is the port's clock: `instruction_counter / 1000.0` milliseconds, i.e. **one guest
instruction = one microsecond**, and it feeds the TSC, the PIT and the ACPI PM timer (MEASURED,
D-F2 in `results/m2-1f-policy.md`). That makes the machine deterministic *and* makes the
**batch shape guest-observable**: an engine that flushes at different points moves the guest's
clock. Every engine therefore reproduces the reference's counter staircase value for value, and a
chained JIT jump — which skips the return to the dispatch loop — is only allowed where the elided
bookkeeping is provably unobservable (`results/jit-6r-review.md`, `results/perf-7.md`).

## 3. The memory model

- Guest RAM is `mem8`, allocated at `memory_size` rounded to 4 KiB **plus 16 bytes of slack**, so an
  unconditional 4-byte load at the end of the last page cannot fault (READ `memory.rs:135`; the
  slack is why the port has `mem8_alloc_size()`, which the selftest pins).
- Every access first tests `in_mapped_range(addr)`: inside it, a plain unaligned load or store;
  outside it, the **device hook table** (`memory_device_hooks_t memory_devices`, `src/cpu/memory.c`)
  — the C replacement for upstream's `mmap_*` wasm imports, routing the VGA window, the LFB, the
  APIC/IOAPIC windows and the unclaimed-memory fallback.
- Paging is a 4 MiB TLB (`int32_t tlb_data[0x100000]`) with a 4 KiB walk kept as an **exact** port of
  upstream's, including its deliberate gap: v86 never tracks guest writes to page tables, so
  invalidation happens only at `invlpg`, CR3/CR0 changes and the explicit clears. Preserving that was
  a plan decision, not an oversight (`results/cpu-port-plan.md` §3).
- PSE 4 MiB pages are supported; PAE is **trapped**, never half-ported (`TODO.md` §1.3).

## 4. The fault contract

Upstream propagates page faults with Rust's `?` (`return_on_pagefault!`): a faulting instruction
returns early **with partial effects** and re-executes after the handler's `iret`. C has no such
propagation, so the port uses a per-CPU flag plus an early return, and fixes the window it is live
for. The seam is load-bearing and was got wrong once — a *delivered* fault left the flag set and the
loop aborted the machine (`results/r5-review.md` B1); the fix and its reason are in
`results/r5f-fixes.md`.

## 5. The device layer

`src/hw/` — one C file per upstream JS file, so the mapping stays auditable in both directions:
`io.c` (the port map, the mmap dispatch and the device hooks), `bus.c`, `pit.c`, `rtc.c`, `dma.c`,
`ps2.c`, `acpi.c`, `pci.c`, `uart.c`, `ide.c`, `virtio.c`, `virtio_console.c`, and `machine.c` (the
device set in the reference's own creation order, plus the timer fan-out).

Two structural notes worth knowing before reading a device:

- **A guest-controlled index into a fixed C array is a bug generator.** Upstream's containers are
  growable JS objects; a C array needs the bound spelled out. The audit found exactly that class in
  `pci_set_io_bars` and it is now bounded (`docs/security.md`).
- **The unclaimed-port contract is the reference's**: reads answer `0xFF`/`0xFFFF`/`-1` and writes
  are discarded, matching `v86/src/io.js`. The port's development stop on an unclaimed port was
  removed for exactly that reason — it would have killed the first boot at `outb $0x80`
  (`results/m2-1f-policy.md`).

## 6. The boot paths

| path | what it is | why it exists |
| --- | --- | --- |
| **multiboot / ELF** | a raw flat image or an ELF, loaded verbatim at a link address | the oracle's path (`README` §4) — the reference's own multiboot loader, so the CPU tests need no BIOS |
| **bzImage** | Linux's boot protocol: setup sectors, boot params, initrd, cmdline | the real workload, and what both baliveau lanes use |
| **firmware** | `tests/minibios.asm`, a 64 KiB 16-bit firmware assembled into the binary | MEASURED: the reference cannot boot a bzImage without a BIOS — `load_kernel`'s option ROM is executed by seabios via fw_cfg, which this port does not have |

A guest reboot **re-enters whichever path started it** (`results/reboot-1.md`), and the reset
deliberately does **not** touch memory or the disk — that absence is what makes an in-place reboot
useful, and it is upstream's behaviour too.

## 7. The emitters' contract with the hand-written code

The dispatch is generated; the semantics are not. The boundary is a naming scheme
(`make_instruction_name`, mirrored verbatim from `v86/gen/generate_interpreter.js:87-101`) and a
fault convention, both documented at the top of `src/cpu/instructions.c` and in
`results/m1a4-emitter-runloop.md`. A hand-written function whose name does not line up simply is not
called — and the generator's census says so.
