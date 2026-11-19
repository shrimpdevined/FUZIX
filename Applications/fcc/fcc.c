/*
 * AMaCC is capable of compiling (subset of) C source files into GNU/Linux
 * executables or running via just-in-time compilation on 32-bit ARM
 * processor-based platforms. There is no preprocessor.
 *
 * The following options are supported:
 *   -s : Print source and generated intermediate representation (IR).
 *   -o : Create executable file and terminate normally.
 *
 * If -o and -s are omitted, the compiled code is executed immediately (if
 * there were no compile errors) with the command line arguments passed
 * after the source file parameter.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include "fuzix_shim.h"

/* 64-bit host support */
#if defined(__x86_64__) || defined(__aarch64__)
#define int long
#endif

char *freep, *p, *lp; // current position in source code
char *freedata, *data, *_data;   // data/bss pointer

int *e, *le, *text;  // current position in emitted code
int *cas;            // case statement patch-up pointer
int *def;            // default statement patch-up pointer
int *brks;           // break statement patch-up pointer
int *cnts;           // continue statement patch-up pointer
int  swtc;           // !0 -> in a switch-stmt context
int  brkc;           // !0 -> in a break-stmt context
int  cntc;           // !0 -> in a continue-stmt context
int *tsize;          // array (indexed by type) of type sizes
int tnew;            // next available type
int tk;              // current token
int ival;            // current token value
/* Floating constants are stored here; `ival` holds the table index. */
#define FNUM_MAX 256
#define FNUM_IDX(v) ((v) & 0x3fff)
#define FNUM_ISFLOAT(v) ((v) & 0x8000)
#define FNUM_ISDOUBLE(v) ((v) & 0x4000)
double fnums[FNUM_MAX];
int nfnums;
/* Byte size (including the NUL terminator) of the most recently lexed wide
 * string literal.  expr() consumes it to build the array type for L"...". */
static int wide_str_size;
static void fp_emit_bin(int *b, int op, int lty, int rty);
static void dbl_emit_bin(int *b, int op, int lty, int rty);
void expr(int lev);

/* Forward function calls: a prototype registers the name with val == -1;
 * the call AST carries a negative fixup index instead of a stale address.
 * Once every definition has been parsed the IR is walked and every negative
 * JSR/CODEADDR operand is patched before branch targets are rewritten to IR
 * indices.  No per-site slot list is needed: the fixup index lives in the
 * operand word, so one index can serve any number of call sites. */
#define FWD_CALL_MAX 64
#define MAX_CALL_ARGS 64
static struct ident_s *fwd_ident[FWD_CALL_MAX];
/* External-function fixups: fwd_efidx[i] is the external cache index of a
 * syscall candidate (>= 0) or -1 when the name is not a syscall.  At
 * resolution time a candidate with no user definition becomes a real svc,
 * while a non-syscall with no definition becomes a clear-failing stub. */
static int fwd_efidx[FWD_CALL_MAX];
static int nfwd_calls;

/* Pool bounds, initialised once in main().  Every pool write is checked so
 * an overly large source fails with a clear diagnostic instead of silently
 * corrupting adjacent memory (CODING_STANDARD rule 6). */
static int *ir_limit;               /* one past the last valid IR word */
static int *ast_floor;              /* lowest valid AST word (pool grows down) */
static char *data_limit;            /* one past the last valid data byte */
static struct ident_s *sym_limit;   /* one past the last valid symbol slot */

/* Bounds helpers; defined after fatal() so diagnostics use the normal path. */
static void need_ir(int count);
static void need_ast(int count);
static void need_data(int count);

int ty;              // current expression type
int compound;        // handle precedence of compound assignment
int loc;             // local variable offset
int line;            // current line number
int warnings;        // count of non-fatal diagnostics emitted
int src;             // print source and assembly flag
int signed_char;     // use `signed char` for `char`
int elf;             // print ELF format
int *n;              // current position in emitted abstract syntax tree
                     // With an AST, the compiler is not limited to generate
                     // code on the fly with parsing.
                     // This capability allows function parameter code to be
                     // emitted and pushed on the stack in the proper
                     // right-to-left order.
int ld;              // local variable depth

/* Struct return-by-value: the callee receives a hidden destination pointer
 * as its last parameter; `return` copies the result words there with SCPY. */
int retstruct;       // current function returns a struct by value
int retwords;        // number of words in that struct
int retlea;          // LEA operand reaching the hidden pointer parameter
int retvoid;         // current function has a void return type

/* Defined below, after stmt(); used by the K&R parameter path in stmt(). */
static void kr_parameters(void);

// identifier
/* Usage tracking for unused-variable warnings lives in a parallel static
 * array, not in struct ident_s: keeping the struct layout byte-for-byte
 * identical guarantees diagnostics cannot affect generated code. */
#define SYM_USED_MAX 128
static unsigned char sym_used[SYM_USED_MAX];

struct ident_s {
    int tk;          // type-id or keyword
    int hash;
    char *name;     // name of this identifier
    /* fields starting with 'h' were designed to save and restore
     * the global class/type/val in order to handle the case if a
     * function declares a local with the same name as a global.
     */
    int class, hclass; // FUNC, GLO (global var), LOC (local var), Syscall
    int type, htype;   // data type such as char and int
    int val, hval;
    int stype;
} *id,  // currently parsed identifier
  *sym; // symbol table (simple list of identifiers)

/* Function-pointer helpers (defined after expr()). */
static int is_fptr_ident(struct ident_s *d);
static int code_fixup(struct ident_s *d);
static void indir_call(struct ident_s *d);
static void indir_call_ast(int *callee);
static int fptr_declarator(struct ident_s **out, int *dims, int *ndims);

// (library) external functions
struct ef_s {
    char *name;
    int addr;
} **ef_cache;
int ef_count;

struct member_s {
    struct ident_s *id;
    int offset;     /* byte offset of the containing storage unit */
    int type;
    int bitoff;     /* bit position within the storage unit (bit-fields) */
    int bitwid;     /* field width in bits; 0 for ordinary members */
    struct member_s *next;
} **members; // array (indexed by type) of struct member lists

// tokens and classes (operators last and in precedence order)
// ( >= 128 so not to collide with ASCII-valued tokens)
enum {
    Num = 128, // the character set of given source is limited to 7-bit ASCII
    FNum,     // floating constant; ival indexes fnums[]
    DNum,     // double constant (8 bytes in data pool)
    Func, Syscall, Main, ClearCache, Glo, Par, Loc, Stc, Keyword, Id, Label, Load, Enter,
    Break, Continue, Case, Char, Default, Else, Enum, If, Int, Return,
    Sizeof, Struct, Union, Switch, For, While, DoWhile, Goto,
    Assign, // operator =, keep Assign as highest priority operator
    OrAssign, XorAssign, AndAssign, ShlAssign, ShrAssign, // |=, ^=, &=, <<=, >>=
    AddAssign, SubAssign, MulAssign, DivAssign, ModAssign, // +=, -=, *=, /=, %=
    Cond, // operator: ?
    Lor, Lan, Or, Xor, And, // operator: ||, &&, |, ^, &
    Eq, Ne, Lt, Gt, Le, Ge, // operator: ==, !=, <, >, <=, >=
    Shl, Shr, Add, Sub, Mul, Div, Mod, // operator: <<, >>, +, -, *, /, %
    Inc, Dec, Dot, Arrow, Bracket, // operator: ++, --, ., ->, [
    Unsigned, ULTok, UGTok, ULEtok, UGEtok,  /* fcc extensions */
    /* Internal AST markers (never produced by the lexer): unsigned
     * divide/modulo and logical shift right, chosen from operand types. */
    UDivTok, UModTok, LShrTok,
    /* Recognised only so that floating point can be rejected with a clear
     * diagnostic instead of being silently misparsed as identifiers. */
    Float, Double, Static,
    /* Internal AST marker: a call whose result is a struct returned through
     * a hidden destination pointer.  Never produced by the lexer. */
    StructFunc,
    FNeg,                /* internal: unary minus on a float value */
    Void,                /* void keyword: distinct type, no stored value */
    WStr,                /* wide string literal: data-pool offset in ival */
    /* Internal AST markers for function pointers (never from next()). */
    CodeAddr,            /* a function designator used as a value */
    IndirFunc,           /* a call through a function-pointer variable */
    /* Hardening additions.  Both are appended so every existing token value
     * is unchanged.  Extern marks a call to a function with no definition in
     * this translation unit (libc/external): it must never become an svc or
     * require a definition.  GlbAddr marks an AST node holding an absolute
     * data-pool address, so the IR rewrite can recognise globals/strings by
     * opcode instead of by a fragile numeric address-range test. */
    Extern,
    GlbAddr,
};

// opcodes
/* The instruction set is designed for building intermediate representation.
 * Expression 10 + 20 will be translated into the following instructions:
 *     i = 0;
 *     text[i++] = IMM;
 *     text[i++] = 10;
 *     text[i++] = PSH;
 *     text[i++] = IMM;
 *     text[i++] = 20;
 *     text[i++] = ADD;
 *     text[i++] = PSH;
 *     text[i++] = EXIT;
 *     pc = text;
 */
enum {
    LEA , /*  0 */
    /* LEA addressed the problem how to fetch arguments inside sub-function.
     * Let's check out what a calling frame looks like before learning how
     * to fetch arguments (Note that arguments are pushed in its calling
     * order):
     *
     *     sub_function(arg1, arg2, arg3);
     *
     *     |    ....       | high address
     *     +---------------+
     *     | arg: 1        |    new_bp + 4
     *     +---------------+
     *     | arg: 2        |    new_bp + 3
     *     +---------------+
     *     | arg: 3        |    new_bp + 2
     *     +---------------+
     *     |return address |    new_bp + 1
     *     +---------------+
     *     | old BP        | <- new BP
     *     +---------------+
     *     | local var 1   |    new_bp - 1
     *     +---------------+
     *     | local var 2   |    new_bp - 2
     *     +---------------+
     *     |    ....       |  low address
     *
     * If we need to refer to arg1, we need to fetch new_bp + 4, which can not
     * be achieved by restricted ADD instruction. Thus another special
     * Instruction is introduced to do this: LEA <offset>.
     * The following pseudocode illustrates how LEA works.
     *     if (op == LEA) { ax = (int) (bp + *pc++); } // load address for arguments
     * Together with JSR, ENT, ADJ, LEV, and LEA instruction, we are able to make
     * function calls.
     */

    IMM , /*  1 */
    /* IMM <num> to put immediate <num> into general register */

    JMP , /*  2 */
    /* JMP <addr> will unconditionally set the value PC register to <addr> */
    /* The following pseudocode illustrates how JMP works:
     *     if (op == JMP) { pc = (int *) *pc; } // jump to the address
     * Note that PC points to the NEXT instruction to be executed. Thus *pc
     * stores the argument of JMP instruction, i.e. the <addr>.
     */

    JSR , /*  3 */
    /* A function is a block of code, which may be far from the instruction
     * we are currently executing. That is reason why JMP instruction exists,
     * jumping into starting point of a function. JSR is introduced to perform
     * some bookkeeping: store the current execution position so that the
     * program can resume after function call returns.
     *
     * JSR <addr> to invoke the function whose starting point is <addr> and
     * LEV to fetch the bookkeeping information to resume previous execution.
     */

    BZ  , /*  4 : conditional jump if general register is zero (jump-if-zero) */
    BNZ , /*  5 : conditional jump if general register is not zero */

    ENT , /*  6 */
    /* ENT <size> is called when we are about to enter the function call to
     * "make a new calling frame". It will store the current PC value onto
     * the stack, and save some space(<size> bytes) to store the local
     * variables for function.
     */

    ADJ , /*  7 */
    /* ADJ <size> is to adjust the stack, to "remove arguments from frame"
     * The following pseudocode illustrates how ADJ works:
     *     if (op == ADJ) { sp += *pc++; } // add esp, <size>
     */

    LEV , /*  8 */
    /* LEV fetches bookkeeping info to resume previous execution.
     * There is no POP instruction in our design, and the following pseudocode
     * illustrates how LEV works:
     *     if (op == LEV) { sp = bp; bp = (int *) *sp++;
     *                      pc = (int *) *sp++; } // restore call frame and PC
     */

    LI  , /*  9 */
    /* LI loads an integer into general register from a given memory
     * address which is stored in general register before execution.
     */

    LC  , /* 10 */
    /* LC loads a character into general register from a given memory
     * address which is stored in general register before execution.
     */

    SI  , /* 11 */
    /* SI stores the integer in general register into the memory whose
     * address is stored on the top of the stack.
     */

    SC  , /* 12 */
    /* SC stores the character in general register into the memory whose
     * address is stored on the top of the stack.
     */

    PSH , /* 13 */
    /* PSH pushes the value in general register onto the stack */

    OR  , /* 14 */  XOR , /* 15 */  AND , /* 16 */
    EQ  , /* 17 */  NE  , /* 18 */
    LT  , /* 19 */  GT  , /* 20 */  LE  , /* 21 */ GE  , /* 22 */
    SHL , /* 23 */  SHR , /* 24 */
    ADD , /* 25 */  SUB , /* 26 */  MUL , /* 27 */ DIV, /* 28 */ MOD, /* 29 */
    /* arithmetic instructions
     * Each operator has two arguments: the first one is stored on the top
     * of the stack while the second is stored in general register.
     * After the calculation is done, the argument on the stack will be poped
     * out and the result will be stored in general register.
     * So you are not able to fetch the first argument from the stack after
     * the calculation.
     */

    SYSC, /* 30 system call */
    CLCA, /* 31 clear cache, used by JIT compilation */
    UDIV = 33, UMOD, LSR,  /* unsigned divide/modulo, logical shift right */
    ULT = 40, UGT, ULE, UGE,  /* unsigned comparisons (fcc extension) */
    /* fcc conversion / extended-load extensions.  Must match ir.h. */
    SXT8 = 44, ZXT8, LCU,
    SCPY = 47,              /* fcc extension: copy N words (operand) src->dst */
    /* Function-pointer support (new opcodes; existing values unchanged).
     * CODEADDR materialises a PC-relative address of a function's first
     * instruction; JSRI calls through the address held in r0. */
    CODEADDR = 48, JSRI = 49,
    FADD = 220, FSUB, FMUL, FDIV,
    FLT, FGT, FLE, FGE, FEQ, FNE,
    F2I, I2F,
    DADD = 232, DSUB, DMUL, DDIV,
    DLT, DGT, DLE, DGE, DEQ, DNE,
    D2I, I2D, F2D, D2F, DNEG,
    PUSHS = 248,
    INVALID
};

// types
enum { CHAR, INT, PTR = 256, PTR2 = 512 };
#define ARR 1024   /* fcc extension: base of the array-type descriptor range */
#define NARR 256   /* number of array-type descriptor slots */
#define UINT 2     /* unsigned int (fcc extension) */
#define UCHAR 3    /* unsigned char (fcc extension) */
#define FLOAT 4    /* IEEE-754 single precision (fcc extension) */
#define DOUBLE 5   /* IEEE-754 double precision (fcc extension) */
#define VOID 6     /* C90 void: no storage, no value (not a basic type) */
/* Basic types occupy 0..5; 6..7 are reserved.  VOID occupies 6 deliberately:
 * IS_BASIC excludes it, so no load/store conversion can be applied to it.
 * Struct type ids are allocated from 8 upward (still below PTR), keeping
 * IS_BASIC/IS_STRUCT disjoint. */
#define BASIC_MAX DOUBLE
#define TNEW_START 8
/* AST-only type marker for a bit-field member access.  It is never used as a
 * variable type (no tsize[] entry, no storage); it only tells gen() that the
 * Load/Assign node carries bit offset/width/signedness metadata. */
#define FIELD 7
#define DATAPTR 32   /* IR opcode: data-pool-relative address */
/* Function-pointer type.  It lives in the pointer range so ty_size() reports
 * 4 bytes and gen() loads/stores it with LI/SI.  FIELD (7) is an AST-only
 * marker that can never be a declared base type, so PTR+FIELD cannot collide
 * with `void *` (PTR+VOID).  C90 forbids pointer arithmetic on function
 * pointers, so ty_step() is never consulted. */
#define FPTR (PTR + FIELD)
/* Type classification helpers. */
#define IS_BASIC(t)  ((t) >= CHAR && (t) <= BASIC_MAX)
#define IS_STRUCT(t) ((t) >= TNEW_START && (t) < PTR)
#define IS_ARRAY(t)  ((t) >= ARR && (t) < ARR + NARR)

// ELF generation
char **plt_func_addr;
char *freebuf;

char *append_strtab(char **strtab, char *str)
{
    char *s;
    for (s = str; *s && (*s != ' '); s++) ; /* ignore trailing space */
    int nbytes = s - str + 1;
    char *res = *strtab;
    memcpy(res, str, nbytes);
    res[s - str] = 0; // null terminator
    *strtab = res + nbytes;
    return res;
}

/* Error path must be bulletproof: the full printf/vfnprintf formatter on
 * this minimal libc can itself fault when called from a low-memory/error
 * state (reproduced as a HardFault in _vfnprintf).  Use only puts/putchar
 * and manual integer formatting so a compiler error always reports cleanly
 * instead of crashing the whole process. */
static void put_dec(int v)
{
    char b[12];
    int i, n;
    if (v == 0) { putchar('0'); return; }
    if (v < 0) { putchar('-'); v = -v; }
    i = 0;
    while (v > 0) { b[i++] = '0' + (v % 10); v = v / 10; }
    n = i;
    while (i > 0) putchar(b[--i]);
}

char fatal(char *msg)
{
    put_dec(line);
    putchar(':');
    putchar(' ');
    puts(msg);
    exit(-1);
}

/* Central pool bounds checks.  `e` and `data` grow upward, `n` grows down.
 * A zero count still verifies the current position is in range. */
static void need_ir(int count)
{
    if (count < 0 || e + count >= ir_limit)
        fatal("compiler IR pool overflow");
}

static void need_ast(int count)
{
    if (count < 0 || n - count < ast_floor)
        fatal("compiler AST pool overflow");
}

static void need_data(int count)
{
    if (count < 0 || data + count >= data_limit)
        fatal("compiler data pool overflow");
}

/* Copy a symbol name into a NUL-terminated buffer.  Symbol-table names are
 * followed by a length byte rather than a NUL, so strcmp() needs a copy. */
static void sym_getname(struct ident_s *d, char *buf, int bufsz)
{
    int len = d->hash & 0x3f;
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, d->name, (size_t) len);
    buf[len] = 0;
}

/* Bounds-checked pool writes.  The checks are on the overflow path only; the
 * emitted IR and AST words are byte-for-byte identical to the original code. */
#define IRW(v)  do { need_ir(1);  *++e = (v); } while (0)
#define ASTW(v) do { need_ast(1); *--n = (v); } while (0)

/* Non-fatal diagnostics.  These only print and count; they never alter the
 * parse, the symbol table's codegen fields, or the emitted IR. */
void warn(char *msg) { printf("%d: warning: %s\n", line, msg); warnings++; }

/* `sym` is the flat symbol table; a usage bit is kept alongside it so that
 * struct ident_s itself is unchanged.  Out-of-range entries are treated as
 * referenced, so an unexpected pointer can never produce a false warning. */
static void set_used(struct ident_s *d, int v)
{
    int idx = (int)(d - sym);
    if (idx >= 0 && idx < SYM_USED_MAX) sym_used[idx] = (unsigned char) v;
}

static int get_used(struct ident_s *d)
{
    int idx = (int)(d - sym);
    if (idx >= 0 && idx < SYM_USED_MAX) return sym_used[idx];
    return 1;
}

/* Build a bounded warning message that names an identifier.  Symbol names
 * are not NUL-terminated in the source pool; the low 6 bits of `hash` hold
 * the length (see next()). */
static void warn_ident(char *prefix, struct ident_s *d)
{
    char buf[128];
    int nlen = d->hash & 0x3f;
    if (nlen > 63) nlen = 63;
    snprintf(buf, sizeof(buf), "%s'%.*s'", prefix, nlen, d->name);
    warn(buf);
}

/* ---- Aggregate type descriptors ----------------------------------------
 * Array types are encoded as ARR+index into atypes[].  Each descriptor
 * records the element type (which may itself be an array type), the number
 * of elements in this dimension and the total byte size.  This lets a type
 * value carry full multidimensional-array information through the parser. */
struct array_type {
    int elem;                   /* element type */
    int nelem;                  /* elements in this dimension */
    int size;                   /* total bytes */
};
static struct array_type atypes[NARR];
static int narr;

/* Total byte size of a type. */
static int ty_size(int t)
{
    if (IS_ARRAY(t)) return atypes[t - ARR].size;
    if (t >= PTR) return 4;     /* all pointers occupy one word */
    /* Defensive: a corrupted type index or a heap-clobbered tsize pointer
     * used to cause a wild-pointer data load (HardFault at tsize[t]).  Turn
     * it into a clear diagnostic so the underlying pool corruption is
     * visible instead of a silent crash. */
    if (t < 0 || !tsize)
        fatal("bad type in ty_size");
    return tsize[t];            /* basic or struct type */
}

