# PERF-FIX-2 — the 15th assert site, the tree-wide sweep of its class, and the benchmark's pin

**Unit:** `HANDOFF=v86c-perf`, unit PERF-FIX-2 (coder). **Inputs:** `/tmp/brief-v86c-perf-fix2.txt`,
`results/perf-fix.md` §4 findings 2 and 3, `handoff-v86c-perf-ctx/-plan/-result`,
`results/r3f-fixes.md`, `results/r4f-fixes.md` §4, `results/r5f-fixes.md` §3. **Files written:**
`src/cpu/instructions.c` (three sites), `tests/perf_bench.sh`, and the new artifacts
`results/perf-fix-2-sweep.py`, `results/perf-fix-2-verdicts.py`,
`results/perf-fix-2-sweep.tsv`/`.md` → `results/perf-fix-2-sweep.md`,
`results/perf-fix-2-sweep-verdicts.tsv`, `results/perf-fix-2-probe.asm`,
`results/perf-fix-2-probe.sh`, `results/perf-fix-2-cpu100.sh`,
`results/perf-fix-2-instructions-prefix.c.gz`, and the gate captures
`results/perf-fix-2-gates-{make,selftest,nasm,bench,g3}.txt`.

**CONCURRENCY, STATED UP FRONT.** A **second coder** (not the reviewer the brief names) was
writing in this directory for the whole of this unit's run — `fx-agent-presence` at 23:59 shows
pid 1209381 in `/home/jaye/futaie/souche`, and `src/cpu/{uop_cache.c,memory.c,uop_exec_selftest.c}`
carry mtimes AFTER this unit's edit (`instructions.c` 23:47:38; theirs 23:52:14, 23:55:35,
00:00:31). Every number below is therefore anchored to the source hashes recorded beside it, and
the one gate that is RED is theirs, attributed by file and line (§4).

---

## 0. Bottom line

