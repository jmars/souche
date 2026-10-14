# JIT-FIX-1R — independent review of the blocker fix, the missing gate, and the should-fixes

**Unit:** `HANDOFF=v86c-jit` JIT-FIX-1R (reviewer), reviewing JIT-FIX-1's tree.

**Verdict: NO BLOCKER, NO SHOULD-FIX.** The fix is correct in both helpers, the write helper's
reachability claim survives an attempted refutation, the new gate is a genuine differential
with the 24-FAIL BEFORE evidence verified from the unit's own transcripts, and the unit's
refutation of one of the prior review's leak prescriptions is **right on the code**. Two
NICE-TO-HAVEs below; both are instrument-honesty items, neither touches correctness.

Reviewed sha256s (identical to the unit's published table): `jit_codegen.c` `5ce8386b…`,
`jit_codegen_selftest.c` `da8c90c1…`, `jit_mem.c` `98af5ce6…`, `jit_selftest.c` `34d93b7c…`,
`jit_x64.c` `229724b5…` (pinned, unedited), plus `uop_cache.c` `023e6123…`, `cpu_state.c`
`5b46e29f…`, `cpu_state.h` `05dbf8ce…`, `jit_mem.h` `d0e18aa5…`, `jit_x64.h` `8149a99c…`,
`jit_codegen.h` `0b9ee34e…`.

---

## Q1 — is the fix correct, in BOTH helpers? YES.

Verified line-by-line against the interpreter's own accessors (`cpu_state.c:19-31`:
`get_reg8_index(i) = (i << 2 & 12) | (i >> 2 & 1)` → register i's byte lives in `reg32[i & 3]`
at byte `i >> 2 & 1`; `write_reg8` truncates to `uint8_t`):

| arm | emitted | matches interpreter |
| --- | --- | --- |
| `jitcg_emit_read_reg8` low (`jit_codegen.c:207`) | `movzx dst, AL/CL/DL/BL` | zero-extends ✓ |
| `jitcg_emit_read_reg8` high (`:210-216`) | `mov dst,h; shr dst,8; and dst,0xFF` **imm32** | `(reg>>8)&0xFF` ✓ |
| `jitcg_emit_write_reg8` low (`:245-247`) | `mov AL/CL/DL/BL, src8` | truncates ✓ |
| `jitcg_emit_write_reg8` high (`:249-253`) | `and h,~0xFF00` **imm32**; `and tmp,0xFF` **imm32**; `shl tmp,8`; `or h,tmp` | clears byte, writes byte ✓ |

Two subtleties check out:

1. **The keep-mask `~0xFF00` is itself the same trap, avoided.** As an imm8, `0xFFFF00FF`
   sign-extends to `0xFFFFFFFF` — a no-op that would let the OR accumulate instead of
   replace. It is emitted `imm8=false` (`81 /4 id`) at `:248`. The comment above the helper
   names only the `0xFF` mask; the class grep below confirms both are imm32.
2. **The REX/H-register unaddressability is dodged structurally, not by care.** Byte sources
   are always materialised in `JC_VAL` (R10, `:137`) before a low-byte write — `mov al, r10b`
   requires `REX.R`, which `jx64_mov_r8_r8` emits (`jit_x64.c:571-577`), and that encoding is
   exercised by matrix cases `c5`/`h8`. A high guest byte is never passed to the encoder as a
   byte register.

