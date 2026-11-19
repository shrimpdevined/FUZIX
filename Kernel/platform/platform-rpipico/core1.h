#ifndef CORE1_H
#define CORE1_H

#include <tty.h>

extern void core1_init(void);
extern volatile uint32_t core1_heartbeat;  /* bumped each core1_main loop */
extern volatile uint32_t core1_state;      /* 0=init 1=pre-tusb 2=post-tusb 3=running */
extern volatile uint32_t usbconsole_tx_dropped;
extern volatile uint32_t usbconsole_rx_dropped;

extern int usbconsole_getc(uint8_t usb_num);
extern ttyready_t usbconsole_ready(uint8_t usb_num);
extern bool usbconsole_is_available(uint8_t usb_num);
extern void usbconsole_putc(uint8_t usb_num, uint8_t c);
extern void usbconsole_sleeping(uint8_t usb_num);
extern void usbconsole_flushinput(uint8_t usb_num);

#endif
