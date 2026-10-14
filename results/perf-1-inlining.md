# PERF-1 — structural inlining, the cosmocc gap, and a benchmark to hold them to

**Unit:** `HANDOFF=v86c-perf`, unit PERF-1 (coder). **Deliverables:** a reproducible
benchmark, the measured cost of the two structural-inlining routes, the cosmocc-vs-glibc
20% gap located, and every existing gate still green with unchanged counts.

**Bottom line — 6.367 s → 3.932 s, i.e. 1.62x, with the guest-visible output unchanged.**
Two changes buy it, and they are independent and multiplicative: the optimizer flags
cosmocc's wrapper forces OFF (1.235x, a Makefile-only change) and the UNITY translation
unit (a further 1.259x on top, structural). **The 20% cosmocc-vs-glibc gap is NOT the
compiler version, NOT cosmopolitan's libc, and NOT the APE: it is
`-fno-optimize-sibling-calls`, one option cosmocc's wrapper injects into every compile.**
Undoing that one option takes the non-unity build to glibc parity (1.009x).

**Route (a)** — the six hottest accessors as `static inline` in their headers — was
measured as well: **1.194x on its own**, but it adds nothing on top of the unity build, so
it is reported and REVERTED (§10). Route (b) wins, and it is the only inlining mechanism
in the shipped tree.

All numbers below are MEASURED on this host in this session, best-of-N `real` seconds of
the workload in §1, unless a row says otherwise. Raw per-run samples:
`results/perf-1/samples.txt`. Gate run: `results/perf-1/gates.txt`.

**Read the block, not just the number.** The same binary configuration measured 3.932 s
(best) / 4.026 s (median) in the final, quiet block and 4.095 / 4.287 earlier — ~4%
block-to-block drift on this shared host. So the honest headline is **1.55x–1.62x**, and
every smaller claim in this document is stated against the block it was measured in.

---

## 1. The benchmark (`tests/perf_bench.sh`, `make bench`)

    tests/perf_bench.sh [--runs N] [--bin PATH] [--label TEXT] [--samples FILE]
    make bench                    # = the script, 7 runs, on build/v86c

**Workload — fixed by the brief, and the one the whole port already runs:** the Alpine
6.6.134 i386 kernel boot

    <bin> --bzimage tests/images/vmlinuz-i386 200000000 --cmdline "console=ttyS0"   (exit 0)

= **200,095,214 guest instructions** (the binary prints the count; the script READS it).

**Protocol** (the point of the thing — a number without one is not defensible):

* N runs, all timed; no warmup is discarded (the workload re-reads its kernel from page
  cache every time, and there is no JIT or per-run state to warm).
* `real` AND `user`+`sys` are sampled, and **best AND median** of each are reported. On
  this host a single best-of-N is the more stable statistic: within one block the spread
  of the 7 runs was 0.8%–5%, and the MEDIAN of the same binary drifted 4.8% between two
  blocks (4.159 best / 4.357 median in one, 4.095 / 4.287 in another). Every claim of
  "faster" below is a difference larger than that drift, and the flag-level differences
  that were NOT larger are called indistinguishable.
* The host's load average (before and after) and core count are printed, so a contended
  block is visible rather than silently averaged in.
* **The script refuses to report a number** when any run's own instruction count differs
  from the others' — mixing two workloads in one average is the one error that would make
  the whole table meaningless.
* The script takes `--bin`, which is what makes the attribution arms in §6 (the reverted
  build) and the cosmocc-vs-glibc comparison in §7 the same measurement as the headline.

## 2. Baseline (gate G1) — measured before any edit

| build | best real | median real | best user | MIPS @ best |
| --- | --- | --- | --- | --- |
| cosmocc (shipped toolchain, gcc 14.1.0) `-O2`, per-TU — **THE BASELINE** | **6.367 s** | 6.419 s | 6.319 s | 31.43 |
| host glibc/`cc` (gcc 16.2.0) `-O2`, per-TU, same source | 5.107 s | 5.163 s | 5.061 s | 39.18 |

**COSMOCC-VS-GLIBC GAP = 6.367/5.107 = 1.247x** (24.7% slower). The ctx node's orientation
figure (6.26 vs 5.20 = 1.204x) is reproduced on this host's numbers, from a different build
session, as expected.

