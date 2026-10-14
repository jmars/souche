/* gdb replacement for v86 nasm tests: run a 32-bit test ELF, stop at `loop`
 * (int3 breakpoint) or on SIGFPE/SIGILL/SIGSEGV/SIGBUS, and emit the same
 * JSON fixture as tests/nasm/gdb-extract-def.
 * This host's compat ptrace returns the x86_64 layouts for GETREGS/GETFPREGS;
 * the x87 tag word reproduces the kernel's convert_from_fxsr twd. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/uio.h>

#define BSS_START 0x100000
#define BSS_END   0x102000

struct regs64 {
    unsigned long r15,r14,r13,r12,rbp,rbx,r11,r10,r9,r8,rax,rcx,rdx,rsi,rdi,
                  orig_rax,rip,cs,eflags,rsp,ss,fs_base,gs_base,ds,es,fs,gs;
};
struct fpr64 {
    unsigned short cwd, swd, ftw, fop;
    unsigned long rip, rdp;
    unsigned mxcsr, mxcsr_mask;
    unsigned st_space[32];   /* 8 regs x 16B, fxsave layout */
    unsigned xmm_space[64];  /* 16 regs x 16B */
    unsigned padding[24];
};

/* this kernel's PEEKTEXT goes EIO on the last word of a mapping and misreads
   at non-aligned addresses; process_vm_readv has neither problem */
static void peek(pid_t pid, unsigned long addr, void *buf, size_t n)
{
    struct iovec local = { buf, n };
    struct iovec remote = { (void*)addr, n };
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != (ssize_t)n) {
        perror("process_vm_readv");
        exit(1);
    }
}

/* The kernel's twd_fxsr_to_i387 classification of one x87 register, from its
   80-bit image: 2 = special (inf/NaN/unnormal), 1 = zero, 0 = valid. */
static unsigned tag_of(const unsigned char st[16])
{
    unsigned exp = (st[8] | st[9] << 8) & 0x7fff;
    unsigned long long mant;
    memcpy(&mant, st, 8);
    if (exp == 0x7fff) return 2;
    if (exp == 0 && mant == 0) return 1;
    return 0;
}

