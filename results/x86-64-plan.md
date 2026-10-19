# X86-64-PLAN — long mode, and the swappable CPU backends it needs

**Unit:** `HANDOFF=v86c-x86-64`, unit **X86-64-PLAN** (planner, plan-only). **Inputs:** memory
node `handoff-v86c-x86-64-ctx` (five `stage=context:` observations, the brief of record),
`results/jit-plan.md` and `results/kvm-plan.md` (the tree's two plan documents, for style and for
the KVM lane this one must leave alone), `tests/nasm_run.py` + `tests/mkfixture.c` +
`tests/nasm2gas.py` + `tests/nasm_skip_c.txt` (the differential corpus), `src/cpu/cpu_state.h`,
`src/cpu/cpu.c`, `src/cpu/paging.c`, `src/cpu/uop_cache.{c,h}`, `src/cpu/jit_codegen.h`,
`src/hw/vga.h` (the seam precedent), `gen/interpreter_c.js`, `gen/interpreter_uop.js`,
`docs/architecture.md`, `docs/testing.md`, `TODO.md` §8, and the code itself.

Claims are tagged **MEASURED** (run on this host in this pass, command named), **READ** (from
source, with `file:line`; `v86/…` is the read-only reference clone), **INTERPRETATION** (a
judgement over the facts above the line) and **PROJECTION** (an estimate, not a measurement).
Where a claim carries no tag the sentence is a **decision** — this plan decides things as well as
finding them, and a decision is not evidence.

**This document does not modify the tree.** Its only product is the design; §6's slices are what
an implementer executes.

---

## 0. Bottom line

Build **x86-64 long mode as a property of the CPU model**, and build **the CPU-backend seam
first**, in that order, with the seam landed **inert**: S0 makes the three existing engines
selectable behind one contract and moves **no byte** of anything the tree ships today (its gate is
the whole existing battery). Then long mode in thirteen slices, each ending in a gate that can
fail (§6).

**The hard truth this plan is organised around: long mode has no reference in this tree, and one
layer of it has no reference anywhere.** Upstream v86 has no long mode at all (MEASURED: zero
files under `v86/src/` match `EFER_LME`, `EFER_LMA`, `CR4_LA57` or `PML4`; `v86/gen/x86_table.js:78`
encodes `0x63` as `arpl`; `0x40`–`0x4F` are INC/DEC, `v86/gen/x86_table.js:831-832`), so:

- the **ISA layer** gets a *real* oracle, and rather better than the brief expects: the fixture leg
  of the nasm differential already runs the test image **natively on the host CPU**
  (`tests/mkfixture.c:76-100` forks, `PTRACE_TRACEME`s and `execl`s the image) and already reads
  the host's **x86-64** `ptrace` register layout (`tests/mkfixture.c:29-44`). A 64-bit corpus is a
  *new document format and a new runner* around an oracle that exists and is proven — not a new
  oracle;
- the **system layer** (4-level walk, mode transitions, SYSCALL/SYSRET/SWAPGS, MSRs, the 64-bit
  descriptor tables, interrupts) has **no userspace oracle at all** — the host will not run it from
  ring 3. It gets spec-derived selftests with mutation probes, **the guest kernel's own `dmesg` as
  a per-feature assertion**, and for each item a *named* gap;
- the **guest layer** is gated by a real x86-64 kernel reaching **userspace**, with the tree's
  existing `qemu-system-x86_64` pre-check as the second leg (the shape `tests/gate-m2-5.sh`'s gate
  1b already uses).

**What the lane can claim when it is done:** the 64-bit ISA subset the corpus covers is
host-verified instruction by instruction; the system layer is spec-derived and guest-exercised at
feature granularity; a real 64-bit Linux boots to userspace. **What it can never claim:** that any
64-bit behaviour outside the corpus and outside the boot's exercised paths is correct. §2.2's table
states the gap per item rather than at the end, and §8 keeps the undecided list so no later reader
finds a silence where a promise was.

**It is an extension, not a rewrite** (§4): of the port's 32-bit-shaped state, eight field groups
are *widened in place* (four of them inside contained accessor functions), **exactly one structure
is parallel** (the TLB), and the memory model, the physical address space, the device layer, the
boot paths and the lazy-flag design are unchanged. The decoder dispatch stays
**generated** — from `v86/gen/x86_table.js` unchanged plus a new *authored overlay* table — and the
semantics stay hand-written, which is `docs/architecture.md` §7's contract kept, not bent (§3.6).

---

## 1. What this pass verified, and five corrections it owes the brief

Twelve findings (referred to elsewhere in this document as **finding N**), then five corrections the
plan owes the brief (**C1**-**C5**).

| # | Finding | How |
|---|---|---|
| 1 | The port's dispatch table has **873** entries, **309** of them `os`-split, spread over **154** opcodes; **586** carry a modrm, **192** are `block_boundary`. Those 154 opcodes are exactly the set that gains a *third* operand-size arm. | **MEASURED** (`node` over `v86/gen/x86_table.js`) |
| 2 | `src/cpu/interpreter_gen.c` is **14,727** lines / **1,024** dispatch arms / **1,922** call sites. A third arm is a generator change, not a hand edit. | **MEASURED** (`grep -c 'case 0x'`) |
| 3 | `cpu_state_t` is **688 bytes**; `src/cpu` is **65,244** lines (`.c`+`.h`), of which the hand-written instruction bodies are **12,997**. | **MEASURED** (`sizeof`, `cat \| wc -l`) |
| 4 | The 32-bit shape is *pervasive*: `instruction_pointer` appears **322** times in 24 files, `previous_ip` **112** in 19, `cpu->reg32` **81** in 7. | **MEASURED** (`grep -ro`) |
| 5 | `EFER_LME`/`EFER_LMA` are **not defined anywhere**; `cpu->efer` is read at exactly two places and only for `EFER_NXE` (`src/cpu/paging.c:101,132`). `wrmsr(EFER)` stores the guest's value **unchecked** (`src/cpu/instructions_0f.c:6293-6298`, the assert was dropped under the S2 rule). | **READ** |
| 6 | `CPUID.80000001h:EDX` = `0x0190A179` — **no bit 29 (`lm`), no bit 11 (`syscall`), no bit 26 (`pdpe1gb`)** — and `CPUID.80000008h:EAX` reports a **32-bit** linear-address width (`src/cpu/instructions_0f.c:1212-1219, 1230-1236`). | **READ** |
| 7 | The bzImage loader is `load_elf` and it **rejects a non-32-bit ELF** (`src/main.c:314`: `if (image[4] != 1) die("elf: not 32-bit")`). The M0 image takes the **address-table** branch (`tests/test.asm:22-30`), the nasm corpus takes the ELF branch — so **both** loader paths are already proven in-tree. | **READ** |
| 8 | `tests/mkfixture.c` runs the image natively and its `regs64`/`fpr64` structs are the **x86_64** `PTRACE_GETREGS`/`GETFPREGS` layouts, including `r8`–`r15` and `fs_base`/`gs_base`. | **READ** (`tests/mkfixture.c:29-44`) |
| 9 | The host is an **AMD Ryzen 9 3950X** with `lm`, `pae`, `nx` and **`cx16`** in `/proc/cpuinfo` — so `CMPXCHG16B` *is* inside the host oracle's reach. | **MEASURED** |
| 10 | The lazy-flag cores are width-limited at 32 bits by construction: `OPSIZE_8/16/32` are *bit positions* `7/15/31` (`src/cpu/cpu_state.h:77-79`) and `opsize_to_mask` is `(2u << op_size) - 1` (`src/cpu/flags.h:64-67`) — `2u << 64` is UB, so `OPSIZE_64` needs a widened core, not a bigger number. | **READ** |
| 11 | The uop decoder has **no mode awareness at all**: it keys on `cpu->is_32` via `dec_osize_32()` (`src/cpu/uop_cache.c:94-97`) and treats `0x40`–`0x4F` as INC/DEC by table. Fed 64-bit bytes it would decode a REX prefix as an *instruction*, advance the stream by the wrong length, and desynchronise — **silently**. | **READ** |
| 12 | The JIT's recipe lookup is keyed on the semantic body's function pointer and refuses on shape mismatch (`src/cpu/jit_codegen.c:1098-1127`), so a body with no recipe **falls back to the C executor by construction**. Long mode needs no JIT refusal. | **READ** |

**C1 — THE 64-BIT CORPUS CANNOT BE AN EXTENSION OF THE EXISTING CORPUS, AND THE DIFFERENCE MATTERS.**
The brief says "a 64-bit extension of the nasm differential corpus". The 123 files in
`v86/tests/nasm/` are 32-bit *sources* (`header.inc` sets `esp`, uses `reg32`); they cannot be
re-assembled as 64-bit tests, and there is no table to generate a 64-bit corpus *from*. So the
64-bit corpus must be **authored and generated by the port**. That is a materially weaker
instrument than the 32-bit one: upstream's tests are *independent* of this port, ours are not, and
"corpus green" then means "the tests we thought of pass". §2.1.6 states the mitigation (the corpus
is written from the SDM's encoding map, not from the bodies; the coverage census is published per
slice) and it is never worded upward.

