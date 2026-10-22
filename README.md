# FLINT 0.1.0

A minimal assembly-style language + execution machine in **pure C99**.
Target use: an embeddable scripting backend and a standalone CLI host.
No dependencies, no global state, no flags register.

```
vm core:       ~45 opcodes, word = int64, 8 registers, flat memory
embed core:    include/flint.h + src/flint_core.c   (zero libc)
               + src/flint_asm.c                    (only if asm→bytecode is needed)
executable:    + src/flint_cli.c + src/main.c       (CLI host: syscalls over stdio)
```

---

## Machine model

| Property      | Value                                                        |
|---------------|--------------------------------------------------------------|
| Word size     | **int64 signed** (`flint_word`), little-endian               |
| Registers     | `r0..r7` — 8 general-purpose                                 |
| Memory        | flat byte image; a word is 8 consecutive bytes, no alignment required |
| Stack         | single (data + return addresses), grows down from the end of memory; `push`/`pop` move 8 bytes |
| Comparisons   | no flags register: comparison is *fused into branches* (MIPS/RISC-V lineage) |
| Syscalls      | host-provided table: up to 64 slots, `sys N` calls the host function; r0 carries the argument and the result |
| Steps         | `max_steps` — protection against runaway loops; `0` = unlimited |

Stack and data share one `mem` image:

```
[ data (from the start): strings/numbers/buffers — size padded up to 16 ]
[ ...stack...            (push/pop and CALL/RET frames grow down from mem_size) ]
```

The assembled memory image (from `flint_assemble`) is **zeroed** — `bytes N`
and everything past the data section reads as zeros until first written.

A program ends with `exit` (opcode `HLT`). The program's return code is **r0
at exit time**: in the CLI that is the process exit code (truncated to the low
31 bits: `720 → 208`); for an embedded host it is the return value of
`flint_run`. Convention: exit cleanly with `set r0, 0` right before `exit`.

---

## CLI

```
flint new <dir>          scaffold a project: Flint.toml + src/main.flint
flint run <file.flint>   run a single file
flint run [dir]          run a project (default: current directory)
flint check <file.flint> assemble without executing (syntax gate)
flint check [dir]        check a project
flint --version
```

Shortcut: `flint path/file.flint` ≡ `flint run path/file.flint`.

CLI exit codes:

| Code | Meaning                                            |
|------|----------------------------------------------------|
| 0    | program ran, r0 = 0                                |
| N    | program ran, r0 = N (N < 2^31)                     |
| −err | runtime trap: −9 = division by zero, −8 = step limit, −7 = stack overflow/underflow, −6 = bad memory address, −5 = bad register, −4 = unknown opcode, −3 = bad pc, −2 = syscall without a host handler |
| 1    | assembly error (source line and reason go to stderr) |
| 2    | file/project could not be read                      |

`check` prints `flint: <file>: ok, <N> code bytes` on success.

---

## Project (Flint.toml)

`flint new demo` creates:

```
demo/
├── Flint.toml
└── src/main.flint
```

```toml
[project]
name = "demo"
entry = "src/main.flint"
stack = 65536
```

Supported TOML subset: `# comments`, sections `[...]`, `key = value`;
string values are quoted. Keys:

| Key    | Default          | Meaning                                  |
|--------|------------------|------------------------------------------|
| `name` | directory name   | plain project name                        |
| `entry`| `src/main.flint` | path to the main program (relative to project root) |
| `stack`| `65536`          | extra memory bytes reserved for the stack |

Run from inside a project directory: `flint run` (no arguments).

---

## Language syntax

### Lexical layer

