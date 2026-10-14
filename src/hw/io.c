/*
 * io.c -- the ISA I/O port bus, the memory-mapped device dispatch, and the port bridge in
 * front of the device layer.  See io.h for the file's contract, its three deliberate
 * divergences (D1 the unclaimed port answers exactly as the pinned reference and the
 * LOUDNESS became a diagnostic; D2 an entry with NO handler stops; D3 write callbacks take
 * the port) and the mmap-to-memory_device_hooks mapping.
 */

#include "io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu/cpu.h"         /* unimplemented_instruction */
#include "../cpu/instructions.h"/* handle_irqs */
#include "../cpu/pic.h"         /* the six PIC port accessors intercepted in front */
#include "../cpu/string.h"      /* io_port_*' declarations (the in/out instructions are its) */

io_port_entry_t io_ports[IO_PORT_COUNT];

/* ------------------------------------------------------------------ *
 * the empty entry -- io.js:36-77
 *
 * Upstream identifies the default handlers BY IDENTITY (`entry.write8 ===
 * this.empty_port_write`, io.js:334) to decide whether to LOG, and the default handlers
 * THEMSELVES are what answers an unclaimed port (0xFF / 0xFFFF / -1 for reads, a no-op for
 * writes, io.js:50-77).  Both halves are reproduced: the dispatch below CALLS these
 * functions (so the answer is the reference's) and compares against them to decide whether
 * to log.
 * ------------------------------------------------------------------ */
static int32_t empty_port_read8(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return 0xFF;
}

static int32_t empty_port_read16(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return 0xFFFF;
}

static int32_t empty_port_read32(void *device, int32_t port)
{
    (void)device;
    (void)port;
    return -1;
}

static void empty_port_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    (void)value;
}

/* ------------------------------------------------------------------ *
 * D-F1's DIAGNOSTIC: the LOUDNESS of an unclaimed port, moved off the machine's semantics
 * and onto stderr.  `V86C_LOG_PORTS=1` (any nonempty value other than "0") makes every
 * unclaimed access print the access, the port and its seabios name -- upstream's
 * `LOG_ALL_IO`/`LOG_IO` dbg_log, io.js:332-394.  A guest CANNOT observe the difference:
 * nothing below this line changes a return value, a device, or the port map.
 *
 * The environment is read ONCE (the first unclaimed access); a run either has the
 * diagnostic or it does not.
 * ------------------------------------------------------------------ */
static bool io_log_ports_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("V86C_LOG_PORTS");
        enabled = v != NULL && v[0] != '\0' && v[0] != '0';
    }
    return enabled != 0;
}

static void io_log_unclaimed_read(int32_t width, int32_t port, int32_t value)
{
    if (!io_log_ports_enabled()) return;
    fprintf(stderr, "io: read%d  port 0x%04x %s -> 0x%x (unclaimed)\n", (int)width,
            (unsigned)port, io_get_port_description(port), (unsigned)value);
}

static void io_log_unclaimed_write(int32_t width, int32_t port, int32_t value)
{
    if (!io_log_ports_enabled()) return;
    fprintf(stderr, "io: write%d port 0x%04x %s <- 0x%x (unclaimed, discarded)\n",
            (int)width, (unsigned)port, io_get_port_description(port), (unsigned)value);
}

io_port_entry_t io_create_empty_entry(void)
{
    io_port_entry_t entry;
    entry.read8 = empty_port_read8;
    entry.read16 = empty_port_read16;
    entry.read32 = empty_port_read32;
    entry.write8 = empty_port_write;
    entry.write16 = empty_port_write;
    entry.write32 = empty_port_write;
    entry.device = NULL;
    return entry;
}

/* ------------------------------------------------------------------ *
 * the memory map -- io.js:259-329 + cpu.js:227-289
 * ------------------------------------------------------------------ */

#define MMAP_BLOCK_COUNT (1u << (32 - MMAP_BLOCK_BITS)) /* 32768: io.js:19-24 */

typedef struct io_mmap_entry {
    int32_t (*read8)(uint32_t addr);
    void (*write8)(uint32_t addr, int32_t value);
    int32_t (*read32)(uint32_t addr);
    void (*write32)(uint32_t addr, int32_t value);
} io_mmap_entry_t;

static io_mmap_entry_t io_mmap[MMAP_BLOCK_COUNT];

