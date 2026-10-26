/*
 * test_asm.c — full pipeline bench: v3 text -> assemble -> VM run.
 * Programs are the design examples; error sources must fail with a
 * diagnostic pointing at the problem line.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "flint.h"

static int fails = 0;
#define CHECK(cond, msg) \
    do { if (cond) printf("PASS  %s\n", (msg)); \
         else     { printf("FAIL  %s\n", (msg)); fails++; } } while (0)

/* host syscalls for tests: 0 = prn (append decimal), 1 = putc */
static char out[512]; static uint32_t outn;

static void s_putc(flint_vm *vm)
{ if (outn < sizeof(out) - 1) out[outn++] = (char)vm->r[0]; }

static void s_prn(flint_vm *vm)
{ outn += (uint32_t)snprintf(out + outn, sizeof(out) - 1 - outn, "%lld",
                             (long long)vm->r[0]); }

static void out_reset(void) { outn = 0; out[0] = 0; }

static flint_vm last;
static int g_rc;

/* assemble + init + run; returns 1 on success, checks the vm */
static int run_p(const char *src, const char *what)
{
    flint_prog p; char err[256] = "";
    if (flint_assemble(src, &p, 4096, err, sizeof(err)) != 0) {
        printf("FAIL  %s: assemble: %s\n", what, err[0] ? err : "?");
        fails++;
        return 0;
    }
    out_reset();
    flint_init(&last, p.code, p.code_size, p.mem, p.mem_size);
    flint_sys_set(&last, 0, s_prn);
    flint_sys_set(&last, 1, s_putc);
    g_rc = (int)flint_run(&last, p.entry);
    flint_prog_free(&p);
    return 1;
}

/* out must equal want */
static void chk_out(const char *want, const char *what)
{
    if (last.err == FLINT_OK && (uint32_t)strlen(want) == outn &&
        memcmp(out, want, outn) == 0)
        printf("PASS  %s  [%s]\n", what, out);
    else {
        printf("FAIL  %s  err=%d%s out=[%.*s]\n", what, last.err,
               last.err != FLINT_OK ? last.err_text : "",
               (int)(outn > 60 ? 60 : outn), out);
        fails++;
    }
}

/* assembler must reject src */
static void chk_reject(const char *src, const char *what)
{
    flint_prog p; char err[256] = "";
    if (flint_assemble(src, &p, 4096, err, sizeof(err)) == 0) {
        printf("FAIL  %s: accepted, expected error\n", what);
        fails++;
        flint_prog_free(&p);
        return;
    }
    printf("PASS  %s  (%s)\n", what, err);
}

/* ---- include machinery: fake resolver over an in-memory table ---- */

typedef struct { const char *name; const char *text; } inc_entry;


static const inc_entry g_inc[] = {
    { "part.flt",  "code:\ninc_helper:\n    set r1, 42\n    return\n" },
    { "data.flt",  "data:\ninc_msg: str \"hi\", 0\ncode:\n" },
    { "n1.flt",     "include \"n2.flt\"\ncode:\ninc_who:\n    copy r4, r5\n    return\n" },
    { "n2.flt",     "code:\ninc_nested_helper:\n    set r1, 7\n    return\n" },
    { "macros.flt", "%macro DOUBLE 1\n    add %1, %1, %1\n%endmacro\n" },
    { "cyc_a.flt",  "include \"cyc_b.flt\"\ncode:\ninc_a:\n    return\n" },
    { "cyc_b.flt",  "include \"cyc_a.flt\"\ncode:\ninc_b:\n    return\n" },
    { 0, 0 }
};

static char g_seen[8][64]; static int g_nseen;

static void inc_reset(void) { g_nseen = 0; }

static char *inc_fake(void *ud, const char *name, const char *from_dir,
                      const char *from_name, char *err, uint32_t errsz)
{
    const inc_entry *e = g_inc;
    size_t nl = strlen(name);
    for (; e->name; e++)
        if (strlen(e->name) == nl && strcmp(e->name, name) == 0) break;
    if (!e->name) {
        snprintf(err, errsz, "not found (from %s)", from_dir);
        return 0;
    }
    for (int i = 0; i < g_nseen; i++)
        if (strcmp(g_seen[i], name) == 0) { char *z = malloc(1); *z = 0; return z; }
    if (g_nseen < 8) snprintf(g_seen[g_nseen++], 64, "%s", name);
    (void)ud; (void)from_name;
    size_t n = strlen(e->text) + 1;
    char *d = malloc(n); memcpy(d, e->text, n);
    return d;
}

