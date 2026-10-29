# FLINT 0.4.0-beta.3

The two fused hosts are one source now. `src/stub.c` builds twice:
`build/stub.elf` (the default build) stays the standalone-executable
host for `flint build --elf`, and `build/stub.so` is built with
`-DBUILD_AS_LIBRARY` as the dlopen host for `flint build --so` —
constructor, `dladdr`, the `flint_so_*` API. The combined host also
gains a JNI entry point, `Java_com_flint_Native_run`, so an Android or
desktop Java host can `System.load()` a fused library and run the
payload through the JNI bridge instead of `dlopen()`.

## Changed

### One `stub.c` for both hosts

The open → size → decode-trailer → validate → load sequence lives once,
in `load_embedded_payload()`. It reads the tail of the host's own file
and dispatches on the trailer magic:

- **FLSO** — the fused-library container `flint build --so` writes
  (version, sizes, symbol records, FNV-1a checksum over code / mem /
  symbols); the library host validates it exactly as before and serves
  `flint_so_call()` names from the symbol table.
- **FLNT** — the images-only container `flint build --elf` writes. The
  ELF host expects this one; the library host also accepts it (see
  below).

Everything else behaves as in 0.4.0-beta.2: the ELF host still exits 127
on a missing or malformed trailer, both hosts run the payload with the
CLI syscall table, honor `FLINT_GC_STRESS` / `FLINT_GC_STATS`, and the
library constructor/destructor still load on `dlopen` and free on
`dlclose`. `src/stub_so.c` is gone.

## Added

### JNI entry point

```c
JNIEXPORT jint JNICALL
Java_com_flint_Native_run(JNIEnv *env, jobject thiz);
```

The native half of:

```java
package com.flint;
public class Native { public native int run(); }
```

It runs `main` on a fresh VM (the one-shot `flint_so_run` input) and
returns the CLI convention: `r0 & 0x7FFFFFFF` at HLT, or −`FLINT_ERR_*`
on a trap or a failed load. For an overloaded Java method add JNI's
signature suffix; for `static native int run()` the second parameter is
`jclass` — the C ABI is identical.

The build prefers a real `jni.h` (`__has_include`); when the toolchain
does not expose one, a fallback declaration set keeps the export
ABI-correct without a JDK — the function never calls through the
`JNIEnv` table.

### Plain `FLNT` fusion for fused libraries

A shared library fused as `[stub.so][code][mem][FlintPayload]` also
loads: `main` and the JNI entry run; only the exported-label API
(`flint_so_call` / `flint_so_find`) needs the FLSO symbol table and
reports `FLINT_SO_ENOENT` without it. (`flint build --so` keeps writing
the FLSO container.)

### Tests

`tests/test_so.c` now dlsym()s the JNI export on the fused library and
calls it directly — no JVM needed, the function ignores `env`/`thiz`.

## Verification

- Full `make test` on the aarch64 Termux source tree: core + asm + heap
  + gc + the 12/12 example programs + the lib/ suites + the fused-`.so`
  host (including the new JNI checks) + `gc.sh` — all green.
- Loader failure paths on a fused `.so`: truncation → load error; a
  flipped payload byte → `checksum mismatch (corrupt library?)`; both
  leave `loaded = 0` and the run entries return −`FLINT_ERR_INIT`.
- Plain-FLNT fusion smoke test: a `[stub.so][code][mem][FLNT]` built
  from a fused ELF's payload loads, runs `main`, and runs the JNI entry.
- The no-`jni.h` fallback path is compile-tested (build + symbol export)
  on the Termux tree, so the Ubuntu runner builds it the same way.
- `release.yml` builds and runs `make test` on its Ubuntu runner before
  packaging the x86_64 archive.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf`, `build/stub.so` and
the standard `lib/` sources, so `include "stdlib.flt"`,
`flint build --elf` and `flint build --so` all work from the extracted
directory (keep the layout: both stubs are looked up at `build/...`
relative to the working directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-beta.3-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-beta.3-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-beta.3-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-beta.3-aarch64-termux.tar.gz.sha256` | same | checksum |

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.

## Notes

- The JNI export is exercised by `dlsym` + a direct call in `make test`;
  a JVM binds the same symbol by name — on-device use with
  `System.load()` is the same ABI.
- Both fused hosts still assume the whole file is mapped (appended
  payload); `strip` and similar rewriters drop the trailer.
- The library exports the whole flint core API — load with the default
  `RTLD_LOCAL` so those symbols do not interpose across the host.
