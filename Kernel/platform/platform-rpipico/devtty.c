#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <stdbool.h>
#include <vt.h>
#include <tty.h>
#include "rawuart.h"
#include "picosdk.h"
#include <pico/multicore.h>
#include "core1.h"
#include "devtty.h"
#include "devlcd.h"

uint8_t ttybuf[TTYSIZ * NUM_DEV_TTY];

int ttymap_count;
struct ttymap ttymap[NUM_DEV_TTY + 1];
struct s_queue ttyinq[NUM_DEV_TTY + 1];
tcflag_t termios_mask[NUM_DEV_TTY + 1];

void no_setup(uint_fast8_t minor, uint_fast8_t devn, uint_fast8_t flags)
{
    used(minor);
    used(devn);
    used(flags);
}

struct ttydriver ttydrivers[2] =
    {
        {rawuart_putc, rawuart_ready, rawuart_sleeping, rawuart_getc, rawuart_setup},
        {usbconsole_putc, usbconsole_ready, usbconsole_sleeping, usbconsole_getc, no_setup},
};

static void devtty_defconfig(uint8_t drv, int count, int minor)
{
    int devnum = 1;
    while (devnum <= count)
    {
        ttymap[minor].tty = devnum++;
        ttymap[minor].drv = drv;
        if (drv == TTYDRV_USB)
        {
            termios_mask[minor] = _CSYS;
        }
        else
        {
            termios_mask[minor] = CSIZE | CBAUD | PARENB | PARODD | _CSYS;
        }
        minor++;
    }
}

/* To be called right after startup to be able to print boot messages */
void devtty_early_init(void)
{
    rawuart_early_init();
    core1_init();
    devtty_init();
}

void devtty_init(void)
{
    int defconfig = 0;
    if (ttymap_count == 0)
    {
        defconfig = 1;
    }
    for (int i = 1; i <= NUM_DEV_TTY; i++)
    {
        ttyinq[i].q_base = ttyinq[i].q_head = ttyinq[i].q_tail = &ttybuf[TTYSIZ * (i - 1)];
        ttyinq[i].q_size = TTYSIZ;
        ttyinq[i].q_count = 0;
        ttyinq[i].q_wakeup = TTYSIZ / 2;
    }

    if (defconfig)
    {
        /* The UART driver on this board is a stub, so USB is the only
         * usable console.  Wait for USB only while a host is actually
         * attached, so we capture every boot message; with no host we
         * skip the wait entirely and boot fast. */
        absolute_time_t until = delayed_by_ms(get_absolute_time(), DEV_USB_DETECT_TIMEOUT);
        int usb_ready = 0;
        while (absolute_time_diff_us(get_absolute_time(), until) > 0)
        {
            if (usbconsole_is_available(1))
            {
                usb_ready = 1;
                break;
            }
        }
        if (usb_ready)
        {
            /* Let core1 start draining the CDC TX ring before the first
             * kprintf so early boot output is not dropped. */
            until = delayed_by_ms(get_absolute_time(), DEV_USB_INIT_TIMEOUT);
            while (absolute_time_diff_us(get_absolute_time(), until) > 0)
                tight_loop_contents();
        }

        devtty_defconfig(TTYDRV_USB, NUM_DEV_TTY_USB, 1);
        devtty_defconfig(TTYDRV_UART, NUM_DEV_TTY_UART, 1 + NUM_DEV_TTY_USB);
        ttymap_count = NUM_DEV_TTY;
    }
}

/* Output for the system console (kprintf etc).  Single dispatch path: all
 * console bytes (kernel output and IRQ echoes) go through tty_putc().  The
 * old code called vtoutput()+usbconsole_putc() from BOTH kputchar() and
 * tty_putc(); because vtoutput() is slow and reentrant via vtpend, an IRQ
 * echo could enqueue its byte to USB before an in-progress kernel byte,
 * reordering output ("dNAND flash", stray 'd' before prompts). */
