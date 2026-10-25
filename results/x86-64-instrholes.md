# x86-64: THE INSTRUMENT HOLES THE REVIEW FOUND, CLOSED WITH ARMS THAT CAN FAIL

Unit: close the six holes `souche-instruments-review` reported in the DEBUG TOOLKIT's behaviour.
Repo `~/futaie/souche`, baseline HEAD `31732dd`. `v86/` untouched. Nothing committed, nothing pushed.
Scope: the toolkit (src/cpu/debug.{c,h}, the store seats in src/cpu/paging.c, the stop paths in
src/main.c and src/cpu/cpu.c) and its own arms in src/cpu/m1c_selftest.c. **The guest's execution
path is unchanged**: every store still issues the same router calls in the same order (the only
new work on that path is guarded by `v86c_dbg_watch_on`, false unless a tool is on).

Claim tags: **MEASURED** = a command run in this unit, output quoted; **READ** = read off the code
with the file:line given; **INTERPRETATION** = an inference over the two.

---

## 0. Bottom line

All six items are landed. Two of the review's findings were **wrong as stated** and one was
**narrower than stated**; the corrections are in ss1 and ss2 and they change what a reader of the
older reports should believe, so they are the first thing to read.

| # | item | verdict |
|---|---|---|
| 1 | long-mode qword store straddling a page reached NO watch hook (B1) | **the mechanism is refuted** (it reached the physical router); the REAL defect (no linear address, two lines for one store) is fixed, one report per store with both frames |
| 2 | `V86C_WATCH` prints 2-4 lines for one store (M2) | fixed: option (a), one store = one line, proved by count for a plain and a straddling store, and by two mutants |
| 3 | the stop dump fires only on a named stop (M4) | fixed: budget, HLT, undeliverable exception, reboot and named stop each dump once; the named stop is not dumped twice |
| 4 | the virtual peek can deliver on a non-canonical address (m2) | fixed at the walk (`trigger_gp` now honours `side_effects`) AND the peek refuses a non-canonical rsp; the walk's named stops can no longer stop the machine from a dump |
| 5 | the WATCH/RSPLOW arms do not cover their seats (M3) | two new arms drive the real store seat; both fail against mutants (measured: hook deleted -> 0 lines, guard removed -> 3 lines) |
| 6 | six small ones | all six done (cr2 label, RSPLOW gloss, watch-range wrap, the 32-bit JIT blind spot, and the counting rule in the header) |

Gates: all six green by EXIT CODE (ss8). Bench pin still 200,095,214.

---

## 1. HOLE 1 (review B1) -- the straddling qword store

### What the review said

> `safe_write64_addr` (paging.c, the `addr & 0xFFF > 0x1000-8` arm) recurses into
> `safe_write32_addr_local`, which has no `v86c_dbg_watch_*` call; the 16- and 32-bit straddle arms
> reach `safe_write8_addr`, which does hook. [...] a physical watch on a frame never sees
> straddling arrivals into the frame's first bytes.

### What the code actually does (READ, and MEASURED against a rebuilt pre-unit arm)

`safe_write32_addr_local` has no hook of its own -- true -- but it ends in `write32(p, value)`
(src/cpu/paging.c, the "write half" helper), and `write32` is the **physical router**, whose first
statement is `if (v86c_dbg_watch_on) v86c_dbg_watch_p(addr, 4, ...)` (src/cpu/memory.c:571). So the
straddling qword **did** reach a watch hook -- twice, once per half.

MEASURED, mutant C (the pre-unit straddle arm restored verbatim, `build/cpu_m1c_selftest` rebuilt
from it, exit code 1):

```
test_debug_watch_straddle_reports_once a straddling qword store reports exactly once ...
v86c dbg: WATCH P 0x1000 width=4 value=0xffffffffaabbccdd rip=0x7000     <- the 2nd frame DID fire
v86c dbg: WATCH P 0xffc width=4 value=0x44556677 rip=0x7000             <- and the 1st frame,
v86c dbg: WATCH P 0x1000 width=4 value=0x112233 rip=0x7000                once per half: 2 lines
FAIL src/cpu/m1c_selftest.c:728: v86c_dbg_test_counts() == base + 2 (got 0x8, want 0x7)
```

So, corrected:

* **B1's stated mechanism is false.** A straddling qword reached the physical hook, and a watch set
  on the frame its tail writes DID fire.
