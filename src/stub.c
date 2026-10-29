/*
 * stub.c — the fused Flint hosts: one source, two builds.
 *
 *   default              standalone ELF host (`flint build --elf`)
 *   -DBUILD_AS_LIBRARY   shared-library host (`flint build --so`)
 *
 * The Makefile builds this file twice: build/stub.elf (linked as a
 * program) and build/stub.so (-DBUILD_AS_LIBRARY, PIC, -shared).
 * `flint build` copies the matching stub, appends the program's images
 * and a trailing trailer, and the result is self-extracting — the kernel
 * (ELF) or the dynamic loader (.so) maps the stub's segments and ignores
 * the appended bytes:
 *
 *   [ stub ELF ][ code ][ mem ][ struct FlintPayload ]            (FLNT)
 *   [ stub.so  ][ code ][ mem ][ symbols ][ FlintSoTrailer ]      (FLSO)
 *
 * Both hosts locate their own file (the ELF through /proc/self/exe, the
 * library through dladdr on its own code, with a /proc/self/maps
 * fallback for search-path loads), decode the trailer and run the
 * program with the same CLI syscall table `flint run` uses — the
 * open/decode/validate/load sequence is shared in
 * load_embedded_payload(), and only the trailer layout (and the library
 * API) differ per build. A library also accepts the plain FLNT fusion:
 * `main` runs through the library API / JNI, but there are no exported
 * labels for flint_so_call().
 *
 * The library host exports the whole flint_so_* API (include/flint_so.h)
 * plus the JNI entry point Java_com_flint_Native_run for Java/Android
 * hosts.
 *
 * Exit codes (ELF): 127 ("command not found" style) when the trailer is
 * missing or malformed — e.g. running build/stub.elf before it was fused
 * — and the usual flint convention otherwise: r0 at HLT, or
 * -FLINT_ERR_* on a trap.
 *
 * Environment: FLINT_GC_STRESS=1 collects before every allocation;
 * FLINT_GC_STATS=1 prints a collector report to stderr after the run
 * (auto-mode programs; both are ignored by manual-mode payloads).
 */
#ifdef BUILD_AS_LIBRARY
#define _GNU_SOURCE                     /* Dl_info / dladdr */
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flint.h"
#include "flint_cli.h"
#include "flint_stub.h"

#ifdef BUILD_AS_LIBRARY
#include "flint_so.h"
#include <dlfcn.h>
#include <stdarg.h>

/* The JNI entry point at the end of this file needs jint / JNIEnv /
 * jobject / JNIEXPORT / JNICALL. Prefer the real jni.h (JDK / NDK); when
 * the build host does not expose one, fall back to the minimal opaque
 * declarations — the function never calls through the JNIEnv table, so
 * the pointer types plus the default-visibility C ABI are all the export
 * requires (the convention real jni.h uses on Linux). */
#if defined(__has_include)
#  if __has_include(<jni.h>)
#    include <jni.h>
#    define FLINT_JNI_H 1
#  endif
#endif
#ifndef FLINT_JNI_H
typedef int32_t jint;
typedef const struct JNINativeInterface_ *JNIEnv;
typedef void *jobject;
#  define JNIEXPORT __attribute__((visibility("default")))
#  define JNICALL
#endif
#endif /* BUILD_AS_LIBRARY */

/* ---- shared payload loader ----
 *
 * Both trailers are little-endian u32 fields — a file format, so host
 * endianness must not leak into it (same convention as flint_img.c):
 *
 *   struct FlintPayload    magic FLNT, code_size, mem_size, entry,
 *                          heap_start, mem_mode                 (24 bytes)
 *   struct FlintSoTrailer  magic FLSO, version, code_size, mem_size,
 *                          entry, heap_start, mem_mode, sym_count,
 *                          checksum, with sym_count symbol records
 *                          right before it                      (36 bytes)
 *
 * load_embedded_payload() reads the tail of the file once, dispatches on
 * the magic, validates and loads the images. The library build expects
 * the FLSO container and also accepts a plain FLNT fusion; the ELF build
 * expects FLNT only (its window covers exactly the trailer).
 */
enum { STUB_TRAILER_MAX = 36 };         /* the larger trailer; a plain
                                           FLNT trailer ends 12 bytes in */

struct stub_payload {
    flint_byte *code;      /* code image; 3 spare bytes past it for the
                              .so call trampoline, unused by the ELF host */
    uint32_t    code_size; /* real code bytes                            */
    flint_byte *mem;       /* mem image (per-VM template for the .so)    */
    uint32_t    mem_size;
    uint32_t    entry;     /* entry offset into code                     */
    uint32_t    heap_start;/* aligned data end                           */
    flint_mem_mode mem_mode;
#ifdef BUILD_AS_LIBRARY
    unsigned char *syms;   /* FLINT_SO_SYMBOL records (NULL: plain FLNT) */
    uint32_t    nsym;
#endif
};