static void print_st(const unsigned char st[16])
{
    /* fxsave 16B: [u64 mantissa][u16 sign+exp][u16 0] -> 80-bit extended */
    long double ld = 0.0L;
    memcpy(&ld, st, 10);
    double d = (double)ld;
    /* gdb prints these unquoted; run.js string-replaces them before parsing */
    if (d != d) printf("nan,\n");
    else if (d == (double)1.0 / 0.0) printf("inf,\n");
    else if (d == (double)-1.0 / 0.0) printf("-inf,\n");
    else printf("    %.100e,\n", d);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s IMAGE LOOP_ADDR\n", argv[0]); return 2; }
    unsigned long loop_addr = strtoul(argv[2], 0, 0);

    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execl(argv[1], argv[1], NULL);
        perror("execl");
        return 127;
    }
    int status;
    waitpid(pid, &status, 0);            /* initial SIGSTOP */
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &status, 0);            /* exec stop */

    /* this kernel's PEEK/POKETEXT misbehave at non-4-byte-aligned addresses:
       read/write the containing aligned word instead (verified: a poke at an
       odd address lands one byte late) */
    unsigned long aligned = loop_addr & ~3UL;
    unsigned shift = (loop_addr & 3) * 8;
    long word = ptrace(PTRACE_PEEKTEXT, pid, (void*)aligned, 0);
    if (ptrace(PTRACE_POKETEXT, pid, (void*)aligned,
               (word & ~(0xFFUL << shift)) | (0xCCUL << shift)) < 0) { perror("POKETEXT"); return 1; }


    const char *signal_name = NULL;
    struct regs64 r;
    memset(&r, 0, sizeof(r));
    for (;;) {
        ptrace(PTRACE_CONT, pid, 0, 0);
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) { fprintf(stderr, "exited code=%d\n", WEXITSTATUS(status)); return 1; }
        if (!WIFSTOPPED(status)) { fprintf(stderr, "killed sig=%d\n", WTERMSIG(status)); return 1; }
        int sig = WSTOPSIG(status);
        if (ptrace(PTRACE_GETREGS, pid, 0, &r) < 0) { perror("GETREGS"); return 1; }

        if (sig == SIGTRAP && r.rip == loop_addr + 1) { r.rip -= 1; ptrace(PTRACE_SETREGS, pid, 0, &r); break; }
        if (sig == SIGFPE)  { signal_name = "SIGFPE";  break; }
        if (sig == SIGILL)  { signal_name = "SIGILL";  break; }
        if (sig == SIGSEGV) { signal_name = "SIGSEGV"; break; }
        if (sig == SIGBUS)  { signal_name = "SIGBUS";  break; }
    }

    unsigned char mem[BSS_END - BSS_START];
    peek(pid, BSS_START, mem, sizeof(mem));

    struct fpr64 f;
    memset(&f, 0, sizeof(f));
    if (ptrace(PTRACE_GETFPREGS, pid, 0, &f) < 0) { perror("GETFPREGS"); return 1; }
    kill(pid, SIGKILL);

    /* The image ptrace hands back for a 32-bit tracee is the x86_64 fxsave layout
       (cwd/swd/ftw/fop, mxcsr, st_space at 0x20, xmm_space at 0xa0), but with two
       measured quirks this file has to honour:
         - st_space slot i holds LOGICAL st(i), the register the guest and gdb call
           st(i); the abridged FTW bit p is set for PHYSICAL register p.
           (Raw buffer + cross-leg `fnstenv` measurements: with top=6 a `fldpi`
           +`fld1` state has st0=1.0 in slot 0 and st1=pi in slot 1, while
           ftw=0xc0 marks physical 6 and 7; v86's fpu_get_sti_f64 agrees on the
           logical sequence.)
         - the status word is a plain 16-bit register: only the TAG word gets
           convert_from_fxsr's | 0xffff0000 (its low half is the physical-indexed
           full tag word, which twd_fxsr_to_i387 derives from the value at
           (p - top) & 7).
       Getting either wrong makes every FPU test report bogus st/fstat mismatches. */
    int top = (f.swd >> 11) & 7;
    unsigned ftag = 0xffff0000u;
    for (int p = 0; p < 8; p++) {
        unsigned tag = 3;
        if ((f.ftw >> p) & 1) {
            tag = tag_of((const unsigned char *)&f.st_space[4 * ((p - top) & 7)]);
        }
        ftag |= tag << (2 * p);
    }
    unsigned fstat = f.swd;

    if (signal_name) printf("(signal %s)\n", signal_name);
    printf("---BEGIN JSON---\n[\n");
    printf("    %d,\n", (int)(unsigned)r.rax);
    printf("    %d,\n", (int)(unsigned)r.rcx);
    printf("    %d,\n", (int)(unsigned)r.rdx);
    printf("    %d,\n", (int)(unsigned)r.rbx);
    printf("    %d,\n", (int)(unsigned)r.rsp);
    printf("    %d,\n", (int)(unsigned)r.rbp);
    printf("    %d,\n", (int)(unsigned)r.rsi);
    printf("    %d,\n", (int)(unsigned)r.rdi);
    printf("    %d,\n", (int)(unsigned)r.rip);
    for (int i = 0; i < 8; i++) {
        int phys = (top + i) & 7;
        if (((ftag >> (2 * phys)) & 3) == 2) printf("    \"invalid\",\n");
        else print_st((const unsigned char*)&f.st_space[4 * i]);
    }
    for (int i = 0; i < 8; i++) {           /* mm i = low 64 bits of PHYSICAL st(i) */
        /* st_space slot s holds LOGICAL st(s) = physical (top+s)&7 (above), so
           physical register i lives in slot (i - top) & 7.  The reference leg
           reads the physical registers (oracle/nasm_state.mjs:121, verbatim from
           run.js:376), and the port reads read_mmx64s(i) -> fpu_st[i]; with top !=
           0 the old slot-i read was logical order and would have disagreed. */
        int slot = (i - top) & 7;
        printf("    %d,\n", (int)f.st_space[4 * slot]);
        printf("    %d,\n", (int)f.st_space[4 * slot + 1]);
    }
    for (int i = 0; i < 8; i++) {           /* physical xmm0-7 */
        printf("    %d,\n", (int)f.xmm_space[4 * i]);
        printf("    %d,\n", (int)f.xmm_space[4 * i + 1]);
        printf("    %d,\n", (int)f.xmm_space[4 * i + 2]);
        printf("    %d,\n", (int)f.xmm_space[4 * i + 3]);
    }
    for (int a = 0; a < (int)sizeof(mem); a += 32) {
        printf("    ");
        for (int b = 0; b < 8; b++) {
            unsigned w; memcpy(&w, mem + a + 4 * b, 4);
            printf("%d, ", (int)w);
        }
        printf("\n");
    }
    printf("    %d,\n", (int)(unsigned)r.eflags);
    printf("    %d,\n", (int)ftag);
    printf("    %d\n", (int)fstat);
    printf("]\n---END JSON---\n");
    return 0;
}
