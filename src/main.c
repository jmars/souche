/*
 * src/main.c -- the v86c executable: the image loader, the run driver and the two
 * output documents the project's oracles consume.
 *
 *     v86c <image> <instruction-limit>              -> the M0 ORACLE CONTRACT transcript
 *     v86c <image> <instruction-limit> --fixture    -> the nasm differential fixture
 *     v86c <image> <instruction-limit> --vga-doc F  -> the same transcript + the VGA document
 *                                                      (VGA-U0; oracle/run-vga.mjs is the
 *                                                      reference leg of the same file)
 *
 * The first form is README.md's contract, byte for byte the shape `node oracle/run.mjs`
 * prints (keys sorted at every level, 2-space indent, one trailing newline, lowercase
 * 0x-prefixed zero-padded hex, decimal counts), so
 *
 *     diff <(build/v86c tests/test.img 1000000) <(node oracle/run.mjs tests/test.img 1000000)
 *
 * is the gate.  Exit 0 iff a transcript was produced: a guest CPU exception, a guest that
 * does not halt within the limit, or a stop inside the emulator all exit nonzero and print
 * NO transcript -- never a partial one.
 *
 * The second form is what `tests/nasm_run.py --actual "build/v86c {img} {limit}
 * --fixture"` consumes: the same gdb-extract-def document the native harness
 * (tests/mkfixture.c) and the reference leg (oracle/nasm_state.mjs) print, so one parser
 * and one comparison serve all three legs.  It is a SEPARATE MODE rather than a second
 * binary: the corpus is ELFs loaded at their own addresses while the transcript's
 * load/entry fields are the fixed M0 contract, so the two documents cannot come from the
 * same validation path.  In this mode an exception is reported IN the document
 * `(signal SIGxxx)`, exactly as mkfixture.c does, and the exit status mirrors
 * oracle/nasm_state.mjs (0 = halted, 3 = did not halt within the limit).
 *
 * THE LOADER is the reference's own bring-up, transcribed rather than re-invented:
 * CPU.prototype.load_multiboot / load_multiboot_option_rom (v86/src/cpu.js:1288-1570) and
 * its ELF branch (elf.js:98-183, cpu.js:1455-1515).  Both branches are implemented because
 * M0's image uses the address table and the nasm corpus is ELF; the README's bring-up
 * table is the address-table branch's result.
 */

#include <math.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* _exit: the host-fault handler's exit (cosmocc pulls it in anyway) */

#include "cpu/cpu.h"
#include "cpu/cpu_backend.h" /* S0: the engine seam -- --cpu-engine, the init, the stats seat */
#include "cpu/cpu_state.h"
#include "cpu/exceptions.h" /* guest_exception: the vector a stopped delivery recorded */
#include "cpu/exec_boundary.h"
#include "cpu/flags.h"
#include "cpu/instructions.h"
#include "cpu/memory.h"
#include "cpu/modrm.h"
#include "cpu/paging.h"
#include "cpu/uop_cache.h" /* PERF-4/M2: the --uop-cache switch and the counters */
#include "cpu/jit_codegen.h" /* JIT-PLAN: the block compiler's coverage counters (--jit-stats) */
#include "cpu/jit_mem.h"   /* JIT-SPIKE: the --jit switch and the executable-memory pool */
#include "hw/acpi_tables.h" /* ACPI-2: the firmware tables (--acpi) */
#include "hw/bus.h"    /* M2-2: the host end of the serial seam */
#include "hw/kernel.h" /* M2-2: the bzImage loader */
#include "hw/machine.h" /* M2-1: the device layer (the machine) */
#include "hw/uart.h"   /* M2-2: the serial ports */
#include "hw/vga.h"    /* VGA-U0: the device, its adapter and the --vga-doc document */

/* README.md ss2/ss3 -- the fixed machine geometry of the oracle contract, and run.js:38-39
 * for the fixture form. */
#define MEMORY_SIZE 0x200000u
#define LOAD_ADDRESS 0x100000u
#define ENTRY_ADDRESS 0x100020u
#define WINDOW_ADDRESS 0x101000u
#define WINDOW_BYTES 4096u
#define BSS 0x100000u
#define STACK_TOP 0x102000u

/* cpu.js:1299-1301 */
#define MULTIBOOT_HEADER_MAGIC 0x1BADB002u
#define MULTIBOOT_HEADER_MEMORY_INFO 0x2u
#define MULTIBOOT_HEADER_ADDRESS 0x10000u
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u
#define MULTIBOOT_SEARCH_BYTES 8192u
#define MULTIBOOT_INFO_STRUCT_LEN 116u
#define MULTIBOOT_INFO_ADDR 0x7C00u
#define ELF_MAGIC 0x464C457Fu

/* ------------------------------------------------------------------ *
 * SHA-256 (FIPS 180-4)
 *
 * The oracle contract needs sha256 of the image and of the 4 KiB window, and the port's
 * target is an APE on a libc that offers no digest.  Self-contained, no allocation.
 * ------------------------------------------------------------------ */
