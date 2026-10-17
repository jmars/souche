/*
 * memory.c -- the guest RAM array, the read/write router and the device hooks.
 *
 * Port of v86/src/rust/cpu/memory.rs:1-352, minus the JIT dirty callbacks
 * (`jit::jit_dirty_page` / `jit_dirty_cache_small`, memory.rs:186-203) which only
 * invalidate JIT code and are meaningless in an interpreter-only port (plan ss3).
 * Every such call site keeps the same control flow, with the callback dropped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "instructions.h" /* handle_irqs -- the APIC/IOAPIC write arms (memory.rs:284-289) */
#include "uop_cache.h" /* PERF-4/M2: the write-path invalidation hook (plan ss4.1) */

uint8_t *mem8 = NULL;
static uint32_t mem8_size = 0;

/* ------------------------------------------------------------------ *
 * allocation -- memory.rs:24-43
 * ------------------------------------------------------------------ */

bool allocate_memory(uint32_t size)
{
    DBG_ASSERT(mem8 == NULL);

    /* Upstream allocates exactly `size` in a wasm linear memory, where the +4-byte
     * overreads of read32_no_mmap_check stay inside the (larger) allocation.  In C
     * they would run past the end of a malloc'd block, so: round to 4 KiB and add
     * 16 bytes of slack (plan ss3, risk R7).  Zeroed, because wasm linear memory
     * starts zeroed and the reference's mem8 therefore reads 0 before anything
     * writes it (cpu.js reset_memory's fill(0) is the belt to that brace). */
    uint64_t rounded = ((uint64_t)size + 0xFFF) & ~0xFFFull;
    size_t alloc_size = (size_t)(rounded + 16);
    uint8_t *p = calloc(1, alloc_size);
    if (p == NULL) {
        return false;
    }
    mem8 = p;
    mem8_size = (uint32_t)alloc_size;
    return true;
}

uint32_t mem8_alloc_size(void) { return mem8_size; }

void zero_memory(uint32_t addr, uint32_t size)
{
    memset(mem8 + addr, 0, size);
}

bool is_memory_zeroed(uint32_t addr, uint32_t length)
{
    DBG_ASSERT(addr % 8 == 0);
    DBG_ASSERT(length % 8 == 0);
    for (uint32_t i = addr; i < addr + length; i += 8) {
        if (*(const int64_t *)(mem8 + i) != 0) {
            return false;
        }
    }
    return true;
}

/* cpu.js:299-311 -- added by M2-1 (see memory.h).
 *
 * AUDIT-1 S2: upstream's two dbg_asserts on the target are compiled out of the pinned
 * release, and `mem8.set(blob, offset)` then THROWS a RangeError for a target that leaves
 * the wasm view (MEASURED in node: `new Uint8Array(16).set(new Uint8Array(4), 20)` is a
 * RangeError) -- the reference dies on this input.  This port's mem8 is a fixed C array
 * and write_blob is on GUEST-reachable paths (a virtio descriptor address through
 * virtq_chain_set_next_blob, a DMA address through dma_do_read), so the unbounded write
 * would run off the guest's RAM into host memory.  The kernel loader's write_blob
 * (kernel.c:56) is the tree's established shape for this exact operation: drop what falls
 * outside guest memory and say so on stderr -- a host artifact, like V86C_LOG_PORTS; no
 * guest-readable value depends on it.  The bound is memory_size (the reference's own view
 * length), not in_mapped_range: a descriptor aimed at the VGA hole writes the same raw
 * bytes the release's wasm view would. */
