# R2 — independent review: string.c / instructions.c / the F1 carve-out

**Unit:** `HANDOFF=v86c`, unit R2 (reviewer). **Authority:** memory node `handoff-v86c-review2`
(this file is the human-readable copy). **Scope:** the M1a-3 files (`src/cpu/string.{c,h}`,
`src/cpu/instructions.{c,h}`, and M1a-3's changes to `paging.h/paging.c`, `memory.h/memory.c`,
`misc_instr.h/misc_instr.c`) and R1F's F1 carve-out, read against the upstream Rust/JS.
Pure code-reading pass; gates were NOT re-run (the orchestrator owns authoritative
verification). `v86/` read-only. Read-only review — nothing under `src/` was edited.

**File hashes reviewed** (currency check):
```
70f7f84193add5c6d92294f452d8dc652f8cc3c3871f0a8a8c189ee3f731fc44  src/cpu/string.c
61c0065becbaaa11de0cd03ef8fb6b5b217b4687745fe393a821a741f8c79633  src/cpu/string.h
b465119307b10a5be62061042ce5d93c31a564d56613e63ab15b857e82b86288  src/cpu/instructions.c
efed6fdce0fb779bde95145c41384787f1eba25b7d03c4fd3878789332568a9f  src/cpu/instructions.h
3d4a22e2a2b8ed69c1eb397fd03f08e0a52929176c448513153c29d4ef084b1a  src/cpu/memory.c
88c6178b5a7f613b6b50e6dd263bda4a1d220e0e5a1f9ee5c51621682d8e4614  src/cpu/memory.h
08e27bb7c884cb08e1c7c1de824eb4248af386707696217cf753819db4cc0a97  src/cpu/paging.h
949942c6b762556b33f6b192e966526575a3f4ab2c691c76c7d79426d4215349  src/cpu/paging.c
61551ca6924a91170719f3ecd9d9e58065bbb92900b4c4439f717396bc6b1af3  src/cpu/misc_instr.c
851ee0ab98913cbc004ab9a05b97b0f3fd7003f7b697789b586192dd1b7545d0  src/cpu/misc_instr.h
```

---

## Verdict

**No blockers, no should-fix findings.** The ported surface is faithful to the cited
upstream code, and the F1 carve-out is the right predicate with the right boundaries —
R1F's deviation from the R1 prescription was correct, and the probe genuinely pins the
carve-out (not merely the F1 value). One NICE-TO-HAVE doc fix (N1 below).

---

## Findings

### N1 (NICE-TO-HAVE) — instructions.c:26 header comment misstates the D8-DF count

`src/cpu/instructions.c:26` says the deferred group is
"instructions.rs:1643-2003, **176 functions**". MEASURED: that upstream range defines
exactly **160** `pub unsafe fn`s, and the unit report's own §5 list has 160 names. The
arithmetic reconciles only with 160: 988 total upstream functions − 160 deferred + 3
C-only helpers (`run_prefix_instruction`, `segment_prefix_op` from cpu.rs; upstream's
non-`pub` `instr_pushf_popf_check`) = 831 ported. Fix: change `176` to `160` in the
comment. The report's §5 title (160) is already correct.

*(No other findings. The notes below are observations, not defects.)*

---

## Coverage

### A. STRING — `string.c` read in full against `string.rs:25-701`, site-by-site

- **Iteration limit** (`count_until_end_of_page`, string.c:52-60 vs string.rs:25-32):
  formula identical, including the `direction == -1` `(addr & 0xFFF)/size + 1` term.
  No off-by-one. The `DBG_ASSERT(count_until_page_end > 0)` is reachable-only-with-true:
  the page-boundary check for movs (string.c:225-226) and the alignment requirement for
  the other ops each force the quotient ≥ 1, exactly as upstream.
- **Fast/slow split and the rewind** (string.c:155-349 vs string.rs:146-386): the
  fast-path gates (movs-or-aligned, `is_asize_32`, rep), the translate-once-then-commit
  structure, the overlap-interference check (`phys_src < phys_dst` ⇒ `direction == 1`,
  `>` ⇒ `-1`), `movs_reenter_fast_path` set **before** the page-boundary check (so a
  slow-path movs re-enters the fast path after one iteration), the backward-movs
  physical adjustment inside the loop, and `count == i` rep-cmp termination — all
  identical. **The rewind surface the brief asked about**: `instruction_pointer =
  previous_ip` is the ONLY thing rewound; `count -= i` and the `src/dst += i*increment`
  register commits happen at the function tail (string.c:341-349, 457-468), so a
  re-entered instruction continues from the committed state — exactly upstream
  (string.rs:378-386, 493-513). A fault in the **slow** path breaks out with the
  completed iterations committed and NO rewind — the dispatcher's fault delivery owns
  the eip reset (plan ss8/R3), matching upstream.
- **DF, ASIZE mask, segment overrides** (string.c:91-141): `direction` from
  `FLAG_DIRECTION`; `read_reg32(ECX/ESI/EDI) & asize_mask` on entry; `(dst + increment)
  & asize_mask` on the slow-path updates (fast path unmasked — rep_fast requires
  `is_asize_32`, so the mask is −1 there, as upstream); `get_seg(ES)` / `get_seg(ds_or_prefix)`
  with the fault check immediately after each; `test_privileges_for_io` before any
  memory access for ins/outs — all identical, including the 16-bit wraparound.
