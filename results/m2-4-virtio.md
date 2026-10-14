# M2-4 — virtio and the virtio console, proved by the guest kernel's OWN virtio driver

**Unit:** `HANDOFF=v86c`, unit M2-4 (coder). **Deliverables:** `src/hw/virtio.{c,h}` (the port of
`v86/src/virtio.js`, 1517 lines), `src/hw/virtio_console.{c,h}` (the port of
`v86/src/virtio_console.js`, 301 lines), the additive wiring in `src/hw/machine.{c,h}`,
`src/main.c`'s boot mode, `src/hw/hw_selftest.c`'s `virtio` group, `tests/mkinitramfs.py` +
`tests/init-virtio.sh` + `tests/images/guest-initramfs-virtio.img`, the firmware's PCI interrupt
routing in `tests/minibios.asm`, and `oracle/run-boot.mjs`'s `--virtio-console` /
`--virtio-console-in`.

**Bottom line: the guest kernel's own `virtio-pci` + `virtio_console` drivers attach the device,
and the kernel's own log comes out of it.** Same device, same BARs, same module set, same
interrupt line and the same interrupt count on both machines, and — the strongest form of the
claim — a byte fed in from the host is read back by the guest through `/dev/hvc0` on both:

```
   the pinned reference (npm v86 0.5.471+g94faf3d)          this port (build/v86c)
   pci 0000:00:0c.0: [1af4:1043] type 00 class 0x000200      (identical)
   pci 0000:00:0c.0: BAR 0 [io  0xb800-0xb83f]               (identical)
   pci 0000:00:0c.0: BAR 1 [io  0xb900-0xb90f]               (identical)
   pci 0000:00:0c.0: BAR 2 [io  0xb700-0xb70f]               (identical)
   pci 0000:00:0c.0: BAR 3 [io  0xb600-0xb60f]               (identical)
   printk: console [hvc0] enabled                            (identical)
   /proc/interrupts  11:  26  XT-PIC  virtio0                (identical, same count)
   /proc/modules: virtio_pci, virtio_pci_modern_dev, virtio_pci_legacy_dev,
                  virtio_console, virtio_ring, virtio        (identical, same order/sizes)
   /proc/devices:  229 hvc, 248 virtio-portsdev              (identical)
   GUEST-INIT: read back from /dev/hvc0: [VIRTIO-INPUT16]    (identical)
```

`11: 26 XT-PIC virtio0` is the machine's own interrupt counter after the same boot, and the two
legs agree on it exactly; the module list, the four IO BARs and the console bring-up line are the
*kernel's* reading of the device, not this port's model of it.

**Two findings, each a real defect found by the gate** (§4):

1. **This port's own dangling pointer** — the notification capability's field table lived in a
   *local* of the constructor while the io-port slots kept pointers into it. A guest `outw` to
   the notify port jumped through the stale stack memory: a host `SIGSEGV` at a guest address
   (§4.1).
2. **THE FIRMWARE MUST ROUTE PCI INTERRUPTS** — v86's `raise_irq` reads the PIIX3's PIRQ router
   (config 0x60-0x63) and Linux reads the device's interrupt-line byte (0x3C) for
   `request_irq`; upstream's own config space leaves **both** at 0, so every PCI interrupt is
   delivered on **IRQ 0, the PIT's**. With the routing left alone the driver attaches and then
   **never receives an interrupt** (MEASURED: the mutated-firmware boot in
   `results/m2-4-mut.txt` gets 0 bytes out of the console, no `virtio0` interrupt line and no
   `hvc0` console at all). `tests/minibios.asm` now does what seabios does, for the one device
   that needs it, and leaves every other route exactly as M2-3 measured it (§4.2).

