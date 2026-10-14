# JIT-6F — the abort-geometry should-fix and two nice-to-haves

**Unit:** `HANDOFF=v86c-jit`, unit **JIT-6F** (coder). **Input:** `results/jit-6r-review.md`
(finding 1 is the brief), `results/jit-6-headroom.md` (§2.1's false claim), `/tmp/brief-v86c-jit-6f.txt`.
**Memory:** `handoff-v86c-jit-result`, `handoff-v86c-jit-review3`. **Raw evidence:** `results/jit-6f/`.

Claims are **MEASURED** (run this session, raw output in `results/jit-6f/`), **READ** (source, with
file:line) or **INTERPRETATION** (marked).

## 0. Bottom line

| item | verdict |
|---|---|
| **Finding 1 (should-fix): the false claim + dead code + the undocumented load-bearing invariant** | **FIXED** — took the reviewer's minimal/true option: corrected the comment and report §2.1 to state the real mechanism, DELETED the never-called geometry seat, and WROTE THE SAME-PAGE INVARIANT DOWN at the abort site |
| **nice-to-have 1: the rdtsc attribution instrument** | **FIXED** (+ a THIRD instrument defect found beyond the reviewer's finding 2) |
| **nice-to-have 2: `jit_mem_init`'s `jit_ready` early-true** | **FIXED** (+ the cosmetic sibling the reviewer named: `pages_alloc` ends at 1) |

Behaviour-neutral: `make` rc 0, nasm **118/118**, transcript-diff rc 0 (default and `--jit off`
byte-identical to the JS reference), `make selftest` all suites green with **every count
unchanged**, the determinism gate rc 0 at the pinned count **4,000,053,925**, the reboot gate
**39/39**, bench best **1.521 s** / median **1.574 s** (vs the pinned 1.571 s), aarch64 compiles
all four files rc 0.

## 1. Finding 1 — the fix

### 1.1 What was false (READ, confirmed line by line)

`jit_exec_block_geom` was gated on `jit_cur_block != JIT_PAGE_NONE`, but its only reader
(`uop_cache.c`, the ABORT continuation) runs **after** `jit_execute_block`, which calls
`jit_block_exit` first — and `jit_block_exit` clears `jit_cur_block` unconditionally
(`jit_mem.c:733`). So the true branch was **unreachable** and `jit_exec_phys`/`jit_exec_virt` were
**write-only**. The ABORT continuation has therefore ALWAYS run the `else` arm:
`phys = blk->phys_start + (ip - blk->virt_start)` with `blk` = the **ENTRY block** — not "the
substrate's last-entered block" the comment and report §2.1 claimed. Exactly the project's named
class (a seat that is never called is indistinguishable from an absent seat).

### 1.2 The changes (source)

- **`src/cpu/uop_cache.c`** (the abort site, the only reader): the false comment is replaced with
  the **real mechanism and the invariant written down as LOAD-BEARING**, and the dead read is
  removed — the fallback is now the literal two-line `phys = blk->phys_start + (ip - blk->virt_start)`
  with no `jit_exec_block_geom` call. The written invariant (verbatim in the source):

  > THE GEOMETRY IS THE ENTRY BLOCK'S (`blk` …), and that is EXACT — but it is LOAD-BEARING that it
  > stay that way … what makes `blk->phys_start + (ip - blk->virt_start)` the right phys for ANY ip a
  > run can reach is `jit_chain_target`'s SAME-PAGE GATE (jit_mem.c) … That same gate is ALSO what
  > keeps the uop side correct: `uop_cur_block` stays the entry block for the whole run and its page
  > test in `uop_invalidate_page` — and the `uop_stats.aborts` that goes with it — fire exactly when
  > `jit_abort_flag` does only because every link is on its page. WIDENING THE CHAIN ACROSS PAGES
  > (adding the walk arm `jit_chain_target` deliberately refuses) WOULD SILENTLY BREAK BOTH …

- **`src/cpu/jit_mem.c`**: DELETED `jit_exec_block_geom`, the `jit_exec_phys`/`jit_exec_virt`
  statics, both write sites (`jit_chain_target`, `jit_block_enter`) and the aarch64 stub. The
  `jit_cur_page`/`jit_cur_block` statics stay (they ARE read — see §2), with an updated comment
  stating the chain updates them to the TARGET.
- **`src/cpu/jit_mem.h`**: DELETED the `jit_exec_block_geom` declaration.
- **`results/jit-6-headroom.md` §2.1**: corrected IN PLACE and marked — the false sentence is
  quoted, marked FALSE, the real mechanism and its exactness argued, and the cross-page trap named
  (`[CORRECTED — JIT-6F …]`). §7's file list and §5's pointer updated to match.

`rg -n 'jit_exec_block_geom|jit_exec_phys|jit_exec_virt' src/` → **0 hits** (MEASURED).

### 1.3 Why the minimal/true option, not the wiring option

The reviewer offered two fixes. The wiring option (capture the geometry into a non-cleared record
before `jit_block_exit`) would make the fallback read the executing block — but it changes what
runs today to reach a state that is **identical today** (all links same-page) and would still need
the same-page invariant documented, because the uop side (`uop_cur_block`'s page test) is NOT
fixed by it. So it adds mechanism and a load-bearing record with no behavioural gain, and the
brief's own instruction is "do not leave dead code whose name promises a mechanism that does not
run". The minimal option removes the promise, states the invariant, and keeps the code equal to
what provably runs. **The invariant is now written where the next reader (who would widen the
chain) will actually hit it** — G2 satisfied.

## 2. G3 — if any part of the geometry record remains, what reads it?

`jit_cur_page` / `jit_cur_block` remain, and they have live readers (READ):

| variable | reader | what it decides |
|---|---|---|
| `jit_cur_page` | `jit_page_recycle` (jit_mem.c:713) | defer a recycle of the RUNNING page (`JP_DEFER`, flushed at `jit_block_exit`) |
| `jit_cur_page` | `jit_block_exit` (jit_mem.c:732-738) | flush a deferred recycle |
| `jit_cur_block` | `jit_code_invalidate_page` (jit_mem.c:658, 662) | set `jit_abort_flag` when a guest write hits the executing block's page — this is the SMC abort the compiled block's per-uop test reads |

The chain updates both per link (`jit_chain_target`, jit_mem.c), so a write to the page of the block
a chain moved execution INTO still aborts it and still does not recycle it under it. The dead seat
(the `jit_exec_*` geometry) had no reader and is gone — a seat that is never called must not remain.

## 3. Nice-to-have 1 — the rdtsc instrument

The reviewer's finding 2 (bracket excludes the prologue/epilogue; first segment includes boot time)
is correct. Fixing it surfaced **two more defects** in `results/jit-6/make-rdtsc-variant.py`:

1. **the first segment's boot inclusion** — fixed (`j6_seg_init` drops the first segment);
2. **`dispatch_raw = t_dispatch + j6_t_seg` grafted the host's TSC-since-boot onto the total**:
   `j6_t_seg` at print time is the ABSOLUTE rdtsc value at the start of the last in-flight segment,
   not a duration. **MEASURED on this host, the script as published before this fix:
   `t_dispatch_raw` = `949,061,756,707,190` cycles (≈9.49e14)** — the TSC-since-boot term. Fixed:
   the instrument now sums only COMPLETE segments (no trailing `+ t_seg`). **The published
   `1.080 G raw` total is therefore not reproducible from the script as published on a host whose
   TSC reads nonzero at process start** (the authored run evidently had a near-zero start TSC, and,
   note, was the pre-chain tree — 15.3 M brackets vs the chained tree's 244 K).
3. **the script did not build at all as published** (two independent reasons, both MEASURED):
   - it ignored `v86/`, but the build regenerates `src/cpu/interpreter_gen.c` FROM
     `v86/gen/x86_table.js` → `make: No rule to make target 'v86/gen/x86_table.js'`; `v86/` is now
     copied.
   - the rdtsc asm is unguarded, and **cosmocc builds the APE for x86-64 AND aarch64**, so the
     aarch64 pass fails with `impossible constraint in 'asm'`; the asm is now behind
     `#if defined(__x86_64__) || defined(_M_X64)` (`J6_TSC_LOHI`, a no-op elsewhere).

The corrected docstring and the printed `j6: NOTE …` line name the bracket boundary explicitly: the
"dispatch" segment is the C BETWEEN blocks (exit handling + loop top + hash probe) and **EXCLUDES
the prologue/epilogue — a LOWER BOUND on what chaining removes**; "interior 96.5 %" must not be read
as "dispatch is only 3.5 %". The boundary is also stated in report §1.2's correction and pointed to
from §5.

**Corrected measurement (MEASURED, chained tree, `results/jit-6/rdtsc-instrument-jit-6f.txt`):**
`t_uoprun` 5.098 G cycles, `t_dispatch_raw` **77.6 M** over **243,783** brackets → net
**64.5 M = 1.27 %**, interior **5.03 G = 98.7 %**. That is the residual C dispatch the chain did
NOT remove (it runs 244 K times, not 15.3 M) — a different tree from the published pre-chain
measurement, so they are not directly comparable. The run's own pins held: 200,095,214
instructions, terminal state `eip=0x21972c1 cs=0x10 …`, chain links 15,073,458.

## 4. Nice-to-have 2 — `jit_ready` early-true, and `pages_alloc`

- **`jit_mem_init`'s `if (jit_ready) return true;` → `return jit_probe_ok;`.** After a failed probe
  `jit_ready` stays true (it is set before the probe so the probe can use the pool's entry points),
  so a second call answered "true" for a JIT this process disabled. Latent (no live caller re-inits
  — `main.c` gates on `jit_enabled`), fixed as the one-liner the reviewer named.
- **the cosmetic sibling the reviewer named**: the probe reserved and recycled one page through
  `jit_page_alloc_run` (which COUNTS the allocation; `jit_page_recycle` does not uncount it), so a
  fresh run reported `pages alloc 1` for a pool holding nothing. `jit_stats.pages_alloc = 0` is now
  set after the probe — it is a per-run counter, and after a successful probe the run has taken
  none.

## 5. G1 — the gates (raw output in `results/jit-6f/`)

| gate | command | result |
|---|---|---|
| build | `make minibios-mem; make` | rc 0 |
| nasm | `make nasm` | rc 0, **118/118**, 0 of 123 produced no state, 5 skipped |
| transcript | `make transcript-diff` | rc 0, C transcript **byte-identical** to the JS reference |
| transcript (`--jit off`) | `./build/v86c tests/test.img 1000000` | rc 0, **byte-identical** to the JS reference |
| selftest | `make selftest` | rc 0, all suites green: cpu **3522** / m1c **228** / hw **551** / uop_cache **7934349** / uop_exec **102** / jit **68** / jit_codegen **3006** — **every count unchanged**, 0 failures |
| determinism | `sh tests/gate-m2-5-determinism.sh` | rc 0, **0 FAILING**, both runs **4,000,053,925** instructions and byte-identical transcripts |
| reboot | `sh tests/gate-reboot-1.sh` | rc 0, **39 passed, 0 failed** |
| bench | `make bench` (7 runs) | rc 0, best **1.521 s** / median **1.574 s** (131.56 MIPS best), every run verified 200,095,214 instructions + terminal state — within noise of the pinned 1.571 s |
| aarch64 | `aarch64-linux-cosmo-gcc … -Werror` on jit_mem / jit_x64 / jit_codegen / uop_cache | all rc 0 (the deleted aarch64 stub does not break the compile) |

Constraints honoured: the JIT's default, the invalidation hooks, the staircase and the same-page
gate itself untouched (documented, not widened); `v86/` untouched; no git operations.

## 6. Files and hashes

| file | sha256 |
|---|---|
| `src/cpu/uop_cache.c` | `c3bf72bd813b3dadaf786d52fa8e4134194c9e8390b8b3907a0a5337d1e2e466` |
| `src/cpu/jit_mem.c` | `04eb11179f4224b5bee1a96908aa1ac78b5c3b326ba2433aa391fd2983da1498` |
| `src/cpu/jit_mem.h` | `97fc0a42b92ff34cea1b9b822626fc9330253db146ef1d76154e727f1dba7f6c` |
| `results/jit-6-headroom.md` | `19c52aa97c505b762e63962071e669504555a8de23bdb9f6ef429ae2a63e46ef` |
| `results/jit-6/make-rdtsc-variant.py` | `9472877434ad83b01205344a848b810f8ec79ce0e0feb0f51912f8224c7d5069` |

**Not this unit's:** `results/jit-6/*.fixed` are the JIT-6 gates script's saved scratch copies and
still carry `jit_exec_block_geom`; they are evidence of that run, not the shipped tree.

## 7. What is NOT done / not covered

- The instrument's prologue/epilogue share is NOT isolated (bracketing it needs in-emitter rdtsc);
  the boundary is documented instead, per the reviewer's own "or say precisely what it measures".
- The `pages_alloc` fix is a one-line extension beyond the brief's second nice-to-have (the
  reviewer named it as the cosmetic sibling of the same finding).
- The published pre-chain `3.5 %` split is not re-measured on the pre-chain tree (the pre-chain
  source is not in the tree; `results/jit-6/v86c-base` is a binary and is not instrumented). The
  correction states what the fixed script measures on the current tree and why the two are not
  comparable.
