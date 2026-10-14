/*
 * virtio.c -- the virtio-PCI transport.  See virtio.h for what this is (MODERN virtio 1.x),
 * what is not ported (state-save, the log channel) and how upstream's compiled-out
 * assertions are handled.
 *
 * The shape is a transcription with the JS receiver dropped and the closures made explicit:
 * upstream's per-field `read`/`write` closures become virtio_cap_field_t entries whose
 * callbacks take a virtio_field_slot_t (the object the io-port map is registered with, io.h
 * D3).  Every function can be read side by side with v86/src/virtio.js.
 */

#include "virtio.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu/cpu.h"      /* unimplemented_instruction */
#include "../cpu/cpu_state.h" /* DBG_ASSERT */
#include "../cpu/memory.h"   /* read16/read32s/write16/write32/write_blob, mem8, in_mapped_range */

/* ------------------------------------------------------------------ *
 * the diagnostic (io.h D1's / ide.c's pattern).  It decides nothing.
 * ------------------------------------------------------------------ */
static bool virtio_log_enabled(void)
{
    static int state = -1;
    if (state < 0) {
        const char *value = getenv("V86C_LOG_VIRTIO");
        state = value != NULL && strcmp(value, "0") != 0;
    }
    return state == 1;
}

static void virtio_logf(const char *fmt, ...)
{
    if (!virtio_log_enabled()) return;
    va_list args;
    va_start(args, fmt);
    fputs("v86c virtio: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

/* lib.js:158-165 -- int_log2 is 31 - clz32(x), asserted x > 0. */
static int32_t virtio_int_log2(uint32_t x)
{
    DBG_ASSERT(x > 0);
    int32_t result = 0;
    while (x > 1) {
        x >>= 1;
        result++;
    }
    return result;
}

#define VIRTIO(device) ((virtio_device_t *)(device))

/* ------------------------------------------------------------------ *
 * cpu.js:313-323 -- `read_blob`: a view of guest RAM.
 *
 * AUDIT-1 S2: upstream's two dbg_asserts are compiled out of the pinned release, and its
 * `mem8.subarray(offset, offset + length)` then CLAMPS -- a descriptor address at or past
 * the end of the wasm view yields an EMPTY view (MEASURED in node: subarray(20, 24) on a
 * 16-byte array is empty; a straddling one is truncated), so no host memory is read and
 * the missing bytes are simply not copied.  This port's mem8 is a fixed C array, so the
 * same descriptor was a host OOB read whose bytes reached the console sink.  The caller
 * still advances by the FULL length (upstream's `dest_offset += read_length` runs on the
 * empty view too), so the dest's gap keeps whatever the caller pre-filled -- upstream's
 * callers allocate zero-filled Uint8Arrays, and console_notify_output now does too.
 * ------------------------------------------------------------------ */
static uint32_t virtio_guest_read_blob(uint32_t offset, uint32_t length, const uint8_t **blob)
{
    if (offset >= cpu->memory_size) {
        *blob = mem8;
        return 0;
    }
    uint32_t available = cpu->memory_size - offset;
    if (available > length) available = length;
    *blob = mem8 + offset;
    return available;
}

/* ------------------------------------------------------------------ *
 * THE COMMON CONFIGURATION FIELDS -- virtio.js:321-603.
 *
 * Upstream's field objects hold closures over the VirtIO instance; here every callback
 * receives the slot (which holds the device).  The READ of a dword-indexed feature word is
 * upstream's `this.device_feature[select] || 0`: an out-of-range select yields undefined,
 * which `|| 0` turns into 0 -- so the C port's bound check is the same answer, not a fix.
 * ------------------------------------------------------------------ */

static int32_t common_device_feature_select_read(virtio_field_slot_t *slot)
{
    return (int32_t)slot->virtio->device_feature_select;
}

static void common_device_feature_select_write(virtio_field_slot_t *slot, int32_t data)
{
    slot->virtio->device_feature_select = (uint32_t)data;
}

static int32_t common_device_feature_read(virtio_field_slot_t *slot)
{
    uint32_t select = slot->virtio->device_feature_select;
    return select < VIRTIO_FEATURE_WORDS ? (int32_t)slot->virtio->device_feature[select] : 0;
}

static void common_device_feature_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

static int32_t common_driver_feature_select_read(virtio_field_slot_t *slot)
{
    return (int32_t)slot->virtio->driver_feature_select;
}

static void common_driver_feature_select_write(virtio_field_slot_t *slot, int32_t data)
{
    slot->virtio->driver_feature_select = (uint32_t)data;
}

static int32_t common_driver_feature_read(virtio_field_slot_t *slot)
{
    uint32_t select = slot->virtio->driver_feature_select;
    return select < VIRTIO_FEATURE_WORDS ? (int32_t)slot->virtio->driver_feature[select] : 0;
}

static void common_driver_feature_write(virtio_field_slot_t *slot, int32_t data)
{
    virtio_device_t *virtio = slot->virtio;
    uint32_t select = virtio->driver_feature_select;
    uint32_t supported_feature = select < VIRTIO_FEATURE_WORDS ? virtio->device_feature[select] : 0;

    if (select < VIRTIO_FEATURE_WORDS) {
        /* Note (upstream's own): only a subset of device_features is stored.  Required for
         * is_feature_negotiated(). */
        virtio->driver_feature[select] = (uint32_t)data & supported_feature;
    }

    /* Check that driver features is an inclusive subset of device features. */
    uint32_t invalid_bits = (uint32_t)data & ~supported_feature;
    virtio->features_ok = virtio->features_ok && invalid_bits == 0;
}

static int32_t common_msix_config_read(virtio_field_slot_t *slot)
{
    (void)slot;
    virtio_logf("No msi-x capability supported.");
    return 0xFFFF;
}

static void common_msix_config_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data;
    virtio_logf("No msi-x capability supported.");
}

static int32_t common_num_queues_read(virtio_field_slot_t *slot)
{
    return slot->virtio->queue_count;
}

static void common_num_queues_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

static int32_t common_device_status_read(virtio_field_slot_t *slot)
{
    return slot->virtio->device_status;
}

static void common_device_status_write(virtio_field_slot_t *slot, int32_t data)
{
    virtio_device_t *virtio = slot->virtio;

    if (data == 0) {
        virtio_logf("Reset device<%s>", virtio->name);
        virtio_reset(virtio);
    }
    else if (data & VIRTIO_STATUS_FAILED) {
        virtio_logf("Warning: Device<%s> status failed", virtio->name);
    }
    else {
        virtio_logf("Device<%s> status: %s%s%s%s%s", virtio->name,
                    (data & VIRTIO_STATUS_ACKNOWLEDGE) ? "ACKNOWLEDGE " : "",
                    (data & VIRTIO_STATUS_DRIVER) ? "DRIVER " : "",
                    (data & VIRTIO_STATUS_DRIVER_OK) ? "DRIVER_OK" : "",
                    (data & VIRTIO_STATUS_FEATURES_OK) ? "FEATURES_OK " : "",
                    (data & VIRTIO_STATUS_DEVICE_NEEDS_RESET) ? "DEVICE_NEEDS_RESET" : "");
    }

    if ((data & ~(int32_t)virtio->device_status & VIRTIO_STATUS_DRIVER_OK) &&
        (virtio->device_status & VIRTIO_STATUS_DEVICE_NEEDS_RESET)) {
        /* We couldn't notify NEEDS_RESET earlier because DRIVER_OK was not set.  Now it has
         * been set, notify now. */
        virtio_notify_config_changes(virtio);
    }

    /* Don't set FEATURES_OK if our device doesn't support requested features. */
    if (!virtio->features_ok) {
        if (data & VIRTIO_STATUS_FEATURES_OK) {
            virtio_logf("Removing FEATURES_OK");
        }
        data &= ~VIRTIO_STATUS_FEATURES_OK;
    }

    virtio->device_status = (uint8_t)data;

    /* UPSTREAM'S on_driver_ok IS UNREACHABLE, and this is measured, not inferred: the
     * condition is `data & ~this.device_status & DRIVER_OK` and the line above it has just
     * assigned `this.device_status = data`, so the expression is `data & ~data`, which is 0
     * for every input (virtio.js:439-444).  The console passes a no-op for on_driver_ok, so
     * nothing is lost here; virtio_balloon's real callback would never run.  Kept verbatim:
     * the port reproduces the reference, and the reference never calls it. */
    if (data & ~(int32_t)virtio->device_status & VIRTIO_STATUS_DRIVER_OK) {
        virtio_logf("Device<%s> on_driver_ok (UNREACHABLE upstream)", virtio->name);
        if (virtio->on_driver_ok != NULL) virtio->on_driver_ok(virtio->device);
    }
}

static int32_t common_config_generation_read(virtio_field_slot_t *slot)
{
    return (int32_t)slot->virtio->config_generation;
}

static void common_config_generation_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

static int32_t common_queue_select_read(virtio_field_slot_t *slot)
{
    return (int32_t)slot->virtio->queue_select;
}

static void common_queue_select_write(virtio_field_slot_t *slot, int32_t data)
{
    virtio_device_t *virtio = slot->virtio;

    virtio->queue_select = (uint32_t)data;

    if (virtio->queue_select < (uint32_t)virtio->queue_count) {
        virtio->queue_selected = &virtio->queues[virtio->queue_select];
    }
    else {
        /* Allow queue_select >= num_queues.  Drivers can then detect that the queue is not
         * available using the fields below. */
        virtio->queue_selected = NULL;
    }
}

static int32_t common_queue_size_read(virtio_field_slot_t *slot)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    return queue != NULL ? (int32_t)queue->size : 0;
}

