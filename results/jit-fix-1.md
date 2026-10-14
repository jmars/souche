# JIT-FIX-1 — the high-byte silent-wrong blocker, and the four should-fixes

**Unit:** `HANDOFF=v86c-jit` JIT-FIX-1 (coder). **Verdict: the blocker is fixed and PROVEN by
asymmetry, the probe is LANDED AS A PERMANENT GATE (`make selftest`), three of the review's four
should-fixes are done and the fourth is deferred with its reason measured rather than assumed.**
Every gate the brief names is green, and the JIT's default is untouched (still OFF).

Reviewed/edited sha256s (this unit's tree):

| file | sha256 |
| --- | --- |
| `src/cpu/jit_codegen.c` | `5ce8386b0e5b9ba2a2ee8551839855916e8add53fb2e1bd7df77784013cd2996` |
| `src/cpu/jit_codegen_selftest.c` | `da8c90c153bcbd9e954fe5b1b481da7d9226a06bdce4f0208b92c16a94ac1f63` |
| `src/cpu/jit_mem.c` | `98af5ce63d45854d4f94f5ab8a67d938aa6da572ebad80c470c3239610c532c7` |
| `src/cpu/jit_selftest.c` | `34d93b7cd95d14d5f000d5013f3f90a4f782a81eb1e14ac5ef0691d395474152` |
| `src/cpu/jit_x64.c` | `229724b516b04cd4658b14f18d18cafbf6e18dd2e93729575205fc0d62275bb5` (UNCHANGED — pinned, not edited) |
| `results/jit-fix-1-gates.sh` | `854ff209bd18a8180e9b45a10dd389926a53d2102780c693810271f6632e987b` |

Raw evidence: `results/jit-fix-1/` (`gates-summary.txt` is the index; the probes' images,
transcripts and every G2 arm's suite output are there). The BEFORE halves of the two probes are
kept alongside the after ones — `probe.BEFORE-jitoff.txt` / `probe.BEFORE-bothdead-jiton.txt`,
`probe2.BEFORE-*.txt`, `probe2.read-only-fixed.txt`, `probe2.write-only-fixed.txt` — so the
asymmetry can be re-read without rebuilding the pre-fix tree.
Unedited-original-side inputs: the orchestrator's `probe.asm`, plus this unit's `probe2.asm`.

---

## 1. THE BLOCKER: the high-byte READ mask was a no-op

`jitcg_emit_read_reg8`'s high-byte arm emitted `83 /4 ib` (`jx64_alu_rm_imm(…, 0xFF, true)`), and
the imm8 form SIGN-EXTENDS: `0xFF` becomes `0xFFFFFFFF`, so the `and` that was supposed to clear
the bits the `shr` left in 8..31 did nothing, and `movzx r32, ah/ch/dh/bh` produced a 24-bit
value. **Fix: the imm32 form** (`81 /4 id`, `imm8 == false`, one token) — the same form the
sibling line at `jit_codegen.c:223` already used, no new encoder surface, and the encoder's
`81 /4 id` pin already existed.

**Why this form and not a shorter one.** A zero-extending move after the shift (`movzx dst, dst8`,
3-4 bytes) would save 2-3 bytes per high-byte read, at the cost of depending on dst==src encoding
support for a form nothing else uses; high-byte reads are ~20 sites in the corpus's whole dynamic path, so
the saving is noise. The imm32 `and` is the minimal correct change and matches the file's idiom.

### The asymmetry (G1), MEASURED with the orchestrator's own recipe

`/tmp/probe/probe.asm` → `as --32` + `ld -m elf_i386 -T tests/test.ld` + `objcopy -O binary`
(the M0 harness's recipe; the script does it from `results/jit-fix-1/probe.asm`):

| guest code | `--jit off` | `--jit on` BEFORE | `--jit on` AFTER |
| --- | --- | --- | --- |
| `mov eax,0x9ABCDEF0` ; `movzx esi, ah` | `0x000000de` | **`0x009abcde`** | `0x000000de` |
| `mov ecx,0x55663488` ; `movzx edi, ch` | `0x00000034` | **`0x00556634`** | `0x00000034` |

`probe.diff.txt` (after the fix) is empty: `--jit off` and `--jit on` transcripts are
byte-identical INCLUDING the RAM sha256, so the write-path cases in that probe (which use LOW-byte
sources) are identical too.

---

## 2. THE SECOND SITE (`jitcg_emit_write_reg8`) — the review and the first probe disagreed, and BOTH were partly right

The review called both sites wrong. The first probe measured the write cases CORRECT. The
resolution is a four-state measurement, run as four arms of the gate script (`gates-G2-*`), on
probe2.asm's `mov ah, bh` with EAX = `0x1111C0DE` and EBX = `0x22223456` (values chosen so the
corruption LANDS — see §3):

| reader (read helper) | writer (write helper) | `mov ah, bh` → EAX | suite |
| --- | --- | --- | --- |
| dead | dead | `0x333334DE` **WRONG** | 24 failures (the shipped tree) |
| fixed | dead | `0x111134DE` | 0 failures |
| dead | fixed | `0x111134DE` | 12 failures (the READS are still broken) |
| fixed | fixed | `0x111134DE` | 0 failures |

Read off that table, which is what the gates actually print:

* **In the shipped tree the write site IS live**: `mov ah, bh` (high-byte source → high-byte
  destination) really did come out `0x333334DE`. The review's *claim* was right.
* **The first probe's observation was a property of its CASES, not of the write site**: its
  write cases use a LOW-byte source (`mov ah, bl`), and the reader's low-byte arm was the one arm
  that was never broken. `mov ah, bl` is correct pre-fix; `mov ah, bh` is not. That distinction
  was the whole question the brief asked ("is the write site reachable with a source whose bits
  8..31 are non-zero?"), and the answer is YES — *because the reader hands it its unmasked 24-bit
  value*.
* **Independently of the reader it is unreachable**: every caller passes `JC_VAL`, and every
  `JC_VAL` producer zero-extends (`movzx r32, r8`; `movzx r32, m8`; the fixed high-byte read), so
  with the reader fixed the write mask has nothing left to mask. Fixed anyway (one token): the
  comment above it is a CONTRACT ("only the low byte of `src` is used"), and the next recipe to
  pass a full register as a byte source would break it silently. `jit_codegen.c:219-241` carries
  the four-state table.

---

## 3. THE PERMANENT GATE (the thing the brief says matters most)

**Where:** `src/cpu/jit_codegen_selftest.c`, `test_recipe_matrix`'s new high-byte block (9 cases
h1..h9) + a new setup function. **It runs in `make selftest`**, the project's own gate, alongside
every other recipe case.

The existing corpus passed with the bug present (118/118 nasm, byte-identical M0 transcript, the
1.5 G boot), so the gate's job is to make the missing coverage permanent. Two properties make it
able to fail:

1. **A setup whose four 32-bit hosts have NON-ZERO high halves** (`setup_matrix_highbyte`:
   EAX = `0x1111C0DE`, EBX = `0x22223456`, ECX = `0x4444A5B6`, EDX = `0x8888AB12`). The existing
   `setup_matrix` has ZERO high halves, which is why all four of its pre-existing high-byte cases
   (`mov al,ah`, `mov ah,al`, `mov ah,cl`) passed with the dead mask.
2. **Values chosen so the leak LANDS.** The first probe of this unit used EAX = `0x9ABCDEF0`,
   EBX = `0x12345678` and `mov ah, bh` came out CORRECT pre-fix: `0x12`'s bits are a SUBSET of
   `0x9A`'s, so the leaked bits were already set in the destination and the OR changed nothing.
   `0x11 | 0x22 == 0x33 != 0x11` cannot cancel. This is recorded in-tree because it is the
   difference between a gate and a gate-shaped no-op.
3. **`repeat = 1`, and the expected value asserted EXPLICITLY in all three arms.** The matrix's
   `repeat = 2` exists for TLB-warm-up on MEMORY recipes; these cases WRITE their destination, so
   a second execution COMPOSES (`movzx eax, ah` twice ends at EAX = 0 — a value a broken JIT could
   also produce). One execution is the value the encoding means, and the interpreter's own arm is
   asserted alongside the compiled one, so a case cannot pass by agreeing with itself.

**ABLE TO FAIL (G2, MEASURED):** with both emitter tokens reverted to the shipped `true`, this
suite reports **`2778 checks, 24 failures`** — 8 of the 9 cases (h1..h7, h9), 2 `cmp_state`
failures + 1 explicit-value failure per case. The restored tree: `2778 checks, 0 failures`.

`h8` (`mov ah, bl`) is kept although it CANNOT fail for this bug: it is the low-byte-source case,
i.e. exactly the shape that made the write direction look correct, and it pins the encoding.

---

## 4. THE FOUR SHOULD-FIXES

### 4.1 Four of the twelve jcc recipe entries had no case and no pin — DONE

All twelve encodings now have both. The matrix gains `76` (jbe rel8, materialised AND lazy), `0F 83`
(jae rel32), `0F 85` (jne rel32), `0F 87` (ja rel32, materialised AND lazy); the table test gains the
seven missing fn pointers (`72 76 77 0F83 0F84 0F85 0F87`), so all 12 slots of
`jitcg_jcc8_fns`/`jitcg_jcc32_fns` are pinned. The two arrays are INDEXED BY CONDITION NIBBLE, so a
misplaced pointer silently gives another condition's recipe — a wrong-answer bug, not a de-inline.

**Able to fail:** swapping `jitcg_jcc8_fns[6]` (BE) and `[7]` (A) is caught: 24 failures,
`jbe rel8 (materialised, CF=0 ZF=0): cpu_state differs first at offset 68: 0x03 vs 0x05` and the
lazy twin; the restored tree is green. **CORRECTED (JIT-FIX-1R NICE-TO-HAVE-1): the claim that this
was "a mutation the OLD matrix could not see, caught by the new cases alone" is REFUTED by this
unit's own transcript — `results/jit-fix-1/gates-G2-jcc-swapped.txt` shows the OLD case j4 (`0x77`,
ja) contributed 8 of the 24 failures, so the old matrix DID see this swap. What remains true is
narrower and is still an open hole: two swap pairs (jb↔jbe and jae↔ja) are invisible to the whole
gate in its current state, because no case runs in the CF=0,ZF=1 flag state. One added case in that
state (`cmp eax,eax` then the jcc) distinguishes them.**

**CORRECTED AGAIN (JIT-FIX-2, MEASURED — `results/jit-fix-2.md` §1): the sentence above is right
about the MATRIX and wrong about the whole gate.** The four swaps (jb↔jbe and jae↔ja in the rel8
AND the rel32 array) do pass every suite — `jit-codegen-selftest` rc 0 at 2778 checks / 0 failures,
`jit-selftest` rc 0, `make nasm` 118/118 — but they are **not** invisible to the gate:
`tests/perf_bench.sh`'s pinned 200 M `--jit on` leg distinguishes all four deterministically
(2 runs each: the fixed tree reproduces the pins `200095214` / `eip=0x21972c1`, `jcc8-swap-bbe`
halts at 280,921 instructions, `jcc8-swap-aea` ends at `eip=0x219a760`, the two `jcc32` swaps halt
at 530,745 / 542,688), and that row IS `note`d — so the script's exit code moves (rc=1, measured on
a swapped baseline). The hole was in the recipe matrix, the twelve fn pins and the nasm corpus, all
of which the CF=0,ZF=1 cases now close (8 cases, +200 checks, with per-pair attribution); the 1.5 G
boot state also differs for all four trees but its comparison was printed and never gated — JIT-FIX-2
made that row gate.

