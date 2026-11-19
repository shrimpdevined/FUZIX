/* xrecv.c - receive a file over the console using XMODEM-CRC.
 *
 * The console is a raw USB CDC serial line, so this tool puts the terminal
 * into raw mode, asks the host for CRC-16 XMODEM packets, and writes the
 * decoded payload to a file.  This is the FUZIX side of a transfer; the host
 * side is the xsend.py script that sends the packets.
 *
 * Protocol (XMODEM-CRC with a length header):
 *   receiver -> 'C'                       request CRC mode
 *   sender   -> SOH 0 0xFF data[128] crc  metadata block (length in bytes 0-3)
 *   receiver -> ACK
 *   sender   -> STX n ~n data[1024] crc   1024 byte data blocks
 *            -> ENQ n ~n data[512] crc    512 byte data blocks
 *            -> ETX n ~n data[256] crc    256 byte data blocks
 *            -> SOH n ~n data[128] crc    128 byte data blocks
 *            (n starts at 1; the host picks the largest size the console
 *             can carry without overrunning its input queue)
 *   receiver -> ACK / NAK
 *   sender   -> EOT, receiver -> ACK
 *
 * The length header lets the receiver write the exact byte count, so neither
 * text nor binary files acquire XMODEM's 0x1A padding.  A sender that skips
 * the header (plain XMODEM) is also accepted: full blocks are then written
 * and any padding is left in the file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#define SOH	0x01
#define STX	0x02
#define ETX	0x03
#define ENQ	0x05
#define EOT	0x04
#define ACK	0x06
#define NAK	0x15
#define CAN	0x18
#define CHUNK	128

/* Timeout, in timer ticks (config.h: 200 ticks/sec). */
#define TICKS	200

struct packet {
	int fd;			/* -1 while reading the metadata block */
	long length;		/* file length, -1 if the sender sent none */
	long written;		/* bytes written before this packet */
	long start;		/* file offset before this packet */
	unsigned char meta[4];	/* first four bytes of the metadata block */
	int metalen;
};

static struct termios oldterm;
static int raw = 0;

static void restore(void)
{
	if (raw)
		tcsetattr(0, TCSANOW, &oldterm);
}

/* Cycling dot "loading bar" drawn under the xrecv command while data is
 * received.  A carriage return rewinds to column zero so every frame
 * overwrites the previous one and the final "received" lands on this line. */
static void progress_tick(void)
{
	static const unsigned char counts[] = { 1, 2, 3, 4, 3, 2, 1, 2 };
	static unsigned int idx;
	unsigned char buf[5];
	int i, n;

	n = counts[idx++ % (sizeof(counts) / sizeof(counts[0]))];
	buf[0] = '\r';				/* back to column zero */
	for (i = 0; i < 4; i++)
		buf[i + 1] = (i < n) ? '.' : ' ';
	write(1, buf, sizeof(buf));
}

static void send_byte(unsigned char c)
{
	write(1, &c, 1);
	/* An acknowledge means a block landed: advance the loading bar. */
	if (c == ACK)
		progress_tick();
}

