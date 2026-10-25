# x86-64: `V86C_RSPJUMP` — the slot that moves `rsp` by K, with the register file and the delivery count

Unit: build the instrument `results/x86-64-rspjump.md` §3 asked for. Repo `~/futaie/souche`, baseline
HEAD `cd9fd94`. `v86/` untouched, nothing committed, nothing pushed. Scope: the toolkit
(`src/cpu/debug.{c,h}`), the per-instruction seat in `src/cpu/cpu.c`, and the toolkit's own arms in
`src/cpu/m1c_selftest.c`. **The guest's execution path is unchanged**: with the tool off the seat is
two `bool` tests per instruction and nothing else, and the flag-off machine is byte-identical (the
transcript gate and the bench pin below are that measurement).

Claim tags: **MEASURED** = a command run in this unit, output quoted; **READ** = read off the code
with the file:line given; **INTERPRETATION** = an inference over the two.

---

## 0. Bottom line

`V86C_RSPJUMP=K` records every instruction SLOT whose `rsp` moved by at least K bytes, either
direction, and prints — with the register file, `cs`/`ss`, `cpl`, `cr2_64` and **the number of
deliveries inside the slot** — the fields the previous measurement had to infer. The delivery delta
is the point: a line now separates *"an instruction moved rsp"* from *"a delivery moved rsp"*.

