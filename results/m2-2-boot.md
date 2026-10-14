# M2-2 — the 8250 UART, the Linux bzImage loader, and A REAL i386 KERNEL BOOTING

**Unit:** `HANDOFF=v86c`, unit M2-2 (coder). **Deliverables:** `src/hw/uart.{c,h}` (the port of
`v86/src/uart.js`), `src/hw/kernel.{c,h}` (the port of `v86/src/kernel.js`), the boot mode in
`src/main.c`, the boot firmware `tests/minibios.asm`, and the reference leg
`oracle/run-boot.mjs`.

**Bottom line.** A **real Alpine i386 Linux kernel (6.6.134-lts, 7,729,664 bytes, protocol
2.15) boots on BOTH machines** — the pinned reference (npm v86 0.5.471+g94faf3d) and
`build/v86c` — from the same kernel bytes, through each side's own port of `load_kernel`,
on the same 64 MiB machine, running the same 16-bit firmware. Both reach

```
[    x] No filesystem could mount root, tried:
[    x] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)
[    x] CPU: 0 PID: 1 Comm: swapper/0 Not tainted 6.6.134-0-lts #1-Alpine
[    x] Call Trace: ... entry_INT80_32+0xf0/0xf5
[    x] Kernel Offset: disabled
[    x] ---[ end Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0) ]---
```

with the machine in the same final state (`eip=0xc1927800`, `cs=0x60`, `cr0=0x80050033`,
`cr3=0x1ea1000`, paging on, cpl 0). The two serial transcripts agree on **279 of the
reference's 297 lines byte for byte** (and 279 of the port's 287) after the kernel's own
timestamps are stripped; every
remaining line is accounted for in §5 with its cause. All gates green: `make` (from
scratch), `make selftest` (3522 + 228 + 338 checks, 0 failures), `make nasm` (118/118),
`make transcript-diff` (EMPTY), `make oracle`, `make determinism`, and the asymmetry rig
(6 mutations, each failing then restored-green). Raw evidence: `results/m2-2-gates.txt`,
`results/m2-2-mut.txt`, `results/m2-2-js-serial.txt`, `results/m2-2-c-serial.txt`,
`results/m2-2-diff.txt`, `results/m2-2-diff-normalized.txt`.

**Two things in this unit were REAL BLOCKERS found by the boot itself**, both in code earlier
units had shipped and both recorded as corrections to those units' own reports:

1. **`rdrand` was not implemented** (`src/cpu/instructions_0f.c`): the register form was
   `#UD` where upstream's is a real `rdrand`, so the kernel's `random_init_early` took an
   invalid-opcode oops in its first seconds and **no console ever came up**. With the fix,
   the whole boot follows. (`results/m1b1-0f-nonsse.md`'s divergence table row for `0F C7 /6`
   is corrected inline; so is `src/cpu/m1c_selftest.c`'s wrong expectation.)
2. **The PCI device index was unbounded** and **a 32-bit write to an absent device stopped
   the machine** (`src/hw/pci.c`): the kernel's bus-255 scan read 260 KiB past a 256-entry
   table and **SIGSEGVed the port** at instruction 550020331 of its boot, and the kernel's
   write to an absent device (bdf 0xf9) killed the boot at 659158271. Both are fixed against
   upstream's own code, and M2-1's claim about write32
   (`results/m2-1-machine.md`, `src/hw/pci.h`) is corrected inline.

---

## 1. Files, and the JS → C mapping

| C file | lines | upstream | lines | what it is |
| --- | --- | --- | --- | --- |
| `src/hw/uart.c` / `.h` | 549 / 87 | `v86/src/uart.js` | 455 | the 8250/16550: the register file, DLAB, the FIFO, the line status, the modem status with its delta bits, the four interrupt sources and the IRQ line, and both ends of the host seam |
| `src/hw/kernel.c` / `.h` | 288 / 67 | `v86/src/kernel.js` | 234 | `load_kernel` (the boot protocol: setup sectors, boot params, `loaded_high`, cmdline, initrd, the protected-mode entry) and `make_linux_boot_rom` (the option ROM) |
| `src/main.c` | +311 (696 → 1007) | — | — | **the boot mode**: `--bzimage <kernel> <limit> [--cmdline …] [--initrd …] [--bios …] [--serial …] [--uart1]`, the 64 MiB machine, the firmware and option ROM placement, the serial sink, the host-fault report |
| `tests/minibios.asm` | 715 | — | — | **the boot firmware both machines run** (§3): a 64 KiB 16-bit real-mode shim — IVT, BDA, the option-ROM scan, and the INT 10h/11h/12h/13h/15h/16h/19h/1Ah services the kernel's real-mode setup calls |
| `oracle/run-boot.mjs` | 232 | — | — | the reference leg: the V86 starter with `bzimage`/`bios`/`cmdline`, the option ROM copied to 0xC0000, and the guest's `serial0-output-byte` bytes on stdout |
| `gen/minibios_bin.py` | 44 | — | — | carries `build/minibios.bin` into the APE as a C array (`make` regenerates it, so it cannot be stale) |

Files **modified** (each with its reason):

| file | change |
| --- | --- |
| `src/cpu/instructions_0f.c` | **`0F C7 /6`'s register form is upstream's `rdrand`** (it was `#UD`); the memory form is upstream's `#UD` (it already was). **This is a `src/cpu/` change and the brief forbids `src/cpu/` unless "a real blocker forces it" — it is one, it is measured in §4/M1, and it is stated here loudly.** |
| `src/hw/pci.c` / `pci.h` | the absent-device bound check (`pci_bdf_is_absent`), and a 32-bit write to an absent device is now IGNORED as upstream's is (it stopped the machine before). The header's claim is corrected, not rewritten. |
| `src/hw/machine.c` / `.h` | the UART joins the device set, in `cpu.js`'s own order (after the PS/2, before the PIT); the "not in this machine" lists lose the UART and gain the COM2-COM4 note |
| `src/cpu/m1c_selftest.c` | the `0F C7 /6` group asserted the wrong behaviour (both forms `#UD`); it now asserts upstream's (rdrand + flags, then the memory form's trap) — 222 → 228 checks |
| `src/hw/hw_selftest.c` | the absent-device write case is corrected (write32 ignored, write8/16 stop) + the bus-255 case + **two new groups**: the UART (32 checks) and the bzImage loader (72 checks) — 225 → 338 checks |
| `Makefile` | the firmware rules (`tests/minibios.asm` → `build/minibios.bin` → `build/minibios_embed.c`), `HW_SRC`/`HW_HDR` + the embedded firmware in the link, a `minibios` target, and one trap documented: **the `MINIBIOS_*` variables must be defined BEFORE the `$(BUILD)/v86c` rule** (a prerequisite list is expanded when make *reads* the rule, a recipe when it *runs* one — the first version built "successfully" with the file on the command line and no rule to produce it) |
| `results/m1b1-0f-nonsse.md`, `results/m2-1-machine.md` | **corrections to two earlier units' records**, inline at the claims they correct (the `0F C7 /6` table row, the "a write to an absent PCI device stops" claim) |

`v86/` was read only. `tests/test.img` and the transcript are untouched
(`make transcript-diff` is still EMPTY).

---

## 2. The gate: a real kernel, in both machines

| item | value |
| --- | --- |
| image | `tests/images/vmlinuz-i386` — `curl -o tests/images/vmlinuz-i386 https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86/netboot/vmlinuz-lts` |
| size | **7,729,664 bytes** |
| sha256 | **ce1f6dad86646efaa980fed238c7ed7e4189fd90c30f99a3788df9c9429cc5c9** |
| `file` | Linux kernel x86 boot executable, bzImage, version **6.6.134-0-lts**, protocol **2.15**, setup size 512\*31, syssize 0x75b20, jump 0x26c, relocatable, max cmdline 2047, **init_size 0x11b6000** |
| header, MEASURED | `setup_sects=31`, `syssize=0x75B200`, `boot_flag=0xAA55`, `HdrS`, `protocol=0x20f`, `loadflags=0x01` (LOADED_HIGH), `realmode_swtch=0`, `code32_start=0x100000`, `heap_end_ptr=0x5500`, `cmdline_size=0x7ff`, `kernel_alignment=0x1000000`, `hardware_subarch=0`, `payload_offset=0x94`, `payload_length=0x74e91f`, `pref_address=0x1000000`, `init_size=0x11b6000` |
| machine, BOTH | 64 MiB, no ACPI (`acpi_enabled` false → CPUID advertises no APIC), no initrd, no VGA/IDE in the port, `console=ttyS0` |
| limit, BOTH | **1,500,000,000 instructions** |
| commands | `./build/v86c --bzimage tests/images/vmlinuz-i386 1500000000 --cmdline "console=ttyS0"` and `node oracle/run-boot.mjs tests/images/vmlinuz-i386 build/minibios.bin 1500000000 "console=ttyS0"` |
| result | both exit 0; both reach the kernel's own panic; both end at `eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000` (MEASURED: the port at 1,500,008,797 instructions / 177.8 s of its own guest time, the reference at 1,500,011,042 / 8.38 s — see `results/m2-2-{c,js}-run.txt`) |

**The two transcripts** (full: `results/m2-2-js-serial.txt` 297 lines / 17,900 bytes, and
`results/m2-2-c-serial.txt` 287 lines / 17,178 bytes; raw diff `results/m2-2-diff.txt`, 556
lines because every timestamp differs; timestamp-stripped diff
`results/m2-2-diff-normalized.txt`, **28 lines**):