* **The real defect is narrower and still real.** (a) No `V -> P` line: a straddling store carried
  **no LINEAR address at all**, so on a long-mode guest whose mapping is not the identity, a watch
  set on the *virtual* address of a page-crossing store saw nothing. (b) It printed **two** lines
  for one store, i.e. it was one of M2's overcounts, not a silence.

### The fix

Every long-mode store now reports exactly once, from the seat that knows both addresses, and the
straddling arms report **one** line matched against **both** frames
(`v86c_dbg_watch_vp_split`, src/cpu/debug.c; `watch_hit` tests `p_lo` and `p_hi`).

Which arm got the hook, and why the per-half calls cannot double-report:

* `safe_write8_addr` / `safe_write16_addr` / `safe_write32_addr` / `safe_write64_addr`: the report is
  at the arm that issues the store, **after** the walk (the physical frame exists only then, and a
  store that faults never stores). For a straddle the low frame is translated first (so a fault is
  the same fault the store would have taken), the halves store, and the report follows.
* The per-half calls cannot double-report because the report entry points are **guarded**: the
  writer raises `v86c_dbg_watch_quiet` around its own store, and `watch_v`/`watch_p`/`watch_vp`/
  `watch_vp_split`/`watch_range`/`watch_fault` all return immediately while it is set
  (src/cpu/debug.c). It is a **counter**, not a bool, because a 16-byte store nests two qword
  stores inside one report; and `v86c_dbg_watch_quiet_depth()` is asserted back to 0 after every
  store in both new arms, so a path that raised it and returned early would fail loudly rather
  than silence every later report.
* The guard is raised **only** when `v86c_dbg_watch_on`, so the flag-off machine pays nothing, and
  the store calls themselves are byte-for-byte the ones that were there before.

A partial store is reported as what stored: if the high half's walk faults, the line says width 4
(the four bytes that landed), not width 8.

---

## 2. HOLE 2 (review M2) -- one store, 2-4 lines

**Chose option (a)**: the `_addr` paths report once and suppress the downstream physical-router
hook for that store, at the place that knows both addresses. Reason: the store is one event, and a
line that names both the linear address and the frame is strictly more informative than a `V` line
plus one or two `P` lines. Option (b) (a store id) would leave the reader to deduplicate; (a) makes
the count itself the answer.

**Coverage was the condition, and it is met**: `watch_hit` tests the entry against the linear
address AND against each physical frame of the store, so suppressing the router's `P` line loses
nothing -- a physical-range entry still matches (MEASURED: the straddle arm's entry is on phys
0x1000 and fires). The `P` hook stays in memory.c for every store that does **not** come through
`safe_write*` (the producer.md class).

MEASURED, the good build (one line per store, plain and straddling):

```
v86c dbg: WATCH V 0x3000 -> P 0x3000 width=8 value=0x1122334455667788 rip=0x7000      (plain qword)
v86c dbg: WATCH V 0xffc -> P 0xffc+0x1000 width=8 value=0xaabbccddeeff0011 rip=0x7000 (STRADDLES a page: one line for one store)
v86c dbg: WATCH V 0x1fff -> P 0x1fff+0x2000 width=2 value=0xcafe rip=0x7000           (STRADDLES a page: one line for one store)
```

and the arm asserts the **count** (not the text): `v86c_dbg_test_counts() == base + 1` for the
plain store, `base + 1` for the straddling qword (first frame), `base + 2` for the same store
watched by its linear address, `base + 3` for a straddling 16-bit store. MEASURED mutant B (the
guard removed, rebuilt, exit 1): the plain qword store prints `V -> P` plus **two** `P` lines --
`got 0x6, want 0x4`, i.e. 3 lines where 1 is asserted.

**Named residual, not fixed**: a 16-byte store (`safe_write128`, long mode) is two qword stores in
this port's split and prints **two** lines, one per qword, each with its own frame. Stated in the
header's counting rule rather than papered over. The 32-bit `safe_write16/32` *straddle* arms also
still print one `V` line at entry plus the router's `P` line(s) -- untouched, because their report
happens before the walk and moving it would reorder the fault path of a store that faults (a
consumer, not the toolkit). The 32-bit `safe_write64` non-straddle arm was the one 32-bit
double-report that is cheap to close and is **not** closed here either: same reason. Both are
32-bit-mode shapes; the lane's own measurements are long-mode `_addr` stores.

---

## 3. HOLE 3 (review M4) -- the dump only fired on a named stop

