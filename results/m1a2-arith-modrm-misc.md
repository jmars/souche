# M1a-2 — arith, modrm, misc_instr: the instruction-semantics spine

**Unit:** `HANDOFF=v86c`, unit M1a-2 (coder). **Deliverables:** `src/cpu/arith.{c,h}`
(port of `v86/src/rust/cpu/arith.rs`, 1206 lines), `src/cpu/modrm.{c,h}` (port of
`modrm.rs`, 133 lines, + the addressing/fetch helpers of `cpu.rs`), `src/cpu/misc_instr.{c,h}`
(port of `misc_instr.rs`, 613 lines, + the misc helpers of `cpu.rs`), and the extension of
`src/cpu/cpu_selftest.c`. `v86/` read-only; `tests/` and `oracle/` untouched
(`tests/test.img` sha256 still `1a597ac1…e737a`).

**Bottom line.** `make selftest` → RC=0, **2406 checks, 0 failures**; `make` → RC=0 with the
three new translation units compiling clean under `-Wall -Wextra -Werror`; `make oracle` →
RC=0; `make nasm` → RC=0. All 14 single-line mutations of this unit's semantics make the
suite FAIL and restore green (`results/m1a2-gates.txt`). Beyond the brief's gates, the
expected values in the tests are **measured on the host CPU** rather than derived from the
same reading of the code as the implementation: 8657 cases across the shift/rotate/shld/shrd,
mul/imul, div/idiv, bt/bsf/bsr and logic families compare equal to real hardware results and
real EFLAGS, and 1320 BCD rows compare equal to a 32-bit probe (`results/m1a2-oracle.txt`).
Two real port bugs were found by the tests during development (D-14, D-15 below) and one
divergence from hardware in SDM-undefined territory (AAD's CF/AF) is recorded rather than
"fixed".

---

## 1. What changed (every file)

| File | Lines | What it is |
| --- | --- | --- |
| `src/cpu/arith.h` | 169 | the ALU surface: mul/imul/div/idiv, the shift/rotate family, bit test/scan, `saturate_*`, and the per-size public wrappers over `flags_*`. Carries the two porting rules (`<<` on `uint32_t`; the naming rule). |
| `src/cpu/arith.c` | 1268 | `arith.rs` transcribed function by function. |
| `src/cpu/modrm.h` | 79 | **the interface M1a-3 and every memory-operand instruction call**: `modrm_resolve`, `resolve_modrm16/32`, `read_imm8/8s/16/32s`, `get_seg_prefix(_ds/_ss)`, `get_seg(_cs/_ss)`, `segment_prefix`, `is_osize_32`/`is_asize_32`, the `PREFIX_*` constants (prefix.rs), and the fault contract. |
| `src/cpu/modrm.c` | 575 | `modrm.rs` + `cpu.rs`'s addressing/fetch/segment-offset helpers (line ranges in the header). |
| `src/cpu/misc_instr.h` | 140 | the misc surface: condition predicates, jumps/loops/cmov/setcc, the whole push/pop family, enter/lss, xchg/bswap, `task_switch_test(_mmx)`, `set_mxcsr`, the asize helpers, `read_moffs`; and the two declared boundaries (M1c's `switch_seg`, M1b/M1c's deferred six). |
| `src/cpu/misc_instr.c` | 607 | `misc_instr.rs:75-613` + the cpu.rs helpers; see §4 for the six functions that are NOT here. |
| `src/cpu/cpu_state.h` | +21 | **the only landed file touched**: the four `trigger_de/ud/nm/gp` declarations (M1c's `exceptions.c` owns the definitions). Declared in the spine header because they are the sibling primitives of the page-fault plumbing already there, and their first callers are this unit's files. No landed symbol is redefined. |
| `src/cpu/cpu_selftest.c` | 842 → 2531 | 9 new test functions (shift table, mul/div table, BCD table, bits/logic/saturate, modrm addressing, modrm fetch+faults, misc stack, misc jumps, task-switch) + the M1b/M1c stand-ins. |
| `Makefile` | +4 | the three new translation units added to `CPU_SRC` and to the object rule's prerequisites. |
| `results/m1a2-gates.txt` | 178 | raw G1/G2/G3 output. |
| `results/m1a2-oracle.txt` | 75 | the host-CPU oracle: method, raw results, the AAD finding. |
| `results/m1a2-oracle-{gen.py,driver.c,probe32.c}` | 660 | the oracle rig itself, kept so the measurement is reproducible. |

## 2. The interface (what the emitter and later units call)

`modrm.h` states it; the parts worth repeating:

```c
bool is_osize_32(void); bool is_asize_32(void);
int32_t modrm_resolve(int32_t modrm_byte);      /* 32- or 16-bit form per is_asize_32 */
int32_t resolve_modrm16(int32_t modrm_byte);
int32_t resolve_modrm32(int32_t modrm_byte);
int32_t read_imm8(void); int32_t read_imm8s(void); int32_t read_imm16(void); int32_t read_imm32s(void);
int32_t get_seg_prefix(int32_t default_segment);
int32_t get_seg_prefix_ds(int32_t offset); int32_t get_seg_prefix_ss(int32_t offset);
int32_t segment_prefix(int32_t default_segment);
```

**Fault behaviour** (the contract from `cpu_state.h`, unchanged): a fallible call sets
`cpu->pagefault` and returns the sentinel `0`; the caller checks the flag *immediately*
before using the value. `read_imm*` advance `instruction_pointer` by 1/2/4 and are the only
readers of the cached `eip_phys` translation; on a fault they leave `instruction_pointer`,
`eip_phys` and `last_virt_eip` untouched (upstream's `?` ordering).

**One deliberate extension of the flag's meaning, stated here because it is load-bearing:**
`get_seg`'s #gp path **sets `cpu->pagefault`** although the cause is not a page fault.
Upstream returns `Err(())` there — "the interrupt has been delivered, abort this
instruction" — and `cpu->pagefault` is this port's only abort channel. Without it, a null
segment would leave the instruction running with a bogus address. It is set **after**
`trigger_gp()`, and M1a-1's "set-only, cleared once per instruction by the dispatcher"
invariant is preserved.

`get_seg_prefix_ds/_ss` propagate a fault as the **sentinel 0**, not as `0 + offset`
(upstream's `get_seg_prefix(DS)? + offset` short-circuits). This was a real bug in the first
draft — see D-15.

## 3. Ownership boundary with M1a-1's `flags.c` (P-NOT-1)

Verified, not just honoured: `flags.c` owns `getcf/getpf/getaf/getzf/getsf/getof`,
`get_eflags`, `update_eflags`, `opsize_to_mask`, and the add/adc/sub/sbb/inc/dec cores as
`flags_add/…/flags_dec` + `flags_neg`. This unit owns the per-size wrappers
(`add8/16/32`, `adc8/16/32`, `sub8/16/32`, `sbb8/16/32`, `cmp8/16/32`, `inc8/16/32`,
`dec8/16/32`, `not8`, `neg8/16/32`) as one-liners over those cores, and nothing else from
`arith.rs:12-136` is duplicated. `opsize_to_mask` is `flags.h`'s; `arith.rs:7`'s own copy is
not re-declared.

## 4. The honest gap: six functions of `misc_instr.rs` are NOT ported here

Not silence — a named boundary, in `misc_instr.h` and here with the upstream lines:

| Function | Upstream | Why it is not here | Who owns it |
| --- | --- | --- | --- |
| `fxsave` | `misc_instr.rs:369-396` | defined entirely in terms of `fpu.c`'s `fpu_load_m80/fpu_store_m80/fpu_load_status_word`/`set_control_word` | M1b |
| `fxrstor` | `misc_instr.rs:397-428` | same | M1b |
| `lar` | `misc_instr.rs:458-503` | defined entirely in terms of the segment-descriptor API (`SegmentSelector`/`SegmentDescriptor`, `cpu.rs:381-446`) and `lookup_segment_selector` (`cpu.rs:2640-2740`) — i.e. M1c's contract. Writing them now means inventing that C interface, which is the one thing this unit must not do (a frozen wrong signature is worse than an absent function). | M1c |
| `lsl` | `misc_instr.rs:505-559` | same | M1c |
| `verr` | `misc_instr.rs:561-584` | same | M1c |
| `verw` | `misc_instr.rs:586-613` | same | M1c |

Their only callers are `instructions_0f.rs` (0F AE /0,/1 for fxsave/fxrstor; 0F 00 /4,/5
and 0F 02,/03 for the descriptor ones), i.e. M1b — nothing in M1a-3 can reach them, so no
unit is blocked. Everything else in `misc_instr.rs` is ported, including the parts that
needed an M1c declaration (`lss16`/`lss32` → `switch_seg`).

Two further functions the brief asked for are **not in `misc_instr.rs` at all** — the brief's
description of the unit is imprecise here (see §6, F-1):

* `cpuid` is at `instructions_0f.rs:3257-3410` (M1b's file), not in `misc_instr.rs`;
* `task_switch_test`/`task_switch_test_mmx` are at `cpu.rs:4210-4272`. Both ARE ported here
  (the brief calls `task_switch_test_mmx` MANDATORY because the emitter's SSE guard depends
  on it), together with `set_mxcsr`, `is_osize_32`/`is_asize_32`, `get_reg_asize`/
  `set_reg_asize`/`decr_ecx_asize`, `get_stack_reg`/`set_stack_reg` and `read_moffs`.

## 5. Divergences from the upstream Rust (every one)

| # | Where | Divergence | Why |
| --- | --- | --- | --- |
| D1 | `modrm.c` | the fetch path (`read_imm8/8s/16/32s`), the prefix decode (`is_osize_32`/`is_asize_32`, `PREFIX_*`), the segment-offset accessors (`get_seg`, `get_seg_cs/_ss`, `segment_prefix`, `get_seg_prefix(_ds/_ss)`) and `modrm_resolve` live here although upstream keeps them all in `cpu.rs` | the plan's step 8 assigns `modrm_resolve`+`get_seg_prefix` to this unit; `modrm.rs` reads its displacements through `read_imm*`, and `paging.c` explicitly left "read_imm16/read_imm32s, which are the fetch path of a later unit" (paging.c:446). One coherent addressing/fetch module. |
| D2 | `misc_instr.c` | `is_osize_32`/`is_asize_32` are in `modrm.c` (D1); `task_switch_test*`, `set_mxcsr`, `get/set_stack_reg`, the asize family and `read_moffs` are here although upstream keeps them in `cpu.rs` | the brief names the first two groups; the others are needed by this file's loop instructions and by `modrm_resolve`, and splitting them would put one register-size logic in two files. `read_moffs` is the `immaddr` reader the emitter calls. |
| D3 | `arith.c` `not8` | `~x` (bitwise) | upstream is `!x` in **Rust**, where `!` is the bitwise complement. C's `!` is logical-not: the literal transcription returns 0/1. Caught by the selftest (D-14). |
| D4 | `arith.c` | the private cores `and`/`or`/`xor` are spelled `arith_and`/`arith_or`/`arith_xor` | C keywords. The public `and8/16/32`, `or8/16/32`, `xor8/16/32`, `test8/16/32` keep their upstream names. |
| D5 | `arith.c` | every LEFT shift whose value can be negative or can exceed `INT32_MAX` is written on `uint32_t` and cast back (e.g. `imul8`'s `read_reg8(AL) << 24`, `imul_reg16`'s `operand1 << 16`, every `result << n` in the rotate/shift flag formulas) | Rust's `<<` on an `i32` is defined on the bit pattern (bits shifted out are discarded, no overflow check); C's `<<` on a signed value is **UB** in exactly those cases. The cast is bit-exact. RIGHT shifts stay signed (`dest_operand >> count`) matching Rust's arithmetic shift; every right shift in the file is bounded well below 32, which was checked case by case. |
| D6 | `arith.c` `int_log2` | a loop instead of `31 - leading_zeros()`, returning -1 for 0 | `__builtin_clz(0)` is UB; upstream's `leading_zeros()` on 0 gives -1 and both call sites are guarded, so the port matches the value without the UB. |
| D7 | `arith.c` `idiv16_without_fault`/`idiv32_without_fault` | an explicit `target == INT32_MIN/INT64_MIN && source == -1` guard before the division, returning `false` | Rust's `checked_div` returns `None` for `INT_MIN / -1` as well as for a zero divisor; C's `/` is UB there. Upstream's next test (`result >= 0x8000 …`) would reject the overflowed quotient anyway, so the divergence is invisible — it exists to keep the C defined. |
| D8 | `arith.c` `idiv32_without_fault` | the 64-bit target is assembled in `uint64_t` | upstream shifts an `i64` (`(target_high as i64) << 32`), which overflows the sign bit for a high word ≥ 2^31 — the same bits, without C's signed-shift UB. |
| D9 | `arith.c` `mul32`/`imul32`/`imul_reg32` | the high half is extracted as `(int32_t)((uint64_t)result >> 32)` | upstream's `(result >> 32) as i32` on a negative `i64` is an arithmetic shift followed by a truncation; the unsigned shift is the same bits. |
| D10 | `arith.c` `bt_reg`/`btc_reg`/`bts_reg`/`btr_reg`, `bt*_mem` | the bit mask is `(int32_t)(1u << bit_offset)` | `1 << 31` on a signed literal is UB; the register forms are always called with the offset masked to 15/31 (`instructions_0f.rs:3420-3422`) and the memory forms mask to 7 first, so no shift ≥ 32 is reachable. |
| D11 | `arith.c` `popcnt` | `__builtin_popcount((uint32_t)v)` | upstream `v.count_ones()`; identical for all 32 bits, and `-Werror`-clean. |
| D12 | `arith.c` `div8`/`idiv8` etc. | `trigger_de()` (declared in `cpu_state.h`, defined by M1c) | upstream calls the same-named function; the C port has one abort channel and one exception module. `div8`'s early `return` after `trigger_de()` is upstream's `return`, kept. |
| D13 | `modrm.c` | `DISABLE_EIP_TRANSLATION_OPTIMISATION` exists as a `#define 0` and the fast path is written out, rather than the branch being folded away | upstream keeps the constant (`cpu.rs:2582`) as a debugging switch; keeping it means the two paths are both present and the selftest can exercise the split (it does, under a page table where the two paths read different bytes). |
| D14 | `modrm.c` `read_imm16`/`read_imm32s` | the two `read_imm8()`/`read_imm16()` calls are sequenced into locals with an explicit fault check between them | upstream's `read_imm8()? \| read_imm8()? << 8` relies on left-to-right evaluation; in C the order of `\|`'s operands is **unspecified** and both calls mutate `instruction_pointer`. Same semantics, defined behaviour. |
| D15 | `modrm.c` `resolve_modrm16`/`resolve_modrm32` | the per-arm expression is split into "compute `offset` (and the default segment), with a fault check after each displacement read" + one `get_seg_prefix_ds/_ss` at the end | upstream repeats `get_seg_prefix_ds(<expr>?)` in every arm. The value, and the *order* (displacement first, segment lookup second), are identical; the single application makes the 48 arms readable and keeps the fault short-circuit exact. |
| D16 | `modrm.c` `resolve_modrm32_` | ported although it is **dead upstream** (no reference outside its own definition — verified by grep) | 1:1 parity with the file; it is `pub` upstream, so the selftest calls it directly rather than leaving an untested path. |
| D17 | `misc_instr.c` | `Result<(), ()>`/`Result<i32, ()>` become `void`/`int32_t` + the `cpu->pagefault` flag; the check is placed exactly where upstream's `?` short-circuits | the port's fault contract. It matters for the stack: `push16`/`push32` leave the stack register **unchanged** on a fault (upstream's `?` returns before the register write), and the selftest asserts that (`SP` stays 0x1234 after a faulting push). |
| D18 | `misc_instr.c` `pusha16`/`pusha32`/`enter16`/`enter32` | upstream's `.unwrap()` on an already-pre-checked result becomes `DBG_ASSERT(!cpu->pagefault)` | upstream *panics* there; a debug C build aborts, a release build continues. Every site is behind `writable_or_pagefault`/`readable_or_pagefault` of the exact range, i.e. unreachable unless the stack wraps its own segment inside the 16 bytes the pre-check covered. Stated, not hidden. |
| D19 | `misc_instr.c` | `dbg_log!`/`dbg_trace()` dropped (in `enter16`/`enter32`, `set_mxcsr`'s four "Unimplemented MXCSR …" warnings, `get_seg`'s "Access null segment") | no log channel exists in this port (same decision as M1a-1's D4/D12); in every case the log line was the only thing dropped and the state effects are kept. |
| D20 | `misc_instr.c` `bswap` | `__builtin_bswap32` | upstream `swap_bytes()`; same value for all 32 bits. |
| D21 | both headers | the C symbol is the upstream Rust name (`arith::add8` → `add8`), so the emitter maps a module-qualified call by stripping the module | no cross-file collisions exist among `arith.rs`/`misc_instr.rs`/`modrm.rs` names (checked). The only renames are D4 and everything `flags.c` already owns. |
| D22 | `cpu_state.h` | +21 lines: `trigger_de/ud/nm/gp` declarations | M1c defines them; this unit is their first caller. Identical re-declaration by M1c is harmless; a *different* signature is not. |

## 6. Findings about the brief, the plan and upstream

* **F-1 — the brief's file description is imprecise in two places.** It says
  `misc_instr.c` should port "push/pop, `get_stack_pointer`, `adjust_stack_reg`, **cpuid**,
  `task_switch_test`/`task_switch_test_mmx` … and the misc helpers upstream puts here".
  Measured: `cpuid` is **not** in `misc_instr.rs` (it is `instructions_0f.rs:3257-3410`, a
  150-line function with its own feature-leaf table, M1b's file), and `task_switch_test*` is
  `cpu.rs:4210-4272`, not `misc_instr.rs`. Both `task_switch_test*` functions are ported here
  as the brief requires; `cpuid` is deliberately left to M1b where upstream keeps it.
* **F-2 — P-NOT-3 overstates its case about the fetch path.** The binding finding says
  `read_imm16`/`read_imm32s` "need `virt_boundary_read16/32s` … USE THEM, do not write your
  own". Read against the code: upstream's `read_imm16`/`read_imm32s` (`cpu.rs:2598-2631`) do
  **not** call `virt_boundary_*` at all — they read `memory::read16`/`read32s` at the cached
  physical address, or split into two `read_imm8` on the page-crossing test. The only callers
  of `virt_boundary_read16/32s` are `virt_boundary_read32s` itself and
  `safe_read_write16`/`safe_read_write32` (`cpu.rs:4078`, `4107`) — the read-modify-write
  helpers, which are a later unit's file. The port follows the real code, so
  `virt_boundary_*` remain unused by this unit; M1a-3's `safe_read_write*` will need them.
* **F-3 — the brief's citation for `get_seg_prefix` is off.** `cpu.rs:3122-3130` is
  `modrm_resolve`; `get_seg_prefix` is at `cpu.rs:3098-3110` and `segment_prefix` at
  `3088-3096`. Both are ported; nothing was lost, but the line range is not what the brief says.
* **F-4 — AAD's CF/AF diverge from hardware, in territory the SDM leaves undefined.**
  MEASURED: a 32-bit probe gives AAD CF=1 (and AF=1) in 60/64 of the sampled cases where
  upstream's rule never sets CF and always clears AF. INTERPRETATION: not a defect — the SDM
  leaves CF/OF/AF undefined after AAD, and the port reproduces upstream bit-for-bit. Recorded
  because a guest reading CF after AAD (DOS `aad`/`daa` pairs did) sees upstream's behaviour,
  not the hardware's.
* **F-5 — the tests found two real bugs in the port, both in the "silent wrong result" class.**
  D3 (`not8` as C logical-not) and D15's fault propagation (`get_seg_prefix_ds` returning
  `0 + offset` on a failed segment lookup). Both are now regression-probed by the suite (M1 and
  M8 in `results/m1a2-gates.txt`). Neither would have been caught by reading the port again.
* **F-6 — the oracle harness was wrong before the port was.** The first run reported 35
  "mismatches", all `div8`/`idiv8`; the cause was in the *harness*: `divb` leaves the
  remainder in AH, not DX, so the capture always returned 0. Recorded because a "found 35
  mismatches" report is exactly the kind of finding that should be suspected of being the
  instrument before the phenomenon.
* **F-7 — `bcd_aad`'s CF rule is dead code upstream.** `if result > 0xFFFF { flags |= 1 }`
  (`arith.rs:354-356`) cannot fire: `result = AL + AH*imm8 ≤ 255 + 255*255 = 65280`. It is
  transcribed verbatim; the observation is that upstream's AAD never sets CF (F-4).

## 7. Gates (`make selftest`, `make`, `make oracle`; raw output in `results/m1a2-gates.txt`)

**G1 — `make selftest` → RC=0, 2406 checks, 0 failures:**

```
PASS reset/init values, state_flags, register accessors
PASS lazy flags: 24 (op,a,b,cf_in) cases + get_eflags/update_eflags
PASS memory: router, slack at memory_size, device hook table, mmio windows
PASS paging: 4 KiB translation, A/D bits, TLB read-only entry
PASS paging: faults (absent, read-only/WP, user vs supervisor) + cr2/eip/delivery/error codes
PASS paging: 4 MiB PSE page (translation, A/D, page boundary)
PASS paging: page-crossing reads/writes (16/32/64/128-bit) + straddle fault
PASS TLB: insert, invlpg, clear_tlb/global, set_cr3/set_cr0 flushes
PASS arith: 170 shift/rotate/shld/shrd cases (host-CPU-derived) + count==0
PASS arith: 46 mul/imul + 44 div/idiv cases (host-CPU-derived) + xadd/cmpxchg
PASS arith: 180 BCD cases (32-bit host probe) + aam 0 -> #de
PASS arith: bt/bsf/bsr/popcnt, logic flag clears, saturating helpers
PASS modrm: 32- and 16-bit addressing, SIB, segment defaults and overrides
PASS modrm: read_imm8/16/32s fetch, cross-page imm, moffs, faults, get_seg #gp
PASS misc: push/pop/pusha/enter/lss, stack-pointer helpers, fault paths
PASS misc: jmp/jcc/loop/jcxz, setcc/cmov, xchg/bswap, condition predicates
PASS misc: task_switch_test/-_mmx (CR0.EM/TS -> #ud/#nm), set_mxcsr
cpu_selftest: 2406 checks, 0 failures
```

What the new cases cover: the whole shift/rotate/`shld`/`shrd` family (result, CF, OF at
count==1, ZF/SF/PF re-derived from the result, rotates asserted to leave ZF/SF/PF/AF alone,
count==0 asserted to touch nothing); mul/imul (both result halves, CF/OF, the two-operand
forms); div/idiv (quotient, remainder, both #DE edges — zero divisor and quotient overflow,
including `INT_MIN/-1`); the BCD family with the 0x99/0x9A CF boundary; `bt`/`bts`/`btr`/`btc`
register and memory forms; `bsf`/`bsr`/`popcnt`; the logic ops' unconditional CF/OF/AF clear;
`xadd`/`cmpxchg`; all seven `saturate_*`; 32- and 16-bit modrm addressing including every SIB
base/index/scale corner and all three segment-override arms; the fetch path (cached fast path
vs per-byte re-translation under a page table, `moffs`, `imm8s` sign extension, `is_osize_32`
via the 66 prefix); the fault paths (unmapped fetch page, unmapped modrm displacement, the
get_seg #gp); the stack (push/pop/pusha/enter/lss in both stack sizes, SP unchanged on the
fault path); jumps/loops/jcxz with both address sizes; setcc/cmov; xchg/bswap; the condition
predicates; and both task-switch tests.

**G2 — every mutation makes the suite FAIL** (raw output per mutation in
`results/m1a2-gates.txt`): 14 single-line mutations, each applied alone, `make selftest` run,
source restored, restore re-verified green.

**G3 — `make` → RC=0**, all three new units compiled with `-Wall -Wextra -Werror`.

**G4 — `make oracle` → RC=0** (`tests/` untouched, `tests/test.img` sha256 unchanged:
`1a597ac1…e737a`) and **`make nasm` → RC=0**: `[+] Passed 113/113 tests`, `0 of 123 selected
test(s) produced no state`, `10 were skipped (tests/nasm_skip.txt)` — the skips are
pre-existing and documented in the runner's own output. Raw output in
`results/m1a2-gates.txt`, including the note that the first `make nasm` attempt was launched
detached inside a tool call and died when that call returned (the documented hazard), and that
the earlier "still running" check was a false positive — `pgrep -f nasm_run.py` matched its own
command line. The re-launched run is the one reported.

## 8. The independent oracle (why the table is not self-confirming)

The expected values in the selftest are not hand-derived from the same reading of the code
that produced the implementation: the **host CPU executed every operation** and its real
result and real EFLAGS were captured (`results/m1a2-oracle.txt`, rig in
`results/m1a2-oracle-{gen.py,driver.c,probe32.c}`).

* 8657 cases compared, **0 mismatches**: shl/shr/sar/rol/ror/rcl/rcr × 8/16/32 bits × every
  applicable count × 6–8 operands × cf_in ∈ {0,1} for the through-carry rotates; shld/shrd
  16/32; mul/imul 8/16/32 and both two-operand forms; div/idiv 8/16/32 including every #DE
  case; bt/bts/btr/btc; bsf/bsr; and/or/xor/test. For shl/shr/sar the ZF/SF/PF comparisons run
  too, and for the rotates the "ZF/SF/PF/AF unchanged" claim is asserted against hardware.
* OF is deliberately *not* compared for count > 1 (SDM-undefined): those rows carry
  `exp_of = -1` and assert nothing. The port's formula there is upstream's, transcribed.
* BCD: the six opcodes are #UD in 64-bit mode, so a 32-bit freestanding probe was built
  (`as`/`ld -m elf_i386`, running natively as the repo's own oracle does) and its 1320 rows
  diff clean against the port except AAD's CF/AF (F-4). `aam 0` → SIGFPE was measured by
  forking a child and reading the wait status.
* The oracle found a bug in *itself* (F-6) before it was trusted.

## 9. The M1b/M1c stand-ins in the selftest

`trigger_de/ud/nm/gp` and `switch_seg` do not exist until M1c lands, so `cpu_selftest.c`
supplies recording definitions (as M1a-1 did for `call_interrupt_vector`, for the same reason:
nothing links without them). They record the vector and, for `switch_seg`, return a settable
answer so `lss16`/`lss32` can be tested on both paths. **M1c replaces them; nothing here
shadows a real definition.** Anyone building a binary from this unit's sources without M1c
needs those definitions, exactly as with M1a-1's `call_interrupt_vector` stub.

## 10. Open items handed on

* M1a-3's emitter maps `arith::x` → `x`, `misc_instr::x` → `x`; the exceptions are D4 and the
  `flags_*` cores it must not call directly (it calls the `add8/16/32` wrappers).
* `safe_read_write16/32` (the `virt_boundary_*` callers, F-2) is unported and belongs to the
  unit that ports those read-modify-write helpers.
* `cpuid` (`instructions_0f.rs:3257-3410`) is M1b's, per F-1.
* The six deferred `misc_instr.rs` functions are listed with their owners in §4.
