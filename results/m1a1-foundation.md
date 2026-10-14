# M1a-1 — the CPU foundation (state, lazy flags, memory router, paging/TLB)

**Unit:** `HANDOFF=v86c`, unit M1a-1 (coder). **Deliverable:** the foundation under
`src/cpu/` — `cpu_state_t` + reset + register accessors, the lazy-flag machinery, the
memory router with a device hook table, and paging/TLB/safe_*. **Not** in this unit:
instructions, opcode dispatch, the emitter, devices, `tests/`, `oracle/`.

Reference: `./v86` (READ-ONLY), plans `results/cpu-port-plan.md` §2/§3/§4 and §7 steps 4-6.
Raw evidence for every gate: **`results/m1a1-gates.txt`** (245 lines, reproduced below in
the places that matter).

**Bottom line.** All four gates pass: `make selftest` → RC=0 (443 checks, 0 failures),
12 mutation probes each make it FAIL (so every test can fail), `make` → RC=0 with the new
translation units compiling clean under `-Wall -Wextra -Werror`, `make oracle` → RC=0
(`tests/` and `oracle/` untouched, `tests/test.img` sha256 unchanged). Beyond the brief's
gates, the 24 lazy-flag cases were cross-checked against the **npm v86 reference itself**
(it is drivable read-only from `oracle/node_modules`): port = hand table = reference on
all 24.

---

## 1. What changed (every file)

| File | Lines | What it is |
| --- | --- | --- |
| `src/cpu/cpu_state.h` | 435 | the state struct in `global_pointers.rs` order, the architectural constants of `cpu.rs:80-300`, `union reg128` (aligned 16), `f80_t`, the register/cached-state accessors, **the page-fault plumbing contract**, the reset-value constants |
| `src/cpu/cpu_state.c` | 232 | `cpu` singleton, `read/write_reg8/16/32` + mmx/xmm accessors, `update_state_flags`/`has_flat_segmentation`/`getiopl`/`vm86_mode`, `reset_cpu`, `cpu_state_init` |
| `src/cpu/flags.h` / `flags.c` | 69 / 229 | the 6 lazy-flag getters (`misc_instr.rs:8-66`), the add/adc/sub/sbb/inc/dec/neg flag cores (`arith.rs:12-103`) as `flags_*`, `opsize_to_mask`, `get_eflags` (`cpu.rs:1911-1918`), `update_eflags` (`cpu.rs:4478-4520`) |
| `src/cpu/memory.h` / `memory.c` | 114 / 439 | `mem8` + `allocate_memory` (+ 16-byte slack, `mem8_alloc_size()`), `in_mapped_range`, the full read/write router (`memory.rs:103-352`), `mmap_write*`, the **device hook table**, VGA/APIC/IOAPIC stubs |
| `src/cpu/paging.h` / `paging.c` | 107 / 750 | `tlb_data` (4 MiB, outside the struct), the TLB bookkeeping + `full_clear_tlb`/`clear_tlb`/`invlpg`, `translate_address` + all wrappers, `do_page_walk` (PSE, PAE trapped), `trigger_pagefault`, `set_cr0`/`set_cr3`/`load_pdpte`, `readable/writable_or_pagefault`, `virt_boundary_*`, `safe_read*`/`safe_write*` |
| `src/cpu/cpu_selftest.c` | 842 | the unit tests (3 gates) + the machine-readable `CASE` lines the reference cross-check parses |
| `Makefile` | +26 | `make selftest`; the foundation's translation units as `build/cpu/*.o` under `-Werror`; `all` also builds those objects |
| `results/m1a1-gates.txt` | 245 | raw gate evidence |

