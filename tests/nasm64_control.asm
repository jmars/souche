# tests/nasm64_control.asm -- HAND-WRITTEN, NOT generated (S4's gate item (c)).
#
# THIS IS THE HARNESS'S OWN CONTROL, and it is the only image in the 64-bit lane whose
# expected state is computed INDEPENDENTLY of the harness: tests/nasm64_run.py --stage
# control rebuilds every compared value here from the constants in THIS file (the 16
# quadwords at 0x100000, the eight 0xA5.. quads at 0x100200, the sixteen GPRs, and the
# flag word) and requires the ptrace document to match it.  A harness that never hits the
# `loop` breakpoint, never reads the 8 KiB window, or never reads the FPU state cannot
# pass it -- which is the point: a dead harness must not be mistakable for a failing
# subject.
#
# It is NOT part of the corpus (it is not in tests/nasm64/): the corpus's cases are
# generated from gen/nasm64_gen.py's table, and this one's value is that it is not.
#
# The .stub section below carries ONLY the multiboot header: its job here is to pin the
# flat image's link base at 0x80000 so tests/mkfixture64.c's LOAD_BASE is right.  The
# native leg jumps straight to the body and never executes a stub; there is no code here
# for the emulated leg to run (this image is native-only by design).
.intel_syntax noprefix

.section .stub, "ax"
.code32
.balign 16
mb_magic:
    .long 0x1BADB002
mb_flags:
    .long 0x00010000
mb_checksum:
    .long -(0x1BADB002 + 0x10000)
mb_header_addr:
    .long mb_magic
mb_load_addr:
    .long mb_magic
mb_load_end:
    .long 0
mb_bss_end:
    .long 0
mb_entry:
    .long 0                          # no stub: the native leg jumps to the body

.section .body, "ax"
.code64
.globl body_start
body_start:
    # the same prologue the generated cases use: every compared GPR at zero, the stack
    # top at the window's top, and the flags clear
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
    mov rsp, 0x102000
    push 0
    popfq

    # the 8 KiB window, part 1: 16 quadwords = 0x1000000000000000 + i at 0x100000 + 8*i.
    # These are 64-bit stores, so the window's word PAIRS are what the model predicts.
    mov rax, 0x1000000000000000
    mov rdi, 0x100000
    mov ecx, 16
.fill:
    mov qword ptr [rdi], rax
    inc rax
    add rdi, 8
    dec ecx
    jnz .fill

    # the window, part 2: eight 0xA5A5A5A5A5A5A5A5 quadwords at 0x100200 via REP STOSQ
    mov rdi, 0x100200
    mov rax, 0xA5A5A5A5A5A5A5A5
    mov ecx, 8
    rep stosq

    # the compared register file: two known 64-bit values, everything else zero
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
    mov rax, 0x0123456789ABCDEF
    mov rcx, 0xFEDCBA9876543210

    # the x87 path: ONE value, so the FPU half of the document has a prediction the model
    # can state exactly (fninit leaves top=0, so `fld1` lands 1.0 in PHYSICAL register 7
    # and leaves the other seven slots zeroed -- no stale slot, no arithmetic to model)
    fld1

    # the compared flag word: CF set on top of the last XOR's ZF|PF
    stc

loop:
    hlt
    jmp loop
