/* SECFIX-1's C-level asymmetry probe (audit-1 S2 read+write arms, S3, audit-2 A2-SF).
 * Built and driven by results/secfix1-probe.sh against a SOURCE ROOT parameter, so the SAME
 * probe runs against the live tree and against the audited pre-fix sources (git a49a887
 * materialized to a shadow) -- the before/after asymmetry is the proof, never a claim.
 *
 * The probe links the REAL translation units (the unity core + every hw/*.c) and drives the
 * real entry points: memory.c's write_blob, virtio.c's chain reader, ide.c's ATAPI command
 * path through the real io-port dispatch (machine_init's device set), and jit_mem.c's bind
 * table -- not reimplementations of any of them.
 *
 * Usage: secfix1-probe <write|read|atapi|hash>
 *   write  S2: write_blob at an out-of-range and a straddling target
 *   read   S2: a virtio descriptor whose address is past guest RAM (the console sink's bytes)
 *   atapi  S3: READ_12 with a guest-chosen count past the media end, media on a guard page
 *   hash   A2-SF: saturate the block hash with tombstones under churn; a live sentinel block
 *          whose home slot a colliding key lands on must stay reachable
 * Exit: 0 = the bounded behaviour held; nonzero = abort, crash, or an unbounded observation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "cpu/cpu.h"
#include "cpu/cpu_state.h"
#include "cpu/memory.h"
#include "cpu/uop_cache.h"
#include "hw/ide.h"
#include "hw/io.h"
#include "hw/virtio.h"
#include "hw/machine.h"
#include "hw/bus.h"
#include "cpu/jit_mem.h"

#define GUEST_RAM (16u * 1024u * 1024u)

/* ide.c keeps its ATA command names private; this is ide.c:76's value. */
#define ATA_CMD_PACKET 0xA0

static void w16(uint32_t a, uint16_t v) { mem8[a] = (uint8_t)v; mem8[a + 1] = (uint8_t)(v >> 8); }
static void w32(uint32_t a, uint32_t v)
{
	mem8[a] = (uint8_t)v; mem8[a + 1] = (uint8_t)(v >> 8);
	mem8[a + 2] = (uint8_t)(v >> 16); mem8[a + 3] = (uint8_t)(v >> 24);
}

static void machine_bringup(void)
{
	cpu_state_init();
	if (mem8 == NULL && !allocate_memory(GUEST_RAM)) {
		fputs("probe: cannot allocate guest memory\n", stderr);
		exit(2);
	}
	cpu->memory_size = GUEST_RAM;
	zero_memory(0, cpu->memory_size);
	/* the uop cache owns the dirty-page bitmap write_blob's invalidation hook tests */
	uop_cache_enabled = true;
	if (!jit_mem_init()) {
		fputs("probe: jit/uop init failed\n", stderr);
		exit(2);
	}
}

static int mode_write(void)
{
	machine_bringup();
	uint8_t data[16];
	for (int i = 0; i < 16; i++) data[i] = (uint8_t)(0xA0 + i);

	/* 1. fully out of range: dropped, the process continues (pre-fix: assert-abort) */
	write_blob(data, 16, GUEST_RAM + 0x1000);

	/* 2. straddling: the in-range half lands, the tail is dropped */
	memset(mem8 + GUEST_RAM - 8, 0xEE, 8);
	write_blob(data, 16, GUEST_RAM - 8);
	for (int i = 0; i < 8; i++) {
		if (mem8[GUEST_RAM - 8 + i] != data[i]) {
			fprintf(stderr, "probe: write clamp byte %d wrong: %02x\n", i,
			        mem8[GUEST_RAM - 8 + i]);
			return 1;
		}
	}
	fprintf(stderr, "probe: write arm bounded (out-of-range dropped, straddle clamped)\n");
	return 0;
}

