/*
 * memory.h -- guest RAM, the read/write router and the device hook table.
 *
 * Port of v86/src/rust/cpu/memory.rs:1-352.  Upstream's router sends every access
 * that is NOT plain RAM to a JS wasm import (`mmap_read8/32`, `mmap_write8/16/32/
 * 64/128`, memory.rs:1-13) plus three in-wasm device arms (VGA LFB, APIC, IOAPIC).
 * The C port replaces the JS imports with `memory_devices` (below): one hook table,
 * installed by the device layer, defaulting to the no-device table.
 *
 * The plan's C-specific care item (ss3, R7): read32_no_mmap_check does an
 * unconditional 4-byte load at an arbitrary address, so mem8 is allocated with
 * 16 bytes of slack past memory_size -- in wasm that read lands inside the linear
 * memory, in C it is a real segfault.
 */

#ifndef V86C_MEMORY_H
#define V86C_MEMORY_H

#include "cpu_state.h"

extern uint8_t *mem8;

/* memory.rs:24-38 (allocate_memory) + the plan's +16-byte slack.  `size` is the
 * guest RAM size (cpu.js create_memory rounds to MMAP_BLOCK_SIZE before calling);
 * the allocation is rounded up to 4 KiB and given 16 bytes of tail slack.  Returns
 * false if the allocation failed. */
bool allocate_memory(uint32_t size);

/* The size actually allocated for mem8: memory_size rounded up to 4 KiB plus the
 * 16-byte tail slack.  Anything that indexes mem8 near memory_size needs this bound
 * (read32_no_mmap_check does an unconditional 4-byte load), so it is readable rather
 * than re-derived -- and the selftest pins it, which is what makes the slack a testable
 * invariant instead of a comment (plan ss3, risk R7). */
uint32_t mem8_alloc_size(void);

/* memory.rs:40-43 */
void zero_memory(uint32_t addr, uint32_t size);
/* memory.rs:322-332 */
bool is_memory_zeroed(uint32_t addr, uint32_t length);

/* cpu.js:299-311 (CPU.prototype.write_blob), minus the jit_dirty_cache call (JIT-only).
 * ADDED BY M2-1: the DMA controller's do_read lands a transfer's bytes in guest memory
 * through it (dma.js:297's `cpu.write_blob(data, addr)`), and the IDE/virtio units will
 * want the same helper rather than a second copy.  SINCE AUDIT-1 S2 the target is
 * BOUNDED (kernel.c's loader shape): what falls outside guest memory is dropped and said
 * on stderr -- upstream's release dies on the same input (mem8.set throws). */
void write_blob(const uint8_t *data, uint32_t length, uint32_t offset);

/*
 * The device hooks: the C stand-in for v86's `mmap_*` JS imports (memory.rs:1-13).
 * The APIC/IOAPIC arms (cpu.rs's APIC_MEM_ADDRESS / IOAPIC_MEM_ADDRESS) keep their
 * address constants and route here too, since both devices are M2 (plan ss3).
 *
 * The default table is the machine the M0 oracle runs with no device layer
 * installed (no APIC, no IOAPIC).  An address no device claimed reads the
 * io.js:40-61 unclaimed-memory fallback -- 0xFF (read8) and -1 (read32), which the
 * reference registers for the whole range [memory_size, MMAP_MAX) in the IO
 * constructor regardless of devices -- and writes are dropped.  MEASURED against
 * the pinned npm reference (2 MiB, oracle config): read8[0x200000] = 0xFF,
 * read16 = 0xFFFF, read32 = 0xFFFFFFFF.  The two ranges the reference's
 * always-present VGA claims -- the 0xA0000-0xBFFFF window and the 8 MiB LFB --
 * read the zeroed VGA buffer at boot (browser/starter.js:234) and so stay 0 until
 * M2 installs a real VGA.
 */
typedef struct memory_device_hooks {
    int32_t (*read8)(uint32_t addr);
    int32_t (*read32)(uint32_t addr);
    void (*write8)(uint32_t addr, int32_t value);
    void (*write16)(uint32_t addr, int32_t value);
    void (*write32)(uint32_t addr, int32_t value);
    void (*write64)(uint32_t addr, uint64_t value);
    void (*write128)(uint32_t addr, uint64_t v0, uint64_t v1);
} memory_device_hooks_t;

extern memory_device_hooks_t memory_devices;
const memory_device_hooks_t *memory_default_device_hooks(void);
void memory_set_device_hooks(const memory_device_hooks_t *hooks);

/* VGA LFB state (memory.rs:50-56).  vga_mem8 is NULL and vga_memory_size 0 until a
 * VGA device exists (M2); in_svga_lfb is then false for every address. */
extern uint8_t *vga_mem8;
extern uint32_t vga_memory_size;
#define VGA_LFB_ADDRESS 0xE0000000u
bool in_svga_lfb(uint32_t addr);
void vga_mark_dirty(uint32_t addr); /* vga::mark_dirty: dirty-bitmap callback, stubbed */

/* APIC / IOAPIC stubs (apic.rs, ioapic.rs are M2 -- plan ss3, ss7) */
int32_t apic_read32(uint32_t addr);
void apic_write32(uint32_t addr, uint32_t value);
int32_t ioapic_read32(uint32_t addr);
void ioapic_write32(uint32_t addr, uint32_t value);

/* memory.rs:64-66 */
static inline bool in_mapped_range(uint32_t addr)
{
    return (addr >= 0xA0000 && addr < 0xC0000) || addr >= cpu->memory_size;
}

/* the router -- memory.rs:103-352 */
int32_t read8(uint32_t addr);
int32_t read16(uint32_t addr);
int32_t read32s(uint32_t addr);
int64_t read64s(uint32_t addr);
reg128 read128(uint32_t addr);

int32_t read8_no_mmap_check(uint32_t addr);
int32_t read16_no_mmap_check(uint32_t addr);
int32_t read32_no_mmap_check(uint32_t addr);

void write8(uint32_t addr, int32_t value);
void write16(uint32_t addr, int32_t value);
void write32(uint32_t addr, int32_t value);

void write8_no_mmap_or_dirty_check(uint32_t addr, int32_t value);
void write16_no_mmap_or_dirty_check(uint32_t addr, int32_t value);
void write32_no_mmap_or_dirty_check(uint32_t addr, int32_t value);
void write64_no_mmap_or_dirty_check(uint32_t addr, uint64_t value);
void write128_no_mmap_or_dirty_check(uint32_t addr, reg128 value);
void memset_no_mmap_or_dirty_check(uint32_t addr, uint8_t value, uint32_t count);
void memcpy_no_mmap_or_dirty_check(uint32_t src_addr, uint32_t dst_addr, uint32_t count);
/* memory.rs:254-264 -- added by M1a-3 (string.c's movs fast path is its first caller) */
void memcpy_into_svga_lfb(uint32_t src_addr, uint32_t dst_addr, uint32_t count);

void mmap_write8(uint32_t addr, int32_t value);
void mmap_write16(uint32_t addr, int32_t value);
void mmap_write32(uint32_t addr, int32_t value);
void mmap_write64(uint32_t addr, uint64_t value);
void mmap_write128(uint32_t addr, uint64_t v0, uint64_t v1);

#endif /* V86C_MEMORY_H */
