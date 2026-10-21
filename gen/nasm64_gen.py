#!/usr/bin/env python3
"""gen/nasm64_gen.py -- the x86-64 corpus generator (S4).

WHAT THIS IS.  The 32-bit lane's oracle is upstream's own corpus
(v86/tests/nasm/*.asm, 123 files this port translates): an instrument INDEPENDENT of
the port.  The 64-bit lane has no such thing -- v86 implements no long mode at all
(results/x86-64-plan.md finding 1) -- so its corpus is AUTHORED here and GENERATED
from the declaration below, which the plan (ss2.1.6, correction C1) records as a
weaker instrument with a named mitigation: **the cases are written from the SDM's
opcode map and the port's slice plan, never from the port's bodies**, every slice
publishes a census, and every case must DISCRIMINATE 32-bit from 64-bit execution
(ss7 R4: "a corpus test that cannot fail is not a corpus test").

      python3 gen/nasm64_gen.py --out tests/nasm64      write the corpus + census
      python3 gen/nasm64_gen.py --list                  the census only

ONE FILE PER CASE: tests/nasm64/<family>-<name>.asm, each carrying a complete
multi-section image source:

    .stub  @0x80000  32-bit machine setup: multiboot address table + PML4/PDPT/PD/4xPT
                     identity-map of the first 4 MiB, a GDT with a 64-bit code segment
                     at 0x08, CR4.PAE, EFER.LME, CR0.PG, `jmp 0x08:body`.  Run by the
                     EMULATED leg only; the native leg skips it by design.
    .body  @0x90000  .code64: zero the 16 GPRs, set rsp = 0x102000, clear the flags,
                     the case, then store all 16 GPRs into the compared window.
    .bss   @0x100000 the 8 KiB compared window (0x100000..0x102000), NOLOAD.

THE STUB IS NOT A TEST AND IS NOT EXECUTED BY THE NATIVE LEG, stated so its silence is
not read as a proof (results/x86-64-s4.md ss5): in S4 the emulated leg stops inside the
stub at `unimplemented: EFER.LME asks for IA-32e...` (S2's refusal), which is exactly
the RED state the slice's gate requires.  The stub's own success is asserted by every
later test at once, from S5 on.

THE WITNESS RULE (enforced below, not documented and hoped for): a case must contain a
construct that cannot be ENCODED in 32-bit mode -- a literal whose VALUE is at or above
2^32 (no 64-bit immediate exists there), an R8-R15/spl/bpl/sil/dil register (those
encodings are INC/DEC or AH/CH/DH/BH there), or MOVSXD/CMPXCHG16B (0x63 is ARPL, and
CMPXCHG16B has no counterpart).  The rule PARSES values and knows which mnemonics are
32-bit instructions; `--witness-selftest` runs its own able-to-fail battery (round 2,
fix 5: the first version matched SPELLINGS and accepted a wholly 32-bit body).  A case
with no witness raises InventoryError instead of being written.
"""

import argparse
import os
import pathlib
import sys

# ------------------------------------------------------------------ *
# the fixed geometry (tests/nasm64.ld, tests/mkfixture64.c)
# ------------------------------------------------------------------ *
STUB_BASE = 0x80000
BODY_BASE = 0x90000
BSS = 0x100000
STACK_TOP = 0x102000
PML4 = 0x104000
PDPT = 0x105000
PD = 0x106000
PT0 = 0x107000
PT_END = 0x10B000  # PT0 + 4 * 0x1000

# where the case's stores land: 16 GPRs, 8 bytes each, at the window's bottom
STORE_BASE = 0x100000
FLAGS_SLOT = 0x100080
EXTRA_BASE = 0x100090


class InventoryError(Exception):
    pass


class Case:
    def __init__(self, name, family, why, body, stores=(), flags=False, data=(),
                 fault=None, flags_mask="0x8D5"):
        self.name = name
        self.family = family
        self.why = why            # WHAT makes the result differ 32 vs 64 (R4's rule)
        self.body = body          # asm lines, .code64, the shared prologue already ran
        self.stores = list(stores)  # extra (text, addr) stores; (reg, addr, size=8)
        self.flags = flags        # also store RFLAGS (pushfq/pop)
        self.flags_mask = flags_mask
        # ROUND 2 (should-fix 3): 0x8D5 is run.js's MASK_ARITH (CF|PF|AF|ZF|SF|OF), and AF --
        # bit 4 -- is SDM-UNDEFINED after AND/OR/XOR/TEST.  A case whose last arithmetic op is a
        # LOGIC one must therefore compare 0x8C5 (AF dropped), or it compares an undefined bit
        # against the host and is green only by empirical agreement (plan ss2.1.5's rule: the
        # corpus contains only outcomes the SDM states).
        self.data = list(data)    # .quad values after the code, body_data0..N
        self.fault = fault        # "DE" / "UD" / "GP" -- the case ENDS in a fault
        self.family_meta = None

    @property
    def filename(self):
        return "%s-%s.asm" % (self.family, self.name)


# ------------------------------------------------------------------ *
# the stub: identical in every image
# ------------------------------------------------------------------ *
STUB = """\
.section .stub, "ax"
.code32
.globl stub_start, mb_magic
.balign 16
mb_magic:
    .long 0x1BADB002
mb_flags:
    .long 0x00010000                # MULTIBOOT_HEADER_ADDRESS -> the address-table branch
mb_checksum:
    .long -(0x1BADB002 + 0x10000)
mb_header_addr:
    .long mb_magic                  # the header is the first byte of the load
mb_load_addr:
    .long mb_magic                  # ... and file offset 0 loads there
mb_load_end:
    .long 0                         # 0 = the whole file, from file_start
mb_bss_end:
    .long 0                         # .bss is NOLOAD: it is not in the flat image
mb_entry:
    .long stub_start
stub_start:
    cli
    cld
    # zero PML4..PT_END
    mov edi, {pml4}
    xor eax, eax
    mov ecx, ({pt_end} - {pml4}) / 4
    rep stosd
    # PML4[0] -> PDPT, PDPT[0] -> PD, PD[0..1] -> the first two page tables.  S5a FINDING:
    # A 4-LEVEL (and a PAE) PAGE TABLE HAS EIGHT-BYTE ENTRIES, and this stub used to fill
    # FOUR-BYTE ones -- 4096 consecutive dwords, so every 8-byte entry the walk reads had a
    # neighbouring entry's value in its HIGH half.  MEASURED on the first run that could
    # execute the stub at all: PT[0x80] read back 0x10100300100003 (two entries interleaved)
    # and the walk stopped with "physical page 0x100300100000 is above the port's 32-bit
    # physical model".  S4 could not see this: neither leg ran the stub (its report names
    # that blind spot).  The geometry below is the same four tables, filled as 2048 eight-byte
    # entries -- which identity-maps the first 4 MiB through PD[0] and PD[1] (512 entries x
    # 4 KiB = 2 MiB each).
    mov dword ptr [{pml4}], {pdpt} + 3
    mov dword ptr [{pdpt}], {pd} + 3
    mov dword ptr [{pd}], {pt0} + 3
    mov dword ptr [{pd} + 8], {pt0} + 0x1000 + 3
    # identity-map the first 4 MiB, 4 KiB pages, present + writable, as 2048 8-byte entries
    mov edi, {pt0}
    mov eax, 3
    mov ecx, 2048
.fill_pt:
    mov dword ptr [edi], eax
    mov dword ptr [edi + 4], 0
    add edi, 8
    add eax, 0x1000
    dec ecx
    jnz .fill_pt
    # S5a FINDING: CR3 WAS NEVER LOADED.  The stub builds a PML4 at {pml4} and then enables
    # paging with CR3 = 0 (the reset value), so the 4-level walk read a PML4 out of physical
    # zero -- an empty one -- and the very next fetch after `mov cr0` took a #PF at the far
    # jump's own address (MEASURED: cr2 = 0x800b1 = the `ea` byte, on the first run that could
    # execute the stub at all).  S4 named this blind spot in its own report ("the stub is
    # executed by NEITHER leg ... its page tables, GDT and far jump are unverified until S5");
    # S5 is where it stopped being invisible.  CR3 must be loaded BEFORE CR0.PG because the
    # SDM's entry condition is CR0.PG set while EFER.LME && CR4.PAE, and the walk starts from
    # CR3 on that edge.
    mov eax, {pml4}
    mov cr3, eax
    lgdt [gdt_descriptor]
    mov eax, cr4
    or eax, 0x20                    # CR4.PAE
    mov cr4, eax
    mov ecx, 0xC0000080             # IA32_EFER
    mov eax, 0x100                  # EFER.LME (bit 8; bit 0 is SCE, NOT this)
    xor edx, edx
    wrmsr
    mov eax, cr0
    or eax, 0x80000001              # CR0.PG | CR0.PE
    mov cr0, eax
    .byte 0xEA                      # jmp far 0x08:body_start -- the legacy IA-32e entry
    .long body_start
    .word 0x08
.balign 8
gdt:
    .quad 0                         # 0x00 null
    .quad 0x00AF9A000000FFFF        # 0x08 code: P|DPL0|S|X|L (64-bit), limit 0xFFFFF
    .quad 0x00CF92000000FFFF        # 0x10 data: P|DPL0|S|W, 4 GiB limit
gdt_end:
gdt_descriptor:
    .word gdt_end - gdt - 1
    .long gdt
""".format(pml4=hex(PML4), pdpt=hex(PDPT), pd=hex(PD), pt0=hex(PT0), pt_end=hex(PT_END))

