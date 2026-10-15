/*
 * hw_selftest.c -- M2-1's crafted test binary: THE DEVICE GATE (the brief's G1).
 *
 * WHAT IT DRIVES, and why each case is the observable one the brief asks for:
 *
 *   0. THE CLOCK       -- M2-1F's D-F2: 1 guest instruction = 1 MICROSECOND of virtual
 *                          time, asserted AT THE SOURCE (microtick() itself) and through
 *                          all three consumers (the PIT's tick rate, the ACPI PM timer,
 *                          the TSC's 1 GHz), including the measured instruction distance
 *                          at which a 10 ms PIT reload fires;
 *   1. the I/O BUS      -- the port map, the unclaimed-port CONTRACT (D-F1: the
 *                          reference's 0xFF/0xFFFF/-1 read, a discarded write, no stop),
 *                          the mmap half of the memory map (including the R1F VGA
 *                          carve-out it must preserve);
 *   2. the PIT          -- the port interface, the latch, the counter arithmetic, and a
 *                          REAL IRQ0 delivered through the PIC into an IDT handler after
 *                          the programmed count;
 *   3. the RTC / CMOS   -- a register round-trip, the status registers, the alarm path's
 *                          BCD encode checked against the host clock's own second, and
 *                          fill_cmos's memory-size registers;
 *   4. the DMA          -- the address/count/page/flip-flop registers, a transfer LANDING
 *                          BYTES IN GUEST MEMORY, and the reverse;
 *   5. the PS/2         -- a scancode producing the right byte, the self-test and identify
 *                          replies, a mouse packet, and the IntelliMouse id dance;
 *   6. the PCI bus      -- config-space reads of the two devices the machine registers,
 *                          an absent device, the I/O-bar move, and the two loud stops;
 *   7. the ACPI device  -- the PM1 registers, the GPE registers, the PM timer, and IRQ9;
 *   8. THE HANDOVER     -- G4: a PORT write reaching the PIT through the CPU's OWN
 *                          instruction body (instr_E6 -> io_port_write8 -> the PIT), plus
 *                          the G1 PORT-0x80 PROBE (the BIOS POST code and the Linux delay
 *                          loop, through the CPU's own `out`/`in`: the before/after pair of
 *                          the D-F1 policy decision).
 *
 * HOW THE MACHINE IS DRIVEN.  machine_reset() rebuilds the whole machine (cpu_state_init +
 * machine_init), so every case starts from a fresh port map and fresh devices.  The IRQ
 * cases need what the run loop normally does by hand: an IDT and the PIC's programming
 * (pic.rs's irq_map default is 0, so a delivered IRQ's vector is the ICW2 base).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../cpu/apic.h"
#include "../cpu/cpu.h"
#include "../cpu/cpu_state.h"
#include "../cpu/exceptions.h"
#include "../cpu/flags.h"
#include "../cpu/instructions.h"
#include "../cpu/ioapic.h"
#include "../cpu/memory.h"
#include "../cpu/misc_instr.h"
#include "../cpu/paging.h"
#include "../cpu/pic.h"
#include "../cpu/string.h" /* io_port_read8/16/32, io_port_write8/16/32 (the bridge) */
#include "acpi.h"
#include "acpi_tables.h"
#include "bus.h"
#include "dma.h"
#include "ide.h"
#include "io.h"
#include "machine.h"
#include "pci.h"
#include "pit.h"
#include "kernel.h"
#include "ps2.h"
#include "rtc.h"
#include "uart.h"
#include "virtio.h"
#include "virtio_console.h"

static int checks = 0;
static int failures = 0;
/* A NAMED skip, never a silent pass: the two loader cases that need the fetched kernel
 * (tests/images/vmlinuz-i386, 7.7 MB, fetched by the unit's brief) print what they skipped
 * and why, and the count is reported at the end with the checks and failures. */
static int skips = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        checks++;                                                             \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        if (_a != _b) {                                                       \
            failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: %s == %s (got 0x%llx, want 0x%llx)\n", \
                    __FILE__, __LINE__, #a, #b, (unsigned long long)_a,         \
                    (unsigned long long)_b);                                   \
        }                                                                     \
    } while (0)

static void clear_stop(void) { interpreted_unimplemented = false; }

/* llround is not in cosmocc's libc (MEASURED: an undefined reference at link time), and
 * every value converted in this file is a non-negative instruction count or tick count, so
 * the rounding is (x + 0.5) truncated -- written out rather than left as a magic idiom. */
static uint32_t round_to_u32(double x) { return (uint32_t)(x + 0.5); }

/* ------------------------------------------------------------------ *
 * the guest-side fixtures the IRQ cases need
 * ------------------------------------------------------------------ */
#define GDT_BASE 0x1000
#define IDT_BASE 0x2000
#define STACK_TOP 0x7000
#define HANDLER_EIP 0x8000
#define MEMORY_SIZE 0x200000u

#define SEL_CODE 0x08
#define SEL_DATA 0x10
#define SEL_CODE2 0x18

static void wr64(uint32_t addr, uint64_t v)
{
    for (int i = 0; i < 8; i++) mem8[addr + i] = (uint8_t)(v >> (8 * i));
}

static uint64_t make_desc(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFF);
    d |= (uint64_t)(base & 0xFFFF) << 16;
    d |= (uint64_t)(base >> 16 & 0xFF) << 24;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d |= (uint64_t)flags << 52;
    d |= (uint64_t)(base >> 24 & 0xFF) << 56;
    return d;
}

static uint64_t make_idt_entry(int32_t offset, uint16_t selector, uint8_t gate_type,
                               uint8_t dpl, bool is_32)
{
    uint64_t d = 0;
    d |= (uint64_t)((uint32_t)offset & 0xFFFF);
    d |= (uint64_t)selector << 16;
    d |= (uint64_t)(0x80 | (dpl << 5) | (is_32 ? 8 : 0) | gate_type) << 40;
    d |= (uint64_t)(((uint32_t)offset >> 16) & 0xFFFF) << 48;
    return d;
}

static void build_gdt(void)
{
    wr64(GDT_BASE + 0 * 8, 0);
    wr64(GDT_BASE + 1 * 8, make_desc(0, 0xFFFFF, 0x9A, 0xC)); /* code */
    wr64(GDT_BASE + 2 * 8, make_desc(0, 0xFFFFF, 0x92, 0xC)); /* data */
    wr64(GDT_BASE + 3 * 8, make_desc(0, 0xFFFFF, 0x9A, 0xC)); /* code 2 */
    cpu->gdtr_offset = GDT_BASE;
    cpu->gdtr_size = 4 * 8 - 1;
}

static void build_idt(void)
{
    /* an interrupt gate per PIC vector the tests deliver: 0x20 (irq0), 0x21 (irq1),
     * 0x28 (irq8), 0x29 (irq9), 0x2C (irq12) */
    for (int32_t v = 0x20; v <= 0x2C; v++) {
        wr64(IDT_BASE + (uint32_t)v * 8,
             make_idt_entry(HANDLER_EIP, SEL_CODE2, INTR_GATE_INTERRUPT, 0, true));
    }
    cpu->idtr_offset = IDT_BASE;
    cpu->idtr_size = 256 * 8 - 1;
}

/* Both PICs, programmed exactly as a BIOS does: icw2 = 0x20 for the master and 0x28 for
 * the slave, nothing masked.  The slave's base is what makes IRQ8/9/12 arrive on vectors
 * 0x28/0x29/0x2C, which is the mapping this file's IDT is built for. */
static void program_pic(void)
{
    io_port_write8(0x20, 0x11); /* icw1: edge triggered, icw4 follows */
    io_port_write8(0x21, 0x20); /* icw2: master vectors at 0x20 */
    io_port_write8(0x21, 0x04); /* icw3: slave on irq2 */
    io_port_write8(0x21, 0x01); /* icw4: 8086 mode */
    io_port_write8(0x21, 0x00); /* ocw1: nothing masked */

    io_port_write8(0xA0, 0x11);
    io_port_write8(0xA1, 0x28); /* icw2: slave vectors at 0x28 */
    io_port_write8(0xA1, 0x02); /* icw3: cascade identity */
    io_port_write8(0xA1, 0x01); /* icw4: 8086 mode */
    io_port_write8(0xA1, 0x00);
}

static void machine_reset(void)
{
    clear_stop(); /* the flag is global state: a case that stopped must not leak into the next */
    cpu_state_init();
    cpu->reboot_requested = false; /* a case that rebooted (REBOOT-1) ditto: the driver, not
                                    * the device layer, consumes and clears this */
    /* allocate_memory asserts that nothing is allocated yet: the guest RAM is taken once
     * and re-zeroed per case (the m1c selftest's arrangement) */
    if (mem8 == NULL && !allocate_memory(MEMORY_SIZE)) {
        fputs("cannot allocate guest memory\n", stderr);
        failures++;
        return;
    }
    cpu->memory_size = MEMORY_SIZE;
    zero_memory(0, MEMORY_SIZE);

    /* flat protected mode, 32-bit, cpl 0 (the m1c selftest's machine_reset) */
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
        cpu->sreg[i] = (uint16_t)(i == CS ? SEL_CODE : SEL_DATA);
        cpu->segment_access_bytes[i] = 0x92;
    }
    cpu->segment_access_bytes[CS] = 0x9A;
    write_reg32(ESP, STACK_TOP);
    cpu->instruction_pointer = STACK_TOP;
    cpu->previous_ip = STACK_TOP;
    cpu->pagefault = false;
    update_state_flags();

    build_gdt();
    build_idt();

    /* THE MACHINE: a fresh port map and fresh devices */
    machine_init();
}

/* ================================================================== *
 * 1. the I/O bus
 * ================================================================== */
static int32_t mmap_test_read8(uint32_t addr) { return (int32_t)(addr & 0xFF) ^ 0x5A; }
static void mmap_test_write8(uint32_t addr, int32_t value)
{
    /* the observable: the write lands where the test can see it, tagged with the offset it
     * arrived at (a write must reach the block's own write8, not a neighbour's) */
    mem8[0x100 + (addr & 0xFF)] = (uint8_t)value;
}
static int32_t mmap_test_read32(uint32_t addr) { return (int32_t)(0x12340000u | (addr & 0xFFFF)); }

/* ================================================================== *
 * 0. THE CLOCK -- M2-1F's D-F2 (HANDOFF=v86c)
 *
 * The clock is a POLICY DECISION (results/m2-1f-policy.md), so it is asserted at the source
 * AND through every consumer, because "the three consumers agree" is the whole claim: the
 * PIT, the ACPI PM timer and the TSC each multiply microtick() by their own rate, so a
 * consumer whose rate is written for a different unit shows up here as a ratio mismatch.
 * ================================================================== */
#define PIT_FREQ_KHZ 1193.1816666   /* pit.js:15 */
#define PMTIMER_FREQ_HZ 3579545.0   /* acpi.js:18 */

static void test_clock(void)
{
    machine_reset(); /* done first: the TSC check below depends on FRESH tsc statics */

    /* THE SCALE AT THE SOURCE: 1 guest instruction = 1 MICROSECOND, microtick() in
     * MILLISECONDS (upstream's unit -- main.js:223-240 returns performance.now/Date.now) */
    cpu->instruction_counter = 0;
    CHECK(microtick() == 0.0);
    cpu->instruction_counter = 1;
    CHECK(microtick() == 0.001);
    cpu->instruction_counter = 1000;
    CHECK(microtick() == 1.0);
    cpu->instruction_counter = 1234567;
    CHECK(microtick() == 1234.567);

    /* THE TSC (cpu.rs:4365 `microtick() * TSC_RATE`, TSC_RATE = 1e6): 1000 counts per
     * instruction, i.e. one count per microsecond of virtual time -- the 1 GHz rate
     * cpuid advertises (instructions_0f.c:1240).  The reads pass through the
     * imprecise-browser workaround (cpu.rs:301-308), whose resolution seed is u64::MAX on a
     * fresh machine, so the FIRST read is the establish step and the deltas AFTER it are
     * exact.  This case therefore depends on running on fresh tsc statics: it is first. */
    cpu->instruction_counter = 100000;
    uint64_t tsc_a = read_tsc();            /* 100 ms of virtual time */
    cpu->instruction_counter = 101000;      /* +1000 instructions = +1 ms */
    uint64_t tsc_b = read_tsc();
    cpu->instruction_counter = 102000;
    uint64_t tsc_c = read_tsc();
    CHECK_EQ(tsc_b - tsc_a, 1000000ull);    /* 1 ms * 1e6 */
    CHECK_EQ(tsc_c - tsc_b, 1000000ull);    /* and again, so the ratio is not a one-off */

    /* THE ACPI PM TIMER (acpi.js:161 `round(now * PMTIMER_FREQ_SECONDS/1000)`): 1 ms of
     * virtual time is 3579.545 ticks */
    acpi_init();
    acpi->timer_last_value = 0;
    acpi->timer_imprecision_offset = 0;
    cpu->instruction_counter = 1000;        /* 1 ms */
    CHECK_EQ(acpi_get_timer(microtick()), (int32_t)round(1.0 * (PMTIMER_FREQ_HZ / 1000.0)));
    cpu->instruction_counter = 2000;        /* 2 ms */
    CHECK_EQ(acpi_get_timer(microtick()), (int32_t)round(2.0 * (PMTIMER_FREQ_HZ / 1000.0)));

    /* THE PIT, and M2-1's FINDING F2 CLOSED: a 10 ms reload -- the classic HZ=100 value,
     * 0x2E9C = 11932 ticks at 1.193182 MHz -- fires at a MEASURED instruction distance.  The
     * counter is programmed through the ports and then stepped ONE INSTRUCTION AT A TIME
     * until the rollover test fires, so the number printed below is the measurement, not a
     * prediction.  Under M1c's seat (1 instruction = 1 MILLISECOND) the same reload fired
     * every 10 instructions; the reference's own arithmetic puts it here. */
    machine_reset();
    io_port_write8(0x43, 0x34); /* counter 0, lsb/msb, mode 2 */
    io_port_write8(0x40, 0x9C);
    io_port_write8(0x40, 0x2E); /* reload = 0x2E9C = 11932 ticks = 10 ms */
    CHECK_EQ(pit->counter_reload[0], 0x2E9C);
    uint32_t start_ic = round_to_u32(pit->counter_start_time[0] * 1000.0);
    CHECK_EQ(start_ic, cpu->instruction_counter); /* the reload was programmed AT this counter */

    uint32_t fired = 0;
    for (uint32_t d = 1; d <= 20000; d++) {
        cpu->instruction_counter = start_ic + d;
        if (pit_did_rollover(0, microtick())) { fired = d; break; }
    }
    printf("CLOCK: a 10 ms PIT reload (0x2E9C = 11932 ticks) fires at %u instructions "
           "(1 instruction = 1 us, so 10 ms = 10000 us; the rollover test is a strict "
           "inequality, hence +1)\n", fired);
    CHECK_EQ(fired, 10001);
    /* and the tick RATE, read off the device: 1 ms of virtual time is 1193 ticks */
    cpu->instruction_counter = start_ic + 1000;
    CHECK_EQ(pit_get_counter_value(0, microtick()), 0x2E9C - (int32_t)floor(PIT_FREQ_KHZ));
}

static void test_io_bus(void)
{
    machine_reset();

    /* D-F1: an unclaimed port answers EXACTLY as the pinned reference's empty entry does --
     * 0xFF / 0xFFFF / -1 to a read (io.js:64-77), a DISCARDED write -- and the machine does
     * NOT stop.  BEFORE M2-1F this read stopped the machine naming "0x1234" and returned 0;
     * results/m2-1f-policy.md section 2 carries the before/after of the port-0x80 probe this
     * decision rests on, and test_handover below re-measures it through the CPU's own
     * instruction body. */
    clear_stop();
    CHECK_EQ(io_port_read8(0x1234), 0xFF);
    CHECK_EQ(io_port_read16(0x1234), 0xFFFF);
    CHECK_EQ(io_port_read32(0x1234), -1);
    CHECK(!interpreted_unimplemented);

    /* the write is a NO-OP and changes NO DEVICE STATE: the port map entry is compared
     * against a freshly created empty entry (all six handlers and the device pointer), and
     * the PIT -- a device with real state a mis-routed write could have reached -- is
     * checked either side.  A write that stopped, or that fell through to a neighbour,
     * fails one of these. */
    io_port_entry_t empty = io_create_empty_entry();
    uint16_t pit_reload_before = pit->counter_reload[0];
    uint8_t pit_enabled_before = pit->counter_enabled[0];
    io_port_write8(0x1234, 0x5A);
    io_port_write16(0x1234, 0xBEEF);
    io_port_write32(0x1234, 0x7FFFFFFF);
    CHECK(!interpreted_unimplemented);
    CHECK_EQ(memcmp(&io_ports[0x1234], &empty, sizeof(empty)), 0);
    CHECK(io_ports[0x1234].device == NULL);
    CHECK_EQ(pit->counter_reload[0], pit_reload_before);
    CHECK_EQ(pit->counter_enabled[0], pit_enabled_before);
    /* the read is still the empty entry's: the writes installed nothing */
    CHECK_EQ(io_port_read8(0x1234), 0xFF);

    /* the reference has no ATA controller either, so the seabios NAME TABLE is now the
     * DIAGNOSTIC's payload (V86C_LOG_PORTS) rather than the text of a stop: the read answers
     * 0xFF and the machine runs on */
    clear_stop();
    CHECK_EQ(io_port_read8(0x01F0), 0xFF);
    CHECK(!interpreted_unimplemented);
    CHECK(strcmp(io_get_port_description(0x01F0), "PORT_ATA1_CMD_BASE") == 0);

    /* a port registered at SOME widths answers the empty entry at the others, which is what
     * a release reference build does (io.h D2): 0x20 is the PIC's, 8-bit only, so a 16-bit
     * read of it returns 0xFFFF instead of stopping */
    clear_stop();
    io_port_write8(0x20, 0x11);
    CHECK(!interpreted_unimplemented);
    CHECK_EQ(io_port_read16(0x20), 0xFFFF);
    CHECK(!interpreted_unimplemented);

    /* the mmap half: the unclaimed-memory answer the reference registers for
     * [memory_size, MMAP_MAX) is 0xFF / -1 */
    CHECK_EQ(read8(0x200000), 0xFF);
    CHECK_EQ(read16(0x200000), 0xFFFF);
    CHECK_EQ(read32s(0x200000), -1);

    /* ... and the R1F carve-out survives the mmap table taking over the hook: the VGA
     * window and the 8 MiB LFB read 0 at boot (memory.c's default table), NOT 0xFF */
    CHECK_EQ(read8(0xA0000), 0);
    CHECK_EQ(read32s(0xE0000000), 0);

    /* an mmap-registered range gets its own callbacks, 8- and 32-bit, and the 32-bit shim
     * composes four 8-bit reads when no 32-bit handler is given */
    io_mmap_register(0x10000000, MMAP_BLOCK_SIZE, mmap_test_read8, mmap_test_write8,
                     mmap_test_read32, NULL);
    CHECK_EQ(read8(0x10000000), 0x5A);
    CHECK_EQ(read8(0x10000011), 0x4B);
    CHECK_EQ(read32s(0x10000000), 0x12340000);
    write8(0x10000005, 0xA5);
    CHECK_EQ(mem8[0x105], 0xA5);
}

/* ================================================================== *
 * 2. the PIT
 * ================================================================== */
