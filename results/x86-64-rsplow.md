# x86-64: the descent is RECORDED — `V86C_RSPLOW`, `cr2_64` on the stop, and a kallsyms parser

**Status: two instruments and one naming tool landed, all default-off, nothing committed. The
instruments are MEASURED (seven mutants, each caught); the parser is MEASURED and proved against
anchors that are independent of it. What is NOT done here is the measurement they unblock: the amd64
guest was not run (the brief reserves that run for the orchestrator).** Claims tagged
MEASURED / READ / INTERPRETATION.

## 0. Bottom line

- **`V86C_RSPLOW=K`** (A) records, per instruction, the moments `rsp` reached a **new minimum** —
  `rip`, `rsp`, and the delta from the previous low — in a ring holding the **last K** events, dumped
  on any stop with the **total** on the first line. It is armed by the same one-shot env parse as the
  rest of the toolkit, capped at the ring bound the file already used (`V86C_DBG_RING_MAX` = 4096),
  and costs one `bool` test per instruction when off.
- **`cr2_64`** (B) now prints on the `state rip=...` line of the stop dump, beside the 32-bit `cr2`.
- **`tools/kallsyms.py`** (C) names a link address from the pinned image's own kallsyms tables. It
  produces page-fault-looking names for both #PF-path anchors, `__stack_chk_fail` for the canary-fail
  anchor, and — the part that matters — it names the six repeated frames of the descent:
  `page_fault_oops`, `search_module_extables`, `search_bpf_extables`, `vsnprintf`, `pointer`,
  `symbol_string` (§4.3). **The `.deb`'s System.map is a decoy; the image's own table is the route,
  and the route works.**

## 1. `V86C_RSPLOW=K` — the seat, and why it is where it is

    src/cpu/debug.c:97-146    the state, the arm surface, the getters, the seat
    src/cpu/debug.c:223-232   the env parse (V86C_RSPLOW=<K>, K > 0, capped at 4096)
    src/cpu/debug.c:595-618   dump_rsplow(), called from v86c_dbg_on_stop (:629)
    src/cpu/debug.h:22,45,71  the surface doc, the mask, the seat declaration
    src/cpu/cpu.c:199-204     THE SEAT: one call in interpreted_run's per-instruction loop

**THE SEAT IS ONE CALL, AND IT IS AFTER THE INSTRUCTION.** The brief pointed at `cpu.c:199-203`; that
window holds the ring hook, which runs *after* `run_instruction`, while `v86c_dbg_break_hit` sits one
line *before* it. The ring must be after: an event is the `rsp` an instruction **left** (a `push` or a
`sub rsp` lowers it), so recording before dispatch would attribute every descent step to the
instruction that follows it — a one-instruction misattribution of exactly the thing the ring exists to
name. The value recorded is `start_eip`, captured before dispatch: **the instruction's own address**,
which is what a kallsyms lookup names. `cpu->previous_ip` was rejected for the same reason it exists
elsewhere (it is the *interrupted* rip for the delivery trace); here the interesting rip is the one
that moved rsp.

**WHAT IS AN EVENT (the definitions the report is accountable for):**

- `cpu->reg64[4]` **strictly lower** than every value seen so far. Equal and higher record nothing —
  the ring is the descent's frontier, not a stack trace, so a value that recovers and comes back to a
  level already reached is not an event.
- The **first** value seen only seeds the minimum and records nothing, so every recorded event is a
  real step below a previous low and `delta` (previous minimum − rsp) is the size of that step.
- **Long mode is not a condition** (the brief left the choice open, so it is stated here): a 16- or
  32-bit guest has a stack too, gating on `EFER.LMA` would make the instrument silently useless for
  any other investigation, and the tool is default-off, so the flag-off machine is untouched either
  way. The minimum is per-process and mode-agnostic; a run that changed modes would carry its minimum
  across, which is what "the stack only went down" means anyway.

