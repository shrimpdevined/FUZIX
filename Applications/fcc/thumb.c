/* thumb.c - Thumb-1 (ARMv6-M / Cortex-M0+) back end for the fcc compiler.
 *
 * Consumes amacc's stack-machine IR (see ir.h) and emits a flat, position-
 * independent Thumb-1 binary.  No ELF, no relocations, no absolute addresses:
 * every branch is PC-relative, so the binary can be loaded anywhere in RAM.
 *
 * Register contract (AAPCS-compatible so emitted code can be called from C):
 *   r0   accumulator ("ax" in the IR semantics)
 *   r1   scratch (popped operand for binary operations)
 *   r2   scratch (shift amount)
 *   r7   frame pointer
 *   r13  stack pointer (also the IR value stack, as in amacc)
 *   r14  link register
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"
#include "fp_helpers.h"

#define CODE_MAX  (12 * 1024)
#define HELPER_TARGET  (-1)   /* branch sentinel: signed division helper */
#define UHELPER_TARGET (-2)   /* branch sentinel: unsigned division helper */
/* FP helper sentinels: target = FPHELPER_FIRST - (opcode - FADD). */
#define FPHELPER_FIRST (-3)

/* Offset of each helper inside fp_helpers[], indexed by opcode - FADD. */
static const int fp_helper_offsets[] = {
    FP_FADD, FP_FSUB, FP_FMUL, FP_FDIV,
    FP_FLT, FP_FGT, FP_FLE, FP_FGE, FP_FEQ, FP_FNE,
    FP_F2I, FP_I2F,
    FP_DADD, FP_DSUB, FP_DMUL, FP_DDIV,
    FP_DLT, FP_DGT, FP_DLE, FP_DGE, FP_DEQ, FP_DNE,
    FP_D2I, FP_I2D, FP_F2D, FP_D2F, FP_DNEG
};
#define FP_HELPER_COUNT ((int)(sizeof(fp_helper_offsets) / sizeof(fp_helper_offsets[0])))

/* Software division helpers (arm-none-eabi-as -mcpu=cortex-m0plus, divhelp.S).
 * udiv_helper sits at offset 0 (UDIV_HELPER_SIZE bytes); div_helper follows it.
 * On entry r0=divisor, r1=dividend; on return r0=quotient, r1=remainder.
 * All branches are PC-relative, so this is position-independent.
 *
 * udiv_helper is a 32-step restoring divider: it cannot hang for any input,
 * including a zero divisor (returns quotient 0).
 * div_helper converts operands to magnitudes, calls udiv_helper, then applies
 * C90 sign rules: quotient truncates toward zero, remainder takes the sign of
 * the dividend (e.g. -17 % 5 == -2).  INT_MIN / -1 is handled without
 * overflowing the magnitude loop. */
static const unsigned char div_helpers[] = {
    0x70, 0xb5, 0x00, 0x26, 0x04, 0x46, 0x00, 0x2c, 0x01, 0xd1, 0x0d, 0x46,
    0x0a, 0xe0, 0x00, 0x25, 0x20, 0x23, 0x49, 0x00, 0x6d, 0x41, 0x76, 0x00,
    0xa5, 0x42, 0x01, 0xd3, 0x2d, 0x1b, 0x01, 0x36, 0x01, 0x3b, 0xf6, 0xd1,
    0x30, 0x46, 0x29, 0x46, 0x70, 0xbd, 0x70, 0xb5, 0x04, 0x46, 0x0d, 0x46,
    0x00, 0x23, 0x00, 0x2d, 0x01, 0xda, 0x6d, 0x42, 0x01, 0x23, 0x00, 0x26,
    0x00, 0x2c, 0x01, 0xda, 0x64, 0x42, 0x01, 0x26, 0x20, 0x46, 0x29, 0x46,
    0x1c, 0x46, 0xff, 0xf7, 0xd9, 0xff, 0xb4, 0x42, 0x00, 0xd0, 0x40, 0x42,
    0x00, 0x2c, 0x00, 0xd0, 0x49, 0x42, 0x70, 0xbd,
};
#define UDIV_HELPER_SIZE 42
#define DIV_HELPERS_SIZE ((int)(sizeof(div_helpers) / sizeof(div_helpers[0])))