void write_blob(const uint8_t *data, uint32_t length, uint32_t offset)
{
    /* upstream's `dbg_assert(blob && blob.length >= 0)` -- the second conjunct cannot fail
     * for an unsigned length, so it is not reproduced (a check that cannot fail is not a
     * check). */
    DBG_ASSERT(data != NULL);

    if (length == 0) return;

    if (offset >= cpu->memory_size) {
        fprintf(stderr, "v86c: write_blob: dropping %u bytes at 0x%x: outside guest memory"
                        " (0x%x)\n",
                (unsigned)length, (unsigned)offset, (unsigned)cpu->memory_size);
        return;
    }
    if ((uint64_t)offset + length > cpu->memory_size) {
        uint32_t kept = cpu->memory_size - offset;
        fprintf(stderr, "v86c: write_blob: dropping %u of %u bytes at 0x%x: outside guest"
                        " memory (0x%x)\n",
                (unsigned)(length - kept), (unsigned)length, (unsigned)offset,
                (unsigned)cpu->memory_size);
        length = kept;
    }

    /* upstream: this.jit_dirty_cache(offset, offset + blob.length) -- restored as the
     * range invalidation (plan ss4.1's last row).  write_blob is the LOADER's path
     * (src/main.c) AND the guest-reachable DMA/IDE/virtio sinks (dma_do_read,
     * ide.c's bus-master DMA, virtq_chain_set_next_blob -- the S2 paths above), so it
     * runs at guest time too; it is the one site that can write across a page and
     * therefore the one that needs the range form.  (N4, AUDIT-1R: the old sentence
     * said "loader-time only", contradicting the S2 comment above.) */
    uop_invalidate_range(offset, length);
    memcpy(mem8 + offset, data, length);
}

/* ------------------------------------------------------------------ *
 * VGA / APIC / IOAPIC (plan ss3: the VGA is still deferred; the APIC and the
 * IOAPIC are unit APIC-1's -- apic.c / ioapic.c define the four functions this
 * file's routing arms call, with the same signatures memory.h declares)
 * ------------------------------------------------------------------ */

uint8_t *vga_mem8 = NULL;
uint32_t vga_memory_size = 0;

/* The RGBA dest buffer the fill writes (vga.rs's dest_buffer global): the POINTER lives
 * here so memory.c is self-contained for the cpu-level selftests, and vga.c points it at
 * its own vga_dest_bytes storage when set_size_graphical sizes the buffer.  NULL / 0 until
 * then -- svga_fill_pixel_buffer no-ops on NULL. */
uint8_t *svga_dest_bytes = NULL;
uint32_t svga_dest_pixels = 0;

/* The port's fixed LFB size (vga.js:216-228's default 8 MiB, set by vga_init).  The
 * reference clamps vga_memory_size to [256 KiB, 256 MiB]; this port fixes it at the default
 * because vga_init sets it and nothing writes it -- and the dirty bitmap's static size below
 * depends on that being a compile-time constant. */
#define VGA_DEFAULT_VRAM_SIZE 0x800000u

bool in_svga_lfb(uint32_t addr)
{
    /* memory.rs:57-59, arithmetic kept in u32 exactly as upstream: with
     * vga_memory_size == 0 the bound wraps to 0xDFFFFFFF and the test is false. */
    return addr >= VGA_LFB_ADDRESS && addr <= VGA_LFB_ADDRESS + (vga_memory_size - 1);
}

/* ------------------------------------------------------------------ *
 * the SVGA dirty bitmap -- v86/src/rust/cpu/vga.rs:1-135 (VGA-U4)
 *
 * The reference keeps a Vec<u64> with ONE BIT PER 4 KiB PAGE of the LFB: mark_dirty sets a
 * page's bit on every LFB write, svga_mark_dirty sets them ALL (a complete redraw or a
 * dest-buffer realloc), and svga_fill_pixel_buffer converts each dirty page's LFB pixels to
 * RGBA in the dest buffer and then clears the whole bitmap.  This port's LFB is a FIXED
 * 8 MiB (VGA_DEFAULT_VRAM_SIZE, set by vga_init), so the bitmap is 8 MiB / 4 KiB / 64 = 32
 * u64 words -- a static, like every other device buffer.
 *
 * OUTPUT INVARIANCE (the brief's hardest requirement): the fill skips CLEAN pages and the
 * dest buffer is ZEROED at allocation (vga.c's set_size_graphical, the reference's fresh
 * wasm region) with svga_mark_dirty called in the same block -- so the FIRST fill at the
 * document stop covers every page, and a skipped recompute of a never-written pixel reads
 * the same zero it would from a full recompute.  The bitmap is therefore invisible to the
 * differential: it exists to let a later unit skip work, never to change a document byte.
 * ------------------------------------------------------------------ */
