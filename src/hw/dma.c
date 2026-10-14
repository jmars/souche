/*
 * dma.c -- the 8237 DMA controller.  See dma.h for what is missing (the devices that
 * would drive a channel) and for the buffer's synchronous shape.
 *
 * The registration: upstream registers forty separate closures with `.bind(this, channel)`
 * (dma.js:38-94).  A C port has no closures, but its write callbacks DO receive the port
 * (io.h D3), so the forty registrations become four tables of ports and four handler
 * functions that recover the channel from the port -- the same forty ports, with the
 * mapping written down once instead of forty times.
 */

#include "dma.h"

#include <string.h>

#include <stdio.h>

#include "../cpu/cpu.h" /* unimplemented_instruction */
#include "../cpu/cpu_state.h"
#include "../cpu/memory.h"
#include "io.h"

static dma_state_t dma_storage;
dma_state_t *dma = NULL;

typedef struct {
    void (*fn)(void *this_value, int32_t channel);
    void *this_value;
} dma_unmask_listener_t;

static dma_unmask_listener_t unmask_listeners[DMA_MAX_UNMASK_LISTENERS];
static int unmask_listener_count;

/* ------------------------------------------------------------------ *
 * the channel tables -- dma.js:38-94
 * ------------------------------------------------------------------ */
static const int32_t addr_ports[8] = {0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC};
static const int32_t count_ports[8] = {0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE};
static const int32_t page_ports[8] = {0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A};
/* channel 4 (the cascade) has no page-high register: dma.js:68-76 registers seven */
static const int32_t pagehi_ports[8] = {0x487, 0x483, 0x481, 0x482, 0x48B, 0x489, 0x48A, -1};

static int32_t channel_of(const int32_t *ports, int32_t port)
{
    for (int32_t i = 0; i < 8; i++) {
        if (ports[i] == port) return i;
    }
    DBG_ASSERT(false);
    return -1;
}

/* ------------------------------------------------------------------ *
 * the register handlers -- dma.js:124-240
 * ------------------------------------------------------------------ */
static void port_count_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    int32_t channel = channel_of(count_ports, port);
    dma->channel_count[channel] =
        (uint16_t)dma_flipflop_get(dma->channel_count[channel], data_byte, false);
    dma->channel_count_init[channel] =
        (uint16_t)dma_flipflop_get(dma->channel_count_init[channel], data_byte, true);
}

static int32_t port_count_read(void *device, int32_t port)
{
    (void)device;
    int32_t channel = channel_of(count_ports, port);
    return (int32_t)dma_flipflop_read(dma->channel_count[channel]);
}

static void port_addr_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    int32_t channel = channel_of(addr_ports, port);
    dma->channel_addr[channel] =
        (uint16_t)dma_flipflop_get(dma->channel_addr[channel], data_byte, false);
    dma->channel_addr_init[channel] =
        (uint16_t)dma_flipflop_get(dma->channel_addr_init[channel], data_byte, true);
}

static int32_t port_addr_read(void *device, int32_t port)
{
    (void)device;
    int32_t channel = channel_of(addr_ports, port);
    return (int32_t)dma_flipflop_read(dma->channel_addr[channel]);
}

static void port_page_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    dma->channel_page[channel_of(page_ports, port)] = (uint8_t)data_byte;
}

static int32_t port_page_read(void *device, int32_t port)
{
    (void)device;
    return dma->channel_page[channel_of(page_ports, port)];
}

static void port_pagehi_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    dma->channel_pagehi[channel_of(pagehi_ports, port)] = (uint8_t)data_byte;
}

static int32_t port_pagehi_read(void *device, int32_t port)
{
    (void)device;
    return dma->channel_pagehi[channel_of(pagehi_ports, port)];
}

static void port_singlemask_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    int32_t channel_offset = port == 0x0A ? 0 : 4;
    int32_t channel = (data_byte & 0x3) + channel_offset;
    int32_t value = data_byte & 0x4 ? 1 : 0;
    dma_update_mask(channel, value);
}

static void port_multimask_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    int32_t channel_offset = port == 0x0F ? 0 : 4;
    for (int32_t i = 0; i < 4; i++) {
        /* dma.js:196-199: the raw masked bit, not a 0/1 (a Uint8Array store keeps it) */
        dma_update_mask(channel_offset + i, data_byte & (1 << i));
    }
}

static int32_t port_multimask_read(void *device, int32_t port)
{
    (void)device;
    int32_t channel_offset = port == 0x0F ? 0 : 4;
    int32_t value = 0;
    value |= dma->channel_mask[channel_offset + 0];
    value |= dma->channel_mask[channel_offset + 1] << 1;
    value |= dma->channel_mask[channel_offset + 2] << 2;
    value |= dma->channel_mask[channel_offset + 3] << 3;
    return value;
}

