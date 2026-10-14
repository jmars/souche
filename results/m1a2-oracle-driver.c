/* M1a-2 host-CPU oracle driver: compares src/cpu/arith.c against the host's own
 * x86-64 execution of the same instruction.  Not part of the build. */
#include "hw_gen.c"

static int mism = 0;
static long compared = 0;

static void bad(const char *what, unsigned v, int cnt, int cf_in, const char *field, long got, long want)
{
    mism++;
    printf("MISMATCH %-10s v=%08x src=%08x cnt=%2d cf_in=%d %-4s port=%ld hw=%ld\n",
           what, v, 0u, cnt, cf_in, field, got, want);
}
static void bad2(const char *what, unsigned v, unsigned src, int cnt, int cf_in, const char *field,
                 long got, long want)
{
    mism++;
    printf("MISMATCH %-10s v=%08x src=%08x cnt=%2d cf_in=%d %-4s port=%ld hw=%ld\n",
           what, v, src, cnt, cf_in, field, got, want);
}

static int32_t port_shift8(int op, int32_t v, int32_t count)
{
    switch (op) {
    case 0: return rol8(v, count);
    case 1: return ror8(v, count);
    case 2: return rcl8(v, count);
    case 3: return rcr8(v, count);
    case 4: return shl8(v, count);
    case 5: return shr8(v, count);
    default: return sar8(v, count);
    }
}
static int32_t port_shift16(int op, int32_t v, int32_t count)
{
    switch (op) {
    case 0: return rol16(v, count);
    case 1: return ror16(v, count);
    case 2: return rcl16(v, count);
    case 3: return rcr16(v, count);
    case 4: return shl16(v, count);
    case 5: return shr16(v, count);
    default: return sar16(v, count);
    }
}
static int32_t port_shift32(int op, int32_t v, int32_t count)
{
    switch (op) {
    case 0: return rol32(v, count);
    case 1: return ror32(v, count);
    case 2: return rcl32(v, count);
    case 3: return rcr32(v, count);
    case 4: return shl32(v, count);
    case 5: return shr32(v, count);
    default: return sar32(v, count);
    }
}

static const char *opname[7] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sar"};

static void set_in_flags(int cf_in)
{
    cpu->flags = FLAGS_DEFAULT | (cf_in ? FLAG_CARRY : 0);
    cpu->flags_changed = 0;
}

static const uint32_t vals8[] = {0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0x81, 0xFF};
static const uint32_t vals16[] = {0x0001, 0x000F, 0x00FF, 0x7FFF, 0x8000, 0x8001, 0xFFFF};
static const uint32_t vals32[] = {0x00000001u, 0x0000000Fu, 0x0F0F0F0Fu, 0x7FFFFFFFu,
                                  0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0x40000000u};
static const uint32_t valsd16[] = {0x0001, 0x8000, 0x1234, 0xFFFF};
static const uint32_t valsd32[] = {0x00000001u, 0x80000000u, 0x12345678u, 0xFFFFFFFFu};

/* rows: only for count in {1, width-1} (plus 2 for 8/16-bit), printed as C table rows */
static int emit_rows = 1;

