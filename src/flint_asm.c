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
 * Syntax v3:
 *   mnemonics  set copy load[.byte] store[.byte] add sub mul div mod
 *              and or xor shl shr inc dec neg not jump[.eq .ne .lt .ge]
 *              call return push pop sys exit
 *   registers  r0..r7
 *   mem        [rN]  [rN+num]  [rN-num]         (store: address first)
 *   data       str "bytes", chars, nums | int num | bytes num
 *   sections   code: / data:  (code is the default)
 *   labels     name:  |  .local:   (locals attach to last global)
 *   comments   // to end of line
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

static void bb_i64(bbuf *x, flint_word v)
{
    int i;
    for (i = 0; i < 8; i++) bb_put(x, (int)(((uint64_t)v >> (8 * i)) & 0xFF));
}

static void bb_patch(bbuf *x, uint32_t at, flint_word v)
{
    int i;
    for (i = 0; i < 8; i++)
        x->b[at + i] = (flint_byte)(((uint64_t)v >> (8 * i)) & 0xFF);
}

/* ---- symbols ---- */

enum { S_CODE = 0, S_DATA };
enum { F_ANY = 0, F_CODE };

typedef struct { char name[T_MAX]; flint_word addr; int kind; } symbol;

typedef struct {
    uint32_t at;                /* byte offset of the i64 field  */
    char     name[T_MAX];       /* referenced label              */
    int      want;              /* F_CODE: label must be code    */
    int      buf;               /* 0 = code image, 1 = data image */
    uint32_t line;
} fixup;

typedef struct {
    const char *src; uint32_t ipos, line;

    bbuf     code, mem;
    symbol  *syms; uint32_t ns, cs;
    fixup   *fix;  uint32_t nf, cf;

    char     last_global[T_MAX];
    int      section;
    char     err[384];
} asmx;

/* ---- lexer ---- */

enum { T_IDENT, T_NUM, T_STR, T_CHR, T_PUNCT, T_EOL, T_EOF };

typedef struct { int type; flint_word num; char s[T_MAX]; } token;

static void fail(asmx *a, const char *fmt, ...)
{
    va_list ap;
    if (a->err[0]) return;                      /* first error wins */
    va_start(ap, fmt);
    vsnprintf(a->err, sizeof(a->err), fmt, ap);
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
    t->s[0] = 0; t->num = 0;
    t->type = T_EOL;                            /* unused len field */

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
            if (n >= T_MAX - 1) { fail(a, "line %u: string too long", a->line); return; }
            if (c == '\\' && a->src[a->ipos + 1]) {
                int e = esc(a->src[a->ipos + 1]);
                if (e < 0) { fail(a, "line %u: bad escape \\%c", a->line, a->src[a->ipos + 1]); return; }
                t->s[n++] = (char)e; a->ipos += 2;
            } else { t->s[n++] = c; a->ipos++; }
        }
        if (a->src[a->ipos] != '"') { fail(a, "line %u: unterminated string", a->line); return; }
        a->ipos++;
        t->s[n] = 0; t->type = T_STR; return;
    }
    if (a->src[a->ipos] == '\'') {
        int v;
        a->ipos++;
        if (a->src[a->ipos] == '\\') {
            v = esc(a->src[a->ipos + 1]);
            if (v < 0 || !a->src[a->ipos + 1]) { fail(a, "line %u: bad char literal", a->line); return; }
            a->ipos += 2;
        } else {
            if (!a->src[a->ipos]) { fail(a, "line %u: bad char literal", a->line); return; }
            v = (unsigned char)a->src[a->ipos]; a->ipos++;
        }
        if (a->src[a->ipos] != '\'') { fail(a, "line %u: expected closing quote", a->line); return; }
        a->ipos++;
        t->num = v; t->type = T_CHR; return;
    }
    if (is_dig(a->src[a->ipos])) {
        uint64_t v = 0;
        if (a->src[a->ipos] == '0' && a->src[a->ipos + 1] == 'x') {
            a->ipos += 2;
            for (;;) {
                int c = a->src[a->ipos], d;
                if (is_dig(c)) d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else break;
                v = v * 16 + (uint64_t)d; a->ipos++;
            }
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
        fail(a, "line %u: stray character '%c'", a->line, a->src[a->ipos]);
        a->ipos++; t->type = T_EOF; return;
    }
}

/* lookahead machinery */

static token ahead_tok; static int have_ahead;