typedef struct {
    int reg;               /* destination register of the ldr literal */
    int off;               /* byte offset of the ldr in the output */
    int lit;               /* index into the literal array */
} LitPatch;

typedef struct {
    int off;               /* byte offset of the branch instruction */
    int target;            /* IR word index of the target */
    int kind;              /* JMP, JSR, BZ, BNZ */
} BrPatch;

/* A code address in a literal pool: the stored word is the PC-relative
 * delta from `mov r0,pc` to the target function's first instruction, so a
 * function pointer is just an absolute runtime address with no relocation
 * tables (the whole image stays position-independent). */
typedef struct {
    int lit;               /* index into e->lit[] */
    int target;            /* IR word index of the target function entry */
    int pcbase;            /* PC base of the `mov r0,pc` instruction */
    int wordoff;           /* byte offset of the literal word (set at flush) */
} CodeLit;

/* A data address in the literal pool.  The pool is flushed near the code,
 * so `ldr r1,[pc,#imm]` stays in range even for large programs; the stored
 * word is the PC-relative delta to the final data section, making the binary
 * position-independent (runbin mallocs the load buffer at a random address). */
typedef struct {
    int lit;               /* index into e->lit[] */
    int dataoff;           /* offset into the data section */
    int pcbase;            /* PC base of the `mov r0,pc` instruction */
    int wordoff;           /* byte offset of the literal word (set at flush) */
} DataLit;

typedef struct {
    unsigned char *buf;           /* points at caller-supplied output */
    int len;
    int cap;
    int *irmap;            /* IR word index -> byte offset in buf */
    int irwords;
    /* Patch tables.  They all grow on demand: the number of branches and of
     * data/function references scales with program size, so a fixed ceiling
     * made larger programs fail with a bogus "unresolved branch target" or
     * "too many data references".  Capacity is always a power of two. */
    unsigned int *lit;             /* literal words */
    int nlit, litcap;
    LitPatch *lp;                  /* ldr -> literal patches in current pool */
    int nlp, lpcap;
    BrPatch *bp;                   /* branch patches, whole program */
    int nbp, bpcap;
    DataLit *dl;                   /* data-address literals, whole program */
    int ndl, dlcap;
    CodeLit *cl;                   /* code-address literals, whole program */
    int ncl, clcap;
    unsigned char *data;
    int datasz;
    int helper_off;
    int fp_off;                    /* base offset of fp_helpers[] in buf */
    char err[128];
} Emit;

static void fail(Emit *e, const char *msg)
{
    if (!e->err[0])
        snprintf(e->err, sizeof(e->err), "%s", msg);
}

/* Grow a patch table so it can hold at least `need` entries.  Returns 0 on
 * success and -1 on allocation failure (with e->err set). */
static int grow_tbl(Emit *e, void **p, int *cap, int need, unsigned int elemsz)
{
    int ncap;
    void *q;
    if (need <= *cap)
        return 0;
    ncap = *cap ? *cap : 32;
    while (ncap < need)
        ncap *= 2;
    q = realloc(*p, (size_t)ncap * elemsz);
    if (!q) {
        fail(e, "out of memory (patch table)");
        return -1;
    }
    *p = q;
    *cap = ncap;
    return 0;
}

static void h(Emit *e, unsigned short w)
{
    if (e->len + 2 > e->cap) { fail(e, "code buffer overflow"); return; }
    e->buf[e->len++] = (unsigned char)(w & 0xff);
    e->buf[e->len++] = (unsigned char)(w >> 8);
}

/* Emit "ldr r<reg>, [pc, #0]" and remember it for pool patching. */
static void ldr_lit(Emit *e, int reg, unsigned int value)
{
    if (grow_tbl(e, (void **)&e->lit, &e->litcap, e->nlit + 1, sizeof(*e->lit)))
        return;
    if (grow_tbl(e, (void **)&e->lp, &e->lpcap, e->nlp + 1, sizeof(*e->lp)))
        return;
    unsigned short insn = (unsigned short)(0x4800 | ((reg & 7) << 8));
    e->lp[e->nlp].reg = reg;
    e->lp[e->nlp].off = e->len;
    e->lp[e->nlp].lit = e->nlit;
    e->nlp++;
    e->lit[e->nlit++] = value;
    h(e, insn);
}

