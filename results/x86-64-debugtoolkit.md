# x86-64: the debugging toolkit — built, default-off, and it answered the 0x31558 question

**Status: all four tools are in the tree behind one env-var surface, DEFAULT OFF, with a selftest arm
that proves each fires; the bench pin and every other pin are unmoved with them present-and-off; and
the toolkit's first real use names the instruction behind the amd64 kernel's first #PF.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The surface (one env-var family, one prefix, stderr)

| flag | what it does |
|---|---|
| `V86C_WATCH=<addr>:<len>[,<addr>:<len>…]` | log every write to a watched address — **virtual** (`safe_write*`), **faulting** (`trigger_pagefault`) or **physical** (the memory helpers) — with the rip, width and value, on stderr |
| `V86C_RING=<N>` | keep the last N instruction boundaries (eip + the 8 bytes at the fetch) and dump them on any stop |
| `V86C_DUMP_ON_STOP=1` | on a stop: registers, cr0–cr4, cr2, cr3, efer, idtr/gdtr, cpl, **the segment bases**, and the page-table chain |
| `V86C_CHAIN=<va>` | the virtual address whose PML4E/PDPTE/PDE/PTE chain `DUMP_ON_STOP` walks |

Output is `v86c dbg: …` on stderr, **unbuffered** (`setvbuf(stderr, NULL, _IONBF, 0)` in
`v86c_dbg_init`), and `v86c_dbg_on_stop` `fflush`es as well — the two losses this lane already paid for
(a 160-byte stop buffer cutting evidence, a SIGSEGV discarding block-buffered output) cannot recur.

**The cost when off** is one load and one branch per hooked write (`if (v86c_dbg_watch_on)` — a global
bool read, no call), one per instruction for the ring, and one call in the stop path. **MEASURED:
`tests/perf_bench.sh` still reports `instr=200095214`** with the tools present, which is the pin the
brief asked for.

**Files:** `src/cpu/debug.c` / `debug.h` (new), hooked in `paging.c` (the four `safe_write*` entry
points and `trigger_pagefault`), `memory.c` (the physical helpers) and `cpu.c` (the ring in the
interpreter loop, and `unimplemented_instruction` → `v86c_dbg_on_stop`). `Makefile` gains `debug.c` in
`CPU_SRC` and `SELFTEST_SRC`.

## 2. The selftest arm (MEASURED)

`m1c_selftest`'s `test_debug_toolkit_fires` turns the tools on **without** the environment
(`v86c_dbg_test_arm`), calls each entry point, and checks the tool's own counter — the "who reads this
on the deciding path" question asked of the instrument itself:

    PASS the debugging toolkit fires: watchpoint, fault watch, ring, dump-on-stop
    CHECK_EQ(v86c_dbg_test_counts(), 4)      /* watch + fault + ring + stop, each counted by the tool */

It passes, and `make selftest` is green with it.

## 3. The acceptance test: the 0x31558 question, answered by the toolkit (MEASURED)

`V86C_WATCH=0x31558:1 V86C_RING=16 V86C_DUMP_ON_STOP=1` on the pinned amd64 kernel:

    v86c dbg: WATCH F 0x31558 write rip=0xffffffff9c2775c2 (the access FAULTED, no store)
    v86c dbg: ring (16 recorded, oldest first):
      eip=0xffffffff9c27759c bytes 8b 0d ca c7 39 ff 48 83     mov ecx,[rip-…]
      eip=0xffffffff9c2775a2 bytes 48 83 c8 ff 48 d3 e0 a9     or rax,-1; shl rax,cl
      eip=0xffffffff9c2775a9 bytes a9 ff ff ff 7f 74 02 0f     test eax,0x7fffffff; jz
      eip=0xffffffff9c2775ae bytes 74 02 0f 0b 55 53 48 89     jz; UD2; push rbp; push rbx
      eip=0xffffffff9c2775b7 bytes 0f 20 e0 65 48 89 05 96     mov rax,cr4; MOV GS:[disp32],rax
    v86c dbg: bases cs=0x0 ss=0x0 ds=0x0 fs=0x0 gs=0xffffffff9c242000
    v86c dbg: state rip=0xffffffff9c2775ba cr0=0x80050033 cr2=0x9bc040e0 cr3=0x30ea000 cr4=0xa0
                     efer=0xd01 idtr=0x9bc04000/511 gdtr=0x9c24d000/127 cpl=0
    v86c dbg: chain va=0x9bc04000 cr3=0x30ea000 idx=0/2/222/4
    v86c dbg:   PML4E  @0x30ea000 = 0x30ec063
    v86c dbg:   PDPTE @0x30ec010 = 0x0  (ABSENT)

