# R5F — the delivered-fault BLOCKER (B1) and S2, resolved

**Unit:** `HANDOFF=v86c`, unit R5F (coder). **Authority:** `results/r5-review.md` (R5),
whose B1 section carries the measured asymmetry and whose prescription I RE-MEASURED rather
than accepted. **Upstream:** `./v86` READ-ONLY. **Scope:** `src/cpu/cpu.c`,
`src/cpu/cpu_state.h` (contract, declared loudly below), `src/cpu/instructions_0f.c` (S2
only), `src/cpu/m1c_selftest.c`, and new evidence under `results/`. `paging.c` NOT touched
(the finding is in the loop's read of the flag, not in the walk that sets it); `src/hw/`,
`tests/`, `oracle/` untouched.

**Verdict: B1 fixed at the seam upstream's shape requires (one line at
`cpu.c:125`); S2's four asserts dropped at their own sites. All gates green — with one
composite caveat caused by the CONCURRENT M2 unit, attributed in §G3.**

---

## B1 — the fix, and why that seam

**One line**, `src/cpu/cpu.c:125`, immediately after the dispatch returns:

```c
		run_instruction(opcode | (cpu->is_32 ? 1 : 0) << 8);
		/* and the SECOND clear, which ends the dispatch's fault window (D1 above) ... */
		cpu->pagefault = false;
		DBG_ASSERT(cpu->prefixes == 0);
```

**The seam, argued from upstream's shape, not from convenience.** In upstream a fault
cannot outlive the instruction that raised it: `return_on_pagefault!` expands to `return`
from the GENERATED `run`, and `jit_run_interpreted` calls `run_instruction(opcode | ...)`
as a plain statement (`cpu.rs:3321`). So the loop tail's
`return_on_pagefault!(get_phys_eip())` (`cpu.rs:3335`) is reachable only with the FETCH's
own error pending. This port's single flag is that same channel, and its consumers all live
*inside* the dispatch (the generated checks, `exceptions.c`); the first reader *after* the
dispatch returns is the tail fetch check. Ending the flag's live window where upstream's
`return` ends it — at the dispatch boundary — is therefore the faithful seam, and it is the
only one that leaves the fetch check honest. `cycle_internal`'s entry clear stays (it covers
a fetch fault at cycle entry), and the dispatch-entry clear stays, so the flag's live window
is exactly one instruction: cleared before, cleared after, never inside a helper.

**Not done, deliberately:** the assert at the end of `cycle_internal` (`cpu.c:168`) is
untouched. It is what caught this; the fix removes its false trigger, not the check.

**`cpu_state.h` WAS TOUCHED — and it should be.** Contract item 4 said "clears it once per
instruction, before dispatch, **and that is the only clear**" — after this fix that sentence
is false, and it is a sentence every later unit reads as the fault protocol. I rewrote item 4
to say the loop *brackets* the dispatch (before and after), kept its nesting rationale
verbatim ("a helper must never clear it: nested faults must not be masked by the inner
return path"), and recorded why the post-dispatch clear is part of the contract (with the
measured consequence). No other line of the header changed; no struct field, no signature.

## G1 — the asymmetry IS the proof (before → after)

The reviewer's probe construction rebuilt (`results/r5f-probe.c`, driven by
`results/r5f-probe.sh`, linking the real TUs and calling the real `cycle_internal`). Two
deliberate corrections to the reviewer's probe, neither of which changes its conclusion: the
handler for the different-page CONTROL is at **0x4010** (page 4) — the reviewer's control put
it at 0x3010, which is the GDT's page (GDT base 0x3000, 3 entries), so the handler's bytes
overwrote the data descriptor. Re-measured with the corrected address, the control reproduces
the reviewer's numbers exactly (`after 1: counter=1`, `after 2: counter=4, marker=1`), which
also shows the clobber was harmless to that scenario.

**BEFORE the fix** (`results/r5f-probe-before.txt`, full file):

