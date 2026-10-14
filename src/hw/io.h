/*
 * io.h -- the ISA I/O port bus (the port map) and the memory-mapped device dispatch.
 *
 * Port of v86/src/io.js (471 lines) plus the mmap half of v86/src/cpu.js:227-289
 * (CPU.prototype.mmap_read8/write8/write16/read32/write32/write64/write128) and
 * cpu.rs:4613-4649 (io_port_read8/16/32, io_port_write8/16/32 with the PIC's six ports
 * intercepted in front).
 *
 * THIS FILE TAKES OVER THE `io_port_*` DEFINITIONS FROM src/cpu/pic.c (the M1c handover
 * the brief makes binding): the PIC's six ports stay intercepted IN FRONT -- M1c's call
 * order, and pic.c keeps their register accessors -- and everything else now routes
 * through the port map below, which answers an unclaimed port as the reference does
 * (D-F1/M2-1F: see D1 below).
 *
 * THREE DELIBERATE DIVERGENCES FROM UPSTREAM, each stated where a guest can see it:
 *
 *  D1  AN UNCLAIMED PORT ANSWERS, AND THE LOUDNESS IS A DIAGNOSTIC.  Upstream's empty entry
 *      answers 0xFF / 0xFFFF / -1 to a read and swallows a write (io.js:50-77), and its
 *      DEBUG build registers the two "discard" ports 0x80 and 0xE9 while its RELEASE build
 *      leaves them to that empty entry (cpu.js:1168-1175 is inside `if(DEBUG)`).  This port
 *      NOW MATCHES THE REFERENCE EXACTLY on both counts -- see results/m2-1f-policy.md
 *      (D-F1) for the measurement and the reason.  What is port-specific is only the
 *      LOUDNESS: with `V86C_LOG_PORTS` set (any nonempty value but "0"), every unclaimed
 *      ACCESS prints the access, the port and its seabios name to stderr.  The log DECIDES
 *      NOTHING: no return value, device or port-map byte depends on it, so a guest cannot
 *      tell whether the diagnostic is on.
 *
 *      THIS REPLACES M1c/M2-1's D1 ("an unclaimed port stops with the port named").  That
 *      policy was right while there was no device layer -- it refused to fabricate a value
 *      -- but with the reference's own empty-entry answer available, stopping IS the
 *      divergence, and it stopped the first boot at the first `outb $0x80` (every BIOS POST
 *      code and the Linux kernel's delay loop write that port).  The old policy survives
 *      for an entry with NO handler at all: D2.
 *
 *  D2  AN ENTRY WITH NO HANDLER STOPS.  Upstream's register_read/write installs a
 *      dbg_assert-fail function for the widths a device did not register, but ONLY under
 *      DEBUG; in a release build the field keeps whatever was there -- and since the port
 *      map starts as empty entries, that is the EMPTY entry's handler, which ANSWERS.  So a
 *      16-bit read of an 8-bit-only port returns 0xFFFF on the reference, and this port
 *      returns 0xFFFF too (D1).  The stop is left for the one case the reference has no
 *      answer for: a handler pointer that was never installed (io_init leaves none behind,
 *      so the arm is unreachable in a correctly built machine; upstream's release build
 *      throws a TypeError there).
 *
 *  D3  WRITE CALLBACKS TAKE THE PORT.  Upstream's write callbacks are closures over the
 *      port (`function(value) { this.counter_write(0, value); }`, io.js:104-125); C has no
 *      closures, so the port is a parameter: `void (*)(void *device, int32_t port, int32_t
 *      value)`.  This is what makes io.js:171-256 (register_*_consecutive) expressible
 *      WITHOUT a closure -- its composed handlers recompute the neighbours from the port
 *      map at call time.  Behaviour is identical; the shape is the language's.
 *
 *  D4  THE UNCLAIMED RANGE IS NOT RE-REGISTERED.  See io.c's io_init: upstream registers
 *      io.js:42-61's 0xFF/-1 callbacks for [memory_size, MMAP_MAX) and its VGA device then
 *      overwrites the blocks it owns; this port's VGA stand-in is memory.c's default table,
 *      so registering the range would SHADOW the R1F carve-out (the VGA window and the
 *      8 MiB LFB must read 0, MEASURED against the pinned reference).  Unregistered blocks
 *      fall through to that table, which is the same answer for every other address.
 *
 * The mmap half: upstream keeps four parallel arrays of 32768 callbacks
 * (cpu.memory_map_read8/…, MMAP_BLOCK_BITS = 17) and routes every access outside plain RAM
 * through them (memory.rs:1-13's wasm imports).  The C port's memory router calls
 * `memory_devices` (memory.c) instead, so io_init() installs the mmap dispatch as that
 * hook table -- the two halves the brief assigns to this file.
 */

