#ifndef PICO_IOCTL_H
#define PICO_IOCTL_H

/* Reboot PI Pico into flash mode */
#define PICOIOC_FLASH 0x0001

/* Console/VT health telemetry (MASTER_PLAN L2).  Non-zero drop counters mean
 * the best-effort output path lost bytes; a stalled core1_heartbeat while
 * core0 is alive means the USB core is wedged.  Read via /dev/sys minor 6. */
#define PICOIOC_DIAG 0x0002

/* Fatal-exception crash record stored in QSPI (MASTER_PLAN S5/S8).  Read it
 * from /dev/sys minor 6 with PICOIOC_CRASHLOG and write the text to a file
 * (see rc / picoctl crashlog) so it survives USB re-enumeration. */
#define PICOIOC_CRASHLOG 0x0003
#define PICO_CRASHLOG_MAGIC 0x43525348UL	/* "CRSH" */

/* VT/LCD shadow buffer snapshot (MASTER_PLAN C1): read exactly what is
 * rendered on the LCD so serial and LCD can be compared byte-for-byte. */
#define PICOIOC_VTSHADOW 0x0004
struct pico_vtshadow {
    uint8_t cursorx;
    uint8_t cursory;
    uint8_t shadow[40][80];   /* VT_HEIGHT x VT_WIDTH on rpipico */
};

struct pico_crashlog {
    uint32_t magic;
    uint32_t len;
    uint32_t cause;
    uint32_t r[17];       /* r0-r12, sp, lr, pc, psr */
    uint32_t crc;
    unsigned char tail[428];  /* tail of the VT console ring */
};

struct pico_diag {
    uint32_t usb_tx_dropped;   /* console bytes dropped: USB TX ring full */
    uint32_t usb_rx_dropped;   /* USB input dropped: staging ring full */
    uint32_t vt_dropped;       /* LCD/VT deferred bytes dropped */
    uint32_t core1_heartbeat;  /* core1 loop counter (should keep rising) */
    uint32_t core1_state;      /* 0=init 1=pre-tusb 2=post-tusb 3=running */
};

#endif