```
---- probe same ----                  # handler 0x1010: SAME page as the faulting 0x1000
same: handler=1010 before: eip=1000 counter=0 marker=0 esp=1f00 pf=0
src/cpu/cpu.c:145: assert(cpu->instruction_counter != initial_instruction_counter) failed
Aborted (core dumped)
rc=134
---- probe diff ----                  # handler 0x4010: DIFFERENT page (the control)
diff: after 1: eip=4010 counter=1 marker=0 esp=1ef0 pf=1 unimplemented=0
diff: after 2: eip=1000 counter=4 marker=1 esp=1f00 pf=0 unimplemented=0
rc=0
```

**AFTER the fix** (`results/r5f-probe-after.txt`):

```
---- probe same ----
same: after 1: eip=1000 counter=4 marker=1 esp=1f00 pf=0 unimplemented=0
same: after 2: eip=1000 counter=8 marker=2 esp=1f00 pf=0 unimplemented=0
same: after 3: eip=1000 counter=12 marker=3 esp=1f00 pf=0 unimplemented=0
rc=0
---- probe diff ----                  # unchanged behaviour for the control
diff: after 1: eip=4010 counter=1 marker=0 esp=1ef0 pf=0 unimplemented=0
diff: after 2: eip=1000 counter=4 marker=1 esp=1f00 pf=0 unimplemented=0
rc=0
```

Same-page now behaves like the control: the fault is delivered, the handler runs
(`marker` increments), `iret` returns to the faulting instruction, the counter advances
(4 per cycle = faulting instruction + inc + add esp + iret, the loop breaking on the iret's
block boundary). One measured difference from the reviewer's control output remains and is
correct: `pf=1` at the `diff` cycle-1 return became `pf=0`, because the flag no longer leaks
out of the dispatch at all. **The only readers of the flag in the whole tree are in
`cpu.c`** (verified by grep over `src/` and `tests/`; the generated dispatcher only *checks*
it inside a dispatch), so nothing can observe that leak.

**The single-variable mutation** (`results/r5f-mut.sh`, output `results/r5f-mut.txt`): with
ONLY the added line deleted, and everything else — including my comments — held constant, the
same-page case aborts again while `diff` and `fetch` pass:

```
=== mutB ===
shadow cpu.c sha: c590aad2…  live: d8dc732a…
/tmp/r5f-mut/cpu/cpu.c:167: assert(cpu->instruction_counter != initial_instruction_counter) failed
rc=134
--- probe diff --- rc=0     --- probe fetch --- rc=0
```

That is the proof of attribution: that one line, and nothing else in this change set, is what
turns the abort into correct delivery.

## G2 — the flag's REAL purpose survives (and is still load-bearing)

`fetch` scenario: `eip` starts on an ABSENT page, so `get_phys_eip` itself faults.

```
fetch: after 1: eip=1010 counter=0 marker=0 esp=1ef0 pf=1 unimplemented=0   # nothing ran, nothing counted
fetch: after 2: eip=2804 counter=3 marker=1 esp=1f00 pf=0 unimplemented=0   # the handler runs NEXT cycle
```

The counter does not move, the fault is still delivered (frame pushed, `eip` = handler,
`pf` set as the abort signal), and the handler runs on the following cycle — identical
before and after the fix.

**Proven load-bearing, not merely present** (`mutA`, `results/r5f-mut.txt`): deleting the
cycle-entry `if (cpu->pagefault) return;` leaves the `fetch` case exiting 0 — with a WRONG
result:

```
--- probe fetch ---
fetch: after 1: eip=2804 counter=3 marker=0 esp=1f00 pf=0
```

**A prediction I made and the measurement refuted, recorded as such:** I expected that
deletion to abort at `cpu.c:90` (`assert(!in_mapped_range(phys_addr))`).
`in_mapped_range(0)` is FALSE (`memory.h:93`: address 0 is neither in the VGA window nor
`>= memory_size`), so nothing asserted. What actually happens is worse and better evidence:
the delivery had already left `eip` at the handler, `get_phys_eip` returned its sentinel 0,
and the interpreter then executed PHYSICAL ADDRESS 0 from there — 3 instructions counted,
none of them the guest's, the handler's marker never set, and an `iret` found in that garbage
restored `eip`/`esp`. Silent wrong execution is exactly what the flag's abort prevents, and
it is unchanged by my fix. The rig's header now carries the MEASURED outcome, not the
prediction.

