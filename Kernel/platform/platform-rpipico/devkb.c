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

/* MASTER_PLAN I2: a small single-producer/single-consumer staging ring for
 * CardKB bytes.  The producer is devkb_poll() (timer tick, core0, IRQs off);
 * the consumer is devkb_drain(), called at the top of that same tick before
 * tty_interrupt() moves USB input into ttyinq.  Because both run in one tick
 * handler with interrupts disabled, volatile head/tail indices are sufficient
 * — no spinlock, so core1 can still be parked for a flash lockout.
 *
 * The old path injected straight into ttyinq and beeped+ dropped keys when it
 * was full.  Now, when ttyinq approaches its high-water mark we stop reading
 * the CardKB at all: the keystroke stays in the ATmega's own FIFO instead of
 * being lost.  Hysteresis (resume at low-water) avoids rapid on/off toggling. */
#define KB_RING_LEN 32u
#define KB_RING_MASK (KB_RING_LEN - 1u)
static volatile uint8_t kb_ring[KB_RING_LEN];
static volatile uint8_t kb_head;   /* producer index: devkb_poll() */
static volatile uint8_t kb_tail;   /* consumer index: devkb_drain() */
static volatile uint8_t kb_throttled;
volatile uint32_t kb_ring_dropped;

/* ttyinq high/low water marks (TTYSIZ is 132 on level 1, 256 on level 2). */
#define KB_HIGH_WATER 96u
#define KB_LOW_WATER  48u

static uint8_t kb_ring_used(void)
{
	return (uint8_t)(kb_head - kb_tail);
}

static void kb_stage(uint8_t b)
{
	if (kb_ring_used() >= KB_RING_LEN) {
		kb_ring_dropped++;
		return;
	}
	kb_ring[kb_head & KB_RING_MASK] = b;
	kb_head++;
}

/* Drain staged bytes into ttyinq.  Kept public only so a platform that wants
 * to call it from tty_interrupt() can; devkb_poll() already calls it each
 * tick, so this build needs no change to devtty.c.  A redundant second call
 * simply finds the ring empty. */
void devkb_drain(void)
{
	while (kb_tail != kb_head) {
		uint8_t b = kb_ring[kb_tail & KB_RING_MASK];
		/* tty_inproc() returns 0 when insq() rejected the byte.  Leave it
		 * (and everything after it) in the ring for the next tick. */
		if (tty_inproc(1, b) == 0)
			break;
		kb_tail++;
	}
}

/* Stage an arrow key as one contiguous three-byte run so the ESC '[' sequence
 * cannot be split by another input handler (MASTER_PLAN I3).  If the ring
 * cannot hold all three, drop the whole key rather than half a sequence. */
static void kb_stage_esc(uint8_t final_char)
{
	if (KB_RING_LEN - kb_ring_used() < 3u) {
		kb_ring_dropped++;
		return;
	}
	kb_stage(0x1B);
	kb_stage('[');
	kb_stage(final_char);
}

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

void devkb_poll(void) {
    uint8_t key = 0;

    /* Move bytes staged by the previous tick into ttyinq first. */
    devkb_drain();

    /* Resume only once ttyinq has drained comfortably below the high-water
     * mark.  This is the "unthrottle from tty_data_consumed() low-water"
     * policy from MASTER_PLAN I2, checked here in the tick instead of adding
     * a hook to devtty.c. */
    if (kb_throttled) {
        if (tty_pending(1) > KB_LOW_WATER)
            return;
        kb_throttled = 0;
    }

    /* Throttle before touching the I2C bus: an unread key stays queued in the
     * CardKB, whereas one injected into a full ttyinq would be lost. */
    if (tty_pending(1) >= KB_HIGH_WATER) {
        kb_throttled = 1;
        return;
    }
    if (kb_ring_used() >= KB_RING_LEN)
        return;

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

    /* Feed the generic input core (MASTER_PLAN I5) with the raw key code;
     * weak linkage keeps the build working even if Kernel/input.c is not in
     * the link.  The tty path below is unchanged and remains authoritative. */
    if (input_emit)
        input_emit(INPUT_EV_KEY, key, 1, time_us_32());

    switch (key) {
    case '\r':
        kb_stage('\n');
        break;
    case 0xB5: /* Up Arrow */
        kb_stage_esc('A');
        break;
    case 0xB6: /* Down Arrow */
        kb_stage_esc('B');
        break;
    case 0xB7: /* Right Arrow */
        kb_stage_esc('C');
        break;
    case 0xB4: /* Left Arrow */
        kb_stage_esc('D');
        break;
    default:
        kb_stage(key);
        break;
    }
}