The glibc leg could not be built at all before this unit: `src/main.c` used cosmocc-only
`ucontext` member names and leaned on cosmocc's implicit `<unistd.h>` for `_exit`. Two
guarded additions make the source buildable by the host compiler (§9); **both edits leave
`build/v86c` and `build/v86c.com.dbg` sha256 IDENTICAL**, i.e. they change nothing cosmocc
emits (and the two builds that produced those identical hashes rule out a per-link random
element in the comparison — see the caveat in §9).

## 2b. What a whole-file hash can and cannot prove (a trap worth recording)

The APE's shell preamble carries a `<<'justine…'` heredoc delimiter that varies with the
link (MEASURED: `justine7u1xo9` for one build, `justine92lxf1` for the next, 14 bytes of
file-size delta between them), so **`build/v86c`'s sha256 is not a pure code oracle**: a
symbol-NAME change moves it. Two code-level checks were therefore used where it mattered:
`objcopy --strip-all` of the two `build/v86c.com.dbg` files (identical), and the `.text`
disassembly of the two APEs (identical, 196,705 lines). The same content twice DOES give
the same hash — verified by rebuilding the shipped source twice: `ddf54be2…` both times —
which is what licenses the §9/section-2 neutrality claims.

## 3. The profile that chose the targets (`-pg`, CALL COUNTS only)

`gprof -pg` distorts self-times (~7x, as the ctx node warns) so **only the call counts are
used here**, and they are used in the thousands-of-millions range:

| function | calls / 200,095,214 instrs | | function | calls |
| --- | --- | --- | --- | --- |
| `read_imm8` | 337,311,116 | | `resolve_modrm32` | 83,079,225 |
| `read_reg32` | 212,599,497 | | `get_seg` | 57,517,370 |
| `run_instruction` | 206,543,063 | | `resolve_sib` | 57,173,208 |
| `get_phys_eip` | 200,095,213 | | `read32s` | 48,233,348 |
| `write_reg32` | 119,033,746 | | `safe_read32s` | 33,064,149 |

This is a **per-operand-CALL-bound interpreter**, not a memory-model-bound one: ~1.1G
cross-TU calls for 200M instructions, every one of them a call/return pair because cosmocc
cannot inline across translation units and has no LTO.

## 4. Route (b): the UNITY translation unit — THE STRUCTURAL WIN

`gen/unity_c.py` emits `build/unity.c` = one `#include` per CPU source (the Makefile's own
`$(CPU_SRC) $(GEN_C)` — the list is not hand-maintained), compiled as ONE translation unit
by `$(UNITY_OBJ)`, and that object is what `build/v86c` links. gcc then inlines intra-TU
WITHOUT LTO.

* **MEASURED, no flag changes: 6.367 s → 4.970 s = 1.281x.** The unity build under cosmocc
  is faster than the glibc per-TU build (5.107 s) — structural inlining buys more than the
  compiler change does.
* CPU-only, deliberately: folding `src/hw` in as well measured **4.962 s vs 4.970 s**, i.e.
  0.2%, indistinguishable — while costing the `src/hw` rule its `-Werror` (the inlined
  io.c port-map accesses trip `-Warray-bounds`) and putting 14 files' file-local statics in
  one namespace for no measured gain. The measured difference between "CPU-only unity" and
  "all-unity" with the final flags (4.159 vs 4.202 best, i.e. 1.0%) is inside the same
  noise band.
* **The hazard it guards, and the guard is tested.** Two file-scope statics collided for
  real: `ps2.c` and `pit.c` both define `js_to_int32`, and `kernel.c`'s `static write_blob`
  collides with `memory.c`'s exported `write_blob` (different signature — the compiler
  caught that one). Same-name-same-signature would bind SILENTLY, so `gen/unity_c.py`
  REFUSES to emit a unit whose inputs define the same file-scope static name, and
  `results/perf-1/guard-asymmetry.sh` proves the refusal **fires on the pre-fix pair and
  passes on the fixed one** (the asymmetry is the proof, not the fact that it passes now).

