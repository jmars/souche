/*
 * kernel.h -- the Linux bzImage boot-protocol loader.
 *
 * Port of v86/src/kernel.js (234 lines): `load_kernel` (kernel.js:42-179) -- the x86 boot
 * protocol's real-mode setup area, the protected-mode kernel, the boot parameters, the
 * command line and the initrd placement -- and its `make_linux_boot_rom` (kernel.js:181-234),
 * the 512-byte OPTION ROM that finishes the job in real mode.
 *
 * WHAT THE OPTION ROM IS, because the whole boot gate depends on it: it is NOT a BIOS and
 * it is not executed by this port directly.  Its own header (kernel.js:183) says it is
 * "executed by seabios after its initialisation"; upstream hands it to the fw_cfg port
 * (cpu.js:1127-1160) and seabios copies it to 0xC0000 and far-calls its entry.  This port
 * has no seabios, so the HOST does what seabios does with it: writes the bytes at 0xC0000
 * and resets the CPU into a firmware shim (tests/minibios.asm) that scans for the 0xAA55
 * signature, checks the block's checksum, and far-calls entry offset 3 -- the same eight
 * instructions the ROM contains, reached the same way.  The ROM's prologue is then what
 * enters the kernel's own real-mode setup code at 0x80200.
 *
 * What the loader writes, and where (all of it kernel.js's own geometry):
 *
 *     0x80000  the real-mode setup block (the bzImage's first (setup_sects+1)*512 bytes)
 *     0x100000 the protected-mode kernel (the rest of the bzImage)
 *     0x8E000  the command line (base_ptr + heap_end, 0x8000:0xE000)
 *     64 MiB   the initrd, if there is one
 *     and the boot params INSIDE the setup block: type_of_loader, loadflags, heap_end_ptr,
 *     vid_mode, the cmdline pointer, and the initrd address/size.
 *
 * ONE LOADER-VISIBLE DIVERGENCE, declared at its site: upstream's four `dbg_assert`s
 * (kernel.js:82, 85, 137, 162, 170) are compiled OUT of the pinned release build, so a
 * malformed image is silently mis-loaded there.  This port STOPS with the assert's own
 * message, which is the convention M1c's ELF loader established for loader structural
 * failures ("continuing would silently mis-load the guest").
 */

#ifndef V86C_HW_KERNEL_H
#define V86C_HW_KERNEL_H

#include <stdbool.h>
#include <stdint.h>

/* kernel.js:46-51 */
#define LINUX_KERNEL_HIGH_ADDRESS 0x100000u
#define LINUX_INITRD_ADDRESS (64u << 20)

typedef struct kernel_loaded {
    const char *name;   /* kernel.js:176 "genroms/kernel.bin" */
    uint8_t *option_rom;
    uint32_t option_rom_length;
} kernel_loaded_t;

/* kernel.js:42-179.  `bzimage` is MODIFIED IN PLACE (upstream writes into the Uint8Array
 * views of the caller's buffer) -- fields of the boot header and nothing else.
 *
 * Returns true and fills `out` when the image has a valid header, false when it does not
 * (upstream logs "Bad checksum1"/"Bad checksum2" and returns undefined; the log channel does
 * not exist here, so the diagnosis is the caller's).  The caller owns `out->option_rom`. */
bool kernel_load(uint8_t *mem8, uint32_t memory_size,
                 uint8_t *bzimage, uint32_t bzimage_length,
                 const uint8_t *initrd, uint32_t initrd_length,
                 const char *cmdline, kernel_loaded_t *out);

/* kernel.js:181-234 -- the boot ROM bytes, exposed so the unit's selftest can check the
 * header, the prologue and the checksum without booting anything. */
void kernel_make_linux_boot_rom(uint16_t real_mode_segment, uint16_t heap_end,
                                uint8_t out[0x200]);

#endif /* V86C_HW_KERNEL_H */
