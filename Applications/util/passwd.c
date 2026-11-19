/*
 * passwd.c - change a user password
 *
 * This version stores hashes in /etc/shadow using the modular
 * PBKDF2-HMAC-SHA-256 password scheme.  The program is installed setuid
 * root so that ordinary users can update their own shadow entry, but
 * they must first prove knowledge of the old password.  Root may change
 * any account without the old password.
 *
 * This file is part of FUZIX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <time.h>

#include "authutil.h"

#define MIN_PASSWORD_LEN	1
#define PASSWORD_TRIES		3

static void wipe(void *p, size_t len)
{
	volatile unsigned char *v = p;

	while (len--)
		*v++ = 0;
}

static int ask_password(const char *user, char *hash, size_t len)
{
	char first[128], second[128];
	char *p;
	int tries;

	for (tries = 0; tries < PASSWORD_TRIES; tries++) {
		p = getpass("New password: ");
		if (p == NULL)
			return -1;
		strncpy(first, p, sizeof(first) - 1);
		first[sizeof(first) - 1] = '\0';

		if (strlen(first) < MIN_PASSWORD_LEN) {
			printf("Password must be at least %d characters.\n",
			       MIN_PASSWORD_LEN);
			wipe(first, sizeof(first));
			continue;
		}
		if (strcmp(first, user) == 0) {
			printf("Password must not be the login name.\n");
			wipe(first, sizeof(first));
			continue;
		}

		p = getpass("Retype new password: ");
		if (p == NULL) {
			wipe(first, sizeof(first));
			return -1;
		}
		strncpy(second, p, sizeof(second) - 1);
		second[sizeof(second) - 1] = '\0';

		if (strcmp(first, second) != 0) {
			printf("Sorry, passwords do not match.\n");
			wipe(first, sizeof(first));
			wipe(second, sizeof(second));
			continue;
		}
		if (auth_generate(first, hash, len) < 0) {
			wipe(first, sizeof(first));
			wipe(second, sizeof(second));
			return -1;
		}
		wipe(first, sizeof(first));
		wipe(second, sizeof(second));
		return 0;
	}
	return -1;
}

int main(int argc, char *argv[])
{
	struct passwd *pw;
	char self_name[32];
	char name[32];
	char old_hash[AUTH_HASH_MAX];
	char new_hash[AUTH_HASH_MAX];
	char *p;
	const char *requested;
	int privileged;
	uid_t ruid;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [name]\n", argv[0]);
		return 1;
	}

	ruid = getuid();
	privileged = (ruid == 0);

	if (geteuid() != 0 && !privileged) {
		fprintf(stderr, "passwd: must be installed setuid root\n");
		return 1;
	}

	pw = getpwuid(ruid);
	if (pw == NULL) {
		fprintf(stderr, "passwd: unknown uid %u\n",
			(unsigned int)ruid);
		return 1;
	}
	strncpy(self_name, pw->pw_name, sizeof(self_name) - 1);
	self_name[sizeof(self_name) - 1] = '\0';

	if (argc == 2) {
		requested = argv[1];
		if (!privileged && strcmp(requested, self_name) != 0) {
			fprintf(stderr,
				"passwd: you may not change the password for %s\n",
				requested);
			return 1;
		}
	} else
		requested = self_name;

	pw = getpwnam(requested);
	if (pw == NULL) {
		fprintf(stderr, "passwd: unknown user %s\n", requested);
		return 1;
	}
	strncpy(name, pw->pw_name, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';

	if (auth_get_hash(name, old_hash, sizeof(old_hash)) < 0)
		old_hash[0] = '\0';

	printf("Changing password for %s\n", name);

	if (!privileged) {
		if (auth_locked(old_hash)) {
			fprintf(stderr, "passwd: account %s is locked\n", name);
			wipe(old_hash, sizeof(old_hash));
			return 1;
		}
		if (old_hash[0] != '\0') {
			p = getpass("Old password: ");
			if (p == NULL || !auth_verify(p, old_hash)) {
				if (p != NULL)
					wipe(p, strlen(p));
				wipe(old_hash, sizeof(old_hash));
				auth_log("passwd: %s : authentication failure",
					 self_name);
				fprintf(stderr,
					"passwd: incorrect password\n");
				sleep(2);
				return 1;
			}
			wipe(p, strlen(p));
		}
	}
	wipe(old_hash, sizeof(old_hash));

	if (ask_password(name, new_hash, sizeof(new_hash)) < 0) {
		fprintf(stderr, "passwd: password unchanged\n");
		return 1;
	}

	if (auth_set_hash(name, new_hash) < 0) {
		perror("passwd: cannot update /etc/shadow");
		wipe(new_hash, sizeof(new_hash));
		return 1;
	}
	wipe(new_hash, sizeof(new_hash));

	auth_log("passwd: %s changed password for %s", self_name, name);
	printf("Password changed.\n");
	return 0;
}
