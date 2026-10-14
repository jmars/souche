# R5 — independent review: the M1c exception/segment machinery, and R4F's oracle repair

**Unit:** `HANDOFF=v86c`, unit R5 (reviewer). **Reviewed:** M1c (`src/cpu/exceptions.{c,h}`,
`src/cpu/pic.{c,h}`, the 31 encodings appended to `src/cpu/instructions_0f.c`,
`lar/lsl/verr/verw` in `misc_instr.c`, `read_tsc`/`set_tsc` in `cpu.c`, the dispatcher's
interaction with the fault flag, `exec_boundary.c`, `m1c_selftest.c` in part) and R4F
(`tests/nasm_run.py` G0 repair, the N1 citations, N2 softfloat types, N3 fxsave/fxrstor).
**Reference:** `./v86` (READ-ONLY). Every disagreement below names both sides' file:line.
**Verdict: 1 BLOCKER, 1 SHOULD-FIX — the transcriptions themselves are faithful; the
blocker is in the dispatcher's interplay with the fault flag, which no current gate can see.**

---

## BLOCKER B1 — a *delivered* fault leaves `cpu->pagefault` set, and the dispatcher then aborts the machine

**Port:** `src/cpu/cpu.c:103,110,133-137,145` + `src/cpu/paging.c:163-166,178-181,209-212`.
**Upstream it disagrees with:** `cpu.rs:3302-3342` (`jit_run_interpreted`), `cpu.rs:3155-3285`
(`cycle_internal`), `cpu.rs:3278-3288` (`get_phys_eip`).

The mechanism, in order:

1. `do_page_walk` sets `cpu->pagefault = true` **unconditionally** after
   `trigger_pagefault` — even when the fault is successfully DELIVERED (the IDT gate
   resolved, the frame pushed, the handler entered). The set is the abort signal for the
   faulting instruction's caller, by design (cpu_state.h's contract).
2. The only clears are `cpu.c:103` (top of the *next* interpreter-loop iteration) and
   `cpu.c:133` (`cycle_internal` entry). Nothing between a delivery and the loop tail clears it.
3. For a faulting **non-boundary** instruction whose handler shares its 4 KiB page, the
   interpreted loop does not break on the block-boundary/page/iteration checks, so it
   reaches `cpu.c:136-137`: `phys_addr = get_phys_eip(); if (cpu->pagefault) return;` —
   the flag is stale, `interpreted_run` returns early, `cpu->instruction_counter += i` is
   **skipped**, and the `DBG_ASSERT(cpu->instruction_counter != initial_instruction_counter)`
   at `cpu.c:145` fires. Assertions are ON in the shipped build (Makefile: no `-DNDEBUG`).
4. Upstream has no persistent flag: each `?` / `return_on_pagefault!` consumes its own
   `Err`, so `return_on_pagefault!(get_phys_eip())` at `cpu.rs:3331` can only fire on a
   *real* fetch fault. A delivered fault does not survive past the instruction upstream.