static void flush_pool(Emit *e);

/* Emit a position-independent data-pool address into r0:
 *   ldr r1,[pc,#lit]   -> r1 = data_base + dataoff - pcbase  (patched later)
 *   mov r0,pc          -> r0 = aligned PC of the next instruction
 *   add r0,r0,r1       -> r0 = data address
 * pcbase matches `mov r0,pc` semantics: (insn_addr + 4) & ~1.
 * Note: unlike LDR literal loads, MOV from PC does NOT align to 4 bytes. */
static void ldr_data_ptr(Emit *e, int dataoff)
{
    int previous = e->nlit;
    int mov_off;
    ldr_lit(e, 1, 0);                  /* placeholder word */
    if (e->nlit <= previous) return;   /* ldr_lit failed */
    if (grow_tbl(e, (void **)&e->dl, &e->dlcap, e->ndl + 1, sizeof(*e->dl)))
        return;
    e->dl[e->ndl].lit = previous;
    e->dl[e->ndl].dataoff = dataoff;
    e->dl[e->ndl].wordoff = -1;
    e->ndl++;
    /* Flush a pool RIGHT HERE so the ldr stays in range even when this is
     * far from the function-end pool.  flush_pool emits a branch over the
     * pool, so execution continues after it. */
    flush_pool(e);
    mov_off = e->len;
    h(e, 0x4678);                      /* mov r0, pc */
    h(e, 0x1808);                      /* add r0, r1, r0 */
    e->dl[e->ndl - 1].pcbase = (mov_off + 4) & ~1;
}

/* Emit a position-independent function address into r0.  The literal word
 * is filled in after the first pass, once the target IR index is mapped to
 * a byte offset in the output buffer.  Shares the pool machinery with
 * ldr_data_ptr() so the ldr stays within its 1 KB range. */
static void ldr_code_ptr(Emit *e, int target)
{
    int previous = e->nlit;
    int mov_off;
    ldr_lit(e, 1, 0);                  /* placeholder word */
    if (e->nlit <= previous) return;   /* ldr_lit failed */
    if (grow_tbl(e, (void **)&e->cl, &e->clcap, e->ncl + 1, sizeof(*e->cl)))
        return;
    e->cl[e->ncl].lit = previous;
    e->cl[e->ncl].target = target;
    e->cl[e->ncl].wordoff = -1;
    e->ncl++;
    flush_pool(e);
    mov_off = e->len;
    h(e, 0x4678);                      /* mov r0, pc */
    h(e, 0x1808);                      /* add r0, r1, r0 */
    e->cl[e->ncl - 1].pcbase = (mov_off + 4) & ~1;
}

/* Write a literal pool after the current code and patch all pending ldrs. */
static void flush_pool(Emit *e)
{
    if (e->nlit == 0)
        return;
    /* Branch over the pool to the next code. */
    /* b.n over the pool; patch its displacement once the pool size is known. */
    int boff = e->len;
    h(e, 0xE000);
    /* Align the pool base to 4 bytes. */
    if (e->len & 2)
        h(e, 0x0000);                 /* movs r0, r0 (harmless pad) */
    int pool_base = e->len;
    int i;
    for (i = 0; i < e->nlit; i++) {
        unsigned int v = e->lit[i];
        int j;
        for (j = 0; j < e->ndl; j++)
            if (e->dl[j].lit == i && e->dl[j].wordoff < 0)
                e->dl[j].wordoff = e->len;
        for (j = 0; j < e->ncl; j++)
            if (e->cl[j].lit == i && e->cl[j].wordoff < 0)
                e->cl[j].wordoff = e->len;
        h(e, (unsigned short)(v & 0xffff));
        h(e, (unsigned short)(v >> 16));
    }
    /* Patch the jump-over instruction: target is the current end of code. */
    {
        int disp = e->len - (boff + 4);
        unsigned short insn = (unsigned short)(0xE000 | ((disp / 2) & 0x7FF));
        e->buf[boff] = (unsigned char)(insn & 0xff);
        e->buf[boff + 1] = (unsigned char)(insn >> 8);
    }
    for (i = 0; i < e->nlp; i++) {
        LitPatch *p = &e->lp[i];
        int literal_addr = pool_base + p->lit * 4;
        /* Thumb literal loads compute PC as (insn_addr + 4) & ~3. */
        int pcbase = (p->off + 4) & ~3;
        int disp = literal_addr - pcbase;
        if (disp < 0 || disp > 1020 || (disp & 3)) {
            fail(e, "literal out of range");
            continue;
        }
        unsigned short insn = (unsigned short)(0x4800 |
                               ((p->reg & 7) << 8) | ((disp / 4) & 0xff));
        e->buf[p->off] = (unsigned char)(insn & 0xff);
        e->buf[p->off + 1] = (unsigned char)(insn >> 8);
    }
    e->nlit = 0;
    e->nlp = 0;
}

