# x86-64: the masks are fine — `numa_init_array` runs, loops 8192 times, and never calls `numa_set_node`

**Status: MEASUREMENT (four port runs, `nokaslr`, no source change, no fix). The masks the brief sent me
for are correct, the WARN path is not taken, the loop is entered — and the branch that should call
`numa_set_node(0, 0)` never fires for any of the 8192 iterations, on a value that is `-1`.** Claims
tagged MEASURED / READ / INTERPRETATION.

## 1. The masks are fine and the WARN path is not taken (MEASURED, `V86C_PEEK` at breaks)

At `0xffffffff8309d7f0` (`cmp $0x400,%rax; je ud2`), `rax = 0x0` — the `nodes_empty()` call returned
false, so the WARN/BUG is **not** taken. Both masks, read at `0xffffffff8309d7d9` and `0xffffffff8309d7f0`:

    PEEK phys 0x2c3df00 (node_possible_map, 16 bytes): 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    PEEK phys 0x31d9ca0 (numa_nodes_parsed, 16 bytes): 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

**bit 0 is SET in both** — `node_set(0, numa_nodes_parsed)` (the `lock orb` at `0xffffffff8309d2c3`) and the
`rep movsl` into `node_possible_map` both landed, and `0xffffffff8309d804` (the `alloc_node_data` loop)
is entered (2 hits, `rbx` walking `numa_meminfo` entries).

## 2. `numa_init_array` runs — and its call never happens (MEASURED, `V86C_BREAK` hit counts)

    break slot 0 0xffffffff8309da3b+0x1 hits=1        ; numa_init_array entry
    break slot 2 0xffffffff8309da58+0x1 hits=8192     ; its call to early_cpu_to_node, every cpu
    break slot 1 0xffffffff81093980+0x1 hits=0        ; numa_set_node -- NEVER EXECUTED

and in a second run, with the call instruction itself as the slot:

    break slot 0 0xffffffff8309da65+0x1 hits=0        ; the `call numa_set_node` at the loop body
    break slot 1 0xffffffff81093980+0x1 hits=0

The code is (READ, `objdump`):

    ffffffff8309da4e: cmp nr_cpu_ids(%rip),%ebp ; jae exit
    ffffffff8309da56: mov %ebp,%edi
    ffffffff8309da58: call 0xffffffff819f64a0   ; early_cpu_to_node(i)
    ffffffff8309da5d: inc  %eax
    ffffffff8309da5f: jne  0xffffffff8309daaf   ; if (nid != NUMA_NO_NODE) continue
    ffffffff8309da61: mov  %ebx,%esi
    ffffffff8309da63: mov  %ebp,%edi
    ffffffff8309da65: call 0xffffffff81093980  ; numa_set_node(i, rr)

so the `jne` is taken on **all 8192** iterations — the loop concludes `early_cpu_to_node(i) != -1`
every time.

## 3. But the value IS `-1`, and the array really is `-1` (MEASURED)

    BREAK 0xffffffff819f64c6 (after `mov (%rax),%eax` in early_cpu_to_node) rax=0xffffffff rdi=0x0   (hit 1)
    PEEK phys 0x31d1ca0: ff ff ff ff ff ff ff ff        ; the early x86_cpu_to_node_map, still -1
    PEEK phys 0x2bfc528: a0 1c 1d 83 ff ff ff ff        ; early_per_cpu_ptr = 0xffffffff831d1ca0 (intact)

`early_cpu_to_node` = `0xffffffff819f64a0` (`mov early_ptr,%rax; test; lea (%rax,%rdi,4); mov (%rax),%eax`),
`numa_set_node` = `0xffffffff81093980`, `numa_clear_node` = `0xffffffff810939d0`; the early pointer
variable is at `0xffffffff82bfc528` and its image value is `0xffffffff831d1ca0` (correct, READ).

**So the loaded value is `0xffffffff`, `inc %eax` gives `0`, and `jne` — which must not be taken —
is taken anyway.** Two readings survive, and this unit did not separate them:

1. **INTERPRETATION (leading):** the port's `inc %eax` does not produce the `ZF=1` that `0xffffffff → 0`
   requires on this path, so every `if (nid == NUMA_NO_NODE)` test in this kernel reads "not -1".
   This is the classic "the mechanism is right, the thing that reads it is wrong" shape, and it would
   be a port defect in a 32-bit `INC`/flag body in long mode.
2. **INTERPRETATION (not excluded):** the value in `%eax` at `0xffffffff8309da5d` is not `-1` even
   though the array word is — i.e. the call's return value is clobbered between the callee's `mov` and
   the caller's `inc` (the return goes through the `jmp 0xffffffff81e01d30` return thunk).

## The ONE next measurement

**`%eax` at `0xffffffff8309da5d`, plus a hit count on the `jne` target `0xffffffff8309daaf`.** Break at
the `inc` (its `rax` is the value the branch will test) and at the jump target: `rax = 0xffffffff` with
the target hit 8192 times selects reading 1 (a flags defect — then the arm is a one-line `inc %eax` /
`jne` case in `m1c_selftest` with `eax = 0xffffffff`, which fails today and passes after the fix);
`rax != 0xffffffff` at the `inc` selects reading 2 (the call's return value is lost on the way back).
