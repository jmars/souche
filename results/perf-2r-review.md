# PERF-2R — independent review of M2 (the uop cache) and PERF-FIX (unity guard + asserts)

**Reviewer pass.** Brief: `/tmp/brief-v86c-perf-2r.txt`. Memory node: `handoff-v86c-perf-review2`
(relates `reviews` → `handoff-v86c-perf-result`; `produces` → `handoff-v86c-perf-fix-2/3/4` with
the full fixes). Read-only: nothing under review was edited.

## 0. Verdict

The M2 executor, cache, staircase and the PERF-FIX unity guard are carefully built and mostly
faithful. The executor was checked **clause by clause** against `interpreted_run`
(src/cpu/cpu.c:111-156) and matches: the counter staircase (pending += 1 unconditional, flush at
`FLUSH || jit_block_boundary`, page-cross flush, iteration-limit break, tail-fetch discard, SMC
abort keeping the count, the sti block-end return keeping pending), `previous_ip`, prefix
re-materialisation, the sti shadow accounting, and the LOOP_COUNTER budget window. The
`unity_symbols.py` check (C) is sound, its fatal/warning split is justified by its own
measurement, and PERF-FIX's correction of PERF-1R's prescribed check is **CONFIRMED** (the
silent rebind lives on the *undefined* side; defined-global parity cannot see it). The five
"provably dead" assert claims are **CONFIRMED** sound (the port's linear-ip model:
`get_real_eip()` subtracts the CS base, so a 16-bit operand is `< 0x10000` regardless of base).

**But the invalidation reconciliation — the item that matters most — is incomplete.** One
BLOCKER class and two SHOULD-FIX holes, all silent-stale-code paths.

## 1. Findings, severity-ranked

### BLOCKER-1 — the RMW write macros have no invalidation hook (a missed site the grep cannot see)

`src/cpu/paging.h:143-203` — `SAFE_READ_WRITE8/16/32` write guest RAM via
`write*_no_mmap_or_dirty_check` (:154/:176/:200) with **no `uop_invalidate_*` and no SMC abort**.
There are **165 call sites** (151 in `src/cpu/instructions.c`, 14 in `instructions_0f.c`): every
read-modify-write instruction with a memory operand — `add/sub/and/or/xor/inc/dec/not/neg/rol/
ror/rcl/rcr/shl/shr/sar/bt*/xadd/cmpxchg [mem]` — executed through the cache.

- Upstream dirties the page at exactly these sites: `v86/src/rust/cpu/cpu.rs:4052-4120`
  (`jit::jit_dirty_page(Page::page_of(phys_addr))` in `safe_read_write8/16/32`, plus
  `dbg_assert!(!jit_page_has_code(...))` on the skip arm). The port dropped it when it dropped
  the JIT; M2 restored the hooks at the plan's §4.1 sites but this family is not one of them.
- The plan's §4.1 table and `results/perf-4-recon.sh`'s list B **cannot see the class**: the
  writers live in a *header macro*, and the grep scope is
  `memory.c/paging.c/string.c/main.c/hw/*.c` — the call sites are `SAFE_READ_WRITE*(...)` macro
  invocations, not function calls the regex matches.
- Consequence: `xor [addr], imm` / `add [addr], imm` self-modifying code (boot-sector
  decryptors, packers — the classic idiom) writes code pages with no unlink and no abort: the
  executing block keeps running its stale uops, and a later branch to the patched bytes hits the
  stale block. Silently wrong guest — the plan's risk #1 verbatim.
- The SMC selftest **cannot catch it**: its patch instruction is a plain `0xC7` mov
  (`uop_exec_selftest.c:240`), which goes through the hooked `write32`.
- The macros' *straddle* arms are fine (they route through `virt_boundary_write16/32` →
  `write8`, hooked); only the non-straddle arm is unhooked.

**Fix** (full code on `handoff-v86c-perf-fix-2`): `#include "uop_cache.h"` in paging.h and call
`uop_invalidate_page_fast(phys_addr_)` before each of the three raw writes; extend the recon
script to scan `paging.h`; add an SMC selftest arm whose patch is an RMW form
(`xor byte [0x100B], 0x2A`).