static int mode_read(void)
{
	machine_bringup();

	/* a chain with one device-readable descriptor at a guest-chosen address past RAM --
	 * exactly what a hostile driver posts (virtq_chain_build's output, hand-set here) */
	virtq_chain_t chain;
	memset(&chain, 0, sizeof(chain));
	chain.read_buffers[0].addr_low = GUEST_RAM + 0x1000; /* fully out of range */
	chain.read_buffers[0].len = 16;
	chain.read_buffer_count = 1;

	uint8_t *dest = calloc(1, 16);
	uint32_t got = virtq_chain_get_next_blob(&chain, dest, 16);

	/* upstream's contract: dest_offset advances by the FULL length even on the empty
	 * subarray, and the dest's gap keeps its pre-fill -- upstream's callers allocate
	 * zero-filled Uint8Arrays, so what the console sink receives is zeros, never host
	 * bytes.  (Pre-fix this process died in the assert; under -DNDEBUG it copied host
	 * heap here.) */
	if (got != 16) {
		fprintf(stderr, "probe: read arm returned %u, expected the full advance 16\n", got);
		return 1;
	}
	for (int i = 0; i < 16; i++) {
		if (dest[i] != 0) {
			fprintf(stderr, "probe: HOST BYTE LEAK into the console sink at %d: %02x\n", i,
			        dest[i]);
			return 1;
		}
	}
	fprintf(stderr, "probe: read arm bounded (16 zero bytes to the sink, no host bytes)\n");
	return 0;
}