/* run_p_ctx: like run_p but through flint_assemble_ctx with the fake resolver */
static int run_ctx(const char *src, const char *name, flint_include_fn fn,
                   void *ud, const char *what)
{
    flint_prog p; char err[256] = "";
    if (flint_assemble_ctx(src, name, fn, ud, &p, 4096, err, sizeof(err)) != 0) {
        printf("FAIL  %s: assemble: %s\n", what, err[0] ? err : "?");
        fails++;
        return 0;
    }
    out_reset();
    flint_init(&last, p.code, p.code_size, p.mem, p.mem_size);
    flint_sys_set(&last, 0, s_prn);
    flint_sys_set(&last, 1, s_putc);
    g_rc = (int)flint_run(&last, p.entry);
    flint_prog_free(&p);
    return 1;
}

/* --- programs --- */

static const char *S_hello =
"data:\n"
"hello: str \"Hello, FLINT!\", 10, 0\n"
"code:\n"
"main:\n"
"    // print the string\n"
"    set r2, hello\n"
"loop:\n"
"    load.byte r1, [r2]\n"
"    jump.eq r1, 0, done      // \\0 ?\n"
"    copy r0, r1\n"
"    sys 1\n"
"    inc r2\n"
"    jump loop\n"
"done:\n"
"    exit\n";

static const char *S_sum =
"code:\n"
"main:\n"
"    set r0, 0\n"
"    set r1, 1\n"
"loop:\n"
"    jump.ge r1, 11, done\n"
"    add r0, r0, r1\n"
"    inc r1\n"
"    jump loop\n"
"done:\n"
"    sys 0\n"
"    exit\n";

static const char *S_fact =
"code:\n"
"main:\n"
"    set r0, 6\n"
"    call fact\n"
"    copy r0, r1\n"
"    sys 0\n"
"    exit\n"
"\n"
"fact:\n"
"    push r0\n"
"    jump.ge r0, 2, rec\n"
"    pop r0\n"
"    set r1, 1\n"
"    return\n"
"rec:\n"
"    dec r0\n"
"    call fact\n"
"    inc r0\n"
"    mul r1, r1, r0\n"
"    pop r0\n"
"    return\n";

static const char *S_off =
"data:\n"
"buf: bytes 16\n"
"code:\n"
"main:\n"
"    set r3, buf\n"
"    set r0, 77\n"
"    store [r3+4], r0\n"
"    set r0, 0\n"
"    load r2, [r3+4]\n"
"    copy r0, r2\n"
"    sys 0\n"
"    exit\n";

static const char *S_local =
"code:\n"
"main:\n"
"    set r1, 10\n"
"next:\n"
"    dec r1\n"
"    jump.ne r1, 0, next\n"
"    copy r0, r1\n"
"    sys 0\n"
"    exit\n";

static const char *S_negimm =
"code:\n"
"main:\n"
"    set r0, -5\n"
"    add r0, r0, 15\n"
"    sys 0\n"
"    exit\n";

static const char *S_sugar =
"code:\n"
"main:\n"
"    set r1, 0\n"
"    jump.eq r1, done          // two-operand sugar\n"
"    set r0, 42\n"
"done:\n"
"    sys 0\n"
"    exit\n";

static const char *S_unsig =
"code:\n"
"main:\n"
"    set r1, -1\n"
"    set r0, 42\n"
"    jump.lo r1, 1, wrong      // unsigned 0xFFFF..FF < 1: no\n"
"    jump.hs r1, 1, ok         // unsigned >= 1: yes\n"
"wrong:\n"
"    set r0, 99\n"
"    sys 0\n"
"    exit\n"
"ok:\n"
"    sys 0\n"
"    set r0, 0\n"
"    exit\n";

static const char *S_pushi =
"code:\n"
"main:\n"
"    push 42\n"
"    push r0\n"
"    pop r2\n"
"    pop r1\n"
"    add r0, r1, r2\n"
"    sys 0\n"
"    exit\n";

static const char *S_dataref =
"data:\n"
"answer: int 42\n"
"code:\n"
"main:\n"
"    set r0, answer\n"
"    load r1, [r0]\n"
"    copy r0, r1\n"
"    sys 0\n"
"    exit\n";

