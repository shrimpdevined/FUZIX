/* cpp.c - a small C90 preprocessor for the fcc toolchain.
 *
 * amacc's lexer ignores all '#' directive lines, so this utility performs
 * macro expansion, conditional compilation and #include handling up front,
 * emitting a clean C source file for fcc to compile.  System headers are
 * skipped (amacc discovers external functions by name), while quoted local
 * includes are expanded inline.
 *
 * Usage: cpp input.c -o output.i
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#define LINE_MAX   512
#define MAX_MACROS 1024
#define NAME_MAX   32
#define REPL_MAX   128
#define MAX_PARAMS 31
#define MAX_INCLUDE 8
#define MAX_IDIRS  8
#define NBUILTIN   5

/* Compact macro storage.  A descriptor is only 12 bytes: names and
 * replacement lists live in two shared bump-allocated pools and are reached
 * through 16-bit offsets.  1024 descriptors now cost 12 KB instead of 44 KB.
 * NO_INDEX marks a free descriptor (pool sizes are below 0xFFFF). */
#define NO_INDEX 0xFFFFu

/* Parameter names are held in one shared static pool instead of a fixed
 * MAX_PARAMS array inside every macro: 256 macros with 31 inline 32-byte
 * names would occupy roughly 290 KB of BSS and would not load on the target.
 * A fixed slot size keeps block reuse a simple first-fit free list.
 * PARAM_SLOTS also bounds the free list: at most PARAM_SLOTS blocks can ever
 * be handed out from the pool (each block occupies at least one slot). */
#define PARAM_NAME_MAX 32
#define PARAM_POOL_MAX 8192
#define PARAM_SLOTS    (PARAM_POOL_MAX / PARAM_NAME_MAX)

struct macro {
    unsigned short name_off;        /* offset into name_pool, NO_INDEX if free */
    unsigned short repl_off;        /* offset into repl_pool */
    unsigned short repl_len;        /* replacement text length, excluding NUL */
    unsigned char func;             /* function-like macro */
    unsigned char nparams;
    int param_off;                  /* first slot in param_pool, or -1 */
};

/* Name and replacement pools.  Bump allocators only: C90 has no #unmacro, so
 * a redefinition leaks the old text (a later redefinition of the same name
 * simply cannot be undone).  Exhaustion is fatal rather than a silent
 * truncation or overflow. */
static char name_pool[16384];
static char repl_pool[24576];
static unsigned short name_used;
static unsigned short repl_used;

static struct macro macros[MAX_MACROS];
static int nmacros;                     /* high-water mark of allocated slots */
static int free_macros;                 /* head of LIFO free descriptor list */
static char param_pool[PARAM_POOL_MAX];
static int param_used;
static struct { int off; int len; } param_free[PARAM_SLOTS];
static int nparam_free;
static int outfd = 1;
static int warned;
static int failed;                      /* set by #error; forces nonzero exit */
static int in_comment;                  /* block comment spanning physical lines */

/* Current logical source location.  Updated per line by process_file and
 * changed by #line; used by __LINE__, __FILE__ and diagnostics. */
static int cur_line = 1;
static const char *cur_file = "";
/* Storage for a filename supplied by a #line directive, one slot per
 * include depth so nested files cannot clobber their parent's setting. */
static char line_file[MAX_INCLUDE + 1][NAME_MAX];

/* -I search directories (command-line option, also used by #include). */
static char idirs[MAX_IDIRS][LINE_MAX];
static int nidirs;

/* Predefined macros live outside the user macro table so they do not consume
 * user descriptor slots.  __LINE__ and __FILE__ have their replacement text
 * refreshed in place before every source line, so each builtin reserves a
 * full REPL_MAX slot in repl_pool (otherwise a long __FILE__ value could
 * overwrite a neighbouring macro's replacement text). */
static struct macro builtins[NBUILTIN];
static int nbuiltins;
static struct macro *builtin_line;
static struct macro *builtin_file;

/* Per-depth scratch buffers for recursive macro expansion.  Kept static so
 * the 4 KB process stack is not exhausted by nested expansions. */
#define MAX_DEPTH 4
static char rep_buf[MAX_DEPTH][LINE_MAX * 2];
static char sub_buf[MAX_DEPTH][LINE_MAX * 2];
static char args_buf[MAX_DEPTH][MAX_PARAMS][LINE_MAX / 4];
/* Only one argument is expanded at a time (the loop in expand_into finishes
 * with an argument before starting the next), so a single buffer per depth
 * replaces the old [MAX_DEPTH][MAX_PARAMS][REPL_MAX] array and saves ~15 KB. */
static char xargs_buf[MAX_DEPTH][LINE_MAX / 4];

/* Conditional-compilation stack. */
struct cond {
    int parent;                     /* parent was active */
    int active;                     /* current region is emitted */
    int taken;                      /* a branch in this group was taken */
};
static struct cond conds[64];
static int nconds;

static int emitting(void)
{
    int i;
    for (i = 0; i < nconds; i++)
        if (!conds[i].active)
            return 0;
    return 1;
}

static void out(const char *s, int n)
{
    if (emitting() && n > 0)
        write(outfd, s, n);
}

/* Emit "file:line: " for a diagnostic.  Kept separate so #error and warn()
 * share the exact same location format. */
static void diag_prefix(void)
{
    char buf[LINE_MAX];
    int n = snprintf(buf, sizeof(buf), "%s:%d: ", cur_file, cur_line);
    if (n > 0) write(2, buf, (size_t)n);
}

static void warn(const char *msg)
{
    if (!warned) {
        write(2, "cpp: ", 5);
        diag_prefix();
        write(2, msg, strlen(msg));
        write(2, "\n", 1);
        warned = 1;
    }
}

/* Unrecoverable storage failure.  Never truncate or overflow a pool: a
 * preprocessor that silently loses macro text produces wrong output. */
static void cfatal(const char *msg)
{
    write(2, "cpp: fatal: ", 12);
    diag_prefix();
    write(2, msg, strlen(msg));
    write(2, "\n", 1);
    exit(1);
}

/* Bump allocate len+1 bytes (text plus NUL) from a pool.  min_slot forces a
 * larger reservation, used for builtins whose text is rewritten in place. */
static unsigned short pool_alloc(char *pool, unsigned short *used,
                                 int pool_size, const char *s, int len,
                                 int min_slot)
{
    unsigned short off;
    int slot = len + 1;
    if (slot < min_slot) slot = min_slot;
    if (len < 0 || slot > pool_size - (int)*used)
        cfatal("macro storage exhausted");
    off = *used;
    if (len > 0) memcpy(pool + off, s, (size_t)len);
    pool[off + len] = 0;
    *used = (unsigned short)(off + slot);
    return off;
}

/* The only accessors for macro text.  Descriptors never hold their own
 * name/replacement strings, so every reader must go through these. */
static char *macro_name(const struct macro *m) { return name_pool + m->name_off; }
static char *macro_repl(const struct macro *m) { return repl_pool + m->repl_off; }

