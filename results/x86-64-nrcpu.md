# x86-64: at `numa_init_array` the map is never written — and the loop bound is not 0

**Status: MEASUREMENT (one port run, `nokaslr`, no source change, no fix). Both branches the brief
offered are REFUTED, by a stronger fact than either predicted: `numa_set_node()` is **never called at
all** — the early `x86_cpu_to_node_map` is never written by the kernel — and the loop bound cannot
explain it (`nr_cpu_ids` was `NR_CPUS` = 8192 at that time, not 0).** Claims tagged MEASURED / READ /
INTERPRETATION.

## The two addresses (READ, from the disassembly of `/tmp/vmlinux.bin`)

| what | link address | how it was found |
|---|---|---|
| `nr_cpu_ids` | `0xffffffff82c3ca50` | rip-relative loads in `setup_per_cpu_areas`'s neighbourhood (`0xffffffff830a2b18`, `0xffffffff830df1c6`) and the store `mov %ebx,nr_cpu_ids` at `0xffffffff8309394a` |
| early `x86_cpu_to_node_map[]` | `0xffffffff831d1ca0` | `per_cpu(x86_cpu_to_node_map, cpu) = early_per_cpu_map(...)` at `0xffffffff83093b61` (`mov -0x7ce2e360(,%rax,4),%ecx`) |
| `node_online_map` | **not resolved** | no symbol table (see `results/x86-64-node.md` §1); it was not needed — see below |

## The measurement (MEASURED, one run, `V86C_WATCH='0x31d1ca0:64,0x2c3ca50:8'`, phys = link − `0xffffffff80000000`)

`nr_cpu_ids`, whole boot: **two writes, both value 1** —

    WATCH P 0x2c3ca50 width=4 value=0x1 rip=0xffffffff83093950   (the store at ...8309394a: `mov %ebx,nr_cpu_ids`)
    WATCH P 0x2c3ca50 width=4 value=0x1 rip=0xffffffff830a8807

and its **image** value is `0x2000` = **8192** = `NR_CPUS` (READ, `/tmp/vmlinux.bin` at
`0xffffffff82c3ca50`). So at `setup_arch` time — when `numa_init_array()` runs — `nr_cpu_ids` was
**8192**, and it is trimmed to 1 much later, at `0xffffffff8309394a` (`setup_nr_cpu_ids`), which runs
before `setup_per_cpu_areas` and long after `numa_init`.

The early `x86_cpu_to_node_map[]`, whole boot: **written only by the decompressor's image load** —

    WATCH P 0x31d1ca0 width=4 value=0xffffffffffffffff rip=0x4f3cfc0   (… repeated across the array)

that is the image's own initial content (`DEFINE_EARLY_PER_CPU(int, x86_cpu_to_node_map, NUMA_NO_NODE)`
= all `-1`), and **not one write afterwards**, by any rip, for the whole boot.

## Interpretation, one sentence each

- **`nr_cpu_ids` is not 0** — it is `NR_CPUS` (8192) at `numa_init_array` time and only becomes 1 later,
  so "the kernel never enumerated CPU 0" is **not** the cause; a `numa_init_array` that ran would have
  written the array 8192 times, and it wrote it zero times.
- **`numa_set_node`'s write is not going astray** — it was never issued, so there is no destination to
  watch; the second branch is refuted too, and `node_online_map`'s value is therefore irrelevant to
  this step.
- **`numa_init()` returned early, before `numa_init_array()`, and silently** — the port's serial
  carries `No NUMA configuration found` and `Faking a node at [mem …]` (both `dummy_numa_init`'s own
  printks) and then **none** of the messages the fallible exits would produce: no
  `NUMA: nodes only cover … Not used.`, no `NUMA: no memory blocks found`, no `Node alignment …
  rejecting NUMA config`, no WARN.

## The ONE next measurement

**Which of `numa_init()`'s early returns fires.** `dummy_numa_init` is at `0xffffffff8309d26c`
(found by its own string `0xffffffff8234785c` = "No NUMA configuration found"), and its callers are
`0xffffffff8309d2da`, `0xffffffff8309df62`, `0xffffffff830c7432`, `0xffffffff830c75f6` — disassembling
around those gives `numa_init`'s address. Then break at each of `numa_init`'s three fallible calls
(`numa_cleanup_meminfo`, `numa_register_memblks`) and read the returned `eax` and `numa_meminfo.nr_blks`
on the port: whichever returns `< 0` is the defect, and `nr_blks` distinguishes "the dummy block was
never added" from "the block was added and then trimmed away".
