/*
 * uart.h -- the 8250/16550 serial ports (COM1-COM4).
 *
 * Port of v86/src/uart.js (455 lines): the register set of the two channels the machine
 * gets, the divisor latch (DLAB), the FIFO and line-status behaviour, the modem-status
 * register with its delta bits, and the interrupt identification register with the four
 * interrupt sources and the IRQ line the port raises.
 *
 * WHAT HAS A SEAT AND WHAT DOES NOT:
 *
 *  - the device's own model is the transcription, exactly as upstream writes it.  The one
 *    upstream field with no counterpart is `current_line` (uart.js:90, 411-422): it exists
 *    only inside `if(DEBUG)` to accumulate a line for the dbg_log channel, and this port has
 *    no dbg_log (log.js is not ported).  A guest cannot observe it -- it is written and
 *    never read except by the log -- so its absence changes nothing a guest can see.
 *  - `get_state`/`set_state` (uart.js:298-330) are NOT ported: this port has no state-save
 *    machinery (no unit has needed one; the reference's state.js is not ported either), so
 *    there is nothing to serialize into.  Named rather than silently dropped.
 *  - the HOST SIDE is a seam with two sides, and BOTH are upstream's:
 *        out: the device sends "serial<N>-output-byte" on the device bus (uart.js:408) --
 *             the same message name the reference's browser SerialAdapter listens for;
 *        in:  the device listens for "serial<N>-input" and the four modem lines
 *             (uart.js:116-141), which the reference's starter.js sends (starter.js:1239,
 *             1252, 1264-1297).
 *    This unit attaches no reader to the output message (main.c's boot mode does, and
 *    writes the bytes to a file/stream) and no sender to the input messages; a guest that
 *    reads a byte something never sent reads 0 with the data-ready bit clear, which is what
 *    an unattached 8250 RX line means.
 */

#ifndef V86C_HW_UART_H
#define V86C_HW_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

/* The four base ports the JS constructor switches on (uart.js:92-114).  COM1 and COM3
 * share IRQ4, COM2 and COM4 share IRQ3 -- the PC's wiring, kept because the IRQ number is
 * device state. */
#define UART_COM1 0x3F8
#define UART_COM2 0x2F8
#define UART_COM3 0x3E8
#define UART_COM4 0x2E8

typedef struct uart_state {
    bus_connector_t *bus;

    int32_t com;   /* 0..3: which channel this is */
    int32_t irq;   /* 4 for COM1/COM3, 3 for COM2/COM4 */

    /* uart.js:64-90 -- the register file and the two counters. */
    int32_t ints;
    int32_t baud_rate;
    int32_t line_control;
    int32_t lsr;
    int32_t fifo_control;
    int32_t ier;
    int32_t iir;
    int32_t modem_control;
    int32_t modem_status;
    int32_t scratch_register;

    /* uart.js:88 `this.input = []` -- a JS array used as a FIFO (push/shift).  The C port
     * keeps the same semantics with a growable ring: it never drops a byte and never wraps
     * into overwriting (the reference's array cannot) -- see uart.c's rx_* helpers. */
    uint8_t *input;
    int32_t input_capacity;
    int32_t input_start;
    int32_t input_length;
} uart_state_t;

/* uart.js:56-296 -- the constructor, one channel per call.  `port` selects com/irq exactly
 * as upstream's switch does, and every register of the channel is wired here. */
void uart_attach(bus_connector_t *bus, int32_t port);

/* uart.js:54-119 and cpu.js:1201: the reference always creates uart0 (0x3F8), and uart1/
 * uart2/uart3 only when settings.uart1/uart2/uart3 are set -- which they are not by
 * default.  This is the uart0 half, i.e. what `new UART(cpu, 0x3F8, device_bus)` does. */
void uart_init(bus_connector_t *bus);

/* uart.js:298-330 is NOT ported (see the file header).  This is the honest statement of
 * that: the device has no state-save half in this port. */
#define UART_HAS_GET_STATE 0

#endif /* V86C_HW_UART_H */
