# x86-64: `V86C_INTS=N` — the delivery trace that names the guest's storm

**Status: MEASURED. One new instrument in the debug toolkit, one call at the ONE delivery seat, one
selftest arm proved by nine mutants, four gates green, the bench pin unmoved.** Claims tagged
MEASURED / READ / INTERPRETATION.

This is the instrument results/x86-64-stackoverflow.md §7 and results/x86-64-peekperturb.md §5 both
asked for. The guest's blocker — a ~0x700-per-level stack descent that never unwinds
(stackoverflow §5), through a self-similar five-frame chain repeating every 0x380 (peekperturb §4)
— could only be *inferred* from stack arithmetic, because the port could not see a single
interrupt or exception delivery. It can now.

## 1. The shape of the tool (MEASURED)

`V86C_INTS=<N>`, default OFF (absent or `0`), `N` capped at `V86C_DBG_INTS_MAX = 100000`
(`src/cpu/debug.c:46`) so a delivery storm cannot fill a disk. The state, the seat and the
selftest surface live together at `src/cpu/debug.c:36-77`, the seat at `:460`, the summary at
`:497`.

One unbuffered line per delivery while the print window is open:

    v86c dbg: INTS <seq> vec=<v> int=<0|1> err=<0x..|-> cpl=<c> depth=<d> from_rip=0x<..> rsp=0x<..> cs=0x<..>

`depth` is the depth the delivery is **NESTED IN** (0 = not nested), `from_rip` the interrupted
instruction, `rsp`/`cs` the machine's state at the seat. When the window closes, ONE line says so
(`INTS print window CLOSED at N line(s) (delivery K)`) — a later silence must never be read as a
quiet guest; this lane has lost five units to an instrument's silence.

**What is counted is NOT what is printed.** Every delivery increments the total and the per-vector
histogram and moves `max_depth` whether or not a line prints, and `v86c_dbg_on_stop` dumps:

    v86c dbg: INTS total=<T> max_depth=<D> printed=<P> limit=<L>
    v86c dbg:   INTS vectors: vec=<v> x<count> ... (the eight most frequent)

The summary is gated on the trace being ARMED, never on the print limit: with `V86C_INTS` at a
small N (or the arm's limit 0) the histogram is the only thing that answers *which vector drives
the loop*, and a summary that goes quiet with the lines would be the same defect as the budget knob
this lane already found once — declared, and unable to fire.

## 2. The seat (READ, `src/cpu/exceptions.c:561-567`)

    v86c_dbg_ints_delivery(interrupt, is_int, has_error_code, error_code, delivery_depth,
                           cpu->previous_ip, cpu->reg64[4], cpu->segment_offsets[CS]);

One call, in `call_interrupt_vector`, **before** `delivery_depth++` and after the nested-delivery
bound's refusal. Every interrupt and every exception the port delivers passes this function, so
this is the only seat there is; no delivery logic was moved or changed. `cpu->previous_ip` is the
interrupted rip because that is the instruction that faulted or was preempted — the same value
`paging.c:144-148`'s rewind pushes as the frame's eip. The printed `cpl` is `cpu->cpl` read inside
`debug.c` (like `v86c_dbg_break_hit` reads the registers), so the delivery's own CPL change cannot
retroactively relabel it.

The cost when off is one `bool` test at the seat, the same discipline as the other tools — and the
bench pin below is the measurement that it is free.

## 3. The arm, and the nine mutants it catches (MEASURED)

`m1c_selftest.c:664`, registered at `:1915`. It drives the **real** seat through `trigger_de()` on
the file's own `machine_setup()` fixture — three flat deliveries, then TEST 3's nesting fixture (a
#DE whose gate names an LDT selector in an absent page, so the delivery's own lookup faults and the
#PF is delivered at depth 1) — and then asserts the ENV path, which is the instrument the user
actually runs. Each assertion was proved able to fail by building the mutant, running it, and
restoring (`/tmp/ints_mut.py`, nine mutants, all caught, real tree green after):

| mutant | assertion that catches it |
|---|---|
| M1 count only what prints | `total() == 3` got 1; `count(0) == 3` got 1 |
| M2 the print bound does not bound (`<=` for `<`) | `printed() == 1` got 2 |
| M3 `max_depth` never updated | `max_depth() == 1` got 0 |
| M4 the depth recorded is the POST-increment one | `max_depth() == 0` got 1, then `== 1` got 2 |
| M5 the histogram is never counted | `count(0) == 3` got 0; `count(14) == 1` got 0 |
| M6 `on_stop` keeps its old early return | `test_counts() == before + 2` got +0 |
| M7 the summary is gated on the print limit | `test_counts() == before + 2` got +1 |
| M8 the storm cap does not cap | `limit() == 100000` got 1000000 |
| M9 `V86C_INTS` parses but never arms the seat | `v86c_dbg_ints_on` |

The shape each one pins, in words: **three deliveries, three counted** (a seat that counts only
what it prints reads 1); **limit 1, three deliveries, one printed line** (the bound really bounds);
**three sequential deliveries nest in nothing (0) and the nested fixture records 1** (depth is
measured, and measured at the nesting the delivery is *in*); **the histogram names vector 0 ×3 and
then vector 14 ×1** (the #PF the delivery itself raised — this is the question the lane asked); and
**a limit of 0 still reports** (the summary is not a function of the print window).

## 4. Gates (MEASURED, run once on the final tree)

| gate | rc | note |
|---|---|---|
| `make` | 0 | no warnings (`-Werror`), no `-DNDEBUG` |
| `make selftest` | 0 | `cpu_m1c_selftest: 274 checks, 0 failures`, all suites green |
| `make bench` | 0 | pin **200,095,214** instructions on every one of 7 runs — UNMOVED (the tool is default-off) |
| `make transcript-diff` | 0 | `results/transcript.c.txt` byte-identical to the reference's (83 lines each) |

A moved pin would have been published as a finding, not re-pinned; it did not move.

## 5. What the tool answers, and what it cannot

**It answers**: which vector was delivered, from which interrupted rip, at which nesting depth, at
which rsp/cs/cpl, how many of each — i.e. whether the guest's 0x380-stride self-similar chain is a
delivery storm, and if so which vector and where it re-enters. Read the last ~40 `INTS` lines plus
the stop summary.

**STATED LIMITS, so no gap is read as a promise:**

- A delivery REFUSED by the nested-delivery bound (`delivery_depth >= v86c_dbg_delivery_bound`) is
  not counted: it never happens. Its refusal is its own `unimplemented_instruction` stop message.
  The bound is default 4 and `V86C_FAULT_DELIVERY_BOUND=N` raises it — a run that wants to see
  past depth 4 needs both.
- The histogram covers vectors 0-255; a vector outside that range is counted in the total but not
  in the histogram. Counters are `int` (2.1e9 deliveries would wrap; nothing measured comes near).
- The printed `cpl` is the CPL **being interrupted**, not the CPL the handler will run at.
- `v86c_dbg_init` is a ONE-SHOT parse: it arms tools and never disarms, so `absent = off` holds for
  the shipped run (`main.c:1650` calls it once) and the arm asserts it from a clean state.
- MEASURED on the 32-bit fixture: `V86C_INTS=3 ./build/v86c tests/test.img 20000000` prints the
  tools-on line and **zero** `INTS` lines — that fixture installs no interrupt source and takes no
  exception, so it is a quiet guest, not a silent instrument. The instrument's own proof is the
  arm, which drives the real seat in-process. The amd64 run is the orchestrator's next measurement.