static void run_shifts(void)
{
    for (int op = 0; op < 7; op++) {
        /* 8-bit */
        for (int cnt = 0; cnt < 8; cnt++) {
            for (unsigned k = 0; k < sizeof(vals8) / sizeof(vals8[0]); k++) {
                for (int cf_in = 0; cf_in < 2; cf_in++) {
                    uint8_t v = (uint8_t)vals8[k], hres = 0;
                    uint64_t fl = 0;
                    hw_shift8[op][cnt](v, cf_in, &hres, &fl);
                    set_in_flags(cf_in);
                    int32_t pres = port_shift8(op, v, cnt);
                    compared++;
                    if ((uint8_t)pres != hres)
                        bad(opname[op], v, cnt, cf_in, "res", (uint8_t)pres, hres);
                    if (cnt != 0) {
                        if (getcf() != FCF(fl))
                            bad(opname[op], v, cnt, cf_in, "getcf", getcf(), FCF(fl));
                        if (cnt == 1 && getof() != FOF(fl))
                            bad(opname[op], v, cnt, cf_in, "of", getof(), FOF(fl));
                        if (op >= 4) { /* shl/shr/sar define ZF/SF/PF */
                            if (getzf() != FZF(fl)) bad(opname[op], v, cnt, cf_in, "zf", getzf(), FZF(fl));
                            if (getsf() != FSF(fl)) bad(opname[op], v, cnt, cf_in, "sf", getsf(), FSF(fl));
                            if (getpf() != FPF(fl)) bad(opname[op], v, cnt, cf_in, "pf", getpf(), FPF(fl));
                        }
                        else { /* rotates leave ZF/SF/PF/AF unchanged */
                            if (getzf() != 0 || getsf() != 0 || getpf() != 0 || getaf() != 0)
                                bad(opname[op], v, cnt, cf_in, "unchanged",
                                    getzf() | getsf() << 1 | getpf() << 2 | getaf() << 3, 0);
                        }
                    }
                    if (emit_rows && (cnt == 1 || cnt == 7) && (v == 0x01 || v == 0x80 || v == 0xFF) && ((op == 2 || op == 3) || cf_in == 0))
                        printf("    {OP_%s, 8, 0x%02X, 0, %2d, %d, 0x%02X, %d, %d},\n", opname[op],
                               v, cnt, cf_in, hres, FCF(fl), (cnt == 1) ? FOF(fl) : -1);
                }
            }
        }
        /* 16-bit */
        for (int cnt = 0; cnt < 16; cnt++) {
            for (unsigned k = 0; k < sizeof(vals16) / sizeof(vals16[0]); k++) {
                for (int cf_in = 0; cf_in < 2; cf_in++) {
                    uint16_t v = (uint16_t)vals16[k], hres = 0;
                    uint64_t fl = 0;
                    hw_shift16[op][cnt](v, cf_in, &hres, &fl);
                    set_in_flags(cf_in);
                    int32_t pres = port_shift16(op, v, cnt);
                    compared++;
                    if ((uint16_t)pres != hres) bad(opname[op], v, cnt, cf_in, "res", (uint16_t)pres, hres);
                    if (cnt != 0) {
                        if (getcf() != FCF(fl)) bad(opname[op], v, cnt, cf_in, "getcf", getcf(), FCF(fl));
                        if (cnt == 1 && getof() != FOF(fl)) bad(opname[op], v, cnt, cf_in, "of", getof(), FOF(fl));
                        if (op >= 4) {
                            if (getzf() != FZF(fl)) bad(opname[op], v, cnt, cf_in, "zf", getzf(), FZF(fl));
                            if (getsf() != FSF(fl)) bad(opname[op], v, cnt, cf_in, "sf", getsf(), FSF(fl));
                            if (getpf() != FPF(fl)) bad(opname[op], v, cnt, cf_in, "pf", getpf(), FPF(fl));
                        }
                    }
                    if (emit_rows && (cnt == 1 || cnt == 15) && (v == 0x0001 || v == 0x8000 || v == 0xFFFF) && ((op == 2 || op == 3) || cf_in == 0))
                        printf("    {OP_%s, 16, 0x%04X, 0, %2d, %d, 0x%04X, %d, %d},\n", opname[op],
                               v, cnt, cf_in, hres, FCF(fl), (cnt == 1) ? FOF(fl) : -1);
                }
            }
        }
        /* 32-bit */
        for (int cnt = 0; cnt < 32; cnt++) {
            for (unsigned k = 0; k < sizeof(vals32) / sizeof(vals32[0]); k++) {
                for (int cf_in = 0; cf_in < 2; cf_in++) {
                    uint32_t v = vals32[k], hres = 0;
                    uint64_t fl = 0;
                    hw_shift32[op][cnt](v, cf_in, &hres, &fl);
                    set_in_flags(cf_in);
                    int32_t pres = port_shift32(op, (int32_t)v, cnt);
                    compared++;
                    if ((uint32_t)pres != hres) bad(opname[op], v, cnt, cf_in, "res", (uint32_t)pres, hres);
                    if (cnt != 0) {
                        if (getcf() != FCF(fl)) bad(opname[op], v, cnt, cf_in, "getcf", getcf(), FCF(fl));
                        if (cnt == 1 && getof() != FOF(fl)) bad(opname[op], v, cnt, cf_in, "of", getof(), FOF(fl));
                        if (op >= 4) {
                            if (getzf() != FZF(fl)) bad(opname[op], v, cnt, cf_in, "zf", getzf(), FZF(fl));
                            if (getsf() != FSF(fl)) bad(opname[op], v, cnt, cf_in, "sf", getsf(), FSF(fl));
                            if (getpf() != FPF(fl)) bad(opname[op], v, cnt, cf_in, "pf", getpf(), FPF(fl));
                        }
                    }
                    if (emit_rows && (cnt == 1 || cnt == 31) && (v == 0x00000001u || v == 0x80000000u || v == 0xFFFFFFFFu) && ((op == 2 || op == 3) || cf_in == 0))
                        printf("    {OP_%s, 32, 0x%08X, 0, %2d, %d, 0x%08X, %d, %d},\n", opname[op],
                               v, cnt, cf_in, hres, FCF(fl), (cnt == 1) ? FOF(fl) : -1);
                }
            }
        }
    }
    /* dual: shld/shrd */
    for (int op = 0; op < 2; op++) {
        const char *nm = op == 0 ? "shld" : "shrd";
        for (int cnt = 0; cnt <= 16; cnt++) {
            for (unsigned k = 0; k < sizeof(valsd16) / sizeof(valsd16[0]); k++) {
                for (unsigned j = 0; j < sizeof(valsd16) / sizeof(valsd16[0]); j++) {
                    uint16_t v = (uint16_t)valsd16[k], s = (uint16_t)valsd16[j], hres = 0;
                    uint64_t fl = 0;
                    hw_dual16[op][cnt](v, s, 0, &hres, &fl);
                    set_in_flags(0);
                    int32_t pres = op == 0 ? shld16(v, s, cnt) : shrd16(v, s, cnt);
                    compared++;
                    if ((uint16_t)pres != hres) bad2(nm, v, s, cnt, 0, "res", (uint16_t)pres, hres);
                    if (cnt != 0) {
                        if (getcf() != FCF(fl)) bad2(nm, v, s, cnt, 0, "getcf", getcf(), FCF(fl));
                        if (cnt == 1 && getof() != FOF(fl)) bad2(nm, v, s, cnt, 0, "of", getof(), FOF(fl));
                        if (getzf() != FZF(fl)) bad2(nm, v, s, cnt, 0, "zf", getzf(), FZF(fl));
                        if (getsf() != FSF(fl)) bad2(nm, v, s, cnt, 0, "sf", getsf(), FSF(fl));
                        if (getpf() != FPF(fl)) bad2(nm, v, s, cnt, 0, "pf", getpf(), FPF(fl));
                    }
                    if (emit_rows && (cnt == 1 || cnt == 15) && k == 2 && j == 1)
                        printf("    {OP_%s, 16, 0x%04X, 0x%04X, %2d, 0, 0x%04X, %d, %d},\n", nm, v, s,
                               cnt, hres, FCF(fl), (cnt == 1) ? FOF(fl) : -1);
                }
            }
        }
        for (int cnt = 0; cnt <= 32; cnt++) {
            if (cnt > 31) continue;
            for (unsigned k = 0; k < sizeof(valsd32) / sizeof(valsd32[0]); k++) {
                for (unsigned j = 0; j < sizeof(valsd32) / sizeof(valsd32[0]); j++) {
                    uint32_t v = valsd32[k], s = valsd32[j], hres = 0;
                    uint64_t fl = 0;
                    hw_dual32[op][cnt](v, s, 0, &hres, &fl);
                    set_in_flags(0);
                    int32_t pres = op == 0 ? shld32((int32_t)v, (int32_t)s, cnt) : shrd32((int32_t)v, (int32_t)s, cnt);
                    compared++;
                    if ((uint32_t)pres != hres) bad2(nm, v, s, cnt, 0, "res", (uint32_t)pres, hres);
                    if (cnt != 0) {
                        if (getcf() != FCF(fl)) bad2(nm, v, s, cnt, 0, "getcf", getcf(), FCF(fl));
                        if (cnt == 1 && getof() != FOF(fl)) bad2(nm, v, s, cnt, 0, "of", getof(), FOF(fl));
                        if (getzf() != FZF(fl)) bad2(nm, v, s, cnt, 0, "zf", getzf(), FZF(fl));
                        if (getsf() != FSF(fl)) bad2(nm, v, s, cnt, 0, "sf", getsf(), FSF(fl));
                        if (getpf() != FPF(fl)) bad2(nm, v, s, cnt, 0, "pf", getpf(), FPF(fl));
                    }
                    if (emit_rows && (cnt == 1 || cnt == 31) && k == 2 && j == 1)
                        printf("    {OP_%s, 32, 0x%08X, 0x%08X, %2d, 0, 0x%08X, %d, %d},\n", nm, v, s,
                               cnt, hres, FCF(fl), (cnt == 1) ? FOF(fl) : -1);
                }
            }
        }
    }
}

