#include <stdint.h>
#include <pico/multicore.h>
#include <stddef.h>
#include <kernel.h>
#include "lib/dhara/nand.h"
#include "picosdk.h"
#include <hardware/flash.h>
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
 * sequence).  Only the lockout START is time-bounded; END does not wait. */
#define FLASH_LOCKOUT_TIMEOUT_US 200000UL   /* 200 ms */

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t b,
                     dhara_error_t *err)
{
    if (!multicore_lockout_start_timeout_us(FLASH_LOCKOUT_TIMEOUT_US)) {
        /* Do NOT report DHARA_E_RECOVER here: dhara's try_recover() swallows
         * that code when the journal is not already in recovery, so a
         * persistent lockout failure became an unbounded 200 ms retry loop
         * in dhara_map_sync/map_write (boot hang at mount).  Return a fatal
         * error that propagates instead of spinning. */
        if (err) *err = DHARA_E_TOO_BAD;
        return -1;
    }
    {
        irqflags_t f = di();
        flash_range_erase(FLASH_OFFSET + ((uint64_t)b * 4096), 4096);
        irqrestore(f);
    }
    multicore_lockout_end_blocking();
    if (err) *err = DHARA_E_NONE;
    return 0;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                    const uint8_t *data,
                    dhara_error_t *err)
{
    if (!multicore_lockout_start_timeout_us(FLASH_LOCKOUT_TIMEOUT_US)) {
        if (err) *err = DHARA_E_TOO_BAD;
        return -1;
    }
    {
        irqflags_t f = di();
        flash_range_program(FLASH_OFFSET + ((uint64_t)p * 512), data, 512);
        irqrestore(f);
    }
    multicore_lockout_end_blocking();
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