static void common_queue_size_write(virtio_field_slot_t *slot, int32_t data)
{
    virtio_device_t *virtio = slot->virtio;
    virtqueue_t *queue = virtio->queue_selected;

    if (queue == NULL) return;

    if (data & (data - 1)) {
        virtio_logf("Warning: dev<%s> Given queue size was not a power of 2.  Rounding up to "
                    "next power of 2.",
                    virtio->name);
        /* upstream's `1 << (int_log2(data - 1) + 1)`: JS masks the shift count to 5 bits,
         * and for a 32-bit guest's 0xFFFFFFFF that is 1 << 32 == 1 << 0 -- written with the
         * mask so the C is defined instead of relying on the host's shift behaviour. */
        data = 1 << ((virtio_int_log2((uint32_t)(data - 1)) + 1) & 31);
    }
    if ((uint32_t)data > queue->size_supported) {
        virtio_logf("Warning: dev<%s> Trying to set queue size greater than supported.  "
                    "Clamping to supported size.",
                    virtio->name);
        data = (int32_t)queue->size_supported;
    }
    virtqueue_set_size(queue, (uint32_t)data);
}

static int32_t common_queue_msix_vector_read(virtio_field_slot_t *slot)
{
    (void)slot;
    virtio_logf("No msi-x capability supported.");
    return 0xFFFF;
}

static void common_queue_msix_vector_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data;
    virtio_logf("No msi-x capability supported.");
}

static int32_t common_queue_enable_read(virtio_field_slot_t *slot)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    return queue != NULL ? (queue->enabled ? 1 : 0) : 0;
}

static void common_queue_enable_write(virtio_field_slot_t *slot, int32_t data)
{
    virtio_device_t *virtio = slot->virtio;
    virtqueue_t *queue = virtio->queue_selected;

    if (queue == NULL) return;

    if (data == 1) {
        if (virtqueue_is_configured(queue)) {
            virtqueue_enable(queue);
        }
        else {
            virtio_logf("Driver bug: tried enabling unconfigured queue");
        }
    }
    else if (data == 0) {
        virtio_logf("Driver bug: tried writing 0 to queue_enable");
    }
}

static int32_t common_queue_notify_off_read(virtio_field_slot_t *slot)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    return queue != NULL ? (int32_t)queue->notify_offset : 0;
}

static void common_queue_notify_off_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

/* the six queue address dwords (desc/avail/used, lo and hi).  Upstream's HI arms accept the
 * write and ignore it ("High dword of 64 bit queue_desc ignored") -- a 32-bit guest cannot
 * reach them, and this transport has no 64-bit guests. */

static int32_t common_queue_desc_lo_read(virtio_field_slot_t *slot)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    return queue != NULL ? (int32_t)queue->desc_addr : 0;
}

static void common_queue_desc_lo_write(virtio_field_slot_t *slot, int32_t data)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    if (queue != NULL) queue->desc_addr = (uint32_t)data;
}

static void common_queue_desc_hi_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    if (data != 0) {
        virtio_logf("Warning: High dword of 64 bit queue_desc ignored:%d", data);
    }
}

static int32_t common_queue_avail_lo_read(virtio_field_slot_t *slot)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    return queue != NULL ? (int32_t)queue->avail_addr : 0;
}

static void common_queue_avail_lo_write(virtio_field_slot_t *slot, int32_t data)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    if (queue != NULL) queue->avail_addr = (uint32_t)data;
}

static void common_queue_avail_hi_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    if (data != 0) {
        virtio_logf("Warning: High dword of 64 bit queue_avail ignored:%d", data);
    }
}

static int32_t common_queue_used_lo_read(virtio_field_slot_t *slot)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    return queue != NULL ? (int32_t)queue->used_addr : 0;
}

static void common_queue_used_lo_write(virtio_field_slot_t *slot, int32_t data)
{
    virtqueue_t *queue = slot->virtio->queue_selected;
    if (queue != NULL) queue->used_addr = (uint32_t)data;
}

static void common_queue_used_hi_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    if (data != 0) {
        virtio_logf("Warning: High dword of 64 bit queue_used ignored:%d", data);
    }
}

