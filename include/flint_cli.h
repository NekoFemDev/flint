/* flint_cli.h — std syscall table + file/project front-end for the flint binary. */
#ifndef FLINT_CLI_H
#define FLINT_CLI_H

#include "flint.h"

/* CLI host syscall table — slot map. Args travel in r0..r2, results in r0.
 *   0  print_str  r1 = addr, r2 = len; raw bytes to stdout; no clobber
 *   1  putc       low byte of r0 -> stdout
 *   2  getc       stdin byte -> r0 (-1 on EOF)
 *   3  open       r0 = path addr, r1 = flags (0 read / 1 write) -> fd
 *   4  read       r0 = fd, r1 = buf, r2 = len -> bytes or -1
 *   5  write      r0 = fd, r1 = buf, r2 = len -> bytes or -1
 *   6  close      r0 = fd -> 0 or -1
 *   8  time       -> r0 = epoch seconds
 *   9  regs       debug dump r0..r15 (not part of the stable ABI)
 * Slots 0..6 are the generic standard set (flint.h FLINT_SYS_*). */
enum {
    CLI_SYS_TIME = 8,
    CLI_SYS_REGS = 9
};

void flint_cli_sysoff(flint_vm *vm);

/* file predicates (used by main dispatch) */
int has_flint_ext_public(const char *s);   /* .flt source            */
int has_fbc_ext_public(const char *s);     /* .fbc bytecode image    */

/* memory model (flint.h flint_mem_mode) as text: "manual" / "auto".
 * parse accepts either spelling, case-insensitively, ignoring surrounding
 * blanks and a trailing newline (prompt input); returns 0 on success and
 * -1 for anything else. */
const char *flint_mem_mode_name(flint_mem_mode mode);
int flint_mem_mode_parse(const char *s, flint_mem_mode *out);

/* single-file front-end. mem is the project memory model (v0.3.0: the
 * allocator switch — see flint_mem_mode). */
int flint_cli_run_file(const char *path, uint32_t stack_extra,
                       const char *include_root, flint_mem_mode mem);
int flint_cli_check_file(const char *path, const char *include_root);

/* bytecode-image front-end (.fbc written by `flint build`): execute the
 * stored code+mem images directly, no assembler at run time */
int flint_cli_run_image(const char *path);

/* run options (flint run --gc-stats --gc-stress --memory ...).
 * gc_stress also comes from FLINT_GC_STRESS=1 / gc_stats from
 * FLINT_GC_STATS=1 when the flags are absent. */
typedef struct {
    int gc_stats;              /* print a collector stats line (stderr) */
    int gc_stress;             /* collect before every allocation       */
    int memory_given;          /* --memory override present             */
    flint_mem_mode memory;     /* override for files/dirs (also .fbc)   */
} flint_run_opts;

int flint_cli_cmd_run_opts(const char *arg, const flint_run_opts *opts);

/* command verbs: arg may be NULL (cwd project) */
/* scaffold <dir>. mem_given == 0 asks on stdin (Enter = manual). */
int flint_cli_cmd_new(const char *dir, flint_mem_mode mem, int mem_given);
/* assemble <file.flt|dir> into one .fbc image; out NULL = build/<name>.fbc.
 * elf != 0 instead fuses build/stub.elf + the program into a standalone
 * executable (out NULL = target/<name>) — see flint_stub.h */
int flint_cli_cmd_build(const char *arg, const char *out, int elf);
int flint_cli_cmd_run(const char *arg);   /* .flt file or project dir */
int flint_cli_cmd_check(const char *arg);
int flint_cli_cmd_dis(const char *arg);

/* host-level parallelism: run the same program in num_threads OS threads,
 * each with its OWN flint_vm — every worker reads (or assembles) its own
 * copy of the file, so code, memory and stack are fully isolated and the
 * only shared state is stdout/stderr. filename must be a .flt source or
 * a .fbc image; stack_size clamps like a project's [stack] and is the
 * assembler stack reserve for .flt sources (ignored for .fbc images).
 * Returns 0 when every worker finished with r0 == 0, the first non-zero
 * r0 in thread order otherwise, -(FLINT_ERR_*) on a VM trap, or 2 on a
 * file/argument failure. */
int flint_run_parallel(const char *filename, int num_threads,
                       uint32_t stack_size);

/* disassembler (flint_dis.c): decode code image to stdout, 0 on ok */
int flint_dis(const flint_byte *code, uint32_t n);

#endif /* FLINT_CLI_H */