/* CRC-16/XMODEM: poly 0x1021, initial value 0. */
static unsigned crc16_add(unsigned crc, const unsigned char *p, int n)
{
	int i;

	while (n--) {
		crc ^= (unsigned)*p++ << 8;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc & 0xffff;
}

/* Read exactly len bytes; caller sets VMIN/VTIME on the tty. */
static int read_full(int fd, unsigned char *buf, int len)
{
	int got = 0, n;

	while (got < len) {
		n = read(fd, buf + got, len - got);
		if (n <= 0)
			return got;
		got += n;
	}
	return got;
}

/* Read a packet payload, writing it straight to the file (or into the
 * metadata area) while the CRC is accumulated.  Returns 0, -1 for a
 * recoverable protocol error, or -2 for a write error. */
static int read_payload(int size, struct packet *p)
{
	unsigned char buf[CHUNK];
	unsigned char tail[2];
	unsigned crc = 0;
	int left = size, n, i;

	p->start = p->fd >= 0 ? lseek(p->fd, 0, SEEK_CUR) : 0;

	while (left > 0) {
		n = left < CHUNK ? left : CHUNK;
		if (read_full(0, buf, n) != n)
			return -1;
		crc = crc16_add(crc, buf, n);

		for (i = 0; i < n && p->metalen < 4; i++)
			p->meta[p->metalen++] = buf[i];

		if (p->fd >= 0) {
			int w = n;

			if (p->length >= 0 && p->written + w > p->length)
				w = (int)(p->length - p->written);
			if (w > 0) {
				if (write(p->fd, buf, w) != w)
					return -2;
				p->written += w;
			}
		}
		left -= n;
	}

	if (read_full(0, tail, 2) != 2)
		return -1;
	if (crc != ((unsigned)tail[0] << 8 | tail[1]))
		return -1;
	return 0;
}

static int receive(const char *name)
{
	struct packet p;
	unsigned char hdr[2], c, want = 1;
	long length = -1, written = 0;
	int fd, tries, n, done = 0, size, rc, first = 1;

	fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror(name);
		return 1;
	}

	/* Handshake: repeatedly offer 'C' until the sender starts.
	 * Flush BEFORE sending C (not after): flushing after C can discard a
	 * first packet that a fast host sent in reaction to the C, leaving a
	 * permanent handshake desync ("no ACK for metadata block"). */
	tcflush(0, TCIFLUSH);		/* discard stale input once, up front */
	for (tries = 0; tries < 30 && !done; tries++) {
		send_byte('C');
		progress_tick();		/* overwrite the bare 'C' with dots */
		n = read(0, &c, 1);		/* VMIN=1, VTIME=TICKS/10 */
		if (n == 1 && (c == SOH || c == STX || c == ETX ||
			       c == ENQ || c == EOT))
			done = 1;
	}
	if (!done) {
		fprintf(stderr, "xrecv: no sender (timed out)\n");
		close(fd);
		return 1;
	}

	/* Packet loop.  Bound consecutive NAKs: a desynchronised sender (or one
	 * that has stopped) used to leave us emitting one NAK every 2 seconds
	 * forever, with the tty stuck in raw mode and the console wedged. */
	unsigned nak_streak = 0;
#define XRECV_NAK() do { \
		if (++nak_streak >= 12) { \
			fprintf(stderr, "xrecv: too many errors, aborting\r\n"); \
			close(fd); \
			restore(); \
			return 1; \
		} \
		send_byte(NAK); \
		/* Discard the rest of the rejected packet, then wait for the
		 * sender's retry start code.  Setting c = 0 here used to make the
		 * next loop iteration NAK again in a tight loop, aborting after
		 * a single bad packet instead of recovering. */ \
		tcflush(0, TCIFLUSH); \
		n = read(0, &c, 1); \
		if (n != 1) \
			c = 0; \
	} while (0)
	for (;;) {
		if (c == EOT) {
			send_byte(ACK);
			break;
		}
		if (c == CAN) {
			fprintf(stderr, "xrecv: sender cancelled\n");
			close(fd);
			return 1;
		}
		if (c != SOH && c != STX && c != ETX && c != ENQ) {
			XRECV_NAK();
			continue;
		}
		size = (c == STX) ? 1024 : (c == ENQ) ? 512 :
		       (c == ETX) ? 256 : 128;

		if (read_full(0, hdr, 2) != 2) {
			XRECV_NAK();
			continue;
		}
		if ((unsigned char)hdr[0] != (unsigned char)~hdr[1]) {
			XRECV_NAK();
			continue;
		}

		memset(&p, 0, sizeof(p));
		p.fd = fd;
		p.length = length;
		p.written = written;

		/* Block zero of the first packet carries the exact file
		 * length rather than file data. */
		if (first && hdr[0] == 0) {
			p.fd = -1;
			p.metalen = 0;
		} else {
			p.metalen = 4;
		}

		rc = read_payload(size, &p);
		if (rc == -2) {
			perror("write");
			close(fd);
			return 1;
		}
		if (rc < 0) {
			if (p.fd >= 0)
				lseek(fd, p.start, SEEK_SET);
			XRECV_NAK();
			continue;
		}

		nak_streak = 0;   /* good packet resets error streak */
		if (first && hdr[0] == 0) {
			length = (long)p.meta[0] | (long)p.meta[1] << 8 |
				 (long)p.meta[2] << 16 | (long)p.meta[3] << 24;
			/* Reject absurd lengths before touching the filesystem. */
			if (length < 0 || length > 16L * 1024 * 1024) {
				send_byte(NAK);
				send_byte(CAN);
				close(fd);
				return 1;
			}
			first = 0;
			send_byte(ACK);
			/* Read the next block's start code, then resume the loop. */
			n = read(0, &c, 1);
			if (n != 1)
				c = 0;
			continue;
		}
		first = 0;

		if ((unsigned char)hdr[0] != want) {
			/* Out of order: undo anything this packet wrote and
			 * ask for the block we are waiting for. */
			lseek(fd, p.start, SEEK_SET);
			XRECV_NAK();
			continue;
		}

		written = p.written;
		want++;
		send_byte(ACK);

		n = read(0, &c, 1);		/* next SOH / STX / EOT */
		if (n != 1)
			c = 0;
	}

	if (close(fd) < 0) {
		perror("close");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct termios t;
	int rc;

	if (argc != 2) {
		fprintf(stderr, "usage: %s filename\n", argv[0]);
		return 1;
	}
	if (tcgetattr(0, &oldterm) < 0) {
		perror("tcgetattr");
		return 1;
	}
	t = oldterm;
	cfmakeraw(&t);
	t.c_iflag &= ~IXON;		/* never treat 0x11/0x13 as flow control */
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = TICKS / 10;	/* ~2 seconds */
	if (tcsetattr(0, TCSANOW, &t) < 0) {
		perror("tcsetattr");
		return 1;
	}
	raw = 1;

	rc = receive(argv[1]);

	restore();
	if (rc == 0)
		/* Rewind one line (not CRLF) so "received" replaces the loading
		 * dots on the same line under the xrecv command. */
		printf("\rxrecv: received '%s'\r\n", argv[1]);
	return rc;
}
