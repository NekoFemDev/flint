/*
 * test_core.c — core bench on hand-encoded bytecode.
 * The assembler doesn't exist yet: the tests use tiny emit helpers
 * that will later become the reference for flint_asm.c.
 */
#include <stdio.h>
#include <string.h>
#include "flint.h"

static int fails = 0;
#define CHECK(cond, msg) \
    do { if (cond) printf("PASS  %s\n", (msg)); \
         else     { printf("FAIL  %s\n", (msg)); fails++; } } while (0)

/* ---- bytecode emitter (proto-asm) ---- */

static flint_byte cb[4096];
static uint32_t   cn;

static void hasm_reset(void)   { cn = 0; }
static void e8(int v)          { cb[cn++] = (flint_byte)v; }
static void e64(flint_word v)  { int i; for (i = 0; i < 8; i++) cb[cn++] = (flint_byte)((uint64_t)v >> (8 * i)); }
/* target-field position: 8 bytes before emit end */
static void tf(uint32_t *at)   { *at = cn - 8; }
static void patch(uint32_t at, flint_word target)
{ int i; flint_word v = (flint_word)target;
  for (i = 0; i < 8; i++) cb[at + i] = (flint_byte)((uint64_t)v >> (8 * i)); }

static void e_set(int rd, flint_word imm) { e8(OP_SET); e8(rd); e64(imm); }
static void e_copy(int rd, int rs)        { e8(OP_COPY); e8(rd); e8(rs); }
static void e_inc(int rd)                 { e8(OP_INC); e8(rd); }
static void e_dec(int rd)                 { e8(OP_DEC); e8(rd); }
static void e_ret(void)                   { e8(OP_RET); }
static void e_hlt(void)                   { e8(OP_HLT); }
static void e_int(int slot)               { e8(OP_INT); e8(slot); }

static void e_loadb(int rd, int rb, flint_word off)  { e8(OP_LOADB); e8(rd); e8(rb); e64(off); }
static void e_loadw(int rd, int rb, flint_word off)  { e8(OP_LOADW); e8(rd); e8(rb); e64(off); }

static void e_jmp(uint32_t *at) { e8(OP_JMP); e64(0); tf(at); }
static void e_jeqi(int ra, flint_word imm, uint32_t *at) { e8(OP_JEQI); e8(ra); e64(imm); e64(0); tf(at); }
static void e_jgei(int ra, flint_word imm, uint32_t *at) { e8(OP_JGEI); e8(ra); e64(imm); e64(0); tf(at); }

static void e_call(uint32_t *at) { e8(OP_CALL); e64(0); tf(at); }
static void e_push(int rs)       { e8(OP_PUSH); e8(rs); }
static void e_pop(int rd)        { e8(OP_POP); e8(rd); }
static void e_mul(int rd, int ra, int rb) { e8(OP_MUL); e8(rd); e8(ra); e8(rb); }
static void e_add(int rd, int ra, int rb) { e8(OP_ADD); e8(rd); e8(ra); e8(rb); }
static void e_enter(flint_word n)        { e8(OP_ENTER); e64(n); }
static void e_leave(void)                { e8(OP_LEAVE); }
static void e_loadx(int rd, int rb, int ri) { e8(OP_LOADX); e8(rd); e8(rb); e8(ri); }
static void e_storx(int rb, int ri, int rs) { e8(OP_STOREX); e8(rb); e8(ri); e8(rs); }

/* ---- host side: sys1 = putc into test buffer ---- */

static char out[256]; static uint32_t outn;
static void s_putc(flint_vm *vm) { if (outn < sizeof(out)) out[outn++] = (char)vm->r[0]; }

/* ---- tests ---- */

static void t_hello(void)
{
    flint_vm vm; uint32_t f_eq, f_jmp, l_load, hlt;
    flint_byte mem[512];

    hasm_reset();
    e_set(2, 8);                       /* r2 = str ptr */
    l_load = cn;
    e_loadb(1, 2, 0);                  /* r1 = byte[r2] */
    e_jeqi(1, 0, &f_eq);               /* \0 -> done */
    e_copy(0, 1);
    e_int(1);                          /* putc(r0) */
    e_inc(2);
    e_jmp(&f_jmp);                     /* loop */
    hlt = cn; e_hlt();
    patch(f_eq, hlt); patch(f_jmp, l_load);

    memset(mem, 0, sizeof(mem));
    memcpy(mem + 8, "FLINT!\n\0", 8);
    outn = 0;

    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_sys_set(&vm, 1, s_putc);
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_OK && outn == 7 && memcmp(out, "FLINT!\n", 7) == 0,
          "hello prints FLINT! via syscall 1");
}

static void t_sum(void)
{
    flint_vm vm; uint32_t f_ge, f_jmp, l_loop, hlt;
    flint_byte mem[512];

    hasm_reset();
    e_set(0, 0);                       /* acc */
    e_set(1, 1);                       /* i   */
    l_loop = cn;
    e_jgei(1, 11, &f_ge);              /* i >= 11 -> done */
    e_add(0, 0, 1);
    e_inc(1);
    e_jmp(&f_jmp);
    hlt = cn; e_hlt();
    patch(f_ge, hlt); patch(f_jmp, l_loop);

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_OK && vm.r[0] == 55, "sum 1..10 = 55, no stack touched");
}

