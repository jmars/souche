# PERF-7R — independent review of the performance landings (commit cc6f0c0)

**Unit:** `HANDOFF=v86c-audit`, PERF-7R (reviewer). **Subject:** PERF-7's three landings
(F1 checks-skip, F2 shift recipes, F4 TLB-mask fold) as committed in cc6f0c0, their gates,
and their measurements (`results/perf-7.md`, `results/perf-7/`). **Method:** pure
code-reading against the executor contract (`uop_cache.c`'s loop) and the C reference
bodies; raw-evidence inspection; no gate re-run (orchestrator-verified per brief).
**Memory:** `handoff-v86c-audit3-review`.

**VERDICT: NO BLOCKER.** Every clause of the F1 criterion was enumerated against the
emitted `l_checks` step by step and each one genuinely establishes that its step cannot
fire for the elided shape (§1). F2 is an exact transcription of `arith.c`'s semantics with
real differential arms (§2). F4's fold preserves the predicate exactly (§3). The rigs are
sound and the quoted ratios recompute from the raw logged runs (§5). Two SHOULD-FIX
documentation findings on the load-bearing comment, both cheap.

sha256 of the files reviewed: `src/cpu/jit_codegen.c` c4eee6ade0f4d01669097b061abf7592813ba4362ea911931ebd2dc3ce0fedd2 ·
`src/cpu/jit_codegen_selftest.c` 59ffa99601b9a8a3af6f0da0d5e6c524bb9b4f861b689e7089806ba416a0fc3a ·
`src/cpu/uop_cache.c` c3bf72bd813b3dadaf786d52fa8e4134194c9e8390b8b3907a0a5337d1e2e466 ·
`src/cpu/paging.c` 5953370dfcb5e6b7b4953870cc917ef1e3af4461809acae4ec0a5fa5508bf2f0 ·
`src/cpu/arith.c` 2ecc7ce171c439cb3bcebb82e572573db30ebb6eb567bad374aefcefb4ffeba9 ·
`src/cpu/instructions.c` 0d6900dcac7d09d5c29b77082f0d9bf4ba9db8d95692361dcaaa99b05149d8b3 ·
`results/perf-7-ab.sh` c682ce91b8adace93b7da88f982e85b890718d54a942460a5d2e6cfc219fcf30 ·
`results/perf-7-mut-rig.sh` f876ec447ca0d31663e51544c236413a563d4c11d6fd6259b423068d9ad4d210.

---

## 1. F1 — the criterion, clause by clause against the emitted `l_checks`

`l_checks` (`jitcg_emit_checks`, jit_codegen.c:1289-1374) runs, in the executor's order:
step 6 (`interpreted_unimplemented`, :1296-1298), step 7 (`ran && (FLUSH ||
jit_block_boundary)` → flush + clear-jbc + budget, :1300-1322), step 8 (page cross,
:1325-1331), step 9 (iteration limit, :1333-1345), step 10 (BLOCK_END, :1348-1351),
step 11 (divergence → TAIL/chain, :1353-1357). On the fall-through it does nothing but
`ret`. The criterion (`jitcg_uop_continues`, :1568-1580) must establish that none of these
can order a flush/exit for the elided shape. Step by step:

* **Step 6** — `interpreted_unimplemented` has ONE writer in the tree: cpu.c:68
  (`unimplemented_instruction`), called from generated interpreter bodies — all C bodies.
  A recipe's fast path runs no C code: the only intra-block calls in admitted recipes are
  `getcf`/`getzf` (:493, :726-740), which are EMITTED pure arithmetic over
  `flags_changed`/`last_result`/`flags` (:1216-1264) and cannot touch it. Any recipe
  failure routes to the slow arm, which re-enters the full checks (:1641-1652).
  **ESTABLISHED.**
* **Step 7, first disjunct (FLUSH)** — decode-time `u->flags`, excluded at :1570 before
  anything else. **ESTABLISHED.**
* **Step 7, second disjunct (`jit_block_boundary`)** — one writer, cpu.c:80
  (`after_block_boundary`), called from generated bodies AFTER the body returns
  (interpreter_gen.c:167-175 etc.). The subtle half — a *stale* jbc carried in from an
  earlier uop — is also impossible: the setter uop necessarily has `ran=true` (the call
  happens inside a body that ran; `ran=false` means the resolve faulted BEFORE the body,
  uop_cache.c:1092-1130), and a called uop's own step 7 — which called uops and recipe
  slow arms ALWAYS run (`cont` is false for `rec == NULL`; the slow arm re-enters at
  :1652) — flushes AND clears it (uop_cache.c:1490-1493). The remaining entry paths clear
  it too (uop_run:1255 at entry; the mid-batch fallback :1304-1311; cpu.c:141). So no
  admitted recipe can see it set. **ESTABLISHED — but the written proof is incomplete, see
  finding 1.**
* **Step 8 (page cross)** — for a recipe, `ip` stays at the step-3 store `body_ip`
  (:1152), and `body_ip == eip + len` for every non-custom-modrm uop (the dispatcher
  consumes opcode+modrm+sib+disp+imm before the body; uop_cache.c:1451-1467). The only
  custom-modrm encodings are 0x8D/0x8F: 0x8D is LEA — excluded by kind; **0x8F has no
  table entry** (checked the full table, :905-969 — no POP recipe), so it is always a
  called uop. The criterion's compile-time XOR (:1579) is therefore the same expression
  the runtime test evaluates. The exclusion is not vacuous: the decoder admits an
  instruction ENDING at its page's last byte (`u->len = dec_eip - inst_start`,
  uop_cache.c:606; DEC_PAGE_END drops only instructions whose bytes leave the page), and
  for those the checks run — pinned by battery arm (b) (`elided == inlined − 1`, 4090 of
  4091). **ESTABLISHED.**
