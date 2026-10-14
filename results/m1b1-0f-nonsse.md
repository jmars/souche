# M1b-1 — the 0F table minus the MMX/SSE groups

**Unit:** `HANDOFF=v86c`, unit M1b-1 (coder). **Deliverable:** `src/cpu/instructions_0f.{c,h}`
grown from M1a-4's three-slice stub to the whole non-MMX/SSE 0F table, plus a representative
slice of unit cases in `src/cpu/cpu_selftest.c`.

Every claim below is marked **MEASURED** (a command was run on this host and its output is in
`results/m1b1-gates.txt`), **READ** (from the reference source) or **INFERRED**.

---

## 1. Bottom line

**MEASURED.** 359 of the 390 symbols `results/m1b-scope-non-sse.txt` names are now DEFINED; the
emitter's census moved from **1083 unimplemented call sites** (M1a-4's recorded figure) to
**564**, i.e. exactly 359 fewer — the same 359. The C leg of the nasm differential gate rose
from **21/21 passed, 102 skipped** (the documented baseline) to **60/60 passed, 0 failed,
63 skipped**, and **13** of the 39 newly-passing tests are attributable to this unit by name
(§7). `make`, `make transcript-diff`, `make selftest` and `make nasm` are all exit 0.

The 31 remaining symbols are **not** a silent shortfall: each one's Rust body needs a helper
that no C file defines yet, and each keeps the emitter's stop that **names its symbol** (§3).

---

## 2. Files

| file | what |
| --- | --- |
| `src/cpu/instructions_0f.c` | **1871 lines**, 377 `instr*` definitions (359 added here; the 18 M1a-4 slices kept, moved to the bottom under their original comments) |
| `src/cpu/instructions_0f.h` | **838 lines**; the prototypes are exactly the 377 definitions (MEASURED: no declared-but-undefined and no defined-but-undeclared symbol), plus the cross-unit x87 block below |
| `src/cpu/cpu_selftest.c` | +209 checks in one new function `test_instr_0f_nonsse()`; the `run_instruction0f_16/32` recording stubs were replaced by one `run_instruction0f` stub (see §6) |
| `Makefile` | **one added line**: `src/cpu/instructions_0f.c` in `SELFTEST_SRC`. Read immediately before the edit; nothing else touched |
| `tests/nasm_skip_c.txt` | regenerated from a `--skip-list /dev/null` run (63 entries) — see §7 |

`v86/` was not modified (READ-ONLY). No commit.

---

## 3. The scope actually implemented, and the 31 that are not

**MEASURED**: `comm -12` of the scope file against the regenerated dispatcher's
`unimplemented_instruction(...)` names leaves exactly these 31:

