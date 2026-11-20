#include <stdint.h>
#include <pico/multicore.h>
#include <stddef.h>
#include <kernel.h>
#include "lib/dhara/nand.h"
#include "picosdk.h"
#include <hardware/flash.h>
#include <hardware/watchdog.h>
#include "globals.h"

/* Bounded wait for the other core.  The old *_blocking() call spins forever
 * if core1 is stuck in a critical_section (e.g. a console/USB ring access),
 * because a parked core cannot service the lockout IRQ -> core0 lockup with
 * interrupts disabled -> hard hang that never reaches the panic handler.
 * Fail cleanly instead: never program flash while core1 may still be in XIP.
 *
 * The SDK flash APIs are RAM-resident but DO NOT mask interrupts, and this is
 * an XIP build with a flash-resident vector table, so we must still disable
 * IRQs around the actual erase/program (the canonical SDK flash_safe_execute
 * sequence).  BOTH the lockout START and END are time-bounded (MASTER_PLAN
 * S6): END used to be unbounded, and a stuck lockout mutex would park core1
 * forever while flash_lockout_active suppressed the tick's wedge detector. */
#define FLASH_LOCKOUT_TIMEOUT_US 200000UL   /* 200 ms per attempt */
#define FLASH_LOCKOUT_ATTEMPTS 3
#define FLASH_BACKOFF_US 200UL

/* Set while core0 holds core1 in the multicore lockout for a flash
 * erase/program.  Core1's heartbeat stops during lockout, so the tick's
 * core1-wedge detector (devices.c) must not count that gap as a wedge —
 * otherwise legitimate flash/GC/TRIM work triggers a false watchdog reboot
 * under compiler load (MASTER_PLAN S1).  Volatile: written by core0, read
 * by core0's tick, no cross-core ordering needed. */
volatile uint8_t flash_lockout_active;

/* A start can fail when core1 is momentarily inside a TinyUSB critical
 * section and cannot service the lockout FIFO IRQ.  The SDK cancels a failed
 * attempt (and releases its mutex) before returning, so retrying is safe. */
static bool flash_lockout_start(void)
{
	for (unsigned i = 0; i < FLASH_LOCKOUT_ATTEMPTS; i++) {
		if (multicore_lockout_start_timeout_us(FLASH_LOCKOUT_TIMEOUT_US))
			return true;
		if (i + 1 < FLASH_LOCKOUT_ATTEMPTS)
			busy_wait_us(FLASH_BACKOFF_US);
	}
	return false;
}

/* multicore_lockout_end_blocking() has no timeout.  Bound the release and,
 * if it still cannot be acquired, reboot: leaving core1 parked would kill
 * USB, and flash_lockout_active would stop the tick from detecting the wedge. */
static void flash_lockout_end(void)
{
	for (unsigned i = 0; i < FLASH_LOCKOUT_ATTEMPTS; i++)
		if (multicore_lockout_end_timeout_us(FLASH_LOCKOUT_TIMEOUT_US))
			return;
	watchdog_reboot(0, 0, 0);
	for (;;)
		tight_loop_contents();
}

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t b,
                     dhara_error_t *err)
{
    if (!flash_lockout_start()) {
        /* Do NOT report DHARA_E_RECOVER here: dhara's try_recover() swallows
         * that code when the journal is not already in recovery, so a
         * persistent lockout failure became an unbounded 200 ms retry loop
         * in dhara_map_sync/map_write (boot hang at mount).  Return a fatal
         * error that propagates instead of spinning. */
        if (err) *err = DHARA_E_TOO_BAD;
        return -1;
    }
    flash_lockout_active = 1;
    {
        irqflags_t f = di();
        flash_range_erase(FLASH_OFFSET + ((uint64_t)b * 4096), 4096);
        irqrestore(f);
    }
    flash_lockout_end();
    flash_lockout_active = 0;
    if (err) *err = DHARA_E_NONE;
    return 0;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                    const uint8_t *data,
                    dhara_error_t *err)
{
    if (!flash_lockout_start()) {
        if (err) *err = DHARA_E_TOO_BAD;
        return -1;
    }
    flash_lockout_active = 1;
    {
        irqflags_t f = di();
        flash_range_program(FLASH_OFFSET + ((uint64_t)p * 512), data, 512);
        irqrestore(f);
    }
    flash_lockout_end();
    flash_lockout_active = 0;
    if (err) *err = DHARA_E_NONE;
    return 0;
}

int dhara_nand_read(const struct dhara_nand *n, dhara_page_t p,
					size_t offset, size_t length,
                    uint8_t *data,
                    dhara_error_t *err)
{
    memcpy(data,
        (uint8_t*)XIP_NOCACHE_NOALLOC_BASE + FLASH_OFFSET + (p*512) + offset,
        length);
	if (err)
		*err = DHARA_E_NONE;
	return 0;
}

/* vim: sw=4 ts=4 et: */

