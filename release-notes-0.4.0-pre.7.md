# FLINT 0.4.0-pre.7

FLINT gets an application surface. Programs can read the command-line
arguments and environment they were started with, seek inside files,
rename and remove paths, and list directories — and the bundled
libraries grow a complete string pack plus an integer math library.

## Added

- **Program arguments and environment** (sys 12..14). `flint run prog.flt
  a b` passes `a b` to the program:

  | Slot | Name | Arguments | Result |
  |------|------|-----------|--------|
  | 12 | argc | — | r0 = number of program arguments |
  | 13 | getarg | r0 = index, r1 = buffer in `mem`, r2 = capacity | r0 = full length or −1; the argument is copied NUL-terminated (truncated to r2−1 bytes) |
  | 14 | getenv | r0 = NUL-terminated name in `mem`, r1 = buffer in `mem`, r2 = capacity | r0 = full length or −1 (unset) |

  Returning the *full* length lets a caller detect truncation
  (`len >= capacity`). Standalone ELFs (`flint build --elf`) expose
  their own process arguments the same way; `--threads` runs take no
  program arguments.

- **File/directory syscalls** (sys 15..20):

  | Slot | Name | Arguments | Result |
  |------|------|-----------|--------|
  | 15 | lseek | r0 = fd, r1 = offset, r2 = whence (0 set / 1 current / 2 end) | r0 = new offset or −1 |
  | 16 | rename | r0 = old path in `mem`, r1 = new path in `mem` | r0 = 0 or −1 |
  | 17 | rmdir | r0 = path in `mem` | r0 = 0 or −1 |
  | 18 | opendir | r0 = path in `mem` | r0 = handle ≥ 0 or −1 |
  | 19 | readdir | r0 = handle, r1 = buffer, r2 = capacity | r0 = name length, 0 = end of directory, −1 = error |
  | 20 | closedir | r0 = handle | r0 = 0 or −1 |

  Directory handles come from a fixed 8-slot table; `.` and `..` are
  passed through like any entry. `open(path, 2)` appends (creating the
  file when missing).

- **`lib/string.flt` pack**: `s_atoi` (string → int64, whitespace-aware,
  wrapping), `s_find` (substring index or −1; empty needle matches at
  0), `s_chr` (byte index or −1; searching 0 finds the terminator),
  `s_starts_with`, `s_ends_with` (1/0; empty pattern matches), `s_trim`
  (in-place space/tab/CR/LF) and `s_reverse` (in-place) join the
  existing helpers — no allocation, reentrant, same `r1..r3 → r0` ABI.

- **`lib/math.flt`** (new): `s_abs`, `s_min`, `s_max`, `s_clamp`,
  `s_gcd` (sign-normalised), `s_lcm`, `s_isqrt` (floor square root),
  `s_pow` (exponentiation by squaring; negative exponent → 0), `s_lcg`
  (one 64-bit LCG step — stateless; feed the state back in for a PRNG)
  and `s_hash64` (splitmix64 finalizer for stateless hashing). Wrapping
  int64 arithmetic throughout.

- **Tests**: `tests/string_test.flt` (32 checks over every string helper,
  including the previously untested strcpy/strcat/strcmp/lower),
  `tests/math_test.flt`, `tests/fsops.flt` (append → lseek → rename →
  readdir → unlink → rmdir round-trip) and `tests/args.flt`
  (argc/getarg/getenv), all wired into `make test`.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf` and the standard `lib/`
sources, so `include "stdlib.flt"` and `flint build --elf` work from the
extracted directory (keep the layout: `flint build --elf` looks for the
stub at `build/stub.elf` relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.7-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.7-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-pre.7-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.7-aarch64-termux.tar.gz.sha256` | same | checksum |

Quick start:

```bash
tar -xzf flint-v0.4.0-pre.7-*.tar.gz
cd flint-v0.4.0-pre.7-*
./flint run path/to/program.flt arg1 arg2
```

## Verification

- Full `make test` on the aarch64 Termux sources used for the archive:
  core + asm + heap + gc + examples + the new string/math/fsops/args
  tests + the `gc.sh` integration run.
- Standalone ELF: `flint build --elf tests/args.flt` then
  `./target/args alpha beta` prints `args: ok`.
- REPL unaffected (sys 12 reports 0 arguments there).
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
