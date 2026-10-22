# x86-64: the KASLR divergence — the E820 is the RIGHT SHAPE, and the port's kernel finds slots and rejects one

**Status: MEASUREMENT ONLY, no code change. The E820 the port publishes is essentially QEMU's shape,
so the "no suitable memory region" is not a fragmented-map defect. The port's own serial carries a
line QEMU's does not — `Invalid physical address chosen!` — which says the kernel DID find candidate
slots and rejected the one it picked. That is the next measurement, and it is named.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The map as the guest sees it (MEASURED — READ of `tests/minibios.asm`, and the serials)

`minibios.asm:803-807` publishes **four** E820 entries (the kernel copies 5 dwords = the standard
20-byte entry, and the chain ends when EBX goes to 0):

| # | base | length | type |
|---|---|---|---|
| 1 | `0x00000000` | `0x0009FC00` | 1 (usable) |
| 2 | `0x0009FC00` | `0x00000400` | 2 (reserved) |
| 3 | `0x000F0000` | `0x00010000` | 2 (reserved) |
| 4 | `0x00100000` | `MEM_TOP − 0x100000` | 1 (usable) |

`MEM_TOP` is `0x4000000` (64 MiB) by default and is set per build (`make minibios-mem`,
`as --defsym MEM_TOP=0x5000000`); at `--memory 256` the booted image's fourth entry is the whole
`0x100000..0x10000000` region.

**QEMU's map for the same 256 MiB** (from its serial, MEASURED):

    BIOS-e820: [mem 0x0000000000000000-0x000000000009fbff] usable
    BIOS-e820: [mem 0x000000000009fc00-0x000000000009ffff] reserved
    BIOS-e820: [mem 0x00000000000f0000-0x00000000000fffff] reserved
    BIOS-e820: [mem 0x0000000000100000-0x000000000ffdffff] usable
    … then reserved high regions (0xffe0000, 0xfeffc000, 0xfffc0000, 0xfd00000000)

**The two are the same shape**: one big usable region from 1 MiB to (nearly) the top of RAM, the same
low reserved holes. **So the E820 is NOT too fragmented and is not the defect** — the "no suitable
memory region" cannot be a missing 18 MiB window, because a ~255 MiB contiguous usable region exists
in both.

**The memory-size discriminator is therefore already answered by the QEMU unit's own run:** QEMU at
`-m 256` (the SAME memory the port gets) chose `Kernel Offset: 0x6400000`, i.e. it found a region.
The port at the same memory did not. Memory size is not the difference.

## 2. The line that distinguishes the two paths (MEASURED)

The port's serial (from the runs of the last several units):

    Invalid physical address chosen!
    Physical KASLR disabled: no suitable memory region!

QEMU's serial contains **neither** line. The first is `slots_fetch_random`'s message: the kernel **did**
find candidate slots and then rejected the physical address it picked; only afterwards does it fall
back and report "no suitable memory region". **So the port's kernel is not failing to find a slot — it
is failing a validity check on the address it chose.**

**INTERPRETATION (and the working hypothesis for the next unit):** that check is against the
kernel's own `mem_avoid`/valid-region bookkeeping, which is built from the boot_params the *firmware*
hands over — and the port's minibios and QEMU differ there, not in the E820: the **boot_params
fields** (mem/initrd/cmdline addresses, `hdr.code32_start`, the `init_size`, the `e820_entries` copy)
are the other half of the map, and the port builds its own. That is where the divergence should be
measured next.

## 3. Why no fix is claimed

The brief's rule is the right one, and the measurement does not support a firmware change:

* the E820 is the same shape as QEMU's, so a "proper map" would change nothing that is wrong;
* **a minibios change touches every guest gate**, and the 32-bit machine's transcript is a pinned
  BYTE — the rule says stop and publish rather than move it on a hypothesis;
* the distinguishing line (`Invalid physical address chosen!`) points at the **boot_params**, not at
  the E820.

**The next measurement, named:** dump the boot_params the port hands the kernel (at the address in
`%rsi`, which the toolkit's dump already gives as `0x84790`) and compare field for field against what
QEMU hands the same kernel — `e820_entries`, the e820 table's first entries, `hdr.code32_start`,
`hdr.init_size`, `hdr.pref_address`, and the cmdline pointer. The toolkit makes it a `V86C_CHAIN`-style
read once the address is in hand.

## 4. Verification (MEASURED; **no functional change**)

    make selftest                          rc 0 — all suites green
    tests/gate-x86-64.sh                   rc 0 — 12 passed, 0 failed
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757

`git status` shows nothing under `src/`, `gen/` or `tests/`. No pin moved. The rest of the battery was
not re-run because nothing functional changed — the QEMU unit's full battery on the same tree is the
record for it.

## 5. Files touched

| file | change |
|---|---|
| — | **none.** READ of `tests/minibios.asm`'s E820 and of both serials. |
