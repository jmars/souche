# x86-64: the port DOES execute `__startup_64` — and the CR3 it builds is the value the kernel asked for

**Status: MEASUREMENT, plus one toolkit fix. The question `results/x86-64-startup64.md` left open is
answered and it is answered NO-DEFECT on the point that unit suspected: the port reaches
`__startup_64`, runs its fixup loops, runs its tail store, returns what a non-SME machine should
return, and loads the CR3 the kernel's own arithmetic calls for. The earlier "the kernel never
writes `early_top_pgt`" conclusion is now supported by a *reason* rather than by an absence.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. A new instrument was needed first: `V86C_WATCH` cannot see an instruction

`V86C_WATCH` hooks the *store* path. The questions here are "did the port execute this code" and
"what did `%rax` hold when it did", which no store hook can answer. The toolkit gained three tools
(`src/cpu/debug.h`), all default-off, same `v86c dbg:` stderr surface, cost when off = one load and
one branch per instruction (the bench pin is the measurement):

| tool | what it does |
|---|---|
| `V86C_BREAK=<addr>:<len>[,…]` | an **execution** watch: fires *before* the instruction runs, prints `%rax..%r15`, `cr0/3/4`, `efer`, `cpl`, `cs/ss`; `V86C_BREAK_MAX` hits printed per slot, the rest counted and printed at a stop; `V86C_BREAK_RING=1` dumps the ring at each printed hit (the path that led there) |
| `V86C_BREAK_CHAIN=<va>` | the 4-level chain for `<va>` at each printed hit |
| `V86C_PEEK=<phys>:<len>[,…]` | raw **physical** bytes at each printed hit — the ground truth a virtual-address log cannot give |

## 2. The port executes the whole entry, `__startup_64` included (MEASURED)

    V86C_BREAK=0x1000000:0x100,0x1000060:1,0x10000b5:1 V86C_BREAK_RING=1 V86C_RING=48 …

    v86c dbg: BREAK 2 0x1000000 (hit 1) rax=0x1000000 rbx=0x4780000 rsi=0x84790 rbp=0x1000000 rsp=0x4f57000
    v86c dbg: BREAK 0 0x1000060 (hit 1) rax=0x0        rbp=0x1000000 rsp=0x2a03f58
    v86c dbg: BREAK 1 0x10000b5 (hit 1) rax=0x30ea000  rdi=0x30ea000 cr3=0x4f76000 cr4=0xa0

and the ring at the first of those is the straight line

    0x1000000 → leaq _text(%rip),%rdi / wrmsr initial_gs / call startup_64_setup_env
    0x1000043 → mov %rsi,%rdi ; call 0x1001a20 (sme_enable) ; call 0x1000160 (verify_cpu)
    0x100005a → call 0x1000340            ← __startup_64(physaddr, bp)
    0x1000060 → add $…,%rax ; 0x10000b5 mov %rax,%cr3

`0x1000340`'s body is `__startup_64` READ from the decompressed image (`objdump` of the ELF at
`vaddr 0x1000340`): it takes `physaddr` in `%rdi` and `bp` in `%rsi` (`mov %r9,%rsi`), tests
`CR4.LA57`, and does the `fixup_pointer()` idiom — the signature **`__startup_64(unsigned long
physaddr, struct boot_params *bp)`** of `arch/x86/kernel/head64.c` v6.1 (`READ`, fetched source).
So the answer to the unit's question is: **YES — the port executes `__startup_64`.**

Its loops ran too (MEASURED, ring + break): the `level2_kernel_pgt` invalidation loop
(`andq $~1,(%rax); add $8; cmp; jne`) at `0x1000661`, and the tail
`*fixup_long(&phys_base, physaddr) += load_delta - sme_get_me_mask()` as a store at
**phys `0x2a1a010`** (`BREAK 0x10006ef rax=0x2a1a010 rdi=0x84790`).

## 3. `__startup_64` returns 0, and 0 is CORRECT on this machine (MEASURED + READ)

`head64.c`'s last line is `return sme_postprocess_startup(bp, pmd);` (`READ`). The port runs that
function and takes its **early return** — `cmp qword [sme_me_mask],0; je` → `xor eax,eax` →
`ret` — because the machine has no memory encryption. `V86C_PEEK=0x2a4c8f8:16` confirms the mask
global is 0. So `%rax = 0` at `0x1000060` is the non-SME answer, not a defect.

## 4. The CR3 is the value the kernel's arithmetic produces (MEASURED + READ)

`head_64.S` v6.1 forms it as `addq $(early_top_pgt - __START_KERNEL_map), %rax` (`READ`). This
6.1.176 image adds one more step, MEASURED:

    0x1000060  add %rax, imm      → 0x1000066 rax = 0xaeea000
    0x10000a4  add %rax, [0x2a1a010]  → 0x10000ac rax = 0x30ea000   ([0x2a1a010] = 0xfffffffff8200000 = -0x7E00000)
    0x10000b5  mov %cr3, %rax     → cr3 = 0x30ea000

The immediate in memory is `0xaeea000` = `0x30ea000 + 0x7E00000`, and the global the kernel adds is
`-0x7E00000`: the decompressor's relocation delta is cancelled by the kernel's own, so the CR3 is
`early_top_pgt - __START_KERNEL_map` = **`0x30EA000`**, which is `fixup_pointer(&early_top_pgt,
0x1000000)` = `0xffffffff830ea000 - _text + 0x1000000`. **The CR3 is right.** (The `0x7E00000`
delta itself is a decompressor-side question — `output` = `0x8E00000` was chosen and then the
kernel was extracted at `0x1000000`; `V86C_PEEK=0x8e00060` is all zeros. Named as the next fork in
§6; it cancels exactly, so it is not what stops this kernel.)