static const virtio_cap_field_t virtio_common_fields[] = {
    {4, "device_feature_select", common_device_feature_select_read, common_device_feature_select_write},
    {4, "device_feature", common_device_feature_read, common_device_feature_write},
    {4, "driver_feature_select", common_driver_feature_select_read, common_driver_feature_select_write},
    {4, "driver_feature", common_driver_feature_read, common_driver_feature_write},
    {2, "msix_config", common_msix_config_read, common_msix_config_write},
    {2, "num_queues", common_num_queues_read, common_num_queues_write},
    {1, "device_status", common_device_status_read, common_device_status_write},
    {1, "config_generation", common_config_generation_read, common_config_generation_write},
    {2, "queue_select", common_queue_select_read, common_queue_select_write},
    {2, "queue_size", common_queue_size_read, common_queue_size_write},
    {2, "queue_msix_vector", common_queue_msix_vector_read, common_queue_msix_vector_write},
    {2, "queue_enable", common_queue_enable_read, common_queue_enable_write},
    {2, "queue_notify_off", common_queue_notify_off_read, common_queue_notify_off_write},
    {4, "queue_desc (low dword)", common_queue_desc_lo_read, common_queue_desc_lo_write},
    {4, "queue_desc (high dword)", NULL, common_queue_desc_hi_write},
    {4, "queue_avail (low dword)", common_queue_avail_lo_read, common_queue_avail_lo_write},
    {4, "queue_avail (high dword)", NULL, common_queue_avail_hi_write},
    {4, "queue_used (low dword)", common_queue_used_lo_read, common_queue_used_lo_write},
    {4, "queue_used (high dword)", NULL, common_queue_used_hi_write},
};
#define VIRTIO_COMMON_FIELD_COUNT (int32_t)(sizeof(virtio_common_fields) / sizeof(virtio_common_fields[0]))

/* ------------------------------------------------------------------ *
 * THE NOTIFICATION AND ISR FIELDS -- virtio.js:609-683.
 * ------------------------------------------------------------------ */

/* One 16-bit notify register per handler.  The VALUE written is the queue id (upstream's
 * handlers are named `queue_id` for that reason); the handler is selected by the register's
 * OFFSET within the capability, which is the field's index. */
static void notify_field_write(virtio_field_slot_t *slot, int32_t data)
{
    virtio_device_t *virtio = slot->virtio;
    int32_t index = slot->index;

    if (index < 0 || index >= virtio->notify_handler_count) {
        /* upstream's `handler || (data => {})`: an undefined handler does nothing. */
        return;
    }
    virtio_notify_handler_fn handler = virtio->notify_handlers[index];
    if (handler != NULL) handler(virtio->notify_owner, (uint32_t)data);
}

static int32_t notify_field_read(virtio_field_slot_t *slot)
{
    (void)slot;
    return 0xFFFF;
}

static int32_t isr_status_read(virtio_field_slot_t *slot)
{
    virtio_device_t *virtio = slot->virtio;
    uint8_t isr_status = virtio->isr_status;
    virtio_lower_irq(virtio);
    return isr_status;
}

static void isr_status_write(virtio_field_slot_t *slot, int32_t data)
{
    (void)slot;
    (void)data; /* read only */
}

static const virtio_cap_field_t virtio_isr_fields[] = {
    {1, "isr_status", isr_status_read, isr_status_write},
};

/* ------------------------------------------------------------------ *
 * THE IO-PORT CALLBACKS.  io.h D3: a callback takes (device, port) / (device, port, value)
 * and the device pointer is the field slot.  The four shims are upstream's
 * (virtio.js:816-833): "DSL (2.4 kernel) does these reads" and "archhurd does these reads".
 * ------------------------------------------------------------------ */

static int32_t field_read8(void *device, int32_t port)
{
    (void)port;
    virtio_field_slot_t *slot = device;
    return slot->field->read != NULL ? slot->field->read(slot) : 0;
}

static int32_t field_read16(void *device, int32_t port)
{
    (void)port;
    virtio_field_slot_t *slot = device;
    return slot->field->read != NULL ? slot->field->read(slot) : 0;
}

static int32_t field_read32(void *device, int32_t port)
{
    (void)port;
    virtio_field_slot_t *slot = device;
    return slot->field->read != NULL ? slot->field->read(slot) : 0;
}

static int32_t field_read8_on_16(void *device, int32_t port)
{
    virtio_logf("Warning: 8-bit read from 16-bit virtio port");
    return (int32_t)(((uint32_t)field_read16(device, port & ~1) >> ((port & 1) << 3)) & 0xFF);
}

static int32_t field_read8_on_32(void *device, int32_t port)
{
    virtio_logf("Warning: 8-bit read from 32-bit virtio port");
    return (int32_t)(((uint32_t)field_read32(device, port & ~3) >> ((port & 3) << 3)) & 0xFF);
}

static int32_t field_read32_on_16(void *device, int32_t port)
{
    virtio_logf("Warning: 32-bit read from 16-bit virtio port");
    return field_read16(device, port);
}

static void field_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    virtio_field_slot_t *slot = device;
    if (slot->field->write != NULL) slot->field->write(slot, value);
}

static void field_write16(void *device, int32_t port, int32_t value)
{
    (void)port;
    virtio_field_slot_t *slot = device;
    if (slot->field->write != NULL) slot->field->write(slot, value);
}

static void field_write32(void *device, int32_t port, int32_t value)
{
    (void)port;
    virtio_field_slot_t *slot = device;
    if (slot->field->write != NULL) slot->field->write(slot, value);
}

/* ------------------------------------------------------------------ *
 * THE CAPABILITIES -- virtio.js:317-910.
 *
 * virtio_capability_info_t is upstream's VirtIO_CapabilityInfo typedef: the type, the bar,
 * the port, whether it is mmio, the offset within the bar, the extra bytes that follow the
 * 16-byte vendor capability, and the field list that becomes the io-port registrations.
 * ------------------------------------------------------------------ */

typedef struct virtio_capability_info {
    int32_t type;
    int32_t bar;
    int32_t port;
    bool use_mmio;
    int32_t offset;
    const uint8_t *extra;
    int32_t extra_len;
    const virtio_cap_field_t *fields;
    int32_t field_count;
    void *owner;
} virtio_capability_info_t;

static virtio_capability_info_t create_common_capability(const virtio_common_options_t *options,
                                                         virtio_device_t *virtio)
{
    virtio_capability_info_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.type = VIRTIO_PCI_CAP_COMMON_CFG;
    cap.bar = 0;
    cap.port = options->initial_port;
    cap.use_mmio = false;
    cap.offset = 0;
    cap.extra = NULL;
    cap.extra_len = 0;
    cap.fields = virtio_common_fields;
    cap.field_count = VIRTIO_COMMON_FIELD_COUNT;
    cap.owner = virtio;
    return cap;
}

/* virtio.js:627-636 names each one "notify" + its index. */
static const char *const notify_field_names[] = {
    "notify0", "notify1", "notify2", "notify3", "notify4", "notify5", "notify6", "notify7",
    "notify8", "notify9", "notify10", "notify11", "notify12", "notify13", "notify14",
    "notify15",
};

/* The notify_off_multiplier, as the four little-endian extra bytes (virtio.js:609-653).
 * The fields are written into the DEVICE's own array (see virtio.h's note): the slots keep
 * pointers to them for the machine's lifetime. */