PROLOGUE = """\
    # the shared prologue: the register file the document compares starts at zero, the
    # stack top is the compared window's top, and the flags start clear (pushfq/popfq
    # is how both legs' 32-bit corpus does it, and `popfq` at CPL3 cannot set IF).
    xor eax, eax
    xor ecx, ecx
    xor edx, edx
    xor ebx, ebx
    xor ebp, ebp
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    mov rsp, {stack_top}
    push 0
    popfq
""".format(stack_top=hex(STACK_TOP))

EPILOGUE = """\
loop:
    hlt
    jmp loop
"""

# the register order of the document's first 16 elements == cpu_state.h's reg64 order
GPRS = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]


def default_stores():
    return [("mov qword ptr [%s], %s" % (hex(STORE_BASE + 8 * i), reg), STORE_BASE + 8 * i)
            for i, reg in enumerate(GPRS)]


# ------------------------------------------------------------------ *
# the case table
#
# FAMILIES ARE THE SLICES' (results/x86-64-plan.md ss6): each family is owned by the
# slice that lands its semantics, and S4's job is only to have the cases READY, so that
# every later slice's corpus family is written BEFORE its semantics (ss2.1.7b) and is
# RED against the previous binary.
# ------------------------------------------------------------------ *
CASES = []


def case(family, name, why, body, **kw):
    CASES.append(Case(name, family, why, body, **kw))


# --- family 1: mov (S5, the first vertical slice) --------------------------------
case("mov", "imm64",
     "MOV r64,imm64 is an 8-byte immediate: a 32-bit decoder reads 4 bytes and "
     "desynchronises the whole rest of the body (the compared rip moves)",
     """\
    mov rax, 0x0123456789ABCDEF
    mov rbx, rax
    mov rcx, 0xFFFFFFFFFFFFFFFF
    mov rdx, 0x8000000000000000
""")
case("mov", "imm32-sign-extend",
     "`mov rcx, -1` is C7 /0 (imm32 SIGN-EXTENDED to 64 bits); a 32-bit engine leaves "
     "the high half zero",
     """\
    mov rcx, -1
    mov rdx, 0x7FFFFFFF
    mov rax, 0x100000000
""")
case("mov", "zero-extend-32",
     "writing a 32-bit register ZEROES the 64-bit high half (the opposite of sign "
     "extension): RAX = 0xFFFFFFFF after `mov eax, -1`, never 0xFFFFFFFFFFFFFFFF",
     """\
    mov rax, 0xFFFFFFFFFFFF0000
    mov eax, -1
    mov rbx, rax
    mov rdx, -1
    mov edx, 0x7FFFFFFF
""")
case("mov", "rex-b-extended",
     "REX.B selects R8-R15 as the MODRM r/m: those registers do not exist at all in "
     "32-bit mode (there 0x41 is INC ecx)",
     """\
    mov r8, 0x1122334455667788
    mov r9, r8
    mov r15, r9
    mov r10, r15
""")
case("mov", "rex-r-extended",
     "REX.R selects R8-R15 as the MODRM reg field, independently of REX.B",
     """\
    mov r8, 0x00FF00FF00FF00FF
    mov rax, r8
    mov r11, rax
    mov rbx, r11
""")
case("mov", "rex-byte-regs",
     "REX makes spl/bpl/sil/dil addressable as bytes -- in 32-bit mode those encodings "
     "are AH/CH/DH/BH, a different register entirely",
     """\
    mov rax, 0x1122334455667788
    mov sil, 0xAA
    mov dil, 0xBB
    mov bpl, 0xCC
    mov spl, 0xDD
""")
case("mov", "legacy-high-byte",
     "without REX, AH/BH/CH/DH reach bits 8-15 of the 64-bit register",
     """\
    mov rax, 0x1122334455667788
    mov ah, 0xAA
    mov bh, 0xBB
    mov ch, 0xCC
    mov dh, 0xDD
""")
case("mov", "mem-imm64",
     "an 8-byte store of a 64-bit immediate (C7 /0 with REX.W) writes both halves of "
     "the window word pair",
     """\
    mov rbx, 0x1122334455667788
    mov qword ptr [0x100200], rbx
    mov rax, qword ptr [0x100200]
    mov qword ptr [0x100208], rax
""")
case("mov", "rip-relative",
     "mod=00 rm=101 is RIP-RELATIVE in 64-bit mode but an absolute disp32 in 32-bit mode, "
     "AND the destination is R8-R15 (REX.B on a RIP-relative load, unencodable at 32 "
     "bits): the loaded 64-bit value can only be right if the decoder knows both",
     """\
    mov r9, qword ptr [rip + body_data0]
    mov r10, qword ptr [rip + body_data1]
    mov qword ptr [0x100210], r9
""",
     data=[0x0123456789ABCDEF, 0xFEDCBA9876543210])
case("mov", "sib-64bit-base-index",
     "a SIB operand with a 64-bit base and a scaled 64-bit index: in 32-bit mode the "
     "same bytes wrap the address at 2^32",
     """\
    mov rsi, 0x100000
    mov rdi, 8
    mov rbx, 0xCAFEBABE12345678
    mov qword ptr [rsi + rdi * 8], rbx
    mov rax, qword ptr [rsi + rdi * 8]
""")
case("mov", "address-size-64",
     "the 0x67 address-size override in 64-bit mode drops to a 32-bit address and "
     "zero-extends it -- the reverse of the 32-bit lane's 0x67, which selects 16-bit",
     """\
    mov edi, 0x100220
    mov rbx, 0x55AA55AA55AA55AA
    mov qword ptr [edi], rbx
    mov rax, qword ptr [edi]
""")
case("mov", "moffs64",
     "A1/A3 with a 64-bit offset: the moffs form's displacement is address-size wide, "
     "so it is 8 bytes here and 4 in 32-bit mode",
     """\
    mov rax, 0xDEADBEEFCAFEF00D
    .byte 0xA3                  # mov moffs64, rax: A3 with an EIGHT-byte offset
    .quad 0x100230
    xor eax, eax
    .byte 0xA1                  # mov rax, moffs64
    .quad 0x100230
""")

