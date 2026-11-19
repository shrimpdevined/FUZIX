#include <kernel.h>
#include "picosdk.h"
#include "rawuart.h"

/* Stubbed out so UART0/UART1 never reclaim GP0/GP1 (CardKB I2C0) or GP6-9 (Touch I2C1) */
void rawuart_early_init(void) {}
void rawuart_putc(uint8_t devn, uint8_t c) { (void)devn; (void)c; }
void rawuart_sleeping(uint8_t devn) { (void)devn; }
ttyready_t rawuart_ready(uint8_t devn) { (void)devn; return TTY_READY_NOW; }
int rawuart_getc(uint8_t devn) { (void)devn; return -1; }
void rawuart_setup(uint_fast8_t minor, uint_fast8_t devn, uint_fast8_t flags) {
    (void)minor; (void)devn; (void)flags;
}