## 5. Route (c, forced by the evidence): the flags cosmocc injects

`cosmocc -v` (MEASURED) shows the wrapper invoking its gcc with a fixed prologue:

    -fno-semantic-interposition -fno-optimize-sibling-calls -mno-omit-leaf-frame-pointer
    -fno-schedule-insns2 -mno-tls-direct-seg-refs -fpatchable-function-entry=18,16
    -fno-inline-functions-called-once -fportcosmo -DFTRACE -fno-pie ...

User flags are appended AFTER that prologue and gcc's same-option rule is last-wins
(MEASURED with `cosmocc -v`), so the tree re-enables three of them from our side
(`$(PERF_CFLAGS)`). On a call-bound interpreter these are not tuning knobs:

| configuration (per-TU, pristine sources) | best real | vs 6.367 |
| --- | --- | --- |
| baseline (cosmocc defaults) | 6.367 s | 1.000x |
| **+ `-foptimize-sibling-calls` ALONE** | **5.154 s** | **1.235x** |
| + that and `-finline-functions-called-once` | 5.173 s | 1.231x |
| + that and `-fschedule-insns2` | 5.167 s | 1.232x |
| + that and `-momit-leaf-frame-pointer` | 5.279 s | 1.206x |
| + all four together | 5.479 s | 1.162x |

**Sibling-call optimization alone is the whole win, and adding the other three to it cancels
part of it** (5.154 → 5.479). They were therefore NOT shipped on the per-TU argument; the
unity argument is different, so they were re-measured there — and because those differences
are of the same size as the block-to-block drift, they were re-measured a THIRD time as an
INTERLEAVED block (three binaries, round-robin, four rounds, back to back), which is the
only form that controls for drift:

| configuration (CPU-only unity), interleaved block | best real | median real |
| --- | --- | --- |
| `-foptimize-sibling-calls` | 4.306 s | 4.337 s |
| + `-finline-functions-called-once` | 4.231 s (−1.7%) | 4.293 s (−1.0%) |
| + `-fpatchable-function-entry=0` | **3.985 s** (−5.8%) | **4.012 s** (−6.5%) |

* **`-fpatchable-function-entry=0` is real and worth ~6%** — cosmocc's 18-byte ftrace patch
  area is 2 executed NOPs per function entry, paid ~1.1G times, and the padding also feeds
  the inliner's size model. The cross-block estimate (+4.4%) and this interleaved one
  (−5.8%/−6.5%) agree in direction and rough size.
* **`-finline-functions-called-once` is only ~1–1.7%** — NOT the +3.2% the cross-block
  comparison first suggested (that block read 4.562 → 4.421 s). Both statistics agree on
  the sign and neither is negative in any block, so it is kept; but it is a 1% flag, and it
  is reported as one. Under unity it can only be inlining the instruction bodies the
  generated dispatcher calls exactly once, which is what the unity build makes visible at
  all — the same mechanism the per-TU build cannot use (there it measured 5.154 → 5.173 s,
  i.e. nothing).
* `-momit-leaf-frame-pointer` and `-fschedule-insns2` bought nothing in any block.

**Shipped `$(PERF_CFLAGS) := -foptimize-sibling-calls -finline-functions-called-once
-fpatchable-function-entry=0`** — every one of them measured, none of them guessed, and the
two neutral/harmful ones dropped.

## 6. Attribution (gate G4) — reverting just the structural change

Same sources, same flags, linked per-TU instead of as one unity TU:
`results/perf-1/revert-attribution.sh` (it seds a COPY of the shipped Makefile; the diff is
printed as the evidence of what "revert" means).

|  | per-TU link | unity TU |
| --- | --- | --- |
| cosmocc defaults | **6.367 s** (baseline) | 4.970 s |
| `$(PERF_CFLAGS)` | **5.155 s** | **4.095 s** |

* flags alone: 6.367 → 5.155 = **1.235x**
* unity alone: 6.367 → 4.970 = **1.281x**
* both: 6.367 → 4.095 = **1.555x**; and unity ON TOP of the flags = 5.155 → 4.095 =
  **1.259x** — the asymmetry that attributes the structural half to the inlining and not to
  the flags.