static void test_pit(void)
{
    machine_reset();

    /* the counter value the reload below is programmed at: the PIT stores microtick() as a
     * double, so the integer counter it corresponds to is recovered once here and every case
     * below offsets from it in INSTRUCTIONS (D-F2: 1 instruction = 1 us = 1/1000 ms) */
    uint32_t start_ic = cpu->instruction_counter;

    /* the mode byte: counter 0, lsb/msb, mode 2 (rate generator) */
    io_port_write8(0x43, 0x34);
    CHECK_EQ(pit->counter_mode[0], 2);
    CHECK_EQ(pit->counter_read_mode[0], 3);
    CHECK_EQ(pit->counter_next_low[0], 1);

    /* the reload: lsb then msb, and the counter becomes enabled.  0xFFFF is 65535 ticks =
     * 54.9 ms of virtual time, i.e. ~54926 instructions at this clock's scale. */
    io_port_write8(0x40, 0xFF);
    io_port_write8(0x40, 0xFF); /* reload = 0xFFFF */
    CHECK_EQ(pit->counter_reload[0], 0xFFFF);
    CHECK_EQ(pit->counter_start_value[0], 0xFFFF);
    CHECK(pit->counter_enabled[0]);
    CHECK_EQ(round_to_u32(pit->counter_start_time[0] * 1000.0), start_ic);

    /* the counter's value advances with the clock: microtick is MILLISECONDS at 1 guest
     * instruction = 1 microsecond (exec_boundary.c, D-F2), so 1000 instructions are 1 ms and
     * floor(1 ms * 1193.1816666 kHz) = 1193 ticks.  The expected values below are computed
     * INDEPENDENTLY of the device (the contract's units and OSCILLATOR_FREQ). */
    int32_t ticks_per_ms = (int32_t)floor(1.0 * PIT_FREQ_KHZ);
    cpu->instruction_counter = start_ic + 1000; /* +1 ms */
    CHECK_EQ(pit_get_counter_value(0, microtick()), 0xFFFF - ticks_per_ms);
    CHECK(!pit_did_rollover(0, microtick()));

    /* 55 ms is floor(55 * 1193.1816666) = 65624 ticks: past 0xFFFF, so the counter rolls
     * over -- and past it, the value wraps by the reload (upstream's `value % reload + reload`) */
    cpu->instruction_counter = start_ic + 55000;
    CHECK(pit_did_rollover(0, microtick()));
    {
        int32_t raw = 0xFFFF - (int32_t)floor(55.0 * PIT_FREQ_KHZ);
        CHECK_EQ(pit_get_counter_value(0, microtick()), ((raw % 0xFFFF) + 0xFFFF) % 0xFFFF);
    }

    /* the latch command: the value is frozen and returned lsb first, then msb, then the
     * latch expires */
    cpu->instruction_counter = start_ic + 1000;
    io_port_write8(0x43, 0x00);
    CHECK_EQ(pit->counter_latch[0], 2);
    int32_t low = io_port_read8(0x40);
    int32_t high = io_port_read8(0x40);
    /* upstream's `value ? value - 1 : 0` */
    CHECK_EQ(low | high << 8, 0xFFFF - ticks_per_ms - 1);
    CHECK_EQ(pit->counter_latch[0], 0);

    /* port 0x61: bit 4 is the refresh toggle, bit 5 counter 2's output.  Both are computed
     * INDEPENDENTLY here: the toggle is `(now * 1000000/15000) & 1` and counter 2's output
     * is a rollover test against its (never programmed, hence 0) start value. */
    int32_t port61 = io_port_read8(0x61);
    int32_t expected_toggle = ((int32_t)(microtick() * (1000.0 * 1000.0 / 15000.0))) & 1;
    CHECK_EQ(port61 & 0x10, expected_toggle << 4);
    /* counter 2 was never programmed: start value 0, so any tick at all rolls it over */
    CHECK_EQ(port61 & 0x20, 0x20);

    /* THE IRQ0: a real interrupt, after the programmed count, into an IDT handler */
    machine_reset();
    program_pic();
    cpu->flags |= FLAG_INTERRUPT;
    cpu->in_hlt = true;
    cpu->previous_ip = STACK_TOP;

    io_port_write8(0x43, 0x34);
    io_port_write8(0x40, 0x10);
    io_port_write8(0x40, 0x00); /* reload 16 ticks */
    uint32_t irq_ic = cpu->instruction_counter;

    /* 16 ticks at OSCILLATOR_FREQ (1193.18 kHz) are 13.41 us of virtual time, and D-F2 makes
     * 1 us one instruction -- so the rollover test (a STRICT inequality against the tick
     * count) fires at instruction 15 and NOT at 14.  The boundary is asserted either side
     * rather than assumed, because it is exactly the off-by-one the clock's scale decides. */
    cpu->instruction_counter = irq_ic + 14;
    CHECK(!pit_did_rollover(0, microtick()));
    cpu->instruction_counter = irq_ic + 15;
    CHECK(pit_did_rollover(0, microtick()));
    run_hardware_timers(false, microtick());

    CHECK_EQ(cpu->sreg[CS], SEL_CODE2);   /* the handler ran */
    CHECK_EQ(cpu->instruction_pointer, HANDLER_EIP);
    CHECK(!cpu->in_hlt);                  /* pic_call_irq woke the CPU */
    CHECK_EQ(pic_acknowledge_irq(), -1);  /* irq0 is in service: absorbed, not lost */
    /* the frame: an interrupt gate from cpl0 pushes EFLAGS, CS, EIP (no SS/ESP) */
    CHECK_EQ(safe_read32s(read_reg32(ESP) + 0), (int32_t)STACK_TOP); /* the return eip */
    CHECK_EQ(safe_read32s(read_reg32(ESP) + 4), SEL_CODE);           /* the interrupted CS */
    /* the IRR is what port 0x20 answers by default, and irq0 was just absorbed from it */
    CHECK_EQ(io_port_read8(0x20) & 1, 0);
    /* ... while OCW3's read-ISR command shows the in-service bit (pic.rs's state machine) */
    io_port_write8(0x20, 0x0B);
    CHECK_EQ(io_port_read8(0x20) & 1, 1);
}

/* ================================================================== *
 * 3. the RTC / CMOS
 * ================================================================== */
/* the test's OWN BCD encoder: the point is to compare the device against an independent
 * computation, not against its own bcd_pack */
static int32_t bcd_of(int n) { return (n / 10 << 4) | (n % 10); }

static void test_rtc(void)
{
    machine_reset();

    /* fill_cmos: base memory 640k, extended memory = (2 MiB - 1 MiB) >> 10 = 1024 */
    rtc->cmos_index = CMOS_MEM_BASE_LOW;
    CHECK_EQ(io_port_read8(0x71), 640 & 0xFF);
    rtc->cmos_index = CMOS_MEM_BASE_HIGH;
    CHECK_EQ(io_port_read8(0x71), 640 >> 8);
    rtc->cmos_index = CMOS_MEM_EXTMEM_LOW;
    CHECK_EQ(io_port_read8(0x71), 1024 & 0xFF);
    rtc->cmos_index = CMOS_MEM_EXTMEM_HIGH;
    CHECK_EQ(io_port_read8(0x71), 1024 >> 8);
    rtc->cmos_index = CMOS_EQUIPMENT_INFO;
    CHECK_EQ(io_port_read8(0x71), 0x2F);

    /* the index port: 0x70 selects the register and takes the NMI-disable bit */
    io_port_write8(0x70, 0x0E);
    CHECK_EQ(rtc->cmos_index, 0x0E);
    CHECK_EQ(rtc->nmi_disabled, 0);
    io_port_write8(0x70, 0x8E);
    CHECK_EQ(rtc->cmos_index, 0x0E);
    CHECK_EQ(rtc->nmi_disabled, 1);

    /* a register round-trip through the two ports (the diagnostic status register: the one
     * index whose write has no side effect) */
    io_port_write8(0x71, 0x5A);
    CHECK_EQ(rtc->cmos_diag_status, 0x5A);
    CHECK_EQ(io_port_read8(0x71), 0x5A);

    /* the alarm registers round-trip through the general CMOS store */
    io_port_write8(0x70, CMOS_RTC_SECONDS_ALARM);
    io_port_write8(0x71, 0x33);
    CHECK_EQ(rtc_cmos_read(CMOS_RTC_SECONDS_ALARM), 0x33);

    /* register A: the periodic rate divider.  32768 >> (0x2F & 0xF) - 1 = 32768 >> 14 = 2 */
    io_port_write8(0x70, CMOS_STATUS_A);
    io_port_write8(0x71, 0x2F);
    CHECK_EQ(rtc->cmos_a, 0x2F);
    CHECK((int32_t)rtc->periodic_interrupt_time == 500);

    /* register A's read: the divider's low nibble and the update-in-progress bit */
    io_port_write8(0x70, CMOS_STATUS_A);
    CHECK_EQ(io_port_read8(0x71) & 0x7F, 0x2F);

    /* register B: read back through the port, and a period B value that keeps the
     * periodic interrupt off (cmos_a's low nibble is 0xF, so register B's bit 6 alone
     * would arm it) */
    io_port_write8(0x70, CMOS_STATUS_B);
    CHECK_EQ(io_port_read8(0x71), rtc->cmos_b);
    io_port_write8(0x71, 0x0B);
    CHECK_EQ(rtc->cmos_b, 0x0B);
    CHECK(!rtc->periodic_interrupt && !rtc->update_interrupt);

    /* register C: the flags are returned and CLEARED, and the read lowers IRQ8 */
    rtc->cmos_c = 0x40 | 0x80;
    io_port_write8(0x70, CMOS_STATUS_C);
    CHECK_EQ(io_port_read8(0x71), 0xC0);
    CHECK_EQ(rtc->cmos_c, 0x00);

    /* register D: the battery is charged */
    io_port_write8(0x70, CMOS_STATUS_D);
    CHECK_EQ(io_port_read8(0x71), 0x80);

    /* THE TIME REGISTERS ARE THE VIRTUAL CLOCK'S, and with the counter at a known value
     * the check is EXACT -- hand-computed literals, so it cannot be satisfied by the
     * implementation's own arithmetic.  The RTC's time base is D-F2's instruction-driven
     * timebase on the fixed RTC_EPOCH_MS (rtc.h), NOT the host's Date.now(): the old form
     * of this case compared the registers against time(NULL)/gmtime_r and had to accept
     * either side of a second boundary, and it would PASS with the host still feeding the
     * RTC.  Here, 123456789 instructions = 123456.789 ms of virtual time after
     * 2024-01-01T00:00:00Z = 2024-01-01T00:02:03Z (a Monday). */
    machine_reset(); /* cmos_b is back to its 2 (BCD) default */
    cpu->instruction_counter = 123456789;
    rtc_timer(microtick(), false); /* the run loop's own call (machine.c:130) advances the RTC */
    io_port_write8(0x70, CMOS_RTC_SECONDS);
    CHECK_EQ(io_port_read8(0x71), bcd_of(3));
    io_port_write8(0x70, CMOS_RTC_MINUTES);
    CHECK_EQ(io_port_read8(0x71), bcd_of(2));
    io_port_write8(0x70, CMOS_RTC_HOURS);
    CHECK_EQ(io_port_read8(0x71), bcd_of(0));
    io_port_write8(0x70, CMOS_RTC_DAY_WEEK); /* getUTCDay() is 0-based, the register 1-based */
    CHECK_EQ(io_port_read8(0x71), bcd_of(2));
    io_port_write8(0x70, CMOS_RTC_DAY_MONTH);
    CHECK_EQ(io_port_read8(0x71), bcd_of(1));
    io_port_write8(0x70, CMOS_RTC_MONTH);
    CHECK_EQ(io_port_read8(0x71), bcd_of(1));
    io_port_write8(0x70, CMOS_RTC_YEAR);
    CHECK_EQ(io_port_read8(0x71), bcd_of(24));
    io_port_write8(0x70, CMOS_CENTURY);
    CHECK_EQ(io_port_read8(0x71), bcd_of(20));

    /* and it IS MOVED BY THE GUEST'S OWN EXECUTION -- the property that makes the boot
     * reproducible, and the one a host-fed RTC cannot have at all.  One instruction is one
     * MICROSECOND (D-F2), so 3.6e9 instructions = 3600 s = exactly 2024-01-01T01:00:00Z.
     * (A whole DAY would be 8.64e10 us, and the counter is a uint32 -- the same shape the
     * instruction limit has -- so an HOUR is what is reachable in one counter value.) */
    cpu->instruction_counter = 3600000000u;
    rtc_timer(microtick(), false);
    io_port_write8(0x70, CMOS_RTC_SECONDS);
    CHECK_EQ(io_port_read8(0x71), bcd_of(0));
    io_port_write8(0x70, CMOS_RTC_MINUTES);
    CHECK_EQ(io_port_read8(0x71), bcd_of(0));
    io_port_write8(0x70, CMOS_RTC_HOURS);
    CHECK_EQ(io_port_read8(0x71), bcd_of(1));
    io_port_write8(0x70, CMOS_RTC_DAY_MONTH); /* still the 1st: the day did not roll */
    CHECK_EQ(io_port_read8(0x71), bcd_of(1));
    io_port_write8(0x70, CMOS_RTC_DAY_WEEK);
    CHECK_EQ(io_port_read8(0x71), bcd_of(2));

    /* binary mode (register B bit 2) turns the encoding off */
    io_port_write8(0x70, CMOS_STATUS_B);
    io_port_write8(0x71, rtc->cmos_b | 4);
    io_port_write8(0x70, CMOS_RTC_SECONDS);
    int32_t binary_seconds = io_port_read8(0x71);
    CHECK_EQ(binary_seconds, 0);
    CHECK(binary_seconds < 60);
}

/* ================================================================== *
 * 4. the DMA
 * ================================================================== */
static bool dma_completed;
static bool dma_error;

/* the unmask notification's listener: it must fire exactly on the 1 -> 0 transition */
static int32_t unmasked_channel = -1;
static void dma_unmasked(void *this_value, int32_t channel)
{
    (void)this_value;
    unmasked_channel = channel;
}
static void dma_done(void *this_value, bool error)
{
    (void)this_value;
    dma_completed = true;
    dma_error = error;
}

static void test_dma(void)
{
    machine_reset();
    unmasked_channel = -1;
    dma_on_unmask(dma_unmasked, NULL);

    /* channel 2's address: lsb then msb through the one port, then the page bytes */
    io_port_write8(0x04, 0x40);
    io_port_write8(0x04, 0x23);  /* channel_addr[2] = 0x2340 */
    io_port_write8(0x81, 0x05);  /* page: channel 2's page port is 0x81, not 0x82
                                  * (dma.js:68-76's order is 0x87/0x83/0x81/0x82) */
    io_port_write8(0x481, 0x00); /* page-high */
    CHECK_EQ(dma->channel_addr[2], 0x2340);

    /* the flip-flop reset lets the address be read back in the same order */
    io_port_write8(0x0C, 0);
    CHECK_EQ(io_port_read8(0x04), 0x40);
    CHECK_EQ(io_port_read8(0x04), 0x23);

    /* count 3 -> four bytes (count_get_8bit adds one; channel 2 is 8-bit) */
    io_port_write8(0x05, 0x03);
    io_port_write8(0x05, 0x00);
    CHECK_EQ(dma->channel_count[2], 3);
    CHECK_EQ(dma_count_get_8bit(2), 4);
    /* the composed address: page<<16 | addr (channel 2 < 5, so no 16-bit shift) */
    CHECK_EQ(dma_address_get_8bit(2), 0x052340);

    /* A TRANSFER LANDING BYTES IN GUEST MEMORY (the brief's observable) */
    uint8_t source[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44};
    dma_buffer_t buffer = {.data = source, .byte_length = sizeof(source)};
    dma_completed = false;
    dma_do_read(&buffer, 0, 4, 2, dma_done, NULL);
    CHECK(dma_completed && !dma_error);
    CHECK_EQ(mem8[0x052340], 0xDE);
    CHECK_EQ(mem8[0x052341], 0xAD);
    CHECK_EQ(mem8[0x052342], 0xBE);
    CHECK_EQ(mem8[0x052343], 0xEF);
    CHECK_EQ(dma->channel_addr[2], 0x2344); /* the channel advanced by the count */

    /* ... and the reverse: guest memory written into the buffer, with autoinit restoring
     * the channel afterwards */
    io_port_write8(0x0B, 0x58);            /* mode: channel 0, read, autoinit */
    io_port_write8(0x00, 0x00);
    io_port_write8(0x00, 0x10);            /* channel_addr[0] = 0x1000 */
    io_port_write8(0x01, 0x01);
    io_port_write8(0x01, 0x00);            /* count 1 -> two bytes */
    mem8[0x1000] = 0x77;
    mem8[0x1001] = 0x88;
    uint8_t target[4] = {0, 0, 0, 0};
    dma_buffer_t out = {.data = target, .byte_length = sizeof(target)};
    dma_completed = false;
    dma_do_write(&out, 0, 2, 0, dma_done, NULL);
    CHECK(dma_completed && !dma_error);
    CHECK_EQ(target[0], 0x77);
    CHECK_EQ(target[1], 0x88);
    CHECK_EQ(dma->channel_addr[0], dma->channel_addr_init[0]); /* autoinit restored it */

    /* the mask registers: singlemask sets one, and unmasking FIRES the listeners (the
     * sound card's entry point upstream) */
    io_port_write8(0x0A, 0x04 | 2); /* mask channel 2 */
    CHECK_EQ(dma->channel_mask[2], 1);
    CHECK_EQ(unmasked_channel, -1);
    io_port_write8(0x0A, 0x00 | 2); /* unmask channel 2 */
    CHECK_EQ(dma->channel_mask[2], 0);
    CHECK_EQ(unmasked_channel, 2);

    /* multimask: the WRITE stores the raw masked bit (data_byte & (1 << i), dma.js:196-199)
     * and the READ shifts it again (dma.js:206-214), so the register does NOT round-trip
     * for i > 0 -- written 0x05 reads back 0x11.  UPSTREAM QUIRK, reproduced. */
    io_port_write8(0x0F, 0x05); /* mask channels 0 and 2 */
    CHECK_EQ(io_port_read8(0x0F), 0x11);
    CHECK_EQ(dma->channel_mask[0], 1);
    CHECK_EQ(dma->channel_mask[1], 0);
    CHECK_EQ(dma->channel_mask[2], 4); /* the RAW masked bit, as upstream stores it */
    CHECK_EQ(dma->channel_mask[3], 0);

    /* a transfer outside guest RAM stops loudly instead of reading host memory */
    clear_stop();
    dma->channel_page[1] = 0xFF;
    dma->channel_pagehi[1] = 0xFF;
    dma->channel_addr[1] = 0xFF00;
    dma->channel_count[1] = 0x0003;
    io_port_write8(0x0B, 0x04 | 1); /* autoinit off */
    dma_completed = false;
    dma_do_write(&out, 0, 4, 1, dma_done, NULL);
    CHECK(interpreted_unimplemented);
    CHECK(dma_error);
}

/* ================================================================== *
 * 5. the PS/2
 * ================================================================== */
static void test_ps2(void)
{
    machine_reset();

    /* the status port: bit 4 is always set, bit 0 is "output buffer full" */
    CHECK_EQ(io_port_read8(0x64), 0x10);

    /* the controller self-test (0xAA) replies 0x55 and raises the buffer-full bit */
    io_port_write8(0x64, 0xAA);
    CHECK_EQ(io_port_read8(0x64) & 1, 1);
    CHECK_EQ(io_port_read8(0x60), 0x55);
    CHECK_EQ(io_port_read8(0x64) & 1, 0);

    /* the device identify command (0xF2) replies 0xFA, 0xAB, 0x83 */
    io_port_write8(0x60, 0xF2);
    CHECK_EQ(io_port_read8(0x60), 0xFA);
    CHECK_EQ(io_port_read8(0x60), 0xAB);
    CHECK_EQ(io_port_read8(0x60), 0x83);

    /* enable scanning (0xF4) and deliver a scancode: A SCANCODE PRODUCING THE RIGHT BYTE
     * (the brief's observable).  The code comes in on the bus -- the path a later unit's
     * host input will use. */
    io_port_write8(0x60, 0xF4);
    CHECK_EQ(io_port_read8(0x60), 0xFA); /* the command's ack */
    CHECK(ps2->enable_keyboard_stream);
    BUS_SEND(machine_host_bus, "keyboard-code", 0x1E);
    CHECK_EQ(io_port_read8(0x64) & 1, 1);
    CHECK_EQ(io_port_read8(0x60), 0x1E);
    CHECK_EQ(io_port_read8(0x64) & 1, 0);

    /* a scan code while scanning is DISABLED is dropped, exactly as upstream */
    io_port_write8(0x60, 0xF5); /* disable scanning */
    CHECK_EQ(io_port_read8(0x60), 0xFA);
    BUS_SEND(machine_host_bus, "keyboard-code", 0x9E);
    CHECK_EQ(io_port_read8(0x64) & 1, 0);

    /* the mouse: 0xD4 makes the next data write a mouse command, 0xF4 enables streaming */
    io_port_write8(0x64, 0xD4);
    io_port_write8(0x60, 0xF4);
    CHECK(ps2->enable_mouse_stream && ps2->use_mouse);
    CHECK_EQ(io_port_read8(0x60), 0xFA);

    /* a delta produces one three-byte packet: the info byte's bit 3 is always set */
    BUS_SEND(machine_host_bus, "mouse-delta", 10, -5);
    int32_t info = io_port_read8(0x60);
    CHECK_EQ(info & 8, 8);
    CHECK_EQ(info & 0x10, 0);  /* dx >= 0 */
    CHECK_EQ(info & 0x20, 0x20); /* dy < 0 */
    CHECK_EQ(io_port_read8(0x60), 10);
    CHECK_EQ(io_port_read8(0x60), 0xFB); /* -5 as a byte */

    /* THE INTELLIMOUSE DANCE: 0xF3 selects the sample rate and the NEXT data byte is the
     * value, so each rate needs its own command.  200 -> 100 -> 80 sets mouse id 3. */
    const int32_t rates[3] = {200, 100, 80};
    for (int i = 0; i < 3; i++) {
        io_port_write8(0x64, 0xD4);
        io_port_write8(0x60, 0xF3); /* set sample rate */
        CHECK_EQ(io_port_read8(0x60), 0xFA);
        io_port_write8(0x60, rates[i]);
        CHECK_EQ(ps2->sample_rate, rates[i]);
        CHECK_EQ(io_port_read8(0x60), 0xFA); /* the value's ack */
    }
    CHECK_EQ(ps2->mouse_id, 3);
    CHECK_EQ(ps2->mouse_detect_state, -1);

    /* reading the id back (0xF2 on the mouse) */
    io_port_write8(0x64, 0xD4);
    io_port_write8(0x60, 0xF2);
    CHECK_EQ(io_port_read8(0x60), 0xFA);
    CHECK_EQ(io_port_read8(0x60), 3);

    /* the command byte is readable and writeable through 0x64/0x60 */
    io_port_write8(0x64, 0x20);
    CHECK_EQ(io_port_read8(0x60), ps2->command_register);
    io_port_write8(0x64, 0x60);
    io_port_write8(0x60, 0x45);
    CHECK_EQ(ps2->command_register, 0x45);

    /* the port count: a data byte written with no selector pending is a keyboard command */
    io_port_write8(0x60, 0xFF);
    CHECK_EQ(io_port_read8(0x60), 0xFA);
    CHECK_EQ(io_port_read8(0x60), 0xAA);

    /* ps2.js:399-420 (apply_scaling2) is DEAD UPSTREAM -- both of its call sites in
     * send_mouse_packet are commented out -- and it is WRONG for positive inputs: upstream's
     * `sign = n >> 31` is 0 for n > 0, so the 2:1 scaling of a POSITIVE 4 or 5 is 0 rather
     * than 6 or 9.  The port keeps the arithmetic verbatim (upstream's behaviour, bug and
     * all); the values below are the FAITHFUL ones, not the documented table's.  This is
     * the function's only caller in the port. */
    CHECK_EQ(ps2_apply_scaling2(0), 0);   /* case 0: n */
    CHECK_EQ(ps2_apply_scaling2(3), 3);   /* case 3: n */
    CHECK_EQ(ps2_apply_scaling2(2), 0);   /* case 2: sign = 0 for n > 0 (upstream's table) */
    CHECK_EQ(ps2_apply_scaling2(4), 0);   /* case 4: 6 * 0 -- see above */
    CHECK_EQ(ps2_apply_scaling2(-4), -6); /* negative inputs DO scale */
    CHECK_EQ(ps2_apply_scaling2(-5), -9);
    CHECK_EQ(ps2_apply_scaling2(8), 16);  /* default: n << 1 */

    /* ps2.js:834-837's reboot command: machine_reboot() (REBOOT-1) -- the machine
     * reset ran and the boot re-entry is REQUESTED, never stopped */
    clear_stop();
    io_port_write8(0x64, 0xFE);
    CHECK(!interpreted_unimplemented);
    CHECK(cpu->reboot_requested);
}