static virtio_capability_info_t create_notification_capability(virtio_device_t *virtio,
                                                               const virtio_notification_options_t *options,
                                                               uint8_t extra[4])
{
    uint32_t notify_off_multiplier;
    virtio_cap_field_t *fields = virtio->notify_fields;

    if (options->single_handler) {
        DBG_ASSERT(options->handler_count == 1);
        notify_off_multiplier = 0; /* forces all queues to use the same address */
    }
    else {
        notify_off_multiplier = 2;
    }

    DBG_ASSERT(options->handler_count <= VIRTIO_MAX_QUEUES);
    for (int32_t i = 0; i < options->handler_count; i++) {
        fields[i].bytes = 2;
        fields[i].name = notify_field_names[i];
        fields[i].read = notify_field_read;
        fields[i].write = notify_field_write;
    }

    extra[0] = (uint8_t)(notify_off_multiplier & 0xFF);
    extra[1] = (uint8_t)((notify_off_multiplier >> 8) & 0xFF);
    extra[2] = (uint8_t)((notify_off_multiplier >> 16) & 0xFF);
    extra[3] = (uint8_t)(notify_off_multiplier >> 24);

    virtio_capability_info_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.type = VIRTIO_PCI_CAP_NOTIFY_CFG;
    cap.bar = 1;
    cap.port = options->initial_port;
    cap.use_mmio = false;
    cap.offset = 0;
    cap.extra = extra;
    cap.extra_len = 4;
    cap.fields = fields;
    cap.field_count = options->handler_count;
    cap.owner = virtio;
    return cap;
}

static virtio_capability_info_t create_isr_capability(const virtio_isr_options_t *options,
                                                      virtio_device_t *virtio)
{
    virtio_capability_info_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.type = VIRTIO_PCI_CAP_ISR_CFG;
    cap.bar = 2;
    cap.port = options->initial_port;
    cap.use_mmio = false;
    cap.offset = 0;
    cap.extra = NULL;
    cap.extra_len = 0;
    cap.fields = virtio_isr_fields;
    cap.field_count = 1;
    cap.owner = virtio;
    return cap;
}

static virtio_capability_info_t create_device_specific_capability(const virtio_device_specific_options_t *options)
{
    /* UPSTREAM'S ASSERTION HERE IS DEAD, and this is measured, not inferred:
     * `dbg_assert(~options.offset & 0x3, "device specific cap offset must be 4-byte
     * aligned")` reads a member its own typedef does not have (VirtIO_DeviceSpecificCapability
     * Options is {initial_port, struct}), so the expression is `~undefined & 3` = 0 -- the
     * assertion FAILS in any debug build and is compiled out of the pinned release.  The port
     * keeps the capability at offset 0 (which is what upstream's init_capabilities then uses)
     * and asserts that, because a wrong offset would move the device's config ports. */
    const int32_t offset = 0;
    DBG_ASSERT((offset & 0x3) == 0);

    virtio_capability_info_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.type = VIRTIO_PCI_CAP_DEVICE_CFG;
    cap.bar = 3;
    cap.port = options->initial_port;
    cap.use_mmio = false;
    cap.offset = 0;
    cap.extra = NULL;
    cap.extra_len = 0;
    cap.fields = options->fields;
    cap.field_count = options->field_count;
    cap.owner = options->owner;
    return cap;
}

static virtio_field_slot_t *virtio_alloc_slot(virtio_device_t *virtio,
                                              const virtio_cap_field_t *field, int32_t index,
                                              void *owner)
{
    if (virtio->slot_count >= VIRTIO_FIELD_SLOT_MAX) {
        fprintf(stderr,
                "v86c: virtio<%s>: more than %d capability fields (this port's bound; "
                "upstream has none)\n",
                virtio->name, VIRTIO_FIELD_SLOT_MAX);
        exit(1);
    }
    virtio_field_slot_t *slot = &virtio->slots[virtio->slot_count++];
    slot->virtio = virtio;
    slot->owner = owner;
    slot->field = field;
    slot->index = index;
    return slot;
}

/* virtio.js:710-910.  Call only within the constructor. */
static void init_capabilities(virtio_device_t *virtio, const virtio_capability_info_t *capabilities,
                              int32_t capability_count)
{
    /* Next available offset for capabilities linked list. */
    int32_t cap_next = 0x40;
    virtio->pci_space[0x34] = 0x40;
    int32_t cap_ptr = cap_next;

    for (int32_t c = 0; c < capability_count; c++) {
        const virtio_capability_info_t *cap = &capabilities[c];
        int32_t cap_len = VIRTIO_PCI_CAP_LENGTH + cap->extra_len;

        cap_ptr = cap_next;
        cap_next = cap_ptr + cap_len;

        DBG_ASSERT(cap_next <= 256);
        DBG_ASSERT(cap->bar >= 0 && cap->bar < 6);

        int32_t bar_size = cap->offset;
        for (int32_t f = 0; f < cap->field_count; f++) {
            bar_size += cap->fields[f].bytes;
        }
        /* Round up to next power of 2, minimum 16 bytes. */
        bar_size = bar_size < 16 ? 16 : 1 << (virtio_int_log2((uint32_t)(bar_size - 1)) + 1);

        DBG_ASSERT((cap->port & (bar_size - 1)) == 0);

        virtio->pci_bars[cap->bar].size = (uint32_t)bar_size;
        /* upstream's `this.pci_bars[cap.bar] = {size}` makes the JS array as long as the
         * highest bar index used; pci.js then iterates that length and skips the holes. */
        if (cap->bar + 1 > virtio->pci_bars_count) virtio->pci_bars_count = cap->bar + 1;

        virtio->pci_space[cap_ptr] = VIRTIO_PCI_CAP_VENDOR;
        virtio->pci_space[cap_ptr + 1] = (uint8_t)cap_next;
        virtio->pci_space[cap_ptr + 2] = (uint8_t)cap_len;
        virtio->pci_space[cap_ptr + 3] = (uint8_t)cap->type;
        virtio->pci_space[cap_ptr + 4] = (uint8_t)cap->bar;

        virtio->pci_space[cap_ptr + 5] = 0; /* Padding. */
        virtio->pci_space[cap_ptr + 6] = 0; /* Padding. */
        virtio->pci_space[cap_ptr + 7] = 0; /* Padding. */

        virtio->pci_space[cap_ptr + 8] = (uint8_t)(cap->offset & 0xFF);
        virtio->pci_space[cap_ptr + 9] = (uint8_t)((cap->offset >> 8) & 0xFF);
        virtio->pci_space[cap_ptr + 10] = (uint8_t)((cap->offset >> 16) & 0xFF);
        virtio->pci_space[cap_ptr + 11] = (uint8_t)(cap->offset >> 24);

        virtio->pci_space[cap_ptr + 12] = (uint8_t)(bar_size & 0xFF);
        virtio->pci_space[cap_ptr + 13] = (uint8_t)((bar_size >> 8) & 0xFF);
        virtio->pci_space[cap_ptr + 14] = (uint8_t)((bar_size >> 16) & 0xFF);
        virtio->pci_space[cap_ptr + 15] = (uint8_t)(bar_size >> 24);

        for (int32_t i = 0; i < cap->extra_len; i++) {
            virtio->pci_space[cap_ptr + 16 + i] = cap->extra[i];
        }

        int32_t bar_offset = 0x10 + 4 * cap->bar;
        virtio->pci_space[bar_offset] = (uint8_t)((cap->port & 0xFE) | !cap->use_mmio);
        virtio->pci_space[bar_offset + 1] = (uint8_t)((cap->port >> 8) & 0xFF);
        virtio->pci_space[bar_offset + 2] = (uint8_t)((cap->port >> 16) & 0xFF);
        virtio->pci_space[bar_offset + 3] = (uint8_t)((cap->port >> 24) & 0xFF);

        int32_t port = cap->port + cap->offset;

        for (int32_t f = 0; f < cap->field_count; f++) {
            const virtio_cap_field_t *field = &cap->fields[f];
            virtio_field_slot_t *slot = virtio_alloc_slot(virtio, field, f, cap->owner);

            if (cap->use_mmio) {
                /* upstream: dbg_assert(false, "... mmio capability not implemented.") */
                fprintf(stderr,
                        "v86c: virtio<%s>: mmio capability not implemented (upstream asserts "
                        "this too); refusing to register a half-ported capability\n",
                        virtio->name);
                exit(1);
            }

            switch (field->bytes) {
            case 4:
                io_register_read(port, slot, field_read8_on_32, NULL, field_read32);
                io_register_read(port + 1, slot, field_read8_on_32, NULL, NULL);
                io_register_read(port + 2, slot, field_read8_on_32, NULL, NULL);
                io_register_read(port + 3, slot, field_read8_on_32, NULL, NULL);
                io_register_write(port, slot, NULL, NULL, field_write32);
                break;
            case 2:
                io_register_read(port, slot, field_read8_on_16, field_read16, field_read32_on_16);
                io_register_read(port + 1, slot, field_read8_on_16, NULL, NULL);
                io_register_write(port, slot, NULL, field_write16, NULL);
                break;
            case 1:
                io_register_read(port, slot, field_read8, NULL, NULL);
                io_register_write(port, slot, field_write8, NULL, NULL);
                break;
            default:
                /* upstream: dbg_assert(false, "... invalid capability field width") */
                fprintf(stderr, "v86c: virtio<%s>: invalid capability field width %u (%s)\n",
                        virtio->name, (unsigned)field->bytes, field->name);
                exit(1);
            }

            port += field->bytes;
        }
    }

    /* Terminate the linked list with the pci config access capability (virtio.js:865-908).
     * Upstream writes its 20 bytes and leaves the fields to the driver -- with its own TODO
     * saying the capability "is required by spec, but so far, devices seem to work well
     * without it" (no cfg_data window is implemented; that is upstream's deferral, kept). */
    int32_t cap_len = VIRTIO_PCI_CAP_LENGTH + 4;
    DBG_ASSERT(cap_next + cap_len <= 256);
    virtio->pci_space[cap_next] = VIRTIO_PCI_CAP_VENDOR;
    virtio->pci_space[cap_next + 1] = 0; /* cap next (null terminator) */
    virtio->pci_space[cap_next + 2] = (uint8_t)cap_len;
    virtio->pci_space[cap_next + 3] = VIRTIO_PCI_CAP_PCI_CFG;
    for (int32_t i = 4; i < cap_len; i++) {
        virtio->pci_space[cap_next + i] = 0;
    }
}