#ifndef V86C_HW_IO_H
#define V86C_HW_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "../cpu/memory.h"

#define IO_PORT_COUNT 0x10000 /* io.js:29-33 */

/* io.js:36-48 -- one port's handlers and the device that owns them.  Upstream's entries
 * are objects with six methods; the C entry is the same six function pointers plus the
 * `device` pointer upstream uses as the callbacks' `this` (D3 adds the port to writes). */
typedef int32_t (*io_read_fn)(void *device, int32_t port);
typedef void (*io_write_fn)(void *device, int32_t port, int32_t value);

typedef struct io_port_entry {
    io_read_fn read8;
    io_read_fn read16;
    io_read_fn read32;
    io_write_fn write8;
    io_write_fn write16;
    io_write_fn write32;
    void *device;
} io_port_entry_t;

/* The port map (upstream `cpu.io.ports`).  PCI's set_io_bars moves entries between ports
 * (pci.js:519-549), so the array is part of this module's interface, not private. */
extern io_port_entry_t io_ports[IO_PORT_COUNT];

/* The IO constructor (io.js:22-61): clear the port map, register the unclaimed-memory
 * answer for [memory_size, MMAP_MAX), and install the mmap dispatch as the memory router's
 * device hook table.  REQUIRES cpu->memory_size to be set; calling it again re-creates an
 * empty port map (what the selftest uses between cases). */
void io_init(void);

/* io.js:81-160 */
void io_register_read(int32_t port, void *device, io_read_fn r8, io_read_fn r16, io_read_fn r32);
void io_register_write(int32_t port, void *device, io_write_fn w8, io_write_fn w16, io_write_fn w32);

/* io.js:171-256 -- the consecutive-port helpers.  `r8_3`/`r8_4` may be NULL for the two-port
 * form (upstream's `arguments.length === 4` arm). */
void io_register_read_consecutive(int32_t port, void *device, io_read_fn r8_1, io_read_fn r8_2,
                                  io_read_fn r8_3, io_read_fn r8_4);
void io_register_write_consecutive(int32_t port, void *device, io_write_fn w8_1, io_write_fn w8_2,
                                   io_write_fn w8_3, io_write_fn w8_4);

/* io.js:36-48 */
io_port_entry_t io_create_empty_entry(void);

/* io.js:259-329 -- the memory-mapped device registration.  `read32`/`write32` may be NULL,
 * in which case upstream's shim (four 8-bit calls on the same block) is installed. */
void io_mmap_register(uint32_t addr, uint32_t size, int32_t (*read8)(uint32_t),
                      void (*write8)(uint32_t, int32_t), int32_t (*read32)(uint32_t),
                      void (*write32)(uint32_t, int32_t));

/* io.js:332-394 -- the dispatch behind the port bridge (what upstream calls
 * `js::io_port_*`): call the registered handler -- WHICH INCLUDES the empty entry that
 * answers an unclaimed port (D1) -- or STOP for a port with no handler at all (D2).  With
 * `V86C_LOG_PORTS` set, an unclaimed access is logged to stderr; see D1. */
int32_t io_portmap_read8(int32_t port);
int32_t io_portmap_read16(int32_t port);
int32_t io_portmap_read32(int32_t port);
void io_portmap_write8(int32_t port, int32_t value);
void io_portmap_write16(int32_t port, int32_t value);
void io_portmap_write32(int32_t port, int32_t value);

/* io.js:396-452 -- the seabios port-name table, used only to make the loud stops readable */
const char *io_get_port_description(int32_t addr);

/* ------------------------------------------------------------------ *
 * cpu.rs:4613-4649 -- the port bridge the CPU's in/out instructions call: the PIC's six
 * programming ports and the two ELCR ports intercepted in front, the port map behind.
 * Declared in src/cpu/string.h (the in/out instruction functions are its); defined in io.c.
 * ------------------------------------------------------------------ */

#endif /* V86C_HW_IO_H */