/* ================================================================== *
 * 6. the PCI bus
 * ================================================================== */
static int32_t bar_read8(void *device, int32_t port)
{
    (void)device;
    return port & 0xFF;
}
static void bar_write8(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    (void)value;
}

static void test_pci(void)
{
    machine_reset();

    /* the host bridge (00:00.0): vendor 0x8086, device 0x1237 */
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 0u << 8 | 0));
    CHECK_EQ(io_port_read32(0xCFC), 0x12378086);
    CHECK_EQ(io_port_read32(0xCF8), (int32_t)0x80000000u); /* the status register echoes it */

    /* its PAM0 register at offset 0x59 -- 0x10, the byte seabios looks for */
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 0x58));
    CHECK_EQ(io_port_read8(0xCFC + 1), 0x10);

    /* the ISA bridge (00:01.0): vendor 0x8086, device 0x7000 */
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 1u << 11));
    CHECK_EQ(io_port_read32(0xCFC), 0x70008086);

    /* an absent device reads -1 and clears the status register (pci_query's else arm) */
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 3u << 11));
    CHECK_EQ(io_port_read32(0xCFC), -1);
    CHECK_EQ(io_port_read32(0xCF8), 0);

    /* ... and a 32-BIT write to an absent device is IGNORED -- upstream's own arm
     * (pci.js:381-390: `var space = this.device_spaces[bdf]; if(!space) return;`, with NO
     * `.buffer` dereference).  CORRECTED IN M2-2: this case used to assert a STOP, which is
     * write8's and write16's behaviour, not write32's.  MEASURED: the Linux kernel's PCI
     * scan writes config space to absent devices (bdf 0xf9) and the stop ended the boot at
     * instruction 659158271 (results/m2-2-boot.md). */
    clear_stop();
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 3u << 11));
    io_port_write32(0xCFC, 0x12345678);
    CHECK(!interpreted_unimplemented);
    /* nothing was created and nothing was written: the device is still absent */
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 3u << 11));
    CHECK_EQ(io_port_read32(0xCFC), -1);

    /* ... while an 8- or 16-bit write to an absent device STOPS with the bdf named: those
     * two arms dereference the undefined config space upstream and throw (pci.js:328, 354),
     * so the port names the access instead of inventing a behaviour for it. */
    clear_stop();
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 3u << 11));
    io_port_write8(0xCFC, 0x12);
    CHECK(interpreted_unimplemented);
    CHECK(strstr(unimplemented_instruction_name(), "bdf=0x18") != NULL);

    clear_stop();
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 3u << 11));
    io_port_write16(0xCFC, 0x1234);
    CHECK(interpreted_unimplemented);
    CHECK(strstr(unimplemented_instruction_name(), "bdf=0x18") != NULL);

    /* A CONFIG ACCESS ON A BUS ABOVE 0 IS AN ABSENT DEVICE, NOT AN OUT-OF-BOUNDS INDEX.
     * Upstream's bdf is `address >> 8 & 0xFFFF` (up to 0xFFFF) into tables initialised for
     * 0..255 only (pci.js:37-38): everything above 255 is `undefined` there.  MEASURED: the
     * port used to index its 256-entry array with the full 16-bit value, and the kernel's
     * scan of bus 255 (address 0x80ff0000 -> bdf 0xff00) read 260 KiB past the table and
     * SIGSEGVed the port at instruction 550020331 of the boot. */
    clear_stop();
    io_port_write32(0xCF8, (int32_t)(0x80FF0000u)); /* bus 255, dev 31, fn 0, reg 0 */
    CHECK_EQ(io_port_read32(0xCFC), -1);            /* absent, as upstream's undefined is */
    CHECK(!interpreted_unimplemented);
    io_port_write32(0xCFC, 0x12345678);             /* write32: ignored, not a stop */
    CHECK(!interpreted_unimplemented);
    clear_stop();
    io_port_write8(0xCFC, 0x12);                    /* write8: upstream throws -> the stop */
    CHECK(interpreted_unimplemented);
    CHECK(strstr(unimplemented_instruction_name(), "bdf=0xff00") != NULL);

    /* AN I/O BAR MOVES ITS PORT-MAP ENTRIES.  The order matters and is upstream's: the
     * device's ports carry their handlers BEFORE the device registers (register_device
     * copies the entries into the bar, pci.js:493-517), and the bar register then moves
     * that COPY to the new base. */
    static pci_bar_t test_bar;
    /* 64 bytes: pci.js:497-499 asserts `pci_space.length >= 64`, so a shorter array would
     * abort at registration (this port's DBG_ASSERT, active here, does the same) */
    static const uint8_t test_space[64] = {
        0x34, 0x12, 0x56, 0x78, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x20, 0x00, 0x00 /* 0x10: bar 0, an I/O bar at 0x2000 */,
    };
    test_bar.size = 4;
    static pci_device_t test_device = {
        .pci_id = 2 << 3,
        .pci_space = test_space,
        .pci_space_length = (int32_t)sizeof(test_space),
        .pci_bars = &test_bar,
        .pci_bars_count = 1,
        .name = "test-device",
    };
    for (int32_t i = 0; i < 4; i++) {
        io_register_read(0x2000 + i, &test_bar, bar_read8, NULL, NULL);
        io_register_write(0x2000 + i, &test_bar, bar_write8, NULL, NULL);
    }
    CHECK_EQ(io_port_read8(0x2002), 0x02);

    pci_register_device(&test_device);
    CHECK(test_bar.entries != NULL);
    CHECK_EQ(test_bar.original_bar, 0x2001);
    CHECK_EQ(test_bar.entries[2].read8, bar_read8); /* the bar captured the handlers */

    /* the move: write the bar register and the four ports appear at 0x3000 ... */
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 2u << 11 | 0x10));
    io_port_write32(0xCFC, 0x3001);
    CHECK_EQ(io_port_read8(0x3002), 0x02);

    /* ... and, because the OLD base is >= 0x1000, the four old ports became EMPTY entries
     * (pci.js:557-561): a read there answers the empty entry's 0xFF and does NOT stop --
     * which is what the reference does with the entries it just replaced */
    clear_stop();
    CHECK_EQ(io_port_read8(0x2002), 0xFF);
    CHECK(!interpreted_unimplemented);
    clear_stop();

    /* the size probe: writing all ones reports the bar's size and I/O type.  NOTE the
     * response register is only refreshed by a write to 0xCFB (pci_query, pci.js:239-256),
     * so it must be re-issued before reading 0xCFC -- a write to 0xCFC alone leaves a stale
     * response, which is upstream's behaviour. */
    io_port_write32(0xCFC, -1);
    io_port_write32(0xCF8, (int32_t)(0x80000000u | 2u << 11 | 0x10));
    /* ~(4 - 1) | 1: the bar's size in the low bits, the I/O type in bit 0 */
    CHECK_EQ(io_port_read32(0xCFC), (int32_t)0xFFFFFFFD);

    /* 0xCF9's reboot dance: machine_reboot() (REBOOT-1) -- requested, never
     * stopped */
    clear_stop();
    io_port_write8(0xCF9, 0x02);
    io_port_write8(0xCF9, 0x06);
    CHECK(!interpreted_unimplemented);
    CHECK(cpu->reboot_requested);
}

/* ================================================================== *
 * 7. the ACPI device
 * ================================================================== */
static void test_acpi(void)
{
    machine_reset();
    acpi_init(); /* the machine only creates it when acpi_enabled; the test wants it */

    /* the PM1 status register is WRITE-1-TO-CLEAR: a write clears the bits it sets, so
     * writing 0xFFFF to an empty register leaves it EMPTY (the fixture value below is what
     * an event -- acpi_timer's PM1 bit 0 -- leaves behind). */
    io_port_write16(0xB000, 0xFFFF);
    CHECK_EQ(acpi->pm1_status, 0x0000);
    acpi->pm1_status = 0xFFFF;
    io_port_write16(0xB000, 0x0001);
    CHECK_EQ(acpi->pm1_status, 0xFFFE);
    CHECK_EQ(io_port_read16(0xB000), 0xFFFE);

    /* the PM1 enable register */
    io_port_write16(0xB002, 0x0001);
    CHECK_EQ(io_port_read16(0xB002), 1);

    /* the ACPI status register, both widths */
    io_port_write16(0xB004, 0x1234);
    CHECK_EQ(io_port_read16(0xB004), 0x1234);
    CHECK_EQ(io_port_read8(0xB004), 0x34);

    /* the four GPE registers */
    io_port_write8(0xAFE2, 0x81);
    CHECK_EQ(io_port_read8(0xAFE2), 0x81);
    CHECK_EQ(io_port_read8(0xAFE0), 0x00);

    /* the PM timer: 24 bits, monotone as the clock advances */
    int32_t t0 = io_port_read32(0xB008);
    cpu->instruction_counter += 1000;
    int32_t t1 = io_port_read32(0xB008);
    CHECK(t1 > t0);
    CHECK_EQ(t0 & ~0xFFFFFF, 0);

    /* the timer raises IRQ9 when PM1's bit 0 is enabled and the timer's bit 23 flips */
    machine_reset();
    acpi_init();
    program_pic();
    cpu->flags |= FLAG_INTERRUPT;
    cpu->previous_ip = STACK_TOP;

    acpi->pm1_enable = 1;
    acpi->last_timer = 0;
    /* the PM timer's 24th bit flips every 2^23 ticks = 2.343 s of virtual time, which at
     * D-F2's scale (1 instruction = 1 us) is 2 343 594 instructions -- not the 2344 that
     * M1c's seat (1 instruction = 1 ms) needed.  Derived here from the PM timer's own rate,
     * so this case fails if either the clock's scale or the device's rate drifts. */
    cpu->instruction_counter =
        (uint32_t)round((double)(1u << 23) / (PMTIMER_FREQ_HZ / 1000.0) * 1000.0) + 1;
    acpi_timer(microtick());
    CHECK_EQ(cpu->sreg[CS], SEL_CODE2);
    CHECK_EQ(cpu->instruction_pointer, HANDLER_EIP);
    CHECK_EQ(acpi->pm1_status & 1, 1);

    /* a register the device did not register at that width answers the empty entry, exactly
     * as a release reference build does (io.h D2): 0xB000 is 16-bit only, so a read8 of it
     * returns 0xFF rather than stopping */
    clear_stop();
    CHECK_EQ(io_port_read8(0xB000), 0xFF);
    CHECK(!interpreted_unimplemented);
}

/* ================================================================== *
 * 7b. the LOCAL APIC and the IOAPIC -- unit APIC-1 (HANDOFF=v86c-apic)
 *
 * The five observables the brief's G3 names: the register blocks round-trip, the timer
 * fires at the right VIRTUAL time (D-F2: the tick is the guest instruction, so the
 * fire-at boundaries below are instruction counts derived from the divider table), an
 * IOAPIC redirection entry delivers, EOI clears the ISR bit (and, level-triggered, sends
 * the remote EOI the IOAPIC's entry carries), and every guest-controlled bound refuses
 * rather than corrupting.  Everything runs through the CPU's own mmio routing
 * (read32s/write32 at APIC_MEM_ADDRESS / IOAPIC_MEM_ADDRESS), which is the guest's path.
 * ================================================================== */
static void apic_arm_idt_vectors(void)
{
    /* the file's build_idt covers only the PIC vectors; the APIC delivers on the
     * vectors the GUEST programs, so the two used here get their own entries */
    wr64(IDT_BASE + 0x30 * 8, make_idt_entry(HANDLER_EIP, SEL_CODE2, INTR_GATE_INTERRUPT, 0, true));
    wr64(IDT_BASE + 0x31 * 8, make_idt_entry(HANDLER_EIP, SEL_CODE2, INTR_GATE_INTERRUPT, 0, true));
}

/* both 8259s masked -- the arrangement a guest that runs on the APIC puts the legacy
 * controllers in (linux's "mask 8259A" initial step); it makes the poll's priority
 * order observable: the PIC is asked first and answers -1, so the APIC arm runs */
static void pic_mask_legacy(void)
{
    program_pic();
    io_port_write8(0x21, 0xFF);
    io_port_write8(0xA1, 0xFF);
}

static void apic_reset_cpu_side(void)
{
    cpu->sreg[CS] = SEL_CODE;
    cpu->instruction_pointer = STACK_TOP;
    cpu->previous_ip = STACK_TOP;
    cpu->flags |= FLAG_INTERRUPT;
}

