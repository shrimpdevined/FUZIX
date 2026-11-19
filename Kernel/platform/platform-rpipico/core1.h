#ifndef USBCONSOLE_H
#define USBCONSOLE_H

#include <tty.h>

extern void core1_init(void);
extern volatile uint32_t core1_heartbeat;  /* bumped each core1_main loop */

extern int usbconsole_getc(uint8_t usb_num);
extern ttyready_t usbconsole_ready(uint8_t usb_num);
extern uint32_t usbconsole_tx_space(uint8_t usb_num);
extern bool usbconsole_is_available(uint8_t usb_num);
extern void usbconsole_putc(uint8_t usb_num, uint8_t c);
extern void usbconsole_sleeping(uint8_t usb_num);
extern void usbconsole_flushinput(uint8_t usb_num);

#endif
