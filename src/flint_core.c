/*
 * flint_core.c — FLINT VM dispatch. Pure C99, zero libc.
 *
 * The dispatcher reads fields straight from the byte stream; the
 * switch arm per opcode is the whole "compiler backend". Error policy:
 * on any violation the VM sets err/err_text and stops; flint_run
 * returns -err. Syscalls are the only interface between VM and host.
 */
#include "flint.h"

/* ---- little-endian field readers ---- */

static int rd_u8(flint_vm *vm, uint32_t *pc, uint8_t *out)
{
    if (*pc + 1 > vm->code_size) { vm->err = FLINT_ERR_BADPC; return 0; }
    *out = vm->code[(*pc)++];
    return 1;
}

static int rd_i64(flint_vm *vm, uint32_t *pc, flint_word *out)
{
    uint32_t i;
    if (*pc + 8 > vm->code_size) { vm->err = FLINT_ERR_BADPC; return 0; }
    for (i = 0; i < 8; i++)
        ((flint_byte *)out)[i] = vm->code[(*pc) + i];
    *pc += 8;
    return 1;
}

static int chk_reg(flint_vm *vm, uint8_t r)
{
    if (r >= FLINT_NREGS) { vm->err = FLINT_ERR_BADREG; return 0; }
    return 1;
}

/* address-base operands accept a real register or the fp pseudo-register */
static int chk_base(flint_vm *vm, uint8_t r)
{
    if (r == FLINT_REG_FP) return 1;
    return chk_reg(vm, r);
}

static flint_word base_of(const flint_vm *vm, uint8_t r)
{
    return (r == FLINT_REG_FP) ? vm->fp : vm->r[r];
}

/* address helpers: base reg + byte offset against mem_size */
static int chk_addr(flint_vm *vm, flint_word base, flint_word off, uint32_t n)
{
    /* unsigned addition keeps the sum well-defined on wrap; the range
     * check below runs on the wrapped value */
    flint_word a = (flint_word)((uint64_t)base + (uint64_t)off);
    if (a < 0 || a > (flint_word)vm->mem_size - (flint_word)n) {
        vm->err = FLINT_ERR_BADMEM; return 0;
    }
    return 1;
}

static flint_word mem_load(flint_vm *vm, uint32_t addr, uint32_t n)
{
    /* byte-copy into the object: little-endian on every host */
    uint32_t i; flint_word v = 0;
    flint_byte *p = (flint_byte *)&v;
    for (i = 0; i < n; i++) p[i] = vm->mem[addr + i];
    return v;
}

static void mem_store(flint_vm *vm, uint32_t addr, uint32_t n, flint_word v)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        vm->mem[addr + i] = (flint_byte)(((uint64_t)v >> (8 * i)) & 0xFF);
}

/* ---- stack (shared data + return stack) ---- */

static int stk_push(flint_vm *vm, flint_word v)
{
    /* The stack shares mem with the heap and grows down. Refuse the push
     * before it can cross heap_ptr: alloc/enter already check this, but
     * push/call used to bypass the frontier and could overwrite a managed
     * block header (or a manual bump block). */
    if (vm->sp < 8 || vm->sp - 8 < vm->heap_ptr) {
        vm->err = FLINT_ERR_STACK;
        return 0;
    }
    vm->sp -= 8;
    mem_store(vm, vm->sp, 8, v);
    return 1;
}

static void stk_pop(flint_vm *vm, flint_word *v)
{
    /* compare in unsigned space so a tiny mem_size cannot wrap the bound */
    if (vm->sp > vm->mem_size || vm->mem_size - vm->sp < 8) {
        vm->err = FLINT_ERR_STACK; return;
    }
    *v = mem_load(vm, vm->sp, 8);
    vm->sp += 8;
}

/* ---- div/mod with a guard instead of CUB ---- */

static flint_word wa_div(flint_vm *vm, flint_word a, flint_word b)
{
    if (b == 0) { vm->err = FLINT_ERR_DIV0; return 0; }
    if (a == INT64_MIN && b == -1) return INT64_MIN;  /* quotient unrepresentable: pin, no SIGFPE */
    return a / b;
}

static flint_word wa_mod(flint_vm *vm, flint_word a, flint_word b)
{
    if (b == 0) { vm->err = FLINT_ERR_DIV0; return 0; }
    if (a == INT64_MIN && b == -1) return 0;          /* same pair: remainder is 0 */
    return a % b;
}

/* shift amounts are masked to 0..63 — defined behavior, no UB */
#define SHIFT_MASK 63

