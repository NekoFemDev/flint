/*
 * flint_dis.c — bytecode disassembler for the FLINT CLI.
 *
 * Decodes one instruction per line. Field widths mirror the ISA layout
 * documented in flint.h (and enforced by flint_core.c):
 *   INT slot(2) | HLT(1) | SET rd,imm(10) | COPY rd,rs(3)
 *   LOADB/W rd,rb,off(11) | LOADX rd,rb,ri(4)
 *   STOREB/W rb,rs,off(11) | STOREX rb,ri,rs(4)
 *   ADD..SHR rd,ra,rb(4) | ADDI..SHRI rd,ra,imm(11)
 *   NEG/NOT(3) | INC/DEC(2)
 *   JEQ..JHS rs,rt,tgt(11) | JxxI rs,imm,tgt(18)
 *   JMP/CALL tgt(9) | RET(1) | ENTER size(10) | LEAVE(1)
 *   PUSH rs(2) | PUSHI imm(9) | POP rd(2)
 *   ALLOC rd,size(10) | FREE rs(2)
 *
 * Unknown opcode or truncated tail: prints what it reached and stops
 * with a nonzero return — a corrupt stream is never guessed past.
 */
#include <stdio.h>
#include <string.h>
#include "flint.h"

/* assembler-surface names: immediate forms share the base mnemonic
 * (OP_ADDI prints as "add rd, ra, imm"), OP_HLT prints as "exit".
 * Designated initialisers keep this glued to the enum. */
static const char *const names[OP_COUNT] = {
    [OP_INT]     = "sys",
    [OP_HLT]     = "exit",
    [OP_SET]     = "set",
    [OP_COPY]    = "copy",
    [OP_LOADB]   = "load.byte",
    [OP_LOADW]   = "load",
    [OP_LOADX]   = "loadx",
    [OP_STOREB]  = "store.byte",
    [OP_STOREW]  = "store",
    [OP_STOREX]  = "storx",
    [OP_ADD]     = "add",   [OP_ADDI] = "add",
    [OP_SUB]     = "sub",   [OP_SUBI] = "sub",
    [OP_MUL]     = "mul",   [OP_MULI] = "mul",
    [OP_DIV]     = "div",   [OP_DIVI] = "div",
    [OP_MOD]     = "mod",   [OP_MODI] = "mod",
    [OP_AND]     = "and",   [OP_ANDI] = "and",
    [OP_OR]      = "or",    [OP_ORI]  = "or",
    [OP_XOR]     = "xor",   [OP_XORI] = "xor",
    [OP_SHL]     = "shl",   [OP_SHLI] = "shl",
    [OP_SHR]     = "shr",   [OP_SHRI] = "shr",
    [OP_NEG]     = "neg",
    [OP_NOT]     = "not",
    [OP_INC]     = "inc",
    [OP_DEC]     = "dec",
    [OP_JEQ]     = "jump.eq", [OP_JEQI] = "jump.eq",
    [OP_JNE]     = "jump.ne", [OP_JNEI] = "jump.ne",
    [OP_JLT]     = "jump.lt", [OP_JLTI] = "jump.lt",
    [OP_JGE]     = "jump.ge", [OP_JGEI] = "jump.ge",
    [OP_JLO]     = "jump.lo", [OP_JLOI] = "jump.lo",
    [OP_JHS]     = "jump.hs", [OP_JHSI] = "jump.hs",
    [OP_JMP]     = "jump",
    [OP_CALL]    = "call",
    [OP_RET]     = "return",
    [OP_ENTER]   = "enter",
    [OP_LEAVE]   = "leave",
    [OP_PUSH]    = "push",  [OP_PUSHI] = "push",
    [OP_POP]     = "pop",
    [OP_ALLOC]   = "alloc",
    [OP_FREE]    = "free"
};

static int op_len(flint_byte op)
{
    switch (op) {
    case OP_INT: case OP_PUSH: case OP_POP: case OP_INC: case OP_DEC:
    case OP_FREE:
        return 2;
    case OP_COPY: case OP_NEG: case OP_NOT:
        return 3;
    case OP_SET: case OP_ALLOC:
        return 10;
    case OP_HLT: case OP_RET: case OP_LEAVE:
        return 1;
    case OP_JMP: case OP_CALL: case OP_PUSHI:
        return 9;
    case OP_ENTER:
        return 10;
    case OP_LOADX: case OP_STOREX:
        return 4;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_AND: case OP_OR: case OP_XOR:
    case OP_SHL: case OP_SHR:
        return 4;
    case OP_ADDI: case OP_SUBI: case OP_MULI: case OP_DIVI: case OP_MODI:
    case OP_ANDI: case OP_ORI: case OP_XORI:
    case OP_SHLI: case OP_SHRI:
        return 11;
    case OP_LOADB: case OP_LOADW: case OP_STOREB: case OP_STOREW:
    case OP_JEQ: case OP_JNE: case OP_JLT: case OP_JGE:
    case OP_JLO: case OP_JHS:
        return 11;
    case OP_JEQI: case OP_JNEI: case OP_JLTI: case OP_JGEI:
    case OP_JLOI: case OP_JHSI:
        return 18;
    default:
        return 0;
    }
}

static int rd_i64(const flint_byte *c, uint32_t *pc, int64_t *out, uint32_t n)
{
    int i; uint64_t v = 0;
    if (*pc + 8 > n) return 0;
    for (i = 7; i >= 0; i--) v = (v << 8) | c[*pc + i];
    *pc += 8; *out = (int64_t)v;
    return 1;
}

