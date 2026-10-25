# FLINT 0.4.0-pre.7

A minimal assembly-style language + execution machine in **pure C99**.
Target use: an embeddable scripting backend and a standalone CLI host.
No dependencies, no global state, no flags register.

Release history: [CHANGELOG.md](CHANGELOG.md).

```
vm core:       ~56 opcodes, word = int64, 16 registers, flat memory
embed core:    include/flint.h + src/flint_core.c + src/flint_gc.c
               (zero libc; flint_gc.c is the auto-mode mark-sweep collector)
               + src/flint_asm.c                    (only if asm→bytecode is needed)
executable:    + src/flint_cli.c + src/main.c       (CLI host: stdio + POSIX file syscalls)
               + src/flint_img.c                    (host-side: .fbc image container)
```

---

## Machine model

| Property      | Value                                                        |
|---------------|--------------------------------------------------------------|
| Word size     | **int64 signed** (`flint_word`), little-endian               |
| Registers     | `r0..r15` — 16 general-purpose; ABI: `r0..r3` args/result, `r4..r11` caller-saved, `r12..r15` callee-saved |
| Memory        | flat byte image; a word is 8 consecutive bytes, no alignment required |
| Stack         | single (locals + return addresses), grows down from the end of memory; `push`/`pop` move 8 bytes; a CALL frame is one word (return pc); every stack write is checked against `heap_ptr` and traps `FLINT_ERR_STACK` instead of overwriting the heap; `enter N`/`leave` build a local frame ([saved fp] + N local bytes) |
| Comparisons   | no flags register: comparison is *fused into branches* (MIPS/RISC-V lineage) |
| Syscalls      | host-provided table: up to 64 slots, `sys N` calls the host function; arguments/result travel in r0..r3 |
| Steps         | `max_steps` — protection against runaway loops; `0` = unlimited |

Stack, data and heap share one `mem` image:

```
[ data (from the start): strings/numbers/buffers — size padded up to 16 ]
[ heap: alloc grows a cursor from the aligned end of the data             ]
[ ...stack...            (push/pop and CALL/RET frames grow down from mem_size) ]
```

The assembled memory image (from `flint_assemble`) is **zeroed** — `bytes N`
and everything past the data section reads as zeros until first written.

The allocator is selected by the project memory model (a real switch
since v0.3.0; it also travels inside `.fbc` images and standalone ELFs):

- **manual** — `alloc` is a bump allocator: the cursor starts at the
  aligned end of the data section and grows up; blocks are never reclaimed
  automatically (`free` is a validated no-op) and a `return` never
  rewinds the cursor.
- **auto** — `alloc` is served by a bundled **conservative, non-moving
  mark-sweep collector**. Every block carries a 16-byte header before
  its payload; `alloc` returns the zeroed payload. A block stays valid
  while a register, the active stack, the data section or a host
  root/pin reaches it (interior pointers count); unreachable blocks —
  **cycles included** — are reclaimed whenever the collector runs: on a
  byte threshold (default 32 KiB), on allocation failure, at an explicit
  `gc`, or from the host. In auto mode `enter N` zeroes the fresh frame
  and the VM zeroes the scratch region at startup so conservative
  scanning starts clean.

A program ends with `exit` (opcode `HLT`). The program's return code is **r0
at exit time**. `flint_run` returns r0 to an embedded host. In the CLI, r0 is
first narrowed to a C `int` with `r0 & 0x7FFFFFFF`; the operating system then
reports only the low 8 bits of that process status, so `r0 = 720` appears as
`208` in the shell (`720 & 0xFF`). Convention: exit cleanly with `set r0, 0`
right before `exit`.

---

## CLI

