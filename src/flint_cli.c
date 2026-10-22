/*
 * flint_cli.c — executable host: syscalls on stdio + file/project front-end.
 * This TU is the "OS" for FLINT programs. Embeddable hosts replace it
 * with their own syscall table and skip the CLI layer entirely.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "flint.h"
#include "flint_cli.h"

/* ---------------- standard syscalls ---------------- */

/* SYS 0: print r0 as signed decimal + newline */
static void std_prn(flint_vm *vm)
{
    printf("%lld\n", (long long)vm->r[0]);
}

/* SYS 1: putc — print low byte of r0 */
static void std_putc(flint_vm *vm)
{
    fputc((int)(vm->r[0] & 0xFF), stdout);
}

/* SYS 2: getc -> r0, -1 on EOF */
static void std_getc(flint_vm *vm)
{
    int c = fgetc(stdin);
    vm->r[0] = (c == EOF) ? -1 : (flint_word)c;
}

void flint_cli_sysoff(flint_vm *vm)
{
    flint_sys_set(vm, 0, std_prn);
    flint_sys_set(vm, 1, std_putc);
    flint_sys_set(vm, 2, std_getc);
}

/* ---------------- file helpers ---------------- */

static char *read_whole(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return 0; }
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return 0; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(buf); return 0;
    }
    buf[len] = 0;
    fclose(f);
    *out_len = len;
    return buf;
}

/* run one assembled program; exit code = r0 at HLT, or -(ERR) on trap */
int flint_cli_run_file(const char *path, uint32_t stack_extra)
{
    long len;
    char *src = read_whole(path, &len);
    if (!src) {
        fprintf(stderr, "flint: %s: %s\n", path, strerror(errno));
        return 2;
    }

    flint_prog p; char err[384] = "";
    if (flint_assemble(src, &p, stack_extra, err, sizeof(err)) != 0) {
        fprintf(stderr, "flint: %s: %s\n", path, err);
        free(src);
        return 1;
    }
    free(src);

    flint_vm vm;
    flint_init(&vm, p.code, p.code_size, p.mem, p.mem_size);
    flint_cli_sysoff(&vm);
    flint_word rc = flint_run(&vm, p.entry);

    int code;
    if (vm.err == FLINT_OK) {
        code = (int)(rc & 0x7FFFFFFF);
    } else {
        fprintf(stderr, "flint: runtime: %s\n", vm.err_text);
        code = (int)(-(int64_t)vm.err);
    }
    flint_prog_free(&p);
    return code;
}

int flint_cli_check_file(const char *path)
{
    long len;
    char *src = read_whole(path, &len);
    if (!src) {
        fprintf(stderr, "flint: %s: %s\n", path, strerror(errno));
        return 2;
    }

    flint_prog p; char err[384] = "";
    int rc = flint_assemble(src, &p, 4096, err, sizeof(err));
    free(src);
    if (rc != 0) {
        fprintf(stderr, "flint: %s: %s\n", path, err);
        return 1;
    }
    printf("flint: %s: ok, %u code bytes\n", path, p.code_size);
    flint_prog_free(&p);
    return 0;
}

/* ---------------- project layer (Flint.toml) ---------------- */

typedef struct {
    char name[64];
    char entry[128];
    long stack;                 /* stack extra bytes; default 65536 */
} flint_proj;

static void trim_ws(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (unsigned char)s[n - 1] <= ' ') s[--n] = 0;
    size_t off = 0;
    while (s[off] != 0 && (unsigned char)s[off] <= ' ') off++;
    if (off > 0) memmove(s, s + off, strlen(s) - off + 1);
}

static uint32_t clamp_stack(long s)
{
    if (s < 1024)   s = 65536;
    if (s > 65536000) s = 65536000;
    return (uint32_t)s;
}

/* parse the TOML subset we support; returns 0/-1, err gets a message */
static int proj_load(const char *root, flint_proj *pr, char *err, size_t ecap)
{
    char path[512];
    snprintf(path, sizeof path, "%.480s/Flint.toml", root);
    long len;
    char *src = read_whole(path, &len);
    if (!src) {
        snprintf(err, ecap, "cannot read %s (not a project dir?)", path);
        return -1;
    }

    pr->stack = 65536;
    pr->name[0] = 0;
    snprintf(pr->entry, sizeof pr->entry, "src/main.flint");

    char *cur = src;
    while (*cur != 0) {
        char *nl = strchr(cur, '\n');
        if (nl != 0) *nl = 0;

        char line[512];
        snprintf(line, sizeof line, "%.500s", cur);
        trim_ws(line);
        if (line[0] != 0 && line[0] != '#' && line[0] != '[') {
            char *eq = strchr(line, '=');
            if (eq != 0) {
                *eq = 0;
                char *key = line, *val = eq + 1;
                trim_ws(key);
                trim_ws(val);
                size_t vl = strlen(val);
                if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') {
                    val[vl - 1] = 0;
                    val++;
                }
                if (strcmp(key, "name") == 0) {
                    snprintf(pr->name, sizeof pr->name, "%.48s", val);
                } else if (strcmp(key, "entry") == 0) {
                    snprintf(pr->entry, sizeof pr->entry, "%.100s", val);
                } else if (strcmp(key, "stack") == 0) {
                    pr->stack = strtol(val, 0, 10);
                }
            }
        }
        if (nl != 0) cur = nl + 1; else break;
    }
    free(src);
    return 0;
}

