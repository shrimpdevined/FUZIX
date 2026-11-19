#ifndef PICO_IOCTL_H
#define PICO_IOCTL_H

/* Reboot PI Pico into flash mode */
#define PICOIOC_FLASH 0x0001

/* Console/VT health telemetry (MASTER_PLAN L2).  Non-zero drop counters mean
 * the best-effort output path lost bytes; a stalled core1_heartbeat while
 * core0 is alive means the USB core is wedged.  Read via /dev/sys minor 6. */
#define PICOIOC_DIAG 0x0002

struct pico_diag {
    uint32_t usb_tx_dropped;   /* console bytes dropped: USB TX ring full */
    uint32_t usb_rx_dropped;   /* USB input dropped: staging ring full */
    uint32_t vt_dropped;       /* LCD/VT deferred bytes dropped */
    uint32_t core1_heartbeat;  /* core1 loop counter (should keep rising) */
    uint32_t core1_state;      /* 0=init 1=pre-tusb 2=post-tusb 3=running */
};

#endif
