/* Intermediate-representation contract between the C front end and the
 * Thumb-1 back end.  Numeric values deliberately match amacc's opcode
 * numbering so the proven front end can be reused unchanged. */
#ifndef FCC_IR_H
#define FCC_IR_H

enum {
    LEA = 0, IMM, JMP, JSR, BZ, BNZ, ENT, ADJ, LEV,
    LI, LC, SI, SC, PSH,
    OR, XOR, AND, EQ, NE, LT, GT, LE, GE, SHL, SHR,
    ADD, SUB, MUL, DIV, MOD,
    SYSC = 30, CLCA = 31,
    DATAPTR = 32,      /* fcc extension: operand is a data-pool offset */
    UDIV = 33, UMOD, LSR,  /* unsigned divide/modulo, logical shift right */
    ULT = 40, UGT, ULE, UGE,  /* unsigned comparisons */
    SXT8 = 44, ZXT8, LCU,     /* sign/zero-extend to 8 bits; unsigned char load */
    SCPY = 47,                /* copy N words (operand) from src to dst */
    /* fcc function-pointer extensions.  Both opcodes are new; existing
     * opcode values are deliberately left unchanged. */
    CODEADDR = 48,            /* operand: IR word index of a function entry;
                               * emits a PC-relative address of its code */
    JSRI = 49,                /* indirect call: target address in r0, no operand */
    /* Single-precision soft-float opcodes.  Each emits a BL to an embedded
     * Thumb-1 helper; operands/results are raw IEEE-754 bit patterns in r0. */
    FADD = 220, FSUB, FMUL, FDIV,
    FLT, FGT, FLE, FGE, FEQ, FNE,
    F2I, I2F,
    /* Double precision: values are addresses of 8-byte slots. */
    DADD = 232, DSUB, DMUL, DDIV,
    DLT, DGT, DLE, DGE, DEQ, DNE,
    D2I, I2D, F2D, D2F, DNEG,
    PUSHS = 248,      /* push N words from the address in r0 */
    IR_INVALID
};

/* Operand layout (each IR slot is one int):
 *   LEA,IMM,JMP,JSR,BZ,BNZ,ENT,ADJ : opcode, operand
 *   CODEADDR                        : CODEADDR, IR word index (relocated)
 *   JSRI                            : opcode only (target address in r0)
 *   SYSC                            : SYSC, ef-index, ADJ, count
 *   all others                      : opcode only
 * Jump/branch operands are absolute pointers into the IR array (amacc
 * convention); the back end converts them to byte offsets.
 */
#endif
