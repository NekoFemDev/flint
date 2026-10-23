/*
 * flint.h — FLINT public header.
 *
 * FLINT — a minimal assembly-style language runtime for embedding.
 * Core invariants:
 *   - zero libc dependency inside the VM core: the host provides
 *     code and memory buffers, syscalls are host callbacks;
 *   - no global state: everything lives in flint_vm, N instances
 *     per process are fine;
 *   - word = signed 64-bit, memory is a flat byte-addressed image,
 *     little-endian, words stored byte-wise (no alignment needed);
 *   - call/return stack grows down from the top of memory;
 *   - no flags register: comparison is fused into branches
 *     (MIPS/RISC-V lineage).
 *
 * Instruction encoding (bytecode, little-endian fields):
 *   every instruction starts with an opcode byte, followed by
 *   register bytes (u8, 0..7) and immediates (i64, 8 bytes LE).
 *
 * Layouts (fields in order):
 *   OP_INT     slot:u8
 *   OP_HLT     -
 *   OP_SET     rd:u8  imm:i64
 *   OP_COPY    rd:u8  rs:u8
 *   OP_LOADB   rd:u8  rb:u8  off:i64
 *   OP_LOADW   rd:u8  rb:u8  off:i64
 *   OP_STOREB  rb:u8  rs:u8  off:i64        (text: store.byte [rb+off], rs)
 *   OP_STOREW  rb:u8  rs:u8  off:i64        (text: store [rb+off], rs)
 *   OP_ADD..OP_SHR       rd:u8 ra:u8 rb:u8
 *   OP_ADDI..OP_SHRI     rd:u8 ra:u8 imm:i64
 *   OP_NEG     rd:u8 rs:u8
 *   OP_NOT     rd:u8 rs:u8
 *   OP_INC     rd:u8
 *   OP_DEC     rd:u8
 *   OP_JEQ     rs:u8  rt:u8  target:i64      (bytes: absolute offset in code)
 *   OP_JEQI    rs:u8  imm:i64  target:i64
 *   OP_JNE/JNEI, OP_JLT/JLTI, OP_JGE/JGEI — same shapes
 *   OP_JMP     target:i64
 *   OP_CALL    target:i64
 *   OP_RET     -
 *   OP_PUSH    rs:u8
 *   OP_PUSHI   imm:i64
 *   OP_POP     rd:u8
 */
#ifndef FLINT_H
#define FLINT_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FLINT_VERSION  "0.1.0-pre.2"
#define FLINT_NREGS    8
#define FLINT_MAX_SYS  64

typedef int64_t flint_word;
typedef uint8_t flint_byte;

/* compile-time guards: the bytecode ISA assumes 8 registers of 64-bit
 * words; a negative-size array turns a mismatch into a build error */
typedef char flint__assert_word_is_64[(sizeof(flint_word) == 8) ? 1 : -1];
typedef char flint__assert_nregs_is_8[(FLINT_NREGS == 8) ? 1 : -1];

/* standard host syscall slots — the table the CLI host implements.
 * Embedders installing their own handlers may use any numbering. */
enum {
    FLINT_SYS_PRINT_STR = 0,  /* r1 = addr, r2 = len: raw bytes out;
                                 must not clobber registers            */
    FLINT_SYS_PUTC      = 1,  /* low byte of r0 -> one byte out       */
    FLINT_SYS_GETC      = 2,  /* one byte in -> r0 (0..255, -1 = EOF) */
    FLINT_SYS_READ_MEM  = 3   /* reserved: debugger read_mem (stub)   */
};

typedef struct flint_vm flint_vm;
typedef void (*flint_syscall)(flint_vm *vm);

/* opcodes — bytecode-level ISA. Text-level names (v3) map onto these. */
enum {
    OP_INT = 0, OP_HLT,
    OP_SET, OP_COPY,
    OP_LOADB, OP_LOADW, OP_STOREB, OP_STOREW,
    OP_ADD, OP_ADDI, OP_SUB, OP_SUBI, OP_MUL, OP_MULI,
    OP_DIV, OP_DIVI, OP_MOD, OP_MODI,
    OP_AND, OP_ANDI, OP_OR, OP_ORI, OP_XOR, OP_XORI,
    OP_SHL, OP_SHLI, OP_SHR, OP_SHRI,
    OP_NEG, OP_NOT, OP_INC, OP_DEC,
    OP_JEQ, OP_JEQI, OP_JNE, OP_JNEI,
    OP_JLT, OP_JLTI, OP_JGE, OP_JGEI,
    OP_JLO, OP_JLOI, OP_JHS, OP_JHSI,   /* unsigned pairs (below / higher-same) */
    OP_JMP, OP_CALL, OP_RET,
    OP_PUSH, OP_PUSHI, OP_POP,
    OP_COUNT
};

