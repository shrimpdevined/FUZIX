#include <kernel.h>
#include <tty.h>
#include "picosdk.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "config.h"
#include "devkb.h"

/* Set only when a device actually ACKs at the CardKB address.  Guarantees no
 * bytes are injected when there is no keypad fitted.  Re-probed periodically
 * because the CardKB's ATmega runs ~1.1 s of NeoPixel startup animation before
 * its I2C slave (Wire.begin) comes up — a single boot-time probe loses that
 * race and would latch "absent" forever. */
static bool kb_present;
static uint8_t kb_probe_div;

void devkb_init(void) {
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(PIN_KB_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_KB_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_KB_SDA);
    gpio_pull_up(PIN_KB_SCL);
    kb_present = false;
    kb_probe_div = 0;
}

static void kb_send_esc_seq(char final_char) {
    tty_inproc(1, 0x1B);
    tty_inproc(1, '[');
    tty_inproc(1, final_char);
}

void devkb_poll(void) {
    uint8_t key = 0;
    if (!kb_present) {
        /* Keep probing until the CardKB boots (~1.1 s) and answers.  10 ticks
         * @ 200 Hz = 50 ms between probes; a NACK injects nothing. */
        if (++kb_probe_div < 10)
            return;
        kb_probe_div = 0;
        kb_present = (i2c_read_timeout_us(i2c0, I2C_KB_ADDR, &key, 1, false, 5000) == 1);
        if (!kb_present)
            return;
    }
    /* Use 5ms timeout so a slow keypad never freezes the timer interrupt. */
    if (i2c_read_timeout_us(i2c0, I2C_KB_ADDR, &key, 1, false, 5000) != 1 || key == 0)
        return;

    switch (key) {
    case '\r':
        tty_inproc(1, '\n');
        break;
    case 0xB5: /* Up Arrow */
        kb_send_esc_seq('A');
        break;
    case 0xB6: /* Down Arrow */
        kb_send_esc_seq('B');
        break;
    case 0xB7: /* Right Arrow */
        kb_send_esc_seq('C');
        break;
    case 0xB4: /* Left Arrow */
        kb_send_esc_seq('D');
        break;
    default:
        tty_inproc(1, key);
        break;
    }
}