#define SVGA_DIRTY_PAGE_BITS 64
#define SVGA_DIRTY_PAGES (VGA_DEFAULT_VRAM_SIZE >> 12)
#define SVGA_DIRTY_WORDS (SVGA_DIRTY_PAGES / SVGA_DIRTY_PAGE_BITS)

static uint64_t svga_dirty_bitmap[SVGA_DIRTY_WORDS];

void vga_mark_dirty(uint32_t addr)
{
    /* vga.rs:22-27.  The callers (the LFB write arms and the banked-window write) have all
     * checked in_svga_lfb(addr) first, so page is in [0, 2048); the bound is the R4 rule --
     * never a guest-reachable OOB static write -- not a value the reference observes. */
    uint32_t page = (addr - VGA_LFB_ADDRESS) >> 12;
    if (page < SVGA_DIRTY_PAGES) {
        svga_dirty_bitmap[page >> 6] |= 1ull << (page & 63);
    }
}

void svga_mark_dirty(void)
{
    for (uint32_t i = 0; i < SVGA_DIRTY_WORDS; i++) svga_dirty_bitmap[i] = ~0ull;
}

static void svga_fill_clear_bitmap(void)
{
    for (uint32_t i = 0; i < SVGA_DIRTY_WORDS; i++) svga_dirty_bitmap[i] = 0;
}

/* vga.rs:64-135.  Each dirty page's LFB pixels -> RGBA dwords in the dest buffer.  The
 * reference's dbg_asserts are compiled out of the pinned release; the port keeps the exact
 * arithmetic and bounds every read against the +16-byte slack of vga_vram (vga.c) and every
 * write against svga_dest_pixels -- a guest can reach this through a dispi enable + LFB
 * write, so nothing here may index past a static (the last unit's review found four of that
 * class).  The 32-bit expression is the reference's own byte-lane shuffle (a 32bpp LFB
 * dword 0xXRGB becomes 0xFFBBGGRR), NOT a "corrected" read. */
