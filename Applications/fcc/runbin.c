/*
 * runbin - execute a raw ARM Thumb-1 binary from a file.
 *
 * FUZIX on the RP2040 runs user code directly in RAM with no MPU and no
 * caches (CONFIG_USERMEM_DIRECT).  A file of raw Thumb-1 machine code can
 * therefore be read into a malloc'd buffer and called as a function.  This
 * is the loader half of the self-hosted compiler toolchain: the compiler
 * emits flat Thumb-1 binaries (no ELF, no relocations) that this program
 * loads from the filesystem (e.g. the SD card at /mnt/sd).
 *
 * The loaded binary is called with the standard ARM AAPCS: r0-r3 are
 * arguments (none for now), the result is returned in r0.  A Thumb function
 * pointer must have bit 0 set, which is how the CPU is told to decode the
 * target as 16-bit Thumb instructions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

/* Keep well inside a FUZIX process (PROGSIZE ~= 64000 bytes). */
#define MAXCODE (56 * 1024)

typedef int (*thumb_fn)(void);

int main(int argc, char **argv)
{
    int fd, n, total = 0;
    unsigned char *buf;
    int rc;

    if (argc != 2) {
        printf("usage: runbin <file.bin>\n");
        return 1;
    }
    fd = open(argv[1], 0);
    if (fd < 0) {
        printf("runbin: cannot open %s\n", argv[1]);
        return 1;
    }
    /* Thumb's PC-relative ADD aligns PC to 4 bytes, so the load address must
     * be 4-byte aligned or data references land two bytes off. */
    buf = (unsigned char *) malloc(MAXCODE + 4);
    if (buf != NULL)
        buf = (unsigned char *)(((unsigned long) buf + 3) & ~3UL);
    if (buf == NULL) {
        printf("runbin: out of memory\n");
        close(fd);
        return 1;
    }
    while ((n = read(fd, buf + total, MAXCODE - total)) > 0)
        total += n;
    close(fd);
    if (total <= 0) {
        printf("runbin: empty file\n");
        return 1;
    }

    /* No MPU / caches on the Cortex-M0+: the heap is executable.  Bit 0 of
       a Thumb function pointer selects Thumb state and is cleared by blx. */
    rc = ((thumb_fn) ((unsigned long) buf | 1UL))();
    printf("%d\n", rc);
    return 0;
}
