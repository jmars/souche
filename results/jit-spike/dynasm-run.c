/*
 * dynasm-run.c -- JIT-SPIKE Q2(a): the DynASM driver.
 *
 * It is the SAME block as results/jit-spike/x64enc.c's emit_A() (see dynasm-shape.dasc), run
 * through DynASM's runtime encoder: dasm_init / dasm_setup / the block's dasm_put calls /
 * dasm_link / dasm_encode, then the bytes are dumped and executed from a W^X page and checked
 * against the same C invariant the hand-written encoder's block is checked against.
 *
 * The comparison the spike writes down: the two routes' BYTE STREAMS for this block, the lines
 * of source each needed, the build steps each needs, and what each adds to the shipped binary.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "dasm_proto.h"
#include "dasm_x86.h"
#include "dynasm-shape.h"

typedef int64_t (*blk_A_t)(uint32_t n, uint32_t seed);

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	dasm_State *dstate;
	dasm_State **Dst = &dstate;
	static uint8_t code[4096];
	size_t sz = 0;

	/* THE THREE CALLS DynASM REQUIRES, IN ORDER (dasm_proto.h:54-61 and LuaJIT's own
	 * lj_asm.c): init (with the section count), setupglobal (BEFORE setup: without it
	 * lglabels is NULL and dasm_setup memsets a NULL pointer -- MEASURED: it faults under
	 * cosmopolitan, which is how this line was found), then setup with the action list.
	 * `1` is the section count: this block declares no `.section`, so DynASM_MAXSECTION is
	 * not emitted and the default single section is used. */
	void *globals[1] = { NULL };
	dasm_init(Dst, 1);
	dasm_setupglobal(Dst, globals, 0);
	dasm_setup(Dst, spike_actionlist);
	spike_A_emit(Dst);
	if (dasm_link(Dst, &sz) != 0) {
		fprintf(stderr, "dasm_link failed\n");
		return 1;
	}
	if (sz == 0 || sz > sizeof code) {
		fprintf(stderr, "dasm_link produced %zu bytes\n", sz);
		return 1;
	}
	if (dasm_encode(Dst, code) != 0) {
		fprintf(stderr, "dasm_encode failed\n");
		return 1;
	}
	dasm_free(Dst);

	printf("dynasm: block is %zu bytes\n", sz);
	printf("dynasm bytes:");
	for (size_t i = 0; i < sz; i++) printf("%s%02x", (i % 16 == 0) ? "\n  " : " ", code[i]);
	printf("\n");

	uint8_t *page = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) { perror("mmap"); return 1; }
	memcpy(page, code, sz);
	if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) { perror("mprotect"); return 1; }

	blk_A_t fn = (blk_A_t)(void *)page;
	/* the invariant: the guest's EDI after `xor edi, esi` n times, with esi = the seed */
	for (uint32_t n = 1; n <= 6; n++) {
		uint32_t seed = 0x12345u;
		int64_t want = (n & 1u) ? (int64_t)seed : 0;
		int64_t got = fn(n, seed);
		printf("dynasm: n=%u -> 0x%llx (want 0x%llx) %s\n", n,
		       (unsigned long long)got, (unsigned long long)want,
		       got == want ? "MATCH" : "MISMATCH");
		if (got != want) return 1;
	}
	return 0;
}