| item | verdict |
|---|---|
| the tool | `V86C_RSPJUMP=K` (threshold in bytes, 0/absent = off, capped at 2^40) + `V86C_RSPJUMP_MAX` (events printed, default 16, capped at the ring's 32) |
| the seat | `src/cpu/cpu.c:203` and `:206` — `v86c_dbg_rspjump_begin(start_eip)` before `run_instruction`, `_end()` after it |
| the event | `\|rsp_out − rsp_in\| >= K`, either direction; 4 printed lines; ring of the last 32, dumped from the existing `v86c_dbg_on_stop` with the TOTAL on its first line |
| the four arms | all four green, all four RED against their mutant (**MEASURED**, §3) |
| gates | all five rc 0; bench pin still `200095214` on 7/7 runs (§4) |

The one thing it still cannot say is *which* vector a delivery carried and what the frame it pushed
looked like — that is `V86C_INTS`' job, and the two tools are now cheap to run together (§5).

---

## 1. What the tool is

### 1.1 The surface (READ: `src/cpu/debug.h`)

| flag | meaning |
|---|---|
| `V86C_RSPJUMP=K` | K is a **threshold in bytes**. `|rsp_out − rsp_in| >= K` is an event. `K=0` or absent = **off**. Capped at 2^40 (`V86C_DBG_RSPJUMP_KMAX`). |
| `V86C_RSPJUMP_MAX=N` | **events printed** per run (each event is four lines). Default 16, capped at the ring's 32; `0` means "ring only, nothing printed". |

`K=0` is OFF rather than "every instruction" deliberately: the rule is `>=`, so a threshold of 0 is
satisfied by every slot (a slot moves rsp by at least zero) — an instrument with no falsifier. The
env layer refuses it, and the test-arm entry point clamps to 1 with that reason in the comment.

### 1.2 The event, as it prints (MEASURED: `./build/cpu_m1c_selftest`, the arm's fixtures)

```
v86c dbg: RSPJUMP 2 rip=0xffffffff815bdf65 rsp_in=0xffffd1fac056ba58 rsp_out=0x58ba56c0 delta=-0xffffd1fa679c6398 deliv=0
v86c dbg:   RSPJUMP regs r0=0x0 r1=0x0 r2=0x0 r3=0x0 r4=0x58ba56c0 r5=0x0 r6=0x0 r7=0x0
v86c dbg:   RSPJUMP regs r8=0x0 r9=0x0 r10=0x0 r11=0x0 r12=0x11223344 r13=0x0 r14=0x0 r15=0x0
v86c dbg:   RSPJUMP cs=0xffff888000000000 ss=0xffff888000001000 cpl=0 cr2_64=0xffff8eaa88a00000
```

Line 1 is the line `results/x86-64-rspjump.md` §3 asked for, with `deliv` added to it because the
delivery count is what decides the reading. `r4` is `rsp`, so a capture taken at the slot's ENTRY
would show `rsp_in` there instead of `rsp_out` — the arm asserts `r4 == rsp_out` for exactly that.

A slot that delivers prints `deliv=1` and, with `V86C_INTS` armed alongside, the same window carries
the vector:

```
v86c dbg: RSPJUMP 4 rip=0x1000 rsp_in=0x8000 rsp_out=0x4000 delta=-0x4000 deliv=1
```

The window announces its own closure, so a bounded print is never read as a quiet guest:

```
v86c dbg: RSPJUMP print window CLOSED at 2 event(s) (event 3); the ring keeps the last 4 and the stop dump carries the TOTAL
```

and the stop dump leads with the total:

```
v86c dbg: RSPJUMP total=6 kept=4 ring=4 threshold=0x1 printed=2 (oldest first; each event is one instruction SLOT whose rsp moved by at least the threshold, either way)
v86c dbg: RSPJUMP 3 rip=0x2002 rsp_in=0x1200 rsp_out=0x2200 delta=+0x1000 deliv=0
...
```

### 1.3 The direction rule, and the correction it carries

**The measured jump is a DECREASE, not a rise.** `0x58ba56c0` is numerically *below*
`0xffffd1fac056ba58`, and the magnitude is `0xffffd1fa679c6398` — the number RSPLOW printed as its
delta. (MEASURED: `rsp_in − rsp_out == 0xffffd1fa679c6398` in arm 1.) So this tool is not "the rise
RSPLOW cannot see"; it is **magnitude** where RSPLOW is **direction**: RSPLOW already saw this event
and named the instruction, and what it could not carry was the register file and the delivery count.

The rule is still *either way*, and arm 1b measures why: the same travel in the opposite direction
is exactly as diagnostic, and a recorder that copied RSPLOW's decrease test would silently drop it.
The printed `delta=` carries the sign (`+`/`-`).

### 1.4 The two bounds

* **Print**: `rspjump_max` events (default 16), each four lines, and one CLOSED line when the window
  shuts.
* **Ring**: the last `rspjump_k` events (32 by default, `V86C_DBG_RSPJUMP_RING`), dumped from
  `v86c_dbg_on_stop` — the seat that already runs on every terminal path (instruction budget, HLT,
  an undeliverable exception, a reboot, a named stop: `results/x86-64-instrholes.md` item 3). No new
  stop path was added, which is the brief's constraint and also the reason the dump cannot be missed.

### 1.5 The cost when off

`src/cpu/cpu.c:203`/`:206`: two loads and two branches per instruction, nothing else. (The brief said
"one `bool` test"; it is two, because the slot's ENTRY rsp has to be taken before `run_instruction`
and compared after it — see §2.1.) The flag-off machine is byte-identical: the transcript gate is
byte-identical and the bench pin did not move (§4).

---

## 2. The seat

### 2.1 Two calls around one instruction, not one post-slot hook

`src/cpu/cpu.c`, inside `interpreted_run`'s loop:

```c
if (v86c_dbg_break_on) v86c_dbg_break_hit(start_eip);
if (v86c_dbg_rspjump_on) v86c_dbg_rspjump_begin(start_eip);
run_instruction(opcode | dispatch_os_index(opcode) << 8);
if (v86c_dbg_rspjump_on) v86c_dbg_rspjump_end();
if (v86c_dbg_rsplow_on) v86c_dbg_rsplow_tick(start_eip, cpu->reg64[4]);
```

`begin` stores `rip`, `cpu->reg64[4]` and `v86c_dbg_ints_total()`; `end` compares. The alternative —
a single post-slot hook using the rsp the PREVIOUS slot left, which is RSPLOW's shape — was rejected
because **anything that moves rsp BETWEEN slots would be attributed to the instruction that follows
it**, and attribution is the entire product of this tool. A delivery raised at a batch boundary or a
task switch is exactly that case.

The `_end` call also runs when the slot faulted, because `run_instruction` returns normally after a
delivery (the delivery runs inside it); a slot that stopped the machine never reaches `_end`, and the
stop dump is what carries the ring.

### 2.2 The delivery count, and why it does not depend on `V86C_INTS`

`v86c_dbg_ints_total()` is the toolkit's existing delivery counter (`results/x86-64-ints.md`), and
its `ints_total++` sat behind `if (!v86c_dbg_ints_on) return;`. Read literally, the brief's "take
`v86c_dbg_ints_total()` before and after the slot" would have produced a `deliv` that is **always 0
unless a second tool happens to be armed** — the field that decides the reading would read "no
delivery" in exactly the run where a reader trusts it. So the seat now counts when EITHER tool is on
(`src/cpu/debug.c`, `v86c_dbg_ints_delivery`):

```c
if (!v86c_dbg_ints_on) {
    if (v86c_dbg_rspjump_on) ints_total++;
    return;
}
```

The histogram, the nesting depth and the printed lines stay the trace's own; only the counter is
shared. Arm 3 asserts this with `V86C_INTS` **off** (`CHECK(!v86c_dbg_ints_on)` then `deliv == 1`).

### 2.3 The engine caveat (the same one RSPLOW has)

The seat is the INTERPRETER's. The default engine is the interpreter (`uop_cache_enabled = false`,
`src/cpu/uop_cache.c:818`), so by default every guest instruction passes through it — that is the
configuration the amd64 measurements run in. Under `--uop-cache on` (and therefore `--jit on`) the
decoded-block path does not pass through `interpreted_run`, and only the fallback batches do, so the
tool would see a subset. Stated here rather than implied: this is RSPLOW's blind spot, unchanged, and
it is why the measurement below should be run with the default engine.

