# x86-64: `numa_init`'s first two fallible calls return 0 — the block IS registered, and the return is later

**Status: MEASUREMENT (two port runs, `nokaslr`, no source change, no fix). Partially decisive: it
CLEARS both fallible calls the brief named and the cover-memory check, and narrows the early return to
`numa_register_memblks`'s tail. The QEMU leg was not needed for the decision (see §4) and was not run.**
Claims tagged MEASURED / READ / INTERPRETATION.

## 1. `numa_init` found, and the addresses this unit adds (READ, disassembly of `/tmp/vmlinux.bin`)

`dummy_numa_init` = `0xffffffff8309d27d`; `numa_add_memblk` = `0xffffffff8309cee8` (its caller passes
`numa_meminfo` in `%rcx`, and the thunk at `0xffffffff8309d26c` is the
`mov $0xffffffff83f72d20,%rcx; jmp` form). `numa_init` contains the inlined
`memset(&numa_meminfo, 0, …)` at `0xffffffff8309d73c` and the sequence

    ffffffff8309d799: call 0xffffffff8309d3d7      ; numa_cleanup_meminfo(&numa_meminfo)
    ffffffff8309d79e: test %eax,%eax
    ffffffff8309d7a0: js   0xffffffff8309dab5      ; the -EINVAL exit
    ffffffff8309d7ac: mov  $0xffffffff83f72d20,%rdi
    ffffffff8309d7b3: call 0xffffffff8309e841      ; numa_emulation(&numa_meminfo, numa_distance_cnt)
    ffffffff8309d7b8: mov  $0xffffffff82c3df00,%rdi  ; node_online_map
    ffffffff8309d7c4: mov  $0xffffffff831d9ca0,%rsi  ; numa_nodes_parsed
    ffffffff8309d7cb: rep movsl                      ; node_online_map = numa_nodes_parsed
    ffffffff8309d7d4: call 0xffffffff8309d0f8        ; numa_nodemask_from_meminfo(...)
    ffffffff8309d7e5: call 0xffffffff81534490        ; nodes_empty()/bitmap over node_possible_map
    ffffffff8309d7ea: cmp  $0x400,%rax
    ffffffff8309d7f0: je   0xffffffff8309d7fd        ; WARN/BUG
    …
    ffffffff8309d894: cmp  $0xff,%rax ; jle …       ; numa_meminfo_cover_memory's 1 MiB threshold
    ffffffff8309d8a3: mov  $0xffffffff82347718,%rdi  ; "NUMA: nodes only cover … Not used."

**New addresses: `numa_meminfo` = `0xffffffff83f72d20` (`nr_blks` at `+0`, `blk[0]` at `+8`, 24-byte
entries), `node_online_map` = `0xffffffff82c3df00`, `numa_nodes_parsed` = `0xffffffff831d9ca0`,
`nr_cpu_ids` = `0xffffffff82c3ca50` (from the previous unit).**

## 2. The dummy block IS registered (MEASURED, `V86C_WATCH='0x3f72d20:24'`, phys = link − 0xffffffff80000000)

    WATCH P 0x3f72d28 width=4 value=0x0        rip=0xffffffff8309cf4e    ; blk[0].start = 0
    WATCH P 0x3f72d30 width=4 value=0x4430000  rip=0xffffffff8309cf52    ; blk[0].end   = 0x4430000
    WATCH P 0x3f72d20 width=4 value=0x1        rip=0xffffffff8309cf58    ; nr_blks = 1

i.e. `dummy_numa_init`'s `numa_add_memblk(0, 0, PFN_PHYS(max_pfn))` landed: one block, `[0, 0x4430000)`
(68.2 MiB = `max_pfn` 0x4430). Nothing else ever wrote `numa_meminfo`.

## 3. The two fallible calls return 0, and cover-memory passes (MEASURED, `V86C_BREAK`, one hit each)

    BREAK 0xffffffff8309d79e (after numa_cleanup_meminfo)  rax=0x0
    BREAK 0xffffffff8309d7b8 (after numa_emulation)        rax=0x0

and the cover-memory failure message is **absent** from the port's serial (`grep "cover|Not used|NUMA"`
→ only `No NUMA configuration found`), so `numa_meminfo_cover_memory()` returned **true** — the 63 MiB
vs 68 MiB gap I expected does **not** trip it (INTERPRETATION: `numaram` and `e820ram` are both computed
over the same `max_pfn` window, so the missing `0x4000000–0x4430000` counts on neither side).

## 4. What that leaves, stated honestly

Both named fallible calls succeed, the block survives cleanup, and the cover check passes — yet
`numa_init_array()` never runs (`results/x86-64-nrcpu.md`: the early `x86_cpu_to_node_map` is never
written by the kernel). So the divergence is **inside `numa_register_memblks`'s tail** — the
`node_possible_map` path at `0xffffffff8309d7b8…0xffffffff8309d7f0` — not in the E820 shape and not in
the meminfo the firmware supplies. The QEMU leg was not run because the port's own two return values
already rule out the branches the comparison was for; a QEMU comparison of `numa_register_memblks`'s
internals would be the next unit's job, not this one's.

## The ONE next measurement

**`node_possible_map` at `0xffffffff8309d7f0`.** Break there (and at `0xffffffff8309d7d9`, to read the
return of `numa_nodemask_from_meminfo`) on the port and read the 128 bytes at
`0xffffffff82c3df00`/the mask operand — if the mask is empty, `WARN_ON(nodes_empty(...))` takes the
`ud2` path and `numa_init` returns before `numa_init_array`; if it is non-empty, the return is in the
`for_each_node_mask` → `alloc_node_data` loop below `0xffffffff8309d804`, and the operand to read there
is `numa_nodes_parsed` at `0xffffffff831d9ca0` (`node_set(0, …)` from `dummy_numa_init` should have made
bit 0 = 1).
