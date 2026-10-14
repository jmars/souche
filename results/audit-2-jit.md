# AUDIT-2 — SECURITY: THE JIT, EXECUTABLE MEMORY AND THE GENERATED-CODE SURFACE

HANDOFF=v86c-audit, unit AUDIT-2 (reviewer, read-only). Tree: one clean commit `a49a887`,
working tree clean. Every file's sha256 is in §8; the two files that moved since
handoff-v86c-jit-review3 (jit_mem.c, uop_cache.c) carry the JIT-6F deletions and the two
JIT-6F one-liners recorded on the result node — nothing else moved.

## 0. VERDICT

**THE CENTRAL CLAIM SURVIVES the refutation attempt.** No emitted opcode, group digit,
condition code, or REX/modrm/SIB *encoding decision* is derived from guest-controlled
data. What the guest does choose — which register, which scale, which displacement,
which immediate, which segment — reaches the encoder only as (a) a selection among a
fixed 8-entry guest→host register map, (b) a masked 2-bit scale, (c) payloads (imm32 /
disp chosen by value), or (d) a bounded 0..5 index into `cpu_state`'s `[8]` segment
arrays. And the claim has a *structural backstop* the design does not state anywhere:
every load/store a compiled block can emit is gated to `mem8[phys]` with
`phys < memory_size` (always MiB-aligned) and no 4 KiB-page straddle, so **even a wrong
recipe could not emit an access outside guest RAM** (§1.4).

**NO BLOCKER. ONE SHOULD-FIX** (the JIT block-hash insert silently overwrites a slot once
binds saturate the table — reachable by adversarial SMC churn; wrong-answer-safe, but it
is silent data-structure corruption under an untrusted guest, and the proven fix exists
100 lines away in the same codebase). Four NICE-TO-HAVEs (§5).

## 1. THE CENTRAL CLAIM: what was attacked, and why it held

The full inventory of guest-derived uop fields (`uop_cache.h:118-147`) and where each
lands in the encoder, read against the decoder that fills them (`uop_cache.c:150-308,
481-618`):

| field (source) | encoder use | verdict |
|---|---|---|
| `fn` (descriptor table, keyed by opcode) | recipe lookup key only (`jit_codegen.c:1024`) | selection among hand-written recipes; opcode/digit/cc are table constants |
| `shape`, `addr_kind` | recipe gates (`:1029-1041`) | refusal conditions only; ADDR_16 refused (16-bit arithmetic differs) |
| `imm`, `imm2` (guest immediates) | `imm32`/`imm8` payloads (`:704,749,758,810`); `& 0xFF` where imm8 | payload only; the imm8 form's sign-extension IS the 83-group's x86 semantics |
| `disp` (modrm/sib bytes) | disp32/disp8 chosen by value (`jit_x64.c:128-141`) | payload; both forms encode the same value |
| `base`/`index` (3-bit sib/modrm fields or decode constants) | `jitcg_greg[base]` host-register selection (`:290-291`) | decoder emits only 0..7 or `UOP_REG_NONE` (`uop_cache.c:191-295`: constants 3/5/6/7 or masked `& 7`); selects within the fixed map {RAX,RCX,RDX,RBX,R12-R15} — never an encoding decision |
| `scale` (sib top 2 bits) | SIB scale field, masked `& 3` (`jit_x64.c:136`) | masked twice (decode `>>6 &3`, emit `&3`) |
| `seg` (DS/SS default or prefix) | `JC_OFF(segment_is_null)+seg`, `+4*seg` displacements (`:303-305`) | `seg ∈ 0..5`: `pfx & PREFIX_MASK_SEGMENT(0x07)` ⇒ 0..7, `SEG_PREFIX_ZERO(7)` early-returns, `pseg-1 ≤ 5`; arrays are `[8]` (`cpu_state.h:287-288`). Bounded by construction |
| `modrm & 7` | guest-register selection via `jitcg_greg[]` | masked; `& 7 > 7` impossible; the `IGNORE_MOD` gate at `:1035` is belt-and-braces |
| `g` (modrm >>3 &7) | `jitcg_greg[u->g]`, `write_reg8(u->g)` | refused `> 7` for every shape whose recipe reads it (`:1037-1041`); REGF/MEMF/PLAIN recipes use table `aux`, never `u->g` |
| `eip`, `len`, `body_ip` | imm32 payloads (`:1073-1074,1510-1511`) and C-side additions | payload |
| `pfx` | `any_pfx` emission toggle; segment-override decode | binary toggle; segment path bounded (above) |
| `flags` (descriptor-derived) | flag-word payload in RDI | payload |
| state byte (machine state) | cpl3 fold into the TLB mask (`:314`) | two-valued decision matching `translate_address`'s own `user` term; the state byte is part of the block key, so it cannot mismatch live state (§4) |

