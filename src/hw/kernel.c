/*
 * kernel.c -- the Linux bzImage boot-protocol loader.  See kernel.h for the geometry, for
 * what the option ROM is and how it reaches the kernel, and for the one declared
 * divergence (upstream's compiled-out dbg_asserts become stops).
 *
 * Every function cites the kernel.js line range it transcribes.
 */

#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu/cpu_state.h" /* DBG_ASSERT */

/* kernel.js:7-39 -- the boot-header field offsets and the two magic values. */
#define LINUX_BOOT_HDR_SETUP_SECTS 0x1F1
#define LINUX_BOOT_HDR_SYSSIZE 0x1F4
#define LINUX_BOOT_HDR_VIDMODE 0x1FA
#define LINUX_BOOT_HDR_BOOT_FLAG 0x1FE
#define LINUX_BOOT_HDR_HEADER 0x202
#define LINUX_BOOT_HDR_VERSION 0x206
#define LINUX_BOOT_HDR_TYPE_OF_LOADER 0x210
#define LINUX_BOOT_HDR_LOADFLAGS 0x211
#define LINUX_BOOT_HDR_HEAP_END_PTR 0x224
#define LINUX_BOOT_HDR_CMD_LINE_PTR 0x228
#define LINUX_BOOT_HDR_RAMDISK_IMAGE 0x218
#define LINUX_BOOT_HDR_RAMDISK_SIZE 0x21C
#define LINUX_BOOT_HDR_CMDLINE_SIZE 0x238

#define LINUX_BOOT_HDR_CHECKSUM1 0xAA55u
#define LINUX_BOOT_HDR_CHECKSUM2 0x53726448u

#define LINUX_BOOT_HDR_TYPE_OF_LOADER_NOT_ASSIGNED 0xFFu

/* PARENTHESISED ON PURPOSE: these are used as `flags & ~QUIET_FLAG` and an unprefixed
 * `~1u << 5` is `(~1u) << 5`, not `~(1u << 5)` -- which silently cleared LOADED_HIGH from
 * the boot params (MEASURED: 0x80 where the reference's own load_kernel produces 0x81, and
 * the selftest case caught it). */
#define LINUX_BOOT_HDR_LOADFLAGS_LOADED_HIGH (1u << 0)
#define LINUX_BOOT_HDR_LOADFLAGS_QUIET_FLAG (1u << 5)
#define LINUX_BOOT_HDR_LOADFLAGS_KEEP_SEGMENTS (1u << 6)
#define LINUX_BOOT_HDR_LOADFLAGS_CAN_USE_HEAPS (1u << 7)

/* kernel.js:113-117 */
#define REAL_MODE_SEGMENT 0x8000u
#define REAL_MODE_BASE (REAL_MODE_SEGMENT << 4)
#define HEAP_END 0xE000u
#define HEAP_END_PTR (HEAP_END - 0x200)

/* The typed-array semantics of `mem8.set(blob, addr)` / `array[i] = v`: an index outside
 * [0, size) is SILENTLY DROPPED by JS.  The C port reproduces that answer and makes it
 * AUDIBLE on stderr (a diagnostic, like V86C_LOG_PORTS: no guest-visible value depends on
 * it).  Without the check the write would run off the end of the port's guest memory. */
static void write_blob(uint8_t *mem8, uint32_t memory_size, const uint8_t *blob, uint32_t length,
                       uint32_t addr)
{
    if (length == 0) return;

    if (addr >= memory_size) {
        fprintf(stderr, "v86c: kernel: dropping %u bytes at 0x%x: outside guest memory\n",
                (unsigned)length, (unsigned)addr);
        return;
    }
    if ((uint64_t)addr + length > memory_size) {
        uint32_t kept = memory_size - addr;
        fprintf(stderr, "v86c: kernel: dropping %u of %u bytes at 0x%x: outside guest memory\n",
                (unsigned)(length - kept), (unsigned)length, (unsigned)addr);
        length = kept;
    }
    memcpy(mem8 + addr, blob, length);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}
