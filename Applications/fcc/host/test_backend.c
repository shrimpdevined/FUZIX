/* Host-side test driver for the Thumb-1 back end.
 * Builds small IR programs, compiles them with thumb_compile(), and writes
 * flat binaries that thumbrun.py verifies with Unicorn.
 * IR follows amacc's convention: text[0] is a dummy, code starts at text[1]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ir.h"

int thumb_compile(int *text, int *e, unsigned char *out, int outcap, unsigned char *data, int datasz, char *errbuf);

static int ir[256];
static int n;

static void begin(void) { ir[0] = 0; n = 1; }
static void put(int v) { ir[n++] = v; }
static void at(int idx, int v) { ir[idx] = v; }

static void emit(const char *name)
{
    unsigned char out[32768];
    char err[128] = "";
    int len = thumb_compile(ir, ir + n, out, sizeof(out), 0, 0, err);
    if (len < 0) {
        fprintf(stderr, "%s: COMPILE FAILED: %s\n", name, err);
        exit(1);
    }
    char path[256];
    snprintf(path, sizeof(path), "tests/%s.bin", name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(out, 1, len, f);
    fclose(f);
    printf("%-16s %3d bytes\n", name, len);
}

int main(void)
{
    /* return 42 */
    begin(); put(ENT); put(0); put(IMM); put(42); put(LEV);
    emit("ret42");

    /* 7 + 3*4 = 19 */
    begin(); put(ENT); put(0);
    put(IMM); put(7); put(PSH);
    put(IMM); put(3); put(PSH);
    put(IMM); put(4); put(MUL);
    put(ADD); put(LEV);
    emit("arith");

    /* 10 - 3 = 7 */
    begin(); put(ENT); put(0);
    put(IMM); put(10); put(PSH);
    put(IMM); put(3); put(SUB);
    put(LEV);
    emit("sub");

    /* local variable: x=5; return x+1 = 6 */
    begin(); put(ENT); put(1);
    put(LEA); put(-1); put(PSH);
    put(IMM); put(5); put(SI);
    put(LEA); put(-1); put(LI); put(PSH);
    put(IMM); put(1); put(ADD);
    put(LEV);
    emit("local");

    /* comparisons */
    begin(); put(ENT); put(0); put(IMM); put(5); put(PSH);
    put(IMM); put(3); put(LT); put(LEV);
    emit("cmp_false");
    begin(); put(ENT); put(0); put(IMM); put(3); put(PSH);
    put(IMM); put(5); put(LT); put(LEV);
    emit("cmp_true");
    begin(); put(ENT); put(0); put(IMM); put(5); put(PSH);
    put(IMM); put(5); put(EQ); put(LEV);
    emit("cmp_eq");

    /* big immediate needs a literal pool */
    begin(); put(ENT); put(0); put(IMM); put(1000); put(LEV);
    emit("literal");

    /* function call: main() { return g(); } g() { return 9; }
       layout: [dummy] ENT 0 | JSR &g | LEV | ENT 0 | IMM 9 | LEV */
    begin();
    put(ENT); put(0);
    int jmpidx = n; put(JSR); put(0); /* patch later */
    put(LEV);
    int gidx = n;
    at(jmpidx + 1, gidx);
    put(ENT); put(0); put(IMM); put(9); put(LEV);
    emit("call");

    /* conditional branch: if (0) return 1; else return 2; */
    begin();
    put(ENT); put(0);
    put(IMM); put(0);
    int bzidx = n; put(BZ); put(0);
    put(IMM); put(1);
    int jmpidx2 = n; put(JMP); put(0);
    int elseidx = n;
    at(bzidx + 1, elseidx);
    put(IMM); put(2);
    int endidx = n;
    at(jmpidx2 + 1, endidx);
    put(LEV);
    emit("branch");

    /* exit syscall: _exit(7) */
    begin(); put(ENT); put(0);
    put(IMM); put(7);
    put(SYSC); put(0); put(ADJ); put(0);
    put(LEV);
    emit("exit7");

    /* shifts: 1 << 4 = 16 */
    begin(); put(ENT); put(0);
    put(IMM); put(1); put(PSH);
    put(IMM); put(4); put(SHL);
    put(LEV);
    emit("shift");

    return 0;
}