```
flint new <dir> [--memory auto|manual]   scaffold a project (prompts if omitted)
flint build <file.flt|dir> [-o out] [--elf]  build .fbc image / standalone ELF
flint run <file.flt|file.fbc>            run a source file or a built image
flint run [dir]                          run a project (default: current directory)
flint run [<arg>] [--gc-stats] [--gc-stress] [--memory auto|manual]
flint run <file.flt|file.fbc> --threads N    N VM instances in N OS threads
flint check <file.flt|file.fbc>          assemble/validate without executing
flint check [dir]                        check a project
flint dis <file.flt|file.fbc>            print bytecode disassembly
flint repl                               interactive REPL (state persists)
flint --version
```

Shortcut: `flint path/file.flt` ≡ `flint run path/file.flt` (same for `.fbc`).

### REPL

`flint repl` opens an interactive session: one statement per line,
assembled and run on a live VM whose registers, memory and heap persist
between lines. `sys 11` (regs) dumps the registers; `exit` alone, or
EOF, closes the session. Each line is wrapped into a little program —
the REPL supplies `main:` and a trailing `exit` when the line brings
neither — so labels resolve only within their line, and a line that
never halts is stopped at 1000 steps:

```
$ flint repl
> set r0, 42
> add r0, r0, 1
> sys 11
r0 = 43
r1 = 0
...
> exit
```