**FINAL (the shipped binary, `build/v86c`, 7 runs, quiet block): best 3.932 s, median
4.026 s, 50.89 MIPS** (baseline: 6.367 s, 31.43 MIPS). By median, 6.419 → 4.026 = 1.595x.
Two further blocks of the same binary agree: best 4.095 s / median 4.287 s in a busier one,
and `make bench` itself gives best 3.958 s / median 4.061 s — so every block lands in
1.55x–1.62x and the ~4% spread is the machine, not the change.

## 7. The cosmocc-vs-glibc gap: LOCATED

| build (same source) | best real |
| --- | --- |
| cosmocc per-TU, defaults | 6.367 s |
| **cosmocc COMPILED objects, linked against HOST GLIBC** | **6.438 s** |
| host glibc per-TU, same flags | 5.107 s |
| cosmocc per-TU + `-foptimize-sibling-calls` | 5.154 s |
| host glibc per-TU + `$(PERF_CFLAGS)` | 5.107 s (**1.009x** — parity) |
| cosmocc unity + `$(PERF_CFLAGS)` | 3.932–4.234 s |
| host glibc unity + `$(PERF_CFLAGS)`, built from the final sources by the same Makefile | 3.662 s |

1. **NOT cosmopolitan's libc, and NOT the APE.** Cosmocc-compiled objects (its gcc 14.1.0)
   linked with the HOST glibc run at 6.438 s — indistinguishable from the APE's 6.367 s.
   The gap is in the code `cc1` emits, and it survives replacing the entire libc and the
   APE startup path.
2. **NOT the libc implementations (memcpy/memset).** The port's own
   `memcpy_no_mmap_or_dirty_check` is called **1,885 times** in 200M instructions, and the
   SoftFloat amalgamation is called **once** (`fpu_finit`/`fstcw`/`fnstsw` at boot): neither
   can carry 20% of 6 s. (The mixed-link result above does not depend on this.)
3. **NOT `-Wl,--gc-sections`** — it was already on, in both legs.
4. **IT IS ONE OPTION: `-fno-optimize-sibling-calls`.** Re-enabling it alone takes the
   non-unity cosmocc build to 5.154 s against glibc's 5.107 s — **1.009x, parity**. A
   call/return pair per operand IS the cost, and cosmocc's wrapper forbids the jump that
   removes half of it. This is the unit's headline for the gap.
5. **A RESIDUAL REMAINS, AND ONLY UNDER THE UNITY BUILD: 3.932 s vs glibc's 3.662 s
   = 1.074x** (1.083x measured in the earlier block; a 4.095-vs-3.782 pair in the block
   before that gave 1.083x too — so ~7–11%, and it is measured like for like, both legs
   built by the same Makefile with the same flags). Stated as a finding, not smoothed
   over: the glibc leg benefits MORE from cross-TU inlining (5.107 → 3.662 = 1.394x) than
   cosmocc does (6.367 → 4.234 = 1.504x — but from a worse start; per-TU-with-flags the
   two are at parity, so the unity build WIDENS the relative gap). The axis this could
   not be separated on, with the tools on this host: **compiler version vs
   `-fportcosmo`**. Only gcc 16 exists here (no gcc-14 to hold the libc constant), and
   `-fno-portcosmo` is an unrecognized option, so cosmopolitan's own pass cannot be
   turned off. What WAS tested is inliner aggressiveness: **`-O3` is WORSE** (4.428 s vs
   4.159 s at `-O2`, same block), so "cosmocc's inliner is merely more conservative" is
   refuted — it is differently tuned, and the difference is worth ~7–11% under inlining
   and 0% without it.

## 8. Gates (G3) — all green, counts unchanged

Raw output with exit codes: `results/perf-1/gates.txt` (one from-scratch build, `make
clean && make -j16`).

