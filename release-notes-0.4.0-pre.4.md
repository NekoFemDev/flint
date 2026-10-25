# FLINT 0.4.0-pre.4

`flint build` now reports Cargo-style — the include tree it resolved, then
the artifact, size and build time. Two new example projects, and one
embedder-facing API change.

## Added

- **Build status with a dependency tree.** `flint build` prints
  `Compiling <name> [<memory>]` (bold cyan), the resolved include tree
  (`├──` / `└──`; each file once, under its first includer), and
  `Finished <out> (<size> KiB) in <ms>` (green). Colors appear on a
  terminal only (`NO_COLOR` opts out). The tree needs no second pass over
  the sources: the include resolver records each edge as it opens a file;
  cycles and diamond includes stay finite because the resolver already
  resolves every real file once. `--elf` uses the same format for
  `target/<name>`.
- **`examples/hexdump/`** — a `hexdump -C` clone reading 4 KiB chunks on
  the auto heap (the collector reclaims each previous chunk; `--gc-stats`
  on 1 MiB: `live=4096 reclaimed=1048576`). Verified byte-identical to
  system `hexdump -C` on empty, 1, 16, 17, 256 (all byte values), 4096,
  4097, 10240 and 1048576-byte inputs, in both memory models.
- **`examples/flint-brainfuck/`** — a Brainfuck interpreter across four
  `lib/` modules (io / tape / execute / bracket); a real multi-include
  project for the build tree. Runs `hello.bf` → `Hello World!`.

## Changed

- **`flint_include_fn` takes a `from_name` argument** (the name the
  including file was opened under; the root source's `src_name` at the
  top level). Embedding hosts add one parameter to their resolver — it
  may be ignored. All in-tree callers are updated (CLI resolver, tests).
  This is what lets the CLI relate includes to one another for the tree.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf` and the standard `lib/`
sources, so `include "stdlib.flt"` and `flint build --elf` work from the
extracted directory (keep the layout: `flint build --elf` looks for the
stub at `build/stub.elf` relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-pre.4-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-pre.4-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-pre.4-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-pre.4-aarch64-termux.tar.gz.sha256` | same | checksum |

Quick start:

```bash
tar -xzf flint-v0.4.0-pre.4-*.tar.gz
cd flint-v0.4.0-pre.4-*
./flint --version
./flint run path/to/program.flt
```

## Verification

- `-Werror` build and full `make test` on the Termux aarch64 sources used
  for the archive; ASan run over the new resolver/tree paths (success,
  error, dedupe and cycle cases).
- `examples/hexdump` diffed against system `hexdump -C` (sizes above);
  `examples/flint-brainfuck` runs `hello.bf`; the build tree was checked
  on nested, diamond and cyclic includes, with and without `-o`/`--elf`,
  from inside and outside the project directory.
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