/* indexed by the public FLINT_ERR_* values (flint.h / README):
 * 1 NOSYS, 2 BADPC, 3 BADOP, 4 BADREG, 5 BADMEM, 6 STACK, 7 DIV0,
 * 8 LIMIT, 9 INIT, 10 OOM. Designated initialisers keep the table glued
 * to the enum even if a code is inserted later. */
static const char *const err_names[FLINT_ERR_OOM + 1] = {
    [FLINT_OK]         = "ok",
    [FLINT_ERR_NOSYS]  = "no syscall on that slot",
    [FLINT_ERR_BADPC]  = "pc out of bounds",
    [FLINT_ERR_BADOP]  = "unknown opcode",
    [FLINT_ERR_BADREG] = "register out of range",
    [FLINT_ERR_BADMEM] = "address out of bounds",
    [FLINT_ERR_STACK]  = "stack violation",
    [FLINT_ERR_DIV0]   = "division by zero",
    [FLINT_ERR_LIMIT]  = "step limit",
    [FLINT_ERR_INIT]   = "bad init arguments",
    [FLINT_ERR_OOM]    = "managed heap exhausted"
};

static void set_err(flint_vm *vm)
{
    /* first description wins: the table fills a fresh err_text, while the
     * collector's more specific corruption message survives the trap */
    if (vm->err_text == 0) {
        if (vm->err >= 0 && vm->err <= FLINT_ERR_OOM)
            vm->err_text = err_names[vm->err];
        else
            vm->err_text = "unknown error";
    }
}

const char *flint_errno(int err)
{
    if (err >= 0 && err <= FLINT_ERR_OOM) return err_names[err];
    return "unknown error";
}

/* ---- public API ---- */

void flint_init(flint_vm *vm,
                flint_byte *code, uint32_t code_size,
                flint_byte *mem,  uint32_t mem_size)
{
    uint32_t i;
    for (i = 0; i < FLINT_NREGS; i++) vm->r[i] = 0;
    for (i = 0; i < FLINT_MAX_SYS; i++) vm->sys[i] = 0;
    vm->code = code;    vm->code_size = code_size;
    vm->mem  = mem;     vm->mem_size  = mem_size;
    vm->pc = 0;         vm->sp = mem_size;
    vm->fp = (flint_word)mem_size;     /* no frame yet */
    vm->heap_ptr = 0;   vm->heap_start = 0;
    vm->mem_mode = FLINT_MEM_MANUAL;   /* default; flint_heap_set() selects */
    vm->steps = 0;      vm->max_steps = 0;
    vm->halted = 0;
    vm->err = FLINT_OK; vm->err_text = 0;
    {   /* collector state starts zeroed; flint_heap_set arms auto mode */
        unsigned char *gz = (unsigned char *)&vm->gc;
        for (i = 0; i < sizeof vm->gc; i++) gz[i] = 0;
    }
    if ((!vm->code || !vm->code_size) || (!vm->mem || !vm->mem_size)) {
        vm->err = FLINT_ERR_INIT;
        set_err(vm);
    }
}

void flint_sys_set(flint_vm *vm, int slot, flint_syscall fn)
{
    if (slot < 0 || slot >= FLINT_MAX_SYS) return;
    vm->sys[slot] = fn;
}

void flint_heap_set(flint_vm *vm, uint32_t heap_start, flint_mem_mode mode)
{
    /* the host picks the start (the CLI passes the assembler's aligned
     * data end); a start past the image would only make the first alloc
     * trap, so clamp instead of storing an impossible cursor */
    if (heap_start > vm->mem_size) heap_start = vm->mem_size;
    vm->heap_start = heap_start;
    vm->heap_ptr   = heap_start;
    /* normalize: the field always holds a valid mode */
    vm->mem_mode = (mode == FLINT_MEM_AUTO) ? FLINT_MEM_AUTO : FLINT_MEM_MANUAL;
    if (vm->mem_mode == FLINT_MEM_AUTO)
        flint_gc_init(vm);      /* align the base, zero the scratch area */
    else
        vm->gc.enabled = 0;
}

