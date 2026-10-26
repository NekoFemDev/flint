/*
 * test_so.c — dlopen host for `flint build --so` (make test).
 *
 * Usage: ./build/test_so target/libso_demo.so
 *
 * The library is fused from tests/so_demo.flt: it must load its payload
 * (magic, sizes, checksum), run main through the CLI syscalls, accept
 * program arguments (sys 12), and expose the two `export`-ed labels to
 * flint_so_call() on isolated VM instances.
 */
#include <dlfcn.h>
#include <stdio.h>
#include "flint.h"
#include "flint_so.h"

static int fails;

static void check(int ok, const char *what)
{
    if (ok) printf("PASS  %s\n", what);
    else { printf("FAIL  %s\n", what); fails++; }
}

int main(int argc, char **argv)
{
    void *h;
    int (*loaded)(void);
    const char *(*error)(void);
    flint_word (*run)(void);
    flint_word (*run_args)(int, char **);
    flint_vm *(*vm_new)(void);
    void (*vm_free)(flint_vm *);
    flint_word (*vm_run)(flint_vm *);
    int (*call)(flint_vm *, const char *, const flint_word *, int, flint_word *);
    int (*find)(const char *);
    flint_vm *vm;
    flint_word ret;
    int e;

    if (argc < 2) {
        fprintf(stderr, "usage: test_so <fused.so>\n");
        return 2;
    }
    h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    loaded   = (int (*)(void))dlsym(h, "flint_so_loaded");
    error    = (const char *(*)(void))dlsym(h, "flint_so_error");
    run      = (flint_word (*)(void))dlsym(h, "flint_so_run");
    run_args = (flint_word (*)(int, char **))dlsym(h, "flint_so_run_args");
    vm_new   = (flint_vm *(*)(void))dlsym(h, "flint_so_vm_new");
    vm_free  = (void (*)(flint_vm *))dlsym(h, "flint_so_vm_free");
    vm_run   = (flint_word (*)(flint_vm *))dlsym(h, "flint_so_vm_run");
    call     = (int (*)(flint_vm *, const char *, const flint_word *, int,
                        flint_word *))dlsym(h, "flint_so_call");
    find     = (int (*)(const char *))dlsym(h, "flint_so_find");

    check(loaded && error && run && run_args && vm_new && vm_free &&
          vm_run && call && find, "all flint_so_* symbols resolve");
    if (fails) return 1;

    check(loaded() == 1, "payload loaded (magic, sizes, checksum)");
    check(error() == 0, "no load error");

    check(run() == 0, "flint_so_run -> main exits 0 (argc 0)");
    {
        char *av[] = { (char *)"one", (char *)"two", (char *)"three" };
        check(run_args(3, av) == 3, "flint_so_run_args -> sys 12 sees argc 3");
    }
    check(run() == 0, "flint_so_run resets the argument vector");

    check(find("s_sum3") >= 0 && find("s_triple") >= 0,
          "flint_so_find resolves exported labels");
    check(find("nope") == -1, "flint_so_find -> -1 for unknown names");

    vm = vm_new();
    check(vm != 0, "flint_so_vm_new");

    {
        flint_word a1[] = { 7 };
        ret = 0;
        e = call(vm, "s_triple", a1, 1, &ret);
        check(e == FLINT_SO_OK && ret == 21,
              "call s_triple(7) -> 21 (fresh VM, before main)");

        e = call(vm, "s_triple", a1, 1, 0);
        check(e == FLINT_SO_OK, "call with NULL ret accepted");

        e = call(vm, "nope", 0, 0, &ret);
        check(e == FLINT_SO_ENOENT, "unknown symbol -> FLINT_SO_ENOENT");

        e = call(vm, "s_triple", a1, 9, &ret);
        check(e == FLINT_SO_EBADARG, "nargs > 4 -> FLINT_SO_EBADARG");

        e = call(0, "s_triple", a1, 1, &ret);
        check(e == FLINT_SO_EBADARG, "NULL vm -> FLINT_SO_EBADARG");
    }

    check(vm_run(vm) == 0, "flint_so_vm_run -> 0");
    {
        flint_word a3[] = { 10, 20, 30 };
        ret = 0;
        e = call(vm, "s_sum3", a3, 3, &ret);
        check(e == FLINT_SO_OK && ret == 60, "call s_sum3(10,20,30) -> 60");
    }
    {
        flint_word a1[] = { 42 };
        ret = 0;
        e = call(vm, "s_div0", a1, 1, &ret);
        check(e == FLINT_SO_ETRAP, "trapping export -> FLINT_SO_ETRAP");
        check(vm->err == FLINT_ERR_DIV0, "vm->err names the trap (DIV0)");

        ret = 0;
        e = call(vm, "s_triple", a1, 1, &ret);
        check(e == FLINT_SO_OK && ret == 126,
              "VM usable after the trap (42 * 3)");
    }
    vm_free(vm);

    /* a second instance starts clean — registers are per-VM */
    vm = vm_new();
    {
        flint_word a1[] = { 2 };
        ret = 0;
        e = call(vm, "s_triple", a1, 1, &ret);
        check(e == FLINT_SO_OK && ret == 6, "second VM: s_triple(2) -> 6");
    }
    vm_free(vm);

    dlclose(h);

    /* dlclose runs the library destructor (payload freed); a reload must
     * re-read and re-validate the file from scratch */
    h = dlopen(argv[1], RTLD_NOW);
    check(h != 0, "re-dlopen after dlclose");
    if (h) {
        loaded = (int (*)(void))dlsym(h, "flint_so_loaded");
        run    = (flint_word (*)(void))dlsym(h, "flint_so_run");
        check(loaded && loaded() == 1, "reload: payload loaded again");
        check(run && run() == 0, "reload: main runs again");
        dlclose(h);
    }

    if (fails) { printf("so: %d checks FAILED\n", fails); return 1; }
    printf("so: ok\n");
    return 0;
}