/* BL encoding (T1) for a signed byte displacement from (pc+4). */
static void emit_bl_at(Emit *e, int off, int disp)
{
    unsigned int imm32 = (unsigned int)disp;
    int S = (imm32 >> 24) & 1;
    int I1 = (imm32 >> 23) & 1;
    int I2 = (imm32 >> 22) & 1;
    int imm10 = (imm32 >> 12) & 0x3ff;
    int imm11 = (imm32 >> 1) & 0x7ff;
    int J1 = ((~I1) ^ S) & 1;
    int J2 = ((~I2) ^ S) & 1;
    unsigned short w1 = (unsigned short)(0xF000 | (S << 10) | imm10);
    unsigned short w2 = (unsigned short)(0xD000 | (J1 << 13) | (J2 << 11) | imm11);
    e->buf[off]     = (unsigned char)(w1 & 0xff);
    e->buf[off + 1] = (unsigned char)(w1 >> 8);
    e->buf[off + 2] = (unsigned char)(w2 & 0xff);
    e->buf[off + 3] = (unsigned char)(w2 >> 8);
}

/* Condition codes for the comparison ops (EQ..GE). */
static int cmp_cond(int op)
{
    switch (op) {
    case EQ: return 0;   /* EQ */
    case NE: return 1;   /* NE */
    case LT: return 11;  /* LT */
    case GE: return 10;  /* GE */
    case GT: return 12;  /* GT */
    case LE: return 13;  /* LE */
    case 40: return 3;   /* ULT -> CC (carry clear = unsigned <) */
    case 41: return 8;   /* UGT -> HI */
    case 42: return 9;   /* ULE -> LS */
    case 43: return 2;   /* UGE -> CS */
    }
    return 14;
}

