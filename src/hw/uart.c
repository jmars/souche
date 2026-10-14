/*
 * uart.c -- the 8250/16550 serial ports.  See uart.h for the JS->C mapping, for the one
 * upstream field with no seat here (current_line, DEBUG-only) and for the two ends of the
 * host seam (the bus messages the device sends and listens for).
 *
 * Every function below cites the uart.js line range it transcribes.
 */

#include "uart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu/cpu_state.h"
#include "io.h"
#include "machine.h" /* device_raise_irq / device_lower_irq */

/* uart.js:16-47 -- the register bit and constant names, kept verbatim so the transcription
 * can be read against the original. */
#define DLAB 0x80

#define UART_IER_MSI  0x08 /* Modem Status Changed int. */
#define UART_IER_THRI 0x02 /* Enable Transmitter holding register int. */
#define UART_IER_RDI  0x01 /* Enable receiver data interrupt */

#define UART_IIR_MSI    0x00 /* Modem status interrupt (Low priority) */
#define UART_IIR_NO_INT 0x01
#define UART_IIR_THRI   0x02 /* Transmitter holding register empty */
#define UART_IIR_RDI    0x04 /* Receiver data interrupt */
#define UART_IIR_RLSI   0x06 /* Receiver line status interrupt (High p.) */
#define UART_IIR_CTI    0x0c /* Character timeout */

#define UART_MCR_LOOPBACK 0x10
#define UART_MCR_RTS      0x2
#define UART_MCR_DTR      0x1

#define UART_LSR_DATA_READY        0x1  /* data available */
#define UART_LSR_TX_EMPTY          0x20 /* TX (THR) buffer is empty */
#define UART_LSR_TRANSMITTER_EMPTY 0x40 /* TX empty and line is idle */

/* the modem status register and its delta bits (uart.js:38-47) */
#define UART_MSR_DCD  0x7
#define UART_MSR_RI   0x6
#define UART_MSR_DSR  0x5
#define UART_MSR_CTS  0x4
#define UART_MSR_DDCD 0x3
#define UART_MSR_TERI 0x2
#define UART_MSR_DDSR 0x1
#define UART_MSR_DCTS 0x0

static void uart_raise_interrupt(uart_state_t *uart, int32_t line);
static void uart_clear_interrupt(uart_state_t *uart, int32_t line);
static void uart_check_interrupt(uart_state_t *uart);

/* ------------------------------------------------------------------ *
 * THE MESSAGE NAMES ARE BUILT FROM THE CHANNEL NUMBER UPSTREAM (`"serial" + this.com +
 * "-input"`, uart.js:116-141, 260-265, 408).  C has no closures and bus_register keeps the
 * POINTER, not a copy (bus.c:43), so the strings must outlive the registration: they are a
 * static table indexed by com, and they are EXACTLY the names a JS string concatenation
 * produces for com 0..3.
 * ------------------------------------------------------------------ */
typedef struct uart_names {
    const char *input;
    const char *carrier_detect;
    const char *ring_indicator;
    const char *data_set_ready;
    const char *clear_to_send;
    const char *output_byte;
    const char *data_terminal_ready_output;
    const char *request_to_send_output;
} uart_names_t;

static const uart_names_t uart_names[4] = {
    { "serial0-input", "serial0-carrier-detect-input", "serial0-ring-indicator-input",
      "serial0-data-set-ready-input", "serial0-clear-to-send-input", "serial0-output-byte",
      "serial0-data-terminal-ready-output", "serial0-request-to-send-output" },
    { "serial1-input", "serial1-carrier-detect-input", "serial1-ring-indicator-input",
      "serial1-data-set-ready-input", "serial1-clear-to-send-input", "serial1-output-byte",
      "serial1-data-terminal-ready-output", "serial1-request-to-send-output" },
    { "serial2-input", "serial2-carrier-detect-input", "serial2-ring-indicator-input",
      "serial2-data-set-ready-input", "serial2-clear-to-send-input", "serial2-output-byte",
      "serial2-data-terminal-ready-output", "serial2-request-to-send-output" },
    { "serial3-input", "serial3-carrier-detect-input", "serial3-ring-indicator-input",
      "serial3-data-set-ready-input", "serial3-clear-to-send-input", "serial3-output-byte",
      "serial3-data-terminal-ready-output", "serial3-request-to-send-output" },
};