static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* The `dbg_assert`s of kernel.js:82, 85, 137, 162, 170 are compiled out of the pinned
 * release build (log.js's dbg_assert is `debug_assert!`, a no-op without DEBUG).  They are
 * STOPS here, with upstream's own message, because the value is used unconditionally by the
 * code that follows -- which is exactly the case M1c settled. */
static void die(const char *message)
{
    fprintf(stderr, "v86c: kernel: %s\n", message);
    exit(1);
}

/* kernel.js:181-234 */
void kernel_make_linux_boot_rom(uint16_t real_mode_segment, uint16_t heap_end,
                                uint8_t out[0x200])
{
    const uint32_t SIZE = 0x200;

    memset(out, 0, SIZE);
    wr16(out, 0xAA55);
    out[2] = (uint8_t)(SIZE / 0x200);

    uint32_t i = 3;

    out[i++] = 0xFA; /* cli */
    out[i++] = 0xB8; /* mov ax, real_mode_segment */
    out[i++] = (uint8_t)(real_mode_segment >> 0);
    out[i++] = (uint8_t)(real_mode_segment >> 8);
    out[i++] = 0x8E; /* mov es, ax */
    out[i++] = 0xC0;
    out[i++] = 0x8E; /* mov ds, ax */
    out[i++] = 0xD8;
    out[i++] = 0x8E; /* mov fs, ax */
    out[i++] = 0xE0;
    out[i++] = 0x8E; /* mov gs, ax */
    out[i++] = 0xE8;
    out[i++] = 0x8E; /* mov ss, ax */
    out[i++] = 0xD0;
    out[i++] = 0xBC; /* mov sp, heap_end */
    out[i++] = (uint8_t)(heap_end >> 0);
    out[i++] = (uint8_t)(heap_end >> 8);
    out[i++] = 0xEA; /* jmp (real_mode_segment+0x20):0x0 */
    out[i++] = 0x00;
    out[i++] = 0x00;
    out[i++] = (uint8_t)((real_mode_segment + 0x20) >> 0);
    out[i++] = (uint8_t)((real_mode_segment + 0x20) >> 8);

    DBG_ASSERT(i < SIZE);

    uint32_t checksum_index = i;
    out[checksum_index] = 0;

    int32_t checksum = 0;
    for (uint32_t j = 0; j < SIZE; j++) {
        checksum += out[j];
    }
    /* JS `data8[checksum_index] = -checksum` on a Uint8Array: the low byte of the 32-bit
     * negation, i.e. exactly (-checksum) mod 256. */
    out[checksum_index] = (uint8_t)(-checksum);
}

/* kernel.js:42-179 */
bool kernel_load(uint8_t *mem8, uint32_t memory_size,
                 uint8_t *bzimage, uint32_t bzimage_length,
                 const uint8_t *initrd, uint32_t initrd_length,
                 const char *cmdline, kernel_loaded_t *out)
{
    /* kernel.js:53-57: `quiet` is the constant false in this port's caller (kernel.js:53),
     * and the typed-array views are reads of the same bytes, so the C port reads the header
     * fields directly. */
    const bool quiet = false;

    memset(out, 0, sizeof(*out));

    if (bzimage_length < 0x240) {
        /* Upstream would read past the end of a short file's views and get undefined for
         * every field, then fail the checksum test.  Same answer, said plainly. */
        return false;
    }

    /* kernel.js:59-62.  `syssize` and `vidmode` are read into locals upstream and appear
     * ONLY in the dbg_log block (kernel.js:101-111), so they have no seat here -- a value
     * that is never used cannot change a result. */
    uint8_t setup_sects = bzimage[LINUX_BOOT_HDR_SETUP_SECTS];
    if (setup_sects == 0) setup_sects = 4;

    uint16_t checksum1 = rd16(bzimage + LINUX_BOOT_HDR_BOOT_FLAG);
    if (checksum1 != LINUX_BOOT_HDR_CHECKSUM1) {
        /* kernel.js:65-69 dbg_log("Bad checksum1: ...") then return. */
        return false;
    }

    /* Not aligned, so upstream splits it into two 16-bit reads (kernel.js:71-74). */
    uint32_t checksum2 = (uint32_t)rd16(bzimage + LINUX_BOOT_HDR_HEADER) |
                         (uint32_t)rd16(bzimage + LINUX_BOOT_HDR_HEADER + 2) << 16;
    if (checksum2 != LINUX_BOOT_HDR_CHECKSUM2) {
        /* kernel.js:75-79 dbg_log("Bad checksum2: ...") then return. */
        return false;
    }

    uint16_t protocol = rd16(bzimage + LINUX_BOOT_HDR_VERSION);
    if (protocol < 0x202) {
        die("kernel.js:82 dbg_assert(protocol >= 0x202): older protocols are not supported");
    }

    uint8_t flags = bzimage[LINUX_BOOT_HDR_LOADFLAGS];
    if ((flags & LINUX_BOOT_HDR_LOADFLAGS_LOADED_HIGH) == 0) {
        die("kernel.js:85 dbg_assert(flags & LOADED_HIGH): low kernels are not supported");
    }

    /* kernel.js:87-99 reads flags2, initrd_addr_max, kernel_alignment, relocatable,
     * min_alignment, payload_offset, payload_length, pref_address(+high) and init_size into
     * locals that feed ONLY the dbg_log block at kernel.js:101-111.  The kernel's own
     * decompressor reads those fields from the boot params in guest memory, not from these
     * locals, so dropping them drops no behaviour -- only log lines this port has no channel
     * for.  cmdline_size IS used (the assert below) and is kept. */
    uint32_t cmdline_size = protocol >= 0x206 ? rd32(bzimage + LINUX_BOOT_HDR_CMDLINE_SIZE) : 255;

    const uint32_t base_ptr = REAL_MODE_BASE;

    /* kernel.js:121-132: fill in the boot header fields the kernel needs to know. */
    bzimage[LINUX_BOOT_HDR_TYPE_OF_LOADER] = LINUX_BOOT_HDR_TYPE_OF_LOADER_NOT_ASSIGNED;

    uint8_t new_flags =
        (uint8_t)(((quiet ? flags | LINUX_BOOT_HDR_LOADFLAGS_QUIET_FLAG
                          : flags & ~LINUX_BOOT_HDR_LOADFLAGS_QUIET_FLAG) &
                   ~LINUX_BOOT_HDR_LOADFLAGS_KEEP_SEGMENTS) |
                  LINUX_BOOT_HDR_LOADFLAGS_CAN_USE_HEAPS);
    bzimage[LINUX_BOOT_HDR_LOADFLAGS] = new_flags;

    wr16(bzimage + LINUX_BOOT_HDR_HEAP_END_PTR, HEAP_END_PTR);

    /* kernel.js:131-132: "should parse the vga=... parameter from cmdline here, but we
     * don't really care" -- 0xFFFF is the protocol's "normal video". */
    wr16(bzimage + LINUX_BOOT_HDR_VIDMODE, 0xFFFF);

    /* kernel.js:136-137 */
    uint32_t cmdline_length = (uint32_t)strlen(cmdline) + 1; /* upstream's cmdline += "\0" */
    if (cmdline_length >= cmdline_size) {
        die("kernel.js:137 dbg_assert(cmdline.length < cmdline_size): the command line is "
            "longer than the header allows");
    }

    const uint32_t cmd_line_ptr = base_ptr + HEAP_END;
    wr32(bzimage + LINUX_BOOT_HDR_CMD_LINE_PTR, cmd_line_ptr);
    write_blob(mem8, memory_size, (const uint8_t *)cmdline, (uint32_t)strlen(cmdline),
               cmd_line_ptr);
    mem8[cmd_line_ptr + strlen(cmdline)] = 0;

    /* kernel.js:148-152 */
    const uint32_t prot_mode_kernel_start = (uint32_t)(setup_sects + 1) * 512;
    if (prot_mode_kernel_start > bzimage_length) {
        return false; /* upstream would read an empty slice and load nothing */
    }
    const uint32_t real_mode_length = prot_mode_kernel_start;
    const uint32_t prot_mode_length = bzimage_length - prot_mode_kernel_start;

    /* kernel.js:154-168 */
    uint32_t ramdisk_address = 0;
    uint32_t ramdisk_size = 0;

    if (initrd != NULL) {
        ramdisk_address = LINUX_INITRD_ADDRESS;
        ramdisk_size = initrd_length;

        if (!(LINUX_KERNEL_HIGH_ADDRESS + prot_mode_length < ramdisk_address)) {
            die("kernel.js:162 dbg_assert(KERNEL_HIGH_ADDRESS + protected_mode_kernel.length "
                "< ramdisk_address): the kernel would overlap the initrd");
        }

        write_blob(mem8, memory_size, initrd, initrd_length, ramdisk_address);
    }

    wr32(bzimage + LINUX_BOOT_HDR_RAMDISK_IMAGE, ramdisk_address);
    wr32(bzimage + LINUX_BOOT_HDR_RAMDISK_SIZE, ramdisk_size);

    if (!(base_ptr + real_mode_length < 0xA0000)) {
        die("kernel.js:170 dbg_assert(base_ptr + real_mode_kernel.length < 0xA0000): the "
            "real-mode setup block does not fit below the video area");
    }

    /* kernel.js:172-173.  NOTE THE ORDER: the real-mode block is copied from the image the
     * header fields were just written into, so the kernel sees the modified boot params. */
    write_blob(mem8, memory_size, bzimage, real_mode_length, base_ptr);
    write_blob(mem8, memory_size, bzimage + prot_mode_kernel_start, prot_mode_length,
               LINUX_KERNEL_HIGH_ADDRESS);

    /* kernel.js:175-178 */
    out->name = "genroms/kernel.bin";
    out->option_rom = malloc(0x200);
    if (out->option_rom == NULL) {
        fprintf(stderr, "v86c: kernel: cannot allocate the option ROM\n");
        exit(1);
    }
    out->option_rom_length = 0x200;
    kernel_make_linux_boot_rom((uint16_t)REAL_MODE_SEGMENT, (uint16_t)HEAP_END, out->option_rom);
    return true;
}