### SHOULD-FIX-1 — `write16`/`write32` hook only the first page; the "cannot straddle" comment is false

`src/cpu/memory.c:365-398`: the non-mmap arms call `uop_invalidate_page_fast(addr)` once, but
the routers have **no boundary split** — a 16-bit write at `0xFFF` (or 32-bit at `0xFFD`–`0xFFF`)
memcpy's into the next physical page, whose cached blocks survive. The comment at :371-374
("a 16-bit write at 0xFFF goes through `virt_boundary_write16`") is true **only for the
`safe_write*` and `SAFE_READ_WRITE*` paths** (which have the explicit straddle arms,
paging.c:667/692) — not for the router itself. Upstream spans both pages
(`jit_dirty_cache_small(addr, addr+2/4)`, `v86/src/rust/cpu/memory.rs:204-232`).

- Reachability today: instruction stores go through `safe_write*` (straddle-safe, verified —
  e.g. `push16_ss32` uses `safe_write16`), so the live callers of the plain routers are the
  **virtio used-ring writes** (`src/hw/virtio.c:1062/1068/1069/1074`) at guest-chosen
  `used_addr` — a ring placed across a code-page boundary writes the code page unhooked — and
  any future caller.
- Same silent-stale-code class as BLOCKER-1, with a narrower arrangement. The unit's own report
  (results/perf-4-uop-exec.md:166-167) carries the same over-wide claim about the router.

**Fix** (`handoff-v86c-perf-fix-3`): hook the last byte's page when straddling
(`(addr & 0xFFF) == 0xFFF` → `fast(addr+1)` for 16; `>= 0xFFD` → `fast(addr+3)` for 32) and
correct both comments.

### SHOULD-FIX-2 — `uop_exec`'s fault/guard contract diverges from the generated dispatcher (two facets, one reorder)

`src/cpu/uop_cache.c:932-1008`:

**(a) Order.** The `task_switch_test` guard runs **before** the address materialisation
(:934-937, then :951-961), while the generated dispatcher resolves **first** and guards *inside
the body* (gen/interpreter_c.js:261-273: `modrm_resolve` + `if (cpu->pagefault) return;` before
the call; :200-203: the guard inside the body). For a TS-guarded memory-form instruction (FPU
0xD8–0xDF, `fxsave`/`fxrstor`, `ldmxcsr`, sse/mmx mem forms) whose segment faults while
CR0.EM/TS makes the guard fail: the reference delivers **#GP** (resolve faults first) and the
executor delivers **#NM/#UD** (guard tested first) — a different exception vector, guest-visible.

