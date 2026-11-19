/*
 * firstboot.c - first boot account provisioning
 *
 * On a freshly written image there is no administrator account.  The
 * boot scripts run this program once, before getty starts, so that the
 * person who owns the hardware can choose their own login name and
 * password.  The account is placed in the "sudo" group and root itself
 * stays locked, which is the same model a modern Linux installer uses.
 *
 * The /etc/firstboot marker is removed only after the account has been
 * completely written.  If anything fails the marker survives and the
 * next boot tries again.
 *
 * This file is part of FUZIX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <termios.h>

#include "authutil.h"

#define FIRSTBOOT_MARKER	"/etc/firstboot"
#define AUTOLOGIN_FILE		"/etc/autologin"
#define HOME_ROOT		"/home"
#define MAX_LOGIN_LEN		8
#define MIN_PASSWORD_LEN	1
#define PASSWORD_TRIES		3

static const char *reserved_names[] = {
	"root", "daemon", "bin", "sys", "sync", "shutdown", "halt",
	"operator", "nobody", "adm", "lp", "mail", "news", "uucp",
	"games", "ftp", "man", "postmaster", "sshd", NULL
};

static void wipe(void *p, size_t len)
{
	volatile unsigned char *v = p;

	while (len--)
		*v++ = 0;
}

/*
 *	Read one line from the console.  Unlike stdio this cannot consume
 *	more than one line, which keeps it from racing with getpass().
 */
static int read_line(const char *prompt, char *buf, size_t len)
{
	size_t i = 0;
	char c;
	int over = 0;
	int n;

	/* Discard any keystrokes that arrived before this prompt (eg a harness
	 * answering the previous question early, or boot-time junk).  Must be
	 * immediately before writing the prompt so the answer is read fresh. */
	tcflush(0, TCIFLUSH);
	fflush(NULL);
	write(1, prompt, strlen(prompt));
	for (;;) {
		n = read(0, &c, 1);
		if (n <= 0)
			return -1;
		if (c == '\n' || c == '\r')
			break;
		if (i + 1 < len) {
			if (!over)
				buf[i++] = c;
		} else
			over = 1;
	}
	buf[i] = '\0';
	return 0;
}