/* ------------------------------------------------------------------ *
 * the receive FIFO -- uart.js:88's JS array with push/shift
 *
 * A JS array grows without bound and shift() removes from the front.  The C port keeps
 * both properties (nothing is ever dropped and nothing overwrites) with a ring that
 * doubles when it fills: the observable behaviour -- FIFO order, length, and "is it
 * empty" -- is the array's.
 * ------------------------------------------------------------------ */
static void rx_init(uart_state_t *uart)
{
    uart->input_capacity = 64;
    uart->input = malloc((size_t)uart->input_capacity);
    if (uart->input == NULL) {
        fprintf(stderr, "v86c: uart: cannot allocate the receiver FIFO\n");
        exit(1);
    }
    uart->input_start = 0;
    uart->input_length = 0;
}

static void rx_push(uart_state_t *uart, uint8_t byte)
{
    if (uart->input_length == uart->input_capacity) {
        int32_t new_capacity = uart->input_capacity * 2;
        uint8_t *grown = malloc((size_t)new_capacity);
        if (grown == NULL) {
            fprintf(stderr, "v86c: uart: cannot grow the receiver FIFO\n");
            exit(1);
        }
        for (int32_t i = 0; i < uart->input_length; i++) {
            grown[i] = uart->input[(uart->input_start + i) % uart->input_capacity];
        }
        free(uart->input);
        uart->input = grown;
        uart->input_capacity = new_capacity;
        uart->input_start = 0;
    }
    uart->input[(uart->input_start + uart->input_length) % uart->input_capacity] = byte;
    uart->input_length++;
}

static int32_t rx_shift(uart_state_t *uart)
{
    if (uart->input_length == 0) return 0;
    int32_t byte = uart->input[uart->input_start];
    uart->input_start = (uart->input_start + 1) % uart->input_capacity;
    uart->input_length--;
    return byte;
}

/* ------------------------------------------------------------------ *
 * the ports -- uart.js:143-295
 * ------------------------------------------------------------------ */

/* uart.js:376-391 */
static void data_received(void *device, int32_t byte)
{
    uart_state_t *uart = device;
    rx_push(uart, (uint8_t)byte);
    uart->lsr |= UART_LSR_DATA_READY;

    if (uart->fifo_control & 1) {
        uart_raise_interrupt(uart, UART_IIR_CTI);
    }
    else {
        uart_raise_interrupt(uart, UART_IIR_RDI);
    }
}

/* uart.js:444-455 */
static void set_modem_status_bit(uart_state_t *uart, int32_t msr_bit, bool set_bit)
{
    int32_t new_modem_status = set_bit ? uart->modem_status | 1 << msr_bit
                                       : uart->modem_status & ~(1 << msr_bit);
    int32_t new_delta_bits = (uart->modem_status ^ new_modem_status) >> 4;
    uart->modem_status = new_modem_status | new_delta_bits;
    if (uart->modem_status & 0x0F) {
        uart_raise_interrupt(uart, UART_IIR_MSI);
    }
}

/* uart.js:393-423 */
static void write_data(uart_state_t *uart, int32_t out_byte)
{
    if (uart->line_control & DLAB) {
        /* the parentheses are load-bearing: JS binds `&` TIGHTER than `|`, C the other way
         * round, so the transcription needs them made explicit (uart.js:397). */
        uart->baud_rate = (uart->baud_rate & ~0xFF) | out_byte;
        return;
    }

    uart_raise_interrupt(uart, UART_IIR_THRI);

    if (uart->modem_control & UART_MCR_LOOPBACK) {
        data_received(uart, out_byte & 0xFF);
    }
    else {
        BUS_SEND(uart->bus, uart_names[uart->com].output_byte, out_byte & 0xFF);
    }
}

/* uart.js:145-152 -- port+0 writes: the 8-bit form sends one byte, the 16-bit form two. */
static void port0_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    write_data(device, value);
}

static void port0_write16(void *device, int32_t port, int32_t value)
{
    (void)port;
    write_data(device, value & 0xFF);
    write_data(device, value >> 8);
}