static void run_mul(void)
{
    static const uint16_t a8[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0x81, 0xFF};
    static const uint16_t b8[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF, 0x0A};
    for (unsigned i = 0; i < 7; i++)
        for (unsigned j = 0; j < 7; j++) {
            uint16_t hres = 0; uint64_t fl = 0;
            hw_mul8((uint8_t)a8[i], (uint8_t)b8[j], &hres, &fl);
            cpu_state_init();
            cpu->reg32[EAX] = a8[i];
            mul8(b8[j]);
            compared++;
            if ((uint16_t)read_reg16(AX) != hres) bad("mul8", a8[i], 0, b8[j], "AX", read_reg16(AX), hres);
            if (getcf() != FCF(fl)) bad("mul8", a8[i], 0, b8[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("mul8", a8[i], 0, b8[j], "of", getof(), FOF(fl));
            /* imul8 */
            int16_t ires = 0;
            hw_imul8((uint8_t)a8[i], (int8_t)b8[j], &ires, &fl);
            cpu_state_init();
            cpu->reg32[EAX] = a8[i];
            imul8((int8_t)b8[j]);
            compared++;
            if ((int16_t)read_reg16(AX) != ires) bad("imul8", a8[i], 0, b8[j], "AX", read_reg16(AX), ires);
            if (getcf() != FCF(fl)) bad("imul8", a8[i], 0, b8[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("imul8", a8[i], 0, b8[j], "of", getof(), FOF(fl));
            if ((int8_t)read_reg16(AX) != (int8_t)ires) bad("imul8", a8[i], 0, b8[j], "AL", read_reg8(AL), (int8_t)ires);
            if ((int8_t)read_reg8(AH) != (int8_t)(ires >> 8)) bad("imul8", a8[i], 0, b8[j], "AH", read_reg8(AH), (int8_t)(ires >> 8));
        }
    static const uint32_t a16[] = {0x0000, 0x0001, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x1234};
    static const uint32_t b16[] = {0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x0002, 0x0100};
    for (unsigned i = 0; i < 7; i++)
        for (unsigned j = 0; j < 7; j++) {
            uint16_t lo = 0, hi = 0; uint64_t fl = 0;
            hw_mul16((uint16_t)a16[i], (uint16_t)b16[j], &lo, &hi, &fl);
            cpu_state_init();
            cpu->reg32[EAX] = a16[i];
            mul16(b16[j]);
            compared++;
            if ((uint16_t)read_reg16(AX) != lo) bad("mul16", a16[i], 0, b16[j], "AX", read_reg16(AX), lo);
            if ((uint16_t)read_reg16(DX) != hi) bad("mul16", a16[i], 0, b16[j], "DX", read_reg16(DX), hi);
            if (getcf() != FCF(fl)) bad("mul16", a16[i], 0, b16[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("mul16", a16[i], 0, b16[j], "of", getof(), FOF(fl));
            hw_imul16((uint16_t)a16[i], (int16_t)b16[j], &lo, &hi, &fl);
            cpu_state_init();
            cpu->reg32[EAX] = a16[i];
            imul16((int16_t)b16[j]);
            compared++;
            if ((uint16_t)read_reg16(AX) != lo) bad("imul16", a16[i], 0, b16[j], "AX", read_reg16(AX), lo);
            if ((uint16_t)read_reg16(DX) != hi) bad("imul16", a16[i], 0, b16[j], "DX", read_reg16(DX), hi);
            if (getcf() != FCF(fl)) bad("imul16", a16[i], 0, b16[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("imul16", a16[i], 0, b16[j], "of", getof(), FOF(fl));
            /* two-operand imul */
            uint16_t r = 0;
            hw_imul16_2((uint16_t)a16[i], (uint16_t)b16[j], &r, &fl);
            cpu_state_init();
            int32_t pres = imul_reg16(a16[i], b16[j]);
            compared++;
            if ((uint16_t)pres != r) bad("imul_reg16", a16[i], 0, b16[j], "res", (uint16_t)pres, r);
            if (getcf() != FCF(fl)) bad("imul_reg16", a16[i], 0, b16[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("imul_reg16", a16[i], 0, b16[j], "of", getof(), FOF(fl));
        }
    static const uint32_t a32[] = {0x00000000u, 0x00000001u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u};
    static const uint32_t b32[] = {0x00000001u, 0x00000002u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x00010000u};
    for (unsigned i = 0; i < 6; i++)
        for (unsigned j = 0; j < 6; j++) {
            uint32_t lo = 0, hi = 0; uint64_t fl = 0;
            hw_mul32(a32[i], b32[j], &lo, &hi, &fl);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)a32[i];
            mul32((int32_t)b32[j]);
            compared++;
            if ((uint32_t)read_reg32(EAX) != lo) bad("mul32", a32[i], 0, b32[j], "EAX", read_reg32(EAX), lo);
            if ((uint32_t)read_reg32(EDX) != hi) bad("mul32", a32[i], 0, b32[j], "EDX", read_reg32(EDX), hi);
            if (getcf() != FCF(fl)) bad("mul32", a32[i], 0, b32[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("mul32", a32[i], 0, b32[j], "of", getof(), FOF(fl));
            hw_imul32(a32[i], (int32_t)b32[j], &lo, &hi, &fl);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)a32[i];
            imul32((int32_t)b32[j]);
            compared++;
            if ((uint32_t)read_reg32(EAX) != lo) bad("imul32", a32[i], 0, b32[j], "EAX", read_reg32(EAX), lo);
            if ((uint32_t)read_reg32(EDX) != hi) bad("imul32", a32[i], 0, b32[j], "EDX", read_reg32(EDX), hi);
            if (getcf() != FCF(fl)) bad("imul32", a32[i], 0, b32[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("imul32", a32[i], 0, b32[j], "of", getof(), FOF(fl));
            uint32_t r = 0;
            hw_imul32_2(a32[i], b32[j], &r, &fl);
            cpu_state_init();
            int32_t pres = imul_reg32((int32_t)a32[i], (int32_t)b32[j]);
            compared++;
            if ((uint32_t)pres != r) bad("imul_reg32", a32[i], 0, b32[j], "res", (uint32_t)pres, r);
            if (getcf() != FCF(fl)) bad("imul_reg32", a32[i], 0, b32[j], "cf", getcf(), FCF(fl));
            if (getof() != FOF(fl)) bad("imul_reg32", a32[i], 0, b32[j], "of", getof(), FOF(fl));
        }
}

static void run_div(void)
{
    static const uint32_t t8[] = {0x0000, 0x0001, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFF, 0x1234};
    static const uint32_t d8[] = {0, 1, 2, 3, 7, 0x10, 0x80, 0xFF};
    for (unsigned i = 0; i < 8; i++)
        for (unsigned j = 0; j < 8; j++) {
            uint8_t q = 0, r = 0;
            int hwok = hw_div8((uint16_t)t8[i], (uint8_t)d8[j], &q, &r);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)t8[i];
            last_vector = -1;
            div8(d8[j]);
            compared++;
            if ((last_vector == CPU_EXCEPTION_DE) != (hwok == 0))
                bad("div8", t8[i], 0, d8[j], "DE", last_vector == CPU_EXCEPTION_DE, hwok == 0);
            if (hwok) {
                if ((uint8_t)read_reg8(AL) != q) bad("div8", t8[i], 0, d8[j], "AL", read_reg8(AL), q);
                if ((uint8_t)read_reg8(AH) != r) bad("div8", t8[i], 0, d8[j], "AH", read_reg8(AH), r);
            }
            last_vector = -1;
            int hwok2 = hw_idiv8((int16_t)t8[i], (int8_t)d8[j], (int8_t *)&q, (int8_t *)&r);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)t8[i];
            idiv8((int8_t)d8[j]);
            compared++;
            if ((last_vector == CPU_EXCEPTION_DE) != (hwok2 == 0))
                bad("idiv8", t8[i], 0, d8[j], "DE", last_vector == CPU_EXCEPTION_DE, hwok2 == 0);
            if (hwok2) {
                if ((uint8_t)read_reg8(AL) != q) bad("idiv8", t8[i], 0, d8[j], "AL", read_reg8(AL), (int8_t)q);
                if ((uint8_t)read_reg8(AH) != r) bad("idiv8", t8[i], 0, d8[j], "AH", read_reg8(AH), (int8_t)r);
            }
        }
    static const uint64_t t16[] = {0x00000000ull, 0x00000001ull, 0x0000FFFFull, 0x00010000ull,
                                   0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull, 0x12345678ull};
    static const uint32_t d16[] = {0, 1, 2, 3, 7, 0x100, 0x8000, 0xFFFF};
    for (unsigned i = 0; i < 8; i++)
        for (unsigned j = 0; j < 8; j++) {
            uint16_t q = 0, r = 0;
            int hwok = hw_div16((uint32_t)t16[i], (uint16_t)d16[j], &q, &r);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)(t16[i] & 0xFFFF);
            cpu->reg32[EDX] = (int32_t)(t16[i] >> 16);
            last_vector = -1;
            div16(d16[j]);
            compared++;
            if ((last_vector == CPU_EXCEPTION_DE) != (hwok == 0))
                bad("div16", (unsigned)t16[i], 0, d16[j], "DE", last_vector == CPU_EXCEPTION_DE, hwok == 0);
            if (hwok) {
                if ((uint16_t)read_reg16(AX) != q) bad("div16", (unsigned)t16[i], 0, d16[j], "AX", read_reg16(AX), q);
                if ((uint16_t)read_reg16(DX) != r) bad("div16", (unsigned)t16[i], 0, d16[j], "DX", read_reg16(DX), r);
            }
            int hwok2 = hw_idiv16((int32_t)t16[i], (int16_t)d16[j], (int16_t *)&q, (int16_t *)&r);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)(t16[i] & 0xFFFF);
            cpu->reg32[EDX] = (int32_t)(t16[i] >> 16);
            last_vector = -1;
            idiv16((int16_t)d16[j]);
            compared++;
            if ((last_vector == CPU_EXCEPTION_DE) != (hwok2 == 0))
                bad("idiv16", (unsigned)t16[i], 0, d16[j], "DE", last_vector == CPU_EXCEPTION_DE, hwok2 == 0);
            if (hwok2) {
                if ((uint16_t)read_reg16(AX) != q) bad("idiv16", (unsigned)t16[i], 0, d16[j], "AX", read_reg16(AX), (int16_t)q);
                if ((uint16_t)read_reg16(DX) != r) bad("idiv16", (unsigned)t16[i], 0, d16[j], "DX", read_reg16(DX), (int16_t)r);
            }
        }
    static const uint64_t t32[] = {0x0ull, 0x1ull, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull,
                                   0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x0000000100000000ull,
                                   0x0000000180000000ull};
    static const uint32_t d32[] = {0, 1, 2, 3, 0x10000, 0x80000000u, 0xFFFFFFFFu, 7};
    for (unsigned i = 0; i < 9; i++)
        for (unsigned j = 0; j < 8; j++) {
            uint32_t q = 0, r = 0;
            int hwok = hw_div32(t32[i], d32[j], &q, &r);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)(uint32_t)t32[i];
            cpu->reg32[EDX] = (int32_t)(uint32_t)(t32[i] >> 32);
            last_vector = -1;
            div32(d32[j]);
            compared++;
            if ((last_vector == CPU_EXCEPTION_DE) != (hwok == 0))
                bad("div32", (unsigned)t32[i], 0, d32[j], "DE", last_vector == CPU_EXCEPTION_DE, hwok == 0);
            if (hwok) {
                if ((uint32_t)read_reg32(EAX) != q) bad("div32", (unsigned)t32[i], 0, d32[j], "EAX", read_reg32(EAX), q);
                if ((uint32_t)read_reg32(EDX) != r) bad("div32", (unsigned)t32[i], 0, d32[j], "EDX", read_reg32(EDX), r);
            }
            int hwok2 = hw_idiv32((int64_t)t32[i], (int32_t)d32[j], (int32_t *)&q, (int32_t *)&r);
            cpu_state_init();
            cpu->reg32[EAX] = (int32_t)(uint32_t)t32[i];
            cpu->reg32[EDX] = (int32_t)(uint32_t)(t32[i] >> 32);
            last_vector = -1;
            idiv32((int32_t)d32[j]);
            compared++;
            if ((last_vector == CPU_EXCEPTION_DE) != (hwok2 == 0))
                bad("idiv32", (unsigned)t32[i], 0, d32[j], "DE", last_vector == CPU_EXCEPTION_DE, hwok2 == 0);
            if (hwok2) {
                if ((uint32_t)read_reg32(EAX) != q) bad("idiv32", (unsigned)t32[i], 0, d32[j], "EAX", read_reg32(EAX), (int32_t)q);
                if ((uint32_t)read_reg32(EDX) != r) bad("idiv32", (unsigned)t32[i], 0, d32[j], "EDX", read_reg32(EDX), (int32_t)r);
            }
        }
}

static const unsigned bcd_als[] = {0x00, 0x04, 0x09, 0x0A, 0x0F, 0x10, 0x60, 0x99, 0x9A, 0xA0, 0xFA};
static const unsigned bcd_ahs[] = {0x00, 0x03, 0x0A, 0x0F, 0xFF};

static void row(const char *nm, unsigned al, unsigned ah, unsigned cf, unsigned af, unsigned oal,
                unsigned oah, unsigned ocf, unsigned oaf)
{
    printf("%s %02x %02x %u %u | %02x %02x %u %u\n", nm, al, ah, cf, af, oal, oah, ocf, oaf);
}

/* the port's own rows, in exactly probe32's format: `diff <(./probe32) <(./optruth --bcdrows)` */
static void run_bcd(int rows_only)
{
    (void)rows_only;
    for (int o = 0; o < 6; o++) {
        const char *nm = o == 0 ? "daa" : o == 1 ? "das" : o == 2 ? "aaa" : o == 3 ? "aas"
                          : o == 4 ? "aam" : "aad";
        for (unsigned i = 0; i < 11; i++)
            for (unsigned j = 0; j < 5; j++)
                for (unsigned cf = 0; cf < 2; cf++)
                    for (unsigned af = 0; af < 2; af++) {
                        set_in_flags(cf);
                        if (af) cpu->flags |= FLAG_ADJUST;
                        cpu->reg32[EAX] = (int32_t)((bcd_ahs[j] << 8) | bcd_als[i]);
                        switch (o) {
                        case 0: bcd_daa(); break;
                        case 1: bcd_das(); break;
                        case 2: bcd_aaa(); break;
                        case 3: bcd_aas(); break;
                        case 4: bcd_aam(10); break;
                        default: bcd_aad(10); break;
                        }
                        row(nm, bcd_als[i], bcd_ahs[j], cf, af, read_reg8(AL), read_reg8(AH),
                            getcf() ? 1 : 0, getaf() ? 1 : 0);
                    }
    }
}

static void run_bits(void)
{
    static const uint32_t bases[] = {0x00000000u, 0x00000001u, 0x80000000u, 0xFFFFFFFFu, 0x12345678u};
    static const int offs[] = {0, 1, 7, 15, 30, 31};
    for (unsigned i = 0; i < 5; i++)
        for (unsigned j = 0; j < 6; j++) {
            int cf = 0; uint32_t r = 0;
            hw_bt_reg(bases[i], offs[j], &cf);
            set_in_flags(0);
            bt_reg((int32_t)bases[i], offs[j]);
            compared++;
            if (getcf() != cf) bad("bt_reg", bases[i], 0, offs[j], "cf", getcf(), cf);
            hw_btc_reg(bases[i], offs[j], &cf);
            set_in_flags(0);
            r = (uint32_t)btc_reg((int32_t)bases[i], offs[j]);
            compared++;
            if (getcf() != cf) bad("btc_reg", bases[i], 0, offs[j], "cf", getcf(), cf);
            hw_bts_reg(bases[i], offs[j], &cf);
            uint32_t hbts = hw_bts_reg(bases[i], offs[j], &cf);
            set_in_flags(0);
            r = (uint32_t)bts_reg((int32_t)bases[i], offs[j]);
            compared++;
            if (r != hbts) bad("bts_reg", bases[i], 0, offs[j], "res", r, hbts);
            set_in_flags(0);
            btr_reg((int32_t)bases[i], offs[j]);
            /* btr result */
            uint32_t hbtr = hw_btr_reg(bases[i], offs[j], &cf);
            set_in_flags(0);
            r = (uint32_t)btr_reg((int32_t)bases[i], offs[j]);
            compared++;
            if (r != hbtr) bad("btr_reg", bases[i], 0, offs[j], "res", r, hbtr);
            /* bsf / bsr */
            uint32_t hres = 0; int zf = 0;
            hw_bsf32(bases[i], &hres, &zf);
            cpu_state_init();
            int32_t pres = bsf32(0xDEAD, (int32_t)bases[i]);
            compared++;
            if (zf == 0 && (uint32_t)pres != hres) bad("bsf32", bases[i], 0, 0, "res", (uint32_t)pres, hres);
            if (getzf() != zf) bad("bsf32", bases[i], 0, 0, "zf", getzf(), zf);
            hw_bsr32(bases[i], &hres, &zf);
            cpu_state_init();
            pres = bsr32(0xDEAD, (int32_t)bases[i]);
            compared++;
            if (zf == 0 && (uint32_t)pres != hres) bad("bsr32", bases[i], 0, 0, "res", (uint32_t)pres, hres);
            if (getzf() != zf) bad("bsr32", bases[i], 0, 0, "zf", getzf(), zf);
        }
}

static void run_logic(void)
{
    static const uint32_t vals[] = {0x00000000u, 0x00000001u, 0x0000000Fu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u};
    for (unsigned i = 0; i < 6; i++)
        for (unsigned j = 0; j < 6; j++) {
            uint32_t a = vals[i], b = vals[j], hr = 0; uint64_t fl = 0;
            /* and / or / xor / test: hardware via C, the flags are the SDM's rules */
            __asm__ volatile("movq $2, %%r11\n\t pushq %%r11\n\t popfq\n\t"
                             "andl %[b], %[a]\n\t pushfq\n\t popq %[f]\n\t"
                             : [a] "+r"(a), [f] "=&r"(fl)
                             : [b] "r"(b)
                             : "r11", "cc");
            hr = a;
            cpu_state_init();
            int32_t pres = and32((int32_t)vals[i], (int32_t)vals[j]);
            compared++;
            if ((uint32_t)pres != hr) bad("and32", vals[i], 0, vals[j], "res", (uint32_t)pres, hr);
            if (getcf() != 0 || getof() != 0 || getaf() != 0)
                bad("and32", vals[i], 0, vals[j], "cfofaf", getcf() | getof() << 1 | getaf() << 2, 0);
            if (getzf() != FZF(fl)) bad("and32", vals[i], 0, vals[j], "zf", getzf(), FZF(fl));
            if (getsf() != FSF(fl)) bad("and32", vals[i], 0, vals[j], "sf", getsf(), FSF(fl));
            if (getpf() != FPF(fl)) bad("and32", vals[i], 0, vals[j], "pf", getpf(), FPF(fl));
            a = vals[i];
            __asm__ volatile("movq $2, %%r11\n\t pushq %%r11\n\t popfq\n\t"
                             "orl %[b], %[a]\n\t pushfq\n\t popq %[f]\n\t"
                             : [a] "+r"(a), [f] "=&r"(fl)
                             : [b] "r"(b)
                             : "r11", "cc");
            hr = a;
            cpu_state_init();
            pres = or32((int32_t)vals[i], (int32_t)vals[j]);
            compared++;
            if ((uint32_t)pres != hr) bad("or32", vals[i], 0, vals[j], "res", (uint32_t)pres, hr);
            if (getzf() != FZF(fl)) bad("or32", vals[i], 0, vals[j], "zf", getzf(), FZF(fl));
            if (getsf() != FSF(fl)) bad("or32", vals[i], 0, vals[j], "sf", getsf(), FSF(fl));
            a = vals[i];
            __asm__ volatile("movq $2, %%r11\n\t pushq %%r11\n\t popfq\n\t"
                             "xorl %[b], %[a]\n\t pushfq\n\t popq %[f]\n\t"
                             : [a] "+r"(a), [f] "=&r"(fl)
                             : [b] "r"(b)
                             : "r11", "cc");
            hr = a;
            cpu_state_init();
            pres = xor32((int32_t)vals[i], (int32_t)vals[j]);
            compared++;
            if ((uint32_t)pres != hr) bad("xor32", vals[i], 0, vals[j], "res", (uint32_t)pres, hr);
            if (getzf() != FZF(fl)) bad("xor32", vals[i], 0, vals[j], "zf", getzf(), FZF(fl));
            if (getsf() != FSF(fl)) bad("xor32", vals[i], 0, vals[j], "sf", getsf(), FSF(fl));
        }
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1 && strcmp(argv[1], "--bcdrows") == 0) {
        if (!allocate_memory(1u << 20)) return 2;
        cpu_state_init();
        cpu->memory_size = 1u << 20;
        run_bcd(1);
        return 0;
    }
    if (argc > 1) emit_rows = 0;
    signal(SIGFPE, fpe_handler);
    if (!allocate_memory(1u << 20)) return 2;
    cpu_state_init();
    cpu->memory_size = 1u << 20;
    if (emit_rows) printf("/* ---- shift/rotate rows from the host oracle ---- */\n");
    run_shifts();
    run_mul();
    run_div();
    run_bits();
    run_logic();
    printf("/* compared=%ld mismatches=%d */\n", compared, mism);
    return mism ? 1 : 0;
}