/* Store replacement text in the pool and update both offset and length. */
static void macro_set_repl(struct macro *m, const char *repl, int min_slot)
{
    const char *s = repl ? repl : "";
    int len = (int)strlen(s);
    if (len > REPL_MAX - 1) len = REPL_MAX - 1;
    m->repl_off = pool_alloc(repl_pool, &repl_used, (int)sizeof(repl_pool),
                             s, len, min_slot);
    m->repl_len = (unsigned short)len;
}

/* Descriptor free list.  A free descriptor has name_off == NO_INDEX and
 * reuses repl_off as the link to the next free slot (NO_INDEX terminates). */
static int alloc_macro_slot(void)
{
    int i;
    if (free_macros >= 0) {
        i = free_macros;
        free_macros = macros[i].repl_off == NO_INDEX ? -1 : (int)macros[i].repl_off;
        return i;
    }
    if (nmacros >= MAX_MACROS) return -1;
    return nmacros++;
}

static void free_macro_slot(int i)
{
    macros[i].name_off = NO_INDEX;
    macros[i].repl_off = free_macros >= 0 ? (unsigned short)free_macros
                                          : (unsigned short)NO_INDEX;
    free_macros = i;
}

static struct macro *find_macro(const char *name)
{
    int i;
    for (i = 0; i < nmacros; i++)
        if (macros[i].name_off != NO_INDEX &&
            !strcmp(macro_name(&macros[i]), name))
            return &macros[i];
    for (i = 0; i < nbuiltins; i++)
        if (!strcmp(macro_name(&builtins[i]), name))
            return &builtins[i];
    return 0;
}

static void undef_macro(const char *name)
{
    int i;
    for (i = 0; i < nmacros; i++) {
        if (macros[i].name_off != NO_INDEX &&
            !strcmp(macro_name(&macros[i]), name)) {
            /* Return the parameter block to the free list before recycling. */
            if (macros[i].nparams > 0 && nparam_free < PARAM_SLOTS) {
                param_free[nparam_free].off = macros[i].param_off;
                param_free[nparam_free].len = macros[i].nparams;
                nparam_free++;
            }
            free_macro_slot(i);
            return;
        }
    }
}

/* Indexed access to a macro parameter name in the shared pool. */
static const char *macro_param(const struct macro *m, int k)
{
    if (k < 0 || k >= m->nparams || m->param_off < 0)
        return "";
    return param_pool + (m->param_off + k) * PARAM_NAME_MAX;
}

/* Number of comma-separated parameter names in a macro's (...) list. */
static int count_params(const char *params)
{
    int n = 0;
    const char *p = params;
    while (*p && n < MAX_PARAMS) {
        while (*p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ')') p++;
        if (p > s) n++;
        if (*p == ',') p++;
    }
    return n;
}

/* Reserve n fixed-size slots, reusing a freed block first so repeated
 * #define/#undef cycles cannot exhaust the static pool. */
static int alloc_param_slots(int n)
{
    int k;
    for (k = 0; k < nparam_free; k++)
        if (param_free[k].len >= n) break;
    if (k < nparam_free) {
        int off = param_free[k].off;
        for (; k + 1 < nparam_free; k++)
            param_free[k] = param_free[k + 1];
        nparam_free--;
        return off;
    }
    if (param_used + n <= PARAM_SLOTS) {
        int off = param_used;
        param_used += n;
        return off;
    }
    warn("too many macro parameters");
    return -1;
}

static void store_params(struct macro *m, const char *params, int slots)
{
    const char *p = params;
    while (*p && m->nparams < slots) {
        const char *s;
        char *dst;
        int len;
        while (*p == ' ' || *p == '\t') p++;
        s = p;
        while (*p && *p != ',' && *p != ')') p++;
        len = (int)(p - s);
        if (len > 0 && len < PARAM_NAME_MAX) {
            dst = param_pool + (m->param_off + m->nparams) * PARAM_NAME_MAX;
            strncpy(dst, s, (size_t)len);
            dst[len] = 0;
            m->nparams++;
        }
        if (*p == ',') p++;
    }
}

static void define_macro(const char *name, int func, char *params,
                         const char *repl)
{
    struct macro *m;
    int idx;
    int nlen = (int)strlen(name);
    undef_macro(name);
    idx = alloc_macro_slot();
    if (idx < 0) { warn("too many macros"); return; }
    m = &macros[idx];
    if (nlen > NAME_MAX - 1) nlen = NAME_MAX - 1;
    m->name_off = pool_alloc(name_pool, &name_used, (int)sizeof(name_pool),
                             name, nlen, 0);
    m->func = (unsigned char)(func != 0);
    m->nparams = 0;
    m->param_off = -1;
    if (func && params) {
        int slots = count_params(params);
        if (slots > 0) {
            int off = alloc_param_slots(slots);
            if (off >= 0) {
                m->param_off = off;
                store_params(m, params, slots);
            }
        }
    }
    macro_set_repl(m, repl, 0);
}

/* Add a predefined macro to the dedicated builtin table.  Each builtin gets
 * a full REPL_MAX slot because __LINE__/__FILE__ are rewritten in place. */
static struct macro *add_builtin(const char *name, const char *repl)
{
    struct macro *m;
    if (nbuiltins >= NBUILTIN) return 0;
    m = &builtins[nbuiltins++];
    m->name_off = pool_alloc(name_pool, &name_used, (int)sizeof(name_pool),
                             name, (int)strlen(name), 0);
    m->func = 0;
    m->nparams = 0;
    m->param_off = -1;
    macro_set_repl(m, repl, REPL_MAX);
    return m;
}

/* Install the C90 predefined macros.  __STDC_VERSION__, __STDC_HOSTED__,
 * __func__ and friends are C99/C95 additions and are deliberately absent.
 * __DATE__/__TIME__ are implementation-defined; a fixed placeholder keeps
 * preprocessing reproducible (no dependency on the target C library clock). */
static void init_builtins(void)
{
    add_builtin("__STDC__", "1");
    add_builtin("__DATE__", "\"Jan  1 1970\"");
    add_builtin("__TIME__", "\"00:00:00\"");
    builtin_line = add_builtin("__LINE__", "1");
    builtin_file = add_builtin("__FILE__", "\"\"");
}

/* Refresh the location-dependent predefined macros before each source line.
 * Writes stay inside the full REPL_MAX slot reserved at builtin creation. */
static void update_builtins(void)
{
    char buf[REPL_MAX];
    int len;
    if (builtin_line) {
        len = snprintf(buf, sizeof(buf), "%d", cur_line);
        if (len < 0) len = 0;
        if (len > REPL_MAX - 1) len = REPL_MAX - 1;
        memcpy(macro_repl(builtin_line), buf, (size_t)len);
        macro_repl(builtin_line)[len] = 0;
        builtin_line->repl_len = (unsigned short)len;
    }
    if (builtin_file) {
        len = snprintf(buf, sizeof(buf), "\"%s\"", cur_file);
        if (len < 0) len = 0;
        if (len > REPL_MAX - 1) len = REPL_MAX - 1;
        memcpy(macro_repl(builtin_file), buf, (size_t)len);
        macro_repl(builtin_file)[len] = 0;
        builtin_file->repl_len = (unsigned short)len;
    }
}

/* Propagate a #line directive to the output.  amacc ignores leading '#'
 * lines, so this is safe for fcc while remaining useful to other tools. */