**THE DUMP** (on any stop, oldest first, in the file's existing style):

    v86c dbg: RSPLOW total=6 kept=4 ring=4 min=0x3b00 (oldest first; delta from the previous minimum)
    v86c dbg: RSPLOW 3 rip=0x1018 rsp=0x3e00 delta=-0x100
    v86c dbg: RSPLOW 4 rip=0x101c rsp=0x3d00 delta=-0x100
    v86c dbg: RSPLOW 5 rip=0x1020 rsp=0x3c00 delta=-0x100
    v86c dbg: RSPLOW 6 rip=0x1024 rsp=0x3b00 delta=-0x100

The **total is on the first line on purpose**: "the last 4096 of N" is only readable if N is printed,
and a bounded window read as the whole story is this lane's most expensive instrument failure. The
event numbers are absolute (3..6 above), so the tail can be lined up against the total. A ring with no
event prints `RSPLOW no event: rsp never went below its first value` rather than an empty silence.
`kept == 0` never suppresses the summary (the summary is gated on the tool being ARMED, not on having
something to show) — that is the budget-knob defect this lane has already found once.

**STATED LIMIT (READ, not measured here):** the seat is in `interpreted_run`, so a batch run by the
uop cache or the JIT does not reach it — the same limit `V86C_BREAK` and `V86C_RING` already have, and
the reason `V86C_BREAK` worked on the amd64 guest in `results/x86-64-stackoverflow.md` §5 (that guest
runs through this loop). A run whose engine is the uop cache will show `total=0`; that is a quiet
engine, not a silent instrument, and it is the one thing a reader of the dump must know.

## 2. The arm, and the seven mutants it is measured against

    src/cpu/m1c_selftest.c:768   test_debug_rsplow_ring      (the ring's own behaviour + the env path)
    src/cpu/m1c_selftest.c:850   test_debug_rsplow_loop_seat (the seat IS called from the loop)
    registered at                :2052 and :2053

Every mutant below was **built, run and restored**, and each is caught:

| # | mutant (the defect) | what the arm reports |
|---|---|---|
| M1 | `rsp >= min` becomes `rsp > min` (a non-lower value records) | 8 assertions fail: total 1 vs 0 after an EQUAL rsp; entry 0 rip 0x1004 vs 0x1008 |
| M2 | the SEED is recorded as an event | 9 fail: total 7 vs 6, and entry 0 is the seed's zeroed slot |
| M3 | the ring keeps the **FIRST** K (stops storing once full) | 9 fail: entry 0 = rip 0x1008 / rsp 0x3ff0 / delta 0x10 where the LAST-K step 0x1018 / 0x3e00 / 0x100 is asserted — **the wrong descent step, which is the reading error the ring exists to prevent** |
| M4 | `delta` computed AFTER the minimum moved | 2 fail: delta 0x0 vs 0x10 |
| M5 | the dump gated on another tool (`ints_on`) | 1 fail: on_stop +1 vs +2 |
| M6 | the env cap removed | 1 fail: limit 100000 vs 4096 |
| M7 | **the seat deleted from `cpu.c`** (the ring works, nothing calls it) | the loop arm fails: total 0 vs 2, then the entry assertions (0x1024/0x3b00 = stale slots) |

M7 is the one that matters most, and it is why there are two arms: a tool wired into `debug.c` but
never called from a run path is indistinguishable from an absent tool to every arm that calls its own
function directly. `test_debug_rsplow_loop_seat` runs the REAL loop — six bytes at `0x2FFA`
(`nop; nop; push %eax; push %eax; nop; nop`) placed so the batch ends on a page crossing — and
asserts two events, at rips `0x2FFC`/`0x2FFD` with `rsp` 0x6ffc → 0x6ff8. **MEASURED: with the seat
removed, `v86c_dbg_rsplow_total() == 2` reads 0** (`m1c_selftest.c:869`).

**END-TO-END THROUGH A REAL IMAGE (MEASURED, and it is not the amd64 guest):**

    $ V86C_RSPLOW=8 V86C_DUMP_ON_STOP=1 ./build/v86c build/nasm64/alu-div-zero.img 200000 \
        --fixture64 --long-mode on
    v86c dbg: STOP (call_interrupt_vector: idtr_size too small for the vector ...)
    v86c dbg: RSPLOW total=0 kept=0 ring=8 min=0x100 (oldest first; delta from the previous minimum)
    v86c dbg:   RSPLOW no event: rsp never went below its first value

The dump speaks from a run that stopped by name, and `total=0` is the correct answer for a flat
fixture whose `rsp` never descends. (A `V86C_RSPLOW` run on `tests/test.img` prints the `tools on`
banner and no dump at all — that image ends by instruction count, and `v86c_dbg_on_stop` is reached
only through `unimplemented_instruction`, `cpu.c:82`. That is the toolkit's existing contract, not a
new silence.)

**ONE PRE-EXISTING DEFECT, MEASURED AND NOT FIXED (out of this unit's scope, published instead):**
`m1c_selftest.c` prints its `cpu_m1c_selftest: N checks, M failures` summary at line 2043, in the
MIDDLE of the run list — the last ten `RUN(...)` arms (including both arms added here) execute after
it. The printed count therefore excludes them: the suite reports **274 checks** while the arms after
the summary run and can fail the exit code. The exit code is honest; the count is not the whole
suite's. Moving the `printf` to the end is a one-line fix, and it changes a number several reports
quote, so it is the orchestrator's call, not a silent edit here.

## 3. `cr2_64` on the stop line (B)

    -    "v86c dbg: state rip=0x%llx cr0=0x%x cr2=0x%x cr3=0x%x cr4=0x%x efer=0x%llx ..."
    +    "v86c dbg: state rip=0x%llx cr0=0x%x cr2=0x%x cr2_64=0x%llx cr3=0x%x cr4=0x%x efer=0x%llx ..."

One line, one added field, `cpu->cr2_64` — the same field every BREAK dump already prints
(`debug.c:440`). The stop is where the double-fault cascade starts, and the 32-bit `cr2` cannot carry a
long-mode linear address: a stop that says `cr2=0xbfffff90` for a fault at `0xffffd1fabfffffc6` is the
projection, not the fault.

## 4. `tools/kallsyms.py` — naming an address from the image's own table (C)

    python3 tools/kallsyms.py /tmp/vmlinux.bin 0xffffffff81c00c02 0xffffffff81a3d3aa
    kallsyms: 94101 symbols, relative_base=0xffffffff81000000, offsets@0x13614a8 (int32,
    absolute-percpu) names@0x13bd310(+0x12a2af) markers@0x14e75c0 (368) token_table@0x152ca40
    token_index@0x152cdd8; 282304 byte(s) before the token table = a 3-byte-per-symbol table
    (kallsyms_seqs_of_names)
    0xffffffff81c00c02 -> asm_exc_page_fault+0x22
    0xffffffff81a3d3aa -> exc_page_fault+0xca

(Line breaks added for this document; the tool prints one banner line and one line per address. The
image is **read in place** — 65.9 MB, nothing copied into the repo — and the parse takes ~3 s.)

### 4.1 How the tables are found (no hard-coded offsets)

| table | how it is pinned |
|---|---|
| `kallsyms_token_table` / `_index` | 256 **distinct** u16 below 0x2000 whose strings chain exactly into a table ending just before them, every token a non-empty name substring |
| `kallsyms_names` | the candidate position whose header is plausible (`relative_base` a `0xffffffff8…` address, `num_syms` a count, its high halves zero) **and** whose first 24 names decompress to printable symbol names |
| `kallsyms_markers` | the u32 run at `align4(names end)` whose k-th entry **is** the byte offset of symbol 256k |
| `kallsyms_offsets` | int32 × num_syms ending at `kallsyms_relative_base` — with up to 4 bytes of zero padding, decided by §4.2 |
| the gap before the token table | 282,304 bytes = 3 × 94,101 + 1 padding: the shape of a 3-byte-per-symbol index. The parser does not need to know what it is; it only needs the markers to end where they end, and it **prints** the gap rather than assuming it is zero. |

The markers are the check on the walk: a walk off by one byte cannot satisfy them. Names are
token-compressed and the first expanded byte is the symbol **type**, which is skipped exactly as the
kernel's own `kallsyms_expand_symbol` does.

### 4.2 The failure mode this parser actually had, and its mutant

**MEASURED, on this image, in this unit:** `kallsyms_offsets` is 4-aligned and
`kallsyms_relative_base` is 8-aligned, so **4 bytes of zero padding** sit between them. Taking the
array to start at `relative_base − 4·num` (i.e. ignoring the padding) shifts every symbol by one, and
the parser then reports:

    0xffffffff81a3d470 -> get_cpu_entry_area+0x0      <- WRONG (that is __stack_chk_fail's address)
    0xffffffff81c00c02 -> asm_exc_int3+0x22           <- WRONG (that is asm_exc_page_fault's stub)
    0xffffffff81a3d3aa -> __sev_es_nmi_complete+0xca  <- WRONG

Every name is plausible; every one is off by one function. **The discriminator is that kallsyms is
sorted by address**: the right alignment is the one whose addresses are non-decreasing end to end,
because a one-symbol shift appends the padding's zero as a final "address" and breaks the order. With
the padding accounted for, the same three addresses name `__stack_chk_fail`, `asm_exc_page_fault` and
`exc_page_fault`. **This is the mutant of the tool itself, and it is why the anchors below are printed
rather than asserted.**

### 4.3 The anchors — the parser's names, and why they are not circular

**(i) Anchors the brief named.** All produced, none papered over:

| address | the tool's name | the brief's expectation |
|---|---|---|
| `0xffffffff81a3d470` | **`__stack_chk_fail+0x0`** | the canary-fail function ✔ |
| `0xffffffff81c00c02` | **`asm_exc_page_fault+0x22`** | inside the #PF IDT stub ✔ |
| `0xffffffff81a3d3aa` | **`exc_page_fault+0xca`** | inside the #PF handler ✔ |

Both page-fault-looking names the brief demanded are present (`asm_exc_page_fault`, `exc_page_fault`;
this kernel has no `do_page_fault`/`handle_page_fault` symbol — x86-64 splits it into
`exc_page_fault` → `do_user_addr_fault`/`do_kern_addr_fault`).

**(ii) The repeated frames, named** (link addresses, as the brief listed them):

| address | name | what the earlier units said by disassembly |
|---|---|---|
| `0xffffffff81087329` | `page_fault_oops+0xf9` | "a canary-check epilogue" |
| `0xffffffff8114bb15` | `search_module_extables+0x15` | "a `preempt_count` + call pair" |
| `0xffffffff812029cb` | `search_bpf_extables+0x5b` | (unnamed) |
| `0xffffffff819e4290` | `symbol_string+0x0` | "a four-argument function that zeroes a 0x298-byte stack structure" |
| `0xffffffff819e6ce0` | `pointer+0x0` | "the function that tail-jumps to 0x819e4290" |
| `0xffffffff819e74b3` | `vsnprintf+0x1a3` | "the call that returns to 0x819e74b8" |

**(iii) Anchors that do NOT come from the parser's own tables.** These are what make (i) and (ii)
evidence rather than a coincidence, and each was checked against the ELF section headers read
independently:

| name (from kallsyms) | address it got | the ELF section's own address |
|---|---|---|
| `_text` / `_stext` | `0xffffffff81000000` | `.text` @ `0xffffffff81000000` ✔ |
| `__start_rodata` | `0xffffffff82000000` | `.rodata` @ `0xffffffff82000000` ✔ |
| `_sdata` | `0xffffffff82a00000` | `.data` @ `0xffffffff82a00000` ✔ |
| `__bss_start` | `0xffffffff8330d000` | `.bss` @ `0xffffffff8330d000` ✔ |
| `__brk_limit` | `0xffffffff84600000` | `.init.scratch` @ `0xffffffff84600000` ✔ |

Four independent section boundaries landing exactly on the section addresses cannot survive the
one-symbol shift of §4.2, and cannot be produced by a wrong name/address pairing.

**(iv) One more, by hand (MEASURED, this unit):** the bytes at `0xffffffff81a3d450` are
`48 ba 00 10 00 00 00 fe ff ff | 48 63 c7 | 48 69 c0 00 b0 03 00 | 48 01 d0 | e9 …` — `movabs
$0xfffffe0000001000,%rdx; movslq %edi,%rdi; imul $0x3b000,%rax,%rax; add %rdx,%rax; jmp` — which is
`get_cpu_entry_area` (the CPU-entry-area base plus a per-cpu stride), and the tool names
`0xffffffff81a3d450` exactly that. The `__stack_chk_fail` body at `0xffffffff81a3d470`
(`mov (%rsp),%rsi; mov $str,%rdi; call …`) is the canary panic. The two functions are 0x10 apart,
which is why the one-symbol shift in §4.2 was so easy to miss — and why it was measured rather than
reasoned about.

### 4.4 What the names say about the descent (INTERPRETATION — the orchestrator's run confirms it)

`vsnprintf → pointer → symbol_string`, entered from `page_fault_oops → search_module_extables /
search_bpf_extables`, is **printk's `%pS`/`%pB` symbol lookup on the #PF path**. That is a plausible
shape for a monotone descent with no return: the oops printer walking the stack (`%pB` prints a
return address by looking it up), each level doing a kallsyms binary search over a 1.2 MB name table
through a nested `vsnprintf`. **It is an inference from names, not a measurement** — the RSPLOW ring
on the same guest is what will confirm or refute it, and if the ring's tail shows rips in
`symbol_string`/`pointer`/`vsnprintf`, this reading is confirmed; if it shows something else, this
paragraph is wrong and the ring wins.

## 5. The gates (run once, at the end, on the final tree; judged by EXIT CODE)

| gate | exit code | evidence |
|---|---|---|
| `make` | **0** | builds |
| `make selftest` | **0** | `selftest: all suites green`; `cpu_m1c_selftest` green with both new arms |
| `make bench` | **0** | all 7 runs `instr=200095214 state=ok serial=ok jit-inline=ok` — **the pin is UNMOVED**, no re-pin (the tools are default-off) |
| `make transcript-diff` | **0** | `milestone: the C transcript is byte-identical to the reference's` |

No `-DNDEBUG`. Nothing committed, nothing pushed, `v86/` untouched, and the two
`results/m2-5-guest-armA-*` files that were already dirty when this unit started were not touched.

## 6. What this unit does NOT claim

- **No amd64 guest run.** The descent's rip sequence, the per-level delta and the recursion's identity
  are the orchestrator's measurement; these tools are what it needs and nothing here measures it.
- The ring sees **one engine**: `interpreted_run`. A uop-cache or JIT run records nothing (stated in
  §1, same limit as `V86C_BREAK`).
- The parser is structural, not portable-by-assertion: it detects this image's layout (including the
  3-byte gap table and the padding) rather than assuming a kernel version, and it fails loudly
  (`no candidate header decompresses to names`) rather than naming garbage. A different image with a
  different table order would need its own look — that is the honest bound.
- The kallsyms names are **link** addresses. The guest's addresses in the earlier reports are slid
  (`slide 0x27800000`); subtract the slide before asking this tool.