/* Array-to-pointer conversion: array of T becomes pointer to T. */
static int ty_decay(int t)
{
    if (IS_ARRAY(t)) return PTR + atypes[t - ARR].elem;
    return t;
}

/* Step size for pointer/array arithmetic: size of the pointed-to element. */
static int ty_step(int t)
{
    if (IS_ARRAY(t)) return ty_size(atypes[t - ARR].elem);
    if (t >= PTR) return ty_size(t - PTR);
    return ty_size(t);
}

/* Allocate a descriptor for "array[nelem] of elem" and return its type. */
static int arr_new(int elem, int nelem)
{
    int esz = ty_size(elem);
    if (narr >= NARR) fatal("too many array types");
    atypes[narr].elem = elem;
    atypes[narr].nelem = nelem;
    atypes[narr].size = esz * nelem;
    return ARR + narr++;
}

void ef_add(char *name, int addr) // add external function
{
    ef_cache[ef_count] = malloc(sizeof(struct ef_s)) ;
    ef_cache[ef_count]->name = malloc(strlen(name)+1);
    strcpy(ef_cache[ef_count]->name, name);
    ef_cache[ef_count]->addr = addr;
    ++ef_count;
}

int ef_getaddr(int idx) // get address external function
{
    return (elf ? (int) plt_func_addr[idx] : ef_cache[idx]->addr);
}

/* Map an external C function name to a FUZIX syscall number.  The number
 * is stored in the external-function cache's addr field and interpreted by
 * the Thumb-1 back end as the immediate of an `svc` instruction. */
struct syscall_map { const char *name; int num; };
static const struct syscall_map syscall_map[] = {
    {"_exit",0},{"exit",0},{"open",1},{"close",2},{"rename",3},
    {"mknod",4},{"link",5},{"unlink",6},{"read",7},{"write",8},
    {"_lseek",9},{"lseek",9},{"chdir",10},{"sync",11},{"access",12},
    {"chmod",13},{"chown",14},{"_stat",15},{"_fstat",16},{"dup",17},
    {"getpid",18},{"getppid",19},{"getuid",20},{"umask",21},
    {"_statfs",22},{"execve",23},{"_getdirent",24},{"setuid",25},
    {"setgid",26},{"_time",27},{"time",27},{"_stime",28},{"ioctl",29},
    {"brk",30},{"sbrk",31},{"_fork",32},{"mount",33},{"_umount",34},
    {"signal",35},{"dup2",36},{"_pause",37},{"_alarm",38},{"kill",39},
    {"pipe",40},{"getgid",41},{"times",42},{"utime",43},{"geteuid",44},
    {"getegid",45},{"chroot",46},{"fcntl",47},{"fchdir",48},
    {"fchmod",49},{"fchown",50},{"mkdir",51},{"rmdir",52},
    {"setpgrp",53},{"_uname",54},{"waitpid",55},{"nice",59},
    /* amacc registers these when it parses / and %; our Thumb backend
     * emits its own helper, so the index is never used at runtime. */
    {"__aeabi_idiv",0},{"__aeabi_idivmod",0},
    /* Unsigned counterparts, registered so the frontend can use ef_getidx
     * for UDIV/UMOD just as it does for DIV/MOD.  The Thumb backend emits its
     * own unsigned helper, so these indices are never called at runtime. */
    {"__aeabi_uidiv",0},{"__aeabi_uidivmod",0},
    {"yield",64},{"_ftruncate",68},{"_select",72},{"setgroups",73},
    {"getgroups",74},{"getrlimit",75},{"setrlimit",76},{"setpgid",77},
    {"setsid",78},{"getsid",79}
};

/* Non-fatal variant used to classify an implicit call.  Returns the syscall
 * number, or -1 when the name is not a syscall.  Unknown names are ordinary
 * external functions and must not abort compilation. */
static int syscall_try(const char *name)
{
    unsigned int i;
    for (i = 0; i < sizeof(syscall_map)/sizeof(syscall_map[0]); i++)
        if (!strcmp(syscall_map[i].name, name))
            return syscall_map[i].num;
    return -1;
}

static int syscall_lookup(const char *name)
{
    int n = syscall_try(name);
    if (n < 0) fatal("unknown external function");
    return n;
}

int ef_getidx(char *name) // get cache index of external function
{
    int i;
    for (i = 0; i < ef_count; ++i)
        if (!strcmp(ef_cache[i]->name, name))
            return i;
    ef_add(name, syscall_lookup(name));
    return ef_count - 1;
}

/* FUZIX's libc strtod mis-converts fractional digits (0.75 -> 0.57), so
 * floating constants are parsed here with a correct decimal algorithm. */
static double fcc_strtod(const char **endp)
{
    const char *s = *endp;
    double d = 0.0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { d = d * 10.0 + (*s - '0'); s++; }
    if (*s == '.') {
        double frac = 0.0, scale = 1.0;
        s++;
        while (*s >= '0' && *s <= '9') {
            frac = frac * 10.0 + (*s - '0');
            scale *= 10.0;
            s++;
        }
        d += frac / scale;
    }
    if (*s == 'e' || *s == 'E') {
        int esign = 1, e = 0;
        s++;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') { e = e * 10 + (*s - '0'); s++; }
        while (e-- > 0) { if (esign > 0) d *= 10.0; else d /= 10.0; }
    }
    *endp = s;
    return sign > 0 ? d : -d;
}

/* Return nonzero if the token starting at s is a floating constant rather
 * than an integer.  A leading 0x is always an integer (C90 has no hex
 * floating constants). */
static int is_float_literal(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return 0;
    while (*s >= '0' && *s <= '9') s++;
    if (*s == '.') return 1;
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-') s++;
        if (*s >= '0' && *s <= '9') return 1;
    }
    return 0;
}

/* C90 escape sequence decoding for string literals and character constants.
 * Numeric values saturate at ESC_UCHAR_MAX so that a long \x escape cannot
 * overflow.  ESC_HEX_MAX is a defensive loop bound larger than the whole
 * source pool (FCC_POOLSZ), so no real escape can be truncated by it. */
#define ESC_HEX_MAX 4096
#define ESC_UCHAR_MAX 255

/* Return the value of a hex digit, or -1 when c is not a hex digit. */
static int esc_hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The first octal digit was already consumed; take at most two more. */
static int esc_octal(int first, char **pp)
{
    int value = first - '0';
    int count = 1;
    while (count < 3 && **pp >= '0' && **pp <= '7') {
        value = value * 8 + (*(*pp)++ - '0');
        ++count;
    }
    return value;
}

/* Consume one or more hex digits after 'x'; saturate at ESC_UCHAR_MAX.
 * A stray '\x' with no digit keeps the historical literal 'x'. */
static int esc_hex(char **pp)
{
    int value = 0;
    int count = 0;
    int d;
    while (count < ESC_HEX_MAX) {
        d = esc_hex_digit(**pp);
        if (d < 0) break;
        ++(*pp);
        if (value < ESC_UCHAR_MAX) {
            if (value > (ESC_UCHAR_MAX - d) / 16)
                value = ESC_UCHAR_MAX;
            else
                value = value * 16 + d;
        }
        ++count;
    }
    if (count == 0) return 'x';
    return value;
}

/* Decode the escape character after a backslash.  Unknown escapes keep the
 * literal character, exactly as before. */
static int decode_escape(char **pp)
{
    int c = *(*pp)++;
    int value = c;
    switch (c) {
    case 'a': value = '\a'; break;      /* bell */
    case 'b': value = '\b'; break;      /* backspace */
    case 'f': value = '\f'; break;      /* form feed */
    case 'n': value = '\n'; break;      /* new line */
    case 'r': value = '\r'; break;      /* carriage return */
    case 't': value = '\t'; break;      /* horizontal tab */
    case 'v': value = '\v'; break;      /* vertical tab */
    case '\\': value = '\\'; break;     /* backslash */
    case '\'': value = '\''; break;     /* single quote */
    case '"': value = '"'; break;       /* double quote */
    case '?': value = '?'; break;       /* question mark */
    case 'x': value = esc_hex(pp); break; /* hexadecimal */
    default:
        if (c >= '0' && c <= '7') value = esc_octal(c, pp); /* octal */
        break;
    }
    return value;
}

/* Store one 32-bit word little-endian in the data pool.  Explicit byte
 * stores keep the pool layout identical on the 32-bit target and on a
 * 64-bit host where `int` may be widened to `long`. */
static void wchar_put(char **dpp, int value)
{
    char *q = *dpp;
    if (q + 4 > data_limit) fatal("compiler data pool overflow");
    *q++ = (char) value;
    *q++ = (char) (value >> 8);
    *q++ = (char) (value >> 16);
    *q++ = (char) (value >> 24);
    *dpp = q;
}

/* Lex a wide character constant or wide string literal introduced by the L
 * prefix.  `quote` is either '\'' or '"'; p points just past that quote.
 * A character constant leaves its 32-bit value in ival with tk=Num, so
 * sizeof(L'a') is 4.  A string writes each character as a 32-bit word into
 * the data pool, records its total byte count in wide_str_size and leaves
 * the pool offset in ival with tk=WStr.  Escape decoding is shared with
 * ordinary literals through decode_escape(). */
static void lex_wide(int quote)
{
    int wide_string = (quote == '"');
    char *first;
    int count = 0;
    if (wide_string) {
        data = (char *) (((int) data + 3) & -4); /* align to a word */
        need_data(4);
        first = data;
    }
    while (*p != 0 && *p != quote) {
        int ch = *p++;
        if (ch == '\\')
            ch = decode_escape(&p);
        if (wide_string) {
            wchar_put(&data, ch);
            ++count;
        } else {
            ival = ch;
        }
    }
    ++p;                       /* consume the closing quote */
    if (wide_string) {
        wchar_put(&data, 0);   /* 32-bit NUL terminator */
        wide_str_size = (count + 1) * 4;
        ival = (int) first;
        tk = WStr;
    } else {
        tk = Num;
    }
}

/* Consume C90 integer suffixes U/L/UL/LU in any order, case-insensitively.
 * fcc's int is 32 bits, so every C90 integer constant fits in it and the
 * suffix can be accepted and ignored without changing the value.  The float
 * suffix path is handled separately by the FNum branches in next(). */
static void skip_int_suffix(void)
{
    int seen_u = 0, seen_l = 0, guard = 0;
    while (guard++ < 4 && ((*p == 'u' || *p == 'U') ||
                           (*p == 'l' || *p == 'L'))) {
        if (*p == 'u' || *p == 'U') {
            if (seen_u) fatal("duplicate integer suffix");
            seen_u = 1;
        } else {
            if (seen_l) fatal("duplicate integer suffix");
            seen_l = 1;
        }
        ++p;
    }
}

/* parse next token
 * 1. store data into id and then set the id to current lexical form
 * 2. set tk to appropriate type
 */
void next()
{
    char *pp;

    /* using loop to ignore whitespace characters, but characters that
     * cannot be recognized by the lexical analyzer are considered blank
     * characters, such as '@' and '$'.
     */
    while ((tk = *p)) {
        ++p;
        if (tk == 'L' && (*p == '\'' || *p == '"')) {
            int quote = *p++;
            lex_wide(quote);
            return;
        }
        if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') ||
            (tk == '_')) {
            pp = p - 1;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || (*p == '_'))
                tk = tk * 147 + *p++; // 147 is the magic number generating hash value
            tk = (tk << 6) + (p - pp);  // hash plus symbol length
            // hash value is used for fast comparison. Since it is inaccurate,
            // we have to validate the memory content as well.
            for (id = sym; id->tk && id < sym_limit; id++) { /* find free slot */
                if (tk == id->hash && /* if token is found (hash match), overwrite */
                    !memcmp(id->name, pp, p - pp)) {
                    tk = id->tk;
                    return;
                }
            }
            if (id >= sym_limit) fatal("compiler symbol table overflow");
            /* At this point, existing symbol name is not found.
             * "id" points to the first unused symbol table entry.
             */
            id->name = pp;
            id->hash = tk;
            tk = id->tk = Id;  // token type identifier
            return;
        }
        /* Calculate the constant */
        // first byte is a number, and it is considered a numerical value
        else if (tk >= '0' && tk <= '9') {
            if (is_float_literal(p - 1)) {   /* tk holds the first digit */
                char *endp;
                const char *np = p - 1; double d = fcc_strtod(&np); endp = (char *)np;   /* include the first digit */
                if (nfnums >= FNUM_MAX) fatal("too many floating constants");
                fnums[nfnums] = d;
                p = endp;
                /* C90: f/F suffix selects float, l/L selects double, an
                 * unsuffixed constant is double.  Record the suffix in the
                 * high bits of ival via a second table entry flag. */
                ival = nfnums++;
                if (*p == 'f' || *p == 'F') { ival |= 0x8000; p++; }
                else { ival |= 0x4000; if (*p == 'l' || *p == 'L') p++; }
                tk = FNum;
                return;
            }
            /* Parse with 3 conditions:
             * 1) not starting with 0 :=> decimal number;
             * 2) starting with 0x :=> hex number;
             * 3) starting with 0: octal number;
             */
            if ((ival = tk - '0')) {
                while (*p >= '0' && *p <= '9')
                    ival = ival * 10 + *p++ - '0';
            }
            // first digit is 0 and it starts with 'x', and it is considered
            // to be a hexadecimal number
            else if (*p == 'x' || *p == 'X') {
                while ((tk = *++p) &&
                       ((tk >= '0' && tk <= '9') ||
                        (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F')))
                    ival = ival * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0);
            } else { // considered octal
                while (*p >= '0' && *p <= '7')
                    ival = ival * 8 + *p++ - '0';
            }
            skip_int_suffix();  /* U/L/UL/LU: accepted, value already fits */
            tk = Num; // token is numeric, return
            return;
        }
        switch (tk) {
        case '\n':
            /* Take an integer (representing an operation) and print out
             * the name of that operation. First thing to say is that "* ++le"
             * is the integer representing the operation to perform.
             * This basically walks through the array of instructions
             * returning each integer in turn.
             *
             * Starting at the beginning of the line, we have "printf" with
             * a format string of "%8.4s". This means printing out the first 4
             * characters of the string that we are about to pass next (padded
             * to 8 characters). There then follows a string containing all of
             * the operation names, in numerical order, padded to 4 characters
             * and separated by commas (so the start of each is 5 apart).
             *
             * Finally, we do a lookup into this string (treating it as an
             * array) at offset "* ++le * 5", i.e. the integer representing
             * the operation multiplied by "5", being the number of characters
             * between the start of each operation name). Doing this lookup
             * gives us a char, but actually we wanted the pointer to this
             * char (as we want printf to print out this char and the
             * following 3 chars), so we take the address of this char
             * (the "&" at the beginning of the whole expression).
             */
            if (src) {
                int *base = le;
                printf("%d: %.*s", line, p - lp, lp);
                lp = p;
                while (le < e) {
                    int off = le - base; // Func IR instruction memory offset
                    printf("%04d: %8.4s", off,
                           & "LEA  IMM  JMP  JSR  BZ   BNZ  ENT  ADJ  LEV  "
                             "LI   LC   SI   SC   PSH  "
                             "OR   XOR  AND  EQ   NE   LT   GT   LE   GE   "
                             "SHL  SHR  ADD  SUB  MUL  DIV  MOD  "
                             "SYSC CLCA" [*++le * 5]);
                    if (*le <= ADJ) {
                        ++le;
                        if (*le > (int) base && *le <= (int) e)
                            printf(" %04d\n", off + ((*le - (int) le) >> 2) + 1);
                        else
                            printf(" %d\n", *le);
                    }
                    else if (*le == SYSC) {
                        printf(" %s\n", ef_cache[*(++le)]->name);
                    }
                    else printf("\n");
                }
            }
            ++line;
        case ' ':
        case '\t':
        case '\v':
        case '\f':
        case '\r':
            break;
        case '/':
            if (*p == '/') { // comment
        case '#': // skip #include statement, preprocessor directives ignored
                while (*p != 0 && *p != '\n') ++p;
            } else if (*p == '*') { // C-style multiline comments
                int t = 0;
                for (++p; (*p != 0) && (t == 0); ++p) {
                    pp = p + 1;
                    if (*p == '\n') line++;
                    else if (*p == '*' && *pp == '/') t = 1;
                }
                ++p;
            } else {
                if (*p == '=') { ++p; tk = DivAssign; }
                else tk = Div; return;
            }
            break;
        case '\'': // quotes start with character (string)
        case '"':
            pp = data;
            // While current character is not `\0` and current character is
            // not the quote character.
            while (*p != 0 && *p != tk) {
                // If current character is '\', it is escape notation or simply
                // '\' character.
                if ((ival = *p++) == '\\')
                    ival = decode_escape(&p);
                // If it is double quotes (string literal), it is considered as
                // a string, copying characters to data
                if (tk == '"') { need_data(1); *data++ = ival; }
            }
            ++p;
            //  If .text too big rwdata v_addr will overlap it, add that to stay away from .text
            if (tk == '"') ival = (int) pp; else tk = Num;
            return;
        case '=': if (*p == '=') { ++p; tk = Eq; } else tk = Assign; return;
        case '+': if (*p == '+') { ++p; tk = Inc; }
                  else if (*p == '=') { ++p; tk = AddAssign; }
                  else tk = Add; return;
        case '-': if (*p == '-') { ++p; tk = Dec; }
                  else if (*p == '>') { ++p; tk = Arrow; }
                  else if (*p == '=') { ++p; tk = SubAssign; }
                  else tk = Sub; return;
        case '!': if (*p == '=') { ++p; tk = Ne; } return;
        case '<': if (*p == '=') { ++p; tk = Le; }
                  else if (*p == '<') {
                      ++p; if (*p == '=') { ++p ; tk = ShlAssign; } else tk = Shl;
                  }
                  else tk = Lt; return;
        case '>': if (*p == '=') { ++p; tk = Ge; }
                  else if (*p == '>') {
                      ++p; if (*p == '=') { ++p ; tk = ShrAssign; } else tk = Shr;
                  }
                  else tk = Gt; return;
        case '|': if (*p == '|') { ++p; tk = Lor; }
                  else if (*p == '=') { ++p; tk = OrAssign; }
                  else tk = Or; return;
        case '&': if (*p == '&') { ++p; tk = Lan; }
                  else if (*p == '=') { ++p; tk = AndAssign; }
                  else tk = And; return;
        case '^': if (*p == '=') { ++p; tk = XorAssign; } else tk = Xor; return;
        case '*': if (*p == '=') { ++p; tk = MulAssign; }
                  else tk = Mul; return;
        case '%': if (*p == '=') { ++p; tk = ModAssign; }
                  else tk = Mod; return;
        case '[': tk = Bracket; return;
        case '?': tk = Cond; return;
        case '.':
            if (*p >= '0' && *p <= '9') {
                char *endp;
                const char *np;
                double d;
                *--p = '.';             /* put the dot back for the parser */
                np = p;
                d = fcc_strtod(&np);
                endp = (char *)np;
                if (nfnums >= FNUM_MAX) fatal("too many floating constants");
                fnums[nfnums] = d;
                p = endp;
                ival = nfnums++;
                if (*p == 'f' || *p == 'F') { ival |= 0x8000; p++; }
                else { ival |= 0x4000; if (*p == 'l' || *p == 'L') p++; }
                tk = FNum;
                return;
            }
            tk = Dot; return;
        default: return;
        }
    }
}

// https://stackoverflow.com/questions/109023/how-to-count-the-number-of-set-bits-in-a-32-bit-integer
int popcount(int i)
{
    i = i - ((i >> 1) & 0x55555555); // add pairs of bits
    i = (i & 0x33333333) + ((i >> 2) & 0x33333333); // quads
    i = (i + (i >> 4)) & 0x0F0F0F0F; // groups of 8
    return (i * 0x01010101) >> 24; // horizontal sum of bytes
}

/* True when t can begin a type-name.  The preprocessor expands typedef
 * aliases to their underlying type before fcc sees the source, so an Id is
 * never a type-name here. */
static int is_type_name(int t)
{
    return t == Int || t == Char || t == Unsigned || t == Float ||
           t == Double || t == Struct || t == Union || t == Enum ||
           t == Void;
}

/* expression parsing
 * lev represents an operator.
 * because each operator `token` is arranged in order of priority, so
 * large `lev` indicates a high priority.
 *
 * Operator precedence (lower first):
 * Assign  =
 * Cond    ?
 * Lor     ||
 * Lan     &&
 * Or      |
 * Xor     ^
 * And     &
 * Eq      ==
 * Ne      !=
 * Lt      <
 * Gt      >
 * Le      <=
 * Ge      >=
 * Shl     <<
 * Shr     >>
 * Add     +
 * Sub     -
 * Mul     *
 * Div     /
 * Mod     %
 * Inc     ++
 * Dec     --
 * Bracket [
 */

enum { REENTRANT = 0x10000 };