static void emit_line_marker(int n, const char *file)
{
    char buf[LINE_MAX];
    int len = snprintf(buf, sizeof(buf), "#line %d \"%s\"\n", n, file);
    if (len > 0) out(buf, len < LINE_MAX ? len : LINE_MAX - 1);
}

static int is_ident(int c) { return isalnum(c) || c == '_'; }

static void skip_space(char **p)
{
    while (**p == ' ' || **p == '\t') (*p)++;
}

static int read_ident(char **p, char *buf, int max)
{
    int n = 0;
    while (is_ident((unsigned char)**p) && n < max - 1)
        buf[n++] = *(*p)++;
    buf[n] = 0;
    return n;
}

/* Copy one literal or comment span verbatim from *src into out at *op.
 * Returns 1 if a span was consumed (including the tail of a block comment
 * carried over from a previous line via *block), 0 if *src does not start a
 * literal/comment and the caller must handle the character itself.
 *
 * This is the core of phase-3 literal protection: neither keyword stripping
 * nor macro expansion may look at identifiers inside strings, character
 * constants or comments.  *block tracks an unterminated block comment across
 * physical lines. */
static int copy_literal(char **src, char *out, int *op, int max, int *block)
{
    char *p = *src;
    int o = *op;
#define CPUT(ch) do { if (o < max - 2) out[o++] = (char)(ch); } while (0)
    if (*block) {
        while (*p) {
            if (p[0] == '*' && p[1] == '/') {
                CPUT('*'); CPUT('/'); p += 2; *block = 0; break;
            }
            CPUT(*p++);
        }
        *src = p; *op = o; return 1;
    }
    if (*p == '"' || *p == '\'') {
        int quote = *p;
        CPUT(*p++);
        while (*p) {
            if (*p == '\\' && p[1]) { CPUT(*p++); CPUT(*p++); continue; }
            CPUT(*p);
            if (*p == quote) { p++; break; }
            p++;
        }
        *src = p; *op = o; return 1;
    }
    if (p[0] == '/' && p[1] == '/') {
        while (*p) CPUT(*p++);
        *src = p; *op = o; return 1;
    }
    if (p[0] == '/' && p[1] == '*') {
        CPUT(*p++); CPUT(*p++);
        for (;;) {
            if (!*p) { *block = 1; break; }
            if (p[0] == '*' && p[1] == '/') {
                CPUT('*'); CPUT('/'); p += 2; break;
            }
            CPUT(*p++);
        }
        *src = p; *op = o; return 1;
    }
#undef CPUT
    return 0;
}

/* Advance *p past a literal or comment starting there, without copying.
 * Used while scanning function-like macro arguments so that commas and
 * parentheses inside strings/character constants do not split arguments. */
static int skip_literal(char **p)
{
    char *s = *p;
    int c = (unsigned char)*s;
    if (c == '"' || c == '\'') {
        s++;
        while (*s) {
            if (*s == '\\' && s[1]) { s += 2; continue; }
            if (*s == c) { s++; break; }
            s++;
        }
        *p = s; return 1;
    }
    if (s[0] == '/' && s[1] == '/') {
        while (*s) s++;
        *p = s; return 1;
    }
    if (s[0] == '/' && s[1] == '*') {
        s += 2;
        for (;;) {
            if (!*s) break;
            if (s[0] == '*' && s[1] == '/') { s += 2; break; }
            s++;
        }
        *p = s; return 1;
    }
    return 0;
}

/* Expand macros in a line into the output.  depth guards against recursion. */
static void expand_into(char *line, char *result, int max, int depth, int *block);

/* Index of id in m's parameter list, or -1 when id is not a parameter. */
static int param_index(struct macro *m, const char *id)
{
    int k;
    for (k = 0; k < m->nparams; k++)
        if (!strcmp(id, macro_param(m, k))) return k;
    return -1;
}

/* True when parameter k is an operand of # or ## in the replacement list;
 * such arguments must be substituted from their raw (unexpanded) spelling. */
static int param_is_raw(struct macro *m, int k)
{
    char *p = macro_repl(m);
    int after_hash = 0, after_paste = 0;
    while (*p) {
        char *s = p;
        if (skip_literal(&s)) { p = s; continue; }
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (p[0] == '#' && p[1] == '#') {
            p += 2; after_paste = 1; after_hash = 0; continue;
        }
        if (*p == '#') { p++; after_hash = 1; after_paste = 0; continue; }
        if (is_ident((unsigned char)*p)) {
            char id[NAME_MAX];
            char *q = p;
            char *r;
            int match;
            read_ident(&q, id, sizeof(id));
            match = !strcmp(id, macro_param(m, k));
            r = q;
            while (*r == ' ' || *r == '\t') r++;
            if (match && (after_hash || after_paste ||
                          (r[0] == '#' && r[1] == '#'))) return 1;
            p = q;
            after_hash = after_paste = 0;
            continue;
        }
        after_hash = after_paste = 0;
        p++;
    }
    return 0;
}

/* Scratch for stringification.  Only one substitute() is active at a time
 * (expand_into recurses only after substitute has returned), so a single
 * static buffer is sufficient and keeps the small stack untouched. */
static char str_buf[LINE_MAX];

/* Build the string literal produced by applying # to an argument.
 * Leading/trailing whitespace is deleted and each internal whitespace run
 * becomes one space.  Inside string/char literals every '\' and '"' gets an
 * extra '\' so the argument's original spelling is preserved verbatim. */
static const char *stringify_arg(const char *arg)
{
    const char *p = arg;
    int o = 0;
    int space = 0;
    int started = 0;                /* has any token been emitted yet? */
#define SPUT(ch) do { if (o < LINE_MAX - 2) str_buf[o++] = (char)(ch); } while (0)
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    SPUT('"');
    while (*p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n') {
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (*p) space = 1;
            continue;
        }
        /* Comments were replaced by one space in translation phase 3, so
         * they contribute a single space to the stringified spelling. */
        if (c == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            space = 1;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            space = 1;
            continue;
        }
        if (space) {
            if (started) SPUT(' ');
            space = 0;
        }
        started = 1;
        if (c == '"' || c == '\'') {
            int quote = c;
            /* Emit the opening delimiter first; the loop below must not
             * mistake it for the matching closing delimiter. */
            if (quote == '"') SPUT('\\');
            SPUT(quote);
            p++;
            while (*p) {
                c = *p++;
                if (c == '\\' || c == '"') SPUT('\\');
                SPUT(c);
                if (c == quote) break;
            }
        } else {
            SPUT(c);
            p++;
        }
    }
    SPUT('"');
    str_buf[o] = 0;
#undef SPUT
    return str_buf;
}

/* Substitute arguments into a function-like macro's replacement list.
 * '#' stringifies the following parameter; '##' pastes the previous and next
 * preprocessing tokens by deleting the whitespace on either side.  Both the
 * stringified spelling and pasted operands use the *raw* argument text; the
 * caller re-expands the resulting string, which handles post-paste macros. */
