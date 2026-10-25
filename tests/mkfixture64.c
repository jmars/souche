/* tests/mkfixture64.c -- the x86-64 corpus's NATIVE leg (S4).
 *
 * The 32-bit oracle (tests/mkfixture.c) runs an ELF image under ptrace, breaks at the
 * `loop` symbol via a poked int3, and prints the gdb-extract-def JSON document.  This
 * file is that harness for the tests/nasm64 corpus, and it differs in exactly ONE way:
 *
 *     THE HARNESS IS THE LOADER.
 *
 * A flat 64-bit image cannot be exec'd (it is not an ELF, and the corpus deliberately
 * does not make it one -- results/x86-64-plan.md ss2.1.1 argues why the loader code
 * belongs in a test rig rather than in src/main.c's shipped load_elf), so the child
 * mmaps the flat image at its LINK BASE with MAP_FIXED_NOREPLACE, zero-fills the region
 * the image does not carry (the compared .bss window and the stack), and jumps straight
 * to the BODY.  It never executes the image's 32-bit stub: the stub exists for the
 * EMULATED leg (which must reach long mode itself), and skipping it here is what keeps
 * the native leg a one-line jump.
 *
 * WHAT IT THEREFORE CANNOT SHOW, stated so its silence is not read as a proof: the stub
 * is exercised by NOTHING on this leg (S5's job, when the port runs it), and the corpus's
 * cases are therefore compared on the BODY only.  That is also why the harness cannot
 * validate the stub's page tables: they are never read here.
 *
 * THE PTRACE LAYOUT is the host's x86-64 PTRACE_GETREGS/GETFPREGS (struct regs64/fpr64
 * below), which is the layout the 32-bit harness already reads (mkfixture.c:19-30 -- its
 * own header records that this host's compat ptrace returns the x86_64 layouts).  What is
 * NEW here is that R8-R15 and RIP are the real machine's: at 32 bits they were the host's
 * incidental values.
 *
 * THE DOCUMENT (format v2, results/x86-64-plan.md ss5.4) is 2124 elements:
 *
 *     [ 0.. 15]  the 16 GPRs, signed decimal of the 64-bit value, in the order
 *                cpu_state.h's reg64[] uses: rax rcx rdx rbx rsp rbp rsi rdi r8..r15
 *     [16]       rip
 *     [17.. 24]  st0..st7 (float, or "invalid")
 *     [25.. 40]  mm0..mm7, two 32-bit words each (the low 64 bits of PHYSICAL st(i))
 *     [41.. 72]  xmm0..xmm7, four 32-bit words each
 *     [73..2120] the 8 KiB window at 0x100000, 32-bit words, signed
 *     [2121]     rflags
 *     [2122]     the x87 tag word (the kernel's convert_from_fxsr form, as mkfixture.c)
 *     [2123]     the x87 status word
 *
 * The FPU half is mkfixture.c's code VERBATIM (its two measured quirks -- logical
 * st_space slots vs the physical abridged FTW, and the status word being a plain 16-bit
 * register -- are findings, not style, and copying them is the point).
 *
 * usage: mkfixture64 IMAGE BODY_ADDR LOOP_ADDR
 *        both addresses are absolute; tests/nasm64_run.py reads them from `nm` on the
 *        image's ELF (they are fixed by tests/nasm64.ld, but a case may add data).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>

/* the corpus geometry -- tests/nasm64.ld and gen/nasm64_gen.py's constants */
#define LOAD_BASE 0x80000UL   /* the flat image's link base (the multiboot header) */
#define MAP_END 0x103000UL    /* one page past the stack top 0x102000: .bss + stack */
#define MAX_IMAGE (MAP_END - LOAD_BASE)
#define BSS_START 0x100000UL
#define BSS_END 0x102000UL

