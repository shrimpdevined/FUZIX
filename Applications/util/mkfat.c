#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>

/*
 * mkfat - format an SD card as a FAT filesystem.
 *
 * The kernel side lives in devfat.c (devfat_mkfs) and is reached through a
 * block-device ioctl.  The whole drive (partition 0) is formatted, and the
 * kernel writes a DOS partition table with a single FAT partition.
 */
int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/hdb";
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    printf("Formatting %s as FAT...\n", dev);
    if (ioctl(fd, HDIO_MKFS, 0) < 0) {
        perror("format failed");
        close(fd);
        return 1;
    }
    close(fd);
    printf("Done. SD card formatted successfully.\n");
    return 0;
}
