/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <kernel.h>
#include <printf.h>
#include "picosdk.h"
#include "config.h"
#include "core1.h"

#define ssize_t __ssize_t
#define time_t __time_t
#include <tusb.h>
#undef ssize_t
#undef time_t

#include <pico/multicore.h>
#include <hardware/uart.h>
#include <hardware/irq.h>

/* The console ring buffers hold bytes between core0's kprintf and core1's
 * TinyUSB drain.  Single-producer/single-consumer: core0 only writes wbuf /
 * reads rbuf, core1 only reads wbuf / writes rbuf.  With one producer and one
 * consumer, separate head/tail indices plus memory barriers are sufficient —
 * no critical_section spinlock is needed.  This matters because core1 is the
 * multicore-lockout victim: if it is parked while holding a spinlock, core0
 * deadlocks.  A lock-free queue removes that failure mode entirely.
 * Buffer size is a power of two so head/tail wrap with a mask.
 *
 * The TX ring is deliberately larger than the RX ring: output has no USB
 * flow control of its own, so it needs enough slack to ride out a host that
 * briefly stops reading.  A full TX ring makes writers sleep (see
 * tty_writeready()); it never silently loses data on the userspace path.
 * A full RX ring instead stops core1 from reading the CDC endpoint, which
 * lets USB NAK the host — again no data loss. */
#define CONFIG_CONSOLE_BUF_SHIFT 9
#define CONFIG_CONSOLE_BUF_LEN (1u << CONFIG_CONSOLE_BUF_SHIFT)
#define CONFIG_CONSOLE_BUF_MASK (CONFIG_CONSOLE_BUF_LEN - 1)

struct console_buf_t
{
	volatile uint8_t buffer[CONFIG_CONSOLE_BUF_LEN];
	volatile uint32_t head;   /* written by producer only */
	volatile uint32_t tail;   /* written by consumer only */
};

struct console_t
{
	struct console_buf_t rbuf;
	struct console_buf_t wbuf;
	volatile uint8_t flush_req;   /* core0 -> core1: drop the CDC FIFO */
	volatile uint8_t flush_ack;   /* core1 -> core0: FIFO dropped */
	volatile uint8_t sleeping;
};

static struct console_t usb_console_table[NUM_DEV_TTY_USB];
/* Set by core1 once its drain loop is running. */
static volatile uint8_t core1_ready;
volatile int usb_host_connected;

/* Diagnostics.  A non-zero TX drop count means a caller ignored
 * tty_writeready() (the kernel's own best-effort console path does this by
 * design, because it may run in an interrupt or panic context). */
volatile uint32_t usbconsole_tx_dropped;
volatile uint32_t usbconsole_rx_dropped;

/* core1 liveness telemetry: bumped once per core1_main loop iteration.  If it
 * stops advancing while core0 stays alive (watchdog fed), core1 is wedged —
 * the "enumerated but mute USB" failure.  Kept in .bss (RAM) so picotool can
 * read it even while XIP is disabled during flash programming. */
volatile uint32_t core1_heartbeat;
volatile uint32_t core1_state;   /* 0=init 1=pre-tusb 2=post-tusb 3=running */

/* Lock-free SPSC queue helpers.  `count` is derived from head-tail; a slot is
 * free when head-tail < LEN.  Barriers ensure the data write is visible before
 * the index increment (producer) and the index is visible before data read
 * (consumer).  No shared spinlock -> no deadlock when core1 is parked. */
static inline uint32_t console_buf_count(struct console_buf_t *buf)
{
	return buf->head - buf->tail;
}

static bool console_buf_write(struct console_buf_t *buf, uint8_t b)
{
	uint32_t head = buf->head;
	if ((head - buf->tail) >= CONFIG_CONSOLE_BUF_LEN)
		return false;                 /* full: caller decides drop/block */
	buf->buffer[head & CONFIG_CONSOLE_BUF_MASK] = b;
	__mem_fence_release();            /* data visible before index publish */
	buf->head = head + 1;
	return true;
}

static bool console_buf_read(struct console_buf_t *buf, uint8_t *out)
{
	uint32_t tail = buf->tail;
	if (tail == buf->head)
		return false;                 /* empty */
	__mem_fence_acquire();
	*out = buf->buffer[tail & CONFIG_CONSOLE_BUF_MASK];
	buf->tail = tail + 1;
	return true;
}

static struct console_t *console_get(uint8_t usb_num)
{
	if (usb_num == 0 || usb_num > NUM_DEV_TTY_USB)
		return NULL;
	return &usb_console_table[usb_num - 1];
}

/* Strong override of the weak tty_flushinput(): discard USB bytes that have
 * not yet reached ttyinq.  We must ask core1 to drop the TinyUSB FIFO as
 * well, otherwise bytes already drained into the USB peripheral reappear at
 * the next prompt after tcflush() (the "Require password" / stray-'d' race).
 * The request/ack handshake closes the window where core1 is midway through
 * moving bytes into rbuf. */
void tty_flushinput(uint_fast8_t minor)
{
	usbconsole_flushinput((uint8_t)minor);
}

