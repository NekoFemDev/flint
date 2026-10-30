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
 *   - call/return stack grows down from the top of memory; a CALL frame
 *     is one word: [return pc]. A function that needs locals uses
 *     ENTER/LEAVE: ENTER pushes the old fp and reserves N bytes;
 *     LEAVE restores sp from fp and pops the old fp;
 *   - no flags register: comparison is fused into branches
 *     (MIPS/RISC-V lineage).
 *
 * Register file / ABI (v0.2.0):
 *   r0..r3    argument / return-value registers
 *   r4..r11   caller-saved (a callee may clobber them)
 *   r12..r15  callee-saved (a callee must preserve them; push/pop
 *             them itself — ENTER/LEAVE do not save them automatically)
 *
 * Instruction encoding (bytecode, little-endian fields):
 *   every instruction starts with an opcode byte, followed by
 *   register bytes (u8, 0..15) and immediates (i64, 8 bytes LE).
 *   In an address-base field the value 16 (FLINT_REG_FP) selects the
 *   frame pointer; it is not a general-purpose register.
 *
 * Layouts (fields in order):
 *   OP_INT     slot:u8
 *   OP_HLT     -
 *   OP_SET     rd:u8  imm:i64
 *   OP_COPY    rd:u8  rs:u8
 *   OP_LOADB   rd:u8  rb:u8  off:i64
 *   OP_LOADW   rd:u8  rb:u8  off:i64
 *   OP_LOADX   rd:u8  rb:u8  ri:u8          (text: loadx rd, [rb+ri])
 *   OP_STOREB  rb:u8  rs:u8  off:i64        (text: store.byte [rb+off], rs)
 *   OP_STOREW  rb:u8  rs:u8  off:i64        (text: store [rb+off], rs)
 *   OP_STOREX  rb:u8  ri:u8  rs:u8          (text: storx [rb+ri], rs)
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
 *   OP_JMPR    rs:u8             (indirect: pc = rs)
 *   OP_CALLR   rs:u8             (indirect: push return pc; pc = rs)
 *   OP_RET     -
 *   OP_ENTER   size:i64          (push fp; fp = sp; sp -= size)
 *   OP_LEAVE   -                 (sp = fp; pop fp)
 *   OP_PUSH    rs:u8
 *   OP_PUSHI   imm:i64
 *   OP_POP     rd:u8
 *   OP_ALLOC   rd:u8  size:i64   (manual: rd = heap cursor, cursor += size;
 *                                 auto: rd = managed block payload, zeroed)
 *   OP_FREE    rs:u8             (operand validated; no-op — see below)
 *   OP_GC      -                 (auto: collect now; manual: no-op)
 *
 * New opcodes are appended before OP_COUNT and existing ones never move:
 * an opcode value is baked into every .fbc image, so renumbering would
 * invalidate them.
 *
 * RAM control: the memory model (Flint.toml `memory`, flint_mem_mode)
 * selects the allocator (v0.3.0):
 *   manual — a bump allocator over [heap_start, sp). A RETURN never
 *            rewinds the cursor, so a returned block stays valid; `free`
 *            is a validated no-op (no size metadata to reclaim by).
 *   auto   — a conservative, non-moving mark-sweep collector
 *            (src/flint_gc.c). `alloc` returns the payload of a managed
 *            block (16-byte header before it, payload zeroed); garbage —
 *            cycles included — is reclaimed when unreachable from
 *            registers, the active stack, the data section, host roots
 *            and pins. The collector runs on allocation thresholds,
 *            allocation failure, and the `gc` instruction /
 *            flint_gc_collect(). `free` stays a no-op: the collector
 *            owns lifetime; hosts can call flint_gc_free() instead.
 */
#ifndef FLINT_H
#define FLINT_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FLINT_VERSION  "0.4.0-beta.5"
#define FLINT_NREGS    16
#define FLINT_MAX_SYS  64
/* pseudo-register used in address-base fields; never a general register */
#define FLINT_REG_FP   16

typedef int64_t flint_word;
typedef uint8_t flint_byte;

/* compile-time guards: the bytecode ISA assumes 16 registers of 64-bit
 * words; a negative-size array turns a mismatch into a build error */
typedef char flint__assert_word_is_64[(sizeof(flint_word) == 8) ? 1 : -1];
typedef char flint__assert_nregs_is_16[(FLINT_NREGS == 16) ? 1 : -1];

/* standard host syscall slots — the table the CLI host implements.
 * Embedders installing their own handlers may use any numbering. */