int thumb_compile(int *text, int *end, unsigned char *out, int outcap,
                  unsigned char *data, int datasz, char *errbuf)
{
    /* Emit is ~16 KB: too large for the 4 KB process stack, and making it
     * static would inflate the ELF BSS past the exec size limit.  Allocate
     * it from the heap instead. */
    Emit *e = (Emit *) malloc(sizeof(Emit));
    int len, i0;
    if (!e) { if (errbuf) snprintf(errbuf, 128, "out of memory"); return -1; }
    memset(e, 0, sizeof(*e));
    e->buf = out;
    e->cap = outcap;
    e->data = data;
    e->datasz = datasz;
    if (errbuf) errbuf[0] = 0;

    int irwords = (int)(end - text);
    if (irwords <= 0 || irwords > 60000) { fail(e, "bad IR size"); goto done; }

    e->irmap = (int *) malloc((size_t)irwords * sizeof(int));
    if (!e->irmap) { fail(e, "out of memory"); goto done; }
    { int i; for (i = 0; i < irwords; i++) e->irmap[i] = -1; }
    e->irwords = irwords;

    /* ---- first pass ---- */
    {
        int *pc = text + 1;
        while (pc < end) {
            int i = (int)(pc - text);
            e->irmap[i] = e->len;
            int op = *pc++;
            int v;
            switch (op) {
            case LEA:
                v = (*pc++) * 4;
                h(e, 0x4638);                       /* mov r0, r7 */
                if (v >= 0 && v <= 255) {
                    h(e, (unsigned short)(0x3000 | v));     /* adds r0, #v */
                } else if (v < 0 && v >= -255) {
                    h(e, (unsigned short)(0x3800 | (-v)));  /* subs r0, #-v */
                } else {
                    /* Load the magnitude and add/sub it to r7.  For a
                     * negative offset r1 must hold -v (positive) so that
                     * `sub r0, r0, r1` yields r7 + v, not v - r7. */
                    ldr_lit(e, 1, (unsigned int)(v >= 0 ? v : -v));
                    h(e, v >= 0 ? 0x1840 : 0x1A40);         /* add/sub r0, r1 */
                }
                break;
            case IMM:
                v = *pc++;
                if (v >= 0 && v <= 255)
                    h(e, (unsigned short)(0x2000 | v));     /* movs r0, #v */
                else
                    ldr_lit(e, 0, (unsigned int)v);
                break;
            case JMP: case JSR: case BZ: case BNZ:
                v = *pc++;                                  /* IR word index */
                if (grow_tbl(e, (void **)&e->bp, &e->bpcap, e->nbp + 1,
                             sizeof(*e->bp))) break;
                e->bp[e->nbp].off = e->len;
                e->bp[e->nbp].target = v;
                e->bp[e->nbp].kind = op;
                e->nbp++;
                if (op == JSR) {
                    h(e, 0xF000); h(e, 0xD000);             /* bl placeholder */
                } else if (op == JMP) {
                    h(e, 0xE000);                           /* b.n placeholder */
                } else {
                    h(e, 0x2800);                           /* cmp r0, #0 */
                    h(e, (unsigned short)(op == BZ ? 0xD100 : 0xD000)); /* inverted skip */
                    /* The second pass patches the b.n, so record ITS offset,
                     * not the cmp's (patching the cmp would clobber it). */
                    e->bp[e->nbp - 1].off = e->len;
                    h(e, 0xE000);                           /* b.n placeholder */
                }
                break;
            case ENT:
                v = (*pc++) * 4;
                h(e, 0xB580);                               /* push {r7, lr} */
                h(e, 0x466F);                               /* mov r7, sp */
                while (v > 508) { h(e, 0xB0FF); v -= 508; } /* sub sp, #508 */
                if (v) h(e, (unsigned short)(0xB080 | (v / 4)));
                break;
            case ADJ:
                v = (*pc++) * 4;
                while (v > 508) { h(e, 0xB07F); v -= 508; } /* add sp, #508 */
                if (v) h(e, (unsigned short)(0xB000 | (v / 4)));
                break;
            case LEV:
                h(e, 0x46BD);                               /* mov sp, r7 */
                h(e, 0xBD80);                               /* pop {r7, pc} */
                flush_pool(e);
                break;
            case LI:  h(e, 0x6800); break;                  /* ldr r0, [r0] */
            /* Plain char is signed on ARM: zero-extending ldrb is followed by
             * sxtb so negative char values are correctly sign-extended. */
            case LC:  h(e, 0x7800); h(e, 0xB240); break;     /* ldrb; sxtb r0,r0 */
            case LCU: h(e, 0x7800); break;                  /* ldrb r0,[r0] (unsigned) */
            case SXT8: h(e, 0xB240); break;                  /* sxtb r0,r0 */
            case ZXT8: h(e, 0xB2C0); break;                  /* uxtb r0,r0 */
            /* Struct value copy: r0 = source address, pop destination into
             * r1, then copy the requested number of words.  Unrolled to keep
             * the encoder simple (word count is bounded by the front end). */
            case SCPY: {
                int count = *pc++;
                int w;
                h(e, 0xBC02);                               /* pop {r1} = dst */
                for (w = 0; w < count; w++) {
                    h(e, 0x6802);                           /* ldr r2, [r0] */
                    h(e, 0x600A);                           /* str r2, [r1] */
                    if (w + 1 < count) {
                        h(e, 0x3004);                       /* adds r0, #4 */
                        h(e, 0x3104);                       /* adds r1, #4 */
                    }
                }
                break;
            }
            case SI:  h(e, 0xBC02); h(e, 0x6008); break;     /* pop r1; str r0,[r1] */
            case SC:  h(e, 0xBC02); h(e, 0x7008); break;     /* pop r1; strb r0,[r1] */
            case PSH: h(e, 0xB401); break;                  /* push {r0} */
            case PUSHS: {
                /* r0 = struct address; push its N words in order so word 0
                 * lands at the highest address (callee's first slot). */
                int n = *pc++;
                int i, off;
                if (n < 1 || n > 64) { fail(e, "bad PUSHS count"); break; }
                /* Push from the last word down so word 0 lands at the
                 * lowest callee slot; member offsets then increase upward. */
                for (i = n - 1; i >= 0; i--) {
                    off = i * 4;
                    if (off <= 255) h(e, (unsigned short)(0x6801 | ((off / 4) << 6)));
                    else { ldr_lit(e, 2, (unsigned int)off);
                           h(e, 0x1840);                    /* add r0,r0,r2 */
                           h(e, 0x6801); }                  /* ldr r1,[r0] */
                    h(e, 0xB402);                           /* push {r1} */
                    if (off <= 255) { /* r0 unchanged for small offsets */ }
                }
                break;
            }
            case OR:  h(e, 0xBC02); h(e, 0x4308); break;     /* pop r1; orrs */
            case XOR: h(e, 0xBC02); h(e, 0x4048); break;     /* pop r1; eors */
            case AND: h(e, 0xBC02); h(e, 0x4008); break;     /* pop r1; ands */
            case ADD: h(e, 0xBC02); h(e, 0x1840); break;     /* pop r1; adds r0,r0,r1 */
            case SUB: h(e, 0xBC02); h(e, 0x1A08); break;     /* pop r1; subs r0,r1,r0 */
            case MUL: h(e, 0xBC02); h(e, 0x4348); break;     /* pop r1; muls */
            case SHL:                                        /* pop r1; r0 = r1 << r0 */
                h(e, 0xBC02); h(e, 0x1C02); h(e, 0x1C08); h(e, 0x4090); break;
            case SHR:
                h(e, 0xBC02); h(e, 0x1C02); h(e, 0x1C08); h(e, 0x4110); break;
            case LSR:
                /* Logical (unsigned) right shift: pop r1, r0 = r1 >> r0. */
                h(e, 0xBC02); h(e, 0x1C02); h(e, 0x1C08); h(e, 0x40D0); break;
            case DIV: case MOD: case UDIV: case UMOD:
                /* r0 = divisor (right operand); pop dividend into r1 and
                 * call the appropriate software division helper with
                 * r0=div, r1=dividend.  Returns quotient in r0, remainder
                 * in r1.  Signed ops use div_helper (C90 sign rules),
                 * unsigned ops use udiv_helper directly. */
                if (grow_tbl(e, (void **)&e->bp, &e->bpcap, e->nbp + 1,
                             sizeof(*e->bp))) break;
                h(e, 0xBC02);                       /* pop {r1} = dividend */
                e->bp[e->nbp].off = e->len;
                e->bp[e->nbp].target = (op == UDIV || op == UMOD)
                                     ? UHELPER_TARGET : HELPER_TARGET;
                e->bp[e->nbp].kind = JSR;
                e->nbp++;
                h(e, 0xF000); h(e, 0xD000);         /* bl helper placeholder */
                if (op == MOD || op == UMOD)
                    h(e, 0x4608);                   /* mov r0, r1 = remainder */
                break;
            case FADD: case FSUB: case FMUL: case FDIV:
            case FLT: case FGT: case FLE: case FGE: case FEQ: case FNE:
            case F2I: case I2F:
            case DADD: case DSUB: case DMUL: case DDIV:
            case DLT: case DGT: case DLE: case DGE: case DEQ: case DNE:
            case D2I: case I2D: case F2D: case D2F: case DNEG: {
                /* Float: r0 = right, pop left into r1.
                 * Double binary: r0 = dst (LEA emitted by gen), pop right
                 * into r2 and left into r1; helpers take (dst, left, right).
                 * Unary conversions: r0 = operand address or value. */
                if (op >= FADD && op <= FNE)
                    h(e, 0xBC02);                   /* pop {r1} = left */
                else if (op >= DADD && op <= DNE) {
                    h(e, 0xBC04);                   /* pop {r2} = right addr */
                    h(e, 0xBC02);                   /* pop {r1} = left addr */
                } else if (op == I2D || op == F2D || op == DNEG) {
                    h(e, 0xBC02);                   /* pop {r1} = value/addr */
                }
                if (grow_tbl(e, (void **)&e->bp, &e->bpcap, e->nbp + 1,
                             sizeof(*e->bp))) break;
                e->bp[e->nbp].off = e->len;
                e->bp[e->nbp].target = FPHELPER_FIRST - (op - FADD);
                e->bp[e->nbp].kind = JSR;
                e->nbp++;
                h(e, 0xF000); h(e, 0xD000);         /* bl helper placeholder */
                break;
            }
            case SYSC: {
                int num = *pc++;                            /* syscall number */
                if (*pc != ADJ) { fail(e, "SYSC without ADJ"); break; }
                pc++;
                int n = *pc++;                              /* argument count */
                if (n < 0 || n > 4) { fail(e, "bad SYSC arg count"); break; }
                int r;
                for (r = n - 1; r >= 0; r--)
                    h(e, (unsigned short)(0xBC00 | (1 << r)));  /* pop {rN} */
                h(e, (unsigned short)(0xDF00 | (num & 0xff)));  /* svc #num */
                break;
            }
            case DATAPTR:
                ldr_data_ptr(e, *pc++);
                break;
            case CODEADDR:
                /* Operand is the IR word index of a function entry. */
                ldr_code_ptr(e, *pc++);
                break;
            case JSRI:
                /* Indirect call: target is already in r0.  BLX selects Thumb
                 * state from bit 0 of the target and (unlike `mov lr,pc`)
                 * stores the return address with bit 0 set, so the callee's
                 * `pop {pc}` stays in Thumb state. */
                h(e, 0x3001);                       /* adds r0, #1  (Thumb bit) */
                h(e, 0x4780);                       /* blx r0 */
                break;
            case CLCA:
                break;                                      /* no caches on M0+ */
            default:
                if ((op >= EQ && op <= GE) || (op >= 40 && op <= 43)) {
                    int cond = cmp_cond(op);
                    int here = e->len;
                    h(e, 0xBC02);                           /* pop r1 */
                    h(e, 0x4281);                           /* cmp r1, r0 */
                    /* b<cond> L1 ; movs r0,#0 ; b.n L2 ; L1: movs r0,#1 ; L2: */
                    h(e, (unsigned short)(0xD000 | (cond << 8) | 1));
                    h(e, 0x2000);
                    h(e, 0xE000);
                    h(e, 0x2001);
                    e->buf[here + 4] = (unsigned char)((0xD000 | (cond << 8) | 1) & 0xff);
                    e->buf[here + 5] = (unsigned char)((0xD000 | (cond << 8) | 1) >> 8);
                    /* b.n at here+6 targets here+8: disp = 0 -> 0xE000 */
                } else {
                    fail(e, "unknown IR opcode");
                }
                break;
            }
            if (e->err[0]) break;
        }
        if (!e->err[0])
            flush_pool(e);
    }

    /* ---- patch function-address literals (targets are known after pass 1) ---- */
    if (!e->err[0]) {
        int k;
        for (k = 0; k < e->ncl; k++) {
            CodeLit *c = &e->cl[k];
            int target, disp;
            unsigned int v;
            if (c->target < 0 || c->target >= irwords ||
                e->irmap[c->target] < 0) {
                fail(e, "unresolved code address");
                continue;
            }
            target = e->irmap[c->target];
            if (c->wordoff < 0 || c->wordoff + 4 > e->len) {
                fail(e, "code address literal out of range");
                continue;
            }
            disp = target - c->pcbase;
            v = (unsigned int) disp;
            e->buf[c->wordoff] = (unsigned char)(v & 0xff);
            e->buf[c->wordoff + 1] = (unsigned char)((v >> 8) & 0xff);
            e->buf[c->wordoff + 2] = (unsigned char)((v >> 16) & 0xff);
            e->buf[c->wordoff + 3] = (unsigned char)((v >> 24) & 0xff);
        }
    }

    /* ---- append the data section (strings/globals) and patch DATAPTRs ---- */
    if (!e->err[0] && e->datasz > 0) {
        while (e->len & 3) {
            if (e->len >= e->cap) { fail(e, "output overflow (pad)"); break; }
            e->buf[e->len++] = 0;
        }
        int data_base = e->len;
        int k;
        for (k = 0; k < e->datasz; k++) {
            if (e->len >= e->cap) { fail(e, "output overflow (data)"); break; }
            e->buf[e->len++] = e->data[k];
        }
        for (k = 0; k < e->ndl; k++) {
            DataLit *d = &e->dl[k];
            int disp = data_base + d->dataoff - d->pcbase;
            unsigned int v;
            /* The displacement may be odd: char-sized data objects (e.g.
             * &global_char_array[1]) legitimately live at odd byte offsets.
             * `mov r0,pc` produces an even PC value and the literal is added
             * to it verbatim, so an odd displacement yields the correct odd
             * byte address.  Only the literal slot bounds must be checked. */
            if (d->wordoff < 0 || d->wordoff + 4 > e->len) {
                fail(e, "data reference out of range");
                continue;
            }
            v = (unsigned int) disp;
            e->buf[d->wordoff] = (unsigned char)(v & 0xff);
            e->buf[d->wordoff + 1] = (unsigned char)((v >> 8) & 0xff);
            e->buf[d->wordoff + 2] = (unsigned char)((v >> 16) & 0xff);
            e->buf[d->wordoff + 3] = (unsigned char)((v >> 24) & 0xff);
        }
    }

    /* ---- append the division helpers and record their base offset ---- */
    if (!e->err[0]) {
        while (e->len & 3) {
            if (e->len >= e->cap) { fail(e, "output overflow (helper pad)"); break; }
            e->buf[e->len++] = 0;
        }
        e->helper_off = e->len;             /* udiv_helper starts here */
        for (i0 = 0; i0 < DIV_HELPERS_SIZE; i0++) {
            if (e->len >= e->cap) { fail(e, "output overflow (helper)"); break; }
            e->buf[e->len++] = div_helpers[i0];
        }
        e->fp_off = e->len;                 /* fp_helpers[] starts here */
        for (i0 = 0; i0 < FP_HELPERS_SIZE; i0++) {
            if (e->len >= e->cap) { fail(e, "output overflow (fp helper)"); break; }
            e->buf[e->len++] = fp_helpers[i0];
        }
    }

    /* ---- second pass: patch branches ---- */
    if (!e->err[0]) {
        int k;
        for (k = 0; k < e->nbp; k++) {
            BrPatch *p = &e->bp[k];
            int target;
            if (p->target <= FPHELPER_FIRST &&
                p->target > FPHELPER_FIRST - FP_HELPER_COUNT) {
                int idx = FPHELPER_FIRST - p->target;
                target = e->fp_off + fp_helper_offsets[idx];
            } else
            target = (p->target == HELPER_TARGET)
                       ? e->helper_off + UDIV_HELPER_SIZE
                       : (p->target == UHELPER_TARGET)
                       ? e->helper_off
                       : ((p->target >= 0 && p->target < irwords)
                          ? e->irmap[p->target] : -1);
            if (target < 0) {
                char m[96];
                snprintf(m, sizeof(m), "unresolved branch target %d (of %d, irmap=%d)",
                         p->target, irwords,
                         (p->target >= 0 && p->target < irwords) ? e->irmap[p->target] : -2);
                fail(e, m);
                break;
            }
            int disp = target - (p->off + 4);
            if (p->kind == JSR) {
                emit_bl_at(e, p->off, disp);
            } else {
                int imm = disp / 2;
                if (disp < -2048 || disp > 2046 || (disp & 1)) {
                    fail(e, "branch out of range");
                    break;
                }
                unsigned short insn = (unsigned short)(0xE000 | (imm & 0x7FF));
                e->buf[p->off] = (unsigned char)(insn & 0xff);
                e->buf[p->off + 1] = (unsigned char)(insn >> 8);
            }
        }
    }

done:
    if (e->err[0] || e->len > outcap) {
        if (errbuf)
            snprintf(errbuf, 128, "%s", e->err[0] ? e->err : "output overflow");
        free(e->irmap);
        free(e->lit); free(e->lp); free(e->bp);
        free(e->dl);  free(e->cl);
        free(e);
        return -1;
    }
    len = e->len;
    free(e->irmap);
    free(e->lit); free(e->lp); free(e->bp);
    free(e->dl);  free(e->cl);
    free(e);
    return len;
}
