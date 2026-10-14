/*
 * virtio.h -- the virtio-PCI transport.
 *
 * Port of v86/src/virtio.js (1517 lines): the PCI device registration and its four
 * capabilities (common / notification / ISR / device-specific), the virtqueues (descriptor
 * tables, the avail and used rings, the buffer chains), the feature negotiation and the
 * device-status handshake, the ISR and the notification path.
 *
 * WHICH virtio THIS IS -- MODERN (VIRTIO 1.x), NOT LEGACY 0.9.5.  The evidence is in the
 * upstream source and it is unambiguous: the device id is 0x1043 (the virtio-console's
 * MODERN id; the transitional/legacy id would be 0x1003), the capabilities list bit
 * (status 0x10) is set and the caps live in a 0x40-terminated vendor-capability linked list,
 * and the transport asserts its own precondition at construction:
 *
 *     dbg_assert(options.common.features.includes(VIRTIO_F_VERSION_1),
 *         "only non-transitional devices are supported");      (virtio.js:262-263)
 *
 * VIRTIO_F_VERSION_1 is bit 32 and REQUIRED.  Consequences a guest can observe, and each is
 * why the port keeps upstream's shape:
 *   - the driver must negotiate through the common configuration structure (device_feature /
 *     guest_feature dwords selected by the two select registers), not the legacy BAR0 layout;
 *   - the queue addresses are the four dwords queue_desc/avail/used (lo and hi) in the common
 *     structure, not the legacy PFN register -- the HI dwords are read as 0 and their write is
 *     IGNORED with a note (virtio.js:556-600: a 32-bit guest could not set them anyway);
 *   - there is no legacy "queue align / queue PFN" pair and no legacy ISR read-and-clear
 *     side effect beyond the ISR capability's.
 *
 * WHAT IS A SEAT AND WHAT IS NOT:
 *
 *  - get_state/set_state (virtio.js:912-950, 1074-1106) are NOT ported: this port has no
 *    state-save machinery (uart.h's precedent, and no unit has needed one).  The one field
 *    that exists ONLY for a restored state -- VirtQueue.fix_wrapping, set by
 *    VirtQueue.set_state (virtio.js:1105) -- is kept, and its honest current value is FALSE
 *    forever here, because nothing can set it.  The counting path that reads it is ported as
 *    upstream writes it.
 *  - dbg_log has no channel (M1b-1's rule): every LOG_VIRTIO line is dropped.  The two
 *    DEBUG-only wrappers init_capabilities installs around every field (virtio.js:784-808)
 *    are dropped with them -- they log and then call the same callback.
 *  - upstream's `dbg_assert(false, ...)` arms are compiled OUT of the pinned release build
 *    (dbg.rs:63 is debug_assert!).  Each is transcribed as the release's OBSERVABLE
 *    behaviour, with the assertion named: where the assert is the only statement the
 *    behaviour is "nothing happens" (a diagnostic under V86C_LOG_VIRTIO, which decides
 *    nothing); where a `return` follows it the return is REAL control flow and is kept.
 *  - V86C_LOG_VIRTIO is this port's addition (io.h D1's / ide.c's pattern): with it set on
 *    stderr only, every field access prints its capability, field name, value and width.
 *    It DECIDES NOTHING.
 */

#ifndef V86C_HW_VIRTIO_H
#define V86C_HW_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

#include "io.h"
#include "pci.h"

/* virtio.js:12-61 -- the constants, names kept. */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4
#define VIRTIO_PCI_CAP_VENDOR 0x09
#define VIRTIO_PCI_CAP_LENGTH 16

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG 5

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED 128

#define VIRTIO_ISR_QUEUE 1
#define VIRTIO_ISR_DEVICE_CFG 2

/* Feature bits (bit positions), virtio.js:53-55. */
#define VIRTIO_F_RING_INDIRECT_DESC 28
#define VIRTIO_F_RING_EVENT_IDX 29
#define VIRTIO_F_VERSION_1 32

