/*
 * flint_cli.c — executable host: syscalls on stdio + file/project front-end.
 * This TU is the "OS" for FLINT programs. Embeddable hosts replace it
 * with their own syscall table and skip the CLI layer entirely.
 */

/* realpath() is POSIX, but glibc guards its declaration by feature-test
 * macros: through 2.41 it was __USE_XOPEN2K8 (POSIX 2008), since 2.42 it
 * is __USE_MISC or __USE_XOPEN_EXTENDED. _XOPEN_SOURCE 700 satisfies both
 * (and still implies POSIX.1-2008); without a macro GCC 14+ turns the
 * implicit declaration into a hard error. */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "flint.h"
#include "flint_cli.h"
#include "flint_img.h"
#include "flint_stub.h"

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

/* one node per included file in `flint build`'s status output; children
 * keep the order the resolver opened them (the DFS order of the source) */
typedef struct DepNode {
    char *file;                 /* display path (root- or cwd-relative) */
    struct DepNode **kids;
    int nkids, cap;
} DepNode;

typedef struct {
    char *seen[INC_SEEN_MAX];
    int   n;
    char *root;                 /* project dir or NULL (single file) */

    /* dependency tree: NULL = off — only `flint build` turns it on */
    DepNode *tree;              /* entry-point node */
    char *root_real;            /* realpath(root): display-path base */
    char *cwd_real;             /* realpath("."): display-path base */
    struct {                    /* one slot per opened file: the name it
                                   was opened under -> its tree node */
        char    *name;
        DepNode *node;
    } open[INC_SEEN_MAX];
    int nopen;
} inc_ctx;

static char *dep_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* p relative to base at a path boundary, else NULL (base may be NULL) */
static const char *dep_rel(const char *p, const char *base)
{
    size_t n;
    if (!base) return 0;
    n = strlen(base);
    if (n == 0 || strncmp(p, base, n) != 0) return 0;
    if (n == 1 && base[0] == '/') return p + 1;
    return (p[n] == '/') ? p + n + 1 : 0;
}

/* display path for a resolved file: project-root-relative first, then
 * cwd-relative, then the absolute path */
static char *dep_display(const inc_ctx *ic, const char *real)
{
    const char *t = dep_rel(real, ic->root_real);
    if (t && *t) return dep_strdup(t);
    t = dep_rel(real, ic->cwd_real);
    if (t && *t) return dep_strdup(t);
    return dep_strdup(real);
}

/* node owning `file` (freed again on OOM, so callers need no cleanup) */
static DepNode *dep_new(char *file)
{
    DepNode *n;
    if (!file) return 0;
    n = (DepNode *)calloc(1, sizeof *n);
    if (!n) { free(file); return 0; }
    n->file = file;
    return n;
}

static void dep_free(DepNode *n)
{
    int i;
    if (!n) return;
    for (i = 0; i < n->nkids; i++) dep_free(n->kids[i]);
    free(n->kids);
    free(n->file);
    free(n);
}

/* parent->kids += n; 0 = ok, -1 = out of memory (n untouched) */
static int dep_link(DepNode *parent, DepNode *n)
{
    if (parent->nkids == parent->cap) {
        int nc = parent->cap ? parent->cap * 2 : 4;
        DepNode **nk = (DepNode **)realloc(parent->kids,
                                           sizeof *nk * (size_t)nc);
        if (!nk) return -1;
        parent->kids = nk;
        parent->cap = nc;
    }
    parent->kids[parent->nkids++] = n;
    return 0;
}

/* tree node of an include's owner: the file whose frame is live. The
 * assembler processes a new file depth-first before returning to its
 * includer, so the most recently opened file carrying `from_name` is
 * exactly the innermost matching frame. */
static DepNode *dep_parent(const inc_ctx *ic, const char *from_name)
{
    int i;
    for (i = ic->nopen - 1; i >= 0; i--)
        if (strcmp(ic->open[i].name, from_name) == 0)
            return ic->open[i].node;
    return ic->tree;                    /* unknown owner: root */
}