* **Step 9 (iteration limit)** — second conjunct `u->eip >= ip` with `ip == eip+len` and
  `len >= 1` (every uop has ≥1 opcode byte) is compile-time false, so the flush it would
  order can never be ordered; skipping the test skips nothing. (The limit still bites on
  backward control transfers — JCC/JMP keep their checks.) **ESTABLISHED.**
* **Step 10 (BLOCK_END)** — decode-time flag, excluded at :1570. **ESTABLISHED.**
* **Step 11 (divergence)** — `rg JC_OFF(instruction_pointer)` over the emitters finds
  exactly three recipe writers: `rec_lea` (:599), `rec_jcc` (:749), `rec_jmp` (:758) —
  re-verified by me, matching the exclusion set by kind (:1571-1575). No other emitter
  touches `ip`. **ESTABLISHED.**

**The brief's hunt list:**

* *A fault raised by the recipe's own body* — fast paths cannot fault into C: every
  memory-touching recipe is `slow=true` in the table (all `_mem` entries; verified
  table-wide), and the slow arm re-enters the checks with the counts first, in the
  executor's count-then-check order (:1643-1652, matching uop_cache.c:1473→1490). The
  write-gate for code pages (:356-376) also routes to the slow arm.
* *`task_switch_test`* — lives inside C bodies (cpu_state.h:389's own note: reached from
  `modrm.c`/instruction bodies); no recipe fast path reaches it. FPU/MMX instructions have
  no recipes.
* *A block-boundary instruction* — BLOCK_END by flag (excluded); `jit_block_boundary` by
  the step-7 analysis above.
* *An iteration-limit edge* — step 9's analysis; `len ≥ 1` pins it.
* *A self-modifying-code abort* — the step-1 abort check (`cmp [R8], 0; jne abort`,
  :1597-1599) is emitted for EVERY uop including elided ones, before `cont` is even
  computed; `jit_abort_flag` is the JIT's abort line and it is not part of `l_checks` at
  all. Elision changes nothing here.
* *The reboot request flag* — `cpu->reboot_requested` is tested per-uop only in the
  interpreted loop (uop_cache.c:1480); the compiled block NEVER had a per-uop reboot test.
  Its contract is: `machine_reboot` raises `reboot_requested` AND `jit_abort_flag`
  (machine.h:86-92), the (unelided) step-1 abort check stops the block at the next uop
  boundary, and uop_run:1354 catches `reboot_requested` on ANY exit kind. No recipe —
  fast path or slow arm — can trigger a reboot (reboots originate in port handlers; no
  recipe body does port I/O; the mem bodies a slow arm can run are mov/alu/test/not).
  **No interaction with the elision.**
* *Counts and side-stores* — `prefixes = 0` on the elided fast path (:1631, the
  previously-measured bug), `INLINED += 1` only on the recipe path (:1635), `EXEC +=
  1`/`PENDING += 1` on both fast (:1669-1670) and slow (:1648-1649) arms with no double
  count (the slow arm bypasses `l_fast` via its own increments and `jmp l_done`), and an
  elided last uop falls into `jmp l_kind_tail` (:1809) exactly as the executor's
  `idx >= count` TAIL. The elided uop never materialises RDI/RSI/R10 — no downstream
  emitter or stub reads them before writing (the shared stub builds its own RDI, :1640).

