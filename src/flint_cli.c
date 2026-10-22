/*
 * flint_cli.c — executable host: syscalls on stdio + file/project front-end.
 * This TU is the "OS" for FLINT programs. Embeddable hosts replace it
 * with their own syscall table and skip the CLI layer entirely.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
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

/* SYS 3: regs — dump r0..r7, one per line */
static void std_regs(flint_vm *vm)
{
    int i;
    for (i = 0; i < FLINT_NREGS; i++)
        printf("r%d = %lld\n", i, (long long)vm->r[i]);
}

/* ---- file layer: handles are 1-based indexes into a FILE* table ---- */
#define CLIF_MAX 32
static FILE *clif_tab[CLIF_MAX];

/* NUL-terminated byte string at mem[addr]; returns length or -1 (runs off) */
static long clif_strn(const flint_vm *vm, flint_word addr)
{
    flint_word i;
    for (i = addr; i < (flint_word)vm->mem_size; i++)
        if (vm->mem[i] == 0) return i - addr;
    return -1;
}

/* buffer span [addr, addr+cnt) inside vm->mem? 1 ok / 0 bad */
static int clif_span(const flint_vm *vm, flint_word addr, flint_word cnt)
{
    if (addr < 0 || addr > (flint_word)vm->mem_size) return 0;
    if (cnt < 0 || cnt > (flint_word)vm->mem_size - addr) return 0;
    return 1;
}

/* SYS 4: fopen — r1 = mode (0 read / 1 write / 2 append), r2 = path in mem.
 * result: r0 = handle (1-based) or -1 */
static void std_fopen(flint_vm *vm)
{
    const char *mode;
    long plen;
    FILE *f;
    int i;

    switch (vm->r[1]) {
    case 0:  mode = "rb"; break;
    case 1:  mode = "wb"; break;
    case 2:  mode = "ab"; break;
    default: vm->r[0] = -1; return;
    }
    plen = clif_strn(vm, vm->r[2]);
    if (plen < 0 || plen > 4096) { vm->r[0] = -1; return; }

    {
        char path[4097];
        memcpy(path, vm->mem + vm->r[2], (size_t)plen);
        path[plen] = 0;
        f = fopen(path, mode);
    }
    if (!f) { vm->r[0] = -1; return; }
    for (i = 0; i < CLIF_MAX; i++) {
        if (clif_tab[i] == 0) { clif_tab[i] = f; vm->r[0] = i + 1; return; }
    }
    fclose(f);
    vm->r[0] = -1;
}

/* SYS 5: fclose — r1 = handle; r0 = 0 or -1 */
static void std_fclose(flint_vm *vm)
{
    flint_word h = vm->r[1];
    if (h < 1 || h > CLIF_MAX || clif_tab[h - 1] == 0) { vm->r[0] = -1; return; }
    fclose(clif_tab[h - 1]);
    clif_tab[h - 1] = 0;
    vm->r[0] = 0;
}

/* SYS 6: fread — r1 = handle, r2 = buffer in mem, r3 = max bytes.
 * result: r0 = bytes read, 0 on EOF, -1 on error */
static void std_fread(flint_vm *vm)
{
    FILE *f;
    flint_word h = vm->r[1];
    flint_word off = vm->r[2];
    flint_word cnt = vm->r[3];
    size_t n;

    if (h < 1 || h > CLIF_MAX || clif_tab[h - 1] == 0) { vm->r[0] = -1; return; }
    if (!clif_span(vm, off, cnt)) { vm->r[0] = -1; return; }
    f = clif_tab[h - 1];
    n = fread(vm->mem + off, 1, (size_t)cnt, f);
    if (n == 0 && ferror(f)) { vm->r[0] = -1; return; }
    vm->r[0] = (flint_word)n;
}

/* SYS 7: fwrite — r1 = handle, r2 = buffer in mem, r3 = bytes.
 * result: r0 = bytes written or -1 */
static void std_fwrite(flint_vm *vm)
{
    FILE *f;
    flint_word h = vm->r[1];
    flint_word off = vm->r[2];
    flint_word cnt = vm->r[3];
    size_t n;

    if (h < 1 || h > CLIF_MAX || clif_tab[h - 1] == 0) { vm->r[0] = -1; return; }
    if (!clif_span(vm, off, cnt)) { vm->r[0] = -1; return; }
    f = clif_tab[h - 1];
    n = fwrite(vm->mem + off, 1, (size_t)cnt, f);
    vm->r[0] = (n == (size_t)cnt) ? (flint_word)n : (flint_word)-1;
}

/* SYS 8: time -> r0 = UNIX epoch seconds */
static void std_time(flint_vm *vm)
{
    vm->r[0] = (flint_word)time(NULL);
}

void flint_cli_sysoff(flint_vm *vm)
{
    flint_sys_set(vm, 0, std_prn);
    flint_sys_set(vm, 1, std_putc);
    flint_sys_set(vm, 2, std_getc);
    flint_sys_set(vm, 3, std_regs);
    flint_sys_set(vm, 4, std_fopen);
    flint_sys_set(vm, 5, std_fclose);
    flint_sys_set(vm, 6, std_fread);
    flint_sys_set(vm, 7, std_fwrite);
    flint_sys_set(vm, 8, std_time);
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
    snprintf(pr->entry, sizeof pr->entry, "src/main.flt");

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
    return n >= 4 && strcmp(s + (n - 4), ".flt") == 0;
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

int flint_cli_cmd_dis(const char *arg)
{
    if (arg == 0 || !has_flint_ext(arg)) {
        fprintf(stderr, "flint dis: <file.flt> required\n");
        return 2;
    }

    long len;
    char *src = read_whole(arg, &len);
    if (!src) {
        fprintf(stderr, "flint: %s: %s\n", arg, strerror(errno));
        return 2;
    }
    flint_prog p; char err[384] = "";
    if (flint_assemble(src, &p, 4096, err, sizeof(err)) != 0) {
        fprintf(stderr, "flint: %s: %s\n", arg, err);
        free(src);
        return 1;
    }
    free(src);
    printf("; %u code bytes, entry 0x%04X\n", p.code_size, p.entry);
    int rc = flint_dis(p.code, p.code_size);
    flint_prog_free(&p);
    return rc;
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
        "entry = \"src/main.flt\"\n"
        "stack = 65536\n",
        name);
    fclose(f);

    char entry[600];
    snprintf(entry, sizeof entry, "%.400s/src/main.flt", dir);
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
