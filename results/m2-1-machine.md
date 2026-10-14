# M2-1 — the machine: port I/O, the bus, and PIT/RTC/DMA/PS2/PCI/ACPI

**Unit:** `HANDOFF=v86c`, unit M2-1 (coder). **Deliverable:** `src/hw/` — the device layer:
the I/O port map, the device bus, and the six small devices a boot needs before storage or
video. Every claim below is tagged **MEASURED** (run on this host and pasted), **READ** (from
the upstream reference) or **INTERPRETATION**.

**Bottom line.** The machine exists and is gated: a PIT port write reaches the PIT through the
CPU's own `out` instruction, an unclaimed port still stops with the port named (M1c's
fallthrough MOVED to `src/hw/io.c` rather than being deleted), and the M0 transcript is still
byte-identical to the pinned JS reference. All gates green — `make` RC=0, `make selftest`
RC=0 (3522 + 218 + 198 checks, 0 failures), `make oracle`/`determinism` RC=0,
`make transcript-diff` RC=0, `make nasm` RC=0 (118/118), the asymmetry rig RC=0 (16/16
mutations visible). Raw evidence: `results/m2-1-gates.txt`, `results/m2-1-gates-mut.txt`.

---

## 1. Files, and the JS → C mapping

The brief asks for one C file per upstream JS file so the mapping stays auditable. That is
what was built, **plus one file the brief's list has no slot for**: `src/hw/machine.c`, which
holds the three things that live in `cpu.js`/`cpu.rs` and have no JS device file of their own
(§4). Saying so here is the brief's own condition for a different layout.

| C file | lines | upstream JS | lines | what it is |
| --- | --- | --- | --- | --- |
| `src/hw/io.c` / `.h` | 511 / 127 | `v86/src/io.js` + `cpu.js:227-289` + `cpu.rs:4613-4649` | 471 + 63 + 37 | the port map, the 8/16/32-bit dispatch, the `mmap_register` half of the memory map, and the **`io_port_*` bridge taken over from `pic.c`** |
| `src/hw/bus.c` / `.h` | 75 / 46 | `v86/src/bus.js` | 103 | `BusConnector` + `Bus.create` (minus `send_async`, §3 D5) |
| `src/hw/pit.c` / `.h` | 337 / 66 | `v86/src/pit.js` | 359 | the 8254, its three counters and the IRQ0 |
| `src/hw/rtc.c` / `.h` | 380 / 128 | `v86/src/rtc.js` + `cpu.js:1644-1711` | 424 + 68 | the CMOS RTC + `CPU.prototype.fill_cmos` (§3 D6) |
| `src/hw/dma.c` / `.h` | 357 / 84 | `v86/src/dma.js` | 430 | the 8237, the mask/mode/flip-flop state machine, the two transfer routines |
| `src/hw/ps2.c` / `.h` | 682 / 109 | `v86/src/ps2.js` | 841 | the 8042 + keyboard + mouse, the non-browser half |
| `src/hw/acpi.c` / `.h` | 191 / 51 | `v86/src/acpi.js` | 216 | the PIIX4 ACPI (PM1, status, PM timer, GPE) |
| `src/hw/pci.c` / `.h` | 452 / 109 | `v86/src/pci.js` | 603 | the PCI bus, config space, the BAR machinery |
| `src/hw/machine.c` / `.h` | 113 / 59 | `cpu.js:1176-1266, 1924-1941`, `cpu.rs:4596-4610` | — | **the wiring**: the device set in order, `run_hardware_timers`, `device_raise_irq`/`device_lower_irq` |
| `src/hw/hw_selftest.c` | 994 | — | — | G1, the device gate (9 groups, 198 checks) |

Files **modified** (and why):