typedef struct {
	uint32_t h[8];
	uint64_t total;
	uint8_t block[64];
	size_t fill;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx *ctx, const uint8_t *p)
{
	uint32_t w[64];
	for (unsigned i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
		       ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
	}
	for (unsigned i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
	uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

	for (unsigned i = 0; i < 64; i++) {
		uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
	ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(sha256_ctx *ctx)
{
	static const uint32_t iv[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};
	memcpy(ctx->h, iv, sizeof(iv));
	ctx->total = 0;
	ctx->fill = 0;
}

static void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = data;
	ctx->total += len;

	while (len > 0) {
		size_t take = 64 - ctx->fill;
		if (take > len) take = len;
		memcpy(ctx->block + ctx->fill, p, take);
		ctx->fill += take;
		p += take;
		len -= take;
		if (ctx->fill == 64) {
			sha256_block(ctx, ctx->block);
			ctx->fill = 0;
		}
	}
}

static void sha256_final(sha256_ctx *ctx, uint8_t out[32])
{
	uint64_t bits = ctx->total * 8;
	uint8_t pad = 0x80;
	sha256_update(ctx, &pad, 1);
	uint8_t zero = 0;
	while (ctx->fill != 56) sha256_update(ctx, &zero, 1);
	uint8_t len_be[8];
	for (unsigned i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_update(ctx, len_be, 8);

	for (unsigned i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(ctx->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(ctx->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(ctx->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)ctx->h[i];
	}
}

static void print_sha256_to(FILE *out, const void *data, size_t len)
{
	sha256_ctx ctx;
	uint8_t digest[32];
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, digest);
	for (unsigned i = 0; i < 32; i++) fprintf(out, "%02x", digest[i]);
}

static void print_sha256(const void *data, size_t len) { print_sha256_to(stdout, data, len); }

/* VGA-U0: the same digest, as a 64-character string -- vga_doc_meta_t.image_sha256, so the
 * document's image field is the transcript's own value and the port keeps ONE sha256. */
static void sha256_hex(const void *data, size_t len, char out[65])
{
	static const char hex[] = "0123456789abcdef";
	sha256_ctx ctx;
	uint8_t digest[32];
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, digest);
	for (unsigned i = 0; i < 32; i++) {
		out[2 * i] = hex[digest[i] >> 4];
		out[2 * i + 1] = hex[digest[i] & 0xF];
	}
	out[64] = '\0';
}

/* ------------------------------------------------------------------ *
 * helpers
 * ------------------------------------------------------------------ */
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* README.md ss6: every machine-state word is a lowercase 0x-prefixed hex STRING, padded
 * to its width.  The reference builds these with `"0x" + ...padStart(digits, "0")`, so the
 * value is quoted in the JSON -- hx() prints the JSON token, not the bare number. */
static void hx(uint32_t value, unsigned digits)
{
	printf("\"0x%0*x\"", (int)digits, (unsigned)value);
}

static void die(const char *message)
{
	fprintf(stderr, "v86c: %s\n", message);
	exit(1);
}

/* ------------------------------------------------------------------ *
 * the loader -- cpu.js:1288-1570
 *
 * NOTE THE NAME: the loader's own blob writer is `loader_write_blob`.  cpu.js:299's
 * CPU.prototype.write_blob -- the same operation without the loader's overlap check -- is
 * memory.c's `write_blob` since M2-1 (the DMA controller's do_read is its first caller),
 * and the two names cannot both be `write_blob` in this translation unit.
 * ------------------------------------------------------------------ */
static void loader_write_blob(const uint8_t *blob, size_t length, uint32_t addr)
{
	if (length == 0) return;
	if (in_mapped_range(addr) || in_mapped_range(addr + (uint32_t)length - 1)) {
		die("image load region overlaps memory-mapped address space");
	}
	memcpy(mem8 + addr, blob, length);
}

/* cpu.js:1543-1570 -- the state multiboot leaves behind.  Factored out because both
 * branches end here. */
static void multiboot_set_state(uint32_t entrypoint)
{
	write32(MULTIBOOT_INFO_ADDR, 0); /* flags bit 1/0: no memory map, no cmdline, no initrd */

	write_reg32(EBX, (int32_t)MULTIBOOT_INFO_ADDR);
	write_reg32(EAX, (int32_t)MULTIBOOT_BOOTLOADER_MAGIC); /* cpu.js:1283-1284 (port 0xF4) */
	cpu->cr[0] = 1;
	cpu->protected_mode = true;
	cpu->flags = FLAGS_DEFAULT;
	cpu->is_32 = true;
	cpu->stack_size_32 = true;

	for (int i = 0; i < 6; i++) {
		cpu->segment_is_null[i] = false;
		cpu->segment_offsets[i] = 0;
		cpu->segment_limits[i] = 0xFFFFFFFFu;
		cpu->sreg[i] = (uint16_t)(i == CS ? 0x08 : 0x10);
	}
	/* S3: through the fetch-cursor store (cpu_state.h), like every other producer of an eip:
	 * get_seg_cs() is 64-bit now, and a bare assignment would let a 32-bit entry point with
	 * bit 31 set sign-extend into the field.  Outside long mode the store projects, which is
	 * upstream's i32 arithmetic; main.c's own entry point is the boot's, and the invariant at
	 * the first batch boundary is what would name a mistake here. */
	set_instruction_pointer(get_seg_cs() + (uint32_t)entrypoint);
	update_state_flags();
}

static uint32_t load_address_table(const uint8_t *image, size_t image_size,
	const uint8_t *search, size_t offset)
{
	uint32_t header_addr = rd32(search + offset + 12);
	uint32_t load_addr = rd32(search + offset + 16);
	uint32_t load_end_addr = rd32(search + offset + 20);
	uint32_t bss_end_addr = rd32(search + offset + 24);
	uint32_t entry_addr = rd32(search + offset + 28);

	if (load_addr > header_addr) die("multiboot: load_addr > header_addr");
	(void)bss_end_addr;

	int64_t file_start = (int64_t)offset - ((int64_t)header_addr - (int64_t)load_addr);
	if (file_start < 0) die("multiboot: address table points before the file");

	/* load_end_addr == 0 means "the whole buffer from file_start" */
	uint32_t length = (load_end_addr == 0)
		? (uint32_t)(image_size - (size_t)file_start)
		: (load_end_addr - load_addr);
	if (load_end_addr != 0 && load_end_addr < load_addr) die("multiboot: load_end_addr < load_addr");
	if ((size_t)file_start + length > image_size) die("multiboot: image ends inside the loaded blob");

	loader_write_blob(image + file_start, length, load_addr);
	return entry_addr;
}

static uint32_t load_elf(const uint8_t *image, size_t size)
{
	if (size < 52) die("elf: file shorter than an ELF header");
	if (rd32(image) != ELF_MAGIC) die("elf: bad magic");
	if (image[4] != 1) die("elf: not 32-bit");
	if (image[5] != 1) die("elf: not little-endian");
	if (rd16(image + 16) != 2) die("elf: not an executable (the reference asserts this too)");

	uint32_t entry = rd32(image + 24);
	uint32_t phoff = rd32(image + 28);
	uint16_t phentsize = rd16(image + 42);
	uint16_t phnum = rd16(image + 44);

	uint32_t entrypoint = entry;

	for (uint16_t i = 0; i < phnum; i++) {
		size_t at = (size_t)phoff + (size_t)phentsize * i;
		if (at + 32 > size) die("elf: program header past the end of the file");

		uint32_t type = rd32(image + at);
		uint32_t offset = rd32(image + at + 4);
		uint32_t vaddr = rd32(image + at + 8);
		uint32_t paddr = rd32(image + at + 12);
		uint32_t filesz = rd32(image + at + 16);
		uint32_t memsz = rd32(image + at + 20);

		/* cpu.js:1471-1490: type 1 loads; 0 and the note/dynamic/interp/tls/e_frame/
		 * stack/relro/property types are skipped.  The reference's assert on anything
		 * else is upgraded to a hard error: an unknown segment is exactly the case where
		 * continuing would silently mis-load the guest. */
		if (type == 0) continue;
		if (type == 1) {
			if ((uint64_t)paddr + memsz >= cpu->memory_size) {
				fprintf(stderr, "v86c: warning: skipping ELF segment paddr=0x%x memsz=0x%x\n",
					(unsigned)paddr, (unsigned)memsz);
				continue;
			}
			if (filesz != 0) {
				if ((size_t)offset + filesz > size) die("elf: segment past the end of the file");
				loader_write_blob(image + offset, filesz, paddr);
			}
			if (entrypoint == entry && vaddr <= entrypoint && vaddr + memsz > entrypoint) {
				entrypoint = (entrypoint - vaddr) + paddr;
			}
			continue;
		}
		if (type == 2 || type == 3 || type == 4 || type == 6 || type == 7 ||
			type == 0x6474e550u || type == 0x6474e551u || type == 0x6474e552u || type == 0x6474e553u) {
			continue;
		}
		die("elf: unimplemented program header type");
	}

	return entrypoint;
}

/* cpu.js:1306-1345: find the multiboot header, then the address-table or the ELF branch.
 * `entrypoint` out-parameter; returns false if there is no bootable image at all. */
static bool load_image(const uint8_t *image, size_t size, uint32_t *entrypoint)
{
	uint8_t search[MULTIBOOT_SEARCH_BYTES];
	memset(search, 0, sizeof(search));
	if (size < sizeof(search)) memcpy(search, image, size);
	else memcpy(search, image, sizeof(search));

	/* the header needs 12 bytes to validate the checksum, so the scan stops there */
	for (size_t offset = 0; offset + 12 <= sizeof(search); offset += 4) {
		if (rd32(search + offset) != MULTIBOOT_HEADER_MAGIC) continue;

		uint32_t flags = rd32(search + offset + 4);
		uint32_t checksum = rd32(search + offset + 8);
		if (MULTIBOOT_HEADER_MAGIC + flags + checksum != 0) {
			fprintf(stderr, "v86c: multiboot checksum check failed at offset %u\n", (unsigned)offset);
			continue;
		}

		/* cpu.js:1350: only bits 0, 1 and 16 are defined */
		if ((flags & ~MULTIBOOT_HEADER_ADDRESS & ~3u) != 0) {
			die("multiboot: unimplemented header flags");
		}
		if (flags & MULTIBOOT_HEADER_MEMORY_INFO) {
			/* cpu.js:1381-1418 builds a memory map from the MMIO map, which is the M2
			 * device layer's.  Neither the M0 image nor the nasm corpus sets this bit;
			 * refusing is the honest answer. */
			die("multiboot: header flag bit 1 (memory map) is not implemented (M2)");
		}

		if (flags & MULTIBOOT_HEADER_ADDRESS) {
			*entrypoint = load_address_table(image, size, search, offset);
			return true;
		}
		if (rd32(image) == ELF_MAGIC) {
			*entrypoint = load_elf(image, size);
			return true;
		}
		die("multiboot: neither the address table nor an ELF header");
	}

	return false;
}

/* ------------------------------------------------------------------ *
 * the run driver -- oracle/run.mjs:104-129 (the bounded main_loop loop)
 * ------------------------------------------------------------------ */
typedef struct run_result {
	bool halted;
	int32_t exception; /* vector, or -1 */
	bool unimplemented;
	uint64_t count;
} run_result_t;

/* REBOOT-1: a guest that reboots forever is a loop, not a boot; the run is a
 * bounded-delivery harness and names the runaway instead of spinning in it. */
#define MAX_REBOOTS 16

/* Between the unwind (cpu.c/uop_cache.c) and boot 2's first instruction: the code
 * caches are cleared and the request flag is consumed.  THE ORDER IS LOAD-BEARING:
 * uop_cache_clear() drops the decoded blocks AND the compiled ones (and zeroes the
 * shared dirty-page bitmap), so the firmware/option-ROM re-copies the boot mode does
 * next are plain memcpy into memory no block is watching.  This is the reference's
 * own reset-time clear (cpu.rs:4757, jit::jit_clear_cache_js inside reset_cpu) --
 * the port dropped that line while it was interpreter-only, and the clear cannot run
 * inside machine_reboot because the executor holds arena pointers mid-block. */
static void reboot_prepare_reentry(void)
{
	/* S0: the clear is the selected ENGINE's (the contract's clear_caches), not a call into
	 * one engine's internals -- a second backend owes the same clear, and this is the call
	 * site the seam names.  All three instances do the same two things today: the caches are
	 * the machine's and every engine's guest writes invalidate them through the same hooks. */
	cpu_backend_current()->clear_caches();
	cpu->reboot_requested = false;
}

/* U64-COUNTER: the instruction limit is now u64 (a limit past 4 G was inexpressible while the
 * counter was a u32), so the parser returns u64.  The cap is INT64_MAX, not UINT64_MAX: the
 * run loop compares the count against the limit and against a signed -1 sentinel, so a limit
 * past 2^63 could not be compared safely. */
static void parse_positive_uint(const char *text, const char *what, uint64_t *out)
{
	char *end = NULL;
	unsigned long long value = strtoull(text, &end, 10);
	if (end == text || *end != '\0' || value == 0 || value > 0x7FFFFFFFFFFFFFFFull) {
		fprintf(stderr, "v86c: %s must be a positive integer, got: %s\n", what, text);
		exit(2);
	}
	*out = (uint64_t)value;
}

static run_result_t run(uint64_t limit)
{
	run_result_t result;
	memset(&result, 0, sizeof(result));
	result.exception = -1;

	/* `previous_count = -1` is a signed counter upstream: `count === previous_count`
	 * detects a main_loop() that made no progress at all.  count is a u64 but always far
	 * below 2^63, so the signed comparison against the -1 sentinel is exact. */
	int64_t previous_count = -1;
	for (;;) {
		main_loop();
		uint64_t count = cpu->instruction_counter;
		if (cpu->in_hlt || guest_exception >= 0 || interpreted_unimplemented
			|| cpu->reboot_requested
			|| count >= limit || (int64_t)count == previous_count) {
			break;
		}
		previous_count = (int64_t)count;
	}

	result.halted = cpu->in_hlt;
	result.exception = guest_exception;
	result.unimplemented = interpreted_unimplemented;
	result.count = cpu->instruction_counter;
	return result;
}

/* ------------------------------------------------------------------ *
 * THE M0 ORACLE CONTRACT TRANSCRIPT -- oracle/run.mjs:131-198
 * ------------------------------------------------------------------ */
static void print_transcript(const uint8_t *image, size_t image_size, uint64_t limit)
{
	printf("{\n");

	printf("  \"cpu\": {\n");
	printf("    \"control_registers\": {\n");
	printf("      \"cr0\": "); hx((uint32_t)cpu->cr[0], 8); printf(",\n");
	printf("      \"cr2\": "); hx((uint32_t)cpu->cr[2], 8); printf(",\n");
	printf("      \"cr3\": "); hx((uint32_t)cpu->cr[3], 8); printf("\n");
	printf("    },\n");
	printf("    \"eflags\": "); hx((uint32_t)get_eflags(), 8); printf(",\n");
	printf("    \"eip\": "); hx((uint32_t)cpu->instruction_pointer, 8); printf(",\n");
	printf("    \"gpr\": {\n");
	/* the JSON keys sort lexicographically at every level (README.md ss6), so this is
	 * alphabetical order, not const.js's register order: ebp < ebx. */
	static const struct { const char *name; int index; } gprs[] = {
		{ "eax", EAX }, { "ebp", EBP }, { "ebx", EBX }, { "ecx", ECX },
		{ "edi", EDI }, { "edx", EDX }, { "esi", ESI }, { "esp", ESP },
	};
	for (unsigned i = 0; i < 8; i++) {
		printf("      \"%s\": ", gprs[i].name);
		hx((uint32_t)cpu->reg64[gprs[i].index], 8);
		printf("%s\n", i == 7 ? "" : ",");
	}
	printf("    },\n");
	printf("    \"segments\": {\n");
	/* const.js:96-101 order ES,CS,SS,DS,FS,GS; the JSON keys sort to cs,ds,es,fs,gs,ss */
	static const struct { const char *name; int index; } segs[] = {
		{ "cs", CS }, { "ds", DS }, { "es", ES }, { "fs", FS }, { "gs", GS }, { "ss", SS },
	};
	for (unsigned i = 0; i < 6; i++) {
		int s = segs[i].index;
		printf("      \"%s\": {\n", segs[i].name);
		printf("        \"base\": "); hx((uint32_t)cpu->segment_offsets[s], 8); printf(",\n");
		printf("        \"limit\": "); hx(cpu->segment_limits[s], 8); printf(",\n");
		printf("        \"selector\": "); hx(cpu->sreg[s], 4); printf("\n");
		printf("      }%s\n", i == 5 ? "" : ",");
	}
	printf("    },\n");
	printf("    \"x87\": {\n");
	printf("      \"control_word\": "); hx(cpu->fpu_control_word, 4); printf(",\n");
	printf("      \"data\": [\n");
	for (unsigned i = 0; i < 8; i++) {
		double value = fpu_get_sti_f64((int32_t)i);
		uint64_t bits;
		memcpy(&bits, &value, sizeof(bits));
		/* f64_hex (run.mjs:132-137) is hx(high,8) + hx(low,8), and each hx carries its
		 * own "0x" -- so the reference prints a doubled prefix.  Reproduced verbatim: a
		 * transcript diff is the gate, and "the reference's quirk" is the contract. */
		printf("        \"0x%08x0x%08x\"%s\n",
			(unsigned)(bits >> 32), (unsigned)bits, i == 7 ? "" : ",");
	}
	printf("      ],\n");
	printf("      \"stack_empty\": "); hx(cpu->fpu_stack_empty, 2); printf(",\n");
	printf("      \"stack_ptr\": "); hx(cpu->fpu_stack_ptr, 4); printf(",\n");
	printf("      \"status_word\": "); hx(fpu_load_status_word(), 4); printf(",\n");
	printf("      \"tag_word\": "); hx((uint32_t)fpu_load_tag_word(), 4); printf("\n");
	printf("    }\n");
	printf("  },\n");

	printf("  \"image\": {\n");
	printf("    \"bytes\": %zu,\n", image_size);
	printf("    \"entry_address\": "); hx(ENTRY_ADDRESS, 8); printf(",\n");
	printf("    \"load_address\": "); hx(LOAD_ADDRESS, 8); printf(",\n");
	printf("    \"sha256\": \""); print_sha256(image, image_size); printf("\"\n");
	printf("  },\n");

	/* U64-COUNTER: %llu, not %u -- for a count below 2^32 the decimal bytes are identical to
	 * %u (the transcript-diff gate's contract), and past 2^32 the true count is printed
	 * rather than the wrapped low 32 bits. */
	printf("  \"instruction_count\": %llu,\n", (unsigned long long)cpu->instruction_counter);
	printf("  \"instruction_limit\": %llu,\n", (unsigned long long)limit);

	printf("  \"memory_window\": {\n");
	printf("    \"address\": "); hx(WINDOW_ADDRESS, 8); printf(",\n");
	printf("    \"bytes\": %u,\n", WINDOW_BYTES);
	printf("    \"sha256\": \""); print_sha256(mem8 + WINDOW_ADDRESS, WINDOW_BYTES); printf("\"\n");
	printf("  }\n");

	printf("}\n");
}

/* ------------------------------------------------------------------ *
 * THE NASM FIXTURE -- oracle/nasm_state.mjs:138-172 / tests/mkfixture.c
 * ------------------------------------------------------------------ */
static const char *exception_signal(int32_t vector)
{
	switch (vector) {
	case CPU_EXCEPTION_DE: return "SIGFPE";
	case CPU_EXCEPTION_UD: return "SIGILL";
	case CPU_EXCEPTION_GP: return "SIGSEGV";
	case CPU_EXCEPTION_PF: return "SIGBUS";
	default: return NULL;
	}
}

static void print_float(double v)
{
	if (v != v) { fputs("nan", stdout); return; }
	if (v == (double)INFINITY) { fputs("inf", stdout); return; }
	if (v == -(double)INFINITY) { fputs("-inf", stdout); return; }
	printf("%.17g", v); /* JS Number.toString is shortest-round-trip; %.17g round-trips too */
}

/* ------------------------------------------------------------------ *
 * S4 -- THE LONG-MODE DOCUMENT'S CONTROL TAIL (plan ss5.4)
 *
 * `--fixture64` prints TWO sections: the compared v2 array (the corpus's actual leg) and
 * this tail, which the corpus comparator IGNORES.  Its consumers are the things that need
 * a machine's whole state rather than a comparison: the 64-bit ENGINE differential (interp
 * vs uop vs jit, byte for byte -- S13) and the KVM lane's state translation
 * (results/kvm-plan.md ss4 T1 asks for exactly one canonical state document).  Printing it
 * now is deliberate: the document's SHAPE is what those consumers will be written against,
 * and a shape invented at their moment of need is a shape nobody checked.
 *
 * WHAT IS NOT IN IT YET, so its absence is not read as a promise: the FS/GS/KERNEL_GS base
 * MSRs and the long-mode MSR set are S11's (the widened segment_offsets[] slots are the
 * seat they land in); cr[3] is a 32-bit PML4 base only because the model's physical space
 * is 32-bit (ss5.1); and `rflags` appears in the compared array above, not here.
 * ------------------------------------------------------------------ */
static const char *document_mode_name(uint32_t mode)
{
	switch (mode) {
	case CPU_MODE_REAL: return "real";
	case CPU_MODE_PROT16: return "prot16";
	case CPU_MODE_PROT32: return "prot32";
	case CPU_MODE_LONG: return "long";
	case CPU_MODE_COMPAT: return "compat";
	default: return "unknown";
	}
}

static void print_state_tail(void)
{
	printf("---BEGIN STATE---\n{\n");
	printf("  \"mode\": \"%s\",\n", document_mode_name(cpu_mode_of(cpu)));
	printf("  \"efer\": \"0x%llx\",\n", (unsigned long long)cpu->efer);
	printf("  \"cr\": [");
	for (int i = 0; i < 5; i++) printf("%s\"0x%x\"", i ? ", " : "", (unsigned)cpu->cr[i]);
	printf("],\n");
	printf("  \"gdtr\": { \"base\": \"0x%llx\", \"limit\": %d },\n",
	       (unsigned long long)cpu->gdtr_offset, (int)cpu->gdtr_size);
	printf("  \"idtr\": { \"base\": \"0x%llx\", \"limit\": %d },\n",
	       (unsigned long long)cpu->idtr_offset, (int)cpu->idtr_size);
	printf("  \"segments\": [");
	for (int i = 0; i < 6; i++) {
		printf("%s{ \"selector\": \"0x%x\", \"base\": \"0x%llx\", \"limit\": \"0x%x\" }",
		       i ? ", " : "", (unsigned)cpu->sreg[i],
		       (unsigned long long)cpu->segment_offsets[i],
		       (unsigned)cpu->segment_limits[i]);
	}
	printf("],\n");
	printf("  \"sysenter\": { \"cs\": \"0x%x\", \"esp\": \"0x%x\", \"eip\": \"0x%x\" },\n",
	       (unsigned)cpu->sysenter_cs, (unsigned)cpu->sysenter_esp,
	       (unsigned)cpu->sysenter_eip);
	printf("  \"state_flags\": \"0x%x\",\n", (unsigned)cpu->state_flags);
	printf("  \"instruction_pointer\": \"0x%llx\",\n",
	       (unsigned long long)cpu->instruction_pointer);
	printf("  \"previous_ip\": \"0x%llx\",\n", (unsigned long long)cpu->previous_ip);
	printf("  \"instruction_count\": %llu\n", (unsigned long long)cpu->instruction_counter);
	printf("}\n---END STATE---\n");
}

/* S4: THE TWO DOCUMENTS, ONE PRINTER.  `wide` selects the x86-64 corpus's format v2
 * (tests/mkfixture64.c, results/x86-64-plan.md ss5.4): sixteen 64-bit GPRs and a 64-bit
 * `rip`, then the SAME x87/MMX/XMM/memory block and flag word.  The 32-bit document is
 * untouched -- every value it prints is the same expression, and the arity stays 2116 --
 * because the M0 transcript and the nasm differential's 2116-check are contracts, not
 * conventions.
 *
 * WHAT THE WIDE DOCUMENT COULD NOT SHOW UNTIL S5a, kept here because it was the reason the
 * instrument existed before the machine could fill it: R8-R15 DID NOT EXIST IN THE MODEL
 * (reg64[] was eight slots), so the sixteen-element register block printed their eight slots
 * as ZERO and an emulated run of a case that USED r8-r15 had to disagree with the native leg
 * -- one of the mechanical ways a corpus family stays RED.  S5a widens the array to sixteen
 * (plan correction GAP 1) and prints all of them from the register file. */
static void print_fixture(bool wide)
{
	int32_t fpu_tag = fpu_load_tag_word();
	uint16_t fpu_status = fpu_load_status_word();
	int32_t top = fpu_status >> 11 & 7;

	/* THE FLAG DOES NOT PRETEND TO HAVE RUN A 64-BIT IMAGE.  The port cannot execute a
	 * long-mode image yet (S5 is the first slice that can), so a run that STOPS BY NAME
	 * never reaches here -- `--fixture64` on a corpus image exits 1 with
	 * `unimplemented: EFER.LME asks for IA-32e...` before any document is printed, and
	 * that IS the refusal.  What reaches here is a machine that stopped in a mode this
	 * model has, and the document says so: the control tail names the mode, and this note
	 * names the projection in the one place a reader would otherwise mistake the wide
	 * register block for a 64-bit state. */
	if (wide && (cpu->efer & EFER_LMA) == 0) {
		fprintf(stderr, "v86c: --fixture64: the machine is NOT in long mode (efer=0x%llx,"
		                " mode=%s); the v2 register block's high halves are the S3"
		                " projection and r8-r15 do not exist yet (S5)\n",
		        (unsigned long long)cpu->efer, document_mode_name(cpu_mode_of(cpu)));
	}

	if (guest_exception >= 0) {
		const char *signal = exception_signal(guest_exception);
		if (signal == NULL) {
			fprintf(stderr, "v86c: unexpected CPU exception: %d\n", guest_exception);
			exit(1);
		}
		printf("(signal %s)\n", signal);
	}

	printf("---BEGIN JSON---\n[\n");
	if (wide) {
		/* format v2: the 16 GPRs of the native document (tests/mkfixture64.c), in
		 * reg64[]'s order, as SIGNED decimal of the 64-bit value -- the same
		 * convention on both sides, so the comparison never has to guess a width.
		 * S5a FILLS THE SEAT S4 PRINTED AS ZERO: reg64[] is sixteen slots now, so
		 * slots 8-15 are r8-r15 and are printed from the register file like the other
		 * eight.  A machine that has not entered long mode still prints ZERO there
		 * (nothing outside long mode can write slots 8-15 -- the REX bits that index
		 * them are only set by the long-mode prefix handler), which is why this line
		 * moves no 32-bit document. */
		for (int i = 0; i < 16; i++) printf("    %lld,\n", (long long)cpu->reg64[i]);
		printf("    %lld,\n", (long long)cpu->instruction_pointer);
	} else {
		/* S1: the register file is 64-bit now; the fixture document is a 32-bit
		 * document (tests/mkfixture.c's `reg32[8]`), so the value printed is the low
		 * half, exactly as before -- (int32_t)(uint32_t) reproduces both the %d width
		 * AND the sign. */
		for (int i = 0; i < 8; i++) printf("    %d,\n", (int32_t)(uint32_t)cpu->reg64[i]);
		/* S3: the eip is a uint64_t field; this is a 32-bit document
		 * (tests/mkfixture.c's `reg32[8]`), so the value printed is the low half with
		 * %d's own sign -- exactly the bytes the old `%d` of an int32_t field
		 * produced. */
		printf("    %d,\n", (int32_t)(uint32_t)cpu->instruction_pointer);
	}

	for (int i = 0; i < 8; i++) {
		int32_t phys = (top + i) & 7;
		if ((fpu_tag >> 2 * phys & 3) == 2) {
			printf("    \"invalid\",\n");
		} else {
			printf("    ");
			print_float(fpu_get_sti_f64(i));
			printf(",\n");
		}
	}
	/* mm: the low 8 bytes of each fpu_st slot, as i32 (nasm_state.mjs:127) */
	for (int i = 0; i < 16; i++) {
		int32_t word;
		memcpy(&word, (const uint8_t *)&cpu->fpu_st[i >> 1] + 4 * (i & 1), sizeof(word));
		printf("    %d,\n", word);
	}
	for (int i = 0; i < 32; i++) printf("    %d,\n", cpu->reg_xmm[i >> 2].i32[i & 3]);

	for (unsigned i = 0; i < (STACK_TOP - BSS) / 4; i++) {
		int32_t word;
		memcpy(&word, mem8 + BSS + 4 * i, sizeof(word));
		printf("    %d%s", word, (i % 8 == 7) ? ",\n" : ", ");
	}

	printf("    %d,\n", (int)get_eflags());
	printf("    %d,\n", (int)fpu_tag);
	printf("    %u\n", (unsigned)fpu_status);
	printf("]\n---END JSON---\n");

	if (wide) print_state_tail();
}

/* ------------------------------------------------------------------ *
 * M2-2 -- THE bzImage BOOT MODE
 *
 *     v86c --bzimage <kernel> <instruction-limit> [--cmdline TEXT] [--initrd FILE]
 *          [--bios FILE] [--serial FILE] [--uart1]
 *
 * WHAT THIS MODE IS: the same machine, brought up the way the reference's baliveau lane
 * brings it up -- `load_kernel`'s memory layout and boot params (src/hw/kernel.c, the port
 * of v86/src/kernel.js) instead of the multiboot/ELF loader, and a 64 MiB machine instead
 * of the transcript's 2 MiB.  The loader's OPTION ROM is what enters the kernel, exactly as
 * it does upstream: the host copies it to 0xC0000 (which is where seabios copies it, and
 * where the reference's fw_cfg hands it over) and resets the CPU into a firmware image at
 * the top of the first MiB -- the same place v86's load_bios puts a BIOS.
 *
 * THE FIRMWARE IS A FILE, NOT A PORT OF SEA BIOS: v86 has no BIOS of its own (load_kernel's
 * ROM says so in its own header, kernel.js:183), and seabios needs the VGA and the IDE
 * controller this port has not got.  tests/minibios.asm is the 64 KiB 16-bit shim BOTH
 * machines run -- it is assembled into build/minibios.bin, embedded here by
 * gen/minibios_bin.py, and handed to the reference harness as the same file -- so the two
 * legs differ in the CPU and the devices, not in the firmware.  See results/m2-2-boot.md.
 *
 * THE M0 ORACLE CONTRACT IS UNTOUCHED: this mode is reached only by its own flag, it prints
 * the guest's SERIAL BYTES on stdout instead of a transcript, and the two-argument
 * invocation below is byte-for-byte what it was.
 * ------------------------------------------------------------------ */
#define BOOT_MEMORY_SIZE 0x4000000u /* 64 MiB: the geometry tests/minibios.asm's E820 reports */
#define BOOT_FIRMWARE_ADDRESS 0xF0000u /* the firmware fills 0xF0000-0xFFFFF */
#define BOOT_OPTION_ROM_ADDRESS 0xC0000u /* where seabios puts a fw_cfg option ROM */
/* M2-3: the machine size is overridable (`--memory MIB`) because an initrd forces it.
 * Upstream's load_kernel puts the initrd at the fixed 64 MiB boundary and says so in its
 * own comment -- "the minimum memory size is 64 MB plus the size of the initrd"
 * (kernel.js:48-50) -- so a boot that carries an initramfs needs a machine whose reported
 * memory reaches past it.  The FIRMWARE must agree (tests/minibios.asm's MEM_TOP), which
 * is why `--memory` pairs with `--bios build/minibios-<n>m.bin`; the default below is
 * M2-2's, so that boot is byte-for-byte what it was. */

extern const unsigned char v86c_minibios_image[];
extern const unsigned int v86c_minibios_image_len;

static FILE *boot_serial_sink = NULL;

/* ------------------------------------------------------------------ *
 * A HOST FAULT IS A PORT BUG, AND IT MUST SAY SO.
 *
 * A guest's own fault is the machine's business (exceptions.c delivers it).  A SIGSEGV in the
 * PORT is a defect, and what makes it actionable is the guest state at the moment it happened
 * plus the ADDRESS the fault touched: the instruction pointer, the segment bases, the
 * registers, the instruction count -- and, with SA_SIGINFO, the host RIP, which tells a bad
 * DATA access from a JUMP through a wild function pointer.  (M2-4 needed exactly that
 * distinction: the faulting address EQUALled the instruction pointer, i.e. a call through a
 * dangling field table, and the symbol table over build/v86c.com.dbg named the caller.)
 *
 * Installed by the boot mode only, so the transcript mode's contract -- "a stop exits nonzero
 * and prints NO transcript" -- is untouched; nothing here changes a guest-visible value, it
 * only reports one.  (This is how the M2-2 unit's own host fault was localised: `--bzimage ...
 * <limit>` bisected the instruction count, and this told the eip.)
 * ------------------------------------------------------------------ */
static void boot_host_fault(int signal, siginfo_t *info, void *context)
{
	fprintf(stderr, "\nv86c: HOST FAULT (%d) at address %p (si_code=%d), instruction %llu\n",
	        signal, info->si_addr, info->si_code,
	        (unsigned long long)cpu->instruction_counter);
#ifdef __x86_64__
	{
		/* cosmocc's ucontext exposes the register file as named members (struct sigcontext's
		 * anonymous struct); REG_* are behind _GNU_SOURCE and are not needed.
		 *
		 * HOST CC: glibc's mcontext_t has no named members -- it is a `gregset_t gregs`
		 * array, indexed by REG_RSP=15 / REG_RIP=16 / REG_RBP=10 (sys/ucontext.h).  The
		 * indices are written out rather than using REG_* so this file needs no
		 * _GNU_SOURCE, which cosmocc's headers would see as a preprocessor change.
		 * Reachable only on a HOST fault, which this port treats as its own defect
		 * (never a guest-visible path), and only under the host compiler. */
#ifdef __GLIBC__
#define BOOT_UC_RSP uc->uc_mcontext.gregs[15]
#define BOOT_UC_RIP uc->uc_mcontext.gregs[16]
#define BOOT_UC_RBP uc->uc_mcontext.gregs[10]
#else
#define BOOT_UC_RSP uc->uc_mcontext.rsp
#define BOOT_UC_RIP uc->uc_mcontext.rip
#define BOOT_UC_RBP uc->uc_mcontext.rbp
#endif
		ucontext_t *uc = context;
		const unsigned long long *stack =
		    (const unsigned long long *)(uintptr_t)BOOT_UC_RSP;
		fprintf(stderr, "v86c: HOST RIP=0x%llx RSP=0x%llx RBP=0x%llx stack:",
		        (unsigned long long)BOOT_UC_RIP,
		        (unsigned long long)BOOT_UC_RSP,
		        (unsigned long long)BOOT_UC_RBP);
		for (int i = 0; i < 6; i++) fprintf(stderr, " %llx", stack[i]);
		fputc('\n', stderr);
	}
#else
	(void)context;
#endif
	fprintf(stderr, "v86c: eip=0x%x cs=0x%x (base 0x%x) cr0=0x%x cr3=0x%x"
	                " protected=%d is_32=%d cpl=%d\n",
	        (unsigned)cpu->instruction_pointer, (unsigned)cpu->sreg[CS],
	        (unsigned)cpu->segment_offsets[CS], (unsigned)cpu->cr[0], (unsigned)cpu->cr[3],
	        cpu->protected_mode ? 1 : 0, cpu->is_32 ? 1 : 0, cpu->cpl);
	fprintf(stderr, "v86c: eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x esi=0x%08x edi=0x%08x"
	                " ebp=0x%08x esp=0x%08x\n",
	        (unsigned)(uint32_t)cpu->reg64[EAX], (unsigned)(uint32_t)cpu->reg64[EBX],
	        (unsigned)(uint32_t)cpu->reg64[ECX], (unsigned)(uint32_t)cpu->reg64[EDX],
	        (unsigned)(uint32_t)cpu->reg64[ESI], (unsigned)(uint32_t)cpu->reg64[EDI],
	        (unsigned)(uint32_t)cpu->reg64[EBP], (unsigned)(uint32_t)cpu->reg64[ESP]);
	_exit(128 + signal);
}

static void boot_install_fault_handlers(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = boot_host_fault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
}

/* the host end of the serial seam: the UART sends "serial0-output-byte" on the device bus
 * (uart.c), the machine pairs it with machine_host_bus, and this writes the byte out. */
static void boot_serial_output(void *this_value, const int32_t *args, int nargs)
{
	(void)this_value;
	if (nargs < 1 || boot_serial_sink == NULL) return;
	fputc(args[0] & 0xFF, boot_serial_sink);
}

/* ------------------------------------------------------------------ *
 * M2-4 -- THE VIRTIO CONSOLE'S HOST SEAM, and the observation surface the gate reads.
 *
 * THE SAME SEAM SHAPE uart.c has, for the same reason (uart.h's header says it): the device
 * sends its bytes on the device bus and a HOST listener writes them out.  Upstream's console
 * sends ONE message per guest buffer carrying a Uint8Array ("virtio-console0-output-bytes",
 * virtio_console.js:100) and the port's bus carries int32 argument vectors, so the port's
 * console sends one message PER BYTE at the same message name (virtio_console.h's D-VC1).
 * The byte stream is identical; this listener is what turns it back into a stream.
 *
 * THE COUNTERS ARE THE REFERENCE HARNESS'S OWN NUMBER: oracle/run-boot.mjs counts the bytes
 * its "virtio-console0-output-bytes" adapter receives and prints how many the guest wrote
 * (run-boot.mjs's `serial_bytes` line, by the same argument for the serial port).  Writing
 * them at the end is how a caller sees the console was used without diffing files.
 * ------------------------------------------------------------------ */
static FILE *boot_virtio_console_sink = NULL;
static uint64_t boot_virtio_console_out_bytes = 0;
static uint64_t boot_virtio_console_in_bytes = 0;

/* The inbound half: the guest announces it is listening (its /dev/hvc0 is open and the driver
 * has posted receive buffers) and the host answers with the bytes of --virtio-console-in.
 * A handshake is needed because upstream's own input path DROPS a byte that arrives with no
 * posted receive buffer (virtio_console.js:213-215's `else { //TODO: Buffer }`), and at the
 * time the harness starts the guest has posted none. */
static const char boot_virtio_console_sentinel[] = "GUEST-INIT: waiting for host input";
static char boot_virtio_console_window[sizeof(boot_virtio_console_sentinel) - 1];
static uint32_t boot_virtio_console_window_fill = 0;
static uint8_t *boot_virtio_console_in = NULL;
static uint32_t boot_virtio_console_in_length = 0;
static bool boot_virtio_console_in_sent = false;

static void boot_virtio_console_send_input(void)
{
	for (uint32_t i = 0; i < boot_virtio_console_in_length; i++) {
		int32_t byte = boot_virtio_console_in[i];
		bus_send(machine_host_bus, "virtio-console0-input-bytes", &byte, 1);
		boot_virtio_console_in_bytes++;
	}
	boot_virtio_console_in_sent = true;
}

static void boot_virtio_console_output(void *this_value, const int32_t *args, int nargs)
{
	(void)this_value;
	if (nargs < 1 || boot_virtio_console_sink == NULL) return;
	int byte = args[0] & 0xFF;
	fputc(byte, boot_virtio_console_sink);
	boot_virtio_console_out_bytes++;

	if (boot_virtio_console_in_sent || boot_virtio_console_in_length == 0) return;

	memmove(boot_virtio_console_window, boot_virtio_console_window + 1,
	        sizeof(boot_virtio_console_window) - 1);
	boot_virtio_console_window[sizeof(boot_virtio_console_window) - 1] = (char)byte;
	if (boot_virtio_console_window_fill < sizeof(boot_virtio_console_window)) {
		boot_virtio_console_window_fill++;
	}
	if (boot_virtio_console_window_fill == sizeof(boot_virtio_console_window) &&
	    memcmp(boot_virtio_console_window, boot_virtio_console_sentinel,
	           sizeof(boot_virtio_console_window)) == 0) {
		boot_virtio_console_send_input();
	}
}

static uint8_t *read_file(const char *path, uint32_t *length)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "v86c: cannot open %s\n", path);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size <= 0) {
		fprintf(stderr, "v86c: empty or unreadable file: %s\n", path);
		exit(1);
	}
	uint8_t *buffer = malloc((size_t)size);
	if (buffer == NULL) {
		fprintf(stderr, "v86c: out of memory reading %s\n", path);
		exit(1);
	}
	if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
		fprintf(stderr, "v86c: short read on %s\n", path);
		exit(1);
	}
	fclose(file);
	*length = (uint32_t)size;
	return buffer;
}