---

## 3. The arms, and the mutants each one kills

Two arms in `src/cpu/m1c_selftest.c`:

* `test_debug_rspjump_records` (TEST 1n) drives the two seat calls directly — the RULE, the RING and
  the ENV surface.
* `test_debug_rspjump_loop_seat` (TEST 1o) runs the REAL seat through `interpreted_run` — because a
  seat nobody calls is indistinguishable from an absent seat to every other test in the file (this
  lane has shipped exactly that; the review's M7 lesson is why TEST 1h was rewritten).

### 3.1 The arms

| # | arm | assertion |
|---|---|---|
| 1 | a slot above K is an event with the right `in`/`out`/`delta` | `rip`/`rsp_in`/`rsp_out` equal the fixture, `rsp_in − rsp_out == 0xffffd1fa679c6398`, `r4 == rsp_out`, `r12` carries the slot's register, `cs`/`ss`/`cpl`/`cr2_64` are the machine's |
| 1b | the SAME travel the other way is also an event | `rsp_out > rsp_in` and the total is 3, not 2 |
| 2 | below K records nothing; EXACTLY K records | `0xfff` of motion → total 0; `0x1000` → total 1 |
| 3 | the delivery delta | `deliv == 1` for a slot that delivers (with `V86C_INTS` off), `deliv == 0` for the quiet slot beside it |
| 4 | the ring keeps the LAST N and the header prints the total | six events into a ring of 4 → entries `0x2002..0x2005` (a first-N ring reads `0x2000` at entry 0), `printed == 2`, and the dump alone adds `+2` entries (STOP + summary) |
| — | the env path | `0x1000` arms; `0` leaves it OFF; `0x100000000000` is capped to `2^40`; `V86C_RSPJUMP_MAX=1000` is capped to 32; absent leaves it OFF |

### 3.2 The mutants, MEASURED (each: patch, rebuild `build/cpu_m1c_selftest`, run, restore)

**M1 — the threshold compared with `>` instead of `>=`** (`if (delta <= rspjump_thresh) return;`):

```
=== M1: the threshold compared with > instead of >= (drop delta == K)
exit=1
120:FAIL src/cpu/m1c_selftest.c:1105: v86c_dbg_rspjump_total() == 1 (got 0x0, want 0x1)
125:FAIL src/cpu/m1c_selftest.c:1114: v86c_dbg_rspjump_total() == 2 (got 0x1, want 0x2)
...
cpu_m1c_selftest: 504 checks, 33 failures
```

The arm that pins it is arm 2's second half — a slot that moves rsp by EXACTLY K — and it is pinned
again at the real seat, where `K = 4` and a `push %eax` moves exactly 4 (TEST 1o reads 0 events
under this mutant).

**M2 — the delivery delta always 0** (`deliv = 0;`):

```
=== M2: the delivery delta always 0
exit=1
136:FAIL src/cpu/m1c_selftest.c:1154: e.deliv == 1 (got 0x0, want 0x1)
cpu_m1c_selftest: 504 checks, 1 failures
```

**M3 — the ring keeps the FIRST N** (the write pointer stops advancing once full):

```
=== M3: the ring keeps the FIRST N (fills once, then stops storing)
exit=1
145:FAIL src/cpu/m1c_selftest.c:1172: e.rip == 0x2002 (got 0x2005, want 0x2002)
146:FAIL src/cpu/m1c_selftest.c:1173: e.rsp_in == 0x1200 (got 0x1500, want 0x1200)
...
cpu_m1c_selftest: 504 checks, 6 failures
```

Note the shape of the failure: entry 0 comes back as event 6 and entry 3 as event 3 — the ring reads
its slots in the wrong order, which is what a reader of the tail would mis-read as the descent.

**M4 — the seat deleted from `cpu.c`** (the two calls removed):

```
=== M4: the seat deleted from cpu.c (the two calls removed)
exit=1
169:FAIL src/cpu/m1c_selftest.c:1251: v86c_dbg_rspjump_total() == 2 (got 0x0, want 0x2)
...
cpu_m1c_selftest: 504 checks, 10 failures
```

Only TEST 1o goes red (TEST 1n drives the calls itself, by design) — which is the asymmetry that
proves TEST 1o is the seat's arm and not a second copy of the rule's.

**The restored build (the good one), MEASURED:**

```
=== restored: the good build
build exit=0
exit=0
cpu_m1c_selftest: 504 checks, 0 failures
```

---

## 4. The gates — ONCE, at the end, by EXIT CODE

The edited sources were `touch`ed before the run, so a stale `build/unity.o` could not skip a real
rebuild.

| gate | rc | evidence |
|---|---|---|
| `make` | **0** | rebuild clean, `-Werror`, no `-DNDEBUG` |
| `make selftest` | **0** | `cpu_selftest: 3888`, `cpu_m1c_selftest: 504`, `hw_selftest: 975`, `uop_cache_selftest: 7934363`, `uop_exec_selftest: 102`, `jit-selftest: 68`, `jit_codegen_selftest: 3570` — **7,943,470 checks, 0 failures**, `selftest: all suites green` |
| `make nasm64` | **0** | `[+] x86-64 corpus: 91 case(s) in 8 family(ies)`, `[+] gate-nasm64: 13 passed, 0 failed` (13/13) |
| `make transcript-diff` | **0** | `milestone: the C transcript is byte-identical to the reference's` |
| `make bench` | **0** | 7/7 runs `instr=200095214  state=ok serial=ok jit-inline=ok`; `verified (every run): 200095214 instructions` — **the pin did not move** |

The two new arms account for the `cpu_m1c_selftest` total of 504 checks (the count is the suite's,
and it prints it last, after every arm). The `--jit`/`--uop-cache` engines were not exercised:
`make bench` is the default (interpreter) workload and it is the pin.

---

## 5. What the tool cannot say, stated so a gap is not read as a promise

1. **Which vector a delivery carried.** `deliv` counts deliveries inside the slot; it does not name
   them. `V86C_INTS=<N>` names them (`vec=`, `err=`, `from_rip=`, `cr2=`), and the two tools are
   independent, so a run with both armed gives the slot's delta AND the delivery lines in the same
   window. Nothing merges them into one line.
2. **`cr2_64` is stale unless the slot's delivery was a walk-raised `#PF`** — the same caveat
   `results/x86-64-instrholes.md` records for the INTS line. A guest `int $0x0e` leaves the previous
   fault's value there.
3. **The seat is the interpreter's** (§2.3): under `--uop-cache on` it sees only fallback batches.
4. **A slot that ends the machine never reaches `_end`** — the event is then only in the ring if a
   LATER slot recorded one, and the ring is dumped at the stop.
5. **It does not name an instruction.** `rip` is the slot's start eip; naming it is
   `tools/kallsyms.py`'s job, and the guest's text is patched (`results/x86-64-rspjump.md` §1), so a
   link address names the kernel function, not necessarily the bytes that ran.

---

## 6. The measurement this unblocks

The one the previous unit wrote down (`results/x86-64-rspjump.md` §3), now with the fields it needed:

    V86C_RSPJUMP=0x1000 V86C_RSPJUMP_MAX=16 V86C_INTS=32 V86C_DUMP_ON_STOP=1 \
      ./build/v86c --bzimage tests/images/vmlinuz-6.1.0-50-amd64 4475481000 --memory 256 \
                   --cmdline 'console=ttyS0' --long-mode on

The line to read is the one whose `rsp_in` is `0xffffd1fac056ba58`: `rsp_out`, the register file and
`deliv` together decide whether the slot's `bswap %r12d` moved rsp (it cannot) or a delivery inside
the slot did (it can), and the `V86C_INTS` window immediately around that sequence names the vector.
