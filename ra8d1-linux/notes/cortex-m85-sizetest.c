// Measure the code size of the mini-rv32ima core alone, compiled for Cortex-M85.
#include <stdint.h>

extern uint32_t ram_amt;
extern void putc_uart(int c);
extern int getc_uart(void);
extern uint8_t *ram_image;

#define MINIRV32WARN(x...)
#define MINIRV32_DECORATE
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, retval)
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) if (HandleControlStore(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value)
#define MINIRV32_OTHERCSR_READ(csrno, value) value = 0;

static uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy);

#include "mini-rv32ima.h"

struct MiniRV32IMAState *core;

static uint32_t HandleControlStore(uint32_t addy, uint32_t val)
{
	if (addy == 0x10000000) { putc_uart(val); }
	else if (addy == 0x11004004) core->timermatchh = val;
	else if (addy == 0x11004000) core->timermatchl = val;
	else if (addy == 0x11100000) { core->pc += 4; return val; }
	return 0;
}

static uint32_t HandleControlLoad(uint32_t addy)
{
	if (addy == 0x10000005) return 0x60 | (getc_uart() >= 0);
	else if (addy == 0x10000000) return getc_uart();
	else if (addy == 0x1100bffc) return core->timerh;
	else if (addy == 0x1100bff8) return core->timerl;
	return 0;
}
