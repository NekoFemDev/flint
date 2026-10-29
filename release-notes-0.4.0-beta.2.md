# FLINT 0.4.0-beta.2

The CLI host can talk to the network. Five new syscall slots give FLINT
programs TCP sockets — `socket`, `connect`, `send`, `recv` and
`close_socket` (slots 21..25) — so a bytecode program can open a
connection, push bytes and pull bytes back without a single line of host
code of its own. The descriptors are plain POSIX fds, like the file
layer's: a socket is an ordinary fd the ordinary slots can use.

## Added

### TCP syscalls (21..25)

| Slot | Name           | Arguments | Result |
|------|----------------|-----------|--------|
| 21   | `socket`       | — | `r0` = fd or −1 (AF_INET, SOCK_STREAM) |
| 22   | `connect`      | r0 = fd, r1 = NUL-terminated dotted-quad address in `mem`, r2 = port (1..65535) | `r0` = 0 or −1 |
| 23   | `send`         | r0 = fd, r1 = buffer in `mem`, r2 = bytes | `r0` = bytes sent or −1 |
| 24   | `recv`         | r0 = fd, r1 = buffer in `mem`, r2 = max bytes | `r0` = bytes received (0 = peer closed) or −1 |
| 25   | `close_socket` | r0 = fd | `r0` = 0 or −1 |

A round trip in text form:

```
data:
host: str "1.1.1.1", 0
msg:  str "hello", 0
buf:  bytes 32

code:
main:
    sys 21                  // TCP socket -> r0 = fd
    copy r4, r0
    copy r0, r4
    set r1, host
    set r2, 80
    sys 22                  // connect -> 0 or -1
    copy r0, r4
    set r1, msg
    set r2, 5
    sys 23                  // send -> bytes sent or -1
    copy r0, r4
    set r1, buf
    set r2, 32
    sys 24                  // recv -> bytes, 0 = peer closed
    copy r0, r4
    sys 25                  // close -> 0
```

Semantics and limits:

- **No name resolution.** `connect` takes an IPv4 dotted-quad literal
  (`inet_pton`); a bad string returns −1 before any syscall, so a FLINT
  program can never park the host inside a resolver.
- **Everything is validated first.** The address must be NUL-terminated
  inside the image, `send`/`recv` spans are checked against image bounds
  with the same guard the file layer uses, fds are range-checked, and
  ports outside 1..65535 are refused — a rejected argument returns −1 and
  never reaches the kernel.
- **Blocking by design.** `connect` waits for the handshake, `send`/`recv`
  wait for the descriptor; interrupted calls (`EINTR`) are retried
  internally. `recv` returning 0 means the peer closed — −1 means error.
- **No SIGPIPE.** `send` sets `MSG_NOSIGNAL` where the platform has it,
  so writing to a dead peer returns −1 instead of killing the whole host.
- **The descriptor space is shared.** `close_socket` is slot 6 `close`
  under a network name — either slot closes either kind of fd, and
  `read`/`write` accept a socket exactly like POSIX does.

### Tests

`tests/net.flt` (wired into `make test`) covers the whole offline-safe
surface: socket creation, a bad address string, ports 0 and 65536,
out-of-image `send`/`recv` spans (refused before the descriptor is
touched), and close on a live socket and on a negative fd.

## Verification

- Full `make test` on the aarch64 Termux source tree: core + asm + heap
  + gc + the 12/12 example programs + the lib/ suites + `gc.sh` + the
  new `net.flt` — all green.
- `net.flt` runs offline on any host: it exercises socket creation and
  the rejection paths above; no traffic ever leaves the machine.
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
| `flint-v0.4.0-beta.2-x86_64-linux.tar.gz` | GitHub Actions (`release.yml`) | Ubuntu runner, `make test` green before packaging |
| `flint-v0.4.0-beta.2-x86_64-linux.tar.gz.sha256` | same | checksum, bare filename (`sha256sum -c` from anywhere) |
| `flint-v0.4.0-beta.2-aarch64-termux.tar.gz` | Termux build (bionic, aarch64) | for Android/Termux |
| `flint-v0.4.0-beta.2-aarch64-termux.tar.gz.sha256` | same | checksum |

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```

or grab an archive above.

## Notes

- IPv4 TCP only, and client-side only for now: there is no
  `bind`/`listen`/`accept` trio yet and no UDP. Those are the natural
  next additions.
- `recv` blocks until at least one byte or EOF; there is no timeout and
  no nonblocking mode in this block. A peer that sends nothing parks the
  guest process until it is interrupted.
- Hostname strings (e.g. `example.com`) are refused by design — pass a
  dotted-quad literal.
- The bytes are raw TCP: no TLS, no framing. Application-level protocols
  are the program's business.