/* PERF-4/M2: set by --uop-cache-stats.  The counters go to STDERR, never stdout: stdout is
 * the oracle transcript and the boot's serial sink, and a stats line in either would be a
 * gate failure that has nothing to do with the cache. */
static bool uop_cache_stats = false;

/* The M2 measurement switches, shared by both argument parsers (the transcript/fixture mode
 * and the --bzimage boot mode): `--uop-cache on|off` is the plan's ss8 step 5 flag -- OFF
 * must reproduce the pre-M2 bytes exactly -- and `--uop-cache-stats` prints the plan's ss6.2
 * numbers (the hit rate, the fallback fraction, invalidations, clears) at exit. */
/* JIT-SPIKE (HANDOFF=v86c-jit): THE `--jit on|off` SWITCH, DEFAULT OFF.
 *
 * The flag exists so the substrate can be reached from the shipped binary without being ON in
 * it: `--jit off` (and no flag at all) is today's machine, byte for byte.  Turning it on calls
 * jit_mem_init, which reports what it did and what it could not do (src/cpu/jit_mem.h: the
 * W^X path is VERIFIED on Linux, UNVERIFIED on macOS/Windows, and there is no aarch64 backend).
 * The INIT is deliberately deferred out of the parse (see the two call sites): the flag's two
 * halves may be given in either order, and jit_mem_init refuses when the uop cache is off --
 * the uop cache's structures are the JIT's dirty-page bitmap and its invalidation entry point.
 *
 * `--jit-stats` is the same switch as `--uop-cache-stats` (both print at exit, both to stderr,
 * so neither can contaminate the transcript or the guest's serial sink). */
