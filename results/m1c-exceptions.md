# M1c — exceptions, interrupts, the segment machinery, the PIC, and the run loop

**Unit:** `HANDOFF=v86c`, unit M1c (implementation; the last 31 deferred symbols and the
machine's control path). **Reference (READ-ONLY):** `./v86/src/rust/cpu/{cpu.rs,pic.rs}`.
**Raw evidence:** `results/m1c-gates.txt` (the five gates), `results/m1c-gates-mut.txt`
(the asymmetry rig). Every claim below is tagged **MEASURED** (run on this host) or
**READ** (from the reference).

**Bottom line.** The machine takes exceptions, interrupts and a task switch. The
dispatcher's census is **0 unimplemented call sites**, `make selftest` is
**3522 + 193 = 3715 checks, 0 failures**, the NASM corpus is **118/118 passed, 5 skipped**
(byte-for-byte the M1a-4 baseline), and the M0 transcript is still **byte-identical** to
the reference's. What is still stubbed is named in §6 with what a guest observes.

---

## 1. What landed (files, and who owns what)

| File | Lines | What it is |
| --- | --- | --- |
| `src/cpu/exceptions.h` | 327 | The interface: `seg_selector_t`/`seg_descriptor_t`/`intr_descriptor_t` and their accessors as `static inline` (cpu.rs:380-471 verbatim), `lookup_segment_selector`, the TSR_* TSS layout, `do_task_switch`, `load_tr`/`load_ldt`, `trigger_np`/`trigger_ss`, `guest_exception`, and the two one-line helpers `cpl_changed`/`update_cs_size` as inline (see §5). |
| `src/cpu/exceptions.c` | 1841 | cpu.rs:473-1900 (`switch_cs_real_mode`, `get_tss_ss_esp`, `iret`, `call_interrupt_vector`, `far_jump`, `far_return`, `do_task_switch`), 2638-2906 (`lookup_segment_selector`, `switch_seg`, `load_tr`, `load_ldt`), 3009-3052 (`test_privileges_for_io`), 3427-3474 + 4546-4568 (`trigger_de/ud/nm/gp/np/ss`). |
| `src/cpu/pic.c` / `pic.h` | 443 / 48 | pic.rs (402) verbatim + cpu.rs:4573-4649: `handle_irqs`, `pic_call_irq`, the six PIC ports and the device layer's `io_port_*` bridge. |
| `src/cpu/misc_instr.c` | +157 | `lar`/`lsl`/`verr`/`verw` (misc_instr.rs:458-613) — M1a-2's deferral, ported where upstream keeps them. |
| `src/cpu/instructions_0f.c` | +640 | **The 31 deferred encodings**, APPENDED (see §2). |
| `src/cpu/cpu.c` | +95 | `read_tsc`/`set_tsc`/`store_current_tsc` (cpu.rs:4357-4412) — the TSC is a function of `microtick`, which is this file's run-loop layer. |
| `src/cpu/exec_boundary.c` / `.h` | 224 → 61 | The M1c/M2 seat is now the M2 timer seat only (`microtick`, `run_hardware_timers`, `cpu_event_halt`, `fwait`). Every exception/segment/PIC stub it carried is DELETED (no second definition anywhere: `nm` on the objects shows one `trigger_gp`, one `switch_seg`, one `handle_irqs`, one `call_interrupt_vector`). |
| `src/cpu/m1c_selftest.c` | 938 | THE CRAFTED GATE (G3) — see §4 and the divergence in §5.1. |
| `results/m1c-mut-rig.sh`, `results/m1c-mutate.py` | 214 / 26 | The asymmetry rig (G4). |
| `Makefile` | +4 | `CPU_SRC`/`CPU_HDR` gain `exceptions.{c,h}`/`pic.{c,h}`; the `selftest` target also builds and runs `build/cpu_m1c_selftest`. |

**The run loop itself was already landed** (M1a-4, `src/cpu/cpu.c`); this unit's item 5 was
to make the seat's stubs real, which is what the table above does.

## 2. The 31 encodings (the census reaching zero)

**MEASURED, before** (the same command the brief names): `rg -c unimplemented_instruction
src/cpu/interpreter_gen.c` → **32 lines / 31 real call sites** (one of the 32 is the
generated file's own header comment). The set was exactly the brief's list:
`0F 00 /2../5` (lldt/ltr/verr/verw × 2 operand sizes × mem/reg = 16), `0F 02/03` (lar/lsl,
8), `0F 30/31/32/34/35` (wrmsr/rdtsc/rdmsr/sysenter/sysexit), `0F C7 /6` register-form ×2.

**MEASURED, after**: the emitter prints
`[+] src/cpu/interpreter_gen.c: 1922 call sites, 1922 implemented, 0 unimplemented, 1926 names defined`,
`rg -c 'unimplemented_instruction\("' src/cpu/interpreter_gen.c` finds **nothing** (exit 1),
and the only remaining line containing the bare string is `interpreter_gen.c:14`, the
emitter's own header comment. Every one of the 31 is defined in `instructions_0f.c` and
declared in `instructions_0f.h`; `instr16/32_0FC7_6_reg` is `trigger_ud()` (upstream names
no function for the register form, and its generated Rust calls `undefined_instruction()`,
which is the same #UD), and `0F 30`/`0F 32` carry the MSR numbers of cpu.rs:214-248 in a
documented block local to the file (the two instructions are their only users).

## 3. The traps this unit was written around

**R3 (fault delivery order) — READ, and reproduced as upstream's shape, not "fixed".**
`trigger_*` rewinds `instruction_pointer = previous_ip` FIRST and then delivers; a fault
raised *inside* a delivery aborts it (`return_on_pagefault!` is inside
`call_interrupt_vector`) with the partial effects kept, and nothing in `exceptions.c`
clears `cpu->pagefault` — that is the dispatcher's once-per-instruction clear (cpu_state.h).
`m1c_selftest.c`'s group 3 is R3's second half, built to be *reachable*: vector 0's gate
names an LDT selector whose LDT sits in an unmapped page, so the descriptor lookup inside
the in-flight delivery faults, the outer delivery aborts (the new CS is the #PF gate's, not
the #DE gate's), `cr2` is the descriptor's linear address, exactly ONE frame is pushed, and
the machine is NOT stopped. **MEASURED:** with that abort removed (mutation M6) the gate
fails — 2 assertions.

**R4 (the segment machinery) — READ + MEASURED.** All of it is here: the descriptor API,
`switch_seg`'s five checks and the accessed-bit write-back, `lar`/`lsl`/`verr`/`verw`,
`lldt`/`ltr`, `iret`'s privilege-return path, `far_jump`'s call-gate and task-gate arms,
`far_return`, and `do_task_switch` (the old-TSS write-back, the busy bits, TR, CR3 and
CR0.TS). Group 6 of the crafted gate drives a full task switch through a real GDT/TSS and
checks each of those effects by value.

## 4. The gates (the brief's G1-G5)

| Gate | Result | Evidence |
| --- | --- | --- |
| **G1** census → 0 | **PASS** | `make` rc 0; emitter: `1922 call sites, 1922 implemented, 0 unimplemented`; `rg -c 'unimplemented_instruction\("'` = no match. |
| **G2** `make nasm` | **PASS** | `[+] Passed 118/118 tests. [!] 0 of 123 selected test(s) produced no state from the actual leg, 5 were skipped` — **the same 118/5 as the M1a-4 baseline**, and no test moved between skip and pass. `rc 0`. |
| **G3** crafted interrupt/fault gate | **PASS** | `cpu_m1c_selftest: 193 checks, 0 failures`, 8 groups (below). |
| **G4** asymmetry | **PASS** | 8 one-line mutations, each failing then restored green; raw output in `results/m1c-gates-mut.txt`. |
| **G5** `make` / `make selftest` / transcript diff | **PASS** | `make` rc 0; `make selftest` rc 0 (`3522` + `193` checks, 0 failures); `make transcript-diff` rc 0 with an EMPTY diff; `make oracle` rc 0 (the reference side, unchanged). |

**G3's eight groups** (all in `m1c_selftest.c`, all on a machine with a real GDT, IDT, page
directory, LDT and two TSSes built in guest RAM):
1. **#PF through a real IDT**: `cr2`, the frame the handler sees
   (`[esp]=error code, +4=eip, +8=cs, +12=eflags`), the pushed eip being the **rewound**
   `previous_ip` (not the post-decode one), the gate's CS/base/limit/access byte, and the
   `iret32` return to the faulting instruction with the stack restored; plus the read-vs-
   write error-code bit (W=1).
2. **#GP with its error code** in both gate sizes (the 16-bit form's word arguments and
   truncated eip), **#UD with no error code**, and #DE's rewind.
3. **R3's second half** (§3).
4. **Gate types**: an interrupt gate clears IF, a trap gate does not; a software int below
   the gate's DPL is a `#GP(sel|2)`.
5. **The PIC end to end**: ICW1-4 + OCW1 through the port bridge, the OCW1 read-back (the
   byte the guest wrote), the priority rule (irq1 is not acknowledged while irq0 is in
   service), a non-specific EOI, and an IRQ that **clears `in_hlt`**, sets `previous_ip`
   and delivers through a real gate; an unclaimed device port still STOPS with the port
   named.
6. **A task switch through a TSS** (§3/R4).
7. **`switch_seg`** (protected load, the accessed-bit write-back, the null-selector path, the
   `#GP` for an out-of-table selector with its error code) and **`lar`/`lsl`/`verr`/`verw`**'s
   ZF (including the 0-order/1-order invalidity tables).
8. **The 31 encodings** through their own entry points, including a real-mode #UD being
   *delivered* through the all-zero IVT (cs:ip 0:0 with the 3-word frame), `rdtsc`'s
   register pair, the `wrmsr`/`rdmsr` round-trip on `IA32_SYSENTER_CS`, `wrmsr` at cpl 3 as
   a #GP, sysenter/sysexit's flat segment loads and RPLs, and rdrand's #UD.

## 5. EVERY divergence from upstream (and from the brief), with its reason

**5.1 FROM THE BRIEF — G3 lives in its own binary, not in `cpu_selftest.c`.** *MEASURED
reason*: `cpu_selftest.c` supplies its OWN recording definitions of `call_interrupt_vector`,
`trigger_de/ud/nm/gp`, `switch_seg`, `far_jump`/`far_return`/`iret16/32`, `handle_irqs`,
`microtick`, `run_hardware_timers`, `cpu_event_halt` and `fwait` (lines 60-1060), and ~100
assertions in the M1a/M1b groups depend on what those recordings recorded — e.g.
`delivered_interrupt == CPU_EXCEPTION_PF` in the paging group, and `switch_seg_answer =
false` to force a failed segment load in the lss/enter group. The real `exceptions.c`
therefore cannot be linked into that binary (duplicate symbols), and deleting the
recordings would invalidate those checks. **Decision:** the crafted cases live in
`src/cpu/m1c_selftest.c`, which links the REAL `exceptions.c`/`pic.c`/`cpu.c`, and is built
and run by the SAME `make selftest` target — the gate command and its exit status are
unchanged. The five symbols the M1a selftest now needs to resolve
(`lookup_segment_selector`, `load_ldt`, `load_tr`, `read_tsc`, `set_tsc`) got recording
definitions there, the same arrangement `switch_seg`/`trigger_*` already had.

**5.2 FROM UPSTREAM — no plain `dbg_assert!` is kept as a `DBG_ASSERT`.** *MEASURED reason,
and this one bit during development*: the first draft kept upstream's
`dbg_assert!(offset <= cs_descriptor.effective_limit())` (cpu.rs:899) and the crafted gate
ABORTED on it — for a gate whose selector points at a descriptor that is not a code
segment, which is legal guest input the release reference simply carries on from
(`dbg_assert!` is `debug_assert!`, dbg.rs:63, compiled out of the pinned wasm). The assert
was the divergence, not the guest. All 36 such asserts are dropped; the only `DBG_ASSERT`s
left are upstream's `x.unwrap()` on a range the code has already proved fault-free, kept as
`DBG_ASSERT(!cpu->pagefault)` with the reading `misc_instr.c` records.

**5.3 FROM UPSTREAM — the `panic!` arms are STOPS, and the ones reachable from a guest's
IDT are recorded.** Upstream's `panic!` is a trap: the emulator cannot proceed. The port
calls `unimplemented_instruction()` with the reason, which halts the machine without
producing a fabricated state (the same stop a missing opcode uses). Where the panic happens
while a GUEST EXCEPTION is being delivered (an empty/too-small IDT, an invalid gate type,
the reserved-zeros violation), the vector is recorded in `guest_exception` first:
`main.c`'s fixture mode maps it to the host signal the native harness took, which is what
the NASM differential compares. This is the ONE path the five class-A corpus tests
(`idiv8/16/32-overflow`, `mov_sreg_ud1/ud2`) take — they PASS through it (**MEASURED**: the
fixtures print `(signal SIGFPE)` / `(signal SIGILL)`, and mutation M7, which records the
wrong vector, makes 5 tests produce no state).

**5.4 FROM UPSTREAM — `guest_exception`'s meaning NARROWED, and `main.c`'s comment with it.**
The seat's record meant "the vector the guest took (the port cannot deliver)". Delivery
exists now, so it means "the vector of a fault the machine STOPPED on because the delivery
could not proceed". A fault a guest CAN have delivered is delivered and recorded nowhere.
`main.c`'s fixture comment is updated to say so rather than to keep the wider claim.

**5.5 FROM UPSTREAM — the timer layer's unit is the guest instruction (kept from M1a-4).**
`microtick` returns `cpu->instruction_counter`, not a wall clock; the reason (a
deterministic oracle, and a yield check that is not dead) is the seat's comment. Its
CONSEQUENCE for the TSC is a finding of its own — §7.

**5.6 FROM UPSTREAM — `set_cr3`'s cr3 in `do_task_switch` is the masked value.** Upstream
masks inside `set_cr3` exactly as the port does (cpu.rs:2959-2975); nothing diverges, noted
only because the crafted gate asserts the masked value.

**5.7 NOT PORTED — `device_raise_irq`/`device_lower_irq` (cpu.rs:4596-4610).** Their only
callers are v86's JAVASCRIPT devices and their bodies also drive the IOAPIC (apic.rs,
deferred to M2). The PIC's own device interface (`set_irq`/`clear_irq`) IS ported, so M2's
device layer adds those two with the IOAPIC. The APIC arm of `handle_irqs` is present but
unreachable (`acpi_enabled` is false for every machine this unit can run — the loader never
sets it).

**5.8 `cpl_changed`/`update_cs_size` are `static inline` in `exceptions.h`,** not
definitions in `exceptions.c`: `instructions_0f.c` (0f 34/35) calls them and is in BOTH
source sets, while `exceptions.c` is in only one (the M1a selftest replaces its symbols).
An inline definition has no link dependency and, unlike a stub in the test binary, there is
exactly ONE copy of the logic.

**5.9 `reset_cpu` still does NOT call `set_tsc(0,0)` (cpu.rs:4744).** The port's
`tsc_offset` starts at 0, which is exactly what `set_tsc(0,0)` computes under the port's
deterministic microtick (`read_tsc()` at reset = `instruction_counter * TSC_RATE - 0` = 0),
so the initial value is right without the call. Residue, stated: a `reset_cpu` AFTER a guest
wrote `IA32_TIME_STAMP_COUNTER` would leave a stale `tsc_offset`. Unreachable in this unit's
gates (main.c calls `cpu_state_init` once) and in every current gate; it is M2's to close
when the TSC gets a real clock.

## 6. What is still stubbed, and exactly what a guest observes

1. **`microtick`/`run_hardware_timers`** (exec_boundary.c, READ-ONLY seat): `microtick` is
   the instruction counter; `run_hardware_timers` returns its argument. **A guest observes:**
   time never advances, and `main_loop`'s yield check is a real batch boundary rather than a
   clock.
2. **The PIT/RTC/keyboard/etc. ports** (pic.c's `io_port_*` default arm): unclaimed ports
   STOP the machine with the port named (`io_port_read8(port=0x40) (device layer, M2)`) —
   a guest reading a device register gets a stop, never a fabricated value. **MEASURED:** the
   crafted gate asserts the stop.
3. **No device ever raises an IRQ.** The PIC is fully programmable and readable, but nothing
   calls `set_irq` except a test. **A guest observes:** `sti; hlt` IDLES FOREVER — no timer
   interrupt arrives, so a guest waiting for one makes no progress. (`cpu_event_halt` does
   nothing; `in_hlt` is cleared only by `pic_call_irq`, i.e. by a delivered IRQ.)
4. **`fwait`** (fpu.rs:803, empty body) stays in the seat; `fpu.h`'s note records that the
   move to fpu.c is blocked by `cpu_selftest.c`'s own `fwait` recording definition.
5. **The APIC arm of `handle_irqs`** and **`apic_read32`/`ioapic_*` stubs** — unchanged
   from M1a/M1b (M2).
6. **The TSC's first read** — §7.

## 7. Open finding (reported, not papered over): the TSC's first read is garbage

**MEASURED.** `set_tsc(0, 0x55667788, 0x11223344)` followed by `read_tsc()` returns
`0x5566778811223346` — the set value **+ 2**. The cause is the port's own microtick, not a
transcription error: cpu.rs:4365 is `value = microtick() * TSC_RATE - tsc_offset` and the
browser workaround (cpu.rs:4371-4381) extrapolates whenever `value == tsc_last_value`. On a
fresh C machine `microtick()` is `instruction_counter` = **0 exactly**, so the very first
read takes that branch with `tsc_resolution` still seeded at `u64::MAX` and returns
`0 + (u64::MAX - 1)`; `set_tsc` then builds its offset on that value. Upstream never sees it
because its `microtick` is a wall clock — nonzero on the first read. **Impact:** no gate
compares a TSC value (the NASM corpus executes no `rdtsc`; MEASURED), so nothing in this
unit's gates is affected. **Recommendation (M2, whoever makes `microtick` a real clock):**
seed `tsc_resolution` to something small (or skip the workaround while the counter is 0).
**Not fixed here**: inventing a guard would be new semantics, and the honest state is
recorded instead. The crafted gate asserts the invariant that does hold (the value is at or
above `set_tsc`'s base, within one extrapolation step) and says why in a comment.

## 8. Sub-finding worth M2's attention: a fault inside a delivery that pushes to a bad stack

If a fault's delivery needs the stack and the stack itself is unmapped, the delivery faults
again and the port recurses through `trigger_pagefault` → `call_interrupt_vector` → the
push → `trigger_pagefault` … Upstream has no double-fault model either (it would trap out of
wasm), and this unit does not build one. **MEASURED once, during development**: with an
unmapped stack the m1c binary runaway-recursed until it hit the guard page (the probe run
printed `PROG` lines without bound). The crafted gate therefore faults at the GATE LOOKUP
(where the stack is still good), which is the R3 case the plan names. Stated as a limit, not
a bug: a guest whose handler stack is unmapped gets a stack-overflow stop, not a modelled
double fault.

## 9. Report of the process, for the next unit

- The plan's §7 steps 17-19 and §8 R3/R4 were accurate on contact with the code, with one
  exception: `lss` was already implemented (M1a-2) and only needed `switch_seg` to become
  real; the brief's list of 31 symbols was exactly right, and `rg -c` counting the
  generated file's header comment is why the census reads 32 lines before and 1 after.
- `results/m1c-gates-mut.txt` is the asymmetry record: **M1** (the rewind) is caught by BOTH
  the corpus (118 → 115/118) and the crafted gate; **M7** (the recorded vector) only by the
  corpus; **M2-M6, M8** only by the crafted gate. That division is the measured answer to
  "is G3 a real gate or a duplicate of the corpus": the corpus boots a multiboot guest with
  `idtr_size` 0, so a DELIVERED vector is unreachable in it at all.
- The rig itself had a bug worth recording: it backed up to `<file>.mutbak` next to the
  source, and a run that died mid-mutation left a stale backup, so the NEXT run's backup
  saved a mutated file. It now backs up to `/tmp`, removes the backup before each run,
  restores on every exit path (`trap`), and reports a non-compiling mutation as such instead
  of counting it as caught. (Recovered by re-applying the two intended edits to a clean copy
  and re-running every gate.)