void svga_fill_pixel_buffer(uint32_t bpp, uint32_t svga_offset)
{
    uint8_t *dest = svga_dest_bytes;
    uint32_t dest_len = svga_dest_pixels;

    if (dest == NULL) {
        /* Unreachable in the shipped machine (set_size_graphical sizes the buffer before
         * the fill can run), but a clean no-op beats a NULL dereference. */
        svga_fill_clear_bitmap();
        return;
    }

    switch (bpp) {
    case 32: {
        for (uint32_t w = 0; w < SVGA_DIRTY_WORDS; w++) {
            uint64_t word = svga_dirty_bitmap[w];
            if (word == 0) continue;
            for (int j = 0; j < SVGA_DIRTY_PAGE_BITS; j++) {
                if (!(word & (1ull << j))) continue;
                uint32_t off = ((w << 6) | (uint32_t)j) << 12;
                int64_t dest_offset = (int64_t)(off / 4) - (int64_t)svga_offset;
                int64_t end = dest_offset < 0 ? 0
                                              : (int64_t)dest_len - dest_offset < 1024
                                                    ? (int64_t)dest_len - dest_offset
                                                    : 1024;
                for (int64_t i = 0; i < end; i++) {
                    uint32_t dword;
                    memcpy(&dword, vga_mem8 + off + 4 * i, sizeof(dword));
                    uint32_t px = (dword << 16) | ((dword >> 16) & 0xFF) |
                                  (dword & 0xFF00) | 0xFF000000u;
                    memcpy(dest + 4 * (dest_offset + i), &px, sizeof(px));
                }
            }
        }
        break;
    }
    case 24: {
        for (uint32_t w = 0; w < SVGA_DIRTY_WORDS; w++) {
            uint64_t word = svga_dirty_bitmap[w];
            if (word == 0) continue;
            for (int j = 0; j < SVGA_DIRTY_PAGE_BITS; j++) {
                if (!(word & (1ull << j))) continue;
                /* vga.rs:75: align the page down to a 3-byte pixel boundary -- the page's
                 * fill then re-reads the boundary pixel, idempotently overlapping the
                 * previous page's last pixel (harmless: same source bytes). */
                uint32_t page_off = ((w << 6) | (uint32_t)j) << 12;
                uint32_t off = page_off - page_off % 3;
                int64_t dest_offset = (int64_t)(off / 3) - (int64_t)svga_offset;
                int64_t end = dest_offset < 0 ? 0
                                              : (int64_t)dest_len - dest_offset < 4096 / 3 + 1
                                                    ? (int64_t)dest_len - dest_offset
                                                    : 4096 / 3 + 1;
                for (int64_t i = 0; i < end; i++) {
                    uint32_t dword;
                    /* A 4-byte unaligned read at the last page's last pixel runs 2 bytes
                     * into vga_vram's +16-byte slack (zeroed) -- the slack's purpose. */
                    memcpy(&dword, vga_mem8 + off + 3 * i, sizeof(dword));
                    uint32_t px = (dword << 16) | ((dword >> 16) & 0xFF) |
                                  (dword & 0xFF00) | 0xFF000000u;
                    memcpy(dest + 4 * (dest_offset + i), &px, sizeof(px));
                }
            }
        }
        break;
    }
    case 16:
    case 15: {
        bool is_15 = bpp == 15;
        for (uint32_t w = 0; w < SVGA_DIRTY_WORDS; w++) {
            uint64_t word = svga_dirty_bitmap[w];
            if (word == 0) continue;
            for (int j = 0; j < SVGA_DIRTY_PAGE_BITS; j++) {
                if (!(word & (1ull << j))) continue;
                uint32_t off = ((w << 6) | (uint32_t)j) << 12;
                int64_t dest_offset = (int64_t)(off / 2) - (int64_t)svga_offset;
                int64_t end = dest_offset < 0 ? 0
                                              : (int64_t)dest_len - dest_offset < 2048
                                                    ? (int64_t)dest_len - dest_offset
                                                    : 2048;
                for (int64_t i = 0; i < end; i++) {
                    uint16_t word16;
                    memcpy(&word16, vga_mem8 + off + 2 * i, sizeof(word16));
                    /* vga.rs:100-111 (16bpp 5-6-5) / 116-127 (15bpp 5-5-5): integer scale
                     * `v * 0xFF / max`, truncating -- the reference's own expansion. */
                    uint32_t r = (uint32_t)(word16 & 0x1F) * 0xFF / 0x1F;
                    uint32_t g = is_15
                                     ? (uint32_t)((word16 >> 5) & 0x1F) * 0xFF / 0x1F
                                     : (uint32_t)((word16 >> 5) & 0x3F) * 0xFF / 0x3F;
                    uint32_t b = is_15
                                     ? (uint32_t)((word16 >> 10) & 0x1F) * 0xFF / 0x1F
                                     : (uint32_t)(word16 >> 11) * 0xFF / 0x1F;
                    uint32_t px = r << 16 | g << 8 | b | 0xFF000000u;
                    memcpy(dest + 4 * (dest_offset + i), &px, sizeof(px));
                }
            }
        }
        break;
    }
    default:
        /* bpp 4 and 8 never reach here (vga.c runs the bpp-8 palette loop; bpp 4 is
         * dbg_assert'd out in the release reference, which then no-ops the fill). */
        break;
    }

    svga_fill_clear_bitmap();
}