### What was wrong

`v86c_dbg_on_stop` had exactly one caller: `unimplemented_instruction` (src/cpu/cpu.c:82). A run
that ended by **instruction budget** (`count >= limit`, src/main.c's `run()` loop), by **HLT**, by
an **undeliverable guest exception**, or by a **reboot** printed no INTS summary, no RSPLOW ring and
no break totals.

### The fix

`run()` (src/main.c) now calls `v86c_dbg_on_stop` once at its single exit, with the reason it can
attribute, and **skips it when a named stop already dumped at its own seat**
(`if (!interpreted_unimplemented)`), so nothing prints twice.

Paths that now dump, and paths that deliberately do not:

| terminal path | dumps? |
|---|---|
| instruction budget (`count >= limit`) | **yes** -- "instruction limit" |
| HLT (`cpu->in_hlt`) | **yes** -- "hlt" |
| undeliverable guest exception | **yes**, and it already dumped at its own seat (`stop_undeliverable` -> `unimplemented_instruction`) -- the guard keeps it to one |
| named stop (`unimplemented_instruction`) | **yes**, once, at the seat that knows the reason |
| guest reboot (`reboot_requested`) | **yes** -- "guest reboot"; the re-entered boot dumps again at its own terminal stop, which is correct (a reboot is a terminal stop of that run, not of the process) |
| no progress (`count == previous_count`) | **yes** -- "no progress" |
| the transcript/fixture modes | the same `run()`, so the same dump (to **stderr** only; stdout is the document) |
| tools off | **nothing**: `v86c_dbg_on_stop` returns on its first line |

MEASURED (the lane's own shape -- the 32-bit kernel stopped by budget):

```
$ V86C_INTS=4 V86C_RSPLOW=6 V86C_BREAK=0x100000:4 ./build/v86c --bzimage tests/images/vmlinuz-i386 5000000 --cmdline console=ttyS0
v86c dbg: STOP (instruction limit)
v86c dbg: break slot 0 0x100000+0x4 hits=0
v86c dbg: INTS total=92 max_depth=0 printed=4 limit=4
v86c dbg:   INTS vectors: vec=16 x51 vec=19 x32 vec=21 x7 vec=22 x2
v86c dbg: RSPLOW total=0 kept=0 ring=6 insns=0 (oldest first; ...)
v86c dbg:   RSPLOW no event: rsp never decreased from one instruction to the next
```

and MEASURED that the named-stop path does not double-dump (`STOP (` lines counted = 1):

```
$ V86C_INTS=3 V86C_RSPLOW=4 ./build/v86c-nasm64 build/nasm64/alu-add64-carry-chain.img 20 --fixture64
v86c dbg: STOP (EFER.LME asks for IA-32e, and cpu backend 'uopjit' cannot run long mode ...)
v86c dbg: INTS total=0 ... v86c dbg: RSPLOW total=0 ...        STOP lines: 1
```

MEASURED that the tools stay silent when off: `./build/v86c tests/test.img 2000` with no V86C_*
variable prints 0 lines matching `v86c dbg`.

**What this exposes, and it is a finding rather than a fix**: on that kernel run RSPLOW reports
`insns=0` -- its tick lives in the **interpreted** per-instruction seat (cpu.c), and a run served by
the uop cache/JIT never reaches it. The dump now speaks ("no event", with `insns=0` telling a
reader the seat was never ticked) instead of being silent, but the ring is blind on any engine but
the interpreter. Named here; not this unit's to fix.

---

## 4. HOLE 4 (review m2) -- the peek could deliver a #GP on a non-canonical address

`do_page_walk64`'s canonicality check called `trigger_gp(0)` unconditionally while every
`trigger_pagefault` in the same function is `if (side_effects)`. So
`translate_address_read_no_side_effects` at a non-canonical address **delivered a real #GP** -- the
same perturbation class `484f04f` fixed for #PF.

**Fixed at the walk**, which is the root cause and covers every caller:
`if (side_effects) trigger_gp(0);` (src/cpu/paging.c). The abort channel (`cpu->pagefault = true`)
is still set, so the instruction does not act on the 0. Every other `trigger_*` call in
`do_page_walk64` was checked: all `trigger_pagefault` sites were already guarded; the
`WALK64_TABLE`/`WALK64_BOUND` macros call `unimplemented_instruction`, which is not a delivery at all
(see below).

**Plus a guard in the peek itself**, because the walk's named stops are the other way a diagnostic
could stop the machine: `dump_peek_sp` now refuses a non-canonical rsp outright ("not walked"), and
sets `v86c_dbg_no_stop` for the duration of its walk -- `unimplemented_instruction` returns
immediately while it is set (src/cpu/cpu.c). The peek still prints "not walked: the walk refused at
+i" because the walk sets `cpu->pagefault`. This also removes a latent recursion: the peek runs from
a break hit, and a walk stop inside it would have re-entered `v86c_dbg_on_stop`.

Which I chose and why: **both, at the walk for the delivery and in the peek for the stops.** The
walk fix is the root cause for the #GP; the peek guard is needed anyway because the named stops are
`unimplemented_instruction` calls that `side_effects` does not gate, and "a diagnostic must not be
able to stop the machine" is not satisfiable by fixing only the delivery.

---

## 5. HOLE 5 (review M3) -- the arms did not cover their seats

Two new arms in src/cpu/m1c_selftest.c (TEST 1n), registered in the suite's own list:

* `test_debug_watch_long_mode_seat` -- builds the same 4-level identity fixture the other long-mode
  arms build, then stores through **`safe_write64_addr` / `safe_write16_addr`** (the functions every
  64-bit store body and the generated overlay call). The watch is on the **virtual** address, so the
  physical router's hook underneath cannot stand in for it. Asserts the bytes landed AND
  `test_counts() == base + 1`.
* `test_debug_watch_straddle_reports_once` -- a qword store at linear 0xFFC (4 bytes in page 0, 4 in
  page 1), entry on the **second frame** (0x1000): asserts both halves landed, one line, and the
  guard back at 0. Then the same store watched by its **linear** address (one line), a straddling
  16-bit store (one line, second frame matches), and the wrap case below.

Both were run against three rebuilt mutants -- each mutant is a source edit, a rebuild of
`build/cpu_m1c_selftest`, and a run:

| mutant | what it does | measured result |
|---|---|---|
| **A: hook deleted** | the `v86c_dbg_watch_vp` call removed from `safe_write64_addr`'s non-straddle arm | `base + 1` -> `got 0x5, want 0x4`; suite rc **1** |
| **B: double report restored** | the one-store guard removed from the same arm | `base + 1` -> `got 0x6, want 0x4`, and the log shows `WATCH V -> P` + `WATCH P 0x3000` + `WATCH P 0x3004` = 3 lines for one store; suite rc **1** |
| **C: pre-unit straddle arm** | `safe_write64_addr`'s straddle body restored verbatim from HEAD | 2 `WATCH P` lines for one store, no `V -> P` line; suite rc **1** -- and the source of ss1's correction |

The good build: `cpu_m1c_selftest: 438 checks, 0 failures` (MEASURED, rc 0).

**A stale-artifact warning worth carrying**: `make` (the default target) does **not** rebuild the
selftest binaries. During mutant C a `make` left `build/cpu_m1c_selftest` from the *previous* source
and the run passed a source that could not compile -- the exact class the reviewer named for
`build/unity.o`. The mutants were therefore re-run with `make build/cpu_m1c_selftest`, and every
edited source was `touch`ed before `make` (see ss8).

---

## 6. HOLE 6 -- the six small ones

(i) **INTS `cr2=` label.** `cr2` is the faulting address only for a **walk-raised** #PF. A guest
`int $0x0e` has the same vector and no walk, so it printed a stale cr2 under a non-stale label. The
vec==14 rule is unchanged; the **label** is now source-aware: `(vector == 14 && !is_int) ? " cr2=" :
" cr2(stale)="` -- `is_int` is the tell (a hardware/exception fault arrives with `is_int` false and
`trigger_pagefault` has set cr2; an `int $0x0e` arrives with `is_int` true).

(ii) **RSPLOW gloss.** A line is a **net decrease over one instruction slot**, which can be a push,
a `sub rsp`, or an exception delivery whose frame push happens inside the slot (delivery runs
inside `run_instruction`, so the per-instruction tick sees post-delivery rsp). The comment in
`dump_rsplow` and the `V86C_RSPLOW` block in debug.h now say that, and say the rip names the
instruction the decrease is **attributable to**, not necessarily one that stored.

(iii) **A watch entry whose `addr + len` wraps.** Clamped at parse time with a printed line
(`v86c dbg: WATCH 0x...:N wraps the address space: clamped to 0x...`), **and** every matcher now
uses a saturating `watch_end()`, so an entry set any other way (the selftest sets one directly) is
still matchable rather than silently unfireable. Arm: entry at `0xFFFFFFFFFFFFFFFC` len 8 matches a
store at that address (MEASURED, `base + 4`).

(iv) **The 32-bit JIT blind spot**, stated in the header's `V86C_WATCH` block: the JIT writes RAM
through `jitcg_emit_mem_store`, which has no hook, so a 32-bit-only measurement of `V86C_WATCH`
under the default engine (`jit_enabled`, jit_mem.c) does not see TLB-hit RAM stores. One sentence,
no code change. Long mode is unaffected (uop_cache.c delegates to `interpreted_run`).

Also corrected while in the header: `watch_vp`'s comment claimed a long-mode physical watch "saw
NOTHING from any 64-bit store" (the review's M1 refutation) -- it now describes what the seat adds
(both addresses, once) without the false history.

---

## 7. Confirmed but NOT fixed (named, per the brief)

* **`tools/kallsyms.py` names confidently outside kernel text** (review M5 + m1). Not touched: not
  this unit's scope, and the review's own measurement stands.
* **The 32-bit `safe_write*` straddle arms and `safe_write64`'s non-straddle arm** still print more
  than one line per store (ss2's residual), and **a 16-byte store prints two lines**. Named in the
  header's counting rule.
* **RSPLOW's seat is the interpreted loop only** (ss3) -- a uop/JIT-served run records no event.
* **`WALK64_TABLE`/`WALK64_BOUND` are named stops reachable from a canonical address during a
  peek**; the peek no longer lets them stop the machine, but the walk itself is unchanged (S11's).

---

## 8. Gates (each by EXIT CODE, once, at the end)

Every edited source was `touch`ed first (`src/cpu/{debug.c,debug.h,paging.c,cpu.c,m1c_selftest.c}`,
`src/main.c`), because a stale `build/unity.o` can silently skip a real rebuild -- and ss5 records
the same trap one level down (the default `make` target does not build the selftest binaries).

| gate | command | result |
|---|---|---|
| build | `make` | **rc 0** |
| selftest | `make selftest` | **rc 0** (`cpu_selftest 3888/0`, `cpu_m1c_selftest 438/0`, `hw_selftest 975/0`, `uop_cache 7934363/0`, `uop_exec 102/0`, `jit 68/0`, `jit_codegen 3570/0`) |
| nasm64 | `make nasm64` | **rc 0** -- `gate-nasm64: 13 passed, 0 failed` |
| transcript | `make transcript-diff` | **rc 0** -- byte-identical to the reference |
| bench | `make bench` | **rc 0** -- 7/7 runs `instr=200095214` (pin held), state/serial/jit-inline all ok |
| x86-64 diff | `sh tests/gate-x86-64-diff.sh` | **rc 0** -- `gate-x86-64-diff: 13 passed, 0 failed` |

No `-DNDEBUG`. The amd64 guest was **not** run (the orchestrator verifies the stop-path fix there).

---

## 9. Files touched

* `src/cpu/debug.h` -- the counting rule + the JIT blind spot in the `V86C_WATCH` block, the
  terminal-stop paragraph, the RSPLOW gloss, `v86c_dbg_watch_quiet`, `v86c_dbg_no_stop`,
  `v86c_dbg_watch_vp_split`, `v86c_dbg_test_watch_set`, `v86c_dbg_watch_quiet_depth`.
* `src/cpu/debug.c` -- saturating `watch_end`/`watch_contains`, `watch_hit` with two frames, the
  guard in every reporting entry point, `vp_split`, the parser clamp, the peek's canonicality
  refusal and `v86c_dbg_no_stop` window, the source-aware `cr2` label, the RSPLOW gloss.
* `src/cpu/paging.c` -- the long-mode store family reports once (`watch_quiet_raise/lower`), the
  straddle arms report once against both frames, `safe_write32_addr_local` documented as hook-free,
  and `if (side_effects) trigger_gp(0)` in `do_page_walk64`.
* `src/cpu/cpu.c` -- `unimplemented_instruction` refuses to record a stop while a diagnostic walks.
* `src/main.c` -- `run()`'s one terminal stop dump with its reason.
* `src/cpu/m1c_selftest.c` -- TEST 1n's two arms (+ the shared long-mode fixture helper) and their
  registration.
