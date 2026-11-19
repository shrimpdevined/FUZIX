/*
 * sudo.c - execute a command with elevated privileges
 *
 * The invoking user must be root or a member of the "sudo" group.  A
 * password is checked against /etc/shadow and, after a successful
 * check, a timestamp is kept in /var/run/sudo so that repeated
 * commands do not re-prompt for five minutes.  The environment handed
 * to the command is rebuilt from scratch so no setuid-unsafe variables
 * survive.
 *
 * This file is part of FUZIX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "authutil.h"

#define SUDO_TIMESTAMP_DIR	"/var/run/sudo"
#define SUDO_TIMEOUT		300
#define SUDO_TRIES		3

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-k] [-u user] command [args...]\n",
		prog);
	exit(1);
}

static void wipe(void *p, size_t len)
{
	volatile unsigned char *v = p;

	while (len--)
		*v++ = 0;
}

static int timestamp_path(char *buf, size_t len, const char *user)
{
	if (strchr(user, '/') != NULL || strstr(user, "..") != NULL)
		return -1;
	snprintf(buf, len, "%s/%s", SUDO_TIMESTAMP_DIR, user);
	return 0;
}

static int timestamp_valid(const char *user)
{
	char path[64];
	struct stat st;
	time_t now;

	if (timestamp_path(path, sizeof(path), user) < 0)
		return 0;
	if (stat(path, &st) < 0)
		return 0;
	if (!S_ISREG(st.st_mode) || st.st_uid != 0)
		return 0;
	now = time(NULL);
	if (now < st.st_mtime)
		return 0;
	return (now - st.st_mtime) < SUDO_TIMEOUT;
}

static int timestamp_update(const char *user)
{
	char path[64];
	int fd;

	if (timestamp_path(path, sizeof(path), user) < 0)
		return -1;
	if (mkdir(SUDO_TIMESTAMP_DIR, 0700) < 0 && errno != EEXIST)
		return -1;
	chmod(SUDO_TIMESTAMP_DIR, 0700);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	fchmod(fd, 0600);
	if (close(fd) < 0)
		return -1;
	return 0;
}

static void timestamp_clear(const char *user)
{
	char path[64];

	if (timestamp_path(path, sizeof(path), user) == 0)
		unlink(path);
}

static int is_allowed(const char *user, uid_t uid, gid_t gid)
{
	struct group *gr;

	if (uid == 0)
		return 1;
	gr = getgrnam(AUTH_SUDO_GROUP);
	if (gr == NULL)
		return 0;
	if ((gid_t)gr->gr_gid == gid)
		return 1;
	return auth_group_has_member(AUTH_SUDO_GROUP, user);
}

static int valid_term(const char *term)
{
	if (term == NULL || *term == '\0' || strlen(term) >= 32)
		return 0;
	while (*term) {
		if (!isalnum((unsigned char)*term) && *term != '.' &&
		    *term != '_' && *term != '-')
			return 0;
		term++;
	}
	return 1;
}

static void join_args(char *buf, size_t len, char *const argv[])
{
	size_t o = 0;
	int i;

	buf[0] = '\0';
	for (i = 0; argv[i] != NULL; i++) {
		const char *s = argv[i];

		if (i && o + 1 < len)
			buf[o++] = ' ';
		while (*s && o + 1 < len)
			buf[o++] = *s++;
	}
	buf[o] = '\0';
}

static int authenticate(const char *user, const char *command)
{
	char hash[AUTH_HASH_MAX];
	char prompt[80];
	char *p;
	int tries;

	if (auth_get_hash(user, hash, sizeof(hash)) < 0) {
		fprintf(stderr, "sudo: cannot read password entry for %s\n",
			user);
		auth_log("sudo: %s : authentication failure (no shadow entry)",
			 user);
		return -1;
	}
	if (auth_locked(hash) || hash[0] == '\0') {
		fprintf(stderr, "sudo: account %s is locked\n", user);
		auth_log("sudo: %s : authentication failure (locked account)",
			 user);
		return -1;
	}

	snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", user);
	for (tries = 0; tries < SUDO_TRIES; tries++) {
		p = getpass(prompt);
		if (p == NULL) {
			wipe(hash, sizeof(hash));
			return -1;
		}
		if (auth_verify(p, hash)) {
			wipe(p, strlen(p));
			wipe(hash, sizeof(hash));
			return 0;
		}
		wipe(p, strlen(p));
		fprintf(stderr, "Sorry, try again.\n");
	}
	wipe(hash, sizeof(hash));
	auth_log("sudo: %s : authentication failure ; COMMAND=%s", user,
		 command);
	fprintf(stderr, "sudo: %d incorrect password attempts\n", SUDO_TRIES);
	return -1;
}

int main(int argc, char *argv[])
{
	const char *target_name = "root";
	const char *prog = argv[0];
	struct passwd *pw, *target;
	char inv_name[32];
	char target_name_buf[32];
	char target_dir[80];
	char target_shell[80];
	char command[256];
	char home_env[96];
	char user_env[48];
	char log_env[48];
	char shell_env[96];
	char term_env[48];
	char cwd[128];
	char *newenv[8];
	char *term;
	uid_t inv_uid, target_uid;
	gid_t inv_gid, target_gid;
	int kflag = 0;
	int i, envc;

	if (geteuid() != 0 && getuid() != 0) {
		fprintf(stderr, "sudo: must be installed setuid root\n");
		return 1;
	}

	pw = getpwuid(getuid());
	if (pw == NULL) {
		fprintf(stderr, "sudo: unknown uid %u\n",
			(unsigned int)getuid());
		return 1;
	}
	strncpy(inv_name, pw->pw_name, sizeof(inv_name) - 1);
	inv_name[sizeof(inv_name) - 1] = '\0';
	inv_uid = pw->pw_uid;
	inv_gid = pw->pw_gid;

	i = 1;
	while (i < argc) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (argv[i][0] != '-')
			break;
		if (strcmp(argv[i], "-k") == 0) {
			kflag = 1;
			i++;
			continue;
		}
		if (strcmp(argv[i], "-u") == 0) {
			if (++i >= argc)
				usage(prog);
			target_name = argv[i++];
			continue;
		}
		usage(prog);
	}

	if (kflag)
		timestamp_clear(inv_name);

	if (i >= argc) {
		if (kflag)
			return 0;
		usage(prog);
	}

	target = getpwnam(target_name);
	if (target == NULL) {
		fprintf(stderr, "sudo: unknown user %s\n", target_name);
		return 1;
	}
	strncpy(target_name_buf, target->pw_name,
		sizeof(target_name_buf) - 1);
	target_name_buf[sizeof(target_name_buf) - 1] = '\0';
	target_uid = target->pw_uid;
	target_gid = target->pw_gid;
	strncpy(target_dir, target->pw_dir[0] ? target->pw_dir : "/",
		sizeof(target_dir) - 1);
	target_dir[sizeof(target_dir) - 1] = '\0';
	strncpy(target_shell, target->pw_shell[0] ? target->pw_shell :
		"/bin/sh", sizeof(target_shell) - 1);
	target_shell[sizeof(target_shell) - 1] = '\0';

	join_args(command, sizeof(command), argv + i);

	if (!is_allowed(inv_name, inv_uid, inv_gid)) {
		fprintf(stderr, "sudo: %s is not allowed to run sudo\n",
			inv_name);
		auth_log("sudo: %s : command not allowed ; COMMAND=%s",
			 inv_name, command);
		return 1;
	}

	if (inv_uid != 0 && !timestamp_valid(inv_name)) {
		if (authenticate(inv_name, command) < 0)
			return 1;
		if (timestamp_update(inv_name) < 0)
			fprintf(stderr,
				"sudo: warning: cannot update timestamp\n");
	}

	/* Rebuild the environment.  Only values that cannot influence
	 * the dynamic loader, the shell or other setuid sensitive code
	 * are carried across. */
	term = getenv("TERM");
	envc = 0;
	newenv[envc++] = "PATH=/bin:/usr/bin";
	snprintf(home_env, sizeof(home_env), "HOME=%s", target_dir);
	newenv[envc++] = home_env;
	snprintf(user_env, sizeof(user_env), "USER=%s", target_name_buf);
	newenv[envc++] = user_env;
	snprintf(log_env, sizeof(log_env), "LOGNAME=%s", target_name_buf);
	newenv[envc++] = log_env;
	snprintf(shell_env, sizeof(shell_env), "SHELL=%s", target_shell);
	newenv[envc++] = shell_env;
	if (valid_term(term)) {
		snprintf(term_env, sizeof(term_env), "TERM=%s", term);
		newenv[envc++] = term_env;
	}
	newenv[envc] = NULL;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		strcpy(cwd, "?");
	auth_log("sudo: %s : TTY=%s ; PWD=%s ; USER=%s ; COMMAND=%s",
		 inv_name, ttyname(0) ? ttyname(0) : "?", cwd,
		 target_name_buf, command);

	environ = newenv;

	if (initgroups(target_name_buf, target_gid) < 0)
		fprintf(stderr, "sudo: warning: cannot set groups\n");
	if (setgid(target_gid) < 0) {
		perror("sudo: setgid");
		return 1;
	}
	if (setuid(target_uid) < 0) {
		perror("sudo: setuid");
		return 1;
	}

	execvp(argv[i], argv + i);
	perror("sudo: exec");
	return 1;
}
