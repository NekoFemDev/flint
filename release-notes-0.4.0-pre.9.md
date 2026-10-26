# FLINT 0.4.0-pre.9

The assembler gains two building blocks: indirect control transfers —
`jump rs` / `call rs`, with the target taken from a register — and `equ`
constants, compile-time immediates that emit no bytes. Together they open
the door to switch tables, register-selected dispatch, and named
constants without touching the data section.

## Added

### Indirect jumps and calls

Two new opcodes back the register forms — `OP_JMPR` and `OP_CALLR`, one
register byte each (2 bytes per instruction) — plus the text surface:

```
set r1, case3
jump r1                  // switch-table dispatch: pc = r1
```

```
set r6, worker
call r6                  // push return pc; pc = r6
```

- `jump rs` — `pc = rs`. The label form `jump label` is unchanged
  (9 bytes, absolute fixup); the same split applies to `call`.
- `call rs` — pushes the return pc onto the shared call/push stack, then
  `pc = rs`; `return` comes back exactly as it does for `call label`.
- The target is re-checked against the code size on **every** transfer:
  a register value outside `[0, code_size)` traps `FLINT_ERR_BADPC` at
  the transfer itself (a negative word is out of range by construction),
  and `pc == code_size` is refused — there is no byte there to decode.
- `flint dis` decodes the pair as `jump rN` / `call rN`; the output stays
  assembler-surface and reassembles as-is.
- The opcodes are appended after `OP_GC` (before `OP_COUNT`), so every
  existing opcode value — and every `.fbc` image built by an earlier
  pre-release — keeps loading unchanged.

### `equ` constants

`name equ value` binds an identifier to an absolute 64-bit immediate.
The line emits no bytecode, so it is legal in either section:

```
BUFSZ  equ 64
LIMIT  equ 3
NEG    equ -5
ALIAS  equ BUFSZ        // the value may name an earlier equate
CHAR   equ 'A'          // char literals work
```

Substitution happens in `parse_imm()` — the equate table is consulted
before label resolution — so the name works everywhere an immediate
does: `set`, the arithmetic short form, `push`, `enter`, and the
branch-compare position.

```
    jump.ge r1, LIMIT, done
```

Semantics and limits: a value is a number, a char literal, `-value`, or
an earlier equate; forward references are not resolved (assembly is
single-pass by design); one value per line (`X equ 1, 2` is rejected
with a diagnostic); re-defining a name is an error. An equate shadows a
same-named label in immediate position, since equates are checked first.

## Verification

- Full `make test` on the aarch64 Termux source tree: core + asm + heap +
  gc + the 12/12 example programs + the lib/ test suite + the `gc.sh`
  integration run — all green.
- `test_core` hand-encodes `OP_JMPR`/`OP_CALLR`: indirect dispatch,
  indirect call + return, negative target (`BADPC`), `pc == code_size`
  (`BADPC` at the transfer, pinned by single-stepping) and register 16
  (`BADREG`).
- `test_asm` runs both indirect forms end to end and exercises the `equ`
  surface: values in arithmetic and branch compares, negative, char
  literal and alias chains, plus the rejection paths (duplicate,
  undefined value, missing value, comma list).
- Both suites also run clean under `-fsanitize=address,undefined`.
- CLI round-trips: `flint run`, `flint check`, `flint build` → `.fbc`
  run, `flint dis` output including the new `jump rN` / `call rN` lines,
  and a `.fbc` image built before the change still runs (the append-only
  opcode policy).
- `release.yml` builds and runs `make test` on its Ubuntu runner before
  packaging the x86_64 archive.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf` and the standard `lib/`
sources, so `include "stdlib.flt"` and `flint build --elf` work from the
extracted directory (keep the layout: `flint build --elf` looks for the
stub at `build/stub.elf` relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.9-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.9-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-pre.9-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.9-aarch64-termux.tar.gz.sha256` | same | checksum |

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.