The opcode/digit/cc constants themselves: every `JC_R(...)` row (`jit_codegen.c:834-892`)
bakes `aux` as a literal; the per-encoding loops (`:896-991`) bake 0..7 and the six
supported ccs; `rec_jcc`'s `not_taken` cc is a compile-time function of that `aux`
(`:746`); `jitcg_emit_alu_rm`'s `digit == 7 ? 5 : digit` likewise (`:613`). The only
guest→recipe path is `u->fn`, and `u->fn` is the *descriptor table's* body pointer
(`uop_cache.c:551-570`) — the same pointer the interpreter calls. A wrong table row
would make the JIT and the interpreter wrong identically; there is no JIT-only opcode
decision. I cross-checked the 83-family rows (`uop_gen.c:308-315`, `UOP_IMM_8S`) against
the recipe digits (0/1/4/5/6/7 == add/or/and/sub/xor/cmp) — the x86 group-1 digit
convention, consistent with the generated names.

Encoder hardening observed on the way: `jx64_setcc_r8`/`jx64_jcc_label` mask `cc & 0xF`;
`jx64_alu_rm_r32`'s `digit` into the opcode byte is *unmasked* but every call site passes
recipe-table constants; `digit ≥ 8` would emit invalid/reserved opcodes (#UD), not
arbitrary code.

### 1.4 The structural backstop (not stated in the tree — worth writing down)

`jitcg_emit_mem_prep` (`jit_codegen.c:312-380`) emits, in order: the segment-null gate,
the page-straddle gate (byte never; 16-bit ≤0xFFE; 32-bit ≤0xFFC), the TLB test
`(entry & mask) == TLB_VALID` with mask ≤ 7 (so the imm8 sign-extension trap cannot fire
here), `in_mapped_range` rejection (`phys >= memory_size` or the 0xA0000-0xC0000 window →
slow path), and — for writes — the code-page gate. The access itself is
`[mem8 + phys]` with `phys` zero-extended (32-bit ops). Since `memory_size` is always
MiB-aligned (`main.c:936-956`), `phys < memory_size` plus no-straddle confines **every**
emitted access to guest RAM. The `[rbp + JC_OFF(...)]` state accesses use `offsetof()`
constants only. So the worst case for a hypothetical recipe bug is a wrong *guest* result
plus the slow-path fallback — never a wild host access. This is the strongest form of
the central claim and it deserves a comment in `jitcg_emit_mem_prep`.

## 2. BUFFER BOUNDS (attack point 2)

- Emission goes only into the static 16 KiB `jitcg_scratch` (`jit_codegen.c:155`);
  every `put` is bounds-checked with a sticky overflow (`jit_x64.c:80-87`), and
  `jx64_finish` refuses on overflow *before* its fixup writes (`:695`), so a full buffer
  can never be patched past its end. `jit_code_reserve_span(len)` rounds up, `publish`
  and `bind` both validate span length against `npages` (`jit_mem.c:306-317, 467-471`),
  and the `memcpy` is exactly `len` bytes (`jit_codegen.c:1732`). **No guest shape can
  make the emitter write past its page or arena; the worst case is a refusal** (counted,
  honest). Blocks are capped at 64 uops / 240 guest bytes at decode
  (`uop_cache.c:654-661`).
- Labels: worst case is 16-19 fixed labels + 3 per uop (l_slow, l_tail, l_ok) for
  all-mem-write recipes ⇒ a 59+-uop all-slow-mem block exhausts
  `JX64_MAX_LABELS (192)` *while still fitting 16 KiB* (~200-250 B/uop × 64 ≈ 13-16 K).
  `jx64_label` returns -1, every consumer of -1 sets overflow, the block is refused.
  Safe, but it silently caps coverage below the fit rule — §5b.

## 3. W^X END TO END (attack point 3)

Traced every transition of every page state (FREE→RW→RX→FREE, plus DEFER):

- The pool is one `mmap(PROT_READ|PROT_WRITE)`; only `mprotect` changes per-page
  permissions, always as a whole span, always RW→RX at publish (before any execution)
  and RX→RW at recycle (after the last execution). **No page is ever simultaneously
  writable and executable; no failure path leaves a page W∧X** (a failed `mprotect(RX)`
  keeps it RW and unexecuted; a failed `mprotect(RW)` leaks the span RX — the safe
  direction, `jit_mem.c:352-359`).
- **Deferred recycle**: `jit_page_recycle` on the executing *span head* defers
  (`JP_DEFER`, no flip) and `jit_block_exit` resolves it after the call returned
  (`jit_mem.c:343-351, 714-727`). The DEFER compare is head==head (`jit_cur_page` is
  only ever set from a span head, per link too), and the non-head pages of a deferred
  span stay JP_RX/owned — nothing can hand them out.
- **SMC abort**: the invalidator sets `jit_abort_flag` *before* dropping, and the drop's
  recycle hits the DEFER arm because `jit_cur_block` still names the executing block.
  During a fallback C call inside a compiled block we are still in the entry block
  (chains update `jit_cur_page` only when a link is taken, and a link is the last thing
  a source block does — no return into it afterwards: the target reuses the source's
  frame). A chained target's page is protected per link (`jit_chain_target`,
  `jit_mem.c:587-595`).
