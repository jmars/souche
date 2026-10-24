# x86-64: SYMBOLICATED — the `-1` is `NUMA_NO_NODE`, and it never reached the ACPI tables

**Status: MEASUREMENT (symbolication + source reading; NO source change, no table change, no battery).
The reader is a NODE lookup, so the brief's NODE branch applies — but the evidence puts the failure
*before* any ACPI table the port could publish.** Claims tagged MEASURED / READ / INTERPRETATION.

## 1. Getting symbols (MEASURED)

The `.deb` is not on disk any more, so it was re-fetched from the pinned URL and its sha256 verified
**identical** to the pin (`439422e41d2dbb840b60b81e3bf5e5955bfa56b7a8c268aec83c9ff882d421e6`,
70,302,080 bytes). **Its `/boot/System.map-6.1.0-50-amd64` is a STUB** — one line: "The real System.map
is in the linux-image-<version>-dbg package" — and the `-dbg` package is 854 MB, so instead the bzImage's
own payload was extracted: xz stream at file offset 21196 → a **65,905,060-byte ELF vmlinux**
(`/tmp/vmlinux.bin`, sections `.text ffffffff81000000`, `.rodata ffffffff82000000`, `.data
ffffffff82a00000`, `.init.text ffffffff83077000`). `objdump` on that gives exact disassembly at link
addresses; names come from the code, not from a symbol table.

## 2. The reader IS a node lookup (MEASURED, `objdump`)

    ffffffff812ef8e5: 65 8b 05 94 ec d3 7e   mov %gs:0x2e580,%eax          # this_cpu_read(numa_node)
    ffffffff812ef8ec: 48 98                  cltq
    ffffffff812ef8ee: 55                     push %rbp
    ffffffff812ef8ef: 89 fd                  mov %edi,%ebp
    ffffffff812ef8f1: 48 8b 14 c5 e0 93 c3 82  mov -0x7d3c6c20(,%rax,8),%rdx  # node_data[nid]
    ffffffff812ef8f9: 53                     push %rbx
    ffffffff812ef8fa: 48 8d 82 00 1e 00 00   lea 0x1e00(%rdx),%rax
    ffffffff812ef901: 3b ba 08 1e 00 00      cmp 0x1e08(%rdx),%edi         # ← the fault

i.e. **`NODE_DATA(numa_node_id())`** — the loaded value is a **NODE id**, and `-1` is `NUMA_NO_NODE`.
The array indexed (`0xffffffff82c393e0`) is in `.data`, all-zero at link time over its first 1024
entries, and is **a different array** from the `__per_cpu_offset` the writer uses
(`0xffffffff8241ab60`). So the previous unit's "__per_cpu_offset, i.e. a CPU id" reading is
**withdrawn** — that array belongs to the writer, not the reader.

## 3. The writer, and what it is (MEASURED, `objdump` in `.init.text`)

    ffffffff83093b7f: movslq %ebx,%rdx
    ffffffff83093b82: mov -0x7dbe54a0(,%rdx,8),%rdx   # __per_cpu_offset[cpu]
    ffffffff83093b8a: mov %eax,%ecx                   # eax = return of the call at ...3b7a
    ffffffff83093b8c: mov $0x2e580,%rax
    ffffffff83093b93: mov %ecx,(%rdx,%rax,1)          # per_cpu(numa_node, cpu) = eax
    ffffffff83093b96: test %ebx,%ebx                  # if (!cpu) switch_to_new_gdt(0)

This is `setup_per_cpu_areas`, and the source line is exact (READ, v6.1.176
`arch/x86/kernel/setup_percpu.c:207`):

```c
		per_cpu(x86_cpu_to_node_map, cpu) = early_per_cpu_map(x86_cpu_to_node_map, cpu);  /* +0x20078 */
		set_cpu_numa_node(cpu, early_cpu_to_node(cpu));                                   /* +0x2e580 */
```

So the kernel stored `-1` because **`early_cpu_to_node(0)` returned `NUMA_NO_NODE`**, and
`early_cpu_to_node` is (READ, `arch/x86/mm/numa.c:841`) a read of the early per-cpu
`x86_cpu_to_node_map[cpu]`.

## 4. Why the port is `-1` where QEMU is `0` — and why it is NOT the ACPI tables

On a machine with no SRAT, that map is filled by **`numa_init_array()`** (`numa.c:620-631`), which runs
at the end of `numa_init()` and does `numa_set_node(i, rr)` for every cpu whose
`early_cpu_to_node(i) == NUMA_NO_NODE`, with `rr = first_node(node_online_map)` — i.e. **node 0**.

Both machines take that same path (MEASURED): both serials print the dummy path's own line
`No NUMA configuration found` + `Faking a node at [mem …]`, and **neither prints `NUMA turned off`,
`no memory blocks found`, a `Node alignment … rejecting NUMA config` warning, or any WARN** — QEMU's map
ends up `0`, the port's `-1`. **QEMU has no SRAT either**, so an SRAT/`_PXM` is not what fills the map on
a reference machine; the port's map is left unwritten by a *port-side* difference in the
`numa_init_array()` step (its loop bound `nr_cpu_ids`, or `numa_set_node()`'s write into the early
per-cpu array), not by a table the firmware failed to publish.

**Therefore the brief's step (2) — add a `Processor` object with `_PXM` to the DSDT — was NOT taken:**
the measurement says the ACPI namespace is not the input this map is waiting for on a no-SRAT machine,
and a table change is a battery-gated unit that should not be landed on a guess. (The CPUID branch is
also closed by measurement: leaf 1 EBX = `1<<16 | 8<<8` = `0x00010800` — logical count 1, **initial APIC
id 0**, `src/cpu/instructions_0f.c:1285` — the same id QEMU reports.)

## 5. The ONE next measurement

**`nr_cpu_ids` at the moment `numa_init_array()` runs.** Break at the `numa_set_node` call site in
`numa_init_array` (`objdump` it from `/tmp/vmlinux.bin`; it is in `.init.text` and calls
`set_cpu_numa_node`) and read `nr_cpu_ids` and `node_online_map` on both machines with `nokaslr` — if
`nr_cpu_ids` is `0` there, the loop never runs and the port's map stays `NUMA_NO_NODE`; if it is `1`,
then `numa_set_node`'s write into the early per-cpu array is the port defect and its address is the
next thing to watch. That single value decides between "the kernel never enumerated a CPU yet" (a
machine input: MADT/MPS cpu enumeration, `nr_cpu_ids`) and "the early per-cpu write went astray".
