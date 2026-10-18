# KVM-PLAN — the KVM backend as a second machine on the v86 C port

**Unit:** `HANDOFF=v86c-kvm`, unit **KVM-PLAN** (planner, plan-only). **Inputs:** memory node
`handoff-v86c-kvm-plan` (rev 19, the design of record) and `handoff-v86c-kvm-artifact-1` (rev 3,
the load-bearing probe snippets); provenance and the throwaway probe sources are named at the
end of this file. This file is a **transcription** of that approved plan, organized into the
tree's plan style: nothing is summarised, and every number, ioctl name, `file:line` reference,
size estimate and caveat is carried exactly as the source has it.

Claims are tagged the way the source tags them: **MEASURED** (run on this host, 2026-10-18) and
**PROJECTION** (the planner's estimate, not a measurement). Those are the only two tags the
source uses, and this transcription adds none — "MEASURED-BOUNDED" (§6) is one of the plan's
three determinism *classes*, not a further claim tag, and where a statement carries no tag the
source gave it none. Design *decisions* in this plan are stated as decisions, not as findings.

---

## 0. Bottom line

A KVM backend for `~/futaie/souche` is a **SECOND MACHINE with its own contract**, not a third
engine beside the interpreter/uop-cache/JIT. The port's CPU (`src/cpu/cpu.c`, `uop_cache.c`,
`jit_codegen.c`) is **REPLACED** by a KVM vCPU on the host; everything else is **SHARED and
unchanged** — the memory model (mem8 + the device hook table), the whole device layer
(`src/hw/*`), the PIC/LAPIC/IOAPIC (`src/cpu/pic.c`, `apic.c`, `ioapic.c`), the boot paths
(kernel.c/minibios), the two output documents (the M0 transcript, the nasm fixture), the run
driver (`src/main.c:455` `run()`).

The one-line contract of the lane: **the hardware is the semantic authority and this tree's
emulated legs are the oracle for the lane's OWN glue** (state translation, exit dispatch, memory
carving, interrupt injection, CPUID model) — never the other way round. The lane therefore
**cannot strengthen any claim about instruction semantics**; it buys **THROUGHPUT and HEADROOM**
(MEASURED: 8418.8 M guest instructions/s on a fused dec/jnz loop vs the interpreter's ~50 M/s
from results/perf-1 — about 170x on pure compute) and it makes a 64-bit guest nearly free,
which TODO.md section 8 says everything else about this tree forbids. **It must never become the
definition of correct.**

The mechanics that make it feasible are measured, not argued: `KVM_INTERRUPT` instead of
`KVM_IRQ_LINE` (the in-kernel irqchip is never created, the port's PIC stays the machine), a
page-aligned `mem8` memslot with the VGA window carved as a hole, the port's own `io_portmap_*`
and `read*/write*` as the only device path, and a one-printer canonical state document so the
KVM leg is judged by the port's own transcript.

---

## 1. The framing — a second machine, not a third engine

A KVM backend for `~/futaie/souche`: a **SECOND MACHINE** with its own contract, not a third
engine beside the interpreter/uop-cache/JIT. The port's CPU (`src/cpu/cpu.c`, `uop_cache.c`,
`jit_codegen.c`) is **REPLACED** by a KVM vCPU on the host. Everything else is **SHARED** and
unchanged: the memory model (mem8 + the device hook table), the whole device layer
(`src/hw/*`), the PIC/LAPIC/IOAPIC (`src/cpu/pic.c`, `apic.c`, `ioapic.c`), the boot paths
(kernel.c/minibios), the two output documents (the M0 transcript, the nasm fixture), the run
driver (`src/main.c:455` `run()`).

**The one-line contract of the lane:** the hardware is the semantic authority and this tree's
emulated legs are the oracle for the lane's OWN glue (state translation, exit dispatch, memory
carving, interrupt injection, CPUID model) — never the other way round. The lane therefore
cannot strengthen any claim about instruction semantics; it buys THROUGHPUT and HEADROOM
(MEASURED: 8418.8 M guest instructions/s on a fused dec/jnz loop vs the interpreter's ~50 M/s
from results/perf-1 — about 170x on pure compute) and it makes a 64-bit guest nearly free,
which TODO.md section 8 says everything else about this tree forbids. It must never become the
definition of correct.

---

## 2. MEASURED TODAY — the bring-up facts (2026-10-18, this host)

MEASURED TODAY (2026-10-18, this host, `/tmp/kvmprobe/probe2.c` .. `probe6.c`, gcc, throwaway —
not deliverables). Host: AMD Ryzen 9 3950X, kernel 7.1.8-1-default, `/dev/kvm` API 12,
`vcpu_mmap_size` 12288, `KVM_CAP_NR_VCPUS` 32. Present caps: `SET_GUEST_DEBUG(23)=1`,
`TSC_CONTROL(60)=1`, `ADJUST_CLOCK(39)`, `X86_DISABLE_EXITS(143)=31`, `X86_USER_SPACE_MSR(188)=1`,
`X86_MSR_FILTER(189)=1`, `IMMEDIATE_EXIT(136)=1`, `HALT_POLL(182)=1`,
`X86_TRIPLE_FAULT_EVENT(218)=1`, `IRQCHIP(0)=1`.

**(a) CPUID PARITY IS MEASURABLE AND NEARLY EXACT.** `KVM_SET_CPUID2` accepts v86's model
(`src/cpu/instructions_0f.c:1118` `instr_0FA2`, transcribed) and `KVM_GET_CPUID2` returns it
with **ZERO changed words and ZERO added leaves** — for **13 of the 14 leaves**. The exception is
leaf `0x80000008`: every value carrying a nonzero linear-address-width byte (bits 15:8) is
refused `EINVAL`; `EAX=0x20` (phys only) is accepted. So v86's `0x00002020` **MUST NOT** be
presented.

**(b) `KVM_IRQ_LINE` DOES NOT EXIST WITHOUT AN IN-KERNEL IRQCHIP:** with no
`KVM_CREATE_IRQCHIP`, `KVM_IRQ_LINE` returns `ENXIO` (errno 6), measured twice.
`KVM_INTERRUPT(0x20)` returns 0 and the vector **IS TAKEN**: a guest at `sti;hlt` with
`IDT[0x20]` -> `0x200` halted again at `rip 0x201`. This is the interrupt primitive, and it is
what keeps the port's PIC the machine.

**(c) SINGLE STEP WORKS AND IS ONE EXIT PER INSTRUCTION:** `KVM_SET_GUEST_DEBUG` with
`KVM_GUESTDBG_ENABLE|KVM_GUESTDBG_SINGLESTEP` gives exit `KVM_EXIT_DEBUG(4)` per instruction
with regs updated; a `hlt` is an HLT exit. **ORDER MATTERS:** `KVM_SET_REGS` must be applied
**BEFORE** the guest_debug control, or the step bit is silently lost (measured: 7 instructions
ran in one `KVM_RUN`; with the order fixed, 1 per run). Throughput **205,827 steps/s = 4.858
us/instruction** including a `KVM_GET_REGS` per step.

