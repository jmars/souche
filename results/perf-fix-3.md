# PERF-FIX-3 — the invalidation BLOCKER (the unhooked RMW macros) and the two should-fixes

HANDOFF=v86c-perf, unit **PERF-FIX-3** (coder).  Authority: `results/perf-2r-review.md` (the
M2 review) and the fixes it parked on `handoff-v86c-perf-fix-2/3/4`.  Files this unit owns:
`src/cpu/paging.{c,h}`, `src/cpu/memory.c`, `src/cpu/uop_cache.{c,h}`,
`src/cpu/uop_exec_selftest.c`, `results/perf-4-recon.sh`.  **`src/cpu/cpu.c` and
`tests/perf_bench.sh` were NOT touched** (the concurrent PERF-FIX-2 unit owns them; `cpu.c` is
still at the M2 hash `8b46c959…` in this tree).  `v86/` is read-only and was only read.

## 0. Verdict

| item | status |
|---|---|
| **BLOCKER-1** — the 165 `SAFE_READ_WRITE8/16/32` RMW sites wrote guest RAM with no invalidation and no SMC abort | **FIXED**, and the class is now UNDER A GATE that fails on the pre-fix tree (`results/perf-4-recon.sh` exits 1 — MEASURED, see G2) |
| **SHOULD-FIX-1** — `write16`/`write32` hooked only the first page; the "cannot straddle" comment was false | **FIXED** (both widths), pinned by a two-arm straddle probe |
| **SHOULD-FIX-2b** — a materialise fault returned `true`, so a faulted FLUSH-point instruction flushed a batch the reference does not end | **FIXED** (returns `false`), pinned by a flush-count probe |
| **SHOULD-FIX-2a** — "materialise BEFORE the guard" | **REFUTED ON CONTACT WITH THE CODE** — the prescription is backwards: in the reference the guard runs FIRST.  Not applied; the tree was already right.  MEASURED: applying it makes the new guard-order pin deliver **#GP (13)** where the reference delivers **#NM (7)** (§2). |

No git commits (per the brief).  The tree is left byte-identical to the built state (hashes in
§5) with `make`, `make selftest`, `make nasm` (both cache modes) and `make transcript-diff`
green.

## 1. What changed

### BLOCKER-1 — `src/cpu/paging.h`

`#include "uop_cache.h"` (the macros are the callers now), plus `uop_invalidate_page_fast(phys_addr_)`
in the non-mmap arm of each of the three macros — upstream's dropped
`jit::jit_dirty_page` (`v86/src/rust/cpu/cpu.rs:4062/4082/4104`), restored at the site the M2
reconciliation could not see:

    paging.h:167  uop_invalidate_page_fast(phys_addr_);   /* SAFE_READ_WRITE8  */
    paging.h:191  uop_invalidate_page_fast(phys_addr_);   /* SAFE_READ_WRITE16 */
    paging.h:217  uop_invalidate_page_fast(phys_addr_);   /* SAFE_READ_WRITE32 */

The 165 call sites (`instructions.c` 151 + `instructions_0f.c` 14 — every `add/sub/and/or/xor/
inc/dec/not/neg/rol/…/bt*/xadd/cmpxchg [mem]`) need no edit: the hook is in the macro.  The
straddle arms need nothing (they route through `virt_boundary_write16/32` → the hooked
`write8`).  A doc block on the macro family now says why the hook is there and why the class
was invisible to the old gate.

### SHOULD-FIX-1 — `src/cpu/memory.c`

`write16`/`write32`'s non-mmap arms hook the LAST byte's page when the write crosses one
(`(addr & 0xFFF) == 0xFFF` → `fast(addr + 1)`; `>= 0xFFD` → `fast(addr + 3)`), and both
comments now say the truth: the routers have no boundary split, so a straddling write DOES
reach the next physical page; only `safe_write16/32` and the `SAFE_READ_WRITE16/32` macros
route their straddles through `virt_boundary_*`.  The live callers of the plain routers are the
virtio used-ring writes (`src/hw/virtio.c:1062/1068/1069/1074`) at guest-chosen addresses.

