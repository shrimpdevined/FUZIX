/* MASTER_PLAN L1 (option 2): kernel log ring sharing 4 KB with mkfs_work.
 *
 * The 4 KB buffer is declared in klog.h as a union: devfat.c uses the
 * mkfs arm only during f_mkfs(), klog uses the ring arm the rest of the
 * time.  That gives a 4096-byte log ring with ZERO new RAM -- the map
 * is unchanged and USERMEM stays 202 KB.
 *
 * klog_putc() runs from kputchar() in IRQ/boot/panic context, so it may
 * only use di() + a fixed array + irqrestore(): no printf, no file, no
 * flash.  Userspace drains via /dev/sys minor 128.
 */
#include <stdint.h>
#include <pico/multicore.h>
#include <stddef.h>
#include <string.h>
#include <kernel.h>
#include "picosdk.h"
#include <hardware/flash.h>
#include "klog.h"

/* The shared 4 KB.  devfat.c refers to klog_mkfs.mkfs_work, we use
 * klog_mkfs.klog_ring.  Placed in .bss once. */
union klog_mkfs_buf klog_mkfs;

/* Ring indices are monotonic 32-bit counters; available = head - tail. */
static volatile uint32_t klog_head;
static volatile uint32_t klog_tail;
static volatile uint32_t klog_dropped;
static volatile uint8_t klog_suspended;

#define KLOG_MASK (KLOG_RING_SIZE - 1)

void klog_putc(uint_fast8_t c)
{
	irqflags_t irq = di();

	if (klog_suspended)
		return;                 /* mkfs owns the buffer right now */
	if ((uint32_t)(klog_head - klog_tail) >= KLOG_RING_SIZE) {
		/* Keep the newest bytes (crash context) by dropping the oldest. */
		klog_tail++;
		klog_dropped++;
	}
	klog_mkfs.klog_ring[klog_head++ & KLOG_MASK] = c;
	irqrestore(irq);
}

unsigned int klog_level(void)
{
	irqflags_t irq = di();
	uint32_t n = klog_head - klog_tail;
	irqrestore(irq);
	return n > KLOG_RING_SIZE ? KLOG_RING_SIZE : n;
}

/* Copy the next `count` bytes to a userspace pointer.  Returns bytes
 * actually written; the ring tail advances only by what was copied. */
int klog_read(uarg_t dest, unsigned int count)
{
	uint32_t head, tail, n;
	irqflags_t irq = di();

	head = klog_head;
	tail = klog_tail;
	n = head - tail;
	if (n > count)
		n = count;
	if (n > KLOG_RING_SIZE)
		n = KLOG_RING_SIZE;
	irqrestore(irq);

	if (n == 0)
		return 0;

	/* Two contiguous slices at most (the ring wraps once). */
	{
		uint32_t off = tail & KLOG_MASK;
		uint32_t first = KLOG_RING_SIZE - off;
		if (first > n)
			first = n;
		if (uput(klog_mkfs.klog_ring + off, (void *)dest, first))
			return -1;
		if (first < n)
			if (uput(klog_mkfs.klog_ring, (void *)(dest + first), n - first))
				return -1;
	}

	irq = di();
	klog_tail = tail + n;
	irqrestore(irq);
	return (int)n;
}

void klog_suspend(void)
{
	irqflags_t irq = di();
	klog_suspended = 1;
	irqrestore(irq);
}

void klog_resume(void)
{
	irqflags_t irq = di();
	klog_suspended = 0;
	klog_head = klog_tail = 0;      /* mkfs overwrote the buffer */
	irqrestore(irq);
}

/* ---- Durability: flush the ring to the pre-FTL QSPI gap ----------------
 * 32 KB region 0x2C000..0x33FFF; crashlog owns 0x30000, so klog uses
 * 0x2C000..0x2FFFF (4 sectors, simple rotation by seq).  Each sector:
 *   magic "KLOG" @0, seq @4, 4088 bytes of ring data @8.
 * Flash writes happen only from plt_monitor (panic) or the FLUSH ioctl,
 * never from the putc hook.  Best-effort: failures increment klog_dropped.
 */
