/*
 * Throughput benchmark for the Pac-Man machine layer.
 *
 * Reports the three numbers that decide whether this runs full speed:
 * Z80 instructions per second, the cost of one video frame's rendering, and
 * the resulting real-time multiple against the board's 3.072 MHz Z80.
 *
 * The same measurement runs on target from src/main.c, so host and board
 * figures are directly comparable.
 *
 *   ./bench [image.bin] [frames]
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "pacman.h"
#include "romimage.h"

static pacman_t machine;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);

	uint8_t *buf = malloc((size_t) n);

	if (!buf || fread(buf, 1, (size_t) n, f) != (size_t) n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t) n;
	return buf;
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "testrom.bin";
	int frames = (argc > 2) ? atoi(argv[2]) : 3000;

	size_t len;
	uint8_t *img = slurp(path, &len);

	if (!img) {
		return 1;
	}

	romimg_t r;

	if (romimg_parse(img, len, true, &r) != ROMIMG_OK ||
	    romimg_check_pacman(&r) != ROMIMG_OK) {
		fprintf(stderr, "bad rom image\n");
		return 1;
	}
	if (!pacman_load(&machine, r.cpu, r.gfx, r.prom)) {
		return 1;
	}

	/* The watchdog would reset the machine mid-run and skew the counts. */
	machine.watchdog_enable = false;

	/* Warm up so the first-frame costs do not land in the sample. */
	for (int i = 0; i < 60; i++) {
		pacman_run_frame(&machine);
	}

	/* --- full frames: CPU plus render, i.e. what the board must sustain */
	uint64_t i0 = machine.instrs;
	unsigned long c0 = machine.cpu.cyc;
	double t0 = now();

	for (int i = 0; i < frames; i++) {
		pacman_run_frame(&machine);
	}
	double t_full = now() - t0;
	uint64_t instrs = machine.instrs - i0;
	unsigned long tstates = machine.cpu.cyc - c0;

	/* --- render alone, same number of calls */
	t0 = now();
	for (int i = 0; i < frames; i++) {
		pacman_render(&machine);
	}
	double t_render = now() - t0;

	double t_cpu = t_full - t_render;

	printf("frames            %d\n", frames);
	printf("z80 instructions  %llu  (%.0f per frame)\n",
	       (unsigned long long) instrs, (double) instrs / frames);
	printf("z80 t-states      %lu  (%.0f per frame, nominal %u)\n",
	       tstates, (double) tstates / frames, PACMAN_CYC_PER_FRAME);
	printf("t-states / instr  %.2f\n", (double) tstates / (double) instrs);
	printf("\n");
	printf("full frame        %.3f s  -> %.1f fps, %.2fx real time\n",
	       t_full, frames / t_full, frames / t_full / 60.606);
	printf("  cpu only        %.3f s  (%.0f%%)  -> %.2f M z80-instr/s\n",
	       t_cpu, 100.0 * t_cpu / t_full, instrs / t_cpu / 1e6);
	printf("  render only     %.3f s  (%.0f%%)  -> %.1f us/frame\n",
	       t_render, 100.0 * t_render / t_full, t_render / frames * 1e6);
	printf("\n");
	printf("pac-man needs     %u t-states/s (%.0f z80-instr/s)\n",
	       PACMAN_CLOCK_HZ,
	       PACMAN_CLOCK_HZ / ((double) tstates / (double) instrs));
	printf("cpu headroom      %.1fx\n",
	       (instrs / t_cpu) / (PACMAN_CLOCK_HZ /
	                           ((double) tstates / (double) instrs)));

	free(img);
	return 0;
}