/* ------------------------------------------------------------------ *
 * device hooks -- the C replacement for the mmap_* JS imports
 * ------------------------------------------------------------------ */

/* The two ranges the reference's always-present VGA claims: the 0xA0000-0xBFFFF
 * window and the LFB.  Both hold a zeroed buffer at boot -- MEASURED against the
 * pinned npm reference through the same read8/read16/read32s entry points:
 * read32 [0xB8000] = 0, read32 [0xE0000000] = 0, read32 [0xE07FFFFF] = 0.  Every
 * OTHER address at or above memory_size is unclaimed and takes the io.js:40-61
 * fallback below; conflating the two would make the C leg read 0xFF at 0xB8000
 * where the reference reads 0. */
static bool device_default_is_vga_blob(uint32_t addr)
{
    /* unsigned wraparound makes the LFB test safe for every addr < VGA_LFB_ADDRESS */
    return (addr >= 0xA0000 && addr < 0xC0000) ||
           (addr - VGA_LFB_ADDRESS) < VGA_DEFAULT_VRAM_SIZE;
}

static int32_t device_read_default8(uint32_t addr)
{
    /* io.js:40-61: an unclaimed address reads 0xFF; the VGA blob reads 0. */
    return device_default_is_vga_blob(addr) ? 0 : 0xFF;
}
static int32_t device_read_default32(uint32_t addr)
{
    /* io.js:40-61: an unclaimed address reads -1; the VGA blob reads 0. */
    return device_default_is_vga_blob(addr) ? 0 : -1;
}
static void device_write_default8(uint32_t addr, int32_t value)
{
    (void)addr;
    (void)value;
}
static void device_write_default16(uint32_t addr, int32_t value)
{
    (void)addr;
    (void)value;
}
static void device_write_default32(uint32_t addr, int32_t value)
{
    (void)addr;
    (void)value;
}
static void device_write_default64(uint32_t addr, uint64_t value)
{
    (void)addr;
    (void)value;
}
static void device_write_default128(uint32_t addr, uint64_t v0, uint64_t v1)
{
    (void)addr;
    (void)v0;
    (void)v1;
}

memory_device_hooks_t memory_devices = {
    .read8 = device_read_default8,
    .read32 = device_read_default32,
    .write8 = device_write_default8,
    .write16 = device_write_default16,
    .write32 = device_write_default32,
    .write64 = device_write_default64,
    .write128 = device_write_default128,
};

const memory_device_hooks_t *memory_default_device_hooks(void)
{
    static const memory_device_hooks_t t = {
        .read8 = device_read_default8,
        .read32 = device_read_default32,
        .write8 = device_write_default8,
        .write16 = device_write_default16,
        .write32 = device_write_default32,
        .write64 = device_write_default64,
        .write128 = device_write_default128,
    };
    return &t;
}

void memory_set_device_hooks(const memory_device_hooks_t *hooks)
{
    memory_devices = hooks == NULL ? *memory_default_device_hooks() : *hooks;
}

/* ------------------------------------------------------------------ *
 * reads -- memory.rs:103-170
 * ------------------------------------------------------------------ */

int32_t read8(uint32_t addr)
{
    if (in_mapped_range(addr)) {
        if (in_svga_lfb(addr)) {
            return (int32_t)vga_mem8[addr - VGA_LFB_ADDRESS];
        }
        else if (addr >= APIC_MEM_ADDRESS && addr < APIC_MEM_ADDRESS + APIC_MEM_SIZE) {
            return (int32_t)apic_read32((addr - APIC_MEM_ADDRESS) & ~3u) >> (8 * (addr & 3)) & 0xFF;
        }
        else if (addr >= IOAPIC_MEM_ADDRESS && addr < IOAPIC_MEM_ADDRESS + IOAPIC_MEM_SIZE) {
            return (int32_t)ioapic_read32((addr - IOAPIC_MEM_ADDRESS) & ~3u) >> (8 * (addr & 3)) &
                   0xFF;
        }
        else {
            return memory_devices.read8(addr);
        }
    }
    else {
        return read8_no_mmap_check(addr);
    }
}