/* ------------------------------------------------------------------ *
 * THE TRANSPORT -- virtio.js:162-1041.
 * ------------------------------------------------------------------ */

bool virtio_init(virtio_device_t *virtio, const virtio_options_t *options)
{
    memset(virtio, 0, sizeof(*virtio));

    if (options->common.queue_count <= 0 || options->common.queue_count > VIRTIO_MAX_QUEUES) {
        fprintf(stderr, "v86c: virtio<%s>: %d queues is outside this port's bound 1..%d\n",
                options->name, (int)options->common.queue_count, VIRTIO_MAX_QUEUES);
        return false;
    }

    virtio->name = options->name;
    virtio->pci_id = options->pci_id;
    virtio->device_id = options->device_id;
    virtio->device = options->device;
    virtio->notify_handlers = options->notification.handlers;
    virtio->notify_handler_count = options->notification.handler_count;
    virtio->notify_owner = options->notification.owner;
    virtio->on_driver_ok = options->common.on_driver_ok;

    /* virtio.js:174-233 -- the config space, byte for byte, then padded to 256. */
    {
        uint8_t *s = virtio->pci_space;
        s[0] = VIRTIO_PCI_VENDOR_ID & 0xFF;
        s[1] = VIRTIO_PCI_VENDOR_ID >> 8;
        s[2] = (uint8_t)(options->device_id & 0xFF);
        s[3] = (uint8_t)(options->device_id >> 8);
        s[4] = 0x07; /* Command */
        s[5] = 0x05;
        s[6] = 0x10; /* Status -- enable capabilities list and... see pci.js's own note */
        s[7] = 0x00;
        s[8] = 0x01; /* Revision ID */
        s[9] = 0x00; /* Prof IF */
        s[10] = 0x02; /* Subclass */
        s[11] = 0x00; /* Class code */
        s[12] = 0x00; /* Cache line size */
        s[13] = 0x00; /* Latency Timer */
        s[14] = 0x00; /* Header Type */
        s[15] = 0x00; /* Built-in self test */
        s[16] = 0x01; /* BAR0 -- filled in by init_capabilities */
        s[17] = 0xa8;
        s[18] = 0x00;
        s[19] = 0x00;
        s[20] = 0x00; /* BAR1 */
        s[21] = 0x10;
        s[22] = 0xbf;
        s[23] = 0xfe;
        /* BAR2..BAR5, the CardBus CIS pointer and the expansion ROM address stay zero. */
        s[44] = VIRTIO_PCI_VENDOR_ID & 0xFF; /* Subsystem vendor ID */
        s[45] = VIRTIO_PCI_VENDOR_ID >> 8;
        s[46] = (uint8_t)(options->subsystem_device_id & 0xFF); /* Subsystem ID */
        s[47] = (uint8_t)(options->subsystem_device_id >> 8);
        s[52] = 0x40; /* Capabilities pointer */
        s[60] = 0x00; /* Interrupt line */
        s[61] = 0x01; /* Interrupt pin */
    }
    virtio->pci_space_length = 256;

    virtio->device_feature_select = 0;
    virtio->driver_feature_select = 0;

    for (int32_t i = 0; i < options->common.feature_count; i++) {
        uint32_t f = options->common.features[i];
        DBG_ASSERT(f < 128); /* upstream's own assumption, virtio.js:254-256 */

        /* Feature bits are grouped in 32 bits. */
        virtio->device_feature[f >> 5] |= 1u << (f & 0x1F);
        virtio->driver_feature[f >> 5] |= 1u << (f & 0x1F);
    }

    /* UPSTREAM'S OWN PRECONDITION, and the reason virtio.h calls this MODERN virtio:
     * "only non-transitional devices are supported" (virtio.js:262-263). */
    {
        bool has_version_1 = false;
        for (int32_t i = 0; i < options->common.feature_count; i++) {
            if (options->common.features[i] == VIRTIO_F_VERSION_1) has_version_1 = true;
        }
        DBG_ASSERT(has_version_1);
    }

    virtio->features_ok = true;
    virtio->device_status = 0;
    virtio->config_has_changed = false;
    virtio->config_generation = 0;

    virtio->queue_count = options->common.queue_count;
    for (int32_t i = 0; i < virtio->queue_count; i++) {
        virtqueue_init(&virtio->queues[i], virtio, &options->common.queues[i]);
    }
    virtio->queue_select = 0;
    virtio->queue_selected = &virtio->queues[0];

    virtio->isr_status = 0;

    /* virtio.js:284-300 -- upstream's DEBUG-only consistency check of the notification
     * options: every queue's effective notify offset must have a handler, and no handler may
     * be unused.  Kept as a construction-time check (it cannot be reached by a guest). */
    {
        bool offsets[VIRTIO_MAX_QUEUES];
        memset(offsets, 0, sizeof(offsets));
        for (int32_t i = 0; i < virtio->queue_count; i++) {
            uint32_t effective =
                options->notification.single_handler ? 0 : virtio->queues[i].notify_offset;
            DBG_ASSERT(effective < (uint32_t)VIRTIO_MAX_QUEUES);
            offsets[effective] = true;
            DBG_ASSERT(options->notification.handlers[effective] != NULL);
        }
        for (int32_t i = 0; i < options->notification.handler_count; i++) {
            DBG_ASSERT(options->notification.handlers[i] == NULL || offsets[i]);
        }
    }

    /* virtio.js:302-313 */
    {
        virtio_capability_info_t capabilities[4];
        uint8_t notify_extra[4];
        int32_t count = 0;

        capabilities[count++] = create_common_capability(&options->common, virtio);
        capabilities[count++] = create_notification_capability(virtio, &options->notification,
                                                              notify_extra);
        capabilities[count++] = create_isr_capability(&options->isr_status, virtio);
        if (options->device_specific != NULL) {
            capabilities[count++] = create_device_specific_capability(options->device_specific);
        }
        init_capabilities(virtio, capabilities, count);
    }

    /* virtio.js:313-314: `cpu.devices.pci.register_device(this); this.reset();` */
    virtio->pci_device.pci_id = virtio->pci_id;
    virtio->pci_device.pci_space = virtio->pci_space;
    virtio->pci_device.pci_space_length = virtio->pci_space_length;
    virtio->pci_device.pci_bars = virtio->pci_bars;
    virtio->pci_device.pci_bars_count = virtio->pci_bars_count;
    virtio->pci_device.name = virtio->name;
    virtio->pci_device.pci_rom_size = 0;
    virtio->pci_device.pci_rom_address = 0;
    pci_register_device(&virtio->pci_device);

    virtio_reset(virtio);
    return true;
}

