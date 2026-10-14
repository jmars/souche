# R3F — closing the emitter review's findings N1 and N2

Unit R3F (coder).  Closes `results/r3-review.md`'s two nice-to-haves.  Raw evidence:
`results/r3f-gates.txt` (every gate's output and exit code, pasted in order).

**Verdict: N1 fixed at the emitter (the fallthrough arm no longer emits a check -- it now
compiles to exactly what the pinned release reference compiles to), N2 fixed (the dead
`pagefault` check is emitted only where a faultable read precedes it).  `make`,
`make transcript-diff`, `make nasm` (21/21 + 102 skips) and `make selftest` (2968 checks /
0 failures) are all green and UNCHANGED against the pre-fix baseline.  G1's second half --
"must produce the same state as the reference for that input" -- is NOT reachable by the
shipped binary today, and that is MEASURED, not asserted: see §3.4.  The abort is gone and
the fallthrough now dispatches the unprefixed entry, which is the part the fix can own.**

Files changed: `gen/interpreter_c.js` (the fix), `src/cpu/interpreter_gen.c` (regenerated).
Nothing under `tests/`, `oracle/` or `v86/` was touched (`tests/test.img` sha256 still
`1a597ac1...e737a`).  New artifacts: `results/r3f-gates.txt`, `results/r3f-probe.asm`,
`results/r3f-probe.sh`, `results/r3f-inject.sh`, `results/r3f-fallthrough.mjs`,
`results/r3f-prefix-interpreter_gen.c.gz`.

| artifact | sha256 |
| --- | --- |
| `gen/interpreter_c.js` (after) | `c01a320897b87eafdb84a2c31b6d28312e1af5b1a512a21564d55492050f9a08` |
| `src/cpu/interpreter_gen.c` (after) | `122853fc5f96f3918792383a7f9966923158cb781e3d9dff36b93caa96da1133` |
| `src/cpu/interpreter_gen.c` (before, = R3's reviewed file) | `bb53cf87e7969d27277e6d7c549bf7e1a6571b0b826386b32dfcb87e693c6f93` |
| `results/r3f-prefix-interpreter_gen.c.gz` (that file, stored) | `f69a4b2995fc1320f67d10d4dceca484d9ffe96cafb273ce7bc9ef4a42587608` |
| `build/v86c` (after) | `a7ee60fd9126bb14dbbbb83a04bd2d7d89997c0c0a523a14db295093d91b0771` |

---

## 1. N1 — the prefix-fallthrough assert

### What the arm is

`gen_instruction_body` emits, for a group whose prefix arms are guarded by `prefixes_`, a
final `else` reached when the guest's prefix combination matches none of the defined arms
(e.g. `F2 0F 6F`, `F3 0F 71`, `66 0F 52`).  Upstream puts a `dbg_assert!` at the head of that
arm; the port emitted `DBG_ASSERT(...)`, and the shipped binary has no `-DNDEBUG`, so the arm
**aborted** where the reference **executes the unprefixed body** (`dbg_assert!` is
`debug_assert!`, `dbg.rs:63` -- compiled out of the pinned release wasm).

### The fix, and why not `-DNDEBUG`

`gen/interpreter_c.js:364` no longer emits a check: the arm is `gen_instruction_body_after_prefix(no_prefix, size)`
and nothing else, i.e. byte-for-byte the release reference's codegen for that site.  The
emitter carries the rationale at the site and its header now declares a **third C-side
difference**; the generated file's header states it once, in prose, instead of 131 identical
comment lines.

I did **not** take the review's other option (`-DNDEBUG` for the APE) because it throws away
stops the port relies on, which the brief warned about.  MEASURED evidence for that choice,
not an argument: `-DNDEBUG` silences `src/cpu/cpu.c:83`'s `assert(!in_mapped_range(phys_addr))`
-- the stop `make nasm`'s `jump` test currently ends on.  Dropping the check at *this one
site* instead leaves **every** test count and every skip identical (§4 G3), which is the
observable statement that no other loud stop changed.  The dropped check is the only one in
the tree that tests the GUEST's input rather than an invariant of the port -- which is why it
is the one that must match the reference's release semantics.

### The generated diff is exactly this and nothing else

```
135 lines removed, 11 added (all 11 in the file header)
  108  DBG_ASSERT((prefixes_ & (PREFIX_66 | PREFIX_F2 | PREFIX_F3)) == 0);   <- sse groups
   23  DBG_ASSERT((prefixes_ & (PREFIX_F2 | PREFIX_F3)) == 0);               <- f2/f3 groups
    4  if (cpu->pagefault) return;                                           <- N2, §2
```

The post-fix `0F 6F` arm reads (with the group's own `task_switch_test_mmx()` guard, which
still runs):

```
else {
        if (!task_switch_test_mmx()) { return; }
        if (modrm_byte < 0xC0) { unimplemented_instruction("instructions_0f::instr_0F6F_mem"); return; }
        else                   { unimplemented_instruction("instructions_0f::instr_0F6F_reg"); return; }
}
```

## 2. N2 — the dead `pagefault` check

`gen_instruction_body_after_fixed_g` now emits the mem-branch check only when a faultable
call precedes it (`resolves_modrm = !encoding.custom_modrm_resolve`).  The review's
prescription was right on contact with the code: for 0x8D/0x8F the raw `modrm_byte` is passed
and `modrm_resolve` is skipped, so the check guarded nothing; the immediate's check travels
with the immediate (below it in the same arm), so no read is left unchecked either way.  The
post-fix 0x8D case:

```
if (modrm_byte < 0xC0) {
        instr16_8D_mem(modrm_byte, modrm_byte >> 3 & 7);      (the check is gone)
}
```

Behavior cannot change: the dispatcher clears `pagefault` once per instruction
(`cpu.c:97`), and the removed line compared a flag only a *preceding* read could have set.

## 3. G1 — the N1 probe (the asymmetry)

Rig: `results/r3f-probe.sh` -- assembles `results/r3f-probe.asm` in five modes and runs each
image on the pinned reference, on the shipped binary, and on **the pre-fix binary rebuilt
from the reviewed artifact**: the stored pre-fix dispatcher's sha256 is checked against
`bb53cf87...` (R3's recorded sha) before it is compiled, so the "before" side is the reviewed
file, not a recollection.  Raw output: `results/r3f-gates.txt`, section G1.

### 3.1 The asymmetry itself

| image | BEFORE (pre-fix binary) | AFTER (shipped) |
| --- | --- | --- |
| `MODE=1` `F2 0F 6F C1` | **exit 134 (SIGABRT)** `interpreter_gen.prefix.c:10484: assert((prefixes_ & (0x20\|0x10\|0x08)) == 0) failed` | exit 1, `v86c: unimplemented: instructions_0f::instr_0F6F_reg` |
| `MODE=2` `3E 0F 6F C1` (same arm, same length, prefix the mask does not name) | exit 1, `instr_0F6F_reg` | exit 1, `instr_0F6F_reg` |
| `MODE=0` `0F 6F C1` (no prefix) | exit 1, `instr_0F6F_reg` | exit 1, `instr_0F6F_reg` |

Three things follow, all measured:

1. the abort is gone;
2. **the fallthrough now dispatches the UNPREFIXED entry** -- the stop names
   `instr_0F6F_reg`, the *plain* entry's symbol, which is only reachable by executing the
   unprefixed body (the F2 arm's symbol would be `instr_F20F6F_reg`);
3. after the fix, MODE=1 lands exactly where MODE=2 lands.  MODE=2 is the control that
   isolates the stray prefix: it reaches the same arm with the same byte length and never
   tripped the assert pre-fix, so the assert was the *only* difference between the two paths.

### 3.2 What the reference actually does (premise, measured not assumed)

* The stray prefix is **inert** in the reference: the MODE=1 and MODE=2 transcripts differ in
  EXACTLY one line -- the image sha256.  Every register, EFLAGS, eip, x87 field, instruction
  count and the hashed memory window are identical.
* The fallthrough body is **executed**, not skipped: MODE=4 puts `eax` into `mm0` and then
  runs the stray-F2 *store* form `F2 0F 7F 05 ...`, and its memory window sha256 equals
  MODE=5's, where the same 8 bytes are written by an ordinary GPR store
  (`638e1209...009ef9` both) -- i.e. the 8 bytes at 0x101000 are `mm0 = 0x0000000011111111`,
  byte for byte.  A prefix-ignoring implementation would have left the window unchanged.

### 3.3 Corrected inference (my own first draft was wrong)

I first wrote that the reference's `MOVQ` leaves no state trace.  The stand-in experiment in
§3.4 falsified that: the reference's MMX write moves the x87 `stack_empty` bit and tag word
(`0x00` / `0x5554` against the reset `0xff` / `0xffff`).  What is inert is the stray prefix
(§3.2), not the instruction.

### 3.4 G1's second half: "the same state as the reference" -- NOT reachable today

The reference's state on these inputs is the state after executing the *unprefixed body*
(MOVQ/MMX, `F2 F3` SSE, JMPE).  The port stops there because **none of those bodies is
written yet**, and that is counted, not claimed: `results/r3f-fallthrough.mjs` walks the
table exactly as the emitter groups it and finds

```
groups with a prefix arm and a no-prefix body : 91
distinct stray prefix combinations             : 174   (66: 2, F2: 89, F3: 83)
whose UNPREFIXED body is IMPLEMENTED in src/cpu: 0
```

`results/r3f-inject.sh` then supplies the missing body as a **no-op stand-in** (built in
`build/r3f-inject/` from a copy of `src/cpu`; the repo is not touched) and measures what the
fix does and does not buy:

* with a body present the fallthrough **runs to completion** (exit 0, a full 1965-byte
  transcript) instead of aborting or stopping -- the arm executes the plain entry;
* the transcript still differs from the reference's, in exactly the x87 fields a real MMX
  write would touch (`data[0]`, `stack_empty`, `tag_word`) and nowhere else.

So the residual divergence is **the unported instruction body (M1b's file)**, and it is not a
divergence the fallthrough introduces: the same encoding *without* the stray prefix stops at
the same symbol (MODE=0 above), and it will be closed for both at once when M1b lands the
body.  What R3F removes is the one thing the fallthrough *did* add on its own: a hard abort
reachable from ordinary guest bytes.

## 4. Gates (raw output and exit codes in `results/r3f-gates.txt`)

| gate | command | pre-fix | post-fix | delta |
| --- | --- | --- | --- | --- |
| G1 | `sh results/r3f-probe.sh` (+ `r3f-inject.sh`, `r3f-fallthrough.mjs`) | abort (134) on MODE=1 | named stop, §3 | the asymmetry, §3.1 |
| G2 | `make` (from-scratch rebuild of the dispatcher + APE, `-Werror`) | rc=0 | **rc=0** | none |
| G2 | `make transcript-diff` | rc=0, empty diff | **rc=0, empty diff**; both transcripts sha256 `8bb8eca53f5c5ccbb1ef2c532c2d4a493180a5e5b44da87bceb6c0a8f094cd61` | none |
| G3 | `make nasm` | rc=0, `Passed 21/21`, 102 skipped | **rc=0, `Passed 21/21`, 102 skipped** | **none** |
| G3 | `make selftest` | rc=0, `2968 checks, 0 failures` | **rc=0, `2968 checks, 0 failures`** | **none** |
| G4 | `node gen/interpreter_c.js` twice, diff | -- | rc=0, empty diff, both sha256 `122853fc...` | none |

G3's zero delta is the evidence for the `-DNDEBUG` decision in §1: a changed assert policy
would have moved `make nasm`'s counts (the `jump` test stops on `cpu.c:83`'s assert today);
no count moved, so no stop was silenced.

## 5. Findings on the brief and the review

1. **The review's N1 is confirmed and refined.** Reachability is not just "an input like
   `F2 0F B8`": 91 prefix groups offer 174 stray combinations (2 of them a stray `66`,
   `0F 52`/`0F 53`; the review's `F2 0F B8` and `F3 0F 71` are both in the list).  The base
   table has NO such case -- its 14 prefix groups (the REP-string forms 6C-6F, A4-A7, AA-AF)
   define both F2 and F3 arms, so their `else` arm is only ever entered with a zero mask.
2. **G1's second half is over-specified for this unit** (stated, not worked around): with 0
   of 174 unprefixed bodies implemented, no probe can make the shipped binary produce the
   reference's state on a fallthrough input.  §3.4 measures the whole distance and shows it is
   M1b's to close, not the fallthrough's.
3. **The review's prescription for N2 was correct on contact** with the code (unlike R1's F1,
   which R1F had to re-scope) -- the check is dead, the immediate's check is independent, and
   the gate on `!custom_modrm_resolve` is exactly right.
4. **A pre-fix artifact is recoverable and verified, but it takes care to say why.**  The
   tree's generated file had already been regenerated from the edited emitter by the time I
   snapshotted it (a `make nasm` on a half-fixed tree), so the stored "before" artifact was
   reconstructed from the emitter with its two functional sites reverted, and *accepted only
   because it reproduces `bb53cf87...`* -- R3's recorded sha for the file it reviewed.  Anyone
   re-deriving a pre-fix artifact should use that sha as the acceptance test, not the file's
   mere presence.

## 6. Reproducing this unit

```sh
make                       # regenerates src/cpu/interpreter_gen.c, rebuilds the APE
sh results/r3f-probe.sh    # G1: the asymmetry, the reference measurements, the premise
sh results/r3f-inject.sh   # G1: the stand-in-body probe (§3.4), needs the probe images
node results/r3f-fallthrough.mjs   # the reachability sweep
make nasm && make selftest && make transcript-diff   # G2/G3
```

`sh results/r3f-probe.sh` re-verifies the pre-fix dispatcher's sha256 before compiling it, so
the "before" column of §3.1 is rebuilt from the reviewed artifact every run.