static void substitute(struct macro *m, char **args, char *dst, int max)
{
    char *p = macro_repl(m);
    int o = 0;
    int block = 0;
    int paste = 0;
    while (*p && o < max - 2) {
        /* '##' removes whitespace before the token being pasted on. */
        if (paste) {
            while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '\t')) o--;
            paste = 0;
        }
        if (copy_literal(&p, dst, &o, max, &block)) continue;
        if (p[0] == '#' && p[1] == '#') {
            p += 2;
            skip_space(&p);
            paste = 1;
            continue;
        }
        if (*p == '#') {
            char id[NAME_MAX];
            char *q = p + 1;
            skip_space(&q);
            read_ident(&q, id, sizeof(id));
            int k = param_index(m, id);
            if (k >= 0) {
                const char *s = stringify_arg(args[k]);
                while (*s && o < max - 2) dst[o++] = *s++;
                p = q;
            } else {
                dst[o++] = *p++;        /* malformed # use: keep verbatim */
            }
            continue;
        }
        if (is_ident((unsigned char)*p)) {
            char id[NAME_MAX];
            char *q = p;
            read_ident(&q, id, sizeof(id));
            int k = param_index(m, id);
            int j;
            if (k >= 0) {
                for (j = 0; args[k][j] && o < max - 2; j++)
                    dst[o++] = args[k][j];
            } else {
                for (j = 0; id[j] && o < max - 2; j++) dst[o++] = id[j];
            }
            p = q;
        } else {
            dst[o++] = *p++;
        }
    }
    dst[o] = 0;
}


static void expand(char *line, int depth, int *block)
{
    char result[LINE_MAX * 2];
    expand_into(line, result, sizeof(result), depth, block);
    out(result, strlen(result));
}

static void expand_into(char *line, char *result, int max, int depth, int *block)
{
    char *src = line;
    int o = 0;
    if (depth >= MAX_DEPTH) {
        strncpy(result, line, max - 1);
        result[max - 1] = 0;
        return;
    }
    while (*src && o < max - 2) {
        if (copy_literal(&src, result, &o, max, block)) continue;
        if (is_ident((unsigned char)*src)) {
            char id[NAME_MAX];
            char *q = src;
            read_ident(&q, id, sizeof(id));
            struct macro *m = find_macro(id);
            if (m) {
                char *rep = rep_buf[depth];
                char *after = q;
                if (!m->func) {
                    int nb = 0;
                    expand_into(macro_repl(m), rep, LINE_MAX * 2, depth + 1, &nb);
                } else {
                    char *a = q;
                    skip_space(&a);
                    if (*a == '(') {
                        char (*args)[LINE_MAX / 4] = args_buf[depth];
                        int na = 0;
                        a++;
                        skip_space(&a);
                        if (*a != ')') {
                            for (;;) {
                                char *st = a;
                                int nest = 0;
                                while (*a) {
                                    if (skip_literal(&a)) continue;
                                    if (*a == '(') nest++;
                                    else if (*a == ')') {
                                        if (!nest) break;
                                        nest--;
                                    } else if (*a == ',' && !nest) break;
                                    a++;
                                }
                                int len = (int)(a - st);
                                if (na < MAX_PARAMS && len < LINE_MAX / 4) {
                                    strncpy(args[na], st, len);
                                    args[na][len] = 0;
                                } else args[na][0] = 0;
                                na++;
                                if (*a == ',') { a++; skip_space(&a); continue; }
                                break;
                            }
                        }
                        if (*a == ')') a++;
                        {
                            int nact = na < MAX_PARAMS ? na : MAX_PARAMS;
                            int ai;
                            while (na < MAX_PARAMS) args[na++][0] = 0;
                            /* Arguments are expanded before substitution,
                             * except when used as # or ## operands (the
                             * stringify/paste operations need raw tokens). */
                            if (depth + 1 < MAX_DEPTH) {
                                for (ai = 0; ai < nact; ai++) {
                                    int nb = 0;
                                    if (ai < m->nparams && !param_is_raw(m, ai)) {
                                        expand_into(args[ai], xargs_buf[depth],
                                                    LINE_MAX / 4, depth + 1, &nb);
                                        strncpy(args[ai], xargs_buf[depth],
                                                LINE_MAX / 4 - 1);
                                        args[ai][LINE_MAX / 4 - 1] = 0;
                                    }
                                }
                            }
                        }
                        char *argp[MAX_PARAMS];
                        { int ai; for (ai = 0; ai < MAX_PARAMS; ai++) argp[ai] = args[ai]; }
                        char *sub = sub_buf[depth];
                        int nb = 0;
                        substitute(m, argp, sub, LINE_MAX * 2);
                        expand_into(sub, rep, LINE_MAX * 2, depth + 1, &nb);
                        after = a;
                    } else {
                        /* function-like name not followed by '(': leave as-is */
                        int k;
                        for (k = 0; id[k] && o < max - 2; k++) result[o++] = id[k];
                        src = q;
                        continue;
                    }
                }
                { char *r = rep; for (; *r && o < max - 2; ) result[o++] = *r++; }
                src = after;
                continue;
            }
            int k;
            for (k = 0; id[k] && o < max - 2; k++) result[o++] = id[k];
            src = q;
        } else {
            result[o++] = *src++;
        }
    }
    result[o] = 0;
}

/* Evaluate a simple #if integer expression. */
static int eval_expr(char *s);
static int eval_cond(char **p);

static int eval_primary(char **p)
{
    skip_space(p);
    if (**p == '(') {
        int v;
        (*p)++;
        /* Let the recursive parse advance p to the matching ')': counting
         * parentheses by hand would be confused by '(' inside char constants. */
        v = eval_cond(p);
        if (**p == ')') (*p)++;
        return v;
    }
    if (**p == '!') {
        (*p)++;
        return !eval_primary(p);
    }
    if (**p == '-') {
        (*p)++;
        return -eval_primary(p);
    }
    if (!strncmp(*p, "defined", 7)) {
        *p += 7;
        skip_space(p);
        int paren = 0;
        if (**p == '(') { paren = 1; (*p)++; skip_space(p); }
        char id[NAME_MAX];
        read_ident(p, id, sizeof(id));
        if (paren && **p == ')') (*p)++;
        return find_macro(id) != 0;
    }
    if (isdigit((unsigned char)**p)) {
        int v = 0;
        if (**p == '0' && (*(*p + 1) == 'x' || *(*p + 1) == 'X')) {
            *p += 2;
            while (isxdigit((unsigned char)**p)) {
                int c = *(*p)++;
                v = v * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
            }
        } else if (**p == '0') {
            /* A leading zero selects octal, so 010 is 8 rather than 10.
             * Stray 8/9 digits are consumed but ignored (invalid octal). */
            while (**p >= '0' && **p <= '7')
                v = v * 8 + *(*p)++ - '0';
            while (isdigit((unsigned char)**p)) (*p)++;
        } else {
            while (isdigit((unsigned char)**p))
                v = v * 10 + *(*p)++ - '0';
        }
        return v;
    }
    if (**p == '\'') {
        /* Character constant: 'A' evaluates to its character code. */
        int v;
        (*p)++;
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
            case 'n': v = '\n'; (*p)++; break;
            case 't': v = '\t'; (*p)++; break;
            case 'r': v = '\r'; (*p)++; break;
            case 'v': v = '\v'; (*p)++; break;
            case 'f': v = '\f'; (*p)++; break;
            case 'b': v = '\b'; (*p)++; break;
            case 'a': v = '\a'; (*p)++; break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7':
                v = 0;
                while (**p >= '0' && **p <= '7') {
                    v = v * 8 + **p - '0';
                    (*p)++;
                }
                break;
            case 'x':
                (*p)++;
                v = 0;
                while (isxdigit((unsigned char)**p)) {
                    int c = *(*p)++;
                    v = v * 16 + (isdigit(c) ? c - '0' :
                                  (tolower(c) - 'a' + 10));
                }
                break;
            default:
                v = **p; if (**p) (*p)++; break;
            }
        } else {
            v = **p; if (**p) (*p)++;
        }
        if (**p == '\'') (*p)++;
        return v;
    }
    /* An identifier in #if is expanded if it is an object-like macro. */
    char id[NAME_MAX];
    read_ident(p, id, sizeof(id));
    struct macro *m = find_macro(id);
    if (m && !m->func) {
        char *r = macro_repl(m);
        skip_space(&r);
        return eval_primary(&r);
    }
    return 0;
}

