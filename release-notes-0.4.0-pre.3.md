# FLINT 0.4.0-pre.3

Source-build fix for current glibc; the prebuilt archives are rebuilt by
the workflow that ships `lib/` natively.

## Fixed

- **`make` failed on glibc ≥ 2.42** (e.g. Arch): glibc moved
  `realpath()`'s declaration guard from `__USE_XOPEN2K8` to
  `__USE_MISC || __USE_XOPEN_EXTENDED`, so the `_POSIX_C_SOURCE` macro
  from 0.4.0-pre.2 stopped exposing the declaration and GCC 14+ failed
  with an implicit-declaration error. `src/flint_cli.c` now defines
  `_XOPEN_SOURCE 700`, which covers both old and new glibc.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf` and the standard `lib/`
sources, so `include "stdlib.flt"` and `flint build --elf` work from the
extracted directory (keep the layout: `flint build --elf` looks for the
stub at `build/stub.elf` relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.3-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.3-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-pre.3-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.3-aarch64-termux.tar.gz.sha256` | same | checksum |

Quick start:

```bash
tar -xzf flint-v0.4.0-pre.3-*.tar.gz
cd flint-v0.4.0-pre.3-*
./flint --version
./flint run path/to/program.flt
```

## Verification

- CI matrix: Ubuntu 24.04 (GCC 13.3, glibc 2.39) and Ubuntu 26.04
  (GCC 15.2, glibc 2.43) — `-Werror` build, full `make test`, and
  include-path probes (`sum.flt` → 55, `libdemo.flt` → 63).
- Termux archive extracted and smoke-tested (`flint --version`,
  `flint run` with includes, `flint build --elf` producing a runnable
  standalone binary).

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.
