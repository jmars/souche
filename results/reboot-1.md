# REBOOT-1 — the machine reboot path (results/reboot-1.md)

Unit: `HANDOFF=v86c-reboot`, REBOOT-1 (coder). Closes TODO.md §1.4: the one MISSING
behaviour — a guest-initiated reboot stopped the machine by name at two sites. Raw gate
output: `results/reboot-1-gates.txt`. Gate script: `tests/gate-reboot-1.sh` (21 checks;
`REBOOT_KERNEL_ARM=1` adds the real-kernel arm). Mutation rig:
`results/reboot-1-mut-rig.sh`.

## What was built

1. **`machine_reboot()`** (`src/hw/machine.c`, declared in `machine.h`) — the port's
   `cpu.reboot_internal()` (cpu.js:939-965). `reset_cpu()`, then the device resets (see
   the decision table below), then it *requests* the boot re-entry
   (`cpu->reboot_requested`, a new field in `cpu_state_t` beside `pagefault`). It does
   NOT clear the code caches and does NOT re-enter the boot itself — see the cache
   answer; the request/unwind/re-entry split is forced by the port's execution model.
2. **The two trigger sites** now call it, preserving each site's own guard:
   - `src/hw/pci.c` `portCF8_write8` case 1: the 0xCF9 dance — `(addr[1]&6)==2 &&
     (out&6)==6` — calls `machine_reboot(); return;` **without storing the trigger byte**,
     exactly as upstream's handler returns before `pci_addr[1] = out_byte` (pci.js:126-131;
     the header's old pointer said 161-165 — the line numbers moved with the seabios PAM0
     block above it, corrected).
   - `src/hw/ps2.c` `port64_write` case 0xFE (ps2.js:834-837): `machine_reboot(); break;`.
   Both header comments (`pci.h`, `ps2.h`) corrected from "STOPS" to the new contract.
3. **The run path** (`src/cpu/cpu.c`, `src/cpu/uop_cache.c`): the reboot must stop the
   current *batch*, not just the block:
   - `do_many_cycles_native`'s while-condition tests `!cpu->reboot_requested` — this is
     THE unwind backstop, and the first gate run proved it load-bearing: reset_cpu zeroes
     `instruction_counter`, so the old "counter underflow closes the batch" argument is
     false whenever the reboot happens inside the first 100,003 instructions of a boot
     (initial == 0). With the flag in the condition the batch closes at the trigger.
   - `main_loop` returns promptly on the flag (no boot-2 instruction may run before the
     host re-establishes the boot state).
   - `uop_run`'s per-uop loop breaks with a flush on the flag (the remaining uops of the
     block are the pre-reboot stream); the mid-batch fallback path has the same check;
     `interpreted_run` (the `--uop-cache off` loop) breaks on it too.
   - The COMPILED path has no per-uop hook of its own, so `machine_reboot` raises
     `jit_abort_flag` when the JIT is on — the same channel an SMC write uses — and
     `uop_run`'s `JIT_EXIT_ABORT` continuation checks `cpu->reboot_requested` FIRST and
     unwinds, because the SMC continuation re-derives `phys` from the old block's
     geometry, which is garbage for the reset eip (0xFFFF0).
4. **The driver** (`src/main.c`): `run()` breaks on the flag; both boot modes loop
   (bounded by `MAX_REBOOTS` 16, a runaway is named and dies):
   - **multiboot/ELF (the transcript mode)**: `multiboot_set_state(entrypoint)` re-runs —
     the entry state and the bootloader handoff (EAX/EBX, the info word at 0x7C00) are
     rebuilt; the loaded segments and every other guest-RAM byte are NOT touched. Memory
     survives; that is the gate image's property.
   - **bzImage (boot mode)**: the LOADER re-runs — `kernel_load`'s own regions (the setup
     block at 0x80000, the protected-mode kernel at 0x100000, the command line, the
     initrd, the boot params) plus the firmware at 0xF0000 and the option ROM at 0xC0000.
     **A MEASURED DIVERGENCE FROM THE BRIEF'S LITERAL WORDING**, stated rather than
     hidden: the brief said "without re-loading … the disk or the initrd"; the disk is in
     fact never touched (its buffer is boot 1's — the kernel arm proves it), but the
     loader re-run DOES re-write the kernel and initrd bytes from their files, because a
     self-decompressing bzImage DESTROYS its own compressed image in RAM during boot 1
     (the Alpine kernel decompresses over 0x100000): with a firmware/option-ROM-only
     re-entry, boot 2 died at #UD inside the stub's former footprint (MEASURED,
     `exception=6` at eip 0x1038f0). Re-running the loader is also what the reference's
     own harness does on a restart; upstream's `reboot_internal`+`load_bios` instead
     lands a bzImage guest at the BIOS's default boot, which the brief forbids. The
     memory-survival property is carried by the multiboot path's gate image; the disk
     property is carried by the kernel arm.

## The device-by-device reset decision