void expr(int lev)
{
    int tc;
    int t, *b, sz, *c;
    struct ident_s *d;
    struct member_s *m;

    switch (tk) {
    case 0: fatal("unexpected EOF in expression");
    case Float:
    case Double:
        fatal("floating point expressions are not supported by fcc");
        break;
    // directly take an immediate value as the expression value
    // IMM recorded in emit sequence
    case Num: ASTW(ival); ASTW(Num); next(); ty = INT; break;
    case FNum: {
        /* Convert to single precision and materialise the IEEE-754 bit
         * pattern as a 32-bit immediate. */
        if (FNUM_ISFLOAT(ival)) {
            float fv = (float) fnums[FNUM_IDX(ival)];
            unsigned int bits = 0;
            memcpy(&bits, &fv, sizeof(bits));
            ASTW((int) bits); ASTW(FNum);
            ty = FLOAT;
        } else {
            double dv = fnums[FNUM_IDX(ival)];
            unsigned long long bits = 0;
            memcpy(&bits, &dv, sizeof(bits));
            data = (char *) (((int) data + 7) & -8);
            need_data((int) sizeof(bits));
            memcpy(data, &bits, sizeof(bits));
            ASTW((int)(data - freedata)); ASTW(DNum);
            data += sizeof(bits);
            ty = DOUBLE;
        }
        next();
        break;
    }
    case '"': // string, as a literal in data segment
        /* ival is an absolute data-pool address.  Tag it with GlbAddr so
         * codegen emits DATAPTR directly instead of relying on a numeric
         * address-range test that could alias an ordinary integer literal. */
        ASTW(ival); ASTW(GlbAddr); next();
        // continuous `"` handles C-style multiline text such as `"abc" "def"`
        while (tk == '"') next();
        /* Point "data" to the next target-word (4-byte) boundary.  Use the
         * literal 4 rather than sizeof(int): this code sits before #undef int
         * on 64-bit hosts, where sizeof(int) would be the widened long (8).
         * -4 is 0b11111100, guaranteeing at least one '\0' after the string.
         *
         * Append the NUL by advancing data one position; the pool starts
         * zeroed.  CHAR + PTR = PTR because CHAR is 0. */
        data = (char *) (((int) data + 4) & -4);
        need_data(1);
        ty = PTR;
        break;
    case WStr:
        /* Wide string literal: an array of 32-bit wchar_t values with a NUL
         * terminator.  Keeping the array type makes sizeof(L"abc") == 16;
         * subscripts decay it to a pointer to INT so each step is 4 bytes. */
        {
            int total = wide_str_size;
            ASTW(ival); ASTW(GlbAddr);
            next();
            while (tk == WStr) { total += wide_str_size - 4; next(); }
            ty = arr_new(INT, total / 4);
        }
        break;
    /* sizeof (type-name), sizeof (object) and sizeof unary-expression.
     * An object's declared type is used verbatim (no array decay); the
     * unary-expression operand is unevaluated, so its AST is discarded and
     * only the size constant is emitted. */
    case Sizeof: {
        char *save_p;
        int save_tk, save_iv, save_line, inner;
        struct ident_s *save_id;
        next();
        if (tk == Id) {
            /* Plain identifier: use the declared type, so an array operand
             * does not decay (the sizeof operand is unevaluated). */
            save_p = p; save_tk = tk; save_iv = ival;
            save_id = id; save_line = line;
            d = id;
            next();                     /* peek past the identifier */
            if (tk != Bracket && tk != Dot && tk != Arrow &&
                tk != Inc && tk != Dec && tk != '(') {
                set_used(d, 1);
                p = save_p; tk = save_tk; ival = save_iv;
                id = save_id; line = save_line;
                next();                 /* consume the identifier */
                ty = d->type;
                if (ty == VOID) fatal("sizeof(void) is undefined");
                ASTW(ty_size(ty)); ASTW(Num);
                ty = INT;
                break;
            }
            p = save_p; tk = save_tk; ival = save_iv;
            id = save_id; line = save_line;
        }
        if (tk == '(') {
            /* One-token lookahead: save and restore the lexer state so a
             * peek can never change the symbol table or the parse. */
            save_p = p; save_tk = tk; save_iv = ival;
            save_id = id; save_line = line;
            next();
            inner = tk;
            p = save_p; tk = save_tk; ival = save_iv;
            id = save_id; line = save_line;
            if (is_type_name(inner)) {
                next();                 /* consume '(' */
                ty = INT;
                switch (tk) {
                case Int: next(); break;
                case Void: next(); ty = VOID; break;
                case Char: next(); ty = CHAR; break;
                case Float: next(); ty = FLOAT; break;
                case Double: next(); ty = DOUBLE; break;
                case Enum:
                    next();
                    if (tk == Id) next(); /* optional enum tag */
                    ty = INT; break;
                case Unsigned:
                    next(); ty = UINT;
                    if (tk == Char) { next(); ty = UCHAR; }
                    else if (tk == Int) next();
                    break;
                case Struct:
                case Union:
                    next();
                    if (tk != Id) fatal("bad struct/union type");
                    ty = id->stype; next(); break;
                }
                while (tk == Mul) { next(); ty += PTR; }
                if (tk != ')') fatal("close parentheses expected in sizeof");
                next();
                if (ty == VOID) fatal("sizeof(void) is undefined");
                ASTW(ty_size(ty)); ASTW(Num);
                ty = INT;
                break;
            }
            if (inner == Id) {
                next();                 /* consume '(' */
                d = id; set_used(d, 1); /* sizeof still counts as a reference */
                ty = d->type;
                next();                 /* consume the object identifier */
                if (tk == ')') {
                    next();
                    if (ty == VOID) fatal("sizeof(void) is undefined");
                    ASTW(ty_size(ty)); ASTW(Num);
                    ty = INT;
                    break;
                }
                /* Not a simple object: reparse as a unary-expression. */
                p = save_p; tk = save_tk; ival = save_iv;
                id = save_id; line = save_line;
            }
        }
        {
            int *save_n = n;
            expr(Inc);
            if (ty == VOID) fatal("sizeof(void) is undefined");
            sz = ty_size(ty);
            n = save_n;                 /* unevaluated operand: drop its AST */
            ASTW(sz); ASTW(Num);
            ty = INT;
        }
        break;
    }
    case Id:
        d = id; next();
        set_used(d, 1);     /* a reference, whether a call or a variable */
        // function call
        if (tk == '(') {
            /* A variable of function-pointer type is called indirectly.
             * Direct calls keep the original code path unchanged. */
            if (is_fptr_ident(d)) {
                indir_call(d);
                break;
            }
            if ((d->class < Func || d->class > ClearCache) &&
                d->class != Extern) {
                int scnum, efidx;
                char namebuf[64];
                if (d->class != 0) fatal("bad function call");
                warn_ident("implicit declaration of function ", d);
                sym_getname(d, namebuf, sizeof(namebuf));
                scnum = syscall_try(namebuf);
                if (scnum >= 0) {
                    /* Known syscall: keep the svc path, but go through the
                     * forward-fixup mechanism so a later user definition of
                     * the same name (collision) can redirect the call. */
                    efidx = ef_getidx(namebuf);
                    d->class = Syscall;
                    if (nfwd_calls >= FWD_CALL_MAX) fatal("too many forward calls");
                    fwd_ident[nfwd_calls] = d;
                    fwd_efidx[nfwd_calls] = efidx;
                    d->val = -(++nfwd_calls);
                } else {
                    /* Any other name (printf, malloc, ...) is an external
                     * function.  Assume int return, require no definition,
                     * and emit a call that fails clearly at run time. */
                    d->class = Extern;
                    d->val = 0;
                }
                d->type = INT;
            }
            next();
            t = 0; b = 0; // parameters count
            while (tk != ')') {
                {
                    int aty;
                    expr(Assign);
                    aty = ty;
                    /* Arg chain node: [prev, arg-type, arg-AST...] */
                    ASTW(aty); ASTW((int) b); b = n;
                    /* Count stack words, not arguments (structs take N). */
                    t += IS_STRUCT(aty) ? (ty_size(aty) + 3) / 4 : 1;
                }
                if (tk == ',') {
                    next();
                    if (tk == ')') fatal("unexpected comma in function call");
                } else if (tk != ')') fatal("missing comma in function call");
            }
            next();
            /* A struct-returning function gets a hidden destination pointer
             * as its last argument.  Reserve a temporary in this frame and
             * pass its address; the call result is that temporary. */
            /* A forward-declared but not yet defined function has val == -1.
             * Encode a fixup index in the call AST; gen() records the JSR
             * operand slot and we patch it once the definition is parsed. */
            if (d->class == Func && d->val == -1) {
                char namebuf[64];
                int fi;
                if (nfwd_calls >= FWD_CALL_MAX) fatal("too many forward calls");
                fi = nfwd_calls;
                fwd_ident[fi] = d;
                /* Record whether this prototype names a syscall.  If no
                 * definition appears, the call becomes a real svc instead of
                 * dying as an unresolved prototype. */
                sym_getname(d, namebuf, sizeof(namebuf));
                fwd_efidx[fi] = (syscall_try(namebuf) >= 0)
                                ? ef_getidx(namebuf) : -1;
                d->val = -(++nfwd_calls);     /* negative fixup index marker */
            }
            if (d->class == Func && (IS_STRUCT(d->type) || d->type == DOUBLE)) {
                int words = d->type == DOUBLE ? 2 : (ty_size(d->type) + 3) / 4;
                int tv;
                if (words < 1 || words > 64) fatal("struct return too large");
                tv = (ld += words);
                ASTW(loc - tv); ASTW(Loc);  /* address of the temp */
                ASTW(PTR); ASTW((int) b); b = n; ++t;
                ASTW(loc - tv);              /* n[4]: result temp LEA */
                ASTW(t); ASTW(d->val); ASTW((int) b); ASTW(StructFunc);
            } else {
                ASTW(t); ASTW(d->val); ASTW((int) b); ASTW(d->class);
            }
            ty = d->type;
        }
        // enumeration, only enums have ->class == Num
        else if (d->class == Num) { ASTW(d->val); ASTW(Num); ty = INT; }
        else if (d->class == Func) {
            /* A function designator decays to a pointer to its first
             * instruction.  CODEADDR is relocated like a JSR target. */
            ASTW(code_fixup(d)); ASTW(CodeAddr);
            ty = FPTR;
        }
        else {
            // Variable get offset
            switch (d->class) {
            case Loc: case Par: ASTW(loc - d->val); ASTW(Loc); break;
            /* Globals/statics hold absolute data-pool addresses; use the
             * dedicated marker so they cannot alias integer literals. */
            case Glo: case Stc: ASTW(d->val); ASTW(GlbAddr); break;
            default: fatal("undefined variable");
            }
            if (IS_ARRAY(d->type)) {
                /* An array name yields the address of its first element.
                 * For a multidimensional array the decayed pointer still
                 * points at the whole sub-array, so the next subscript uses
                 * the sub-array's size as its stride.  An array *parameter*
                 * slot holds the pointer value, so that value must be loaded. */
                ty = ty_decay(d->type);
                if (d->class == Par) { ASTW(ty); ASTW(Load); }
            } else {
                ASTW(ty = d->type); ASTW(Load);
                /* A double parameter slot holds a pointer to the 8-byte
                 * value; load that pointer.  Local/global doubles already
                 * evaluate to their storage address. */
                if (ty == DOUBLE && d->class == Par) n[1] = INT;
            }
        }
        break;
    // Type cast or parenthesis
    case '(':
        next();
        /* C90 also permits (*fp)(args).  Detect the exact `(*id)(` shape
         * with a restorable lookahead; anything else is unchanged. */
        if (tk == Mul) {
            char *save_p;
            int save_tk, save_iv, save_line;
            struct ident_s *save_id, *fd;
            save_p = p; save_tk = tk; save_iv = ival;
            save_id = id; save_line = line;
            next();
            if (tk == Id) {
                fd = id;
                next();
                if (tk == ')') {
                    next();
                    if (tk == '(' && is_fptr_ident(fd)) {
                        indir_call(fd);
                        break;
                    }
                }
            }
            p = save_p; tk = save_tk; ival = save_iv;
            id = save_id; line = save_line;
        }
        if (tk == Int || tk == Char || tk == Unsigned || tk == Float ||
            tk == Double || tk == Struct || tk == Union || tk == Void) {
            switch (tk) {
            case Double: next(); t = DOUBLE; break;
            case Float: next(); t = FLOAT; break;
            case Int: next(); t = INT; break;
            case Void: next(); t = VOID; break;
            case Char: next(); t = CHAR; break;
            case Unsigned:
                next(); t = UINT;
                if (tk == Char) { next(); t = UCHAR; }
                else if (tk == Int) next();
                break;
            default:
                next();
                if (tk != Id) fatal("bad struct/union type");
                t = id->stype; next(); break;
            }
            // t: pointer
            while (tk == Mul) { next(); t += PTR; }
            if (tk != ')') fatal("bad cast");
            next();
            expr(Inc); // cast has precedence as Inc(++)
            {
                int srcty = ty;
                ty = t;
                /* Narrow integer casts need explicit truncation; float
                 * conversions call the embedded soft-float helpers.  A cast
                 * to void merely discards the value: no conversion code. */
                if (t == VOID) {
                    /* Operand AST stays for side effects only. */
                } else if (t == DOUBLE && srcty != DOUBLE) {
                    int dlea = loc - (ld += 2);
                    ASTW(dlea);
                    ASTW((srcty == FLOAT) ? F2D : I2D);
                } else if (srcty == DOUBLE && t != DOUBLE) {
                    ASTW(0); ASTW((t == FLOAT) ? D2F : D2I);
                    if (t == CHAR) { ASTW(0); ASTW(SXT8); }
                    else if (t == UCHAR) { ASTW(0); ASTW(ZXT8); }
                } else if (t == FLOAT && srcty != FLOAT) {
                    ASTW(0); ASTW((srcty == DOUBLE) ? D2F : I2F);
                } else if (srcty == FLOAT && t != FLOAT) {
                    ASTW(0); ASTW((t == DOUBLE) ? F2D : F2I);
                    if (t == CHAR) { ASTW(0); ASTW(SXT8); }
                    else if (t == UCHAR) { ASTW(0); ASTW(ZXT8); }
                } else if (t == CHAR) { ASTW(0); ASTW(SXT8); }
                else if (t == UCHAR) { ASTW(0); ASTW(ZXT8); }
            }
        } else {
            expr(Assign);
            while (tk == ',') { next(); expr(Assign); }
            if (tk != ')') fatal("close parentheses expected");
            next();
            /* `(expr)(args)` where expr yields a function pointer is a call
             * through that pointer, e.g. `(*patterns[i])(args)`. */
            if (ty == FPTR && tk == '(')
                indir_call_ast(n);
        }
        break;
    case Mul: // "*", dereferencing the pointer operation
        next();
        expr(Inc); // dereference has the same precedence as Inc(++)
        /* `*fp` denotes the function itself: keep the function-pointer value
         * so a following `(args)` can call it through indir_call_ast(). */
        if (ty == FPTR) break;
        if (ty < PTR) fatal("bad dereference");
        ty -= PTR;
        if (ty < CHAR || ty >= PTR2) fatal("unexpected type");
        ASTW(ty); ASTW(Load);
        break;
    case And: // "&", take the address operation
        /* when "token" is a variable, it takes the address first and
         * then LI/LC, so `--e` becomes the address of "a".
         */
        next(); expr(Inc);
        if (*n == Load) { n += 2; ty += PTR; }
        else if (*n != CodeAddr) fatal("bad address-of");
        /* &function is already the function's code address. */
        break;
    case '!': // "!x" is equivalent to "x == 0"
        next(); expr(Inc);
        if (*n == Num) n[1] = !n[1];
        else { ASTW(0); ASTW(Num); --n; *n = (int) (n + 3); ASTW(Eq); }
        ty = INT;
        break;
    case '~': // "~x" is equivalent to "x ^ -1"
        next(); expr(Inc);
        if (*n == Num) n[1] = ~n[1];
        else { ASTW(-1); ASTW(Num); --n; *n = (int) (n + 3); ASTW(Xor); }
        ty = INT;
        break;
    case Add:
        next(); expr(Inc); ty = INT;
        break;
    case Sub:
        next();
        expr(Inc);
        if (ty == FLOAT) { ASTW(FNeg); break; }
        if (ty == DOUBLE) {
            int dlea = loc - (ld += 2);
            ASTW(dlea); ASTW(DNEG);
            break;
        }
        if (*n == Num) n[1] = -n[1];
        else { ASTW(-1); ASTW(Num); --n; *n = (int) (n + 3); ASTW(Mul); }
        ty = INT;
        break;
    case Div:
    case Mod:
        break;
    // processing ++x and --x. x-- and x++ is handled later
    case Inc:
    case Dec:
        t = tk; next(); expr(Inc);
        if (*n != Load) fatal("bad lvalue in pre-increment");
        *n = t;
        break;
    default:
        if (tk & REENTRANT) tk ^= REENTRANT;
        else fatal("bad expression");
    }

    // "precedence climbing" or "Top Down Operator Precedence" method
    while (tk >= lev) {
        // tk is ASCII code will not exceed `Num=128`. Its value may be changed
        // during recursion, so back up currently processed expression type
        t = ty; b = n;
        switch (tk) {
        case Assign:
            next();
            // the left part is processed by the variable part of `tk=ID`
            // and pushes the address
            if (*n != Load) fatal("bad lvalue in assignment");
            {
                int lhs_is_field = (*(b + 1) == FIELD);
                // get the value of the right part `expr` as the result of `a=expr`
                expr(Assign);
                /* C90 assignment converts the value to the left operand's
                 * type; fcc needs explicit IR for float/int mixing. */
                if (t == DOUBLE && ty != DOUBLE) {
                    int dlea = loc - (ld += 2);
                    ASTW(dlea); ASTW((ty == FLOAT) ? F2D : I2D);
                } else if (ty == DOUBLE && t != DOUBLE) {
                    ASTW(0); ASTW((t == FLOAT) ? D2F : D2I);
                } else if (t == FLOAT && ty != FLOAT) { ASTW(0); ASTW(I2F); }
                else if (ty == FLOAT && t != FLOAT) { ASTW(0); ASTW(F2I); }
                ASTW((int) (b + 2));
                ASTW(lhs_is_field ? FIELD : t);
                ty = t;                 /* expression result is the field type */
                ASTW(Assign);
            }
            break;
        case  OrAssign: // right associated
        case XorAssign:
        case AndAssign:
        case ShlAssign:
        case ShrAssign:
        case AddAssign:
        case SubAssign:
        case MulAssign:
        case DivAssign:
        case ModAssign:
            *--n=';'; ASTW(t); ASTW(Load);
            if (tk < ShlAssign) tk = Or + (tk - OrAssign);
            else tk = Shl + (tk - ShlAssign);
            tk |= REENTRANT; compound = 1; expr(Assign);
            ASTW((int) (b + 2)); ASTW(ty = t); ASTW(Assign);
            break;
        case Cond: // `x?a:b` is similar to if except that it relies on else
            next(); expr(Assign);
            if (tk != ':') fatal("conditional missing colon");
            next(); c = n;
            expr(Cond); --n;
            *n = (int) (n + 1); ASTW((int) c); ASTW((int) b); ASTW(Cond);
            break;
        case Lor: // short circuit, the logical or
            next(); expr(Lan);
            if (*n == Num && *b == Num) n[1] = b[1] || n[1];
            else { ASTW((int) b); ASTW(Lor); }
            ty = INT;
            break;
        case Lan: // short circuit, logic and
            next(); expr(Or);
            if (*n == Num && *b == Num) n[1] = b[1] && n[1];
            else { ASTW((int) b); ASTW(Lan); }
            ty = INT;
            break;
        case Or: // push the current value, calculate the right value
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Xor);
            if (*n == Num && *b == Num) n[1] = b[1] | n[1];
            else { ASTW((int) b); ASTW(Or); }
            ty = INT;
            break;
        case Xor:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(And);
            if (*n == Num && *b == Num) n[1] = b[1] ^ n[1];
            else { ASTW((int) b); ASTW(Xor); }
            ty = INT;
            break;
        case And:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Eq);
            if (*n == Num && *b == Num) n[1] = b[1] & n[1];
            else { ASTW((int) b); ASTW(And); }
            ty = INT;
            break;
        case Eq:
            next(); expr(Lt);
            if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DEQ, t, ty); ty = INT; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FEQ, t, ty); ty = INT; break; }
            if (*n == Num && *b == Num) n[1] = b[1] == n[1];
            else { ASTW((int) b); ASTW(Eq); }
            ty = INT;
            break;
        case Ne:
            next(); expr(Lt);
            if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DNE, t, ty); ty = INT; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FNE, t, ty); ty = INT; break; }
            if (*n == Num && *b == Num) n[1] = b[1] != n[1];
            else { ASTW((int) b); ASTW(Ne); }
            ty = INT;
            break;
        case Lt:
            { int lty = ty;
              next(); expr(Shl);
              if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DLT, t, ty); ty = INT; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FLT, t, ty); ty = INT; break; }
              if (*n == Num && *b == Num) n[1] = b[1] < n[1];
              else { ASTW((int) b); ASTW(((lty == UINT || ty == UINT) ? ULTok : Lt)); }
              ty = INT; }
            break;
        case Gt:
            { int lty = ty;
              next(); expr(Shl);
              if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DGT, t, ty); ty = INT; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FGT, t, ty); ty = INT; break; }
              if (*n == Num && *b == Num) n[1] = b[1] > n[1];
              else { ASTW((int) b); ASTW(((lty == UINT || ty == UINT) ? UGTok : Gt)); }
              ty = INT; }
            break;
        case Le:
            { int lty = ty;
              next(); expr(Shl);
              if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DLE, t, ty); ty = INT; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FLE, t, ty); ty = INT; break; }
              if (*n == Num && *b == Num) n[1] = b[1] <= n[1];
              else { ASTW((int) b); ASTW(((lty == UINT || ty == UINT) ? ULEtok : Le)); }
              ty = INT; }
            break;
        case Ge:
            { int lty = ty;
              next(); expr(Shl);
              if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DGE, t, ty); ty = INT; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FGE, t, ty); ty = INT; break; }
              if (*n == Num && *b == Num) n[1] = b[1] >= n[1];
              else { ASTW((int) b); ASTW(((lty == UINT || ty == UINT) ? UGEtok : Ge)); }
              ty = INT; }
            break;
        case Shl:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Add);
            if (*n == Num && *b == Num) {
                n[1] = b[1] << n[1];
            } else { ASTW((int) b); ASTW(Shl); }
            ty = INT;
            break;
        case Shr:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Add);
            if (*n == Num && *b == Num) {
                n[1] = b[1] >> n[1];
            } else {
                ASTW((int) b);
                ASTW((t == UINT || ty == UINT) ? LShrTok : Shr);
            }
            ty = INT;
            break;
        case Add:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Mul);
            if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DADD, t, ty); ty = DOUBLE; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FADD, t, ty); ty = FLOAT; break; }
            tc = ((t | ty) & (PTR | PTR2)) ? (t >= PTR) : (t >= ty);
            c = n; if (tc) ty = t;
            sz = ty >= PTR ? ty_step(ty) : 1;
            if (*n == Num && tc) { n[1] *= sz; sz = 1; }
            else if (*b == Num && !tc) { b[1] *= sz; sz = 1; }
            if (*n == Num && *b == Num) n[1] += b[1];
            else if (sz != 1) {
                ASTW(sz); ASTW(Num);
                ASTW((int) (tc ? c : b)); ASTW(Mul);
                ASTW((int) (tc ? b : c)); ASTW(Add);
            }
            else { ASTW((int) b); ASTW(Add); }
            break;
        case Sub: // 4 cases: ptr-ptr, ptr-int, int-ptr (err), int-int
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Mul); // t = left type, ty = right type
            if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DSUB, t, ty); ty = DOUBLE; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FSUB, t, ty); ty = FLOAT; break; }
            if (t < PTR && ty >= PTR) fatal("bad pointer subtraction");
            if (t >= PTR) { // left arg is ptr
                sz = ty_step(t);
                if (ty >= PTR) { // ptr - ptr
                    if (t != ty) fatal("mismatched ptr type subtraction");
                    if (*n == Num && *b == Num) n[1] = (b[1] - n[1]) / sz;
                    else {
                        ASTW((int) b); ASTW(Sub);
                        if (sz > 1) {
                            if ((sz & (sz - 1)) == 0) { // 2^n
                                ASTW(popcount(sz - 1)); ASTW(Num);
                                --n; *n = (int) (n + 3); ASTW(Shr);
                            } else {
                                ASTW(sz); ASTW(Num); --n; *n = (int) (n + 3);
                                ASTW(Div); ef_getidx("__aeabi_idiv");
                            }
                        }
                    }
                    ty = INT;
                } else { // ptr - int
                    if (*n == Num) {
                        n[1] *= sz;
                        if (*b == Num) n[1] = b[1] - n[1];
                        else { ASTW((int) b); ASTW(Sub); }
                    } else {
                        if (sz > 1) {
                            if ((sz & (sz - 1)) == 0) { // 2^n
                                ASTW(popcount(sz - 1)); ASTW(Num);
                                --n; *n = (int) (n + 3); ASTW(Shl);
                            }
                            else {
                                ASTW(sz); ASTW(Num);
                                --n; *n = (int) (n + 3); ASTW(Mul);
                            }
                        }
                        ASTW((int) b); ASTW(Sub);
                    }
                    ty = t;
                }
            } else { // int - int
                if (*n == Num && *b == Num) n[1] = b[1] - n[1];
                else { ASTW((int) b); ASTW(Sub); }
                ty = INT;
            }
            break;
        case Mul:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Inc);
            if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DMUL, t, ty); ty = DOUBLE; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FMUL, t, ty); ty = FLOAT; break; }
            if (*n == Num && *b == Num) n[1] *= b[1];
            else {
                ASTW((int) b);
                if (n[1] == Num && n[2] > 0 && (n[2] & (n[2] - 1)) == 0) {
                    n[2] = popcount(n[2] - 1); ASTW(Shl); // 2^n
                }
                else ASTW(Mul);
            }
            ty = INT;
            break;
        case Inc:
        case Dec:
            sz = ty >= PTR ? ty_step(ty) : 1;
            if (*n != Load) fatal("bad lvalue in post-increment");
            *n = tk;
            ASTW(sz); ASTW(Num);
            ASTW((int) b); ASTW((tk == Inc) ? Sub : Add);
            next();
            break;
        case Div:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Inc);
            if (t == DOUBLE || ty == DOUBLE) { dbl_emit_bin(b, DDIV, t, ty); ty = DOUBLE; break; }
            if (t == FLOAT || ty == FLOAT) { fp_emit_bin(b, FDIV, t, ty); ty = FLOAT; break; }
            if (*n == Num && *b == Num) n[1] = b[1] / n[1];
            else {
                ASTW((int) b);
                if (n[1] == Num && n[2] > 0 && (n[2] & (n[2] - 1)) == 0) {
                    n[2] = popcount(n[2] - 1);
                    ASTW((t == UINT || ty == UINT) ? LShrTok : Shr); // 2^n
                } else if (t == UINT || ty == UINT) {
                    ASTW(UDivTok);
                    ef_getidx("__aeabi_uidiv");
                } else {
                    ASTW(Div);
                    ef_getidx("__aeabi_idiv");
                }
            }
            ty = INT;
            break;
        case Mod:
            next();
            if (compound) { compound = 0; expr(Assign); }
            else expr(Inc);
            if (*n == Num && *b == Num) n[1] = b[1] % n[1];
            else {
                ASTW((int) b);
                if (n[1] == Num && n[2] > 0 && (n[2] & (n[2] - 1)) == 0) {
                    --n[2]; ASTW(And); // 2^n
                } else if (t == UINT || ty == UINT) {
                    ASTW(UModTok);
                    ef_getidx("__aeabi_uidivmod");
                } else {
                    ASTW(Mod);
                    ef_getidx("__aeabi_idivmod");
                }
            }
            ty = INT;
            break;
        case Dot:
            ty += PTR;
            if (n[0] == Load && IS_STRUCT(n[1])) n += 2; // struct
        case Arrow:
            if (ty <= PTR+INT || ty >= PTR2) fatal("structure expected");
            next();
            if (tk != Id) fatal("structure member expected");
            m = members[ty - PTR]; while (m && m->id != id) m = m->next;
            if (!m) fatal("structure member not found");
            if (m->offset) {
                ASTW(m->offset); ASTW(Num); --n; *n = (int) (n + 3);
                ASTW(Add);
            }
            ty = m->type;
            if (m->bitwid) {
                /* Bit-field rvalue: address of the storage unit is the child
                 * at n+5; n[2..4] hold bit offset, width and signedness. */
                ASTW((ty == UINT || ty == UCHAR) ? 0 : 1);
                ASTW(m->bitwid);
                ASTW(m->bitoff);
                ASTW(FIELD);
                ASTW(Load);
            } else {
                if (IS_BASIC(ty) || ty >= PTR)
                    ASTW(((ty == CHAR || ty == UCHAR) ? ty : (ty == DOUBLE ? DOUBLE : INT)));
                else ASTW(ty); // struct, not struct pointer
                ASTW(Load);
            }
            next();
            break;
        case Bracket:
            next(); expr(Assign);
            if (tk != ']') fatal("close bracket expected");
            next();
            /* The subscripted expression may still be an array type (a
             * sub-array); decay it to a pointer to its first element. */
            if (IS_ARRAY(t)) t = ty_decay(t);
            if (t < PTR) fatal("pointer type expected");
            ty = t - PTR;               /* element type */
            sz = ty_size(ty);           /* element stride */
            if (sz > 1) {
                if (*n == Num) n[1] *= sz;
                else {
                    ASTW(sz); ASTW(Num); --n; *n = (int) (n + 3); ASTW(Mul);
                }
            }
            if ((*n == Num || *n == GlbAddr) &&
                (*b == Num || *b == GlbAddr)) {
                n[1] += b[1];
                /* Keep the data-address marker so gen() emits DATAPTR, not a
                 * raw immediate (the rewrite no longer guesses by range). */
                if (*b == GlbAddr) *n = GlbAddr;
            }
            else { ASTW((int) b); ASTW(Add); }
            if (IS_ARRAY(ty)) {
                /* Sub-array lvalue: keep its address, the following subscript
                 * (or decay) will turn it into a pointer. */
                ty = ty_decay(ty);
            } else {
                if (IS_BASIC(ty) || ty >= PTR)
                    ASTW(((ty == CHAR || ty == UCHAR) ? ty : (ty == DOUBLE ? DOUBLE : INT)));
                else ASTW(ty);         /* struct */
                ASTW(Load);
            }
            /* A subscript may itself yield a function pointer: allow the
             * direct call form `patterns[i](args)` as well as an assignment. */
            if (ty == FPTR && tk == '(')
                indir_call_ast(n);
            break;
        default:
            printf("%d: compiler error tk=%d\n", line, tk); exit(-1);
        }
    }
}

