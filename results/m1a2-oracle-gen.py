SHIFT_OPS = [("rol","rol"),("ror","ror"),("rcl","rcl"),("rcr","rcr"),
             ("shl","shl"),("shr","shr"),("sar","sar")]
WIDTHS = [(8,"b","uint8_t"),(16,"w","uint16_t"),(32,"l","uint32_t")]
DUAL_OPS = [("shld","shld"),("shrd","shrd")]
DUAL_WIDTHS = [(16,"w","uint16_t"),(32,"l","uint32_t")]
out=[]; w=out.append

w('#include <stdio.h>')
w('#include <stdint.h>')
w('#include <string.h>')
w('#include <signal.h>')
w('#include <setjmp.h>')
w('#include "cpu_state.h"')
w('#include "flags.h"')
w('#include "memory.h"')
w('#include "paging.h"')
w('#include "arith.h"')
w('#include "modrm.h"')
w('#include "misc_instr.h"')
w('')
w('int32_t last_vector = -1;')
w('void call_interrupt_vector(int32_t interrupt, bool is_int, int32_t error_code)')
w('{ (void)is_int; (void)error_code; last_vector = interrupt; }')
w('void trigger_de(void) { last_vector = CPU_EXCEPTION_DE; }')
w('void trigger_ud(void) { last_vector = CPU_EXCEPTION_UD; }')
w('void trigger_nm(void) { last_vector = CPU_EXCEPTION_NM; }')
w('void trigger_gp(int32_t c) { (void)c; last_vector = CPU_EXCEPTION_GP; }')
w('bool switch_seg(int32_t reg, int32_t raw) { (void)reg; (void)raw; return false; }')
w('')
w('#define FCF(f) ((int)((f) >> 0 & 1))')
w('#define FPF(f) ((int)((f) >> 2 & 1))')
w('#define FAF(f) ((int)((f) >> 4 & 1))')
w('#define FZF(f) ((int)((f) >> 6 & 1))')
w('#define FSF(f) ((int)((f) >> 7 & 1))')
w('#define FOF(f) ((int)((f) >> 11 & 1))')
w('')

for op, instr in SHIFT_OPS:
    for bits, suf, ctype in WIDTHS:
        for cnt in range(0, bits):
            w(f'static void hw_{op}{bits}_{cnt}({ctype} v, int cf_in, {ctype} *res, uint64_t *fl)')
            w('{')
            w(f'    {ctype} r = v; uint64_t f;')
            w('    __asm__ volatile("movq $2, %%r11\\n\\t"')
            w('                     "orq %[cin], %%r11\\n\\t"')
            w('                     "pushq %%r11\\n\\t" "popfq\\n\\t"')
            w(f'                     "{instr}{suf} ${cnt}, %[r]\\n\\t"')
            w('                     "pushfq\\n\\t" "popq %[f]\\n\\t"')
            w('                     : [r] "+r"(r), [f] "=&r"(f)')
            w('                     : "0"(v), [cin] "r"((uint64_t)(cf_in & 1))')
            w('                     : "r11", "cc");')
            w('    *res = r; *fl = f;')
            w('}')
            w('')
for op, instr in DUAL_OPS:
    for bits, suf, ctype in DUAL_WIDTHS:
        for cnt in range(0, bits + 1):
            w(f'static void hw_{op}{bits}_{cnt}({ctype} v, {ctype} src, int cf_in, {ctype} *res, uint64_t *fl)')
            w('{')
            w(f'    {ctype} r = v; uint64_t f;')
            w('    __asm__ volatile("movq $2, %%r11\\n\\t"')
            w('                     "orq %[cin], %%r11\\n\\t"')
            w('                     "pushq %%r11\\n\\t" "popfq\\n\\t"')
            w(f'                     "{instr}{suf} ${cnt}, %[s], %[r]\\n\\t"')
            w('                     "pushfq\\n\\t" "popq %[f]\\n\\t"')
            w('                     : [r] "+r"(r), [f] "=&r"(f)')
            w('                     : "0"(v), [s] "r"(src), [cin] "r"((uint64_t)(cf_in & 1))')
            w('                     : "r11", "cc");')
            w('    *res = r; *fl = f;')
            w('}')
            w('')

