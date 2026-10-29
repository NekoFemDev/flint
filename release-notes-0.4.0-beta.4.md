# FLINT 0.4.0-beta.4

A diagnostics release for `connect()` and the shared NUL-terminated-string
helper. No ISA, image-format or syscall-number changes.

## Fixed

### `clif_strn()` rejects out-of-range addresses up front

`clif_strn()` (`src/flint_cli.c`) used to check only `addr < 0`, then let
the scan loop terminate on `mem_size`. It now rejects the whole invalid
range explicitly:

```c
if (addr < 0 || addr >= (flint_word)vm->mem_size)
    return -1;
```

A pointer equal to `mem_size`, a negative word, and an oversized word all
fail before a byte is read. The helper is shared by every syscall that
takes a NUL-terminated string: `open` (3), `stat` (7), `mkdir` (8),
`unlink` (9), `getenv` (14), `rename` (16), `rmdir` (17), `opendir` (18)
and `connect` (22).

## Added

### `FLINT_DEBUG_CONNECT=1`

`std_connect()` now traces the guest address before validation and
`inet_pton`:

```text
$ FLINT_DEBUG_CONNECT=1 flint run prog.flt
flint: connect: r1=0 (0x0) mem_size=65568 bytes: 31 32 37 2e 30 2e 30 2e 31 00 ...
flint: connect: string len=9 "127.0.0.1"
```

The trace prints:

- the raw `r1` value in decimal and hex,
- `vm->mem_size`,
- the first 32 bytes at `r1` (or `<outside mem>`),
- the resolved string and length, or `no NUL terminator from r1 within
  mem_size`.

That distinguishes the two failure cases that both make `clif_strn()`
return −1: a pointer outside the image, and a valid pointer whose bytes
never reach a NUL. The switch follows the `FLINT_GC_*` convention — set,
non-empty and not `0` turns it on. It is off by default, so normal runs
and `make test` produce no extra stderr output.

## Notes on the "pointer is garbage" report

The trace was added to settle whether the assembler emits mem-relative or
file-relative addresses for `set r1, label`. On the current tree it emits
mem-relative offsets: a data label at the start of the data section is
`r1=0`, the next is `r1=10`, and a `.fbc` / fused `--elf` run produces the
same values as the source path. Code and mem are loaded into separate
buffers, `heap_start` is the 16-aligned end of the data section, and
`mem_size` covers data + heap + stack. There is no offset bug and no
change was made to the assembler.

The practical cause of a missing terminator is the source data: `str`
emits exactly the bytes written and does **not** append a NUL. Keep the
explicit tail on C-string arguments:

```flint
data:
ip: str "127.0.0.1", 0
```

A buffer filled by `read` / `recv` also needs its own terminator written
after the call. `str` is left alone so binary literals keep their exact
length and label addresses do not shift.

## Verification

- Full `make test` on the aarch64 Termux tree: core, asm, heap, gc, the
  example programs, the `lib/` suites, the fused `.so` host and `gc.sh`
  — all green.
- Source, `.fbc` and fused `--elf` runs of the same program produce
  identical `connect` traces.
- Out-of-image probes (`1000000000`, `mem_size`) report `<outside mem>`
  and `no NUL terminator`; a valid labelled string resolves and connects.
- Trace disabled: the same probes are silent on stderr.

## Installation

```bash
git clone https://github.com/NekoFemDev/flint.git
cd flint
make
make test
```