static void pk(asmx *a, token *t)
{
    if (!have_ahead) {
        next_tok(a, &ahead_tok);
        have_ahead = 1;
    }
    *t = ahead_tok;
}

static void nx(asmx *a, token *t)
{
    if (have_ahead) { *t = ahead_tok; have_ahead = 0; }
    else next_tok(a, t);
}

/* ---- symbols / fixups ---- */

static int sym_add(asmx *a, const char *name, flint_word addr, int kind)
{
    uint32_t i;
    for (i = 0; i < a->ns; i++)
        if (strcmp(a->syms[i].name, name) == 0) {
            fail(a, "line %u: duplicate label \"%s\"", a->line, name);
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
    uint8_t    reg;            /* O_REG / O_MEM base */
    flint_word num;            /* O_NUM / O_MEM offset */
    char       name[T_MAX];    /* O_SYM */
} opr;

static int reg_no(const token *t)
{
    if (t->type == T_IDENT && t->s[0] == 'r' &&
        t->s[1] >= '0' && t->s[1] <= '7' && t->s[2] == 0)
        return t->s[1] - '0';
    return -1;
}

static int parse_reg(asmx *a, int *r, token *t)
{
    pk(a, t);
    if (reg_no(t) < 0) { fail(a, "line %u: expected register r0..r7", a->line); return 0; }
    nx(a, t);
    *r = t->s[1] - '0';
    return 1;
}

static int parse_imm(asmx *a, opr *o, token *t)
{
    pk(a, t);
    if (t->type == T_PUNCT && t->s[0] == '-') {
        nx(a, t);
        pk(a, t);
        if (t->type != T_NUM) { fail(a, "line %u: expected number after '-'", a->line); return 0; }
        nx(a, t);
        o->kind = O_NUM; o->num = -t->num; return 1;
    }
    if (t->type == T_NUM || t->type == T_CHR) {
        nx(a, t);
        o->kind = O_NUM; o->num = t->num; return 1;
    }
    if (t->type == T_IDENT && reg_no(t) < 0) {
        char full[T_MAX];
        nx(a, t);
        full_local(a, t->s, full, sizeof(full));
        o->kind = O_SYM;
        snprintf(o->name, sizeof(o->name), "%s", full);
        return 1;
    }
    fail(a, "line %u: expected immediate or label", a->line);
    return 0;
}

static int parse_mem(asmx *a, opr *o, token *t)
{
    int rb;
    pk(a, t);
    if (t->type != T_PUNCT || t->s[0] != '[') { fail(a, "line %u: expected '['", a->line); return 0; }
    nx(a, t);
    if (!parse_reg(a, &rb, t)) return 0;
    o->reg = (uint8_t)rb;
    o->kind = O_MEM; o->num = 0;
    pk(a, t);
    if (t->type == T_PUNCT && (t->s[0] == '+' || t->s[0] == '-')) {
        int neg = (t->s[0] == '-');
        nx(a, t);
        pk(a, t);
        if (t->type != T_NUM) { fail(a, "line %u: expected offset number", a->line); return 0; }
        nx(a, t);
        o->num = neg ? -t->num : t->num;
    }
    pk(a, t);
    if (t->type != T_PUNCT || t->s[0] != ']') { fail(a, "line %u: expected ']'", a->line); return 0; }
    nx(a, t);
    return 1;
}

/* ---- emit helpers (code section) ---- */

static void e_cst(asmx *a, int v)   { if (!bb_put(&a->code, v)) fail(a, "out of memory"); }
static void e_ci64(asmx *a, flint_word v) { bb_i64(&a->code, v); }
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
    N_SET, N_COPY, N_LOAD, N_LOADB, N_STORE, N_STOREB,
    N_ARITH, N_INC, N_DEC, N_NEG, N_NOT,
    N_JUMP, N_BRANCH, N_CALL, N_RET,
    N_PUSH, N_POP, N_SYS, N_EXIT,
    D_STR, D_INT, D_BYTES
};

typedef struct { const char *nm; int cls; int opA, opB; } mnem_t;