/* virtio.js:952-973 */
void virtio_reset(virtio_device_t *virtio)
{
    virtio->device_feature_select = 0;
    virtio->driver_feature_select = 0;
    memcpy(virtio->driver_feature, virtio->device_feature, sizeof(virtio->driver_feature));

    virtio->features_ok = true;
    virtio->device_status = 0;

    virtio->queue_select = 0;
    virtio->queue_selected = &virtio->queues[0];

    for (int32_t i = 0; i < virtio->queue_count; i++) {
        virtqueue_reset(&virtio->queues[i]);
    }

    virtio->config_has_changed = false;
    virtio->config_generation = 0;

    virtio_lower_irq(virtio);
}

/* virtio.js:979-992 */
void virtio_notify_config_changes(virtio_device_t *virtio)
{
    virtio->config_has_changed = true;

    if (virtio->device_status & VIRTIO_STATUS_DRIVER_OK) {
        virtio_raise_irq(virtio, VIRTIO_ISR_DEVICE_CFG);
    }
    else {
        DBG_ASSERT(false); /* upstream: "attempted to notify driver before DRIVER_OK" */
    }
}

/* virtio.js:997-1005 */
void virtio_update_config_generation(virtio_device_t *virtio)
{
    if (virtio->config_has_changed) {
        virtio->config_generation++;
        virtio->config_generation &= 0xFF;
        virtio->config_has_changed = false;
    }
}

/* virtio.js:1007-1012 */
bool virtio_is_feature_negotiated(virtio_device_t *virtio, uint32_t feature)
{
    return (virtio->driver_feature[feature >> 5] & (1u << (feature & 0x1F))) > 0;
}

/* virtio.js:1018-1027 */
void virtio_needs_reset(virtio_device_t *virtio)
{
    virtio_logf("Device<%s> experienced error - requires reset", virtio->name);
    virtio->device_status |= VIRTIO_STATUS_DEVICE_NEEDS_RESET;

    if (virtio->device_status & VIRTIO_STATUS_DRIVER_OK) {
        virtio_notify_config_changes(virtio);
    }
}

/* virtio.js:1029-1041 */
void virtio_raise_irq(virtio_device_t *virtio, uint32_t type)
{
    virtio_logf("Raise irq 0x%x", (unsigned)type);
    virtio->isr_status |= (uint8_t)type;
    pci_raise_irq(virtio->pci_id);
}

void virtio_lower_irq(virtio_device_t *virtio)
{
    virtio_logf("Lower irq");
    virtio->isr_status = 0;
    pci_lower_irq(virtio->pci_id);
}

/* ------------------------------------------------------------------ *
 * THE VIRTQUEUE -- virtio.js:1048-1517.
 * ------------------------------------------------------------------ */

static uint16_t virtqueue_avail_get_flags(virtqueue_t *queue)
{
    return (uint16_t)read16(queue->avail_addr);
}

static uint16_t virtqueue_avail_get_idx(virtqueue_t *queue)
{
    return (uint16_t)read16(queue->avail_addr + 2);
}

static uint16_t virtqueue_avail_get_entry(virtqueue_t *queue, uint32_t i)
{
    return (uint16_t)read16(queue->avail_addr + 4 + VIRTQ_AVAIL_ENTRYSIZE * (i & queue->mask));
}

static uint16_t virtqueue_avail_get_used_event(virtqueue_t *queue)
{
    return (uint16_t)read16(queue->avail_addr + 4 + VIRTQ_AVAIL_ENTRYSIZE * queue->size);
}

static uint16_t virtqueue_used_get_idx(virtqueue_t *queue)
{
    return (uint16_t)read16(queue->used_addr + 2);
}

static void virtqueue_used_set_idx(virtqueue_t *queue, uint16_t value)
{
    write16(queue->used_addr + 2, value);
}

static void virtqueue_used_set_entry(virtqueue_t *queue, uint32_t i, uint32_t desc_idx,
                                     uint32_t length_written)
{
    write32(queue->used_addr + 4 + VIRTQ_USED_ENTRYSIZE * i, (int32_t)desc_idx);
    write32(queue->used_addr + 8 + VIRTQ_USED_ENTRYSIZE * i, (int32_t)length_written);
}

static void virtqueue_used_set_avail_event(virtqueue_t *queue, uint16_t value)
{
    write16(queue->used_addr + 4 + VIRTQ_USED_ENTRYSIZE * queue->size, value);
}

