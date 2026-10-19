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
                 fault=None):
        self.name = name
        self.family = family
        self.why = why            # WHAT makes the result differ 32 vs 64 (R4's rule)
        self.body = body          # asm lines, .code64, the shared prologue already ran
        self.stores = list(stores)  # extra (text, addr) stores; (reg, addr, size=8)
        self.flags = flags        # also store RFLAGS (pushfq/pop)
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
    # PML4[0] -> PDPT, PDPT[0] -> PD, PD[0..3] -> the four page tables
    mov dword ptr [{pml4}], {pdpt} + 3
    mov dword ptr [{pdpt}], {pd} + 3
    mov dword ptr [{pd}], {pt0} + 3
    mov dword ptr [{pd} + 8], {pt0} + 0x1000 + 3
    mov dword ptr [{pd} + 16], {pt0} + 0x2000 + 3
    mov dword ptr [{pd} + 24], {pt0} + 0x3000 + 3
    # identity-map the first 4 MiB, 4 KiB pages, present + writable (4 x 1024 entries)
    mov edi, {pt0}
    mov eax, 3
    mov ecx, 4096
.fill_pt:
    mov dword ptr [edi], eax
    add edi, 4
    add eax, 0x1000
    dec ecx
    jnz .fill_pt
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
""", flags=True)
case("alu", "test-cmp-64",
     "TEST/CMP set ZF from the 64-bit difference: 0x100000000 - 0x100000000 is zero "
     "at 64 bits and 0 at 32 too, but 0x100000000 vs 0 is only equal at 32",
     """\
    mov rax, 0x100000000
    test rax, rax
    pushfq
    pop qword ptr [0x100200]
    mov rcx, 0
    cmp rax, rcx
    pushfq
    pop qword ptr [0x100208]
    mov rdx, 0x1FFFFFFFF
    cmp rdx, rax
    pushfq
    pop qword ptr [0x100210]
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
    mov rbx, 0x100000000
    add rbx, 1
    pushfq
    pop qword ptr [0x100208]
    mov rcx, 0x7FFFFFFFFFFFFFFF
    add rcx, 1
    pushfq
    pop qword ptr [0x100210]
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
     "a call/ret pair in 64-bit mode pushes and pops an 8-BYTE return address: a "
     "4-byte push desynchronises rsp and the stack window",
     """\
    mov rax, 0x1111111111111111
    call .helper
    jmp .after
.helper:
    add rax, 1
    ret
.after:
    mov qword ptr [0x100200], rax
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
""")
case("muldiv", "imul-one",
     "the one-operand IMUL sign-extends its 128-bit product into RDX:RAX, and a "
     "negative 64-bit factor is only negative at 64 bits",
     """\
    mov rax, 0xFFFFFFFFFFFFFFFF
    mov rcx, 0x100000000
    imul rcx
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
""")
case("muldiv", "div-64",
     "DIV r64 divides the 128-bit RDX:RAX dividend: a 64-bit quotient above 2^32 is "
     "only representable at 64 bits",
     """\
    mov rax, 0x1000000000000000
    xor edx, edx
    mov rcx, 0x10
    div rcx
""")
case("muldiv", "idiv-64",
     "IDIV r64 with a negative dividend: the 64-bit radix point is what makes the "
     "quotient negative",
     """\
    mov rax, 0xFFFFFFFFFFFFFFFF
    mov rdx, 0xFFFFFFFFFFFFFFFF
    mov rcx, 0x02
    idiv rcx
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
""", flags=True)
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
""", flags=True)
case("muldiv", "bsf-bsr-bswap",
     "BSF/BSR find bit 63 only in a 64-bit operand, and BSWAP reverses all 8 bytes",
     """\
    mov rax, 0x8000000000000000
    bsf rbx, rax
    bsr rcx, rax
    mov rdx, 0x1122334455667788
    bswap rdx
""", flags=True)
case("muldiv", "shld-shrd",
     "SHLD/SHRD concatenate TWO 64-bit operands: the shifted-in bits come from the "
     "other 64-bit register, not from its low half",
     """\
    mov rax, 0x8000000000000000
    mov rbx, 0x0000000000000001
    mov rcx, 1
    shld rax, rbx, cl
    shrd rbx, rax, cl
""", flags=True)

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
""", flags=True)
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
    out.append("[+] the PORT-TABLE census (how many of the 873-encoding dispatch table's "
               "entries have a long-mode arm, and how many of those have a case here) is "
               "the later slices': gen/x86_table64.js does not exist yet (S5), so 0 "
               "encodings have an arm and every case below is RED by construction")
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
