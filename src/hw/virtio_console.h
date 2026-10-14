/*
 * virtio_console.h -- the virtio console device (the multi-port one).
 *
 * Port of v86/src/virtio_console.js (301 lines): the console on top of virtio.h's transport,
 * with its four-plus-six virtqueues (port 0's receive/send, the control receive/send pair,
 * and a receive/send pair per extra port), its four ports, its device-specific configuration
 * (cols/rows/max_nr_ports/emerg_wr) and the control-message protocol -- DEVICE_READY drives
 * the DEVICE_ADD/PORT_READY/CONSOLE_PORT/PORT_NAME/PORT_OPEN handshake a Linux guest's
 * virtio_console driver performs.
 *
 * THE HOST SEAM, and the one divergence this file has:
 *
 *   upstream exposes the console on the device bus with FOUR message names, one set per
 *   port: "virtio-console<N>-output-bytes" (a Uint8Array of bytes the guest wrote),
 *   "virtio-console<N>-input-bytes" (a Uint8Array the host feeds in), "virtio-console<N>-resize"
 *   (a [cols, rows] pair) -- this is what baliveau's harness names (see the file's own
 *   consumer note in results/m2-4-virtio.md), and it is the SAME seam uart.c uses
 *   ("serial0-output-byte", uart.js:408).
 *
 *   This port's bus (bus.h) carries an int32 argument VECTOR, not arbitrary JS values, so a
 *   Uint8Array-valued message is expressed as ONE BYTE PER MESSAGE, at the same message
 *   name, in the same order (D-VC1).  A listener that wrote the bytes of upstream's array to
 *   a file gets the same file: the stream is identical, the framing is not.  The "-resize"
 *   message keeps upstream's pair as two arguments, and "-input-bytes" takes its bytes the
 *   same way.
 *
 * WHAT IS A SEAT AND WHAT IS NOT:
 *  - get_state/set_state (virtio_console.js:255-277) -- not ported (no state-save machinery
 *    anywhere in this port; uart.h's precedent).
 *  - dbg_assert(false, ...) arms are compiled OUT of the pinned release build (dbg.rs:63 is
 *    debug_assert!).  Each is transcribed as the release's observable behaviour: where the
 *    assertion is the only statement (the "Emergency write!" arm, the wrong-queue arms of
 *    the control-rx handler) nothing happens except a note under V86C_LOG_VIRTIO; where a
 *    `return` follows it, the return is REAL control flow and is kept.
 *  - THE OBSERVATION SURFACE is this port's addition: bytes out, bytes in, and the two
 *    queue-chain byte totals the transport keeps (virtio.h).  The reference's harness counts
 *    the same bytes at its adapter; these counters are the same number, and they DECIDE
 *    NOTHING (results/m2-4-virtio.md's G1/G2 read them).
 */

#ifndef V86C_HW_VIRTIO_CONSOLE_H
#define V86C_HW_VIRTIO_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"
#include "virtio.h"

/* virtio_console.js:11-22 -- the two constant sets, names kept. */
#define VIRTIO_CONSOLE_DEVICE_READY 0
#define VIRTIO_CONSOLE_DEVICE_ADD 1
#define VIRTIO_CONSOLE_DEVICE_REMOVE 2
#define VIRTIO_CONSOLE_PORT_READY 3
#define VIRTIO_CONSOLE_CONSOLE_PORT 4
#define VIRTIO_CONSOLE_RESIZE 5
#define VIRTIO_CONSOLE_PORT_OPEN 6
#define VIRTIO_CONSOLE_PORT_NAME 7

#define VIRTIO_CONSOLE_F_SIZE 0
#define VIRTIO_CONSOLE_F_MULTIPORT 1
#define VIRTIO_CONSOLE_F_EMERG_WRITE 2

/* virtio_console.js:33-35 */
#define VIRTIO_CONSOLE_ROWS 25
#define VIRTIO_CONSOLE_COLS 80
#define VIRTIO_CONSOLE_PORTS 4

/* The bus message names, built per port (upstream concatenates them too).  40 is
 * "virtio-console" (14) + a 32-bit port number (10) + "-output-bytes" (13) + NUL. */
#define VIRTIO_CONSOLE_NAME_MAX 40

/* This port's bound on ONE "-input-bytes" message (upstream's message is a Uint8Array of
 * unbounded length; the host seam here carries its bytes as int32 arguments). */
#define VIRTIO_CONSOLE_INPUT_MAX 256

typedef struct virtio_console virtio_console_t;

/* The per-port bus registrations' context (upstream closes over port/queue_id; C has no
 * closures, so the context is stored and the listener is called with it -- io.h D3's
 * pattern).  The names are stored because bus_register keeps the POINTER. */
typedef struct virtio_console_listener {
    virtio_console_t *console;
    int32_t port;
    int32_t queue_id;
    char input_name[VIRTIO_CONSOLE_NAME_MAX];
    char resize_name[VIRTIO_CONSOLE_NAME_MAX];
} virtio_console_listener_t;

struct virtio_console {
    bus_connector_t *bus;

    /* virtio_console.js:31-35 */
    int32_t rows;
    int32_t cols;
    int32_t ports;

    virtio_device_t virtio;

    virtio_console_listener_t listeners[VIRTIO_CONSOLE_PORTS];

    /* the observation surface (this port's; it decides nothing) */
    uint64_t output_bytes;
    uint64_t input_bytes;
    uint64_t output_messages;
};

/* virtio_console.js:29-229 -- the constructor.  `bus` is the device bus (machine_bus), the
 * same connector the UART is attached to.  Returns false when the transport refuses the
 * options (virtio_init's bounds). */
bool virtio_console_init(virtio_console_t *console, bus_connector_t *bus);

/* virtio_console.js:275-277 */
void virtio_console_reset(virtio_console_t *console);

/* The two counters the gate reads, in a form a caller can print. */
uint64_t virtio_console_output_bytes(const virtio_console_t *console);
uint64_t virtio_console_input_bytes(const virtio_console_t *console);
/* The transport's own totals across every chain (virtio.h). */
uint64_t virtio_console_chain_bytes_read(void);
uint64_t virtio_console_chain_bytes_written(void);

#endif /* V86C_HW_VIRTIO_CONSOLE_H */