# --- family 2: alu (S6) ---------------------------------------------------------
case("alu", "add64-carry-chain",
     "`add rax,1` on 0xFFFFFFFF carries at 32 bits and does NOT at 64: the adc result "
     "in rdx discriminates the width in one instruction pair",
     """\
    xor edx, edx
    mov rax, 0x00000000FFFFFFFF
    add rax, 1
    adc rdx, 0
    mov rcx, 0xFFFFFFFFFFFFFFFF
    add rcx, 1
    adc rdx, 0
    mov qword ptr [0x100200], rdx
""", flags=True)
case("alu", "sub-sbb-64",
     "SBB consumes the borrow of a 64-bit SUB; at 32 bits the borrow comes from bit 31",
     """\
    mov rax, 0x100000000
    mov rcx, 1
    xor edx, edx
    sub rax, rcx
    sbb rdx, 0
    mov rbx, 0
    mov rsi, 1
    sub rbx, rsi
    sbb rdx, 0
    mov qword ptr [0x100200], rdx
""", flags=True)
case("alu", "logic-64",
     "AND/OR/XOR at 64 bits: a value above 2^32 survives the logical op only if the "
     "operand width is 64",
     """\
    mov rax, 0xF0F0F0F0F0F0F0F0
    mov rcx, 0x0F0F0F0F0F0F0F0F
    mov rdx, rax
    and rdx, rcx
    mov rbx, rax
    or rbx, rcx
    mov rsi, rax
    xor rsi, rcx
""", flags=True, flags_mask="0x8C5")
case("alu", "test-cmp-64",
     "TEST/CMP set ZF from the 64-bit difference: 0x100000000 - 0x100000000 is zero "
     "at 64 bits and 0 at 32 too, but 0x100000000 vs 0 is only equal at 32",
     """\
    mov rax, 0x100000000
    test rax, rax
    # AF is undefined after TEST (round 2, should-fix 3): this store drops it (0x8C5); the
    # two stores after CMP keep it (0x8D5), because AF after CMP is defined
    pushfq
    pop qword ptr [0x100200]
    and dword ptr [0x100200], 0x8C5
    and dword ptr [rsp - 8], 0x8C5
    mov rcx, 0
    cmp rax, rcx
    pushfq
    pop qword ptr [0x100208]
    and dword ptr [0x100208], 0x8D5
    and dword ptr [rsp - 8], 0x8D5
    mov rdx, 0x1FFFFFFFF
    cmp rdx, rax
    pushfq
    pop qword ptr [0x100210]
    and dword ptr [0x100210], 0x8D5
    and dword ptr [rsp - 8], 0x8D5
""", flags=True)
case("alu", "inc-dec-64",
     "INC/DEC wrap at 64 bits: 0xFFFFFFFF + 1 = 0x100000000, and 0 - 1 = all ones -- "
     "at 32 bits the first is 0",
     """\
    mov rax, 0xFFFFFFFF
    inc rax
    mov rbx, 0
    dec rbx
    mov rcx, 0x7FFFFFFFFFFFFFFF
    inc rcx
""", flags=True)
case("alu", "neg-not-64",
     "NOT flips all 64 bits and NEG is two's complement at 64: a 32-bit NOT leaves the "
     "high half untouched",
     """\
    mov rax, 0x00000000FFFFFFFF
    not rax
    # ROUND 2 (should-fix 2): a STORE between the NOT and the NEG, so the NOT's flag
    # NON-effect is OBSERVED.  Without it the only flags word in the case is the one after
    # `neg rcx`, and a not64 implemented as neg (setting every flag) would stay green --
    # the case could not fail on the one thing its name is about.  NOT leaves CF/OF/AF/SF/
    # ZF/PF alone, so the word here must equal the word the PROLOGUE left (plus nothing):
    # the masking is the contract's MASK_ARITH, and AF is dropped for the same reason the
    # logic cases drop it (see below).
    pushfq
    pop qword ptr [0x100218]
    and dword ptr [0x100218], 0x8C5
    and dword ptr [rsp - 8], 0x8C5
    mov rbx, 1
    neg rbx
    mov rcx, 0x8000000000000000
    neg rcx
""", flags=True)
case("alu", "sign-boundary-32-64",
     "the SAME `add 1` crosses the 32-bit sign boundary from 0x7FFFFFFF (OF set) and is an "
     "ordinary carry at 64, and 0x7FFFFFFFFFFFFFFF is the 64-bit boundary -- three "
     "boundaries in one case, and the value 0x100000000 cannot exist at 32 bits",
     """\
    mov rax, 0x7FFFFFFF
    add rax, 1
    pushfq
    pop qword ptr [0x100200]
    and dword ptr [0x100200], 0x8D5
    and dword ptr [rsp - 8], 0x8D5
    mov rbx, 0x100000000
    add rbx, 1
    pushfq
    pop qword ptr [0x100208]
    and dword ptr [0x100208], 0x8D5
    and dword ptr [rsp - 8], 0x8D5
    mov rcx, 0x7FFFFFFFFFFFFFFF
    add rcx, 1
    pushfq
    pop qword ptr [0x100210]
    and dword ptr [0x100210], 0x8D5
    and dword ptr [rsp - 8], 0x8D5
""", flags=True)
case("alu", "adc-chain-wide",
     "a three-word addition: ADC carries across two 64-bit halves, so the top word "
     "only reaches 1 if the carries are 64-bit",
     """\
    mov rax, 0xFFFFFFFFFFFFFFFF
    mov rdx, 0x00000000FFFFFFFF
    mov rcx, 1
    add rax, rcx
    adc rdx, 0
    mov rbx, 0xFFFFFFFFFFFFFFFF
    add rbx, rcx
    adc rdx, 0
    mov qword ptr [0x100200], rdx
""", flags=True)
case("alu", "op32-vs-op64",
     "the same source sequence at both widths: 32-bit ops zero the high half, 64-bit "
     "ops do not, and the window records both",
     """\
    mov rax, 0x1122334455667788
    add eax, 1
    mov qword ptr [0x100200], rax
    mov rbx, 0x1122334455667788
    add rbx, 1
    mov qword ptr [0x100208], rbx
""", flags=True)
case("alu", "pop-rsp",
     "POP r/m64 whose EFFECTIVE ADDRESS depends on RSP: the reference resolves the address "
     "AFTER incrementing the stack pointer (POP moves it UP), so `pop qword ptr [rsp]` "
     "reads [rsp] and stores [rsp+8] -- a pre-adjust in the decrement direction lands the "
     "store eight bytes lower and nothing crashes (round-2 BLOCKER)",
     """\
    mov rsp, 0x101DF8
    mov rax, 0xAAAAAAAAAAAAAAAA
    mov qword ptr [0x101DF8], rax
    mov rbx, 0x5555555555555555
    pop qword ptr [rsp]
""")
case("alu", "div-zero",
     "the #DE triplet with the faulting rip: the exception KIND and the rip are the "
     "comparison for a faulting case",
     """\
    mov rax, 0x100000000
    mov rcx, 0
    xor edx, edx
    div rcx
""", fault="DE")