static int eval_mul(char **p)
{
    int v = eval_primary(p);
    for (;;) {
        skip_space(p);
        char op = **p;
        if (op == '*' || op == '/' || op == '%') {
            (*p)++;
            int r = eval_primary(p);
            if (op == '*') v *= r;
            else if (op == '/') v = r ? v / r : 0;
            else v = r ? v % r : 0;
        } else break;
    }
    return v;
}

static int eval_add(char **p)
{
    int v = eval_mul(p);
    for (;;) {
        skip_space(p);
        char op = **p;
        if (op == '+' || op == '-') {
            (*p)++;
            int r = eval_mul(p);
            v = (op == '+') ? v + r : v - r;
        } else break;
    }
    return v;
}

static int eval_shift(char **p)
{
    int v = eval_add(p);
    for (;;) {
        skip_space(p);
        if (**p == '<' && *(*p + 1) == '<') { *p += 2; v <<= eval_add(p); }
        else if (**p == '>' && *(*p + 1) == '>') { *p += 2; v >>= eval_add(p); }
        else break;
    }
    return v;
}

static int eval_rel(char **p)
{
    int v = eval_shift(p);
    for (;;) {
        skip_space(p);
        char op = **p, op2 = *(*p + 1);
        if (op == '<' && op2 == '=') { *p += 2; v = v <= eval_shift(p); }
        else if (op == '>' && op2 == '=') { *p += 2; v = v >= eval_shift(p); }
        else if (op == '<') { (*p)++; v = v < eval_shift(p); }
        else if (op == '>') { (*p)++; v = v > eval_shift(p); }
        else break;
    }
    return v;
}

static int eval_eq(char **p)
{
    int v = eval_rel(p);
    for (;;) {
        skip_space(p);
        char op = **p, op2 = *(*p + 1);
        if (op == '=' && op2 == '=') { *p += 2; v = v == eval_rel(p); }
        else if (op == '!' && op2 == '=') { *p += 2; v = v != eval_rel(p); }
        else break;
    }
    return v;
}

static int eval_and(char **p)
{
    int v = eval_eq(p);
    for (;;) {
        skip_space(p);
        if (**p == '&' && *(*p + 1) != '&') { (*p)++; v &= eval_eq(p); }
        else break;
    }
    return v;
}

static int eval_xor(char **p)
{
    int v = eval_and(p);
    for (;;) {
        skip_space(p);
        if (**p == '^') { (*p)++; v ^= eval_and(p); }
        else break;
    }
    return v;
}

static int eval_or(char **p)
{
    int v = eval_xor(p);
    for (;;) {
        skip_space(p);
        if (**p == '|' && *(*p + 1) != '|') { (*p)++; v |= eval_xor(p); }
        else break;
    }
    return v;
}

static int eval_land(char **p)
{
    int v = eval_or(p);
    for (;;) {
        skip_space(p);
        if (**p == '&' && *(*p + 1) == '&') { *p += 2; v = v && eval_or(p); }
        else break;
    }
    return v;
}

static int eval_lor(char **p)
{
    int v = eval_land(p);
    for (;;) {
        skip_space(p);
        if (**p == '|' && *(*p + 1) == '|') { *p += 2; v = v || eval_land(p); }
        else break;
    }
    return v;
}

/* Conditional operator: lowest precedence here (there is no assignment or
 * comma level) and right associative.  The middle operand is parsed up to
 * the ':'; the third operand is another conditional. */
static int eval_cond(char **p)
{
    int v = eval_lor(p);
    skip_space(p);
    if (**p == '?') {
        int a, b;
        (*p)++;
        a = eval_cond(p);
        skip_space(p);
        if (**p == ':') (*p)++;
        b = eval_cond(p);
        v = v ? a : b;
    }
    return v;
}

static int eval_expr(char *s)
{
    char *p = s;
    return eval_cond(&p);
}

/* Keywords amacc does not model.  Strip them so more C90 source compiles;
 * on this 32-bit target long==int and signed is the default.  This is an
 * approximation: unsigned range semantics and short truncation are NOT
 * preserved (documented in STATUS.md).
 *
 * Storage-class specifiers (static/extern/register/auto) are deliberately
 * NOT stripped any more: removing them silently changes linkage and storage,
 * and fcc needs to see them.  This means fcc/amacc must accept and ignore
 * (or act on) these words.  Previously cpp removed all four because amacc's
 * lexer did not recognise them; that old behaviour broke semantic analysis.
 * Behaviour risk: if fcc still does not recognise storage-class specifiers,
 * sources using them will now fail to compile. */
static int is_ignored_kw(const char *s)
{
    static const char *kw[] = { "signed", "long", "short",
                                "const", "volatile", 0 };
    int i;
    for (i = 0; kw[i]; i++)
        if (!strcmp(s, kw[i])) return 1;
    return 0;
}

/* True when q points just past "long" and the next token is "double".
 * C90 permits long double to have the same representation as double, so the
 * pair is rewritten to "double" instead of the generic "long" -> "int". */
static int next_is_double(const char *q)
{
    while (*q == ' ' || *q == '\t') q++;
    return !strncmp(q, "double", 6) && !is_ident((unsigned char)q[6]);
}

static void strip_type_keywords(char *line, int *block)
{
    char out[LINE_MAX];
    char *src = line;
    int o = 0;
    while (*src && o < LINE_MAX - 2) {
        if (copy_literal(&src, out, &o, LINE_MAX, block)) continue;
        if (is_ident((unsigned char)*src)) {
            char id[NAME_MAX];
            char *q = src;
            read_ident(&q, id, sizeof(id));
            if (!strcmp(id, "long") && next_is_double(q)) {
                const char *d = "double";
                char *a = q;
                int k;
                while (*a == ' ' || *a == '\t') a++;
                a += 6;                 /* consume the "double" token */
                for (k = 0; d[k] && o < LINE_MAX - 2; k++) out[o++] = d[k];
                src = a;
                continue;
            }
            if (!is_ignored_kw(id)) {
                int k;
                for (k = 0; id[k] && o < LINE_MAX - 2; k++) out[o++] = id[k];
            } else if (!strcmp(id, "long") || !strcmp(id, "short")) {
                /* long/short alone are types -> int; "long int"/"short int"
                 * keep just the int. */
                char *a = q;
                while (*a == ' ' || *a == '\t') a++;
                if (strncmp(a, "int", 3) || is_ident((unsigned char)a[3])) {
                    out[o++] = 'i'; out[o++] = 'n'; out[o++] = 't';
                }
                if (o > 3 && out[o - 4] == ' ' && *q == ' ' && a == q) o--;
            }
            src = q;
        } else {
            out[o++] = *src++;
        }
    }
    out[o] = 0;
    strcpy(line, out);
}