static const char *S_bad_mnem =
"code:\n"
"main:\n"
"    frobnicate r1, r2\n"
"    exit\n";

static const char *S_bad_label =
"code:\n"
"main:\n"
"    jump nowhere\n"
"    exit\n";

static const char *S_bad_jdata =
"data:\n"
"konst: int 1\n"
"code:\n"
"main:\n"
"    jump konst\n"
"    exit\n";

static const char *S_bad_nomain =
"code:\n"
"start:\n"
"    exit\n";

static const char *S_bad_dup =
"code:\n"
"main:\n"
"    exit\n"
"main:\n"
"    exit\n";

static const char *S_bad_dirc =
"code:\n"
"main:\n"
"    str \"oops\"\n"
"    exit\n";

static const char *S_bad_reg =
"code:\n"
"main:\n"
"    copy r16, r0\n"
"    exit\n";

static const char *S_bad_setreg =
"code:\n"
"main:\n"
"    set r3, r1\n"
"    exit\n";

/* v0.2.0: frame pointer locals + indexed array access + expanded regs */
static const char *S_frames =
"code:\n"
"main:\n"
"    set r0, 5\n"
"    set r1, 7\n"
"    call add2\n"
"    copy r1, r0\n"
"    sys 0\n"
"    exit\n"
"add2:\n"
"    enter 16\n"
"    store [fp-8], r0\n"
"    store [fp-16], r1\n"
"    load r0, [fp-8]\n"
"    load r2, [fp-16]\n"
"    add r0, r0, r2\n"
"    leave\n"
"    return\n";

static const char *S_index =
"data:\n"
"arr: int 10, 20, 30, 40\n"
"code:\n"
"main:\n"
"    set r1, arr\n"
"    set r2, 0\n"
"    set r0, 0\n"
"    set r3, 32\n"
"loop:\n"
"    jump.ge r2, r3, done\n"
"    loadx r4, [r1+r2]\n"
"    add r0, r0, r4\n"
"    add r2, 8\n"
"    jump loop\n"
"done:\n"
"    copy r1, r0\n"
"    sys 0\n"
"    exit\n";

static const char *S_index_store =
"data:\n"
"buf: bytes 32\n"
"code:\n"
"main:\n"
"    set r1, buf\n"
"    set r2, 16\n"
"    set r3, 77\n"
"    storx [r1+r2], r3\n"
"    set r2, 16\n"
"    load r4, [r1+r2]\n"
"    copy r0, r4\n"
"    sys 0\n"
"    exit\n";

static const char *S_r15 =
"code:\n"
"main:\n"
"    set r15, 909\n"
"    copy r0, r15\n"
"    sys 0\n"
"    exit\n";

/* v0.4.0: indirect jumps/calls and equ constants */

static const char *S_jmpr =
"code:\n"
"main:\n"
"    set r1, second\n"
"    jump r1                // indirect through the register\n"
"first:\n"
"    set r0, 111            // must not run\n"
"    sys 0\n"
"    exit\n"
"second:\n"
"    set r0, 42\n"
"    sys 0\n"
"    exit\n";

static const char *S_callr =
"code:\n"
"main:\n"
"    set r1, answer\n"
"    call r1                // indirect call; ret returns here\n"
"    sys 0\n"
"    exit\n"
"answer:\n"
"    set r0, 42\n"
"    return\n";

/* equ: constants substituted into immediates and branch compares */
static const char *S_equ =
"BASE  equ 10\n"
"STEP  equ 2\n"
"LIMIT equ 3\n"
"code:\n"
"main:\n"
"    set r1, 0\n"
"    set r0, 0\n"
"loop:\n"
"    jump.ge r1, LIMIT, done\n"
"    add r0, BASE\n"
"    add r0, STEP\n"
"    inc r1\n"
"    jump loop\n"
"done:\n"
"    sys 0\n"
"    exit\n";

static const char *S_equ_neg =
"NEG equ -5\n"
"code:\n"
"main:\n"
"    set r0, NEG\n"
"    add r0, 15\n"
"    sys 0\n"
"    exit\n";

static const char *S_equ_alias =
"CHAR  equ 'A'\n"
"ALIAS equ CHAR\n"
"code:\n"
"main:\n"
"    set r0, ALIAS\n"
"    sub r0, 23\n"
"    sys 0\n"
"    exit\n";

static const char *S_bad_equ_dup =
"code:\n"
"DUP equ 1\n"
"DUP equ 2\n"
"main:\n"
"    exit\n";

