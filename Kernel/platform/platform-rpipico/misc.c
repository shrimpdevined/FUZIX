#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <exec.h>
#include "picosdk.h"
#include "pico_ioctl.h"
#include "globals.h"
#include "core1.h"
#include "devlcd.h"
#include <vt.h>
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <hardware/watchdog.h>

uint8_t sys_cpu = A_ARM;
uint8_t sys_cpu_feat = AF_CORTEX_M0;
uint8_t need_resched;
uaddr_t ramtop = (uaddr_t) PROGTOP;
uint8_t sys_stubs[sizeof(struct exec)];
uint16_t swap_dev = 0xffff;

/* Set by any path that wants BOOTSEL (picoctl flash, or the SDK USB-reset
 * driver via the --wrap below).  Serviced by the core0 tick, which owns the
 * watchdog feed; see picosdk.h and pico-sdk issue #2689. */
volatile uint8_t plt_bootsel_pending;

/* Enter the ROM USB bootloader.  The bootrom schedules a BOOTSEL reset through
 * the hardware watchdog (about 10 ms) and idles in wfi waiting for it.  Our
 * timer tick feeds that watchdog every 5 ms, so unless we stop the feed the
 * reset is continually reloaded and never happens: the core just parks and a
 * later ordinary watchdog reset boots the application again.  Disabling
 * interrupts keeps this core from feeding it and watchdog_disable() clears
 * the application timeout, leaving the bootrom's short timeout as the only
 * one armed (pico-sdk #2689). */
void plt_bootsel_enter(void)
{
    typedef void (*reset_usb_boot_fn)(uint32_t, uint32_t);
    reset_usb_boot_fn fn;
    irqflags_t irq = di();

    /* Suppress the tick feed even on the failure path below. */
    plt_bootsel_pending = 1;
    watchdog_disable();

    fn = (reset_usb_boot_fn)rom_func_lookup(ROM_FUNC_RESET_USB_BOOT);
    if (fn != NULL)
        fn(0, 0);

    /* Only reached if the bootrom declined to reboot; do not leave the
     * machine running with no watchdog or with a stuck request flag. */
    plt_bootsel_pending = 0;
    watchdog_enable(5000, false);
    irqrestore(irq);
}

/* The SDK USB-reset driver (pico_usb_reset) calls this from core1 when the
 * host asks picotool to reboot into BOOTSEL.  We must not drive the watchdog
 * from there: core0's tick would keep reloading it.  Record the request and
 * park core1; the core0 tick performs the reset with the feed suppressed.
 * Linked with -Wl,--wrap=rom_reset_usb_boot_extra. */
void __wrap_rom_reset_usb_boot_extra(int gpio, uint32_t disable_mask,
                                     bool active_low)
{
    (void)gpio;
    (void)disable_mask;
    (void)active_low;
    plt_bootsel_pending = 1;
    __mem_fence_release();
    for (;;)
        tight_loop_contents();
}

/* Kernel-stack overflow canary (MASTER_PLAN M1).  The syscall stack starts at
 * udata+UDATA_SIZE and grows DOWN toward the u_data struct; the historic
 * "panic: corrupt inode" came from it overwriting u_files/u_cwd.  Place a
 * magic word in the unused u_s[] padding just above u_data (offset 0x120),
 * initialized at boot and inherited by fork/exec via the u_data copy.  The
 * tick checks it; a value change means the stack reached the guard.
 * Guarded by a compile-time assert so a future u_data growth cannot collide. */
#define KSTACK_CANARY_OFF  0x120u
#define KSTACK_CANARY_MAGIC 0x5A5AA5A5u
_Static_assert(sizeof(u_data) <= KSTACK_CANARY_OFF,
               "u_data grew past kstack canary offset");

void plt_stack_canary_init(void)
{
    *(volatile uint32_t *)((uint8_t *)&udata + KSTACK_CANARY_OFF) =
        KSTACK_CANARY_MAGIC;
}

uint8_t plt_stack_canary_ok(void)
{
    return *(volatile uint32_t *)((uint8_t *)&udata + KSTACK_CANARY_OFF) ==
           KSTACK_CANARY_MAGIC;
}

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
    /* Persist the fatal-exception record to QSPI BEFORE the reset.  The
     * live kprintf dump is frequently lost when USB re-enumerates; the
     * flash record is read by userspace into /var/log/crash.log at boot. */
    crashlog_flush();

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
        /* Runs on core0 in syscall context; enter BOOTSEL with the tick's
         * watchdog feed suppressed rather than racing it. */
        plt_bootsel_enter();
        return 0;   /* only if the bootrom declined to reboot */
    }
    if (request == PICOIOC_BOOTLOG)
    {
        /* Return the first 512 bytes of console output (boot text that
         * the USB TX ring dropped before host enumeration). */
        unsigned char buf[512];
        unsigned int n = bootlog_get(buf, sizeof(buf));
        uputw((uint16_t)n, data);
        return uput(buf, data + 2, n);
    }
    if (request == PICOIOC_VTSHADOW)
    {
        /* Snapshot exactly what is rendered on the LCD so userspace can
         * compare it with the serial stream (MASTER_PLAN C1).  Copy the
         * 3.2 KB shadow straight from kernel BSS to user space -- no
         * staging buffer (only ~2.6 KB RAM free). */
        uint8_t cx, cy;
        vt_get_cursor(&cx, &cy);
        uputc(cx, data);
        uputc(cy, data + 1);
        return uput((const unsigned char *)lcd_shadow_ptr(), data + 2, 80 * 40);
    }
    if (request == PICOIOC_CRASHLOG)
    {
        /* Return the persisted fatal-exception record (512 B) so userspace
         * can write it to /var/log/crash.log with cat/picoctl. */
        struct pico_crashlog rec;
        if (!crashlog_get(&rec))
            return -1;
        return uput((const unsigned char *)&rec, data, sizeof(rec));
    }
    if (request == PICOIOC_DIAG)
    {
        /* Linux-style observability: expose the best-effort output path's
         * drop counters and core1 liveness so userspace can tell a lost
         * prompt terminator from a render lag or a wedged USB core. */
        struct pico_diag d;
        d.usb_tx_dropped = usbconsole_tx_dropped;
        d.usb_rx_dropped = usbconsole_rx_dropped;
        d.vt_dropped = vt_dropped_count();
        d.core1_heartbeat = core1_heartbeat;
        d.core1_state = core1_state;
        return uput(&d, data, sizeof(d));
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


