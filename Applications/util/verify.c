/* verify.c - stream-verify CRC32-trailered data files.  No malloc.
 *
 * Data retention (G2): V7 has no native checksums, so integrity lives in
 * userspace.  Each record is:
 *
 *   [ payload ][ "FZCK" ][ length:u32 LE ][ crc32:u32 LE ]
 *                        \______ 12-byte trailer ______/
 *
 * length is the payload byte count; crc32 is standard CRC-32 over the
 * payload only.  A 12-byte sliding window lets a single forward pass
 * locate trailers, so files far larger than RAM can be verified with a
 * fixed 4 KB buffer.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define BUFSZ   4096u
#define TRAIL   12u
#define MAXREC  (1u << 24)          /* 16 MB sanity cap on declared length */

static const unsigned char MAGIC[4] = { 'F', 'Z', 'C', 'K' };
#define CRC_INIT 0xFFFFFFFFu

static unsigned char buf[BUFSZ];    /* static: 4 KB, ~2% of the user pool */
static uint32_t   crctab[256];      /* runtime-built: 1 KB .bss, 0 flash rodata */
static int        table_ready;

static void crc_init_table(void)
{
	uint32_t c;
	int i, j;
	if (table_ready)
		return;
	for (i = 0; i < 256; i++) {
		c = (uint32_t)i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
		crctab[i] = c;
	}
	table_ready = 1;
}

static uint32_t crc_byte(uint32_t c, unsigned char b)
{
	return (c >> 8) ^ crctab[(c ^ (uint32_t)b) & 0xFFu];
}

static uint32_t crc_done(uint32_t c) { return c ^ 0xFFFFFFFFu; }

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* returns 0 OK, 1 verification failure, 2 usage/I/O error */
static int verify(const char *name)
{
	int fd, n, i, bad = 0;
	unsigned long off = 0, rec_start = 0, records = 0, total = 0;
	unsigned char win[TRAIL];
	int wn = 0;
	uint32_t crc = CRC_INIT, count = 0;

	fd = open(name, O_RDONLY);
	if (fd < 0) { perror(name); return 2; }
	crc_init_table();

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0) { perror(name); close(fd); return 2; }
		if (n == 0)
			break;
		for (i = 0; i < n; i++) {
			unsigned char b = buf[i];
			off++;              /* bytes consumed so far */
			if (wn == TRAIL) {
				uint32_t len, want, got;
				/* window full: oldest byte is not part of a trailer,
				 * fold it into the current record's CRC. */
				crc = crc_byte(crc, win[0]);
				count++;
				memmove(win, win + 1, TRAIL - 1);
				win[TRAIL - 1] = b;
				if (win[0] == MAGIC[0] && win[1] == MAGIC[1] &&
				    win[2] == MAGIC[2] && win[3] == MAGIC[3]) {
					len  = le32(win + 4);
					want = le32(win + 8);
					got  = crc_done(crc);
					if (len > MAXREC || count != len || got != want) {
						fprintf(stderr,
						  "%s: record %lu at offset %lu: %s"
						  " (len %lu/%lu, crc %08lx/%08lx)\n",
						  name, records, rec_start,
						  len > MAXREC ? "bad length" :
						  count != len ? "length mismatch" : "crc mismatch",
						  count, (unsigned long)len,
						  (unsigned long)got, (unsigned long)want);
						bad = 1;
						break;
					}
					records++;
					crc = CRC_INIT;     /* reset for next record */
					count = 0;
					wn = 0;             /* discard the 12-byte trailer */
					rec_start = off;    /* next record starts here */
				}
			} else {
				win[wn++] = b;
			}
		}
		total += (unsigned long)n;
		if (bad)
			break;
	}
	close(fd);
	if (bad)
		return 1;
	if (wn != 0 || count != 0) {
		fprintf(stderr, "%s: truncated/unterminated record %lu at offset %lu\n",
		        name, records, off);
		return 1;
	}
	printf("%s: %lu records OK, %lu bytes\n", name, records, total);
	return 0;
}

int main(int argc, char **argv)
{
	int rc = 0;
	if (argc < 2) {
		fprintf(stderr, "usage: verify file ...\n");
		return 2;
	}
	for (argv++; argc > 1; argv++, argc--)
		if (verify(*argv) != 0)
			rc = 1;
	return rc;
}