**Criterion "table-driven" claim:** verified in the strong sense — the flag and page
clauses read decode-time uop fields, the kind clause reads the table entry's kind, and the
kind exclusion set is exactly the set of ip-writing emitters (re-verified by rg). The
exclusion set is maintained by hand, but a future ip-writing kind is caught IF it gets a
matrix case: rule (b)'s repeat-2 places a second copy after the instruction, so an elided
ip-writer falls through into the wrong instruction stream and `cmp_state` fails — mut2's
139 failures demonstrate exactly this mechanism.

## 2. F2 — the shift recipes, against the C reference

`rec_shift_r32_cl` (:784-825) vs `instr32_D3_4/5_reg` (instructions.c:1720-1722) and
`shl32`/`shr32` (arith.c:803-865):

* **Count masking**: body is `read_reg8(CL) & 31`; recipe computes `ECX & 31` (:792-793)
  and the native `shl/shr dst, cl` masks CL to 5 bits internally — same count, no
  materialisation. The **count-0 early-out** tests the MASKED count (`jz l_done`,
  :794) and skips write-back AND all flag stores — exactly `shl32`/`shr32`'s
  `if (count == 0) return dest_operand;` before any store. CL=0x20 therefore stores
  nothing — the classic silent-wrong, and the matrix's `z1` arm pins it
  (`flags_changed` must still be 0, jit_codegen_selftest.c:1735-1743).
* **CF**: native CF after the shift = the last bit out = arith.c's `b`
  (shl: bit 32−count of the original; shr: bit count−1). `setc` (:798). The
  `and r11d, 31` has cleared bits 8+ and the count (≤31) lives in the byte `setc`
  overwrites, so JC_TMP is exactly 0/1 at the `or` (:821) — the comment's claim checks
  out arithmetically.
* **OF**: shl `(b ^ sign(result)) << 11 & OF` (:800-807) = arith.c:814 exactly; shr
  `(original >> 20) & OF` (:808-813, original preserved in JC_VAL across the shift,
  :796) = arith.c:863 exactly. (v86's shr-OF is the original's sign bit unconditionally —
  the recipe matches the reference, which is the contract here.)
* **PF/ZF/SF** stay lazy: `last_result = result`, `last_op_size = 32`,
  `flags_changed = FLAGS_ALL & ~CF & ~OF`, `flags = (flags & ~CF & ~OF) | CF | OF`
  (:815-823) — the same stores arith.c:809-815 makes. The `l1` arm (shift then `jae`,
  :1757+) pins the lazy composition.
* **Register forms only**: the table carries only `instr32_D3_4/5_reg` (REGF0); the mem
  forms (uop_gen.c:533-534) are not recipes, so they keep the full C path with the
  invalidation hook — matching the report's stated scope.
* **The arms are differentials, not hand expectations**: `recipe_case` runs the program
  through all three arms (interpreter / uop-cache / compiled) and `cmp_state` compares
  flags, flags_changed, last_result, ip, counters and finally the whole cpu_state + RAM
  (jit_codegen_selftest.c:689-714) — a wrong recipe fails against the reference, not
  against itself; the pinned values are additionally hand-computed (0x1A345678≪5 =
  0x468ACF00, ≫5 = 0x00D1A2B3 — both correct). Engagement is asserted (blocks_compiled ≥ 1,
  inlined ≥ 1), so a dropped table entry cannot pass vacuously — and the two table pins
  keep the entries findable. mut4 (mask dropped → count test reads unmasked CL) fails 11
  checks (mut-mut4.txt), confirming the count arms have teeth.

## 3. F4 — the TLB-mask fold

The deleted code (git diff a49a887..cc6f0c0) was, per access: `cmp phys, memory_size;
jae slow` + `lea rdi, [phys − 0xA0000]; cmp rdi, 0x20000; jb slow` — exactly
`in_mapped_range(phys)`. The replacement requires the fill-time bit
`TLB_IN_MAPPED_RANGE` (paging.c:249, 259-261, set from `in_mapped_range(high)` where
`high` is the physical page base, DBG_ASSERTed page-aligned at :280) to be CLEAR on the
fast path (mask at :314-316, equality test :340-343).