- **Reboot**: `machine_reboot` resets the CPU and sets `reboot_requested`; every exit
  kind (including TAIL and ABORT) unwinds at `uop_cache.c:1354-1357, 1376-1379`; main.c
  runs `uop_cache_clear()` before boot 2 (`main.c:413`), which drops compiled blocks with
  the decoded ones (`uop_cache.c:834-856` — the load-bearing comment there is correct:
  the shared bitmap is zeroed, so leaving a compiled block alive would unwatch it).
- **No guest-writable memory is executed**: the only pointers ever called are
  `blk->code` (set only from `jit_compile_block`, a pool page), chain entries
  (`pool + chain_off`, `chain_off < len`), and the probe's canary (a pool page). `mem8`
  is a separate mapping and is never entered.

## 4. CACHE-KEY CONFUSION (attack point 4)

Key = `(phys_start, virt_start, state)`, `state = cpu->state_flags & CACHED_MASK_ALL`
(IS_32 | SS32 | CPL3 | FLAT_SEGS, `cpu_state.h:434-438`) — the reference's own cached
state byte, and the same triple the uop cache has keyed on since PERF-4 (gate-tested by
nasm 118/118 in every mode and the byte-identical transcripts). Both hashes compare the
full triple (`jit_mem.c:531-532, 584-585`), so a block compiled under one state byte
cannot be returned under another. The one JIT-baked input, the cpl3 TLB fold, is part of
the key, so it cannot mismatch live cpl. Segment bases/nulls, flags, and EIP are read
from `cpu` at run time — not baked. The chain recomputes the state from live flags and
takes only the fetch-cache-hit arm; the fetch cache (`last_virt_eip`) is cleared on every
TLB flush path (`paging.c:51, 64, 90`), so a CR3 change/invlpg cannot make a link enter
a block compiled for the old mapping. The same-page invariant that makes the ABORT
fallback's entry-block geometry exact is documented at the abort site
(`uop_cache.c:1385-1401`, the JIT-6F fix) — verified still accurate. **No confusion path
found.**