static const char *S_bad_equ_undef =
"code:\n"
"X equ NOPE\n"
"main:\n"
"    exit\n";

static const char *S_bad_equ_missing =
"code:\n"
"X equ\n"
"main:\n"
"    exit\n";

static const char *S_bad_equ_list =
"code:\n"
"X equ 1, 2\n"
"main:\n"
"    exit\n";

/* v0.4.0-beta.1: export — the flint_prog symbol table (flint build --so) */

static const char *S_export =
"code:\n"
"export helper\n"
"main:\n"
"    set r0, 1\n"
"    exit\n"
"helper:\n"
"    set r0, 77\n"
"    return\n";

static const char *S_bad_export_dup =
"code:\n"
"export a\n"
"export a\n"
"a:\n"
"    return\n"
"main:\n"
"    exit\n";

static const char *S_bad_export_data =
"data:\n"
"export buf\n"
"buf: bytes 8\n"
"code:\n"
"main:\n"
"    exit\n";

static const char *S_bad_export_undef =
"code:\n"
"export nope\n"
"main:\n"
"    exit\n";

static const char *S_bad_export_local =
"code:\n"
"main:\n"
"    export .loop\n"
".loop:\n"
"    exit\n";

/* v0.4.0: NASM-style text macros (%macro / %endmacro, %1..%N) */

static const char *S_macro0 =
"code:\n"
"%macro SAVE_REGS 0\n"
"    push r1\n"
"    push r2\n"
"%endmacro\n"
"%macro RESTORE_REGS 0\n"
"    pop r2\n"
"    pop r1\n"
"%endmacro\n"
"main:\n"
"    set r1, 30\n"
"    set r2, 12\n"
"    SAVE_REGS\n"
"    set r1, 0\n"
"    set r2, 0\n"
"    RESTORE_REGS\n"
"    add r0, r1, r2\n"
"    sys 0\n"
"    exit\n";

static const char *S_macro_args =
"code:\n"
"%macro LOADV 2\n"
"    load %1, [%2]\n"
"%endmacro\n"
"data:\n"
"v: int 42\n"
"code:\n"
"main:\n"
"    set r1, v\n"
"    LOADV r0, r1\n"
"    sys 0\n"
"    exit\n";

static const char *S_macro_nest =
"code:\n"
"%macro PUSH2 0\n"
"    push r1\n"
"    push r2\n"
"%endmacro\n"
"%macro SAVE2 0\n"
"    PUSH2\n"
"%endmacro\n"
"main:\n"
"    set r1, 40\n"
"    set r2, 2\n"
"    SAVE2\n"
"    set r1, 0\n"
"    set r2, 0\n"
"    pop r2\n"
"    pop r1\n"
"    add r0, r1, r2\n"
"    sys 0\n"
"    exit\n";

static const char *S_bad_endmacro =
"code:\n"
"main:\n"
"    %endmacro\n"
"    exit\n";

static const char *S_bad_macro_open =
"code:\n"
"%macro FOO 0\n"
"    push r1\n"
"main:\n"
"    exit\n";

static const char *S_bad_macro_ref =
"code:\n"
"%macro TWO 2\n"
"    add %1, %2, %3\n"
"%endmacro\n"
"main:\n"
"    exit\n";

static const char *S_bad_macro_count =
"code:\n"
"%macro ONE 1\n"
"    push %1\n"
"%endmacro\n"
"main:\n"
"    ONE\n"
"    exit\n";

static const char *S_bad_marg =
"code:\n"
"main:\n"
"    push %1\n"
"    exit\n";

static const char *S_bad_macro_body =
"code:\n"
"%macro BAD 0\n"
"    frobnicate r1\n"
"%endmacro\n"
"main:\n"
"    BAD\n"
"    exit\n";

/* --- bench --- */

static void t_hello(void)
{
    flint_prog p; char err[256] = "";
    if (flint_assemble(S_hello, &p, 4096, err, sizeof(err)) != 0) {
        printf("FAIL  hello assemble: %s\n", err); fails++;
        return;
    }
    out_reset();
    flint_init(&last, p.code, p.code_size, p.mem, p.mem_size);
    flint_sys_set(&last, 1, s_putc);
    flint_run(&last, p.entry);
    if (last.err == FLINT_OK && outn == 14 &&
        memcmp(out, "Hello, FLINT!\n", 14) == 0)
        printf("PASS  hello via assembler\n");
    else {
        printf("FAIL  hello via assembler err=%d out=[%.*s]\n", last.err,
               (int)(outn > 40 ? 40 : outn), out);
        fails++;
    }
    flint_prog_free(&p);
}