int32_t read8_no_mmap_check(uint32_t addr) { return (int32_t)mem8[addr]; }

int32_t read16(uint32_t addr)
{
    if (in_mapped_range(addr)) {
        if (in_svga_lfb(addr)) {
            uint16_t v;
            memcpy(&v, vga_mem8 + (addr - VGA_LFB_ADDRESS), sizeof(v));
            return (int32_t)v;
        }
        else {
            return read8(addr) | read8(addr + 1) << 8;
        }
    }
    else {
        return read16_no_mmap_check(addr);
    }
}

int32_t read16_no_mmap_check(uint32_t addr)
{
    uint16_t v;
    memcpy(&v, mem8 + addr, sizeof(v)); /* upstream ptr::read_unaligned */
    return (int32_t)v;
}

int32_t read32s(uint32_t addr)
{
    if (in_mapped_range(addr)) {
        if (in_svga_lfb(addr)) {
            int32_t v;
            memcpy(&v, vga_mem8 + (addr - VGA_LFB_ADDRESS), sizeof(v));
            return v;
        }
        else if (addr >= APIC_MEM_ADDRESS && addr < APIC_MEM_ADDRESS + APIC_MEM_SIZE) {
            return apic_read32(addr - APIC_MEM_ADDRESS);
        }
        else if (addr >= IOAPIC_MEM_ADDRESS && addr < IOAPIC_MEM_ADDRESS + IOAPIC_MEM_SIZE) {
            return ioapic_read32(addr - IOAPIC_MEM_ADDRESS);
        }
        else {
            return memory_devices.read32(addr);
        }
    }
    else {
        return read32_no_mmap_check(addr);
    }
}

int32_t read32_no_mmap_check(uint32_t addr)
{
    int32_t v;
    memcpy(&v, mem8 + addr, sizeof(v)); /* unconditional 4-byte load: mem8's +16B slack */
    return v;
}

int64_t read64s(uint32_t addr)
{
    if (in_mapped_range(addr)) {
        if (in_svga_lfb(addr)) {
            int64_t v;
            memcpy(&v, vga_mem8 + (addr - VGA_LFB_ADDRESS), sizeof(v));
            return v;
        }
        else {
            /* UPSTREAM QUIRK, reproduced deliberately (memory.rs:158-161): the two
             * halves are sign-extended i32s, so a low word with bit 31 set ORs
             * 0xFFFFFFFF over the high word.  Upstream has it only on this
             * non-RAM (device) path -- the RAM path below is a real 64-bit load --
             * and matching it costs nothing while a "cleanup" would silently
             * diverge. */
            return (int64_t)read32s(addr) | ((int64_t)read32s(addr + 4) << 32);
        }
    }
    else {
        int64_t v;
        memcpy(&v, mem8 + addr, sizeof(v));
        return v;
    }
}

reg128 read128(uint32_t addr)
{
    reg128 v;
    if (in_mapped_range(addr)) {
        if (in_svga_lfb(addr)) {
            memcpy(&v, vga_mem8 + (addr - VGA_LFB_ADDRESS), sizeof(v));
            return v;
        }
        else {
            v.i32[0] = read32s(addr + 0);
            v.i32[1] = read32s(addr + 4);
            v.i32[2] = read32s(addr + 8);
            v.i32[3] = read32s(addr + 12);
            return v;
        }
    }
    else {
        memcpy(&v, mem8 + addr, sizeof(v));
        return v;
    }
}

/* ------------------------------------------------------------------ *
 * writes -- memory.rs:172-230
 * ------------------------------------------------------------------ */