static int mode_atapi(void)
{
	/* media whose END abuts a PROT_NONE guard page: the pre-fix memcpy in
	 * interface_data_set reads into the guard and the process dies on the spot */
	const uint32_t MEDIA = 8192;
	uint8_t *media = mmap(NULL, MEDIA + 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (media == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	if (mprotect(media, MEDIA, PROT_READ | PROT_WRITE) != 0) {
		perror("mprotect");
		return 2;
	}
	for (uint32_t i = 0; i < MEDIA; i++) media[i] = (uint8_t)(i * 7 + 3);

	cpu_state_init();
	if (!allocate_memory(GUEST_RAM)) return 2;
	cpu->memory_size = GUEST_RAM;
	zero_memory(0, cpu->memory_size);
	uop_cache_enabled = true;
	if (!jit_mem_init()) return 2;

	memset(&machine_config, 0, sizeof(machine_config));
	machine_config.cdrom.buffer = media;
	machine_config.cdrom.length = MEDIA;
	machine_config.cdrom.is_cdrom = true;
	machine_init();

	/* the real ATAPI path, through the real io-port dispatch: select the secondary
	 * master, set the DMA feature bit (the arm interface_atapi_handle dispatches on),
	 * PACKET, then the 12-byte READ_12 CDB in 6 word writes.
	 * lba 3 -> start 6144 (inside 8192); count 0x2000 sectors -> byte_count 0x4000000
	 * (64 MiB), 8190 bytes past the media end. */
	io_portmap_write8(0x176, 0xA0);            /* drive/head: master */
	io_portmap_write8(0x171, 0x01);            /* features: DMA */
	io_portmap_write8(0x177, ATA_CMD_PACKET);  /* PACKET */
	uint8_t cdb[12] = { 0xA8, 0, 0, 0, 0, 3, /* READ_12, lba 3 (big-endian: cmd[5] is the LSB) */
		            0, 0x00, 0x20, 0x00, 0, 0 }; /* count 0x2000 */
	for (int w = 0; w < 6; w++) {
		io_portmap_write16(0x170, (int32_t)(cdb[2 * w] | cdb[2 * w + 1] << 8));
	}

	/* still alive: the clamp held.  The transfer must be the clamped 2048 bytes. */
	fprintf(stderr, "probe: atapi DMA read bounded (media %u B, lba 3, count 0x2000 sectors)\n",
	        MEDIA);
	return 0;
}

static int mode_hash(void)
{
	machine_bringup();
	uint8_t state = 0; /* jit_selftest binds with literal 0 */

	/* THE SENTINEL: one live block at (0x2000, 0x2000), never invalidated */
	uint8_t *sbuf = jit_code_reserve();
	if (sbuf == NULL || jit_code_publish(sbuf, 4) == NULL ||
	    !jit_cache_bind(0x2000, 0x2000, state, sbuf, 4)) {
		fputs("probe: sentinel bind failed\n", stderr);
		return 2;
	}

	/* a churn key whose hash lands on the SENTINEL's home slot: with the table saturated
	 * by tombstones, the pre-fix insert's exhausted scan comes back to its own home --
	 * the sentinel's slot -- and overwrites the live entry. */
	uint32_t h_target = jit_test_hash(0x2000, 0x2000, state);
	uint32_t coll_virt = 0;
	for (uint32_t v = 0x4000; v < 0xFFFFFFu; v += 4) {
		if (jit_test_hash(0x1000, v, state) == h_target) {
			coll_virt = v;
			break;
		}
	}
	if (coll_virt == 0) {
		fputs("probe: no colliding key found\n", stderr);
		return 2;
	}
	fprintf(stderr, "probe: churn key 0x%x collides with the sentinel's home slot\n", coll_virt);

	/* THE CHURN: bind + invalidate on one guest page, distinct (virt) keys -- each cycle
	 * leaves one DEAD tombstone, and the pool page is recycled for the next.  Saturates
	 * the table well past JIT_HASH_SLOTS. */
	for (uint32_t i = 0; i < 6000; i++) {
		uint8_t *buf = jit_code_reserve();
		if (buf == NULL) {
			fprintf(stderr, "probe: churn reserve failed at %u\n", i);
			return 2;
		}
		if (jit_code_publish(buf, 4) == NULL || !jit_cache_bind(0x1000, 0x1000 + i, state, buf, 4)) {
			fprintf(stderr, "probe: churn bind failed at %u\n", i);
			return 1;
		}
		if (jit_cache_lookup(0x2000, 0x2000, state) == NULL) {
			fprintf(stderr, "probe: SENTINEL LOST after churn cycle %u (live slot overwritten)"
			            " -- the hash-full overwrite is real\n", i);
			return 1;
		}
		jit_code_invalidate_page(0x1000);
		if (jit_cache_lookup(0x2000, 0x2000, state) == NULL) {
			fprintf(stderr, "probe: SENTINEL LOST by the invalidation at cycle %u\n", i);
			return 1;
		}
	}

	/* THE DECIDING BIND: the colliding key, table saturated */
	uint8_t *cbuf = jit_code_reserve();
	if (cbuf == NULL || jit_code_publish(cbuf, 4) == NULL ||
	    !jit_cache_bind(0x1000, coll_virt, state, cbuf, 4)) {
		fputs("probe: colliding bind refused\n", stderr);
		return 1;
	}
	if (jit_cache_lookup(0x2000, 0x2000, state) == NULL) {
		fprintf(stderr, "probe: SENTINEL LOST by the colliding bind -- the hash-full"
			        " overwrite is real\n");
		return 1;
	}
	if (jit_cache_lookup(0x1000, coll_virt, state) == NULL) {
		fputs("probe: the colliding block itself is not findable\n", stderr);
		return 1;
	}
	fprintf(stderr, "probe: hash survived 6000 tombstone churns + the colliding bind"
		        " (sentinel live, DEAD slots reused)\n");
	return 0;
}

static int mode_virtio(int step)
{
	machine_bringup();
	machine_config.virtio_console = true;
	machine_init();

	/* 1-2. NOT abort sites (verified against the pre-fix sources): the field layer rounds
	 *      a non-power-of-2 queue_size up and clamps it to size_supported BEFORE
	 *      virtqueue_set_size (common_queue_size_write), and refuses an enable on an
	 *      unconfigured queue (common_queue_enable_write).  These two writes document
	 *      that the field guards hold -- they pass on the pre-fix tree too. */
	io_portmap_write16(0xB818, 3); /* common cfg @0xB800 + 24: queue_size = 3 */
	io_portmap_write16(0xB81C, 1); /* + 28: queue_enable = 1, queue 0 unconfigured */

	/* 3. THE BARE UNCONFIGURED-NOTIFY (step 3): one port write on a queue the driver
	 *    never configured.  Pre-fix: virtqueue_has_request's avail_addr assert aborts.
	 *    Post-fix: the named drop. */
	if (step == 3 || step == 0) io_portmap_write16(0xB906, 3);
	if (step == 3) return 0;

	/* 4. THE AUDIT'S ATTACK (step 4): configure ONLY queue 3, post one 8-byte
	 *    DEVICE_READY control packet, notify -- the handler answers with
	 *    console_send_event, which pops the NEVER-CONFIGURED queue 2.  Pre-fix:
	 *    virtqueue_pop_request's assert.  Post-fix: the send is dropped by name. */
	io_portmap_write16(0xB816, 3); /* + 22: queue_select = 3 */
	io_portmap_write32(0xB820, 0x5000); /* + 32: queue_desc = 0x5000 */
	io_portmap_write32(0xB824, 0);
	io_portmap_write32(0xB828, 0x5200); /* + 40: queue_avail = 0x5200 */
	io_portmap_write32(0xB82C, 0);
	io_portmap_write32(0xB830, 0x5400); /* + 48: queue_used = 0x5400 */
	io_portmap_write32(0xB834, 0);
	fprintf(stderr, "probe: readback select=%#x desc=%#x avail=%#x used=%#x\n",
	        io_portmap_read16(0xB816) & 0xFFFF, io_portmap_read32(0xB820),
	        io_portmap_read32(0xB828), io_portmap_read32(0xB830));
	{
		virtio_console_t *c = machine_virtio_console_device();
		if (c != NULL)
			fprintf(stderr, "probe: device queues[3]: desc=%#x avail=%#x used=%#x"
			                " (count=%d)\n",
			        c->virtio.queues[3].desc_addr, c->virtio.queues[3].avail_addr,
			        c->virtio.queues[3].used_addr, c->virtio.queue_count);
	}

	w32(0x5000 + 0, 0x5600);  /* desc 0: addr */
	w32(0x5000 + 4, 0);       /*         addr high */
	w32(0x5000 + 8, 8);       /*         len 8 */
	w16(0x5000 + 12, 0);      /*         flags: no NEXT */
	w16(0x5000 + 14, 0);      /*         next */
	w16(0x5200 + 0, 0);       /* avail: flags */
	w16(0x5200 + 2, 1);       /* avail: idx = 1 (one posted request) */
	w16(0x5200 + 4, 0);       /* avail: entry[0] = desc 0 */
	memset(mem8 + 0x5600, 0, 8); /* the packet: port 0, event 0 = DEVICE_READY, value 0 */

	if (step == 3) return 0; /* step 3's abort site is the bare notify above */

	/* 5's setup, BEFORE the notify: queue 2 configured for real with one writable buffer,
	 * then queue_size = 0 through the field (the field layer clamps only the upper
	 * bound).  With this in place the notify's reply path reaches virtqueue_push_reply
	 * with size == 0: the first staged reply fails `0 < 0`.  Pre-fix:
	 * virtqueue_push_reply's assert.  Post-fix: the reply is staged, as the release
	 * stages it.  (Without this block -- step 4 -- queue 2 is never configured, and the
	 * same notify aborts pre-fix in virtqueue_pop_request instead.) */
	if (step == 5) {
		io_portmap_write16(0xB816, 2); /* queue_select = 2 */
		io_portmap_write32(0xB820, 0x5800);
		io_portmap_write32(0xB824, 0);
		io_portmap_write32(0xB828, 0x5A00);
		io_portmap_write32(0xB82C, 0);
		io_portmap_write32(0xB830, 0x5C00);
		io_portmap_write32(0xB834, 0);
		w32(0x5800 + 0, 0x5E00);  /* desc 0: a WRITABLE buffer for the device's reply */
		w32(0x5800 + 4, 0);
		w32(0x5800 + 8, 8);
		w16(0x5800 + 12, 2);      /* flags: VIRTQ_DESC_F_WRITE */
		w16(0x5800 + 14, 0);
		w16(0x5A00 + 0, 0);
		w16(0x5A00 + 2, 1);       /* avail: idx = 1 */
		w16(0x5A00 + 4, 0);

		io_portmap_write16(0xB816, 2); /* queue_select = 2 */
		io_portmap_write16(0xB818, 0); /* queue_size = 0 -- the guest's move */
	}

	/* THE NOTIFY under test (steps 4 and 5) */
	io_portmap_write16(0xB906, 3);

	if (step == 5)
		fprintf(stderr, "probe: virtio field writes + the push_reply attack ran on\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "virtio") == 0)
		return mode_virtio(argc > 2 ? atoi(argv[2]) : 0);
	if (argc != 2) {
		fprintf(stderr, "usage: secfix1-probe <write|read|atapi|hash|virtio[3|4|5]>\n");
		return 2;
	}
	if (strcmp(argv[1], "write") == 0) return mode_write();
	if (strcmp(argv[1], "read") == 0) return mode_read();
	if (strcmp(argv[1], "atapi") == 0) return mode_atapi();
	if (strcmp(argv[1], "hash") == 0) return mode_hash();
	fprintf(stderr, "unknown mode %s\n", argv[1]);
	return 2;
}