/* THE ABOVE-4-GIBIT ALIAS (results/x86-64-addrwidth.md).  The emulated leg's stub maps
 * linear 0x100000000 onto physical 0x100000 -- the compared window itself -- so a corpus case
 * whose MEMORY OPERAND is above 4 GiB writes bytes the document already compares.  The host
 * has no such mapping, and without one such a case would SIGSEGV here (which the comparison
 * would read as a #GP) and prove nothing about the port.  So the image lives in a SHARED
 * memfd mapped TWICE: at the link base, and again at HIGH_BASE with the file offset that
 * makes host address HIGH_BASE + x BE host address BSS_START + x.  THE WINDOW IS A PAGE TABLE
 * RANGE, NOT THE WHOLE IMAGE: gen/nasm64_gen.py's PT2 identity-maps physical
 * 0x100000..0x108000 (8 x 4 KiB), and the alias covers exactly that.
 *
 * MEASURED, the reason this exists: the reviewer's sweep found that no corpus case had ever
 * used the generator's HIGH_BASE window, so the corpus could not catch ANY narrowing of a
 * 64-bit effective address -- the class that produced the setcc blocker and the fxsave one. */
#define HIGH_BASE 0x100000000UL
#define HIGH_LEN 0x8000UL                          /* PT2: physical 0x100000..0x108000 */
#define HIGH_FILE_OFF (BSS_START - LOAD_BASE)      /* 0x80000: address 0x100000's offset */
#define HIGH_ALIAS_FILE_END (HIGH_FILE_OFF + HIGH_LEN)

struct regs64 {
	unsigned long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi,
	              rdi, orig_rax, rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};
struct fpr64 {
	unsigned short cwd, swd, ftw, fop;
	unsigned long rip, rdp;
	unsigned mxcsr, mxcsr_mask;
	unsigned st_space[32];   /* 8 regs x 16B, fxsave layout */
	unsigned xmm_space[64];  /* 16 regs x 16B */
	unsigned padding[24];
};

/* this kernel's PEEKTEXT goes EIO on the last word of a mapping and misreads at
   non-aligned addresses; process_vm_readv has neither problem (mkfixture.c:32-42) */
static void peek(pid_t pid, unsigned long addr, void *buf, size_t n)
{
	struct iovec local = { buf, n };
	struct iovec remote = { (void *)addr, n };
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
	long double ld = 0.0L;
	memcpy(&ld, st, 10);
	double d = (double)ld;
	/* gdb prints these unquoted; nasm64_run.py string-replaces them before parsing */
	if (d != d) printf("nan,\n");
	else if (d == (double)1.0 / 0.0) printf("inf,\n");
	else if (d == (double)-1.0 / 0.0) printf("-inf,\n");
	else printf("    %.100e,\n", d);
}

/* The trampoline: set the stack top, zero every GPR the document compares, and jump.
 * It never returns.  The entry address goes through a MEMORY operand on purpose: a
 * register operand may be allocated to a register the trampoline itself zeroes, and the
 * first version of this file jumped to 0 for exactly that reason (a plain `xor %rax`
 * before `jmp *%rax`).  r11 is the only register left holding anything, and it is
 * clobbered last. */
