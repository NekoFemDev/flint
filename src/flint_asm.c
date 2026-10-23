/*
 * flint_asm.c — FLINT assembler: v3 text surface -> bytecode.
 *
 * Single pass + fixups: labels are defined at their output positions;
 * forward references emit a zero i64 and register a named fixup;
 * resolve at the end — undefined labels are errors. Branch/call
 * fixups demand a code-section label (F_CODE); immediates accept
 * either space (F_ANY).
 *
 * Isolated TU: libc is allowed (malloc for growable buffers);
 * a host embedding only the VM never links this file.
 *
 * Syntax v3 (v0.2.0 surface):
 *   mnemonics  set copy load[.byte] loadx store[.byte] storx
 *              add sub mul div mod and or xor shl shr inc dec neg not
 *              jump[.eq .ne .lt .ge .lo .hs] call return enter leave
 *              push pop alloc free gc sys exit
 *   registers  r0..r15
 *   mem        [rN]  [rN+num]  [rN-num]  [rN+rM]  [fp±num]  [fp+rM]
 *              (store: address first)
 *   data       str "bytes", chars, nums | int num | bytes num
 *   sections   code: / data:  (code is the default)
 *   labels     name:  |  .local:   (locals attach to last global)
 *   includes   include "file"  |  .include "file"   (same directive)
 *   comments   // to end of line
 *
 * ABI (documented in flint.h): r0..r3 args/result, r4..r11 caller-saved,
 * r12..r15 callee-saved. enter N / leave build the local frame; locals
 * are addressed as [fp-8], [fp-16], ... .
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "flint.h"

#define T_MAX 256

/* ---- growable byte buffers ---- */

typedef struct { flint_byte *b; uint32_t n, cap; } bbuf;

static int bb_put(bbuf *x, int v)
{
    if (x->n + 1 > x->cap) {
        uint32_t nc = x->cap ? x->cap * 2 : 256;
        flint_byte *nb = (flint_byte *)realloc(x->b, nc);
        if (!nb) return 0;
        x->b = nb; x->cap = nc;
    }
    x->b[x->n++] = (flint_byte)v;
    return 1;
}

static int bb_i64(bbuf *x, flint_word v)
{
    int i;
    for (i = 0; i < 8; i++)
        if (!bb_put(x, (int)(((uint64_t)v >> (8 * i)) & 0xFF))) return 0;
    return 1;
}

static void bb_patch(bbuf *x, uint32_t at, flint_word v)
{
    int i;
    for (i = 0; i < 8; i++)
        x->b[at + i] = (flint_byte)(((uint64_t)v >> (8 * i)) & 0xFF);
}

/* ---- file frame management (include) ---- */

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* "a/b/c.flt" -> "a/b"  (or "."); also accepts '\\' separators */
static char *dir_of(const char *path)
{
    const char *sl = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
#endif
    size_t n = sl ? (size_t)(sl - path) : 0;
    if (n == 0 && sl) n = 1;                    /* "/x.flt" -> "/" */
    char *d = (char *)malloc(n ? n + 1 : 2);
    if (!d) return dupstr(".");
    if (n == 0) { d[0] = '.'; d[1] = 0; return d; }
    memcpy(d, path, n); d[n] = 0;
    return d;
}

/* ---- symbols ---- */

enum { S_CODE = 0, S_DATA };
enum { F_ANY = 0, F_CODE };

typedef struct { char name[T_MAX]; flint_word addr; int kind; } symbol;

/* ---- lexer ---- */

enum { T_IDENT, T_NUM, T_STR, T_CHR, T_PUNCT, T_EOL, T_EOF };

typedef struct {
    int type;
    int len;                    /* T_STR payload length; may embed NULs */
    flint_word num;
    char s[T_MAX];
} token;

typedef struct {
    uint32_t at;                /* byte offset of the i64 field  */
    char     name[T_MAX];       /* referenced label              */
    char     file[T_MAX];       /* source file the fixup text lives in */
    int      want;              /* F_CODE: label must be code    */
    int      buf;               /* 0 = code image, 1 = data image */
    uint32_t line;
} fixup;

/* include machinery: a stack of saved parent lexer states */
#define FLINX_MAX_INC 24

typedef struct {
    const char *saved_src; uint32_t saved_ipos, saved_line;
    char *saved_name, *saved_dir;     /* strdup'd, owned by the assembler */
    int    saved_owned;               /* saved text: freed by the owner chain */
    int    saved_have; token saved_tok;
    int    saved_section;             /* section + last_global are per-file */
    char   saved_last_global[T_MAX];
} incframe;

typedef struct {
    const char *src; uint32_t ipos, line;

    char *cur_name, *cur_dir;         /* strdup'd file name + directory */
    int    cur_owned;                 /* root text is not ours; includes are */
    incframe fr[FLINX_MAX_INC]; int fsp;

    flint_include_fn inc_fn; void *inc_ud;

    bbuf     code, mem;
    symbol  *syms; uint32_t ns, cs;
    fixup   *fix;  uint32_t nf, cf;

    char     last_global[T_MAX];
    int      section;
    char     err[384];

    /* lookahead — struct fields, must survive frame swaps */
    token ahead_v; int have_v;
} asmx;

static void fail_common(asmx *a, const char *file, uint32_t line,
                        const char *fmt, va_list ap)
{
    char buf[300];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    snprintf(a->err, sizeof(a->err), "%s:%u: %s", file ? file : "<src>", line, buf);
}

static void fail(asmx *a, const char *fmt, ...)
{
    va_list ap;
    if (a->err[0]) return;                      /* first error wins */
    va_start(ap, fmt);
    fail_common(a, a->cur_name, a->line, fmt, ap);
    va_end(ap);
}

static void fail_at(asmx *a, const char *file, uint32_t line, const char *fmt, ...)
{
    va_list ap;
    if (a->err[0]) return;
    va_start(ap, fmt);
    fail_common(a, file, line, fmt, ap);
    va_end(ap);
}

static int is_idstart(int c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.'; }
static int is_id(int c)  { return is_idstart(c) || (c >= '0' && c <= '9'); }
static int is_dig(int c) { return c >= '0' && c <= '9'; }

static int esc(int c)
{
    switch (c) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
    case '0': return 0; case '\\': return '\\';
    case '\'': return '\''; case '"': return '"';
    default: return -1;
    }
}

