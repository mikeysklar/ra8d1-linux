/*
 * telnet.h - guest console over TCP, and the UART-only console beneath it.
 *
 * The guest's console has both ends inside this application: the emulated
 * 8250 in emu/rv_machine.c calls plat_putc()/plat_getc(), and so does the SBI
 * debug console in emu/rv_hostsbi.c. Re-pointing those two calls at a socket
 * is the whole of the telnet bridge - nothing in the guest changes and nothing
 * in the guest can tell.
 *
 * Two consoles are therefore distinguished:
 *
 *   plat_putc / plat_getc        the guest's console. Goes to the socket
 *                                while a client is attached, to the UART
 *                                otherwise. Declared in emu/rv_platform.h.
 *
 *   plat_uart_putc / _puts       this application's own diagnostics. Always
 *                                the physical UART, never the socket: the
 *                                line that tells you which address to telnet
 *                                to has to reach the wire that already works.
 *
 * Everything here degrades to a no-op inline when networking is off, so
 * src/main.c and src/platform_zephyr.c carry no #ifdefs.
 */
#ifndef RVT_TELNET_H_
#define RVT_TELNET_H_

#include <stdbool.h>

/* Implemented in src/platform_zephyr.c. Always available. */
void plat_uart_putc(char c);
void plat_uart_puts(const char *s);

#ifdef CONFIG_NETWORKING

/*
 * Bring the interface up and get an IPv4 address, blocking the boot for up to
 * CONFIG_RVT_NET_DHCP_WAIT_S purely so the address can be printed before the
 * guest's own output starts scrolling. On timeout the guest boots anyway and
 * DHCP keeps running; the address is announced whenever it turns up.
 */
void rvt_net_bringup(void);

/*
 * Lower the calling thread to CONFIG_RVT_EMU_PRIORITY.
 *
 * The emulator step loop in rv_machine_run_bounded() neither blocks nor
 * yields, and Zephyr schedules preemptible threads strictly by priority, so at
 * main()'s default priority 0 it would starve the Renesas Ethernet RX thread
 * (2), the TCP worker (2) and the telnet thread (5) for the entire life of the
 * guest. Called immediately before the emulator is entered.
 */
void rvt_emu_priority_drop(void);

#else
static inline void rvt_net_bringup(void) { }
static inline void rvt_emu_priority_drop(void) { }
#endif /* CONFIG_NETWORKING */

#ifdef CONFIG_RVT_TELNET

/* Start the listener thread. Returns immediately; the thread waits for an
 * address of its own accord. */
void rvt_telnet_start(void);

/* Guest stdout. Returns true when the byte was taken by the socket path, in
 * which case the caller must not also write the UART. */
bool rvt_console_out(char c);

/* Guest stdin from the socket, or -1 when nothing is waiting or no client is
 * attached. Never blocks. */
int rvt_console_in(void);

#else
static inline void rvt_telnet_start(void) { }
static inline bool rvt_console_out(char c) { (void)c; return false; }
static inline int rvt_console_in(void) { return -1; }
#endif /* CONFIG_RVT_TELNET */

#endif /* RVT_TELNET_H_ */
