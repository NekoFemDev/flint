# FLINT 0.4.0-pre.8

The assembler grows a real diagnostics front-end. Errors now render as
Rust/Clang-style blocks — message, source location, the offending source
line, and a caret run under the column — with ANSI colors on interactive
terminals. The lexer tracks columns (not just lines), so every token and
every error knows exactly where it is.

## Added

- **Rust/Clang-style assembler diagnostics** (`src/flint_asm.c`). The
  first error of an assembly pass is printed as:

  ```text
  error: expected register r0..r15
   --> demo.flt:3:10
    |
  3 |     copy r16, r1
    |          ^^^
  ```

  `error:` is bold red, `-->` cyan, the carets bold red. The block is
  written to stderr — with color only when stderr is an interactive
  terminal (`NO_COLOR` opts out). The snippet scans the current file for
  the line, strips a trailing CR, caps at 240 chars, and pads the caret
  run tab-aware (tabs advance to the next multiple of 8, as terminals
  do).

- **`include/flint_ui.h`**: the ANSI SGR palette shared by the
  diagnostics and the CLI status output (`CLR_RESET`/`CLR_BOLD`/
  `CLR_RED`/`CLR_GREEN`/`CLR_YELLOW`/`CLR_BLUE`/`CLR_CYAN`/`CLR_GRAY`).

- **Column tracking in the lexer**: `asmx` carries `col` alongside
  `line`/`ipos`, columns advance on every consumed character and reset
  on `\n`, and every `token` records the 1-based `line`/`col` it starts
  at. Include frames and macro frames save/restore the column; errors
  inside a macro body still report the invocation site.

- **`report_diag()` / `fail_tok()`**: `fail()` and `fail_at()` route
  through the reporter; `fail_tok()` points at the offending token for
  parser sites that hold one (`parse_reg`, `parse_mem`, `m_sys`).

## Changed

- The compact error string returned by `flint_assemble()` /
  `flint_assemble_ctx()` now includes the column: `file:line:col:
  message` (was `file:line: message`). The rich block is the primary
  report; the compact string stays for embedders and tests.
- Fixup diagnostics (undefined label, non-code label) report column 1
  and degrade to the header-only form when the snippet's file has
  already been popped.

## Verification

- Full `make test` on the aarch64 Termux sources used for the archive:
  core + asm + heap + gc + examples + the string/math/fsops/args tests +
  the `gc.sh` integration run.
- `test_asm`'s rejection paths exercise the reporter end to end,
  including the macro-body invocation-site check.
- Colors verified under a pseudo-tty (`script`); piped output is plain;
  `NO_COLOR` opts out.
- `release.yml` builds and runs `make test` on its Ubuntu runner before
  packaging the x86_64 archive.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf` and the standard `lib/`
sources, so `include "stdlib.flt"` and `flint build --elf` work from the
extracted directory (keep the layout: `flint build --elf` looks for the
stub at `build/stub.elf` relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.8-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.8-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-pre.8-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.8-aarch64-termux.tar.gz.sha256` | same | checksum |

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.
