# PAE-1 — the PAE page walk, load_pdpte #GP, and the two-level gate

Plan: `handoff-v86c-pae-plan` (adopted, engineer-routed). Scope decided by the user:
**LEVEL 1 + LEVEL 2**; Level 3 (a `CONFIG_X86_PAE=y` kernel build) is deferred and not
part of this unit.  Every claim tagged MEASURED / READ / INTERPRETATION.

Reviewed by `handoff-v86c-pae-review` (cleared the reserved masks, A/D bits, the three
named call sites' atomicity and the trap removal); its findings F1-F7 are closed in the
Round-2 section below.

## Write scope

- `src/cpu/paging.c`, `src/cpu/instructions_0f.c`, `src/cpu/paging.h`
- `src/cpu/cpu_state.h`, `src/cpu/cpu_state.c` (comments only)
- `src/cpu/cpu_selftest.c`
- `tests/` (the new synthetic PAE guest + its gate)
- `TODO.md` §1.3, this report
- NOT: `v86/` (READ-ONLY), other `results/*`, no commit.

---

## F1 — the pre-change asymmetry capture (MEASURED, before any paging.c edit)

`make build/cpu_selftest` (rc 0) on the unmodified tree, then `./build/cpu_selftest`:

```
rc = 134 (SIGABRT, core dumped)
...
PASS paging: 4 KiB translation, A/D bits, TLB read-only entry
PASS paging: faults (absent, read-only/WP, user vs supervisor) + cr2/eip/delivery/error codes
PASS paging: 4 MiB PSE page (translation, A/D, page boundary)
v86c: FATAL: PAE paging (CR4.PAE / 64-bit page tables) is not implemented in M1 -- deferred to M2 (plan ss3). Refusing to misinterpret a 64-bit PTE.
```

The new PAE cases abort the moment `test_pae_4k` calls `load_pdpte` (the trap stub);
the three pre-existing non-PAE paging suites still pass before the abort.  This is the
asymmetry half of the proof: the suite FAILS (aborts) before the walk is ported.

## Gates

| gate | result |
| --- | --- |
| `make` | **rc 0** (MEASURED) |
| `make selftest` | **rc 0** — all suites green incl. the 7 new PAE cases (`cpu_selftest` 3582 checks / 0 failures) |
| `make nasm` | **rc 0, 118/118** (`[+] Passed 118/118 tests.`) |
| `make transcript-diff` | **rc 0** — C transcript byte-identical to the reference (flag-off machine unchanged) |
| `make determinism` | **rc 0** — two oracle runs byte-identical |
| `make bench` | **rc 0** — best 0.863 s / 231.86 MIPS, 200095214 instr (the pin, unchanged) |
| `make minibios-mem` | **rc 0** (up to date) |
| `sh tests/gate-m2-5-guest.sh` | **rc 0** — `### m2-5 slice-3 gate: 0 FAILING assertion(s)` |
| `REBOOT_KERNEL_ARM=1 sh tests/gate-m2-5-guest.sh` | **rc 0** — same (that env var is not read by this script; re-run identical) |
| `sh tests/gate-m2-5-determinism.sh` | **rc 0** — `### m2-5 determinism gate: 0 FAILING assertion(s)`, 4000024062 instr both runs |
| `sh tests/gate-reboot-1.sh` | **rc 0** — `39 passed, 0 failed` |
| `REBOOT_KERNEL_ARM=1 sh tests/gate-reboot-1.sh` | **rc 0** — `39 passed, 0 failed` + `kernel arm: PASS (two boots, the disk write survived)` |
| `sh tests/gate-pae-1.sh` (Level 2) | **rc 0** — `11 passed, 0 failed` |

## F2 mutation probes (MEASURED, one-line reverts, each flips its case pass->fail)

Each probe: mutate `src/cpu/paging.c`, `make build/cpu_selftest`, run; then restore.

| probe (mutation) | selftest rc | flip |
| --- | --- | --- |
| drop the 0x1FE000 2 MiB exception | 1 | `cpu->pagefault` + `delivered_error_code == 9` fail (bit-13 PDE stops faulting) |
| always reserve NX (`(nxe ? NX : NX)`) | 1 | NXE-on data read faults (got 0, want 0x11223344); fetch error 0x19 (rsvd set) not 0x11 |
| swap 2 MiB high back to 0xFFC00000/0x3FF000 | 1 | 0x401234 translates to 0x401234 (want 0x601234) |
| `=` instead of `|=` for the 4 KiB-arm no_exec | 1 | fetch of a PDE-NX page succeeds (got 0x5000, want 0) |
| truncate pde to int32_t before the reserved check | 1 | high-half PDE silently translates through a valid PTE (no fault, cr2 stays 0) |
| re-add the load_pdpte aborts | 134 | the #GP cases abort (SIGABRT) instead of checking |

## F3 synthetic guest both-leg differential (MEASURED)

`tests/pae-guest.asm` -> `tests/pae-guest.img` (multiboot, built by a Makefile
rule).  It builds a PDPT/PD/PT, `lgdt`/`lidt` a flat GDT and a minimal #PF gate,
enables EFER.NXE + CR4.PAE + CR3 + CR0.PG, then exercises the ported walk:

- every post-paging instruction fetch runs through a 2 MiB mapping (PD[0] -> phys 0,
  identity for virtual [0, 2M));
- a stub is CALLed through a 4 KiB mapping (virtual 0x200000 -> phys 0x113000) --
  4 KiB fetch/execute;
- 4 KiB (virtual 0x202000) and 2 MiB (virtual 0x180000) data round-trips;
- **the 2 MiB high-composition discriminator**: PD[2] maps virtual [0x400000,
  0x600000) to physical 0x200000, so the PDE's bit 21 (1) differs from the access's
  bit 21 (0).  A write at 0x400000 must land at phys 0x200000 (out of RAM, dropped)
  and leave the seeded canary at phys 0 intact; a PSE high-mask
  (0xFFC00000/0x3FF000) computes phys 0 and clobbers the canary -- the verdict then
  leaves GOLD (FAIL 0xFACD0005);
- an NX #PF: JMP to virtual 0x201000 (PTE bit 63 + EFER.NXE) faults with error code
  0x11, caught by the minimal IDT whose handler records it and redirects EIP.

The guest's own verdict (EBX) is GOLD `0x600D600D`; every round-trip, the canary and
the NX error code are checked in-guest.

Result (MEASURED, after the discriminator was added):

```
oracle transcript sha256 = 4ad51f570ec5eeea
C default config       = 4ad51f570ec5eeea
C --jit off            = 4ad51f570ec5eeea
C --uop-cache off      = 4ad51f570ec5eeea
```

All four byte-identical; `--fixture` EBX = 1611489293 = 0x600D600D; 95 instructions
to hlt.  `sh tests/gate-pae-1.sh` -> `11 passed, 0 failed`.

**Asymmetry proof of the new discriminator (both arms, MEASURED):** replacing only
the PAE 2 MiB high composition with the PSE one (`0xFFE00000/0x1FF000` ->
`0xFFC00000/0x3FF000`) in `src/cpu/paging.c` and rebuilding `build/v86c`:
- mutant: `sh tests/gate-pae-1.sh` -> **rc 1, `7 passed, 4 failed`** (the three
  configs' `--fixture` verdicts are 0xFACD0005 = -87228411, and `default ==
  reference` fails; the C transcript sha256 is 550e1739c1f24fb8 vs the reference's
  4ad51f570ec5eeea);
- restored: `make` + `sh tests/gate-pae-1.sh` -> **rc 0, `11 passed, 0 failed`**,
  `--fixture` EBX = 0x600D600D.

Two implementation notes the guest forced out (both MEASURED, not assumed):
1. GNU as intel syntax treats `mov reg, label` as a MEMORY load; loading the address
   needs `lea` (the first build silently copied the stub and the IDT offset from the
   wrong places, which surfaced as a #UD on the garbage stub bytes).
2. The NX fetch fault's saved EIP is the fetch TARGET (0x201000), not the transferring
   instruction, so the handler must redirect EIP to a known continuation rather than
   "skip past the faulting instruction".

## What Level 2 does NOT prove

The guest does NOT discriminate: the 4 KiB arm's PTE reserved mask (a high-half PTE),
the PDE reserved mask, the PDPTE not-present/reserved paths, or load_pdpte's #GP.
Those are covered by the selftest battery and its F2 mutation probes instead (the
pinned reference still asserts on reserved PDPTEs, so the #GP cases are unreachable
in the guest by design).  It also does not prove real-OS table patterns or the
16->32-bit boot transition (a multiboot image enters already in protected mode).
Level 3 -- a `CONFIG_X86_PAE=y` kernel boot -- is named and deferred to a follow-on
unit, per the user's scope decision.

Also outside every gate here: the hardware task-switch `set_cr3` call site
(`exceptions.c`, review F3) does not validate-before-commit (a bad-PDPT TSS delivers
#GP in a half-switched context); it is documented in a comment at the site and is not
exercised by any in-tree PAE guest.

## Round 2 — review findings (handoff-v86c-pae-review), all closed

| finding | fix | verification |
| --- | --- | --- |
| **F1** `load_pdpte`'s `DBG_ASSERT((cr3 & 0xF) == 0)` rested on a false invariant (set_cr0 and the CR4 arm pass `cpu->cr[3]` unmasked) | mask `cr3 &= ~0xF` inside `load_pdpte`, so the assert's premise holds by construction | `make selftest` rc 0; the assert is now unreachable from any caller |
| **F2** (PRE-EXISTING, found by review, reproduced by the orchestrator) `paging.c:489` `DBG_ASSERT((cr3 & 0xFFF) == 0)` aborts the shipped binary on a legal non-PAE `mov cr3, <PWT/PCD>` | mask `~0xFE7` (upstream `!0b111111100111` -- aligned to upstream: the old `~0x7E7` left bit 11) and DROP the assert (upstream's is `debug_assert!`, compiled out of the shipped wasm) | see F2 below |
| **F3** the hardware task-switch `set_cr3` site (`exceptions.c`) does not validate-before-commit | documented the residual in a comment at the site (a fix would hoist the PDPTE load above the segment switches; task switches are not exercised by any in-tree PAE guest) | READ-verified the comment names the exact half-switch shape |
| **F4** `test_pae_gp`'s CR3-atomicity check could not fail (sentinel == committed value) | sentinel `0xDEAD0000` + a new set_cr0 atomicity case | see F4 below |
| **F5** `paging.c` do_page_walk header still claimed the PAE arm is "replaced by an explicit trap" | reworded to name `cpu.rs:2087-2134` as ported | READ |
| **F6** `TODO.md:80` section header still listed PAE under "Deliberately not ported" | header now "Deliberately not ported (the devices)" | READ |
| **F7** `load_pdpte` committed `reg_pdpte` entry-at-a-time, leaving a partial cache behind an uncommitted CR3 on a #GP at index>=1 | validate into a local `tmp[4]`, copy on success | `make selftest` rc 0 (the existing #GP cases still pass with the copy) |

### F2 measurement (PRE-EXISTING; MEASURED)

The orchestrator built a 42-byte multiboot probe (`results/pae-1/cr3.asm` ->
`cr3.img`) that executes `mov cr3, 0x1008` (4 KiB base, PWT set), then `hlt`.

- **pre-fix**: `./build/v86c cr3.img` ABORTS rc 134 --
  `paging.c:489: assert((cr3 & 0xFFF) == 0) failed ... 1008` (the orchestrator's
  `cr3-probe-c-port-stderr.txt`), while the pinned reference runs to completion
  (`cr3-probe-reference-transcript.json`: eip 0x00100033, cr3 0x1008, EBX GOLD).
- **post-fix**: `./build/v86c cr3.img 1000` -> **byte-identical** to
  `cr3-probe-reference-transcript.json` (diff empty; instruction_limit 1000 matched).

The port now matches the measured reference behaviour: no abort, CR3 committed with
PWT/PCD preserved (mask `~0xFE7`).  Chosen consequence, stated: a PWT/PCD-set CR3
flows into the non-PAE walk and offsets the PD base by 8/16 -- upstream-faithful, not
hardware-faithful (hardware masks the base to 4 KiB); the comment at `paging.c`
records it, and aligning the walk base would be a separate one-line change outside
this unit.

### F2 asymmetry (both arms, MEASURED)

Re-adding the old `~0x7E7` mask + `DBG_ASSERT((cr3 & 0xFFF) == 0)` to `set_cr3`'s
non-PAE arm and rebuilding `build/cpu_selftest`:
- pre-fix arm: `./build/cpu_selftest` -> **rc 134**, `paging.c:489: assert((cr3 & 0xFFF)
  == 0) failed` (the new `test_cr3_nonpae_mask` case aborts);
- post-fix arm: **rc 0**, `PASS paging: non-PAE MOV CR3 with PWT/PCD set does not
  abort (mask ~0xFE7)`.

### F4 asymmetry (both arms, MEASURED)

Mutating `set_cr3` to commit `cpu->cr[3]` BEFORE the `load_pdpte` check (the bug the
atomicity test exists to catch), then:
- **new sentinel** (`0xDEAD0000`): `./build/cpu_selftest` -> **rc 1**, two `FAIL`s
  (`cpu->cr[3] == cr3_sentinel`, got 0x1000 want 0xffffffffdead0000) -- the bug is
  caught;
- **old sentinel** (`PAE_PDPT_PHYS`, the pre-F4 value): **rc 0**, 3588 checks / 0
  failures -- the old test could not fail (its "unwritten" and "written" values were
  identical), which is exactly the defect F4 fixes.

### Round-2 gate re-run

`make` 0 · `make selftest` 0 (3588 checks/0 failures, 8 paging suites incl. the new
non-PAE-CR3 case) · `make nasm` 118/118 · `make transcript-diff` 0 · `make
determinism` 0 · `make bench` 0 (222.82 MIPS best this run vs 231.86 earlier --
host-load variance; the determinism pin `200095214 instructions` is UNCHANGED) ·
`sh tests/gate-pae-1.sh` 11/0.
