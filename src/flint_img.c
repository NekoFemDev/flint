/*
 * flint_img.c — .fbc bytecode image container (see flint_img.h).
 *
 * Host-side module: libc only for file I/O, the VM core is untouched.
 * The byte layout is a file-format ABI — bump FLINT_IMG_VERSION on any
 * change (the loader rejects other versions outright).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "flint.h"
#include "flint_img.h"

/* ---- little-endian u32 fields ---- */

static int wr_raw(FILE *f, const void *p, size_t n)
{
    return fwrite(p, 1, n, f) == n;
}

static int wr_u32(FILE *f, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v);
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
    return wr_raw(f, b, 4);
}

static int rd_raw(FILE *f, void *p, size_t n)
{
    return fread(p, 1, n, f) == n;
}

static int rd_u32(FILE *f, uint32_t *v)
{
    unsigned char b[4];
    if (!rd_raw(f, b, 4)) return 0;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

/* FNV-1a 32 over the code bytes, then the mem bytes */
static uint32_t img_checksum(const flint_byte *code, uint32_t cs,
                             const flint_byte *mem, uint32_t ms)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < cs; i++) { h ^= code[i]; h *= 16777619u; }
    for (i = 0; i < ms; i++) { h ^= mem[i];  h *= 16777619u; }
    return h;
}

int flint_img_save(const char *path, const flint_prog *p,
                   flint_mem_mode mem, char *err, uint32_t errsz)
{
    FILE *f;
    int ok = 1;

    f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errsz, "%s: cannot write (%s)", path, strerror(errno));
        return -1;
    }
    ok = ok && wr_raw(f, FLINT_IMG_MAGIC, 4);
    ok = ok && wr_u32(f, FLINT_IMG_VERSION);
    ok = ok && wr_u32(f, p->code_size);
    ok = ok && wr_u32(f, p->mem_size);
    ok = ok && wr_u32(f, p->entry);
    ok = ok && wr_u32(f, p->heap_start);
    ok = ok && wr_u32(f, (uint32_t)mem);
    ok = ok && wr_u32(f, img_checksum(p->code, p->code_size,
                                      p->mem,  p->mem_size));
    ok = ok && wr_raw(f, p->code, p->code_size);
    ok = ok && wr_raw(f, p->mem,  p->mem_size);
    if (fclose(f) != 0) ok = 0;

    if (!ok) {
        snprintf(err, errsz, "%s: write failed", path);
        remove(path);                       /* no half-written images */
        return -1;
    }
    return 0;
}

int flint_img_load(const char *path, flint_prog *p, flint_mem_mode *mem,
                   char *err, uint32_t errsz)
{
    FILE *f;
    char magic[4];
    uint32_t ver, cs, ms, entry, heap, mode, sum;

    p->code = 0; p->mem = 0;
    p->code_size = p->mem_size = 0;
    p->entry = p->heap_start = 0;
    p->syms = 0; p->nsyms = 0;      /* .fbc carries no symbol table */

    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "%s: cannot read (%s)", path, strerror(errno));
        return -1;
    }

    if (!rd_raw(f, magic, 4) || memcmp(magic, FLINT_IMG_MAGIC, 4) != 0) {
        snprintf(err, errsz, "%s: not a flint bytecode image", path);
        goto bad;
    }
    if (!rd_u32(f, &ver) || !rd_u32(f, &cs) || !rd_u32(f, &ms) ||
        !rd_u32(f, &entry) || !rd_u32(f, &heap) || !rd_u32(f, &mode) ||
        !rd_u32(f, &sum)) {
        snprintf(err, errsz, "%s: truncated image header", path);
        goto bad;
    }
    if (ver != FLINT_IMG_VERSION) {
        snprintf(err, errsz, "%s: unsupported image version %u (want %u)",
                 path, (unsigned)ver, (unsigned)FLINT_IMG_VERSION);
        goto bad;
    }
    if (cs == 0 || ms == 0 || cs > FLINT_IMG_MAX || ms > FLINT_IMG_MAX ||
        entry >= cs || heap > ms) {
        snprintf(err, errsz, "%s: bad image sizes", path);
        goto bad;
    }

    p->code = (flint_byte *)malloc((size_t)cs);
    p->mem  = (flint_byte *)malloc((size_t)ms);
    if (!p->code || !p->mem) {
        snprintf(err, errsz, "%s: out of memory", path);
        goto bad;
    }
    if (!rd_raw(f, p->code, (size_t)cs) || !rd_raw(f, p->mem, (size_t)ms)) {
        snprintf(err, errsz, "%s: truncated image (%u + %u byte payload)",
                 path, (unsigned)cs, (unsigned)ms);
        goto bad;
    }
    if (fgetc(f) != EOF) {                  /* exact length: no trailing data */
        snprintf(err, errsz, "%s: unexpected trailing data", path);
        goto bad;
    }
    if (img_checksum(p->code, cs, p->mem, ms) != sum) {
        snprintf(err, errsz, "%s: checksum mismatch (corrupt image?)", path);
        goto bad;
    }

    p->code_size  = cs;
    p->mem_size   = ms;
    p->entry      = entry;
    p->heap_start = heap;
    if (mem)
        *mem = (mode == FLINT_MEM_AUTO) ? FLINT_MEM_AUTO : FLINT_MEM_MANUAL;

    fclose(f);
    return 0;

bad:
    free(p->code);
    free(p->mem);
    p->code = 0;
    p->mem = 0;
    fclose(f);
    return -1;
}
