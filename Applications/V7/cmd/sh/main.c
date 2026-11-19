/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */
/* Changes: Copyright (c) 1999 Robert Nordier. All rights reserved. */

#
/*
 * UNIX shell
 *
 * S. R. Bourne
 * Bell Telephone Laboratories
 *
 */

#include	"defs.h"
#include	<stdlib.h>
#include	<string.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	"sym.h"
#include	"timeout.h"
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<setjmp.h>
#include	<readline/readline.h>

UFD output = 2;
static BOOL beenhere = FALSE;
CHAR tmpout[20] = "/tmp/sh-";
FILEBLK stdfile;
FILE standin = &stdfile;
char* tempfile;

static void exfile(BOOL);

#ifdef BUILD_FSH
static char history[1024];
static char inbuf[256];
static unsigned int inleft;
static char *inptr;
static char ineof;

static int line_input(const char *prmpt)
{
	register int l;
	if (!isatty(standin->fdes))
		return -1;	/* Not a tty */
	do {
		l = rl_edit(standin->fdes, output,
			prmpt,
			inbuf, 256);
		if (l >= 0) {
			inbuf[l] = '\n';
			inptr = inbuf;
			inleft = l + 1;
			ineof = 0;
		}
		else
			ineof = 1;
	} while(l == -2);
	/* 0 - EOF, 1+ buffer including \n */
	return l + 1;
}

int lineread(int fd, char *bufp, int len)
{
	register char *buf = bufp;
	int bias = 0;
	int r;
	if (fd == standin->fdes && inleft) {
		if (len <= inleft) {
			memcpy(buf, inptr, len);
			inleft -= len;
			inptr += len;
			return len;
		}
		memcpy(buf, inptr, inleft);
		len -= inleft;
		buf += inleft;
		bias = inleft;
		inleft = 0;
	}
	r = 0;
	if (!ineof)
		r = read(fd, buf, len);
	if (r >= 0)
		r += bias;
	return r;
}

#else
#define rl_hinit(x,y)
#define line_input(x)	(-1)
#endif

/*
 *	Bash style prompt expansion.
 *
 *	\u user, \h short host, \H full host, \w cwd with ~ for $HOME,
 *	\W basename of cwd, \$ root marker, \s shell name, \n newline
 *	and \\ for a literal backslash. Unknown escapes are preserved.
 */

#define	PROMPT_MAX	512
#define	PROMPT_CWD_MAX	256

static const char *prompt_user(void)
{
	static char user[16];
	const char *name;

	if (user[0] == '\0') {
		name = getenv("LOGNAME");
		if (name == NULL || name[0] == '\0')
			name = getenv("USER");
		if (name == NULL || name[0] == '\0')
			name = getlogin();
		if (name == NULL || name[0] == '\0')
			user[0] = '\0';
		else {
			strncpy(user, name, sizeof(user) - 1);
			user[sizeof(user) - 1] = '\0';
		}
	}
	return user;
}

static const char *prompt_host(int full)
{
	static char host[64];
	static char shorthost[64];
	char *p;

	if (host[0] == '\0') {
		if (gethostname(host, sizeof(host) - 1) < 0)
			host[0] = '\0';
		host[sizeof(host) - 1] = '\0';
		p = host + strlen(host);
		while (p > host) {
			p--;
			if (*p != '\n' && *p != '\r' &&
			    *p != ' ' && *p != '\t')
				break;
			*p = '\0';
		}
		if (host[0] == '\0')
			strcpy(host, "localhost");
		strncpy(shorthost, host, sizeof(shorthost) - 1);
		shorthost[sizeof(shorthost) - 1] = '\0';
		p = strchr(shorthost, '.');
		if (p != NULL)
			*p = '\0';
	}
	return full ? host : shorthost;
}

static void prompt_cwd(char *out, size_t len, int basename)
{
	static char cwd[PROMPT_CWD_MAX];
	const char *home = homenod.namval;
	char *p;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		strcpy(cwd, ".");
	if (basename) {
		p = strrchr(cwd, '/');
		if (p == NULL || p[1] == '\0') {
			strncpy(out, cwd, len - 1);
			out[len - 1] = '\0';
		}
		else {
			strncpy(out, p + 1, len - 1);
			out[len - 1] = '\0';
		}
		return;
	}
	if (home != NULL && home[0] != '\0') {
		size_t hl = strlen(home);

		if (strncmp(cwd, home, hl) == 0 &&
		    (cwd[hl] == '\0' || cwd[hl] == '/') &&
		    strlen(cwd + hl) + 2 <= len) {
			out[0] = '~';
			strcpy(out + 1, cwd + hl);
			return;
		}
	}
	strncpy(out, cwd, len - 1);
	out[len - 1] = '\0';
}

