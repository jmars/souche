# x86-64: the nested exceptions are one `swapgs` on an unfilled SS slot — localized, not yet fixed

**Status: MEASUREMENT. The three nested exceptions are localized end to end and the chain is
MEASURED at every link; no fix is claimed, because the evidence stops one link short of one — the
kernel's return path reads an **SS slot the early stub never fills**, and what *should* be in that
slot on hardware is the open question. The instrument for it is now in place.**

Every claim is tagged **MEASURED** / **READ** / **INTERPRETATION** / **PROJECTION**.

---

## 1. All four frames (MEASURED)

`V86C_BREAK=0xffffffff8ae77120` (`early_idt_handler_common`) with `BREAK_MAX=8` and the new
`V86C_PEEK_SP` (an rsp-relative **virtual** dump — the kernel stack is not identity-mapped, so a
frame cannot be read with the physical `V86C_PEEK`). Four entries, **every vector 0x0e (#PF)**:

| # | frame rip | `cr2_64` |
|---|---|---|
| 1 | `0xffffffff8ae77246` (`copy_bootdata`'s `rep movsd`) | `0xffff888000084790` ✓ |
| 2 | `0xffffffff89a0123a` (`mov %gs:0x19000,%rdi`) | **`0x19000`** |
| 3 | `0xffffffff88f4bb09` (`call 0xffffffff81a4a780`) | **`0x1fb40`** |
| 4 | `0xffffffff88f4bb09` (same `call`) | **`0x1fb40`** |

Frame 1's `cr2` is the full direct-map address ✓. Frames 2–4 are **the bare offset, without the GS
base** — and that is faithful, not a resolver bug: §3 shows `GS.base` really is 0 at that access.

**The pushed frame is SDM-correct.** Read at the handler's entry: vector, error code, rip, cs,
rflags — three words of hardware frame plus the stub's vector, and *no* SS/RSP, which is exactly what
a same-privilege exception pushes. **Candidate (a) — a wrong frame layout for the push — is ruled
out.**

## 2. The handler's tail is the real restore path (READ)

Disassembled from the image at link `0xffffffff83077120`:

    ffffffff83077148  mov %rsp,%rdi
    ffffffff8307714b  call 0xffffffff830774f3        ← do_early_exception
    ffffffff83077150  decl early_recursion_flag      ← the decrement IS reached
    ffffffff83077156  jmp 0xffffffff81c011f9         ← restore_regs_and_return_to_kernel

So the early stub **hands its frame to the real return path**, and that path is where the next three
faults come from:

    ffffffff81c011f9  pop r15 … pop rdi              (15 pops)
    ffffffff81c01210  add $0x8,%rsp                  (skip orig_ax)
    ffffffff81c01219  testb $0x4,0x20(%rsp)          ← the SS slot decides
    ffffffff81c0121e  jne 0xffffffff81c01222
    ffffffff81c01220  iretq
    ffffffff81c01222  push %rdi ; swapgs ; jmp 0xffffffff81c0123a
    ffffffff81c0123a  mov %gs:0x19000,%rdi

## 3. What the branch actually reads (MEASURED)

At `0xffffffff89a01219`, `rsp = 0xffffffff8a803f18`, and `V86C_PEEK_SP` gives:

    0x00(%rsp) = 0xffffffff8ae77246   ← ip      (correct: the faulting rip)
    0x08       = 0x10                 ← cs      (correct)
    0x10       = 0x46                 ← rflags  (correct)
    0x18       = 0x84790              ← "sp"    — copy_bootdata's own rbx/rsi value
    0x20       = 0xffffffff8ae7760d   ← "ss"    — a kernel TEXT address, not a selector

`testb $0x4` on `0x0d` is **set**, so the kernel takes its user-return branch: `swapgs`, then the
per-cpu copy. And at that `swapgs` (MEASURED with the new dump fields):

    gs_base = 0xffffffff8ae42000   kernel_gs = 0x0

so the swap sends `GS.base` to **0**, and the very next instruction's `%gs:0x19000` resolves to the
bare `0x19000` → #PF. `early_make_pgtable(0x19000)` then computes
`physaddr = 0x19000 - 0xffff888000000000` (wrapped, `>= MAXMEM`) and **refuses** — the demand-repair
mechanism only knows direct-map addresses — so the kernel panics, and the third nested fault trips
`early_recursion_flag > 2`, which halts **silently** (the panic print never reaches the serial).

## 4. Where the evidence stops

The `sp`/`ss` slots hold **copy_bootdata's own register values** (`0x84790` was `%rbx`/`%rsi` at the
`rep movsd`), i.e. the restore path is reading two slots **past** the frame the stub built. Two
readings are consistent with that and the measurements do not separate them:

1. **The early stub is meant to push SS/RSP** for a same-privilege exception (hardware does not) and
   the port mis-executes those two pushes — this is candidate (b) in a new place, and the stub's
   push list is the artifact to read next (its 15 register pushes are at link
   `0xffffffff8307715c`…`0xffffffff83077171`).
2. **The kernel relies on `sp`/`ss` being stale** and hardware's stack happens to hold a value whose
   bit 2 is clear, while the port's holds `0xffffffff8ae7760d`. That would make the port faithful and
   the divergence a *stack-content* difference, which is a different (and much less likely) story —
   it needs the same slot's content on QEMU to settle.

**INTERPRETATION, offered as the cheaper hypothesis:** (1) is the one to test first, because the
`sp` slot holding a *register* value is what a two-slot shortfall looks like, and because this lane
has already paid twice for exactly that shape (a 32-bit field holding a 64-bit address; a base
dropped from one branch of a resolver).

## 5. The instrument this unit added

`V86C_PEEK_SP=<len>` — an **rsp-relative virtual** dump at each printed break hit (through the
port's own walk, with the guest's abort state restored afterwards). Without it a frame cannot be read
at all: the kernel stack is not identity-mapped, and the existing `V86C_PEEK` is physical. The break
dump also prints `gs_base`, `kernel_gs` and `fs_base` now — the two fields that named §3.

## 6. Verification (MEASURED; **toolkit code changed**, full battery, by EXIT CODE)

    make / selftest / nasm / nasm64        rc 0 (selftest: all suites green)
    make bench                             200095214
    make transcript-diff                   byte-identical to the reference
    make determinism                       two consecutive runs byte-identical
    gate-m2-5-determinism                  4,000,096,757 both runs, serials byte-identical
    gate-x86-64 12/0 · gate-long-mode 57/0 · gate-long-mode-sys 5/0 · gate-x86-64-diff 13/0
    gate-nasm64 13/0 · gate-reboot-1 39/0 · gate-pae-1 11/0 · gate-pae-l3 0 FAILING
    gate-vga-0..4                          0 FAILING each

**No pin moved** — the change is two `fprintf`s and one dump function behind `v86c_dbg_break_on`.

**The calibrated target is NOT reached**: no first `printk`, so nothing was added to
`gate-x86-64.sh`.

## 7. Files touched

| file | change |
|---|---|
| `src/cpu/debug.c` | `V86C_PEEK_SP` (rsp-relative virtual dump); `gs_base`/`kernel_gs`/`fs_base` in the break dump |
| `results/x86-64-nest.md` | this report |
| `TODO.md` | §8 |