void write8(uint32_t addr, int32_t value)
{
    if (in_mapped_range(addr)) {
        mmap_write8(addr, value & 0xFF);
    }
    else {
        /* jit::jit_dirty_page(Page::page_of(addr)) -- upstream's JIT-only call, RESTORED as
         * the uop cache's invalidation (PERF-4/M2, plan ss4.1 row 1).  A write into a page
         * holding decoded blocks makes them stale code; the cache-off build is unaffected
         * because the cache's tables are empty (uop_invalidate_page_fast is a load of a
         * zero byte and a branch). */
        uop_invalidate_page_fast(addr);
        write8_no_mmap_or_dirty_check(addr, value);
    }
}

void write8_no_mmap_or_dirty_check(uint32_t addr, int32_t value) { mem8[addr] = (uint8_t)value; }

void write16(uint32_t addr, int32_t value)
{
    if (in_mapped_range(addr)) {
        mmap_write16(addr, value & 0xFFFF);
    }
    else {
        /* jit::jit_dirty_cache_small(addr, addr + 2) -- restored (plan ss4.1 row 2), and the
         * write CAN straddle here: the routers have no boundary split (only safe_write16 and
         * the SAFE_READ_WRITE16 macro route a straddle through virt_boundary_write16), so a
         * write at 0xFFF memcpy's the next physical page's first byte and that page's cached
         * blocks would survive it.  Upstream spans both pages (memory.rs:204-232), so hook
         * the last byte's page too when the write crosses (PERF-FIX-3/SHOULD-FIX-1).  `addr`
         * is < memory_size here (in_mapped_range excluded the rest), so addr + 1 cannot wrap. */
        uop_invalidate_page_fast(addr);
        if ((addr & 0xFFFu) == 0xFFFu) uop_invalidate_page_fast(addr + 1u);
        write16_no_mmap_or_dirty_check(addr, value);
    }
}

void write16_no_mmap_or_dirty_check(uint32_t addr, int32_t value)
{
    uint16_t v = (uint16_t)value;
    memcpy(mem8 + addr, &v, sizeof(v));
}

void write32(uint32_t addr, int32_t value)
{
    if (in_mapped_range(addr)) {
        mmap_write32(addr, value);
    }
    else {
        /* jit::jit_dirty_cache_small(addr, addr + 4) -- restored (plan ss4.1 row 3), with the
         * same straddle argument as write16: a start at 0xFFD-0xFFF crosses the page, and the
         * "cannot straddle" claim was true of virt_boundary_write32, not of this router
         * (PERF-FIX-3/SHOULD-FIX-1).  `addr` is < memory_size, so addr + 3 cannot wrap. */
        uop_invalidate_page_fast(addr);
        if ((addr & 0xFFFu) >= 0xFFDu) uop_invalidate_page_fast(addr + 3u);
        write32_no_mmap_or_dirty_check(addr, value);
    }
}

void write32_no_mmap_or_dirty_check(uint32_t addr, int32_t value)
{
    memcpy(mem8 + addr, &value, sizeof(value));
}

void write64_no_mmap_or_dirty_check(uint32_t addr, uint64_t value)
{
    memcpy(mem8 + addr, &value, sizeof(value));
}

void write128_no_mmap_or_dirty_check(uint32_t addr, reg128 value)
{
    memcpy(mem8 + addr, &value, sizeof(value));
}

void memset_no_mmap_or_dirty_check(uint32_t addr, uint8_t value, uint32_t count)
{
    memset(mem8 + addr, value, count);
}

void memcpy_no_mmap_or_dirty_check(uint32_t src_addr, uint32_t dst_addr, uint32_t count)
{
    DBG_ASSERT(src_addr < cpu->memory_size);
    DBG_ASSERT(dst_addr < cpu->memory_size);
    memmove(mem8 + dst_addr, mem8 + src_addr, count); /* upstream ptr::copy, overlap-safe */
}