static void prompt_expand(const char *fmt, char *out, size_t len)
{
	const char *s;
	char tmp[160];
	size_t o = 0;

	while (*fmt != '\0') {
		if (*fmt != '\\') {
			if (o + 1 >= len)
				break;
			out[o++] = *fmt++;
			continue;
		}
		fmt++;
		switch (*fmt) {
		case 'u': s = prompt_user(); break;
		case 'h': s = prompt_host(0); break;
		case 'H': s = prompt_host(1); break;
		case 'w': prompt_cwd(tmp, sizeof(tmp), 0); s = tmp; break;
		case 'W': prompt_cwd(tmp, sizeof(tmp), 1); s = tmp; break;
		case '$': s = (getuid() == 0) ? "#" : "$"; break;
		case 's': s = "sh"; break;
		case 'n': s = "\n"; break;
		case '\\': s = "\\"; break;
		case '\0':
			if (o + 1 < len)
				out[o++] = '\\';
			continue;
		default:
			if (o + 1 < len)
				out[o++] = '\\';
			if (o + 1 < len)
				out[o++] = *fmt;
			fmt++;
			continue;
		}
		fmt++;
		while (*s != '\0' && o + 1 < len)
			out[o++] = *s++;
	}
	out[o] = '\0';
}

int main(int c, const char *v[])
{
	register int rflag = ttyflg;

	/* initialise storage allocation */
	blokinit();

	stdsigs();

	setbrk(BRKINCR);
	addblok((POS) 0);

	rl_hinit(history, sizeof(history));

	/* set names from userenv */
	/* sh_getenv can call error handlers so initialize the
	   subshell trap and if it fails (eg being passed a broken
	   environment) just carry on instead of entering hyperspace */
	if (setjmp(subshell) == 0)
		sh_getenv();

	/* look for restricted */
/*	if(c>0 && any('r', *v) ) { rflag=0 ;} */

	/* look for options */
	dolc = options(c, v);
	if (dolc < 2)
		flags |= stdflg;

	if ((flags & stdflg) == 0)
		dolc--;

	dolv = v + c - dolc;
	dolc--;

	/* return here for shell file execution */
	setjmp(subshell);

	/* number of positional parameters */
	assnum(&dolladr, dolc);
	cmdadr = (char *)dolv[0];

	/* set pidname */
	assnum(&pidadr, getpid());

	/* set up temp file names */
	settmp();

	/* default ifs */
	dfault(&ifsnod, sptbnl);

	if ((beenhere++) == FALSE) {	/* ? profile */
		if (*cmdadr == '-'
		    && (input = pathopen(nullstr, profile)) >= 0) {
			exfile(rflag);
			flags &= ~ttyflg;
			;
		}
		if (rflag == 0) {
			flags |= rshflg;
		}

		/* open input file if specified */
		if (comdiv) {
			estabf(comdiv);
			input = -1;
		} else {
			input = ((flags & stdflg) ? 0 : chkopen(cmdadr));
			comdiv--;
			;
		}
//      } else {        *execargs=(char *)dolv; /* for `ps' cmd */
		;
	}
	exfile(0);
	done();
	return 0;	/* done() calls exit(); this keeps -Wreturn-type quiet */
}

static void exfile(BOOL prof)
{
	register L_INT mailtime = 0;
	register int userid;
	struct stat statb;

	/* move input */
	if (input > 0) {
		Ldup(input, INIO);
		input = INIO;
	}

	/* move output to safe place */
	if (output == 2) {
		Ldup(dup(2), OTIO);
		output = OTIO;
	}

	userid = getuid();

	/* decide whether interactive */
	if ((flags & intflg)
	    || ((flags & oneflg) == 0 && isatty(output) && isatty(input))) {
		dfault(&ps1nod, (userid ? stdprompt : supprompt));
		dfault(&ps2nod, readmsg);
		flags |= ttyflg | prompt;
		ignsig(KILL);
	} else {
		flags |= prof;
		flags &= ~prompt;
	}

	if (setjmp(errshell) && prof) {
		close(input);
		return;
	}

	/* error return here */
	loopcnt = breakcnt = peekc = 0;
	iopend = 0;
	if (input >= 0)
		initf(input);

	/* command loop */
	for (;;) {
		tdystak(0);
		stakchk();	/* may reduce sbrk */
		exitset();
		if ((flags & prompt) && standin->fstak == 0 && !eof) {
			if (mailnod.namval
			    && stat(mailnod.namval, &statb) >= 0
			    && statb.st_size
			    && (statb.st_mtime != mailtime)
			    && mailtime) {
				prs(mailmsg);
			}
			mailtime = statb.st_mtime;
			{
				static char pbuf[PROMPT_MAX];

				prompt_expand(ps1nod.namval, pbuf,
					      sizeof(pbuf));
				if (line_input(pbuf) < 0) {
					prs(pbuf);
					alarm(TIMEOUT);
				}
			}
			flags |= waiting;
		}

		trapnote = 0;
		peekc = readc();
		if (eof)
			return;
		alarm(0);
		flags &= ~waiting;
		execute(cmd(NL, MTFLG), 0, NULL, NULL);
		eof |= (flags & oneflg);
	}
}

void chkpr(char eor)
{
	if ((flags & prompt) && standin->fstak == 0 && eor == NL) {
		static char pbuf[PROMPT_MAX];

		prompt_expand(ps2nod.namval, pbuf, sizeof(pbuf));
		if (line_input(pbuf) < 0)
			prs(pbuf);
	}
}

void settmp(void)
{
	itos(getpid());
	serial = 0;
	tempfile = movstr(numbuf, &tmpout[TMPNAM]);
}

void Ldup(register int fa, register int fb)
{
	dup2(fa, fb);
	close(fa);
	fcntl(fb, F_SETFD, FD_CLOEXEC);
}