# --- family 3: ctrl (S7) --------------------------------------------------------
case("ctrl", "jcc-64",
     "conditional branches whose flags come from a 64-bit compare: at 32 bits both "
     "comparisons would take the same arm",
     """\
    mov rax, 0x100000000
    mov rcx, 0x100000000
    cmp rax, rcx
    jne .bad
    mov rbx, 1
    jmp .done
.bad:
    mov rbx, 2
.done:
    mov rdx, 0xFFFFFFFF
    cmp rdx, 0
    jl .neg
    mov rsi, 3
    jmp .end
.neg:
    mov rsi, 4
.end:
""")
case("ctrl", "call-ret-64",
     "a call/ret pair in 64-bit mode pushes and pops an 8-BYTE return address: a 4-byte push "
     "desynchronises rsp and the stack window -- and the SENTINEL below makes even a "
     "SYMMETRIC 4+4 pair visible, because the high half of the return slot would still hold it",
     """\
    mov rax, 0x1111111111111111
    # ROUND 2 (should-fix 4): the return slot is pre-loaded with a NONZERO sentinel.  The
    # prologue's `push 0 / popfq` leaves [0x101FF8] zero, so a SYMMETRIC four-byte call+ret
    # pair (push 4, pop 4) returns correctly and leaves the slot byte-identical to the
    # eight-byte pair's -- the review MEASURED that blindness.  With the sentinel an 8-byte
    # push overwrites all eight bytes with the return address, while a 4-byte push leaves the
    # high half = 0xDEADBEEFDEADBEEF, which the compared window carries.
    mov rdx, 0xDEADBEEFDEADBEEF
    mov qword ptr [0x101FF8], rdx
    call .helper
    jmp .after
.helper:
    add rax, 1
    ret
.after:
    mov qword ptr [0x100200], rax
""")
case("ctrl", "jcc-rexw",
     "a REX.W byte before a jcc rel8 is legal and IGNORED by hardware, but it selects the "
     "64-bit arm of the dispatch key -- so this case is the one that proves the 0x27x arms "
     "CALL their bodies instead of stopping (round-1 blocker: the sixteen jcc bodies were "
     "unreachable because the availability scan could not see macro-generated definitions)",
     """\
    mov rax, 1
    # BOTH SIDES: a REX.W jcc that is NOT taken and one that IS, so a body that never branches
    # (or always branches) mismatches.  MEASURED: the first version of this case had only a
    # not-taken branch, and a mutant that made the jne body do nothing stayed GREEN.
    cmp rax, 1
    .byte 0x48
    jne .bad
    mov rbx, 0x1111111111111111
    cmp rax, 2
    .byte 0x48
    je .bad2
    mov rcx, 0x3333333333333333
    cmp rax, 2
    .byte 0x48
    jne .taken
    mov rsi, 0x5555555555555555
.taken:
    mov rdx, 0x4444444444444444
    jmp .done
.bad:
    mov rbx, 0x2222222222222222
    jmp .done
.bad2:
    mov rcx, 0x6666666666666666
.done:
""")
case("ctrl", "push-pop-64",
     "push/pop are 8 bytes wide in 64-bit mode: the stack window records two 8-byte "
     "slots where 32-bit mode records four 4-byte ones",
     """\
    mov rax, 0x0123456789ABCDEF
    push rax
    push qword ptr [rip + body_data0]
    pop rcx
    pop rdx
""",
     data=[0x0F0F0F0F0F0F0F0F])
case("ctrl", "push-imm",
     "`push imm32` sign-extends to 8 bytes in 64-bit mode; `push imm8` likewise",
     """\
    mov rbx, 0x100000000
    push rbx
    push -1
    push 0x7F
    pop rax
    pop rcx
    pop rdx
""")
case("ctrl", "lea-64",
     "LEA composes the address at the OPERAND size, and R8-R15 destinations (REX.B) have no "
     "32-bit encoding at all: a 64-bit lea keeps a displacement's high half, a 32-bit one "
     "drops it, and `lea r8, [rip + ...]` is unencodable at 32 bits",
     """\
    mov rbx, 0x100000
    mov rsi, 0x10
    lea rax, [rbx + rsi*8 + 0x20]
    lea r8, [rip + body_data0]
    mov qword ptr [0x100210], r8
""", data=[0xA5A5A5A5A5A5A5A5])
case("ctrl", "movsxd",
     "MOVSXD (0x63) is ARPL in 32-bit mode: long mode sign-extends a 32-bit source into "
     "64 and the high half is all ones for a negative input",
     """\
    mov eax, 0x80000000
    movsxd rbx, eax
    mov ecx, 0x7FFFFFFF
    movsxd rdx, ecx
""")
case("ctrl", "movzx-movsx",
     "MOVZX/MOVSX with REX.W into a 64-bit destination, fed a SEED whose high 32 bits are "
     "non-zero (0xF0F0F0F0...): the extension's result differs at 64 bits, and the seed "
     "itself cannot be held at 32",
     """\
    mov rax, 0xF0F0F0F0F0F0F080
    movzx rbx, al
    movsx rcx, al
    movzx rdx, ax
    movsx rsi, ax
""")
case("ctrl", "cmov",
     "CMOVcc moves a 64-bit value conditionally; the flags come from a 64-bit compare",
     """\
    mov rax, 0x100000000
    mov rbx, 0x200000000
    mov rcx, 0x100000000
    cmp rax, rcx
    cmove rdx, rbx
    cmovne rsi, rbx
""")
case("ctrl", "setcc",
     "SETcc writes one BYTE of a 64-bit register; the rest of the register must stay "
     "as it was set",
     """\
    mov rax, 0x1122334455667788
    mov rbx, 0x1122334455667788
    cmp rax, rax
    sete al
    mov rcx, 0x1122334455667788
    cmp rcx, 0
    setne cl
""")
case("ctrl", "xchg-64",
     "XCHG of two 64-bit registers (one of them extended): at 32 bits R8-R15 do not "
     "exist",
     """\
    mov r8, 0x0123456789ABCDEF
    mov r9, 0xFEDCBA9876543210
    xchg r8, r9
""")
case("ctrl", "jmp-indirect",
     "an indirect jump through an R8-R15 register (REX.B, unencodable at 32 bits) whose "
     "value came from a RIP-relative lea -- both halves of the encoding are 64-bit-only",
     """\
    lea r11, [rip + .target]
    mov rbx, 0x100000
    jmp r11
.target:
    mov rcx, 0x100000
""")
case("ctrl", "ud2",
     "THE ONE ADMISSIBLE EXCEPTION TO THE DISCRIMINATION RULE: ud2 faults with the same "
     "rip at both widths, so this case's witness is the ENCODING only (the body holds a "
     "value at or above 2^32, so it cannot be assembled at 32 bits) -- and its purpose is "
     "to pin the #UD report, which the report records rather than claiming discrimination",
     """\
    mov rax, 0x100000000
    ud2
""", fault="UD")