/* virtio.js:1268-1277 */
static void virtqueue_get_descriptor(virtqueue_t *queue, uint32_t table_address, uint32_t i,
                                     virtq_desc_t *out)
{
    (void)queue; /* upstream's method reads only through cpu.read32s/read16 */
    uint32_t base = table_address + i * VIRTQ_DESC_ENTRYSIZE;
    out->addr_low = (uint32_t)read32s(base);
    out->addr_high = (uint32_t)read32s(base + 4);
    out->len = (uint32_t)read32s(base + 8);
    out->flags = (uint16_t)read16(base + 12);
    out->next = (uint16_t)read16(base + 14);
}

void virtqueue_init(virtqueue_t *queue, virtio_device_t *virtio,
                    const virtio_queue_options_t *options)
{
    memset(queue, 0, sizeof(*queue));
    queue->virtio = virtio;
    queue->size = options->size_supported;
    queue->size_supported = options->size_supported;
    queue->mask = queue->size - 1;
    queue->enabled = false;
    queue->notify_offset = options->notify_offset;
    queue->desc_addr = 0;
    queue->avail_addr = 0;
    queue->avail_last_idx = 0;
    queue->used_addr = 0;
    queue->num_staged_replies = 0;
    queue->chain.queue = queue;

    virtqueue_reset(queue);
}

/* virtio.js:1108-1117 */
void virtqueue_reset(virtqueue_t *queue)
{
    queue->enabled = false;
    queue->desc_addr = 0;
    queue->avail_addr = 0;
    queue->avail_last_idx = 0;
    queue->used_addr = 0;
    queue->num_staged_replies = 0;
    virtqueue_set_size(queue, queue->size_supported);
}

/* virtio.js:1119-1122 */
bool virtqueue_is_configured(virtqueue_t *queue)
{
    return queue->desc_addr && queue->avail_addr && queue->used_addr;
}

/* virtio.js:1124-1128 */
void virtqueue_enable(virtqueue_t *queue)
{
    /* AUDIT-1 S1 sweep verdict: NOT guest-reachable -- the field layer guards it
     * (common_queue_enable_write only calls this on a configured queue), so the
     * transcribed assert is a genuine internal contract and stays. */
    DBG_ASSERT(virtqueue_is_configured(queue)); /* "must be configured before enabled" */
    queue->enabled = true;
}

/* virtio.js:1130-1136 */
void virtqueue_set_size(virtqueue_t *queue, uint32_t size)
{
    /* AUDIT-1 S1 sweep verdict: NOT guest-reachable -- common_queue_size_write rounds a
     * non-power-of-2 up and clamps to size_supported BEFORE calling this, and every other
     * caller passes the queue's own size_supported.  The transcribed asserts are internal
     * contracts and stay. */
    DBG_ASSERT((size & (size - 1)) == 0); /* power of 2 or zero */
    DBG_ASSERT(size <= queue->size_supported);
    queue->size = size;
    queue->mask = size - 1;
}

/* virtio.js:1141-1149 */
uint32_t virtqueue_count_requests(virtqueue_t *queue)
{
    DBG_ASSERT(queue->avail_addr != 0);

    if (queue->fix_wrapping) {
        /* Only reachable through upstream's set_state, which this port does not have: the
         * field's honest current value is false.  Ported because upstream's count does it. */
        queue->fix_wrapping = false;
        queue->avail_last_idx = (virtqueue_avail_get_idx(queue) & ~queue->mask) +
                                (queue->avail_last_idx & queue->mask);
    }

    return (virtqueue_avail_get_idx(queue) - queue->avail_last_idx) & 0xFFFF;
}

/* virtio.js:1154-1158 */
bool virtqueue_has_request(virtqueue_t *queue)
{
    DBG_ASSERT(queue->avail_addr != 0);
    return virtqueue_count_requests(queue) != 0;
}

/* virtio.js:1341-1431 -- the chain traversal, upstream's VirtQueueBufferChain constructor.
 * `head_idx` is the descriptor index the avail ring named. */
static void virtq_chain_build(virtq_chain_t *chain, virtqueue_t *queue, uint16_t head_idx)
{
    chain->head_idx = head_idx;
    chain->read_buffer_count = 0;
    chain->read_buffer_idx = 0;
    chain->read_buffer_offset = 0;
    chain->length_readable = 0;
    chain->write_buffer_count = 0;
    chain->write_buffer_idx = 0;
    chain->write_buffer_offset = 0;
    chain->length_written = 0;
    chain->length_writable = 0;

    uint32_t table_address = queue->desc_addr;
    uint32_t desc_idx = head_idx;
    uint32_t chain_length = 0;
    uint32_t chain_max = queue->size;
    bool writable_region = false;
    bool has_indirect_feature = virtio_is_feature_negotiated(queue->virtio, VIRTIO_F_RING_INDIRECT_DESC);

    do {
        virtq_desc_t desc;
        virtqueue_get_descriptor(queue, table_address, desc_idx, &desc);

        virtio_logf("descriptor: idx=%u addr=%08x:%08x len=%08x flags=%04x next=%04x",
                    (unsigned)desc_idx, (unsigned)desc.addr_high, (unsigned)desc.addr_low,
                    (unsigned)desc.len, (unsigned)desc.flags, (unsigned)desc.next);

        if (has_indirect_feature && (desc.flags & VIRTQ_DESC_F_INDIRECT)) {
            if (desc.flags & VIRTQ_DESC_F_NEXT) {
                virtio_logf("Driver bug: VIRTQ_DESC_F_NEXT set in an indirect table descriptor");
            }
            /* Carry on using the indirect table, starting at its first entry. */
            table_address = desc.addr_low;
            desc_idx = 0;
            chain_length = 0;
            chain_max = desc.len / VIRTQ_DESC_ENTRYSIZE;
            continue;
        }

        if (chain_length >= VIRTIO_MAX_CHAIN) {
            /* Not reachable for the devices in this port (see virtio.h's
             * VIRTIO_MAX_CHAIN): a bound with a named stop rather than an overrun. */
            char name[180];
            snprintf(name, sizeof(name),
                     "virtio<%s>: descriptor chain longer than this port's bound of %d "
                     "(queue size %u, chain_max %u)",
                     queue->virtio->name, VIRTIO_MAX_CHAIN, (unsigned)queue->size,
                     (unsigned)chain_max);
            unimplemented_instruction(name);
            break;
        }

        if (desc.flags & VIRTQ_DESC_F_WRITE) {
            writable_region = true;
            chain->write_buffers[chain->write_buffer_count++] = desc;
            chain->length_writable += desc.len;
        }
        else {
            if (writable_region) {
                virtio_logf("Driver bug: readonly buffer after writeonly buffer within chain");
                break;
            }
            chain->read_buffers[chain->read_buffer_count++] = desc;
            chain->length_readable += desc.len;
        }

        chain_length++;
        if (chain_length > chain_max) {
            virtio_logf("Driver bug: descriptor chain cycle detected");
            break;
        }

        if (desc.flags & VIRTQ_DESC_F_NEXT) {
            desc_idx = desc.next;
        }
        else {
            break;
        }
    } while (true);
}

