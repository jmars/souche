/*
 * ps2.c -- the 8042 keyboard controller.  See ps2.h for the non-browser scope and for the
 * one command that stops loudly.
 */

#include "ps2.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../cpu/cpu.h" /* unimplemented_instruction */
#include "../cpu/cpu_state.h"
#include "io.h"
#include "machine.h"

static ps2_state_t ps2_storage;
ps2_state_t *ps2 = NULL;

/* JS `x | 0` -- ToInt32, used for the mouse deltas (ps2.js:318-321). */
static int32_t js_to_int32(double x)
{
    if (!isfinite(x)) return 0;
    double m = fmod(trunc(x), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

/* ------------------------------------------------------------------ *
 * ByteQueue -- lib.js
 * ------------------------------------------------------------------ */
void byte_queue_push(byte_queue_t *queue, int32_t item)
{
    /* lib.js: an intentional overwrite when full: `length` does not grow, `end` wraps. */
    if (queue->length != PS2_QUEUE_SIZE) {
        queue->length++;
    }
    queue->data[queue->end] = (uint8_t)item;
    queue->end = (queue->end + 1) & (PS2_QUEUE_SIZE - 1);
}

int32_t byte_queue_shift(byte_queue_t *queue)
{
    if (!queue->length) {
        return -1;
    }
    int32_t item = queue->data[queue->start];
    queue->start = (queue->start + 1) & (PS2_QUEUE_SIZE - 1);
    queue->length--;
    return item;
}

int32_t byte_queue_peek(byte_queue_t *queue)
{
    if (!queue->length) {
        return -1;
    }
    return queue->data[queue->start];
}

void byte_queue_clear(byte_queue_t *queue)
{
    queue->start = 0;
    queue->end = 0;
    queue->length = 0;
}

/* ------------------------------------------------------------------ *
 * ps2.js:62-155 -- reset
 * ------------------------------------------------------------------ */
void ps2_reset(void)
{
    ps2->enable_mouse_stream = false;
    ps2->use_mouse = false;
    ps2->have_mouse = true;
    ps2->mouse_delta_x = 0;
    ps2->mouse_delta_y = 0;
    ps2->mouse_clicks = 0;
    ps2->have_keyboard = true;
    ps2->enable_keyboard_stream = false;
    ps2->next_is_mouse_command = false;
    ps2->next_read_sample = false;
    ps2->next_read_led = false;
    ps2->next_handle_scan_code_set = false;
    ps2->next_read_rate = false;
    ps2->next_read_resolution = false;
    byte_queue_clear(&ps2->kbd_buffer);
    ps2->last_port60_byte = 0;
    ps2->sample_rate = 100;
    ps2->mouse_detect_state = 0;
    ps2->mouse_id = 0x00;
    ps2->mouse_reset_workaround = false;
    ps2->wheel_movement = 0;
    ps2->resolution = 4;
    ps2->scaling2 = false;
    ps2->last_mouse_packet = -1;
    byte_queue_clear(&ps2->mouse_buffer);
    ps2->next_byte_is_ready = false;
    ps2->next_byte_is_aux = false;
    /* TODO: What should be the initial value? (ps2.js:149-151) */
    ps2->command_register = 1 | 4;
    ps2->controller_output_port = 0;
    ps2->read_output_register = false;
    ps2->read_command_register = false;
    ps2->read_controller_output_port = false;
}

/* ps2.js:232-235 */
static bool mouse_stream_active(void)
{
    return ps2->enable_mouse_stream && !(ps2->command_register & 0x20);
}

/* ps2.js:237-289 */
static void ps2_raise_irq(void)
{
    if (ps2->next_byte_is_ready) {
        /* Wait until previous byte is read
         * http://halicery.com/Hardware/8042/8042_1503033_TXT.htm */
        return;
    }

    /* Kbd has priority over aux */
    if (ps2->kbd_buffer.length) {
        ps2_kbd_irq();
    }
    else if (ps2->mouse_buffer.length) {
        ps2_mouse_irq();
    }
}

void ps2_mouse_irq(void)
{
    ps2->next_byte_is_ready = true;
    ps2->next_byte_is_aux = true;

    if (ps2->command_register & 2) {
        /* Pulse the irq line; can't lower immediately after rising, so lower before rising
         * (ps2.js:266-270) */
        device_lower_irq(12);
        device_raise_irq(12);
    }
}

void ps2_kbd_irq(void)
{
    ps2->next_byte_is_ready = true;
    ps2->next_byte_is_aux = false;

    if (ps2->command_register & 1) {
        /* Pulse the irq line, as above (ps2.js:283-287) */
        device_lower_irq(1);
        device_raise_irq(1);
    }
}

/* ps2.js:291-299 */
void ps2_kbd_send_code(int32_t code)
{
    if (ps2->enable_keyboard_stream) {
        byte_queue_push(&ps2->kbd_buffer, code);
        ps2_raise_irq();
    }
}

/* ps2.js:301-336 */
void ps2_mouse_send_delta(double delta_x, double delta_y)
{
    if (!ps2->have_mouse || !ps2->use_mouse) {
        return;
    }

    /* note: delta_x or delta_y can be floating point numbers.  ps2.js:310-311 has the
     * scaling factor commented out and fixed to 1. */
    const double factor = 1;

    ps2->mouse_delta_x += delta_x * factor;
    ps2->mouse_delta_y += delta_y * factor;

    if (mouse_stream_active()) {
        /* ps2.js:318-321: `this.mouse_delta_x | 0` -- ToInt32 */
        int32_t change_x = js_to_int32(ps2->mouse_delta_x);
        int32_t change_y = js_to_int32(ps2->mouse_delta_y);

        if (change_x || change_y) {
            ps2->mouse_delta_x -= change_x;
            ps2->mouse_delta_y -= change_y;
            ps2_send_mouse_packet(change_x, change_y);
        }
    }
}

/* ps2.js:338-351 */
void ps2_mouse_send_click(int32_t left, int32_t middle, int32_t right)
{
    if (!ps2->have_mouse || !ps2->use_mouse) {
        return;
    }

    ps2->mouse_clicks = left | right << 1 | middle << 2;

    if (mouse_stream_active()) {
        ps2_send_mouse_packet(0, 0);
    }
}

/* ps2.js:353-397 */
void ps2_send_mouse_packet(int32_t dx, int32_t dy)
{
    int32_t info_byte = (dy < 0) << 5 | (dx < 0) << 4 | 1 << 3 | ps2->mouse_clicks;

    ps2->last_mouse_packet = 0; /* upstream: Date.now(), only ever compared against a
                                 * disabled rate limiter (ps2.js:363, 322-328) */

    byte_queue_push(&ps2->mouse_buffer, info_byte);
    byte_queue_push(&ps2->mouse_buffer, dx);
    byte_queue_push(&ps2->mouse_buffer, dy);

    if (ps2->mouse_id == 0x04) {
        byte_queue_push(&ps2->mouse_buffer, (0 << 5) | (0 << 4) | (ps2->wheel_movement & 0x0F));
        ps2->wheel_movement = 0;
    }
    else if (ps2->mouse_id == 0x03) {
        byte_queue_push(&ps2->mouse_buffer, ps2->wheel_movement & 0xFF);
        ps2->wheel_movement = 0;
    }

    ps2_raise_irq();
}

/* ps2.js:399-420 -- DEAD UPSTREAM (both call sites in send_mouse_packet are commented out);
 * ported for parity and reachable only from the selftest. */
int32_t ps2_apply_scaling2(int32_t n)
{
    /* http://www.computer-engineering.org/ps2mouse/#Inputs.2C_Resolution.2C_and_Scaling */
    int32_t abs_n = n < 0 ? -n : n;
    int32_t sign = n >> 31;

    switch (abs_n) {
    case 0:
    case 1:
    case 3:
        return n;
    case 2:
        return sign;
    case 4:
        return 6 * sign;
    case 5:
        return 9 * sign;
    default:
        /* JS `n << 1` is a 32-bit shift; the C port writes it on uint32_t. */
        return (int32_t)((uint32_t)n << 1);
    }
}

/* ------------------------------------------------------------------ *
 * the ports -- ps2.js:422-761
 * ------------------------------------------------------------------ */
static int32_t port60_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    ps2->next_byte_is_ready = false;

    if (!ps2->kbd_buffer.length && !ps2->mouse_buffer.length) {
        /* should not happen (ps2.js:428-433) */
        return ps2->last_port60_byte;
    }

    if (ps2->next_byte_is_aux) {
        device_lower_irq(12);
        ps2->last_port60_byte = byte_queue_shift(&ps2->mouse_buffer);
    }
    else {
        device_lower_irq(1);
        ps2->last_port60_byte = byte_queue_shift(&ps2->kbd_buffer);
    }

    if (ps2->kbd_buffer.length || ps2->mouse_buffer.length) {
        ps2_raise_irq();
    }

    return ps2->last_port60_byte;
}

static int32_t port64_read(void *device, int32_t port)
{
    (void)device;
    (void)port;
    /* status port */
    int32_t status_byte = 0x10;

    if (ps2->next_byte_is_ready) {
        status_byte |= 0x1;
    }
    if (ps2->next_byte_is_aux) {
        status_byte |= 0x20;
    }

    return status_byte;
}

static void port60_write(void *device, int32_t port, int32_t write_byte)
{
    (void)device;
    (void)port;

    if (ps2->read_command_register) {
        ps2->command_register = write_byte;
        ps2->read_command_register = false;
        /* ps2.js:485-487: a spurious-ack push is commented out upstream. */
    }
    else if (ps2->read_output_register) {
        ps2->read_output_register = false;
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->mouse_buffer, write_byte);
        ps2_mouse_irq();
    }
    else if (ps2->next_read_sample) {
        ps2->next_read_sample = false;
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->mouse_buffer, 0xFA);

        ps2->sample_rate = write_byte;

        switch (ps2->mouse_detect_state) {
        case -1:
            if (write_byte == 60) {
                /* Detect Windows NT and turn on the workaround for the bug
                 * 200->100->80->60 */
                ps2->mouse_reset_workaround = true;
                ps2->mouse_detect_state = 0;
            }
            else {
                ps2->mouse_reset_workaround = false;
                ps2->mouse_detect_state = (write_byte == 200) ? 1 : 0;
            }
            break;
        case 0:
            if (write_byte == 200) ps2->mouse_detect_state = 1;
            break;
        case 1:
            if (write_byte == 100) ps2->mouse_detect_state = 2;
            else if (write_byte == 200) ps2->mouse_detect_state = 3;
            else ps2->mouse_detect_state = 0;
            break;
        case 2:
            /* 200->100->80 activates the IntelliMouse wheel */
            if (write_byte == 80) ps2->mouse_id = 0x03;
            ps2->mouse_detect_state = -1;
            break;
        case 3:
            /* 200->200->80 activates the 4th/5th buttons */
            if (write_byte == 80) ps2->mouse_id = 0x04;
            ps2->mouse_detect_state = -1;
            break;
        }

        if (!ps2->sample_rate) {
            ps2->sample_rate = 100;
        }

        ps2_mouse_irq();
    }
    else if (ps2->next_read_resolution) {
        ps2->next_read_resolution = false;
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->mouse_buffer, 0xFA);

        if (write_byte > 3) {
            ps2->resolution = 4;
        }
        else {
            ps2->resolution = 1 << write_byte;
        }
        ps2_mouse_irq();
    }
    else if (ps2->next_read_led) {
        /* nope (ps2.js:573) */
        ps2->next_read_led = false;
        byte_queue_push(&ps2->kbd_buffer, 0xFA);
        ps2_kbd_irq();
    }
    else if (ps2->next_handle_scan_code_set) {
        ps2->next_handle_scan_code_set = false;

        byte_queue_push(&ps2->kbd_buffer, 0xFA);
        ps2_kbd_irq();

        if (write_byte) {
            /* set scan code set */
        }
        else {
            byte_queue_push(&ps2->kbd_buffer, 1);
        }
    }
    else if (ps2->next_read_rate) {
        /* nope (ps2.js:596) */
        ps2->next_read_rate = false;
        byte_queue_push(&ps2->kbd_buffer, 0xFA);
        ps2_kbd_irq();
    }
    else if (ps2->next_is_mouse_command) {
        ps2->next_is_mouse_command = false;

        if (!ps2->have_mouse) {
            return;
        }

        /* send ack */
        byte_queue_clear(&ps2->kbd_buffer);
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->mouse_buffer, 0xFA);

        switch (write_byte) {
        case 0xE6:
            /* set scaling to 1:1 */
            ps2->scaling2 = false;
            break;
        case 0xE7:
            /* set scaling to 2:1 */
            ps2->scaling2 = true;
            break;
        case 0xE8:
            /* set mouse resolution */
            ps2->next_read_resolution = true;
            break;
        case 0xE9:
            /* status request - send one packet */
            ps2_send_mouse_packet(0, 0);
            break;
        case 0xEB:
            /* request single packet */
            ps2_send_mouse_packet(0, 0);
            break;
        case 0xF2:
            /* MouseID Byte */
            byte_queue_push(&ps2->mouse_buffer, ps2->mouse_id);

            ps2->mouse_clicks = ps2->mouse_delta_x = ps2->mouse_delta_y = 0;
            ps2_raise_irq();
            break;
        case 0xF3:
            /* sample rate */
            ps2->next_read_sample = true;
            break;
        case 0xF4:
            /* enable streaming */
            ps2->enable_mouse_stream = true;
            ps2->use_mouse = true;
            BUS_SEND(ps2->bus, "mouse-enable", 1);

            ps2->mouse_clicks = ps2->mouse_delta_x = ps2->mouse_delta_y = 0;
            break;
        case 0xF5:
            /* disable streaming */
            ps2->enable_mouse_stream = false;
            break;
        case 0xF6:
            /* set defaults */
            ps2->enable_mouse_stream = false;
            ps2->sample_rate = 100;
            ps2->scaling2 = false;
            ps2->resolution = 4;
            break;
        case 0xFF:
            /* reset, send completion code */
            byte_queue_push(&ps2->mouse_buffer, 0xAA);
            byte_queue_push(&ps2->mouse_buffer, 0);

            ps2->use_mouse = true;
            BUS_SEND(ps2->bus, "mouse-enable", 1);

            ps2->enable_mouse_stream = false;
            ps2->sample_rate = 100;
            ps2->scaling2 = false;
            ps2->resolution = 4;

            if (!ps2->mouse_reset_workaround) {
                ps2->mouse_id = 0x00;
            }

            ps2->mouse_clicks = ps2->mouse_delta_x = ps2->mouse_delta_y = 0;
            break;

        default:
            /* Unimplemented mouse command (ps2.js:695-696): logged, not refused */
            break;
        }

        ps2_mouse_irq();
    }
    else if (ps2->read_controller_output_port) {
        ps2->read_controller_output_port = false;
        ps2->controller_output_port = write_byte;
        /* If we ever want to implement A20 masking, here is where we should turn the
         * masking off if the second bit is on (ps2.js:705-706) */
    }
    else {
        /* send ack */
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_clear(&ps2->kbd_buffer);
        byte_queue_push(&ps2->kbd_buffer, 0xFA);

        switch (write_byte) {
        case 0xED:
            ps2->next_read_led = true;
            break;
        case 0xF0:
            /* get/set scan code set */
            ps2->next_handle_scan_code_set = true;
            break;
        case 0xF2:
            /* identify */
            byte_queue_push(&ps2->kbd_buffer, 0xAB);
            byte_queue_push(&ps2->kbd_buffer, 0x83);
            break;
        case 0xF3:
            /* Set typematic rate and delay */
            ps2->next_read_rate = true;
            break;
        case 0xF4:
            /* enable scanning */
            ps2->enable_keyboard_stream = true;
            break;
        case 0xF5:
            /* disable scanning */
            ps2->enable_keyboard_stream = false;
            break;
        case 0xF6:
            /* reset defaults */
            break;
        case 0xFF:
            byte_queue_clear(&ps2->kbd_buffer);
            byte_queue_push(&ps2->kbd_buffer, 0xFA);
            byte_queue_push(&ps2->kbd_buffer, 0xAA);
            byte_queue_push(&ps2->kbd_buffer, 0);
            break;
        default:
            /* Unimplemented keyboard command (ps2.js:755-756): logged, not refused */
            break;
        }

        ps2_kbd_irq();
    }
}