```
   reference (first 12)                                  port (first 12)
   minibios: option ROM at c000                          minibios: option ROM at c000
   [    0.000000] Linux version 6.6.134-0-lts (buildozer@build-3-20-x86) (gcc (Alpine 13.2.1_git20240309) 13.2.1 20240309, GNU ld (GNU Binutils) 2.42) #1-Alpine SMP PREEMPT_DYNAMIC 2026-04-11 17:03:51
   [    0.000000] BIOS-provided physical RAM map:         (identical)
   [    0.000000] BIOS-e820: [mem 0x0000000000000000-0x000000000009fbff] usable     (identical)
   [    0.000000] BIOS-e820: [mem 0x000000000009fc00-0x000000000009ffff] reserved (identical)
   [    0.000000] BIOS-e820: [mem 0x00000000000f0000-0x00000000000fffff] reserved (identical)
   [    0.000000] BIOS-e820: [mem 0x0000000000100000-0x0000000003ffffff] usable     (identical)
   [    0.000000] Notice: NX (Execute Disable) protection cannot be enabled: non-PAE kernel! (identical)
   [    0.000000] APIC: Static calls initialized            (identical)
   [    0.000000] DMI not present or invalid.               (identical)
   [    0.000000] vmware: hypercall mode: 0x00              (identical)
   ...
   (last 8 of the port; the reference's differ only in the timestamps)
   [  183.05...]  dump_stack+0xd/0x20
   [  183.05...]  panic+0x25f/0x290
   [  183.05...]  mount_root_generic+0x2da/0x330
   [  183.05...]  mount_root+0x209/0x2f0
   [  183.05...]  prepare_namespace+0x83/0x390
   [  183.05...]  kernel_init_freeable+0x3c5/0x450
   [  183.05...]  kernel_init+0x17/0x1a0
   [  183.05...]  ret_from_fork+0x38/0x60
   [  183.05...]  ---[ end Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0) ]---
```

The port's own instruction count to the panic is **below the reference's** for the same boot:
it is inside the kernel's console/log output by 500 M instructions (112 lines) and reaches the
panic before 800 M, where the reference (whose clock is the host wall clock, so its guest time
advances only as fast as this host can interpret) needs about 1 G. That is a *consequence of
the clock policy* (§5 D4), not of any device difference: at 100 M instructions the two
machines are **instruction-for-instruction in the same place** — both at `eip=0x219735f`,
`cs=0x10`, `cr0=0x60000011`, `cr3=0`, i.e. inside the decompressor's inflate loop.

---

## 3. What the firmware is, and why the reference needs one

**v86 cannot boot a bzImage on its own, and the brief's premise that it can is the one thing
this unit had to correct on contact with the code.** `load_kernel` writes the kernel image,
the boot params and the command line into guest memory and returns an **option ROM** whose own
header says

```
// This rom will be executed by seabios after its initialisation     kernel.js:183
```

`cpu.js:1024-1035` only pushes it onto `this.option_roms`, from where **seabios** fetches it
through the fw_cfg port (`cpu.js:1127-1160`) and executes it. So the reference's bzImage path
has always needed a BIOS; nobody notices because the browser lane ships one.

**Seabios is not an option here** (it needs the VGA and the IDE controller this port has not
ported, and the brief forbids starting them), so **both machines run the same purpose-built
16-bit firmware**, `tests/minibios.asm`, assembled by `make` into a 64 KiB image at
`0xF0000-0xFFFFF` — a real PC BIOS's own address, executed at `CS=0xF000`, with the reset
vector at `0xFFFF0`. The host does the two things seabios does with a fw_cfg option ROM: it
copies it to `0xC0000` and resets the CPU into the firmware, which scans the option-ROM area
for a valid `0xAA55` block (checksum included) and far-calls entry offset 3.

What the firmware provides, each handler written against the kernel source that calls it
(`arch/x86/boot/*.c` of 6.6, read for this unit):