/* Define one typedef alias as an object-like macro (an existing definition
 * with the same name is replaced, matching #define semantics). */
static void add_typedef_alias(const char *name, const char *type)
{
    if (name[0] && type[0]) define_macro(name, 0, 0, type);
}

/* Copy [start,end) collapsing whitespace runs to a single space and trimming
 * the ends.  Keeps multi-word types such as "unsigned long" and braced
 * struct bodies readable and valid when emitted as a macro replacement. */
static void copy_type(const char *start, const char *end, char *dst, int max)
{
    int o = 0;
    int space = 0;
    while (start < end) {
        unsigned char c = (unsigned char)*start++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = 1; continue; }
        if (space && o > 0 && o < max - 2) dst[o++] = ' ';
        space = 0;
        if (o < max - 2) dst[o++] = (char)c;
    }
    dst[o] = 0;
}

/* Locate the declared name in one declarator by scanning backwards, skipping
 * array/function suffixes.  Sets [ns,ne) to the identifier spelling. */
static int typedef_name(const char *start, const char *end,
                        const char **ns, const char **ne)
{
    const char *q = end;
    while (q > start) {
        char c = q[-1];
        if (c == ')' || c == ']') {
            int depth = 0;
            while (q > start) {
                c = q[-1];
                q--;
                if (c == ')' || c == ']') depth++;
                else if (c == '(' || c == '[') {
                    depth--;
                    if (depth == 0) break;
                }
            }
            continue;
        }
        if (is_ident((unsigned char)c)) {
            const char *s = q;
            while (s > start && is_ident((unsigned char)s[-1])) s--;
            *ns = s;
            *ne = q;
            return 1;
        }
        q--;
    }
    return 0;
}

/* Split a typedef body into top-level declarators.  Commas nested inside
 * {}, () or [] (and inside literals/comments) do not separate declarators.
 * The body is modified in place by inserting NUL at each separator. */
static int split_declarators(char *body, char **pieces, int maxpieces)
{
    int np = 0;
    char *start = body;
    char *p = body;
    int brace = 0, paren = 0, bracket = 0;
    while (*p) {
        if (skip_literal(&p)) continue;
        if (*p == '{') brace++;
        else if (*p == '}') brace--;
        else if (*p == '(') paren++;
        else if (*p == ')') paren--;
        else if (*p == '[') bracket++;
        else if (*p == ']') bracket--;
        else if (*p == ',' && !brace && !paren && !bracket) {
            if (np < maxpieces) pieces[np++] = start;
            *p = 0;
            start = p + 1;
        }
        p++;
    }
    if (np < maxpieces) pieces[np++] = start;
    return np;
}

/* True when a line begins a braced typedef whose body is not yet complete;
 * used by process_file to join multi-line `typedef struct { ... } Name;`. */
static int typedef_needs_more(const char *line)
{
    const char *p = line;
    int open = 0, close = 0, lastclose = -1, semi = -1, i = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "typedef", 7) || !isspace((unsigned char)p[7])) return 0;
    while (*p) {
        char c = *p++;
        if (c == '{') open++;
        else if (c == '}') { close++; lastclose = i; }
        else if (c == ';') semi = i;
        i++;
    }
    if (open > close) return 1;
    if (open == close && open > 0 && semi < lastclose) return 1;
    return 0;
}

/* Handle `typedef TYPE NAME;`, `typedef TYPE A, B;`, and
 * `typedef struct { ... } NAME;` by defining each alias as TYPE. */
static int handle_typedef(char *line, int depth)
{
    char *p = line;
    /* Static like the other scratch buffers: the 4 KB process stack cannot
     * absorb several 512-byte arrays per call.  Not re-entrant, but neither
     * caller (process_line / handle_logical_line) nests this function. */
    static char body[LINE_MAX];
    static char base[LINE_MAX];
    char *pieces[MAX_PARAMS];
    int n = 0, braces = 0, npieces, i;
    const char *ns, *ne;
    char name[NAME_MAX];
    int len;

    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "typedef", 7) || !isspace((unsigned char)p[7])) return 0;
    p += 7;
    /* Copy the body up to the terminating ';' at brace depth zero. */
    while (*p && n < LINE_MAX - 1) {
        if (*p == '"' || *p == '\'') {
            int quote = *p++;
            body[n++] = (char)quote;
            while (*p && n < LINE_MAX - 2) {
                body[n++] = *p;
                if (*p == '\\' && p[1]) {
                    body[n++] = p[1]; p += 2; continue;
                }
                if (*p == quote) { p++; break; }
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            body[n++] = *p++;
            body[n++] = *p++;
            while (*p && n < LINE_MAX - 2) {
                if (p[0] == '*' && p[1] == '/') {
                    body[n++] = *p++; body[n++] = *p++; break;
                }
                body[n++] = *p++;
            }
            continue;
        }
        if (*p == '{') braces++;
        else if (*p == '}') braces--;
        else if (*p == ';' && braces == 0) break;
        body[n++] = *p++;
    }
    body[n] = 0;
    npieces = split_declarators(body, pieces, MAX_PARAMS);
    if (npieces <= 0) return 0;
    if (!typedef_name(pieces[0], pieces[0] + strlen(pieces[0]), &ns, &ne))
        return 0;
    /* Base type is everything before the first declarator's name. */
    copy_type(pieces[0], ns, base, LINE_MAX);
    len = (int)(ne - ns);
    if (len <= 0 || len >= NAME_MAX) return 0;
    strncpy(name, ns, (size_t)len); name[len] = 0;
    add_typedef_alias(name, base);
    /* Additional declarators share the base type plus their own prefix. */
    for (i = 1; i < npieces; i++) {
        static char prefix[LINE_MAX];
        static char type[LINE_MAX];
        if (!typedef_name(pieces[i], pieces[i] + strlen(pieces[i]), &ns, &ne))
            continue;
        len = (int)(ne - ns);
        if (len <= 0 || len >= NAME_MAX) continue;
        strncpy(name, ns, (size_t)len); name[len] = 0;
        copy_type(pieces[i], ns, prefix, LINE_MAX);
        snprintf(type, sizeof(type), "%s%s%s", base,
                 prefix[0] ? " " : "", prefix);
        add_typedef_alias(name, type);
    }
    (void) depth;
    return 1;
}

static void process_file(const char *path, int depth);

/* Open a source file: try the given path first (preserving the old behavior),
 * then each -I directory.  Used for both the main input and #include. */