## G3 — the gates

Run on the frozen tree (`results/r5f-gates.txt`, `results/r5f-gates-final.txt`):

| gate | result |
|---|---|
| `make` | **rc 0** (no warnings; `-Werror` on the core) |
| `make selftest` → `cpu_selftest` | **rc 0 — 3522 checks, 0 failures** (unchanged) |
| `make selftest` → `cpu_m1c_selftest` | **rc 0 — 222 checks, 0 failures** (was 193; +25 new group, +4 S2 regression) |
| `make transcript-diff` (M0) | **rc 0** — "the C transcript is byte-identical to the reference's" |
| `make nasm` | **rc 0 — Passed 118/118**, 0 produced no state, the same 5 skips |
| generator | `1922 call sites, 1922 implemented, 0 unimplemented, 1926 names defined (unchanged)` |

**New gate so this cannot come back** (`src/cpu/m1c_selftest.c`, group 9
`test_dispatcher_fault_flag`, 25 checks): the same-page delivered fault, its different-page
control, and the genuine fetch fault — all driven through the real `cycle_internal`, because
B1 was a dispatcher bug and every earlier group calls the delivery helpers directly (the
review says so, and it is why the bug was invisible to them). It FAILS as required: with the
fix reverted, the shipped binary aborts inside exactly this group (rc 134, `rc=134 (a nonzero
rc or an abort above is the mutation being caught)` in `results/r5f-mut.txt`); with `mutA`
applied it reports **5 concrete failures** in group (c) and rc 1.

**The composite caveat, attributed.** `make selftest` as a single target is currently RED
(rc 2) — in `hw_selftest`, the M2 unit's suite — because the other coder session was editing
`src/hw/` throughout my gate runs: `src/hw/io.c` mtime **15:21:03** (35 failures at 15:22),
`src/hw/hw_selftest.c` mtime **15:23:27** with
`'test_clock' defined but not used [-Werror=unused-function]` (does not compile), then
**225 checks / 24–28 failures** at 15:26–15:28 as they add tests. **Not attributed to R5F by
measurement and by construction, not by argument:**

- the 15:18:19 gate run (`results/r5f-gates-interim.txt`) has `make selftest rc=0` with
  `hw_selftest: 198 checks, 0 failures` — and the SAME invocation ran `cpu_m1c_selftest:
  218 checks, 0 failures`, i.e. group 9 (the same-page delivered fault) PASSED, which can
  only happen with this fix linked: their suite passed, in the same `make selftest`, on a
  tree that already carried the fix, and broke three minutes later after their own write;
- every current failure is one family — `io_port_read*(unclaimed) == 0xFF (got 0x0)` and
  `!interpreted_unimplemented` — i.e. their own D-F1 unclaimed-port policy in `src/hw/io.c`;
- `src/hw/` contains **no call to `cycle_internal`, `interpreted_run` or
  `do_many_cycles_native`** (grep over `src/hw/*.c`, `*.h`: zero hits), and their G4 group
  drives `instr_E6`/`instr_E4` directly — so the M2 suite does not reach the line I changed
  at all.

My two suites were rebuilt and run directly in `results/r5f-gates-final.txt` (rc 0, 3522/0
and 222/0) so their status does not depend on the M2 unit's file. **Nothing in `src/hw/` was
touched by me.**

## G4 — S2: the four guest-reachable `DBG_ASSERT`s

Resolved the way R4F resolved the fxsave/fxrstor pair and R3F the emitter's: **dropped at
their own sites**, upstream's message text kept as a comment, NOT by `-DNDEBUG`.

- `instructions_0f.c` `instr_0F30` (the wrmsr family): the MSR_EFER reserved-bits assert, the
  `high == 0` / relocated-address / EXTD asserts on IA32_APIC_BASE — four asserts, per R5.
