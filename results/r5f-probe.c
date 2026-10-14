/* R5F's asymmetry probe for R5's B1 (the delivered-fault pagefault flag + the
 * dispatcher's read of it).  Built and driven by results/r5f-probe.sh; it links the REAL
 * translation units (CPU_SRC, the same object set `make` builds) and drives the real
 * `cycle_internal`, so it exercises the dispatcher and not the delivery helpers.
 *
 * The reviewer's R5 probe (/tmp/r5probe.c) is the provenance; TWO corrections are made
 * here and both are deliberate:
 *   - the different-page CONTROL handler is at 0x4010 (page 4).  The reviewer's control
 *     put it at 0x3010, which is the GDT's page (GDT_BASE 0x3000, 3 entries) -- the
 *     handler's bytes overwrote the data descriptor.  The scenario still showed the
 *     asymmetry, but it is not a construction to reuse.
 *   - the same-page handler is at 0x1010, the faulting instruction at 0x1000 (page 1), so
 *     the two share a 4 KiB page exactly as B1 describes.
 *
 * Usage: r5f-probe <same|diff|fetch> [cycles]
 *   same   faulting instruction 0x1000 (page 1), #PF handler at 0x1010 (SAME page)
 *   diff   identical, handler at 0x4010 (a DIFFERENT page) -- the control
 *   fetch  eip starts on an ABSENT page (the genuine instruction-fetch fault)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu.h"
#include "cpu/cpu_state.h"
#include "cpu/flags.h"
#include "cpu/memory.h"
#include "cpu/paging.h"

static void w8(uint32_t a, uint8_t v) { mem8[a] = v; }
static void w32(uint32_t a, uint32_t v)
{
    mem8[a] = (uint8_t)v; mem8[a + 1] = (uint8_t)(v >> 8);
    mem8[a + 2] = (uint8_t)(v >> 16); mem8[a + 3] = (uint8_t)(v >> 24);
}
static void w64(uint32_t a, uint64_t v) { w32(a, (uint32_t)v); w32(a + 4, (uint32_t)(v >> 32)); }
static uint32_t rd32(uint32_t a)
{
    return (uint32_t)mem8[a] | (uint32_t)mem8[a + 1] << 8 |
           (uint32_t)mem8[a + 2] << 16 | (uint32_t)mem8[a + 3] << 24;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "same";
    int cycles = argc > 2 ? atoi(argv[2]) : 3;

    cpu_state_init();
    if (!allocate_memory(2u * 1024 * 1024)) { puts("alloc fail"); return 2; }
    cpu->memory_size = 2u * 1024 * 1024;
    zero_memory(0, 2u * 1024 * 1024);

    /* GDT at 0x3000: 0=null, 8=code flat 32-bit, 0x10=data flat */
    w64(0x3000, 0);
    w64(0x3008, 0xFFFFull | 0x9Aull << 40 | 0xCull << 52);
    w64(0x3010, 0xFFFFull | 0x92ull << 40 | 0xCull << 52);
    cpu->gdtr_offset = 0x3000;
    cpu->gdtr_size = 3 * 8 - 1;

    /* paging: PD at 0x20000, PT at 0x21000; every page identity-mapped EXCEPT page 2 */
    w32(0x20000, 0x21000u | 7);
    for (int i = 0; i < 1024; i++) {
        uint32_t e = (i == 2) ? 0 : ((uint32_t)i << 12 | 7u);
        w32(0x21000 + 4u * (uint32_t)i, e);
    }

    /* the faulting instruction: mov eax,[0x2800] (page 2, absent) at 0x1000 ; hlt */
    w8(0x1000, 0xA1); w32(0x1001, 0x2800); w8(0x1005, 0xF4);

    /* the #PF handler: inc dword[0x1800] ; add esp,4 ; iret */
    uint32_t handler = (strcmp(mode, "diff") == 0) ? 0x4010u : 0x1010u;
    w8(handler + 0, 0xFF); w8(handler + 1, 0x05); w32(handler + 2, 0x1800);
    w8(handler + 6, 0x83); w8(handler + 7, 0xC4); w8(handler + 8, 0x04);
    w8(handler + 9, 0xCF);

    /* IDT at 0x3800: vector 14 -> cs 8, the handler, 32-bit interrupt gate */
    w64(0x3800 + 14 * 8,
        (uint64_t)(handler & 0xFFFF) | 8ull << 16 | 0x8Eull << 40 |
        ((uint64_t)(handler >> 16) & 0xFFFF) << 48);
    cpu->idtr_offset = 0x3800;
    cpu->idtr_size = 256 * 8 - 1;

    cpu->cr[0] = 1;
    cpu->protected_mode = true;
    cpu->flags = FLAGS_DEFAULT;
    cpu->is_32 = true;
    cpu->stack_size_32 = true;
    cpu->cpl = 0;
    for (int i = 0; i < 6; i++) {
        cpu->segment_is_null[i] = false;
        cpu->segment_offsets[i] = 0;
        cpu->segment_limits[i] = 0xFFFFFFFFu;
        cpu->sreg[i] = (uint16_t)(i == CS ? 8 : 0x10);
        cpu->segment_access_bytes[i] = (uint8_t)(i == CS ? 0x9A : 0x92);
    }
    write_reg32(ESP, 0x1F00);
    if (strcmp(mode, "fetch") == 0) {
        cpu->instruction_pointer = 0x2804; /* an absent page: the FETCH itself faults */
    }
    else {
        cpu->instruction_pointer = 0x1000;
    }
    cpu->previous_ip = cpu->instruction_pointer;
    cpu->pagefault = false;
    update_state_flags();
    set_cr3(0x20000);
    set_cr0(cpu->cr[0] | CR0_PG);

    printf("%s: handler=%x before: eip=%x counter=%u marker=%x esp=%x pf=%d\n",
           mode, handler, cpu->instruction_pointer, cpu->instruction_counter,
           rd32(0x1800), read_reg32(ESP), (int)cpu->pagefault);
    fflush(stdout);
    for (int c = 0; c < cycles; c++) {
        cycle_internal();
        printf("%s: after %d: eip=%x counter=%u marker=%x esp=%x pf=%d unimplemented=%d\n",
               mode, c + 1, cpu->instruction_pointer, cpu->instruction_counter,
               rd32(0x1800), read_reg32(ESP), (int)cpu->pagefault,
               (int)interpreted_unimplemented);
        fflush(stdout);
    }
    return 0;
}

/* M2-1 moved run_hardware_timers to src/hw/machine.c and the io bridge to src/hw/io.c;
 * this probe links the CORE only and never runs a timer or a port.
 * (io_port_read8/16/32 too? -- see the linker: only what the core references.) */
double run_hardware_timers(bool acpi_enabled, double t) { (void)acpi_enabled; return t; }
int32_t io_port_read8(int32_t port) { (void)port; return 0; }
int32_t io_port_read16(int32_t port) { (void)port; return 0; }
int32_t io_port_read32(int32_t port) { (void)port; return 0; }
void io_port_write8(int32_t port, int32_t value) { (void)port; (void)value; }
void io_port_write16(int32_t port, int32_t value) { (void)port; (void)value; }
void io_port_write32(int32_t port, int32_t value) { (void)port; (void)value; }
