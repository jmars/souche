# R6 REVIEW — the UART, the kernel loader, the minibios, and the two src/cpu/ changes made under pressure

**Unit under review:** M2-2 (`HANDOFF=v86c`). **Reviewed:** `src/hw/uart.c`, `src/hw/kernel.c`,
`tests/minibios.asm`, the `rdrand` change in `src/cpu/instructions_0f.c`, the two `src/hw/pci.c`
fixes, and the two report corrections. **Reviewer:** independent pass, read-only, against
`v86/` (the pinned npm release's source, 0.5.471+g94faf3d). No gates re-run.

## Verdict

**No blocker.** The two transcriptions the unit asked to have read are **faithful site by
site**: `uart.c` against `uart.js:143-295` (and its `CheckInterrupt`/`ThrowInterrupt`/
`ClearInterrupt`/`data_received`/`write_data`/`set_modem_status_bit` at 332-455 and the
constructor at 56-114) and `kernel.c` against `kernel.js:42-234` — every field read and
written, every order, every parenthesisation, the loader geometry, and the option-ROM
prologue and checksum. The two PCI fixes are **what upstream does**, not merely what makes
the boot proceed. The rdrand port is **faithful, including the flags**: upstream models no
failure arm at all (`*flags = (*flags & !FLAGS_ALL) | 1; *flags_changed = 0` — always
CF=1, ZF=0), and the C reproduces exactly that. The two report corrections are accurate.

Eight findings follow: one **SHOULD-FIX** and seven **NICE-TO-HAVE**. None touches a gate.

---

## SHOULD-FIX

**S1 — guest-reachable `DBG_ASSERT`s remain in the two config-write functions this unit
edited, where the release reference silently proceeds.** `src/hw/pci.c:286` (8-bit write to
BAR/ROM offsets), `:296`+`:310` (write16 alignment + ROM offsets), `:320` (write32
alignment), `:108` (the CF8 default case) vs `v86/src/pci.js:337-338, 348, 368-369, 379` —
upstream's `dbg_assert` is compiled out of the pinned release wasm, so a guest writing
16-bit config space at 0x30-0x34 (the ROM address register) or 8-bit at 0x10-0x2B **aborts
the shipped machine where upstream proceeds**. Pre-existing M2-1 code, but it sits in the
exact functions this unit changed, and the unit already fixed the same-class machine-stopper
(write32 to an absent device) a few lines away. Review5's S2 established this class and the
fix pattern: drop the asserts, keep upstream's text as comments. No current guest reaches
them (the kernel booted 1.5 G instructions cleanly), which is why this is not a blocker.

---

## NICE-TO-HAVE

**N1 — `rtc_read`'s "update-in-progress wait" is not the mechanism its comment names.**
`tests/minibios.asm:633-650`: the comment claims a bounded wait on *register A bit 7*, but
the loop reads the **target data register** and tests *its* bit 7 — register A (0x0A) is
never read. For every register actually read (BCD sec/min/hr/day/month/year/century all have
bit 7 clear on this host) the wait **never engages**, so a read straddling an RTC update can
tear. Caveat for the fix: even a correct UIP wait cannot make the three reads atomic on this
machine, because the port's RTC time registers read the live host clock per access
(`src/hw/rtc.c:210-233`). Honest fix: either read 0x0A and test bit 7 as the comment claims
(it works — the port models UIP for the last virtual millisecond of each second,
`rtc.c:226-232`), or rewrite the comment to say the reads are unlatched. Guest impact today:
at most a 1 s skew in the kernel's 30 s `getchar_timeout` menu window.

**N2 — the option-ROM scan's coverage claim overstates what it scans.**
`tests/minibios.asm:242-245` + `246-270` (and `oracle/run-boot.mjs:50`, which repeats the
claim): the comment says 0xC0000-0xEFFFF, but the loop keeps `es=0xC000` and advances `bx`
in 0x800 steps while `bx < 0xE000` — actual coverage is **0xC0000-0xCD800**. A ROM placed
above that (still inside the real option-ROM area) would be missed where a real BIOS finds
it. Unreachable today: the host always places the kernel ROM at 0xC0000
(`src/main.c:611,781`, `run-boot.mjs:51,153`). Fix the comment, or cross the segment
boundary (`es += 0x80` per 64 KiB, stop at 0xF000).

**N3 — `vid_ega_config` tests the dispatch's clobbered BL, not the caller's subfunction.**
`tests/minibios.asm:344-346` + `414-418`: `video_dispatch` does `mov bl, ah` to dispatch,
destroying the caller's BL (the subfunction, 0x10) before `vid_ega_config` tests
`cmp bl, 0x10` — the test therefore compares the *function code* (0x12) and can never take
the `xor bx, bx` arm; the "BL=0 → EGA/VGA present" answer the comment describes is
unreachable. Latent for this kernel: `vga_probe` takes the AX=1A00h answer (the firmware
returns AL=0x1A, BL=0x08) and only uses the 0x1200 answer's BL when AL≠0x1A, which never
happens here. Fix: dispatch on AH without destroying BL, or save/restore BL.

**N4 — `kernel.c:242-244` misdescribes upstream's short-image behaviour.** The comment says
upstream "would read an empty slice and load nothing"; in fact
`new Uint8Array(bzimage, 0, prot_mode_kernel_start)` with an out-of-range length **throws
RangeError** (and the byteOffset-only view at `kernel.js:152` throws too). Outcome is
equivalent — both sides fail the load, the C returns false — only the comment is wrong. Say
"throws".

**N5 — the cmdline NUL terminator bypasses the bounds guard every other byte uses.**
`src/hw/kernel.c:238`: `mem8[cmd_line_ptr + strlen(cmdline)] = 0;` writes one byte without
the `write_blob` guard that covers the cmdline's other bytes and both kernel copies. If
`cmd_line_ptr + strlen == memory_size`, the write lands one byte past guest memory (inside
the +16 slack — no crash — but the guard exists precisely for this class; upstream's
`mem8[i]` assignment drops OOB silently). Unreachable in the boot mode (64 MiB vs 0x8E000).
Fix: write `strlen+1` bytes through one `write_blob` call.

**N6 — the corrected rdrand row in M1b-1's report carries a stray fragment from the PCI
correction.** `results/m1b1-0f-nonsse.md:51`: "the tables are initialised for 0..255
(pci.js:37-38 equivalent: …)" makes the 0F C7 /6 correction read as if it were about PCI
tables. The substance is right (reg form = rdrand at `instructions_0f.rs:3996-4013`, mem
form = `trigger_ud()` at 4016-4018, `mem_ud: 1` at `x86_table.js:462`). Report-only: trim
the fragment.

**N7 — `rtc.c`'s "the one nondeterminism left" is stale after this unit's change.**
`src/hw/rtc.c:230-231` was written by M2-1F before rdrand existed; this unit added the
second host-entropy seam (`/dev/urandom`, `instructions_0f.c:6542-6564`), so there are now
two. M2-2 updated the m1b1 and m2-1 reports but not this comment. Suggest "the one
nondeterminism in the clock policy", or drop "the one".

---

## The brief's direct questions, answered

- **Is the rdrand port faithful, including CF/ZF-of-failure semantics?** Faithful — with the
  key observation that **upstream has no failure semantics to port**: `instr16/32_0FC7_6_reg`
  (`instructions_0f.rs:3996-4013`) unconditionally writes the random value, sets
  `flags = (flags & !FLAGS_ALL) | 1` (CF=1, all other arithmetic flags 0) and
  `flags_changed = 0`; the memory form is `trigger_ud()`. The C
  (`instructions_0f.c:6566-6578`) reproduces all three exactly; `FLAGS_ALL` is bit-identical
  to `cpu.rs:119-121`, and `flags_changed = 0` falls through to the materialized `cpu->flags`
  in every lazy getter, exactly as upstream intends.
- **Is `/dev/urandom` the right peer of `js::get_rand_int`?** Yes — measured in the pinned
  build: `libv86.mjs`'s `ca` is `crypto.getRandomValues` / node
  `crypto.randomBytes(4).readInt32LE(0)`, i.e. the platform CSPRNG. `/dev/urandom` is the
  same kind of host-entropy source. The deterministic LCG fallback is a port-only arm,
  declared at the site, reached only when the file cannot be opened/read.
- **Is the nondeterminism declared, and is it the same kind of divergence as D-F2?**
  Declared (code comment `instructions_0f.c:6532-6540`, report §6.1 and §7). It is **not**
  the D-F2 class: D-F2 *replaces* upstream's host-wall-clock behaviour with a deterministic
  virtual clock, whereas rdrand *matches* upstream's already-nondeterministic behaviour. The
  report says exactly this.
- **Are the two pci.c fixes what upstream actually does?** Yes. Upstream indexes tables
  initialised only for 0..255 (`pci.js:37-38`), so any bdf ≥ 256 is `undefined` — an absent
  device; the C's `pci_bdf_is_absent` (bdf < 0 || bdf ≥ 256) is that predicate, and the
  measured SIGSEGV (bdf 0xFF00 on the kernel's bus-255 scan) was the unbounded C read, not a
  semantic difference. A write32 to an absent device is a **silent no-op** upstream
  (`pci.js:384-390`, no dereference before the `if(!space) return`), and the C now ignores it;
  write8/write16 **do** dereference `undefined.buffer` and throw (`pci.js:329,353`) — the
  C's stop is the declared stand-in for a behaviour that has no reference. M2-1's claim was
  half-right and is corrected accurately.
- **Are the two corrections accurate?** Yes: the m1b1 `0F C7 /6` row now states the register
  form is rdrand and the memory form is `#UD` (correct per the source), and M2-1's "a write
  to an absent device stops" is corrected to the width-dependent truth above, inline at the
  claims they fix (plus `src/hw/pci.h`).
- **Is the firmware honest, and does any handler silently succeed where a real BIOS would
  refuse?** Honest, and no. Every unsupported service — EDD (INT 13 all functions), APM
  5300h, MCA C0h, IST E980h, E801 — returns CF=1. The two that could look like silent
  successes are not: A20 reports enabled *because neither machine models an A20 mask* (the
  kernel's own `a20_test_short` passes first), and EC00 is a named no-op. The finding the
  unit recorded — a handler's CF/ZF must be patched into the IRET frame — is correct
  (verified the frame offset: after `push bp`, `[bp+6]` is FLAGS), and the three helpers do
  it properly. The INT 16 "no key ever pending" and the INT 1A raw-BCD passthrough (the
  port's RTC returns BCD, `rtc.c:152-189`) are what they claim. The only dishonest spots are
  the three named above (N1's comment names a wait that doesn't run, N2's coverage claim,
  N3's unreachable "present" answer), none of which changes this kernel's boot.

## Coverage

**Read in full, both sides:** `src/hw/uart.c` (549) vs `v86/src/uart.js` 1-455; `kernel.c`
+ `kernel.h` vs `kernel.js` 1-234; `tests/minibios.asm` (715); `src/hw/pci.c` (495) vs
`pci.js` 1-603; `instructions_0f.c:6505-6578` (rdrand) + 1700-1760 (0F C7 /1 neighbour) +
1130-1140 (cpuid) vs `instructions_0f.rs:3990-4020` + 3270-3290 + the `libv86.mjs` `ca`
definition; `flags.c` getter semantics for `flags_changed = 0`;
`src/cpu/m1c_selftest.c:1054-1108`; `oracle/run-boot.mjs` (232).

**Sampled:** `src/main.c` boot mode (594-830, 400-430); `src/hw/rtc.c:190-240`;
`machine.c` device order + bus pairing; `io.c` write8 dispatch; `cpu_state.c` reset
(166-232); `hw_selftest.c` UART group (1190-1330) + loader-group opening; `Makefile`
78-85, 171-183; `results/m2-2-boot.md` in full; `m2-2-gates.txt` header/shas; the two
corrected report rows; `m2-1f-policy.md` D-F2.

**Could not check:** gate re-runs (forbidden by the brief); the 1.5 G boot itself
(orchestrator-reproduced, not re-run); the concurrently written `src/hw/ide.{c,h}` (M2-3,
ignored per brief).

**Shas reviewed** (stable throughout the pass; match `results/m2-2-gates.txt`):
uart.c `0b435bcf`, uart.h `b8eb897e`, kernel.c `61fceef8`, kernel.h `362286fb`,
minibios.asm `0b0a775f`, pci.c `3cc41b9a`, pci.h `78a821f5`, instructions_0f.c `1dbad114`,
main.c `cdce43af`, rtc.c `eecc16d0`.