static void port_mode_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    int32_t channel_offset = port == 0x0B ? 0 : 4;
    int32_t channel = (data_byte & 0x3) + channel_offset;
    dma->channel_mode[channel] = (uint8_t)data_byte;
}

static void portC_write(void *device, int32_t port, int32_t data_byte)
{
    (void)device;
    (void)port;
    (void)data_byte;
    dma->lsb_msb_flipflop = 0;
}

void dma_init(void)
{
    dma = &dma_storage;
    memset(&dma_storage, 0, sizeof(dma_storage));
    unmask_listener_count = 0;
    memset(unmask_listeners, 0, sizeof(unmask_listeners));

    static const int32_t singlemask_ports[2] = {0x0A, 0xD4};
    static const int32_t multimask_ports[2] = {0x0F, 0xDE};
    static const int32_t mode_ports[2] = {0x0B, 0xD6};
    static const int32_t portc_ports[2] = {0x0C, 0xD8};

    for (int32_t i = 0; i < 8; i++) {
        io_register_write(addr_ports[i], dma, port_addr_write, NULL, NULL);
        io_register_read(addr_ports[i], dma, port_addr_read, NULL, NULL);
        io_register_write(count_ports[i], dma, port_count_write, NULL, NULL);
        io_register_read(count_ports[i], dma, port_count_read, NULL, NULL);
        io_register_write(page_ports[i], dma, port_page_write, NULL, NULL);
        io_register_read(page_ports[i], dma, port_page_read, NULL, NULL);
        if (pagehi_ports[i] >= 0) {
            io_register_write(pagehi_ports[i], dma, port_pagehi_write, NULL, NULL);
            io_register_read(pagehi_ports[i], dma, port_pagehi_read, NULL, NULL);
        }
    }

    io_register_write(singlemask_ports[0], dma, port_singlemask_write, NULL, NULL);
    io_register_write(singlemask_ports[1], dma, port_singlemask_write, NULL, NULL);
    io_register_write(multimask_ports[0], dma, port_multimask_write, NULL, NULL);
    io_register_write(multimask_ports[1], dma, port_multimask_write, NULL, NULL);
    io_register_read(multimask_ports[0], dma, port_multimask_read, NULL, NULL);
    io_register_read(multimask_ports[1], dma, port_multimask_read, NULL, NULL);
    io_register_write(mode_ports[0], dma, port_mode_write, NULL, NULL);
    io_register_write(mode_ports[1], dma, port_mode_write, NULL, NULL);
    io_register_write(portc_ports[0], dma, portC_write, NULL, NULL);
    io_register_write(portc_ports[1], dma, portC_write, NULL, NULL);
}

/* ------------------------------------------------------------------ *
 * dma.js:243-266 -- unmask notifications
 * ------------------------------------------------------------------ */
void dma_on_unmask(void (*fn)(void *this_value, int32_t channel), void *this_value)
{
    DBG_ASSERT(unmask_listener_count < DMA_MAX_UNMASK_LISTENERS);
    unmask_listeners[unmask_listener_count].fn = fn;
    unmask_listeners[unmask_listener_count].this_value = this_value;
    unmask_listener_count++;
}

