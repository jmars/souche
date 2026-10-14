# v86 → C — a native port of the v86 x86 PC emulator

**What this is.** [v86](https://github.com/copy/v86) is an x86 PC emulator written today as
JavaScript (devices, ~35k lines under `v86/src`) plus Rust (the CPU core, ~25k lines under
`v86/src/rust/cpu`, compiled to wasm and driven from `v86/src/cpu.js`). This project ports it
to C and builds it with **cosmocc**, so the result is an Actually Portable Executable: one
binary that boots a guest image on Linux/macOS/Windows/BSD, on x86-64 and aarch64. The
browser lane already exists (v86 itself); **this project is the native lane**.

**Scope (MEASURED from the reference).** A 32-bit x86 PC: the CPU core (386-level
instructions plus the 486/Pentium-era pieces the reference implements), the devices the
reference implements as JS, executed by an interpreter, a uop cache **and a native x86-64 JIT**.

**NON-GOALS — not "not yet", these are out of scope for the port:**

| Out of scope | Why |
| --- | --- |
| **the reference's own JIT** (the wasm code generator, `v86/src/rust/jit*.rs`) | it *is* wasm; the native lane compiles to **native x86-64** instead (`src/cpu/jit_{mem,x64,codegen}.c`) |
| wasm / async-compilation anywhere | the build target is an APE, not a browser |
| **multicore (`APIC`, SMP)** | single-CPU port; the APIC's own size and trigger are priced in `TODO.md` §1.1 |
| 64-bit mode (long mode, `EFER.LMA`, PAE) | 32-bit only, as the reference's non-JIT path |
| sound (SB16), NE2K/virtio-net, floppy, modem, 9p | device scope trimmed for the native lane |
| aarch64's JIT backend | the JIT is x86-64 only by design; aarch64 runs the uop cache (`TODO.md` §3) |

**Read-only reference.** `./v86` is a depth-1 clone of `github.com/copy/v86` and is
**read-only** for this project: not one file under it is modified, not even a scratch file.
It is the *specification*; every claim below about the reference cites a path:line in it.

---

## What is implemented

The port is **complete for its declared scope** — see `TODO.md` for what is deliberately left out
and why. In one screen:

| | |
|---|---|
| **CPU core** | the whole reference CPU: the 386-level base table, the 0F table, MMX/SSE, the x87 FPU on a vendored softfloat, the segment/GDT/LDT/TSS machinery, exceptions, interrupts, paging (PSE), the PIC. The instruction dispatch is **generated from the reference's own `v86/gen/x86_table.js`** — never hand-written — so an opcode's semantics cannot be re-decided by mistake. Census: **0 unimplemented call sites** |
| **Execution** | a uop cache (decoded blocks, physically keyed) **and an x86-64 JIT**, both gated against the interpreter; the JIT is ON by default at **~1.7× over the uop cache** and 91% dynamic inlined coverage. `--uop-cache off` and `--jit off` each reproduce the pre-existing machine exactly |
| **Devices** | port I/O and the mmap device hooks, bus, PIC, PIT, RTC, DMA, 8042 PS/2, ACPI, PCI, 8250 UART, IDE/ATA, virtio + virtio-console |
| **Boot** | a Linux `bzImage` + initrd + cmdline; a raw multiboot/ELF path (the oracle's); and an embedded 64 KiB purpose-built firmware (`tests/minibios.asm`), needed because the reference cannot boot a bzImage without a BIOS |
| **Proved by** | a real Alpine i386 kernel booting to its own panic on both machines, the guest's own `libata` reading our ATA (its md5 of the disk equals the host's), its `virtio_console` driver attaching `hvc0`, **and the baliveau guest reaching `fx-init: boot-ok v3`** |

## Why the oracle came first

A faithful port is unverifiable without a reference implementation. So: feed the *same* boot image
to both the reference (JavaScript v86) and the C port, dump a *canonical* machine-state transcript
from each, and `diff`. **Every unit since has been gated by that diff.** It is the reason the port
can claim anything at all — and the reason a JIT whose codegen was silently wrong for high-byte
registers was caught (see `TODO.md` §4).

---

## Layout

```
src/cpu/     the CPU: state, flags, memory/TLB/paging, modrm, arith, string, the base and 0F
             tables, MMX/SSE, softfloat + x87, exceptions, pic, the run loop, the uop cache and
             the JIT (jit_mem, jit_x64, jit_codegen).  interpreter_gen.c and uop_gen.c are
             GENERATED — do not edit them
src/hw/      the machine: io (port map + device hooks), bus, pit, rtc, dma, ps2, acpi, pci, uart,
             ide, virtio, virtio_console, machine (the device set + the timer fan-out)
src/main.c   the CLI, the loaders (multiboot/ELF and bzImage), and the two output documents
gen/         the emitters: interpreter_c.js and interpreter_uop.js read v86/gen/x86_table.js and
             emit the two dispatch tables; unity_c.py builds the shipped translation unit
oracle/      the reference harness (node): run.mjs (the transcript), run-boot.mjs (the kernel boot)
tests/       the images, the fixtures, the differential runner and every gate
results/     every unit's report and its raw evidence — 50+ documents, the project's record
TODO.md      what is left, with the evidence for each decision
```

## Build and run

```sh
# ONCE, in a fresh clone -- two fetched dependencies, neither vendored:
git clone --depth 1 https://github.com/copy/v86.git v86   # the read-only reference:
                                                          # gen/x86_table.js (the emitters
                                                          # import it) and the nasm corpus
cd oracle && npm ci && cd ..                              # the pinned reference (v86 0.5.471)
make                                                      # -> build/v86c, an APE, x86-64 + aarch64
```

```sh
# the ORACLE CONTRACT: the C transcript must equal the reference's, byte for byte
make oracle                    # build the test image, run both legs, check it, exit 0
make transcript-diff           # the same comparison, side by side
make determinism               # two consecutive oracle runs must be byte-identical

# the CPU gates
make selftest                  # six suites: the foundation, M1c, the devices, the uop cache,
                               # the executors, and the JIT codegen (~8M checks)
make nasm                      # the differential corpus: 118 upstream asm images, native
                               # fixtures (built without nasm or gdb) vs the C leg

# the machine
make bench                     # the 200M-instruction kernel-boot benchmark, with its pins
./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0"
sh tests/gate-m2-5-guest.sh    # the baliveau guest: kernel + initrd + a 64 MiB ATA disk
sh tests/gate-m2-5-determinism.sh   # the same boot twice: serial, virtio AND the instruction
                                    # count must be byte-identical
```

Flags worth knowing: `--hda FILE`, `--initrd FILE`, `--memory MiB`, `--serial FILE`,
`--virtio-console FILE`, `--uop-cache on|off`, `--jit on|off` (default ON), and `--uop-cache-stats`
/ `--jit-stats` (both print to **stderr**, so neither can contaminate the transcript or the guest's
serial sink).

`CC ?= cosmocc`: `make` uses `/home/jaye/.local/cosmo/bin/cosmocc` if present, otherwise
`cosmocc` from `PATH`; an explicit `CC=...` on the command line or in the environment always
wins.

---

## The ORACLE CONTRACT

Everything below is the contract. **A later C unit is correct when its transcript is
byte-identical to the reference's** for the same image and the same limit.

### 1. Command surface

| Side | Invocation |
| --- | --- |
| reference | `node oracle/run.mjs <image> <instruction-limit>` → canonical transcript on stdout |
| C port | `build/v86c <image> <instruction-limit>` → the same canonical transcript on stdout |

Exit status: `0` iff a transcript was produced. A guest CPU exception, or a guest that does
not halt within the limit, is a **nonzero exit and no transcript** — never a partial one.

### 2. Machine configuration (fixed)

MEASURED from `oracle/run.mjs`:

* `memory_size` = 2 MiB (as in the reference's own harness, `v86/tests/nasm/run.js:309`);
* `disable_jit = true` — interpreter only, matching the port;
* `autostart = false`; `log_level = 0`;
* **no BIOS** (`bios`/`vga_bios` unset) — deliberate, see §4;
* no network device (`net_device.type = null`), no cdrom, no floppy, no 9p, no virtio;
* serial: the reference's UART objects exist but nothing is attached to them and no serial
  byte enters the transcript ("serial off" = never read);
* VGA: `VGAScreen` is constructed unconditionally by the reference (`v86/src/cpu.js:1193`)
  and cannot be switched off. It is **inert** here — no listeners, no output, no screen read,
  and its memory (0xA0000+) is outside the hashed window. INTERPRETATION: this is the
  closest achievable "no VGA" without patching the reference, which this project never does.

### 3. The image

`tests/test.asm` (GNU as, `.intel_syntax noprefix`), linked by `tests/test.ld` at `0x100000`
and flattened with `objcopy -O binary` (`as --32` / `ld -m elf_i386`, no nasm on this host):

* **offset 0: a 32-byte multiboot header with the address table** (flags `0x10000`):
  magic `0x1BADB002`, checksum, `header_addr = load_addr = 0x100000`, `load_end_addr = 0`
  (= the whole file), `bss_end_addr = 0x101000`, `entry_addr = 0x100020`;
* offset 32 (`0x100020`): `_start` — real deterministic 386-level code, not a NOP sled:
  a counted sum loop, `sub/add/and/or/xor/not/neg`, `shl/ror/sar/shr/rol`, a 16-bit and an
  8-bit pass with a segment override (`es:`), `movzx`/`movsx`, `lea` with base+index*4,
  an LCG table fill, a checksum loop over that table, and a push/pop pass. Every result is
  written into the hashed 4 KiB window. No interrupts, no I/O ports, no paging (M0).
* **Geometry.** Code at `0x100000`; the hashed window and the stack live in
  `[0x101000, 0x102000)` with `STACK_TOP = 0x102000`. The brief asked for the geometry of
  the reference's nasm tests, which is `BSS = 0x100000`, `STACK_TOP = 0x102000`
  (`v86/tests/nasm/run.js:38-39`) — but there the code is linked at `0x80000` and BSS at
  `0x100000` (`v86/tests/nasm/create_tests.js:131`), so the hashed 8 KiB is data+stack. With
  the code at `0x100000` as the brief requires, the code *is* the thing at `0x100000`, so the
  data window is shifted by `0x1000` — the size of the code — to `[0x101000, 0x102000)`.
  **The hashed window is `0x101000 .. 0x102000` (4 KiB); it is the image's own `.bss` base.**

### 4. The boot path — and the one real divergence from the brief

The brief proposed a BIOS plus `hda` pointing at the image. That is **not usable**: the test
image is a flat program linked at `0x100000`, not a bootable disk (an MBR would need a
`0x55AA` signature and a real-mode boot loader), and a BIOS would be a second implementation
to port before the CPU core is even testable. MEASURED alternatives in the reference:

* `CPU.prototype.load_multiboot` **requires** `bios.main == null` (`v86/src/cpu.js:1291-1304`
  `dbg_assert(false, "load_multiboot not supported with BIOS")`), and is exactly what the
  reference's own CPU tests use (`v86/tests/nasm/run.js:261`).

So the oracle loads the image through **the reference's own multiboot loader** — the machine
bring-up is the reference's code, not a re-implementation. The flat-image equivalence the C
side must reproduce is a verbatim copy plus a fixed state, MEASURED from
`v86/src/cpu.js:1306-1570`:

| State | Value | Source |
| --- | --- | --- |
| guest RAM | the image's bytes, verbatim, at `0x100000` | `file_start = offset - (header_addr - load_addr) = 0` for a header at offset 0 with `header_addr == load_addr`, `length = undefined` ⇒ the whole buffer; `cpu.write_blob(blob, load_addr)` |
| `CR0` | `1` (PE set; PG clear) | `cpu.cr[0] = 1` |
| `protected_mode` | true | `cpu.protected_mode[0] = +true` |
| segments 0..5 | null=0, base=0, limit=`0xFFFFFFFF`; `CS` selector `0x08`, all others `0x10` | the `for(var i = 0; i < 6; i++)` block |
| `is_32`, `stack_size_32` | true, true | same block |
| `EFLAGS` | `FLAGS_DEFAULT` = `0x2` | `cpu.flags[0] = FLAGS_DEFAULT` |
| `EIP` | `get_seg_cs() + 0x100020` = `0x100020` | `cpu.instruction_pointer[0] = cpu.get_seg_cs() + entrypoint` |
| `EAX` | `0x2BADB002` (multiboot magic) | `v86/src/cpu.js:1302` |
| `EBX` | `0x7C00` (multiboot info struct) | `cpu.reg32[REG_EBX] = multiboot_info_addr` |
| `ESP` | whatever `reset_cpu` left (`0x100`) | `reset_cpu` sets `ESP = 0x100` |

`EAX`/`EBX`/`ESP` never reach the transcript: `_start` zeroes every GPR (`cld; mov esp, …;
xor`×7) before anything is measured, and the multiboot info struct at `0x7C00` is far outside
the hashed window. MEASURED: the transcript equals `tests/model.py`, which starts from
all-zero registers. Two runs on this host are byte-identical (`make determinism`).

### 5. Stop condition

**The guest's `HLT`**, with the instruction limit as a hard upper bound — *not* wall-clock.

The brief asked for an exact instruction count. That is **not expressible** through the
reference's public API: `CPU.prototype.main_loop` runs the interpreter in chunks of
`LOOP_COUNTER = 100_003` instructions and only returns at a chunk boundary
(`v86/src/rust/cpu/cpu.rs:293,3398-3411,3418-3423`); there is no single-step export. So the
reference stops at `HLT` (up to a chunk early or late), reports the instruction count it
actually reached, and reports the limit it was given. The C port must stop at `HLT` too; its
count is then exact by construction and diffable. INTERPRETATION: for M0's images the whole
program fits in one chunk, so the reference's chunking does not bias this transcript at all.

### 6. The transcript

Canonical JSON on stdout: **keys sorted at every level**, 2-space indent, one trailing
newline. Rules: every machine-state word is **lowercase `0x`-prefixed hex**, zero-padded to
its width (8 digits for 32-bit, 4 for 16-bit, 2 for 8-bit); `sha256` is 64 lowercase hex
digits; the two counts are decimal; **no host-specific field** (no timestamp, filename, path,
host name, pid, or locale-dependent formatting).

Fields (see `results/transcript.txt` for a full example):

| Field | Meaning |
| --- | --- |
| `cpu.gpr.{eax,ebx,ecx,edx,esi,edi,ebp,esp}` | the 8 GPRs, `v86/src/const.js:87-94` order |
| `cpu.eip`, `cpu.eflags` | `instruction_pointer[0]`; `get_eflags()`, i.e. the lazy flag cache materialized (`v86/src/rust/cpu/cpu.rs:1911`) |
| `cpu.control_registers.{cr0,cr2,cr3}` | `cpu.cr[0|2|3]` |
| `cpu.segments.{es,cs,ss,ds,fs,gs}.{selector,base,limit}` | `sreg` / `segment_offsets` / `segment_limits`, index order `v86/src/const.js:96-101` |
| `cpu.x87.{control_word,status_word,tag_word,stack_ptr,stack_empty}` | `fpu_control_word[0]`, `fpu_load_status_word()`, `fpu_load_tag_word()`, `fpu_stack_ptr[0]`, `fpu_stack_empty[0]` |
| `cpu.x87.data[8]` | the 8 x87 data registers, each as its 16-digit IEEE-754 bit pattern (`fpu_get_sti_f64`) |
| `memory_window.{address,bytes,sha256}` | `sha256` and length of the fixed 4 KiB window `[0x101000, 0x102000)` of guest RAM, read from `cpu.mem8` |
| `image.{bytes,sha256,load_address,entry_address}` | image length and digest, and the fixed load/entry contract — provenance, not a path |
| `instruction_count` | instructions reached at the stop (`instruction_counter[0]`) |
| `instruction_limit` | the limit the run was given |

The brief's phrase "the 8 x87 status/control/tag words" is read as the x87 status, control and tag
words *plus* the 8-register x87 file (`cpu.x87.data`) and the stack pointer/empty flag, which is
every x87 piece the reference exposes to JS. MEASURED at M0's image: `control_word=0x037f`,
`status_word=0x0000`, `tag_word=0xffff` (the reset state), all 8 data registers zero — the test
program issues no x87 instruction; a later unit that does will need these fields to move.

### 7. The gate

Three assertions, all mechanical (`make oracle`, `make determinism`):

1. the reference produces a transcript and exits 0;
2. two consecutive runs on one host are **byte-identical**;
3. the transcript equals `tests/model.py` — an independent model of the test program's
   arithmetic — on all 8 GPRs, EFLAGS, EIP, the executed-instruction count, and the sha256 of
   the 4 KiB window. A transcript that is all zeros fails here.

**Honest scope of gate 3** (MEASURED): the model shares the program's *constants* with
`tests/test.asm` — a constant mistranscribed into both would cancel out — but implements the
x86 semantics independently, so it does test the transcript against the program's arithmetic,
flags, memory effects and instruction count. It is not a proof of x86 semantics.

---

## Claim marking

Facts stated above are marked inline as **MEASURED** (read from the source, or observed as a
command's output), **INTERPRETATION** (an inference from those measurements) or
**PROJECTION** (expected, not yet observed). The project's record is `results/` — one report per
unit, each with its raw evidence and its claims marked the same way; `TODO.md` is the standing list
of what is left, with the evidence behind each decision. Every number in this README is
MEASURED on this host unless it says otherwise, and the portability claims in §3 of `TODO.md` are
the ones to read before running the APE anywhere other than Linux/x86-64.
