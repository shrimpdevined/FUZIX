# fcc codebase coding standard (NASA-style)

This is the ruleset the compiler source is written and reviewed against.
It is adapted from the JPL "Power of 10" rules for safety-critical C plus
embedded constraints specific to the Pico.  Deviations must be documented
at the point of use with a rationale.

## Control flow
1. **No unrestrained recursion in the emitted compiler.** Parser recursion
   depth must be bounded; input size is capped by `FCC_POOLSZ`.
2. **All loops have a fixed upper bound.** No `while(1)` without an explicit
   exit and iteration cap.
3. **No `goto`** in new code.
4. **Single entry / single exit** per function where practical.

## Memory and types
5. **No dynamic allocation after initialization.** `fcc` allocates its pools
   once at startup; the code generator must not `malloc` per expression.
   (`thumb_compile` allocates one fixed-size emitter struct per compilation —
   a single bounded allocation, not per-node.)
6. **All arrays are statically sized with a compile-time `*_MAX`** and every
   write is bounds-checked (`h()`, literal/branch tables).
7. **Explicit widths** at hardware boundaries: Thumb encodings use `uint16_t`,
   syscall numbers `int`, addresses `unsigned long`.
8. **No pointer arithmetic on `void *`**; cast to `unsigned char *` first.

## Functions and size
9. **Functions <= 60 source lines** where practical; split helpers out.
10. **All return values and error paths checked.** `open`, `read`, `write`,
    `malloc` and codegen return codes are validated; failures call `die()`.

## Verification
11. **Every emitted instruction is host-verified** with Unicorn before it is
    trusted on hardware (`host/verify.py`, currently 12/12).
12. **Every compiler change requires**: clean host backend tests, a full
    warning-free image rebuild, and an on-device compile+run smoke test.

## Platform constraints
13. **Stack <= 4 KB**: no large automatic structs.  Large buffers are heap or
    static (see the `Emit` struct history in `thumb.c`).
14. **Process size <= 80 KB**: pools are sized so code + heap + stack fit.
15. **Position-independent output**: no absolute code addresses; branches are
    PC-relative and data is addressed with `DATAPTR`.
