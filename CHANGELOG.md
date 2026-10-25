# Changelog

## 0.4.0-pre.6 — 2026-10-26

### Added

- **File-metadata syscalls** in the CLI host (`src/flint_cli.c`): slot 7
  `stat` (r0 = path, r1 = 8-byte out buffer; the size is written back as
  int64), slot 8 `mkdir` (r0 = path, r1 = mode, e.g. 0755) and slot 9
  `unlink` (r0 = path). Paths are validated as NUL-terminated spans
  inside the VM image before the POSIX call; results are 0/−1.
- **`flint repl`** (`src/flint_cli.c`): an interactive session over a
  persistent 64 KiB VM — one statement per line, assembled and run with
  registers, memory and the heap surviving between lines. Each line is
  wrapped into a little program (the REPL supplies `main:` and a
  trailing `exit` when the line brings neither), gets a fresh 1000-step
  budget against runaway loops, and reports assembly errors without
  closing the session; `sys 11` (regs) dumps state, `exit`/EOF ends it.

### Changed

- **`time` and `regs` moved from syscall slots 8/9 to 10/11**
  (`include/flint_cli.h`) to make room for the file-meta set (7..9).
  Breaking for programs that use `CLI_SYS_TIME` / `CLI_SYS_REGS` by
  number; the bundled users (`lib/progress.flt`,
  `examples/progress-demo`, `tests/regs.flt`) and the README syscall
  table are updated.

## 0.4.0-pre.5 — 2026-10-25

### Added

- **NASM-style text macros** (`src/flint_asm.c`): `%macro name argc` …
  `%endmacro` captures a body and replays it at each invocation;
  `%1`…`%N` substitute the comma-separated arguments (`[...]` operands
  stay glued, so `LOADV r0, [r1+8]` passes one argument). Bodies expand
  as token streams into the scanner ahead of the enclosing source: macros
  nest (depth cap 64), work in `code:`/`data:`, work from included files,
  and shadow a same-named mnemonic. Duplicate macros, an unterminated
  `%macro`, an orphan `%endmacro`, a wrong argument count and an
  out-of-range `%N` (checked at definition time) are rejected with a
  line; an error inside a body reports the invocation line.
  `README.md` documents the layer under Language syntax.

## 0.4.0-pre.4 — 2026-10-25

### Added

- **`flint build` prints a Cargo-style dependency tree**: `Compiling
  <name> [<memory>]`, the resolved include tree (`├──` / `└──`), then
  `Finished <out> (<size> KiB) in <ms>`. `Compiling` is bold cyan and
  `Finished` green, on a terminal only (`NO_COLOR` opts out). Each file
  appears once, under its first includer (matching the resolver's
  include-once semantics); paths are shown relative to the project root,
  then the cwd. `--elf` reports `target/<name>` the same way, replacing
  the `[OK] Standalone ELF: ...` line.
- **`examples/hexdump/`**: streaming `hexdump -C` clone — 4 KiB chunks on
  the auto heap, so memory stays flat (1 MiB of input: one live chunk,
  every previous one reclaimed); output is byte-identical to
  `hexdump -C` from empty input to 1 MiB, in both memory models.
- **`examples/flint-brainfuck/`**: Brainfuck interpreter across four
  `lib/` modules (io / tape / execute / bracket) — the multi-include
  demo for the new build tree.

### Changed

