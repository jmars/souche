/*
 * virtio_console.c -- the virtio console device.  See virtio_console.h for what is
 * transcribed, what is not ported, and the one bus-shape divergence (D-VC1: an
 * int32-argument bus carries upstream's Uint8Array-valued messages as one byte per message).
 */

#include "virtio_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * the marshalling upstream does through ../lib/marshall.js.  Marshall's four type codes are
 * little-endian writes into a byte buffer ("w" = 4 bytes, "h" = 2, "b" = 1); the console uses
 * ["w","h","h"] and ["w","h","h","h","h"] only, so the two helpers below are the whole of it.
 * ------------------------------------------------------------------ */
static void put_le32(uint8_t *buffer, uint32_t offset, uint32_t value)
{
    buffer[offset + 0] = (uint8_t)(value & 0xFF);
    buffer[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
    buffer[offset + 2] = (uint8_t)((value >> 16) & 0xFF);
    buffer[offset + 3] = (uint8_t)((value >> 24) & 0xFF);
}

static void put_le16(uint8_t *buffer, uint32_t offset, uint16_t value)
{
    buffer[offset + 0] = (uint8_t)(value & 0xFF);
    buffer[offset + 1] = (uint8_t)(value >> 8);
}

/* ------------------------------------------------------------------ *
 * the device-specific configuration fields -- virtio_console.js:171-203.  `slot->owner` is
 * the console (the transport's own capabilities pass the transport).
 * ------------------------------------------------------------------ */
static virtio_console_t *CONSOLE(virtio_field_slot_t *slot) { return slot->owner; }

static int32_t console_cols_read(virtio_field_slot_t *slot) { return CONSOLE(slot)->cols; }
static void console_cols_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

static int32_t console_rows_read(virtio_field_slot_t *slot) { return CONSOLE(slot)->rows; }
static void console_rows_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

static int32_t console_max_nr_ports_read(virtio_field_slot_t *slot) { return CONSOLE(slot)->ports; }
static void console_max_nr_ports_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

static int32_t console_emerg_wr_read(virtio_field_slot_t *slot)
{
    (void)slot;
    return 0;
}

static void console_emerg_wr_write(virtio_field_slot_t *slot, int32_t data)
{
    /* upstream: dbg_assert(false, "Emergency write!") -- compiled out of the release build,
     * and the assertion is the whole handler, so the release's observable behaviour is a
     * write that changes nothing.  Not invented as an emergency-output path: this device has
     * no host console behind it, and upstream's emergency write would not have one either. */
    (void)slot;
    fprintf(stderr, "v86c: virtio-console%u: emergency write of 0x%x (upstream asserts"
                    " \"Emergency write!\"; the release build drops it)\n",
            (unsigned)slot->index, (unsigned)data);
}

static const virtio_cap_field_t console_device_fields[] = {
    {2, "cols", console_cols_read, console_cols_write},
    {2, "rows", console_rows_read, console_rows_write},
    {4, "max_nr_ports", console_max_nr_ports_read, console_max_nr_ports_write},
    {4, "emerg_wr", console_emerg_wr_read, console_emerg_wr_write},
};

/* ------------------------------------------------------------------ *
 * the four queue-id helpers and the two queue operations (upstream's Send and Ack,
 * virtio_console.js:289-301).
 * ------------------------------------------------------------------ */

/* virtio_console.js:289-294.  Returns what LANDED in the chain's writable buffers: a message
 * longer than the chain's capacity is truncated by set_next_blob (virtio.js:1480-1517), and
 * the observation counter must count what the guest could read, not what was offered. */
static uint32_t console_send(virtio_console_t *console, int32_t queue_id, virtq_chain_t *chain,
                             const uint8_t *blob, uint32_t length)
{
    uint32_t written = virtq_chain_set_next_blob(chain, blob, length);
    virtqueue_push_reply(&console->virtio.queues[queue_id], chain);
    virtqueue_flush_replies(&console->virtio.queues[queue_id]);
    return written;
}

/* virtio_console.js:296-301 */
static void console_ack(virtio_console_t *console, int32_t queue_id, virtq_chain_t *chain)
{
    virtq_chain_set_next_blob(chain, NULL, 0);
    virtqueue_push_reply(&console->virtio.queues[queue_id], chain);
    virtqueue_flush_replies(&console->virtio.queues[queue_id]);
}

/* AUDIT-1 S1 / N3: the guard the three control sends were missing.  Each of them pops the
 * control RECEIVE queue (2) to stage its reply, and a guest that never posts a buffer
 * there -- or never configures the queue at all -- made the pop's dbg_asserts abort the
 * process.  Upstream has NO guard on this path (its release build pops a garbage chain
 * and runs on; there is no defined observable to match), so the port names the case and
 * drops the send -- the same shape console_resize and console_input_bytes already carry,
 * which is upstream's own guard where IT has one.  The guest-visible difference: no reply
 * is staged into a used ring the driver never posted to. */
static bool console_control_queue_ready(virtio_console_t *console, const char *what)
{
    virtqueue_t *queue = &console->virtio.queues[2];
    if (virtqueue_is_configured(queue) && virtqueue_has_request(queue)) return true;
    fprintf(stderr, "v86c: virtio-console: %s with no posted buffer on the control receive"
                    " queue (configured=%d, has_request=%d); dropping the send\n",
            what, virtqueue_is_configured(queue) ? 1 : 0,
            virtqueue_is_configured(queue) && virtqueue_has_request(queue) ? 1 : 0);
    return false;
}

/* virtio_console.js:231-239 */
static void console_send_window_size(virtio_console_t *console, int32_t port, int32_t cols,
                                     int32_t rows)
{
    if (rows == 0) rows = console->rows;
    if (cols == 0) cols = console->cols;

    if (!console_control_queue_ready(console, "window-size")) return;
    virtq_chain_t *chain = virtqueue_pop_request(&console->virtio.queues[2]);
    uint8_t buffer[12];
    put_le32(buffer, 0, (uint32_t)port);
    put_le16(buffer, 4, VIRTIO_CONSOLE_RESIZE);
    put_le16(buffer, 6, 0);
    put_le16(buffer, 8, (uint16_t)rows);
    put_le16(buffer, 10, (uint16_t)cols);
    console_send(console, 2, chain, buffer, sizeof(buffer));
}

/* virtio_console.js:241-252 */
static void console_send_name(virtio_console_t *console, int32_t port, const char *name)
{
    uint32_t name_length = (uint32_t)strlen(name);

    if (!console_control_queue_ready(console, "port-name")) return;
    virtq_chain_t *chain = virtqueue_pop_request(&console->virtio.queues[2]);
    uint8_t buffer[8 + 64 + 1];
    if (name_length > 64) name_length = 64; /* this port's bound; upstream's names are "virtio-N" */
    memset(buffer, 0, sizeof(buffer));
    put_le32(buffer, 0, (uint32_t)port);
    put_le16(buffer, 4, VIRTIO_CONSOLE_PORT_NAME);
    put_le16(buffer, 6, 1);
    memcpy(buffer + 8, name, name_length);
    buffer[8 + name_length] = 0;
    console_send(console, 2, chain, buffer, 8 + name_length + 1);
}

/* virtio_console.js:279-287 */
static void console_send_event(virtio_console_t *console, int32_t port, uint16_t event,
                               uint16_t value)
{
    if (!console_control_queue_ready(console, "event")) return;
    virtq_chain_t *chain = virtqueue_pop_request(&console->virtio.queues[2]);
    uint8_t buffer[8];
    put_le32(buffer, 0, (uint32_t)port);
    put_le16(buffer, 4, event);
    put_le16(buffer, 6, value);
    console_send(console, 2, chain, buffer, sizeof(buffer));
}

/* ------------------------------------------------------------------ *
 * the guest -> host path (queue 1 and each extra port's send queue).
 * ------------------------------------------------------------------ */

/* A queue id the driver wrote into a notify register is used as an ARRAY INDEX upstream
 * (`this.virtio.queues[queue_id]`); a value out of range is `undefined` there and a
 * JS TypeError follows.  This port names it and ignores the notification instead of reading
 * past the array -- the one place this file adds a check upstream has not got. */
static virtqueue_t *console_queue_for_notify(virtio_console_t *console, uint32_t queue_id,
                                             const char *what)
{
    if (queue_id >= (uint32_t)console->virtio.queue_count) {
        fprintf(stderr, "v86c: virtio-console: %s notified for queue %u, which this device"
                        " does not have (%d queues)\n",
                what, (unsigned)queue_id, (int)console->virtio.queue_count);
        return NULL;
    }
    return &console->virtio.queues[queue_id];
}

/* virtio_console.js:91-103 -- the notification handler for a port's SEND queue: the guest
 * has written bytes, hand them to the host seam. */
static void console_notify_output(void *device, uint32_t queue_id)
{
    virtio_console_t *console = device;
    virtqueue_t *queue = console_queue_for_notify(console, queue_id, "output");
    if (queue == NULL) return;

    /* virtio_console.js:94 -- upstream's port arithmetic, transcribed as it stands.  Its own
     * expression is `(queue_id-3 >> 1)`; for the ports above 0 this labels port 1's queue as
     * 0, 2's as 1 and 3's as 2 (MEASURED by evaluating it), which is upstream's own off-by-one
     * in a HOST-VISIBLE LABEL only: it changes the message name, never the bytes.  Port 0 --
     * the one virtio-console0-output-bytes names, and the one a `console=hvc0` guest boots
     * on -- takes the `queue_id > 3` false arm and is unaffected. */
    int32_t port = queue_id > 3 ? (int32_t)((queue_id - 3) >> 1) : 0;
    if (port >= console->ports) port = console->ports - 1;

    /* AUDIT-1 S1: a notify on a queue the driver never configured made
     * virtqueue_has_request's dbg_assert (avail_addr != 0) abort the process -- one port
     * write, fully guest-controlled.  Upstream's release reads a garbage count and pops a
     * garbage chain; the port names the case and drops the notification (console_input
     * bytes' own guard below is upstream's, and has this shape). */
    if (!virtqueue_is_configured(queue)) {
        fprintf(stderr, "v86c: virtio-console: output notified for unconfigured queue %u;"
                        " dropping the notification\n", (unsigned)queue_id);
        return;
    }

    char message_name[VIRTIO_CONSOLE_NAME_MAX];
    snprintf(message_name, sizeof(message_name), "virtio-console%d-output-bytes", (int)port);

    while (virtqueue_has_request(queue)) {
        virtq_chain_t *chain = virtqueue_pop_request(queue);
        uint32_t length = chain->length_readable;
        /* upstream allocates `new Uint8Array(length)` here, which is ZERO-filled -- the
         * port's malloc was not, and that was invisible only while every read filled the
         * whole buffer.  With the audit-1 S2 clamp, a past-the-end descriptor supplies
         * FEWER bytes than the chain asks for (upstream's empty subarray copies nothing),
         * so the unfilled tail must be zeros as upstream's is, not host heap. */
        uint8_t *buffer = calloc(1, length > 0 ? length : 1);
        if (buffer == NULL) {
            fprintf(stderr, "v86c: virtio-console: cannot allocate %u bytes for a guest"
                            " write\n",
                    (unsigned)length);
            exit(1);
        }
        uint32_t got = virtq_chain_get_next_blob(chain, buffer, length);

        /* D-VC1: one message per byte, at upstream's own message name. */
        for (uint32_t i = 0; i < got; i++) {
            int32_t byte = (int32_t)buffer[i];
            bus_send(console->bus, message_name, &byte, 1);
        }
        console->output_bytes += got;
        console->output_messages++;

        free(buffer);
        console_ack(console, (int32_t)queue_id, chain);
    }
}

/* virtio_console.js:104-113 -- the control RECEIVE queue's notification handler.  Upstream's
 * body is exactly `if(queue_id !== 2) dbg_assert(false, ...)`: the assertion is compiled out
 * of the release build, so the release's behaviour is "nothing happens", which is what this
 * port does (with the note below under V86C_LOG_VIRTIO).  The queue has nothing to send until
 * the DRIVER writes an event, which arrives on queue 3. */
static void console_notify_control_rx(void *device, uint32_t queue_id)
{
    (void)device;
    if (queue_id != 2) {
        fprintf(stderr, "v86c: virtio-console: notified for wrong queue %u on the control"
                        " receive queue (expected 2); upstream's assertion for this is"
                        " compiled out of the release build\n",
                (unsigned)queue_id);
    }
}

/* virtio_console.js:114-164 -- the control TRANSMIT queue: the driver's own events. */
static void console_notify_control_tx(void *device, uint32_t queue_id)
{
    virtio_console_t *console = device;

    if (queue_id != 3) {
        /* upstream: dbg_assert(false, "Notified for wrong queue") then `return` -- the return
         * is outside the assertion, so it is real control flow even in the release build. */
        fprintf(stderr, "v86c: virtio-console: notified for wrong queue %u on the control"
                        " transmit queue (expected 3)\n",
                (unsigned)queue_id);
        return;
    }

    virtqueue_t *queue = console_queue_for_notify(console, queue_id, "control transmit");
    if (queue == NULL) return;

    /* AUDIT-1 S1: the same unconfigured-queue guard console_notify_output carries -- a
     * notify on a queue the driver never configured made virtqueue_has_request's
     * dbg_assert abort the process. */
    if (!virtqueue_is_configured(queue)) {
        fprintf(stderr, "v86c: virtio-console: control transmit notified for unconfigured"
                        " queue %u; dropping the notification\n", (unsigned)queue_id);
        return;
    }

    while (virtqueue_has_request(queue)) {
        virtq_chain_t *chain = virtqueue_pop_request(queue);
        uint32_t length = chain->length_readable;
        /* upstream allocates `new Uint8Array(length_readable)`, which is ZERO-filled: the
         * control packet's fields are then read out of it whatever the length.  A chain
         * shorter than the 8-byte packet therefore reads zeros upstream, and this port must
         * too (a stack buffer would read its own previous contents). */
        uint8_t buffer[16];
        memset(buffer, 0, sizeof(buffer));
        if (length > sizeof(buffer)) {
            /* The control packet is 8 bytes (an id, an event and a value); upstream reads
             * only those 8 out of its own heap copy.  A longer chain's tail is not read, so
             * this is upstream's behaviour, not a truncation of a message that matters. */
            length = sizeof(buffer);
        }
        virtq_chain_get_next_blob(chain, buffer, length);

        uint32_t port = (uint32_t)buffer[0] | (uint32_t)buffer[1] << 8 |
                        (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
        uint16_t event = (uint16_t)((uint16_t)buffer[4] | (uint16_t)buffer[5] << 8);
        /* uint16_t value = buffer[6] | buffer[7] << 8; -- upstream unmarshals it and uses it
         * in the arms below only through the events it selects; kept for the trace. */

        console_ack(console, (int32_t)queue_id, chain);

        switch (event) {
        case VIRTIO_CONSOLE_DEVICE_READY:
            for (int32_t i = 0; i < console->ports; i++) {
                console_send_event(console, i, VIRTIO_CONSOLE_DEVICE_ADD, 0);
            }
            break;
        case VIRTIO_CONSOLE_PORT_READY: {
            char name[VIRTIO_CONSOLE_NAME_MAX];
            /* upstream's SECOND Ack of the same chain, transcribed: virtio_console.js:146
             * acks the request once at line 137 and again here, which stages the same
             * descriptor chain into the used ring twice.  Kept because it is what the
             * reference does and the gate compares the two machines; results/m2-4-virtio.md
             * records whether a real driver notices. */
            console_ack(console, (int32_t)queue_id, chain);
            console_send_event(console, (int32_t)port, VIRTIO_CONSOLE_CONSOLE_PORT, 1);
            snprintf(name, sizeof(name), "virtio-%u", (unsigned)port);
            console_send_name(console, (int32_t)port, name);
            console_send_event(console, (int32_t)port, VIRTIO_CONSOLE_PORT_OPEN, 1);
            break;
        }
        case VIRTIO_CONSOLE_PORT_OPEN:
            console_ack(console, (int32_t)queue_id, chain); /* upstream's second Ack again */
            if (port == 0) {
                console_send_window_size(console, (int32_t)port, 0, 0);
            }
            break;
        default:
            /* upstream: dbg_assert(false, "received unknown event") then `return`. */
            fprintf(stderr, "v86c: virtio-console: unknown control event %u (upstream asserts"
                            " this; the release build drops it and returns)\n",
                    (unsigned)event);
            return;
        }
    }
}

/* virtio_console.js:87-90 -- the notification handler for queue 0's notify offset, which
 * upstream registers EMPTY: nothing is done when the driver notifies that it added receive
 * buffers, because this device has no buffered input to push. */
static void console_notify_empty(void *device, uint32_t queue_id)
{
    (void)device;
    (void)queue_id;
}

static const virtio_notify_handler_fn console_notify_handlers[] = {
    console_notify_empty,        /* notify offset 0: port 0/lower receive queues */
    console_notify_output,       /* notify offset 1: a port's send queue */
    console_notify_control_rx,   /* notify offset 2: the control receive queue */
    console_notify_control_tx,   /* notify offset 3: the control transmit queue */
};

/* ------------------------------------------------------------------ *
 * the host -> guest path (virtio_console.js:206-228): the two bus listeners per port.
 * ------------------------------------------------------------------ */


/* virtio_console.js:208-216 */
static void console_input_bytes(void *this_value, const int32_t *args, int nargs)
{
    virtio_console_listener_t *ctx = this_value;
    virtio_console_t *console = ctx->console;
    virtqueue_t *queue = &console->virtio.queues[ctx->queue_id];

    if (!virtqueue_has_request(queue)) {
        /* upstream: `//TODO: Buffer` -- a byte that arrives with no posted receive buffer is
         * DROPPED.  Transcribed: this device does not invent a queue. */
        return;
    }

    /* upstream's `new Uint8Array(data)`: one message becomes one blob, written into ONE
     * chain.  The port's bus carries the bytes as int32 arguments (D-VC1), so they are
     * converted here; VIRTIO_CONSOLE_INPUT_MAX is this port's bound on one message, and a
     * longer message's tail is dropped exactly as upstream drops whatever does not fit the
     * chain (set_next_blob stops at the last writable buffer). */
    uint8_t bytes[VIRTIO_CONSOLE_INPUT_MAX];
    int32_t length = nargs;
    if (length > (int32_t)sizeof(bytes)) length = (int32_t)sizeof(bytes);
    for (int32_t i = 0; i < length; i++) bytes[i] = (uint8_t)args[i];

    virtq_chain_t *chain = virtqueue_pop_request(queue);
    console->input_bytes += console_send(console, ctx->queue_id, chain, bytes, (uint32_t)length);
}

/* virtio_console.js:218-227 */
static void console_resize(void *this_value, const int32_t *args, int nargs)
{
    virtio_console_listener_t *ctx = this_value;
    virtio_console_t *console = ctx->console;
    int32_t cols = nargs > 0 ? args[0] : console->cols;
    int32_t rows = nargs > 1 ? args[1] : console->rows;

    if (ctx->port == 0) {
        console->cols = cols;
        console->rows = rows;
    }

    virtqueue_t *control = &console->virtio.queues[2];
    if (virtqueue_is_configured(control) && virtqueue_has_request(control)) {
        console_send_window_size(console, ctx->port, cols, rows);
    }
}

/* ------------------------------------------------------------------ *
 * the constructor -- virtio_console.js:29-229.
 * ------------------------------------------------------------------ */

bool virtio_console_init(virtio_console_t *console, bus_connector_t *bus)
{
    memset(console, 0, sizeof(*console));
    console->bus = bus;
    console->rows = VIRTIO_CONSOLE_ROWS;
    console->cols = VIRTIO_CONSOLE_COLS;
    console->ports = VIRTIO_CONSOLE_PORTS;

    /* virtio_console.js:37-60 -- the queue table: four for port 0 and the control pair, then
     * a receive/send pair per extra port. */
    virtio_queue_options_t queues[VIRTIO_MAX_QUEUES];
    int32_t queue_count = 0;
    for (int32_t i = 0; i < 4; i++) {
        queues[queue_count].size_supported = 16;
        queues[queue_count].notify_offset = (uint32_t)i;
        queue_count++;
    }
    for (int32_t port = 1; port < console->ports; port++) {
        queues[queue_count].size_supported = 16;
        queues[queue_count].notify_offset = 0;
        queue_count++;
        queues[queue_count].size_supported = 8;
        queues[queue_count].notify_offset = 1;
        queue_count++;
    }

    /* virtio_console.js:73-78 -- VIRTIO_CONSOLE_F_SIZE and F_MULTIPORT are the console's own
     * features; F_VERSION_1 is the transport's own precondition (virtio.h).  F_EMERG_WRITE is
     * NOT offered, which is upstream's choice: the driver then never calls the emerg_wr field
     * (its write arm above is dead for a driver that negotiates like Linux's). */
    static const uint32_t features[] = {
        VIRTIO_CONSOLE_F_SIZE,
        VIRTIO_CONSOLE_F_MULTIPORT,
        VIRTIO_F_VERSION_1,
    };

    virtio_options_t options;
    memset(&options, 0, sizeof(options));
    options.name = "virtio-console";
    options.pci_id = 0x0C << 3;
    options.device_id = 0x1043;
    options.subsystem_device_id = 3;
    options.device = console;
    options.common.initial_port = 0xB800;
    options.common.queues = queues;
    options.common.queue_count = queue_count;
    options.common.features = features;
    options.common.feature_count = (int32_t)(sizeof(features) / sizeof(features[0]));
    options.common.on_driver_ok = NULL; /* upstream's `on_driver_ok: () => {}` */
    options.notification.initial_port = 0xB900;
    options.notification.single_handler = false;
    options.notification.handlers = console_notify_handlers;
    options.notification.handler_count = (int32_t)(sizeof(console_notify_handlers) /
                                                  sizeof(console_notify_handlers[0]));
    options.notification.owner = console;
    options.isr_status.initial_port = 0xB700;

    virtio_device_specific_options_t device_specific;
    memset(&device_specific, 0, sizeof(device_specific));
    device_specific.initial_port = 0xB600;
    device_specific.fields = console_device_fields;
    device_specific.field_count = (int32_t)(sizeof(console_device_fields) /
                                            sizeof(console_device_fields[0]));
    device_specific.owner = console;
    options.device_specific = &device_specific;

    if (!virtio_init(&console->virtio, &options)) return false;

    /* virtio_console.js:206-228 -- one input and one resize listener per port.  Upstream's
     * closure captures (port, queue_id); the port stores them. */
    for (int32_t port = 0; port < console->ports; port++) {
        int32_t queue_id = port == 0 ? 0 : port * 2 + 2;
        console->listeners[port].console = console;
        console->listeners[port].port = port;
        console->listeners[port].queue_id = queue_id;
        snprintf(console->listeners[port].input_name, VIRTIO_CONSOLE_NAME_MAX,
                 "virtio-console%d-input-bytes", (int)port);
        snprintf(console->listeners[port].resize_name, VIRTIO_CONSOLE_NAME_MAX,
                 "virtio-console%d-resize", (int)port);

        bus_register(bus, console->listeners[port].input_name, console_input_bytes,
                     &console->listeners[port]);
        bus_register(bus, console->listeners[port].resize_name, console_resize,
                     &console->listeners[port]);
    }

    return true;
}

/* virtio_console.js:275-277 */
void virtio_console_reset(virtio_console_t *console)
{
    virtio_reset(&console->virtio);
}

uint64_t virtio_console_output_bytes(const virtio_console_t *console)
{
    return console->output_bytes;
}

uint64_t virtio_console_input_bytes(const virtio_console_t *console)
{
    return console->input_bytes;
}

uint64_t virtio_console_chain_bytes_read(void) { return virtq_chain_bytes_read(); }
uint64_t virtio_console_chain_bytes_written(void) { return virtq_chain_bytes_written(); }
