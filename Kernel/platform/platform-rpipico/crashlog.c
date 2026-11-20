/* MASTER_PLAN S5/S8: fatal-exception crash log stored in QSPI flash.
 *
 * The live kprintf() dump is often lost: the watchdog reset tears down USB
 * before core1 drains the TX ring, and the host re-enumerates.  Capture the
 * fault frame into a RAM buffer from the exception handler (safe, no flash
 * work in a fault context), then flush it to a reserved pre-FTL flash sector
 * from plt_monitor() BEFORE watchdog_reboot().  Userspace reads the record
 * via PICOIOC_CRASHLOG and writes it to /var/log/crash.log at boot, so it is
 * an ordinary file readable with cat.
 */
#include <stdint.h>
#include <pico/multicore.h>
#include <hardware/watchdog.h>
#include <stddef.h>
#include <string.h>
#include <kernel.h>
#include "picosdk.h"
#include <hardware/flash.h>
#include "pico_ioctl.h"
#include "globals.h"
#include "vt.h"

typedef char crash_record_size_assert[
	(sizeof(struct pico_crashlog) == CRASH_LOG_REC_SIZE) ? 1 : -1];

/* RAM staging buffer.  Filled by the fault handler, flushed by
 * plt_monitor().  Survives watchdog reset in SRAM but is only trusted when
 * magic matches, which we clear after a successful flush. */
static struct pico_crashlog crash_ram_rec;

static uint32_t crash_crc(const struct pico_crashlog *rec)
{
	const uint8_t *p = (const uint8_t *)rec;
	uint32_t crc = 0;
	size_t i;
	for (i = 0; i < offsetof(struct pico_crashlog, crc); i++)
		crc = crc * 33 + p[i];
	for (i = offsetof(struct pico_crashlog, crc) + 4;
	     i < sizeof(*rec); i++)
		crc = crc * 33 + p[i];
	return crc;
}

/* Called from fatal_exception_handler() -- fault context, no flash I/O. */
void crashlog_capture(struct extended_exception_frame *eh)
{
	struct pico_crashlog *r = &crash_ram_rec;

	memset(r, 0, sizeof(*r));
	r->magic = PICO_CRASHLOG_MAGIC;
	r->len = sizeof(*r);
	r->cause = eh->cause;
	r->r[0] = eh->r0;
	r->r[1] = eh->r1;
	r->r[2] = eh->r2;
	r->r[3] = eh->r3;
	r->r[4] = eh->r4;
	r->r[5] = eh->r5;
	r->r[6] = eh->r6;
	r->r[7] = eh->r7;
	r->r[8] = eh->r8;
	r->r[9] = eh->r9;
	r->r[10] = eh->r10;
	r->r[11] = eh->r11;
	r->r[12] = eh->r12;
	r->r[13] = eh->sp;
	r->r[14] = eh->lr;
	r->r[15] = eh->pc;
	r->r[16] = eh->psr;
	/* Snapshot the tail of the virtual console ring: output that was on
	 * its way to USB/LCD when the fault hit. */
	vt_crash_tail(r->tail, sizeof(r->tail));
	r->crc = crash_crc(r);
}

/* Called from plt_monitor() before the watchdog reset.  Uses the same
 * lockout + di() + flash_range_* pattern as rawflash.c. */
void crashlog_flush(void)
{
	const struct pico_crashlog *r = &crash_ram_rec;
	irqflags_t f;

	if (r->magic != PICO_CRASHLOG_MAGIC)
		return;
	if (r->crc != crash_crc(r))
		return;
	if (!multicore_lockout_start_timeout_us(200000))
		return;

	f = di();
	flash_range_erase(CRASH_LOG_FLASH_OFF, CRASH_LOG_SECTOR);
	flash_range_program(CRASH_LOG_FLASH_OFF, (const uint8_t *)r, CRASH_LOG_REC_SIZE);
	irqrestore(f);
	/* M10 hardening: bounded release, matching rawflash.c:52-60.  An
	 * unbounded lockout-end here can hang with IRQs off during a panic,
	 * turning a deliberate 1.5 s reset into an unrecorded watchdog reset. */
	if (!multicore_lockout_end_timeout_us(200000))
		watchdog_reboot(0, 0, 0);

	/* Don't flush the same record twice if panic() is re-entered. */
	((struct pico_crashlog *)&crash_ram_rec)->magic = 0;
}

/* Copy the persisted record (if any) into a userspace buffer.  Used by the
 * PICOIOC_CRASHLOG ioctl.  Returns 1 if a valid record was copied, 0 if
 * none. */
int crashlog_get(struct pico_crashlog *dst)
{
	const struct pico_crashlog *rec = (const struct pico_crashlog *)
		(XIP_NOCACHE_NOALLOC_BASE + CRASH_LOG_FLASH_OFF);

	if (rec->magic != PICO_CRASHLOG_MAGIC || rec->len != sizeof(*rec) ||
	    rec->crc != crash_crc(rec))
		return 0;
	memcpy(dst, rec, sizeof(*dst));
	return 1;
}