static void port64_write(void *device, int32_t port, int32_t write_byte)
{
    (void)device;
    (void)port;

    switch (write_byte) {
    case 0x20:
        byte_queue_clear(&ps2->kbd_buffer);
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->kbd_buffer, ps2->command_register);
        ps2_kbd_irq();
        break;
    case 0x60:
        ps2->read_command_register = true;
        break;
    case 0xD0:
        /* read controller output port */
        byte_queue_clear(&ps2->kbd_buffer);
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->kbd_buffer, ps2->controller_output_port);
        ps2_kbd_irq();
        break;
    case 0xD1:
        ps2->read_controller_output_port = true;
        break;
    case 0xD3:
        ps2->read_output_register = true;
        break;
    case 0xD4:
        ps2->next_is_mouse_command = true;
        break;
    case 0xA7:
        /* Disable second port */
        ps2->command_register |= 0x20;
        break;
    case 0xA8:
        /* Enable second port */
        ps2->command_register &= ~0x20;
        break;
    case 0xA9:
        /* test second ps/2 port */
        byte_queue_clear(&ps2->kbd_buffer);
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->kbd_buffer, 0);
        ps2_kbd_irq();
        break;
    case 0xAA:
        byte_queue_clear(&ps2->kbd_buffer);
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->kbd_buffer, 0x55);
        ps2_kbd_irq();
        break;
    case 0xAB:
        /* Test first PS/2 port */
        byte_queue_clear(&ps2->kbd_buffer);
        byte_queue_clear(&ps2->mouse_buffer);
        byte_queue_push(&ps2->kbd_buffer, 0);
        ps2_kbd_irq();
        break;
    case 0xAD:
        /* Disable Keyboard */
        ps2->command_register |= 0x10;
        break;
    case 0xAE:
        /* Enable Keyboard */
        ps2->command_register &= ~0x10;
        break;
    case 0xFE:
        /* ps2.js:834-837: "CPU reboot via PS2" calls cpu.reboot_internal().
         * REBOOT-1: machine_reboot() (machine.c) resets the machine -- including this
         * device, the reference's own list -- and requests the boot re-entry; the run
         * loop unwinds and main.c re-establishes the boot. */
        machine_reboot();
        break;
    default:
        /* Unimplemented command byte (ps2.js:838-839): logged, not refused */
        break;
    }
}

