/*
 * shadow.c - shadow password database
 *
 * Reads and writes /etc/shadow in the traditional colon separated
 * format:
 *
 *	name:password:lastchange:min:max:warn:inactive:expire:flag
 *
 * Numeric fields may be empty, which is reported as -1.  The reader
 * works on a raw file descriptor so it can share code between
 * getspent() and fgetspent().
 *
 * This file is part of FUZIX.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <shadow.h>

char *_path_shadow = "/etc/shadow";

#define SPWD_BUFFER_SIZE	256
#define SPWD_FIELDS		9

static int spwd_fd = -1;

static long spwd_number(const char *p)
{
	char *end;
	long v;

	if (*p == '\0')
		return -1;
	v = strtol(p, &end, 10);
	if (*end != '\0')
		return -1;
	return v;
}

static unsigned long spwd_flag(const char *p)
{
	char *end;
	unsigned long v;

	if (*p == '\0')
		return 0;
	v = strtoul(p, &end, 10);
	if (*end != '\0')
		return 0;
	return v;
}

static struct spwd *__getspent(int fd)
{
	static char line[SPWD_BUFFER_SIZE];
	static struct spwd sp;
	char *field[SPWD_FIELDS];
	char *p, *nl;
	int len, i;

	for (;;) {
		len = read(fd, line, sizeof(line));
		if (len <= 0)
			return NULL;

		nl = memchr(line, '\n', len);
		if (nl == NULL) {
			/* Line longer than the buffer: discard it. */
			do {
				len = read(fd, line, sizeof(line));
				if (len <= 0)
					return NULL;
				nl = memchr(line, '\n', len);
			} while (nl == NULL);
			continue;
		}
		lseek(fd, (long)(nl - line) - len + 1, SEEK_CUR);
		*nl = '\0';

		if (line[0] == '#' || line[0] == '\0')
			continue;

		p = line;
		for (i = 0; i < SPWD_FIELDS; i++) {
			field[i] = p;
			if (i == SPWD_FIELDS - 1)
				break;
			p = strchr(p, ':');
			if (p == NULL)
				break;
			*p++ = '\0';
		}
		if (i != SPWD_FIELDS - 1)
			continue;

		sp.sp_namp = field[0];
		sp.sp_pwdp = field[1];
		sp.sp_lstchg = spwd_number(field[2]);
		sp.sp_min = spwd_number(field[3]);
		sp.sp_max = spwd_number(field[4]);
		sp.sp_warn = spwd_number(field[5]);
		sp.sp_inact = spwd_number(field[6]);
		sp.sp_expire = spwd_number(field[7]);
		sp.sp_flag = spwd_flag(field[8]);
		return &sp;
	}
}

void setspent(void)
{
	if (spwd_fd != -1)
		close(spwd_fd);
	spwd_fd = open(_path_shadow, O_RDONLY);
}

void endspent(void)
{
	if (spwd_fd != -1)
		close(spwd_fd);
	spwd_fd = -1;
}

struct spwd *getspent(void)
{
	if (spwd_fd == -1)
		return NULL;
	return __getspent(spwd_fd);
}

struct spwd *fgetspent(FILE *file)
{
	if (file == NULL) {
		errno = EINVAL;
		return NULL;
	}
	return __getspent(fileno(file));
}

struct spwd *getspnam(const char *name)
{
	struct spwd *sp;
	int fd;

	if (name == NULL) {
		errno = EINVAL;
		return NULL;
	}

	fd = open(_path_shadow, O_RDONLY);
	if (fd < 0)
		return NULL;

	while ((sp = __getspent(fd)) != NULL) {
		if (strcmp(sp->sp_namp, name) == 0) {
			close(fd);
			return sp;
		}
	}
	close(fd);
	return NULL;
}

static int put_number(FILE *f, long value)
{
	if (value >= 0)
		return fprintf(f, "%ld", value);
	return 0;
}

int putspent(const struct spwd *sp, FILE *f)
{
	const char *name;
	const char *pass;

	if (sp == NULL || f == NULL) {
		errno = EINVAL;
		return -1;
	}
	name = sp->sp_namp ? sp->sp_namp : "";
	pass = sp->sp_pwdp ? sp->sp_pwdp : "";

	if (fprintf(f, "%s:%s:", name, pass) < 0)
		return -1;
	if (put_number(f, sp->sp_lstchg) < 0 || fputc(':', f) == EOF)
		return -1;
	if (put_number(f, sp->sp_min) < 0 || fputc(':', f) == EOF)
		return -1;
	if (put_number(f, sp->sp_max) < 0 || fputc(':', f) == EOF)
		return -1;
	if (put_number(f, sp->sp_warn) < 0 || fputc(':', f) == EOF)
		return -1;
	if (put_number(f, sp->sp_inact) < 0 || fputc(':', f) == EOF)
		return -1;
	if (put_number(f, sp->sp_expire) < 0 || fputc(':', f) == EOF)
		return -1;
	if (fprintf(f, "%lu\n", sp->sp_flag) < 0)
		return -1;
	return 0;
}