**The class grep (the brief's repeated-bite shape).** All 7 `imm8=true` calls to
`jx64_alu_rm_imm` in the file:

- `:333` the TLB mem_prep mask — max value `TLB_VALID|TLB_NO_USER|TLB_READONLY` = 7, below
  0x80, safe.
- `:441`/`:477` the 83-group ALU recipes — sign extension **is** the x86 imm8 semantics there.
- `:658` `rec_test_rm8_imm8` (`u->imm & 0xFF`, true) — harmless because `JC_VAL` is
  zero-extended, so the sign-extended high bits cannot reach a bit the operand has. The unit
  names exactly this at `results/jit-fix-1.md` §5.3, and the reasoning is correct.
- `:1149` immediate 1, sign-extension irrelevant.

No other site carries the class.

## Q2 — is the write helper really now unreachable? THE CLAIM SURVIVES.

All three paths into `jitcg_emit_write_reg8` (`rec_mov_r8_rm8` reg+mem at `:543`/`:547`,
`rec_mov_rm8_r8` via `jitcg_emit_op_store` W8 at `:414`) pass a value produced by the **fixed
reader** (`movzx`, or `mov+shr+and-imm32` — both zero-extending) or by a `movzx` memory load.
No 8-bit ALU recipe writes a byte register (grep: W8 stores exist only for the two `mov`
recipes), so no full-width value can reach the helper. With the reader fixed, the writer's
mask has nothing left to mask; fixing it anyway (as a contract, `:219-241` documents the
four-state table) is the right call for the next recipe.

The four-state arithmetic reproduces by hand from the shipped values
(EAX=`0x1111C0DE`, EBX=`0x22223456`): reader-dead gives `JC_VAL = EBX>>8 = 0x00222234`;
writer-dead shifts it to `0x22223400` and ORs into `0x111100DE` → `0x333334DE` — the exact
BEFORE value in `probe2.BEFORE-bothdead.txt`. The unit's table is not an assertion; it is the
measurement, and it checks out.

## Q3 — is the new gate a genuine differential? YES.

`test_recipe_matrix`'s high-byte block (`jit_codegen_selftest.c:1519-1570`):

- **Three arms, interpreter as reference.** `recipe_case` runs interpreter / uop-cache /
  compiled (`arm_run`, `:643-660`) and compares the full `cpu_state_t` plus pending, vector,
  and 2 MiB RAM; the h-cases add explicit `CHECK_EQ`s **in all three arms** — a wrong
  hand-written constant would fail the interpreter arm, so a case cannot pass by agreeing
  with itself.
- **Both helpers, both recipe entries, all four registers.** h1-h4 read AH/BH/CH/DH
  (expected `0xC0/0x34/0xA5/0xAB`, hand-checked against `setup_matrix_highbyte`); h5-h8 and h9
  write through both the 88 and the 8A entry (`:543`/`:547`), exercising both code paths into
  the write helper.
- **The corruption lands.** All four hosts have non-zero high halves, and
  `0x11 | 0x22 == 0x33 != 0x11`. The `0x9ABCDEF0`/`0x12345678`-hides-it claim verified by
  hand: 0x12's bits are a subset of 0x9A's, so the OR leaves the destination unchanged.
- **`repeat = 1` with a stated reason** (the write cases compose on a second execution), and
  **h8 is labelled honestly** — the comment explains exactly why `mov ah, bl` cannot fail for
  this bug, and the BEFORE transcript confirms h8 is absent from the failure set.
- **ABLE TO FAIL, verified from the unit's own evidence:** `gates-G2-blocker.txt` = 24 FAIL
  lines, exactly the 8 non-h8 cases × 3 (2 `cmp_state` + 1 explicit); `read-only-fixed` = 0
  FAILs (the reader alone suffices — consistent with reachability); `write-only-fixed` = 12
  FAILs (h1-h4 only).

## Q4 — the should-fixes. DONE, and the leak refutation is correct.

- **(a) jcc pins/cases.** All 12 fn pointers pinned (`:1825-1842`); the 4 missing encodings
  have matrix cases (`j13-j16`). *Caveat in NICE-TO-HAVE-1 below.*
- **(b) the 11 byte pins** are real byte pins — each encoding hand-verified against `as`
  semantics (`66 45 8B 20`, `41 FF C3`, `FF 0F`, `F7 D0`, `80 A5 … 0F`, `40 80 E6 0F`,
  `F6 85 … 0F`, `40 F6 C6 0F`, `41 D3 E2`, `44 85 1F`, `66 8B 7D 04`). The forced-REX pair
  correctly selects SIL over DH (`rm=6`).
- **(c) the leak paths.** The three bind-path recycles (`jit_mem.c:346/355/397`) and the two
  publish-path recycles (`:259/249`) verified. The deliberately NOT-recycled path
  (page-carries-a-live-block, `:351-354`) is the ownership rule, and the refutation is
  **right**: that page is owned by a live block; recycling would free a page a block is
  executing from — worse than a leak, and reachable only by double-binding one code page. The
  gate (`jit_selftest.c:449-547`, run FIRST) is an exact-reuse assertion (`b==a, d==c, f==e`,
  `g==page0` after a real 2048-block fill + clear), not a count.
- **(d) the TLB fold.** `test_tlb_fold_bits` (`:1298-1353`) pins the decision through the
  entry bits **and** the `called>=2 / inlined>=1` counters — the who-reads-this concern is
  addressed (`tlb_data` is not in `cpu_state_t`, so the entry state is the only fingerprint).
  Mechanism verified against `paging.c:255/285` (a READ walk stores `TLB_READONLY`; the write
  must decline and re-store it clear). The cpl3 deferral's reason is **real, not assumed**:
  `paging.c:136` sets `allow_user = true` and only the PG-on arms clear it — with paging off,
  `TLB_NO_USER` is never set, so a nominal cpl3 case would pass with the bit deleted.

## Q5 — side findings

- The aarch64 stub for `jit_code_reserve_span` exists (`jit_mem.c:578`).
- The block-table-full latch stays a named NICE-TO-HAVE with honest calibration: the
  630,776 stderr lines and the per-decode reserve+publish+bind churn are real but
  guest-invisible.
- The false-pass double guard is good one level down; the residual gap is one level up
  (NICE-TO-HAVE-2).

---

## Findings

**NICE-TO-HAVE-1 — the §4.1 attribution is refuted by the unit's own transcript, and two swap
pairs remain invisible to the gate.** `results/jit-fix-1.md` §4.1 says the `jcc8_fns[6]/[7]`
swap is "a mutation the OLD matrix could not see" and "caught by the new cases alone".
`gates-G2-jcc-swapped.txt` shows otherwise: the **old** case j4 (`0x77` "ja (materialised,
CF=0)") also failed and contributed **8 of the 24** failures — with the swap, 77's entry
carries BE's recipe, so taken becomes not-taken, which the old matrix sees. What the old
matrix genuinely could not see was a *de-inline* (a pointer to an unsupported fn — invisible,
safe) — that is what the new pins catch. Separately: two swap pairs remain invisible to the
**whole** gate today, `B↔BE` and `AE↔A` — they produce identical behavior in both flag states
the matrix uses (CF=0,ZF=0 and CF=1,ZF=0), and the pins cannot see swaps at all
(`jit_recipe_name(u.fn) == r->name` compares the same lookup twice — a tautology by
construction, `jit_codegen.c:1044-1048` vs the pin at selftest `:1869`). Every adjacent
supported pair is distinguished, so the realistic mutations are caught; one added case in the
CF=0,ZF=1 state (`cmp eax,eax` then jcc) would distinguish all pairs. **Fix:** correct the two
sentences in §4.1; add the one case.

**NICE-TO-HAVE-2 — the gate script's own exit code does not aggregate its rows.**
`results/jit-fix-1-gates.sh` runs with `set -u` (no `-e`), never aggregates the `note` rc
values, and exits with the last command's status — so a "VARIANT MISSING (rc=2)" row (the
exact false-pass shape this unit fixed one level down) coexists with script exit 0. The rows
are loud in `gates-summary.txt` and this unit demonstrably reads them, but the headline claim
"`bash results/jit-fix-1-gates.sh` → exit 0" is not gated on them. **Fix:** track the worst rc
across `note` calls and `exit` it.