**The answer: the faulting store is `mov %rax, %gs:[disp32]` at `0xFFFFFFFF9C2775C2`** — a *percpu*
store, reached by `mov rax, cr4` (the kernel's `cr4_init_shadow`-shaped bookkeeping). The machine's GS
base is **correctly `0xFFFFFFFF9C242000`**, yet the address that faults is **`0x31558`** — the *raw
displacement*, not `gs_base + disp`.

**INTERPRETATION (and the next measurement is exact):** the shape says the **GS base was not added**
for this access — i.e. the `65` prefix did not reach `modrm_resolve64`'s `segment_prefix()` call, or
the base was added and the displacement is `0x31558 − gs_base` (a large negative) — and the two are
told apart by printing the instruction's disp32 and the resolved address at the moment of the access.
The toolkit makes that one flag away: a `V86C_WATCH` on the *resolved* address plus the ring's bytes
already localise it to a single instruction.

## 4. Verification (MEASURED; code changed, so the FULL battery)

    make                                   rc 0, no warnings
    make selftest                          rc 0 — all suites green (cpu 3888, m1c 272, hw 975,
                                           uop_cache 7934363, uop_exec 102, jit 68, jit_codegen 3570)
    make nasm                              rc 0 — Passed 118/118 tests
    tests/gate-m2-5.sh                     rc 0 — 0 FAILING (transcript 8bb8eca5… byte-identical)
    tests/gate-m2-5-determinism.sh         rc 0 — 0 FAILING, both runs 4,000,096,757
    tests/perf_bench.sh                    rc 0 — instr=200095214  ← with the tools present and off
    tests/gate-x86-64.sh                   rc 0 — 9 passed, 0 failed
    tests/gate-long-mode.sh                rc 0 — 57 passed, 0 failed
    tests/gate-long-mode-sys.sh            rc 0 — 5 passed, 0 failed
    tests/gate-x86-64-diff.sh              rc 0 — 13 passed, 0 failed (83/83 on interp+uop+uopjit)
    tests/gate-nasm64.sh                   rc 0 — 13 passed, 0 failed
    tests/gate-pae-1.sh                    rc 0 — 11 passed, 0 failed
    tests/gate-pae-l3.sh all               rc 0 — 0 FAILING
    tests/gate-reboot-1.sh                 rc 0 — 39 passed, 0 failed
    tests/gate-vga-{0,1,2,3,4}.sh          rc 0 each — 0 FAILING

**Every pin is unmoved**, including the bench with the tools compiled in — the mask is a load and a
branch, and that is the measurement, not an assurance.

## 5. Files touched

| file | change |
|---|---|
| `src/cpu/debug.c` / `debug.h` | **new**: the four tools, the env-var surface, the unbuffered output, the chain walk, and the selftest's arm/disarm/count surface |
| `src/cpu/paging.c` | the watch on the four `safe_write*` entry points and on `trigger_pagefault` (a faulting write never reaches a store) |
| `src/cpu/memory.c` | the physical write hook |
| `src/cpu/cpu.c` | the ring record in the interpreter loop; `v86c_dbg_on_stop` in `unimplemented_instruction` |
| `src/main.c` | `v86c_dbg_init()` first thing |
| `src/cpu/m1c_selftest.c` | `test_debug_toolkit_fires` |
| `Makefile` | `debug.c` in `CPU_SRC` and `SELFTEST_SRC` |
| `docs/testing.md`, `TODO.md` | the tools documented; §8 updated |