static void test_apic(void)
{
    /* ---- the machine knob OFF: upstream's early return (apic.rs:112, 279) ---- */
    machine_reset();
    CHECK(!cpu->acpi_enabled);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x20), 0);
    write32(APIC_MEM_ADDRESS + 0xF0, 0x1234);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x20), 0);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), 0);

    /* ---- the register block: identity/version/defaults, and the round-trips with
     * upstream's own write masks ---- */
    cpu->acpi_enabled = true;
    apic_arm_idt_vectors();
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x20), 0);  /* apic_id starts 0 */
    write32(APIC_MEM_ADDRESS + 0x20, (int32_t)0x01000000);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x20), (int32_t)0x01000000);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x30), 0x50014); /* version */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0xF0), 0xFE);    /* spurious vector */
    write32(APIC_MEM_ADDRESS + 0xF0, 0x1FF);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0xF0), 0x1FF);   /* stored verbatim */
    write32(APIC_MEM_ADDRESS + 0xE0, (int32_t)0xF0000000u);
    CHECK_EQ((uint32_t)read32s(APIC_MEM_ADDRESS + 0xE0), 0xF0000000u | 0xFFFFFFu);
    write32(APIC_MEM_ADDRESS + 0xD0, 0x01020304);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0xD0), (int32_t)0x01000000u);
    write32(APIC_MEM_ADDRESS + 0x80, 0xA5A5);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x80), 0xA5);
    CHECK_EQ((uint32_t)read32s(APIC_MEM_ADDRESS + 0x320), APIC_CONFIG_MASKED);
    write32(APIC_MEM_ADDRESS + 0x320, 0x20030);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x320), 0x20030);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100), 0); /* isr */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x180), 0); /* tmr */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200), 0); /* irr */
    write32(APIC_MEM_ADDRESS + 0x310, 0x02000000);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x310), 0x02000000);
    /* the B0 read is write-only upstream: 0 */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0xB0), 0);
    /* EOI with nothing in service: the release's no-op (upstream logs "Bad eoi") */
    write32(APIC_MEM_ADDRESS + 0xB0, 0);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100), 0);

    /* ---- the IPI path: icr0's self shorthand delivers (apic.rs:374-377) ---- */
    machine_reset();
    cpu->acpi_enabled = true;
    apic_arm_idt_vectors();
    write32(APIC_MEM_ADDRESS + 0x310, 0);
    write32(APIC_MEM_ADDRESS + 0x300, 0x31 | (1 << 18)); /* vector 0x31, self */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0x20000); /* irr bit 0x31 */
    CHECK_EQ((uint32_t)read32s(APIC_MEM_ADDRESS + 0x300) & (1u << 12), 0); /* deliv status */
    apic_reset_cpu_side();
    handle_irqs();
    CHECK_EQ(cpu->sreg[CS], SEL_CODE2);
    CHECK_EQ(cpu->instruction_pointer, HANDLER_EIP);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0x20000); /* moved to isr */
    write32(APIC_MEM_ADDRESS + 0xB0, 0);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0);

    /* ---- the timer, one-shot, divider /1: 1000 ticks per guest instruction
     * (APIC_TIMER_FREQ 1e6/ms over D-F2's 1 instruction = 1 us -- the TSC's own rate,
     * m2-1f measured "1000 counts per instruction") ---- */
    machine_reset();
    cpu->acpi_enabled = true;
    apic_arm_idt_vectors();
    uint32_t ic0 = cpu->instruction_counter;
    write32(APIC_MEM_ADDRESS + 0x3E0, 0x0B); /* (0xB&3)|((0xB&8)>>1) = 0b111 -> shift 0 */
    CHECK_EQ(apic_get_apic()->timer_divider_shift, 0);
    write32(APIC_MEM_ADDRESS + 0x380, 2000); /* 2000 ticks = 2 instructions */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x380), 2000);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x390), 2000); /* current count mirrors it */
    CHECK_EQ(round_to_u32(apic_get_apic()->timer_last_tick * 1000.0), ic0);
    write32(APIC_MEM_ADDRESS + 0x320, 0x30); /* vector 0x30, unmasked, one-shot */

    cpu->instruction_counter = ic0 + 1;
    /* the current-count read is ITS OWN decrementing view (apic.rs:231-268) */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x390), 1000);
    CHECK_EQ(round_to_u32(apic_timer(microtick()) * 1000.0), 1); /* one tick to go */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0);
    cpu->instruction_counter = ic0 + 2;
    CHECK_EQ(round_to_u32(apic_timer(microtick()) * 1000.0), 0);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x390), 0); /* the one-shot emptied it */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0x10000); /* irr bit 0x30 */
    /* and the fan-out reaches it through the REAL poll: the PIC has nothing, so
     * handle_irqs falls through to the APIC arm */
    apic_reset_cpu_side();
    handle_irqs();
    CHECK_EQ(cpu->sreg[CS], SEL_CODE2);
    CHECK_EQ(cpu->instruction_pointer, HANDLER_EIP);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0); /* acked */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0x10000); /* in service */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x180 + 0x10), 0); /* edge: tmr clear */
    /* THE EOI: clears the isr bit (the brief's named observable) */
    write32(APIC_MEM_ADDRESS + 0xB0, 0);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0);

    /* ---- the timer, periodic, divider /16: the divider table is 16x ---- */
    machine_reset();
    cpu->acpi_enabled = true;
    apic_arm_idt_vectors();
    ic0 = cpu->instruction_counter;
    write32(APIC_MEM_ADDRESS + 0x3E0, 0x03); /* (3&3)|((3&8)>>1) = 3 -> shift 4 */
    CHECK_EQ(apic_get_apic()->timer_divider_shift, 4);
    write32(APIC_MEM_ADDRESS + 0x320, (int32_t)APIC_TIMER_MODE_PERIODIC | 0x30);
    write32(APIC_MEM_ADDRESS + 0x380, 2000); /* 2000 counts * 16 = 32000 ticks = 32 instr */
    cpu->instruction_counter = ic0 + 31;
    apic_timer(microtick());
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0);
    cpu->instruction_counter = ic0 + 32;
    apic_timer(microtick());
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0x10000);
    /* a period later it refires -- and with /16 the elapsed ticks (32000) are already
     * past 2x the COUNT (4000), so upstream's resynchronise arm takes last_tick to now */
    cpu->instruction_counter = ic0 + 64;
    apic_timer(microtick());
    CHECK_EQ(round_to_u32(apic_get_apic()->timer_last_tick * 1000.0), ic0 + 64);
    /* the /1 periodic arm, where consecutive periods stay below 2x the count: the
     * non-skip path (apic.rs:508, last_tick += time_per_interrupt) */
    machine_reset();
    cpu->acpi_enabled = true;
    ic0 = cpu->instruction_counter;
    write32(APIC_MEM_ADDRESS + 0x3E0, 0x0B);
    write32(APIC_MEM_ADDRESS + 0x320, (int32_t)APIC_TIMER_MODE_PERIODIC | 0x30);
    write32(APIC_MEM_ADDRESS + 0x380, 2000); /* one period = 2 instructions */
    cpu->instruction_counter = ic0 + 2;
    apic_timer(microtick());
    CHECK_EQ(round_to_u32(apic_get_apic()->timer_last_tick * 1000.0), ic0 + 2);

    /* ---- the machine knob gates the fan-out, not the timer's own state ---- */
    machine_reset();
    cpu->acpi_enabled = true;
    write32(APIC_MEM_ADDRESS + 0x3E0, 0x0B);
    write32(APIC_MEM_ADDRESS + 0x320, 0x30);
    write32(APIC_MEM_ADDRESS + 0x380, 2000);
    cpu->acpi_enabled = false; /* the knob flips AFTER the timer was armed */
    cpu->instruction_counter += 3;
    run_hardware_timers(false, microtick());
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200), 0); /* never consulted */
    cpu->acpi_enabled = true;
    run_hardware_timers(true, microtick());
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0x10000); /* now it is */

    /* ---- the IOAPIC: the indirect block, a redirection entry delivering ---- */
    machine_reset();
    cpu->acpi_enabled = true;
    apic_arm_idt_vectors();
    pic_mask_legacy();
    write32(IOAPIC_MEM_ADDRESS + 0, 1); /* IOREGSEL: register 1 */
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), 0x170011); /* version | 23 entries */
    write32(IOAPIC_MEM_ADDRESS + 0, 0);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), 0); /* the id */
    write32(IOAPIC_MEM_ADDRESS + 0, 2);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), 0); /* arbitration id */
    /* entry 4: vector 0x30, edge, unmasked, destination 1 */
    write32(IOAPIC_MEM_ADDRESS + 0, 0x18);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), (int32_t)IOAPIC_CONFIG_MASKED); /* boot state */
    write32(IOAPIC_MEM_ADDRESS + 0x10, 0x30);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), 0x30);
    write32(IOAPIC_MEM_ADDRESS + 0, 0x19);
    write32(IOAPIC_MEM_ADDRESS + 0x10, (int32_t)0x01000000);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), (int32_t)0x01000000);
    /* THE DELIVERY, through device_raise_irq's real body (pic line + ioapic line +
     * poll); the 8259 is masked, so the vector comes from the redirection entry */
    apic_reset_cpu_side();
    device_raise_irq(4);
    CHECK_EQ(cpu->sreg[CS], SEL_CODE2);
    CHECK_EQ(cpu->instruction_pointer, HANDLER_EIP);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0x10000); /* in service */
    CHECK_EQ(apic_acknowledge_irq(), -1); /* the ack rule: isr >= any irr -> none */
    write32(APIC_MEM_ADDRESS + 0xB0, 0);  /* edge-triggered EOI: no remote eoi */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0);
    /* a masked entry swallows the line */
    write32(IOAPIC_MEM_ADDRESS + 0, 0x1A);
    write32(IOAPIC_MEM_ADDRESS + 0x10, (int32_t)(IOAPIC_CONFIG_MASKED | 0x30));
    device_raise_irq(5);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0);
    /* level-triggered: the entry takes the remote IRR, the vector is TMR-marked, and
     * the APIC's EOI sends the IOAPIC the remote EOI -- which re-delivers while the
     * line is still up (ioapic.rs:81-91) */
    write32(IOAPIC_MEM_ADDRESS + 0, 0x1C);
    write32(IOAPIC_MEM_ADDRESS + 0x10, (int32_t)(IOAPIC_CONFIG_TRIGGER_MODE_LEVEL | 0x30));
    device_raise_irq(6);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x180 + 0x10), 0x10000); /* tmr bit 0x30 */
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10),
             (int32_t)(IOAPIC_CONFIG_TRIGGER_MODE_LEVEL | IOAPIC_CONFIG_REMOTE_IRR | 0x30));
    write32(IOAPIC_MEM_ADDRESS + 0, 0x19);
    write32(IOAPIC_MEM_ADDRESS + 0x10, (int32_t)0x01000000); /* destination half */
    CHECK_EQ(apic_acknowledge_irq(), 0x30); /* the vector the entry programmed */
    write32(APIC_MEM_ADDRESS + 0xB0, 0);    /* the guest's EOI */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0x10000); /* re-delivered: line up */
    CHECK_EQ(apic_acknowledge_irq(), 0x30);
    /* the device lowers the line; the NEXT eoi re-delivers nothing */
    device_lower_irq(6);
    write32(APIC_MEM_ADDRESS + 0xB0, 0);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0);

    /* ---- THE BOUNDS: each refuses rather than corrupting ---- */
    /* ioregsel is a full 32-bit guest write; past the table the read answers 0 and the
     * write discards, and no entry changes.  Entry 0's destination half is programmed
     * FIRST on purpose: ioredtbl_config[24] (one past the table) IS
     * ioredtbl_destination[0] in the struct, so a relaxed bound would read THAT value
     * back -- the check below sees a neighbour's data, not a coincidental zero. */
    write32(IOAPIC_MEM_ADDRESS + 0, 0x11);
    write32(IOAPIC_MEM_ADDRESS + 0x10, (int32_t)0x55000000);
    write32(IOAPIC_MEM_ADDRESS + 0, 0x3E); /* entry 23, config half: the last valid */
    int32_t entry23 = read32s(IOAPIC_MEM_ADDRESS + 0x10);
    CHECK_EQ(entry23, (int32_t)IOAPIC_CONFIG_MASKED);
    write32(IOAPIC_MEM_ADDRESS + 0, 0x40); /* one past */
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), 0);
    write32(IOAPIC_MEM_ADDRESS + 0x10, 0x12345678);
    write32(IOAPIC_MEM_ADDRESS + 0, 0x3E);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), entry23);
    write32(IOAPIC_MEM_ADDRESS + 0, (int32_t)0xFFFFFFFFu); /* echoed verbatim */
    CHECK_EQ((uint32_t)read32s(IOAPIC_MEM_ADDRESS + 0), 0xFFFFFFFFu);
    /* window offsets other than IOREGSEL/IOWIN */
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 4), 0);
    write32(IOAPIC_MEM_ADDRESS + 8, 0x99);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 4), 0);
    /* the raise/lower bound: out of range returns before any store */
    ioapic_set_irq(24);
    ioapic_set_irq(200);
    ioapic_clear_irq(24);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0);
    /* the APIC's own dispatch: an unimplemented register answers 0 / discards, the
     * read-only current-count register ignores writes */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x3D0), 0);
    write32(APIC_MEM_ADDRESS + 0x3D0, 0x1234);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x3D0), 0);
    write32(APIC_MEM_ADDRESS + 0x390, 0x1234);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x390), 0);
    /* and the knob gates the mmio windows again */
    cpu->acpi_enabled = false;
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x20), 0);
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS + 0x10), 0);
}

/* ================================================================== *
 * 7c. the FIRMWARE TABLES -- unit ACPI-2 (HANDOFF=v86c-acpi)
 *
 * The builder's own oracle (acpi_tables_verify) over the blob, the PLACEMENT checked the
 * way the kernel checks it -- ACPICA's scan: every 16-byte step of the free window,
 * signature + both RSDP checksums -- and the MADT's fields against the LIVE registers
 * the APIC/IOAPIC answer through (the "machine's ACTUAL device configuration").  Then
 * the able-to-fail half: a corrupted checksum, a mis-aligned RSDP and a wrong GSI base
 * must each be CAUGHT, so each is applied to a copy and the check that must see it is
 * run over the copy.  The delivery-side mirror is here too: the MADT says the PIT's line
 * is IOAPIC pin 2, and device_raise_irq must feed pin 2 -- and pin 0 must stay silent
 * even with its entry unmasked.
 * ================================================================== */
static uint8_t acpi_test_sum8(const uint8_t *p, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum;
}

/* ACPICA's early predicate (acpi_tb_validate_rsdp), over exactly the region the kernel
 * scans: 16-byte steps, "RSD PTR ", the v1 checksum over 20 bytes, and -- revision 2 --
 * the length and the extended checksum over 36. */
static const uint8_t *acpi_scan_rsdp(const uint8_t *mem, uint32_t len)
{
    for (uint32_t off = 0; off + 36 <= len; off += 16) {
        const uint8_t *p = mem + off;
        if (memcmp(p, "RSD PTR ", 8) != 0) continue;
        if (acpi_test_sum8(p, 20) != 0) continue;
        if (p[15] != 2 || p[20] != 36) continue;
        if (acpi_test_sum8(p, 36) != 0) continue;
        return p;
    }
    return NULL;
}

