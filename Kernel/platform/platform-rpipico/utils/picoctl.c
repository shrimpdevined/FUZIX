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
        puts("\tcrashlog [file]\tDump last fatal-exception record (to file).");
        puts("\tvtshadow\tDump the LCD shadow buffer (serial vs LCD stream).");
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

    if (strcmp(argv[1], "crashlog") == 0)
    {
        struct pico_crashlog r;
        static const char *names[17] = {
            "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
            "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc", "psr"
        };
        int i;
        FILE *out = stdout;
        /* Write directly to a file when named.  FUZIX sh mis-executes a
         * redirected binary (treats the ELF as a script), so rc cannot use
         * `picoctl crashlog > file` -- let the tool open the file itself. */
        if (argc >= 3)
        {
            out = fopen(argv[2], "w");
            if (out == NULL)
            {
                perror("cannot create crashlog file");
                close(fd);
                return 1;
            }
        }
        if (ioctl(fd, PICOIOC_CRASHLOG, &r) != 0)
        {
            fputs("No crash record.\n", out);
            if (out != stdout)
                fclose(out);
            close(fd);
            return 0;   /* not an error: clean boot has none */
        }
        fprintf(out, "FLAGRANT SYSTEM ERROR! EXCEPTION %u\n", (unsigned)r.cause);
        for (i = 0; i < 17; i++)
            fprintf(out, "%s=%x\n", names[i], (unsigned)r.r[i]);
        fprintf(out, "PROGBASE=%x PROGLOAD=%x PROGTOP=%x\n",
                0x20008C98, 0x20009898, 0x20036498);
        fprintf(out, "user-rel lr=%x pc=%x\n",
                (unsigned)(r.r[14] - 0x20009898),
                (unsigned)(r.r[15] - 0x20009898));
        fputs("--- console tail before fault ---\n", out);
        for (i = 0; i < (int)sizeof(r.tail); i++) {
            unsigned char c = r.tail[i];
            if (c == 0)
                break;
            if ((c >= 32 && c < 127) || c == '\n' || c == '\r')
                fputc(c, out);
            else
                fputc('.', out);
        }
        fputs("\n--- end ---\n", out);
        if (out != stdout)
            fclose(out);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "vtshadow") == 0)
    {
        struct pico_vtshadow s;
        int y, x;
        if (ioctl(fd, PICOIOC_VTSHADOW, &s) != 0)
        {
            perror("Failed to read VT shadow");
            close(fd);
            return 1;
        }
        printf("CURSOR %u %u\n", (unsigned)s.cursorx, (unsigned)s.cursory);
        for (y = 0; y < 40; y++) {
            int last = 79;
            while (last >= 0 && s.shadow[y][last] == ' ')
                last--;
            if (last < 0)
                continue;   /* skip blank rows */
            printf("%02d|", y);
            for (x = 0; x <= last; x++)
                putchar(s.shadow[y][x] ? s.shadow[y][x] : ' ');
            putchar('\n');
        }
        puts("---");
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