static void next_tok(asmx *a, token *t)
{
    t->s[0] = 0; t->num = 0; t->len = 0;
    t->type = T_EOL;

    for (;;) {
        if (a->src[a->ipos] == 0) { t->type = T_EOF; return; }
        if (a->src[a->ipos] == '/' && a->src[a->ipos + 1] == '/') {
            while (a->src[a->ipos] && a->src[a->ipos] != '\n') a->ipos++;
            continue;
        }
        if (a->src[a->ipos] == ' ' || a->src[a->ipos] == '\t' ||
            a->src[a->ipos] == '\r') { a->ipos++; continue; }
        if (a->src[a->ipos] == '\n') { a->line++; a->ipos++; t->type = T_EOL; return; }
        break;
    }

    if (is_idstart(a->src[a->ipos])) {
        int n = 0;
        while (is_id(a->src[a->ipos]) && n < T_MAX - 1)
            t->s[n++] = a->src[a->ipos++];
        t->s[n] = 0; t->type = T_IDENT; return;
    }
    if (a->src[a->ipos] == '"') {
        int n = 0;
        a->ipos++;
        while (a->src[a->ipos] && a->src[a->ipos] != '"') {
            char c = a->src[a->ipos];
            if (n >= T_MAX - 1) { fail(a, "string too long"); return; }
            if (c == '\\' && a->src[a->ipos + 1]) {
                int e = esc(a->src[a->ipos + 1]);
                if (e < 0) { fail(a, "bad escape \\%c", a->src[a->ipos + 1]); return; }
                t->s[n++] = (char)e; a->ipos += 2;
            } else { t->s[n++] = c; a->ipos++; }
        }
        if (a->src[a->ipos] != '"') { fail(a, "unterminated string"); return; }
        a->ipos++;
        t->s[n] = 0; t->len = n; t->type = T_STR; return;
    }
    if (a->src[a->ipos] == '\'') {
        int v;
        a->ipos++;
        if (a->src[a->ipos] == '\\') {
            v = esc(a->src[a->ipos + 1]);
            if (v < 0 || !a->src[a->ipos + 1]) { fail(a, "bad char literal"); return; }
            a->ipos += 2;
        } else {
            if (!a->src[a->ipos]) { fail(a, "bad char literal"); return; }
            v = (unsigned char)a->src[a->ipos]; a->ipos++;
        }
        if (a->src[a->ipos] != '\'') { fail(a, "expected closing quote"); return; }
        a->ipos++;
        t->num = v; t->type = T_CHR; return;
    }
    if (is_dig(a->src[a->ipos])) {
        uint64_t v = 0;
        if (a->src[a->ipos] == '0' &&
            (a->src[a->ipos + 1] == 'x' || a->src[a->ipos + 1] == 'X')) {
            int ndig = 0;
            a->ipos += 2;
            for (;;) {
                int c = a->src[a->ipos], d;
                if (is_dig(c)) d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                v = v * 16 + (uint64_t)d; a->ipos++; ndig++;
            }
            if (ndig == 0) { fail(a, "empty 0x literal"); t->type = T_EOF; return; }
        } else {
            while (is_dig(a->src[a->ipos])) {
                v = v * 10 + (uint64_t)(a->src[a->ipos] - '0'); a->ipos++;
            }
        }
        t->num = (flint_word)v; t->type = T_NUM; return;
    }
    switch (a->src[a->ipos]) {
    case '[': case ']': case ',': case ':': case '+': case '-':
        t->type = T_PUNCT; t->s[0] = a->src[a->ipos]; t->s[1] = 0;
        a->ipos++; return;
    default:
        fail(a, "stray character '%c'", a->src[a->ipos]);
        a->ipos++; t->type = T_EOF; return;
    }
}

/* lookahead machinery — per-asmx, survives include frame swaps */

static void pk(asmx *a, token *t)
{
    if (!a->have_v) {
        next_tok(a, &a->ahead_v);
        a->have_v = 1;
    }
    *t = a->ahead_v;
}

static void nx(asmx *a, token *t)
{
    if (a->have_v) { *t = a->ahead_v; a->have_v = 0; }
    else next_tok(a, t);
}

/* ---- symbols / fixups ---- */

static int sym_add(asmx *a, const char *name, flint_word addr, int kind)
{
    uint32_t i;
    for (i = 0; i < a->ns; i++)
        if (strcmp(a->syms[i].name, name) == 0) {
            fail(a, "duplicate label \"%s\"", name);
            return -1;
        }
    if (a->ns + 1 > a->cs) {
        uint32_t nc = a->cs ? a->cs * 2 : 64;
        symbol *ns = (symbol *)realloc(a->syms, nc * sizeof(symbol));
        if (!ns) { fail(a, "out of memory"); return -1; }
        a->syms = ns; a->cs = nc;
    }
    memset(a->syms + a->ns, 0, sizeof(symbol));
    snprintf(a->syms[a->ns].name, T_MAX, "%s", name);
    a->syms[a->ns].addr = addr; a->syms[a->ns].kind = kind;
    return (int)a->ns++;
}

static int sym_find(asmx *a, const char *name)
{
    uint32_t i;
    for (i = 0; i < a->ns; i++)
        if (strcmp(a->syms[i].name, name) == 0) return (int)i;
    return -1;
}

static void full_local(asmx *a, const char *l, char *out, unsigned n)
{
    if (l[0] == '.') snprintf(out, n, "%s%s", a->last_global, l);
    else             snprintf(out, n, "%s", l);
}

static int fx_add(asmx *a, uint32_t at, const char *name, int want, int buf)
{
    if (a->nf + 1 > a->cf) {
        uint32_t nc = a->cf ? a->cf * 2 : 64;
        fixup *nf = (fixup *)realloc(a->fix, nc * sizeof(fixup));
        if (!nf) { fail(a, "out of memory"); return 0; }
        a->fix = nf; a->cf = nc;
    }
    a->fix[a->nf].at = at;
    snprintf(a->fix[a->nf].name, T_MAX, "%s", name);
    snprintf(a->fix[a->nf].file, T_MAX, "%s", a->cur_name ? a->cur_name : "<src>");
    a->fix[a->nf].want = want;
    a->fix[a->nf].buf  = buf;
    a->fix[a->nf].line = a->line;
    a->nf++;
    return 1;
}

/* ---- operands ---- */

enum { O_REG, O_NUM, O_SYM, O_MEM };

typedef struct {
    int        kind;
    uint8_t    reg;            /* O_REG / O_MEM base (may be FLINT_REG_FP) */
    uint8_t    idx;            /* O_MEM indexed form: the rM of [rN+rM] */
    int        indexed;        /* O_MEM: 1 = [rN+rM], 0 = [rN±imm] */
    flint_word num;            /* O_NUM / O_MEM offset */
    char       name[T_MAX];    /* O_SYM */
} opr;

/* r0..r15, strictly; the fp pseudo-register is handled separately and is
 * not a general register */
