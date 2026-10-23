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

/* file extension predicate (used by main dispatch) */
int has_flint_ext_public(const char *s);

/* single-file front-end */
/* use_manual_ram is retained for compatibility; since v0.2.0 RETURN never
 * rewinds the heap cursor, so the flag no longer changes allocation lifetime */
int flint_cli_run_file(const char *path, uint32_t stack_extra,
                       const char *include_root, int use_manual_ram);
int flint_cli_check_file(const char *path, const char *include_root);

/* command verbs: arg may be NULL (cwd project) */
int flint_cli_cmd_new(const char *dir);
int flint_cli_cmd_run(const char *arg);   /* .flt file or project dir */
int flint_cli_cmd_check(const char *arg);
int flint_cli_cmd_dis(const char *arg);

/* disassembler (flint_dis.c): decode code image to stdout, 0 on ok */
int flint_dis(const flint_byte *code, uint32_t n);

#endif /* FLINT_CLI_H */