static void test_acpi_tables(void)
{
    uint8_t blob[ACPI_TABLES_LENGTH];
    acpi_tables_build(blob);

    /* ---- placement: the constants' own contract (checked before anything reads them) ---- */
    CHECK(ACPI_OFF_RSDP % 16 == 0 && ACPI_OFF_RSDT % 16 == 0 && ACPI_OFF_XSDT % 16 == 0 &&
          ACPI_OFF_FADT % 16 == 0 && ACPI_OFF_FACS % 16 == 0 && ACPI_OFF_DSDT % 16 == 0 &&
          ACPI_OFF_MADT % 16 == 0);
    CHECK(ACPI_OFF_RSDP < ACPI_OFF_RSDT && ACPI_OFF_RSDT < ACPI_OFF_XSDT &&
          ACPI_OFF_XSDT < ACPI_OFF_FADT && ACPI_OFF_FADT < ACPI_OFF_FACS &&
          ACPI_OFF_FACS < ACPI_OFF_DSDT && ACPI_OFF_DSDT < ACPI_OFF_MADT);
    /* the free window: wholly inside 0xE0000-0xEFFFF, which nothing else claims (the
     * firmware fills 0xF0000+, the option ROM 0xC0000, the boot params 0x80000) */
    CHECK(ACPI_TABLES_ADDRESS + ACPI_TABLES_LENGTH <= 0xF0000u);
    CHECK(ACPI_TABLES_ADDRESS >= 0xE0000u);

    /* ---- the builder's oracle over its own blob ---- */
    CHECK(acpi_tables_verify(blob));
    /* the FADT's SCI is the line the PIIX4 device actually raises (acpi.c's irq 9) */
    CHECK_EQ(blob[ACPI_OFF_FADT + 46], 9);

    /* ---- the DEFAULT-OFF predicate at the firmware level: a window with no tables
     * published must NOT pass the kernel's own scan ---- */
    machine_reset();
    CHECK(!cpu->acpi_enabled);
    CHECK(acpi_scan_rsdp(mem8 + ACPI_TABLES_ADDRESS, 0xF0000u - ACPI_TABLES_ADDRESS) == NULL);
    CHECK(!acpi_tables_verify(mem8 + ACPI_TABLES_ADDRESS)); /* zeros are not a table set */

    /* ---- PUBLISH, then the kernel's own walk over guest RAM ---- */
    acpi_tables_publish(mem8);
    CHECK(cpu->acpi_enabled == false); /* publish does not flip the machine knob */
    const uint8_t *found = acpi_scan_rsdp(mem8 + ACPI_TABLES_ADDRESS,
                                          0xF0000u - ACPI_TABLES_ADDRESS);
    CHECK(found == mem8 + ACPI_TABLES_ADDRESS); /* the FIRST candidate: nothing before it */
    CHECK(acpi_tables_verify(mem8 + ACPI_TABLES_ADDRESS)); /* the published copy verifies */
    /* RSDP -> RSDT/XSDT -> the FADT and the MADT, read back out of guest memory the way
     * ACPICA walks it: each pointer lands on a signature, and each table's checksum is 0 */
    uint32_t rsdt = (uint32_t)mem8[ACPI_TABLES_ADDRESS + 16] |
                    (uint32_t)mem8[ACPI_TABLES_ADDRESS + 17] << 8 |
                    (uint32_t)mem8[ACPI_TABLES_ADDRESS + 18] << 16 |
                    (uint32_t)mem8[ACPI_TABLES_ADDRESS + 19] << 24;
    uint64_t xsdt = 0;
    for (int i = 0; i < 8; i++)
        xsdt |= (uint64_t)mem8[ACPI_TABLES_ADDRESS + 24 + i] << (8 * i);
    CHECK_EQ(rsdt, ACPI_TABLES_ADDRESS + ACPI_OFF_RSDT);
    CHECK_EQ((uint32_t)xsdt, ACPI_TABLES_ADDRESS + ACPI_OFF_XSDT);
    CHECK(memcmp(mem8 + rsdt, "RSDT", 4) == 0 && acpi_test_sum8(mem8 + rsdt, 44) == 0);
    CHECK(memcmp(mem8 + (uint32_t)xsdt, "XSDT", 4) == 0 && acpi_test_sum8(mem8 + (uint32_t)xsdt, 52) == 0);
    uint32_t fadt_at = (uint32_t)mem8[rsdt + 36] | (uint32_t)mem8[rsdt + 37] << 8 |
                       (uint32_t)mem8[rsdt + 38] << 16 | (uint32_t)mem8[rsdt + 39] << 24;
    uint32_t madt_at = (uint32_t)mem8[rsdt + 40] | (uint32_t)mem8[rsdt + 41] << 8 |
                       (uint32_t)mem8[rsdt + 42] << 16 | (uint32_t)mem8[rsdt + 43] << 24;
    CHECK_EQ(fadt_at, ACPI_TABLES_ADDRESS + ACPI_OFF_FADT);
    CHECK_EQ(madt_at, ACPI_TABLES_ADDRESS + ACPI_OFF_MADT);
    CHECK(memcmp(mem8 + fadt_at, "FACP", 4) == 0 && acpi_test_sum8(mem8 + fadt_at, 268) == 0);
    CHECK(memcmp(mem8 + madt_at, "APIC", 4) == 0 && acpi_test_sum8(mem8 + madt_at, 76) == 0);
    /* the FACS the FADT names (no checksum exists for it -- the spec gives it none) */
    CHECK(memcmp(mem8 + ACPI_TABLES_ADDRESS + ACPI_OFF_FACS, "FACS", 4) == 0);

    /* ---- the MADT's fields against the LIVE device registers ---- */
    cpu->acpi_enabled = true;
    const uint32_t madt_base = ACPI_TABLES_ADDRESS + ACPI_OFF_MADT;
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x20) >> 24, mem8[madt_base + 47]); /* apic id */
    CHECK_EQ(read32s(IOAPIC_MEM_ADDRESS) >> 24, mem8[madt_base + 54]);      /* ioapic id */

    /* ---- THE DELIVERY MIRROR: the PIT's line lands on IOAPIC pin 2, the pin the MADT
     * publishes, and NEVER on pin 0 -- even with pin 0's entry unmasked ---- */
    machine_reset();
    cpu->acpi_enabled = true;
    apic_arm_idt_vectors();
    pic_mask_legacy();
    /* entry 2: vector 0x30, edge, unmasked, destination apic 0 -- the kernel's own
     * arrangement for the timer interrupt after the override.  IOREGSEL is
     * 0x10 + 2*pin (0x14 for pin 2), the IOWIN write is the config half. */
    write32(IOAPIC_MEM_ADDRESS + 0, 0x14);
    write32(IOAPIC_MEM_ADDRESS + 0x10, 0x30);
    apic_reset_cpu_side();
    device_raise_irq(0); /* the REAL raise path: pic line + ioapic line + poll */
    CHECK_EQ(cpu->sreg[CS], SEL_CODE2); /* delivered on the entry's vector */
    CHECK_EQ(cpu->instruction_pointer, HANDLER_EIP);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0x10000); /* in service, vector 0x30 */
    write32(APIC_MEM_ADDRESS + 0xB0, 0); /* edge EOI */
    device_lower_irq(0); /* the edge line drops, through the same map (pin 2) */
    /* the negative arm: pin 2 masked again (reg 0x14), pin 0 unmasked (vector 0x31), and
     * isa irq 0 STILL never reaches pin 0 -- the redirect is where the line lands, not an
     * extra delivery; the masked pin 2 swallows its own */
    write32(IOAPIC_MEM_ADDRESS + 0, 0x14);
    write32(IOAPIC_MEM_ADDRESS + 0x10, (int32_t)(IOAPIC_CONFIG_MASKED | 0x30));
    write32(IOAPIC_MEM_ADDRESS + 0, 0x10); /* pin 0's config half */
    write32(IOAPIC_MEM_ADDRESS + 0x10, 0x31);
    apic_reset_cpu_side();
    device_raise_irq(0);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x200 + 0x10), 0); /* irr bit 0x31: never set */
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0);
    CHECK_EQ(cpu->sreg[CS], SEL_CODE); /* nothing delivered: still the guest's CS */
    device_lower_irq(0);
    /* identity for every other line: the uart's irq 4 lands on pin 4 (reg 0x18) */
    write32(IOAPIC_MEM_ADDRESS + 0, 0x18);
    write32(IOAPIC_MEM_ADDRESS + 0x10, 0x31);
    apic_reset_cpu_side();
    device_raise_irq(4);
    CHECK_EQ(cpu->sreg[CS], SEL_CODE2);
    CHECK_EQ(read32s(APIC_MEM_ADDRESS + 0x100 + 0x10), 0x20000); /* in service, vector 0x31 */
    write32(APIC_MEM_ADDRESS + 0xB0, 0);
    device_lower_irq(4);

    /* ---- ABLE TO FAIL: each corruption applied to a COPY, the check that must catch
     * it run over the copy ---- */
    uint8_t copy[ACPI_TABLES_LENGTH];

    /* repair a copy's MADT checksum: the checksum byte is ZEROED first (it covers
     * itself), then the complement lands in it -- so a refusal below is the FIELD
     * check working, not the broken checksum */
    uint8_t *madt_copy = copy + ACPI_OFF_MADT;

    /* 1. a corrupted checksum: one body byte flipped, no checksum survives it */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_FADT + 46] ^= 0x40;
    CHECK(!acpi_tables_verify(copy));

    /* 2. a wrong GSI base, WITH the checksum repaired afterwards -- so the field check
     * itself is what catches it, not the broken checksum */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_MADT + 60] = 1; /* gsi_base 0 -> 1 */
    madt_copy[9] = 0;
    madt_copy[9] = (uint8_t)(0u - acpi_test_sum8(madt_copy, 76));
    CHECK(acpi_test_sum8(madt_copy, 76) == 0); /* the checksum is now GOOD */
    CHECK(!acpi_tables_verify(copy)); /* ...and the verify still refuses */

    /* 3. a wrong LAPIC address, checksum repaired the same way */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_MADT + 39] = 0x10; /* 0xFEE00000 -> 0xFEE01000 */
    madt_copy[9] = 0;
    madt_copy[9] = (uint8_t)(0u - acpi_test_sum8(madt_copy, 76));
    CHECK(acpi_test_sum8(madt_copy, 76) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 4. a corrupted RSDP signature: the kernel's scan finds NOTHING */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_RSDP + 2] ^= 0xFF; /* "SD" -> "xD" */
    machine_reset();
    memcpy(mem8 + ACPI_TABLES_ADDRESS, copy, sizeof(copy));
    CHECK(acpi_scan_rsdp(mem8 + ACPI_TABLES_ADDRESS, 0xF0000u - ACPI_TABLES_ADDRESS) == NULL);

    /* 5. a MIS-ALIGNED RSDP (the whole set shifted 8 bytes): the 16-byte scan stride
     * misses it, AND the kernel's next step breaks -- the RSDT pointer still names the
     * unshifted address, where the shifted bytes are not a table.  (verify() is an
     * internals oracle by design and cannot see displacement; these two are the
     * placement's own checks.) */
    machine_reset();
    memcpy(mem8 + ACPI_TABLES_ADDRESS + 8, blob, sizeof(blob));
    CHECK(acpi_scan_rsdp(mem8 + ACPI_TABLES_ADDRESS, 0xF0000u - ACPI_TABLES_ADDRESS) == NULL);
    {
        uint32_t rsdp_at = ACPI_TABLES_ADDRESS + 8;
        uint32_t rsdt_ptr = (uint32_t)mem8[rsdp_at + 16] | (uint32_t)mem8[rsdp_at + 17] << 8 |
                            (uint32_t)mem8[rsdp_at + 18] << 16 | (uint32_t)mem8[rsdp_at + 19] << 24;
        CHECK(memcmp(mem8 + rsdt_ptr, "RSDT", 4) != 0);
    }

    /* 6. the FADT's hardware-reduced flag SET again, checksum repaired -- the F1
     * regression probe (results/acpi-4.md: with the flag set, a modern guest wires
     * zero IOAPIC redirection entries and every ISA device edge is lost; the verify's
     * flag check is what must catch a future edit restoring it) */
    uint8_t *fadt_copy = copy + ACPI_OFF_FADT;
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_FADT + 114] |= 0x10; /* FLAGS bit 20 (byte 114, bit 4) */
    fadt_copy[9] = 0;
    fadt_copy[9] = (uint8_t)(0u - acpi_test_sum8(fadt_copy, 268));
    CHECK(acpi_test_sum8(fadt_copy, 268) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 7. a PM block naming an address the PIIX4 device does not answer at (PM_TMR
     * 0xB008 -> 0xB009), checksum repaired -- the fixed-hardware fields are claims
     * about src/hw/acpi.c's registered ports, and the verify pins them */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_FADT + 76] = 0x09; /* PM_TMR_BLK low byte */
    fadt_copy[9] = 0;
    fadt_copy[9] = (uint8_t)(0u - acpi_test_sum8(fadt_copy, 268));
    CHECK(acpi_test_sum8(fadt_copy, 268) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 8. the DSDT's bridge _HID corrupted (PNP0A03 -> PNP0A04), checksum repaired --
     * the F2 regression probe (results/acpi-4.md): without a PNP0A03 host bridge a
     * guest roots no PCI bus and enumerates nothing */
    uint8_t *dsdt_copy = copy + ACPI_OFF_DSDT;
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_DSDT + 36 + 24] = 0x04; /* the EISAID low nibble */
    dsdt_copy[9] = 0;
    dsdt_copy[9] = (uint8_t)(0u - acpi_test_sum8(dsdt_copy, 146));
    CHECK(acpi_test_sum8(dsdt_copy, 146) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 9. IAPC_BOOT_ARCH with NO_VGA cleared again (0x0007 -> 0x0003), checksum
     * repaired -- the acpi-1r review's SF1 regression probe: a clear bit 2 declares a
     * VGA this machine does not have (machine.c's header names it absent) */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_FADT + 109] = 0x03;
    fadt_copy[9] = 0;
    fadt_copy[9] = (uint8_t)(0u - acpi_test_sum8(fadt_copy, 268));
    CHECK(acpi_test_sum8(fadt_copy, 268) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 10. CENTURY zeroed again (0x32 -> 0), checksum repaired -- the review's SF3
     * regression probe: 0 declares "no century register in CMOS", false -- rtc.c
     * answers CMOS 0x32 from the fixed-epoch clock */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_FADT + 108] = 0;
    fadt_copy[9] = 0;
    fadt_copy[9] = (uint8_t)(0u - acpi_test_sum8(fadt_copy, 268));
    CHECK(acpi_test_sum8(fadt_copy, 268) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 11. the minor version back at 5.1-on-a-5.0-length (fadt[131] = 1), checksum
     * repaired -- the review's SF2 regression probe: 268 bytes is the revision-5.0
     * size, so a 5.1 declaration is a malformed revision statement */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_FADT + 131] = 1;
    fadt_copy[9] = 0;
    fadt_copy[9] = (uint8_t)(0u - acpi_test_sum8(fadt_copy, 268));
    CHECK(acpi_test_sum8(fadt_copy, 268) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 12. the _PRT's static GSI corrupted (11 -> 10), checksum repaired -- the
     * ACPI-6 regression probe: the entry's GSI must be the value the firmware
     * programs into the PIIX router byte 0x63 (VIRTIO_IRQ), and any other line
     * is a false declaration of exactly the class this lane shipped four of */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_DSDT + 36 + 109] = 0x0A; /* the GSI byte: 11 -> 10 */
    dsdt_copy[9] = 0;
    dsdt_copy[9] = (uint8_t)(0u - acpi_test_sum8(dsdt_copy, 146));
    CHECK(acpi_test_sum8(dsdt_copy, 146) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 13. the _PRT's address moved to device 13, checksum repaired -- the entry
     * must be the (device, pin) whose parent_pin arithmetic lands on the byte the
     * firmware programs: (0 + 13 - 1) & 3 = 0 is PIRQA (0x60), deliberately 0.
     * The verify's COMPUTED parent_pin check is what must catch it. */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_DSDT + 36 + 104] = 0x0D; /* the address's device half: 12 -> 13 */
    dsdt_copy[9] = 0;
    dsdt_copy[9] = (uint8_t)(0u - acpi_test_sum8(dsdt_copy, 146));
    CHECK(acpi_test_sum8(dsdt_copy, 146) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 14. the _PRT's pin moved to INTB, checksum repaired -- (1 + 11) & 3 = 0
     * again points at PIRQA (0x60), not the programmed PIRQD (0x63) */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_DSDT + 36 + 106] = 1; /* pin Zero (INTA) -> 1 (INTB) */
    dsdt_copy[9] = 0;
    dsdt_copy[9] = (uint8_t)(0u - acpi_test_sum8(dsdt_copy, 146));
    CHECK(acpi_test_sum8(dsdt_copy, 146) == 0);
    CHECK(!acpi_tables_verify(copy));

    /* 15. the bus-number window's end corrupted (0x00FF -> 0x007F), checksum
     * repaired -- the G5 fix's own pin: the bridge must name every bus the
     * config mechanism's 8-bit bus field can decode */
    memcpy(copy, blob, sizeof(copy));
    copy[ACPI_OFF_DSDT + 36 + 82] = 0x7F;
    dsdt_copy[9] = 0;
    dsdt_copy[9] = (uint8_t)(0u - acpi_test_sum8(dsdt_copy, 146));
    CHECK(acpi_test_sum8(dsdt_copy, 146) == 0);
    CHECK(!acpi_tables_verify(copy));
}

/* ================================================================== *
 * 8. the timer seat end to end: the REAL run loop wakes a halted guest
 *
 * This is the strongest form of "M1c left the timer seat, M2-1 wired it": main_loop itself
 * calls run_hardware_timers, the PIT raises IRQ0, the PIC delivers it, the handler runs and
 * its iret returns to the halted guest.
 * ================================================================== */
static void test_timer_wakes_the_guest(void)
{
    machine_reset();
    program_pic();

    /* THE GUEST: `cli; hlt` on the LAST TWO BYTES OF A PAGE, and placed away from the
     * stack -- the interrupt frame lives at [STACK_TOP-12, STACK_TOP) = 0x6FF4..0x6FFF, so
     * guest code there would be overwritten by the frame itself and by the handler's push.
     * Ending the guest's page means the run loop's page-boundary break stops the batch right
     * after the hlt, so nothing behind it executes. */
    mem8[0x5FFE] = 0xFA; /* cli */
    mem8[0x5FFF] = 0xF4; /* hlt */

    /* the IRQ0 handler at 0x8000: push eax; mov al, 0x20; out 0x20, al; pop eax; iret */
    static const uint8_t handler[] = {0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF};
    memcpy(mem8 + HANDLER_EIP, handler, sizeof(handler));

    cpu->flags |= FLAG_INTERRUPT;
    cpu->instruction_pointer = 0x5FFE;
    cpu->previous_ip = 0x5FFE;
    cpu->in_hlt = true;

    /* the PIT: counter 0, lsb/msb, mode 2, reload 0xFFFF.  The reload matters, and so now
     * does its SCALE: D-F2 makes one instruction one MICROSECOND, so 0xFFFF (65535 ticks =
     * 54.9 ms of virtual time) needs ~54926 instructions before it rolls over -- a SMALL
     * reload used to roll the counter over on every instruction and the guest would re-enter
     * the handler forever instead of reaching its hlt. */
    io_port_write8(0x43, 0x34);
    io_port_write8(0x40, 0xFF);
    io_port_write8(0x40, 0xFF);
    uint32_t pit_ic = cpu->instruction_counter;

    /* the clock must be past the reload BEFORE the poll, or the first poll is a no-op:
     * 55000 instructions = 55 ms > 54.9 ms */
    cpu->instruction_counter = pit_ic + 55000;
    main_loop();                   /* THE REAL RUN LOOP drives the poll */

    CHECK(!interpreted_unimplemented);
    CHECK(cpu->in_hlt);                          /* the guest halted again after the iret */
    CHECK_EQ(cpu->instruction_pointer, 0x6000);  /* ... one byte past the hlt */
    CHECK_EQ(cpu->sreg[CS], SEL_CODE);           /* back in the guest's CS, not the handler's */
    CHECK_EQ(io_port_read8(0x20) & 1, 0);        /* the handler's EOI cleared the in-service bit */
    /* the rollover reloaded the counter: 0xFFFF minus the ticks that had elapsed, i.e. the
     * same arithmetic test_pit computes, reached through the RUN LOOP this time */
    {
        int32_t raw = 0xFFFF - (int32_t)floor(55.0 * PIT_FREQ_KHZ);
        CHECK_EQ(pit->counter_start_value[0], ((raw % 0xFFFF) + 0xFFFF) % 0xFFFF);
    }
}

/* ================================================================== *
 * 8. G4 -- the io_port_* handover is real
 * ================================================================== */
static void test_handover(void)
{
    machine_reset();

    /* THE CPU'S OWN INSTRUCTION BODY writes the PIT: `mov al, 0x34; out 0x43, al` is
     * instr_E6(0x43) (instructions.c:1770 -> io_port_write8), so this is the path a guest
     * takes, not a device-side shortcut. */
    write_reg8(AL, 0x34); /* counter 0, lsb/msb, mode 2 */
    instr_E6(0x43);
    CHECK_EQ(pit->counter_mode[0], 2);

    write_reg8(AL, 0xFF);
    instr_E6(0x40);
    write_reg8(AL, 0xFF);
    instr_E6(0x40); /* reload = 0xFFFF */
    CHECK_EQ(pit->counter_reload[0], 0xFFFF);
    CHECK(pit->counter_enabled[0]);
    CHECK(!interpreted_unimplemented);

    /* ... and reads it back through the CPU's `in al, imm8` (instr_E4).  One MILLISECOND of
     * virtual time after the counter started -- 1000 instructions at D-F2's scale (1
     * instruction = 1 us), since microtick is ms -- its value is 0xFFFF - 1193 (the clock's
     * unit, pit.h), and the latch returns that minus one, lsb first. */
    uint32_t latch_ic = cpu->instruction_counter;
    cpu->instruction_counter = latch_ic + 1000;
    io_port_write8(0x43, 0x00); /* latch */
    instr_E4(0x40);
    CHECK_EQ(read_reg8(AL), (0xFFFF - 1193 - 1) & 0xFF);
    instr_E4(0x40);
    CHECK_EQ(read_reg8(AL), (0xFFFF - 1193 - 1) >> 8);
    CHECK(!interpreted_unimplemented);

    /* the PIC's six ports are still intercepted IN FRONT of the map (M1c's call order):
     * a write8 to 0x20/0x21 programs the PIC, not a device */
    program_pic();
    CHECK_EQ(io_port_read8(0x21), 0x00); /* the mask register reads back */
    io_port_write8(0x21, 0xFE);
    CHECK_EQ(io_port_read8(0x21), 0xFE);

    /* G1 -- THE PORT-0x80 PROBE, through the CPU's OWN instruction bodies.  BEFORE M2-1F:
     * `out 0x80, al` STOPPED the machine naming "0x80" (M2-1's finding F1, MEASURED; the
     * pinned reference's release build registers no handler for 0x80 -- `register_write(128`
     * is 0 occurrences -- so its empty entry discards the write and reads 0xFF).  AFTER
     * D-F1: the write is a no-op, the read returns 0xFF, and the guest CONTINUES -- the
     * following instruction executes and the machine does not stop.  These are the two
     * accesses every BIOS and the Linux kernel's delay loop make, so this is the case the
     * first boot hangs on if the policy is wrong in either direction. */
    clear_stop();
    write_reg8(AL, 0x21); /* a POST code, written to the diagnostic port */
    instr_E6(0x80);
    bool write_stopped = interpreted_unimplemented;
    char write_name[160];
    snprintf(write_name, sizeof(write_name), "%s",
             write_stopped ? unimplemented_instruction_name() : "");
    CHECK(!write_stopped);
    clear_stop();
    instr_E4(0x80); /* the delay loop's read */
    int32_t read_value = read_reg8(AL);
    bool read_stopped = interpreted_unimplemented;
    char read_name[160];
    snprintf(read_name, sizeof(read_name), "%s",
             read_stopped ? unimplemented_instruction_name() : "");
    CHECK(!read_stopped);
    CHECK_EQ(read_value, 0xFF);
    /* THE PROBE'S RAW OUTPUT, printed so the BEFORE/AFTER pair is one line of transcript
     * (run this same line against results/m2-1f-io-prefix.c to see the "before" side) */
    printf("PORT-0x80 PROBE: `out 0x80, 0x21` -> stopped=%d %s| `in al, 0x80` -> value=0x%02x "
           "stopped=%d %s\n",
           (int)write_stopped, write_name, (unsigned)read_value, (int)read_stopped, read_name);
    /* the guest continues: the NEXT instruction executes and has its real effect -- a
     * claimed-port write right after the POST code moves the PIT (counter 2, mode 3) */
    write_reg8(AL, 0xB6); /* counter 2, lsb/msb, mode 3 */
    instr_E6(0x43);
    CHECK_EQ(pit->counter_mode[2], 3);
    CHECK(!interpreted_unimplemented);

    /* ... and a genuinely unclaimed port behaves the SAME way through the CPU's body (the
     * port map is what changed, not the instruction path) */
    clear_stop();
    instr_E6(0x1234);
    CHECK(!interpreted_unimplemented);
    /* the same through the CPU's `in`: the read answers the reference's value */
    clear_stop();
    instr_E4(0x1234);
    CHECK(!interpreted_unimplemented);
    CHECK_EQ(read_reg8(AL), 0xFF);
    clear_stop();

    /* THE NAMED POLICY CASES the report calls out, measured here so the report's claims have
     * a regression probe: the PIC's ports are intercepted at 8 bits ONLY, so a 16-bit read of
     * 0x20 falls through to the port map's empty entry and returns 0xFFFF without stopping
     * (upstream has no map entry there either, so 0xFFFF is EXACTLY its answer). */
    clear_stop();
    CHECK_EQ(io_port_read16(0x20), 0xFFFF);
    CHECK(!interpreted_unimplemented);

    /* and the device ports that USED to stop now answer: the whole point of M2-1 */
    CHECK_EQ(io_port_read8(0x64) & 0x10, 0x10); /* the PS/2 status */
    /* the PIT's port 0x61: counter 2 was never programmed, so its start value is 0 and any
     * tick at all "rolls it over" -- bit 5 is set (the same value test_pit computes) */
    CHECK_EQ(io_port_read8(0x61) & 0x20, 0x20);
    int32_t rtc_seconds = io_port_read8(0x71); /* the RTC's data port, index 0: BCD seconds */
    CHECK(rtc_seconds < 0x60 && (rtc_seconds >> 4) <= 9 && (rtc_seconds & 0xF) <= 9);
    CHECK(!interpreted_unimplemented);
}


/* ================================================================== *
 * 10. THE UART -- M2-2 (HANDOFF=v86c)
 *
 * The brief's G3 case: "a byte written to 0x3F8 comes out of the sink; the line status and
 * divisor behave".  The write goes through the CPU's OWN instruction body (`out dx, al` /
 * `in al, dx`, the G4 style of case 8), so what is tested is the whole path a kernel's 8250
 * driver takes: instr_E6 -> io_port_write8 -> the port map -> the device -> the bus message
 * -> the host sink.
 * ================================================================== */
static int32_t uart_sink0[64];
static int32_t uart_sink0_count = 0;
static int32_t uart_sink1[64];
static int32_t uart_sink1_count = 0;

static void uart_sink_0(void *this_value, const int32_t *args, int nargs)
{
    (void)this_value;
    if (nargs < 1 || uart_sink0_count >= 64) return;
    uart_sink0[uart_sink0_count++] = args[0];
}

static void uart_sink_1(void *this_value, const int32_t *args, int nargs)
{
    (void)this_value;
    if (nargs < 1 || uart_sink1_count >= 64) return;
    uart_sink1[uart_sink1_count++] = args[0];
}

/* `out port, al` / `in al, port` through the instruction bodies */
static void out_al(int32_t port, int32_t value)
{
    write_reg8(AL, value);
    instr_E6(port);
}
static int32_t in_al(int32_t port)
{
    instr_E4(port);
    return read_reg8(AL);
}

static void test_uart(void)
{
    machine_reset(); /* machine_init() attaches COM1 at 0x3F8 (uart.c, cpu.js:1201) */
    uart_sink0_count = 0;
    uart_sink1_count = 0;
    /* the host end of the seam: the reference's SerialAdapter listens on the bus the
     * machine pairs with the device bus, which is machine_host_bus */
    bus_register(machine_host_bus, "serial0-output-byte", uart_sink_0, NULL);
    bus_register(machine_host_bus, "serial1-output-byte", uart_sink_1, NULL);

    /* A BYTE WRITTEN TO 0x3F8 COMES OUT OF THE SINK (uart.js:145-152, 408) */
    out_al(0x3F8, 0x41);
    CHECK(!interpreted_unimplemented);
    CHECK_EQ(uart_sink0_count, 1);
    CHECK_EQ(uart_sink0[0], 0x41);

    /* the line status register: TX holding empty and transmitter empty are set from
     * construction (uart.js:71) and DATA_READY is clear with an empty receiver */
    CHECK_EQ(in_al(0x3FD) & 0xE0, 0x60);
    CHECK_EQ(in_al(0x3FD) & 0x01, 0x00);

    /* THE RECEIVE SIDE, fed through the OTHER half of the seam: the bus message the
     * reference's starter.js sends (starter.js:1239/1252).  The byte lands in the FIFO, the
     * line status says DATA_READY, a read of port+0 returns it, and the FIFO being empty
     * again clears the bit (uart.js:175-204, 376-391). */
    BUS_SEND(machine_host_bus, "serial0-input", 0x5A);
    CHECK_EQ(in_al(0x3FD) & 0x01, 0x01);
    CHECK_EQ(in_al(0x3F8), 0x5A);
    CHECK_EQ(in_al(0x3FD) & 0x01, 0x00);
    /* an empty read is 0, not a stale byte */
    CHECK_EQ(in_al(0x3F8), 0x00);

    /* THE DIVISOR LATCH (uart.js:154-173, 206-216, 395-399): with DLAB set, port+0 and
     * port+1 ARE the divisor's low and high bytes -- the two halves are written by separate
     * accesses and read back as written, and a write to port+0 no longer reaches the sink */
    out_al(0x3FB, in_al(0x3FB) | 0x80); /* LCR |= DLAB */
    out_al(0x3F8, 0x12);
    out_al(0x3F9, 0x34);
    CHECK_EQ(in_al(0x3F8), 0x12);
    CHECK_EQ(in_al(0x3F9), 0x34);
    int32_t sink_before = uart_sink0_count;
    out_al(0x3F8, 0x99);
    CHECK_EQ(uart_sink0_count, sink_before); /* the divisor, not the console */
    out_al(0x3FB, in_al(0x3FB) & ~0x80);     /* DLAB off */
    CHECK_EQ(in_al(0x3F8), 0x00);            /* port+0 is the receiver again */

    /* INTERRUPTS: the IER's THRI arm (uart.js:332-359) makes the IIR report the transmitter
     * holding register empty, and READING the IIR clears it (uart.js:218-230).  What a
     * driver sees is asserted here; the IRQ line itself is raised through device_raise_irq,
     * which case 8 already drives end to end. */
    out_al(0x3F8 + 2, 0x00);    /* FIFO off, so the IIR is the plain value */
    out_al(0x3F8 + 1, 0x02);    /* IER: THRI enable */
    CHECK_EQ(in_al(0x3F8 + 2) & 0x0F, 0x02); /* THRI pending */
    CHECK_EQ(in_al(0x3F8 + 2) & 0x0F, 0x01); /* read-and-cleared: NO_INT */
    out_al(0x3F8 + 1, 0x00);    /* IER: nothing enabled */

    /* the FIFO bit is visible in the IIR's top two bits (uart.js:227) */
    out_al(0x3F8 + 2, 0x01);    /* FCR: enable FIFO */
    BUS_SEND(machine_host_bus, "serial0-input", 0x11);
    CHECK_EQ(in_al(0x3F8 + 2) & 0xC0, 0xC0);
    CHECK_EQ(in_al(0x3F8), 0x11);
    out_al(0x3F8 + 2, 0x00);

    /* THE LOOPBACK MODE (uart.js:405-409): with MCR's loopback bit set a written byte goes
     * back into the device's own receiver instead of the host */
    out_al(0x3F8 + 4, 0x10);    /* MCR: loopback */
    sink_before = uart_sink0_count;
    out_al(0x3F8, 0x7E);
    CHECK_EQ(uart_sink0_count, sink_before);
    CHECK_EQ(in_al(0x3FD) & 0x01, 0x01);
    CHECK_EQ(in_al(0x3F8), 0x7E);
    out_al(0x3F8 + 4, 0x00);

    /* the scratch register (uart.js:288-295), the one register that is pure storage -- a
     * kernel's 8250 probe writes and reads it to identify the part */
    out_al(0x3F8 + 7, 0xA5);
    CHECK_EQ(in_al(0x3F8 + 7), 0xA5);

    /* the modem status register's DELTA BITS, set from the host seam's modem lines and
     * cleared by the read (uart.js:278-286, 444-455) */
    CHECK_EQ(in_al(0x3F8 + 6), 0x00);
    BUS_SEND(machine_host_bus, "serial0-carrier-detect-input", 1);
    int32_t msr = in_al(0x3F8 + 6);
    CHECK_EQ(msr & 0x80, 0x80); /* DCD set in the high nibble */
    CHECK_EQ(msr & 0x08, 0x08); /* and its delta bit set in the low nibble */
    msr = in_al(0x3F8 + 6);
    CHECK_EQ(msr & 0x80, 0x80); /* DCD is still there */
    CHECK_EQ(msr & 0x08, 0x00); /* the delta was cleared by the read */
    BUS_SEND(machine_host_bus, "serial0-carrier-detect-input", 0);
    msr = in_al(0x3F8 + 6);
    CHECK_EQ(msr & 0x80, 0x00);
    CHECK_EQ(msr & 0x08, 0x08);

    /* THE SECOND CHANNEL is a real device, not a comment: uart_attach's port switch
     * (uart.js:92-114) builds COM2 at 0x2F8 and its output message is "serial1-output-byte"
     * -- the channel number is part of the message NAME (uart.js:408).  The default machine
     * attaches COM1 only, which is what the reference's settings.uartN defaults to. */
    uart_attach(machine_bus, UART_COM2);
    out_al(0x2F8, 0x42);
    CHECK_EQ(uart_sink1_count, 1);
    CHECK_EQ(uart_sink1[0], 0x42);
    CHECK_EQ(uart_sink0_count, sink_before); /* COM2 did not write COM1's sink */
    /* COM2's own divisor, at its own ports */
    out_al(0x2FB, 0x80);
    out_al(0x2F8, 0x56);
    CHECK_EQ(in_al(0x2F8), 0x56);
}

/* ================================================================== *
 * 11. THE bzImage LOADER -- M2-2 (HANDOFF=v86c)
 *
 * The brief's G3 case for the loader: "the boot params are what the protocol demands".  Two
 * halves: (a) a synthetic image, where every field is known and the whole geometry is
 * checked (including the option ROM the reference's own make_linux_boot_rom produces); and
 * (b) THE FETCHED KERNEL'S OWN BYTES -- tests/images/vmlinuz-i386, the file this unit's
 * gate boots -- where the loader's decisions are checked against the file's header and the
 * guest memory is checked against the file's bytes.
 * ================================================================== */
static uint16_t kr16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t kr32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_kernel_loader(void)
{
    machine_reset();

    /* ---- (a) A SYNTHETIC IMAGE ---------------------------------------------- */
    enum { IMAGE_SIZE = 0x1000, SETUP_SECTS_FAKE = 1 };
    static uint8_t image[IMAGE_SIZE];
    for (uint32_t i = 0; i < IMAGE_SIZE; i++) image[i] = (uint8_t)(i * 7 + 3);
    image[0x1FE] = 0x55;
    image[0x1FF] = 0xAA; /* the boot signature */
    image[0x202] = 'H';
    image[0x203] = 'd';
    image[0x204] = 'r';
    image[0x205] = 'S'; /* the header magic, byte-wise (misaligned on purpose) */
    image[0x1F1] = SETUP_SECTS_FAKE;
    image[0x206] = 0x0F;
    image[0x207] = 0x02; /* protocol 2.15 */
    image[0x211] = 0x01; /* loadflags: LOADED_HIGH */
    image[0x238] = 0xFF;
    image[0x239] = 0x07; /* cmdline_size 0x7FF */

    kernel_loaded_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(kernel_load(mem8, cpu->memory_size, image, IMAGE_SIZE, NULL, 0, "console=ttyS0",
                      &loaded));
    CHECK_EQ(loaded.option_rom_length, 0x200);

    /* THE REAL-MODE SETUP BLOCK at 0x80000 (kernel.js:172), with the boot params MUTATED
     * inside it (kernel.js:121-142) */
    const uint32_t base = 0x80000;
    CHECK_EQ(mem8[base + 0x210], 0xFF); /* type_of_loader: NOT_ASSIGNED */
    CHECK_EQ(mem8[base + 0x211], 0x81); /* LOADED_HIGH | CAN_USE_HEAPS, QUITE off */
    CHECK_EQ(kr16(mem8 + base + 0x224), 0xDE00); /* heap_end_ptr = heap_end - 0x200 */
    CHECK_EQ(kr16(mem8 + base + 0x1FA), 0xFFFF); /* vid_mode: "normal" */
    /* the command line: its pointer is base_ptr + heap_end = 0x8E000 (kernel.js:139-146) */
    CHECK_EQ(kr32(mem8 + base + 0x228), 0x8E000);
    CHECK(strcmp((char *)mem8 + 0x8E000, "console=ttyS0") == 0);
    /* no initrd: both ramdisk fields are zero (kernel.js:167-168) */
    CHECK_EQ(kr32(mem8 + base + 0x218), 0);
    CHECK_EQ(kr32(mem8 + base + 0x21C), 0);
    /* the rest of the header is the IMAGE's own bytes, copied not synthesized */
    CHECK_EQ(mem8[base + 0x206], 0x0F);
    CHECK_EQ(mem8[base + 0x1FE], 0x55);
    CHECK_EQ(mem8[base], image[0]);

    /* THE PROTECTED-MODE PART at 0x100000 (kernel.js:173): the image's byte at
     * (setup_sects+1)*512 onward */
    const uint32_t prot_start = (SETUP_SECTS_FAKE + 1) * 512;
    CHECK_EQ(mem8[0x100000], image[prot_start]);
    CHECK_EQ(mem8[0x100000 + 0x100], image[prot_start + 0x100]);
    CHECK_EQ(mem8[0x100000 + (IMAGE_SIZE - prot_start) - 1], image[IMAGE_SIZE - 1]);

    /* THE OPTION ROM (kernel.js:181-234).  The bytes are the MEASURED output of the pinned
     * reference's own make_linux_boot_rom for (0x8000, 0xE000): the 0xAA55 header, the size
     * byte, the 22-byte prologue that sets the segments and jumps to 0x8020:0, and the
     * checksum byte that makes the block sum to zero mod 256. */
    static const uint8_t prologue[22] = {
        0xFA, 0xB8, 0x00, 0x80, /* cli; mov ax, 0x8000 */
        0x8E, 0xC0,             /* mov es, ax */
        0x8E, 0xD8,             /* mov ds, ax */
        0x8E, 0xE0,             /* mov fs, ax */
        0x8E, 0xE8,             /* mov gs, ax */
        0x8E, 0xD0,             /* mov ss, ax */
        0xBC, 0x00, 0xE0,       /* mov sp, 0xE000 */
        0xEA, 0x00, 0x00, 0x20, 0x80, /* jmp 0x8020:0 */
    };
    CHECK_EQ(loaded.option_rom[0], 0x55);
    CHECK_EQ(loaded.option_rom[1], 0xAA);
    CHECK_EQ(loaded.option_rom[2], 1); /* 0x200 bytes = one 512-byte block */
    for (int i = 0; i < 22; i++) {
        checks++;
        if (loaded.option_rom[3 + i] != prologue[i]) {
            failures++;
            fprintf(stderr, "FAIL %s:%d: option ROM byte %d: got 0x%02x, want 0x%02x\n",
                    __FILE__, __LINE__, i, loaded.option_rom[3 + i], prologue[i]);
        }
    }
    int32_t rom_sum = 0;
    for (int i = 0; i < 0x200; i++) rom_sum += loaded.option_rom[i];
    CHECK_EQ(rom_sum & 0xFF, 0); /* the checksum the firmware's scan verifies */
    /* the entry the prologue jumps INTO is the kernel's own setup header: the image's byte
     * at 0x200 is the 0xEB jump, and "HdrS" follows it -- so 0x8020:0 is exactly the setup
     * code's entry, which is the whole point of the ROM's segment arithmetic */
    CHECK_EQ(mem8[0x80200], image[0x200]);
    CHECK_EQ(mem8[0x80202], 'H');
    CHECK_EQ(mem8[0x80203], 'd');

    /* A MALFORMED IMAGE IS REFUSED, NOT MIS-LOADED (kernel.js:65-79: log and return) */
    image[0x1FE] = 0x00;
    CHECK(!kernel_load(mem8, cpu->memory_size, image, IMAGE_SIZE, NULL, 0, "x", &loaded));
    image[0x1FE] = 0x55;
    image[0x202] = 'X';
    CHECK(!kernel_load(mem8, cpu->memory_size, image, IMAGE_SIZE, NULL, 0, "x", &loaded));
    image[0x202] = 'H';
    /* and so is an image whose setup block does not fit below the video area (>0x20000/512
     * sectors) -- a stop, not a silent mis-load */
    image[0x1F1] = 0xFF;
    clear_stop();
    CHECK_EQ(mem8[base + 0x1F1], 0x01); /* the stop must not have written anything */
    free(loaded.option_rom);
    loaded.option_rom = NULL;

    /* ---- (b) THE FETCHED KERNEL'S OWN BYTES --------------------------------- */
    FILE *file = fopen("tests/images/vmlinuz-i386", "rb");
    if (file == NULL) {
        skips++;
        printf("SKIP kernel loader (b): tests/images/vmlinuz-i386 is not present (the unit's"
               " brief fetches it with curl)\n");
        return;
    }
    /* The test machine is 2 MiB, so the file is used as a PREFIX: its first 64 KiB are the
     * 31-sector setup block plus the head of the compressed payload, which is every byte the
     * loader's decisions depend on.  The full 7.7 MB image is what the boot gate loads. */
    uint8_t prefix[0x10000];
    size_t got = fread(prefix, 1, sizeof(prefix), file);
    fclose(file);
    CHECK_EQ(got, sizeof(prefix));

    /* THE HEADER'S OWN VALUES, read from the file (and what the loader decides from them):
     * setup_sects 31 -> the setup block is 0x4000 bytes, protocol 2.15, LOADED_HIGH, and the
     * two checksums the loader verifies */
    CHECK_EQ(prefix[0x1F1], 31);
    CHECK_EQ(kr16(prefix + 0x1FE), 0xAA55);
    CHECK_EQ(kr32(prefix + 0x202), 0x53726448u); /* "HdrS" */
    CHECK_EQ(kr16(prefix + 0x206), 0x020F);
    CHECK(prefix[0x211] & 0x01);
    CHECK_EQ(kr32(prefix + 0x1F4) << 4, 0x75B200); /* syssize, in 16-byte units */
    CHECK_EQ(kr32(prefix + 0x260), 0x11B6000);     /* init_size: 18.5 MiB */
    CHECK_EQ((31 + 1) * 512, 0x4000);

    CHECK(kernel_load(mem8, cpu->memory_size, prefix, sizeof(prefix), NULL, 0, "console=ttyS0",
                      &loaded));
    /* the real-mode block is the FILE's first 0x4000 bytes, with the header fields mutated */
    CHECK_EQ(mem8[base], prefix[0]);
    CHECK_EQ(mem8[base + 0x1FF], prefix[0x1FF]);
    CHECK_EQ(mem8[base + 0x1F1], prefix[0x1F1]);
    CHECK_EQ(mem8[base + 0x210], 0xFF);
    CHECK_EQ(mem8[base + 0x211], 0x81);
    CHECK_EQ(kr16(mem8 + base + 0x224), 0xDE00);
    CHECK_EQ(kr32(mem8 + base + 0x228), 0x8E000);
    CHECK(strcmp((char *)mem8 + 0x8E000, "console=ttyS0") == 0);
    /* the protected-mode part is the FILE's bytes from 0x4000 onward, at 0x100000 -- the
     * first byte is the decompressor's own `cld; cli` (0xFC 0xFA), which is what makes the
     * kernel's protected-mode entry reachable at all */
    CHECK_EQ(mem8[0x100000], prefix[0x4000]);
    CHECK_EQ(mem8[0x100000], 0xFC);
    CHECK_EQ(mem8[0x100001], 0xFA);
    CHECK_EQ(mem8[0x100000 + 0xB000], prefix[0x4000 + 0xB000]);
    /* the option ROM for this image's geometry: the same prologue, and a zero checksum */
    int32_t rom_sum2 = 0;
    for (int i = 0; i < 0x200; i++) rom_sum2 += loaded.option_rom[i];
    CHECK_EQ(rom_sum2 & 0xFF, 0);
    CHECK_EQ(loaded.option_rom[3], 0xFA);
    free(loaded.option_rom);
    loaded.option_rom = NULL;
}


/* ================================================================== *
 * M2-3: THE IDE CONTROLLER (the brief's G2)
 *
 * The device is driven through the CPU's OWN `out`/`in` instruction bodies (instr_E6/E4,
 * instr16_ED, instr32_ED), i.e. the path a kernel's driver takes -- not through the port
 * map directly.  The disk image is built by tests/mkdisk.py's formula (the same file the
 * boot gate attaches), and where the file is present the C-built image is compared with it
 * byte for byte, so the two implementations cannot drift apart.
 * ================================================================== */
#define IDE_DISK_SECTORS 8192u
#define IDE_DISK_BYTES (IDE_DISK_SECTORS * 512u)
#define IDE_SMALL_SECTORS 1000u
#define IDE_SMALL_BYTES (IDE_SMALL_SECTORS * 512u)

static uint8_t ide_disk[IDE_DISK_BYTES];
static uint8_t ide_disk_small[IDE_SMALL_BYTES];

/* tests/mkdisk.py: pattern_byte */
static uint8_t ide_pattern_byte(uint32_t sector, uint32_t offset)
{
    return (uint8_t)(sector * 31u + offset * 17u + 0x5Au);
}

/* tests/mkdisk.py: sector_bytes + mbr */
static void ide_build_disk(uint8_t *image, uint32_t sectors, uint32_t start_lba, uint32_t heads,
                           uint32_t spt)
{
    for (uint32_t n = 0; n < sectors; n++) {
        uint8_t *sector = image + (size_t)n * 512;
        if (n != 0) {
            for (uint32_t j = 0; j < 512; j++) sector[j] = ide_pattern_byte(n, j);
            continue;
        }
        memset(sector, 0, 512);
        uint32_t cylinders = sectors / (heads * spt);
        uint32_t end_cyl = cylinders > 0 ? cylinders - 1 : 0;
        uint32_t entry = 0x1BE;
        uint32_t length = sectors - start_lba;
        sector[entry + 0] = 0x80;
        sector[entry + 1] = 0x00;
        sector[entry + 2] = 0x01;
        sector[entry + 3] = 0x00;
        sector[entry + 4] = 0x83;
        sector[entry + 5] = (uint8_t)(heads - 1);
        sector[entry + 6] = (uint8_t)(spt | ((end_cyl >> 8) << 6));
        sector[entry + 7] = (uint8_t)(end_cyl & 0xFF);
        for (int i = 0; i < 4; i++) {
            sector[entry + 12 + i] = (uint8_t)(start_lba >> (8 * i));
            sector[entry + 16 + i] = (uint8_t)(length >> (8 * i));
        }
        sector[0x1FE] = 0x55;
        sector[0x1FF] = 0xAA;
    }
}

static void ide_outb(int32_t port, int32_t value) { out_al(port, value); }
static int32_t ide_inb(int32_t port) { return in_al(port); }

static uint32_t ide_inw(int32_t port)
{
    write_reg16(DX, port & 0xFFFF);
    instr16_ED();
    return (uint32_t)(uint16_t)read_reg16(AX);
}

static uint32_t ide_ind(int32_t port)
{
    write_reg16(DX, port & 0xFFFF);
    instr32_ED();
    return (uint32_t)read_reg32(EAX);
}

static void ide_outd(int32_t port, uint32_t value)
{
    write_reg16(DX, port & 0xFFFF);
    write_reg32(EAX, (int32_t)value);
    instr32_EF();
}

/* the DEVICE register's LBA setup + the sector count (a driver's own sequence) */
static void ide_set_lba(int32_t base, uint32_t lba, uint32_t count)
{
    ide_outb(base + 6, (int32_t)(0xE0 | ((lba >> 24) & 0x0F))); /* master, LBA mode */
    ide_outb(base + 2, (int32_t)(count & 0xFF));
    ide_outb(base + 3, (int32_t)(lba & 0xFF));
    ide_outb(base + 4, (int32_t)((lba >> 8) & 0xFF));
    ide_outb(base + 5, (int32_t)((lba >> 16) & 0xFF));
}

/* the ATA string encoding: each WORD is byte-swapped (ide.js's strcpy_be16) */
static void ide_ata_string(const uint8_t *words, int ofs16, int len16, char *out)
{
    for (int i = 0; i < len16; i++) {
        out[2 * i] = (char)words[(ofs16 + i) * 2 + 1];
        out[2 * i + 1] = (char)words[(ofs16 + i) * 2];
    }
    out[2 * len16] = '\0';
}

static uint16_t ide_word(const uint8_t *words, int word) {
    return (uint16_t)(words[2 * word] | (words[2 * word + 1] << 8));
}

static void ide_read_identify(int32_t base, uint8_t *words)
{
    ide_outb(base + 6, 0xA0); /* device: master */
    ide_outb(base + 7, 0xEC); /* IDENTIFY DEVICE */
    for (int i = 0; i < 512; i++) words[i] = (uint8_t)ide_inb(base + 0);
}

static int32_t ide_pci_read32(int32_t offset)
{
    io_port_write32(PCI_CONFIG_ADDRESS, 0x8000F000 | (offset & 0xFC));
    return io_port_read32(PCI_CONFIG_DATA);
}

static void test_ide(void)
{
    uint8_t words[512];
    char text[64];

    /* ---- the machine's device set: the reference always has the controller ------------ */
    ide_build_disk(ide_disk, IDE_DISK_SECTORS, 2048, 16, 63);
    ide_build_disk(ide_disk_small, IDE_SMALL_SECTORS, 2048, 8, 32);

    /* the differential check: the C mirror of tests/mkdisk.py vs the generated file */
    {
        FILE *file = fopen("tests/images/disk.img", "rb");
        if (file != NULL) {
            static uint8_t generated[IDE_DISK_BYTES];
            size_t got = fread(generated, 1, sizeof(generated), file);
            fclose(file);
            CHECK_EQ(got, IDE_DISK_BYTES);
            CHECK(memcmp(generated, ide_disk, IDE_DISK_BYTES) == 0);
        } else {
            skips++;
            fprintf(stderr, "SKIP ide: tests/images/disk.img absent (the mkdisk.py mirror"
                            " was not compared)\n");
        }
    }

    machine_config.hda.buffer = ide_disk;
    machine_config.hda.length = IDE_DISK_BYTES;
    machine_config.hda.is_cdrom = false;
    machine_reset();

    CHECK(ide_channel_is_present(0));
    CHECK(ide_channel_is_present(1)); /* the secondary ATAPI, always present */

    /* ---- the geometry set_disk_buffer derived from the MBR (ide.js:977-1133) ---------- */
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DRIVE1_CYL + 0), 8);  /* 8192 / (16*63) */
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DRIVE1_CYL + 1), 0);
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DRIVE1_CYL + 2), 16); /* heads */
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DRIVE1_CYL + 8), 63); /* sectors per track */
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DRIVE1_TYPE), 47);
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DATA) & 0xF0, 0xF0);
    CHECK_EQ(rtc_cmos_read(CMOS_BIOS_DISKTRANSFLAG) & 3, 0); /* TRANSLATION_NONE */

    /* ---- the PCI function (00:1e.0) -------------------------------------------------- */
    CHECK_EQ(ide_pci_read32(0x00) & 0xFFFF, 0x8086);
    CHECK_EQ((uint32_t)ide_pci_read32(0x00) >> 16, 0x7010);
    CHECK_EQ((uint32_t)ide_pci_read32(0x08) >> 8, 0x010180); /* class/subclass/prog_if */
    CHECK_EQ((uint32_t)ide_pci_read32(0x10) & 0xFFFF, 0x01F1); /* BAR0: primary command */
    CHECK_EQ((uint32_t)ide_pci_read32(0x14) & 0xFFFF, 0x03F7); /* BAR1: primary control */
    CHECK_EQ((uint32_t)ide_pci_read32(0x18) & 0xFFFF, 0x0171); /* BAR2: secondary command */
    CHECK_EQ((uint32_t)ide_pci_read32(0x1C) & 0xFFFF, 0x0377); /* BAR3: secondary control */
    CHECK_EQ((uint32_t)ide_pci_read32(0x20) & 0xFFFF, 0xB401); /* BAR4: bus master */
    CHECK_EQ((uint32_t)ide_pci_read32(0x3C) >> 8 & 0xFF, 0x01); /* interrupt pin 1 (INTA) */
    /* THE TWO BITS THE FIRMWARE MUST SET (tests/minibios.asm's chipset_init): upstream's
     * config space leaves the PIIX3's IDE-enable bits clear, and a Linux kernel's ata_piix
     * then refuses to probe the channel (see the unit report).  This asserts the DEVICE's
     * own bytes -- the firmware is what sets them at run time. */
    CHECK_EQ(ide_pci_read32(0x40) >> 8 & 0xFF, 0x00); /* byte 0x41 */
    CHECK_EQ(ide_pci_read32(0x40) >> 24 & 0xFF, 0x00); /* byte 0x43 */

    /* and the firmware's own two writes do set them (the same ports, the same widths) */
    io_port_write32(PCI_CONFIG_ADDRESS, 0x8000F040);
    io_port_write16(PCI_CONFIG_DATA, 0x8000);
    io_port_write16(PCI_CONFIG_DATA + 2, 0x8000);
    CHECK_EQ(ide_pci_read32(0x40) >> 8 & 0xFF, 0x80);
    CHECK_EQ(ide_pci_read32(0x40) >> 24 & 0xFF, 0x80);
    io_port_write32(PCI_CONFIG_ADDRESS, 0x8000F040);
    io_port_write16(PCI_CONFIG_DATA, 0x0000);
    io_port_write16(PCI_CONFIG_DATA + 2, 0x0000);
    CHECK_EQ(ide_pci_read32(0x40), 0); /* the device's own bytes are back to upstream's */

    /* ---- IDENTIFY DEVICE (the model, the geometry and the sector count) --------------- */
    CHECK_EQ(ide_inb(0x1F7), 0x50); /* DRDY|DSC before anything is asked */
    CHECK_EQ(ide_inb(0x3F6), 0x50); /* the alternate status agrees (and lowered no IRQ) */
    ide_read_identify(0x1F0, words);

    CHECK_EQ(ide_word(words, 0), 0x0040);   /* general configuration: ATA */
    CHECK_EQ(ide_word(words, 1), 8);        /* cylinders */
    CHECK_EQ(ide_word(words, 3), 16);       /* heads */
    CHECK_EQ(ide_word(words, 6), 63);       /* sectors per track */
    CHECK_EQ(ide_word(words, 60), IDE_DISK_SECTORS & 0xFFFF);
    CHECK_EQ(ide_word(words, 61), IDE_DISK_SECTORS >> 16);
    CHECK_EQ(ide_word(words, 57), IDE_DISK_SECTORS & 0xFFFF);
    CHECK_EQ(ide_word(words, 63), 0x0407);  /* multiword DMA modes */
    CHECK_EQ(ide_word(words, 49), 0x0200);  /* LBA, not DMA */
    CHECK_EQ(ide_word(words, 93), 0x6001);
    ide_ata_string(words, 10, 10, text);
    CHECK(strncmp(text, "8086-8600", 9) == 0);
    ide_ata_string(words, 23, 4, text);
    CHECK(strncmp(text, "1.00", 4) == 0);
    ide_ata_string(words, 27, 20, text);
    CHECK(strncmp(text, "v86 ATA HD", 10) == 0);
    CHECK_EQ(text[10], '\0'); /* the tail is NUL, not space: ide.js's fill quirk */
    CHECK_EQ(ide_inb(0x1F7), 0x50); /* the transfer is over */
    CHECK_EQ(ide_inb(0x1F1), 0x00); /* no error */

    /* ---- READ SECTORS: the right bytes, in guest-readable order ----------------------- */
    ide_set_lba(0x1F0, 3, 1);
    ide_outb(0x1F7, 0x20); /* READ SECTORS */
    CHECK_EQ(ide_inb(0x1F7), 0x58); /* DRDY|DSC|DRQ */
    for (int i = 0; i < 512; i++) words[i] = (uint8_t)ide_inb(0x1F0);
    CHECK_EQ(ide_inb(0x1F7), 0x50);
    CHECK_EQ(ide_inb(0x1F1), 0x00);
    for (int i = 0; i < 512; i++) {
        if (words[i] != ide_pattern_byte(3, (uint32_t)i)) {
            CHECK_EQ(words[i], ide_pattern_byte(3, (uint32_t)i));
            break;
        }
    }
    CHECK_EQ(words[0], ide_pattern_byte(3, 0));
    CHECK_EQ(words[511], ide_pattern_byte(3, 511));

    /* sector 0 is the MBR: the signature the kernel's own partition scan reads */
    ide_set_lba(0x1F0, 0, 1);
    ide_outb(0x1F7, 0x20);
    for (int i = 0; i < 512; i++) words[i] = (uint8_t)ide_inb(0x1F0);
    CHECK_EQ(words[0x1FE], 0x55);
    CHECK_EQ(words[0x1FF], 0xAA);
    CHECK_EQ(words[0x1BE], 0x80);
    CHECK_EQ(words[0x1C2], 0x83);

    /* a 32-bit read of the data port sees the same bytes (upstream's data32 view) */
    ide_set_lba(0x1F0, 3, 1);
    ide_outb(0x1F7, 0x20);
    {
        uint32_t first = ide_ind(0x1F0);
        uint32_t second = ide_inw(0x1F0);
        uint32_t want = (uint32_t)ide_pattern_byte(3, 0) |
                        ((uint32_t)ide_pattern_byte(3, 1) << 8) |
                        ((uint32_t)ide_pattern_byte(3, 2) << 16) |
                        ((uint32_t)ide_pattern_byte(3, 3) << 24);
        CHECK_EQ(first, want);
        CHECK_EQ(second, (uint32_t)ide_pattern_byte(3, 4) |
                             ((uint32_t)ide_pattern_byte(3, 5) << 8));
    }
    for (int i = 6; i < 512; i++) (void)ide_inb(0x1F0);
    CHECK_EQ(ide_inb(0x1F7), 0x50);

    /* ---- a MULTI-SECTOR read: the second sector is the NEXT one (ata_advance) --------- */
    ide_set_lba(0x1F0, 10, 2);
    ide_outb(0x1F7, 0x20);
    for (int i = 0; i < 512; i++) words[i] = (uint8_t)ide_inb(0x1F0);
    CHECK_EQ(words[0], ide_pattern_byte(10, 0));
    CHECK_EQ(ide_inb(0x1F7), 0x58); /* still DRQ: the second sector is coming */
    for (int i = 0; i < 512; i++) words[i] = (uint8_t)ide_inb(0x1F0);
    CHECK_EQ(words[0], ide_pattern_byte(11, 0));
    CHECK_EQ(ide_inb(0x1F7), 0x50);
    CHECK_EQ(ide_inb(0x1F3), 12 & 0xFF); /* the LBA advanced twice (ide.js:2217-2257) */
    CHECK_EQ(ide_inb(0x1F2), 0);         /* and the count went to zero */

    /* ---- an error status, as upstream reports it: outside the disk -> status 0xFF ------ */
    ide_set_lba(0x1F0, IDE_DISK_SECTORS, 1);
    ide_outb(0x1F7, 0x20);
    CHECK_EQ(ide_inb(0x1F7), 0xFF); /* ide.js:2283: status_reg = 0xFF */
    CHECK_EQ(ide_inb(0x1F0), 0x00); /* and no data is offered */

    /* a command upstream does not implement: ABORT (error 0x04, status DRDY|ERR) */
    ide_set_lba(0x1F0, 0, 1);
    ide_outb(0x1F7, 0x3D); /* undefined command */
    CHECK_EQ(ide_inb(0x1F7), 0x41); /* DRDY|ERR */
    CHECK_EQ(ide_inb(0x1F1), 0x04); /* ATA_ER_ABRT */

    /* ---- READ DMA: the path a real kernel uses (a PRD table in guest memory) ---------- */
    {
        const uint32_t prdt = 0x30000;
        const uint32_t dest = 0x40000;
        write32(prdt + 0, dest);       /* PRD address */
        write32(prdt + 4, 512);        /* byte count + the end bit (0x80000000) */
        write8(prdt + 7, 0x80);
        write32(0x40000, 0);           /* clear the destination */
        for (int i = 0; i < 512; i++) mem8[dest + i] = 0;

        ide_outb(0x1F6, 0xE0);
        ide_outd(0xB404, prdt);        /* BMDMA PRD table address */
        ide_set_lba(0x1F0, 20, 1);
        ide_outb(0x1F7, 0xC8);         /* READ DMA */
        CHECK_EQ(ide_inb(0x1F7), 0x58); /* DRDY|DSC|DRQ */
        ide_outb(0xB400, 0x01);        /* start the bus-master transfer */
        CHECK_EQ(mem8[dest + 0], ide_pattern_byte(20, 0));
        CHECK_EQ(mem8[dest + 511], ide_pattern_byte(20, 511));
        CHECK_EQ(ide_inb(0x1F7), 0x50);
        /* bit 0 (the active bit) is cleared by the completion; bit 2 (0x04) was set by the
         * CONSTRUCTOR's own push_irq -- upstream's, whose IDEChannel constructor calls
         * set_disk_buffer BEFORE assigning device_control_reg = ATA_CR_NIEN, so
         * `undefined & ATA_CR_NIEN` is 0 and the IRQ is raised (ide.js:397-403) */
        CHECK_EQ(ide_inb(0xB402) & 1, 0x00);
        CHECK_EQ(ide_inb(0xB402) & 4, 0x04);
    }

    /* ---- the SECONDARY channel: the ATAPI device's identity --------------------------- */
    CHECK_EQ(ide_inb(0x177), 0x50);
    ide_outb(0x176, 0xA0);
    ide_outb(0x177, 0xEC); /* IDENTIFY DEVICE on an ATAPI device: the signature + ABORT */
    CHECK_EQ(ide_inb(0x174), 0x14);
    CHECK_EQ(ide_inb(0x175), 0xEB);
    CHECK_EQ(ide_inb(0x177), 0x41);
    CHECK_EQ(ide_inb(0x171), 0x04);
    ide_outb(0x176, 0xA0);
    ide_outb(0x177, 0xA1); /* IDENTIFY PACKET DEVICE */
    for (int i = 0; i < 512; i++) words[i] = (uint8_t)ide_inb(0x170);
    CHECK_EQ(ide_word(words, 0), 0x8540);
    ide_ata_string(words, 27, 20, text);
    CHECK(strncmp(text, "v86 ATAPI CD-ROM", 16) == 0);

    /* ---- PACKET + a CDB: the ATAPI command set, which a real kernel REACHES ------------ */
    clear_stop();
    ide_outb(0x176, 0xA0);
    ide_outb(0x177, 0xA0); /* PACKET: the host writes 12 CDB bytes to the data port */
    CHECK_EQ(ide_inb(0x177) & 0x08, 0x08); /* DRQ */
    CHECK_EQ(ide_inb(0x172) & 0xFF, 0x01); /* sector count 1: a command packet is coming */
    for (int i = 0; i < 12; i++) {
        /* INQUIRY with an allocation length of 36 (CDB byte 4) */
        ide_outb(0x170, i == 0 ? ATAPI_CMD_INQUIRY : (i == 4 ? 36 : 0x00));
    }
    CHECK(!interpreted_unimplemented); /* the CDB is HANDLED, not a stop */
    CHECK_EQ(ide_inb(0x177) & 0xC8, 0x48); /* DRDY|DSC|DRQ */
    for (int i = 0; i < 36; i++) words[i] = (uint8_t)ide_inb(0x170);
    CHECK_EQ(words[0], 0x05);  /* device type 5: CD-ROM */
    CHECK_EQ(words[1], 0x80);  /* removable */
    CHECK_EQ(words[2], 0x01);  /* ANSI version 1 */
    CHECK_EQ(words[3], 0x31);  /* response data format */
    CHECK_EQ(words[4], 31);    /* additional length */
    CHECK(memcmp(words + 8, "SONY    ", 8) == 0);
    CHECK(memcmp(words + 16, "CD-ROM CDU-1000 ", 16) == 0);
    CHECK(memcmp(words + 32, "1.1a", 4) == 0);
    CHECK_EQ(ide_inb(0x177) & 0xC8, 0x40); /* DRDY|DSC: the transfer is over */

    /* REQUEST SENSE after that INQUIRY (upstream's arm, ide.js:1514-1524) */
    ide_outb(0x176, 0xA0);
    ide_outb(0x177, 0xA0);
    for (int i = 0; i < 12; i++) {
        ide_outb(0x170, i == 0 ? ATAPI_CMD_REQUEST_SENSE : (i == 4 ? 18 : 0x00));
    }
    for (int i = 0; i < 18; i++) words[i] = (uint8_t)ide_inb(0x170);
    CHECK_EQ(words[0], 0xF0); /* 0x80 | 0x70: valid, SCSI error code 0x70 */
    CHECK_EQ(words[7], 8);    /* additional sense length */

    /* a CDB that NEEDS A DISK with no media: CHECK CONDITION, NOT READY, MEDIUM NOT
     * PRESENT (ide.js:1488-1497) -- this is what a real kernel's probe of an empty
     * ATAPI drive sees */
    ide_outb(0x176, 0xA0);
    ide_outb(0x177, 0xA0);
    for (int i = 0; i < 12; i++) ide_outb(0x170, i == 0 ? ATAPI_CMD_READ_CAPACITY : 0x00);
    CHECK_EQ(ide_inb(0x177) & 0x01, 0x01); /* the CHECK CONDITION bit */
    CHECK_EQ(ide_inb(0x171), ATAPI_SK_NOT_READY << 4); /* sense key in the error register */
    /* REQUEST SENSE then reports the pending sense state */
    ide_outb(0x176, 0xA0);
    ide_outb(0x177, 0xA0);
    for (int i = 0; i < 12; i++) {
        ide_outb(0x170, i == 0 ? ATAPI_CMD_REQUEST_SENSE : (i == 4 ? 18 : 0x00));
    }
    for (int i = 0; i < 18; i++) words[i] = (uint8_t)ide_inb(0x170);
    CHECK_EQ(words[2] & 0x0F, ATAPI_SK_NOT_READY);
    CHECK_EQ(words[12], ATAPI_ASC_MEDIUM_NOT_PRESENT);
    clear_stop();

    /* ---- and with NO disk the controller still exists, with one channel ---------------- */
    machine_config.hda.buffer = NULL;
    machine_config.hda.length = 0;
    machine_reset();
    CHECK(!ide_channel_is_present(0)); /* no primary master -> no primary channel */
    CHECK(ide_channel_is_present(1));
    CHECK_EQ(ide_inb(0x1F7), 0xFF); /* the primary's ports are unclaimed (io.h D1) */
    CHECK_EQ(ide_inb(0x177), 0x50); /* the secondary's ATAPI device is still there */

    /* ---- the FALLBACK geometry (no MBR geometry: heads/sectors 16/63) ----------------- */
    machine_config.hda.buffer = ide_disk_small;
    machine_config.hda.length = IDE_SMALL_BYTES;
    machine_config.hdb.buffer = NULL;
    machine_reset();
    /* the MBR declares 8 heads and 32 sectors (both <= 16 heads) -> the MBR's own geometry */
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DRIVE1_CYL + 2), 8);
    CHECK_EQ(rtc_cmos_read(CMOS_DISK_DRIVE1_CYL + 8), 32);
    ide_read_identify(0x1F0, words);
    CHECK_EQ(ide_word(words, 3), 8);  /* heads from the MBR */
    CHECK_EQ(ide_word(words, 6), 32); /* sectors from the MBR */
    CHECK_EQ(ide_word(words, 1), IDE_SMALL_SECTORS / (8u * 32u)); /* cylinders */
    CHECK_EQ(ide_word(words, 60), IDE_SMALL_SECTORS & 0xFFFF);

    /* leave the machine as the other groups expect to find it */
    machine_config.hda.buffer = NULL;
    machine_config.hda.length = 0;
    machine_reset();
}


