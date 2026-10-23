/* flint_img.h — .fbc bytecode image container.
 *
 * `flint build` packs an assembled program into a single binary:
 *
 *   offset  size  field
 *   0       4     magic   "FLBC"
 *   4       4     version (1)
 *   8       4     code_size
 *   12      4     mem_size
 *   16      4     entry
 *   20      4     heap_start
 *   24      4     mem_mode   (flint_mem_mode: 0 manual / 1 auto)
 *   28      4     checksum   (FNV-1a 32 over code bytes, then mem bytes)
 *   32      N     code image
 *   32+N    M     mem image
 *
 * All header fields are little-endian u32. The image is self-contained:
 * includes were resolved at build time, so loading needs neither the
 * assembler nor the source files.
 *
 * Errors follow the assembler convention: 0 on success, -1 with `err`
 * filled ("...: not a flint bytecode image", "...: checksum mismatch").
 */
#ifndef FLINT_IMG_H
#define FLINT_IMG_H

#include "flint.h"

#define FLINT_IMG_MAGIC    "FLBC"
#define FLINT_IMG_VERSION  1u
#define FLINT_IMG_MAX      0x40000000u  /* 1 GiB per section: sanity cap */

/* write the assembled program as an .fbc image;
 * mem = the project memory model (stored in the header) */
int flint_img_save(const char *path, const flint_prog *p,
                   flint_mem_mode mem, char *err, uint32_t errsz);

/* read an .fbc image: fills *p (malloc'd; free with flint_prog_free)
 * and *mem */
int flint_img_load(const char *path, flint_prog *p, flint_mem_mode *mem,
                   char *err, uint32_t errsz);

#endif /* FLINT_IMG_H */
