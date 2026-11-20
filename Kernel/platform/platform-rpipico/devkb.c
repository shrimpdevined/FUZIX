#include <kernel.h>
#include <tty.h>
#include "picosdk.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "config.h"
#include "devkb.h"

/* Laptop: the CardKB is the ONLY input device, always enabled.  Runs the I2C
 * bus at 50kHz (the ATmega is unreliable at 100kHz) and re-probes each tick
 * until the keyboard answers, to survive its ~1.1s boot animation. */
static bool kb_present;
static uint8_t kb_probe_div;

/* Upper bound on a single 1-byte I2C read.  This runs from the 200 Hz timer
 * tick with interrupts disabled, so it directly bounds the IRQ-off window.
 * At 50 kHz a 1-byte read is ~0.36 ms; 2 ms is ample but halves the worst-case
 * stall versus the old 5 ms (MASTER_PLAN I1).  The ~1.1 s keypad boot race is
 * handled by the periodic re-probe below, NOT by this per-read timeout. */
#define KB_I2C_TIMEOUT_US 2000

void devkb_init(void) {
    uint8_t probe = 0;
    /* CardKB ATmega can be finicky about I2C speed; 50kHz is more reliable
     * than 100kHz with long/wiry laptop harnesses. */
    i2c_init(i2c0, 50 * 1000);
    gpio_set_function(PIN_KB_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_KB_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_KB_SDA);
    gpio_pull_up(PIN_KB_SCL);
    kb_present = (i2c_read_timeout_us(i2c0, I2C_KB_ADDR, &probe, 1, false, KB_I2C_TIMEOUT_US) == 1);
    kb_probe_div = 0;
}

static void kb_send_esc_seq(char final_char) {
    /* Keep the three-byte escape sequence atomic with respect to other
     * input handlers so ESC '[' cannot be split by a USB keystroke
     * (MASTER_PLAN I3).  di() nests safely inside the tick's di(). */
    irqflags_t irq = di();
    tty_inproc(1, 0x1B);
    tty_inproc(1, '[');
    tty_inproc(1, final_char);
    irqrestore(irq);
}

void devkb_poll(void) {
    uint8_t key = 0;
    if (!kb_present) {
        /* The CardKB's ATmega runs ~1.1s of NeoPixel animation before its
         * I2C slave starts.  The init-time probe can lose that race on a cold
         * boot; keep re-probing every 50ms (10 ticks @200Hz) until it answers.
         * A NACK costs ~90us and injects nothing. */
        if (++kb_probe_div < 10)
            return;
        kb_probe_div = 0;
        if (i2c_read_timeout_us(i2c0, I2C_KB_ADDR, &key, 1, false, KB_I2C_TIMEOUT_US) != 1)
            return;
        kb_present = true;
        /* fall through: process the byte from the detecting read */
    } else {
        if (i2c_read_timeout_us(i2c0, I2C_KB_ADDR, &key, 1, false, KB_I2C_TIMEOUT_US) != 1)
            return;
    }
    if (key == 0)
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