All gates green: `make` (clean), `make selftest` (**541** hw checks, 0 failures — 3522 + 228 +
541), `make nasm` (118/118), `make transcript-diff` (**EMPTY**), `make oracle`,
`make determinism`, plus the M2-2 and M2-3 boots re-run (G4). The asymmetry rig
(`results/m2-4-mut-rig.sh` → `results/m2-4-mut.txt`): **eight** mutations, each alone, each
rebuilt, each required to fail, then restored and required green — seven against the selftest and
one against the live boot (the firmware's routing). Raw evidence: `results/m2-4-gates.txt`, the
two gate transcripts (`results/m2-4-{c,js}-serial.txt`, `results/m2-4-{c,js}-virtio.txt`) and the
leg diff (`results/m2-4-diff.txt`).

---

## 1. WHICH VIRTIO THIS IS: MODERN (VIRTIO 1.x), NOT LEGACY 0.9.5

The brief asks which revision upstream models, with the evidence. It is **modern, and
non-transitional**. The upstream evidence, each item checkable in the reference:

| evidence | where |
| --- | --- |
| the device id is **0x1043** — the console's *modern* PCI id (the transitional/legacy id would be 0x1003) | `virtio_console.js:67` |
| the PCI **status** word has the capabilities-list bit (0x10) and a vendor-capability linked list hangs off 0x34 | `virtio.js:181-183`, 713 |
| the four capabilities (`COMMON`/`NOTIFY`/`ISR`/`DEVICE`) and the 0x40-terminated PCI-CFG terminator | `virtio.js:302-311`, 865-908 |
| the feature negotiation runs through the common configuration structure's two select registers, **not** the legacy BAR0 layout | `virtio.js:321-375` |
| the queue addresses are the 64-bit `queue_desc`/`avail`/`used` dwords in the common structure, **not** the legacy PFN register | `virtio.js:548-600` |
| **and upstream asserts its own precondition**, verbatim: `dbg_assert(options.common.features.includes(VIRTIO_F_VERSION_1), "VirtIO device<...> only non-transitional devices are supported")` | `virtio.js:262-263` |

`VIRTIO_F_VERSION_1` is bit **32** and required: the port keeps upstream's assertion as a
construction-time `DBG_ASSERT` (it cannot be reached by a guest). MEASURED on the wire, in both
legs' gate transcripts: the kernel's own driver reads the feature dwords and reports the device
class line `[1af4:1043]` and the four IO BARs above — a legacy driver could not have found the
device at all, because a modern-only id is not one of the 0x1000-0x103F transitional ids.

## 2. THE FILES

| C file | lines | upstream | lines | what it is |
| --- | --- | --- | --- | --- |
| `src/hw/virtio.c` | 1418 | `v86/src/virtio.js` | 1517 | the transport: the PCI function and its four capabilities, the feature negotiation and device-status handshake, the virtqueues (descriptor tables, avail/used rings, buffer chains), the notification path and the ISR |
| `src/hw/virtio.h` | 376 | — | — | the mapping, the MODERN-virtio evidence above, the two bounds this port adds (queues, io-port field slots), and what is not ported |
| `src/hw/virtio_console.c` | 491 | `v86/src/virtio_console.js` | 301 | the console: ten queues, four ports, the device-specific configuration, the control-message protocol, and the host seam |
| `src/hw/virtio_console.h` | 123 | — | — | the mapping, the one bus-shape divergence (D-VC1), the observation surface |
| `src/hw/machine.{c,h}` | +29 | `cpu.js:1253-1256` | — | `machine_config.virtio_console` (the reference's `settings.virtio_console`) and the device in the device set, in cpu.js's order (after the IDE, before the PIT) |
| `src/main.c` | +105 | — | — | the boot mode's `--virtio-console FILE` (enables the device and names its output sink) and `--virtio-console-in FILE` (the inbound half), plus the byte counters |
| `src/hw/hw_selftest.c` | +343 | — | — | the `virtio` group (96 checks: 445 -> 541) and its host-seam listener |
| `tests/minibios.asm` | +72 | — | — | `pci_irq_route`: the firmware's PCI interrupt routing (§4.2) |
| `tests/mkinitramfs.py` | 108 | — | — | a reusable Alpine initramfs builder (the M2-3 image's builder was not kept) |
| `tests/init-virtio.sh` | 117 | — | — | the virtio gate's guest init |
| `oracle/run-boot.mjs` | +47 | — | — | `--virtio-console`, `--virtio-console-in` for the reference leg |
| `Makefile` | +4 | — | — | `virtio.c`/`virtio_console.c` in the device-layer lists |

`v86/` was read only. `src/cpu/` was **not touched** — unlike M2-2 and M2-3, this unit found no
reason to.

## 3. THE JS → C MAPPING, AND WHAT HAS NO SEAT

The port is a transcription with the JS receiver dropped (`VirtIO.prototype.raise_irq` →
`virtio_raise_irq`, `VirtQueue.prototype.pop_request` → `virtqueue_pop_request`), so every
function can be read side by side with `virtio.js`. The decisions worth stating:

* **The io-port callbacks replace the JS closures.** Upstream builds a capability's field list
  out of objects with `read`/`write` closures over the device (`virtio.js:330-601`); `io.h`'s
  callbacks take `(device, port, value)`, so a field becomes a `virtio_cap_field_t` whose
  callbacks take a **`virtio_field_slot_t`** — the object the port map is registered with. The
  slot carries the device, the field and the field's **index**, which is what the notification
  handlers need: upstream's notify handlers receive the value the driver wrote (= the queue
  index) at the register selected by the field's offset, and the index is what selects the
  handler.
* **One live chain per queue.** Upstream allocates a `VirtQueueBufferChain` object per pop;
  the C port embeds one chain in each queue, which is safe because every call path in the
  ported devices pops, consumes and pushes before it pops again (stated in `virtio.h`, not
  assumed).
* **The guest-memory views.** `cpu.read_blob` (a `mem8.subarray` with two assertions) becomes
  `virtio_guest_read_blob`, which returns the address and keeps both assertions; `cpu.write_blob`
  is `memory.c`'s existing helper, which M2-1 added for the DMA controller and named as the
  helper this unit would want.
* **`dbg_log` has no seat** (the M1b-1 rule): every `LOG_VIRTIO` line is dropped, including the
  DEBUG-only read/write wrappers `init_capabilities` installs around every field
  (`virtio.js:784-808`), which log and then call the same callback. **`V86C_LOG_VIRTIO`** is this
  port's addition (io.h D1's / ide.c's pattern): every field access prints its capability, field,
  value and width to stderr. It decides nothing.
* **`get_state`/`set_state` are not ported** (no state-save machinery in this port; `uart.h`'s
  precedent). One field exists *only* for a restored state — `VirtQueue.fix_wrapping`
  (`virtio.js:1105`) — and is kept with its honest current value, `false` forever, because
  nothing can set it; the counting path that reads it is ported as upstream writes it.
* **The upstream quirks that are transcribed rather than tidied**, each named at its site:

| quirk | evidence | what the port does |
| --- | --- | --- |
| `on_driver_ok` is **unreachable**: the condition is evaluated *after* `device_status = data`, so `data & ~data` is always 0 | `virtio.js:439-444` | kept verbatim (the console passes a no-op; the balloon's real callback would never run — an upstream finding, not fixed here) |
| the device-specific capability asserts `~options.offset & 0x3` on a member its own typedef has **not got**, i.e. `~undefined & 3` = 0: the assertion *fails* in a debug build and is compiled out of the release | `virtio.js:691-692`, the typedef at 140 | the port asserts the offset it actually uses (0) and names the dead assertion |
| the `has_passed` computation in the EVENT_IDX arm of `flush_replies` is **unused** — the `if` around the IRQ is commented out | `virtio.js:1223-1237` | transcribed as it stands, with the value kept |
| the port arithmetic for a port's output message is `(queue_id-3) >> 1`, which labels port 1's queue as 0, 2's as 1, 3's as 2 | `virtio_console.js:94` (MEASURED by evaluating it) | transcribed: it changes a host-visible **label** (the message name), never the bytes; port 0 — the one `virtio-console0-output-bytes` names — takes the false arm and is unaffected |
| the second `Ack` of the same descriptor chain in the PORT_READY and PORT_OPEN arms (the chain is staged into the used ring twice) | `virtio_console.js:137` vs 146, 152-153 | transcribed (the gate compares the two machines); the live gate shows a real driver is not disturbed by it — 26 interrupts and a completed handshake on both legs |
| `dbg_assert(false, …)` arms: the wrong-queue notify, the unknown control event, the emergency write | `virtio_console.js:108`, 118, 159, 199 | each becomes the **release's observable behaviour**: where a `return` follows the assertion the return is kept (it is real control flow); where the assertion is the only statement nothing happens, with a note under `V86C_LOG_VIRTIO`; the emergency write has no effect, exactly as upstream's release |
| `update_config_generation` and `needs_reset` have **no caller anywhere** upstream (grep) | `virtio.js:997`, 1018 | ported for parity, exercised by the selftest so neither is an untested path |

## 4. THE FINDINGS (each measured; each cost a real run)

### 4.1 This port's dangling pointer: a constructor local kept as a live field table

`virtio_init` builds the notification capability's field table. The first version kept it in a
**block-local array** while `init_capabilities` stored `&fields[i]` in every io-port slot —
exactly what upstream's closures do *and can do*, because JS's objects are garbage-collected and
outlive the constructor. In C the pointer dangles the moment the constructor returns.

MEASURED, and it is the strongest kind of evidence: the guest's `outw` to the notify port
`0xB904` **jumped through the stale stack memory** — a host `SIGSEGV` whose faulting address WAS
the instruction pointer (`RIP=0xc25d3edc`, a guest kernel address), with `field_write16` as the
return address on the stack and `0xB904` in the argument registers. It took a `SA_SIGINFO`
handler plus an ELF-symbol resolver over `build/v86c.com.dbg` to name the frame. The table now
lives in `virtio_device_t` (`virtio.h`, with the measurement in the comment).

### 4.2 THE FIRMWARE MUST PROGRAM THE PCI INTERRUPT ROUTING, or the driver never gets an IRQ

**This is the unit's central finding, and it is the same *class* as M2-3's §4.2** (the firmware
is the seam; a kernel's silence is not evidence of a device's health).

Two bytes decide whether a PCI interrupt ever reaches a Linux driver here, and **neither is
written by anything but a BIOS**:

* **The device's interrupt-line byte (config 0x3C).** Linux's enumeration reads it
  (`pci_read_irq`: the pin from 0x3D, then the line from 0x3C) and that value is `dev->irq` —
  i.e. what the driver passes to `request_irq()`. Upstream v86 leaves it 0.
* **The PIIX3's PIRQ router (the ISA bridge's config 0x60-0x63).** v86's `raise_irq` computes
  the vector a device's interrupt is *delivered* on from
  `isa_bridge_space8[0x60 + parent_pin]`, `parent_pin = (pin + device) & 3`
  (`pci.js:581-583`, and this port's `pci_raise_irq`). Upstream's ISA bridge config space is 64
  bytes and **zero beyond 0x3F**, so every PCI device is delivered on **IRQ 0 — the PIT's**.
  seabios programs these registers as part of its PCI bring-up, and that is the only reason the
  browser lane's interrupts work.

**MEASURED, both ways.** With the routing left alone, the reference's and the port's kernels both
*attach* the driver (the handshake's first event goes through) and then the console stalls: the
console path waits, in `__send_to_port` (`drivers/char/virtio_console.c`), for the host to consume
each buffer, and only the notify-interrupt path (or a re-queued buffer) ever wakes it. The
mutated-firmware boot in `results/m2-4-mut.txt` (M8) is the asymmetry: **0 bytes out of the
console, no `virtio0` line in `/proc/interrupts`, no `console [hvc0] enabled`** — against 2391
bytes, `11: 26 XT-PIC virtio0` and the console line when the routing is programmed.

`tests/minibios.asm`'s `pci_irq_route` does what seabios does, and does it **narrowly and
provably**:

* it **probes first** (an absent device answers -1 to a config read — `pci_query`'s own arm), so a
  machine with no virtio device gets the reads and **no writes**: M2-2's boot is byte-for-byte
  unaffected;
* it writes **the same vector to both places** — IRQ 11 to 00:12.0's byte 0x3C and to the PIIX3's
  byte 0x63 (PIRQD, which `(pin + device) & 3` selects for device 12's INTA);
* it leaves **every other route alone**: 0x60-0x62 stay 0, so the IDE controller keeps the exact
  routing M2-3 measured (its driver asks for the hardcoded IRQ 14, the device's interrupts still
  land on IRQ 0, and `libata` works by polling — M2-3's `/proc/interrupts` line `14: ata_piix 0`).

The selftest asserts both states through the config space itself (the device's 0x3C and the
bridge's 0x63 are 0 upstream, 11 after the firmware's two writes) and then asserts that the same
device interrupt **reaches the PIC on IRQ 11** — the slave chip's bit 3 — where it used to arrive
on IRQ 0.

### 4.3 What the guest kernel did with it, in one boot

From `results/m2-4-c-serial.txt` / `results/m2-4-js-serial.txt` (identical on both legs; the
kernel's own lines):

```
   pci 0000:00:0c.0: [1af4:1043] type 00 class 0x000200     the kernel's enumeration
   pci 0000:00:0c.0: BAR 0 [io  0xb800-0xb83f]              the common config: 56 fields, 64 bytes
   pci 0000:00:0c.0: BAR 1 [io  0xb900-0xb90f]              ten notify registers, multiplier 2
   pci 0000:00:0c.0: BAR 2 [io  0xb700-0xb70f]              the ISR
   pci 0000:00:0c.0: BAR 3 [io  0xb600-0xb60f]              cols/rows/max_nr_ports/emerg_wr
   printk: console [hvc0] enabled                           the KERNEL's console came up on it
   /proc/devices:  229 hvc      248 virtio-portsdev
   /proc/interrupts:  11:  26  XT-PIC  virtio0
   /proc/modules: virtio_pci, virtio_pci_modern_dev, virtio_pci_legacy_dev,
                  virtio_console, virtio_ring, virtio
```

and, through the device in the other direction:

```
   GUEST-INIT: written to /dev/hvc0 by the guest (userspace write through the device)
   GUEST-INIT: read back from /dev/hvc0: [VIRTIO-INPUT16]
```

The gate's command line carries **`console=hvc0 console=ttyS0`** deliberately: ttyS0 is the
preferred console (`/dev/console`), so the guest's report is comparable between the legs, and
hvc0 is the virtio console — where the kernel's **own** log lines arrive as well (the
`CON_PRINTBUFFER` dump at registration and every later message). **A second, stronger run with `console=hvc0` ALONE** (`results/m2-4-hvconly-virtio.txt`,
captured by `--virtio-console`; the same boot with `--serial results/m2-4-hvconly-serial.txt`)
shows the kernel's log arriving where **no other console exists at all**: ttyS0 receives **30
bytes** and the virtio console **2154**, among them

```
   [    9.536053] Kernel command line: console=hvc0
   [  259.032452] Warning: unable to open an initial console.
   [  511.372857] printk: console [hvc0] enabled
   GUEST-INIT: read back from /dev/hvc0: [VIRTIO-INPUT16]
```

— i.e. the kernel had **no** console until the virtio one came up, and every kernel message
after that (and the `CON_PRINTBUFFER` dump of everything before it) came out of the device.

### 4.4 The inbound direction's two bounds, both upstream's and both measured

* **At most one posted receive buffer per message.** Upstream's input path DROPS a byte that
  arrives with no posted receive buffer (`virtio_console.js:213-215`'s `//TODO: Buffer`), and the
  driver posts exactly as many buffers as the queue holds (`fill_queue` adds until `add_inbuf`
  fails; the console's queue size is 16). MEASURED: a first run of this gate offered 32 bytes and
  the guest received **16** (the device's chain-write total rose by exactly 16); the read then
  blocked forever and the machine halted. The gate therefore offers what the queue can take.
* **A write longer than the chain's capacity is truncated** by `set_next_blob`, exactly as
  upstream: asserted in the selftest (16 bytes offered into an 8-byte descriptor writes 8).

### 4.5 The read side of a console tty, and why the gate holds it open

`/dev/hvc0` is a canonical-mode tty: a line read completes on the newline, and — MEASURED — the
host answers the guest's sentinel *inside the guest's own `out` instruction* (the notification is
synchronous), so the bytes arrive while the one-shot write fd is the tty's only open and are
discarded when it closes. The gate's init opens the read side first (`exec 3< /dev/hvc0`), writes
the sentinel, then reads from the held-open fd. Without that, the first three runs of this gate
blocked at the read and the machine halted at the HLT.

## 5. THE GATES

### G1 — the external oracle: the guest kernel's own virtio driver, both machines

Machine, both: 80 MiB, the same firmware bytes (`build/minibios-80m.bin`), the same initramfs
(`tests/images/guest-initramfs-virtio.img`) and the same command line
(`console=hvc0 console=ttyS0`), 3,000,000,000 instructions.

```
port:      ./build/v86c --bzimage tests/images/vmlinuz-i386 3000000000 \
               --cmdline "console=hvc0 console=ttyS0" \
               --initrd tests/images/guest-initramfs-virtio.img --bios build/minibios-80m.bin \
               --memory 80 --virtio-console results/m2-4-c-virtio.txt \
               --virtio-console-in results/m2-4-virtio-in.txt --serial results/m2-4-c-serial.txt
reference: node oracle/run-boot.mjs tests/images/vmlinuz-i386 build/minibios-80m.bin 3000000000 \
               "console=hvc0 console=ttyS0" --initrd tests/images/guest-initramfs-virtio.img \
               --memory 80 --virtio-console results/m2-4-js-virtio.txt \
               --virtio-console-in results/m2-4-virtio-in.txt > results/m2-4-js-serial.txt
```

| measurement | the reference | this port |
| --- | --- | --- |
| the device's PCI line | `[1af4:1043] type 00 class 0x000200` | identical |
| the four IO BARs | `b800-b83f`, `b900-b90f`, `b700-b70f`, `b600-b60f` | identical |
| the modules the kernel loaded | virtio_pci, virtio_pci_modern_dev, virtio_pci_legacy_dev, virtio_console, virtio_ring, virtio | identical (same order, sizes and load addresses) |
| the console bring-up | `printk: console [hvc0] enabled` | identical |
| the interrupt | `11:  26  XT-PIC  virtio0` | identical, **same count** |
| the device nodes | `/dev/hvc0` … `/dev/hvc7`, `/sys/bus/virtio/drivers: virtio_console virtio_rproc_serial` | identical |
| the outbound bytes | `virtio_console_bytes: 2689` | `2391 bytes out` (the difference is exactly the reference's 7 VGA lines + its timing lines below) |
| the inbound handshake | `virtio_console_in_sent: true`, `read back … [VIRTIO-INPUT16]` | `15 bytes in`, the same read-back line |
| the machine's end state | `in_hlt=0`, `exception=null`, 3,000,003,489 instructions, **eip 0xc1928515** | `in_hlt=0`, no exception, 3,000,010,181 instructions, **eip 0xc1928515** |

**Every divergence, with its cause** (`results/m2-4-diff.txt`: 65 lines of the serial
transcripts, of which 7 are the reference's VGA device and the rest are clock-derived timings;
the *whole* virtio console stream diff is 23 lines and every one of them is in those two
classes — the counts move by a line or two between runs because the timing lines are the
guest's own; two kinds and no others):

| # | divergence | cause (MEASURED) |
| --- | --- | --- |
| D1 | the reference's machine **has a VGA** at 00:12.0 (`[1234:1111]`, an 8 MiB LFB, `vgaarb` lines, `Console: colour EGA`); the port's has none and says `colour dummy device` | the port DEFERS THE VGA (M2-1's carve-out: the 0xA0000-0xBFFFF window and the 8 MiB LFB read 0, MEASURED against the reference). Not this unit's, and named here because the same boot shows it |
| D2 | the guest timestamps and a few line orders/timings differ (`quirk_… took N usecs`, `hrtimer: interrupt took …`, the count of GSI/`sched_clock` lines) | M2-1F's D-F2 clock policy: the port's guest time is instruction-driven (1 instruction = 1 µs), the reference's is the host wall clock. The SEQUENCE of the virtio facts is identical; the two legs even agree on the interrupt COUNT (26) |
| D3 | the outbound byte totals differ (2391 vs 2689) | D1 + D2 and nothing else: the reference's stream carries the 7 VGA lines and its own timing lines. (An EARLIER pair also differed in the guest's busy-wait round count — `waited 14 round(s)` vs `15` — which is the same clock dependence; in the final captured pair both legs say 14 and every non-VGA, non-timing line is identical) |

### G2 — the selftest, and the proof that it can FAIL

`make selftest` → `hw_selftest: 541 checks, 0 failures, 0 skipped` (445 → 541; the new group is
96 checks). It drives the device **through the CPU's own `out`/`in` instruction bodies**
(`instr_E6`/`E4`, `instr16_ED`/`EF`, `instr32_ED`/`EF`) against the port numbers the capability
structure names, so every assertion is something a guest can observe:

* the machine's device set with and without `machine_config.virtio_console` (and, without it,
  0xB800 is an **unclaimed** port answering 0xFF);
* the PCI function: vendor/device/substatus, the capabilities-list bit, the subsystem ids, INTA,
  and the four bars with their exact ports;
* the **capability chain**, walked as a driver walks it: each entry's vendor id, next pointer,
  type, bar, offset and length — including the notification capability's extra dword
  (`notify_off_multiplier = 2`) and the PCI-CFG terminator;
* the device-specific configuration (cols 80, rows 25, `max_nr_ports` 4);
* `num_queues` 10, both device-feature dwords (F_SIZE|F_MULTIPORT; VIRTIO_F_VERSION_1 at 32), an
  out-of-range select reading 0, and the **8-bit shim on a 32-bit field**;
* the feature negotiation's two halves: an unoffered bit is masked out of `driver_feature` **and**
  FEATURES_OK is refused (and a write of 0 is a reset);
* the queue: size, notify offset, `queue_enable` refused before the addresses are set, the four
  dwords, the ignored high dword, and a queue index the device has not got reading as unavailable;
* **a descriptor chain the guest posts is CONSUMED**: its three bytes leave through the host seam
  (`virtio-console0-output-bytes`), the used ring carries the head index and the length, and the
  ISR register returns and clears `VIRTIO_ISR_QUEUE`;
* **a byte fed in reaches the guest**: it lands in the descriptor's buffer in guest memory, the
  chain is consumed, the used entry's length is 1 — and a second byte with **no posted buffer is
  dropped** (upstream's TODO), and a 16-byte message into an 8-byte descriptor writes 8;
* the IRQ route: upstream's config space (device byte 0x3C = 0, PIRQ byte 0x63 = 0) versus the
  firmware's two writes (= 11), with the same device interrupt asserted to reach the **slave PIC's
  bit 3 (IRQ 11)** afterwards.

**G2's asymmetry: `results/m2-4-mut-rig.sh` → `results/m2-4-mut.txt`, EIGHT single-line
mutations, each alone, each rebuilt, each required to fail, then restored and required green**
(the tree's three sha256s are printed before and after, and they match):

| # | mutation | result when mutated |
| --- | --- | --- |
| M1 | the negotiated-feature mask removed | 1 failure: `vq_ind(0xB80C) & 0x800 == 0` |
| M2 | the FEATURES_OK refusal removed | 1 failure: FEATURES_OK stayed set |
| M3 | the chain's written-length accounting zeroed | 2 failures: the used entry's length |
| M4 | the WRITE descriptor flag ignored | 8 failures: the fed-in byte never lands |
| M5 | the flush's ISR bit changed to DEVICE_CFG | 2 failures: the ISR register |
| M6 | the bar-size rounding disabled | 1 failure: the bar size a driver's probe reads |
| M7 | the notify register's index ignored | 12 failures: nothing comes out of the seam |
| M8 | **the firmware's PIRQ-routing write deleted** (a live boot, not the selftest) | 0 bytes out, **no `virtio0` interrupt line**, no `console [hvc0] enabled`, no read-back |

### G3/G4 — the project's gates, raw in `results/m2-4-gates.txt`

| gate | result |
| --- | --- |
| `make clean && make` | rc=0 (from scratch) |
| `make selftest` | rc=0 — `cpu_selftest: 3522 checks, 0 failures` (**unchanged**), `cpu_m1c_selftest` (228), `hw_selftest: 541 checks, 0 failures, 0 skipped` (445 → 541) |
| `make oracle` | rc=0 — the M0 transcript still matches `tests/model.py` |
| `make transcript-diff` | rc=0, diff **EMPTY** — the M0 machine is byte-identical to the reference's (the virtio console is NOT in it: the reference creates it only for `settings.virtio_console`) |
| `make determinism` | rc=0 — two consecutive reference runs byte-identical |
| `make nasm` | rc=0 (118/118) |
| G4a: the M2-2 boot | rc=0 **both legs**, 1000000000 instructions: the kernel panics (`2` panics, 4 `BIOS-e820` rows, 294 lines), both legs end at the SAME `eip=0xc1927800 cr0=0x80050033 cr3=0x1ea1000`, and there are **zero** virtio lines (no `--virtio-console`) — no virtio device exists, so this boot is exactly M2-2's. The C-versus-reference diff is the VGA (D1) and the clock (D2) and nothing else |
| G4b: the M2-3 boot | rc=0 **both legs**, `--hda tests/images/disk.img --initrd tests/images/guest-initramfs.img --memory 80`: the kernel still discovers, identifies and reads the disk — `ata1.00: ATA-0: v86 ATA HD, 1.00, max MWDMA2`, `8192 sectors`, `sd 0:0:0:0: [sda]`, `sda: sda1`, `/proc/partitions: 8 0 4096 sda / 8 1 1024 sda1`, and `dd | md5sum` = **87d5ec3151caf04d90236947e6f09874**, the same md5 M2-3 recorded |

## 6. THE INSTRUMENTS THIS UNIT ADDED

* **`V86C_LOG_VIRTIO`** (`src/hw/virtio.c`) — every capability field access with its capability,
  field name, value and width, on stderr. It DECIDES NOTHING. It is what turned "the guest's
  driver does not touch the device" into "the driver stops after the first control event".
* **the byte counters** — `--virtio-console` prints `N bytes out, M bytes in, K chain bytes read,
  L chain bytes written` at the end of a boot, and the device keeps the same totals
  (`virtio_console.h`). This is the port's form of the reference harness's own
  `virtio_console_bytes`, and it is how §4.4's 16-of-32 measurement was made.
* **the host-fault report** (`src/main.c`) — `SA_SIGINFO` plus a symbol resolver over
  `build/v86c.com.dbg` is what named §4.1's frame. It reports a PORT defect and the guest state
  at the moment it happened; it changes no guest-visible value.
* **`tests/mkinitramfs.py`** — the M2-3 initramfs's builder was not kept, and this unit needed a
  SECOND image (the Alpine netboot kernel has no built-in virtio driver either: 3 `virtio`
  strings in the 16.3 MiB decompressed kernel, none of them `virtio_pci` or `virtio_console`).
  The M2-3 image is untouched: its sha256 is pinned in `results/m2-3-ide.md` and G4b re-runs that
  gate with it.

## 7. DIVERGENCES FROM UPSTREAM, DELIBERATELY, AND WHAT THEY COST

| site | divergence | reason / guest-visible cost |
| --- | --- | --- |
| `src/hw/virtio_console.h` (D-VC1) | upstream's `virtio-console<N>-output-bytes` message carries a `Uint8Array`; this port's bus carries an int32 argument VECTOR, so the port's console sends **one message per byte** at the same message name (and its input takes its bytes the same way) | the byte STREAM is identical and in the same order; only the framing differs. A listener that would have written upstream's array to a file gets the same file |
| `src/hw/virtio.c` | the io-port slots hold function-table pointers and must outlive the constructor: the notify field table lives in `virtio_device_t` | §4.1: the alternative is a dangling pointer (MEASURED as a host SIGSEGV) |
| `src/hw/virtio.{c,h}` | bounds upstream has not got: `VIRTIO_MAX_QUEUES` (16), `VIRTIO_FIELD_SLOT_MAX` (64), `VIRTIO_MAX_CHAIN` (64) | each is a construction-time refusal with a named message (a C array cannot read past its end the way a JS array returns `undefined`) |
| `src/hw/virtio_console.c` | a notify value that is not a queue index is named and IGNORED where upstream indexes `queues[queue_id]` (an out-of-range read) | the only check this file adds; a driver writes the queue index there |
| `src/hw/virtio.c` | `dbg_assert(false, …)` arms become the release's behaviour plus a diagnostic (§3) | upstream's release drops them; the port names them instead of aborting the machine |
| `src/hw/virtio_console.c` | a bounded input message (`VIRTIO_CONSOLE_INPUT_MAX` 256) | upstream's message is an unbounded `Uint8Array`; a longer message's tail is dropped exactly as upstream drops what does not fit the chain |
| `src/hw/virtio.{c,h}` | no `get_state`/`set_state`, no `dbg_log`, no DEBUG wrappers | no state-save machinery and no log channel in this port (`uart.h`'s precedent) |
| `src/hw/virtio.c` | two C-specific care items, both value-identical to upstream: the queue-size rounding writes `1 << ((int_log2(size-1) + 1) & 31)` (JS masks a shift count to 5 bits; C's `1 << 32` is undefined behaviour — a 32-bit guest writing 0xFFFFFFFF hits it), and the control-packet buffer is `memset` to zero (upstream allocates a zero-filled `Uint8Array`, so a short control chain reads zeros there and must not read a stack buffer's previous contents) | neither changes a guest-visible value; the first removes UB, the second removes an uninitialised read |
| `tests/minibios.asm` | `pci_irq_route`: the two PCI interrupt-routing writes | §4.2 — a real BIOS's job, and the reason no interrupt reached the driver. Narrow, probed, and the only route the port touches |
| `src/main.c` | `--virtio-console FILE`, `--virtio-console-in FILE` (boot mode only) | the M0 oracle contract's two-argument form is untouched; the transcript mode's machine still gets **no** virtio console, as the reference's default machine does |
| `oracle/run-boot.mjs` | `--virtio-console`, `--virtio-console-in`; the output is written with `fs.writeSync` | the reference leg takes the same two settings. The synchronous write is a measured requirement: `process.exit(0)` discarded a `createWriteStream`'s buffered bytes and left **one byte of 2689** on disk while reporting all 2689 |

## 8. WHAT IS DEFERRED, AND WHAT A GUEST WOULD OBSERVE

* **VIRTIO-BLK, VIRTIO-NET, VIRTIO-9P, VIRTIO-BALLOON ARE NOT PORTED.** The transport is generic
  (the console proves the four capabilities, the queues and the handshake end to end), but
  nothing registers a second device. A guest sees a machine with the console and no other virtio
  function — which is exactly what the reference's default machine has, and what this unit's
  consumer (baliveau's v86 lane) names as what it needs.
* **The VGA is still deferred** (M2-1's carve-out), so the reference's `Console: colour EGA` is
  this port's `colour dummy device` (§5, D1). A later unit's; named here because the same boot
  shows it.
* **The APIC/IOAPIC are still deferred**, so `device_raise_irq` stops if a machine has
  `acpi_enabled` set (M2-1's policy, unchanged). The gate's machine does not.
* **`virtio_console`'s `F_EMERG_WRITE` is NOT offered** (upstream's own choice,
  `virtio_console.js:73-78`), so a driver that negotiates like Linux's never writes the
  `emerg_wr` field; the field exists and its write is a no-op with the assertion named (§3).
* **No CLI flag attaches a second console port's host sink**: `virtio-console1..3-*` are
  registered on the bus (upstream's own per-port model) and nothing listens to them, so ports
  1-3's bytes go nowhere — which is what upstream's browser build does when only port 0 has an
  adapter.

## 9. HANDOFF NOTES

* **The firmware is the seam again, and this time for INTERRUPTS.** Any later unit whose device
  needs to interrupt the guest must check what a BIOS programs before concluding the device model
  is wrong: the PIRQ router (0x60-0x63) and the device's interrupt-line byte (0x3C) are both
  firmware's, and upstream leaves both at 0 — i.e. **IRQ 0, the PIT's, for every PCI device**.
  M2-3's firmware lesson (the IDE-enable bits) and this one are the same lesson twice.
* **A console device's tx path works WITHOUT interrupts; its rx path does not.** `put_chars`
  busy-waits on the used ring (`__send_to_port`), so log output flows even with no IRQ; the
  inbound direction needs the notify-interrupt path. A gate that only pushes bytes INTO the guest
  cannot tell those apart — this unit's first three runs did not (they blamed the tty).
* **`V86C_LOG_VIRTIO` is the instrument to reach for**, and the host-fault `SA_SIGINFO` report
  plus the `.com.dbg` symbol table is what localises a port fault: the faulting *address* being
  the *instruction pointer* is the signature of a jumped-through function pointer, not a data
  fault.
* **For the baliveau lane (the consumer this unit serves):** `/dev/hvc0` now works — the kernel's
  own driver binds, the kernel's console comes up on it, and the host can write to the guest's
  console and read it back. Two caveats are upstream v86's, not this port's: the *browser* lane
  gets working PCI interrupts only because seabios programs the PIRQ router (a purpose-built
  firmware must do it too — `tests/minibios.asm`'s `pci_irq_route` is the working example), and
  v86 still has **no virtio-blk**, so a guest's root filesystem must come from the disk image
  (M2-3) or the initramfs.