void memcpy_into_svga_lfb(uint32_t src_addr, uint32_t dst_addr, uint32_t count)
{
    /* memory.rs:254-264.  Added by M1a-3: M1a-1 left it out because nothing in its
     * files called it, and string.c's movs fast path is its first caller.  The VGA
     * device itself is M2, so vga_mem8 is NULL and in_svga_lfb false for every address
     * until M2 allocates it -- the function is still the faithful port of the copy into
     * the LFB with the single dirty mark. */
    DBG_ASSERT(src_addr < cpu->memory_size);
    DBG_ASSERT(in_svga_lfb(dst_addr));
    DBG_ASSERT((dst_addr >> 12) == ((dst_addr + count - 1) >> 12)); /* Page::page_of */
    vga_mark_dirty(dst_addr);
    memcpy(vga_mem8 + (dst_addr - VGA_LFB_ADDRESS), mem8 + src_addr, count);
}

/* ------------------------------------------------------------------ *
 * the mmap write path -- memory.rs:232-320
 * ------------------------------------------------------------------ */

void mmap_write8(uint32_t addr, int32_t value)
{
    if (in_svga_lfb(addr)) {
        vga_mark_dirty(addr);
        vga_mem8[addr - VGA_LFB_ADDRESS] = (uint8_t)value;
    }
    else {
        memory_devices.write8(addr, value);
    }
}

void mmap_write16(uint32_t addr, int32_t value)
{
    if (in_svga_lfb(addr)) {
        uint16_t v = (uint16_t)value;
        vga_mark_dirty(addr);
        memcpy(vga_mem8 + (addr - VGA_LFB_ADDRESS), &v, sizeof(v));
    }
    else {
        memory_devices.write16(addr, value);
    }
}

void mmap_write32(uint32_t addr, int32_t value)
{
    if (in_svga_lfb(addr)) {
        vga_mark_dirty(addr);
        memcpy(vga_mem8 + (addr - VGA_LFB_ADDRESS), &value, sizeof(value));
    }
    else if (addr >= APIC_MEM_ADDRESS && addr < APIC_MEM_ADDRESS + APIC_MEM_SIZE) {
        apic_write32(addr - APIC_MEM_ADDRESS, (uint32_t)value);
        handle_irqs();
    }
    else if (addr >= IOAPIC_MEM_ADDRESS && addr < IOAPIC_MEM_ADDRESS + IOAPIC_MEM_SIZE) {
        ioapic_write32(addr - IOAPIC_MEM_ADDRESS, (uint32_t)value);
        {
            static int enabled = -1;
            if (enabled < 0) {
                const char *v = getenv("V86C_TRACE_ACPI");
                enabled = v != NULL && v[0] != '\0' && v[0] != '0';
            }
            if (enabled) {
                fprintf(stderr, "trace: mmio ioapic write32 addr %08x <- %08x\n",
                        (unsigned)addr, (unsigned)value);
            }
        }
        handle_irqs();
    }
    else {
        memory_devices.write32(addr, value);
    }
}

void mmap_write64(uint32_t addr, uint64_t value)
{
    if (in_svga_lfb(addr)) {
        vga_mark_dirty(addr);
        memcpy(vga_mem8 + (addr - VGA_LFB_ADDRESS), &value, sizeof(value));
    }
    else {
        memory_devices.write64(addr, value);
    }
}

void mmap_write128(uint32_t addr, uint64_t v0, uint64_t v1)
{
    if (in_svga_lfb(addr)) {
        vga_mark_dirty(addr);
        memcpy(vga_mem8 + (addr - VGA_LFB_ADDRESS), &v0, sizeof(v0));
        memcpy(vga_mem8 + (addr - VGA_LFB_ADDRESS + 8), &v1, sizeof(v1));
    }
    else {
        memory_devices.write128(addr, v0, v1);
    }
}