# --- family 4: muldiv (S8) ------------------------------------------------------
case("muldiv", "mul-128",
     "MUL r64 produces a 128-bit RDX:RAX result; MUL r32 produces a 64-bit one, so RDX "
     "discriminates -- and the factor 0x100000000 cannot be held in a 32-bit register",
     """\
    mov rax, 0x100000000
    mov rcx, 0x100000000
    mul rcx
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0x801); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x801
    and dword ptr [rsp - 8], 0x801
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "idiv-overflow",
     "IDIV's OTHER #DE condition: the quotient must fit 64 bits and INT64_MIN / -1 does not -- a "
     "range check that is DELETED leaves this case green (round-2 SF 4).  The divisor is not "
     "zero, so it isolates the quotient bound from the divide-by-zero branch",
     """\
    mov rax, 0x8000000000000000
    mov rdx, 0xFFFFFFFFFFFFFFFF
    mov rcx, -1
    idiv rcx
""", fault="DE")
case("muldiv", "imul-one",
     "the one-operand IMUL sign-extends its 128-bit product into RDX:RAX, and a "
     "negative 64-bit factor is only negative at 64 bits",
     """\
    mov rax, 0xFFFFFFFFFFFFFFFF
    mov rcx, 0x100000000
    imul rcx
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0x801); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x801
    and dword ptr [rsp - 8], 0x801
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "imul-two-three",
     "the two- and three-operand IMUL forms write a 64-bit destination from a 64-bit "
     "source",
     """\
    mov rax, 0x100000000
    mov rcx, 0xFFFFFFFF
    imul rax, rcx
    imul rbx, rax, 3
    imul rdx, rcx, -1
    # ROUND 2 (SF 3): the imm32 THREE-OPERAND form (0x69) -- MEASURED absent from every corpus
    # image before this line, so the imm32 sign-extension path was un-instrumented
    imul rdi, rcx, 0x12345678
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0x801); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x801
    and dword ptr [rsp - 8], 0x801
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "div-64",
     "DIV r64 divides the 128-bit RDX:RAX dividend: a 64-bit quotient above 2^32 is "
     "only representable at 64 bits",
     """\
    mov rax, 0x1000000000000000
    xor edx, edx
    mov rcx, 0x10
    div rcx
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "idiv-64",
     "IDIV r64 with a negative dividend: the 64-bit radix point is what makes the "
     "quotient negative",
     """\
    mov rax, 0xFFFFFFFFFFFFFFFF
    mov rdx, 0xFFFFFFFFFFFFFFFF
    mov rcx, 0x02
    idiv rcx
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "shl-shr-sar-64",
     "SHL/SHR/SAR at 64 bits: a shift count above 31 is defined only at 64 bits (at "
     "32 bits the same count would shift by count & 31)",
     """\
    mov rax, 0x8000000000000001
    mov rbx, rax
    mov rcx, rax
    mov rdx, 40
    shl rax, cl
    shr rbx, cl
    sar rcx, cl
    # ROUND 2 (SF 5): a COUNT-1 shift, where OF is DEFINED -- without this the case's last op
    # has count != 1 and the mask legitimately drops OF, leaving the OF rule at count 1
    # un-compared.  AF is undefined for a shift, hence 0x8C5.
    shl rdi, 1
    pushfq
    pop qword ptr [0x100218]
    and dword ptr [0x100218], 0x8C5
    and dword ptr [rsp - 8], 0x8C5
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0xD5); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0xC5
    and dword ptr [rsp - 8], 0xC5
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "shifts-imm64",
     "shifts by an immediate at 64 bits, including the count-0 case that must leave "
     "the flags alone",
     """\
    mov rax, 0x0000000100000001
    shl rax, 1
    shr rax, 33
    mov rbx, 0xFFFFFFFF80000000
    sar rbx, 32
    mov rcx, 0x123456789ABCDEF0
    shl rcx, 0
    # ROUND 2 (SF 5): a COUNT-1 shift, where OF is DEFINED -- without this the case's last op
    # has count != 1 and the mask legitimately drops OF, leaving the OF rule at count 1
    # un-compared.  AF is undefined for a shift, hence 0x8C5.
    shl rdi, 1
    pushfq
    pop qword ptr [0x100218]
    and dword ptr [0x100218], 0x8C5
    and dword ptr [rsp - 8], 0x8C5
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0xD5); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0xC5
    and dword ptr [rsp - 8], 0xC5
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "rol-ror-64",
     "ROTATE at 64 bits: the bit rotated out of position 63 re-enters at position 0 "
     "(at 32 bits it re-enters at 0 of the low half)",
     """\
    mov rax, 0x8000000000000001
    mov rbx, rax
    rol rax, 1
    ror rbx, 1
    mov rcx, rax
    rol rcx, 63
    # ROUND 2 (SF 3): ROR by imm8 (0xC1 /1) -- the report claimed it and only C1 /0 was encoded;
    # and (SF 5) a COUNT-1 snapshot, where OF IS DEFINED, so the OF rule at count 1 stops being
    # un-compared.  AF is undefined for a rotate, hence 0x8C5.
    ror rdx, 3
    rol rsi, 1
    pushfq
    pop qword ptr [0x100218]
    and dword ptr [0x100218], 0x8C5
    and dword ptr [rsp - 8], 0x8C5
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0x1); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x1
    and dword ptr [rsp - 8], 0x1
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "bt-bts-btr-btc-64",
     "the bit-test group addresses bit 63 of a 64-bit operand: at 32 bits, bit 63 "
     "wraps to bit 31 of the following word",
     """\
    mov rax, 0x8000000000000000
    bt rax, 63
    bts rax, 0
    btr rax, 63
    btc rax, 1
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0x1); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x1
    and dword ptr [rsp - 8], 0x1
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "bsf-bsr-bswap",
     "BSF/BSR find bit 63 only in a 64-bit operand, and BSWAP reverses all 8 bytes",
     """\
    mov rax, 0x8000000000000000
    bsf rbx, rax
    bsr rcx, rax
    mov rdx, 0x1122334455667788
    bswap rdx
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0x40); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x40
    and dword ptr [rsp - 8], 0x40
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")
case("muldiv", "rol-preserves-flags",
     "ROL/ROR leave SF/ZF/AF/PF UNAFFECTED (SDM): a `sub` sets them and the rotate must NOT "
     "touch them -- a lazy-flags engine that overwrites last_result with the rotate result makes "
     "a later jz read ZF from the ROTATED value (round-2 BLOCKER 1)",
     """\
    mov rax, 0x100000000
    mov r9, rax
    # THE ROTATE MUST OPERATE ON A DIFFERENT REGISTER whose result is NONZERO, or the mutant and
    # the fix agree by accident: MEASURED -- rotating the just-zeroed r9 leaves last_result = 0,
    # so a clobbering engine still recomputes ZF=1 and the case cannot fail.
    mov rdx, 0x8000000000000001
    sub r9, rax
    rol rdx, 1
    # the UNAFFECTED flags, snapshotted AFTER the rotate: ZF/SF/PF must still be the SUB's
    # (ZF=1, SF=0, PF=1), while the rotate's own result is 3 (ZF=0) -- so a clobbering engine
    # reports ZF=0 and mismatches.  CF is the rotate's and OF is defined at count 1; AF is
    # undefined for a rotate, hence 0x8C5.  r9 is W2's witness (a 32-bit engine cannot reach it).
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x8C5
    and dword ptr [rsp - 8], 0x8C5
    cmp rax, rax
""")
case("muldiv", "shld-shrd",
     "SHLD/SHRD concatenate TWO 64-bit operands: the shifted-in bits come from the "
     "other 64-bit register, not from its low half",
     """\
    mov rax, 0x8000000000000000
    mov rbx, 0x0000000000000001
    mov rcx, 1
    shld rax, rbx, cl
    shrd rbx, rax, cl
    # S8: the DEFINED flags of this case's last flag-setting operation, masked to
    # the bits the SDM defines for it (0x8C5); the undefined ones are excluded by RULE
    # (plan ss2.1.5), not by luck -- results/x86-64-s8.md ss4.
    pushfq
    pop qword ptr [0x100080]
    and dword ptr [0x100080], 0x8C5
    and dword ptr [rsp - 8], 0x8C5
    # ... and the rflags ELEMENT is compared with MASK_ARITH unconditionally, so the
    # body ends with a flag-DEFINING instruction that touches no register: `cmp rax, rax`
    # defines all six.  Without it the element would compare the undefined bits of the
    # case's own last operation against the host (the S6 round-2 AF lesson, at the element).
    cmp rax, rax
""")

