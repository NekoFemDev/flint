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
 *   7  stat       r0 = path addr, r1 = out buf (8 bytes); the file size
 *                 (int64) lands in the buffer -> 0, or -1 on error
 *   8  mkdir      r0 = path addr, r1 = mode (e.g. 0755) -> 0 or -1
 *   9  unlink     r0 = path addr -> 0 or -1
 *   10 time       -> r0 = epoch seconds
 *   11 regs       debug dump r0..r15 (not part of the stable ABI)
 *   12 argc       -> r0 = number of program arguments
 *   13 getarg     r0 = index, r1 = buf, r2 = cap -> full length or -1
 *   14 getenv     r0 = name, r1 = buf, r2 = cap -> full length or -1
 *   15 lseek      r0 = fd, r1 = offset, r2 = whence (0/1/2) -> offset, -1
 *   16 rename     r0 = old path, r1 = new path -> 0 or -1
 *   17 rmdir      r0 = path -> 0 or -1
 *   18 opendir    r0 = path -> handle or -1
 *   19 readdir    r0 = handle, r1 = buf, r2 = cap -> name length,
 *                 0 = end of directory, -1 = bad handle/error
 *   20 closedir   r0 = handle -> 0 or -1
 *   21 socket     AF_INET/SOCK_STREAM (TCP) -> fd or -1
 *   22 connect    r0 = fd, r1 = dotted-quad addr in mem, r2 = port
 *                 (1..65535) -> 0 or -1
 *   23 send       r0 = fd, r1 = buf, r2 = len -> bytes sent or -1
 *   24 recv       r0 = fd, r1 = buf, r2 = max (>= 1) -> bytes (0 = peer
 *                 closed the connection) or -1
 *   25 close_socket  r0 = fd -> 0 or -1
 * Slots 0..6 are the generic standard set (flint.h FLINT_SYS_*); 7..25
 * are the CLI extensions (7..9 file-meta, 10..11 time/debug, 12..14
 * args/env, 15..20 files/dirs, 21..25 network). */
enum {
    CLI_SYS_STAT   = 7,
    CLI_SYS_MKDIR  = 8,
    CLI_SYS_UNLINK = 9,
    CLI_SYS_TIME   = 10,
    CLI_SYS_REGS   = 11,
    CLI_SYS_ARGC   = 12,
    CLI_SYS_GETARG = 13,
    CLI_SYS_GETENV = 14,
    CLI_SYS_LSEEK  = 15,
    CLI_SYS_RENAME = 16,
    CLI_SYS_RMDIR  = 17,
    CLI_SYS_OPENDIR      = 18,
    CLI_SYS_READDIR      = 19,
    CLI_SYS_CLOSEDIR     = 20,
    CLI_SYS_SOCKET       = 21,
    CLI_SYS_CONNECT      = 22,
    CLI_SYS_SEND         = 23,
    CLI_SYS_RECV         = 24,
    CLI_SYS_CLOSE_SOCKET = 25
};

void flint_cli_sysoff(flint_vm *vm);

/* program arguments for the next run (sys 12..14): the front-end passes
 * the argv tail that followed the script or project, the stub passes
 * its own process argv minus argv[0]. The pointers must stay valid
 * while the program runs; `--threads` runs never call this. */
void flint_cli_set_args(int argc, char **argv);

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

/* interactive REPL (flint repl): assemble and run one statement per line
 * on a persistent 64 KiB VM — registers, memory and the heap survive
 * between lines; each line gets a 1000-step budget; `sys 11` (regs)
 * dumps the registers. `exit` alone, or EOF, ends the session. Returns 0
 * when the session closes cleanly, 2 on allocation failure. */
int flint_repl(void);

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

/* build kind: bytecode image / standalone ELF / fused shared library */
typedef enum {
    FLINT_BUILD_FBC = 0,
    FLINT_BUILD_ELF,
    FLINT_BUILD_SO
} flint_build_kind;

/* assemble <file.flt|dir> into one .fbc image; out NULL = build/<name>.fbc.
 * FLINT_BUILD_ELF instead fuses build/stub.elf + the program into a
 * standalone executable (out NULL = target/<name>) — see flint_stub.h;
 * FLINT_BUILD_SO fuses build/stub.so + the images + the exported symbols
 * into a dlopen-able shared library (out NULL = target/lib<name>.so) —
 * see flint_so.h */
int flint_cli_cmd_build(const char *arg, const char *out,
                        flint_build_kind kind);
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
