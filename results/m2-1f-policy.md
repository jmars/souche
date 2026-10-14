# M2-1F — the two boot-blocking policy decisions (D-F1: unclaimed ports; D-F2: the clock)

**Unit:** `HANDOFF=v86c`, unit M2-1F (`coder`). **Deliverables:** the two policy decisions
implemented and DECLARED in the tree, `results/m2-1f-policy.md` (this file), and the raw gate
capture `results/m2-1f-gates.txt`.

**Bottom line.** Both decisions are implemented, both are recorded at the definition as well
as here, and every gate is green: `make` RC=0, `make selftest` RC=0 (**3522 + 222 + 225**
checks, 0 failures), `make oracle`/`determinism`/`transcript-diff` RC=0 (the C transcript is
still byte-identical to the pinned reference's), `make nasm` RC=0 (**118/118**), G1's
before/after asymmetry RC=1→RC=0 as required, and 5 mutations of the two decisions each make
the suite fail alone. **MEASURED, the two headline numbers:** a 10 ms PIT reload
(`0x2E9C` = 11932 ticks) now fires at **10001 instructions** where M1c's seat fired it at
**11**; and the port-0x80 probe reads `stopped=0, value=0xff` where M2-1 read
`stopped=1 io_port_write8(port=0x80, PORT_DIAG)`.

Every claim below is tagged **MEASURED** (run here and pasted), **READ** (from the pinned
source), **ARITHMETIC** (derived from the two, shown), or **INTERPRETATION**.

---

## 1. The decision, stated once, in the two places a reader looks

**D-F1 — an unclaimed I/O port behaves as the PINNED REFERENCE does, not as the port's
development stop did.** An unclaimed read answers `0xFF` (8-bit) / `0xFFFF` (16-bit) / `-1`
(32-bit); a write is DISCARDED; the machine does NOT stop. The loudness is kept as a
DIAGNOSTIC on stderr (`V86C_LOG_PORTS`), which changes no guest-visible byte.

**D-F2 — the clock is a DETERMINISTIC VIRTUAL clock: 1 guest instruction = 1 microsecond.**
`microtick()` returns `instruction_counter / 1000.0` — MILLISECONDS, upstream's unit —
which makes it the same unit the PIT, the ACPI PM timer and the TSC are all written for.

Both are declared in the tree where a reader finds them, not only here (§5 below).

---

## 2. D-F1 — the unclaimed port

### 2.1 Why the old policy was the divergence (READ, from the pinned source)

- `v86/src/io.js:36-48` — every one of the 65536 port-map entries starts as an **empty
  entry**; `io.js:60-77` is that entry's handlers: `read8 → 0xFF`, `read16 → 0xFFFF`,
  `read32 → -1`, `write8/16/32 → {}` (a no-op). The entry is the ANSWER, not a marker.
- `v86/src/io.js:332-394` — the dispatch logs when the handler is the empty one
  (`entry.read8 === this.empty_port_read8`) and then **calls it anyway**. There is no stop
  anywhere in that path.
- `v86/src/io.js:113-121` — `register_read`/`register_write` overwrite ONLY the widths a
  device gives. A width the device did not register keeps the empty entry's handler, so a
  16-bit read of an 8-bit-only port returns `0xFFFF` in a **release** build (the DEBUG-only
  `fail` closures at `io.js:115-120` never exist there). **This corrects M2-1's divergence
  table row D2**, which claimed a release build "would throw"; the table now carries the
  correction.
- `v86/src/cpu.js:1168-1175` — ports 0x80 and 0xE9 are registered **inside `if(DEBUG)`**;
  MEASURED by M2-1 and re-confirmed here, the pinned release bundle contains no
  `register_write(128` / `register_write(0x80` at all. So on the reference, every BIOS POST
  code and the Linux kernel's delay loop hit the empty entry.
- **INTERPRETATION, scoped:** that a real BIOS writes 0x80 on every boot, and that the
  Linux delay loop reads it, is the reading of `io.js`'s own name table (`PORT_DIAG`) plus
  the two facts above. It is the reason this decision is boot-blocking, and it is not
  measured here (no BIOS is ported yet).

### 2.2 The change (src/hw/io.c, src/hw/io.h)

The six dispatch functions now narrow to: if the handler was never installed → stop (D2,
unreachable after `io_init`); otherwise **call it** and log if it is the empty entry. So the
answer is literally the reference's function returning the reference's value
(`empty_port_read8`/`16`/`32` = `0xFF`/`0xFFFF`/`-1`, `empty_port_write` = no-op).

The diagnostic: `V86C_LOG_PORTS` (any nonempty value but `"0"`), read ONCE:

```
$ V86C_LOG_PORTS=1 ./build/hw_selftest   # stderr
io: read8  port 0x1234  -> 0xff (unclaimed)
io: read8  port 0x01f0 PORT_ATA1_CMD_BASE -> 0xff (unclaimed)
io: write8 port 0x0080 PORT_DIAG <- 0x21 (unclaimed, discarded)
io: read8  port 0x0080 PORT_DIAG -> 0xff (unclaimed)
... 16 lines total
```

**MEASURED, and this is the "decides nothing" claim:** the same suite with the variable set
reports `hw_selftest: 225 checks, 0 failures` and RC=0 on stdout, and with it unset stderr
is EMPTY (0 lines). The seabios name table survives as the diagnostic's payload.

**M2-1's D1 is SUPERSEDED, and the record says so rather than being silently rewritten.**
`results/m2-1-machine.md` keeps F1, F2 and the D1/D2 rows as measured, with an appended
`> UPDATE — RESOLVED BY M2-1F` block under F1/F2 and a `SUPERSEDED` / `CORRECTED` mark
inline on rows D1/D2.

### 2.3 G1 — the port-0x80 probe, BEFORE and AFTER

Raw: `results/m2-1f-g1.txt`, reproduced by `sh results/m2-1f-g1-rig.sh`. The rig is the
asymmetry AGENTS.md asks for: `results/m2-1f-io-prefix.c` IS the pre-fix artifact (this
tree's `io.c` with the six dispatch bodies restored to M1c's stop form and the stop message
restored to M2-1's wording), and the selftest is run against both.

```
--- BEFORE: results/m2-1f-io-prefix.c copied over src/hw/io.c ---
build RC=0
PORT-0x80 PROBE: `out 0x80, 0x21` -> stopped=1 io_port_write8(port=0x80, PORT_DIAG) (no device claimed this port at this width)| `in al, 0x80` -> value=0x00 stopped=1 io_port_read8(port=0x80, PORT_DIAG) (no device claimed this port at this width)
hw_selftest: 225 checks, 24 failures
FAIL lines: 24
selftest exit status (THE GATE): 1

--- AFTER: the tree's src/hw/io.c restored ---
build RC=0
PORT-0x80 PROBE: `out 0x80, 0x21` -> stopped=0 | `in al, 0x80` -> value=0xff stopped=0
hw_selftest: 225 checks, 0 failures
FAIL lines: 0
selftest exit status (THE GATE): 0
```

Both probes go through the CPU's OWN instruction bodies (`instr_E6` = `out imm8, al`,
`instr_E4` = `in al, imm8`), so this is the guest's path, not a device-side shortcut — and the
AFTER case additionally proves the guest CONTINUES: the very next instruction (a claimed-port
PIT write) takes effect.

### 2.4 The positive contract, and its regression probes

`hw_selftest`'s io group now asserts, MEASURED: an unclaimed read8/16/32 returns
`0xFF`/`0xFFFF`/`-1` and does NOT stop; an unclaimed write8/16/32 is discarded with **no
device state changed** (the port-map entry is `memcmp`ed against a freshly created empty
entry, `device == NULL`, and the PIT's reload/enable are checked either side of the writes);
a port registered at some widths (`0x20`, PIC, 8-bit only) returns `0xFFFF` at 16 bits rather
than stopping; a PCI bar's vacated old base (`0x2002`) returns `0xFF` (`pci.js:557-561`
replaces it with an empty entry); `0xB000` read as 8-bit returns `0xFF`; and reading 0x01F0
returns `0xFF` while `io_get_port_description(0x01F0)` still names `PORT_ATA1_CMD_BASE`.

Asymmetry (G2, raw in `results/m2-1f-mutations.txt`): **5 mutations, each alone**, all
VISIBLE, each followed by a green restore —

| mutation | failing assertions |
| --- | --- |
| `io.c`: the unclaimed read8 answers a fabricated `0` | 11 |
| `io.c`: the unclaimed write8 STOPS | 5 |
| `exec_boundary.c`: `microtick()` returns the raw counter (M1c's seat) | 16 |
| `exec_boundary.c`: `INSTRUCTIONS_PER_MILLISECOND` 1000 → 100 (a 10× error) | 24 |
| `acpi.c`: the PM timer's rate loses its `/1000` | 3 |

---

## 3. D-F2 — the clock, and the three consumers

### 3.1 The unit analysis (READ + ARITHMETIC)

Upstream's `microtick` is the HOST WALL CLOCK (`v86/src/main.js:223-240`: `performance.now`,
`Date.now`, or `process.hrtime`) — **MILLISECONDS**, despite the name. The three consumers
multiply it by their own rate, and each rate is only dimensionally right if the clock is ms:

| consumer | expression | rate | what ms means |
| --- | --- | --- | --- |
| `pit.js:117-140` | `diff * OSCILLATOR_FREQ` | 1193.1816666 kHz | ms × ticks/ms = ticks |
| `acpi.js:161` | `round(now * PMTIMER_FREQ_SECONDS/1000)` | 3579545/1000 per ms | PM ticks per ms |
| `cpu.rs:4365` | `microtick() * TSC_RATE`, `TSC_RATE = 1e6` (cpu.rs:296) | 1e6 counts/ms = 1 GHz | counts per ms |

M1c's seat returned the RAW counter, i.e. **one MILLISECOND per guest instruction** —
1000× fast, M2-1's finding F2. D-F2 fixes the unit to **1 instruction = 1 µs**:
`microtick() = instruction_counter / 1000.0`, so a 1000-instruction step is exactly 1 ms and
each consumer's own arithmetic comes out at the physically right rate. One named constant
(`INSTRUCTIONS_PER_MILLISECOND`), one function, one comment; the definition carries the
"upstream uses the wall clock, here is why the port does not" note.

### 3.2 The three consumers, checked against EACH OTHER (MEASURED, `hw_selftest`'s clock group)

- **the source:** `microtick() == 0.001` at 1 instruction, `1.0` at 1000, `1234.567` at
  1234567 — i.e. µs of virtual time / 1000.
- **the TSC:** deltas of exactly `1 000 000` counts per 1000 instructions, twice in a row —
  **1000 counts per instruction**, the 1 GHz rate `instructions_0f.c:1240` advertises
  through cpuid. (The reads pass the imprecise-browser workaround, `cpu.rs:301-308`; the
  first read is the establish step, which is why that case runs first on fresh statics.)
- **the ACPI PM timer:** `acpi_get_timer()` returns `round(ms × 3579.545)` at 1 ms and 2 ms.
  The IRQ9 case (bit 23 of the PM timer flipping) now fires at **2 343 594 instructions** —
  ARITHMETIC: 2^23 / 3.579545 MHz = 2.343 s = 2 343 594 µs — where M1c's seat needed 2344.
  The test derives the number from the PM timer's own rate, so it fails if EITHER the clock's
  scale or the device's rate drifts.
- **the PIT (G2's headline):** a 10 ms reload (`0x2E9C` = 11932 ticks, the classic HZ=100
  value) is programmed through the ports and then stepped ONE INSTRUCTION AT A TIME until the
  rollover test fires:

```
CLOCK: a 10 ms PIT reload (0x2E9C = 11932 ticks) fires at 10001 instructions
```

  and the same probe against the pre-F2 clock (the `F2.1` mutation) MEASURES the "before":

```
CLOCK: a 10 ms PIT reload (0x2E9C = 11932 ticks) fires at 11 instructions
```

  10001 = 10 ms of virtual time plus the one instruction the strict `>` needs; **11** is what
  1000× too fast looks like. (ARITHMETIC for the 65536-tick reload M2-1 used: 65536 / 1.19318
  = 54926 instructions; the suite parks the clock at 55000 to be safely past it.)

**Determinism is unaffected (MEASURED):** `make determinism` RC=0 (two consecutive reference
runs byte-identical, unchanged by this unit) and `make transcript-diff` RC=0 (the C
transcript is byte-identical to the reference's).

### 3.3 The guest-visible consequence, declared

**The machine's time base is the guest's own execution.** An IDLE guest's clock STOPS, where
upstream's keeps running against the wall; "one instruction per microsecond" is a time base,
not a performance promise (the port does not run at 1 M instructions/s). A guest that
measures elapsed time — `rdtsc`, the PIT, the PM timer — sees time advance exactly as fast as
it executes: deterministic, and byte-identical run to run.

**What this does NOT make deterministic, named because the claim must not overreach:** the
RTC's *time* registers still read the HOST clock (`rtc.c`'s `host_now_ms`, upstream's
`Date.now()` — M2-1's finding F3), so a guest reading CMOS 0x00-0x09 or 0x32 sees host UTC
time and two runs differ there. Only the RTC's *status-A update-in-progress* bit uses
`microtick()`, and it now uses it in milliseconds, as upstream's `% 1000` does. The word
"deterministic" in this unit is scoped to the clock and to everything derived from it; the
M0/oracle gates never read the RTC.

---

## 4. Files changed, and the write scope

| file | what |
| --- | --- |
| `src/hw/io.c` | the six dispatch functions (D-F1), the `V86C_LOG_PORTS` diagnostic, the D2 wording, the file header |
| `src/hw/io.h` | D1/D2 rewritten (D1 now the reference's contract + the diagnostic; D2 restated and corrected) |
| `src/hw/hw_selftest.c` | a new clock group (D-F2, the three consumers, the 10 ms measurement); the io group's D1 contract; the PCI/ACPI width cases; the port-0x80 probe with its printed before/after line; the PIT/handover arithmetic rescaled to the new clock |
| `src/cpu/exec_boundary.c` | `microtick` = counter/1000 + the D-F2 declaration at the definition (G4) |
| `src/hw/pit.h`, `src/hw/acpi.h`, `src/hw/rtc.c`, `src/hw/machine.c`, `src/cpu/exec_boundary.h` | the clock's unit / the unclaimed-port contract, corrected at the places that stated the old one |
| `src/cpu/instructions.h`, `src/cpu/cpu.h` | **outside the brief's file list** — comment-only, see below |
| `results/` | this report, `m2-1f-gates.txt`, `m2-1f-gates-rig.sh`, `m2-1f-g1.txt`, `m2-1f-g1-rig.sh`, `m2-1f-io-prefix.c`, `m2-1f-mutations.txt`, `m2-1f-mut-rig.sh`, and the appended corrections in `m2-1-machine.md` |

**The Makefile was NOT changed** (no new source or target: the clock lives in an existing TU
and the runtime selftest already links everything new needs).

**`src/cpu/m1c_selftest.c` was NOT changed, and I checked why rather than assuming:** its
"unclaimed device ports still STOP" case reads port 0x40 on a machine where `machine_init`
was never called, so the entry's handler really is NULL and the D2 stop still fires (`make
selftest` passes it unchanged). It is a D2 case, not a D1 one.

**The two edits outside the brief's allowed list** are the clock's unit at its two DECLARATION
sites (`instructions.h`'s `microtick` interface block and `cpu.h`'s TSC block), which is where
G4 asks the divergence to be findable. Both are inside `/* */` blocks only, and that is
PROVEN rather than asserted: with the pre-change comment text restored into copies,
`cc -E -P` output is byte-identical to the current files' (3608 and 39041 bytes, `cmp` clean),
which is the "comments-only" proof preprocessing gives.

No `v86/` file was touched; `tests/` and `oracle/` were not modified (the transcript gates
re-derive `tests/test.img`'s sha256). No commits.

---

## 5. G4 — where the two decisions live in the tree

- **D-F1**: `src/hw/io.h`'s `D1`/`D2` divergence entries (the contract, the diagnostic, and
  what is superseded); `src/hw/io.c`'s dispatch comment block (`D-F1 (HANDOFF=v86c unit
  M2-1F)`) and the diagnostic's own comment; `src/hw/machine.c`'s "absent devices" paragraph.
- **D-F2**: `src/cpu/exec_boundary.c`'s definition comment (the scale, the three consumers,
  M2-1's F2 measurement, and the guest-visible consequence) plus the `D-F2` note at
  `microtick`'s declaration in `src/cpu/instructions.h`; `src/cpu/cpu.h`'s TSC note;
  `src/hw/pit.h`, `src/hw/acpi.h`, `src/hw/rtc.c` at each consumer.

---

## 6. Handoff notes

1. **The two boot blockers are both cleared, in the order the first boot meets them.** No
   port stops a guest on the reference's path any more, and the timer no longer fires 1000×
   too fast.
2. **The clock is now a compile-time policy in one place.** If a later unit wants a
   wall-clock or a different `µs per instruction` (e.g. to make a guest's calibration loop
   behave), it changes `INSTRUCTIONS_PER_MILLISECOND` in `exec_boundary.c` and nothing else —
   all three consumers and the selftest's clock group follow from it. A wall clock remains
   incompatible with the oracle contract.
3. **The RTC's time registers are the machine's one remaining nondeterminism** (§3.3). If the
   first boot needs a fixed date (some kernels sanity-check the CMOS year), that is the next
   decision, and it belongs to the RTC's owner.
4. **`V86C_LOG_PORTS` is the intended instrument for the first boot attempt**: turn it on and
   the first missing device announces itself by name (the seabios table is the payload) with
   no change in behaviour. Note the flip side, stated rather than discovered later: a port
   NOTHING claims is now silent by default, so "which device is missing" is a diagnostic you
   must ask for, not a crash you get.
5. **`llround` is not in cosmocc's libc** (MEASURED: an undefined reference at link time);
   the selftest uses a local `round_to_u32`. Worth knowing before the next unit reaches for
   the C99 rounding family.