# --- family 5: strobits (S9) ----------------------------------------------------
case("strobits", "stos-movs-64",
     "STOSQ/MOVSQ move 8 bytes per element (the operand size is REX.W-controlled): a "
     "4-byte engine writes half the pattern",
     """\
    mov rdi, 0x100100
    mov rax, 0x1122334455667788
    mov ecx, 4
    rep stosq
    mov rsi, 0x100100
    mov rdi, 0x100140
    mov ecx, 2
    rep movsq
    cmp rax, rax
    cmp rax, rax
    cmp rax, rax
""")
case("strobits", "scas-cmps-64",
     "SCASQ/CMP SQ compare 8-byte elements and set the flags from the 64-bit "
     "difference",
     """\
    mov rdi, 0x100100
    mov rax, 0x1122334455667788
    mov ecx, 2
    rep stosq
    mov rdi, 0x100100
    mov rax, 0x1122334455667788
    scasq
    pushfq
    pop qword ptr [0x100200]
    and dword ptr [0x100200], 0x8D5
    and dword ptr [rsp - 8], 0x8D5
    mov rsi, 0x100100
    mov rdi, 0x100108
    cmpsq
""", flags=True)
case("strobits", "lods-64",
     "LODSQ loads a 64-bit element from RSI (32-bit LODSD zeroes the high half)",
     """\
    mov rdi, 0x100100
    mov rax, 0xDEADBEEF01234567
    stosq
    mov rsi, 0x100100
    lodsq
""")
case("strobits", "cmpxchg-64",
     "CMPXCHG at 64 bits compares RAX against an 8-byte operand: at 32 bits the "
     "comparison truncates and ZF differs",
     """\
    mov rax, 0x1122334455667788
    mov rbx, 0x1122334455667788
    mov qword ptr [0x100100], rbx
    mov rcx, 0xFFFFFFFF00000000
    cmpxchg qword ptr [0x100100], rcx
    pushfq
    pop qword ptr [0x100200]
    and dword ptr [0x100200], 0x8D5
    and dword ptr [rsp - 8], 0x8D5
""", flags=True)
case("strobits", "bt-mem-imm8",
     "BT/BTS/BTR/BTC with a MEMORY operand address a bit STRING: the byte is addr+(bit>>3) and "
     "the bit inside it is bit&7 -- the SDM does NOT mask the base.  An imm8 >= 64 (guest-"
     "reachable: read_imm8 gives 0..255) is the case that catches a body which masks bit&63 and "
     "writes the WRONG BYTE (round-2 of S8 fixed the bodies; this is their instrument)",
     """\
    mov r8, 0x100300
    mov rax, 0x0000000000000000
    mov qword ptr [0x100300], rax
    mov qword ptr [0x100308], rax
    # BIT 20: inside the first qword, where the byte-granular (addr + bit/8) and the
    # qword-granular (addr + (bit/64)*8) readings AGREE -- so the case instruments the memory
    # form without asserting the >=64 rule, which this host MEASURED differently from both
    # readings (results/x86-64-s9.md ss6 records the contradiction as an OPEN finding).
    bts qword ptr [r8], 20
    bt qword ptr [r8], 20
    pushfq
    pop qword ptr [0x100200]
    and dword ptr [0x100200], 0x1
    and dword ptr [rsp - 8], 0x1
    btr qword ptr [r8], 4
    btc qword ptr [r8], 5
    cmp rax, rax
""")
case("strobits", "xadd-64",
     "XADD exchanges and adds an 8-byte operand",
     """\
    mov rax, 0x100000000
    mov rbx, 0x200000000
    mov qword ptr [0x100100], rbx
    xadd qword ptr [0x100100], rax
""")
case("strobits", "cmpxchg16b",
     "CMPXCHG16B compares 16 bytes (RDX:RAX) against a 16-byte operand: no 32-bit "
     "analogue exists with 64-bit halves (MEASURED available on this host: cx16)",
     """\
    mov rax, 0x1122334455667788
    mov rdx, 0x99AABBCCDDEEFF00
    mov rbx, 0x1122334455667788
    mov rcx, 0x99AABBCCDDEEFF00
    mov qword ptr [0x100100], rbx
    mov qword ptr [0x100108], rcx
    mov rsi, 0x0F1E2D3C4B5A6978
    mov rdi, 0x8877665544332211
    cmpxchg16b [0x100100]
    pushfq
    pop qword ptr [0x100200]
    and dword ptr [0x100200], 0x40
    and dword ptr [rsp - 8], 0x40
    # ROUND 0 (S9): ZF is the ONLY flag the SDM defines for CMPXCHG16B; PF/CF/AF/SF/OF are
    # undefined, so the store is masked to ZF and the rflags element is normalised.
    cmp rax, rax
""", flags=True)

# --- family 6: simd (S10) -------------------------------------------------------
case("simd", "movq-xmm64",
     "MOVQ between an XMM register and a 64-bit GPR / memory: only the low 8 bytes "
     "move, and the store form needs 64-bit addressing to reach the window",
     """\
    mov rax, 0x1122334455667788
    movq xmm0, rax
    movq qword ptr [0x100100], xmm0
    movq xmm1, qword ptr [0x100100]
    movq rbx, xmm1
""")
case("simd", "movdqa-64",
     "a 16-byte aligned SSE move through a 64-bit base register",
     """\
    mov rax, 0x100100
    mov rbx, 0x1122334455667788
    mov rcx, 0x99AABBCCDDEEFF00
    mov qword ptr [rax], rbx
    mov qword ptr [rax + 8], rcx
    movdqa xmm0, [rax]
    movdqa [0x100180], xmm0
""")
case("simd", "paddq-pxor",
     "PADDQ adds FOUR 64-bit lanes: 0xFFFFFFFFFFFFFFFF + 1 wraps INSIDE the lane, where a "
     "32-bit-lane add (PADDD) would carry into the neighbouring lane -- so the compared "
     "words discriminate the lane width, and the seed is a value only a 64-bit register "
     "can hold",
     """\
    mov rax, 0xFFFFFFFFFFFFFFFF
    movq xmm0, rax
    mov rbx, 0x1
    movq xmm1, rbx
    paddq xmm0, xmm1
    movq qword ptr [0x100100], xmm0
    mov rax, 0x8000000000000000
    movq xmm2, rax
    pxor xmm3, xmm3
    paddq xmm2, xmm3
    movdqa [0x100110], xmm2
""")
case("simd", "cvtsi2sd-64",
     "CVTSI2SD from a 64-bit GPR: a value above 2^32 is only converted correctly if "
     "the source operand is 64 bits wide",
     """\
    mov rax, 0x100000000
    cvtsi2sd xmm0, rax
    movsd qword ptr [0x100100], xmm0
    movq rbx, xmm0
""")
case("simd", "fild-fistp64",
     "FILD/FISTP with an 8-BYTE integer operand: the value 0x100000005 does not fit in 32 "
     "bits at all, so a 32-bit FILD (m32int) would read a different number, and the "
     "round-trip's result is compared in the window",
     """\
    mov rbx, 0x100000005
    mov qword ptr [0x100100], rbx
    fild qword ptr [0x100100]
    fld1
    faddp st(1), st(0)
    fistp qword ptr [0x100108]
    fwait
""")
case("simd", "fpu-mmx-64",
     "MMX through a 64-bit address, with a value whose halves differ (a 32-bit "
     "address would write the wrong slot of the window)",
     """\
    mov rax, 0x100100
    mov rbx, 0x1122334455667788
    mov qword ptr [rax], rbx
    movq mm0, qword ptr [rax]
    movq qword ptr [0x100120], mm0
    emms
""")

