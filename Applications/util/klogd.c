/* MASTER_PLAN L1: kernel log daemon.  Drains the /dev/klog ring into
 * /var/log/klog, rotates at ROTATE_SIZE, and archives rotated copies to
 * /mnt/sd/log/ when the SD card is mounted.  Modelled on Linux klogd's
 * "drain the printk buffer and write it to a file" contract, kept simple
 * for 264 KB SRAM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <signal.h>

#define KLOG_DEV      "/dev/klog"
#define KLOG_FILE     "/var/log/klog"
#define KLOG_ARCHIVE  "/mnt/sd/log/klog.1"
#define ROTATE_SIZE   (32 * 1024)
#define BUF_SIZE      512

/* ioctl numbers must match Kernel/platform/platform-rpipico/klog.h */
#define KLOGIOC_FLUSH  0x4B04
#define KLOGIOC_REPLAY 0x4B05

static char buf[BUF_SIZE];

static void rotate(void)
{
    struct stat st;

    /* Only rotate if the log is actually large. */
    if (stat(KLOG_FILE, &st) == 0 && st.st_size < ROTATE_SIZE)
        return;

    /* Best-effort archive to SD (may not be mounted). */
    unlink(KLOG_ARCHIVE);
    rename(KLOG_FILE, KLOG_ARCHIVE);
}

int main(int argc, char **argv)
{
    int fd, out;

    if (argc > 1 && strcmp(argv[1], "-p") == 0) {
        /* dmesg-style: dump the current ring and exit. */
        fd = open(KLOG_DEV, O_RDONLY, 0);
        if (fd == -1) { perror(KLOG_DEV); return 1; }
        for (;;) {
            int n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }

    /* Replay the flash copy of the log from the previous boot, if any. */
    fd = open(KLOG_DEV, O_RDWR, 0);
    if (fd == -1) { perror(KLOG_DEV); return 1; }
    ioctl(fd, KLOGIOC_REPLAY);

    /* Detach: daemonise like cron (double fork). */
    if (fork())
        return 0;
    if (fork())
        return 0;

    for (;;) {
        int n;
        out = open(KLOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (out == -1) {
            sleep(5);
            continue;
        }
        rotate();
        for (;;) {
            n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            if (write(out, buf, n) != n)
                break;
        }
        close(out);
        sleep(2);
    }
}