**C2 — THE RUNG EXPECTATION IS RESOLVED AGAINST THE HOUSE RULE, AS THE KVM PLAN ALREADY DID.**
The brief expects slices touching state translation, the page walk, mode transitions or the decoder
to *start* at `engineer`. `~/.config/hax/AGENTS.md` (user-stated 2026-10-11) says every unit starts
at `coder` and a rung is never skipped because a later one *might* be stronger; `results/kvm-plan.md`
§3 C3 settled the identical conflict the same way. This plan follows the house rule for **every**
slice and gives each slice an explicit **escalation criterion** (what counts as a demonstrated
failure) so the rung climb is a decision with evidence, not a mood.

**C3 — THE SEAM IS NOT THREE PEERS; IT IS ONE ENGINE AXIS OVER A CASCADE.** The JIT is not a third
engine beside the interpreter and the uop cache: `uop_run` *contains* the compiled-block path
(`src/cpu/uop_cache.c:1334-1420`) and the JIT's exit kinds are uop_run's own
(`src/cpu/jit_codegen.h:44-48`). A contract drawn as "three interchangeable engines" would force
re-deriving that layering. §3 draws it as one **engine** contract with three *instances*, of which
two share a substrate.

**C4 — THE HOST ORACLE IS BIGGER THAN THE BRIEF THINKS IN ONE DIRECTION AND SMALLER IN ANOTHER.**
Bigger: finding 8 (the harness already speaks x86-64 and already runs natively) and finding 7 (both
multiboot loader paths are exercised today). Smaller: the host cannot run privileged state, MSRs,
paging, interrupts or SYSCALL/SYSRET/SWAPGS from ring 3 at all, *and* it is not even a valid oracle
for architecturally **undefined** results (undefined flags after an oversized shift, undefined
behaviour on a faulting instruction's partial effects). §2.1.5 makes the exclusion a rule of the
corpus, because an undefined case silently "verified" against one host is worse than an untested
one.

**C5 — A LIVE DEFECT CLASS EXISTS TODAY, AND IT IS THE CHEAPEST THING IN THIS PLAN.**
`EFER_LME` is dead: nothing reads it. A guest that writes `EFER.LME` and then sets `CR0.PG` with
`CR4.PAE` gets `load_pdpte(cr3)` (`src/cpu/paging.c:538`) reading a **PML4** as if it were a
PDPT, then a 3-level walk over 4-level tables — either a plausible-looking wrong translation or a
`#PF` about a reserved bit, and **nothing in the machine says which**. This is 5 lines to refuse by
name and it belongs in S2, before a single 64-bit instruction exists.

---

## 2. The oracle: three layers, three different answers

### 2.1 Layer 1 — the ISA, with the host CPU as the reference

**2.1.1 The mechanism.** The 32-bit corpus's shape is: *assemble a fixture, run it natively, compare
machine state* (`tests/nasm_run.py:1-30`). The native run is `tests/mkfixture.c` (ptrace: break at
the `loop` symbol via a poked `int3`, dump regs/x87/MMX/XMM/an 8 KiB window of `.bss`/eflags as a
JSON array; `tests/mkfixture.c:76-215`). The actual run is the port, printing **the same document**
(`src/main.c:590-635`). The comparison is a transcription of upstream's `run.js:384-510`
(`tests/nasm_run.py:163-260`): `reg32[8]`, the 8 KiB memory window, `eflags & MASK_ARITH`, the
fpu/MMX branch keyed on `FPU_TAG_ALL_INVALID`, the masked FPU status word, the 7-digit float
tolerance, the exception kind and its `eip`.

**64-bit generalises every one of those except the register file and the width of the values.** The
new pieces are:

| piece | what | cost |
|---|---|---|
| the image | one flat 64-bit image, **two entry points**: the *stub* (32-bit, sets up a PML4 + identity map, sets CR4.PAE, EFER.LME, CR0.PG, loads a 64-bit CS, jumps) and the *body* (the test) | authored once |
| the native leg | `tests/mkfixture64.c`: **mmap the flat image at its link base with `MAP_FIXED`, set `rsp`, jump to `body_off`** — the harness becomes the loader | ~250 lines |
| the emulated leg | the port enters the **stub** at the multiboot address-table entry (`src/main.c:284-305`) and reaches the body itself | 0 new loader code |
| the document | **format v2**: 16 × u64 GPRs, `rip`, the same x87/MMX/XMM/memory block, `rflags & MASK_ARITH`, exception kind + `rip` | a second printer `--fixture64` (~60 lines in `main.c`) |
| the runner | `tests/nasm64_run.py` + `tests/nasm64_skip.txt` — **beside** `nasm_run.py`, never editing it (the `2116`-arity check at `tests/nasm_run.py:253-282` is a property of the 32-bit contract and must not be taught a second shape) | ~300 lines |

**Why the harness becomes the loader (the one design decision here worth arguing).** The alternative
is to make the corpus image a genuine 64-bit ELF and teach `load_elf` the ELFCLASS64 header — ~30
lines in a **shipped** file (`src/main.c:308-355`) that a guest can reach. The mmap harness puts the
new code in a **test rig**, where a mistake is loud (no document, or a document that mismatches on
its first word) instead of in the loader, where a mistake is the guest-reachable defect class this
tree has paid for three times. It also removes the two-entry-point `ld` gymnastics: the native leg
never executes the stub, so the stub can be plain 32-bit code at a fixed address with no ELF
constraints on it at all. **Fallback if the mmap base fights a hardened host** (`vm.mmap_min_addr`,
a PIE-by-default toolchain): the ELF64 branch in `load_elf`, gated by the corpus itself.

**2.1.2 The stub, and what it is allowed to assume.** The stub is the corpus's *machine setup*, not
a test: identity-map the first 4 MiB with 4 KiB pages through a real PML4 → PDPT → PD → PT, set
`CR4.PAE`, write `EFER.LME`, set `CR0.PG`, `ljmp` to a 64-bit code segment, zero the GPRs except
`rsp`, and jump to the body. It is *inside* the corpus's coverage: its own success is asserted by
every other test (the body cannot run otherwise) and its failure is a named stop. It must **not**
use anything the slice under test has not landed, which is why the address map is 4 KiB pages only
at S5 and gains 2 MiB/1 GiB pages with their own tests later (§6).

**2.1.3 The comparison.** Field for field as the 32-bit comparison, plus:

- **the compared flag mask is NOT widened in the first slice.** `rflags & MASK_ARITH` is upstream's
  mask. The 64-bit document *prints* the full `rflags`, but the comparison starts on the same
  `MASK_ARITH` bits, because a mask is a claim about what the emulator models and the first slice
  should not claim more than the 32-bit lane does. Widening it is a **separate, countable change**
  with its own red/green — and a candidate one, since `SF`/`ZF`/`OF` at 64-bit width are exactly
  what the corpus's ALU family exercises.
- **the memory window is the same 8 KiB at `0x100000`** (`.bss`), so the existing window model
  (`tests/reviewfix-pmtimer.asm`-style independent models) transfers.
- **`rip` replaces `eip` and is compared at 64 bits** — that alone catches every decoder-length
  error in the corpus, because a wrong instruction length desynchronises the stream and the final
  `rip` moves.

**2.1.4 What the host oracle *can* cover** (each is a corpus family in §6):

all data movement, arithmetic, logical, shift/rotate, bit-test and bit-scan operations at 16/32/64;
REX.R/X/B register-extension (R8–R15) and REX.W; the REX-only byte registers `spl/bpl/sil/dil` and
their difference from the legacy `ah/ch/dh/bh`; `MOVSXD`; `MOV r64, imm64`; 64-bit addressing
(`mod=00 rm=101` RIP-relative, `mod=01/10`, SIB with 64-bit base and index, and the `0x67`
address-size override, `moffs` with a 64-bit offset); `LEA`; `CMPXCHG16B` (**MEASURED**: `cx16` on
this host); 64-bit `push`/`pop`/`call`/`ret` plus the 16-bit and 64-bit `push imm` forms long mode
adds; the sign-extension boundaries (`0x7FFFFFFF`/`0x80000000`, `0xFFFFFFFF`/`0x100000000`, `-1`);
`IMUL`/`MUL`/`IDIV`/`DIV` including the 128-bit results; `SHLD`/`SHRD`; `BSWAP`;
**non-canonical-address `#GP`** (a ring-3 load/store of `0x0000800000000000` faults on the host —
this is the one paging-adjacent rule the host *can* check, and §2.2 uses it); `x87`/`MMX`/`SSE`/`SSE2`
with the new addressing and the extended register fields; and the exception triplet `#DE`/`#UD`/`#GP`
with the faulting `rip`.

**2.1.5 What it *cannot* cover — and the rule that follows.**

*Structurally out of reach* (the host will not run it from ring 3): `MOV CR*`/`DR*` semantics;
`WRMSR`/`RDMSR` (any of them); the 4-level walk's bit composition (present/RW/US/NX/PS and the
reserved-bit rules), because a ring-3 process cannot run on a `CR3` of its choosing; `INVLPG`;
interrupt/exception **delivery** (the IDT, the 64-bit interrupt frame, `iretq`, IST stack switches,
CPL changes); the 64-bit `GDT`/`IDT`/`TSS` encodings; `SYSCALL`/`SYSRET`/`SWAPGS`; the
`FS`/`GS`-base MSRs; `EFER`'s `LME`/`LMA`/`SCE`; `#PF` error-code bits; `HLT`/`CLI`/`STI`; `CPUID`
parity (the host's own leaves are the host's, and a port that copies them is copying a model it did
not choose).

