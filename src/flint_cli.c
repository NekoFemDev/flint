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
#include "flint_img.h"

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

/* init + run an assembled image; exit code = r0 at HLT, or -(ERR) on
 * trap. Does not free p — the caller owns the images. */
static int run_prog(flint_prog *p, flint_mem_mode mem)
{
    flint_vm vm;
    flint_init(&vm, p->code, p->code_size, p->mem, p->mem_size);
    flint_heap_set(&vm, p->heap_start, mem);
    flint_cli_sysoff(&vm);
    flint_word rc = flint_run(&vm, p->entry);

    if (vm.err == FLINT_OK) return (int)(rc & 0x7FFFFFFF);
    fprintf(stderr, "flint: runtime: %s\n", vm.err_text);
    return (int)(-(int64_t)vm.err);
}

/* assemble one source file (includes resolved by the host) and run it */
int flint_cli_run_file(const char *path, uint32_t stack_extra,
                       const char *include_root, flint_mem_mode mem)
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

    int code = run_prog(&p, mem);
    flint_prog_free(&p);
    return code;
}

/* load a built .fbc image and run it — no assembler involved */
int flint_cli_run_image(const char *path)
{
    flint_prog p; flint_mem_mode mem = FLINT_MEM_MANUAL; char err[384] = "";

    if (flint_img_load(path, &p, &mem, err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    int code = run_prog(&p, mem);
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

/* ---- memory model: text <-> flint_mem_mode ---- */

const char *flint_mem_mode_name(flint_mem_mode mode)
{
    return (mode == FLINT_MEM_AUTO) ? "auto" : "manual";
}

/* see flint_cli.h for the contract */
int flint_mem_mode_parse(const char *s, flint_mem_mode *out)
{
    char word[8];
    size_t n = 0, i;

    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    while (s[n] != 0 && s[n] != ' ' && s[n] != '\t' &&
           s[n] != '\n' && s[n] != '\r') {
        if (n + 1 >= sizeof word) return -1;       /* word too long */
        word[n] = (char)((s[n] >= 'A' && s[n] <= 'Z')
                         ? (s[n] + ('a' - 'A')) : s[n]);
        n++;
    }
    i = n;                                  /* only blanks may follow */
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] != 0 && s[i] != '\n' && s[i] != '\r') return -1;
    word[n] = 0;

    if (strcmp(word, "manual") == 0) {
        if (out) *out = FLINT_MEM_MANUAL;
        return 0;
    }
    if (strcmp(word, "auto") == 0) {
        if (out) *out = FLINT_MEM_AUTO;
        return 0;
    }
    return -1;
}

typedef struct {
    char name[64];
    char entry[128];
    long stack;                 /* stack extra bytes; default 65536 */
    flint_mem_mode mem;         /* Flint.toml `memory`; default manual */
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
    pr->mem = FLINT_MEM_MANUAL;
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
                } else if (strcmp(key, "memory") == 0) {
                    if (flint_mem_mode_parse(val, &pr->mem) != 0) {
                        snprintf(err, ecap, "Flint.toml: memory: want "
                                 "\"auto\" or \"manual\", got \"%.64s\"",
                                 val);
                        free(src);
                        return -1;
                    }
                } else if (strcmp(key, "use_hand_made_ram_control") == 0) {
                    /* legacy v0.1 key (deprecated): true = manual control,
                     * false = auto. If both this and `memory` appear, the
                     * later line wins (single pass, file order). */
                    if (strcmp(val, "true") == 0)
                        pr->mem = FLINT_MEM_MANUAL;
                    else if (strcmp(val, "false") == 0)
                        pr->mem = FLINT_MEM_AUTO;
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

/* resolve project root -> entry path + stack + memory model */
static int open_project(const char *root, char *entry, size_t cap,
                        uint32_t *stack, flint_mem_mode *mem,
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
    if (mem) *mem = pr.mem;
    return 0;
}

static int has_ext(const char *s, const char *ext4)
{
    size_t n = strlen(s);
    return n >= 4 && strcmp(s + (n - 4), ext4) == 0;
}

static int has_flint_ext(const char *s) { return has_ext(s, ".flt"); }
static int has_fbc_ext(const char *s)   { return has_ext(s, ".fbc"); }
static int is_flint_file(const char *s) { return has_flint_ext(s) ||
                                                 has_fbc_ext(s); }

int has_flint_ext_public(const char *s) { return has_flint_ext(s); }
int has_fbc_ext_public(const char *s)   { return has_fbc_ext(s); }

int flint_cli_cmd_run(const char *arg)
{
    if (arg != 0 && has_fbc_ext(arg))
        return flint_cli_run_image(arg);
    if (arg != 0 && has_flint_ext(arg))
        return flint_cli_run_file(arg, 65536, 0, FLINT_MEM_MANUAL);

    const char *root = arg ? arg : ".";
    char entry[600], err[256];
    uint32_t stack;
    flint_mem_mode mem = FLINT_MEM_MANUAL;
    if (open_project(root, entry, sizeof entry, &stack, &mem,
                     err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    return flint_cli_run_file(entry, stack, root, mem);
}

int flint_cli_cmd_check(const char *arg)
{
    if (arg != 0 && has_fbc_ext(arg)) {
        struct stat st;
        flint_prog p; flint_mem_mode mem = FLINT_MEM_MANUAL;
        char err[384] = "";

        if (stat(arg, &st) != 0) {
            fprintf(stderr, "flint: %s: %s\n", arg, strerror(errno));
            return 2;
        }
        if (flint_img_load(arg, &p, &mem, err, sizeof err) != 0) {
            fprintf(stderr, "flint: %s\n", err);
            return 1;
        }
        printf("flint: %s: ok, %u code bytes, %u mem bytes (memory = %s)\n",
               arg, p.code_size, p.mem_size, flint_mem_mode_name(mem));
        flint_prog_free(&p);
        return 0;
    }
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
    if (arg == 0 || !is_flint_file(arg)) {
        fprintf(stderr, "flint dis: <file.flt|file.fbc> required\n");
        return 2;
    }

    if (has_fbc_ext(arg)) {
        flint_prog p; flint_mem_mode mem = FLINT_MEM_MANUAL;
        char err[384] = "";
        if (flint_img_load(arg, &p, &mem, err, sizeof err) != 0) {
            fprintf(stderr, "flint: %s\n", err);
            return 1;
        }
        printf("; %u code bytes, entry 0x%04X\n", p.code_size, p.entry);
        int rc = flint_dis(p.code, p.code_size);
        flint_prog_free(&p);
        return rc;
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

/* ---------------- name helpers ---------------- */

/* leaf name of a path (trailing slashes ignored); "." / ".." / empty
 * collapse to "flint-project" */
static void leaf_name(const char *path, char *out, size_t cap)
{
    const char *b = path + strlen(path);
    const char *s;
    size_t bl;

    while (b > path && b[-1] == '/') b--;
    s = b;
    while (s > path && s[-1] != '/') s--;
    bl = (size_t)(b - s);
    if (bl == 0 || (bl == 1 && s[0] == '.') ||
        (bl == 2 && s[0] == '.' && s[1] == '.'))
        snprintf(out, cap, "flint-project");
    else
        snprintf(out, cap, "%.*s",
                 (int)(bl > cap - 1 ? cap - 1 : bl), s);
}

/* leaf name with a trailing .flt/.fbc dropped */
static void source_stem(const char *path, char *out, size_t cap)
{
    size_t n;

    leaf_name(path, out, cap);
    n = strlen(out);
    if (n > 4 && (strcmp(out + n - 4, ".flt") == 0 ||
                  strcmp(out + n - 4, ".fbc") == 0))
        out[n - 4] = 0;
}

/* ---------------- `flint build` ---------------- */

/* assemble <file.flt|dir> into one self-contained .fbc image; includes
 * are resolved here, so the image needs no sources at run time */
int flint_cli_cmd_build(const char *arg, const char *outarg)
{
    int from_file = (arg != 0 && is_flint_file(arg));
    const char *root = arg ? arg : ".";
    char src_path[600], name[128], out[700];
    uint32_t stack = 65536;
    flint_mem_mode mem = FLINT_MEM_MANUAL;

    if (arg != 0 && has_fbc_ext(arg)) {
        fprintf(stderr, "flint build: %s is already a bytecode image\n", arg);
        return 2;
    }

    if (from_file) {
        if (snprintf(src_path, sizeof src_path, "%s", arg) >=
            (int)sizeof src_path) {
            fprintf(stderr, "flint build: path too long\n");
            return 2;
        }
        source_stem(arg, name, sizeof name);
    } else {
        flint_proj pr;
        char err[256];
        if (proj_load(root, &pr, err, sizeof err) != 0) {
            fprintf(stderr, "flint: %s\n", err);
            return 2;
        }
        if (snprintf(src_path, sizeof src_path, "%.380s/%.100s",
                     root, pr.entry) >= (int)sizeof src_path) {
            fprintf(stderr, "flint build: entry path too long\n");
            return 2;
        }
        stack = clamp_stack(pr.stack);
        mem = pr.mem;
        if (pr.name[0] != 0)
            snprintf(name, sizeof name, "%.100s", pr.name);
        else
            leaf_name(root, name, sizeof name);
    }

    long len;
    char *src = read_whole(src_path, &len);
    if (!src) {
        fprintf(stderr, "flint: %s: %s\n", src_path, strerror(errno));
        return 2;
    }

    flint_prog p; char err[384] = "";
    inc_ctx ic; inc_ctx_init(&ic, from_file ? 0 : root);
    int rc = flint_assemble_ctx(src, src_path, inc_load, &ic, &p, stack,
                                err, sizeof(err));
    free(src);
    inc_ctx_free(&ic);
    if (rc != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 1;
    }

    if (outarg != 0) {
        if (snprintf(out, sizeof out, "%s", outarg) >= (int)sizeof out) {
            fprintf(stderr, "flint build: output path too long\n");
            flint_prog_free(&p);
            return 2;
        }
    } else {
        if (mkdir("build", 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "flint build: cannot create build/: %s\n",
                    strerror(errno));
            flint_prog_free(&p);
            return 2;
        }
        if (snprintf(out, sizeof out, "build/%.100s.fbc", name) >=
            (int)sizeof out) {
            fprintf(stderr, "flint build: output path too long\n");
            flint_prog_free(&p);
            return 2;
        }
    }

    if (flint_img_save(out, &p, mem, err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        flint_prog_free(&p);
        return 2;
    }
    printf("flint: built %s (%u code bytes, %u mem bytes, memory = %s)\n",
           out, p.code_size, p.mem_size, flint_mem_mode_name(mem));
    flint_prog_free(&p);
    return 0;
}

/* ---------------- `flint new` ---------------- */

/* starter programs, one per memory model (Flint.toml `memory`):
 *   manual — the program takes its own blocks with `alloc`
 *   auto   — locals live in `enter`/`leave` frames
 * Both print "hello, world" and exit 0. */

static const char *NEW_MAIN_MANUAL =
    "// %s — FLINT starter project (memory = \"manual\")\n"
    "//\n"
    "// Manual memory: `alloc` takes a block from the bump arena and the\n"
    "// program owns it — later `return`s never invalidate the block.\n"
    "\n"
    "data:\n"
    "msg: str \"hello, world\", 10, 0\n"
    "\n"
    "code:\n"
    "main:\n"
    "    alloc r1, 16             // scratch block; r1 = its address\n"
    "    set r2, msg              // r2 walks the message\n"
    "    copy r3, r1              // r3 walks the block\n"
    ".copy:\n"
    "    load.byte r4, [r2]\n"
    "    store.byte [r3], r4\n"
    "    jump.eq r4, .print       // NUL copied: done\n"
    "    inc r2\n"
    "    inc r3\n"
    "    jump .copy\n"
    ".print:\n"
    "    set r2, 13               // message + newline (the NUL stays)\n"
    "    sys 0                    // print span [r1, r1+r2)\n"
    "    set r0, 0\n"
    "    exit\n";

static const char *NEW_MAIN_AUTO =
    "// %s — FLINT starter project (memory = \"auto\")\n"
    "//\n"
    "// Automatic memory: locals live in `enter`/`leave` frames — the\n"
    "// frame is reserved on entry and released on `leave`.\n"
    "\n"
    "data:\n"
    "msg: str \"hello, world\", 10, 0\n"
    "\n"
    "code:\n"
    "main:\n"
    "    set r1, 4\n"
    "    set r2, 9\n"
    "    call add_two             // r0 = 13; args r1/r2, result r0\n"
    "    set r1, msg\n"
    "    copy r2, r0              // print length = message + newline\n"
    "    sys 0                    // print span [r1, r1+r2)\n"
    "    set r0, 0\n"
    "    exit\n"
    "\n"
    "add_two:\n"
    "    enter 16                 // local slots [fp-8] and [fp-16]\n"
    "    store [fp-8], r1\n"
    "    store [fp-16], r2\n"
    "    load r0, [fp-8]\n"
    "    load r3, [fp-16]\n"
    "    add r0, r0, r3\n"
    "    leave                    // frame released here\n"
    "    return\n";

/* ask for the memory model on stdin; EOF or a blank line takes the
 * default (manual), anything else that is not auto/manual re-asks */
static flint_mem_mode prompt_memory_mode(void)
{
    char buf[16];

    for (;;) {
        flint_mem_mode m;
        int i = 0;

        printf("Choose memory model [auto/manual] (default: manual): ");
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) return FLINT_MEM_MANUAL;
        while (buf[i] == ' ' || buf[i] == '\t') i++;
        if (buf[i] == 0 || buf[i] == '\n' || buf[i] == '\r')
            return FLINT_MEM_MANUAL;
        if (flint_mem_mode_parse(buf, &m) == 0) return m;
        printf("please answer 'auto' or 'manual'\n");
    }
}

int flint_cli_cmd_new(const char *dirarg, flint_mem_mode mem, int mem_given)
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

    /* no --memory: ask before any directory exists on disk */
    if (!mem_given) mem = prompt_memory_mode();

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
    leaf_name(dir, name, sizeof name);

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
        "memory = \"%s\"\n",
        name, flint_mem_mode_name(mem));
    fclose(f);

    char entry[600];
    snprintf(entry, sizeof entry, "%.400s/src/main.flt", dir);
    f = fopen(entry, "w");
    if (!f) {
        fprintf(stderr, "flint: cannot write %s: %s\n", entry, strerror(errno));
        return 2;
    }
    fprintf(f, mem == FLINT_MEM_AUTO ? NEW_MAIN_AUTO : NEW_MAIN_MANUAL, name);
    fclose(f);

    printf("created %s\n", dir);
    printf("  Flint.toml      (memory = \"%s\")\n", flint_mem_mode_name(mem));
    printf("  src/main.flt\n");
    printf("next: flint run %s\n", dir);
    printf("  or: flint build %s\n", dir);
    return 0;
}