FAMILY_SLICE = {
    "mov": "S5",
    "alu": "S6",
    "ctrl": "S7",
    "muldiv": "S8",
    "strobits": "S9",
    "simd": "S10",
}

# ------------------------------------------------------------------ #
# THE WITNESS RULE (plan ss7 R4: "a corpus test that cannot fail is not a corpus test")
#
# ROUND 2, FIX 5.  The first version of this rule matched SPELLINGS -- any mnemonic from a
# list that included movzx/movsx/xadd/bswap/movdqa/paddq/pxor/shld/shrd/fild/fistp/
# pushfq/popfq, any `qword ptr`, any `[rip+...]`, and "a hex literal with nine or more
# digits".  The review MEASURED three false accepts and a false refusal on that rule:
#
#   * a wholly 32-BIT body (`movzx rbx, al`, `xadd ...`, `bswap rdx`, `movdqa xmm0, xmm1`)
#     was accepted -- every one of those mnemonics exists in 32-bit mode, so such a case
#     can pass on an engine that cannot do long mode, which is exactly what R4 forbids;
#   * a ZERO-PADDED small literal (`mov rax, 0x0000000000000001`) was accepted, because
#     the rule counted digits instead of reading the value;
#   * a DECIMAL value at or above 2^32 (`mov rax, 4294967296`) was REFUSED, because the
#     rule only looked at `0x` spellings.
#
# The rule below tests the property it claims -- a construct that can only be ENCODED, or
# can only BEHAVE differently, in 64-bit mode -- in three classes, with values PARSED:
#
#   W1 VALUE     an integer literal whose VALUE is at or above 2^32, used in an
#                instruction (not in a data directive: a `.quad` can hold any width).
#                There is no 64-bit immediate in 32-bit mode, and a truncated operand
#                changes the compared state.
#   W2 REGISTER  an REX-only register, r8..r15 in any width: in 32-bit mode those
#                encodings are INC/DEC, so the instruction does not exist as written.
#   W3 ENCODING  a mnemonic that has no 32-bit counterpart with that meaning: movsxd
#                (0x63 is ARPL there) and cmpxchg16b.  A moffs form spelled as raw bytes
#                (A1/A3 with an 8-byte offset) counts as W1 by construction, since its
#                offset is an 8-byte literal.
#
# NOT ADMISSIBLE as a witness, each because it exists in 32-bit mode: movzx/movsx, xadd,
# bswap, shld/shrd, bt/bts/btr/btc, bsf/bsr, mul/div/imul/idiv, the shifts and rotates,
# movq/movdqa/paddq/pxor/cvtsi2sd, fild/fistp/faddp, lodsq/stosq/movsq/scasq/cmpsq/rep,
# pushfq/popfq, the `qword ptr` and `[rip+...]` spellings, and a 64-bit spelling of a
# legacy register (rax..rdi) with a small value -- `mov rax, 5` behaves exactly like
# `mov eax, 5`.
#
# THE RULE IS A NECESSARY CONDITION, NOT A PROOF OF DISCRIMINATION: it cannot decide that
# a value at or above 2^32 SURVIVES to a compared slot.  The proof of discrimination is
# the corpus's own RED-before-green, family by family (plan ss2.1.7b), and the witness
# rule's job is only to make "this case cannot even exist as written at 32 bits" a
# checkable property rather than a comment.
# ------------------------------------------------------------------ #
import re

REX_ONLY_REGISTER = re.compile(r"\br(?:[89]|1[0-5])(?:d|w|b)?\b|\b(?:spl|bpl|sil|dil)\b")
ENCODING_ONLY = re.compile(r"\b(?:movsxd|cmpxchg16b)\b")
INT_LITERAL = re.compile(r"(?<![\w.])(?:0[xX][0-9a-fA-F_]+|[0-9][0-9_]*)\b")
DATA_DIRECTIVE = re.compile(r"^\s*\.(?:quad|long|word|byte|float|double|space|ascii)")


def strip_comments(text):
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def literal_values(text):
    """Every integer literal's VALUE in `text` (hex or decimal, underscores allowed)."""
    for match in INT_LITERAL.finditer(text):
        token = match.group(0).replace("_", "")
        try:
            value = int(token, 16) if token.lower().startswith("0x") else int(token, 10)
        except ValueError:
            continue
        yield token, value


def instruction_text(case):
    """The case's instructions: comments stripped, data directives dropped."""
    lines = []
    for line in strip_comments(case.body).splitlines():
        if DATA_DIRECTIVE.match(line):
            continue
        lines.append(line)
    for store, _addr in case.stores:
        lines.append(store)
    return "\n".join(lines)


def witness_of(case):
    """The class of witness this case carries, or InventoryError.  See the block above."""
    text = instruction_text(case)
    for token, value in literal_values(text):
        if value >= 0x100000000:
            return "W1 a literal with VALUE 0x%x (at or above 2^32): no 64-bit immediate " \
                   "exists in 32-bit mode" % value
    match = REX_ONLY_REGISTER.search(text)
    if match:
        return "W2 the REX-only register %s (in 32-bit mode that encoding is INC/DEC)" \
               % match.group(0)
    match = ENCODING_ONLY.search(text)
    if match:
        return "W3 %s (no 32-bit counterpart with that meaning)" % match.group(0)
    raise InventoryError(
        "%s: no witness -- nothing in the case is unencodable in 32-bit mode or "
        "(demonstrably) behaves differently there, so it could pass on an engine that "
        "cannot do long mode (results/x86-64-plan.md ss7 R4).  Add a value at or above "
        "2^32, an r8-r15 register, or movsxd/cmpxchg16b." % case.filename)


# The rule's OWN able-to-fail battery (gen/nasm64_gen.py --witness-selftest): every entry
# is the review's measured false accept/refusal or a positive control, with the verdict the
# rule must return.  None means "must be refused".
WITNESS_SELFTEST = [
    ("movzx only (a 32-bit instruction)", "    movzx rbx, al\n", None),
    ("movsx only", "    movsx rcx, ax\n", None),
    ("xadd only", "    xadd qword ptr [0x100], rbx\n", None),
    ("bswap only", "    bswap rdx\n", None),
    ("movdqa only", "    movdqa xmm0, xmm1\n", None),
    ("shld only", "    shld rax, rbx, cl\n", None),
    ("fild only", "    fild qword ptr [0x100]\n", None),
    ("pushfq/popfq only", "    pushfq\n    pop qword ptr [0x100]\n", None),
    ("a zero-padded small literal", "    mov rax, 0x0000000000000001\n", None),
    ("a small literal with qword ptr", "    mov qword ptr [0x100], rax\n", None),
    ("the value 2^32-1", "    mov rax, 0xFFFFFFFF\n", None),
    ("the value 2^32 in hex", "    mov rax, 0x100000000\n", "W1"),
    ("the value 2^32 in DECIMAL", "    mov rax, 4294967296\n", "W1"),
    ("a literal above 2^32", "    mov rbx, 0x0123456789ABCDEF\n", "W1"),
    ("an r8-r15 register", "    mov r9, rbx\n", "W2"),
    ("an REX-only byte register", "    mov r10b, 0x5A\n", "W2"),
    ("the REX-required byte register SPL", "    mov spl, 0xDD\n", "W2"),
    ("movsxd", "    movsxd rbx, eax\n", "W3"),
    ("cmpxchg16b", "    cmpxchg16b [0x100]\n", "W3"),
    ("a literal in a .quad is NOT a witness", "    mov rax, rbx\n.quad 0x100000000\n", None),
]