### 4.2 Eight encoders neither called nor byte-pinned — DONE (pinned, not deleted)

`jx64_mov_r16_rm16`, `jx64_inc_rm32`, `jx64_dec_rm32`, `jx64_not_rm32`, `jx64_alu_rm8_imm8`,
`jx64_test_rm8_imm8`, `jx64_shift_rm32_cl`, `jx64_test_rm32_r32` now have **11 byte pins** whose
ground truth is the host assembler again (`as` + `objdump`, transcribed). Two of them are the
FORCED-REX cases (an 8-bit REGISTER operand always carries a REX so that rm=6 means SIL and not
BH) — the encoder's bytes are `40 80 E6 0F` / `40 F6 C6 0F`, which `as` produces for `and sil,0Fh`
/ `test sil,0Fh`. Pinned rather than deleted because the header is the module's API.

**Able to fail:** changing `jx64_not_rm32`'s digit `2 → 3` — a function NO call site reaches — is
caught by its new pin and by nothing else: `FAIL byte pin: not r/m32 (eax)`.

### 4.3 `jit_cache_bind`'s failure paths leaked the reserved span — DONE, with one path deliberately NOT fixed

The leak is real and it is not theoretical: the 1.5 G boot with `--jit on` fills the 2048-block
table and prints `jit: bind: the block table is full (2048)` **630,776 times**, once per decode
attempt that reaches bind. Each of those was leaking a published-and-unnamed span; the pool (4096
pages) is drained after ~4096 of them, after which the JIT is silently dead for the rest of the
run.