## 5. THE PLATFORM PROBE (attack point 5) and THE DESCRIPTOR TABLE (attack point 6)

- Probe (`jit_mem.c:376-408`): reserve → write canary (`B8 5A 2A 00 00 C3` — verified
  encoding) → publish (the W^X flip) → execute → recycle. `jit_probe_ok` goes false
  first; failure ⇒ `jit_mem_init` false ⇒ `jit_enabled = false` (`main.c:891-896`) and
  the machine runs the uop cache (the forced-fail gate proved byte-identical behavior).
  The canary is written through the pool's own entry points, so the probe exercises the
  production sequence. **One gap**: if a platform let `mprotect(RX)` succeed but then
  faulted on execution, the canary call is a SIGSEGV — a crash, not a fallback (§5a).
  On Linux/x86-64 there is no such window (RX ⇒ executable), and on the platforms the
  caveats name (macOS without MAP_JIT, Windows) the failure lands in `mprotect`, which
  the probe handles cleanly. Fail-stop, not silent — hence NICE-TO-HAVE.
- Generator (`gen/interpreter_uop.js`): all indices are consistent **by construction** —
  `first_arm`/`n_arms` are captured around the arms pushed for each case
  (`:387-398`), `first`/`n` around each arm's entries, `desc_index` is interned over
  exactly the ranges the decoder walks (`:407-414`), and `case_lines` **fails** if any
  of the 512 case slots was never built (`:419-423`) — so the decoder's `cases[key]`,
  `uop_arms[c->first_arm + i]`, `uop_entries[arm->first + i]`, `uop_descs[e->desc]`
  cannot go out of range. A NULL fn form is a refusal (`uop_cache.c:592-594`), not a
  call through garbage. **MEASURED this audit**: the generator run in a /tmp sandbox
  (tree copied, `OUT_FILE` untouched) reproduces `src/cpu/uop_gen.c` **byte-identically**
  ("1182 descriptors, 1182 entries, 924 arms, 1922 forms, 0 unimplemented" — unchanged).

## 6. FINDINGS

