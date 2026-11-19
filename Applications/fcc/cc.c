/* cc.c - C90 compiler driver for FUZIX.
 *
 * Runs cpp to preprocess, then replaces itself with fcc to compile.  Using
 * exec (rather than a shell script) keeps only two processes resident during
 * compilation, which matters in the Pico's 128 KB user pool.
 *
 * Usage: cc source.c -o output.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* Keep all compiler temporaries on the SD card.  The root filesystem is a
 * small (2272 block) flash volume and must not absorb compiler artifacts;
 * creation and truncation on FAT are now handled correctly by devfat. */
#define TMP "/mnt/sd/.cc.tmp.i"

int main(int argc, char **argv)
{
    pid_t pid;
    int status;

    if (argc < 4) {
        write(2, "usage: cc source.c -o output.bin\n", 33);
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        write(2, "cc: fork failed\n", 16);
        return 1;
    }
    if (pid == 0) {
        /* The compiler lives in the optional SD application store, so
         * resolve it through PATH rather than assuming /bin. */
        execlp("cpp", "cpp", argv[1], "-o", TMP, (char *)0);
        _exit(1);
    }
    wait(&status);
    if (status != 0) {
        unlink(TMP);
        return 1;
    }

    /* Replace this process with the compiler: no extra shell process. */
    execlp("fcc", "fcc", TMP, "-o", argv[3], (char *)0);
    _exit(1);
    return 1;                       /* unreachable; keeps gnu89 happy */
}
