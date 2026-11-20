/* MASTER_PLAN L1/C8: capture the first bytes of console output so boot
 * text lost to the 512 B USB TX ring (before the host enumerates) is still
 * recoverable.  A small RAM ring filled from tty_putc(); userspace reads
 * it via PICOIOC_BOOTLOG and writes /var/log/boot.log at boot.
 *
 * No flash I/O here: RAM survives until the next reset, and rc reads the
 * record immediately after mounting the filesystem.  512 B fits the spare
 * RAM budget (~2.6 KB free).
 */
#include <stdint.h>
#include <string.h>
#include <kernel.h>

static unsigned char boot_buf[512];
static unsigned int boot_len;

/* Called from tty_putc() for the console minor.  Only the FIRST 512 bytes
 * are kept -- that is exactly the boot banner/mount text that the USB
 * ring drops before the host starts reading. */
void bootlog_putc(uint_fast8_t minor, uint_fast8_t c)
{
	if (minor != 1)
		return;
	if (boot_len < sizeof(boot_buf))
		boot_buf[boot_len++] = c;
}

/* Copy out the captured bytes: first 2 bytes are the count (little endian),
 * followed by up to 510 bytes of console output. */
unsigned int bootlog_get(unsigned char *dst, unsigned int cap)
{
	unsigned int n = boot_len;

	if (n > sizeof(boot_buf))
		n = sizeof(boot_buf);
	if (n > cap)
		n = cap;
	memcpy(dst, boot_buf, n);
	return n;
}