/* ---- Function-pointer helpers ------------------------------------------
 * fcc has no first-class function type: a pointer to function is a PTR-range
 * type (FPTR) holding the address of the function's first IR instruction.
 * Taking the address emits CODEADDR; a call through it emits IndirFunc. */

#define FPTR_PARAM_MAX 256

static int is_fptr_ident(struct ident_s *d)
{
    return (d->class == Loc || d->class == Par || d->class == Glo ||
            d->class == Stc) && d->type == FPTR;
}

/* Reserve a fixup slot for a function address not yet known.  The identifier
 * is patched after every definition has been parsed (in main()).  d->val is
 * deliberately left untouched: a direct call must still see the original
 * forward-declaration marker and allocate its own independent slot. */
static int code_fixup(struct ident_s *d)
{
    if (d->val > 0) return d->val;
    if (d->val == 0) fatal("address of undefined function");
    if (nfwd_calls >= FWD_CALL_MAX) fatal("too many forward function references");
    fwd_ident[nfwd_calls] = d;
    /* An address fixup is never a syscall; clear the flag so a stale entry
     * from an earlier fixup cannot redirect it through the svc path. */
    fwd_efidx[nfwd_calls] = -1;
    return -(++nfwd_calls);
}

/* Parse `(*name[dims])(param-types)` when tk is '('.  On a match, store the
 * name in *out, the array dimensions in dims[] with their count in *ndims,
 * and leave tk just past the parameter list's ')'.  Otherwise restore the
 * lexer state and return 0.  Parameter types are consumed syntactically;
 * the backend uses the caller's argument types. */
static int fptr_declarator(struct ident_s **out, int *dims, int *ndims)
{
    char *save_p;
    int save_tk, save_iv, save_line;
    struct ident_s *save_id;
    int depth = 1;
    int guard = 0;

    save_p = p; save_tk = tk; save_iv = ival;
    save_id = id; save_line = line;
    *ndims = 0;
    next();
    if (tk != Mul) {
        p = save_p; tk = save_tk; ival = save_iv;
        id = save_id; line = save_line;
        return 0;
    }
    next();
    if (tk != Id) fatal("bad function pointer declaration");
    *out = id;
    next();
    /* An array of function pointers places its [N] suffixes inside the
     * declarator, between `*name` and the `)(params)` that follows. */
    while (tk == Bracket) {
        int dim = 0;
        next();
        if (tk != ']') {
            if (tk != Num) fatal("bad function pointer array size");
            dim = ival;
            next();
        }
        if (tk != ']') fatal("bad function pointer declaration");
        next();
        if (*ndims >= 16) fatal("too many function pointer array dimensions");
        dims[(*ndims)++] = dim;
    }
    if (tk != ')') fatal("bad function pointer declaration");
    next();
    if (tk != '(') fatal("bad function pointer declaration");
    next();
    while (depth > 0) {
        if (guard++ >= FPTR_PARAM_MAX)
            fatal("function pointer parameter list too long");
        if (!tk) fatal("bad function pointer declaration");
        if (tk == '(') depth++;
        else if (tk == ')') depth--;
        next();
    }
    return 1;
}

/* Parse the argument list of an indirect call.  tk is the '(' of the call and
 * callee is the AST node (already on the stack) evaluating to the target
 * address.  The resulting AST is [IndirFunc, args, words, callee-AST]; gen()
 * pushes the arguments, loads the target into r0 and emits JSRI. */
static void indir_call_ast(int *callee)
{
    int *args = 0;
    int nwords = 0;
    int aty;

    next();                              /* consume '(' */
    while (tk != ')') {
        expr(Assign);
        aty = ty;
        ASTW(aty); ASTW((int) args); args = n;
        nwords += IS_STRUCT(aty) ? (ty_size(aty) + 3) / 4 : 1;
        if (tk == ',') {
            next();
            if (tk == ')') fatal("unexpected comma in function call");
        } else if (tk != ')') fatal("missing comma in function call");
    }
    next();                              /* consume ')' */
    ASTW((int) callee);
    ASTW(nwords);
    ASTW((int) args);
    ASTW(IndirFunc);
    ty = INT;                            /* return type is not encoded */
}

/* Parse a call through the function-pointer variable d.  Build the callee
 * node first (an ordinary variable load), then delegate to indir_call_ast(). */
static void indir_call(struct ident_s *d)
{
    int *callee;

    switch (d->class) {                  /* r0 = function address */
    case Loc: case Par:
        ASTW(loc - d->val); ASTW(Loc); break;
    case Glo: case Stc:
        ASTW(d->val); ASTW(Num); break;
    default: fatal("undefined variable");
    }
    ASTW(FPTR); ASTW(Load);
    callee = n;
    indir_call_ast(callee);
}

// AST parsing for IR generation
// With a modular code generator, new targets can be easily supported such as
// native Arm machine code.
/* Emit a floating binary AST node: [op, left-type, right-type, left-ptr,
 * right-AST...].  gen() applies I2F conversions to integer operands. */
static void fp_emit_bin(int *b, int op, int lty, int rty)
{
    ASTW((int) b);
    ASTW(rty);
    ASTW(lty);
    ASTW(op);
}

/* Emit a double binary AST node:
 *   [op, lty, rty, left-ptr, result-slot, left-temp, right-temp, right...]
 * Non-double operands get 2-word scratch slots for the implicit conversion. */
static void dbl_emit_bin(int *b, int op, int lty, int rty)
{
    int dlea = loc - (ld += 2);
    int ltemp = (lty == DOUBLE) ? 0 : (loc - (ld += 2));
    int rtemp = (rty == DOUBLE) ? 0 : (loc - (ld += 2));
    ASTW(rtemp);
    ASTW(ltemp);
    ASTW(dlea);
    ASTW((int) b);
    ASTW(rty);
    ASTW(lty);
    ASTW(op);
}

/* Statement sequences are built as nested '{' AST nodes: each statement
 * wraps the previous one, so a function with N statements produces a chain
 * N deep.  Recursing once per node (the old `case '{': gen(n[1]); gen(n+2)`)
 * with gen()'s ~300-byte frame (argp[64]) overflowed the 7 KB process stack
 * at ~150-200 statements, corrupting the heap and faulting in ty_size().
 * Handle the sequence iteratively: loop over the chain and call gen1() for
 * each individual statement.  gen1() keeps the large frame; the loop lives
 * in the small gen() wrapper. */
static void gen1(int *n);   /* forward decl: iterative wrapper calls gen1 */

void gen(int *n)
{
    while (n && *n == '{') {
        gen1((int *) n[1]);     /* the statement stored in this node */
        n = n + 2;              /* continue with the rest of the sequence */
    }
    if (n)
        gen1(n);
}

