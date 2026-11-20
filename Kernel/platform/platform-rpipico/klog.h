#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

/* MASTER_PLAN L1 (option 2): the kernel log ring shares its 4 KB with the
 * mkfs scratch buffer in devfat.c.  Only one arm is live at a time: mkfs
 * suspends logging for the duration of f_mkfs(), and logging resumes with
 * a cleared ring afterwards.  This gives a 4 KB log ring with ZERO new
 * RAM: the map is unchanged and USERMEM stays at 202 KB.
 */
#define KLOG_RING_SIZE 4096

union klog_mkfs_buf {
	uint8_t mkfs_work[KLOG_RING_SIZE];	/* devfat.c arm */
	uint8_t klog_ring[KLOG_RING_SIZE];	/* klog.c arm   */
};

extern union klog_mkfs_buf klog_mkfs;

/* Called from kputchar() (IRQ/boot/panic context): append one byte. */
void klog_putc(uint_fast8_t c);

/* Drain up to count bytes into a userspace buffer; returns bytes moved
 * (or -1 on a bad user pointer). */
int klog_read(uarg_t dest, unsigned int count);

/* Ioctl handlers for /dev/sys minor 128. */
int klog_ioctl(unsigned int request, char *data);

/* Manage the shared buffer around mkfs. */
void klog_suspend(void);
void klog_resume(void);

/* Durability: flush the ring to the pre-FTL flash gap and replay at boot. */
void klog_flush(void);
void klog_replay(void);

/* Number of bytes currently pending (for LEVEL). */
unsigned int klog_level(void);

/* ioctl numbers (KLOGIOC_*), also used by userspace klogctl. */
#define KLOGIOC_DRAIN  0x4B01
#define KLOGIOC_LEVEL  0x4B02
#define KLOGIOC_CLEAR  0x4B03
#define KLOGIOC_FLUSH  0x4B04
#define KLOGIOC_REPLAY 0x4B05
#define KLOGIOC_PEEK   0x4B06

#endif