static bool jit_stats_flag = false;

static int parse_jit_flag(int argc, char *argv[], int i)
{
	if (strcmp(argv[i], "--jit-stats") == 0) {
		jit_stats_flag = true;
		return i;
	}
	if (i + 1 < argc) {
		if (strcmp(argv[i + 1], "on") == 0) {
			jit_enabled = true;
			return i + 1;
		}
		if (strcmp(argv[i + 1], "off") == 0) {
			jit_enabled = false;
			return i + 1;
		}
	}
	fprintf(stderr, "v86c: --jit needs on|off\n");
	exit(2);
}

/* The exit handler the two stats switches share.  A flag that was not given prints NOTHING, so
 * every pre-existing invocation's stderr is unchanged.
 *
 * S0: the engine's OWN counters are the selected backend's (`--uop-cache-stats` asks the
 * engine, and an engine with no counters of its own answers NULL).  The compiled path's
 * counters are not the engine's -- they are the JIT's, and `--jit-stats` is the flag that
 * names them, which is why they keep their own line here. */
static void v86c_stats_print(void)
{
	const cpu_backend_t *engine = cpu_backend_current();

	if (uop_cache_stats && engine->print_stats != NULL) engine->print_stats();
	if (jit_stats_flag || jit_enabled) {
		jit_stats_print();
		/* JIT-PLAN (unit JIT-1): the block compiler's own numbers -- how many blocks were
		 * compiled, and the DYNAMIC inlined-vs-called split that the plan's ss3 coverage
		 * target (M2 >= 85%, M3 >= 93%) is measured with. */
		jit_codegen_stats_print();
	}
}

