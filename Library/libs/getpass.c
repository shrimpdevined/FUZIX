/* getpass.c
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define EOF	(-1)

static int gp_getchar(int fd)
{
	static char ch;
	return (read(fd, &ch, 1) == 1) ? (unsigned char)ch : EOF;
}

static char *_gets(int fd, char *buf, int len)
{
	int ch, i = 0;

	while (i < len) {
		ch = gp_getchar(fd);
		if (ch == EOF) {
			if (i == 0)
				return NULL;
			break;
		}
		if (ch == '\n' || ch == '\r')
			break;
		buf[i++] = ch;
	}
	buf[i] = 0;
	return buf;
}

char *getpass(char *prompt)
{
	static char result[128];
	struct termios t;
	tcflag_t ol = 0;
	int own = 0;
	int fd;
	int tv;

	/* Prefer the controlling terminal.  When there is none (for
	 * example while the boot scripts run) fall back to standard
	 * input rather than failing outright. */
	fd = open("/dev/tty", O_RDWR);
	if (fd == -1)
		fd = 0;
	else
		own = 1;

	/* Make sure anything already printed appears before the prompt, and
	 * discard keystrokes buffered before this prompt so the answer is
	 * always read fresh (deterministic prompt->answer sequencing). */
	fflush(NULL);
	tcflush(fd, TCIFLUSH);

	/* Disable ECHO BEFORE the prompt, so an early password keystroke is
	 * never reflected to the console (mirrors on both USB and LCD). */
	tv = tcgetattr(fd, &t);
	if (tv == 0) {
		ol = t.c_lflag;
		t.c_lflag &= ~(ECHO | ECHOE | ECHOK);
		tcsetattr(fd, TCSANOW, &t);
	}

	/* display the prompt */
	write(fd, prompt, strlen(prompt));
	/* read the input */
	if (_gets(fd, result, sizeof(result) - 1) == NULL)
		result[0] = 0;
	if (tv == 0) {
		t.c_lflag = ol;
		tcsetattr(fd, TCSANOW, &t);
	}
	/* The newline isn't echoed as we have echo off, so we need to
	   output it as the end of the task */
	write(fd, "\n", 1);
	if (own)
		close(fd);
	return result;
}