/* ------------------------------------------------------------------ *
 * the bus -- ps2.js:27-53
 * ------------------------------------------------------------------ */
static void on_keyboard_code(void *this_value, const int32_t *args, int nargs)
{
    (void)this_value;
    if (nargs >= 1) ps2_kbd_send_code(args[0]);
}

static void on_mouse_click(void *this_value, const int32_t *args, int nargs)
{
    (void)this_value;
    if (nargs >= 3) ps2_mouse_send_click(args[0], args[1], args[2]);
}

static void on_mouse_delta(void *this_value, const int32_t *args, int nargs)
{
    (void)this_value;
    if (nargs >= 2) ps2_mouse_send_delta(args[0], args[1]);
}

static void on_mouse_wheel(void *this_value, const int32_t *args, int nargs)
{
    (void)this_value;
    if (nargs < 1) return;
    if (!ps2->have_mouse || !ps2->use_mouse || !mouse_stream_active()) {
        return;
    }

    ps2->wheel_movement -= args[0];
    if (nargs >= 2) ps2->wheel_movement -= args[1] * 2; /* X Wheel Movement */
    if (ps2->wheel_movement > 7) ps2->wheel_movement = 7;
    if (ps2->wheel_movement < -8) ps2->wheel_movement = -8;
    ps2_send_mouse_packet(0, 0);
}

void ps2_init(bus_connector_t *bus)
{
    ps2 = &ps2_storage;
    memset(&ps2_storage, 0, sizeof(ps2_storage));
    ps2->bus = bus;

    ps2_reset();

    /* ps2.js:27-53: the four messages the (browser) input path sends in. */
    bus_register(bus, "keyboard-code", on_keyboard_code, ps2);
    bus_register(bus, "mouse-click", on_mouse_click, ps2);
    bus_register(bus, "mouse-delta", on_mouse_delta, ps2);
    bus_register(bus, "mouse-wheel", on_mouse_wheel, ps2);

    io_register_read(0x60, ps2, port60_read, NULL, NULL);
    io_register_read(0x64, ps2, port64_read, NULL, NULL);

    io_register_write(0x60, ps2, port60_write, NULL, NULL);
    io_register_write(0x64, ps2, port64_write, NULL, NULL);
}
