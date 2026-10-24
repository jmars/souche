# x86-64: `V86C_WATCH` could not see the PHYSICAL frame a long-mode store went to

**Status: INSTRUMENT FIXED, gated, four gates green, no pin moved.** Claims tagged MEASURED / READ.
This is trap 1 of `results/x86-64-handoff.md` §7 ("an instrument's silence is not evidence unless the
instrument can speak") in its sixth instance; the next scheduled measurement (a physical watch on the
frame the walk resolves, `results/x86-64-peekfix.md`'s "ONE next measurement") was BLOCKED on it.

## 1. The defect (READ, `src/cpu/debug.c:110-133`, `src/cpu/paging.c:1424-1560`)

`watch_hit()` tested ONE address space per call: `v86c_dbg_watch_v()` reported the LINEAR address and
`v86c_dbg_watch_p()` the PHYSICAL one, both against the same `watch[]` entries. Every long-mode store
goes through `safe_write{8,16,32,64}_addr()`, and each of those called `v86c_dbg_watch_v()` with the
linear address **before** `translate_address_write()`. The physical frame the store actually landed
in — the local `p` those functions hold one line later — was never reported.

Consequence, and it is exactly the shape the peekfix note hit: an operator who sets `V86C_WATCH` on a
PHYSICAL range (the documented way to watch a frame, `V86C_WATCH=0xf802000:0x1000`) saw **nothing**
from any long-mode store. The 32-bit path was fine (`memory.c`'s `write*()` report physical addresses,
`paging.c`'s `safe_write*()` the virtual one); the amd64 guest runs in no other mode than the blind one.

## 2. The change

- `src/cpu/debug.h` / `src/cpu/debug.c`: new
  `void v86c_dbg_watch_vp(uint64_t va, uint32_t phys, int width, uint64_t value, uint64_t rip)`.
  It tests every armed entry against **va AND phys**, counts an entry **once** even when its range
  contains both, and prints one unbuffered line per hit carrying **both** addresses:
  `v86c dbg: WATCH V 0x<va> -> P 0x<phys> width=<n> value=0x<v> rip=0x<rip>`.
  `v86c_dbg_watch_v()` and `v86c_dbg_watch_p()` are byte-for-byte unchanged (the 32-bit paths still
  use them; no existing output line changed — the `WATCH V`/`WATCH P`/`WATCH R`/`WATCH F` formats are
  all still produced by the same code).
- `src/cpu/paging.c`: in `safe_write8_addr`, `safe_write16_addr`, `safe_write32_addr` and
  `safe_write64_addr`, **the non-straddle arms only**, the hook MOVED to after
  `translate_address_write()` and after the `if (cpu->pagefault) return;` guard, and now calls
  `v86c_dbg_watch_vp(addr, p, width, value, cpu->instruction_pointer)`. Rationale in one comment line
  at each site: the physical address only exists after translation, and a store that faults never
  stores. The straddle arms are untouched — they recurse into the per-half writers, which now hook
  themselves. `safe_write32_addr`'s non-straddle arm inlines what `safe_write32_addr_local` did on a
  branch unreachable from that call site (READ: `_local`'s extra `translate_address_write_and_can_
  skip_dirty` arm needs `addr & 0xFFF > 0xFFC`, which `safe_write32_addr` has already sent to the
  straddle arm); `_local` stays, still used by the 64-bit straddle.

**The deliberate behaviour change, stated: a long-mode store that FAULTS used to print a `V` line
(the hook was before the walk) and no longer prints anything.** That line was reporting a store that
never happened. `V86C_WATCH_F` is the tool for the faulting access and it still speaks
(`src/cpu/paging.c:150`).

## 3. The arm, and the asymmetry (MEASURED)

`src/cpu/m1c_selftest.c`, TEST 1h `test_debug_toolkit_watch_vp`, next to TEST 1g, using the file's
`machine_setup`/`v86c_dbg_test_arm()`/`CHECK_EQ`/`v86c_dbg_test_disarm()` style. The armed entry is
`watch[0] = 0x31558:1` (READ, `src/cpu/debug.c:30-45`):

| call | expected count |
| --- | --- |
| `vp(0xdead0000, 0x31558, ...)` — PHYSICAL only | 1 |
| `vp(0x31558, 0xdead0000, ...)` — VIRTUAL only | 2 |
| `vp(0xdead0000, 0xdead0000, ...)` — neither | 2 |
| `vp(0x31558, 0x31558, ...)` — both in one entry | 3 (not 4) |

**The arm can fail, proved by mutation, not asserted** (three mutants of `v86c_dbg_watch_vp`, each
rebuilt and run against the arm; the good file restored afterwards):

- MUTANT A, physical test removed (`in_p = false`) — the one-sided implementation this seat exists to
  replace: `FAIL m1c_selftest.c:632: counts() == 1 (got 0x0, want 0x1)`, plus :634/:636/:638, suite
  **rc=1**.
- MUTANT B, virtual test removed (`in_v = false`): `FAIL :634: counts() == 2 (got 0x1, want 0x2)`,
  plus :636/:638, suite **rc=1**.
- MUTANT C, a count per ADDRESS (`test_counts += in_v + in_p`): `FAIL :638: counts() == 3 (got 0x4,
  want 0x3)`, suite **rc=1** — the "once, not twice" half is load-bearing.

**It also speaks end to end (MEASURED, the synthetic 64-bit corpus, NOT the amd64 guest — the brief
reserves that run):** with `V86C_WATCH` armed, `build/v86c-nasm64 build/nasm64/alu-pop-rsp.img 20000000
--fixture64 --long-mode on` prints 19 `WATCH V 0x… -> P 0x…` lines (mov-imm64: 17), e.g.
`WATCH V 0x100000 -> P 0x100000 width=8 value=0x123456789abcdef rip=0x90056`. Before this change those
stores printed a `V` line with no physical address at all.

**Stated limit of that run:** the corpus stubs identity-map the first 4 MiB, so `va == phys` there and
the corpus cannot discriminate the two address spaces — a defect whose signature is "a PHYSICAL watch
sees nothing" is invisible under identity mapping. That is why the discrimination lives in the arm's
three mutants above, and why the defect was MEASURED (READ) on the guest's non-identity mappings
(`0xffffd1fac0000000 → 0xf802000`, `results/x86-64-peekfix.md`).

## 4. Gates (exit codes, one pass, at the end)

| gate | rc | evidence |
| --- | --- | --- |
| `make` | **0** | |
| `make selftest` | **0** | cpu 3888/0, m1c **274/0** (the arm adds 5 checks; 274-5=269 before — arithmetic, not a second build), hw 975/0, uop 7,934,363/0, uop-exec 102/0, jit 68/0, jit-codegen 3570/0, `all suites green` |
| `make bench` | **0** | pin **200,095,214 instructions**, unchanged (the hook is default-off) |
| `make transcript-diff` | **0** | `milestone: the C transcript is byte-identical to the reference's` |

Harness note, not attributed: one bench run's `time` output came back mangled (`real 0.:00s`) and
`tests/perf_bench.sh:206`'s float parse printed a `ValueError`; the target still exited 0 with the pin
verified in every run, and `tests/perf_bench.sh` is unmodified by this unit.

## 5. One sentence

**A `V86C_WATCH` set on a physical range now sees every long-mode store that lands in that frame,
with the linear address that produced it — and a watch set on a linear address still sees the store,
now with the frame it landed in.**