/* io.js:259-277.  The shims are installed when a registration gives no 32-bit handler, and
 * they decompose the access into four 8-bit calls on the SAME block.  The shim looks its
 * 8-bit handler up at CALL time, as upstream's closure does. */
static int32_t mmap_read32_shim(uint32_t addr)
{
    int32_t (*fn)(uint32_t) = io_mmap[addr >> MMAP_BLOCK_BITS].read8;
    return (int32_t)((uint32_t)fn(addr) | (uint32_t)fn(addr + 1) << 8 |
                     (uint32_t)fn(addr + 2) << 16 | (uint32_t)fn(addr + 3) << 24);
}

static void mmap_write32_shim(uint32_t addr, int32_t value)
{
    void (*fn)(uint32_t, int32_t) = io_mmap[addr >> MMAP_BLOCK_BITS].write8;
    fn(addr, value & 0xFF);
    fn(addr + 1, value >> 8 & 0xFF);
    fn(addr + 2, value >> 16 & 0xFF);
    fn(addr + 3, value >> 24 & 0xFF);
}

void io_mmap_register(uint32_t addr, uint32_t size, int32_t (*read8)(uint32_t),
                      void (*write8)(uint32_t, int32_t), int32_t (*read32)(uint32_t),
                      void (*write32)(uint32_t, int32_t))
{
    DBG_ASSERT((addr & (MMAP_BLOCK_SIZE - 1)) == 0);
    DBG_ASSERT(size != 0 && (size & (MMAP_BLOCK_SIZE - 1)) == 0);

    if (read32 == NULL) read32 = mmap_read32_shim;
    if (write32 == NULL) write32 = mmap_write32_shim;

    uint32_t aligned_addr = addr >> MMAP_BLOCK_BITS;
    for (; size > 0; aligned_addr++) {
        DBG_ASSERT(aligned_addr < MMAP_BLOCK_COUNT);
        io_mmap[aligned_addr].read8 = read8;
        io_mmap[aligned_addr].write8 = write8;
        io_mmap[aligned_addr].read32 = read32;
        io_mmap[aligned_addr].write32 = write32;
        size -= MMAP_BLOCK_SIZE;
    }
}

/* An unregistered block falls back to memory.c's default table -- see io.h. */
static int32_t io_mmap_read8(uint32_t addr)
{
    int32_t (*fn)(uint32_t) = io_mmap[addr >> MMAP_BLOCK_BITS].read8;
    return fn == NULL ? memory_default_device_hooks()->read8(addr) : fn(addr);
}

static void io_mmap_write8(uint32_t addr, int32_t value)
{
    void (*fn)(uint32_t, int32_t) = io_mmap[addr >> MMAP_BLOCK_BITS].write8;
    if (fn == NULL) memory_default_device_hooks()->write8(addr, value);
    else fn(addr, value);
}

static int32_t io_mmap_read32(uint32_t addr)
{
    int32_t (*fn)(uint32_t) = io_mmap[addr >> MMAP_BLOCK_BITS].read32;
    return fn == NULL ? memory_default_device_hooks()->read32(addr) : fn(addr);
}

static void io_mmap_write32(uint32_t addr, int32_t value)
{
    void (*fn)(uint32_t, int32_t) = io_mmap[addr >> MMAP_BLOCK_BITS].write32;
    if (fn == NULL) memory_default_device_hooks()->write32(addr, value);
    else fn(addr, value);
}

/* ------------------------------------------------------------------ *
 * the memory router's device hook table (memory.h).  This is the mmap dispatch above
 * translated to the port's hook shape; the 16/64/128-bit writers are cpu.js:248-289's
 * decompositions (write16 -> two write8; write64 -> two write32; write128 -> four write32).
 * ------------------------------------------------------------------ */
static int32_t io_hook_read8(uint32_t addr) { return io_mmap_read8(addr); }
static int32_t io_hook_read32(uint32_t addr) { return io_mmap_read32(addr); }
static void io_hook_write8(uint32_t addr, int32_t value) { io_mmap_write8(addr, value); }

static void io_hook_write16(uint32_t addr, int32_t value)
{
    io_mmap_write8(addr, value & 0xFF);
    io_mmap_write8(addr + 1, value >> 8 & 0xFF);
}

static void io_hook_write32(uint32_t addr, int32_t value) { io_mmap_write32(addr, value); }