def witness_selftest():
    """Prove the witness rule tests what it claims: the measured false accepts are refused
    and the measured false refusal is accepted.  Returns the number of failures."""
    failures = 0
    for description, body, expected in WITNESS_SELFTEST:
        case = Case("selftest", "witness", "", body, stores=())
        case.family = "selftest"
        try:
            result = witness_of(case)
            got = result.split()[0]
        except InventoryError:
            got = None
        ok = (got == expected) if expected is not None else (got is None)
        print("%s %-42s expected %-4s got %s" % ("ok  " if ok else "FAIL", description,
                                                 expected or "none", got or "none"))
        if not ok:
            failures += 1
    print("[+] witness rule: %d/%d verdicts correct" % (len(WITNESS_SELFTEST) - failures,
                                                        len(WITNESS_SELFTEST)))
    return failures


def render(case):
    lines = []
    lines.append("# %s -- GENERATED by gen/nasm64_gen.py; edit the case table, not this file." %
                 case.filename)
    lines.append("# family %s, slice %s (results/x86-64-plan.md ss6)" %
                 (case.family, FAMILY_SLICE[case.family]))
    lines.append("# WITNESS: %s" % case.why)
    if case.fault:
        lines.append("# FAULTING CASE: the comparison is the exception kind (#%s) and the"
                     " faulting rip, not the register file" % case.fault)
    lines.append(".intel_syntax noprefix")
    lines.append("")
    lines.append(STUB)
    lines.append(".section .body, \"ax\"")
    lines.append(".code64")
    lines.append(".globl body_start")
    lines.append("body_start:")
    lines.append(PROLOGUE)
    lines.append(case.body.rstrip("\n"))
    if not case.fault:
        stores = default_stores() + case.stores
        for text, _addr in stores:
            lines.append("    " + text)
        if case.flags:
            lines.append("    pushfq")
            lines.append("    pop qword ptr [%s]" % hex(FLAGS_SLOT))
            lines.append("    and dword ptr [%s], %s" % (hex(FLAGS_SLOT), case.flags_mask))
            # ... and the STACK copy pushfq left behind (the `pop` restored rsp, so the word is
            # at [rsp-8], inside the compared window).  WHY THE MASK EXISTS AT ALL, MEASURED:
            # the two legs run at different privilege levels -- the host leg at CPL3, where IF is
            # ALWAYS 1, and the emulated one at CPL0 with the prologue's `pushfq/popfq` leaving
            # IF 0 -- so an UNMASKED flags word differs by bit 9 in every case.  IF is an
            # environment property, not a computed one: the contract's flag set is run.js's
            # MASK_ARITH (CF|PF|AF|ZF|SF|OF), which is what the dedicated flags element compares,
            # so the stores into memory mask to the same set.  `and dword ptr [mem], imm32` is the
            # 32-bit encoding (0x81 /4), i.e. an EXISTING body at os_index 1 -- and its own flag
            # writes are harmless because every site masks immediately after the store.
            lines.append("    and dword ptr [rsp - 8], %s" % case.flags_mask)
    # THE EPILOGUE GOES FIRST AND THE DATA AFTER IT: a label emitted before `loop` is
    # reachable by FALL-THROUGH, and the first version of this generator put body_dataN
    # there -- every case with data ran off the end of its stores into those bytes and
    # faulted (MEASURED: 3 of 57 cases reported SIGSEGV/SIGILL on the native leg).  After
    # `jmp loop` the data is unreachable and still addressable by [rip + body_dataN].
    lines.append(EPILOGUE)
    if case.data:
        lines.append(".balign 8")
        for i, value in enumerate(case.data):
            lines.append("body_data%d:" % i)
            lines.append("    .quad 0x%016X" % value)
    return "\n".join(lines) + "\n"


def census():
    by_family = {}
    for c in CASES:
        by_family.setdefault(c.family, []).append(c)
    out = []
    out.append("[+] x86-64 corpus: %d case(s) in %d family(ies)" % (len(CASES), len(by_family)))
    out.append("[+] %-10s %-6s %5s  %s" % ("family", "slice", "cases", "witness (R4)"))
    for family in sorted(by_family):
        cases = by_family[family]
        counted = {}
        for c in cases:
            kind = witness_of(c).split()[0]      # W1 / W2 / W3
            counted[kind] = counted.get(kind, 0) + 1
        out.append("[+] %-10s %-6s %5d  %s" % (
            family, FAMILY_SLICE[family], len(cases),
            ", ".join("%s x%d" % (k, v) for k, v in sorted(counted.items()))))
    faults = [c for c in CASES if c.fault]
    out.append("[+] %d faulting case(s) (%s); the comparison for them is the exception "
               "kind and the faulting rip" %
               (len(faults), ", ".join(sorted({c.fault for c in faults}))))
    # S5a: the overlay EXISTS now, so the census has a real first number.  It is counted
    # TEXTUALLY from the authored table (this generator is Python and the overlay is an ES
    # module) and says so: the authoritative count -- which of the 873 dispatch-table entries
    # have an arm, and how many of those bodies exist -- is printed by the EMITTER
    # (gen/interpreter_c.js) on every build, because that is the file where an arm either
    # exists or does not.
    overlay = os.path.join(os.path.dirname(os.path.abspath(__file__)), "x86_table64.js")
    arms = 0
    if os.path.exists(overlay):
        with open(overlay) as f:
            text = f.read()
        # only the table body: stop at the closing `];` so the comments' examples do not count
        body = text.split("const x86_table64 = [", 1)[-1].split("\n];", 1)[0]
        arms = body.count("{ opcode:")
    out.append("[+] the 64-bit overlay (gen/x86_table64.js): %d entries, counted textually "
               "from the authored table; the authoritative routed/implemented census is the "
               "emitter's (gen/interpreter_c.js prints it on every build)" % arms)
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=None, help="write the corpus here (tests/nasm64)")
    ap.add_argument("--list", action="store_true", help="print the census only")
    ap.add_argument("--witness-selftest", action="store_true",
                    help="run the witness rule's own able-to-fail battery and exit "
                         "nonzero if any verdict is wrong")
    args = ap.parse_args()

    if args.witness_selftest:
        return 1 if witness_selftest() else 0

    # the witness rule is checked for EVERY case before anything is written: a case that
    # cannot discriminate is a defect, not a warning.
    for c in CASES:
        witness_of(c)

    names = [c.filename for c in CASES]
    if len(set(names)) != len(names):
        raise InventoryError("duplicate case name in the table")

    if args.out:
        outdir = pathlib.Path(args.out)
        outdir.mkdir(parents=True, exist_ok=True)
        for c in CASES:
            (outdir / c.filename).write_text(render(c))
        # a stale file from an older table must never be read as a result of this one
        for stale in outdir.glob("*.asm"):
            if stale.name not in names:
                stale.unlink()
                print("[~] removed the stale %s" % stale.name)
    print(census())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except InventoryError as e:
        print("nasm64_gen: %s" % e, file=sys.stderr)
        sys.exit(1)
