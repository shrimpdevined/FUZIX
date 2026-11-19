#include <kernel.h>
#include <version.h>
#include <kdata.h>
#include <devsys.h>
#include <blkdev.h>
#include <tty.h>
#include <devtty.h>
#include <dev/devsd.h>
#include <printf.h>
#include "globals.h"
#include "picosdk.h"
#include <hardware/irq.h>
#include <hardware/watchdog.h>
#include "core1.h"
#include "devlcd.h"
#include "devkb.h"
#include "devtouch.h"

/* Set by rawflash.c while core0 parks core1 for a flash erase/program.
 * See the core1-wedge check in timer_tick_cb (MASTER_PLAN S1). */
extern volatile uint8_t flash_lockout_active;


struct devsw dev_tab[] =  /* The device driver switch table */
{
// minor    open         close        read      write           ioctl
// ---------------------------------------------------------------------
  /* 0: /dev/hd - block device interface */
  {  blkdev_open,   no_close,   blkdev_read,    blkdev_write,	blkdev_ioctl},
  /* 1: /dev/fd - Floppy disk block devices */
  {  no_open,	    no_close,	no_rdwr,	no_rdwr,	no_ioctl},
  /* 2: /dev/tty	TTY devices */
  {  tty_open,     tty_close,   tty_read,  tty_write,  tty_ioctl },
  /* 3: /dev/lpr	Printer devices */
  {  no_open,     no_close,   no_rdwr,   no_rdwr,  no_ioctl  },
  /* 4: /dev/mem etc	System devices (one offs) */
  {  no_open,      sys_close,    sys_read, sys_write, sys_ioctl  },
  /* Pack to 7 with nxio if adding private devices and start at 8 */
  /* 5..7: Reserved padding */
  {  no_open,      no_close,     no_rdwr,  no_rdwr,   no_ioctl  },
  {  no_open,      no_close,     no_rdwr,  no_rdwr,   no_ioctl  },
  {  no_open,      no_close,     no_rdwr,  no_rdwr,   no_ioctl  },
  /* 8: /dev/fb0 - Hosyond 3.5" ST7796S Framebuffer */
  {  devfb_open,   devfb_close,  no_rdwr,  devfb_write, devfb_ioctl },
  /* 9: /dev/touch - FT6336U Capacitive Touch */
  {  devtouch_open, devtouch_close, devtouch_read, no_rdwr, no_ioctl },

};

static absolute_time_t now;

bool validdev(uint16_t dev)
{
    /* This is a bit uglier than needed but the right hand side is
       a constant this way */
    if(dev > ((sizeof(dev_tab)/sizeof(struct devsw)) << 8) - 1)
	return false;
    else
        return true;
}

static void timer_tick_cb(unsigned alarm)
{
    irqflags_t irq = di();
    udata.u_ininterrupt = 1;

    /* Feed the watchdog FIRST, before any potentially-long work (I2C keyboard
     * poll, tty drain).  Linux's watchdog discipline touches the device early
     * so a slow handler still has the full timeout window; doing it last meant
     * a stuck I2C read could consume the budget before the feed. */
    watchdog_update();

    /* Re-arm from the monotonic hardware timer each tick.  The old code
     * computed `next` from an init-time `now` that was NEVER advanced, so
     * after the first tick the target was always in the past and the primary
     * hardware_alarm_set_target() path never succeeded (every tick relied on
     * the fallback below).  Linux periodic timers re-arm from a monotonic
     * clock source; time_us_64() is ours. */
    update_us_since_boot(&now, time_us_64());
    absolute_time_t next;
    update_us_since_boot(&next, to_us_since_boot(now) + (1000000 / TICKSPERSEC));

    devkb_poll();       /* laptop: CardKB is the primary input, always on */
    /* One pass now moves up to a full 512-byte staging ring into ttyinq
     * (the old 8x loop existed for a 16-byte ring).  Keeping this bounded
     * matters: tty_interrupt() runs with interrupts disabled, and every
     * extra pass adds latency for the SD, USB and timer paths. */
    tty_interrupt();

    /* Wake writers that slept because the USB transmit ring was full.
     * core1 cannot call into the kernel safely, so the tick is the single
     * place that observes the ring draining.  When no host is attached the
     * ring will never drain: report the console as ready so writers are
     * released instead of sleeping forever. */
    if (usbconsole_ready(1) != TTY_READY_LATER ||
        !usbconsole_is_available(1))
        tty_outproc(1);

    timer_interrupt();

    /* Core1 owns TinyUSB (the only console).  If its heartbeat stops for
     * >2 s while core0 is alive, core1 is wedged (e.g. faulted during XIP-off
     * flash) — the "enumerated but mute" failure.  A full watchdog reset is
     * the only reliable recovery: core1 keeps the USB stack alive and cannot
     * be restarted independently.
     *
     * BUT: a heartbeat gap during a legitimate multicore lockout (flash
     * erase/program, Dhara GC, swap TRIM) is EXPECTED — core1 is parked in the
     * lockout handler, not wedged.  Counting that gap caused false full-chip
     * reboots under compiler load (MASTER_PLAN S1).  Suppress the stale
     * counter while rawflash holds core1, and reset it when lockout ends. */
    {
        static uint32_t last_hb;
        static uint32_t stale_ticks;
        if (flash_lockout_active) {
            /* Legitimate lockout: do not accumulate wedge evidence. */
            last_hb = core1_heartbeat;
            stale_ticks = 0;
        } else if (core1_heartbeat != last_hb) {
            last_hb = core1_heartbeat;
            stale_ticks = 0;
        } else if (++stale_ticks >= 4 * TICKSPERSEC) {
            /* 4 s of real inactivity outside lockout (raised from 2 s) —
             * require a sustained stall before the drastic whole-chip reset. */
            watchdog_reboot(0, 0, 0);
        }
    }

    if (hardware_alarm_set_target(0, next))
    {
        update_us_since_boot(&next, time_us_64() + (1000000 / TICKSPERSEC));
        hardware_alarm_set_target(0, next);
    }

    udata.u_ininterrupt = 0;
    irqrestore(irq);
}

#ifdef CONFIG_NET
extern void netdev_init(void);
#endif

void device_init(void)
{
    /* Timer interrup must be initialized before blcok devices.
       set_boot_line uses pause syscall which will not be operational otherwise. */
    hardware_alarm_claim(0);
    update_us_since_boot(&now, time_us_64());
    hardware_alarm_set_callback(0, timer_tick_cb);
    hardware_alarm_force_irq(0);

    /* Initialise the on-board QSPI NOR flash backing the Dhara FTL that
     * provides the firmware root and user filesystems (hda1/hda2). */
#ifdef CONFIG_PICO_FLASH
    flash_dev_init();
#endif
#ifdef CONFIG_NET
	netdev_init();
#endif
    sd_rawinit();
    devsd_init();
}

/* vim: sw=4 ts=4 et: */

