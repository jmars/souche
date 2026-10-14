# PERF-3 — M1: the decode side of the uop cache, and the checker that pins it

**Unit:** `HANDOFF=v86c-perf`, unit PERF-3 (coder), **milestone M1 ONLY** — the emitter, the
decoder and the decode-checker selftest. **No executor, no cache, no invalidation, no
`--uop-cache` flag** (that is M2). **Inputs:** `/tmp/brief-v86c-perf-3.txt`, and
`results/perf-2-blockcache-plan.md` (the design of record) read in full, plus
`results/perf-1-inlining.md` (step 1).

Every claim below is tagged **MEASURED** (run on this host in this session), **READ** (from
source, with file:line) or **INTERPRETATION**; where a measurement contradicts the plan, the
disagreement is stated in §5 rather than coded around.

---

## 0. Bottom line

The decoder exists, is data-driven from the same `v86/gen/x86_table.js` the interpreter is
generated from, and is **pinned against the port's own interpreter by a differential checker
that runs from the same `make selftest`**:

| | |
|---|---|
| `gen/interpreter_uop.js` (new) → `src/cpu/uop_gen.c` (generated, 5558 lines) | 1182 descriptors, 924 prefix arms, 1922 call forms, **0 unimplemented**, **1 forced block end**; byte-reproducible |
| `src/cpu/uop_cache.{h,c}` (new, 623 + 259 lines) | the decode walk, the block framing, the decode-time modrm extraction and the execute-time materialisation — **no execution path** |
| `src/cpu/uop_cache_selftest.c` (new, 1381 lines) + `make selftest` | **7,934,349 checks, 0 failures**; 996 of 1024 dispatch cases; 1,555,069 instructions differentially checked; 1,426,777 address extractions compared against `resolve_modrm16/32` |
| the asymmetry rig (`results/perf-3-mut-rig.sh`) | **8 of 8 single-line decoder mutations caught**, control arm green, binary hash different per arm (proof the rebuild happened) |
| the shipped path | **UNCHANGED**: byte-identical `objcopy --strip-all` of `build/v86c.com.dbg` against the same tree built with every PERF-3 Makefile addition removed (G4e) |

The unit's own value, beyond the code: the differential **found a real decoder bug in its first
targeted use** (§5.1), the hand table **refuted four of my own hand-written expectations before
it accepted the code** (§5.4), and the plan's claim that the instruction table marks `sti` as a
block boundary is **false** (§5.2).

---

## 1. What was built

| file | lines | what it is |
|---|---|---|
| `gen/interpreter_uop.js` | 534 | the SECOND emitter pass: table → descriptor tables. Mirrors `gen/interpreter_c.js` structure (`gen_instruction_body` → the prefix-arm + `fixed_g` structure; `gen_instruction_body_after_fixed_g` → the per-entry descriptor), imports `v86/gen/x86_table.js` unchanged, and runs the SAME availability scan (regex and exclusions copied verbatim, plus the two new generated/selftest files excluded). `--dump` prints the case/arm/entry structure for a reader to check against `interpreter_gen.c`. |
| `src/cpu/uop_gen.c` | 5558 (generated) | `uop_base_cases[512]`, `uop_0f_cases[512]`, `uop_arms[]`, `uop_entries[]`, `uop_descs[]`, `uop_desc_names[]`. A pure function of the table, the port's `instr*` definitions and the emitter — re-running it reproduces it byte for byte (G1b). |
| `src/cpu/uop_cache.h` | 259 | `uop_t` (the plan §2.1 struct, extended with the fields the decoder needs to be checkable: `len`, `opcode_bytes`, `addr_kind`), `uop_block_t`, the descriptor/arm/entry/case types the generator emits, `UOP_STOP_*` framing reasons, and the interface. |
| `src/cpu/uop_cache.c` | 623 | the decode walk, the block framing, `dec_extract_modrm{16,32,sib}`, `uop_materialize_addr`, `uop_shape_of`. |
| `src/cpu/uop_cache_selftest.c` | 1381 | the M1 gate: the differential, the address check, the framing assertions, the hand table, the sweeps, the corpus walk. |
| `gen/nasm_corpus.py` + `build/nasm_corpus.bin` (build product) | 75 | the 123 corpus images' `.text` (10,893 bytes) packed flat, parsed from the ELF instead of shelling out to objcopy. |
| `results/perf-3-mut-rig.sh` | 127 | G3's rig. |
| `Makefile` | — | `src/cpu/uop_cache.c` joins `CPU_SRC` (so the shipped unity TU contains it and it gets per-file `-Werror` coverage); `uop_gen.c` joins `UNITY_SRC`; the generator rule; the corpus rule; the `cpu_uop_selftest` binary + its line in `selftest`. |

`src/cpu/modrm.c` is **NOT touched** by this unit — see §3.2.

---

## 2. What is decoded, and into what

### 2.1 The generated structure (and why it is data, not code)

`uop_gen.c` is the interpreter's own dispatch *shape* as data, three levels deep, in the order
`gen/interpreter_c.js` emits code:

1. `uop_case_t cases[512]` per table, indexed by the generator's own key
   `opcode | (is_osize_32() << 8)`, carrying `{first_arm, n_arms, reads_modrm}`. `reads_modrm`
   is `gen_instruction_c.js:358-366`'s `uses_modrm` — whether the interpreter fetches a modrm
   byte at all is part of the instruction's LENGTH, so the decoder must fetch exactly when the
   interpreter does (MEASURED: the first version of the sweep did not model this and the
   differential caught the resulting length divergence on the `fixed_g` groups whose only `e`
   encodings are `ignore_mod`).
2. `uop_arm_t arms[924]` — one per prefix group of a case, in the order the generated if-chain
   tests them (66, F2, F3) with the **fallthrough last**, `cond == 0` marking it. This is
   `gen_instruction_c.js:368-406` as data; the decoder's first matching arm is the generated
   if-chain's arm, including the no-check fallthrough (C-side difference 3).
3. `uop_entry_t entries[1182]` + `uop_desc_t descs[1182]` — one entry per (arm, `fixed_g`). A
   `g` an arm does not define has **no entry**, which is exactly the generated
   `default: trigger_ud(); break;` and therefore the decoder's `UOP_STOP_NO_DESC`.
   A descriptor carries the three body pointers (plain / mem / reg), the immediate kinds
   (`gen_read_imm_call`, `:115-130`), and the flags: HAS_MODRM, IGNORE_MOD, FIXED_G,
   FETCH_INT (`custom_modrm_resolve`), TS_GUARD, HAS_IMM, HAS_IMM2, BLOCK_END.

`fn_*` is the port's own `instr_XX_*` symbol, or **NULL** where the port defines no body for
that form. **MEASURED: 1922 call forms, 0 with no definition** — the port is complete, so the
"per-opcode enablement lever" (plan §2.2 item 2 / §7) has no members in this tree *at the
form level*; the lever is live only through the generated `fixed_g` default, and the checker
exercises exactly that (framing test 7).

### 2.2 The uop

The plan §2.1 struct, field for field, plus what a *checkable* decoder needs:

```c
fn, shape, pfx, seg, base, index, scale, g, flags, ts_guard, disp, imm, imm2, eip
+ len            /* the total instruction length -- the field the checker's (a) compares */
+ opcode_bytes   /* 1, or 2 when the 0F escape was consumed */
+ addr_kind      /* NONE / 16 / 32 / SIB -- the form uop_materialize_addr switches on */
```

`off` is the **opcode byte's** offset within the block (the plan's field), so for a prefixed
instruction `off > eip - block_start`; the checker asserts `off == eip - block_start + pfx_len`
rather than the naive equality, which is what caught that distinction (a `0x66` sweep stream
whose first uop starts at offset 0 but whose opcode is at offset 1).

### 2.3 The block

Plan §2.2's four ends, in the decoder (`uop_decode_block`):

| end | MEASURED (checker counts) |
|---|---|
| `block_boundary && !no_block_boundary_in_interpreted` | 8056 blocks |
| the virtual page end | 1 block |
| the cap (64 uops / 240 bytes) | 23630 blocks |
| no descriptor (incl. the `fixed_g` default) | 263 streams |

and it **continues through** jcc/jmp/loop (`no_block_boundary_in_interpreted`) and ordinary
modrm instructions — asserted explicitly by the framing tests (test 4: `74 04`, four nops and
`EB 02` are five uops and the block keeps going).

---

## 3. Faithfulness: what the decoder mirrors, and the two divergences

### 3.1 Mirrored, with the site

| rule | site |
|---|---|
| byte order: modrm byte, then sib/disp, then the immediate, then an extra immediate | `gen/interpreter_c.js:236-291`, and the MEASURED divergence that comment exists for (`66 c7 05 10 10 10 00 34 12`) |
| case key `opcode \| (is_osize_32() << 8)`, with the operand size computed from the prefixes accumulated SO FAR | `modrm.c:16`, `gen/interpreter_c.js:446-451` |
| the 0F escape keys its second byte the same way | `instructions.c:182-194`, `instructions_0f.c:75-76` |
| the prefix bytes and their accumulation (segment prefixes REPLACE the 3-bit field; the rest OR their bit) | `instructions.c:66-70`, `:606-627`, `:1880-1918` |
| prefixes are cleared after every instruction (`prefixes == 0` at the top of the loop) | `cpu.c:116`, `:127` |
| the address size comes from `is_asize_32()`, so the 16/32-bit form and the SIB form follow the 67 prefix, not the table's `os` | `modrm.c:567-574` |
| every 16-bit form except `0x06` masks its offset to 16 bits BEFORE the segment base is added | `modrm.c:198-333` |
| the default segment is DS except the BP/EBP-based forms (SS), and the SIB forms choose their own | `modrm.c:231/237/249/255/301/308/423/430`, `:469-517` |
| `read_moffs` follows the address size | `misc_instr.c:198-208` |

### 3.2 Divergence 1: `src/cpu/modrm.c` is NOT split (plan §8 step 3 not taken)

