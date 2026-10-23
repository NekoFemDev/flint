/*
 * flint_cli.c — executable host: syscalls on stdio + file/project front-end.
 * This TU is the "OS" for FLINT programs. Embeddable hosts replace it
 * with their own syscall table and skip the CLI layer entirely.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "flint.h"
#include "flint_cli.h"

/* ---------------- standard syscalls ---------------- */

static int  clif_span(const flint_vm *vm, flint_word addr, flint_word cnt);
static long clif_strn(const flint_vm *vm, flint_word addr);

/* SYS 0 (FLINT_SYS_PRINT_STR): write the raw byte span mem[r1 .. r1+r2).
 * Exactly r2 bytes go out — NULs included — and no register is touched. */
static void std_print_str(flint_vm *vm)
{
    flint_word off = vm->r[1];
    flint_word cnt = vm->r[2];

    if (!clif_span(vm, off, cnt)) return;        /* outside image: no-op */
    if (cnt > 0) fwrite(vm->mem + off, 1, (size_t)cnt, stdout);
    fflush(stdout);                              /* prompts stay visible */
}

/* SYS 1 (FLINT_SYS_PUTC): print the low byte of r0; stdio-buffered */
static void std_putc(flint_vm *vm)
{
    fputc((int)(vm->r[0] & 0xFF), stdout);
}

/* SYS 2 (FLINT_SYS_GETC): read one byte into r0; -1 on EOF */
static void std_getc(flint_vm *vm)
{
    int c;
    fflush(stdout);                     /* pending prompt output first */
    c = fgetc(stdin);
    vm->r[0] = (c == EOF) ? (flint_word)-1 : (flint_word)(unsigned char)c;
}

/* SYS 3 (FLINT_SYS_OPEN): r0 = path address in mem, r1 = flags
 * (0 read / 1 write). Result: r0 = fd or -1. Write mode creates and
 * truncates; the path must be NUL-terminated inside the memory image. */
static void std_open(flint_vm *vm)
{
    int oflags, fd;

    if (clif_strn(vm, vm->r[0]) < 0) { vm->r[0] = -1; return; }
    switch (vm->r[1]) {
    case 0: oflags = O_RDONLY; break;
    case 1: oflags = O_WRONLY | O_CREAT | O_TRUNC; break;
    default: vm->r[0] = -1; return;
    }
    fd = open((const char *)(vm->mem + vm->r[0]), oflags, 0644);
    vm->r[0] = (fd < 0) ? (flint_word)-1 : (flint_word)fd;
}

/* SYS 9 (CLI_SYS_REGS, debug): dump r0..r15, one per line */
static void std_regs(flint_vm *vm)
{
    int i;
    for (i = 0; i < FLINT_NREGS; i++)
        printf("r%d = %lld\n", i, (long long)vm->r[i]);
}

/* ---- file layer: POSIX descriptors, args in r0..r2 ---- */

