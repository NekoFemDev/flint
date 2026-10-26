/*
 * stub_so.c — shared-library host for a fused Flint library
 * (`flint build --so`).
 *
 * The Makefile links this file (with the VM core and the CLI host) into
 * build/stub.so. `flint build --so` then copies that stub, appends the
 * program's code and mem images, the exported-symbol records and a
 * trailing struct FlintSoTrailer (include/flint_so.h) — one shared
 * library, target/lib<name>.so, that any process can dlopen():
 *
 *   [ stub.so ][ code ][ mem ][ symbols ][ trailer ]   <- dladdr(self)
 *
 * The dynamic loader maps the stub's segments and ignores the appended
 * bytes (the same self-extracting trick as the --elf stub, which reads
 * /proc/self/exe; a library finds its own file through dladdr instead,
 * with a /proc/self/maps fallback for search-path loads). The payload is
 * read once, in a constructor, and validated (magic, version, sizes,
 * checksum, symbol records); the images are served through the API in
 * flint_so.h.
 *
 * Run entry points return r0 at HLT or -FLINT_ERR_* on a trap, matching
 * flint_run. flint_so_call() runs an exported label through a two
 * instruction trampoline (`call r15` + `exit`) appended past the payload
 * code, so the VM core needs no call-in API: r15 carries the target, the
 * callee's `return` lands on the HLT, and r0 comes back as the result.
 *
 * Environment: FLINT_GC_STRESS=1 collects before every allocation;
 * FLINT_GC_STATS=1 prints a collector report to stderr after a
 * flint_so_run() (auto-mode programs; both are ignored by manual-mode
 * payloads) — same convention as the CLI and the --elf stub.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flint.h"
#include "flint_cli.h"
#include "flint_so.h"

/* ---- payload state (read-only after the constructor) ---- */

static unsigned char *so_code;        /* payload code + call trampoline  */
static uint32_t so_code_size;         /* code + trampoline bytes         */
static uint32_t so_code_real;         /* payload code bytes (no trampoline) */
static unsigned char *so_mem;         /* payload mem (per-VM template)   */
static uint32_t so_mem_size;
static uint32_t so_entry;             /* 'main' code offset              */
static uint32_t so_heap_start;
static flint_mem_mode so_mem_mode;
static struct FlintSoSymbol *so_syms; /* exported labels                 */
static uint32_t so_nsyms;
static int so_loaded;
static char so_err[256] = "not loaded";

static void so_set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(so_err, sizeof so_err, fmt, ap);
    va_end(ap);
}

/* little-endian u32 decode: the trailer is a file format, so host
 * endianness must not leak into it (same convention as stub.c) */
static uint32_t so_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* FNV-1a 32 over the code bytes, then mem, then the symbol records —
 * keep in sync with build_so() in flint_cli.c */
static uint32_t so_checksum(const flint_byte *code, uint32_t cs,
                            const flint_byte *mem, uint32_t ms,
                            const flint_byte *syms, uint32_t ss)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < cs; i++) { h ^= code[i]; h *= 16777619u; }
    for (i = 0; i < ms; i++) { h ^= mem[i];  h *= 16777619u; }
    for (i = 0; i < ss; i++) { h ^= syms[i]; h *= 16777619u; }
    return h;
}

/* ---- locating our own file ----
 *
 * dladdr names the file the loader mapped; for a plain path that is
 * directly openable. When the library was found through a search path
 * (LD_LIBRARY_PATH etc.) the name may not resolve from the cwd, so fall
 * back to /proc/self/maps: the mapping that contains one of our
 * functions is always named by its fully resolved path. */

static int so_path_from_maps(const void *addr, char *out, size_t cap)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[4096];
    uintptr_t a = (uintptr_t)addr;

    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char *p;
        int field;
        size_t n;

        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        if (a < (uintptr_t)lo || a >= (uintptr_t)hi) continue;

        /* skip "addr perms offset dev inode"; the rest is the pathname */
        p = line;
        for (field = 0; field < 5; field++) {
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            while (*p == ' ' || *p == '\t') p++;
        }
        n = strcspn(p, "\n");
        if (n == 0 || n >= cap) break;           /* anonymous mapping */
        memcpy(out, p, n);
        out[n] = 0;
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

static int so_self_path(char *out, size_t cap)
{
    Dl_info info;

    out[0] = 0;
    if (dladdr((void *)&flint_so_loaded, &info) && info.dli_fname &&
        info.dli_fname[0]) {
        FILE *probe = fopen(info.dli_fname, "rb");
        if (probe) {
            fclose(probe);
            snprintf(out, cap, "%s", info.dli_fname);
            return 1;
        }
    }
    return so_path_from_maps((const void *)&flint_so_loaded, out, cap);
}