enum {
    FLINT_SYS_PRINT_STR = 0,  /* r1 = addr, r2 = len: raw bytes out;
                                 must not clobber registers            */
    FLINT_SYS_PUTC      = 1,  /* low byte of r0 -> one byte out       */
    FLINT_SYS_GETC      = 2,  /* one byte in -> r0 (0..255, -1 = EOF) */
    FLINT_SYS_OPEN      = 3,  /* r0 = path addr, r1 = flags (0 read /
                                 1 write); r0 = fd or -1              */
    FLINT_SYS_READ      = 4,  /* r0 = fd, r1 = buf, r2 = len;
                                 r0 = bytes read or -1                */
    FLINT_SYS_WRITE     = 5,  /* r0 = fd, r1 = buf, r2 = len;
                                 r0 = bytes written or -1             */
    FLINT_SYS_CLOSE     = 6   /* r0 = fd; r0 = 0 or -1                */
};

/* memory model — how a program manages its storage (Flint.toml `memory`).
 * FLINT_MEM_MANUAL: `alloc` is a bump allocator; blocks are never
 *                   reclaimed automatically; `free` is a validated no-op.
 * FLINT_MEM_AUTO:   `alloc` is served by the bundled conservative,
 *                   non-moving mark-sweep collector (src/flint_gc.c): a
 *                   block of at least the requested size (payload zeroed)
 *                   comes from a first-fit free list or the bump frontier;
 *                   unreachable blocks — cycles included — are reclaimed
 *                   at collection points (allocation failure, threshold,
 *                   the `gc` opcode or flint_gc_collect()). `free` stays a
 *                   no-op; managed lifetime belongs to the collector.
 * The mode travels with the VM (and in .fbc/ELF images) and is a real
 * allocator switch since v0.3.0 — auto mode is a breaking semantic
 * change from the v0.2 informational flag. */
typedef enum {
    FLINT_MEM_MANUAL = 0,
    FLINT_MEM_AUTO   = 1
} flint_mem_mode;

typedef struct flint_vm flint_vm;
typedef void (*flint_syscall)(flint_vm *vm);

/* opcodes — bytecode-level ISA. Text-level names (v3) map onto these. */
enum {
    OP_INT = 0, OP_HLT,
    OP_SET, OP_COPY,
    OP_LOADB, OP_LOADW, OP_LOADX, OP_STOREB, OP_STOREW, OP_STOREX,
    OP_ADD, OP_ADDI, OP_SUB, OP_SUBI, OP_MUL, OP_MULI,
    OP_DIV, OP_DIVI, OP_MOD, OP_MODI,
    OP_AND, OP_ANDI, OP_OR, OP_ORI, OP_XOR, OP_XORI,
    OP_SHL, OP_SHLI, OP_SHR, OP_SHRI,
    OP_NEG, OP_NOT, OP_INC, OP_DEC,
    OP_JEQ, OP_JEQI, OP_JNE, OP_JNEI,
    OP_JLT, OP_JLTI, OP_JGE, OP_JGEI,
    OP_JLO, OP_JLOI, OP_JHS, OP_JHSI,   /* unsigned pairs (below / higher-same) */
    OP_JMP, OP_CALL, OP_RET, OP_ENTER, OP_LEAVE,
    OP_PUSH, OP_PUSHI, OP_POP,
    OP_ALLOC, OP_FREE, OP_GC,
    OP_JMPR, OP_CALLR,   /* indirect jump/call: target comes from a register */
    OP_COUNT
};

/* error codes — these values are the public ABI (and the README table);
 * flint_run returns -err, e.g. FLINT_ERR_DIV0 -> -7 */
enum {
    FLINT_OK = 0,
    FLINT_ERR_NOSYS  = 1,   /* INT N with no host callback on slot N        */
    FLINT_ERR_BADPC  = 2,   /* pc out of code bounds                        */
    FLINT_ERR_BADOP  = 3,   /* unknown opcode byte                          */
    FLINT_ERR_BADREG = 4,   /* register index outside 0..15                 */
    FLINT_ERR_BADMEM = 5,   /* address outside memory bounds (or corrupt
                               managed-heap block header in auto mode)      */
    FLINT_ERR_STACK  = 6,   /* stack underflow/overflow                     */
    FLINT_ERR_DIV0   = 7,   /* division/modulo by zero                      */
    FLINT_ERR_LIMIT  = 8,   /* max_steps exceeded                           */
    FLINT_ERR_INIT   = 9,   /* init called with bad buffers                 */
    FLINT_ERR_OOM    = 10   /* managed heap exhausted after a collection    */
};

