/*
 * test_heap.c — allocator bench: manual bump arena vs auto collector.
 *
 * Manual: OP_ALLOC bumps a cursor from the aligned end of the data
 * image; OP_FREE validates its operand and does nothing; a RET never
 * rewinds the cursor, so a callee's block stays valid (returning an
 * array is safe).
 *
 * Auto (v0.3.0): OP_ALLOC returns the payload of a collector-managed
 * block — a 16-byte header first, payload zeroed — and the mark-sweep
 * collector owns lifetime. The assertions below pin the arithmetic
 * while nothing is unreachable yet and no collection is forced, so the
 * blocks simply march up from the heap base like a bump arena shifted
 * by the per-block header.
 */
#include <stdio.h>
#include <string.h>
#include "flint.h"

static int fails = 0;
#define CHECK(cond, msg) \
    do { if (cond) printf("PASS  %s\n", (msg)); \
         else     { printf("FAIL  %s\n", (msg)); fails++; } } while (0)

/* assemble + run the same program in the requested mode */
static void rmode(const char *src, flint_mem_mode mode,
                  flint_vm *vm, flint_word *rc)
{
    flint_prog p; char err[256] = "";
    if (flint_assemble(src, &p, 4096, err, sizeof err) != 0) {
        printf("FAIL  assemble: %s\n", err[0] ? err : "?"); fails++;
        memset(vm, 0, sizeof *vm);
        *rc = 0;
        return;
    }
    flint_init(vm, p.code, p.code_size, p.mem, p.mem_size);
    flint_heap_set(vm, p.heap_start, mode);
    *rc = flint_run(vm, p.entry);
    flint_prog_free(&p);
}

static const char *S_basic =
"data:\n"
"pad: bytes 32\n"
"code:\n"
"helper:\n"
"    alloc r1, 16\n"
"    free r1\n"
"    return\n"
"main:\n"
"    alloc r2, 32\n"
"    call helper\n"
"    alloc r3, 32\n"
"    copy r0, r3\n"
"    exit\n";

static void t_basic(void)
{
    flint_vm vm; flint_word rc;
    flint_word base;

    rmode(S_basic, FLINT_MEM_MANUAL, &vm, &rc);
    CHECK(vm.err == FLINT_OK, "manual: run clean");
    base = (flint_word)vm.heap_start;
    CHECK(vm.r[2] == base,                     "manual: first alloc at heap_start");
    CHECK(rc == base + 32 + 16,                "manual: helper's block still counted");
    CHECK(vm.heap_ptr == base + 32 + 16 + 32,  "manual: cursor not rewound by RET");

    rmode(S_basic, FLINT_MEM_AUTO, &vm, &rc);
    CHECK(vm.err == FLINT_OK, "auto: run clean");
    CHECK(vm.r[2] == base + 16,                 "auto: first payload after header");
    CHECK(rc == base + 96,                      "auto: callee block counted (no GC yet)");
    CHECK(vm.heap_ptr == base + 128,            "auto: frontier after three blocks");
}

static const char *S_nested =
"code:\n"
"g:\n"
"    alloc r1, 8\n"
"    return\n"
"f:\n"
"    alloc r1, 8\n"
"    call g\n"
"    return\n"
"main:\n"
"    alloc r0, 32\n"
"    call f\n"
"    alloc r0, 32\n"
"    exit\n";

static void t_nested(void)
{
    flint_vm vm; flint_word rc;
    flint_word base;

    rmode(S_nested, FLINT_MEM_MANUAL, &vm, &rc);
    CHECK(vm.err == FLINT_OK, "manual nested: run clean");
    base = (flint_word)vm.heap_start;
    CHECK(rc == base + 32 + 8 + 8 && vm.heap_ptr == base + 80,
          "manual nested: f and g blocks persist");

    rmode(S_nested, FLINT_MEM_AUTO, &vm, &rc);
    CHECK(vm.err == FLINT_OK, "auto nested: run clean");
    CHECK(rc == base + 112 && vm.heap_ptr == base + 144,
          "auto nested: f and g blocks honored until a collection runs");
}

static const char *S_ret_array =
"code:\n"
"helper:\n"
"    alloc r1, 16\n"
"    set r2, 42\n"
"    store [r1], r2\n"
"    copy r0, r1\n"
"    return\n"
"main:\n"
"    call helper\n"
"    load r1, [r0]\n"
"    copy r0, r1\n"
"    exit\n";

static void t_return_array(void)
{
    flint_vm vm; flint_word rc;

    /* the audit fix: a pointer returned by a callee must not dangle */
    rmode(S_ret_array, FLINT_MEM_MANUAL, &vm, &rc);
    CHECK(vm.err == FLINT_OK && rc == 42,
          "returned allocation stays valid (manual)");
    rmode(S_ret_array, FLINT_MEM_AUTO, &vm, &rc);
    CHECK(vm.err == FLINT_OK && rc == 42,
          "returned allocation stays valid (auto)");
}

static void t_trap(void)
{
    flint_vm vm; flint_word rc;

    rmode("code:\nmain:\n    alloc r0, 1000000\n    exit\n", FLINT_MEM_MANUAL, &vm, &rc);
    CHECK(vm.err == FLINT_ERR_BADMEM, "manual: alloc past the stack pointer traps");

    rmode("code:\nmain:\n    alloc r0, -8\n    exit\n", FLINT_MEM_MANUAL, &vm, &rc);
    CHECK(vm.err == FLINT_ERR_BADMEM, "manual: negative alloc size traps");

    rmode("code:\nmain:\n    alloc r0, 1000000\n    exit\n", FLINT_MEM_AUTO, &vm, &rc);
    CHECK(vm.err == FLINT_ERR_OOM, "auto: alloc beyond memory traps OOM");

    rmode("code:\nmain:\n    alloc r0, -8\n    exit\n", FLINT_MEM_AUTO, &vm, &rc);
    CHECK(vm.err == FLINT_ERR_BADMEM, "auto: negative alloc size traps");
}

static void t_badfree(void)
{
    static flint_byte code[] = { OP_FREE, 16, OP_HLT };
    static flint_byte mem[64];
    flint_vm vm;

    flint_init(&vm, code, sizeof code, mem, sizeof mem);
    flint_heap_set(&vm, 16, FLINT_MEM_MANUAL);
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_ERR_BADREG, "free r16 (hand-encoded) traps BADREG");

    flint_init(&vm, code, sizeof code, mem, sizeof mem);
    flint_heap_set(&vm, 16, FLINT_MEM_AUTO);
    flint_run(&vm, 0);
    CHECK(vm.err == FLINT_ERR_BADREG, "auto: free r16 still traps BADREG");
}

int main(void)
{
    printf("flint heap bench %s\n", FLINT_VERSION);
    t_basic();
    t_nested();
    t_return_array();
    t_trap();
    t_badfree();
    printf("fails: %d\n", fails);
    return fails ? 1 : 0;
}