/* S0: THE ENGINE'S INIT, THROUGH THE SEAM, at the site jit_init_if_enabled occupied.
 *
 * Called after the arguments are parsed and the machine's memory exists (both backends that
 * allocate anything need the guest RAM: the cache's dirty-page bitmap and the JIT's pool are
 * sized from mem8_alloc_size()), in both modes.
 *
 * THE CONTRACT'S OTHER HALF IS THE REFUSAL: a backend whose init returns false is REFUSED BY
 * NAME, the machine adopts the previous backend in the cascade and SAYS SO -- it does not
 * fail to start and it does not silently run something else.  On this host the refusal path
 * is the JIT's platform probe failing (jit_mem.h's caveats), which pre-seam printed only the
 * caps line; the run's behaviour is unchanged (it continues on the uop cache, exactly as
 * `--jit off` would) and the ENGINE is now named as well. */
static bool engine_init_if_needed(void)
{
	const cpu_backend_t *engine = cpu_backend_current();
	/* WHETHER THE COMPILED PATH WAS ASKED FOR, read before init may clear it: it decides the
	 * caps line's presence, and that line is stderr the pre-seam machine printed. */
	bool compiled_path_requested = jit_enabled;
	bool ready = engine->init();

	if (!ready) {
		const cpu_backend_t *fallback = cpu_backend_fallback(engine);
		if (fallback != NULL) cpu_backend_adopt(fallback);
		else jit_enabled = false;
	}
	if (compiled_path_requested) fprintf(stderr, "v86c: %s\n", jit_mem_caps());
	if (!ready) {
		fprintf(stderr, "v86c: cpu engine '%s' could not initialise; running the '%s'"
		                " engine\n", engine->name, cpu_backend_current()->name);
	}
	return ready;
}

static int parse_uop_cache_flag(int argc, char *argv[], int i)
{
	if (strcmp(argv[i], "--uop-cache-stats") == 0) {
		uop_cache_stats = true;
		return i;
	}
	if (i + 1 < argc) {
		if (strcmp(argv[i + 1], "on") == 0) {
			uop_cache_enabled = true;
			return i + 1;
		}
		if (strcmp(argv[i + 1], "off") == 0) {
			uop_cache_enabled = false;
			return i + 1;
		}
	}
	fprintf(stderr, "v86c: --uop-cache needs on|off\n");
	exit(2);
	return i;
}

/* ------------------------------------------------------------------ *
 * S0: THE ENGINE SEAM'S USER-VISIBLE SPELLING (results/x86-64-plan.md ss3.4)
 *
 * The seam this slice lands has three instances -- `interp`, `uop`, `uopjit` -- and the two
 * existing flags already SELECT two of their three things: `--uop-cache off` is the
 * interpreter, `--jit off` is uop_run with the compiled path held down, and the default is
 * uop_run with it on.  `--cpu-engine <name>` is the NEW spelling that names the engine
 * directly, so the seam is reachable and testable at all: a seat that cannot be selected is
 * a seat nobody calls.
 *
 * `auto` is the default and derives from the two flags, which is why no gate script changes.
 * An explicit name WINS over them (whatever their order on the command line): naming the
 * engine is naming the configuration, and a machine whose engine and flags disagree is a
 * machine whose stderr lies about what is running.
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * S5a: `--long-mode on|off` (results/x86-64-plan.md ss6 S5's flag)
 *
 * DEFAULT OFF, and that is the shipped machine: with the flag absent nothing about the
 * 32-bit CPU changes -- no backend declares CPU_MODE_LONG, so the S2 refusal still stops a
 * guest at the three writes that would enter IA-32e, by name, exactly as S4's RED census
 * pinned.  `on` declares the capability on the interpreter and selects it (a capability the
 * running engine does not have would be a false declaration -- cpu_backend_enable_long_mode).
 *
 * WHY A GUEST NEEDS TO BE TOLD: the flag is the MACHINE's declaration, not the guest's
 * request.  A real x86-64 kernel decides whether to enter long mode from CPUID (which stays
 * honest: lm/syscall/pdpe1gb are returned only when the backend declares the mode), so from
 * S12 the flag is what makes an amd64 bzImage boot instead of printing its refusal.
 *
 * It is parsed in BOTH modes deliberately: the transcript mode cannot reach long mode (plan
 * ss5.3: the M0 document is a 32-bit document), but it must not silently ACCEPT a flag it
 * does not honour -- so it parses it, and says on stderr that this mode will not use it.
 * ------------------------------------------------------------------ */
static bool long_mode_requested = false;

static int parse_long_mode_flag(int argc, char *argv[], int i)
{
	if (i + 1 < argc && strcmp(argv[i + 1], "on") == 0) {
		long_mode_requested = true;
		return i + 1;
	}
	if (i + 1 < argc && strcmp(argv[i + 1], "off") == 0) {
		long_mode_requested = false;
		return i + 1;
	}
	fprintf(stderr, "v86c: --long-mode needs on|off\n");
	exit(2);
}

static const char *cpu_engine_name = "auto";

static int parse_cpu_engine_flag(int argc, char *argv[], int i)
{
	if (i + 1 >= argc) {
		fprintf(stderr, "v86c: --cpu-engine needs a name (auto|interp|uop|uopjit)\n");
		exit(2);
	}
	cpu_engine_name = argv[i + 1];
	return i + 1;
}

/* Resolve and select, after the whole command line has been parsed (both parsers call this
 * once, at the end, before the machine is built).  An unknown name is refused BY NAME and
 * the process exits: the contract's "refuses an unknown name by name" is the resolver's
 * NULL, and this is the caller's half of it. */
static bool cpu_engine_select_from_args(void)
{
	const cpu_backend_t *engine = cpu_backend_select(cpu_engine_name);

	if (engine == NULL) {
		fprintf(stderr, "v86c: --cpu-engine: unknown engine '%s' (have: auto, interp, uop,"
		                " uopjit)\n", cpu_engine_name);
		return false;
	}
	/* Printed only when the flag was GIVEN: every pre-seam invocation's stderr is unchanged,
	 * and a run that names an engine can see what it got. */
	if (strcmp(cpu_engine_name, "auto") != 0) {
		fprintf(stderr, "v86c: cpu engine: '%s' (--cpu-engine %s)\n", engine->name,
		        cpu_engine_name);
	}
	return true;
}

