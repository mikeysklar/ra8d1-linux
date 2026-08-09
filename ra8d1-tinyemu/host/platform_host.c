/*
 * platform_host.c - rv_platform.h on a POSIX host.
 *
 * The point of this file is that emu/ can be compiled and run on the Mac
 * exactly as it is on the board. Every bug that is not about Renesas
 * peripherals - the SBI implementation, the devicetree, the page-table walk,
 * the 8250 - is cheaper to find here, and a build here is seconds rather than
 * a flash cycle on hardware someone else is using.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "rv_platform.h"

#ifndef HOST_RAM_SIZE
#define HOST_RAM_SIZE (64u << 20)
#endif

static uint8_t *host_ram;
static uint64_t t0_us;
static struct termios saved_termios;
static int termios_saved;

/* ------------------------------------------------------------- guest RAM */

uint8_t *plat_guest_ram(size_t *size)
{
	if (host_ram == NULL) {
		host_ram = calloc(1, HOST_RAM_SIZE);
		if (host_ram == NULL) {
			return NULL;
		}
	}
	*size = HOST_RAM_SIZE;
	return host_ram;
}

/* --------------------------------------------------------- image storage */

/*
 * No flash slots on the host. main_host.c takes its kernel and rootfs as
 * command-line paths instead, which is the same thing one layer up.
 */
const uint8_t *plat_slot_base(int slot, size_t *size)
{
	(void)slot;
	*size = 0;
	return NULL;
}

/* ------------------------------------------------------------------ time */

uint64_t plat_now_us(void)
{
	struct timespec ts;
	uint64_t now;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
	if (t0_us == 0) {
		t0_us = now;
	}
	return now - t0_us;
}

/* --------------------------------------------------------------- console */

void plat_putc(char c)
{
	fputc(c, stdout);
	if (c == '\n') {
		fflush(stdout);
	}
}

void plat_puts(const char *s)
{
	fputs(s, stdout);
	fflush(stdout);
}

int plat_getc(void)
{
	unsigned char c;
	ssize_t n = read(STDIN_FILENO, &c, 1);

	return n == 1 ? c : -1;
}

static void host_console_restore(void)
{
	if (termios_saved) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
		termios_saved = 0;
	}
}

/* Raw, non-blocking stdin, so keystrokes reach the guest one at a time and
 * plat_getc() never stalls the run loop. */
void host_console_raw(void)
{
	struct termios t;
	int flags;

	if (!isatty(STDIN_FILENO)) {
		flags = fcntl(STDIN_FILENO, F_GETFL, 0);
		fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
		return;
	}
	if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
		termios_saved = 1;
		atexit(host_console_restore);
		t = saved_termios;
		t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
		t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &t);
	}
	flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

/* ------------------------------------------------------------ paravirt IO */

void plat_pv_caps(uint8_t *n_i2c, uint8_t *n_gpio)
{
	if (n_i2c) {
		*n_i2c = 0;
	}
	if (n_gpio) {
		*n_gpio = 0;
	}
}

int plat_pv_i2c_write(uint8_t bus, uint8_t addr, const uint8_t *buf,
		      size_t len)
{
	(void)bus; (void)addr; (void)buf; (void)len;
	return -ENOTSUP;
}

int plat_pv_i2c_read(uint8_t bus, uint8_t addr, uint8_t *buf, size_t len)
{
	(void)bus; (void)addr; (void)buf; (void)len;
	return -ENOTSUP;
}

int plat_pv_i2c_write_read(uint8_t bus, uint8_t addr,
			   const uint8_t *wbuf, size_t wlen,
			   uint8_t *rbuf, size_t rlen)
{
	(void)bus; (void)addr; (void)wbuf; (void)wlen; (void)rbuf; (void)rlen;
	return -ENOTSUP;
}

int plat_pv_gpio_config(uint8_t pin, uint32_t flags)
{
	(void)pin; (void)flags;
	return -ENODEV;
}

int plat_pv_gpio_set(uint8_t pin, uint32_t val)
{
	(void)pin; (void)val;
	return -ENODEV;
}

int plat_pv_gpio_get(uint8_t pin, uint32_t *val)
{
	(void)pin; (void)val;
	return -ENODEV;
}

int plat_pv_gpio_toggle(uint8_t pin)
{
	(void)pin;
	return -ENODEV;
}

/* ---------------------------------------------------------------- system */

void plat_poweroff(void)
{
	host_console_restore();
	printf("\n[guest powered off]\n");
}

void plat_reboot(void)
{
	printf("\n[guest requested reboot]\n");
}
