#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <exec.h>
#include "picosdk.h"
#include "pico_ioctl.h"
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <hardware/watchdog.h>

uint8_t sys_cpu = A_ARM;
uint8_t sys_cpu_feat = AF_CORTEX_M0;
uint8_t need_resched;
uaddr_t ramtop = (uaddr_t) PROGTOP;
uint8_t sys_stubs[sizeof(struct exec)];
uint16_t swap_dev = 0xffff;

/* Unused on this port */

void set_cpu_type(void) {}
void map_init(void) {}
void plt_discard(void) {}
void program_vectors(uint16_t* pageptr) {}

void plt_reboot(void)
{
    multicore_reset_core1();
    watchdog_reboot(0, 0, 0);
}

void plt_monitor(void)
{
    /* A panic must not permanently brick the device (that was the old
     * infinite sleep, and it also killed core1 = the only USB console, so
     * the failure looked like a dead hang).  Give the console time to flush
     * the diagnostic, then watchdog-reset so a transient fault recovers. */
    sleep_ms(1500);            /* allow panic/diagnostic text to drain */
    watchdog_reboot(0, 0, 0);  /* clean full reset, keeps app image */
    for (;;) { tight_loop_contents(); }
}

int plt_dev_ioctl(uarg_t request, char *data)
{
    used(data);
    if (request == PICOIOC_FLASH)
    {
        reset_usb_boot(0, 0);
        return 0;
    }
    return -1;
}

uaddr_t pagemap_base(void)
{
    return PROGBASE;
}

usize_t valaddr(const uint8_t *base, usize_t size, uint_fast8_t is_write)
{
        if (base + size < base)
                size = MAXUSIZE - (usize_t)base + 1;
        if (!base || base < (const uint8_t *)PROGBASE)
                size = 0;
        else if (base + size > (const uint8_t *)(size_t)udata.u_ptab->p_top)
                size = (uint8_t *)(size_t)udata.u_ptab->p_top - base;
        if (size == 0)
                udata.u_error = EFAULT;
        return size;
}

usize_t valaddr_r(const uint8_t *pp, usize_t l)
{
	return valaddr(pp, l, 0);
}

usize_t valaddr_w(const uint8_t *pp, usize_t l)
{
	return valaddr(pp, l, 1);
}

/* vim: sw=4 ts=4 et: */