int flint_step(flint_vm *vm)
{
    uint32_t pc = vm->pc, next;
    uint8_t  op, rd, ra, rb, slot;
    flint_word a, b, off, target, t;

    if (vm->err) return vm->err;

    if (!rd_u8(vm, &pc, &op)) { set_err(vm); return vm->err; }

    switch (op) {

    case OP_INT:
        if (!rd_u8(vm, &pc, &slot)) break;
        if (slot >= FLINT_MAX_SYS || !vm->sys[slot]) {
            vm->err = FLINT_ERR_NOSYS; break;
        }
        next = pc;
        vm->pc = pc;
        vm->sys[slot](vm);
        if (vm->err) break;
        vm->pc = next;
        goto done;

    case OP_HLT:
        vm->halted = 1;
        vm->pc = pc;
        goto done;

    case OP_SET:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd)) break;
        if (!rd_i64(vm, &pc, &t)) break;
        vm->r[rd] = t; break;

    case OP_COPY:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra)) break;
        vm->r[rd] = vm->r[ra]; break;

    case OP_LOADB:
    case OP_LOADW: {
        uint32_t n = (op == OP_LOADB) ? 1 : 8;
        flint_word base;
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &rb) || !chk_base(vm, rb) ||
            !rd_i64(vm, &pc, &off)) break;
        base = base_of(vm, rb);
        if (!chk_addr(vm, base, off, n)) break;
        vm->r[rd] = mem_load(vm,
            (uint32_t)((uint64_t)base + (uint64_t)off), n);
        break;
    }

    /* indexed load/store: address = base + index (byte address, no scale) */
    case OP_LOADX: {
        flint_word base, idx;
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &rb) || !chk_base(vm, rb) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra)) break;
        base = base_of(vm, rb);
        idx  = vm->r[ra];
        if (!chk_addr(vm, base, idx, 8)) break;
        vm->r[rd] = mem_load(vm,
            (uint32_t)((uint64_t)base + (uint64_t)idx), 8);
        break;
    }

    case OP_STOREB:
    case OP_STOREW: {
        uint32_t n = (op == OP_STOREB) ? 1 : 8;
        flint_word base;
        if (!rd_u8(vm, &pc, &rb) || !chk_base(vm, rb) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_i64(vm, &pc, &off)) break;
        base = base_of(vm, rb);
        if (!chk_addr(vm, base, off, n)) break;
        mem_store(vm,
            (uint32_t)((uint64_t)base + (uint64_t)off), n, vm->r[ra]);
        break;
    }

    case OP_STOREX: {
        flint_word base, idx;
        if (!rd_u8(vm, &pc, &rb) || !chk_base(vm, rb) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd)) break;
        base = base_of(vm, rb);
        idx  = vm->r[ra];
        if (!chk_addr(vm, base, idx, 8)) break;
        mem_store(vm,
            (uint32_t)((uint64_t)base + (uint64_t)idx), 8, vm->r[rd]);
        break;
    }

    /* three-register arithmetic */
    case OP_ADD:  case OP_SUB:  case OP_MUL:
    case OP_AND:  case OP_OR:   case OP_XOR:
    case OP_SHL:  case OP_SHR:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_u8(vm, &pc, &rb) || !chk_reg(vm, rb)) break;
        a = vm->r[ra]; b = vm->r[rb];
        switch (op) {
        case OP_ADD:  t = (flint_word)((uint64_t)a + (uint64_t)b); break;
        case OP_SUB:  t = (flint_word)((uint64_t)a - (uint64_t)b); break;
        case OP_MUL:  t = (flint_word)((uint64_t)a * (uint64_t)b); break;
        case OP_AND:  t = a &  b; break;
        case OP_OR:   t = a |  b; break;
        case OP_XOR:  t = a ^  b; break;
        case OP_SHL:  t = (flint_word)((uint64_t)a << (SHIFT_MASK & b)); break;
        default:      t = (flint_word)((uint64_t)a >> (SHIFT_MASK & b)); break;
        }
        vm->r[rd] = t; break;

    case OP_DIV: case OP_MOD:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_u8(vm, &pc, &rb) || !chk_reg(vm, rb)) break;
        a = vm->r[ra]; b = vm->r[rb];
        vm->r[rd] = (op == OP_DIV) ? wa_div(vm, a, b) : wa_mod(vm, a, b);
        break;

    /* arithmetic with immediate */
    case OP_ADDI: case OP_SUBI: case OP_MULI:
    case OP_ANDI: case OP_ORI:  case OP_XORI:
    case OP_SHLI: case OP_SHRI:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_i64(vm, &pc, &b)) break;
        a = vm->r[ra];
        switch (op) {
        case OP_ADDI: t = (flint_word)((uint64_t)a + (uint64_t)b); break;
        case OP_SUBI: t = (flint_word)((uint64_t)a - (uint64_t)b); break;
        case OP_MULI: t = (flint_word)((uint64_t)a * (uint64_t)b); break;
        case OP_ANDI: t = a &  b; break;
        case OP_ORI:  t = a |  b; break;
        case OP_XORI: t = a ^  b; break;
        case OP_SHLI: t = (flint_word)((uint64_t)a << (SHIFT_MASK & b)); break;
        default:      t = (flint_word)((uint64_t)a >> (SHIFT_MASK & b)); break;
        }
        vm->r[rd] = t; break;

    case OP_DIVI: case OP_MODI:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_i64(vm, &pc, &b)) break;
        a = vm->r[ra];
        vm->r[rd] = (op == OP_DIVI) ? wa_div(vm, a, b) : wa_mod(vm, a, b);
        break;

    case OP_NEG:
    case OP_NOT:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd) ||
            !rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra)) break;
        vm->r[rd] = (op == OP_NEG)
                  ? (flint_word)(-(uint64_t)vm->r[ra])
                  : ~vm->r[ra];
        break;

    case OP_INC:
    case OP_DEC:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd)) break;
        vm->r[rd] = (op == OP_INC)
                  ? (flint_word)((uint64_t)vm->r[rd] + 1u)
                  : (flint_word)((uint64_t)vm->r[rd] - 1u);
        break;

    /* fused branches: absolute target */
    case OP_JEQ: case OP_JNE: case OP_JLT: case OP_JGE:
    case OP_JLO: case OP_JHS:
        if (!rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_u8(vm, &pc, &rb) || !chk_reg(vm, rb) ||
            !rd_i64(vm, &pc, &target)) break;
        a = vm->r[ra]; off = vm->r[rb];
        if (target < 0 || target > vm->code_size) { vm->err = FLINT_ERR_BADPC; break; }
        if ((op == OP_JEQ && a == off) || (op == OP_JNE && a != off) ||
            (op == OP_JLT && a <  off) || (op == OP_JGE && a >= off) ||
            (op == OP_JLO && (uint64_t)a <  (uint64_t)off) ||
            (op == OP_JHS && (uint64_t)a >= (uint64_t)off))
            pc = (uint32_t)target;
        break;

    case OP_JEQI: case OP_JNEI: case OP_JLTI: case OP_JGEI:
    case OP_JLOI: case OP_JHSI:
        if (!rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra) ||
            !rd_i64(vm, &pc, &off) || !rd_i64(vm, &pc, &target)) break;
        a = vm->r[ra];
        if (target < 0 || target > vm->code_size) { vm->err = FLINT_ERR_BADPC; break; }
        if ((op == OP_JEQI && a == off) || (op == OP_JNEI && a != off) ||
            (op == OP_JLTI && a <  off) || (op == OP_JGEI && a >= off) ||
            (op == OP_JLOI && (uint64_t)a <  (uint64_t)off) ||
            (op == OP_JHSI && (uint64_t)a >= (uint64_t)off))
            pc = (uint32_t)target;
        break;

    case OP_JMP:
        if (!rd_i64(vm, &pc, &target)) break;
        if (target < 0 || target > vm->code_size) { vm->err = FLINT_ERR_BADPC; break; }
        pc = (uint32_t)target; break;

    case OP_CALL:
        if (!rd_i64(vm, &pc, &target)) break;
        if (target < 0 || target > vm->code_size) { vm->err = FLINT_ERR_BADPC; break; }
        /* v0.2.0 frame: one word, the return pc. Locals live in an
         * ENTER/LEAVE frame below it; the heap cursor is never rewound. */
        if (!stk_push(vm, (flint_word)pc)) break;
        pc = (uint32_t)target; break;

    /* indirect transfers: the target is a register value, not a stream
     * field, so it is re-checked against code_size on every take — a
     * register can hold any 64-bit word at any time. pc == code_size is
     * refused too: there is no byte there to decode. */
    case OP_JMPR:
    case OP_CALLR:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd)) break;
        t = vm->r[rd];
        if (t < 0 || (uint64_t)t >= (uint64_t)vm->code_size) {
            vm->err = FLINT_ERR_BADPC; break;
        }
        if (op == OP_CALLR && !stk_push(vm, (flint_word)pc)) break;
        pc = (uint32_t)t; break;

    case OP_RET:
        stk_pop(vm, &t);
        if (vm->err) break;
        if (t < 0 || t > (flint_word)vm->code_size) { vm->err = FLINT_ERR_BADPC; break; }
        pc = (uint32_t)t;
        break;

    /* frame prologue: push old fp, set fp = sp, reserve size bytes.
     * locals are [fp-size, fp); the first 8-byte slot is at fp-8. */
    case OP_ENTER: {
        flint_word sz;
        if (!rd_i64(vm, &pc, &sz)) break;
        if (sz < 0 || (uint64_t)sz + 8u > (uint64_t)vm->sp) {
            vm->err = FLINT_ERR_STACK; break;
        }
        /* do not let the frame grow into the bump-allocator arena */
        if ((uint64_t)vm->sp - 8u - (uint64_t)sz < (uint64_t)vm->heap_ptr) {
            vm->err = FLINT_ERR_STACK; break;
        }
        if (!stk_push(vm, vm->fp)) break;
        vm->fp = (flint_word)vm->sp;
        vm->sp -= (uint32_t)sz;
        /* auto mode: zero the fresh locals so stale words cannot act as
         * conservative roots (manual keeps its historical behavior) */
        if (vm->mem_mode == FLINT_MEM_AUTO) {
            uint32_t z;
            for (z = vm->sp; z < (uint32_t)vm->fp; z++) vm->mem[z] = 0;
        }
        break;
    }

    case OP_LEAVE:
        if (vm->fp < 0 || (uint64_t)vm->fp > (uint64_t)vm->mem_size) {
            vm->err = FLINT_ERR_STACK; break;
        }
        vm->sp = (uint32_t)vm->fp;
        stk_pop(vm, &t);
        if (vm->err) break;
        vm->fp = t;
        break;

    case OP_PUSH:
        if (!rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra)) break;
        if (!stk_push(vm, vm->r[ra])) break;
        break;

    case OP_PUSHI:
        if (!rd_i64(vm, &pc, &t)) break;
        if (!stk_push(vm, t)) break;
        break;

    case OP_POP:
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd)) break;
        stk_pop(vm, &t);
        if (vm->err) break;
        vm->r[rd] = t; break;

    case OP_ALLOC: {
        flint_word sz;
        if (!rd_u8(vm, &pc, &rd) || !chk_reg(vm, rd)) break;
        if (!rd_i64(vm, &pc, &sz)) break;
        if (vm->mem_mode == FLINT_MEM_AUTO) {
            /* managed allocation: a collector-owned block payload,
             * zeroed; runs GC on threshold / failure, OOM if still no
             * space (FLINT_ERR_OOM) */
            if (sz < 0 || (uint64_t)sz > (uint64_t)vm->mem_size) {
                vm->err = (sz < 0) ? FLINT_ERR_BADMEM : FLINT_ERR_OOM;
                break;
            }
            t = flint_gc_alloc(vm, (uint32_t)sz);
            if (!t) break;                       /* trap set inside */
            vm->r[rd] = t;
            break;
        }
        /* manual: the original bump allocator. bounds: the block must
         * fit between the heap cursor and the stack pointer — mem_size
         * alone would let the up-growing heap silently overwrite live
         * stack frames (the stack grows down) */
        if (sz < 0 || (uint64_t)vm->heap_ptr + (uint64_t)sz > (uint64_t)vm->sp) {
            vm->err = FLINT_ERR_BADMEM; break;
        }
        vm->r[rd] = (flint_word)vm->heap_ptr;
        vm->heap_ptr += (uint32_t)sz;
        break;
    }

    case OP_FREE:
        if (!rd_u8(vm, &pc, &ra) || !chk_reg(vm, ra)) break;
        /* deliberate no-op in both modes: manual has no size metadata to
         * reclaim by; auto lifetime belongs to the collector (hosts may
         * use flint_gc_free for an explicit, validated release). The
         * operand is validated so malformed bytecode still traps. */
        break;

    case OP_GC:
        if (vm->mem_mode == FLINT_MEM_AUTO) flint_gc_collect(vm);
        break;

    default:
        vm->err = FLINT_ERR_BADOP; break;
    }

done:
    if (vm->err) set_err(vm);
    vm->pc = pc;
    vm->steps++;
    return vm->err;
}

flint_word flint_run(flint_vm *vm, uint32_t entry)
{
    vm->pc = entry;
    vm->halted = 0;
    while (!vm->halted) {
        if (vm->max_steps && vm->steps >= vm->max_steps) {
            vm->err = FLINT_ERR_LIMIT;
            set_err(vm);
            return -(flint_word)vm->err;
        }
        if (flint_step(vm)) return -(flint_word)vm->err;
    }
    return vm->r[0];
}