static const mnem_t g_tab[] = {
    {"set",        N_SET,   0, 0},
    {"copy",       N_COPY,  0, 0},
    {"load",       N_LOAD,  0, 0},
    {"load.byte",  N_LOADB, 0, 0},
    {"store",      N_STORE, 0, 0},
    {"store.byte", N_STOREB, 0, 0},
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
    {"push",       N_PUSH,  0, 0},
    {"pop",        N_POP,   0, 0},
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
        fail(a, "line %u: expected ','", a->line); return 0;
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

static void m_load(asmx *a, int byte)
{
    int rd, rb; opr mo; token t;
    if (!parse_reg(a, &rd, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_mem(a, &mo, &t)) return;
    rb = mo.reg;
    e_cst(a, byte ? OP_LOADB : OP_LOADW);
    e_regb(a, rd); e_regb(a, rb); e_imm(a, mo.num);
}

static void m_store(asmx *a, int byte)
{
    int rs; opr mo; token t;
    if (!parse_mem(a, &mo, &t)) return;
    if (!need_comma(a, &t)) return;
    if (!parse_reg(a, &rs, &t)) return;
    e_cst(a, byte ? OP_STOREB : OP_STOREW);
    e_regb(a, mo.reg); e_regb(a, rs); e_imm(a, mo.num);
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
            fail(a, "line %u: expected third operand", a->line);
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
        fail(a, "line %u: expected label after jump", a->line);
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
        if (!need_comma(a, &t)) { fail(a, "line %u: expected branch target", a->line); return; }
        pk(a, &t);
        if (t.type == T_EOL || t.type == T_EOF) {
            fail(a, "line %u: expected branch target", a->line); return;
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
        fail(a, "line %u: expected branch target", a->line); return;
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
        fail(a, "line %u: expected label after call", a->line);
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

static void m_sys(asmx *a)
{
    token t;
    pk(a, &t);
    if (t.type != T_NUM) { fail(a, "line %u: expected syscall number", a->line); return; }
    nx(a, &t);
    if (t.num < 0 || t.num > 255) { fail(a, "line %u: syscall number out of range", a->line); return; }
    e_cst(a, OP_INT); e_cst(a, (int)t.num);
}

/* exit: OP_HLT */
static void m_exit(asmx *a) { e_cst(a, OP_HLT); }
static void m_ret(asmx *a)  { e_cst(a, OP_RET); }

/* ---- data directives (mem section) ---- */

static void d_str(asmx *a)
{
    token t;
    pk(a, &t);
    if (t.type == T_STR) {
        int i;
        nx(a, &t);
        for (i = 0; t.s[i]; i++) { if (!bb_put(&a->mem, (int)(unsigned char)t.s[i])) fail(a, "out of memory"); }
    } else if (t.type == T_NUM || t.type == T_CHR) {
        opr o;
        nx(a, &t);
        o.kind = O_NUM; o.num = t.num;
        if (o.num < 0 || o.num > 255) { fail(a, "line %u: str byte out of range", a->line); return; }
        if (!bb_put(&a->mem, (int)o.num)) fail(a, "out of memory");
    } else {
        fail(a, "line %u: expected string literal", a->line);
        return;
    }
    for (;;) {                                   /* ", byte" tail */
        pk(a, &t);
        if (t.type == T_PUNCT && t.s[0] == ',') {
            nx(a, &t);
            pk(a, &t);
            if (t.type != T_NUM && t.type != T_CHR) { fail(a, "line %u: expected byte value", a->line); return; }
            if (t.num < 0 || t.num > 255) { fail(a, "line %u: str byte out of range", a->line); return; }
            nx(a, &t);
            if (!bb_put(&a->mem, (int)t.num)) fail(a, "out of memory");
            continue;
        }
        break;
    }
}

static void d_int(asmx *a)
{
    token t;
    pk(a, &t);
    if (t.type != T_NUM) { fail(a, "line %u: expected number", a->line); return; }
    nx(a, &t);
    bb_i64(&a->mem, t.num);
}

static void d_bytes(asmx *a)
{
    token t;
    uint32_t n;
    pk(a, &t);
    if (t.type != T_NUM) { fail(a, "line %u: expected size", a->line); return; }
    nx(a, &t);
    for (n = 0; n < (uint32_t)t.num; n++) { if (!bb_put(&a->mem, 0)) fail(a, "out of memory"); }
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

static int asm_statement(asmx *a, const mnem_t *m, const char *nm)
{
    token t;
    (void)nm;

    switch (m->cls) {
    case N_SET:    m_set(a);      break;
    case N_COPY:   m_copy(a);     break;
    case N_LOAD:   m_load(a, 0);  break;
    case N_LOADB:  m_load(a, 1);  break;
    case N_STORE:  m_store(a, 0); break;
    case N_STOREB: m_store(a, 1); break;
    case N_ARITH:  m_arith(a, m->opA, m->opB); break;
    case N_INC:    m_incdec(a, OP_INC); break;
    case N_DEC:    m_incdec(a, OP_DEC); break;
    case N_NEG:    m_unary2(a, OP_NEG); break;
    case N_NOT:    m_unary2(a, OP_NOT); break;
    case N_JUMP:   m_jump(a);     break;
    case N_BRANCH: m_branch(a, m->opA, m->opB); break;
    case N_CALL:   m_call(a);     break;
    case N_RET:    m_ret(a);      break;
    case N_PUSH:   m_push(a);     break;
    case N_POP:    m_pop(a);      break;
    case N_SYS:    m_sys(a);      break;
    case N_EXIT:   m_exit(a);     break;
    case D_STR:    d_str(a);      break;
    case D_INT:    d_int(a);      break;
    case D_BYTES:  d_bytes(a);    break;
    default:       pk(a, &t); fail(a, "line %u: internal dispatch miss", a->line); break;
    }
    return 0;
}

static int asm_scan(asmx *a)
{
    token t, t2;

    for (;;) {
        pk(a, &t);
        if (t.type == T_EOF || a->err[0]) break;

        if (t.type == T_EOL) { nx(a, &t); continue; }

        if (t.type != T_IDENT) {
            fail(a, "line %u: expected mnemonic, label or section", a->line);
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

        {
            const mnem_t *m = find_mnem(t.s);
            if (!m) { fail(a, "line %u: unknown mnemonic \"%s\"", a->line, t.s); break; }
            if (a->section == S_CODE && (m->cls == D_STR || m->cls == D_INT || m->cls == D_BYTES)) {
                fail(a, "line %u: data directive \"%s\" in code section", a->line, t.s);
                break;
            }
            if (a->section == S_DATA && m->cls != D_STR && m->cls != D_INT && m->cls != D_BYTES) {
                fail(a, "line %u: instruction \"%s\" in data section", a->line, t.s);
                break;
            }
            asm_statement(a, m, t.s);
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
            fail(a, "line %u: undefined label \"%s\"", a->fix[i].line, a->fix[i].name);
            return -1;
        }
        if (a->fix[i].want == F_CODE && a->syms[s].kind != S_CODE) {
            fail(a, "line %u: \"%s\" is not a code label", a->fix[i].line, a->fix[i].name);
            return -1;
        }
        if (a->fix[i].buf == 0) bb_patch(&a->code, a->fix[i].at, a->syms[s].addr);
        else                    bb_patch(&a->mem,  a->fix[i].at, a->syms[s].addr);
    }
    return 0;
}

/* ---- public API ---- */

static void flint_prog_free_tmp(asmx *a)
{
    free(a->code.b); free(a->mem.b);
    free(a->syms); free(a->fix);
}

int flint_assemble(const char *src, flint_prog *out,
                   uint32_t stack_extra, char *err, uint32_t err_size)
{
    asmx a;
    int i, e;
    memset(&a, 0, sizeof(a));
    have_ahead = 0;
    a.src = src; a.ipos = 0; a.line = 1; a.section = S_CODE;

    e = asm_scan(&a);
    if (e == 0) e = asm_resolve(&a);
    if (e == 0) {
        int mi = sym_find(&a, "main");
        if (mi < 0 || a.syms[mi].kind != S_CODE) {
            fail(&a, "no entry label 'main' in code section");
            e = -1;
        } else {
            uint32_t data_n = a.mem.n;
            uint32_t reuse  = (data_n + 15) & ~15u;
            uint32_t msize  = reuse + (stack_extra ? stack_extra : 4096);
            i = (int)a.syms[mi].addr;

            out->code = (flint_byte *)calloc(a.code.n ? a.code.n : 1, 1);
            out->mem  = (flint_byte *)calloc(msize, 1);
            if (!out->code || !out->mem) { e = -1; fail(&a, "out of memory"); }
            else {
                memcpy(out->code, a.code.b, a.code.n);
                memset(out->mem, 0, msize);
                if (data_n) memcpy(out->mem, a.mem.b, data_n);
                out->code_size = a.code.n;
                out->mem_size  = msize;
                out->entry     = (uint32_t)i;
            }
        }
    }

    if (err && err_size) {
        snprintf(err, err_size, "%s", a.err);
    }

    flint_prog_free_tmp(&a);
    return e;
}

void flint_prog_free(flint_prog *p)
{
    if (!p) return;
    free(p->code);
    free(p->mem);
    p->code = 0; p->mem = 0;
    p->code_size = 0; p->mem_size = 0;
}
