# x86-64 lane — HANDOFF (for a compacted / fresh session)

**Status: a real amd64 Debian Linux kernel boots on the port, prints its own banner, and runs ~30
seconds of its own C code into `setup_per_cpu_areas`.** The banner is a GATE step
(`gate-x86-64` 14/0). The lane's plan (S0–S13) is complete; what remains is a measured, one-link-at-
a-time chase inside the kernel's own boot, plus the reviews owed on the later slices.

Everything below is on `main` at the commit named; the tree is clean, the remote is pushed.

---

## 1. Where the guest is, exactly

```
$ ./build/v86c --bzimage tests/images/vmlinuz-6.1.0-50-amd64 6000000000 --memory 256 \
    --cmdline 'console=ttyS0 earlyprintk=serial' --serial /tmp/x.txt --long-mode on
serial: [0.000000] Linux version 6.1.0-50-amd64 ... Debian 6.1.176-1 (2026-07-02)
boot:   4488210693 instructions, cr3=0x30ea000, cr0=0x80050033, cs=0x10
stop:   a fault raised while delivering a fault ... (or the next named stop, see §4)
```

The kernel has: decompressed all 8 MiB, built its page tables, run `__startup_64` and its fixup,
taken a genuine `#PF`, **delivered it through its own IDT and repaired its own direct map**
(`PML4[273]`), reached `start_kernel`, and printed. It then runs ~30 s of boot and dies in
`setup_per_cpu_areas`' path.

## 2. The two-fold defect that ends the current run (MEASURED, `results/x86-64-producer.md`)

`page_offset_base = 0xffff8c16c0000000` is **CORRECT**; the defect is downstream and has two parts:

1. **Physical addresses truncated to 24 bits** — `0xc0e00000 → 0x00e00000`, `0xc0fd4000 → 0x00fd4000`.
2. **Those physical addresses are ~3.2 GB — outside the guest's 256 MiB**, i.e.
   `__alloc_bootmem_node_nopanic` returns RAM the machine does not have.

**Next two measurements, in this order** (do not skip to a fix):
- **The allocator's E820 view**: what the kernel reads as usable RAM (it believes ~3.2 GB). The
  firmware is `tests/minibios.asm`; a minibios change touches EVERY guest gate, so if the 32-bit
  transcript moves, STOP AND PUBLISH rather than re-pin.
- **The 24-bit truncation's site**: a width/mask bug (a 3-byte access?) or a halves artefact. Watch
  the store that produces the truncated value with `V86C_WATCH`, and read the register file at it.

## 3. The instruments (build on these; do not hand-roll probes)

All default-off; the shipped machine is byte-identical with them compiled in (**bench pin
200,095,214 measured WITH the toolkit present**). Documented in `docs/testing.md`'s instruments
section; the code is `src/cpu/debug.{c,h}`.

| variable | what it does |
| --- | --- |
| `V86C_WATCH=addr:len[,...]` | every WRITE, physical or virtual, with rip/width/value, unbuffered |
| `V86C_BREAK=addr:len[,...]`, `V86C_BREAK_MAX`, `V86C_BREAK_RING` | first EXECUTIONS in a range, with registers and gs_base/kernel_gs/fs_base, optionally the ring |
| `V86C_RING=N` | crash-surviving ring of the last N instruction boundaries, dumped on every fatal path |
| `V86C_DUMP_ON_STOP=1`, `V86C_CHAIN` | one artifact: registers, cr0-4, efer, idtr/gdtr, cr3, and the page-table chain |
| `V86C_PEEK`, `V86C_PEEK_SP` | read guest memory (and rsp-relative) at a break |
| `V86C_BOOTPARAMS` | dump the boot_params the firmware hands the kernel |
| `V86C_FAULT_DELIVERY_BOUND=N` | debug override of the fault-recursion bound |

**QEMU is the reference, and its gdbstub needs no `gdb` binary** (`-s` + a socket speaking the gdb
packets with checksums; memory reads go through the guest MMU so ask for VIRTUAL addresses). That is
how the eight-byte stack imbalance was found. `-d int` (NOT `-d cpu`, which wrote 23 GB in 3½ min)
logs exceptions with their frames.

## 4. The stop's history (so a regression is recognisable)

| stop | what it taught |
| --- | --- |
| `unimplemented: instr64_*` (a body) | implement it, add a corpus arm, re-point the gate's `stop-is-the-next-unimplemented-body` step |
| `a fault raised while delivering a fault` | the delivery path: check the frame (SS:RSP is ALWAYS pushed in IA-32e), the IDT stride (16 bytes), `lgdt`/`lidt` (a 64-bit base must not be masked), `iretq` |
| `long mode: ...` | a named unimplemented rule; the name says which |