- Comment: `// ...` — to end of line.
- Numbers: decimal and `0x...`; signed literals (`-5`).
- Character literal: `'A'`; escapes in strings/characters: `\n \t \r \0 \\ \' \"`.
- Strings: `"..."` (one per data directive statement; no auto-concatenation).
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*` — labels and mnemonic names.
- Registers: `r0`…`r7` (strict; `r9` is an error).
- Registers and mnemonics are case-sensitive: `r0` is valid, `R0` is not.

### Labels

```
global labels     loop:  fact:  done:      — visible from anywhere
local labels      .print: .done            — attached to the last global label
```

Labels may be declared in `code:` (referring to a `pc` offset) and in `data:`
(referring to a memory offset). A label used as an immediate value yields its
*position*: a code label → bytecode offset; a data label → memory address.
Hence the canonical "take the data address — load the value" chain:

```
set r1, answer     // r1 = ADDRESS of answer (data label)
load r0, [r1]      // r0 = 42
```

A jump to a data label is rejected at assembly ("is not a code label").
Assembly is single-pass over the source plus a fixup pass, so forward
references and cross-references work freely.

### Sections and data

The default section is `code:`. Data objects live in `data:`.

```
data:
hello: str "Hello, FLINT!", 10, 0     // string bytes + tail (newline, NUL)
answer: int 42                        // 8-byte word
buf: bytes 16                         // 16 zero bytes (scratch)
```

Directives inside `data:`:

| Directive       | Byte footprint                  |
|-----------------|---------------------------------|
| `str "..."`     | string contents as bytes        |
| `int N`         | 8 bytes, int64 LE               |
| `bytes N`       | N zero bytes (scratch)          |

All three accept comma-separated lists: `str "ab", 13, 10, 0` and so on.

### Memory operands

```
[r2]        // base address in a register
[r3+4]      // base + offset
[r3-8]      // base − offset
```

The store form is **address first**:

```
store [r3+4], r0          // word (8 bytes)
store.byte [r2], r1       // one byte
```

### Program entry

The entry point is the mandatory `main` label in the `code:` section
(a missing `main` is an assembly error).

---

## Instruction set

Notation: `imm` = immutable literal (a number, `'c'`, or a label).
Wherever `rb|imm` is shown, the second operand accepts either a register or
an immediate. The short arithmetic form `add rd, imm` means `rd = rd + imm`.

### Data movement

| Form                         | Bytes | Semantics                                    |
|------------------------------|-------|----------------------------------------------|
| `set rd, imm`                | 10    | load a constant or label into rd             |
| `copy rd, rs`                | 3     | rd = rs                                      |
| `load rd, [rb]`              | 11    | rd = int64 LE read at [rb]                   |
| `load rd, [rb±off]`          | 11    | same with an offset                          |
| `load.byte rd, [rb]`         | 11    | one byte, zero-extended                      |
| `store [rb±off], rs`         | 11    | full word write at the address               |
| `store.byte [rb±off], rs`    | 11    | one-byte write at the address                |

### Arithmetic and logic (3-address + shortimm)

| Form                             | Bytes | Semantics                 |
|----------------------------------|-------|---------------------------|
| `add rd, ra, rb\|imm`            | 4 / 11 | rd = ra + rb            |
| `sub rd, ra, rb\|imm`            | 4 / 11 | rd = ra − rb            |
| `mul rd, ra, rb\|imm`            | 4 / 11 | rd = ra * rb            |
| `div rd, ra, rb\|imm`            | 4 / 11 | integer division; divide-by-zero traps |
| `mod rd, ra, rb\|imm`            | 4 / 11 | remainder; divide-by-zero traps |
| `and / or / xor rd, ra, rb\|imm` | 4 / 11 | bitwise                 |
| `shl rd, ra, rb\|imm`            | 4 / 11 | rd = ra << (rb & 63)    |
| `shr rd, ra, rb\|imm`            | 4 / 11 | rd = ra >> (rb & 63)  (arithmetic, sign kept) |
| `neg rd, rs`                     | 3     | rd = −rs                 |
| `not rd, rs`                     | 3     | rd = ~rs (bitwise NOT)   |
| `inc rd` / `dec rd`              | 2     | rd += 1 / rd −= 1        |

```
add r0, r0, r1        // full form
add r0, 5             // short form: rd + imm  (ra = rd)
```

### Control flow: branches (comparison fused in)

Full three-operand form:

```
jump.eq rs, rt\|imm, target      // if rs == rt|imm → pc = target
jump.ne  ...                     // rs != rt
jump.lt  ...                     // rs <  rt    (signed)
jump.ge  ...                     // rs >= rt   (signed)
```

Short form — compare against zero (the common case):

```
jump.eq r1, done                 // JEQI r1, 0, done
```

| Form                      | Bytes |
|---------------------------|-------|
| `jump.xx rs, rt, target`  | 11    |
| `jump.xx rs, imm, target` | 18    |
| `jump.xx rs, target`      | 18    (sugar for `rs ?= 0`) |

Unconditional jump and procedures:

| Form           | Bytes | Semantics                                       |
|----------------|-------|-------------------------------------------------|
| `jump label`   | 9     | pc = label (unconditional)                      |
| `call label`   | 9     | next pc → stack; pc = label                     |
| `return`       | 1     | pc = stack (return address)                     |
| `push rs`      | 2     | sp −= 8; mem[sp] = rs                           |
| `push imm`     | 9     | immediate onto the stack                        |
| `pop rd`       | 2     | rd = mem[sp]; sp += 8                           |

Recursion — `call`/`return` share the same stack as `push`/`pop`:
the factorial example (depth × 16 bytes per frame) runs as-is.

### Platform

| Form        | Bytes | Semantics                                       |
|-------------|-------|-------------------------------------------------|
| `sys N`     | 2     | host function of slot N; r0 — input and result. A slot with no host handler traps |
| `exit`      | 1     | end the program; r0 — return code               |

---

## Syscalls (included CLI host)

Convention: arguments pass in `r1..r3`; `r0` is always the result slot (an
argument in `r0` is consumed before the result lands there). Memory arguments
point into the flat `mem` image — the host validates every span against
image bounds. Tables: handles are 1-based (0 is not a handle).

| Slot | Name   | Args (r1..r3) | Result                         |
|------|--------|---------------|--------------------------------|
| 0    | print  | r0 = number   | print r0 as text + newline     |
| 1    | putc   | r0 = character | print the low byte            |
| 2    | getc   | —             | r0 = character or −1 (EOF)     |
| 3    | regs   | —             | print r0..r7, one per line     |
| 4    | fopen  | r1 = mode (0 read / 1 write / 2 append), r2 = NUL-terminated path in `mem` | r0 = handle or −1 |
| 5    | fclose | r1 = handle   | r0 = 0 or −1                   |
| 6    | fread  | r1 = handle, r2 = buffer in `mem`, r3 = max bytes | r0 = bytes read, 0 on EOF, −1 on error |
| 7    | fwrite | r1 = handle, r2 = buffer in `mem`, r3 = bytes | r0 = bytes written or −1 |
| 8    | time   | —             | r0 = UNIX epoch seconds        |

Slots 9..63 stay free for embedding hosts to define. Calling an unfilled
slot is a predictable `NOSYS` trap.

Common recipes:

```
sys 0           // print the result/counter              (r0 carries it)
sys 1           // byte-by-byte printing of a data string
sys 2           // manual stdin reading, one character at a time
sys 3           // debug: dump registers mid-run
sys 4/6/7       // file round-trip: fopen(path) -> fread/fwrite(buf, n) -> fclose
```

---

## Embedding (C API)

The core pulls in no libc, holds no global state, and runs entirely out of
host-provided memory. All mutable state lives in the struct.

### The full chain

```c
#include "flint.h"

