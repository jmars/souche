# PERF-FIX — the exact unity guard (PERF-1R's SHOULD-FIX) and the 14 guest-reachable asserts (PERF-3 finding 3)

**Unit:** `HANDOFF=v86c-perf`, unit PERF-FIX (coder). **Inputs:** `/tmp/brief-v86c-perf-fix.txt`,
`results/perf-1r-review.md` (full), `results/perf-3-uop-decode.md` §5.3 and §5.8,
`results/r3f-fixes.md` §1/§3, `results/r4f-fixes.md` §4, `handoff-v86c-perf-fix-1` (the full code of
the check the reviewer verified). **Files written:** `gen/unity_symbols.py` (new),
`gen/unity_c.py`, `Makefile` (guard/selftest wiring), `src/cpu/instructions.c`, and the rigs
`results/perf-fix-guard.sh`, `results/perf-fix-n3.sh` + `results/perf-fix-probe.asm`,
`results/perf-fix-unity_c-prefix.py`, `results/perf-fix-instructions-prefix.c.gz`.

**CONCURRENCY, STATED UP FRONT.** A second coder (the M2/PERF-3 unit) was editing
`src/cpu/{cpu.c,cpu.h,uop_cache.c,uop_cache.h,uop_gen.c,modrm.c,memory.c,paging.c,string.c}` and
the same `Makefile` for the whole of this unit's run, and its tree state moved under several
measurements below (the tree grew from 2552 to 2579 defined globals while this unit ran). Every
number is therefore labelled with the source set it was taken on; the gate rows that were blocked by
*its* in-flight state are recorded as such with the evidence that they are not this unit's, and the
final run is green on every gate. This unit touched none of those files.

---

## 0. Bottom line