/* ---- payload load (constructor, once per dlopen) ---- */

static void so_load(void)
{
    char path[4096];
    unsigned char tr[FLINT_SO_TRAILER];
    FILE *f;
    long sz;
    uint32_t magic, ver, cs, ms, entry, heap, mode, nsym, sum;
    unsigned long long need;
    long off;
    unsigned char *code = 0, *mem = 0, *syms = 0;
    uint32_t i;

    if (!so_self_path(path, sizeof path)) {
        so_set_err("cannot locate the library file");
        return;
    }
    f = fopen(path, "rb");
    if (!f) { so_set_err("cannot open %s", path); return; }

    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        so_set_err("cannot size %s", path);
        fclose(f);
        return;
    }
    if (sz < (long)FLINT_SO_TRAILER ||
        fseek(f, -(long)FLINT_SO_TRAILER, SEEK_END) != 0 ||
        fread(tr, 1, FLINT_SO_TRAILER, f) != FLINT_SO_TRAILER) {
        so_set_err("not a fused Flint library (truncated trailer)");
        fclose(f);
        return;
    }

    magic = so_u32(tr + 0);
    ver   = so_u32(tr + 4);
    cs    = so_u32(tr + 8);
    ms    = so_u32(tr + 12);
    entry = so_u32(tr + 16);
    heap  = so_u32(tr + 20);
    mode  = so_u32(tr + 24);
    nsym  = so_u32(tr + 28);
    sum   = so_u32(tr + 32);

    if (magic != FLINT_SO_MAGIC) {
        so_set_err("not a fused Flint library (bad magic)");
        fclose(f);
        return;
    }
    if (ver != FLINT_SO_VERSION) {
        so_set_err("unsupported library version %u (want %u)",
                   (unsigned)ver, (unsigned)FLINT_SO_VERSION);
        fclose(f);
        return;
    }
    if (cs == 0 || ms == 0 || cs > FLINT_SO_MAX || ms > FLINT_SO_MAX ||
        entry >= cs || heap > ms || nsym > FLINT_SO_SYM_MAX) {
        so_set_err("bad payload sizes");
        fclose(f);
        return;
    }

    need = (unsigned long long)cs + (unsigned long long)ms +
           (unsigned long long)nsym * FLINT_SO_SYMBOL + FLINT_SO_TRAILER;
    if (need > (unsigned long long)sz) {
        so_set_err("truncated payload");
        fclose(f);
        return;
    }
    off = sz - (long)need;

    code = (unsigned char *)malloc((size_t)cs + 3);   /* + trampoline */
    mem  = (unsigned char *)malloc((size_t)ms);
    if (nsym) syms = (unsigned char *)malloc((size_t)nsym * FLINT_SO_SYMBOL);
    if (!code || !mem || (nsym && !syms)) {
        so_set_err("out of memory");
        free(code); free(mem); free(syms);
        fclose(f);
        return;
    }

    if (fseek(f, off, SEEK_SET) != 0 ||
        fread(code, 1, cs, f) != cs ||
        fread(mem, 1, ms, f) != ms ||
        (nsym && fread(syms, FLINT_SO_SYMBOL, nsym, f) != nsym)) {
        so_set_err("truncated payload");
        free(code); free(mem); free(syms);
        fclose(f);
        return;
    }
    fclose(f);

    if (so_checksum(code, cs, mem, ms, syms, nsym * FLINT_SO_SYMBOL) != sum) {
        so_set_err("checksum mismatch (corrupt library?)");
        free(code); free(mem); free(syms);
        return;
    }

    for (i = 0; i < nsym; i++) {
        const unsigned char *rec = syms + (size_t)i * FLINT_SO_SYMBOL;
        uint32_t pc = so_u32(rec);
        if (pc >= cs || rec[4] == 0 ||
            !memchr(rec + 4, 0, FLINT_SO_SYM_NAME)) {
            so_set_err("corrupt symbol table");
            free(code); free(mem); free(syms);
            return;
        }
    }

    /* call trampoline past the payload code: call r15; exit */
    code[cs]     = OP_CALLR;
    code[cs + 1] = 15;
    code[cs + 2] = OP_HLT;

    so_code      = code;
    so_code_size = cs + 3;
    so_code_real = cs;
    so_mem       = mem;
    so_mem_size  = ms;
    so_entry     = entry;
    so_heap_start = heap;
    so_mem_mode  = (mode == FLINT_MEM_AUTO) ? FLINT_MEM_AUTO : FLINT_MEM_MANUAL;
    so_syms      = (struct FlintSoSymbol *)syms;
    so_nsyms     = nsym;
    so_loaded    = 1;
    so_err[0]    = 0;
}