`make` does **not** link the foundation into the APE: `src/main.c` is still the M0
placeholder and the core joins the run loop in M1c. `make` therefore *compiles* the
translation units (which is the brief's stated G3: "the new .c files must compile clean
under -Wall -Wextra"), and `make selftest` links and runs them. Nothing in `tests/` or
`oracle/` was created or modified (`tests/test.img` sha256 is still
`1a597ac1…e737a`, the M0 record).

## 2. The page-fault plumbing — the interface every later unit calls

This is the piece the brief says to fix *now*, in a header. It is `src/cpu/cpu_state.h`
(the "PAGE-FAULT PLUMBING" block) and the shape is:

1. a faultable helper **sets `cpu->pagefault`** and returns a **sentinel** (0, or a
   zeroed `reg128`/`0.0f`; the value is never used, exactly as in Rust's `Err` arm);
2. the caller checks **`if (cpu->pagefault) return;` immediately after the call**,
   before using the result — which is precisely what the emitter will print
   (plan §1: `v = safe_read32s(addr); if (cpu->pagefault) return;`);
3. the instruction's **partial effects are kept** (plan §8/R3): a fault aborts the
   instruction, it never undoes it;
4. **`cpu->pagefault` is SET-ONLY inside a helper.** The dispatch loop clears it once
   per instruction, before dispatch, and that is the only clear — a helper must never
   clear it, or an inner fault (a fault while reading the IDT inside
   `call_interrupt_vector`, `cpu.rs:822-830`) would be masked by the inner return path.

`RETURN_ON_PAGEFAULT(expr)` is the statement-form macro for hand-written call sites
(upstream's `return_on_pagefault!`). MEASURED that this contract is load-bearing, not
decoration: the selftest's first draft called two helpers in sequence without clearing
the flag between them (i.e. it simulated two instructions without the dispatcher), and a
**good** address then returned the sentinel — the flag gates the value. The selftest now
clears it per simulated instruction, and that is asserted
(`cpu_selftest.c`: "the fault flag is SET-ONLY and gates the returned value").

`call_interrupt_vector` is **declared** in `paging.h` (M1a's `trigger_pagefault` is its
first caller) but **defined by M1c's `exceptions.c`**. M1a's selftest supplies a
recording definition — so anyone building a binary from M1a's sources without M1c needs
that stub, and M1c should replace it, not shadow it.

## 3. The F80 representation (the plan's open question, decided)

READ `v86/src/rust/softfloat.rs:1-45`: the arithmetic core is **Berkeley SoftFloat**,
called as `extF80M_*` over an `F80`, and `softfloat.rs:47-56` declares
`#[repr(C)] struct F80 { mantissa: u64, sign_exponent: u16 }` — i.e. Berkeley's
`extFloat80M { uint64_t signif; uint16_t signExp; }`, with `size_of::<F80>() == 16`
(8 + 2 + 6 padding), which is why the state block's `fpu_st` slots are 16 bytes apart
(`fpu_st`@1152, `efer`@1280 = 1152 + 8·16).

**Choice:** `typedef struct f80 { uint64_t mantissa; uint16_t sign_exponent; } f80_t;` —
the same layout, with `_Static_assert(sizeof(f80_t) == 16)` and
`_Static_assert(offsetof(f80_t, sign_exponent) == 8)`, so M1b's `softfloat.c` can pass
`&cpu->fpu_st[i]` straight to the `extF80M_*` routines with no marshalling. Rejected:
the host `long double` (same bytes on x86, silently rounds to double on aarch64 — an APE
target) and a 128-bit struct (the JS view reads 4×i32 per slot but only the low 10 bytes
are ever meaningful).

## 4. Divergences from the upstream Rust (the brief asks for every one)

| # | Where | Divergence | Why |
| --- | --- | --- | --- |
| D1 | struct | the absolute offsets are gone; members are **named**, in `global_pointers.rs` order | plan §2 DECISION 2: the C port has no JS peer to share a flat memory with |
| D2 | `paging.c` | the TLB entry stores `high ^ (page << 12) \| bits` where `high` is the **guest physical** page, and `translate_address` returns a **guest physical address** | upstream (`cpu.rs:2323-2325`) bakes in **mem8's host address** and subtracts it back — a 32-bit-wasm trick. A host pointer does not fit in the 32-bit entry on a 64-bit APE target, so "keep the same trick" is not implementable literally; the information content is identical. Taking the plan's phrase literally would be the bug here |
| D3 | all of `paging.c` | **JIT-only plumbing dropped**: the `jit: bool` parameter of `translate_address`/`do_page_walk`/`trigger_pagefault` (and its `jit_exit_reason` branch, `cpu.rs:2503-2509`), `tlb_code`/`TLB_HAS_CODE`/`clear_tlb_code`/`update_tlb_code`, `jit_dirty_page`/`jit_dirty_cache_small`, and the `can_skip_dirty_page` bool of `translate_address_write_and_can_skip_dirty` | interpreter-only port (plan §3); the dropped bool only ever gated `jit_dirty_page`, so the three write paths collapse to two |
| D4 | `paging.c` `trigger_pagefault` | the log lines (`config::LOG_PAGE_FAULTS`, `dbg_trace`) and the DEBUG `js::cpu_exception_hook(CPU_EXCEPTION_PF)` are dropped | no log/hook channel exists in M1a; the interrupt delivery is unchanged |
| D5 | `paging.c` `load_pdpte` + `do_page_walk`'s PAE arm | **trap** (`abort` with a named message) instead of walking PAE | plan §3: PAE is deferred and must "never silently misinterpret a 64-bit PTE". I put the trap in `load_pdpte` too, because it is PAE-only and would otherwise fill `reg_pdpte` with tables nothing can walk — so `reg_pdpte` exists in the state and stays 0 |
| D6 | `memory.c` `read64s` (device path) | the upstream **sign-extension quirk** is reproduced deliberately: `(int64)read32s(addr) \| ((int64)read32s(addr+4) << 32)` loses the high word when the low word's bit 31 is set | it is upstream's behaviour on the non-RAM path only (the RAM path is a real 64-bit load); "fixing" it would be a silent divergence, and it costs nothing to match |
| D7 | `memory.c` | `mem8` is allocated `memory_size` rounded to 4 KiB **+ 16 bytes** and zeroed (`calloc`) | plan §3/R7: `read32_no_mmap_check` loads 4 bytes unconditionally. Zeroed because wasm linear memory starts zeroed, so the reference reads 0 before anything writes there. `mem8_alloc_size()` exposes the bound so the invariant is testable |
| D8 | `memory.c` | the `apic::write32`/`ioapic::write32` call sites drop the `handle_irqs()` that follows them; VGA/APIC/IOAPIC and the `mmap_*` imports become stubs + the hook table | `handle_irqs` is M1c's; with a stubbed APIC no IRQ can be latched, so the call is a no-op — recorded rather than silently omitted |
| D9 | `cpu_state.c` `reset_cpu` | `set_tsc(0, 0)` becomes `current_tsc = 0`; `jit::jit_clear_cache_js()` dropped; the `switch_cs_real_mode(0xF000)` and `switch_seg(SS, 0x30)` calls are **inlined as their real-mode arms** | TSC needs the host clock (M1c's run loop); `switch_seg`/`switch_cs_real_mode` live in M1c's `exceptions.c`, and at reset `protected_mode` is false so only the real-mode arm is reachable (asserted by upstream's own `dbg_assert`) |
| D10 | `flags.c` | the arith cores are named `flags_add`/`flags_sub`/… (upstream's private `add`/`sub`/…) and `opsize_to_mask` is written in **unsigned** arithmetic | C has one namespace; and `(2 << 31) - 1` is UB for signed `int` in C (upstream relies on Rust's shift-out-then-wrap to make the 32-bit mask `-1`) |
| D11 | `cpu_state.h` | `dbg_assert!` → `DBG_ASSERT` (= `assert` unless `NDEBUG`) | upstream's is `debug_assert!`, compiled out of the shipped wasm (handoff-v86c-result, "NOTE FOR LATER UNITS"); the selftest runs with them **on**, which is how the flag invariants are pinned |
| D12 | `flags.c` `update_eflags` | the `dbg_log!("Not supported: trap flag")` is dropped, the flag clear is kept | no log channel yet; the observable behaviour is identical |

Everything else is a verbatim port, including the awkward parts (the `0x9669 << 2 >> …`
parity table, the XOR-pattern CF/AF/OF formulas, `inc`/`dec` materializing CF before
marking it clean, the `valid_tlb_entries` duplicate-entry TODO, `invlpg` *not* removing
the page from `valid_tlb_entries`, and the A/D-bit writes done as **byte** stores
because `memory::write8` is what upstream calls).

## 5. Findings about the plan and about upstream

The plan held up where it mattered — its `file:line` citations all resolved to the code
it claimed — but three items are worth propagating to the later units:

- **P-NOT-1 — the file split for the flags.** Plan §7 puts the flag *getters* in step 4
  (`cpu_state.c`) and `flags.c` in step 7; my brief puts the getters **and** the arith
  setters in `flags.c`. That is what is built: `flags.c` owns `getcf/getpf/getaf/getzf/
  getsf/getof`, `get_eflags`, `update_eflags`, and the add/adc/sub/sbb/inc/dec/neg cores
  as `flags_add/…`. **`arith.c` (M1a-2) must not redefine them** — it owns the
  mul/imul/div/idiv/shift/rotate families and the per-size public wrappers
  (`add8/16/32`, `sub8/16/32`, …, `cmp8/16/32`), each of which is a one-liner calling the
  corresponding `flags_*`.
- **P-NOT-2 — `get_reg16_index` is a trap.** Upstream's `get_reg16_index(index) = index
  << 1` is an index into a `*mut u16`, so the **element** index in a `uint16_t` view of
  `reg32` is `2*index` (`reg32[1]`'s low half is `u16[2]`, i.e. CX). I initially dropped
  the shift and **DX silently aliased to CX** — caught by the selftest, and exactly the
  silent-wrong class. Any later unit indexing `reg16`/`seg16` views must apply it.
- **P-NOT-3 — `read_imm16`/`read_imm32s` need `virt_boundary_read16/32s`.** They are
  *not* on the `safe_read*` path (`safe_read32s` splits with two `safe_read16`s);
  `virt_boundary_read16/32s` (`cpu.rs:3477-3504`) exist for the **fetch** path
  (`cpu.rs:2584-2640`). I ported them (the write halves are needed by `safe_write*`), so
  M1a-2 should use them rather than writing its own.

Two upstream oddities found while porting, **recorded and not "fixed"**:

- **U-1 — `set_cr3`'s mask and its own assert disagree.** Non-PAE: `cr3 &= !0b111111100111`
  clears bits 0,1,2,5-10 but *keeps* bits 3,4,11, and the next line is
  `dbg_assert!(cr3 & 0xFFF == 0)`. A `cr3` with bit 3, 4 or 11 set therefore trips the
  assert *after* the mask was supposed to have sanitized it. Harmless for a guest that
  writes an aligned CR3 (my test uses `0x2007 -> 0x2000`), but it means the mask is not
  a sanitizer. `DBG_ASSERT` is on in the selftest, so this is only reachable with an
  unaligned guest CR3.
- **U-2 — the plan's init-value line for `mxcsr` is imprecise.** Plan §2 says `mxcsr`
  is "initialized via `ldmxcsr` path"; in the code `reset_cpu` writes `0x1F80` directly
  (`cpu.rs:4709`), which is what the port does. The *value* matches; the derivation in
  the plan does not.

## 6. Gates

Full raw output in `results/m1a1-gates.txt` (245 lines: G1, all 12 mutations, G3/G4, the cross-check, and a from-scratch re-verification appendix).

**G1 — `make selftest` → RC=0**, 443 checks, 0 failures:

```
PASS reset/init values, state_flags, register accessors
PASS lazy flags: 24 (op,a,b,cf_in) cases + get_eflags/update_eflags
PASS memory: router, slack at memory_size, device hook table, mmio windows
PASS paging: 4 KiB translation, A/D bits, TLB read-only entry
PASS paging: faults (absent, read-only/WP, user vs supervisor) + cr2/eip/delivery/error codes
PASS paging: 4 MiB PSE page (translation, A/D, page boundary)
PASS paging: page-crossing reads/writes (16/32/64/128-bit) + straddle fault
PASS TLB: insert, invlpg, clear_tlb/global, set_cr3/set_cr0 flushes
cpu_selftest: 443 checks, 0 failures
```

The gates cover exactly what the brief asks: the §2 init values
(`fpu_control_word 0x37F`, `flags = FLAGS_DEFAULT = 2`, `cr0 = 0x60000010`,
`dreg[6] = 0xFFFF0FF0`, `eip = 0xFFFF0`, `sreg[CS] = 0xF000`,
`segment_offsets[CS] = 0xF0000`, `SS = 0x30`, `ESP = 0x100`, `mxcsr = 0x1F80`,
`pat = 0x0007040600070406`, `state_flags = 0`); a 24-case `(op,a,b,size,cf_in) ->
{CF,PF,AF,ZF,SF,OF}` table over add/adc/sub/cmp/sbb/inc/dec/neg (including `inc`/`dec`
CF preservation and `adc`/`sbb` carry-in), `get_eflags`/`update_eflags` (dont-update,
cpl gating); and hand-built 32-bit **and** 4 MiB PSE page tables driven through
`translate_address`/`safe_read`/`safe_write` with present/absent/read-only/user-vs-
supervisor/WP/page-crossing, asserting values, the fault flag, **cr2**, **eip reset to
`previous_ip`**, the delivered interrupt + **error code** (0/3/5/7), the A/D-bit side
effects, the `TLB_READONLY` read-caches-write-misses behaviour, `invlpg`, the
global-page survival of `clear_tlb`, and the `set_cr3`/`set_cr0` flushes.

**G2 — every test shown able to FAIL.** 12 mutations, each applied alone, `make
selftest` run, source restored, restore re-verified green. All 12 produce failures:

| Mutation | Result |
| --- | --- |
| M1 `fpu_control_word 0x37F -> 0x37E` | RC=2, 1 failure |
| M2 reset `FLAGS_DEFAULT -> 0` | RC=2, 4 failures |
| M3 `getcf`'s `sub_mask -> 0` | RC=2, 8 failures |
| M4 `dec` no longer preserves CF | RC=2, 2 failures |
| M5 `getzf` drops the op-size shift | RC=2, 9 failures |
| M6 the absent-PTE check is dropped | RC=2, 15 failures |
| M7 `cr2` gets the wrong address | RC=2, 5 failures |
| M8 the PTE dirty bit is never set | RC=2, 1 failure |
| M9 `invlpg` leaves a bogus TLB entry | RC=2, 1 failure |
| M10 the device hook read is bypassed | RC=2, 2 failures |
| M11 the +16-byte slack of `mem8` is dropped | RC=2, 1 failure |
| M12 `in_mapped_range` loses the `memory_size` bound | RC=2, 4 failures, then the run aborts (out-of-range writes became RAM writes) — no summary line, which is itself a failure signal |
| (restored) | RC=0, 443 checks, 0 failures |

Two of the twelve needed a second attempt, and both were **my test's** fault, not the
code's: M9's first form (`tlb_data[page] = 0;` deleted) failed to *compile* under
`-Werror=unused-variable`, and M11's first form went undetected because a 1-byte slack
was still large enough for the test to write into. The fix was to make the invariant
checkable (`mem8_alloc_size()`), which is what a test that can fail requires.

**G3 — `make` → RC=0**, APE built by cosmocc, verified **from a clean `build/`**. All
four new translation units compile with `-std=gnu11 -O2 -Wall -Wextra -Werror` and warn
on nothing:

```
cc -std=gnu11 -O2 -Wall -Wextra -Werror -c -o build/cpu/cpu_state.o src/cpu/cpu_state.c
cc -std=gnu11 -O2 -Wall -Wextra -Werror -c -o build/cpu/flags.o     src/cpu/flags.c
cc -std=gnu11 -O2 -Wall -Wextra -Werror -c -o build/cpu/memory.o    src/cpu/memory.c
cc -std=gnu11 -O2 -Wall -Wextra -Werror -c -o build/cpu/paging.o    src/cpu/paging.c
cc -std=gnu11 -O2 -Wall -Wextra -o build/v86c src/main.c            (RC=0)
```
`od -An -c -N8 build/v86c` → `M Z q F p D = '` (APE signature), `./build/v86c` prints the
M0 banner and exits 0. `make determinism` (the M0 gate) is still RC=0.

**G4 — `make oracle` → RC=0**, `tests/` and `oracle/` untouched:

```
check_transcript: transcript matches tests/model.py (8 GPRs, EFLAGS, EIP, 561 instructions, 4096-byte window sha256)
1a597ac159554271a380493a19aa52ca0b65481cdb6c979c407684c9c96e737a  tests/test.img
```

## 7. Beyond the gates: the flag machinery cross-checked against the reference

The unit-level flag gate is a table I derived from the SDM rules — and the port is also
my own code, so a shared misreading could pass both. To close that hole I drove the same
24 cases through the **reference implementation** (npm `v86@0.5.471+g94faf3d`, read-only
from `oracle/node_modules`, using `oracle/run.mjs`'s recipe): a generated guest image
(24 × `mov`/`stc|clc`/`<op>`/`pushf`/`pop [slot]`, multiboot header, linked at
0x100000 like the M0 image) runs under the reference, and each case's materialized
EFLAGS is read back out of guest memory.

```
cases compared: 24
port-vs-reference mismatches: 0
reference instruction count: 147
VERDICT: all 24 flag cases agree (port == table == npm v86 reference)
```

The instrument is preserved as `results/flagref-gen.py`, `results/flagref-run.mjs`
and `results/flagref-cmp.py` (it ran with `cwd=/tmp/flagref`; `gen.py` writes
`flagcases.s`, assembles with `as --32`, links with the M0 `tests/test.ld` and flattens
with `objcopy`; `run.mjs` drives the pinned package read-only).

The first run of that check **disagreed** on exactly the three `adc`/`sbb` cases with a
carry-in — and the disagreement was in my *generator*: it skipped the `mov` for a
zero operand, so the register kept a stale value, and the reference's numbers matched
that misread operand exactly. That is a useful independent signal in itself (the
reference is computing the real flags, and so is the port), and it is recorded here
rather than quietly fixed.

**What this cross-check does NOT cover:** paging/faults against the reference (that needs
a guest IDT + PF handler — the plan's corpus oracle, P0/M1a-2, is the instrument for it),
and the TLB/`safe_*` layer, which the unit tests cover against upstream's code semantics
only.

## 8. Left dangling on purpose (for the next units)

- `call_interrupt_vector` — declared, defined by M1c (`exceptions.c`); M1a's selftest
  provides a recording stub.
- `is_osize_32`/`is_asize_32`/the `prefixes` decode helpers — not in this unit (they
  belong with `modrm.c`/the emitter's prefix handling); the `prefixes` **byte** is in the
  state and reset to 0.
- `read_imm8/8s/16/32s/moffs` (the fetch path), `modrm_resolve`, `trigger_ud/gp/de/nm`,
  `task_switch_test*`, the FPU and SSE — all later units.
- `memory_devices` is a plain global hook table (`memory_set_device_hooks(NULL)` restores
  the no-device default); the M0 oracle's configuration is exactly that default.
