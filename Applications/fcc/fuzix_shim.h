/* Compatibility shims for the unused Linux-only code left in amacc.c.
 *
 * The original ELF writer and mmap-based JIT remain in the source for
 * reference but are never called by fcc.  These stubs let that dead code
 * compile; the linker's --gc-sections removes it from the final binary. */
#ifndef FCC_FUZIX_SHIM_H
#define FCC_FUZIX_SHIM_H

#include <stddef.h>

#define FCC_UNUSED __attribute__((unused))

static FCC_UNUSED void *dlsym(void *h, const char *n) { (void)h; (void)n; return 0; }
static FCC_UNUSED void *dlopen(const char *n, int f) { (void)n; (void)f; return 0; }
static FCC_UNUSED void *mmap(void *a, size_t l, int p, int f, int fd, long o)
{ (void)a; (void)l; (void)p; (void)f; (void)fd; (void)o; return 0; }
static FCC_UNUSED int mprotect(void *a, size_t l, int p)
{ (void)a; (void)l; (void)p; return 0; }

#define __clear_cache(a, b) do { (void)(a); (void)(b); } while (0)

#endif
