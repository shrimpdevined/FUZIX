#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "../pico_ioctl.h"

int main(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "--help") == 0)
    {
        puts("usage: picoctl [ --help ] <command>");
        puts("Command list:");
        puts("\tflash\tReset into flash mode.");
        puts("\tdiag\tShow console/VT drop counters and core1 liveness.");
        return 0;
    }
    int fd = open("/dev/sys", O_RDWR, 0);
    if (fd == -1)
    {
        perror("Failed to open /dev/sys");
        exit(1);
    }

    if (strcmp(argv[1], "diag") == 0)
    {
        struct pico_diag d;
        if (ioctl(fd, PICOIOC_DIAG, &d) != 0)
        {
            perror("Failed to read diagnostics");
            close(fd);
            exit(1);
        }
        printf("usb_tx_dropped: %u\n", d.usb_tx_dropped);
        printf("usb_rx_dropped: %u\n", d.usb_rx_dropped);
        printf("vt_dropped:     %u\n", d.vt_dropped);
        printf("core1_beat:     %u\n", d.core1_heartbeat);
        printf("core1_state:    %u\n", d.core1_state);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "flash") == 0)
    {
        if (ioctl(fd, PICOIOC_FLASH) != 0)
        {
            perror("Failed to perform operation");
            close(fd);
            exit(1);
        }
        close(fd);
        return 0;
    }

    puts("Unknown command; try --help");
    close(fd);
    return 1;
}
