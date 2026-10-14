# R1 — independent review of the CPU foundation and the nasm oracle scaffold

**Unit:** `HANDOFF=v86c`, review node `handoff-v86c-review`. Read-only pass; the
authoritative record is the memory node, this file is the human-readable copy.

**Verdict: the port is faithful and the oracle tells the truth — no blockers.**
One measured divergence (F1), two latent harness items (F2, F3).

---

## F1 — SHOULD-FIX: the default device hooks return 0 where the reference returns 0xFF/-1

`src/cpu/memory.c:105-113` — `device_read_default8`/`device_read_default32` return 0,
and the justification comment (`src/cpu/memory.h:47-49`) claims:

> MEASURED basis: v86's own CPU tests boot with no devices (oracle/run.mjs config),
> and the reference's mmap_read* for an address no device claimed returns 0.

Upstream disagrees on both counts:

- `v86/src/io.js:40-61` registers the unclaimed-memory fallback for
  `[memory_size, MMAP_MAX)` in the IO constructor, **regardless of devices**:
  read8 → `0xFF`, read32 → `-1` (read16 composes two read8s via
  `memory.rs:106-109`, so → `0xFFFF`); writes are dropped.
- The npm build does create a VGA regardless of config (`browser/starter.js:234`,
  `vga_memory_size` defaults to 8 MiB), so "no devices" is not the boot state.

**Measured** against the pinned npm reference through `oracle/nasm_state.mjs`
(probe guest; RAM write/read control roundtrips exactly, then):
`read8 [0x200000] = 0xFF`, `read16 = 0xFFFF`, `read32 = 0xFFFFFFFF` (identical at
0x300000). The VGA window and LFB read the zeroed VGA buffer at boot
(`read32 [0xB8000] = 0`, `[0xE0000000] = 0`), so the C default of 0 happens to
match *there* — but for every address ≥ `memory_size` the C leg will see 0 where
the reference sees 0xFF/0xFFFF/0xFFFFFFFF. The first guest that touches ≥
memory_size diverges, and the class-B nasm tests the C leg is meant to un-skip
(`jump` fetches at 0x42b001, past 2 MiB) would decode a different instruction
stream than the reference.

**Fix:** `device_read_default8` → `0xFF`, `device_read_default32` → `-1` (in both
`memory_devices` and `memory_default_device_hooks`); keep the write defaults as
no-ops; correct the `memory.h` comment to cite `io.js:40-61` as the basis.

## F2 — NICE-TO-HAVE: mkfixture.c:164's mm comment is wrong and the mm ordering would fail if the mm branch were reachable

`tests/mkfixture.c:164` says "physical mm = low 64 bits of st", but the harness's
own measured layout (`mkfixture.c:126-136`) is that `st_space` slot i holds
**logical** st(i) = physical (top+i)&7 — so physical register p lives at slot
(p−top)&7. The loop reads slots 0..7 directly, i.e. logical order, while the
reference leg reads physical `fpu_st[0..7]` (`oracle/nasm_state.mjs:121`, verbatim
from `run.js:376`). With top≠0 the two legs would disagree systematically.

Today the branch is unreachable — the reconstructed ftag always carries
`0xffff0000` (`mkfixture.c:138`) and can never equal `FPU_TAG_ALL_INVALID`
(0xAAAA); measured 0/123 fixtures, honestly documented in
`results/p0-coverage-notes.txt`. So this is latent, not live.

**Fix for a later unit that wants real mm coverage:** emit the 16-bit tag word
(drop the `0xffff0000` OR — upstream's gdb `$ftag` is 16-bit) so the branch can be
reached, and read mm word p from slot (p−top)&7. At minimum, correct the comment now.

## F3 — NICE-TO-HAVE: the reference leg's exception map is extended but not declared

`oracle/nasm_state.mjs:74` maps vector 14 → SIGBUS, where upstream `run.js:329-341`
aborts on any vector not in {0, 6, 13}. Benign and deliberate — no corpus fixture
produces SIGBUS, the reference takes no #PF on any corpus test, and the extension
serves the C leg — but it is an unlisted transcription divergence: if a future
corpus test made the *reference* take #PF, the JS leg would record-and-compare
where upstream would abort. One line in `results/p0-oracle-scaffold.md` would make
it declared rather than silent.

---

## What was checked

**Foundation** — read `src/cpu/{cpu_state,flags,memory,paging}.{c,h}` and
`cpu_selftest.c` line-by-line against the cited upstream lines:
`misc_instr.rs:8-66`, `arith.rs:12-103`, `cpu.rs:1911-1918, 1921-1953, 1955-2050,
2062-2336, 2338-2385, 2461-2512, 2931-2997, 3345-3361, 3477-3530, 3538-3585,
3941-4040, 4128-4196, 4412-4418, 4478-4520, 4671-4760`, `state_flags.rs:3-26`,
`memory.rs:24-66, 103-352`, `global_pointers.rs` (member order), the constants of
`cpu.rs:80-300`/`regs.rs`, `cpu.js:150-215` (reset/init values), `fpu.rs:582-597`
(tag-word indexing). **All match.** The specific hunt items from the brief came up
clean: no getter/setter reads `flags` directly; every `flags_changed` update
matches the upstream bit pattern (including `FLAG_SUB` in bit 31 and adc/sbb's
eager CF/AF/OF); no write tracking was added to the TLB; no off-by-one or masking
error in the page walk; the fault path restores `instruction_pointer =
previous_ip`, sets cr2, builds the error code (present | write<<1 | user<<2 |
rsvd<<3 | fetch<<4) and invalidates the TLB entry exactly as upstream. All 12
documented divergences (D1–D12) verified against the code — no *undeclared*
divergence beyond F1.

**Oracle** — `tests/nasm_run.py:149-253` vs `run.js:362-511`: the fixture layout
(8 reg32, eip, 8 st, 16 mm, 32 xmm, 2048 mem, eflags, ftag, fstat), the
`fpu_tag == FPU_TAG_ALL_INVALID` branch, `FPU_STATUS_MASK` 0xFDD5, the
7-significant-digit float tolerance, the "invalid"-entry skip, the exception
gating (GPRs only when no exception; exception eip only when one) and the
exception-kind comparison are all field-for-field faithful. The negative control
(`results/p0-negative-control.txt`) genuinely fails on the GPR, x87 and
exception-kind paths — verified in the runner code, not just the log, and the
unperturbed contrast passes. The 10 `tests/nasm_skip.txt` entries are genuine
reference-leg gaps, not port gaps: class A (5) — the release wasm panics because
`cpu_exception_hook` is DEBUG-gated (`cpu.rs:2441, 2505, 3431-3469`) and #DE/#UD
delivery hits the empty IDT (`cpu.rs:819`); class B (5) — the reference takes no
fault where the hardware does. Both classes are things the *C leg* can and should
check against the native fixture. The two mkfixture fixes are verified against
`results/p0-fpu-harness-evidence.txt` (fnstenv-arbitrated) and the tag-word
reconstruction matches v86's physical indexing (`fpu.rs:582-597`).

**Could not check:** PAE (trapped by design), `call_interrupt_vector` delivery
(owned by M1c), the JIT-only dropped paths, `nasm2gas.py` semantics beyond the
byte-equivalence gate, and the `read_imm`/fetch path (a later unit).

**Note, not a finding:** the non-exception branch never compares eip — faithful to
`run.js`, which also does not. The C leg emits eip in its fixture document anyway
(`decode_actual` reads it), so M1c could add that check cheaply if it wants
tighter gating than upstream's own comparison.
