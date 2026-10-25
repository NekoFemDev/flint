# FLINT 0.4.0-pre.5

FLINT's text surface gains a macro layer: `%macro` / `%endmacro` with
positional argument substitution. Write the register-save pair, the
load-with-offset idiom or a syscall preamble once and invoke it by name;
the assembler expands it in place.

## Added

- **NASM-style text macros** (`src/flint_asm.c`): `%macro name argc` …
  `%endmacro` captures a body; `%1`…`%N` substitute the invocation
  arguments. Invoke a macro like a mnemonic — `SAVE_REGS`, or
  `LOADV r0, r1` with a comma-separated list; a bracketed operand stays
  glued, so `LOADV r0, [r1+8]` passes `[r1+8]` as one argument:

  ```flint
  %macro SAVE_REGS 0
      push r1
      push r2
  %endmacro

  %macro LOADV 2
      load %1, [%2]
  %endmacro
  ```

  Bodies expand as token streams into the scanner ahead of the enclosing
  source, so macros nest (depth capped at 64), run in `code:` or `data:`,
  work from included files, and shadow a same-named mnemonic. The
  assembler rejects a duplicate macro, an unterminated `%macro`, an orphan
  `%endmacro`, a wrong argument count and an out-of-range `%N` (checked
  against `argc` at definition time); an error inside a body reports the
  invocation line.
- `tests/test_asm.c`: zero-argument, argument, nested and
  include-defined macro programs, plus the rejection paths above and an
  invocation-line diagnostic check.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf` and the standard `lib/`
sources, so `include "stdlib.flt"` and `flint build --elf` work from the
extracted directory (keep the layout: `flint build --elf` looks for the
stub at `build/stub.elf` relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.5-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.5-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-pre.5-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.5-aarch64-termux.tar.gz.sha256` | same | checksum |

Quick start:

```bash
tar -xzf flint-v0.4.0-pre.5-*.tar.gz
cd flint-v0.4.0-pre.5-*
./flint --version
./flint run path/to/program.flt
```

## Verification

- Full `make test` (core + asm + heap + gc + examples + the `gc.sh`
  integration run) on the aarch64 Termux sources used for the archive.
- ASan build of `tests/test_asm` over the macro paths (expansion,
  nesting, argument errors, recursion cap): clean.
- `release.yml` builds and runs `make test` on its Ubuntu runner before
  packaging the x86_64 archive.

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.
