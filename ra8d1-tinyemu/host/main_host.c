/*
 * main_host.c - run the machine layer on the Mac.
 *
 *   ./tinyemu-host <kernel.bin> [initrd] [-c "cmdline"] [-n insn_limit]
 *
 * `kernel.bin` is whatever would be written to guest RAM at 0x80000000 and
 * entered in S-mode: a Linux flat Image, or one of the small test payloads in
 * host/tests/.
 */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rv_hostsbi.h"
#include "rv_machine.h"
#include "rv_platform.h"
#include "rv_virtio.h"

void host_console_raw(void);

/*
 * The rootfs is mapped, not read. That is not an optimisation here - it is
 * how the board sees it. On the EK-RA8D1 the rootfs is a pointer into the
 * memory-mapped OSPI window and is never copied into RAM, so mapping it on
 * the host exercises the same "read in place from a const pointer" path the
 * block device takes on hardware.
 */
static const uint8_t *map_file(const char *path, size_t *len)
{
	struct stat st;
	void *p;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		fprintf(stderr, "cannot map %s\n", path);
		return NULL;
	}
	*len = (size_t)st.st_size;
	return p;
}

static uint8_t *load_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (f == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "cannot read %s\n", path);
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

int main(int argc, char **argv)
{
	RVBootImage img;
	const char *kernel_path = NULL;
	const char *initrd_path = NULL;
	const char *rootfs_path = NULL;
	const char *cmdline = "console=ttyS0,115200 earlycon=sbi";
	const char *fdt_out = NULL;
	uint64_t insn_limit = 0;
	uint64_t time_limit_us = 0;
	uint64_t t0, t1, n;
	int i, ret;

	memset(&img, 0, sizeof(img));

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			cmdline = argv[++i];
		} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			insn_limit = strtoull(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
			rootfs_path = argv[++i];
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			time_limit_us = strtoull(argv[++i], NULL, 0) * 1000000ull;
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			fdt_out = argv[++i];
		} else if (kernel_path == NULL) {
			kernel_path = argv[i];
		} else {
			initrd_path = argv[i];
		}
	}
	if (kernel_path == NULL) {
		fprintf(stderr,
			"usage: %s <kernel.bin> [initrd] [-r rootfs.img] [-c cmdline] [-n insns] [-t seconds] [-d fdt.dtb]\n",
			argv[0]);
		return 1;
	}

	img.kernel = load_file(kernel_path, &img.kernel_size);
	if (img.kernel == NULL) {
		return 1;
	}
	if (initrd_path != NULL) {
		img.initrd = load_file(initrd_path, &img.initrd_size);
		if (img.initrd == NULL) {
			return 1;
		}
	}
	if (rootfs_path != NULL) {
		img.rootfs = map_file(rootfs_path, &img.rootfs_size);
		if (img.rootfs == NULL) {
			return 1;
		}
	}
	img.cmdline = cmdline;

	ret = rv_machine_init(&img);
	if (ret != 0) {
		fprintf(stderr, "rv_machine_init: %d\n", ret);
		return 1;
	}

	/* Write the generated devicetree out so `dtc -I dtb` can pass judgement
	 * on it. Checking the blob against a real parser is the only way to
	 * know the generator is right rather than merely self-consistent. */
	if (fdt_out != NULL) {
		uint32_t fdt_size = 0;
		uint32_t fdt_addr = rv_machine_fdt(&fdt_size);
		const uint8_t *p = rv_guest_phys_ptr(fdt_addr, fdt_size);
		FILE *f = fopen(fdt_out, "wb");

		if (p == NULL || f == NULL ||
		    fwrite(p, 1, fdt_size, f) != fdt_size) {
			fprintf(stderr, "cannot write %s\n", fdt_out);
			return 1;
		}
		fclose(f);
		fprintf(stderr, "[wrote %u bytes of devicetree from 0x%08x to %s]\n",
			fdt_size, fdt_addr, fdt_out);
		return 0;
	}

	host_console_raw();

	t0 = plat_now_us();
	/* Bounded when -n was given: a payload that misbehaves usually loops
	 * rather than stopping, and a bound turns that from a hang into a
	 * result. */
	ret = rv_machine_run_bounded(insn_limit, time_limit_us);
	t1 = plat_now_us();

	n = rv_machine_insns();
	{
		uint32_t calls, unsupported;

		riscv_host_sbi_stats(&calls, &unsupported);
		if (rv_virtio_blk_sectors() != 0) {
			fprintf(stderr, "[virtio-blk: %llu sectors, %u requests]\n",
				(unsigned long long)rv_virtio_blk_sectors(),
				rv_virtio_blk_requests());
		}
		fprintf(stderr,
			"\n[%llu insns in %llu us = %.1f MIPS; %u sbi calls, %u unsupported; exit %d]\n",
			(unsigned long long)n, (unsigned long long)(t1 - t0),
			(t1 > t0) ? (double)n / (double)(t1 - t0) : 0.0,
			calls, unsupported, ret);
	}
	return ret;
}