| opcodes | symbols | why not |
| --- | --- | --- |
| `0F 00 /2..5` (lldt, ltr, verr, verw) | 16 | the bodies are `load_ldt`/`load_tr`/`verr`/`verw`, defined over cpu.rs's segment-descriptor API (`lookup_segment_selector`, cpu.rs:2640-2740). `misc_instr.h:20-27` **already** records lar/lsl/verr/verw as M1c's (exceptions.c); freezing that interface inside `instructions_0f.c` would invent M1c's contract and would collide with M1c's own definitions later |
| `0F 02/03` (lar, lsl) | 8 | same API |
| `0F 30/31/32` (wrmsr, rdtsc, rdmsr) | 3 | `read_tsc`/`set_tsc` (cpu.rs:4357-4380); `cpu_state.h:381` **already** defers them to M1c's run loop |
| `0F 34/35` (sysenter, sysexit) | 2 | `update_cs_size`/`cpl_changed` (cpu.rs:3000-3006), same deferral |
| `0F C7 /6` (rdrand) | 2 | **CORRECTED BY M2-2 (HANDOFF=v86c): see the note below the table.** The port does implement it now — the REGISTER form is upstream's rdrand (instructions_0f.rs:3996-4013) and the MEMORY form is upstream's `trigger_ud()` (instructions_0f.rs:4016-4018); the tables are initialised for 0..255 (pci.js:37-38 equivalent: gen/x86_table.js:462's `mem_ud: 1` is on the g=6 entry whose REG form is rdrand). `js::get_rand_int`'s C peer is /dev/urandom, the same kind of source the reference's JS uses |

> **CORRECTION (M2-2, HANDOFF=v86c), and it is a table row above, not a footnote: this unit
> got `0F C7 /6` BACKWARDS.** The row recorded rdrand as a two-symbol gap with the reason "a
> non-deterministic value cannot be reconciled with the transcript gate", and the code
> implemented BOTH forms as `trigger_ud()` -- i.e. the register form, which upstream defines
> as rdrand, was #UD. That was not a tidiness issue: an i386 Linux kernel's `random_init_early`
> executes `rdrand ecx` when CPUID advertises RDRAND (this port's CPUID does), so the guest
> took an invalid-opcode oops in the first seconds of its boot and never reached a console.
> MEASURED in results/m2-2-boot.md: with the old body the whole boot gate's serial output is
> ONE line (the firmware's own); with the fix both machines reach
> `Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)`.
> The same wrong expectation was asserted in src/cpu/m1c_selftest.c and is corrected there
> (222 -> 228 checks), and the corpus has no rdrand test in either skip list, so no gate moved.
Each of these still stops the machine **by name** (the emitter's `unimplemented_instruction`),
so the gap is observable and per-test accounted for in the skip list — it is not a silent
fallthrough and not a wrong value.

`0F AE /0` and `/1` (fxsave/fxrstor) are **implemented** through the cross-unit contract (§4).

Two scope-split notes, both **MEASURED** against the two scope files:

* `instr_660F7C_{mem,reg}`, `instr_660F7D_{mem,reg}`, `instr_F20F7C_{mem,reg}`,
  `instr_F20F7D_{mem,reg}` are **SSE3** instructions (haddpd/haddps/hsubpd/hsubps,
  instructions_0f.rs:3018-3093). They are in *this* unit's scope file and in **neither** part of
  the MMX/SSE file, so they would have fallen between the two units; they are ported here. They
  are plain `float`/`double` arithmetic over `reg128` — no softfloat and no `sse_instr.rs` — so
  the assignment is harmless. **Reported as an assignment, not as a preference.**
* `0F AE /2,/3` (ldmxcsr/stmxcsr) live in **instructions_0f.rs**, not in sse_instr.rs, and their
  whole body is the MXCSR word, so they are ported here rather than deferred.

---

## 4. The cross-unit x87 contract — satisfied and MEASURED

`instructions_0f.h` declares, with `fpu.c`/M1b-3 named as the definer:

```c
void fpu_set_tag_word(uint16_t tag_word);
void fxsave(int32_t addr);
void fxrstor(int32_t addr);
```

This unit **calls** `fxsave`/`fxrstor` (`0F AE /0,/1`) and **does not define or stub** them:
a silent stub of x87 state save/restore would let a guest that runs fxsave/fxrstor keep the
wrong FPU state instead of stopping, which is exactly the class of wrong this port refuses.

**MEASURED, and the tree moved during the unit:** M1b-3 landed `src/cpu/fpu.c` while this unit
was in flight — `fpu.c:1146` `void fxsave(int32_t addr)`, `fpu.c:1179`
`void fxrstor(int32_t addr)`, the declared signatures, and it added `softfloat.c`/`fpu.c` to the
Makefile's `SELFTEST_SRC`. Consequences, both handled:

1. the APE links against the **real** functions (from-scratch `make` exit 0);
2. the recording stubs this unit had drafted in `cpu_selftest.c` became **duplicate symbols**
   and were **removed**. The `0F AE` case therefore drives the **real** FXSAVE image (it checks
   the FCW at +0 and the MXCSR at +24, round-trips through `fxrstor`, and checks that both
   forms fault on an absent page). It is an integration test, not a mock — which is the stronger
   evidence for the contract, and it is stated here because it differs from what the brief
   anticipated ("a CONCURRENT UNIT OWNS fpu.c and DEFINES those three").

`fpu_set_tag_word` is declared per the brief but **not called from this file**: its only caller
is `emms` (`0F 77`), which is in the MMX/SSE scope (M1b-2), not in
`results/m1b-scope-non-sse.txt`. Reported rather than left for a reader to notice.

---

## 5. Divergences from upstream — each with its reason

1. **`undefined_instruction` / `unimplemented_sse` drop their `dbg_assert!(false)`**
   (instructions_0f.rs:3-10). `dbg_assert!` is `debug_assert!` (`dbg.rs:63`), i.e. compiled out
   of the pinned release reference, and its condition is a property of the **guest's bytes**.
   Aborting on them would be a divergence from the reference, not one of the port's stops — the
   same ruling the emitter records for the prefix fallthrough (`interpreter_gen.c`'s header).
   The C body is the `#UD` alone. (`misc_instr.c`'s `DBG_ASSERT(false)` for `bound` is a
   *different* case: that instruction has no other effect at all.)
2. **`dbg_log!` is dropped**, its text kept as a comment — the port has no log channel
   (`instructions.c`, `misc_instr.c` do the same).
3. **`.unwrap()` on a proved-fault-free result becomes `DBG_ASSERT(!cpu->pagefault)`**
   (sgdt/sidt `safe_write16/32`, cmpxchg8b `safe_read64s`/`safe_write64`) — `misc_instr.c:14`'s
   rule.
4. **`SEX8`/`SEX16` macros** for Rust's `x << 24 >> 24` idiom: the shift is done on the unsigned
   pattern and cast back, because `x << 16` into the sign bit is UB on a C `int32_t` (the M1a-2
   D5 rule). Two users here: movsx, and the bt group's 16-bit memory forms whose bit offset is
   `read_reg16(r) << 16 >> 16`.
5. **`1 << 31` (CPUID leaf 1's hypervisor bit) is written `(int32_t)0x80000000u`** — `1 << 31`
   on a C `int` is UB; the result is bit-identical.
6. **`config::VMWARE_HYPERVISOR_PORT`, `cpu.rs:296 TSC_RATE`, `cpu.rs:298 cpuid_level`** are
   file-local constants here: the port has no `config.h`, and copying `true`/`1e6`/`0x16` is
   what upstream's build does. `set_cpuid_level` (cpu.rs:4761, the JS hook that lowers the level
   for Windows NT) is **not** ported — there is no JS half to call it; `cpuid_level` is a
   `static const`. `floor()` is kept verbatim from cpu.rs:1198-1199 rather than folded away.
7. **`instr_660F7C/7D` / `instr_F20F7C/7D`** are ported here although they are SSE3 — see §3.

Nothing else diverges: the arm/condition tables, the masking (`& 15` | `& 31`), the argument
order of every `safe_read*`/`read_reg*`, the read-modify-write forms, the CPUID leaves and the
`0F AE` sub-group's `/4../7` bodies are 1:1 with instructions_0f.rs.

---

## 6. Divergences from the BRIEF, and things the brief got wrong

Reported loudly, per the brief's instruction, rather than coded around.

1. **"0F 05 (syscall)" is wrong for this reference.** READ: instructions_0f.rs:415 is
   `pub unsafe fn instr_0F05() { undefined_instruction(); }` — the pinned reference has **no
   `syscall`**. It is ported as `#UD`, which is what the reference does. `sysenter`/`sysexit`
   are `0F 34/35` and are **not** ported (§3).
2. **"0F 02/03 (lar/lsl)" and `0F 00 /2../5` are not implementable in this unit.** The brief lists
   them as deliverable, but their bodies are entirely `lookup_segment_selector` +
   `SegmentDescriptor` (cpu.rs:381-446, 2640-2740), which the plan (ss7 step 17) and
   `misc_instr.h:20-27` assign to M1c. Porting a private copy here would either duplicate or
   pre-empt M1c's contract. 24 of the 31 unimplemented symbols are these. **If the orchestrator
   wants them inside M1b, that is a re-scope (a `lar`/`lsl`/`verr`/`verw` helper block in
   `misc_instr.c` plus M1c's `lookup_segment_selector`), not a fix inside this file.**
3. **The brief's x87 justification names `0F 77 emms -> fpu_set_tag_word`.** `0F 77` is in the
   MMX/SSE scope file, not in this unit's. Only `fxsave`/`fxrstor` are called from this file
   (§4).
4. **The scope file is DERIVED and its MMX/SSE boundary leaks** (§3, the four hadd/hsub pairs).
5. **`tests/nasm_skip_c.txt` was being rewritten concurrently** (§7).
6. **`src/cpu/cpu_selftest.c` is a SHARED file and both units wrote it.** **MEASURED:** while
   this unit was adding `test_instr_0f_nonsse`, M1b-3 added `test_fpu_words_and_stack` /
   `test_fpu_env_save_restore` / `test_fpu_softfloat_roundtrip` / `test_fpu_rounding_and_status`
   to the same file; the check count passed through 2968 → 3170 → 3349. **No clobber was
   observed** (the final file contains both units' functions and is green), and the reason is
   worth naming: every edit here was a read-modify-write of the current file, and the one
   collision window that did open was detected — a scratch-tree snapshot taken mid-write of
   `fpu.c` showed four FPU failures that the repo did not have, which is what a torn read of a
   file being rewritten by a peer looks like. It is a hazard, not a method.
7. **The Makefile was edited by both units** (`SELFTEST_SRC`, and M1b-3's `SF_*` block). Every
   edit here re-read the file immediately before and added one line.

---

## 7. Gates

Raw output and exit codes: **`results/m1b1-gates.txt`**.

| gate | command | exit | result |
| --- | --- | --- | --- |
| G1 | `make nasm` | **0** | **Passed 60/60 tests, 0 failed, 63 skipped**, "0 of 123 produced no state" |
| G2 | `make selftest` | **0** | **3349 checks, 0 failures** (209 of them this unit's) |
| G3 | `sh /tmp/m1b1-mut/rig.sh` | — | 4/4 mutations fail-then-pass (§8) |
| G4a | `make BUILD=/tmp/m1b1-final` (from an empty dir) | **0** | 15 TUs + the vendor softfloat object + the link, all clean under `-Wall -Wextra -Werror` |
| G4b | `make transcript-diff` | **0** | **empty diff** — the M0 transcript is still byte-identical to the reference |
| — | `node gen/interpreter_c.js` | **0** | 1922 call sites, **1358 implemented, 564 unimplemented**, 1362 names defined |

### G1's DELTA, and its attribution

Documented baseline (M1a-4): **21 passed / 0 failed / 102 skipped**.
Now: **60 passed / 0 failed / 63 skipped** — **+39 passed, −39 skipped**.

The +39 is attributable by NAME, from M1a-4's own 102-entry skip list (every entry carried the
symbol the binary printed), not by inference:

* **13 are this unit's.** Each one's baseline skip reason is a symbol defined in this file:

  | test | its baseline skip reason |
  | --- | --- |
  | bts-imm | `instructions_0f::instr16_0FBA_5_mem` |
  | btc-imm | `instructions_0f::instr16_0FBA_7_mem` |
  | btr-imm | `instructions_0f::instr16_0FBA_6_mem` |
  | bts-reg16 / bts-reg32 | `instr16_0FAB_mem` / `instr32_0FAB_mem` |
  | btr-reg16 / btr-reg32 | `instr16_0FB3_mem` / `instr32_0FB3_mem` |
  | btc-reg16 / btc-reg32 | `instr16_0FBB_mem` / `instr32_0FBB_mem` |
  | cmp16-setbe | `instructions_0f::instr_0F96_reg` (setcc) |
  | cmpxchg / cmpxchg-setbe | `instructions_0f::instr32_0FB1_reg` |
  | cmpxchg8b | `instructions_0f::instr32_0FC7_1_mem` |

* **26 are the FPU (D8..DF) tests**, attributable to **M1b-3's** `fpu.c`/`softfloat.c`, which
  landed concurrently. They are why the raw pass count moved further than this unit's 13.
* 21 + 13 + 26 = **60** exactly. No test regressed and none was added.

The remaining **63** skips are **61 "could not run"** (60 MMX/SSE stops — `instr_0F6F_mem` alone
still blocks a large group — plus the `jump` emulator-model assert) **+ 2 recorded class-B
divergences** (`pop_esp`, `pop_esp2`: the native fixture faults on a host-MMU store to address
0, which v86's flat model maps to RAM; the reference has the same gap). Zero D8..DF reasons
remain in the list.

### A note on `tests/nasm_skip_c.txt` (concurrent writer)

**MEASURED.** The brief predicted this file would be rewritten concurrently, and it was: at
12:36 it held **76** entries written by M1b-3, of which **15 were stale** (they named symbols
that now have definitions, so those tests actually run and pass, and they were being skipped by
name rather than on evidence). Regenerated here from a `--skip-list /dev/null` run:

```
passed 60 / skipped 63 / failed 0        (after regeneration)
passed 47 / skipped 76 / failed 0        (M1b-3's stale list, for comparison)
```

15 entries left that list; 13 are this unit's, and `pop_esp`/`pop_esp2` remain as class-B. The
orchestrator regenerates it once at the end, so a later writer is expected and fine.

---

## 8. G3 — asymmetry (the tests can FAIL)

Rig: `/tmp/m1b1-mut/rig.sh` (raw output in `results/m1b1-gates.txt`). It runs in a **scratch
copy** of the tree so the concurrent unit's build is never perturbed, and it verifies at the end
that `src/cpu/instructions_0f.c` was restored **byte-identical**.

| arm | the one-line mutation | BEFORE | AFTER (restored) |
| --- | --- | --- | --- |
| M1 | `0F 96`: `setcc_reg(test_be(), …)` → `test_b()` | `make nasm` **rc=2**, Passed **59/60**, `cmpxchg-setbe` mismatches | rc=0, 60/60 |
| M2 | `0F B3`'s 16-bit memory form: `btr_mem(addr, SEX16(read_reg16(r)))` → `read_reg16(r)` | `make selftest` **rc=2**, 4 failures (CF and the byte at `0x3000−4096` both change) | rc=0, 3349/0 |
| M3 | `cmovne`'s memory form: the page-fault guard → `0 && cpu->pagefault` | `make selftest` **rc=2**, 1 failure (the destination is written on a fault) | rc=0, 3349/0 |
| M4 | `haddps`: the destination pair and the source pair swapped | `make selftest` **rc=2**, 2 failures | rc=0, 3349/0 |

**Two arms were rewritten because the first targets were invisible, and the reasons differ —
one was a weak test, one was a wrong target:**

* The first M2 mutated `instr16_0FB3_mem` and was **not seen**, because the test at that point
  exercised the sign-extending bit offset only through `0F AB` (bts). The **test was
  strengthened** (bt/btr/btc on `0F A3/B3/BB` now all exercise it, observed through CF *and* the
  byte it writes), and the arm above is the re-run.
* The first M3 mutated `instr32_0F44_mem` and was **not seen** because that arm's condition is
  *false* in the test, so the missing guard had no effect. The test was already adequate — the
  arm was re-targeted at `0F 45` (`cmovne`), whose condition is true. **Stated rather than
  quietly re-run.**

A mutation the test cannot see proves nothing; all four arms above can be seen.

---

## 9. What the unit did NOT prove

* **The 31 unimplemented encodings are untested by construction** — there is nothing to test.
  Their gap is *named* (the emitter prints the symbol) and *counted* (per test, in the skip
  list), which is the honest form: a stop, not a plausible wrong value.
* **The nasm corpus does not exercise cpuid, the CR/DR moves, sgdt/sidt/lgdt/lidt/smsw/lmsw,
  invlpg, lss, the `0F AE` group, or the hadd/hsub pairs.** Those rest on the unit cases in §2
  plus the reference's own source being read line by line. The selftest cases assert values
  computed independently (page-table mappings, FXSAVE offsets, the pairwise-sum order), not
  values re-read from the implementation — but they are still this unit's own reading of the
  reference, one step weaker than the corpus.
* **`INFEASIBLE`/INFERRED, not measured:** that no *other* host could reach these encodings in
  the corpus.

---

## 10. How to re-run

```sh
make                                     # the APE (G4a)
make transcript-diff                     # G4b: must print an EMPTY diff
make selftest                            # G2
make nasm                                # G1 + the skip-list regeneration below
python3 tests/nasm_run.py --stage run \
    --actual "build/v86c-nasm {img} {limit} --fixture" --skip-list /dev/null   # the raw list
sh /tmp/m1b1-mut/rig.sh                  # G3 (scratch-tree copy; see the script's header)
```