## 5. What IS missing, and the two corrections to the earlier units

**5a. `early_top_pgt`'s page is never written by the kernel (MEASURED, widened).** A 24 KiB window
`V86C_WATCH=0x30ea000:24576` — the whole `early_top_pgt` page plus the dynamic page tables
(`r10 = 0x30ed000` at the fixup) — sees **24,589 writes, every one of them the decompressor's**
(`0x4f399c0` 24547, `0x4f3a168` 29, `0x4f39d3d` 13). So `pgd[0]`/`pgd[511]` do not land, and the
kernel's first 4-level walk reads image bytes as a PML4 entry — the fault chain
`results/x86-64-firstfault.md` recorded.

**5b. The earlier "no writer" conclusions rest on an instrument that was NOT exhaustive
(MEASURED).** `string.c:140-143` stores through `safe_write*_addr`, and the bulk fast path calls
`memcpy_no_mmap_or_dirty_check`/`memset_no_mmap_or_dirty_check` once per run (`string.c:474`,
`:492`, `:508`). The watch hook lived in the **non-`_addr`** `safe_write*`, so *every* long-mode
store and *every* bulk copy was invisible. Proof, not inference: `[0x2a1a010]` holds
`0xfffffffff8200000` in the port's run while the watch on `0x2a1a000:32` logged only the
decompressor's flush — a real value with no logged writer. **A watch that reports no writer is not
evidence of no writer.** (A concurrent writer on this hunt closed the long-mode half by moving the
hook into the `*_addr` twins — `paging.c`; §7.)

**5c. The fix landed here is the other half: a RANGE hook for bulk copies.**
`v86c_dbg_watch_range(phys, len, rip)` — one call per copy, overlap test only, so a 65 MB `memcpy`
costs what a one-byte store does — wired into the two bulk sites in `string.c`. Its arm
(`test_debug_toolkit_sees_bulk_string_store`) is able to fail: a copy that ends one byte short of
the watched byte must count 0, a copy that covers it must count exactly 1.

**NAMED GAP:** the arm probes the overlap logic directly; the *wiring* (that the fast path calls
it) has no probe of its own yet — the next guest run with `V86C_WATCH` is what exercises it, and
until then this is a seat with a unit test and no integration test.

## 6. The next fork, named

Two candidates remain for "why do `pgd[0]`/`pgd[511]` not land", and the instrument for each is
now in place:

1. **The fixup's `physaddr` is not what `fixup_pointer` needs.** `fixup_pointer(ptr, physaddr) =
   ptr - _text + physaddr`; with `physaddr = 0x1000000` `early_top_pgt` resolves to `0x30ea000`
   (which is why the CR3 agrees). `V86C_BREAK=0x1000613` prints `%r10` — the page table the
   identity-map loop stores into — and `V86C_BREAK_CHAIN`/`V86C_PEEK` name the physical page it
   actually reaches.
2. **The decompressor's `output`/delta divergence** (`0x8E00000` chosen, `0x1000000` used): find
   `handle_relocations` in the stub (not in the vmlinux; it is the `compressed/` code) and break on
   its patch store. It cancels in the CR3 but it does *not* cancel in `__startup_64`'s page-table
   entries, which are built from `physaddr` and `load_delta`.

## 7. A collision that must be recorded, not smoothed over

`src/cpu/paging.c` and `src/cpu/m1c_selftest.c` were modified **while this unit was measuring**
(`paging.c` 00:00:14, `m1c_selftest.c` twice more afterwards), by another writer working the same
wall under a different report name (`results/x86-64-reach2.md`, which does not exist). Their
`paging.c` change is *better* than this unit's first attempt — it moved the per-store hook into the
`safe_write*_addr` twins, which covers every long-mode store — so it was left intact, and this
unit's own per-element `string64_write` hook was **removed** as a double-log under it. Their
`test_watch_sees_long_mode_store` is unfinished (its fixture's store does not land and the count is
2, not 1) and fails, so **`make selftest` is red on a tree this unit does not own**. One writer per
lane; the failure is recorded here rather than fixed in someone else's in-flight edit.

## 8. Files touched

| file | change |
|---|---|
| `src/cpu/debug.h` | `V86C_BREAK`/`V86C_BREAK_MAX`/`V86C_BREAK_RING`/`V86C_BREAK_CHAIN`/`V86C_PEEK` documented; `v86c_dbg_break_hit`, `v86c_dbg_watch_range`, `v86c_dbg_break_on` declared |
| `src/cpu/debug.c` | the execution watch (registers + ring + chain + peek at the hit), the range watch, break slots in the selftest surface, break counts at a stop, `dump_ring()` factored out |
| `src/cpu/cpu.c` | `if (v86c_dbg_break_on) v86c_dbg_break_hit(start_eip);` **before** `run_instruction` — the registers printed are the ones the instruction sees |
| `src/cpu/string.c` | `v86c_dbg_watch_range` at the two bulk sites (MOVS/STOS fast path) |
| `src/cpu/m1c_selftest.c` | `test_debug_toolkit_sees_bulk_string_store`; the toolkit arm's count 4 → 5 and the break-range assertions |