/* little-endian u32 decode: the trailers are file formats, so host
 * endianness must not leak into them (same convention as flint_img.c) */
static uint32_t stub_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef BUILD_AS_LIBRARY
/* FNV-1a 32 over the code bytes, then mem, then the symbol records —
 * keep in sync with build_so() in flint_cli.c */
static uint32_t stub_checksum(const flint_byte *code, uint32_t cs,
                              const flint_byte *mem, uint32_t ms,
                              const unsigned char *syms, uint32_t ss)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < cs; i++) { h ^= code[i]; h *= 16777619u; }
    for (i = 0; i < ms; i++) { h ^= mem[i];  h *= 16777619u; }
    for (i = 0; i < ss; i++) { h ^= syms[i]; h *= 16777619u; }
    return h;
}
#endif

/* read the fused payload out of `path`: open, decode the trailing
 * trailer, validate, load the images into fresh buffers. Returns 0 and
 * fills *pl, or -1 with a message in err. The loader keeps no state and
 * frees its own allocations on failure. */
static int load_embedded_payload(const char *path, struct stub_payload *pl,
                                 char *err, uint32_t errsz)
{
    unsigned char tr[STUB_TRAILER_MAX];
#ifdef BUILD_AS_LIBRARY
    const uint32_t trailer = FLINT_SO_TRAILER;      /* FLNT fits inside */
#else
    const uint32_t trailer = (uint32_t)sizeof(struct FlintPayload);
#endif
    const unsigned char *pt;                        /* trailer present */
    FILE *f = 0;
    long sz, off;
    uint32_t magic, cs = 0, ms = 0, entry = 0, heap = 0, mode = 0;
#ifdef BUILD_AS_LIBRARY
    uint32_t ver = 0, nsym = 0, sum = 0, i;
    int so_layout = 0;
#endif
    unsigned long long need = 0;
    unsigned char *code = 0, *mem = 0;
#ifdef BUILD_AS_LIBRARY
    unsigned char *syms = 0;
#endif
    int ok = 0;

    memset(pl, 0, sizeof *pl);

    f = fopen(path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open %s", path); return -1; }

    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        snprintf(err, errsz, "cannot size %s", path);
        goto out;
    }
    if ((unsigned long long)sz < (unsigned long long)trailer ||
        fseek(f, -(long)trailer, SEEK_END) != 0 ||
        fread(tr, 1, trailer, f) != trailer) {
#ifdef BUILD_AS_LIBRARY
        snprintf(err, errsz, "not a fused Flint library (truncated trailer)");
#else
        snprintf(err, errsz, "not a Flint binary");
#endif
        goto out;
    }

#ifdef BUILD_AS_LIBRARY
    if (stub_u32(tr + 0) == FLINT_SO_MAGIC) {
        so_layout = 1;                  /* FLSO fills the whole window */
        pt = tr;
    } else {
        /* not FLSO: a plain fusion ends in FlintPayload — the last 24
         * bytes of the 36-byte window */
        pt = tr + (FLINT_SO_TRAILER - sizeof(struct FlintPayload));
    }
#else
    pt = tr;
#endif
    magic = stub_u32(pt + 0);

#ifdef BUILD_AS_LIBRARY
    if (so_layout) {
        ver   = stub_u32(pt + 4);
        cs    = stub_u32(pt + 8);
        ms    = stub_u32(pt + 12);
        entry = stub_u32(pt + 16);
        heap  = stub_u32(pt + 20);
        mode  = stub_u32(pt + 24);
        nsym  = stub_u32(pt + 28);
        sum   = stub_u32(pt + 32);

        if (ver != FLINT_SO_VERSION) {
            snprintf(err, errsz, "unsupported library version %u (want %u)",
                     (unsigned)ver, (unsigned)FLINT_SO_VERSION);
            goto out;
        }
        if (cs == 0 || ms == 0 || cs > FLINT_SO_MAX || ms > FLINT_SO_MAX ||
            entry >= cs || heap > ms || nsym > FLINT_SO_SYM_MAX) {
            snprintf(err, errsz, "bad payload sizes");
            goto out;
        }
        need = (unsigned long long)cs + (unsigned long long)ms +
               (unsigned long long)nsym * FLINT_SO_SYMBOL + FLINT_SO_TRAILER;
    } else
#endif
    {
        if (magic != FLINT_PAYLOAD_MAGIC) {
#ifdef BUILD_AS_LIBRARY
            snprintf(err, errsz, "not a fused Flint library (bad magic)");
#else
            snprintf(err, errsz, "not a Flint binary");
#endif
            goto out;
        }
        cs    = stub_u32(pt + 4);
        ms    = stub_u32(pt + 8);
        entry = stub_u32(pt + 12);
        heap  = stub_u32(pt + 16);
        mode  = stub_u32(pt + 20);
        if (cs == 0 || ms == 0 || entry >= cs || heap > ms) {
            snprintf(err, errsz, "truncated Flint payload");
            goto out;
        }
        need = (unsigned long long)sizeof(struct FlintPayload) +
               (unsigned long long)cs + (unsigned long long)ms;
    }

    if (need > (unsigned long long)sz) {
        snprintf(err, errsz, "truncated payload");
        goto out;
    }
    off = sz - (long)need;

    /* +3: room for the .so call trampoline (`call r15; exit`) — written
     * by the library host after the load, unused by the ELF host */
    code = (unsigned char *)malloc((size_t)cs + 3);
    mem  = (unsigned char *)malloc((size_t)ms);
#ifdef BUILD_AS_LIBRARY
    if (nsym) syms = (unsigned char *)malloc((size_t)nsym * FLINT_SO_SYMBOL);
    if (!code || !mem || (nsym && !syms)) {
#else
    if (!code || !mem) {
#endif
        snprintf(err, errsz, "out of memory");
        goto out;
    }

    if (fseek(f, off, SEEK_SET) != 0 ||
        fread(code, 1, cs, f) != cs ||
        fread(mem, 1, ms, f) != ms) {
        snprintf(err, errsz, "truncated payload");
        goto out;
    }
#ifdef BUILD_AS_LIBRARY
    if (nsym && fread(syms, FLINT_SO_SYMBOL, nsym, f) != nsym) {
        snprintf(err, errsz, "truncated payload");
        goto out;
    }
    if (so_layout &&
        stub_checksum(code, cs, mem, ms, syms,
                      nsym * FLINT_SO_SYMBOL) != sum) {
        snprintf(err, errsz, "checksum mismatch (corrupt library?)");
        goto out;
    }
    for (i = 0; so_layout && i < nsym; i++) {
        const unsigned char *rec = syms + (size_t)i * FLINT_SO_SYMBOL;
        if (stub_u32(rec) >= cs || rec[4] == 0 ||
            !memchr(rec + 4, 0, FLINT_SO_SYM_NAME)) {
            snprintf(err, errsz, "corrupt symbol table");
            goto out;
        }
    }
#endif

    pl->code       = code;
    pl->code_size  = cs;
    pl->mem        = mem;
    pl->mem_size   = ms;
    pl->entry      = entry;
    pl->heap_start = heap;
    pl->mem_mode   = (mode == FLINT_MEM_AUTO) ? FLINT_MEM_AUTO
                                              : FLINT_MEM_MANUAL;
#ifdef BUILD_AS_LIBRARY
    pl->syms       = syms;
    pl->nsym       = nsym;
#endif
    ok = 1;

out:
    if (f) fclose(f);
    if (!ok) {
        free(code);
        free(mem);
#ifdef BUILD_AS_LIBRARY
        free(syms);
#endif
    }
    return ok ? 0 : -1;
}

/* post-load VM preparation shared by both hosts: heap placement, the CLI
 * syscall table, the FLINT_GC_STRESS env hook */
static void stub_vm_prepare(flint_vm *vm, flint_byte *code, uint32_t code_size,
                            flint_byte *mem, uint32_t mem_size,
                            uint32_t heap_start, flint_mem_mode mem_mode)
{
    const char *e;

    flint_init(vm, code, code_size, mem, mem_size);
    flint_heap_set(vm, heap_start, mem_mode);
    flint_cli_sysoff(vm);
    e = getenv("FLINT_GC_STRESS");
    if (e && e[0] && e[0] != '0') flint_gc_set_stress(vm, 1);
}

/* FLINT_GC_STATS=1 report (auto-mode programs only) — the same line both
 * hosts printed before the merge, `label` picks "stub" / "flint-so" */
static void stub_report_gc_stats(const flint_vm *vm, const char *label)
{
    const char *e = getenv("FLINT_GC_STATS");
    flint_gc_stats gs;

    if (!e || !e[0] || e[0] == '0') return;
    flint_gc_get_stats(vm, &gs);
    if (gs.enabled)
        fprintf(stderr, "%s: gc: collections=%u live=%u free=%u "
                "alloc=%u reclaimed=%u\n",
                label, gs.collections, gs.bytes_live, gs.bytes_free,
                gs.bytes_alloced, gs.bytes_reclaimed);
}

#ifdef BUILD_AS_LIBRARY
/* ==================== shared-library host ====================
 * build/stub.so: the payload is read once, in a constructor, and the
 * images are served through the flint_so.h API; flint_so_call() runs an
 * exported label through a two-instruction trampoline (`call r15; exit`)
 * appended past the payload code, so the VM core needs no call-in API.
 * The JNI entry point at the end drives the same machinery for
 * Java/Android hosts. */

/* ---- payload state (read-only after the constructor) ---- */

static flint_byte *so_code;           /* payload code + call trampoline  */
static uint32_t so_code_size;         /* code + trampoline bytes         */
static uint32_t so_code_real;         /* payload code bytes (no trampoline) */
static flint_byte *so_mem;            /* payload mem (per-VM template)   */
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
    char err[256];
    struct stub_payload pl;

    if (!so_self_path(path, sizeof path)) {
        so_set_err("cannot locate the library file");
        return;
    }
    if (load_embedded_payload(path, &pl, err, sizeof err) != 0) {
        so_set_err("%s", err);
        return;
    }

    /* call trampoline past the payload code: call r15; exit */
    pl.code[pl.code_size]     = OP_CALLR;
    pl.code[pl.code_size + 1] = 15;
    pl.code[pl.code_size + 2] = OP_HLT;

    so_code       = pl.code;
    so_code_size  = pl.code_size + 3;
    so_code_real  = pl.code_size;
    so_mem        = pl.mem;
    so_mem_size   = pl.mem_size;
    so_entry      = pl.entry;
    so_heap_start = pl.heap_start;
    so_mem_mode   = pl.mem_mode;
    so_syms       = (struct FlintSoSymbol *)pl.syms;
    so_nsyms      = pl.nsym;
    so_loaded     = 1;
    so_err[0]     = 0;
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

    stub_vm_prepare(vm, so_code, so_code_size, mem, so_mem_size,
                    so_heap_start, so_mem_mode);
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
    stub_report_gc_stats(vm, "flint-so");
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

/* ---- JNI entry point ----
 *
 * The native half of a Java/Android host:
 *
 *     package com.flint;
 *     public class Native { public native int run(); }
 *
 * Runs the payload's main on a fresh VM — the one-shot flint_so_run()
 * input, with the same syscall table and env hooks — and returns the CLI
 * convention: r0 at HLT narrowed like the process exit code
 * (r0 & 0x7FFFFFFF), -(FLINT_ERR_*) when the VM traps or the payload
 * never loaded. For an overloaded Java method add the JNI signature
 * suffix; for `static native int run()` the second parameter is jclass —
 * the C ABI is identical. */
JNIEXPORT jint JNICALL Java_com_flint_Native_run(JNIEnv *env, jobject thiz)
{
    flint_vm *vm;
    flint_word rc;
    jint out;

    (void)env;                          /* the JNI table is never used */
    (void)thiz;
    if (!so_loaded) return -(jint)FLINT_ERR_INIT;
    vm = flint_so_vm_new();
    if (!vm) return -(jint)FLINT_ERR_INIT;

    flint_cli_set_args(0, 0);           /* one-shot run: no program args */
    rc = flint_so_vm_run(vm);
    out = (vm->err != FLINT_OK) ? -(jint)vm->err
                                : (jint)(rc & 0x7FFFFFFF);
    flint_so_vm_free(vm);
    return out;
}

#else /* !BUILD_AS_LIBRARY */
/* ==================== standalone ELF host ==================== */

int main(int argc, char **argv)
{
    struct stub_payload pl;
    char err[256];
    flint_vm vm;
    flint_word rc;

    if (load_embedded_payload("/proc/self/exe", &pl, err, sizeof err) != 0) {
        fprintf(stderr, "stub: %s\n", err);
        return 127;
    }

    stub_vm_prepare(&vm, pl.code, pl.code_size, pl.mem, pl.mem_size,
                    pl.heap_start, pl.mem_mode);
    /* sys 12..14: program arguments are the argv tail (argv[0] is the
     * executable path itself) */
    flint_cli_set_args(argc - 1, argv + 1);
    rc = flint_run(&vm, pl.entry);
    stub_report_gc_stats(&vm, "stub");

    if (vm.err != FLINT_OK) {
        fprintf(stderr, "stub: runtime: %s\n",
                vm.err_text ? vm.err_text : "trap");
        rc = -(flint_word)vm.err;            /* same as the flint CLI */
    } else {
        rc &= 0x7FFFFFFF;                    /* r0 at HLT, int-narrowed */
    }

    free(pl.code);
    free(pl.mem);
    return (int)rc;
}

#endif /* !BUILD_AS_LIBRARY */