## 5. The lane's plan: DONE, with the slices' reviews partly owed

S0 seam (inert) · S1 register file · S2 mode refusals (C5 closed) · S3 address widening · S4 host
corpus · S5 first instructions · S6 ALU+flags · S7 control flow/stack/far transfers · S8 mul/div/
shifts · S9 string/bit/atomic · S10 FPU/SIMD · S11 system layer (explicitly half: the walk,
canonicality, MSRs, TLB flags; SYSCALL/SYSRET/SWAPGS and the 64-bit GDT/TSS/IST were named and are
now largely covered by the guest's progress) · S12 the pinned amd64 guest · S13 the engines (the
64-bit differential is REAL: 85 cases × interpreter/uop/JIT, byte for byte).

**OWED: the `reviewer` passes on S10–S13 and on the ~20 debugging units since.** Every review in
this lane found something the corpus could not (a blocker in three of six), so this is real debt,
not ceremony.

## 6. Gates and pins (run all of them before reporting done)

`make` · `make selftest` · `make nasm` (118/118) · `make transcript-diff` (**byte-identical**) ·
`make determinism` · `make bench` (**200,095,214**) · `make nasm64` (13/0) ·
`sh tests/gate-x86-64.sh` (**14/0, the banner gated**) · `sh tests/gate-long-mode.sh` (57/0) ·
`sh tests/gate-long-mode-sys.sh` (5/0) · `sh tests/gate-x86-64-diff.sh` (13/0, 85 cases × 3
engines) · `sh tests/gate-reboot-1.sh` (39/39) · `gate-m2-5-guest`, `gate-m2-5-determinism`
(4,000,096,757 both runs) · `gate-pae-1` (11/0) · `gate-pae-l3` · the five VGA gates.

**A pin that moves is a finding, not a re-pin.** Every movement in this lane was declared with its
attribution (corpus counts when cases were added; the m2-5 count once, for PCI 00:12.0).

## 7. Traps this lane paid for (do not relearn)

1. **An instrument's silence is not evidence unless the instrument can speak.** Five instrument
   failures voided units: buffered stderr lost to a SIGSEGV; a probe whose output died with the
   process; `V86C_WATCH` blind to ALL long-mode stores (every 64-bit store goes through
   `safe_write*_addr()`); a QEMU `-d cpu` log that ate the disk; and a missing `PEEKing` of an
   rsp-relative slot.
2. **Do not run two processes on one session or one tree.** A resumed session collided with the
   still-running original and correctly reported `BLOCKED`; another read mtimes moving under it.
3. **The provider timeout kills long units** (`TimeoutError` / the earlier `reasoning_content` 400).
   Publish `stage=implement: PROGRESS` observations **before** each long run; write measurements to
   the report file as you go.
4. **Backticks inside an inline double-quoted shell string are command-substituted** — write briefs
   to a file and pass `$(cat file)`.
5. **A reviewer's finding is a hypothesis; so is a brief.** Several units began by refuting the
   previous unit (the "all-null IDT", "QEMU doesn't fault", "page_offset_base is wrong" stories were
   each corrected by measurement).
6. **A fix that lands while the tree is red is unverified**: one unit died mid-edit and left
   `m1c_selftest` and `gate-long-mode` failing; the fix was adjudicated against the SDM (IA-32e
   always pushes SS:RSP), not against the failing tests, and then the arms were updated with reasons.

## 8. Where to look

- Plan: `results/x86-64-plan.md` (14 slices, the oracle analysis, the risks).
- The debugging chain, in order: `results/x86-64-{s12,qemu,kaslr,bootparams,discriminator,memsize,
  bisect,startup64,reach,reach2,highpf,dmap,repair,retzero,nest,ssslot,branch,va,kgs,f7,recursion,
  halves,producer}.md` — each names its own next measurement.
- Memory: `handoff-v86c-x86-64-*` nodes (ctx, plan, s0/s1/s2/s3/s4/s5/…/s13 results, reviews, and
  one result node per debugging unit).
- The kernel pin and the gate: `tests/gate-x86-64.sh` (provenance, the banner step, the QEMU leg).
- TODO.md §8: long mode as recorded, now largely SHIPPED; §4 for the JIT notes; §3 for portability.

## 9. One-paragraph summary for the next session

The x86-64 lane is done as a plan and is now a debugging tail inside a real amd64 kernel: it boots,
prints, and dies ~30 s in on a bootmem allocator that believes the machine has ~3.2 GB while the
guest has 256 MiB, with physical addresses truncated to 24 bits. Two measurements are named in
`results/x86-64-producer.md` §4. Use the toolkit (`src/cpu/debug.c`) and QEMU's gdbstub; never trust
an instrument's silence; publish progress observations before long runs; and treat any pin movement
as a finding to declare.