/* Queue struct sizes, virtio.js:63-76. */
#define VIRTQ_DESC_ENTRYSIZE 16
#define VIRTQ_AVAIL_BASESIZE 6
#define VIRTQ_AVAIL_ENTRYSIZE 2
#define VIRTQ_USED_BASESIZE 6
#define VIRTQ_USED_ENTRYSIZE 8
#define VIRTQ_IDX_MASK 0xFFFF

#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_DESC_F_INDIRECT 4
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTQ_USED_F_NO_NOTIFY 1

/* Feature bits are grouped in dwords: upstream assumes < 128 bits (virtio.js:247-256). */
#define VIRTIO_FEATURE_WORDS 4

/* The device index space this transport is built for.  The multiport console needs ten
 * queues (four of its own + two per extra port), and upstream's own devices range from one
 * queue (balloon's stats) to 2 + 2*npairs.  16 covers every device upstream constructs. */
#define VIRTIO_MAX_QUEUES 16

/* A descriptor chain is bounded by the queue size upstream (chain_max = virtqueue.size,
 * virtio.js:1369) except inside an INDIRECT table, whose bound is the table's own length
 * (virtio.js:1391).  Indirect descriptors are only honoured when the driver negotiated
 * VIRTIO_F_RING_INDIRECT_DESC -- which NO device in this header offers (see
 * virtio_queue_options_t's callers), so the arm is unreachable here; the chain arrays are
 * sized generously (60x the largest queue this port builds) and the traversal STOPS with a
 * named message rather than overrunning if it is ever reached. */
#define VIRTIO_MAX_CHAIN 64

/* One io-port field slot per capability field.  The multiport console registers 19 common +
 * 10 notification + 1 ISR + 4 device-specific = 34; the constant is a bound with room. */
#define VIRTIO_FIELD_SLOT_MAX 64

typedef struct virtio_device virtio_device_t;
typedef struct virtq_chain virtq_chain_t;
typedef struct virtio_field_slot virtio_field_slot_t;

typedef int32_t (*virtio_field_read_fn)(virtio_field_slot_t *slot);
typedef void (*virtio_field_write_fn)(virtio_field_slot_t *slot, int32_t data);

/* One field of a capability's structure: its width, its name (for the trace) and its two
 * accessors.  Upstream's fields are objects with `read`/`write` closures over the device
 * (virtio.js:330-601); the C callback takes the SLOT, which carries both the device and the
 * field's index (the notification handlers need the index -- see virtio.c's
 * notify_field_write). */
typedef struct virtio_cap_field {
    uint8_t bytes;
    const char *name;
    virtio_field_read_fn read;
    virtio_field_write_fn write;
} virtio_cap_field_t;

/* The object an io-port callback is registered with (io.h D3's pattern). */
struct virtio_field_slot {
    virtio_device_t *virtio;
    void *owner; /* who the field's own callbacks are called with (virtio for the three
                  * transport capabilities, the DEVICE for the device-specific one) */
    const virtio_cap_field_t *field;
    int32_t index;
};

/* virtio.js:1189-1197 -- a descriptor as the device read it out of guest memory.  The high
 * address dword is kept because upstream reads it (and never uses it outside its trace). */
