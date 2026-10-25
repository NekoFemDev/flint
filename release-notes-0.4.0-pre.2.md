# FLINT 0.4.0-pre.2

Build fix for Linux, plus the first prebuilt binaries.

## Fixed

- **`make` failed on glibc with GCC 14+** (e.g. Arch Linux):
  `realpath()`'s declaration is hidden by glibc under `-std=c99` unless a
  feature-test macro is defined, and GCC 14 turns the implicit declaration
  into a hard error. `src/flint_cli.c` now defines
  `_POSIX_C_SOURCE 200809L` before its includes.
- **`include` aborted on fortify glibc builds** (Ubuntu's default
  `_FORTIFY_SOURCE=3`): the include loader gave `realpath()` a 1 KiB
  buffer, while glibc's `__realpath_chk` requires `PATH_MAX + 1` bytes —
  every program using `include "..."` died with "buffer overflow
  detected". It now uses `realpath(path, NULL)`. Caught by the new Linux
  CI on its very first run.
- `-Wformat-truncation` warnings from `full_local()` and the CLI error
  messages on newer GCC are gone (explicit bounded copies, precision
  bounds on paths).

## Prebuilt binaries

Attached to this release — each archive contains `flint`,
`build/stub.elf` and the standard `lib/` sources, so `include "stdlib.flt"`
and `flint build --elf` both work from the extracted directory (keep the
layout: `flint build --elf` looks for the stub at `build/stub.elf`
relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.2-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.2-x86_64-linux.tar.gz.sha256` | same | checksum |
| `flint-v0.4.0-pre.2-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.2-aarch64-termux.tar.gz.sha256` | same | checksum |

Quick start:

```bash
tar -xzf flint-v0.4.0-pre.2-*.tar.gz
cd flint-v0.4.0-pre.2-*
./flint --version
./flint run path/to/program.flt
```

## Verification

- `make test` passes on the CI Linux runner and on Termux (core/asm/heap/gc
  units, 12/12 examples, SHA-256/base64 vectors, `--threads`, gc.sh).
- The packaged Termux archive was extracted and smoke-tested end to end
  (`flint --version`, `flint run`, and `flint build --elf` producing a
  runnable standalone binary from the extracted directory).

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.
