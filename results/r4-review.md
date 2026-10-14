# R4 — independent review: fpu.c / softfloat.c / the D8..DF group / instructions_0f.c

Unit: `HANDOFF=v86c`, unit R4 (reviewer).  Read-only pass against the pinned upstream
`./v86` (0.5.471+g94faf3d).  Gates were NOT re-run (orchestrator verified them from a clean
build); this pass is reading only, as the brief directs.

**VERDICT: NO BLOCKERS, NO SHOULD-FIX transcription defects.**  Every surface the two units
named as beyond their own instruments was read line-by-line against the upstream Rust and
matches.  Three NICE-TO-HAVE findings below (citation accuracy, one ABI-hygiene option, one
reachable assert), none of which is a wrong-value defect.

Files reviewed (sha256 at read time):

| file | sha256 |
| --- | --- |
| src/cpu/fpu.c (1217) | 5987fa5cf0c4323bc323d7b56d108145ee28d3d239bd813d9aba0b427042a096 |
| src/cpu/softfloat.c (274) | b96aea1b44216ad1f88db5e01e8995ef8c4518dacf345c9f919f0c3a09a1dfb6 |
| src/cpu/fpu.h (194) | 476f08f2146b741722b8ecc8b8845f3415fcf8e506fb8480f5d5c9643635664f |
| src/cpu/softfloat.h (191) | 4535bb390f29ab959f6560b693ad58fdff3afa850d2d27080935db0b4038b897 |
| src/cpu/instructions.c (2658; D8..DF block 2355-2658) | bcab21bad3aeb943880bcbde247a694cf8a7b49f8e3b22bed9fa15601636324a |
| src/cpu/instructions_0f.c (1871) | 3dfa5fc16526136146192c690d559911b394bb08020afc757c062d9f5eee7722 |
| src/cpu/instructions_0f.h (838) | 7bdeba51ed98d40138269157ba19f9f910e5d91cc7e8d5080a8410c9399795bf |
| Makefile (227) | 6e73fe60cc96e6bddf30f5781e8ec1b004147a72c08a2576454e409c978c4b23 |
| src/cpu/sse_instr.c (479; M1b-2 in flight, observed) | df7469d4e760ad16b8546e1ba13eb14dbda4beb8038f93d4fea93382b0f818ec |

---

## 1. M1b-3's named blind spots — all verified 1:1

**env save/restore layouts.** fpu_fldenv16/32, fpu_fstenv16/32, fpu_fsave32/frstor32
(fpu.c:802-905) match fpu.rs:417-580 byte-for-byte: the 16-bit layout (+0 cw, +2 sw, +4 tag,
+6 ip16, +8 ip_sel, +10 dp16, +12 dp_sel), the 32-bit layout (+0 cw, +4 sw, +8 tag, +12 ip,
+16 ip_sel, +18 opcode, +20 dp, +24 dp_sel), the high_bits ADD for cw/sw/tag vs the OR for
dp_selector in fstenv32 (reproduced verbatim, asymmetry included), the 108-byte
fsave/frstor image with `(i + sp) & 7` / `(sp + i) & 7` slot order, and fsave's implicit
finit.  The D9/DD entry points pass the right range checks (14/28 for env, 10 for m80,
108 for save/restore — instructions.c:2398-2446, 2522-2527, 2577-2581, 2640-2645).

