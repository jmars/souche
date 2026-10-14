# JIT-2R — independent review of the JIT runtime (jit_codegen, jit_x64, jit_mem, uop_cache integration)

**Unit:** `HANDOFF=v86c-jit` JIT-2R (reviewer). **Verdict: one BLOCKER (silent wrong
codegen, reachable in the shipped corpus) — the JIT must not default ON until it is fixed.**
Everything else on the recipe-by-recipe surface checked out clean.

Reviewed sha256s (the tree as reviewed): jit_codegen.c `ff09889a…ae35`, jit_codegen.h
`0b9ee34e…bdde`, jit_x64.c `229724b5…75bb`, jit_x64.h `8149a99c…bbd44`, jit_mem.c
`da5d73cb…aa39`, jit_mem.h `d0e18aa5…e702`, uop_cache.c `023e6123…1641` (including the
orchestrator's getenv-removal edit, reviewed in place), jit_codegen_selftest.c
`138696a7…6078`.

---

## BLOCKER — the two `and …, 0xFF` byte masks in the 8-bit register helpers are no-ops

**Where:** `src/cpu/jit_codegen.c:211` (`jitcg_emit_read_reg8`, high-byte arm) and `:225`
(`jitcg_emit_write_reg8`, high-byte arm). Both call `jx64_alu_rm_imm(e, 4, …, 0xFF, true)`,
which emits `83 /4 ib` — **the imm8 form sign-extends 0xFF to 0xFFFFFFFF**, so the mask does
nothing. Verified by compiling the tree's own `jit_x64.c` against a 16-byte probe and
disassembling: `41 83 e2 ff` = `and r10d, 0xffffffff` (not `and r10d, 0xff`).

**Consequences (silently wrong values):**
- High-byte **read** returns `reg32 >> 8` as a 24-bit value, so `movzx r32, ah/ch/dh/bh`
  (`0F B6` with rm 4–7) writes the shifted value into the 32-bit destination.
- High-byte **write** shifts the unmasked source, so `mov ah/ch/dh/bh, <high-byte reg>`
  (`88`/`8A` with both operands high) ORs the source register's bits 16–31 into the
  destination's high half.
- Both are wrong whenever the source register's bits 16–31 are nonzero. The selftest matrix
  never exercises either pattern (its byte cases are `mov al,ah` / `mov ah,al` / `mov ah,cl` —
  low-byte source or destination, which consume only the low byte and pass despite the dead
  mask), and `movzx`-from-high-byte is absent from the matrix entirely.
- **The bytes are in the shipped corpus** (byte scan of `tests/images/`): `0F B6 C4–C7` ~20×
  in `vmlinuz-i386` and `busybox-i386-v86guest`; high,high byte moves 4340× in `vmlinuz-i386`,
  1746× in `guest-initrd-fxdisk.xz`. The gates pass because executed sites either have zero
  upper halves or the corrupted half is overwritten before use.

**Fix (trivial):** `imm8=false` on both calls (`81 /4 id`, real mask), plus two matrix cases
(`0F B6 C4` = movzx eax,ah; `88 FC` = mov ah,bh) with distinct nonzero high halves so each
fails before the fix and passes after. Full code on `handoff-v86c-jit-fix-1`.

This is exactly the class the brief names: a divergence the corpus may or may not catch.

---

## SHOULD-FIX

1. **Four of the twelve jcc recipe entries have no differential case and no table pin**
   (`instr32_0F83`/`0F85`/`0F87`/`instr32_76`; matrix executes 8/12, table test pins 4/12 fn
   pointers, `jit_codegen_selftest.c:1441–1474, 1559–1564`). The `jitcg_jcc8_fns`/
   `jitcg_jcc32_fns` arrays are positional; a wrong pointer would silently de-inline a cc
   (safe but invisible — no per-case `inlined ≥ 1` assertion exists for these four). Add the
   four cases or pin all 12 pointers.
2. **Eight encoder functions are neither called nor byte-pinned**, contradicting jit_x64.h's
   own claim that every emitter is one or the other: `jx64_inc_rm32`, `jx64_dec_rm32`,
   `jx64_not_rm32`, `jx64_alu_rm8_imm8`, `jx64_test_rm8_imm8`, `jx64_shift_rm32_cl`,
   `jx64_test_rm32_r32`, `jx64_mov_r16_rm16`. Pin them or delete them.
3. **`jit_cache_bind` failure paths leak the reserved span** (`jit_mem.c:312–364`): block-table
   full, page-carries-block, span mismatch, and gpage-out-of-range all return false leaving the
   pages JP_RX forever. Block-table exhaustion leaks one span per decode while full. A leak,
   not a wrong answer — recycle the span before returning.
4. **No cpl==3 or TLB_READONLY selftest pin**: the two folded TLB mask bits
   (`jit_codegen.c:282–283`) are only as good as the boot's coverage. The fold is sound (state
   is keyed; cpl cannot change mid-block), but a one-page cpl3 program and a write to a
   read-only page would pin both bits cheaply.