__attribute__((constructor))
static void so_ctor(void)
{
    so_load();
}

/* release the payload on dlclose (or process exit): a host that cycles
 * dlopen/dlclose must not leak the images, and the constructor simply
 * reloads on the next dlopen */
__attribute__((destructor))
static void so_dtor(void)
{
    free(so_code); so_code = 0;
    free(so_mem);  so_mem = 0;
    free(so_syms); so_syms = 0;
    so_nsyms = 0;
    so_loaded = 0;
    so_set_err("not loaded");
}

/* ---- public API (include/flint_so.h) ---- */

int flint_so_loaded(void) { return so_loaded; }

const char *flint_so_error(void) { return so_loaded ? 0 : so_err; }

flint_vm *flint_so_vm_new(void)
{
    flint_vm *vm;
    flint_byte *mem;

    if (!so_loaded) return 0;
    vm = (flint_vm *)malloc(sizeof *vm);
    mem = (flint_byte *)malloc(so_mem_size);
    if (!vm || !mem) { free(vm); free(mem); return 0; }
    memcpy(mem, so_mem, so_mem_size);

    flint_init(vm, so_code, so_code_size, mem, so_mem_size);
    flint_heap_set(vm, so_heap_start, so_mem_mode);
    flint_cli_sysoff(vm);
    {
        const char *e = getenv("FLINT_GC_STRESS");
        if (e && e[0] && e[0] != '0') flint_gc_set_stress(vm, 1);
    }
    return vm;
}

void flint_so_vm_free(flint_vm *vm)
{
    if (!vm) return;
    free(vm->mem);              /* the private copy flint_so_vm_new made */
    free(vm);
}

flint_word flint_so_vm_run(flint_vm *vm)
{
    if (!vm) return -(flint_word)FLINT_ERR_INIT;
    vm->err = FLINT_OK;         /* a previous trap must not leak into this run */
    vm->err_text = 0;
    return flint_run(vm, so_entry);
}

flint_word flint_so_run_args(int argc, char **argv)
{
    flint_vm *vm;
    flint_word rc;

    if (!so_loaded) return -(flint_word)FLINT_ERR_INIT;
    vm = flint_so_vm_new();
    if (!vm) return -(flint_word)FLINT_ERR_INIT;

    flint_cli_set_args(argc, argv);
    rc = flint_so_vm_run(vm);

    {
        const char *e = getenv("FLINT_GC_STATS");
        if (e && e[0] && e[0] != '0') {
            flint_gc_stats gs;
            flint_gc_get_stats(vm, &gs);
            if (gs.enabled)
                fprintf(stderr, "flint-so: gc: collections=%u live=%u "
                        "free=%u alloc=%u reclaimed=%u\n",
                        gs.collections, gs.bytes_live, gs.bytes_free,
                        gs.bytes_alloced, gs.bytes_reclaimed);
        }
    }

    flint_so_vm_free(vm);
    return rc;
}

flint_word flint_so_run(void)
{
    return flint_so_run_args(0, 0);
}

static int so_find_pc(const char *name)
{
    uint32_t i;

    if (!name) return -1;
    for (i = 0; i < so_nsyms; i++)
        if (strcmp(so_syms[i].name, name) == 0) return (int)so_syms[i].pc;
    return -1;
}

int flint_so_find(const char *name)
{
    if (!so_loaded) return -1;
    return so_find_pc(name);
}

int flint_so_call(flint_vm *vm, const char *name,
                  const flint_word *args, int nargs, flint_word *ret)
{
    int pc, i;
    flint_word rc;
    uint32_t sp0;
    flint_word fp0;

    if (!vm || nargs < 0 || nargs > 4 || (nargs > 0 && !args))
        return FLINT_SO_EBADARG;
    if (!so_loaded) return FLINT_SO_ENOENT;
    pc = so_find_pc(name);
    if (pc < 0) return FLINT_SO_ENOENT;

    for (i = 0; i < nargs; i++) vm->r[i] = args[i];
    vm->r[15] = (flint_word)pc;         /* the trampoline calls r15 */
    vm->err = FLINT_OK;
    vm->err_text = 0;
    sp0 = vm->sp;
    fp0 = vm->fp;
    rc = flint_run(vm, so_code_real);   /* trampoline: call r15; exit */
    if (vm->err != FLINT_OK) {
        /* abandon the call: drop the frames it left behind so the VM
         * stays usable for the next one */
        vm->sp = sp0;
        vm->fp = fp0;
        return FLINT_SO_ETRAP;
    }
    if (ret) *ret = rc;
    return FLINT_SO_OK;
}
