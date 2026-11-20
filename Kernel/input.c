/* MASTER_PLAN I5: a minimal, generic input core.
 *
 * A single small ring of timestamped events shared by all input transports
 * (CardKB keyboard, touch panel).  There is intentionally no /dev/input
 * device node yet: the tty path remains the authoritative way to deliver
 * keystrokes to userspace.  This core just gives the transports a common,
 * observable event sink that a future device node can drain via input_get().
 *
 * Producers run in two contexts (the CardKB poll in the timer IRQ and the
 * touch read in process context), so the short critical section uses di();
 * nested di() is safe because PRIMASK is saved/restored, and the window is a
 * handful of stores.  No dynamic allocation, no sleeping. */

#include <kernel.h>
#include <tty.h>

/* 16 events is ample for a sink with no consumer yet and keeps the .bss
 * cost (~192 B) small — RAM headroom on this build is under 1 KB. */
#define INPUT_RING_LEN 16u
#define INPUT_RING_MASK (INPUT_RING_LEN - 1u)

static struct input_event input_ring[INPUT_RING_LEN];
static volatile uint8_t input_head;
static volatile uint8_t input_tail;
volatile uint32_t input_dropped;

void input_emit(uint16_t type, uint16_t code, int16_t value, uint32_t ts)
{
	irqflags_t irq = di();
	uint8_t h = input_head;

	if ((uint8_t)(h - input_tail) >= INPUT_RING_LEN) {
		input_dropped++;
		irqrestore(irq);
		return;
	}
	input_ring[h & INPUT_RING_MASK].type = type;
	input_ring[h & INPUT_RING_MASK].code = code;
	input_ring[h & INPUT_RING_MASK].value = value;
	input_ring[h & INPUT_RING_MASK].ts = ts;
	input_head = h + 1;
	irqrestore(irq);
}

/* Drain one event into *ev.  Returns 1 if an event was available, 0 if the
 * ring was empty.  Called with interrupts enabled from consumer context. */
int input_get(struct input_event *ev)
{
	irqflags_t irq = di();
	uint8_t t = input_tail;

	if (t == input_head) {
		irqrestore(irq);
		return 0;
	}
	*ev = input_ring[t & INPUT_RING_MASK];
	input_tail = t + 1;
	irqrestore(irq);
	return 1;
}
