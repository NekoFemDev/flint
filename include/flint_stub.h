/* flint_stub.h — standalone ELF trailer for `flint build --elf` (src/stub.c).
 *
 * A standalone binary is a copy of the stub ELF with the program's images
 * appended, fixed by a trailing header — self-extracting-executable style
 * (the kernel ignores bytes past the loaded segments, so the fused file is
 * still a valid executable):
 *
 *   [ stub ELF ][ code image ][ mem image ][ struct FlintPayload ]
 *
 * At run time the stub re-reads its own file through /proc/self/exe,
 * decodes the trailer, loads both images into malloc'd buffers and runs
 * the program with the same CLI syscall table `flint run` uses — no flint
 * install, no sources, no .fbc file needed. The trailer sits at the very
 * end of the file; all fields are little-endian u32 (same convention as
 * the .fbc container in flint_img.h).
 *
 * heap_start/mem_mode are stored for the same reason the .fbc header
 * stores them: flint_init() leaves the bump allocator at address 0, so a
 * host must feed flint_heap_set() the aligned end of the data image (and
 * the project memory model) or `alloc` would scribble over the data.
 */
#ifndef FLINT_STUB_H
#define FLINT_STUB_H

#include "flint.h"

#define FLINT_PAYLOAD_MAGIC 0x464C4E54u     /* "FLNT" */
#define FLINT_STUB_PATH     "build/stub.elf" /* built by the Makefile */

/* trailer at the end of a standalone ELF; code[] and mem[] precede it,
 * in that order, so: payload_off = filesize - sizeof(trailer)
 *                                  - code_size - mem_size            */
struct FlintPayload {
    uint32_t magic;       /* FLINT_PAYLOAD_MAGIC                     */
    uint32_t code_size;   /* byte length of the code image           */
    uint32_t mem_size;    /* byte length of the mem image            */
    uint32_t entry_pc;    /* entry offset into code (flint_prog.entry) */
    uint32_t heap_start;  /* aligned data end (flint_prog.heap_start) */
    uint32_t mem_mode;    /* flint_mem_mode: 0 manual / 1 auto       */
} __attribute__((packed));

/* six u32 fields: the size is a file-format ABI, keep it padding-free */
typedef char flint__assert_payload_is_24[
    (sizeof(struct FlintPayload) == 24) ? 1 : -1];

#endif /* FLINT_STUB_H */