/* resolve project root -> entry path + stack */
static int open_project(const char *root, char *entry, size_t cap,
                        uint32_t *stack, char *err, size_t ecap)
{
    flint_proj pr;
    if (proj_load(root, &pr, err, ecap) != 0) return -1;
    int n = snprintf(entry, cap, "%.380s/%.100s", root, pr.entry);
    if (n < 0 || (size_t)n >= cap) {
        snprintf(err, ecap, "entry path too long");
        return -1;
    }
    *stack = clamp_stack(pr.stack);
    return 0;
}

static int has_flint_ext(const char *s)
{
    size_t n = strlen(s);
    return n >= 6 && strcmp(s + (n - 6), ".flint") == 0;
}

int has_flint_ext_public(const char *s)
{
    return has_flint_ext(s);
}

int flint_cli_cmd_run(const char *arg)
{
    if (arg != 0 && has_flint_ext(arg))
        return flint_cli_run_file(arg, 65536);

    const char *root = arg ? arg : ".";
    char entry[600], err[256];
    uint32_t stack;
    if (open_project(root, entry, sizeof entry, &stack, err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    return flint_cli_run_file(entry, stack);
}

int flint_cli_cmd_check(const char *arg)
{
    if (arg != 0 && has_flint_ext(arg))
        return flint_cli_check_file(arg);

    const char *root = arg ? arg : ".";
    char entry[600], err[256];
    uint32_t stack;
    if (open_project(root, entry, sizeof entry, &stack, err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    return flint_cli_check_file(entry);
}

/* ---------------- `flint new` ---------------- */

static const char *NEW_MAIN =
    "// %s — FLINT starter project\n"
    "\n"
    "data:\n"
    "hi: str \"hello, world\", 10, 0\n"
    "\n"
    "code:\n"
    "main:\n"
    "    set r2, hi\n"
    ".print:\n"
    "    load.byte r0, [r2]\n"
    "    jump.eq r0, 0, .done\n"
    "    sys 1                    // SYS 1 = putc(r0)\n"
    "    inc r2\n"
    "    jump .print\n"
    ".done:\n"
    "    set r0, 0                // clean exit code\n"
    "    exit\n";

int flint_cli_cmd_new(const char *dirarg)
{
    const char *dir = dirarg ? dirarg : ".";

    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "flint: cannot create %s: %s\n", dir, strerror(errno));
        return 2;
    }
    char sub[560];
    snprintf(sub, sizeof sub, "%.500s/src", dir);
    if (mkdir(sub, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "flint: cannot create %s: %s\n", sub, strerror(errno));
        return 2;
    }

    char toml[600];
    snprintf(toml, sizeof toml, "%.500s/Flint.toml", dir);
    if (fopen(toml, "r") != 0) {
        fprintf(stderr, "flint: %s already exists\n", toml);
        return 1;
    }

    char name[64];
    const char *b = dir + strlen(dir);
    while (b > dir && b[-1] == '/') b--;
    const char *s = b;
    while (s > dir && s[-1] != '/') s--;
    size_t bl = (size_t)(b - s);
    if (bl == 0 || strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
        snprintf(name, sizeof name, "flint-project");
    else
        snprintf(name, sizeof name, "%.*s", (int)(bl > 63 ? 63 : bl), s);

    FILE *f = fopen(toml, "w");
    if (!f) {
        fprintf(stderr, "flint: cannot write %s: %s\n", toml, strerror(errno));
        return 2;
    }
    fprintf(f,
        "[project]\n"
        "name = \"%s\"\n"
        "entry = \"src/main.flint\"\n"
        "stack = 65536\n",
        name);
    fclose(f);

    char entry[600];
    snprintf(entry, sizeof entry, "%.400s/src/main.flint", dir);
    f = fopen(entry, "w");
    if (!f) {
        fprintf(stderr, "flint: cannot write %s: %s\n", entry, strerror(errno));
        return 2;
    }
    fprintf(f, NEW_MAIN, name);
    fclose(f);

    printf("created %s\n", dir);
    printf("  Flint.toml\n");
    printf("  src/main.flint\n");
    printf("next: flint run %s\n", dir);
    return 0;
}
