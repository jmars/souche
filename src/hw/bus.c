/* bus.c -- the pair of BusConnectors.  See bus.h. */

#include "bus.h"

#include <string.h>

#include "../cpu/cpu_state.h" /* DBG_ASSERT */

#define BUS_MAX_LISTENERS 64

typedef struct bus_listener {
    const char *name;
    bus_listener_fn fn;
    void *this_value;
} bus_listener_t;

struct bus_connector {
    bus_listener_t listeners[BUS_MAX_LISTENERS];
    int listener_count;
    struct bus_connector *pair;
};

static struct bus_connector bus_storage[2];

void bus_reset(void)
{
    memset(bus_storage, 0, sizeof(bus_storage));
}

void bus_create(bus_connector_t **c0, bus_connector_t **c1)
{
    bus_storage[0].pair = &bus_storage[1];
    bus_storage[1].pair = &bus_storage[0];
    *c0 = &bus_storage[0];
    *c1 = &bus_storage[1];
}

void bus_register(bus_connector_t *connector, const char *name, bus_listener_fn fn, void *this_value)
{
    DBG_ASSERT(connector != NULL);
    DBG_ASSERT(connector->listener_count < BUS_MAX_LISTENERS);
    bus_listener_t *listener = &connector->listeners[connector->listener_count++];
    listener->name = name;
    listener->fn = fn;
    listener->this_value = this_value;
}

void bus_unregister(bus_connector_t *connector, const char *name, bus_listener_fn fn)
{
    DBG_ASSERT(connector != NULL);
    /* bus.js:44-58 -- filter out the entry; order of the rest is preserved. */
    for (int i = 0; i < connector->listener_count; i++) {
        if (strcmp(connector->listeners[i].name, name) == 0 && connector->listeners[i].fn == fn) {
            for (int j = i; j + 1 < connector->listener_count; j++) {
                connector->listeners[j] = connector->listeners[j + 1];
            }
            connector->listener_count--;
            return;
        }
    }
}

void bus_send(bus_connector_t *connector, const char *name, const int32_t *args, int nargs)
{
    /* bus.js:60-64: a connector with no pair drops the message. */
    if (connector == NULL || connector->pair == NULL) return;

    bus_connector_t *target = connector->pair;
    for (int i = 0; i < target->listener_count; i++) {
        bus_listener_t *listener = &target->listeners[i];
        if (strcmp(listener->name, name) == 0) {
            listener->fn(listener->this_value, args, nargs);
        }
    }
}