/* uart.js:154-173 */
static void port1_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    uart_state_t *uart = device;

    if (uart->line_control & DLAB) {
        /* uart.js:158: `this.baud_rate & 0xFF | out_byte << 8` -- JS is
         * `(baud_rate & 0xFF) | (out_byte << 8)`. */
        uart->baud_rate = (uart->baud_rate & 0xFF) | (value << 8);
        /* uart.js:159 dbg_log("baud rate: ..."): the log channel does not exist here. */
    }
    else {
        if ((uart->ier & UART_IIR_THRI) == 0 && (value & UART_IIR_THRI)) {
            /* re-throw THRI if it was masked */
            uart_raise_interrupt(uart, UART_IIR_THRI);
        }

        uart->ier = value & 0xF;
        /* uart.js:170 dbg_log("interrupt enable: ...") -- no channel. */
        /* uart.js:171 this.CheckInterrupt() */
        uart_raise_interrupt(uart, -1);
    }
}

/* uart.js:175-204 */
static int32_t port0_read8(void *device, int32_t port)
{
    (void)port;
    uart_state_t *uart = device;

    if (uart->line_control & DLAB) {
        return uart->baud_rate & 0xFF;
    }
    else {
        int32_t data = 0;

        if (uart->input_length == 0) {
            /* uart.js:187 dbg_log("Read input empty") -- no channel. */
        }
        else {
            data = rx_shift(uart);
            /* uart.js:192 dbg_log("Read input: ...") -- no channel. */
        }

        if (uart->input_length == 0) {
            uart->lsr &= ~UART_LSR_DATA_READY;
            uart_clear_interrupt(uart, UART_IIR_CTI);
            uart_clear_interrupt(uart, UART_IIR_RDI);
        }

        return data;
    }
}

/* uart.js:206-216 */
static int32_t port1_read8(void *device, int32_t port)
{
    (void)port;
    uart_state_t *uart = device;

    if (uart->line_control & DLAB) {
        return uart->baud_rate >> 8;
    }
    else {
        return uart->ier & 0xF;
    }
}

/* uart.js:218-230 */
static int32_t port2_read8(void *device, int32_t port)
{
    (void)port;
    uart_state_t *uart = device;
    int32_t ret = uart->iir & 0xF;
    /* uart.js:221 dbg_log("read interrupt identification: ...") -- no channel. */

    if (uart->iir == UART_IIR_THRI) {
        uart_clear_interrupt(uart, UART_IIR_THRI);
    }

    if (uart->fifo_control & 1) ret |= 0xC0;

    return ret;
}

/* uart.js:231-235 */
static void port2_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    uart_state_t *uart = device;
    /* uart.js:233 dbg_log("fifo control: ...") -- no channel. */
    uart->fifo_control = value;
}

/* uart.js:237-246 */
static int32_t port3_read8(void *device, int32_t port)
{
    (void)port;
    return ((uart_state_t *)device)->line_control;
}

static void port3_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    ((uart_state_t *)device)->line_control = value;
}

/* uart.js:249-266 */
static int32_t port4_read8(void *device, int32_t port)
{
    (void)port;
    return ((uart_state_t *)device)->modem_control;
}

static void port4_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    uart_state_t *uart = device;
    int32_t bits_changed = uart->modem_control ^ value;
    uart->modem_control = value;

    /* uart.js:260-265: the two modem-control outputs, as bus messages.  Nothing in this
     * port listens for them yet (the reference's starter.js does not either -- it only
     * sends the corresponding inputs); they are sent because the device sends them. */
    if (bits_changed & UART_MCR_DTR) {
        BUS_SEND(uart->bus, uart_names[uart->com].data_terminal_ready_output,
                 (value & UART_MCR_DTR) ? 1 : 0);
    }
    if (bits_changed & UART_MCR_RTS) {
        BUS_SEND(uart->bus, uart_names[uart->com].request_to_send_output,
                 (value & UART_MCR_RTS) ? 1 : 0);
    }
}

/* uart.js:268-276 */
static int32_t port5_read8(void *device, int32_t port)
{
    (void)port;
    return ((uart_state_t *)device)->lsr;
}

static void port5_write8(void *device, int32_t port, int32_t value)
{
    (void)device;
    (void)port;
    (void)value;
    /* uart.js:275 dbg_log("Factory test write") -- the write is discarded, as upstream. */
}