/* virtio.js:1163-1177 */
virtq_chain_t *virtqueue_pop_request(virtqueue_t *queue)
{
    DBG_ASSERT(queue->avail_addr != 0);
    DBG_ASSERT(virtqueue_has_request(queue)); /* "must not pop nonexistent request" */

    uint16_t desc_idx = virtqueue_avail_get_entry(queue, queue->avail_last_idx);
    virtio_logf("Pop request: avail_last_idx=%u desc_idx=%u", (unsigned)queue->avail_last_idx,
                (unsigned)desc_idx);

    virtq_chain_t *chain = &queue->chain;
    virtq_chain_build(chain, queue, desc_idx);

    queue->avail_last_idx = (queue->avail_last_idx + 1) & 0xFFFF;
    return chain;
}

/* virtio.js:1185-1196 */
void virtqueue_push_reply(virtqueue_t *queue, virtq_chain_t *chain)
{
    DBG_ASSERT(queue->used_addr != 0);
    /* AUDIT-1 S1: upstream's second assert here is compiled out of the pinned release and
     * IS guest-reachable: the guest writes queue_size = 0 through the common-config field
     * AFTER configuring (common_queue_size_write clamps only the upper bound) and then
     * posts a buffer -- the first staged reply fails `0 < 0` and aborted the process.
     * The release proceeds: used_set_entry's write is a router-bounded write at the
     * configured used_addr, and num_staged_replies is bounded by the pop/push pairing in
     * the callers (one reply per popped chain, flushed per send). */
    uint32_t used_idx = (virtqueue_used_get_idx(queue) + queue->num_staged_replies) & queue->mask;
    virtio_logf("Push reply: used_idx=%u desc_idx=%u", (unsigned)used_idx,
                (unsigned)chain->head_idx);

    virtqueue_used_set_entry(queue, used_idx, chain->head_idx, chain->length_written);
    queue->num_staged_replies++;
}

/* virtio.js:1202-1246 */
void virtqueue_flush_replies(virtqueue_t *queue)
{
    DBG_ASSERT(queue->used_addr != 0);

    if (queue->num_staged_replies == 0) {
        virtio_logf("flush_replies: Nothing to flush");
        return;
    }

    virtio_logf("Flushing %u replies", (unsigned)queue->num_staged_replies);
    uint16_t old_idx = virtqueue_used_get_idx(queue);
    uint16_t new_idx = (uint16_t)((old_idx + queue->num_staged_replies) & VIRTQ_IDX_MASK);
    virtqueue_used_set_idx(queue, new_idx);

    queue->num_staged_replies = 0;

    if (virtio_is_feature_negotiated(queue->virtio, VIRTIO_F_RING_EVENT_IDX)) {
        uint16_t used_event = virtqueue_avail_get_used_event(queue);

        /* Fire irq when the idx values associated with the pushed reply buffers have reached
         * or gone past used_event.  UPSTREAM COMPUTES THIS AND THEN DOES NOT USE IT -- the
         * condition is commented out in the source ("Workaround for sometimes loading from
         * the filesystem hangs and the emulator stays idle", virtio.js:1233-1237) and the
         * IRQ is raised unconditionally.  Transcribed as it stands; the value is kept
         * because it is what the commented-out line reads. */
        bool has_passed = old_idx <= used_event && used_event < new_idx;
        if (new_idx <= old_idx) {
            has_passed = used_event < new_idx || old_idx <= used_event;
        }
        (void)has_passed;

        virtio_raise_irq(queue->virtio, VIRTIO_ISR_QUEUE);
    }
    else {
        if (~virtqueue_avail_get_flags(queue) & VIRTQ_AVAIL_F_NO_INTERRUPT) {
            virtio_raise_irq(queue->virtio, VIRTIO_ISR_QUEUE);
        }
    }
}

/* virtio.js:1255-1262 */
void virtqueue_notify_me_after(virtqueue_t *queue, uint32_t num_skipped_requests)
{
    /* upstream's `dbg_assert(num_skipped_requests >= 0)` cannot fail for an unsigned
     * parameter, and a check that cannot fail is not a check (M1a-2's rule), so it is not
     * reproduced -- the parameter's own type is the constraint. */

    uint16_t avail_event = (uint16_t)((virtqueue_avail_get_idx(queue) + num_skipped_requests) & 0xFFFF);
    virtqueue_used_set_avail_event(queue, avail_event);
}

/* ------------------------------------------------------------------ *
 * THE BUFFER CHAIN -- virtio.js:1438-1517.
 *
 * Upstream's get_next_blob fills a Uint8Array the caller allocated; the C port fills a
 * caller-provided byte buffer and returns the count, which is what every call site up there
 * does with it (`new Uint8Array(bufchain.length_readable)` followed by the call).
 * ------------------------------------------------------------------ */

static uint64_t chain_bytes_read_total;
static uint64_t chain_bytes_written_total;

uint64_t virtq_chain_bytes_read(void) { return chain_bytes_read_total; }
uint64_t virtq_chain_bytes_written(void) { return chain_bytes_written_total; }

uint32_t virtq_chain_get_next_blob(virtq_chain_t *chain, uint8_t *dest_buffer, uint32_t length)
{
    uint32_t dest_offset = 0;
    uint32_t remaining = length;

    while (remaining) {
        if (chain->read_buffer_idx == chain->read_buffer_count) {
            virtio_logf("Device<%s> Read more than device-readable buffers has",
                        chain->queue->virtio->name);
            break;
        }

        const virtq_desc_t *buf = &chain->read_buffers[chain->read_buffer_idx];
        uint32_t read_address = buf->addr_low + chain->read_buffer_offset;
        uint32_t read_length = buf->len - chain->read_buffer_offset;

        if (read_length > remaining) {
            read_length = remaining;
            chain->read_buffer_offset += remaining;
        }
        else {
            chain->read_buffer_idx++;
            chain->read_buffer_offset = 0;
        }

        const uint8_t *src;
        uint32_t src_len = virtio_guest_read_blob(read_address, read_length, &src);
        memcpy(dest_buffer + dest_offset, src, src_len);

        dest_offset += read_length;
        remaining -= read_length;
    }

    chain_bytes_read_total += dest_offset;
    return dest_offset;
}

uint32_t virtq_chain_set_next_blob(virtq_chain_t *chain, const uint8_t *src_buffer, uint32_t length)
{
    uint32_t src_offset = 0;
    uint32_t remaining = length;

    while (remaining) {
        if (chain->write_buffer_idx == chain->write_buffer_count) {
            virtio_logf("Device<%s> Write more than device-writable capacity",
                        chain->queue->virtio->name);
            break;
        }

        const virtq_desc_t *buf = &chain->write_buffers[chain->write_buffer_idx];
        uint32_t write_address = buf->addr_low + chain->write_buffer_offset;
        uint32_t write_length = buf->len - chain->write_buffer_offset;

        if (write_length > remaining) {
            write_length = remaining;
            chain->write_buffer_offset += remaining;
        }
        else {
            chain->write_buffer_idx++;
            chain->write_buffer_offset = 0;
        }

        write_blob(src_buffer + src_offset, write_length, write_address);

        src_offset += write_length;
        remaining -= write_length;
    }

    chain->length_written += src_offset;
    chain_bytes_written_total += src_offset;
    return src_offset;
}