/* ---- textual preprocessor (flint_pp_includes): real files in build/ ---- */

static void t_pp(void)
{
    FILE *f;
    char *out; char err[256] = "";

    /* nested expansion + .include spelling, relative to the includer */
    f = fopen("build/pp_b.flt", "wb");
    if (f) { fputs("set r0, 7\n", f); fclose(f); }
    f = fopen("build/pp_a.flt", "wb");
    if (f) { fputs("include \"pp_b.flt\"\nset r1, 9\n", f); fclose(f); }

    out = flint_pp_includes(".include \"build/pp_a.flt\"\n", "main.flt", 0,
                            err, sizeof err);
    if (out && strstr(out, "set r0, 7") && strstr(out, "set r1, 9") &&
        strstr(out, "include") == 0)
        printf("PASS  pp: nested textual include, directives gone\n");
    else { printf("FAIL  pp: nested  [%s] err=%s\n",
                  out ? out : "(null)", err); fails++; }
    free(out);

    /* cycle: ppc_a <-> ppc_b must die on the depth cap */
    f = fopen("build/ppc_b.flt", "wb");
    if (f) { fputs(".include \"ppc_a.flt\"\nset r2, 1\n", f); fclose(f); }
    f = fopen("build/ppc_a.flt", "wb");
    if (f) { fputs(".include \"ppc_b.flt\"\nset r3, 1\n", f); fclose(f); }
    err[0] = 0;
    out = flint_pp_includes(".include \"build/ppc_a.flt\"\n", "main.flt", 0,
                            err, sizeof err);
    if (!out && strstr(err, "depth"))
        printf("PASS  pp: cycle caught by depth cap  (%s)\n", err);
    else { printf("FAIL  pp: cycle  [%s]\n", out ? out : "(null)"); fails++; }
    free(out);

    remove("build/pp_a.flt");  remove("build/pp_b.flt");
    remove("build/ppc_a.flt"); remove("build/ppc_b.flt");
}

