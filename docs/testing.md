# Testing, and the rules the gates were earned by

This project's method is the reason it can claim anything: **a port is unverifiable without a
reference, so the reference is instrumented first and every unit is gated against it.** What follows
is what the instruments are, how to run them, and — the part that matters more — which rule each one
cost to learn.

---

## 1. The gates, and how to run them

```sh
make                 # build the APE (x86-64 + aarch64) with cosmocc
make oracle          # the M0 contract: build the test image, run both legs, compare, exit 0
make transcript-diff # the same comparison, side by side
make determinism     # two consecutive oracle runs must be byte-identical
make selftest        # seven suites (~7.9M checks): the foundation, M1c, the devices, the uop cache,
                     # the executors, the JIT, and the JIT codegen
make nasm            # the 118-image differential corpus, C leg vs native fixtures
make bench           # the 200M-instruction kernel-boot benchmark, with its pins
```

and the end-to-end gates, which need `make minibios-mem` first (they use `build/minibios-80m.bin`,
which plain `make` does not produce — a missing prerequisite, not a failure):

```sh
sh tests/gate-m2-5-guest.sh         # the baliveau guest: kernel + initrd + a 64 MiB ATA disk
REBOOT_KERNEL_ARM=1 sh tests/gate-m2-5-guest.sh   # plus a real guest rebooting itself
sh tests/gate-m2-5-determinism.sh   # the same boot twice: serial, virtio AND the instruction count
sh tests/gate-reboot-1.sh           # the reboot path, both trigger sites, the properties
```

**A gate is its exit code, never the tail of its stdout.** Several gates print intentionally
error-looking fixture lines; only the status decides.