The reference names exactly four devices (cpu.js:943-958): virtio_9p, virtio_console,
virtio_net, ps2. Only the console (when configured) and the PS/2 exist here. THE PORT
RESETS MORE THAN THE REFERENCE NAMES, and the reason is firmware, not preference: the
reference's boot re-entry re-runs SEABIOS, whose POST reprograms the PIC, the PIT and
COM1 before any guest code runs; this port's firmware (tests/minibios.asm) re-runs too
but programs NONE of them (it hands the kernel the PIC all-unmasked at first boot and
the kernel masks it itself, pm.c:36-42). Without a reset, boot 2 runs the firmware under
boot 1's interrupt regime — a stale remap plus any line a raiser still drives delivers
into the firmware's IVT.

| device | reset? | why |
|---|---|---|
| CPU (`reset_cpu`) | YES | the reference's own first step; the reset vector lands at 0xFFFF0 |
| PIC (`pic_reset`, new) | YES | boot 1's remap/IMR must not reach boot 2's firmware; `pic.rs`'s initial state is all-unmasked with irq_map 0, so boot 1's own regime differs from the reset regime |
| PIT (`pit_init`) | YES | boot 1's tick rate must not fire into boot 2's bring-up |
| RTC (`rtc_init`+`rtc_fill_cmos`) | YES | the boot-2 firmware re-reads the CMOS the BIOS bring-up left; fixed-epoch virtual time (D-F2) makes the re-run deterministic |
| virtio_console | YES (when present) | the reference NAMES it (cpu.js:948-951); `virtio_console_reset` |
| PS/2 | YES | the reference NAMES it (cpu.js:952-955); `ps2_reset` |
| UART | **NO** | (a) every raise is guest-write- or host-input-driven (uart.c: data_received / set_modem_status_bit / write_data / port1_write8) — no poll, no timer — so a stale IER cannot fire uninvited; (b) `uart_attach` ALLOCATES and `bus_register` APPENDS, so a reboot-time re-init leaks a device + five listeners per boot — MEASURED: the runaway image hit bus.c:41's 64-listener assert before MAX_REBOOTS fired |
| DMA | NO | no live timer, no interrupt line; users reprogram before transferring (the reference leaves it) |
| IDE | NO | no `reset` exists upstream; a mid-command busy drive is recovered by boot 2's own probe discipline (libata softreset) — the reference's browser lane boots the same arrangement |
| PCI | NO | config space persists; the firmware's IDE-enable writes re-run idempotently; the trigger byte is not stored |
| ACPI | NO | not constructed in this port's boots (`acpi_enabled` false, no CLI flag) — no state exists |
| guest memory + disk | **NEVER** | `reset_memory()` (cpu.js:969-972) is a separate method the reference's reboot does not call; the disk buffer is boot 1's. THE property. |
| `fw_value = []` | n/a | the reference's fw_cfg read queue has no counterpart — the port's firmware reads the option ROM out of guest RAM (minibios.asm:252-258) |

## The uop cache / JIT question (the unit's deliverable 4)

**What the key includes.** A decoded block is keyed `(phys_start, virt_start, state)`
where `state` is `cpu->state_flags & CACHED_MASK_ALL` (is_32, stack_size_32, cpl==3,
flat segmentation) — `uop_cache.c`'s `uop_cblock` and `uop_hash_pos`. Everything a uop
captures besides the key is a pure function of the bytes and the mode the key carries:
segment bases and paging are applied at EXECUTE time (`get_seg_prefix`, the per-access
translation), immediates are extracted data, `body_ip` derives from the block's own
virtual start.

**Is a clear required?** YES — because the REFERENCE SAYS SO, not because the keys go
stale. `reset_cpu` upstream contains `jit::jit_clear_cache_js()` (cpu.rs:4757) — the
code cache is cleared at every CPU reset. The port's `reset_cpu` carries the comment
"dropped (interpreter-only port)" — a reason that stopped being true when the JIT
shipped. What WOULD go wrong without it, mechanically: after a reboot the state byte
usually differs (reset is real-mode), so boot-2 lookups miss boot-1 blocks and re-decode
— but for a same-state re-entry (e.g. the multiboot entry re-establishes the identical
state byte) boot 2 would EXECUTE boot 1's cached blocks. Those blocks are still faithful
decodings of unchanged bytes, so the two-boot image cannot see the difference (the
mutation rig says exactly that: omitting `uop_cache_clear` is NOT CAUGHT, verdict GOLD).
The clear is nonetheless mandatory: it is the reference's own reset-time behaviour, it
reclaims the arena/block tables, and it keeps the compiled-block registry consistent
with the decoded one (`uop_cache_clear` → `jit_code_clear_all` is a CORRECTNESS pair —
the shared dirty-page bitmap is the only thing that makes write paths watch compiled
pages).

