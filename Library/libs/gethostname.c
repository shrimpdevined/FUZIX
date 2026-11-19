#include <stdio.h>
#include <paths.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/*
 *	Emulate Berkley hostname calls
 */

static int openhostname(int flags, mode_t mode)
{
  return open(_PATH_HOSTNAME, flags, mode);
}

int gethostname(char *name, size_t len)
{
  int got;
  int fd;

  if (len == 0) {
    errno = EINVAL;
    return -1;
  }

  fd = openhostname(O_RDONLY, 0);
  if (fd == -1) {
    /* No host name configured: report an empty name */
    name[0] = 0;
    return 0;
  }

  /* Read at most len - 1 bytes so the name is always terminated */
  got = read(fd, name, len - 1);
  close(fd);
  if (got < 0) {
    name[0] = 0;
    return 0;
  }

  /* Drop any trailing white space, e.g. the newline in /etc/hostname */
  while (got > 0 && (name[got - 1] == '\n' || name[got - 1] == '\r' ||
                     name[got - 1] == ' ' || name[got - 1] == '\t'))
    got--;

  name[got] = 0;
  return 0;
}

int sethostname(const char *name, size_t len)
{
  int fd = openhostname(O_TRUNC|O_CREAT, 0644);
  if (fd == -1)
    return -1;
  /* Not clear how to handle this */
  if (write(fd, name, len) != len)
    return -1;
  fchmod(fd, 0644);
  close(fd);
  return 0;
}