static void inc_ctx_init(inc_ctx *ic, const char *root)
{
    ic->n = 0; ic->root = 0;
    ic->tree = 0; ic->root_real = 0; ic->cwd_real = 0; ic->nopen = 0;
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
    for (i = 0; i < ic->nopen; i++) free(ic->open[i].name);
    dep_free(ic->tree);
    free(ic->root);
    free(ic->root_real);
    free(ic->cwd_real);
}

/* start dependency tracking (`flint build`); entry = the file about to
 * be assembled. Without this call the resolver behaves exactly as
 * before — run/check/dis never record a tree. */
static void inc_dep_begin(inc_ctx *ic, const char *entry)
{
    char *real, *disp, *key;

    ic->root_real = ic->root ? realpath(ic->root, NULL) : 0;
    ic->cwd_real  = realpath(".", NULL);

    real = realpath(entry, NULL);
    disp = real ? dep_display(ic, real) : dep_strdup(entry);
    free(real);
    ic->tree = dep_new(disp);
    if (!ic->tree) return;

    key = dep_strdup(entry);
    if (key) {                          /* the entry's own include site */
        ic->open[0].name = key;
        ic->open[0].node = ic->tree;
        ic->nopen = 1;
    }
}

/* record one opened include: an edge from the includer to `real` */
static void inc_dep_note(inc_ctx *ic, const char *name, const char *from_name,
                         const char *real)
{
    DepNode *parent, *n;
    char *key;

    if (!ic->tree) return;
    parent = dep_parent(ic, from_name);
    n = dep_new(dep_display(ic, real));
    if (!n) return;
    if (!parent || dep_link(parent, n) != 0) { dep_free(n); return; }
    if (ic->nopen < INC_SEEN_MAX) {
        key = dep_strdup(name);
        if (key) {
            ic->open[ic->nopen].name = key;
            ic->open[ic->nopen].node = n;
            ic->nopen++;
        }
    }
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
                      const char *from_name, char *err, uint32_t errsz)
{
    inc_ctx *ic = (inc_ctx *)ud;
    char cand[1024];
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
        /* realpath(path, NULL) mallocs the result. Passing a caller
         * buffer instead requires PATH_MAX + 1 bytes — glibc's fortify
         * wrapper (__realpath_chk, _FORTIFY_SOURCE builds) aborts the
         * process for anything smaller. */
        char *real = realpath(cand, NULL);
        if (!real) continue;
        if (inc_seen(ic, real)) {                 /* already included once */
            char *z = (char *)malloc(1);
            if (z) *z = 0;
            free(real);
            return z;
        }
        long len;
        char *text = read_whole(real, &len);
        if (!text) {                              /* unreadable: next dir */
            free(real);
            continue;
        }
        inc_dep_note(ic, name, from_name, real);  /* tree edge for `build` */
        free(real);
        return text;
    }
    snprintf(err, errsz, "include \"%.200s\": file not found", name);
    return 0;
}

/* stderr report for --gc-stats / FLINT_GC_STATS=1 */
static void print_gc_stats(const flint_vm *vm)
{
    flint_gc_stats gs;
    flint_gc_get_stats(vm, &gs);
    if (!gs.enabled) {
        fprintf(stderr, "flint: gc: manual mode (no collector)\n");
        return;
    }
    fprintf(stderr,
            "flint: gc: collections=%u live=%u free=%u alloc=%u "
            "reclaimed=%u threshold=%u%s\n",
            gs.collections, gs.bytes_live, gs.bytes_free, gs.bytes_alloced,
            gs.bytes_reclaimed, gs.threshold, gs.stress ? " stress" : "");
    if (gs.overflow_rescans)
        fprintf(stderr, "flint: gc: overflow rescans=%u\n",
                gs.overflow_rescans);
}

/* init + run an assembled image; exit code = r0 at HLT, or -(ERR) on
 * trap. Does not free p — the caller owns the images. */
