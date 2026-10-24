# x86-64: the stack straddles a 1 GiB boundary and only the UPPER half's PDPTE is ever written

**Status: MEASUREMENT ONLY (one KASLR boot watching the whole table page, no source change). The
populating store for the stack's PDPTE is never issued: across the entire run the table page receives
exactly ONE non-zero write, and it is the entry NEXT TO the one the stack needs.** Claims tagged
MEASURED / READ / INTERPRETATION.

## (A) The whole table page, watched (MEASURED, `V86C_WATCH='0x100000:0x1000'`)

1026 watch lines in the whole run, of which **exactly one is non-zero**:

    WATCH P 0x1000d8 width=4 value=0x4b6067 rip=0xffffffff88e7e5f3

`0x1000d8` is PDPT **index 27**; the entry the stack needs is `0x1000d0` = **index 26**, and no non-zero
write ever lands there. The writer, at slide `0x7E00000` → link `0xffffffff8107e5f3`:

    ffffffff8107e5f0: mov %rsi,(%rdi)      ; ← a generic single-entry store (`set_pXd(p, value)`)
    ffffffff8107e5f3: jmp 0xffffffff81e01d30

so the *store itself* is a plain 8-byte helper call — a port defect would have to be in **which entry its
caller passes**, not in the store.

## (B) Where `rsp` sits (MEASURED, the same run's registers)

    rsp = 0xffffd206bfffffc0   -> PML4 420, PDPT index 26   (the entry that is ABSENT)
    rbx = 0xffffd206c0000382   -> PML4 420, PDPT index 27   (the entry that IS mapped, 0x4b6067)

The two are 0x3c2 bytes apart and **straddle the 1 GiB boundary at `0xffffd206c0000000`**: the task's
stack top (`rbx`-relative) is in the mapped region, and `rsp` has descended 0x3c2 bytes into the region
the kernel never mapped. The stack therefore spans both 1 GiB regions, and only the upper one has a PDPT
entry.

## The two sentences

- **(A)** The populating store for the stack's PDPTE was **never issued** — index 27 was written (by the
  generic `set_pXd` helper at link `0xffffffff8107e5f0`) and index 26 never was, so this is not a store
  that landed at the wrong address; it is a store that does not exist in this run.
- **(B)** `rsp` is not a mis-derived pointer in the truncation sense: it is 0x3c2 bytes below a stack top
  that sits in the *mapped* region, so the kernel is running on a stack whose lower half it never mapped
  — which is either the kernel's own lazy-vmalloc ordering (and the port is asked to deliver a fault it
  cannot, `results/x86-64-deliver2.md`) or a population the kernel issued and the port lost; **this run
  cannot separate those two**, and the difference is exactly one call.

## No fix

The evidence does not name a one-line defect: the store that would have to be wrong does not exist in the
run at all, and the alternative (a stack pointer the port computed) is contradicted by `rbx` sitting in
the mapped region 0x3c2 bytes above it.

## The ONE next measurement

**List every page-table entry the kernel writes, with its caller.** Break at the generic store
`0xffffffff8107e5f0` and read `rdi`/`rsi` for **every** call (the helper is used for every `set_pXd` in
the kernel), then check whether a call with `rdi = 0x1000d0` exists at all: if the kernel never makes that
call, the missing mapping is the kernel's own ordering on this machine and the next unit is the delivery
of a fault the kernel cannot take (a `#DF` the hardware would raise); if it makes the call and the port
drops it, the `rip` of that call's caller is the defect and the arm is a store to a PDPT slot through the
same helper.
