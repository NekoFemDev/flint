/*
 * main.c — flint CLI entry.
 *   flint new <dir>          scaffold a project
 *   flint run <file.flint>   run a program file
 *   flint run [dir]          run a project (default: cwd)
 *   flint check <file.flint> assemble without running
 *   flint check [dir]        check a project (default: cwd)
 */
#include <stdio.h>
#include <string.h>
#include "flint.h"
#include "flint_cli.h"

static void usage(FILE *o)
{
    fprintf(o,
        "flint %s — minimal assembly language VM\n"
        "\n"
        "usage:\n"
        "  flint new <dir>          scaffold a project\n"
        "  flint run <file.flint>   run a program file\n"
        "  flint run [dir]          run a project (default: cwd)\n"
        "  flint check <file.flint> assemble without running\n"
        "  flint check [dir]        check a project (default: cwd)\n"
        "  flint --version\n"
        "\n"
        "exit code: r0 at HLT (or negative error code on trap)\n",
        FLINT_VERSION);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "--version") == 0) {
        printf("flint %s\n", FLINT_VERSION);
        return 0;
    }
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "new") == 0) {
        if (argc < 3) {
            fprintf(stderr, "flint new: <dir> required\n");
            return 2;
        }
        return flint_cli_cmd_new(argv[2]);
    }
    if (strcmp(cmd, "run") == 0)
        return flint_cli_cmd_run(argc >= 3 ? argv[2] : 0);
    if (strcmp(cmd, "check") == 0)
        return flint_cli_cmd_check(argc >= 3 ? argv[2] : 0);

    /* convenience: `flint path/file.flint` == `flint run path/file.flint` */
    if (has_flint_ext_public(cmd))
        return flint_cli_run_file(cmd, 65536);

    usage(stderr);
    return 2;
}