static int open_source(const char *path)
{
    int fd = open(path, 0);
    char full[LINE_MAX];
    int i;
    if (fd >= 0) return fd;
    for (i = 0; i < nidirs; i++) {
        int n = snprintf(full, sizeof(full), "%s/%s", idirs[i], path);
        if (n <= 0 || n >= (int)sizeof(full)) continue;
        fd = open(full, 0);
        if (fd >= 0) return fd;
    }
    return -1;
}

/* Minimal C90 standard-header support.  System headers are otherwise skipped
 * (amacc discovers external functions by name), but a few constants and types
 * are needed by ordinary source and fcc can represent them directly.  They
 * are injected as object-like macros only when the header is named. */
static void define_std_header(const char *name)
{
    static const struct {
        const char *header;
        const char *sym;
        const char *value;
    } defs[] = {
        { "stddef.h", "NULL",                "0" },
        { "stddef.h", "size_t",              "unsigned int" },
        { "stddef.h", "wchar_t",             "int" },
        { "limits.h", "EOF",                 "-1" },
        { "limits.h", "SCHAR_MIN",           "(-128)" },
        { "limits.h", "SCHAR_MAX",           "127" },
        { "limits.h", "UCHAR_MAX",           "255" },
        { "limits.h", "SHRT_MIN",            "(-32768)" },
        { "limits.h", "SHRT_MAX",            "32767" },
        { "limits.h", "USHRT_MAX",           "65535" },
        { "limits.h", "INT_MIN",             "(-2147483647-1)" },
        { "limits.h", "INT_MAX",             "2147483647" },
        { "limits.h", "UINT_MAX",            "4294967295" },
        { "limits.h", "LONG_MIN",            "(-2147483647-1)" },
        { "limits.h", "LONG_MAX",            "2147483647" },
        { "limits.h", "ULONG_MAX",           "4294967295" },
        { "float.h",  "FLT_MANT_DIG",        "24" },
        { "float.h",  "FLT_MAX_10_EXP",      "38" },
        { "float.h",  "DBL_MANT_DIG",        "53" },
        { "float.h",  "DBL_MAX_10_EXP",      "308" },
        { 0, 0, 0 }
    };
    int i;
    for (i = 0; defs[i].header; i++)
        if (!strcmp(name, defs[i].header))
            define_macro(defs[i].sym, 0, 0, defs[i].value);
}

static void do_directive(char *line, int depth)
{
    char *p = line + 1;              /* skip '#' */
    char cmd[32];
    skip_space(&p);
    read_ident(&p, cmd, sizeof(cmd));
    skip_space(&p);

    if (!strcmp(cmd, "define")) {
        char name[NAME_MAX];
        if (!read_ident(&p, name, sizeof(name))) return;
        int func = 0;
        char params[256] = "";
        if (*p == '(') {
            func = 1;
            char *q = ++p;
            while (*q && *q != ')') q++;
            int len = (int)(q - p);
            if (len > 0 && len < 256) { strncpy(params, p, len); params[len] = 0; }
            p = (*q == ')') ? q + 1 : q;
        }
        skip_space(&p);
        define_macro(name, func, params, p);
    } else if (!strcmp(cmd, "undef")) {
        char name[NAME_MAX];
        read_ident(&p, name, sizeof(name));
        undef_macro(name);
    } else if (!strcmp(cmd, "ifdef") || !strcmp(cmd, "ifndef")) {
        char name[NAME_MAX];
        read_ident(&p, name, sizeof(name));
        int def = find_macro(name) != 0;
        int parent = emitting();
        if (!strcmp(cmd, "ifndef")) def = !def;
        if (nconds < 64) {
            conds[nconds].parent = parent;
            conds[nconds].active = parent && def;
            conds[nconds].taken = def;
            nconds++;
        }
    } else if (!strcmp(cmd, "if")) {
        int parent = emitting();
        int v = eval_expr(p);
        if (nconds < 64) {
            conds[nconds].parent = parent;
            conds[nconds].active = parent && v;
            conds[nconds].taken = v;
            nconds++;
        }
    } else if (!strcmp(cmd, "elif")) {
        if (nconds > 0) {
            struct cond *c = &conds[nconds - 1];
            int v = eval_expr(p);
            c->active = c->parent && !c->taken && v;
            if (v) c->taken = 1;
        }
    } else if (!strcmp(cmd, "else")) {
        if (nconds > 0) {
            struct cond *c = &conds[nconds - 1];
            c->active = c->parent && !c->taken;
            c->taken = 1;
        }
    } else if (!strcmp(cmd, "endif")) {
        if (nconds > 0) nconds--;
    } else if (!strcmp(cmd, "include")) {
        if (!emitting()) return;
        char name[LINE_MAX];
        int quoted = 0;
        if (*p == '"') {
            quoted = 1;
            p++;
            int n = 0;
            while (*p && *p != '"' && n < LINE_MAX - 1) name[n++] = *p++;
            name[n] = 0;
        } else if (*p == '<') {
            p++;
            int n = 0;
            while (*p && *p != '>' && n < LINE_MAX - 1) name[n++] = *p++;
            name[n] = 0;
            define_std_header(name);
            return;                 /* system headers: amacc needs none */
        } else {
            while (*p == ' ' || *p == '\t') p++;
            int n = 0;
            while (*p && n < LINE_MAX - 1) name[n++] = *p++;
            name[n] = 0;
        }
        (void) quoted;
        if (depth < MAX_INCLUDE)
            process_file(name, depth + 1);
        else warn("include nesting too deep");
    } else if (!strcmp(cmd, "error")) {
        /* A #error in a skipped conditional group is harmless; otherwise
         * report it and make preprocessing fail so cc stops. */
        if (emitting()) {
            write(2, "cpp: ", 5);
            diag_prefix();
            write(2, p, strlen(p));
            write(2, "\n", 1);
            failed = 1;
        }
    } else if (!strcmp(cmd, "line")) {
        char *end;
        long n;
        /* Like other non-conditional directives, #line is ignored when it
         * appears in a skipped conditional group. */
        if (!emitting()) return;
        n = strtol(p, &end, 10);
        if (end != p) {
            /* process_file increments cur_line after this directive line,
             * so the line following #line becomes n. */
            cur_line = (int)n - 1;
            skip_space(&end);
            if (*end == '"') {
                int slot = depth <= MAX_INCLUDE ? depth : MAX_INCLUDE;
                char *fname = line_file[slot];
                int k = 0;
                end++;
                while (*end && *end != '"' && k < NAME_MAX - 1)
                    fname[k++] = *end++;
                fname[k] = 0;
                cur_file = fname;
            }
            emit_line_marker((int)n, cur_file);
        }
    } else if (!strcmp(cmd, "pragma")) {
        /* ignored */
    }
}

/* Process one logical (already spliced) line.  copy_literal/expand carry the
 * block-comment state across lines via the global in_comment flag, so a '#'
 * on a line that begins inside a comment is not mistaken for a directive. */