typedef struct virtq_desc {
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

/* A descriptor chain in flight, virtio.js:1341-1517.  ONE per queue in this port: every call
 * path in the ported devices pops, consumes and pushes before it pops again, so a single
 * chain cannot be aliased.  (Stated, not assumed: it is the C port's replacement for
 * upstream's one-object-per-pop JS allocation.) */
struct virtq_chain {
    struct virtqueue *queue;

    uint16_t head_idx;

    virtq_desc_t read_buffers[VIRTIO_MAX_CHAIN];
    int32_t read_buffer_count;
    int32_t read_buffer_idx;
    uint32_t read_buffer_offset;
    uint32_t length_readable;

    virtq_desc_t write_buffers[VIRTIO_MAX_CHAIN];
    int32_t write_buffer_count;
    int32_t write_buffer_idx;
    uint32_t write_buffer_offset;
    uint32_t length_written;
    uint32_t length_writable;
};

typedef struct virtqueue {
    virtio_device_t *virtio;

    uint32_t size;
    uint32_t size_supported;
    uint32_t mask;
    bool enabled;
    uint32_t notify_offset;

    uint32_t desc_addr;
    uint32_t avail_addr;
    uint32_t avail_last_idx;
    uint32_t used_addr;
    uint32_t num_staged_replies;

    /* Set only by upstream's set_state (virtio.js:1105), which this port does not have:
     * its current value is false forever.  Ported because count_requests reads it. */
    bool fix_wrapping;

    virtq_chain_t chain;
} virtqueue_t;

/* virtio.js:99-105, 1046-1060 */
typedef struct virtio_queue_options {
    uint32_t size_supported;
    uint32_t notify_offset;
} virtio_queue_options_t;

typedef void (*virtio_notify_handler_fn)(void *device, uint32_t queue_id);

/* virtio.js:107-116.  `handlers` is upstream's array, indexed by NOTIFY OFFSET: the
 * notification capability registers one 16-bit port per entry, and the handler is called
 * with the VALUE the driver wrote there -- which virtio's own drivers set to the QUEUE
 * INDEX (the argument is named queue_id upstream for that reason).  A NULL entry is
 * upstream's `undefined` handler: the write is a no-op. */
typedef struct virtio_notification_options {
    int32_t initial_port;
    bool single_handler;
    const virtio_notify_handler_fn *handlers;
    int32_t handler_count;
    void *owner;
} virtio_notification_options_t;

/* virtio.js:97-105 */
typedef struct virtio_common_options {
    int32_t initial_port;
    const virtio_queue_options_t *queues;
    int32_t queue_count;
    const uint32_t *features;
    int32_t feature_count;
    void (*on_driver_ok)(void *device);
} virtio_common_options_t;

typedef struct virtio_isr_options {
    int32_t initial_port;
} virtio_isr_options_t;

typedef struct virtio_device_specific_options {
    int32_t initial_port;
    const virtio_cap_field_t *fields;
    int32_t field_count;
    void *owner;
} virtio_device_specific_options_t;

/* virtio.js:142-155 */
typedef struct virtio_options {
    const char *name;
    int32_t pci_id;
    int32_t device_id;
    int32_t subsystem_device_id;
    virtio_common_options_t common;
    virtio_notification_options_t notification;
    virtio_isr_options_t isr_status;
    const virtio_device_specific_options_t *device_specific; /* NULL: no such capability */
    void *device; /* the owning device: what on_driver_ok and the notify handlers get */
} virtio_options_t;

struct virtio_device {
    const char *name;

    /* pci.js's device object (pci.h) -- the transport registers itself at the end of its
     * constructor, exactly as virtio.js:313 does. */
    uint8_t pci_space[256]; /* virtio.js:175-233 + init_capabilities' writes */
    int32_t pci_space_length;
    pci_bar_t pci_bars[6];
    int32_t pci_bars_count;
    pci_device_t pci_device;

    int32_t pci_id;
    int32_t device_id;

    uint32_t device_feature_select;
    uint32_t driver_feature_select;
    uint32_t device_feature[VIRTIO_FEATURE_WORDS];
    uint32_t driver_feature[VIRTIO_FEATURE_WORDS];
    bool features_ok;
    uint8_t device_status;
    bool config_has_changed;
    uint32_t config_generation;

    virtqueue_t queues[VIRTIO_MAX_QUEUES];
    int32_t queue_count;
    uint32_t queue_select;
    virtqueue_t *queue_selected;

    uint8_t isr_status;

    /* the io-port field slots (this port's stand-in for upstream's closures) */
    virtio_field_slot_t slots[VIRTIO_FIELD_SLOT_MAX];
    int32_t slot_count;

    /* The notification capability's fields.  THEY LIVE HERE, not in a local of the
     * constructor: the slots hold POINTERS into the field array (upstream closes over the
     * closures, which the garbage collector keeps alive), so a stack-local table would be a
     * dangling pointer the moment the constructor returned.  MEASURED, and it cost this unit
     * a host SIGSEGV: the guest's `outw` to 0xB904 jumped through the stale memory
     * (results/m2-4-virtio.md 4.1). */
    virtio_cap_field_t notify_fields[VIRTIO_MAX_QUEUES];

    /* what the notification fields call.  Kept from the options: upstream's handlers are
     * closures built in create_notification_capability and never stored on the device. */
    const virtio_notify_handler_fn *notify_handlers;
    int32_t notify_handler_count;
    void *notify_owner;

    void (*on_driver_ok)(void *device);
    void *device;
};

/* virtio.js:162-315 -- the constructor.  Returns false (and says why on stderr) if the
 * options exceed a bound of this port's (queues, fields, bars); upstream has no such bounds
 * and would build them. */
bool virtio_init(virtio_device_t *virtio, const virtio_options_t *options);

/* virtio.js:952-973 */
void virtio_reset(virtio_device_t *virtio);

/* virtio.js:1007-1012 */
bool virtio_is_feature_negotiated(virtio_device_t *virtio, uint32_t feature);

/* virtio.js:979-992 -- call when the device's own configuration changes. */
void virtio_notify_config_changes(virtio_device_t *virtio);

/* virtio.js:997-1005.  NO CALLER, UPSTREAM OR HERE: upstream's only call site is a comment
 * ("to be called after reading any field whose write can trigger notify_config_changes()")
 * and grep finds none.  Ported for parity and exercised by the selftest, which is what keeps
 * it from being an untested path. */
void virtio_update_config_generation(virtio_device_t *virtio);

/* virtio.js:1018-1027.  Same status as update_config_generation: no caller upstream. */
void virtio_needs_reset(virtio_device_t *virtio);

/* virtio.js:1029-1041 */
void virtio_raise_irq(virtio_device_t *virtio, uint32_t type);
void virtio_lower_irq(virtio_device_t *virtio);

/* ---- the virtqueue (virtio.js:1048-1517), public for the selftest and for devices ---- */

void virtqueue_init(virtqueue_t *queue, virtio_device_t *virtio,
                    const virtio_queue_options_t *options);
/* virtio.js:1108-1117 */
void virtqueue_reset(virtqueue_t *queue);
/* virtio.js:1119-1122 */
bool virtqueue_is_configured(virtqueue_t *queue);
/* virtio.js:1124-1128 */
void virtqueue_enable(virtqueue_t *queue);
/* virtio.js:1130-1136 */
void virtqueue_set_size(virtqueue_t *queue, uint32_t size);
/* virtio.js:1141-1149 */
uint32_t virtqueue_count_requests(virtqueue_t *queue);
/* virtio.js:1154-1158 */
bool virtqueue_has_request(virtqueue_t *queue);
/* virtio.js:1163-1177 -- the chain lives in the queue (virtq_chain_t above) */
virtq_chain_t *virtqueue_pop_request(virtqueue_t *queue);
/* virtio.js:1185-1196 */
void virtqueue_push_reply(virtqueue_t *queue, virtq_chain_t *chain);
/* virtio.js:1202-1246 */
void virtqueue_flush_replies(virtqueue_t *queue);
/* virtio.js:1255-1262 */
void virtqueue_notify_me_after(virtqueue_t *queue, uint32_t num_skipped_requests);

/* virtio.js:1438-1473 -- read the chain's device-readable buffers into `dest_buffer`.
 * Returns the number of bytes actually read. */
uint32_t virtq_chain_get_next_blob(virtq_chain_t *chain, uint8_t *dest_buffer, uint32_t length);
/* virtio.js:1480-1517 -- write `src_buffer` into the chain's device-writable buffers. */
uint32_t virtq_chain_set_next_blob(virtq_chain_t *chain, const uint8_t *src_buffer,
                                   uint32_t length);

/* The observation surface the gate needs: how many bytes each queue's chains consumed
 * (readable side) and wrote (writable side).  This port's addition, like ide.c's counters --
 * upstream's device is a black box between the driver and the bus messages, and the gate
 * wants the same numbers the reference's harness counts at the adapter.  They DECIDE
 * NOTHING. */
uint64_t virtq_chain_bytes_read(void);
uint64_t virtq_chain_bytes_written(void);

#endif /* V86C_HW_VIRTIO_H */