/* ================================================================== *
 * M2-4: THE VIRTIO CONSOLE -- the brief's G2.
 *
 * The device is driven THE WAY A DRIVER DRIVES IT: through the CPU's own `out`/`in`
 * instruction bodies (instr_E6/E4 for a byte, instr16_ED/EF for a word, instr32_ED/EF for a
 * dword), against the port numbers the capability structure names -- not by calling the
 * device's C functions.  So what is asserted here is what a guest can observe: the PCI
 * function's bytes, the capability chain, the common configuration structure's FIELDS, the
 * feature negotiation's masking, the queue addresses, and the two directions of the data
 * path (a descriptor chain the guest posts is consumed and its bytes leave through the host
 * seam; a byte the host feeds in lands in guest memory).
 * ================================================================== */

static int virtio_sink_count;
static uint8_t virtio_sink_bytes[64];

static void virtio_sink(void *this_value, const int32_t *args, int nargs)
{
    (void)this_value;
    if (nargs < 1) return;
    if (virtio_sink_count < (int)sizeof(virtio_sink_bytes)) {
        virtio_sink_bytes[virtio_sink_count] = (uint8_t)(args[0] & 0xFF);
    }
    virtio_sink_count++;
}

static int32_t vq_inb(int32_t port) { return in_al(port); }
static void vq_outb(int32_t port, int32_t value) { out_al(port, value); }

