# FLINT 0.4.0-pre.1

## What's New

### New Features

- **SHA-256 library** (`lib/sha256.flt`): complete FIPS 180-4
  implementation written in FLINT — bitwise core with no tables. The
  message schedule, hash state, round counter and padding live in
  `enter`/`leave` frames, so the library has no static scratch:
  reentrant, thread-safe and safe in both memory models.
  API: `s_sha256(r1 = data, r2 = len, r3 = out) -> r0 = out`.
- **Host-level multithreading**: `flint run <file> --threads N` runs N
  isolated VM instances in N OS threads. Each thread reads (or
  assembles) its own copy of the program and builds its own
  `flint_vm` — code, memory and stack are fully isolated. The embedding
  entry point is `int flint_run_parallel(const char *filename, int
  num_threads, uint32_t stack_size)`.
- **Base64 library** (`lib/base64.flt`): RFC 4648 encoder **and**
  decoder over caller buffers, no allocation, no static scratch.
  `s_b64_encode(src, len, dst)` returns chars written (size the output
  with `s_b64_encoded_len(len)`); `s_b64_decode(src, len, dst)` returns
  bytes written or −1 on invalid input, skips whitespace and accepts
  unpadded final groups.

### Changed

- **Reentrant stdlib**: `s_num` no longer uses a static scratch buffer —
  it prints digit-by-digit through `sys 1`. The standard library now has
  no mutable static state.

### New Tests

- `tests/sha256_test.flt`: FIPS 180-4 vectors — `""`, `"abc"` and the
  56-byte message that exercises the two-block padding path.
- `tests/base64_test.flt`: RFC 4648 vectors, padded / unpadded /
  whitespace decode, invalid-input cases, 300-byte round-trip.
- `make test` also runs a `--threads 4` smoke check.

### Documentation

- `README.md`: documented `--threads`, the SHA-256 and base64 libraries,
  and the new tests.
- `CHANGELOG.md`: recorded 0.3.0-pre.3 (previously missing) and this
  release.

## Verification

`make test` passes completely:

- Core/asm/heap/gc unit tests: all pass
- Examples: 12/12 programs run correctly
- SHA-256: NIST vectors; cross-checked against coreutils `sha256sum`
  for every input length 0–200 (manual and auto modes); r12–r15
  preservation verified
- Base64: RFC vectors + round-trip; encode/decode cross-checked against
  coreutils `base64` for 0–60,000 byte inputs, including wrapped input
- `--threads 4`: isolated-instance check (4 builds of the SHA-256 test
  produce 12 intact digests)
- `tests/gc.sh`: 8 inputs × 4 execution paths all match `base64 -w0`
- `gc: integration bench ok`

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

Prebuilt standalone ELFs are not provided for this prerelease; build
from source with `make`.