/* ---- automatic memory: conservative mark-sweep collector (auto mode) ----
 *
 * Managed heap layout inside the existing mem image: blocks are contiguous
 * from gc.heap_begin (heap_start rounded up to 16) to heap_ptr (the bump
 * frontier). Every block starts with a 16-byte flint_gc_header; `alloc`
 * returns the payload pointer (header + 16). A block that will not fit
 * below sp fails; if a collection then frees nothing usable the VM traps
 * FLINT_ERR_OOM.
 *
 * Roots are scanned conservatively: registers r0..r15, the active stack
 * [sp, mem_size), the data section [0, heap_start) and host-registered
 * roots/pins. A word is a candidate pointer when it lands anywhere inside
 * the payload of an allocated block (interior pointers mark the
 * containing block); every other word is ignored. The collector is
 * non-moving and non-recursive (fixed mark worklist + a full-rescan slow
 * path on overflow). False retention is possible — an integer, a return
 * pc or other stale value that "looks like" an address keeps a block
 * alive — and is bounded by the number of root words; it is documented,
 * not eliminated. Pointers stored at unaligned addresses are missed
 * (aligned-word scanning); store pointers at 8-byte-aligned addresses.
 *
 * The header `next` field is reserved (set to FLINT_GC_NIL); free blocks
 * are found by walking the block sequence in address order (first fit),
 * and adjacent free blocks are coalesced during sweep. */
#define FLINT_GC_MAGIC   0x4D474C46u  /* "FLGM" as bytes F,L,G,M (LE)     */
#define FLINT_GC_HDR     16u          /* header bytes before the payload  */
#define FLINT_GC_ALIGN   8u           /* payload size/scan granularity    */
#define FLINT_GC_NIL     0xFFFFFFFFu  /* no block (reserved header field) */
#define FLINT_GC_F_ALLOC 0x1u         /* block is allocated               */
#define FLINT_GC_F_MARK  0x2u         /* block marked this cycle          */
#define FLINT_GC_MARKSTACK  1024u     /* fixed mark worklist (block ads)  */
#define FLINT_GC_ROOTS_MAX  8u        /* host root slots per VM           */
#define FLINT_GC_PINS_MAX   8u        /* host pins per VM                 */
#define FLINT_GC_DEFAULT_THRESHOLD 32768u /* bytes between proactive GCs  */

/* managed block header (little-endian u32 fields, byte-addressed) */
typedef struct {
    uint32_t size;    /* payload bytes (multiple of 8)                  */
    uint32_t flags;   /* FLINT_GC_F_*                                   */
    uint32_t next;    /* reserved for a free-list chain; FLINT_GC_NIL   */
    uint32_t magic;   /* FLINT_GC_MAGIC (validation)                    */
} flint_gc_header;

/* collector statistics — a snapshot, see flint_gc_get_stats */
typedef struct {
    uint32_t collections;      /* completed collections                    */
    uint32_t bytes_live;       /* payload bytes in allocated blocks        */
    uint32_t bytes_free;       /* payload bytes in free blocks             */
    uint32_t bytes_alloced;    /* lifetime payload bytes handed out        */
    uint32_t bytes_reclaimed;  /* lifetime payload bytes swept             */
    uint32_t threshold;        /* proactive collection threshold (bytes)   */
    uint32_t overflow_rescans; /* mark-stack-overflow slow-path passes     */
    uint8_t  enabled;          /* 1 in auto mode, 0 in manual              */
    uint8_t  stress;           /* collect before every allocation         */
} flint_gc_stats;

/* collector state — public because it lives inside flint_vm (zero hidden
 * state, zero allocation); treat fields as internal where possible. */
typedef struct {
    uint8_t  enabled;
    uint8_t  stress;
    uint8_t  oflow;                 /* mark stack overflowed (internal)    */
    uint32_t since;                 /* bytes allocated since last collect  */
    uint32_t threshold;
    uint32_t collections;
    uint32_t bytes_live;
    uint32_t bytes_free;
    uint32_t bytes_alloced;
    uint32_t bytes_reclaimed;
    uint32_t overflow_rescans;
    uint32_t heap_begin;            /* first managed block (16-aligned)    */
    uint32_t msp;                   /* mark worklist depth (internal)      */
    uint32_t find_blk, find_end;    /* lookup cache: payload span of the
                                       last hit (internal)                 */
    uint32_t markstack[FLINT_GC_MARKSTACK];
    flint_word *roots[FLINT_GC_ROOTS_MAX];
    uint32_t   nroots;
    flint_word pins[FLINT_GC_PINS_MAX];
    uint32_t   npins;
} flint_gc_state;