**tag-word encoding.** fpu_set_tag_word (fpu.c:220-228) = fpu.rs:444-450: only `(tag >>
2i) & 3 == 3` means empty (the "zero vs valid" collapse is upstream's model, kept — the
report's §7 note is accurate).  fpu_load_tag_word (fpu.c:230-247) = fpu.rs:582-597: empty→3,
zero→1, non-finite→2, else 0, iterating PHYSICAL slots.  fxsave's
`~(stack_empty) & 0xFF` byte and fxrstor's inverse match misc_instr.rs:381/408.

**stack-fault C1 rules.** fpu_get_st0/fpu_get_sti (fpu.c:56-84): empty slot → C1 CLEARED +
SF|I + INDEFINITE_NAN = fpu.rs:32-42, 67-78.  fpu_push (fpu.c:407-420): empty slot → C1
cleared, push proceeds; full → C1 SET + SF|I + INDEFINITE_NAN = fpu.rs:259-271.  Every
`+/- & 7` index computation has the Rust precedence (as/+ before &) pinned correctly.

**fcomi's flag mapping.** fpu.c:365-386 = fpu.rs:203-216 exactly: flags_changed = 0,
flags &= ~FLAGS_ALL, then LT→CF (FLAG_CARRY=1 ✓), EQ→ZF (FLAG_ZERO=64 ✓),
unordered→CF|PF|ZF, GT→nothing.  fucomi identical with lt_quiet.  Constants checked
against cpu_state.h:51-67 and fpu.rs:8-23.

**fbstp.** fpu.c:763-787 = fpu.rs:645-664 exactly: to_i64 under the CURRENT rounding mode
(not truncation — upstream's choice), the overflow-safe unsigned_abs spelling, the
10^19−1 limit, the 9-iteration digit loop with `high << 4 | low` byte order, the sign byte,
the else-branch indefinite-NaN + #I, the pop.  DF/6's writable_or_pagefault(10) pre-check
matches instructions.rs:1998-2002.

**Everything else in fpu.c** was read against fpu.rs function-for-function (all 90+ items
including fprem's quotient-bit arithmetic, fxam's C1=sign, the fcmovcc
fault-outside-condition quirk, the store_m32 fault-then-flags ordering, the D-1..D-11
divergence claims).  The D-5/D-8 claims check out: fpu_unimpl drops only the debug assert
(the release reference delivers #UD — correct), and fprem's dead intel_compatibility arm is
kept, reproducing the reference's dead code.

**softfloat.c** was read against softfloat.rs item-for-item.  All constants match the
bit patterns (LN_10/LN_2/PI/LOG2_E/INDEFINITE_NAN/POS/NEG_INFINITY), the exception-flag
translation `f>>4&1 | f>>1&4 | f<<3&16` is exact (including its dropping of inexact and
overflow — upstream's mapping, reproduced), rem is the quotient-truncate-subtract form
(NOT extF80M_rem — upstream's commented-out route), the comparisons reproduce the
`lt(a,b) → Less; lt(b,a) → Greater; eq → Equal; else None` order, and the engine entry
points were checked against the vendored amalgamation's own prototypes
(softfloat.c:660-685, 25840-25920): eq/lt/lt_quiet return `bool` there — the wrapper's bool
declarations are exact, not just ABI-plausible.  Rounding-mode values 0/1/2/3
(softfloat.c:449-452) and the 32/64/80 precision values match the port's enums and
set_control_word's rc/pc mapping (fpu.c:250-274 = fpu.rs:283-309).

## 2. The D8..DF group (instructions.c:2355-2658) — all 160 verified

Read the whole block against instructions.rs:1643-2003.  Every entry point matches: the
D8/DA/DC/DE m32/m64/m16 arithmetic with the hoisted-load pagefault check (equivalent to
`return_on_pagefault!` — the sentinel is never used), the D9/DD 16- vs 32-bit env splits,
the DA/DB FCMOVcc pairs (b/z/be/p and their complements), the DB/4 sub-encodings
(finit/fclex/fsetpm/fdisi/fneni-nop), the DE reg forms' pop-after-operate, the DF group
(ffree+pop, fxch, fstp×2, fnstsw-reg, fucomip/fcomip), the trigger_ud encodings (D9/1,
D9/2≠0, DA/4/6/7-reg, DB/4-mem, DB/6-mem, DB/7-reg, DD/5, DD/6/7-reg, DE/3≠1, DF/7-reg)
and fbld→fpu_unimpl→#UD (matching the release reference, which has no assert there).
No ordering/state-mutation divergence found: the fault-flag checks sit exactly where the
Rust `?` short-circuits, partial effects match.

## 3. instructions_0f.c — the five named surfaces, then the rest of the file

Read the ENTIRE file (1871 lines) against instructions_0f.rs.  In the unit's own order:

* **cpuid** (instructions_0f.c:1109-1258) = instructions_0f.rs:3256-3417: every leaf
  (0/1/2/4/5/7/0x80000000/0x80000001/0x40000000/0x80000008/0x15/0x16) and every constant
  (including leaf-1's `0x80000000u` hypervisor bit, the acpi_enabled-gated APIC bit, the
  TSC_RATE-derived 0x15/0x16 values, the default-leaf zeros) is bit-exact.
* **CR/DR moves** (0F 20-27, instructions_0f.c:432-549) = instructions_0f.rs:727-866: cpl
  gating, creg 0/2/3/4 else #UD, the CR4 invalid-bit mask `(1<<11|1<<12|1<<15|1<<16|1<<19)
  | 0xFFC00000` (upstream's own omissions of bits 17/18 reproduced), the
  PGE|PSE|PAE TLB flush, the load_pdpte condition, DR4/5→DR6/7 under CR4.DE.
* **0F 01 group** (instructions_0f.c:147-306) = instructions_0f.rs:200-324: sgdt/sidt
  (write16 + write32 & mask), lgdt/lidt (cpl gate, read order, mask), smsw (16-bit writes
  in BOTH mem forms — the upstream quirk is reproduced), lmsw (`cr & ~15 | new & 15`, the
  protected-mode PE bit), invlpg, all _reg forms #UD.
* **lss** (instructions_0f.c:1519-1574) = instructions_0f.rs:3629-3640: lss16/lss32 with
  SS/FS/GS, _reg forms #UD.  (lss16/32 bodies are misc_instr.c's — M1a-2's surface, outside
  this review; the call sites pass the right arguments.)
* **0F AE** (instructions_0f.c:1399-1488) = instructions_0f.rs:3520-3553: /0,/1 → fpu.c's
  fxsave/fxrstor (the contract, single model), /2 ldmxcsr with the MXCSR_MASK #GP check,
  /3 stmxcsr, /4,/5,/6 mem = #UD, /5,/6,/7 reg = fences (empty), /7 mem = clflush with
  translate_address_read.
* **hadd/hsub** (instructions_0f.c:870-943) = instructions_0f.rs:3018-3093: pair order
  (dest0+dest1, src0+src1) is upstream's, including reading the destination before writing
  it.

Then the REST of the file, also read against upstream: 0F 00 /0,/1 (sldt/str with the
protected_mode/vm86 gates), 0F 04-0F (the undefined encodings → #UD, matching the release
reference — the unit's divergence-1 ruling is right; clts/wbinvd/UD2 correct), 0F 18-1F
nops, jcc (0F 80-8F, all 16 conditions correct per opcode), setcc (0F 90-9F, same order),
cmovcc (0F 40-4F, all 64 forms, mem reads before condition — the M1a-4 order rule holds),
push/pop fs/gs, bt/bts/btr/btc (reg masks 15/31, mem forms SEX16 for the 16-bit — matches
`read_reg16(r) << 16 >> 16`), the 0F BA immediate group, shld/shrd (SAFE_READ_WRITE*
matches the upstream closures), bsf/bsr (argument order `(old_dest, source)` preserved),
imul 0F AF, cmpxchg 0F B0/B1, popcnt (F3 forms live, plain forms #UD — upstream's exact
split), 0F B9 UD2, xadd, cmpxchg8b (0F C7 /1: flags|ZF then `flags_changed &= ~FLAG_ZERO`
— the lazy-flag bookkeeping matches upstream line for line), bswap, 0F FF, the movzx/movsx
slices (SEX8/SEX16 through uint32_t — the documented UB avoidance), and the pxor slice
(66 0F EF, the corpus's shared prologue).  The 31 unimplemented encodings are accounted for
as the report states: 0F C7 /6 _reg forms are absent (emitter stops by name) while the _mem
forms are present and #UD — exactly upstream's split.

## 4. The hunt classes

* **second model of the tag word**: none.  instructions_0f.c reaches x87 state only through
  fpu.c's fxsave/fxrstor; fpu_set_tag_word is declared (the contract) but uncalled here
  (emms is M1b-2's); the in-flight sse_instr.c uses fpu.c's fpu_set_tag_word for
  transition_fpu_to_mmx (sse_instr.c:44-47 = cpu.rs:4201-4207) — one model, three callers.
* **silent stub / silent default**: none.  Every upstream trigger_ud/undefined_instruction/
  unimplemented_sse site maps to a real #UD, and unimplemented encodings stop the machine
  by name through the emitter (verified shape in R3).  The one place the port stops where
  the release reference proceeds is finding N3.
* **ordering/state-mutation divergence**: none found; the mem-form hoist-check-use pattern
  is uniform and correct across both files.
* **`-Wl,--gc-sections`** (Makefile:72-82, 127, 138, 144): sound.  Only SF_OBJ is compiled
  with `-ffunction-sections -fdata-sections`; every CPU object is single-section, so the
  link-time GC can only ever drop unreferenced sections OF THE VENDOR OBJECT — which is
  exactly the F128 dead code it exists to drop.  The engine sections actually needed at
  runtime are retained (direct calls from softfloat.c's wrapper; the three globals
  referenced directly), and the 47/47 nasm FPU tests execute the engine live, proving the
  needed sections survive in the real binary.  SF_DEFINES match upstream's own
  (v86/Makefile:243-245).

## 5. Findings (all NICE-TO-HAVE)

**N1 — the upstream line citations in instructions_0f.c's comments are systematically
stale** (the D8..DF citations in instructions.c are accurate; the 0F file's mostly are
not).  Measured examples: cpuid cited "instructions_0f.rs:1064-1228" — the real instr_0FA2
is at 3256-3417 (1064 is cvtsd2si); jcc cited "973-1004" — real 3162-3194; setcc cited
"1005-1036" — real 3195-3227; 0F 20-27 cited "407-536" — real 727-866 (407 is 0F 04); bt
cited "1229-1232" — real 3420-3423; lss cited "1438-1472" — real 3629-3640; popcnt cited
"1489-1492" — real ~3681; cmpxchg8b cited "1589-1637" — real 3973-3994.  The sgdt/sidt/
lgdt/lidt citations (200-262) ARE right.  These comments are the project's traceability
mechanism — a future spot-check following one lands on unrelated SSE code.  Fix: re-cite
against the pinned clone.  Related: fpu.c:702 says "instructions.rs's DB_5_mem does
readable_or_pagefault(addr, 10) first" — true of DB_5_mem, but DB_5_mem calls fpu_fldm80,
not fpu_fldm80_without_fault; the without_fault variant has NO caller anywhere upstream
(it is dead in the reference too) — the comment should say so.

**N2 — softfloat.h declares the SmallFloat entry points with plain integer types where the
engine defines struct parameters** (softfloat.h:72-88 vs softfloat.c:663-665).  This is a
cross-TU incompatible-type redeclaration — formally UB in C, ABI-identical on SysV amd64
and AAPCS64, deliberately documented (D-2) and MEASURED by the selftest round-trip and the
47 FPU tests.  It is upstream's own licence (softfloat.rs:24-27 declares i32 over the wasm
export), so it is not wrong — but the UB costs nothing to remove: typedef the two
single-member structs in softfloat.h and use them in the six declarations; every call site
stays textually identical.

**N3 — the `DBG_ASSERT((addr & 0xF) == 0)` in fxsave/fxrstor (fpu.c:1152, 1186) is
reachable from guest code and diverges from the pinned reference.**  Upstream's alignment
check is a `dbg_assert!` (misc_instr.rs:370/398 — compiled out of the release wasm; the
reference then just executes), and a guest can reach a misaligned address trivially
(`fxsave [eax]` with eax&0xF != 0).  The port's shipped build has assertions ON, so that
guest aborts the machine where the reference writes.  Same class as R3's N1 (reachable
asserts, loud not silent) — but note the asymmetry: for this site the reference's own
comment says "TODO: #gp", so real hardware faults where the reference writes.  Fix options:
(i) the project-wide -DNDEBUG shipped-build decision (kills this and the prefix-fallthrough
assert together), or (ii) downgrade these two asserts to the comment, matching the release
reference exactly.

## 6. COVERAGE

READ IN FULL (both sides): src/cpu/fpu.c (1217) vs v86/src/rust/cpu/fpu.rs (913) +
misc_instr.rs:369-428; src/cpu/softfloat.c (274) vs v86/src/rust/softfloat.rs (286);
src/cpu/fpu.h, src/cpu/softfloat.h; the D8..DF block (instructions.c:2355-2658) vs
instructions.rs:1643-2003; src/cpu/instructions_0f.c (1871, entire file) vs
instructions_0f.rs for every ported group; the Makefile.  SAMPLED with verification:
engine prototypes/globals in the vendored amalgamation (v86/lib/softfloat/softfloat.c:
449-455, 623, 660-685, 25840-25920), SF_DEFINES vs v86/Makefile:242-248, MXCSR_MASK vs
cpu.rs:255, FLAG_* constants vs the fcomi mapping, sse_instr.c's transition_fpu_to_mmx vs
cpu.rs:4201-4207, the header-prototype census (spot counts in instructions_0f.h/c agree
with the unit's 377/377 claim; the rdrand _reg/_mem split is as documented).  COULD NOT
CHECK / out of scope: the unit selftest cases (instrument), the emitter (R3's surface), the
lss16/lss32 and cmovcc/arith bodies (misc_instr.c/arith.c, M1a-2's surface), M1b-2's
in-flight sse_instr.c bodies, and the gates (not re-run, per the brief).  instructions_0f.c
was reviewed at the sha256 recorded above; the concurrent append-only M1b-2 edit may have
moved the file since.