static int reg_no(const token *t)
{
    int n;
    if (t->type != T_IDENT || t->s[0] != 'r') return -1;
    if (t->s[1] < '0' || t->s[1] > '9') return -1;
    if (t->s[2] == 0) {
        n = t->s[1] - '0';
    } else if (t->s[2] >= '0' && t->s[2] <= '9' && t->s[3] == 0) {
        n = (t->s[1] - '0') * 10 + (t->s[2] - '0');
    } else {
        return -1;
    }
    return (n < FLINT_NREGS) ? n : -1;
}

static int is_fp(const token *t)
{
    return t->type == T_IDENT && strcmp(t->s, "fp") == 0;
}

/* address-base operand: a general register or fp (encoded as FLINT_REG_FP) */
static int mem_base_no(const token *t)
{
    int n = reg_no(t);
    if (n >= 0) return n;
    if (is_fp(t)) return FLINT_REG_FP;
    return -1;
}

static int parse_reg(asmx *a, int *r, token *t)
{
    int n;
    pk(a, t);
    n = reg_no(t);
    if (n < 0) { fail(a, "expected register r0..r15"); return 0; }
    nx(a, t);
    *r = n;
    return 1;
}

static int parse_imm(asmx *a, opr *o, token *t)
{
    pk(a, t);
    if (t->type == T_PUNCT && t->s[0] == '-') {
        nx(a, t);
        pk(a, t);
        if (t->type != T_NUM) { fail(a, "expected number after '-'"); return 0; }
        nx(a, t);
        o->kind = O_NUM; o->num = -t->num; return 1;
    }
    if (t->type == T_NUM || t->type == T_CHR) {
        nx(a, t);
        o->kind = O_NUM; o->num = t->num; return 1;
    }
    if (is_fp(t)) {
        fail(a, "'fp' is only valid as a memory base");
        return 0;
    }
    if (t->type == T_IDENT && reg_no(t) < 0) {
        char full[T_MAX];
        nx(a, t);
        full_local(a, t->s, full, sizeof(full));
        o->kind = O_SYM;
        snprintf(o->name, sizeof(o->name), "%s", full);
        return 1;
    }
    fail(a, "expected immediate or label");
    return 0;
}

/* [rN] | [rN±imm] | [rN+rM] | [fp±imm] | [fp+rM]
 * The indexed form [rN+rM] sets o->indexed and o->idx; the standard form
 * leaves o->indexed = 0 and stores the offset in o->num. */
static int parse_mem(asmx *a, opr *o, token *t)
{
    int rb;
    o->kind = O_MEM;
    o->num = 0;
    o->indexed = 0;
    o->idx = 0;

    pk(a, t);
    if (t->type != T_PUNCT || t->s[0] != '[') { fail(a, "expected '['"); return 0; }
    nx(a, t);

    pk(a, t);
    rb = mem_base_no(t);
    if (rb < 0) { fail(a, "expected register or fp after '['"); return 0; }
    nx(a, t);
    o->reg = (uint8_t)rb;

    pk(a, t);
    if (t->type == T_PUNCT && (t->s[0] == '+' || t->s[0] == '-')) {
        int neg = (t->s[0] == '-');
        nx(a, t);
        pk(a, t);
        if (!neg && reg_no(t) >= 0) {              /* [rb+ri] */
            o->idx = (uint8_t)reg_no(t);
            o->indexed = 1;
            nx(a, t);
        } else if (t->type == T_NUM) {             /* [rb±imm] */
            nx(a, t);
            o->num = neg ? -t->num : t->num;
        } else {
            fail(a, "expected offset number or index register");
            return 0;
        }
    }
    pk(a, t);
    if (t->type != T_PUNCT || t->s[0] != ']') { fail(a, "expected ']'"); return 0; }
    nx(a, t);
    return 1;
}

/* ---- emit helpers (code section) ---- */

static void e_cst(asmx *a, int v)   { if (!bb_put(&a->code, v)) fail(a, "out of memory"); }
static void e_ci64(asmx *a, flint_word v)
{
    if (!bb_i64(&a->code, v)) fail(a, "out of memory");
}
static void e_regb(asmx *a, int r)  { e_cst(a, r); }

/* 8-byte field: O_NUM/O_MEM emit raw; O_SYM registers a fixup */
static void e_field(asmx *a, const opr *o, int want)
{
    uint32_t at = a->code.n;
    e_ci64(a, o->num);
    if (o->kind == O_SYM) fx_add(a, at, o->name, want, 0);
}

/* raw 8-byte immediate, no fixup */
static void e_imm(asmx *a, flint_word v) { e_ci64(a, v); }

/* ---- dispatch table ---- */

enum {
    N_SET, N_COPY, N_LOAD, N_LOADB, N_LOADX, N_STORE, N_STOREB, N_STORX,
    N_ARITH, N_INC, N_DEC, N_NEG, N_NOT,
    N_JUMP, N_BRANCH, N_CALL, N_RET, N_ENTER, N_LEAVE,
    N_PUSH, N_POP, N_ALLOC, N_FREE, N_GC, N_SYS, N_EXIT,
    D_STR, D_INT, D_BYTES
};

typedef struct { const char *nm; int cls; int opA, opB; } mnem_t;

static const mnem_t g_tab[] = {
    {"set",        N_SET,   0, 0},
    {"copy",       N_COPY,  0, 0},
    {"load",       N_LOAD,  0, 0},
    {"load.byte",  N_LOADB, 0, 0},
    {"loadx",      N_LOADX, 0, 0},
    {"store",      N_STORE, 0, 0},
    {"store.byte", N_STOREB, 0, 0},
    {"storx",      N_STORX, 0, 0},
    {"add",        N_ARITH, OP_ADD,  OP_ADDI},
    {"sub",        N_ARITH, OP_SUB,  OP_SUBI},
    {"mul",        N_ARITH, OP_MUL,  OP_MULI},
    {"div",        N_ARITH, OP_DIV,  OP_DIVI},
    {"mod",        N_ARITH, OP_MOD,  OP_MODI},
    {"and",        N_ARITH, OP_AND,  OP_ANDI},
    {"or",         N_ARITH, OP_OR,   OP_ORI},
    {"xor",        N_ARITH, OP_XOR,  OP_XORI},
    {"shl",        N_ARITH, OP_SHL,  OP_SHLI},
    {"shr",        N_ARITH, OP_SHR,  OP_SHRI},
    {"inc",        N_INC,   0, 0},
    {"dec",        N_DEC,   0, 0},
    {"neg",        N_NEG,   0, 0},
    {"not",        N_NOT,   0, 0},
    {"jump",       N_JUMP,  0, 0},
    {"jump.eq",    N_BRANCH, OP_JEQ, OP_JEQI},
    {"jump.ne",    N_BRANCH, OP_JNE, OP_JNEI},
    {"jump.lt",    N_BRANCH, OP_JLT, OP_JLTI},
    {"jump.ge",    N_BRANCH, OP_JGE, OP_JGEI},
    {"jump.lo",    N_BRANCH, OP_JLO, OP_JLOI},
    {"jump.hs",    N_BRANCH, OP_JHS, OP_JHSI},
    {"call",       N_CALL,  0, 0},
    {"return",     N_RET,   0, 0},
    {"enter",      N_ENTER, 0, 0},
    {"leave",      N_LEAVE, 0, 0},
    {"push",       N_PUSH,  0, 0},
    {"pop",        N_POP,   0, 0},
    {"alloc",      N_ALLOC, 0, 0},
    {"free",       N_FREE,  0, 0},
    {"gc",         N_GC,    0, 0},
    {"sys",        N_SYS,   0, 0},
    {"exit",       N_EXIT,  0, 0},
    {"str",        D_STR,   0, 0},
    {"int",        D_INT,   0, 0},
    {"bytes",      D_BYTES, 0, 0},
};