| gate | result |
| --- | --- |
| `make` (from scratch, `make clean && make -j16`, unity TU) | exit 0, in 16 s; every C compile is warning-free under `-Werror` (`build/unity.c` included). The build's ONE diagnostic is the pre-existing `ld: warning: cannot find entry symbol _start` from the minibios `as`/`ld` step, unchanged by this unit |
| `make selftest` | **3522 + 228 + 541 checks, 0 failures, 0 skipped** — exactly the brief's counts — exit 0 |
| `make nasm` | **118/118 passed**, 0 of 123 produced no state, **the same 5 skipped** (`tests/nasm_skip_c.txt`) — exit 0 |
| `make oracle` | exit 0 |
| `make determinism` | exit 0 ("two consecutive oracle runs are byte-identical") |
| `make transcript-diff` | exit 0 — "the C transcript is byte-identical to the reference's" |
| the boot (M2-5 GATE 1a's own command line, `--hda … --initrd … --bios build/minibios-80m.bin --memory 80 --virtio-console … --virtio-console-in …`) | exit 0; **virtio stream byte-identical** to `results/m2-5-c-virtio.txt`; **serial byte-identical** to `results/m2-5-c-serial.txt`; same `eip=0xc101acb2 cs=0x60 cr0=0x80050033 in_hlt=1 unimplemented=0` |

The whole suite was run TWICE in this session — once on a build with the two `src/hw`
renames applied, once on the SHIPPED tree after reverting them. `gates.txt` holds the
second (shipped) run; both were green with the same counts, and the two builds' code is
identical (§2b, §9), so no gate can distinguish them. Every gate below is exit status —
the suites print fixture lines that LOOK like errors; the exit code is what decides.

**The boot's INSTRUCTION COUNT is not a comparable quantity, and this is pre-existing —
measured, not assumed.** The same command on the untouched binary gives different counts
and a different `cr3` on different runs, because the guest reads the host clock
(`results/m2-5-baliveau-kernel.md:174-175` documents exactly this, with four values for
this command: 579,621,977 / 579,646,211 / 579,688,312 / 579,728,752). Re-measured in this
session: the PRISTINE binary twice → 579,513,400 `cr3=0x376000` and 579,616,813
`cr3=0x1d6000`; the FINAL binary three times → 579,679,814 / 578,903,754 / 579,625,369 with
three different `cr3`. The one serial line that moves is the same one —
`/proc/interrupts`'s XT-PIC timer count (874 in the recorded artifact, 873/875/868 on
re-runs). **Both binaries occupy the same value set**, and the final binary's three runs
agree with the recorded artifact on 1394/1394 serial bytes in run 3 and in run 1 of the
recorded gate run. The gate's comparison is therefore the BYTES of the serial and virtio
streams and `eip/cs/cr0`, which are identical — not the count.

## 9. Files changed

| file | change |
| --- | --- |
| `Makefile` | `$(PERF_CFLAGS)` defined and appended after `$(CFLAGS)` in every rule; the unity rules (`gen/unity_c.py` → `build/unity.c` → `build/unity.o`); `build/v86c` links `$(UNITY_OBJ)`; `unity` and `bench` targets; `$(CPU_OBJ)` dropped from the link (it is inside the unity object) |
| `gen/unity_c.py` | NEW — emits the unity TU; refuses a unit with a colliding file-scope static |
| `src/cpu/interpreter_gen.c` | unchanged, but now compiled INSIDE the unity TU |
| `src/main.c` | two portability additions so the host cc can build the same source: a `__GLIBC__` branch reading `mcontext_t`'s `gregs[15/16/10]` (indices, not `REG_*`, so no `_GNU_SOURCE` is defined and cosmocc's preprocessor input is untouched), and an explicit `#include <unistd.h>` for `_exit`. **`build/v86c` and `build/v86c.com.dbg` sha256 IDENTICAL before/after** |
| `tests/perf_bench.sh` | NEW — the benchmark (§1) |
| `results/perf-1/revert-attribution.sh` | NEW — the G4 revert arm (§6) |
| `results/perf-1/guard-asymmetry.sh`, `guard-asymmetry.txt` | NEW — the unity generator's collision guard, tested by asymmetry (§4) |
| `results/perf-1/gates.sh`, `gates.txt`, `samples.txt`, `boot-serial.txt`, `boot-virtio.txt` | NEW — the gate run, the raw per-run numbers, the boot legs |
| `results/perf-1/pre-a/cpu_state.{h,c}` | NEW — the pre-route-(a) bytes, kept so the revert is auditable |