struct flint_vm {
    /* registers r0..r15 — full 64-bit words. Instruction fields that name
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
    flint_word   fp;           /* frame pointer: [sp, fp) is the local area */
    uint32_t     heap_ptr;     /* bump frontier (OP_ALLOC); in auto mode it
                                  is also the end of the managed blocks */
    uint32_t     heap_start;   /* bottom of the heap (auto: 16-aligned) */
    flint_mem_mode mem_mode;   /* manual / auto — a real allocator switch */
    flint_gc_state gc;         /* auto-mode collector (src/flint_gc.c) */
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
/* heap configuration, call after flint_init. heap_start = first
 * allocatable offset (the CLI passes flint_prog.heap_start, the aligned
 * end of the data image); host-built images pick their own. mode selects
 * the allocator (v0.3.0): manual = the bump arena (unchanged), auto =
 * the managed mark-sweep heap — auto rounds heap_start up to 16 and
 * zeroes the scratch region [heap_start, mem_size) so conservative
 * scanning starts clean. Call once, right after flint_init. */
void  flint_heap_set(flint_vm *vm, uint32_t heap_start, flint_mem_mode mode);

/* ---- automatic memory API (auto mode; see src/flint_gc.c) ----
 * All of these are safe to call on a manual-mode VM: collect/stats and
 * the root/pin registration are no-ops or harmless, while
 * flint_gc_alloc/free set FLINT_ERR_INIT (misuse). A host that keeps a
 * raw pointer into a managed payload outside the VM roots must pin it
 * before letting a collection run. */
void flint_gc_init(flint_vm *vm);   /* flint_heap_set calls this in auto */
void flint_gc_collect(flint_vm *vm);       /* mark-sweep now (manual: no-op) */
flint_word flint_gc_alloc(flint_vm *vm, uint32_t size); /* payload, 0 = OOM */
void flint_gc_free(flint_vm *vm, flint_word payload);    /* explicit release */
void flint_gc_get_stats(const flint_vm *vm, flint_gc_stats *out);
void flint_gc_set_threshold(flint_vm *vm, uint32_t bytes); /* 0 = fail-only */
void flint_gc_set_stress(flint_vm *vm, int on);
int  flint_gc_root(flint_vm *vm, flint_word *slot);  /* 0 ok, -1 (null/full) */
int  flint_gc_unroot(flint_vm *vm, flint_word *slot);    /* 0 ok, -1 unknown */
int  flint_gc_pin(flint_vm *vm, flint_word addr);    /* 0 ok, -1 (full) */
int  flint_gc_unpin(flint_vm *vm, flint_word addr);  /* 0 ok, -1 unknown */

/* execution: returns r[0] at HLT; negative values = -FLINT_ERR_* */
flint_word  flint_run(flint_vm *vm, uint32_t entry);
int   flint_step(flint_vm *vm); /* execute exactly one instruction, 0 on ok */

/* exported label — one `export name` directive's output: the label name
 * and its code offset, ready for hosts that call into a program by name
 * (the fused .so; see flint_so.h). FLINT_SYM_NAME_MAX counts the
 * terminating NUL, and matches the .so symbol record. */
#define FLINT_SYM_NAME_MAX 60

typedef struct {
    char     name[FLINT_SYM_NAME_MAX];
    uint32_t pc;             /* code offset of the label */
} flint_sym;

/* assembler output: images ready for flint_vm_init */
typedef struct flint_prog {
    flint_byte *code;        /* bytecode image        */
    uint32_t    code_size;
    flint_byte *mem;         /* data + stack space    */
    uint32_t    mem_size;
    uint32_t    entry;       /* offset of 'main'      */
    uint32_t    heap_start;  /* aligned end of the data section: first
                                allocatable offset (feed to flint_heap_set) */
    flint_sym  *syms;        /* `export`-ed code labels, in source order
                                (NULL without the directive); malloc'd,
                                freed by flint_prog_free */
    uint32_t    nsyms;
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
 * from_dir is the directory of the text that contains the include;
 * from_name is the name that text was opened under (the including file —
 * the root source's src_name at the top level). Hosts use it to relate
 * includes to one another, e.g. to build a dependency tree. */
typedef char *(*flint_include_fn)(void *ud, const char *name,
                                  const char *from_dir, const char *from_name,
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