static void gen1(int *n)
{
    int i = *n, j, k, l;
    int *a, *argp[MAX_CALL_ARGS], *b, *c, *d, *t;
    struct ident_s *label;

    switch (i) {
    case Num: // get the value of integer
        IRW(IMM); IRW(n[1]);
        break;
    case FNum:                       /* IEEE-754 bit pattern as immediate */
        IRW(IMM); IRW(n[1]);
        break;
    case DNum:                       /* address of 8-byte double literal */
        IRW(DATAPTR); IRW(n[1]);
        break;
    case GlbAddr:                    /* absolute address of a global/string */
        /* Convert to a data-pool offset here, where the AST still tells us
         * this is a real data address.  The IR rewrite no longer guesses by
         * numeric range, so ordinary integer literals are never rewritten. */
        IRW(DATAPTR); IRW(n[1] - (int) freedata);
        break;
    case FNeg:                       /* 0.0f - x; child is at n+1 */
        IRW(IMM); IRW(0); IRW(PSH);
        gen(n + 1);
        IRW(FSUB);
        break;
    case Loc: // get the value of variable
        IRW(LEA); IRW(n[1]);
        break;
    case Label: // target of goto
        label = (struct ident_s *) n[1];
        if (label->class != 0) fatal("duplicate label definition");
        d = e + 1; b = (int *) label->val;
        while (b != 0) { t = (int *) *b; *b = (int) d; b = t; }
        label->val = (int) d; label->class = Label;
        break;
    case SXT8:
    case ZXT8:
    case F2I:
    case I2F:
        gen(n + 2); IRW(i);       /* integer/float conversion */
        break;
    case Load:
        if (n[1] == FIELD) {
            /* Extract a bit-field with shifts/masks built from existing IR
             * opcodes: value = (word >> bitoff) & mask, sign-extended. */
            int bitoff = n[2], width = n[3], flags = n[4];
            int mask = (width >= 32) ? -1 : (1 << width) - 1;
            gen(n + 5);                 /* address of the storage unit */
            IRW(LI);                  /* r0 = the whole word */
            IRW(PSH); IRW(IMM); IRW(bitoff); IRW(LSR);
            IRW(PSH); IRW(IMM); IRW(mask);   IRW(AND);
            if (flags && width < 32) {
                IRW(PSH); IRW(IMM); IRW((32 - width)); IRW(SHL);
                IRW(PSH); IRW(IMM); IRW((32 - width)); IRW(SHR);
            }
        } else {
            gen(n + 2); // load the value
            /* Struct and double rvalues are represented by their address. */
            if (IS_STRUCT(n[1]) || n[1] == DOUBLE) break;
            IRW((n[1] == CHAR) ? LC : (n[1] == UCHAR) ? LCU : LI);
        }
        break;
    case Assign: // assign the value to variables
        if (n[1] == FIELD) {
            /* Read-modify-write one bit-field: clear the old bits, OR in the
             * masked new value, store the containing word back. */
            int *meta = (int *) n[2];   /* [bitoff, width, flags] */
            int bitoff = meta[0], width = meta[1];
            int mask = (width >= 32) ? -1 : (1 << width) - 1;
            gen(meta + 3);              /* address of the storage unit */
            IRW(PSH); IRW(LI);      /* keep old word */
            IRW(PSH);                 /* stack: addr, old */
            IRW(IMM); IRW(mask); IRW(PSH);
            IRW(IMM); IRW(bitoff); IRW(SHL);   /* mask << bitoff */
            IRW(PSH); IRW(IMM); IRW(-1); IRW(XOR); /* ~fieldmask */
            IRW(AND);                 /* old & ~fieldmask */
            IRW(PSH);                 /* stack: addr, cleared */
            gen(n + 3);                 /* new value */
            IRW(PSH); IRW(IMM); IRW(mask); IRW(AND);
            IRW(PSH); IRW(IMM); IRW(bitoff); IRW(SHL);
            IRW(OR);                  /* cleared | (new & mask) << bitoff */
            IRW(SI);
        } else {
            gen((int *) n[2]); IRW(PSH); gen(n + 3);
            if (n[1] == DOUBLE) { IRW(SCPY); IRW(2); }
            else if (IS_STRUCT(n[1])) {
                int words = (ty_size(n[1]) + 3) / 4;
                if (words < 1 || words > 64)
                    fatal("struct too large for value copy");
                IRW(SCPY); IRW(words);
            } else {
                IRW(((n[1] == CHAR || n[1] == UCHAR)) ? SC : SI);
            }
        }
        break;
    // increment or decrement variables
    case Inc:
    case Dec:
        gen(n + 2);
        IRW(PSH);
        IRW((n[1] == CHAR) ? LC : (n[1] == UCHAR) ? LCU : LI);
        IRW(PSH);
        IRW(IMM); IRW(n[1] >= PTR ? ty_step(n[1]) : 1);
        IRW((i == Inc) ? ADD : SUB);
        IRW(((n[1] == CHAR || n[1] == UCHAR)) ? SC : SI);
        break;
    case Cond: // if else condition case
        gen((int *) n[1]); // condition
        // Add jump-if-zero instruction "BZ" to jump to false branch.
        // Point "b" to the jump address field to be patched later.
        IRW(BZ); need_ir(1); b = ++e;
        gen((int *) n[2]); // expression
        // Patch the jump address field pointed to by "b" to hold the address
        // of false branch. "+ 3" counts the "JMP" instruction added below.
        //
        // Add "JMP" instruction after true branch to jump over false branch.
        // Point "b" to the jump address field to be patched later.
        if (n[3]) {
            *b = (int) (e + 3); IRW(JMP); need_ir(1); b = ++e; gen((int *) n[3]);
        } // else statement
        // Patch the jump address field pointed to by "d" to hold the address
        // past the false branch.
        *b = (int) (e + 1);
        break;
    // operators
    /* If current token is logical OR operator:
     * Add jump-if-nonzero instruction "BNZ" to implement short circuit.
     * Point "b" to the jump address field to be patched later.
     * Parse RHS expression.
     * Patch the jump address field pointed to by "b" to hold the address past
     * the RHS expression.
     */
    case Lor:  gen((int *) n[1]); IRW(BNZ);
               need_ir(1); b = ++e; gen(n + 2); *b = (int) (e + 1); break;
    case Lan:  gen((int *) n[1]); IRW(BZ);
               need_ir(1); b = ++e; gen(n + 2); *b = (int) (e + 1); break;
    /* If current token is bitwise OR operator:
     * Add "PSH" instruction to push LHS value in register to stack.
     * Parse RHS expression.
     * Add "OR" instruction to compute the result.
     */
    case Or:   gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(OR); break;
    case Xor:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(XOR); break;
    case And:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(AND); break;
    case Eq:   gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(EQ); break;
    case Ne:   gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(NE); break;
    case ULTok: gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(ULT); break;
    case Lt:   gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(LT); break;
    case UGTok: gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(UGT); break;
    case Gt:   gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(GT); break;
    case ULEtok: gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(ULE); break;
    case Le:   gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(LE); break;
    case UGEtok: gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(UGE); break;
    case Ge:   gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(GE); break;
    case Shl:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(SHL); break;
    case Shr:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(SHR); break;
    case LShrTok: gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(LSR); break;
    case Add:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(ADD); break;
    case Sub:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(SUB); break;
    case Mul:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(MUL); break;
    case Div:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(DIV); break;
    case Mod:  gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(MOD); break;
    case UDivTok: gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(UDIV); break;
    case UModTok: gen((int *) n[1]); IRW(PSH); gen(n + 2); IRW(UMOD); break;
    case FADD: case FSUB: case FMUL: case FDIV:
    case FLT: case FGT: case FLE: case FGE: case FEQ: case FNE:
        gen((int *) n[3]);
        if (n[1] != FLOAT) IRW(I2F);
        IRW(PSH);
        gen(n + 4);
        if (n[2] != FLOAT) IRW(I2F);
        IRW(i);
        break;
    case DADD: case DSUB: case DMUL: case DDIV:
    case DLT: case DGT: case DLE: case DGE: case DEQ: case DNE:
        gen((int *) n[3]);                    /* left operand value/address */
        if (n[1] != DOUBLE) {                 /* implicit int/float -> double */
            IRW(PSH);
            IRW(LEA); IRW(n[5]);           /* r0 = conversion scratch */
            IRW((n[1] == FLOAT) ? F2D : I2D);
        }
        IRW(PSH);                            /* left double address */
        gen(n + 7);                            /* right operand value/address */
        if (n[2] != DOUBLE) {
            IRW(PSH);
            IRW(LEA); IRW(n[6]);           /* r0 = conversion scratch */
            IRW((n[2] == FLOAT) ? F2D : I2D);
        }
        IRW(PSH);                            /* right double address */
        IRW(LEA); IRW(n[4]);               /* r0 = result scratch */
        IRW(i);                              /* pop r2/r1, call helper */
        break;
    case D2I: case D2F:
        gen(n + 2);                           /* operand address */
        IRW(i);
        break;
    case I2D: case F2D: case DNEG:
        gen(n + 2); IRW(PSH);               /* value / operand address */
        IRW(LEA); IRW(n[1]);              /* r0 = result scratch */
        IRW(i);                             /* pop r1, call helper */
        break;
    case CodeAddr:
        /* Function designator used as a value: materialise its address.
         * A negative operand is a forward-reference fixup slot, as in JSR. */
        IRW(CODEADDR);
        IRW(n[1]);
        break;
    case IndirFunc:
        /* Same argument push sequence as Func, then load the target
         * address into r0 and jump through it with JSRI. */
        c = b = (int *) n[1]; k = 0; l = 1;
        while (b && l) { ++k; if (!(int *) *b) l = 0; else b = (int *) *b; }
        j = 0; a = malloc(sizeof(int *) * k); b = c; l = 1;
        while (b && l) {
            a[j] = (int) b;
            if (!(int *) *b) l = 0; else b = (int *) *b; ++j;
        }
        if (j > 0) --j;
        while (j >= 0 && k > 0) {
            int aty = b[1];
            gen(b + 2);
            if (IS_STRUCT(aty)) {
                IRW(PUSHS); IRW((ty_size(aty) + 3) / 4);
            } else {
                IRW(PSH);
            }
            --j; b = (int *) a[j];
        }
        free(a);
        gen((int *) n[3]);                  /* r0 = function address */
        IRW(JSRI);
        if (n[2]) { IRW(ADJ); IRW(n[2]); }
        break;
    case StructFunc:
        /* n[4] holds the LEA operand of the caller's result temporary. */
        IRW(LEA); IRW(n[4]); IRW(PSH);   /* keep the result address */
        /* fall through */
    case Func:
    case Syscall:
    case Extern:
    case ClearCache:
        c = b = (int *) n[1]; k = 0; l = 1;
        /* Count arguments with a fixed upper bound (no unbounded malloc). */
        while (b && l) {
            if (k >= MAX_CALL_ARGS) fatal("too many call arguments");
            ++k;
            if (!(int *) *b) l = 0; else b = (int *) *b;
        }
        j = 0; b = c; l = 1;
        while (b && l) {
            argp[j] = b;
            if (!(int *) *b) l = 0; else b = (int *) *b;
            ++j;
        }
        if (j > 0) --j;
        /* Push parameters right-to-left.  BUG D: never read a[j] when j has
         * become negative; the previous `b = a[-1]` was an out-of-bounds read
         * on every call that had arguments. */
        while (j >= 0 && k > 0) {
            int aty = b[1];             /* arg chain: [prev, type, AST...] */
            gen(b + 2);
            if (IS_STRUCT(aty)) {
                IRW(PUSHS); IRW((ty_size(aty) + 3) / 4);
            } else {
                IRW(PSH);
            }
            --j;
            if (j >= 0) b = argp[j];
        }
        /* A negative operand means a patchable call slot.  Func prototypes
         * and implicit syscall calls both use it.  Extern calls need no
         * slot: they branch through a null sentinel (see below). */
        if (i == Extern) {
            IRW(IMM); IRW(0); IRW(JSRI);
        } else {
            if (i == Syscall && n[2] < 0) {
                IRW(JSR);
            } else {
                if (i == Syscall) IRW(SYSC);
                if (i == Func || i == StructFunc) {
                    IRW(JSR);
                }
            }
            IRW(n[2]);
        }
        if (n[3]) { IRW(ADJ); IRW(n[3]); }
        if (i == StructFunc) {
            /* Pop the saved result address into r0 (IMM 0; ADD = r1 + 0). */
            IRW(IMM); IRW(0); IRW(ADD);
        }
        break;
    case While:
    case DoWhile:
        if (i == While) { IRW(JMP); need_ir(1); a = ++e; }
        d = (e + 1);
        b = brks; brks = 0;
        c = cnts; cnts = 0;
        gen((int *) n[1]); // loop body
        if (i == While) *a = (int) (e + 1);
        while (cnts) { t = (int *) *cnts; *cnts = (int) (e + 1); cnts = t; }
        cnts = c;
        gen((int *) n[2]); // condition
        IRW(BNZ); IRW((int) d);
        while (brks) { t = (int *) *brks; *brks = (int) (e + 1); brks = t; }
        brks = b;
        break;
    case For:
        gen((int *) n[4]);  // init
        IRW(JMP); need_ir(1); a = ++e;
        d = (e + 1);  
        b = brks; brks = 0;
        c = cnts; cnts = 0;
        gen((int *) n[3]); // loop body
        while (cnts) { t = (int *) *cnts; *cnts = (int) (e + 1); cnts = t; }
        cnts = c;
        gen((int *) n[2]); // increment
        *a = (int) (e + 1);
        gen((int *) n[1]); // condition
        IRW(BNZ); IRW((int) d);
        while (brks) { t = (int *) *brks; *brks = (int) (e + 1); brks = t; }
        brks = b;
        break;
    case Switch:
        gen((int *) n[1]); // condition
        a = cas; IRW(JMP); need_ir(1); cas = ++e;
        b = brks; d = def; brks = def = 0;
        gen((int *) n[2]); // case statement
        // deal with no default inside switch case
        *cas = def ? (int) def : (int) (e + 1); cas = a;
        while (brks) { t = (int *) * brks; *brks = (int) (e + 1); brks = t; }
        brks = b; def = d;
        break;
    case Case:
        IRW(JMP); need_ir(1); ++e;
        a = 0;
        *e = (int) (e + 7); IRW(PSH); i = *cas; *cas = (int) e;
        gen((int *) n[1]); // condition
        if (e[-1] != IMM) fatal("bad case immediate");
        IRW(SUB); IRW(BNZ); need_ir(1); cas = ++e; *e = i + e[-3];
        if (*(int *) n[2] == Switch) a = cas;
        gen((int *) n[2]); // expression
        if (a != 0) cas = a;
        break;
    case Break:
        // set jump locate
        IRW(JMP); IRW((int) brks); brks = e;
        break;
    case Continue:
        // set jump locate
        IRW(JMP); IRW((int) cnts); cnts = e;
        break;
    case Goto:
        label = (struct ident_s *) n[1];
        IRW(JMP); IRW(label->val);
        if (label->class == 0) label->val = (int) e; // Define label address later
        break;
    case Default:
        def = e + 1;
        gen((int *) n[1]); break;
    case Return:
        if (retstruct) {
            /* Copy the returned struct into the caller's hidden destination. */
            if (n[1]) {
                IRW(LEA); IRW(retlea); IRW(LI); IRW(PSH); /* dest */
                gen((int *) n[1]);            /* r0 = source struct address */
                IRW(SCPY); IRW(retwords);
            }
            IRW(LEV);
        } else {
            if (n[1]) gen((int *) n[1]); IRW(LEV); // parse return AST
        }
        break;
    case '{':
        /* Nested block/sequence: use the iterative gen() wrapper so long
         * statement chains do not recurse here (stack overflow). */
        gen((int *) n[1]); gen(n + 2); break;
    case Enter: IRW(ENT); IRW(n[1]); gen(n + 2);
                if (*e != LEV) IRW(LEV); break;
    default:
        if (i != ';') {
            printf("%d: compiler error gen=%d\n", line, i); exit(-1);
        }
    }
}

void check_label(int **tt)
{
    if (tk != Id) return;
    char *ss = p;
    while (*ss == ' ' || *ss == '\t') ++ss;
    if (*ss == ':') {
        if (id->class != 0 || !(id->type == 0 || id->type == -1))
            fatal("invalid label");
        id->type = -1 ; // hack for id->class deficiency
        ASTW((int) id); ASTW(Label);
        ASTW((int) *tt); ASTW('{'); *tt = n;
        next(); next();
    }
}

/* Restore the identifier entries created while parsing a parameter list to
 * the class/type/value they held beforehand.  A forward declaration parses
 * its parameters only to register the function name; the temporary Par
 * entries must be discarded so they cannot leak into the next declaration.
 * No diagnostics are emitted here: a prototype's parameter names are not
 * definitions and an "unused" warning for them would be misleading. */
static void unwind_parms(void)
{
    struct ident_s *d;
    for (d = sym; d->tk; d++) {
        if (d->class == Par) {
            d->class = d->hclass;
            d->type = d->htype;
            d->val = d->hval;
        }
    }
}

/* Compile-time initializer support for file-scope objects.
 *
 * C90 requires initializers for objects with static storage duration to be
 * constant expressions.  expr() already constant-folds integer constant
 * expressions into a single Num node, so we parse the initializer with expr()
 * and write the folded value directly into the data-pool image at `dst`.
 * This is the only place a global's initial value can be stored: there is no
 * C runtime to run startup constructors, and the data pool is appended
 * verbatim to the flat binary by the backend.
 *
 * `ty` is the declared type of the object (possibly an array).  On entry tk
 * is the first token after '{' (when braced) and on return tk is the token
 * that terminated the list ('}' or a ',' handled by the caller). */
static void glo_init_list(int ty, char *dst)
{
    int ety, nelem, esz, idx, k;
    int *a;
    char *q;

    if (IS_ARRAY(ty)) {
        ety = atypes[ty - ARR].elem;
        nelem = atypes[ty - ARR].nelem;
    } else {
        ety = ty;
        nelem = 1;
    }
    if (nelem <= 0)
        fatal("incomplete array needs an explicit size for an initializer");
    esz = ty_size(ety);
    idx = 0;
    while (tk != '}' && tk != 0) {
        if (tk == '{') {
            if (!IS_ARRAY(ety))
                fatal("too many braces in initializer");
            next();
            glo_init_list(ety, dst + idx * esz);
            if (tk != '}') fatal("expected '}' in initializer");
            next();
        } else {
            expr(Assign);
            a = n;                      /* root is at n after expr() */
            if (*a != Num)
                fatal("initializer must be a constant expression");
            q = dst + idx * esz;
            for (k = 0; k < esz; ++k)
                *q++ = (char) (a[1] >> (8 * k));
        }
        if (++idx > nelem)
            fatal("too many initializers");
        if (tk == ',') next();
        else if (tk != '}') fatal("expected ',' or '}' in initializer");
    }
}