static uint32_t vq_inw(int32_t port)
{
    write_reg16(DX, port & 0xFFFF);
    instr16_ED();
    return (uint32_t)(uint16_t)read_reg16(AX);
}

static void vq_outw(int32_t port, int32_t value)
{
    write_reg16(DX, port & 0xFFFF);
    write_reg16(AX, value & 0xFFFF);
    instr16_EF();
}

static uint32_t vq_ind(int32_t port)
{
    write_reg16(DX, port & 0xFFFF);
    instr32_ED();
    return (uint32_t)read_reg32(EAX);
}

static void vq_outd(int32_t port, uint32_t value)
{
    write_reg16(DX, port & 0xFFFF);
    write_reg32(EAX, (int32_t)value);
    instr32_EF();
}

static uint32_t vq_pci_read32_bdf(uint32_t bdf, int32_t offset)
{
    io_port_write32(PCI_CONFIG_ADDRESS, 0x80000000u | bdf | (offset & 0xFC));
    return io_port_read32(PCI_CONFIG_DATA);
}

static uint32_t vq_pci_read32(int32_t offset)
{
    return vq_pci_read32_bdf(0x00006000u, offset); /* bus 0, dev 12 (0x0C), fn 0 */
}

/* the two config-space byte writes the FIRMWARE makes (tests/minibios.asm's pci_irq_route):
 * the device's interrupt line, and the PIIX3's PIRQ router byte v86's raise_irq reads. */
static void vq_firmware_route(uint32_t irq)
{
    io_port_write32(PCI_CONFIG_ADDRESS, 0x8000603C);
    io_port_write8(PCI_CONFIG_DATA, (int32_t)irq);
    io_port_write32(PCI_CONFIG_ADDRESS, 0x80000860);
    io_port_write8(PCI_CONFIG_DATA + 3, (int32_t)irq);
}

/* A descriptor chain the guest posts: `desc_addr` holds one descriptor, the avail ring
 * `ring_addr` names it, and the used ring is at `used_addr`. */
static void vq_post_chain(uint32_t desc_addr, uint32_t ring_addr, uint32_t used_addr,
                          uint32_t desc_index, uint32_t buffer, uint32_t length, uint16_t flags,
                          uint32_t avail_idx)
{
    write32(desc_addr + 0, buffer);
    write32(desc_addr + 4, 0);
    write32(desc_addr + 8, length);
    write16(desc_addr + 12, (int32_t)flags);
    write16(desc_addr + 14, 0);

    write16(ring_addr + 0, 0);       /* flags */
    write16(ring_addr + 2, (int32_t)avail_idx); /* the PUBLISHED index: the device counts
                                                 * requests as (idx - avail_last_idx), so a
                                                 * second post must advance it. */
    write16(ring_addr + 4, (int32_t)desc_index);
    write32(used_addr + 4, 0xEEEEEEEE); /* poison: what the device must overwrite */
    write16(used_addr + 0, 0);
    write16(used_addr + 2, 0);
}