/* error codes */
enum {
    FLINT_OK = 0,
    FLINT_ERR_NOSYS,    /* INT N with no host callback on slot N        */
    FLINT_ERR_BADPC,    /* pc out of code bounds                        */
    FLINT_ERR_BADOP,    /* unknown opcode byte                          */
    FLINT_ERR_BADREG,   /* register index outside 0..7                  */
    FLINT_ERR_BADMEM,   /* address outside memory bounds                */
    FLINT_ERR_STACK,    /* stack underflow/overflow                     */
    FLINT_ERR_DIV0,     /* division/modulo by zero                      */
    FLINT_ERR_LIMIT,    /* max_steps exceeded                          */
    FLINT_ERR_INIT      /* init called with bad buffers                 */
};

struct flint_vm {
    /* registers r0..r7 — full 64-bit words. Instruction fields that name
     * a register carry a single uint8_t index, not the register file. */
    flint_word   r[FLINT_NREGS];

    /* host-provided images */
    flint_byte  *code;         /* bytecode      */
    uint32_t     code_size;
    flint_byte  *mem;          /* flat memory   */
    uint32_t     mem_size;

    /* execution state (visible for dumps/debug) */
    uint32_t     pc;           /* byte offset into code */
    uint32_t     sp;           /* top-of-stack pointer, init = mem_size */
    uint32_t     steps;
    uint32_t     max_steps;    /* 0 = unlimited */
    uint8_t      halted;

    /* host syscall table; unused slots stay NULL -> FLINT_ERR_NOSYS */
    flint_syscall sys[FLINT_MAX_SYS];

    /* last error */
    int          err;
    const char  *err_text;
};

/* lifecycle */
void  flint_init(flint_vm *vm,
                 flint_byte *code, uint32_t code_size,
                 flint_byte *mem,  uint32_t mem_size);
void  flint_sys_set(flint_vm *vm, int slot, flint_syscall fn);

/* execution: returns r[0] at HLT; negative values = -FLINT_ERR_* */
flint_word  flint_run(flint_vm *vm, uint32_t entry);
int   flint_step(flint_vm *vm); /* execute exactly one instruction, 0 on ok */

/* assembler output: images ready for flint_vm_init */
typedef struct flint_prog {
    flint_byte *code;        /* bytecode image        */
    uint32_t    code_size;
    flint_byte *mem;         /* data + stack space    */
    uint32_t    mem_size;
    uint32_t    entry;       /* offset of 'main'      */
} flint_prog;

/* assembler (isolated TU, never required by a VM-only embed) */
int  flint_assemble(const char *src, flint_prog *out,
                    uint32_t stack_extra, char *err, uint32_t err_size);

/* standalone include preprocessor (flint_asm.c): textually expands lines
 *     include "path"      .include "path"
 * (each path resolved relative to the file containing the directive) and
 * returns a malloc'd source text ready for flint_assemble(), or NULL with
 * err filled. max_depth 0 = default (10); cycles die on the cap.
 * Hosts wired through flint_assemble_ctx(inc_fn) do not need this — the
 * CLI resolves includes by callback, keeping per-file line numbers. */
char *flint_pp_includes(const char *src, const char *src_name,
                        uint32_t max_depth, char *err, uint32_t errsz);

/* include support: the host decides where a quoted path lives.
 * Return a NUL-terminated source text (malloc'd; the assembler frees it),
 *   "" (also malloc'd)  = this file was already included once — skip it,
 *   NULL + err filled   = hard error (include fails at that source line).
 * from_dir is the directory of the text that contains the include. */
typedef char *(*flint_include_fn)(void *ud, const char *name,
                                  const char *from_dir,
                                  char *err, uint32_t errsz);

int  flint_assemble_ctx(const char *src, const char *src_name,
                        flint_include_fn inc_fn, void *inc_ud,
                        flint_prog *out,
                        uint32_t stack_extra, char *err, uint32_t err_size);
void flint_prog_free(flint_prog *p);

#define FLINT_ERR_STR(e) flint_errno(e)
const char *flint_errno(int err);

#ifdef __cplusplus
}
#endif
#endif /* FLINT_H */