static void process_line(char *line, int depth)
{
    static char tmp[LINE_MAX * 2];
    int started_in_comment = in_comment;
    char *s = line;
    if (!in_comment) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#') { do_directive(s, depth); return; }
    }
    if (!emitting()) return;
    if (!started_in_comment && handle_typedef(line, depth)) {
        out("\n", 1);
        return;
    }
    /* Each pass needs its own copy of the starting state: the keyword pass
     * may consume a closing delimiter on this line, but the expansion pass
     * must still treat that delimiter and everything before it as comment. */
    {
        int bstrip = started_in_comment;
        int bexpand = started_in_comment;
        strip_type_keywords(line, &bstrip);
        strcpy(tmp, line);
        expand(tmp, 0, &bexpand);
        in_comment = bstrip;
    }
    out("\n", 1);
}

/* Dispatch one logical source line.  Braced typedefs that span physical
 * lines are joined until their terminating ';' so the alias is recognised
 * as a single declaration instead of leaking struct members as globals. */
static void handle_logical_line(char *line, char *td, int *tdpos,
                                int *in_td, int depth)
{
    int l;
    update_builtins();
    if (!emitting() || in_comment) { process_line(line, depth); return; }
    if (*in_td) {
        l = (int)strlen(line);
        if (*tdpos + l + 2 < LINE_MAX) {
            td[(*tdpos)++] = ' ';
            strcpy(td + *tdpos, line);
            *tdpos += l;
            td[*tdpos] = 0;
        }
        if (!typedef_needs_more(td)) {
            *in_td = 0;
            if (!handle_typedef(td, depth)) process_line(td, depth);
            else out("\n", 1);
        }
        return;
    }
    if (typedef_needs_more(line)) {
        strncpy(td, line, LINE_MAX - 1);
        td[LINE_MAX - 1] = 0;
        *tdpos = (int)strlen(td);
        *in_td = 1;
        return;
    }
    process_line(line, depth);
}

/* C90 2.2.1.1: the nine trigraphs are replaced in translation phase 1,
 * before line splicing and tokenization.  Returns 0 for a non-trigraph. */
static int trigraph_repl(int c)
{
    switch (c) {
    case '=': return '#';
    case '/': return '\\';
    case '\'': return '^';
    case '(': return '[';
    case ')': return ']';
    case '!': return '|';
    case '<': return '{';
    case '>': return '}';
    case '-': return '~';
    default:  return 0;
    }
}

/* Feed one raw source byte through phase 1.  At most two '?' are held across
 * calls; up to three characters can be emitted, so out[] has room for three.
 * When a third '?' arrives the first one is emitted and the last two stay
 * pending, which gives the left-to-right behaviour required by C90. */
static int phase1_char(int c, int *q, char *out)
{
    int rep;
    if (*q == 0) {
        if (c == '?') { *q = 1; return 0; }
        out[0] = (char)c;
        return 1;
    }
    if (*q == 1) {
        if (c == '?') { *q = 2; return 0; }
        out[0] = '?';
        out[1] = (char)c;
        *q = 0;
        return 2;
    }
    if (c == '?') {                 /* "???": emit one, keep two pending */
        out[0] = '?';
        return 1;
    }
    rep = trigraph_repl(c);
    if (rep) {
        out[0] = (char)rep;
        *q = 0;
        return 1;
    }
    out[0] = '?';
    out[1] = '?';
    out[2] = (char)c;
    *q = 0;
    return 3;
}

/* Emit the accumulated logical line and reset the buffer position. */
static void flush_line(char *buf, char *td, int *tdpos, int *in_td,
                       int depth, int *pos)
{
    buf[*pos] = 0;
    handle_logical_line(buf, td, tdpos, in_td, depth);
    cur_line++;
    *pos = 0;
}

static void process_file(const char *path, int depth)
{
    int fd = open_source(path);
    static char buf[LINE_MAX];
    char td[LINE_MAX];
    int pos = 0;
    int tdpos = 0;
    int in_td = 0;
    int q = 0;
    int saved_line = cur_line;
    const char *saved_file = cur_file;
    if (fd < 0) { warn("cannot open include"); return; }
    cur_line = 1;
    cur_file = path;
    for (;;) {
        char c;
        char emit[3];
        int n, ne, j;
        n = (int)read(fd, &c, 1);
        if (n <= 0) break;
        ne = phase1_char((unsigned char)c, &q, emit);
        for (j = 0; j < ne; j++) {
            char ch = emit[j];
            if (ch == '\n') {
                if (pos > 0 && buf[pos - 1] == '\\') {
                    /* Phase 2: delete backslash-newline, splicing the next
                     * physical line into this logical line. */
                    pos--;
                    continue;
                }
                flush_line(buf, td, &tdpos, &in_td, depth, &pos);
            } else if (pos >= LINE_MAX - 2) {
                /* Match the historical truncation: flush the line and drop
                 * the character that does not fit. */
                flush_line(buf, td, &tdpos, &in_td, depth, &pos);
            } else {
                buf[pos++] = ch;
            }
        }
    }
    while (q > 0) {                 /* flush a trailing '?' or '??' at EOF */
        if (pos >= LINE_MAX - 2)
            flush_line(buf, td, &tdpos, &in_td, depth, &pos);
        buf[pos++] = '?';
        q--;
    }
    if (pos > 0) {
        buf[pos] = 0;
        handle_logical_line(buf, td, &tdpos, &in_td, depth);
    }
    close(fd);
    cur_line = saved_line;
    cur_file = saved_file;
}

/* -D option: NAME (implies 1) or NAME=value. */
static void define_from_option(const char *opt)
{
    char name[NAME_MAX];
    const char *eq;
    int n = 0;
    while (opt[n] && n < NAME_MAX - 1 && is_ident((unsigned char)opt[n])) {
        name[n] = opt[n];
        n++;
    }
    name[n] = 0;
    if (!n) return;
    eq = strchr(opt, '=');
    define_macro(name, 0, 0, eq ? eq + 1 : "1");
}

/* -I option: append a directory to the #include search path. */
static void add_idir(const char *dir)
{
    if (nidirs >= MAX_IDIRS) { warn("too many -I directories"); return; }
    strncpy(idirs[nidirs], dir, LINE_MAX - 1);
    idirs[nidirs][LINE_MAX - 1] = 0;
    nidirs++;
}

int main(int argc, char **argv)
{
    const char *input = 0, *output = 0;
    int i;
    free_macros = -1;
    init_builtins();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            output = argv[++i];
        } else if (!strcmp(argv[i], "-D") && i + 1 < argc) {
            define_from_option(argv[++i]);
        } else if (!strncmp(argv[i], "-D", 2) && argv[i][2]) {
            define_from_option(argv[i] + 2);
        } else if (!strcmp(argv[i], "-I") && i + 1 < argc) {
            add_idir(argv[++i]);
        } else if (!strncmp(argv[i], "-I", 2) && argv[i][2]) {
            add_idir(argv[i] + 2);
        } else if (!input) {
            input = argv[i];
        }
    }
    if (!input) {
        write(2, "usage: cpp input.c -o output.i\n", 32);
        return 1;
    }
    if (output) {
        outfd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outfd < 0) { warn("cannot open output"); return 1; }
    }
    process_file(input, 0);
    if (outfd != 1) close(outfd);
    return failed ? 1 : 0;
}