static int run_prog(flint_prog *p, flint_mem_mode mem,
                    const flint_run_opts *o)
{
    flint_vm vm;
    int stress = o && o->gc_stress;
    int stats  = o && o->gc_stats;
    const char *e;
    flint_word rc;

    e = getenv("FLINT_GC_STRESS");
    if (!stress && e && e[0] && e[0] != '0') stress = 1;
    e = getenv("FLINT_GC_STATS");
    if (!stats && e && e[0] && e[0] != '0') stats = 1;

    flint_init(&vm, p->code, p->code_size, p->mem, p->mem_size);
    flint_heap_set(&vm, p->heap_start, mem);
    if (stress) flint_gc_set_stress(&vm, 1);
    flint_cli_sysoff(&vm);
    rc = flint_run(&vm, p->entry);

    if (stats) print_gc_stats(&vm);
    if (vm.err == FLINT_OK) return (int)(rc & 0x7FFFFFFF);
    fprintf(stderr, "flint: runtime: %s\n", vm.err_text);
    return (int)(-(int64_t)vm.err);
}

/* assemble one source file (includes resolved by the host) and run it */
static int run_file_opts(const char *path, uint32_t stack_extra,
                         const char *include_root, flint_mem_mode mem,
                         const flint_run_opts *o)
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

    int code = run_prog(&p, mem, o);
    flint_prog_free(&p);
    return code;
}

int flint_cli_run_file(const char *path, uint32_t stack_extra,
                       const char *include_root, flint_mem_mode mem)
{
    return run_file_opts(path, stack_extra, include_root, mem, 0);
}