| INT | provided | called by |
| --- | --- | --- |
| `10h` | `00h` set mode, `01h` cursor shape, `02h`/`03h` cursor, `0Eh` teletype (writes into the 0xB8000 window and keeps the cursor in the BDA), `0Fh` get mode, `12h` EGA/VGA config, `1Ah` display combination | `store_mode_params`, `vga_probe`, `vga_set_basic_mode`, `restore_screen`, `putchar` |
| `11h`/`12h` | the equipment word and base memory, from the BDA | `query_mca`-adjacent probing |
| `13h` | every function fails (CF=1): no disk | `query_edd`, `read_mbr` — both report "no EDD" |
| `15h` | **`E820h`** (a 64 MiB map: 0-0x9FBFF usable, 0x9FC00-0x9FFFF and 0xF0000-0xFFFFF reserved, 0x100000-0x3FFFFFF usable), `88h` extended memory, `2400h`/`2401h`/`2402h` A20, `EC00h` no-op; everything else CF=1 | `detect_memory` (**always** queries: a loader-supplied map is not consulted, `memory.c:116-123`), `set_bios_mode`, `query_ist` |
| `16h` | no key is ever pending (ZF=1), shift state 0 | `keyboard_init`, `getchar_timeout` |
| `1Ah` | `00h` tick count, `02h`/`04h` the RTC read through the CMOS | `getchar_timeout` |
| `19h` | halt | — |
| any other | **print the vector and halt**, so a missing service stops loudly instead of iret-ing into a zero IVT | — |

Two firmware facts are worth stating because they cost measurements:

* **A BIOS handler's returned flags live in the IRET frame, not in the flags register.** `int
  n` pushes the *caller's* FLAGS and `iret` pops them, so a handler's own `clc`/`stc` never
  reaches its caller: the bit has to be patched into the frame (`[bp+6]` after `push bp`).
  MEASURED consequence of getting it wrong: with CF unwritten the kernel reads back the CF=1
  that its own `initregs` sets in **every** call, which is why every "not supported" probe
  (EDD, APM, MCA) accidentally worked while `detect_memory_e820` broke on its first call and
  the kernel fell back to the int 15h/88h path — the log said `BIOS-88: [mem 0x0-0x9efff]
  usable` instead of `BIOS-e820: …`. Fixed with three helpers (`iret_cf0`/`iret_cf1`/
  `iret_zf1_cf0`), and §4/M4 is the regression probe for it.
* The E820 map is what the *machine* has: 64 MiB. It is a constant in the firmware
  (`MEM_TOP`) because both sides are configured from the same number, and it is named in the
  report rather than hidden in an arithmetic expression.

**What the firmware is NOT** (each with what a guest sees): no POST, no disk BIOS (a guest
asking one gets CF=1, i.e. "no such device"), no keyboard input (a guest waiting for a
keystroke waits forever, exactly as on the reference machine, which has no UI attached), no
scrolling (the teletype stops at row 24), no EBDA tables and no VGA — and since a teletype
write goes into the VGA window that the *reference* also only reads as zeros
(`results/m2-1-machine.md` R1F), the kernel's setup messages are visible on neither machine.
The serial console is where the kernel talks, in both.

---

## 4. G3 and G4: the selftests and the asymmetry rig

**G3 — `src/hw/hw_selftest.c` gains two groups (225 → 338 checks, 0 failures, 0 skipped),
run by the same `make selftest`.**

```
PASS uart: a byte out of the sink, the line status, the divisor, the seam (266 checks so far)
PASS kernel: the bzImage boot params, the option ROM, the real image (338 checks so far)
hw_selftest: 338 checks, 0 failures, 0 skipped
```

* The UART group (32 checks) drives **the CPU's own instruction bodies** (`out dx, al` /
  `in al, dx` → `instr_E6`/`instr_E4` → `io_port_write8` → the port map → the device → the
  bus message → the host sink), so it tests the path a kernel's 8250 driver takes: a byte
  written to `0x3F8` comes out of the sink; the line status says TX empty (0x60) and clears
  DATA_READY when the FIFO is empty; the *receive* side is fed through the seam's other end
  (`serial0-input`); **DLAB** makes port+0/port+1 the divisor's two halves and the write stops
  reaching the sink; the IIR reports THRI and its read clears it; the FIFO bit appears in the
  IIR's top two bits; **loopback** returns a written byte to the device's own receiver; the
  scratch register round-trips; the modem-status **delta bits** set from the seam and clear on
  read; and **the second channel** (COM2 at 0x2F8) is a real device whose message name is
  `serial1-output-byte`.
* The loader group (72 checks) is the brief's own case, in two halves: **(a)** a synthetic
  image, where the whole geometry is asserted — the real-mode block lands at 0x80000 with the
  boot params mutated exactly as the protocol demands (`type_of_loader=0xFF`,
  `loadflags=0x81` = LOADED_HIGH|CAN_USE_HEAPS with QUIET and KEEP_SEGMENTS cleared,
  `heap_end_ptr=0xDE00`, `vid_mode=0xFFFF`, `cmd_line_ptr=0x8E000` and the command line's own
  bytes there, both ramdisk fields zero), the protected-mode part at 0x100000, the **option
  ROM's** header/prologue/checksum (its 22 prologue bytes are the reference's *measured*
  output), and the refusal arm (both header checksums, and a setup block that would not fit
  below the video area); and **(b)** against **the fetched kernel's own bytes** —
  `tests/images/vmlinuz-i386`'s header read from the file (`setup_sects=31`, `HdrS`,
  protocol `0x20F`, LOADED_HIGH, `syssize*16=0x75B200`, `init_size=0x11B6000`) versus what the
  loader makes of it, and the guest memory versus the file's bytes (the real-mode block, the
  protected-mode part from 0x4000, and the first bytes at 0x100000 being the decompressor's
  own `cld; cli`). The file is 7.7 MB and the test machine is 2 MiB, so the image is used as a
  **prefix** — named in the test, and the test prints `SKIP` (counted, at the end of the
  suite's line) if the file is absent rather than passing silently.

**G4 — `results/m2-2-mut-rig.sh` → `results/m2-2-mut.txt`: six single-line mutations, each
alone, each rebuilt, each required to fail, then restored and required green.** All six were
VISIBLE, and the before/after pair is the proof:

| # | file | the single line | MUTATED | restored |
| --- | --- | --- | --- | --- |
| M1 | `src/cpu/instructions_0f.c` | `rdrand`'s register body → `{ trigger_ud(); (void)r; }` | boot: **1 line**, no `Linux version`, no panic (the oops the unit found) | 287 lines, 4 e820 rows, panic |
| M2 | `src/hw/pci.c` | `pci_bdf_is_absent` → `(void)bdf, false` | boot: **rc=139 (SIGSEGV)**, 148 lines | rc=0, 287 lines, panic |
| M3 | `tests/minibios.asm` | the option-ROM checksum's second byte add deleted | boot: **1 line**, `option ROM` absent ("no option ROM found") | rc=0, 287 lines |
| M4 | `tests/minibios.asm` | the E820 success path's `jmp iret_cf0` → `iret` | boot: 285 lines, **0 `BIOS-e820` and 2 `BIOS-88`** (the kernel falls back) | 287/288 lines, 4 e820 rows |
| M5 | `src/hw/uart.c` | COM2's output message name → COM1's | selftest: **3 failures** (`uart_sink1_count`, `uart_sink1[0]`) | 338 checks, 0 failures |
| M6 | `src/hw/kernel.c` | `HEAP_END_PTR` → `HEAP_END - 0x1200` | selftest: **2 failures** (`heap_end_ptr == 0xDE00` got `0xce00`) | 338 checks, 0 failures |

The rig's own first version **did not rebuild after mutating** and reported "no effect" for
M1–M4 — a mutation that is never compiled measures the previous binary. That is recorded in
the rig's own comment, and the fix (a `rebuild` call after every mutation *and* after every
restore) is why the table above has real before/after pairs. Every touched file's sha256 is
printed before and after, and the restored tree's shas match the control's.

---

## 5. THE DIVERGENCE LIST (every line that differs, with its cause)

`results/m2-2-diff-normalized.txt` (timestamps stripped) is the artefact: 28 marked lines out
of the reference's 297. MEASURED counts: **279 lines of each machine's output appear verbatim
in the other** — 18 of the reference's 297 lines have no counterpart in the port and 8 of the
port's 287 have none in the reference (297-18 = 287-8 = 279), and two of the pairs are the
same line at a neighbouring position. Every difference is one of five
classes, and there is no unexplained line:

| # | lines | difference | cause (MEASURED unless marked) |
| --- | --- | --- | --- |
| D1 | 1 (line 71) | `Console: colour EGA 80x25` (reference) vs `Console: colour dummy device 80x25` (port) | **The reference's machine always has a VGA device** — `cpu.js:1193` creates a `VGAScreen` unconditionally, so the kernel's `vgacon` initialises and registers a colour console. The port's machine has no VGA (M2-1 defers it, and this unit was told not to start it), so `vgacon` fails its probe and the kernel installs `dummycon`. A guest sees: identical serial output (both consoles are registered *and* ttyS0 is), but the VGA console does not exist on the port. |
| D2 | 14 (164-174, 176-178) | the reference enumerates two PCI devices the port has not got: `pci 0000:00:12.0 [1234:1111]` (the VGA) with its three BAR/ROM lines, `pci 0000:00:1e.0 [8086:7010]` (the IDE controller) with its four legacy-quirk BAR lines, and three `vgaarb:` lines | the same device-set difference as D1, seen through PCI: upstream's `vga.js` registers a PCI device and `ide.js` registers the PIIX IDE function; this port's machine has the host bridge, the ISA bridge and (when `acpi_enabled`) ACPI. The kernel then has no `hd*` block device, which is *why* both machines end in "No filesystem could mount root" — the reference's IDE controller carries no disk image either — MEASURED: both machines' `List of all partitions:` is the same 16 `ram*` disks and nothing else (no `hd*`). |
| D3 | 4 (1 + 3) | the port prints `hrtimer: interrupt took 15500002 ns` and three `pci 0000:00:00.0/00:01.0: quirk_… took 1xxxx usecs` lines the reference does not | **the clock policy, seen from the other side** (M2-1F's D-F2): the port's `microtick()` is `instruction_counter / 1000` — 1 guest instruction = 1 µs — while the reference's is the host wall clock. The kernel's own `took N usecs` accounting therefore measures instruction counts on the port and real time on the reference; a 15,000-instruction quirk is 15 ms of guest time there and ~0.4 ms here. A guest sees interior code that looks 30x slower per instruction on the port. |
| D4 | 4 | the four clock-valued lines: `audit(1791823226.676:1)` / `audit(1791823184.883:1)`, `rtc_cmos … setting system clock to 2026-10-12T16:40:30 UTC (1791823230)` / `…T16:39:48 UTC (1791823188)`, `sched_clock: Marking stable (7748082444, 26410338)->(7779299371, -4806589)` / `(131299741476, 1103787000)->(324948949000, -192545420524)` | the **host clock**: the RTC's time registers read the host's date in *both* machines (M2-1's F3, declared there), so the two runs' stamps differ by the ~50 s between them. The `sched_clock` numbers are the port's instruction-derived TSC (1000 counts/instruction) vs the reference's wall-clock TSC. A guest sees a different wall clock and a different monotonic clock, both self-consistent. |
| D5 | 2 (moved by one position) | the two `rtc_cmos rtc_cmos:` probe lines sit *after* `input: AT Raw Set 2 keyboard …` on the port and *before* it on the reference | INTERPRETATION, flagged as such: the keyboard's `input:` line comes from the serio **kthread**'s device registration, while the RTC's comes from a platform-driver probe, so their order depends on when the scheduler runs the kthread relative to the probe loop — and D3's clock difference changes exactly that window. The same three lines are present in both; one position moves. Not a device difference: the `i8042`/`serio` lines around it are byte-identical, i.e. the PS/2 device model is exercised identically by the kernel in both machines. |

Two further, non-line divergences in the same comparison, stated because the gate is a
*comparison* and not a byte diff:

* **The instruction counts to the same point differ** (1,500,008,797 vs 1,500,011,042 at the
  limit, and the *guest uptime* at the panic is 177.8 s vs 8.38 s; the port reaches the panic
  earlier in instructions), and the port's *own* output is not byte-stable across its runs — D3's `hrtimer:` line appears at a limit-dependent moment (287 vs 288
  lines between two runs of the same binary). The gate therefore compares *line content after
  the timestamp*, not raw bytes; the raw diff (`results/m2-2-diff.txt`) is kept as the
  evidence that the *only* raw differences are the timestamps and D1–D5.
* **Both machines' final state is identical**: `eip=0xc1927800` (inside the kernel's
  interrupt-entry stubs), `cs=0x60`, `cr0=0x80050033` (PG|PE|…), `cr3=0x1ea1000`, cpl 0 —
  and so are the *NMI testsuite* results ("`remote IPI: ok`", "`local IPI:TIMEOUT`"), the
  `i8042`/`serio` PS/2 bring-up, the `8250/16550` probe, the TSC clocksource switch, the
  partition list (16 `ram*` disks and nothing else) and the whole panic call trace, byte for
  byte after the timestamp. A machine difference that broke the 8259 PIC, the PS/2, the PIT
  or the UART's contract with a real driver would show up in one of those.

---

## 6. The two blockers, in detail (they are the unit's findings)

### 6.1 `rdrand` was not implemented, and the kernel needs it early

* **What the port did**: `instr16/32_0FC7_6_reg` were `trigger_ud()` with the note "upstream
  defines only the MEMORY form".
* **What upstream does** (`v86/src/rust/cpu/instructions_0f.rs`): `instr16_0FC7_6_reg` (3996)
  and `instr32_0FC7_6_reg` (4006) are `rdrand` — the random value into the register, `*flags =
  (*flags & !FLAGS_ALL) | 1`, `*flags_changed = 0` — and `instr16/32_0FC7_6_mem` (4016-4018)
  are `trigger_ud()`. `gen/x86_table.js:462` says exactly that (`mem_ud: 1` on the entry whose
  register form is rdrand).
* **Why it mattered**: CPUID leaf 1 advertises RDRAND (`instructions_0f.c:1132`, ECX bit 30 —
  upstream's own value), so the kernel's `random_init_early` executes `rdrand ecx`. MEASURED
  with the old body: a guest oops in that function, and the boot's entire serial output is the
  firmware's one line. This unit's own selftest had *asserted* the wrong behaviour, which is
  how a wrong transcription survived a green gate.
* **The fix**: upstream's rdrand, with `/dev/urandom` as the C peer of `js::get_rand_int`
  (MEASURED: the pinned build's is `crypto.getRandomValues` / `node crypto.randomBytes(4)
  .readInt32LE(0)`), and a named deterministic fallback if `/dev/urandom` cannot be opened.
  The value is not part of any contract — upstream's is random too, no test in either skip
  list touches rdrand, and the gate's output is identical either way.

### 6.2 The PCI device index, and what a write to an absent device is

* **What the port did**: `pci->device_spaces[bdf]` with `bdf = address >> 8 & 0xFFFF` into a
  256-entry C array, and a *stop* for a write to an absent device at every width.
* **What upstream does**: the same 16-bit `bdf` into JS tables initialised for 0..255
  (`pci.js:37-38`), so anything above 255 is `undefined` — an absent device. An absent device
  is: a read answering `-1`/status `0` (`pci_query`), a **`write32` silently IGNORED**
  (`pci.js:381-390`: `var space = this.device_spaces[bdf]; if(!space) return;`), and a
  `write8`/`write16` that **throws** (`pci.js:328`, `354`: `new Uint8Array(this.device_spaces
  [bdf].buffer)` on `undefined`).
* **Why it mattered**: MEASURED, in the boot: the kernel scans PCI and writes config space on
  bus 255 (`0x80ff0000` → bdf `0xff00`) — the unbounded index read 260 KiB past the table and
  **SIGSEGVed the port at instruction 550020331** — and it writes 32-bit config space to a
  legitimately absent device (bdf `0xf9`, dev 31 fn 1) — the stop **ended the boot at
  instruction 659158271**.
* **The fix**: `pci_bdf_is_absent` (absent, exactly as upstream's `undefined` is) and
  `write32` ignored. The `write8`/`write16` stops stay: upstream throws there, so the port's
  stop is the documented stand-in for a behaviour that does not exist, and no gate reaches it.
  M2-1's report and `src/hw/pci.h` are corrected inline at the claim.

---

## 7. Divergences from upstream, deliberately, and what they cost

Beyond §5's *measured* differences between the two machines, these are the port's own
divergences from `v86/`, each stated where a reader can find it:

| file | divergence | reason / guest-visible cost |
| --- | --- | --- |
| `src/hw/uart.c` | upstream's `current_line` field is not ported (uart.js:90, 411-422) | it exists only inside `if(DEBUG)` to accumulate a line for `dbg_log`, and this port has no `dbg_log` channel. Nothing reads it, so no guest can tell. |
| `src/hw/uart.c` | `get_state`/`set_state` (uart.js:298-330) are not ported | this port has no state-save machinery at all (upstream's `state.js` is not ported either). Stated in the header, not silently dropped. |
| `src/hw/uart.c` | every `dbg_log` call is dropped, its text kept as a comment | the log channel does not exist in the port (the M1b-1 rule). |
| `src/hw/uart.c` | the bus message names are a static table indexed by channel | `bus_register` keeps the **pointer** (bus.c:43), so a name built by `snprintf` would dangle; the table's strings are byte-identical to what `"serial" + com + "-input"` produces. |
| `src/hw/uart.c` | the receive FIFO is a growable ring, not a JS array | same semantics (FIFO order, unbounded, nothing overwritten or dropped) without a JS array. |
| `src/hw/kernel.c` | the `dbg_log` block's locals (`syssize`, `vidmode`, `flags2`, `initrd_addr_max`, `kernel_alignment`, `relocatable`, `min_alignment`, `payload_offset`, `payload_length`, `pref_address`, `init_size`) are not read | they feed **only** kernel.js:101-111's log lines, and the kernel reads those fields from the boot params in guest memory, not from these locals. `cmdline_size` IS used and is kept. |
| `src/hw/kernel.c` | upstream's four `dbg_assert`s become **stops** | they are `debug_assert!`, compiled out of the pinned release (so upstream silently mis-loads a malformed image). M1c's ELF-loader convention: a structural failure that would mis-load the guest stops. |
| `src/hw/kernel.c` | a write past guest memory **drops** the out-of-range bytes and says so on stderr | JS index assignment drops them silently; JS `mem8.set(bigger, off)` *throws*. Neither is a behaviour to reproduce; the port warns on stderr (a diagnostic no guest can see) and drops. In the boot gate nothing is ever dropped (64 MiB). |
| `src/main.c` | the boot mode is **additive** (`--bzimage`), and the firmware is embedded | the M0 oracle contract's two-argument invocation is byte-for-byte unchanged (`make transcript-diff` still EMPTY). `--bios FILE` overrides the embedded copy, which is how the reference leg and the port are guaranteed the *same* firmware bytes. |
| `src/main.c` | the boot mode installs a `SIGSEGV`/`SIGBUS`/`SIGABRT` handler that prints the guest state and exits 128+sig | a host fault is a port defect, not a guest event; the report it prints (`eip`, `cs` and its base, `cr0`/`cr3`, the eight GPRs and the instruction count) is what localised §6.2. It changes no guest-visible value, and the transcript mode does not install it. |
| `src/main.c` | `V86C_BOOT_DUMP` (a diagnostic, like `V86C_LOG_PORTS`) | searches guest memory for the kernel's *rendered* log and prints it, plus the boot params as the guest sees them. It decides nothing. |
| `src/cpu/instructions_0f.c` | `rdrand` implemented with `/dev/urandom` + a deterministic fallback | §6.1. Upstream's is the platform CSPRNG. |

**Still stubbed, and what a guest observes** (the M2 horizon, restated for this unit's
machine): no VGA (§5 D1, and the VGA window/LFB still read 0), no IDE/floppy/network/virtio/
sound (so a kernel finds no disk and no root filesystem — which is exactly the reference's
outcome too, minus its empty IDE controller), no ACPI/APIC (`acpi_enabled` false → the kernel
uses the 8259 PIC; the reference is the same), COM2-COM4 exist in code but are attached only
when asked (`--uart1`), no host input path (the PS/2 is complete and bus-driven but nothing is
attached to `machine_host_bus`), and `cpu.reboot_internal` does not exist (the kernel's panic
takes the `native_halt` path in both machines, so this was not reached).

---

## 8. Gate evidence (G2), raw in `results/m2-2-gates.txt`

| gate | result |
| --- | --- |
| `make clean && make` | rc=0 (from scratch: the firmware, the embedded copy and the APE all build) |
| `make selftest` | rc=0 — `cpu_selftest: 3522 checks, 0 failures` (**unchanged**), `cpu_m1c_selftest: 228 checks, 0 failures` (222 → 228: the corrected `0F C7 /6` group), `hw_selftest: 338 checks, 0 failures, 0 skipped` (225 → 338: the corrected PCI cases + the UART group + the loader group) |
| `make transcript-diff` | rc=0, diff **EMPTY** — "the C transcript is byte-identical to the reference's" (the M0 contract) |
| `make oracle` | rc=0 — `transcript matches tests/model.py (8 GPRs, EFLAGS, EIP, 561 instructions, 4096-byte window sha256)` |
| `make determinism` | rc=0 — two consecutive reference runs byte-identical |
| `make nasm` | rc=0 — `[+] Passed 118/118 tests`, 0 of 123 produced no state, the same 5 skips (all pre-existing P0 class-B model gaps) |
| the boot pair (§2) | both rc=0, both reach the panic, both end in the same CPU state |

`sha256` of the files this unit touched is in `results/m2-2-gates.txt`'s header, and the
asymmetry rig prints its own before/after shas in `results/m2-2-mut.txt`.

---

## 9. Handoff notes

* **The firmware is the seam, and it is shared code.** `tests/minibios.asm` is a build
  product of `make` (a 64 KiB check on its size, because the reset vector's position depends
  on it) and both legs take it from `build/minibios.bin`: the port embeds it
  (`gen/minibios_bin.py`) and `oracle/run-boot.mjs` reads the file. A later unit that needs
  another BIOS service adds a handler there and *both* machines get it by construction.
* **The reference's bzImage path has no BIOS of its own** (§3). Anyone planning the baliveau
  lane should plan for the firmware: seabios (the browser lane's answer) needs the VGA and
  the IDE controller, and the browser lane's *purpose-built i386 kernel* would need exactly
  the services `tests/minibios.asm` already implements.
* **The clock policy is what to watch when a later unit adds a device.** Every line in §5's
  D3/D4/D5 comes from M2-1F's "1 instruction = 1 µs": a device that makes the guest wait
  (a disk, a NIC, a console with a timeout) will be reached at a different *instruction*
  count on the port, and the kernel's own timeouts will fire at different points. The
  instruction-level agreement at 100 M (§2) is the evidence that this is a clock effect and
  not a device effect.
* **A real kernel is now a cheap test.** `./build/v86c --bzimage tests/images/vmlinuz-i386
  500000000 --cmdline "console=ttyS0"` reaches the kernel's console in **15.4 s** (MEASURED)
  and prints the first **112 lines**; the panic arrives before 800 M (MEASURED: at 800 M the
  output is 287 lines and ends in `---[ end Kernel panic … ]---`). That is the instrument the next unit
  (VGA? IDE? ACPI?) should use, and `V86C_BOOT_DUMP=1` reads the kernel's own log buffer when
  the serial console and the kernel disagree.