**MEASURED** (my own probe, not a gate re-run — this is the asymmetry proof for this
finding). A standalone binary linking the real TUs (`/tmp/r5probe1`, same object set as
`make selftest`'s m1c binary): protected mode + paging, flat GDT, a real IDT with a 32-bit
#PF gate, `mov eax,[0x2800]` at 0x1000 (page 2 absent), the handler `inc [0x1800]; add
esp,4; iret` at **0x1010 — the same page**:

```
before: eip=1000 counter=0
src/cpu/cpu.c:145: assert(cpu->instruction_counter != initial_instruction_counter) failed
Aborted (rc 134)   -- on the FIRST delivered fault
```

**Control** (`/tmp/r5probe2`): identical machine, handler at 0x3010 (a *different* page):

```
after 1: eip=3010 counter=1  pf=1     (fault delivered, handler entered)
after 2: eip=1000 counter=4  marker=1 (handler ran, iret back to the faulting instruction)
after 3..6: the fault→handler→iret loop runs indefinitely, marker increments
```

The asymmetry is exactly the prediction: different-page handlers break the loop on the page
check (counter updated, flag cleared at the next `cycle_internal` entry), same-page ones
abort. My first two probe versions also served as sanity checks of the frame layout — the
dump showed `[esp]=error code, +4=eip, +8=cs, +12=eflags`, the hardware order (a probe-side
GDT bug in an early version was caught by exactly this).

**Why no gate sees it.** The corpus boots a multiboot image with `idtr_size` 0 — no fault
is deliverable there (the unit's own report says so). The crafted gate
(`m1c_selftest.c`) drives helpers *directly* and manually replays "the loop's clear"
(lines 248, 301, 413) — it never runs `interpreted_run`/`cycle_internal`, so a
dispatcher-level bug is invisible to it by construction. In NDEBUG builds the same path
silently skips the counter update — corrupting the instruction-limit accounting for
**every** delivered fault, whatever the handler's page.

**Fix.** Clear `cpu->pagefault` at the end of each `interpreted_run` iteration — i.e. add
`cpu->pagefault = false;` immediately after `run_instruction(opcode | ...)` at `cpu.c:110`.
Reasoning: once `run_instruction` returns, the instruction's fault channel is consumed
(every consumer lives inside it, per cpu_state.h's contract — the generated code's checks
and exceptions.c's); the only post-return reader is the fetch check at `cpu.c:137`, which
must see *only the fetch's own* fault. Keep the `cycle_internal` entry clear (it covers a
fetch fault at cycle entry). Add one dispatcher-driven gate: a delivered fault with a
same-page handler run through the real `cycle_internal`.

## SHOULD-FIX S2 — four guest-reachable `DBG_ASSERT`s kept in `instr_0F30` where the release reference proceeds

**Port:** `src/cpu/instructions_0f.c:6280` (`value & ~EFER_NXE == 0`), `:6293`
(`high == 0` for `IA32_APIC_BASE`), `:6295` (`address == 0/APIC_MEM_ADDRESS`), `:6296`
(`low & IA32_APIC_BASE_EXTD == 0`). **Upstream:** `cpu.rs:1197, 1203, 1208, 1213` — the
same conditions as `dbg_assert!`, which is `debug_assert!` (`v86/src/rust/dbg.rs:63-67`),
compiled out of the pinned release wasm.

A guest writing `MSR_EFER` with reserved bits, or `IA32_APIC_BASE` with a nonzero high
word / a relocated address / the EXTD bit, **aborts the shipped machine** where the
reference silently proceeds (it uses the unchecked value). This violates the unit's own
rule (results/m1c-exceptions.md §5.2: no property of the guest's bytes kept as a live
assert) and R4F's N3 resolution in the same pass (the fxsave/fxrstor asserts were dropped
at their sites for exactly this reason). The file's justification — "kept only where they
guard a value the code then USES" — does not distinguish: upstream's release code uses
the unchecked value. **Fix:** drop all four, keeping upstream's message text as comments
(the fpu.c:1152/1197 pattern).

---

## What was checked and holds (the claims the brief asked me to verify rather than accept)

- **R3 abort-and-deliver**: reproduced faithfully. `lookup_segment_selector` callers check
  `cpu->pagefault` in upstream's `?` position; `trigger_pagefault` sets cr2, rewinds
  `instruction_pointer = previous_ip`, and delivers; `do_page_walk` then sets the flag and
  returns the sentinel; nothing in exceptions.c clears it. The selftest's group 3
  assertions (cr2 = the LDT descriptor's linear address, exactly one frame, CS = the #PF
  gate's, machine not stopped) match the code.
- **Pushed-frame layout per vector**: `push32(old_flags); push32(CS); push32(eip);` then
  the error code iff `has_error_code` → `[esp]=error, +4=eip, +8=cs, +12=eflags` —
  hardware order, and the handler (not iret) drops the error code. The 16-bit gate form
  pushes 2-byte arguments with the truncated eip. Selftest groups 1-2 assert these; my
  probe's frame dump confirmed them live.
- **Error-code asymmetry**: `trigger_gp/np/ss` pass one, `trigger_de/ud/nm` do not — the
  only thing upstream's `Option` decides.
- **#GP-vs-#SS selection** (`switch_seg`, cpu.rs:2676-2801): SS + (system/rpl/writable/dpl)
  → #GP, SS not present → #SS, data not present → #NP — all match, error codes included.
- **Privilege checks** in `iret`/`far_return`/`call_interrupt_vector`/`far_jump` read
  line-by-line against cpu.rs:507-797, 799-1126, 1128-1505, 1507-1655 — match, including
  the VIF/VIP handling, the vm86-return arm's register order, and the call-gate parameter
  copy direction.
- **`do_task_switch`/`load_tr`/`load_ldt`** (cpu.rs:1664-1899, 2803-2906) — match,
  including the busy-bit writes, the backlink, CR0.TS, and the dropped upstream
  `dbg_assert!`s (16-bit-TSS ones included, which is correct: the release reference
  proceeds and the port must too).
- **The "no double-fault model" recursion claim** (report §8): accurate. Both sides
  recurse unboundedly on a fault-during-delivery with an unmapped handler stack — upstream
  would trap out of wasm, the port recurses to the guard page. A stated limit, not a
  divergence.
- **PIC** (pic.c vs pic.rs): verbatim, including the priority rule (in-service
  lower-or-equal mask blocks the candidate), the wrapping `isr - 1` u8 subtraction, the
  slave's IRQ-2 forwarding, and the ICW/OCW state machine. Selftest group 5 asserts the
  priority rule and the IRQ-wakes-HLT path.
- **The 31 encodings**: every body read against instructions_0f.rs (91-160, 329-380,
  1183-1421, 3996-4018); the MSR constants against cpu.rs:214-248. The
  `instr16/32_0FC7_6_reg` = `trigger_ud()` answer matches upstream's generated
  `undefined_instruction()`. The only issue found is S2 above.
- **The TSC** (cpu.c vs cpu.rs:4357-4412): transcription exact; report §7's "first read is
  garbage" finding is real (value == tsc_last_value == 0 at reset → extrapolation branch
  with `tsc_resolution = u64::MAX`), honestly reported and correctly deferred to M2.
- **R4F (a) — the repaired predicate** `(fpu_tag & 0xFFFF) != FPU_TAG_ALL_INVALID`
  (tests/nasm_run.py:222) is upstream's intent, not a loosening: the constant is defined
  over the tag word's low 16 bits; the fixture's high half is the kernel's
  `convert_from_fxsr` artifact (mkfixture.c:138-189, the same `%d` print upstream's
  gdb-extract-def uses). It still refuses to compare where upstream's design refuses (the
  st arm for non-empty stacks) and it demonstrably reports mismatches — G1 names
  `mm0.int32[1]`/`mm1.int32[0]` with concrete values. The reference leg passes 113/113
  with the same predicate, so the repair is consistent with the reference emulator.
- **R4F (b) — the G0 asymmetry**: read the raw evidence (results/r4f-gates.txt) rather
  than re-running the gate: the mutation fails 117/118 naming the mm fields, restored
  byte-identical → 118/118, and the reachability count is 0 → 7 over the real fixtures.
  The rig (results/r4f-g0.sh) applies the mutation ALONE and sha-checks the restore. Not
  re-run, per the brief.
- **R4F (c) — N3**: both asserts dropped at their own sites (fpu.c:1152/1197) with the
  reasoning recorded; consistent with R3F's emitter-side precedent; and the stop the
  corpus depends on (the cpu.c:83 jump assert) is still live — the gates show the
  unchanged skip reason. No other stop was silenced by this change.
- **R4F (d) — citations**: six spot-checked against ./v86, all correct: cpuid 3256
  (`instr_0FA2`), 0F FF 5296 (`instr_0FFF`), movlpd 547-552 (`instr_660F12_mem`),
  punpcklbw 2223 (`instr_0F60`), pslld 4916 (`instr_0FF2`), psllq 4935 (`instr_0FF3`).
- **R4F N2**: the four declarations now use the engine's own `float32_t`/`float64_t`
  struct types (engine prototypes at v86/lib/softfloat/softfloat.c:566, 601, 665, 666;
  the i32/i64 ones take plain ints at 502/511), and the four call sites construct/read the
  structs — the report's correction of R4's "zero cost" claim is accurate. The remaining
  pointer-type UB (`f80_t*` vs `const extFloat80_t*`) is reported, not fixed, and the
  report says so (§3/§6).

## Coverage

**Read in full, both sides:** exceptions.c (1841 lines) vs cpu.rs:473-1900, 2638-2906,
3009-3052, 3427-3474, 4546-4568, 4573-4649; exceptions.h (327); pic.c/pic.h vs pic.rs
(all 402); the 31-encoding block (instructions_0f.c:6020-6515) vs instructions_0f.rs:91-160,
329-380, 1183-1421, 3996-4018 + cpu.rs:214-248; lar/lsl/verr/verw (misc_instr.c:693-850)
vs misc_instr.rs:458-613; read_tsc/set_tsc and the run loop (cpu.c) vs cpu.rs:3135-3342,
4357-4412; exec_boundary.c.

**Read in part:** paging.c (fault sites and the flag/delivery interaction; the rest was
R1's surface); m1c_selftest.c (setup, tests 1-3 and 5 in full; groups 4/6/7/8 sampled via
the report's claims plus code-level checks); nasm_run.py's comparison core; fpu.c's
fxsave/fxrstor sites; softfloat.h/.c's N2 declarations and call sites; mkfixture.c's tag
word printing; the Makefile flags.

**Probes written and run** (the B1 asymmetry proof, not gate re-runs): `/tmp/r5probe1`
(same-page handler → abort at cpu.c:145), `/tmp/r5probe2` (different-page control → clean
fault/iret loop). Both link the real translation units with the same flags as the selftest
binary.

**Could not check / out of scope:** the 31 bodies were read but not executed (the gates
were not re-run, per the brief); selftest groups 4/6/7/8 not read in full; the
reference-leg decode in nasm_run.py beyond the comparison core.

**Concurrent mutation, noted for the record:** `pic.c` and `exec_boundary.c` changed sha
*while this review was running* — M2-1 moved the io-port bridge to `src/hw/io.c` and
`run_hardware_timers` to `src/hw/machine.c`. My review of those two files is of the
pre-move content (sha `7608dcf…`, `dda04383…`). The files the findings name
(exceptions.c, cpu.c, paging.c, instructions_0f.c, fpu.c, softfloat.*, nasm_run.py) were
stable throughout. All reviewed shas are on the memory node.
