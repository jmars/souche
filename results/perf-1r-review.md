# PERF-1R — independent review of step 1 (unity TU, re-enabled codegen flags, the cosmocc gap)

**Reviewer verdict: the change is correct AS SHIPPED.** The unity TU does not change
guest-visible behaviour today, by three independent verifications: the generator's guard
emits for the shipped set; an exact symbol-parity check I built and ran on the real tree is
clean (2552 defined globals per-TU == 2552 in `build/unity.o`, zero diff in both
directions); and every behavioural gate runs the shipped (unity) binary and is green. The
re-enabled codegen flags are semantics-safe for this codebase. The performance attribution
is honestly measured and its caveats are stated.

**One SHOULD-FIX**: the guard's safety model is wrong in its own documentation, and the one
class that is genuinely silent against a working build is invisible to the guard — while an
**exact mechanical check exists, is verified, and is not shipped.** Plus four
nice-to-haves. Coverage statement at the end.

---

## SHOULD-FIX-1 — the guard cannot see the ONE silent class, and its rationale describes the wrong failure mode

`gen/unity_c.py:19-28`, `results/perf-1/guard-asymmetry.sh:5-9` and
`results/perf-1-inlining.md` §4 all claim: two same-name file-scope statics "bind silently
if the signatures agree; the compiler only catches the type-mismatch half."

**Measured with the shipped compiler (cosmocc gcc 14.1, real files — not stdin):** every
in-unit same-name two-DEFINITION pair errors regardless of signature:

| collision (both definitions inside the one TU) | compiler verdict |
| --- | --- |
| `static fn` + `static fn` | `error: redefinition` |
| `static fn` + global fn, **same signature** | `error: redefinition` |
| `static obj` + global tentative obj (either order) | `error: non-static/static declaration follows` |
| `static obj` + global obj with initializer | `error: ... follows static declaration` |

So the guard's in-set refusals are **diagnostics-quality** (a clearer message than gcc's),
not the safety net the header describes. There is no type-dependence.

**The class that IS silent** (demonstrated end-to-end; per-TU works, unity silently
rebinds, zero diagnostics, link fine):

- `cpua.c`: `static int rebind(int x) { return x + 1; }`
- `cpub.c`: `extern int rebind(int x);  int cb(void){ return rebind(1); }`
- `hwdef.c` (outside the unit): `int rebind(int x) { return x + 1000; }`

Per-TU link: `cb()` = **1001** (binds to hwdef's global). Unity (`#include cpua.c, cpub.c`
+ hwdef.o): `cb()` = **2** — the call silently binds to the static. Also probed: a
`static` + a later declaration-only prototype in one TU binds silently (cosmocc, rc=0) —
the unity build can turn a former link-error into a silent bind, or, with the definition
outside the unit, a **silent behaviour change with a working per-TU baseline**.

`gen/unity_c.py` scans only `UNITY_SRC` (the 18 CPU files), so this class is structurally
invisible to it. **Verified ABSENT in the current tree** (cross-set scan: none of the 274
unit static names equals any hw/main/SF global definition or any header-declared name; no
multi-declarator statics; no `__COUNTER__`/`__LINE__`/`__FILE__` in the unit; 277/277
static-definition lines matched by the regex with 0 misses). The next edit, though, can
create one and nothing will say a word until a byte-level gate trips.

**Fix (full code stored on `handoff-v86c-perf-fix-1`):**
1. Extend `gen/unity_c.py` with `--watch FILES` (hw + main + SF sources and the headers):
   refuse a unit whose static name equals any watched definition or declaration.
2. Add the exact mechanical gate: a `unity-symbols` make target running a small
   `nm --defined-only` global-set parity check between Σ(per-TU CPU objects) and
   `build/unity.o`, required equal in **both** directions. I built the per-TU objects
   myself (same flags, in /tmp) and ran it on the shipped tree: **2552 == 2552, clean**,
   and it catches a synthetic shadowing the regex cannot. Wire it into `make selftest` —
   this is also the answer to "tested ≠ shipped" (below).

## NICE-TO-HAVE-1 — tested ≠ shipped: acceptable as shipped

The unity TU **is** exercised by three behavioural gates — confirmed in
`results/perf-1/gates.txt`: `make nasm` runs `build/v86c-nasm` → the unity binary
(118/118), `make transcript-diff` runs `build/v86c` (byte-identical), and the boot gate
runs `build/v86c` (serial and virtio **byte-identical** to the pinned pristine artifacts,
`diff -q` verified). The selftests (3522+228+541) compile per-TU (all three link lines in
gates.txt show the `.c` files). The split is defensible (`cpu_selftest` deliberately links
its own recording `cpu_state.c`, Makefile:206-210); SHOULD-FIX-1's parity target is the
cheap way to give the unity TU a build-time structural gate. Side effect worth knowing:
`make clean && make` no longer compiles the per-TU CPU objects, so per-file `-Werror`
coverage now lives only in `make selftest` (documented in the Makefile; fine while
selftest stays in the gate suite).

## NICE-TO-HAVE-2 — SEX8/SEX16 defined twice

`src/cpu/instructions.c:80-81` and `src/cpu/instructions_0f.c:59-60` define SEX8/SEX16 with
byte-identical bodies. Per-TU they are independent macros; under unity the second
redefinition silently wins (benign while identical; a divergent edit → `macro redefined`
warning → `-Werror` error, so loud). The guard cannot see macros. Consider hoisting to a
shared header. (The only other cross-file `#define` duplicates are CHECK/CHECK_EQ in the
selftests, not in the unit.)

