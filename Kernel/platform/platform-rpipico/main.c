#include <stdlib.h>
#include <kernel.h>
#include <kdata.h>
#include "devtty.h"
#include "picosdk.h"
#include <hardware/watchdog.h>
#include "core1.h"
#include "rawuart.h"
#include "kernel-armm0.def"
#include "globals.h"
#include "printf.h"
#include "devlcd.h"
#include "devkb.h"
#include "devtouch.h"

//the led that indicates power
//The on board one is pin 25
const uint POWER_LED = 25;

uint_fast8_t plt_param(char* p)
{
    char *s;
    uint8_t drv;
    if (strncmp(p, "tty=", sizeof("tty=")-1) == 0)
    {
        ttymap_count = 0;
        p += sizeof("tty=")-1;
        while(*p)
        {
            s = p;
            while(*p && *p != ',' && *p != '\n')
                p++;
            if(*p)
                *p++=0;
            drv = 0xff;
            if (strncmp(s, "usb", 3) == 0)
            {
                s += 3;
                drv = TTYDRV_USB;
            }
            else if(strncmp(s, "uart", 4) == 0)
            {
                s += 4;
                drv = TTYDRV_UART;
            }

            if (drv == 0xff || !(*s))
            {
                kprintf("invalid param %s\n", s);
                panic("tty=");
            }
            ttymap[ttymap_count+1].tty = atoi(s);
            ttymap[ttymap_count+1].drv = drv;
            ttymap_count++;
            if (ttymap_count >= NUM_DEV_TTY)
            {
                panic("ttycount");
            }
        }
        devtty_init();
        return 1;
    }
    return 0;
}

/* Forensics survive watchdog reset: the fatal handler captures the fault
 * frame into a RAM buffer (crashlog.c), which plt_monitor() flushes to a
 * reserved QSPI sector before the reset.  Userspace reads it via
 * PICOIOC_CRASHLOG and writes /var/log/crash.log at boot. */
void fatal_exception_handler(struct extended_exception_frame* eh)
{
    /* A core1 fault must not run the full handler: it shares the vector
     * table with core0, and crashlog_flush() would try to lock out the
     * very core it is running on (deadlock).  Record the minimal context
     * in scratch and reset immediately. */
    if (get_core_num() == 1) {
        watchdog_hw->scratch[0] = 0x43314331UL;  /* "C1C1" */
        watchdog_hw->scratch[1] = (uint32_t)eh->cause;
        watchdog_hw->scratch[2] = (uint32_t)eh->pc;
        watchdog_hw->scratch[3] = (uint32_t)eh->lr;
        watchdog_reboot(0, 0, 0);
        for (;;) { tight_loop_contents(); }
    }

    /* Capture BEFORE any printf: a hard-fault context may not survive
     * kprintf/VT re-entry.  No flash I/O here -- only a RAM copy. */
    crashlog_capture(eh);
    kprintf("FLAGRANT SYSTEM ERROR! EXCEPTION %d\n", eh->cause);
    kprintf(" r0=%p r1=%p  r2=%p  r3=%p\n", eh->r0, eh->r1, eh->r2, eh->r3);
    kprintf(" r4=%p r5=%p  r6=%p  r7=%p\n", eh->r4, eh->r5, eh->r6, eh->r7);
    kprintf(" r8=%p r9=%p r10=%p r11=%p\n", eh->r8, eh->r9, eh->r10, eh->r11);
    kprintf("r12=%p sp=%p  lr=%p  pc=%p\n", eh->r12, eh->sp, eh->lr, eh->pc);
    kprintf("PROGBASE=%p PROGLOAD=%p PROGTOP=%p\n", PROGBASE, PROGLOAD, PROGTOP);
    kprintf("UDATA=%p KSTACK=%p-%p\n", &udata, &udata+1, ((uint32_t)&udata) + UDATA_SIZE);
    kprintf("user mode relative: lr=%p pc=%p isp=%p brk=%p\n",
        eh->lr-PROGLOAD, eh->pc-PROGLOAD, udata.u_isp, udata.u_break);
    panic("fatal exception");
}

void syscall_handler(struct svc_frame* eh)
{
    udata.u_callno = *(uint8_t*)(eh->pc - 2);
    udata.u_argn = eh->r0;
    udata.u_argn1 = eh->r1;
    udata.u_argn2 = eh->r2;
    udata.u_argn3 = eh->r3;
    udata.u_insys = 1;

    unix_syscall();

    udata.u_insys = 1;
    eh->r0 = udata.u_retval;
    eh->r1 = udata.u_error;
}

int main(void)
{
    // early init to handle boot kernel messages
    devtty_early_init();

    /* Report a core1 fault from the previous boot (S4).  Core1 shares
     * the vector table with core0 but cannot run the full crash-log
     * flush (it would lock itself out), so it stores a minimal record. */
    if (watchdog_hw->scratch[0] == 0x43314331UL) {
        kprintf("Previous boot: CORE1 FAULT cause=%u pc=%x lr=%x\n",
                (unsigned)watchdog_hw->scratch[1],
                (unsigned)watchdog_hw->scratch[2],
                (unsigned)watchdog_hw->scratch[3]);
        watchdog_hw->scratch[0] = 0;
    }

    if ((U_DATA__U_SP_OFFSET != offsetof(struct u_data, u_sp)) ||
        (U_DATA__U_PTAB_OFFSET != offsetof(struct u_data, u_ptab)) ||
        (P_TAB__P_PID_OFFSET != offsetof(struct p_tab, p_pid)) ||
        (P_TAB__P_STATUS_OFFSET != offsetof(struct p_tab, p_status)) ||
        (UDATA_SIZE_ASM != UDATA_SIZE))
    {
        kprintf("U_DATA__U_SP = %d\n", offsetof(struct u_data, u_sp));
        kprintf("U_DATA__U_PTAB = %d\n", offsetof(struct u_data, u_ptab));
        kprintf("P_TAB__P_PID_OFFSET = %d\n", offsetof(struct p_tab, p_pid));
        kprintf("P_TAB__P_STATUS_OFFSET = %d\n", offsetof(struct p_tab, p_status));
        panic("bad offsets");
    }
    ramsize = (SRAM_END - SRAM_BASE) / 1024;
    procmem = USERMEM / 1024;
    //turn on power led
    gpio_init(POWER_LED);
    gpio_set_dir(POWER_LED, GPIO_OUT);
    gpio_put(POWER_LED, 1);

    di();

    devlcd_init();
    devkb_init();       /* laptop: CardKB is the primary input, always on */
    devtouch_init();

    /* Hardware watchdog: the single guarantee of recovery from ANY lockup
     * (Cortex-M0 nested-fault lockup, IRQ-off spin, XIP-off hang), none of
     * which can execute the software reset in plt_monitor().  The watchdog
     * is an independent hardware block clocked from the tick generator.
     * 5 s is generous: flash ops are short, so a healthy core0 always feeds
     * it from the 200 Hz timer tick; only a genuine lockup lets it expire. */
    watchdog_enable(5000, false);

    fuzix_main();
}

/* vim: sw=4 ts=4 et: */