void usbconsole_flushinput(uint8_t usb_num)
{
	struct console_t *c = console_get(usb_num);
	uint8_t b;
	uint32_t spin;

	if (c == NULL)
		return;

	/* Ask core1 to drop the CDC hardware FIFO.  Clear the ack first so a
	 * stale ack from a previous flush cannot fool us. */
	c->flush_ack = 0;
	__mem_fence_release();
	c->flush_req = 1;

	/* Drain whatever core0 has already staged. */
	while (console_buf_read(&c->rbuf, &b))
		;

	/* Wait (bounded) for core1 to drop the FIFO.  If core1 is parked for
	 * a flash lockout it will honour the request as soon as it resumes;
	 * there is no point spinning for long here. */
	for (spin = 0; spin < 200000 && !c->flush_ack; spin++)
		tight_loop_contents();

	if (c->flush_ack) {
		__mem_fence_acquire();
		/* Anything core1 staged before it saw the request is stale. */
		while (console_buf_read(&c->rbuf, &b))
			;
	}
}

int usbconsole_getc(uint8_t usb_num)
{
	int c = -1;
	struct console_t *ct = console_get(usb_num);
	uint8_t b;

	if (ct != NULL && console_buf_read(&ct->rbuf, &b))
		c = b;
	return c;
}

ttyready_t usbconsole_ready(uint8_t usb_num)
{
	struct console_t *ct = console_get(usb_num);

	if (ct == NULL)
		return TTY_READY_LATER;
	if (console_buf_count(&ct->wbuf) < CONFIG_CONSOLE_BUF_LEN)
		return TTY_READY_NOW;
	return TTY_READY_LATER;
}

bool usbconsole_is_available(uint8_t usb_num)
{
	return usb_host_connected && usb_num != 0 && usb_num <= NUM_DEV_TTY_USB;
}

void usbconsole_putc(uint8_t usb_num, uint8_t c)
{
	struct console_t *ct = console_get(usb_num);

	if (ct == NULL)
		return;
	if (!console_buf_write(&ct->wbuf, c))
		usbconsole_tx_dropped++;
}

void usbconsole_sleeping(uint8_t usb_num)
{
	struct console_t *ct = console_get(usb_num);

	if (ct != NULL)
		ct->sleeping = true;
}

void tud_mount_cb(void)
{
	usb_host_connected = 1;
}

void tud_umount_cb(void)
{
	usb_host_connected = 0;
}

static void core1_main(void)
{
	/* With XIP execution the flash functions disable XIP while
	 * programming.  Core1 must be parked in RAM before that happens. */
	multicore_lockout_victim_init();
	/* Signal readiness before tusb_init(): core0 must not block on USB
	 * initialisation while waiting for the lockout victim. */
	core1_ready = 1;
	core1_state = 1;
	tusb_init();
	core1_state = 2;

	for (;;)
	{
		core1_state = 3;
		core1_heartbeat++;
		tud_task();

		/*
		We ignore tud_cdc_n_connected because if DTR is not set by the host
		all tty's will appear disconnected, which seems to be the case for pi pico
		*/

		/* Batch CDC TX: drain as many bytes as the USB endpoint can take and
		 * flush once.  The old one-byte-per-flush loop capped throughput at
		 * ~1 KB/s (one byte per USB frame), which caused the "2...6...4kb"
		 * symptom and kept the ring full. */
		for (int i = 0; i < CFG_TUD_CDC; i++)
		{
			struct console_t *ct = &usb_console_table[i];
			uint32_t room = tud_cdc_n_write_available(i);
			uint8_t chunk[64];
			uint32_t n = 0;
			while (n < room && n < sizeof(chunk))
			{
				if (!console_buf_read(&ct->wbuf, &chunk[n]))
					break;
				n++;
			}
			if (n)
				tud_cdc_n_write(i, chunk, n);
			/* Flush unconditionally: if a previous pre-enumeration write left
			 * TinyUSB's 64-byte TX FIFO full, write_available()==0 means we
			 * read no bytes (n==0) and the old code never flushed — so the
			 * stale FIFO was never pushed out and TX stalled permanently
			 * (console prints a few chars then freezes).  A periodic flush
			 * pushes any pending data once the host is draining. */
			tud_cdc_n_write_flush(i);
		}

		for (int i = 0; i < CFG_TUD_CDC; i++)
		{
			struct console_t *ct = &usb_console_table[i];

			/* Honour a tcflush() request before touching the FIFO, and
			 * do not read in the same iteration: any byte left in the
			 * FIFO at this point is stale by definition. */
			if (ct->flush_req)
			{
				tud_cdc_n_read_flush(i);
				__mem_fence_release();
				ct->flush_ack = 1;
				ct->flush_req = 0;
				continue;
			}

			/* Drain the CDC FIFO only while the staging ring has room.
			 * When it is full we stop reading and let the USB peripheral
			 * NAK the host, so input is throttled rather than dropped. */
			uint32_t space = CONFIG_CONSOLE_BUF_LEN - console_buf_count(&ct->rbuf);
			while (space > 0 && tud_cdc_n_available(i))
			{
				uint8_t b;
				if (tud_cdc_n_read(i, &b, 1) != 1)
					break;
				if (!console_buf_write(&ct->rbuf, b)) {
					usbconsole_rx_dropped++;
					break;
				}
				space--;
			}
		}
	}
}

void core1_init(void)
{
	//multicore_reset_core1();
#if NUM_DEV_TTY_USB > 0
	core1_ready = 0;
	multicore_launch_core1(core1_main);
	{
		volatile uint32_t spin;
		for (spin = 0; spin < 500000 && !core1_ready; spin++)
			tight_loop_contents();
	}
	/* If core1 did not become ready, continue anyway rather than hanging
	 * the console; flash operations will still use the SDK lockout. */
#endif
}