static void io_hook_write64(uint32_t addr, uint64_t value)
{
    io_mmap_write32(addr, (int32_t)(uint32_t)value);
    io_mmap_write32(addr + 4, (int32_t)(uint32_t)(value >> 32));
}

static void io_hook_write128(uint32_t addr, uint64_t v0, uint64_t v1)
{
    io_mmap_write32(addr, (int32_t)(uint32_t)v0);
    io_mmap_write32(addr + 4, (int32_t)(uint32_t)(v0 >> 32));
    io_mmap_write32(addr + 8, (int32_t)(uint32_t)v1);
    io_mmap_write32(addr + 12, (int32_t)(uint32_t)(v1 >> 32));
}

static const memory_device_hooks_t io_hooks = {
    .read8 = io_hook_read8,
    .read32 = io_hook_read32,
    .write8 = io_hook_write8,
    .write16 = io_hook_write16,
    .write32 = io_hook_write32,
    .write64 = io_hook_write64,
    .write128 = io_hook_write128,
};

/* ------------------------------------------------------------------ *
 * init -- io.js:22-61
 * ------------------------------------------------------------------ */
void io_init(void)
{
    io_port_entry_t empty = io_create_empty_entry();
    for (uint32_t i = 0; i < IO_PORT_COUNT; i++) {
        io_ports[i] = empty;
    }

    memset(io_mmap, 0, sizeof(io_mmap));

    /* DIVERGENCE D4: THE UNCLAIMED RANGE IS NOT RE-REGISTERED HERE.  Upstream's
     * constructor registers io.js:42-61's callbacks for [memory_size, MMAP_MAX) and the
     * VGA device then overwrites the blocks it owns; this port has no VGA device yet, and
     * memory.c's default table IS the stand-in for it -- MEASURED against the pinned
     * reference, it answers 0xFF/-1 for every unclaimed address AND 0 for the two ranges
     * the always-present VGA owns (0xA0000-0xBFFFF and the 8 MiB LFB from 0xE0000000).
     * Registering io.js's callbacks here would SHADOW that carve-out for the whole LFB and
     * a guest would read 0xFFFFFFFF where the reference reads 0 -- so the unregistered
     * block falls through to memory.c's table instead.  A device that registers a range
     * (M2's VGA, virtio, ...) still overwrites it through io_mmap_register. */
    memory_set_device_hooks(&io_hooks);
}

/* ------------------------------------------------------------------ *
 * registration -- io.js:81-256
 * ------------------------------------------------------------------ */

/* D2: an entry whose handler pointer was never installed.  io_init leaves none behind, so
 * this is unreachable in a correctly built machine -- it is the arm that keeps a corrupted
 * or half-initialised port map LOUD instead of calling through NULL.  Upstream's release
 * build would throw a TypeError at the same place (io.js:352 `entry.read8.call(...)`). */
static void io_stop_width(int32_t port, int width, const char *what)
{
    char name[192];
    snprintf(name, sizeof(name), "%s%d(port=0x%x%s%s) (no handler installed for this port at this width)",
             what, width, (unsigned)port,
             io_get_port_description(port)[0] ? ", " : "", io_get_port_description(port));
    unimplemented_instruction(name);
}

static int32_t unregistered_read(void *device, int32_t port)
{
    (void)device;
    io_stop_width(port, 8, "io_port_read");
    return 0;
}

static void unregistered_write(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)value;
    io_stop_width(port, 8, "io_port_write");
}

void io_register_read(int32_t port, void *device, io_read_fn r8, io_read_fn r16, io_read_fn r32)
{
    DBG_ASSERT(port >= 0 && port < 0x10000);
    DBG_ASSERT(device != NULL);
    DBG_ASSERT(r8 != NULL || r16 != NULL || r32 != NULL);

    if (r8) io_ports[port].read8 = r8;
    if (r16) io_ports[port].read16 = r16;
    if (r32) io_ports[port].read32 = r32;
    io_ports[port].device = device;
}

void io_register_write(int32_t port, void *device, io_write_fn w8, io_write_fn w16, io_write_fn w32)
{
    DBG_ASSERT(port >= 0 && port < 0x10000);
    DBG_ASSERT(device != NULL);
    DBG_ASSERT(w8 != NULL || w16 != NULL || w32 != NULL);

    if (w8) io_ports[port].write8 = w8;
    if (w16) io_ports[port].write16 = w16;
    if (w32) io_ports[port].write32 = w32;
    io_ports[port].device = device;
}

