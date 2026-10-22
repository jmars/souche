# x86-64: the CR4 write's PDPTE reload — the port reloaded too eagerly, and the kernel now runs on

**Status: FIXED, with an arm. The `mov cr4` #GP is gone, the amd64 kernel switches page tables and
runs 110 instructions further, and the stop it reaches now is a pre-existing named gap of this port
(`RETF` target ≥ 4 GiB).**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. What the code does, and which of the three candidates it is (READ + MEASURED)

READ (`src/cpu/instructions_0f.c`, `case 4:` of MOV CRn): after the reserved-bit mask the port runs

    if ((data & CR4_PAE) != 0 &&
        ((old_cr4 ^ data) & (CR4_PAE | CR4_PGE | CR4_PSE | CR4_SMEP)) != 0 &&
        (cpu->cr[0] & CR0_PG) != 0 && !load_pdpte(cpu->cr[3])) { return; }

and `load_pdpte` → `read_pdpt` (paging.c:994) judges **four 8-byte entries at CR3** by the **32-bit
PAE PDPTE rules**: `(entry & 0xFFFFFFFF00000000) != 0` → `#GP`, and when present
`(entry & 0x1E6) != 0` → `#GP`.

In long mode CR3 points at a **PML4**, and a PML4E legitimately carries **PWT (bit 3), PS (bit 7)**
and **NX (bit 63)** — all three rejected by those rules.

**So the answer is (ii) *and* (i), and the measurement separates them:**

* **(ii) the reload fires when hardware would not.** The SDM reloads the PDPTEs when **CR4.PAE is
  set (0→1)**; changing PGE, PSE or SMEP only flushes the TLB. The port reloaded on any of the four.
  MEASURED: the kernel's write is `mov cr4, rcx` with `rcx = 0xA0` (PAE|PGE) against `old = 0x20` —
  `old ^ new = 0x80`, a **PGE-only** change, no PAE transition at all (results/x86-64-jz.md).
* **(i) the rules applied are the wrong ones for the entry it reads.** Even where a reload is
  correct, in long mode the entry is a **PML4E**, so the 32-bit PDPTE mask is too strict.

Candidate (iii) — the guest's table being bad — is **refuted**: the kernel's PML4E is the table it
built for 4-level paging, and the port was reading it as a PAE PDPT. The port's own S2 note already
names this shape ("the same PML4-as-PDPT mistake") and *refuses* the two transitions that reach it;
this is the third site, and it was reached.

## 2. The fix

    if ((data & CR4_PAE) != 0 && (old_cr4 & CR4_PAE) == 0 &&
        (cpu->cr[0] & CR0_PG) != 0 && (cpu->efer & EFER_LMA) == 0 &&
        !load_pdpte(cpu->cr[3])) { return; }

Two clauses, each with its reason: **PAE 0→1 only** (the SDM's rule), and **not in long mode** —
where the 4-level walk (`do_page_walk64`) reads CR3 directly and `reg_pdpte` is not consulted at all,
so the reload is not merely wrong there, it is unnecessary.

## 3. The arm that fails before and passes after (MEASURED)

The corpus cannot carry this arm: `mov cr4` is privileged and the corpus's **native** leg runs the
body at CPL 3, so the host takes `#GP` and the case cannot compare (MEASURED: the first attempt,
`ctrl/cr4-pge-no-pdpte-reload`, reported `Exception actual None / expected GP`). The arm therefore
lives where the machine is at CPL 0 — the selftest — and the existing test that asserted the **old**
behaviour is the better one:

    cpu_selftest.c: "the CR4 write path faults atomically too: CR4 stays uncommitted"

wrote `CR4 = PAE|PSE` over a deliberately bad PDPT and asserted `#GP` + CR4 uncommitted. That
assertion **was the defect**. It is corrected, with its reason in the file, to assert the SDM rule:
**no fault, and the write commits**. MEASURED asymmetry, with the old trigger restored
(`/* MUTATION */`):

    FAIL cpu_selftest.c:1351: exc_vector == (uint64_t)-1   (got 0xd, want 0xffffffffffffffff)
    FAIL cpu_selftest.c:1352: cpu->cr[4] == CR4_PAE | CR4_PSE (got 0x20, want 0x30)

and with the fix: `cpu_selftest: 3888 checks, 0 failures`. **The pin moves 3889 → 3888** and is stated
rather than absorbed: the old test's three assertions became two (the exception check plus the
commit check, replacing exception/code/register).

A second arm was added to `m1c_selftest` (`test_cr4_pge_write_no_pdpte_reload`: a long-mode fixture
whose PML4E carries PWT and NX, then `mov cr4, 0xA0`). It **passes** — but it does **not
discriminate**: it also passes under the mutation, so it is recorded as a *seat* and not as an
instrument. The discriminating arm is the corrected `cpu_selftest` one above.

## 4. The acceptance test: the kernel runs on (MEASURED)

| | before | after |
|---|---|---|
| instructions | 4,475,480,658 | **4,475,480,768** (+110) |
| stop | `mov cr4, rcx` #GP at eip=0x10000A1 | **`long mode: a RETF target at or above 4 GiB`** |
| state | cr3 = 0x4f76000 (the decompressor's tables) | **cr3 = 0x30ea000** (the KERNEL's own tables), eip = 0x88e0015a |
| wall clock | 112 s | 115 s |

**The CR4 write no longer #GPs, and the kernel has switched to its own page tables** — the first time
this lane has seen `cr3` move off the decompressor's value. The new stop is a **pre-existing named
gap of this port**, not a new defect: `instr64_CB`'s `RETF` target bound
(`far_return` takes a 32-bit `eip`), recorded in `results/x86-64-idt64.md` §2.3 and still open.

**Next, named:** widen `far_return` to a 64-bit `eip` (its two `int32_t` consequences — a target
≥ 4 GiB and a CPL-changing RETF — are both named stops today), then re-run. That is a `far_return` /
`iretq` unit, not a one-liner: the CPL-change path reads a 32-bit ESP/SS frame too.

## 5. Verification (MEASURED; code changed, so the FULL battery)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — cpu 3888/0 (was 3889, §3), m1c 272/0, hw 975/0,
                                           uop_cache 7934363/0, uop_exec 102/0, jit 68/0,
                                           jit_codegen 3570/0
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (83/83 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-pae-l3.sh all               rc 0 — 0 FAILING
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed
    tests/gate-vga-{0,1,2,3,4}.sh          rc 0 each — 0 FAILING

**The four M0 pins did not move** (transcript byte-identical, determinism 4,000,096,757, bench
200,095,214, nasm 118/118) — the evidence that the corrected reload rule is inert outside long mode.
The one pin that moved is the selftest count of §3, and it moved because an assertion that encoded
the defect was corrected.

## 6. Files touched

| file | change |
|---|---|
| `src/cpu/instructions_0f.c` | the PDPTE reload: PAE 0→1 only, and not in long mode |
| `src/cpu/cpu_selftest.c` | the old assertion corrected to the SDM rule (the arm) |
| `src/cpu/m1c_selftest.c` | `test_cr4_pge_write_no_pdpte_reload` (a seat; non-discriminating, stated) |
