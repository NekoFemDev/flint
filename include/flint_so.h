/* flint_so.h — fused shared library container (`flint build --so`).
 *
 * A standalone shared library is a copy of the stub shared object with
 * the program's images and exported-symbol table appended, fixed by a
 * trailing header — the same self-extracting-container style as the
 * --elf stub (flint_stub.h), with one difference: the dynamic loader
 * maps a library's segments and ignores everything past them, so the
 * fused file stays a valid .so for dlopen():
 *
 *   [ stub.so ][ code image ][ mem image ][ symbols ][ trailer ]
 *
 * At load time the stub finds its own file (dladdr on its own code, with
 * a /proc/self/maps fallback), decodes the trailer at the end, loads the
 * images and serves the exported symbols through flint_so_call() — no
 * flint install, no sources, no .fbc needed by the host process.
 *
 * All trailer fields are little-endian u32 (same convention as .fbc /
 * FlintPayload). The magic is chosen so the four bytes on disk spell
 * "FLSO"; the checksum is FNV-1a 32 over the code bytes, then the mem
 * bytes, then the symbol records exactly as written. The symbol table
 * holds one fixed-size record per `export`-ed code label:
 *
 *   offset  size  field
 *   0       4     pc    (code offset of the label)
 *   4       60    name  (NUL-terminated, same cap as flint_sym)
 *
 * `flint build --so` needs build/stub.so (built by the Makefile, shipped
 * in release archives) and refuses to build without it, like --elf.
 *
 * Load a fused library with RTLD_LOCAL (the dlopen default): it exports
 * the whole flint core API, and under RTLD_GLOBAL those symbols would
 * interpose across the host and any other fused library. Separate
 * libraries carry separate payload state, so several can coexist.
 *
 * Error convention (mirrors the assembler): 0 on success, negative codes
 * for flint_so_call (below), -FLINT_ERR_* from the run entry points.
 */
#ifndef FLINT_SO_H
#define FLINT_SO_H

#include "flint.h"

#define FLINT_SO_MAGIC      0x4F534C46u     /* "FLSO" as bytes on disk (LE) */
#define FLINT_SO_VERSION    1u
#define FLINT_SO_STUB_PATH  "build/stub.so" /* built by the Makefile */
#define FLINT_SO_SYM_NAME   60u             /* name bytes per record, NUL incl. */
#define FLINT_SO_SYMBOL     64u             /* bytes per symbol record */
#define FLINT_SO_SYM_MAX    65536u          /* sanity cap on exported labels */
#define FLINT_SO_TRAILER    36u             /* nine u32 fields */
#define FLINT_SO_MAX        0x40000000u     /* 1 GiB per section: sanity cap */

/* trailer at the end of a fused .so; the images and the symbol records
 * precede it, in that order, so:
 *   payload_off = filesize - sizeof(trailer)
 *                          - code_size - mem_size - sym_count * 64     */
struct FlintSoTrailer {
    uint32_t magic;       /* FLINT_SO_MAGIC                          */
    uint32_t version;     /* FLINT_SO_VERSION                        */
    uint32_t code_size;   /* byte length of the code image           */
    uint32_t mem_size;    /* byte length of the mem image            */
    uint32_t entry;       /* entry offset into code (flint_prog.entry) */
    uint32_t heap_start;  /* aligned data end (flint_prog.heap_start) */
    uint32_t mem_mode;    /* flint_mem_mode: 0 manual / 1 auto       */
    uint32_t sym_count;   /* exported symbols (each FLINT_SO_SYMBOL) */
    uint32_t checksum;    /* FNV-1a 32 over code, mem, symbol records */
} __attribute__((packed));

/* one exported label; fields are encoded field-by-field (u32 LE + raw
 * name bytes), the struct documents the on-disk layout */
struct FlintSoSymbol {
    uint32_t pc;
    char     name[FLINT_SO_SYM_NAME];
} __attribute__((packed));

/* the sizes are a file-format ABI, keep them padding-free */
typedef char flint__assert_so_trailer_is_36[
    (sizeof(struct FlintSoTrailer) == 36) ? 1 : -1];
typedef char flint__assert_so_symbol_is_64[
    (sizeof(struct FlintSoSymbol) == 64) ? 1 : -1];
typedef char flint__assert_sym_names_match[
    (FLINT_SO_SYM_NAME == FLINT_SYM_NAME_MAX) ? 1 : -1];

/* ---- the API a fused .so exports (include this header in the host) ----
 *
 * flint_so_call result codes (run entry points return r0 at HLT, or
 * -FLINT_ERR_* on a trap instead): */
enum {
    FLINT_SO_OK      =  0,
    FLINT_SO_ENOENT  = -1,   /* no such exported symbol (or no payload)   */
    FLINT_SO_EBADARG = -2,   /* nargs outside 0..4                        */
    FLINT_SO_ETRAP   = -3    /* the VM trapped; inspect vm->err/err_text  */
};

/* payload state: flint_so_loaded() is 1 once the trailer decoded and the
 * checksum passed; flint_so_error() names the failure otherwise (NULL
 * when loaded). */
int         flint_so_loaded(void);
const char *flint_so_error(void);

/* one-shot runs of `main` on a fresh VM each call: the return is r0 at
 * HLT (raw 64-bit, same convention as flint_run), or -FLINT_ERR_* on a
 * trap. _args also feeds sys 12..14 (argc/getarg/getenv); the argument
 * vector must stay valid for the duration of the call (it is referenced,
 * not copied, and is library-global — do not change it concurrently). */
flint_word  flint_so_run(void);
flint_word  flint_so_run_args(int argc, char **argv);

/* isolated VM instances: flint_so_vm_new() copies the payload's mem
 * image and installs the CLI syscall table (stdio + POSIX files); the
 * shared code image is read-only. Free with flint_so_vm_free() — do not
 * free() the struct or its mem buffer yourself, and only pass VMs this
 * library created. The whole flint core API (flint_sys_set, flint_step,
 * flint_gc_*, ...) works on them. */
flint_vm   *flint_so_vm_new(void);
void        flint_so_vm_free(flint_vm *vm);

/* run `main` on an existing VM; resets a previous trap first */
flint_word  flint_so_vm_run(flint_vm *vm);

/* call an exported label by name: up to 4 arguments travel in r0..r3,
 * the return value comes back from r0. vm must be an instance this
 * library created (flint_so_vm_new — the call runs against the shared
 * payload code). The label must `return` (an `exit` halts the call, the
 * trampoline's HLT never runs) and follow the register ABI — r15
 * carries the target, so the callee must preserve it (callee-saved by
 * convention). Returns FLINT_SO_OK on success with *ret filled (ret may
 * be NULL), FLINT_SO_ENOENT for an unknown name, FLINT_SO_EBADARG for a
 * bad vm/nargs, FLINT_SO_ETRAP on a VM trap — the call is then
 * abandoned and the stack/frame pointers restored, so the VM stays
 * usable (inspect vm->err / vm->err_text before the next call resets
 * them). */
int         flint_so_call(flint_vm *vm, const char *name,
                          const flint_word *args, int nargs,
                          flint_word *ret);

/* code offset of an exported label, or -1 when unknown / not loaded */
int         flint_so_find(const char *name);

#endif /* FLINT_SO_H */
