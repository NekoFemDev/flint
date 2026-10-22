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
    flint_run(&last, p.entry);
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
        printf("FAIL  %s  err=%d out=[%.*s]\n", what, last.err,
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
"    copy r9, r0\n"
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
    if (run_p(S_unsig,  "unsig")) {
        chk_out("42", "unsigned pair lo/hs");
        /* lo must not fire: the "99" marker must never reach stdout */
        if (outn == 2 && out[0] == '4' && out[1] == '2')
            printf("PASS  wrong-branch path silent\n");
        else { printf("FAIL  wrong-branch path leaked out=[%.*s]\n",
                      (int)(outn > 60 ? 60 : outn), out); fails++; }
    }

    chk_reject(S_bad_mnem,  "unknown mnemonic rejected");
    chk_reject(S_bad_label, "undefined label rejected");
    chk_reject(S_bad_jdata, "jump to data label rejected");
    chk_reject(S_bad_nomain,"missing 'main' rejected");
    chk_reject(S_bad_dup,   "duplicate label rejected");
    chk_reject(S_bad_dirc,  "data directive in code rejected");
    chk_reject(S_bad_reg,   "register r9 rejected");

    printf("fails: %d\n", fails);
    return fails ? 1 : 0;
}