/* uart.js:278-286 */
static int32_t port6_read8(void *device, int32_t port)
{
    (void)port;
    uart_state_t *uart = device;
    int32_t result = uart->modem_status;
    /* clear the MSR delta bits and the interrupt flag */
    uart->modem_status &= 0xF0;
    uart_clear_interrupt(uart, UART_IIR_MSI);
    return result;
}

/* uart.js:288-295 */
static int32_t port7_read8(void *device, int32_t port)
{
    (void)port;
    return ((uart_state_t *)device)->scratch_register;
}

static void port7_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    ((uart_state_t *)device)->scratch_register = value;
}

/* ------------------------------------------------------------------ *
 * the interrupt logic -- uart.js:332-371
 *
 * CheckInterrupt() is reached from two directions upstream: the explicit `this.CheckInterrupt()`
 * calls (ports 1, 6, 2) and, more importantly, the tail of Throw/ClearInterrupt, which are
 * called by every state change that can move the line.  Here it is `uart_raise_interrupt`
 * (ThrowInterrupt) and `uart_clear_interrupt` (ClearInterrupt) plus `uart_check_interrupt`
 * for the explicit calls.
 * ------------------------------------------------------------------ */
static void uart_check_interrupt(uart_state_t *uart)
{
    if ((uart->ints & 1 << UART_IIR_CTI) && (uart->ier & UART_IER_RDI)) {
        uart->iir = UART_IIR_CTI;
        device_raise_irq((uint8_t)uart->irq);
    }
    else if ((uart->ints & 1 << UART_IIR_RDI) && (uart->ier & UART_IER_RDI)) {
        uart->iir = UART_IIR_RDI;
        device_raise_irq((uint8_t)uart->irq);
    }
    else if ((uart->ints & 1 << UART_IIR_THRI) && (uart->ier & UART_IER_THRI)) {
        uart->iir = UART_IIR_THRI;
        device_raise_irq((uint8_t)uart->irq);
    }
    else if ((uart->ints & 1 << UART_IIR_MSI) && (uart->ier & UART_IER_MSI)) {
        uart->iir = UART_IIR_MSI;
        device_raise_irq((uint8_t)uart->irq);
    }
    else {
        uart->iir = UART_IIR_NO_INT;
        device_lower_irq((uint8_t)uart->irq);
    }
}

/* uart.js:361-365 -- ThrowInterrupt(line).  `-1` is the port's stand-in for the
 * `this.CheckInterrupt()` that port1_write8 calls on itself (uart.js:171): a line number
 * outside 0..31 cannot set a bit, so the state after the call is exactly upstream's. */
static void uart_raise_interrupt(uart_state_t *uart, int32_t line)
{
    if (line >= 0) {
        uart->ints |= 1 << line;
    }
    uart_check_interrupt(uart);
}

/* uart.js:367-371 -- ClearInterrupt(line) */
static void uart_clear_interrupt(uart_state_t *uart, int32_t line)
{
    uart->ints &= ~(1 << line);
    uart_check_interrupt(uart);
}

/* ------------------------------------------------------------------ *
 * the bus listeners -- uart.js:116-141
 * ------------------------------------------------------------------ */
static void serial_input(void *this_value, const int32_t *args, int nargs)
{
    if (nargs < 1) return;
    data_received(this_value, args[0]);
}

/* The four modem lines.  Each listener is the same shape: set one modem-status bit.  C has
 * no closures, so the bit is a distinct function and the device rides in `this_value`
 * (bus.c stores it, and bus_send passes it back -- io.h D3's pattern). */
static void set_dcd(void *this_value, const int32_t *args, int nargs)
{
    if (nargs < 1) return;
    set_modem_status_bit(this_value, UART_MSR_DCD, args[0] != 0);
}

static void set_ri(void *this_value, const int32_t *args, int nargs)
{
    if (nargs < 1) return;
    set_modem_status_bit(this_value, UART_MSR_RI, args[0] != 0);
}

static void set_dsr(void *this_value, const int32_t *args, int nargs)
{
    if (nargs < 1) return;
    set_modem_status_bit(this_value, UART_MSR_DSR, args[0] != 0);
}