- **`flint_include_fn` gained a `from_name` parameter**
  (`include/flint.h`): the name the including file was opened under (the
  root source's `src_name` at the top level). Breaking for hosts that
  implement the resolver callback — add the parameter (it may be
  ignored). `do_include` passes the includer's name; the CLI resolver
  uses it to relate includes to one another, which is what builds the
  tree without a second pass over the sources.

## 0.4.0-pre.3 — 2026-10-25

### Fixed

- **Building from source on glibc ≥ 2.42** (e.g. current Arch): glibc
  moved `realpath()`'s declaration guard from `__USE_XOPEN2K8` to
  `__USE_MISC || __USE_XOPEN_EXTENDED`, so the `_POSIX_C_SOURCE` macro
  introduced in 0.4.0-pre.2 stopped exposing the declaration there and
  GCC failed with an implicit-declaration error. `src/flint_cli.c` now
  defines `_XOPEN_SOURCE 700`, which satisfies both the old and the new
  guard. Verified on Ubuntu 24.04 (GCC 13.3, glibc 2.39) and Ubuntu
  26.04 (GCC 15.2, glibc 2.43): `-Werror` build, full `make test`, and
  include-path probes.

### Changed

- The release workflow now packs `lib/` into the prebuilt archives and
  writes bare filenames into the `.sha256` files, so the archives are
  self-contained and `sha256sum -c` works from any download directory
  (the 0.4.0-pre.2 assets were adjusted by hand after publishing).

## 0.4.0-pre.2 — 2026-10-25

### Fixed

- **glibc build under GCC 14+**: `realpath()` is hidden by glibc under
  `-std=c99` without a feature-test macro, and GCC 14 turns the implicit
  declaration into a hard error, so `make` failed on e.g. Arch.
  `src/flint_cli.c` now defines `_POSIX_C_SOURCE 200809L` before its
  includes.
- **glibc fortify abort on every `include`**: the include loader passed
  a 1 KiB buffer to `realpath()`, but glibc's `__realpath_chk`
  (`_FORTIFY_SOURCE=3`, Ubuntu's default) requires `PATH_MAX + 1` bytes
  when a buffer is supplied — any program using `include "..."` aborted
  with "buffer overflow detected". `inc_load` now calls
  `realpath(path, NULL)` (malloc'd result). Caught by the new release
  workflow on its first run.
- `-Wformat-truncation` warnings from `full_local()` and the CLI error
  messages on newer GCC are gone (explicit bounded copies; precision
  bounds on paths).

### Added

- `.github/workflows/release.yml`: on every version tag, GitHub Actions
  builds and tests FLINT on Linux and attaches the release archive
  (`flint` + `build/stub.elf`, so `flint build --elf` works from the
  extracted directory) to the GitHub release.
- Prebuilt archives on the release page: `x86_64-linux` from CI and
  `aarch64-termux` built on Termux (bionic).

## 0.4.0-pre.1 — 2026-10-25

### Added

- `lib/sha256.flt`: complete SHA-256 (FIPS 180-4) — bitwise core with
  no tables, message schedule, hash state, round counter and padding all
  in `enter`/`leave` frames. `s_sha256(r1 = data, r2 = len, r3 = out)`
  writes the 32-byte digest and returns `out`. No allocation, no static
  scratch: reentrant and safe under host-level threads; works in manual
  and auto modes.
- `tests/sha256_test.flt`: FIPS 180-4 vectors (`""`, `"abc"`, the
  56-byte message that exercises two-block padding). Wired into
  `make test`.
- `lib/base64.flt`: RFC 4648 encoder + decoder over caller buffers.
  `s_b64_encode(src, len, dst)` returns chars written;
  `s_b64_decode(src, len, dst)` returns bytes written or −1 on invalid
  input (skips whitespace, accepts unpadded final groups);
  `s_b64_encoded_len(len)` sizes the output buffer. No allocation, no
  static scratch.
- `tests/base64_test.flt`: RFC 4648 vectors, padded / unpadded /
  whitespace decode, invalid-input cases and a 300-byte round-trip.
  Wired into `make test`.
- **Host-level multithreading**: `flint run <file> --threads N` runs N
  isolated VM instances in N OS threads, and
  `flint_run_parallel(filename, num_threads, stack_size)` is the
  embedding entry point. Every worker reads (or assembles) its own copy
  — code, memory and stack are fully isolated; stdout is shared, so
  output may interleave at print granularity. A `--threads 4` run is
  part of `make test`.

### Changed

- `lib/stdlib.flt`: `s_num` no longer uses a static scratch buffer — it
  prints digit-by-digit through `sys 1`, so the standard library has no
  mutable static state left and is fully reentrant.
- `Makefile`: `-pthread` on the CLI and stub links; `make test` gained
  SHA-256, base64 and `--threads` checks.

## 0.3.0-pre.3 — 2026-10-25

### Added

- `lib/crc32.flt`: CRC-32/ISO-HDLC (reflected polynomial `0xEDB88320`)
  with one-shot `s_crc32` and streaming `s_crc32_update`. No allocation,
  no static scratch; works in manual and auto modes.
- `lib/crc32c.flt`: CRC-32C / Castagnoli (reflected polynomial
  `0x82F63B78`) with one-shot `s_crc32c` and streaming
  `s_crc32c_update`. Same ABI, no allocation, works in manual and auto.
- `tests/crc32_test.flt` and `tests/crc32c_test.flt`: known-answer
  vectors for empty input, `"123456789"`, the classic quick-brown-fox
  sentence, short strings, and streaming parity. Wired into `make test`.
- `lib/progress.flt`: minimal progress bar inspired by tqdm —
  `s_progress_bar(done, total, width)` prints `\r[####....] pct%`,
  `s_progress_eta(start, done, total)` estimates remaining time.
  No allocation, works in manual and auto modes.
- `tests/progress_test.flt`: render check for 0/25/50/75/100%.
  Wired into `make test`.

## 0.3.0-pre.2 — 2026-10-24

### Fixed

- **Stack/heap collision.** `push` and `call` grow the stack through the
  shared `stk_push()` helper, which only checked `sp >= 8`. A program that
  allocated most of memory could drive the stack below `heap_ptr` and
  overwrite the heap. In auto mode this corrupted a managed block header,
  so the next collector walk reported `managed heap corrupt (block header
  damaged)` instead of the real cause. Manual mode had the same gap and
  could silently overwrite its bump arena.
  `stk_push()` now rejects any push whose new `sp` would be below
  `heap_ptr`, trapping `FLINT_ERR_STACK` (−6). This covers `push`, `pushi`,
  the return address of `call`, and `enter` (which already pre-checked its
  full frame).
- Added regression tests for both allocators: `tests/test_gc.c` (auto) and
  `tests/test_core.c` (manual) allocate most of the memory image, push past
  the frontier, and require `FLINT_ERR_STACK` with the heap frontier and
  block header left intact.
- Fixed a stale `flint_init()` comment that still described `mem_mode` as
  informational; `flint_heap_set()` selects the real model.

### Documentation

- `README.md`: documented the stack/heap frontier guard and the pre.2 fix;
  updated the version and stack model description.
- `TODOS.md`: updated status, evidence, and the regression description.
- Added this changelog.

### Compatibility

- No API or bytecode changes. Manual-mode programs that previously pushed
  past `heap_ptr` now trap instead of silently corrupting the bump arena.

## 0.3.0-pre.1 — 2026-10-23

- Automatic memory: conservative, non-moving mark-sweep collector, first-fit
  free-list allocator, managed block headers, `gc` opcode, GC stats/stress
  flags, host root/pin API, and `examples/b64-mem`.
- See the GitHub release notes for the full pre.1 description.