### SHOULD-FIX-2 — `src/cpu/uop_cache.c` (the half that stands)

`uop_exec`'s materialise-fault return is `false`, not `true`.  The generated body's resolve
check is a `return` **before** its postfix (`gen/interpreter_c.js:261-273`), so a FLUSH-flagged
encoding that faults at materialisation is NOT a reference batch end; returning `true` made the
caller (`uop_cache.c:1216`) call `uop_flush()` for it.  The guard stays FIRST (§2); the
function's contract comment now states both halves and cites the artifact.

## 2. SHOULD-FIX-2a is refuted — the guard runs first in the reference

The review's claim was "the generated dispatcher resolves FIRST and guards inside the body
(`gen/interpreter_c.js:261-273` … `:200-203`)".  The artifact says the opposite, at every
level:

* **upstream's own generator** (`v86/gen/generate_interpreter.js`): the guard is the encoding's
  `instruction_prefix` (`:233-251`), and the mem call whose argument evaluates `modrm_resolve`
  is emitted AFTER it (`:315-330`, `mem_args = ["match modrm_resolve(modrm_byte) { … }"]`).
* **the port's generated interpreter** (`src/cpu/interpreter_gen.c`), in every guarded case:
  `:12010-12017` (`fxsave`, 0F AE /0), `:4994-5011` (the FPU D8 group) and the other 200-odd —
  `if (!task_switch_test()) return;` precedes `int32_t addr = modrm_resolve(modrm_byte);`.
  The port's own comment says it too (`gen/interpreter_c.js:396`: "an sse encoding's
  `task_switch_test_mmx()` guard still runs first").
* **the plan** (`results/perf-2-blockcache-plan.md` §4.4): "the uop runs the guard at EXECUTE
  time and **materializes only if it passes**".

