# x86-64: `page_offset_base`'s low half is wrong — and it explains the whole recursion

**Status: MEASUREMENT, mechanism named end to end. The wrong high half the previous unit localized is
not in the per-cpu base at all: the per-cpu base is computed **correctly** from
`page_offset_base`; the *global itself* carries a wrong **low** half, and every reader after the
randomization therefore disagrees with the mapping code. No fix is claimed — the producer of that
store is the next single measurement.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. The store, and its operands (MEASURED)

`V86C_WATCH=0xffffffff8aa393e0:16` (the runtime VA of `__per_cpu_offset[0]`, link `0xffffffff82c393e0`
+ the `0x7E00000` slide) logs exactly one write:

    WATCH V 0xffffffff8aa393e0 width=8 value=0xffff8c16c0fd4000 rip=0xffffffff8ae9d9c6

so the value is **already wrong when it is stored** — shape (a) or (b), **not** (c) (the `wrmsr` path
is not implicated). The writer is link `0xffffffff8309d9be`:

    ffffffff8309d98b  49 01 c5                  add %rax,%r13
    ffffffff8309d9be  4c 89 2c c5 e0 93 c3 82   mov %r13,-0x7d3c6c20(,%rax,8)   ← __per_cpu_offset[cpu] = r13

and `V86C_BREAK` on the `add` shows **`%rax = 0xfd4000`** — the *physical* offset of the per-cpu area,
with no high bits — while `%r13` (the other operand) is `page_offset_base`:

    PEEK phys 0x2413d60 (page_offset_base) = 0xffff8c16c0000000

**INTERPRETATION, and it is exact:** `0xffff8c16c0000000 + 0xfd4000 = 0xffff8c16c0fd4000` ✓ — the
stored value is the *correct* result of adding the *stored* base to the physical offset. So the
per-cpu base is not the defect; **the base is.**

## 2. The base is wrong in its LOW half (MEASURED)

The kernel randomized `page_offset_base` (`kernel_randomize_memory`, a multiple of 1 GiB added to
`__PAGE_OFFSET_BASE_L4`):

    early boot (results/x86-64-kgs.md):  page_offset_base = 0xffff888000000000
    now, in memory (PEEK 0x2413d60):     0xffff8c16c0000000
    what the kernel's OWN arithmetic used: 0xffff8c1600000000

The randomization's own value must be 1 GiB-aligned with a **zero low 32 bits**
(`0xffff8c1600000000` — a multiple of `0x40000000` from `0xffff888000000000`); the value in memory has
`0xc0000000` in its low half instead of 0. **That is a wrong LOW half**, and it is what every later
reader gets.

## 3. Why that produces the recursion, link by link (MEASURED)

The randomized direct map's page tables are built with `phys = va - page_offset_base`, so the wrong
base shifts every entry by `0xc0000000`. `V86C_BREAK_CHAIN=0xffff8c16c0fd4000` (the per-cpu area):

    PML4E  @0x2a108c0 = 0x4400067        present
    PDPTE @0x44002d8 = 0x4401067        present
    PDE   @0x4401038 = 0x8000000000e000e3   → 2 MiB page at phys 0x00e00000
    PTE   @0xe00ea0 = 0x0  (ABSENT)          (the dump continues past a PS page; the PDE is the answer)

The correct physical address for that VA is **0xc0e00000**, and `0xc0e00000 - 0xc0000000 = 0x00e00000`
— exactly the entry's address. So the chain, the offset and the base agree on one story:

1. the per-cpu area lives at physical `0xc0fd4000` (a legitimate 192 MiB `memblock` placement; QEMU's
   is `0xffb5000`, equally legitimate — the *allocation* is not the defect);
2. its direct-map VA is `0xffff8c16c0fd4000` and is stored correctly;
3. the **mapping** for that VA points at phys `0xfd4000` instead of `0xc0fd4000`, because it was
   built with the wrong base;
4. so the kernel's `mov %gs:0x…,%eax` (the per-cpu `cpu_number`) reads **garbage** (`0x1e00`);
5. `__per_cpu_offset[0x1e00]` is out of bounds → `%rdx = 0` → `cmp %edi,0x1e08(%rdx)` faults at
   `0x1e08`;
6. the delivery then reads the CS descriptor at `gdtr + 0x10` = `gs_base + 0xb000 + 0x10` =
   `0xc0a0b010` — and that page is unmapped for the same reason — so the delivery faults, four
   times, and the bound fires.

**So the whole chain is one wrong 32-bit half in one global**, and it is the 32-bit-halves class this
lane has paid for repeatedly.

## 4. The next fork, one measurement

**Who writes `page_offset_base`'s low half?** `V86C_WATCH=0xffffffff82413d60:8` on the runtime VA of
the global (link `0xffffffff82413d60` + the slide) — the watch prints the writer's rip and the stored
value, so it names the instruction and shows whether it stores a *good* value that something else
spoils, or a bad one directly. Then the shape: a 32-bit store where a 64-bit one belongs, a halves
swap in a 64-bit move, or an arithmetic carry.

**No fix here** — the producer is unidentified and the shapes differ in their fixes.

## 5. Verification (MEASURED; **no source change** — instruments only, so the subset)

    make                     rc 0
    make selftest            rc 0 — all suites green
    gate-x86-64.sh           14 passed, 0 failed   (the banner still gated)
    make bench               200095214

No pin moved. `git status` shows only this report and `TODO.md`.

## 6. Files touched

| file | change |
|---|---|
| `results/x86-64-halves.md` | this report |
| `TODO.md` | §8 |