void kputchar(uint_fast8_t c)
{
    /* Kernel console output is non-blocking: usbconsole_putc() enqueues into
     * a ring and drops+counters if full.  Do NOT wait for TX room here —
     * kputchar is called from boot and fault/panic contexts with interrupts
     * possibly disabled, and a host that enumerated but is not draining CDC
     * would make an unbounded wait hang the kernel.  Occasional dropped bytes
     * (garbled boot text) are preferable to a deadlock. */
    if (c == '\n')
        tty_putc(1, '\r');
    tty_putc(1, c);
}

void tty_putc(uint_fast8_t minor, uint_fast8_t c)
{
    if (minor == 1) {
        /* USB first: fast, ordered enqueue.  LCD second: slow, visual only.
         *
         * No LCD/VT work may sit on the serial path: one character costs
         * several SPI transactions and several cell redraws, so rendering
         * inline made an 8 KB cat take 30 seconds.  The serial byte and
         * its deferred VT byte are enqueued here in the same order, so
         * the display mirrors the line exactly; the queue is drained from
         * idle context (plt_idle_hook). */
        /* Make the two enqueues atomic with respect to the timer IRQ, which
         * also calls tty_putc() (echo).  Without this, an IRQ echo can land
         * between them and the serial/LCD streams get different byte orders.
         * vtdefer()/usbconsole_putc() take nested di() safely (PRIMASK is
         * saved/restored), so no cross-core lock is introduced. */
        irqflags_t irq = di();
        usbconsole_putc(1, c);
        vtdefer(c);
        irqrestore(irq);
        return;
    }
    struct ttymap *map = &ttymap[minor];
    if (map->tty == 0)
        return;
    struct ttydriver *drv = &ttydrivers[map->drv];
    drv->putc(map->tty, c);
}

ttyready_t tty_writeready(uint_fast8_t minor)
{
    if (minor == 1) {
        /* Honest backpressure for the USB console.  With no host attached
         * nothing will ever drain the ring, so report ready (and let the
         * byte be dropped) rather than blocking the system forever. */
        if (!usbconsole_is_available(1))
            return TTY_READY_NOW;
        return usbconsole_ready(1);
    }
    struct ttymap *map = &ttymap[minor];
    if (map->tty == 0)
        return TTY_READY_LATER;
    struct ttydriver *drv = &ttydrivers[map->drv];
    return drv->ready(map->tty);
}

/* For the moment */
int tty_carrier(uint_fast8_t minor)
{
    return 1;
}

void tty_sleeping(uint_fast8_t minor)
{
    struct ttymap *map = &ttymap[minor];
    if (map->tty == 0)
        return;
    struct ttydriver *drv = &ttydrivers[map->drv];
    drv->sleeping(map->tty);
}

void tty_data_consumed(uint_fast8_t minor) {}

/*
 *	This function is called whenever the terminal interface is opened
 *	or the settings changed. It is responsible for making the requested
 *	changes to the port if possible. Strictly speaking it should write
 *	back anything that cannot be implemented to the state it selected.
 */
void tty_setup(uint_fast8_t minor, uint_fast8_t flags)
{
    struct ttymap *map = &ttymap[minor];
    if (map->tty == 0)
        return;
    struct ttydriver *drv = &ttydrivers[map->drv];
    drv->setup(minor, map->tty, flags);
}

void tty_interrupt(void)
{
    int c;
    for (int minor = 1; minor <= ttymap_count; minor++)
    {
        struct ttymap *map = &ttymap[minor];
        if (map->tty == 0)
            continue;
        struct ttydriver *drv = &ttydrivers[map->drv];
        while ((c = drv->getc(map->tty)) >= 0)
        {
            if (tty_inproc(minor, c) == 0)
            {
                break;
            }
        }
    }
}

/* Called from plt_idle() with interrupts enabled and no runnable process.
 * This is the safe point to do the work that must stay out of interrupt
 * context: deferred VT output (input echo) and repainting LCD rows that were
 * skipped because the shared SPI bus was busy with the SD card. */
void plt_idle_hook(void)
{
    if (udata.u_ininterrupt)
        return;
    vt_task();
    lcd_task();
}
/* vim: sw=4 ts=4 et: */