## Coverage statement

**Read in full:** the two helpers and their callers in `jit_codegen.c` (188-254, 380-420,
500-570, 650-668, 860-995, 1044-1048, 300-340); `jit_codegen_selftest.c`'s `arm_run`/`cmp_state`
(643-712), the setups (1373-1424), the high-byte block (1519-1570), the jcc block (1680-1740),
`test_recipe_table` (1785-1917), the 11 new byte pins (505-565), `test_tlb_fold_bits`
(1298-1353); `jit_selftest.c:449-547`; `jit_mem.c`'s bind/publish/recycle (226-400) and the
aarch64 stub (578); `cpu_state.c:19-31`; `paging.c:125-260`; `results/jit-fix-1-gates.sh` in
full; `results/jit-fix-1.md` and `results/jit-2r-review.md` in full. **Evidence read:** the G2
transcripts (FAIL counts and per-case failure sets), the probe/probe2 BEFORE transcripts
(corrupted values reproduced). **Not re-run** (orchestrator-verified per brief): make, nasm,
transcript gates, the boots, the benchmarks, the aarch64 build.

**Bottom line:** the blocker fix, the missing gate, and all four should-fixes hold up under
line-by-line review. The two findings are documentation/ harness items; nothing here blocks
the orchestrator's default-ON decision on correctness grounds.

---

## CORRECTION (JIT-FIX-2, MEASURED — `results/jit-fix-2.md` §1) — NICE-TO-HAVE-1(b)'s SCOPE

The claim in NICE-TO-HAVE-1 that the two swap pairs "remain invisible to the whole gate today" is
**overstated, and the measurement is in `results/jit-fix-2.md` §1**. What is true: the four swaps
(jb↔jbe and jae↔ja, rel8 and rel32) pass every *suite* — the recipe matrix (rc 0 at 2778 checks,
0 failures), `jit-selftest`, and the 118/118 nasm corpus — and pass the transcript too for three of
the four. What is not true: they are invisible to the gate. `tests/perf_bench.sh`'s pinned 200 M
`--jit on` leg separates all four from the fixed tree deterministically (2 runs each; the fixed tree
reproduces its pins `200095214` / `eip=0x21972c1`, while the four mutants terminate at 280,921 /
200,097,867 / 530,745 / 542,688 instructions with a different terminal state), and that row is
`note`d, so the script's exit code moves — rc=1 measured on a swapped baseline. The remaining
finding is the narrower and still-real one: the hole was in the MATRIX and the pins, which the added
CF=0,ZF=1 cases now close (8 cases, 2778→2978 checks, each swap failing exactly its own two cases).
Separately, the 1.5 G boot state also differs for all four mutants but its off-vs-on comparison was
printed and never gated; JIT-FIX-2 gated that row.