static void set_cts(void *this_value, const int32_t *args, int nargs)
{
    if (nargs < 1) return;
    set_modem_status_bit(this_value, UART_MSR_CTS, args[0] != 0);
}

/* ------------------------------------------------------------------ *
 * the constructor -- uart.js:56-296
 * ------------------------------------------------------------------ */
static void uart_constructor(bus_connector_t *bus, int32_t port, uart_state_t *uart)
{
    memset(uart, 0, sizeof(*uart));
    uart->bus = bus;

    uart->ints = 1 << UART_IIR_THRI;

    uart->baud_rate = 0;
    uart->line_control = 0;
    /* line status register */
    uart->lsr = UART_LSR_TRANSMITTER_EMPTY | UART_LSR_TX_EMPTY;
    uart->fifo_control = 0;
    /* interrupts enable */
    uart->ier = 0;
    /* interrupt identification register */
    uart->iir = UART_IIR_NO_INT;
    uart->modem_control = 0;
    uart->modem_status = 0;
    uart->scratch_register = 0;
    uart->irq = 0;
    rx_init(uart);

    switch (port) {
    case 0x3F8: uart->com = 0; uart->irq = 4; break;
    case 0x2F8: uart->com = 1; uart->irq = 3; break;
    case 0x3E8: uart->com = 2; uart->irq = 4; break;
    case 0x2E8: uart->com = 3; uart->irq = 3; break;
    default:
        /* uart.js:110-113 dbg_log("Invalid serial port: ...") then com=0, irq=4.  The log
         * channel does not exist here; the two assignments are what a later read observes. */
        uart->com = 0;
        uart->irq = 4;
        break;
    }

    /* uart.js:116-141 -- the input half of the host seam.  The listeners carry the device
     * and the bit, because C has no closures. */
    bus_register(bus, uart_names[uart->com].input, serial_input, uart);
    bus_register(bus, uart_names[uart->com].carrier_detect, set_dcd, uart);
    bus_register(bus, uart_names[uart->com].ring_indicator, set_ri, uart);
    bus_register(bus, uart_names[uart->com].data_set_ready, set_dsr, uart);
    bus_register(bus, uart_names[uart->com].clear_to_send, set_cts, uart);

    /* uart.js:143-295 -- every register, at every width upstream registers. */
    io_register_write(port, uart, port0_write8, port0_write16, NULL);
    io_register_write(port | 1, uart, port1_write8, NULL, NULL);

    io_register_read(port, uart, port0_read8, NULL, NULL);
    io_register_read(port | 1, uart, port1_read8, NULL, NULL);
    io_register_read(port | 2, uart, port2_read8, NULL, NULL);
    io_register_write(port | 2, uart, port2_write8, NULL, NULL);
    io_register_read(port | 3, uart, port3_read8, NULL, NULL);
    io_register_write(port | 3, uart, port3_write8, NULL, NULL);
    io_register_read(port | 4, uart, port4_read8, NULL, NULL);
    io_register_write(port | 4, uart, port4_write8, NULL, NULL);
    io_register_read(port | 5, uart, port5_read8, NULL, NULL);
    io_register_write(port | 5, uart, port5_write8, NULL, NULL);
    /* port|6 has a READ only (uart.js:278-286): a write to it is unclaimed and is discarded
     * by the empty entry, exactly as in the reference. */
    io_register_read(port | 6, uart, port6_read8, NULL, NULL);
    io_register_read(port | 7, uart, port7_read8, NULL, NULL);
    io_register_write(port | 7, uart, port7_write8, NULL, NULL);
}

void uart_attach(bus_connector_t *bus, int32_t port)
{
    uart_state_t *uart = malloc(sizeof(uart_state_t));
    if (uart == NULL) {
        fprintf(stderr, "v86c: uart: cannot allocate the device\n");
        exit(1);
    }
    uart_constructor(bus, port, uart);
}

void uart_init(bus_connector_t *bus)
{
    /* cpu.js:1201: `this.devices.uart0 = new UART(this, 0x3F8, device_bus)`.  The other
     * three channels exist in uart_attach and are created when settings.uart1/2/3 say so,
     * which by default they do not. */
    uart_attach(bus, UART_COM1);
}