### SHOULD-FIX 1 — the JIT block-hash insert silently overwrites a slot once binds saturate
`src/cpu/jit_mem.c:487-493` (`jit_cache_bind`'s insert):

```c
uint32_t slot = jit_hash_pos(phys_start, virt_start, state);
for (uint32_t n = 0; n < JIT_HASH_SLOTS; n++) {
    if (jit_hash[slot] == 0) break;
    slot = (slot + 1u) & (JIT_HASH_SLOTS - 1u);
}
b->hash_slot = (uint16_t)slot;
jit_hash[slot] = (uint16_t)(id + 1u);
```

Unlike `uop_hash_insert` (`uop_cache.c:927-941`, 100 lines away in the same tree), this
insert reuses neither DEAD slots nor detects a full table. Tombstones accumulate one per
invalidation and are reclaimed **only** by `jit_code_clear_all` (a whole-cache clear,
which happens only when the *uop* arena/block table overflows). Every invalidation+re-decode
cycle on the same page therefore permanently consumes one non-zero slot: an untrusted
guest running a self-modifying loop on a few pages (the classic SMC churn shape — the
benchmark itself already produced 308 invalidations with no clear) can push total binds
past `JIT_HASH_SLOTS (4096)` without ever triggering a clear. Then the loop exhausts, the
insert **overwrites the last-probed slot**: (a) a live block becomes unreachable
(lookups miss ⇒ recompile ⇒ more churn — a spiral, all still key-compared so never a
wrong block); (b) two blocks now alias one `hash_slot`, and dropping either writes `DEAD`
over the other's entry; (c) unreachable-but-live blocks hold pool pages until their guest
page is written or a clear runs — pool exhaustion follows, which degrades honestly to the
uop cache (`lowmem_failures`). Wrong-answer-safe throughout; silent data-structure
corruption and an adversarial performance cliff under the audit's stated threat model.

**Fix** (either): reuse the first DEAD slot seen, exactly as `uop_hash_insert` does; or
refuse the bind when the scan finds neither EMPTY nor DEAD (count it in
`jit_stats.lowmem_failures`-style). The first option also removes the churn spiral's
unbounded tombstone growth. Why I did not run a probe: demonstrating it needs a crafted
~4096-cycle SMC guest and a build (this audit is read-only on the tree); the mechanism is
a ten-line data-structure argument with no branch I have not read.

### NICE-TO-HAVE a — the probe's execute arm can crash instead of disabling
`src/cpu/jit_mem.c:397`. If `mprotect(RX)` succeeds but the CPU then refuses execution
(no known platform in the shipped matrix behaves this way; the untested platforms the
caveats name fail at `mprotect`), the canary call raises SIGSEGV and the process dies
before the "JIT disabled" fallback can act. Loud, not silent — but the probe exists to
make unverified platforms *honest*, and a crash is not the promised degradation. Fix:
wrap the call in a `sigsetjmp` + SIGSEGV/SIGBUS handler (restore default on exit), or
fork-and-wait for the canary.

### NICE-TO-HAVE b — `JX64_MAX_LABELS` silently caps coverage below the fit rule
`src/cpu/jit_x64.h:73` (192) vs `jit_codegen.c`'s allocation pattern: 16 fixed labels +
up to 3 per uop (l_slow + l_tail + l_ok) ⇒ a 59+-uop block of all-mem-write recipes
exhausts labels while still fitting the 16 KiB byte cap (~200-250 B/uop). The refusal is
safe (overflow ⇒ `jx64_finish` false ⇒ `refused_fit`) but it is counted as a *fit*
refusal, so the instrument mis-attributes it. Fix: raise `JX64_MAX_LABELS` to 256 (the
struct grows ~512 B; nothing else reads it), or count label exhaustion separately.

### NICE-TO-HAVE c — `JITCG_DUMP` getenv in the per-uop compile loop
`src/cpu/jit_codegen.c:1467` runs `getenv` once per emitted uop at compile time
(~35 k calls on the benchmark's 740 blocks × 47 uops; every recompile too). This is the
same class as M2's debug-hook lesson (~2 µs/getenv ⇒ plausibly tens of ms per boot, on
the compile path only — the A/B numbers in the record were measured with it present).
`jit_mem.c:637`'s `JITDBG` is correctly bounded (`dbg < 12`). **Cross-lane note: cost is
the performance reviewer's scope** — flagged here only because it is the M2 lesson's own
shape. Fix: hoist to a `static int` checked once, or drop behind `#ifndef NDEBUG`.

### NICE-TO-HAVE d — `blocks_reused`'s stated mechanism does not exist
`src/cpu/jit_codegen.c:1529-1539` justifies the compile-time reuse lookup with "the
cache's own clear (an arena/block overflow) drops decoded blocks without any guest
write, ... so the compiled block is still valid". But `uop_clear_internal` calls
`jit_code_clear_all` (`uop_cache.c:849`), and page invalidation drops both registries
symmetrically — **no path drops a uop record while its compiled block survives**, so
`blocks_reused` is identically 0 and the lookup is currently dead. The lookup is
harm-defensive (it protects a future asymmetric drop path); the comment is the project's
own named "false mechanism" class. Fix: correct the comment to say it is defensive, or
delete the lookup and the counter.

## 7. COVERAGE

Read IN FULL: `src/cpu/jit_x64.{c,h}`, `src/cpu/jit_codegen.{c,h}`, `src/cpu/jit_mem.{c,h}`,
`src/cpu/uop_cache.h`, `gen/interpreter_uop.js`.
Read in targeted part: `src/cpu/uop_cache.c` (prefix/modrm/sib extraction 150-308,
materialisation 336-435, shape/decode walk 438-700, cache init/clear/hash 820-1001,
invalidation 1003-1060, run path + JIT arm + abort continuation 1243-1490);
`src/cpu/paging.c` (TLB flush paths 47-92, translate/fold 255-320); `src/cpu/paging.h`,
`src/cpu/memory.h`, `src/cpu/cpu_state.h` (definitions cited); `src/hw/machine.c`
(120-200); `src/main.c` (jit init + memory_size sites); `src/cpu/modrm.h` (prefix
constants); `src/cpu/uop_gen.c` (sampled rows + the byte-identical regeneration proof —
not all 5558 lines read line-by-line; it is generated and I verified the generator
reproduces it exactly).
Referenced, NOT re-read this round (covered by reviews 2-3 whose sha256s match where
they overlap): `jit_selftest.c`, `jit_codegen_selftest.c` (the byte pins and matrices).
NOT checked: macOS/Windows behaviour (no host — that is the probe's job); the peer
reviewers' lanes (host memory safety outside the JIT, performance).
MEASURED this audit: the /tmp generator reproduction (§5). Not run: builds, gates, guest
probes (read-only constraint; the tree's gates are green per the report record and every
finding above rests on code-reading whose branch inventory is stated).

## 8. sha256 OF EVERY FILE REVIEWED

```
c177996dafb60264f57e8e7edbf93094af3d91f855fd06298a47e46af25b5e4f  src/cpu/jit_x64.c
03e25887acb1a0f109665aed8d27f4fadab8ae2b2db1fb1eb27b4f156e1e1e90  src/cpu/jit_x64.h
184a286e557d6003a2e702b24ebc0dca9cca87f48f7dff14c72f0cec694674eb  src/cpu/jit_codegen.c
114d3782d7da45ad341f1d24e906b8a4771e1fc0aea68136a702e8c624518a70  src/cpu/jit_codegen.h
04eb11179f4224b5bee1a96908aa1ac78b5c3b326ba2433aa391fd2983da1498  src/cpu/jit_mem.c
97fc0a42b92ff34cea1b9b822626fc9330253db146ef1d76154e727f1dba7f6c  src/cpu/jit_mem.h
c3bf72bd813b3dadaf786d52fa8e4134194c9e8390b8b3907a0a5337d1e2e466  src/cpu/uop_cache.c
c3c2efc14b88e8b6064966de81bc546fe7f24a0ebe6132751ec601498ebdc596  src/cpu/uop_cache.h
f238f48eada86414edc25e0bc12308a52111e5bb2214476897887b44697e52d5  src/cpu/uop_gen.c
4f7694d478f96dda68b4b9b3828fb90e75a95de61a9aad73e05e4de78beda217  gen/interpreter_uop.js
a6a0f1ed2e7d7249cc47fa7834cb65175bdcaba1894e2e91a2b8aeebed7c6542  src/cpu/paging.c   (part)
78e1ed03fda8e45cf282755c7158289d61b2f9ae2e173c6877b5494a627da457  src/cpu/paging.h  (part)
5a9a468fc8fb8afb0d80698e3f2e9324d6631e70abbf6d99700bc536d39c0a4a  src/cpu/memory.h  (part)
f2c9a339283d6950a468fffde08194d60d3f1cee8bcfbd17cae2038b979e1f2a  src/cpu/cpu_state.h (part)
405e562478babcc017995bd39c47ac44ebb17700f3899a3b26875765fa963292  src/hw/machine.c  (part)
c4a712ab80f701edbddc84165072581a72bdb8f1dc7646c243b0f1654d68d45e  src/main.c       (part)
351b15f2a4dcce1df93c889cd672a867ba59533edd53e10ffd2a9bff4b8c9861  src/cpu/modrm.h  (part)
958c3990cb9d0d8514480a9967ef6bef8c1879dc9bd8d3e08ecfaf08c132bd64  src/cpu/jit_selftest.c (referenced)
d3e57808db514d5914c8bc707a567859b7da2a90868630cd0e9bf68d8adf2f6e  src/cpu/jit_codegen_selftest.c (referenced)
```
