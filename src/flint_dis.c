/*
 * flint_dis.c — bytecode disassembler for the FLINT CLI.
 *
 * Decodes one instruction per line. Field widths mirror the ISA layout
 * documented in flint.h (and enforced by flint_core.c):
 *   INT slot(2) | HLT(1) | SET rd,imm(10) | COPY rd,rs(3)
 *   LOADB/W rd,rb,off(11) | STOREB/W rb,rs,off(11)
 *   ADD..SHR rd,ra,rb(4) | ADDI..SHRI rd,ra,imm(11)
 *   NEG/NOT(3) | INC/DEC(2)
 *   JEQ..JHS rs,rt,tgt(11) | JxxI rs,imm,tgt(18)
 *   JMP/CALL tgt(9) | RET(1) | PUSH rs(2) | PUSHI imm(9) | POP rd(2)
 *
 * Unknown opcode or truncated tail: prints what it reached and stops
 * with a nonzero return — a corrupt stream is never guessed past.
 */
#include <stdio.h>
#include <string.h>
#include "flint.h"

static const char *names[OP_COUNT] = {
    "int",  "hlt",
    "set",  "copy",
    "loadb",  "loadw",  "storeb",  "storew",
    "add",  "addi", "sub",  "subi", "mul",  "muli",
    "div",  "divi", "mod",  "modi",
    "and",  "andi", "or",   "ori",  "xor",  "xori",
    "shl",  "shli", "shr",  "shri",
    "neg",  "not",  "inc",  "dec",
    "jeq",  "jeqi", "jne",  "jnei", "jlt",  "jlti", "jge", "jgei",
    "jlo",  "jloi", "jhs",  "jhsi",
    "jmp",  "call", "ret",
    "push", "pushi", "pop"
};

static int op_len(flint_byte op)
{
    switch (op) {
    case OP_INT: case OP_PUSH: case OP_POP: case OP_INC: case OP_DEC:
        return 2;
    case OP_COPY: case OP_NEG: case OP_NOT:
        return 3;
    case OP_SET:
        return 10;
    case OP_HLT: case OP_RET:
        return 1;
    case OP_JMP: case OP_CALL: case OP_PUSHI:
        return 9;
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

#define P(...)  printf(__VA_ARGS__)

int flint_dis(const flint_byte *code, uint32_t n)
{
    uint32_t pc = 0;
    while (pc < n) {
        int64_t imm = 0, tgt = 0;
        int ra, rb, rd;
        uint32_t at = pc;
        flint_byte op = code[pc++];
        int len = op_len(op);

        if (len == 0 || op >= OP_COUNT) {
            fprintf(stderr, "dis: bad opcode 0x%02X at 0x%04X\n", op, at);
            return 1;
        }
        if (at + (uint32_t)len > n) {
            fprintf(stderr, "dis: truncated tail at 0x%04X (needs %d)\n", at, len);
            return 1;
        }
        if (pc > 0xFFFF) break;    /* images beyond 64K print with wide offsets */

        P("0x%04X  ", at);
        switch (op) {
        case OP_INT:   P("int   %u", (unsigned)code[pc]); pc += 1; break;
        case OP_HLT:   P("hlt");           break;
        case OP_RET:   P("ret");           break;
        case OP_SET:
            rd = code[pc++];
            rd_i64(code, &pc, &imm, n);
            P("set   r%d, %lld", rd, (long long)imm); break;
        case OP_COPY:
            rd = code[pc++]; ra = code[pc++];
            P("copy  r%d, r%d", rd, ra); break;
        case OP_LOADB: case OP_LOADW:
            rd = code[pc++]; rb = code[pc++];
            rd_i64(code, &pc, &imm, n);
            P("%s r%d, [r%d%lld]", op == OP_LOADB ? "load.byte" : "load ",
              rd, rb, (long long)imm);
            break;
        case OP_STOREB: case OP_STOREW:
            rb = code[pc++]; rd = code[pc++];
            rd_i64(code, &pc, &imm, n);
            P("%s [r%d%lld], r%d", op == OP_STOREB ? "store.byte" : "store",
              rb, (long long)imm, rd);
            break;
        case OP_NEG: case OP_NOT:
            rd = code[pc++]; ra = code[pc++];
            P("%s r%d, r%d", names[op], rd, ra); break;
        case OP_INC: case OP_DEC:
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
            ra = code[pc++]; P("push  r%d", ra); break;
        case OP_PUSHI:
            rd_i64(code, &pc, &imm, n);
            P("pushi %lld", (long long)imm); break;
        case OP_POP:
            rd = code[pc++]; P("pop   r%d", rd); break;
        default:
            fprintf(stderr, "dis: unhandled opcode %u\n", (unsigned)op);
            return 1;
        }
        P("\n");
    }
    return 0;
}