| item | delivered | evidence |
|---|---|---|
| **1a** the exact check | `gen/unity_symbols.py` — defined-global parity **plus** the outside-reference preservation that makes the class claim true | `results/perf-fix-guard.sh` §4/§5: the review's formulation **PASSES** the cross-set shadowing, the delivered one **FAILS** it |
| **1b** the heuristic gate | `gen/unity_c.py --watch` (refuses a unit static whose name is defined/declared outside the unit) + a **corrected rationale** (the old one at `:19-28` was factually wrong) | the rig's §1 re-measures the compiler's in-unit verdicts; §6b shows the watch gate refusing an injected real-tree shadowing |
| **1c** the gate | `make unity-symbols` = `$(BUILD)/.unity-symbols-ok`, a prerequisite of **`make selftest`** (which also closes the unit's own "tested ≠ shipped" caveat) | real tree GREEN: `2579 per-TU / 2579 unity defined globals, identical sets; 54 of 54 outside references preserved`, and it runs inside a green `make selftest` (rc=0) |
| **2** the 14 asserts | dropped **at each site** in `src/cpu/instructions.c`, matching the release reference's codegen, with the class reasoning in the file header | `results/perf-fix-n3.sh`: with the reviewed pre-fix `instructions.c` the probe aborts at `:1802`/`:2279` (rc=134); without it, rc=0 and the transcript is **byte-identical to the pinned reference** |
| **2 (bonus)** | the removal is also **1.9 % faster** on the boot benchmark (measured in isolation; the tree's own 3.128 s also carries the concurrent unit's work) | interleaved, 6/6 pairs, 3.283 s → 3.222 s medians |

**THE REVIEW'S PRESCRIBED FIX IS WRONG ON CONTACT** — not in its diagnosis, which is right, but in
its *remedy*: the `nm --defined-only` **defined-global parity it specifies does not catch the class
it was proposed for** (MEASURED below, on the reviewer's own synthetic). The delivered check adds the
one comparison that does, and the parity is kept (it is a real, if weaker, invariant). This is the
fourth round in this project where a review prescription had to be corrected on contact (R1F, R3F,
R4F); the details are in §4.

---

## 1. ITEM 1 — the unity guard

### 1.1 What the review got right

* Two same-name file-scope **statics** are not what the old guard's own header said they were: every
  in-unit collision is a **hard error** under the shipped compiler, whatever the signatures are.
  RE-MEASURED here (`results/perf-fix-guard.sh` §1, cosmocc GCC 14.1.0, real files):

  | in-unit collision | verdict |
  | --- | --- |
  | `static fn` + `static fn` (same signature) | `error: redefinition of 'f'` |
  | `static fn` + global fn (same signature) | `error: redefinition of 'g'` |
  | `static obj` + global tentative obj | `error: non-static declaration of 'o' follows static declaration` |
  | `static obj` + global obj with initializer | `error: static declaration of 'p' follows non-static declaration` |

  So the old scan's in-set refusal is *diagnostics quality*, not a safety net — the old header's
  "if the types happened to agree the call would silently bind" is wrong.
* The class that **is** silent needs a definition OUTSIDE the unit plus a prototype visible to a
  later unit file, and it is invisible to a scan of `UNITY_SRC` alone.
* Wiring an exact check into `make selftest` is the right shape (the shipped binary is the unity
  TU; the selftests compile per-TU, so the unity TU had no structural gate).

### 1.2 What the review got wrong

**Its check, as specified, does not catch its own synthetic.** MEASURED (rig §4, and here on the
reviewer's own `/tmp/unityprobe` files):

```
cpua.c: static int rebind(int x) { return x + 1; } int ca(void) { return rebind(1); }
cpub.c: extern int rebind(int x);                  int cb(void) { return rebind(1); }
hwdef.c (OUTSIDE the unit): int rebind(int x) { return x + 1000; }

per-TU link (cpua.o cpub.o hwdef.o): ca=2 cb=1001   <- binds to the OUTSIDE global
unity   link (unitysynth.o hwdef.o): ca=2 cb=2      <- silently bound to the static
compiler on the unity TU: rc=0, no diagnostic

defined-global sets:  per-TU {ca, cb} == unity {ca, cb}   => the parity PASSES it
```

The reason is structural, not incidental: the rebind happens on the **undefined** side. Per-TU,
`cpub.o` carries `U rebind` and the link resolves it from `hwdef.o`; in the unity TU that reference
is *gone*. Both objects still *define* exactly `{ca, cb}`. No set of defined globals can see it, and
the parity is what the review's stored code (`handoff-v86c-perf-fix-1`) implements.

### 1.3 Delivered: `gen/unity_symbols.py` (the exact check)

```
unity_symbols.py --unity UNITY.o OBJ.o ... [-- OUTSIDE.o ...]
```

* **(A) defined-global parity**, both directions, exactly as the review specified (kept; it is a
  real invariant — a name defined per-TU and not in unity, or vice versa, is a red flag).
* **(C) outside-reference preservation — the comparison that closes the class.** Let
  `REF_OUT = undefined(Σ per-TU unit objects) − defined(Σ per-TU unit objects)`: the names the
  per-TU unit needs from *outside* the unit. Every one of them must still be referenced
  (`nm -u`) by `unity.o`. A name that vanishes was bound to something the unit defines — the
  silent rebind. Violations among names **the watched outside objects actually define** are FATAL
  (that is the hazard); a vanished toolchain name (libm/libc) is a **warning**, not a failure,
  because a legitimate compiler-side call elimination is possible there — MEASURED to matter: the
  injected real-tree shadowing (§1.5) also drops a `roundf` reference, and failing on that would be
  a false alarm.
* `--parity-only` keeps the review's formulation addressable (it is what the rig measures against);
  it is explicitly **not** the gate.

### 1.4 Delivered: `gen/unity_c.py --watch` and the corrected rationale

* `--watch FILE ...` (the other files of the link: `$(HW_SRC) src/main.c $(SF_SRC) $(CPU_HDR)
  $(HW_HDR)`) refuses a unit whose file-scope `static` name is DEFINED **or DECLARED** with
  external linkage in a watched file. Prototypes count: a header declaration is what gives a later
  unit file its external reference. It is the same heuristic shape as the in-set scan — it
  over-refuses, never under-refuses — and it is now a *build* gate, not only a script check.
* The header at `:19-28` now states the MEASURED truth (the table in §1.1, the cpua/cpub/hwdef
  scenario, and that the in-set scan is diagnostics-quality), and points at
  `gen/unity_symbols.py` as the exact check.
* The generator is otherwise unchanged; the pre-fix file is stored for the rig as
  `results/perf-fix-unity_c-prefix.py`, sha256
  `52346016517cdeb296e0900aca6e95b0dd9cd4be2cbed9579f86987abcb5cdce` — **the sha the reviewer
  recorded for `gen/unity_c.py`** on `handoff-v86c-perf-review`, so the "before" side of every
  asymmetry below is the reviewed artifact, not a recollection.

### 1.5 Delivered: the Makefile wiring

```make
UNITY_WATCH      := $(HW_SRC) src/main.c $(SF_SRC) $(CPU_HDR) $(HW_HDR)
UNITY_PERTU_OBJ  := $(patsubst src/cpu/%.c,$(BUILD)/cpu/%.o,$(UNITY_SRC))
UNITY_SYM_OK     := $(BUILD)/.unity-symbols-ok

$(UNITY_C): ... $(UNITY_WATCH)
	$(PYTHON) gen/unity_c.py --watch $(UNITY_WATCH) -o $@ $(UNITY_SRC)
$(UNITY_SYM_OK): $(UNITY_OBJ) $(UNITY_PERTU_OBJ) gen/unity_symbols.py
	$(PYTHON) gen/unity_symbols.py --unity $(UNITY_OBJ) $(UNITY_PERTU_OBJ) -- $(HW_OBJ) $(SF_OBJ)
unity-symbols: $(UNITY_SYM_OK)
selftest: ... $(UNITY_SYM_OK)
```

The per-TU object list is **derived from `$(UNITY_SRC)`** — the same list the generator is given —
so whatever joins the unit is covered without a second list to keep in sync. That mattered during
this very run: the concurrent unit added `src/cpu/uop_cache.c` to `CPU_SRC` and `$(UOP_GEN)` to
`UNITY_SRC`, and the check tracked both with no edit.

### 1.6 The evidence (G1/G2), raw

`sh results/perf-fix-guard.sh` → `results/perf-fix-guard.txt`.

**G1 — the asymmetry, all three guards side by side on the same synthetic unit:**

```
3. THE OLD GUARD (the pre-fix generator, no --watch) ON THAT UNIT
  rc=0 EMITTED  <- unity_c.py: 2 sources -> build/pf-guard/old.c
4. THE REVIEW'S CHECK AS FORMULATED (defined-global parity only, --parity-only)
unity_symbols.py: PARITY ONLY (PERF-1R's formulation): 2 per-TU / 2 unity defined globals, identical sets
  rc=0 EMITTED  <- the parity alone does NOT see the rebind
5. THE DELIVERED CHECK (parity + outside-reference preservation) ON THE SAME UNIT
unity_symbols.py: 1 reference(s) the per-TU unit resolves from OUTSIDE the unit are gone from the unity TU: ...
  outside-provided but not referenced by unity.o: rebind
  rc=1 REFUSED
  ASYMMETRY: the old guard and the review's parity check both pass it; the delivered check fails it.
```

**G2a — green on the real tree** (the shipped gate, `make unity-symbols`):

```
unity_symbols.py: 2579 per-TU / 2579 unity defined globals, identical sets; 55 of 55 outside references preserved
unity-symbols: per-TU and unity defined-global symbol sets identical, outside references preserved
```

(The counts move with the tree: measured at `2552 == 2552, 52/52` on the pre-M2 sources, then
`2579/2579, 55/55`, then `2579/2579, 54/54` — the **invariant** is what is green; the absolute
counts track how many files have joined the unit and how many outside names its code calls.)

**G2b — a deliberate cross-set shadowing in the REAL tree shape.** A `static int32_t
io_port_read8(int32_t)` (shadowing `src/hw/io.c:523`'s global) prepended to the real `UNITY_SRC`:

```
old generator on the injected list:  rc=0 EMITTED
NEW generator with --watch:          rc=1 REFUSED
  unity_c.py:   io_port_read8 (static) build/pf-guard/shadow.c:1
  unity_c.py:   io_port_read8 (outside) src/hw/io.c:523
the injected unity TU COMPILES SILENTLY under the shipped flags (-Werror)
the delivered check: rc=1 REFUSED, "outside-provided but not referenced by unity.o: io_port_read8"
```

The silent compile is the point: nothing but the delivered check says a word, and the fix lands
*inside the shipped build* rather than in a script somebody has to remember to run.

### 1.7 What the guard still does not cover

* A `static` **object or function inside a header** — headers are scanned by the watch list, but a
  macro-defined name (`#define X f`) is invisible to a line-oriented scan. The nm check is the
  backstop for anything the scan cannot see *in the built objects*, and it is exact for the class.
* **`src/main.c` is not in the `--outside` set**, because it has no object in the link: the shipped
  rule compiles it on the link line. So a future unit `static` shadowing a **main.c** global would be
  reported as a *warning*, not fatally. MEASURED to be an empty gap today: of the 54 names the unit
  resolves from outside, **none** has a definition-shaped line in `src/main.c` (the outside set the
  gate watches — `$(HW_OBJ)` + softfloat — covers all 26 non-toolchain ones). Adding a `main.o` rule
  is the fix if that changes.
* `results/perf-1/guard-asymmetry.sh` (a PERF-1 artifact, not mine to edit) still passes unchanged —
  including its "the shipped unity set emits" case. The fix-1 note asked for a synthetic *watch*
  case "there"; it exists instead in `results/perf-fix-guard.sh` §6b, with the real-tree injection.

---

## 2. ITEM 2 — the 14 guest-reachable `dbg_assert!` sites

### 2.1 The class, and why the sites exist

Upstream guards 14 control transfers with `dbg_assert!(*is_32 || value < 0x10000)`. Those are
`debug_assert!`s (`v86/src/rust/cpu/dbg.rs:63`), **COMPILED OUT of the pinned release reference**,
which therefore simply executes; the port emitted them as live `DBG_ASSERT` (= `assert` with no
`-DNDEBUG`), so guest bytes a 16-bit code segment can emit aborted the machine where the reference
carries on. MEASURED by PERF-3 and re-measured here.

**Resolution: dropped AT EACH SITE**, matching the release reference's codegen — the R3F/R4F
precedent, twice already on this exact class (R3's N1: the prefix-fallthrough assert; R4F's N3: the
`fxsave`/`fxrstor` alignment asserts). NOT `-DNDEBUG`: the port's own invariants must stay live, and
the observable is that every count and every skip is unchanged (§3). The reasoning lives in the file
header of `src/cpu/instructions.c` (the porting-rules list, which already carries the
`dbg_assert! → DBG_ASSERT` rule this class is the exception to) and each site carries a one-line
pointer to it.

### 2.2 The 14 sites, one decision each

`value` is the expression the site tests. "Reach" is the guest's ability to make the test false:
`MEASURED` where a probe exercises it, `DERIVED` where it follows from the operand's width. **No site
is load-bearing**: the value is USED as the new `instruction_pointer` (an address), never as an
index, length or shift count, and the release reference reaches exactly the same state.

| # | port site | function | upstream | `value` | reach | decision |
|---|---|---|---|---|---|---|
| 1 | `:1442` | `instr16_C2` (retn imm16, 16-bit) | `instructions.rs:1319` | `get_real_eip()` after `ip = cs + pop16()` | **dead** (DERIVED: a 16-bit read; `get_real_eip() == v < 0x10000`) | drop — provably inert |
| 2 | `:1451` | `instr32_C2` (retn imm16) | `:1326` | `ip` = `pop32s()` | reachable (DERIVED: `66 C2` in a 16-bit segment) | drop |
| 3 | `:1469` | `instr32_C3` (retn) | `:1339` | `ip` = `pop32s()` | reachable (DERIVED: `66 C3`) | drop |
| 4 | `:1539` | `instr32_CA` (retf imm16) | `:1404` | `get_real_eip()` after `far_return(ip32,…)` | reachable (DERIVED: `far_return` does NOT mask the eip for a non-32 target — `cpu.rs:1642`/`exceptions.c:1420`) | drop |
| 5 | `:1549` | `instr16_CB` (retf) | `:1412` | `get_real_eip()` after `far_return(ip16,…)` | **dead** (DERIVED: a 16-bit read) | drop — provably inert |
| 6 | `:1559` | `instr32_CB` (retf) | `:1420` | `get_real_eip()` after `far_return(ip32,…)` | reachable (DERIVED: as #4) | drop |
| 7 | `:1802` | `instr32_E8` (call rel32) | `:2060` | `get_real_eip()` after `ip += imm32s` | **reachable, MEASURED** — this is PERF-3's abort, and MODE=1 of the probe | drop |
| 8 | `:1813` | `instr32_E9` (jmp rel32) | `:2069` | `get_real_eip()` after `ip += imm32s` | reachable (DERIVED: `66 E9`) | drop |
| 9 | `:1831` | `instr16_EB` (jmp rel8) | `:2086` | `get_real_eip()` after `jmp_rel16` | **dead** (DERIVED: `jmp_rel16` masks with `& 0xFFFF`, `misc_instr.c:58`) | drop — provably inert |
| 10 | `:1837` | `instr32_EB` (jmp rel8) | `:2091` | `get_real_eip()` after `ip += imm8` | reachable (DERIVED: `66 EB` in a 16-bit segment) | drop |
| 11 | `:2200` | `instr16_FF_2_helper` (call near) | `:2390` | `get_real_eip()` after `ip = cs + data16` | **dead** (DERIVED: `read_reg16`/`safe_read16` are 16-bit) | drop — provably inert |
| 12 | `:2230` | `instr16_FF_4_helper` (jmp near) | `:2412` | `get_real_eip()` after `ip = cs + data16` | **dead** (DERIVED: as #11) | drop — provably inert |
| 13 | `:2279` | `instr32_FF_2_helper` (call near) | `:2447` | `data` (32-bit `r/m`) | **reachable, MEASURED** — MODE=4 of the probe | drop |
| 14 | `:2312` | `instr32_FF_4_helper` (jmp near) | `:2474` | `data` (32-bit `r/m`) | reachable (DERIVED: `66 FF /4`) | drop |

Five of the fourteen can never fire, so their removal is *provably* inert; the other nine can, and
for those the port now does what the release reference does. `DBG_ASSERT` count in the file: **28 →
14** (the remaining 14 are internal invariants, e.g. `far_return`'s `.unwrap()`s).

### 2.3 The probe (G3) and what it measures

`results/perf-fix-n3.sh` builds **two binaries from the same tree, differing only in
`instructions.c`** (the pre-fix file is substituted into the generated unity TU, so both legs are
full unity builds; the pre-fix artifact is gzipped in the repo, sha256 checked before use:
`bcab21bad3aeb943880bcbde247a694cf8a7b49f8e3b22bed9fa15601636324a`), plus the pinned reference.
The guest (`results/perf-fix-probe.asm`) leaves protected mode (`mov cr0`, PE cleared), takes a far
jump's REAL-MODE arm to selector `0xF010`, and so runs in a 16-bit code segment with a *legitimate*
small ip.

| MODE | what it is | BEFORE (pre-fix `instructions.c`) | AFTER (the tree) | AFTER vs reference |
|---|---|---|---|---|
| 0 | the 16-bit switch alone (control) | rc=0 | rc=0 | **IDENTICAL** (eip 0x100044, 9 instr) |
| 1 | **the brief's bytes** `66 E8 24 78 56 34` | **rc=134**, `instructions.prefix.c:1802: assert(cpu->is_32 \|\| get_real_eip() < 0x10000)` | rc=134 at **`cpu.c:100`** — the *next fetch*, out of RAM (see §2.4) | neither leg produced a transcript |
| 2 | the same opcode with `is_32` TRUE (control) | rc=0 | rc=0 | **IDENTICAL** (eip 0x100031, 5 instr) |
| 3 | the same instruction, `rel32 = 0` (value < 0x10000) | rc=0 | rc=0 | **IDENTICAL** (eip 0x10004a, 10 instr) |
| 4 | `66 FF D0` with `eax = 0x10100` — a **controlled in-image landing** (0xF0100+0x10100 = 0x100200) | **rc=134**, `instructions.prefix.c:2279: assert(cpu->is_32 \|\| data < 0x10000)` | rc=0 | **IDENTICAL** (eip 0x10022e, 27 instr) |
| 5 | a huge eip set entirely in **32-bit** mode (not one of the 14) | rc=134 at `cpu.c:100` | rc=134 at `cpu.c:100` | neither leg produced a transcript |

Three things follow, all measured:

1. the abort at the class's sites is gone (MODE=4: rc=134 → rc=0; MODE=1 leaves the site and stops
   for a different, later reason);
2. the state it then reaches matches the pinned reference **byte for byte** where the guest stays
   in RAM (MODE=4, and MODE=0/2/3 as the controls);
3. MODE=3 isolates the abort to the VALUE, not to the mode or the opcode: the same `66 E8` with a
   small immediate is rc=0 even **pre-fix** — which is what the condition says, and what PERF-3's
   reading claimed.

### 2.4 A named divergence, and a 15th site of the same class (NOT in my scope)

For the brief's exact bytes (MODE=1) the AFTER leg does not return to a comparable state: it
continues past the dropped assert and the **next fetch** (linear `0x3557785A`, outside the guest's
2 MiB RAM) hits `src/cpu/cpu.c:100`, `DBG_ASSERT(!in_mapped_range(phys_addr))` in `get_phys_eip`
— upstream's `dbg_assert!` at `cpu.rs:3295`, again a DEBUG assert the release reference does not
have. There, the reference does not "continue" either: it dies with an uncatchable wasm trap
(`RuntimeError: memory access out of bounds`, rc=1, no transcript). So on these bytes **both** legs
stop and the divergence is the *kind* of stop.

MODE=5 shows it is **pre-existing and unrelated to ITEM 2**: a 32-bit-mode guest that sets a huge
eip (no 66 prefix, no 16-bit segment, none of the 14 sites) aborts at `cpu.c:100` in **both** the
pre-fix and the post-fix binary. `cpu.c` belongs to the concurrent M2 unit, so this unit did not
touch it — it is reported here as the same class, one file over, with the same "guest input, not a
port invariant" reading and the same precedent (the port's `-DNDEBUG`-lessness makes it live).
A future unit that closes it should note that the reference has no graceful behaviour to match: it
traps.

### 2.5 No other stop was silenced — the observable

* `make nasm` **118/118**, 0 produced no state, **5 skipped** — unchanged, and the `jump` skip's
  reason still reads `src/cpu/cpu.c:83: assert(!in_mapped_range(phys_addr)) failed`, i.e. the stop
  the corpus depends on is still live. (A `-DNDEBUG` would have moved that count.)
* `make transcript-diff`: the C transcript is **byte-identical** to the reference's.
* The full suite counts: see §3 — the per-TU selftests are blocked by the concurrent unit's
  in-flight link (not by this change), and `unity_symbols` (this unit's addition) is green.

### 2.6 The bonus: 1.9 % faster, MEASURED interleaved

The 14 conditions were evaluated on every execution of those control transfers (a kernel boot takes
them constantly), and `assert(cond)` evaluates `cond`. Interleaved 6-pair comparison of the two rig
binaries (same sources, only `instructions.c` differs, order alternating):

```
pair 1 before 3.292  after 3.213      before best 3.273  median 3.283
... 6 pairs, sign consistent 6/6 ...  after  best 3.212  median 3.222
ratio of medians: 1.019
```

---

## 3. Gates (raw output: `results/perf-fix-gates-final.txt` = the final green run; `results/perf-fix-gates.txt` = the first pass, including the rows the concurrent unit's in-flight wiring blocked; `results/perf-fix-guard.txt`, `results/perf-fix-n3.txt`)

| gate | command | rc | result |
|---|---|---|---|
| G4a | `make` | **0** | generators unchanged (`1922 call sites, 0 unimplemented`; `1182 descriptors…`) and the link green |
| G4b | `make selftest` | **0** | **ALL SUITES GREEN**: `cpu_selftest: 3522 checks, 0 failures` · `cpu_m1c_selftest: 228 checks, 0 failures` · `hw_selftest: 541 checks, 0 failures, 0 skipped` · **`uop_cache_selftest: 7934349 checks, 0 failures`** (PERF-3's number) · `uop_exec_selftest: 31 checks, 0 failures` · and this unit's new prerequisite `unity-symbols` first: `2579 per-TU / 2579 unity defined globals, identical sets; 54 of 54 outside references preserved`. |
| G4b‡ | the same target, twice earlier | **2** | Both failures were the **concurrent unit's in-flight wiring**, and both are recorded because they are what the tree looked like while this unit was being verified: at 22:52 `build/cpu_selftest` failed to link (`undefined reference to uop_page_code / uop_page_code_len / uop_invalidate_page / uop_invalidate_range` from **their** `memory.c`/`paging.c`/`string.c`, their `SELFTEST_SRC` not yet listing their `uop_cache.c`); at 22:58 `build/cpu_uop_selftest` failed with `multiple definition of \`uop_run'` (that link line then named **both** the source and the object). A third run at 23:00 died with SIGBUS in `cpu_m1c_selftest` on a **stale** binary (mtime 23:02 — rebuilt under it). All three are outside this unit's files; reported on the bus. |
| G4b″ | `make unity-symbols` (this unit's addition, and a `selftest` prerequisite) | **0** | `2579 per-TU / 2579 unity defined globals, identical sets; 55 of 55 outside references preserved` — and `54 of 54` on the rig's later run; the counts track the tree, the equality does not move |
| G4c | `make nasm` | **0** | **118/118**, 0 produced no state, **5 skipped** (same list, same reasons; `jump` still `cpu.c:83`) |
| G4d | `make transcript-diff` | **0** | `milestone: the C transcript is byte-identical to the reference's` |
| G4e | `make bench` | **2** | **refuses**: `200095212 instructions, expected 200095214`. NOT this unit — the pre-fix leg reports `200095212` too (§4 finding 2) |
| G4e′ | `tests/perf_bench.sh --runs 7 --bin build/v86c --expect-instrs 200095212` | **0** | best **3.128 s** / median **3.158 s** = **63.97 MIPS** at load 8.9–10.9, 7/7 runs at the same count (an earlier identical run read 3.213/3.224 = 62.3 MIPS) — *faster* than the brief's 3.97 s baseline, not within noise of it (§4 finding 3) |
| G1/G2 | `sh results/perf-fix-guard.sh` | **0** | §1.6 (old guard + the review's parity both pass the shadowing; the delivered check fails it; real tree green; injected shadowing caught) |
| G3 | `sh results/perf-fix-n3.sh` | **0** | §2.3 table |

---

## 4. Findings on the brief and the review

1. **The review's check does not catch the class it was prescribed for** (§1.2, §1.6). The delivered
   check keeps the prescribed parity and adds the outside-reference comparison that does. The
   reviewer's *diagnosis* stands and its real-tree parity measurement reproduces exactly
   (`2552 == 2552` on the pre-M2 sources).
2. **`tests/perf_bench.sh`'s pinned instruction count no longer matches the tree.** Every binary
   built from the current tree — including the pre-fix `instructions.c` leg — reports
   `200095212`, 3/3 runs each, against the script's `EXPECT_INSTRS=200095214`. The pre-fix leg
   reporting the same number is the asymmetry test that clears this unit; the remaining candidates
   are the concurrent M2 unit (the only other difference from PERF-1's measurement) or the host.
   The bench was therefore run with `--expect-instrs 200095212` — the observed count, *not* a
   re-pin — and the constant is left for its owner to re-pin deliberately.
3. **The bench is now ~21 % faster than the brief's baseline** (3.97 s → 3.128 s best, 63.97 MIPS).
   This unit's share is MEASURED at 1.9 % interleaved (§2.6); ITEM 1 is build-time only. The rest
   is not attributable from here and should be claimed (or not) by whoever made it — the concurrent
   M2/PERF-3 unit is the candidate, and the orchestrator owns the final verification.
4. **A 15th same-class site exists at `src/cpu/cpu.c:100`** (`get_phys_eip`'s
   `DBG_ASSERT(!in_mapped_range(phys_addr))`, upstream `cpu.rs:3295`, a compiled-out `dbg_assert!`),
   reachable from ordinary guest bytes in **32-bit** mode (MODE=5, rc=134 in both legs). Out of this
   unit's write scope; reported with a probe.
5. **The brief's G3 wording is satisfiable only in part** for MODE=1: at the brief's exact bytes the
   reference does not "carry on" into a comparable state — its wasm traps on the out-of-RAM fetch.
   The fix's claim is therefore demonstrated at the class level (MODE=4, byte-identical transcript)
   and the MODE=1 divergence is *named* rather than papered over, which the brief allows.

---

## 5. What this unit did NOT prove

* **That the unity TU is now safe in general.** The delivered check is exact for *symbol binding*
  between the per-TU and unity compilations of the same source list. It says nothing about
  semantics that do not show up as symbols — macros, `__COUNTER__`/`__LINE__`, `static inline`
  ODR questions, or a header's own definitions (a header cannot be "outside the unit" for this
  purpose; `src/hw` is per-TU and therefore covered, headers are watched only by the heuristic).
* **That the outside set is complete.** `--outside` is `$(HW_OBJ)` + softfloat; a shadow of a
  `src/main.c` global would be a warning rather than a failure (§1.7 — an empty gap today, measured).
* **That the `--watch` scan is complete.** It is a line-oriented heuristic that over-refuses; the
  nm check is the exact backstop, and neither reads the *preprocessor*. A `#define X f` alias is
  invisible to both.
* **That the 14 sites are the class's whole inventory.** PERF-3 found them by grepping one
  condition; this unit did NOT sweep the tree for other live `DBG_ASSERT`s whose condition is a
  property of the guest's input. Finding 4 above is one such site, found by accident of the probe,
  and there may be more — the sweep has not been run.
* **That the nine reachable sites now match the reference on all inputs.** MODE=4 measures one of
  them (the FF /2 helper) end to end; the other eight are reasoned from the operand width and the
  reference's codegen, not probed individually.
* **That the 1.9 % is stable** beyond this host's load and these 6 interleaved pairs.
* **Anything about the concurrent M2 unit's correctness** — its files, its link wiring and its
  numbers are its own; the two red gate rows in §3 are reported, not reviewed.

---

## 6. How to re-run

```sh
sh results/perf-fix-guard.sh        # G1/G2: the guard asymmetry + the real tree (needs python3, cc)
sh results/perf-fix-n3.sh           # G3: the ITEM 2 probe, 6 modes x before/after/reference
make unity-symbols                  # the exact check on the real tree
make && make nasm && make transcript-diff
tests/perf_bench.sh --runs 7 --bin build/v86c --expect-instrs 200095212   # see §4 finding 2
bash results/perf-1/guard-asymmetry.sh                       # unchanged, still green
```

Artifacts: `results/perf-fix-unity_c-prefix.py` (the reviewed pre-fix generator, sha-checked),
`results/perf-fix-instructions-prefix.c.gz` (the pre-fix `instructions.c`, sha-checked),
`results/perf-fix-probe.asm`, and the raw logs `results/perf-fix-{gates,gates-final,guard,n3}.txt`
(+ `results/perf-fix-n3.err`, the shell's reports of the intentional rc=134 legs).