// statement parsing (syntax analysis, except for declarations)
void stmt(int ctx)
{
    int *a, *b, *c, *d;
    int i, j, atk;
    int bt = INT;
    int is_static = 0;
    int dims[16], ndims;

    if (tk == Static) { is_static = 1; next(); }
    while (tk == Keyword) next();   /* extern/register/auto qualifiers */
    switch (tk) {
    case Enum:
        next();
        // If current token is not "{", it means having enum type name.
        // Skip the enum type name.
        if (tk != '{') next();
        if (tk == '{') {
            next();
            i = 0; // Enum value starts from 0
            while (tk != '}') {
                // Current token should be enum name.
                // If current token is not identifier, stop parsing.
                if (tk != Id) fatal("bad enum identifier");
                next();
                if (tk == Assign) {
                    next();
                    expr(Cond);
                    if (*n != Num) fatal("bad enum initializer");
                    i = n[1]; // Set enum value
                }
                /* "id" is pointing to the enum name's symbol table entry.
                 * Set the symbol table entry's symbol type be "Num".
                 * Set the symbol table entry's associated value type be "INT".
                 * Set the symbol table entry's associated value be enum value.
                 */
                id->class = Num; id->type = INT; id->val = i++;
                if (tk == ',') next(); // If current token is ",", skip.
            }
            next(); // Skip "}"
        } else if (tk == Id) {
            id->type = INT; id->class = ctx; id->val = ld++;
            next();
        }
        return;
    case Int:
    case Char:
    case Unsigned:
    case Float:
    case Double:
    case Struct:
    case Union:
    case Void:
        switch (tk) {
        case Float:
            bt = FLOAT; next(); break;
        case Double:
            bt = DOUBLE; next(); break;
        case Void:
            bt = VOID; next(); break;
        case Struct:
        case Union:
            atk = tk; next();
            if (tk == Id) {
                if (!id->stype) id->stype = tnew++;
                bt = id->stype;
                next();
            } else {
                bt = tnew++;
            }
            if (tk == '{') {
                int bitpos = 0;     /* next free bit in the current unit */
                tsize[bt] = 0; // for unions
                next();
                if (members[bt]) fatal("duplicate structure definition");
                i = 0;
                while (tk != '}') {
                    int mbt = INT;
                    switch (tk) {
                    case Float:
                        next(); mbt = FLOAT; break;
                    case Double:
                        next(); mbt = DOUBLE; break;
                    case Int: next(); break;
                    case Unsigned:
                        next(); mbt = UINT;
                        if (tk == Char) { next(); mbt = UCHAR; }
                        else if (tk == Int) next();
                        break;
                    case Char: next(); mbt = CHAR; break;
                    case Struct:
                    case Union:
                        next();
                        if (tk != Id) fatal("bad struct/union declaration");
                        mbt = id->stype;
                        next(); break;
                    }
                    while (tk != ';') {
                        int w = 0, isbf = 0;
                        struct member_s *m;
                        ty = mbt;
                        // If the beginning of * is a pointer type,
                        // then type plus `PTR` indicates what kind of pointer
                        while (tk == Mul) { next(); ty += PTR; }
                        if (tk != Id) fatal("bad struct member definition");
                        m = malloc(sizeof(struct member_s));
                        m->id = id;
                        m->type = ty;
                        m->bitoff = 0;
                        m->bitwid = 0;
                        m->next = members[bt];
                        members[bt] = m;
                        next();
                        if (tk == ':') {
                            next();
                            if (tk != Num) fatal("bit-field width must be constant");
                            w = ival;
                            next();
                            if (w < 0 || w > 32) fatal("bad bit-field width");
                            isbf = 1;
                        }
                        if (isbf && w == 0) {
                            /* zero-width field: align the next one */
                            if (bitpos) { i += 4; bitpos = 0; }
                            m->offset = i;
                        } else if (isbf) {
                            if (bitpos + w > 32) { i += 4; bitpos = 0; }
                            m->offset = i;
                            m->bitoff = bitpos;
                            m->bitwid = w;
                            bitpos += w;
                            if (bitpos == 32) { i += 4; bitpos = 0; }
                        } else {
                            if (bitpos) { i += 4; bitpos = 0; }
                            m->offset = i;
                            i += ty_size(ty);
                            i = (i + 3) & -4;
                        }
                        if (atk == Union) {
                            if (i > tsize[bt]) tsize[bt] = i;
                            i = 0; bitpos = 0;
                        }
                        if (tk == ',') next();
                    }
                    next();
                }
                next();
                if (bitpos) i += 4;
                if (atk != Union) tsize[bt] = i;
            }
            break;
        case Int:
            bt = INT; next(); break;
        case Char:
            bt = CHAR; next(); break;
        case Unsigned:
            next(); bt = UINT;
            if (tk == Char) { next(); bt = UCHAR; }
            else if (tk == Int) next();
            break;
        }
        /* parse statement such as 'int a, b, c;'
         * "enum" finishes by "tk == ';'", so the code below will be skipped.
         * While current token is not statement end or block end.
         */
        while (tk == Keyword)          /* static/extern/register/auto */
            next();
        b = 0;
        while (tk != ';' && tk != '}' && tk != ',' && tk != ')') {
            struct ident_s *decl_id;
            int is_fptr = 0;
            int fptr_ndims = 0;
            ndims = 0;
            ty = bt;
            // If the beginning of * is a pointer type, then type plus `PTR`
            // indicates what kind of pointer
            while (tk == Mul) { next(); ty += PTR; }
            /* C90 function-pointer declarator: int (*name)(params), or an
             * array of them: int (*name[N])(params). */
            if (tk == '(')
                is_fptr = fptr_declarator(&decl_id, dims, &fptr_ndims);
            if (is_fptr) {
                ty = FPTR;
                if (ctx == Glo && decl_id->class >= Glo)
                    fatal("duplicate global definition");
                if (ctx == Loc && decl_id->class >= Loc)
                    fatal("duplicate local definition");
                /* Build array-of-FPTR, innermost dimension first. */
                for (i = fptr_ndims - 1; i >= 0; i--)
                    ty = arr_new(ty, dims[i]);
            } else {
                switch (ctx) {
                case Glo:
                    if (tk != Id) fatal("bad global declaration");
                    if (id->class >= ctx) fatal("duplicate global definition");
                    break;
                case Loc:
                    if (tk != Id) fatal("bad local declaration");
                    if (id->class >= ctx) fatal("duplicate local definition");
                    break;
                }
                decl_id = id;
                next();
            }
            id = decl_id;
            id->type = ty;
            set_used(id, 0);    /* reset for this declaration/function */
            /* Parse every [N] suffix.  For int a[2][3] this builds
             * array[2] of array[3] of int (innermost dimension first). */
            while (tk == Bracket) {
                int dim = 0;                /* [] denotes an incomplete array */
                next();
                if (tk != ']') {
                    if (tk != Num) fatal("bad array size");
                    dim = ival;
                    next();
                }
                if (tk != ']') fatal("bad array declaration");
                next();
                if (ndims >= 16) fatal("too many array dimensions");
                dims[ndims++] = dim;
            }
            for (i = ndims - 1; i >= 0; i--) ty = arr_new(ty, dims[i]);
            id->type = ty;
            if (!is_fptr && tk == '(') { // function
                if (b != 0) fatal("func decl can't be mixed with var decl(s)");
                if (ctx != Glo) fatal("nested function");
                id->class = Func; // type is function
                // "+ 1" is because the code to add instruction always uses "++e".
                id->val = (int) (e + 1); // function Pointer? offset/address
                id->type = ty;
                /* stmt(Par) clobbers the global `ty`, so remember the return
                 * type and function ident before parsing parameters. */
                {
                    struct ident_s *fid = id;
                    int retty = ty;
                    next(); ld = 0; // "ld" is parameter's index.
                    if (tk == Id) {
                        /* K&R identifier list: only possible here because a
                         * typed parameter list always starts with a type
                         * token.  Parses declarations up to '{' or ';'. */
                        kr_parameters();
                    } else {
                        while (tk != ')') {
                            /* C90 variadic prototype: skip the `...` token
                             * (three Dot tokens) without declaring a param. */
                            if (tk == Dot) {
                                next();
                                if (tk != Dot) fatal("bad ellipsis in prototype");
                                next();
                                if (tk != Dot) fatal("bad ellipsis in prototype");
                                next();
                                break;
                            }
                            stmt(Par);
                            if (tk == ',') next();
                        }
                        next();
                    }
                    if (tk == ';') {
                        /* Forward declaration.  The name has already been
                         * registered above as Func with its return type and
                         * code address (the latter is overwritten by a later
                         * definition).  Discard the temporary parameter
                         * entries and emit no code; leave `tk` at ';' so the
                         * caller's next() consumes it, exactly as it does for
                         * an ordinary variable declaration. */
                        /* Mark as not-yet-defined so calls emitted before
                         * the definition use a fixup instead of this stale
                         * text-pool address. */
                        fid->val = -1;
                        unwind_parms();
                        return;
                    }
                    if (tk != '{') fatal("bad function definition");
                    retvoid = (retty == VOID);
                    retstruct = IS_STRUCT(retty) || retty == DOUBLE;
                if (retstruct) {
                    int hidden = ld++;       /* hidden destination pointer */
                    retwords = retty == DOUBLE ? 2 : (ty_size(retty) + 3) / 4;
                    if (retwords < 1 || retwords > 64)
                        fatal("struct return too large");
                    loc = ++ld;
                    retlea = loc - hidden;
                } else {
                    loc = ++ld;
                }
                }
                next();
                // Not declare and must not be function, analyze inner block.
                // e represents the address which will store pc
                // (ld - loc) indicates memory size to allocate
                ASTW(';');
                while (tk != '}') {
                    int *t = n; check_label(&t); stmt(Loc);
                    if (t != n) { ASTW((int) t); ASTW('{'); }
                }
                ASTW(ld - loc); ASTW(Enter);
                cas = 0;
                gen(n);
                id = sym; // unwind symbol table locals
                while (id->tk) {
                    if (id->class == Loc || id->class == Par ||
                        id->class == Stc) {
                        if (id->class != Stc && !get_used(id))
                            warn_ident("unused variable ", id);
                        id->class = id->hclass;
                        id->type = id->htype;
                        id->val = id->hval;
                    }
                    else if (id->class == Label) { // clear id for next func
                        id->class = 0; id->val = 0; id->type = 0;
                    }
                    else if (id->class == 0 && id->type == -1) {
                        printf("%d: label %.*s not defined\n",
                               line, id->hash & 0x3f, id->name);
                        exit(-1);
                    }
                    id++;
                }
            }
            else {
                int sz;
                int persistent = (ctx == Loc && is_static);
                if (ty == VOID) fatal("variable of type void");
                sz = ty_size(ty);
                id->hclass = id->class;
                id->class = persistent ? Stc : ctx;
                id->htype = id->type; id->type = ty;
                id->hval = id->val;
                if (ctx == Glo || persistent) {
                    /* Globals and static locals live in the data segment. */
                    data = (char *) (((int) data + 3) & -4);
                    need_data(sz);
                    id->val = (int) data; data += sz;
                }
                else if (ctx == Loc) { id->val = (ld += (sz + 3) / 4); }

                /* C90 file-scope initializers: fold the constant expression
                 * at compile time and bake the bytes into the data-pool image
                 * that the backend appends to the flat binary.  Local/static
                 * initializers are handled below as a runtime store instead. */
                if (ctx == Glo && tk == Assign) {
                    int saved_ty = ty;
                    int *a;
                    int k;
                    char *q;
                    next();                     /* consume '=' */
                    if (tk == '{') {
                        next();
                        glo_init_list(ty, (char *) id->val);
                        if (tk != '}') fatal("expected '}' in initializer");
                        next();                 /* consume '}' */
                    } else {
                        expr(Assign);           /* folds constants to Num */
                        a = n;                  /* root is at n after expr() */
                        if (*a != Num)
                            fatal("global initializer must be a constant expression");
                        q = (char *) id->val;
                        for (k = 0; k < sz; ++k)
                            *q++ = (char) (a[1] >> (8 * k));
                    }
                    ty = saved_ty;              /* expr() clobbers ty */
                }

                else if (ctx == Par) {
                    if (IS_STRUCT(ty)) {
                        int words = (ty_size(ty) + 3) / 4;
                        if (words < 1 || words > 64)
                            fatal("struct parameter too large");
                        id->val = ld + words - 1;  /* word 0 = lowest slot */
                        ld += words;
                    } else {
                        id->val = ld++;
                    }
                }
                if (ctx == Loc && tk == Assign) {
                    int ptk = tk;
                    if (b == 0) ASTW(';');
                    b = n;
                    if (persistent) { ASTW(id->val); ASTW(Num); }
                    else { ASTW(loc - id->val); ASTW(Loc); }
                    next(); a = n; expr(ptk);
                    ASTW((int)a); ASTW(ty); ASTW(Assign);
                    ASTW((int) b); ASTW('{');
                }
            }
            if (ctx != Par && tk == ',') next();
        }
        return;
    case If:
        next();
        if (tk != '(') fatal("open parentheses expected");
        next();
        expr(Assign); a = n;
        if (*n == Assign)
            warn("suggest parentheses around assignment used as truth value");
        if (tk != ')') fatal("close parentheses expected");
        next();
        stmt(ctx);
        b = n;
        if (tk == Else) { next(); stmt(ctx); d = n; } else d = 0;
        ASTW((int)d); ASTW((int) b); ASTW((int) a); ASTW(Cond);
        return;
    case While:
        next();
        if (tk != '(') fatal("open parentheses expected");
        next();
        expr(Assign); b = n; // condition
        if (*n == Assign)
            warn("suggest parentheses around assignment used as truth value");
        if (tk != ')') fatal("close parentheses expected");
        next();
        ++brkc; ++cntc;
        stmt(ctx); a = n; // parse body of "while"
        --brkc; --cntc;
        ASTW((int) b); ASTW((int) a); ASTW(While);
        return;
    case DoWhile:
        next();
        ++brkc; ++cntc;
        stmt(ctx); a = n; // parse body of "do-while"
        --brkc; --cntc;
        if (tk != While) fatal("while expected");
        next();
        if (tk != '(') fatal("open parentheses expected");
        next();
        ASTW(';');
        expr(Assign); b = n;
        if (tk != ')') fatal("close parentheses expected");
        next();
        ASTW((int) b); ASTW((int) a); ASTW(DoWhile);
        return;
    case Switch:
        i = 0; j = 0;
        if (cas) j = (int) cas;
        cas = &i;
        next();
        if (tk != '(') fatal("open parentheses expected");
        next();
        expr(Assign);
        a = n;
        if (tk != ')') fatal("close parentheses expected");
        next();
        ++swtc; ++brkc;
        stmt(ctx);
        --swtc; --brkc;
        b = n;
        ASTW((int) b); ASTW((int) a); ASTW(Switch);
        if (j) cas = (int *) j;
        return;
    case Case:
        if (!swtc) fatal("case-statement outside of switch");
        i = *cas;
        next();
        expr(Or);
        a = n;
        if (*n != Num) fatal("bad case immediate");
        j = n[1]; n[1] -= i; *cas = j;
        ASTW(';');
        if (tk != ':') fatal("colon expected");
        next();
        stmt(ctx);
        b = n;
        ASTW((int) b);ASTW((int) a); ASTW(Case);
        return;
    case Break:
        if (!brkc) fatal("misplaced break statement");
        next();
        if (tk != ';') fatal("semicolon expected");
        next();
        ASTW(Break);
        return;
    case Continue:
        if (!cntc) fatal("misplaced continue statement");
        next();
        if (tk != ';') fatal("semicolon expected");
        next();
        ASTW(Continue);
        return;
    case Default:
        if (!swtc) fatal("default-statement outside of switch");
        next();
        if (tk != ':') fatal("colon expected");
        next();
        stmt(ctx); a = n;
        ASTW((int) a); ASTW(Default);
        return;
    // RETURN_stmt -> 'return' expr ';' | 'return' ';'
    case Return:
        a = 0; next();
        if (tk != ';') {
            if (retvoid) fatal("void function should not return a value");
            expr(Assign); a = n;
        }
        ASTW((int) a); ASTW(Return);
        if (tk != ';') fatal("semicolon expected");
        next();
        return;
    /* For iteration is implemented as:
     * Init -> Cond -> Bz to end -> Jmp to Body
     * After -> Jmp to Cond -> Body -> Jmp to After
     */
    case For:
        next();
        if (tk != '(') fatal("open parentheses expected");
        next();
        ASTW(';');
        if (tk != ';') expr(Assign);
        while (tk == ',') {
            int *f = n; next(); expr(Assign); ASTW((int) f); ASTW('{');
        }
        d = n;
        if (tk != ';') fatal("semicolon expected");
        next();
        ASTW(';');
        expr(Assign); a = n; // Point to entry of for cond
        if (tk != ';') fatal("semicolon expected");
        next();
        ASTW(';');
        if (tk != ')') expr(Assign);
        while (tk == ',') {
            int *g = n; next(); expr(Assign); ASTW((int) g); ASTW('{');
        }
        b = n;
        if (tk != ')') fatal("close parentheses expected");
        next();
        ++brkc; ++cntc;
        stmt(ctx); c = n;
        --brkc; --cntc;
        ASTW((int) d); ASTW((int) c); ASTW((int) b); ASTW((int) a);
        ASTW(For);
        return;
    case Goto:
        next();
        if (tk != Id || (id->type != 0 && id->type != -1)
                     || (id->class != Label && id->class != 0))
            fatal("goto expects label");
        id->type = -1; // hack for id->class deficiency
        ASTW((int) id); ASTW(Goto); next();
        if (tk != ';') fatal("semicolon expected");
        next();
        return;
    // stmt -> '{' stmt '}'
    case '{':
        next();
        ASTW(';');
        while (tk != '}') {
            a = n; check_label(&a); stmt(ctx);
            if (a != n) { ASTW((int) a); ASTW('{'); }
        }
        next();
        return;
    // stmt -> ';'
    case ';':
        next();
        ASTW(';');
        return;
    default:
        // general statements are considered assignment statements/expressions
        expr(Assign);
        if (tk != ';' && tk != ',') fatal("semicolon expected");
        next();
    }
}

#define KR_PARAM_MAX 32

/* Base type of a K&R parameter declaration.  Mirrors stmt()'s type switch;
 * inline struct definitions in a parameter declaration are not supported. */
static int kr_base_type(void)
{
    int bt = INT;
    switch (tk) {
    case Void:   bt = VOID;   next(); break;
    case Char:   bt = CHAR;   next(); break;
    case Float:  bt = FLOAT;  next(); break;
    case Double: bt = DOUBLE; next(); break;
    case Unsigned:
        next(); bt = UINT;
        if (tk == Char) { next(); bt = UCHAR; }
        else if (tk == Int) next();
        break;
    case Struct:
    case Union:
        next();
        if (tk != Id) fatal("bad struct/union type");
        bt = id->stype; next(); break;
    case Enum:
        next();
        if (tk == Id) next();
        break;
    case Int: next(); break;
    default: fatal("bad K&R parameter declaration");
    }
    return bt;
}

/* Declare a K&R parameter, preserving its pre-declaration attributes for the
 * unwind at function end.  A repeated declaration keeps the first backup. */
static void kr_declare_param(struct ident_s *d, int ty)
{
    if (d->class != Par) {
        d->hclass = d->class;
        d->htype = d->type;
        d->hval = d->val;
    }
    d->class = Par;
    d->type = ty;
}

/* Parse a K&R identifier list and its declaration list.  `tk` is the first
 * identifier after '('; on return `tk` is '{' (definition) or ';'
 * (declaration).  Slots are assigned in identifier-list order, so generated
 * code matches a prototype definition with the same parameters. */
static void kr_parameters(void)
{
    struct ident_s *krids[KR_PARAM_MAX];
    int nkr = 0, i, words, guard = 0, bt, ty;
    struct ident_s *d;

    while (tk == Id) {
        if (nkr >= KR_PARAM_MAX) fatal("too many K&R parameters");
        krids[nkr++] = id;
        next();
        if (tk != ',') break;
        next();
    }
    if (tk != ')') fatal("bad K&R parameter list");
    next();                             /* consume ')' */
    while (tk != '{' && tk != ';') {
        if (guard++ >= KR_PARAM_MAX * 2 || !is_type_name(tk))
            fatal("bad K&R parameter declaration");
        while (tk == Static || tk == Keyword) next();
        bt = kr_base_type();
        for (;;) {
            ty = bt;
            while (tk == Mul) { next(); ty += PTR; }
            if (tk != Id) fatal("bad K&R parameter declaration");
            kr_declare_param(id, ty);
            next();
            if (tk != ',') break;
            next();
        }
        if (tk != ';') fatal("bad K&R parameter declaration");
        next();
    }
    ld = 0;
    for (i = 0; i < nkr; i++) {
        d = krids[i];
        if (d->class != Par) {
            /* C90: an undeclared K&R parameter has implicit int type. */
            kr_declare_param(d, INT);
        }
        if (IS_STRUCT(d->type)) {
            words = (ty_size(d->type) + 3) / 4;
            if (words < 1 || words > 64) fatal("K&R parameter too large");
            d->val = ld + words - 1;
            ld += words;
        } else {
            d->val = ld++;
        }
    }
}

void die(char *msg) { printf("%s\n", msg); exit(-1); }

int reloc_imm(int offset) { return (((offset) - 8) >> 2) & 0x00ffffff; }
int reloc_bl(int offset) { return 0xeb000000 | reloc_imm(offset); }

int *codegen(int *jitmem, int *jitmap)
{
    int i, ii, tmp, c;
    int *je, *tje;    // current position in emitted native code
    int *immloc, *il;

    immloc = il = malloc(1024 * 4);
    int *iv = malloc(1024 * 4);
    int *imm0 = 0;

    // first pass: emit native code
    int *pc = text + 1; je = jitmem; line = 0;
    while (pc <= e) {
        i = *pc;
        // Store mapping from IR index to native instruction buffer location
        // "pc - text" gets the index of IR.
        // "je" points to native instruction buffer's current location.
        jitmap[((int) pc++ - (int) text) >> 2] = (int) je;
        switch (i) {
        case LEA:
            tmp = *pc++;
            if (tmp >= 64 || tmp <= -64) {
                printf("jit: LEA %d out of bounds\n", tmp); exit(6);
            }
            if (tmp >= 0)
                *je++ = 0xe28b0000 | tmp * 4;    // add     r0, fp, #(tmp)
            else
                *je++ = 0xe24b0000 | (-tmp) * 4; // sub     r0, fp, #(tmp)
            break;
        case IMM:
            tmp = *pc++;
            if (0 <= tmp && tmp < 256)
                *je++ = 0xe3a00000 + tmp;        // mov r0, #(tmp)
            else { if (!imm0) imm0 = je; *il++ = (int) (je++); *iv++ = tmp; }
            break;
        case JSR:
        case JMP:
            pc++; je++; // postponed till second pass
            break;
        case BZ:
        case BNZ:
            *je++ = 0xe3500000; pc++; je++;      // cmp r0, #0
            break;
        case ENT:
            *je++ = 0xe92d4800; *je++ = 0xe28db000; // push {fp, lr}; add  fp, sp, #0
            ii = c = 0; tmp = 4 * (*pc++);
            while (tmp >= 255) { c |= tmp & 3; tmp >>= 2; ++ii; }
            tmp += (c ? 1 : 0); if ((tmp << (2*ii)) >= 32768 || tmp < 0) {
                printf("jit: ENT %d out of bounds\n", tmp << (2*ii)); exit(6);
            } // sub  sp, sp, #tmp (scaled)
            if (tmp) *je++ = 0xe24dd000 | (((16-ii) & 0xf) << 8) | tmp;
            break;
        case ADJ:
            *je++ = 0xe28dd000 + *pc++ * 4;      // add sp, sp, #(tmp * 4)
            break;
        case LEV:
            *je++ = 0xe28bd000; *je++ = 0xe8bd8800; // add sp, fp, #0; pop {fp, pc}
            break;
        case LI:
            *je++ = 0xe5900000;                  // ldr r0, [r0]
            break;
        case LC:
            *je++ = 0xe5d00000; if (signed_char)  *je++ = 0xe6af0070; // ldrb r0, [r0]; (sxtb r0, r0)
            break;
        case SI:
            *je++ = 0xe49d1004; *je++ = 0xe5810000; // pop {r1}; str r0, [r1]
            break;
        case SC:
            *je++ = 0xe49d1004; *je++ = 0xe5c10000; // pop {r1}; strb r0, [r1]
            break;
        case PSH:
            *je++ = 0xe52d0004;                       // push {r0}
            break;
        case OR:
            *je++ = 0xe49d1004; *je++ = 0xe1810000; // pop {r1}; orr r0, r1, r0
            break;
        case XOR:
            *je++ = 0xe49d1004; *je++ = 0xe0210000; // pop {r1}; eor r0, r1, r0
            break;
        case AND:
            *je++ = 0xe49d1004; *je++ = 0xe0010000; // pop {r1}; and r0, r1, r0
            break;
        case SHL:
            *je++ = 0xe49d1004; *je++ = 0xe1a00011; // pop {r1}; lsl r0, r1, r0
            break;
        case SHR:
            *je++ = 0xe49d1004; *je++ = 0xe1a00051; // pop {r1}; asr r0, r1, r0
            break;
        case LSR:
            *je++ = 0xe49d1004; *je++ = 0xe1a00031; // pop {r1}; lsr r0, r1, r0
            break;
        case ADD:
            *je++ = 0xe49d1004; *je++ = 0xe0800001; // pop {r1}; add r0, r0, r1
            break;
        case SUB:
            *je++ = 0xe49d1004; *je++ = 0xe0410000; // pop {r1}; sub r0, r1, r0
            break;
        case MUL:
            *je++ = 0xe49d1004; *je++ = 0xe0000091; // pop {r1}; mul r0, r1, r0
            break;
        case DIV:
        case MOD:
        case UDIV:
        case UMOD:
            /* Host JIT fallback: the EABI helper is signed, so unsigned
             * operands are only correct here while they fit in signed int.
             * The Thumb backend (the actual target) uses udiv_helper. */
            *je++ = 0xe52d0004;                     // push {r0}
            int ti = ef_getidx((i == DIV || i == UDIV) ? "__aeabi_idiv"
                                                      : "__aeabi_idivmod");
            tmp = ef_getaddr(ti);
            *je++ = 0xe49d0004 | (1 << 12); // pop r1
            *je++ = 0xe49d0004 | (0 << 12); // pop r0
            *je++ = 0xe28fe000;                          // add lr, pc, #0
            if (!imm0) imm0 = je;
            *il++ = (int) je++ + 1;
            *iv++ = tmp;
            // ARM EABI modulo helper function produces quotient in r0
            // and the remainder in r1.
            if (i == MOD || i == UMOD)
                *je++ = 0xe1a00001;                 // mov r0, r1
            break;
        case SYSC:
            tmp = ef_getaddr(*pc++);  // look up address from ef index
            if (*pc++ != ADJ) die("codegen: no ADJ after native proc");
            i = *pc;
            if (i > 10) die("codegen: no support for 10+ arguments");
            while (i > 0) *je++ = 0xe49d0004 | (--i << 12); // pop r(i-1)
            i = *pc++;
            if (i > 4) *je++ = 0xe92d03f0;               // push {r4-r9}
            *je++ = 0xe28fe000;                          // add lr, pc, #0
            if (!imm0) imm0 = je;
            *il++ = (int) je++ + 1;
            *iv++ = tmp;
            if (i > 4) *je++ = 0xe28dd018;              // add sp, sp, #24
            break;
        case CLCA:
            *je++ = 0xe59d0004; *je++ = 0xe59d1000; // ldr r0, [sp, #4]
                                                    // ldr r1, [sp]
            *je++ = 0xe3a0780f; *je++ = 0xe2877002; // mov r7, #0xf0000
                                                    // add r7, r7, #2
            *je++ = 0xe3a02000; *je++ = 0xef000000; // mov r2, #0
                                                    // svc 0
            break;
        default:
            if (EQ <= i && i <= GE) {
                *je++ = 0xe49d1004; *je++ = 0xe1510000; // pop {r1}; cmp r1, r0
                if (i <= NE) { je[0] = 0x03a00000; je[1] = 0x13a00000; }   // moveq r0, #0; movne r0, #0
                else if (i == LT || i == GE) { je[0] = 0xb3a00000; je[1] = 0xa3a00000; } // movlt r0, #0; movge   r0, #0
                else { je[0] = 0xc3a00000; je[1] = 0xd3a00000; }           // movgt r0, #0; movle r0, #0
                if (i == EQ || i == LT || i == GT) je[0] = je[0] | 1;
                else je[1] = je[1] | 1;
                je += 2;
                break;
            } else {
                printf("code generation failed for %d!\n", i);
                free(iv);
                return 0;
            }
        }

        int genpool = 0;
        if (imm0) {
            if (i == LEV) genpool = 1;
            else if ((int) je > (int) imm0 + 3000) {
                tje = je++; genpool = 2;
            }
        }
        if (genpool) {
            *iv = 0;
            while (il > immloc) {
                tmp = *--il;
                if ((int) je > tmp + 4096 + 8) die("codegen: can't reach the pool");
                iv--; if (iv[0] == iv[1]) je--;
                if (tmp & 1) {
                    // ldr pc, [pc, #..]
                    *(int *) (tmp - 1) = 0xe59ff000 | ((int) je - tmp - 7);
                } else {
                    // ldr r0, [pc, #..]
                    *(int *) tmp = 0xe59f0000 | ((int) je - tmp - 8);
                }
                *je++ = *iv;
            }
            if (genpool == 2) { // jump past the pool
                tmp = ((int) je - (int) tje - 8) >> 2;
                *tje = 0xea000000 | (tmp & 0x00ffffff); // b #(je)
            }
            imm0 = 0;
            genpool = 0;
        }
    }
    if (il > immloc) die("codegen: not terminated by a LEV");
    tje = je;

    // second pass
    pc = text + 1; // Point instruction pointer "pc" to the first instruction.
    while (pc <= e) { // While instruction end is not met.
        // Get the IR's corresponding native instruction buffer address.
        je = (int *) jitmap[((int) pc - (int) text) >> 2];
        i = *pc++; // Get current instruction
        // If the instruction is one of the jumps.
        if (i == JSR || i == JMP || i == BZ || i == BNZ) {
            switch (i) {
            case JSR:
                *je = 0xeb000000;  // bl #(tmp)
                break;
            case JMP:
                *je = 0xea000000;  // b #(tmp)
                break;
            case BZ:
                *++je = 0x0a000000; // beq #(tmp)
                break;
            case BNZ:
                *++je = 0x1a000000; // bne #(tmp)
                break;
            }
            tmp = *pc++;
            *je = (*je |
                   reloc_imm(jitmap[(tmp - (int) text) >> 2] - (int) je));
        }
        // If the instruction has operand, increment instruction pointer to
        // skip the operand.
        else if (i <= ADJ || i == SYSC) { ++pc; }
    }
    free(iv);
    return tje;
}