* **Polarity**: bit set = page in the device/≥memory_size range → fast path declines →
  C helper (which re-tests `in_mapped_range` itself). Same answer, evaluated earlier.
* **Page-granularity is exact**: the straddle gate (:321-326) runs BEFORE the TLB test,
  so a fast-path access lies within one page; `0xA0000`/`0xC0000` are page multiples;
  `memory_size` is `mib << 20` (main.c:956) or the constants `BOOT_MEMORY_SIZE =
  0x4000000` / `MEMORY_SIZE = 0x200000` — all page-aligned — so `in_mapped_range` is
  constant across every page and `in_mapped_range(phys) == in_mapped_range(page_base)`
  = the bit.
* **No staleness axis**: no runtime writer of `memory_size` after boot (rg over src/;
  dma/ide only read it), and the VGA window is fixed hardware geometry. TLB entries die
  wholesale on flush; nothing can change the predicate under a live entry.
* **Cross-check**: v86's own JIT masks the mapped-range bit in its fast-path entry test
  (the report's citation) — same fold, independently invented upstream.
* The LFB (R1F) lives above `memory_size` → bit set → slow path, as before.

## 4. The gates' new arms — can they fail, and what they cover

* `test_continuation_elision` (jit_codegen_selftest.c:2174-2248): a genuine 3-arm
  differential with a compile-time counter instrument. Arm (a) pins the elision ENGAGED
  (`elided == 6`, `inlined == 6`, `called == 1`) — the vacuous-pass guard; arm (b) pins
  the page-edge exclusion (`elided == inlined − 1` and `> 0xFF0`). Dropping the page
  clause flips arm (b)'s counter deterministically (all 4091 would elide); mut1
  (criterion → false) fails the engage pins (3 failures, mut-mut1.txt) — the rig can see
  both directions.
* The mutation rig (`results/perf-7-mut-rig.sh`): control sees green, each arm mutates
  ONE clause by exact-string patch (asserted present), requires the suite to FAIL,
  restores on exit/trap. mut2 = 139 failures, mut4 = 11 — both caught (raw transcripts
  corroborate the counts).
* **The flag-clause instrument is an invariant pin, not a mutation** — the brief asked
  this be said plainly, and it should be: pin (d) (jit_codegen_selftest.c:2052-2070)
  cannot fail TODAY because no firing instance exists; it fires only when a future table
  entry's descriptor carries `UOP_DESC_FLUSH`/`UOP_DESC_BLOCK_END` (it checks the right
  descriptor fields — the `flags`/`iflags` mapping at uop_cache.c:600-603 matches). This
  is correctly disclosed (report §F1, rig header). Note the *guest's* safety never rested
  on the pin: the criterion's own `u->flags` test is evaluated per-uop at compile time,
  so a flagged recipe would correctly keep its checks — the pin protects the clause from
  being deleted as dead code, which is the weaker but honest role.
* The slow-arm re-entry (cont + `rec->slow` → counts + full checks) is exercised by every
  memory matrix case's first access (TLB miss → decline → C body → re-entry), with
  `pending`/`instruction_counter` in the compared state.

## 5. The measurements

`results/perf-7-ab.sh`: pins both arms to one core (`taskset -c "$PIN"`, PIN=8), interleaves
A/B inside each pair with order flipped between pairs, asserts EVERY run's identity
(instruction count, terminal state, serial sha; a mismatch exits 1 rather than averaging
the run), `rc` captured correctly, best AND median reported. The noise floor was measured
with the same binary in both arms (1.0036x/1.0043x). Every quoted ratio recomputes from
the raw logged runs: 0.994/1.418 = 0.7010 (ab-f1), 1.331/1.409 = 0.9446 (ab-f2),
1.362/1.413 = 0.9639 (ab-f4), 0.834/1.402 = 0.5949 (ab-stack); the F3 legs (1.0660x /
1.3451x median) match the report's 1.066x/1.35x. All 10 runs per block
identity-asserted (grep-counted). stack-stats.txt corroborates the staircase counters and
the elision engagement (6817 of 8330) quoted in the report. The F3 decision (keep unity)
is a measurement-backed refutation of the audit's premise on the post-F1 tree, and option
1 is the status quo — nothing landed, nothing to fix.

## Findings

