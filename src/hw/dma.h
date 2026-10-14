/*
 * dma.h -- the 8237 DMA controller: the channel address/count/page registers, the
 * mask/mode/flip-flop state machine, and the two transfer routines the (later) devices
 * that hold the channels drive.
 *
 * Port of v86/src/dma.js (430 lines).
 *
 * WHAT IS MISSING, and what a guest observes: upstream's only callers of do_read/do_write
 * are the devices that own a channel -- the floppy controller and the sound card -- and
 * NEITHER IS PORTED YET (they are later M2 units).  So the controller is fully programmable
 * and fully readable (a guest's BIOS setup and any DMA-channel probe sees the real
 * registers), unmask events fire their listeners, and the two transfer routines are
 * implemented and tested through the register interface -- but on this machine no device
 * ever STARTS a transfer by itself, because no device that would is present.  A guest that
 * programs a channel and waits for a device to drain it waits forever, exactly as it would
 * with the device absent.
 *
 * THE BUFFER TYPE is the port's stand-in for v86's async Buffer (buffer.js): upstream's
 * `buffer.get(start, count, cb)` / `buffer.set(start, bytes, cb)` are asynchronous because
 * a state image's buffer may be read from disk.  The C port's transfers are synchronous --
 * the callback is called before the function returns -- which is what the emulator's
 * instruction-by-instruction contract needs; the callback is kept so the callers' shape is
 * the same.
 */

#ifndef V86C_HW_DMA_H
#define V86C_HW_DMA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct dma_state {
    uint8_t channel_page[8];
    uint8_t channel_pagehi[8];
    uint16_t channel_addr[8];
    uint16_t channel_addr_init[8];
    uint16_t channel_count[8];
    uint16_t channel_count_init[8];
    uint8_t channel_mask[8];
    uint8_t channel_mode[8];

    int32_t lsb_msb_flipflop;
} dma_state_t;

extern dma_state_t *dma;

/* The byte buffer a transfer moves (dma.js:277-380's `buffer`): a plain byte array. */
typedef struct dma_buffer {
    uint8_t *data;
    uint32_t byte_length;
} dma_buffer_t;

/* dma.js:380-415 -- the completion callback; `error` is upstream's fn(true)/fn(false). */
typedef void (*dma_complete_fn)(void *this_value, bool error);

#define DMA_MAX_UNMASK_LISTENERS 8

void dma_init(void);

/* dma.js:243-266 -- the unmask notification (the sound card's entry point upstream) */
void dma_on_unmask(void (*fn)(void *this_value, int32_t channel), void *this_value);

/* dma.js:266-289 -- the mask register's write path and its notification */
void dma_update_mask(int32_t channel, int32_t value);

/* dma.js:272-311 -- read from the buffer into guest memory */
void dma_do_read(dma_buffer_t *buffer, uint32_t start, uint32_t len, int32_t channel,
                 dma_complete_fn fn, void *this_value);

/* dma.js:313-384 -- write guest memory into the buffer */
void dma_do_write(dma_buffer_t *buffer, uint32_t start, uint32_t len, int32_t channel,
                  dma_complete_fn fn, void *this_value);

/* dma.js:386-405 -- the two address/count composition rules (the 16-bit channel shift and
 * the page byte's high half).  Public for the selftest and for the devices that own a
 * channel. */
uint32_t dma_address_get_8bit(int32_t channel);
uint32_t dma_count_get_8bit(int32_t channel);

/* dma.js:407-430 -- the shared byte/word latch */
uint32_t dma_flipflop_get(uint32_t old_dword, int32_t new_byte, bool continuing);
uint32_t dma_flipflop_read(uint32_t dword);

#endif /* V86C_HW_DMA_H */