static void t_fact(void)
{
    flint_vm vm; uint32_t f_call, f_call2, f_rec, fact_off, rec_off;
    flint_byte mem[1024];

    hasm_reset();
    e_set(0, 6);
    e_call(&f_call);                   /* call fact */
    e_hlt();

    fact_off = cn;
    e_push(0);
    e_jgei(0, 2, &f_rec);              /* n >= 2 -> rec */
    e_pop(0);
    e_set(1, 1);
    e_ret();

    rec_off = cn;
    e_dec(0);
    e_call(&f_call2);                  /* recurse */
    e_inc(0);
    e_mul(1, 1, 0);                    /* r1 *= n */
    e_pop(0);
    e_ret();

    patch(f_call, fact_off); patch(f_call2, fact_off); patch(f_rec, rec_off);

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_OK && vm.r[1] == 720, "fact(6) = 720, recursion on shared stack");
}

static void t_limit(void)
{
    flint_vm vm;
    uint32_t f;
    static flint_byte mem[512];

    hasm_reset();
    e_inc(0);
    e_jmp(&f);
    patch(f, 2);                       /* self-referencing jump */

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    vm.max_steps = 16;
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_ERR_LIMIT, "step limit kills infinite loop");
}

static void t_badmem(void)
{
    flint_vm vm;
    static flint_byte mem[512];

    hasm_reset();
    e_set(0, 511);                     /* last byte of 512 */
    e_loadw(1, 0, 0);                  /* 8-byte word at 511 walks off the edge */
    e_hlt();

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_ERR_BADMEM, "out-of-range load traps");
}

static void t_stack(void)
{
    flint_vm vm;
    static flint_byte mem[64];

    hasm_reset();
    e8(OP_POP); e8(0);                 /* pop from empty stack */
    e_hlt();

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_ERR_STACK, "pop on empty stack traps");
}

static void t_badreg(void)
{
    flint_vm vm;
    static flint_byte mem[512];

    hasm_reset();
    e8(OP_SET); e8(16); e64(42);       /* r0..r15 are legal; 16 is not */
    e_hlt();

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_ERR_BADREG, "register 16 traps");
}

static void t_reg15(void)
{
    flint_vm vm;
    static flint_byte mem[512];

    hasm_reset();
    e_set(15, 4242);                   /* the expanded register file */
    e_copy(0, 15);
    e_hlt();

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_OK && vm.r[0] == 4242, "r15 round-trips");
}

static void t_frames(void)
{
    flint_vm vm;
    static flint_byte mem[512];

    hasm_reset();
    e_set(0, 11);
    e_enter(8);                       /* one local slot at fp-8 */
    e8(OP_STOREW); e8(16); e8(0); e64(-8);   /* store [fp-8], r0 */
    e_set(0, 0);
    e8(OP_LOADW); e8(0); e8(16); e64(-8);    /* load r0, [fp-8] */
    e_leave();
    e_hlt();

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_OK && vm.r[0] == 11, "enter/leave + fp local");
}

static void t_indexed(void)
{
    flint_vm vm;
    static flint_byte mem[512];

    hasm_reset();
    e_set(1, 16);                     /* base */
    e_set(2, 8);                      /* index */
    e_set(3, 77);
    e_storx(1, 2, 3);                 /* mem[24] = 77 */
    e_set(3, 0);
    e_loadx(4, 1, 2);                 /* r4 = mem[24] */
    e_hlt();

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_OK && vm.r[4] == 77, "loadx/storx base+index");
}

static void t_enter_overflow(void)
{
    flint_vm vm;
    static flint_byte mem[64];

    hasm_reset();
    e_enter(1024);                    /* larger than the whole image */
    e_leave();
    e_hlt();

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_ERR_STACK, "oversized enter traps STACK");
}

static void t_unsigned(void)
{
    /* -1 (0xFFFF..FFFF) vs 1: signed <, unsigned >.
       JLO vs r2=1 must not branch (else marker sets r0=99); JHS must branch over. */
    flint_vm vm; uint32_t f_lo, f_hs, hlt;
    static flint_byte mem[512];

    hasm_reset();
    e_set(1, -1);
    e_set(2, 1);
    e_set(0, 42);
    e8(OP_JLO); e8(1); e8(1); e64(0); tf(&f_lo);  /* r1 lo r1? imm-form avoided: rb=1 */
    e8(OP_JHS); e8(1); e8(1); e64(0); tf(&f_hs);  /* r1 hs r1? */
    e_set(0, 99);                       /* trap marker */
    hlt = cn; e_hlt();

    patch(f_lo, hlt - 10);              /* lo takes -> marker sets r0 = 99 */
    patch(f_hs, hlt);                   /* hs takes  -> skips the marker */

    memset(mem, 0, sizeof(mem));
    flint_init(&vm, cb, cn, mem, sizeof(mem));
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_OK && vm.r[0] == 42,
          "unsigned pair: lo(-1,1) false, hs(-1,1) true");
}

int main(void)
{
    printf("flint core %s\n", FLINT_VERSION);
    t_hello();
    t_sum();
    t_fact();
    t_limit();
    t_badmem();
    t_stack();
    t_badreg();
    t_reg15();
    t_frames();
    t_indexed();
    t_enter_overflow();
    t_unsigned();
    printf("fails: %d\n", fails);
    return fails ? 1 : 0;
}