flint_prog p;
char err[384];
if (flint_assemble(source_text, &p, 65536, err, sizeof err) != 0) {
    // err: "line N: <what is wrong>"
}

flint_vm vm;
flint_init(&vm, p.code, p.code_size, p.mem, p.mem_size);
flint_word rc = flint_run(&vm, p.entry);          // r0, or −(FLINT_ERR_*)
flint_prog_free(&p);
```

### Custom syscalls

```c
static void my_double(flint_vm *vm)   /* sys 10: r0 = r0 * 2 */
{
    vm->r[0] *= 2;
}

/* before the run: */
flint_sys_set(&vm, 10, my_double);
```

### Step-by-step mode

```c
vm.max_steps = 1000000;                /* escape hatch against infinite loops */
flint_run(...);                        /* err == FLINT_ERR_LIMIT on runaway loops */
```

`flint_step(&vm)` — exactly one instruction per call — for
debuggers/tracers.

### Errors

```c
switch (vm.err) {
case FLINT_ERR_NOSYS:  ... case FLINT_ERR_BADPC: ...
case FLINT_ERR_BADOP:  ... case FLINT_ERR_BADREG: ...
case FLINT_ERR_BADMEM: ... case FLINT_ERR_STACK: ...
case FLINT_ERR_DIV0:   ... case FLINT_ERR_LIMIT: ...
}
const char *txt = vm.err_text;         /* human-readable line */
```

---

## Building

```
make          # build/flint
make test     # full bench: core (7) + asm (16) + examples
make clean
```

Embedding is minimal: `flint_core.c` (+ `flint_asm.c` if you want source
assembly) is one TU each; build with `-I include`.

---

## Examples

```
tests/hello.flint   Hello, FLINT!                       (byte-wise string printing)
tests/sum.flint     55                                  (sum 1..10)
tests/fact.flint    720                                 (recursion: fact(6))
tests/off.flint     119                                 (store/load with an offset)
tests/fio.flint     42                                  (file write→read round-trip)
tests/regs.flint    r0 = 7 … r7 = 0                     (register dump)
```

Run one: `flint run tests/sum.flint` — or all of them: `make test`.