The plan's step 3 splits `resolve_modrm32/16/sib` into a decode-time extraction and an
execute-time materialisation, so that ONE copy of the addressing semantics serves both the
interpreter and the cache. **This unit did not take that step**: the extraction lives in
`uop_cache.c` as a second copy, and `modrm.c` is byte-identical to the plan's §10 hash
(MEASURED, G5).

Why: M1 is a decode-only milestone whose whole point is to leave the shipped interpreter
untouched; the brief marks the split optional ("*if you take it*"), and taking it would put a
rewrite of the hot addressing path into the same unit as the new decoder, with `make nasm`
plus the transcript diff as the only structural gate on it. What replaces the missing
*by-construction* equality is a measured one: the checker compares
`uop_materialize_addr(extracted)` against `modrm_resolve(modrm_byte)` — the interpreter's own
entry point — for **every memory operand it decodes**: 1,426,777 comparisons (356,307 16-bit,
1,053,569 32-bit, 16,901 SIB), 0 failures, with DS and SS given **different segment bases**
(0x10000000 / 0x20000000) so the default-segment choice of every form is part of the
comparison, and with every address out of guest RAM so a wrong choice cannot be masked by the
memory model. **M2 should still take step 3** and delete this copy (§7).

### 3.3 Divergence 2: a page-crossing instruction is not part of a block

Plan §2.2 makes an instruction whose bytes cross the page end the block's last uop, registered
in both physical pages. **This decoder ends the block BEFORE such an instruction** (it is not
in the block; the interpreter runs it).

Why: decoding the next page's bytes needs that page's translation, and the port's only
translation entry point is `translate_address_fetch`, which on success writes page-table
accessed bits and on failure DELIVERS a fault (`paging.c:307+`). A decode-time peek at page B
would therefore be guest-observable in a way the reference's execute-time fetch is not, and
the decoder would have to reproduce the reference's exact fault order for every crossing
instruction. The cost of the conservative choice is one fallback per page-crossing instruction;
the benefit is that the decoder reads nothing but the page it was entered on, cannot fault,
cannot deliver, and cannot change state.

Two consequences worth stating:
* the plan's §4.4 residual ("the decoder never caches a block whose crossing uop is
  guard-flagged") **does not exist in this design** — no crossing instruction is ever cached,
  guard-flagged or not;
* the block is therefore always inside ONE virtual page, hence inside one physical page, hence
  `blk.phys_len <= (virt_start | 0xFFF) + 1 - virt_start` (asserted).

### 3.4 Divergence 3 (forced end, and a plan-vs-table disagreement): `sti` — see §5.2

---

## 4. The M1 gate: the decode checker

`src/cpu/uop_cache_selftest.c`, built and run by the same `make selftest` as the other suites
(the M1c/M2-1 precedent of a separate binary: it needs the REAL `run_instruction`, the real
fetch path and the real `exceptions.c`, which `cpu_selftest.c`'s recording stand-ins cannot
coexist with). It supplies its own `io_port_*` bridge and a `run_hardware_timers` stub, and
deliberately does **not** link `src/hw` (§5.6).

### 4.1 (a) the length differential

Per decoded uop, the checker runs the interpreter for exactly ONE dispatch at the same eip, on
the same bytes, in the same mode — `interpreted_run`'s own three lines (`cpu.c:111-126`) — and
compares. Because a single dispatch's advance is the next instruction's start only when the
instruction did not transfer control, the rule set is:

| observation | verdict |
|---|---|
| `advance == decoder_len` | **PINNED** |
| `advance == the rel-branch's own target` | **PINNED_REL** — the target is computed from the RAW bytes by hand (rel8/rel16/rel32 with the port's `jmp_rel16` 16-bit mask from `misc_instr.c:58`), so it is independent of the decoder's `len`/`imm` |
| an exception was delivered, or a far transfer moved CS | counted (`6586`), not compared — the advance is a handler address |
| the interpreter stopped at decode time (a qualified symbol) | the decoder must have NO descriptor: **0** occurrences |
| the interpreter stopped for a RUNTIME reason (a sentence: an unclaimed port, an invalid `iret` frame) | counted (`48`, 8 distinct names), not compared — the machine stopped for a sandbox reason |
| `advance` outside [1,15] | **UNPINNED** — a register/memory/stack value decided where execution went, and the encoding must be on the named list below (`201`), else it is a FAILURE |
| `advance` inside [1,15] and ≠ `decoder_len` | **FAILURE**, unless the encoding is on that list (an unconditional `jmp`'s target can land in the window) |

Every probe runs **twice**, with the arithmetic flags clear and then set (CF|PF|AF|ZF|SF|OF,
nothing else), so each of the sixteen conditions is exercised on its not-taken side too; either
side being decisive pins the length. The flags choice is measured, not assumed: the first
version used `0xFFFFFFFF`, which sets FLAG_VM, puts the sandbox in vm86 mode and aborts at
`instructions.c:1140` (`instr32_9A`'s 16-bit `new_ip` check).

**The named "unpinnable" list** (encodings whose advance is a register/memory/stack value, so
no single-dispatch differential can pin their length): `ret`(C2/C3), `retf`(CA/CB),
`iret`(CF), `int3`(CC), `int  n`(CD), `into`(CE), `callf`(9A), `jmpf`(EA), `FF /2 /3 /4 /5`
(indirect/far call and jmp), the four `sysenter/syscall/sysret/sysexit` entries, and
**`sti`(FB)** — which is special (its body nests a dispatch) and is length-checked by the hand
table instead. A probe the differential cannot pin whose encoding is absent from that list is
a FAILURE with the byte stream printed; it never silently becomes a skip.

### 4.2 (b) the extraction vs the port's own resolver

For every memory operand: `uop_materialize_addr(uop)` against `modrm_resolve(modrm_byte)` run
from the same cursor, under the same registers and the same prefix byte. **1,426,777
comparisons, 0 failures** (§3.2). This is what makes up for not taking step 3.

### 4.3 (c) the block framing, asserted explicitly

seven fixture tests: the page-end stop and the exact set of uops before it; the crossing
instruction NOT being in the block; a block ending AFTER a `block_boundary` instruction and
carrying that flag; `jcc`/`jmp` NOT ending it; `hlt` ending it; `sti` ending it; the two caps
(64 uops on a page of nops, 240 bytes on a run of 6-byte instructions); and the `fixed_g`
default (no descriptor ⇒ the instruction is not a uop and the interpreter delivers the `#UD`
the generated `default` arm raises — asserted by running both).

### 4.4 Coverage (MEASURED, the counts the brief asks for)

```
dispatch cases decoded (base+0F) x (16+32 keys), of 1024: 996
  not reached: 00F 026 02E 036 03E 062 064 065 066 067 0F0 0F1 0F2 0F3 10F 126 12E 136 13E
               162 164 165 166 167 1F0 1F1 1F2 1F3
streams probed                     : 31954
instructions decoded and checked   : 1555069
  length PINNED by the interpreter : 1551551 (6829 via a rel-branch target)
  length unpinned                  : 402 (of which 201 named/allowed)
  no descriptor (block falls back) : 263 (interpreter agreed: 263)
  decode-time stops with a descriptor (must be 0): 0
  runtime stops (sandbox-caused)   : 48 (8 distinct)
  io ports reached by probes (stubbed): 164384 reads, 164384 writes
  probes skipped (body is DBG_ASSERT(false) in this build)       : 108
  probes skipped (16-bit + 66 + a control transfer; §5.3)        : 1
  probes whose advance was not a length (delivery/far transfer)  : 6586
  blocks produced                  : 31691 (block-end 8056, page-end 1, cap 23630)
(b) address comparisons vs modrm_resolve: 1426777 (failures 0)
    forms: 16-bit 356307, 32-bit 1053569, sib 16901
uop_cache_selftest: 7934349 checks, 0 failures
```

The 28 unreached cases are all structural: 24 are the **prefix-byte cases** (`26 2E 36 3E 64
65 66 67 F0 F2 F3` and `0F`, both keys), which the decoder folds into the instruction it
prefixes rather than dispatching — the interpreter DOES dispatch them, but a uop table has no
reason to (the plan's design: the prefix chain is decode-time), and 4 are `0x62`/`0xF1`, whose
bodies are `DBG_ASSERT(false)` in this build (§5.5). The coverage line names them rather than
rounding to "1000 of 1024".

**What the sweeps are.** Every base and 0F opcode × {16, 32} × 13 modrm forms (five memory
forms including all four SIB shapes and the eight values of the `reg` field — §5.7); all 10
prefix bytes and 19 prefix combinations × both tables; all 256 SIB bytes × 4 mod values, plus
every misaligned byte offset of those streams; the hand table (§4.5); and the 123 nasm corpus
images' `.text` walked instruction by instruction by the decoder's own boundaries (3045
instructions).

### 4.5 The hand table

41 hand-written entries carrying a hand-written expected length and, where the encoding has a
memory operand, the expected `addr_kind`/base/index/scale/seg/disp — the forms the sweeps
cannot pin: the immediate widths in both sizes, `moffs`, both `callf` sizes, all eight 16-bit
addressing forms (with the SS default of the BP forms and the sign-extended disp8), the 32-bit
forms (absolute disp32, EBP/SS, the SIB shapes), six prefix combinations, `lea`'s
`custom_modrm_resolve` form, and the decoder-only entries for the encodings whose bodies assert
in this build.

**It refuted four of my expectations on its first run** and the decoder was right on all four
(§5.4).

### 4.6 The corpus

`build/nasm_corpus.bin` = the 123 images' `.text` (10,893 bytes), a build product of
`gen/nasm_corpus.py`, which depends on the images when they exist and on `nasm-translate`
when they do not (so a from-scratch `make` has no unsatisfiable prerequisite). This is the
brief's "reuse the nasm gate's build dir", not a second copy of the corpus.

---

## 5. Findings — every divergence from the plan, with its measured reason

### 5.1 A real decoder bug, found by the differential in its first targeted use

The first version of the walk kept ONE prefix accumulator across the block and did not clear it
between instructions. The reference clears `cpu->prefixes` after every iteration
(`cpu.c:127`'s `DBG_ASSERT(cpu->prefixes == 0)`), so a prefix leaked into the next
instruction and flipped its operand size. The checker reported it as
`(a) reg-form: opcode 0x05 at 0x4008 (is32=0 pfx=0x20 pfx_len=0): interpreter advanced 3,
decoder says 5` — the byte stream's 9th byte is `05 A8 00 10 00`, and a leaked `66` makes the
decoder read an `imm32` where the interpreter reads `imm16`. Fixed by accumulating per
instruction, with the failure recorded in the comment (`uop_cache.c`, the walk). The mutation
rig reproduces the bug deliberately (arm 3) and the checker catches it.

**This is the asymmetry the unit needed**: the checker bit on a real defect before any
mutation was written.

### 5.2 The plan is WRONG about `sti` (reported loudly, as the brief asks)

Plan §5.1: *"`sti` … **It must be a block end** … **It is: block_boundary, no no_bib** — and
step 3 adds it to the decoder's forced-end set with a comment, because it is the one entry
where the table flag and the cache's need coincide."*

**MEASURED: the table does not mark it.** `v86/gen/x86_table.js:329` is

```js
{ opcode: 0xFB, custom: 1, custom_sti: 1, skip: 1 },
```

— no `block_boundary`. So the forced end is not belt-and-braces over a table flag; **it is the
only thing that makes `sti` a block end at all**. An implementation that trusted the plan's
sentence and added nothing would cache a uop after `sti` and execute the shadow instruction
twice (plan §5.1's own hazard). The emitter therefore forces `UOP_DESC_BLOCK_END` from the
table's `custom_sti` marker (the flag that marks exactly that body, and the only encoding that
carries it — `run_instruction` callers are `cpu.c:119`, `instructions.c:68/182/188/2166`), and
the emitter prints how many ends it forced (1). The framing test 5b asserts it.

### 5.3 A class of PORT (upstream-inherited) debug asserts fires on guest-reachable bytes

`instructions.c:1438, 1451, 1469, 1539, 1549, 1559, 1802, 1813, 1831, 1837, 2200, 2230, 2279,
2312` all assert `cpu->is_32 || value < 0x10000` — upstream's `debug_assert!(*is_32 || …)`
verbatim (`instructions.rs:1319, 1326, 1339, 1404, 1412, 1420, 2060, 2069, 2086, 2091, 2390,
2412, 2447, 2474`). They encode the assumption that **in a 16-bit code segment no ip/data
value ever leaves 16 bits**, which a 66-prefixed (32-bit operand size) control transfer
falsifies by construction.

**MEASURED**: the byte stream `66 E8 24 78 56 34` with `is_32 == false` aborts the checker at
`instructions.c:1802` inside `run_instruction` (the first version of the sweep hit it). The
pinned npm reference is a RELEASE build, where `debug_assert!` is compiled out, and executes
those bytes — so the semantics under test are the ones after the assert, and the checker skips
the ORACLE for exactly that combination (a 66-prefixed encoding of the transfer class in
16-bit mode: **1 probe** in the whole run), while still checking the decoder side. Nothing else
is affected.

**Stated as a finding about the port, not about this unit**: the shipped `build/v86c` is built
with assertions ON (no `NDEBUG` anywhere in the Makefile), so those 14 sites are live in the
shipped interpreter and a 16-bit-mode guest reaching a ≥64 KiB ip/data value aborts the
emulator where the reference carries on. Whether that should be fixed (mask instead of assert,
or build the shipped APE with `-DNDEBUG`) is a decision for the orchestrator — this unit does
not change the interpreter.

### 5.4 The hand table's first run refuted four of MY expectations (the decoder was right)

Recorded because it is the failure mode a hand table exists to have, and because "the checker
was wrong, the code was right" is worth as much as the reverse:

| my expectation | the encoding |
|---|---|
| `sib 0xA5` used `ESP` as an index and base | the index field `4` means **no index**, and base field 5 with `mod == 00` is a **disp32 base** (`modrm.cc:484-495`) — the decoder said base `NONE`, index `NONE`, and the port's own resolver agreed |
| `sib 0xA5` with `mod == 01` used ESP as base and index | base 5 with a displacement is **EBP with SS** |
| `F0 66 81 /0 imm32` is 8 bytes | the 66 prefix makes the immediate **16-bit**: 6 bytes |
| `0F 7F /0` with modrm `00` is 4 bytes | the modrm `00` is the memory form with **no displacement**: 3 bytes |

All four were corrected in the hand table, with the reason in the entry's note.

### 5.5 Two encodings whose BODY is `DBG_ASSERT(false)` in this build

`0x62` (`bound`, `instructions.c:569/575`) and `0xF1` (`int1`, `:1901`). The reference release
executes them (its `dbg_assert!` is compiled out); this suite's binary does not, because the
project's selftests run with assertions on. The checker therefore skips the ORACLE for them
(**108 probes**), detects them from the DECODER's own opcode position (so it also holds for a
misaligned probe: **MEASURED**, sweeping every SIB byte produces a stream whose *data* byte is
`0x62` and whose instruction is `bound`), and covers their decoder side in the hand table
(a descriptor, the length, and the address check — none of which runs a body).

### 5.6 The device layer's unclaimed-port stop, and why the suite does not link `src/hw`

With `src/hw` linked, the sweep stopped at
`io_port_read8(port=0x3000) (no handler installed for this port at this width)` — the device
layer's stop for a port no device claims (`src/hw/io.c:266-286`), reached by any `in`/`out`
probe. That stop is a property of the machine, not of the decoder, and it ends a probe inside
its own instruction. The suite therefore supplies its own benign, counting `io_port_*` bridge
(the `cpu_selftest.c:995` precedent) and links only the core: **164,384 port reads and
164,384 writes** were absorbed by the stubs during the run.

### 5.7 The sweep's own coverage hole, found by the coverage counter

The first version of the opcode sweep used six modrm forms whose `reg` field was **0 in all
six** — so for every `fixed_g` encoding whose bodies are not g = 0, no body was ever decoded.
The coverage line said so (995 of 1024) and named the casualty: the `0F C7` group, whose
entries are g = 1 and g = 6, was **never decoded once**. The forms now cover all eight `g`
values. This is the "the checker must be strengthened, not widened" case, caught by a counter
rather than by a judgement.

### 5.7b A measurement that lied, and how it was caught (worth recording)

The first attribution run (4 rounds, interleaved) read the files-IN-the-unity-TU arm as **+1.9%
slower** with the sign consistent in 4 of 4 pairs, which looked like a decision. It was not:
that run's host load was climbing 9.0 → 10.3 *through* the block and the files-IN binary was
**always the second measurement in each pair**, so the pairing "controlled" for drift only if
the drift were symmetric — and a 6-round repeat reversed the sign (−0.2%, i.e. inside the
noise). The decision was then re-grounded on an identity proof instead of on a difference
(§6, G4e). **The lesson is perf-1's, re-learned**: a consistent sign in an interleaved block is
evidence *only* if the order inside the pair alternates or is irrelevant — a fixed order plus a
drifting host manufactures it.

### 5.8 Related: `enter16` and the value asserts, and why the sandbox's registers are small

With registers pointing above RAM (`0x7F001000`-ish), `enter16` pushes `ESP - 2`
(`misc_instr.c:559/570`) and trips `safe_write16`'s `value < 0x10000` assert
(`paging.c:658`), and `mov cr3, r` trips `paging.c:425`'s alignment assert. Both are the
port's debug checks meeting a sandbox value the reference's Uint16 store would have truncated
silently. The checker's registers are therefore **small and 4 KiB-aligned** (0x1000·k), while
every address stays out of guest RAM because the SEGMENT BASES are what put it there
(DS/SS/ES/FS/GS at 0x10000000+). No probe can write guest RAM at all, which is why the checker
restores only the cpu state between probes. Recorded because it is a live trap for M2's
executor tests: a sandbox that hands the port large register values will trip these asserts and
they are not decoder defects.

---

## 6. Gates

*(raw evidence: `results/perf-3/gates.txt`, the suites' full output
`results/perf-3/selftest.txt`, the mutation rig `results/perf-3/mut-rig.txt`)*

### G1 — a scratch build, clean under `-Wall -Wextra -Werror`; `uop_gen.c` byte-reproducible

**MEASURED**: `make clean && make -j16` → `rc=0` in 17 s, and the build log's ONLY diagnostic
is the pre-existing minibios `ld: warning: cannot find entry symbol _start` (1 occurrence, the
same note perf-1 records). `uop_gen.c` is byte-reproducible: re-running `gen/interpreter_uop.js`
leaves it unchanged (`(unchanged)`), and after DELETING the file the generator rewrites it to
the identical `sha256 161a23a8f44c…`. **G1c**: `make uop-unity` builds the 20-source unity TU
(the step-1 18 + `uop_cache.c` + `uop_gen.c`) with `$(CPU_CFLAGS) -Werror` → `rc=0`, so the new
files fit the one translation unit the shipped build uses — while `make unity-src` prints the
shipped TU's own 18-source list, i.e. the two new files are NOT in it (§6, G4e).

### G2 — the M1 decode checker: rc 0, with counts

**MEASURED: rc 0, 7,934,349 checks, 0 failures**, counts in §4.4.

### G3 — ASYMMETRY: 8 of 8 single-line DECODER mutations caught

`results/perf-3-mut-rig.sh`, one line of `src/cpu/uop_cache.c` per arm, the checker binary
REMOVED and rebuilt per arm (the recorded sha256 differs per arm, so no stale object answered
for a mutation):

| arm | mutation | result |
|---|---|---|
| 0 | control: the shipped decoder | green (rc 0) |
| 1 | `dec_read16` consumes one extra byte | caught, rc 1 — `uops[2].len == 5` |
| 2 | `0x66` is not a prefix byte | caught, rc 134 — `u->len == h->want_len (got 0x1, want 0x4)` |
| 3 | the prefix accumulator leaks across instructions (5.1's bug) | caught, rc 1 — the (a) divergence |
| 4 | a SIB base of EBP with a displacement uses DS instead of SS | caught, rc 1 — `u->seg == h->want_seg (got 0x3, want 0x2)` |
| 5 | SIB index field 4 (no index) becomes an index | caught, rc 1 — `u->index == h->want_index` |
| 6 | a `block_end` instruction no longer ends the block | caught, rc 1 — `n == 3 (got 0x40)` |
| 7 | a `fixed_g` memory form gets the register form's shape | caught, rc 1 — the shape/addr-kind cross-check |
| 8 | the decode window is two pages wide | caught, rc 1 — the page-end framing |

Arm 7 is the one that needed the checker strengthened: with only the (b) comparison, a mutated
`uop_shape_of` **silently drops every `fixed_g` memory operand out of the comparison** (the
checker dispatches on the uop's shape). The cross-check
`is_mem_shape(shape) == (addr_kind != UOP_ADDR_NONE)` closes it, and arm 7 is red because of it.

### G4 — no regression, no behaviour change

| gate | result |
|---|---|
| `make selftest` | **rc 0**; `cpu_selftest 3522 checks, 0 failures`, `cpu_m1c_selftest 228`, `hw_selftest 541 checks 0 failures 0 skipped` — **the brief's counts, unchanged** — plus the new `uop_cache_selftest 7,934,349 checks, 0 failures`; **0 FAIL lines anywhere** |
| `make nasm` | **rc 0**, 118/118 passed, 0 of 123 produced no state, **the same 5 skips** (`tests/nasm_skip_c.txt`) |
| `make transcript-diff` | **rc 0** — "the C transcript is byte-identical to the reference's" |
| `make bench` (7 runs, shipped binary) | **best 4.453 s / median 4.541 s**, 44.93 MIPS, all 7 runs at 200,095,214 instructions, exit 0 — measured at **load 12.1 (before) / 15.2 (after)**, against perf-1's 3.93–4.03 s at load 1.2–3.4 |
| `make bench` on the same host, earlier in this session (load ~9) | 4.392 / 4.455 s — i.e. the absolute number tracks the host's load, not this unit |
| `make uop-unity` | rc 0 (G1c) |

**The absolute bench number is the host, not a regression** — and that is not an excuse, it is
the reason the unit's cost was measured as an IDENTITY instead of as a timing:

### G4e — the shipped binary is BYTE-IDENTICAL to the same tree without this unit's Makefile additions

`results/perf-3/attribution.sh`, PART 1: reconstruct the pre-unit Makefile (every PERF-3
addition removed — the diff is printed), build the same tree with it, and compare the two
shipped binaries with `objcopy --strip-all` (the perf-1 §2b technique, because `build/v86c`'s
own sha256 is not a code oracle: the APE's shell preamble carries a per-content heredoc token):

```
b8f247fb054445758c7ea33a9c58f4dd82048aa734dee2c59fe0ecbc30aafcb4  build/pre/v86c.com.dbg (stripped)
b8f247fb054445758c7ea33a9c58f4dd82048aa734dee2c59fe0ecbc30aafcb4  build/v86c.com.dbg     (stripped)
   IDENTICAL
```

The shipped TU's source list is the step-1 18 (`make unity-src`), the compile and link command
lines are unchanged, and the two binaries' code is identical to the byte. **This unit does not
compile one instruction of the shipped path differently.**

### G4f — the arm the plan would have shipped: not resolvable, and why that is the honest answer

`results/perf-3/attribution.sh`, PART 2 moves the two files INTO the unity TU (the plan's
arrangement, what this unit first shipped, what M2 will do) and measures it interleaved:

**Four readings of this arm** (raw samples: `results/perf-3/samples.txt`; the full output of
the last in `results/perf-3/attribution.txt`):

| attempt | arm as built | n | median OUT | median IN | difference |
|---|---|---|---|---|---|
| 1 | shipped Makefile WITH both files vs WITHOUT (the version this unit first shipped) | 4 | 4.491 | *4.423* | **+1.9% for files-IN** — and this is the confounded one (§5.7b: fixed order, load climbing 9.0→10.3) |
| 2 | half-arm (only `uop_gen.c` — the sed for `CPU_SRC` had not applied; the rig prints the arm's diff, which is how it was caught) | 6 | 4.419 | 4.409 | −0.21% |
| 3 | same half-arm | 6 | 4.325 | 4.333 | +0.18% |
| 4 | the correct arm (`uop_cache.c` into `$(CPU_SRC)`, `uop_gen.c` into `$(UNITY_SRC)`; 4 references) | 4 | 4.351 | 4.340 | −0.25% |

**Verdict: NO SIGNAL at this host's load (8–15).** Two signs, magnitudes 0.2–1.9%, and the only
large one came from the run whose ordering confounded it. So the choice was made on bloat and
rebuild time (750 lines of decoder plus ~50 KB of tables and 1182 name strings in the shipped
TU, for code nothing calls in M1), not on a speed difference this host cannot resolve — and the
M1 shipped binary is provably identical either way, because the files are out. **M2, which
makes something call the decoder, should move them in and re-run this arm**; the rig is here
for it.

### G5 — the plan's §10 hashes

**MEASURED**: every file in the plan's §10 list still has the hash the plan recorded, with ONE
exception — the `Makefile` (planned `45b1dfa60f35`, now `93aa54348c3d`), which is exactly the
file PERF-1 changed (the unity rules) and this unit changed (the generator, the corpus and the
selftest wiring). So: every file this unit was built ON is the current one, and the plan's
hashes are otherwise still valid at this commit — the plan's warning that PERF-1 might move
them was not borne out for the core sources.

| file | plan | now |
|---|---|---|
| cpu.c | 42668edf380d | 42668edf380d |
| cpu.h | 15916226b0c4 | 15916226b0c4 |
| cpu_state.h | 05dbf8ce2628 | 05dbf8ce2628 |
| memory.c | 9ca61a23817a | 9ca61a23817a |
| paging.c | 949942c6b762 | 949942c6b762 |
| modrm.c | 48c5e9bd2163 | 48c5e9bd2163 |
| modrm.h | 351b15f2a4dc | 351b15f2a4dc |
| instructions.c | bcab21bad3ae | bcab21bad3ae |
| instructions_0f.c | 1dbad114ef66 | 1dbad114ef66 |
| string.c | 70f7f84193ad | 70f7f84193ad |
| cpu_state.c | 5b46e29f2198 | 5b46e29f2198 |
| interpreter_gen.c | ba3ed9f6920f | ba3ed9f6920f |
| gen/interpreter_c.js | c01a320897b8 | c01a320897b8 |
| v86/gen/x86_table.js | 272218337ad6 | 272218337ad6 |
| results/cpu-port-plan.md | 7b585fdeca61 | 7b585fdeca61 |
| **Makefile** | 45b1dfa60f35 | **93aa54348c3d (MOVED)** |

### 6.5 the raw evidence

| file | what |
|---|---|
| `results/perf-3/gates.txt` | the full gate run: G5's hashes, G1's scratch build, G1b's generator reproducibility, G1c's unity fit, G2/G4a's suites, G4b's nasm, G4c's transcript diff, G4d's bench |
| `results/perf-3/selftest.txt` | `make selftest`'s complete output (all four suites, 0 FAIL lines) |
| `results/perf-3/attribution.txt` | `results/perf-3/attribution.sh`: PART 1 the pre-unit identity proof, PART 2 the files-IN arm |
| `results/perf-3/mut-rig.txt` | the G3 asymmetry rig's output, per arm |

---

## 7. What M2 must still build

1. **The executor** (plan §2.3): the ~12 generic shape executors and the run loop — the
   artifact node `handoff-v86c-perf-plan-artifact-1` holds the skeleton, and `uop_t::shape`
   (with `uop_shape_of`) is the dispatch key it switches on. **The decoder hands it everything
   it needs**: `fn`, `shape`, `pfx`, `seg`/`base`/`index`/`scale`/`g`/`disp` (materialised by
   `uop_materialize_addr`), `imm`/`imm2`, `eip`, `flags`, `pfx_len`/`opcode_bytes` (for the
   `FETCH_INTERNAL` bodies).
2. **The cache and its key**: `blk.state` is already filled
   (`cpu->state_flags & CACHED_MASK_ALL`); the arena, the hash and the per-page registry are
   M2's.
3. **Move the two files into the shipped unity TU** — `$(CPU_SRC) += src/cpu/uop_cache.c`,
   `$(UNITY_SRC) += $(UOP_GEN)` — and re-run `results/perf-3/attribution.sh` with the arm the
   plan intends (this unit's §6 G4f is the baseline for it); the `uop-unity` target then becomes
   redundant with `make` itself and can go.
4. **Step 3's modrm split** (§3.2): take it, and delete the decoder's second copy of the
   addressing semantics — the extraction in `uop_cache.c` is a *duplicate* and M2 should be the
   unit that removes it, with this checker's (b) as the proof that the split is behaviour
   preserving.
5. **Invalidation** (plan §4), the SMC abort (§4.2), the `--uop-cache` flag, and the
   cache-on/cache-off byte-identical gates.
6. **Two traps this unit measured, so M2 does not have to**: the 16-bit `dbg_assert!` class
   (§5.3) and the value asserts a sandbox with large registers trips (§5.8) will bite any
   executor test that runs the interpreter on 16-bit-mode bytes; and the `sti` forced end
   (§5.2) is not in the table, so anything that re-derives block ends from `x86_table.js`
   alone will get it wrong the same way the plan did.
7. **M3**: the instrumentation, the loop fast path, the arena sizing — and the note that this
   decoder's cap is 64 uops / 240 bytes per block (§2.3).