- The block header's justification ("kept only where they guard a value the code then USES")
  is corrected, not deleted: a value the release reference uses is precisely a value it uses
  UNCHECKED (`dbg_assert!` = `debug_assert!`, `dbg.rs:63-67`).
- Upstream's `address` local existed only as the second assert's operand; with the assert
  gone it is dead, so it went too (nothing else read it).

**Gate:** `make nasm` unchanged — 118/118 passed, 5 skipped, the same skip reasons
(`results/r5f-gates.txt:318-326`); the generator's counts unchanged; and the stop the corpus
depends on is still live — `jump`'s skip reason is still
`src/cpu/cpu.c:83: assert(!in_mapped_range(phys_addr)) failed`, i.e. no other stop was
silenced.

**The asymmetry, measured** (`mutC` in `results/r5f-mut.txt`): re-adding ONLY the EFER assert
to a shadow `instructions_0f.c` makes the shipped selftest abort on ordinary guest bytes, at
its own site:

```
/tmp/r5f-mut/cpu/instructions_0f.c:6286: assert((value & ~((uint64_t)1 << 11)) == 0) failed
rc=134
```

and with the fix, `test_m1c_opcodes` now writes reserved EFER bits / a relocated APIC base
and asserts the unchecked values are stored and no fault is raised (4 new checks).

## Where I diverge from R5 — and where its prescription held

- **Prescription held.** The review's fix ("clear `cpu->pagefault` at the end of each
  `interpreted_run` iteration, immediately after `run_instruction`") is what I implemented,
  and its diagnosis is exactly right: the mechanism (the walk's unconditional set) is
  correct, the loop's read of it was not.
- **One probe correction, conclusion unchanged:** the review's control handler sat in the
  GDT's page (§G1). It still demonstrated the asymmetry; the address is not a construction to
  reuse, so the rig uses 0x4010 and reproduces its numbers.
- **One expectation corrected by measurement:** §G2's `mutA` — silent misinterpretation of
  physical address 0, not the abort I predicted.
- **The review's "N1 citations" style is followed:** the new comments cite `cpu.rs:3321/3335`
  and `dbg.rs:63-67`, each re-checked with grep against `./v86` before being written.

## Limits, stated (what this unit did NOT establish)

- **The loop-tail fetch check (`cpu.c:144`) is not exercised by any probe here.** It is the
  site my clear protects, but reaching it needs an instruction that changes its own page's
  mapping mid-instruction (CR3 reload / a PTE write); the reachable genuine-fetch case is the
  cycle-entry one. The tail stays as upstream's shape (`return` before the counter update,
  D1's named quirk) and was NOT "fixed".
- `interpreted_unimplemented` (D2) and the PAE trap paths are untouched.
- The selftest's group (c) asserts the flag is SET when `cycle_internal` returns after a
  fetch fault (port state, matching the contract); upstream has no such persistent state to
  compare against, and the header's D1 note is where that divergence is recorded.
- `mutA`/`mutC` are shadow builds; `make nasm` / the M0 transcript were not re-run under a
  mutation (they do not execute the mutated paths in this configuration).

## Reproducing the evidence

```sh
sh results/r5f-probe.sh                 # the live tree: same / diff / fetch
sh results/r5f-mut.sh                   # mutB, mutA, mutC (shadow only; live shas printed and re-checked)
make                                    # gates
make build/cpu_selftest build/cpu_m1c_selftest && ./build/cpu_selftest && ./build/cpu_m1c_selftest
make transcript-diff && make nasm
```

Evidence files: `results/r5f-probe.c`, `results/r5f-probe.sh`,
`results/r5f-probe-before.txt`, `results/r5f-probe-after.txt`, `results/r5f-mut.sh`,
`results/r5f-mut.txt`, `results/r5f-gates.txt`, `results/r5f-gates-interim.txt`,
`results/r5f-gates-final.txt`.

Shas at gate time (`results/r5f-gates-final.txt`): `src/cpu/cpu.c`, `src/cpu/cpu_state.h`,
`src/cpu/instructions_0f.c`, `src/cpu/m1c_selftest.c` — the comment-only edits made after the
15:22 run are covered by that final build.