#define KLOG_FLASH_BASE 0x2C000UL
#define KLOG_FLASH_SECTORS 4
#define KLOG_MAGIC 0x4B4C4F47UL    /* "KLOG" */

static uint8_t klog_stage[256];

static void klog_program_sector(uint32_t seq)
{
	uint32_t off = KLOG_FLASH_BASE + ((seq % KLOG_FLASH_SECTORS) << 12);
	uint32_t tail, i, chunk, base;
	irqflags_t irq;

	/* Snapshot the newest 4088 bytes. */
	irq = di();
	tail = klog_tail;
	irqrestore(irq);
	base = (tail + KLOG_RING_SIZE - 4088) & KLOG_MASK;

	if (!multicore_lockout_start_timeout_us(200000))
		return;
	irq = di();
	flash_range_erase(off, 4096);

	/* First 256-byte page: header + 248 bytes of log. */
	memset(klog_stage, 0, sizeof(klog_stage));
	klog_stage[0] = (uint8_t)KLOG_MAGIC;
	klog_stage[1] = (uint8_t)(KLOG_MAGIC >> 8);
	klog_stage[2] = (uint8_t)(KLOG_MAGIC >> 16);
	klog_stage[3] = (uint8_t)(KLOG_MAGIC >> 24);
	klog_stage[4] = (uint8_t)seq;
	klog_stage[5] = (uint8_t)(seq >> 8);
	klog_stage[6] = (uint8_t)(seq >> 16);
	klog_stage[7] = (uint8_t)(seq >> 24);
	for (i = 0; i < 248; i++)
		klog_stage[8 + i] = klog_mkfs.klog_ring[(base + i) & KLOG_MASK];
	flash_range_program(off, klog_stage, 256);

	/* Remaining 15 pages straight from the ring (may wrap). */
	for (chunk = 1; chunk < 16; chunk++) {
		uint32_t p = (base + 248 + (chunk - 1) * 256) & KLOG_MASK;
		uint32_t first = KLOG_RING_SIZE - p;
		if (first > 256)
			first = 256;
		flash_range_program(off + chunk * 256,
		                    klog_mkfs.klog_ring + p, first);
		if (first < 256)
			flash_range_program(off + chunk * 256 + first,
			                    klog_mkfs.klog_ring, 256 - first);
	}
	irqrestore(irq);
	multicore_lockout_end_blocking();
}

void klog_flush(void)
{
	static uint32_t seq;
	uint32_t n = klog_level();

	if (klog_suspended || n == 0)
		return;
	klog_program_sector(++seq);
}

void klog_replay(void)
{
	uint32_t best_seq = 0, best_sector = 0, i, found = 0;

	for (i = 0; i < KLOG_FLASH_SECTORS; i++) {
		uint32_t off = KLOG_FLASH_BASE + (i << 12);
		const uint8_t *p = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + off);
		uint32_t magic = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		uint32_t seq = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
		               ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
		if (magic == KLOG_MAGIC && (!found || seq > best_seq)) {
			best_seq = seq; best_sector = i; found = 1;
		}
	}
	if (!found)
		return;

	{
		uint32_t off = KLOG_FLASH_BASE + (best_sector << 12);
		const uint8_t *p = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + off);
		for (i = 0; i < 4088; i++)
			klog_mkfs.klog_ring[i] = p[8 + i];
	}
	klog_head = 4088;
	klog_tail = 0;
}

/* ioctl dispatcher (called from sys_ioctl for /dev/sys minor 128). */
int klog_ioctl(unsigned int request, char *data)
{
	switch (request) {
	case KLOGIOC_LEVEL:
		return uputw((uint16_t)klog_level(), data);
	case KLOGIOC_CLEAR:
		klog_head = klog_tail = 0;
		return 0;
	case KLOGIOC_FLUSH:
		klog_flush();
		return 0;
	case KLOGIOC_REPLAY:
		klog_replay();
		return 0;
	case KLOGIOC_DRAIN:
		return klog_read((uarg_t)data, (unsigned int)udata.u_count);
	case KLOGIOC_PEEK:
		/* Not implemented: klogd uses DRAIN. */
		return -1;
	default:
		return -1;
	}
}
