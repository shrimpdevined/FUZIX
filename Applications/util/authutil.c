/*
 * authutil.c - account database helpers
 *
 * All file updates are done by writing a temporary file in the same
 * directory and renaming it into place.  A crash or power loss can
 * therefore never leave a half written passwd, group or shadow file.
 *
 * This file is part of FUZIX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "authutil.h"

#define PASSWD_FILE	"/etc/passwd"
#define GROUP_FILE	"/etc/group"
#define AUTH_LOG_FILE	"/var/log/auth.log"

static int copy_string(char *dst, size_t len, const char *src)
{
	size_t n;

	if (src == NULL)
		src = "";
	n = strlen(src);
	if (n >= len)
		return -1;
	memcpy(dst, src, n + 1);
	return 0;
}

int auth_get_hash(const char *user, char *buf, size_t len)
{
	struct spwd *sp;
	struct passwd *pw;
	const char *hash = NULL;

	if (user == NULL || buf == NULL || len == 0)
		return -1;

	sp = getspnam(user);
	if (sp != NULL && sp->sp_pwdp != NULL)
		hash = sp->sp_pwdp;

	if (hash == NULL) {
		pw = getpwnam(user);
		if (pw == NULL)
			return -1;
		hash = pw->pw_passwd;
	}
	return copy_string(buf, len, hash);
}

int auth_locked(const char *hash)
{
	if (hash == NULL)
		return 1;
	return hash[0] == '*' || hash[0] == '!';
}

int auth_verify(const char *password, const char *hash)
{
	if (password == NULL || hash == NULL)
		return 0;
	if (hash[0] == '\0')
		return 1;		/* no password set */
	if (auth_locked(hash))
		return 0;
	return crypt_check(password, hash);
}

int auth_generate(const char *password, char *buf, size_t len)
{
	char *hash;

	hash = crypt_generate(password);
	if (hash == NULL)
		return -1;
	return copy_string(buf, len, hash);
}

/*
 *	Create a temporary file next to path.  Returns an open FILE *.
 */
static FILE *open_temp(const char *path, char *tmp, size_t tmplen,
		       mode_t mode)
{
	FILE *f;
	int fd;

	if (snprintf(tmp, tmplen, "%s.XXXXXX", path) >= (int)tmplen) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	fd = mkstemp(tmp);
	if (fd < 0)
		return NULL;
	if (fchmod(fd, mode) < 0) {
		close(fd);
		unlink(tmp);
		return NULL;
	}
	f = fdopen(fd, "w");
	if (f == NULL) {
		close(fd);
		unlink(tmp);
		return NULL;
	}
	return f;
}

