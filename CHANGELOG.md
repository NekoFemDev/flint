# Changelog

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
