# FLINT 0.4.0-pre.6

The CLI host gains an interactive REPL and the file-metadata syscalls —
`stat`, `mkdir` and `unlink` — so FLINT programs can inspect paths and
create or remove files, and experiments no longer need a file plus a
build round-trip per change: `flint repl` keeps one VM alive between
lines.

## Added

- **`flint repl`** (`src/flint_cli.c`): an interactive session over a
  persistent 64 KiB VM. Each line is assembled and run on the same VM —
  registers, memory and the heap survive between lines:

  ```
  $ flint repl
  > set r0, 42
  > add r0, r0, 1
  > sys 11
  r0 = 43
  ...
  > exit
  ```

  The REPL wraps each line into a little program (a `main:` entry label
  and a trailing `exit` are supplied when the line brings neither; the
  terminator switches back to `code:` so a line may end inside `data:`),
  so a line may use labels of its own but symbols never outlive their
  line. Every line gets a fresh 1000-step budget — a runaway loop
  reports `step limit hit (1000 steps)` and returns to the prompt
  instead of locking the session. Blank lines are skipped, an over-long
  line is reported and skipped rather than truncated, and `exit` or EOF
  closes the session. A line's `data:` section is refreshed at the
  bottom of the persistent image; heap blocks and the stack above it
  stay put.
- **File-metadata syscalls** in the CLI host syscall table:

  | Slot | Name   | Arguments | Result |
  |------|--------|-----------|--------|
  | 7    | stat   | r0 = NUL-terminated path in `mem`, r1 = 8-byte buffer in `mem` | r0 = 0 or −1; the file size (int64) is written to the buffer |
  | 8    | mkdir  | r0 = NUL-terminated path in `mem`, r1 = mode (e.g. 0755) | r0 = 0 or −1 |
  | 9    | unlink | r0 = NUL-terminated path in `mem` | r0 = 0 or −1 |

  Paths are validated as NUL-terminated spans inside the image before
  the POSIX call; `stat` writes the size with an unaligned-safe store.

## Changed

- **`time` and `regs` moved to syscall slots 10 and 11** — the new
  file-meta set takes 7..9 as above. Breaking for programs that use
  `CLI_SYS_TIME` / `CLI_SYS_REGS` by number: the bundled users
  (`lib/progress.flt`, `examples/progress-demo`, `tests/regs.flt`) are
  updated, and the README syscall table documents slots 7..11.
  `include/flint_cli.h` gained `CLI_SYS_STAT`, `CLI_SYS_MKDIR` and
  `CLI_SYS_UNLINK`.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf` and the standard `lib/`
sources, so `include "stdlib.flt"` and `flint build --elf` work from the
extracted directory (keep the layout: `flint build --elf` looks for the
stub at `build/stub.elf` relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.6-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.6-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-pre.6-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.6-aarch64-termux.tar.gz.sha256` | same | checksum |

Quick start:

```bash
tar -xzf flint-v0.4.0-pre.6-*.tar.gz
cd flint-v0.4.0-pre.6-*
./flint --version
./flint repl
```

## Verification

- Full `make test` (core + asm + heap + gc + examples + the `gc.sh`
  integration run) on the aarch64 Termux sources used for the archive.
- Syscall smoke program: missing `stat` → −1, `mkdir` → 0, `stat` of the
  new directory → 0, file round-trip (open / close / stat / unlink),
  second `unlink` → −1.
- REPL: register and heap persistence across lines, per-line 1000-step
  cap, assembly-error recovery, blank/over-long line handling, EOF exit.
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
