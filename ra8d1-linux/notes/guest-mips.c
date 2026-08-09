/* Counted loop with an exactly known dynamic instruction count, so the guest's
 * effective MIPS under an emulator can be measured instead of guessed.
 * Body is 2 instructions (addi + bnez), so total = 2 * ITERS. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
	unsigned long iters = (argc > 1) ? strtoul(argv[1], NULL, 0) : 100000000UL;
	unsigned long n = iters;
	struct timespec a, b;
	double secs, insns;

	clock_gettime(CLOCK_MONOTONIC, &a);
	__asm__ volatile("1:\n\t"
			 "addi %0, %0, -1\n\t"
			 "bnez %0, 1b\n\t"
			 : "+r"(n)::);
	clock_gettime(CLOCK_MONOTONIC, &b);

	secs = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	insns = 2.0 * (double)iters;
	printf("%lu iters, %.0f insns, %.3f s, %.1f MIPS\n",
	       iters, insns, secs, insns / secs / 1e6);
	return 0;
}
