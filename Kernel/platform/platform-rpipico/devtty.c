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
        /* No blocking wait for USB.  core1 + TinyUSB enumerate the CDC
         * asynchronously in the background, and the 512-byte TX ring buffers
         * early boot output until a host attaches.  Polling here only delayed
         * boot for a cosmetic benefit; the console works as soon as the
         * host mounts, regardless. */
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
        /* USB console TX is best-effort and NEVER blocks the writer.  When
         * the host is attached but not draining CDC (terminal not reading),
         * the 512-byte TX ring stays full and usbconsole_ready() would
         * return TTY_READY_LATER forever: tty_putc_maywait() would psleep()
         * mid-string and nothing calls tty_outproc() to wake it (the tick's
         * wake condition is false while the ring is full).  That deadlocked
         * userspace writes — e.g. getpass() printed only "P"/"Passw".  Report
         * ready always: usbconsole_putc() drops+counts on overflow instead,
         * so output is best-effort but the process always makes progress. */
        return TTY_READY_NOW;
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