static int boot_main(int argc, char *argv[])
{
	const char *kernel_path = NULL;
	const char *limit_text = NULL;
	const char *cmdline = "";
	const char *initrd_path = NULL;
	const char *bios_path = NULL;
	const char *serial_path = NULL;
	const char *hda_path = NULL;
	const char *virtio_console_path = NULL;
	const char *virtio_console_in_path = NULL;
	bool uart1 = false;
	bool acpi = false;
	uint32_t memory_size = BOOT_MEMORY_SIZE;

	uop_cache_enabled = true; /* PERF-4/M2: see main()'s note -- the shipped default */

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bzimage") == 0) continue; /* the mode flag */
		else if (strcmp(argv[i], "--cmdline") == 0 && i + 1 < argc) cmdline = argv[++i];
		else if (strcmp(argv[i], "--initrd") == 0 && i + 1 < argc) initrd_path = argv[++i];
		else if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc) bios_path = argv[++i];
		else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc) serial_path = argv[++i];
		else if (strcmp(argv[i], "--hda") == 0 && i + 1 < argc) hda_path = argv[++i];
		else if (strcmp(argv[i], "--virtio-console") == 0 && i + 1 < argc) virtio_console_path = argv[++i];
		else if (strcmp(argv[i], "--virtio-console-in") == 0 && i + 1 < argc) virtio_console_in_path = argv[++i];
		else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
			uint64_t mib;
			parse_positive_uint(argv[++i], "--memory (MiB)", &mib);
			if (mib > 4096) {
				fprintf(stderr, "v86c: --memory: %llu MiB is more than this port's 4 GiB guest\n",
				        (unsigned long long)mib);
				return 2;
			}
			memory_size = (uint32_t)(mib << 20);
		}
		else if (strcmp(argv[i], "--uart1") == 0) uart1 = true;
		else if (strcmp(argv[i], "--acpi") == 0) acpi = true;
		else if (strcmp(argv[i], "--vga-ansi") == 0) machine_config.vga_screen = VGA_SCREEN_ANSI;
		else if (strcmp(argv[i], "--vga-doc") == 0) {
			/* VGA-U0: the document is the M0 TRANSCRIPT's stop (vga.h).  A boot-mode stop is
			 * an instruction limit in a guest that never halts, and a framebuffer document
			 * there is U4's LFB image -- refusing is the honest answer, not writing a
			 * document whose stop point nothing has pinned. */
			fprintf(stderr, "v86c: --vga-doc is the transcript mode's stop (v86c <image>"
			                " <limit> --vga-doc FILE); a boot-mode framebuffer document is"
			                " unit U4's\n");
			return 2;
		}
		else if (strcmp(argv[i], "--uop-cache") == 0 || strcmp(argv[i], "--uop-cache-stats") == 0)
			i = parse_uop_cache_flag(argc, argv, i);
		else if (strcmp(argv[i], "--jit") == 0 || strcmp(argv[i], "--jit-stats") == 0)
			i = parse_jit_flag(argc, argv, i);
		else if (strcmp(argv[i], "--cpu-engine") == 0) i = parse_cpu_engine_flag(argc, argv, i);
		else if (strcmp(argv[i], "--long-mode") == 0) i = parse_long_mode_flag(argc, argv, i);
		else if (kernel_path == NULL) kernel_path = argv[i];
		else if (limit_text == NULL) limit_text = argv[i];
		else {
			fprintf(stderr, "v86c: too many arguments (unexpected: %s)\n", argv[i]);
			return 2;
		}
	}

	if (kernel_path == NULL || limit_text == NULL) {
		fprintf(stderr, "usage: %s --bzimage <kernel> <instruction-limit> [--cmdline TEXT]"
		                " [--initrd FILE] [--bios FILE] [--serial FILE] [--hda FILE]"
		                " [--memory MIB] [--virtio-console FILE] [--virtio-console-in FILE]"
		                " [--uart1] [--acpi] [--vga-ansi] [--uop-cache on|off]"
		                " [--uop-cache-stats] [--jit on|off] [--cpu-engine NAME] [--long-mode on|off]\n",
		                argv[0]);
		return 2;
	}
	/* S0: resolve the ENGINE once, now that the whole command line is known (an explicit
	 * --cpu-engine forces the two flags; `auto` derives from them). */
	if (!cpu_engine_select_from_args()) return 2;
	/* S5a: the capability, AFTER the engine resolution, and the ENGINE only when none was
	 * NAMED: `--long-mode on` alone must run the interpreter (the only backend that can), and
	 * `--long-mode on --cpu-engine uopjit` must be REFUSED BY NAME at the transition instead of
	 * silently moved to another engine -- cpu_backend_enable_long_mode's note has the
	 * measurement. */
	if (long_mode_requested) {
		if (!cpu_backend_enable_long_mode()) return 2;
		if (strcmp(cpu_engine_name, "auto") == 0) cpu_backend_adopt(cpu_backend_by_index(0));
	}
	if (uop_cache_stats || jit_stats_flag) atexit(v86c_stats_print);

	uint64_t limit;
	parse_positive_uint(limit_text, "instruction limit", &limit);

	uint32_t kernel_length = 0;
	uint8_t *kernel = read_file(kernel_path, &kernel_length);
	uint32_t initrd_length = 0;
	uint8_t *initrd = initrd_path != NULL ? read_file(initrd_path, &initrd_length) : NULL;

	/* M2-3: the disk image.  It is the same bytes both legs attach: the reference's own
	 * IDEController takes them as `settings.hda` (cpu.js:1221-1224), and this port hands
	 * them to machine_config before machine_init() builds the machine. */
	uint32_t hda_length = 0;
	uint8_t *hda = hda_path != NULL ? read_file(hda_path, &hda_length) : NULL;
	if (hda != NULL && hda_length % 512 != 0) {
		fprintf(stderr, "v86c: --hda: the image must be a whole number of 512-byte sectors,"
		                " got %u bytes\n", (unsigned)hda_length);
		return 2;
	}

	const uint8_t *firmware = v86c_minibios_image;
	uint32_t firmware_length = v86c_minibios_image_len;
	if (bios_path != NULL) {
		firmware = read_file(bios_path, &firmware_length);
	}
	if (firmware_length != 0x10000) {
		fprintf(stderr, "v86c: the firmware must be exactly 64 KiB (it fills 0xF0000-0xFFFFF),"
		                " got %u bytes\n", (unsigned)firmware_length);
		return 2;
	}

	/* the machine, in the same order the transcript mode builds it (src/main.c above) */
	cpu_state_init();
	if (!allocate_memory(memory_size)) die("cannot allocate guest memory");
	cpu->memory_size = memory_size;
	zero_memory(0, memory_size);

	/* APIC-1: `--acpi` is the machine's one writer for cpu->acpi_enabled, mirroring
	 * upstream's `this.acpi_enabled[0] = +settings.acpi` (cpu.js:1014): it is set once,
	 * BEFORE the machine is assembled, and never reset (reset_cpu does not touch it --
	 * upstream's byte at state offset 552 is not in its reset either).  It is the
	 * reference's own composition: the same flag creates the PIIX4 ACPI device
	 * (machine.c), gates the APIC/IOAPIC mmio windows, the delivery paths, the APIC
	 * timer and the CPUID APIC bit.  DEFAULT OFF: every existing gate's guest runs the
	 * 8259 machine, and that must stay byte-identical. */
	if (acpi) {
		cpu->acpi_enabled = true;
	}

	/* cpu.js:1221-1224: `if(settings.hda) ide_config[0][0] = { buffer: settings.hda }`.
	 * The disk is attached BEFORE the machine is assembled, so the controller is built with
	 * it -- exactly the order the reference's own constructor uses. */
	if (hda != NULL) {
		machine_config.hda.buffer = hda;
		machine_config.hda.length = hda_length;
		machine_config.hda.is_cdrom = false;
	}

	/* M2-4: the virtio console, when the caller asks for it -- the reference's own
	 * `settings.virtio_console` (cpu.js:1253).  The machine is built with the device, and
	 * the host listener is attached to the bus the machine pairs with machine_host_bus. */
	if (virtio_console_path != NULL) {
		machine_config.virtio_console = true;
	}
	if (virtio_console_in_path != NULL) {
		boot_virtio_console_in = read_file(virtio_console_in_path, &boot_virtio_console_in_length);
	}

	machine_init();

	/* JIT-SPIKE: THE DEFERRED INIT, and it is deferred this far deliberately -- jit_mem_init
	 * calls uop_cache_init, which sizes the shared dirty-page bitmap from mem8_alloc_size(),
	 * i.e. it needs the guest's memory to EXIST.  Called at argument-parse time (where this
	 * unit first put it) it always failed with "the uop cache could not be initialized" and
	 * `--jit on` was a no-op in BOTH modes; the flag's smoke test caught it.  Both halves of
	 * the switch are parsed long before this point, so the ordering the deferral was for is
	 * still solved.  S0: the INIT is the selected engine's, through the seam. */
	engine_init_if_needed();

	if (uart1) {
		/* the reference's `settings.uart1` arm (cpu.js:1203-1206).  The default is COM1
		 * alone, which is what settings does by default too. */
		uart_attach(machine_bus, UART_COM2);
	}

	boot_install_fault_handlers();

	/* the HOST seam's two ends (uart.h's precedent for the first; M2-4's console for the
	 * second), attached to the connector the machine pairs with machine_host_bus. */
	boot_serial_sink = serial_path != NULL ? fopen(serial_path, "wb") : stdout;
	if (boot_serial_sink == NULL) die("cannot open the serial output");
	bus_register(machine_host_bus, "serial0-output-byte", boot_serial_output, NULL);

	if (virtio_console_path != NULL) {
		boot_virtio_console_sink = strcmp(virtio_console_path, "-") == 0
		                               ? stdout : fopen(virtio_console_path, "wb");
		if (boot_virtio_console_sink == NULL) die("cannot open the virtio console output");
		bus_register(machine_host_bus, "virtio-console0-output-bytes",
		             boot_virtio_console_output, NULL);
		memset(boot_virtio_console_window, 0, sizeof(boot_virtio_console_window));
	}

	/* The file's digest is printed BEFORE the loader runs: load_kernel writes into the image
	 * (the boot header fields below), and the reference harness hashes the file as read. */
	char kernel_sha256[65];
	{
		sha256_ctx ctx;
		uint8_t digest[32];
		sha256_init(&ctx);
		sha256_update(&ctx, kernel, kernel_length);
		sha256_final(&ctx, digest);
		for (unsigned i = 0; i < 32; i++) sprintf(kernel_sha256 + 2 * i, "%02x", digest[i]);
		kernel_sha256[64] = '\0';
	}

	/* the kernel: the reference's own loader, geometry and all */
	kernel_loaded_t loaded;
	if (!kernel_load(mem8, cpu->memory_size, kernel, kernel_length, initrd, initrd_length,
	                 cmdline, &loaded)) {
		die("not a bootable bzImage (the boot-header checksum does not match)");
	}

	/* what seabios does with the option ROM the loader produced */
	if ((uint64_t)BOOT_OPTION_ROM_ADDRESS + loaded.option_rom_length > cpu->memory_size) {
		die("no room for the option ROM at 0xC0000");
	}
	memcpy(mem8 + BOOT_OPTION_ROM_ADDRESS, loaded.option_rom, loaded.option_rom_length);

	/* the firmware at the top of the first MiB -- v86's load_bios start address
	 * (cpu.js:1706-1709), and the reset vector 0xFFFF0 is inside it */
	memcpy(mem8 + BOOT_FIRMWARE_ADDRESS, firmware, firmware_length);

	/* ACPI-2: the firmware tables, in the free window 0xE0000-0xEFFFF (see
	 * acpi_tables.h), published ONLY when the machine has the APIC: the tables
	 * describe the LAPIC/IOAPIC, so a boot without --acpi must not carry them --
	 * that is the default-off identity property, by construction and not just by
	 * gate.  Same bytes every boot: a pure function of the configuration. */
	if (acpi) {
		acpi_tables_publish(mem8);
	}

	fprintf(stderr, "v86c boot: kernel %u bytes sha256 %s\n", (unsigned)kernel_length,
	        kernel_sha256);
	fprintf(stderr, "v86c boot: firmware %u bytes at 0x%x, option ROM %u bytes at 0x%x,"
	                " memory %u MiB, hda ", (unsigned)firmware_length,
	        (unsigned)BOOT_FIRMWARE_ADDRESS, (unsigned)loaded.option_rom_length,
	        (unsigned)BOOT_OPTION_ROM_ADDRESS, (unsigned)(memory_size >> 20));
	if (hda != NULL) {
		fprintf(stderr, "%u bytes sha256 ", (unsigned)hda_length);
		print_sha256_to(stderr, hda, hda_length);
	} else {
		fputs("none", stderr);
	}
	fprintf(stderr, ", initrd %s, cmdline '%s'\n",
	        initrd != NULL ? "attached" : "none", cmdline);
	fprintf(stderr, "v86c boot: reset cs=0x%x eip=0x%x\n", (unsigned)cpu->sreg[CS],
	        (unsigned)cpu->instruction_pointer);

	/* REBOOT-1: a guest reboot re-enters THIS boot.  The LOADER re-runs --
	 * kernel_load's own regions (the setup block at 0x80000, the protected-mode
	 * kernel at 0x100000, the command line, the initrd, the boot params) plus the
	 * firmware at 0xF0000 and the option ROM at 0xC0000.  This is the one honest
	 * answer for a bzImage guest, and it is a MEASURED one: the Alpine kernel
	 * decompresses ITSELF over 0x100000 during boot 1, so after the guest ran,
	 * the compressed image it entered through NO LONGER EXISTS in guest RAM -- a
	 * re-entry that only re-copied the firmware/option ROM died at #UD inside the
	 * stub's former footprint.  Re-running the loader is also what the reference's
	 * own harness does on a restart (its settings bring-up re-runs load_kernel);
	 * reboot_internal + load_bios instead lands a bzImage guest at the BIOS's
	 * default boot, which this unit's brief forbids.  WHAT IS NOT TOUCHED: the
	 * disk (machine_config.hda's buffer is boot 1's -- a guest write to it
	 * survives, the gate's boot 2 reads its marker back out of it) and every
	 * guest-RAM byte outside the loader's own regions. */
	uint32_t reboots = 0;
	run_result_t result;
	for (;;) {
		result = run(limit);
		if (!cpu->reboot_requested) break;
		if (++reboots > MAX_REBOOTS) die("guest rebooted more than 16 times: a reboot loop, not a boot");
		reboot_prepare_reentry();
		if (!kernel_load(mem8, cpu->memory_size, kernel, kernel_length, initrd,
		                 initrd_length, cmdline, &loaded)) {
			die("the reboot re-entry could not re-run the bzImage loader");
		}
		memcpy(mem8 + BOOT_OPTION_ROM_ADDRESS, loaded.option_rom, loaded.option_rom_length);
		memcpy(mem8 + BOOT_FIRMWARE_ADDRESS, firmware, firmware_length);
		if (acpi) {
			acpi_tables_publish(mem8); /* the firmware tables are firmware: boot 2
			                            * carries them exactly as boot 1 did */
		}
	}

	if (boot_serial_sink != stdout) fclose(boot_serial_sink);
	else fflush(stdout);

	/* A DIAGNOSTIC, not a decision (V86C_BOOT_DUMP, like V86C_LOG_PORTS): when the guest's
	 * serial output and its own log buffer disagree -- the console never attached, say --
	 * the strings the kernel wrote are still in guest memory, and this is how to read them
	 * without a debugger.  Nothing below changes a guest-visible value. */
	if (getenv("V86C_BOOT_DUMP") != NULL) {
		static const char *needles[] = {
			"Linux version", "BIOS-e820", "BIOS-88", "Kernel panic",
			"No filesystem could mount root", "console [ttyS0]", "serial8250",
			"is a 16550A", "ttyS0", "printk: console", "Unable to open console",
		};
		/* the boot params as the GUEST sees them (the setup block is at 0x80000, so its
		 * header fields are at 0x80200+offset -- kernel.h's constants) */
		uint32_t cmd_line_ptr = (uint32_t)mem8[0x80228] | (uint32_t)mem8[0x80229] << 8 |
		                        (uint32_t)mem8[0x8022A] << 16 | (uint32_t)mem8[0x8022B] << 24;
		fprintf(stderr, "v86c dump: loadflags=0x%02x type_of_loader=0x%02x heap_end_ptr=0x%04x"
		                " vid_mode=0x%04x cmd_line_ptr=0x%x\n",
		        mem8[0x80211], mem8[0x80210],
		        (unsigned)mem8[0x80224] | (unsigned)mem8[0x80225] << 8,
		        (unsigned)mem8[0x8021A] | (unsigned)mem8[0x8021B] << 8, (unsigned)cmd_line_ptr);
		if (cmd_line_ptr + 64 <= cpu->memory_size) {
			char text[65];
			memcpy(text, mem8 + cmd_line_ptr, 64);
			text[64] = '\0';
			for (int i = 0; i < 64; i++) if (text[i] && (text[i] < 32 || text[i] > 126)) text[i] = '.';
			fprintf(stderr, "v86c dump: cmdline at 0x%x: '%s'\n", (unsigned)cmd_line_ptr, text);
		}
		for (unsigned n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
			size_t len = strlen(needles[n]);
			for (uint32_t addr = 0; addr + len <= cpu->memory_size; addr++) {
				if (memcmp(mem8 + addr, needles[n], len) == 0) {
					fprintf(stderr, "v86c dump: '%s' at 0x%x\n", needles[n], (unsigned)addr);
					break;
				}
			}
		}
		/* the kernel's RENDERED log, if it wrote one: "buildozer@build" appears only in
		 * the composed "Linux version ... (buildozer@build-3-20-x86) ..." line, never in a
		 * format string, so it locates the ring buffer rather than the image's rodata. */
		{
			/* "Kernel command line: console=ttyS0" can only exist in the RENDERED log:
			 * the format string is "Kernel command line: %s", and "console=ttyS0" as a
			 * literal is in the cmdline area at 0x8E000, not beside "Kernel command line:". */
			static const char *rendered = "Kernel command line: console=ttyS0";
			size_t len = strlen(rendered);
			for (uint32_t addr = 0; addr + len <= cpu->memory_size; addr++) {
				if (memcmp(mem8 + addr, rendered, len) != 0) continue;
				uint32_t start = addr > 0x400 ? addr - 0x400 : 0;
				uint32_t end = start + 30000 < cpu->memory_size ? start + 30000
				                                                : cpu->memory_size;
				fprintf(stderr, "v86c dump: the kernel's log buffer starts near 0x%x\n",
				        (unsigned)start);
				for (uint32_t a = start; a < end; a++) {
					uint8_t c = mem8[a];
					fputc(c >= 32 && c < 127 ? c : (c == '\n' ? '\n' : '.'), stderr);
				}
				fputc('\n', stderr);
				break;
			}
		}
	}



	fprintf(stderr, "v86c boot: %llu instructions, in_hlt=%d, exception=%d, unimplemented=%d,"
	                " eip=0x%x cs=0x%x cr0=0x%x cr3=0x%x\n",
	        (unsigned long long)result.count, result.halted ? 1 : 0, (int)result.exception,
	        result.unimplemented ? 1 : 0, (unsigned)cpu->instruction_pointer,
	        (unsigned)cpu->sreg[CS], (unsigned)cpu->cr[0], (unsigned)cpu->cr[3]);

	if (virtio_console_path != NULL) {
		virtio_console_t *console = machine_virtio_console_device();
		fprintf(stderr, "v86c boot: virtio console: %llu bytes out, %llu bytes in,"
		                " %llu chain bytes read, %llu chain bytes written%s\n",
		        (unsigned long long)boot_virtio_console_out_bytes,
		        (unsigned long long)boot_virtio_console_in_bytes,
		        (unsigned long long)(console != NULL ? virtio_console_chain_bytes_read() : 0),
		        (unsigned long long)(console != NULL ? virtio_console_chain_bytes_written() : 0),
		        boot_virtio_console_in_length > 0
		            ? (boot_virtio_console_in_sent ? " (inbound handshake fired)"
		                                           : " (inbound handshake NOT fired)")
		            : "");
		if (boot_virtio_console_sink != stdout) fclose(boot_virtio_console_sink);
	}

	if (result.exception >= 0) {
		fprintf(stderr, "v86c: guest raised cpu exception %d after %llu instructions\n",
		        result.exception, (unsigned long long)result.count);
		return 1;
	}
	if (result.unimplemented) {
		fprintf(stderr, "v86c: unimplemented: %s\n", unimplemented_instruction_name());
		return 1;
	}
	/* --vga-ansi's reader.  Upstream's browser starter polls the adapter's get_text_screen
	 * from the UI; this port has no UI, so the run's stop prints the text screen to stderr
	 * (never stdout: the serial sink and the transcript own those).  A no-op for every other
	 * adapter. */
	vga_screen_ansi_flush(vga_device()->screen);
	return 0;
}