/* NUL-terminated byte string at mem[addr]; returns length or -1 (runs off) */
static long clif_strn(const flint_vm *vm, flint_word addr)
{
    flint_word i;
    if (addr < 0) return -1;            /* never index before the image */
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

/* SYS 4 (FLINT_SYS_READ): r0 = fd, r1 = buffer in mem, r2 = max bytes.
 * result: r0 = bytes read (0 on EOF) or -1 */
static void std_read(flint_vm *vm)
{
    flint_word fd = vm->r[0];
    flint_word off = vm->r[1];
    flint_word cnt = vm->r[2];
    ssize_t n;

    if (fd < 0 || fd > INT_MAX || !clif_span(vm, off, cnt)) {
        vm->r[0] = -1; return;
    }
    do {
        n = read((int)fd, vm->mem + off, (size_t)cnt);
    } while (n < 0 && errno == EINTR);
    vm->r[0] = (n < 0) ? (flint_word)-1 : (flint_word)n;
}

/* SYS 5 (FLINT_SYS_WRITE): r0 = fd, r1 = buffer in mem, r2 = bytes.
 * result: r0 = bytes written or -1 */
static void std_write(flint_vm *vm)
{
    flint_word fd = vm->r[0];
    flint_word off = vm->r[1];
    flint_word cnt = vm->r[2];
    ssize_t n;

    if (fd < 0 || fd > INT_MAX || !clif_span(vm, off, cnt)) {
        vm->r[0] = -1; return;
    }
    do {
        n = write((int)fd, vm->mem + off, (size_t)cnt);
    } while (n < 0 && errno == EINTR);
    vm->r[0] = (n < 0) ? (flint_word)-1 : (flint_word)n;
}

/* SYS 6 (FLINT_SYS_CLOSE): r0 = fd; result: r0 = 0 or -1 */
static void std_close(flint_vm *vm)
{
    flint_word fd = vm->r[0];
    if (fd < 0 || fd > INT_MAX) { vm->r[0] = -1; return; }
    vm->r[0] = (close((int)fd) == 0) ? 0 : -1;
}

/* SYS 8: time -> r0 = UNIX epoch seconds */
static void std_time(flint_vm *vm)
{
    vm->r[0] = (flint_word)time(NULL);
}

void flint_cli_sysoff(flint_vm *vm)
{
    /* ABI slots 0..6 (flint.h); time/debug extensions 8..9 */
    flint_sys_set(vm, FLINT_SYS_PRINT_STR, std_print_str);
    flint_sys_set(vm, FLINT_SYS_PUTC,      std_putc);
    flint_sys_set(vm, FLINT_SYS_GETC,      std_getc);
    flint_sys_set(vm, FLINT_SYS_OPEN,      std_open);
    flint_sys_set(vm, FLINT_SYS_READ,      std_read);
    flint_sys_set(vm, FLINT_SYS_WRITE,     std_write);
    flint_sys_set(vm, FLINT_SYS_CLOSE,     std_close);
    flint_sys_set(vm, CLI_SYS_TIME,        std_time);
    flint_sys_set(vm, CLI_SYS_REGS,        std_regs);
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

/* ---------------- runtime helpers ---------------- */

/* ---------------- include resolver ---------------- */

/* search order: includer dir, project root, project lib/, cwd variants */
#define INC_SEEN_MAX 64

typedef struct {
    char *seen[INC_SEEN_MAX];
    int   n;
    char *root;                 /* project dir or NULL (single file) */
} inc_ctx;

static void inc_ctx_init(inc_ctx *ic, const char *root)
{
    ic->n = 0; ic->root = 0;
    if (root) {
        size_t n = strlen(root) + 1;
        ic->root = (char *)malloc(n);
        if (ic->root) memcpy(ic->root, root, n);
    }
}

static void inc_ctx_free(inc_ctx *ic)
{
    int i;
    for (i = 0; i < ic->n; i++) free(ic->seen[i]);
    free(ic->root);
}

static int inc_seen(inc_ctx *ic, const char *real)
{
    int i;
    for (i = 0; i < ic->n; i++)
        if (strcmp(ic->seen[i], real) == 0) return 1;
    if (ic->n >= INC_SEEN_MAX) return 0;
    size_t n = strlen(real) + 1;
    ic->seen[ic->n] = (char *)malloc(n);
    if (!ic->seen[ic->n]) return 0;
    memcpy(ic->seen[ic->n], real, n);
    ic->n++;
    return 0;
}

static char *inc_load(void *ud, const char *name, const char *from_dir,
                      char *err, uint32_t errsz)
{
    inc_ctx *ic = (inc_ctx *)ud;
    char cand[1024], real[1024];
    char *dirs[8];              /* search dirs */
    int nd = 0;

    if (name[0] == '/') {       /* absolute: single candidate */
        dirs[nd++] = 0;         /* 0 = use name as-is */
    } else {
        dirs[nd++] = (char *)from_dir;
        if (ic->root)  dirs[nd++] = ic->root;
        if (ic->root)  dirs[nd++] = 0;          /* special: root/lib */
        dirs[nd++] = (char *)".";
        dirs[nd++] = 0;                          /* special: ./lib */
    }

    for (int i = 0; i < nd; i++) {
        if (dirs[i] == 0) {                       /* lib dir candidate */
            if (name[0] == '/') continue;
            if (ic->root) snprintf(cand, sizeof cand, "%.480s/lib/%.480s", ic->root, name);
            else          snprintf(cand, sizeof cand, "lib/%.480s", name);
        } else {
            snprintf(cand, sizeof cand, "%.480s/%.480s", dirs[i], name);
        }
        if (!realpath(cand, real)) continue;
        if (inc_seen(ic, real)) {                 /* already included once */
            char *z = (char *)malloc(1);
            if (z) *z = 0;
            return z;
        }
        long len;
        char *text = read_whole(real, &len);
        if (!text) continue;                      /* unreadable: next dir */
        return text;
    }
    snprintf(err, errsz, "include \"%.200s\": file not found", name);
    return 0;
}

/* run one assembled program; exit code = r0 at HLT, or -(ERR) on trap */
int flint_cli_run_file(const char *path, uint32_t stack_extra,
                       const char *include_root, int use_manual_ram)
{
    long len;
    char *src = read_whole(path, &len);
    if (!src) {
        fprintf(stderr, "flint: %s: %s\n", path, strerror(errno));
        return 2;
    }

    flint_prog p; char err[384] = "";
    inc_ctx ic; inc_ctx_init(&ic, include_root);
    if (flint_assemble_ctx(src, path, inc_load, &ic, &p, stack_extra,
                           err, sizeof(err)) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        free(src);
        inc_ctx_free(&ic);
        return 1;
    }
    free(src);
    inc_ctx_free(&ic);

    flint_vm vm;
    flint_init(&vm, p.code, p.code_size, p.mem, p.mem_size);
    flint_heap_set(&vm, p.heap_start, use_manual_ram);
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

int flint_cli_check_file(const char *path,
                       const char *include_root)
{
    long len;
    char *src = read_whole(path, &len);
    if (!src) {
        fprintf(stderr, "flint: %s: %s\n", path, strerror(errno));
        return 2;
    }

    flint_prog p; char err[384] = "";
    inc_ctx ic; inc_ctx_init(&ic, include_root);
    int rc = flint_assemble_ctx(src, path, inc_load, &ic, &p, 4096,
                                err, sizeof(err));
    free(src);
    inc_ctx_free(&ic);
    if (rc != 0) {
        fprintf(stderr, "flint: %s\n", err);
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
    int  manual_ram;            /* use_hand_made_ram_control; default 1 */
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
    pr->manual_ram = 1;
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
                } else if (strcmp(key, "use_hand_made_ram_control") == 0) {
                    if (strcmp(val, "true") == 0)       pr->manual_ram = 1;
                    else if (strcmp(val, "false") == 0) pr->manual_ram = 0;
                    else {
                        snprintf(err, ecap, "Flint.toml: "
                                 "use_hand_made_ram_control: want true/false, got \"%.64s\"",
                                 val);
                        free(src);
                        return -1;
                    }
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
                        uint32_t *stack, int *manual_ram,
                        char *err, size_t ecap)
{
    flint_proj pr;
    if (proj_load(root, &pr, err, ecap) != 0) return -1;
    int n = snprintf(entry, cap, "%.380s/%.100s", root, pr.entry);
    if (n < 0 || (size_t)n >= cap) {
        snprintf(err, ecap, "entry path too long");
        return -1;
    }
    *stack = clamp_stack(pr.stack);
    if (manual_ram) *manual_ram = pr.manual_ram;
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
        return flint_cli_run_file(arg, 65536, 0, 1);   /* no project file */

    const char *root = arg ? arg : ".";
    char entry[600], err[256];
    uint32_t stack;
    int manual_ram = 1;
    if (open_project(root, entry, sizeof entry, &stack, &manual_ram,
                     err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    return flint_cli_run_file(entry, stack, root, manual_ram);
}

int flint_cli_cmd_check(const char *arg)
{
    if (arg != 0 && has_flint_ext(arg))
        return flint_cli_check_file(arg, 0);

    const char *root = arg ? arg : ".";
    char entry[600], err[256];
    uint32_t stack;
    if (open_project(root, entry, sizeof entry, &stack, 0, err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    return flint_cli_check_file(entry, root);
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
    inc_ctx ic; inc_ctx_init(&ic, 0);
    if (flint_assemble_ctx(src, arg, inc_load, &ic, &p, 4096,
                           err, sizeof(err)) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        free(src);
        inc_ctx_free(&ic);
        return 1;
    }
    free(src);
    inc_ctx_free(&ic);
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
    struct stat st;
    char toml[600];

    /* check before creating anything: a refused `new` must not leave
     * half-scaffolded dirs behind */
    snprintf(toml, sizeof toml, "%.500s/Flint.toml", dir);
    if (stat(toml, &st) == 0) {
        fprintf(stderr, "flint: %s already exists\n", toml);
        return 1;
    }

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
        "stack = 65536\n"
        "use_hand_made_ram_control = true\n",
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
    printf("  src/main.flt\n");
    printf("next: flint run %s\n", dir);
    return 0;
}
