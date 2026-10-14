/* calibration: the cost of one lfence+rdtsc bracket pair on THIS host/core */
#include <stdio.h>
#include <stdlib.h>
static inline unsigned long long rdtsc_lf(void)
{
	unsigned int lo, hi;
	__asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}
int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 10000000;
	unsigned long long acc = 0, t0 = rdtsc_lf();
	for (long i = 0; i < n; i++) {
		unsigned long long a = rdtsc_lf();
		unsigned long long b = rdtsc_lf();
		acc += b - a;
	}
	unsigned long long tot = rdtsc_lf() - t0;
	printf("pair_cost %llu cyc (avg over %ld; loop-total %llu, acc %llu)\n",
	       acc / (unsigned long long)n, n, tot, acc);
	return 0;
}
