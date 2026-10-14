/*
 * bus.h -- the device bus (BusConnector): a one-way message channel between two paired
 * connectors.
 *
 * Port of v86/src/bus.js (103 lines).  The JS bus carries arbitrary JS values ("name" plus
 * an optional value); the C port carries the same messages as a small int32 argument
 * vector, which is all any message in the ported device set uses ("pcspeaker-update" is a
 * pair [mode, reload], "mouse-delta" a pair, "keyboard-code" a single code).
 *
 * The one thing the C port drops is `send_async` (bus.js:84-88): it is `setTimeout(this.send,
 * 0)`, the browser event loop's ack to the UI, and a synchronous emulator has no event loop
 * to defer to.  Every listener in the ported device set is called synchronously either way.
 */

#ifndef V86C_HW_BUS_H
#define V86C_HW_BUS_H

#include <stdint.h>

typedef struct bus_connector bus_connector_t;

/* bus.js:6-15 -- a listener's callback and the `this_value` upstream calls it with. */
typedef void (*bus_listener_fn)(void *this_value, const int32_t *args, int nargs);

/* bus.js:90-101 (Bus.create): two connectors paired to each other.  `bus_reset` re-creates
 * an empty pair (the selftest's fresh machine); the storage is static, so the pointers stay
 * valid. */
void bus_reset(void);
void bus_create(bus_connector_t **c0, bus_connector_t **c1);

/* bus.js:20-39, 44-58 */
void bus_register(bus_connector_t *connector, const char *name, bus_listener_fn fn, void *this_value);
void bus_unregister(bus_connector_t *connector, const char *name, bus_listener_fn fn);

/* bus.js:60-82 -- send to the PAIRED connector's listeners.  `args` may be NULL when
 * nargs == 0. */
void bus_send(bus_connector_t *connector, const char *name, const int32_t *args, int nargs);

#define BUS_SEND(connector, name, ...)                                                            \
    do {                                                                                          \
        const int32_t bus_send_args_[] = {__VA_ARGS__};                                            \
        bus_send((connector), (name), bus_send_args_,                                              \
                 (int)(sizeof(bus_send_args_) / sizeof(bus_send_args_[0])));                        \
    } while (0)

#endif /* V86C_HW_BUS_H */