**(d) THE MEMORY REGION MUST BE PAGE-ALIGNED:** `KVM_SET_USER_MEMORY_REGION` with
`userspace_addr` unaligned returns `EINVAL`. A carved hole works: slots `[0,0xA0000)` and
`[0xC0000,RAM)` give `KVM_EXIT_MMIO` with phys/len/is_write/data (len 4 read, len 1 write
measured), and a read is answered by writing `run->mmio.data` then re-entering.

**(e) AN INSTRUCTION FETCH FROM A CARVED HOLE IS NOT AN MMIO EXIT:** it gave
`KVM_EXIT_INTERNAL_ERROR(17)` and left the vCPU unusable (measured twice, once with no CPUID
model installed — re-check with the model before relying on it).

**(f) MSRs ARE NOT INTERCEPTED BY DEFAULT:** a guest `wrmsr` to `IA32_APIC_BASE` (0x1B) ran with
exit HLT (no exit) and `KVM_GET_MSRS` read back the written value. The port's MSR model is
bypassed entirely. The fix seat exists on this host (`X86_MSR_FILTER=1`,
`X86_USER_SPACE_MSR=1`).

**(g) TSC:** `KVM_SET_TSC_KHZ`(1 GHz) is **REFUSED** (`EINVAL`) both before and after vCPU
creation; the host rate is **3,499,998 kHz**. Writing `IA32_TSC` via `KVM_SET_MSRS` **DOES** drive
the guest: `rdtsc` read back written+13,720 .. +15,120 counts for writes at 2^32, 3*2^32,
4*2^32 (i.e. the guest saw the written value plus the host time of the run window). So the TSC
can be anchored at every exit but cannot be stopped between exits.

**(h) A HOST TIMER BREAKS `KVM_RUN`:** SIGALRM (`setitimer`, 2 ms) made `KVM_RUN` return -1
`EINTR` with `KVM_EXIT_INTR(10)` after 0.002 s. This is the mechanism that lets the port's own
PIT/RTC/PM-timer be serviced.

**(i)** `KVM_EXIT_IO` carries port/size/direction/count; `rep outsb` produced `count=1` on this
host (KVM may coalesce `rep` — handle `count>1` anyway).

**(j) cosmocc SHIPS NO `linux/kvm.h`:** `/home/jaye/.local/cosmo/include/linux/` holds only
futex.h, limits.h, param.h, types.h. The ABI must be vendored.

**(k)** The M0 oracle image executes **561 instructions** (`results/transcript.txt:76`), not a
million — so a stepped KVM leg over the whole M0 corpus costs **~2.7 ms**.

---

## 3. Three corrections to the brief (reported, not reconciled silently)

**C1 — THE BRIEF'S Q3 MECHANISM IS WRONG, AND MECHANICALLY SO.** It says interrupts must be
injected through `KVM_IRQ_LINE` so the port's own PIC/PIT/IOAPIC remain the machine. Measured:
`KVM_IRQ_LINE` is the interface TO an in-kernel irqchip; with no `KVM_CREATE_IRQCHIP` it returns
`ENXIO`. The primitive that keeps the port's PIC the machine is `KVM_INTERRUPT` (a vector handed
to the vCPU), and the bridge is: the port's own PIC acknowledges -> the vector ->
`KVM_INTERRUPT`. Injecting via `KVM_INTERRUPT` is exactly the QEMU-with-userspace-PIC
arrangement, so the brief's INTENT is right and only its ioctl is wrong.

**C2 — THE CPUID QUESTION IS ALREADY ANSWERED BY MEASUREMENT**, so the plan does not argue
whether machine parity via CPUID is sound: 13 leaves round-trip exactly and one is refused. What
remains is a design choice (which leaves to present) and a declared-diff instrument, not a
soundness debate.

**C3 — THE RUNG EXPECTATION CONFLICTS WITH THE HOUSE RULE.** The brief expects anything
touching state translation or exit handling to START at engineer. `~/.config/hax/AGENTS.md`
(user-stated 2026-10-11) says every unit starts at coder and a rung is never skipped because a
later one might be stronger. This plan follows the house rule: every KVM unit starts at coder
and escalates on a demonstrated failure. The safety that a higher starting rung would have
bought is bought instead by the T1 differential (the lane's own step-by-step state comparison),
which finds translation errors mechanically — this is precisely the class where a cheap
implementer plus a sharp gate works and a sharp implementer plus no gate does not.

---

## 4. Q1 — THE VERIFICATION CONTRACT (T1, T2, T3)

Three tiers, each with what it CANNOT catch. The lane's own risk surface (state translation,
exit dispatch, memory carving, injection, CPUID) is what they are aimed at; **none of them
verifies instruction semantics, and none may be presented as if it did.**

