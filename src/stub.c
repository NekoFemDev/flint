/*
 * stub.c — standalone host for a fused Flint executable (`flint build --elf`).
 *
 * The Makefile links this file (with the VM core and the CLI host) into
 * build/stub.elf. `flint build --elf` then copies that stub, appends the
 * program's code and mem images plus a trailing struct FlintPayload, and
 * chmods the result — one executable file, zero external dependencies
 * beyond what the toolchain linked (see the Makefile on static linking).
 *
 *   [ stub ELF ][ code ][ mem ][ struct FlintPayload ]   <- /proc/self/exe
 *
 * The kernel loads the stub normally and ignores the appended bytes; the
 * stub re-opens its own file through /proc/self/exe, decodes the trailer
 * at the end, computes where the images start, loads them and runs the
 * program exactly like `flint run` would (same CLI syscall table).
 *
 * Exit codes: 127 ("command not found" style) when the trailer is missing
 * or malformed — e.g. running build/stub.elf before it was fused — and the
 * usual flint convention otherwise: r0 at HLT, or -FLINT_ERR_* on a trap.
 */
#include <stdio.h>
#include <stdlib.h>
#include "flint.h"
#include "flint_cli.h"
#include "flint_stub.h"

/* little-endian u32 decode: the trailer is a file format, so host
 * endianness must not leak into it (same convention as flint_img.c) */
static uint32_t stub_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int stub_fail(const char *why)
{
    fprintf(stderr, "stub: %s\n", why);
    return 127;
}

int main(void)
{
    FILE *f;
    long file_size, payload_off;
    unsigned char tr[sizeof(struct FlintPayload)];
    uint32_t magic, code_size, mem_size, entry, heap_start, mem_mode;
    flint_byte *code, *mem;
    flint_vm vm;
    flint_word rc;

    f = fopen("/proc/self/exe", "rb");
    if (!f) return stub_fail("cannot open /proc/self/exe");

    if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0) {
        fclose(f);
        return stub_fail("cannot size /proc/self/exe");
    }

    /* the payload trailer is the last thing in the file */
    if (file_size < (long)sizeof tr ||
        fseek(f, -(long)sizeof tr, SEEK_END) != 0 ||
        fread(tr, 1, sizeof tr, f) != sizeof tr) {
        fclose(f);
        return stub_fail("not a Flint binary");
    }

    magic      = stub_u32(tr + 0);
    code_size  = stub_u32(tr + 4);
    mem_size   = stub_u32(tr + 8);
    entry      = stub_u32(tr + 12);
    heap_start = stub_u32(tr + 16);
    mem_mode   = stub_u32(tr + 20);

    if (magic != FLINT_PAYLOAD_MAGIC) {
        fclose(f);
        return stub_fail("not a Flint binary");
    }

    /* images sit between the stub and the trailer */
    payload_off = file_size - (long)sizeof tr
                - (long)code_size - (long)mem_size;
    if (code_size == 0 || mem_size == 0 || payload_off < 0) {
        fclose(f);
        return stub_fail("truncated Flint payload");
    }

    code = (flint_byte *)malloc(code_size);
    mem  = (flint_byte *)malloc(mem_size);
    if (!code || !mem) {
        free(code);
        free(mem);
        fclose(f);
        return stub_fail("out of memory");
    }

    if (fseek(f, payload_off, SEEK_SET) != 0 ||
        fread(code, 1, code_size, f) != code_size ||
        fread(mem, 1, mem_size, f) != mem_size) {
        free(code);
        free(mem);
        fclose(f);
        return stub_fail("truncated Flint payload");
    }
    fclose(f);

    flint_init(&vm, code, code_size, mem, mem_size);
    flint_heap_set(&vm, heap_start, (flint_mem_mode)mem_mode);
    flint_cli_sysoff(&vm);
    rc = flint_run(&vm, entry);

    if (vm.err != FLINT_OK) {
        fprintf(stderr, "stub: runtime: %s\n",
                vm.err_text ? vm.err_text : "trap");
        return (int)(-(int64_t)vm.err);      /* same as the flint CLI */
    }
    return (int)(rc & 0x7FFFFFFF);           /* r0 at HLT, int-narrowed */
}