| item | delivered | evidence |
|---|---|---|
| **A1 the 15th site** (`cpu.c:100`) | **KEPT, with the class claim REFUTED BY MEASUREMENT** — dropping it does not match the release reference, it moves the stop one frame later and, with its twin also gone, gives a **host SIGSEGV**; the reference on the same guest runs 1e6 instructions of wasm-fabricated memory and never halts | `results/perf-fix-2-cpu100.sh` / `.txt` (§1.2) |
| **A2 the sweep** | every `DBG_ASSERT` in `src/`: **340 sites, 120 distinct conditions**, each with a verdict — 47 of the 120 have an upstream `dbg_assert!` and are adjudicated individually; the rest by rule with a stated reason code | `results/perf-fix-2-sweep.md` (121 rows), `results/perf-fix-2-sweep.py` (rc 0, `groups unverdicted: 0`) |
| **A3 what the sweep FOUND** | **three more sites of the class, in the class's SECOND FORM** (upstream `dbg_assert!(false)` under the same guest-input guard): `instr32_9A`, `instr32_FF_3_mem` (callf m16:32), `instr32_FF_5_mem` (jmpf m16:32) — **dropped at their sites** | `results/perf-fix-2-probe.sh` / `.txt`: before rc=134 at each site, after rc=0 with a transcript **byte-identical to the reference's** (§1.3) |
| **B the benchmark pin** | the pin was **CORRECT all along** (200095214, 10/10 runs; PERF-FIX's 200095212 was the mid-flight M2 sti bug); the benchmark's *identity assertion* is rewritten to be about the **guest-visible** result — the terminal machine state and the guest's serial bytes — with the count kept (it has detection power) and the mechanism documented in the failure message | `results/perf-fix-2-gates-bench.txt`, `§2`, G3 in §4 |
| **G4** | `make` rc 0; `make nasm` **118/118** with the same 5 skips, cache ON **and** OFF; `make transcript-diff` byte-identical; `make bench` best **3.256 s** / median 3.286 s = **61.45 MIPS**; `make selftest` **rc 0**, all five suites green (the concurrent unit's suite was RED mid-flight on its own new tests and cleared on its own fix — attributed, §4) | `results/perf-fix-2-gates-*.txt` |

---

## 1. ITEM A — the 15th site and the sweep of its class

### 1.1 The class, restated in its two forms

Upstream guards code with `dbg_assert!`/`debug_assert!` (`v86/src/rust/dbg.rs:63`), which is
**compiled out of the pinned RELEASE reference**. The port emitted those as live `DBG_ASSERT`
(= `assert`, no `-DNDEBUG`), so guest bytes can abort the machine where the reference carries on.
The class has **two spellings** upstream, and PERF-FIX closed only the first:

* `dbg_assert!(<condition>)` where the condition is a property of the GUEST's input — PERF-FIX's 14
  (`instructions.rs:1319..2474`, closed in `results/perf-fix.md` ITEM 2);
* `dbg_assert!(false)` inside a guest-input guard — a "this cannot happen, but if it does, it is the
  guest's bytes" that in release silently falls through. **That is the form this unit found.**

This is what PERF-FIX's own caveat meant by "one grep's inventory, not a tree-wide sweep": the grep
matched the first spelling.

### 1.2 The named site, `src/cpu/cpu.c:100` — KEPT, and why dropping it is NOT the class's remedy

`get_phys_eip`'s `DBG_ASSERT(!in_mapped_range(phys_addr))` is upstream `cpu.rs:3295`, and it is a
`dbg_assert!` the release reference compiles out. So the *provenance* claim in the brief is right.
The **class's remedy** (drop it, so the port does what the release reference does) is **wrong here**,
and this is MEASURED, not argued (`results/perf-fix-2-cpu100.sh`, raw output
`results/perf-fix-2-cpu100.txt`; the guest is the nasm corpus's own `jump` image, which is the test
that reaches this site):

| leg | result |
|---|---|
| the tree as it ships | **rc=134**, `src/cpu/cpu.c:100: assert(!in_mapped_range(phys_addr)) failed` |
| that ONE line deleted | **rc=134**, `/build/pf2-cpu100/cpu.c:112: assert(!in_mapped_range(phys_addr)) failed` — the **same stop, one frame later** (upstream `cpu.rs:3301`, i.e. a *different* upstream site, in `interpreted_run`; the uop cache falls back into it here) |
| **BOTH** guards deleted | **rc=139 SEGMENTATION FAULT**, no state at all — the raw `mem8[phys_addr]` read outside the allocation |
| the pinned reference, same image | rc=3, **"guest did not halt within 1000000 instructions"**, eip stuck at `0x42b001`, executing wasm memory past the 2 MiB of guest RAM |

Three things follow, each checkable:

1. **Dropping the site does not make the port match the reference** — on these bytes the reference
   does not "continue" in any sense the port could reproduce: it executes *wasm-fabricated* memory
   (zeros past `memory_size`, inside its larger linear memory) and never halts. Dropping the check in
   C reads **host** memory that no one allocated.
2. **The site is not a release-compiled-out invariant whose condition the reference ignores; it
   stands in for a mechanism the reference has IN RELEASE.** In Rust/wasm, `mem8[phys_addr]` is a
   *bounds-checked* linear-memory access: it traps. MEASURED (PERF-FIX's MODE=5, re-observed here):
   the reference dies with `RuntimeError: memory access out of bounds`. The port's `mem8` is a flat
   host pointer with **no** such check, so the check MUST be explicit — and the only difference left
   is where the reference's own check fires (its linear memory is bigger than the guest's RAM; the
   port's buffer is the guest's RAM).
3. **The site is redundant with its twin, and the pair is not removable**: cpu.c:100 is
   `get_phys_eip`'s (upstream 3295) and cpu.c:113 is `interpreted_run`'s (upstream 3301). Deleting
   only the first is behaviour-neutral (measured: the stop is unchanged in kind); deleting both is a
   host out-of-bounds read. So the honest verdict is **KEEP**, on the ground that the remedy's
   *premise* (the reference proceeds with the same value) is false at this site.

**This is where the brief is wrong, not the execution** (flagged for a re-plan, not escalated):
ITEM A's opening — "resolve it the way the precedent does — at the site, matching the release
reference's behaviour" — cannot be honoured at `cpu.c:100` without introducing a guest-controlled
host out-of-bounds read, because the reference's behaviour on that input is a trap/hang and not a
continuation. `src/cpu/cpu.c` is therefore **unchanged** by this unit.

**No other stop was silenced, stated as an observable** (G1): `make nasm` is **118/118, 5 skipped,
the same list and reasons** (`results/perf-fix-2-gates-nasm.txt`), and the stop the corpus depends
on is **still live at the site the brief names**, MEASURED directly rather than read off the skip
list (the skip list's `jump` reason string says `cpu.c:83`, a *stale pre-M2 line number* — the same
assert, moved by M2's insertions): `results/perf-fix-2-cpu100.txt` line 1 shows the shipped tree
stopping at `src/cpu/cpu.c:100` on `build/nasm/jump.img`.

### 1.3 The sweep: 340 sites, one verdict each

**Method** (`results/perf-fix-2-sweep.py`, re-runnable, rc 0):

1. every `DBG_ASSERT(` invocation in `src/**/*.{c,h}` with its file:line and enclosing function,
   from COMMENT-STRIPPED source (so the two comment mentions of `DBG_ASSERT(!cpu->pagefault)` in
   `exceptions.c` are not counted as sites);
2. for each condition, whether upstream has a `dbg_assert!`/`debug_assert!` with the same
   normalised condition — the class's defining property, mechanised instead of recalled (the
   normaliser drops the C/Rust cast and numeric-literal spellings; the one place it also drops
   redundant parentheses is stated in its docstring, and every group it newly matched was read);
3. grouping by condition, because 121 distinct conditions cover 340 sites (one condition is 135 of
   them) — the grouping is what makes a per-site verdict writable AND reviewable;
4. the verdicts (`results/perf-fix-2-verdicts.py` → `results/perf-fix-2-sweep-verdicts.tsv`) are
   joined in **by key**, and the sweep **exits 3 on any un-verdicted group**, so a new assert
   cannot enter the tree silently.

**Result:** **340 invocations, 120 distinct conditions, 47 with an upstream `dbg_assert!`
counterpart, 0 unverdicted.** The full table (condition, every site with file:line(function),
upstream counterpart, verdict, why) is `results/perf-fix-2-sweep.md`.

**The verdict rule, in one line each** (the reason codes are the table's `verdict` column):

| code | meaning |
|---|---|
| **DROP** | the class, AND the guest can falsify it in a non-buggy run, AND the release reference then proceeds with the same value and so does the port after the drop (the value is used as an address/arithmetic operand, not as an index into a host array) |
| **(a) PORT-INVARIANT** | no upstream `dbg_assert!` here: the condition is on the port's own state or on the device/registration API; falsifying it means a PORT bug (and stopping there is the point — MEASURED once: `modrm.c`'s default arm caught M2's own wrong addressing call, `results/perf-4-uop-exec.md`) |
| **(b) HOST-BOUND** | the checked value is used as a raw `mem8` address or as an index into a host array. The reference's equivalent access is bounds-checked by the **wasm VM in release** (it traps — MEASURED), the port's flat pointer is not, so dropping it is a host out-of-bounds read (MEASURED: rc=139) |
| **(c) UNREACHABLE** | the class's condition, but the operands are masked/normalised before the call, or the arm is a switch default over a bounded field — sometimes with the measurement that shows it (`F2 F2 90` runs green because `run_prefix_instruction` clears the accumulator) |
| **(d) DELIBERATE** | the port's documented D2 policy stops where the reference's release build silently does nothing (`0x62` BOUND, `0xF1` INT1, the emitter's own default arms). A **named divergence**, not this unit's to change |
| **(e) UNVERIFIED** | reachability NOT established by this unit; flagged for the reviewer rather than asserted either way — **2 groups**, both `virtqueue_set_size`'s guest-written fields (`(size & (size-1)) == 0`, `size <= queue->size_supported`) |

**The DROP verdicts — the class's sites, 17 total:**

| # | port sites | upstream | why it is the class |
|---|---|---|---|
| 1-14 | `src/cpu/instructions.c` (PERF-FIX, already landed) | `instructions.rs:1319..2474` | `dbg_assert!(*is_32 \|\| value < 0x10000)` on 14 control transfers |
| 15 | `instructions.c` `instr32_9A` (callf ptr16:32) | `instructions.rs:1001-1005` | `dbg_assert!(false)` under `!protected_mode \|\| vm86_mode()` |
| 16 | `instructions.c` `instr32_FF_3_mem` (callf m16:32) | `instructions.rs:2462-2469` | as above |
| 17 | `instructions.c` `instr32_FF_5_mem` (jmpf m16:32) | `instructions.rs:2490-2494` | as above |

The three new ones are the **second form**, and they are closed in this unit (§1.4).

### 1.4 The three sites this sweep found and closed (G1's asymmetry)

`src/cpu/instructions.c`: `instr32_9A`, `instr32_FF_3_mem`, `instr32_FF_5_mem` carried a live
`DBG_ASSERT(((uint32_t)new_ip & 0xFFFF0000u) == 0)` where upstream has

```rust
if !*protected_mode || vm86_mode() {
    if 0 != new_ip as u32 & 0xFFFF0000 { dbg_assert!(false); }
}
```

`dbg_assert!(false)` is compiled out of the release reference, which therefore `far_jump`s with the
**full 32-bit** `new_ip`; the port aborted. Dropped **at their sites** (the R3F N1 / R4F N3 / R5F /
PERF-FIX rule), with the class reasoning in the file header and a one-line pointer at each site.
The value is not indexed and not shifted — it becomes `cpu->instruction_pointer = get_seg_cs() + eip`
in both implementations — so the guest is simply allowed a ≥ 64 KiB target, exactly as upstream is.

**The asymmetry, MEASURED** (`sh results/perf-fix-2-probe.sh`, raw: `results/perf-fix-2-probe.txt`;
one leg = the tree, the other = the same tree with the **pre-`perf-fix-2` `instructions.c`**
substituted into the unity TU, sha256-verified against
`results/perf-fix-2-instructions-prefix.c.gz` = `38256a3b…`; both legs' unity-source snapshot
`8f057f28…`, identical):

| mode | guest | BEFORE (pre-fix file) | AFTER (the tree) | AFTER vs the reference |
|---|---|---|---|---|
| **6** | `66 FF 1E` callf fword (new_ip `0x10120`) | **rc=134**, `instructions.prefix.c:2320` | **rc=0** | transcript **IDENTICAL** (eip 0x10022e, 12 instrs) |
| **7** | `66 9A` callf ptr16:32 (new_ip `0x10120`) | **rc=134**, `:1156` | **rc=0** | transcript **IDENTICAL** |
| **8** | `66 FF 2E` jmpf fword (new_ip `0x10120`) | **rc=134**, `:2353` | **rc=0** | transcript **IDENTICAL** |
| **9** | the control: the same instruction with new_ip `0x330` (< 64 KiB) | rc=0 | rc=0 | transcript **IDENTICAL** (eip 0x10032e) |

The control isolates the abort to the **value**, not the opcode or the mode. (The probe's own
history is in the file: its first form used `67`-prefixed disp32 addressing and reached nothing —
MEASURED, **both** the port and the reference decode that as a 16-bit displacement in a 16-bit code
segment, agreeing to the byte (`eip 0x31000, esp 0xf8`), so the fword is placed in low memory and
read with a genuine disp16 instead. That is an upstream-inherited model detail, recorded, not a port
defect this unit introduced.)

### 1.5 What the sweep did NOT find

No other DROP. In particular, the candidates a reader would expect to be reachable are **not**:

* the shift-count asserts (`count >= 0 && count < 32`, 25 sites) — every caller masks
  (`imm & 31`, `read_reg8(CL) & 31`, `instructions.c:1414-1459,1678`);
* `add8/add16/cmp8/cmp16`'s operand ranges (8 sites) — the callers pass `read_reg8/16`;
* `rtc_cmos_read`'s `index < 128` — the CMOS index is **masked at the port write**
  (`rtc.c:65`, `cmos_index = value & 0x7F`, rtc.js:77-81);
* `instr_F2`/`instr_F3`'s `(prefixes & PREFIX_MASK_REP) == 0` (upstream `instructions.rs:2159+`,
  identical) — MEASURED not reachable: `F2 F2 90` on the current tree runs green and matches the
  reference, because `run_prefix_instruction` clears the accumulator before re-dispatching;
* `set_mxcsr`'s reserved-bit check — upstream `cpu.rs`'s own comment says "checked by caller", and
  the port's caller (`instructions_0f.c:1425`) triggers `#GP` first;
* the unknown-MSR `dbg_assert!(false)` pair (`instructions_0f.rs:1267/1351`) — **already dropped**
  by R5F, and the port's file says so at both sites;
* the TSC "overshot" assert (`cpu.rs:306-308`, `#[cfg(debug_assertions)]`) — provably unreachable
  because the branch above clamps `extra` to `< tsc_resolution - 1`.

---

## 2. ITEM B — the benchmark's instruction pin

### 2.1 The premise is refuted; the pin was right

The brief (and `results/perf-fix.md` finding 3) says the tree reports **200095212** against the
script's pinned 200095214 and that "the count moves run to run". **MEASURED on the current tree:
10 of 10 runs report 200095214**, with the identical terminal state, and every run recorded in
`results/perf-4/` (cache ON and OFF) reports 200095214 too. PERF-FIX's 200095212 was the
**mid-flight M2 tree**, and M2's own report names the cause: *"every instruction_counter after the
first sti in the boot was 2 low (200,095,212 vs 200,095,214) and the guest's own clock moved (the
/proc/interrupts XT-PIC count read 861 vs the pin's 874)"* — i.e. **the pin caught a real bug**, and
it was never a host-clock artefact of this workload.

The host-clock variance is real but belongs to the **other** workload: the boot-to-HLT kernel boot
(579M instructions, `results/m2-5-baliveau-kernel.md:171-175`) moves by tens of thousands run to run
because the guest's own timing decides when it halts. This workload stops at a **fixed
200,000,000-instruction limit**, so its count is the batch boundary nearest that limit — stable, but
a property of the guest's *path*, not of its results.

### 2.2 The decision: keep the pin, and assert the guest-visible result beside it

Neither (a) re-pin with a tolerance nor (b) drop the count. What was actually wrong is what the
brief's last sentence says: **the benchmark's output did not say what it was asserting**, so a
passing number could be read as a statement about guest-visible behaviour. So:

* **the count pin is KEPT** (it has detection power, proven by M2's bug), and its failure message
  now states the **mechanism** (the batch boundary nearest the limit; the guest reads the host-clock
  RTC in its timing loops, so a moved count means a moved *path*) and cites the M2 case;
* **the guest-visible identity is now asserted too**: `EXPECT_STATE` — the terminal machine state
  (`in_hlt=0, exception=-1, unimplemented=0, eip=0x21972c1 cs=0x10 cr0=0x60000011 cr3=0x0`), checked
  every run — and `EXPECT_SERIAL_SHA`, the sha256 of the guest's own serial output
  (`fae4924b…`, the 30 bytes `minibios: option ROM at c000` at this limit). Both were MEASURED
  identical across runs with the cache ON and OFF;
* the script now **prints what it asserts** before the timings, and closes the report with a line
  saying that every number in the output *times* that workload rather than asserting it.

### 2.3 G3 — the assertion does its job (three perturbed arms, each rc=1)

Raw: `results/perf-fix-2-gates-g3.txt`.

| arm | result |
|---|---|
| a **perturbed workload**: `--limit 199000000` | rc=1, names the count it got and the pin, and states the mechanism ("the batch boundary nearest the 199000000-instruction limit…") |
| a **perturbed state pin**: `--expect-state "… cr0=0xdeadbeef …"` | rc=1, prints `got`/`expected` and says **"THE GUEST-VISIBLE RESULT CHANGED — this is the assertion that must never pass silently (eip/cs/cr0/cr3/in_hlt/exception)"** |
| a **perturbed count pin**: `--expect-instrs 999` | rc=1, names the count |

A benchmark that cannot fail is not a benchmark: all three arms fail, each naming what changed.

---

## 3. Files changed

| file | change | sha256 (first 20) |
|---|---|---|
| `src/cpu/instructions.c` | **three sites dropped** (`instr32_9A`, `instr32_FF_3_mem`, `instr32_FF_5_mem`) + the class's second form added to the file header's class note | `0d6900dcac7d09d5c29b` |
| `tests/perf_bench.sh` | the count's mechanism documented in the header; `EXPECT_STATE` + `EXPECT_SERIAL_SHA` pinned and checked per run; `--expect-state`; failure messages name what changed; the summary states what is asserted | `d08ea57647e0b42e6bfb` |
| `results/perf-fix-2-sweep.py` (new) | the inventory: every `DBG_ASSERT`, its function, its upstream counterpart, grouping, rc 3 on an un-verdicted group | `335ee6a7e103bf1ce2a1` |
| `results/perf-fix-2-verdicts.py` (new) | the judgement half: the verdict rule + one verdict per group, written to the TSV | `d56877d43997c574b2e3` |
| `results/perf-fix-2-probe.asm` + `-probe.sh` (new) | the second form's probe guest and the before/after rig | `9b1044aad1d583283ff2` |
| `results/perf-fix-2-cpu100.sh` (new) | the cpu.c:100 decision rig (three legs + the reference) | — |
| `results/perf-fix-2-sweep.md`, `-sweep-verdicts.tsv` (new) | the table (121 rows) and its machine-readable verdicts | — |
| `src/cpu/cpu.c` | **UNCHANGED** (§1.2 — the brief's remedy is refuted by measurement) | `8b46c9597f756221ed0b` (as M2 left it) |

`v86/` was never written (READ-ONLY). No commit. The git tree is absent, so "restored" claims are
hash-based: the rigs verify the pre-fix `instructions.c` against the stored artifact before use
(`38256a3b…`), and `perf-fix-2-cpu100.sh` writes its mutated `cpu.c` to `build/pf2-cpu100/`, never
into `src/`.

---

## 4. Gates (raw output in `results/perf-fix-2-gates-*.txt`)

| gate | command | rc | result |
|---|---|---|---|
| G4a | `make` | **0** | generators report `unchanged`; no warnings |
| G4b | `make selftest` | **0** (final) / *2 earlier, attributed* | `cpu_selftest` **3522 checks, 0 failures**; `cpu_m1c_selftest` **228, 0**; `hw_selftest` **541, 0, 0 skipped**; `uop_cache_selftest` **7,934,349, 0**; **`uop_exec_selftest` 63 checks, 3 FAILURES** at `src/cpu/uop_exec_selftest.c:527/528/624` — **the concurrent coder's in-flight new tests, since RESOLVED: a re-run of the same command is now **rc 0** with all five suites green (`uop_exec_selftest` 63 checks, 0 failures; raw `results/perf-fix-2-gates-selftest-final.txt`) (their file's mtime 00:00:31, after every file this unit touched; the three functions existed but were not yet wired into `main()` when `make selftest` first failed here, then failed on their own assertions). **Not attributed to this unit by construction**: `src/cpu/uop_exec_selftest.c` is untouched by it and the failures are in tests that did not exist when this unit started |
| G4c | `make nasm` (cache ON) | **0** | **118/118**, 0 produced no state, **5 skipped, same list and reasons** |
| G4c' | `tests/nasm_run.py --actual "build/v86c-nasm {img} {limit} --fixture --uop-cache off"` | **0** | **118/118**, 5 skipped — the same counts with the cache OFF |
| G4d | `make transcript-diff` | **0** | "the C transcript is byte-identical to the reference's" |
| G4e | `make bench` (7 runs, load ≈ 12–15) | **0** | best **3.256 s** / median **3.286 s** = **61.45 / 60.89 MIPS**, **7/7 runs at 200095214**, `state=ok serial=ok` on every run |
| G1 | `sh results/perf-fix-2-cpu100.sh` | 0 | the site's verdict, §1.2 |
| G1' | `sh results/perf-fix-2-probe.sh` | 0 | the asymmetry, §1.4 |
| G2 | `python3 results/perf-fix-2-sweep.py --md …` | **0** | 340 invocations, 120 conditions, 47 with an upstream counterpart, **0 unverdicted** |
| G3 | the three perturbed arms | 1,1,1 | §2.3 |
| **final** | `make`, `make transcript-diff`, `make nasm`, `make bench` re-run on the tree AS THIS UNIT LEAVES IT (the concurrent unit's last edits included, source hashes printed in the capture) | **0/0/0/0** | generators `unchanged`; transcript byte-identical; **118/118**, 5 skips; best **3.193 s** / median **3.256 s** = **62.67 / 61.45 MIPS**, 7/7 at 200095214 with `state=ok serial=ok`. Raw: `results/perf-fix-2-gates-final.txt` |

> Note for the next runner: the nasm harness must be pointed at the **wrapper** `build/v86c-nasm`
> (`#!/bin/sh; exec ./build/v86c "$@"`), not at `build/v86c` directly — Python's `subprocess` gets
> `OSError: [Errno 8] Exec format error` on the APE. That is why the Makefile's `nasm` target, and
> the cache-off command above, use the wrapper.

---

## 5. Findings on the brief

1. **The 15th site is not a DROP, and the brief's framing of it is wrong on contact with the code**
   (§1.2). Its provenance (an upstream `dbg_assert!`) is right; the *remedy* is not, because at this
   site the release reference does not proceed with the same value — it traps or executes
   wasm-fabricated memory — and the port's substitute for that (a bounds check it must have) is a
   **release-live** mechanism in the reference. Recommended re-plan: treat `cpu.c:100`/`:113` as
   (b)-class boundaries, not class sites.
2. **ITEM B's premise is refuted**: the pin is correct today and the 200095212 in the record was the
   mid-flight M2 sti bug, i.e. the pin caught a real bug (§2.1). What needed fixing was the
   benchmark's *statement*, and that is what was fixed.
3. **PERF-FIX's caveat was well founded**: the tree-wide sweep found **three more** class sites, in
   the class's second form, all reachable from two to six guest bytes (§1.3/§1.4).
4. **The `jump` skip's reason string is stale** (`src/cpu/cpu.c:83`, a pre-M2 line number). The stop
   it names is live at `cpu.c:100`; PERF-FIX, R3F, R4F and R5F all used that stale string as their
   "no other stop was silenced" observable, which is a *re-read of a file*, not a measurement. This
   unit replaced that with a direct run of the image (`perf-fix-2-cpu100.sh`) and did **not** edit
   the skip list (its own file, and the reason text is a historical record).

## 6. What this unit did NOT prove (for the reviewer)

* **The (e) rows**: `virtqueue_set_size`'s `(size & (size - 1)) == 0` and
  `size <= queue->size_supported` take a value the **guest** writes through the virtio config bar; I
  did not establish what the JS reference does with a non-power-of-two size (the pinned bundle is
  minified and carries no `dbg_assert`). Reachability is the open question, not the port's memory
  model — a follow-up unit with the reference's source at hand should close it.
* **The (b) class's boundary is stated, not reduced to one rule**: `!in_mapped_range(...)` sites
  (cpu.c:100/113, modrm.c:121, memory.c's blob path) are all KEEP by the same argument, but only
  `cpu.c:100` was MEASURED by the drop-and-run rig. `modrm.c:121` (the 337M-calls-per-benchmark
  fetch) is the one a reviewer should attack next, because it is the hottest guard in the tree.
* **The sweep's 47 upstream-matched groups are matched by TEXT**, not by a call-graph argument; the
  normaliser is conservative and its docstring says how, but a mismatch there would show up as a
  group wrongly marked "no upstream counterpart" — the 120 rows are there to be read.
* **The `make selftest` attribution is closed, and is recorded because it is evidence**: the gate
  was RED while the concurrent coder's new tests were mid-flight, the failures named THAT file and
  those lines, and they cleared on their own fix with no change to any file this unit touched (the
  final run of the same command is rc 0, all five suites green, `results/perf-fix-2-gates-selftest-final.txt`).
  What this unit did NOT prove is anything about their tests.

## 7. How to re-run

```sh
make                                                   # G4a
python3 results/perf-fix-2-verdicts.py                 # regenerate the verdicts TSV
python3 results/perf-fix-2-sweep.py --md results/perf-fix-2-sweep.md   # G2, rc 0
sh results/perf-fix-2-probe.sh                         # G1' the three dropped sites
sh results/perf-fix-2-cpu100.sh                        # G1  the cpu.c:100 verdict
make transcript-diff && make nasm                      # G4c/G4d
python3 tests/nasm_run.py --stage run \
    --actual "build/v86c-nasm {img} {limit} --fixture --uop-cache off" \
    --skip-list tests/nasm_skip_c.txt                  # G4c' cache OFF
make bench                                             # G4e
tests/perf_bench.sh --runs 1 --bin build/v86c --expect-state "… cr0=0xdeadbeef …"   # G3
```