int main(void)
{
    printf("flint core %s + asm\n", FLINT_VERSION);
    t_hello();

    if (run_p(S_sum,    "sum"))     chk_out("55",   "sum 1..10");
    if (run_p(S_fact,   "fact"))    chk_out("720",  "fact(6) via assembler");
    if (run_p(S_off,    "off"))     chk_out("77",   "store/load with offset");
    if (run_p(S_local,  "local"))   chk_out("0",    "global + local labels");
    if (run_p(S_negimm, "negimm"))  chk_out("10",   "-5 + 15 immediate");
    if (run_p(S_sugar,  "sugar"))   chk_out("0",    "branch two-operand sugar");
    if (run_p(S_pushi,  "pushi"))   chk_out("42",   "push imm / pop / add");
    if (run_p(S_dataref,"dataref")) chk_out("42",   "set label + load word");
    if (run_p(S_frames, "frames"))  chk_out("12",   "enter/leave + fp locals");
    if (run_p(S_index,  "index"))   chk_out("100",  "loadx array sum");
    if (run_p(S_index_store, "index_store"))
        chk_out("77", "storx + auto-indexed load");
    if (run_p(S_r15,    "r15"))     chk_out("909",  "r15 usable");
    if (run_p(S_jmpr,   "jmpr"))    chk_out("42",   "jump rs: indirect jump");
    if (run_p(S_callr,  "callr"))   chk_out("42",   "call rs: indirect call + return");
    if (run_p(S_equ,    "equ"))     chk_out("36",   "equ: arith + branch compares");
    if (run_p(S_equ_neg,"equneg"))  chk_out("10",   "equ: negative value");
    if (run_p(S_equ_alias, "equalias"))
        chk_out("42", "equ: char literal + alias");
    if (run_p(S_macro0, "macro0"))
        chk_out("42", "macro: zero-arg SAVE_REGS/RESTORE_REGS");
    if (run_p(S_macro_args, "macroargs"))
        chk_out("42", "macro: %1/%2 substitution with bracketed arg");
    if (run_p(S_macro_nest, "macronest"))
        chk_out("42", "macro: nested expansion");
    if (run_p(S_unsig,  "unsig")) {
        chk_out("42", "unsigned pair lo/hs");
        /* lo must not fire: the "99" marker must never reach stdout */
        if (outn == 2 && out[0] == '4' && out[1] == '2')
            printf("PASS  wrong-branch path silent\n");
        else { printf("FAIL  wrong-branch path leaked out=[%.*s]\n",
                      (int)(outn > 60 ? 60 : outn), out); fails++; }
    }

    /* include machinery */
    inc_reset();
    if (run_ctx("include \"part.flt\"\ncode:\nmain:\n    call inc_helper\n"
                "    copy r0, r1\n"
                "    exit\n", "main.flt", inc_fake, 0, "inc-part")) {
        if (last.err == FLINT_OK && outn == 0 && g_rc == 42)
            printf("PASS  include: code via resolver  [rc=42]\n");
        else { printf("FAIL  include: code via resolver  rc=%d\n", g_rc); fails++; }
    }
    inc_reset();
    if (run_ctx(".include \"part.flt\"\ncode:\nmain:\n    call inc_helper\n"
                "    copy r0, r1\n    exit\n", "main.flt", inc_fake, 0,
                ".include-alias")) {
        if (last.err == FLINT_OK && outn == 0 && g_rc == 42)
            printf("PASS  .include alias via resolver  [rc=42]\n");
        else { printf("FAIL  .include alias via resolver  rc=%d\n", g_rc); fails++; }
    }
    inc_reset();
    if (run_ctx("include \"data.flt\"\n"
                "code:\n"
                "main:\n"
                "    set r1, inc_msg\n"
                "    load.byte r4, [r1+0]\n"
                "    copy r0, r4\n"
                "    sys 1\n"
                "    inc r1\n"
                "    load.byte r4, [r1+0]\n"
                "    copy r0, r4\n"
                "    sys 1\n"
                "    set r0, 0\n"
                "    exit\n", "main.flt", inc_fake, 0,
                "inc-data"))
        chk_out("hi", "include: data labels global");
    inc_reset();
    if (run_ctx("include \"n1.flt\"\ncode:\nmain:\n    call inc_nested_helper\n"
                "    copy r0, r1\n    exit\n", "main.flt", inc_fake, 0,
                "inc-nested")) {
        if (last.err == FLINT_OK && outn == 0 && g_rc == 7)
            printf("PASS  include: nested two levels  [rc=7]\n");
        else { printf("FAIL  include: nested two levels  rc=%d out=[%.*s]\n",
                      g_rc, (int)(outn > 40 ? 40 : outn), out); fails++; }
    }
    inc_reset();
    if (run_ctx("include \"macros.flt\"\n"
                "code:\n"
                "main:\n"
                "    set r0, 21\n"
                "    DOUBLE r0\n"
                "    sys 0\n"
                "    exit\n", "main.flt", inc_fake, 0, "inc-macro"))
        chk_out("42", "include: macro defined in an included file");
    inc_reset();
    if (run_ctx("include \"cyc_a.flt\"\ncode:\nmain:\n    call inc_a\n"
                "    call inc_b\n    set r0, 0\n    exit\n", "main.flt",
                inc_fake, 0, "inc-cycle") &&
        last.err == FLINT_OK)
        printf("PASS  include: cycle deduped by resolver\n");
    inc_reset();
    {
        flint_prog p; char err[256] = "";
        if (flint_assemble_ctx("include \"missing.flt\"\ncode:\nmain:\n    exit\n",
                               "main.flt", inc_fake, 0, &p, 4096,
                               err, sizeof(err)) == 0) {
            printf("FAIL  inc-missing: accepted, expected error\n"); fails++;
            flint_prog_free(&p);
        } else if (strstr(err, "missing.flt"))
            printf("PASS  include: missing file rejected  (%s)\n", err);
        else { printf("FAIL  inc-missing: wrong error [%s]\n", err); fails++; }
    }
    inc_reset();
    {
        flint_prog p; char err[256] = "";
        /* plain flint_assemble has no resolver: include must fail with a clear msg */
        if (flint_assemble("include \"x.flt\"\ncode:\nmain:\n    exit\n",
                           &p, 4096, err, sizeof(err)) == 0) {
            printf("FAIL  inc-noresolver: accepted, expected error\n"); fails++;
            flint_prog_free(&p);
        } else if (strstr(err, "no resolver"))
            printf("PASS  include: no resolver -> clear error  (%s)\n", err);
        else { printf("FAIL  inc-noresolver: wrong error [%s]\n", err); fails++; }
    }

    t_pp();

    /* export: flint_prog.syms + the trampoline call pattern the .so uses */
    {
        flint_prog p; char err[256] = "";
        if (flint_assemble(S_export, &p, 4096, err, sizeof(err)) != 0) {
            printf("FAIL  export: assemble: %s\n", err); fails++;
        } else {
            if (p.nsyms == 1 && strcmp(p.syms[0].name, "helper") == 0 &&
                p.syms[0].pc < p.code_size)
                printf("PASS  export: symbol table (helper @ %u)\n",
                       (unsigned)p.syms[0].pc);
            else { printf("FAIL  export: symbol table wrong\n"); fails++; }

            /* call the exported pc like the .so does: `call r15; exit` */
            {
                uint32_t n = p.code_size + 3;
                flint_byte *code = (flint_byte *)malloc(n);
                flint_vm vm;
                flint_word rc;
                if (!code) { printf("FAIL  export: out of memory\n"); fails++; }
                else {
                    memcpy(code, p.code, p.code_size);
                    code[p.code_size]     = OP_CALLR;
                    code[p.code_size + 1] = 15;
                    code[p.code_size + 2] = OP_HLT;
                    flint_init(&vm, code, n, p.mem, p.mem_size);
                    vm.r[15] = (flint_word)p.syms[0].pc;
                    rc = flint_run(&vm, p.code_size);
                    if (vm.err == FLINT_OK && rc == 77)
                        printf("PASS  export: trampoline call -> 77\n");
                    else { printf("FAIL  export: trampoline rc=%lld err=%d\n",
                                  (long long)rc, vm.err); fails++; }
                    free(code);
                }
            }
            flint_prog_free(&p);
        }
    }
    {
        flint_prog p; char err[256] = "";
        if (flint_assemble(S_bad_export_undef, &p, 4096, err, sizeof(err)) == 0) {
            printf("FAIL  export-undefined: accepted, expected error\n"); fails++;
            flint_prog_free(&p);
        } else if (strstr(err, ":2:") && strstr(err, "undefined")) {
            printf("PASS  export: undefined label rejected at its line  (%s)\n",
                   err);
        } else { printf("FAIL  export-undefined: wrong error [%s]\n", err); fails++; }
    }

    chk_reject(S_bad_mnem,  "unknown mnemonic rejected");
    chk_reject(S_bad_label, "undefined label rejected");
    chk_reject(S_bad_jdata, "jump to data label rejected");
    chk_reject(S_bad_nomain,"missing 'main' rejected");
    chk_reject(S_bad_dup,   "duplicate label rejected");
    chk_reject(S_bad_dirc,  "data directive in code rejected");
    chk_reject(S_bad_reg,   "register r16 rejected");
    chk_reject(S_bad_setreg, "set reg, reg rejected (use copy)");
    chk_reject(S_bad_endmacro,  "%endmacro without %macro rejected");
    chk_reject(S_bad_macro_open, "unterminated %macro rejected");
    chk_reject(S_bad_macro_ref,  "%N out of range rejected");
    chk_reject(S_bad_macro_count, "wrong macro argument count rejected");
    chk_reject(S_bad_marg,  "%1 outside a macro rejected");
    chk_reject(S_bad_equ_dup,  "duplicate equate rejected");
    chk_reject(S_bad_equ_undef, "undefined equate value rejected");
    chk_reject(S_bad_equ_missing, "missing equ value rejected");
    chk_reject(S_bad_equ_list, "equ comma list rejected (one value per line)");
    chk_reject(S_bad_export_dup,   "duplicate export rejected");
    chk_reject(S_bad_export_data,  "export of a data label rejected");
    chk_reject(S_bad_export_local, "export of a local label rejected");
    {
        flint_prog p; char err[256] = "";
        if (flint_assemble(S_bad_macro_body, &p, 4096, err, sizeof(err)) == 0) {
            printf("FAIL  macro-body error: accepted, expected error\n"); fails++;
            flint_prog_free(&p);
        } else if (strstr(err, ":6:"))
            printf("PASS  macro-body error points at the invocation  (%s)\n", err);
        else { printf("FAIL  macro-body error: wrong line [%s]\n", err); fails++; }
    }

    printf("fails: %d\n", fails);
    return fails ? 1 : 0;
}