void dma_update_mask(int32_t channel, int32_t value)
{
    if (dma->channel_mask[channel] != value) {
        dma->channel_mask[channel] = (uint8_t)value;

        if (!value) {
            for (int i = 0; i < unmask_listener_count; i++) {
                unmask_listeners[i].fn(unmask_listeners[i].this_value, channel);
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * the transfers -- dma.js:272-384
 * ------------------------------------------------------------------ */
void dma_do_read(dma_buffer_t *buffer, uint32_t start, uint32_t len, int32_t channel,
                 dma_complete_fn fn, void *this_value)
{
    uint32_t read_count = dma_count_get_8bit(channel);
    uint32_t addr = dma_address_get_8bit(channel);

    /* dma.js:280-283: upstream only logs when len < read_count. */
    (void)len;

    if (start + read_count > buffer->byte_length) {
        fn(this_value, true);
        return;
    }

    /* THE C PORT'S BOUND, the twin of dma_do_write's (below): upstream reads mem8 -- the
     * whole wasm linear memory -- and an access past it traps.  AUDIT-1 S2: this arm had
     * NO such guard and handed the guest-chosen DMA address straight to write_blob, whose
     * asserts were its only bounds (its twin's asymmetry was the audit's point; dead
     * today with the floppy unported, but the asymmetry is the bug).  Same shape, same
     * named stop. */
    if ((uint64_t)addr + read_count > cpu->memory_size) {
        char name[160];
        snprintf(name, sizeof(name),
                 "dma_do_read(channel=%d, addr=0x%x, len=%u) outside guest RAM"
                 " (memory_size=0x%x)",
                 (int)channel, (unsigned)addr, (unsigned)read_count,
                 (unsigned)cpu->memory_size);
        unimplemented_instruction(name);
        fn(this_value, true);
        return;
    }

    dma->channel_addr[channel] = (uint16_t)(dma->channel_addr[channel] + read_count);

    write_blob(buffer->data + start, read_count, addr);
    fn(this_value, false);
}

void dma_do_write(dma_buffer_t *buffer, uint32_t start, uint32_t len, int32_t channel,
                  dma_complete_fn fn, void *this_value)
{
    uint32_t read_count = (uint32_t)(dma->channel_count[channel] + 1) & 0xFFFF;
    uint32_t bytes_per_count = channel >= 5 ? 2 : 1;
    uint32_t read_bytes = read_count * bytes_per_count;
    uint32_t addr = dma_address_get_8bit(channel);
    bool unfinished = false;
    bool want_more = false;
    bool autoinit = (dma->channel_mode[channel] & 0x10) != 0;

    if (len < read_bytes) {
        read_count = len / bytes_per_count;
        read_bytes = read_count * bytes_per_count;
        unfinished = true;
    }
    else if (len > read_bytes) {
        want_more = true;
    }

    if (start + read_bytes > buffer->byte_length) {
        fn(this_value, true);
        return;
    }

    /* THE C PORT'S BOUND (dma.h): upstream reads mem8 -- the whole wasm linear memory --
     * and an access past it traps; here the read would be host memory outside the guest's
     * 2 MiB allocation, so an out-of-range transfer STOPS loudly with the channel and the
     * address named instead of reading it. */
    if ((uint64_t)addr + read_bytes > cpu->memory_size) {
        char name[160];
        snprintf(name, sizeof(name),
                 "dma_do_write(channel=%d, addr=0x%x, len=%u) outside guest RAM (memory_size=0x%x)",
                 (int)channel, (unsigned)addr, (unsigned)read_bytes, (unsigned)cpu->memory_size);
        unimplemented_instruction(name);
        fn(this_value, true);
        return;
    }

    dma->channel_addr[channel] = (uint16_t)(dma->channel_addr[channel] + read_count);
    dma->channel_count[channel] = (uint16_t)(dma->channel_count[channel] - read_count);
    /* when complete, counter should underflow to 0xFFFF */

    if (!unfinished && autoinit) {
        dma->channel_addr[channel] = dma->channel_addr_init[channel];
        dma->channel_count[channel] = dma->channel_count_init[channel];
    }

    memcpy(buffer->data + start, mem8 + addr, read_bytes);

    if (want_more && autoinit) {
        dma_do_write(buffer, start + read_bytes, len - read_bytes, channel, fn, this_value);
    }
    else {
        fn(this_value, false);
    }
}

/* ------------------------------------------------------------------ *
 * dma.js:386-430
 * ------------------------------------------------------------------ */
uint32_t dma_address_get_8bit(int32_t channel)
{
    uint32_t addr = dma->channel_addr[channel];
    uint32_t page = dma->channel_page[channel];

    /* http://wiki.osdev.org/ISA_DMA#16_bit_issues */
    if (channel >= 5) {
        addr = addr << 1;
        page &= 0xFE;
    }

    addr |= page << 16;
    addr |= (uint32_t)dma->channel_pagehi[channel] << 24;

    return addr;
}

uint32_t dma_count_get_8bit(int32_t channel)
{
    uint32_t count = (uint32_t)(dma->channel_count[channel] + 1);

    if (channel >= 5) {
        count *= 2;
    }

    return count;
}

uint32_t dma_flipflop_get(uint32_t old_dword, int32_t new_byte, bool continuing)
{
    if (!continuing) {
        dma->lsb_msb_flipflop ^= 1;
    }

    if (dma->lsb_msb_flipflop) {
        /* low byte */
        return (old_dword & ~0xFFu) | (uint32_t)new_byte;
    }
    /* high byte */
    return (old_dword & ~0xFF00u) | (uint32_t)new_byte << 8;
}

uint32_t dma_flipflop_read(uint32_t dword)
{
    dma->lsb_msb_flipflop ^= 1;

    if (dma->lsb_msb_flipflop) {
        /* low byte */
        return dword & 0xFF;
    }
    /* high byte */
    return (dword >> 8) & 0xFF;
}