`run` options: `--gc-stats` prints a collector report to stderr after the
run; `--gc-stress` forces a collection before every allocation;
`--memory auto|manual` overrides the project model for a single file or
directory (and a `.fbc` image's stored model). The environment variables
`FLINT_GC_STRESS=1` and `FLINT_GC_STATS=1` do the same without flags —
the standalone ELF stub honors them too.

`--threads N` runs N isolated VM instances of one source file or `.fbc`
image, each in its own OS thread: every thread reads (or assembles) its
own copy, so code, memory and stack are fully isolated — only stdout is
shared, so output may interleave at print granularity. Not combinable
with the GC flags; the exit code is 0 when every thread finished with
`r0 = 0`, otherwise the first non-zero/trapped thread's code. Embedders
can call `flint_run_parallel(filename, num_threads, stack_size)`
directly.

Arguments after the file or project (`flint run prog.flt a b`) are
program arguments: `sys 12` returns the count, `sys 13` copies one into
a buffer, `sys 14` looks up an environment variable. `--threads` runs
take no program arguments; standalone ELFs (`flint build --elf`) expose
their own process arguments the same way.

CLI exit codes:

| Code | Meaning                                            |
|------|----------------------------------------------------|
| 0    | program ran, r0 = 0                                |
| N    | program ran, r0 = N (N < 2^31; the shell observes N & 0xFF) |
| −err | runtime trap; `err` is a `FLINT_ERR_*` code: −1 NOSYS, −2 BADPC, −3 BADOP, −4 BADREG, −5 BADMEM, −6 STACK, −7 DIV0, −8 LIMIT, −9 INIT, −10 OOM (managed heap exhausted) |
| 1    | assembly error (source line and reason go to stderr) |
| 2    | file/project could not be read                      |

`check` prints `flint: <file>: ok, <N> code bytes` on success (`check` of
a `.fbc` image also reports the mem size and memory model).

`dis` assembles and prints the bytecode back as text — sugar resolves into
its emitted form (expert names, absolute targets), so a branch can 
be seen exactly as the interpreter will run it:

```
$ flint dis demo/src/main.flt
; 63 code bytes, entry 0x0000
0x0000  set   r2, 0
0x000A  load.byte r1, [r2+0]
0x0015  jeqi r1, 0, 0x0037
...
```

### Bytecode images (.fbc)

`flint build` resolves `include`s, assembles the program and packs the
result into one binary — the only artifact needed to run it. Status output
is Cargo-style: the include tree it resolved, then the artifact with its
size and build time (colored on a terminal; `NO_COLOR` disables that):

```
$ flint build examples/bench_fact
   Compiling bench_fact [auto]
   src/main.flt
   └── lib/fib.flt
    Finished build/bench_fact.fbc (64.2 KiB) in 1ms
$ flint run build/bench_fact.fbc
bench_fact: ok
```

Default output is `build/<name>.fbc` (`<name>` from Flint.toml, or the
file stem for a single-file build); `-o` picks another path. The image is
self-contained: no assembler, no sources, no `Flint.toml` at run time.
The container is versioned and checksummed (FNV-1a 32 over the code
bytes, then the mem bytes); the loader rejects wrong versions, truncated
payloads, trailing data and checksum mismatches. Header layout, all
fields little-endian u32:

| Offset | Field                              |
|--------|------------------------------------|
| 0      | magic `"FLBC"`                     |
| 4      | version (1)                        |
| 8      | code_size                          |
| 12     | mem_size                           |
| 16     | entry (main's code offset)         |
| 20     | heap_start                         |
| 24     | mem_mode (0 manual / 1 auto — auto runs on the collector) |
| 28     | checksum (FNV-1a over code, mem)   |
| 32     | code image, then mem image         |

Embedding hosts can read/write images with `include/flint_img.h`
(`flint_img_save` / `flint_img_load`, 0/-1 with an error string).

### Standalone executables (--elf)

`flint build --elf` fuses the stub with the assembled program into one
executable at `target/<name>` — `-o` picks another path. `make` builds the
stub first (`build/stub.elf`); a missing stub is a build error:

```
$ flint build --elf examples/b64-demo
   Compiling b64-demo [manual]
   src/main.flt
    Finished target/b64-demo (145.0 KiB) in 1ms
$ printf 'foobar' | ./target/b64-demo
Zm9vYmFy
```

The file is the stub followed by the code and mem images and a fixed
trailer (`struct FlintPayload` in `include/flint_stub.h`: magic `"FLNT"`,
code_size, mem_size, entry_pc, heap_start, mem_mode; all little-endian
u32). At run time the stub re-reads its own file through `/proc/self/exe`,
loads the images and runs them with the standard syscall table — no flint,
no sources, no `.fbc` on the target machine. The stub links statically
where the toolchain ships a static libc (glibc/musl); Termux's bionic has
none, so it falls back to `-static-libgcc` + the system libc
(`make STUB_LDFLAGS=...` forces a choice).

---

## Project (Flint.toml)

`flint new demo` (add `--memory auto|manual`, or answer the prompt)
creates:

```
demo/
├── Flint.toml
└── src/main.flt
```

```toml
[project]
name = "demo"
entry = "src/main.flt"
stack = 65536
memory = "manual"
```

Supported TOML subset: `# comments`, sections `[...]`, `key = value`;
string values are quoted. Keys:

| Key    | Default          | Meaning                                  |
|--------|------------------|------------------------------------------|
| `name` | directory name   | plain project name                        |
| `entry`| `src/main.flt` | path to the main program (relative to project root) |
| `stack`| `65536`          | extra memory bytes reserved for the stack |
| `memory` | `"manual"`     | allocator: `"manual"` (bump arena; `alloc` blocks are never reclaimed) or `"auto"` (conservative mark-sweep collector; unreachable blocks — cycles included — are reclaimed, `alloc` returns a managed zeroed payload). The mode travels into `.fbc` images, standalone ELFs and the starter `flint new` writes |

The legacy v0.1 key `use_hand_made_ram_control` is still accepted
(`true` = manual, `false` = auto); if both keys appear, the later line
wins.

Run from inside a project directory: `flint run` (no arguments).
Single-file runs (`flint run file.flt`) have no project file and use the
default memory model (`manual`); `flint run --memory auto file.flt`
overrides it.

---

## Language syntax

### Lexical layer

- Comment: `// ...` — to end of line.
- Numbers: decimal and `0x...`; signed literals (`-5`).
- Character literal: `'A'`; escapes in strings/characters: `\n \t \r \0 \\ \' \"`.
- Strings: `"..."` (one per data directive statement; no auto-concatenation).
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*` — labels and mnemonic names.
- Registers: `r0`…`r15` (strict; `r16` is an error). `fp` is an address-base pseudo-register used for frame locals.
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

### Includes

`include "path"` (or `.include "path"`) splices another source file at the
directive's position. The CLI host resolves the path relative to the
including file, then the project root and `lib/`, and skips a real file
that was already included once:

```
.include "stdlib.flt"
```

Embedding hosts decide where includes come from: `flint_assemble_ctx`
takes a resolver callback per file — it gets the path as written plus the
including file's directory and name, which is how the CLI builds the
dependency tree above — while `flint_pp_includes()` is a
standalone textual preprocessor (fopen, depth-capped at 10) for hosts
without a resolver.

### Macros

`%macro name argc` … `%endmacro` defines a text macro; the body is
captured at the definition and replayed at every invocation. `%1` … `%N`
stand for the invocation arguments — `argc` is how many there are
(0 for none):

```
%macro SAVE_REGS 0
    push r1
    push r2
%endmacro

%macro LOADV 2                  // LOADV rd, base  =>  load rd, [base]
    load %1, [%2]
%endmacro

main:
    SAVE_REGS
    LOADV r0, r1
```

Invoke a macro like a mnemonic: its arguments follow the name on the same
line, comma-separated, and a bracketed operand stays together — in
`LOADV r0, [r1+8]` the second argument is `[r1+8]`. A macro may be defined
in any section, works from included files and nests (expansion depth
capped at 64), and shadows a same-named mnemonic. The assembler rejects a
duplicate macro, an unterminated `%macro`, an orphan `%endmacro`, a wrong
argument count and an out-of-range `%N` (checked against `argc` at
definition time); an error inside a body reports the invocation line.

### Bundled libraries

```
lib/stdlib.flt  print/strlen/memcpy/memset/itoa/num (CLI syscalls)
lib/string.flt  strcpy/strcat/strcmp/upper/lower + atoi/find/chr/
                starts_with/ends_with/trim/reverse
lib/math.flt    abs/min/max/clamp/gcd/lcm/isqrt/pow/lcg/hash64 (integer)
lib/crc32.flt   CRC-32/ISO-HDLC; one-shot + streaming, no allocation
lib/crc32c.flt  CRC-32C / Castagnoli; one-shot + streaming, no allocation
lib/sha256.flt  SHA-256 (FIPS 180-4); s_sha256(data, len, out) -> out
lib/base64.flt  RFC 4648 encode/decode; caller buffers, no static scratch
lib/progress.flt minimal tqdm-style progress bar: \r[####....] pct% + rough ETA
```

`math.flt` exposes integer helpers: `s_abs`, `s_min`, `s_max`,
`s_clamp`, `s_gcd`, `s_lcm`, `s_isqrt` (floor square root), `s_pow`
(exponentiation by squaring), `s_lcg` (one 64-bit LCG step — feed the
state back in for a PRNG) and `s_hash64` (splitmix64 finalizer for
stateless hashing). Arithmetic wraps on overflow; `s_isqrt` returns 0
for negatives and `s_pow` returns 0 for a negative exponent.

`crc32.flt` exposes `s_crc32(r1 = data, r2 = len)` for a complete
checksum, and `s_crc32_update(r1 = crc, r2 = data, r3 = len)` for
streaming; streaming callers start at `0xFFFFFFFF` and apply the final
`0xFFFFFFFF` xor themselves. It is reentrant and works in both memory
models.

`sha256.flt` exposes `s_sha256(r1 = data, r2 = len, r3 = out)`: it
writes the 32-byte digest and returns `out`. The message schedule, the
hash state, the round counter and the padding all live in
`enter`/`leave` frames — no allocation, no static scratch, reentrant.
Works in manual and auto modes.

`base64.flt` exposes `s_b64_encode(r1 = src, r2 = len, r3 = dst)`
(returns the number of chars written; size `dst` with
`s_b64_encoded_len(len)`) and `s_b64_decode(r1 = src, r2 = len,
r3 = dst)` (returns bytes written, or −1 on invalid input — it skips
whitespace and accepts unpadded final groups). No allocation, no static
scratch; both are reentrant.

### Memory operands

```
[r2]        // base address in a register
[r3+4]      // base + offset
[r3-8]      // base − offset
[r1+r2]     // base + index register (word loads/stores; byte address)
[fp-8]      // frame local: fp minus offset
[fp+r2]     // frame base + index register
```

The indexed form is word-only: `loadx` / `storx` (or plain `load`/`store`,
which detect `[rb+ri]` automatically). The store form is **address first**:

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
| `load rd, [rb+ri]`           | 4     | indexed: rd = word at rb+ri                  |
| `loadx rd, [rb+ri]`          | 4     | explicit indexed form (same encoding)        |
| `load.byte rd, [rb]`         | 11    | one byte, zero-extended                      |
| `store [rb±off], rs`         | 11    | full word write at the address               |
| `store [rb+ri], rs`          | 4     | indexed: word write at rb+ri                 |
| `storx [rb+ri], rs`          | 4     | explicit indexed form (same encoding)        |
| `store.byte [rb±off], rs`    | 11    | one-byte write at the address                |

`set rd, imm` loads an immediate, character literal, or label — it is not a
register move. Use `copy rd, rs` for register-to-register transfer; `set r3, r1`
is rejected at assembly.

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
jump.lo  ...                     // rs <u rt|imm (unsigned below)
jump.hs  ...                     // rs >=u rt|imm (unsigned higher-or-same)
```

Signed pairs follow int64 ordering; `lo`/`hs` reinterpret both sides as
uint64 — the honest form for comparing memory addresses (a signed branch
would lie for spans above 2^63).

Short form — compare against zero (the common case):

```
jump.eq r1, done                 // JEQI r1, 0, done
```

| Form                      | Bytes |
|---------------------------|-------|
| `jump.xx rs, rt, target`  | 11    |
| `jump.xx rs, imm, target` | 18    |
| `jump.xx rs, target`      | 18    (sugar for `rs ?= 0`) |

`xx` ∈ {eq, ne, lt, ge, lo, hs}. The sugar is a compare-against-zero —
meaningful for `eq`/`ne`; `lo r, x` as `lo r, 0, x` is unsigned-impossible
and never branches.

Unconditional jump and procedures:

| Form           | Bytes | Semantics                                       |
|----------------|-------|-------------------------------------------------|
| `jump label`   | 9     | pc = label (unconditional)                      |
| `call label`   | 9     | push return pc; pc = label                      |
| `return`       | 1     | pop the return pc                               |
| `enter imm`    | 10    | push fp; fp = sp; sp −= imm (reserve locals)    |
| `leave`        | 1     | sp = fp; pop fp                                 |
| `push rs`      | 2     | sp −= 8; mem[sp] = rs                           |
| `push imm`     | 9     | immediate onto the stack                        |
| `pop rd`       | 2     | rd = mem[sp]; sp += 8                           |

Recursion — `call`/`return` share the same stack as `push`/`pop`:
the factorial example (one pushed register + a one-word return frame
per level) runs as-is. Functions that need locals use `enter N` /
`leave`; the slots are `[fp-8]`, `[fp-16]`, … and the callee is
responsible for preserving `r12..r15` (push/pop around the body).

### Memory allocation

Two models share one heap region between the data section and the stack
(the cursor grows up, the stack grows down). Allocation fails if a block
would cross the stack pointer; every stack write (`push`, `pushi`, and the
return address of `call`) is rejected with `FLINT_ERR_STACK` if it would
cross `heap_ptr`, and `enter N` checks its whole frame before reserving it.
The model is picked by `Flint.toml`, `--memory`, or the defaults above.

> **0.3.0-pre.2:** every stack write is now checked against `heap_ptr`.
> Previously `push`/`call` could cross the frontier and overwrite a managed
> block header, making the next collector walk report
> `managed heap corrupt (block header damaged)` instead of the real cause.
> The guard applies to both memory models; the manual bump arena had the
> same silent overwrite. Regression tests cover auto and manual.

**manual — bump arena.** `alloc rd, n` returns the cursor and moves it
up by n. `return` never rewinds the cursor: a callee's block stays
valid, so returning an array from a function is safe. `free` is a
validated no-op — blocks have no size metadata to reclaim by, so
reclamation is the program's problem.

**auto — managed heap (conservative mark-sweep).** `alloc rd, n`
returns the zeroed payload of a collector-managed block (a 16-byte
header sits before the payload; a reused block may be slightly larger
than requested). `free` stays a no-op — the collector owns lifetime:
a block stays valid while any register, active stack word, data word,
host root or pin reaches it; interior pointers mark the containing
block; cycles are collectable. The collector runs when the bytes
allocated since the last collection pass a threshold (default 32 KiB),
whenever an allocation does not fit and reclamation could help, at an
explicit `gc`, or from the host (`flint_gc_collect`). If nothing can be
reclaimed and the block still does not fit, the VM traps
`FLINT_ERR_OOM` (−10). Manual mode is completely unchanged by this.

| Form            | Bytes | Semantics                                          |
|-----------------|-------|----------------------------------------------------|
| `alloc rd, imm` | 10    | manual: rd = heap cursor; cursor += imm            |
|                 |       | auto: rd = zeroed payload of a managed block       |
| `free rs`       | 2     | operand validated; no-op in both modes             |
| `gc`            | 1     | auto: collect now; manual: no-op                   |

```
alloc r1, 64        // r1 = block address (manual cursor / auto payload)
free r1             // validated no-op in both modes
gc                  // auto: reclaim everything unreachable, cycles included
```

Conservative-GC caveats (auto mode): *false retention* is possible — an
integer or stale word that happens to land inside a payload keeps that
block alive (bounded by the number of root words); pointers stored at
unaligned addresses are missed (scanning walks aligned words); and a
program that writes outside its blocks can corrupt block headers, which
traps `FLINT_ERR_BADMEM` at the next collector walk. The collector is
non-moving, so addresses never change under a running program.

### Platform

| Form        | Bytes | Semantics                                       |
|-------------|-------|-------------------------------------------------|
| `sys N`     | 2     | host function of slot N; r0 — input and result. A slot with no host handler traps |
| `exit`      | 1     | end the program; r0 — return code               |

---

## Syscalls (included CLI host)

Convention: arguments travel in `r0..r2`; `r0` is also the result slot (an
argument in `r0` is consumed before the result lands there). Memory arguments
point into the flat `mem` image — the host validates every span against
image bounds. File results are POSIX descriptors; failures return −1.

| Slot | Name      | Arguments | Result                         |
|------|-----------|-----------|--------------------------------|
| 0    | print_str | r1 = addr, r2 = len | writes r2 raw bytes (NULs included); clobbers no register |
| 1    | putc      | r0 = character | print the low byte            |
| 2    | getc      | —             | r0 = character or −1 (EOF)     |
| 3    | open      | r0 = NUL-terminated path in `mem`, r1 = flags (0 read / 1 write / 2 append) | r0 = fd or −1 |
| 4    | read      | r0 = fd, r1 = buffer in `mem`, r2 = max bytes | r0 = bytes read (0 on EOF) or −1 |
| 5    | write     | r0 = fd, r1 = buffer in `mem`, r2 = bytes | r0 = bytes written or −1 |
| 6    | close     | r0 = fd       | r0 = 0 or −1                   |
| 7    | stat      | r0 = NUL-terminated path in `mem`, r1 = 8-byte buffer in `mem` | r0 = 0 or −1; the file size (int64) is written to the buffer |
| 8    | mkdir     | r0 = NUL-terminated path in `mem`, r1 = mode (e.g. 0755) | r0 = 0 or −1 |
| 9    | unlink    | r0 = NUL-terminated path in `mem` | r0 = 0 or −1 |
| 10   | time      | —             | r0 = UNIX epoch seconds        |
| 11   | regs      | —             | debug: print r0..r15, one per line (not part of the stable ABI) |
| 12   | argc      | —             | r0 = number of program arguments |
| 13   | getarg    | r0 = index, r1 = buffer in `mem`, r2 = capacity | r0 = full length or −1; the argument is copied NUL-terminated (truncated to r2−1 bytes) |
| 14   | getenv    | r0 = NUL-terminated name in `mem`, r1 = buffer in `mem`, r2 = capacity | r0 = full length or −1 (unset) |
| 15   | lseek     | r0 = fd, r1 = offset, r2 = whence (0 set / 1 current / 2 end) | r0 = new offset or −1 |
| 16   | rename    | r0 = old path in `mem`, r1 = new path in `mem` | r0 = 0 or −1 |
| 17   | rmdir     | r0 = NUL-terminated path in `mem` | r0 = 0 or −1 |
| 18   | opendir   | r0 = NUL-terminated path in `mem` | r0 = handle ≥ 0 or −1 |
| 19   | readdir   | r0 = handle, r1 = buffer in `mem`, r2 = capacity | r0 = name length (`.`, `..` included), 0 = end of directory, −1 = error |
| 20   | closedir  | r0 = handle   | r0 = 0 or −1                   |

`open` flags: `0` = read-only; `1` = write, creating/truncating; `2` =
append, creating the file when missing. Slots 0..6 are the standard set
(`FLINT_SYS_*` in `flint.h`); 7..20 are the CLI extensions (7..9 file
metadata, 10..11 time/debug, 12..14 args/env, 15..20 files/dirs). Slots
21..63 stay free for embedding hosts. Calling an unfilled slot is a
predictable `NOSYS` trap.

Common recipes:

```
sys 0           // print a memory span (r1 = addr, r2 = len) — stdlib s_print
sys 1           // putc: low byte of r0
sys 2           // manual stdin reading, one character at a time
sys 11          // debug: dump registers mid-run
sys 3/4/5/6     // file round-trip: open(path) -> read/write(buf, n) -> close
sys 7/8/9       // file metadata: stat(path, buf) / mkdir(path, mode) / unlink(path)
sys 12/13/14    // argc / getarg(index, buf, cap) / getenv(name, buf, cap)
sys 15/16/17    // lseek(fd, off, whence) / rename(old, new) / rmdir(path)
sys 18/19/20    // opendir(path) / readdir(handle, buf, cap) / closedir(handle)
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
static void my_double(flint_vm *vm)   /* sys 12: r0 = r0 * 2 */
{
    vm->r[0] *= 2;
}

/* before the run: */
flint_sys_set(&vm, 12, my_double);
```

### Step-by-step mode

```c
vm.max_steps = 1000000;                /* escape hatch against infinite loops */
flint_run(...);                        /* err == FLINT_ERR_LIMIT on runaway loops */
```

`flint_step(&vm)` — exactly one instruction per call — for
debuggers/tracers.

### Automatic memory API (auto mode)

```c
flint_heap_set(&vm, heap_start, FLINT_MEM_AUTO);  /* arms the collector */

flint_gc_set_threshold(&vm, 65536);   /* bytes between proactive collections
                                         (0 = collect on allocation failure only) */
flint_gc_set_stress(&vm, 1);          /* collect before every allocation */

flint_gc_stats gs;
flint_gc_get_stats(&vm, &gs);         /* collections, live/free/alloc/
                                         reclaimed bytes, threshold, rescans */

flint_gc_collect(&vm);                /* explicit collection */
flint_word p = flint_gc_alloc(&vm, 64);   /* 0 + vm.err == FLINT_ERR_OOM */
flint_gc_free(&vm, p);                /* explicit release, validated
                                         (traps invalid/double free) */
```

The collector state (including its mark worklist) lives inside
`flint_vm`; `flint_gc.c` needs no libc and no allocation of its own.
Hosts that keep a raw payload pointer outside the VM must register it
before letting a collection run — `flint_gc_root(vm, &slot)` for a
`flint_word` slot the host owns, or `flint_gc_pin(vm, addr)` for a
plain address. Conservative scanning finds pointers in VM memory
(registers, stack, data), not in host structs.

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
make          # build/flint + build/stub.elf (--elf stub)
make test     # full bench: core + asm + heap + gc + examples + the
              # tests/gc.sh integration run (4 exec paths x base64 vs base64 -w0)
make bench    # build the bench examples + compare run speeds
make clean
```

Tagged releases also ship prebuilt archives (x86_64 Linux built and
tested by the release workflow, aarch64 Termux built locally); each
contains `flint`, `build/stub.elf` and the standard `lib/`, so programs
with `include` and `flint build --elf` both work from the extracted
directory.

Embedding is minimal: `flint_core.c` + `flint_gc.c` (+ `flint_asm.c` if
you want source assembly) is one TU each; build with `-I include`.

---

## Examples

```
tests/hello.flt   Hello, FLINT!                       (byte-wise string printing)
tests/sum.flt     55                                  (sum 1..10)
tests/fact.flt    720                                 (recursion: fact(6))
tests/off.flt     119                                 (store/load with an offset)
tests/fio.flt     42                                  (open/write/read/close round-trip)
tests/regs.flt    r0 = 15 … r15 = 0                   (register dump)
tests/libdemo.flt 63 / 9 / 11 / HELLO WORLD           (stdlib + string includes)
tests/crc32_test.flt empty / 123456789 / fox / incremental (CRC-32 known vectors)
tests/crc32c_test.flt empty / 123456789 / fox / abc / incremental (CRC-32C known vectors)
tests/progress_test.flt 0% / 25% / 50% / 75% / 100% (mini progress bar)
tests/sha256_test.flt empty / abc / long NIST vectors (SHA-256)
tests/base64_test.flt RFC 4648 vectors + padded/unpadded/whitespace decode + round-trip
tests/mem.flt     flint                               (s_memset/s_memcpy via .include)
tests/heap.flt    heap mode: manual + heap ok         (alloc/free + persistent allocations)
```

Benchmark examples (project form + single-file form of each):

```
examples/bench_sum.flt   bench_sum: ok     (sum 1..40,000,000; ~1.1s)
examples/bench_sum/      same program as a project (Flint.toml + src/main.flt)
examples/bench_fact.flt  bench_fact: ok    (recursive fib(33); ~1.0s)
examples/bench_fact/     project form, fib in lib/fib.flt (bundled by `build`)
```

Demo (project form):

```
examples/b64-demo/       base64 encoder — stdin → stdout, RFC 4648 (≈ `base64 -w0`)
examples/b64-mem/        same encoder, chunked: allocs a fresh input block + output
                         block per 3 KiB chunk and never frees — the auto-mode
                         collector reclaims each chunk. Passes 1 MiB in
                         run/.fbc/ELF; the same program in manual mode dies at
                         the 10th chunk (36 KiB of output, rc=251).
                         `sh tests/gc.sh` diffs every path.
examples/hexdump/        `hexdump -C` clone: 4 KiB chunks on the auto heap (the
                         collector reclaims each previous one), offset / hex /
                         ASCII columns; byte-identical to `hexdump -C` from
                         empty input to 1 MiB.
examples/flint-brainfuck/ Brainfuck interpreter across four `lib/` modules
                         (io / tape / execute / bracket) — the multi-include
                         demo for `flint build`'s dependency tree:
                         `flint run examples/flint-brainfuck < prog.bf`
                         (try `examples/flint-brainfuck/examples/hello.bf`).
```

Run the demos: `printf 'foobar' | flint run examples/b64-demo` → `Zm9vYmFy`;
`printf 'foobar' | flint run examples/hexdump` → the `hexdump -C` dump;
b64-demo also fuses into a standalone ELF
(`flint build --elf examples/b64-demo`).

Run one: `flint run tests/sum.flt` — or all of them: `make test`.
`make bench` builds both examples with `flint build` and compares
project / single-file / `.fbc` run times (`sh tests/bench.sh [reps]`).