/* io.js:186-215 -- the composed handlers.  Upstream closes over the four registered
 * callbacks; here the port map is recomputed at call time, which is the same four calls in
 * the same order (D3). */
static int32_t consecutive_read16_1(void *device, int32_t port)
{
    return io_ports[port].read8(device, port) | io_ports[port + 1].read8(device, port + 1) << 8;
}

static int32_t consecutive_read16_2(void *device, int32_t port)
{
    return io_ports[port].read8(device, port) | io_ports[port + 1].read8(device, port + 1) << 8;
}

static int32_t consecutive_read32(void *device, int32_t port)
{
    return (int32_t)((uint32_t)io_ports[port].read8(device, port) |
                     (uint32_t)io_ports[port + 1].read8(device, port + 1) << 8 |
                     (uint32_t)io_ports[port + 2].read8(device, port + 2) << 16 |
                     (uint32_t)io_ports[port + 3].read8(device, port + 3) << 24);
}

static void consecutive_write16_1(void *device, int32_t port, int32_t value)
{
    io_ports[port].write8(device, port, value & 0xFF);
    io_ports[port + 1].write8(device, port + 1, value >> 8 & 0xFF);
}

static void consecutive_write16_2(void *device, int32_t port, int32_t value)
{
    io_ports[port].write8(device, port, value & 0xFF);
    io_ports[port + 1].write8(device, port + 1, value >> 8 & 0xFF);
}

static void consecutive_write32(void *device, int32_t port, int32_t value)
{
    io_ports[port].write8(device, port, value & 0xFF);
    io_ports[port + 1].write8(device, port + 1, value >> 8 & 0xFF);
    io_ports[port + 2].write8(device, port + 2, value >> 16 & 0xFF);
    io_ports[port + 3].write8(device, port + 3, (int32_t)((uint32_t)value >> 24));
}

void io_register_read_consecutive(int32_t port, void *device, io_read_fn r8_1, io_read_fn r8_2,
                                  io_read_fn r8_3, io_read_fn r8_4)
{
    if (r8_3 != NULL && r8_4 != NULL) {
        io_register_read(port, device, r8_1, consecutive_read16_1, consecutive_read32);
        io_register_read(port + 1, device, r8_2, NULL, NULL);
        io_register_read(port + 2, device, r8_3, consecutive_read16_2, NULL);
        io_register_read(port + 3, device, r8_4, NULL, NULL);
    }
    else {
        io_register_read(port, device, r8_1, consecutive_read16_1, NULL);
        io_register_read(port + 1, device, r8_2, NULL, NULL);
    }
}

void io_register_write_consecutive(int32_t port, void *device, io_write_fn w8_1, io_write_fn w8_2,
                                   io_write_fn w8_3, io_write_fn w8_4)
{
    if (w8_3 != NULL && w8_4 != NULL) {
        io_register_write(port, device, w8_1, consecutive_write16_1, consecutive_write32);
        io_register_write(port + 1, device, w8_2, NULL, NULL);
        io_register_write(port + 2, device, w8_3, consecutive_write16_2, NULL);
        io_register_write(port + 3, device, w8_4, NULL, NULL);
    }
    else {
        io_register_write(port, device, w8_1, consecutive_write16_1, NULL);
        io_register_write(port + 1, device, w8_2, NULL, NULL);
    }
}

/* ------------------------------------------------------------------ *
 * the dispatch -- io.js:332-394
 *
 * D-F1 (HANDOFF=v86c unit M2-1F): AN UNCLAIMED PORT ANSWERS, IT DOES NOT STOP.  The entry
 * installed by io_init IS upstream's empty entry, so the dispatch simply CALLS it -- the
 * read returns the reference's 0xFF / 0xFFFF / -1, the write is discarded -- exactly as
 * io.js:337-394 does.  The DEBUG-gated ports (cpu.js:1168-1175: 0x80, 0xE9) have no handler
 * in the pinned RELEASE build either, so 0x80 falls here too; the loudness is now the
 * V86C_LOG_PORTS diagnostic above, which DECIDES NOTHING a guest can see.
 *
 * This replaces M1c/M2-1's stop-by-name for unclaimed ports.  That policy was right while
 * there was no device layer (it refused to fabricate a value); with the reference's own
 * empty-entry answer available it was a DIVERGENCE, and it stopped the first boot at the
 * first POST code.  See results/m2-1f-policy.md (declared divergence D-F1).
 * ------------------------------------------------------------------ */