/* load a built .fbc image and run it — no assembler involved */
static int run_image_opts(const char *path, const flint_run_opts *o)
{
    flint_prog p; flint_mem_mode mem = FLINT_MEM_MANUAL; char err[384] = "";

    if (flint_img_load(path, &p, &mem, err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    if (o && o->memory_given) mem = o->memory;
    int code = run_prog(&p, mem, o);
    flint_prog_free(&p);
    return code;
}

int flint_cli_run_image(const char *path)
{
    return run_image_opts(path, 0);
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
        snprintf(err, ecap, "cannot read %.200s (not a project dir?)", path);
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

int flint_cli_cmd_run_opts(const char *arg, const flint_run_opts *o)
{
    if (arg != 0 && has_fbc_ext(arg))
        return run_image_opts(arg, o);
    if (arg != 0 && has_flint_ext(arg)) {
        flint_mem_mode mem = FLINT_MEM_MANUAL;
        if (o && o->memory_given) mem = o->memory;
        return run_file_opts(arg, 65536, 0, mem, o);
    }

    const char *root = arg ? arg : ".";
    char entry[600], err[256];
    uint32_t stack;
    flint_mem_mode mem = FLINT_MEM_MANUAL;
    if (open_project(root, entry, sizeof entry, &stack, &mem,
                     err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        return 2;
    }
    if (o && o->memory_given) mem = o->memory;
    return run_file_opts(entry, stack, root, mem, o);
}

int flint_cli_cmd_run(const char *arg)
{
    return flint_cli_cmd_run_opts(arg, 0);
}

/* ---------------- host-level parallelism ---------------- */

/* One worker per OS thread. Every worker reads (or assembles) its own
 * copy of the program and builds a private flint_vm — the VM has no
 * global state, so code, memory and stack are isolated; the only thing
 * shared is stdout/stderr (and thus interleaved output). */
typedef struct {
    const char *path;
    uint32_t    stack;      /* assembler stack reserve for .flt sources */
    int         rc;         /* r0 at HLT (0 = clean)                    */
    int         trap;       /* FLINT_ERR_* code, 0 = none               */
    int         bad;        /* 1 = file/assemble failure                */
    char        err[384];   /* failure text (reported for the first one) */
} flint_pworker;

static void *flint_pworker_main(void *ud)
{
    flint_pworker *w = (flint_pworker *)ud;
    flint_prog p;
    flint_mem_mode mem = FLINT_MEM_MANUAL;
    flint_vm vm;
    flint_word rc;

    if (has_fbc_ext(w->path)) {
        if (flint_img_load(w->path, &p, &mem, w->err, sizeof w->err) != 0) {
            w->bad = 1;
            return 0;
        }
    } else {
        long len;
        char *src = read_whole(w->path, &len);
        inc_ctx ic;
        if (!src) {
            snprintf(w->err, sizeof w->err, "%s: %s",
                     w->path, strerror(errno));
            w->bad = 1;
            return 0;
        }
        inc_ctx_init(&ic, 0);
        if (flint_assemble_ctx(src, w->path, inc_load, &ic, &p, w->stack,
                               w->err, sizeof w->err) != 0) {
            free(src);
            inc_ctx_free(&ic);
            w->bad = 1;
            return 0;
        }
        free(src);
        inc_ctx_free(&ic);
    }

    flint_init(&vm, p.code, p.code_size, p.mem, p.mem_size);
    flint_heap_set(&vm, p.heap_start, mem);
    flint_cli_sysoff(&vm);
    rc = flint_run(&vm, p.entry);

    if (vm.err != FLINT_OK) {
        w->trap = vm.err;
        snprintf(w->err, sizeof w->err, "runtime: %s",
                 vm.err_text ? vm.err_text : flint_errno(vm.err));
    } else {
        w->rc = (int)(rc & 0x7FFFFFFF);
    }
    flint_prog_free(&p);
    return 0;
}

/* see flint_cli.h for the contract */
int flint_run_parallel(const char *filename, int num_threads,
                       uint32_t stack_size)
{
    pthread_t *tid;
    flint_pworker *w;
    int i, started = 0, rc = 0, first_err = -1;

    if (!filename) {
        fprintf(stderr, "flint run: --threads needs a file\n");
        return 2;
    }
    if (num_threads < 1 || num_threads > 1024) {
        fprintf(stderr, "flint run: --threads wants a count in 1..1024\n");
        return 2;
    }
    if (!has_flint_ext(filename) && !has_fbc_ext(filename)) {
        fprintf(stderr,
                "flint run: --threads: %s: not a .flt or .fbc file\n",
                filename);
        return 2;
    }

    tid = (pthread_t *)malloc(sizeof *tid * (size_t)num_threads);
    w = (flint_pworker *)calloc((size_t)num_threads, sizeof *w);
    if (!tid || !w) {
        free(tid);
        free(w);
        fprintf(stderr, "flint run: out of memory\n");
        return 2;
    }

    for (i = 0; i < num_threads; i++) {
        w[i].path = filename;
        w[i].stack = clamp_stack((long)stack_size);
        if (pthread_create(&tid[i], 0, flint_pworker_main, &w[i]) != 0) {
            fprintf(stderr, "flint run: pthread_create failed\n");
            rc = 2;
            break;
        }
        started++;
    }

    for (i = 0; i < started; i++) pthread_join(tid[i], 0);

    if (rc == 0 && started == num_threads) {
        for (i = 0; i < started; i++)
            if (w[i].bad) { first_err = i; rc = 1; break; }
        if (rc == 0)
            for (i = 0; i < started; i++)
                if (w[i].trap) { first_err = i; rc = -w[i].trap; break; }
        if (rc == 0)
            for (i = 0; i < started; i++)
                if (w[i].rc != 0) { rc = w[i].rc; break; }
    }
    if (first_err >= 0)
        fprintf(stderr, "flint: %s\n", w[first_err].err);

    free(tid);
    free(w);
    return rc;
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

/* `--elf` drops the fused standalone executable in target/ (same
 * "relative to the cwd" convention as build/); the stub it copies comes
 * from build/stub.elf — see include/flint_stub.h for the fused layout */
#define ELF_OUTPUT_DIR "target"

/* little-endian u32 encode (trailer file format; flint_stub.h) */
static void elf_put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* append a whole file to an open writer; 0 = ok, -1 with err set */
static int append_file(FILE *out, const char *path, char *err, uint32_t errsz)
{
    unsigned char buf[65536];
    FILE *in = fopen(path, "rb");
    size_t n;

    if (!in) {
        snprintf(err, errsz, "%s: cannot read (%s)", path, strerror(errno));
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            snprintf(err, errsz, "write failed");
            fclose(in);
            return -1;
        }
    }
    if (ferror(in)) {
        snprintf(err, errsz, "%s: read failed", path);
        fclose(in);
        return -1;
    }
    fclose(in);
    return 0;
}

/* fuse build/stub.elf + code + mem + trailer into `out`, then chmod it
 * executable; 0 = ok, -1 with err set (partial output removed) */
static int build_elf(const char *out, const flint_prog *p, flint_mem_mode mem,
                     char *err, uint32_t errsz)
{
    unsigned char tr[sizeof(struct FlintPayload)];
    FILE *f;
    int ok = 1;

    f = fopen(out, "wb");
    if (!f) {
        snprintf(err, errsz, "%.300s: cannot write (%s)", out, strerror(errno));
        return -1;
    }

    if (append_file(f, FLINT_STUB_PATH, err, errsz) != 0) ok = 0;
    if (ok && fwrite(p->code, 1, p->code_size, f) != p->code_size) {
        snprintf(err, errsz, "%.300s: write failed", out);
        ok = 0;
    }
    if (ok && fwrite(p->mem, 1, p->mem_size, f) != p->mem_size) {
        snprintf(err, errsz, "%.300s: write failed", out);
        ok = 0;
    }
    if (ok) {
        elf_put_u32(tr + 0,  FLINT_PAYLOAD_MAGIC);
        elf_put_u32(tr + 4,  p->code_size);
        elf_put_u32(tr + 8,  p->mem_size);
        elf_put_u32(tr + 12, p->entry);
        elf_put_u32(tr + 16, p->heap_start);
        elf_put_u32(tr + 20, (uint32_t)mem);
        if (fwrite(tr, 1, sizeof tr, f) != sizeof tr) {
            snprintf(err, errsz, "%.300s: write failed", out);
            ok = 0;
        }
    }
    if (fclose(f) != 0) {
        if (ok) snprintf(err, errsz, "%.300s: write failed", out);
        ok = 0;
    }
    if (ok && chmod(out, 0755) != 0) {
        snprintf(err, errsz, "%.300s: cannot chmod 0755 (%s)", out,
                 strerror(errno));
        ok = 0;
    }

    if (!ok) {
        remove(out);                 /* no half-written executables */
        return -1;
    }
    return 0;
}

/* ---- cargo-style status output ---- */

#define CLR_RESET     "\033[0m"
#define CLR_BOLD_CYAN "\033[1;36m"
#define CLR_GREEN     "\033[32m"

/* colors only when stdout is a terminal; NO_COLOR opts out */
static int use_color(void)
{
    return isatty(STDOUT_FILENO) && getenv("NO_COLOR") == 0;
}

/* include tree, one file per line: `prefix` is the indentation at this
 * level, `is_last` picks └── over ├──. A negative `is_last` marks the
 * entry point, printed without a connector of its own. */
static void print_dependency_tree(const DepNode *node, const char *prefix,
                                  int is_last)
{
    int i;

    if (!node) return;
    if (is_last < 0) {                          /* entry point */
        printf("%s%s\n", prefix, node->file);
        for (i = 0; i < node->nkids; i++)
            print_dependency_tree(node->kids[i], prefix,
                                  i == node->nkids - 1);
        return;
    }
    printf("%s%s %s\n", prefix, is_last ? "└──" : "├──", node->file);
    if (node->nkids > 0) {
        char sub[512];
        snprintf(sub, sizeof sub, "%s%s", prefix,
                 is_last ? "    " : "│   ");
        for (i = 0; i < node->nkids; i++)
            print_dependency_tree(node->kids[i], sub,
                                  i == node->nkids - 1);
    }
}

/* assemble <file.flt|dir> into one self-contained .fbc image; includes
 * are resolved here, so the image needs no sources at run time. With
 * elf != 0 the output is build/stub.elf fused with the program instead:
 * one executable, no flint host needed at run time. */
int flint_cli_cmd_build(const char *arg, const char *outarg, int elf)
{
    int from_file = (arg != 0 && is_flint_file(arg));
    const char *root = arg ? arg : ".";
    char src_path[600], name[128], out[700];
    uint32_t stack = 65536;
    flint_mem_mode mem = FLINT_MEM_MANUAL;
    struct stat st;
    struct timespec t0, t1;
    int color;

    if (arg != 0 && has_fbc_ext(arg)) {
        fprintf(stderr, "flint build: %s is already a bytecode image\n", arg);
        return 2;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    color = use_color();

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
    inc_dep_begin(&ic, src_path);        /* record the include tree */

    printf("%s   Compiling %s [%s]%s\n", color ? CLR_BOLD_CYAN : "",
           name, flint_mem_mode_name(mem), color ? CLR_RESET : "");
    fflush(stdout);                      /* keep status ahead of any error */

    int rc = flint_assemble_ctx(src, src_path, inc_load, &ic, &p, stack,
                                err, sizeof(err));
    free(src);
    if (rc != 0) {
        inc_ctx_free(&ic);
        fprintf(stderr, "flint: %s\n", err);
        return 1;
    }

    /* the --elf stub must have been built first (Makefile: build/stub.elf);
     * checked before target/ is created so a failed build leaves nothing */
    if (elf && stat(FLINT_STUB_PATH, &st) != 0) {
        fprintf(stderr, "flint build: %s missing, run make\n", FLINT_STUB_PATH);
        inc_ctx_free(&ic);
        flint_prog_free(&p);
        return 2;
    }

    /* preprocessing succeeded: show what actually went into the image */
    print_dependency_tree(ic.tree, "   ", -1);
    inc_ctx_free(&ic);
    fflush(stdout);

    if (outarg != 0) {
        if (snprintf(out, sizeof out, "%s", outarg) >= (int)sizeof out) {
            fprintf(stderr, "flint build: output path too long\n");
            flint_prog_free(&p);
            return 2;
        }
    } else if (elf) {
        if (mkdir(ELF_OUTPUT_DIR, 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "flint build: cannot create %s/: %s\n",
                    ELF_OUTPUT_DIR, strerror(errno));
            flint_prog_free(&p);
            return 2;
        }
        if (snprintf(out, sizeof out, ELF_OUTPUT_DIR "/%.100s", name) >=
            (int)sizeof out) {
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

    if (elf) {
        if (build_elf(out, &p, mem, err, sizeof err) != 0) {
            fprintf(stderr, "flint: %s\n", err);
            flint_prog_free(&p);
            return 2;
        }
    } else if (flint_img_save(out, &p, mem, err, sizeof err) != 0) {
        fprintf(stderr, "flint: %s\n", err);
        flint_prog_free(&p);
        return 2;
    }
    flint_prog_free(&p);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (long)(t1.tv_sec - t0.tv_sec) * 1000L +
              (long)(t1.tv_nsec - t0.tv_nsec) / 1000000L;
    if (ms < 0) ms = 0;

    if (stat(out, &st) == 0) {
        printf("%s    Finished %s (%.1f KiB) in %ldms%s\n",
               color ? CLR_GREEN : "", out,
               (double)st.st_size / 1024.0, ms, color ? CLR_RESET : "");
    } else {
        printf("%s    Finished %s in %ldms%s\n", color ? CLR_GREEN : "",
               out, ms, color ? CLR_RESET : "");
    }
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
    "// Automatic memory: `alloc` blocks are owned by the mark-sweep\n"
    "// collector — a block stays valid while any register, stack slot or\n"
    "// global reaches it, and unreachable blocks (cycles included) are\n"
    "// reclaimed automatically. `free` is not needed; `enter`/`leave`\n"
    "// still build the local frame.\n"
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
