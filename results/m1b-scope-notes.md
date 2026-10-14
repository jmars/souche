# M1b scope split (orchestrator, 2026-10-11)

The 1083 unimplemented call sites in the generated `src/cpu/interpreter_gen.c` split three ways by
the RUST MODULE the emitter names in the stop message. Regenerate these lists with:

```
cd /home/jaye/futaie/souche
rg -o 'unimplemented_instruction\("[^"]+"' src/cpu/interpreter_gen.c | sed 's/unimplemented_instruction("//' | sort -u > /tmp/missing.txt
```

then classify by the `0Fxx` opcode inside each symbol:

| file | sites | opcode groups |
| --- | --- | --- |
| `results/m1b-scope-non-sse.txt` | 390 | everything in the 0F table EXCEPT the MMX/SSE groups |
| `results/m1b-scope-mmx-sse.txt` | 533 | 0F 10-17, 28-2F, 50-5F, 60-6F, 70-77, 7E/7F, C2-C6, D0-FE |
| `results/m1b-scope-fpu.txt` | 160 | the base-table `D8..DF` group (upstream `instructions.rs:1643-2003`) |

`instr_660F..`, `instr_F20F..`, `instr_F30F..` symbols belong to the group of their opcode
(the prefix is the arm, not a different instruction). Sites reached by MORE than one group
(e.g. `0F 6F` also serves `MMX_SSE`) are listed in the group whose opcode they carry.

The scope files are DERIVED, not authoritative: the emitter's census is. A symbol that is not in
your file because it was already implemented, or because the generator changed, is not an error —
`make` regenerates the dispatcher and the census moves by itself.