**Where the clear runs.** NOT inside `machine_reboot`: the function fires mid-block from
a device handler, and the executor holds pointers into the arena the clear recycles
(`uop_block_decode` reuses slot 0) — a synchronous clear would hand the executor
re-decoded uops as the current block's remainder. The reference can clear inside its
reset because its JS interpreter holds no cache pointers. So: the flag + the abort
unwind the execution to `main.c`, and `reboot_prepare_reentry()` runs
`uop_cache_clear()` (compiled blocks go with the decoded ones) BEFORE any boot-2
instruction, then clears `jit_abort_flag` (its consumer may never have run) and
consumes `cpu->reboot_requested`.

**Instruction counter note.** `reset_cpu` zeroes `instruction_counter` (cpu.rs:4730,
transcribed faithfully long before this unit), so each boot gets its own limit budget
and the transcript's `instruction_count` is boot 2's only. The virtual clock rewinds
with it (D-F2's instruction-clock): the reference's diff<0 arm in `pit_did_rollover`
reads a rewind as "restore_state rollover" — deterministic, and the reason the gate
image's PIT witness programs MODE 2, not mode 0 (a mode-0 counter gets disabled by that
arm before boot 2's latch — MEASURED; the image's comment records it).

## What a reboot does NOT restore

Guest RAM (including boot 1's writes anywhere outside the loader's own regions — on the
multiboot path, everywhere), the disk and everything ever written to it, the CMOS RTC's
fixed-epoch time base, PCI config space, the IDE controller's state and media. Deliberate
non-goals unchanged from before this unit: no A20 mask (unmodeled), no VGA, no APIC.

## Gates (exit codes and raw output in results/reboot-1-gates.txt)

- **G1** `tests/reboot2x.asm` (one source, `TRIGGER_PS2` defsym → `tests/reboot-pci.img`
  / `tests/reboot-ps2.img`, the M0 recipe) runs TWO boots in the transcript mode: boot 1
  writes a marker, arms the witnesses (PS/2 command register 0x65, PIT channel 0 mode 2,
  ECX 0xDEADBEEF), fires the trigger; boot 2 reads everything back into the hashed
  window and halts. Pre-fix asymmetry captured: both sites stopped with their named
  messages, rc 1, no transcript. Post-fix: verdict `0x600D600D`, marker read back,
  ECX zero, CS=0x08, CR0=1 — under `--jit on` (default), `--jit off`, `--uop-cache off`,
  the three transcripts byte-identical per image, the two images' windows distinct
  (the trigger id word is in the window).
- **G2** memory survives + the CPU entered at the multiboot reset state + the devices at
  their reset values: checked against an INDEPENDENT model of the whole 4 KiB window
  (`tests/reboot_window_model.py` — every word accounted for; an interrupt frame under
  the stack top would break the hash). Determinism: two runs byte-identical. Disk not
  re-seeded: THE KERNEL ARM — a real i386 Linux guest (Alpine 6.6.134-lts) reboots
  ITSELF (`sysrq: Resetting` → the keyboard controller → port 0x64 0xFE), and boot 2
  reads boot 1's marker back from /dev/sda sector 8 through the guest's own libata:
  `DISK PERSIST OK`, rc 0, 1m58s. The host's `tests/images/disk.img` is untouched
  (mtime and sector-8 content are the pre-run originals — the marker lived only in the
  device's buffer).
- **G3** `make` rc 0; `make selftest` — cpu 3522 + m1c 228 + hw 551 + uop-cache
  7,934,349 + uop-exec 102 + jit 51 + jit-codegen 2978, 0 failures (the hw suite's ps2/
  pci/timer-seat expectations were updated from "stops" to "requests the re-entry"; the
  count is unchanged); `make nasm` 118/118, 5 named skips; `make transcript-diff`
  byte-identical; `make bench` 122.5/120.2 MIPS (the pre-unit number was ~119) with the
  terminal kernel-boot state asserted; `build/v86c.aarch64.elf` fresh.
- **G4** `results/reboot-1-mut-rig.sh`: omit `reset_cpu` → CAUGHT (EBX 0xFACD0002);
  omit `ps2_reset` → CAUGHT (0xFACD0003); omit `pit_init` → CAUGHT (0xFACD0004); omit
  `uop_cache_clear` → NOT CAUGHT by the image, honestly recorded (the keys survive;
  the clear's justification is the reference's own reset-time behaviour + arena
  reclaim, not this image). `pic_reset`/`rtc_fill_cmos` omissions are not observable by
  the image by construction (it masks both PICs before its only probe; it reads no
  CMOS) — the kernel arm is the PIC's catcher (boot 2's firmware under boot 1's
  unmasked remap would take stray vectors into its IVT).

## Notes for the next reader

- `instruction_count` in a rebooting transcript counts boot 2 only (reset zeroes the
  counter); each boot gets its own limit budget.
- The kernel arm's stderr carries ~1.6M `jit: bind: the block table is full (2048)`
  lines over two 4G-instruction boots — the pre-existing capacity degradation path
  (bind fails, the block stays interpreted). Not investigated here; flagging as noise
  worth a look in a JIT unit.
- TODO.md §1.4 now describes fixed behaviour; the sweep's "stops loudly by name" list
  there is stale for these two sites.