**(b) Return value.** A materialise fault returns `true` ("`modrm_resolve`'s own
return_on_pagefault!", :959), which the caller reads as *ran* — so a **faulted flush-point
instruction** (UOP_FLAG_FLUSH set, body never ran, postfix never ran) triggers `uop_flush()` at
:1190 where the reference's postfix was skipped by the resolve-fault `return`. The counter moves
mid-batch; a same-page handler's `rdtsc` reads a different staircase. The port's own D1 comment
(cpu.c:19-33) documents same-page handlers as the constant case in small kernels.

**Fix** (`handoff-v86c-perf-fix-4`): materialise `arg0` **first** — `return false` on fault (no
body, no postfix, **no flush**) — then test the guard (`return false`). Nothing else reads
`ran`; the faulted instruction is still counted (pending += 1 is unconditional, matching the
reference's unconditional `i += 1`).

### NICE-1 — `UOP_DESC_FLUSH` is a second, hand-maintained copy of the postfix predicate (question C)

gen/interpreter_uop.js:215-221 re-writes interpreter_c.js:187-190's expression verbatim, and
`make uop-gen-check` compares only the availability verdicts, **not the postfix predicate**. The
staircase pin exercises a handful of the 242 flush descriptors; drift on any other descriptor
changes the guest's clock silently. Suggest sharing the predicate between the two emitters, or
adding a uop-gen-check arm that derives per-encoding "body calls `after_block_boundary`" from the
generated interpreter and compares it against the descriptor table's `UOP_DESC_FLUSH` bits.
(Risk is moderate — both sites cross-reference line numbers — but the two copies *can* drift and
no gate would speak.)

### NICE-2 — the mid-batch fallback (`uop_step`) has no exercising gate (question B)

`uop_step` (uop_cache.c:1095-1131) is reached only by a page-crossing or no-descriptor
instruction mid-batch. The staircase pin's "crossing rdtsc" at 0x1FFE **ends exactly at the page
end** (uop_exec_selftest.c:301-302), so its bytes stay inside the decode window, it runs as a
**uop**, and the page-cross *flush* fires — `uop_step` never runs there; the boot and bench both
measure 0 fallback steps. The path's code reads correct against cpu.c:118-153 (one cosmetic
miss: no `DBG_ASSERT(cpu->prefixes == 0)` after the dispatch, cf. cpu.c:134). Suggest one pin
arm whose instruction's own bytes cross the page (e.g. a 6-byte instruction at 0x1FFC), compared
cache-on vs cache-off.

## 2. Checked and safe (the rest of the reconciliation and the brief's other questions)

- **write_blob / device DMA**: confirmed — `dma.c:240`, `ide.c:829`, `virtio.c:1409` all call the
  CPU's hooked `write_blob` (range invalidation); the virtio used-ring writes go through the
  hooked `write16`/`write32` routers (subject to SHOULD-FIX-1).
- **string.c**: all five fast-path sites hooked (INS :280 page; MOVS :300 range; STOS-B :312
  range; STOS-W/D :318/:322 page, each page-bounded by `count_until_page_end`); the slow path
  uses `safe_write*` (hooked).
- **paging.c**: all five `safe_write*` non-mmap arms hooked (:655/680/702/732/760); straddle
  arms route through `virt_boundary_write16/32` → hooked `write8`s; A/D-bit writes go through
  the hooked `write8` router.
- **Loader-time class**: holds. `zero_memory` has no runtime caller (the reboot path does not
  exist — it is a loud `unimplemented_instruction` stop: ps2.c:618, pci.c:94); kernel.c's static
  `write_blob` + cmdline byte and main.c's loader copies run before the first `uop_run`, and the
  cache initialises lazily. `vga_mem8` writes are the LFB, which `uop_block_decode` refuses.
- **SMC abort semantics**: the abort is checked at the next uop boundary with the count KEPT;
  the hook unlinks the page's blocks before setting the flag; a write to a non-executing cached
  page is handled by the unlink alone; the abort re-entry recomputes `phys` within the page
  (blocks never cross a page).
- **Executor vs interpreter** (question B): verified clause-by-clause (see §0); `in_hlt` can only
  be set by `hlt` (instructions.c:1945), which ends the block, so the `do_many_cycles_native`
  re-test loop is safe; the lookup key `(phys_start, virt_start, state)` is sound — `virt_start`
  is load-bearing (uops carry virtual eips for `previous_ip`/fault reporting; aliases must
  miss), and the unit's statement that no gate pins that half is honest.
- **Unity guard** (question D): the (C) check's fatal/warning split is justified by measurement
  (the injected real-tree shadowing also drops a `roundf` reference, which would be a false
  alarm if fatal); the `src/main.c` gap (a shadow of a main.c global would be a warning) is
  measured empty today — the documented `main.o`-rule fix should land when the link is next
  touched. The PERF-FIX unit's claim that PERF-1R's prescribed parity-only check does not catch
  the class is **reproduced from the logic** (the rebind is invisible on the defined side).
- **The 14 asserts** (question D): the drop matches the release reference's codegen
  (`dbg.rs:63` debug-assert, compiled out); the five "provably dead" claims hold (linear-ip
  model); the nine reachable sites use the value as an address, never an index; the
  remaining-14-DBG_ASSERTs invariant-vs-guest classification was NOT swept (unit-stated), and
  the 15th site (`cpu.c:100`) is reported, not fixed — both acceptable, stated gaps.
- **Shipped default ON** (question E): right. The fallback is correct by construction —
  `interpreted_run` verbatim for whole batches, `uop_step` mirrors the loop body — so a
  low-hit-rate workload degrades to interpreter speed, never to wrong behaviour. The library
  default OFF (harnesses write `mem8` directly, measured segfault) is a stated, defensible
  asymmetry.

## 3. Coverage

**Read in full:** `src/cpu/uop_cache.c` (1295), `uop_cache.h` (385), `cpu.c` (326),
`memory.c` (508), `uop_exec_selftest.c` (604), `gen/unity_symbols.py` (156),
`results/perf-4-uop-exec.md` (371), `results/perf-fix.md` (402), `results/perf-4-recon.sh` +
`results/perf-4/reconciliation.txt`.

**Read in part (the load-bearing regions):** `paging.h` (full, 205), `paging.c:560-766` (all
safe-write arms; translate paths sampled), `string.c:1-60/150-350` (both paths + hooks),
`instructions.c` (file header + the 14 sites' bodies + `instr_FA/FB`), `misc_instr.c`
(`get_real_eip`, `task_switch_test[_mmx]`, `push16_ss32`), `gen/interpreter_c.js:180-300/328-412`
(guard/postfix/resolve order), `gen/interpreter_uop.js:1-60/195-270` (iflags emission),
`main.c:1130-1209` + flag parsing, `Makefile` (unity wiring, flags, source lists), upstream
`v86/src/rust/cpu/memory.rs` (write16/32) + `cpu.rs` (`safe_read_write8/16/32`).

**Inventoried by grep:** every `mem8` write; every `write*_no_mmap_or_dirty_check` caller;
every `write_blob` caller; all plain `write16/32` router callers; all `SAFE_READ_WRITE` call
sites (165); `zero_memory` callers; `in_hlt` setters; reboot paths; `prefixes = 0` sites.

**Not checked (orchestrator-verified, per the brief):** the gate re-runs, the bench timings, the
mutation-rig mechanics, the nasm/boot runs. The concurrent PERF-FIX-2 edits to `cpu.c`'s assert
and `tests/perf_bench.sh` were ignored as briefed.

**sha256 of reviewed files** (full): uop_cache.c `886d9334f4896fc9be5b883450134e4e0879d56a764a5220d2b27d9fb9f63db2`;
uop_cache.h `b387322eb727bed51323f2441faec3bca92ee9877f2cb320d36da496f55d7797`;
cpu.c `8b46c9597f756221ed0b385a8fe51576a348b0ff983a27856b218765898043fc`;
cpu.h `69558c4558eb2f254f06cfb0dbca73dbac3dab67e83c8b9cf3a85aaff9583675`;
memory.c `1439a48c98619cfae7e988e5fe503feaca73acb9082da9b93dab23abb27ab8bc`;
paging.c `a6a0f1ed2e7d7249cc47fa7834cb65175bdcaba1894e2e91e2b8aeebed7c6542`;
paging.h `08e27bb7c884cb08e1c7c1de824eb4248af386707696217cf753819db4cc0a97`;
string.c `73bb9822af0ea1dfd264f234ae572dd51a37894e29d77cdacece1ddb86604ae6`;
uop_exec_selftest.c `4b0a5b14e87a41b250c91235404e26c7774fc4a5c602d0457edc14b1e97619e7`;
unity_symbols.py `e5516193f18fa20af892f717da0ee74054db8771afc954d0cd92d67c82b49b8e`;
unity_c.py `1252984f83175ba13db7e9a0d803be0c4bb220d6c8742a0afcb14b96f9b36b39`;
interpreter_uop.js `4f7694d478f96dda68b4b9b3828fb90e75a95de61a9aad73e05e4de78beda217`;
interpreter_c.js `c01a320897b87eafdb84a2c31b6d28312e1af5b1a512a21564d55492050f9a08`;
main.c `2cf3c094ce60865d4a4473fac24c9dd452da08754da61aeef99896aaae3e1cc6`;
instructions.c `38256a3bc7b5379015382a6e80b1ceede625ea94ab53cd7fc6ac185e1eb8b197`;
Makefile `bb6db73fb7562aeb54d73d251014c3efed9eb6bc22e7fd1fad92337bb07512eb`;
perf-4-uop-exec.md `87bdf2b8edbc1fafe014605ea908bcf127c9ac00ed8a362e5cdae2cf56e0a625`;
perf-fix.md `ce28d0af8c94f5c8717fe5dc137d237accd667f69f4563e2f19ba1467f4b2e37`.