So the reorder would INTRODUCE the divergence it was written to remove.  MEASURED, with the new
guard-order pin: CR0.TS set **and** DS null (so a materialisation would raise #GP), instruction
`fadd dword [0x100]` (an FPU mem form whose descriptor carries `UOP_DESC_TS_GUARD`,
`src/cpu/uop_gen.c:541`), the delivered vector read out of a per-vector IDT stub:

    tree as built   (guard first):  vector cache-on 7 / cache-off 7     <- #NM, matches
    arm 4           (resolve first): vector cache-on 13 / cache-off 7   <- #GP, diverges

An earlier attempt at this pin is also worth recording as a measurement: with the first
(stub-based) version, the handler wrote through the DEFAULT segment while DS was null, so the
handler itself kept faulting; the frames walked down from 0x7FFF4 into the IDT at 0x6000 and
the delivery read a frame word as a gate descriptor ("invalid gate type").  The stubs now use
an SS override and the mechanism is documented in the file.

**Stated honestly**: the swap moves the materialisation only for a guard-FAILED instruction
whose segment would fault — a cold path.  But the code as built was already the reference's, so
the correct action was to change nothing and say so, with the falsifying probe attached.

## 3. The gates

Raw evidence: `results/perf-fix-3/mut-rig.txt` (the asymmetry rig),
`results/perf-fix-3/reconciliation.txt` (G2), `results/perf-fix-3/nasm-{on,off}.txt`,
`results/perf-fix-3/boot-*.txt`, `results/perf-fix-3/selftest-final.txt`.

### G1 — the RMW class is hooked AND PROVED (fail before, pass after)

New selftest arm `test_smc_rmw` (`src/cpu/uop_exec_selftest.c`): the patch instruction is the
RMW form `xor byte [0x100B], 0x2A` (`80 35 0B 10 00 00 2A` → `instr_80_6_mem` →
`SAFE_READ_WRITE8`), patching the immediate of a following `mov eax, 0` **in the same block**
(`instr_80_6_mem` is neither a block end nor a flush point: `uop_gen.c:274`, flags 0x45,
iflags 0x00).  The existing SMC test could not see the class — its patch is a plain `0xC7` mov,
which goes through the hooked `write32`.

    FIXED tree (cache on):  smc-rmw: xor byte [mem] patch executed, aborts 1, invalidations 1
                            (ram32(0x3000) == 0x2A; cache-off agrees == the reference's own answer)
    PRE-FIX tree (the rig's arm 1 = the three hooks removed from paging.h):
      FAIL …:324: ram32(0x3000) == 0x2A (got 0x0, want 0x2a)
      FAIL …:326: uop_stats.aborts >= 1
      FAIL …:327: uop_stats.invalidations >= 1
      smc-rmw: xor byte [mem] patch executed, aborts 0, invalidations 0
      selftest rc 1 (3 failures)

`results/perf-fix-3-mut-rig.py` is the rig (one inverse mutation per arm, rebuilt from source,
verdict from the exit code, tree restored afterwards): **5 arms, 5 caught, 0 holes**, control
green.  Its arms, with the pin each one trips:

| arm | mutation | pin (all CAUGHT) |
|---|---|---|
| 1 | remove the `SAFE_READ_WRITE*` hooks | `smc-rmw` (3 failures) **and the reconciliation refuses the tree (rc 1)** |
| 2 | remove the `write16` straddle hook | `straddle-w16` (2 failures) |
| 3 | remove the `write32` straddle hook | `straddle-w32` (2 failures) |
| 4 | materialise before the guard (= the review's 2a) | `guard-order` — delivered 13, not 7 |
| 5 | `return true` on a materialise fault | `faulted-flush` — 5 batch ends, not 4 |

### G2 — the extended reconciliation, and what it still cannot see

`sh results/perf-4-recon.sh` → `results/perf-fix-3/reconciliation.txt`, **exit 0**:

    == C. THE CLASS CHECK (PERF-FIX-3/BLOCKER-1): every writer is hooked or exempt ==
       checked 123 hits: 19 hooked, 104 exempt-classified, 0 FAIL
       EXEMPT-CLASS EXEMPT-DECLARATION 7 · EXEMPT-DEFINITION 13 · EXEMPT-LOADER-TIME 6 ·
       EXEMPT-SELFTEST 78 · OK 19
    --- the class check's exit code: 0  (0 = every writer hooked or exempt)

Two changes, because the old gate was satisfied by a tree with a whole unhooked family in it:

1. **list B now scans `src/cpu/paging.h` and matches the `SAFE_READ_WRITE8/16/32` invocations** —
   the shape that beat the old gate (a macro invocation is not a `write*()` call).
2. **section C is mechanical and has teeth**: `results/perf-fix-3-recon-check.py` walks every
   `.c`/`.h` under `src/`, finds every writer (`*_no_mmap_or_dirty_check` call, direct
   `mem8[..] =`, `memcpy/memset/memmove` into `mem8`), and requires each to be EXEMPT for a
   printed, stated reason (a writer's own definition; a header prototype; the LOADER-TIME class;
   a selftest; the VGA LFB) **or to have a `uop_invalidate_*` within the 8 preceding lines**.
   It EXITS NONZERO otherwise — the rig's arm 1 proves that (recon rc 1 on the pre-fix tree).

**Shapes I checked for, and the measurement that settles each** (section E of the script):
function-like **macros** (the failure mode: `paging.h`'s three are now in list B *and* their
bodies are checked; `rg '#define'` over `src/cpu/*.h` finds no other macro containing a writer);
**static inline functions** in headers (none writes guest RAM — `memory.h`'s only inline is the
`in_mapped_range` predicate); **aliases** (`rg '#define\s+\w+\s+(write|mem)'` → empty);
**function pointers** (the tables are `src/hw/io.c`'s `io_mmap` DEVICE hooks and the CPU's
`memory_devices` hooks; every device path that writes guest RAM calls the CPU's hooked
`write_blob`/`write16`/`write32` — `dma.c:240`, `ide.c:829`, `virtio.c:1409`, and the DMA read
side only *reads* mem8, `dma.c:292`); **pointer aliases** (`&mem8[` → empty; the only pointer
handed out into guest RAM is `src/hw/virtio.c:69 virtio_guest_read_blob`, typed `const uint8_t *`
and used only as a memcpy SOURCE).

**BLIND SPOTS, stated so a reader can attack them**: a writer reached ONLY through a function
pointer (a new device hook that stores into `mem8` directly) is in no list — the device
inventory is manual; and a pointer alias into guest RAM stored through after the call site is
invisible to a textual scan.  The check is textual; it cannot see shapes nobody has invented
yet, which is exactly the failure mode it was written to reduce — the mitigation is that any
NEW writer shape shows up as a FAIL until someone classifies it.

### G3 — the straddling write: the second page's blocks ARE invalidated

`test_straddle_write` (`straddle_case(false|true)`): a block is cached on page 0x1000
(`nop; mov [0x3000],eax; hlt`), then a device-style router write crosses into it —
`write16(0xFFF, 0xB800)` / `write32(0xFFD, 0xB8000000)` turns the block's first byte into
`0xB8` (`mov eax, imm32`).  Run 2 re-enters with a different `eax`: with the fix the page's block
was dropped, the bytes are re-decoded and `mov [0x3000], eax` is gone.

    FIXED:   straddle-w16: write16(0xFFF) invalidated the next page (0x3000 stayed 0x11)
             straddle-w32: write32(0xFFD) invalidated the next page (0x3000 stayed 0x11)
             (+ `uop_stats.invalidations - before == 1` asserted in each case)
    PRE-FIX (rig arm 2 / arm 3): the invalidation delta is 0 and 0x3000 became 0x22 — the stale
             block re-ran.  Two arms because each width has its own hook.

### G4 — the fault/guard contract (both facets)

    (a) guard ORDER:  test_guard_order — cache-on 7 / cache-off 7 (#NM); the review's reorder
        (rig arm 4) gives 13 (#GP) against the reference's 7.  See §2.
    (b) fault RETURN: test_faulted_flush — `cmpxchg byte [0x100], al` (a mem form whose
        descriptor carries UOP_DESC_FLUSH and NOT BLOCK_END) faults at materialisation (DS null)
        with a same-page #GP handler that repairs DS, pops the error code and irets; the guest
        observes the counter with `rdtsc`.
            FIXED:     faulted-flush: cache-on batch ends 4 (must be 4), guest rdtsc 6000/6999
            PRE-FIX (rig arm 5): cache-on batch ends 5, guest rdtsc 6000/6999
        The four batch ends are derived from the reference's own table, each named in the test:
        the handler's `mov ds, ax` (mov-sreg is a block boundary), its `iret`, the cmpxchg's own
        `UOP_DESC_FLUSH` when it executes, and the final `hlt` — and NOT a fifth for the faulted
        instance.

**A RESIDUAL OF THE SAME CLASS, recorded and NOT fixed (found while checking 2b's reach).**
The flush gate reads `ran`, and `ran` is still `true` when the fault is raised *inside the
instruction BODY* rather than at materialisation — e.g. a write whose translation faults under
paging (`SAFE_READ_WRITE32`'s `if (cpu->pagefault) return;` returns from the INSTRUCTION body,
so the reference's postfix is skipped there too), on a FLUSH-flagged encoding.  The reference
does not end the batch; this executor still flushes.  It is pre-existing (untouched by the 2b
change, which only moves the materialise-fault return) and it is the same bookkeeping class as
2b with the same visibility limit (the fault's handler runs inside the body, so the flush still
lands after it).  NOT fixed here because the honest fix is not the naive
`if (cpu->pagefault) return false;` after the switch: `cpu->pagefault` also stays set for a
body that raised a fault via a helper and then reached its END, where the reference's postfix
DOES run — so "no fault" and "the postfix ran" are not the same predicate from `uop_exec`'s
position, and separating them needs a probe per shape (all of them under paging, since a
write-fault needs a real page table).  Flagged for the reviewer with that reasoning rather than
guessed at here.

**STATED LIMIT (measured, not assumed) for (b)**: I could not construct a GUEST-VISIBLE
discrimination for this defect.  The spurious flush lands after the handler has already run
(`uop_exec` returns only then) and the handler's `iret` is itself a batch end, so both builds
flush the same total at the same later point — the guest's `rdtsc` read is identical with and
without the fix (6000/6999 → 6/6 in both arms; asserted).  What the fix changes is the
executor's batch bookkeeping (`uop_stats.flushes`, and through `uop_run`'s early return the
budget window).  The reviewer's stated mechanism — "a same-page handler's rdtsc reads a
different staircase" — is NOT reproduced, because the read happens before the flush in both
builds.  The fix is still the right direction (it removes a boundary the reference does not
have, so it can only make the executor more faithful), but a reader should know the pin that
proves it is a bookkeeping counter, not the guest's clock.

### G5 — no regression

| gate | result |
|---|---|
| `make` | rc 0 |
| `make selftest` | rc 0 — `cpu_selftest` 3522, `cpu_m1c_selftest` 228, `hw_selftest` 541 (0 skipped), `uop_cache_selftest` 7934349, `uop_exec_selftest` **66** (was 48; the four new pins), all 0 failures; `uop-unity` green (`results/perf-fix-3/selftest-final.txt`) |
| `make nasm` (cache ON) | rc 0, **Passed 118/118**, 0 of 123 produced no state, 5 skipped (`results/perf-fix-3/nasm-on.txt`) |
| `make nasm` (cache OFF) | rc 0, **Passed 118/118**, same skips (`results/perf-fix-3/nasm-off.txt`) |
| `make transcript-diff` | rc 0, "the C transcript is byte-identical to the reference's" |
| 1.5 G boot, cache on vs off | **final state IDENTICAL**: `1500053676 instructions, in_hlt=0, exception=-1, unimplemented=0, eip=0xc1927800 cs=0x60 cr0=0x80050033 cr3=0x1ea1000` (the instruction COUNT differs, 1500053676 vs 1500025082 — see the note below) |
| `make bench` (7 runs, cache ON, shipped default) | **best 3.166 s / 63.20 MIPS, median 3.185 / 62.82** vs the brief's baseline 3.380 s / 59.2 MIPS — every run 200,095,214 instructions, terminal state and serial sha256 pinned and `ok` |

**The 1.5 G serial transcripts need a caveat, and it is measured.**  `cmp` reports the two
serial files differ (byte 8484): the differing lines are the kernel's `[  <ms>]` timestamps and
one `audit(<wall-second>` — i.e. guest reads of the HOST clock (`rtc.c`'s CLOCK_REALTIME),
exactly the host dependence `tests/perf_bench.sh` now documents.  Two runs of the SAME cache
mode differ in the SAME way and MORE (286 raw differing lines off-vs-off2 vs 256 on-vs-off);
after normalising the timestamp prefix and the audit second, **both comparisons differ by
exactly 12 lines**, all of them guest-measured durations (`hrtimer: interrupt took N ns`, `pci …
quirk … took N usecs`).  So cache-on vs cache-off is inside this boot's run-to-run noise, and
the pinned comparator that IS stable — the terminal guest state — is identical.  (The
instruction count cannot be a comparator at a limit crossing: `perf_bench.sh`'s own header
explains that the count is the batch boundary where the limit was crossed and moves with the
guest's timing loops.)

**WHAT THE FIXES COST**: nothing measurable.  3 runs with the hooks and 3 without (the same
mutation the rig's arm 1 applies, tree restored afterwards and its hash re-verified):

    hooks PRESENT: best 3.172  median 3.182   (62.69 / 62.55 MIPS)
    hooks REMOVED: best 3.216  median 3.225   (61.85 / 61.55 MIPS)

i.e. the ordering is opposite to a cost and inside the run-to-run spread: at 3 runs the three
hooks are below this workload's resolution.  (The mechanism says why: the hook is one load from
a per-page byte array that is zero for every data page, plus a predictable branch — the safe
direction of upstream's dropped `can_skip_dirty_page` optimisation.)

## 4. Files this unit touched

| file | change |
|---|---|
| `src/cpu/paging.h` | `#include "uop_cache.h"`; the three hooks; the macro-family doc block |
| `src/cpu/memory.c` | the two straddle hooks; both comments corrected |
| `src/cpu/uop_cache.c` | `uop_exec`: fault returns `false`; the contract comment (order + return), citing the artifact that refutes 2a |
| `src/cpu/uop_exec_selftest.c` | four new pins (`test_smc_rmw`, `straddle_case`×2, `test_guard_order`, `test_faulted_flush`), the IDT-stub machinery, and the recorded measurement of the delivery-loop trap the first version hit |
| `results/perf-4-recon.sh` | list B scans `paging.h` + `SAFE_READ_WRITE*`; section C (the mechanical class check); section E (the shapes and the blind spots); exits with the check's code |
| `results/perf-fix-3-recon-check.py` | **new** — the class check itself |
| `results/perf-fix-3-mut-rig.py` | **new** — the asymmetry rig (5 arms) |
| `results/perf-fix-3.md`, `results/perf-fix-3/` | this report and the raw evidence |

## 5. Hashes (sha256) at the end of the unit

    src/cpu/paging.h                78e1ed03fda8e45cf282755c7158289d61b2f9ae2e173c6877b5494a627da457
    src/cpu/memory.c                5ed5786fb9f2d211b6620c0d7d81efadc6bfb8846b6df56422647de65da28d76
    src/cpu/uop_cache.c             602f887d1c6045ccca718af81162975fc8bebc1e687245612ac3a34ab8c4049c
    src/cpu/uop_exec_selftest.c     5db14ae987fcd4c0eeaf1801de4d8cccb623241ac5b9c915492beae5f9c22cea
    results/perf-4-recon.sh         5321ae915797e3c02ccc20ebf40da5f4397502ae7a4a50c887435d7af4e61439
    src/cpu/cpu.c                   8b46c9597f756221ed0b385a8fe51576a348b0ff983a27856b218765898043fc  (untouched)

The pre-fix hashes (for a reader who wants to reproduce the asymmetry):
`paging.h 08e27bb7c884cb08e1c7c1de824eb4248af386707696217cf753819db4cc0a97`,
`memory.c 1439a48c98619cfae7e988e5fe503feaca73acb9082da9b93dab23abb27ab8bc`,
`uop_cache.c 886d9334f4896fc9be5b883450134e4e0879d56a764a5220d2b27d9fb9f63db2`,
`uop_exec_selftest.c 4b0a5b14e87a41b250c91235404e26c7774fc4a5c602d0457edc14b1e97619e7`.

## 6. What a reviewer should attack

1. **The reconciliation's teeth.**  Section C is textual: it knows the writer shapes listed in
   §3/G2 and nothing else.  The right attack is to *add* a writer in a shape not on the list
   (a function pointer, an alias, a `mem8 + n` parameter) and check the gate FAILS.  It should.
2. **The 2b pin is a counter, not the guest.**  If a reviewer can build a guest-visible
   discrimination for the faulted-flush case, that is a new finding — I could not, and I have
   recorded why (§3/G4) rather than claiming the guest sees it.
3. **The 2a refutation** rests on the generator's emission order (`v86/gen/generate_interpreter.js`
   :233-251/:315-330) and the port's generated interpreter.  Read those two, not this report.
4. **The RMW pin's block shape.**  `test_smc_rmw` asserts `aborts >= 1`, i.e. the patch write
   happened inside the executing block.  That depends on `instr_80_6_mem` being neither a block
   end nor a flush point (`uop_gen.c:274`); if a future emitter change makes it either, the pin
   weakens (the store would still be checked, the abort half would not) — the assertion would
   then need the same attention.
5. **The body-fault residual** (§3/G4): the same flush-gate class, one level deeper.  It is the
   natural next item for whoever owns the staircase next; it needs a paging fixture.