# --- tables of function pointers (uniform per-width signatures) ---
w('typedef void (*hwfn8)(uint8_t v, int cf_in, uint8_t *res, uint64_t *fl);')
w('typedef void (*hwfn16)(uint16_t v, int cf_in, uint16_t *res, uint64_t *fl);')
w('typedef void (*hwfn32)(uint32_t v, int cf_in, uint32_t *res, uint64_t *fl);')
for bits, suf, ctype in WIDTHS:
    for op, instr in SHIFT_OPS:
        names = ", ".join(f"hw_{op}{bits}_{c}" for c in range(0, bits))
        w(f'static const hwfn{bits} hw_{op}{bits}_tab[] = {{ {names} }};')
    ops = ", ".join(f"hw_{op}{bits}_tab" for op, _ in SHIFT_OPS)
    w(f'static const hwfn{bits} *const hw_shift{bits}[] = {{ {ops} }};')
w('')
# dual: 16/32 x counts 0..bits, with src
w('typedef void (*hwd16)(uint16_t v, uint16_t src, int cf_in, uint16_t *res, uint64_t *fl);')
w('typedef void (*hwd32)(uint32_t v, uint32_t src, int cf_in, uint32_t *res, uint64_t *fl);')
for bits, suf, ctype in DUAL_WIDTHS:
    for op, instr in DUAL_OPS:
        names = ", ".join(f"hw_{op}{bits}_{c}" for c in range(0, bits + 1))
        w(f'static const hwd{bits} hw_{op}{bits}_tab[] = {{ {names} }};')
    ops = ", ".join(f"hw_{op}{bits}_tab" for op, _ in DUAL_OPS)
    w(f'static const hwd{bits} *const hw_dual{bits}[] = {{ {ops} }};')
w('')
open('/tmp/m1a2/hw_gen.h','w').write("\n".join(out)+"\n")

