/*
 * main.c — flint CLI entry.
 *   flint new <dir> [--memory auto|manual]   scaffold a project
 *   flint build <file.flt|dir> [-o out.fbc]  assemble into one image
 *   flint run <file.flt|file.fbc>   run source, or a built image
 *   flint run [dir]        run a project (default: cwd)
 *   flint check <file.flt|file.fbc> assemble/validate without running
 *   flint check [dir]      check a project (default: cwd)
 *   flint dis <file.flt|file.fbc>   print bytecode disassembly
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
        "  flint new <dir> [--memory auto|manual]   scaffold a project\n"
        "  flint build <file.flt|dir> [-o out.fbc]  build a bytecode image\n"
        "  flint run <file.flt|file.fbc>   run source, or a built image\n"
        "  flint run [dir]        run a project (default: cwd)\n"
        "  flint check <file.flt|file.fbc> assemble/validate without running\n"
        "  flint check [dir]      check a project (default: cwd)\n"
        "  flint dis <file.flt|file.fbc>   print bytecode disassembly\n"
        "  flint --version\n"
        "\n"
        "memory model: manual (`alloc`) or auto (`enter`/`leave` frames);\n"
        "`flint new` asks on stdin when --memory is omitted.\n"
        "`flint build` resolves includes into one self-contained .fbc image;\n"
        "`flint run img.fbc` executes it directly (no assembler).\n"
        "\n"
        "exit code: r0 at HLT (or negative error code on trap)\n",
        FLINT_VERSION);
}

/* flint new <dir> [--memory auto|manual] */
static int cmd_new_args(int argc, char **argv)
{
    flint_mem_mode mem = FLINT_MEM_MANUAL;
    const char *dir = 0;
    int mem_given = 0;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--memory") == 0) {
            if (i + 1 >= argc ||
                flint_mem_mode_parse(argv[i + 1], &mem) != 0) {
                fprintf(stderr, "flint new: --memory wants auto or manual\n");
                return 2;
            }
            i++;
            mem_given = 1;
        } else if (strncmp(argv[i], "--memory=", 9) == 0) {
            if (flint_mem_mode_parse(argv[i] + 9, &mem) != 0) {
                fprintf(stderr, "flint new: --memory wants auto or manual\n");
                return 2;
            }
            mem_given = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "flint new: unknown option %s\n", argv[i]);
            return 2;
        } else if (dir == 0) {
            dir = argv[i];
        } else {
            fprintf(stderr, "flint new: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (dir == 0) {
        fprintf(stderr, "flint new: <dir> required\n");
        return 2;
    }
    return flint_cli_cmd_new(dir, mem, mem_given);
}

/* flint build <file.flt|dir> [-o out.fbc] */
static int cmd_build_args(int argc, char **argv)
{
    const char *in = 0;
    const char *out = 0;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "flint build: -o needs a path\n");
                return 2;
            }
            out = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != 0) {
            fprintf(stderr, "flint build: unknown option %s\n", argv[i]);
            return 2;
        } else if (in == 0) {
            in = argv[i];
        } else {
            fprintf(stderr, "flint build: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    return flint_cli_cmd_build(in, out);
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
    if (strcmp(cmd, "new") == 0)
        return cmd_new_args(argc - 2, argv + 2);
    if (strcmp(cmd, "build") == 0)
        return cmd_build_args(argc - 2, argv + 2);
    if (strcmp(cmd, "run") == 0)
        return flint_cli_cmd_run(argc >= 3 ? argv[2] : 0);
    if (strcmp(cmd, "check") == 0)
        return flint_cli_cmd_check(argc >= 3 ? argv[2] : 0);
    if (strcmp(cmd, "dis") == 0)
        return flint_cli_cmd_dis(argc >= 3 ? argv[2] : 0);

    /* convenience: `flint path/file.flt` == `flint run path/file.flt` */
    if (has_flint_ext_public(cmd))
        return flint_cli_run_file(cmd, 65536, 0, FLINT_MEM_MANUAL);
    if (has_fbc_ext_public(cmd))
        return flint_cli_run_image(cmd);

    usage(stderr);
    return 2;
}