enum {
    _PROT_EXEC = 4, _PROT_READ = 1, _PROT_WRITE = 2,
    _MAP_PRIVATE = 2, _MAP_ANON = 32
};

int jit(int poolsz, int *main, int argc, char **argv)
{
    char *jitmem;  // executable memory for JIT-compiled native code
    int retval;
    if (src) return 0; // skip for IR listing

    // setup JIT memory
    if (!(jitmem = mmap(0, poolsz, _PROT_EXEC | _PROT_READ | _PROT_WRITE,
                        _MAP_PRIVATE | _MAP_ANON, -1, 0))) {
        printf("could not mmap(%d) jit executable memory\n", poolsz);
        return -1;
    }
    int *jitmap = (int *) (jitmem + (poolsz >> 1));
    int *je = (int *) jitmem;
    *je++ = (int) &retval;
    *je++ = argc;
    *je++ = (int) argv;
    int *_start = je;
    *je++ = 0xe92d5ff0;       // push    {r4-r12, lr}
    *je++ = 0xe51f0014;       // ldr     r0, [pc, #-20] ; argc
    *je++ = 0xe51f1014;       // ldr     r1, [pc, #-20] ; argv
    *je++ = 0xe52d0004;       // push    {r0}
    *je++ = 0xe52d1004;       // push    {r1}
    int *tje = je++;          // bl      jitmain
    *je++ = 0xe51f502c;       // ldr     r5, [pc, #-44] ; retval
    *je++ = 0xe5850000;       // str     r0, [r5]
    *je++ = 0xe28dd008;       // add     sp, sp, #8
    *je++ = 0xe8bd9ff0;       // pop     {r4-r12, pc}
    if (!(je = codegen(je, jitmap))) return 1;
    if (je >= jitmap) die("jitmem too small");
    *tje = reloc_bl(jitmap[((int) main - (int) text) >> 2] - (int) tje);

    // hack to jump into specific function pointer
    __clear_cache(jitmem, je);
    int *res = bsearch(&sym, sym, 1, 1, (void *) _start);
    if (((void *) 0) != res) return 0; return -1; // make compiler happy
}

int ELF32_ST_INFO(int b, int t) { return (b << 4) + (t & 0xf); }
enum {
    EHDR_SIZE = 52, ET_EXEC = 2, EM_ARM = 40,
    PHDR_ENT_SIZE = 32, SHDR_ENT_SIZE = 40,
    SYM_ENT_SIZE = 16, REL_ENT_SIZE = 8, PLT_ENT_SIZE = 12,
    DYN_ENT_SIZE = 8
};

struct Elf32_Shdr {
    int sh_name;      // [Elf32_Word] Section name (index into string table)
    int sh_type;      // [Elf32_Word] Section type (SHT_*)
    int sh_flags;     // [Elf32_Word] Section flags (SHF_*)
    int sh_addr;      // [Elf32_Addr] Address where section is to be loaded
    int sh_offset;    // [Elf32_Off] File offset of section data, in bytes
    int sh_size;      // [Elf32_Word] Size of section, in bytes
    int sh_link;      // [Elf32_Word] Section type-specific header table
                      //              index link
    int sh_info;      // [Elf32_Word] Section type-specific extra information
    int sh_addralign; // [Elf32_Word] Section address alignment
    int sh_entsize;   // [Elf32_Word] Size of records contained within section
};

enum {
    // Special section indices
    SHN_UNDEF     = 0,      // Undefined, missing, irrelevant, or meaningless

    // Section types
    SHT_NULL          = 0,  // No associated section (inactive entry)
    SHT_PROGBITS      = 1,  // Program-defined contents
    SHT_STRTAB        = 3,  // String table
    SHT_DYNAMIC       = 6,  // Information for dynamic linking
    SHT_REL           = 9,  // Relocation entries; no explicit addends
    SHT_DYNSYM        = 11, // Symbol table

    // Section flags
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4,
};

// Symbol table entries for ELF32
struct Elf32_Sym {
    int st_name;  // [Elf32_Word] Symbol name (index into string table)
    int st_value; // [Elf32_Addr] Value or address associated with the symbol
    int st_size;  // [Elf32_Word] Size of the symbol
    char st_info; // [unsigned] Symbol's type and binding attributes
    char st_other;// [unsigned] Must be zero; reserved
    char st_shndx, st_shndx_1, st_shndx_2, st_shndx_3; // [Elf32_Half]
                  // Which section (header table index) it's defined
};

enum {
    // Symbol bindings
    STB_LOCAL = 0,   /* Local symbol, not visible outside obj file
                                      containing def */
    STB_GLOBAL = 1,  /* Global symbol, visible to all object files
                                     being combined */

    // Symbol types
    STT_NOTYPE  = 0,   // Symbol's type is not specified
    STT_FUNC    = 2,   // Symbol is executable code (function, etc.)

    // Symbol number
    STN_UNDEF = 0
};

// Program header for ELF32
struct Elf32_Phdr {
    int p_type;   // [Elf32_Word] Type of segment
    int p_offset; // [Elf32_Off] File offset where segment is located, in bytes
    int p_vaddr;  // [Elf32_Addr] Virtual address of beginning of segment
    int p_paddr;  // [Elf32_Addr] Physical address of beginning of segment
                  //              (OS-specific)
    int p_filesz; // [Elf32_Word] Number of bytes in file image of segment
                  //              (may be zero)
    int p_memsz;  // [Elf32_Word] Number of bytes in mem image of segment
                  //              (may be zero)
    int p_flags;  // [Elf32_Word] Segment flags
    int p_align;  // [Elf32_Word] Segment alignment constraint
};

// Segment types
enum {
    PT_NULL    = 0, // Unused segment
    PT_LOAD    = 1, // Loadable segment
    PT_DYNAMIC = 2, // Dynamic linking information
    PT_INTERP  = 3, // Interpreter pathname

    // Segment flag bits
    PF_X        = 1,         // Execute
    PF_W        = 2,         // Write
    PF_R        = 4,         // Read
};

int phdr_idx, shdr_idx, sym_idx;

int gen_phdr(char *ptr, int type, int offset, int addr, int size,
             int flag, int align)
{
    struct Elf32_Phdr *phdr = (struct Elf32_Phdr *) ptr;
    phdr->p_type =  type;
    phdr->p_offset = offset;
    phdr->p_vaddr = addr;
    phdr->p_paddr = addr;
    phdr->p_filesz = size;
    phdr->p_memsz = size;
    phdr->p_flags = flag;
    phdr->p_align = align;
    return phdr_idx++;
}

int gen_shdr(char *ptr, int type, int name, int offset, int addr,
             int size, int link, int info,
             int flag, int align, int entsize)
{
    struct Elf32_Shdr *shdr = (struct Elf32_Shdr *) ptr;
    shdr->sh_name = name;       shdr->sh_type = type;
    shdr->sh_addr = addr;       shdr->sh_offset = offset;
    shdr->sh_size = size;       shdr->sh_link = link;
    shdr->sh_info = info;       shdr->sh_flags = flag;
    shdr->sh_addralign = align; shdr->sh_entsize = entsize;
    return shdr_idx++;
}

int gen_sym(char *ptr, int name, char info,
            int shndx, int size, int value)
{
    struct Elf32_Sym *s = (struct Elf32_Sym *) ptr;
    s->st_name = name;
    s->st_info = info;
    s->st_other = 0;
    // s->st_shndx = shndx;
    memcpy(&(s->st_shndx), (char *) &shndx, 2);
    s->st_value = value;
    s->st_size = size;
    return sym_idx++;
}

int append_func_sym(char **sdata, int name)
{
    int idx = gen_sym(*sdata, name, ELF32_ST_INFO(STB_GLOBAL, STT_FUNC), 0, 0, 0);
    *sdata += SYM_ENT_SIZE;
    return idx;
}

// shdr names which start with 'S'
enum {
    SNONE = 0, SSTAB, STEXT, SDATA, SDYNS, SDYNM, SDYNC,
    SINTP, SREL, SPLT, SGOT
};

enum {
    PAGE_SIZE = 0x1000, PHDR_NUM = 4, SHDR_NUM = 11,
    DYN_NUM = 15
};

void elf32_init(int poolsz)
{
    int i;
    freebuf = malloc(poolsz);
    char *o = (char *) (((int) freebuf + PAGE_SIZE - 1)  & -PAGE_SIZE);
    /* We must assign the plt_func_addr[x] a non-zero value, and also,
     * plt_func_addr[i] and plt_func_addr[i-1] has an offset of 16
     * (4 instruction * 4 bytes), so the first codegen and second codegen
     * have consistent code_size. Dummy address at this point.
     */
    plt_func_addr = malloc(sizeof(char *) * PTR);
    for (i = 0; i < PTR; ++i)
        plt_func_addr[i] = o + i * 16;

    ef_getidx("__libc_start_main"); // slot 0 of external func cache
}

int elf32(int poolsz, int *main, int elf_fd)
{
    int i;
    char *freecode;
    char *code = freecode = malloc(poolsz);
    char *buf = freebuf;
    int *jitmap = (int *) (code + (poolsz >> 1));
    memset(buf, 0, poolsz);
    char *o = buf = (char *) (((int) buf + PAGE_SIZE - 1)  & -PAGE_SIZE);
    code = (char *) (((int) code + PAGE_SIZE - 1) & -PAGE_SIZE);

    phdr_idx = 0;
    shdr_idx = 0;
    sym_idx = 0;

    /* Run __libc_start_main() and pass main trampoline.
     *
     * Note: The function prototype of __libc_start_main() is:
     *
     *     int __libc_start_main(int (*main)(int, char**, char**),
     *                           int argc, char **argv,
     *                           int (*init)(int, char**, char**),
     *                           void (*fini)(void),
     *                           void (*rtld_fini)(void),
     *                           void *stack_end);
     *
     * Usually, we should pass __libc_csu_init as init and __libc_csu_fini
     * as fini; however, we will need a interp to link the non-shared part
     * of libc.  It sounds too complex.  To keep this compiler simple,
     * let's simply pass NULL pointer.
     */
    int *stub_end = (int *) code;

    *stub_end++ = 0xe3a0b000;  // mov   fp, #0  @ initialize frame pointer
    *stub_end++ = 0xe3a0e000;  // mov   lr, #0  @ initialize link register
    *stub_end++ = 0xe49d1004;  // pop   {r1}    @ get argc
    *stub_end++ = 0xe1a0200d;  // mov   r2, sp  @ get argv
    *stub_end++ = 0xe52d2004;  // push  {r2}    @ setup stack end
    *stub_end++ = 0xe52d0004;  // push  {r0}    @ setup rtld_fini
    *stub_end++ = 0xe3a0c000;  // mov   ip, #0  @ FIXME: __libc_csu_fini()
    *stub_end++ = 0xe52dc004;  // push  {ip}    @ setup fini
    *stub_end++ = 0xe28f0010;  // add   r0, pc, #16  @ load main trampoline
    *stub_end++ = 0xe3a03000;  // mov   r3, #0  @ FIXME: __libc_csu_init()
    *stub_end++ = 0xebfffffe;  // bl    __libc_start_main  @ Need relocation

    // Return 127 if __libc_start_main() returns (which should not.)
    *stub_end++ = 0xe3a0007f;  // mov   r0, #127
    *stub_end++ = 0xe3a07001;  // mov   r7, #1
    *stub_end++ = 0xef000000;  // svc   0x00000000

    // main() trampoline: convert ARM AAPCS calling convention to ours.
    *stub_end++ = 0xe92d5ff0;  // push  {r4-r12, lr}
    *stub_end++ = 0xe52d0004;  // push  {r0}
    *stub_end++ = 0xe52d1004;  // push  {r1}
    *stub_end++ = 0xebfffffe;  // bl    0 <main>  @ Need relocation
    *stub_end++ = 0xe28dd008;  // add   sp, sp, #8
    *stub_end++ = 0xe8bd9ff0;  // pop   {r4-r12, pc}

    int start_stub_size = (char *) stub_end - code;

    // Compile and generate the code.
    char *je = (char *) codegen((int *) (code + start_stub_size), jitmap);
    if (!je) return 1;
    if ((int *) je >= jitmap) die("elf32: jitmem too small");

    // elf32_hdr
    *o++ = 0x7f; *o++ = 'E'; *o++ = 'L'; *o++ = 'F';
    *o++ = 1;    *o++ = 1;   *o++ = 1;   *o++ = 0;
    o += 8;
    *o++ = ET_EXEC; *o++ = 0; // e_type
    *o++ = EM_ARM;  *o++ = 0; // e_machine
    *(int *) o = 1;          o += 4;
    char *entry = o;               o += 4; // e_entry
    *(int *) o = EHDR_SIZE;  o += 4; // e_phoff
    char *e_shoff = o;             o += 4; // e_shoff
    *(int *) o = 0x5000400;  o += 4; // e_flags
    *o++ = EHDR_SIZE; *o++ = 0;
    *o++ = PHDR_ENT_SIZE; *o++ = 0; *o++ = PHDR_NUM; *o++ = 0; // e_phentsize & e_phnum
    *o++ = SHDR_ENT_SIZE; *o++ = 0; *o++ = SHDR_NUM; *o++ = 0; // e_shentsize & e_shnum
    *o++ =  1; *o++ = 0;

    int phdr_size = PHDR_ENT_SIZE * PHDR_NUM;
    char *phdr = o; o += phdr_size;

    // .text
    int code_off = o - buf;
    int code_size = je - code;
    char *code_addr = o;
    o += code_size;

    // .rel.plt (embedded in PT_LOAD of text)
    int rel_size = REL_ENT_SIZE * ef_count;
    int rel_off = code_off + code_size;
    char *rel_addr = code_addr + code_size;
    o += rel_size;

    // .plt (embedded in PT_LOAD of text)
    int plt_size = 20 + PLT_ENT_SIZE * ef_count; // 20 is the size of .plt entry code to .got
    int plt_off = rel_off + rel_size;
    char *plt_addr = rel_addr + rel_size;
    o += plt_size;

    memcpy(code_addr, code,  code_size);
    *(int *) entry = (int) code_addr;

    // .data
    char *_data_end = data;
    // Use load_bias to align offset and v_addr, the elf loader
    // needs PAGE_SIZE align to do mmap().
    int load_bias = PAGE_SIZE + ((int) _data & (PAGE_SIZE - 1))
                    - ((o - buf) & (PAGE_SIZE - 1));
    o += load_bias;
    char *dseg = o;

    // rwdata (embedded in PT_LOAD of data)
    // rwdata is all the data (R/O and R/W) in source code,
    // e.g, the variable with initial value and all the string.
    int rwdata_off = dseg - buf;
    int rwdata_size = _data_end - _data;
    o += rwdata_size;

    // .dynamic (embedded in PT_LOAD of data)
    char *pt_dyn = data;
    int pt_dyn_size = DYN_NUM * DYN_ENT_SIZE;
    int pt_dyn_off = rwdata_off + rwdata_size; data += pt_dyn_size;
    o += pt_dyn_size;

    // .interp (embedded in PT_LOAD of data)
    char *interp_str = "/lib/ld-linux-armhf.so.3";
    int interp_str_size = 25; // strlen(interp_str) + 1
    char *interp = data; memcpy(interp, interp_str, interp_str_size);
    int interp_off = pt_dyn_off + pt_dyn_size; data += interp_str_size;
    o += interp_str_size;

    // .shstrtab (embedded in PT_LOAD of data)
    char *shstrtab_addr = data;
    int shstrtab_off = interp_off + interp_str_size;
    int shstrtab_size = 0;

    int *shdr_names = malloc(sizeof(int) * SHDR_NUM);
    if (!shdr_names) die("elf32: could not malloc shdr_names table");

    shdr_names[SNONE] = append_strtab(&data, "") - shstrtab_addr;
    shdr_names[SSTAB] = append_strtab(&data, ".shstrtab") - shstrtab_addr;
    shdr_names[STEXT] = append_strtab(&data, ".text") - shstrtab_addr;
    shdr_names[SDATA] = append_strtab(&data, ".data") - shstrtab_addr;
    shdr_names[SDYNS] = append_strtab(&data, ".dynstr") - shstrtab_addr;
    shdr_names[SDYNM] = append_strtab(&data, ".dynsym") - shstrtab_addr;
    shdr_names[SDYNC] = append_strtab(&data, ".dynamic") - shstrtab_addr;
    shdr_names[SINTP] = append_strtab(&data, ".interp") - shstrtab_addr;
    shdr_names[SREL] = append_strtab(&data, ".rel.plt") - shstrtab_addr;
    shdr_names[SPLT] = append_strtab(&data, ".plt") - shstrtab_addr;
    shdr_names[SGOT] = append_strtab(&data, ".got") - shstrtab_addr;
    shstrtab_size = data - shstrtab_addr;
    o += shstrtab_size;

    // .dynstr (embedded in PT_LOAD of data)
    char *dynstr_addr = data;
    int dynstr_off = shstrtab_off + shstrtab_size;
    append_strtab(&data, "");
    char *libc = append_strtab(&data, "libc.so.6");
    char *ldso = append_strtab(&data, "libdl.so.2");
    char *libgcc_s = append_strtab(&data, "libgcc_s.so.1");

    int *func_entries = malloc(sizeof(int) * ef_count);
    if (!func_entries) die("elf32: could not malloc func_entries table");

    for (i = 0; i < ef_count; ++i)
        func_entries[i] = append_strtab(&data, ef_cache[i]->name) - dynstr_addr;

    int dynstr_size = data - dynstr_addr;
    o += dynstr_size;

    // .dynsym (embedded in PT_LOAD of data)
    char *dynsym_addr = data;
    int dynsym_off = dynstr_off + dynstr_size;
    memset(data, 0, SYM_ENT_SIZE);
    data += SYM_ENT_SIZE;

    for (i = 0; i < ef_count; ++i)
        append_func_sym(&data, func_entries[i]);

    int dynsym_size = SYM_ENT_SIZE * (ef_count + 1);
    o += dynsym_size;

    // .got (embedded in PT_LOAD of data)
    char *got_addr = data;
    int got_off = dynsym_off + dynsym_size;
    *(int *) data = (int) pt_dyn; data += 4;
    data += 4;  // reserved 2 and 3 entry for interp
    char *to_got_movw = data;  // The address manipulates dynamic
    char *to_got_movt = data;  // linking, plt must jump here.
    data += 4;  // reserved 2 and 3 entry for interp
    // .got function slot
    char **got_func_slot = malloc(sizeof(char *) * ef_count);
    for (i = 0; i < ef_count; i++) {
        got_func_slot[i] = data;
        *(int *) data = (int) plt_addr; data += 4;
    }
    data += 4;  // end with 0x0
    int got_size = (int) data - (int) got_addr;
    o += got_size;

    int dseg_size = o - dseg;

    // .plt -- Now we back to handle .plt after .got was initial
    char *to = plt_addr;
    *(int *) to = 0xe52de004; to += 4; // push {lr}
    // movw r10 addr_to_got
    *(int *) to = 0xe300a000 | (0xfff & (int) (to_got_movw)) |
                  (0xf0000 & ((int) (to_got_movw) << 4));
    to += 4;
    // movt r10 addr_to_got
    *(int *) to = 0xe340a000 | (0xfff & ((int) (to_got_movt) >> 16)) |
                  (0xf0000 & ((int) (to_got_movt) >> 12));
    to += 4;
    *(int *) to = 0xe1a0e00a; to += 4;  // mov lr,r10
    *(int *) to = 0xe59ef000; to += 4;  // ldr pc, [lr]

    // We must preserve ip for code below, dyn link use this as return address
    for (i = 0; i < ef_count; i++) {
        plt_func_addr[i] = to;
        // movt ip addr_to_got
        *(int *) to = 0xe300c000 | (0xfff & (int) (got_func_slot[i])) |
                      (0xf0000 & ((int) (got_func_slot[i]) << 4));
        to += 4;
        // movw ip addr_to_got
        *(int *) to = 0xe340c000 |
                      (0xfff & ((int) (got_func_slot[i]) >> 16)) |
                      (0xf0000 & ((int) (got_func_slot[i]) >> 12));
        to += 4;
        *(int *) to = 0xe59cf000; to += 4;  // ldr pc, [ip]
    }

    // .rel.plt
    to = rel_addr;
    for (i = 0; i < ef_count; i++) {
        *(int *) to = (int) got_func_slot[i]; to += 4;
        *(int *) to = 0x16 | (i + 1) << 8 ; to += 4;
        // 0x16 R_ARM_JUMP_SLOT | .dymstr index << 8
    }

    // Generate program header after we got address, offset and size.
    to = phdr;
    // PT_LOAD for .text
    gen_phdr(to, PT_LOAD, 0, (int) buf,
            EHDR_SIZE + phdr_size + code_size + rel_size + plt_size,
            PF_X | PF_R, PAGE_SIZE);
    to += PHDR_ENT_SIZE;

    // PT_LOAD for .data
    gen_phdr(to, PT_LOAD, rwdata_off, (int) _data,
            dseg_size, PF_W | PF_R, PAGE_SIZE);
    to += PHDR_ENT_SIZE;

    // PT_INTERP for .interp
    gen_phdr(to, PT_INTERP, interp_off, (int) interp,
            interp_str_size , PF_R, 0x1);
    to += PHDR_ENT_SIZE;

    // PT_DYNAMIC for .dynamic
    gen_phdr(to, PT_DYNAMIC, pt_dyn_off, (int) pt_dyn,
            pt_dyn_size , PF_R | PF_W, 0x4);

    // .dynamic (embedded in PT_LOAD of data)
    to = pt_dyn;
    *(int *) to =  5; to += 4; *(int *) to = (int) dynstr_addr;  to += 4;
    *(int *) to = 10; to += 4; *(int *) to = dynstr_size;        to += 4;
    *(int *) to =  6; to += 4; *(int *) to = (int) dynsym_addr;  to += 4;
    *(int *) to = 11; to += 4; *(int *) to = 16;                 to += 4;
    *(int *) to = 17; to += 4; *(int *) to = (int) rel_addr;     to += 4;
    *(int *) to = 18; to += 4; *(int *) to = rel_size;           to += 4;
    *(int *) to = 19; to += 4; *(int *) to = 8;                  to += 4;
    *(int *) to =  3; to += 4; *(int *) to = (int) got_addr;     to += 4;
    *(int *) to =  2; to += 4; *(int *) to = rel_size;           to += 4;
    *(int *) to = 20; to += 4; *(int *) to = 17;                 to += 4;
    *(int *) to = 23; to += 4; *(int *) to = (int) rel_addr;     to += 4;
    *(int *) to =  1; to += 4; *(int *) to = libc - dynstr_addr; to += 4;
    *(int *) to =  1; to += 4; *(int *) to = ldso - dynstr_addr; to += 4;
    *(int *) to =  1; to += 4; *(int *) to = libgcc_s - dynstr_addr; to += 4;
    *(int *) to =  0;

    /* Generate code again bacause address of .plt function slots must
     * be confirmed before codegen() to make sure the code is correct.
     */
    je = (char *) codegen((int *) (code + start_stub_size), jitmap);
    if (!je) {
        free(func_entries);
        free(shdr_names);
        return 1;
    }
    if ((int *) je >= jitmap) die("elf32: jitmem too small");

    // Relocate __libc_start_main() and main().
    *((int *) (code + 0x28)) = reloc_bl(plt_func_addr[0] - code_addr - 0x28);
    *((int *) (code + 0x44)) =
        reloc_bl(jitmap[((int) main - (int) text) >> 2] - (int) code - 0x44);

    // Copy the generated binary.
    memcpy(code_addr, code,  je - code);

    // Generate section header
    *(int *) e_shoff = (int) (o - buf);
    gen_shdr(o, SHT_NULL, shdr_names[SNONE], 0, 0, 0,
             0, 0, 0, 0, 0);
    o += SHDR_ENT_SIZE;

    // sh_shstrtab_idx
    gen_shdr(o, SHT_STRTAB, shdr_names[SSTAB], shstrtab_off, 0,
             shstrtab_size, 0, 0, 0, 1, 0);
    o += SHDR_ENT_SIZE;

    // sh_text_idx
    gen_shdr(o, SHT_PROGBITS, shdr_names[STEXT], code_off, (int) code_addr,
            code_size, 0, 0, SHF_ALLOC | SHF_EXECINSTR, 4, 0);
    o += SHDR_ENT_SIZE;

    // sh_data_idx
    gen_shdr(o, SHT_PROGBITS, shdr_names[SDATA], rwdata_off, (int) _data,
             dseg_size, 0, 0, SHF_ALLOC | SHF_WRITE, 4, 0);
    o += SHDR_ENT_SIZE;

    int sh_dynstr_idx =
    gen_shdr(o, SHT_STRTAB, shdr_names[SDYNS], dynstr_off, (int) dynstr_addr,
             dynstr_size, 0, 0, SHF_ALLOC, 1, 0);
    o += SHDR_ENT_SIZE;

    int sh_dynsym_idx =
    gen_shdr(o, SHT_DYNSYM, shdr_names[SDYNM], dynsym_off, (int) dynsym_addr,
             dynsym_size, sh_dynstr_idx, 1, SHF_ALLOC, 4, 0x10);
    o += SHDR_ENT_SIZE;

    // sh_dynamic_idx
    gen_shdr(o, SHT_DYNAMIC, shdr_names[SDYNC], pt_dyn_off, (int) pt_dyn,
             pt_dyn_size, sh_dynstr_idx, 0, SHF_ALLOC | SHF_WRITE, 4, 0);
    o += SHDR_ENT_SIZE;

    // sh_interp_idx
    gen_shdr(o, SHT_PROGBITS, shdr_names[SINTP], interp_off, (int) interp,
             interp_str_size, 0, 0, SHF_ALLOC, 1, 0);
    o += SHDR_ENT_SIZE;

    // sh_rel_idx
    gen_shdr(o, SHT_REL, shdr_names[SREL], rel_off, (int) rel_addr,
             rel_size, sh_dynsym_idx, 11, SHF_ALLOC | 0x40, 4, 8);
    o += SHDR_ENT_SIZE;

    // sh_plt_idx
    gen_shdr(o, SHT_PROGBITS, shdr_names[SPLT], plt_off, (int) plt_addr,
             plt_size, 0, 0, SHF_ALLOC | SHF_EXECINSTR, 4, 4);
    o += SHDR_ENT_SIZE;

    // sh_got_idx
    gen_shdr(o, SHT_PROGBITS, shdr_names[SGOT], got_off, (int) got_addr,
             got_size, 0, 0, SHF_ALLOC | SHF_WRITE, 4, 4);
    o += SHDR_ENT_SIZE;

    // Copy .data to a part of (o - buf) where _data located.
    memcpy(dseg, _data, dseg_size);
    write(elf_fd, buf, o - buf);

    free(func_entries);
    free(shdr_names);
    free(freebuf);
    free(freecode);
    free(plt_func_addr);
    free(got_func_slot);
    return 0;
}

