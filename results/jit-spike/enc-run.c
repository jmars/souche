/*
 * enc-run.c -- JIT-SPIKE Q2(b): the SAME block emitted by our own encoder (x64enc.h).  It exists
 * to be weighed against dynasm-run.c: same block, same runtime behaviour, different feeder.  It
 * is deliberately the smaller file -- the comparison is one of the points of the unit.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "x64enc.h"

typedef int64_t (*blk_A_t)(uint32_t n, uint32_t seed);

int main(void)
{
	static uint8_t code[4096];
	enc_t e;
	e_init(&e, code, sizeof code);
	e_push_r64(&e, RBX);
	e_mov_r32_r32(&e, R8, RDI);
	e_xor_r32_r32(&e, RAX, RAX);
	e_xor_r32_r32(&e, RBX, RBX);
	e_mov_r32_r32(&e, RCX, RSI);
	e_xor_r32_r32(&e, RDI, RDI);
	size_t loop = e.n;
	e_add_r32_r32(&e, RAX, RBX);
	e_sub_r32_r32(&e, RCX, RBX);
	e_xor_r32_r32(&e, RDI, RSI);
	e_dec_r32(&e, R8);
	e_jcc_rel8(&e, CC_NE);
	e_patch_rel8(&e, e.n - 1, loop);
	e_mov_r32_r32(&e, RAX, RDI);
	e_pop_r64(&e, RBX);
	e_ret(&e);
	if (e.err) { fputs("encoder error\n", stderr); return 1; }

	printf("encoder: block is %zu bytes\n", e.n);
	printf("encoder bytes:");
	for (size_t i = 0; i < e.n; i++) printf("%s%02x", (i % 16 == 0) ? "\n  " : " ", code[i]);
	printf("\n");

	uint8_t *page = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) { perror("mmap"); return 1; }
	memcpy(page, code, e.n);
	if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); return 1; }
	blk_A_t fn = (blk_A_t)(void *)page;
	for (uint32_t n = 1; n <= 6; n++) {
		uint32_t seed = 0x12345u;
		int64_t want = (n & 1u) ? (int64_t)seed : 0;
		int64_t got = fn(n, seed);
		printf("encoder: n=%u -> 0x%llx (want 0x%llx) %s\n", n,
		       (unsigned long long)got, (unsigned long long)want,
		       got == want ? "MATCH" : "MISMATCH");
		if (got != want) return 1;
	}
	return 0;
}