- **Partial-store surface**: the fast path's memcpy/memset is page-bounded and
  overlap-guarded before any write; mid-copy faults are impossible (RAM-to-RAM), and the
  slow path can only fault between iterations — no partial store upstream would not
  have made.
- **The 48 wrappers** (string.c:471-531): names, signatures, and the size/rep wiring
  identical to string.rs:516-701.

### B. INSTRUCTIONS — `instructions.c` vs `instructions.rs`

Read function-by-function: 00-3F (ALU/mov/segment), 40-6F (inc/dec, push/pop,
pusha/popa, bound/arpl, prefixes, imm-push, imul, ins/outs), 70-7F (jcc), 80-8F
(immediate groups, test/xchg/mov/lea/mov-sreg/pop-mem), 90-9F (nop/xchg, cbw/cwd, far
call, fwait, pushf/popf, sahf/lahf), A0-AF (moffs, strings), B0-BF (mov imm), C0-CF
(shifts, ret/retf, les/lds, mov-imm, enter/leave, int/int3/into, iret), D0-D7
(shift-by-1/by-CL, aam/aad, salc, xlat), E0-EF (loop, in/out, call/jmp rel, jmpf),
F0-FF (prefixes, hlt, cmc, F6/F7, clc/stc, cli/sti, cld/std, FE/FF groups).

Every multi-line/stateful function matches, including the traps the brief named:
- **RMW instructions**: the `SAFE_READ_WRITE8/16/32` macros (paging.h:143-203) are the
  faithful port of `safe_read_write*` (cpu.rs:4052/4073/4101) — the 32-bit boundary's
  `translate((addr+3)&~3) | ((addr+3)&3)` reproduces Rust's `addr + 3 & !3` /
  `(addr as u32) + 3 & 3` precedence exactly; the 16-bit boundary uses
  `translate(addr+1)`; the 32-bit form correctly has **no** dbg_assert (upstream has
  none there). **The no-write-on-fault property holds**: `return_on_pagefault!`
  (paging.rs:5-9) returns from `safe_read_write*`, which is the whole body of every
  caller upstream; the C macro's bare `return` is equivalent because — MEASURED by
  script over all 151 call sites — every first argument is the plain parameter `addr`
  (no double/triple evaluation in the boundary branches) and the macro is the **last
  statement of every caller**.
- **Segment-register moves** (8C/8E): `seg >= 6` ⇒ #ud before any access; `8C_mem`
  stores 16 bits in both operand sizes (upstream quirk kept); `8E` accepts exactly
  {ES,SS,DS,FS,GS} and faults abort before `switch_seg`.
- **Far call/jmp/ret, iret** (9A/EA, FF/3, FF/5, CA/CB, CF): the
  `DBG_ASSERT((new_ip & 0xFFFF0000) == 0)` on the non-protected 32-bit forms, the
  32-bit retf `cs & 0xFFFF`, the FF/3-FF/5 `addr+2`/`addr+4` far-pointer reads, and the
  fault check between the two reads — all match.
- **Stack-pointer adjustments**: `8F/0` mem-pop's adjust-before-resolve + reset-on-fault
  (instructions.rs:937-971 verbatim); C2/C3's adjust after the pop with the fault return
  in between; C9 leave's `safe_read32s(get_seg_ss() + old_vbp)` before `set_stack_reg`.
- **Flags-touching family**: 9C/9D (the 0xFCFFFF pushf image; 16-bit popf keeping the
  upper 16 via the 0xFFFF0000 mask; `old_eflags` captured before the update; the IF
  0→1 handle_irqs trigger), 9E/9F, F5 cmc, F8/F9, FC/FD, the jcc/loop dispatch — match.
  (setcc/cmovcc live in the 0F table, M1b's, per the name partition.)
- **Miscellany**: D6 salc's `-(int32_t)getcf()` == `-(getcf() as i32)`; D7 xlat's
  16-bit `(BX + AL) & 0xFFFF`; F4 hlt's cpl check and the IF-gated timer/irq path; FA/FB
  cli/sti including the dead `false &&` VME arm transcribed with its dead condition
  (D13) and sti's shadow-dispatch order (prefixes=0 → previous_ip=ip → counter+=1 →
  read_imm8 → run → handle_irqs; a faulting read_imm8 skips handle_irqs in both);
  F6/F7's `SEX8/SEX16` for Rust's `x << 24 >> 24` / `<< 16 >> 16` and the
  `(uint32_t)x` div casts; FF/2-FF/4 helpers; the `_mem` hoisted-read forms (02/03,
  0A/0B, 1A/1B, 22/23, 2A/2B, 32/33, 38-3B, 69/6B, 80-83/7, 84/85, F6/F7, A0-A3, D7):
  the read-then-check ordering is equivalent to upstream's argument order because
  register reads have no side effects and the fault aborts before the register write
  in both.