static const mnem_t *find_mnem(const char *nm)
{
    uint32_t i;
    for (i = 0; i < sizeof(g_tab) / sizeof(g_tab[0]); i++)
        if (strcmp(g_tab[i].nm, nm) == 0) return &g_tab[i];
    return 0;
}

/* expect ',' between operands */
static int need_comma(asmx *a, token *t)
{
    pk(a, t);
    if (t->type == T_PUNCT && t->s[0] == ',') { nx(a, t); return 1; }
    if (t->type == T_IDENT || t->type == T_NUM || t->type == T_CHR ||
        (t->type == T_PUNCT && t->s[0] == '[')) {
        fail(a, "expected ','"); return 0;
    }
    return 1;    /* EOL — nothing more on this line */
}

/* ---- statement encoders (code section) ---- */

static void m_set(asmx *a)
{
    int rd; opr o; token t;
    if (!parse_reg(a, &rd, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_imm(a, &o, &t)) return;
    e_cst(a, OP_SET); e_regb(a, rd); e_field(a, &o, F_ANY);
}

static void m_copy(asmx *a)
{
    int rd, rs; token t;
    if (!parse_reg(a, &rd, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_reg(a, &rs, &t)) return;
    e_cst(a, OP_COPY); e_regb(a, rd); e_regb(a, rs);
}

/* load[.byte] rd, [rb±off]      -> OP_LOADB / OP_LOADW
 * load         rd, [rb+ri]       -> OP_LOADX (word-only) */
static void m_load(asmx *a, int byte)
{
    int rd; opr mo; token t;
    if (!parse_reg(a, &rd, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_mem(a, &mo, &t)) return;
    if (mo.indexed) {
        if (byte) { fail(a, "indexed addressing is word-only (load/store)"); return; }
        e_cst(a, OP_LOADX);
        e_regb(a, rd); e_regb(a, mo.reg); e_regb(a, mo.idx);
        return;
    }
    e_cst(a, byte ? OP_LOADB : OP_LOADW);
    e_regb(a, rd); e_regb(a, mo.reg); e_imm(a, mo.num);
}

/* store[.byte] [rb±off], rs     -> OP_STOREB / OP_STOREW
 * store        [rb+ri], rs      -> OP_STOREX (word-only) */
static void m_store(asmx *a, int byte)
{
    int rs; opr mo; token t;
    if (!parse_mem(a, &mo, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_reg(a, &rs, &t)) return;
    if (mo.indexed) {
        if (byte) { fail(a, "indexed addressing is word-only (load/store)"); return; }
        e_cst(a, OP_STOREX);
        e_regb(a, mo.reg); e_regb(a, mo.idx); e_regb(a, rs);
        return;
    }
    e_cst(a, byte ? OP_STOREB : OP_STOREW);
    e_regb(a, mo.reg); e_regb(a, rs); e_imm(a, mo.num);
}

/* explicit indexed forms: fail loudly if the operand is not [rb+ri] */
static void m_loadx(asmx *a)
{
    int rd; opr mo; token t;
    if (!parse_reg(a, &rd, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_mem(a, &mo, &t)) return;
    if (!mo.indexed) { fail(a, "loadx requires [rb+ri]"); return; }
    e_cst(a, OP_LOADX);
    e_regb(a, rd); e_regb(a, mo.reg); e_regb(a, mo.idx);
}

static void m_storx(asmx *a)
{
    int rs; opr mo; token t;
    if (!parse_mem(a, &mo, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_reg(a, &rs, &t)) return;
    if (!mo.indexed) { fail(a, "storx requires [rb+ri]"); return; }
    e_cst(a, OP_STOREX);
    e_regb(a, mo.reg); e_regb(a, mo.idx); e_regb(a, rs);
}

/* add rd, ra, rb    -> three-address
 * add rd, ra, imm   -> immediate form
 * add rd, imm       -> sugar: rd = rd op imm                          */
static void m_arith(asmx *a, int rop, int iop)
{
    int rd, ra, rb; opr o; token t;
    if (!parse_reg(a, &rd, &t)) return;
    if (!need_comma(a, &t)) return;

    pk(a, &t);
    if (reg_no(&t) >= 0) {
        if (!parse_reg(a, &ra, &t)) return;
        pk(a, &t);
        if (t.type == T_EOL || t.type == T_EOF) {
            fail(a, "expected third operand");
            return;
        }
        if (!need_comma(a, &t)) return;
        pk(a, &t);
        if (reg_no(&t) >= 0) {
            if (!parse_reg(a, &rb, &t)) return;
            e_cst(a, rop); e_regb(a, rd); e_regb(a, ra); e_regb(a, rb);
        } else {
            if (!parse_imm(a, &o, &t)) return;
            e_cst(a, iop); e_regb(a, rd); e_regb(a, ra); e_field(a, &o, F_ANY);
        }
    } else {
        if (!parse_imm(a, &o, &t)) return;
        e_cst(a, iop); e_regb(a, rd); e_regb(a, rd); e_field(a, &o, F_ANY);
    }
}

static void m_unary2(asmx *a, int op)
{
    int rd, rs; token t;
    if (!parse_reg(a, &rd, &t)) return;
    pk(a, &t);
    if (t.type == T_PUNCT && t.s[0] == ',') {
        nx(a, &t);
        if (!parse_reg(a, &rs, &t)) return;
    } else rs = rd;                          /* neg r1 == neg r1, r1 */
    e_cst(a, op); e_regb(a, rd); e_regb(a, rs);
}

static void m_incdec(asmx *a, int op)
{
    int rd; token t;
    if (!parse_reg(a, &rd, &t)) return;
    e_cst(a, op); e_regb(a, rd);
}

static void m_jump(asmx *a)
{
    token t;
    pk(a, &t);
    if (t.type == T_IDENT) {
        opr o; if (!parse_imm(a, &o, &t)) return;
        e_cst(a, OP_JMP); e_field(a, &o, F_CODE);
    } else {
        fail(a, "expected label after jump");
    }
}

/* jump.eq rs, rt|imm, target   |  sugar: jump.eq rs, target */
static void m_branch(asmx *a, int bop, int bopi)
{
    int rs, rt; opr cmp, tgt; token t;
    cmp.kind = O_NUM; cmp.num = 0;

    if (!parse_reg(a, &rs, &t)) return;
    if (!need_comma(a, &t)) return;
    pk(a, &t);

    if (reg_no(&t) >= 0) {                       /* rs vs rt|imm */
        if (!parse_reg(a, &rt, &t)) return;
        if (!need_comma(a, &t)) { fail(a, "expected branch target"); return; }
        pk(a, &t);
        if (t.type == T_EOL || t.type == T_EOF) {
            fail(a, "expected branch target"); return;
        }
        if (!parse_imm(a, &tgt, &t)) return;
        e_cst(a, bop); e_regb(a, rs); e_regb(a, rt);
        e_field(a, &tgt, F_CODE);
        return;
    }

    /* second operand is immediate or label */
    if (!parse_imm(a, &cmp, &t)) return;
    pk(a, &t);
    if (t.type == T_EOL || t.type == T_EOF) {
        /* sugar: jump.eq r1, done  ==>  JEQI r1, 0, done */
        e_cst(a, bopi); e_regb(a, rs); e_imm(a, 0);
        e_field(a, &cmp, F_CODE);
        return;
    }
    if (!need_comma(a, &t)) return;
    pk(a, &t);
    if (t.type == T_EOL || t.type == T_EOF) {
        fail(a, "expected branch target"); return;
    }
    if (!parse_imm(a, &tgt, &t)) return;
    e_cst(a, bopi); e_regb(a, rs);
    e_field(a, &cmp, F_ANY);
    e_field(a, &tgt, F_CODE);
}

static void m_call(asmx *a)
{
    token t;
    pk(a, &t);
    if (t.type == T_IDENT) {
        opr o; if (!parse_imm(a, &o, &t)) return;
        e_cst(a, OP_CALL); e_field(a, &o, F_CODE);
    } else {
        fail(a, "expected label after call");
    }
}

static void m_push(asmx *a)
{
    token t;
    pk(a, &t);
    if (reg_no(&t) >= 0) {
        int rs; if (!parse_reg(a, &rs, &t)) return;
        e_cst(a, OP_PUSH); e_regb(a, rs);
    } else {
        opr o; if (!parse_imm(a, &o, &t)) return;
        e_cst(a, OP_PUSHI); e_field(a, &o, F_ANY);
    }
}

static void m_pop(asmx *a)
{
    int rd; token t;
    if (!parse_reg(a, &rd, &t)) return;
    e_cst(a, OP_POP); e_regb(a, rd);
}

/* alloc rd, imm   -> rd = heap cursor, cursor += imm */
static void m_alloc(asmx *a)
{
    int rd; opr o; token t;
    if (!parse_reg(a, &rd, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_imm(a, &o, &t)) return;
    e_cst(a, OP_ALLOC); e_regb(a, rd); e_field(a, &o, F_ANY);
}

/* free rs         -> validated no-op (auto: the collector owns lifetime) */
static void m_free(asmx *a)
{
    int rs; token t;
    if (!parse_reg(a, &rs, &t)) return;
    e_cst(a, OP_FREE); e_regb(a, rs);
}

/* gc              -> run a collection in auto mode (no-op in manual) */
static void m_gc(asmx *a)
{
    e_cst(a, OP_GC);
}

static void m_sys(asmx *a)
{
    token t;
    pk(a, &t);
    if (t.type != T_NUM) { fail(a, "expected syscall number"); return; }
    nx(a, &t);
    if (t.num < 0 || t.num > 255) { fail(a, "syscall number out of range"); return; }
    e_cst(a, OP_INT); e_cst(a, (int)t.num);
}

/* exit: OP_HLT */
static void m_exit(asmx *a) { e_cst(a, OP_HLT); }
static void m_ret(asmx *a)  { e_cst(a, OP_RET); }

/* enter N: frame prologue — push fp, fp = sp, reserve N local bytes */
static void m_enter(asmx *a)
{
    opr o; token t;
    if (!parse_imm(a, &o, &t)) return;
    if (o.kind == O_NUM && o.num < 0) { fail(a, "enter size must be >= 0"); return; }
    e_cst(a, OP_ENTER); e_field(a, &o, F_ANY);
}

/* leave: frame epilogue — sp = fp, pop fp */
static void m_leave(asmx *a) { e_cst(a, OP_LEAVE); }

/* ---- data directives (mem section) ---- */

static void d_str(asmx *a)
{
    token t;
    pk(a, &t);
    if (t.type == T_STR) {
        int i;
        nx(a, &t);
        /* walk len, not NUL: \0 bytes inside the literal reach the image */
        for (i = 0; i < t.len; i++) {
            if (!bb_put(&a->mem, (int)(unsigned char)t.s[i])) fail(a, "out of memory");
        }
    } else if (t.type == T_NUM || t.type == T_CHR) {
        opr o;
        nx(a, &t);
        o.kind = O_NUM; o.num = t.num;
        if (o.num < 0 || o.num > 255) { fail(a, "str byte out of range"); return; }
        if (!bb_put(&a->mem, (int)o.num)) fail(a, "out of memory");
    } else {
        fail(a, "expected string literal");
        return;
    }
    for (;;) {                                   /* ", byte" tail */
        pk(a, &t);
        if (t.type == T_PUNCT && t.s[0] == ',') {
            nx(a, &t);
            pk(a, &t);
            if (t.type != T_NUM && t.type != T_CHR) { fail(a, "expected byte value"); return; }
            if (t.num < 0 || t.num > 255) { fail(a, "str byte out of range"); return; }
            nx(a, &t);
            if (!bb_put(&a->mem, (int)t.num)) fail(a, "out of memory");
            continue;
        }
        break;
    }
}

/* int 42 | int 10, 20, 30, 40 | int -5, 7 — each value is one word */
static void d_int(asmx *a)
{
    token t;
    for (;;) {
        int neg = 0;
        flint_word v;
        pk(a, &t);
        if (t.type == T_PUNCT && t.s[0] == '-') {
            neg = 1;
            nx(a, &t);
            pk(a, &t);
        }
        if (t.type != T_NUM) { fail(a, "expected number"); return; }
        nx(a, &t);
        v = neg ? -t.num : t.num;
        if (!bb_i64(&a->mem, v)) { fail(a, "out of memory"); return; }
        pk(a, &t);
        if (t.type == T_PUNCT && t.s[0] == ',') { nx(a, &t); continue; }
        break;
    }
}

/* bytes 16 | bytes 16, 8 — zero runs, comma-separated */
static void d_bytes(asmx *a)
{
    token t;
    for (;;) {
        uint32_t n;
        pk(a, &t);
        if (t.type != T_NUM) { fail(a, "expected size"); return; }
        nx(a, &t);
        if (t.num < 0) { fail(a, "bytes size must be >= 0"); return; }
        for (n = 0; n < (uint32_t)t.num; n++) {
            if (!bb_put(&a->mem, 0)) fail(a, "out of memory");
        }
        pk(a, &t);
        if (t.type == T_PUNCT && t.s[0] == ',') { nx(a, &t); continue; }
        break;
    }
}

/* ---- program scanner ---- */

static void asm_define_label(asmx *a, const token *tname)
{
    char full[T_MAX];
    flint_word pos;

    full_local(a, tname->s, full, sizeof(full));
    pos = (a->section == S_CODE) ? (flint_word)a->code.n : (flint_word)a->mem.n;
    if (sym_add(a, full, pos, a->section) < 0) return;
    if (tname->s[0] != '.')
        snprintf(a->last_global, sizeof(a->last_global), "%s", tname->s);
}

/* ---- include ---- */

static void pop_file(asmx *a)
{
    incframe *f = &a->fr[--a->fsp];
    if (a->cur_owned) free((char *)a->src);
    free(a->cur_name);
    free(a->cur_dir);
    a->src      = f->saved_src;
    a->ipos     = f->saved_ipos;
    a->line     = f->saved_line;
    a->cur_name = f->saved_name;
    a->cur_dir  = f->saved_dir;
    a->cur_owned = f->saved_owned;
    a->have_v   = f->saved_have;
    a->ahead_v  = f->saved_tok;
    a->section  = f->saved_section;
    memcpy(a->last_global, f->saved_last_global, sizeof(a->last_global));
}

static void do_include(asmx *a)
{
    token t;
    char path[T_MAX], ierr[256];

    pk(a, &t);
    if (t.type != T_STR) {
        fail(a, "include takes a quoted path");
        return;
    }
    if (strlen(t.s) != (size_t)t.len) {
        fail(a, "include path contains NUL");
        return;
    }
    snprintf(path, sizeof(path), "%s", t.s);
    nx(a, &t);                                  /* consume the string       */
    pk(a, &t);
    if (t.type != T_EOL && t.type != T_EOF) {
        fail(a, "unexpected text after include");
        return;
    }
    nx(a, &t);                                  /* eat the EOL (if any)     */

    if (!a->inc_fn) {
        fail(a, "include unavailable: no resolver (plain flint_assemble?)");
        return;
    }
    if (a->fsp >= FLINX_MAX_INC) {
        fail(a, "include depth over %d", FLINX_MAX_INC);
        return;
    }

    ierr[0] = 0;
    char *text = a->inc_fn(a->inc_ud, path, a->cur_dir ? a->cur_dir : ".",
                           ierr, (uint32_t)sizeof(ierr));
    if (!text) {
        fail(a, "include \"%s\": %s", path, ierr[0] ? ierr : "not found");
        return;
    }
    if (!text[0]) {                             /* already included, skip   */
        free(text);
        return;
    }
    if (a->err[0]) { free(text); return; }      /* resolver failed mid-way?? */
    if (!a->cur_name || !a->cur_dir) { free(text); fail(a, "out of memory"); return; }

    incframe *f = &a->fr[a->fsp++];
    f->saved_src   = a->src;
    f->saved_ipos  = a->ipos;
    f->saved_line  = a->line;
    f->saved_name  = a->cur_name;
    f->saved_dir   = a->cur_dir;
    f->saved_owned = a->cur_owned;
    f->saved_have  = a->have_v;
    f->saved_tok   = a->ahead_v;
    f->saved_section = a->section;
    memcpy(f->saved_last_global, a->last_global, sizeof(f->saved_last_global));

    a->src       = text;
    a->ipos      = 0;
    a->line      = 1;
    a->cur_owned = 1;
    a->have_v    = 0;
    a->ahead_v.type = T_EOF; a->ahead_v.len = 0; a->ahead_v.s[0] = 0; a->ahead_v.num = 0;
    a->cur_name  = dupstr(path);
    a->cur_dir   = dir_of(path);
    if (!a->cur_name || !a->cur_dir) {
        free(a->cur_name); free(a->cur_dir);
        a->cur_name = f->saved_name; a->cur_dir = f->saved_dir;   /* undo */
        a->src = f->saved_src; a->ipos = f->saved_ipos; a->line = f->saved_line;
        a->cur_owned = f->saved_owned; a->have_v = f->saved_have; a->ahead_v = f->saved_tok;
        a->section = f->saved_section;
        memcpy(a->last_global, f->saved_last_global, sizeof(a->last_global));
        a->fsp--;
        free(text);
        fail(a, "out of memory");
    }
}

static int asm_statement(asmx *a, const mnem_t *m, const char *nm)
{
    token t;
    (void)nm;

    switch (m->cls) {
    case N_SET:    m_set(a);      break;
    case N_COPY:   m_copy(a);     break;
    case N_LOAD:   m_load(a, 0);  break;
    case N_LOADB:  m_load(a, 1);  break;
    case N_LOADX:  m_loadx(a);    break;
    case N_STORE:  m_store(a, 0); break;
    case N_STOREB: m_store(a, 1); break;
    case N_STORX:  m_storx(a);    break;
    case N_ARITH:  m_arith(a, m->opA, m->opB); break;
    case N_INC:    m_incdec(a, OP_INC); break;
    case N_DEC:    m_incdec(a, OP_DEC); break;
    case N_NEG:    m_unary2(a, OP_NEG); break;
    case N_NOT:    m_unary2(a, OP_NOT); break;
    case N_JUMP:   m_jump(a);     break;
    case N_BRANCH: m_branch(a, m->opA, m->opB); break;
    case N_CALL:   m_call(a);     break;
    case N_RET:    m_ret(a);      break;
    case N_ENTER:  m_enter(a);    break;
    case N_LEAVE:  m_leave(a);    break;
    case N_PUSH:   m_push(a);     break;
    case N_POP:    m_pop(a);      break;
    case N_ALLOC:  m_alloc(a);    break;
    case N_FREE:   m_free(a);     break;
    case N_GC:     m_gc(a);       break;
    case N_SYS:    m_sys(a);      break;
    case N_EXIT:   m_exit(a);     break;
    case D_STR:    d_str(a);      break;
    case D_INT:    d_int(a);      break;
    case D_BYTES:  d_bytes(a);    break;
    default:       pk(a, &t); fail(a, "internal dispatch miss"); break;
    }

    /* one statement per line: nothing may follow but EOL/EOF */
    if (!a->err[0]) {
        pk(a, &t);
        if (t.type != T_EOL && t.type != T_EOF) {
            fail(a, "unexpected text after statement");
            return -1;
        }
    }
    return 0;
}

static int asm_scan(asmx *a)
{
    token t, t2;

    for (;;) {
        pk(a, &t);
        if (t.type == T_EOF || a->err[0]) {
            if (!a->err[0] && t.type == T_EOF && a->fsp > 0) {
                pop_file(a);
                continue;
            }
            break;
        }

        if (t.type == T_EOL) { nx(a, &t); continue; }

        if (t.type != T_IDENT) {
            fail(a, "expected mnemonic, label or section");
            break;
        }
        nx(a, &t);                          /* ident consumed */

        pk(a, &t2);
        if (t2.type == T_PUNCT && t2.s[0] == ':') {
            nx(a, &t2);                    /* ':' */
            if (strcmp(t.s, "code") == 0) { a->section = S_CODE; continue; }
            if (strcmp(t.s, "data") == 0) { a->section = S_DATA; continue; }
            asm_define_label(a, &t);
            continue;
        }

        if (strcmp(t.s, "include") == 0 || strcmp(t.s, ".include") == 0) {
            do_include(a); continue;
        }

        {
            const mnem_t *m = find_mnem(t.s);
            if (!m) { fail(a, "unknown mnemonic \"%s\"", t.s); break; }
            if (a->section == S_CODE && (m->cls == D_STR || m->cls == D_INT || m->cls == D_BYTES)) {
                fail(a, "data directive \"%s\" in code section", t.s);
                break;
            }
            if (a->section == S_DATA && m->cls != D_STR && m->cls != D_INT && m->cls != D_BYTES) {
                fail(a, "instruction \"%s\" in data section", t.s);
                break;
            }
            if (asm_statement(a, m, t.s) != 0) break;
        }
    }
    if (a->err[0]) return -1;
    return 0;
}

static int asm_resolve(asmx *a)
{
    uint32_t i;
    for (i = 0; i < a->nf; i++) {
        int s = sym_find(a, a->fix[i].name);
        if (s < 0) {
            fail_at(a, a->fix[i].file, a->fix[i].line, "undefined label \"%s\"",
                    a->fix[i].name);
            return -1;
        }
        if (a->fix[i].want == F_CODE && a->syms[s].kind != S_CODE) {
            fail_at(a, a->fix[i].file, a->fix[i].line, "\"%s\" is not a code label",
                    a->fix[i].name);
            return -1;
        }
        if (a->fix[i].buf == 0) bb_patch(&a->code, a->fix[i].at, a->syms[s].addr);
        else                    bb_patch(&a->mem,  a->fix[i].at, a->syms[s].addr);
    }
    return 0;
}

/* ---- standalone include preprocessor (host convenience) ----
 *
 * flint_pp_includes() textually expands
 *     include "path"      .include "path"
 * lines before assembly, resolving each path relative to the file that
 * contains it (fopen). For hosts that do not register a resolver with
 * flint_assemble_ctx — the CLI front-end uses the streaming resolver
 * instead (per-file line numbers, include-once semantics).
 * Cycles die on the depth cap.
 */

#define PP_DEFAULT_DEPTH 10

typedef struct { char *s; uint32_t n, cap; int oom; } ppbuf;

static int pp_put(ppbuf *b, const char *p, uint32_t n)
{
    if (b->oom) return 0;
    if ((uint64_t)b->n + n + 1 > b->cap) {
        uint32_t nc = b->cap ? b->cap : 1024;
        char *ns;
        while ((uint64_t)nc < (uint64_t)b->n + n + 1) {
            if (nc > 0x40000000u) { b->oom = 1; return 0; }
            nc *= 2;
        }
        ns = (char *)realloc(b->s, nc);
        if (!ns) { b->oom = 1; return 0; }
        b->s = ns; b->cap = nc;
    }
    if (n) memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
    return 1;
}

/* "dir" + "/" + "name"; absolute names pass through untouched */
static char *pp_join(const char *dir, const char *name)
{
    size_t dl, nl;
    char *p;
    if (name[0] == '/' || name[0] == 0) return dupstr(name);
#ifdef _WIN32
    if (name[1] == ':') return dupstr(name);
#endif
    dl = strlen(dir);
    nl = strlen(name);
    p = (char *)malloc(dl + nl + 2);
    if (!p) return 0;
    memcpy(p, dir, dl);
    p[dl] = '/';
    memcpy(p + dl + 1, name, nl + 1);
    return p;
}

/* slurp a file; *out_len optional */
static char *pp_read_whole(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;

    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    len = ftell(f);
    if (len < 0) { fclose(f); return 0; }
    rewind(f);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return 0; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(buf); return 0;
    }
    buf[len] = 0;
    fclose(f);
    if (out_len) *out_len = len;
    return buf;
}

/* classify one line (sans '\n'):
 *   0 = plain text, 1 = include (path filled), -1 = malformed include */
static int pp_line(const char *l, uint32_t len, char *path, uint32_t cap)
{
    uint32_t i = 0, j, k = 0;

    while (i < len && (l[i] == ' ' || l[i] == '\t')) i++;
    if      (i + 7 <= len && memcmp(l + i, "include", 7) == 0) j = i + 7;
    else if (i + 8 <= len && memcmp(l + i, ".include", 8) == 0) j = i + 8;
    else return 0;

    /* word boundary: "includeX" is an ordinary token, not a directive */
    if (j < len && l[j] != ' ' && l[j] != '\t' && l[j] != '"' && l[j] != '\r')
        return 0;
    while (j < len && (l[j] == ' ' || l[j] == '\t')) j++;
    if (j >= len || l[j] != '"') return -1;          /* path missing */
    j++;
    while (j < len && l[j] != '"') {
        if (k + 1 >= cap) return -1;                 /* path too long */
        path[k++] = l[j++];
    }
    if (j >= len) return -1;                         /* unterminated  */
    path[k] = 0;
    j++;
    while (j < len && (l[j] == ' ' || l[j] == '\t' || l[j] == '\r')) j++;
    if (j + 1 < len && l[j] == '/' && l[j + 1] == '/') return 1;  /* comment */
    return (j == len) ? 1 : -1;
}

/* splice text into out, recursing through includes */
static int pp_expand(ppbuf *out, const char *text, const char *file,
                     const char *dir, uint32_t depth, uint32_t max_depth,
                     char *err, uint32_t errsz)
{
    uint32_t pos = 0, line = 0;

    while (text[pos] != 0) {
        uint32_t start = pos, end;
        char path[1024];
        int kind;

        while (text[pos] != 0 && text[pos] != '\n') pos++;
        end = pos;
        line++;
        kind = pp_line(text + start, end - start, path,
                       (uint32_t)sizeof(path));

        if (kind < 0) {
            snprintf(err, errsz,
                     "%s:%u: malformed include (want: include \"path\")",
                     file ? file : "<src>", line);
            return 0;
        }
        if (kind == 1) {                             /* splice the file in */
            char *full, *fdir, *ftext;

            if (depth + 1 > max_depth) {
                snprintf(err, errsz,
                         "%s:%u: include depth %u exceeded (circular include?)",
                         file ? file : "<src>", line, max_depth);
                return 0;
            }
            full = pp_join(dir, path);
            if (!full) { snprintf(err, errsz, "out of memory"); return 0; }
            ftext = pp_read_whole(full, 0);
            if (!ftext) {
                snprintf(err, errsz, "%s:%u: include \"%s\": cannot open",
                         file ? file : "<src>", line, path);
                free(full);
                return 0;
            }
            fdir = dir_of(full);
            if (!fdir) {
                snprintf(err, errsz, "out of memory");
                free(ftext); free(full);
                return 0;
            }
            if (!pp_expand(out, ftext, full, fdir, depth + 1, max_depth,
                           err, errsz)) {
                free(ftext); free(fdir); free(full);
                return 0;                            /* err already filled */
            }
            free(ftext); free(fdir); free(full);
        } else {
            if (!pp_put(out, text + start, end - start)) {
                snprintf(err, errsz, "out of memory");
                return 0;
            }
        }
        if (text[pos] == '\n') {
            if (!pp_put(out, "\n", 1)) {
                snprintf(err, errsz, "out of memory");
                return 0;
            }
            pos++;
        }
    }
    return 1;
}

char *flint_pp_includes(const char *src, const char *src_name,
                        uint32_t max_depth, char *err, uint32_t errsz)
{
    ppbuf out;
    char scratch[256];
    char *e = err ? err : scratch;
    uint32_t es = err ? errsz : (uint32_t)sizeof(scratch);
    char *dir;

    e[0] = 0;
    if (!src) return 0;
    if (max_depth == 0) max_depth = PP_DEFAULT_DEPTH;

    memset(&out, 0, sizeof(out));
    dir = src_name ? dir_of(src_name) : dupstr(".");
    if (!dir) { snprintf(e, es, "out of memory"); return 0; }

    if (!pp_expand(&out, src, src_name ? src_name : "<src>", dir, 0,
                   max_depth, e, es)) {
        free(dir);
        free(out.s);
        return 0;
    }
    free(dir);
    if (!out.s) {                                    /* empty expansion */
        out.s = (char *)malloc(1);
        if (!out.s) { snprintf(e, es, "out of memory"); return 0; }
        out.s[0] = 0;
    }
    return out.s;
}

/* ---- public API ---- */

static void flint_prog_free_tmp(asmx *a)
{
    free(a->code.b); free(a->mem.b);
    free(a->syms); free(a->fix);
}

int flint_assemble_ctx(const char *src, const char *src_name,
                       flint_include_fn inc_fn, void *inc_ud,
                       flint_prog *out,
                       uint32_t stack_extra, char *err, uint32_t err_size)
{
    asmx a;
    int i, e;
    if (!out || !src) {
        if (err && err_size) snprintf(err, err_size, "bad assemble arguments");
        return -1;
    }
    memset(&a, 0, sizeof(a));
    memset(out, 0, sizeof(*out));
    a.src = src; a.ipos = 0; a.line = 1; a.section = S_CODE;
    a.inc_fn = inc_fn; a.inc_ud = inc_ud;
    a.cur_name = dupstr(src_name ? src_name : "<src>");
    a.cur_dir  = src_name ? dir_of(src_name) : dupstr(".");
    if (!a.cur_name || !a.cur_dir) {
        if (err && err_size) snprintf(err, err_size, "out of memory");
        free(a.cur_name); free(a.cur_dir);
        return -1;
    }

    e = asm_scan(&a);
    if (e == 0) e = asm_resolve(&a);
    if (e == 0) {
        int mi = sym_find(&a, "main");
        if (mi < 0 || a.syms[mi].kind != S_CODE) {
            fail(&a, "no entry label 'main' in code section");
            e = -1;
        } else {
            uint32_t data_n = a.mem.n;
            uint32_t reuse  = (data_n + 15) & ~15u;   /* heap starts here */
            uint32_t msize  = reuse + (stack_extra ? stack_extra : 4096);
            i = (int)a.syms[mi].addr;

            out->code = (flint_byte *)calloc(a.code.n ? a.code.n : 1, 1);
            out->mem  = (flint_byte *)calloc(msize, 1);
            if (!out->code || !out->mem) {
                free(out->code); free(out->mem);
                out->code = 0; out->mem = 0;
                e = -1; fail(&a, "out of memory");
            } else {
                memcpy(out->code, a.code.b, a.code.n);
                memset(out->mem, 0, msize);
                if (data_n) memcpy(out->mem, a.mem.b, data_n);
                out->code_size  = a.code.n;
                out->mem_size   = msize;
                out->entry      = (uint32_t)i;
                out->heap_start = reuse;
            }
        }
    }

    if (err && err_size) {
        snprintf(err, err_size, "%s", a.err);
    }

    flint_prog_free_tmp(&a);
    free(a.cur_name); free(a.cur_dir);
    return e;
}

int flint_assemble(const char *src, flint_prog *out,
                   uint32_t stack_extra, char *err, uint32_t err_size)
{
    return flint_assemble_ctx(src, 0, 0, 0, out, stack_extra, err, err_size);
}

void flint_prog_free(flint_prog *p)
{
    if (!p) return;
    free(p->code);
    free(p->mem);
    p->code = 0; p->mem = 0;
    p->code_size = 0; p->mem_size = 0;
}
