/*
 * su.c - substitute user
 *
 * A modernised replacement for the original UZI su.  Passwords are
 * checked against /etc/shadow, the target account must not be locked,
 * and the environment handed to the new shell is rebuilt so setuid
 * tricks such as IFS or ENV cannot survive.
 *
 * This file is part of FUZIX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#include "authutil.h"

static void wipe(void *p, size_t len)
{
	volatile unsigned char *v = p;

	while (len--)
		*v++ = 0;
}

static const char *base_name(const char *path)
{
	const char *p = strrchr(path, '/');

	return p ? p + 1 : path;
}

int main(int argc, char *argv[])
{
	const char *name = "root";
	struct passwd *pwd, *invoker;
	char inv_name[32];
	char target_name[32];
	char target_dir[80];
	char shell[80];
	char hash[AUTH_HASH_MAX];
	char home_env[96];
	char user_env[48];
	char log_env[48];
	char shell_env[96];
	char term_env[48];
	char arg0[32];
	char *sargv[16];
	char *newenv[8];
	char *term;
	char *p;
	uid_t target_uid;
	gid_t target_gid;
	int login_shell = 0;
	int argi, sargc, envc, fd;

	/* If stdin/stdout/stderr were closed a setuid program could end
	 * up writing to an unexpected file.  Make sure all three exist. */
	fd = open("/dev/null", O_RDONLY);
	if (fd < 0)
		return 1;
	if (fd < 3) {
		close(fd);
		return 1;
	}
	close(fd);

	if (geteuid() != 0 && getuid() != 0) {
		fprintf(stderr, "su: must be installed setuid root\n");
		return 1;
	}

	argi = 1;
	if (argi < argc && strcmp(argv[argi], "-") == 0) {
		login_shell = 1;
		argi++;
	}
	if (argi < argc && argv[argi][0] != '-')
		name = argv[argi++];

	pwd = getpwnam(name);
	if (pwd == NULL) {
		fprintf(stderr, "su: user %s does not exist\n", name);
		return 1;
	}
	strncpy(target_name, pwd->pw_name, sizeof(target_name) - 1);
	target_name[sizeof(target_name) - 1] = '\0';
	strncpy(target_dir, pwd->pw_dir[0] ? pwd->pw_dir : "/",
		sizeof(target_dir) - 1);
	target_dir[sizeof(target_dir) - 1] = '\0';
	strncpy(shell, pwd->pw_shell[0] ? pwd->pw_shell : "/bin/sh",
		sizeof(shell) - 1);
	shell[sizeof(shell) - 1] = '\0';
	target_uid = pwd->pw_uid;
	target_gid = pwd->pw_gid;

	if (access(shell, 0) < 0)
		strcpy(shell, "/bin/sh");

	invoker = getpwuid(getuid());
	if (invoker != NULL) {
		strncpy(inv_name, invoker->pw_name, sizeof(inv_name) - 1);
		inv_name[sizeof(inv_name) - 1] = '\0';
	} else
		snprintf(inv_name, sizeof(inv_name), "%u",
			 (unsigned int)getuid());

	if (getuid() != 0) {
		if (auth_get_hash(target_name, hash, sizeof(hash)) < 0) {
			fprintf(stderr, "su: cannot read password entry\n");
			return 1;
		}
		if (auth_locked(hash) || hash[0] == '\0') {
			fprintf(stderr, "su: account %s is locked\n",
				target_name);
			auth_log("su: %s -> %s : denied (locked account)",
				 inv_name, target_name);
			wipe(hash, sizeof(hash));
			return 1;
		}
		p = getpass("Password: ");
		if (p == NULL || !auth_verify(p, hash)) {
			if (p != NULL)
				wipe(p, strlen(p));
			wipe(hash, sizeof(hash));
			auth_log("su: %s -> %s : authentication failure",
				 inv_name, target_name);
			fprintf(stderr, "su: incorrect password\n");
			sleep(2);
			return 1;
		}
		wipe(p, strlen(p));
		wipe(hash, sizeof(hash));
	}

	term = getenv("TERM");

	if (login_shell) {
		snprintf(arg0, sizeof(arg0), "-%s", base_name(shell));
		if (chdir(target_dir) < 0 && chdir("/") < 0) {
			/* nothing sensible left to do */
		}
	} else {
		strncpy(arg0, base_name(shell), sizeof(arg0) - 1);
		arg0[sizeof(arg0) - 1] = '\0';
	}

	sargc = 0;
	sargv[sargc++] = arg0;
	while (argi < argc && sargc < (int)(sizeof(sargv) / sizeof(sargv[0])) - 1)
		sargv[sargc++] = argv[argi++];
	sargv[sargc] = NULL;

	envc = 0;
	newenv[envc++] = "PATH=/bin:/usr/bin";
	snprintf(home_env, sizeof(home_env), "HOME=%s", target_dir);
	newenv[envc++] = home_env;
	snprintf(user_env, sizeof(user_env), "USER=%s", target_name);
	newenv[envc++] = user_env;
	snprintf(log_env, sizeof(log_env), "LOGNAME=%s", target_name);
	newenv[envc++] = log_env;
	snprintf(shell_env, sizeof(shell_env), "SHELL=%s", shell);
	newenv[envc++] = shell_env;
	if (term != NULL && *term != '\0' && strlen(term) < 32) {
		snprintf(term_env, sizeof(term_env), "TERM=%s", term);
		newenv[envc++] = term_env;
	}
	newenv[envc] = NULL;

	auth_log("su: %s -> %s : success", inv_name, target_name);

	environ = newenv;

	if (initgroups(target_name, target_gid) < 0)
		fprintf(stderr, "su: warning: cannot set groups\n");
	if (setgid(target_gid) < 0) {
		perror("su: setgid");
		return 1;
	}
	if (setuid(target_uid) < 0) {
		perror("su: setuid");
		return 1;
	}

	execve(shell, sargv, newenv);
	perror("su: exec");
	return 1;
}