/* rN or the fp pseudo-register */
static const char *base_name(int b, char *buf, size_t n)
{
    if (b == FLINT_REG_FP) return "fp";
    snprintf(buf, n, "r%d", b);
    return buf;
}

#define P(...)  printf(__VA_ARGS__)

int flint_dis(const flint_byte *code, uint32_t n)
{
    uint32_t pc = 0;
    while (pc < n) {
        int64_t imm = 0, tgt = 0;
        int ra, rb, rd;
        char b1[16];
        uint32_t at = pc;
        flint_byte op = code[pc++];
        int len = op_len(op);

        if (len == 0 || op >= OP_COUNT || !names[op]) {
            fprintf(stderr, "dis: bad opcode 0x%02X at 0x%04X\n", op, at);
            return 1;
        }
        if (at + (uint32_t)len > n) {
            fprintf(stderr, "dis: truncated tail at 0x%04X (needs %d)\n", at, len);
            return 1;
        }

        P("0x%04X  ", at);
        switch (op) {
        case OP_INT:   P("%s %u", names[op], (unsigned)code[pc]); pc += 1; break;
        case OP_HLT:   P("%s", names[op]); break;
        case OP_RET:   P("%s", names[op]); break;
        case OP_LEAVE: P("%s", names[op]); break;
        case OP_SET:
        case OP_ALLOC:
            rd = code[pc++];
            rd_i64(code, &pc, &imm, n);
            P("%s r%d, %lld", names[op], rd, (long long)imm); break;
        case OP_ENTER:
            rd_i64(code, &pc, &imm, n);
            P("enter %lld", (long long)imm); break;
        case OP_COPY:
            rd = code[pc++]; ra = code[pc++];
            P("%s r%d, r%d", names[op], rd, ra); break;
        case OP_LOADB: case OP_LOADW:
            rd = code[pc++]; rb = code[pc++];
            rd_i64(code, &pc, &imm, n);
            P("%s r%d, [%s%+lld]", names[op], rd,
              base_name(rb, b1, sizeof b1), (long long)imm);
            break;
        case OP_LOADX:
            rd = code[pc++]; rb = code[pc++]; ra = code[pc++];
            P("%s r%d, [%s+r%d]", names[op], rd,
              base_name(rb, b1, sizeof b1), ra);
            break;
        case OP_STOREB: case OP_STOREW:
            rb = code[pc++]; rd = code[pc++];
            rd_i64(code, &pc, &imm, n);
            P("%s [%s%+lld], r%d", names[op],
              base_name(rb, b1, sizeof b1), (long long)imm, rd);
            break;
        case OP_STOREX:
            rb = code[pc++]; ra = code[pc++]; rd = code[pc++];
            P("%s [%s+r%d], r%d", names[op],
              base_name(rb, b1, sizeof b1), ra, rd);
            break;
        case OP_NEG: case OP_NOT:
            rd = code[pc++]; ra = code[pc++];
            P("%s r%d, r%d", names[op], rd, ra); break;
        case OP_INC: case OP_DEC: case OP_FREE:
            rd = code[pc++];
            P("%s r%d", names[op], rd); break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_MOD: case OP_AND: case OP_OR: case OP_XOR:
        case OP_SHL: case OP_SHR:
            rd = code[pc++]; ra = code[pc++]; rb = code[pc++];
            P("%s r%d, r%d, r%d", names[op], rd, ra, rb); break;
        case OP_ADDI: case OP_SUBI: case OP_MULI: case OP_DIVI:
        case OP_MODI: case OP_ANDI: case OP_ORI: case OP_XORI:
        case OP_SHLI: case OP_SHRI:
            rd = code[pc++]; ra = code[pc++];
            rd_i64(code, &pc, &imm, n);
            P("%s r%d, r%d, %lld", names[op], rd, ra, (long long)imm);
            break;
        case OP_JEQ: case OP_JNE: case OP_JLT: case OP_JGE:
        case OP_JLO: case OP_JHS:
            ra = code[pc++]; rb = code[pc++];
            rd_i64(code, &pc, &tgt, n);
            P("%s r%d, r%d, 0x%04X", names[op], ra, rb, (unsigned)tgt);
            break;
        case OP_JEQI: case OP_JNEI: case OP_JLTI: case OP_JGEI:
        case OP_JLOI: case OP_JHSI:
            ra = code[pc++];
            rd_i64(code, &pc, &imm, n);
            rd_i64(code, &pc, &tgt, n);
            P("%s r%d, %lld, 0x%04X", names[op], ra, (long long)imm, (unsigned)tgt);
            break;
        case OP_JMP: case OP_CALL:
            rd_i64(code, &pc, &tgt, n);
            P("%s 0x%04X", names[op], (unsigned)tgt); break;
        case OP_PUSH:
            ra = code[pc++]; P("%s r%d", names[op], ra); break;
        case OP_PUSHI:
            rd_i64(code, &pc, &imm, n);
            P("%s %lld", names[op], (long long)imm); break;
        case OP_POP:
            rd = code[pc++]; P("%s r%d", names[op], rd); break;
        default:
            fprintf(stderr, "dis: unhandled opcode %u\n", (unsigned)op);
            return 1;
        }
        P("\n");
    }
    return 0;
}