**T1 INSTRUCTION-BOUNDARY STATE PARITY (exact; stepped).** Same image, same initial state, both
legs; the KVM leg single-stepped so it has an exact instruction count; compare the canonical
state document at the stop and, in a window, at every step. **CATCHES:** every
state-translation field error in either direction, every exit-handling error, the FPU tag-word
translation, the instruction-count off-by-one, a segment/CR/EFLAGS field dropped. **CANNOT
CATCH:** anything the image does not exercise; anything about instruction semantics (the
hardware is the authority, so a T1 pass on hardware says nothing about v86's fidelity — that is
the M0/nasm gates' job, already owned); and it costs 4.858 us/instruction, so **its DOMAIN IS
SMALL IMAGES**: the M0 image (561 instructions, 2.7 ms), the nasm corpus (118 tiny images), and
WINDOWS of bigger guests.

**T2 EVENT-TRACE PARITY (full speed).** The ordered trace of guest->device interactions (port,
size, direction, value; MMIO addr, size, direction, value) and the guest-visible outputs (serial
bytes, the VGA document, disk writes, and the terminal state). Compared as **SEQUENCES, never as
timelines** — because the interrupt arrival points DIFFER between legs and always will.
**CATCHES:** the device seam, an exit not dispatched, a wrong width, the carved-hole geometry, a
missing hook, a device that stops working. **CANNOT CATCH:** any divergence that does not change
an observable device interaction (pure compute that ends elsewhere is invisible here — it is
T1/T3's business); and the trace is enormous unless filtered, so its filter (which
ports/addresses) is part of the gate's contract.

**T3 TERMINAL-STATE PARITY WITH A MEASURED DIVERGENCE BOUND (full speed, real guests).** For the
kernel/baliveau guests: the terminal RIP, the guest's own serial/virtio output and the pinned
disk md5 must reproduce; run N times and MEASURE the divergence. The tree already has the shape
and the evidence: `results/m2-5-baliveau-kernel.md` section 5 — the terminal eip reproduces
byte-exactly across runs and hosts, the instruction count does not. **CATCHES:** a boot that does
not happen, a device that dies, a translation error that sends the guest elsewhere. **CANNOT
CATCH:** silent-wrong inside the instruction path, and any interrupt-timing property.

**VERDICT ON THE BRIEF'S CANDIDATE (CPUID + checkpoint state documents):** sound as a
PRECONDITION, unsound as the primary instrument. A checkpoint taken at a device event cannot be
compared state-for-state across legs, because the guest's interrupt timeline differs and the
state at the Nth port write is then genuinely different. That candidate therefore DEGRADES into
T2 (compare the event and its data, not the whole state). The whole state may only be compared
where the same instruction path led to it — which is T1, and T1 is exact. **NO NEW REFERENCE IS
NEEDED:** for instruction semantics real hardware is the reference (and `make nasm` already
validates the port against it), and for the lane's glue the emulated legs are the reference. Two
declared limits that T1/T2 cannot paper over are listed under risks: a fetch from the carved VGA
hole, and MSR semantics.

---

## 5. Q1b — THE INSTRUMENTS TO BUILD

This is the concrete work behind T1-T3, and it is most of S0/S1.

**I1 THE CANONICAL STATE DOCUMENT FROM ONE PRINTER FOR BOTH LEGS.** The KVM leg must fill
`cpu_state_t` from `KVM_GET_REGS`/`GET_SREGS`/`GET_FPU`/`GET_MSRS` at every stop and then reuse
`src/main.c:486` `print_transcript` (or a checkpoint variant of it) **UNCHANGED** — one document
format, one comparison, and the printer itself is exercised by both legs. This is the single
most load-bearing design decision in the plan: it means the KVM leg is judged by the port's own
document, not by a second format that could flatter it.

**I2 STEP MODE ON BOTH LEGS.** KVM: `--kvm-step` (guest_debug single-step; regs applied before
the control, measured trap (c)). Emulated: a one-instruction-per-call mode, smallest change in
`src/cpu/cpu.c`'s `interpreted_run`: after the existing break checks add
`if (cpu_step_mode) break;` so the tail's counter update adds exactly 1; it must force
`--uop-cache off --jit off` and say so on stderr if the user asked for them. **ASYMMETRY
SELF-CHECK:** the emulated leg in step mode must produce the SAME transcript as without it — if
it does not, the step plumbing is wrong before KVM is in the picture at all.

**I3 THE WINDOWED STEPDIFF.** Run normally to a trigger, then single-step N instructions
comparing canonical state per step. Triggers: the Nth exit of a chosen kind (a port number / an
MMIO address), or "from the start" for the small corpus. Cost 4.858 us/step => 100k-instruction
windows are 0.5 s. This is the instrument that makes T1 affordable on real guests.

**I4 THE EVENT TRACE** (`--kvm-trace FILE`, and the same switch on the emulated legs so the
traces are comparable): one line per guest->device interaction, filtered by the gate. It is
cheap, it is the only full-speed instrument, and it is what T2 compares.

**I5 THE DIVERGENCE MEASURE (T3):** the gate runs the guest N times and records the per-field
divergence (terminal RIP, byte counts, the guest's own `/proc/interrupts` counters). This is a
**NUMBER IN THE GATE**, not a threshold nobody states — see the decision rule in Q2.

---

## 6. Q2 — THE TIME CONTRACT

**THE LANE'S CLOCK IS THE HOST MONOTONIC CLOCK**, and that is a deliberate, declared choice, not
a fallback.

**WHY IT IS THE RIGHT ONE:** upstream v86's own microtick IS the host wall clock
(`main.js:223-240` — `src/cpu/exec_boundary.c:51-58` records this), and D-F2 (1 guest
instruction = 1 microsecond of virtual time) is the PORT's deviation from it, adopted only to
make the port's own gates deterministic. Under KVM the instruction counter does not exist, so
D-F2 has no carrier: adopting the host clock makes the lane MORE faithful to the pinned
reference than the emulated lane is, and it is the only base under which an idle guest's time
advances (an exit-count clock would freeze a guest that never exits — measured hazard: a dec/jnz
loop ran 200M instructions with ZERO exits).

**IMPLEMENTATION:** `microtick()` (`src/cpu/exec_boundary.c:87`) becomes one indirection —
`microtick` returns `clock_now()`, where the shipped default is the existing counter/1000.0 and
the KVM lane installs `CLOCK_MONOTONIC` ms since run start. The flag-off machine is
byte-identical (the default function pointer is the same arithmetic). **NOTHING ELSE IN THE PORT
CHANGES:** pit.c, rtc.c, acpi.c, apic.c all consume `microtick()` and get the lane's clock for
free.

**D-F2 AND EVERY PIN ARE REFUSED BY NAME, NOT INHERITED:** `cpu->instruction_counter` stays 0 in
the lane; the bench pin (200,095,214), the m2-5 determinism count (4,000,096,757) and
`make determinism`'s byte-for-byte contract do not apply. Refusal must be explicit:
`tests/perf_bench.sh` and the determinism gate refuse `--kvm` with a named message, and no KVM
gate may assert a byte-identical transcript for a guest that reads the host clock (that is the
reword-downward failure this tree forbids — the honest seat is a gate that states what it does
assert).

**WHAT DETERMINISM THE LANE CAN CLAIM**, in three parts, each with its evidence: **(1) EXACT**
for the T1 corpus — images with no devices, no interrupts and no host-time reads reproduce the
transcript byte-for-byte INCLUDING the instruction count, because there is nothing
nondeterministic in them but the exit cadence, which the step count makes canonical (the M0
machine registers no devices: `src/hw/machine.c`'s `run_hardware_timers` has nothing to poll).
**(2) MEASURED-BOUNDED** for real guests — terminal state, guest output and device lines
reproduce; the timing does not; the bound is measured per image by the gate itself (T3).
**(3) NONE** for any guest that reads RDTSC twice inside one `KVM_RUN` window: RDTSC is
**UNTRAPPABLE** through KVM's ABI (no exit reason exists for it; the only TSC controls are rate,
offset and kvmclock), so the lane cannot make a host-time-reading guest deterministic, and must
say so rather than pick a run mode that hides it.

**THE DECISION RULE** (so a gate cannot read noise as a direction): T3 gates assert "terminal
RIP identical in N/N runs" and "the listed guest lines present in N/N runs"; anything weaker is
**INSUFFICIENT** and says so. Thresholds over interrupt counts are forbidden: the measured class
(`results/m2-5-baliveau-kernel.md` section 5, `/proc/interrupts` timer 578 vs 875 between two
legs) is a real, expected divergence, and a tolerance on it would be a number nobody can defend.

---

## 7. Q2b — THE TWO TIMING MECHANISMS (both MEASURED, both load-bearing)

**M1 THE TSC.**

(a) `KVM_SET_TSC_KHZ` cannot be used to declare 1 GHz — this kernel refuses it (`EINVAL`) at any
point, before or after vCPU creation, and the host rate is 3.5 GHz. Therefore v86's CPUID leaves
`0x15` and `0x16`, which declare a 1 GHz TSC, would LIE to the guest. **DECISION:** the lane
presents v86's model MINUS leaves `0x15`, `0x16` and `0x80000008` — a declared three-leaf diff,
recorded in the header and enforced by the CPUID parity instrument (S0's gate), with the reason
stated per leaf (`0x15`/`0x16`: the host's TSC rate is not settable on this host, so a declared
1 GHz would be a lie the guest can measure; `0x80000008`: KVM refuses a linear-width byte,
MEASURED). A Linux guest then calibrates its TSC against the PIT/PM timer, which are the port's
own devices and therefore trustworthy, and gets ~the host rate — consistent, not a lie.

(b) `KVM_SET_MSRS(IA32_TSC=v)` DOES set the guest's TSC (measured: rdtsc = written +
13,720..15,120 counts, i.e. the host time of the window). **DECISION:** pin the TSC origin to 0
at run start (deterministic origin, everything a delta-measuring guest calibrates becomes more
reproducible run to run — and that improvement is measurable, so S1 makes it an asymmetry probe:
the same guest with and without the origin pin, N runs each, divergence compared).

(c) Between exits the TSC runs at host rate and is NOT controllable — stated, not hidden.

**M2 THE HOST TICK.** The port's PIT/RTC/PM-timer/APIC-timer can only be serviced when userspace
holds control, and a guest with no exits (measured: a dec/jnz loop over 200M instructions) would
starve them: a kernel would run with stretched ticks and never notice, which is the silent-wrong
class. **THE MECHANISM IS MEASURED TO WORK:** a SIGALRM (`setitimer`) makes `KVM_RUN` return -1
`EINTR` with `KVM_EXIT_INTR` after the interval. **DESIGN:** at every exit run the port's own
`run_hardware_timers(microtick())` — which already RETURNS the next device deadline
(`src/hw/machine.c:253`, the value upstream's main.js uses for its `setTimeout`) — then arm the
host timer for that deadline, floored at a MAXIMUM PERIOD (default 1 ms) so a compute-bound
guest can never be starved. This uses an existing seam for its intended purpose; it is the one
place where the lane's own code must be careful with signals (no `SA_RESTART` games, no handlers
that touch guest memory).

---

## 8. Q3 — THE DEVICE COUPLING (B1-B5)

The QEMU split holds: every device stays in userspace, the in-kernel irqchip is **NEVER**
created, and the lane reuses the port's two existing bridges instead of growing a second device
path. That reuse is the whole feasibility argument and it is also the main defence against
silent-wrong (a second path would drift).

**B1 THE PORT-I/O BRIDGE.** A `KVM_EXIT_IO` dispatch calls the port's OWN entry points —
`io_portmap_read8/16/32` and `io_portmap_write8/16/32` (`src/hw/io.c:413/448` and siblings) —
which already intercept the PIC's six ports in front of the port map. So the guest's `out`/`in`
and the device's handlers see exactly one code path. Must handle `run->io.count` (rep I/O may
coalesce; measured `count=1` on this host for `rep outsb`, but the ABI allows more) by looping,
and must answer IN reads by writing the run struct's `io.data` region.

**B2 THE MMIO BRIDGE.** A `KVM_EXIT_MMIO` dispatch calls the port's OWN router —
`read8/read16/read32s/write8/write16/write32` (`src/cpu/memory.c:389/437/518`) — so width
composition (`read16 = read8|read8<<8`, memory.c's own rule), the VGA LFB arm, the APIC/IOAPIC
arms and the unclaimed-memory fallback all come for free and cannot diverge. A read is answered
by writing `run->mmio.data` then re-entering. A 16-byte access is expected to arrive as two
8-byte exits (the ABI's `data[8]`); the VGA's `write128` hook must therefore be tolerant — to be
MEASURED in S2, not assumed.

**B3 THE INTERRUPT BRIDGE.** `device_raise_irq` (`src/hw/machine.c:294`) -> `handle_irqs`
(`src/cpu/pic.c:437`) -> `pic_acknowledge_irq` (`pic.c:318`) -> `pic_call_irq` (`pic.c:423`) ->
`call_interrupt_vector(...)`. **THE SEAM:** `pic_call_irq`'s last step becomes a module-level
delivery sink — a function pointer in `exceptions.h` whose DEFAULT is
`call_interrupt_vector(vector,false,false,0)` (the emulated lane, byte-identical) and which the
KVM lane sets to "`KVM_INTERRUPT(vector)`". Three lines in pic.c, one declaration, no change to
any device and no change on the instruction path. The port's PIC still owns the ISR/mask/priority
state and the guest's EOI still goes through port writes, so the port's PIC really is the
machine; KVM only carries the vector. The vCPU must have IF set for delivery — if it does not,
KVM queues it, which is hardware behaviour; and `cpu->in_hlt`'s mirror is irrelevant (KVM's own
halt state governs, and an injected vector wakes it).

**B4 THE MEMORY GEOMETRY.** The memslot MUST be `mem8` itself, not a copy: devices write guest
memory directly (`dma.c`'s `write_blob`, ide.c, vga.c's DMA-visible paths), and
`architecture.md` section 3's one-memory-model claim is exactly the property that makes this
work. Two consequences, both concrete:

- **(i)** `ALLOCATE_MEMORY` MUST PRODUCE A PAGE-ALIGNED `mem8` (measured: unaligned
  `userspace_addr` is refused `EINVAL`) — `src/cpu/memory.c:25` `allocate_memory` switches to an
  aligned allocation; that is behaviour-neutral for the emulated lanes (a stronger alignment)
  and must be gated by `make transcript-diff` + `make determinism`;
- **(ii)** the VGA window `0xA0000-0xBFFFF` is a **HOLE** between two slots (`[0,0xA0000)` and
  `[0xC0000,memory_size)`), because the port's `in_mapped_range` (`memory.h`) deliberately routes
  that window to the device while the rest of RAM is plain. The 8 MiB LFB at `0xE0000000`, the
  LAPIC page and the IOAPIC page are above `memory_size` and therefore unmapped holes already.

**B5 THE DECLARED CONSEQUENCE OF B4(ii):** a guest that FETCHES from the carved window no longer
executes mem8's bytes (which is what the emulated legs do) — it stops. Measured: a fetch from a
carved hole produced `KVM_EXIT_INTERNAL_ERROR`, not an MMIO exit. The lane must turn that into
the port's own named stop (the `get_phys_eip` message class, `cpu.c:113-130`) and record it in
the lane's difference list. Narrow, loud, and honest — and re-measure it with the v86 CPUID
model installed before writing the message.

---

## 9. Q3b — THE DEVICE-BY-DEVICE EXIT MAP

Every device in `src/hw/machine.c`'s set, plus the two the CPU owns. "IO" = `KVM_EXIT_IO` via
`io_portmap_*`; "MMIO" = `KVM_EXIT_MMIO` via `read*`/`write*`; "IRQ" = the vector reaches
`KVM_INTERRUPT` through B3; "internal" = no exit of its own.

- **pic.c (8259 pair)** INTERNAL — its six ports and the two ELCR ports are intercepted in front
  of the port map by `io_portmap_*` (already), so they are reached by B1; its output is the
  delivery sink (B3). No `KVM_CREATE_IRQCHIP`, ever.
- **pit.c (8254)** IO `0x40-0x43`; IRQ0; needs M2's host tick to fire at its deadline.
- **rtc.c CMOS** IO `0x70/0x71`; IRQ8 (slave); the CMOS clock reads `microtick()`-based virtual
  time, which under the lane is the host clock — the RTC therefore ticks realistically, and its
  guest-visible time is NOT reproducible (a T3-class divergence, already named).
- **dma.c 8237** IO `0x00-0x0F`, `0x80-0x8F`, `0xC0-0xDF`; its transfers write guest memory
  DIRECTLY (`write_blob`) — coherent because the memslot is mem8 (B4).
- **ps2.c 8042** IO `0x60/0x64`; IRQ1/IRQ12; host input arrives through `machine_host_bus` as
  today.
- **uart.c COM1** IO `0x3F8-0x3FF`; IRQ4; the host sink (file/stdout) is the same one the other
  lanes write.
- **ide.c** IO `0x1F0-0x1F7`, `0x3F6`, `0x170-0x177`, `0x376`; IRQ14/15; its disk buffer is a
  host allocation, not guest memory.
- **pci.c** IO `0xCF8-0xCFF` (config address/data); INTx routing to the IOAPIC/PIRQ path
  (`device_raise_irq`); its `set_io_bars` MOVES port-map entries — coherent because it is the
  same object graph. Its BARs are MMIO ranges: an unmapped BAR address becomes an MMIO exit and
  reaches the owning device through `read*`/`write*`.
- **vga.c** IO `0x3C0-0x3DF` (attribute, sequencer, graphics, CRTC, status, DAC, misc); MMIO the
  `0xA0000` window (the carved hole, B4) AND the 8 MiB LFB at `0xE0000000` (unmapped already).
  The most demanding exit map in the machine, and the one with an existing strong instrument:
  `--vga-doc`'s document lets T2 compare frames, not bytes.
- **acpi.c** PM1 status/enable/control + the PM timer, IO; IRQ9; the PM timer is a microtick
  consumer.
- **apic.c (LAPIC)** MMIO `0xFEE00000` page (above RAM, unmapped, reaches `apic_read32` /
  `apic_write32` through `read*`/`write*`); its timer is a microtick consumer; its delivery is
  B3. `cpu->apic_enabled` (`IA32_APIC_BASE`'s EN bit) is the MSR seam's business, not the MMIO
  path's.
- **ioapic.c** MMIO `0xFEC00000` page; `device_raise_irq`'s pin routing (machine.c's
  `IOAPIC_PIN_PIT=2` quirk) is unchanged.
- **virtio_console.c** IO + PCI BAR MMIO; IRQ via PCI INTx; its virtqueue rings are guest memory
  reached only through the port's own accessors — note for S2/S4: the BAR MMIO range must be a
  hole (it is above RAM, so it already is).
- **kernel.c / tests/minibios.asm** NOT devices: the boot path, unchanged; the firmware is
  real-mode and runs natively (brief's measurement, not re-proved).
- **bus.c** HOST-SIDE, no guest view.

---

## 10. Q3c — THE MSR SEAM

Measured, and it is a silent-wrong class, not a nicety. Under KVM the guest's RDMSR/WRMSR go to
the host CPU and KVM's MSR model. **MEASURED:** a guest `wrmsr` to `IA32_APIC_BASE` (0x1B)
produced NO exit, and the value was readable back through `KVM_GET_MSRS`. The port's own MSR
table (`src/cpu/instructions_0f.c:6279` `instr_0F30` and `:6374` `instr_0F32` — EFER,
SYSENTER_CS/EIP/ESP, `IA32_APIC_BASE`, `IA32_TIME_STAMP_COUNTER`, `IA32_PAT`, `MISC_ENABLE`, and
the reference's "unknown MSR is silently ignored, rdmsr answers 0" rule) is then **BYPASSED**.
Concretely: the reference answers 0 for an unknown MSR and real hardware raises #GP — a guest
that PROBES MSRs takes a different code path on the two machines, quietly.

**THE SEAT IS AVAILABLE ON THIS HOST** (`X86_MSR_FILTER=1`): `KVM_SET_MSR_FILTER` with the port's
modelled MSRs marked for exit gives `KVM_EXIT_X86_RDMSR/WRMSR`, which the lane answers from the
port's table. **DECISION:** this is in scope, as its own unit inside S3, because a kernel guest
does read a handful of MSRs during boot and the divergence would be silent. The unit's contract:
the port's table is the authority for the MSRs it models; everything else forwards to KVM; a
gate reads the same MSR list on both legs and compares (the emulated leg takes the values from
the port's rdmsr, the lane from the exit). If the filter turns out to be unavailable on a host,
the lane must REFUSE that host by name rather than run with a silently different MSR machine.

---

## 11. Q4 — WHICH GUESTS, IN WHAT ORDER, AND WHY THIS ORDER

Slice order follows instrument strength, not guest glamour: the smallest image that can carry the
exact instrument first, the biggest guest last.

1. **tests/test.img** — the M0 corpus image. 561 instructions, NO devices on the M0 machine, no
   interrupts, no paging, no RDTSC, and an existing byte-pinned transcript. It is the only guest
   in the tree for which the lane can claim EXACT parity including the instruction count (T1).
   Everything the lane must get right about state translation is exercised by it except
   segments' unusual bits, FPU corner states and devices — those are the nasm corpus's job next.
2. **The nasm differential corpus (118 images).** Each is tiny and already compared against
   NATIVE hardware (`tests/mkfixture.c`, a ptrace harness) — so under KVM the lane should match
   the native leg, and its skip list must be a SUBSET of `tests/nasm_skip_c.txt` (v86's own
   divergences from hardware vanish when the hardware is the machine). Each remaining skip needs
   a measured reason. This is the cheapest broad state-translation oracle in the tree.
3. **The device fixtures:** `tests/vga-mode13.img` / `vga-planar0d.img` / `vga-lfb.img` / the
   reboot fixtures — they exercise B1/B2/B3 and the VGA window carve, and they have a
   frame-level comparison instrument (`--vga-doc`) plus pinned transcripts.
4. **The kernel:** `vmlinuz-i386-v86guest` + `guest-initramfs.img` + the 64 MiB disk
   (`tests/gate-m2-5-guest.sh`'s own artifacts). Here T3 replaces T1: terminal RIP + the 14
   guest lines + the pinned disk md5 + a measured divergence. The tree already has an
   independent sanity check for this exact guest (`tests/gate-m2-5.sh` gate 1b runs qemu+KVM and
   records the same lines) — the lane's gate should be able to state that its own output matches
   those recorded lines.
5. **64-bit: NOT SCOPED IN** (the user has not decided; TODO.md section 8 stands). The plan
   records the argument the lane adds: KVM makes the CODE nearly free (the host CPU is already
   64-bit; the work is state translation for R8-R15, the long-mode segment model and 4-level
   paging) and it also changes the VERIFICATION story that §8 calls the real cost — a 64-bit
   guest under KVM needs no new instruction semantics and no new reference for semantics, only
   the lane's own T1/T2 glue instruments. That makes long mode the first thing in this tree that
   KVM makes cheaper rather than harder. It still needs the user's decision.

---

## 12. Q5 — THE FLAG, THE DETECTION AND THE REFUSALS

Implementable as written.

**FLAG:** `--kvm` (default OFF; no default flip, ever, without the user). Companions:
`--kvm-step`, `--kvm-state FILE` (the canonical checkpoint document), `--kvm-trace FILE` (the
event trace), `--kvm-window N` (the stepdiff window length). All are parsed in BOTH parsers
(`main.c:1324` `main()`'s loop and `main.c:955` `boot_main`'s loop), and the transcript mode's
usage line (`main.c:1361`) is extended.

**DETECTION**, in this order, each failing with its OWN message naming what is missing (never a
silent fallback to another engine):

1. compile-time: `#if defined(__x86_64__)`. Else: "v86c: --kvm: this build's x86-64 slice is
   required; the aarch64 slice has no KVM backend (the v86 machine is an x86 machine)." The APE
   must still BUILD for aarch64 — the vendored ABI header is plain structs, no arch intrinsics.
2. `open("/dev/kvm", O_RDWR) < 0`: "v86c: --kvm: /dev/kvm is not openable (<errno text>); KVM
   mode needs the kvm kernel module and permission on /dev/kvm."
3. `ioctl(KVM_GET_API_VERSION) != 12`: "v86c: --kvm: /dev/kvm reports API version N, this
   backend implements 12."
4. `KVM_CREATE_VM < 0`: "v86c: --kvm: KVM_CREATE_VM failed (<errno text>)" (this is the real "is
   virtualization available to me" test; parsing `/proc/cpuinfo`'s svm flag is redundant and is
   NOT part of the sequence — CREATE_VM is the honest probe).
5. `KVM_GET_SUPPORTED_CPUID` (or `SET_CPUID2` with the lane's model) < 0: "v86c: --kvm: this
   host's KVM refused the machine's CPUID model (<errno>); leaves X, Y are the declared diff."
   (The model's usability is a PRECONDITION: the lane refuses rather than runs with a silently
   different model.)
6. The MSR filter (S3) unavailable: refuse, naming `X86_MSR_FILTER` (see Q3c).

**FLAG INTERACTIONS**, all explicit: `--kvm --jit on` / `--uop-cache on` given TOGETHER is a
refusal ("--kvm replaces the CPU engine; --jit/--uop-cache do not apply"), because the user typed
a contradiction; with the DEFAULTS on, `--kvm` simply prints one stderr line saying the engine is
KVM's and that the uop cache/JIT are unused. `--kvm --fixture` is valid (the nasm gate needs it).
`--kvm` with the bench or the determinism gate is REFUSED with the D-F2 message (Q2). `--kvm` in
boot mode is valid (the kernel gates).

**NO SILENT ANYTHING:** an unknown exit reason, `KVM_EXIT_INTERNAL_ERROR`, `KVM_EXIT_FAIL_ENTRY`
and `KVM_EXIT_SHUTDOWN` are STOPS that print the exit reason, the RIP and the port's own stop
class — never ignored, never treated as a halt. (`KVM_EXIT_SHUTDOWN` is a triple fault, which the
port's emulated lanes can also reach; the message must say which.)

---

## 13. Q6 — THE SLICES, WITH FILES, GATES, FALSIFIERS, SIZES AND RUNGS

Every unit starts at **CODER** (house rule; see C3) and escalates on a demonstrated failure.
Every unit's first gate is that the flag-off machine is unchanged:
`make transcript-diff && make determinism`, then `make selftest`. **Sizes are the planner's
estimate (PROJECTION), not measurements.**

**S0 THE SEAT** — flag, detection, ABI, vCPU lifecycle, state translation, memory carve, exit
skeleton, the step harness. FILES: `src/kvm/kvm_abi.h` (new, vendored ABI, ~230 lines);
`src/kvm/kvm.h` (~120); `src/kvm/kvm_state.c` (~450: `cpu_state_t`<->`kvm_regs`/`kvm_sregs`/
`kvm_fpu`/`KVM_GET_MSRS`, plus a TABLE OF EVERY `cpu_state_t` FIELD naming its carrier ioctl or
why it has none — that table is the honest seat against R1); `src/kvm/kvm_exit.c` (~380:
IO/MMIO/HLT/INTR/SHUTDOWN/INTERNAL/FAIL_ENTRY/DEBUG, the host tick, the delivery sink);
`src/kvm/kvm_selftest.c` (~400); edits: `src/cpu/memory.c:25` (page-aligned mem8),
`src/cpu/exec_boundary.c:87` (the clock indirection), `src/cpu/pic.c:423` + exceptions.h (the
delivery sink), `src/main.c` (parse + engine select + the transcript printer reused), `Makefile`
(`:112` `HW_SRC` area for `KVM_SRC`/`KVM_HDR`, `:378` the v86c link, `:433` the selftest list,
`:286` `.PHONY`). GATE `make kvm-selftest`: ~40 synthetic images through BOTH engines stepped,
canonical documents diffed; skips-with-a-name on a host with no KVM (like the JIT's platform
probe), fails on a real mismatch. FALSIFIERS (each must fail before and pass after): drop
`segment_access_bytes[FS]` from the translation; transpose CS/DS bases; drop `cs.db` (the
`is_32` duplicate); drop the FTW translation; forget the TSC origin pin. **SIZE ~1600 lines.**

**S1 THE M0 CORPUS, EXACT.** `--kvm --kvm-step` on `tests/test.img`; the transcript must be
byte-identical to the emulated leg's and the instruction count must be **561**. Gate
`make oracle-kvm` = diff of the two legs' documents (no node dependency, the emulated leg is the
arbiter). Falsifiers: an off-by-one in step counting (the hlt) -> 560 != 561; a dropped x87
tag-word translation -> the x87 fields differ. Then the corpus: `make nasm-kvm` compares the lane
against the NATIVE fixtures with a skip list that must be a subset of `nasm_skip_c.txt`.
**SIZE ~250.**

**S2 FULL SPEED** — the run loop, the host clock, the host tick, IO exits, the window carve.
GATE `make kvm-devices`: the VGA fixtures produce byte-identical `--vga-doc` documents and
identical transcripts; the T2 event traces match on the filtered port set. FALSIFIERS: do not
carve the window -> the document is empty/different; skip MMIO dispatch -> the LFB document
differs; make the tick period shorter than the PIT deadline -> the guest's tick count changes (an
asymmetry probe on M2). **SIZE ~400.**

**S3 INTERRUPTS AND MSRs** — the delivery sink in anger, the deadline tick, the MSR filter. GATE:
a fixture that programs the PIT and waits for IRQ0 takes the interrupt
(`tests/reviewfix-pmtimer.img` is the closest existing one); the MSR list gate compares the
port's table on both legs. FALSIFIERS: inject without updating the PIC's ISR -> the guest takes
exactly one tick and then none (a loud, measurable shape); forward an MSR the port models -> the
guest's readback differs. **SIZE ~350 + ~200.**

**S4 THE KERNEL** — `tests/gate-kvm-guest.sh`, the T3 shape: terminal RIP identical in N/N runs,
the same 14 guest lines as `tests/gate-m2-5-guest.sh`, the pinned disk md5, and the divergence
recorded. Cross-check against the lines `tests/gate-m2-5.sh`'s qemu arm already records for the
same artifacts. **SIZE ~200 + the script.**

**S5 64-BIT: NOT SCOPED** (see Q4.5).

**TESTS THE LANE MUST NOT TOUCH:** `make bench` and `make determinism` keep their meaning by
refusing `--kvm`; `make oracle`, `make nasm`, `make transcript-diff`, `make selftest` run exactly
as today with the flag absent.

---

## 14. Risks, ranked with SILENT-WRONG first

Each has its detector named, because a risk without a detector is a wish.

**R1 (silent) STATE TRANSLATION INCOMPLETENESS** — a field that is not round-tripped yields a
machine that runs and is wrong. Detectors: the field/carrier table in `kvm_state.c` (a field with
no carrier is a stated gap, not an omission); T1's stepped differential over the M0 + nasm
corpora; the five S0 falsifiers. This is the top risk and the reason S0 is the biggest slice: the
CPU mirror must be COMPLETE before a single device runs.

**R2 (silent) INTERRUPT STARVATION** — a guest that never exits gets stretched ticks and never
notices. Detector: the host tick with a maximum period (measured to work), plus the guest's own
`/proc/interrupts` counters in the T3 gate, plus an S2 falsifier that shortens the period and
must change the count.

**R3 (silent) MSR SEMANTICS BYPASS** — measured today; a guest probing MSRs takes a different
path. Detector: the S3 MSR filter unit and its two-leg list gate; refusal on hosts without the
filter.

**R4 (silent if ignored, loud if stopped) AN EXIT NOT HANDLED** — a new exit reason or a rep I/O
`count>1`. Detector: the rule that every unhandled exit is a named stop; the rep-I/O loop; the S0
selftest's synthetic-exit case.

**R5 (silent) THE VGA WINDOW CARVE** — if the window stays inside the RAM slot, the guest's VRAM
writes land in RAM and the VGA device goes dead. Detector: the `--vga-doc` document gate (frames,
not bytes) and the T2 trace.

**R6 (loud by design, silent if worded away) THE DECLARED CPUID DIFF (three leaves).** Detector:
the CPUID parity instrument records the accepted model and the refused leaves on every run; the
header names them; a guest that needs one is a finding, not a silence.

**R7 (silent) STEP-COUNT SEMANTICS** — an instruction that produces TWO exits inside a stepped
window (a DEBUG exit plus an MMIO/IO exit) must count ONCE. Detector: the M0 image has no I/O, so
it cannot catch this — add a stepped fixture that does an `out`, and count only DEBUG exits,
treating a device exit inside stepping as the same instruction.

**R8 (loud) THE aarch64 SLICE** — the vendored ABI must compile for both slices and the flag must
refuse by name on aarch64. Detector: `make` (the shipped APE is the aarch64 build too).

**R9 (process) INHERITING A PIN THE LANE CANNOT HOLD.** Detector: the bench/determinism refusal,
and the rule that every KVM gate states its own claim and its own bound in its own header.

---

## 15. NOT DECIDED BY THIS PLAN

The open questions, stated so nobody reads a gap as a promise.

1. **64-bit guest scope** — the user's decision (Q4.5, TODO.md section 8).
2. **Whether `--kvm` ever becomes a default or a second arm of an existing gate** — out of scope;
   the lane lives behind its flag.
3. **An exit-derived VIRTUAL clock** (`--kvm-clock=virtual`: N microseconds per exit) as a
   diagnostic mode for reproducible device-timing probes. It is implementable and it would give
   back determinism for device fixtures, but it breaks "an idle guest's time advances", so it is
   a THIRD contract, not a free win — deliberately not scoped.
4. **The breadth of MSR forwarding vs emulation** beyond the port's modelled set.
5. **Multi-vCPU:** the lane is SINGLE-vCPU because the machine is (the reference is single-CPU;
   every device module is written for one processor) — a second vCPU would be a different machine.
6. **The exact 16-byte MMIO split behaviour** (to be measured in S2).
7. **Whether a fetch from a carved hole is `INTERNAL_ERROR` with the full CPUID model installed**
   (measured once without it; re-measure).

---

## 16. OBLIGATIONS ON THE SHIPPED TREE

Independent of the lane: `allocate_memory`'s alignment change and the clock indirection and the
delivery sink are **three edits to validated files** — each must be proven inert by
`make transcript-diff` AND `make determinism` AND `make selftest`, with the pre-change and
post-change transcripts compared (the tree's rule: a comment-only or behaviour-neutral edit to a
validated file is proved, not asserted).

The KVM lane's own documentation deliverable is a `docs/architecture.md` section (a fourth boot
path table row is NOT right — it is a second MACHINE) plus a `docs/testing.md` entry stating
T1/T2/T3 and what each cannot catch; without those two paragraphs the lane's gates will later be
read as the emulated gates.

---

## Appendix A — the CPUID model table and the four bring-up traps

Self-contained: an implementer must be able to build the table from this file alone. Copied from
`handoff-v86c-kvm-artifact-1` (the load-bearing snippets from the probes — **copy, do not
re-derive**; `/tmp/kvmprobe/probe{2,3,4,5,6}.c` are throwaway recon, not deliverables).

### A.1 v86's CPUID model as `KVM_SET_CPUID2` entries

Transcribed from `src/cpu/instructions_0f.c:1118` `instr_0FA2` with acpi off, **SORTED ascending
by function** (KVM refuses an unsorted list — `EINVAL`, which cost a probe cycle).

Each brace is `{function, index, EAX, EBX, ECX, EDX}`; `flags` is 0 except where noted.

```c
{0x00000000,0,0x00000016,0x756E6547,0x6C65746E,0x49656E69}
{0x00000001,0,0x00000673,0x00010800,0xC0800001,0x0789A979}
{0x00000002,0,0x665B5001,0x00000000,0x00000000,0x007A7000}
{0x00000004,0,0x00000121,0x01C0003F,0x0000003F,0x00000001}
{0x00000004,1,0x00000122,0x01C0003F,0x0000003F,0x00000001}
{0x00000004,2,0x00000143,0x05C0003F,0x00000FFF,0x00000001}
{0x00000005,0,0x00000040,0x00000040,0x00000003,0x00142120}
{0x00000007,0,0x00000000,0x00000200,0x00000000,0x00000000}
{0x40000000,0,0x00000000,0x61774D56,0x4D566572,0x65726177}
{0x80000000,0,0x80000008,0x00000000,0x00000000,0x00000000}
{0x80000001,0,0x00000000,0x00000000,0x00000000,0x0190A179}
```

**LEAVES 4 AND 7 TAKE `flags = KVM_CPUID_FLAG_SIGNIFCANT_INDEX`.** Leaves `0x15`, `0x16` and
`0x80000008` are OMITTED by decision (Q2b, Q1). With exactly these **11** entries `SET_CPUID2`
succeeded and `GET_CPUID2` returned all 11 unchanged with no added leaves (MEASURED). If leaf
`0x1`'s EDX bit 9 (APIC) is wanted under `--acpi`, add it there and nothing else; the flushes that
matter are: after `SET_CPUID2`, KVM recomputes its own capability gates, so `SET_CPUID2` must
precede any device/IRQ setup that depends on CPUID.

> Note, carried as the sources state it: the plan's bring-up fact (a) reports the acceptance test
> on **14 leaves** ("13 of the 14 leaves", the exception being `0x80000008`), while the shipped
> table above has **11** entries (the three omitted leaves are `0x15`, `0x16`, `0x80000008`).

**CORRECTED BY THE ORCHESTRATOR — two EDX values in the source table were wrong, and the port
itself says so (MEASURED).** A tiny multiboot probe (`cpuid` then `mov ebx,edx; hlt`, read back
through `--fixture`) reports the port's real values: leaf `0x00000001` EDX = **`0x0789A979`**,
not `0x0F8BFBFF`; leaf `0x80000001` EDX = **`0x0190A179`**, not `0x0190A139` — the source table
had dropped **bit 6 (PAE)**, the one feature this tree spent a whole lane proving, and leaf 1's
EDX named a completely different feature set (the `0x0F8BFBFF` value includes bits the port never
sets and omits ones it does). The table above now carries the port's values, computed from
`src/cpu/instructions_0f.c:1118-1219` and confirmed by that probe. **Leaf 1's ECX is also
`0xC0800001` in a shipped build, not `0x40800001`:** `VMWARE_HYPERVISOR_PORT` is `1`
(`instructions_0f.c:1111`), so bit 31 (hypervisor-present) is always set. THE LESSON THE PARITY
INSTRUMENT MUST CARRY: a CPUID table is a claim about the emulated machine, and it has to be
*read back from the thing that presents it* (the probe above took one image and two runs), never
transcribed from a nearby source — the two fields that were wrong are exactly the two a guest
branches on (PAE and the leaf-1 feature set).

### A.2 The four bring-up traps

Each measured (all four cost a probe cycle).

- **`KVM_SET_REGS` BEFORE `KVM_SET_GUEST_DEBUG`**, or the single-step bit is silently lost (7
  instructions ran in one `KVM_RUN`).
- **A GATE DELIVERY READS THE GDT:** injecting a vector into a guest whose GDT is empty
  triple-faults to `rip 0xfff0`. A `KVM_SET_SREGS` test harness needs a real GDT (code `0x08`,
  data `0x10`) and a valid ESP, not just an IDT.
- **A triple fault POISONS the vCPU** for every later ioctl (`KVM_EXIT_INTERNAL_ERROR`
  afterwards). One VM per test case.
- **`KVM_SET_USER_MEMORY_REGION` requires PAGE-ALIGNED `userspace_addr`** (`EINVAL` otherwise) and
  slots that do not overlap; define the hole's geometry at VM creation, not by adding slots
  later.

### A.3 The step loop shape

Set regs, set `guest_debug(SINGLESTEP)`, then loop:

```
KVM_RUN;
if exit == DEBUG   -> GET_REGS/GET_SREGS, count one instruction
if exit == HLT     -> count one instruction and stop
any other exit     -> dispatch it as the same instruction (do NOT count twice)
```

### A.4 The host tick shape

`setitimer(ITIMER_REAL, period = min(next device deadline, 1 ms))`; `KVM_RUN` returns
-1/`EINTR` with `KVM_EXIT_INTR`; then `run_hardware_timers(microtick())` and inject. **Measured to
work at 2 ms.**

---

## Orchestrator verification

ORCHESTRATOR VERIFICATION OF THE PLAN'S TWO HINGES (read the artifact, not the summary).

1. **THE IRQ MECHANISM: the planner is RIGHT and my brief was WRONG** — `KVM_IRQ_LINE` without an
   in-kernel irqchip returns `ENXIO`; my probe measured **rc=-1 errno=6 (ENXIO)** against a VM
   with no `KVM_CREATE_IRQCHIP`, so the primitive that keeps the port's own PIC/PIT the machine
   is `KVM_INTERRUPT`, as the plan says.
2. **CPUID PARITY:** confirmed for the leaves v86 actually reports, **WITH A CAVEAT I MEASURED**.
   The port's model is small and known (`src/cpu/instructions_0f.c`: leaves 1, 4, 0x40000000,
   0x80000001, 0x80000008, 0x15, 0x16 and the rest), and the plan's three declared exceptions are
   exactly the ones KVM cannot reproduce: leaf `0x80000008` is REFUSED when the linear-width byte
   is non-zero (the port reports **32/32** — so it MUST be dropped, which is the plan's own
   finding) and `0x15`/`0x16` (TSC frequency) cannot be pinned because `KVM_SET_TSC_KHZ` was
   refused on this host. **MY PROBE ADDS A CAVEAT THE PLAN DOES NOT STATE:** "bit-exact
   round-trip" is a property of v86's 11 leaves, NOT of KVM's table in general — when I fed KVM
   the host's full **58-leaf** supported set, `SET_CPUID2`/`GET_CPUID2` came back **NOT
   bit-exact, normalising leaf 0xD** (XSAVE state sizes: **EBX 0x340 -> 0x240**). v86 does not
   report 0xD today, so the lane is unaffected, but **any leaf v86 adds later must be re-checked
   against SET/GET before the parity claim is extended to it.** RECORD THAT IN THE IMPLEMENTER'S
   BRIEF OR THE REPORT, not just here.
3. **The port's `0x80000008` (32/32 widths) and `0x15`/`0x16` cannot be presented** — the three
   leaves Q2b declares as the diff.

---

## Provenance

Measured **2026-10-18 on this host**: `/dev/kvm` **API 12**, an **AMD Ryzen 9 3950X with svm**
(kernel 7.1.8-1-default). The probe sources `/tmp/kvmprobe/probe2.c` .. `probe6.c` are
**throwaway and NOT deliverables**. The **memory nodes are the durable record**:
`handoff-v86c-kvm-plan` (this plan) and `handoff-v86c-kvm-artifact-1` (the CPUID table and the
four bring-up traps transcribed in Appendix A).

READ-BACK: the plan node's first line is the one to paste; the artifact node
`handoff-v86c-kvm-artifact-1` carries the CPUID model table and the four bring-up traps in code.