*Out of reach by rule* (**C4**): anything the SDM leaves **undefined**. The corpus may contain
**only** tests whose outcome the SDM states. Undefined cases (the flags after a shift by ≥ operand
size, the destination of a faulting `CMPXCHG16B`, the partial effects of a faulting `REP MOVS`) are
**excluded from the corpus** and decided in the port by a stated rule with a named source — because
a host that happens to do X is not evidence about a model that is allowed to do Y.

**2.1.6 Coverage discipline (the mitigation for C1).** The corpus is **generated from a declarative
encoding list** (`gen/nasm64_gen.py`), written from the SDM's opcode map and *not* from the bodies
the port wrote. Every slice's corpus family is a list of `(encoding, operand-class, edge-case)`
triples, and every slice publishes its **census**: how many of the 873-entry port table's encodings
now have a long-mode arm, how many of those have a corpus test, and how many have *neither*. The
census is a measurement printed by the generator, exactly like `gen/interpreter_c.js`'s
"`N` call sites, `M` unimplemented" census (`gen/interpreter_c.js:545-556`) — so "how much of long
mode is live" is never a guess, and a reader can see the holes.

**2.1.7 The negative control (a corpus that cannot fail is not a corpus).** Two, both available
before any 64-bit code exists: (a) **perturb a fixture** by one word and the comparison must fail —
the 32-bit runner already has the flag for this (`tests/nasm_run.py`'s `--reuse-fixtures`, described
at its argparse as the negative control's mechanism) and the 64-bit runner copies the shape; (b)
**RED before green**: because an unimplemented encoding in this port is a **named stop**
(`unimplemented_instruction`, `src/cpu/cpu.c:66-70`), a slice's corpus family run against the
*previous* slice's binary reports `unimplemented: <symbol>` for every test — which is a specific,
mechanical red state, not a hang. That is the standard falsifier for every ISA slice in §6.

### 2.2 Layer 2 — the system layer: what replaces the oracle, item by item

**2.2.1 The one place the port's existing work pays off.** PAE is ported
(`src/cpu/paging.c:119-330`): the walk already reads 64-bit entries, already applies the
"high half 0 except NX" reserved mask (`:145-147`), already composes `PSE` 2 MiB pages, already
carries `NX` through the 4 KiB path (`:229-236`), and already caches `reg_pdpte[4]` via
`load_pdpte`. **The 4-level walk is that walk with one more level, one more page size, and a
canonicality test.** What is genuinely new is: the PML4 level; 1 GiB pages; the 48-bit virtual
address and the canonical-address check; the `CR3`-is-a-PML4 rule (`EFER.LMA`); the 64-bit TLB key
(§4); and the `#PF` error-code bits that only exist at 4 levels. Everything else is *already
reference-verified* by the PAE gates (`tests/gate-pae-1.sh`, `tests/gate-pae-l3.sh`) — which is why
this plan's biggest single cost is the ISA layer, not the walk.

**2.2.2 The instruments, and per item what is left uncovered.** Column 4 is the honest seat: an
item with no instrument does not get implemented silently, it gets a **named gap** in the unit
report.

| item | instrument | how it can fail | what it cannot tell us |
|---|---|---|---|
| PML4 level, 4 KiB/2 MiB/1 GiB pages | a synthetic 64-bit guest image (the `tests/pae-guest.asm` shape, extended: build the tables, walk them, discriminate 1 GiB vs 2 MiB vs 4 KiB by the high bits, take a `#PF` on a non-present entry) + a **mutation probe** per rule | flip a reserved-bit rule, a PS-bit rule, an A/D write-back → the image's own verdict word leaves GOLD | nothing about the real kernel's table layout |
| canonicality (`#GP` on a non-canonical linear address) | **the host** (§2.1.4) for the fault, plus the synthetic image for the `#GP` vector and the saved `rip` | remove the check → the host test passes but the in-guest test `#PF`s instead of `#GP`s | the exact `rip` a real CPU saves for a multi-byte instruction |
| `NX`, `SMEP` | the synthetic image (NX) + the kernel's own "NX (Execute Disable) protection: active" line | NX ignored → the image's `#PF` never fires; SMEP mis-wired → a kernel boot line changes | `SMAP` (deferred, §8) |
| mode transitions (`LME`→`LMA`, the CS.L descriptor, the `#GP` rules) | spec-derived selftest over `set_cr0`/`set_cr4`/`wrmsr(EFER)`/`far_jump`, one case per SDM rule, each with a mutation probe; plus the guest boot itself | drop the "`CR4.PAE` required" rule → the selftest's `#GP` case fails | the transition's *timing* versus a real CPU's pipeline |
| 64-bit GDT/IDT/TSS, IST, the interrupt frame (`iretq`) | selftest (the descriptor encodings, one case per field) + **the guest**: a kernel interrupt is unmissable — the boot dies | a wrong 16-byte gate layout → the kernel triple-faults; a wrong `iretq` frame → the first timer tick never returns | the exact delivery order for simultaneous exceptions |
| `SYSCALL`/`SYSRET`/`SWAPGS` | **the guest's userspace** is the assertion (§2.3): a Linux user process cannot exist without them; plus a selftest per MSR field and per CPL rule | any of the five MSRs mis-wired → the kernel panics at the first user `syscall` | `SYSRET`'s `#GP`-on-non-canonical corner (a kernel-avoided bug class; named gap) |
| `FS`/`GS` base MSRs | the guest (per-CPU `swapgs` is load-bearing in Linux) + a selftest | the boot dies at the per-CPU setup | nothing material |
| `#PF` error-code bits at 4 levels | selftest per bit (P/W/US/RSVD/I-D) | one bit dropped → the selftest's mask fails; a real kernel's `bad_area` messages change | the bits a real CPU sets on a *nested* fault |
| `CPUID` (the declaration) | **structural**: the CPUID model becomes a function of the backend's declared modes (§3.5), plus a selftest that a non-long-mode backend cannot advertise `lm`; plus the guest (a kernel *refuses to start* without `lm` — a decisive asymmetry) | advertise `lm` without the implementation → the kernel enters long mode and dies obscurely; refuse it → the kernel prints its own refusal | vendor-specific leaves (deliberately not copied) |

**2.2.3 What is deliberately NOT used as an oracle.** **QEMU** is *not* used as a per-instruction or
per-walk reference. It would be a second reference this tree cannot pin byte-for-byte (its state
document is its own), its value over the guest gate is small, and the tree's rule is that a control
establishes attributability, never exclusivity. QEMU's *existing* role in this tree — the
`qemu-system-x86_64`+KVM pre-check in `tests/gate-m2-5.sh`'s gate 1b — is exactly right and is
reused at the guest layer only (§2.3). A QEMU *differential of the 4-level walk* is listed in §8 as
not decided: it is implementable, and it is a second reference whose trust cost has not been paid.

### 2.3 Layer 3 — the guest: what proves long mode

**The artifact.** The tree pins kernels by sha256 in `tests/images/` with a documented provenance
(`tests/gate-pae-l3.sh:43,59,73`). The 64-bit gate's artifact is the **same Debian 6.1.0-50 source
the PAE lane already pins, in the `amd64` flavour** — same version, same config family, one
architecture different, so a boot-message difference is *the architecture*, not the kernel. Its
sha256 goes in the gate script beside `SHA_KERNEL`, and the initramfs builder
(`tests/mkinitramfs*.py`) reuses the `guest-initramfs-pae.img` shape for the amd64 userspace.
**PROJECTION:** `vmlinuz-6.1.0-50-amd64` is ~5.9 MB (the 686-pae pin is 5,759,488 B) and boots in
the existing 64/80 MiB machine; both must be measured, not assumed.

**The boot path needs nothing — this is the plan's most useful negative finding.** An x86-64 Linux
bzImage starts its setup code in **16-bit real mode** and enters long mode itself, so
`tests/minibios.asm` (16-bit firmware handing off to the kernel's own setup, see its file header)
and `src/hw/kernel.c` are **unchanged**. The gate script is the PAE-L3 gate with a different
`KERNEL=`, and that is a real claim about the design: a 64-bit guest needs no new boot path.
**INTERPRETATION**, and it rests on the x86 boot protocol rather than on this tree's design: a
bzImage's setup code is 16-bit real mode by protocol, so both flavours enter the same way and the
difference is inside the kernel. The first run is what confirms it, and it is a one-line check
(does the kernel's early output appear at all).

**The assertions, in the order the machine reaches them** (each a `PROJECTION` until the first run
pins the exact string — the implementer must pin them from the first green run and must not
transcribe this table as if it were measured):

1. **the kernel's own CPU test** passes: an x86-64 kernel prints its refusal ("This kernel requires
   an x86-64 CPU…") and halts when `CPUID.80000001h:EDX[29]` is clear. So *the CPUID declaration is
   gated for free by the kernel's refusal asymmetry* — flag off → refusal; flag on → the next line.
2. the early long-mode lines (`NX (Execute Disable) protection: active` is the PAE lane's own
   decisive string and stays decisive; the 64-bit lane adds the memory-map and per-CPU lines).
3. a **second leg**: the same kernel on the host's `qemu-system-x86_64` (the 1b shape), asserting
   the same serial lines — an independent implementation of the same guest.
4. **userspace**: the initramfs's own `fx-init`-style lines. This is the decisive milestone and the
   only one that proves `SYSCALL`/`SYSRET`/`SWAPGS`/`FS`-base/the user-kernel 4-level split, exactly
   as PAE-L3's FULL arm taught (`tests/gate-pae-l3.sh`: "reaching userspace" is the honest seat the
   3G arm could not assert).
5. **the flag-off machine is byte-identical**: `make transcript-diff` and `make determinism` with
   `--long-mode off`, which is the default (§3.5).

---

## 3. The backend seam

### 3.1 What exists today (READ)

One dispatch point, no interface:

```
cycle_internal()                        src/cpu/cpu.c:189-211
  phys_addr = get_phys_eip()            :196
  if (uop_cache_enabled) uop_run(phys_addr);  :205   <- the ONE choice
  else                   interpreted_run(phys_addr);
```

and the "engines" are a cascade behind that one boolean:

- `interpreted_run` (`src/cpu/cpu.c:126-186`) — the generated switch, one instruction at a time;
- `uop_run` (`src/cpu/uop_cache.c:1251-1470`) — decoded blocks; *inside* its block loop a cached
  block with `code != NULL` is executed by `jit_execute_block` (`:1334-1345`), else by the uop
  executor. So **the JIT is a property of a block record**, not a sibling engine (**C3**);
- `uop_run` falls back to `interpreted_run` per batch whenever the decoder produces no block
  (`:1270-1290`).

The switches are **process-global booleans** set by argv parsing: `uop_cache_enabled`
(`src/cpu/uop_cache.h:324`, set at `src/main.c:934-960`), `jit_enabled`
(`src/cpu/jit_mem.h:83`, set at `src/main.c:883-905`), and `--uop-cache off` is *defined* as
"exactly the pre-M2 interpreter" (`src/cpu/uop_cache.h:321-323`). Other globals the engine half
touches: `uop_pending` (`src/cpu/uop_cache.h:339`), `jit_block_boundary` (`src/cpu/cpu.c:56`),
`interpreted_unimplemented` + its name (`src/cpu/cpu.c:58-74`), `jit_abort_flag`
(`src/cpu/jit_mem.h:94`), `uop_page_code` (`src/cpu/uop_cache.h:392`).

### 3.2 The contract

The precedent is this tree's own: `src/hw/vga.h:134-153` — the 11 methods the device actually calls,
an opaque state pointer, three adapters, the headless one the default (`vga_screen_t`). The CPU seam
is drawn the same way, and the whole of it is:

```c
/* src/cpu/cpu_backend.h (new, S0) */
typedef struct cpu_backend {
    const char *name;           /* "interp", "uop", "uopjit", and one day "kvm" */
    uint32_t modes;             /* CPU_MODE_* bitmask: what this backend can RUN */
    bool (*init)(void);         /* allocate what it needs; false => refuse, the machine picks
                                 * the previous backend and SAYS SO */
    void (*run_batch)(uint32_t phys_addr);  /* cycle_internal's engine half: run until a
                                 * reference batch end.  Returns; the dispatcher does the rest */
    void (*clear_caches)(void); /* the reboot re-entry path (uop_cache_clear + jit_abort_flag) */
    void (*print_stats)(void);  /* --uop-cache-stats / --jit-stats, to stderr, or NULL */
} cpu_backend_t;

const cpu_backend_t *cpu_backend_select(const char *name);  /* refuses an unknown name by name */
const cpu_backend_t *cpu_backend_current(void);
bool cpu_backend_can(uint32_t mode);   /* the mode-refusal query (§3.3) */
uint32_t cpu_mode_of(const cpu_state_t *cpu);  /* real / 16-prot / 32-prot / long / compat */
```

That is deliberately **four function pointers, a name and a capability word**. Anything more is a
reimplementation of the cascade (**C3**).

### 3.3 What is CONTRACT, what is an implementation detail

The brief asks which of today's globals are contract. Answer, one line each:

| global | verdict | why |
|---|---|---|
| the reference's **batch structure** and the counter staircase (`uop_pending`'s *meaning*, `instruction_counter`'s flush points) | **CONTRACT** | the clock is `instruction_counter / 1000` and is guest-observable (`docs/architecture.md` §2 "The staircase"); a backend that flushes elsewhere moves the guest's clock. `uop_pending` *itself* is the uop backend's internal and must not be read by the dispatcher |
| `cpu->in_hlt`, `cpu->reboot_requested`, the stop channel (`interpreted_unimplemented` + name), `cpu->pagefault` | **CONTRACT** | the machine's only ways to stop; they are `cpu_state_t` fields or `cpu.c` accessors, not backend state |
| `uop_cache_enabled`, `jit_enabled` | **implementation detail** (becomes backend identity) | S0 re-expresses them as the selected instance and keeps the flags as aliases so no gate script changes |
| `uop_pending`, `uop_page_code`, `jit_abort_flag`, `jit_block_boundary` | **implementation detail** | backend-internal; only the backend's own `run_batch` and the invalidation hook touch them |
| `jit_block_boundary` | **detail with a contract shadow** | the *reference's* "this instruction ends a batch" is contract; the port spells it as this flag. A second backend must reproduce the batch end, not the flag |
| `mem8`, `memory_size`, `memory_devices`, `vga_mem8`, the TLB, `cpu->cr[]`, `EFLAGS`, the lazy-flag fields | **shared model, outside the contract** | a backend *implements instruction semantics over* the model; it does not own it. This is what makes a KVM backend possible at all: KVM keeps `mem8` as a memslot and the device hook table as its MMIO path (`results/kvm-plan.md` §1) |
| the uop arena/caps, the JIT pool, the recipe table, `UOP_MAX_*` | **implementation detail** | never named outside the backend |

**The mode-refusal invariant, stated as something a checker can hold:**

> `cycle_internal` asserts `cpu_backend_can(cpu_mode_of(cpu))` **before** calling `run_batch`, and
> the assertion is a **named stop** (`"cpu backend 'interp' cannot run long mode"`), never a silent
> path. The premise that makes a per-*cycle* check sufficient rather than per-instruction is a
> property of the reference: every instruction that can change the mode (192 `block_boundary`
> entries, plus `MOV CR*`/`MOV sreg`/`far` transfers) **ends the batch**, and the dispatcher
> re-checks at the next `cycle_internal`. The invariant is a `READ` of
> `v86/gen/x86_table.js` + `src/cpu/cpu.c:161-178`, and S2 pins it with a probe that changes the
> mode inside a batch and requires the check to fire.

### 3.4 How the three existing engines land on it, with no byte moved

| backend | `run_batch` | `modes` | what is forced |
|---|---|---|---|
| `interp` | `interpreted_run` | `16 | 32P` (until §6's S5) | nothing |
| `uop` | `uop_run` with the compiled path **disabled for this instance** | `16 | 32P` | today `--uop-cache on --jit off` |
| `uopjit` | `uop_run` with the compiled path enabled | `16 | 32P` | today `--uop-cache on --jit on` (the shipped default) |

Selection stays **exactly** the current flags: `--cpu-engine auto` (the default) resolves to
`uopjit`/`uop`/`interp` from the two existing booleans, and the booleans keep their names and
defaults. `--cpu-engine interp|uop|uopjit` is the *new* user-visible spelling that makes the seam
reachable and testable — a seat that cannot be selected is a seat nobody calls
(`AGENTS.md`'s "a seat that is never called is indistinguishable from an absent seat"), so S0 ships
the flag, and a selftest pins that each of the three selections resolves to the expected `name` and
`run_batch` symbol. **The gate for S0 is that nothing else moves** (§6).

### 3.5 The mode refusal, and the CPUID declaration coupled to it

Two rules, both structural rather than documentary:

1. **A backend that cannot run a mode must not advertise it.** The CPUID model
   (`src/cpu/instructions_0f.c:1116-1265`) currently returns constants; as of S2 it returns
   constants **conditioned on `cpu_backend_can(CPU_MODE_LONG)`** for exactly the bits that declare
   long mode: `80000001h:EDX[29]` (`lm`), `[:11]` (`syscall`), `[:26]` (`pdpe1gb`), and
   `80000008h:EAX`'s linear-width byte (32 → 48). This is the ACPI-FADT lesson applied pre-emptively
   (`docs/architecture.md` §5: "a wrong declaration in [the firmware tables] is indistinguishable
   from a broken device"), and it is what makes the kernel's own `CPUID` refusal a *gate on the
   seam* rather than on the decoder.
2. **Entering long mode requires a backend that declares it.** `set_cr0`'s `PG` arm and
   `wrmsr(EFER)`'s `LME` bit check `cpu_backend_can(CPU_MODE_LONG)` and stop by name otherwise.
   **C5**'s live defect closes here: from S2 on, `EFER.LME` + `CR0.PG` on a backend that cannot run
   long mode is a named stop, not a 3-level walk over 4-level tables.

### 3.6 Where the 64-bit decoder/dispatch lives (and the generated-vs-hand-written contract)

Three candidates, one chosen:

- **(a) a third operand-size arm in the existing emitter, fed by a second (authored) table** —
  **CHOSEN**;
- (b) a separate hand-written long-mode dispatcher — rejected: it forks the dispatch, and
  `docs/architecture.md` §7 says the dispatch is generated and the semantics are not;
- (c) a prefix-driven split inside the interpreter (`if (is_long_mode()) …` per arm) — rejected: it
  duplicates every modrm-bearing arm (586 entries) inside the *generated* file, which is where a
  hand decision is hardest to see.

**The chosen shape, concretely:**

1. The dispatch key becomes **three-valued**: `run_instruction(opcode | (os_index << 8))` with
   `os_index ∈ {0,1,2}` = 16/32/64-bit operand size. It has exactly **three** call sites:
   `src/cpu/cpu.c:154` (`interpreted_run`), `src/cpu/uop_cache.c` (the mid-batch fallback step), and
   `src/cpu/instructions.c:92-98` (`run_prefix_instruction`). A third arm exists **only** where the
   new table says so.
2. `gen/x86_table64.js` (new) is a **sparse overlay keyed by `(opcode, prefix arm, fixed_g)`**, not a
   second copy of the 873 entries. It carries: `w64: 1` for an encoding that has a 64-bit form
   (route `os_index == 2` to `instr64_*`), `body: WRAP|AUTHORED` (see §6's census), `default64: 1`
   for the encodings whose *unprefixed* long-mode operand size is 64 (push/pop/call/ret/jmp,
   `MOVSXD`), and the long-mode **semantic changes**: `0x40`–`0x4F` stop being INC/DEC and become
   REX; `0x63` becomes `MOVSXD`; `0x9A`/`0xEA` become invalid; `0xC4`/`0xC5` (VEX-encoded) become
   `#UD`; `0F05` becomes `SYSCALL`.
3. **An encoding with no 64-bit arm in the overlay is a NAMED STOP**, never a fall-through:
   the emitter prints `unimplemented_instruction("long mode: 0xNN has no 64-bit form in
   gen/x86_table64.js")`. That is the mechanism that lets the lane be built in slices with no
   silent-wrong window, and it is what makes §2.1.7's RED state mechanical.
4. The **availability scan** (`gen/interpreter_c.js:96-110`) is unchanged: a `w64` entry whose
   `instr64_*` body is not yet defined emits the named stop by symbol, and the census says how many.
5. **REX is a prefix with its own state.** `cpu->prefixes` is a `uint8_t` at upstream's wasm offset
   648 (`src/cpu/cpu_state.h:278`); the REX nibble needs 4 more bits, so the field widens to
   `uint16_t` with the REX byte in bits 8-11 and the legacy bits where they are. The spec rule
   "a legacy prefix after REX cancels the REX" is then a test in the prefix dispatcher, and the
   `os_index` computation is one function: `rex.W ? 2 : (0x66 ? 0 : 1)` in long mode, else today's.
   `MOV r64, imm64` (`B8+r` with REX.W) and `moffs` with a 64-bit address need a **`read_imm64`**
   and a `UOP_IMM_64` kind — the two places where an immediate is 8 bytes wide.

---

## 4. The 32-bit-shaped state, field by field (Q2)

**Verdict legend: WIDEN** (change the field's type in place), **PARALLEL** (a second field/table
beside it), **UNCHANGED** (long mode does not touch it).

| what | where | verdict | reason / what it costs |
|---|---|---|---|
| `int32_t reg32[8]` | `cpu_state.h:249` | **WIDEN → `uint64_t reg64[8]`, renamed** | a field named `reg32` holding a 64-bit value is a comment that lies. 81 references, all mechanical. **The byte/word accessor's index formula changes** (`cpu_state.c:21`: `(index << 2 & 12) \| (index >> 2 & 1)` is the *flat* 4-byte layout; with 8-byte elements it becomes `(index & 3) * 8 + (index >> 2 & 1)`), and REX's `spl/bpl/sil/dil` need a second mapping for indices 4-7 with REX |
| `int32_t dreg[8]` | `cpu_state.h:287` | **WIDEN** | DR0-DR7 are 64-bit in long mode; `dreg[6]`/`dreg[7]` are the reset-visible ones (`RESET_DREG6/7`, `cpu_state.h:473-474`). Trivial, and it removes a "we don't model it" claim from the debug path |
| `int32_t instruction_pointer`, `previous_ip` | `cpu_state.h:263-264` | **WIDEN → `uint64_t`** | 322 + 112 references in 24/19 files. The one thing that makes it tractable is that **the fetch machinery is already page-relative**: `eip_phys = translate_fetch(eip) ^ eip` (`cpu.c:79-95`) generalises *unchanged* to 64-bit operands — `E ^ V` cancels the page bits and leaves `P` exactly, with the physical result below 2³² — and `phys` indexes `mem8` as a `uint32_t`. **Stated invariant (hardware-true, and the reason the 32-bit engines keep working): outside long mode `rip`'s high half is zero.** |
| `int32_t tlb_data[0x100000]`, `int32_t valid_tlb_entries[…]` | `paging.c:17-19`, `paging.h:35` | **PARALLEL** | 1M entries indexed by `addr >> 12` cannot key a 48-bit virtual page. A **new** small direct-mapped table (`(tag64, entry32)`), used only when `EFER.LMA`; the existing 4 MiB array is left **exactly** as it is so the 32-bit path's invalidation semantics do not move. Both flush from the same four entry points (`invlpg`, `set_cr3`, `clear_tlb`, `full_clear_tlb`) |
| `do_page_walk(int32_t addr, …)` | `paging.c:119` | **WIDEN + one level** | the PAE body is reused verbatim (see §2.2.1); new: the PML4 level, 1 GiB pages, and the canonicality test before the walk |
| the 32-bit physical address space (`uint32_t memory_size`, `in_mapped_range`) | `memory.h:107-110` | **UNCHANGED, with a stated bound and a named refusal** | see §5.1. A 4-level PTE can name a physical address above 4 GiB; the port cannot represent it, so it **stops by name** rather than truncating |
| `segment_offsets[8]`, `segment_limits[8]`, `sreg[8]`, `segment_is_null[8]` | `cpu_state.h:293-296` | **WIDEN the offsets**, limits/sreg **UNCHANGED** | in long mode CS/DS/ES/SS bases are 0 and limits are ignored; **FS/GS keep real 64-bit bases** (the MSRs), which is the whole reason `segment_offsets` must widen |
| `idtr_offset`, `gdtr_offset` | `cpu_state.h:265-268` | **WIDEN** | a `LGDT`/`LIDT` in long mode loads a 64-bit base (10-byte operand) |
| `last_op1`, `last_result`, `last_op_size` + the `OPSIZE_*` set | `cpu_state.h:253,255, 77-79`; `flags.c:12-229` | **WIDEN the two operands to `int64_t`, add `OPSIZE_64 = 63`, widen the cores** | C5-adjacent and the highest-value correctness item in the widening: `opsize_to_mask` is `(2u << op_size) - 1` (`flags.h:64-67`) and `2u << 64` is UB. The getters' `>> cpu->last_op_size` (`flags.c:12-61`) must move to `uint64_t`. The 16/32-bit behaviour is unchanged **iff** every `(uint32_t)` cast that is *about the operand* becomes `(uint64_t)` and every one that is *about `flags_changed`* stays 32-bit — that is the single silent-wrong seam in this slice, and the nasm corpus is its detector |
| `state_flags` | `cpu_state.h:254`; `cpu_state.c:99-108` | **WIDEN the mask set: add `CACHED_MASK_LONG`** | the byte is the decoded-block cache key half (`uop_cache.h:165`), so a fifth bit automatically invalidates any cross-mode reuse — a free correctness property |
| `cr[8]`, `efer`, `pat`, `current_tsc` | `cpu_state.h:271, 320-321, 304` | **UNCHANGED** | `cr[]` stays `int32_t` **because the physical model is 32-bit** (§5.1): a `CR3` above 4 GiB is exactly the case §5.1 refuses by name, so widening the field would widen a bound the machine does not have. (CR8 belongs to the APIC and is unused; `efer`/`pat` are already `uint64_t`) |
| `reg_xmm[8]`, `fpu_st[8]`, `fpu_*`, `mxcsr` | `cpu_state.h:298-327` | **UNCHANGED** | long mode adds no registers; SSE2 is the 64-bit baseline and the port has it |
| `prefixes` (`uint8_t`) | `cpu_state.h:278` | **WIDEN → `uint16_t`** | REX needs 4 more bits (§3.6.5) |
| the JIT's recipes | `jit_codegen.c:1098-1127` | **UNCHANGED code, ZERO 64-bit coverage initially** | lookup is by body pointer + shape; an `instr64_*` body has no recipe and **falls back to `uop_exec_uop` by construction** (finding 12). Long-mode JIT recipes are pure upside, later (§6 S13) |
| the uop cache's decoded forms | `uop_cache.h:118-147` | **WIDEN the tables (generated) + `disp` to `uint64_t`** | the descriptor tables are generated per `(opcode \| os<<8)` case; the overlay above gives them a third arm. **This is the silent-wrong channel** (finding 11) and the reason S5 must land the decoder's long-mode refusal *before* any 64-bit instruction |

**The verdict in one line:** this is an **extension**, and the extension's cost is concentrated in
exactly two places — the register file's byte-access formula, and the flags cores' width — both of
which are *contained functions* with a total gate already pointed at them.

---

## 5. The 64-bit model in the machine (Q3)

### 5.1 The physical address space: unchanged, with a bound and a refusal

`memory_size` is `uint32_t` and `mem8` is a host allocation of it (`src/cpu/memory.c:23-43`). Long
mode does not change that: **the machine is a 32-bit-physical machine with 64-bit virtual
addressing**, exactly the PAE situation the port already ships, and exactly what v86's own model is
(`TODO.md` §8: "a 32-bit physical address space throughout").

What a 64-bit guest needs and gets: physical addresses up to the memory top; the MMIO holes
(`0xA0000-0xBFFFF`, the 8 MiB LFB at `0xE0000000`, the APIC/IOAPIC windows) — all below 4 GiB.
What it does **not** get: RAM above 4 GiB. **The refusal, stated so it is never silent:** a
4-level PTE/PDE/PDPTE whose physical base is ≥ 4 GiB makes the walk **stop by name**
(`"physical address 0x… is above the port's 32-bit physical model"`). Truncating instead would turn
a guest's `#PF` into a device read — the silent-wrong class, and the same defect shape as the
`eip_phys` XOR's AUDIT-1 finding. Real Linux x86_64 on a 4 GiB-or-less machine never asks.

### 5.2 The virtual address space

48-bit canonical (`bits 63:48` = `bit 47`), 4-level, `CR4.LA57` **not** implemented — and not
silently: `MOV CR4` already refuses bit 12 in its reserved mask
(`src/cpu/instructions_0f.c:503-506`), which is the correct #GP for a non-LA57 CPU. A 5-level guest
therefore fails at `MOV CR4` exactly as it would on real hardware that does not have `LA57`, provided
`CPUID.7:ECX[16]` stays clear — which it does (`instructions_0f.c:1198-1205` reports only
`ERMSB`). **That is a *declared* bound, in the tree's own idiom.**

### 5.3 The boot path to long mode: nothing new is needed (and one thing is)

- **A real guest**: unchanged (§2.3). The kernel's own setup enters long mode. `minibios` and
  `kernel.c` do not grow. **INTERPRETATION**, and it rests on the x86 boot protocol rather than on
  this tree's design: a bzImage's setup code is 16-bit real mode by protocol, so both flavours enter
  the same way and the difference is inside the kernel. The first run is what confirms it, and it
  is a one-line check (does the kernel's early output appear at all).
- **The corpus**: the stub (§2.1.2), inside the image.
- **The transcript mode**: untouched. The M0 image is 32-bit and the M0 contract must not move, so
  long mode is **not** reachable through the transcript invocation at all — the flag exists only in
  the boot/fixture invocations. Stated because a reader will otherwise expect a `"rip"` field in the
  transcript one day: it is not coming, and that is deliberate.

### 5.4 The state document (and the KVM lane's checkpoint, later)

The M0 transcript (`src/main.c:487-565`) stays a 32-bit document. The **long-mode state document** is
`--fixture64`, one printer, two sections:

- `---BEGIN JSON---`: the **v2 compared array** — 16 × u64 GPRs, `rip`, x87/MMX/XMM, the 8 KiB
  memory window, `rflags & MASK_ARITH`, exception kind + `rip`. This is the corpus's actual leg and
  the two-leg comparison's input;
- `---BEGIN STATE---`: the **control tail** — `efer`, `cr0/cr2/cr3/cr4`, `gdtr/idtr` base+limit, the
  six segment bases/selectors/limits, `FS`/`GS`/`KERNEL_GS` base, the modelled MSR set, `state_flags`,
  the pending/flush bookkeeping. The corpus comparator **ignores** this section; the **64-bit engine
  differential** (interp vs uop vs jit, byte for byte) and the **KVM lane's state translation**
  (`results/kvm-plan.md` §4 T1, "a one-printer canonical state document") read it.

One printer, three consumers, and `sizeof(cpu_state_t) == 688` today (**MEASURED**) bounds how much
a checkpoint can be.

---

## 6. The slices

Fourteen slices. Each: what lands, its files, its **gate**, its **falsifier** (what makes the gate
fail), a **size** (PROJECTION) and a **rung** with its escalation criterion. Per **C2** every slice
**starts at `coder`**; the criterion column says what a demonstrated failure looks like, so the climb
to `engineer` is evidence-driven.

**The standard falsifier for every ISA slice (S5-S10)**: the slice's corpus family is written
**first**, run against the previous slice's binary, and must be **RED with a `unimplemented: <symbol>`
stop per test** (§2.1.7b); then it goes green. A slice whose corpus family is green before it lands
has tested nothing.

| id | what lands | gate | falsifier | size | rung / escalation |
|---|---|---|---|---|---|
| **S0** | **THE SEAM, INERT.** `cpu_backend.{h,c}`; `cycle_internal` routes through `->run_batch`; the three instances; `--cpu-engine` + the `name`/symbol selftest | **the whole existing battery, unmoved**: `make`, `make selftest`, `make nasm` 118/118, `make transcript-diff` byte-identical, `make determinism` 4,000,096,757, `make bench` 200,095,214, `sh tests/gate-reboot-1.sh` 39/39, the five VGA gates, `gate-pae-1.sh`, `gate-pae-l3.sh` | any of them moves; or the new selftest's three selections resolve to the wrong instance | ~450 | **coder** (a mechanical refactor whose gate is total). Escalate if any battery item moves in a way the diff does not explain in one pass |
| **S1** | **THE REGISTER FILE AND THE FLAGS AT 64 BITS, INERT.** `reg64[8]`, the byte-access formula, `dreg`, `last_op1/last_result` `int64_t`, `OPSIZE_64`, the widened cores, `CACHED_MASK_LONG` (unused yet) | the same battery **plus a NEW poison probe**: seed every widened field's high half and R8-R15 with `0xA5A5A5A5…`, run the M0 image, require a **byte-identical transcript** | the poison probe moves the transcript — a widened field leaked into a 32-bit path; or the nasm corpus drops below 118 (the flags cores are its subject) | ~700 changed + ~150 new | **coder**. Escalate on a second silent differential failure in the same class (*not* on the first: a first failure here is a bug, not a wall) |
| **S2** | **THE MODE PREDICATE AND THE REFUSALS.** `cpu_mode_of`, `modes`, `cpu_backend_can`; `EFER_LME`/`LMA`/`SCE` bits defined; `wrmsr(EFER)` and `set_cr0(PG)` refuse long mode when the backend cannot run it (**C5**); **CPUID gated on the declaration** (§3.5.1) | battery unmoved (nothing 32-bit sets LME) **+ the two refusals fire**: a probe image that sets `EFER.LME` then `CR0.PG` must now stop **by name** instead of taking a 3-level walk; a CPUID selftest must show `lm` clear with a 32-bit backend and set with a long-mode one | the probe image does **not** stop ⇒ the half-landed case still mis-runs; `transcript-diff` moves ⇒ a CPUID bit leaked | ~200 | **coder**. This is the cheapest and most valuable slice in the plan |
| **S3** | **THE ADDRESS/IP WIDENING, INERT.** `instruction_pointer`/`previous_ip`/`eip_phys`/`last_virt_eip`/`segment_offsets`/`idtr_offset`/`gdtr_offset` to 64-bit; `translate_address*` signatures; the uop block key; the JIT's `eip` stores | battery + the **poison probe** + a new **"high-half is zero outside long mode"** invariant asserted at the mode boundaries | poison probe moves the transcript; or a truncating cast turns up as a `mem8` index (the transcript's window hash) | ~900 changed | **coder**. Escalate (→ engineer) if the widening produces >1 unexplained differential failure: this is the slice the brief would have started at engineer, and the *evidence* is what should move it |
| **S4** | **THE HOST-CORPUS INFRASTRUCTURE.** `tests/mkfixture64.c` (mmap loader + jump), `tests/nasm64_run.py`, `gen/nasm64_gen.py`, `tests/nasm64/*.asm` (generated), `--fixture64`, `make nasm64` | **the negative controls**: a perturbed fixture must fail; every corpus test run against the *current* binary must be RED with a named stop; the harness's own `loop` breakpoint and the 8 KiB window must reproduce on a hand-written trivial body | a perturbed fixture passes ⇒ the comparison cannot fail; a corpus test **passes** before S5 ⇒ it tests nothing | ~900 | **coder** |
| **S5** | **THE FIRST VERTICAL SLICE**: stub → `LMA` → 64-bit fetch → `REX` decode → a `mov`/`add` subset → `hlt`; the minimal PML4 walk (4 KiB pages only); `prefixes` widened; `read_imm64`; `--long-mode on`; the uop decoder's **long-mode refusal** | corpus family 1 green (≥ 25 tests) **and** the 32-bit battery unmoved **and** the 64-bit engine differential available for the first time (interp is the only engine, so the differential is vacuous — stated, not implied) | family 1 red after the slice; or a 32-bit path moves | ~1,200 | **coder**. **Escalate on the first decoder failure** — this slice is the plan's centre of gravity and the one place a wrong instruction *length* is silent |
| **S6** | **ALU at 64 bits**: add/adc/sub/sbb/and/or/xor/cmp/test/inc/dec/neg/not, the `WRAP` bodies over the widened cores, plus the sign-extension boundaries | corpus family 2 | RED-before-green per encoding | ~350 body + corpus | **coder** (all `WRAP`) |
| **S7** | **Control flow and stack at 64 bits**: jcc/jmp/call/ret/push/pop/`mov imm64`/`lea`/`movsxd`/`movzx`/`movsx`/`cmov`/`setcc`/`xchg` | corpus family 3 | ditto; **plus** a `rip` comparison (a wrong length shows here first) | ~500 | **coder** |
| **S8** | **MUL/DIV/SHIFTS at 64 bits** — the 128-bit results, the count masks, the `OF` cases | corpus family 4 | ditto; the corpus must include the SDM's *defined* `OF` cases only (§2.1.5) | ~450 | **coder**. Escalate if the 128-bit results fight `long long` in the port's style (they may want `__int128`, a portability decision — see §8) |
| **S9** | **String/bit/atomic**: movs/stos/scas/lods/cmps (+`rep`), bt/bts/btr/btc/bsf/bsr/shld/shrd/bswap/xadd/cmpxchg/**cmpxchg16b** | corpus family 5 (with `cx16` tests, **MEASURED** available) | ditto | ~600 | **coder** |
| **S10** | **x87/MMX/SSE/SSE2 in long mode**: the addressing and the REX register fields, semantics unchanged | corpus family 6 | ditto; and the 32-bit FPU tests must not move | ~250 | **coder** |
| **S11** | **THE SYSTEM LAYER**: the full 4-level walk (1 GiB, canonicality, NX), the long-mode TLB, the 64-bit GDT/IDT/TSS/IST, the 64-bit interrupt frame, `FS`/`GS`-base MSRs, `SYSCALL`/`SYSRET`/`SWAPGS`, the long-mode MSR set | the synthetic 64-bit guest image (the `pae-guest.asm` shape) with a **mutation probe per rule**; the per-item table of §2.2.2 | a mutation that the selftest does not catch ⇒ that rule has no instrument | ~1,500 | **coder**. Escalate on the second round of the same class: this is the layer with no oracle and the one where the ladder will be climbed |
| **S12** | **THE GUEST**: the amd64 pin + a `gate-x86-64.sh` in the PAE-L3 shape; the QEMU leg; the userspace milestone | the kernel's own lines + the second leg + the flag-off byte-identity | the corpus-green-but-guest-dead case (a real finding, not a regression) | ~400 (gate + pin) | **coder** |
| **S13** | **THE ENGINES**: the uop decoder's long-mode arms (gated by the **64-bit engine differential**, interp vs uop, byte for byte); then the JIT recipes family by family | the differential per corpus family + the guest gate | a differential mismatch that the corpus does not have a test for ⇒ corpus gap, recorded | ~800 | **coder** |
| — | **AFTER THIS PLAN**: the KVM backend consumes the seam (§9). Explicitly **not** part of this work (the user has deferred it) | | | | |

**Slice order argument (the brief's Q1, argued rather than accepted).** S0 first, inert, is right
for the reason the brief gives (a seam landed with new semantics is a seam nobody can attribute a
breakage to) and for one more: S0's gate is the *only* gate in this plan that is a **total**
statement about the shipped tree, and it is worth having that measurement in hand before the tree
starts growing. S2 before S5 is the plan's own insertion: **the live defect (C5) closes for 200
lines and one probe**, and every later slice inherits a machine that *refuses* what it cannot do.
S4 before S5 is forced (a corpus with no way to execute is untestable) but S4 is still separable
from S5 because its own negative controls do not need 64-bit execution.

**Where this plan disagrees with the brief's expectation, explicitly:** the brief wants S0's gate to
be "the whole existing battery — a pure refactor proved inert". Agreed, and it is stated as the
gate. The brief also expects the *first* long-mode slice to be the backend seam; this plan inserts
S1/S2/S3 between them (a register file, a refusal, an address width) because they are each **inert**
and each closes a class of silent error *before* the semantics arrive — and an inert slice is cheaper
to attribute than a semantic one.

---

## 7. Risks, SILENT-WRONG first, each with its detector

**R1 (SILENT) THE UOP DECODER DECODES 64-BIT BYTES WITH 32-BIT RULES.** Finding 11. It reads
`0x40`–`0x4F` as INC/DEC and gets the instruction **length** wrong, desynchronising the stream while
producing plausible state. **Detector:** the S5 refusal (the decoder produces no block in long mode
until S13), and then the 64-bit engine differential (interp vs uop) per corpus family, byte for byte.
**This is the plan's top risk because it is the only silent channel that exists *before* any 64-bit
semantics land.**

**R2 (SILENT) A TRUNCATING CAST IN THE WIDENING.** 322 `instruction_pointer` references and 81
`reg32` ones; one `(int32_t)` on the wrong side of a comparison is invisible in 32-bit mode.
**Detector:** S1/S3's **poison probe** (high halves poisoned; the transcript must not move) and the
"high half is zero outside long mode" invariant. Both are able-to-fail by construction.

**R3 (SILENT) A FLAG CORE WIDENED THE WRONG WAY.** `getcf`/`getzf`'s `>> cpu->last_op_size` and
`getcf`'s `sub_mask = flags_changed >> 31` are 32-bit *about `flags_changed`* and must stay 32-bit
while the *operands* are 64-bit. Getting it backwards makes carry/zero wrong on **every** 64-bit ALU
result and right on every 32-bit one, so the existing battery stays green. **Detector:** the 64-bit
corpus's arithmetic family (S6) **must** include carry/borrow chains across the 32/64-bit boundary
(`adc` after a `add` that overflows 32 bits) — a family that only tests 64-bit operands with small
values would pass. And S1's nasm 118/118 stays the *inertness* detector.

**R4 (SILENT) A CORPUS TEST THAT CANNOT FAIL** — the tree's most-repeated defect (`docs/testing.md`
§3: the MMX comparison that could never fire, the high-byte blocker that passed every gate). A
64-bit corpus authored by the port is *maximally* exposed to this. **Detector:** §2.1.7's perturbed
fixture, per family, plus the rule that every family contains at least one test whose expected value
differs between the 32-bit and the 64-bit form (otherwise the test does not discriminate).

**R5 (SILENT) THE PHYSICAL-ADDRESS TRUNCATION.** §5.1. **Detector:** the walk's named refusal, plus a
selftest that feeds a PTE with a ≥4 GiB base and requires the stop.

**R6 (SILENT) THE `EFER.LME` + `CR0.PG` HALF-LANDED PATH** — live today (**C5**). **Detector:** S2's
probe image, which must go from "whatever happens" to "stop by name".

**R7 (LOUD, silent if worded away) THE SYSTEM LAYER'S GAPS.** Each item in §2.2.2's table that has no
instrument gets a **named gap** in its unit report — never a green gate over an untested rule. The
tree's rule: "where the correct thing cannot yet be built, build the seat, put the honest current
value in it, and state the gap explicitly".

**R8 (LOUD) THE GUEST REFUSES.** The x86-64 bzImage's own `verify_cpu` prints a refusal when `lm` is
absent and dies on a bad `boot_params` layout. **Detector:** the refusal *is* the detector (a
message asymmetry), and the kernel's first line is the gate's first assertion.

**R9 (PROCESS) INHERITING A PIN THE LANE CANNOT HOLD.** The amd64 kernel's sha256 must live in the
gate script beside `SHA_KERNEL` (`tests/gate-pae-l3.sh:59`), and every 64-bit gate states its own
claim and its own bound in its own header — the KVM lane's R9, same shape, same reason.

**R10 (PROJECTION, not a risk to design around) SIZE.** ~6,000-9,000 new/changed lines across 14
slices, most of it in S5/S11. `TODO.md` §8 said "months"; this plan's shape agrees and makes the
months *countable* (the census per slice) instead of a mood.

---

## 8. Not decided by this plan

Stated so a gap is never read as a promise.

1. **`SMAP`** (and `CR4.PKE`/`PKU`). Linux x86_64 boots without them; a kernel with
   `CONFIG_X86_SMAP=y` probes `CPUID.7:EBX[20]` and *disables* the feature if it is clear — so the
   honest answer today is "declare nothing, and the kernel turns it off". Implementing it is a
   separate slice with its own instrument. **Not scoped.**
2. **5-level paging (`LA57`)** — a *declared* non-goal (§5.2), refused at `MOV CR4` and unadvertised
   at `CPUID.7:ECX[16]`.
3. **RAM above 4 GiB** — a declared bound (§5.1). A 64-bit guest with more than 4 GiB of physical
   memory is not representable and the walk refuses by name.
4. **The 64-bit JIT's recipe coverage target.** §6 S13 lands the differential-gated cache path and
   *some* recipes; which shapes are worth inlining is a measurement this plan deliberately does not
   guess. The 32-bit lane's method (`results/jit-plan.md` §3's dynamic shape census) transfers.
5. **Whether QEMU ever becomes a per-walk differential.** Implementable; the trust cost (a second
   reference, its own state document, its own pin) is unpaid and §2.2.3 puts it out of scope.
6. **`__int128`** for the 64-bit `mul`/`div`'s 128-bit intermediates. It is a GCC/Clang extension
   and both of this tree's toolchains (cosmocc's gcc, the host cc) have it, but it is a new
   portability dependency and the port's house style has avoided the extension so far. **Decide in
   S8 with a measurement** (the alternative is a pair of 64-bit halves, which the portable
   `softfloat` usage proves is expressible).
7. **The exact `rflags` mask** the corpus compares at 64 bits (§2.1.3) — widening it is a separate,
   countable change with its own red/green.
8. **`FXSAVE`/`XRSTOR`/`XSAVE`** in long mode. `0F AE` is currently at whatever the 32-bit lane
   supports; the 64-bit form has a different layout (the `MXCSR_MASK`, the 64-bit `FIP`/`FOP`). A
   guest kernel uses `fxsave` in context switches, so this is **likely to be forced** by S12 —
   listed here because nothing in this plan measures it yet, and a slice may be inserted.
9. **Whether the amd64 guest boots to userspace inside the existing 80 MiB machine** (§2.3's
   PROJECTION). If it does not, the machine grows — which is a device-layer decision, not this
   plan's.
10. **The transcript's future.** It stays 32-bit (§5.3). If a later lane wants a 64-bit transcript,
    that is a *new* document with its own pin, not an extension of the M0 contract.

---

## 9. The relationship to the KVM lane

The user has deferred KVM until this work lands. What that ordering buys, and what the KVM backend
will and will not reuse:

**Reused, and the reason this plan is a prerequisite:**

- **the seam** (§3). A KVM backend is `cpu_backend_t` instance número cuatro: `run_batch` becomes
  `KVM_RUN` + the exit dispatch, `clear_caches` its own, `modes` its capability word. The KVM plan
  already frames the lane as "a SECOND MACHINE with its own contract" sharing `mem8`, the device
  hook table, the PIC/LAPIC/IOAPIC and the boot paths (`results/kvm-plan.md` §1) — that is exactly
  the "shared model, outside the contract" row of §3.3, and the seam is what makes the sharing
  expressible instead of implied.
- **the mode-refusal discipline** (§3.5): a KVM backend that cannot honour a mode (or a CPUID leaf)
  refuses by name, and the CPUID model is already conditioned on the capability word. The KVM lane's
  Declared-CPUID-Diff instrument (`results/kvm-plan.md` §14 R6) is the same rule at a different
  layer.
- **the state document** (§5.4). Its `---BEGIN STATE---` section is the KVM lane's state-translation
  *target* — `results/kvm-plan.md` §4's T1 wants "a one-printer canonical state document" and this
  plan gives it a name, a printer and a diff harness (the 64-bit engine differential) before the
  lane starts.
- **the corpus.** A 64-bit host-verified corpus is the only thing that can judge a KVM leg's
  *state translation* on a 64-bit guest: the KVM plan is explicit that the lane "cannot strengthen
  any claim about instruction semantics", and this plan's corpus is where that claim lives.

**Not reused, deliberately:**

- **the counter staircase.** KVM's execution has no per-batch flush points, which is why that lane
  has its own three determinism classes (`results/kvm-plan.md` §6, MEASURED-BOUNDED). The staircase
  is CONTRACT for the emulated backends (§3.3) and **not** available to a KVM backend; that
  asymmetry is a property of the seam's honesty, not a hole in it.
- **the physical-address bound.** KVM does not need §5.1's 32-bit physical model; a KVM leg can host
  a guest with more RAM than `mem8` holds. The seam must therefore **not** put `modes` in charge of
  the memory model — a good reason the capability word is about *modes*, not about features.

**What the KVM lane must not inherit.** This plan's slices build a long-mode machine whose every
claim is either host-verified (the corpus) or guest-exercised (the boot). A KVM leg run against it
is a **throughput** instrument (MEASURED: 8418.8 M guest instructions/s,
`results/kvm-plan.md` §0/§2) and never a semantic authority — so the moment a KVM backend disagrees
with this lane's emulated machine on a 64-bit guest, the corpus and the spec-derived selftests
decide, not the hardware. That rule is written here because the ordering makes it tempting to invert.

---

## Appendix A — the mode predicate and the transition rules this plan depends on

`cpu_mode_of(cpu)` (SDM Vol 3A §9.8, §10.2; the port's fields in brackets):

| mode | condition |
|---|---|
| real | `!cr0.PE` |
| 16-bit protected | `cr0.PE && !efer.LMA && !is_32` |
| 32-bit protected | `cr0.PE && !efer.LMA && is_32` |
| long (compat) | `efer.LMA && !CS.L` |
| long (64-bit) | `efer.LMA && CS.L` |

`EFER.LMA` is set on `CR0.PG`'s 0→1 edge **iff** `EFER.LME && CR4.PAE`, and cleared when `CR0.PG`
goes to 0 (`EFER.LME` itself is **not** cleared). `MOV CR0` is also where the `CR0.PG` transition's
`#GP` rules live (the `CR0.PE`/`PG`/`CD`/`NW` combinations the port already handles,
`src/cpu/paging.c:445-478`), and `wrmsr(EFER)`'s change already flushes the TLB
(`src/cpu/instructions_0f.c:6293-6298`) — the hook S2 and S11 both ride.

**Three rules this plan relies on being true of the reference, hence of the port:**

1. every instruction that can change any of `cr0.PE/PG`, `EFER.LMA`, or `CS.L` is a
   `block_boundary` entry in `v86/gen/x86_table.js` (192 such entries overall, **MEASURED**), so a
   batch never spans a mode change (§3.3's per-cycle check). The ones that matter were checked
   individually (**MEASURED**, all `block_boundary: 1, no_block_boundary_in_interpreted: 0`):
   `0F 20`/`0F 22` (`MOV CR*`, the `PG`/`PE` edge), `0F 30` (`WRMSR`, `EFER.LME`), `0F 05`
   (`SYSCALL`), `0F 34`/`0F 35` (`SYSENTER`/`SYSEXIT`), and the far transfers `9A`/`EA`/`CA`/`CB`/`CF`.
   `rel8`/`rel32` near jumps are `block_boundary` **with** `no_block_boundary_in_interpreted`, which
   is irrelevant here (they do not change the mode);
2. `CS.L` is a descriptor bit (bit 5 of the type-specific dword that also carries `D`/`B` at bit 6),
   so `switch_seg`/`far_jump` (`src/cpu/exceptions.c:932-1240`) is where it enters the model — and
   it is the one bit that makes a `CS` descriptor mean 64-bit vs 32-bit, so the port's existing
   descriptor decode gains a case rather than a rewrite;
3. in long mode `DS/ES/SS` bases are 0 and limits are ignored, so **no segment machinery changes**
   except `FS`/`GS`, whose bases come from the MSRs and whose `segment_offsets[]` entries widen
   (§4).

## Appendix B — one corpus test, end to end (the shape every family copies)

```
; tests/nasm64/alu-add64.asm        (generated by gen/nasm64_gen.py from a triple)
.stub      @0x80000   ; 32-bit: build PML4/PDPT/PD/PT identity-mapping the first 4 MiB,
                      ; CR4.PAE; EFER.LME; CR0.PG; ljmp 0x08:body (.code64)
.body      @0x90000   ; .code64
                      mov  rsp, 0x102000            ; the compared window's top
                      xor  edx, edx                 ; RDX = 0 and CF = 0
                      mov  rax, 0x00000000FFFFFFFF  ; MOV r64, imm64 (an 8-BYTE immediate)
                      add  rax, 1                   ; 64-BIT add: RAX = 0x100000000, CF = 0
                      adc  rdx, 0                   ; RDX = 0  -- and 1 if this ran as a 32-bit add
                      mov  rcx, 0xFFFFFFFFFFFFFFFF
                      add  rcx, 1                   ; 64-bit wrap: RCX = 0, CF = 1
                      adc  rdx, 0                   ; RDX = 1
                      mov  [0x100000], rdx          ; lands in the compared memory window
                      hlt
.loop                      ; the fixture's int3 goes here; both legs stop at it
```

**Why this test discriminates, which is the property §7 R4 demands:** `add rax, 1` on
`0xFFFFFFFF` produces **no carry** at 64 bits and **a carry** at 32 bits, so a body that ran the
32-bit form leaves `[0x100000] == 1` where the host says `0`; and `mov rax, imm64` cannot be encoded
at all without an 8-byte immediate, so a decoder that read 4 bytes would desynchronise the whole rest
of the body and the compared `rip` would move. A test whose expected value is the same under both
widths would pass on a 32-bit engine and is not admissible in this family.

- **native leg**: `mkfixture64` mmaps the flat image at `0x80000`, sets `rsp`, jumps to the body —
  `RDX = 1` and `[0x100000] = 1` are the **host's** answer;
- **emulated leg**: the port enters the **stub** at the address-table entry
  (`src/main.c:284-305`), reaches long mode itself, and `--fixture64` prints the v2 array;
- **the two red states available before S6**: run it against the S5 binary under `os_index == 2` —
  the encodings with no overlay entry **stop by name**; and perturb `rdx` in the fixture — the
  comparison must fail (§2.1.7).
