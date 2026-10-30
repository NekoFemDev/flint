# FLINT 0.4.0-beta.5

A socket-correctness release. Three host fixes, one tightened syscall
contract, and a working TCP client example. No ISA, opcode, image-format
or syscall-number changes.

## Fixed

### Zero-length `recv()` no longer looks like peer-close

POSIX returns `0` for `recv(fd, buf, 0, 0)`, while the FLINT ABI documents
`0` as "the peer closed the connection". A guest that passed `r2 = 0`
therefore saw a phantom EOF. Slot 24 now rejects the empty request before
the POSIX call:

```c
if (fd < 0 || fd > INT_MAX || cnt < 1 || !clif_span(vm, off, cnt)) {
    vm->r[0] = -1;
    return;
}
```

Callers that want a no-op should skip the syscall; `r2` must be at least
1. `tests/net.flt` asserts the `-1` result, and the header/README table
now says `r2 >= 1`.

### `send()` cannot SIGPIPE the host on BSD/macOS

The host passed `MSG_NOSIGNAL` to `send()`. That flag exists on
Linux/Android but not on BSD/macOS, where writing to a peer that has
closed raises `SIGPIPE` and terminates the process. `socket()` now sets
`SO_NOSIGPIPE` when the platform defines it:

```c
#ifdef SO_NOSIGPIPE
    if (fd >= 0) {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif
```

Linux/Android keeps the per-send `MSG_NOSIGNAL` path, so its behaviour is
unchanged.

### Convenience runs pass program arguments

`flint file.flt alpha beta` is a shorthand for `flint run file.flt alpha
beta`, but the shorthand bypassed `flint_cli_set_args()`, so `sys 12..14`
saw no arguments. Both the `.flt` and `.fbc` shorthand paths now register
the trailing argv words exactly like the explicit verb. `make test` runs
`tests/args.flt` through both forms.

## Added

### `examples/http-get.flt`

A complete TCP client on the existing 21..25 syscalls:

- argv `host port path` with defaults `127.0.0.1 8080 /`;
- builds `GET <path> HTTP/1.0`, `Host:` and `Connection: close`;
- send-until-done loop, because `send()` may accept fewer bytes than
  requested;
- recv loop until `0` (peer close), printing each chunk as it arrives;
- closes the descriptor on success and on every failure path.

It runs from source, from a `.fbc` image, and from a fused `--elf`
binary:

```bash
python3 -m http.server 8080
flint run examples/http-get.flt 127.0.0.1 8080 /index.html

flint run examples/http-get.flt 1.1.1.1 80 /       # no DNS: dotted quad

flint build --elf examples/http-get.flt
./target/http-get 1.1.1.1 80 /
```

## Changed

### `recv` requires `r2 >= 1`

`recv` (slot 24) is the only contract change in this release: the max
byte count must be at least one. `0` stays reserved for the peer-close
sentinel. This is a host-side check, not a new opcode or syscall number;
existing programs that passed a positive length are unaffected.

## Verification

- Full `make test` on the aarch64 Termux source tree: core, asm, heap, gc,
  the example programs, the `lib/` suites, the fused `.so` host and
  `gc.sh` — all green.
- `tests/net.flt` now covers the zero-length `recv` rejection and runs
  with empty stderr.
- `examples/http-get.flt` verified from source, `.fbc` and fused `--elf`
  against a local Python HTTP server, and from source/ELF against
  `1.1.1.1:80` (301 response). Short sends, peer-close, bad port and
  failed-connect paths were exercised.
- `make test` covers both `flint run file.flt alpha beta` and
  `flint file.flt alpha beta`.

## Prebuilt binaries

Each archive contains `flint`, `build/stub.elf`, `build/stub.so` and the
standard `lib/` sources, so `include "stdlib.flt"`, `flint build --elf`
and `flint build --so` all work from the extracted directory (keep the
layout: both stubs are looked up at `build/...` relative to the working
directory):

| File | Built by | Notes |
|------|----------|-------|
| `flint-v0.4.0-beta.5-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-beta.5-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-beta.5-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-beta.5-aarch64-termux.tar.gz.sha256` | same | checksum |

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.

## Notes

- IPv4 TCP client side only: there is still no `bind`/`listen`/`accept`,
  no UDP, no DNS and no IPv6.
- `connect()` and `recv()` remain blocking without a timeout; a blackholed
  peer parks the guest until the process is interrupted.
- Sockets are raw POSIX descriptors: no TLS and no framing.
  `close_socket` is `close(2)`, so it will close any descriptor.