**The review's prescription ("recycle the span before returning", all four paths) is wrong on
contact with the code for ONE of them**, and the fix is the ownership rule instead:

| path | span state | action |
| --- | --- | --- |
| block table full | published (RX), no block | **recycle** |
| span does not match the reserved length | published (RX), no block | **recycle** |
| physical page outside guest memory | published (RX), rolled back | **recycle** |
| **code page already carries a live block** | **owned by that block** | **NOT recycled** — recycling would free a page a live block is executing from, which is worse than a leak. (Reachable only by binding one code page twice; no caller does.) |
| `publish`: `buf` outside the pool / not writable | the caller still holds a reservation it may still write and publish | not touched |
| `publish`: length does not match / `mprotect(RX)` failed | RW and unowned | **recycle** (extension: same class of leak, one function over) |

**Able to fail (G2):** `results/jit-fix-1/jit_mem.c.leaky` is the same tree with the five recycle
calls deleted. `make jit-selftest` → **`45 checks, 4 failures`** (`b == a`, `d == c`, `f == e`,
`g == page0`); the fixed tree → `51 checks, 0 failures`.

**The gate (`jit_selftest.c`'s `test_failed_bind_recycle`, run FIRST in `main`) is an exact
reuse assertion, not a count.** The pool is all free when it starts, so the first free run of N
pages is page 0: after a refused bind the very next reservation must return the SAME run. It
covers all three refusals, including filling the block table with 2048 real blocks, and it ends by
asserting the pool is back to its initial state (`g == page0`) after `jit_code_clear_all`, so the
tests after it keep the environment they had. Pre-existing observable unchanged: `test_refusals`
still reports `pool exhausted after 4092 pages`.

*(While writing this gate I leaked exactly one page myself — the reclaimed span in case (c) was
reserved and never taken — and the SAME pool bookkeeping caught it: 4091 instead of 4092. It is
recorded in the test's comment because the instrument is that sensitive and it cost one bisect.)*

### 4.4 No test for the cpl3 / TLB_READONLY folded mask bits — **HALF DONE, the other half DEFERRED with its reason measured**

**TLB_READONLY: pinned.** `test_tlb_fold_bits` reads a page (the walk stores the entry with
`TLB_READONLY`), then writes it (the compiled write's mask must REJECT that entry, so the C body
walks and re-stores the bit CLEAR), then reads again (that one must INLINE). `tlb_data` is not part
of `cpu_state_t`, so the full-state differential cannot see any of this — the walk's rewrite of the
exported entry is the only fingerprint of the decision, and it is asserted directly, together with
the decisions themselves (`called_uops >= 2`, `inlined_uops >= 1`).

**Able to fail:** deleting the write mask's `TLB_READONLY` term leaves the entry at `0x00000003`
(READONLY still set) and the gate fails: `entry & (TLB_VALID | TLB_READONLY) == TLB_VALID (got
0x3, want 0x1)`. Correct tree: entry `0x00000001`, `inlined 1, called 3`.

**cpl3 (TLB_NO_USER): DEFERRED, and the reason is a measurement.** With paging OFF — the only mode
this harness has, and the mode every JIT memory case runs in — `allow_user` is unconditionally true
(`paging.c:136`; only the PG-on arm can clear it), so **TLB_NO_USER is never set in ANY entry** and
BOTH the C helper's test and the compiled mask are insensitive to the bit. A nominal `cpl == 3`
case here would pass with the bit deleted — a test that cannot fail, which this project counts as
worse than no test. Pinning it needs a PG-on fixture (a page directory + page tables, one
supervisor page, CR3, CR0.PG) plus a cpl3 guest: new fixture work in a suite that has no paging
anywhere. **Named as the one thing this unit leaves undone.**

---

## 5. SIDE FINDINGS (not in the review, found while doing this)

1. **`jit_mem.c`'s aarch64 stub set was missing `jit_code_reserve_span`**, which the header
   declares unconditionally. No caller existed until the new leak gate, so the first caller failed
   to LINK on aarch64 (cosmocc links both halves of the fat binary) while the x86-64 half built
   fine. One-line stub added; the fix is the sort of thing only a new CALLER can find.
2. **Once the block table is full the compiler keeps retrying**, 630,776 times on the 1.5 G boot,
   each with a `stderr` line. Pre-fix the leak self-limited this (the pool died after ~4096
   attempts). Neither behaviour is guest-visible, but the message volume and the per-attempt
   `reserve + publish(mprotect) + bind` work are wasted. A one-flag latch ("stop compiling while
   the table is full", or rate-limit the message) is a NICE-TO-HAVE for whoever owns M3 — it is
   NOT in this unit's scope and the guest state is identical either way.
3. **`jit_codegen.c:633` (`rec_test_rm8_imm8`) uses the same `imm8=true` form** for its byte mask
   and is HARMLESS for the same reason the write site is: its operand (`JC_VAL`) is already
   zero-extended, so the sign-extended `0xFFFFFF80` cannot reach a bit the operand does not have.
   Left alone deliberately, and named here so the next reader does not have to re-derive it.
4. **This unit's own gate harness had a false pass, and it is worth recording.** The first version
   of `results/jit-fix-1-gates.sh` scored a MISSING variant file (a `cp` that failed, so the suite
   ran the FIXED tree) as `rc=0` next to "want rc!=0", for four arms. Two causes: a `SyntaxError`
   in the variant-generating python (a missing paren), and no check that the arm's input existed
   and differed. Fixed twice: the generator asserts the three variants are DISTINCT files, and
   `g2_arm` refuses to score an arm whose variant is missing or identical to the fixed tree. Found
   by counting FAIL lines per arm rather than reading the rc column.

---

## 6. GATES (G1–G4) — all green

Full log: `results/jit-fix-1/gates-summary.txt`. Script: `results/jit-fix-1-gates.sh` (≈5 min).

**G1 the probe** — `probe` and `probe2`: `--jit on` transcript == `--jit off` transcript,
byte-identical including the RAM sha256, with the expected register values in `probe2.jitoff.txt`
(`eax 0x111134de`, `ecx 0x4444abb6`, `esi 0x000000de`, `edi 0x00000034`). Before the fix the same
two probes differ on six registers (§1, §2 tables).

**G2 every new gate shown able to fail** (rc=0 rows are the harness's own verdicts):

| arm | variant | suite result |
| --- | --- | --- |
| blocker (both sites shipped) | 2 tokens → `true` | `2778 checks, 24 failures` |
| read-only-fixed | reader fixed, writer dead | `2778 checks, 0 failures` (measured: the reader alone suffices) |
| write-only-fixed | writer fixed, reader dead | `2778 checks, 12 failures` |
| leaky | 5 recycle calls removed | `45 checks, 4 failures` |
| jcc-swapped | `jcc8_fns[6]`/`[7]` swapped | `24 failures` |
| encoder-mutated | `jx64_not_rm32` digit 2→3 | `1 failure` (its new byte pin) |
| no-readonly-fold | write mask's TLB_READONLY deleted | `1 failure` (the new TLB gate) |
| restored (control) | — | `2778 checks, 0 failures`, `jit-selftest 51/0` |

Every arm is restored from a saved copy and the restore is verified with `cmp`.

**G3 the existing gates**
* `make` rc 0; `build/v86c` sha256 `2ee29c846da219dc88b5e5a05a52cf72301fab3b07cc7385d3cdaadc5d3bf703`.
* `make selftest` rc 0 — **all suites green**: `cpu_selftest 3522/0`, `cpu_m1c_selftest 228/0`,
  `hw_selftest 551/0`, `uop_cache_selftest 7934349/0`, `uop_exec_selftest 102/0`,
  `jit-selftest 51/0`, `jit_codegen_selftest 2778/0`.
  **Counts of the pre-existing suites are UNCHANGED** (3522 / 228 / 7934349 / 102 — identical to
  the numbers in `handoff-v86c-jit-ctx`). The two JIT suites grew only by this unit's cases:
  `jit_codegen_selftest` **2302 → 2778**, of which **+252** is the blocker gate (MEASURED: build
  the tree with the high-byte block removed → `2526 checks`) and **+224** the four should-fixes
  (MEASURED: 2778 − the blocker-gate-stage 2554); the 2302 figure is those two measured deltas
  subtracted from 2778. `jit-selftest` **29 → 51** (+22, MEASURED by building the tree with the
  new test disabled). `hw_selftest` 549 → 551 is **NOT this unit**: a peer's RTC
  unit was landing in the same tree (§7).
* `make nasm` **118/118** in all three legs: default, `--uop-cache off`, `--jit on`
  (`[+] Passed 118/118 tests.`; the 5 skips are `tests/nasm_skip_c.txt`'s known port gaps, unchanged).
* `make transcript-diff` rc 0, and `--jit off` / `--jit on` transcripts both byte-identical to the
  JS reference at 1 M instructions.
* the 1.5 G boot: final state IDENTICAL between default and `--jit on`
  (`in_hlt=0, exception=-1, unimplemented=0, eip=0xc1927800 …`) and the serial output sha256 equal
  (`7044a6f6b3f1d3a734e91186e1acac1c4634200ece3eb69069befdd1e99e1aee`) — on the leak-fixed tree,
  i.e. with the pool still usable at the end of the run.
* aarch64: `jit_codegen.c`, `jit_codegen_selftest.c`, `jit_mem.c`, `jit_x64.c`, `uop_cache.c`,
  `jit_selftest.c` all compile clean under `-Werror` (and `make selftest` links the fat binary,
  which is what caught §5.1).

**G4 the benchmark** (`tests/perf_bench.sh --runs 7`, 200,095,214-instruction boot, host load 3.5):

```
jit off  best 2.713  median 2.870 s
jit on   best 1.571  median 1.679 s
ratio (off/on, >1 means the JIT is faster): best 1.727  median 1.710
```

So **~1.72x**, against the orchestrator's 1.80x on a quieter host (their `--jit off` legs were
2.891-2.928 s; mine are 2.713-2.870 s, i.e. this host is faster in absolute terms and the
ratio is load-sensitive). The fix does not touch the compiled code's shape beyond +3 bytes on
high-byte WRITE recipes, and the 200 M boot never fills the block table, so the leak fix has no
effect on this number either way.

---

## 7. WHAT IS NOT DONE, NAMED

* **The cpl3 half of should-fix 4** (§4.4) — needs a PG-on + cpl3 fixture that does not exist in
  the JIT suite. Deliberately not faked.
* **The block-table-full latch / message rate limit** (§5.2) — NICE-TO-HAVE, guest-invisible,
  belongs to whoever owns the next JIT unit.
* `JX64_MAX_FIXUPS` (512 vs a ~640 worst-case RMW budget) and the stale ONE-BLOCK-PER-CODE-PAGE
  comment in `jit_mem.h:108-110` are the review's own NICE-TO-HAVEs and are untouched here.
* **The JIT's default is still OFF**, as instructed. The decision to default it ON remains the
  orchestrator's.
* **A concurrent peer was editing `src/hw/rtc.c` in this same tree** while these gates ran (a
  `make selftest` leg failed mid-flight on their in-progress file: `rtc.c:304: implicit
  declaration of host_now_ms` under `-Werror`; the final green run is after they finished). The
  attribution matters: `hw_selftest`'s 549 → 551 and the RTC check changes are THEIRS, and
  `hw_selftest` does not link `jit_codegen.c` (`CPU_SRC`), so nothing in this unit can reach it.
  A note was broadcast on the `fx-agents` lobby. My write scope was `src/cpu/jit_*.{c,h}` +
  `jit_*_selftest.c` only; the peer's file was never touched by me.

---

## 8. HOW TO REPRODUCE

```bash
bash results/jit-fix-1-gates.sh          # G1 probes, all G2 asymmetry arms, G3 gates, G4 bench
make selftest                            # the permanent gate, in the project's own gate
make jit-codegen-selftest                # 2778 checks, 0 failures
make jit-selftest                        # 51 checks, 0 failures
```

The two probe images are built by the script from `results/jit-fix-1/probe.asm` (the
orchestrator's) and `probe2.asm` (this unit's), with the M0 image recipe.