static int commit_temp(FILE *f, char *tmp, const char *path)
{
	if (fflush(f) < 0 || ferror(f)) {
		fclose(f);
		unlink(tmp);
		return -1;
	}
	fsync(fileno(f));
	if (fclose(f) < 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int auth_set_hash(const char *user, const char *hash)
{
	char tmp[64];
	FILE *in, *out;
	struct spwd *sp;
	int found = 0;
	time_t now;
	long days = 0;

	if (user == NULL || hash == NULL)
		return -1;

	now = time(NULL);
	if (now > 0)
		days = (long)(now / 86400L);

	out = open_temp(_path_shadow, tmp, sizeof(tmp), 0600);
	if (out == NULL)
		return -1;

	in = fopen(_path_shadow, "r");
	if (in != NULL) {
		while ((sp = fgetspent(in)) != NULL) {
			if (strcmp(sp->sp_namp, user) == 0) {
				sp->sp_pwdp = (char *)hash;
				sp->sp_lstchg = days;
				if (sp->sp_min < 0)
					sp->sp_min = 0;
				if (sp->sp_max < 0)
					sp->sp_max = 99999;
				if (sp->sp_warn < 0)
					sp->sp_warn = 7;
				found = 1;
			}
			if (putspent(sp, out) < 0) {
				fclose(in);
				fclose(out);
				unlink(tmp);
				return -1;
			}
		}
		if (ferror(in)) {
			fclose(in);
			fclose(out);
			unlink(tmp);
			return -1;
		}
		fclose(in);
	}

	if (!found) {
		struct spwd ns;

		memset(&ns, 0, sizeof(ns));
		ns.sp_namp = (char *)user;
		ns.sp_pwdp = (char *)hash;
		ns.sp_lstchg = days;
		ns.sp_min = 0;
		ns.sp_max = 99999;
		ns.sp_warn = 7;
		ns.sp_inact = -1;
		ns.sp_expire = -1;
		if (putspent(&ns, out) < 0) {
			fclose(out);
			unlink(tmp);
			return -1;
		}
	}

	/* shadow is only readable by the superuser */
	if (chmod(tmp, 0600) < 0) {
		fclose(out);
		unlink(tmp);
		return -1;
	}
	return commit_temp(out, tmp, _path_shadow);
}

int auth_user_exists(const char *user)
{
	if (user == NULL)
		return 0;
	return getpwnam(user) != NULL;
}

int auth_group_exists(const char *group)
{
	if (group == NULL)
		return 0;
	return getgrnam(group) != NULL;
}

int auth_next_id(uid_t *uid, gid_t *gid)
{
	uid_t id;

	for (id = AUTH_ADMIN_UID; id < AUTH_UID_MAX; id++) {
		if (getpwuid(id) == NULL && getgrgid(id) == NULL) {
			*uid = id;
			*gid = id;
			return 0;
		}
	}
	return -1;
}

int auth_add_user(const char *name, uid_t uid, gid_t gid,
		  const char *gecos, const char *home, const char *shell,
		  const char *hash)
{
	char tmp[64];
	FILE *in, *out;
	struct passwd *pw;
	struct passwd np;
	int found = 0;

	if (name == NULL || home == NULL || shell == NULL)
		return -1;
	if (auth_user_exists(name))
		return -1;

	out = open_temp(PASSWD_FILE, tmp, sizeof(tmp), 0644);
	if (out == NULL)
		return -1;

	in = fopen(PASSWD_FILE, "r");
	if (in != NULL) {
		while ((pw = fgetpwent(in)) != NULL) {
			if (strcmp(pw->pw_name, name) == 0)
				found = 1;
			if (putpwent(pw, out) < 0) {
				fclose(in);
				fclose(out);
				unlink(tmp);
				return -1;
			}
		}
		if (ferror(in)) {
			fclose(in);
			fclose(out);
			unlink(tmp);
			return -1;
		}
		fclose(in);
	}

	if (found) {
		fclose(out);
		unlink(tmp);
		return -1;
	}

	np.pw_name = (char *)name;
	np.pw_passwd = "x";
	np.pw_uid = uid;
	np.pw_gid = gid;
	np.pw_gecos = (char *)(gecos ? gecos : "");
	np.pw_dir = (char *)home;
	np.pw_shell = (char *)shell;
	if (putpwent(&np, out) < 0) {
		fclose(out);
		unlink(tmp);
		return -1;
	}

	if (chmod(tmp, 0644) < 0) {
		fclose(out);
		unlink(tmp);
		return -1;
	}
	if (commit_temp(out, tmp, PASSWD_FILE) < 0)
		return -1;

	/* The caller normally adds the shadow entry too; do it here so an
	 * account can never exist with an unset hash in /etc/passwd. */
	if (hash != NULL)
		return auth_set_hash(name, hash);
	return 0;
}

static int write_group(FILE *out, const char *name, const char *pass,
		       gid_t gid, char **members)
{
	int i;

	if (fprintf(out, "%s:%s:%u:", name, pass ? pass : "x",
		    (unsigned int)gid) < 0)
		return -1;
	if (members != NULL) {
		for (i = 0; members[i] != NULL; i++) {
			if (i && fputc(',', out) == EOF)
				return -1;
			if (fputs(members[i], out) == EOF)
				return -1;
		}
	}
	if (fputc('\n', out) == EOF)
		return -1;
	return 0;
}

int auth_add_group(const char *name, gid_t gid)
{
	char tmp[64];
	FILE *in, *out;
	struct group *gr;

	if (name == NULL || auth_group_exists(name))
		return -1;

	out = open_temp(GROUP_FILE, tmp, sizeof(tmp), 0644);
	if (out == NULL)
		return -1;

	in = fopen(GROUP_FILE, "r");
	if (in != NULL) {
		while ((gr = fgetgrent(in)) != NULL) {
			if (strcmp(gr->gr_name, name) == 0) {
				fclose(in);
				fclose(out);
				unlink(tmp);
				return -1;
			}
			if (write_group(out, gr->gr_name, gr->gr_passwd,
					(gid_t)gr->gr_gid, gr->gr_mem) < 0) {
				fclose(in);
				fclose(out);
				unlink(tmp);
				return -1;
			}
		}
		if (ferror(in)) {
			fclose(in);
			fclose(out);
			unlink(tmp);
			return -1;
		}
		fclose(in);
	}

	if (write_group(out, name, "x", gid, NULL) < 0) {
		fclose(out);
		unlink(tmp);
		return -1;
	}

	if (chmod(tmp, 0644) < 0) {
		fclose(out);
		unlink(tmp);
		return -1;
	}
	return commit_temp(out, tmp, GROUP_FILE);
}

int auth_group_add_member(const char *group, const char *member)
{
	char tmp[64];
	FILE *in, *out;
	struct group *gr;
	char *members[GR_MAX_MEMBERS + 2];
	int found_group = 0;
	int i, n;

	if (group == NULL || member == NULL)
		return -1;

	out = open_temp(GROUP_FILE, tmp, sizeof(tmp), 0644);
	if (out == NULL)
		return -1;

	in = fopen(GROUP_FILE, "r");
	if (in == NULL) {
		fclose(out);
		unlink(tmp);
		return -1;
	}

	while ((gr = fgetgrent(in)) != NULL) {
		if (strcmp(gr->gr_name, group) != 0) {
			if (write_group(out, gr->gr_name, gr->gr_passwd,
					(gid_t)gr->gr_gid, gr->gr_mem) < 0)
				goto fail;
			continue;
		}

		found_group = 1;
		n = 0;
		if (gr->gr_mem != NULL) {
			for (i = 0; gr->gr_mem[i] != NULL; i++) {
				if (n >= GR_MAX_MEMBERS)
					goto fail;
				members[n++] = gr->gr_mem[i];
			}
		}
		for (i = 0; i < n; i++) {
			if (strcmp(members[i], member) == 0)
				break;
		}
		if (i == n) {
			if (n >= GR_MAX_MEMBERS)
				goto fail;
			members[n++] = (char *)member;
		}
		members[n] = NULL;
		if (write_group(out, gr->gr_name, gr->gr_passwd,
				(gid_t)gr->gr_gid, members) < 0)
			goto fail;
	}

	if (ferror(in))
		goto fail;
	fclose(in);
	in = NULL;

	if (!found_group) {
		fclose(out);
		unlink(tmp);
		return -1;
	}

	if (chmod(tmp, 0644) < 0)
		goto fail;
	return commit_temp(out, tmp, GROUP_FILE);

fail:
	if (in != NULL)
		fclose(in);
	fclose(out);
	unlink(tmp);
	return -1;
}

int auth_group_has_member(const char *group, const char *user)
{
	struct group *gr;
	int i;

	if (group == NULL || user == NULL)
		return 0;
	gr = getgrnam(group);
	if (gr == NULL || gr->gr_mem == NULL)
		return 0;
	for (i = 0; gr->gr_mem[i] != NULL; i++) {
		if (strcmp(gr->gr_mem[i], user) == 0)
			return 1;
	}
	return 0;
}

int auth_log(const char *fmt, ...)
{
	char msg[256];
	char line[320];
	char stamp[32];
	va_list ap;
	time_t now;
	char *ts;
	int fd;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	now = time(NULL);
	ts = ctime(&now);
	if (ts != NULL) {
		strncpy(stamp, ts, sizeof(stamp) - 1);
		stamp[sizeof(stamp) - 1] = '\0';
		/* ctime ends with a newline; the log line adds its own */
		if (stamp[0] != '\0') {
			size_t n = strlen(stamp);
			while (n > 0 && (stamp[n - 1] == '\n' ||
					 stamp[n - 1] == '\r'))
				stamp[--n] = '\0';
		}
	} else {
		snprintf(stamp, sizeof(stamp), "%ld", (long)now);
	}
	snprintf(line, sizeof(line), "%s %s\n", stamp, msg);

	fd = open(AUTH_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, line, strlen(line)) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}