## NICE-TO-HAVE

`JX64_MAX_FIXUPS` (512) sits below the worst-case 64-uop all-RMW fixup budget (~640) — such a
block is refused-for-fit (safe fallback, a coverage edge). jit_mem.h:108–110 still says ONE
BLOCK PER CODE PAGE (stale after the M2 span change). The `JITDBG`/`JITCG_DUMP` getenv hooks
remain on compile/invalidation paths (not hot, but the project's own D-F1 rule says shipped
debug hooks go).

## What was checked clean (the questions that matter)

- **A — every emitted sequence vs the executor:** operand order, `previous_ip`/`prefixes`
  stores, the `ran` gate (and that false-`ran` does not clear `jit_block_boundary`), flag
  writes (`flags_add/sub` last_op1-before-result order, the sub inversion via
  `flags_changed>>31`, the logic-tail materialised CF/OF/AF clears, inc/dec CF preservation),
  the TLB transcription (entry test mask, `(entry&~0xFFF)^addr`, straddle test on the linear
  address — equivalent because translation preserves the low 12 bits —, the
  `in_mapped_range` window), the segment lookup (null → slow = the C body's #GP+pagefault
  path), and the lea/jcc/jmp `instruction_pointer` writes. All four exit kinds and their
  continuations match `uop_run`'s interpreted path, including pending survival through
  BLOCK_END and the abort's keep-the-count re-decode.
- **C — inlined RMW invalidation:** every store recipe (mov [m] all widths, ALU RMW with
  aux ≠ 7, `not [m]`) passes through the code-page gate and falls back to the C body, whose
  own `uop_invalidate_page_fast` hook fires; reads correctly skip it; `need_code` is set for
  exactly those kinds.
- **D — fit rule:** contiguous multi-page span, 16 KiB cap, refuse-not-truncate, publish/bind
  re-verify the span; no truncation path exists.
- **E — jit_mem:** W^X one-mmap + per-span mprotect, deferred recycle of the executing span,
  abort-flag equivalence with `uop_abort_pending`, pool exhaustion → stop compiling,
  `jit_code_clear_all`'s correctness role on the shared bitmap, aarch64 compile-out complete.
- **F — selftest teeth:** a real differential (full `cpu_state_t` + 2 MiB RAM + pending +
  vector across interpreter/uop-cache/compiled arms, `inlined ≥ 1` per case so nothing passes
  vacuously), able to fail by construction — and it has the one blind spot the blocker names.

## Coverage statement

Read in full: all eight files above (the executor loop, invalidation/clear, and decode walk of
`uop_cache.c` line-by-line). Reference bodies read for the operand-order check: `flags.c`
15–178, `arith.c` 396–422, every recipe fn's body in `instructions.c`, `paging.c` 274–339,
`paging.h` 151–221, `memory.h` 93–96. Executed evidence: one scratch disassembly probe of the
defect; a byte scan of `tests/images/` for the affected encodings. Not re-run (orchestrator-
verified, per brief): the make/nasm/transcript gates, the boots, the aarch64 build, the
mutation rigs. Not checked by anyone: macOS/Windows W^X (stated UNVERIFIED in-tree).
