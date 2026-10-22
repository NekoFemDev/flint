/* flint_cli.h — std syscall table + file/project front-end for the flint binary. */
#ifndef FLINT_CLI_H
#define FLINT_CLI_H

#include "flint.h"

void flint_cli_sysoff(flint_vm *vm);

/* file extension predicate (used by main dispatch) */
int has_flint_ext_public(const char *s);

/* single-file front-end */
int flint_cli_run_file(const char *path, uint32_t stack_extra);
int flint_cli_check_file(const char *path);

/* command verbs: arg may be NULL (cwd project) */
int flint_cli_cmd_new(const char *dir);
int flint_cli_cmd_run(const char *arg);   /* .flint file or project dir */
int flint_cli_cmd_check(const char *arg);

#endif /* FLINT_CLI_H */