# --- mul/imul/div/idiv/bcd/bit-op hardware helpers (hand-written fragment) ---
tail = r'''
/* ---- mul / imul ---- */
static void hw_mul8(uint8_t a, uint8_t b, uint16_t *res, uint64_t *fl) {
    uint16_t r; uint64_t f;
    __asm__ volatile("mulb %[b]\n\t pushfq\n\t popq %[f]\n\t"
                     : "=a"(r), [f] "=&r"(f) : "a"(a), [b] "r"(b) : "cc");
    *res = r; *fl = f;
}
static void hw_imul8(uint8_t a, int8_t b, int16_t *res, uint64_t *fl) {
    int16_t r; uint64_t f;
    __asm__ volatile("imulb %[b]\n\t pushfq\n\t popq %[f]\n\t"
                     : "=a"(r), [f] "=&r"(f) : "a"(a), [b] "r"(b) : "cc");
    *res = r; *fl = f;
}
static void hw_mul16(uint16_t a, uint16_t b, uint16_t *lo, uint16_t *hi, uint64_t *fl) {
    uint16_t l, h; uint64_t f;
    __asm__ volatile("mulw %[b]\n\t pushfq\n\t popq %[f]\n\t"
                     : "=a"(l), "=d"(h), [f] "=&r"(f) : "a"(a), [b] "r"(b) : "cc");
    *lo = l; *hi = h; *fl = f;
}
static void hw_imul16(uint16_t a, int16_t b, uint16_t *lo, uint16_t *hi, uint64_t *fl) {
    uint16_t l, h; uint64_t f;
    __asm__ volatile("imulw %[b]\n\t pushfq\n\t popq %[f]\n\t"
                     : "=a"(l), "=d"(h), [f] "=&r"(f) : "a"(a), [b] "r"(b) : "cc");
    *lo = l; *hi = h; *fl = f;
}
static void hw_mul32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi, uint64_t *fl) {
    uint32_t l, h; uint64_t f;
    __asm__ volatile("mull %[b]\n\t pushfq\n\t popq %[f]\n\t"
                     : "=a"(l), "=d"(h), [f] "=&r"(f) : "a"(a), [b] "r"(b) : "cc");
    *lo = l; *hi = h; *fl = f;
}
static void hw_imul32(uint32_t a, int32_t b, uint32_t *lo, uint32_t *hi, uint64_t *fl) {
    uint32_t l, h; uint64_t f;
    __asm__ volatile("imull %[b]\n\t pushfq\n\t popq %[f]\n\t"
                     : "=a"(l), "=d"(h), [f] "=&r"(f) : "a"(a), [b] "r"(b) : "cc");
    *lo = l; *hi = h; *fl = f;
}
static void hw_imul16_2(uint16_t a, uint16_t b, uint16_t *res, uint64_t *fl) {
    uint16_t r = a; uint64_t f;
    __asm__ volatile("imulw %[b], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "+r"(r), [f] "=&r"(f) : [b] "r"(b) : "cc");
    *res = r; *fl = f;
}
static void hw_imul32_2(uint32_t a, uint32_t b, uint32_t *res, uint64_t *fl) {
    uint32_t r = a; uint64_t f;
    __asm__ volatile("imull %[b], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "+r"(r), [f] "=&r"(f) : [b] "r"(b) : "cc");
    *res = r; *fl = f;
}

/* ---- div / idiv, with #DE captured as "returns 0" ---- */
static sigjmp_buf fpe_env;
static void fpe_handler(int sig) { (void)sig; siglongjmp(fpe_env, 1); }
static int hw_div8(uint16_t target, uint8_t src, uint8_t *q, uint8_t *r) {
    /* divb leaves the QUOTIENT in AL and the REMAINDER in AH -- both halves of AX */
    uint16_t ax = target;
    if (sigsetjmp(fpe_env, 1)) { return 0; }
    __asm__ volatile("divb %[s]" : "+a"(ax) : [s] "r"(src) : "cc");
    *q = ax & 0xFF; *r = (ax >> 8) & 0xFF; return 1;
}
static int hw_idiv8(int16_t target, int8_t src, int8_t *q, int8_t *r) {
    int16_t ax = target;
    if (sigsetjmp(fpe_env, 1)) { return 0; }
    __asm__ volatile("idivb %[s]" : "+a"(ax) : [s] "r"(src) : "cc");
    *q = (int8_t)(ax & 0xFF); *r = (int8_t)((ax >> 8) & 0xFF); return 1;
}
static int hw_div16(uint32_t target, uint16_t src, uint16_t *q, uint16_t *r) {
    uint32_t t = target; uint16_t qq = 0, rr = 0;
    if (sigsetjmp(fpe_env, 1)) { return 0; }
    __asm__ volatile("divw %[s]" : "=a"(qq), "=d"(rr) : "a"(t), "d"(t >> 16), [s] "r"(src) : "cc");
    *q = qq; *r = rr; return 1;
}
static int hw_idiv16(int32_t target, int16_t src, int16_t *q, int16_t *r) {
    int32_t t = target; int16_t qq = 0, rr = 0;
    if (sigsetjmp(fpe_env, 1)) { return 0; }
    __asm__ volatile("idivw %[s]" : "=a"(qq), "=d"(rr) : "a"(t), "d"(t >> 16), [s] "r"(src) : "cc");
    *q = qq; *r = rr; return 1;
}
static int hw_div32(uint64_t target, uint32_t src, uint32_t *q, uint32_t *r) {
    uint64_t t = target; uint32_t qq = 0, rr = 0;
    if (sigsetjmp(fpe_env, 1)) { return 0; }
    __asm__ volatile("divl %[s]" : "=a"(qq), "=d"(rr) : "a"((uint32_t)t), "d"((uint32_t)(t >> 32)), [s] "r"(src) : "cc");
    *q = qq; *r = rr; return 1;
}
static int hw_idiv32(int64_t target, int32_t src, int32_t *q, int32_t *r) {
    int64_t t = target; int32_t qq = 0, rr = 0;
    if (sigsetjmp(fpe_env, 1)) { return 0; }
    __asm__ volatile("idivl %[s]" : "=a"(qq), "=d"(rr) : "a"((uint32_t)t), "d"((uint32_t)(t >> 32)), [s] "r"(src) : "cc");
    *q = qq; *r = rr; return 1;
}

/* ---- BCD (raw opcode bytes: no GAS pseudo-mnemonic ambiguity) ---- */
#define BCD_FN(NAME, MNEMONIC)                                            \
static void NAME(uint8_t al, uint8_t ah, int cf_in, uint8_t *oal, uint8_t *oah, uint64_t *fl) { \
    uint16_t ax = (uint16_t)(((uint16_t)ah << 8) | al);                   \
    uint64_t f;                                                          \
    __asm__ volatile("movq $2, %%r11\n\t"                               \
                     "orq %[cin], %%r11\n\t"                             \
                     "pushq %%r11\n\t" "popfq\n\t"                     \
                     MNEMONIC "\n\t"                                     \
                     "pushfq\n\t popq %[f]\n\t"                          \
                     : "+a"(ax), [f] "=&r"(f)                            \
                     : [cin] "r"((uint64_t)(cf_in & 1))                  \
                     : "r11", "cc");                                     \
    *oal = (uint8_t)(ax & 0xFF); *oah = (uint8_t)(ax >> 8); *fl = f;      \
}
BCD_FN(hw_daa, ".byte 0x27")
BCD_FN(hw_das, ".byte 0x2f")
BCD_FN(hw_aaa, ".byte 0x37")
BCD_FN(hw_aas, ".byte 0x3f")
BCD_FN(hw_aam10, ".byte 0xd4, 0x0a")
BCD_FN(hw_aad10, ".byte 0xd5, 0x0a")
BCD_FN(hw_aam0, ".byte 0xd4, 0x00")

/* ---- bit test / scan ---- */
static void hw_bt_reg(uint32_t base, int off, int *cf) {
    uint32_t r = base; uint64_t f;
    __asm__ volatile("btl %[o], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "+r"(r), [f] "=&r"(f) : [o] "r"((uint32_t)off) : "cc");
    *cf = FCF(f);
}
static uint32_t hw_btc_reg(uint32_t base, int off, int *cf) {
    uint32_t r = base; uint64_t f;
    __asm__ volatile("btcl %[o], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "+r"(r), [f] "=&r"(f) : [o] "r"((uint32_t)off) : "cc");
    *cf = FCF(f); return r;
}
static uint32_t hw_bts_reg(uint32_t base, int off, int *cf) {
    uint32_t r = base; uint64_t f;
    __asm__ volatile("btsl %[o], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "+r"(r), [f] "=&r"(f) : [o] "r"((uint32_t)off) : "cc");
    *cf = FCF(f); return r;
}
static uint32_t hw_btr_reg(uint32_t base, int off, int *cf) {
    uint32_t r = base; uint64_t f;
    __asm__ volatile("btrl %[o], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "+r"(r), [f] "=&r"(f) : [o] "r"((uint32_t)off) : "cc");
    *cf = FCF(f); return r;
}
static void hw_bsr32(uint32_t v, uint32_t *res, int *zf) {
    uint32_t r = v; uint64_t f;
    __asm__ volatile("bsrl %[v], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "=&r"(r), [f] "=&r"(f) : [v] "r"(v) : "cc");
    *res = r; *zf = FZF(f);
}
static void hw_bsf32(uint32_t v, uint32_t *res, int *zf) {
    uint32_t r = v; uint64_t f;
    __asm__ volatile("bsfl %[v], %[r]\n\t pushfq\n\t popq %[f]\n\t"
                     : [r] "=&r"(r), [f] "=&r"(f) : [v] "r"(v) : "cc");
    *res = r; *zf = FZF(f);
}
'''
open('/tmp/m1a2/hw_gen.c','w').write("#include \"/tmp/m1a2/hw_gen.h\"\n"+tail)
print("generated")
