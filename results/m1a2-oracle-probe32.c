/* 32-bit (i386) freestanding probe: the BCD opcodes are #UD in long mode, so the only
 * hardware oracle for DAA/DAS/AAA/AAS/AAM/AAD is a 32-bit process.
 * Prints one row per (op, AL, AH, CF, AF): "daa al ah cf af | al ah cf af" (hex). */
static inline int sys_write(int fd, const char *b, int n)
{ int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(4), "b"(fd), "c"(b), "d"(n) : "memory"); return r; }
static inline void sys_exit(int c) { __asm__ volatile("int $0x80" : : "a"(1), "b"(c) : "memory"); }

static char out[65536];
static int opos;
static void cput(char c) { if (opos < 65000) out[opos++] = c; }
static void hx(unsigned v) { cput("0123456789abcdef"[(v >> 4) & 15]); cput("0123456789abcdef"[v & 15]); }

struct res { unsigned al, ah, cf, af; };

#define BCD_ARM(MNEM)                                                     \
    __asm__ volatile("pushl %[f]\n\t"                                     \
                     "popfl\n\t"                                          \
                     MNEM "\n\t"                                          \
                     "pushfl\n\t"                                         \
                     "popl %[fo]"                                         \
                     : "+a"(eax), [fo] "=r"(flags_in)                     \
                     : [f] "r"(flags_in)                                  \
                     : "cc")

static struct res run(int op, unsigned al, unsigned ah, unsigned cf, unsigned af)
{
    unsigned flags_in = 2 | cf | (af << 4);
    unsigned eax = (ah << 8) | al;
    struct res r;
    switch (op) {
    case 0: BCD_ARM("daa"); break;
    case 1: BCD_ARM("das"); break;
    case 2: BCD_ARM("aaa"); break;
    case 3: BCD_ARM("aas"); break;
    case 4: BCD_ARM("aam"); break;
    default: BCD_ARM("aad"); break;
    }
    r.al = eax & 0xFF; r.ah = (eax >> 8) & 0xFF;
    r.cf = flags_in & 1; r.af = (flags_in >> 4) & 1;
    return r;
}

static inline int sys_fork(void)
{ int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(2) : "memory"); return r; }
static inline int sys_wait4(int pid, int *st)
{ int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(114), "b"(pid), "c"(st), "d"(0), "S"(0) : "memory"); return r; }
static inline int sys_kill(int pid, int sig)
{ int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(37), "b"(pid), "c"(sig) : "memory"); return r; }

/* AAM with imm8 == 0 must raise #DE (division by zero).  Measured on the hardware via
 * fork: the child executes aam 0, the parent reports the wait status. */
static void probe_aam0(void)
{
    int st = 0;
    int pid = sys_fork();
    if (pid == 0) {
        unsigned eax = 0x0005, fi = 2;
        __asm__ volatile("pushl %[f]\n\tpopfl\n\t.byte 0xd4\n\t.byte 0x00\n\tpushfl\n\tpopl %[fo]"
                         : "+a"(eax), [fo] "=r"(fi) : [f] "r"(fi) : "cc");
        sys_exit(0);
    }
    sys_wait4(pid, &st);
    const char *msg = "aam0 #DE: ";
    sys_write(1, msg, 9);
    if ((st & 0x7f) == 8) sys_write(1, "SIGFPE", 6);
    else if ((st & 0x7f) == 0) sys_write(1, "exited", 6);
    else sys_write(1, "other", 5);
    sys_write(1, "\n", 1);
}

static const unsigned als[] = {0x00, 0x04, 0x09, 0x0A, 0x0F, 0x10, 0x60, 0x99, 0x9A, 0xA0, 0xFA};
static const unsigned ahs[] = {0x00, 0x03, 0x0A, 0x0F, 0xFF};

void _start(void)
{
    const char *names[6] = {"daa ", "das ", "aaa ", "aas ", "aam ", "aad "};
    for (int o = 0; o < 6; o++) {
        for (unsigned i = 0; i < sizeof(als) / sizeof(als[0]); i++) {
            for (unsigned j = 0; j < sizeof(ahs) / sizeof(ahs[0]); j++) {
                for (unsigned cf = 0; cf < 2; cf++) {
                    for (unsigned af = 0; af < 2; af++) {
                        struct res r = run(o, als[i], ahs[j], cf, af);
                        for (int k = 0; k < 4; k++) cput(names[o][k]);
                        hx(als[i]); cput(' '); hx(ahs[j]); cput(' ');
                        cput('0' + cf); cput(' '); cput('0' + af); cput(' ');
                        cput('|'); cput(' ');
                        hx(r.al); cput(' '); hx(r.ah); cput(' ');
                        cput('0' + r.cf); cput(' '); cput('0' + r.af); cput('\n');
                    }
                }
            }
        }
    }
    sys_write(1, out, opos);
    probe_aam0();
    sys_exit(0);
}
