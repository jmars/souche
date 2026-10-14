/*
 * ps2.h -- the 8042 keyboard controller and the two PS/2 devices behind it (the keyboard
 * and the mouse).
 *
 * Port of v86/src/ps2.js (841 lines), the NON-BROWSER half: the controller state machine,
 * the two byte queues, the mouse command set (including the sample-rate signature dance
 * that turns on the IntelliMouse wheel / 4th-5th-button ids), and the two data ports.
 * Dropped, and named: the DOM/event-listener code does not exist here, so "keyboard-code"
 * and the three "mouse-*" bus messages are what feeds this device -- they are registered
 * on the bus connector the machine hands it (machine.c) and a later unit's host input path
 * sends them (machine_host_bus).
 *
 * WHAT A GUEST OBSERVES with no host input attached: the controller's registers, the reset
 * and self-test commands (0xAA -> 0x55, 0xAB/0xA9 -> 0x00), the command byte read/write
 * (0x20/0x60), enabling/disabling the two ports, keyboard and mouse command acks (0xFA),
 * the identify reply (0xAB 0x83), the mouse id and its reset sequence -- all real.  What
 * never happens is a scancode or a mouse packet, because nothing is sending one: a guest
 * that waits for a keystroke waits forever.  That is the honest consequence of having no
 * browser event source, and it is the same machine the reference has with `autostart:
 * false` and no UI.
 *
 * THE ONE UPSTREAM BEHAVIOUR THAT USED TO BE A LOUD STOP REBOOTS NOW (REBOOT-1): port
 * 0x64 command 0xFE ("CPU reboot via PS2", ps2.js:834-837) calls `cpu.reboot_internal()`;
 * this port's machine_reboot() (machine.c) does the same job -- it resets the machine
 * (the PS/2 among the devices, as the reference's own list names) and the run loop
 * re-enters the boot.  Memory and the disk survive, as in the reference.
 */

#ifndef V86C_HW_PS2_H
#define V86C_HW_PS2_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

/* lib.js's ByteQueue: a fixed-size ring with an intentional overwrite when full. */
#define PS2_QUEUE_SIZE 1024

typedef struct byte_queue {
    uint8_t data[PS2_QUEUE_SIZE];
    int32_t length;
    int32_t start;
    int32_t end;
} byte_queue_t;

void byte_queue_push(byte_queue_t *queue, int32_t item);
int32_t byte_queue_shift(byte_queue_t *queue);
int32_t byte_queue_peek(byte_queue_t *queue);
void byte_queue_clear(byte_queue_t *queue);

/* ps2.js:62-155 */
typedef struct ps2_state {
    bool enable_mouse_stream;
    bool use_mouse;
    bool have_mouse;
    double mouse_delta_x;
    double mouse_delta_y;
    int32_t mouse_clicks;
    bool have_keyboard;
    bool enable_keyboard_stream;
    bool next_is_mouse_command;
    bool next_read_sample;
    bool next_read_led;
    bool next_handle_scan_code_set;
    bool next_read_rate;
    bool next_read_resolution;
    byte_queue_t kbd_buffer;
    int32_t last_port60_byte;
    int32_t sample_rate;
    int32_t mouse_detect_state;
    int32_t mouse_id;
    bool mouse_reset_workaround;
    int32_t wheel_movement;
    int32_t resolution;
    bool scaling2;
    double last_mouse_packet;
    byte_queue_t mouse_buffer;
    bool next_byte_is_ready;
    bool next_byte_is_aux;
    int32_t command_register;
    int32_t controller_output_port;
    bool read_output_register;
    bool read_command_register;
    bool read_controller_output_port;

    bus_connector_t *bus;
} ps2_state_t;

extern ps2_state_t *ps2;

void ps2_init(bus_connector_t *bus);

/* ps2.js:62-155 -- the reset puts every field back to the constructor's state (upstream's
 * `reset()` is both the constructor's body and the 0xFF-reset path's). */
void ps2_reset(void);

/* ps2.js:237-397 -- the device-side half: the two IRQ pulses, the keyboard code path, the
 * three mouse input paths and the mouse packet.  Public because the bus listeners (below in
 * ps2.c) and the port handlers are declared before them -- and because a later unit's host
 * input path is a second caller. */
void ps2_mouse_irq(void);
void ps2_kbd_irq(void);
void ps2_kbd_send_code(int32_t code);
void ps2_mouse_send_delta(double delta_x, double delta_y);
void ps2_mouse_send_click(int32_t left, int32_t middle, int32_t right);
void ps2_send_mouse_packet(int32_t dx, int32_t dy);
int32_t ps2_apply_scaling2(int32_t n);

#endif /* V86C_HW_PS2_H */