/* ------------------------------------------------------------------ *
 * S1 (results/x86-64-s1-s2.md): THE POISON PROBE
 *
 * The register file and the lazy-flag operands were WIDENED IN PLACE from 32 to 64 bits
 * (results/x86-64-plan.md ss4).  The slice's whole claim is that nothing 32-bit reads the
 * high half, and the way to test that claim is to FILL the high half with a value that
 * would be visible if anything did: 0xA5A5A5A5 in every widened field's upper 32 bits,
 * seeded AFTER the boot state is established (so the low halves are exactly the ones the
 * run would have had) and BEFORE the first guest instruction.
 *
 * `make poison-probe` runs the M0 image twice -- once plain, once with this seed -- and
 * requires a BYTE-IDENTICAL transcript.  If the transcript moves, a widened field leaked
 * into a 32-bit path: that is the finding, not a re-pin (results/x86-64-s1-s2.md ss3).
 *
 * S3 EXTENDED ITS REACH TO THE ADDRESS FIELDS (results/x86-64-s3.md): instruction_pointer,
 * previous_ip, last_virt_eip, eip_phys, idtr_offset, gdtr_offset and all eight
 * segment_offsets[] slots.  That extension is the DIRECT detector for RISK R2 ("a truncating
 * cast in the widening"): the claim is that every 32-bit path PROJECTS these fields (the
 * batch boundary's invariant, the translate/mem8 casts), so a high half that is present from
 * the boot on must not move a single byte of the transcript.  A move means some consumer read
 * the high half.
 *
 * The seed also has to be compatible with the invariant itself: cpu_state.h's
 * cpu_assert_address_widening would rightly stop the machine on a poisoned high half, so the
 * probe DISABLES it (`cpu_address_widening_check_enabled = false`) -- the two instruments
 * point in opposite directions and each is the other's control.  What the invariant proves is
 * that the SHIPPED paths never produce a high half; what the probe proves is that a high half,
 * once present, changes nothing.
 *
 * WHAT IT STILL CANNOT SHOW, stated so its silence is not read as a proof: it seeds the fields
 * that EXIST -- and S5a is where the SECOND HALF of S1's promise became executable.  S1's
 * claim was "seed every widened field's high half and R8-R15"; until the register file had
 * sixteen slots there was no R8-R15 to seed, so half of that sentence had no arm.  It has one
 * now (below), and what it proves is the strongest form: a poisoned 64-bit value in slots 8-15
 * changes nothing about the 32-bit machine.  For
 * last_op1/last_result the seed is inert BY CONSTRUCTION while flags_changed is clear (every
 * getter recomputes them from the operation that set the bit), so what the run exercises there
 * is only that the seed itself does not move the machine; and the ADDRESS fields are seeded
 * only where the boot state makes the machine consult them -- the M0 image is flat and its
 * GDTR/IDTR bases are read once each, so segment_offsets[FS]/[GS]/[TR]/[LDTR] and
 * idtr_offset/gdtr_offset are seeded but the image barely reads them.  cpl/paging matter: with
 * CR0.PG clear there is no page walk, so the TLB fields are not consulted either.  The flag
 * cores' high-half discipline (the one place the probe is inert by construction) is pinned
 * where it can actually fail: cpu_selftest.c's test_widened_state_poison, and the address
 * invariant's own able-to-fail arm is cpu_selftest.c's test_address_widening_invariant.
 * ------------------------------------------------------------------ */