static void jump_to_body(unsigned long entry)
{
	static volatile unsigned long tramp_entry;
	tramp_entry = entry;
	__asm__ volatile(
	    "movq %0, %%r11\n\t"
	    "movq %1, %%rsp\n\t"
	    "xorl %%eax, %%eax\n\t"
	    "xorl %%ecx, %%ecx\n\t"
	    "xorl %%edx, %%edx\n\t"
	    "xorl %%ebx, %%ebx\n\t"
	    "xorl %%ebp, %%ebp\n\t"
	    "xorl %%esi, %%esi\n\t"
	    "xorl %%edi, %%edi\n\t"
	    "xorl %%r8d, %%r8d\n\t"
	    "xorl %%r9d, %%r9d\n\t"
	    "xorl %%r10d, %%r10d\n\t"
	    "xorl %%r12d, %%r12d\n\t"
	    "xorl %%r13d, %%r13d\n\t"
	    "xorl %%r14d, %%r14d\n\t"
	    "xorl %%r15d, %%r15d\n\t"
	    /* the FPU state is part of the compared document, so it must be DEFINED before
	     * the body runs: there is no exec here, so the child inherits the parent's x87
	     * and SSE registers -- and the parent used memcpy, which leaves the xmm registers
	     * holding junk (MEASURED: the first version of this file reported a body's
	     * xmm0-15 as the memcpy's leftovers).
	     *
	     * S5a FINDING (the second half of the same defect): `fninit` ALONE IS NOT ENOUGH,
	     * because it only sets the TAG to empty -- it does NOT clear the eight 80-bit
	     * register slots.  The document prints mm0-mm7 as the low 64 bits of the PHYSICAL
	     * stack slots and the comparison's MMX branch fires whenever the tag says
	     * all-invalid, so the native leg was reporting the PARENT'S x87 residue as the
	     * body's MMX result.  MEASURED: the first family-1 comparison failed on exactly one
	     * field per case -- mm7.int32[1] = 0xFFFFFFFF80000000 on the native side against the
	     * model's 0 -- for all twelve cases, i.e. a constant that had nothing to do with any
	     * body.  The fix is to zero the DATA as well: FXSAVE the area, zero st_space and
	     * xmm_space (offsets 0x20..0x1A0), FXRSTOR it back.  The scratch area is 512 bytes
	     * just below BSS_END, which is OUTSIDE the compared 8 KiB window (0x100000..
	     * 0x101FFF) and inside the harness's own mapping.
	     *
	     * v86's initial state is exactly this: every fpu_st slot is F80_ZERO and the tag is
	     * all-empty (cpu_state.c reset_cpu), so the zeroed data is the MODEL's state, not a
	     * convenience. */
	    /* the FXSAVE scratch: 16-byte aligned, inside the harness's mapping (which ends at
	     * 0x103000) and ABOVE the compared window (which ends at the stack top, 0x102000) --
	     * a scratch at rsp-512 lands INSIDE the window and changes the document (MEASURED:
	     * the window then carried MXCSR at 0x101E18) */
	    "movl $0x102400, %%edi\n\t"
	    "fxsave (%%rdi)\n\t"
	    "leaq 32(%%rdi), %%rdi\n\t"
	    "xorl %%eax, %%eax\n\t"
	    "movl $48, %%ecx\n\t"          /* 48 quadwords = 0x180 bytes: 0x20..0x1A0 */
	    "rep stosq\n\t"
	    "movl $0x102400, %%edi\n\t"
	    "fxrstor (%%rdi)\n\t"
	    "fninit\n\t"
	    "pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\t"
	    "pxor %%xmm2, %%xmm2\n\tpxor %%xmm3, %%xmm3\n\t"
	    "pxor %%xmm4, %%xmm4\n\tpxor %%xmm5, %%xmm5\n\t"
	    "pxor %%xmm6, %%xmm6\n\tpxor %%xmm7, %%xmm7\n\t"
	    "pxor %%xmm8, %%xmm8\n\tpxor %%xmm9, %%xmm9\n\t"
	    "pxor %%xmm10, %%xmm10\n\tpxor %%xmm11, %%xmm11\n\t"
	    "pxor %%xmm12, %%xmm12\n\tpxor %%xmm13, %%xmm13\n\t"
	    "pxor %%xmm14, %%xmm14\n\tpxor %%xmm15, %%xmm15\n\t"
	    "jmp *%%r11\n\t"
	    :: "m"(tramp_entry), "i"(BSS_END)
	    : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
	      "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "memory");
}

/* ------------------------------------------------------------------ *
 * THE ABRIDGED FTW: MEASURED, NOT ASSUMED (round 2, fix 2)
 *
 * The tag word the comparison's mm/st gate keys on is built from the fxsave abridged FTW
 * field -- and that field's POLARITY is a documented AMD/Intel disagreement, so this
 * harness CALIBRATES it once and names the convention it found instead of assuming one.
 *
 * MEASURED on this host (Ryzen 9 3950X, plain `fxsave` in-process and again through the
 * shipped harness on the shipped control image, four states each):
 *
 *   state                               swd     top  ftw    the registers holding values
 *   fninit                              0x0000  0    0x00   none
 *   fninit; fld1                        0x3800  7    0x80   physical 7 (logical st0)
 *   fninit; fld1; fld1; fld1            0x2800  5    0xe0   physical 5, 6, 7
 *   fninit; fld1; fstp st(0)            0x0000  0    0x00   none (stale slots 6,7)
 *   fninit; movq mm0, [mem]             0x0000  0    0xff   every register marked
 *
 * i.e. on THIS host a set bit means the physical register is PRESENT (AMD's reading of
 * the abridged FTW is the valid mask; Intel documents the inverse).  The calibration
 * below measures which convention is in force -- four are possible and the byte says
 * which -- and only a byte matching none of them makes the harness fall back, LOUDLY, to
 * the value-derived rule.
 *
 * THE FIRST VERSION OF THIS FILE GOT THIS WRONG and the record has to say so: it read the
 * field with Intel's polarity, concluded from 0x00-that-meant-"all present" that the field
 * was dead, and derived the tag from the values with a "a zeroed slot is untouched" rule.
 * The consequence was measurable and wrong (results/x86-64-s4.md ss7.2): a case whose x87
 * stack had been emptied still reported two registers live, and the comparison's MMX
 * branch did not fire for it.
 * ------------------------------------------------------------------ */
static int ftw_set_means_present;   /* +1 = a set bit is PRESENT, 0 = a set bit is EMPTY */
static int ftw_index_is_physical;   /* +1 = bit p is physical register p, 0 = logical st p */
static int ftw_is_dead;             /* the field carried no information; values are used */

static void calibrate_ftw(void)
{
	struct fpr64 probe;
	/* fninit; fld1 -- exactly ONE register present: physical 7, logical st(0) */
	__asm__ volatile("fninit\n\tfld1" ::: "st");
	memset(&probe, 0, sizeof(probe));
	__asm__ volatile("fxsave %0" : "=m"(probe) : : "memory");

	unsigned byte = probe.ftw & 0xFF;
	if (byte == 0x80) { ftw_set_means_present = 1; ftw_index_is_physical = 1; }
	else if (byte == 0x7F) { ftw_set_means_present = 0; ftw_index_is_physical = 1; }
	else if (byte == 0x01) { ftw_set_means_present = 1; ftw_index_is_physical = 0; }
	else if (byte == 0xFE) { ftw_set_means_present = 0; ftw_index_is_physical = 0; }
	else {
		ftw_is_dead = 1;
		fprintf(stderr, "mkfixture64: WARNING: the fxsave abridged FTW is 0x%02x after"
		                " `fninit; fld1`, which is none of the four conventions this"
		                " harness knows (0x80/0x7f physical, 0x01/0xfe logical); the tag"
		                " word is therefore derived from the VALUES (a slot holding exactly"
		                " 0.0 is read as untouched) -- x87 cases that compute exactly 0.0"
		                " are then skipped.  results/x86-64-s4.md ss7.2\n", byte);
		return;
	}
	fprintf(stderr, "mkfixture64: abridged FTW calibrated: byte 0x%02x after `fninit;"
	                " fld1` -> bit set means %s, indexed by %s\n",
	        byte, ftw_set_means_present ? "PRESENT" : "EMPTY",
	        ftw_index_is_physical ? "PHYSICAL register" : "LOGICAL st");
}

/* is physical register p holding a value, according to the calibrated convention? */
static int register_present(unsigned ftw, int phys, int top)
{
	int bit = ftw_index_is_physical ? phys : ((phys - top) & 7);
	int set = (int)((ftw >> bit) & 1);
	return ftw_set_means_present ? set : !set;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s IMAGE BODY_ADDR LOOP_ADDR\n", argv[0]);
		return 2;
	}
	unsigned long body_addr = strtoul(argv[2], 0, 0);
	unsigned long loop_addr = strtoul(argv[3], 0, 0);

	/* the image is read BEFORE the fork: the child must not do file I/O while traced */
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	static unsigned char image[MAX_IMAGE];
	ssize_t image_size = read(fd, image, sizeof(image));
	close(fd);
	if (image_size <= 0) { perror("read"); return 1; }
	if ((unsigned long)image_size == sizeof(image)) {
		fprintf(stderr, "%s: image is not smaller than %lu bytes\n", argv[1],
		        (unsigned long)sizeof(image));
		return 1;
	}
	/* the addresses the runner read from the image's ELF must land inside what this
	   harness can map -- a wrong link base is a harness error, never a test result */
	if (body_addr < LOAD_BASE || body_addr >= MAP_END || loop_addr < LOAD_BASE ||
	    loop_addr >= MAP_END) {
		fprintf(stderr, "body 0x%lx / loop 0x%lx outside the mapped [0x%lx,0x%lx)\n",
		        body_addr, loop_addr, LOAD_BASE, MAP_END);
		return 1;
	}

	calibrate_ftw();     /* BEFORE the fork: the child inherits a defined FPU state and
	                      * the trampoline re-does fninit anyway */

	/* the shared object the child maps twice -- see HIGH_BASE's comment.  Created BEFORE the
	 * fork: the child must not do syscalls it does not have to while traced. */
	int mfd = memfd_create("nasm64-image", 0);
	if (mfd < 0) { perror("memfd_create"); return 1; }
	if (ftruncate(mfd, HIGH_ALIAS_FILE_END) < 0) { perror("ftruncate"); return 1; }

	pid_t pid = fork();
	if (pid == 0) {
		void *mapped = mmap((void *)LOAD_BASE, MAX_IMAGE, PROT_READ | PROT_WRITE | PROT_EXEC,
		                    MAP_SHARED | MAP_FIXED_NOREPLACE, mfd, 0);
		if (mapped != (void *)LOAD_BASE) {
			/* the plan's named fallback is the ELF64 branch in load_elf; until then
			   this must be LOUD, because everything downstream is meaningless */
			perror("mmap(MAP_FIXED_NOREPLACE at 0x80000)");
			_exit(127);
		}
		memcpy((void *)LOAD_BASE, image, (size_t)image_size);
		void *high = mmap((void *)HIGH_BASE, HIGH_LEN, PROT_READ | PROT_WRITE | PROT_EXEC,
		                  MAP_SHARED | MAP_FIXED_NOREPLACE, mfd, (off_t)HIGH_FILE_OFF);
		if (high != (void *)HIGH_BASE) {
			perror("mmap(MAP_FIXED_NOREPLACE at 0x100000000)");
			_exit(127);
		}
		if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) { perror("TRACEME"); _exit(127); }
		raise(SIGSTOP);      /* the parent pokes int3 at `loop` from here */
		jump_to_body(body_addr);
		_exit(127);          /* not reached: jump_to_body does not return */
	}

	int status;
	waitpid(pid, &status, 0);            /* the child's SIGSTOP */
	if (!WIFSTOPPED(status)) { fprintf(stderr, "child did not stop before the jump\n"); return 1; }

	/* PEEK/POKETEXT misbehave at non-4-byte-aligned addresses: read/write the
	   containing aligned word instead (mkfixture.c:87-94, same measured kernel) */
	unsigned long aligned = loop_addr & ~3UL;
	unsigned shift = (loop_addr & 3) * 8;
	long word = ptrace(PTRACE_PEEKTEXT, pid, (void *)aligned, 0);
	if (ptrace(PTRACE_POKETEXT, pid, (void *)aligned,
	           (word & ~(0xFFL << shift)) | (0xCCL << shift)) < 0) {
		perror("POKETEXT");
		return 1;
	}

	const char *signal_name = NULL;
	struct regs64 r;
	memset(&r, 0, sizeof(r));
	for (;;) {
		if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) { perror("CONT"); return 1; }
		waitpid(pid, &status, 0);
		if (WIFEXITED(status)) {
			fprintf(stderr, "the body exited (status %d) without reaching `loop`\n",
			        WEXITSTATUS(status));
			return 1;
		}
		if (!WIFSTOPPED(status)) { fprintf(stderr, "killed sig=%d\n", WTERMSIG(status)); return 1; }
		int sig = WSTOPSIG(status);
		if (ptrace(PTRACE_GETREGS, pid, 0, &r) < 0) { perror("GETREGS"); return 1; }

		if (sig == SIGTRAP && r.rip == loop_addr + 1) {
			r.rip -= 1;                       /* the breakpoint is not part of the state */
			ptrace(PTRACE_SETREGS, pid, 0, &r);
			break;
		}
		/* SIGTRAP anywhere else is a real `int3` in the body (or a stray one): report it
		   as the reference's exception set has no name for it, so it must be loud */
		if (sig == SIGTRAP) {
			fprintf(stderr, "stray SIGTRAP at rip 0x%lx (the loop breakpoint is 0x%lx)\n",
			        r.rip, loop_addr + 1);
			return 1;
		}
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

	/* the two measured quirks of this kernel's fxsave->i387 conversion, verbatim from
	   mkfixture.c:123-146 (logical st_space slots with a physical abridged FTW; the
	   status word is a plain 16-bit register) */
	int top = (f.swd >> 11) & 7;
	unsigned ftag = 0xffff0000u;
	for (int p = 0; p < 8; p++) {
		/* THE TAG FROM THE FIELD THE HARDWARE MAINTAINS (round 2, fix 2): bit p of the
		 * abridged FTW says whether PHYSICAL register p holds a value -- in the polarity
		 * calibrate_ftw() measured on this host -- and a register that holds nothing is
		 * the comparison's all-invalid sentinel (2), whatever its stale slot bytes say.
		 * That is the architecture's own answer: after `fninit; fld1; fstp st(0)` the
		 * slot still contains 1.0 but the register is EMPTY, and a tag word that called
		 * it live would compare residue the guest cannot observe.
		 *
		 * A slot that IS present is classified from its value, the kernel's
		 * twd_fxsr_to_i387 rule (2 = special/inf/NaN/unnormal, 1 = zero, 0 = valid), so
		 * an x87 case's values are compared normally -- including a computed 0.0, which
		 * the previous value-derived rule could not distinguish from "untouched". */
		int tag;
		if (!register_present(f.ftw, p, top)) {
			tag = 2;
		} else if (ftw_is_dead) {
			tag = tag_of((const unsigned char *)&f.st_space[4 * ((p - top) & 7)]);
			if (tag == 1) tag = 2;
		} else {
			tag = tag_of((const unsigned char *)&f.st_space[4 * ((p - top) & 7)]);
		}
		ftag |= (unsigned)tag << (2 * p);
	}
	unsigned fstat = f.swd;

	if (signal_name) printf("(signal %s)\n", signal_name);
	printf("---BEGIN JSON---\n[\n");
	/* the 16 GPRs, signed decimal of the 64-bit value, reg64[]'s order */
	printf("    %lld,\n", (long long)r.rax);
	printf("    %lld,\n", (long long)r.rcx);
	printf("    %lld,\n", (long long)r.rdx);
	printf("    %lld,\n", (long long)r.rbx);
	printf("    %lld,\n", (long long)r.rsp);
	printf("    %lld,\n", (long long)r.rbp);
	printf("    %lld,\n", (long long)r.rsi);
	printf("    %lld,\n", (long long)r.rdi);
	printf("    %lld,\n", (long long)r.r8);
	printf("    %lld,\n", (long long)r.r9);
	printf("    %lld,\n", (long long)r.r10);
	printf("    %lld,\n", (long long)r.r11);
	printf("    %lld,\n", (long long)r.r12);
	printf("    %lld,\n", (long long)r.r13);
	printf("    %lld,\n", (long long)r.r14);
	printf("    %lld,\n", (long long)r.r15);
	printf("    %lld,\n", (long long)r.rip);
	/* THE DOCUMENT'S st0..st7 ARE LOGICAL (the order v86's fpu_get_sti_f64 uses and the
	 * order the 32-bit document is in), and SO IS THIS LEG'S st_space: slot i holds
	 * logical st(i).  MEASURED, twice, and it is mkfixture.c's convention too -- after
	 * `fninit; fld1` the status word's top is 7 and the 1.0 sits in slot 0 (its logical
	 * position), while physical register 7 is the one the FTW marks present; with top 0
	 * after the pops, a stale 6.0 sits in slot 7.  So logical st(j) lives in slot j, and
	 * PHYSICAL register p lives in logical slot (p - top) & 7 -- which is exactly what
	 * the tag lookup and the mm loop below use.
	 *
	 * (A earlier version of this comment said the opposite -- "slot i is physical
	 * register i" -- which was wrong and would have invited a breaking "fix": the code
	 * was right.  Corrected in round 2, fix 3.) */
	for (int j = 0; j < 8; j++) {
		int phys = (top + j) & 7;
		if (((ftag >> (2 * phys)) & 3) == 2) printf("    \"invalid\",\n");
		else print_st((const unsigned char *)&f.st_space[4 * j]);
	}
	for (int i = 0; i < 8; i++) {           /* mm i = low 64 bits of PHYSICAL st(i) */
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
			unsigned w;
			memcpy(&w, mem + a + 4 * b, 4);
			printf("%d, ", (int)w);
		}
		printf("\n");
	}
	printf("    %lld,\n", (long long)r.eflags);
	printf("    %d,\n", (int)ftag);
	printf("    %d\n", (int)fstat);
	printf("]\n---END JSON---\n");
	return 0;
}