static int valid_login(const char *name)
{
	size_t i, n = strlen(name);

	if (n == 0 || n > MAX_LOGIN_LEN)
		return 0;
	if (!((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
		return 0;
	for (i = 1; i < n; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '-'))
			return 0;
	}
	for (i = 0; reserved_names[i] != NULL; i++) {
		if (strcmp(name, reserved_names[i]) == 0)
			return 0;
	}
	return 1;
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

/*
 *	With no login password the system boots straight into the
 *	administrator's shell; sudo still asks for the account password.
 *	/etc/autologin carries the account name when that mode is chosen.
 */
static int write_autologin(const char *user)
{
	int fd, len;

	fd = open(AUTOLOGIN_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	len = strlen(user);
	if (write(fd, user, len) != len || write(fd, "\n", 1) != 1) {
		close(fd);
		return -1;
	}
	return close(fd);
}

static int configure_login(const char *user)
{
	char answer[16];
	int required;

	for (;;) {
		if (read_line("Require password at login? (y/n): ",
			      answer, sizeof(answer)) < 0)
			return -1;
		if (answer[0] == 'y' || answer[0] == 'Y') {
			required = 1;
			break;
		}
		if (answer[0] == 'n' || answer[0] == 'N') {
			required = 0;
			break;
		}
		printf("Please answer y or n.\n");
	}
	if (required) {
		unlink(AUTOLOGIN_FILE);
		return 0;
	}
	return write_autologin(user);
}

static int ensure_home(const char *home, uid_t uid, gid_t gid)
{
	static const char profile[] =
		"# ~/.profile: executed by the Bourne shell for login shells.\n"
		"\n"
		"# Bash style prompt: user@host:cwd, with # for the superuser.\n"
		"PS1='\\u@\\h:\\w\\$ '\n"
		"\n"
		"umask 022\n";
	static const char *const skeleton[] = {
		"Documents", "Music", "Pictures", "Videos"
	};
	char path[80];
	unsigned int i;
	int fd;

	if (mkdir(HOME_ROOT, 0755) < 0 && errno != EEXIST)
		return -1;
	if (mkdir(home, 0755) < 0 && errno != EEXIST)
		return -1;
	if (chown(home, uid, gid) < 0 || chmod(home, 0755) < 0)
		return -1;

	/* Give the account a Ubuntu-style home so `ls' shows folders
	 * immediately after first boot. */
	for (i = 0; i < sizeof(skeleton) / sizeof(skeleton[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", home, skeleton[i]);
		if (mkdir(path, 0755) < 0 && errno != EEXIST)
			return -1;
		if (chown(path, uid, gid) < 0)
			return -1;
	}

	snprintf(path, sizeof(path), "%s/.profile", home);
	if (access(path, F_OK) == 0)
		return 0;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return -1;
	if (write(fd, profile, sizeof(profile) - 1) != sizeof(profile) - 1) {
		close(fd);
		return -1;
	}
	if (fchown(fd, uid, gid) < 0) {
		close(fd);
		return -1;
	}
	if (close(fd) < 0)
		return -1;
	return 0;
}

/*
 *	An earlier run may have completed everything except removing the
 *	marker, or may have died between creating the account and adding
 *	it to the sudo group.  Repair those cases instead of asking for a
 *	new password.
 */
static int finish_existing(void)
{
	struct passwd *pw;
	char name[32];
	char home[80];
	uid_t uid;
	gid_t gid;

	pw = NULL;
	setpwent();
	while ((pw = getpwent()) != NULL) {
		if (pw->pw_uid >= AUTH_ADMIN_UID && pw->pw_uid < 65534)
			break;
	}
	if (pw == NULL) {
		endpwent();
		return 0;
	}
	strncpy(name, pw->pw_name, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	strncpy(home, pw->pw_dir, sizeof(home) - 1);
	home[sizeof(home) - 1] = '\0';
	uid = pw->pw_uid;
	gid = pw->pw_gid;
	endpwent();

	if (!auth_group_exists(AUTH_SUDO_GROUP)) {
		if (auth_add_group(AUTH_SUDO_GROUP, AUTH_SUDO_GID) < 0)
			return -1;
	}
	if (!auth_group_has_member(AUTH_SUDO_GROUP, name) &&
	    auth_group_add_member(AUTH_SUDO_GROUP, name) < 0)
		return -1;
	if (ensure_home(home, uid, gid) < 0)
		return -1;
	/* Repair path must be non-interactive: asking "Require password" here
	 * blocks rc (and getty never starts) if no one is watching.  If the
	 * autologin file already exists, keep it; otherwise leave the default
	 * (password required) without prompting. */
	{
		int fd = open(AUTOLOGIN_FILE, O_RDONLY);
		if (fd >= 0)
			close(fd);   /* preserve existing autologin setting */
	}

	if (unlink(FIRSTBOOT_MARKER) < 0)
		return -1;
	printf("System already provisioned for %s.\n", name);
	auth_log("firstboot: completed provisioning for %s", name);
	return 1;
}

/*
 *	A plain-text Debian-style provisioning screen carrying the FUZIX
 *	system identity.  uname(2) is the POSIX interface for this, so the
 *	version and machine shown are the ones the running kernel reports.
 */
static void show_banner(void)
{
	/* Make sure boot messages cannot leak into the first answer. */
	puts("");
	struct utsname u;

	printf("\n");
	printf("+------------------------------------------------+\n");
	printf("|  FUZIX - administrator setup                   |\n");
	printf("+------------------------------------------------+\n");
	if (uname(&u) == 0) {
		const char *host = u.nodename;

		/* u.machine is the kernel's board string (RP2040/RP2350,
		 * stamped by version.c).  Fall back to it if no hostname
		 * file is present so the banner never shows a blank host
		 * or a stale board name such as "rpipico". */
		if (host[0] == '\0')
			host = u.machine;
		printf("  System   : %s %s\n", u.sysname, u.release);
		printf("  Host     : %s\n", host);
		printf("  Platform : %s\n", u.machine);
	}
	printf("\n");
	printf("No administrator account exists on this system.\n");
	printf("Create one now.\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	char user[32];
	char gecos[64];
	char hash[AUTH_HASH_MAX];
	char home[80];
	uid_t uid;
	gid_t gid;
	size_t i;
	int ret;

	if (geteuid() != 0) {
		fprintf(stderr, "firstboot: must be run as root\n");
		return 1;
	}

	if (access(FIRSTBOOT_MARKER, F_OK) < 0) {
		printf("This system is already provisioned.\n");
		return 0;
	}

	ret = finish_existing();
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	show_banner();

	for (;;) {
		if (read_line("Username: ", user, sizeof(user)) < 0)
			return 1;
		if (!valid_login(user)) {
			printf("Use 1 to %d lowercase letters, digits, '_' or '-'.\n",
			       MAX_LOGIN_LEN);
			continue;
		}
		if (auth_user_exists(user)) {
			printf("That name is already in use.\n");
			continue;
		}
		break;
	}

	if (read_line("Full name (optional): ", gecos, sizeof(gecos)) < 0)
		return 1;
	for (i = 0; gecos[i] != '\0'; i++) {
		if (gecos[i] == ':')
			gecos[i] = ' ';
	}

	printf("\nChoose a password for %s.\n", user);
	if (ask_password(user, hash, sizeof(hash)) < 0) {
		fprintf(stderr, "firstboot: no password set, will retry next boot\n");
		return 1;
	}

	if (auth_next_id(&uid, &gid) < 0) {
		fprintf(stderr, "firstboot: no free user id\n");
		return 1;
	}
	snprintf(home, sizeof(home), "%s/%s", HOME_ROOT, user);

	if (ensure_home(home, uid, gid) < 0) {
		perror("firstboot: home directory");
		return 1;
	}
	if (!auth_group_exists(user)) {
		if (auth_add_group(user, gid) < 0) {
			fprintf(stderr, "firstboot: cannot create group\n");
			return 1;
		}
	} else if (getgrnam(user)->gr_gid != (int)gid) {
		fprintf(stderr, "firstboot: group %s has the wrong id\n", user);
		return 1;
	}
	if (!auth_user_exists(user)) {
		if (auth_add_user(user, uid, gid, gecos, home, "/bin/sh",
				  hash) < 0) {
			fprintf(stderr, "firstboot: cannot create user\n");
			return 1;
		}
	} else if (auth_set_hash(user, hash) < 0) {
		fprintf(stderr, "firstboot: cannot set password\n");
		return 1;
	}

	if (!auth_group_exists(AUTH_SUDO_GROUP) &&
	    auth_add_group(AUTH_SUDO_GROUP, AUTH_SUDO_GID) < 0) {
		fprintf(stderr, "firstboot: cannot create sudo group\n");
		return 1;
	}
	if (auth_group_add_member(AUTH_SUDO_GROUP, user) < 0) {
		fprintf(stderr, "firstboot: cannot add %s to sudo group\n",
			user);
		return 1;
	}
	if (configure_login(user) < 0) {
		fprintf(stderr, "firstboot: cannot configure login\n");
		return 1;
	}

	if (unlink(FIRSTBOOT_MARKER) < 0) {
		perror("firstboot: cannot remove marker");
		return 1;
	}
	sync();
	auth_log("firstboot: created user %s (uid %u)", user,
		 (unsigned int)uid);

	printf("\nPassword successfully set.\n\n");
	wipe(hash, sizeof(hash));
	return 0;
}