static void poison_widened_state(void)
{
	const uint64_t poison = 0xA5A5A5A5A5A5A5A5ull;
	const uint64_t high = 0xA5A5A5A5ull << 32;

	for (int i = 0; i < 8; i++) {
		cpu->reg64[i] |= poison; /* the low half keeps the boot's value */
		cpu->dreg[i] |= poison;
		/* S3: the segment bases (u64 slots since ss4).  `| high`, not `| poison`: the
		 * LOW half must keep the boot's value or the very first data access moves. */
		cpu->segment_offsets[i] |= high;
	}
	/* S5a: r8-r15 (the REX-only slots).  The WHOLE value, not `|=`: nothing in a 32-bit mode
	 * may read them, so the probe's claim is that a poisoned 64-bit value changes nothing --
	 * a leaked index would move the transcript on the first instruction that touched it. */
	for (int i = 8; i < 16; i++) cpu->reg64[i] = poison;
	cpu->last_op1 = (int64_t)poison;
	cpu->last_result = (int64_t)poison;
	/* S3: the address fields.  last_virt_eip's boot value is VIRT_EIP_NONE, whose low 12
	 * bits are set; ORing the high half on top keeps it unmatchable as a page. */
	cpu->instruction_pointer |= high;
	cpu->previous_ip |= high;
	cpu->last_virt_eip |= high;
	cpu->eip_phys |= high;
	cpu->idtr_offset |= high;
	cpu->gdtr_offset |= high;

	/* the two instruments point in opposite directions: see the note above */
	cpu_address_widening_check_enabled = false;
	fprintf(stderr, "v86c: --poison: the widened state's high halves are 0xA5A5A5A5"
	                " (registers, lazy operands, and since S3 the address/segment fields)\n");
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
	/* M2-2's boot mode: selected by its OWN flag, so the two-argument oracle contract below
	 * is reached by exactly the invocations it always was. */
	if (argc > 1 && strcmp(argv[1], "--bzimage") == 0) {
		return boot_main(argc, argv);
	}

	bool fixture_mode = false;
	bool fixture64_mode = false;
	bool poison_mode = false;
	const char *image_path = NULL;
	const char *limit_text = NULL;

	/* PERF-4/M2: THE SHIPPED DEFAULT.  The uop cache is ON unless --uop-cache off says
	 * otherwise, and the plan's ss8 step 5 gate is run by giving that flag: cache-off must
	 * reproduce the pre-M2 transcript byte for byte.  (In the library the switch defaults
	 * off -- src/cpu/uop_cache.c explains why the selftests keep the interpreter.) */
	uop_cache_enabled = true;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fixture") == 0) fixture_mode = true;
		/* S4: the x86-64 corpus's document (format v2 + the control tail).  See
		 * print_fixture's header: it is the SAME printer with the wide register block, so
		 * the 32-bit document's bytes are untouched. */
		else if (strcmp(argv[i], "--fixture64") == 0) { fixture_mode = true; fixture64_mode = true; }
		/* S1: the poison probe (see poison_widened_state).  A diagnostic flag, never a
		 * default: nothing in the shipped invocations changes. */
		else if (strcmp(argv[i], "--poison") == 0) poison_mode = true;
		/* VGA-U0: the adapter selection (the device itself is always installed -- vga.h).
		 * --vga-doc FILE selects the recording adapter AND names the document's path, the
		 * shape --serial FILE and --virtio-console FILE already have. */
		else if (strcmp(argv[i], "--vga-ansi") == 0) machine_config.vga_screen = VGA_SCREEN_ANSI;
		else if (strcmp(argv[i], "--vga-doc") == 0 && i + 1 < argc) {
			machine_config.vga_screen = VGA_SCREEN_DOC;
			machine_config.vga_doc_path = argv[++i];
		}
		else if (strcmp(argv[i], "--uop-cache") == 0 || strcmp(argv[i], "--uop-cache-stats") == 0)
			i = parse_uop_cache_flag(argc, argv, i);
		else if (strcmp(argv[i], "--jit") == 0 || strcmp(argv[i], "--jit-stats") == 0)
			i = parse_jit_flag(argc, argv, i);
		else if (strcmp(argv[i], "--cpu-engine") == 0) i = parse_cpu_engine_flag(argc, argv, i);
		else if (strcmp(argv[i], "--long-mode") == 0) i = parse_long_mode_flag(argc, argv, i);
		else if (image_path == NULL) image_path = argv[i];
		else if (limit_text == NULL) limit_text = argv[i];
		else die("too many arguments");
	}

	if (image_path == NULL || limit_text == NULL) {
		fprintf(stderr, "usage: %s <image> <instruction-limit> [--fixture] [--fixture64]"
		                " [--vga-ansi] [--vga-doc FILE] [--uop-cache on|off]"
		                " [--uop-cache-stats] [--jit on|off] [--cpu-engine NAME] [--long-mode on|off]"
		                " [--poison]\n",
		        argv[0]);
		return 2;
	}
	/* S0: resolve the ENGINE once, now that the whole command line is known. */
	if (!cpu_engine_select_from_args()) return 2;
	/* S5a: the capability, in THIS mode too -- it is the one the x86-64 corpus runs in
	 * (`build/v86c-nasm64 {img} {limit} --fixture64 --long-mode on`), because the corpus
	 * images are the only 64-bit guests this slice has.
	 *
	 * PLAN ss5.3's "the transcript stays a 32-bit document" is kept BY CONSTRUCTION rather
	 * than by refusing the flag here: the transcript printer is reached only without
	 * --fixture/--fixture64, its own image is a 32-bit one that never enters IA-32e, and the
	 * flag only grants a CAPABILITY -- no byte of any 32-bit run changes with it absent
	 * (`--long-mode` off is the default and the shipped machine). */
	if (long_mode_requested) {
		if (!cpu_backend_enable_long_mode()) return 2;
		if (strcmp(cpu_engine_name, "auto") == 0) cpu_backend_adopt(cpu_backend_by_index(0));
	}
	if (uop_cache_stats || jit_stats_flag) atexit(v86c_stats_print);

	uint64_t limit;
	parse_positive_uint(limit_text, "instruction limit", &limit);

	/* read the image */
	FILE *file = fopen(image_path, "rb");
	if (file == NULL) die("cannot open the image");
	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (file_size <= 0) die("empty or unreadable image");
	uint8_t *image = malloc((size_t)file_size);
	if (image == NULL) die("out of memory reading the image");
	if (fread(image, 1, (size_t)file_size, file) != (size_t)file_size) die("short read");
	fclose(file);

	/* the machine: cpu.js create_memory (2 MiB, rounded to MMAP_BLOCK_SIZE) then
	 * reboot_internal's reset_cpu + reset_memory (cpu.js:939-967) */
	cpu_state_init();
	if (!allocate_memory(MEMORY_SIZE)) die("cannot allocate guest memory");
	cpu->memory_size = MEMORY_SIZE;
	zero_memory(0, MEMORY_SIZE);

	/* M2-1 (HANDOFF=v86c): THE MACHINE.  io_init + the device set in cpu.js's order --
	 * the same bring-up the reference does when `load_devices` is on (its default), minus
	 * the devices this port has not ported (see src/hw/machine.h).  The M0 contract is
	 * unaffected: the test image touches no device port, and the poll can only deliver an
	 * interrupt the guest itself programmed. */
	machine_init();

	/* JIT-SPIKE: THE DEFERRED INIT, and it is deferred this far deliberately.  jit_mem_init
	 * calls uop_cache_init, which sizes the shared dirty-page bitmap from mem8_alloc_size() --
	 * i.e. it needs the guest's memory to EXIST.  Called at argument-parse time (which is where
	 * this unit first put it) it always failed with "the uop cache could not be initialized" and
	 * `--jit on` was a no-op everywhere; the flag's smoke test caught it.  Both halves of the
	 * switch have been parsed long before this point, so the ordering problem it was deferred
	 * for is still solved.  S0: the INIT is the selected engine's, through the seam. */
	engine_init_if_needed();

	if (!fixture_mode) {
		/* README.md ss1: the transcript's image fields are the FIXED load/entry
		 * contract, so the image must match it -- exactly the check run.mjs makes. */
		if (file_size < 32 || rd32(image) != MULTIBOOT_HEADER_MAGIC) {
			fprintf(stderr, "v86c: image does not start with a multiboot header\n");
			return 2;
		}
		if (rd32(image + 16) != LOAD_ADDRESS || rd32(image + 28) != ENTRY_ADDRESS) {
			fprintf(stderr, "v86c: image multiboot header does not match the fixed load/entry contract\n");
			return 2;
		}
	}

	uint32_t entrypoint = 0;
	if (!load_image(image, (size_t)file_size, &entrypoint)) die("not a bootable multiboot format");
	multiboot_set_state(entrypoint);

	/* REBOOT-1: a guest reboot re-enters THIS boot, not the CLI's default.  The
	 * multiboot entry state is re-established from the entrypoint the loader
	 * found; the loaded segments are NOT re-copied and nothing else in guest RAM
	 * is touched (memory survives -- the property the two-boot gate image proves).
	 * What IS rebuilt is the bootloader's own handoff: EAX/EBX and the info word
	 * at 0x7C00 -- a re-running bootloader re-writes those. */
	/* S1: AFTER multiboot_set_state (so the low halves are the boot's) and BEFORE the first
	 * guest instruction (so the run is the thing under test). */
	if (poison_mode) poison_widened_state();

	uint32_t reboots = 0;
	run_result_t result;
	for (;;) {
		result = run(limit);
		if (!cpu->reboot_requested) break;
		if (++reboots > MAX_REBOOTS) die("guest rebooted more than 16 times: a reboot loop, not a boot");
		reboot_prepare_reentry();
		multiboot_set_state(entrypoint);
	}

	if (fixture_mode) {
		/* VGA-U0: --vga-doc/--vga-ansi are transcript-mode flags, and a silent no-op is the
		 * one thing a flag may not be (the fixture document is the nasm differential's, and
		 * it has no machine geometry to describe). */
		if (machine_config.vga_doc_path != NULL || machine_config.vga_screen != VGA_SCREEN_HEADLESS) {
			fprintf(stderr, "v86c: --vga-ansi/--vga-doc select a VGA adapter for the"
			                " TRANSCRIPT mode's stop (v86c <image> <limit> --vga-doc FILE);"
			                " the --fixture document has no machine geometry\n");
			return 2;
		}
		/* A fault the machine STOPPED ON because it could not be DELIVERED (an empty or
		 * too-small IDT, an invalid gate -- the paths upstream `panic!`s on) is a result
		 * in this mode: the runner compares the host signal and the faulting eip, so the
		 * document is printed and the exit status is 0.  A fault the guest CAN have
		 * delivered is DELIVERED and the machine runs on (M1c's exceptions.c), which is
		 * why this branch is "an undeliverable fault", not "any guest exception" -- see
		 * exceptions.h's note on guest_exception.  The reference leg can produce this
		 * report only in its DEBUG build (P0 ss5.2) and the native harness gets it from
		 * the kernel's signal, so this is the port's version of the same report.  NOTE
		 * the one thing the port cannot report: `no_halt` is set by the JS leg's exit 3
		 * and by nothing else -- the port stops AT the fault instead of patching a hlt
		 * over it and running on, so "did not halt" is not observable here.  Stated
		 * because the runner's `no_halt` path is therefore unusable for the C leg. */
		/* S5a: THE NAMED STOP OUTRANKS THE GUEST EXCEPTION, and the order is the fix, not a
		 * preference.  A long-mode walk that stops BY NAME ("a 1 GiB page is S11's") must also
		 * abort the instruction that asked (cpu->pagefault, the port's abort channel), so both
		 * flags are set and the exception arm -- which came first -- printed a bare
		 * `(signal SIGBUS)` document and HID the reason.  The stop is the CAUSE of the abort, so
		 * it is what a reader needs; a run with no stop is untouched by this order.  MEASURED:
		 * before the flip, gate-long-mode.sh's 1 GiB / 2 MiB / 4 GiB mutation probes each
		 * reported SIGBUS and found no stop text at all. */
		if (result.exception >= 0) {
			print_fixture(fixture64_mode);
			return 0;
		}
		if (result.unimplemented) {
			fprintf(stderr, "v86c: unimplemented: %s\n", unimplemented_instruction_name());
			return 1;
		}
		print_fixture(fixture64_mode);
		if (!result.halted) {
			fprintf(stderr, "v86c: guest did not halt within %llu instructions (reached %llu)\n",
				(unsigned long long)limit, (unsigned long long)result.count);
			return 3;
		}
		return 0;
	}

	if (result.exception >= 0) {
		fprintf(stderr, "v86c: guest raised cpu exception %d after %llu instructions; transcript not produced\n",
			result.exception, (unsigned long long)result.count);
		return 1;
	}
	if (result.unimplemented) {
		fprintf(stderr, "v86c: unimplemented: %s; transcript not produced\n",
			unimplemented_instruction_name());
		return 1;
	}
	if (!result.halted) {
		fprintf(stderr, "v86c: guest did not halt within %llu instructions (reached %llu)\n",
			(unsigned long long)limit, (unsigned long long)result.count);
		return 1;
	}

	/* VGA-U0: the framebuffer document, at THE SAME STOP the transcript is taken at --
	 * the HLT (or the limit, which the M0 contract already makes a nonzero exit) -- so
	 * both legs document the same machine state.  Never on a stop the transcript refuses:
	 * a failed write is a nonzero exit, like a failed stop. */
	if (machine_config.vga_doc_path != NULL) {
		char digest[65];
		sha256_hex(image, (size_t)file_size, digest);
		if (!vga_write_document(machine_config.vga_doc_path, digest, (uint64_t)file_size,
		                        limit, cpu->instruction_counter)) {
			return 1;
		}
	}
	/* --vga-ansi's reader: upstream's browser starter polls the adapter's
	 * get_text_screen; this port has no UI, so the stop prints it to stderr (never
	 * stdout, which the transcript owns).  A no-op for every other adapter. */
	vga_screen_ansi_flush(vga_device()->screen);

	print_transcript(image, (size_t)file_size, limit);
	return 0;
}