/* Resolve patchable calls once every definition is known.  A call slot was
 * emitted as a JSR; depending on the name it becomes:
 *   - a real user function (val > 0): keep JSR, patch in the address;
 *   - a syscall with no user definition: rewrite the opcode word to SYSC and
 *     keep the ef-cache index (the IR rewrite resolves it to a svc number);
 *   - any other name with no definition: point JSR at a shared stub that
 *     branches to address 0, so the external call fails clearly at run time
 *     instead of being miscompiled as a syscall or rejected as undefined.
 *
 * The negative fixup index is stored in the operand word itself, so the IR
 * is walked once and every marked operand is patched.  (Keeping a single
 * slot pointer per fixup index lost all but the last site whenever a symbol
 * was called more than once, e.g. three ioctl() calls, and the stale
 * negative markers then poisoned the branch-target rewrite.) */
static void resolve_forward_calls(void)
{
    int ext_stub = 0;               /* IR pointer, 0 means not yet allocated */
    int *pc = text + 1;
    while (pc <= e) {
        int op = *pc++;
        if (op == JSR || op == CODEADDR) {
            int fi = -(*pc) - 1;
            if (*pc < 0 && fi >= 0 && fi < nfwd_calls) {
                struct ident_s *d = fwd_ident[fi];
                if (d->val > 0 && d->class == Func) {
                    *pc = d->val;
                } else if (op == JSR && fwd_efidx[fi] >= 0) {
                    pc[-1] = SYSC;  /* opcode word immediately precedes operand */
                    *pc = fwd_efidx[fi];
                } else {
                    if (!ext_stub) {
                        need_ir(3);
                        IRW(IMM); IRW(0); IRW(JSRI);
                        ext_stub = (int) (e - 2);
                    }
                    *pc = ext_stub;
                }
            }
            pc++;               /* skip the operand */
        } else if (op == LEA || op == IMM || op == JMP ||
                   op == BZ || op == BNZ || op == ENT || op == ADJ ||
                   op == SCPY || op == DATAPTR || op == PUSHS) {
            pc++;               /* skip the operand */
        } else if (op == SYSC) {
            pc += 3;            /* ef-index, ADJ, arg-count */
        }
    }
}

enum { _O_CREAT = 64, _O_WRONLY = 1 };

#ifdef int
/* Eliminate clang compilation error:
 *   first parameter of 'main' (argument count) must be of type 'int'
 */
#undef int
#endif
int thumb_compile(int *text, int *end, unsigned char *out, int outcap, unsigned char *data, int datasz, char *errbuf);
/* Pool sizes are kept small to fit a FUZIX process (~182 KB PROGSIZE).
 * FCC_POOLSZ covers the symbol table and the data image; the IR and the AST
 * get their own, larger pools because they scale with code complexity rather
 * than with the number of declared names.  On target int is 32-bit, so
 * FCC_IRSZ is 3072 IR words and FCC_ASTSZ is 3072 AST words.  The AST only
 * has to hold one function body at a time because n is reset after each
 * top-level declaration (see main()).  The largest program measured so far
 * (the full-screen colour demo) needs ~1200 IR words, ~900 AST words and
 * 1136 data bytes, so every pool keeps better than 2x headroom.  runbin
 * refuses a flat image above 56 KB. */
#define FCC_POOLSZ  (8 * 1024)
#define FCC_IRSZ    (16 * 1024)
#define FCC_ASTSZ   (20 * 1024)
#define FCC_CODESZ  (26 * 1024)
#define FCC_SRC_MAX (40 * 1024)   /* largest accepted source file */

/* Bound every pool allocation: a negative size or one larger than any pool
 * can produce a wrapped malloc argument or an unchecked pool overrun.  Fail
 * early with a clear message instead of silently corrupting memory. */
#define FCC_MAX_ALLOC (64 * 1024)

static void *xmalloc(int n)
{
    void *p;
    if (n <= 0 || n > FCC_MAX_ALLOC)
        die("fcc: pool allocation size out of bounds");
    p = malloc((size_t) n);
    if (!p) die("fcc: out of memory");
    return p;
}

/* Counted allocation: check the product for overflow before xmalloc. */
static void *xmalloc_array(int count, int size)
{
    if (count <= 0 || size <= 0 || count > FCC_MAX_ALLOC / size)
        die("fcc: pool array size out of bounds");
    return xmalloc(count * size);
}

int main(int argc, char **argv)
{
    int *ast;
    int i, len, fd, outfd;
    char *outpath = "a.out";
    char *srcpath;
    char *poolsrc;
    unsigned char *code;
    int poolsz = FCC_POOLSZ;
    struct ident_s *idmain;
    char err[128];
    int stub[16], stublen, mainidx;
    int *ir2, ir2len, k;

    --argc; ++argv;
    srcpath = 0;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) die("no output file");
            outpath = argv[i];
        } else if (!srcpath) {
            srcpath = argv[i];
        }
    }
    if (!srcpath) die("usage: fcc [-o out] file.c");
    text = le = e = xmalloc(FCC_IRSZ);
    sym            = xmalloc(poolsz);
    freedata = _data = data = xmalloc(poolsz);
    tsize          = xmalloc_array(PTR, sizeof(int));
    members        = xmalloc_array(PTR, sizeof(struct member_s *));
    ast = xmalloc(FCC_ASTSZ);
    ef_cache       = xmalloc_array(PTR, sizeof(struct ef_s *));

    memset(sym, 0, poolsz);
    memset(e, 0, FCC_IRSZ);
    memset(data, 0, poolsz);
    memset(tsize, 0, PTR * sizeof(int));
    memset(members, 0, PTR * sizeof(struct member_s *));
    memset(ast, 0, FCC_ASTSZ);
    /* Initialise bounds before any parse emits into a pool. */
    ir_limit   = text + FCC_IRSZ / (int) sizeof(*text);
    ast_floor  = ast;
    data_limit = freedata + poolsz;
    sym_limit  = sym + poolsz / (int) sizeof(*sym);
    ast = (int *)((char *)ast + FCC_ASTSZ);

    /* Storage-class specifiers.  "static" gets its own token so local
     * declarations can allocate persistent storage in the data segment;
     * the others remain generic keywords that are simply skipped. */
    p = "static ";
    next(); id->tk = Static; id->class = Keyword;
    p = "extern register auto ";
    for (i = 0; i < 3; i++) {
        next();
        id->tk = Keyword;
        id->class = Keyword;
    }
    /* Floating point is recognised only to reject it with a clear
     * diagnostic -- it is never silently misparsed. */
    p = "float double ";
    /* Not class=Keyword: stmt() skips generic keywords before its type
     * switch, so float/double must keep their own token class to be seen. */
    next(); id->tk = Float; id->class = Float;
    next(); id->tk = Double; id->class = Double;
    p = "break continue case char default else enum if int return sizeof "
        "struct union switch for while do goto unsigned __clear_cache void main";
    for (i = Break; i <= Goto; i++) {
        next(); id->tk = i; id->class = Keyword;
    }
    next(); id->tk = Unsigned; id->class = Keyword;   /* "unsigned" */
    next(); id->class = ClearCache; id->type = INT; id->val = CLCA;
    next(); id->tk = Void; id->class = Void;          /* "void" */
    next();                                            /* "main" */
    idmain = id; idmain->class = Main;
    /* Size the source buffer from the file itself.  A fixed pool silently
     * capped every program at a few hundred lines; a compiler for larger
     * programs needs to accept whatever fits in its memory budget. */
    fd = open(srcpath, 0);
    if (fd < 0) die("cannot open source");
    {
        long srclen = lseek(fd, 0L, 2);          /* SEEK_END */
        if (srclen <= 0) die("empty source");
        if (srclen > FCC_SRC_MAX) die("source file too large");
        if (lseek(fd, 0L, 0) < 0) die("cannot seek source");  /* SEEK_SET */
        p = poolsrc = xmalloc((int) srclen + 1);
        i = 0;
        while (i < (int) srclen) {
            int got = read(fd, p + i, (int) srclen - i);
            if (got < 0) die("error reading source");
            if (got == 0) die("short read on source");
            i += got;
        }
        p[i] = 0;
    }
    close(fd);

    tsize[CHAR]  = 1;
    /* The target is fixed 32-bit ARM Thumb-1: int is 4 bytes regardless of
     * the width of the compiler's own int (which may be widened to long by
     * the host-support macro above).  Mirroring tsize[FLOAT] makes compiled
     * programs report sizeof(int)==4 on every build host. */
    tsize[INT]   = 4;
    tsize[UINT]  = 4;
    tsize[UCHAR] = 1;
    tsize[FLOAT] = 4;           /* target is 32-bit even on 64-bit hosts */
    tsize[DOUBLE] = 8;
    tnew = TNEW_START;            /* struct ids start above basic types */
    narr = 0;                     /* array descriptors start empty */
    line = 1;
    next();
    /* Parse and generate one top-level declaration at a time.  stmt(Glo)
     * consumes the AST it built, so reclaim the pool for the next one: AST
     * usage then scales with the largest single function instead of with the
     * whole translation unit (which overflowed on larger programs). */
    n = ast;
    while (tk) { stmt(Glo); next(); n = ast; }

    /* Lexing is finished: release the source buffer before the back end
     * allocates its code buffer, keeping the peak process size down. */
    free(poolsrc);
    poolsrc = 0;

    /* Resolve forward calls now that every definition is parsed.  Prototypes
     * with no definition are legal for external/libc functions and for
     * syscalls; they no longer abort compilation. */
    resolve_forward_calls();

    if (!idmain->val) die("no main function");

    /* Build a flat IR array with a small entry stub in front:
     *   call main; _exit(result)
     * and rewrite branch operands from absolute IR pointers to indices. */
    stublen = 0;
    stub[stublen++] = ENT; stub[stublen++] = 0;
    stub[stublen++] = JSR; stub[stublen++] = 0;    /* patched below */
    stub[stublen++] = SYSC; stub[stublen++] = 0;
    stub[stublen++] = ADJ;  stub[stublen++] = 0;
    stub[stublen++] = LEV;

    /* amacc's `e` points at the last emitted IR word (inclusive), so the
     * valid IR range is text[1..e] — one more word than (e - text). */
    /* amacc emits branch targets of `e + 1` (one past the last IR word) to
     * mean "leave the function", so append a sentinel LEV to land on. */
    ir2len = stublen + (int)(e - text) + 2;
    ir2 = xmalloc_array(ir2len, sizeof(int));
    ir2[0] = 0;                       /* amacc convention: text[0] is a dummy */
    for (k = 0; k < stublen; k++) ir2[1 + k] = stub[k];
    for (k = 1; k <= (e - text); k++)
        ir2[stublen + k] = text[k];
    ir2[ir2len - 1] = LEV;            /* sentinel for e+1 branch targets */
    mainidx = ((int *)idmain->val - text) + stublen;
    ir2[4] = mainidx;   /* JSR operand: stub starts at ir2[1], JSR at ir2[3] */

    /* Rewrite absolute IR pointers in branch operands into ir2 indices.
     * This must PARSE the stream, not scan for opcode values: an IMM operand
     * may legitimately equal 3 (JSR) or another opcode number.  Data-pool
     * addresses are now emitted as DATAPTR directly by gen() (GlbAddr AST
     * marker), so no numeric address-range heuristic is needed here. */
    k = stublen + 1;                  /* skip the entry stub (already patched) */
    while (k < ir2len) {
        int op = ir2[k++];
        if (op == LEA || op == IMM || op == JMP || op == JSR ||
                   op == BZ || op == BNZ || op == ENT || op == ADJ ||
                   op == SCPY || op == DATAPTR || op == PUSHS ||
                   op == CODEADDR) {
            if (op == JMP || op == JSR || op == BZ || op == BNZ ||
                op == CODEADDR) {
                int target = (int)(((char *)ir2[k] - (char *)text) / sizeof(int));
                ir2[k] = target + stublen;
            }
            k++;                      /* skip the operand */
        } else if (op == SYSC) {
            /* The IR operand is an ef-cache index; the back end needs the
             * actual FUZIX syscall number stored in the cache entry. */
            int ei = ir2[k];
            if (ei >= 0 && ei < ef_count && ef_cache[ei])
                ir2[k] = ef_cache[ei]->addr;
            k += 3;                   /* efidx, ADJ, arg-count */
        }
    }
    code = xmalloc(FCC_CODESZ);
    len = thumb_compile(ir2, ir2 + ir2len, code, FCC_CODESZ,
                        (unsigned char *) freedata, (int)(data - freedata), err);
    if (len < 0) die(err);

    outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (outfd < 0) die("cannot open output");
    i = write(outfd, code, len);
    close(outfd);
    if (i != len) die("short write");

    if (warnings) printf("%d warnings generated.\n", warnings);
    return 0;
}
