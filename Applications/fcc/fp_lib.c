/* Soft-float helpers compiled to position-independent Thumb-1.
 *
 * Argument order is deliberately REVERSED for non-commutative operations:
 * fcc's IR leaves the right operand in r0 and pops the left into r1, while
 * AAPCS passes the first C argument in r0 and the second in r1.  Naming the
 * first parameter `b` (right) and the second `a` (left) means each helper
 * directly computes  a OP b  with no register shuffling in the backend.
 */
float fp_fadd(float a, float b) { return a + b; }
float fp_fsub(float b, float a) { return a - b; }
float fp_fmul(float a, float b) { return a * b; }
/* Exact float division by bitwise long division on the 24-bit mantissas.
 * Only 32-bit integer ops: no 64-bit helpers, no .rodata jump tables.
 * Parameters reversed: b = divisor (r0), a = dividend (r1). */
float fp_fdiv(float b, float a)
{
    union { float f; unsigned int u; } ua, ub, ur;
    unsigned int sa, sb, ma, mb, q, rem;
    int ea, eb, er, bit;
    ua.f = a; ub.f = b;
    sa = ua.u >> 31; sb = ub.u >> 31;
    ea = (int)((ua.u >> 23) & 0xff);
    eb = (int)((ub.u >> 23) & 0xff);
    ma = (ua.u & 0x7fffffu) | 0x800000u;
    mb = (ub.u & 0x7fffffu) | 0x800000u;
    er = ea - eb + 127;
    if (ma < mb) { ma <<= 1; er--; }   /* quotient in [1,2) */
    rem = ma; q = 0;
    if (rem >= mb) { rem -= mb; q = 1; }  /* integer part */
    for (bit = 0; bit < 23; bit++) {       /* 23 fraction bits */
        rem <<= 1;
        q <<= 1;
        if (rem >= mb) { rem -= mb; q |= 1u; }
    }
    if ((rem << 1) >= mb) q++;          /* round to nearest */
    ur.u = ((sa ^ sb) << 31) | (((unsigned int)er) << 23) | (q & 0x7fffffu);
    return ur.f;
}
int fp_flt(float b, float a) { return a < b; }
int fp_fgt(float b, float a) { return a > b; }
int fp_fle(float b, float a) { return a <= b; }
int fp_fge(float b, float a) { return a >= b; }
int fp_feq(float a, float b) { return a == b; }
int fp_fne(float a, float b) { return a != b; }
int fp_f2i(float x) { return (int)x; }
float fp_i2f(int x) { return (float)x; }

/* ---- double precision -------------------------------------------------
 * Double values are represented in the IR by the ADDRESS of an 8-byte slot.
 * Helpers take pointers in r0/r1/r2 and return the destination pointer in r0.
 */
double *fp_dadd(double *dst, double *a, double *b) { *dst = *a + *b; return dst; }
double *fp_dsub(double *dst, double *a, double *b) { *dst = *a - *b; return dst; }
double *fp_dmul(double *dst, double *a, double *b) { *dst = *a * *b; return dst; }
/* Division via Newton-Raphson reciprocal (divdf3 needs a .rodata jump table).
 * b = divisor, a = dividend, reversed to match the IR stack order. */
/* Division via 53-bit restoring division on the mantissas.  Uses only
 * 64-bit shift/compare/subtract, which the M0+ compiler emits inline. */
double *fp_ddiv(double *dst, double *a, double *b)
{
    union { double d; unsigned long long u; } ua, ub, ur;
    unsigned long long sa, sb, ma, mb, q, rem;
    int ea, eb, er, i;
    ua.d = *a; ub.d = *b;
    sa = ua.u >> 63; sb = ub.u >> 63;
    ea = (int)((ua.u >> 52) & 0x7ff);
    eb = (int)((ub.u >> 52) & 0x7ff);
    ma = (ua.u & 0xfffffffffffffULL) | 0x10000000000000ULL;
    mb = (ub.u & 0xfffffffffffffULL) | 0x10000000000000ULL;
    er = ea - eb + 1023;
    if (ma < mb) { ma <<= 1; er--; }
    rem = ma; q = 0;
    if (rem >= mb) { rem -= mb; q = 1; }
    for (i = 0; i < 52; i++) {
        rem <<= 1; q <<= 1;
        if (rem >= mb) { rem -= mb; q |= 1; }
    }
    if ((rem << 1) >= mb) q++;
    ur.u = ((sa ^ sb) << 63) | ((unsigned long long)er << 52) |
           (q & 0xfffffffffffffULL);
    *dst = ur.d;
    return dst;
}
int fp_dlt(double *dst, double *a, double *b) { (void)dst; return *a < *b; }
int fp_dgt(double *dst, double *a, double *b) { (void)dst; return *a > *b; }
int fp_dle(double *dst, double *a, double *b) { (void)dst; return *a <= *b; }
int fp_dge(double *dst, double *a, double *b) { (void)dst; return *a >= *b; }
int fp_deq(double *dst, double *a, double *b) { (void)dst; return *a == *b; }
int fp_dne(double *dst, double *a, double *b) { (void)dst; return *a != *b; }
int fp_d2i(double *a) { return (int)*a; }
double *fp_i2d(double *dst, int x) { *dst = (double)x; return dst; }
double *fp_f2d(double *dst, float x) { *dst = (double)x; return dst; }
float fp_d2f(double *a) { return (float)*a; }
double *fp_dneg(double *dst, double *a) { *dst = -*a; return dst; }