**The ACPI lane has no gate script of its own, and that is a fact to know before looking for one:**
its gates ARE the guest gates above, run twice — once with `--acpi` and once without — plus the
flag-off `make transcript-diff` (the proof a new device is *additive*, and the first gate of every
unit in the lane) and `acpi_tables_verify()` inside the `hw_selftest` suite, which re-derives the
DSDT's structure instead of trusting byte values. The exact commands, the expected kernel lines and
the pinned instruction count are in `results/acpi-7.md` §8. `tests/mk-initramfs-acpi.sh` builds the
smallest probe initramfs (`tests/images/guest-initramfs-acpi.img` — an init that prints
`/proc/interrupts` and the kernel's ACPI lines, and prints nothing host-derived); the later virtio
and reboot arms reuse the tree's `guest-initramfs-virtio.img` and `guest-initramfs-reboot.img`.

## 2. The instruments

### 2.1 The M0 oracle contract (`oracle/run.mjs` ↔ `build/v86c`)

The same image into both machines, a **canonical machine-state transcript** out of each, `diff`. The
transcript is the live CPU state, the segment/control registers and a hashed 4 KiB window of guest
RAM. It is the port's spine: the C transcript is **byte-identical** to the pinned JS reference's
(`sha256 8bb8eca5…cd61`, MEASURED). The exact contract — command surface, fixed machine
configuration, the image's geometry and the boot path — is `README` §ORACLE CONTRACT.

### 2.2 The nasm differential corpus (`make nasm`)

The reference's own instruction tests (`v86/tests/nasm/*.asm`, 123 files) are run **natively** on
this host and compared against the emulator's state. Upstream does that with `nasm` and `gdb` —
**neither exists on this host**, so the port replaced both:

- `tests/nasm2gas.py` — the NASM→GNU-as translator (all 123 files translate, assemble and link at
  upstream's exact addresses);
- `tests/mkfixture.c` — a **ptrace harness replacing gdb**: it runs the ELF, breaks at the test's
  end, and emits the same fixture document upstream's `gdb-extract-def` does;
- `tests/nasm_run.py` — the differential runner, with upstream's own comparison logic transcribed
  and a per-test skip list, where **every skip carries its measured reason** (`tests/nasm_skip_c.txt`).

Result: **118/118** (MEASURED), in every engine configuration.

### 2.3 The selftest suites (`make selftest`)

Six suites, ~8M checks. The large count is the uop cache's **decode checker**: it walks byte streams
and asserts, per instruction, that the decoder's eip advance equals the interpreter's own, that the
extracted addressing fields equal `resolve_modrm16/32`'s outputs, and that the block framing is what
the plan specifies. The JIT has its own matrix (one differential case per recipe against the C path)
plus an **elision battery** for the checks that were optimised away.

### 2.4 The independent-consumer gates

The strongest evidence in the project is not its own tests but **other implementations of the
drivers**:

- **ATA**: a real i386 Linux kernel's own `libata` discovers the disk, and the guest's `md5sum` of
  `/dev/sda`'s first 64 sectors equals the host's md5 of the image file (MEASURED).
- **virtio-console**: the kernel's `virtio-pci` + `virtio_console` attach our device and `hvc0`
  becomes the console (MEASURED).
- **The baliveau guest**: the same artifacts on the C port and the JS reference, reaching
  `fx-init: boot-ok v3` (MEASURED).
- **The reboot path**: a real guest reboots **itself** (its sysrq restart takes the keyboard-controller
  path) and boot 2 reads boot 1's marker back through libata (MEASURED).

### 2.5 The determinism gate

Two boots of the same image and cmdline, compared **byte for byte**: serial, virtio *and the
instruction count*. It is what makes the JIT's optimisations checkable — the counter staircase is
guest-observable, so an engine that batches differently moves the guest's clock.

### 2.6 The debugging toolkit (`src/cpu/debug.c`, all DEFAULT OFF)

Four tools behind one env-var family, for the investigations that keep needing them.  **They are off
unless asked for**: with no variable set the hooks are one load and one branch each, and
`tests/perf_bench.sh` still reports its pinned `instr=200095214` with the code compiled in.

| flag | what it does |
|---|---|
| `V86C_WATCH=<addr>:<len>[,<addr>:<len>…]` | log every write to a watched address, with the rip, width and value.  Watches **virtual** addresses (`safe_write*`), **faulting** accesses (`trigger_pagefault` — a write that faults never reaches a store, which is why it needs its own hook) and **physical** addresses |
| `V86C_RING=<N>` | keep the last N instruction boundaries (eip + the eight bytes at the fetch) and dump them on any stop |
| `V86C_DUMP_ON_STOP=1` | on a stop: registers, cr0–cr4, cr2, cr3, efer, idtr/gdtr, cpl, the segment bases |
| `V86C_CHAIN=<va>` | with the above, the PML4E/PDPTE/PDE/PTE chain of a virtual address, with each entry's physical address and value |

Output goes to **stderr** with the prefix `v86c dbg:` and is **unbuffered** (`v86c_dbg_init` calls
`setvbuf(stderr, NULL, _IONBF, 0)`, and `v86c_dbg_on_stop` flushes), so a crash cannot eat it — this
lane lost probe output to block buffering twice before the toolkit existed.  `m1c_selftest`'s
`test_debug_toolkit_fires` proves each tool fires (it checks the tool's own counter, not just that the
call returned).

Example — the question that built the toolkit (the amd64 kernel's first #PF):

    V86C_WATCH=0x31558:1 V86C_RING=16 V86C_DUMP_ON_STOP=1 \
      ./build/v86c --bzimage tests/images/vmlinuz-6.1.0-50-amd64 4475481000 --memory 256 \
                   --cmdline 'console=ttyS0' --long-mode on

## 3. The rules, each of which cost something real

- **Measure the instrument before the phenomenon.** A `2>&1` capture made `--jit off` vs `--jit on`
  look divergent; the banner goes to stderr and stdout was identical. Three `getenv` debug hooks in a
  hot loop were worth **30%** and *masked the JIT's own gain* — the unit that ran with them measured
  its own scaffolding.
- **A test that cannot fail is not a test.** The oracle's MMX comparison could never fire (a signed
  fixture word against an unsigned `0xAAAA`), so MMX registers were compared by *nothing*; the JIT's
  high-byte blocker **shipped through every gate green** (118/118, byte-identical transcript, kernel
  boot, whole selftest suite) and was caught only by a review plus a hand-built probe. Ask what makes
  a gate FAIL, and add it.
- **Prove a fix by asymmetry.** Fail before, pass after — and build the pre-fix artifact if you need
  it. A fix without a demonstrated flip is not closed.
- **Never `-DNDEBUG`.** Assertions are load-bearing stops. A *guest-reachable* assert is resolved at
  its site, matching what the release reference does — three separate units learned this.
- **A reviewer's prescription is a hypothesis.** Seven were refuted by measurement in this project;
  the most recent dissolved a plan's own expectation formula and a build-change premise. Measure
  before adopting, and record the refutation.
- **The mechanism can be right while the thing that reads it is wrong.** Four instances: the
  delivered-fault flag, the dead MMX oracle branch, a comment claiming a mechanism that never ran
  (guarding an invariant nothing enforced), and an argument-order bug that wrote to the wrong address
  with every other compared field matching.
- **Mark every claim MEASURED / READ / PROJECTION**, and stop where the evidence stops. The record in
  `results/` is written that way on purpose; this file is too.