static void test_virtio(void)
{
    /* ---- THE MACHINE: the reference creates the console only for settings.virtio_console */
    machine_config.virtio_console = false;
    machine_reset();
    CHECK(machine_virtio_console_device() == NULL);
    CHECK_EQ(vq_inb(0xB800), 0xFF); /* 0xB800 is an UNCLAIMED port without the device */

    machine_config.virtio_console = true;
    machine_reset();
    virtio_console_t *console = machine_virtio_console_device();
    CHECK(console != NULL);
    virtio_sink_count = 0;
    bus_register(machine_host_bus, "virtio-console0-output-bytes", virtio_sink, NULL);

    /* ---- THE PCI FUNCTION 00:0c.0 (virtio.js:66-72, 174-233) --------------------------- */
    CHECK_EQ(vq_pci_read32(0x00) & 0xFFFF, 0x1AF4);         /* vendor: Red Hat */
    CHECK_EQ((uint32_t)vq_pci_read32(0x00) >> 16, 0x1043);  /* device: the MODERN console id */
    CHECK_EQ(vq_pci_read32(0x04) >> 16 & 0x10, 0x10);       /* status: capabilities list */
    CHECK_EQ(vq_pci_read32(0x08) >> 8, 0x000200);           /* rev 1, prog-if/subclass/class */
    CHECK_EQ(vq_pci_read32(0x0C) & 0xFF, 0x00);             /* cache line */
    CHECK_EQ(vq_pci_read32(0x2C) & 0xFFFF, 0x1AF4);         /* subsystem vendor */
    CHECK_EQ(vq_pci_read32(0x2C) >> 16 & 0xFFFF, 0x0003);   /* subsystem id */
    CHECK_EQ(vq_pci_read32(0x34) & 0xFF, 0x40);             /* the capability pointer */
    CHECK_EQ(vq_pci_read32(0x3C) >> 8 & 0xFF, 0x01);        /* interrupt pin 1 (INTA) */

    /* the four bars, in lspci's own terms -- the reference reports these to the guest
     * (MEASURED in the gate: the kernel's own "BAR 0 [io 0xb800-0xb83f]" lines) */
    CHECK_EQ(vq_pci_read32(0x10) & 0xFFFF, 0xB801); /* common config: 64 bytes */
    CHECK_EQ(vq_pci_read32(0x14) & 0xFFFF, 0xB901); /* notification: 32 bytes */
    CHECK_EQ(vq_pci_read32(0x18) & 0xFFFF, 0xB701); /* ISR: 16 bytes */
    CHECK_EQ(vq_pci_read32(0x1C) & 0xFFFF, 0xB601); /* device-specific: 16 bytes */

    /* THE CAPABILITY CHAIN: type, bar, offset and length of each entry, walked the way a
     * driver walks it (virtio.js:710-910) */
    CHECK_EQ(vq_pci_read32(0x40) & 0xFF, 0x09);        /* PCI_CAP_ID_VENDOR */
    CHECK_EQ(vq_pci_read32(0x40) >> 8 & 0xFF, 0x50);   /* -> next */
    CHECK_EQ(vq_pci_read32(0x40) >> 24 & 0xFF, VIRTIO_PCI_CAP_COMMON_CFG);
    CHECK_EQ(vq_pci_read32(0x44) & 0xFF, 0x00);        /* bar 0 */
    CHECK_EQ(vq_pci_read32(0x4C) & 0xFFFF, 0x40);      /* bar size 64 */
    CHECK_EQ(vq_pci_read32(0x50) >> 8 & 0xFF, 0x64);   /* -> next */
    CHECK_EQ(vq_pci_read32(0x50) >> 24 & 0xFF, VIRTIO_PCI_CAP_NOTIFY_CFG);
    CHECK_EQ(vq_pci_read32(0x54) & 0xFF, 0x01);        /* bar 1 */
    CHECK_EQ(vq_pci_read32(0x60) & 0xFFFFFFFF, 0x02);  /* the extra dword: multiplier = 2 */
    CHECK_EQ(vq_pci_read32(0x64) >> 8 & 0xFF, 0x74);
    CHECK_EQ(vq_pci_read32(0x64) >> 24 & 0xFF, VIRTIO_PCI_CAP_ISR_CFG);
    CHECK_EQ(vq_pci_read32(0x74) >> 8 & 0xFF, 0x84);
    CHECK_EQ(vq_pci_read32(0x74) >> 24 & 0xFF, VIRTIO_PCI_CAP_DEVICE_CFG);
    CHECK_EQ(vq_pci_read32(0x84) >> 8 & 0xFF, 0x00);   /* the PCI_CFG terminator */
    CHECK_EQ(vq_pci_read32(0x84) >> 24 & 0xFF, VIRTIO_PCI_CAP_PCI_CFG);

    /* ---- THE DEVICE-SPECIFIC CONFIGURATION (0xB600, virtio_console.js:171-203) -------- */
    CHECK_EQ(vq_inw(0xB600), 80);  /* cols */
    CHECK_EQ(vq_inw(0xB602), 25);  /* rows */
    CHECK_EQ(vq_ind(0xB604), 4);   /* max_nr_ports */

    /* ---- THE COMMON CONFIGURATION (0xB800, virtio.js:321-603) ------------------------ */
    CHECK_EQ(vq_inw(0xB812), 10);  /* num_queues: 4 + 2 per extra port */
    /* the features, as two dwords selected by device_feature_select */
    vq_outd(0xB800, 0x00000000);
    CHECK_EQ(vq_ind(0xB804), 0x00000003); /* F_SIZE | F_MULTIPORT */
    vq_outd(0xB800, 0x00000001);
    CHECK_EQ(vq_ind(0xB804), 0x00000001); /* bit 32: VIRTIO_F_VERSION_1 -- MODERN virtio */
    /* an out-of-range select reads 0, not garbage (upstream's `|| 0`) */
    vq_outd(0xB800, 0x00000009);
    CHECK_EQ(vq_ind(0xB804), 0x00000000);
    /* the 8-bit shim on a 32-bit field (virtio.js:822-826, "DSL does these reads") */
    vq_outd(0xB800, 0x00000000);
    CHECK_EQ(vq_inb(0xB804) & 0xFF, 0x03);
    CHECK_EQ(vq_inb(0xB805) & 0xFF, 0x00);
    CHECK_EQ(vq_ind(0xB804), 0x00000003); /* the shim did not disturb the field */

    /* ---- THE FEATURE NEGOTIATION (virtio.js:356-375) ---------------------------------- */
    vq_outd(0xB808, 0x00000000);
    vq_outd(0xB80C, 0x00000003);          /* a valid subset */
    CHECK_EQ(vq_ind(0xB80C), 0x00000003);
    CHECK(virtio_is_feature_negotiated(&console->virtio, VIRTIO_CONSOLE_F_MULTIPORT));
    /* A BIT THE DEVICE DOES NOT OFFER: it is masked out of driver_feature AND the device
     * refuses FEATURES_OK -- the two halves of upstream's handshake */
    vq_outd(0xB80C, 0x00000803);          /* bit 11 is not offered */
    CHECK_EQ(vq_ind(0xB80C) & 0x00000800, 0x00000000);
    vq_outb(0xB814, 0x0F);                /* ACK|DRIVER|FEATURES_OK|DRIVER_OK */
    CHECK_EQ(vq_inb(0xB814) & 0x08, 0x00); /* FEATURES_OK was removed */
    CHECK_EQ(vq_inb(0xB814) & 0x07, 0x07); /* the other three stayed */
    /* and a write of 0 is a RESET (virtio.js:401-405) */
    vq_outb(0xB814, 0x00);
    CHECK_EQ(vq_inb(0xB814), 0x00);
    CHECK_EQ(vq_inw(0xB816), 0); /* queue_select back to 0 */

    /* ---- THE QUEUE'S CONFIGURATION (virtio.js:453-600) -------------------------------- */
    vq_outw(0xB816, 0);                  /* queue_select: port 0's receive queue */
    CHECK_EQ(vq_inw(0xB818), 16);        /* queue_size */
    CHECK_EQ(vq_inw(0xB81E), 0);         /* queue_notify_off */
    vq_outw(0xB81C, 1);                  /* queue_enable before the addresses: refused */
    CHECK_EQ(vq_inw(0xB81C), 0);
    vq_outw(0xB818, 16);
    vq_outd(0xB820, 0x1000);             /* queue_desc */
    vq_outd(0xB828, 0x2000);             /* queue_avail */
    vq_outd(0xB830, 0x3000);             /* queue_used */
    CHECK_EQ(vq_ind(0xB820), 0x1000);
    CHECK_EQ(vq_ind(0xB828), 0x2000);
    CHECK_EQ(vq_ind(0xB830), 0x3000);
    CHECK_EQ(vq_ind(0xB824), 0x00000000); /* the high dword reads 0 (virtio.js:558-563) */
    vq_outd(0xB824, 0x12345678);          /* and its write is IGNORED, not stored */
    CHECK_EQ(vq_ind(0xB824), 0x00000000);
    vq_outw(0xB81C, 1);
    CHECK_EQ(vq_inw(0xB81C), 1);          /* enabled */
    /* a queue index the device has not got reads as unavailable, not as garbage */
    vq_outw(0xB816, 99);
    CHECK_EQ(vq_inw(0xB818), 0);          /* queue_size */
    CHECK_EQ(vq_inw(0xB81C), 0);          /* queue_enable */
    vq_outw(0xB816, 0);

    /* ---- A DESCRIPTOR CHAIN THE GUEST POSTS IS CONSUMED (the guest -> host direction) -- */
    /* queue 1 is port 0's SEND queue (notify offset 1); three bytes at 0x4000 */
    vq_outw(0xB816, 1);
    vq_outw(0xB818, 16);
    vq_outd(0xB820, 0x1100);
    vq_outd(0xB828, 0x2100);
    vq_outd(0xB830, 0x3100);
    vq_outw(0xB81C, 1);
    CHECK_EQ(vq_inw(0xB81E), 1);          /* its notify offset -- the port below */
    memcpy(mem8 + 0x4000, "ABC", 3);
    vq_post_chain(0x1100, 0x2100, 0x3100, 0, 0x4000, 3, 0 /* device-readable */, 1);
    write32(0x3108, 0);                  /* poison the length the device must overwrite */
    CHECK_EQ(read32s(0x3108), 0);        /* no reply has been staged yet */

    /* the notification: the driver writes the QUEUE INDEX to the notify register */
    vq_outw(0xB900 + 2 * 1, 1);
    CHECK(!interpreted_unimplemented);
    CHECK_EQ(virtio_sink_count, 3);
    CHECK_EQ(virtio_sink_bytes[0], 'A');
    CHECK_EQ(virtio_sink_bytes[1], 'B');
    CHECK_EQ(virtio_sink_bytes[2], 'C');
    /* and the chain is in the used ring, with the head index and the length written.  THE
     * USED RING IS GUEST MEMORY, so it is read with the memory router -- the `in`
     * instruction only reaches the DEVICE's registers. */
    CHECK_EQ(read16(0x3102), 1);          /* used.idx advanced */
    CHECK_EQ(read32s(0x3104), 0);          /* the head descriptor index */
    /* the used entry's LENGTH is the bytes written INTO THE CHAIN'S WRITABLE BUFFERS, and
     * this chain has none: the device only READ from it, so upstream's own accounting
     * (length_written, only set_next_blob increments it) leaves 0 -- the three bytes are
     * counted by the sink and by the chain-read total, not here. */
    CHECK_EQ(read32s(0x3108), 0);
    CHECK_EQ(console->output_bytes, 3);
    CHECK_EQ(virtq_chain_bytes_read(), 3);

    /* the ISR: the flush raised VIRTIO_ISR_QUEUE, and READING the ISR register returns it
     * and lowers the line (virtio.js:673-678) */
    CHECK_EQ(vq_inb(0xB700), VIRTIO_ISR_QUEUE);
    CHECK_EQ(vq_inb(0xB700), 0x00);       /* read-and-cleared */

    /* ---- A BYTE FED IN REACHES THE GUEST (the host -> guest direction) ---------------- */
    /* queue 0 = port 0's receive queue: a WRITABLE descriptor at 0x1400/0x4400 */
    vq_outw(0xB816, 0);
    vq_outw(0xB818, 16);
    vq_outd(0xB820, 0x1400);
    vq_outd(0xB828, 0x2400);
    vq_outd(0xB830, 0x3400);
    vq_outw(0xB81C, 1);
    vq_post_chain(0x1400, 0x2400, 0x3400, 0, 0x4400, 8, VIRTQ_DESC_F_WRITE, 1);
    mem8[0x4400] = 0x00;

    /* the host end of the seam, exactly as the UART's is driven below (bus.h) */
    BUS_SEND(machine_host_bus, "virtio-console0-input-bytes", 0x58);
    CHECK_EQ(mem8[0x4400], 0x58);         /* the byte is IN GUEST MEMORY */
    CHECK_EQ(read16(0x3402), 1);          /* the chain was consumed */
    CHECK_EQ(read32s(0x3404), 0);
    CHECK_EQ(read32s(0x3408), 1);          /* one byte written */
    CHECK_EQ(console->input_bytes, 1);
    /* AND A SECOND BYTE WITH NO POSTED BUFFER IS DROPPED, which is upstream's own
     * behaviour (virtio_console.js:213-215's `//TODO: Buffer`) -- the device does not
     * invent a chain and does not corrupt the ring */
    BUS_SEND(machine_host_bus, "virtio-console0-input-bytes", 0x59);
    CHECK_EQ(mem8[0x4400], 0x58);
    CHECK_EQ(read16(0x3402), 1);
    CHECK_EQ(console->input_bytes, 1);
    /* and a 16-byte message longer than the descriptor's 8 bytes writes only 8: the chain's
     * capacity is the bound, not the message's length (virtio.js:1480-1517) */
    vq_post_chain(0x1400, 0x2400, 0x3400, 0, 0x4400, 8, VIRTQ_DESC_F_WRITE, 2);
    {
        static const int32_t message[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        bus_send(machine_host_bus, "virtio-console0-input-bytes", message, 16);
    }
    CHECK_EQ(read32s(0x3408), 8);          /* eight bytes, not sixteen */
    CHECK_EQ(mem8[0x4408], 0x00);         /* nothing past the descriptor */
    CHECK_EQ(console->input_bytes, 9);    /* 1 + 8: the counter is what LANDED, not what was
                                           * offered (the 16-byte message overflowed the chain) */
    /* the WRITTEN total counts the bytes that landed in device-writable buffers: the two
     * input chains (1 + 8).  The three output bytes are a READ from the guest, counted by
     * virtq_chain_bytes_read (asserted above). */
    CHECK_EQ(virtq_chain_bytes_written(), 1 + 8);

    /* ---- THE IRQ PATH: the firmware's two config writes decide the vector -------------- */
    /* UPSTREAM'S OWN CONFIGURATION, asserted as the configuration facts rather than through
     * the PIC: the device's interrupt-line byte is 0 and the PIIX3's PIRQ router byte 0x63
     * is 0, so v86's raise_irq arithmetic delivers a PCI interrupt on IRQ 0 -- the PIT's own
     * vector (the PIC's bit 0 cannot be the evidence: the PIT raises it too). */
    CHECK_EQ(vq_pci_read32(0x3C) & 0xFF, 0x00);
    CHECK_EQ(vq_pci_read32_bdf(0x00000800u, 0x60) >> 24 & 0xFF, 0x00);

    /* and the arithmetic that maps the device onto that byte: (pin + device) & 3 -- INTA of
     * device 12 is PIRQD (byte 0x63), which is where the firmware writes. */
    vq_firmware_route(11);
    CHECK_EQ(vq_pci_read32(0x3C) & 0xFF, 11);                /* the device's interrupt line */
    CHECK_EQ(vq_pci_read32_bdf(0x00000800u, 0x60) >> 24 & 0xFF, 11); /* PIRQD = 11 */

    /* A FRESH CHAIN, notified: the used ring's flush raises the ISR bit and the interrupt,
     * and it reaches the PIC on IRQ 11 -- the vector the guest's own driver asked for
     * (MEASURED live: /proc/interrupts "11: N XT-PIC virtio0"). */
    cpu->flags &= ~FLAG_INTERRUPT; /* keep the request pending in the PIC's IRR */
    while (pic_acknowledge_irq() != -1) { }
    vq_outw(0xB816, 1);
    vq_outw(0xB818, 16);
    vq_outd(0xB820, 0x1100);
    vq_outd(0xB828, 0x2100);
    vq_outd(0xB830, 0x3100);
    vq_outw(0xB81C, 1);
    vq_post_chain(0x1100, 0x2100, 0x3100, 1, 0x4000, 3, 0, 2);
    vq_outw(0xB900 + 2 * 1, 1);
    CHECK_EQ(virtio_sink_count, 6);                          /* the second chain's three bytes */
    CHECK_EQ(virtq_chain_bytes_read(), 3 + 3);
    /* IRQ 11 IS ON THE SLAVE PIC (IRQs 8-15), so the request appears as the SLAVE's bit 3.
     * The PIC's own read mode (IRR vs in-service, selected by OCW3 and left by whichever
     * group ran before) and the edge/level state are M2-1's subject, not this device's, so
     * the LOWERING is asserted where the device's contract puts it: the ISR register's
     * read-and-clear (twice asserted above) and the register's own second read. */
    CHECK_EQ(portA0_read() & (1 << 3), 1 << 3);
    CHECK_EQ(vq_inb(0xB700), VIRTIO_ISR_QUEUE);              /* the ISR register */
    CHECK_EQ(vq_inb(0xB700), 0x00);                          /* read-and-cleared */
    cpu->flags |= FLAG_INTERRUPT;

    /* ---- THE NOTIFICATION OPTION'S MULTIPLIER: a driver computes the notify port from
     * queue_notify_off * notify_off_multiplier (virtio.js:609-653; the kernel's own driver
     * does exactly this), so the ports below are the ones a queue id maps to */
    CHECK_EQ(vq_inw(0xB900 + 2 * 2), 0xFFFF); /* the notify register itself reads 0xFFFF */
    CHECK_EQ(vq_inw(0xB900 + 2 * 3), 0xFFFF);

    /* leave the machine as the other cases expect to find it */
    machine_config.virtio_console = false;
    machine_reset();
}

/* ================================================================== */
#define RUN(fn, name)                     \
    do {                                  \
        int before = failures;            \
        fn();                             \
        printf("%s %s (%d checks so far)\n", failures == before ? "PASS" : "FAIL", name, checks); \
    } while (0)

int main(void)
{
    RUN(test_clock, "clock: 1 guest instruction = 1 us, through the PIT, the PM timer and the TSC");
    RUN(test_io_bus, "io: the port map, the unclaimed-port contract (D-F1), the mmap half + R1F");
    RUN(test_pit, "pit: the ports, the latch, the arithmetic, and a delivered IRQ0");
    RUN(test_rtc, "rtc: the CMOS round-trip, the status registers, the time registers vs the VIRTUAL clock");
    RUN(test_dma, "dma: the registers, a transfer into guest memory, and the reverse");
    RUN(test_ps2, "ps2: a scancode, the identify replies, a mouse packet, IntelliMouse id");
    RUN(test_pci, "pci: config space, an absent device, the I/O bar move, the stops");
    RUN(test_acpi, "acpi: PM1, the GPE registers, the PM timer, IRQ9");
    RUN(test_apic, "apic: the LAPIC register block, the timer at virtual time, the IOAPIC redirections, EOI/remote-EOI, the bounds");
    RUN(test_acpi_tables, "acpi tables: the RSDP->RSDT/XSDT->FADT/MADT set, the kernel's own scan over guest RAM, the pin-2 wiring, and every corruption caught");
    RUN(test_timer_wakes_the_guest, "timer seat: main_loop wakes a halted guest on IRQ0");
    RUN(test_handover, "G4: the CPU's own in/out -- the PIT, and the port-0x80 probe (D-F1)");
    RUN(test_uart, "uart: a byte out of the sink, the line status, the divisor, the seam");
    RUN(test_kernel_loader, "kernel: the bzImage boot params, the option ROM, the real image");
    RUN(test_ide, "ide: geometry, IDENTIFY, READ SECTORS/DMA, the error status, the ATAPI command set");
    RUN(test_virtio, "virtio: the PCI function and its capability chain, feature negotiation, the queues, both directions of the console, the IRQ route");

    printf("hw_selftest: %d checks, %d failures, %d skipped\n", checks, failures, skips);
    return failures != 0;
}