**NOT in the shipped diff, and why:**

* `src/cpu/cpu_state.{h,c}` — route (a) was applied, measured (§10) and REVERTED; the
  shipped bytes are identical to the pre-unit state (`results/perf-1/pre-a/`).
* `src/hw/kernel.c`, `src/hw/ps2.c` — the two file-local statics that collide when
  `src/hw` is folded into the unity TU were renamed for the ALL-unity experiment, then
  reverted, because the shipped unit is CPU-only and the collision therefore does not
  exist there. The names are back as they were, and the hazard is now guarded by
  `gen/unity_c.py` + `results/perf-1/guard-asymmetry.sh` instead of by a rename that
  nothing in the shipped build needs. (Proof the revert is code-neutral: §2b.)

## 10. What did NOT help — a negative result is a result

Route (a) is here because it did not survive contact with route (b), not because it failed
on its own terms — it works, and it works about as well as the orchestrator's own earlier
measurement said it would.

* **Route (a), `static inline` in the headers: MEASURED — and it LOSES to the unity build.**
  The six register accessors moved from `cpu_state.c` into `cpu_state.h` as `static
  inline` (bodies verbatim, `get_reg8_index` with them):

  | configuration | best real | vs |
  | --- | --- | --- |
  | baseline (cosmocc defaults, per-TU) | 6.367 s | 1.000x |
  | **(a) alone**, per-TU, no flag changes | **5.331 s** | **1.194x** vs baseline |
  | (a) + `$(PERF_CFLAGS)`, per-TU | 4.349 s | 1.185x vs the 5.155 s flag-only build |
  | `-foptimize-sibling-calls` alone (for scale) | 5.154 s | 1.235x |
  | unity + `$(PERF_CFLAGS)` — **SHIPPED** | **4.095 s** | 1.555x |
  | (a) + unity + `$(PERF_CFLAGS)` | 4.229 s | 1.505x — i.e. **no better than unity alone** |

  So route (a) genuinely works on its own (1.194x, close to the 1.11x the orchestrator
  measured for the same six functions), and on top of the flags it buys 1.185x — but it
  **does not add anything to the unity build** (4.229 vs 4.095 is inside the block-to-block
  drift, and not in (a)'s favour). What it structurally CANNOT reach is everything that is
  not a one-line accessor: `read_imm8`'s 337M calls, `resolve_modrm32`, `read32s`,
  `get_phys_eip` — moving those bodies into headers duplicates the fetch/decode code at
  ~800 call sites, whereas the unity TU gets them inlined for free. **Reported and
  reverted: the shipped tree has ONE inlining mechanism, not two.** (The reverted files are
  byte-identical to their pre-unit state: `results/perf-1/pre-a/` holds them, and the
  final build's `.text` was compared against the shipped one — §9.)

* **`-O3`**: 4.428 s vs 4.159 s at `-O2` on the unity build — worse, and it refutes the
  "just inline more" hypothesis for the residual gap (§7.5).
* **`-momit-leaf-frame-pointer` and `-fschedule-insns2`**: each is ~neutral or harmful once
  sibling calls are on (per-TU 5.279 / 5.167 against 5.154; unity 4.243 against 4.234), and
  together with `-finline-functions-called-once` they CANCEL part of the sibling-call win
  (5.479). Not shipped.
* **Folding `src/hw` into the unity TU**: 0.2%, indistinguishable (4.962 vs 4.970) and it
  costs `-Werror` on the device layer. Not shipped.
* **`-fno-portcosmo`**: unrecognized option — cosmopolitan's pass cannot be disabled from
  our side, which is why §7.5's residual is not fully separable.

## 11. What to distrust

* The MEDIAN is noisier than the best-of-N on this host (up to ~5% within a block, ~4.8%
  drift between blocks for the same binary). Sub-2% differences in the tables above are
  within that drift; the reported wins are 3.2%–55%.
* The blocks were measured at different times over ~40 minutes with other agents' work on
  a 32-core host; the load averages printed by every block (1.2–3.4) are in the record.
* The `-pg` profile is a glibc-leg build (the counts are the code's, not the compiler's, so
  they transfer) and its SELF-TIMES are not used anywhere in this document.