- **The D8-DF deferral** (160 functions): not a gap in this unit's scope — the name
  list in the report §5 is complete (MEASURED: 160 `pub fn`s in
  instructions.rs:1643-2003, 160 names listed; 988 − 160 + 3 = 831 reconciles the
  ported count). The emitter's `run_instruction` contract (low byte = opcode, bit 8 =
  operand size) matches `cpu.rs:3131` and the generator's case split.
- **The one-liner bulk** (637 functions) was NOT re-read individually; it is covered by
  the implementer's token-level bulkcheck (493 single-statement functions compared,
  0 unexplained mismatches) plus block-level spot checks during this pass.

### C. THE F1 CARVE-OUT — `memory.c:119-135` vs the reference

`device_default_is_vga_blob(addr) = [0xA0000, 0xC0000) || (addr − 0xE0000000) < 8 MiB`
(unsigned arithmetic; safe below the LFB by wraparound). Boundaries verified against
the reference source, not just R1F's measurements:

- **VGA window**: `vga.js:384` registers exactly `[0xA0000, +0x20000)` ⇒
  `[0xA0000, 0xC0000)`. ✓
- **LFB**: `cpu.js:1193` constructs `VGAScreen(..., settings.vga_memory_size ||
  8*1024*1024)`; `vga.js:216-227` clamps to `[4*VGA_BANK_SIZE, 256 MiB]` and rounds up
  to a power of two — 8 MiB is a power of two inside the range, so the effective default
  **is** 8 MiB. The Rust router serves the LFB from the zeroed svga buffer
  (`memory.rs:80-82, 86-92`) and the window through the JS mmap table. ✓
- R1F's measurements (0xB8000=0, 0xBFFFF=0, 0xE07FFFFF=0, 0xE0800000=0xFF) agree with
  all four boundaries. ✓
- **The asymmetry claim holds in both directions.** Pre-fix (hooks return 0): exactly
  the 7 unclaimed checks fail while the VGA checks pass (r1f-fixes.md's raw log).
  The R1 blanket prescription (0xFF/−1 everywhere) would FAIL the 5 VGA-blob checks
  (`cpu_selftest.c:819-823` expect 0 at 0xB8000/0xE0000000/0xE07FFFFC), because those
  addresses route through the same device hook. The carve-out — not merely the F1 value
  — is therefore what the probe pins. It is a real test in both directions, not a
  test that cannot fail.

Also checked among the landed files: `memcpy_into_svga_lfb` (memory.rs:254-264: the
three DBG_ASSERTs, `Page::page_of == >> 12`, mark-dirty-then-copy),
`get_real_eip` (cpu.rs:4300-4304), `popa16/32` (cpu.rs:3054-3078: the 16/32-byte
pre-check, the DI/SI/BP+adjust/BX/DX/CX/AX order, `.unwrap()` → `DBG_ASSERT(!pagefault)`),
`paging.c:117`'s `call_interrupt_vector(CPU_EXCEPTION_PF, false, true, error_code)`
(PF always pushes an error code), the `has_error_code` spelling of `Option<i32>`
(cpu.rs:1051/1889 control only the push), the prefix constants (prefix.rs:1-15 exact),
`segment_prefix`/`get_seg_prefix` (cpu.rs:3088-3110 exact), and the Makefile wiring.

---

## Notes (not findings)

1. **The VGA-window default (flat 0 reads) equals the reference only at boot state.**
   The reference's `vga_memory_read` is a function of VGA registers (plane select, read
   mode 0/1, chain-4); once a guest programs VGA ports the flat-0 default diverges. The
   C tree has no VGA ports until M2, so this is the documented seam (memory.c:105-118
   states it); M2 replacing the table with a real VGA is the exit. Same class: blob
   writes are no-ops in the default table where the reference stores them —
   self-consistent until M2.
2. `instr_62` (BOUND), `instr_F1` (INT1), and the rep-prefix
   `DBG_ASSERT((prefixes & PREFIX_MASK_REP) == 0)` are asserts-on aborts / release no-ops
   exactly as upstream's `dbg_assert!` — the selftest cannot call the first two
   (documented, F-7).

## Could not check (stated)

- The emitter interplay (`run_instruction`, `after_block_boundary`): not this unit's
  file; another unit is concurrently writing `gen/` and `src/cpu/cpu.c` (ignored per the
  brief).
- The D8-DF bodies (deferred by design, M1b's `fpu.c`).
- The selftest's expected **values** were not re-derived (the 10 gate functions exist
  and are wired into main — cpu_selftest.c:4153-4162 — and the orchestrator confirmed
  2968 checks/0 failures, but the constants are the implementer's).
- PAE (trapped by design) and the JIT-only dropped paths (by design).

## Evidence

- `handoff-v86c-review2` (memory node, type review, `--reviews--> handoff-v86c-result`) —
  the authoritative record.
- This file: `results/r2-review.md`.
- Measurements taken during the pass: `awk` function count over instructions.rs:1643-2003
  (160); a Python script over `instructions.c` verifying all 151 `SAFE_READ_WRITE` call
  sites pass a plain `addr` parameter and the macro is the last statement of its caller;
  `sha256sum` of the ten reviewed files (above).