| file | change |
| --- | --- |
| `src/cpu/pic.c` / `.h` | the `io_port_*` bridge DELETED (it is io.c's now); the twelve port accessors promoted from `static` to `pic.h`'s interface so io.c can intercept them in front. **No behaviour change**: the same six ports, the same `handle_irqs()` after an 8-bit PIC write. |
| `src/cpu/exec_boundary.c` / `.h` | `run_hardware_timers` DELETED there — `machine.c` defines it (the brief's "wire the seat"). Two header claims that are now false were CORRECTED rather than left ("nothing raises an IRQ", "a guest that does `sti; hlt` idles forever"). |
| `src/cpu/instructions.h` | the timer seat's comment (who owns `run_hardware_timers` now) |
| `src/cpu/memory.c` / `.h` | `write_blob` (cpu.js:299-311), a **pure addition**: the DMA's `do_read` is its first caller |
| `src/main.c` | `machine_init()` after the memory setup; the loader's own private `write_blob` renamed `loader_write_blob` (the name is memory.c's now — a name collision in one translation unit) |
| `Makefile` | `HW_SRC`/`HW_OBJ` + the hw object rule; the three selftest binaries became real file targets (the asymmetry rig needs to rebuild ONE of them); `HW_SRC` in the m1c link |

`v86/` was read only. `tests/` and `oracle/` were not touched (`tests/test.img` sha256 is
unchanged: the transcript-diff gate re-derives it from the tree).

---

## 2. The gates (G1–G4), with raw evidence

Raw capture: **`results/m2-1-gates.txt`** (whole suite, exit codes included),
**`results/m2-1-gates-mut.txt`** (the rig), `results/m2-1-mut-rig.sh` (the rig itself).

**G1 — the C selftest, `src/hw/hw_selftest.c`, built and run by the SAME `make selftest`**
(`make selftest` RC=0; `hw_selftest: 198 checks, 0 failures`). Nine groups:

```
PASS io: the port map, the loud stops, the mmap half + the R1F carve-out
PASS pit: the ports, the latch, the arithmetic, and a delivered IRQ0
PASS rtc: the CMOS round-trip, the status registers, BCD vs the host clock
PASS dma: the registers, a transfer into guest memory, and the reverse
PASS ps2: a scancode, the identify replies, a mouse packet, IntelliMouse id
PASS pci: config space, an absent device, the I/O bar move, the stops
PASS acpi: PM1, the GPE registers, the PM timer, IRQ9
PASS timer seat: main_loop wakes a halted guest on IRQ0
PASS G4: the CPU's own in/out reaching the PIT and stopping on 0x1234
hw_selftest: 198 checks, 0 failures
```

The brief's five named observables, each with the case that pins it:

- **a PIT channel firing a callback after the programmed count** — `test_pit` programs
  counter 0 through ports 0x43/0x40, advances the clock past the reload and calls the poll;
  an IDT handler is entered (`cs = SEL_CODE2`, `eip = HANDLER_EIP`), the frame is on the
  stack, and the halted CPU is woken. `test_timer_wakes_the_guest` does the same through the
  REAL run loop (`main_loop()` → `run_hardware_timers` → the PIT → the PIC → the handler →
  `iret` → the guest's `hlt`).
- **an RTC register round-trip** — the diagnostic-status register through ports 0x70/0x71
  (the one index whose write has no side effect), plus register A's rate divider
  (`32768 >> (0x2F & 0xF) - 1 = 2` ⇒ `periodic_interrupt_time == 500`), register B, register
  C's read-and-clear, register D, and the BCD second/minute/year checked against the host
  clock's own fields.
- **a DMA transfer landing bytes in guest memory** — channel 2 programmed (address 0x2340,
  page 0x05 ⇒ 0x052340, count 3 ⇒ four bytes), `dma_do_read` writes `DE AD BE EF` to
  0x052340 and advances the channel to 0x2344; the reverse (`dma_do_write`) copies guest
  memory into the buffer and the autoinit restores the channel.
- **a PS/2 scancode producing the right byte** — 0xF4 enables scanning, the code arrives on
  the bus (`keyboard-code`), the status port's bit 0 rises, and port 0x60 reads exactly
  0x1E. Also: the self-test (0xAA→0x55), identify (0xF2→0xFA,0xAB,0x83), a mouse delta
  producing the 3-byte packet with the right sign bits, and the IntelliMouse sample-rate
  dance (200→100→80 ⇒ id 3).
- **a PCI config-space read/write** — the host bridge 0x12378086, its PAM0 byte at offset
  0x59 = 0x10, the ISA bridge 0x70008086, an absent device (`response = -1`, `status = 0`),
  the I/O-bar move, and the size probe.

  > **CORRECTED BY M2-2 (HANDOFF=v86c).** M2-1 said a WRITE to an absent device's config space
  > STOPS the machine, citing a `new Uint8Array(this.device_spaces[bdf].buffer)` dereference.
  > That is true of `pci_write8` and `pci_write16` (pci.js:328, 354) and FALSE of
  > `pci_write32`, which reads the space with no dereference and returns silently
  > (pci.js:381-390) — as M2-1's own cited line number (390) was inside write32's body. The
  > port's write32 now matches upstream (ignored), the 8/16-bit stops stay, and the index is
  > BOUNDS-CHECKED: upstream's 16-bit `bdf` into 256 initialised entries is `undefined` above
  > 255 (an absent device), while the C port's array read ran off its end — MEASURED as a
  > SIGSEGV at instruction 550020331 of the first real kernel boot (results/m2-2-boot.md).

**G2 — asymmetry.** `results/m2-1-mut-rig.sh`: **16 single-line mutations**, each applied
alone and each REQUIRED to make `hw_selftest` fail, then restored and required green again.
All 16 were VISIBLE (RC=0); no mutation was invalid (none failed to compile, none was
invisible, and the restore is green). The rig prints the failing assertions per mutation —
e.g. M1 (`cmos_c &= ~0xF0` → `~0x0F`) fails `cmos_c == 0x00`; M3 (the PIT's start value no
longer follows the reload) fails 3 assertions; M15 (`machine.c`'s poll not driving the
devices) fails the delivered-IRQ group. The rig prints `INVALID` and exits non-zero if any
mutation compiles and passes — the rule m1c's rig records (two of M1a-2's first attempts were
invalid, and one of this rig's own first attempts was too: mutation "M5" was initially a
no-op `(void)0;` line, which is why the rig now says that a mutation which compiles and
passes is a FAILURE of the rig, not a pass of the test).

**G3 — no regression.** `make clean && make` RC=0; `make selftest` RC=0; `make nasm` RC=0
(`[+] Passed 118/118 tests`, 5 labelled skips — the same set and count as M1b/M1c);
`make oracle` RC=0 and `make determinism` RC=0; **`make transcript-diff` RC=0 — "the C
transcript is byte-identical to the reference's"**, so the M0 contract survives a real
machine being installed. MEASURED why the corpus cannot move: no test in
`v86/tests/nasm/*.asm` contains an I/O instruction (the only `in`/`out` matches are inside
comments).

**G4 — the handover is real.** Inside `hw_selftest`'s last group, and it is the CPU's own
instruction bodies, not a device-side shortcut:

```
write_reg8(AL, 0x34); instr_E6(0x43);      /* out 0x43, al  -> instructions.c:1770 -> io_port_write8 */
CHECK_EQ(pit->counter_mode[0], 2);          /* ... the PIT saw it */
...
io_port_write8(0x21, 0xFE);                 /* the PIC's six ports are still intercepted IN FRONT */
CHECK_EQ(io_port_read8(0x21), 0xFE);
...
instr_E6(0x1234);                           /* a genuinely unclaimed port */
CHECK(interpreted_unimplemented);
CHECK(strstr(unimplemented_instruction_name(), "0x1234") != NULL);   /* ... still stops BY NAME */
```

and then the pair that says WHAT MOVED: `io_port_read8(0x64)`, `0x61` and `0x71` — PS/2
status, the PIT's control byte, the RTC's data port — now ANSWER instead of stopping.

---

## 3. Every divergence from upstream, with its reason

Written once here and at the site. The three in `io.h` are the policy ones; the rest are
per-device.

| # | divergence | reason, and what a guest observes |
| --- | --- | --- |
| **D1** | **An unclaimed port STOPS the machine** with the port named (io.c's `io_stop_width`); upstream's empty entry answers 0xFF/0xFFFF/-1 and swallows writes (io.js:50-77). | The brief's explicit instruction: keep M1c's behaviour for the ports no device claims, and change only WHICH ports are claimed. A guest writing a port nothing owns stops instead of the reference's silent drop. **Consequence named because it is not small** — see §5 finding F1 (port 0x80). **SUPERSEDED by M2-1F's D-F1** (the finding above carries the update; results/m2-1f-policy.md). |
| **D2** | A port registered for SOME widths stops for the others, naming port + width + device. Upstream installs a `dbg_assert`-fail function under DEBUG and leaves the field `undefined` in a release build (io.js:97-107 — a release build would throw). | The same information a DEBUG build gives, in every build, with no fabricated value. A guest reading 0xB008 as 8-bit stops here; upstream's DEBUG build asserts and its release build throws. **CORRECTED by M2-1F**: MEASURED against the source, a release build does NOT throw -- `register_read` leaves the unregistered width at the EMPTY entry's handler, which ANSWERS 0xFFFF (io.js:113-121), so this row described a case the port map never reaches; the stop now survives only for an entry with NO handler at all (io.h D2). |
| **D3** | Write callbacks take the port: `void (*)(void *device, int32_t port, int32_t value)`. | C has no closures (`function(value) { this.counter_write(0, value); }`). This is what makes io.js:171-256 (`register_*_consecutive`) expressible WITHOUT one: the composed handlers recompute their neighbours from the port map at call time. Behaviour identical. |
| **D4** | **The unclaimed range is NOT re-registered**: `io_init` does not install io.js:42-61's callbacks for `[memory_size, MMAP_MAX)`; an unregistered block falls through to `memory.c`'s default table. | Upstream registers that range and then its VGA device OVERWRITES the two ranges the VGA owns. This port has no VGA, and memory.c's default table is its stand-in (M1a-1's R1F measurement: an unclaimed address reads 0xFF/-1, and the VGA window + the 8 MiB LFB read 0). Registering the range would SHADOW that carve-out for the whole LFB. **MEASURED, and it is a real error I made and caught**: with the registration in place, `read32s(0xE0000000)` = -1 where the reference reads 0 (the selftest's R1F case failed). A device that registers a range (M2's VGA, virtio) still overwrites it through `io_mmap_register`. |
| **D5** | `BusConnector.send_async` is dropped (bus.js:84-88). | It is `setTimeout(this.send, 0)` — the browser event loop's ack to the UI. A synchronous emulator has no event loop to defer to, and every listener in the ported set is called synchronously either way. |
| **D6** | `CPU.prototype.fill_cmos` (cpu.js:1644-1711) lives in `rtc.c`, with its `settings` dependency fixed to this machine (`boot_order = BOOT_ORDER_CD_FIRST`, no `fastboot`). | It is the RTC's bring-up — the CMOS registers a BIOS reads the memory size and the boot order from — and it has no other home in the port. Without it a BIOS sees 0 memory above 1 MiB. A guest sees: base 640 KiB, extended = (memory_size − 1 MiB) >> 10 (1024 for the 2 MiB machine), equipment 0x2F. |
| **D7** | `dma_do_write`'s guest-memory read is BOUNDED: a transfer past `memory_size` STOPS, naming the channel, the address and the length. | Upstream reads `mem8` (the whole wasm linear memory) and an access past it traps in wasm; in C that read is host memory outside the guest's 2 MiB allocation (`mem8 + 0xFFFFFF00` is a segfault, not a trap). The bounded arm is reachable only from a device that misprograms a channel, and it says so instead of crashing. |
| **D8** | `ps2.c` port 0x64 command 0xFE ("CPU reboot via PS2", ps2.js:834-837) stops with the command named; `pci.c`'s 0xCF9 reboot (pci.js:161-165) likewise. | `cpu.reboot_internal()` does not exist in this port (the machine reset path is a later unit). A guest that asks for a reboot gets a named stop, not a silent no-op and not a pretend reboot. |
| **D9** | `pci_write8/16/32` to an ABSENT device stops with the bdf named; `pci_query` seeds its response array once. | Upstream DEREFERENCES the undefined config space (`new Uint8Array(this.device_spaces[bdf].buffer)`, pci.js:390) and throws a TypeError; there is no upstream behaviour to reproduce, and a stop with the bdf is strictly more useful than a segfault. |
| **D10** | `pci_bar_t.entries` are VALUE copies from a fixed pool (4096 entries); `pci_device.pci_space` carries its own length (64..256). | Upstream's `bar.entries[j] = this.io.ports[port+j]` holds object REFERENCES; value semantics reproduce `set_io_bars` exactly (the source port is replaced by a fresh empty entry, the destination gets the copy). The length is load-bearing: the host bridge's space is **96 bytes**, not 64, because PAM0 is at offset 0x59 — outside the 64-byte header — while `register_device` copies only what the device declares into a zeroed 256-byte space. |
| **D11** | `io_port_read8(0x20/0x21/0xA0/0xA1/0x4D0/0x4D1)` returns `uint32_t` from pic.c's accessors; the ELCR bytes are `elcr` fields. | Verbatim from pic.rs:361-385 — the type is the only change (the C port has no `u8` return in that spot). |
| **D12** | `dbg_log!`/`dbg_trace!` are dropped everywhere (the port has no log channel); their TEXT is kept as a comment where it names a behaviour. | As in M1a-1/M1b/M1c. The one place this loses information is the "Unimplemented ..." lines: they are LOGS upstream, so the behaviour they describe is a silent no-op in BOTH implementations (e.g. an unknown PS/2 command, an unimplemented mouse command, a PIT counter-1 programming). Kept verbatim, and listed in §6. |
| **D13** | Two upstream checks that cannot fail are NOT reproduced: `dbg_assert(blob && blob.length >= 0)` (a length cannot be negative) and `dbg_assert(index < 128)`-style checks on an already-masked value. | A check that cannot fail is not a check (the project's standing rule). The CAN-fail halves are kept (address range, PCI space length ≥ 64, bar size a power of two, the `tlb`-style alignment asserts). |
| **D14** | The ACPI device is created only when `cpu->acpi_enabled` (as cpu.js:1181-1185) — i.e. **not** in the APE's default machine. | MEASURED against the pinned reference: `settings.acpi` is undefined in oracle/run.mjs's config, so `acpi_enabled[0] = +undefined` = 0 and the reference does not create it either. The selftest creates it explicitly (`acpi_init`) to exercise it. |

---

## 4. `machine.c` — the three things with no JS device file

1. **the device set, in cpu.js:1176-1266's order** — `io_init`, `pci_init`, `acpi_init` (if
   enabled), `rtc_init` + `rtc_fill_cmos`, `dma_init`, `ps2_init(device_bus)`,
   `pit_init(device_bus)`. This is the ONE place the port answers "what machine is this";
   the selftest's fresh machine is `machine_init()` again.
2. **`run_hardware_timers`** (cpu.js:1924-1941) — drives the PIT and the RTC, and the ACPI
   timer when the machine has one; returns the smallest next-interrupt time. It keeps M1c's
   no-device behaviour as its `!machine_ready` arm (return the argument), which is why
   `m1c_selftest.c` still passes with no change: its binary never calls `machine_init`, so it
   runs the M1c machine. Upstream also folds in `apic_timer`'s value; apic.rs is deferred,
   which is stated at the site.
3. **`device_raise_irq`/`device_lower_irq`** (cpu.rs:4596-4610) — the devices' only way to
   interrupt: set/clear the PIC line, then run the poll. The `acpi_enabled` arm also drives
   the IOAPIC upstream; ioapic.rs is deferred, so a machine with `acpi_enabled` set STOPS at
   the first raise with "ioapic::set_irq (ioapic.rs is M2)" rather than silently dropping
   half the interrupt path. Unreachable on this machine (`acpi_enabled` is never set) and
   named so it cannot be forgotten.

---

## 5. Findings (things a later unit must decide or know)

**F1 — the 0x80 POST/delay port STOPS the machine.** MEASURED, with a regression probe in
`hw_selftest`: `io_port_write8(0x80, 0x21)` sets `interpreted_unimplemented` and names
"0x80". READ: `cpu.js:1168-1175` registers 0x80 (read → 0xFF, write → discarded) and 0xE9
**only under DEBUG**, and this port's policy (D1) makes an unclaimed port loud. MEASURED
against the pinned build: the npm reference's `build/libv86.mjs` contains **no** registration
for port 128 (`register_write(128`/`register_write(0x80`/`128,this,function`: 0 occurrences),
i.e. the empty entry answers it — a silent no-op write and a 0xFF read. READ/INTERPRETATION
on the guest side: io.js's own port-name table calls 0x80 `PORT_DIAG` "via seabios
ioport.h", and a BIOS's POST codes and the Linux kernel's delay loop both use it — that it is
WRITTEN ON EVERY BOOT on the reference is INTERPRETATION from those two facts, not a
measurement I made here (no BIOS is ported yet). This is
a genuine tension: the loud stop is the port's best property (no fabricated values), and it
is exactly what will stop the FIRST boot attempt at the first `outb $0x80` in seabios.
**Recommendation (a policy decision, not mine to make):** register the DEBUG ports
(cpu.js:1168-1175) unconditionally in `machine_init` — 6 lines, and they are the two ports
whose entire semantics is "discard/read 0xFF" — or accept the stop as the intended loud
failure. I did NOT do it unilaterally because the brief constrains D1 to "unclaimed ports
stop", and 0x80 would then be claimed by a device that does nothing.

> **UPDATE — RESOLVED BY M2-1F (HANDOFF=v86c unit M2-1F, decision D-F1).** The policy call
> was made the other way: an unclaimed port now ANSWERS EXACTLY AS THE PINNED REFERENCE DOES
> (0xFF/0xFFFF/-1 on a read, a discarded write; io.c's dispatch) and the loudness moved to
> the `V86C_LOG_PORTS` stderr diagnostic. M2-1's D1 (published on the same page as this
> finding) is therefore SUPERSEDED: it was right while there was no device layer, and it is
> a divergence from the reference now that there is one. The measured before/after of this
> very probe is in `results/m2-1f-g1.txt`, and the full account in
> `results/m2-1f-policy.md`.

**F2 — the timer's unit makes the PIT ~1000× too fast for real guest work.** MEASURED:
`run_hardware_timers` now drives the PIT for real, and the clock it consumes is
`exec_boundary.c`'s `microtick` = the **guest instruction counter** (M1c's seat, which M1c
documented as "the unit is the guest instruction" without fixing a scale). But `pit.js`'s
arithmetic is written for MICROSECONDS-turned-MILLISECONDS: `ticks = diff * OSCILLATOR_FREQ`
with `OSCILLATOR_FREQ = 1193.18` kHz means `diff` is in MILLISECONDS (READ, and confirmed by
`acpi.js`'s `now * PMTIMER_FREQ_SECONDS/1000`). So this machine advances **one emulated
millisecond per guest instruction**, and a Linux-style PIT reload of 11932 (10 ms) rolls over
every **10 instructions** — the guest would spend nearly all of its time in the timer
handler. MEASURED in the selftest: with reload 0x0010 the guest can never get past the
handler (each instruction re-fires IRQ0); the end-to-end case therefore uses reload 0xFFFF
(~55 instructions per tick) and says why. **This is a decision the next unit must make**:
either scale the clock (1 instruction ≈ 1 µs, i.e. `microtick() = instruction_counter /
1000.0` — the M0 transcript does not change: it is unaffected by batching, and no gate reads
the devices) or give the timers a real clock and accept nondeterminism. I did NOT change
M1c's seat: it is another unit's, the brief said "wire it", and the honest state is reported.

> **UPDATE — RESOLVED BY M2-1F (HANDOFF=v86c unit M2-1F, decision D-F2).** The first option
> above was taken, and it is now DECLARED IN THE CODE: `microtick()` returns
> `instruction_counter / 1000.0` — MILLISECONDS of a deterministic VIRTUAL clock at 1 guest
> instruction = 1 MICROSECOND (src/cpu/exec_boundary.c, the definition). All three consumers
> were checked against one another and now agree; MEASURED: a 10 ms PIT reload (0x2E9C =
> 11932 ticks) fires at 10001 instructions, and the TSC advances 1000 counts per
> instruction. `results/m2-1f-policy.md`.

**F3 — the PIT/RTC/ACPI's *determinism* is preserved, and the one exception is named.** The
PIT and the ACPI PM timer are pure functions of the instruction stream (F2's caveat aside).
The RTC is NOT: upstream reads `Date.now()` for the CMOS time registers and for scheduling,
so a guest reading CMOS 0x00-0x09/0x32 sees the HOST's current UTC time and two runs differ
there. MEASURED against the host clock in the selftest (the BCD second/minute are checked
against an independently computed BCD of `gmtime`'s fields). This is upstream's behaviour,
not an artifact, and it is the only device value a guest can read that is not a function of
its own instruction stream.

**F4 — the multimask register does not round-trip, upstream.** MEASURED:
`io_port_write8(0x0F, 0x05)` then `io_port_read8(0x0F)` returns **0x11**, because the write
stores the raw masked bit (`data_byte & (1 << i)`, dma.js:196-199) and the read shifts it
again (dma.js:206-214). Reproduced verbatim and asserted as-is; a guest that writes and reads
back this register sees upstream's values, not the documented ones.

**F5 — `ps2.apply_scaling2` is dead AND wrong upstream.** READ: both of its call sites in
`send_mouse_packet` are commented out (ps2.js:365-370), and its `sign = n >> 31` is 0 for
positive inputs, so `apply_scaling2(4)` is 0, not 6. Ported verbatim, asserted as the faithful
values in the selftest, and its only caller anywhere is that test.

**F6 — the guest-program lesson from this unit's own rig.** The end-to-end timer case first
failed for a reason that was MINE, not the machine's: I placed the guest's `hlt` on the last
byte of the stack page (0x6FFF), where the interrupt frame itself lives (`[STACK_TOP-12,
STACK_TOP)` = 0x6FF4..0x6FFF), so the frame overwrote the guest's code and the handler's
`push` overwrote the saved EFLAGS. Worth recording for later units that craft guest programs:
the machine's own stack frame is at the top of the stack page, so code placed there is not
code, it is the frame.

**F7 — `cpu_m1c_selftest`'s check count moved from 193 to 218, and the move is ANOTHER
UNIT's, not this one's.** The brief's STATE section says "3522 + 193 checks"; the tree now
reports 218. MEASURED: `m1c_selftest.c` has 220 `CHECK`/`CHECK_EQ` macro sites and executes
218, and `test_dispatcher_fault_flag` — its label is "R5F:", i.e. the file's own attribution
— is exactly **25** checks: 193 + 25 = 218. The file's mtime is **14:46:00**, between my own
14:33 observation of "193" and the 218 I measured at 14:50, so **a concurrent unit wrote it
while this unit was running** (see F8) — the INTERPRETATION that the brief's 193 predates that
unit's test is consistent with both numbers, and the arithmetic is MEASURED. Nothing in this
handover adds a check to that binary: it never calls `machine_init`, so it runs the M1c
machine (no devices; `run_hardware_timers` returns its argument through machine.c's
`!machine_ready` arm), which is why its `io_port_read8(0x40)` "stops with 0x40" assertion
still holds.

**F8 — COORDINATION: the brief says the concurrent unit "writes nothing", and that is not
what the tree shows.** MEASURED: `src/cpu/cpu.c` (14:43:54), `src/cpu/cpu_state.h`
(14:44:00), `src/cpu/m1c_selftest.c` (14:46:00) and `src/cpu/instructions_0f.c` (14:47:07)
were all modified DURING this unit's session, by a unit whose own artifacts are
`results/r5f-{probe.sh,probe.c,mut.sh,mut.txt}` (its rig's header says "another unit owns
src/hw/ while this runs" — it knew about M2-1 and used a shadow copy for its mutations, which
is the right discipline; `fx-agent-presence` showed a live `coder` session in this directory
throughout). Two consequences, both handled rather than assumed away: (a) my check-count
observation above is explained by it, and (b) my final gate run was made at 15:0x, AFTER its
last write, so the green numbers in §2 are against a tree that includes its changes —
**MEASURED by the file mtimes, not by trusting the brief**. One thing I could not verify
inside this unit: whether that unit's probe build (which links `src/cpu/*` WITHOUT `src/hw/*`)
was affected by this handover removing `io_port_*` from `pic.c` at 14:21 — its probe output
shows successful runs, so either it defines its own recording bridge (as this project's
probes do for boundary symbols) or it built before the change. Flagged for the orchestrator
because both units are live in one tree.

---

## 6. What is stubbed or approximated, and what a guest observes

Nothing in the ported behaviours is stubbed: every device is a full transcription of its
upstream JS. What is missing is DEVICES and host paths, and each is named with its
consequence:

| missing | what a guest observes |
| --- | --- |
| The VGA (and with it the real LFB device registration) | The VGA window (0xA0000-0xBFFFF) and the 8 MiB LFB read 0 at boot, as the reference's zeroed VGA buffer does (memory.c's default table, D4). A guest that programs the VGA sees nothing happen; port writes to 0x3C0-0x3DA stop with the port named. |
| The UARTs (0x3F8...), the parallel port (0x378), the IDE controller (0x1F0/0x170), the floppy controller (0x3F0-0x3F7 and DMA channel 2), virtio, the network cards, the sound card | Those ports STOP with the port named (D1). A BIOS probing for a serial port stops the machine rather than seeing an absent device. |
| The APIC and the IOAPIC | `handle_irqs`' `acpi_enabled` arm and `device_*_irq`'s IOAPIC half stop with the name (unreachable without `acpi_enabled`). Both mmio windows are stubs from M1a. |
| `cpu.reboot_internal` | The two reboot commands (PCI 0xCF9, PS/2 0x64 0xFE) STOP with the command named (D8). A guest that asks for a reset does not get one. |
| The host input path | The PS/2 is complete and bus-driven, but nothing is attached to `machine_host_bus` yet, so no scancode or mouse packet ever arrives: a guest that waits for a keystroke waits forever. The bus messages and their listener signatures are in place (`keyboard-code`, `mouse-click`, `mouse-delta`, `mouse-wheel`, `mouse-enable`). |
| The speaker | `pcspeaker-enable`/`-disable`/`-update` are SENT on the bus and nobody listens (upstream's are for the browser's audio). Silent, as on a machine with no speaker. |
| Logs | `dbg_log` has no channel: everything upstream logs and does nothing about is a silent no-op here too — an unknown PS/2 controller/keyboard/mouse command, a PIT counter-1 programming, a PIT read-back command (port 0x43 with i = 3), a PCI 16-bit write to a BAR, an unsupported memory-bar change, a PIT counter mode other than 0/2/3 (the mode is still STORED). Each of those is a comment at the site. |
| `fill_cmos`'s `fastboot` and `boot_order` settings | Boiled down to this machine's defaults (D6); the 0x3f fast-boot byte is not written. |

A guest that programs the PIT and does `sti; hlt` now gets its IRQ0 and makes progress
(MEASURED end-to-end in `test_timer_wakes_the_guest`) — that is the M1c limitation this unit
closes, subject to F2's timing scale.

---

## 7. Handoff notes for the next units

- **The `io_port_*` handover is done and is a two-sided contract**: `pic.c` keeps the six
  ports' register accessors (now `pic.h`'s interface) and `io.c` intercepts them in front of
  the map. A unit that adds a device registers it through `io_register_read/write` (or the
  consecutive helpers) and must NOT add a case to the bridge.
- **`io.h`'s D1/D2 are the port's policy for a port nobody claims.** M2-4 (virtio) and the
  storage/video units inherit it; F1 is the case that needs a decision before a boot.
- **The mmap half is `io_mmap_register`** with `memory_device_hooks_t` behind it (memory.c).
  A device that needs an mmio window (virtio's bars, the VGA LFB) registers there — and, per
  D4, the VGA unit should register the LFB and the 0xA0000 window itself, replacing
  memory.c's default-table stand-in.
- **`write_blob` (memory.h) is the port's shared guest-memory blob writer** (cpu.js:299);
  the IDE/virtio units should call it rather than copying it. `main.c`'s loader keeps its own
  `loader_write_blob` because it dies on an overlap instead of asserting.
- **F2 is the timer unit's largest open question**, and it is a clock-scale decision, not a
  device one.
- **The PIT's `counter_start_time`/`counter_start_value` arithmetic assumes the clock is
  monotone and is never rewound**; a `restore_state` path (which this port has no image for)
  would need `did_rollover`'s `diff < 0` arm, which is ported.