## NICE-TO-HAVE-3 — "closes the ENTIRE gap" overstates precision

The parity pair (5.154 s vs 5.107 s) is cross-block and 0.9% apart — inside the measured
~4% block drift. The load-bearing evidence is elsewhere and is solid: sibling-calls alone
5.154 vs baseline 6.367 (24%, real) and the mixed-link arm (cosmocc objects + host glibc
= 6.438 vs the APE's 6.367 — libc/APE provably irrelevant). Precise wording: "brings
cosmocc to within noise of glibc parity". One-line fix in `results/perf-1-inlining.md`
§5/§7.

## NICE-TO-HAVE-4 — the residual 7–11%: honestly stated, one cheap probe not run

3.932 vs 3.662 (1.074x; 1.083x in two other blocks) is correctly stated as unseparable on
this host (cc1 version 14.1.0 vs 16.2.0 vs `-fportcosmo`; no gcc-14 available,
`-fno-portcosmo` unrecognized). The evidence gathered (mixed-link, libc call counts, the
`-O3`-is-worse probe) supports "cc1 codegen/inliner difference" and does not point to an
overlooked cause. One probe not run: disassemble the hot loop in both legs (same Makefile,
same flags) and diff where the codegen diverges — confirms inliner vs backend. Direction
for PERF-2's M3 measurement, not a defect.

## Checked and safe

- **`-foptimize-sibling-calls`**: no `setjmp/longjmp/alloca/VLA` anywhere in `src/`
  (grep, zero hits in the core); no `__builtin_return_address`/`__builtin_frame_address`/
  `backtrace`/`dladdr`; the host-fault handler (src/main.c:643-691) reads the
  kernel-provided ucontext RIP/RSP (correct under tail calls) and prints raw stack words,
  diagnostic-only, boot mode only — never a guest-visible path; dispatch recursion is
  bounded (~3 frames: prefix → 0F entry → sti re-dispatch; the main path is the loop
  cpu.c:119), so tail-call stack depth is not guest-observable.
- **`-finline-functions-called-once`**: semantics-neutral by construction (GCC does not
  inline address-taken calls).
- **`-fpatchable-function-entry=0`**: only drops the ftrace patch area from our objects;
  the port never uses cosmopolitan ftrace.
- **The two src/main.c portability lines**: `<unistd.h>` was already transitively
  included under cosmocc; the `__GLIBC__` `gregs[15]/[16]/[10]` branch uses the correct
  x86-64 glibc REG_RSP/REG_RIP/REG_RBP indices and is reachable only on a host fault under
  a glibc build (a measurement leg, never shipped). Neutrality is proven by the artifact
  (reproducible hash `ddf54be2…` + identical `.com.dbg`) — the right evidence, given the
  heredoc-token trap §2b records.
- **The inlining-revert attribution**: route (a) alone 5.331 vs 6.367 (19%, real);
  (a)+unity+flags 4.229 vs unity+flags 4.095/3.932 — (a) is not faster than unity alone in
  any block (samples.txt, ROUTE-A and FINAL SHIPPED blocks). "Unity subsumes it" holds by
  direction.
- **The boot instruction-count attribution is right and pre-existing**:
  results/m2-5-baliveau-kernel.md:171-175 documents the same value-set variance
  (579.6M–580.6M) with the same cause (the guest reads the host clock); the final binary
  occupies the same value set; the one moving serial line is `/proc/interrupts`' XT-PIC
  count — consistent with timer-count variance. The gate compares serial/virtio **bytes**
  plus terminal eip/cs/cr0: correct. The bench's fixed count (200,095,214 every run) is
  consistent with this: it stops AT the limit, and the RTC variance manifests later in the
  579M-instruction boot.

## Coverage

- **Read in full**: `gen/unity_c.py` (98 lines), `Makefile` (414), `src/main.c` (1206 —
  the fault handler, both portability lines, the loader and the run driver),
  `tests/perf_bench.sh`, `results/perf-1/guard-asymmetry.sh` + `.txt`,
  `revert-attribution.sh`, `gates.sh`, `samples.txt`, `results/perf-1-inlining.md` (354),
  and `results/perf-1/gates.txt` (G3.1–G3.6).
- **Sampled by targeted greps** (not read line-by-line): the 18 unity sources (~30k
  lines) — all 277 file-scope static-definition lines checked against the guard's regex
  (0 misses), all `#define`/`#ifdef`/`#undef`, `__COUNTER__`/`__LINE__`/`__FILE__`,
  setjmp/alloca/VLA, return-address/backtrace intrinsics, multi-declarators, object
  globals, cross-set name collisions, header declarations vs unit statics.
- **Verified by building in /tmp** (not the tree): per-TU cosmocc compiles of all 18
  files with the shipped flags + `nm` parity vs `build/unity.o` (2552 == 2552, clean);
  compiler-semantics probes under cosmocc gcc 14.1 (all in-set collision classes error
  loudly; static+prototype binds silently; the full per-TU-works/unity-rebinds scenario:
  `cb()` = 1001 → 2); `diff -q` of the boot serial/virtio captures against the pinned
  pristine artifacts (identical).
- **Not re-checked** (orchestrator-verified, per the brief): the 50.45 MIPS figure, the
  gate re-runs, the interleaved timing blocks (read from samples only). Ignored as
  briefed: PERF-2's new files.
- sha256 of every reviewed file is recorded on `handoff-v86c-perf-review` (the COVERAGE
  observation).
