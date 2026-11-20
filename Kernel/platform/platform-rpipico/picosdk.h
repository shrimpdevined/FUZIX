#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
#endif
#ifndef __off_t_defined
#define __off_t_defined
#endif

#define MANGLED 0
#include "mangle.h"

#include <hardware/gpio.h>
#include <hardware/regs/addressmap.h>
#include "pico/stdlib.h"

#define MANGLED 1
#include "mangle.h"

/* Deferred BOOTSEL entry.  The RP2040 bootrom's reset_usb_boot() schedules a
 * ~10 ms reset through the single hardware watchdog and then waits in wfi.
 * FUZIX feeds that same watchdog from core0's timer tick, so a bootrom call
 * made while the tick keeps feeding it never fires: the caller parks and a
 * later full watchdog reset boots the application instead (pico-sdk #2689).
 * Any path that wants BOOTSEL therefore sets plt_bootsel_pending and lets
 * core0's tick run plt_bootsel_enter() with IRQs off and the watchdog
 * disabled, so the bootrom's short timeout is the only one armed. */
extern volatile uint8_t plt_bootsel_pending;
void plt_bootsel_enter(void);