1. **SHOULD-FIX (documentation on the highest-risk line).** The step-7 row of the
   criterion's proof comment (jit_codegen.c:1540-1543) states only "jit_block_boundary is
   set only by after_block_boundary ... No recipe writes it" — the *current-uop* half. The
   conjunct that actually closes the step is the **carry-in half**, which the audit's
   specification spelled out ("every called uop runs its own checks, which clear it",
   results/audit-3-perf.md §F1) and which the landed comment dropped, despite the audit's
   landing gate saying "keep the proof obligations above as the comment at the criterion".
   The invariant holds (verified independently, §1 above): the setter's body necessarily
   ran (`after_block_boundary` is called after the body returns), so its own step 7 —
   which called uops and recipe slow arms always execute — flushes and clears it, and the
   three other entry paths clear it too. But a future reader of the comment as written
   could believe "no recipe writes it" is the whole ground and admit, say, an inlined
   `sti`-class recipe whose fast path sets the flag — which would leak a stale
   `jit_block_boundary` into the next called uop's step 7 and shift the staircase. FIX:
   restore the audit's full clause in the step-7 row of the comment, e.g. "...
   furthermore it cannot be carried in: any setter ran (the call follows the body), and a
   called uop's or slow arm's own step 7 — which those always run — clears it before the
   next uop (uop_cache.c:1490-1493); uop_run's entry and the fallback paths clear the
   rest."
2. **SHOULD-FIX (one line, same comment).** results/perf-7.md:253-257 claims "A future
   recipe that calls into C on its FAST path must keep the checks ... — the comment at
   `jitcg_uop_continues` says so." The comment contains no such sentence (it states the
   invariant, not the obligation; read in full, :1528-1567). The obligation currently
   lives only in the report, which a future recipe author will not have open. FIX: add
   the obligation to the comment — "a future recipe whose FAST path calls into C, or
   writes `instruction_pointer` / `interpreted_unimplemented` / `jit_block_boundary`, must
   join an exclusion (or the matrix's arms must keep its checks)" — or soften the report's
   sentence. Prefer the former.
3. **NICE-TO-HAVE.** The flag-clause instrument (pin (d)) is an invariant pin — weaker
   than a mutation in the specific sense that it cannot fail on today's tree; its
   disclosure in the report and the rig header is accurate and the mut3 retirement is the
   right call (a mutation with no firing instance is a test that cannot fail). No change
   requested; recorded because the brief asked that the weaker instrument be named as
   what it is.

No findings on F2, F4, the rigs, or the F3 resolution.

## Coverage

Read in full (or in the relevant entirety): `src/cpu/jit_codegen.c` — the contract header,
all emitters touched by the three changes (addr/mem_prep/load/store, flags helpers, ALU,
test/not, lea, jcc, jmp, the new shift), the recipe table, `jitcg_emit_stores/spill/
reload/post/slow/getcf/getzf/flush/checks`, the F1 criterion and `jitcg_emit_uop`, the
compiler pre-pass and label layout, the epilogue count fold; `src/cpu/uop_cache.c` —
`uop_run`'s full loop (both fallbacks and the compiled-arm continuation), the decode flag
mapping and `u->len`/`body_ip` derivation, `uop_exec`'s `ran` semantics;
`src/cpu/jit_codegen_selftest.c` — `arm_run`/`cmp_state`, `test_tlb_fold_bits`, the
matrix's rules and all new shift/elision arms, `test_recipe_table`'s pin (d),
`test_continuation_elision`, `test_chain`; `src/cpu/paging.c` TLB fill and
`translate_address`; `cpu_state.h` flag/TLB bits; `cpu.c` writers; `arith.c` shl32/shr32
and siblings; `instructions.c:1720-1722`; `interpreter_gen.c` `after_block_boundary` call
sites (sampled); `machine.h`'s reboot contract; both rigs line by line;
`results/perf-7.md` in full; every file in `results/perf-7/` (identity-assert counts,
summary blocks, mutation failure lines, staircase counters); `results/audit-3-perf.md` §F1
(the specification); the git diff a49a887..cc6f0c0 for `jit_codegen.c`, `paging.c`,
`jit_codegen.h`, `results/perf-1/revert-attribution.sh`.

Not checked: the SECFIX-1 file set (concurrent reviewer's scope, per brief); the JS
reference and `v86/` (read-only, unnecessary — the C interpreter is the contract and the
recipes were checked against it directly); gates and A/B not re-run (orchestrator-verified
per brief; I verified the raw evidence is internally consistent and the quoted numbers
recompute from it). Working tree clean at review time; no file moved.