int32_t io_portmap_read8(int32_t port)
{
    io_port_entry_t *entry = &io_ports[(uint32_t)port & 0xFFFF];
    if (entry->read8 == NULL) return unregistered_read(entry->device, port);
    int32_t value = entry->read8(entry->device, (int32_t)((uint32_t)port & 0xFFFF));
    if (entry->read8 == empty_port_read8) {
        io_log_unclaimed_read(8, port, value);
    }
    DBG_ASSERT(value >= 0 && value < 0x100);
    return value;
}

int32_t io_portmap_read16(int32_t port)
{
    io_port_entry_t *entry = &io_ports[(uint32_t)port & 0xFFFF];
    if (entry->read16 == NULL) return unregistered_read(entry->device, port);
    int32_t value = entry->read16(entry->device, (int32_t)((uint32_t)port & 0xFFFF));
    if (entry->read16 == empty_port_read16) {
        io_log_unclaimed_read(16, port, value);
    }
    DBG_ASSERT(value >= 0 && value < 0x10000);
    return value;
}

int32_t io_portmap_read32(int32_t port)
{
    io_port_entry_t *entry = &io_ports[(uint32_t)port & 0xFFFF];
    if (entry->read32 == NULL) return unregistered_read(entry->device, port);
    int32_t value = entry->read32(entry->device, (int32_t)((uint32_t)port & 0xFFFF));
    if (entry->read32 == empty_port_read32) {
        io_log_unclaimed_read(32, port, value);
    }
    return value;
}

void io_portmap_write8(int32_t port, int32_t value)
{
    io_port_entry_t *entry = &io_ports[(uint32_t)port & 0xFFFF];
    if (entry->write8 == NULL) {
        unregistered_write(entry->device, port, value);
        return;
    }
    if (entry->write8 == empty_port_write) {
        io_log_unclaimed_write(8, port, value);
    }
    entry->write8(entry->device, (int32_t)((uint32_t)port & 0xFFFF), value);
}

void io_portmap_write16(int32_t port, int32_t value)
{
    io_port_entry_t *entry = &io_ports[(uint32_t)port & 0xFFFF];
    if (entry->write16 == NULL) {
        unregistered_write(entry->device, port, value);
        return;
    }
    if (entry->write16 == empty_port_write) {
        io_log_unclaimed_write(16, port, value);
    }
    entry->write16(entry->device, (int32_t)((uint32_t)port & 0xFFFF), value);
}

void io_portmap_write32(int32_t port, int32_t value)
{
    io_port_entry_t *entry = &io_ports[(uint32_t)port & 0xFFFF];
    if (entry->write32 == NULL) {
        unregistered_write(entry->device, port, value);
        return;
    }
    if (entry->write32 == empty_port_write) {
        io_log_unclaimed_write(32, port, value);
    }
    entry->write32(entry->device, (int32_t)((uint32_t)port & 0xFFFF), value);
}

/* ------------------------------------------------------------------ *
 * the seabios port names -- io.js:396-452, used only to make the stops readable
 * ------------------------------------------------------------------ */
typedef struct {
    int32_t port;
    const char *name;
} io_port_name_t;

static const io_port_name_t io_port_names[] = {
    {0x0004, "PORT_DMA_ADDR_2"},        {0x0005, "PORT_DMA_CNT_2"},
    {0x000a, "PORT_DMA1_MASK_REG"},     {0x000b, "PORT_DMA1_MODE_REG"},
    {0x000c, "PORT_DMA1_CLEAR_FF_REG"}, {0x000d, "PORT_DMA1_MASTER_CLEAR"},
    {0x0020, "PORT_PIC1_CMD"},          {0x0021, "PORT_PIC1_DATA"},
    {0x0040, "PORT_PIT_COUNTER0"},      {0x0041, "PORT_PIT_COUNTER1"},
    {0x0042, "PORT_PIT_COUNTER2"},      {0x0043, "PORT_PIT_MODE"},
    {0x0060, "PORT_PS2_DATA"},          {0x0061, "PORT_PS2_CTRLB"},
    {0x0064, "PORT_PS2_STATUS"},        {0x0070, "PORT_CMOS_INDEX"},
    {0x0071, "PORT_CMOS_DATA"},         {0x0080, "PORT_DIAG"},
    {0x0081, "PORT_DMA_PAGE_2"},        {0x0092, "PORT_A20"},
    {0x00a0, "PORT_PIC2_CMD"},          {0x00a1, "PORT_PIC2_DATA"},
    {0x00b2, "PORT_SMI_CMD"},           {0x00b3, "PORT_SMI_STATUS"},
    {0x00d4, "PORT_DMA2_MASK_REG"},     {0x00d6, "PORT_DMA2_MODE_REG"},
    {0x00da, "PORT_DMA2_MASTER_CLEAR"}, {0x00f0, "PORT_MATH_CLEAR"},
    {0x0170, "PORT_ATA2_CMD_BASE"},     {0x01f0, "PORT_ATA1_CMD_BASE"},
    {0x0278, "PORT_LPT2"},              {0x02e8, "PORT_SERIAL4"},
    {0x02f8, "PORT_SERIAL2"},           {0x0374, "PORT_ATA2_CTRL_BASE"},
    {0x0378, "PORT_LPT1"},              {0x03e8, "PORT_SERIAL3"},
    {0x03f0, "PORT_FD_BASE"},           {0x03f2, "PORT_FD_DOR"},
    {0x03f4, "PORT_FD_STATUS"},         {0x03f5, "PORT_FD_DATA"},
    {0x03f6, "PORT_HD_DATA"},           {0x03f7, "PORT_FD_DIR"},
    {0x03f8, "PORT_SERIAL1"},           {0x0cf8, "PORT_PCI_CMD"},
    {0x0cf9, "PORT_PCI_REBOOT"},        {0x0cfc, "PORT_PCI_DATA"},
    {0x0402, "PORT_BIOS_DEBUG"},        {0x0510, "PORT_QEMU_CFG_CTL"},
    {0x0511, "PORT_QEMU_CFG_DATA"},     {0xb000, "PORT_ACPI_PM_BASE"},
    {0xb100, "PORT_SMB_BASE"},          {0x8900, "PORT_BIOS_APM"},
};

const char *io_get_port_description(int32_t addr)
{
    for (unsigned i = 0; i < sizeof(io_port_names) / sizeof(io_port_names[0]); i++) {
        if (io_port_names[i].port == addr) return io_port_names[i].name;
    }
    return "";
}

/* ------------------------------------------------------------------ *
 * cpu.rs:4613-4649 -- the port bridge.  The six PIC programming ports and the two ELCR
 * ports are intercepted IN FRONT of the device layer, exactly as upstream intercepts them
 * in front of `js::io_port_*`, and only the 8-bit PIC writes run the interrupt poll
 * afterwards (upstream's `handle_irqs()` sits in the 0x20|0x21|0xA0|0xA1|0x4D0|0x4D1 arm of
 * io_port_write8 alone).
 * ------------------------------------------------------------------ */
int32_t io_port_read8(int32_t port)
{
    switch (port) {
    case 0x20: return (int32_t)port20_read();
    case 0x21: return (int32_t)port21_read();
    case 0xA0: return (int32_t)portA0_read();
    case 0xA1: return (int32_t)portA1_read();
    case 0x4D0: return (int32_t)port4D0_read();
    case 0x4D1: return (int32_t)port4D1_read();
    default: return io_portmap_read8(port);
    }
}

int32_t io_port_read16(int32_t port) { return io_portmap_read16(port); }

int32_t io_port_read32(int32_t port) { return io_portmap_read32(port); }

void io_port_write8(int32_t port, int32_t value)
{
    switch (port) {
    case 0x20: port20_write((uint8_t)value); break;
    case 0x21: port21_write((uint8_t)value); break;
    case 0xA0: portA0_write((uint8_t)value); break;
    case 0xA1: portA1_write((uint8_t)value); break;
    case 0x4D0: port4D0_write((uint8_t)value); break;
    case 0x4D1: port4D1_write((uint8_t)value); break;
    default:
        io_portmap_write8(port, value);
        return;
    }
    handle_irqs();
}

void io_port_write16(int32_t port, int32_t value) { io_portmap_write16(port, value); }

void io_port_write32(int32_t port, int32_t value) { io_portmap_write32(port, value); }
